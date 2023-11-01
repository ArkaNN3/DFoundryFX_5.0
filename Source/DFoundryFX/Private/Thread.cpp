#include "Thread.h"

#define LOCTEXT_NAMESPACE "DFX_Thread"
DECLARE_CYCLE_STAT(TEXT("DFoundryFX_ThreadProcEvents"), STAT_ThreadProcEvents, STATGROUP_DFoundryFX);
DECLARE_CYCLE_STAT(TEXT("DFoundryFX_ThreadNewFrame"), STAT_ThreadNewFrame, STATGROUP_DFoundryFX);
DECLARE_CYCLE_STAT(TEXT("DFoundryFX_ThreadRender"), STAT_ThreadRender, STATGROUP_DFoundryFX);
DECLARE_CYCLE_STAT(TEXT("DFoundryFX_ThreadDraw"), STAT_ThreadDraw, STATGROUP_DFoundryFX);

FDFX_Thread::FDFX_Thread()
{
  // Events
  hOnGameModeInitialized = FDelegateHandle();
  hOnWorldBeginPlay = FDelegateHandle();
  hOnHUDPostRender = FDelegateHandle();

  // Thread
  UE_LOG(LogDFoundryFX, Log, TEXT("Thread: Initializing DFoundryFX multithread."));
  DFoundryFX_Thread = FRunnableThread::Create(this, TEXT("DFoundryFX_Thread"), 128 * 1024, FPlatformAffinity::GetTaskThreadPriority(),
    FPlatformAffinity::GetNoAffinityMask(), EThreadCreateFlags::None
  );
}

FDFX_Thread::~FDFX_Thread()
{
  UE_LOG(LogDFoundryFX, Log, TEXT("Thread: Destroying DFoundryFX multithread."));

  RemoveDelegates();

  // Thread
  if (DFoundryFX_Thread != nullptr)
  {
    DFoundryFX_Thread->Kill(true);
    delete DFoundryFX_Thread;
  }
}

bool FDFX_Thread::Init()
{
  hOnGameModeInitialized = FGameModeEvents::OnGameModeInitializedEvent().AddRaw(this, &FDFX_Thread::OnGameModeInitialized);
  hOnViewportCreated = UGameViewportClient::OnViewportCreated().AddRaw(this, &FDFX_Thread::OnViewportCreated);

  // Ignore when Engine still loading
  if (GEngine && GEngine->GetWorldContexts().Num() != 0) {
    // Restarting FXThread on a existing viewport.
    int WorldContexts = GEngine->GetWorldContexts().Num();
    for (int i = 0; i < WorldContexts; i++) {
      if (GEngine->GetWorldContexts()[i].World()->IsGameWorld()) {
        OnGameModeInitialized(UGameplayStatics::GetGameMode(GEngine->GetWorldContexts()[i].World()));
        OnWorldBeginPlay();
        break;
      }
    }
  }

  return true;
}

// Events -> GameMode and Viewport
void FDFX_Thread::OnGameModeInitialized(AGameModeBase* aGameMode)
{
  // ImGui
  UE_LOG(LogDFoundryFX, Log, TEXT("Thread: Initializing ImGui resources and context."));
  m_ImGuiContext = ImGui::CreateContext();
  m_ImPlotContext = ImPlot::CreateContext();
  ImGui_ImplUE_CreateDeviceObjects();
  ImGui_ImplUE_Init();

  GameMode = aGameMode;
  uWorld = GameMode->GetWorld();
  GameViewport = uWorld->GetGameViewport();

  // Cleanup if PIE or second window.
  RemoveDelegates();

  hOnWorldBeginPlay = uWorld->OnWorldBeginPlay.AddRaw(this, &FDFX_Thread::OnWorldBeginPlay);
  hOnViewportClose = GameViewport->GetWindow()->GetOnWindowClosedEvent().AddLambda(
  [this](const TSharedRef<SWindow>& Window) {
    FDFX_Thread::OnViewportClose();
  });
  //hOnPipelineStateLogged = FPipelineFileCacheManager::OnPipelineStateLogged().AddRaw(this, &FDFX_Thread::OnPipelineStateLogged);
  ShaderLogTime = FApp::GetCurrentTime();
}


void FDFX_Thread::OnWorldBeginPlay()
{
  ViewportSize = FVector2D::ZeroVector;
  bExternalOpened = false;

  // Fix for GameMode HUD Class = None
  PlayerController = uWorld->GetFirstPlayerController();
  if (!GameMode->HUDClass)
  {
    PlayerController->SpawnDefaultHUD();
  }

  // Fix for new PIE window
  if (hOnHUDPostRender.IsValid()) {
    if (PlayerController->GetHUD()->OnHUDPostRender.IsBound())
    {
      PlayerController->GetHUD()->OnHUDPostRender.Remove(hOnHUDPostRender);
      hOnHUDPostRender.Reset();
    }
  }

  if(!hOnViewportClose.IsValid()) {
    hOnViewportClose = GameViewport->GetWindow()->GetOnWindowClosedEvent().AddLambda(
      [this](const TSharedRef<SWindow>& Window) {
        FDFX_Thread::OnViewportClose();
      });
  }

  if (PlayerController) {
    hOnHUDPostRender = PlayerController->GetHUD()->OnHUDPostRender.AddRaw(this, &FDFX_Thread::OnHUDPostRender);
  }
}

void FDFX_Thread::OnHUDPostRender(AHUD* HUD, UCanvas* Canvas)
{
  if (bStopping) { return; }

  ViewportSize = FVector2D(Canvas->SizeX, Canvas->SizeY);
  uCanvas = Canvas;
  FDFX_Thread::ImGui_ImplUE_Render();
}

void FDFX_Thread::OnViewportCreated()
{
}

bool FDFX_Thread::OnViewportClose()
{
  //ExternalWindow(true);

  RemoveDelegates();

  // ImGui
  if (m_ImGuiContext) {
    ImPlot::DestroyContext(m_ImPlotContext);
    ImGui::DestroyContext(m_ImGuiContext);

    m_ImPlotContext = nullptr;
    m_ImGuiContext = nullptr;
  }

  ViewportSize = FVector2D::ZeroVector;
  m_ImGuiDiffTime = 0;

  // Hook for the next viewport/PIE
  hOnGameModeInitialized = FGameModeEvents::OnGameModeInitializedEvent().AddRaw(this, &FDFX_Thread::OnGameModeInitialized);
  hOnViewportCreated = UGameViewportClient::OnViewportCreated().AddRaw(this, &FDFX_Thread::OnViewportCreated);

  return true;
}

void FDFX_Thread::OnPipelineStateLogged(FPipelineCacheFileFormatPSO& PipelineCacheFileFormatPSO)
{
  FString AssetName = "";
  double m_ShaderLogDiff = FApp::GetCurrentTime() - ShaderLogTime;
  uint32 m_hash = PipelineCacheFileFormatPSO.Hash;
  uint32 m_type = static_cast<int>(PipelineCacheFileFormatPSO.Type);

  switch(m_type) {
    case 0:  //Compute
      FDFX_StatData::AddShaderLog(1, 
        *PipelineCacheFileFormatPSO.ComputeDesc.ComputeShader.ToString(), 
        m_ShaderLogDiff);
      break;
    case 1:  //Graphics
      FDFX_StatData::AddShaderLog(2,
        *PipelineCacheFileFormatPSO.GraphicsDesc.ShadersToString(),
        m_ShaderLogDiff);
      break;
    case 2:  //Raytracing
      FDFX_StatData::AddShaderLog(4,
        *PipelineCacheFileFormatPSO.RayTracingDesc.ShaderHash.ToString(),
        m_ShaderLogDiff);
      break;
  }
  ShaderLogTime = FApp::GetCurrentTime();
}


ImGuiIO& FDFX_Thread::GetImGuiIO() const
{
  checkf(m_ImGuiContext, TEXT("ImGuiContext is invalid!"));

  ImGui::SetCurrentContext(m_ImGuiContext);
  ImPlot::SetCurrentContext(m_ImPlotContext);
  return ImGui::GetIO();
}

bool FDFX_Thread::ImGui_ImplUE_Init()
{
  ImGuiIO& IO = GetImGuiIO();
  IO.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
  IO.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  IO.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  IO.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset;

  ImGui::StyleColorsDark();

  IO.SetClipboardTextFn = ImGui_ImplUE_SetClipboardText;
  IO.GetClipboardTextFn = ImGui_ImplUE_GetClipboardText;

  return true;
}


bool FDFX_Thread::ImGui_ImplUE_CreateDeviceObjects()
{
  // Build texture atlas
  const ImGuiIO& IO = GetImGuiIO();
  unsigned char* Pixels;
  int Width, Height;
  IO.Fonts->GetTexDataAsRGBA32(&Pixels, &Width, &Height);

  if (!FDFX_Module::FontTexture_Updated) {
    AsyncTask(ENamedThreads::GameThread, [=](){
      UE_LOG(LogDFoundryFX, Log, TEXT("ImGui FontTexture : TexData %d x %d."), Width, Height);
  
      FDFX_Module::FontTexture->UnlinkStreaming();
      FTexture2DMipMap& Mip = FDFX_Module::FontTexture->GetPlatformData()->Mips[0];
      void* Data = Mip.BulkData.Lock(LOCK_READ_WRITE);
      const int Size = Width * Height * 4; // Fix lnt-arithmetic-overflow warning
      FMemory::Memcpy(Data, Pixels, Size);
      Mip.BulkData.Unlock();

      FDFX_Module::FontTexture->UpdateResource();
      FDFX_Module::MaterialInstance->SetTextureParameterValue(FName("param"), FDFX_Module::FontTexture);
      //FDFX_Module::MaterialInstance->Shader(EMaterialShaderPrecompileMode::Synchronous);
      //FDFX_Module::MaterialInstance->AllMaterialsCacheResourceShadersForRendering(); DX12 ValidateBoundUniformBuffer Crash
      FDFX_Module::FontTexture_Updated = true;
    });
  }

  // Store our identifier
  IO.Fonts->TexID = static_cast<void*>(FDFX_Module::FontTexture);

  UE_LOG(LogDFoundryFX, Log, TEXT("ImGui FontTexture loaded %d x %d.."), Width, Height);

  return true;
}

void FDFX_Thread::ImGui_ImplUE_Render()
{
  const uint64 M_ImGuiBeginTime = FPlatformTime::Cycles64();
  { 
    SCOPE_CYCLE_COUNTER(STAT_ThreadProcEvents);
    ImGui_ImplUE_ProcessEvent();
  }
  { 
    SCOPE_CYCLE_COUNTER(STAT_ThreadNewFrame);
    ImGui_ImplUE_NewFrame();
  }
  FDFX_StatData::RunDFoundryFX(GameViewport, m_ImGuiDiffTime * 1000);
  { 
    SCOPE_CYCLE_COUNTER(STAT_ThreadRender);
    ImGui::Render();
  }
  { 
    SCOPE_CYCLE_COUNTER(STAT_ThreadDraw);
    ImGui_ImplUE_RenderDrawLists();
  }
  const uint64 M_ImGuiEndTime = FPlatformTime::Cycles64();
  m_ImGuiDiffTime = M_ImGuiEndTime - M_ImGuiBeginTime;
}

void FDFX_Thread::ImGui_ImplUE_ProcessEvent()
{
  if (!ControllerInput())
    return;

  ImGuiIO& IO = GetImGuiIO();

  IO.KeyShift = PlayerController->IsInputKeyDown(EKeys::LeftShift) || PlayerController->IsInputKeyDown(EKeys::RightShift);
  IO.KeyCtrl = PlayerController->IsInputKeyDown(EKeys::LeftControl) || PlayerController->IsInputKeyDown(EKeys::RightControl);
  IO.KeyAlt = PlayerController->IsInputKeyDown(EKeys::LeftAlt) || PlayerController->IsInputKeyDown(EKeys::RightAlt);
  IO.KeySuper = false;

  TArray<FKey> Keys;
  EKeys::GetAllKeys(Keys);
  for (int i = 0; i < Keys.Num(); i++)
  {
    const ImGuiKey m_ImGuiKey = FKeyToImGuiKey(Keys[i].GetFName());
    if (PlayerController->IsInputKeyDown(Keys[i])) {
      if (m_ImGuiKey != ImGuiKey_None) {
        IO.AddKeyEvent(m_ImGuiKey, true);
      }
    } else {
      IO.AddKeyEvent(m_ImGuiKey, false);
    }

    if (Keys[i].IsTouch()) {
      float TouchY, TouchX;
      bool TouchPressed;
      PlayerController->GetInputTouchState(ETouchIndex::Touch1, TouchY, TouchX, TouchPressed);
      IO.AddMousePosEvent(TouchX, TouchY);
      IO.AddMouseButtonEvent(0, TouchPressed);
      continue;
    }

    const bool bKeyboard = !Keys[i].IsMouseButton() &&
      !Keys[i].IsModifierKey() &&
      !Keys[i].IsGamepadKey() &&
      !Keys[i].IsAxis1D() &&
      !Keys[i].IsAxis2D();
    if (!bKeyboard)
    {
      continue;
    }

    if (PlayerController->WasInputKeyJustPressed(Keys[i]))
    {
      const uint32* key_code = NULL;
      const uint32* char_code = NULL;
      FInputKeyManager::Get().GetCodesFromKey(Keys[i], key_code, char_code);

      if (char_code)
      {
        int c = tolower((int)*char_code);
        if (PlayerController->IsInputKeyDown(EKeys::LeftShift) || PlayerController->IsInputKeyDown(EKeys::RightShift))
          c = toupper(c);
        IO.AddInputCharacter((ImWchar)c);
      }
    }
  }

  ControllerInput();
  //ExternalWindow();
}

void FDFX_Thread::ImGui_ImplUE_NewFrame()
{
  ImGuiIO& IO = GetImGuiIO();
  
  IO.DisplaySize = ImVec2(static_cast<float>(ViewportSize.X), static_cast<float>(ViewportSize.Y));
  IO.DisplayFramebufferScale = ImVec2(1, 1);

  if (PlayerController)
  {
    PlayerController->GetMousePosition(IO.MousePos.x, IO.MousePos.y);
    IO.MouseDown[0] = PlayerController->IsInputKeyDown(EKeys::LeftMouseButton);
    IO.MouseDown[1] = PlayerController->IsInputKeyDown(EKeys::RightMouseButton);
    IO.MouseDown[2] = PlayerController->IsInputKeyDown(EKeys::MiddleMouseButton);
  }

  //TODO : Add MouseWheelAxis
  //io.AddMouseWheelEvent(0.f, PlayerController->IsInputKeyDown(EKeys::MouseWheelAxis));

  ImGui::NewFrame();
}

void FDFX_Thread::ImGui_ImplUE_RenderDrawLists()
{
  // Avoid rendering when minimized
  ImGuiIO& IO = GetImGuiIO();
  int Fb_Width = static_cast<int>(IO.DisplaySize.x * IO.DisplayFramebufferScale.x);
  int Fb_Height = static_cast<int>(IO.DisplaySize.y * IO.DisplayFramebufferScale.y);
  if ((Fb_Width == 0) || (Fb_Height == 0))
    return;

  ImDrawData* draw_data = ImGui::GetDrawData();
  draw_data->ScaleClipRects(IO.DisplayFramebufferScale);

  // Render command lists
  for (int n = 0; n < draw_data->CmdListsCount; n++)
  {
    const ImDrawList* Cmd_List = draw_data->CmdLists[n];
    const ImDrawVert* vtx_buffer = Cmd_List->VtxBuffer.Data;
    const ImDrawIdx* Idx_Buffer = Cmd_List->IdxBuffer.Data;

    for (int cmd_i = 0; cmd_i < Cmd_List->CmdBuffer.Size; cmd_i++)
    {
      const ImDrawCmd* pcmd = &Cmd_List->CmdBuffer[cmd_i];
      TArray<FCanvasUVTri> triangles;
      for (unsigned int elem = 0; elem < pcmd->ElemCount / 3; elem++)
      {
        ImDrawVert v[] =
        {
          Cmd_List->VtxBuffer[Idx_Buffer[elem * 3]],
          Cmd_List->VtxBuffer[Idx_Buffer[elem * 3 + 1]],
          Cmd_List->VtxBuffer[Idx_Buffer[elem * 3 + 2]]
        };

        ImVec4 Col[] =
        {
          ImGui::ColorConvertU32ToFloat4(v[0].col),
          ImGui::ColorConvertU32ToFloat4(v[1].col),
          ImGui::ColorConvertU32ToFloat4(v[2].col)
        };

        ImVec2 min_pos = v[0].pos;
        ImVec2 max_pos = v[0].pos;
        for (int i = 0; i < 3; i++)
        {
          if (v[i].pos.x < min_pos.x)
            min_pos.x = v[i].pos.x;
          if (v[i].pos.y < min_pos.y)
            min_pos.y = v[i].pos.y;
          if (v[i].pos.x > max_pos.x)
            max_pos.x = v[i].pos.x;
          if (v[i].pos.y > max_pos.y)
            max_pos.y = v[i].pos.y;
        }

        ImVec2 min_uv = v[0].uv;
        ImVec2 max_uv = v[0].uv;
        for (int i = 0; i < 3; i++)
        {
          if (v[i].uv.x < min_uv.x)
            min_uv.x = v[i].uv.x;
          if (v[i].uv.y < min_uv.y)
            min_uv.y = v[i].uv.y;
          if (v[i].uv.x > max_uv.x)
            max_uv.x = v[i].uv.x;
          if (v[i].uv.y > max_uv.y)
            max_uv.y = v[i].uv.y;
        }

        for (int i = 0; i < 3; i++)
        {
          if (v[i].pos.x < pcmd->ClipRect.x)
          {
            v[i].uv.x += (max_uv.x - v[i].uv.x) * (pcmd->ClipRect.x - v[i].pos.x) / (max_pos.x - v[i].pos.x);
            v[i].pos.x = pcmd->ClipRect.x;
          }
          else if (v[i].pos.x > pcmd->ClipRect.z)
          {
            v[i].uv.x -= (v[i].uv.x - min_uv.x) * (v[i].pos.x - pcmd->ClipRect.z) / (v[i].pos.x - min_pos.x);
            v[i].pos.x = pcmd->ClipRect.z;
          }
          if (v[i].pos.y < pcmd->ClipRect.y)
          {
            v[i].uv.y += (max_uv.y - v[i].uv.y) * (pcmd->ClipRect.y - v[i].pos.y) / (max_pos.y - v[i].pos.y);
            v[i].pos.y = pcmd->ClipRect.y;
          }
          else if (v[i].pos.y > pcmd->ClipRect.w)
          {
            v[i].uv.y -= (v[i].uv.y - min_uv.y) * (v[i].pos.y - pcmd->ClipRect.w) / (v[i].pos.y - min_pos.y);
            v[i].pos.y = pcmd->ClipRect.w;
          }
        }

        FCanvasUVTri triangle;
        triangle.V0_Pos = FVector2D(v[0].pos.x, v[0].pos.y);
        triangle.V1_Pos = FVector2D(v[1].pos.x, v[1].pos.y);
        triangle.V2_Pos = FVector2D(v[2].pos.x, v[2].pos.y);
        triangle.V0_UV = FVector2D(v[0].uv.x, v[0].uv.y);
        triangle.V1_UV = FVector2D(v[1].uv.x, v[1].uv.y);
        triangle.V2_UV = FVector2D(v[2].uv.x, v[2].uv.y);
        triangle.V0_Color = FLinearColor(Col[0].x, Col[0].y, Col[0].z, Col[0].w);
        triangle.V1_Color = FLinearColor(Col[1].x, Col[1].y, Col[1].z, Col[1].w);
        triangle.V2_Color = FLinearColor(Col[2].x, Col[2].y, Col[2].z, Col[2].w);
        triangles.Push(triangle);
      }

      // Draw triangles
      uCanvas->K2_DrawMaterialTriangle(FDFX_Module::MaterialInstance, triangles);
      Idx_Buffer += pcmd->ElemCount;
    }
  }
}

const char* FDFX_Thread::ImGui_ImplUE_GetClipboardText(void* user_data)
{
// Removed for Android builds
/*
  FString text;
  FGenericPlatformApplicationMisc::ClipboardPaste(text);
  FTCHARToUTF8 result(*text);
  return result.Get();
*/
  return nullptr;
}

void FDFX_Thread::ImGui_ImplUE_SetClipboardText(void* user_data, const char* text)
{
/*
  int new_size = strlen(text) + 1;
  TCHAR* new_str = new TCHAR[new_size];
  size_t convertedChars = 0;
  mbstowcs_s(&convertedChars, new_str, new_size, text, _TRUNCATE);
  FGenericPlatformApplicationMisc::ClipboardCopy(new_str);
  delete[] new_str;
*/
}

bool FDFX_Thread::ControllerInput()
{
  static bool bControllerDisabled = false;
  static bool bMainWindowStillOpen = false;
  if (APawn* aPawn = PlayerController->GetPawn(); aPawn)
  {
    

    if (!FDFX_StatData::bMainWindowOpen && !bMainWindowStillOpen)
      return false;

    if (!FDFX_StatData::bMainWindowOpen && bControllerDisabled) {
      aPawn->EnableInput(PlayerController);
      bControllerDisabled = false;
      bMainWindowStillOpen = false;
      return true;
    }

    if (FDFX_StatData::bMainWindowOpen && !FDFX_StatData::bDisableGameControls) {
      if (aPawn) {
        aPawn->EnableInput(PlayerController);
      }
      bControllerDisabled = false;
      return true;
    }

    if (FDFX_StatData::bDisableGameControls) {
      if (FDFX_StatData::bMainWindowOpen) {
        aPawn->DisableInput(PlayerController);
        bMainWindowStillOpen = true;
        bControllerDisabled = true;
      }
    }
    return true;
  }
  return false;
}

void FDFX_Thread::ExternalWindow(bool IsExiting)
{
// TODO: Open external window and move charts.
// FAILED: To create an UCanvas on external window

  if (!FDFX_StatData::bExternalWindow && !bExternalOpened)
    return;

  bool extWinValid = m_extWindow.IsValid();

  if (IsExiting && extWinValid) {
    m_extWindow->RequestDestroyWindow();
    return;
  }

  if (FDFX_StatData::bExternalWindow && !bExternalOpened && !extWinValid) {
    FVector2D winPos = FVector2D(GameViewport->GetWindow()->GetPositionInScreen().X + ViewportSize.X, GameViewport->GetWindow()->GetPositionInScreen().Y);
    m_extWindow = SNew(SWindow)
      .Title(FText::FromString("DFoundryFX"))
      .Type(EWindowType::GameWindow)
      .ClientSize(FVector2D(ViewportSize.X / 4, ViewportSize.Y))
      .ScreenPosition(winPos)
      .FocusWhenFirstShown(true)
      .SupportsMaximize(true)
      .SupportsMinimize(true)
      .UseOSWindowBorder(false)
      .SizingRule(ESizingRule::UserSized);
    m_extWindow->SetAllowFastUpdate(true);
    m_extWindow->GetOnWindowClosedEvent().AddLambda(
      [this](const TSharedRef<SWindow>& Window) {
        m_extWindow = nullptr;
      }
    );
    m_extWindow = FSlateApplication::Get().AddWindow(m_extWindow.ToSharedRef(), true);
    m_extWindow->ShowWindow();
    m_extWindow->MoveWindowTo(winPos);
    FSlateApplication::Get().Tick();
    
    bExternalOpened = true;
    return;
  }

  if (!FDFX_StatData::bExternalWindow && bExternalOpened && extWinValid) {
    m_extWindow->RequestDestroyWindow();
    bExternalOpened = false;
    return;
  }

  if (FDFX_StatData::bExternalWindow && bExternalOpened && !extWinValid) {
    FDFX_StatData::bExternalWindow = false;
    bExternalOpened = false;
  }
}


void FDFX_Thread::Stop()
{
  SetPaused(true);
  bStopping = true;

  RemoveDelegates();

  DFoundryFX_Thread->WaitForCompletion();

  if (m_ImGuiContext) {
    //ImPlot::DestroyContext(m_ImPlotContext);
    //ImGui::DestroyContext(m_ImGuiContext);

    m_ImPlotContext = nullptr;
    m_ImGuiContext = nullptr;
  }
}

void FDFX_Thread::RemoveDelegates() {
  // Release all active delegates
  if (hOnViewportCreated.IsValid()) { 
    GameViewport->OnViewportCreated().Remove(hOnViewportCreated);
    hOnViewportCreated.Reset();
  }
  if (hOnViewportClose.IsValid()) { 
    GameViewport->GetWindow()->GetOnWindowClosedEvent().Remove(hOnViewportClose);
    hOnViewportClose.Reset();
  }
  if (hOnHUDPostRender.IsValid()) { 
    PlayerController->GetHUD()->OnHUDPostRender.Remove(hOnHUDPostRender);
    hOnHUDPostRender.Reset();
  }
  if (hOnGameModeInitialized.IsValid()) {
    FGameModeEvents::OnGameModeInitializedEvent().Remove(hOnGameModeInitialized);
    hOnGameModeInitialized.Reset();
  }
  if (hOnPipelineStateLogged.IsValid()) {
    //FPipelineFileCacheManager::OnPipelineStateLogged().Remove(hOnPipelineStateLogged);
    hOnPipelineStateLogged.Reset();
  }
  if (hOnWorldBeginPlay.IsValid()) {
    uWorld->OnWorldBeginPlay.Remove(hOnWorldBeginPlay);
    hOnWorldBeginPlay.Reset();
  }
}

// *******************
// Whatever
// *******************
void FDFX_Thread::Wait(float Seconds)
{
  FPlatformProcess::Sleep(Seconds);
}

void FDFX_Thread::Tick()
{
}

uint32 FDFX_Thread::Run()
{
  return 0;
}

void FDFX_Thread::Exit()
{
}

void FDFX_Thread::SetPaused(bool MakePaused)
{
  bPaused.AtomicSet(MakePaused);
  if (!MakePaused)
  {
    bIsVerifiedSuspended.AtomicSet(false);
  }
}

bool FDFX_Thread::IsThreadPaused()
{
  return bPaused;
}

bool FDFX_Thread::IsThreadVerifiedSuspended()
{
  return bIsVerifiedSuspended;
}

bool FDFX_Thread::HasThreadStopped()
{
  return bHasStopped;
}

ImGuiKey FDFX_Thread::FKeyToImGuiKey(FName Keyname)
{
#define LITERAL_TRANSLATION(Key) { EKeys::Key.GetFName(), ImGuiKey_##Key }
  // not an exhaustive mapping, some keys are missing :^|
  static const TMap<FName, ImGuiKey> FKeyToImGuiKey =
  {
    LITERAL_TRANSLATION(A), LITERAL_TRANSLATION(B), LITERAL_TRANSLATION(C), LITERAL_TRANSLATION(D), LITERAL_TRANSLATION(E), LITERAL_TRANSLATION(F),
    LITERAL_TRANSLATION(G), LITERAL_TRANSLATION(H), LITERAL_TRANSLATION(I), LITERAL_TRANSLATION(J), LITERAL_TRANSLATION(K), LITERAL_TRANSLATION(L),
    LITERAL_TRANSLATION(M), LITERAL_TRANSLATION(N), LITERAL_TRANSLATION(O), LITERAL_TRANSLATION(P), LITERAL_TRANSLATION(Q), LITERAL_TRANSLATION(R),
    LITERAL_TRANSLATION(S), LITERAL_TRANSLATION(T), LITERAL_TRANSLATION(U), LITERAL_TRANSLATION(V), LITERAL_TRANSLATION(W), LITERAL_TRANSLATION(X),
    LITERAL_TRANSLATION(Y), LITERAL_TRANSLATION(Z),
    LITERAL_TRANSLATION(F1), LITERAL_TRANSLATION(F2), LITERAL_TRANSLATION(F3), LITERAL_TRANSLATION(F4),
    LITERAL_TRANSLATION(F5), LITERAL_TRANSLATION(F6), LITERAL_TRANSLATION(F7), LITERAL_TRANSLATION(F8),
    LITERAL_TRANSLATION(F9), LITERAL_TRANSLATION(F10), LITERAL_TRANSLATION(F11), LITERAL_TRANSLATION(F12),
    LITERAL_TRANSLATION(Enter), LITERAL_TRANSLATION(Insert), LITERAL_TRANSLATION(Delete), LITERAL_TRANSLATION(Escape), LITERAL_TRANSLATION(Tab),
    LITERAL_TRANSLATION(PageUp), LITERAL_TRANSLATION(PageDown), LITERAL_TRANSLATION(Home), LITERAL_TRANSLATION(End),
    LITERAL_TRANSLATION(NumLock), LITERAL_TRANSLATION(ScrollLock), LITERAL_TRANSLATION(CapsLock),
    LITERAL_TRANSLATION(RightBracket), LITERAL_TRANSLATION(LeftBracket), LITERAL_TRANSLATION(Backslash), LITERAL_TRANSLATION(Slash),
    LITERAL_TRANSLATION(Semicolon), LITERAL_TRANSLATION(Period), LITERAL_TRANSLATION(Comma), LITERAL_TRANSLATION(Apostrophe), LITERAL_TRANSLATION(Pause),
    { EKeys::Zero.GetFName(), ImGuiKey_0 }, { EKeys::One.GetFName(), ImGuiKey_1 }, { EKeys::Two.GetFName(), ImGuiKey_2 },
    { EKeys::Three.GetFName(), ImGuiKey_3 }, { EKeys::Four.GetFName(), ImGuiKey_4 }, { EKeys::Five.GetFName(), ImGuiKey_5 },
    { EKeys::Six.GetFName(), ImGuiKey_6 }, { EKeys::Seven.GetFName(), ImGuiKey_7 }, { EKeys::Eight.GetFName(), ImGuiKey_8 }, { EKeys::Nine.GetFName(), ImGuiKey_9 },
    { EKeys::NumPadZero.GetFName(), ImGuiKey_Keypad0 }, { EKeys::NumPadOne.GetFName(), ImGuiKey_Keypad1 }, { EKeys::NumPadTwo.GetFName(), ImGuiKey_Keypad2 },
    { EKeys::NumPadThree.GetFName(), ImGuiKey_Keypad3 }, { EKeys::NumPadFour.GetFName(), ImGuiKey_Keypad4 }, { EKeys::NumPadFive.GetFName(), ImGuiKey_Keypad5 },
    { EKeys::NumPadSix.GetFName(), ImGuiKey_Keypad6 }, { EKeys::NumPadSeven.GetFName(), ImGuiKey_Keypad7 }, { EKeys::NumPadEight.GetFName(), ImGuiKey_Keypad8 },
    { EKeys::NumPadNine.GetFName(), ImGuiKey_Keypad9 },
    { EKeys::LeftShift.GetFName(), ImGuiKey_LeftShift }, { EKeys::LeftControl.GetFName(), ImGuiKey_LeftCtrl }, { EKeys::LeftAlt.GetFName(), ImGuiKey_LeftAlt },
    { EKeys::RightShift.GetFName(), ImGuiKey_RightShift }, { EKeys::RightControl.GetFName(), ImGuiKey_RightCtrl }, { EKeys::RightAlt.GetFName(), ImGuiKey_RightAlt },
    { EKeys::SpaceBar.GetFName(), ImGuiKey_Space }, { EKeys::BackSpace.GetFName(), ImGuiKey_Backspace },
    { EKeys::Up.GetFName(), ImGuiKey_UpArrow }, { EKeys::Down.GetFName(), ImGuiKey_DownArrow },
    { EKeys::Left.GetFName(), ImGuiKey_LeftArrow }, { EKeys::Right.GetFName(), ImGuiKey_RightArrow },
    { EKeys::Subtract.GetFName(), ImGuiKey_KeypadSubtract }, { EKeys::Add.GetFName(), ImGuiKey_KeypadAdd },
    { EKeys::Multiply.GetFName(), ImGuiKey_KeypadMultiply }, { EKeys::Divide.GetFName(), ImGuiKey_KeypadDivide },
    { EKeys::Decimal.GetFName(), ImGuiKey_KeypadDecimal }, { EKeys::Equals.GetFName(), ImGuiKey_Equal },
  };

  const ImGuiKey* Key = FKeyToImGuiKey.Find(Keyname);
  return Key ? *Key : ImGuiKey_None;
}
#undef LOCTEXT_NAMESPACE