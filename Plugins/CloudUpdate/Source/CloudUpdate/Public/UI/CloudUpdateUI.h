#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "CloudUpdateTypes.h"
#include "CloudUpdateUI.generated.h"

class UCloudUpdateSubsystem;

/** Shared update screen behavior; named designer widgets are optional and delegates are owned by this view. */
UCLASS(Abstract, Blueprintable)
class CLOUDUPDATE_API UCloudUpdateScreen : public UUserWidget
{
 GENERATED_BODY()
public:
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|UI")
 UCloudUpdateSubsystem* GetUpdateSubsystem() const;
protected:
 virtual void NativeConstruct() override;
 virtual void NativeDestruct() override;
 virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
private:
#if WITH_DEV_AUTOMATION_TESTS
 friend class FCloudUpdateScreenTest;
#endif
 UPROPERTY(Transient)
 TObjectPtr<UCloudUpdateSubsystem> BoundSubsystem;
 bool bChecking = false;
 bool bApplying = false;
 bool bCancelRequested = false;
 float RefreshElapsed = 0.f;
 void RefreshVersions();
 void RefreshButtons();
 void SetStatus(const FText& Text, bool bError = false);
 void UnbindSubsystem();
 UFUNCTION() void CheckClicked();
 UFUNCTION() void UpdateClicked();
 UFUNCTION() void CancelClicked();
 UFUNCTION() void CloseClicked();
 /** 一键修复：先做完整性检查，发现问题再自动修复（对应设计师里的 RepairButton，可缺省） */
 UFUNCTION() void RepairClicked();
 UFUNCTION() void RepairFinished(bool bSuccess, int32 RepairedCount, const FString& Message);
 UFUNCTION() void PlanReady(bool bSuccess, const FCloudUpdatePlan& Plan, const FString& Message);
 UFUNCTION() void DownloadChanged(const FCloudDownloadProgressInfo& Progress);
 UFUNCTION() void MergeChanged(const FCloudMergeProgressInfo& Progress);
 UFUNCTION() void UpdateFinished(bool bSuccess, bool bRestartRequired, const FString& VersionId, const FString& Message);
};

/** Local UI controller; no character or gameplay input is needed for the update level. */
UCLASS()
class CLOUDUPDATE_API ACloudUpdateUIPlayerController : public APlayerController
{
 GENERATED_BODY()
protected:
 virtual void BeginPlay() override;
 virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
 UPROPERTY(Transient)
 TObjectPtr<UUserWidget> UpdateScreen;
};

/** Set UpdateWidgetClass on a Blueprint child and assign that GameMode to the update map. */
UCLASS(Blueprintable)
class CLOUDUPDATE_API ACloudUpdateUIGameMode : public AGameModeBase
{
 GENERATED_BODY()
public:
 ACloudUpdateUIGameMode();
 UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "CloudUpdate|UI")
 TSubclassOf<UCloudUpdateScreen> UpdateWidgetClass;
};
