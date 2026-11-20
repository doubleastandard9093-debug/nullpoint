#include "HDLaserComponent.h"
#include "HDShipPowerComponent.h"
#include "HDShipMovementComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/DamageType.h"
#include "Kismet/GameplayStatics.h"
#include "DrawDebugHelpers.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HDShipPawn.h"
#include "HDWorldActors.h"
#include "HDWreckActor.h"
#include "HDCargoComponent.h"
#include "HDFittingComponent.h"
#include "HDNavComponent.h"
#include "HDPlayerState.h"
#include "HDPlayerController.h"
#include "HDEconomy.h"
#include "HDCatalog.h"
#include "HDUniverseSubsystem.h"

UHDLaserComponent::UHDLaserComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	SetIsReplicatedByDefault(true);
}

void UHDLaserComponent::ServerSetFiring_Implementation(bool bFiring)
{
	bWantsFire = bFiring;
}

void UHDLaserComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	if (!GetOwner() || !GetOwner()->HasAuthority()) return;
	HoldFullWarn = FMath::Max(0.f, HoldFullWarn - DeltaTime);
	TickGasSiphon(DeltaTime);
	Cooldown -= DeltaTime;
	if (!bWantsFire)
	{
		Cooldown = FMath::Max(0.f, Cooldown);
		return;
	}
	// Keep the remainder so the fire rate does not depend on frame rate; cap shots per tick.
	const float Interval = FMath::Max(FireInterval, 0.02f);
	int32 Shots = 0;
	while (Cooldown <= 0.f && Shots < 4)
	{
		FireOnce();
		Cooldown += Interval;
		++Shots;
	}
	if (Cooldown < 0.f) { Cooldown = 0.f; } // hitched frame: drop excess shots instead of bursting later
}

void UHDLaserComponent::FireOnce()
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World) return;
	// Server validation: no weapons while spooling or in slipstream.
	if (const UHDShipMovementComponent* Move = Owner->FindComponentByClass<UHDShipMovementComponent>())
	{
		if (Move->GetTravelMode() != EHDTravelMode::Normal) return;
	}
	const bool bTools = MiningRate > 0.f || SalvageRate > 0.f;
	if (Damage <= 0.f && !bTools) return; // nothing fitted that fires

	const FVector Fwd = Owner->GetActorForwardVector();
	const FVector Start = Owner->GetActorLocation() + Fwd * MuzzleOffset;
	const float TraceRange = FMath::Max(Damage > 0.f ? Range : 0.f, bTools ? ToolRange : 0.f);
	const FVector End = Start + Fwd * TraceRange;

	FCollisionQueryParams Params(SCENE_QUERY_STAT(HDLaser), false, Owner);
	FHitResult Hit;
	const bool bHit = World->LineTraceSingleByChannel(Hit, Start, End, TraceChannel.GetValue(), Params);
	const float Interval = FMath::Max(FireInterval, 0.02f);

	if (bHit && Hit.GetActor())
	{
		const bool bInToolRange = Hit.Distance <= ToolRange;
		if (AHDAsteroidActor* Rock = Cast<AHDAsteroidActor>(Hit.GetActor()))
		{
			if (MiningRate > 0.f && bInToolRange) MineRock(Rock, Interval);
		}
		else if (AHDWreckActor* Wreck = Cast<AHDWreckActor>(Hit.GetActor()))
		{
			if (SalvageRate > 0.f && bInToolRange) SalvageWreck(Wreck, Interval);
		}
		else if (Damage > 0.f && Hit.Distance <= Range)
		{
			float Weapons = 50.f;
			if (const UHDShipPowerComponent* Power = Owner->FindComponentByClass<UHDShipPowerComponent>())
			{
				Weapons = Power->GetPower(EHDSubsystem::Weapons);
			}
			const float Dmg = FMath::Max(0.f, Damage * (PowerScaleBase + Weapons / FMath::Max(PowerScaleDivisor, 1.f)));
			const APawn* Pawn = Cast<APawn>(Owner);
			UGameplayStatics::ApplyDamage(Hit.GetActor(), Dmg, Pawn ? Pawn->GetController() : nullptr, Owner, UDamageType::StaticClass());
		}
	}
	MulticastTracer(Start, bHit ? Hit.ImpactPoint : End, bHit);
}

float UHDLaserComponent::GetFleetBoost()
{
	BoostTimer -= FireInterval;
	if (BoostTimer > 0.f) return BoostCache;
	BoostTimer = 2.f;
	BoostCache = 0.f;
	const AHDShipPawn* Me = Cast<AHDShipPawn>(GetOwner());
	const AHDPlayerState* MyPS = Me ? Me->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!Me || !GetWorld()) return 0.f;
	for (TActorIterator<AHDShipPawn> It(GetWorld()); It; ++It)
	{
		const AHDShipPawn* Other = *It;
		if (Other == Me || Other->SystemIndex != Me->SystemIndex || !Other->Fitting || Other->Fitting->Stats.CommandBoost <= 0.f) continue;
		if (FVector::DistSquared(Other->GetActorLocation(), Me->GetActorLocation()) > 5.0e6 * 5.0e6) continue; // 50 km
		const AHDPlayerState* OPS = Other->GetPlayerState<AHDPlayerState>();
		const bool bFleet = !OPS || !MyPS || (!MyPS->Corp.IsNone() && MyPS->Corp == OPS->Corp) || OPS == MyPS;
		if (bFleet) BoostCache = FMath::Max(BoostCache, Other->Fitting->Stats.CommandBoost);
	}
	return BoostCache;
}

void UHDLaserComponent::MineRock(AHDAsteroidActor* Rock, float Seconds)
{
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwner());
	if (!Ship || !Ship->Cargo || !Rock || Rock->IsDepleted()) return;
	AHDPlayerController* PC = Cast<AHDPlayerController>(Ship->GetController());
	// Belts need a stable orbit, ice needs a long stationary hold (doc 16).
	const float Speed = (float)Ship->GetVelocity().Size();
	const bool bIce = Rock->FieldType == EHDFieldType::IceField;
	if (Speed > (bIce ? 1000.f : 5000.f))
	{
		if (PC && HoldFullWarn <= 0.f) { PC->Notify(bIce ? TEXT("Hold still (<10 m/s) to cut ice") : TEXT("Match the rock: under 50 m/s to mine")); HoldFullWarn = 3.f; }
		return;
	}
	const float Rate = MiningRate * (bIce ? 0.6f : 1.f) * (1.f + GetFleetBoost()) * (Rock->bDeepCore ? 1.5f : 1.f);
	MiningAccum += Rate * Seconds;
	const int32 Whole = FMath::FloorToInt(MiningAccum);
	if (Whole <= 0) return;
	const int32 Space = Ship->Cargo->SpaceFor(Rock->ItemId);
	if (Space <= 0)
	{
		if (PC && HoldFullWarn <= 0.f) { PC->Notify(TEXT("Hold full")); HoldFullWarn = 5.f; }
		MiningAccum = 0.f;
		return;
	}
	const int32 Taken = FMath::FloorToInt(Rock->Extract((float)FMath::Min(Whole, Space)));
	MiningAccum -= Whole;
	Ship->Cargo->Add(Rock->ItemId, Taken);
	if (UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(this)) Econ->AddMined(Rock->RegionIndex, (float)Taken);
	if (Rock->IsDepleted())
	{
		Rock->SetActorHiddenInGame(true);
		Rock->SetActorEnableCollision(false);
		if (PC) PC->Notify(TEXT("Rock mined out"));
	}
}

void UHDLaserComponent::SalvageWreck(AHDWreckActor* Wreck, float Seconds)
{
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwner());
	if (!Ship || !Wreck) return;
	AHDPlayerController* PC = Cast<AHDPlayerController>(Ship->GetController());
	FString Why;
	if (!Wreck->CanWork(Ship, Why))
	{
		if (PC && HoldFullWarn <= 0.f) { PC->Notify(Why); HoldFullWarn = 4.f; }
		return;
	}
	const FString Msg = Wreck->Work(Ship, SalvageRate * Seconds);
	if (!Msg.IsEmpty() && PC) PC->Notify(Msg);
}

void UHDLaserComponent::TickGasSiphon(float Dt)
{
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwner());
	if (!Ship || !Ship->Fitting || !Ship->Cargo || Ship->Fitting->Stats.Gas <= 0.f) return;
	GasTimer += Dt;
	if (GasTimer < 1.f) return;
	const float Step = GasTimer;
	GasTimer = 0.f;
	// Siphoned while flying slowly with no cover (doc 16).
	if (Ship->GetVelocity().Size() > 5000.f || !Ship->Movement || Ship->Movement->GetTravelMode() != EHDTravelMode::Normal) return;
	AHDPlayerController* PC = Cast<AHDPlayerController>(Ship->GetController());
	FName Item;
	float Mul = 1.f;
	AHDAsteroidActor* Pocket = nullptr;
	for (TActorIterator<AHDAsteroidActor> It(GetWorld()); It; ++It)
	{
		if (It->FieldType != EHDFieldType::GasCloud || It->IsDepleted() || It->SystemIndex != Ship->SystemIndex) continue;
		if (FVector::Dist(It->GetActorLocation(), Ship->GetActorLocation()) < It->RadiusCm + 50000.f) { Pocket = *It; Item = It->ItemId; break; }
	}
	if (!Pocket)
	{
		// Gas-giant cloud tops: unlimited, but the yield follows this week's density.
		double Alt = 0.0;
		AHDCelestialBody* B = Ship->Nav ? Ship->Nav->FindNearestBody(Alt) : nullptr;
		if (!B || !B->IsGasGiant() || Alt > 5.0e6) return;
		Item = TEXT("gas_nebular");
		Mul = UHDUniverseSubsystem::DensityYieldMultiplier(B->Density);
	}
	GasAccum += Ship->Fitting->Stats.Gas * Mul * Step * (1.f + GetFleetBoost());
	const int32 Whole = FMath::FloorToInt(GasAccum);
	if (Whole <= 0) return;
	GasAccum -= Whole;
	int32 N = Pocket ? FMath::FloorToInt(Pocket->Extract((float)FMath::Min(Whole, Ship->Cargo->SpaceFor(Item)))) : FMath::Min(Whole, Ship->Cargo->SpaceFor(Item));
	N = Ship->Cargo->Add(Item, N);
	if (N <= 0 && PC && HoldFullWarn <= 0.f) { PC->Notify(TEXT("Hold full")); HoldFullWarn = 5.f; }
	if (Pocket && Pocket->IsDepleted()) Pocket->SetActorHiddenInGame(true);
}

void UHDLaserComponent::MulticastTracer_Implementation(const FVector& Start, const FVector& End, bool bHit)
{
#if ENABLE_DRAW_DEBUG
	if (UWorld* World = GetWorld())
	{
		DrawDebugLine(World, Start, End, bHit ? FColor::Orange : FColor::Red, false, 0.08f, 0, 12.f);
		if (bHit) { DrawDebugSphere(World, End, 80.f, 8, FColor::Yellow, false, 0.15f); }
	}
#endif
}
