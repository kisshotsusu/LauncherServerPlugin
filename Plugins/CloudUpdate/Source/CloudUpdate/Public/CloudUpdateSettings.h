#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "CloudUpdateSettings.generated.h"

/** 随所有 HTTP 请求附加的自定义请求头（键值对） */
USTRUCT(BlueprintType)
struct FCloudUpdateHttpHeader
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudUpdate")
	FString Key;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "CloudUpdate")
	FString Value;
};

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

	virtual FName GetCategoryName() const override { return TEXT("Game"); }
#if WITH_EDITOR
	// GetSectionText / GetSectionDescription 仅在编辑器构建中由 UDeveloperSettings 提供
	// （引擎 DeveloperSettings.h 中位于 #if WITH_EDITOR 内）。打包（Game/Shipping）构建
	// 里基类没有这两个虚函数，必须一并条件编译，否则 override 会因“无可重写基类方法”而 C3668。
	virtual FText GetSectionText() const override { return NSLOCTEXT("CloudUpdate", "SettingsSection", "云更新"); }
	virtual FText GetSectionDescription() const override { return NSLOCTEXT("CloudUpdate", "SettingsDesc", "配置更新服务器、本地路径与更新行为"); }
#endif

    /** Select once before engine creates its GameInstance. Takes effect after process restart. */
    UPROPERTY(EditAnywhere, config, Category = "启动实例")
    bool bSelectGameInstanceFromResources = false;

    /** String paths intentionally avoid making gameplay assets cooker dependencies. */
    UPROPERTY(EditAnywhere, config, Category = "启动实例")
    FString BootstrapGameInstanceClass = TEXT("/Script/Engine.GameInstance");

    UPROPERTY(EditAnywhere, config, Category = "启动实例")
    FString ResourceGameInstanceClass;

	/**
	 * 局域网回退总开关。开启后，若服务器地址解析出的 IP 与本机地址相同
	 * （例如配置了 127.0.0.1、或域名解析到了设备自己的地址），
	 * 所有请求会自动改用下面的「局域网服务器地址」，保证同网段联调/内网部署可用。
	 * 只改主机与端口，路径与查询串保持不变；解析结果缓存（见 LanFallbackCacheSeconds）。
	 */
	UPROPERTY(EditAnywhere, config, Category = "局域网回退", meta = (DisplayName = "启用局域网回退"))
	bool bEnableLanFallback;

	/** 回退目标局域网地址，例如 http://192.168.1.10:8710（可带路径前缀；缺端口时沿用原 URL 的端口） */
	UPROPERTY(EditAnywhere, config, Category = "局域网回退", meta = (DisplayName = "局域网服务器地址"))
	FString LanServerUrl;

	/** 编辑器进程内是否也应用回退。默认关闭，避免打断本机 127.0.0.1 的编辑器联调 */
	UPROPERTY(EditAnywhere, config, Category = "局域网回退", meta = (DisplayName = "编辑器内也应用"))
	bool bLanFallbackInEditor;

	/** 回退判定（DNS + 网卡枚举）的缓存时长，单位秒；0 表示每次请求都重新判定 */
	UPROPERTY(EditAnywhere, config, Category = "局域网回退", meta = (DisplayName = "回退判定缓存（秒）", ClampMin = "0", UIMin = "0", UIMax = "600"))
	float LanFallbackCacheSeconds;

	/** 管理服务器地址，例如 http://127.0.0.1:8710 */
	UPROPERTY(EditAnywhere, config, Category = "服务器", meta = (DisplayName = "服务器地址（如 http://127.0.0.1:8710）"))
	FString ServerUrl;

	/** 项目名，与 Server/config.json 中的 project 一致 */
	UPROPERTY(EditAnywhere, config, Category = "服务器", meta = (DisplayName = "项目名"))
	FString ProjectName;

	/**
	 * 直接模式：填写 HotPatcher 产物 HTTP 根地址后，客户端将直接按
	 * {根}/{版本号}/{版本号}_PatchConfig.json 与 _PakFilesInfo.json 解析下载包。
	 * 留空则使用管理服务器的 /api/version/{id} 描述文件。
	 */
	UPROPERTY(EditAnywhere, config, Category = "服务器", meta = (DisplayName = "HotPatcher JSON 直连根地址（可选）"))
	FString HotPatcherBaseUrl;

	/** 访问令牌（可选）：非空时以 Authorization: Bearer <token> 形式随所有请求发送，供需要鉴权的服务器使用 */
	UPROPERTY(EditAnywhere, config, Category = "服务器", meta = (DisplayName = "访问令牌（可选，Bearer）"))
	FString ServerToken;

	/** 自定义 HTTP 请求头（可选）：随所有请求附加，适合 API 网关 / 鉴权等场景 */
	UPROPERTY(EditAnywhere, config, Category = "服务器", meta = (DisplayName = "自定义请求头（可选）"))
	TArray<FCloudUpdateHttpHeader> CustomHeaders;

	/** 外部文件根目录覆盖；默认桌面使用安装目录，移动端使用 Saved/CloudUpdate/Files。移动端资源包使用 Saved/Paks。 */
	UPROPERTY(EditAnywhere, config, Category = "本地", meta = (DisplayName = "本地安装根目录覆盖（可选）"))
	FString LocalRootOverride;

 /** Initial resource release installed with this build. Empty migrates the old CurrentVersionId. */
 UPROPERTY(EditAnywhere, config, Category = "本地", meta = (DisplayName = "初始资源版本号"))
 FString InitialResourceVersionId;

 /** Optional installed binary version. Empty uses Project Settings / Project Version. Never advanced by resource updates. */
 UPROPERTY(EditAnywhere, config, Category = "本地", meta = (DisplayName = "游戏包版本覆盖（可选）"))
 FString GameVersionOverride;

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

	/** HTTP 超时（秒）。UE 的 SetTimeout 是"整个请求"的总超时，这里作为**下限**。 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "HTTP 超时（秒）", ClampMin = "5"))
	int32 HttpTimeoutSeconds;

	/**
	 * 估算下载超时时假定的最低速率（字节/秒）。实际超时 = max(HttpTimeoutSeconds, 剩余字节 / 本值)，
	 * 再被 DownloadTimeoutMaxSeconds 截断。0 表示不做大小估算（只用 HttpTimeoutSeconds）。
	 * 固定 60s 总超时会让大 pak/utoc 在慢网络下必然被掐断，是"进度到 100% 却报失败"的常见成因。
	 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "下载最低速率（字节/秒）", ClampMin = "0"))
	int32 DownloadMinBytesPerSecond;

	/** 单次下载超时的上限（秒），0 表示不限制 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "下载超时上限（秒）", ClampMin = "0"))
	int32 DownloadTimeoutMaxSeconds;

	/** 安全模式：资源实例连续启动失败到该次数后，强制回退 bootstrap 引导包（保留更新/修复入口） */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "安全模式触发次数", ClampMin = "1", UIMax = "10"))
	int32 SafeModeFailureThreshold;

	/** 启动后多少秒视为"已稳定"（清除本次资源实例启动的尝试记录）。崩溃发生在稳定之前才会累计次数 */
	UPROPERTY(EditAnywhere, config, Category = "行为", meta = (DisplayName = "启动稳定判定（秒）", ClampMin = "5"))
	int32 SafeModeStableSeconds;

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
