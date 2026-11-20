// Milestone 5: NPCs, antagonists, encounter anomalies, Accord security, PVP legality (doc 15, 18, 20).
#include "HDPlayerController.h"
#include "HDShipPawn.h"
#include "HDCargoComponent.h"
#include "HDPlayerState.h"
#include "HDWorldActors.h"
#include "HDWreckActor.h"
#include "HDNpcController.h"
#include "HDNpcSubsystem.h"
#include "HDSystemDirector.h"
#include "EngineUtils.h"

bool AHDPlayerController::HandleNpcCommand(const FString& Verb, const FString& Arg, AActor* Target)
{
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	AHDPlayerState* PS = GetPlayerState<AHDPlayerState>();
	UHDNpcSubsystem* Npc = UHDNpcSubsystem::Get(this);
	if (!Npc || !PS) return false;
	TArray<FString> A;
	Arg.ParseIntoArrayWS(A);

	if (Verb == TEXT("npcs"))
	{
		// npcs: contacts within 500 km.
		if (!Ship) return true;
		int32 N = 0;
		for (TActorIterator<AHDNpcController> It(GetWorld()); It; ++It)
		{
			const AHDShipPawn* S = It->GetShip();
			if (!S || S->SystemIndex != Ship->SystemIndex || (It->Pop == EHDNpcPop::Shadow && !It->bRevealed)) continue;
			const double D = FVector::Dist(S->GetActorLocation(), Ship->GetActorLocation());
			if (D > 5.0e7) continue;
			static const TCHAR* States[] = { TEXT("idle"), TEXT("patrol"), TEXT("travel"), TEXT("engaging"), TEXT("fleeing"), TEXT("guarding") };
			Notify(FString::Printf(TEXT("%s [%s] %.0f km, %s%s%s"), *S->DisplayLabel, *It->Faction.ToString(), D / 100000.0, States[(int32)It->State],
				It->IsHostileTo(Ship) ? TEXT(", HOSTILE") : TEXT(""), It->Target.Get() == Ship ? TEXT(", targeting you") : TEXT("")));
			if (++N >= 25) break;
		}
		if (N == 0) Notify(TEXT("No NPC contacts within 500 km."));
		return true;
	}
	if (Verb == TEXT("security"))
	{
		if (!Ship) return true;
		static const TCHAR* Bands[] = { TEXT("Secured: no PVP (duel flag only), Accord enforcement"), TEXT("Restricted: PVP only under declared war"), TEXT("Borderland: PVP only inside claims"), TEXT("Frontier: open") };
		float Pressure = 0.f;
		if (AHDSystemDirector* D = AHDSystemDirector::Get(this))
		{
			if (const FHDActiveSystem* Act = D->FindActive(Ship->SystemIndex)) Pressure = Npc->GetSpawnPressure(*Act);
		}
		const FString Status = PS->IsCriminal() ? FString::Printf(TEXT("CRIMINAL for %.0f s"), PS->CriminalUntil - AHDPlayerState::NowUnix()) : FString(TEXT("lawful"));
		Notify(FString::Printf(TEXT("%s | raider pressure %.2f | %s%s"), Bands[(int32)Npc->BandOf(Ship->SystemIndex)], Pressure, *Status, PS->bDuelFlag ? TEXT(", duel flag ON") : TEXT("")));
		return true;
	}
	if (Verb == TEXT("duel"))
	{
		// duel on|off: opt-in flag; two flagged pilots may fight anywhere, Secured included.
		PS->bDuelFlag = A.Num() > 0 ? A[0] == TEXT("on") : !PS->bDuelFlag;
		Notify(PS->bDuelFlag ? TEXT("Duel flag ON: other flagged pilots can engage you.") : TEXT("Duel flag off."));
		return true;
	}
	if (Verb == TEXT("report"))
	{
		// report: hand a contraband drift to the local authority. The Secured-legal version; pays less than keeping it.
		if (!Ship) return true;
		int32 Crates = 0;
		for (TActorIterator<AHDWreckActor> It(GetWorld()); It; ++It)
		{
			AHDWreckActor* W = *It;
			if (!W->bCrate || W->SystemIndex != Ship->SystemIndex || FVector::Dist(W->GetActorLocation(), Ship->GetActorLocation()) > 1.0e6) continue;
			bool bContra = false;
			for (const FHDItemStack& S : W->Loot) { if (S.Flags & (HDItemFlags::Contraband | HDItemFlags::Stolen)) bContra = true; }
			if (!bContra) continue;
			W->Loot.Reset();
			W->SetLifeSpan(2.f);
			++Crates;
		}
		if (Crates == 0) { Notify(TEXT("Nothing to report within 10 km.")); return true; }
		const int32 Band = (int32)Npc->BandOf(Ship->SystemIndex);
		const int64 Pay = 800 * Crates * (1 + Band);
		PS->AddCredits(Pay);
		PS->AddStanding(TEXT("accord"), 0.15f * Crates);
		Notify(FString::Printf(TEXT("Reported %d container(s) to the Accord: +%lld cr, standing up."), Crates, Pay));
		return true;
	}
	if (Verb == TEXT("shadow"))
	{
		// shadow [deliver]: the contract a shadow faction offered you.
		FHDShadowContract* K = Npc->ShadowContracts.Find(PS->PilotId);
		if (!K) { Notify(TEXT("No shadow contract. They find you, not the other way round.")); return true; }
		if (AHDPlayerState::NowUnix() > K->ExpiresUnix) { Npc->ShadowContracts.Remove(PS->PilotId); Notify(TEXT("The contract lapsed.")); return true; }
		if (A.Num() == 0 || A[0] != TEXT("deliver")) { Notify(FString::Printf(TEXT("Survey %s (system %d), then: shadow deliver"), *K->SiteName, K->System)); return true; }
		AHDSiteActor* Site = nullptr;
		for (TActorIterator<AHDSiteActor> It(GetWorld()); It; ++It) { if (It->ObjectId == K->SiteId) { Site = *It; break; } }
		if (!Ship || !Site || Ship->SystemIndex != K->System || FVector::Dist(Site->GetActorLocation(), Ship->GetActorLocation()) > 5.0e6) { Notify(TEXT("Get within 50 km of the site first.")); return true; }
		// Paid in standing and information: every hidden signature in this system resolves for you.
		PS->AddStanding(TEXT("shadow"), 0.5f);
		int32 Revealed = 0;
		for (TActorIterator<AHDSpaceObject> It(GetWorld()); It; ++It)
		{
			if (It->SystemIndex == Ship->SystemIndex && It->bRequiresScan && !PS->HasRevealed(It->ObjectId)) { PS->Reveal(It->ObjectId); ++Revealed; }
		}
		Npc->ShadowContracts.Remove(PS->PilotId);
		Notify(FString::Printf(TEXT("Survey delivered. Shadow standing up; %d hidden signature(s) marked on your map."), Revealed));
		return true;
	}

#if !UE_BUILD_SHIPPING
	if (Verb == TEXT("spawnnpc"))
	{
		// spawnnpc <accord|patrol|trader|rogue|pirate|sentinel|shadow> [hull] [count]
		if (!Ship) return true;
		const FString P = A.Num() > 0 ? A[0].ToLower() : TEXT("rogue");
		EHDNpcPop Pop = EHDNpcPop::Rogue;
		if (P == TEXT("accord")) Pop = EHDNpcPop::Accord;
		else if (P == TEXT("patrol")) Pop = EHDNpcPop::Patrol;
		else if (P == TEXT("trader")) Pop = EHDNpcPop::Trader;
		else if (P == TEXT("pirate")) Pop = EHDNpcPop::Pirate;
		else if (P == TEXT("sentinel")) Pop = EHDNpcPop::Sentinel;
		else if (P == TEXT("shadow")) Pop = EHDNpcPop::Shadow;
		FRandomStream R(FMath::Rand());
		const int32 Count = A.IsValidIndex(2) ? FMath::Clamp(FCString::Atoi(*A[2]), 1, 10) : 1;
		AHDNpcController* Lead = nullptr;
		for (int32 i = 0; i < Count; ++i)
		{
			const FName Hull = A.IsValidIndex(1) && A[1] != TEXT("-") ? FName(*A[1]) : UHDNpcSubsystem::PickHull(Pop, Npc->BandOf(Ship->SystemIndex), R, i == 0);
			const FVector Loc = Ship->GetActorLocation() + Ship->GetActorForwardVector() * 1.5e6f + R.GetUnitVector() * 200000.f;
			AHDNpcController* C = Npc->SpawnNpc(Ship->SystemIndex, Loc, Pop, Hull, Pop == EHDNpcPop::Accord ? FName(TEXT("accord")) : FName(*P));
			if (!C) continue;
			if (Pop == EHDNpcPop::Pirate) { if (!Lead) Lead = C; else { C->Leader = Lead; C->bHadLeader = true; } }
			Notify(FString::Printf(TEXT("Spawned %s 15 km ahead"), *C->GetShip()->DisplayLabel));
		}
		return true;
	}
	if (Verb == TEXT("crime"))
	{
		// crime [seconds]: flag yourself criminal to test the Accord response.
		if (Ship) Npc->ReportCrime(Ship, TEXT("test crime"), A.Num() > 0 ? FCString::Atof(*A[0]) : 300.f);
		return true;
	}
	if (Verb == TEXT("clearnpcs"))
	{
		int32 N = 0;
		for (TActorIterator<AHDNpcController> It(GetWorld()); It; ++It)
		{
			if (AHDShipPawn* S = It->GetShip()) S->Destroy();
			It->Destroy();
			++N;
		}
		Notify(FString::Printf(TEXT("Removed %d NPCs"), N));
		return true;
	}
	if (Verb == TEXT("pardon"))
	{
		PS->CriminalUntil = 0.0;
		Notify(TEXT("Criminal flag cleared."));
		return true;
	}
#endif
	return false;
}

void AHDPlayerController::OnWreckLooted(AHDWreckActor* Wreck)
{
	// Taking another pilot's cargo is theft in lawful space (doc 15/18); open space allows it.
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	UHDNpcSubsystem* Npc = UHDNpcSubsystem::Get(this);
	if (!Ship || !Npc || !Wreck) return;
	if (Npc->BandOf(Ship->SystemIndex) <= EHDSecurityBand::Restricted) Npc->ReportCrime(Ship, TEXT("looting another pilot's wreck"), 900.f);
}
