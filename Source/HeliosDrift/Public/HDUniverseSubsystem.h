#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HDUniverseTypes.h"
#include "HDUniverseSubsystem.generated.h"

/**
 * Seeded galaxy (doc 13). Runs on server and every client: the same seed produces the same galaxy,
 * so the galaxy map works client-side with no replication. Systems are generated in two passes:
 * the skeleton (positions, regions, bands, gate network, anomalies) at startup, and the detail
 * (bodies, stations, fields, sites, lanes) lazily on first access.
 *
 * Config: [HeliosDrift.Universe] in DefaultGame.ini. Command line: -UniverseSeed=1234 -SystemCount=800
 */
UCLASS()
class HELIOSDRIFT_API UHDUniverseSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	static UHDUniverseSubsystem* Get(const UObject* WorldContext);

	/** Regenerate everything (e.g. client got a different seed from the server). */
	void Generate(int32 InSeed, int32 InSystemCount);

	UFUNCTION(BlueprintPure, Category = "Universe") int32 GetSeed() const { return Seed; }
	UFUNCTION(BlueprintPure, Category = "Universe") int32 NumSystems() const { return Systems.Num(); }
	UFUNCTION(BlueprintPure, Category = "Universe") bool IsValidSystem(int32 Index) const { return Systems.IsValidIndex(Index); }

	/** Skeleton only (no detail generated). */
	const FHDStarSystem& GetSystemSkeleton(int32 Index) const;
	/** Full system: generates detail on first call. Index must be valid. */
	const FHDStarSystem& GetSystem(int32 Index);
	const TArray<FHDRegion>& GetRegions() const { return Regions; }

	/** Accepts "NP-0042", "42", or a system name (case-insensitive). Returns -1 if not found. */
	int32 FindSystem(const FString& IdOrName) const;
	/** A Secured, settled system near the core: the new-pilot start. */
	int32 GetStartSystem() const { return StartSystem; }

	/** All outgoing links for a system this week: gates, large gates, anomalies, ruins, plus this week's wormholes. */
	TArray<FHDJumpLink> GetLinks(int32 Index);

	UFUNCTION(BlueprintPure, Category = "Universe") float DistanceLy(int32 A, int32 B) const;

	// ---- Galaxy map API ----
	/** Systems within RadiusLy of Center, sorted by distance. */
	TArray<FHDMapEntry> GetSystemsNear(int32 Center, float RadiusLy, int32 MaxResults = 64) const;
	/** Gate-network route for small ships (fighters through battleships). Uses gates, large gates and (if bUseWormholes) this week's wormholes. Empty if unreachable. */
	TArray<int32> FindGateRoute(int32 From, int32 To, bool bUseWormholes = false);
	/** Capital self-jump route: hops of at most RangeLy, fewest jumps. Empty if unreachable within MaxHops. */
	TArray<int32> FindJumpRoute(int32 From, int32 To, float RangeLy, int32 MaxHops = 64) const;

	// ---- Time / weekly re-rolls ----
	/** Week index used for gas-giant density and wormhole re-rolls. Override with HDSetWeek for testing. */
	int64 GetWeekIndex() const;
	void SetWeekOverride(int64 Week) { WeekOverride = Week; WormholeWeek = MIN_int64; }
	/** Gas-giant cloud density this week (doc 19 §7D.1b): Rare 30% / Consistent 50% / Abundant 20% by default. */
	EHDGasDensity GetGasDensity(int32 SystemIndex, int32 BodyIndex) const;
	EHDGasDensity GetGasDensityForWeek(int32 SystemIndex, int32 BodyIndex, int64 Week) const;
	static float DensityYieldMultiplier(EHDGasDensity D);

	// ---- Scale ----
	/** World cm per AU inside a system. */
	double AUcm = 1.0e10;
	/** Planet/moon rendered radius = real radius * BodyScale. */
	double BodyScale = 0.25;
	/** Star rendered radius = real radius * StarScale. */
	double StarScale = 0.02;
	/** Instanced pockets sit this far "above" the system plane (cm). */
	double InstancePocketCm = 2.0e11;

	float OddsRare = 0.30f;
	float OddsConsistent = 0.50f;

private:
	void GenerateSkeleton();
	void GenerateDetail(FHDStarSystem& S);
	void BuildGrid();
	void QueryGrid(const FVector& P, float Radius, TArray<int32>& Out) const;
	void RefreshWormholes();
	FString MakeName(FRandomStream& R) const;
	void AddLink(int32 A, int32 B, EHDLinkType Type, const FName& Id, bool bHidden, float Signature, bool bStable, bool bWeekly);

	int32 Seed = 1337;
	int32 SystemCount = 5000;
	float GalaxyRadiusLy = 200.f;
	int32 StartSystem = 0;
	int64 WeekOverride = MIN_int64;
	int64 WeekOffsetSeconds = 4 * 86400; // weeks start Monday 00:00 UTC

	UPROPERTY() TArray<FHDStarSystem> Systems;
	UPROPERTY() TArray<FHDRegion> Regions;
	/** This week's transient wormholes, per system. */
	TMap<int32, TArray<FHDJumpLink>> WeeklyWormholes;
	int64 WormholeWeek = MIN_int64;
	TMap<FIntVector, TArray<int32>> Grid;
	float GridCell = 6.f;
};
