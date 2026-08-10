// ======================================================================
// CloudUpdate 更新检查与应用
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
