#pragma once

#include "CoreMinimal.h"
#include "Subsystems/EngineSubsystem.h"
#include "CloudUpdateTypes.h"
#include "CloudUpdateSubsystem.generated.h"

class FCloudUpdateService;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCloudIntegrityCheckFinished, bool, bSuccess, const TArray<FCloudFileIssue>&, Issues, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCloudRepairProgress, int32, Completed, int32, Total, const FString&, CurrentFile);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCloudRepairFinished, bool, bSuccess, int32, RepairedCount, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FiveParams(FOnCloudUpdateCheckFinished, bool, bSuccess, bool, bHasUpdate, const FString&, LatestVersion, const TArray<FCloudUpdateVersionInfo>&, Versions, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnCloudUpdateProgress, float, Progress, int32, Completed, int32, Total, const FString&, CurrentFile);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnCloudUpdateFinished, bool, bSuccess, bool, bRestartRequired, const FString&, VersionId, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnCloudRollbackFinished, bool, bSuccess, bool, bRestartRequired, const FString&, VersionId, const FString&, Message);

/** 二进制补丁合并（或回退）完成事件，供蓝图显示「合并中/合并完成」状态 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCloudBinaryPatchFinished, bool, bSuccess, const FString&, BaseFilePath, const FString&, Message);

/** 目录级自动合并进度：已完成数、总数、当前补丁路径、该补丁是否成功 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnCloudAutoMergeProgress, int32, Completed, int32, Total, const FString&, CurrentPatch, bool, bSuccess);
/** 目录级自动合并完成：是否全部成功、成功数、失败数 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCloudAutoMergeFinished, bool, bAllSucceeded, int32, SucceededCount, int32, FailedCount);
/** 更新大小查询完成：是否成功、总字节数、待更新版本数、提示信息 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_FourParams(FOnCloudUpdateSizeQueryFinished, bool, bSuccess, int64, TotalBytes, int32, PendingVersionCount, const FString&, Message);
/** 下载字节级进度：当前文件已完成字节、总字节、整体进度 0-1 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCloudDownloadProgress, int64, BytesDone, int64, BytesTotal, float, OverallProgress);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnCloudUpdatePlanReady, bool, bSuccess, const FCloudUpdatePlan&, Plan, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCloudDetailedDownloadProgress, const FCloudDownloadProgressInfo&, Progress);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnCloudMergeProgress, const FCloudMergeProgressInfo&, Progress);

/**
 * 云更新运行时子系统
 * 提供：完整性检查、损坏文件云端修复、基于 HotPatcher JSON 的更新包下载与应用。
 * 可在蓝图/动画蓝图/关卡蓝图中直接调用。
 */
UCLASS(BlueprintType)
class CLOUDUPDATE_API UCloudUpdateSubsystem : public UEngineSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintPure, Category = "CloudUpdate")
	static UCloudUpdateSubsystem* GetCloudUpdateSubsystem();

 /** Local game version comes from the installed build's ProjectVersion; resource patches never modify it. */
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|版本", meta = (DisplayName = "Get Local Game Version"))
 FString GetLocalGameVersion() const;
 /** Server getters are cached. Call QueryUpdatePlan and wait for OnUpdatePlanReady first. */
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|版本")
 FString GetServerGameVersion() const;
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|版本")
 FString GetLocalResourceVersion() const;
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|版本")
 FString GetServerResourceVersion() const;
 /** Downloaded resource version awaiting restart/activation. Empty means none. */
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|版本")
 FString GetPendingResourceVersion() const;
 /** Configure an initial resource version via project settings; legacy setter remains compatible. */
 UFUNCTION(BlueprintCallable, Category = "CloudUpdate|版本")
 void SetLocalResourceVersion(const FString& VersionId);

 /** Fetch platform-specific index and descriptors, without downloading game/resource files. */
 UFUNCTION(BlueprintCallable, Category = "CloudUpdate|更新信息")
 void QueryUpdatePlan();
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|更新信息")
 FCloudUpdatePlan GetUpdatePlan() const;
 /** Compatible resource files. Check GetUpdatePlan().bValid before using the cached value. */
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|更新信息")
 int32 GetRequiredUpdateFileCount() const;
 /** Bytes. If bTotalBytesKnown is false this is only the sum of known sizes. */
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|更新信息")
 int64 GetRequiredUpdateSizeBytes() const;
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|进度")
 FCloudDownloadProgressInfo GetDownloadProgress() const;
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|进度")
 FCloudMergeProgressInfo GetMergeProgress() const;

 UPROPERTY(BlueprintAssignable, Category = "CloudUpdate|更新信息")
 FOnCloudUpdatePlanReady OnUpdatePlanReady;
 UPROPERTY(BlueprintAssignable, Category = "CloudUpdate|进度")
 FOnCloudDetailedDownloadProgress OnDetailedDownloadProgress;
 UPROPERTY(BlueprintAssignable, Category = "CloudUpdate|进度")
 FOnCloudMergeProgress OnMergeProgress;

 /** C++ internal reporting, always called on the game thread. */
 void ReportMergeProgress(const FCloudMergeProgressInfo& Progress);

	/** 启动完整性检查：下载服务端清单并与本地文件比对 */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void CheckIntegrity(ECloudCheckMode CheckMode = ECloudCheckMode::FullHash);

	/** 修复上次完整性检查发现的问题文件（缺失/损坏文件从云端下载替换） */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void RepairIssues();

	/** 检查服务端是否有可用更新（根据 HotPatcher 产物索引） */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void CheckForUpdates();

	/** 下载并应用指定版本更新（解析 HotPatcher JSON -> 下载 Pak/外部文件 -> 挂载） */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void ApplyUpdate(const FString& VersionId);

	/** 查询待更新总大小（异步）。完成后广播 OnUpdateSizeQueryFinished。 */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void QueryPendingUpdateSize();

	/** 按服务端依赖顺序更新；失败或需重启时停止，重启后再次调用可继续。 */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void ApplyLatestUpdate();

	/** 高级接口：区分立即完成、失败和暂存待重启。FeatureName 留空使用项目设置。 */
    UFUNCTION(BlueprintCallable, Category = "CloudUpdate|二进制补丁", meta = (AdvancedDisplay = "FeatureName"))
    EBinaryMergeResult ApplyBinaryPatchToBaseEx(const FString& BaseFilePath, const FString& PatchFilePath, FString& OutMergedPath, const FString& FeatureName = TEXT(""));

 /** UI-friendly async merge. Returns false if another task is busy; completion uses OnBinaryPatchFinished. */
 UFUNCTION(BlueprintCallable, Category = "CloudUpdate|二进制补丁", meta = (AdvancedDisplay = "FeatureName"))
 bool ApplyBinaryPatchAsync(const FString& BaseFilePath, const FString& PatchFilePath, const FString& FeatureName = TEXT(""));

    /** 中止当前任务（当前 HTTP 请求完成后停止） */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void AbortCurrentTask();

	UFUNCTION(BlueprintPure, Category = "CloudUpdate")
	bool IsBusy() const;

	/** 兼容接口：获取本地资源版本号，不代表游戏二进制版本。 */
	UFUNCTION(BlueprintPure, Category = "CloudUpdate")
	FString GetLocalVersion() const;

	/** 写入本地已应用版本号 */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void SetLocalVersion(const FString& VersionId);

	UFUNCTION(BlueprintPure, Category = "CloudUpdate")
	FString GetServerUrl() const;

	/** 运行时修改服务器地址并保存到配置 */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void SetServerUrl(const FString& InUrl);

	/** 读取访问令牌（与项目设置中的「访问令牌」一致） */
	UFUNCTION(BlueprintPure, Category = "CloudUpdate")
	FString GetServerToken() const;

	/** 运行时修改访问令牌并保存到配置 */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void SetServerToken(const FString& InToken);

	// ---------- 二进制补丁合并（蓝图控制） ----------

	/** 运行时是否有可用的二进制补丁实现（HDiffPatch 已注册）。UI 可据此判断能否走增量更新 */
	UFUNCTION(BlueprintPure, Category = "CloudUpdate|二进制补丁")
	bool IsBinaryMergeAvailable() const;

	/** 读取设置项：是否启用了二进制补丁合并 */
	UFUNCTION(BlueprintPure, Category = "CloudUpdate|二进制补丁")
	bool IsBinaryMergeEnabled() const;

	/** 运行时开关二进制补丁合并，并写入配置（下次启动仍生效） */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate|二进制补丁")
	void SetBinaryMergeEnabled(bool bEnable);

	/** 返回当前会使用的二进制补丁特性名（如 HDiffPatchUE）；无可用实现时返回空 */
	UFUNCTION(BlueprintPure, Category = "CloudUpdate|二进制补丁")
	FString GetBinaryPatchFeatureName() const;

	/**
	 * 手动将补丁应用到本地基础文件（高级/调试用）：把 InPatchFilePath 合并到 InBaseFilePath，
	 * 成功时基础文件被重建为新版本，OutMergedPath 等于 InBaseFilePath。
	 * 失败（无 HDiffPatch / 基础或补丁文件缺失）返回 false。
	 */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate|二进制补丁")
	bool ApplyBinaryPatchToBase(const FString& BaseFilePath, const FString& PatchFilePath, const FString& FeatureName, FString& OutMergedPath);

	/** 查找目录中的所有 .patch 文件（递归由 bIncludeSubdirectories 控制），供自动合并前预览 */
	UFUNCTION(BlueprintPure, Category = "CloudUpdate|二进制补丁")
	TArray<FString> FindPatchFiles(const FString& Directory, bool bIncludeSubdirectories = true) const;

	/**
	 * 自动合并：扫描目录中所有 .patch 并依次应用到相邻基础文件（X.patch -> X）。
	 * 在后台线程执行以避免阻塞游戏线程，过程中持续广播 OnAutoMergeProgress，
	 * 全部完成后广播 OnAutoMergeFinished。HDiffPatch 不可用时直接广播完成（全部失败）。
	 * 基础文件缺失的补丁会被跳过并计入失败。
	 */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate|二进制补丁")
	void AutoMergePatches(const FString& Directory, bool bIncludeSubdirectories = true);

	/** AutoMergePatches 的便捷版：直接扫描项目 Paks 目录（与下载流程同一目录） */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate|二进制补丁")
	void AutoMergePatchesInPaksDir(bool bIncludeSubdirectories = true);

	/** 完整性检查完成 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudIntegrityCheckFinished OnIntegrityCheckFinished;

	/** 修复进度 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudRepairProgress OnRepairProgress;

	/** 修复完成 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudRepairFinished OnRepairFinished;

	/** 更新检查完成 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudUpdateCheckFinished OnUpdateCheckFinished;

	/** 更新下载/应用进度 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudUpdateProgress OnUpdateProgress;

	/** 更新完成 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudUpdateFinished OnUpdateFinished;

	/** 待更新大小查询完成 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudUpdateSizeQueryFinished OnUpdateSizeQueryFinished;

	/** 下载实时进度（字节级，HTTP 回调线程投递回游戏线程） */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudDownloadProgress OnDownloadProgress;

	/** 回滚被撤销（隐藏/删除）版本完成 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudRollbackFinished OnRollbackFinished;

	/** 二进制补丁合并（或回退整文件）完成 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate|二进制补丁")
	FOnCloudBinaryPatchFinished OnBinaryPatchFinished;

	/** 目录级自动合并进度 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate|二进制补丁")
	FOnCloudAutoMergeProgress OnAutoMergeProgress;

	/** 目录级自动合并完成 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate|二进制补丁")
	FOnCloudAutoMergeFinished OnAutoMergeFinished;

private:
	TSharedPtr<FCloudUpdateService> Service;
 FCloudMergeProgressInfo MergeProgress;
	/** 防止同一目录被并发自动合并（仅由游戏线程读写，安全） */
	bool bAutoMergeRunning = false;
};
