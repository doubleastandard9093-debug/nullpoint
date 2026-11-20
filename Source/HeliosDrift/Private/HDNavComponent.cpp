#include "HDNavComponent.h"
#include "HDEconomy.h"
#include "HDShipPawn.h"
#include "HDShipMovementComponent.h"
#include "HDShipTypes.h"
#include "HDWorldActors.h"
#include "HDSystemDirector.h"
#include "HDUniverseSubsystem.h"
#include "HDPlayerState.h"
#include "HDPlayerController.h"
#include "HDCatalog.h"
#include "HDFittingComponent.h"
#include "HeliosDrift.h"
#include "Net/UnrealNetwork.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace HDShips
{
	FName FactionKey(const FName& Faction)
	{
		if (Faction.IsNone()) return NAME_None;
		FString S = Faction.ToString().ToLower();
		S.ReplaceInline(TEXT(" "), TEXT("_"));
		return FName(*S);
	}
	const TCHAR* ClassName(EHDHullClass C)
	{
		switch (C)
		{
		case EHDHullClass::Fighter: return TEXT("Fighter");
		case EHDHullClass::Corvette: return TEXT("Corvette");
		case EHDHullClass::Frigate: return TEXT("Frigate");
		case EHDHullClass::Destroyer: return TEXT("Destroyer");
		case EHDHullClass::Cruiser: return TEXT("Cruiser");
		case EHDHullClass::Battlecruiser: return TEXT("Battlecruiser");
		case EHDHullClass::Battleship: return TEXT("Battleship");
		case EHDHullClass::Capital: return TEXT("Capital");
		case EHDHullClass::Supercapital: return TEXT("Supercapital");
		case EHDHullClass::JumpFreighter: return TEXT("Jump freighter");
		default: return TEXT("Capital industrial");
		}
	}
	const TCHAR* RoleName(EHDHullRole R)
	{
		switch (R)
		{
		case EHDHullRole::Combat: return TEXT("Combat");
		case EHDHullRole::Miner: return TEXT("Miner");
		case EHDHullRole::Hauler: return TEXT("Hauler");
		case EHDHullRole::Extractor: return TEXT("Extractor");
		case EHDHullRole::Surveyor: return TEXT("Surveyor");
		case EHDHullRole::Salvager: return TEXT("Salvager");
		case EHDHullRole::Interdictor: return TEXT("Interdictor");
		case EHDHullRole::CovertOps: return TEXT("Covert ops");
		case EHDHullRole::IndustrialCommand: return TEXT("Industrial command");
		case EHDHullRole::Explorer: return TEXT("Explorer");
		case EHDHullRole::Carrier: return TEXT("Carrier");
		case EHDHullRole::Dreadnought: return TEXT("Dreadnought");
		default: return TEXT("Freighter");
		}
	}
}

UHDNavComponent::UHDNavComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.1f;
	SetIsReplicatedByDefault(true);
}

void UHDNavComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(UHDNavComponent, ShipFuel, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UHDNavComponent, MaxShipFuel, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UHDNavComponent, JumpFuel, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UHDNavComponent, MaxJumpFuel, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UHDNavComponent, StrainUntil, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UHDNavComponent, CooldownUntil, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UHDNavComponent, JumpTarget, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UHDNavComponent, ScanCountdown, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(UHDNavComponent, ScanStrength, COND_OwnerOnly);
	DOREPLIFETIME(UHDNavComponent, Pending);
	DOREPLIFETIME(UHDNavComponent, JumpCountdown);
	DOREPLIFETIME(UHDNavComponent, DockedAt);
	DOREPLIFETIME(UHDNavComponent, LandedOn);
}

AHDShipPawn* UHDNavComponent::Ship() const { return Cast<AHDShipPawn>(GetOwner()); }

void UHDNavComponent::Notify(const FString& Msg) const
{
	const AHDShipPawn* S = Ship();
	if (AHDPlayerController* PC = S ? Cast<AHDPlayerController>(S->GetController()) : nullptr) { PC->Notify(Msg); }
}

float UHDNavComponent::GetStrainMinutes() const { return (float)FMath::Max(0.0, (StrainUntil - AHDPlayerState::NowUnix()) / 60.0); }
float UHDNavComponent::GetCooldownMinutes() const { return (float)FMath::Max(0.0, (CooldownUntil - AHDPlayerState::NowUnix()) / 60.0); }

float UHDNavComponent::GetJumpRangeLy() const
{
	const AHDShipPawn* S = Ship();
	if (!S) return 0.f;
	const AHDPlayerState* PS = S->GetPlayerState<AHDPlayerState>();
	const int32 Lvl = PS ? PS->GetSkill(TEXT("jump_range")) : 0;
	if (S->Fitting && !S->Fitting->HullId.IsNone() && !S->Fitting->Stats.bJumpDrive) return 0.f; // capital without a jump drive module
	// +20% per level, five levels: doubles base range (doc 13).
	return HDShips::BaseJumpRangeLy(S->HullClass) * (1.f + 0.2f * Lvl);
}

float UHDNavComponent::GetJumpFuelFor(float Ly) const
{
	const AHDShipPawn* S = Ship();
	if (!S) return 0.f;
	const AHDPlayerState* PS = S->GetPlayerState<AHDPlayerState>();
	const int32 Lvl = PS ? PS->GetSkill(TEXT("jump_fuel")) : 0;
	// Linear per ly; skills cut up to 50%.
	return HDShips::JumpFuelPerLy(S->HullClass) * Ly * (1.f - 0.1f * Lvl);
}

AHDCelestialBody* UHDNavComponent::FindNearestBody(double& OutAltitude) const
{
	const AHDShipPawn* S = Ship();
	return (S && S->Movement) ? S->Movement->GetNearestBody(OutAltitude) : nullptr;
}

FHDCorridorStatus UHDNavComponent::CheckCorridor(const AHDCelestialBody* Body, const FVector& ShipWorld, double NowUnix)
{
	FHDCorridorStatus S;
	if (!Body || Body->Body.Dev == EHDDevState::Undeveloped) return S;
	S.bRestricted = true;
	const bool bHardened = Body->Body.Dev == EHDDevState::Hardened;
	const double Cycle = 180.0;
	S.ActiveCorridor = bHardened ? 0 : (int32)(((int64)(NowUnix / Cycle)) % 4);
	S.SecondsLeft = bHardened ? 0.f : (float)(Cycle - FMath::Fmod(NowUnix, Cycle));
	const uint32 H = GetTypeHash(Body->ObjectId);
	const FVector Axis = FRotator(35.f - 10.f * S.ActiveCorridor, (float)(H % 360u) + 90.f * S.ActiveCorridor, 0.f).Vector();
	const FVector Up = (ShipWorld - Body->GetActorLocation()).GetSafeNormal();
	S.DegreesOff = (float)FMath::RadiansToDegrees(FMath::Acos(FMath::Clamp((double)FVector::DotProduct(Up, Axis), -1.0, 1.0)));
	S.bOpen = S.DegreesOff <= (bHardened ? 12.f : 25.f);
	return S;
}

AHDSpaceObject* UHDNavComponent::FindInteractable(FString& OutPrompt) const
{
	OutPrompt.Reset();
	const AHDShipPawn* S = Ship();
	UWorld* World = GetWorld();
	if (!S || !World) return nullptr;
	if (DockedAt)
	{
		AHDSpaceObject* D = Cast<AHDSpaceObject>(DockedAt);
		OutPrompt = FString::Printf(TEXT("Undock from %s"), D ? *D->DisplayName : TEXT("station"));
		return D;
	}
	if (LandedOn) { OutPrompt = TEXT("Take off"); return Cast<AHDSpaceObject>(LandedOn); }
	const AHDPlayerState* PS = S->GetPlayerState<AHDPlayerState>();
	const FVector L = S->GetActorLocation();
	AHDSpaceObject* Best = nullptr;
	double BestD = TNumericLimits<double>::Max();
	for (TActorIterator<AHDSpaceObject> It(World); It; ++It)
	{
		AHDSpaceObject* O = *It;
		if (O->SystemIndex != S->SystemIndex || Cast<AHDAsteroidActor>(O)) continue;
		if (O->bRequiresScan && !(PS && PS->HasRevealed(O->ObjectId))) continue;
		double D = TNumericLimits<double>::Max();
		FString P;
		if (AHDStationActor* St = Cast<AHDStationActor>(O))
		{
			D = FVector::Dist(L, St->GetActorLocation());
			if (D < St->InteractRadius) P = FString::Printf(TEXT("Dock at %s"), *St->DisplayName);
		}
		else if (AHDJumpPointActor* J = Cast<AHDJumpPointActor>(O))
		{
			D = FVector::Dist(L, J->GetActorLocation());
			if (D < J->InteractRadius) P = FString::Printf(TEXT("Jump: %s"), *J->DisplayName);
		}
		else if (AHDTradeLaneActor* Lane = Cast<AHDTradeLaneActor>(O))
		{
			bool bStart = true;
			D = Lane->DistanceToEnd(L, bStart);
			if (D < Lane->InteractRadius) P = Lane->Lane.Toll > 0 ? FString::Printf(TEXT("Enter lane (toll %d cr)"), Lane->Lane.Toll) : FString(TEXT("Enter trade lane"));
		}
		else if (AHDCelestialBody* B = Cast<AHDCelestialBody>(O))
		{
			D = B->GetAltitude(L);
			if (B->Body.bLandable && D < LandMaxAltitude) P = FString::Printf(TEXT("Land on %s"), *B->DisplayName);
		}
		if (!P.IsEmpty() && D < BestD) { BestD = D; Best = O; OutPrompt = P; }
	}
	if (!Best && HDShips::IsCapitalSize(S->HullClass) && JumpTarget >= 0)
	{
		if (UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this))
		{
			OutPrompt = FString::Printf(TEXT("Jump drive to %s (%.1f ly)"), *U->GetSystemSkeleton(JumpTarget).Name, U->DistanceLy(S->SystemIndex, JumpTarget));
		}
	}
	return Best;
}

// ---------------------------------------------------------------------------------------------
// Requests (server)
// ---------------------------------------------------------------------------------------------
void UHDNavComponent::RequestInteract(AActor* Target)
{
	AHDShipPawn* S = Ship();
	if (!S || !S->HasAuthority()) return;
	if (DockedAt) { RequestDock(nullptr); return; }
	if (LandedOn) { RequestLand(); return; }
	FString Prompt;
	AHDSpaceObject* Obj = FindInteractable(Prompt);
	if (AHDStationActor* St = Cast<AHDStationActor>(Obj)) { RequestDock(St); return; }
	if (AHDJumpPointActor* J = Cast<AHDJumpPointActor>(Obj)) { RequestJump(J); return; }
	if (AHDTradeLaneActor* L = Cast<AHDTradeLaneActor>(Obj)) { RequestLane(L); return; }
	if (Cast<AHDCelestialBody>(Obj)) { RequestLand(); return; }
	if (HDShips::IsCapitalSize(S->HullClass) && JumpTarget >= 0) { RequestJump(nullptr); return; }
	Notify(TEXT("Nothing in range to dock, land, jump or enter."));
}

void UHDNavComponent::RequestDock(AActor* Target)
{
	AHDShipPawn* S = Ship();
	if (!S || !S->HasAuthority() || !S->Movement) return;
	if (DockedAt)
	{
		AHDStationActor* St = Cast<AHDStationActor>(DockedAt);
		const FVector Out = St ? St->GetDockPoint() + St->GetActorForwardVector() * 300000.f : S->GetActorLocation();
		S->Movement->SetModeAuthority(EHDTravelMode::Normal);
		S->Movement->TeleportAuthority(Out, St ? St->GetActorQuat() : S->GetActorQuat(), true);
		S->Movement->SetVelocityAuthority((St ? St->GetActorForwardVector() : S->GetActorForwardVector()) * 5000.f);
		Notify(FString::Printf(TEXT("Undocked from %s"), St ? *St->DisplayName : TEXT("station")));
		DockedAt = nullptr;
		return;
	}
	AHDStationActor* St = Cast<AHDStationActor>(Target);
	if (!St)
	{
		FString P;
		St = Cast<AHDStationActor>(FindInteractable(P));
	}
	if (!St || FVector::Dist(St->GetActorLocation(), S->GetActorLocation()) > St->InteractRadius) { Notify(TEXT("No station within docking range (5 km).")); return; }
	if (S->Movement->GetTravelMode() != EHDTravelMode::Normal) { Notify(TEXT("Drop to sub space to dock.")); return; }
	if (S->GetVelocity().Size() > MaxDockSpeed) { Notify(TEXT("Too fast to dock (max 200 m/s).")); return; }
	if (!St->OwnerCorp.IsNone())
	{
		const AHDPlayerState* PS = S->GetPlayerState<AHDPlayerState>();
		if (!PS || (PS->Corp != St->OwnerCorp && St->Info.Family == EHDStationFamily::CorporateModular && !St->Info.bFuelStation))
		{
			Notify(TEXT("Docking refused: company-only station."));
			return;
		}
	}
	if (St->Info.Family == EHDStationFamily::PirateKitbash && St->OwnerCorp.IsNone())
	{
		const AHDPlayerState* PS = S->GetPlayerState<AHDPlayerState>();
		if (PS && PS->GetStanding(St->Info.Faction) < -2.f) { Notify(TEXT("Docking refused: the pirates here want you dead.")); return; }
	}
	S->Movement->SetModeAuthority(EHDTravelMode::Docked);
	S->Movement->TeleportAuthority(St->GetDockPoint(), St->GetActorQuat(), true);
	DockedAt = St;
	FString Services = TEXT("market");
	if (St->Info.bRepair) Services += TEXT(", repair");
	if (St->Info.bRefinery) Services += TEXT(", refinery");
	if (St->Info.bShipyard) Services += TEXT(", shipyard");
	if (St->Info.bFuelStation) Services += TEXT(", jump fuel");
	Notify(FString::Printf(TEXT("Docked at %s (%s). Services: %s"), *St->DisplayName, *St->GetKindLabel(), *Services));
	AHDPlayerState* PS = S->GetPlayerState<AHDPlayerState>();
	UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(this);
	if (PS && Cat && !PS->bStarterGiven)
	{
		PS->bStarterGiven = true;
		for (const TPair<FName, int32>& P : Cat->StarterStorage) { PS->AddStorage(St->ObjectId, P.Key, P.Value); }
		Notify(TEXT("Starter kit delivered to station storage here (HDCmd storage, HDCmd fit <slot> <module>)."));
	}
}

void UHDNavComponent::RequestLand()
{
	AHDShipPawn* S = Ship();
	if (!S || !S->HasAuthority() || !S->Movement) return;
	AHDCelestialBody* Body = Cast<AHDCelestialBody>(LandedOn);
	if (Body)
	{
		const FVector Up = (S->GetActorLocation() - Body->GetActorLocation()).GetSafeNormal();
		S->Movement->SetModeAuthority(EHDTravelMode::Normal);
		S->Movement->TeleportAuthority(S->GetActorLocation() + Up * 30000.f, S->GetActorQuat(), true);
		S->Movement->SetVelocityAuthority(Up * 5000.f);
		LandedOn = nullptr;
		Notify(TEXT("Lift-off"));
		return;
	}
	double Alt = 0.0;
	Body = FindNearestBody(Alt);
	if (!Body || !Body->Body.bLandable) { Notify(TEXT("No landable body below.")); return; }
	if (S->Movement->GetTravelMode() != EHDTravelMode::Normal) { Notify(TEXT("Surface flight only: drop below 3 km first.")); return; }
	if (Alt > LandMaxAltitude) { Notify(FString::Printf(TEXT("Too high to land (%.1f km). Get below %.1f km."), Alt / 1.0e5, LandMaxAltitude / 1.0e5)); return; }
	if (S->GetVelocity().Size() > LandMaxSpeed) { Notify(TEXT("Too fast to land (max 100 m/s).")); return; }
	const float GCap = Body->Body.bAuthored ? 5.f : 2.f;
	if (Body->Body.Gravity > GCap) { Notify(TEXT("Gravity too high to set down.")); return; }
	const FVector Up = (S->GetActorLocation() - Body->GetActorLocation()).GetSafeNormal();
	const FVector Pos = Body->GetActorLocation() + Up * (Body->GetRadiusCm() + 250.0);
	const FQuat Q = FRotationMatrix::MakeFromZX(Up, S->GetActorForwardVector()).ToQuat();
	S->Movement->SetModeAuthority(EHDTravelMode::Landed);
	S->Movement->TeleportAuthority(Pos, Q, true);
	LandedOn = Body;
	Notify(FString::Printf(TEXT("Landed on %s (%.2f g). %s"), *Body->DisplayName, Body->Body.Gravity,
		Body->Body.Gravity > 2.7f ? TEXT("On-foot disembark disabled above 2.7 g.") : TEXT("Disembark available (interiors: planned).")));
}

void UHDNavComponent::RequestCruise()
{
	AHDShipPawn* S = Ship();
	if (!S || !S->HasAuthority() || !S->Movement) return;
	if (DockedAt || LandedOn || Pending != EHDPendingJump::None) { Notify(TEXT("Can't cruise now.")); return; }
	if (ShipFuel <= 0.f && S->Movement->GetTravelMode() != EHDTravelMode::Cruise) { Notify(TEXT("No fuel for cruise.")); return; }
	S->Movement->ToggleCruiseAuthority();
}

void UHDNavComponent::RequestLane(AHDTradeLaneActor* Lane)
{
	AHDShipPawn* S = Ship();
	if (!S || !S->HasAuthority() || !S->Movement || !Lane) return;
	if (S->Movement->GetTravelMode() != EHDTravelMode::Normal) { Notify(TEXT("Drop to sub space to enter a lane.")); return; }
	bool bStart = true;
	if (Lane->DistanceToEnd(S->GetActorLocation(), bStart) > Lane->InteractRadius) { Notify(TEXT("Fly to a lane ring (within 20 km).")); return; }
	if (!Lane->Lane.bNPC && Lane->Lane.Toll > 0) { ChargeToll(Lane->Lane.Toll, Lane->Lane.Owner); }
	const FVector Exit = bStart ? Lane->EndWorld : Lane->StartWorld;
	S->Movement->StartLaneAuthority(Exit, LaneSpeed * FMath::Clamp(Lane->Lane.Integrity, 0.2f, 1.f));
	Notify(FString::Printf(TEXT("Lane engaged: %.0f km to exit"), FVector::Dist(Exit, S->GetActorLocation()) / 1.0e5));
}

void UHDNavComponent::ChargeToll(int32 Toll, const FName& Owner)
{
	AHDShipPawn* S = Ship();
	AHDPlayerState* PS = S ? S->GetPlayerState<AHDPlayerState>() : nullptr;
	if (PS && PS->TrySpend(Toll)) { Notify(FString::Printf(TEXT("Toll paid: %d cr to %s"), Toll, *Owner.ToString())); }
	// Toll income goes to the owner; it never counts toward a claim's Defense Budget (doc 15). See HDTerritory.
	OnTollPaid.Broadcast(Owner, Toll);
}

bool UHDNavComponent::CanUseLink(const AHDJumpPointActor* J, FString& Why) const
{
	const AHDShipPawn* S = Ship();
	if (!S || !J) { Why = TEXT("No jump point"); return false; }
	const AHDPlayerState* PS = S->GetPlayerState<AHDPlayerState>();
	if (J->bRequiresScan && !(PS && PS->HasRevealed(J->ObjectId))) { Why = TEXT("Signature not resolved: scan it down first"); return false; }
	switch (J->Link.Type)
	{
	case EHDLinkType::Gate:
		if (HDShips::IsCapitalSize(S->HullClass)) { Why = TEXT("Capitals are too large for standard gates: use a large gate or your jump drive"); return false; }
		return true;
	case EHDLinkType::LargeGate:
		return true;
	case EHDLinkType::Wormhole:
	case EHDLinkType::StellarAnomaly:
	case EHDLinkType::InstanceLane:
		return true;
	case EHDLinkType::GalacticRuin:
		Why = TEXT("The galactic gate is an inert ruin. Its far side is not reachable yet.");
		return false;
	}
	return false;
}

void UHDNavComponent::RequestJump(AActor* Target)
{
	AHDShipPawn* S = Ship();
	if (!S || !S->HasAuthority() || !S->Movement) return;
	if (Pending != EHDPendingJump::None) { Notify(TEXT("Jump already in progress")); return; }
	if (DockedAt || LandedOn) { Notify(TEXT("Undock or take off first.")); return; }
	if (S->Movement->GetTravelMode() != EHDTravelMode::Normal) { Notify(TEXT("Drop to sub space to jump.")); return; }
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	if (!U) return;

	AHDJumpPointActor* J = Cast<AHDJumpPointActor>(Target);
	if (!J)
	{
		FString P;
		J = Cast<AHDJumpPointActor>(FindInteractable(P));
	}
	if (J && FVector::Dist(J->GetActorLocation(), S->GetActorLocation()) <= J->InteractRadius)
	{
		FString Why;
		if (!CanUseLink(J, Why)) { Notify(Why); return; }
		PendingLinkId = J->Link.Id;
		PendingSystem = J->Link.To;
		switch (J->Link.Type)
		{
		case EHDLinkType::Wormhole: Pending = EHDPendingJump::Wormhole; break;
		case EHDLinkType::StellarAnomaly: Pending = EHDPendingJump::Anomaly; break;
		case EHDLinkType::InstanceLane: Pending = EHDPendingJump::InstanceLane; PendingSystem = S->SystemIndex; break;
		default: Pending = EHDPendingJump::Gate; break;
		}
		if (Pending == EHDPendingJump::InstanceLane)
		{
			const FHDStarSystem& Sys = U->GetSystem(S->SystemIndex);
			if (!Sys.Sites.IsValidIndex(J->Link.TargetSite)) { Pending = EHDPendingJump::None; Notify(TEXT("Lane leads nowhere")); return; }
			PendingLocal = Sys.Sites[J->Link.TargetSite].LocalPos + FVector(-3.0e6, 0.0, 0.0);
		}
		JumpCountdown = GateSpoolSeconds;
		S->Movement->SetModeAuthority(EHDTravelMode::Jumping);
		Notify(FString::Printf(TEXT("%s: jumping in %.0f s"), *J->DisplayName, JumpCountdown));
		return;
	}

	if (!HDShips::IsCapitalSize(S->HullClass))
	{
		Notify(TEXT("No gate in range. Fighters through battleships need a gate, lane, wormhole or anomaly to leave a system."));
		return;
	}
	// Capital self-jump (doc 13).
	if (GetJumpRangeLy() <= 0.f) { Notify(TEXT("No jump drive fitted.")); return; }
	if (!U->IsValidSystem(JumpTarget) || JumpTarget == S->SystemIndex) { Notify(TEXT("Set a jump target on the galaxy map (M) first.")); return; }
	const float Ly = U->DistanceLy(S->SystemIndex, JumpTarget);
	const float Range = GetJumpRangeLy();
	if (Ly > Range) { Notify(FString::Printf(TEXT("Out of range: %.2f ly (max %.2f ly)"), Ly, Range)); return; }
	if (GetCooldownMinutes() > 0.f) { Notify(FString::Printf(TEXT("Drive spooling down: %.1f min"), GetCooldownMinutes())); return; }
	const float Fuel = GetJumpFuelFor(Ly);
	if (JumpFuel < Fuel) { Notify(FString::Printf(TEXT("Not enough jump fuel: need %.0f t, have %.0f t"), Fuel, JumpFuel)); return; }
	Pending = EHDPendingJump::CapitalDrive;
	PendingSystem = JumpTarget;
	PendingLy = Ly;
	JumpCountdown = DriveSpoolSeconds;
	S->Movement->SetModeAuthority(EHDTravelMode::Jumping);
	Notify(FString::Printf(TEXT("Jump drive charging: %s, %.2f ly, %.0f t fuel, %.0f s"), *U->GetSystemSkeleton(JumpTarget).Name, Ly, Fuel, JumpCountdown));
}

void UHDNavComponent::CompleteJump()
{
	AHDShipPawn* S = Ship();
	AHDSystemDirector* Director = AHDSystemDirector::Get(this);
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	const EHDPendingJump Kind = Pending;
	Pending = EHDPendingJump::None;
	if (!S || !Director || !U || !S->Movement) return;
	S->Movement->SetModeAuthority(EHDTravelMode::Normal);

	int32 Dest = PendingSystem;
	FVector Local = FVector::ZeroVector;
	FString Extra;
	switch (Kind)
	{
	case EHDPendingJump::Gate:
	case EHDPendingJump::Wormhole:
		Local = Director->GetLinkArrivalLocal(Dest, PendingLinkId);
		break;
	case EHDPendingJump::Anomaly:
		// Odd distortions: usually the charted far side, sometimes somewhere random (doc 13).
		if (FMath::FRand() < 0.25f)
		{
			const TArray<FHDMapEntry> Near = U->GetSystemsNear(S->SystemIndex, 45.f, 200);
			if (Near.Num() > 1) { Dest = Near[FMath::RandRange(1, Near.Num() - 1)].System; Extra = TEXT(" The anomaly threw you off course!"); }
			Local = Director->GetArrivalLocal(Dest) + FMath::VRand() * 5.0e9;
		}
		else
		{
			Local = Director->GetLinkArrivalLocal(Dest, PendingLinkId);
		}
		break;
	case EHDPendingJump::InstanceLane:
		Local = PendingLocal;
		break;
	case EHDPendingJump::CapitalDrive:
	{
		const float Fuel = GetJumpFuelFor(PendingLy);
		JumpFuel = FMath::Max(0.f, JumpFuel - Fuel);
		const double Now = AHDPlayerState::NowUnix();
		const float Cur = GetStrainMinutes();
		const float EffLy = HDShips::IsLogisticsCapital(S->HullClass) ? PendingLy * 0.1f : PendingLy;
		const float NewStrain = FMath::Min(300.f, FMath::Max(10.f * (1.f + EffLy), Cur * (1.f + EffLy)));
		const float SpoolDown = FMath::Min(30.f, FMath::Max(Cur / 10.f, 1.f + EffLy));
		StrainUntil = Now + NewStrain * 60.0;
		CooldownUntil = Now + SpoolDown * 60.0;
		Local = Director->GetArrivalLocal(Dest) + FVector(FMath::FRandRange(-1.f, 1.f), FMath::FRandRange(-1.f, 1.f), 0.f) * 2.0e8;
		Extra = FString::Printf(TEXT(" Fuel -%.0f t. Strain %.0f min, drive spool-down %.0f min."), Fuel, NewStrain, SpoolDown);
		break;
	}
	default:
		return;
	}
	const FRotator Rot = (-Local).Rotation();
	if (!Director->PlaceShip(S, Dest, Local, Rot))
	{
		Notify(TEXT("Jump failed: destination unavailable (server at its system limit)."));
		return;
	}
	const FHDStarSystem& D = U->GetSystemSkeleton(Dest);
	Notify(FString::Printf(TEXT("Arrived: %s (%s).%s"), *D.Name, HDUniverse::BandName(D.Band), *Extra));
	OnArrived.Broadcast(Dest);
}

void UHDNavComponent::SetJumpTarget(int32 System)
{
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	if (!U || !U->IsValidSystem(System)) { JumpTarget = -1; return; }
	JumpTarget = System;
}

void UHDNavComponent::RequestScan()
{
	AHDShipPawn* S = Ship();
	if (!S || !S->HasAuthority()) return;
	if (ScanCountdown > 0.f) return;
	if (DockedAt) { Notify(TEXT("Can't scan while docked")); return; }
	ScanCountdown = ScanSeconds;
	Notify(TEXT("Scan pulse..."));
}

void UHDNavComponent::CompleteScan()
{
	AHDShipPawn* S = Ship();
	UWorld* World = GetWorld();
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	AHDPlayerState* PS = S ? S->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!S || !World || !U || !PS) return;
	const double Range = ScanRangeAU * ScanStrength * U->AUcm;
	int32 Resolved = 0, Partial = 0;
	FString Names;
	for (TActorIterator<AHDSpaceObject> It(World); It; ++It)
	{
		AHDSpaceObject* O = *It;
		if (O->SystemIndex != S->SystemIndex || !O->bRequiresScan || PS->HasRevealed(O->ObjectId)) continue;
		const double D = FVector::Dist(O->GetActorLocation(), S->GetActorLocation());
		if (D > Range) continue;
		// Strong, close signatures resolve in one pulse; faint or far ones take several.
		const float Score = O->Signature * ScanStrength * (float)(1.0 - D / Range) * 1.6f;
		float& P = ScanProgress.FindOrAdd(O->ObjectId);
		P += Score;
		if (P >= 1.f)
		{
			PS->Reveal(O->ObjectId);
			ScanProgress.Remove(O->ObjectId);
			++Resolved;
			if (Names.Len() < 120) { Names += (Names.IsEmpty() ? TEXT("") : TEXT(", ")) + O->DisplayName; }
		}
		else
		{
			++Partial;
		}
	}
	Notify(FString::Printf(TEXT("Scan: %d resolved%s%s, %d partial"), Resolved, Names.IsEmpty() ? TEXT("") : TEXT(": "), *Names, Partial));
}

void UHDNavComponent::Refuel(float Fraction)
{
	AHDShipPawn* S = Ship();
	AHDStationActor* St = Cast<AHDStationActor>(DockedAt);
	AHDPlayerState* PS = S ? S->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!S || !St || !PS) { Notify(TEXT("Dock at a station to refuel.")); return; }
	Fraction = FMath::Clamp(Fraction, 0.05f, 1.f);
	const float ShipNeed = FMath::Max(0.f, MaxShipFuel * Fraction - ShipFuel);
	const float JumpNeed = St->Info.bFuelStation ? FMath::Max(0.f, MaxJumpFuel * Fraction - JumpFuel) : 0.f;
	const float Avail = St->FuelStock;
	const float ShipT = FMath::Min(ShipNeed, Avail);
	const float JumpT = FMath::Max(0.f, FMath::Min(JumpNeed, Avail - ShipT));
	const UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(this);
	const float ShipPrice = Econ ? (float)Econ->ShipFuelPrice(St) : 40.f;
	const float JumpPrice = Econ ? (float)Econ->JumpFuelPrice(St) : 120.f;
	const int64 Cost = (int64)FMath::CeilToDouble(ShipT * ShipPrice + JumpT * JumpPrice);
	if (ShipT + JumpT <= 0.f) { Notify(MaxJumpFuel > 0.f && !St->Info.bFuelStation ? TEXT("Tanks full. Jump fuel is sold at gas-giant fuel platforms.") : TEXT("Tanks full.")); return; }
	if (!PS->TrySpend(Cost)) { Notify(FString::Printf(TEXT("Need %lld cr"), Cost)); return; }
	ShipFuel += ShipT;
	JumpFuel += JumpT;
	St->FuelStock -= ShipT + JumpT;
	Notify(FString::Printf(TEXT("Refuelled %.0f t ship fuel, %.0f t jump fuel for %lld cr"), ShipT, JumpT, Cost));
}

void UHDNavComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	AHDShipPawn* S = Ship();
	if (!S || !S->HasAuthority() || !S->Movement) return;
	if (Pending != EHDPendingJump::None)
	{
		JumpCountdown -= DeltaTime;
		if (JumpCountdown <= 0.f) { JumpCountdown = 0.f; CompleteJump(); }
	}
	if (ScanCountdown > 0.f)
	{
		ScanCountdown -= DeltaTime;
		if (ScanCountdown <= 0.f) { ScanCountdown = 0.f; CompleteScan(); }
	}
	const EHDTravelMode Mode = S->Movement->GetTravelMode();
	const float Burn = Mode == EHDTravelMode::Cruise ? CruiseFuelPerSecond : Mode == EHDTravelMode::Slipstream ? SlipFuelPerSecond : 0.f;
	if (Burn > 0.f)
	{
		ShipFuel = FMath::Max(0.f, ShipFuel - Burn * DeltaTime);
		if (ShipFuel <= 0.f) { S->Movement->DropOut(false); Notify(TEXT("Out of fuel: dropped to sub space")); }
	}
}
