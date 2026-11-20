#include "HDNpcSubsystem.h"
#include "HDNpcController.h"
#include "HDShipPawn.h"
#include "HDShipTypes.h"
#include "HDShipMovementComponent.h"
#include "HDLaserComponent.h"
#include "HDFittingComponent.h"
#include "HDCargoComponent.h"
#include "HDCatalog.h"
#include "HDPlayerState.h"
#include "HDPlayerController.h"
#include "HDWorldActors.h"
#include "HDWreckActor.h"
#include "HDEconomy.h"
#include "HDSystemDirector.h"
#include "HDUniverseSubsystem.h"
#include "HeliosDrift.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "TimerManager.h"

namespace
{
	const FName AccordKey(TEXT("accord"));
	const FName ShadowKey(TEXT("shadow"));

	int32 BandIndex(EHDSecurityBand B) { return (int32)B; } // 0 Secured .. 3 Frontier

	void NotifyPilot(AHDShipPawn* Ship, const FString& Msg)
	{
		if (AHDPlayerController* PC = Ship ? Cast<AHDPlayerController>(Ship->GetController()) : nullptr) PC->Notify(Msg);
	}

	void NotifyNear(UWorld* World, int32 System, const FVector& Loc, double Range, const FString& Msg)
	{
		if (!World) return;
		for (TActorIterator<AHDShipPawn> It(World); It; ++It)
		{
			if (It->SystemIndex == System && It->IsPlayerControlled() && FVector::Dist(It->GetActorLocation(), Loc) < Range) NotifyPilot(*It, Msg);
		}
	}

	FName PirateFaction(int32 System) { return (System & 1) ? FName(TEXT("rustwake_brethren")) : FName(TEXT("black_tally")); }
}

UHDNpcSubsystem* UHDNpcSubsystem::Get(const UObject* WorldContext)
{
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UHDNpcSubsystem>() : nullptr;
}

void UHDNpcSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	DeathHandle = AHDShipPawn::OnAnyShipDestroyed().AddUObject(this, &UHDNpcSubsystem::HandleShipDestroyed);
}

void UHDNpcSubsystem::Deinitialize()
{
	AHDShipPawn::OnAnyShipDestroyed().Remove(DeathHandle);
	Super::Deinitialize();
}

EHDSecurityBand UHDNpcSubsystem::BandOf(int32 System) const
{
	if (const AHDSystemDirector* D = AHDSystemDirector::Get(this))
	{
		if (const FHDActiveSystem* A = D->FindActive(System)) return A->Band;
	}
	if (UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this))
	{
		if (System >= 0 && System < U->NumSystems()) return U->GetSystem(System).Band;
	}
	return EHDSecurityBand::Secured;
}

bool UHDNpcSubsystem::IsInClaim(int32 System, const FVector& WorldLoc) const
{
	return false; // replaced by the territory layer (M6)
}

// ---------------------------------------------------------------------------------------------
// Spawning
// ---------------------------------------------------------------------------------------------
FName UHDNpcSubsystem::PickHull(EHDNpcPop Pop, EHDSecurityBand Band, FRandomStream& R, bool bLeader)
{
	const int32 B = BandIndex(Band);
	auto Pick = [&R](const TArray<const TCHAR*>& Arr) -> FName
	{
		return FName(Arr[R.RandRange(0, Arr.Num() - 1)]);
	};
	switch (Pop)
	{
	case EHDNpcPop::Accord: return bLeader ? FName(TEXT("sentry")) : Pick({ TEXT("lance"), TEXT("harrier") });
	case EHDNpcPop::Patrol: return bLeader ? Pick({ TEXT("bulwark"), TEXT("outrider"), TEXT("concord"), TEXT("hammerline") }) : Pick({ TEXT("lance"), TEXT("harrier") });
	case EHDNpcPop::Trader: return B >= 2 ? Pick({ TEXT("satchel"), TEXT("drayman") }) : Pick({ TEXT("mule"), TEXT("satchel"), TEXT("drayman") });
	case EHDNpcPop::Rogue:
		if (B == 0) return Pick({ TEXT("wasp"), TEXT("skiff") });
		if (B == 1) return Pick({ TEXT("skiff"), TEXT("lance") });
		if (B == 2) return Pick({ TEXT("lance"), TEXT("harrier") });
		return Pick({ TEXT("harrier"), TEXT("wildcard") });
	case EHDNpcPop::Pirate:
		if (bLeader) return B >= 3 ? Pick({ TEXT("bulwark"), TEXT("citadel") }) : B == 2 ? Pick({ TEXT("verdict"), TEXT("sentry") }) : FName(TEXT("harrier"));
		return B >= 2 ? Pick({ TEXT("lance"), TEXT("harrier"), TEXT("snare") }) : Pick({ TEXT("skiff"), TEXT("lance") });
	case EHDNpcPop::Sentinel: return B >= 3 && bLeader ? FName(TEXT("citadel")) : FName(TEXT("sentry"));
	default: return FName(TEXT("whisper"));
	}
}

AHDNpcController* UHDNpcSubsystem::SpawnNpc(int32 System, const FVector& WorldLoc, EHDNpcPop Pop, FName Hull, FName Faction, float DamageScale)
{
	UWorld* World = GetWorld();
	AHDSystemDirector* D = AHDSystemDirector::Get(this);
	UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(this);
	if (!World || !D || !Cat || !D->IsActive(System)) return nullptr;
	if (!Cat->FindHull(Hull)) Hull = FName(TEXT("lance"));
	const FTransform T(FRotator(0.f, FMath::FRandRange(0.f, 360.f), 0.f), WorldLoc);
	AHDShipPawn* Ship = World->SpawnActorDeferred<AHDShipPawn>(AHDShipPawn::StaticClass(), T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
	if (!Ship) return nullptr;
	Ship->SetSystem(System, D->GetOrigin(System));
	Ship->FinishSpawning(T);
	FString Err;
	TArray<FName> Fit = UHDFittingComponent::SuggestFit(Cat, Hull, Pop != EHDNpcPop::Trader);
	if (Ship->Fitting && !Ship->Fitting->SetFitAuthority(Hull, Fit, Err))
	{
		UE_LOG(LogHelios, Warning, TEXT("NPC fit for %s failed: %s"), *Hull.ToString(), *Err);
	}
	// NPC hardware has a stat ceiling (doc 15): below player-grade, except the Accord, which wins by definition.
	if (Ship->Laser)
	{
		Ship->Laser->Damage *= DamageScale * (Pop == EHDNpcPop::Accord ? 2.5f : 0.8f);
		if (Ship->Laser->Damage <= 0.f) Ship->Laser->Damage = Pop == EHDNpcPop::Trader ? 0.f : 30.f;
	}
	AHDNpcController* C = World->SpawnActor<AHDNpcController>(AHDNpcController::StaticClass(), T);
	if (!C) { Ship->Destroy(); return nullptr; }
	C->Pop = Pop;
	C->Faction = Faction;
	C->Home = WorldLoc;
	C->FleeHullFraction = Pop == EHDNpcPop::Rogue ? 0.4f : Pop == EHDNpcPop::Trader ? 0.9f : 0.f;
	C->AggroRange = Pop == EHDNpcPop::Rogue ? 6.0e6f : Pop == EHDNpcPop::Accord ? 2.0e7f : 1.0e7f; // 60 / 200 / 100 km
	C->Possess(Ship);
	Ship->DisplayLabel = FString::Printf(TEXT("%s %s"), AHDNpcController::PopName(Pop), Cat->FindHull(Hull) ? *Cat->FindHull(Hull)->Name : *Hull.ToString());
	if (Pop == EHDNpcPop::Shadow) { C->bRevealed = false; Ship->SetActorHiddenInGame(true); }
	if (Pop == EHDNpcPop::Trader && Ship->Cargo)
	{
		// Haulers carry real goods: a kill drops them.
		static const TCHAR* Goods[] = { TEXT("food_rations"), TEXT("medical_supplies"), TEXT("consumer_goods"), TEXT("machinery"), TEXT("luxury_goods") };
		Ship->Cargo->Add(FName(Goods[FMath::RandRange(0, 4)]), FMath::RandRange(10, 60));
	}
	D->AddTransient(System, Ship);
	D->AddTransient(System, C);
	return C;
}

// ---------------------------------------------------------------------------------------------
// Ambient traffic and economic spawn pressure (doc 18)
// ---------------------------------------------------------------------------------------------
float UHDNpcSubsystem::GetSpawnPressure(const FHDActiveSystem& A) const
{
	const UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(this);
	const float Trade = Econ ? Econ->GetRegionTrade(A.Region) : 0.f;
	return FMath::Clamp(0.5f + Trade / 250000.f + 0.15f * A.Pilots, 0.5f, 2.5f);
}

void UHDNpcSubsystem::SpawnAmbient(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Pilots, EHDNpcPop Pop, FRandomStream& R)
{
	TArray<AHDStationActor*> Stations;
	for (const TWeakObjectPtr<AHDStationActor>& S : A.Stations) { if (S.IsValid()) Stations.Add(S.Get()); }
	if (Pilots.Num() == 0) return;
	AHDShipPawn* Near = Pilots[R.RandRange(0, Pilots.Num() - 1)];
	const EHDSecurityBand Band = A.Band;

	if (Pop == EHDNpcPop::Accord || Pop == EHDNpcPop::Patrol || Pop == EHDNpcPop::Trader)
	{
		// Lawful traffic lives around stations; pick the station nearest a pilot so it's seen.
		AHDStationActor* Best = nullptr;
		double BestD = TNumericLimits<double>::Max();
		for (AHDStationActor* St : Stations)
		{
			if (Pop == EHDNpcPop::Accord && St->Info.Family != EHDStationFamily::AccordCivic && Band != EHDSecurityBand::Secured) continue;
			if (Pop == EHDNpcPop::Patrol && St->Info.Family != EHDStationFamily::FactionIndustrial) continue;
			if (St->Info.Family == EHDStationFamily::PirateKitbash) continue;
			const double D = FVector::Dist(St->GetActorLocation(), Near->GetActorLocation());
			if (D < BestD) { BestD = D; Best = St; }
		}
		if (!Best) return;
		const FVector Loc = Best->GetActorLocation() + R.GetUnitVector() * R.FRandRange(5.0e6f, 1.5e7f) * FVector(1, 1, 0.2);
		const FName Faction = Pop == EHDNpcPop::Accord ? AccordKey : HDShips::FactionKey(Best->Info.Faction);
		AHDNpcController* C = SpawnNpc(A.System, Loc, Pop, PickHull(Pop, Band, R, Pop == EHDNpcPop::Patrol && R.FRand() < 0.3f), Faction);
		if (!C) return;
		if (Pop == EHDNpcPop::Trader && Stations.Num() > 1)
		{
			for (AHDStationActor* St : Stations) { if (St->Info.Family != EHDStationFamily::PirateKitbash) C->Waypoints.Add(St->GetDockPoint()); }
			C->WaypointIndex = R.RandRange(0, C->Waypoints.Num() - 1);
		}
		else
		{
			for (int32 i = 0; i < 4; ++i) C->Waypoints.Add(Best->GetActorLocation() + FRotator(0.f, 90.f * i + R.FRandRange(-20.f, 20.f), 0.f).Vector() * 2.0e7f);
		}
		return;
	}

	// Raiders appear out where pilots are, a bit beyond scanner-eyeball range.
	const FVector Center = Near->GetActorLocation();
	const FVector Loc = Center + R.GetUnitVector() * R.FRandRange(8.0e6f, 2.0e7f) * FVector(1, 1, 0.3);
	if (Pop == EHDNpcPop::Pirate)
	{
		const FName Faction = PirateFaction(A.System);
		AHDNpcController* Lead = SpawnNpc(A.System, Loc, Pop, PickHull(Pop, Band, R, true), Faction);
		if (!Lead) return;
		for (int32 i = 0; i < 4; ++i) Lead->Waypoints.Add(Center + R.GetUnitVector() * 1.0e7f * FVector(1, 1, 0.2));
		const int32 Wing = R.RandRange(2, 2 + BandIndex(Band));
		for (int32 i = 0; i < Wing; ++i)
		{
			AHDNpcController* W = SpawnNpc(A.System, Loc + R.GetUnitVector() * 200000.f, Pop, PickHull(Pop, Band, R, false), Faction);
			if (!W) continue;
			W->Leader = Lead;
			W->bHadLeader = true;
		}
		return;
	}
	const FName Faction = Pop == EHDNpcPop::Shadow ? ShadowKey : FName(TEXT("rogue"));
	AHDNpcController* C = SpawnNpc(A.System, Loc, Pop, PickHull(Pop, Band, R, false), Faction);
	if (C && Pop == EHDNpcPop::Rogue) { for (int32 i = 0; i < 4; ++i) C->Waypoints.Add(Center + R.GetUnitVector() * 5.0e6f * FVector(1, 1, 0.2)); }
}

void UHDNpcSubsystem::TickSystem(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Ships, float Dt)
{
	UWorld* World = GetWorld();
	if (!World) return;
	TArray<AHDShipPawn*> Pilots;
	for (AHDShipPawn* S : Ships) { if (S->IsPlayerControlled()) Pilots.Add(S); }

	TickEncounters(A);
	ScanTimer -= Dt;
	if (ScanTimer <= 0.f) { ScanTimer = 10.f; ContrabandScans(A, Pilots); }

	A.AmbientTimer -= Dt;
	if (A.AmbientTimer > 0.f || Pilots.Num() == 0) return;
	A.AmbientTimer = 15.f;

	// Census and culling of ambient NPCs far from every pilot.
	int32 Count[7] = { 0, 0, 0, 0, 0, 0, 0 };
	int32 Total = 0;
	for (TActorIterator<AHDNpcController> It(World); It; ++It)
	{
		AHDNpcController* C = *It;
		AHDShipPawn* S = C->GetShip();
		if (!S || S->SystemIndex != A.System || !C->bAmbient) continue;
		double Nearest = TNumericLimits<double>::Max();
		for (const AHDShipPawn* P : Pilots) Nearest = FMath::Min(Nearest, (double)FVector::Dist(P->GetActorLocation(), S->GetActorLocation()));
		if (Nearest > 2.0e8 && C->State != EHDNpcState::Engage) { S->Destroy(); C->Destroy(); continue; }
		++Count[(int32)C->Pop];
		++Total;
	}
	if (Total >= MaxAmbientPerSystem) return;

	// Targets by band. Raider counts are the economy readout: pressure follows regional wealth and traffic.
	const float P = GetSpawnPressure(A);
	int32 Want[7] = { 0, 0, 0, 0, 0, 0, 0 };
	switch (A.Band)
	{
	case EHDSecurityBand::Secured:    Want[0] = 3; Want[2] = 3; Want[3] = FMath::RoundToInt(1.f * P); break;
	case EHDSecurityBand::Restricted: Want[0] = 1; Want[1] = 2; Want[2] = 3; Want[3] = FMath::RoundToInt(1.5f * P); Want[4] = P > 1.2f ? 3 : 0; break;
	case EHDSecurityBand::Borderland: Want[1] = 1; Want[2] = 2; Want[3] = FMath::RoundToInt(2.f * P); Want[4] = FMath::RoundToInt(2.f * P) + 1; break;
	default:                          Want[2] = 1; Want[3] = FMath::RoundToInt(2.f * P); Want[4] = FMath::RoundToInt(3.f * P) + 1; Want[6] = 1; break;
	}
	FRandomStream R((int32)(World->GetTimeSeconds() * 1000.0) ^ (A.System * 7919));
	// One spawn per population per tick keeps arrivals gradual.
	for (int32 Pop = 0; Pop < 7; ++Pop)
	{
		if (Count[Pop] < Want[Pop]) SpawnAmbient(A, Pilots, (EHDNpcPop)Pop, R);
	}
}

// ---------------------------------------------------------------------------------------------
// Encounter anomalies and guarded sites (doc 20, 16, 18)
// ---------------------------------------------------------------------------------------------
void UHDNpcSubsystem::SpawnSiteEncounter(FHDActiveSystem& A, AHDSiteActor* Site, FRandomStream& R, TArray<AActor*>& OutActors)
{
	UWorld* World = GetWorld();
	if (!World || !Site) return;
	const FVector C = Site->GetActorLocation();
	const EHDSecurityBand Band = A.Band;
	const int32 B = BandIndex(Band);
	FHDEncounter E;
	E.Site = Site;
	E.Type = Site->Site.Type;
	E.Band = Band;
	E.System = A.System;

	auto Guard = [&](EHDNpcPop Pop, FName Hull, FName Faction, float Radius, bool bHostileList) -> AHDNpcController*
	{
		AHDNpcController* N = SpawnNpc(A.System, C + R.GetUnitVector() * R.FRandRange(50000.f, 400000.f), Pop, Hull, Faction);
		if (!N) return nullptr;
		N->bAmbient = false;
		N->Site = Site;
		N->Home = C;
		N->GuardRadius = Radius;
		if (AHDShipPawn* S = N->GetShip()) OutActors.Add(S);
		if (bHostileList) E.Hostiles.Add(N); else E.Protect.Add(N);
		return N;
	};
	auto Crate = [&](const FString& Name, const TArray<FHDItemStack>& Loot, bool bChart)
	{
		TArray<FName> None;
		AHDWreckActor* W = UHDEconomySubsystem::SpawnWreck(World, A.System, C + R.GetUnitVector() * R.FRandRange(20000.f, 150000.f), NAME_None, None, 4, 0, false);
		if (!W) return;
		W->MakeCrate(Name, bChart);
		W->Loot = Loot;
		W->SalvageTollOwner = Site->TollOwner;
		OutActors.Add(W);
	};
	auto Stack = [](const TCHAR* Item, int32 Qty, uint8 Flags = 0) { FHDItemStack S; S.Item = FName(Item); S.Qty = Qty; S.Flags = Flags; return S; };

	switch (Site->Site.Type)
	{
	case EHDSiteType::SignalFire:
	{
		// A convoy that stopped answering: traders already losing when you arrive.
		for (int32 i = 0; i < 2; ++i)
		{
			if (AHDNpcController* T = Guard(EHDNpcPop::Trader, PickHull(EHDNpcPop::Trader, Band, R), AccordKey, 1.0e6f, false))
			{
				if (AHDShipPawn* S = T->GetShip()) S->HullHP *= 0.6f;
			}
		}
		const EHDNpcPop Raider = B >= 2 ? EHDNpcPop::Pirate : EHDNpcPop::Rogue;
		const int32 N = 2 + B;
		AHDNpcController* Lead = nullptr;
		for (int32 i = 0; i < N; ++i)
		{
			AHDNpcController* H = Guard(Raider, PickHull(Raider, Band, R, i == 0 && Raider == EHDNpcPop::Pirate), Raider == EHDNpcPop::Pirate ? PirateFaction(A.System) : FName(TEXT("rogue")), 3.0e6f, true);
			if (!H) continue;
			if (Raider == EHDNpcPop::Rogue) H->FleeHullFraction = 0.25f;
			if (i == 0) Lead = H; else if (Lead && Raider == EHDNpcPop::Pirate) { H->Leader = Lead; H->bHadLeader = true; }
		}
		break;
	}
	case EHDSiteType::ContrabandDrift:
	{
		// Unmarked cargo and a fence finishing the transfer. Scoop it, fight it, or report it.
		TArray<FHDItemStack> Loot;
		Loot.Add(Stack(R.FRand() < 0.5f ? TEXT("contraband_narcotics") : TEXT("contraband_weapons"), R.RandRange(3, 6 + 4 * B), HDItemFlags::Contraband));
		Loot.Add(Stack(TEXT("luxury_goods"), R.RandRange(5, 20), HDItemFlags::Stolen));
		Crate(TEXT("Unmarked cargo"), Loot, false);
		Crate(TEXT("Cut-open hauler cargo"), TArray<FHDItemStack>({ Stack(TEXT("consumer_goods"), R.RandRange(10, 30), HDItemFlags::Stolen) }), false);
		for (int32 i = 0; i < 1 + B / 2; ++i) Guard(EHDNpcPop::Rogue, PickHull(EHDNpcPop::Rogue, Band, R), TEXT("rogue"), 2.0e6f, true);
		break;
	}
	case EHDSiteType::RogueNest:
	{
		// A squadron that stopped pretending. They fight for the nest (no flee) and never drop a rebuildable hull.
		const int32 N = 3 + 2 * B;
		for (int32 i = 0; i < N; ++i)
		{
			if (AHDNpcController* H = Guard(EHDNpcPop::Rogue, PickHull(EHDNpcPop::Rogue, Band, R), TEXT("rogue"), 4.0e6f, true)) H->FleeHullFraction = 0.f;
		}
		// The Black: a nest may field a capital-scale NPC hull (the fight Class 4 used to promise).
		if (Band == EHDSecurityBand::Frontier && R.FRand() < 0.25f) Guard(EHDNpcPop::Rogue, TEXT("monolith"), TEXT("rogue"), 6.0e6f, true);
		break;
	}
	case EHDSiteType::DeadSignature:
	{
		// Components, data, and occasionally a chart toward a real wormhole or ruin gate.
		const float ChartChance = B >= 3 ? 0.4f : B == 2 ? 0.15f : 0.05f;
		const int32 N = R.RandRange(2, 3);
		for (int32 i = 0; i < N; ++i)
		{
			TArray<FHDItemStack> Loot;
			Loot.Add(Stack(TEXT("signal_data"), R.RandRange(1, 2 + B)));
			Loot.Add(Stack(R.FRand() < 0.3f ? TEXT("jump_core") : TEXT("salvage_electronics"), R.RandRange(1, 3 + B)));
			Crate(TEXT("Ancient buoy"), Loot, R.FRand() < ChartChance);
		}
		if (B >= 2 && R.FRand() < 0.3f) Guard(EHDNpcPop::Shadow, TEXT("whisper"), ShadowKey, 2.0e6f, true);
		break;
	}
	case EHDSiteType::PirateBase:
	{
		AHDNpcController* Lead = Guard(EHDNpcPop::Pirate, PickHull(EHDNpcPop::Pirate, Band, R, true), PirateFaction(A.System), 1.0e7f, true);
		for (int32 i = 0; i < 3 + B; ++i)
		{
			AHDNpcController* W = Guard(EHDNpcPop::Pirate, PickHull(EHDNpcPop::Pirate, Band, R), PirateFaction(A.System), 1.0e7f, true);
			if (W && Lead) { W->Leader = Lead; W->bHadLeader = true; }
		}
		break;
	}
	case EHDSiteType::SentinelVault:
	{
		// Guard; never pursue, never retreat.
		for (int32 i = 0; i < 2 + B; ++i) Guard(EHDNpcPop::Sentinel, PickHull(EHDNpcPop::Sentinel, Band, R, i == 0), TEXT("sentinel"), 5.0e6f, true);
		break;
	}
	case EHDSiteType::DeepCore:
	{
		const EHDNpcPop Pop = B >= 2 ? EHDNpcPop::Pirate : EHDNpcPop::Rogue;
		for (int32 i = 0; i < 2; ++i) Guard(Pop, PickHull(Pop, Band, R), Pop == EHDNpcPop::Pirate ? PirateFaction(A.System) : FName(TEXT("rogue")), 4.0e6f, true);
		break;
	}
	case EHDSiteType::Hulk:
	{
		if (B >= 2 && R.FRand() < 0.3f) Guard(EHDNpcPop::Rogue, PickHull(EHDNpcPop::Rogue, Band, R), TEXT("rogue"), 3.0e6f, true);
		break;
	}
	default: break;
	}
	if (E.Hostiles.Num() > 0 || E.Protect.Num() > 0)
	{
		Encounters.RemoveAll([Site](const FHDEncounter& X) { return X.Site.Get() == Site; });
		Encounters.Add(E);
	}
}

void UHDNpcSubsystem::TickEncounters(FHDActiveSystem& A)
{
	for (int32 I = Encounters.Num() - 1; I >= 0; --I)
	{
		FHDEncounter& E = Encounters[I];
		AHDSiteActor* Site = E.Site.Get();
		if (!Site) { Encounters.RemoveAt(I); continue; }
		if (E.System != A.System || E.bResolved) continue;
		int32 HostilesAlive = 0, ProtectAlive = 0;
		for (const TWeakObjectPtr<AHDNpcController>& H : E.Hostiles) { if (H.IsValid() && H->GetShip()) ++HostilesAlive; }
		for (const TWeakObjectPtr<AHDNpcController>& P : E.Protect) { if (P.IsValid() && P->GetShip()) ++ProtectAlive; }
		// Payout steps up by band: Secured is the floor of the table (doc 20).
		const int64 Pay = (int64)(3000 * (1 + 2 * BandIndex(E.Band)));
		if (E.Protect.Num() > 0 && ProtectAlive == 0)
		{
			E.bResolved = true;
			NotifyNear(GetWorld(), A.System, Site->GetActorLocation(), 5.0e7, TEXT("Signal fire: the convoy is lost."));
			continue;
		}
		if (E.Hostiles.Num() > 0 && HostilesAlive == 0)
		{
			E.bResolved = true;
			for (TActorIterator<AHDShipPawn> It(GetWorld()); It; ++It)
			{
				if (!It->IsPlayerControlled() || It->SystemIndex != A.System || FVector::Dist(It->GetActorLocation(), Site->GetActorLocation()) > 5.0e7) continue;
				AHDPlayerState* PS = It->GetPlayerState<AHDPlayerState>();
				if (!PS) continue;
				FString Msg;
				switch (E.Type)
				{
				case EHDSiteType::SignalFire:
					PS->AddCredits(Pay + 1000 * ProtectAlive);
					PS->AddStanding(AccordKey, 0.1f);
					Msg = FString::Printf(TEXT("Signal fire: convoy saved (%d survived). +%lld cr, Accord standing up."), ProtectAlive, Pay + 1000 * ProtectAlive);
					break;
				case EHDSiteType::RogueNest:
					PS->AddCredits(Pay);
					Msg = FString::Printf(TEXT("Rogue nest cleared. +%lld cr"), Pay);
					break;
				case EHDSiteType::SentinelVault:
					Msg = TEXT("Vault guardians down. The vault is open.");
					break;
				case EHDSiteType::PirateBase:
					Msg = TEXT("Pirate garrison destroyed: the base is undefended (claim it with: capture).");
					break;
				default:
					Msg = TEXT("Site clear.");
					break;
				}
				NotifyPilot(*It, Msg);
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Security: legality, crimes, Accord response, contraband scans (doc 15, 18)
// ---------------------------------------------------------------------------------------------
bool UHDNpcSubsystem::CheckAttack(AHDShipPawn* Victim, AController* Inst, AActor* Causer)
{
	AHDShipPawn* Attacker = Cast<AHDShipPawn>(Causer);
	if (!Attacker && Inst) Attacker = Cast<AHDShipPawn>(Inst->GetPawn());
	if (!Victim || !Attacker || Attacker == Victim || !Attacker->IsPlayerControlled()) return true; // NPCs and hazards always hit
	AHDPlayerState* APS = Attacker->GetPlayerState<AHDPlayerState>();
	if (!APS) return true;
	const EHDSecurityBand Band = BandOf(Victim->SystemIndex);

	if (AHDNpcController* VNC = Cast<AHDNpcController>(Victim->GetController()))
	{
		if (!VNC->IsLawful()) return true;
		// Attacking lawful traffic: a crime in Secured / Restricted, a standing hit everywhere.
		if (Band <= EHDSecurityBand::Restricted)
		{
			ReportCrime(Attacker, FString::Printf(TEXT("attacking %s"), AHDNpcController::PopName(VNC->Pop)), Band == EHDSecurityBand::Secured ? 1800.f : 900.f);
		}
		return true;
	}
	AHDPlayerState* VPS = Victim->GetPlayerState<AHDPlayerState>();
	if (!VPS || VPS == APS) return true;
	if (VPS->IsCriminal()) return true;                 // criminals are fair game
	if (APS->bDuelFlag && VPS->bDuelFlag) return true;  // mutual duel flags
	const AHDGameState* GS = GetWorld() ? GetWorld()->GetGameState<AHDGameState>() : nullptr;
	const bool bWar = GS && GS->AreAtWar(APS->Corp, VPS->Corp);
	bool bLegal = false;
	FString Why;
	switch (Band)
	{
	case EHDSecurityBand::Secured: Why = TEXT("Secured space: no PVP (duel flag only)"); break;
	case EHDSecurityBand::Restricted: bLegal = bWar; Why = TEXT("Restricted space: PVP only under a declared war (48 h notice)"); break;
	case EHDSecurityBand::Borderland: bLegal = bWar || IsInClaim(Victim->SystemIndex, Victim->GetActorLocation()); Why = TEXT("Borderland: PVP only inside a claimed volume"); break;
	default: bLegal = true; break;
	}
	if (!bLegal)
	{
		const double Now = AHDPlayerState::NowUnix();
		double& Last = LastBlockedNotify.FindOrAdd(APS->PilotId);
		if (Now - Last > 5.0) { Last = Now; NotifyPilot(Attacker, FString::Printf(TEXT("Weapons safe: %s"), *Why)); }
	}
	return bLegal;
}

void UHDNpcSubsystem::ReportCrime(AHDShipPawn* Criminal, const FString& What, float Seconds)
{
	AHDPlayerState* PS = Criminal ? Criminal->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!PS) return;
	const bool bWas = PS->IsCriminal();
	PS->MarkCriminal(Seconds);
	PS->AddStanding(AccordKey, bWas ? -0.01f : -0.5f);
	const EHDSecurityBand Band = BandOf(Criminal->SystemIndex);
	if (!bWas) NotifyPilot(Criminal, FString::Printf(TEXT("CRIMINAL: %s. You are a legal target for %.0f min."), *What, Seconds / 60.f));
	if (Band > EHDSecurityBand::Restricted) return;
	// The Accord answers in lawful space; throttle to one dispatch per minute per pilot.
	const double Now = AHDPlayerState::NowUnix();
	double& Last = LastResponseUnix.FindOrAdd(PS->PilotId);
	if (Now - Last < 60.0) return;
	Last = Now;
	const float Delay = Band == EHDSecurityBand::Secured ? 8.f : 25.f;
	const int32 Count = Band == EHDSecurityBand::Secured ? 3 : 2;
	NotifyPilot(Criminal, FString::Printf(TEXT("Accord security responding in %.0f s"), Delay));
	TWeakObjectPtr<AHDShipPawn> Weak(Criminal);
	TWeakObjectPtr<UHDNpcSubsystem> Self(this);
	FTimerHandle H;
	GetWorld()->GetTimerManager().SetTimer(H, FTimerDelegate::CreateLambda([Self, Weak, Count]()
	{
		if (Self.IsValid()) Self->DispatchResponse(Weak, Count);
	}), Delay, false);
}

void UHDNpcSubsystem::DispatchResponse(TWeakObjectPtr<AHDShipPawn> Criminal, int32 Count)
{
	AHDShipPawn* S = Criminal.Get();
	AHDPlayerState* PS = S ? S->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!S || !PS || !PS->IsCriminal() || S->HullHP <= 0.f) return;
	FRandomStream R(FMath::Rand());
	for (int32 i = 0; i < Count; ++i)
	{
		const FVector Loc = S->GetActorLocation() + R.GetUnitVector() * R.FRandRange(300000.f, 800000.f);
		AHDNpcController* C = SpawnNpc(S->SystemIndex, Loc, EHDNpcPop::Accord, PickHull(EHDNpcPop::Accord, EHDSecurityBand::Secured, R, true), AccordKey);
		if (!C) continue;
		C->bAmbient = false;
		C->Pursuit = S;
		C->Target = S;
		C->State = EHDNpcState::Engage;
	}
	NotifyPilot(S, TEXT("Accord security on grid."));
}

void UHDNpcSubsystem::ContrabandScans(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Pilots)
{
	if (A.Band > EHDSecurityBand::Restricted) return;
	UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(this);
	if (!Cat) return;
	TArray<FVector> Scanners;
	for (TActorIterator<AHDNpcController> It(GetWorld()); It; ++It)
	{
		const AHDShipPawn* S = It->GetShip();
		if (S && S->SystemIndex == A.System && (It->Pop == EHDNpcPop::Accord || It->Pop == EHDNpcPop::Patrol)) Scanners.Add(S->GetActorLocation());
	}
	const double Now = AHDPlayerState::NowUnix();
	for (AHDShipPawn* P : Pilots)
	{
		AHDPlayerState* PS = P->GetPlayerState<AHDPlayerState>();
		if (!PS || !P->Cargo) continue;
		bool bNear = false;
		for (const FVector& L : Scanners) { if (FVector::Dist(L, P->GetActorLocation()) < 3.0e6f) { bNear = true; break; } }
		if (!bNear) continue;
		double& Last = LastScanUnix.FindOrAdd(PS->PilotId);
		if (Now - Last < 300.0) continue;
		Last = Now;
		int64 Value = 0;
		int32 Seized = 0;
		for (int32 I = P->Cargo->Items.Num() - 1; I >= 0; --I)
		{
			const FHDItemStack S = P->Cargo->Items[I];
			const FHDItemDef* D = Cat->FindItem(S.Item);
			const bool bContra = (S.Flags & HDItemFlags::Contraband) != 0 || (D && D->Category == FName(TEXT("Contraband")));
			const bool bStolen = (S.Flags & HDItemFlags::Stolen) != 0;
			if (!bContra && !bStolen) continue;
			Value += (int64)(D ? D->Price : 100) * S.Qty;
			Seized += S.Qty;
			P->Cargo->Items.RemoveAt(I);
		}
		if (Seized == 0) { NotifyPilot(P, TEXT("Accord cargo scan: clean.")); continue; }
		const int64 Fine = Value / 4;
		if (PS->TrySpend(Fine))
		{
			NotifyPilot(P, FString::Printf(TEXT("Accord cargo scan: %d units of contraband / marked goods seized, fined %lld cr."), Seized, Fine));
			PS->AddStanding(AccordKey, -0.2f);
		}
		else
		{
			ReportCrime(P, TEXT("smuggling (fine unpaid)"), 600.f);
		}
	}
}

void UHDNpcSubsystem::OnShadowRevealed(AHDNpcController* Shadow, AHDShipPawn* Pilot)
{
	AHDPlayerState* PS = Pilot ? Pilot->GetPlayerState<AHDPlayerState>() : nullptr;
	AHDSystemDirector* D = AHDSystemDirector::Get(this);
	if (!PS || !D || ShadowContracts.Contains(PS->PilotId)) return;
	// Offer: survey a site in this system. Pays in standing and information.
	const FHDActiveSystem* A = D->FindActive(Pilot->SystemIndex);
	if (!A || A->Sites.Num() == 0) return;
	const TWeakObjectPtr<AHDSiteActor>& W = A->Sites[FMath::RandRange(0, A->Sites.Num() - 1)];
	if (!W.IsValid()) return;
	FHDShadowContract K;
	K.SiteId = W->ObjectId;
	K.System = Pilot->SystemIndex;
	K.SiteName = W->DisplayName;
	K.ExpiresUnix = AHDPlayerState::NowUnix() + 3600.0;
	ShadowContracts.Add(PS->PilotId, K);
	NotifyPilot(Pilot, FString::Printf(TEXT("An unregistered ship decloaks. \"Survey %s for us. Come within 50 km and send: shadow deliver.\" (1 h)"), *K.SiteName));
}

void UHDNpcSubsystem::HandleShipDestroyed(AHDShipPawn* Victim, AController* Killer)
{
	AHDNpcController* VNC = Victim ? Cast<AHDNpcController>(Victim->GetController()) : nullptr;
	AHDPlayerState* KPS = Killer ? Killer->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!VNC || !KPS) return;
	switch (VNC->Pop)
	{
	case EHDNpcPop::Rogue: KPS->AddStanding(AccordKey, 0.02f); break;
	case EHDNpcPop::Pirate: KPS->AddStanding(AccordKey, 0.05f); KPS->AddStanding(VNC->Faction, -0.2f); break;
	case EHDNpcPop::Patrol: KPS->AddStanding(VNC->Faction, -1.f); break;
	case EHDNpcPop::Trader: KPS->AddStanding(AccordKey, -0.3f); break;
	case EHDNpcPop::Accord: KPS->AddStanding(AccordKey, -2.f); break;
	case EHDNpcPop::Shadow: KPS->AddStanding(ShadowKey, -1.f); break;
	default: break;
	}
}
