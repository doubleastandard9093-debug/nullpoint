#pragma once
#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "HDUniverseTypes.h"
#include "HDNpcController.h"
#include "HDNpcSubsystem.generated.h"

class AHDShipPawn;
class AHDSiteActor;
class AHDPlayerState;
struct FHDActiveSystem;

/** A live encounter anomaly (doc 20) being tracked for its outcome. */
struct FHDEncounter
{
	TWeakObjectPtr<AHDSiteActor> Site;
	EHDSiteType Type = EHDSiteType::SignalFire;
	EHDSecurityBand Band = EHDSecurityBand::Secured;
	int32 System = -1;
	TArray<TWeakObjectPtr<AHDNpcController>> Hostiles;
	TArray<TWeakObjectPtr<AHDNpcController>> Protect;
	bool bResolved = false;
};

/** Shadow contract: paid in standing and information, never cargo (doc 18). */
struct FHDShadowContract
{
	FName SiteId;
	int32 System = -1;
	FString SiteName;
	double ExpiresUnix = 0.0;
};

/**
 * Server NPC layer (doc 15, 18, 20): spawns and culls ambient traffic and antagonists with economic spawn pressure,
 * populates encounter anomalies and guarded sites, decides PVP legality by band, flags criminals, dispatches the
 * Accord security response, runs contraband scans, and pays out standing / bounties on NPC deaths.
 */
UCLASS()
class HELIOSDRIFT_API UHDNpcSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	static UHDNpcSubsystem* Get(const UObject* WorldContext);

	/** Server: spawn an NPC ship in a system at a world location. */
	AHDNpcController* SpawnNpc(int32 System, const FVector& WorldLoc, EHDNpcPop Pop, FName Hull, FName Faction, float DamageScale = 1.f);
	/** Hull choice per population and band. */
	static FName PickHull(EHDNpcPop Pop, EHDSecurityBand Band, FRandomStream& R, bool bLeader = false);

	/** Called by the system director for each active system. */
	void TickSystem(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Ships, float Dt);
	/** Spawn the contents of an encounter / guarded site. Returns spawned actors (for site cleanup). */
	void SpawnSiteEncounter(FHDActiveSystem& A, AHDSiteActor* Site, FRandomStream& R, TArray<AActor*>& OutActors);

	/** Rogue / pirate pressure for a system: a readout of regional wealth and traffic (doc 18). 0.5 .. 2.5. */
	float GetSpawnPressure(const FHDActiveSystem& A) const;

	// ---- Security (doc 15) ----
	EHDSecurityBand BandOf(int32 System) const;
	/** Server, from TakeDamage: false blocks the damage (illegal PVP). May flag the attacker a criminal. */
	bool CheckAttack(AHDShipPawn* Victim, AController* Inst, AActor* Causer);
	/** Flag a pilot criminal; in Secured / Restricted space the Accord responds. */
	void ReportCrime(AHDShipPawn* Criminal, const FString& What, float Seconds);
	/** Territory hook (M6): is this point inside an announced claim? */
	bool IsInClaim(int32 System, const FVector& WorldLoc) const;

	// ---- Shadow contracts ----
	void OnShadowRevealed(AHDNpcController* Shadow, AHDShipPawn* Pilot);
	TMap<FString, FHDShadowContract> ShadowContracts;

	TArray<FHDEncounter> Encounters;

	/** Tuning: ambient NPC cap per system. */
	int32 MaxAmbientPerSystem = 18;

private:
	void HandleShipDestroyed(AHDShipPawn* Victim, AController* Killer);
	void TickEncounters(FHDActiveSystem& A);
	void ContrabandScans(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Ships);
	void SpawnAmbient(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Pilots, EHDNpcPop Pop, FRandomStream& R);
	void DispatchResponse(TWeakObjectPtr<AHDShipPawn> Criminal, int32 Count);
	FDelegateHandle DeathHandle;
	TMap<FString, double> LastScanUnix;
	TMap<FString, double> LastResponseUnix;
	TMap<FString, double> LastBlockedNotify;
	float ScanTimer = 0.f;
};
