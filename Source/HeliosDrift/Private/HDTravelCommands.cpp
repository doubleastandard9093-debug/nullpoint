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
#include "HDCatalog.h"
#include "HDFittingComponent.h"
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

/** Server: switch to a catalog hull (by id, or the first hull of a class) with its suggested fit and full tanks. */
static bool ApplyTestHull(AHDShipPawn* Ship, const FString& Arg, FString& OutMsg)
{
	UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(Ship);
	if (!Cat || !Ship->Fitting) { OutMsg = TEXT("No catalog"); return false; }
	FName HullId(*Arg.ToLower());
	if (!Cat->FindHull(HullId))
	{
		EHDHullClass C;
		if (!ParseHullClass(Arg, C)) { OutMsg = TEXT("hull <hull id | class>, e.g. hull longhaul, hull Capital"); return false; }
		HullId = NAME_None;
		for (const TPair<FName, FHDHullDef>& P : Cat->GetHulls()) { if (P.Value.Class == C) { HullId = P.Key; break; } }
		if (HullId.IsNone()) { OutMsg = TEXT("No hull of that class in the catalog"); return false; }
	}
	FString Err;
	if (!Ship->Fitting->SetFitAuthority(HullId, UHDFittingComponent::SuggestFit(Cat, HullId), Err)) { OutMsg = Err; return false; }
	Ship->HullHP = Ship->MaxHullHP;
	if (Ship->Nav) { Ship->Nav->ShipFuel = Ship->Nav->MaxShipFuel; Ship->Nav->JumpFuel = Ship->Nav->MaxJumpFuel; }
	OutMsg = Ship->Fitting->DescribeFit();
	return true;
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
		FString Msg;
		ApplyTestHull(Ship, Arg, Msg);
		TArray<FString> Lines;
		Msg.ParseIntoArrayLines(Lines);
		for (const FString& L : Lines) Notify(L);
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
