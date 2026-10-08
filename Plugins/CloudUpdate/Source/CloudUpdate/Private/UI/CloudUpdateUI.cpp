#include "UI/CloudUpdateUI.h"
#include "CloudUpdateSubsystem.h"
#include "CloudUpdate.h"
#include "Engine/World.h"

UCloudUpdateSubsystem* UCloudUpdateScreen::GetUpdateSubsystem() const
{
 return UCloudUpdateSubsystem::GetCloudUpdateSubsystem();
}

ACloudUpdateUIGameMode::ACloudUpdateUIGameMode()
{
 PlayerControllerClass = ACloudUpdateUIPlayerController::StaticClass();
 DefaultPawnClass = nullptr;
 HUDClass = nullptr;
 bStartPlayersAsSpectators = true;
}

void ACloudUpdateUIPlayerController::BeginPlay()
{
 Super::BeginPlay();
 if (!IsLocalController()) return;
 const ACloudUpdateUIGameMode* Mode = GetWorld()->GetAuthGameMode<ACloudUpdateUIGameMode>();
 if (!Mode || !Mode->UpdateWidgetClass)
 {
  UE_LOG(LogCloudUpdate, Warning, TEXT("Update map: assign UpdateWidgetClass on the CloudUpdate UI GameMode."));
  return;
 }
 UpdateScreen = CreateWidget<UCloudUpdateScreen>(this, Mode->UpdateWidgetClass);
 if (UpdateScreen)
 {
  UpdateScreen->AddToViewport();
  bShowMouseCursor = true;
  FInputModeUIOnly InputMode;
  InputMode.SetWidgetToFocus(UpdateScreen->TakeWidget());
  SetInputMode(InputMode);
 }
}

void ACloudUpdateUIPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
 if (UpdateScreen) { UpdateScreen->RemoveFromParent(); UpdateScreen = nullptr; }
 SetInputMode(FInputModeGameOnly());
 bShowMouseCursor = false;
 Super::EndPlay(EndPlayReason);
}
