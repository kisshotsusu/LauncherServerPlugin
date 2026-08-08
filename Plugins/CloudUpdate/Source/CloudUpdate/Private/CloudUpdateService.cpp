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

namespace
{
	FString BytesToHex(const uint8* InBytes, int32 InCount)
	{
		static const TCHAR* HexDigits = TEXT("0123456789abcdef");
		FString Result;
		Result.Reserve(InCount * 2);
		for (int32 i = 0; i < InCount; ++i)
		{
			Result += HexDigits[(InBytes[i] >> 4) & 0xF];
			Result += HexDigits[InBytes[i] & 0xF];
		}
		return Result;
	}

	bool ComputeFileHash(const FString& InPath, int64& OutSize, FString& OutHash)
	{
		OutSize = 0;
		OutHash.Empty();
		IFileHandle* Handle = FPlatformFileManager::Get().GetPlatformFile().OpenRead(*InPath);
		if (!Handle)
		{
			return false;
		}
		FMD5 Md5;
		uint8 Buffer[1024 * 1024];
		int64 ReadBytes = 0;
		while ((ReadBytes = Handle->Read(Buffer, sizeof(Buffer))) > 0)
		{
			OutSize += ReadBytes;
			Md5.Update(Buffer, static_cast<int32>(ReadBytes));
		}
		delete Handle;
		uint8 Digest[16];
		Md5.Final(Digest);
		OutHash = BytesToHex(Digest, 16);
		return true;
	}

	FString NormalizeSlashes(const FString& InPath)
	{
		FString Result = InPath;
		Result.ReplaceInline(TEXT("\\"), TEXT("/"));
		return Result;
	}

	TArray<FString> SplitVersionParts(const FString& InVersion)
	{
		FString Tmp = InVersion;
		Tmp.ReplaceInline(TEXT("-"), TEXT("."));
		Tmp.ReplaceInline(TEXT("_"), TEXT("."));
		TArray<FString> Parts;
		Tmp.ParseIntoArray(Parts, TEXT("."), true);
		return Parts;
	}

	bool IsVersionNewer(const FString& InA, const FString& InB)
	{
		const TArray<FString> AParts = SplitVersionParts(InA);
		const TArray<FString> BParts = SplitVersionParts(InB);
		const int32 Count = FMath::Max(AParts.Num(), BParts.Num());
		for (int32 i = 0; i < Count; ++i)
		{
			const FString A = AParts.IsValidIndex(i) ? AParts[i] : TEXT("0");
			const FString B = BParts.IsValidIndex(i) ? BParts[i] : TEXT("0");
			if (A.IsNumeric() && B.IsNumeric())
			{
				const int64 AI = FCString::Atoi64(*A);
				const int64 BI = FCString::Atoi64(*B);
				if (AI != BI)
				{
					return AI > BI;
				}
			}
			else if (!A.Equals(B, ESearchCase::IgnoreCase))
			{
				return A > B;
			}
		}
		return false;
	}
}

FCloudUpdateService::FCloudUpdateService(UCloudUpdateSubsystem* InOwner)
	: Owner(InOwner)
{
}

FCloudUpdateService::~FCloudUpdateService()
{
}

void FCloudUpdateService::SetBusy(bool bInBusy)
{
	bBusy = bInBusy;
	if (!bBusy)
	{
		bAbortRequested = false;
	}
}

FString FCloudUpdateService::GetProjectName() const
{
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	if (Settings && !Settings->ProjectName.IsEmpty())
	{
		return Settings->ProjectName;
	}
	return FApp::GetProjectName();
}

FString FCloudUpdateService::GetPlatform() const
{
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	if (Settings && !Settings->Platform.IsEmpty())
	{
		return Settings->Platform;
	}
	return TEXT("Windows");
}

FString FCloudUpdateService::GetServerUrl() const
{
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	if (Settings && !Settings->ServerUrl.IsEmpty())
	{
		FString Url = Settings->ServerUrl;
		while (Url.EndsWith(TEXT("/")))
		{
			Url.LeftChopInline(1);
		}
		return Url;
	}
	return TEXT("http://127.0.0.1:8710");
}

void FCloudUpdateService::SetServerUrl(const FString& InUrl)
{
	if (UCloudUpdateSettings* Settings = GetMutableDefault<UCloudUpdateSettings>())
	{
		Settings->ServerUrl = InUrl;
		Settings->SaveConfig();
	}
}

FString FCloudUpdateService::GetLocalRoot() const
{
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	if (Settings && !Settings->LocalRootOverride.IsEmpty())
	{
		return FPaths::ConvertRelativePathToFull(Settings->LocalRootOverride);
	}
	if (!GIsEditor)
	{
		return FPaths::RootDir();
	}
	return FPaths::ProjectDir();
}

FString FCloudUpdateService::GetPakDir() const
{
	return FPaths::ProjectContentDir() / TEXT("Paks");
}

FString FCloudUpdateService::GetManifestUrl() const
{
	return GetServerUrl() / TEXT("api") / TEXT("manifest.json")
		+ FString::Printf(TEXT("?project=%s&platform=%s"),
			*FGenericPlatformHttp::UrlEncode(GetProjectName()),
			*FGenericPlatformHttp::UrlEncode(GetPlatform()));
}

FString FCloudUpdateService::GetVersionsUrl() const
{
	return GetServerUrl() / TEXT("api") / TEXT("versions");
}

FString FCloudUpdateService::GetVersionUrl(const FString& InVersionId) const
{
	return GetServerUrl() / TEXT("api") / TEXT("version") / InVersionId
		+ FString::Printf(TEXT("?platform=%s"), *FGenericPlatformHttp::UrlEncode(GetPlatform()));
}

bool FCloudUpdateService::IsPathIgnored(const FString& InRelativePath) const
{
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	if (!Settings || Settings->IgnorePathPrefixes.Num() == 0)
	{
		return false;
	}
	const FString Normalized = NormalizeSlashes(InRelativePath);
	for (const FString& Prefix : Settings->IgnorePathPrefixes)
	{
		if (Normalized.StartsWith(NormalizeSlashes(Prefix)))
		{
			return true;
		}
	}
	return false;
}

void FCloudUpdateService::FetchJson(const FString& InUrl, TFunction<void(bool, const TSharedPtr<FJsonObject>&)> InCallback)
{
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(InUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Accept"), TEXT("application/json"));
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	Request->SetTimeout(Settings ? Settings->HttpTimeoutSeconds : 60);
	Request->OnProcessRequestComplete().BindLambda([InCallback](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedSuccessfully)
	{
		TSharedPtr<FJsonObject> Json;
		bool bOk = false;
		if (bConnectedSuccessfully && Resp.IsValid() && EHttpResponseCodes::IsOk(Resp->GetResponseCode()))
		{
			const FString ResponseStr = Resp->GetContentAsString();
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseStr);
			bOk = FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid();
		}
		if (InCallback)
		{
			InCallback(bOk, Json);
		}
	});
	Request->ProcessRequest();
}

void FCloudUpdateService::DownloadFileTo(const FString& InUrl, const FString& InTargetPath, int32 InRetriesLeft, TFunction<void(bool)> InCallback)
{
	if (bAbortRequested)
	{
		if (InCallback)
		{
			InCallback(false);
		}
		return;
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(InTargetPath), true);
	const FString TempPath = InTargetPath + TEXT(".download");
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(InUrl);
	Request->SetVerb(TEXT("GET"));
	Request->SetTimeout(Settings ? Settings->HttpTimeoutSeconds : 60);
	TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
	Request->OnProcessRequestComplete().BindLambda([WeakThis, InUrl, InTargetPath, TempPath, InRetriesLeft, InCallback](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bConnectedSuccessfully)
	{
		auto Service = WeakThis.Pin();
		if (!Service)
		{
			if (InCallback)
			{
				InCallback(false);
			}
			return;
		}
		const bool bHttpOk = bConnectedSuccessfully && Resp.IsValid() && EHttpResponseCodes::IsOk(Resp->GetResponseCode());
		if (bHttpOk)
		{
			const TArray<uint8>& Content = Resp->GetContent();
			if (FFileHelper::SaveArrayToFile(Content, *TempPath))
			{
				if (IFileManager::Get().FileExists(*InTargetPath))
				{
					IFileManager::Get().Delete(*InTargetPath, false, true);
				}
				if (IFileManager::Get().Move(*InTargetPath, *TempPath, true, true))
				{
					if (InCallback)
					{
						InCallback(true);
					}
					return;
				}
			}
		}
		IFileManager::Get().Delete(*TempPath, false, true);
		if (InRetriesLeft > 0)
		{
			UE_LOG(LogCloudUpdate, Warning, TEXT("下载失败 %s，剩余重试 %d 次"), *InUrl, InRetriesLeft);
			Service->DownloadFileTo(InUrl, InTargetPath, InRetriesLeft - 1, InCallback);
		}
		else if (InCallback)
		{
			InCallback(false);
		}
	});
	Request->ProcessRequest();
}
// ===================== 完整性检查 =====================

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

// ===================== 修复文件 =====================

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

// ===================== 更新检查 =====================

void FCloudUpdateService::ParseVersionsIndex(const TSharedPtr<FJsonObject>& InJson)
{
	TArray<FCloudUpdateVersionInfo> AllVersions;
	const TArray<TSharedPtr<FJsonValue>>* VersionValues = nullptr;
	if (InJson->TryGetArrayField(TEXT("versions"), VersionValues))
	{
		for (const TSharedPtr<FJsonValue>& Value : *VersionValues)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FCloudUpdateVersionInfo Info;
			Obj->TryGetStringField(TEXT("versionId"), Info.VersionId);
			Obj->TryGetStringField(TEXT("baseVersionId"), Info.BaseVersionId);
			Obj->TryGetStringField(TEXT("date"), Info.Date);
			Obj->TryGetStringField(TEXT("type"), Info.Type);
			Obj->TryGetStringField(TEXT("url"), Info.Url);
			Obj->TryGetNumberField(TEXT("changedAssetCount"), Info.ChangedAssetCount);
			Obj->TryGetNumberField(TEXT("deletedAssetCount"), Info.DeletedAssetCount);
			Obj->TryGetNumberField(TEXT("totalSizeBytes"), Info.TotalSizeBytes);
			if (!Info.VersionId.IsEmpty())
			{
				AllVersions.Add(Info);
			}
		}
	}

	TArray<FString> UpdateChain;
	const TArray<TSharedPtr<FJsonValue>>* ChainValues = nullptr;
	if (InJson->TryGetArrayField(TEXT("updateChain"), ChainValues))
	{
		for (const TSharedPtr<FJsonValue>& Value : *ChainValues)
		{
			FString Id;
			Value->TryGetString(Id);
			if (!Id.IsEmpty())
			{
				UpdateChain.Add(Id);
			}
		}
	}

	FString LatestVersion;
	InJson->TryGetStringField(TEXT("current"), LatestVersion);

	const FString LocalVersion = LoadLocalVersion();

	// 第一步：基础包更新（整包优先，先于补丁检查）
	TArray<FString> BaseVersions;
	const TSharedPtr<FJsonObject>* BaseVersionsObj = nullptr;
	if (InJson->TryGetObjectField(TEXT("baseVersions"), BaseVersionsObj) && BaseVersionsObj->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* BaseValues = nullptr;
		if ((*BaseVersionsObj)->TryGetArrayField(GetPlatform(), BaseValues))
		{
			for (const TSharedPtr<FJsonValue>& Value : *BaseValues)
			{
				FString Id;
				Value->TryGetString(Id);
				if (!Id.IsEmpty())
				{
					BaseVersions.Add(Id);
				}
			}
		}
	}

	TArray<FCloudUpdateVersionInfo> Pending;
	for (const FString& BaseId : BaseVersions)
	{
		if (!IsVersionNewer(BaseId, LocalVersion))
		{
			continue;
		}
		bool bFound = false;
		for (const FCloudUpdateVersionInfo& Info : AllVersions)
		{
			if (Info.VersionId.Equals(BaseId, ESearchCase::IgnoreCase) && Info.Type == TEXT("full"))
			{
				Pending.Add(Info);
				bFound = true;
				break;
			}
		}
		if (!bFound)
		{
			FCloudUpdateVersionInfo Info;
			Info.VersionId = BaseId;
			Info.Type = TEXT("full");
			Info.Url = FString::Printf(TEXT("/api/version/%s?platform=%s"),
				*FGenericPlatformHttp::UrlEncode(BaseId), *FGenericPlatformHttp::UrlEncode(GetPlatform()));
			Pending.Add(Info);
		}
	}

	int32 LocalIndex = INDEX_NONE;
	for (int32 i = 0; i < UpdateChain.Num(); ++i)
	{
		if (UpdateChain[i].Equals(LocalVersion, ESearchCase::IgnoreCase))
		{
			LocalIndex = i;
			break;
		}
	}

	// 第二步：补丁更新（基础包检查完之后）
	for (int32 i = LocalIndex + 1; i < UpdateChain.Num(); ++i)
	{
		const FString& PendingId = UpdateChain[i];
		for (const FCloudUpdateVersionInfo& Info : AllVersions)
		{
			if (Info.VersionId.Equals(PendingId, ESearchCase::IgnoreCase))
			{
				Pending.Add(Info);
				break;
			}
		}
	}

	const bool bHasUpdate = !Pending.IsEmpty();
	int32 BaseUpdateCount = 0;
	for (const FCloudUpdateVersionInfo& Info : Pending)
	{
		if (Info.Type == TEXT("full"))
		{
			++BaseUpdateCount;
		}
	}
	FString Message = TEXT("已是最新版本");
	if (bHasUpdate)
	{
		if (BaseUpdateCount > 0 && BaseUpdateCount < Pending.Num())
		{
			Message = TEXT("发现新基础包与补丁更新（请先更新基础包）");
		}
		else if (BaseUpdateCount > 0)
		{
			Message = TEXT("发现新基础包");
		}
		else
		{
			Message = TEXT("发现新补丁");
		}
	}
	UE_LOG(LogCloudUpdate, Log, TEXT("更新检查完成：本地版本 %s，最新版本 %s，基础包更新 %d，待更新 %d 个"),
		*LocalVersion, *LatestVersion, BaseUpdateCount, Pending.Num());

	if (Owner)
	{
		Owner->OnUpdateCheckFinished.Broadcast(true, bHasUpdate, LatestVersion, Pending, Message);
	}
	SetBusy(false);
}

void FCloudUpdateService::CheckForUpdates()
{
	if (bBusy)
	{
		if (Owner)
		{
			Owner->OnUpdateCheckFinished.Broadcast(false, false, TEXT(""), {}, TEXT("当前已有任务在执行"));
		}
		return;
	}
	SetBusy(true);
	bAbortRequested = false;

	const FString Url = GetVersionsUrl();
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
			if (Service->Owner)
			{
				Service->Owner->OnUpdateCheckFinished.Broadcast(false, false, TEXT(""), {}, TEXT("无法获取更新索引"));
			}
			Service->SetBusy(false);
			return;
		}
		Service->ParseVersionsIndex(Json);
	});
}

// ===================== 应用更新 =====================

void FCloudUpdateService::ApplyUpdate(const FString& InVersionId)
{
	if (bBusy)
	{
		if (Owner)
		{
			Owner->OnUpdateFinished.Broadcast(false, false, InVersionId, TEXT("当前已有任务在执行"));
		}
		return;
	}
	if (InVersionId.IsEmpty())
	{
		if (Owner)
		{
			Owner->OnUpdateFinished.Broadcast(false, false, TEXT(""), TEXT("版本号不能为空"));
		}
		return;
	}

	SetBusy(true);
	bAbortRequested = false;
	PendingVersionId = InVersionId;
	PendingFiles.Empty();
	bIoStoreApplied = false;
	bRestartRequired = false;

	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	if (Settings && !Settings->HotPatcherBaseUrl.IsEmpty())
	{
		UE_LOG(LogCloudUpdate, Log, TEXT("使用 HotPatcher JSON 直连模式应用版本 %s"), *InVersionId);
		ResolveUpdateDirect();
	}
	else
	{
		const FString Url = GetVersionUrl(InVersionId);
		TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
		FetchJson(Url, [WeakThis, InVersionId](bool bOk, const TSharedPtr<FJsonObject>& Json)
		{
			auto Service = WeakThis.Pin();
			if (!Service.IsValid())
			{
				return;
			}
			if (!bOk || !Json.IsValid())
			{
				Service->FinishUpdate(false, FString::Printf(TEXT("无法获取版本 %s 的更新描述"), *InVersionId));
				return;
			}
			Service->ParseDescriptor(Json);
			Service->StartDownloadUpdateFiles();
		});
	}
}

void FCloudUpdateService::ResolveUpdateDirect()
{
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	FString Base = Settings ? Settings->HotPatcherBaseUrl : TEXT("");
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}
	if (Base.IsEmpty())
	{
		FinishUpdate(false, TEXT("HotPatcherBaseUrl 为空"));
		return;
	}

	const FString PatchConfigUrl = Base / PendingVersionId / (PendingVersionId + TEXT("_PatchConfig.json"));
	TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
	FetchJson(PatchConfigUrl, [WeakThis](bool bOk, const TSharedPtr<FJsonObject>& Json)
	{
		auto Service = WeakThis.Pin();
		if (!Service.IsValid())
		{
			return;
		}
		if (!bOk || !Json.IsValid())
		{
			Service->FinishUpdate(false, TEXT("无法获取 HotPatcher PatchConfig JSON"));
			return;
		}
		Service->OnPatchConfigFetched(Json);
	});
}

void FCloudUpdateService::OnPatchConfigFetched(const TSharedPtr<FJsonObject>& InJson)
{
	FString VersionId;
	InJson->TryGetStringField(TEXT("versionId"), VersionId);

	bool bIoStore = false;
	const TSharedPtr<FJsonObject>* IoStoreObj = nullptr;
	if (InJson->TryGetObjectField(TEXT("ioStoreSettings"), IoStoreObj) && IoStoreObj->IsValid())
	{
		(*IoStoreObj)->TryGetBoolField(TEXT("bIoStore"), bIoStore);
	}

	FString Platform = GetPlatform();
	const TArray<TSharedPtr<FJsonValue>>* PlatformValues = nullptr;
	if (InJson->TryGetArrayField(TEXT("pakTargetPlatforms"), PlatformValues) && PlatformValues->Num() > 0)
	{
		(*PlatformValues)[0]->TryGetString(Platform);
	}

	bDirectIoStore = bIoStore;
	bRestartRequired = bIoStore;
	UE_LOG(LogCloudUpdate, Log, TEXT("PatchConfig 解析完成：版本 %s，平台 %s，IoStore=%d"), *VersionId, *Platform, bIoStore ? 1 : 0);

	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	FString Base = Settings ? Settings->HotPatcherBaseUrl : TEXT("");
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}

	const FString PakInfoUrl = Base / PendingVersionId / (PendingVersionId + TEXT("_PakFilesInfo.json"));
	TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
	FetchJson(PakInfoUrl, [WeakThis, Platform, bIoStore, Base](bool bOk, const TSharedPtr<FJsonObject>& Json)
	{
		auto Service = WeakThis.Pin();
		if (!Service.IsValid())
		{
			return;
		}
		if (!bOk || !Json.IsValid())
		{
			Service->FinishUpdate(false, TEXT("无法获取 HotPatcher PakFilesInfo JSON"));
			return;
		}
		Service->OnPakFilesInfoFetched(Json);
	});
}

void FCloudUpdateService::OnPakFilesInfoFetched(const TSharedPtr<FJsonObject>& InJson)
{
	const TSharedPtr<FJsonObject>* PlatformMapObj = nullptr;
	if (!InJson->TryGetObjectField(TEXT("pakFilesMap"), PlatformMapObj) || !PlatformMapObj->IsValid())
	{
		FinishUpdate(false, TEXT("PakFilesInfo 中缺少 pakFilesMap"));
		return;
	}

	TArray<FString> PakNames;
	const TSharedPtr<FJsonObject>* PlatformObj = nullptr;
	FString Platform = GetPlatform();
	if ((*PlatformMapObj)->TryGetObjectField(Platform, PlatformObj) && PlatformObj->IsValid())
	{
		const TArray<TSharedPtr<FJsonValue>>* PakInfos = nullptr;
		if ((*PlatformObj)->TryGetArrayField(TEXT("pakFileInfos"), PakInfos))
		{
			for (const TSharedPtr<FJsonValue>& Value : *PakInfos)
			{
				const TSharedPtr<FJsonObject> Obj = Value->AsObject();
				if (!Obj.IsValid())
				{
					continue;
				}
				FString FileName;
				Obj->TryGetStringField(TEXT("fileName"), FileName);
				if (!FileName.IsEmpty())
				{
					PakNames.Add(FileName);
				}
			}
		}
	}

	if (PakNames.IsEmpty())
	{
		FinishUpdate(false, TEXT("PakFilesInfo 中没有找到该平台的 Pak 文件"));
		return;
	}

	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	FString Base = Settings ? Settings->HotPatcherBaseUrl : TEXT("");
	while (Base.EndsWith(TEXT("/")))
	{
		Base.LeftChopInline(1);
	}

		for (const FString& PakName : PakNames)
	{
		const FString PakUrl = Base / PendingVersionId / TEXT("Windows") / PakName;
		FCloudDownloadFile File;
		File.FileName = PakName;
		File.Url = PakUrl;
		File.TargetRelativePath = PakName;
		File.Kind = ECloudDownloadKind::ContentPak;
		PendingFiles.Add(File);

		if (bDirectIoStore)
		{
			FString BaseName = PakName;
			BaseName.RemoveFromEnd(TEXT(".pak"));
			const FString Utoc = BaseName + TEXT(".utoc");
			const FString Ucas = BaseName + TEXT(".ucas");
			PendingFiles.Add(FCloudDownloadFile{ Utoc, Base / PendingVersionId / TEXT("Windows") / Utoc, Utoc, TEXT(""), 0, ECloudDownloadKind::IoStoreContainer });
			PendingFiles.Add(FCloudDownloadFile{ Ucas, Base / PendingVersionId / TEXT("Windows") / Ucas, Ucas, TEXT(""), 0, ECloudDownloadKind::IoStoreContainer });
		}
	}

	if (bDirectIoStore)
	{
		PendingFiles.Add(FCloudDownloadFile{ TEXT("global.utoc"), Base / PendingVersionId / TEXT("Windows") / TEXT("global.utoc"), TEXT("global.utoc"), TEXT(""), 0, ECloudDownloadKind::IoStoreContainer });
		PendingFiles.Add(FCloudDownloadFile{ TEXT("global.ucas"), Base / PendingVersionId / TEXT("Windows") / TEXT("global.ucas"), TEXT("global.ucas"), TEXT(""), 0, ECloudDownloadKind::IoStoreContainer });
	}

	UE_LOG(LogCloudUpdate, Log, TEXT("根据 HotPatcher JSON 生成 %d 个待下载文件"), PendingFiles.Num());
	StartDownloadUpdateFiles();
}

void FCloudUpdateService::ParseDescriptor(const TSharedPtr<FJsonObject>& InJson)
{
	PendingFiles.Empty();
	const TArray<TSharedPtr<FJsonValue>>* FileValues = nullptr;
	if (InJson->TryGetArrayField(TEXT("files"), FileValues))
	{
		for (const TSharedPtr<FJsonValue>& Value : *FileValues)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FCloudDownloadFile File;
			Obj->TryGetStringField(TEXT("fileName"), File.FileName);
			Obj->TryGetStringField(TEXT("url"), File.Url);
			Obj->TryGetStringField(TEXT("targetRelativePath"), File.TargetRelativePath);
			Obj->TryGetStringField(TEXT("hash"), File.Hash);
			Obj->TryGetNumberField(TEXT("size"), File.FileSize);
			FString KindStr;
			if (Obj->TryGetStringField(TEXT("kind"), KindStr))
			{
				if (KindStr == TEXT("ContentPak"))
				{
					File.Kind = ECloudDownloadKind::ContentPak;
				}
				else if (KindStr == TEXT("IoStore"))
				{
					File.Kind = ECloudDownloadKind::IoStoreContainer;
				}
				else
				{
					File.Kind = ECloudDownloadKind::ExternFile;
				}
			}
			if (!File.FileName.IsEmpty() && !File.Url.IsEmpty())
			{
				PendingFiles.Add(File);
				if (File.Kind == ECloudDownloadKind::IoStoreContainer)
				{
					bIoStoreApplied = true;
				}
			}
		}
	}
	bool bRestart = false;
	InJson->TryGetBoolField(TEXT("restartRequired"), bRestart);
	bRestartRequired = bRestart;

	if (PendingFiles.IsEmpty())
	{
		UE_LOG(LogCloudUpdate, Warning, TEXT("版本 %s 没有可下载的文件"), *PendingVersionId);
	}
}

void FCloudUpdateService::StartDownloadUpdateFiles()
{
	if (PendingFiles.IsEmpty())
	{
		FinishUpdate(false, TEXT("更新描述中没有可下载的文件"));
		return;
	}
	CurrentFileIndex = 0;
	CompletedFiles = 0;
	FailedFiles = 0;
	if (Owner)
	{
		Owner->OnUpdateProgress.Broadcast(0.0f, 0, PendingFiles.Num(), TEXT(""));
	}
	DownloadNextUpdateFile();
}

void FCloudUpdateService::DownloadNextUpdateFile()
{
	if (bAbortRequested || CurrentFileIndex >= PendingFiles.Num())
	{
		if (FailedFiles > 0)
		{
			FinishUpdate(false, FString::Printf(TEXT("更新失败：成功 %d / 失败 %d"), CompletedFiles, FailedFiles));
			return;
		}
		MountPendingPaks();
		SaveLocalVersion(PendingVersionId);
		const bool bRestart = bRestartRequired;
		FinishUpdate(true, bRestart
			? TEXT("更新完成（包含 IoStore 容器，建议重启游戏后生效）")
			: TEXT("更新完成"));
		return;
	}

	const FCloudDownloadFile& File = PendingFiles[CurrentFileIndex];
	FString TargetPath;
	if (File.Kind == ECloudDownloadKind::ExternFile)
	{
		TargetPath = GetLocalRoot() / File.TargetRelativePath;
	}
	else
	{
		TargetPath = GetPakDir() / File.FileName;
	}

	if (Owner)
	{
		Owner->OnUpdateProgress.Broadcast(
			PendingFiles.Num() > 0 ? static_cast<float>(CurrentFileIndex) / static_cast<float>(PendingFiles.Num()) : 0.0f,
			CurrentFileIndex, PendingFiles.Num(), File.FileName);
	}

	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	const int32 Retries = Settings ? Settings->DownloadRetryCount : 2;
	TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
	DownloadFileTo(File.Url, TargetPath, Retries, [WeakThis, File](bool bOk)
	{
		auto Service = WeakThis.Pin();
		if (Service.IsValid())
		{
			Service->OnUpdateFileDownloaded(bOk, File);
		}
	});
}

void FCloudUpdateService::OnUpdateFileDownloaded(bool bSuccess, const FCloudDownloadFile& InFile)
{
	if (bSuccess)
	{
		++CompletedFiles;
		UE_LOG(LogCloudUpdate, Log, TEXT("已下载：%s"), *InFile.FileName);
	}
	else
	{
		++FailedFiles;
		UE_LOG(LogCloudUpdate, Error, TEXT("下载失败：%s (%s)"), *InFile.FileName, *InFile.Url);
	}
	++CurrentFileIndex;
	DownloadNextUpdateFile();
}

void FCloudUpdateService::MountPendingPaks()
{
	for (const FCloudDownloadFile& File : PendingFiles)
	{
		if (File.Kind != ECloudDownloadKind::ContentPak)
		{
			continue;
		}
		const FString PakPath = GetPakDir() / File.FileName;
		if (!FPaths::FileExists(PakPath) || FPaths::GetExtension(PakPath) != TEXT("pak"))
		{
			continue;
		}
		const int32 Order = UFlibPakHelper::GetPakOrderByPakPath(PakPath);
		const bool bMounted = UFlibPakHelper::MountPak(PakPath, Order, TEXT(""));
		UE_LOG(LogCloudUpdate, Log, TEXT("挂载 %s (Order=%d) -> %s"), *PakPath, Order, bMounted ? TEXT("成功") : TEXT("失败/编辑器下跳过"));
	}
	if (bRestartRequired)
	{
		UE_LOG(LogCloudUpdate, Log, TEXT("本次更新包含 IoStore 容器，重启后由引擎自动挂载"));
	}
}

void FCloudUpdateService::FinishUpdate(bool bSuccess, const FString& InMessage)
{
	if (Owner)
	{
		Owner->OnUpdateFinished.Broadcast(bSuccess, bRestartRequired, PendingVersionId, InMessage);
	}
	SetBusy(false);
}

// ===================== 本地版本记录 =====================

FString FCloudUpdateService::LoadLocalVersion() const
{
	const FString Path = FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / TEXT("local_version.json");
	FString JsonStr;
	if (FFileHelper::LoadFileToString(JsonStr, *Path))
	{
		FCloudLocalVersion Local;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		TSharedPtr<FJsonObject> Json;
		if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
		{
			Json->TryGetStringField(TEXT("versionId"), Local.VersionId);
			if (!Local.VersionId.IsEmpty())
			{
				return Local.VersionId;
			}
		}
	}
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	return Settings ? Settings->CurrentVersionId : TEXT("");
}

void FCloudUpdateService::SaveLocalVersion(const FString& InVersionId)
{
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("CloudUpdate");
	IFileManager::Get().MakeDirectory(*Dir, true);

	FCloudLocalVersion Local;
	Local.VersionId = InVersionId;
	Local.AppliedAt = FDateTime::Now().ToString();

	FString JsonStr;
	FJsonObjectConverter::UStructToJsonObjectString(Local, JsonStr);
	const FString Path = Dir / TEXT("local_version.json");
	if (FFileHelper::SaveStringToFile(JsonStr, *Path))
	{
		UE_LOG(LogCloudUpdate, Log, TEXT("本地版本已记录：%s (%s)"), *InVersionId, *Path);
	}

	if (UCloudUpdateSettings* Settings = GetMutableDefault<UCloudUpdateSettings>())
	{
		Settings->CurrentVersionId = InVersionId;
		Settings->SaveConfig();
	}
}

void FCloudUpdateService::SetLocalVersion(const FString& InVersionId)
{
	SaveLocalVersion(InVersionId);
}

FString FCloudUpdateService::GetLocalVersion() const
{
	return LoadLocalVersion();
}

void FCloudUpdateService::Abort()
{
	bAbortRequested = true;
}
