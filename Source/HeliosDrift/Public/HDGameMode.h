#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "HDGameMode.generated.h"

class AHDSystemDirector;
class AHDShipPawn;

/**
 * Server-authoritative rules. Spawns the system director into whatever level is loaded (an empty level
 * is fine: no .umap content is required), places pilots in their saved or start system, and saves on logout.
 */
UCLASS()
class HELIOSDRIFT_API AHDGameMode : public AGameModeBase
{
	GENERATED_BODY()
public:
	AHDGameMode();
	virtual void StartPlay() override;
	virtual void PostLogin(APlayerController* NewPlayer) override;
	virtual void Logout(AController* Exiting) override;
	virtual void RestartPlayer(AController* NewPlayer) override;

	/** Legacy single-zone id (from -SystemId= cmdline). If it names a universe system, pilots start there. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Zone")
	FName SystemId = TEXT("sys-0001");

	static FString PilotIdFor(const AController* C);

	AHDSystemDirector* GetDirector() const { return Director; }
	/** System a new pilot starts in. */
	int32 GetStartSystemFor(const AController* C) const;

protected:
	UPROPERTY(Transient) TObjectPtr<AHDSystemDirector> Director;
	/** Pending saved system per pilot, applied when the pawn exists. */
	TMap<FString, int32> PendingSystem;
	TMap<FString, FVector> PendingLocal;
	TMap<FString, FRotator> PendingRot;

	void EnsureDirector();
	void PlaceNewShip(AController* C, AHDShipPawn* Ship);
};
