#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HDUniverseTypes.h"
#include "HDTerritory.generated.h"

class AHDShipPawn;
class AHDPlayerState;
class AHDStationActor;
class AHDStructureActor;
class AHDTradeLaneActor;
class AHDNpcController;
struct FHDActiveSystem;

/** Player-built structure types (doc 19 §7D). */
UENUM(BlueprintType)
enum class EHDStructureType : uint8 { Beacon, GunPlacement, DefensePlatform, Habitat, Refinery, Factory, FuelPlatform, Station, Interdictor };

/** Where a structure is anchored (doc 19): belts, surfaces, dead space; fuel platforms in a gas giant's upper atmosphere. */
UENUM(BlueprintType)
enum class EHDAnchor : uint8 { DeadSpace, Belt, Surface, Atmosphere };

/** Who may dock at a player fuel platform / station (doc 19 §7D.1b). */
UENUM(BlueprintType)
enum class EHDAccess : uint8 { Open, Standing, Corp };

/** Siege state of a beacon or station (doc 06). */
UENUM(BlueprintType)
enum class EHDSiegeState : uint8 { Normal, Reinforced, Vulnerable };

/** A company (Syndicate). */
struct FHDCorp
{
	FName Id;
	FString Name;
	FString Ceo;
	TArray<FString> Members;
	TArray<FString> Invites;
	int64 Treasury = 0;
	/** Lifetime toll income (lanes, sites, salvage rights). Never counts toward any Defense Budget. */
	int64 TollIncome = 0;
	float TaxRate = 0.05f;
};

/** One placed structure. Authoritative record; the actor is spawned while its system is active. */
struct FHDStructure
{
	FName Id;
	EHDStructureType Type = EHDStructureType::Beacon;
	EHDAnchor Anchor = EHDAnchor::DeadSpace;
	FName Corp;
	int32 System = -1;
	FVector Local = FVector::ZeroVector;
	int32 Body = -1;
	float Integrity = 1.f;
	float MaxIntegrity = 1.f;
	/** Upkeep stocks (doc 19: upkeep is fuel and ammo). */
	float Fuel = 0.f;
	float Ammo = 0.f;
	double FuelEmptySinceUnix = 0.0;
	bool bDerelict = false;
	bool bActive = true;
	EHDSiegeState Siege = EHDSiegeState::Normal;
	double SiegeUntilUnix = 0.0;
	double LastAttackedUnix = 0.0;
	/** Stations: family and sections (power, hardpoint, support, market, refinery, shipyard, dock). */
	EHDStationFamily Family = EHDStationFamily::CorporateModular;
	TArray<FName> Sections;
	/** HP left in the outermost section: station fights degrade outside-in (doc 19). */
	float SectionHP = 0.f;
	FName StationId;
	/** Fuel platforms: access + toll. */
	EHDAccess Access = EHDAccess::Open;
	int32 Toll = 0;
	float StandingMin = 0.f;
};

/** Tier 5: a hired player defender, escrowed out of the Defense Budget (doc 15). */
struct FHDHire
{
	FString Pilot;
	int64 RatePerDay = 0;
	double UntilUnix = 0.0;
	int64 PaidOut = 0;
};

/** An announced claim on a system (doc 06 / 15). */
struct FHDClaim
{
	FName Id;
	FName Corp;
	int32 System = -1;
	EHDSecurityBand Band = EHDSecurityBand::Borderland;
	FName Beacon;
	/** Slow inputs, kept as exponentially windowed sums: 30-day industrial output and trade volume (credits). */
	double Output30 = 0.0;
	double Trade30 = 0.0;
	/** 14-day registered residency: pilot -> last seen (unix). */
	TMap<FString, double> Residents;
	/** Hourly raw-DB samples for the anti-spike window (up to 168). */
	TArray<float> DbSamples;
	double LastSampleUnix = 0.0;
	/** Spending per tier (cr/day): 0 guns, 1 platforms, 2 capital stipend, 3 NPC-flown, 4 hired players. */
	float Alloc[5] = { 0.f, 0.f, 0.f, 0.f, 0.f };
	TArray<FHDHire> Hires;
	/** Kill-on-sight corps and recent aggressors (pilot -> until). */
	TArray<FName> Kos;
	TMap<FString, double> Aggressors;
	/** Vulnerability hour (UTC) set in advance for the final battle (doc 06). */
	int32 VulnHourUtc = 20;
	/** Hire vetting: standing minimum only (Accord standing). No refusal lists. */
	float HireMinStanding = -10.f;
	int32 SiteTollPct = 10;
	FName WingFaction;
	TArray<TWeakObjectPtr<AHDNpcController>> Defenders;
	float DefenderTimer = 0.f;
	bool bUndefended = true;
	bool bThinGarrison = false;
};

/** Planet colony (doc 19 §7D.1a). */
struct FHDColony
{
	FName Id;
	FName Corp;
	FString Owner;
	int32 System = -1;
	int32 Body = -1;
	FString Name;
	TMap<FName, int32> Buildings;
	float Population = 100.f;
	/** Governor mode (Direct when GovernorQuality <= 0). */
	FString Governor;
	float GovernorQuality = 0.f;
	int64 GovernorSalary = 0;
	FName Focus = TEXT("Balanced");
	int64 WeeklyBudget = 0;
	TArray<FName> Queue;
	double NextWeekUnix = 0.0;
	float BlockadeHours = 0.f;
	float HoursThisWeek = 0.f;
	double LastOutput = 0.0;
};

/** Player-built trade lane (doc 13). */
struct FHDPlayerLane
{
	FHDTradeLane Lane;
	int32 System = -1;
	bool bSuppressed = false;
	double SuppressPaidUntil = 0.0;
};

/**
 * Territory (doc 06, 15, 19): companies, claims with a Defense Budget (sublinear in 30-day output, 14-day residency
 * and trade; min of 24 h / 7-day averages; band ceilings), the five-tier defense ladder with the 50% hired-player cap,
 * escrowed hires, upkeep off the top, structures with fuel/ammo upkeep and decay into derelicts, sieges with
 * reinforcement and a fixed vulnerability window, player stations (modular / kitbash stress), gas-giant fuel
 * platforms with access rules, player lanes with tolls (never DB), interdiction and suppression, and colonies with
 * governors. Lives on the game instance; ticked by the system director.
 */
UCLASS()
class HELIOSDRIFT_API UHDTerritorySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	static UHDTerritorySubsystem* Get(const UObject* WorldContext);
	void Tick(float Dt, UWorld* World);

	// ---- Companies ----
	TMap<FName, FHDCorp> Corps;
	FHDCorp* CorpOf(const AHDPlayerState* PS);
	bool IsCeo(const AHDPlayerState* PS) const;

	// ---- Claims and the Defense Budget ----
	TMap<FName, FHDClaim> Claims;
	FHDClaim* ClaimInSystem(int32 System);
	const FHDClaim* ClaimInSystem(int32 System) const;
	/** Raw DB (cr/day) from the slow inputs, before the anti-spike window and the band ceiling. */
	float RawDefenseBudget(const FHDClaim& C) const;
	/** DB = min(24 h avg, 7-day avg), capped by band. */
	float DefenseBudget(const FHDClaim& C) const;
	static float BandCeiling(EHDSecurityBand Band);
	int32 ResidentCount(const FHDClaim& C) const;
	int32 Berths(const FHDClaim& C) const;
	/** Structure upkeep in cr/day, paid off the top before any hire. */
	float UpkeepPerDay(const FHDClaim& C) const;
	/** Validate and set a tier allocation. Tier index 0..4. */
	bool SetAllocation(FHDClaim& C, int32 Tier, float CrPerDay, FString& OutWhy);
	bool Hire(FHDClaim& C, AHDPlayerState* Pilot, int64 RatePerDay, float Days, FString& OutWhy);
	FString DescribeClaim(const FHDClaim& C) const;

	// Inputs from other systems (taxes and tolls do not feed DB).
	void AddIndustrialOutput(int32 System, double Credits);
	void AddTrade(int32 System, double Credits);
	void AddTollIncome(const FName& Owner, int64 Credits);

	// ---- Structures ----
	TMap<FName, FHDStructure> Structures;
	FHDStructure* FindStructure(const FName& Id) { return Structures.Find(Id); }
	/** Anchor a structure from a kit in the pilot's hold. */
	bool Anchor(AHDShipPawn* Ship, EHDStructureType Type, FString& OutMsg);
	static FName KitFor(EHDStructureType Type);
	static const TCHAR* TypeName(EHDStructureType Type);
	static float MaxIntegrityFor(EHDStructureType Type);
	static float UpkeepCredits(EHDStructureType Type);
	static float UpkeepFuelPerDay(const FHDStructure& S);
	/** Server: structure damage (legality, reinforcement, outside-in station sections). Returns damage taken. */
	float DamageStructure(AHDStructureActor* Actor, float Amount, AActor* Causer);
	/** Is this pilot/NPC a hostile to the structure's owner right now? */
	bool IsHostileToCorp(const FName& Corp, const AHDShipPawn* Ship) const;
	/** Station services from its sections, efficiency after kitbash routing loss. */
	void ApplyStationSections(FHDStructure& S);
	static float RoutingEfficiency(const FHDStructure& S);
	int32 StandardSections(const FHDStructure& S) const { return S.Family == EHDStationFamily::CorporateModular ? 8 : 6; }

	// ---- Docking access (fuel platforms / player stations) ----
	bool CanDock(const AHDStationActor* St, AHDShipPawn* Ship, int32& OutToll, FString& OutWhy) const;

	// ---- Lanes and interdiction ----
	TArray<FHDPlayerLane> Lanes;
	TMap<FString, FVector> LaneStarts;
	bool Interdict(AHDShipPawn* By, FString& OutMsg);
	TMap<int32, double> LaneCutUntil;

	// ---- Pirate bases taken over ----
	TMap<FName, FName> CapturedStations;

	// ---- Colonies ----
	TMap<FName, FHDColony> Colonies;
	static FName ColonyKey(int32 System, int32 Body);
	/** Building gate: world type plus resources actually present (doc 19). */
	static bool CanBuild(const FHDBody& B, const FName& Building, FString& OutWhy);
	static int64 BuildingCost(const FName& Building);
	static const TArray<FName>& AllBuildings();
	/** Governor's proposed weekly queue for its focus and quality (editable). */
	TArray<FName> ProposeQueue(const FHDColony& C, const FHDBody& B, FRandomStream& R) const;
	/** Run the colony week: build queue within budget, grow, produce. Returns a report. */
	FString RunColonyWeek(UWorld* World, FHDColony& C);
	double ColonyDailyOutput(const FHDColony& C, const FHDBody& B) const;
	int32 ColonyResidents(int32 System) const;

	// ---- World presence ----
	/** Spawn structure actors, player lanes and station proxies for a system that just activated. */
	void SpawnForSystem(FHDActiveSystem& A);
	TMap<FName, TWeakObjectPtr<AHDStructureActor>> StructureActors;
	TMap<FName, TWeakObjectPtr<AHDStationActor>> StationActors;
	TMap<FString, int32> PilotSystem;

	// ---- Persistence (M7) ----
	FString ExportJson() const;
	void ImportJson(const FString& Json);

private:
	void TickClaim(UWorld* World, FHDClaim& C, float Hours);
	void TickStructures(UWorld* World, float Dt);
	void TickDefenders(UWorld* World, FHDClaim& C, float Dt);
	void SpawnStructureActor(UWorld* World, FHDStructure& S);
	void SpawnStationProxy(UWorld* World, FHDStructure& S);
	void Derelict(UWorld* World, FHDStructure& S, const TCHAR* Why);
	void DropSalvage(UWorld* World, const FHDStructure& S, float Fraction);
	void AnnounceClaims(UWorld* World);
	float HourTimer = 0.f;
	float SecTimer = 0.f;
	int32 NextId = 1;
};
