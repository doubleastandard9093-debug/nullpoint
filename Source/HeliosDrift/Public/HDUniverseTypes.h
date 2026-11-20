#pragma once
#include "CoreMinimal.h"
#include "HDUniverseTypes.generated.h"

/** Security bands (doc 15). Secured = Sec Space, Restricted = Rest Space, Borderland = Bor Space, Frontier = The Black. */
UENUM(BlueprintType)
enum class EHDSecurityBand : uint8 { Secured, Restricted, Borderland, Frontier };

UENUM(BlueprintType)
enum class EHDStarClass : uint8 { BlueGiant, BlueWhite, White, YellowWhite, Yellow, Orange, RedDwarf, WhiteDwarf, Neutron, BlackHole };

UENUM(BlueprintType)
enum class EHDBodyType : uint8 { Star, Rocky, Desert, Ice, Water, Lava, Terran, GasGiant, Moon, LargeAsteroid };

UENUM(BlueprintType)
enum class EHDGasDensity : uint8 { Rare, Consistent, Abundant };

UENUM(BlueprintType)
enum class EHDWeather : uint8 { Calm, Stormy, Violent };

/** Landing corridor state (doc 17 §7C.2a). */
UENUM(BlueprintType)
enum class EHDDevState : uint8 { Undeveloped, Administered, Hardened };

/** Station families (doc 19 §7D.0). */
UENUM(BlueprintType)
enum class EHDStationFamily : uint8 { AccordCivic, FactionIndustrial, CorporateModular, PirateKitbash };

/** Long-jump links between (or inside) systems (doc 13). */
UENUM(BlueprintType)
enum class EHDLinkType : uint8 { Gate, LargeGate, Wormhole, StellarAnomaly, GalacticRuin, InstanceLane };

/** Mining deposit fields (doc 16 §7A). Deep-core anomalies are sites. */
UENUM(BlueprintType)
enum class EHDFieldType : uint8 { AsteroidBelt, IceField, GasCloud };

/** Scannable / visitable sites. Encounter anomalies (doc 20), hulks (doc 16), and others. */
UENUM(BlueprintType)
enum class EHDSiteType : uint8 { SignalFire, ContrabandDrift, RogueNest, DeadSignature, Hulk, DeepCore, PirateBase, SentinelVault, DerelictStation };

USTRUCT(BlueprintType)
struct FHDBody
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDBodyType Type = EHDBodyType::Rocky;
	/** Index of the parent body in the system's Bodies array; -1 orbits the star (or is the star). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 Parent = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float OrbitAU = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float RadiusKm = 1000.f;
	/** Surface gravity in g. Generated landable bodies <= 2.0 g, authored <= 5.0 g (doc 17). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Gravity = 0.5f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bLandable = false;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bAtmosphere = false;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bAuthored = false;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDDevState Dev = EHDDevState::Undeveloped;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDWeather Weather = EHDWeather::Calm;
	/** Resource presence 0..1 (gates planet building, doc 19 §7D.1a). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Minerals = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Water = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Fertility = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Volatiles = 0.f;
	/** Position relative to the system origin (star), world cm. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector LocalPos = FVector::ZeroVector;
	/** Rendered radius in world cm (bodies are scaled; see HDUniverseSubsystem). */
	UPROPERTY() double VisualRadiusCm = 0.0;
};

USTRUCT(BlueprintType)
struct FHDStation
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDStationFamily Family = EHDStationFamily::AccordCivic;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Faction;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 Body = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector LocalPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bMarket = true;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bShipyard = false;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bRefinery = false;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bRepair = true;
	/** Gas-giant fuel mining station (doc 19 §7D.1b). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bFuelStation = false;
};

USTRUCT(BlueprintType)
struct FHDJumpLink
{
	GENERATED_BODY()
	/** Shared by both ends of the link. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDLinkType Type = EHDLinkType::Gate;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 From = -1;
	/** -1 for galactic ruins and instance lanes (in-system). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 To = -1;
	/** Must be scanned down before use (wormholes, some anomalies). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bHidden = false;
	/** Scan signature strength 0..1 (higher = easier). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Signature = 1.f;
	/** Wormholes: stable ones never collapse; others live for one week. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bStable = true;
	/** Instance lanes: index of the target site in this system. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 TargetSite = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector LocalPos = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct FHDSite
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDSiteType Type = EHDSiteType::SignalFire;
	/** Hulk class 1..4 (doc 16); 0 for non-hulk sites. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 HulkClass = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Difficulty = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bHidden = true;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Signature = 0.5f;
	/** Reached through an instance jump lane; sits in its own pocket far from the system plane. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bInstanced = false;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector LocalPos = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct FHDTradeLane
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector StartLocal = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector EndLocal = FVector::ZeroVector;
	/** "Accord" for the free NPC network; a company id for player lanes (doc 13). */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Owner;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bNPC = true;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 Toll = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Integrity = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Alignment = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float SignatureDamping = 0.5f;
};

USTRUCT(BlueprintType)
struct FHDField
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDFieldType Type = EHDFieldType::AsteroidBelt;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName PrimaryItem;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName SecondaryItem;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float Richness = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector LocalPos = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 NearBody = -1;
};

USTRUCT(BlueprintType)
struct FHDStarSystem
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 Index = -1;
	/** Stable id, e.g. "NP-0042". */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 Region = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDSecurityBand Band = EHDSecurityBand::Secured;
	/** Galactic position in light-years. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector PositionLy = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDStarClass Star = EHDStarClass::Yellow;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bSettled = false;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Faction;
	/** Gate, large-gate and anomaly links (both directions stored per system). Wormholes are added per week. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") TArray<FHDJumpLink> Links;

	// ---- Detail (generated on first access) ----
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bDetailed = false;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") TArray<FHDBody> Bodies;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") TArray<FHDStation> Stations;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") TArray<FHDSite> Sites;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") TArray<FHDTradeLane> Lanes;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") TArray<FHDField> Fields;
	/** In-system links: instance lanes and galactic ruins, plus local positions for gates. */
	UPROPERTY(BlueprintReadOnly, Category = "Universe") TArray<FHDJumpLink> LocalLinks;
};

USTRUCT(BlueprintType)
struct FHDRegion
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 Index = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDSecurityBand Band = EHDSecurityBand::Secured;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector CenterLy = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FName Faction;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") TArray<int32> Systems;
};

/** Lightweight entry for the galaxy map UI. */
USTRUCT(BlueprintType)
struct FHDMapEntry
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Universe") int32 System = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") EHDSecurityBand Band = EHDSecurityBand::Secured;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") FVector PositionLy = FVector::ZeroVector;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") float DistanceLy = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Universe") bool bSettled = false;
};

namespace HDUniverse
{
	HELIOSDRIFT_API const TCHAR* BandName(EHDSecurityBand B);
	HELIOSDRIFT_API const TCHAR* BandShortName(EHDSecurityBand B);
	HELIOSDRIFT_API const TCHAR* BodyTypeName(EHDBodyType T);
	HELIOSDRIFT_API const TCHAR* LinkTypeName(EHDLinkType T);
	HELIOSDRIFT_API const TCHAR* SiteTypeName(EHDSiteType T);
	HELIOSDRIFT_API const TCHAR* DensityName(EHDGasDensity D);
	HELIOSDRIFT_API const TCHAR* StationFamilyName(EHDStationFamily F);
	HELIOSDRIFT_API FLinearColor BandColor(EHDSecurityBand B);
	/** Deterministic 32-bit mix (splitmix64 finalizer). */
	HELIOSDRIFT_API uint32 Mix(uint32 A, uint32 B);
	HELIOSDRIFT_API float Unit(uint32 A, uint32 B);
}
