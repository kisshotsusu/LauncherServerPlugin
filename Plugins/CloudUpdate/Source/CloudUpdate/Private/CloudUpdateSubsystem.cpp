#include "CloudUpdateSubsystem.h"
#include "CloudUpdateService.h"
#include "CloudUpdateSettings.h"
#include "Engine/Engine.h"
#include "HttpModule.h"

UCloudUpdateSubsystem* UCloudUpdateSubsystem::GetCloudUpdateSubsystem()
{
	return GEngine ? GEngine->GetEngineSubsystem<UCloudUpdateSubsystem>() : nullptr;
}

void UCloudUpdateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	// 允许自签名证书：服务器启用 HTTPS（自签名或内网证书）时，启动器才能正常拉取版本/清单。
	// 仅对内网分发与自签名场景放开；若服务器使用公网合法证书，此设置无副作用。
	FHttpModule::Get().SetAllowSelfSignedCertificates(true);
	Service = MakeShared<FCloudUpdateService>(this);

	const UCloudUpdateSettings* Settings = UCloudUpdateSettings::Get();
	if (Settings)
	{
		if (Settings->bAutoCheckUpdateOnStart)
		{
			CheckForUpdates();
		}
		if (Settings->bAutoCheckIntegrityOnStart)
		{
			CheckIntegrity(ECloudCheckMode::FullHash);
		}
	}
}

void UCloudUpdateSubsystem::Deinitialize()
{
	if (Service.IsValid())
	{
		Service->Abort();
		Service.Reset();
	}
	Super::Deinitialize();
}

void UCloudUpdateSubsystem::CheckIntegrity(ECloudCheckMode CheckMode)
{
	if (Service.IsValid())
	{
		Service->CheckIntegrity(CheckMode);
	}
}

void UCloudUpdateSubsystem::RepairIssues()
{
	if (Service.IsValid())
	{
		Service->RepairIssues();
	}
}

void UCloudUpdateSubsystem::CheckForUpdates()
{
	if (Service.IsValid())
	{
		Service->CheckForUpdates();
	}
}

void UCloudUpdateSubsystem::ApplyUpdate(const FString& VersionId)
{
	if (Service.IsValid())
	{
		Service->ApplyUpdate(VersionId);
	}
}

void UCloudUpdateSubsystem::AbortCurrentTask()
{
	if (Service.IsValid())
	{
		Service->Abort();
	}
}

bool UCloudUpdateSubsystem::IsBusy() const
{
	return Service.IsValid() && Service->IsBusy();
}

FString UCloudUpdateSubsystem::GetLocalVersion() const
{
	return Service.IsValid() ? Service->GetLocalVersion() : FString();
}

void UCloudUpdateSubsystem::SetLocalVersion(const FString& VersionId)
{
	if (Service.IsValid())
	{
		Service->SetLocalVersion(VersionId);
	}
}

FString UCloudUpdateSubsystem::GetServerUrl() const
{
	return Service.IsValid() ? Service->GetServerUrl() : FString();
}

void UCloudUpdateSubsystem::SetServerUrl(const FString& InUrl)
{
	if (Service.IsValid())
	{
		Service->SetServerUrl(InUrl);
	}
}