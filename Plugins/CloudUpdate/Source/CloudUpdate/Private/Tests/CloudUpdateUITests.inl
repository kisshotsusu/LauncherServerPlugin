#if WITH_DEV_AUTOMATION_TESTS
#include "UI/CloudUpdateUI.h"
#include "CloudUpdateSubsystem.h"
#include "Components/TextBlock.h"
#include "Components/Button.h"
#include "Components/ProgressBar.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/BufferArchive.h"
#include "Slate/WidgetRenderer.h"
#include "Engine/TextureRenderTarget2D.h"
#include "ImageUtils.h"
#include <initializer_list>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudUpdateScreenTest, "CloudUpdate.UI.ScreenStates",
 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FCloudUpdateScreenTest::RunTest(const FString& Parameters)
{
 // 实际继承 CloudUpdateScreen 的面板是 WBP_GameStart（WBP_UpDataGame / WBP_CheckEnd 只是普通 UserWidget）。
 UClass* WidgetClass = LoadClass<UCloudUpdateScreen>(nullptr, TEXT("/CloudUpdate/UI/WBP_GameStart.WBP_GameStart_C"));
 if (!TestNotNull(TEXT("Shared plugin widget class loads"), WidgetClass)) return false;
 TStrongObjectPtr<UCloudUpdateScreen> Screen(NewObject<UCloudUpdateScreen>(GetTransientPackage(), WidgetClass));
 if (!TestTrue(TEXT("Widget tree initializes"), Screen->Initialize())) return false;
 const auto SlateWidget = Screen->TakeWidget();
 auto* Subsystem = Screen->GetUpdateSubsystem();
 // 面板资产换过控件名（CheckButton→RetryButton、CloseButton→ExitButton）：按候选名兼容新旧布局。
 const auto FindFirst = [Screen](std::initializer_list<const TCHAR*> Names) -> UWidget*
 {
  for (const TCHAR* Name : Names) if (UWidget* Widget = Screen->GetWidgetFromName(FName(Name))) return Widget;
  return nullptr;
 };
 auto* Status = Cast<UTextBlock>(FindFirst({TEXT("StatusText")}));
 auto* Summary = Cast<UTextBlock>(FindFirst({TEXT("SummaryText")}));
 auto* Check = Cast<UButton>(FindFirst({TEXT("CheckButton"), TEXT("RetryButton")}));
 auto* Update = Cast<UButton>(FindFirst({TEXT("UpdateButton")}));
 auto* Cancel = Cast<UButton>(FindFirst({TEXT("CancelButton")}));
 auto* Bar = Cast<UProgressBar>(FindFirst({TEXT("DownloadProgress")}));
 // 面板资产允许自定义布局：只强制要求状态文本与下载进度条；Summary/Check/Update/Cancel 按存在与否分别断言。
 // 当前 WBP_GameStart 用 RetryButton/ExitButton，且没有 SummaryText/UpdateButton/CancelButton
 // （更新按钮由蓝图图表直接调用 QueryUpdatePlan/ApplyLatestUpdate）。
 if (!TestNotNull(TEXT("Subsystem exists"),Subsystem) || !TestNotNull(TEXT("Status exists"),Status) || !TestNotNull(TEXT("Download progress exists"),Bar)) return false;
 const bool bClassicCheckButton = Check != nullptr && Check->GetFName() == FName(TEXT("CheckButton"));
 if (Check) TestTrue(TEXT("Check click bound by native parent"),Check->OnClicked.IsBound());
 if (Update) { TestTrue(TEXT("Update click bound by native parent"),Update->OnClicked.IsBound()); TestFalse(TEXT("Unqueried update is disabled"),Update->GetIsEnabled()); }
 if (Cancel) TestTrue(TEXT("Cancel click bound by native parent"),Cancel->OnClicked.IsBound());
 FString Screenshot;
 if (FParse::Value(FCommandLine::Get(),TEXT("CloudUpdateUIScreenshot="),Screenshot))
 {
  FWidgetRenderer Renderer(false,true);
  const auto Capture = [this,&Renderer,&SlateWidget](const FString& Path, FVector2D Size)
  {
   UTextureRenderTarget2D* Target = Renderer.DrawWidget(SlateWidget,Size);
   FBufferArchive PNG;
   if (TestNotNull(TEXT("UI render target"),Target) && TestTrue(TEXT("UI screenshot renders"),FImageUtils::ExportRenderTarget2DAsPNG(Target,PNG)))
   {
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true);
    TestTrue(TEXT("UI screenshot saved"),FFileHelper::SaveArrayToFile(PNG,*Path));
   }
  };
  Capture(Screenshot,FVector2D(1280,720));
  FString MobileScreenshot;
  if (FParse::Value(FCommandLine::Get(),TEXT("CloudUpdateUIMobileScreenshot="),MobileScreenshot)) Capture(MobileScreenshot,FVector2D(720,1280));
 }
 FCloudUpdatePlan Plan; Plan.bValid = true; Plan.bTotalBytesKnown = false; Plan.RequiredBytes = 4096; Plan.RequiredFileCount = 2; Plan.PendingResourceVersionCount = 1;
 Subsystem->OnUpdatePlanReady.Broadcast(true,Plan,TEXT(""));
 if (Summary) TestTrue(TEXT("Unknown total shown explicitly"),Summary->GetText().ToString().Contains(TEXT("总大小待确认")));
 Plan.bGameUpdateRequired = true; Subsystem->OnUpdatePlanReady.Broadcast(true,Plan,TEXT(""));
 TestTrue(TEXT("Application package update distinguished from resource update"),Status->GetText().ToString().Contains(TEXT("应用安装包")));
 Subsystem->OnUpdatePlanReady.Broadcast(false,FCloudUpdatePlan(),TEXT("network test failure"));
 TestTrue(TEXT("Failure provides retry and cause"),Status->GetText().ToString().Contains(TEXT("network test failure")));
 FCloudDownloadProgressInfo Download; Download.bActive=true; Download.bTotalBytesKnown=true; Download.ByteProgress=.5f;
 Download.DownloadedBytes=512; Download.TotalBytes=1024; Download.TotalFiles=2; Download.CurrentFile=TEXT("test.pak");
 Subsystem->OnDetailedDownloadProgress.Broadcast(Download);
 TestTrue(TEXT("Byte progress controls main bar"),FMath::IsNearlyEqual(Bar->GetPercent(),.5f));
 Subsystem->OnUpdateFinished.Broadcast(true,true,TEXT("test-resource"),TEXT(""));
 TestTrue(TEXT("Restart instruction names pending resource"),Status->GetText().ToString().Contains(TEXT("test-resource")) && Status->GetText().ToString().Contains(TEXT("重新打开")));
 Screen->NativeDestruct();
 // 新面板（RetryButton）可能同时被蓝图图表绑定，原生解绑只移除自己的委托，故仅对经典命名做严格断言。
 if (bClassicCheckButton) TestFalse(TEXT("Destruction removes check binding"),Check->OnClicked.IsBound());
 const FString Before = Status->GetText().ToString();
 Subsystem->OnUpdatePlanReady.Broadcast(false,FCloudUpdatePlan(),TEXT("must not reach removed screen"));
 TestEqual(TEXT("Removed screen no longer receives subsystem delegates"),Status->GetText().ToString(),Before);
 return true;
}
#endif
