#include "HDSystemDirector.h"
#include "HDUniverseSubsystem.h"
#include "HDWorldActors.h"
#include "HDShipPawn.h"
#include "HDShipMovementComponent.h"
#include "HDNavComponent.h"
#include "HDTargetDrone.h"
#include "HDGameMode.h"
#include "HDEconomy.h"
#include "HDCatalog.h"
#include "HDFittingComponent.h"
#include "HDWreckActor.h"
#include "HDNpcSubsystem.h"
#include "HeliosDrift.h"
#include "Engine/World.h"
#include "EngineUtils.h"

AHDSystemDirector::AHDSystemDirector()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.25f;
	bReplicates = false;
	SetHidden(true);
}

void AHDSystemDirector::BeginPlay()
{
	Super::BeginPlay();
	if (UHDUniverseSubsystem* U = Universe()) { LastWeek = U->GetWeekIndex(); }
}

AHDSystemDirector* AHDSystemDirector::Get(const UObject* WorldContext)
{
	UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	AHDGameMode* GM = World ? World->GetAuthGameMode<AHDGameMode>() : nullptr;
	return GM ? GM->GetDirector() : nullptr;
}

UHDUniverseSubsystem* AHDSystemDirector::Universe() const
{
	return UHDUniverseSubsystem::Get(this);
}

FVector AHDSystemDirector::SlotOrigin(int32 Slot) const
{
	const int32 G = FMath::Max(1, SlotGrid);
	const int32 X = Slot % G, Y = Slot / G;
	const double Half = (G - 1) * 0.5;
	return FVector((X - Half) * SlotSpacing, (Y - Half) * SlotSpacing, SlotZ);
}

int32 AHDSystemDirector::FreeSlot() const
{
	const int32 MaxSlots = FMath::Max(1, SlotGrid * SlotGrid);
	for (int32 s = 0; s < MaxSlots; ++s)
	{
		bool bUsed = false;
		for (const FHDActiveSystem& A : Active) { if (A.Slot == s) { bUsed = true; break; } }
		if (!bUsed) return s;
	}
	return -1;
}

const FHDActiveSystem* AHDSystemDirector::FindActive(int32 System) const
{
	for (const FHDActiveSystem& A : Active) { if (A.System == System) return &A; }
	return nullptr;
}

FHDActiveSystem* AHDSystemDirector::FindActive(int32 System)
{
	for (FHDActiveSystem& A : Active) { if (A.System == System) return &A; }
	return nullptr;
}

FVector AHDSystemDirector::GetOrigin(int32 System) const
{
	const FHDActiveSystem* A = FindActive(System);
	return A ? A->Origin : FVector::ZeroVector;
}

bool AHDSystemDirector::EnsureActive(int32 System)
{
	UHDUniverseSubsystem* U = Universe();
	if (!U || !U->IsValidSystem(System)) return false;
	if (FindActive(System)) return true;
	const int32 Slot = FreeSlot();
	if (Slot < 0)
	{
		UE_LOG(LogHelios, Warning, TEXT("Director: no free slot for system %d (all %d busy)"), System, SlotGrid * SlotGrid);
		return false;
	}
	FHDActiveSystem A;
	A.System = System;
	A.Slot = Slot;
	A.Origin = SlotOrigin(Slot);
	const FHDStarSystem& S = U->GetSystem(System);
	A.Band = S.Band;
	A.Region = S.Region;
	const int32 Idx = Active.Add(A);
	SpawnSystemContent(Active[Idx]);
	UE_LOG(LogHelios, Log, TEXT("Director: activated %s (%s, %s) in slot %d: %d bodies, %d stations, %d links, %d sites, %d lanes, %d fields"),
		*S.Name, *S.Id.ToString(), HDUniverse::BandName(S.Band), Slot, S.Bodies.Num(), S.Stations.Num(), Active[Idx].JumpPoints.Num(), S.Sites.Num(), S.Lanes.Num(), S.Fields.Num());
	return true;
}

void AHDSystemDirector::SpawnSystemContent(FHDActiveSystem& A)
{
	UWorld* World = GetWorld();
	UHDUniverseSubsystem* U = Universe();
	if (!World || !U) return;
	const FHDStarSystem& S = U->GetSystem(A.System);
	const FVector O = A.Origin;

	auto Deferred = [World](UClass* Class, const FVector& Loc, const FRotator& Rot) -> AActor*
	{
		const FTransform T(Rot, Loc);
		return World->SpawnActorDeferred<AActor>(Class, T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	};

	for (int32 b = 0; b < S.Bodies.Num(); ++b)
	{
		const FHDBody& B = S.Bodies[b];
		AHDCelestialBody* Act = Cast<AHDCelestialBody>(Deferred(AHDCelestialBody::StaticClass(), O + B.LocalPos, FRotator::ZeroRotator));
		if (!Act) continue;
		const EHDGasDensity D = B.Type == EHDBodyType::GasGiant ? U->GetGasDensity(A.System, b) : EHDGasDensity::Consistent;
		Act->InitBody(A.System, b, B, D);
		Act->FinishSpawning(FTransform(FRotator::ZeroRotator, O + B.LocalPos));
		A.Bodies.Add(Act);
		A.Persistent.Add(Act);
	}
	for (int32 s = 0; s < S.Stations.Num(); ++s)
	{
		const FHDStation& St = S.Stations[s];
		AHDStationActor* Act = Cast<AHDStationActor>(Deferred(AHDStationActor::StaticClass(), O + St.LocalPos, FRotator::ZeroRotator));
		if (!Act) continue;
		Act->InitStation(A.System, s, St);
		Act->FinishSpawning(FTransform(FRotator::ZeroRotator, O + St.LocalPos));
		A.Stations.Add(Act);
		A.Persistent.Add(Act);
	}
	TArray<FHDJumpLink> Links = U->GetLinks(A.System);
	Links.Append(S.LocalLinks);
	for (const FHDJumpLink& L : Links)
	{
		const FRotator Rot = L.LocalPos.IsNearlyZero() ? FRotator::ZeroRotator : (-L.LocalPos).Rotation();
		AHDJumpPointActor* Act = Cast<AHDJumpPointActor>(Deferred(AHDJumpPointActor::StaticClass(), O + L.LocalPos, Rot));
		if (!Act) continue;
		FString Dest;
		EHDSecurityBand DestBand = S.Band;
		if (U->IsValidSystem(L.To) && L.Type != EHDLinkType::InstanceLane)
		{
			const FHDStarSystem& D = U->GetSystemSkeleton(L.To);
			Dest = D.Name;
			DestBand = D.Band;
		}
		else if (L.Type == EHDLinkType::InstanceLane && S.Sites.IsValidIndex(L.TargetSite))
		{
			Dest = FString::Printf(TEXT("%s pocket"), HDUniverse::SiteTypeName(S.Sites[L.TargetSite].Type));
		}
		Act->InitLink(A.System, L, Dest, DestBand);
		Act->FinishSpawning(FTransform(Rot, O + L.LocalPos));
		A.JumpPoints.Add(Act);
		A.Persistent.Add(Act);
	}
	for (int32 i = 0; i < S.Sites.Num(); ++i)
	{
		const FHDSite& Site = S.Sites[i];
		AHDSiteActor* Act = Cast<AHDSiteActor>(Deferred(AHDSiteActor::StaticClass(), O + Site.LocalPos, FRotator::ZeroRotator));
		if (!Act) continue;
		Act->InitSite(A.System, i, Site, S.Band);
		Act->FinishSpawning(FTransform(FRotator::ZeroRotator, O + Site.LocalPos));
		A.Sites.Add(Act);
		A.Persistent.Add(Act);
	}
	for (const FHDTradeLane& L : S.Lanes)
	{
		AHDTradeLaneActor* Act = Cast<AHDTradeLaneActor>(Deferred(AHDTradeLaneActor::StaticClass(), O + L.StartLocal, FRotator::ZeroRotator));
		if (!Act) continue;
		Act->InitLane(A.System, L, O);
		Act->FinishSpawning(FTransform(FRotator::ZeroRotator, O + L.StartLocal));
		A.Lanes.Add(Act);
		A.Persistent.Add(Act);
	}
	for (const FHDField& F : S.Fields)
	{
		FHDFieldStream FS;
		FS.Field = F;
		FS.World = O + F.LocalPos;
		A.Fields.Add(FS);
	}

	// M1 sandbox targets near the start station so the flight tests work in an empty level.
	if (bSpawnTestDrones && A.System == U->GetStartSystem())
	{
		const FVector Arr = O + GetArrivalLocal(A.System);
		const FVector Offsets[] = { FVector(8000.f, -3000.f, 0.f), FVector(12000.f, 3000.f, 500.f), FVector(15000.f, 0.f, -800.f), FVector(0.f, 2000000.f, 0.f) };
		for (const FVector& Off : Offsets)
		{
			FActorSpawnParameters P;
			P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			if (AHDTargetDrone* D = World->SpawnActor<AHDTargetDrone>(AHDTargetDrone::StaticClass(), Arr + Off, FRotator::ZeroRotator, P))
			{
				D->SetNetCullDistanceSquared(1.0e14f);
				A.Persistent.Add(D);
			}
		}
	}
}

void AHDSystemDirector::Deactivate(int32 ActiveIndex)
{
	if (!Active.IsValidIndex(ActiveIndex)) return;
	FHDActiveSystem& A = Active[ActiveIndex];
	for (FHDFieldStream& F : A.Fields) { for (TWeakObjectPtr<AHDAsteroidActor>& R : F.Rocks) { if (R.IsValid()) R->Destroy(); } }
	for (TWeakObjectPtr<AHDSiteActor>& S : A.Sites) { if (S.IsValid()) ClearSiteContents(S.Get()); }
	for (TWeakObjectPtr<AActor>& T : A.Transient) { if (T.IsValid()) T->Destroy(); }
	for (TWeakObjectPtr<AActor>& P : A.Persistent) { if (P.IsValid()) P->Destroy(); }
	UE_LOG(LogHelios, Log, TEXT("Director: deactivated system %d (slot %d)"), A.System, A.Slot);
	Active.RemoveAt(ActiveIndex);
}

void AHDSystemDirector::RebuildSystem(int32 System)
{
	for (int32 i = 0; i < Active.Num(); ++i)
	{
		if (Active[i].System != System) continue;
		const int32 Slot = Active[i].Slot;
		const FVector Origin = Active[i].Origin;
		for (FHDFieldStream& F : Active[i].Fields) { for (TWeakObjectPtr<AHDAsteroidActor>& R : F.Rocks) { if (R.IsValid()) R->Destroy(); } }
		for (TWeakObjectPtr<AHDSiteActor>& S : Active[i].Sites) { if (S.IsValid()) ClearSiteContents(S.Get()); }
		for (TWeakObjectPtr<AActor>& P : Active[i].Persistent) { if (P.IsValid()) P->Destroy(); }
		FHDActiveSystem Fresh;
		Fresh.System = System; Fresh.Slot = Slot; Fresh.Origin = Origin;
		Fresh.Band = Active[i].Band; Fresh.Region = Active[i].Region;
		Fresh.Transient = Active[i].Transient;
		Active[i] = Fresh;
		SpawnSystemContent(Active[i]);
		return;
	}
}

FVector AHDSystemDirector::GetArrivalLocal(int32 System) const
{
	UHDUniverseSubsystem* U = Universe();
	if (!U || !U->IsValidSystem(System)) return FVector(1.0e10, 0.0, 0.0);
	const FHDStarSystem& S = U->GetSystem(System);
	for (const FHDStation& St : S.Stations)
	{
		if (!St.bFuelStation) return St.LocalPos + FVector(-2.0e6, 0.0, 0.0); // 20 km off the station, facing it
	}
	return FVector(2.0 * U->AUcm, 0.0, 0.0);
}

FVector AHDSystemDirector::GetLinkArrivalLocal(int32 System, const FName& LinkId) const
{
	UHDUniverseSubsystem* U = Universe();
	if (!U || !U->IsValidSystem(System)) return FVector::ZeroVector;
	for (const FHDJumpLink& L : U->GetLinks(System))
	{
		if (L.Id == LinkId)
		{
			const FVector Out = L.LocalPos.GetSafeNormal();
			return L.LocalPos - Out * 1.5e6; // 15 km inside the mouth, toward the star
		}
	}
	return GetArrivalLocal(System);
}

bool AHDSystemDirector::PlaceShip(AHDShipPawn* Ship, int32 System, const FVector& LocalPos, const FRotator& Rot)
{
	if (!Ship || !EnsureActive(System)) return false;
	const FHDActiveSystem* A = FindActive(System);
	if (!A) return false;
	Ship->SetSystem(System, A->Origin);
	if (Ship->Nav) { Ship->Nav->DockedAt = nullptr; Ship->Nav->LandedOn = nullptr; }
	if (Ship->Movement && Ship->Movement->GetTravelMode() != EHDTravelMode::Normal) { Ship->Movement->SetModeAuthority(EHDTravelMode::Normal); }
	if (Ship->Movement) { Ship->Movement->TeleportAuthority(A->Origin + LocalPos, Rot.Quaternion(), true); }
	else { Ship->SetActorLocationAndRotation(A->Origin + LocalPos, Rot); }
	return true;
}

TArray<AHDShipPawn*> AHDSystemDirector::GetShipsInSystem(int32 System) const
{
	TArray<AHDShipPawn*> Out;
	UWorld* World = GetWorld();
	if (!World) return Out;
	for (TActorIterator<AHDShipPawn> It(World); It; ++It)
	{
		if (It->SystemIndex == System && !It->IsPendingKillPending()) Out.Add(*It);
	}
	return Out;
}

AHDJumpPointActor* AHDSystemDirector::FindJumpPoint(int32 System, const FName& LinkId) const
{
	if (const FHDActiveSystem* A = FindActive(System))
	{
		for (const TWeakObjectPtr<AHDJumpPointActor>& J : A->JumpPoints) { if (J.IsValid() && J->Link.Id == LinkId) return J.Get(); }
	}
	return nullptr;
}

AHDStationActor* AHDSystemDirector::FindStation(int32 System, const FName& StationId) const
{
	if (const FHDActiveSystem* A = FindActive(System))
	{
		for (const TWeakObjectPtr<AHDStationActor>& S : A->Stations) { if (S.IsValid() && (StationId.IsNone() || S->Info.Id == StationId)) return S.Get(); }
	}
	return nullptr;
}

AHDSiteActor* AHDSystemDirector::FindSite(int32 System, int32 SiteIndex) const
{
	if (const FHDActiveSystem* A = FindActive(System))
	{
		for (const TWeakObjectPtr<AHDSiteActor>& S : A->Sites) { if (S.IsValid() && S->SiteIndex == SiteIndex) return S.Get(); }
	}
	return nullptr;
}

void AHDSystemDirector::AddTransient(int32 System, AActor* Actor)
{
	if (!Actor) return;
	// An actor belongs to one system: moving (e.g. a towed wreck) must not leave it in the old system's cleanup list.
	for (FHDActiveSystem& Other : Active)
	{
		if (Other.System != System) Other.Transient.RemoveAll([Actor](const TWeakObjectPtr<AActor>& P) { return P.Get() == Actor; });
	}
	if (FHDActiveSystem* A = FindActive(System)) { A->Transient.AddUnique(Actor); }
}

void AHDSystemDirector::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority()) return;
	UHDUniverseSubsystem* U = Universe();

	// Weekly re-roll: gas-giant densities and wormholes change; rebuild active systems.
	WeekCheck -= DeltaSeconds;
	if (U && WeekCheck <= 0.f)
	{
		WeekCheck = 10.f;
		const int64 Week = U->GetWeekIndex();
		if (Week != LastWeek)
		{
			UE_LOG(LogHelios, Log, TEXT("Director: week %lld -> %lld, re-rolling gas giants and wormholes"), LastWeek, Week);
			LastWeek = Week;
			TArray<int32> Ids;
			for (const FHDActiveSystem& A : Active) Ids.Add(A.System);
			for (int32 Id : Ids) RebuildSystem(Id);
		}
	}

	for (int32 i = Active.Num() - 1; i >= 0; --i)
	{
		FHDActiveSystem& A = Active[i];
		A.Persistent.RemoveAll([](const TWeakObjectPtr<AActor>& P) { return !P.IsValid(); });
		A.Transient.RemoveAll([](const TWeakObjectPtr<AActor>& P) { return !P.IsValid(); });
		const TArray<AHDShipPawn*> Ships = GetShipsInSystem(A.System);
		int32 Pilots = 0;
		for (AHDShipPawn* S : Ships) { if (S->IsPlayerControlled()) ++Pilots; }
		A.Pilots = Pilots;
		if (Pilots == 0)
		{
			A.EmptyTime += DeltaSeconds;
			if (A.EmptyTime > EmptySystemGrace) { Deactivate(i); continue; }
		}
		else
		{
			A.EmptyTime = 0.f;
		}
		StreamFields(A, Ships, DeltaSeconds);
		StreamSites(A, Ships, DeltaSeconds);
		TickAmbient(A, Ships, DeltaSeconds);
	}

	if (UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(this)) Econ->Tick(DeltaSeconds, GetWorld());
}

void AHDSystemDirector::StreamFields(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Ships, float Dt)
{
	const UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(this);
	const float Richness = Econ ? Econ->GetRegionRichness(A.Region) : 1.f;
	for (FHDFieldStream& F : A.Fields)
	{
		// Mined-out rocks return at about one per minute in an untouched region, slower where mining is heavy (doc 16).
		if (F.RespawnDebt > 0.f) F.RespawnDebt = FMath::Max(0.f, F.RespawnDebt - Dt * Richness / 60.f);
		bool bNear = false;
		for (const AHDShipPawn* S : Ships)
		{
			if (S->IsPlayerControlled() && FVector::DistSquared(S->GetActorLocation(), F.World) < FieldStreamRadius * FieldStreamRadius) { bNear = true; break; }
		}
		if (bNear)
		{
			F.EmptyTime = 0.f;
			if (!F.bStreamedIn) { SpawnFieldRocks(A, F); F.bStreamedIn = true; }
		}
		else if (F.bStreamedIn)
		{
			F.EmptyTime += Dt;
			if (F.EmptyTime > StreamOutGrace)
			{
				int32 Depleted = 0;
				for (TWeakObjectPtr<AHDAsteroidActor>& R : F.Rocks) { if (R.IsValid()) { if (R->IsDepleted()) ++Depleted; R->Destroy(); } }
				F.Rocks.Reset();
				F.RespawnDebt += Depleted;
				F.bStreamedIn = false;
			}
		}
	}
}

void AHDSystemDirector::SpawnFieldRocks(FHDActiveSystem& A, FHDFieldStream& F)
{
	UWorld* World = GetWorld();
	if (!World) return;
	FRandomStream R((int32)HDUniverse::Mix(GetTypeHash(F.Field.Id), (uint32)LastWeek));
	// Mined-out rocks come back slowly, and heavily mined regions stream in thinner belts (doc 16).
	const UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(this);
	const float Richness = Econ ? Econ->GetRegionRichness(A.Region) : 1.f;
	const int32 Count = FMath::Max(4, FMath::RoundToInt(RocksPerField * Richness) - FMath::FloorToInt(F.RespawnDebt));
	for (int32 i = 0; i < Count; ++i)
	{
		const FVector Loc = F.World + R.GetUnitVector() * R.FRandRange(5000.f, 3.0e6f) * FVector(1.0, 1.0, 0.25);
		const float Radius = F.Field.Type == EHDFieldType::GasCloud ? R.FRandRange(20000.f, 60000.f) : R.FRandRange(1500.f, 12000.f);
		const bool bSecondary = R.FRand() < 0.3f;
		const FName Item = bSecondary ? F.Field.SecondaryItem : F.Field.PrimaryItem;
		const float Units = (Radius / 100.f) * 8.f * F.Field.Richness;
		AHDAsteroidActor* Rock = World->SpawnActorDeferred<AHDAsteroidActor>(AHDAsteroidActor::StaticClass(), FTransform(Loc), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Rock) continue;
		Rock->InitDeposit(A.System, F.Field.Type, Item, Units, Radius, false);
		Rock->FieldId = F.Field.Id;
		Rock->RegionIndex = A.Region;
		Rock->FinishSpawning(FTransform(R.GetUnitVector().Rotation(), Loc));
		F.Rocks.Add(Rock);
	}
}

void AHDSystemDirector::StreamSites(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Ships, float Dt)
{
	for (TWeakObjectPtr<AHDSiteActor>& W : A.Sites)
	{
		AHDSiteActor* Site = W.Get();
		if (!Site) continue;
		bool bNear = false;
		for (const AHDShipPawn* S : Ships)
		{
			if (S->IsPlayerControlled() && FVector::DistSquared(S->GetActorLocation(), Site->GetActorLocation()) < SiteStreamRadius * SiteStreamRadius) { bNear = true; break; }
		}
		if (bNear)
		{
			Site->EmptyTime = 0.f;
			if (!Site->bStreamedIn && !Site->bCleared) { SpawnSiteContents(A, Site); Site->bStreamedIn = true; }
		}
		else if (Site->bStreamedIn)
		{
			Site->EmptyTime += Dt;
			if (Site->EmptyTime > StreamOutGrace) { ClearSiteContents(Site); Site->bStreamedIn = false; }
		}
	}
}

void AHDSystemDirector::ClearSiteContents(AHDSiteActor* Site)
{
	if (!Site) return;
	// A site whose wrecks were picked clean (or rocks mined out) stays cleared until the weekly re-roll,
	// so streaming out and back can't refill it.
	int32 Workable = 0, Total = 0;
	for (const TObjectPtr<AActor>& C : Site->Contents)
	{
		AActor* Act = C.Get();
		if (!IsValid(Act)) { ++Total; continue; } // destroyed guard / collapsed wreck
		++Total;
		if (const AHDWreckActor* W = Cast<AHDWreckActor>(Act)) { if (W->Grade < 5 || W->Loot.Num() > 0) ++Workable; }
		else if (const AHDAsteroidActor* R = Cast<AHDAsteroidActor>(Act)) { if (!R->IsDepleted()) ++Workable; }
		else ++Workable; // living NPCs, structures
	}
	if (Total > 0 && Workable == 0) Site->bCleared = true;
	for (TObjectPtr<AActor>& C : Site->Contents) { if (IsValid(C)) C->Destroy(); }
	Site->Contents.Reset();
}

void AHDSystemDirector::SpawnSiteContents(FHDActiveSystem& A, AHDSiteActor* Site)
{
	UWorld* World = GetWorld();
	if (!World || !Site) return;
	FRandomStream R((int32)HDUniverse::Mix(GetTypeHash(Site->ObjectId), (uint32)LastWeek));
	const FVector C = Site->GetActorLocation();
	if (Site->Site.Type == EHDSiteType::DeepCore)
	{
		// Deep-core anomalies mix combat with extraction: rich rocks (guards are added by the encounter layer).
		for (int32 i = 0; i < 6; ++i)
		{
			const FVector Loc = C + R.GetUnitVector() * R.FRandRange(20000.f, 400000.f);
			AHDAsteroidActor* Rock = World->SpawnActorDeferred<AHDAsteroidActor>(AHDAsteroidActor::StaticClass(), FTransform(Loc), nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
			if (!Rock) continue;
			Rock->InitDeposit(A.System, EHDFieldType::AsteroidBelt, A.Band >= EHDSecurityBand::Borderland ? FName(TEXT("ore_exotic")) : FName(TEXT("ore_crystal")), 2500.f * Site->Site.Difficulty, R.FRandRange(6000.f, 15000.f), true);
			Rock->RegionIndex = A.Region;
			Rock->FinishSpawning(FTransform(Loc));
			Site->Contents.Add(Rock);
		}
	}
	else if (Site->Site.Type == EHDSiteType::Hulk || Site->Site.Type == EHDSiteType::DerelictStation || Site->Site.Type == EHDSiteType::SentinelVault)
	{
		// Hulks: graded wrecks by class (doc 16). Class 1 small hulls ... Class 4 capitals. Grades B-D for hulks,
		// D-E for station derelicts and sentinel vaults (ancient, mostly materials and relics).
		const UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(this);
		if (!Cat) return;
		const int32 HClass = FMath::Clamp(Site->Site.HulkClass, 1, 4);
		const bool bAncient = Site->Site.Type != EHDSiteType::Hulk;
		TArray<FName> Pool;
		for (const TPair<FName, FHDHullDef>& H : Cat->GetHulls())
		{
			const EHDHullClass K = H.Value.Class;
			int32 Want = 1;
			if (K >= EHDHullClass::Capital) Want = 4;
			else if (K >= EHDHullClass::Battlecruiser) Want = 3;
			else if (K == EHDHullClass::Cruiser) Want = 2;
			if (Want == HClass && H.Value.Supply != EHDSupplyTier::Advanced) Pool.Add(H.Key);
		}
		if (Pool.Num() == 0) { for (const TPair<FName, FHDHullDef>& H : Cat->GetHulls()) Pool.Add(H.Key); }
		if (Pool.Num() == 0) return;
		Pool.Sort([](const FName& L, const FName& Rn) { return L.LexicalLess(Rn); }); // deterministic with the seed
		const int32 Num = bAncient ? 1 : (HClass >= 3 ? 1 : R.RandRange(1, 3));
		for (int32 i = 0; i < Num; ++i)
		{
			const FName Hull = Pool[R.RandRange(0, Pool.Num() - 1)];
			TArray<FName> Mods = UHDFittingComponent::SuggestFit(Cat, Hull, R.FRand() < 0.5f);
			Mods.RemoveAll([](const FName& M) { return M.IsNone(); });
			const int32 Grade = bAncient ? R.RandRange(3, 4) : R.RandRange(1, 3);
			const FVector Loc = C + R.GetUnitVector() * R.FRandRange(10000.f, 200000.f);
			AHDWreckActor* W = UHDEconomySubsystem::SpawnWreck(World, A.System, Loc, Hull, Mods, Grade, HClass, Site->Site.Type == EHDSiteType::DerelictStation);
			if (!W) continue;
			W->SalvageTollOwner = Site->TollOwner;
			Site->Contents.Add(W);
		}
	}
	// Guards, convoys, raiders and loot containers for encounter anomalies and guarded sites.
	if (UHDNpcSubsystem* Npc = UHDNpcSubsystem::Get(this))
	{
		TArray<AActor*> Spawned;
		Npc->SpawnSiteEncounter(A, Site, R, Spawned);
		for (AActor* Act : Spawned) Site->Contents.Add(Act);
	}
}

void AHDSystemDirector::TickAmbient(FHDActiveSystem& A, const TArray<AHDShipPawn*>& Ships, float Dt)
{
	// Ambient traffic, antagonists with economic spawn pressure, encounters and security (doc 15, 18, 20).
	if (UHDNpcSubsystem* Npc = UHDNpcSubsystem::Get(this)) Npc->TickSystem(A, Ships, Dt);
}
