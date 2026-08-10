// ======================================================================
// CloudUpdate 核心：构造 / URL-路径 / 版本记录 / 取消
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
