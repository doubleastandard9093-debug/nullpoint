#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HDUniverseTypes.h"
#include "HDSystemDirector.generated.h"

class AHDShipPawn;
class AHDSpaceObject;
class AHDCelestialBody;
class AHDStationActor;
class AHDJumpPointActor;
class AHDSiteActor;
class AHDTradeLaneActor;
class AHDAsteroidActor;
class UHDUniverseSubsystem;

/** Streamed deposit field: rocks exist only while a ship is near. */
struct FHDFieldStream
{
	FHDField Field;
	FVector World = FVector::ZeroVector;
	TArray<TWeakObjectPtr<AHDAsteroidActor>> Rocks;
	bool bStreamedIn = false;
	float EmptyTime = 0.f;
	/** Rocks mined out last time; respawn is throttled by regional consumption (doc 16). */
	float RespawnDebt = 0.f;
};

/** One star system currently simulated by this server, placed in a world slot. */
struct FHDActiveSystem
{
	int32 System = -1;
	int32 Slot = -1;
	FVector Origin = FVector::ZeroVector;
	EHDSecurityBand Band = EHDSecurityBand::Secured;
	int32 Region = -1;
	TArray<TWeakObjectPtr<AActor>> Persistent;
	TArray<TWeakObjectPtr<AHDCelestialBody>> Bodies;
	TArray<TWeakObjectPtr<AHDStationActor>> Stations;
	TArray<TWeakObjectPtr<AHDJumpPointActor>> JumpPoints;
	TArray<TWeakObjectPtr<AHDSiteActor>> Sites;
	TArray<TWeakObjectPtr<AHDTradeLaneActor>> Lanes;
	TArray<FHDFieldStream> Fields;
	/** Streamed / ambient actors (NPCs, wrecks, crates) to clean up with the system. */
	TArray<TWeakObjectPtr<AActor>> Transient;
	float EmptyTime = 0.f;
	float AmbientTimer = 10.f;
	int32 Pilots = 0;
};

/**
 * Server-only runtime spawner (doc 08/13). Activates star systems on demand, places each in a world slot
 * (several systems share one server, far apart, inside Large World Coordinate limits), spawns the
 * system's bodies, stations, gates, wormholes, sites and lanes, and streams rocks and site contents
 * in and out around ships. Systems with no pilots are torn down after a grace period.
 * No .umap content is needed: the GameMode spawns this into any empty level.
 */
UCLASS(NotPlaceable)
class HELIOSDRIFT_API AHDSystemDirector : public AActor
{
	GENERATED_BODY()
public:
	AHDSystemDirector();
	virtual void Tick(float DeltaSeconds) override;

	static AHDSystemDirector* Get(const UObject* WorldContext);

	/** Activate (spawn) a system if needed. Returns false if all slots are busy. */
	bool EnsureActive(int32 System);
	bool IsActive(int32 System) const { return FindActive(System) != nullptr; }
	FVector GetOrigin(int32 System) const;
	const FHDActiveSystem* FindActive(int32 System) const;
	FHDActiveSystem* FindActive(int32 System);
	const TArray<FHDActiveSystem>& GetActiveSystems() const { return Active; }

	/** Arrival point near the system's main station (or the star if unsettled), in system-local cm. */
	FVector GetArrivalLocal(int32 System) const;
	/** Moves a ship into a system at a local position, activating the system as needed. */
	bool PlaceShip(AHDShipPawn* Ship, int32 System, const FVector& LocalPos, const FRotator& Rot);
	/** Arrival at the far end of a link (the reciprocal jump point), local cm. */
	FVector GetLinkArrivalLocal(int32 System, const FName& LinkId) const;

	TArray<AHDShipPawn*> GetShipsInSystem(int32 System) const;
	AHDJumpPointActor* FindJumpPoint(int32 System, const FName& LinkId) const;
	AHDStationActor* FindStation(int32 System, const FName& StationId) const;
	AHDSiteActor* FindSite(int32 System, int32 SiteIndex) const;

	/** Register a streamed/ambient actor so it is cleaned up with the system. */
	void AddTransient(int32 System, AActor* Actor);

	/** Destroy and respawn all content of a system (e.g. after a weekly re-roll). */
	void RebuildSystem(int32 System);

	// Tuning
	UPROPERTY(EditAnywhere, Category = "Director") double SlotSpacing = 2.0e12;
	UPROPERTY(EditAnywhere, Category = "Director") int32 SlotGrid = 4;
	UPROPERTY(EditAnywhere, Category = "Director") double SlotZ = 1.0e11;
	UPROPERTY(EditAnywhere, Category = "Director") float EmptySystemGrace = 120.f;
	UPROPERTY(EditAnywhere, Category = "Director") double FieldStreamRadius = 1.0e8; // 1,000 km
	UPROPERTY(EditAnywhere, Category = "Director") double SiteStreamRadius = 5.0e7;  // 500 km
	UPROPERTY(EditAnywhere, Category = "Director") float StreamOutGrace = 60.f;
	UPROPERTY(EditAnywhere, Category = "Director") int32 RocksPerField = 24;
	/** Spawn the three M1 target drones near the start station. */
	UPROPERTY(EditAnywhere, Category = "Director") bool bSpawnTestDrones = true;

	int64 LastWeek = MIN_int64;

protected:
	virtual void BeginPlay() override;

private:
	void SpawnSystemContent(FHDActiveSystem& A);
	void Deactivate(int32 ActiveIndex);
	void StreamFields(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Ships, float Dt);
	void StreamSites(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Ships, float Dt);
	void SpawnFieldRocks(FHDActiveSystem& A, FHDFieldStream& F);
	void SpawnSiteContents(FHDActiveSystem& A, AHDSiteActor* Site);
	void ClearSiteContents(AHDSiteActor* Site);
	void TickAmbient(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Ships, float Dt);
	int32 FreeSlot() const;
	FVector SlotOrigin(int32 Slot) const;
	UHDUniverseSubsystem* Universe() const;

	TArray<FHDActiveSystem> Active;
	float WeekCheck = 0.f;
};
