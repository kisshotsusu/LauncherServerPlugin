#include "CloudUpdateSubsystem.h"
#include "CloudUpdateService.h"
#include "CloudUpdateSettings.h"
#include "Engine/Engine.h"

UCloudUpdateSubsystem* UCloudUpdateSubsystem::GetCloudUpdateSubsystem()
{
	return GEngine ? GEngine->GetEngineSubsystem<UCloudUpdateSubsystem>() : nullptr;
}

void UCloudUpdateSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
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