#pragma once

#include "CoreMinimal.h"
#include "CloudUpdateTypes.h"

class UCloudUpdateSubsystem;
class FJsonObject;

/** 云更新核心逻辑（非 UObject，生命周期跟随子系统） */
class FCloudUpdateService : public TSharedFromThis<FCloudUpdateService>
{
public:
	/** 完整性检查的后台哈希计算结果（由 RunLocalComparison 产出，CompareLocalResults 消费） */
	struct FCompareResult
	{
		int32 Index = 0;
		bool bExists = false;
		int64 Size = 0;
		FString Hash;
	};

	explicit FCloudUpdateService(UCloudUpdateSubsystem* InOwner);
	~FCloudUpdateService();

	bool IsBusy() const { return bBusy; }

	void CheckIntegrity(ECloudCheckMode InMode);
	void RepairIssues();
	void CheckForUpdates();
	void ApplyUpdate(const FString& InVersionId);
	void ApplyLatestUpdate();
	void QueryPendingUpdateSize();
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
	/** 查询更新大小时的待更新列表（复用 CheckForUpdates 解析结果） */
	TArray<FCloudUpdateVersionInfo> SizeQueryPendingVersions;

	/** 经二进制补丁合并后生成/更新的 ContentPak 基础文件路径，供最终挂载 */
	TArray<FString> MergedPakPaths;

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
	void DownloadFileTo(const FString& InUrl, const FString& InTargetPath, int32 InRetriesLeft,
		TFunction<void(bool)> InCallback,
		TFunction<void(int64 /*BytesDone*/, int64 /*TotalBytes*/)> InProgress = nullptr);

	// ---------- 完整性检查 ----------
	void ParseManifest(const TSharedPtr<FJsonObject>& InJson);
	void RunLocalComparison();

	void CompareLocalResults(const TArray<FCompareResult>& InResults);
	void FinishIntegrityCheck(bool bSuccess, const TArray<FCloudFileIssue>& InIssues, const FString& InMessage);

	// ---------- 修复 ----------
	void StartRepair();
	void DownloadNextRepairFile();
	void OnRepairFileDownloaded(bool bSuccess, const FCloudFileIssue& InIssue);

	// ---------- 更新检查 ----------
	void ParseVersionsIndex(const TSharedPtr<FJsonObject>& InJson);
	void HandleCheckForUpdatesFinished(bool bSuccess, bool bHasUpdate, const FString& LatestVersion,
		const TArray<FCloudUpdateVersionInfo>& Versions, const FString& Message);

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

	// ---------- 二进制补丁合并 ----------
	bool IsBinaryPatchEntry(const FCloudDownloadFile& InFile) const;
	/** 计算补丁条目的合并目标（基础文件）路径 */
	FString ResolveBaseTargetPath(const FCloudDownloadFile& InFile) const;
	/** 处理一个二进制补丁条目：下载 .patch 并合并，或回退整文件 */
	void HandleBinaryPatchEntry(const FCloudDownloadFile& InFile);
	/** 合并失败/无法合并时的整文件回退下载 */
	void TryBinaryPatchFallback(const FCloudDownloadFile& InFile, const FString& BasePath, const FString& PatchTemp);
	/** 补丁合并或回退下载完成后的统一收尾 */
	void OnBinaryPatchApplied(bool bSuccess, const FCloudDownloadFile& InFile, const FString& BasePath, bool bStagedForRestart = false);

	// ---------- 版本记录 ----------
	FString LoadLocalVersion() const;
	void SaveLocalVersion(const FString& InVersionId);

	void SetBusy(bool bInBusy);
};
