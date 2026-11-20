#include "HDTerritory.h"
#include "HDStructureActor.h"
#include "HDShipPawn.h"
#include "HDShipTypes.h"
#include "HDShipMovementComponent.h"
#include "HDFittingComponent.h"
#include "HDCargoComponent.h"
#include "HDNavComponent.h"
#include "HDCatalog.h"
#include "HDPlayerState.h"
#include "HDPlayerController.h"
#include "HDWorldActors.h"
#include "HDWreckActor.h"
#include "HDEconomy.h"
#include "HDNpcController.h"
#include "HDNpcSubsystem.h"
#include "HDSystemDirector.h"
#include "HDUniverseSubsystem.h"
#include "HeliosDrift.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameStateBase.h"
#include "EngineUtils.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr double TerrDay = 86400.0;
	constexpr double TerrHour = 3600.0;
	// DB reference point (doc 15, FIRST PASS): 120M output, 400M trade, 60 residents -> ~50k cr/day;
	// doubling all three gives ~1.45x, i.e. exponent log2(1.45) ~ 0.536 on the mean input ratio.
	constexpr double TerrRefOutput = 120.0e6, TerrRefTrade = 400.0e6, TerrRefResidents = 60.0, TerrRefDb = 50000.0, TerrDbExponent = 0.536;

	AHDPlayerState* TerrFindPilot(UWorld* World, const FString& PilotOrName)
	{
		AGameStateBase* GS = World ? World->GetGameState() : nullptr;
		if (!GS) return nullptr;
		for (APlayerState* P : GS->PlayerArray)
		{
			AHDPlayerState* H = Cast<AHDPlayerState>(P);
			if (H && (H->PilotId == PilotOrName || H->GetPlayerName() == PilotOrName)) return H;
		}
		return nullptr;
	}

	void TerrNotifyPS(AHDPlayerState* PS, const FString& Msg)
	{
		if (AHDPlayerController* PC = PS ? Cast<AHDPlayerController>(PS->GetOwner()) : nullptr) PC->Notify(Msg);
	}

	void TerrNotifyCorp(UWorld* World, const FName& Corp, const FString& Msg)
	{
		AGameStateBase* GS = World ? World->GetGameState() : nullptr;
		if (!GS || Corp.IsNone()) return;
		for (APlayerState* P : GS->PlayerArray)
		{
			AHDPlayerState* H = Cast<AHDPlayerState>(P);
			if (H && H->Corp == Corp) TerrNotifyPS(H, Msg);
		}
	}

	double TerrNow() { return AHDPlayerState::NowUnix(); }

	/** Next occurrence of a UTC hour at least MinAhead seconds from now. */
	double TerrNextWindow(int32 HourUtc, double MinAhead)
	{
		const double Now = TerrNow();
		double DayStart = FMath::FloorToDouble(Now / TerrDay) * TerrDay;
		double T = DayStart + FMath::Clamp(HourUtc, 0, 23) * TerrHour;
		while (T < Now + MinAhead) T += TerrDay;
		return T;
	}
}

UHDTerritorySubsystem* UHDTerritorySubsystem::Get(const UObject* WorldContext)
{
	if (!WorldContext || !GEngine) return nullptr;
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UHDTerritorySubsystem>() : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Companies
// ---------------------------------------------------------------------------------------------
FHDCorp* UHDTerritorySubsystem::CorpOf(const AHDPlayerState* PS)
{
	return PS && !PS->Corp.IsNone() ? Corps.Find(PS->Corp) : nullptr;
}

bool UHDTerritorySubsystem::IsCeo(const AHDPlayerState* PS) const
{
	const FHDCorp* C = PS && !PS->Corp.IsNone() ? Corps.Find(PS->Corp) : nullptr;
	return C && C->Ceo == PS->PilotId;
}

void UHDTerritorySubsystem::AddTollIncome(const FName& Owner, int64 Credits)
{
	// Tolls are income to the owner and never count toward the claim's Defense Budget (doc 15).
	if (FHDCorp* C = Corps.Find(Owner)) { C->Treasury += Credits; C->TollIncome += Credits; }
}

// ---------------------------------------------------------------------------------------------
// Claims and the Defense Budget (doc 15 §7)
// ---------------------------------------------------------------------------------------------
FHDClaim* UHDTerritorySubsystem::ClaimInSystem(int32 System)
{
	for (TPair<FName, FHDClaim>& P : Claims) { if (P.Value.System == System) return &P.Value; }
	return nullptr;
}

const FHDClaim* UHDTerritorySubsystem::ClaimInSystem(int32 System) const
{
	for (const TPair<FName, FHDClaim>& P : Claims) { if (P.Value.System == System) return &P.Value; }
	return nullptr;
}

float UHDTerritorySubsystem::BandCeiling(EHDSecurityBand Band)
{
	switch (Band)
	{
	case EHDSecurityBand::Restricted: return 70000.f;
	case EHDSecurityBand::Borderland: return 100000.f;
	case EHDSecurityBand::Frontier: return 150000.f;
	default: return 0.f; // Secured is not on the ladder
	}
}

int32 UHDTerritorySubsystem::Berths(const FHDClaim& C) const
{
	int32 B = 10; // the beacon's own berths
	for (const TPair<FName, FHDStructure>& P : Structures)
	{
		const FHDStructure& S = P.Value;
		if (S.System != C.System || S.bDerelict || S.Corp != C.Corp) continue;
		if (S.Type == EHDStructureType::Habitat) B += 40;
		else if (S.Type == EHDStructureType::Station) B += 60;
	}
	for (const TPair<FName, FHDColony>& P : Colonies)
	{
		if (P.Value.System == C.System && P.Value.Corp == C.Corp) { const int32* H = P.Value.Buildings.Find(TEXT("habitat")); B += H ? *H * 20 : 0; }
	}
	return B;
}

int32 UHDTerritorySubsystem::ColonyResidents(int32 System) const
{
	float N = 0.f;
	for (const TPair<FName, FHDColony>& P : Colonies) { if (P.Value.System == System) N += P.Value.Population / 1000.f; }
	return FMath::FloorToInt(N);
}

int32 UHDTerritorySubsystem::ResidentCount(const FHDClaim& C) const
{
	// Registered residency over the last 14 days plus colonists, capped per structure berth (anti-bloat).
	const double Cut = TerrNow() - 14.0 * TerrDay;
	int32 N = 0;
	for (const TPair<FString, double>& P : C.Residents) { if (P.Value >= Cut) ++N; }
	N += ColonyResidents(C.System);
	return FMath::Min(N, Berths(C));
}

float UHDTerritorySubsystem::RawDefenseBudget(const FHDClaim& C) const
{
	const double Mean = (C.Output30 / TerrRefOutput + C.Trade30 / TerrRefTrade + ResidentCount(C) / TerrRefResidents) / 3.0;
	return Mean <= 0.0 ? 0.f : (float)(TerrRefDb * FMath::Pow(Mean, TerrDbExponent));
}

float UHDTerritorySubsystem::DefenseBudget(const FHDClaim& C) const
{
	// Anti-spike: DB = min(24 h rolling average, 7-day rolling average). A war chest dumped in changes nothing for a week.
	float Db = RawDefenseBudget(C);
	if (C.DbSamples.Num() > 0)
	{
		double S24 = 0.0, S7 = 0.0;
		const int32 N = C.DbSamples.Num();
		const int32 N24 = FMath::Min(24, N);
		for (int32 i = 0; i < N; ++i) { S7 += C.DbSamples[i]; if (i >= N - N24) S24 += C.DbSamples[i]; }
		// Missing history counts as zero for the 7-day window: a brand-new claim ramps up over its first week.
		Db = (float)FMath::Min(S24 / N24, S7 / 168.0);
	}
	else
	{
		Db = 0.f;
	}
	return FMath::Min(Db, BandCeiling(C.Band));
}

float UHDTerritorySubsystem::UpkeepCredits(EHDStructureType Type)
{
	switch (Type)
	{
	case EHDStructureType::Beacon: return 3000.f;
	case EHDStructureType::GunPlacement: return 1500.f;
	case EHDStructureType::DefensePlatform: return 4000.f;
	case EHDStructureType::Habitat: return 1000.f;
	case EHDStructureType::Station: return 6000.f;
	default: return 2000.f;
	}
}

float UHDTerritorySubsystem::UpkeepPerDay(const FHDClaim& C) const
{
	float U = 0.f;
	for (const TPair<FName, FHDStructure>& P : Structures)
	{
		if (P.Value.System == C.System && P.Value.Corp == C.Corp && !P.Value.bDerelict) U += UpkeepCredits(P.Value.Type) + 500.f * P.Value.Sections.Num();
	}
	return U;
}

bool UHDTerritorySubsystem::SetAllocation(FHDClaim& C, int32 Tier, float CrPerDay, FString& OutWhy)
{
	if (Tier < 0 || Tier > 4) { OutWhy = TEXT("Tier is 1-5"); return false; }
	CrPerDay = FMath::Max(0.f, CrPerDay);
	const float Db = DefenseBudget(C);
	const float Avail = Db - UpkeepPerDay(C); // upkeep is paid off the top before hire
	float Others = 0.f;
	for (int32 i = 0; i < 5; ++i) { if (i != Tier) Others += C.Alloc[i]; }
	if (Tier == 4 && CrPerDay > 0.5f * Db)
	{
		OutWhy = FString::Printf(TEXT("Tier 5 (hired players) is capped at 50%% of the Defense Budget: max %.0f cr/day"), 0.5f * Db);
		return false;
	}
	if (Tier == 2 && CrPerDay > 0.f)
	{
		// Tier 3 is capitals: players only. The allocation is a fuel/ammo stipend to the corp's capital pilots in the claim.
	}
	if (Others + CrPerDay > Avail)
	{
		OutWhy = FString::Printf(TEXT("Over budget: DB %.0f - upkeep %.0f = %.0f available, other tiers use %.0f"), Db, UpkeepPerDay(C), Avail, Others);
		return false;
	}
	if (Tier == 4)
	{
		int64 Committed = 0;
		for (const FHDHire& H : C.Hires) Committed += H.RatePerDay;
		if (CrPerDay < Committed) { OutWhy = FString::Printf(TEXT("Active hires already commit %lld cr/day"), Committed); return false; }
	}
	C.Alloc[Tier] = CrPerDay;
	return true;
}

bool UHDTerritorySubsystem::Hire(FHDClaim& C, AHDPlayerState* Pilot, int64 RatePerDay, float Days, FString& OutWhy)
{
	if (!Pilot) { OutWhy = TEXT("Pilot not online"); return false; }
	if (Pilot->Corp == C.Corp) { OutWhy = TEXT("Members defend for free; hire outsiders"); return false; }
	// Vetting is standing minimums only (no refusal lists).
	if (Pilot->GetStanding(TEXT("accord")) < C.HireMinStanding) { OutWhy = FString::Printf(TEXT("Below the claim's standing minimum (%.1f)"), C.HireMinStanding); return false; }
	int64 Committed = 0;
	for (const FHDHire& H : C.Hires) Committed += H.RatePerDay;
	// Escrow: payment comes out of DB before the fight. If tier 5 can't cover it, there is no contract.
	if ((float)(Committed + RatePerDay) > C.Alloc[4]) { OutWhy = FString::Printf(TEXT("Tier 5 budget can't cover it: %.0f allocated, %lld committed"), C.Alloc[4], Committed); return false; }
	FHDHire H;
	H.Pilot = Pilot->PilotId;
	H.RatePerDay = RatePerDay;
	H.UntilUnix = TerrNow() + FMath::Clamp(Days, 0.1f, 30.f) * TerrDay;
	C.Hires.Add(H);
	return true;
}

FString UHDTerritorySubsystem::DescribeClaim(const FHDClaim& C) const
{
	const float Db = DefenseBudget(C);
	FString S = FString::Printf(TEXT("Claim %s by %s, %s. DB %.0f / %.0f cr/day (raw %.0f; inputs: output %.1fM, trade %.1fM, residents %d / %d berths)\n"),
		*C.Id.ToString(), *C.Corp.ToString(), HDUniverse::BandName(C.Band), Db, BandCeiling(C.Band), RawDefenseBudget(C), C.Output30 / 1.0e6, C.Trade30 / 1.0e6, ResidentCount(C), Berths(C));
	S += FString::Printf(TEXT("Upkeep %.0f off the top. Tiers: 1 guns %.0f | 2 platforms %.0f | 3 capital stipend %.0f | 4 NPC-flown %.0f | 5 hired %.0f (cap %.0f)\n"),
		UpkeepPerDay(C), C.Alloc[0], C.Alloc[1], C.Alloc[2], C.Alloc[3], C.Alloc[4], 0.5f * Db);
	S += FString::Printf(TEXT("Hires %d, NPC defenders %d, window %02d:00 UTC%s%s"), C.Hires.Num(), C.Defenders.Num(), C.VulnHourUtc,
		C.bUndefended ? TEXT(", UNDEFENDED") : TEXT(""), C.bThinGarrison ? TEXT(", THIN GARRISON") : TEXT(""));
	return S;
}

void UHDTerritorySubsystem::AddIndustrialOutput(int32 System, double Credits)
{
	if (FHDClaim* C = ClaimInSystem(System)) C->Output30 += Credits;
}

void UHDTerritorySubsystem::AddTrade(int32 System, double Credits)
{
	if (FHDClaim* C = ClaimInSystem(System)) C->Trade30 += Credits;
}

void UHDTerritorySubsystem::TickClaim(UWorld* World, FHDClaim& C, float Hours)
{
	const double Now = TerrNow();
	// 30-day windows as exponential decay (time constant 30 days).
	const double K = FMath::Exp(-(double)Hours / (30.0 * 24.0));
	C.Output30 *= K;
	C.Trade30 *= K;
	// Residency: corp members present keep their registration fresh; older than 14 days drops off.
	for (TActorIterator<AHDShipPawn> It(World); It; ++It)
	{
		const AHDPlayerState* PS = It->GetPlayerState<AHDPlayerState>();
		if (PS && It->SystemIndex == C.System && PS->Corp == C.Corp && C.Residents.Contains(PS->PilotId)) C.Residents[PS->PilotId] = Now;
	}
	for (auto It = C.Residents.CreateIterator(); It; ++It) { if (Now - It.Value() > 14.0 * TerrDay) It.RemoveCurrent(); }
	for (auto It = C.Aggressors.CreateIterator(); It; ++It) { if (Now > It.Value()) It.RemoveCurrent(); }

	if (Now - C.LastSampleUnix >= TerrHour)
	{
		C.LastSampleUnix = Now;
		C.DbSamples.Add(RawDefenseBudget(C));
		while (C.DbSamples.Num() > 168) C.DbSamples.RemoveAt(0);
	}

	UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(World);
	// Tier 5: escrowed hires paid pro rata.
	for (int32 i = C.Hires.Num() - 1; i >= 0; --i)
	{
		FHDHire& H = C.Hires[i];
		const int64 Pay = (int64)FMath::FloorToDouble((double)H.RatePerDay * Hours / 24.0);
		if (Pay > 0 && Econ) { Econ->PayPilot(World, H.Pilot, Pay); H.PaidOut += Pay; }
		if (Now > H.UntilUnix) C.Hires.RemoveAt(i);
	}
	// Tier 3: stipend to the company's capital pilots inside the claim.
	if (C.Alloc[2] > 0.f)
	{
		TArray<AHDPlayerState*> Caps;
		for (TActorIterator<AHDShipPawn> It(World); It; ++It)
		{
			AHDPlayerState* PS = It->GetPlayerState<AHDPlayerState>();
			if (PS && It->SystemIndex == C.System && PS->Corp == C.Corp && HDShips::IsCapitalSize(It->HullClass)) Caps.Add(PS);
		}
		for (AHDPlayerState* PS : Caps) PS->AddCredits((int64)(C.Alloc[2] * Hours / 24.f / Caps.Num()));
	}
	// Tiers 1-2: the allocation buys munitions for the guns and platforms.
	int32 Guns = 0, Platforms = 0;
	for (TPair<FName, FHDStructure>& P : Structures)
	{
		FHDStructure& S = P.Value;
		if (S.System != C.System || S.Corp != C.Corp || S.bDerelict) continue;
		if (S.Type == EHDStructureType::GunPlacement) ++Guns;
		if (S.Type == EHDStructureType::DefensePlatform) ++Platforms;
	}
	for (TPair<FName, FHDStructure>& P : Structures)
	{
		FHDStructure& S = P.Value;
		if (S.System != C.System || S.Corp != C.Corp || S.bDerelict) continue;
		if (S.Type == EHDStructureType::GunPlacement && Guns > 0) S.Ammo = FMath::Min(2000.f, S.Ammo + C.Alloc[0] * Hours / 24.f / 90.f / Guns);
		if (S.Type == EHDStructureType::DefensePlatform && Platforms > 0) S.Ammo = FMath::Min(5000.f, S.Ammo + C.Alloc[1] * Hours / 24.f / 90.f / Platforms);
	}
	// Flags shown on approach.
	const float Db = DefenseBudget(C);
	const float Funded = C.Alloc[0] + C.Alloc[1] + C.Alloc[3] + C.Alloc[4];
	C.bUndefended = Db <= 0.f || Funded < 0.1f * Db || (Guns + Platforms == 0 && C.Defenders.Num() == 0 && C.Hires.Num() == 0);
	C.bThinGarrison = Guns + Platforms >= 6 && ResidentCount(C) < 5;
}

void UHDTerritorySubsystem::TickDefenders(UWorld* World, FHDClaim& C, float Dt)
{
	C.DefenderTimer -= Dt;
	if (C.DefenderTimer > 0.f) return;
	C.DefenderTimer = 30.f;
	C.Defenders.RemoveAll([](const TWeakObjectPtr<AHDNpcController>& W) { return !W.IsValid() || !W->GetShip(); });
	AHDSystemDirector* D = AHDSystemDirector::Get(World);
	UHDNpcSubsystem* Npc = UHDNpcSubsystem::Get(World);
	FHDActiveSystem* A = D ? D->FindActive(C.System) : nullptr;
	const FHDStructure* Beacon = Structures.Find(C.Beacon);
	if (!A || !Npc || !Beacon || A->Pilots == 0) return;
	// Tier 4: patrol fighters (cheap hulls, attrition) and standing-gated NPC squadron wings.
	int32 Want = FMath::Min(12, FMath::FloorToInt(C.Alloc[3] / 1500.f));
	bool bWing = false;
	if (C.Alloc[3] >= 12000.f && !C.WingFaction.IsNone())
	{
		const FHDCorp* Corp = Corps.Find(C.Corp);
		AHDPlayerState* Ceo = Corp ? TerrFindPilot(World, Corp->Ceo) : nullptr;
		bWing = Ceo && Ceo->GetStanding(C.WingFaction) >= 0.f; // a hostile faction refuses the contract
	}
	if (C.Defenders.Num() >= Want) return;
	FRandomStream R(FMath::Rand());
	const FVector Home = A->Origin + Beacon->Local;
	const FName Hull = bWing && C.Defenders.Num() % 3 == 0 ? UHDNpcSubsystem::PickHull(EHDNpcPop::Patrol, C.Band, R, true) : FName(TEXT("lance"));
	if (AHDNpcController* N = Npc->SpawnNpc(C.System, Home + R.GetUnitVector() * 1.0e6f, EHDNpcPop::Patrol, Hull, C.Corp))
	{
		N->DefendCorp = C.Corp;
		N->bAmbient = false;
		N->AggroRange = 1.5e7f;
		for (int32 i = 0; i < 4; ++i) N->Waypoints.Add(Home + FRotator(0.f, 90.f * i, 0.f).Vector() * 3.0e6f);
		C.Defenders.Add(N);
	}
}

bool UHDTerritorySubsystem::IsHostileToCorp(const FName& Corp, const AHDShipPawn* Ship) const
{
	if (!Ship || Corp.IsNone()) return false;
	if (const AHDNpcController* NC = Cast<AHDNpcController>(Ship->GetController()))
	{
		if (NC->DefendCorp == Corp) return false;
		return NC->Pop == EHDNpcPop::Pirate || NC->Pop == EHDNpcPop::Rogue || (NC->Pop == EHDNpcPop::Shadow && NC->bRevealed && NC->LastAttacker.IsValid());
	}
	const AHDPlayerState* PS = Ship->GetPlayerState<AHDPlayerState>();
	if (!PS || PS->Corp == Corp) return false;
	if (PS->IsCriminal()) return true;
	const AHDGameState* GS = Ship->GetWorld() ? Ship->GetWorld()->GetGameState<AHDGameState>() : nullptr;
	if (GS && GS->AreAtWar(Corp, PS->Corp)) return true;
	const FHDClaim* C = ClaimInSystem(Ship->SystemIndex);
	if (C && C->Corp == Corp)
	{
		if (!PS->Corp.IsNone() && C->Kos.Contains(PS->Corp)) return true;
		if (const double* Until = C->Aggressors.Find(PS->PilotId)) return *Until > TerrNow();
	}
	return false;
}

// ---------------------------------------------------------------------------------------------
// Structures (doc 19 §7D)
// ---------------------------------------------------------------------------------------------
FName UHDTerritorySubsystem::KitFor(EHDStructureType Type)
{
	switch (Type)
	{
	case EHDStructureType::Beacon: return TEXT("kit_beacon");
	case EHDStructureType::GunPlacement: return TEXT("kit_gun");
	case EHDStructureType::DefensePlatform: return TEXT("kit_platform");
	case EHDStructureType::Habitat: return TEXT("kit_habitat");
	case EHDStructureType::Refinery: return TEXT("kit_refinery");
	case EHDStructureType::Factory: return TEXT("kit_factory");
	case EHDStructureType::FuelPlatform: return TEXT("kit_fuel_platform");
	case EHDStructureType::Station: return TEXT("kit_station");
	default: return TEXT("kit_interdictor");
	}
}

const TCHAR* UHDTerritorySubsystem::TypeName(EHDStructureType Type)
{
	switch (Type)
	{
	case EHDStructureType::Beacon: return TEXT("Claim beacon");
	case EHDStructureType::GunPlacement: return TEXT("Gun placement");
	case EHDStructureType::DefensePlatform: return TEXT("Defense platform");
	case EHDStructureType::Habitat: return TEXT("Habitat ring");
	case EHDStructureType::Refinery: return TEXT("Orbital refinery");
	case EHDStructureType::Factory: return TEXT("Orbital factory");
	case EHDStructureType::FuelPlatform: return TEXT("Fuel platform");
	case EHDStructureType::Station: return TEXT("Station");
	default: return TEXT("Lane interdictor");
	}
}

float UHDTerritorySubsystem::MaxIntegrityFor(EHDStructureType Type)
{
	switch (Type)
	{
	case EHDStructureType::Beacon: return 200000.f;
	case EHDStructureType::GunPlacement: return 8000.f;
	case EHDStructureType::DefensePlatform: return 40000.f;
	case EHDStructureType::Station: return 300000.f;
	case EHDStructureType::FuelPlatform: return 120000.f;
	default: return 60000.f;
	}
}

float UHDTerritorySubsystem::UpkeepFuelPerDay(const FHDStructure& S)
{
	switch (S.Type)
	{
	case EHDStructureType::Beacon: return 20.f;
	case EHDStructureType::GunPlacement: return 5.f;
	case EHDStructureType::DefensePlatform: return 15.f;
	case EHDStructureType::Station: return 40.f + 5.f * S.Sections.Num();
	case EHDStructureType::FuelPlatform: return 0.f; // fuels itself from the column
	default: return 10.f;
	}
}

float UHDTerritorySubsystem::RoutingEfficiency(const FHDStructure& S)
{
	if (S.Type != EHDStructureType::Station) return 1.f;
	int32 Power = 2, Load = 0;
	for (const FName& N : S.Sections) { if (N == FName(TEXT("power"))) Power += 3; else ++Load; }
	float E = Power >= Load ? 1.f : 0.5f;
	// Kitbash: improvised joins lose ~15% routing efficiency each, past the standard plan.
	const int32 Std = S.Family == EHDStationFamily::CorporateModular ? 8 : 6;
	if (S.Family == EHDStationFamily::PirateKitbash && S.Sections.Num() > Std) E *= FMath::Pow(0.85f, (float)(S.Sections.Num() - Std));
	return E;
}

void UHDTerritorySubsystem::ApplyStationSections(FHDStructure& S)
{
	TWeakObjectPtr<AHDStationActor>* W = StationActors.Find(S.StationId);
	AHDStationActor* St = W ? W->Get() : nullptr;
	if (!St) return;
	FHDStation Info = St->Info;
	if (S.Type == EHDStructureType::Station)
	{
		const bool bPower = RoutingEfficiency(S) > 0.5f || S.Sections.Num() == 0;
		Info.bMarket = bPower && S.Sections.Contains(FName(TEXT("market")));
		Info.bRefinery = bPower && S.Sections.Contains(FName(TEXT("refinery")));
		Info.bShipyard = bPower && S.Sections.Contains(FName(TEXT("shipyard")));
		Info.bRepair = bPower && S.Sections.Contains(FName(TEXT("support")));
	}
	St->Info = Info;
}

bool UHDTerritorySubsystem::Anchor(AHDShipPawn* Ship, EHDStructureType Type, FString& OutMsg)
{
	AHDPlayerState* PS = Ship ? Ship->GetPlayerState<AHDPlayerState>() : nullptr;
	UWorld* World = Ship ? Ship->GetWorld() : nullptr;
	AHDSystemDirector* D = AHDSystemDirector::Get(World);
	FHDActiveSystem* A = D ? D->FindActive(Ship->SystemIndex) : nullptr;
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(World);
	if (!PS || !World || !A || !U || !Ship->Cargo || !Ship->Nav) { OutMsg = TEXT("Not available here."); return false; }
	if (PS->Corp.IsNone() || !Corps.Contains(PS->Corp)) { OutMsg = TEXT("Structures belong to companies: corp create <id> <name>"); return false; }
	const FName Kit = KitFor(Type);
	if (Ship->Cargo->Count(Kit) < 1) { OutMsg = FString::Printf(TEXT("Needs a %s in your hold (build it at a factory: build %s)"), *Kit.ToString(), *Kit.ToString()); return false; }
	if (A->Band == EHDSecurityBand::Secured) { OutMsg = TEXT("Secured space is Accord-held: no claims or private structures."); return false; }
	if (Ship->Movement && Ship->Movement->GetTravelMode() != EHDTravelMode::Normal && Ship->Movement->GetTravelMode() != EHDTravelMode::Landed) { OutMsg = TEXT("Drop to sub space first."); return false; }
	FHDClaim* Claim = ClaimInSystem(Ship->SystemIndex);
	if (Type == EHDStructureType::Beacon)
	{
		if (Claim) { OutMsg = FString::Printf(TEXT("Already claimed by %s. Take the beacon down first (siege)."), *Claim->Corp.ToString()); return false; }
	}
	else if (!(Claim && Claim->Corp == PS->Corp) && !(Type == EHDStructureType::FuelPlatform && A->Band == EHDSecurityBand::Frontier))
	{
		OutMsg = TEXT("Anchor a claim beacon here first (structures need your company's claim).");
		return false;
	}
	// Anchor kind: surface (landed), gas-giant upper atmosphere, belt, or dead space.
	EHDAnchor Kind = EHDAnchor::DeadSpace;
	int32 Body = -1;
	double Alt = 0.0;
	AHDCelestialBody* Near = Ship->Nav->FindNearestBody(Alt);
	if (Ship->Nav->LandedOn) { Kind = EHDAnchor::Surface; if (const AHDCelestialBody* B = Cast<AHDCelestialBody>(Ship->Nav->LandedOn)) Body = B->BodyIndex; }
	else if (Near && Near->IsGasGiant() && Alt < 5.0e6) { Kind = EHDAnchor::Atmosphere; Body = Near->BodyIndex; }
	else
	{
		for (const FHDFieldStream& F : A->Fields) { if (FVector::Dist(F.World, Ship->GetActorLocation()) < 3.0e7) { Kind = EHDAnchor::Belt; break; } }
	}
	if (Type == EHDStructureType::FuelPlatform && Kind != EHDAnchor::Atmosphere) { OutMsg = TEXT("Fuel platforms anchor in a gas giant's upper atmosphere (within 50 km of the cloud tops)."); return false; }
	if (Type != EHDStructureType::FuelPlatform && Kind == EHDAnchor::Atmosphere) { OutMsg = TEXT("Only fuel platforms anchor inside a gas giant's atmosphere."); return false; }
	if (Kind == EHDAnchor::Surface && (Type == EHDStructureType::Station || Type == EHDStructureType::Beacon || Type == EHDStructureType::Interdictor)) { OutMsg = TEXT("That anchors in space, not on a surface."); return false; }
	for (const TWeakObjectPtr<AHDStationActor>& St : A->Stations)
	{
		if (St.IsValid() && FVector::Dist(St->GetActorLocation(), Ship->GetActorLocation()) < 1.0e7 && Kind != EHDAnchor::Surface) { OutMsg = TEXT("Too close to a station (keep 100 km)."); return false; }
	}

	Ship->Cargo->Remove(Kit, 1);
	FHDStructure S;
	S.Id = FName(*FString::Printf(TEXT("st-%d-%d"), Ship->SystemIndex, NextId++));
	S.Type = Type;
	S.Anchor = Kind;
	S.Corp = PS->Corp;
	S.System = Ship->SystemIndex;
	S.Local = Ship->GetActorLocation() - A->Origin + Ship->GetActorForwardVector() * (Kind == EHDAnchor::Surface ? 5000.f : 60000.f);
	S.Body = Body;
	S.MaxIntegrity = S.Integrity = MaxIntegrityFor(Type);
	S.Fuel = 50.f;
	S.Ammo = (Type == EHDStructureType::GunPlacement || Type == EHDStructureType::DefensePlatform) ? 200.f : 0.f;
	if (Type == EHDStructureType::Station || Type == EHDStructureType::FuelPlatform)
	{
		S.StationId = FName(*FString::Printf(TEXT("pst-%s"), *S.Id.ToString()));
		S.Family = EHDStationFamily::CorporateModular;
		S.Access = Type == EHDStructureType::FuelPlatform ? EHDAccess::Open : EHDAccess::Corp;
		S.Toll = Type == EHDStructureType::FuelPlatform ? 500 : 0;
	}
	if (Type == EHDStructureType::Beacon)
	{
		FHDClaim C;
		C.Id = FName(*FString::Printf(TEXT("claim-%d"), Ship->SystemIndex));
		C.Corp = PS->Corp;
		C.System = Ship->SystemIndex;
		C.Band = A->Band;
		C.Beacon = S.Id;
		C.LastSampleUnix = 0.0;
		Claims.Add(C.Id, C);
		// Borderland claim owners may toll encounter sites like a lane (doc 20).
		if (A->Band == EHDSecurityBand::Borderland) { for (const TWeakObjectPtr<AHDSiteActor>& Site : A->Sites) { if (Site.IsValid()) Site->TollOwner = PS->Corp; } }
		TerrNotifyCorp(World, PS->Corp, FString::Printf(TEXT("Claim announced on %s (%s). DB ramps up over a week from output, residency and trade."), *U->GetSystem(Ship->SystemIndex).Name, HDUniverse::BandName(A->Band)));
	}
	Structures.Add(S.Id, S);
	FHDStructure& Stored = Structures[S.Id];
	SpawnStructureActor(World, Stored);
	if (!Stored.StationId.IsNone()) SpawnStationProxy(World, Stored);
	static const TCHAR* Kinds[] = { TEXT("dead space"), TEXT("a belt"), TEXT("the surface"), TEXT("the upper atmosphere") };
	OutMsg = FString::Printf(TEXT("%s anchored in %s (%s). Upkeep: %.0f t fuel/day%s. Supply it: supply fuel|ammo <n>"), TypeName(Type), Kinds[(int32)Kind], *S.Id.ToString(),
		UpkeepFuelPerDay(S), S.Ammo > 0.f ? TEXT(" + ammo") : TEXT(""));
	return true;
}

void UHDTerritorySubsystem::SpawnStructureActor(UWorld* World, FHDStructure& S)
{
	AHDSystemDirector* D = AHDSystemDirector::Get(World);
	FHDActiveSystem* A = D ? D->FindActive(S.System) : nullptr;
	if (!A || S.bDerelict) return;
	const FTransform T(FRotator::ZeroRotator, A->Origin + S.Local);
	AHDStructureActor* Act = World->SpawnActorDeferred<AHDStructureActor>(AHDStructureActor::StaticClass(), T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!Act) return;
	Act->InitStructure(S);
	Act->FinishSpawning(T);
	A->Persistent.Add(Act);
	StructureActors.Add(S.Id, Act);
}

void UHDTerritorySubsystem::SpawnStationProxy(UWorld* World, FHDStructure& S)
{
	AHDSystemDirector* D = AHDSystemDirector::Get(World);
	FHDActiveSystem* A = D ? D->FindActive(S.System) : nullptr;
	if (!A || S.bDerelict) return;
	FHDStation Info;
	Info.Id = S.StationId;
	Info.Name = FString::Printf(TEXT("%s %s"), *S.Corp.ToString(), S.Type == EHDStructureType::FuelPlatform ? TEXT("Fuel Platform") : TEXT("Station"));
	Info.Family = S.Family;
	Info.Faction = S.Corp;
	Info.Body = S.Body;
	Info.LocalPos = S.Local;
	Info.bMarket = S.Type == EHDStructureType::FuelPlatform;
	Info.bRepair = false;
	Info.bFuelStation = S.Type == EHDStructureType::FuelPlatform;
	const FTransform T(FRotator::ZeroRotator, A->Origin + S.Local + FVector(0.f, 0.f, 20000.f));
	AHDStationActor* St = World->SpawnActorDeferred<AHDStationActor>(AHDStationActor::StaticClass(), T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!St) return;
	St->InitStation(S.System, -1, Info);
	St->OwnerCorp = S.Corp;
	St->FuelStock = 0.f;
	St->FinishSpawning(T);
	A->Stations.Add(St);
	A->Persistent.Add(St);
	StationActors.Add(S.StationId, St);
	ApplyStationSections(S);
}

void UHDTerritorySubsystem::DropSalvage(UWorld* World, const FHDStructure& S, float Fraction)
{
	// Pressure valve (doc 15): destroyed hardware drops salvage worth ~40% of build cost.
	const UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(World);
	const FHDRecipeDef* R = Cat ? Cat->FindRecipeFor(KitFor(S.Type)) : nullptr;
	AHDSystemDirector* D = AHDSystemDirector::Get(World);
	if (!R || !D || !D->IsActive(S.System)) return;
	TArray<FHDItemStack> Loot;
	for (const TPair<FName, int32>& I : R->Inputs)
	{
		FHDItemStack L;
		L.Item = I.Key;
		L.Qty = FMath::Max(1, FMath::RoundToInt(I.Value * Fraction));
		Loot.Add(L);
	}
	TArray<FName> None;
	if (AHDWreckActor* W = UHDEconomySubsystem::SpawnWreck(World, S.System, D->GetOrigin(S.System) + S.Local, NAME_None, None, 4, 0, false))
	{
		W->MakeCrate(FString::Printf(TEXT("Debris of a %s"), TypeName(S.Type)), false);
		W->Loot = Loot;
	}
}

void UHDTerritorySubsystem::Derelict(UWorld* World, FHDStructure& S, const TCHAR* Why)
{
	// Unmaintained structures decay into derelicts, which feed salvage, which feeds industry (doc 19).
	S.bDerelict = true;
	TerrNotifyCorp(World, S.Corp, FString::Printf(TEXT("%s %s is now a derelict: %s"), TypeName(S.Type), *S.Id.ToString(), Why));
	if (TWeakObjectPtr<AHDStructureActor>* W = StructureActors.Find(S.Id)) { if (W->IsValid()) (*W)->Destroy(); StructureActors.Remove(S.Id); }
	if (TWeakObjectPtr<AHDStationActor>* W = StationActors.Find(S.StationId)) { if (W->IsValid()) (*W)->Destroy(); StationActors.Remove(S.StationId); }
	AHDSystemDirector* D = AHDSystemDirector::Get(World);
	if (D && D->IsActive(S.System))
	{
		UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(World);
		TArray<FName> Mods;
		for (const FName& Sec : S.Sections) { (void)Sec; Mods.Add(TEXT("kit_section")); }
		const bool bBig = S.Type == EHDStructureType::Station || S.Type == EHDStructureType::Beacon || S.Type == EHDStructureType::FuelPlatform;
		if (AHDWreckActor* W = UHDEconomySubsystem::SpawnWreck(World, S.System, D->GetOrigin(S.System) + S.Local, NAME_None, Mods, 2, bBig ? 4 : 2, bBig))
		{
			W->bNoRebuild = true;
			(void)Cat;
		}
	}
	if (S.Type == EHDStructureType::Beacon)
	{
		for (auto It = Claims.CreateIterator(); It; ++It)
		{
			if (It.Value().Beacon == S.Id)
			{
				TerrNotifyCorp(World, It.Value().Corp, FString::Printf(TEXT("Claim %s has fallen."), *It.Value().Id.ToString()));
				for (TWeakObjectPtr<AHDNpcController>& Def : It.Value().Defenders) { if (Def.IsValid()) { if (AHDShipPawn* P = Def->GetShip()) P->Destroy(); Def->Destroy(); } }
				It.RemoveCurrent();
			}
		}
	}
}

float UHDTerritorySubsystem::DamageStructure(AHDStructureActor* Actor, float Amount, AActor* Causer)
{
	FHDStructure* S = Actor ? Structures.Find(Actor->ObjectId) : nullptr;
	UWorld* World = Actor ? Actor->GetWorld() : nullptr;
	if (!S || !World || S->bDerelict) return 0.f;
	AHDShipPawn* By = Cast<AHDShipPawn>(Causer);
	AHDPlayerState* PS = By ? By->GetPlayerState<AHDPlayerState>() : nullptr;
	const double Now = TerrNow();
	FHDClaim* Claim = ClaimInSystem(S->System);
	if (PS)
	{
		if (PS->Corp == S->Corp) return 0.f; // no friendly fire on your own hardware
		const UHDNpcSubsystem* Npc = UHDNpcSubsystem::Get(World);
		const EHDSecurityBand Band = Npc ? Npc->BandOf(S->System) : EHDSecurityBand::Frontier;
		const AHDGameState* GS = World->GetGameState<AHDGameState>();
		if (Band == EHDSecurityBand::Restricted && !(GS && GS->AreAtWar(PS->Corp, S->Corp))) return 0.f; // Restricted: only under war
		// Surface structures can't be reached from orbit: an attacker has to land (doc 19).
		if (S->Anchor == EHDAnchor::Surface && By->Movement && By->Movement->GetTravelMode() != EHDTravelMode::Landed)
		{
			double Alt = 0.0;
			if (By->Nav && By->Nav->FindNearestBody(Alt) && Alt > 300000.0) return 0.f;
		}
		if (Claim) Claim->Aggressors.Add(PS->PilotId, Now + 1800.0);
	}
	if (S->Siege == EHDSiegeState::Reinforced) return 0.f;
	S->LastAttackedUnix = Now;

	// Stations degrade outside-in: the outermost section absorbs damage first and shears off as salvage.
	float Dmg = Amount;
	if (S->Type == EHDStructureType::Station && S->Sections.Num() > 0)
	{
		if (S->SectionHP <= 0.f) S->SectionHP = 20000.f;
		S->SectionHP -= Dmg;
		if (S->SectionHP <= 0.f)
		{
			const FName Lost = S->Sections.Pop();
			S->SectionHP = 20000.f;
			DropSalvage(World, *S, 0.1f);
			ApplyStationSections(*S);
			TerrNotifyCorp(World, S->Corp, FString::Printf(TEXT("Station %s lost a %s section"), *S->Id.ToString(), *Lost.ToString()));
		}
		Actor->SyncFrom(*S);
		return Amount;
	}
	const bool bSiegeable = S->Type == EHDStructureType::Beacon || S->Type == EHDStructureType::Station;
	if (bSiegeable && S->Siege == EHDSiegeState::Normal && S->Integrity - Dmg < 0.5f * S->MaxIntegrity)
	{
		// Reinforced for 24-48 h until the owner's pre-set window; the final battle happens then (doc 06).
		S->Integrity = 0.5f * S->MaxIntegrity;
		S->Siege = EHDSiegeState::Reinforced;
		S->SiegeUntilUnix = TerrNextWindow(Claim ? Claim->VulnHourUtc : 20, TerrDay);
		TerrNotifyCorp(World, S->Corp, FString::Printf(TEXT("%s under siege: REINFORCED until the vulnerability window (in %.1f h)."), TypeName(S->Type), (S->SiegeUntilUnix - Now) / TerrHour));
		if (By) { if (AHDPlayerController* PC = Cast<AHDPlayerController>(By->GetController())) PC->Notify(FString::Printf(TEXT("Target reinforced. Final battle in %.1f h."), (S->SiegeUntilUnix - Now) / TerrHour)); }
		Actor->SyncFrom(*S);
		return Amount;
	}
	S->Integrity -= Dmg;
	Actor->SyncFrom(*S);
	if (S->Integrity <= 0.f)
	{
		TerrNotifyCorp(World, S->Corp, FString::Printf(TEXT("%s %s destroyed"), TypeName(S->Type), *S->Id.ToString()));
		const FName Id = S->Id;
		if (S->Type == EHDStructureType::Beacon || S->Type == EHDStructureType::Station || S->Type == EHDStructureType::FuelPlatform)
		{
			Derelict(World, *S, TEXT("destroyed"));
		}
		else
		{
			DropSalvage(World, *S, 0.4f);
			Actor->Destroy();
			StructureActors.Remove(Id);
			Structures.Remove(Id);
		}
	}
	return Amount;
}

void UHDTerritorySubsystem::TickStructures(UWorld* World, float Dt)
{
	const double Now = TerrNow();
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(World);
	TArray<FName> Ids;
	Structures.GetKeys(Ids);
	for (const FName& Id : Ids)
	{
		FHDStructure* S = Structures.Find(Id);
		if (!S || S->bDerelict) continue;
		// Upkeep: fuel. Empty for 48 h -> derelict.
		S->Fuel = FMath::Max(0.f, S->Fuel - UpkeepFuelPerDay(*S) * Dt / (float)TerrDay);
		if (S->Fuel <= 0.f && UpkeepFuelPerDay(*S) > 0.f)
		{
			if (S->FuelEmptySinceUnix <= 0.0) { S->FuelEmptySinceUnix = Now; TerrNotifyCorp(World, S->Corp, FString::Printf(TEXT("%s %s is out of fuel: 48 h to derelict"), TypeName(S->Type), *S->Id.ToString())); }
			else if (Now - S->FuelEmptySinceUnix > 2.0 * TerrDay) { Derelict(World, *S, TEXT("unmaintained")); continue; }
		}
		else
		{
			S->FuelEmptySinceUnix = 0.0;
		}
		// Fuel platforms: produce at the giant's weekly density (doc 19 §7D.1b); value shifts week to week.
		if (S->Type == EHDStructureType::FuelPlatform)
		{
			TWeakObjectPtr<AHDStationActor>* W = StationActors.Find(S->StationId);
			if (W && W->IsValid() && U)
			{
				const float Mul = UHDUniverseSubsystem::DensityYieldMultiplier(U->GetGasDensity(S->System, S->Body));
				(*W)->FuelStock = FMath::Min(30000.f, (*W)->FuelStock + 3.f * Mul * Dt);
				AddIndustrialOutput(S->System, 3.0 * Mul * Dt * 120.0);
			}
		}
		// Kitbash has no structural floor: stress drains integrity and can shear a section off.
		if (S->Type == EHDStructureType::Station && S->Family == EHDStationFamily::PirateKitbash)
		{
			const int32 Extra = FMath::Max(0, S->Sections.Num() - StandardSections(*S));
			if (Extra > 0)
			{
				S->Integrity = FMath::Max(1.f, S->Integrity - S->MaxIntegrity * 0.01f * Extra * Dt / (float)TerrDay);
				if (FMath::FRand() < 0.03f * Extra * Dt / (float)TerrDay)
				{
					const FName Lost = S->Sections.Pop();
					DropSalvage(World, *S, 0.1f);
					ApplyStationSections(*S);
					TerrNotifyCorp(World, S->Corp, FString::Printf(TEXT("Stress event: station %s sheared off a %s section"), *S->Id.ToString(), *Lost.ToString()));
				}
			}
		}
		// Siege clock.
		if (S->Siege == EHDSiegeState::Reinforced && Now >= S->SiegeUntilUnix)
		{
			S->Siege = EHDSiegeState::Vulnerable;
			S->SiegeUntilUnix = Now + 2.0 * TerrHour;
			TerrNotifyCorp(World, S->Corp, FString::Printf(TEXT("%s %s is VULNERABLE for 2 h: the final battle is now"), TypeName(S->Type), *S->Id.ToString()));
		}
		else if (S->Siege == EHDSiegeState::Vulnerable && Now >= S->SiegeUntilUnix)
		{
			S->Siege = EHDSiegeState::Normal;
			S->Integrity = FMath::Max(S->Integrity, 0.5f * S->MaxIntegrity);
			// The largest bonus goes to defenders for a surviving structure, not for kills (doc 15).
			if (FHDClaim* C = ClaimInSystem(S->System))
			{
				UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(World);
				for (const FHDHire& H : C->Hires) { if (Econ) Econ->PayPilot(World, H.Pilot, H.RatePerDay * 2); }
				for (TActorIterator<AHDShipPawn> It(World); It; ++It)
				{
					AHDPlayerState* PS = It->GetPlayerState<AHDPlayerState>();
					if (PS && It->SystemIndex == S->System && PS->Corp == S->Corp) { PS->AddCredits(10000); TerrNotifyPS(PS, TEXT("Structure held: defender bonus +10,000 cr")); }
				}
			}
			TerrNotifyCorp(World, S->Corp, FString::Printf(TEXT("%s %s survived the siege"), TypeName(S->Type), *S->Id.ToString()));
		}
		if (TWeakObjectPtr<AHDStructureActor>* W = StructureActors.Find(S->Id)) { if (W->IsValid()) (*W)->SyncFrom(*S); }
	}
}

bool UHDTerritorySubsystem::CanDock(const AHDStationActor* St, AHDShipPawn* Ship, int32& OutToll, FString& OutWhy) const
{
	OutToll = 0;
	if (!St || St->OwnerCorp.IsNone()) return true;
	const AHDPlayerState* PS = Ship ? Ship->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!PS) return false;
	if (PS->Corp == St->OwnerCorp) return true;
	if (IsHostileToCorp(St->OwnerCorp, Ship)) { OutWhy = TEXT("Docking refused: you're hostile to the owners."); return false; }
	for (const TPair<FName, FHDStructure>& P : Structures)
	{
		const FHDStructure& S = P.Value;
		if (S.StationId != St->ObjectId) continue;
		switch (S.Access)
		{
		case EHDAccess::Corp: OutWhy = TEXT("Docking refused: company-only."); return false;
		case EHDAccess::Standing:
			if (PS->GetStanding(TEXT("accord")) < S.StandingMin) { OutWhy = FString::Printf(TEXT("Docking refused: standing %.1f required."), S.StandingMin); return false; }
			OutToll = S.Toll;
			return true;
		default: OutToll = S.Toll; return true;
		}
	}
	return true; // captured pirate bases: open
}

bool UHDTerritorySubsystem::Interdict(AHDShipPawn* By, FString& OutMsg)
{
	AHDPlayerState* PS = By ? By->GetPlayerState<AHDPlayerState>() : nullptr;
	UWorld* World = By ? By->GetWorld() : nullptr;
	const UHDNpcSubsystem* Npc = UHDNpcSubsystem::Get(World);
	if (!PS || !World || !Npc || !By->Fitting || !By->Cargo) return false;
	if (!By->Fitting->Stats.bInterdictor) { OutMsg = TEXT("Fit an interdiction_field module."); return false; }
	// Only in claimed volumes or Frontier (doc 15).
	if (Npc->BandOf(By->SystemIndex) != EHDSecurityBand::Frontier && !ClaimInSystem(By->SystemIndex)) { OutMsg = TEXT("Interdiction works only inside claimed volumes or in Frontier."); return false; }
	if (By->Cargo->Count(TEXT("munitions")) < 5) { OutMsg = TEXT("Each pulse costs a module charge: 5 munitions."); return false; }
	By->Cargo->Remove(TEXT("munitions"), 5);
	int32 Pulled = 0;
	for (TActorIterator<AHDShipPawn> It(World); It; ++It)
	{
		AHDShipPawn* S = *It;
		if (S == By || S->SystemIndex != By->SystemIndex || !S->Movement || S->Movement->GetTravelMode() != EHDTravelMode::TradeLane) continue;
		if (FVector::Dist(S->GetActorLocation(), By->GetActorLocation()) > 1.0e7) continue;
		// Suppression bought by the lane owner protects its traffic.
		const AHDTradeLaneActor* Lane = nullptr;
		double Best = TNumericLimits<double>::Max();
		for (TActorIterator<AHDTradeLaneActor> L(World); L; ++L)
		{
			if (L->SystemIndex != S->SystemIndex) continue;
			const FVector P = FMath::ClosestPointOnSegment(S->GetActorLocation(), L->StartWorld, L->EndWorld);
			const double D = FVector::Dist(P, S->GetActorLocation());
			if (D < Best) { Best = D; Lane = *L; }
		}
		if (Lane && Lane->bSuppressed) continue;
		S->Movement->DropOut(true);
		++Pulled;
		if (AHDPlayerController* PC = Cast<AHDPlayerController>(S->GetController())) PC->Notify(TEXT("INTERDICTED: pulled out of the lane"));
		// Lane telemetry shows it to the owner.
		if (Lane && !Lane->Lane.bNPC) TerrNotifyCorp(World, Lane->Lane.Owner, FString::Printf(TEXT("Lane telemetry: interdiction on %s"), *Lane->Lane.Id.ToString()));
	}
	LaneCutUntil.Add(By->SystemIndex, TerrNow() + TerrHour);
	OutMsg = FString::Printf(TEXT("Interdiction pulse: %d ship(s) pulled from lanes"), Pulled);
	return true;
}

// ---------------------------------------------------------------------------------------------
// Colonies (doc 19 §7D.1a)
// ---------------------------------------------------------------------------------------------
FName UHDTerritorySubsystem::ColonyKey(int32 System, int32 Body)
{
	return FName(*FString::Printf(TEXT("colony-%d-%d"), System, Body));
}

const TArray<FName>& UHDTerritorySubsystem::AllBuildings()
{
	static TArray<FName> B;
	if (B.Num() == 0)
	{
		const TCHAR* Names[] = { TEXT("habitat"), TEXT("farm"), TEXT("mine"), TEXT("refinery"), TEXT("factory"), TEXT("fuel_extractor"), TEXT("spaceport"), TEXT("defense_grid"), TEXT("research_lab") };
		for (const TCHAR* N : Names) B.Add(FName(N));
	}
	return B;
}

int64 UHDTerritorySubsystem::BuildingCost(const FName& Building)
{
	const FString S = Building.ToString();
	if (S == TEXT("habitat")) return 60000;
	if (S == TEXT("farm")) return 40000;
	if (S == TEXT("mine")) return 50000;
	if (S == TEXT("refinery")) return 120000;
	if (S == TEXT("factory")) return 150000;
	if (S == TEXT("fuel_extractor")) return 100000;
	if (S == TEXT("spaceport")) return 200000;
	if (S == TEXT("defense_grid")) return 150000;
	if (S == TEXT("research_lab")) return 180000;
	return 0;
}

bool UHDTerritorySubsystem::CanBuild(const FHDBody& B, const FName& Building, FString& OutWhy)
{
	if (B.Type == EHDBodyType::GasGiant) { OutWhy = TEXT("A gas giant can't be settled (its moons can)."); return false; }
	if (B.Type == EHDBodyType::Star) { OutWhy = TEXT("No."); return false; }
	const FString S = Building.ToString();
	if (BuildingCost(Building) <= 0) { OutWhy = TEXT("Unknown building."); return false; }
	if (S == TEXT("mine") && B.Minerals < 0.15f) { OutWhy = TEXT("No workable minerals here."); return false; }
	if (S == TEXT("farm") && (B.Water < 0.3f || B.Fertility < 0.3f)) { OutWhy = TEXT("Can't feed anyone here: needs water and fertile ground."); return false; }
	if (S == TEXT("fuel_extractor") && B.Volatiles < 0.25f) { OutWhy = TEXT("Not enough volatiles."); return false; }
	if (S == TEXT("refinery") && B.Minerals < 0.15f && B.Volatiles < 0.15f) { OutWhy = TEXT("Nothing local to refine."); return false; }
	return true;
}

TArray<FName> UHDTerritorySubsystem::ProposeQueue(const FHDColony& C, const FHDBody& B, FRandomStream& R) const
{
	TArray<FName> Prefs;
	const FString F = C.Focus.ToString();
	auto Add = [&Prefs](const TCHAR* N) { Prefs.Add(FName(N)); };
	if (F == TEXT("Extraction")) { Add(TEXT("mine")); Add(TEXT("fuel_extractor")); }
	else if (F == TEXT("Refining")) { Add(TEXT("refinery")); Add(TEXT("mine")); }
	else if (F == TEXT("Industry")) { Add(TEXT("factory")); Add(TEXT("refinery")); }
	else if (F == TEXT("Trade")) { Add(TEXT("spaceport")); Add(TEXT("habitat")); }
	else if (F == TEXT("Residency")) { Add(TEXT("habitat")); Add(TEXT("farm")); }
	else if (F == TEXT("Defense")) { Add(TEXT("defense_grid")); Add(TEXT("habitat")); }
	else if (F == TEXT("Research")) { Add(TEXT("research_lab")); Add(TEXT("habitat")); }
	else { Add(TEXT("habitat")); Add(TEXT("mine")); Add(TEXT("farm")); Add(TEXT("factory")); }
	TArray<FName> Q;
	const int32 N = 2 + FMath::RoundToInt(C.GovernorQuality * 3.f);
	FString Why;
	if (C.GovernorQuality < 0.5f)
	{
		// A cheap governor misses the geology and overbuilds one sector.
		const FName Pick = Prefs[R.RandRange(0, Prefs.Num() - 1)];
		for (int32 i = 0; i < N; ++i) Q.Add(i < N - 1 ? Pick : Prefs[R.RandRange(0, Prefs.Num() - 1)]);
		return Q;
	}
	TArray<FName> Valid;
	for (const FName& P : Prefs) { if (CanBuild(B, P, Why)) Valid.Add(P); }
	if (Valid.Num() == 0) { for (const FName& P : AllBuildings()) { if (CanBuild(B, P, Why)) Valid.Add(P); } }
	if (Valid.Num() == 0) return Q;
	const int32* Hab = C.Buildings.Find(TEXT("habitat"));
	const int32* Farm = C.Buildings.Find(TEXT("farm"));
	for (int32 i = 0; i < N; ++i)
	{
		FName Pick = Valid[i % Valid.Num()];
		if (C.GovernorQuality >= 0.75f)
		{
			// A great governor reads the world: housing before it's full, food when it can grow food, resources it actually has.
			const float Cap = (Hab ? *Hab : 0) * 1000.f;
			if (C.Population > Cap * 0.8f && CanBuild(B, TEXT("habitat"), Why) && !Q.Contains(FName(TEXT("habitat")))) Pick = TEXT("habitat");
			else if (C.Population > (Farm ? *Farm : 0) * 1500.f && CanBuild(B, TEXT("farm"), Why) && !Q.Contains(FName(TEXT("farm")))) Pick = TEXT("farm");
			else if (B.Minerals > 0.6f && CanBuild(B, TEXT("mine"), Why) && Valid.Contains(FName(TEXT("mine")))) Pick = i % 2 == 0 ? FName(TEXT("mine")) : Pick;
		}
		Q.Add(Pick);
	}
	return Q;
}

double UHDTerritorySubsystem::ColonyDailyOutput(const FHDColony& C, const FHDBody& B) const
{
	auto N = [&C](const TCHAR* K) -> double { const int32* V = C.Buildings.Find(K); return V ? (double)*V : 0.0; };
	const double Jobs = (N(TEXT("mine")) + N(TEXT("refinery")) + N(TEXT("factory")) + N(TEXT("fuel_extractor")) + N(TEXT("spaceport"))) * 500.0;
	const double Staffed = Jobs > 0.0 ? FMath::Min(1.0, (double)C.Population / Jobs) : 0.0;
	return Staffed * (N(TEXT("mine")) * 3000.0 * B.Minerals + N(TEXT("refinery")) * 2000.0 + N(TEXT("factory")) * 3000.0 + N(TEXT("fuel_extractor")) * 2500.0 * B.Volatiles + N(TEXT("spaceport")) * 1500.0);
}

FString UHDTerritorySubsystem::RunColonyWeek(UWorld* World, FHDColony& C)
{
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(World);
	if (!U || C.System < 0) return FString();
	const FHDStarSystem& Sys = U->GetSystem(C.System);
	if (!Sys.Bodies.IsValidIndex(C.Body)) return FString();
	const FHDBody& B = Sys.Bodies[C.Body];
	FHDCorp* Corp = Corps.Find(C.Corp);
	TArray<FString> Lines;
	// Governor: salary, then the (editable) queue within the weekly budget.
	if (C.GovernorQuality > 0.f)
	{
		if (Corp && Corp->Treasury >= C.GovernorSalary) Corp->Treasury -= C.GovernorSalary;
		else { Lines.Add(FString::Printf(TEXT("Governor %s quit: salary unpaid"), *C.Governor)); C.GovernorQuality = 0.f; C.Governor.Reset(); C.Queue.Reset(); }
	}
	int64 Budget = Corp ? FMath::Min<int64>(C.WeeklyBudget, Corp->Treasury) : 0;
	TArray<FName> Left;
	for (const FName& Bld : C.Queue)
	{
		const int64 Cost = BuildingCost(Bld);
		if (Cost > Budget) { Left.Add(Bld); continue; }
		FString Why;
		if (!CanBuild(B, Bld, Why))
		{
			// Builds that ignore the geology fail and waste part of the money.
			Budget -= Cost / 4;
			if (Corp) Corp->Treasury -= Cost / 4;
			Lines.Add(FString::Printf(TEXT("Wasted %lld cr: %s (%s)"), Cost / 4, *Bld.ToString(), *Why));
			continue;
		}
		Budget -= Cost;
		if (Corp) Corp->Treasury -= Cost;
		C.Buildings.FindOrAdd(Bld) += 1;
		Lines.Add(FString::Printf(TEXT("Built %s"), *Bld.ToString()));
	}
	C.Queue = Left;
	// Population: housing and food.
	auto N = [&C](const TCHAR* K) -> int32 { const int32* V = C.Buildings.Find(K); return V ? *V : 0; };
	const float Cap = 200.f + N(TEXT("habitat")) * 1000.f;
	const float Food = N(TEXT("farm")) * 1500.f + 200.f;
	C.Population = FMath::Clamp(C.Population * (C.Population <= Food ? 1.1f : 0.95f), 10.f, Cap);
	// Output, reduced by an orbital blockade (soft counter).
	const float Blockade = C.HoursThisWeek > 0.f ? C.BlockadeHours / C.HoursThisWeek : 0.f;
	const double Week = ColonyDailyOutput(C, B) * 7.0 * (1.0 - 0.5 * Blockade);
	C.LastOutput = Week;
	AddIndustrialOutput(C.System, Week);
	if (UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(World))
	{
		// Produce goods into the owner's colony storage.
		const float Staff = FMath::Clamp(C.Population / FMath::Max(1.f, (N(TEXT("mine")) + N(TEXT("refinery")) + N(TEXT("factory")) + N(TEXT("fuel_extractor"))) * 500.f), 0.f, 1.f) * (1.f - 0.5f * Blockade);
		if (N(TEXT("mine"))) Econ->DeliverToStorage(World, C.Owner, C.Id, TEXT("ore_ferrite"), FMath::RoundToInt(N(TEXT("mine")) * 800 * B.Minerals * Staff), 0);
		if (N(TEXT("refinery"))) Econ->DeliverToStorage(World, C.Owner, C.Id, TEXT("ferrite_ingot"), FMath::RoundToInt(N(TEXT("refinery")) * 150 * Staff), 0);
		if (N(TEXT("factory"))) Econ->DeliverToStorage(World, C.Owner, C.Id, TEXT("structural_frame"), FMath::RoundToInt(N(TEXT("factory")) * 10 * Staff), 0);
		if (N(TEXT("fuel_extractor"))) Econ->DeliverToStorage(World, C.Owner, C.Id, TEXT("jump_fuel"), FMath::RoundToInt(N(TEXT("fuel_extractor")) * 400 * B.Volatiles * Staff), 0);
		if (N(TEXT("farm"))) Econ->DeliverToStorage(World, C.Owner, C.Id, TEXT("food_rations"), FMath::RoundToInt(FMath::Max(0.f, Food - C.Population) * 0.1f), 0);
	}
	Lines.Add(FString::Printf(TEXT("Population %.0f / %.0f, output %.0f cr this week%s"), C.Population, Cap, Week, Blockade > 0.f ? *FString::Printf(TEXT(" (blockaded %.0f%%)"), Blockade * 100.f) : TEXT("")));
	C.BlockadeHours = 0.f;
	C.HoursThisWeek = 0.f;
	C.NextWeekUnix = TerrNow() + 7.0 * TerrDay;
	if (C.GovernorQuality > 0.f)
	{
		FRandomStream R(FMath::Rand());
		C.Queue = ProposeQueue(C, B, R);
		Lines.Add(FString::Printf(TEXT("Governor's proposed queue for next week: %d builds (edit with governor queue/add/remove)"), C.Queue.Num()));
	}
	return FString::Join(Lines, TEXT("\n"));
}

// ---------------------------------------------------------------------------------------------
// Tick, world presence, announcements
// ---------------------------------------------------------------------------------------------
void UHDTerritorySubsystem::AnnounceClaims(UWorld* World)
{
	for (TActorIterator<AHDShipPawn> It(World); It; ++It)
	{
		AHDPlayerState* PS = It->GetPlayerState<AHDPlayerState>();
		if (!PS || !It->IsPlayerControlled()) continue;
		// Company membership is territory state: restore it on a fresh login.
		if (PS->Corp.IsNone())
		{
			for (const TPair<FName, FHDCorp>& CP : Corps) { if (CP.Value.Members.Contains(PS->PilotId)) { PS->Corp = CP.Key; break; } }
		}
		int32& Last = PilotSystem.FindOrAdd(PS->PilotId, -1);
		if (Last == It->SystemIndex) continue;
		Last = It->SystemIndex;
		// The claim announces itself, including whether its garrison is funded (doc 15).
		if (const FHDClaim* C = ClaimInSystem(It->SystemIndex))
		{
			TerrNotifyPS(PS, FString::Printf(TEXT("Entering %s claim (%s%s). Defense budget %.0f cr/day%s%s"), *C->Corp.ToString(), HDUniverse::BandName(C->Band),
				C->Band == EHDSecurityBand::Borderland ? TEXT(": PVP open inside") : TEXT(""), DefenseBudget(*C),
				C->bUndefended ? TEXT(" - UNDEFENDED") : TEXT(""), C->bThinGarrison ? TEXT(" - THIN GARRISON") : TEXT("")));
		}
	}
}

void UHDTerritorySubsystem::Tick(float Dt, UWorld* World)
{
	if (!World) return;
	SecTimer += Dt;
	HourTimer += Dt;
	if (SecTimer >= 1.f)
	{
		const float Step = SecTimer;
		SecTimer = 0.f;
		TickStructures(World, Step);
		for (TPair<FName, FHDClaim>& P : Claims) TickDefenders(World, P.Value, Step);
		AnnounceClaims(World);
	}
	if (HourTimer >= 60.f)
	{
		const float Hours = HourTimer / 3600.f;
		HourTimer = 0.f;
		for (TPair<FName, FHDClaim>& P : Claims) TickClaim(World, P.Value, Hours);
		const double Now = TerrNow();
		// Colonies: blockade sampling and the weekly run.
		for (TPair<FName, FHDColony>& P : Colonies)
		{
			FHDColony& C = P.Value;
			C.HoursThisWeek += Hours;
			UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(World);
			AHDSystemDirector* D = AHDSystemDirector::Get(World);
			if (U && D && D->IsActive(C.System))
			{
				const FHDStarSystem& Sys = U->GetSystem(C.System);
				const FVector BodyW = D->GetOrigin(C.System) + (Sys.Bodies.IsValidIndex(C.Body) ? Sys.Bodies[C.Body].LocalPos : FVector::ZeroVector);
				bool bHostile = false, bFriendly = false;
				for (TActorIterator<AHDShipPawn> It(World); It; ++It)
				{
					if (It->SystemIndex != C.System || FVector::Dist(It->GetActorLocation(), BodyW) > 1.0e7 + (Sys.Bodies.IsValidIndex(C.Body) ? Sys.Bodies[C.Body].VisualRadiusCm : 0.0)) continue;
					if (IsHostileToCorp(C.Corp, *It)) bHostile = true;
					const AHDPlayerState* PS = It->GetPlayerState<AHDPlayerState>();
					if (PS && PS->Corp == C.Corp) bFriendly = true;
				}
				if (bHostile && !bFriendly) C.BlockadeHours += Hours;
			}
			if (Now >= C.NextWeekUnix)
			{
				const FString Report = RunColonyWeek(World, C);
				TerrNotifyCorp(World, C.Corp, FString::Printf(TEXT("Colony %s weekly report:\n%s"), *C.Name, *Report));
			}
		}
		// Lane suppression is a running cost; cut-lane price shocks expire after the hour.
		for (FHDPlayerLane& L : Lanes)
		{
			if (!L.bSuppressed) continue;
			FHDCorp* Corp = Corps.Find(L.Lane.Owner);
			const int64 Cost = (int64)(50000.0 * Hours / 24.0);
			if (Corp && Corp->Treasury >= Cost) Corp->Treasury -= Cost;
			else
			{
				L.bSuppressed = false;
				for (TActorIterator<AHDTradeLaneActor> It(World); It; ++It) { if (It->Lane.Id == L.Lane.Id) It->bSuppressed = false; }
			}
		}
	}
	if (UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(World))
	{
		const double Now = TerrNow();
		for (auto It = LaneCutUntil.CreateIterator(); It; ++It)
		{
			if (Now > It.Value()) { Econ->LaneCutSystems.Remove(It.Key()); It.RemoveCurrent(); }
			else Econ->LaneCutSystems.Add(It.Key());
		}
	}
}

void UHDTerritorySubsystem::SpawnForSystem(FHDActiveSystem& A)
{
	UGameInstance* GI = GetGameInstance();
	UWorld* World = GI ? GI->GetWorld() : nullptr;
	if (!World) return;
	for (TPair<FName, FHDStructure>& P : Structures)
	{
		FHDStructure& S = P.Value;
		if (S.System != A.System || S.bDerelict) continue;
		SpawnStructureActor(World, S);
		if (!S.StationId.IsNone()) SpawnStationProxy(World, S);
	}
	for (FHDPlayerLane& L : Lanes)
	{
		if (L.System != A.System) continue;
		const FTransform T(FRotator::ZeroRotator, A.Origin + L.Lane.StartLocal);
		AHDTradeLaneActor* Act = World->SpawnActorDeferred<AHDTradeLaneActor>(AHDTradeLaneActor::StaticClass(), T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Act) continue;
		Act->InitLane(A.System, L.Lane, A.Origin);
		Act->bSuppressed = L.bSuppressed;
		Act->FinishSpawning(T);
		A.Lanes.Add(Act);
		A.Persistent.Add(Act);
	}
	for (const TWeakObjectPtr<AHDStationActor>& St : A.Stations)
	{
		if (!St.IsValid()) continue;
		if (const FName* Owner = CapturedStations.Find(St->ObjectId)) St->OwnerCorp = *Owner;
	}
	if (const FHDClaim* C = ClaimInSystem(A.System))
	{
		if (C->Band == EHDSecurityBand::Borderland) { for (const TWeakObjectPtr<AHDSiteActor>& Site : A.Sites) { if (Site.IsValid()) Site->TollOwner = C->Corp; } }
	}
}

// ---------------------------------------------------------------------------------------------
// Persistence
// ---------------------------------------------------------------------------------------------
namespace
{
	TSharedRef<FJsonObject> TerrVecJson(const FVector& V)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetNumberField(TEXT("x"), V.X); O->SetNumberField(TEXT("y"), V.Y); O->SetNumberField(TEXT("z"), V.Z);
		return O;
	}
	FVector TerrJsonVec(const TSharedPtr<FJsonObject>& O)
	{
		if (!O.IsValid()) return FVector::ZeroVector;
		return FVector(O->GetNumberField(TEXT("x")), O->GetNumberField(TEXT("y")), O->GetNumberField(TEXT("z")));
	}
	TArray<TSharedPtr<FJsonValue>> TerrNamesJson(const TArray<FName>& L)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FName& N : L) A.Add(MakeShared<FJsonValueString>(N.ToString()));
		return A;
	}
	TArray<FName> TerrJsonNames(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
	{
		TArray<FName> L;
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (O.IsValid() && O->TryGetArrayField(Key, A) && A) { for (const TSharedPtr<FJsonValue>& V : *A) L.Add(FName(*V->AsString())); }
		return L;
	}
	TArray<TSharedPtr<FJsonValue>> TerrStringsJson(const TArray<FString>& L)
	{
		TArray<TSharedPtr<FJsonValue>> A;
		for (const FString& N : L) A.Add(MakeShared<FJsonValueString>(N));
		return A;
	}
	TArray<FString> TerrJsonStrings(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
	{
		TArray<FString> L;
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (O.IsValid() && O->TryGetArrayField(Key, A) && A) { for (const TSharedPtr<FJsonValue>& V : *A) L.Add(V->AsString()); }
		return L;
	}
	double TerrNum(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double Default = 0.0)
	{
		double V = Default;
		if (O.IsValid()) O->TryGetNumberField(Key, V);
		return V;
	}
	FString TerrStr(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
	{
		FString V;
		if (O.IsValid()) O->TryGetStringField(Key, V);
		return V;
	}
}

FString UHDTerritorySubsystem::ExportJson() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> CorpA, ClaimA, StructA, ColA, LaneA, CapA;
	for (const TPair<FName, FHDCorp>& P : Corps)
	{
		const FHDCorp& C = P.Value;
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), C.Id.ToString()); O->SetStringField(TEXT("name"), C.Name); O->SetStringField(TEXT("ceo"), C.Ceo);
		O->SetArrayField(TEXT("members"), TerrStringsJson(C.Members));
		O->SetNumberField(TEXT("treasury"), (double)C.Treasury); O->SetNumberField(TEXT("tollIncome"), (double)C.TollIncome); O->SetNumberField(TEXT("tax"), C.TaxRate);
		CorpA.Add(MakeShared<FJsonValueObject>(O));
	}
	for (const TPair<FName, FHDClaim>& P : Claims)
	{
		const FHDClaim& C = P.Value;
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), C.Id.ToString()); O->SetStringField(TEXT("corp"), C.Corp.ToString()); O->SetNumberField(TEXT("system"), C.System);
		O->SetNumberField(TEXT("band"), (int32)C.Band); O->SetStringField(TEXT("beacon"), C.Beacon.ToString());
		O->SetNumberField(TEXT("output30"), C.Output30); O->SetNumberField(TEXT("trade30"), C.Trade30);
		TSharedRef<FJsonObject> Res = MakeShared<FJsonObject>();
		for (const TPair<FString, double>& R : C.Residents) Res->SetNumberField(R.Key, R.Value);
		O->SetObjectField(TEXT("residents"), Res);
		TArray<TSharedPtr<FJsonValue>> Samples;
		for (float S : C.DbSamples) Samples.Add(MakeShared<FJsonValueNumber>(S));
		O->SetArrayField(TEXT("dbSamples"), Samples);
		O->SetNumberField(TEXT("lastSample"), C.LastSampleUnix);
		TArray<TSharedPtr<FJsonValue>> Alloc;
		for (int32 i = 0; i < 5; ++i) Alloc.Add(MakeShared<FJsonValueNumber>(C.Alloc[i]));
		O->SetArrayField(TEXT("alloc"), Alloc);
		TArray<TSharedPtr<FJsonValue>> Hires;
		for (const FHDHire& H : C.Hires)
		{
			TSharedRef<FJsonObject> HO = MakeShared<FJsonObject>();
			HO->SetStringField(TEXT("pilot"), H.Pilot); HO->SetNumberField(TEXT("rate"), (double)H.RatePerDay); HO->SetNumberField(TEXT("until"), H.UntilUnix);
			Hires.Add(MakeShared<FJsonValueObject>(HO));
		}
		O->SetArrayField(TEXT("hires"), Hires);
		O->SetArrayField(TEXT("kos"), TerrNamesJson(C.Kos));
		O->SetNumberField(TEXT("vulnHour"), C.VulnHourUtc); O->SetNumberField(TEXT("hireMin"), C.HireMinStanding); O->SetNumberField(TEXT("siteToll"), C.SiteTollPct);
		O->SetStringField(TEXT("wingFaction"), C.WingFaction.ToString());
		ClaimA.Add(MakeShared<FJsonValueObject>(O));
	}
	for (const TPair<FName, FHDStructure>& P : Structures)
	{
		const FHDStructure& S = P.Value;
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), S.Id.ToString()); O->SetNumberField(TEXT("type"), (int32)S.Type); O->SetNumberField(TEXT("anchor"), (int32)S.Anchor);
		O->SetStringField(TEXT("corp"), S.Corp.ToString()); O->SetNumberField(TEXT("system"), S.System); O->SetObjectField(TEXT("local"), TerrVecJson(S.Local));
		O->SetNumberField(TEXT("body"), S.Body); O->SetNumberField(TEXT("integrity"), S.Integrity); O->SetNumberField(TEXT("maxIntegrity"), S.MaxIntegrity);
		O->SetNumberField(TEXT("fuel"), S.Fuel); O->SetNumberField(TEXT("ammo"), S.Ammo); O->SetNumberField(TEXT("fuelEmptySince"), S.FuelEmptySinceUnix);
		O->SetBoolField(TEXT("derelict"), S.bDerelict); O->SetNumberField(TEXT("siege"), (int32)S.Siege); O->SetNumberField(TEXT("siegeUntil"), S.SiegeUntilUnix);
		O->SetNumberField(TEXT("family"), (int32)S.Family); O->SetArrayField(TEXT("sections"), TerrNamesJson(S.Sections)); O->SetStringField(TEXT("stationId"), S.StationId.ToString());
		O->SetNumberField(TEXT("access"), (int32)S.Access); O->SetNumberField(TEXT("toll"), S.Toll); O->SetNumberField(TEXT("standingMin"), S.StandingMin);
		StructA.Add(MakeShared<FJsonValueObject>(O));
	}
	for (const TPair<FName, FHDColony>& P : Colonies)
	{
		const FHDColony& C = P.Value;
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), C.Id.ToString()); O->SetStringField(TEXT("corp"), C.Corp.ToString()); O->SetStringField(TEXT("owner"), C.Owner);
		O->SetNumberField(TEXT("system"), C.System); O->SetNumberField(TEXT("body"), C.Body); O->SetStringField(TEXT("name"), C.Name);
		TSharedRef<FJsonObject> B = MakeShared<FJsonObject>();
		for (const TPair<FName, int32>& K : C.Buildings) B->SetNumberField(K.Key.ToString(), K.Value);
		O->SetObjectField(TEXT("buildings"), B);
		O->SetNumberField(TEXT("population"), C.Population); O->SetStringField(TEXT("governor"), C.Governor); O->SetNumberField(TEXT("govQuality"), C.GovernorQuality);
		O->SetNumberField(TEXT("govSalary"), (double)C.GovernorSalary); O->SetStringField(TEXT("focus"), C.Focus.ToString()); O->SetNumberField(TEXT("budget"), (double)C.WeeklyBudget);
		O->SetArrayField(TEXT("queue"), TerrNamesJson(C.Queue)); O->SetNumberField(TEXT("nextWeek"), C.NextWeekUnix);
		ColA.Add(MakeShared<FJsonValueObject>(O));
	}
	for (const FHDPlayerLane& L : Lanes)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("id"), L.Lane.Id.ToString()); O->SetStringField(TEXT("owner"), L.Lane.Owner.ToString()); O->SetNumberField(TEXT("system"), L.System);
		O->SetObjectField(TEXT("start"), TerrVecJson(L.Lane.StartLocal)); O->SetObjectField(TEXT("end"), TerrVecJson(L.Lane.EndLocal));
		O->SetNumberField(TEXT("toll"), L.Lane.Toll); O->SetBoolField(TEXT("suppressed"), L.bSuppressed);
		LaneA.Add(MakeShared<FJsonValueObject>(O));
	}
	for (const TPair<FName, FName>& P : CapturedStations)
	{
		TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("station"), P.Key.ToString()); O->SetStringField(TEXT("corp"), P.Value.ToString());
		CapA.Add(MakeShared<FJsonValueObject>(O));
	}
	Root->SetArrayField(TEXT("corps"), CorpA); Root->SetArrayField(TEXT("claims"), ClaimA); Root->SetArrayField(TEXT("structures"), StructA);
	Root->SetArrayField(TEXT("colonies"), ColA); Root->SetArrayField(TEXT("lanes"), LaneA); Root->SetArrayField(TEXT("captured"), CapA);
	Root->SetNumberField(TEXT("nextId"), NextId);
	FString Out;
	const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

void UHDTerritorySubsystem::ImportJson(const FString& Json)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
	auto Objects = [&Root](const TCHAR* Key) -> TArray<TSharedPtr<FJsonObject>>
	{
		TArray<TSharedPtr<FJsonObject>> L;
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (Root->TryGetArrayField(Key, A) && A) { for (const TSharedPtr<FJsonValue>& V : *A) { if (V.IsValid() && V->AsObject().IsValid()) L.Add(V->AsObject()); } }
		return L;
	};
	Corps.Reset(); Claims.Reset(); Structures.Reset(); Colonies.Reset(); Lanes.Reset(); CapturedStations.Reset();
	for (const TSharedPtr<FJsonObject>& O : Objects(TEXT("corps")))
	{
		FHDCorp C;
		C.Id = FName(*TerrStr(O, TEXT("id"))); C.Name = TerrStr(O, TEXT("name")); C.Ceo = TerrStr(O, TEXT("ceo")); C.Members = TerrJsonStrings(O, TEXT("members"));
		C.Treasury = (int64)TerrNum(O, TEXT("treasury")); C.TollIncome = (int64)TerrNum(O, TEXT("tollIncome")); C.TaxRate = (float)TerrNum(O, TEXT("tax"), 0.05);
		Corps.Add(C.Id, C);
	}
	for (const TSharedPtr<FJsonObject>& O : Objects(TEXT("claims")))
	{
		FHDClaim C;
		C.Id = FName(*TerrStr(O, TEXT("id"))); C.Corp = FName(*TerrStr(O, TEXT("corp"))); C.System = (int32)TerrNum(O, TEXT("system"), -1);
		C.Band = (EHDSecurityBand)(uint8)TerrNum(O, TEXT("band"), 2); C.Beacon = FName(*TerrStr(O, TEXT("beacon")));
		C.Output30 = TerrNum(O, TEXT("output30")); C.Trade30 = TerrNum(O, TEXT("trade30"));
		const TSharedPtr<FJsonObject>* Res = nullptr;
		if (O->TryGetObjectField(TEXT("residents"), Res) && Res) { for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Res)->Values) C.Residents.Add(P.Key, P.Value->AsNumber()); }
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (O->TryGetArrayField(TEXT("dbSamples"), A) && A) { for (const TSharedPtr<FJsonValue>& V : *A) C.DbSamples.Add((float)V->AsNumber()); }
		C.LastSampleUnix = TerrNum(O, TEXT("lastSample"));
		if (O->TryGetArrayField(TEXT("alloc"), A) && A) { for (int32 i = 0; i < 5 && i < A->Num(); ++i) C.Alloc[i] = (float)(*A)[i]->AsNumber(); }
		if (O->TryGetArrayField(TEXT("hires"), A) && A)
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject> HO = V->AsObject();
				FHDHire H; H.Pilot = TerrStr(HO, TEXT("pilot")); H.RatePerDay = (int64)TerrNum(HO, TEXT("rate")); H.UntilUnix = TerrNum(HO, TEXT("until"));
				C.Hires.Add(H);
			}
		}
		C.Kos = TerrJsonNames(O, TEXT("kos"));
		C.VulnHourUtc = (int32)TerrNum(O, TEXT("vulnHour"), 20); C.HireMinStanding = (float)TerrNum(O, TEXT("hireMin"), -10.0); C.SiteTollPct = (int32)TerrNum(O, TEXT("siteToll"), 10);
		const FString Wing = TerrStr(O, TEXT("wingFaction"));
		C.WingFaction = Wing.IsEmpty() || Wing == TEXT("None") ? NAME_None : FName(*Wing);
		Claims.Add(C.Id, C);
	}
	for (const TSharedPtr<FJsonObject>& O : Objects(TEXT("structures")))
	{
		FHDStructure S;
		S.Id = FName(*TerrStr(O, TEXT("id"))); S.Type = (EHDStructureType)(uint8)TerrNum(O, TEXT("type")); S.Anchor = (EHDAnchor)(uint8)TerrNum(O, TEXT("anchor"));
		S.Corp = FName(*TerrStr(O, TEXT("corp"))); S.System = (int32)TerrNum(O, TEXT("system"), -1);
		const TSharedPtr<FJsonObject>* L = nullptr;
		if (O->TryGetObjectField(TEXT("local"), L) && L) S.Local = TerrJsonVec(*L);
		S.Body = (int32)TerrNum(O, TEXT("body"), -1); S.Integrity = (float)TerrNum(O, TEXT("integrity"), 1.0); S.MaxIntegrity = (float)TerrNum(O, TEXT("maxIntegrity"), 1.0);
		S.Fuel = (float)TerrNum(O, TEXT("fuel")); S.Ammo = (float)TerrNum(O, TEXT("ammo")); S.FuelEmptySinceUnix = TerrNum(O, TEXT("fuelEmptySince"));
		O->TryGetBoolField(TEXT("derelict"), S.bDerelict);
		S.Siege = (EHDSiegeState)(uint8)TerrNum(O, TEXT("siege")); S.SiegeUntilUnix = TerrNum(O, TEXT("siegeUntil"));
		S.Family = (EHDStationFamily)(uint8)TerrNum(O, TEXT("family"), 2); S.Sections = TerrJsonNames(O, TEXT("sections"));
		const FString StId = TerrStr(O, TEXT("stationId"));
		S.StationId = StId.IsEmpty() || StId == TEXT("None") ? NAME_None : FName(*StId);
		S.Access = (EHDAccess)(uint8)TerrNum(O, TEXT("access")); S.Toll = (int32)TerrNum(O, TEXT("toll")); S.StandingMin = (float)TerrNum(O, TEXT("standingMin"));
		Structures.Add(S.Id, S);
	}
	for (const TSharedPtr<FJsonObject>& O : Objects(TEXT("colonies")))
	{
		FHDColony C;
		C.Id = FName(*TerrStr(O, TEXT("id"))); C.Corp = FName(*TerrStr(O, TEXT("corp"))); C.Owner = TerrStr(O, TEXT("owner"));
		C.System = (int32)TerrNum(O, TEXT("system"), -1); C.Body = (int32)TerrNum(O, TEXT("body"), -1); C.Name = TerrStr(O, TEXT("name"));
		const TSharedPtr<FJsonObject>* B = nullptr;
		if (O->TryGetObjectField(TEXT("buildings"), B) && B) { for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*B)->Values) C.Buildings.Add(FName(*P.Key), (int32)P.Value->AsNumber()); }
		C.Population = (float)TerrNum(O, TEXT("population"), 100.0); C.Governor = TerrStr(O, TEXT("governor")); C.GovernorQuality = (float)TerrNum(O, TEXT("govQuality"));
		C.GovernorSalary = (int64)TerrNum(O, TEXT("govSalary")); C.Focus = FName(*TerrStr(O, TEXT("focus"))); C.WeeklyBudget = (int64)TerrNum(O, TEXT("budget"));
		C.Queue = TerrJsonNames(O, TEXT("queue")); C.NextWeekUnix = TerrNum(O, TEXT("nextWeek"));
		Colonies.Add(C.Id, C);
	}
	for (const TSharedPtr<FJsonObject>& O : Objects(TEXT("lanes")))
	{
		FHDPlayerLane L;
		L.Lane.Id = FName(*TerrStr(O, TEXT("id"))); L.Lane.Owner = FName(*TerrStr(O, TEXT("owner"))); L.Lane.bNPC = false; L.System = (int32)TerrNum(O, TEXT("system"), -1);
		const TSharedPtr<FJsonObject>* V = nullptr;
		if (O->TryGetObjectField(TEXT("start"), V) && V) L.Lane.StartLocal = TerrJsonVec(*V);
		if (O->TryGetObjectField(TEXT("end"), V) && V) L.Lane.EndLocal = TerrJsonVec(*V);
		L.Lane.Toll = (int32)TerrNum(O, TEXT("toll")); O->TryGetBoolField(TEXT("suppressed"), L.bSuppressed);
		Lanes.Add(L);
	}
	for (const TSharedPtr<FJsonObject>& O : Objects(TEXT("captured"))) CapturedStations.Add(FName(*TerrStr(O, TEXT("station"))), FName(*TerrStr(O, TEXT("corp"))));
	NextId = FMath::Max(NextId, (int32)TerrNum(Root, TEXT("nextId"), 1.0));
}
