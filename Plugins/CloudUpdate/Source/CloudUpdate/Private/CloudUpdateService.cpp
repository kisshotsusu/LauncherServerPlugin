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
#include "HAL/PlatformProperties.h"
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
    return GetRuntimeUpdatePlatform();
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
#if PLATFORM_ANDROID || PLATFORM_IOS
        return FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / TEXT("Files");
#else
		return FPaths::RootDir();
#endif
	}
	return FPaths::ProjectDir();
}

FString FCloudUpdateService::GetPakDir() const
{
    return GetUpdatePakDirectory();
}

FString FCloudUpdateService::ResolveLocalFilePath(const FString& RelativePath, bool bForWrite) const
{
    FString Writable = GetLocalRoot() / RelativePath;
#if PLATFORM_ANDROID || PLATFORM_IOS
    const FString Extension = FPaths::GetExtension(RelativePath).ToLower();
    if (Extension == TEXT("pak") || Extension == TEXT("utoc") || Extension == TEXT("ucas"))
        Writable = GetPakDir() / FPaths::GetCleanFilename(RelativePath);
    if (!bForWrite && !IFileManager::Get().FileExists(*Writable))
        return FPaths::RootDir() / RelativePath;
#endif
    return Writable;
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
	TSharedPtr<FJsonObject> Json;
	if (ReadLocalVersionRecord(Json))
	{
		FCloudLocalVersion Local;
		if (Json.IsValid())
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
 Record->SetStringField(TEXT("platform"), GetPlatform());
 Record->SetStringField(TEXT("versionId"), InVersionId);
 Record->SetStringField(TEXT("resourceVersionId"), InVersionId);
 Record->SetStringField(TEXT("installedGameVersion"), GetLocalGameVersion());
 Record->SetStringField(TEXT("appliedAt"), Local.AppliedAt);
 // Preserve installation evidence across restarts and incremental updates.
 TMap<FString, TSharedPtr<FJsonValue>> Installed;
 TSharedPtr<FJsonObject> Previous;
 if (ReadLocalVersionRecord(Previous))
 {
  for (const TCHAR* Key : {TEXT("installedFiles"), TEXT("pendingFiles")})
  {
   const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
   if (Previous->TryGetArrayField(Key, Files))
    for (const auto& Value : *Files)
     if (Value.IsValid() && Value->Type == EJson::Object)
     {
      FString Path; Value->AsObject()->TryGetStringField(TEXT("path"), Path);
      if (!Path.IsEmpty()) Installed.Add(Path, Value);
     }
  }
 }
 for (const auto& File : PendingFiles)
 {
  const FString Path = FPaths::ConvertRelativePathToFull(IsBinaryPatchEntry(File) ? ResolveBaseTargetPath(File) :
   (File.Kind == ECloudDownloadKind::ExternFile ? GetLocalRoot() / File.TargetRelativePath : GetPakDir() / File.FileName));
  int64 Size = 0; FString Hash;
  if (!CloudUpdatePrivate::ComputeFileHash(Path, Size, Hash)) continue;
  TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
  Entry->SetStringField(TEXT("path"), Path);
  Entry->SetStringField(TEXT("hash"), Hash);
  Entry->SetNumberField(TEXT("size"), Size);
  Installed.Add(Path, MakeShared<FJsonValueObject>(Entry));
 }
 TArray<TSharedPtr<FJsonValue>> Manifest; Installed.GenerateValueArray(Manifest);
 Record->SetArrayField(TEXT("installedFiles"), Manifest);

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
 return LocalVersionRecordPath.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / GetPlatform() / TEXT("local_version.json") : LocalVersionRecordPath;
}
bool FCloudUpdateService::ReadLocalVersionRecord(TSharedPtr<FJsonObject>& OutRecord) const
{
 OutRecord.Reset();
 const auto Read = [&OutRecord](const FString& Path)
 {
  FString Text;
  return FFileHelper::LoadFileToString(Text, *Path)
   && FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), OutRecord) && OutRecord.IsValid();
 };
 if (Read(GetVersionRecordPath())) return MatchesResponsePlatform(OutRecord, GetPlatform());
 // An untagged shared record cannot identify its original platform or flavor.
 // Keep it untouched; only adopt a legacy record explicitly tagged for this build.
 if (LocalVersionRecordPath.IsEmpty() && Read(FPaths::ProjectSavedDir() / TEXT("CloudUpdate/local_version.json")))
  return OutRecord->HasField(TEXT("platform")) && MatchesResponsePlatform(OutRecord, GetPlatform());
 return false;
}
FString FCloudUpdateService::GetPendingResourceVersion() const
{
 FString Text, Pending, RecordedGame;
 TSharedPtr<FJsonObject> Json;
 if (ReadLocalVersionRecord(Json))
 {
  if (!MatchesResponsePlatform(Json, GetPlatform())) return TEXT("");
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
 Json->SetStringField(TEXT("platform"), GetPlatform());
 Json->SetStringField(TEXT("versionId"), LoadLocalVersion());
 Json->SetStringField(TEXT("resourceVersionId"), LoadLocalVersion());
 Json->SetStringField(TEXT("installedGameVersion"), GetLocalGameVersion());
 Json->SetStringField(TEXT("pendingResourceVersionId"), Version);
 TSharedPtr<FJsonObject> Previous;
 const TArray<TSharedPtr<FJsonValue>>* Installed = nullptr;
 if (ReadLocalVersionRecord(Previous) && Previous->TryGetArrayField(TEXT("installedFiles"), Installed))
  Json->SetArrayField(TEXT("installedFiles"), *Installed);

 TArray<TSharedPtr<FJsonValue>> Files;
 for (const FCloudDownloadFile& File : PendingFiles)
 {
  const FString Path = IsBinaryPatchEntry(File) ? ResolveBaseTargetPath(File) :
   (File.Kind == ECloudDownloadKind::ExternFile ? GetLocalRoot() / File.TargetRelativePath : GetPakDir() / File.FileName);
  TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
  Entry->SetStringField(TEXT("path"), FPaths::ConvertRelativePathToFull(Path));
  int64 Size = 0; FString Hash;
  const FString StagedPath = IFileManager::Get().FileExists(*(Path + TEXT(".pending"))) ? Path + TEXT(".pending") : Path;
  if (CloudUpdatePrivate::ComputeFileHash(StagedPath, Size, Hash))
  {
   Entry->SetStringField(TEXT("hash"), Hash);
   Entry->SetNumberField(TEXT("size"), Size);
  }
  else Entry->SetStringField(TEXT("hash"), IsBinaryPatchEntry(File) ? TEXT("") : File.Hash);
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
 if (!ReadLocalVersionRecord(Json)) return;
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
