#include "CloudUpdateSettings.h"
#include "UObject/SoftObjectPath.h"

UCloudUpdateSettings::UCloudUpdateSettings()
{
	ServerUrl = TEXT("http://127.0.0.1:8710");
	ProjectName = TEXT("CodeBuild");
	HotPatcherBaseUrl = TEXT("");
	ServerToken = TEXT("");
	LocalRootOverride = TEXT("");
	CurrentVersionId = TEXT("1.0");
	// 引导用 GameInstance 默认就是引擎原生 UGameInstance（软类引用，类选择器里可改选蓝图类）。
	BootstrapGameInstanceClass = TSoftClassPtr<UGameInstance>(FSoftObjectPath(TEXT("/Script/Engine.GameInstance")));
	ResourceGameInstanceClass = nullptr;
	bAutoCheckUpdateOnStart = false;
	bAutoCheckIntegrityOnStart = false;
	bVerifyDownloadHash = true;
	HttpTimeoutSeconds = 60;
	// 超时按"剩余字节 / 最低速率"估算：32 KiB/s、上限 30 分钟。
	DownloadMinBytesPerSecond = 32768;
	DownloadTimeoutMaxSeconds = 1800;
	SafeModeFailureThreshold = 2;
	SafeModeStableSeconds = 45;
	DownloadRetryCount = 2;
	bEnableBinaryMerge = true;
	BinaryPatchFeatureName = TEXT("");
	// 局域网回退：默认关闭，不影响既有部署；开启后仅在"服务器地址解析到本机"时生效。
	bEnableLanFallback = false;
	LanServerUrl = TEXT("");
	bLanFallbackInEditor = false;
	LanFallbackCacheSeconds = 30.0f;
}

const UCloudUpdateSettings* UCloudUpdateSettings::Get()
{
	return GetDefault<UCloudUpdateSettings>();
}
