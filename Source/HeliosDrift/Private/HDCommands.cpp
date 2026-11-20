// Server-side handling of AHDPlayerController::ServerCommand. Every input action and console command lands here.
#include "HDPlayerController.h"
#include "HDShipPawn.h"
#include "HDPlayerState.h"
#include "HDSystemDirector.h"
#include "HDUniverseSubsystem.h"
#include "HDWorldActors.h"
#include "HDGameMode.h"
#include "HeliosDrift.h"
#include "Engine/World.h"
#include "EngineUtils.h"

namespace
{
	FString FmtDist(double Cm)
	{
		const double Km = Cm / 1.0e5;
		if (Km < 1.0) return FString::Printf(TEXT("%.0f m"), Cm / 100.0);
		if (Km < 1.0e5) return FString::Printf(TEXT("%.0f km"), Km);
		return FString::Printf(TEXT("%.2f AU"), Cm / 1.0e10);
	}
}

void AHDPlayerController::HandleCommand(const FString& Verb, const FString& Arg, AActor* Target)
{
	UWorld* World = GetWorld();
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	AHDSystemDirector* Director = AHDSystemDirector::Get(this);
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	AHDPlayerState* PS = GetPlayerState<AHDPlayerState>();
	if (!World || !U) return;

	// ---------------- Universe (milestone 1) ----------------
	if (Verb == TEXT("goto"))
	{
		const int32 Sys = U->FindSystem(Arg);
		if (Sys < 0) { Notify(FString::Printf(TEXT("No system '%s'"), *Arg)); return; }
		if (!Ship || !Director) return;
		if (Director->PlaceShip(Ship, Sys, Director->GetArrivalLocal(Sys), FRotator::ZeroRotator))
		{
			const FHDStarSystem& S = U->GetSystemSkeleton(Sys);
			Notify(FString::Printf(TEXT("Arrived in %s (%s, %s)"), *S.Name, *S.Id.ToString(), HDUniverse::BandName(S.Band)));
		}
		else
		{
			Notify(TEXT("Server is at its active-system limit; try again shortly."));
		}
		return;
	}
	if (Verb == TEXT("systeminfo"))
	{
		if (!Ship || !U->IsValidSystem(Ship->SystemIndex)) return;
		const FHDStarSystem& S = U->GetSystem(Ship->SystemIndex);
		Notify(FString::Printf(TEXT("%s %s | %s | region %s | %s | %d bodies, %d stations, %d sites, %d lanes"),
			*S.Id.ToString(), *S.Name, HDUniverse::BandName(S.Band), U->GetRegions().IsValidIndex(S.Region) ? *U->GetRegions()[S.Region].Name : TEXT("?"),
			S.bSettled ? TEXT("settled") : TEXT("unsettled"), S.Bodies.Num(), S.Stations.Num(), S.Sites.Num(), S.Lanes.Num()));
		const FVector Here = Ship->GetLocalPosition();
		for (const FHDBody& B : S.Bodies)
		{
			ClientNotify(FString::Printf(TEXT("  body %s: %s r=%.0f km g=%.2f %s %s"), *B.Name, HDUniverse::BodyTypeName(B.Type), B.RadiusKm, B.Gravity,
				B.bLandable ? TEXT("landable") : TEXT(""), *FmtDist(FVector::Dist(Here, B.LocalPos))));
		}
		for (const FHDStation& St : S.Stations) { ClientNotify(FString::Printf(TEXT("  station %s (%s) %s"), *St.Name, HDUniverse::StationFamilyName(St.Family), *FmtDist(FVector::Dist(Here, St.LocalPos)))); }
		for (const FHDJumpLink& L : U->GetLinks(Ship->SystemIndex))
		{
			ClientNotify(FString::Printf(TEXT("  %s -> %s%s %s"), HDUniverse::LinkTypeName(L.Type), U->IsValidSystem(L.To) ? *U->GetSystemSkeleton(L.To).Name : TEXT("?"),
				L.bHidden ? TEXT(" (hidden)") : TEXT(""), *FmtDist(FVector::Dist(Here, L.LocalPos))));
		}
		for (const FHDSite& Site : S.Sites) { ClientNotify(FString::Printf(TEXT("  site %s class %d%s %s"), HDUniverse::SiteTypeName(Site.Type), Site.HulkClass, Site.bHidden ? TEXT(" (hidden)") : TEXT(""), *FmtDist(FVector::Dist(Here, Site.LocalPos)))); }
		return;
	}
	if (Verb == TEXT("galaxy"))
	{
		if (!Ship) return;
		const float R = Arg.IsEmpty() ? 15.f : FMath::Clamp(FCString::Atof(*Arg), 1.f, 100.f);
		const TArray<FHDMapEntry> Near = U->GetSystemsNear(Ship->SystemIndex, R, 24);
		Notify(FString::Printf(TEXT("%d systems within %.0f ly"), Near.Num(), R));
		for (const FHDMapEntry& E : Near) { ClientNotify(FString::Printf(TEXT("  NP-%04d %s  %.1f ly  %s%s"), E.System, *E.Name, E.DistanceLy, HDUniverse::BandShortName(E.Band), E.bSettled ? TEXT("") : TEXT(" (unsettled)"))); }
		return;
	}
	if (Verb == TEXT("find"))
	{
		const int32 Sys = U->FindSystem(Arg);
		if (Sys < 0) { Notify(TEXT("Not found")); return; }
		const FHDStarSystem& S = U->GetSystemSkeleton(Sys);
		const float D = Ship ? U->DistanceLy(Ship->SystemIndex, Sys) : 0.f;
		const TArray<int32> Route = Ship ? U->FindGateRoute(Ship->SystemIndex, Sys) : TArray<int32>();
		Notify(FString::Printf(TEXT("%s %s: %s, %.1f ly away, %s"), *S.Id.ToString(), *S.Name, HDUniverse::BandName(S.Band), D,
			Route.Num() > 0 ? *FString::Printf(TEXT("%d gate jumps"), Route.Num() - 1) : TEXT("no gate route (wormholes or capital jump)")));
		return;
	}
	if (Verb == TEXT("week"))
	{
		AHDGameState* GS = World->GetGameState<AHDGameState>();
		const int64 Week = Arg.Equals(TEXT("live"), ESearchCase::IgnoreCase) || Arg.IsEmpty() ? MIN_int64 : (int64)FCString::Atoi64(*Arg);
		U->SetWeekOverride(Week);
		if (GS) GS->WeekOverride = Week;
		if (Director) { Director->LastWeek = MIN_int64; } // forces a rebuild on the next director tick
		Notify(FString::Printf(TEXT("Week index now %lld"), U->GetWeekIndex()));
		return;
	}
	if (Verb == TEXT("revealall"))
	{
		if (!PS || !Ship) return;
		int32 N = 0;
		for (TActorIterator<AHDSpaceObject> It(World); It; ++It)
		{
			if (It->SystemIndex == Ship->SystemIndex && It->bRequiresScan) { PS->Reveal(It->ObjectId); ++N; }
		}
		Notify(FString::Printf(TEXT("Revealed %d signatures"), N));
		return;
	}
	if (Verb == TEXT("gas"))
	{
		if (!Ship || !U->IsValidSystem(Ship->SystemIndex)) return;
		const FHDStarSystem& S = U->GetSystem(Ship->SystemIndex);
		const int64 W = U->GetWeekIndex();
		int32 N = 0;
		for (int32 b = 0; b < S.Bodies.Num(); ++b)
		{
			if (S.Bodies[b].Type != EHDBodyType::GasGiant) continue;
			++N;
			FString Next;
			for (int64 k = 1; k <= 4; ++k) { Next += FString::Printf(TEXT(" %s"), HDUniverse::DensityName(U->GetGasDensityForWeek(Ship->SystemIndex, b, W + k))); }
			ClientNotify(FString::Printf(TEXT("%s: %s this week, next:%s"), *S.Bodies[b].Name, HDUniverse::DensityName(U->GetGasDensity(Ship->SystemIndex, b)), *Next));
		}
		if (N == 0) Notify(TEXT("No gas giants in this system"));
		return;
	}

	if (HandleTravelCommand(Verb, Arg, Target)) return;
	if (HandleGameplayCommand(Verb, Arg, Target)) return;
	Notify(FString::Printf(TEXT("Unknown command '%s'"), *Verb));
}
