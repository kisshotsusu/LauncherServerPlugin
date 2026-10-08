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
#include "GeneralProjectSettings.h"
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
	return GetServerUrl() / TEXT("api") / TEXT("versions") + TEXT("?platform=") + FGenericPlatformHttp::UrlEncode(GetPlatform());
}

FString FCloudUpdateService::GetVersionUrl(const FString& InVersionId) const
{
	return GetServerUrl() / TEXT("api") / TEXT("version") / FGenericPlatformHttp::UrlEncode(InVersionId)
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
	const FString Path = GetVersionRecordPath();
	FString JsonStr;
	if (FFileHelper::LoadFileToString(JsonStr, *Path))
	{
		FCloudLocalVersion Local;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonStr);
		TSharedPtr<FJsonObject> Json;
		if (FJsonSerializer::Deserialize(Reader, Json) && Json.IsValid())
		{
            FString RecordedGame;
            Json->TryGetStringField(TEXT("installedGameVersion"), RecordedGame);
            if (!RecordedGame.IsEmpty() && RecordedGame != GetLocalGameVersion())
            {
                const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
                return Settings->InitialResourceVersionId.IsEmpty() ? Settings->CurrentVersionId : Settings->InitialResourceVersionId;
            }
			if (!Json->TryGetStringField(TEXT("resourceVersionId"), Local.VersionId))
    Json->TryGetStringField(TEXT("versionId"), Local.VersionId);
			if (!Local.VersionId.IsEmpty())
			{
				return Local.VersionId;
			}
		}
	}
	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	return Settings ? (Settings->InitialResourceVersionId.IsEmpty() ? Settings->CurrentVersionId : Settings->InitialResourceVersionId) : TEXT("");
}

void FCloudUpdateService::SaveLocalVersion(const FString& InVersionId)
{
	const FString Dir = FPaths::GetPath(GetVersionRecordPath());
	IFileManager::Get().MakeDirectory(*Dir, true);

	FCloudLocalVersion Local;
	Local.VersionId = InVersionId;
	Local.AppliedAt = FDateTime::Now().ToString();

	FString JsonStr;
	TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
 Record->SetNumberField(TEXT("schemaVersion"), 2);
 Record->SetStringField(TEXT("versionId"), InVersionId);
 Record->SetStringField(TEXT("resourceVersionId"), InVersionId);
 Record->SetStringField(TEXT("installedGameVersion"), GetLocalGameVersion());
 Record->SetStringField(TEXT("appliedAt"), Local.AppliedAt);
 FJsonSerializer::Serialize(Record, TJsonWriterFactory<>::Create(&JsonStr));
	const FString Path = GetVersionRecordPath();
	if (FFileHelper::SaveStringToFile(JsonStr, *Path))
	{
		UE_LOG(LogCloudUpdate, Log, TEXT("本地版本已记录：%s (%s)"), *InVersionId, *Path);
	}

}

void FCloudUpdateService::SetLocalVersion(const FString& InVersionId)
{
    SaveLocalVersion(InVersionId);
    UpdatePlan.bValid = false;
    PlannedVersions.Reset();
}

FString FCloudUpdateService::GetLocalVersion() const
{
	return LoadLocalVersion();
}

void FCloudUpdateService::Abort()
{
	bAbortRequested = true;
}

FString FCloudUpdateService::GetLocalGameVersion() const
{
 const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
 if (Settings && !Settings->GameVersionOverride.IsEmpty()) return Settings->GameVersionOverride;
 return GetDefault<UGeneralProjectSettings>()->ProjectVersion;
}

FString FCloudUpdateService::GetVersionRecordPath() const
{
 return LocalVersionRecordPath.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("CloudUpdate/local_version.json") : LocalVersionRecordPath;
}
FString FCloudUpdateService::GetPendingResourceVersion() const
{
 FString Text, Pending, RecordedGame;
 TSharedPtr<FJsonObject> Json;
 if (FFileHelper::LoadFileToString(Text, *GetVersionRecordPath()) && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) && Json.IsValid())
 {
  Json->TryGetStringField(TEXT("installedGameVersion"), RecordedGame);
  if (!RecordedGame.IsEmpty() && RecordedGame != GetLocalGameVersion()) return TEXT("");
  Json->TryGetStringField(TEXT("pendingResourceVersionId"), Pending);
 }
 return Pending;
}
void FCloudUpdateService::SavePendingResourceVersion(const FString& Version)
{
 TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
 Json->SetNumberField(TEXT("schemaVersion"), 2);
 Json->SetStringField(TEXT("versionId"), LoadLocalVersion());
 Json->SetStringField(TEXT("resourceVersionId"), LoadLocalVersion());
 Json->SetStringField(TEXT("installedGameVersion"), GetLocalGameVersion());
 Json->SetStringField(TEXT("pendingResourceVersionId"), Version);
 TArray<TSharedPtr<FJsonValue>> Files;
 for (const FCloudDownloadFile& File : PendingFiles)
 {
  const FString Path = IsBinaryPatchEntry(File) ? ResolveBaseTargetPath(File) :
   (File.Kind == ECloudDownloadKind::ExternFile ? GetLocalRoot() / File.TargetRelativePath : GetPakDir() / File.FileName);
  TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
  Entry->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(Path));
  Entry->SetStringField(TEXT("hash"), IsBinaryPatchEntry(File) ? TEXT("") : File.Hash);
  Files.Add(MakeShared<FJsonValueObject>(Entry));
 }
 Json->SetArrayField(TEXT("pendingFiles"), Files);
 FString Text;
 FJsonSerializer::Serialize(Json, TJsonWriterFactory<>::Create(&Text));
 IFileManager::Get().MakeDirectory(*FPaths::GetPath(GetVersionRecordPath()), true);
 FFileHelper::SaveStringToFile(Text, *GetVersionRecordPath());
}
void FCloudUpdateService::PromotePendingResourceVersion()
{
 const FString Pending = GetPendingResourceVersion();
 if (Pending.IsEmpty()) return;
 FString Text;
 TSharedPtr<FJsonObject> Json;
 if (!FFileHelper::LoadFileToString(Text, *GetVersionRecordPath()) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Json) || !Json.IsValid()) return;
 const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
 if (!Json->TryGetArrayField(TEXT("pendingFiles"), Values) || Values->IsEmpty()) return;
 for (const auto& Value : *Values)
 {
  if (!Value.IsValid() || Value->Type != EJson::Object) return;
  const auto Entry = Value->AsObject();
  FString Path, Hash;
  Entry->TryGetStringField(TEXT("path"), Path);
  Entry->TryGetStringField(TEXT("hash"), Hash);
  if (Path.IsEmpty() || !IFileManager::Get().FileExists(*Path) || IFileManager::Get().FileExists(*(Path + TEXT(".pending")))) return;
  if (!Hash.IsEmpty())
  {
   int64 Size = 0; FString Actual;
   if (!CloudUpdatePrivate::ComputeFileHash(Path, Size, Actual) || !Actual.Equals(Hash, ESearchCase::IgnoreCase)) return;
  }
 }
 SaveLocalVersion(Pending);
}
