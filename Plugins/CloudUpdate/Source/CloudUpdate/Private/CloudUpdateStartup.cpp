#include "CloudUpdate.h"
#include "CloudUpdateSettings.h"
#include "CloudUpdateUtil.h"
#include "Modules/ModuleManager.h"
#include "Engine/GameInstance.h"
#include "GameMapsSettings.h"
#include "GeneralProjectSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "HAL/PlatformFileManager.h"
#include "IPlatformFilePak.h"
#include "FlibPakHelper.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"


namespace
{
bool ValidateStartupManifest(const TMap<FString, TSharedPtr<FJsonObject>>& Manifest, TArray<FString>& OutPaks, bool bAllowStagedReplacement = false)
{
    OutPaks.Reset();
    for (const auto& Pair : Manifest)
    {
        const FString& Path = Pair.Key;
        FString Hash, ActualHash; int64 Size = -1, ActualSize = 0;
        Pair.Value->TryGetStringField(TEXT("hash"), Hash);
        if (!Pair.Value->TryGetNumberField(TEXT("size"), Size) || Size < 0 || Hash.IsEmpty() ||
            IFileManager::Get().FileSize(*Path) != Size || (!bAllowStagedReplacement && IFileManager::Get().FileExists(*(Path + TEXT(".pending"))))) return false;
        if (!CloudUpdatePrivate::ComputeFileHash(Path, ActualSize, ActualHash) || !ActualHash.Equals(Hash, ESearchCase::IgnoreCase)) return false;
        if (FPaths::GetExtension(Path).Equals(TEXT("pak"), ESearchCase::IgnoreCase)) OutPaks.Add(Path);
    }
    return !OutPaks.IsEmpty();
}
bool HasInstalledResources(const UCloudUpdateSettings& Settings, TArray<FString>& OutPaks)
{
    const FString Platform = CloudUpdatePrivate::GetRuntimeUpdatePlatform();
    FString Text, RecordedPlatform, GameVersion, ResourceVersion;
    TSharedPtr<FJsonObject> Record;
    const FString RecordPath = FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / Platform / TEXT("local_version.json");
    if (!FFileHelper::LoadFileToString(Text, *RecordPath) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Record) || !Record.IsValid()) return false;
    Record->TryGetStringField(TEXT("platform"), RecordedPlatform);
    Record->TryGetStringField(TEXT("installedGameVersion"), GameVersion);
    Record->TryGetStringField(TEXT("pendingResourceVersionId"), ResourceVersion);
    if (ResourceVersion.IsEmpty()) Record->TryGetStringField(TEXT("resourceVersionId"), ResourceVersion);
    const FString CurrentGame = Settings.GameVersionOverride.IsEmpty() ? GetDefault<UGeneralProjectSettings>()->ProjectVersion : Settings.GameVersionOverride;
    if (Platform.IsEmpty() || RecordedPlatform != Platform || GameVersion != CurrentGame || ResourceVersion.IsEmpty()) return false;
    TMap<FString, TSharedPtr<FJsonObject>> Manifest;
    for (const TCHAR* Key : {TEXT("installedFiles"), TEXT("pendingFiles")})
    {
        const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
        if (!Record->TryGetArrayField(Key, Files)) continue;
        for (const auto& Value : *Files)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object) return false;
            FString Path; Value->AsObject()->TryGetStringField(TEXT("path"), Path);
            if (Path.IsEmpty()) return false;
            Manifest.Add(Path, Value->AsObject());
        }
    }
    if (Manifest.IsEmpty()) return false; // Legacy version number alone is not installation evidence.
    if (ValidateStartupManifest(Manifest, OutPaks)) return true;
    // A failed/incomplete newer download must not block an intact offline base.
    Manifest.Reset();
    const TArray<TSharedPtr<FJsonValue>>* Active = nullptr;
    FString ActiveVersion;
    Record->TryGetStringField(TEXT("resourceVersionId"), ActiveVersion);
    if (ActiveVersion.IsEmpty() || !Record->TryGetArrayField(TEXT("installedFiles"), Active)) return false;
    for (const auto& Value : *Active)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object) return false;
        FString Path; Value->AsObject()->TryGetStringField(TEXT("path"), Path);
        if (Path.IsEmpty()) return false;
        Manifest.Add(Path, Value->AsObject());
    }
    return ValidateStartupManifest(Manifest, OutPaks, true);
}
}

void CloudUpdatePrivate::SelectStartupGameInstance()
{
#if !WITH_EDITOR
        if (IsRunningCommandlet() || IsRunningDedicatedServer()) return;
        const auto* Settings = UCloudUpdateSettings::Get();
        if (!Settings->bSelectGameInstanceFromResources) return;
        auto* Maps = GetMutableDefault<UGameMapsSettings>();
        UClass* Bootstrap = FSoftClassPath(Settings->BootstrapGameInstanceClass).TryLoadClass<UGameInstance>();
        if (!Bootstrap) Bootstrap = UGameInstance::StaticClass();
        Maps->GameInstanceClass = FSoftClassPath(Bootstrap);
        TArray<FString> Paks;
        if (!HasInstalledResources(*Settings, Paks))
        {
            UE_LOG(LogCloudUpdate, Display, TEXT("Startup GameInstance: bootstrap (resources missing, incompatible or incomplete)"));
            return;
        }
        // Engine subsystem initialization is AFTER the initial container mount and BEFORE GameInstance creation. Never exchange
        // .pending files here. Mount only containers not already mounted, including IoStore sidecars.
        auto* PakFile = static_cast<FPakPlatformFile*>(FPlatformFileManager::Get().FindPlatformFile(TEXT("PakFile")));
        if (!PakFile) return;
        TArray<FString> Mounted; PakFile->GetMountedPakFilenames(Mounted);
        for (const FString& Pak : Paks)
        {
            const bool AlreadyMounted = Mounted.ContainsByPredicate([&Pak](const FString& Item)
                { return FPaths::IsSamePath(FPaths::ConvertRelativePathToFull(Item), FPaths::ConvertRelativePathToFull(Pak)); });
            if (!AlreadyMounted && !PakFile->Mount(*Pak, UFlibPakHelper::GetPakOrderByPakPath(Pak), nullptr))
            {
                UE_LOG(LogCloudUpdate, Error, TEXT("Startup resource mount failed: %s; keeping bootstrap"), *Pak);
                return;
            }
        }
        if (UClass* GameClass = FSoftClassPath(Settings->ResourceGameInstanceClass).TryLoadClass<UGameInstance>())
        {
            Maps->GameInstanceClass = FSoftClassPath(GameClass);
            UE_LOG(LogCloudUpdate, Display, TEXT("Startup GameInstance: %s (verified resources)"), *GameClass->GetPathName());
        }
        else UE_LOG(LogCloudUpdate, Error, TEXT("Resource GameInstance unavailable; keeping bootstrap"));
#endif
}


#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FCloudStartupManifestTest, "CloudUpdate.Startup.LocalResourceManifest", (EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext) | EAutomationTestFlags::EngineFilter)
bool FCloudStartupManifestTest::RunTest(const FString& Parameters)
{
    const FString Root = FPaths::ProjectSavedDir() / TEXT("Automation/CloudStartup") / FGuid::NewGuid().ToString();
    IFileManager::Get().MakeDirectory(*Root, true);
    const FString Path = Root / TEXT("base.pak");
    FFileHelper::SaveStringToFile(TEXT("offline base"), *Path);
    int64 Size = 0; FString Hash;
    CloudUpdatePrivate::ComputeFileHash(Path, Size, Hash);
    auto Entry = MakeShared<FJsonObject>();
    Entry->SetStringField(TEXT("hash"), Hash); Entry->SetNumberField(TEXT("size"), Size);
    TMap<FString, TSharedPtr<FJsonObject>> Manifest; Manifest.Add(Path, Entry);
    TArray<FString> Paks;
    TestTrue(TEXT("An installed local base is accepted without any server request"), ValidateStartupManifest(Manifest, Paks));
    Entry->SetNumberField(TEXT("size"), Size + 1);
    TestFalse(TEXT("Truncated/wrong sized file rejected"), ValidateStartupManifest(Manifest, Paks));
    Entry->SetNumberField(TEXT("size"), Size); Entry->SetStringField(TEXT("hash"), TEXT("bad"));
    TestFalse(TEXT("Corrupted file rejected"), ValidateStartupManifest(Manifest, Paks));
    Entry->SetStringField(TEXT("hash"), Hash);
    FFileHelper::SaveStringToFile(TEXT("staged"), *(Path + TEXT(".pending")));
    TestFalse(TEXT("Unapplied replacement rejected"), ValidateStartupManifest(Manifest, Paks));
    TestTrue(TEXT("Intact active base stays playable while a replacement is staged"), ValidateStartupManifest(Manifest, Paks, true));
    IFileManager::Get().Delete(*(Path + TEXT(".pending")));
    IFileManager::Get().Delete(*Path);
    TestFalse(TEXT("Missing package rejected"), ValidateStartupManifest(Manifest, Paks));
    Manifest.Reset();
    TestFalse(TEXT("Version number without installed files rejected"), ValidateStartupManifest(Manifest, Paks));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
#endif

