#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/PlayerController.h"
#include "CloudUpdateUI.generated.h"

class UCloudUpdateSubsystem;

/** Base for update screens. Bind subsystem delegates in Construct and unbind in Destruct. */
UCLASS(Abstract, Blueprintable)
class CLOUDUPDATE_API UCloudUpdateScreen : public UUserWidget
{
 GENERATED_BODY()
public:
 UFUNCTION(BlueprintPure, Category = "CloudUpdate|UI")
 UCloudUpdateSubsystem* GetUpdateSubsystem() const;
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
