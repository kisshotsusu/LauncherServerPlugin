// ======================================================================
// CloudUpdate 完整性检查与修复
// (由 CloudUpdateService.cpp 按职责拆分，逻辑与原文一致)
// ======================================================================
#include "CloudUpdateService.h"
#include "CloudUpdateSubsystem.h"
#include "CloudUpdateSettings.h"
#include "CloudUpdate.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "JsonObjectConverter.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Async/Async.h"
#include "Misc/App.h"
#include "FlibPakHelper.h"

#include "CloudUpdateUtil.h"
using namespace CloudUpdatePrivate;

void FCloudUpdateService::ParseManifest(const TSharedPtr<FJsonObject>& InJson)
{
	ServerManifest = FCloudUpdateManifest();
	InJson->TryGetStringField(TEXT("project"), ServerManifest.Project);
	InJson->TryGetStringField(TEXT("platform"), ServerManifest.Platform);
	InJson->TryGetStringField(TEXT("baseVersionId"), ServerManifest.BaseVersionId);

	const TArray<TSharedPtr<FJsonValue>>* FileValues = nullptr;
	if (InJson->TryGetArrayField(TEXT("files"), FileValues))
	{
		for (const TSharedPtr<FJsonValue>& FileValue : *FileValues)
		{
			const TSharedPtr<FJsonObject> FileObj = FileValue->AsObject();
			if (!FileObj.IsValid())
			{
				continue;
			}
			FCloudFileEntry Entry;
			FileObj->TryGetStringField(TEXT("path"), Entry.RelativePath);
			FileObj->TryGetNumberField(TEXT("size"), Entry.FileSize);
			FileObj->TryGetStringField(TEXT("hash"), Entry.Hash);
			if (!FileObj->TryGetStringField(TEXT("hashType"), Entry.HashType))
			{
				Entry.HashType = TEXT("md5");
			}
			if (!Entry.RelativePath.IsEmpty())
			{
				ServerManifest.Files.Add(Entry);
			}
		}
	}
}

void FCloudUpdateService::RunLocalComparison()
{
	struct FWorkItem
	{
		int32 Index = 0;
		FString FullPath;
	};

	TArray<FWorkItem> WorkItems;
	WorkItems.Reserve(ServerManifest.Files.Num());
	for (int32 i = 0; i < ServerManifest.Files.Num(); ++i)
	{
		const FCloudFileEntry& Entry = ServerManifest.Files[i];
		if (IsPathIgnored(Entry.RelativePath))
		{
			continue;
		}
		FWorkItem Item;
		Item.Index = i;
		Item.FullPath = GetLocalRoot() / Entry.RelativePath;
		WorkItems.Add(Item);
	}

	UE_LOG(LogCloudUpdate, Log, TEXT("开始本地完整性比对，共 %d 个文件"), WorkItems.Num());
	TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
	Async(EAsyncExecution::ThreadPool, [WorkItems, WeakThis]()
	{
		TArray<TPair<int32, FString>> Results;
		Results.Reserve(WorkItems.Num());
		for (const FWorkItem& Item : WorkItems)
		{
			int64 Size = 0;
			FString Hash;
			ComputeFileHash(Item.FullPath, Size, Hash);
			Results.Emplace(Item.Index, Hash);
		}
		AsyncTask(ENamedThreads::GameThread, [Results, WeakThis]()
		{
			auto Service = WeakThis.Pin();
			if (Service.IsValid())
			{
				Service->CompareLocalResults(Results);
			}
		});
	});
}

void FCloudUpdateService::CompareLocalResults(const TArray<TPair<int32, FString>>& InResults)
{
	TArray<FCloudFileIssue> Issues;
	const bool bFullHash = (IntegrityMode == ECloudCheckMode::FullHash);
	const FString LocalRoot = GetLocalRoot();

	for (const TPair<int32, FString>& Result : InResults)
	{
		const int32 Index = Result.Key;
		if (!ServerManifest.Files.IsValidIndex(Index))
		{
			continue;
		}
		const FCloudFileEntry& Expected = ServerManifest.Files[Index];
		const FString LocalPath = LocalRoot / Expected.RelativePath;
		FCloudFileIssue Issue;
		Issue.RelativePath = Expected.RelativePath;
		Issue.ExpectedHash = Expected.Hash;
		Issue.ExpectedSize = Expected.FileSize;

		int64 LocalSize = 0;
		FString LocalHash;
		const bool bExists = ComputeFileHash(LocalPath, LocalSize, LocalHash);
		Issue.ActualSize = LocalSize;
		Issue.ActualHash = LocalHash;

		if (!bExists)
		{
			Issue.IssueType = ECloudFileIssueType::Missing;
			Issues.Add(Issue);
		}
		else if (LocalSize != Expected.FileSize)
		{
			Issue.IssueType = ECloudFileIssueType::SizeMismatch;
			Issues.Add(Issue);
		}
		else if (bFullHash && !Expected.Hash.IsEmpty() && !LocalHash.IsEmpty() && !LocalHash.Equals(Expected.Hash, ESearchCase::IgnoreCase))
		{
			Issue.IssueType = ECloudFileIssueType::HashMismatch;
			Issues.Add(Issue);
		}
	}

	UE_LOG(LogCloudUpdate, Log, TEXT("完整性检查完成，发现 %d 个问题"), Issues.Num());
	FinishIntegrityCheck(true, Issues, FString::Printf(TEXT("检查完成，共发现 %d 个问题文件"), Issues.Num()));
}

void FCloudUpdateService::FinishIntegrityCheck(bool bSuccess, const TArray<FCloudFileIssue>& InIssues, const FString& InMessage)
{
	if (Owner)
	{
		Owner->OnIntegrityCheckFinished.Broadcast(bSuccess, InIssues, InMessage);
	}
	SetBusy(false);
}

void FCloudUpdateService::CheckIntegrity(ECloudCheckMode InMode)
{
	if (bBusy)
	{
		if (Owner)
		{
			Owner->OnIntegrityCheckFinished.Broadcast(false, {}, TEXT("当前已有任务在执行"));
		}
		return;
	}
	SetBusy(true);
	bAbortRequested = false;
	IntegrityMode = InMode;

	const FString Url = GetManifestUrl();
	TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
	FetchJson(Url, [WeakThis](bool bOk, const TSharedPtr<FJsonObject>& Json)
	{
		auto Service = WeakThis.Pin();
		if (!Service.IsValid())
		{
			return;
		}
		if (!bOk || !Json.IsValid())
		{
			Service->FinishIntegrityCheck(false, {}, TEXT("无法获取服务端完整性清单"));
			return;
		}
		Service->ParseManifest(Json);
		Service->RunLocalComparison();
	});
}

void FCloudUpdateService::RepairIssues()
{
	if (bBusy)
	{
		if (Owner)
		{
			Owner->OnRepairFinished.Broadcast(false, 0, TEXT("当前已有任务在执行"));
		}
		return;
	}
	if (PendingIssues.IsEmpty())
	{
		if (Owner)
		{
			Owner->OnRepairFinished.Broadcast(true, 0, TEXT("没有待修复的文件，请先执行完整性检查"));
		}
		return;
	}
	SetBusy(true);
	bAbortRequested = false;
	StartRepair();
}

void FCloudUpdateService::StartRepair()
{
	CurrentFileIndex = 0;
	CompletedFiles = 0;
	FailedFiles = 0;
	DownloadNextRepairFile();
}

void FCloudUpdateService::DownloadNextRepairFile()
{
	if (bAbortRequested || CurrentFileIndex >= PendingIssues.Num())
	{
		const FString Message = FString::Printf(TEXT("修复完成：成功 %d / 失败 %d"), CompletedFiles, FailedFiles);
		if (Owner)
		{
			Owner->OnRepairFinished.Broadcast(!bAbortRequested, CompletedFiles, Message);
		}
		SetBusy(false);
		return;
	}

	const FCloudFileIssue& Issue = PendingIssues[CurrentFileIndex];
	if (Owner)
	{
		Owner->OnRepairProgress.Broadcast(CurrentFileIndex, PendingIssues.Num(), Issue.RelativePath);
	}

	FString Url = GetServerUrl() / TEXT("files") / TEXT("packages") / GetPlatform();
	if (!ServerManifest.BaseVersionId.IsEmpty())
	{
		Url = Url / ServerManifest.BaseVersionId;
	}
	Url = Url / FGenericPlatformHttp::UrlEncode(Issue.RelativePath);
	const FString Target = GetLocalRoot() / Issue.RelativePath;
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	const int32 Retries = Settings ? Settings->DownloadRetryCount : 2;

	TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
	DownloadFileTo(Url, Target, Retries, [WeakThis, Issue](bool bOk)
	{
		auto Service = WeakThis.Pin();
		if (Service.IsValid())
		{
			Service->OnRepairFileDownloaded(bOk, Issue);
		}
	});
}

void FCloudUpdateService::OnRepairFileDownloaded(bool bSuccess, const FCloudFileIssue& InIssue)
{
	bool bVerified = bSuccess;
	if (bVerified)
	{
		const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
		if (Settings && Settings->bVerifyDownloadHash && !InIssue.ExpectedHash.IsEmpty())
		{
			int64 Size = 0;
			FString Hash;
			const FString Target = GetLocalRoot() / InIssue.RelativePath;
			ComputeFileHash(Target, Size, Hash);
			if (!Hash.Equals(InIssue.ExpectedHash, ESearchCase::IgnoreCase))
			{
				bVerified = false;
				UE_LOG(LogCloudUpdate, Error, TEXT("文件 %s 下载后哈希校验失败"), *InIssue.RelativePath);
			}
		}
	}

	if (bVerified)
	{
		++CompletedFiles;
		UE_LOG(LogCloudUpdate, Log, TEXT("已修复文件：%s"), *InIssue.RelativePath);
	}
	else
	{
		++FailedFiles;
		UE_LOG(LogCloudUpdate, Error, TEXT("修复失败：%s"), *InIssue.RelativePath);
	}
	++CurrentFileIndex;
	DownloadNextRepairFile();
}
