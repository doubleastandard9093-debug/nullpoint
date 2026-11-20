#include "HDNpcController.h"
#include "HDShipPawn.h"
#include "HDShipMovementComponent.h"
#include "HDLaserComponent.h"
#include "HDFittingComponent.h"
#include "HDCargoComponent.h"
#include "HDPlayerState.h"
#include "HDWorldActors.h"
#include "HDNpcSubsystem.h"
#include "HDTerritory.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AHDNpcController::AHDNpcController()
{
	PrimaryActorTick.bCanEverTick = true;
}

const TCHAR* AHDNpcController::PopName(EHDNpcPop P)
{
	switch (P)
	{
	case EHDNpcPop::Accord: return TEXT("Accord security");
	case EHDNpcPop::Patrol: return TEXT("Faction patrol");
	case EHDNpcPop::Trader: return TEXT("Trader");
	case EHDNpcPop::Rogue: return TEXT("Rogue");
	case EHDNpcPop::Pirate: return TEXT("Pirate");
	case EHDNpcPop::Sentinel: return TEXT("Sentinel");
	default: return TEXT("Unknown signature");
	}
}

AHDShipPawn* AHDNpcController::GetShip() const
{
	return Cast<AHDShipPawn>(GetPawn());
}

bool AHDNpcController::IsHostileTo(const AActor* Other) const
{
	const AHDShipPawn* S = Cast<AHDShipPawn>(Other);
	if (!S || S == GetPawn() || S->HullHP <= 0.f) return false;
	if (Other == LastAttacker.Get() && GetWorld() && GetWorld()->GetTimeSeconds() - LastAttackedTime < 60.f) return true;
	const AHDNpcController* NC = Cast<AHDNpcController>(S->GetController());
	const AHDPlayerState* PS = S->GetPlayerState<AHDPlayerState>();
	if (!DefendCorp.IsNone())
	{
		const UHDTerritorySubsystem* T = UHDTerritorySubsystem::Get(this);
		return T && T->IsHostileToCorp(DefendCorp, S);
	}
	switch (Pop)
	{
	case EHDNpcPop::Accord:
	case EHDNpcPop::Patrol:
		// Lawful forces fight raiders and criminals.
		if (NC) return !NC->IsLawful() && NC->Pop != EHDNpcPop::Shadow;
		return PS && PS->IsCriminal();
	case EHDNpcPop::Trader:
		return false;
	case EHDNpcPop::Rogue:
		// Opportunists: pick on lawful traffic and pilots in small hulls; leave big ships alone.
		if (NC) return NC->Pop == EHDNpcPop::Trader;
		if (S->Fitting && S->Fitting->Stats.HP > 6000.f) return false;
		return PS != nullptr;
	case EHDNpcPop::Pirate:
		if (NC) return NC->IsLawful();
		return PS != nullptr;
	case EHDNpcPop::Sentinel:
		// Anything not ancient that enters the guarded volume.
		return !NC || NC->Pop != EHDNpcPop::Sentinel;
	default:
		return false; // shadows only answer fire
	}
}

void AHDNpcController::OnDamaged(AActor* By)
{
	AActor* Root = By;
	LastAttacker = Root;
	LastAttackedTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	if (Pop == EHDNpcPop::Shadow && !bRevealed) Reveal();
	if (Pop == EHDNpcPop::Trader) { State = EHDNpcState::Flee; FleeFrom = Root ? Root->GetActorLocation() : FVector::ZeroVector; return; }
	if (State != EHDNpcState::Flee && Root) { Target = Root; State = EHDNpcState::Engage; }
}

void AHDNpcController::Reveal()
{
	bRevealed = true;
	if (AHDShipPawn* S = GetShip()) { S->SetActorHiddenInGame(false); }
}

AActor* AHDNpcController::FindTarget() const
{
	const AHDShipPawn* Me = GetShip();
	if (!Me || !GetWorld()) return nullptr;
	const float Range = Pop == EHDNpcPop::Accord && Pursuit.IsValid() ? 1.0e8f : AggroRange;
	if (AHDShipPawn* P = Pursuit.Get())
	{
		if (P->SystemIndex == Me->SystemIndex && P->HullHP > 0.f) return P;
	}
	AActor* Best = nullptr;
	double BestD = Range;
	for (TActorIterator<AHDShipPawn> It(GetWorld()); It; ++It)
	{
		AHDShipPawn* S = *It;
		if (S->SystemIndex != Me->SystemIndex || !IsHostileTo(S)) continue;
		const double D = FVector::Dist(S->GetActorLocation(), Me->GetActorLocation());
		if (Pop == EHDNpcPop::Sentinel && FVector::Dist(S->GetActorLocation(), Home) > GuardRadius) continue;
		if (D < BestD) { BestD = D; Best = S; }
	}
	return Best;
}

void AHDNpcController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);
	bEverPossessed = true;
}

void AHDNpcController::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority()) return;
	AHDShipPawn* Ship = GetShip();
	if (!Ship)
	{
		// Our ship was destroyed or streamed out: nothing left to fly.
		if (bEverPossessed) Destroy();
		return;
	}
	Clock += DeltaSeconds;
	ThinkTimer -= DeltaSeconds;
	if (ThinkTimer <= 0.f)
	{
		ThinkTimer = 0.5f + FMath::FRand() * 0.2f;
		Think();
	}
	Steer(DeltaSeconds);
}

void AHDNpcController::Think()
{
	AHDShipPawn* Ship = GetShip();
	if (!Ship) return;
	const float MaxHP = Ship->Fitting ? FMath::Max(1.f, Ship->Fitting->Stats.HP) : FMath::Max(1.f, Ship->MaxHullHP);
	const float HullFrac = Ship->HullHP / MaxHP;

	// Chain of command: a pirate wing breaks when its leader dies.
	if (Pop == EHDNpcPop::Pirate && bHadLeader && !Leader.IsValid() && State != EHDNpcState::Flee)
	{
		State = EHDNpcState::Flee;
		FleeFrom = Ship->GetActorLocation() - Ship->GetActorForwardVector() * 1000.f;
		return;
	}
	if (Pop == EHDNpcPop::Rogue && HullFrac < FleeHullFraction && State != EHDNpcState::Flee)
	{
		State = EHDNpcState::Flee;
		FleeFrom = Target.IsValid() ? Target->GetActorLocation() : Ship->GetActorLocation() - Ship->GetActorForwardVector() * 1000.f;
		Target = nullptr;
		return;
	}
	if (State == EHDNpcState::Flee)
	{
		// Fled far enough: vanish (rogues jump out; traders resume later as fresh traffic).
		if (FVector::Dist(Ship->GetActorLocation(), FleeFrom) > 3.0e7f && Pop != EHDNpcPop::Trader) { Ship->Destroy(); Destroy(); }
		else if (Pop == EHDNpcPop::Trader && GetWorld()->GetTimeSeconds() - LastAttackedTime > 30.f && FVector::Dist(Ship->GetActorLocation(), FleeFrom) > 1.0e7f) State = EHDNpcState::Travel;
		return;
	}

	// Shadows reveal when they choose: a pilot lingering close.
	if (Pop == EHDNpcPop::Shadow && !bRevealed)
	{
		for (TActorIterator<AHDShipPawn> It(GetWorld()); It; ++It)
		{
			if (It->IsPlayerControlled() && It->SystemIndex == Ship->SystemIndex && FVector::Dist(It->GetActorLocation(), Ship->GetActorLocation()) < 1.5e6f)
			{
				Reveal();
				if (UHDNpcSubsystem* N = GetWorld()->GetSubsystem<UHDNpcSubsystem>()) N->OnShadowRevealed(this, *It);
				break;
			}
		}
	}

	AActor* T = Target.Get();
	const AHDShipPawn* TS = Cast<AHDShipPawn>(T);
	const bool bTargetGone = !T || (TS && (TS->HullHP <= 0.f || TS->SystemIndex != Ship->SystemIndex)) || !IsHostileTo(T);
	if (bTargetGone) { Target = FindTarget(); T = Target.Get(); }
	// Guards and sentinels never leave the volume.
	// Sentinels never pursue past their volume; other site guards give up a little further out.
	const float Leash = Pop == EHDNpcPop::Sentinel ? GuardRadius : GuardRadius * 1.5f;
	const bool bLeashed = Pop == EHDNpcPop::Sentinel || Site.IsValid();
	if (T && bLeashed && FVector::Dist(T->GetActorLocation(), Home) > Leash) { Target = nullptr; T = nullptr; }

	// Wingmen fight the leader's target and otherwise fly formation on the leader.
	AHDNpcController* Lead = Leader.Get();
	if (!T && Lead && Lead->Target.IsValid() && IsHostileTo(Lead->Target.Get())) { Target = Lead->Target; T = Target.Get(); }
	if (T) { State = EHDNpcState::Engage; return; }
	if (Lead && Lead->GetShip())
	{
		Waypoints.Reset();
		Waypoints.Add(Lead->GetShip()->GetActorLocation() - Lead->GetShip()->GetActorForwardVector() * 150000.f + Lead->GetShip()->GetActorRightVector() * (GetUniqueID() % 2 ? 100000.f : -100000.f));
		State = EHDNpcState::Patrol;
		return;
	}
	if (!Pursuit.IsValid() && Pop == EHDNpcPop::Accord && !bAmbient)
	{
		// Response ships leave once the job is done.
		Ship->Destroy();
		Destroy();
		return;
	}
	if (Pop == EHDNpcPop::Sentinel || Site.IsValid()) { State = EHDNpcState::Guard; return; }
	State = Waypoints.Num() > 0 ? (Pop == EHDNpcPop::Trader ? EHDNpcState::Travel : EHDNpcState::Patrol) : EHDNpcState::Guard;
}

void AHDNpcController::FlyToward(const FVector& Point, float DesiredSpeedCm, float Dt)
{
	AHDShipPawn* Ship = GetShip();
	UHDShipMovementComponent* M = Ship ? Ship->Movement.Get() : nullptr;
	if (!M) return;
	const FVector ToW = Point - Ship->GetActorLocation();
	const double Dist = ToW.Size();
	const FVector Local = Ship->GetActorQuat().UnrotateVector(ToW.GetSafeNormal());
	const float YawErr = FMath::RadiansToDegrees(FMath::Atan2(Local.Y, Local.X));
	const float PitchErr = FMath::RadiansToDegrees(FMath::Atan2(Local.Z, FMath::Sqrt(Local.X * Local.X + Local.Y * Local.Y)));
	M->InputRot = FVector(FMath::Clamp(PitchErr / 15.f, -1.f, 1.f), FMath::Clamp(YawErr / 15.f, -1.f, 1.f), 0.f);
	const float Speed = (float)Ship->GetVelocity().Size();
	const bool bAligned = FMath::Abs(YawErr) < 25.f && FMath::Abs(PitchErr) < 25.f;
	M->InputLinear = FVector(bAligned ? (Speed < DesiredSpeedCm ? 1.f : 0.f) : 0.2f, 0.f, 0.f);

	// Long legs: cruise when lined up and far; drop when close.
	const EHDTravelMode Mode = M->GetTravelMode();
	if (Mode == EHDTravelMode::Normal && Dist > 5.0e7 && FMath::Abs(YawErr) < 5.f && FMath::Abs(PitchErr) < 5.f) M->ToggleCruiseAuthority();
	else if (Mode == EHDTravelMode::Cruise && Dist < FMath::Max(2.0e7, (double)M->GetTravelSpeed() * 4.0)) M->ToggleCruiseAuthority();
}

void AHDNpcController::Steer(float Dt)
{
	AHDShipPawn* Ship = GetShip();
	UHDShipMovementComponent* M = Ship ? Ship->Movement.Get() : nullptr;
	UHDLaserComponent* L = Ship ? Ship->Laser.Get() : nullptr;
	if (!M) return;
	const float Cruise = Ship->Fitting ? Ship->Fitting->Stats.Speed * 100.f : 30000.f;
	bool bFire = false;
	switch (State)
	{
	case EHDNpcState::Engage:
	{
		AActor* T = Target.Get();
		if (!T) break;
		const FVector TV = T->GetVelocity();
		const double D = FVector::Dist(T->GetActorLocation(), Ship->GetActorLocation());
		// Lead the target a little; orbit at ~60% of gun range.
		const FVector Aim = T->GetActorLocation() + TV * FMath::Min(D / 300000.0, 2.0);
		const float Range = L ? L->Range : 500000.f;
		// Close in fast, then slow down and keep the nose on target inside gun range.
		FlyToward(Aim, D > Range ? Cruise : (D < Range * 0.4f ? Cruise * 0.2f : Cruise * 0.6f), Dt);
		const FVector Fwd = Ship->GetActorForwardVector();
		bFire = D < Range && FVector::DotProduct(Fwd, (Aim - Ship->GetActorLocation()).GetSafeNormal()) > 0.996f;
		break;
	}
	case EHDNpcState::Flee:
	{
		const FVector Away = Ship->GetActorLocation() + (Ship->GetActorLocation() - FleeFrom).GetSafeNormal() * 1.0e9;
		FlyToward(Away, Cruise, Dt);
		break;
	}
	case EHDNpcState::Patrol:
	case EHDNpcState::Travel:
	{
		if (Waypoints.Num() == 0) break;
		WaypointIndex = WaypointIndex % Waypoints.Num();
		const FVector W = Waypoints[WaypointIndex];
		FlyToward(W, Cruise * 0.7f, Dt);
		if (FVector::Dist(W, Ship->GetActorLocation()) < 300000.f) WaypointIndex = (WaypointIndex + 1) % Waypoints.Num();
		break;
	}
	case EHDNpcState::Guard:
	default:
	{
		// Loiter: slow circle around home.
		const double D = FVector::Dist(Home, Ship->GetActorLocation());
		const FVector Goal = D > 400000.0 ? Home : Home + FRotator(0.f, Clock * 6.f, 0.f).Vector() * 300000.f;
		FlyToward(Goal, Cruise * 0.3f, Dt);
		break;
	}
	}
	if (L) L->SetFiringAuthority(bFire);
}
