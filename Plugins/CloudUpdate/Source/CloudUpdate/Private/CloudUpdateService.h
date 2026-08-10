#pragma once

#include "CoreMinimal.h"
#include "CloudUpdateTypes.h"

class UCloudUpdateSubsystem;
class FJsonObject;

/** 云更新核心逻辑（非 UObject，生命周期跟随子系统） */
class FCloudUpdateService : public TSharedFromThis<FCloudUpdateService>
{
public:
	explicit FCloudUpdateService(UCloudUpdateSubsystem* InOwner);
	~FCloudUpdateService();

	bool IsBusy() const { return bBusy; }

	void CheckIntegrity(ECloudCheckMode InMode);
	void RepairIssues();
	void CheckForUpdates();
	void ApplyUpdate(const FString& InVersionId);
	void Abort();

	FString GetLocalVersion() const;
	void SetLocalVersion(const FString& InVersionId);
	FString GetServerUrl() const;
	void SetServerUrl(const FString& InUrl);

private:
	UCloudUpdateSubsystem* Owner = nullptr;

	bool bBusy = false;
	bool bAbortRequested = false;

	ECloudCheckMode IntegrityMode = ECloudCheckMode::FullHash;
	TArray<FCloudFileIssue> PendingIssues;
	FCloudUpdateManifest ServerManifest;

	FString PendingVersionId;
	TArray<FCloudDownloadFile> PendingFiles;
	int32 CurrentFileIndex = 0;
	int32 CompletedFiles = 0;
	int32 FailedFiles = 0;
	bool bIoStoreApplied = false;
	bool bDirectIoStore = false;
	bool bRestartRequired = false;

	// ---------- URL / 路径 ----------
	FString GetProjectName() const;
	FString GetPlatform() const;
	FString GetLocalRoot() const;
	FString GetPakDir() const;
	FString GetManifestUrl() const;
	FString GetVersionsUrl() const;
	FString GetVersionUrl(const FString& InVersionId) const;
	bool IsPathIgnored(const FString& InRelativePath) const;

	// ---------- HTTP ----------
	void FetchJson(const FString& InUrl, TFunction<void(bool, const TSharedPtr<FJsonObject>&)> InCallback);
	void DownloadFileTo(const FString& InUrl, const FString& InTargetPath, int32 InRetriesLeft, TFunction<void(bool)> InCallback);

	// ---------- 完整性检查 ----------
	void ParseManifest(const TSharedPtr<FJsonObject>& InJson);
	void RunLocalComparison();
	void CompareLocalResults(const TArray<TPair<int32, FString>>& InResults);
	void FinishIntegrityCheck(bool bSuccess, const TArray<FCloudFileIssue>& InIssues, const FString& InMessage);

	// ---------- 修复 ----------
	void StartRepair();
	void DownloadNextRepairFile();
	void OnRepairFileDownloaded(bool bSuccess, const FCloudFileIssue& InIssue);

	// ---------- 更新检查 ----------
	void ParseVersionsIndex(const TSharedPtr<FJsonObject>& InJson);

	// ---------- 回滚被撤销版本 ----------
	void RollbackRevokedVersion(const FString& RevokedVersion, const TArray<FCloudDownloadFile>& Files, const FString& TargetVersion);

	// ---------- 应用更新 ----------
	void ResolveUpdateDirect();
	void OnPatchConfigFetched(const TSharedPtr<FJsonObject>& InJson);
	void OnPakFilesInfoFetched(const TSharedPtr<FJsonObject>& InJson);
	void ParseDescriptor(const TSharedPtr<FJsonObject>& InJson);
	void StartDownloadUpdateFiles();
	void DownloadNextUpdateFile();
	void OnUpdateFileDownloaded(bool bSuccess, const FCloudDownloadFile& InFile);
	void MountPendingPaks();
	void FinishUpdate(bool bSuccess, const FString& InMessage);

	// ---------- 版本记录 ----------
	FString LoadLocalVersion() const;
	void SaveLocalVersion(const FString& InVersionId);

	void SetBusy(bool bInBusy);
};
