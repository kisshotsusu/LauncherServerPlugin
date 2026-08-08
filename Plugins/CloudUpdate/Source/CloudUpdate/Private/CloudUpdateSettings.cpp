#include "CloudUpdateSettings.h"

UCloudUpdateSettings::UCloudUpdateSettings()
{
	ServerUrl = TEXT("http://127.0.0.1:8710");
	ProjectName = TEXT("CodeBuild");
	Platform = TEXT("Windows");
	HotPatcherBaseUrl = TEXT("");
	LocalRootOverride = TEXT("");
	CurrentVersionId = TEXT("1.0");
	bAutoCheckUpdateOnStart = false;
	bAutoCheckIntegrityOnStart = false;
	bVerifyDownloadHash = true;
	HttpTimeoutSeconds = 60;
	DownloadRetryCount = 2;
}

const UCloudUpdateSettings* UCloudUpdateSettings::Get()
{
	return GetDefault<UCloudUpdateSettings>();
}