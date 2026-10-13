#include "UI/CloudUpdateUI.h"
#include "CloudUpdateSubsystem.h"
#include "CloudUpdate.h"
#include "Engine/World.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/ProgressBar.h"

#define LOCTEXT_NAMESPACE "CloudUpdateScreen"
namespace
{
void PutText(UUserWidget* Screen, const TCHAR* Name, const FText& Text)
{
 if (UTextBlock* Widget = Cast<UTextBlock>(Screen->GetWidgetFromName(FName(Name)))) Widget->SetText(Text);
}
void PutProgress(UUserWidget* Screen, const TCHAR* Name, float Value, bool bUnknown = false)
{
 if (UProgressBar* Bar = Cast<UProgressBar>(Screen->GetWidgetFromName(FName(Name))))
 { Bar->SetPercent(FMath::Clamp(Value, 0.f, 1.f)); Bar->SetIsMarquee(bUnknown); }
}
FText VersionText(const FString& Version) { return Version.IsEmpty() ? LOCTEXT("UnknownVersion", "未查询") : FText::FromString(Version); }
FText BytesText(int64 Bytes)
{
 const TCHAR* Units[] = {TEXT("B"), TEXT("KB"), TEXT("MB"), TEXT("GB")};
 double Value = FMath::Max<int64>(0, Bytes); int32 Unit = 0;
 while (Value >= 1024. && Unit < 3) { Value /= 1024.; ++Unit; }
 FNumberFormattingOptions Options; Options.MinimumFractionalDigits = 0; Options.MaximumFractionalDigits = Unit ? 1 : 0;
 return FText::Format(LOCTEXT("Bytes", "{0} {1}"), FText::AsNumber(Value, &Options), FText::FromString(Units[Unit]));
}
}

void UCloudUpdateScreen::NativeConstruct()
{
 Super::NativeConstruct();
 UnbindSubsystem();
 BoundSubsystem = GetUpdateSubsystem();
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("CheckButton")))) Button->OnClicked.AddUniqueDynamic(this, &UCloudUpdateScreen::CheckClicked);
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("UpdateButton")))) Button->OnClicked.AddUniqueDynamic(this, &UCloudUpdateScreen::UpdateClicked);
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("CancelButton")))) Button->OnClicked.AddUniqueDynamic(this, &UCloudUpdateScreen::CancelClicked);
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("CloseButton")))) Button->OnClicked.AddUniqueDynamic(this, &UCloudUpdateScreen::CloseClicked);
 bChecking = bApplying = bCancelRequested = false;
 if (BoundSubsystem)
 {
  BoundSubsystem->OnUpdatePlanReady.AddUniqueDynamic(this, &UCloudUpdateScreen::PlanReady);
  BoundSubsystem->OnDetailedDownloadProgress.AddUniqueDynamic(this, &UCloudUpdateScreen::DownloadChanged);
  BoundSubsystem->OnMergeProgress.AddUniqueDynamic(this, &UCloudUpdateScreen::MergeChanged);
  BoundSubsystem->OnUpdateFinished.AddUniqueDynamic(this, &UCloudUpdateScreen::UpdateFinished);
  const auto Plan = BoundSubsystem->GetUpdatePlan();
  if (Plan.bValid) PlanReady(true, Plan, FString());
  else SetStatus(BoundSubsystem->GetPendingResourceVersion().IsEmpty() ? LOCTEXT("Ready", "点击检查更新，获取当前平台的版本信息。") : LOCTEXT("Pending", "资源已准备好，请关闭并重新打开应用使其生效。"));
  const auto Progress = BoundSubsystem->GetDownloadProgress();
  if (Progress.bActive) { bApplying = true; DownloadChanged(Progress); MergeChanged(BoundSubsystem->GetMergeProgress()); }
 }
 else SetStatus(LOCTEXT("Unavailable", "更新服务不可用，请重新打开此页面。"), true);
 RefreshVersions(); RefreshButtons();
}

void UCloudUpdateScreen::UnbindSubsystem()
{
 if (!BoundSubsystem) return;
 BoundSubsystem->OnUpdatePlanReady.RemoveDynamic(this, &UCloudUpdateScreen::PlanReady);
 BoundSubsystem->OnDetailedDownloadProgress.RemoveDynamic(this, &UCloudUpdateScreen::DownloadChanged);
 BoundSubsystem->OnMergeProgress.RemoveDynamic(this, &UCloudUpdateScreen::MergeChanged);
 BoundSubsystem->OnUpdateFinished.RemoveDynamic(this, &UCloudUpdateScreen::UpdateFinished);
 BoundSubsystem = nullptr;
}
void UCloudUpdateScreen::NativeDestruct()
{
 UnbindSubsystem();
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("CheckButton")))) Button->OnClicked.RemoveDynamic(this, &UCloudUpdateScreen::CheckClicked);
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("UpdateButton")))) Button->OnClicked.RemoveDynamic(this, &UCloudUpdateScreen::UpdateClicked);
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("CancelButton")))) Button->OnClicked.RemoveDynamic(this, &UCloudUpdateScreen::CancelClicked);
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("CloseButton")))) Button->OnClicked.RemoveDynamic(this, &UCloudUpdateScreen::CloseClicked);
 Super::NativeDestruct();
}
void UCloudUpdateScreen::NativeTick(const FGeometry& Geometry, float DeltaTime)
{
 Super::NativeTick(Geometry, DeltaTime);
 RefreshElapsed += DeltaTime;
 if (RefreshElapsed >= .2f) { RefreshElapsed = 0.f; RefreshButtons(); }
}
void UCloudUpdateScreen::SetStatus(const FText& Text, bool bError)
{
 PutText(this, TEXT("StatusText"), Text);
 if (auto* Widget = Cast<UTextBlock>(GetWidgetFromName(TEXT("StatusText"))))
  Widget->SetColorAndOpacity(FSlateColor(bError ? FLinearColor(.95f,.35f,.35f) : FLinearColor(.65f,.85f,1.f)));
}
void UCloudUpdateScreen::RefreshVersions()
{
 if (!BoundSubsystem) return;
 PutText(this, TEXT("PlatformText"), FText::Format(LOCTEXT("Platform", "更新通道  ·  {0}"), FText::FromString(BoundSubsystem->GetUpdatePlatform())));
 PutText(this, TEXT("VersionsText"), FText::Format(LOCTEXT("Versions", "游戏版本   本地 {0}    /    服务器 {1}\n资源版本   本地 {2}    /    服务器 {3}"),
  VersionText(BoundSubsystem->GetLocalGameVersion()), VersionText(BoundSubsystem->GetServerGameVersion()),
  VersionText(BoundSubsystem->GetLocalResourceVersion()), VersionText(BoundSubsystem->GetServerResourceVersion())));
}
void UCloudUpdateScreen::RefreshButtons()
{
 const bool Busy = BoundSubsystem && BoundSubsystem->IsBusy();
 const FCloudUpdatePlan Plan = BoundSubsystem ? BoundSubsystem->GetUpdatePlan() : FCloudUpdatePlan();
 const bool Pending = BoundSubsystem && !BoundSubsystem->GetPendingResourceVersion().IsEmpty();
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("CheckButton")))) Button->SetIsEnabled(BoundSubsystem && !Busy);
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("UpdateButton"))))
  Button->SetIsEnabled(BoundSubsystem && !Busy && !Pending && Plan.bValid && !Plan.bGameUpdateRequired && !Plan.bResourceUpdateRequiresGame && Plan.PendingResourceVersionCount > 0);
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("CancelButton"))))
  Button->SetIsEnabled(Busy && (bChecking || bApplying) && !bCancelRequested);
 if (auto* Button = Cast<UButton>(GetWidgetFromName(TEXT("CloseButton")))) Button->SetIsEnabled(!Busy);
 PutText(this, TEXT("CheckLabel"), bChecking ? LOCTEXT("CheckingLabel", "检查中…") : LOCTEXT("CheckLabel", "检查更新"));
 PutText(this, TEXT("UpdateLabel"), bApplying ? LOCTEXT("ApplyingLabel", "更新中…") : LOCTEXT("UpdateLabel", "更新资源"));
}
void UCloudUpdateScreen::CheckClicked()
{
 if (!BoundSubsystem || BoundSubsystem->IsBusy()) return;
 bChecking = true; bCancelRequested = false;
 SetStatus(LOCTEXT("Checking", "正在检查当前平台的更新…"));
 PutText(this, TEXT("SummaryText"), LOCTEXT("Fetching", "正在获取版本与文件清单…"));
 PutText(this, TEXT("DownloadText"), LOCTEXT("NotDownloading", "等待下载"));
 PutText(this, TEXT("FileProgressText"), LOCTEXT("NotStartedFile", "尚未开始"));
 PutText(this, TEXT("MergeText"), LOCTEXT("NotMerging", "等待合并"));
 PutProgress(this,TEXT("DownloadProgress"),0); PutProgress(this,TEXT("FileProgress"),0); PutProgress(this,TEXT("MergeProgress"),0);
 BoundSubsystem->QueryUpdatePlan(); RefreshButtons();
}
void UCloudUpdateScreen::PlanReady(bool bSuccess, const FCloudUpdatePlan& Plan, const FString& Message)
{
 bChecking = false;
 RefreshVersions();
 if (!bSuccess || !Plan.bValid)
 {
  PutText(this,TEXT("SummaryText"),LOCTEXT("FailedPlan", "未获取到有效的更新计划。"));
  SetStatus(bCancelRequested ? LOCTEXT("CancelledCheck", "已取消检查。") : FText::Format(LOCTEXT("CheckFailed", "检查失败，可重试。{0}"),FText::FromString(Message)), !bCancelRequested);
 }
 else
 {
  PutText(this,TEXT("SummaryText"), FText::Format(Plan.bTotalBytesKnown ? LOCTEXT("Summary", "资源更新  {0} 个版本 · {1} 个文件 · {2}") : LOCTEXT("PartialSummary", "资源更新  {0} 个版本 · {1} 个文件 · 已知大小 {2}，总大小待确认"),
   FText::AsNumber(Plan.PendingResourceVersionCount), FText::AsNumber(Plan.RequiredFileCount), BytesText(Plan.RequiredBytes)));
  if (!Plan.PendingResourceVersion.IsEmpty()) SetStatus(FText::Format(LOCTEXT("PendingVersion", "资源 {0} 待生效，请关闭并重新打开应用。"),FText::FromString(Plan.PendingResourceVersion)));
  else if (Plan.bGameUpdateRequired || Plan.bResourceUpdateRequiresGame) SetStatus(LOCTEXT("GameRequired", "需要更新应用安装包；安装完成后再更新资源。"));
  else if (Plan.PendingResourceVersionCount > 0) SetStatus(LOCTEXT("ResourceAvailable", "发现资源更新，可以开始下载。"));
  else SetStatus(LOCTEXT("Latest", "当前平台已是最新版本。"));
 }
 RefreshButtons();
}
void UCloudUpdateScreen::UpdateClicked()
{
 if (!BoundSubsystem || BoundSubsystem->IsBusy()) return;
 const auto Plan = BoundSubsystem->GetUpdatePlan();
 if (!Plan.bValid || Plan.bGameUpdateRequired || Plan.bResourceUpdateRequiresGame || !BoundSubsystem->GetPendingResourceVersion().IsEmpty()) return;
 bApplying = true; bCancelRequested = false;
 SetStatus(LOCTEXT("Downloading", "正在下载资源，请保持网络连接…"));
 BoundSubsystem->ApplyLatestUpdate(); RefreshButtons();
}
void UCloudUpdateScreen::CancelClicked()
{
 if (!BoundSubsystem || !BoundSubsystem->IsBusy() || !(bChecking || bApplying)) return;
 bCancelRequested = true; BoundSubsystem->AbortCurrentTask();
 SetStatus(LOCTEXT("Cancelling", "正在取消，请等待当前文件处理结束…")); RefreshButtons();
}
void UCloudUpdateScreen::CloseClicked() { if (!BoundSubsystem || !BoundSubsystem->IsBusy()) RemoveFromParent(); }
void UCloudUpdateScreen::DownloadChanged(const FCloudDownloadProgressInfo& Progress)
{
 if (Progress.bActive) bApplying = true;
 PutProgress(this,TEXT("DownloadProgress"), Progress.bTotalBytesKnown ? Progress.ByteProgress : Progress.FileProgress, Progress.bActive && !Progress.bTotalBytesKnown);
 PutProgress(this,TEXT("FileProgress"),Progress.CurrentFileProgress,Progress.bActive && Progress.CurrentFileTotalBytes <= 0);
 PutText(this,TEXT("DownloadText"), FText::Format(Progress.bTotalBytesKnown ? LOCTEXT("DownloadKnown", "下载  {0} / {1} · 文件 {2}/{3}") : LOCTEXT("DownloadUnknown", "已下载 {0} · 总大小待确认 · 文件 {2}/{3}"),
  BytesText(Progress.DownloadedBytes),BytesText(Progress.TotalBytes),FText::AsNumber(Progress.CompletedFiles),FText::AsNumber(Progress.TotalFiles)));
 PutText(this,TEXT("FileProgressText"),FText::Format(LOCTEXT("CurrentFile", "{0}\n{1} / {2}"),FText::FromString(Progress.CurrentFile),BytesText(Progress.CurrentFileBytes),
  Progress.CurrentFileTotalBytes > 0 ? BytesText(Progress.CurrentFileTotalBytes) : LOCTEXT("FileSizeUnknown", "大小待确认")));
 RefreshButtons();
}
void UCloudUpdateScreen::MergeChanged(const FCloudMergeProgressInfo& Progress)
{
 PutProgress(this,TEXT("MergeProgress"),Progress.TotalFiles > 0 ? float(Progress.CompletedFiles) / Progress.TotalFiles : 0.f,Progress.bCurrentFileInProgress);
 PutText(this,TEXT("MergeText"),FText::Format(Progress.bCurrentFileInProgress ? LOCTEXT("Merging", "正在合并 {0} · 已完成 {1}/{2}") : LOCTEXT("MergeCount", "合并已完成 {1}/{2} · 回退 {3} · 失败 {4}"),
  FText::FromString(Progress.CurrentFile),FText::AsNumber(Progress.CompletedFiles),FText::AsNumber(Progress.TotalFiles),FText::AsNumber(Progress.SkippedFiles),FText::AsNumber(Progress.FailedFiles)));
}
void UCloudUpdateScreen::UpdateFinished(bool bSuccess, bool bRestartRequired, const FString& VersionId, const FString& Message)
{
 bApplying = false;
 RefreshVersions();
 if (!bSuccess) SetStatus(bCancelRequested ? LOCTEXT("UpdateCancelled", "更新已取消，可重新检查后继续。") : FText::Format(LOCTEXT("UpdateFailed", "更新失败，可重试。{0}"),FText::FromString(Message)), !bCancelRequested);
 else if (bRestartRequired) SetStatus(FText::Format(LOCTEXT("UpdatedPending", "资源 {0} 已准备好，请关闭并重新打开应用使其生效。"),FText::FromString(VersionId)));
 else if (BoundSubsystem && BoundSubsystem->IsBusy()) { bApplying = true; SetStatus(LOCTEXT("NextVersion", "继续处理后续资源版本…")); }
 else SetStatus(FText::Format(LOCTEXT("Updated", "资源已更新到 {0}。"),FText::FromString(VersionId)));
 RefreshButtons();
}
#undef LOCTEXT_NAMESPACE

UCloudUpdateSubsystem* UCloudUpdateScreen::GetUpdateSubsystem() const
{
 return UCloudUpdateSubsystem::GetCloudUpdateSubsystem();
}

ACloudUpdateUIGameMode::ACloudUpdateUIGameMode()
{
 PlayerControllerClass = ACloudUpdateUIPlayerController::StaticClass();
 DefaultPawnClass = nullptr;
 HUDClass = nullptr;
 bStartPlayersAsSpectators = true;
}

void ACloudUpdateUIPlayerController::BeginPlay()
{
 Super::BeginPlay();
 if (!IsLocalController()) return;
 const ACloudUpdateUIGameMode* Mode = GetWorld()->GetAuthGameMode<ACloudUpdateUIGameMode>();
 if (!Mode || !Mode->UpdateWidgetClass)
 {
  UE_LOG(LogCloudUpdate, Warning, TEXT("Update map: assign UpdateWidgetClass on the CloudUpdate UI GameMode."));
  return;
 }
 UpdateScreen = CreateWidget<UCloudUpdateScreen>(this, Mode->UpdateWidgetClass);
 if (UpdateScreen)
 {
  UpdateScreen->AddToViewport();
  bShowMouseCursor = true;
  FInputModeUIOnly InputMode;
  InputMode.SetWidgetToFocus(UpdateScreen->TakeWidget());
  SetInputMode(InputMode);
 }
}

void ACloudUpdateUIPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
 if (UpdateScreen) { UpdateScreen->RemoveFromParent(); UpdateScreen = nullptr; }
 SetInputMode(FInputModeGameOnly());
 bShowMouseCursor = false;
 Super::EndPlay(EndPlayReason);
}
