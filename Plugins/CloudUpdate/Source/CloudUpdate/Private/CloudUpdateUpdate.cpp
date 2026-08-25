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
#include "CloudUpdateBinaryMerge.h"
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

	// 回滚检测：若本地版本被服务器撤销（隐藏/删除），删除本地内容并回退到上一链版本
	TArray<FCloudDownloadFile> RevokedFiles;
	FString RevokedVersion;
	const TArray<TSharedPtr<FJsonValue>>* RevokedValues = nullptr;
	if (InJson->TryGetArrayField(TEXT("revoked"), RevokedValues))
	{
		for (const TSharedPtr<FJsonValue>& Value : *RevokedValues)
		{
			const TSharedPtr<FJsonObject> Obj = Value->AsObject();
			if (!Obj.IsValid())
			{
				continue;
			}
			FString Rid;
			Obj->TryGetStringField(TEXT("versionId"), Rid);
			if (Rid.IsEmpty() || !Rid.Equals(LocalVersion, ESearchCase::IgnoreCase))
			{
				continue;
			}
			FString RType;
			Obj->TryGetStringField(TEXT("type"), RType);
			if (RType == TEXT("full"))
			{
				continue; // 整包不回滚
			}
			RevokedVersion = Rid;
			const TArray<TSharedPtr<FJsonValue>>* FileValues = nullptr;
			if (Obj->TryGetArrayField(TEXT("files"), FileValues))
			{
				for (const TSharedPtr<FJsonValue>& Fv : *FileValues)
				{
					const TSharedPtr<FJsonObject> FObj = Fv->AsObject();
					if (!FObj.IsValid())
					{
						continue;
					}
					FCloudDownloadFile File;
					FObj->TryGetStringField(TEXT("fileName"), File.FileName);
					FObj->TryGetStringField(TEXT("targetRelativePath"), File.TargetRelativePath);
					FObj->TryGetStringField(TEXT("hash"), File.Hash);
					FObj->TryGetNumberField(TEXT("size"), File.FileSize);
					FString KindStr;
					if (FObj->TryGetStringField(TEXT("kind"), KindStr))
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
					if (!File.FileName.IsEmpty())
					{
						RevokedFiles.Add(File);
					}
				}
			}
			break;
		}
	}

	if (!RevokedVersion.IsEmpty())
	{
		// 计算回退目标：比被撤销版本低的最高可用版本；没有则退到最新基础包
		FString PreviousVersion;
		TArray<FString> Candidates;
		for (const FCloudUpdateVersionInfo& Info : AllVersions)
		{
			Candidates.Add(Info.VersionId);
		}
		for (const FString& B : BaseVersions)
		{
			Candidates.Add(B);
		}
		for (const FString& C : Candidates)
		{
			if (IsVersionNewer(RevokedVersion, C))
			{
				if (PreviousVersion.IsEmpty() || IsVersionNewer(C, PreviousVersion))
				{
					PreviousVersion = C;
				}
			}
		}
		if (PreviousVersion.IsEmpty() && BaseVersions.Num() > 0)
		{
			PreviousVersion = BaseVersions.Last();
		}
		RollbackRevokedVersion(RevokedVersion, RevokedFiles, PreviousVersion);
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

		// 二进制补丁条目：下载 .patch 并合并到基础容器；缺失 HDiffPatch 时回退整文件
		if (FCloudBinaryMerge::IsPatchFile(PakName))
		{
			FCloudDownloadFile File;
			File.FileName = PakName;
			File.Url = PakUrl;
			File.TargetRelativePath = PakName;
			File.bBinaryPatch = true;
			// 根据基础名后缀推断容器类型，避免把 .utoc.patch / .ucas.patch 误判为 ContentPak
			const FString BaseName = FCloudBinaryMerge::GetBaseFileName(PakName);
			if (BaseName.EndsWith(TEXT(".pak"), ESearchCase::IgnoreCase))
			{
				File.Kind = ECloudDownloadKind::ContentPak;
			}
			else if (BaseName.EndsWith(TEXT(".utoc"), ESearchCase::IgnoreCase)
				|| BaseName.EndsWith(TEXT(".ucas"), ESearchCase::IgnoreCase))
			{
				File.Kind = ECloudDownloadKind::IoStoreContainer;
			}
			else
			{
				File.Kind = ECloudDownloadKind::ExternFile;
			}
			File.FallbackUrl = Base / PendingVersionId / TEXT("Windows") / BaseName;
			PendingFiles.Add(File);
			continue;
		}

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
			FCloudDownloadFile UtocFile;
			UtocFile.FileName = Utoc;
			UtocFile.Url = Base / PendingVersionId / TEXT("Windows") / Utoc;
			UtocFile.TargetRelativePath = Utoc;
			UtocFile.Kind = ECloudDownloadKind::IoStoreContainer;
			PendingFiles.Add(MoveTemp(UtocFile));

			FCloudDownloadFile UcasFile;
			UcasFile.FileName = Ucas;
			UcasFile.Url = Base / PendingVersionId / TEXT("Windows") / Ucas;
			UcasFile.TargetRelativePath = Ucas;
			UcasFile.Kind = ECloudDownloadKind::IoStoreContainer;
			PendingFiles.Add(MoveTemp(UcasFile));
		}
	}

	if (bDirectIoStore)
	{
		FCloudDownloadFile GlobalUtoc;
		GlobalUtoc.FileName = TEXT("global.utoc");
		GlobalUtoc.Url = Base / PendingVersionId / TEXT("Windows") / TEXT("global.utoc");
		GlobalUtoc.TargetRelativePath = TEXT("global.utoc");
		GlobalUtoc.Kind = ECloudDownloadKind::IoStoreContainer;
		PendingFiles.Add(MoveTemp(GlobalUtoc));

		FCloudDownloadFile GlobalUcas;
		GlobalUcas.FileName = TEXT("global.ucas");
		GlobalUcas.Url = Base / PendingVersionId / TEXT("Windows") / TEXT("global.ucas");
		GlobalUcas.TargetRelativePath = TEXT("global.ucas");
		GlobalUcas.Kind = ECloudDownloadKind::IoStoreContainer;
		PendingFiles.Add(MoveTemp(GlobalUcas));
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
			Obj->TryGetBoolField(TEXT("binaryPatch"), File.bBinaryPatch);
			Obj->TryGetStringField(TEXT("fallbackUrl"), File.FallbackUrl);
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
				else if (KindStr == TEXT("BinaryPatch"))
				{
					// 二进制补丁条目：根据基础文件名后缀推断容器类型
					File.bBinaryPatch = true;
					const FString BaseName = FCloudBinaryMerge::GetBaseFileName(File.FileName);
					if (BaseName.EndsWith(TEXT(".pak"), ESearchCase::IgnoreCase))
					{
						File.Kind = ECloudDownloadKind::ContentPak;
					}
					else if (BaseName.EndsWith(TEXT(".utoc"), ESearchCase::IgnoreCase)
						|| BaseName.EndsWith(TEXT(".ucas"), ESearchCase::IgnoreCase))
					{
						File.Kind = ECloudDownloadKind::IoStoreContainer;
					}
					else
					{
						File.Kind = ECloudDownloadKind::ExternFile;
					}
				}
				else
				{
					File.Kind = ECloudDownloadKind::ExternFile;
				}
			}
			// 兼容：文件名以 .patch 结尾但服务端未显式标注 kind/flag
			if (!File.bBinaryPatch && FCloudBinaryMerge::IsPatchFile(File.FileName))
			{
				File.bBinaryPatch = true;
				if (File.Kind == ECloudDownloadKind::ExternFile)
				{
					const FString BaseName = FCloudBinaryMerge::GetBaseFileName(File.FileName);
					if (BaseName.EndsWith(TEXT(".pak"), ESearchCase::IgnoreCase))
					{
						File.Kind = ECloudDownloadKind::ContentPak;
					}
					else if (BaseName.EndsWith(TEXT(".utoc"), ESearchCase::IgnoreCase)
						|| BaseName.EndsWith(TEXT(".ucas"), ESearchCase::IgnoreCase))
					{
						File.Kind = ECloudDownloadKind::IoStoreContainer;
					}
				}
			}
			if (!File.FileName.IsEmpty() && !File.Url.IsEmpty()
				&& (File.TargetRelativePath.IsEmpty() || IsSafeRelativePath(File.TargetRelativePath)))
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

	// 二进制补丁条目：下载 .patch 并合并到基础文件（或回退整文件下载）
	if (IsBinaryPatchEntry(File))
	{
		HandleBinaryPatchEntry(File);
		return;
	}

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

bool FCloudUpdateService::IsBinaryPatchEntry(const FCloudDownloadFile& InFile) const
{
	if (InFile.bBinaryPatch)
	{
		return true;
	}
	// 兼容服务器直接以 .patch 结尾的文件名标识补丁
	return FCloudBinaryMerge::IsPatchFile(InFile.FileName);
}

FString FCloudUpdateService::ResolveBaseTargetPath(const FCloudDownloadFile& InFile) const
{
	// 合并目标为基础文件（去掉末尾 .patch）。
	// pak/utoc 落在 Paks 目录；外部文件按 TargetRelativePath 去掉 .patch。
	if (InFile.Kind == ECloudDownloadKind::ExternFile)
	{
		const FString BaseRel = FCloudBinaryMerge::GetBaseFileName(InFile.TargetRelativePath);
		return GetLocalRoot() / BaseRel;
	}
	const FString BaseName = FCloudBinaryMerge::GetBaseFileName(InFile.FileName);
	return GetPakDir() / BaseName;
}

void FCloudUpdateService::HandleBinaryPatchEntry(const FCloudDownloadFile& InFile)
{
	const FString BasePath = ResolveBaseTargetPath(InFile);
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	const bool bEnabled = Settings ? Settings->bEnableBinaryMerge : true;
	const FString FeatureName = Settings ? Settings->BinaryPatchFeatureName : TEXT("");
	const bool bCanApply = bEnabled && FCloudBinaryMerge::IsHDiffPatchAvailable() && FPaths::FileExists(BasePath);

	if (Owner)
	{
		Owner->OnUpdateProgress.Broadcast(
			PendingFiles.Num() > 0 ? static_cast<float>(CurrentFileIndex) / static_cast<float>(PendingFiles.Num()) : 0.0f,
			CurrentFileIndex, PendingFiles.Num(), InFile.FileName);
	}

	if (bCanApply)
	{
		// 下载 .patch 到临时目录，再合并到基础文件
		const FString PatchDir = FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / TEXT("patches");
		IFileManager::Get().MakeDirectory(*PatchDir, true);
		const FString PatchTemp = PatchDir / InFile.FileName;
		// 备注：真正合并时写的中间文件是 BasePath + ".merged.tmp" 与 BasePath + ".pending"，
		// 这两者与 AutoMergePatches（后台线程）使用的是同一对命名。若更新与自动合并并发执行，
		// 存在同名临时文件互相覆盖的竞态（详见 AutoMergePatches / AutoMergeDirectory 备注）。
		const int32 Retries = Settings ? Settings->DownloadRetryCount : 2;
		TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
		UE_LOG(LogCloudUpdate, Log, TEXT("二进制补丁合并：下载 %s 并应用到 %s"), *InFile.Url, *BasePath);
		DownloadFileTo(InFile.Url, PatchTemp, Retries, [WeakThis, InFile, BasePath, PatchTemp, FeatureName](bool bOk)
		{
			auto Service = WeakThis.Pin();
			if (!Service.IsValid())
			{
				return;
			}
			FString MergedPath;
			const EBinaryMergeResult MergeResult = bOk
				? FCloudBinaryMerge::ApplyPatchToFileEx(BasePath, PatchTemp, MergedPath, FeatureName)
				: EBinaryMergeResult::Failed;
			IFileManager::Get().Delete(*PatchTemp, false, true);
			if (MergeResult == EBinaryMergeResult::Success || MergeResult == EBinaryMergeResult::StagedForRestart)
			{
				if (MergeResult == EBinaryMergeResult::StagedForRestart)
				{
					// 基础文件被占用（运行中 pak 已挂载），已暂存为 .pending，需重启后由 FinalizePendingMerges 交换
					bRestartRequired = true;
				}
				Service->OnBinaryPatchApplied(true, InFile, BasePath,
					MergeResult == EBinaryMergeResult::StagedForRestart);
			}
			else
			{
				// 合并失败：尝试整文件回退
				Service->TryBinaryPatchFallback(InFile, BasePath, TEXT(""));
			}
		});
	}
	else
	{
		if (!bEnabled)
		{
			UE_LOG(LogCloudUpdate, Log, TEXT("二进制补丁合并已关闭，回退整文件下载：%s"), *InFile.FileName);
		}
		else if (!FCloudBinaryMerge::IsHDiffPatchAvailable())
		{
			UE_LOG(LogCloudUpdate, Log, TEXT("HDiffPatch 不可用，回退整文件下载：%s"), *InFile.FileName);
		}
		else
		{
			UE_LOG(LogCloudUpdate, Log, TEXT("基础文件缺失，回退整文件下载：%s"), *BasePath);
		}
		TryBinaryPatchFallback(InFile, BasePath, TEXT(""));
	}
}

void FCloudUpdateService::TryBinaryPatchFallback(const FCloudDownloadFile& InFile, const FString& BasePath, const FString& PatchTemp)
{
	if (!PatchTemp.IsEmpty())
	{
		IFileManager::Get().Delete(*PatchTemp, false, true);
	}
	if (!InFile.FallbackUrl.IsEmpty())
	{
		const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
		const int32 Retries = Settings ? Settings->DownloadRetryCount : 2;
		TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
		UE_LOG(LogCloudUpdate, Log, TEXT("二进制补丁回退：整文件下载 %s -> %s"), *InFile.FallbackUrl, *BasePath);
		DownloadFileTo(InFile.FallbackUrl, BasePath, Retries, [WeakThis, InFile, BasePath](bool bOk)
		{
			auto Service = WeakThis.Pin();
			if (Service.IsValid())
			{
				Service->OnBinaryPatchApplied(bOk, InFile, BasePath);
			}
		});
	}
	else
	{
		UE_LOG(LogCloudUpdate, Error, TEXT("无法应用二进制补丁且无回退地址，更新将不完整：%s"), *InFile.FileName);
		OnBinaryPatchApplied(false, InFile, BasePath);
	}
}

void FCloudUpdateService::OnBinaryPatchApplied(bool bSuccess, const FCloudDownloadFile& InFile, const FString& BasePath, bool bStagedForRestart)
{
	if (bSuccess)
	{
		++CompletedFiles;
		UE_LOG(LogCloudUpdate, Log, TEXT("二进制补丁合并完成：%s -> %s"), *InFile.FileName, *BasePath);
		if (InFile.Kind == ECloudDownloadKind::IoStoreContainer)
		{
			// IoStore 容器无法运行时热替换，重建后需重启生效
			bRestartRequired = true;
		}
		if (InFile.Kind == ECloudDownloadKind::ContentPak)
		{
			// 仅在合并立即生效时才加入挂载列表；StagedForRestart 时基础文件仍是旧内容且被锁定，
			// 需等重启后由 FinalizePendingMerges 交换，不应在此重复挂载。
			if (!bStagedForRestart)
			{
				MergedPakPaths.AddUnique(BasePath);
			}
		}
	}
	else
	{
		++FailedFiles;
		UE_LOG(LogCloudUpdate, Error, TEXT("二进制补丁合并/回退失败：%s"), *InFile.FileName);
	}

	if (Owner)
	{
		const FString Msg = bSuccess
			? FString::Printf(TEXT("二进制补丁合并完成：%s"), *BasePath)
			: FString::Printf(TEXT("二进制补丁合并/回退失败：%s"), *InFile.FileName);
		Owner->OnBinaryPatchFinished.Broadcast(bSuccess, BasePath, Msg);
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

	// 经二进制补丁合并后生成/更新的基础 pak（如 Game.pak 被原地重建）也需要挂载
	for (const FString& PakPath : MergedPakPaths)
	{
		if (!FPaths::FileExists(PakPath) || FPaths::GetExtension(PakPath) != TEXT("pak"))
		{
			continue;
		}
		const int32 Order = UFlibPakHelper::GetPakOrderByPakPath(PakPath);
		const bool bMounted = UFlibPakHelper::MountPak(PakPath, Order, TEXT(""));
		UE_LOG(LogCloudUpdate, Log, TEXT("挂载(补丁合并) %s (Order=%d) -> %s"), *PakPath, Order, bMounted ? TEXT("成功") : TEXT("失败/编辑器下跳过"));
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

void FCloudUpdateService::RollbackRevokedVersion(const FString& RevokedVersion, const TArray<FCloudDownloadFile>& Files, const FString& TargetVersion)
{
	bool bRestart = false;
	for (const FCloudDownloadFile& File : Files)
	{
		FString Path;
		if (File.Kind == ECloudDownloadKind::ExternFile)
		{
			Path = GetLocalRoot() / File.TargetRelativePath;
		}
		else
		{
			Path = GetPakDir() / File.FileName;
		}
		if (File.Kind == ECloudDownloadKind::ContentPak && FPaths::FileExists(Path))
		{
			const int32 Order = UFlibPakHelper::GetPakOrderByPakPath(Path);
			UFlibPakHelper::UnmountPak(Path, Order);
		}
		if (FPaths::FileExists(Path))
		{
			if (IFileManager::Get().Delete(*Path, false, true))
			{
				UE_LOG(LogCloudUpdate, Log, TEXT("已删除被撤销版本的文件：%s"), *Path);
			}
			else
			{
				// 文件被占用（游戏运行中）：标记需重启后由系统释放
				bRestart = true;
				UE_LOG(LogCloudUpdate, Warning, TEXT("无法删除被占用的撤销文件（重启后生效）：%s"), *Path);
			}
		}
		if (File.Kind == ECloudDownloadKind::IoStoreContainer)
		{
			// IoStore 容器运行时无法卸载，删除后需重启生效
			bRestart = true;
		}
	}

	SaveLocalVersion(TargetVersion);
	UE_LOG(LogCloudUpdate, Log, TEXT("已回滚被撤销版本 %s，本地版本回退至 %s（需重启=%d）"),
		*RevokedVersion, *TargetVersion, bRestart ? 1 : 0);

	if (Owner)
	{
		Owner->OnRollbackFinished.Broadcast(true, bRestart, RevokedVersion,
			FString::Printf(TEXT("已回滚被撤销的版本 %s，回退至 %s%s"),
				*RevokedVersion, *TargetVersion, bRestart ? TEXT("（需重启生效）") : TEXT("")));
	}
}
