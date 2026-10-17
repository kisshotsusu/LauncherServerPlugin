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
bool ValidateStartupManifestDetailed(const TMap<FString, TSharedPtr<FJsonObject>>& Manifest, TArray<FString>& OutPaks,
 FString& OutReason, bool bAllowStagedReplacement = false)
{
    OutPaks.Reset();
    OutReason.Reset();
    for (const auto& Pair : Manifest)
    {
        const FString& Path = Pair.Key;
        FString Hash, ActualHash; int64 Size = -1, ActualSize = 0;
        Pair.Value->TryGetStringField(TEXT("hash"), Hash);
        const int64 ActualFileSize = IFileManager::Get().FileSize(*Path);
        if (!Pair.Value->TryGetNumberField(TEXT("size"), Size) || Size < 0 || Hash.IsEmpty())
        {
            OutReason = FString::Printf(TEXT("资源记录条目缺少 size/hash：%s"), *Path);
            return false;
        }
        if (ActualFileSize != Size)
        {
            OutReason = FString::Printf(TEXT("文件大小不符：%s（记录 %lld，实际 %lld）"), *Path, Size, ActualFileSize);
            return false;
        }
        if (!bAllowStagedReplacement && IFileManager::Get().FileExists(*(Path + TEXT(".pending"))))
        {
            OutReason = FString::Printf(TEXT("仍有未交换的暂存文件：%s.pending（启动前交换没有执行）"), *Path);
            return false;
        }
        if (!CloudUpdatePrivate::ComputeFileHash(Path, ActualSize, ActualHash) || !ActualHash.Equals(Hash, ESearchCase::IgnoreCase))
        {
            OutReason = FString::Printf(TEXT("文件哈希不符：%s（记录 %s，实际 %s）"), *Path, *Hash, *ActualHash);
            return false;
        }
        if (FPaths::GetExtension(Path).Equals(TEXT("pak"), ESearchCase::IgnoreCase)) OutPaks.Add(Path);
    }

    // 容器集完整性：非 global 的 .utoc/.ucas 必须与同名 .pak/.utoc/.ucas 成组出现
    // （引擎用 .pak 作为挂载入口，缺 .utoc 时整个 .pak 会被丢弃）。
    // 例外：global.utoc/global.ucas 是 IoStore 的全局脚本/数据容器，引擎不通过 .pak 挂载，
    // 不存在也不需要 global.pak。之前把 global 也按"三件套"校验，会让已安装的基础包
    // 永远被判为"容器集不完整"，GameInstance 永远回退 bootstrap（真机日志可证）。
    // IoStore 允许把一个容器的数据切成多个 .ucas 分片（..._s0.ucas 等），按去掉 _sN 的容器名归组。
    const auto ContainerStem = [](const FString& InPath)
    {
        FString Stem = FPaths::ChangeExtension(InPath, TEXT(""));
        const int32 UnderscoreIndex = Stem.Find(TEXT("_s"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
        if (UnderscoreIndex != INDEX_NONE)
        {
            const FString Suffix = Stem.Mid(UnderscoreIndex + 2);
            if (Suffix.IsNumeric()) Stem = Stem.Left(UnderscoreIndex);
        }
        return Stem;
    };
    TSet<FString> PakStems, UtocStems, UcasStems;
    for (const auto& Pair : Manifest)
    {
        const FString Extension = FPaths::GetExtension(Pair.Key).ToLower();
        if (Extension == TEXT("pak")) PakStems.Add(ContainerStem(Pair.Key));
        else if (Extension == TEXT("utoc")) UtocStems.Add(ContainerStem(Pair.Key));
        else if (Extension == TEXT("ucas")) UcasStems.Add(ContainerStem(Pair.Key));
    }
    for (const auto& Pair : Manifest)
    {
        const FString Extension = FPaths::GetExtension(Pair.Key).ToLower();
        if (Extension != TEXT("utoc") && Extension != TEXT("ucas")) continue;
        const FString Stem = ContainerStem(Pair.Key);
        const bool bGlobalContainer = FPaths::GetCleanFilename(Stem).Equals(TEXT("global"), ESearchCase::IgnoreCase);
        if (bGlobalContainer)
        {
            // global.ucas 必须有 global.utoc；global.utoc 单独存在是合法的（全局索引可以没有数据分区）。
            if (Extension == TEXT("ucas") && !UtocStems.Contains(Stem))
            {
                UE_LOG(LogCloudUpdate, Warning,
                    TEXT("资源记录中的全局 IoStore 容器缺少索引（缺 %s.utoc），拒绝启用"), *Stem);
                OutReason = FString::Printf(TEXT("全局 IoStore 容器缺少索引：%s.utoc"), *Stem);
                return false;
            }
            continue;
        }
        if (!PakStems.Contains(Stem) || !UtocStems.Contains(Stem) || !UcasStems.Contains(Stem))
        {
            UE_LOG(LogCloudUpdate, Warning,
                TEXT("资源记录中的 IoStore 容器集不完整，拒绝启用（缺 .pak/.utoc/.ucas 之一）：%s"), *Stem);
            OutReason = FString::Printf(TEXT("IoStore 容器集不完整：%s"), *Stem);
            return false;
        }
    }
    if (Manifest.IsEmpty())
    {
        OutReason = TEXT("资源记录里没有任何文件");
        return false;
    }
    return true;
}

bool ValidateStartupManifest(const TMap<FString, TSharedPtr<FJsonObject>>& Manifest, TArray<FString>& OutPaks, bool bAllowStagedReplacement = false)
{
    FString UnusedReason;
    return ValidateStartupManifestDetailed(Manifest, OutPaks, UnusedReason, bAllowStagedReplacement);
}


bool HasInstalledResources(const UCloudUpdateSettings& Settings, TArray<FString>& OutPaks, FString& OutReason)
{
    OutReason.Reset();
    const FString Platform = CloudUpdatePrivate::GetRuntimeUpdatePlatform();
    FString Text, RecordedPlatform, GameVersion, ResourceVersion;
    TSharedPtr<FJsonObject> Record;
    const FString RecordPath = FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / Platform / TEXT("local_version.json");
    if (!FFileHelper::LoadFileToString(Text, *RecordPath) ||
        !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Record) || !Record.IsValid())
    {
        OutReason = FString::Printf(TEXT("没有可用的资源记录：%s"), *RecordPath);
        return false;
    }
    Record->TryGetStringField(TEXT("platform"), RecordedPlatform);
    Record->TryGetStringField(TEXT("installedGameVersion"), GameVersion);
    Record->TryGetStringField(TEXT("pendingResourceVersionId"), ResourceVersion);
    if (ResourceVersion.IsEmpty()) Record->TryGetStringField(TEXT("resourceVersionId"), ResourceVersion);
    const FString CurrentGame = Settings.GameVersionOverride.IsEmpty() ? GetDefault<UGeneralProjectSettings>()->ProjectVersion : Settings.GameVersionOverride;
    if (Platform.IsEmpty())
    {
        OutReason = TEXT("无法识别运行时平台");
        return false;
    }
    if (RecordedPlatform != Platform)
    {
        OutReason = FString::Printf(TEXT("资源记录平台 %s 与当前平台 %s 不一致（%s）"), *RecordedPlatform, *Platform, *RecordPath);
        return false;
    }
    if (GameVersion != CurrentGame)
    {
        OutReason = FString::Printf(TEXT("资源记录的游戏版本 %s 与当前包版本 %s 不一致；换包后需重新下载资源"), *GameVersion, *CurrentGame);
        return false;
    }
    if (ResourceVersion.IsEmpty())
    {
        OutReason = TEXT("资源记录没有版本号");
        return false;
    }
    TMap<FString, TSharedPtr<FJsonObject>> Manifest;
    for (const TCHAR* Key : {TEXT("installedFiles"), TEXT("pendingFiles")})
    {
        const TArray<TSharedPtr<FJsonValue>>* Files = nullptr;
        if (!Record->TryGetArrayField(Key, Files)) continue;
        for (const auto& Value : *Files)
        {
            if (!Value.IsValid() || Value->Type != EJson::Object)
            {
                OutReason = FString::Printf(TEXT("资源记录 %s 条目格式非法"), Key);
                return false;
            }
            FString Path; Value->AsObject()->TryGetStringField(TEXT("path"), Path);
            if (Path.IsEmpty())
            {
                OutReason = FString::Printf(TEXT("资源记录 %s 条目缺少 path"), Key);
                return false;
            }
            Manifest.Add(Path, Value->AsObject());
        }
    }
    if (Manifest.IsEmpty())
    {
        OutReason = TEXT("资源记录里没有任何文件（只有版本号不算安装证据）");
        return false;
    }
    if (ValidateStartupManifestDetailed(Manifest, OutPaks, OutReason)) return true;
    // A failed/incomplete newer download must not block an intact offline base.
    const FString NewerDetail = OutReason;
    Manifest.Reset();
    const TArray<TSharedPtr<FJsonValue>>* Active = nullptr;
    FString ActiveVersion;
    Record->TryGetStringField(TEXT("resourceVersionId"), ActiveVersion);
    if (ActiveVersion.IsEmpty() || !Record->TryGetArrayField(TEXT("installedFiles"), Active))
    {
        OutReason = FString::Printf(TEXT("资源校验失败：%s"), *NewerDetail);
        return false;
    }
    for (const auto& Value : *Active)
    {
        if (!Value.IsValid() || Value->Type != EJson::Object) continue;
        FString Path; Value->AsObject()->TryGetStringField(TEXT("path"), Path);
        if (!Path.IsEmpty()) Manifest.Add(Path, Value->AsObject());
    }
    FString FallbackDetail;
    if (ValidateStartupManifestDetailed(Manifest, OutPaks, FallbackDetail, true))
    {
        OutReason = TEXT("使用当前已生效的基础文件（新的替换尚未交换）");
        return true;
    }
    OutReason = FString::Printf(TEXT("资源校验失败：%s"), *NewerDetail);
    return false;
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

namespace
{
/** 本次进程的启动 GameInstance 决策摘要（供日志/蓝图诊断查询）。 */
FString GStartupGameInstanceDecision;

/**
 * 解析 GameInstance 类路径。蓝图生成类必须带 _C 后缀；配置里若写成资产路径
 * （/Game/BiNanSuo/BNS_GI），UE 的 LoadClass 找不到生成类，这里自动补 _C 或按蓝图资产取 GeneratedClass。
 */
UClass* ResolveGameInstanceClass(const FString& InConfiguredPath, FString& OutUsedPath)
{
    OutUsedPath = InConfiguredPath;
    if (InConfiguredPath.IsEmpty()) return nullptr;
    const auto TryPath = [](const FString& Path) -> UClass*
    {
        if (Path.IsEmpty()) return nullptr;
        return FSoftClassPath(Path).TryLoadClass<UGameInstance>();
    };
    if (UClass* Loaded = TryPath(InConfiguredPath)) return Loaded;
    if (InConfiguredPath.StartsWith(TEXT("/Game/")) && !InConfiguredPath.EndsWith(TEXT("_C")))
    {
        const FString WithClassSuffix = InConfiguredPath + TEXT("_C");
        if (UClass* Loaded = TryPath(WithClassSuffix))
        {
            OutUsedPath = WithClassSuffix;
            return Loaded;
        }
    }
    if (UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *InConfiguredPath))
    {
        if (Blueprint->GeneratedClass && Blueprint->GeneratedClass->IsChildOf(UGameInstance::StaticClass()))
        {
            OutUsedPath = Blueprint->GeneratedClass->GetPathName();
            return Blueprint->GeneratedClass;
        }
    }
    return nullptr;
}

/**
 * 把"项目设置里直接选中的 GameInstance 类"（软类引用）解析成 UClass。
 * 正常情况同步加载选中的类；兼容旧配置里不带 _C 的蓝图资产路径（交给 ResolveGameInstanceClass 兜底）。
 */
UClass* ResolveSelectedGameInstanceClass(const TSoftClassPtr<UGameInstance>& InSelectedClass, FString& OutUsedPath)
{
    OutUsedPath = InSelectedClass.ToString();
    if (OutUsedPath.IsEmpty()) return nullptr;
    if (UClass* Loaded = InSelectedClass.LoadSynchronous())
    {
        if (Loaded->IsChildOf(UGameInstance::StaticClass()))
        {
            OutUsedPath = Loaded->GetPathName();
            return Loaded;
        }
    }
    const FString FallbackPath = OutUsedPath;
    return ResolveGameInstanceClass(FallbackPath, OutUsedPath);
}

/** 记录启动决策到日志 + Saved/CloudUpdate/<平台>/startup_gameinstance.json（真机可用 adb pull 查看）。 */
void RecordStartupGameInstanceDecision(const TCHAR* Mode, const FString& ConfiguredPath, const FString& EffectivePath, const FString& Reason)
{
    const FString Platform = CloudUpdatePrivate::GetRuntimeUpdatePlatform();
    GStartupGameInstanceDecision = FString::Printf(TEXT("%s | 配置=%s | 生效=%s | %s"),
        Mode, ConfiguredPath.IsEmpty() ? TEXT("<空>") : *ConfiguredPath,
        EffectivePath.IsEmpty() ? TEXT("<空>") : *EffectivePath, *Reason);
    UE_LOG(LogCloudUpdate, Display, TEXT("Startup GameInstance: %s"), *GStartupGameInstanceDecision);

    TSharedRef<FJsonObject> Record = MakeShared<FJsonObject>();
    Record->SetStringField(TEXT("mode"), Mode);
    Record->SetStringField(TEXT("platform"), Platform);
    Record->SetStringField(TEXT("configuredClass"), ConfiguredPath);
    Record->SetStringField(TEXT("effectiveClass"), EffectivePath);
    Record->SetStringField(TEXT("reason"), Reason);
    Record->SetStringField(TEXT("gameVersion"), GetDefault<UGeneralProjectSettings>()->ProjectVersion);
    Record->SetStringField(TEXT("timestamp"), FDateTime::Now().ToString());
    FString Text;
    FJsonSerializer::Serialize(Record, TJsonWriterFactory<>::Create(&Text));
    const FString Path = FPaths::ProjectSavedDir() / TEXT("CloudUpdate") / Platform / TEXT("startup_gameinstance.json");
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
    FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}
}

FString CloudUpdatePrivate::GetStartupGameInstanceDecision()
{
    return GStartupGameInstanceDecision;
}

void CloudUpdatePrivate::SelectStartupGameInstance()
{
#if !WITH_EDITOR
        if (IsRunningCommandlet() || IsRunningDedicatedServer()) return;
        const auto* Settings = UCloudUpdateSettings::Get();
        if (!Settings) return;
        if (!Settings->bSelectGameInstanceFromResources)
        {
            RecordStartupGameInstanceDecision(TEXT("disabled"), TEXT(""),
                GetDefault<UGameMapsSettings>()->GameInstanceClass.ToString(),
                TEXT("bSelectGameInstanceFromResources=False（使用项目/命令行配置的 GameInstance）"));
            return;
        }
        auto* Maps = GetMutableDefault<UGameMapsSettings>();
        FString BootstrapUsedPath;
        UClass* Bootstrap = ResolveSelectedGameInstanceClass(Settings->BootstrapGameInstanceClass, BootstrapUsedPath);
        if (!Bootstrap)
        {
            Bootstrap = UGameInstance::StaticClass();
            BootstrapUsedPath = Bootstrap->GetPathName();
            UE_LOG(LogCloudUpdate, Warning, TEXT("BootstrapGameInstanceClass 无法解析（%s），退回 UGameInstance"), *Settings->BootstrapGameInstanceClass.ToString());
        }
        Maps->GameInstanceClass = FSoftClassPath(BootstrapUsedPath);
        TArray<FString> Paks;
        FString ResourceReason;
        if (!HasInstalledResources(*Settings, Paks, ResourceReason))
        {
            RecordStartupGameInstanceDecision(TEXT("bootstrap"), Settings->ResourceGameInstanceClass.ToString(), BootstrapUsedPath, ResourceReason);
            return;
        }
        // Engine subsystem initialization is AFTER the initial container mount and BEFORE GameInstance creation. Never exchange
        // .pending files here. Mount only containers not already mounted, including IoStore sidecars.
        auto* PakFile = static_cast<FPakPlatformFile*>(FPlatformFileManager::Get().FindPlatformFile(TEXT("PakFile")));
        if (!PakFile)
        {
            RecordStartupGameInstanceDecision(TEXT("bootstrap"), Settings->ResourceGameInstanceClass.ToString(), BootstrapUsedPath,
                TEXT("PakFile 平台文件层不可用，无法挂载资源容器"));
            return;
        }
        TArray<FString> Mounted; PakFile->GetMountedPakFilenames(Mounted);
        for (const FString& Pak : Paks)
        {
            const bool AlreadyMounted = Mounted.ContainsByPredicate([&Pak](const FString& Item)
                { return FPaths::IsSamePath(FPaths::ConvertRelativePathToFull(Item), FPaths::ConvertRelativePathToFull(Pak)); });
            if (!AlreadyMounted && !PakFile->Mount(*Pak, UFlibPakHelper::GetPakOrderByPakPath(Pak), nullptr))
            {
                RecordStartupGameInstanceDecision(TEXT("bootstrap"), Settings->ResourceGameInstanceClass.ToString(), BootstrapUsedPath,
                    FString::Printf(TEXT("资源 pak 挂载失败：%s"), *Pak));
                return;
            }
        }
        // 安全模式：资源实例连续启动失败（崩在稳定窗口内，记录未被清除）达到阈值时，
        // 本次强制使用 bootstrap 引导包 —— 保证更新/修复入口一定可见。
        const FString TrackedVersion = EffectiveRecordedResourceVersion();
        const int32 Threshold = FMath::Max(1, Settings->SafeModeFailureThreshold);
        const int32 PreviousAttempts = CloudUpdatePrivate::ReadStartupAttempt(TrackedVersion);
        if (!TrackedVersion.IsEmpty() && PreviousAttempts >= Threshold)
        {
            bStartupSafeModeTriggered = true;
            CloudUpdatePrivate::WriteStartupAttempt(TrackedVersion, Threshold); // 保持计数，等 UI 显式清除
            RecordStartupGameInstanceDecision(TEXT("bootstrap"), Settings->ResourceGameInstanceClass.ToString(), BootstrapUsedPath,
                FString::Printf(TEXT("安全模式：资源版本 %s 连续启动失败 %d 次"), *TrackedVersion, PreviousAttempts));
            return;
        }
        // 记一次尝试：撑过安全窗口后由 UCloudUpdateSubsystem 的 ticker 清除。
        if (!TrackedVersion.IsEmpty()) CloudUpdatePrivate::WriteStartupAttempt(TrackedVersion, PreviousAttempts + 1);

        FString ResourceUsedPath;
        UClass* GameClass = ResolveSelectedGameInstanceClass(Settings->ResourceGameInstanceClass, ResourceUsedPath);
        if (!GameClass)
        {
            RecordStartupGameInstanceDecision(TEXT("bootstrap"), Settings->ResourceGameInstanceClass.ToString(), BootstrapUsedPath,
                FString::Printf(TEXT("资源 GameInstance 类加载失败（%s）；请确认该值指向 _C 生成类，且该资源在已安装的基础包里"),
                    *Settings->ResourceGameInstanceClass.ToString()));
            return;
        }
        Maps->GameInstanceClass = FSoftClassPath(ResourceUsedPath);
        RecordStartupGameInstanceDecision(TEXT("resource"), Settings->ResourceGameInstanceClass.ToString(), ResourceUsedPath,
            FString::Printf(TEXT("资源校验通过（版本 %s）"), *TrackedVersion));
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

        // global.utoc / global.ucas 是 IoStore 全局容器，引擎不通过 .pak 挂载（不存在 global.pak）；
        // 真机上曾因为要求 global.pak 而永远判为"容器集不完整"，GameInstance 永远回退 bootstrap。
        {
            const FString GlobalUtoc = Root / TEXT("global.utoc");
            const FString GlobalUcas = Root / TEXT("global.ucas");
            FFileHelper::SaveStringToFile(TEXT("gu"), *GlobalUtoc);
            FFileHelper::SaveStringToFile(TEXT("ga"), *GlobalUcas);
            TMap<FString, TSharedPtr<FJsonObject>> GlobalManifest;
            GlobalManifest.Add(GlobalUtoc, MakeEntry(GlobalUtoc));
            GlobalManifest.Add(GlobalUcas, MakeEntry(GlobalUcas));
            TArray<FString> GlobalPaks;
            TestTrue(TEXT("Global IoStore container requires no global.pak"),
                ValidateStartupManifest(GlobalManifest, GlobalPaks));
            GlobalManifest.Remove(GlobalUtoc);
            TestFalse(TEXT("Global .ucas still requires its .utoc"),
                ValidateStartupManifest(GlobalManifest, GlobalPaks));
            IFileManager::Get().Delete(*GlobalUtoc); IFileManager::Get().Delete(*GlobalUcas);
        }
        // 分片 .ucas（..._s0.ucas）按容器名归组，不要求存在 ..._s0.pak。
        {
            const FString SplitPak = Root / TEXT("split.pak");
            const FString SplitUtoc = Root / TEXT("split.utoc");
            const FString SplitUcas = Root / TEXT("split_s0.ucas");
            FFileHelper::SaveStringToFile(TEXT("sp"), *SplitPak);
            FFileHelper::SaveStringToFile(TEXT("su"), *SplitUtoc);
            FFileHelper::SaveStringToFile(TEXT("sc"), *SplitUcas);
            TMap<FString, TSharedPtr<FJsonObject>> SplitManifest;
            SplitManifest.Add(SplitPak, MakeEntry(SplitPak));
            SplitManifest.Add(SplitUtoc, MakeEntry(SplitUtoc));
            SplitManifest.Add(SplitUcas, MakeEntry(SplitUcas));
            TArray<FString> SplitPaks;
            TestTrue(TEXT("Split .ucas shards group under the container stem"),
                ValidateStartupManifest(SplitManifest, SplitPaks));
            IFileManager::Get().Delete(*SplitPak); IFileManager::Get().Delete(*SplitUtoc); IFileManager::Get().Delete(*SplitUcas);
        }
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

    // GameInstance 类路径解析：蓝图生成类必须带 _C；配置里写成资产路径（无 _C）时也要能解析出来，
    // 否则引擎会报 "Unable to load GameInstance Class ... Falling back to generic UGameInstance"。
    {
        FString UsedPath;
        TestNotNull(TEXT("Native GameInstance class resolves"),
            ResolveGameInstanceClass(TEXT("/Script/Engine.GameInstance"), UsedPath));
        UClass* BlueprintClass = ResolveGameInstanceClass(TEXT("/Game/BiNanSuo/BNS_GI"), UsedPath);
        if (TestNotNull(TEXT("Blueprint GameInstance asset path resolves without _C suffix"), BlueprintClass))
        {
            TestTrue(TEXT("Resolved class is a UGameInstance"), BlueprintClass->IsChildOf(UGameInstance::StaticClass()));
            TestTrue(TEXT("Resolved path uses the generated class"), UsedPath.EndsWith(TEXT("_C")));
        }
        TestNull(TEXT("Unknown GameInstance path resolves to null"),
            ResolveGameInstanceClass(TEXT("/Game/NoSuch/NoGameInstance"), UsedPath));

        // 项目设置直接选类：软类引用应保存为类路径（蓝图带 _C）并能解析成 UGameInstance 子类。
        const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
        TestFalse(TEXT("Settings bootstrap GameInstance is a selectable soft class"),
            Settings->BootstrapGameInstanceClass.IsNull());
        UClass* SettingsBootstrap = Settings->BootstrapGameInstanceClass.LoadSynchronous();
        TestTrue(TEXT("Settings bootstrap GameInstance resolves to a UGameInstance subclass"),
            SettingsBootstrap != nullptr && SettingsBootstrap->IsChildOf(UGameInstance::StaticClass()));
        TestTrue(TEXT("Settings resource GameInstance is stored as a class path"),
            Settings->ResourceGameInstanceClass.ToString().EndsWith(TEXT("_C")));

        RecordStartupGameInstanceDecision(TEXT("automation"), TEXT("/Game/BiNanSuo/BNS_GI"), UsedPath, TEXT("unit test"));
        const FString DecisionPath = FPaths::ProjectSavedDir() / TEXT("CloudUpdate")
            / CloudUpdatePrivate::GetRuntimeUpdatePlatform() / TEXT("startup_gameinstance.json");
        TestTrue(TEXT("Startup GameInstance decision record is written"), IFileManager::Get().FileExists(*DecisionPath));
        TestTrue(TEXT("Startup GameInstance decision is queryable"),
            CloudUpdatePrivate::GetStartupGameInstanceDecision().Contains(TEXT("automation")));
    }
    IFileManager::Get().DeleteDirectory(*Root, false, true);
    return true;
}
#endif

