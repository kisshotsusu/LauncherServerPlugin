#include "CloudUpdateSubsystem.h"
#include "CloudUpdate.h"
#include "CloudUpdateService.h"
#include "CloudUpdateSettings.h"
#include "CloudUpdateBinaryMerge.h"
#include "Engine/Engine.h"
#include "HttpModule.h"
#include "Async/Async.h"
#include "Misc/Paths.h"

UCloudUpdateSubsystem* UCloudUpdateSubsystem::GetCloudUpdateSubsystem()
{
	return GEngine ? GEngine->GetEngineSubsystem<UCloudUpdateSubsystem>() : nullptr;
}

void UCloudUpdateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Service = MakeShared<FCloudUpdateService>(this);

	// 交换上一轮因文件被占用而暂存的补丁（.pending -> 基础文件）。
	// 备注（时序硬伤，务必知悉）：UEngineSubsystem::Initialize 的调用时机晚于引擎对项目
	// Paks 目录的自动挂载（FPakPlatformFile 在 FEngineLoop::Init 期间已挂载 pak/utoc）。
	// 因此当本行执行时，项目自带 pak/utoc 往往已被锁定，Move 交换会失败，
	// StagedForRestart 的补丁在这里通常「交换不成功」。对 IoStore(utoc/ucas) 几乎必然失效。
	// 该调用作为「尽力而为」的安全网保留；可靠的交换须放在引擎挂载之前（启动器/Launcher 侧、
	// 自定义 FPlatformFile、或插件模块更早的 StartupModule 中）。详见 FinalizePendingMerges 实现备注。
	FCloudBinaryMerge::FinalizePendingMerges(FPaths::ProjectContentDir() / TEXT("Paks"), true);
 Service->PromotePendingResourceVersion();

	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	if (Settings)
	{
		if (Settings->bAutoCheckUpdateOnStart)
		{
			CheckForUpdates();
		}
		if (Settings->bAutoCheckIntegrityOnStart)
		{
			CheckIntegrity(ECloudCheckMode::FullHash);
		}
	}
}

void UCloudUpdateSubsystem::Deinitialize()
{
	if (Service.IsValid())
	{
		Service->DetachOwner();
		Service.Reset();
	}
	Super::Deinitialize();
}

void UCloudUpdateSubsystem::CheckIntegrity(ECloudCheckMode CheckMode)
{
    if (bAutoMergeRunning) { OnIntegrityCheckFinished.Broadcast(false, {}, TEXT("自动合并正在进行")); return; }
	if (Service.IsValid())
	{
		Service->CheckIntegrity(CheckMode);
	}
}

void UCloudUpdateSubsystem::RepairIssues()
{
    if (bAutoMergeRunning) { OnRepairFinished.Broadcast(false, 0, TEXT("自动合并正在进行")); return; }
	if (Service.IsValid())
	{
		Service->RepairIssues();
	}
}

void UCloudUpdateSubsystem::CheckForUpdates()
{
    if (bAutoMergeRunning) { OnUpdateCheckFinished.Broadcast(false, false, TEXT(""), {}, TEXT("自动合并正在进行")); return; }
	if (Service.IsValid())
	{
		Service->CheckForUpdates();
	}
}

void UCloudUpdateSubsystem::ApplyUpdate(const FString& VersionId)
{
    if (bAutoMergeRunning) { OnUpdateFinished.Broadcast(false, false, VersionId, TEXT("自动合并正在进行")); return; }
	if (Service.IsValid())
	{
		Service->ApplyUpdate(VersionId);
	}
}

void UCloudUpdateSubsystem::QueryPendingUpdateSize()
{
    if (bAutoMergeRunning) { OnUpdateSizeQueryFinished.Broadcast(false, 0, 0, TEXT("自动合并正在进行")); return; }
	if (Service.IsValid())
	{
		Service->QueryPendingUpdateSize();
	}
	else if (OnUpdateSizeQueryFinished.IsBound())
	{
		OnUpdateSizeQueryFinished.Broadcast(false, 0, 0, TEXT("服务未初始化"));
	}
}

void UCloudUpdateSubsystem::ApplyLatestUpdate()
{
    if (bAutoMergeRunning) { OnUpdateFinished.Broadcast(false, false, TEXT(""), TEXT("自动合并正在进行")); return; }
	if (Service.IsValid())
	{
		Service->ApplyLatestUpdate();
	}
}

void UCloudUpdateSubsystem::AbortCurrentTask()
{
	if (Service.IsValid())
	{
		Service->Abort();
	}
}

bool UCloudUpdateSubsystem::IsBusy() const
{
	return bAutoMergeRunning || (Service.IsValid() && Service->IsBusy());
}

FString UCloudUpdateSubsystem::GetLocalVersion() const
{
	return Service.IsValid() ? Service->GetLocalVersion() : FString();
}

void UCloudUpdateSubsystem::SetLocalVersion(const FString& VersionId)
{
	if (Service.IsValid())
	{
		Service->SetLocalVersion(VersionId);
	}
}

FString UCloudUpdateSubsystem::GetServerUrl() const
{
	return Service.IsValid() ? Service->GetServerUrl() : FString();
}

void UCloudUpdateSubsystem::SetServerUrl(const FString& InUrl)
{
	if (Service.IsValid())
	{
		Service->SetServerUrl(InUrl);
	}
}

FString UCloudUpdateSubsystem::GetServerToken() const
{
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	return Settings ? Settings->ServerToken : FString();
}

void UCloudUpdateSubsystem::SetServerToken(const FString& InToken)
{
	if (UCloudUpdateSettings* Settings = GetMutableDefault<UCloudUpdateSettings>())
	{
		Settings->ServerToken = InToken;
		Settings->SaveConfig();
		UE_LOG(LogCloudUpdate, Log, TEXT("访问令牌已更新（已写入配置）"));
	}
}

bool UCloudUpdateSubsystem::IsBinaryMergeAvailable() const
{
	return FCloudBinaryMerge::IsHDiffPatchAvailable();
}

bool UCloudUpdateSubsystem::IsBinaryMergeEnabled() const
{
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	return Settings ? Settings->bEnableBinaryMerge : false;
}

void UCloudUpdateSubsystem::SetBinaryMergeEnabled(bool bEnable)
{
	if (UCloudUpdateSettings* Settings = GetMutableDefault<UCloudUpdateSettings>())
	{
		Settings->bEnableBinaryMerge = bEnable;
		Settings->SaveConfig();
		UE_LOG(LogCloudUpdate, Log, TEXT("二进制补丁合并已%s（已写入配置）"), bEnable ? TEXT("启用") : TEXT("关闭"));
	}
}

FString UCloudUpdateSubsystem::GetBinaryPatchFeatureName() const
{
	return FCloudBinaryMerge::GetFeatureName();
}

bool UCloudUpdateSubsystem::ApplyBinaryPatchToBase(const FString& BaseFilePath, const FString& PatchFilePath, const FString& FeatureName, FString& OutMergedPath)
{
	OutMergedPath = TEXT("");
	if (!Service.IsValid())
	{
		return false;
	}
    return ApplyBinaryPatchToBaseEx(BaseFilePath, PatchFilePath, OutMergedPath, FeatureName) != EBinaryMergeResult::Failed;
}

TArray<FString> UCloudUpdateSubsystem::FindPatchFiles(const FString& Directory, bool bIncludeSubdirectories) const
{
	const FString Dir = Directory.IsEmpty() ? (FPaths::ProjectContentDir() / TEXT("Paks")) : Directory;
	return FCloudBinaryMerge::FindPatchFiles(Dir, bIncludeSubdirectories);
}

void UCloudUpdateSubsystem::AutoMergePatches(const FString& Directory, bool bIncludeSubdirectories)
{
	if (IsBusy())
	{
		UE_LOG(LogCloudUpdate, Warning, TEXT("自动合并已在进行中，忽略本次请求"));
		return;
	}

	const FString Dir = Directory.IsEmpty() ? (FPaths::ProjectContentDir() / TEXT("Paks")) : Directory;
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	const FString FeatureName = Settings ? Settings->BinaryPatchFeatureName : TEXT("");

	// 先回报总文件数（即便 0 个也广播一次，便于 UI 进入「进行中」态）
	const int32 Total = FCloudBinaryMerge::FindPatchFiles(Dir, bIncludeSubdirectories).Num();
    bAutoMergeRunning = true;
    MergeProgress = FCloudMergeProgressInfo();
    MergeProgress.TotalFiles = Total;
    MergeProgress.bActive = Total > 0;
    ReportMergeProgress(MergeProgress);
	OnAutoMergeProgress.Broadcast(0, Total, TEXT(""), true);

	if (!FCloudBinaryMerge::IsHDiffPatchAvailable())
	{
		UE_LOG(LogCloudUpdate, Warning, TEXT("自动合并跳过：HDiffPatch 不可用"));
        MergeProgress.CompletedFiles = Total;
        MergeProgress.SkippedFiles = Total;
        MergeProgress.Progress = Total > 0 ? 1.0f : 0.0f;
        MergeProgress.bActive = false;
        ReportMergeProgress(MergeProgress);
        bAutoMergeRunning = false;
		OnAutoMergeFinished.Broadcast(Total == 0, 0, Total);
		return;
	}

	TWeakObjectPtr<UCloudUpdateSubsystem> Self = this;
    // Busy state prevents update and manual merge tasks from overlapping this worker.
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Self, Dir, bIncludeSubdirectories, FeatureName, Total]()
	{
		FBinaryMergeResult Result = FCloudBinaryMerge::AutoMergeDirectory(
			Dir, bIncludeSubdirectories, FeatureName,
			[Self](int32 Completed, int32 InTotal, const FString& PatchPath, bool bOk)
			{
				// 进度回调从后台线程回到游戏线程广播
				AsyncTask(ENamedThreads::GameThread, [Self, Completed, InTotal, PatchPath, bOk]()
				{
					if (Self.IsValid())
					{
                        FCloudMergeProgressInfo Progress = Self->MergeProgress;
                        Progress.TotalFiles = InTotal;
                        Progress.CurrentFile = PatchPath;
                        Progress.bCurrentFileInProgress = Completed == Progress.CompletedFiles;
                        if (!Progress.bCurrentFileInProgress)
                        {
                            Progress.CompletedFiles = Completed;
                            if (bOk) ++Progress.SucceededFiles; else ++Progress.FailedFiles;
                        }
                        Progress.Progress = InTotal > 0 ? static_cast<float>(Completed) / InTotal : 0.0f;
                        Self->ReportMergeProgress(Progress);
						Self->OnAutoMergeProgress.Broadcast(Completed, InTotal, PatchPath, bOk);
					}
				});
			});

		AsyncTask(ENamedThreads::GameThread, [Self, Result]()
		{
			if (!Self.IsValid())
			{
				return;
			}
            FCloudMergeProgressInfo Progress = Self->MergeProgress;
            Progress.CompletedFiles = Result.Succeeded + Result.Failed;
            Progress.TotalFiles = Progress.CompletedFiles;
            Progress.SucceededFiles = Result.Succeeded;
            Progress.FailedFiles = Result.Failed;
            Progress.bRestartRequired = Result.RestartRequiredCount > 0;
            Progress.bActive = Progress.bCurrentFileInProgress = false;
            Progress.Progress = Progress.TotalFiles > 0 ? 1.0f : 0.0f;
            Self->ReportMergeProgress(Progress);
			Self->bAutoMergeRunning = false;
			const bool bAllSucceeded = (Result.Failed == 0);
			Self->OnAutoMergeFinished.Broadcast(bAllSucceeded, Result.Succeeded, Result.Failed);
		});
	});
}

void UCloudUpdateSubsystem::AutoMergePatchesInPaksDir(bool bIncludeSubdirectories)
{
	AutoMergePatches(FPaths::ProjectContentDir() / TEXT("Paks"), bIncludeSubdirectories);
}

EBinaryMergeResult UCloudUpdateSubsystem::ApplyBinaryPatchToBaseEx(const FString& BaseFilePath, const FString& PatchFilePath, FString& OutMergedPath, const FString& FeatureName)
{
 OutMergedPath.Reset();
 if (!Service.IsValid() || IsBusy()) return EBinaryMergeResult::Failed;
 const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
 const FString SelectedFeature = FeatureName.IsEmpty() && Settings ? Settings->BinaryPatchFeatureName : FeatureName;
 return FCloudBinaryMerge::ApplyPatchToFileEx(BaseFilePath, PatchFilePath, OutMergedPath, SelectedFeature);
}

FString UCloudUpdateSubsystem::GetLocalGameVersion() const { return Service.IsValid() ? Service->GetLocalGameVersion() : FString(); }
FString UCloudUpdateSubsystem::GetServerGameVersion() const { return GetUpdatePlan().ServerGameVersion; }
FString UCloudUpdateSubsystem::GetLocalResourceVersion() const { return GetLocalVersion(); }
FString UCloudUpdateSubsystem::GetServerResourceVersion() const { return GetUpdatePlan().ServerResourceVersion; }
void UCloudUpdateSubsystem::SetLocalResourceVersion(const FString& VersionId) { SetLocalVersion(VersionId); }
void UCloudUpdateSubsystem::QueryUpdatePlan()
{
 if (bAutoMergeRunning || !Service.IsValid()) { OnUpdatePlanReady.Broadcast(false, GetUpdatePlan(), TEXT("服务不可用或正在合并")); return; }
 Service->QueryUpdatePlan();
}
FCloudUpdatePlan UCloudUpdateSubsystem::GetUpdatePlan() const { return Service.IsValid() ? Service->GetUpdatePlan() : FCloudUpdatePlan(); }
int32 UCloudUpdateSubsystem::GetRequiredUpdateFileCount() const { return GetUpdatePlan().RequiredFileCount; }
int64 UCloudUpdateSubsystem::GetRequiredUpdateSizeBytes() const { return GetUpdatePlan().RequiredBytes; }
FCloudDownloadProgressInfo UCloudUpdateSubsystem::GetDownloadProgress() const { return Service.IsValid() ? Service->GetDownloadProgress() : FCloudDownloadProgressInfo(); }
FCloudMergeProgressInfo UCloudUpdateSubsystem::GetMergeProgress() const { return MergeProgress; }
void UCloudUpdateSubsystem::ReportMergeProgress(const FCloudMergeProgressInfo& Progress)
{
 MergeProgress = Progress;
 MergeProgress.Percent = MergeProgress.Progress * 100.0f;
 const FCloudMergeProgressInfo Snapshot = MergeProgress;
 OnMergeProgress.Broadcast(Snapshot);
}

FString UCloudUpdateSubsystem::GetPendingResourceVersion() const { return Service.IsValid() ? Service->GetPendingResourceVersion() : FString(); }
bool UCloudUpdateSubsystem::ApplyBinaryPatchAsync(const FString& BaseFilePath, const FString& PatchFilePath, const FString& FeatureName)
{
 if (!Service.IsValid() || IsBusy()) return false;
 const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
 const FString Selected = FeatureName.IsEmpty() && Settings ? Settings->BinaryPatchFeatureName : FeatureName;
 bAutoMergeRunning = true;
 FCloudMergeProgressInfo Progress;
 Progress.TotalFiles = 1;
 Progress.CurrentFile = PatchFilePath;
 Progress.bActive = Progress.bCurrentFileInProgress = true;
 ReportMergeProgress(Progress);
 TWeakObjectPtr<UCloudUpdateSubsystem> Self = this;
 AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Self, BaseFilePath, PatchFilePath, Selected]()
 {
  FString MergedPath;
  const EBinaryMergeResult Result = FCloudBinaryMerge::ApplyPatchToFileEx(BaseFilePath, PatchFilePath, MergedPath, Selected);
  AsyncTask(ENamedThreads::GameThread, [Self, BaseFilePath, Result]()
  {
   if (!Self.IsValid()) return;
   FCloudMergeProgressInfo Completed = Self->MergeProgress;
   Completed.CompletedFiles = 1;
   Completed.SucceededFiles = Result == EBinaryMergeResult::Failed ? 0 : 1;
   Completed.FailedFiles = Result == EBinaryMergeResult::Failed ? 1 : 0;
   Completed.Progress = 1.0f;
   Completed.bActive = Completed.bCurrentFileInProgress = false;
   Completed.bRestartRequired = Result == EBinaryMergeResult::StagedForRestart;
   Self->ReportMergeProgress(Completed);
   Self->bAutoMergeRunning = false;
   Self->OnBinaryPatchFinished.Broadcast(Result != EBinaryMergeResult::Failed, BaseFilePath,
    Result == EBinaryMergeResult::StagedForRestart ? TEXT("合并已暂存，需启动前交换") : (Result == EBinaryMergeResult::Success ? TEXT("合并完成") : TEXT("合并失败")));
  });
 });
 return true;
}
