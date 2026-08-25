#include "CloudUpdateSubsystem.h"
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
	// 允许自签名证书：服务器启用 HTTPS（自签名或内网证书）时，启动器才能正常拉取版本/清单。
	// 仅对内网分发与自签名场景放开；若服务器使用公网合法证书，此设置无副作用。
	FHttpModule::Get().SetAllowSelfSignedCertificates(true);
	Service = MakeShared<FCloudUpdateService>(this);

	// 启动早期、pak 尚未挂载前，交换上一轮因文件被占用而暂存的补丁（.pending -> 基础文件）
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
	AsyncTask(ENamedThreads::AnyBackgroundThreadNormalTask, [Self, Dir, bIncludeSubdirectories, FeatureName, Total]()
	{
		FCloudBinaryMerge::FBinaryMergeResult Result = FCloudBinaryMerge::AutoMergeDirectory(
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