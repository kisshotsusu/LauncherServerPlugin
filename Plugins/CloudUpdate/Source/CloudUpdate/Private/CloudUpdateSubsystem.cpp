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
		Service->Abort();
		Service.Reset();
	}
	Super::Deinitialize();
}

void UCloudUpdateSubsystem::CheckIntegrity(ECloudCheckMode CheckMode)
{
	if (Service.IsValid())
	{
		Service->CheckIntegrity(CheckMode);
	}
}

void UCloudUpdateSubsystem::RepairIssues()
{
	if (Service.IsValid())
	{
		Service->RepairIssues();
	}
}

void UCloudUpdateSubsystem::CheckForUpdates()
{
	if (Service.IsValid())
	{
		Service->CheckForUpdates();
	}
}

void UCloudUpdateSubsystem::ApplyUpdate(const FString& VersionId)
{
	if (Service.IsValid())
	{
		Service->ApplyUpdate(VersionId);
	}
}

void UCloudUpdateSubsystem::QueryPendingUpdateSize()
{
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
	return Service.IsValid() && Service->IsBusy();
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
	// 备注：本函数返回 bool，会丢失 ApplyPatchToFileEx 的「StagedForRestart（需重启生效）」语义——
	// 若合并结果是暂存待重启，调用方仅得到 true，却不知道需要重启才能真正生效。
	// 若蓝图需要区分，可改用返回 EBinaryMergeResult 的接口（FCloudBinaryMerge::ApplyPatchToFileEx）。
	return FCloudBinaryMerge::ApplyPatchToFile(BaseFilePath, PatchFilePath, OutMergedPath, FeatureName);
}

TArray<FString> UCloudUpdateSubsystem::FindPatchFiles(const FString& Directory, bool bIncludeSubdirectories) const
{
	const FString Dir = Directory.IsEmpty() ? (FPaths::ProjectContentDir() / TEXT("Paks")) : Directory;
	return FCloudBinaryMerge::FindPatchFiles(Dir, bIncludeSubdirectories);
}

void UCloudUpdateSubsystem::AutoMergePatches(const FString& Directory, bool bIncludeSubdirectories)
{
	if (bAutoMergeRunning)
	{
		UE_LOG(LogCloudUpdate, Warning, TEXT("自动合并已在进行中，忽略本次请求"));
		return;
	}

	const FString Dir = Directory.IsEmpty() ? (FPaths::ProjectContentDir() / TEXT("Paks")) : Directory;
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	const FString FeatureName = Settings ? Settings->BinaryPatchFeatureName : TEXT("");

	// 先回报总文件数（即便 0 个也广播一次，便于 UI 进入「进行中」态）
	const int32 Total = FCloudBinaryMerge::FindPatchFiles(Dir, bIncludeSubdirectories).Num();
	OnAutoMergeProgress.Broadcast(0, Total, TEXT(""), true);

	if (!FCloudBinaryMerge::IsHDiffPatchAvailable())
	{
		UE_LOG(LogCloudUpdate, Warning, TEXT("自动合并跳过：HDiffPatch 不可用"));
		OnAutoMergeFinished.Broadcast(false, 0, Total);
		return;
	}

	bAutoMergeRunning = true;
	TWeakObjectPtr<UCloudUpdateSubsystem> Self = this;
	// 备注（并发风险）：bAutoMergeRunning 仅防止「两次 AutoMergePatches 并发」。
	// 但更新下载流程（FCloudUpdateService::HandleBinaryPatchEntry）也会在游戏线程/HTTP 回调中
	// 对同一基础文件做合并，二者不共享该标志。若更新进行中用户又触发自动合并，
	// 两条路径会同时写 Foo.merged.tmp / Foo.pending，存在临时文件互相覆盖的竞态。
	// 如需严格安全，应引入跨流程的合并锁或将自动合并与更新合并纳入同一串行队列。
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
			Self->bAutoMergeRunning = false;
			const bool bAllSucceeded = (Result.Failed == 0 && Result.Succeeded > 0);
			Self->OnAutoMergeFinished.Broadcast(bAllSucceeded, Result.Succeeded, Result.Failed);
		});
	});
}

void UCloudUpdateSubsystem::AutoMergePatchesInPaksDir(bool bIncludeSubdirectories)
{
	AutoMergePatches(FPaths::ProjectContentDir() / TEXT("Paks"), bIncludeSubdirectories);
}
