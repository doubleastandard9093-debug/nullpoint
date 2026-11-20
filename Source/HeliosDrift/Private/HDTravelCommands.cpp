// Travel verbs (milestone 2): cruise, dock, land, lanes, gate / wormhole / anomaly / capital jumps, scanning, fuel.
#include "HDPlayerController.h"
#include "HDShipPawn.h"
#include "HDShipTypes.h"
#include "HDNavComponent.h"
#include "HDShipMovementComponent.h"
#include "HDPlayerState.h"
#include "HDSystemDirector.h"
#include "HDUniverseSubsystem.h"
#include "HDWorldActors.h"
#include "Engine/World.h"

#if !UE_BUILD_SHIPPING
namespace
{
	bool ParseHullClass(const FString& S, EHDHullClass& Out)
	{
		for (int32 I = 0; I <= (int32)EHDHullClass::CapitalIndustrial; ++I)
		{
			const EHDHullClass C = (EHDHullClass)I;
			FString N = HDShips::ClassName(C);
			N.ReplaceInline(TEXT(" "), TEXT(""));
			FString A = S;
			A.ReplaceInline(TEXT(" "), TEXT(""));
			A.ReplaceInline(TEXT("_"), TEXT(""));
			if (N.Equals(A, ESearchCase::IgnoreCase)) { Out = C; return true; }
		}
		return false;
	}
}

/** Server: apply a hull class and its default fuel tanks (M3 fitting overrides these). */
static void ApplyHullClassDefaults(AHDShipPawn* Ship, EHDHullClass C)
{
	Ship->HullClass = C;
	if (!Ship->Nav) return;
	switch (C)
	{
	case EHDHullClass::Capital: Ship->Nav->MaxJumpFuel = 2000.f; break;
	case EHDHullClass::Supercapital: Ship->Nav->MaxJumpFuel = 3000.f; break;
	case EHDHullClass::JumpFreighter: case EHDHullClass::CapitalIndustrial: Ship->Nav->MaxJumpFuel = 4000.f; break;
	default: Ship->Nav->MaxJumpFuel = 0.f; break;
	}
	Ship->Nav->JumpFuel = Ship->Nav->MaxJumpFuel;
	Ship->Nav->MaxShipFuel = HDShips::IsCapitalSize(C) ? 400.f : 60.f;
	Ship->Nav->ShipFuel = Ship->Nav->MaxShipFuel;
}
#endif

bool AHDPlayerController::HandleTravelCommand(const FString& Verb, const FString& Arg, AActor* Target)
{
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	AHDPlayerState* PS = GetPlayerState<AHDPlayerState>();
	UHDNavComponent* Nav = Ship ? Ship->Nav.Get() : nullptr;

	if (Verb == TEXT("cruise")) { if (Nav) Nav->RequestCruise(); return true; }
	if (Verb == TEXT("interact")) { if (Nav) Nav->RequestInteract(Target); return true; }
	if (Verb == TEXT("dock") || Verb == TEXT("undock")) { if (Nav) Nav->RequestDock(Target); return true; }
	if (Verb == TEXT("land") || Verb == TEXT("takeoff")) { if (Nav) Nav->RequestLand(); return true; }
	if (Verb == TEXT("jump")) { if (Nav) Nav->RequestJump(Target); return true; }
	if (Verb == TEXT("scan")) { if (Nav) Nav->RequestScan(); return true; }
	if (Verb == TEXT("lane"))
	{
		if (!Nav) return true;
		AHDTradeLaneActor* Lane = Cast<AHDTradeLaneActor>(Target);
		if (!Lane)
		{
			FString P;
			Lane = Cast<AHDTradeLaneActor>(Nav->FindInteractable(P));
		}
		if (Lane) Nav->RequestLane(Lane); else Notify(TEXT("No lane ring in range"));
		return true;
	}
	if (Verb == TEXT("jumptarget"))
	{
		if (!Nav || !U) return true;
		const int32 Sys = U->FindSystem(Arg);
		if (Sys < 0) { Nav->SetJumpTarget(-1); Notify(TEXT("Jump target cleared")); return true; }
		Nav->SetJumpTarget(Sys);
		if (HDShips::IsCapitalSize(Ship->HullClass))
		{
			const float Ly = U->DistanceLy(Ship->SystemIndex, Sys);
			Notify(FString::Printf(TEXT("Jump target %s: %.2f ly (range %.2f), fuel %.0f t"), *U->GetSystemSkeleton(Sys).Name, Ly, Nav->GetJumpRangeLy(), Nav->GetJumpFuelFor(Ly)));
		}
		return true;
	}
	if (Verb == TEXT("refuel"))
	{
		if (Nav) Nav->Refuel(Arg.IsEmpty() ? 1.f : FCString::Atof(*Arg));
		return true;
	}

	// ---------------- Testing helpers (stripped from Shipping builds) ----------------
#if !UE_BUILD_SHIPPING
	if (Verb == TEXT("hull"))
	{
		if (!Ship) return true;
		EHDHullClass C;
		if (!ParseHullClass(Arg, C)) { Notify(TEXT("hull <Fighter|Corvette|Frigate|Destroyer|Cruiser|Battlecruiser|Battleship|Capital|Supercapital|JumpFreighter|CapitalIndustrial>")); return true; }
		ApplyHullClassDefaults(Ship, C);
		Notify(FString::Printf(TEXT("Hull class: %s. %s"), HDShips::ClassName(C), HDShips::IsCapitalSize(C)
			? *FString::Printf(TEXT("Jump drive %.1f ly, %.0f t jump fuel."), Nav ? Nav->GetJumpRangeLy() : 0.f, Nav ? Nav->JumpFuel : 0.f)
			: TEXT("Gate-bound.")));
		return true;
	}
	if (Verb == TEXT("fuel"))
	{
		if (Nav) { Nav->ShipFuel = Nav->MaxShipFuel; Nav->JumpFuel = Nav->MaxJumpFuel; Notify(TEXT("Tanks filled (test)")); }
		return true;
	}
	if (Verb == TEXT("strain"))
	{
		if (!Nav) return true;
		if (Arg == TEXT("reset")) { Nav->StrainUntil = 0.0; Nav->CooldownUntil = 0.0; }
		Notify(FString::Printf(TEXT("Drive strain %.1f min (cap 300), spool-down %.1f min"), Nav->GetStrainMinutes(), Nav->GetCooldownMinutes()));
		return true;
	}
	if (Verb == TEXT("skill"))
	{
		// skill <id> <level>, e.g. "skill jump_range 5"
		FString Id, Lvl;
		if (!PS || !Arg.Split(TEXT(" "), &Id, &Lvl)) { Notify(TEXT("skill <id> <0-5>")); return true; }
		PS->SetSkill(FName(*Id), FMath::Clamp(FCString::Atoi(*Lvl), 0, 5));
		Notify(FString::Printf(TEXT("Skill %s = %d"), *Id, PS->GetSkill(FName(*Id))));
		return true;
	}
	if (Verb == TEXT("navstatus"))
	{
		if (!Nav || !Ship->Movement) return true;
		double Alt = 0.0;
		AHDCelestialBody* B = Nav->FindNearestBody(Alt);
		Notify(FString::Printf(TEXT("Hull %s | fuel %.1f/%.0f t | jump fuel %.0f/%.0f t | range %.2f ly | strain %.1f min | near %s at %.1f km"),
			HDShips::ClassName(Ship->HullClass), Nav->ShipFuel, Nav->MaxShipFuel, Nav->JumpFuel, Nav->MaxJumpFuel, Nav->GetJumpRangeLy(),
			Nav->GetStrainMinutes(), B ? *B->DisplayName : TEXT("-"), Alt / 1.0e5));
		return true;
	}
#endif
	return false;
}
