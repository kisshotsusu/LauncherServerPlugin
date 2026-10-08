#include "CloudUpdateService.h"
#include "CloudUpdateSubsystem.h"
#include "CloudUpdateSettings.h"
#include "CloudUpdateBinaryMerge.h"
#include "CloudUpdateUtil.h"
#include "Dom/JsonObject.h"
#include "Async/Async.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
using namespace CloudUpdatePrivate;

FString FCloudUpdateService::ResolveRemoteUrl(const FString& Url) const
{
 if (Url.StartsWith(TEXT("https://")) || Url.StartsWith(TEXT("http://"))) return Url;
 const FString Base = GetServerUrl();
 if (Url.StartsWith(TEXT("/")))
 {
  const int32 Scheme = Base.Find(TEXT("://"));
  const int32 Path = Scheme == INDEX_NONE ? INDEX_NONE : Base.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Scheme + 3);
  return (Path == INDEX_NONE ? Base : Base.Left(Path)) + Url;
 }
 return Base / Url;
}

bool FCloudUpdateService::ReadDescriptorFiles(const TSharedPtr<FJsonObject>& Json, TArray<FCloudDownloadFile>& OutFiles) const
{
 OutFiles.Reset();
 const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
 if (!Json.IsValid() || !Json->TryGetArrayField(TEXT("files"), Values)) return false;
 TSet<FString> Targets;
 for (const TSharedPtr<FJsonValue>& Value : *Values)
 {
  if (!Value.IsValid() || Value->Type != EJson::Object) return false;
  const TSharedPtr<FJsonObject> Obj = Value->AsObject();
  FCloudDownloadFile File;
  Obj->TryGetStringField(TEXT("fileName"), File.FileName);
  Obj->TryGetStringField(TEXT("url"), File.Url);
  Obj->TryGetStringField(TEXT("targetRelativePath"), File.TargetRelativePath);
  Obj->TryGetStringField(TEXT("hash"), File.Hash);
  File.bSizeKnown = Obj->TryGetNumberField(TEXT("size"), File.FileSize);
  if (File.FileSize < 0) return false;
  Obj->TryGetBoolField(TEXT("binaryPatch"), File.bBinaryPatch);
  Obj->TryGetStringField(TEXT("fallbackUrl"), File.FallbackUrl);
  File.bFallbackSizeKnown = Obj->TryGetNumberField(TEXT("fallbackSize"), File.FallbackFileSize) && File.FallbackFileSize >= 0;
  FString Kind;
  Obj->TryGetStringField(TEXT("kind"), Kind);
  File.bBinaryPatch |= Kind == TEXT("BinaryPatch") || FCloudBinaryMerge::IsPatchFile(File.FileName);
  if (Kind == TEXT("ContentPak")) File.Kind = ECloudDownloadKind::ContentPak;
  else if (Kind == TEXT("IoStore") || Kind == TEXT("IoStoreContainer")) File.Kind = ECloudDownloadKind::IoStoreContainer;
  else if (File.bBinaryPatch)
  {
   const FString BaseName = FCloudBinaryMerge::GetBaseFileName(File.FileName);
   if (BaseName.EndsWith(TEXT(".pak"))) File.Kind = ECloudDownloadKind::ContentPak;
   else if (BaseName.EndsWith(TEXT(".utoc")) || BaseName.EndsWith(TEXT(".ucas"))) File.Kind = ECloudDownloadKind::IoStoreContainer;
  }
  if (File.TargetRelativePath.IsEmpty()) File.TargetRelativePath = File.FileName;
  if (File.FileName.IsEmpty() || File.Url.IsEmpty() || File.FileName != FPaths::GetCleanFilename(File.FileName)
   || !IsSafeRelativePath(File.FileName) || !IsSafeRelativePath(File.TargetRelativePath)) return false;
  const FString TargetKey = FString::FromInt(static_cast<int32>(File.Kind)) + TEXT(":") + File.TargetRelativePath.ToLower();
  if (Targets.Contains(TargetKey)) return false;
  Targets.Add(TargetKey);
  File.Url = ResolveRemoteUrl(File.Url);
  if (!File.FallbackUrl.IsEmpty()) File.FallbackUrl = ResolveRemoteUrl(File.FallbackUrl);
  OutFiles.Add(MoveTemp(File));
 }
 return true;
}

void FCloudUpdateService::QueryUpdatePlan()
{
 if (bBusy)
 {
  if (Owner) Owner->OnUpdatePlanReady.Broadcast(false, UpdatePlan, TEXT("当前已有任务在执行"));
  return;
 }
 bQueryingPlan = true;
 CheckForUpdates();
}

void FCloudUpdateService::ResolveNextPlanDescriptor()
{
 if (bAbortRequested) { FinishPlan(false, TEXT("更新信息查询已取消")); return; }
 if (PlanDescriptorIndex >= PlanPendingVersions.Num()) { FinishPlan(true, PlanMessage); return; }
 const FCloudUpdateVersionInfo Info = PlanPendingVersions[PlanDescriptorIndex];
 TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
 FetchJson(GetVersionUrl(Info.VersionId), [WeakThis, Info](bool bOk, const TSharedPtr<FJsonObject>& Json)
 {
  const auto Self = WeakThis.Pin();
  if (!Self) return;
  if (!bOk || !Json.IsValid()) { Self->FinishPlan(false, TEXT("无法获取版本文件清单：") + Info.VersionId); return; }
  FPlannedVersion Version;
  Version.Info = Info;
  Json->TryGetStringField(TEXT("resourceVersion"), Version.Info.ResourceVersion);
  Json->TryGetStringField(TEXT("requiredGameVersion"), Version.Info.RequiredGameVersion);
  FString Type;
  if (Json->TryGetStringField(TEXT("type"), Type) && Type != Info.Type)
  { Self->FinishPlan(false, TEXT("索引与描述文件的版本类型不一致")); return; }
  Json->TryGetBoolField(TEXT("restartRequired"), Version.bRestartRequired);
  if (!Self->ReadDescriptorFiles(Json, Version.Files)) { Self->FinishPlan(false, TEXT("文件清单缺失、重复或路径无效：") + Info.VersionId); return; }
  const bool bGamePackage = Info.Type == TEXT("full");
  const bool bCompatible = Version.Info.RequiredGameVersion.IsEmpty() || Version.Info.RequiredGameVersion == Self->GetLocalGameVersion();
  if (!bGamePackage && (!bCompatible || Self->UpdatePlan.bResourceUpdateRequiresGame)) Self->UpdatePlan.bResourceUpdateRequiresGame = true;
  else
  {
   for (FCloudDownloadFile& File : Version.Files)
   {
    if (!bGamePackage && Self->IsBinaryPatchEntry(File))
    {
     const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
     const FString BasePath = File.Kind == ECloudDownloadKind::ExternFile ? Self->GetLocalRoot() / FCloudBinaryMerge::GetBaseFileName(File.TargetRelativePath) : Self->GetPakDir() / FCloudBinaryMerge::GetBaseFileName(File.FileName);
     File.bWillUseFallback = !Settings->bEnableBinaryMerge || !FCloudBinaryMerge::IsHDiffPatchAvailable() || !IFileManager::Get().FileExists(*BasePath);
    }
    if (bGamePackage)
    {
     ++Self->UpdatePlan.GamePackageFileCount;
     Self->UpdatePlan.GamePackageBytes += File.FileSize;
     Self->UpdatePlan.bGamePackageBytesKnown &= File.bSizeKnown;
    }
    else
    {
     const FString Ext = FPaths::GetExtension(File.FileName).ToLower();
     if (Ext == TEXT("exe") || Ext == TEXT("dll") || Ext == TEXT("so") || Ext == TEXT("dylib"))
     { Self->FinishPlan(false, TEXT("资源补丁包含游戏二进制文件，应发布为游戏整包：") + File.FileName); return; }
     ++Self->UpdatePlan.RequiredFileCount;
     Self->UpdatePlan.RequiredBytes += File.bWillUseFallback ? File.FallbackFileSize : File.FileSize;
     Self->UpdatePlan.bTotalBytesKnown &= File.bWillUseFallback ? File.bFallbackSizeKnown : File.bSizeKnown;
    }
   }
   if (!bGamePackage) ++Self->UpdatePlan.PendingResourceVersionCount;
   Self->PlannedVersions.Add(MoveTemp(Version));
  }
  ++Self->PlanDescriptorIndex;
  Self->ResolveNextPlanDescriptor();
 });
}

void FCloudUpdateService::FinishPlan(bool bSuccess, const FString& Message)
{
 const bool bCancelled = bAbortRequested;
 bSuccess &= !bCancelled;
 UpdatePlan.bValid = bSuccess;
 const bool bLatest = bResolvingLatestUpdate;
 const bool bSize = bQueryingUpdateSize;
 const bool bPlanOnly = bQueryingPlan;
 bResolvingLatestUpdate = bQueryingUpdateSize = bQueryingPlan = false;
 if (!bSuccess) PlannedVersions.Reset();
 const FCloudUpdatePlan Plan = UpdatePlan;
 const TArray<FCloudUpdateVersionInfo> Versions = PlanPendingVersions;
 const FString Latest = PlanLatestVersion;
 if (bLatest && bSuccess)
 {
  LatestUpdateQueue.Reset();
  for (const FPlannedVersion& Version : PlannedVersions)
   if (Version.Info.Type != TEXT("full")) LatestUpdateQueue.Add(Version.Info.VersionId);
  if (!LatestUpdateQueue.IsEmpty())
  {
   InitializeTransferSession(PlannedVersions);
   const FString Next = LatestUpdateQueue[0];
   LatestUpdateQueue.RemoveAt(0);
   SetBusy(false);
   ApplyUpdate(Next);
   return;
  }
 }
 SetBusy(false);
 if (bLatest)
 {
  PendingVersionId = LoadLocalVersion();
  FinishUpdate(bSuccess && !Plan.bResourceUpdateRequiresGame && !Plan.bGameUpdateRequired,
   !bSuccess ? Message : (Plan.bGameUpdateRequired || Plan.bResourceUpdateRequiresGame ? TEXT("需要先由启动器更新游戏包；运行时不替换 C++ 二进制") : TEXT("资源已是最新版本")));
  return;
 }
 if (bSize)
 {
  if (Owner) Owner->OnUpdateSizeQueryFinished.Broadcast(bSuccess && Plan.bTotalBytesKnown, Plan.RequiredBytes, Plan.PendingResourceVersionCount, Message);
  return;
 }
 // Use copies: listeners may synchronously initiate a new task.
 if (Owner) Owner->OnUpdatePlanReady.Broadcast(bSuccess, Plan, Message);
 if (!bPlanOnly && Owner) Owner->OnUpdateCheckFinished.Broadcast(bSuccess,
  bSuccess && (Plan.RequiredFileCount > 0 || Plan.bGameUpdateRequired || Plan.bResourceUpdateRequiresGame), Latest, Versions, Message);
}

void FCloudUpdateService::InitializeTransferSession(const TArray<FPlannedVersion>& Versions)
{
 Transfers.Reset();
 ActiveTransfer = INDEX_NONE;
 ++TransferGeneration;
 DownloadProgress = FCloudDownloadProgressInfo();
 MergeProgress = FCloudMergeProgressInfo();
 bTransferSessionActive = true;
 DownloadProgress.bActive = true;
 for (const FPlannedVersion& Version : Versions)
 {
  if (Version.Info.Type == TEXT("full")) continue;
  for (const FCloudDownloadFile& File : Version.Files)
  {
   FTransfer Transfer;
   Transfer.Key = Version.Info.VersionId + TEXT("\n") + File.TargetRelativePath;
   Transfer.Size = FMath::Max<int64>(File.bWillUseFallback ? File.FallbackFileSize : File.FileSize, 0);
   Transfer.bSizeKnown = File.bWillUseFallback ? File.bFallbackSizeKnown : File.bSizeKnown;
   Transfers.Add(MoveTemp(Transfer));
   if (IsBinaryPatchEntry(File)) ++MergeProgress.TotalFiles;
  }
 }
 BroadcastDownloadProgress();
 BroadcastMergeProgress();
}

void FCloudUpdateService::BroadcastDownloadProgress()
{
 DownloadProgress.CompletedFiles = DownloadProgress.FailedFiles = 0;
 DownloadProgress.TotalFiles = Transfers.Num();
 DownloadProgress.TotalBytes = DownloadProgress.DownloadedBytes = 0;
 DownloadProgress.bTotalBytesKnown = true;
 for (const FTransfer& Transfer : Transfers)
 {
  DownloadProgress.TotalBytes += Transfer.Size;
  DownloadProgress.DownloadedBytes += Transfer.Bytes;
  DownloadProgress.bTotalBytesKnown &= Transfer.bSizeKnown;
  DownloadProgress.CompletedFiles += Transfer.bCompleted ? 1 : 0;
  DownloadProgress.FailedFiles += Transfer.bFailed ? 1 : 0;
 }
 DownloadProgress.FileProgress = Transfers.Num() > 0 ? static_cast<float>(DownloadProgress.CompletedFiles) / Transfers.Num() : 0.0f;
 DownloadProgress.ByteProgress = !DownloadProgress.bTotalBytesKnown ? -1.0f :
  (DownloadProgress.TotalBytes > 0 ? FMath::Clamp(static_cast<float>(static_cast<double>(DownloadProgress.DownloadedBytes) / DownloadProgress.TotalBytes), 0.0f, 1.0f) :
   (DownloadProgress.CompletedFiles == Transfers.Num() && !Transfers.IsEmpty() ? 1.0f : 0.0f));
 if (Transfers.IsValidIndex(ActiveTransfer))
 {
  const FTransfer& Active = Transfers[ActiveTransfer];
  DownloadProgress.CurrentFileBytes = Active.Bytes;
  DownloadProgress.CurrentFileTotalBytes = Active.bSizeKnown ? Active.Size : -1;
  DownloadProgress.CurrentFileProgress = !Active.bSizeKnown ? -1.0f : (Active.Size > 0 ? FMath::Clamp(static_cast<float>(static_cast<double>(Active.Bytes) / Active.Size), 0.0f, 1.0f) : (Active.bCompleted ? 1.0f : 0.0f));
 }
 DownloadProgress.FilePercent = DownloadProgress.FileProgress * 100.0f;
 DownloadProgress.BytePercent = DownloadProgress.ByteProgress < 0 ? -1.0f : DownloadProgress.ByteProgress * 100.0f;
 DownloadProgress.CurrentFilePercent = DownloadProgress.CurrentFileProgress < 0 ? -1.0f : DownloadProgress.CurrentFileProgress * 100.0f;
 if (Owner)
 {
  const FCloudDownloadProgressInfo Snapshot = DownloadProgress;
  Owner->OnDetailedDownloadProgress.Broadcast(Snapshot);
  if (Owner) Owner->OnDownloadProgress.Broadcast(Snapshot.CurrentFileBytes, Snapshot.CurrentFileTotalBytes, Snapshot.ByteProgress);
 }
}

void FCloudUpdateService::UpdateTransferProgress(uint32 Generation, int64 Done, int64 Total)
{
 if (!bTransferSessionActive || Generation != TransferGeneration || !Transfers.IsValidIndex(ActiveTransfer)) return;
 FTransfer& Transfer = Transfers[ActiveTransfer];
 if (Total >= 0) { Transfer.Size = Total; Transfer.bSizeKnown = true; }
 Transfer.Bytes = FMath::Max(Transfer.Bytes, FMath::Max<int64>(Done, 0));
 if (Transfer.bSizeKnown) Transfer.Bytes = FMath::Min(Transfer.Bytes, Transfer.Size);
 BroadcastDownloadProgress();
}

void FCloudUpdateService::CompleteTransfer(uint32 Generation, bool bSuccess)
{
 if (Generation != TransferGeneration || !Transfers.IsValidIndex(ActiveTransfer)) return;
 FTransfer& Transfer = Transfers[ActiveTransfer];
 Transfer.bCompleted = bSuccess;
 Transfer.bFailed = !bSuccess;
 if (!bSuccess) Transfer.Bytes = 0;
 BroadcastDownloadProgress();
}

void FCloudUpdateService::StartTrackedDownload(const FCloudDownloadFile& File, const FString& Path, TFunction<void(bool)> Callback, bool bFallback)
{
 const FString Key = PendingVersionId + TEXT("\n") + File.TargetRelativePath;
 ActiveTransfer = Transfers.IndexOfByPredicate([&Key](const FTransfer& Transfer) { return Transfer.Key == Key; });
 if (ActiveTransfer == INDEX_NONE)
 {
  FTransfer Transfer; Transfer.Key = Key; Transfers.Add(Transfer); ActiveTransfer = Transfers.Num() - 1;
 }
 FTransfer& Transfer = Transfers[ActiveTransfer];
 if (bFallback) { Transfer.Bytes = 0; Transfer.bCompleted = false; Transfer.bFailed = false; Transfer.Size = File.FallbackFileSize; Transfer.bSizeKnown = File.bFallbackSizeKnown; }
 const uint32 Generation = ++TransferGeneration;
 DownloadProgress.CurrentFile = File.FileName;
 BroadcastDownloadProgress();
 const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
 TWeakPtr<FCloudUpdateService> WeakThis = AsShared();
 DownloadFileTo(bFallback ? File.FallbackUrl : File.Url, Path, Settings ? Settings->DownloadRetryCount : 2,
  [WeakThis, Generation, Callback](bool bSuccess)
  {
   const auto Self = WeakThis.Pin();
   if (!Self) return;
   Self->CompleteTransfer(Generation, bSuccess);
   if (Callback) Callback(bSuccess);
  },
  [WeakThis, Generation](int64 Done, int64 Total)
  {
   auto Report = [WeakThis, Generation, Done, Total]()
   {
    const auto Self = WeakThis.Pin();
    if (Self) Self->UpdateTransferProgress(Generation, Done, Total);
   };
   if (IsInGameThread()) Report(); else AsyncTask(ENamedThreads::GameThread, MoveTemp(Report));
  });
}

void FCloudUpdateService::BroadcastMergeProgress()
{
 MergeProgress.Progress = MergeProgress.TotalFiles > 0 ? static_cast<float>(MergeProgress.CompletedFiles) / MergeProgress.TotalFiles : 0.0f;
 MergeProgress.Percent = MergeProgress.Progress * 100.0f;
 if (Owner) Owner->ReportMergeProgress(MergeProgress);
}
void FCloudUpdateService::BeginMerge(const FString& File)
{
 MergeProgress.bActive = true;
 MergeProgress.bCurrentFileInProgress = true;
 MergeProgress.CurrentFile = File;
 BroadcastMergeProgress();
}
void FCloudUpdateService::CompleteMerge(bool bSuccess, bool bSkipped, bool bRestart)
{
 ++MergeProgress.CompletedFiles;
 if (bSkipped) ++MergeProgress.SkippedFiles;
 else if (bSuccess) ++MergeProgress.SucceededFiles;
 else ++MergeProgress.FailedFiles;
 MergeProgress.bRestartRequired |= bRestart;
 MergeProgress.bCurrentFileInProgress = false;
 MergeProgress.bActive = MergeProgress.CompletedFiles < MergeProgress.TotalFiles;
 BroadcastMergeProgress();
}
