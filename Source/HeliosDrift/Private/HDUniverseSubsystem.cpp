#include "HDUniverseSubsystem.h"
#include "HeliosDrift.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Misc/DateTime.h"
#include "HAL/PlatformTime.h"

// ---------------------------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------------------------
namespace HDUniverse
{
	const TCHAR* BandName(EHDSecurityBand B)
	{
		switch (B)
		{
		case EHDSecurityBand::Secured: return TEXT("Secured");
		case EHDSecurityBand::Restricted: return TEXT("Restricted");
		case EHDSecurityBand::Borderland: return TEXT("Borderland");
		default: return TEXT("Frontier (The Black)");
		}
	}
	const TCHAR* BandShortName(EHDSecurityBand B)
	{
		switch (B)
		{
		case EHDSecurityBand::Secured: return TEXT("Sec");
		case EHDSecurityBand::Restricted: return TEXT("Rest");
		case EHDSecurityBand::Borderland: return TEXT("Bor");
		default: return TEXT("Black");
		}
	}
	const TCHAR* BodyTypeName(EHDBodyType T)
	{
		switch (T)
		{
		case EHDBodyType::Star: return TEXT("Star");
		case EHDBodyType::Rocky: return TEXT("Rocky world");
		case EHDBodyType::Desert: return TEXT("Desert world");
		case EHDBodyType::Ice: return TEXT("Ice world");
		case EHDBodyType::Water: return TEXT("Water world");
		case EHDBodyType::Lava: return TEXT("Lava world");
		case EHDBodyType::Terran: return TEXT("Terran world");
		case EHDBodyType::GasGiant: return TEXT("Gas giant");
		case EHDBodyType::Moon: return TEXT("Moon");
		default: return TEXT("Large asteroid");
		}
	}
	const TCHAR* LinkTypeName(EHDLinkType T)
	{
		switch (T)
		{
		case EHDLinkType::Gate: return TEXT("Jump gate");
		case EHDLinkType::LargeGate: return TEXT("Large gate");
		case EHDLinkType::Wormhole: return TEXT("Wormhole");
		case EHDLinkType::StellarAnomaly: return TEXT("Stellar anomaly");
		case EHDLinkType::GalacticRuin: return TEXT("Galactic gate (ruin)");
		default: return TEXT("Instance jump lane");
		}
	}
	const TCHAR* SiteTypeName(EHDSiteType T)
	{
		switch (T)
		{
		case EHDSiteType::SignalFire: return TEXT("Signal fire");
		case EHDSiteType::ContrabandDrift: return TEXT("Contraband drift");
		case EHDSiteType::RogueNest: return TEXT("Rogue nest");
		case EHDSiteType::DeadSignature: return TEXT("Dead signature");
		case EHDSiteType::Hulk: return TEXT("Hulk");
		case EHDSiteType::DeepCore: return TEXT("Deep-core anomaly");
		case EHDSiteType::PirateBase: return TEXT("Pirate base");
		case EHDSiteType::SentinelVault: return TEXT("Sentinel vault");
		default: return TEXT("Derelict station");
		}
	}
	const TCHAR* DensityName(EHDGasDensity D)
	{
		switch (D)
		{
		case EHDGasDensity::Rare: return TEXT("Rare");
		case EHDGasDensity::Consistent: return TEXT("Consistent");
		default: return TEXT("Abundant");
		}
	}
	const TCHAR* StationFamilyName(EHDStationFamily F)
	{
		switch (F)
		{
		case EHDStationFamily::AccordCivic: return TEXT("Accord civic");
		case EHDStationFamily::FactionIndustrial: return TEXT("Faction industrial");
		case EHDStationFamily::CorporateModular: return TEXT("Corporate modular");
		default: return TEXT("Pirate kitbash");
		}
	}
	FLinearColor BandColor(EHDSecurityBand B)
	{
		switch (B)
		{
		case EHDSecurityBand::Secured: return FLinearColor(0.3f, 0.9f, 1.f);
		case EHDSecurityBand::Restricted: return FLinearColor(0.4f, 1.f, 0.4f);
		case EHDSecurityBand::Borderland: return FLinearColor(1.f, 0.7f, 0.2f);
		default: return FLinearColor(1.f, 0.25f, 0.25f);
		}
	}
	uint32 Mix(uint32 A, uint32 B)
	{
		uint64 X = (static_cast<uint64>(A) << 32) ^ static_cast<uint64>(B);
		X += 0x9E3779B97F4A7C15ull;
		X = (X ^ (X >> 30)) * 0xBF58476D1CE4E5B9ull;
		X = (X ^ (X >> 27)) * 0x94D049BB133111EBull;
		X ^= (X >> 31);
		return static_cast<uint32>(X ^ (X >> 32));
	}
	float Unit(uint32 A, uint32 B)
	{
		return static_cast<float>(Mix(A, B) & 0xFFFFFF) / 16777216.f;
	}
}

namespace
{
	const TCHAR* GIndustrialFactions[] = { TEXT("Halvar Combine"), TEXT("Tessaly Freehold"), TEXT("Meridian Compact"), TEXT("Orrin Union") };
	const TCHAR* GPirateFactions[] = { TEXT("Black Tally"), TEXT("Rustwake Brethren") };
	const TCHAR* GSyllA[] = { TEXT("Ka"), TEXT("Ve"), TEXT("Tor"), TEXT("Ash"), TEXT("Mi"), TEXT("Sel"), TEXT("Dra"), TEXT("Or"), TEXT("Lu"), TEXT("Bren"), TEXT("Ith"), TEXT("Cal"), TEXT("Nor"), TEXT("Pe"), TEXT("Zan"), TEXT("Har"), TEXT("Qui"), TEXT("Ro"), TEXT("Sa"), TEXT("Wen") };
	const TCHAR* GSyllB[] = { TEXT("ra"), TEXT("len"), TEXT("vi"), TEXT("dor"), TEXT("mar"), TEXT("tis"), TEXT("ko"), TEXT("ne"), TEXT("ru"), TEXT("sha"), TEXT("bel"), TEXT("ga"), TEXT("lo"), TEXT("thi"), TEXT("ven"), TEXT("za") };
	const TCHAR* GSyllC[] = { TEXT(""), TEXT(""), TEXT("n"), TEXT("s"), TEXT("th"), TEXT("ra"), TEXT("ion"), TEXT("a"), TEXT("ex"), TEXT("ar"), TEXT("is"), TEXT("um") };
	const TCHAR* GRoman[] = { TEXT("I"), TEXT("II"), TEXT("III"), TEXT("IV"), TEXT("V"), TEXT("VI"), TEXT("VII"), TEXT("VIII"), TEXT("IX"), TEXT("X"), TEXT("XI"), TEXT("XII") };

	FIntVector GridKey(const FVector& P, float Cell)
	{
		return FIntVector(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell), FMath::FloorToInt(P.Z / Cell));
	}

	struct FUnionFind
	{
		TArray<int32> P;
		void Init(int32 N) { P.SetNum(N); for (int32 i = 0; i < N; ++i) P[i] = i; }
		int32 Find(int32 X) { while (P[X] != X) { P[X] = P[P[X]]; X = P[X]; } return X; }
		bool Union(int32 A, int32 B) { A = Find(A); B = Find(B); if (A == B) return false; P[A] = B; return true; }
	};

	uint64 EdgeKey(int32 A, int32 B) { const int32 L = FMath::Min(A, B), H = FMath::Max(A, B); return (static_cast<uint64>(L) << 32) | static_cast<uint32>(H); }

	float StarRadiusKm(EHDStarClass C)
	{
		switch (C)
		{
		case EHDStarClass::BlueGiant: return 4.5e6f;
		case EHDStarClass::BlueWhite: return 1.8e6f;
		case EHDStarClass::White: return 1.2e6f;
		case EHDStarClass::YellowWhite: return 9.0e5f;
		case EHDStarClass::Yellow: return 7.0e5f;
		case EHDStarClass::Orange: return 5.5e5f;
		case EHDStarClass::RedDwarf: return 3.0e5f;
		case EHDStarClass::WhiteDwarf: return 1.2e5f;
		case EHDStarClass::Neutron: return 8.0e4f;
		default: return 1.0e5f;
		}
	}

	FVector RandomPlanarDir(FRandomStream& R)
	{
		const float A = R.FRandRange(0.f, 2.f * PI);
		return FVector(FMath::Cos(A), FMath::Sin(A), 0.f);
	}
}

// ---------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------
void UHDUniverseSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	int32 CfgSeed = 1337, CfgCount = 5000;
	float CfgRadius = 200.f, CfgAU = 1.0e10f, CfgBody = 0.25f, CfgStar = 0.02f;
	if (GConfig)
	{
		const TCHAR* Sec = TEXT("HeliosDrift.Universe");
		GConfig->GetInt(Sec, TEXT("Seed"), CfgSeed, GGameIni);
		GConfig->GetInt(Sec, TEXT("SystemCount"), CfgCount, GGameIni);
		GConfig->GetFloat(Sec, TEXT("GalaxyRadiusLy"), CfgRadius, GGameIni);
		GConfig->GetFloat(Sec, TEXT("CmPerAU"), CfgAU, GGameIni);
		GConfig->GetFloat(Sec, TEXT("BodyScale"), CfgBody, GGameIni);
		GConfig->GetFloat(Sec, TEXT("StarScale"), CfgStar, GGameIni);
		GConfig->GetFloat(Sec, TEXT("OddsRare"), OddsRare, GGameIni);
		GConfig->GetFloat(Sec, TEXT("OddsConsistent"), OddsConsistent, GGameIni);
	}
	FParse::Value(FCommandLine::Get(), TEXT("UniverseSeed="), CfgSeed);
	FParse::Value(FCommandLine::Get(), TEXT("SystemCount="), CfgCount);
	GalaxyRadiusLy = FMath::Max(20.f, CfgRadius);
	AUcm = FMath::Clamp((double)CfgAU, 1.0e8, 2.0e10);
	BodyScale = FMath::Clamp((double)CfgBody, 0.001, 1.0);
	StarScale = FMath::Clamp((double)CfgStar, 0.0005, 0.2);
	Generate(CfgSeed, CfgCount);
}

UHDUniverseSubsystem* UHDUniverseSubsystem::Get(const UObject* WorldContext)
{
	if (!WorldContext || !GEngine) return nullptr;
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UHDUniverseSubsystem>() : nullptr;
}

void UHDUniverseSubsystem::Generate(int32 InSeed, int32 InSystemCount)
{
	const double T0 = FPlatformTime::Seconds();
	Seed = InSeed;
	SystemCount = FMath::Clamp(InSystemCount, 60, 20000);
	// Keep density roughly constant if the count changes (about 4 ly between neighbours).
	const float AutoRadius = 200.f * FMath::Sqrt(SystemCount / 5000.f);
	GalaxyRadiusLy = FMath::Max(GalaxyRadiusLy == 200.f ? AutoRadius : GalaxyRadiusLy, 20.f);
	Systems.Reset();
	Regions.Reset();
	WeeklyWormholes.Reset();
	WormholeWeek = MIN_int64;
	Grid.Reset();
	GenerateSkeleton();
	UE_LOG(LogHelios, Log, TEXT("Universe generated: seed %d, %d systems, %d regions, start %s, %.0f ms"),
		Seed, Systems.Num(), Regions.Num(), Systems.IsValidIndex(StartSystem) ? *Systems[StartSystem].Name : TEXT("?"), (FPlatformTime::Seconds() - T0) * 1000.0);
}

FString UHDUniverseSubsystem::MakeName(FRandomStream& R) const
{
	FString N = GSyllA[R.RandRange(0, (int32)UE_ARRAY_COUNT(GSyllA) - 1)];
	N += GSyllB[R.RandRange(0, (int32)UE_ARRAY_COUNT(GSyllB) - 1)];
	if (R.FRand() < 0.35f) { N += GSyllB[R.RandRange(0, (int32)UE_ARRAY_COUNT(GSyllB) - 1)]; }
	N += GSyllC[R.RandRange(0, (int32)UE_ARRAY_COUNT(GSyllC) - 1)];
	return N;
}

// ---------------------------------------------------------------------------------------------
// Spatial grid
// ---------------------------------------------------------------------------------------------
void UHDUniverseSubsystem::BuildGrid()
{
	Grid.Reset();
	for (const FHDStarSystem& S : Systems) { Grid.FindOrAdd(GridKey(S.PositionLy, GridCell)).Add(S.Index); }
}

void UHDUniverseSubsystem::QueryGrid(const FVector& P, float Radius, TArray<int32>& Out) const
{
	Out.Reset();
	FIntVector Lo = GridKey(P - FVector(Radius), GridCell);
	FIntVector Hi = GridKey(P + FVector(Radius), GridCell);
	// The disk is thin (|z| < ~8 ly): never scan empty layers above and below it.
	Lo.Z = FMath::Max(Lo.Z, -2);
	Hi.Z = FMath::Min(Hi.Z, 1);
	const float R2 = Radius * Radius;
	for (int32 X = Lo.X; X <= Hi.X; ++X)
	for (int32 Y = Lo.Y; Y <= Hi.Y; ++Y)
	for (int32 Z = Lo.Z; Z <= Hi.Z; ++Z)
	{
		if (const TArray<int32>* Cell = Grid.Find(FIntVector(X, Y, Z)))
		{
			for (int32 I : *Cell)
			{
				if (Systems.IsValidIndex(I) && FVector::DistSquared(Systems[I].PositionLy, P) <= R2) { Out.Add(I); }
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Skeleton: positions, regions, bands, gates, anomalies
// ---------------------------------------------------------------------------------------------
void UHDUniverseSubsystem::AddLink(int32 A, int32 B, EHDLinkType Type, const FName& Id, bool bHidden, float Signature, bool bStable, bool bWeekly)
{
	FHDJumpLink L;
	L.Id = Id; L.Type = Type; L.bHidden = bHidden; L.Signature = Signature; L.bStable = bStable;
	L.From = A; L.To = B;
	FHDJumpLink R = L;
	R.From = B; R.To = A;
	if (bWeekly)
	{
		WeeklyWormholes.FindOrAdd(A).Add(L);
		WeeklyWormholes.FindOrAdd(B).Add(R);
	}
	else
	{
		Systems[A].Links.Add(L);
		Systems[B].Links.Add(R);
	}
}

void UHDUniverseSubsystem::GenerateSkeleton()
{
	FRandomStream R(Seed);
	Systems.SetNum(SystemCount);
	const int32 Arms = 4;
	const float GR = GalaxyRadiusLy;
	const float MinSep = 1.0f;

	// 1) Positions: four arms plus a disk fill, thinner toward the rim.
	for (int32 i = 0; i < SystemCount; ++i)
	{
		FVector P = FVector::ZeroVector;
		for (int32 Attempt = 0; Attempt < 10; ++Attempt)
		{
			const float U = R.FRand();
			const float Rad = GR * FMath::Pow(U, 0.62f) + 2.f;
			float Ang;
			if (R.FRand() < 0.7f)
			{
				const int32 Arm = R.RandRange(0, Arms - 1);
				const float Noise = (R.FRand() + R.FRand() + R.FRand() - 1.5f) * 0.55f;
				Ang = Arm * (2.f * PI / Arms) + (Rad / GR) * 2.2f * PI + Noise;
			}
			else
			{
				Ang = R.FRandRange(0.f, 2.f * PI);
			}
			const float Z = (R.FRand() + R.FRand() + R.FRand() - 1.5f) * 5.f * (1.f - 0.7f * FMath::Min(Rad / GR, 1.f));
			P = FVector(Rad * FMath::Cos(Ang), Rad * FMath::Sin(Ang), Z);
			bool bClear = true;
			const FIntVector K = GridKey(P, GridCell);
			for (int32 X = -1; X <= 1 && bClear; ++X)
			for (int32 Y = -1; Y <= 1 && bClear; ++Y)
			for (int32 Zc = -1; Zc <= 1 && bClear; ++Zc)
			{
				if (const TArray<int32>* Cell = Grid.Find(K + FIntVector(X, Y, Zc)))
				{
					for (int32 J : *Cell) { if (FVector::DistSquared(Systems[J].PositionLy, P) < MinSep * MinSep) { bClear = false; break; } }
				}
			}
			if (bClear) break;
		}
		FHDStarSystem& S = Systems[i];
		S.Index = i;
		S.Id = FName(*FString::Printf(TEXT("NP-%04d"), i));
		S.PositionLy = P;
		S.Name = MakeName(R);
		Grid.FindOrAdd(GridKey(P, GridCell)).Add(i);
	}

	// 2) Regions (Voronoi around seed systems), bands by distance from the core with some jitter.
	const int32 NumRegions = FMath::Clamp(SystemCount / 100, 12, 64);
	Regions.SetNum(NumRegions);
	for (int32 r = 0; r < NumRegions; ++r)
	{
		FHDRegion& G = Regions[r];
		G.Index = r;
		G.CenterLy = Systems[R.RandRange(0, SystemCount - 1)].PositionLy;
		G.Name = MakeName(R) + TEXT(" Reach");
	}
	// The region nearest the core is always Secured.
	int32 CoreRegion = 0;
	for (int32 r = 1; r < NumRegions; ++r) { if (Regions[r].CenterLy.Size() < Regions[CoreRegion].CenterLy.Size()) CoreRegion = r; }
	for (int32 r = 0; r < NumRegions; ++r)
	{
		FHDRegion& G = Regions[r];
		float Rn = G.CenterLy.Size2D() / GR + R.FRandRange(-0.06f, 0.06f);
		if (r == CoreRegion) Rn = 0.f;
		G.Band = Rn < 0.24f ? EHDSecurityBand::Secured : Rn < 0.47f ? EHDSecurityBand::Restricted : Rn < 0.72f ? EHDSecurityBand::Borderland : EHDSecurityBand::Frontier;
		const float Ang = FMath::Atan2(G.CenterLy.Y, G.CenterLy.X) + PI;
		const int32 Quad = FMath::Clamp(FMath::FloorToInt(Ang / (PI * 0.5f)), 0, 3);
		G.Faction = (G.Band == EHDSecurityBand::Frontier) ? FName(GPirateFactions[r % 2]) : FName(GIndustrialFactions[Quad]);
	}
	for (FHDStarSystem& S : Systems)
	{
		int32 Best = 0; double BestD = TNumericLimits<double>::Max();
		for (int32 r = 0; r < NumRegions; ++r)
		{
			const double D = FVector::DistSquared(S.PositionLy, Regions[r].CenterLy);
			if (D < BestD) { BestD = D; Best = r; }
		}
		S.Region = Best;
		S.Band = Regions[Best].Band;
		S.Faction = Regions[Best].Faction;
		Regions[Best].Systems.Add(S.Index);
		const float SettleOdds = S.Band == EHDSecurityBand::Secured ? 1.f : S.Band == EHDSecurityBand::Restricted ? 0.85f : S.Band == EHDSecurityBand::Borderland ? 0.55f : 0.15f;
		S.bSettled = R.FRand() < SettleOdds;
		const float StarRoll = R.FRand();
		S.Star = StarRoll < 0.02f ? EHDStarClass::BlueGiant : StarRoll < 0.06f ? EHDStarClass::BlueWhite : StarRoll < 0.12f ? EHDStarClass::White
			: StarRoll < 0.22f ? EHDStarClass::YellowWhite : StarRoll < 0.42f ? EHDStarClass::Yellow : StarRoll < 0.62f ? EHDStarClass::Orange
			: StarRoll < 0.92f ? EHDStarClass::RedDwarf : StarRoll < 0.97f ? EHDStarClass::WhiteDwarf : StarRoll < 0.99f ? EHDStarClass::Neutron : EHDStarClass::BlackHole;
	}

	// Start system: settled Secured system nearest the core.
	StartSystem = 0;
	double BestStart = TNumericLimits<double>::Max();
	for (const FHDStarSystem& S : Systems)
	{
		if (S.Band != EHDSecurityBand::Secured || !S.bSettled) continue;
		const double D = S.PositionLy.SizeSquared();
		if (D < BestStart) { BestStart = D; StartSystem = S.Index; }
	}

	// 3) Gate network over every non-Frontier system: k-nearest candidates, Kruskal MST, then extra loops.
	//    The Frontier (The Black) has no gates: small ships reach it only through wormholes (doc 13).
	TArray<int32> Gated;
	for (const FHDStarSystem& S : Systems) { if (S.Band != EHDSecurityBand::Frontier) Gated.Add(S.Index); }
	struct FEdge { int32 A; int32 B; float D; };
	TArray<FEdge> Cand;
	TArray<int32> Near;
	for (int32 A : Gated)
	{
		float Radius = 7.f;
		Near.Reset();
		for (int32 Pass = 0; Pass < 4; ++Pass)
		{
			QueryGrid(Systems[A].PositionLy, Radius, Near);
			int32 Count = 0;
			for (int32 B : Near) { if (B != A && Systems[B].Band != EHDSecurityBand::Frontier) ++Count; }
			if (Count >= 4) break;
			Radius *= 1.8f;
		}
		TArray<TPair<float, int32>> Sorted;
		for (int32 B : Near)
		{
			if (B == A || Systems[B].Band == EHDSecurityBand::Frontier) continue;
			Sorted.Add(TPair<float, int32>((float)FVector::Dist(Systems[A].PositionLy, Systems[B].PositionLy), B));
		}
		Sorted.Sort([](const TPair<float, int32>& X, const TPair<float, int32>& Y) { return X.Key < Y.Key; });
		for (int32 k = 0; k < FMath::Min(6, Sorted.Num()); ++k) { Cand.Add(FEdge{ A, Sorted[k].Value, Sorted[k].Key }); }
	}
	Cand.Sort([](const FEdge& X, const FEdge& Y) { return X.D < Y.D; });
	FUnionFind UF; UF.Init(SystemCount);
	TSet<uint64> EdgeSet;
	TArray<FEdge> GateEdges;
	TArray<int32> Degree; Degree.Init(0, SystemCount);
	for (const FEdge& E : Cand)
	{
		if (UF.Union(E.A, E.B) && !EdgeSet.Contains(EdgeKey(E.A, E.B)))
		{
			EdgeSet.Add(EdgeKey(E.A, E.B)); GateEdges.Add(E); ++Degree[E.A]; ++Degree[E.B];
		}
	}
	// Loops: short extra edges so the network is not a pure tree.
	for (const FEdge& E : Cand)
	{
		if (EdgeSet.Contains(EdgeKey(E.A, E.B))) continue;
		if (E.D < 6.f && Degree[E.A] < 4 && Degree[E.B] < 4 && R.FRand() < 0.35f)
		{
			EdgeSet.Add(EdgeKey(E.A, E.B)); GateEdges.Add(E); ++Degree[E.A]; ++Degree[E.B];
		}
	}
	// Join any remaining islands to the start system's network.
	for (int32 Guard = 0; Guard < 64; ++Guard)
	{
		const int32 MainRoot = UF.Find(StartSystem);
		TMap<int32, TArray<int32>> Comps;
		for (int32 A : Gated) { const int32 Root = UF.Find(A); if (Root != MainRoot) Comps.FindOrAdd(Root).Add(A); }
		if (Comps.Num() == 0) break;
		for (const TPair<int32, TArray<int32>>& C : Comps)
		{
			if (UF.Find(C.Value[0]) == UF.Find(StartSystem)) continue;
			FEdge Best{ -1, -1, TNumericLimits<float>::Max() };
			for (int32 A : C.Value)
			{
				for (float Radius = 10.f; Radius <= 160.f && Best.A < 0; Radius *= 2.f)
				{
					QueryGrid(Systems[A].PositionLy, Radius, Near);
					for (int32 B : Near)
					{
						if (Systems[B].Band == EHDSecurityBand::Frontier || UF.Find(B) == UF.Find(A)) continue;
						const float D = (float)FVector::Dist(Systems[A].PositionLy, Systems[B].PositionLy);
						if (D < Best.D) Best = FEdge{ A, B, D };
					}
				}
			}
			if (Best.A >= 0)
			{
				UF.Union(Best.A, Best.B);
				EdgeSet.Add(EdgeKey(Best.A, Best.B)); GateEdges.Add(Best); ++Degree[Best.A]; ++Degree[Best.B];
			}
		}
	}
	for (const FEdge& E : GateEdges)
	{
		// Large gates (doc 13): rare, on hub-to-hub routes. The only standard-network path for capitals.
		const bool bLarge = Degree[E.A] >= 3 && Degree[E.B] >= 3 && R.FRand() < 0.07f;
		const FName Id(*FString::Printf(TEXT("%s-%d-%d"), bLarge ? TEXT("LG") : TEXT("G"), FMath::Min(E.A, E.B), FMath::Max(E.A, E.B)));
		AddLink(E.A, E.B, bLarge ? EHDLinkType::LargeGate : EHDLinkType::Gate, Id, false, 1.f, true, false);
	}

	// 4) Stellar anomalies: odd distortions that throw a ship a long way (sometimes at random). Any band.
	const int32 NumAnoms = FMath::Max(4, SystemCount / 80);
	for (int32 a = 0; a < NumAnoms; ++a)
	{
		const int32 A = R.RandRange(0, SystemCount - 1);
		int32 B = -1;
		for (int32 Try = 0; Try < 40; ++Try)
		{
			const int32 C = R.RandRange(0, SystemCount - 1);
			const float D = DistanceLy(A, C);
			if (C != A && D > 15.f && D < 45.f) { B = C; break; }
		}
		if (B >= 0) { AddLink(A, B, EHDLinkType::StellarAnomaly, FName(*FString::Printf(TEXT("SA-%d"), a)), true, R.FRandRange(0.4f, 0.8f), true, false); }
	}

	// 5) Stable wormholes: every Frontier region keeps two charted mouths from the nearest Borderland space.
	for (const FHDRegion& G : Regions)
	{
		if (G.Band != EHDSecurityBand::Frontier || G.Systems.Num() == 0) continue;
		TArray<TPair<double, int32>> Border;
		for (const FHDStarSystem& S : Systems)
		{
			if (S.Band == EHDSecurityBand::Borderland) Border.Add(TPair<double, int32>(FVector::DistSquared(S.PositionLy, G.CenterLy), S.Index));
		}
		Border.Sort([](const TPair<double, int32>& X, const TPair<double, int32>& Y) { return X.Key < Y.Key; });
		for (int32 k = 0; k < FMath::Min(2, Border.Num()); ++k)
		{
			const int32 B = G.Systems[R.RandRange(0, G.Systems.Num() - 1)];
			AddLink(Border[k].Value, B, EHDLinkType::Wormhole, FName(*FString::Printf(TEXT("SW-%d-%d"), G.Index, k)), true, 0.45f, true, false);
		}
	}

	// Local positions for every skeleton link: gates face their destination.
	for (FHDStarSystem& S : Systems)
	{
		for (FHDJumpLink& L : S.Links)
		{
			const uint32 H = GetTypeHash(L.Id);
			FVector Dir;
			if (Systems.IsValidIndex(L.To) && (L.Type == EHDLinkType::Gate || L.Type == EHDLinkType::LargeGate))
			{
				Dir = (Systems[L.To].PositionLy - S.PositionLy);
				Dir.Z = 0.f;
				Dir = Dir.GetSafeNormal();
				if (Dir.IsNearlyZero()) Dir = FVector::ForwardVector;
			}
			else
			{
				const float A = HDUniverse::Unit(H, (uint32)S.Index) * 2.f * PI;
				Dir = FVector(FMath::Cos(A), FMath::Sin(A), 0.f);
			}
			const float OrbitAU = (L.Type == EHDLinkType::Gate || L.Type == EHDLinkType::LargeGate) ? 10.f + 8.f * HDUniverse::Unit(H, 7u + S.Index) : 6.f + 20.f * HDUniverse::Unit(H, 11u + S.Index);
			L.LocalPos = Dir * OrbitAU * AUcm + FVector(0.f, 0.f, (HDUniverse::Unit(H, 3u) - 0.5f) * 0.1f * AUcm);
		}
	}
}

void UHDUniverseSubsystem::RefreshWormholes()
{
	const int64 Week = GetWeekIndex();
	if (Week == WormholeWeek) return;
	WormholeWeek = Week;
	WeeklyWormholes.Reset();
	FRandomStream R((int32)HDUniverse::Mix((uint32)Seed ^ 0x5EEDu, (uint32)Week));
	TArray<int32> Frontier;
	for (const FHDStarSystem& S : Systems) { if (S.Band == EHDSecurityBand::Frontier) Frontier.Add(S.Index); }
	if (Frontier.Num() == 0) return;
	const int32 Count = FMath::Max(8, SystemCount / 25);
	TArray<int32> Near;
	for (int32 w = 0; w < Count; ++w)
	{
		const int32 A = Frontier[R.RandRange(0, Frontier.Num() - 1)];
		const bool bToSettled = R.FRand() < 0.4f;
		QueryGrid(Systems[A].PositionLy, bToSettled ? 45.f : 25.f, Near);
		TArray<int32> Ok;
		for (int32 B : Near)
		{
			if (B == A) continue;
			const bool bFrontier = Systems[B].Band == EHDSecurityBand::Frontier;
			if (bToSettled ? (Systems[B].Band == EHDSecurityBand::Borderland || Systems[B].Band == EHDSecurityBand::Restricted) : bFrontier) Ok.Add(B);
		}
		if (Ok.Num() == 0) continue;
		const int32 B = Ok[R.RandRange(0, Ok.Num() - 1)];
		AddLink(A, B, EHDLinkType::Wormhole, FName(*FString::Printf(TEXT("WH-%lld-%d"), Week, w)), true, R.FRandRange(0.15f, 0.6f), false, true);
	}
	for (TPair<int32, TArray<FHDJumpLink>>& P : WeeklyWormholes)
	{
		for (FHDJumpLink& L : P.Value)
		{
			const uint32 H = GetTypeHash(L.Id);
			const float A = HDUniverse::Unit(H, (uint32)P.Key) * 2.f * PI;
			L.LocalPos = FVector(FMath::Cos(A), FMath::Sin(A), 0.f) * (4.f + 24.f * HDUniverse::Unit(H, 99u + P.Key)) * AUcm;
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Detail: bodies, fields, stations, sites, lanes
// ---------------------------------------------------------------------------------------------
void UHDUniverseSubsystem::GenerateDetail(FHDStarSystem& S)
{
	if (S.bDetailed) return;
	S.bDetailed = true;
	FRandomStream R((int32)HDUniverse::Mix((uint32)Seed, (uint32)S.Index + 1u));
	const double KmToCm = 1.0e5;
	const float RichByBand[] = { 0.8f, 1.0f, 1.3f, 1.7f };
	const float Rich = RichByBand[(int32)S.Band];

	// Star
	FHDBody Star;
	Star.Id = FName(*FString::Printf(TEXT("%s-star"), *S.Id.ToString()));
	Star.Name = S.Name;
	Star.Type = EHDBodyType::Star;
	Star.RadiusKm = StarRadiusKm(S.Star);
	Star.VisualRadiusCm = Star.RadiusKm * KmToCm * StarScale;
	S.Bodies.Add(Star);

	const float MinOrbit = FMath::Max(0.35f, (float)(Star.VisualRadiusCm * 4.0 / AUcm));
	const int32 NumPlanets = R.RandRange(3, 9) + (S.Band == EHDSecurityBand::Frontier ? 1 : 0);
	int32 MainWorld = -1;
	for (int32 p = 0; p < NumPlanets; ++p)
	{
		const float Orbit = MinOrbit * FMath::Pow(1.7f, (float)p) * R.FRandRange(0.9f, 1.1f);
		if (Orbit > 40.f) break;
		FHDBody B;
		const float Roll = R.FRand();
		if (Orbit < 0.7f) B.Type = Roll < 0.4f ? EHDBodyType::Lava : Roll < 0.8f ? EHDBodyType::Rocky : EHDBodyType::Desert;
		else if (Orbit < 1.8f) B.Type = Roll < 0.3f ? EHDBodyType::Terran : Roll < 0.5f ? EHDBodyType::Water : Roll < 0.7f ? EHDBodyType::Desert : EHDBodyType::Rocky;
		else if (Orbit < 3.5f) B.Type = Roll < 0.4f ? EHDBodyType::Rocky : Roll < 0.75f ? EHDBodyType::GasGiant : Roll < 0.85f ? EHDBodyType::Desert : EHDBodyType::Ice;
		else B.Type = Roll < 0.55f ? EHDBodyType::GasGiant : EHDBodyType::Ice;
		B.Name = FString::Printf(TEXT("%s %s"), *S.Name, GRoman[FMath::Min(p, 11)]);
		B.Id = FName(*FString::Printf(TEXT("%s-p%d"), *S.Id.ToString(), p));
		B.OrbitAU = Orbit;
		switch (B.Type)
		{
		case EHDBodyType::GasGiant: B.RadiusKm = R.FRandRange(20000.f, 70000.f); break;
		case EHDBodyType::Terran: case EHDBodyType::Water: B.RadiusKm = R.FRandRange(4000.f, 9000.f); break;
		case EHDBodyType::Ice: B.RadiusKm = R.FRandRange(1500.f, 6000.f); break;
		default: B.RadiusKm = R.FRandRange(1500.f, 7000.f); break;
		}
		if (B.Type == EHDBodyType::GasGiant)
		{
			B.Gravity = B.RadiusKm / 7000.f * R.FRandRange(0.3f, 0.6f);
			B.bLandable = false; // gas giants are orbital-only (doc 17)
			B.bAtmosphere = true;
			B.Volatiles = 1.f;
			const float W = R.FRand();
			B.Weather = W < 0.4f ? EHDWeather::Calm : W < 0.8f ? EHDWeather::Stormy : EHDWeather::Violent;
		}
		else
		{
			B.Gravity = (B.RadiusKm / 6371.f) * R.FRandRange(0.6f, 1.25f);
			B.bLandable = B.Gravity <= 2.0f; // generated bodies cap at 2.0 g (doc 17)
			B.bAtmosphere = B.Type == EHDBodyType::Terran || B.Type == EHDBodyType::Water
				|| (B.Type == EHDBodyType::Desert && R.FRand() < 0.5f) || (B.Type == EHDBodyType::Lava && R.FRand() < 0.4f) || (B.Type == EHDBodyType::Ice && R.FRand() < 0.2f);
			switch (B.Type)
			{
			case EHDBodyType::Rocky: B.Minerals = R.FRandRange(0.7f, 1.f); B.Water = R.FRandRange(0.f, 0.1f); break;
			case EHDBodyType::Lava: B.Minerals = R.FRandRange(0.6f, 1.f); B.Volatiles = 0.3f; break;
			case EHDBodyType::Desert: B.Minerals = R.FRandRange(0.4f, 0.8f); B.Water = R.FRandRange(0.f, 0.15f); break;
			case EHDBodyType::Terran: B.Minerals = R.FRandRange(0.3f, 0.7f); B.Water = R.FRandRange(0.5f, 0.8f); B.Fertility = R.FRandRange(0.6f, 1.f); break;
			case EHDBodyType::Water: B.Minerals = R.FRandRange(0.f, 0.15f); B.Water = R.FRandRange(0.9f, 1.f); B.Fertility = R.FRandRange(0.5f, 0.8f); break;
			case EHDBodyType::Ice: B.Minerals = R.FRandRange(0.2f, 0.5f); B.Water = R.FRandRange(0.6f, 0.9f); B.Volatiles = 0.5f; break;
			default: break;
			}
		}
		B.VisualRadiusCm = B.RadiusKm * KmToCm * BodyScale;
		B.LocalPos = RandomPlanarDir(R) * (double)Orbit * AUcm + FVector(0.f, 0.f, R.FRandRange(-0.02f, 0.02f) * Orbit * AUcm);
		const int32 PlanetIndex = S.Bodies.Add(B);
		if (MainWorld < 0 && B.bLandable && (B.Type == EHDBodyType::Terran || B.Type == EHDBodyType::Water || B.Type == EHDBodyType::Rocky || B.Type == EHDBodyType::Desert)) MainWorld = PlanetIndex;

		// Moons: gas giants can't be settled, but their moons can (doc 19).
		const int32 NumMoons = B.Type == EHDBodyType::GasGiant ? R.RandRange(1, 5) : (B.RadiusKm > 3000.f ? R.RandRange(0, 2) : 0);
		for (int32 m = 0; m < NumMoons; ++m)
		{
			FHDBody M;
			M.Type = EHDBodyType::Moon;
			M.Parent = PlanetIndex;
			M.Name = FString::Printf(TEXT("%s %c"), *B.Name, TCHAR('a' + m));
			M.Id = FName(*FString::Printf(TEXT("%s-p%dm%d"), *S.Id.ToString(), p, m));
			M.RadiusKm = R.FRandRange(300.f, B.Type == EHDBodyType::GasGiant ? 3000.f : 1800.f);
			M.Gravity = (M.RadiusKm / 6371.f) * R.FRandRange(0.6f, 1.f);
			M.bLandable = true;
			M.bAtmosphere = R.FRand() < 0.1f;
			M.Minerals = R.FRandRange(0.4f, 0.9f);
			M.Water = R.FRandRange(0.1f, 0.5f);
			M.VisualRadiusCm = M.RadiusKm * KmToCm * BodyScale;
			M.OrbitAU = B.OrbitAU;
			const double Dist = B.VisualRadiusCm * R.FRandRange(3.f, 7.f) + M.VisualRadiusCm * 3.0 + 2.0e7 * m;
			M.LocalPos = B.LocalPos + RandomPlanarDir(R) * Dist;
			S.Bodies.Add(M);
		}
	}
	// Development (landing corridors, doc 17): settled main world is Administered, Secured core worlds Hardened.
	if (S.bSettled && S.Bodies.IsValidIndex(MainWorld))
	{
		S.Bodies[MainWorld].Dev = S.Band == EHDSecurityBand::Secured ? EHDDevState::Hardened : EHDDevState::Administered;
	}
	if (S.bSettled)
	{
		for (int32 b = 1; b < S.Bodies.Num(); ++b)
		{
			if (b != MainWorld && S.Bodies[b].bLandable && S.Bodies[b].Type != EHDBodyType::Moon && R.FRand() < 0.2f) S.Bodies[b].Dev = EHDDevState::Administered;
		}
	}

	// Deposit fields (doc 16 §7A)
	static const TCHAR* BeltOres[] = { TEXT("ore_ferrite"), TEXT("ore_silicate"), TEXT("ore_crystal") };
	const int32 NumBelts = R.RandRange(0, 2) + (S.Band == EHDSecurityBand::Frontier ? 1 : 0) + (S.Index == StartSystem ? 1 : 0);
	for (int32 f = 0; f < NumBelts; ++f)
	{
		FHDField F;
		F.Type = EHDFieldType::AsteroidBelt;
		F.Id = FName(*FString::Printf(TEXT("%s-belt%d"), *S.Id.ToString(), f));
		F.PrimaryItem = FName(BeltOres[R.RandRange(0, 2)]);
		F.SecondaryItem = S.Band >= EHDSecurityBand::Borderland ? FName(TEXT("ore_exotic")) : FName(BeltOres[R.RandRange(0, 2)]);
		F.Richness = Rich * R.FRandRange(0.8f, 1.2f);
		F.LocalPos = RandomPlanarDir(R) * (double)R.FRandRange(1.2f, 6.f) * AUcm;
		S.Fields.Add(F);
		// Large asteroids can be landed on and built on (doc 16/17/19).
		if (R.FRand() < 0.5f)
		{
			FHDBody A;
			A.Type = EHDBodyType::LargeAsteroid;
			A.Name = FString::Printf(TEXT("%s Rock %d"), *S.Name, f + 1);
			A.Id = FName(*FString::Printf(TEXT("%s-rock%d"), *S.Id.ToString(), f));
			A.RadiusKm = R.FRandRange(50.f, 400.f);
			A.Gravity = 0.02f;
			A.bLandable = true;
			A.Minerals = 1.f;
			A.VisualRadiusCm = A.RadiusKm * KmToCm * FMath::Max(BodyScale, 0.5);
			A.LocalPos = F.LocalPos + RandomPlanarDir(R) * 4.0e8;
			S.Bodies.Add(A);
		}
	}
	if (R.FRand() < 0.6f)
	{
		FHDField F;
		F.Type = EHDFieldType::IceField;
		F.Id = FName(*FString::Printf(TEXT("%s-ice"), *S.Id.ToString()));
		F.PrimaryItem = TEXT("ice_water");
		F.SecondaryItem = TEXT("ice_volatile");
		F.Richness = Rich;
		F.LocalPos = RandomPlanarDir(R) * (double)R.FRandRange(5.f, 15.f) * AUcm;
		S.Fields.Add(F);
	}
	for (int32 b = 1; b < S.Bodies.Num(); ++b)
	{
		if (S.Bodies[b].Type != EHDBodyType::GasGiant || R.FRand() > 0.5f) continue;
		FHDField F;
		F.Type = EHDFieldType::GasCloud;
		F.Id = FName(*FString::Printf(TEXT("%s-gas%d"), *S.Id.ToString(), b));
		F.PrimaryItem = TEXT("gas_nebular");
		F.SecondaryItem = TEXT("gas_volatile");
		F.Richness = Rich;
		F.NearBody = b;
		F.LocalPos = S.Bodies[b].LocalPos + RandomPlanarDir(R) * S.Bodies[b].VisualRadiusCm * 4.0;
		S.Fields.Add(F);
	}

	// Stations (doc 19 §7D.0): space stations in every settled system.
	auto AddStation = [&S, &R](int32 Body, EHDStationFamily Family, const FString& Suffix)
	{
		FHDStation St;
		St.Family = Family;
		St.Body = Body;
		St.Faction = Family == EHDStationFamily::AccordCivic ? FName(TEXT("Accord")) : Family == EHDStationFamily::PirateKitbash ? FName(GPirateFactions[S.Index % 2]) : S.Faction;
		St.Id = FName(*FString::Printf(TEXT("%s-st%d"), *S.Id.ToString(), S.Stations.Num()));
		const FHDBody& B = S.Bodies[FMath::Clamp(Body, 0, S.Bodies.Num() - 1)];
		St.Name = FString::Printf(TEXT("%s %s"), *B.Name, *Suffix);
		St.bMarket = true;
		St.bRepair = true;
		St.bShipyard = Family == EHDStationFamily::FactionIndustrial || Family == EHDStationFamily::CorporateModular;
		St.bRefinery = Family != EHDStationFamily::AccordCivic;
		const double Alt = B.Type == EHDBodyType::Star ? B.VisualRadiusCm * 3.0 : B.VisualRadiusCm * 1.15 + 3.0e7;
		St.LocalPos = B.LocalPos + RandomPlanarDir(R) * Alt + FVector(0.f, 0.f, 2.0e6);
		S.Stations.Add(St);
	};
	if (S.bSettled)
	{
		const int32 Home = S.Bodies.IsValidIndex(MainWorld) ? MainWorld : FMath::Min(1, S.Bodies.Num() - 1);
		const float Roll = R.FRand();
		EHDStationFamily Fam = EHDStationFamily::AccordCivic;
		if (S.Band == EHDSecurityBand::Restricted) Fam = Roll < 0.6f ? EHDStationFamily::FactionIndustrial : EHDStationFamily::AccordCivic;
		else if (S.Band == EHDSecurityBand::Borderland) Fam = Roll < 0.5f ? EHDStationFamily::CorporateModular : EHDStationFamily::FactionIndustrial;
		else if (S.Band == EHDSecurityBand::Frontier) Fam = Roll < 0.5f ? EHDStationFamily::PirateKitbash : EHDStationFamily::CorporateModular;
		AddStation(Home, Fam, Fam == EHDStationFamily::AccordCivic ? TEXT("Accord Hub") : TEXT("Station"));
		if (S.Band == EHDSecurityBand::Secured) { AddStation(Home, EHDStationFamily::FactionIndustrial, TEXT("Yards")); }
		const int32 Extra = R.RandRange(0, 2);
		for (int32 e = 0; e < Extra && S.Bodies.Num() > 2; ++e)
		{
			const int32 Body = R.RandRange(1, S.Bodies.Num() - 1);
			if (S.Bodies[Body].Type == EHDBodyType::GasGiant) continue;
			AddStation(Body, S.Band == EHDSecurityBand::Frontier ? EHDStationFamily::PirateKitbash : EHDStationFamily::CorporateModular, TEXT("Outpost"));
		}
	}
	// Gas-giant fuel stations, anchored in the upper atmosphere.
	for (int32 b = 1; b < S.Bodies.Num(); ++b)
	{
		if (S.Bodies[b].Type != EHDBodyType::GasGiant) continue;
		if (R.FRand() > (S.bSettled ? 0.45f : 0.15f)) continue;
		FHDStation St;
		St.Family = S.Band == EHDSecurityBand::Frontier ? EHDStationFamily::CorporateModular : EHDStationFamily::FactionIndustrial;
		St.Faction = S.Faction;
		St.Body = b;
		St.bFuelStation = true;
		St.bMarket = true;
		St.bRepair = false;
		St.Id = FName(*FString::Printf(TEXT("%s-st%d"), *S.Id.ToString(), S.Stations.Num()));
		St.Name = FString::Printf(TEXT("%s Fuel Platform"), *S.Bodies[b].Name);
		St.LocalPos = S.Bodies[b].LocalPos + RandomPlanarDir(R) * S.Bodies[b].VisualRadiusCm * 0.96;
		S.Stations.Add(St);
	}

	// Sites (doc 16, 18, 20)
	const int32 NumSites = R.RandRange(4, 8) + (int32)S.Band;
	int32 LaneTargets = 0;
	for (int32 s = 0; s < NumSites; ++s)
	{
		FHDSite Site;
		Site.Id = FName(*FString::Printf(TEXT("%s-site%d"), *S.Id.ToString(), s));
		// Weighted pick. Encounter anomalies spawn in every band (doc 20).
		struct FW { EHDSiteType T; float W; };
		TArray<FW> Table;
		Table.Add(FW{ EHDSiteType::SignalFire, 4.f });
		Table.Add(FW{ EHDSiteType::ContrabandDrift, 3.f });
		Table.Add(FW{ EHDSiteType::RogueNest, 3.f });
		Table.Add(FW{ EHDSiteType::DeadSignature, S.Band == EHDSecurityBand::Frontier ? 4.f : 2.f });
		Table.Add(FW{ EHDSiteType::Hulk, 4.f });
		Table.Add(FW{ EHDSiteType::DeepCore, S.Band == EHDSecurityBand::Secured ? 0.5f : 2.f });
		Table.Add(FW{ EHDSiteType::PirateBase, S.Band == EHDSecurityBand::Secured ? 0.f : S.Band == EHDSecurityBand::Restricted ? 0.7f : 2.5f });
		Table.Add(FW{ EHDSiteType::SentinelVault, S.Band == EHDSecurityBand::Borderland ? 0.6f : S.Band == EHDSecurityBand::Frontier ? 1.5f : 0.f });
		Table.Add(FW{ EHDSiteType::DerelictStation, S.Band == EHDSecurityBand::Secured ? 0.f : 0.8f });
		float Sum = 0.f; for (const FW& W : Table) Sum += W.W;
		float Pick = R.FRand() * Sum;
		for (const FW& W : Table) { Pick -= W.W; if (Pick <= 0.f) { Site.Type = W.T; break; } }

		if (Site.Type == EHDSiteType::Hulk)
		{
			// Class 3-4 do not spawn in Secured (doc 16 §7B, 2026-10-28 brief): Restricted and outward.
			const float C = R.FRand();
			Site.HulkClass = S.Band == EHDSecurityBand::Secured ? (C < 0.6f ? 1 : 2) : (C < 0.4f ? 1 : C < 0.7f ? 2 : C < 0.9f ? 3 : 4);
		}
		else if (Site.Type == EHDSiteType::DerelictStation || Site.Type == EHDSiteType::SentinelVault)
		{
			Site.HulkClass = 4; // station derelicts and sentinel vaults are Class 4
		}
		const float DiffByBand[] = { 1.f, 1.5f, 2.2f, 3.f };
		Site.Difficulty = DiffByBand[(int32)S.Band] * R.FRandRange(0.8f, 1.3f);
		Site.bHidden = Site.Type != EHDSiteType::SignalFire; // distress calls broadcast; everything else is scanned down
		Site.Signature = Site.Type == EHDSiteType::SignalFire ? 1.f : R.FRandRange(0.2f, 0.8f);
		Site.LocalPos = RandomPlanarDir(R) * (double)R.FRandRange(0.6f, 20.f) * AUcm + FVector(0.f, 0.f, R.FRandRange(-0.05f, 0.05f) * AUcm);
		if ((Site.Type == EHDSiteType::RogueNest || Site.Type == EHDSiteType::PirateBase) && LaneTargets < 2 && R.FRand() < 0.35f)
		{
			Site.bInstanced = true;
			Site.LocalPos = FVector(R.FRandRange(-1.f, 1.f) * 5.0e9, R.FRandRange(-1.f, 1.f) * 5.0e9, InstancePocketCm + 1.0e9 * LaneTargets);
			++LaneTargets;
		}
		const int32 SiteIndex = S.Sites.Add(Site);
		if (Site.bInstanced)
		{
			FHDJumpLink L;
			L.Id = FName(*FString::Printf(TEXT("IL-%s"), *Site.Id.ToString()));
			L.Type = EHDLinkType::InstanceLane;
			L.From = S.Index;
			L.To = S.Index;
			L.TargetSite = SiteIndex;
			L.bHidden = false;
			const FVector Anchor = S.Stations.Num() > 0 ? S.Stations[0].LocalPos : FVector(2.0 * AUcm, 0.0, 0.0);
			L.LocalPos = Anchor + RandomPlanarDir(R) * 2.0e7 * (1 + SiteIndex % 3);
			S.LocalLinks.Add(L);
		}
	}

	// Galactic gates (incomplete / ancient ruins): only at the outer rim of The Black.
	if (S.Band == EHDSecurityBand::Frontier && S.PositionLy.Size2D() > GalaxyRadiusLy * 0.93f && HDUniverse::Unit((uint32)Seed, (uint32)S.Index) < 0.08f)
	{
		FHDJumpLink L;
		L.Id = FName(*FString::Printf(TEXT("GG-%d"), S.Index));
		L.Type = EHDLinkType::GalacticRuin;
		L.From = S.Index;
		L.bHidden = true;
		L.Signature = 0.25f;
		L.LocalPos = RandomPlanarDir(R) * 38.0 * AUcm;
		S.LocalLinks.Add(L);
	}

	// Trade lanes (doc 13): NPC-owned, free, in main Secured space (and some Restricted systems).
	const bool bLanes = S.bSettled && (S.Band == EHDSecurityBand::Secured || (S.Band == EHDSecurityBand::Restricted && R.FRand() < 0.5f));
	if (bLanes)
	{
		TArray<FVector> Points;
		for (const FHDStation& St : S.Stations) { if (!St.bFuelStation) Points.Add(St.LocalPos); }
		for (const FHDJumpLink& L : S.Links) { if (L.Type == EHDLinkType::Gate || L.Type == EHDLinkType::LargeGate) Points.Add(L.LocalPos); }
		Points.Sort([](const FVector& A, const FVector& B) { return FMath::Atan2(A.Y, A.X) < FMath::Atan2(B.Y, B.X); });
		// Connect consecutive points around the system, hub first.
		for (int32 i = 0; i + 1 < Points.Num() && S.Lanes.Num() < 6; ++i)
		{
			const FVector A = Points[i], B = Points[i + 1];
			const double Len = FVector::Dist(A, B);
			if (Len < 1.0e9) continue; // too short to be worth a lane
			const FVector Dir = (B - A) / Len;
			FHDTradeLane Lane;
			Lane.Id = FName(*FString::Printf(TEXT("%s-lane%d"), *S.Id.ToString(), S.Lanes.Num()));
			Lane.StartLocal = A + Dir * 5.0e6;
			Lane.EndLocal = B - Dir * 5.0e6;
			Lane.Owner = TEXT("Accord");
			Lane.bNPC = true;
			Lane.Toll = 0;
			S.Lanes.Add(Lane);
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Queries
// ---------------------------------------------------------------------------------------------
const FHDStarSystem& UHDUniverseSubsystem::GetSystemSkeleton(int32 Index) const
{
	static const FHDStarSystem Empty;
	return Systems.IsValidIndex(Index) ? Systems[Index] : Empty;
}

const FHDStarSystem& UHDUniverseSubsystem::GetSystem(int32 Index)
{
	static const FHDStarSystem Empty;
	if (!Systems.IsValidIndex(Index)) return Empty;
	GenerateDetail(Systems[Index]);
	return Systems[Index];
}

int32 UHDUniverseSubsystem::FindSystem(const FString& IdOrName) const
{
	FString Q = IdOrName.TrimStartAndEnd();
	if (Q.IsEmpty()) return -1;
	FString Num = Q;
	if (Num.StartsWith(TEXT("NP-"), ESearchCase::IgnoreCase)) Num = Num.Mid(3);
	if (Num.IsNumeric())
	{
		const int32 I = FCString::Atoi(*Num);
		return Systems.IsValidIndex(I) ? I : -1;
	}
	for (const FHDStarSystem& S : Systems) { if (S.Name.Equals(Q, ESearchCase::IgnoreCase)) return S.Index; }
	return -1;
}

TArray<FHDJumpLink> UHDUniverseSubsystem::GetLinks(int32 Index)
{
	TArray<FHDJumpLink> Out;
	if (!Systems.IsValidIndex(Index)) return Out;
	RefreshWormholes();
	Out = Systems[Index].Links;
	if (const TArray<FHDJumpLink>* W = WeeklyWormholes.Find(Index)) { Out.Append(*W); }
	return Out;
}

float UHDUniverseSubsystem::DistanceLy(int32 A, int32 B) const
{
	if (!Systems.IsValidIndex(A) || !Systems.IsValidIndex(B)) return TNumericLimits<float>::Max();
	return (float)FVector::Dist(Systems[A].PositionLy, Systems[B].PositionLy);
}

TArray<FHDMapEntry> UHDUniverseSubsystem::GetSystemsNear(int32 Center, float RadiusLy, int32 MaxResults) const
{
	TArray<FHDMapEntry> Out;
	if (!Systems.IsValidIndex(Center)) return Out;
	TArray<int32> Near;
	QueryGrid(Systems[Center].PositionLy, FMath::Clamp(RadiusLy, 1.f, 200.f), Near);
	for (int32 I : Near)
	{
		FHDMapEntry E;
		E.System = I;
		E.Name = Systems[I].Name;
		E.Band = Systems[I].Band;
		E.PositionLy = Systems[I].PositionLy;
		E.DistanceLy = DistanceLy(Center, I);
		E.bSettled = Systems[I].bSettled;
		Out.Add(E);
	}
	Out.Sort([](const FHDMapEntry& A, const FHDMapEntry& B) { return A.DistanceLy < B.DistanceLy; });
	if (MaxResults > 0 && Out.Num() > MaxResults) Out.SetNum(MaxResults);
	return Out;
}

TArray<int32> UHDUniverseSubsystem::FindGateRoute(int32 From, int32 To, bool bUseWormholes)
{
	TArray<int32> Route;
	if (!Systems.IsValidIndex(From) || !Systems.IsValidIndex(To)) return Route;
	if (From == To) { Route.Add(From); return Route; }
	RefreshWormholes();
	TMap<int32, int32> Prev;
	TArray<int32> Queue;
	Queue.Add(From);
	Prev.Add(From, -1);
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const int32 Cur = Queue[Head];
		if (Cur == To) break;
		auto Visit = [&](const FHDJumpLink& L)
		{
			const bool bOk = L.Type == EHDLinkType::Gate || L.Type == EHDLinkType::LargeGate || (bUseWormholes && L.Type == EHDLinkType::Wormhole);
			if (bOk && Systems.IsValidIndex(L.To) && !Prev.Contains(L.To)) { Prev.Add(L.To, Cur); Queue.Add(L.To); }
		};
		for (const FHDJumpLink& L : Systems[Cur].Links) Visit(L);
		if (bUseWormholes) { if (const TArray<FHDJumpLink>* W = WeeklyWormholes.Find(Cur)) { for (const FHDJumpLink& L : *W) Visit(L); } }
	}
	if (!Prev.Contains(To)) return Route;
	for (int32 C = To; C != -1; C = Prev.FindRef(C)) { Route.Insert(C, 0); if (C == From) break; }
	return Route;
}

TArray<int32> UHDUniverseSubsystem::FindJumpRoute(int32 From, int32 To, float RangeLy, int32 MaxHops) const
{
	TArray<int32> Route;
	if (!Systems.IsValidIndex(From) || !Systems.IsValidIndex(To) || RangeLy <= 0.f) return Route;
	if (From == To) { Route.Add(From); return Route; }
	TMap<int32, int32> Prev;
	TMap<int32, int32> Hops;
	TArray<int32> Queue;
	Queue.Add(From);
	Prev.Add(From, -1);
	Hops.Add(From, 0);
	TArray<int32> Near;
	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const int32 Cur = Queue[Head];
		if (Cur == To) break;
		if (Hops.FindRef(Cur) >= MaxHops) continue;
		QueryGrid(Systems[Cur].PositionLy, RangeLy, Near);
		// Greedy ordering: neighbours closest to the target first keeps the BFS small.
		Near.Sort([this, To](int32 A, int32 B) { return FVector::DistSquared(Systems[A].PositionLy, Systems[To].PositionLy) < FVector::DistSquared(Systems[B].PositionLy, Systems[To].PositionLy); });
		for (int32 N : Near)
		{
			if (Prev.Contains(N)) continue;
			Prev.Add(N, Cur);
			Hops.Add(N, Hops.FindRef(Cur) + 1);
			Queue.Add(N);
		}
		if (Queue.Num() > 20000) break;
	}
	if (!Prev.Contains(To)) return Route;
	for (int32 C = To; C != -1; C = Prev.FindRef(C)) { Route.Insert(C, 0); if (C == From) break; }
	return Route;
}

int64 UHDUniverseSubsystem::GetWeekIndex() const
{
	if (WeekOverride != MIN_int64) return WeekOverride;
	const int64 Now = FDateTime::UtcNow().ToUnixTimestamp();
	return (Now - WeekOffsetSeconds) / (7 * 86400);
}

EHDGasDensity UHDUniverseSubsystem::GetGasDensityForWeek(int32 SystemIndex, int32 BodyIndex, int64 Week) const
{
	const float U = HDUniverse::Unit(HDUniverse::Mix((uint32)Seed ^ 0xA5A5u, (uint32)(SystemIndex * 64 + BodyIndex)), (uint32)Week);
	if (U < OddsRare) return EHDGasDensity::Rare;
	if (U < OddsRare + OddsConsistent) return EHDGasDensity::Consistent;
	return EHDGasDensity::Abundant;
}

EHDGasDensity UHDUniverseSubsystem::GetGasDensity(int32 SystemIndex, int32 BodyIndex) const
{
	return GetGasDensityForWeek(SystemIndex, BodyIndex, GetWeekIndex());
}

float UHDUniverseSubsystem::DensityYieldMultiplier(EHDGasDensity D)
{
	switch (D)
	{
	case EHDGasDensity::Rare: return 0.4f;
	case EHDGasDensity::Consistent: return 1.f;
	default: return 1.8f;
	}
}
