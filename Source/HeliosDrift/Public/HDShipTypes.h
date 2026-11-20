#pragma once
#include "CoreMinimal.h"
#include "HDShipTypes.generated.h"

/** Hull size classes (doc 13/14). Fighters through battleships use gates; capital-size hulls jump on their own drives. */
UENUM(BlueprintType)
enum class EHDHullClass : uint8
{
	Fighter, Corvette, Frigate, Destroyer, Cruiser, Battlecruiser, Battleship,
	Capital, Supercapital, JumpFreighter, CapitalIndustrial
};

UENUM(BlueprintType)
enum class EHDHullRole : uint8 { Combat, Miner, Hauler, Extractor, Surveyor, Salvager, Interdictor, CovertOps, IndustrialCommand, Explorer, Carrier, Dreadnought, Freighter };

/** Supply split (doc 14): NPC baseline bare hulls, faction-only hulls, player-built advanced hulls. */
UENUM(BlueprintType)
enum class EHDSupplyTier : uint8 { Baseline, Faction, Advanced };

namespace HDShips
{
	/** Faction id key: "Halvar Combine" -> halvar_combine (stations carry display names, catalog/standing use keys). */
	HELIOSDRIFT_API FName FactionKey(const FName& Faction);
	/** Capital-size: capitals, supercapitals, jump freighters, capital industrials. */
	inline bool IsCapitalSize(EHDHullClass C) { return C >= EHDHullClass::Capital; }
	/** Needs gates, lanes, wormholes or anomalies to leave a system. */
	inline bool UsesGates(EHDHullClass C) { return !IsCapitalSize(C); }
	/** Jump freighters and capital industrials count 10% of distance for drive strain (doc 13). */
	inline bool IsLogisticsCapital(EHDHullClass C) { return C == EHDHullClass::JumpFreighter || C == EHDHullClass::CapitalIndustrial; }
	/** Base self-jump range in ly (doc 13). 0 for gate-bound hulls. */
	inline float BaseJumpRangeLy(EHDHullClass C)
	{
		switch (C)
		{
		case EHDHullClass::Capital: return 3.5f;
		case EHDHullClass::Supercapital: return 3.0f;
		case EHDHullClass::JumpFreighter: case EHDHullClass::CapitalIndustrial: return 5.0f;
		default: return 0.f;
		}
	}
	/** Jump fuel (t) per light-year before skills (doc 13). */
	inline float JumpFuelPerLy(EHDHullClass C)
	{
		switch (C)
		{
		case EHDHullClass::Capital: case EHDHullClass::Supercapital: return 30.f;
		case EHDHullClass::JumpFreighter: case EHDHullClass::CapitalIndustrial: return 90.f;
		default: return 0.f;
		}
	}
	HELIOSDRIFT_API const TCHAR* ClassName(EHDHullClass C);
	HELIOSDRIFT_API const TCHAR* RoleName(EHDHullRole R);
}
