#include "CloudUpdate.h"
#include "CloudUpdateSettings.h"
#include "CloudUpdateUtil.h"
#include "Modules/ModuleManager.h"
#include "Engine/GameInstance.h"
#include "GameMapsSettings.h"
#include "GeneralProjectSettings.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/DateTime.h"
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

    // 容器集完整性：只要资源记录里出现 .utoc 或 .ucas，就必须 .pak/.utoc/.ucas 三者齐全。
    // 引擎把 .pak 当作 IoStore 挂载入口，用 ChangeExtension(.pak -> .utoc) 去找容器；缺 .utoc 时
    // 整个 .pak 都会被丢弃不挂载（GPak_AllowMissingUToc=false，见 IPlatformFilePak.cpp:6186-6203）。
    // 只逐个校验"清单里列出的文件"的大小/哈希是发现不了这种残缺的——记录可以完全自洽，
    // 引擎却挂载不上，于是切到资源 GameInstance 后在加载资源时崩溃。
    // 检出残缺就返回 false，让上层回退 bootstrap 引导包（保留更新/修复入口）。
    TSet<FString> ManifestPaths;
    for (const auto& Pair : Manifest) ManifestPaths.Add(Pair.Key);
    for (const auto& Pair : Manifest)
    {
        const FString Extension = FPaths::GetExtension(Pair.Key).ToLower();
        if (Extension != TEXT("utoc") && Extension != TEXT("ucas")) continue;
        const FString PakPath = CloudUpdatePrivate::GetContainerSetPakPath(Pair.Key);
        const FString UtocPath = FPaths::ChangeExtension(Pair.Key, TEXT(".utoc"));
        const FString UcasPath = FPaths::ChangeExtension(Pair.Key, TEXT(".ucas"));
        if (!ManifestPaths.Contains(PakPath) || !ManifestPaths.Contains(UtocPath) || !ManifestPaths.Contains(UcasPath))
        {
            // 这是"记录与磁盘不一致"的数据状态，调用方会安全回退 bootstrap 引导包，
            // 因此用 Warning（Error 级日志会被自动化测试框架当成测试失败）。
            UE_LOG(LogCloudUpdate, Warning,
                TEXT("资源记录中的 IoStore 容器集不完整，拒绝启用（缺 .pak/.utoc/.ucas 之一）：%s"), *PakPath);
            return false;
        }
    }
    // 所有记录文件均通过校验即认为资源可用。不再强制要求必须存在 .pak：
    // OutPaks 只用于"补挂载还没挂上的 pak"，容器会随同 basename 的 .pak 一起被引擎挂载。
    // 但"只有版本号、没有任何文件记录"不算安装证据。
    return !Manifest.IsEmpty();
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

namespace
{
/** 本次进程是否因安全模式回退到 bootstrap 引导包 */
bool bStartupSafeModeTriggered = false;

FString StartupAttemptPath()
{
    return FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / CloudUpdatePrivate::GetRuntimeUpdatePlatform() / TEXT("startup_attempt.json");
}

/** 记录里当前生效的资源版本（优先 pending，其次已安装），用作"连续启动失败"的判定键。 */
FString EffectiveRecordedResourceVersion()
{
    FString Text; TSharedPtr<FJsonObject> Record;
    const FString RecordPath = FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / CloudUpdatePrivate::GetRuntimeUpdatePlatform() / TEXT("local_version.json");
    if (!FFileHelper::LoadFileToString(Text, *RecordPath)
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Record) || !Record.IsValid()) return FString();
    FString Version; Record->TryGetStringField(TEXT("pendingResourceVersionId"), Version);
    if (Version.IsEmpty()) Record->TryGetStringField(TEXT("resourceVersionId"), Version);
    return Version;
}
}

int32 CloudUpdatePrivate::ReadStartupAttempt(const FString& InVersion)
{
    FString Text; TSharedPtr<FJsonObject> Record;
    if (InVersion.IsEmpty()
        || !FFileHelper::LoadFileToString(Text, *StartupAttemptPath())
        || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Record) || !Record.IsValid()) return 0;
    FString Version; int32 Attempt = 0;
    Record->TryGetStringField(TEXT("versionId"), Version);
    Record->TryGetNumberField(TEXT("attempt"), Attempt);
    return Version == InVersion ? FMath::Max(0, Attempt) : 0;
}

void CloudUpdatePrivate::WriteStartupAttempt(const FString& InVersion, int32 InAttempt)
{
    TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
    Record->SetStringField(TEXT("versionId"), InVersion);
    Record->SetNumberField(TEXT("attempt"), InAttempt);
    Record->SetStringField(TEXT("timestamp"), FDateTime::Now().ToString());
    FString Text; FJsonSerializer::Serialize(Record, TJsonWriterFactory<>::Create(&Text));
    const FString Path = StartupAttemptPath();
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void CloudUpdatePrivate::ClearStartupAttempt()
{
    IFileManager::Get().Delete(*StartupAttemptPath(), false, true);
}

bool CloudUpdatePrivate::WasSafeModeTriggered()
{
    return bStartupSafeModeTriggered;
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
        // 安全模式：资源实例连续启动失败（崩在稳定窗口内，记录未被清除）达到阈值时，
        // 本次强制使用 bootstrap 引导包 —— 保证更新/修复入口一定可见，而不是继续崩在资源加载里。
        const FString TrackedVersion = EffectiveRecordedResourceVersion();
        const int32 Threshold = FMath::Max(1, Settings->SafeModeFailureThreshold);
        const int32 PreviousAttempts = CloudUpdatePrivate::ReadStartupAttempt(TrackedVersion);
        if (!TrackedVersion.IsEmpty() && PreviousAttempts >= Threshold)
        {
            bStartupSafeModeTriggered = true;
            CloudUpdatePrivate::WriteStartupAttempt(TrackedVersion, Threshold); // 保持计数，等 UI 显式清除
            UE_LOG(LogCloudUpdate, Error, TEXT("Startup GameInstance: bootstrap (safe mode: %s failed to start %d time(s))"), *TrackedVersion, PreviousAttempts);
            return;
        }
        // 记一次尝试：撑过安全窗口后由 UCloudUpdateSubsystem 的 ticker 清除。
        if (!TrackedVersion.IsEmpty()) CloudUpdatePrivate::WriteStartupAttempt(TrackedVersion, PreviousAttempts + 1);

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

    // 容器集完整性：有 .utoc 没有 .ucas 时必须拒绝 —— 引擎按 .utoc 读容器，
    // 缺配套文件会读到不一致数据；而"每个文件自身大小/哈希都对"是查不出这种残缺的。
    {
        const FString SetPak = Root / TEXT("set.pak");
        const FString SetUtoc = Root / TEXT("set.utoc");
        const FString SetUcas = Root / TEXT("set.ucas");
        const auto MakeEntry = [](const FString& InFile)
        {
            int64 InSize = 0; FString InHash;
            CloudUpdatePrivate::ComputeFileHash(InFile, InSize, InHash);
            auto InEntry = MakeShared<FJsonObject>();
            InEntry->SetNumberField(TEXT("size"), InSize);
            InEntry->SetStringField(TEXT("hash"), InHash);
            return InEntry;
        };
        FFileHelper::SaveStringToFile(TEXT("pak"), *SetPak);
        FFileHelper::SaveStringToFile(TEXT("utoc"), *SetUtoc);
        TMap<FString, TSharedPtr<FJsonObject>> SetManifest;
        SetManifest.Add(SetPak, MakeEntry(SetPak));
        SetManifest.Add(SetUtoc, MakeEntry(SetUtoc));
        TArray<FString> SetPaks;
        TestFalse(TEXT("Incomplete IoStore container set (.utoc without .ucas) rejected"), ValidateStartupManifest(SetManifest, SetPaks));

        FFileHelper::SaveStringToFile(TEXT("ucas"), *SetUcas);
        SetManifest.Add(SetUcas, MakeEntry(SetUcas));
        TestTrue(TEXT("Complete IoStore container set (.pak/.utoc/.ucas) accepted"), ValidateStartupManifest(SetManifest, SetPaks));
        IFileManager::Get().Delete(*SetPak); IFileManager::Get().Delete(*SetUtoc); IFileManager::Get().Delete(*SetUcas);
    }

    // 安全模式计数：读/写/清 + 换版本后计数归零。测试结束后恢复原有标记文件。
    {
        const FString AttemptPath = FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / CloudUpdatePrivate::GetRuntimeUpdatePlatform() / TEXT("startup_attempt.json");
        FString Backup; const bool bHadBackup = FFileHelper::LoadFileToString(Backup, *AttemptPath);
        TestEqual(TEXT("No recorded attempt for an unknown version"), CloudUpdatePrivate::ReadStartupAttempt(TEXT("v-test")), 0);
        CloudUpdatePrivate::WriteStartupAttempt(TEXT("v-test"), 2);
        TestEqual(TEXT("Recorded attempt is read back"), CloudUpdatePrivate::ReadStartupAttempt(TEXT("v-test")), 2);
        TestEqual(TEXT("Attempt counter resets for another version"), CloudUpdatePrivate::ReadStartupAttempt(TEXT("v-other")), 0);
        CloudUpdatePrivate::ClearStartupAttempt();
        TestEqual(TEXT("Cleared attempt reads back as zero"), CloudUpdatePrivate::ReadStartupAttempt(TEXT("v-test")), 0);
        if (bHadBackup) FFileHelper::SaveStringToFile(Backup, *AttemptPath);
    }
    TestFalse(TEXT("Missing package rejected"), ValidateStartupManifest(Manifest, Paks));
    Manifest.Reset();
    TestFalse(TEXT("Version number without installed files rejected"), ValidateStartupManifest(Manifest, Paks));
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
#endif

