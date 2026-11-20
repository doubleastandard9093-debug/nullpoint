#include "HDPlayerController.h"

bool AHDPlayerController::HandleGameplayCommand(const FString& Verb, const FString& Arg, AActor* Target)
{
	return HandleShipCommand(Verb, Arg, Target)
		|| HandleEconomyCommand(Verb, Arg, Target)
		|| HandleNpcCommand(Verb, Arg, Target)
		|| HandleTerritoryCommand(Verb, Arg, Target);
}

void AHDPlayerController::NotifyLines(const FString& Text)
{
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines);
	for (const FString& L : Lines) Notify(L);
}
