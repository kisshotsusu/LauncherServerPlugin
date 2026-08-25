#pragma once

#include "CoreMinimal.h"
#include "CloudUpdateTypes.generated.h"

/** 完整性检查模式 */
UENUM(BlueprintType)
enum class ECloudCheckMode : uint8
{
	/** 仅比较文件大小（快速） */
	SizeOnly UMETA(DisplayName = "仅大小"),
	/** 计算并比较哈希（完整） */
	FullHash UMETA(DisplayName = "完整哈希")
};

/** 云端任务阶段 */
UENUM(BlueprintType)
enum class ECloudUpdatePhase : uint8
{
	Idle UMETA(DisplayName = "空闲"),
	CheckingIntegrity UMETA(DisplayName = "完整性检查"),
	Repairing UMETA(DisplayName = "修复文件"),
	CheckingUpdates UMETA(DisplayName = "检查更新"),
	DownloadingUpdate UMETA(DisplayName = "下载更新"),
	ApplyingUpdate UMETA(DisplayName = "应用更新")
};

/** 文件问题类型 */
UENUM(BlueprintType)
enum class ECloudFileIssueType : uint8
{
	Missing UMETA(DisplayName = "缺失"),
	SizeMismatch UMETA(DisplayName = "大小不符"),
	HashMismatch UMETA(DisplayName = "哈希不符"),
	DownloadFailed UMETA(DisplayName = "下载失败")
};

/** 下载文件类型 */
UENUM(BlueprintType)
enum class ECloudDownloadKind : uint8
{
	ContentPak UMETA(DisplayName = "内容 Pak"),
	IoStoreContainer UMETA(DisplayName = "IoStore 容器"),
	ExternFile UMETA(DisplayName = "外部文件")
};

/** 二进制补丁合并结果（供蓝图区分「需重启生效」的场景） */
UENUM(BlueprintType)
enum class EBinaryMergeResult : uint8
{
	/** 已就地重建并替换基础文件，立即可用 */
	Success UMETA(DisplayName = "成功"),
	/** 合并失败（无 HDiffPatch / 基础或补丁缺失 / 补丁损坏） */
	Failed UMETA(DisplayName = "失败"),
	/** 基础文件被占用（如 pak 已挂载），已暂存为 .pending，需重启后由 FinalizePendingMerges 交换生效 */
	StagedForRestart UMETA(DisplayName = "已暂存，需重启")
};

/** 服务端清单中的文件条目 */
USTRUCT(BlueprintType)
struct CLOUDUPDATE_API FCloudFileEntry
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString RelativePath;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	int64 FileSize = 0;

	/** 小写十六进制哈希 */
	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString Hash;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString HashType;
};

/** 完整性检查结果条目 */
USTRUCT(BlueprintType)
struct CLOUDUPDATE_API FCloudFileIssue
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString RelativePath;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	ECloudFileIssueType IssueType = ECloudFileIssueType::Missing;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString ExpectedHash;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString ActualHash;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	int64 ExpectedSize = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	int64 ActualSize = 0;
};

/** 需要下载的文件描述 */
USTRUCT(BlueprintType)
struct CLOUDUPDATE_API FCloudDownloadFile
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString FileName;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString Url;

	/** 相对本地安装根目录的目标路径（Pak 文件为相对 Paks 目录的文件名） */
	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString TargetRelativePath;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString Hash;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	int64 FileSize = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	ECloudDownloadKind Kind = ECloudDownloadKind::ExternFile;

	/**
	 * 是否为二进制补丁条目：下载的是 .patch 增量文件，需先经 HDiffPatch
	 * 合并到本地基础文件（pak/utoc）后，基础文件即为更新后的版本。
	 * 此时 TargetRelativePath/FileName 代表补丁文件，合并目标为基础文件（去掉 .patch）。
	 */
	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	bool bBinaryPatch = false;

	/** 无法应用二进制补丁（无 HDiffPatch 或基础文件缺失）时，用于整文件回退下载的地址 */
	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString FallbackUrl;
};

/** 版本信息（来自管理服务器索引） */
USTRUCT(BlueprintType)
struct CLOUDUPDATE_API FCloudUpdateVersionInfo
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString VersionId;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString BaseVersionId;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString Date;

	/** patch / full */
	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString Type;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString Url;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	int32 ChangedAssetCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	int32 DeletedAssetCount = 0;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	int64 TotalSizeBytes = 0;
};

/** 完整性清单 */
USTRUCT(BlueprintType)
struct CLOUDUPDATE_API FCloudUpdateManifest
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString Project;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString Platform;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	FString BaseVersionId;

	UPROPERTY(BlueprintReadOnly, Category = "CloudUpdate")
	TArray<FCloudFileEntry> Files;
};

/** 本地已应用版本记录 */
USTRUCT()
struct FCloudLocalVersion
{
	GENERATED_BODY()

	UPROPERTY()
	FString VersionId;

	UPROPERTY()
	FString AppliedAt;
};