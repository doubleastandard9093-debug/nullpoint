#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "HDGameMode.generated.h"

class AHDSystemDirector;
class AHDShipPawn;
class AHDPlayerState;

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

	// ---- M7 persistence (backend /v1/world/* and /v1/pilots/{id}/state) ----
	/** Seconds between saves of world documents and online pilots. */
	UPROPERTY(EditAnywhere, Category = "Backend") float SaveIntervalSeconds = 60.f;
	/** Save every loaded world document and every loaded online pilot now. */
	void SaveWorldState();
	/** Status line for the `persist` console verb. */
	FString DescribePersistence() const;
	/** Pilot state as JSON (wallet, company, standings, skills, storage, hangar, active fit and hold). */
	static FString BuildPilotJson(const AHDPlayerState* PS, const AHDShipPawn* Ship);
	/** Apply pilot JSON. Fit and hold go to Ship when given. */
	static void ApplyPilotJson(AHDPlayerState* PS, AHDShipPawn* Ship, const FString& Json);

protected:
	UPROPERTY(Transient) TObjectPtr<AHDSystemDirector> Director;
	/** Pending saved system per pilot, applied when the pawn exists. */
	TMap<FString, int32> PendingSystem;
	TMap<FString, FVector> PendingLocal;
	TMap<FString, FRotator> PendingRot;

	void EnsureDirector();
	void PlaceNewShip(AController* C, AHDShipPawn* Ship);

	void LoadWorldState();
	void ApplyWorldDoc(const FString& Kind, const FString& Json);
	FString ExportWorldDoc(const FString& Kind) const;
	void LoadPilot(APlayerController* PC);
	void SavePilot(AController* C);
	/** Kind -> version this server loaded or last wrote. Only kinds present here are ever saved (never clobber unseen state). */
	TMap<FString, int64> WorldVersions;
	/** Kinds another server wrote first (HTTP 409): this server stops saving them. */
	TSet<FString> WorldBlocked;
	TSet<FString> WorldSaving;
	/** Pilots whose saved state was loaded (or confirmed absent): only these are saved. */
	TSet<FString> PilotLoaded;
	/** Pilot JSON waiting for a pawn. */
	TMap<FString, FString> PendingPilotJson;
	FTimerHandle SaveTimer;
	FTimerHandle WorldRetryTimer;
};
