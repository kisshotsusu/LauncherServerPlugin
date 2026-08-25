#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CloudUpdateSettings.generated.h"

/**
 * CloudUpdate 运行时设置
 * 可在 项目设置 -> Cloud Update (云更新) 中配置
 */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "Cloud Update (云更新)"))
class CLOUDUPDATE_API UCloudUpdateSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UCloudUpdateSettings();

	/** 管理服务器地址，例如 http://127.0.0.1:8710 */
	UPROPERTY(EditAnywhere, config, Category = "服务器", meta = (DisplayName = "服务器地址"))
	FString ServerUrl;

	/** 项目名，与 Server/config.json 中的 project 一致 */
	UPROPERTY(EditAnywhere, config, Category = "服务器", meta = (DisplayName = "项目名"))
	FString ProjectName;

	/** 目标平台，例如 Windows */
	UPROPERTY(EditAnywhere, config, Category = "服务器", meta = (DisplayName = "平台"))
	FString Platform;

	/**
	 * 直接模式：填写 HotPatcher 产物 HTTP 根地址后，客户端将直接按
	 * {根}/{版本号}/{版本号}_PatchConfig.json 与 _PakFilesInfo.json 解析下载包。
	 * 留空则使用管理服务器的 /api/version/{id} 描述文件。
	 */
	UPROPERTY(EditAnywhere, config, Category = "服务器", meta = (DisplayName = "HotPatcher JSON 直连根地址（可选）"))
	FString HotPatcherBaseUrl;

	/** 本地安装根目录覆盖，留空时自动判断（打包版为 FPaths::RootDir） */
	UPROPERTY(EditAnywhere, config, Category = "本地", meta = (DisplayName = "本地安装根目录覆盖（可选）"))
	FString LocalRootOverride;

	/** 当前本地版本号，更新成功后会自动写入 Saved/CloudUpdate/local_version.json */
	UPROPERTY(EditAnywhere, config, Category = "本地", meta = (DisplayName = "当前版本号"))
	FString CurrentVersionId;

	/** 完整性检查时忽略的路径前缀（每行一个，如 Engine/） */
	UPROPERTY(EditAnywhere, config, Category = "本地", meta = (DisplayName = "忽略的路径前缀"))
	TArray<FString> IgnorePathPrefixes;

	/** 启动时自动检查更新 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "启动时自动检查更新"))
	bool bAutoCheckUpdateOnStart;

	/** 启动时自动完整性检查 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "启动时自动完整性检查"))
	bool bAutoCheckIntegrityOnStart;

	/** 下载完成后校验哈希，不匹配则重试 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "下载后校验哈希"))
	bool bVerifyDownloadHash;

	/** HTTP 超时（秒） */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "HTTP 超时（秒）", ClampMin = "5"))
	int32 HttpTimeoutSeconds;

	/** 单个文件下载失败重试次数 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "下载失败重试次数", ClampMin = "0"))
	int32 DownloadRetryCount;

	/**
	 * 启用二进制补丁合并：下载 .patch 后通过 HotPatcher 的 HDiffPatch（IBinariesDiffPatchFeature）
	 * 把本地基础 pak/utoc 重建为更新版本。HDiffPatch 不可用、或基础文件缺失时，自动回退到整文件下载。
	 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "启用二进制补丁合并"))
	bool bEnableBinaryMerge;

	/** 指定使用的二进制补丁特性名（对应 HDiffPatchUE 注册名）；留空自动选第一个可用特性 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "二进制补丁特性名（可选）"))
	FString BinaryPatchFeatureName;

	static const UCloudUpdateSettings* Get();
};