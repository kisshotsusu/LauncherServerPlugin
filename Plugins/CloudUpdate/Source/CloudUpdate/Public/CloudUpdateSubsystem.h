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

	/** 中止当前任务（当前 HTTP 请求完成后停止） */
	UFUNCTION(BlueprintCallable, Category = "CloudUpdate")
	void AbortCurrentTask();

	UFUNCTION(BlueprintPure, Category = "CloudUpdate")
	bool IsBusy() const;

	/** 获取本地已应用版本号 */
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

	/** 回滚被撤销（隐藏/删除）版本完成 */
	UPROPERTY(BlueprintAssignable, Category = "CloudUpdate")
	FOnCloudRollbackFinished OnRollbackFinished;

private:
	TSharedPtr<FCloudUpdateService> Service;
};