#include "HDPlayerController.h"

bool AHDPlayerController::HandleNpcCommand(const FString& Verb, const FString& Arg, AActor* Target)
{
	return false; // M5
}

void AHDPlayerController::OnWreckLooted(AHDWreckActor* Wreck)
{
	Notify(TEXT("Looted another pilot's wreck.")); // M5: criminal flag in lawful space
}
