// Milestone 6: companies, claims and the Defense Budget, the defense ladder, structures and sieges, player stations,
// fuel platforms, player lanes and interdiction, pirate base capture, colonies and governors (doc 06, 13, 15, 19).
#include "HDPlayerController.h"
#include "HDTerritory.h"
#include "HDStructureActor.h"
#include "HDShipPawn.h"
#include "HDShipMovementComponent.h"
#include "HDNavComponent.h"
#include "HDCargoComponent.h"
#include "HDCatalog.h"
#include "HDPlayerState.h"
#include "HDWorldActors.h"
#include "HDNpcController.h"
#include "HDSystemDirector.h"
#include "HDUniverseSubsystem.h"
#include "HDGameMode.h"
#include "EngineUtils.h"

namespace
{
	AHDPlayerState* TerrCmdFindPilot(UWorld* World, const FString& PilotOrName)
	{
		AGameStateBase* GSB = World ? World->GetGameState() : nullptr;
		if (!GSB) return nullptr;
		for (APlayerState* P : GSB->PlayerArray)
		{
			AHDPlayerState* H = Cast<AHDPlayerState>(P);
			if (H && (H->PilotId == PilotOrName || H->GetPlayerName().Equals(PilotOrName, ESearchCase::IgnoreCase))) return H;
		}
		return nullptr;
	}

	/** Nearest structure owned by Corp within MaxCm of the ship. */
	FHDStructure* TerrCmdNearestOwn(UHDTerritorySubsystem* Terr, const AHDShipPawn* Ship, const FName& Corp, double MaxCm)
	{
		AHDSystemDirector* Dir = AHDSystemDirector::Get(Ship);
		if (!Terr || !Ship || !Dir || !Dir->IsActive(Ship->SystemIndex)) return nullptr;
		const FVector Origin = Dir->GetOrigin(Ship->SystemIndex);
		FHDStructure* Best = nullptr;
		double BestD = MaxCm;
		for (TPair<FName, FHDStructure>& P : Terr->Structures)
		{
			FHDStructure& St = P.Value;
			if (St.System != Ship->SystemIndex || St.bDerelict || (!Corp.IsNone() && St.Corp != Corp)) continue;
			const double Dist = FVector::Dist(Origin + St.Local, Ship->GetActorLocation());
			if (Dist < BestD) { BestD = Dist; Best = &St; }
		}
		return Best;
	}

	bool TerrCmdParseType(const FString& S, EHDStructureType& Out)
	{
		const FString L = S.ToLower();
		if (L == TEXT("beacon")) Out = EHDStructureType::Beacon;
		else if (L == TEXT("gun")) Out = EHDStructureType::GunPlacement;
		else if (L == TEXT("platform")) Out = EHDStructureType::DefensePlatform;
		else if (L == TEXT("habitat")) Out = EHDStructureType::Habitat;
		else if (L == TEXT("refinery")) Out = EHDStructureType::Refinery;
		else if (L == TEXT("factory")) Out = EHDStructureType::Factory;
		else if (L == TEXT("fuel")) Out = EHDStructureType::FuelPlatform;
		else if (L == TEXT("station")) Out = EHDStructureType::Station;
		else if (L == TEXT("interdictor")) Out = EHDStructureType::Interdictor;
		else return false;
		return true;
	}

	FString TerrCmdRest(const TArray<FString>& Args, int32 From)
	{
		TArray<FString> Parts;
		for (int32 I = From; I < Args.Num(); ++I) Parts.Add(Args[I]);
		return FString::Join(Parts, TEXT(" "));
	}
}

bool AHDPlayerController::HandleTerritoryCommand(const FString& Verb, const FString& Arg, AActor* Target)
{
	static const TCHAR* Verbs[] = { TEXT("corp"), TEXT("claim"), TEXT("anchor"), TEXT("structures"), TEXT("supply"), TEXT("unanchor"), TEXT("station"), TEXT("platform"),
		TEXT("capture"), TEXT("corplane"), TEXT("interdict"), TEXT("colony"), TEXT("governor"), TEXT("siegenow"), TEXT("persist") };
	bool bMine = false;
	for (const TCHAR* V : Verbs) { if (Verb == V) { bMine = true; break; } }
	if (!bMine) return false;

	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	AHDPlayerState* PS = GetPlayerState<AHDPlayerState>();
	UHDTerritorySubsystem* Terr = UHDTerritorySubsystem::Get(this);
	UWorld* World = GetWorld();
	if (!PS || !Terr || !World) return true;
	TArray<FString> Args;
	Arg.ParseIntoArrayWS(Args);
	const FString Sub = Args.Num() > 0 ? Args[0].ToLower() : FString();
	const double Now = AHDPlayerState::NowUnix();
	FHDCorp* MyCorp = Terr->CorpOf(PS);
	const bool bCeo = Terr->IsCeo(PS);

	if (Verb == TEXT("persist"))
	{
		// persist [save]: backend persistence status (M7); "save" writes world + online pilots now.
		AHDGameMode* GM = World->GetAuthGameMode<AHDGameMode>();
		if (!GM) return true;
		if (Sub == TEXT("save")) { GM->SaveWorldState(); Notify(TEXT("Saving world state and online pilots...")); }
		Notify(GM->DescribePersistence());
		return true;
	}

	// ---------------------------------------------------------------- corp
	if (Verb == TEXT("corp"))
	{
		if (Sub == TEXT("create"))
		{
			if (MyCorp) { Notify(TEXT("Leave your company first.")); return true; }
			if (Args.Num() < 2) { Notify(TEXT("corp create <id> <name>")); return true; }
			const FName Id(*Args[1].ToLower());
			if (Terr->Corps.Contains(Id)) { Notify(TEXT("That id is taken.")); return true; }
			if (!PS->TrySpend(100000)) { Notify(TEXT("Registering a company costs 100,000 cr.")); return true; }
			FHDCorp NewCorp;
			NewCorp.Id = Id;
			NewCorp.Name = Args.Num() > 2 ? TerrCmdRest(Args, 2) : Args[1];
			NewCorp.Ceo = PS->PilotId;
			NewCorp.Members.Add(PS->PilotId);
			Terr->Corps.Add(Id, NewCorp);
			PS->Corp = Id;
			Notify(FString::Printf(TEXT("Company %s [%s] registered. You are CEO."), *NewCorp.Name, *Id.ToString()));
			return true;
		}
		if (Sub == TEXT("invite"))
		{
			if (!bCeo || !MyCorp) { Notify(TEXT("CEO only.")); return true; }
			AHDPlayerState* Invitee = Args.Num() > 1 ? TerrCmdFindPilot(World, Args[1]) : nullptr;
			if (!Invitee) { Notify(TEXT("Pilot not online.")); return true; }
			MyCorp->Invites.AddUnique(Invitee->PilotId);
			if (AHDPlayerController* IPC = Cast<AHDPlayerController>(Invitee->GetOwner())) IPC->Notify(FString::Printf(TEXT("%s invites you to %s: corp join %s"), *PS->GetPlayerName(), *MyCorp->Name, *MyCorp->Id.ToString()));
			Notify(TEXT("Invite sent."));
			return true;
		}
		if (Sub == TEXT("join"))
		{
			if (MyCorp) { Notify(TEXT("Leave your company first.")); return true; }
			FHDCorp* Join = Args.Num() > 1 ? Terr->Corps.Find(FName(*Args[1].ToLower())) : nullptr;
			if (!Join || !Join->Invites.Contains(PS->PilotId)) { Notify(TEXT("No invite from that company.")); return true; }
			Join->Invites.Remove(PS->PilotId);
			Join->Members.AddUnique(PS->PilotId);
			PS->Corp = Join->Id;
			Notify(FString::Printf(TEXT("Joined %s."), *Join->Name));
			return true;
		}
		if (Sub == TEXT("leave"))
		{
			if (!MyCorp) { Notify(TEXT("Not in a company.")); return true; }
			if (bCeo && MyCorp->Members.Num() > 1) { Notify(TEXT("Hand over or disband first: the CEO leaves last.")); return true; }
			MyCorp->Members.Remove(PS->PilotId);
			const FName OldCorp = PS->Corp;
			PS->Corp = NAME_None;
			if (MyCorp->Members.Num() == 0) { PS->AddCredits(MyCorp->Treasury); Terr->Corps.Remove(OldCorp); Notify(TEXT("Company disbanded; treasury paid out to you.")); }
			else Notify(TEXT("Left the company."));
			return true;
		}
		if (Sub == TEXT("deposit") || Sub == TEXT("withdraw"))
		{
			if (!MyCorp) { Notify(TEXT("Not in a company.")); return true; }
			const int64 Amount = Args.Num() > 1 ? FCString::Atoi64(*Args[1]) : 0;
			if (Amount <= 0) { Notify(TEXT("Amount?")); return true; }
			if (Sub == TEXT("deposit"))
			{
				if (!PS->TrySpend(Amount)) { Notify(TEXT("Not enough credits.")); return true; }
				MyCorp->Treasury += Amount;
			}
			else
			{
				if (!bCeo) { Notify(TEXT("CEO only.")); return true; }
				if (MyCorp->Treasury < Amount) { Notify(TEXT("Treasury is short.")); return true; }
				MyCorp->Treasury -= Amount;
				PS->AddCredits(Amount);
			}
			Notify(FString::Printf(TEXT("Treasury now %lld cr"), MyCorp->Treasury));
			return true;
		}
		if (Sub == TEXT("war"))
		{
			AHDGameState* GS = World->GetGameState<AHDGameState>();
			if (!bCeo || !GS) { Notify(TEXT("CEO only.")); return true; }
			const FName Enemy = Args.Num() > 1 ? FName(*Args[1].ToLower()) : NAME_None;
			if (!Terr->Corps.Contains(Enemy) || Enemy == PS->Corp) { Notify(TEXT("corp war <company id>")); return true; }
			GS->DeclareWar(PS->Corp, Enemy);
			Notify(FString::Printf(TEXT("War declared on %s. It goes live after the notice period."), *Enemy.ToString()));
			return true;
		}
		if (Sub == TEXT("list"))
		{
			for (const TPair<FName, FHDCorp>& P : Terr->Corps) Notify(FString::Printf(TEXT("[%s] %s, %d members"), *P.Key.ToString(), *P.Value.Name, P.Value.Members.Num()));
			if (Terr->Corps.Num() == 0) Notify(TEXT("No companies yet: corp create <id> <name>"));
			return true;
		}
		// info
		if (!MyCorp) { Notify(TEXT("Not in a company. corp create <id> <name> | corp join <id> | corp list")); return true; }
		int32 NumClaims = 0, NumStructures = 0;
		for (const TPair<FName, FHDClaim>& P : Terr->Claims) { if (P.Value.Corp == MyCorp->Id) ++NumClaims; }
		for (const TPair<FName, FHDStructure>& P : Terr->Structures) { if (P.Value.Corp == MyCorp->Id && !P.Value.bDerelict) ++NumStructures; }
		Notify(FString::Printf(TEXT("[%s] %s | CEO %s | %d members | treasury %lld cr (toll income lifetime %lld) | %d claims, %d structures"),
			*MyCorp->Id.ToString(), *MyCorp->Name, *MyCorp->Ceo, MyCorp->Members.Num(), MyCorp->Treasury, MyCorp->TollIncome, NumClaims, NumStructures));
		return true;
	}

	// ---------------------------------------------------------------- claim
	if (Verb == TEXT("claim"))
	{
		if (!Ship) return true;
		FHDClaim* Claim = Terr->ClaimInSystem(Ship->SystemIndex);
		if (!Claim) { Notify(TEXT("No claim in this system. Anchor a beacon: build kit_beacon at a factory, then anchor beacon (not in Secured).")); return true; }
		const bool bOwner = PS->Corp == Claim->Corp;
		if (Sub.IsEmpty() || Sub == TEXT("info")) { NotifyLines(Terr->DescribeClaim(*Claim)); return true; }
		if (Sub == TEXT("reside"))
		{
			if (!bOwner) { Notify(TEXT("Residency is for members of the claiming company.")); return true; }
			if (!TerrCmdNearestOwn(Terr, Ship, Claim->Corp, 1.0e7)) { Notify(TEXT("Register at one of your structures (within 100 km).")); return true; }
			Claim->Residents.Add(PS->PilotId, Now);
			Notify(FString::Printf(TEXT("Registered resident. Counted residents: %d / %d berths (14-day window)."), Terr->ResidentCount(*Claim), Terr->Berths(*Claim)));
			return true;
		}
		if (Sub == TEXT("hires"))
		{
			for (const FHDHire& H : Claim->Hires) Notify(FString::Printf(TEXT("%s: %lld cr/day, %.1f days left, paid %lld"), *H.Pilot, H.RatePerDay, (H.UntilUnix - Now) / 86400.0, H.PaidOut));
			if (Claim->Hires.Num() == 0) Notify(TEXT("No hired defenders."));
			return true;
		}
		if (!bOwner || !bCeo) { Notify(TEXT("Only the claiming company's CEO can change the claim.")); return true; }
		if (Sub == TEXT("budget"))
		{
			if (Args.Num() < 3) { Notify(TEXT("claim budget <tier 1-5> <cr/day>  (1 guns, 2 platforms, 3 capital stipend, 4 NPC-flown, 5 hired players)")); return true; }
			FString Why;
			const int32 Tier = FCString::Atoi(*Args[1]) - 1;
			if (!Terr->SetAllocation(*Claim, Tier, FCString::Atof(*Args[2]), Why)) { Notify(Why); return true; }
			NotifyLines(Terr->DescribeClaim(*Claim));
			return true;
		}
		if (Sub == TEXT("hire"))
		{
			if (Args.Num() < 4) { Notify(TEXT("claim hire <pilot> <cr/day> <days>")); return true; }
			FString Why;
			AHDPlayerState* Hiree = TerrCmdFindPilot(World, Args[1]);
			if (!Terr->Hire(*Claim, Hiree, FCString::Atoi64(*Args[2]), FCString::Atof(*Args[3]), Why)) { Notify(Why); return true; }
			if (AHDPlayerController* HPC = Cast<AHDPlayerController>(Hiree->GetOwner())) HPC->Notify(FString::Printf(TEXT("Hired to defend %s's claim: %s cr/day, escrowed"), *Claim->Corp.ToString(), *Args[2]));
			Notify(TEXT("Contract escrowed from tier 5."));
			return true;
		}
		if (Sub == TEXT("kos"))
		{
			if (Args.Num() < 3) { Notify(TEXT("claim kos add|remove <company>")); return true; }
			const FName Kos(*Args[2].ToLower());
			if (Args[1] == TEXT("add")) Claim->Kos.AddUnique(Kos); else Claim->Kos.Remove(Kos);
			Notify(FString::Printf(TEXT("Kill-on-sight list: %d companies"), Claim->Kos.Num()));
			return true;
		}
		if (Sub == TEXT("window"))
		{
			Claim->VulnHourUtc = Args.Num() > 1 ? FMath::Clamp(FCString::Atoi(*Args[1]), 0, 23) : Claim->VulnHourUtc;
			Notify(FString::Printf(TEXT("Vulnerability window: %02d:00-%02d:00 UTC (applies to the next reinforcement)"), Claim->VulnHourUtc, (Claim->VulnHourUtc + 2) % 24));
			return true;
		}
		if (Sub == TEXT("minstanding"))
		{
			Claim->HireMinStanding = Args.Num() > 1 ? FCString::Atof(*Args[1]) : -10.f;
			Notify(FString::Printf(TEXT("Hire vetting: Accord standing >= %.1f"), Claim->HireMinStanding));
			return true;
		}
		if (Sub == TEXT("wing"))
		{
			Claim->WingFaction = Args.Num() > 1 && Args[1] != TEXT("none") ? FName(*Args[1].ToLower()) : NAME_None;
			Notify(Claim->WingFaction.IsNone() ? FString(TEXT("NPC squadron wing contract cleared.")) : FString::Printf(TEXT("Tier 4 wings contracted from %s (needs >= 12,000 cr/day and non-negative standing)."), *Claim->WingFaction.ToString()));
			return true;
		}
		if (Sub == TEXT("toll"))
		{
			Claim->SiteTollPct = Args.Num() > 2 ? FMath::Clamp(FCString::Atoi(*Args[2]), 0, 50) : Claim->SiteTollPct;
			Notify(FString::Printf(TEXT("Site salvage toll: %d%% for non-members (Borderland claims). Tolls never count toward DB."), Claim->SiteTollPct));
			return true;
		}
#if !UE_BUILD_SHIPPING
		if (Sub == TEXT("simulate"))
		{
			// claim simulate <output M> <trade M> <residents>: set the slow inputs for testing.
			Claim->Output30 = Args.Num() > 1 ? FCString::Atod(*Args[1]) * 1.0e6 : Claim->Output30;
			Claim->Trade30 = Args.Num() > 2 ? FCString::Atod(*Args[2]) * 1.0e6 : Claim->Trade30;
			if (Args.Num() > 3)
			{
				const int32 Fake = FMath::Clamp(FCString::Atoi(*Args[3]), 0, 500);
				for (int32 I = 0; I < Fake; ++I) Claim->Residents.Add(FString::Printf(TEXT("sim-%d"), I), Now);
			}
			Notify(FString::Printf(TEXT("Raw DB now %.0f cr/day (window still applies: claim hours <n>)"), Terr->RawDefenseBudget(*Claim)));
			return true;
		}
		if (Sub == TEXT("hours"))
		{
			// claim hours <n>: push n hourly samples of the current raw DB into the anti-spike window.
			const int32 Hours = Args.Num() > 1 ? FMath::Clamp(FCString::Atoi(*Args[1]), 1, 168) : 24;
			for (int32 I = 0; I < Hours; ++I) Claim->DbSamples.Add(Terr->RawDefenseBudget(*Claim));
			while (Claim->DbSamples.Num() > 168) Claim->DbSamples.RemoveAt(0);
			NotifyLines(Terr->DescribeClaim(*Claim));
			return true;
		}
#endif
		Notify(TEXT("claim info|reside|hires|budget|hire|kos|window|minstanding|wing|toll site <pct>"));
		return true;
	}

	// ---------------------------------------------------------------- structures
	if (Verb == TEXT("anchor"))
	{
		EHDStructureType Type;
		if (!Ship || !TerrCmdParseType(Sub, Type)) { Notify(TEXT("anchor beacon|gun|platform|habitat|refinery|factory|fuel|station|interdictor (needs the kit in your hold)")); return true; }
		FString Msg;
		Terr->Anchor(Ship, Type, Msg);
		Notify(Msg);
		return true;
	}
	if (Verb == TEXT("structures"))
	{
		if (!Ship) return true;
		int32 N = 0;
		for (const TPair<FName, FHDStructure>& P : Terr->Structures)
		{
			const FHDStructure& St = P.Value;
			if (St.System != Ship->SystemIndex) continue;
			static const TCHAR* SiegeNames[] = { TEXT(""), TEXT(" REINFORCED"), TEXT(" VULNERABLE") };
			Notify(FString::Printf(TEXT("%s %s [%s] %.0f%%%s fuel %.0f ammo %.0f%s%s"), *St.Id.ToString(), UHDTerritorySubsystem::TypeName(St.Type), *St.Corp.ToString(),
				100.f * St.Integrity / FMath::Max(1.f, St.MaxIntegrity), SiegeNames[(int32)St.Siege], St.Fuel, St.Ammo, St.bDerelict ? TEXT(" DERELICT") : TEXT(""),
				St.Sections.Num() > 0 ? *FString::Printf(TEXT(" sections %d (eff %.0f%%)"), St.Sections.Num(), 100.f * UHDTerritorySubsystem::RoutingEfficiency(St)) : TEXT("")));
			++N;
		}
		if (N == 0) Notify(TEXT("No structures in this system."));
		return true;
	}
	if (Verb == TEXT("supply"))
	{
		// supply fuel|ammo <n>: move jump_fuel or munitions from your hold into the nearest company structure.
		if (!Ship || !Ship->Cargo || Args.Num() < 2) { Notify(TEXT("supply fuel|ammo <n> (within 50 km of your structure)")); return true; }
		FHDStructure* St = TerrCmdNearestOwn(Terr, Ship, PS->Corp, 5.0e6);
		if (!St || PS->Corp.IsNone()) { Notify(TEXT("No company structure within 50 km.")); return true; }
		const bool bFuel = Sub == TEXT("fuel");
		const FName Item = bFuel ? FName(TEXT("jump_fuel")) : FName(TEXT("munitions"));
		const int32 Moved = Ship->Cargo->Remove(Item, FMath::Max(1, FCString::Atoi(*Args[1])));
		if (Moved <= 0) { Notify(FString::Printf(TEXT("No %s in your hold."), *Item.ToString())); return true; }
		if (bFuel) { St->Fuel += Moved; St->FuelEmptySinceUnix = 0.0; } else { St->Ammo += Moved; }
		Notify(FString::Printf(TEXT("%s supplied: fuel %.0f (%.1f days), ammo %.0f"), *St->Id.ToString(), St->Fuel, St->Fuel / FMath::Max(0.1f, UHDTerritorySubsystem::UpkeepFuelPerDay(*St)), St->Ammo));
		return true;
	}
	if (Verb == TEXT("unanchor"))
	{
		if (!Ship || !Ship->Cargo) return true;
		FHDStructure* St = TerrCmdNearestOwn(Terr, Ship, PS->Corp, 5.0e6);
		if (!St || PS->Corp.IsNone()) { Notify(TEXT("No company structure within 50 km.")); return true; }
		if (St->Siege != EHDSiegeState::Normal || Now - St->LastAttackedUnix < 900.0) { Notify(TEXT("Can't unanchor under fire or siege.")); return true; }
		if (St->Type == EHDStructureType::Beacon)
		{
			for (const TPair<FName, FHDStructure>& P : Terr->Structures) { if (P.Value.System == St->System && P.Value.Corp == St->Corp && P.Value.Type != EHDStructureType::Beacon && !P.Value.bDerelict) { Notify(TEXT("Unanchor the claim's other structures first.")); return true; } }
			for (auto It = Terr->Claims.CreateIterator(); It; ++It) { if (It.Value().Beacon == St->Id) It.RemoveCurrent(); }
		}
		const FName Kit = UHDTerritorySubsystem::KitFor(St->Type);
		const bool bFull = St->Integrity >= 0.95f * St->MaxIntegrity;
		if (bFull) Ship->Cargo->Add(Kit, 1);
		const FName Id = St->Id;
		if (TWeakObjectPtr<AHDStructureActor>* W = Terr->StructureActors.Find(Id)) { if (W->IsValid()) (*W)->Destroy(); }
		if (TWeakObjectPtr<AHDStationActor>* WS = Terr->StationActors.Find(St->StationId)) { if (WS->IsValid()) (*WS)->Destroy(); }
		Terr->StructureActors.Remove(Id);
		Terr->Structures.Remove(Id);
		Notify(bFull ? FString::Printf(TEXT("Unanchored; %s back in your hold."), *Kit.ToString()) : FString(TEXT("Unanchored. Too damaged to repack the kit.")));
		return true;
	}
	if (Verb == TEXT("station"))
	{
		// station add <section> | station remove | station family modular|kitbash
		if (!Ship || !Ship->Cargo) return true;
		FHDStructure* St = TerrCmdNearestOwn(Terr, Ship, PS->Corp, 1.0e7);
		if (!St || St->Type != EHDStructureType::Station) { Notify(TEXT("No company station within 100 km.")); return true; }
		if (Sub == TEXT("family"))
		{
			if (St->Sections.Num() > 0) { Notify(TEXT("The family is set before the first section.")); return true; }
			St->Family = Args.Num() > 1 && Args[1] == TEXT("kitbash") ? EHDStationFamily::PirateKitbash : EHDStationFamily::CorporateModular;
			Notify(St->Family == EHDStationFamily::PirateKitbash ? TEXT("Kitbash: cheap, unlimited sections, but past 6 the joins lose efficiency and stress can shear sections off.") : TEXT("Modular: clean joins, hard cap of 8 sections."));
			return true;
		}
		if (Sub == TEXT("add"))
		{
			static const TCHAR* Sections[] = { TEXT("power"), TEXT("hardpoint"), TEXT("support"), TEXT("market"), TEXT("refinery"), TEXT("shipyard"), TEXT("dock") };
			const FString Want = Args.Num() > 1 ? Args[1].ToLower() : FString();
			bool bValid = false;
			for (const TCHAR* Sec : Sections) { if (Want == Sec) bValid = true; }
			if (!bValid) { Notify(TEXT("station add power|hardpoint|support|market|refinery|shipyard|dock (one kit_section each)")); return true; }
			if (St->Family == EHDStationFamily::CorporateModular && St->Sections.Num() >= Terr->StandardSections(*St)) { Notify(TEXT("Modular stations cap at 8 sections. Kitbash has no cap (and no structural floor).")); return true; }
			if (Ship->Cargo->Remove(TEXT("kit_section"), 1) < 1) { Notify(TEXT("Needs a kit_section in your hold.")); return true; }
			St->Sections.Add(FName(*Want));
			Terr->ApplyStationSections(*St);
			Notify(FString::Printf(TEXT("Section %s added: %d sections, routing efficiency %.0f%%"), *Want, St->Sections.Num(), 100.f * UHDTerritorySubsystem::RoutingEfficiency(*St)));
			return true;
		}
		if (Sub == TEXT("remove"))
		{
			if (St->Sections.Num() == 0) { Notify(TEXT("No sections.")); return true; }
			const FName Lost = St->Sections.Pop();
			Ship->Cargo->Add(TEXT("kit_section"), 1);
			Terr->ApplyStationSections(*St);
			Notify(FString::Printf(TEXT("Removed the %s section."), *Lost.ToString()));
			return true;
		}
		Notify(TEXT("station family|add|remove"));
		return true;
	}
	if (Verb == TEXT("platform"))
	{
		// platform access open|standing <min>|corp ; platform toll <cr>
		if (!Ship) return true;
		FHDStructure* St = TerrCmdNearestOwn(Terr, Ship, PS->Corp, 1.0e7);
		if (!St || (St->Type != EHDStructureType::FuelPlatform && St->Type != EHDStructureType::Station)) { Notify(TEXT("No company fuel platform or station within 100 km.")); return true; }
		if (Sub == TEXT("access"))
		{
			const FString Mode = Args.Num() > 1 ? Args[1].ToLower() : FString();
			if (Mode == TEXT("open")) St->Access = EHDAccess::Open;
			else if (Mode == TEXT("standing")) { St->Access = EHDAccess::Standing; St->StandingMin = Args.Num() > 2 ? FCString::Atof(*Args[2]) : 0.f; }
			else if (Mode == TEXT("corp")) St->Access = EHDAccess::Corp;
			else { Notify(TEXT("platform access open|standing <min>|corp")); return true; }
			Notify(TEXT("Access updated."));
			return true;
		}
		if (Sub == TEXT("toll"))
		{
			St->Toll = Args.Num() > 1 ? FMath::Clamp(FCString::Atoi(*Args[1]), 0, 100000) : St->Toll;
			Notify(FString::Printf(TEXT("Docking toll %d cr (to the treasury, never DB)."), St->Toll));
			return true;
		}
		static const TCHAR* AccessNames[] = { TEXT("open"), TEXT("standing"), TEXT("company") };
		Notify(FString::Printf(TEXT("%s: access %s, toll %d cr"), *St->Id.ToString(), AccessNames[(int32)St->Access], St->Toll));
		return true;
	}
	if (Verb == TEXT("capture"))
	{
		// capture: take over a pirate base once its defenders are dead (doc 19).
		if (!Ship || PS->Corp.IsNone()) { Notify(TEXT("Capture for a company: join or create one.")); return true; }
		AHDStationActor* Base = nullptr;
		for (TActorIterator<AHDStationActor> It(World); It; ++It)
		{
			if (It->SystemIndex == Ship->SystemIndex && It->Info.Family == EHDStationFamily::PirateKitbash && It->OwnerCorp.IsNone() && FVector::Dist(It->GetActorLocation(), Ship->GetActorLocation()) < 1.0e7) { Base = *It; break; }
		}
		if (!Base) { Notify(TEXT("No unowned pirate base within 100 km.")); return true; }
		for (TActorIterator<AHDNpcController> It(World); It; ++It)
		{
			const AHDShipPawn* Def = It->GetShip();
			if (Def && It->Pop == EHDNpcPop::Pirate && Def->SystemIndex == Ship->SystemIndex && FVector::Dist(Def->GetActorLocation(), Base->GetActorLocation()) < 2.0e7) { Notify(TEXT("Its defenders are still flying.")); return true; }
		}
		Base->OwnerCorp = PS->Corp;
		Terr->CapturedStations.Add(Base->ObjectId, PS->Corp);
		Notify(FString::Printf(TEXT("%s captured for %s. Docking is now yours to grant."), *Base->DisplayName, *PS->Corp.ToString()));
		return true;
	}
	if (Verb == TEXT("corplane"))
	{
		if (!Ship || !Ship->Cargo) return true;
		AHDSystemDirector* Dir = AHDSystemDirector::Get(this);
		FHDActiveSystem* Act = Dir ? Dir->FindActive(Ship->SystemIndex) : nullptr;
		if (!Act) return true;
		if (Sub == TEXT("start"))
		{
			if (PS->Corp.IsNone()) { Notify(TEXT("Lanes belong to companies.")); return true; }
			Terr->LaneStarts.Add(PS->PilotId, Ship->GetActorLocation() - Act->Origin);
			Notify(TEXT("Lane start marked. Fly at least 500 km and: corplane finish <toll>"));
			return true;
		}
		if (Sub == TEXT("finish"))
		{
			const FVector* Start = Terr->LaneStarts.Find(PS->PilotId);
			if (!Start) { Notify(TEXT("corplane start first.")); return true; }
			const FVector EndLocal = Ship->GetActorLocation() - Act->Origin;
			if (FVector::Dist(*Start, EndLocal) < 5.0e7) { Notify(TEXT("A lane runs at least 500 km.")); return true; }
			if (Ship->Cargo->Remove(TEXT("kit_lane"), 1) < 1) { Notify(TEXT("Needs a kit_lane in your hold.")); return true; }
			FHDPlayerLane PL;
			PL.System = Ship->SystemIndex;
			PL.Lane.Id = FName(*FString::Printf(TEXT("plane-%d-%d"), Ship->SystemIndex, Terr->Lanes.Num() + 1));
			PL.Lane.Owner = PS->Corp;
			PL.Lane.bNPC = false;
			PL.Lane.StartLocal = *Start;
			PL.Lane.EndLocal = EndLocal;
			PL.Lane.Toll = Args.Num() > 1 ? FMath::Clamp(FCString::Atoi(*Args[1]), 0, 100000) : 0;
			Terr->Lanes.Add(PL);
			Terr->LaneStarts.Remove(PS->PilotId);
			const FTransform T(FRotator::ZeroRotator, Act->Origin + PL.Lane.StartLocal);
			if (AHDTradeLaneActor* LA = World->SpawnActorDeferred<AHDTradeLaneActor>(AHDTradeLaneActor::StaticClass(), T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
			{
				LA->InitLane(Ship->SystemIndex, PL.Lane, Act->Origin);
				LA->FinishSpawning(T);
				Act->Lanes.Add(LA);
				Act->Persistent.Add(LA);
			}
			Notify(FString::Printf(TEXT("Lane %s online, toll %d cr. Tolls go to the treasury, never DB."), *PL.Lane.Id.ToString(), PL.Lane.Toll));
			return true;
		}
		if (Sub == TEXT("toll") || Sub == TEXT("suppress"))
		{
			// corplane toll <cr> [id] | corplane suppress on|off [id]: nearest own lane if no id.
			FHDPlayerLane* Mine = nullptr;
			const FString WantId = Args.Num() > 2 ? Args[2] : FString();
			double BestD = TNumericLimits<double>::Max();
			for (FHDPlayerLane& L : Terr->Lanes)
			{
				if (L.Lane.Owner != PS->Corp) continue;
				if (!WantId.IsEmpty()) { if (L.Lane.Id.ToString() == WantId) { Mine = &L; break; } continue; }
				if (L.System != Ship->SystemIndex) continue;
				const double Dist = FVector::Dist(Act->Origin + L.Lane.StartLocal, Ship->GetActorLocation());
				if (Dist < BestD) { BestD = Dist; Mine = &L; }
			}
			if (!Mine) { Notify(TEXT("No company lane found.")); return true; }
			if (Sub == TEXT("toll")) Mine->Lane.Toll = Args.Num() > 1 ? FMath::Clamp(FCString::Atoi(*Args[1]), 0, 100000) : Mine->Lane.Toll;
			else
			{
				if (!bCeo) { Notify(TEXT("CEO only: suppression costs 50,000 cr/day from the treasury.")); return true; }
				Mine->bSuppressed = Args.Num() > 1 && Args[1] == TEXT("on");
			}
			for (TActorIterator<AHDTradeLaneActor> It(World); It; ++It) { if (It->Lane.Id == Mine->Lane.Id) { It->Lane.Toll = Mine->Lane.Toll; It->bSuppressed = Mine->bSuppressed; } }
			Notify(FString::Printf(TEXT("%s: toll %d cr, suppression %s"), *Mine->Lane.Id.ToString(), Mine->Lane.Toll, Mine->bSuppressed ? TEXT("ON (50,000 cr/day)") : TEXT("off")));
			return true;
		}
		for (const FHDPlayerLane& L : Terr->Lanes) { if (L.Lane.Owner == PS->Corp) Notify(FString::Printf(TEXT("%s in NP-%04d: toll %d%s"), *L.Lane.Id.ToString(), L.System, L.Lane.Toll, L.bSuppressed ? TEXT(", suppressed") : TEXT(""))); }
		Notify(TEXT("corplane start | corplane finish <toll> | corplane toll <cr> [id] | corplane suppress on|off [id]"));
		return true;
	}
	if (Verb == TEXT("interdict"))
	{
		if (!Ship) return true;
		if (Ship->GetWorld()->GetTimeSeconds() < InterdictReadyAt) { Notify(TEXT("Interdiction field recharging.")); return true; }
		FString Msg;
		if (Terr->Interdict(Ship, Msg)) InterdictReadyAt = Ship->GetWorld()->GetTimeSeconds() + 60.f;
		Notify(Msg);
		return true;
	}

	// ---------------------------------------------------------------- colonies
	if (Verb == TEXT("colony") || Verb == TEXT("governor"))
	{
		UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
		if (!Ship || !Ship->Nav || !U) return true;
		const AHDCelestialBody* Landed = Cast<AHDCelestialBody>(Ship->Nav->LandedOn);
		FHDColony* Here = Landed ? Terr->Colonies.Find(UHDTerritorySubsystem::ColonyKey(Ship->SystemIndex, Landed->BodyIndex)) : nullptr;
		if (Verb == TEXT("colony") && Sub == TEXT("list"))
		{
			for (const TPair<FName, FHDColony>& P : Terr->Colonies) { if (P.Value.Corp == PS->Corp) Notify(FString::Printf(TEXT("%s (%s): pop %.0f, last week %.0f cr"), *P.Value.Name, *P.Key.ToString(), P.Value.Population, P.Value.LastOutput)); }
			return true;
		}
		if (Verb == TEXT("colony") && Sub == TEXT("found"))
		{
			if (!Landed) { Notify(TEXT("Land on the world first.")); return true; }
			if (Here) { Notify(TEXT("Already settled.")); return true; }
			if (PS->Corp.IsNone()) { Notify(TEXT("Colonies belong to companies.")); return true; }
			const FHDStarSystem& Sys = U->GetSystem(Ship->SystemIndex);
			if (Sys.Band == EHDSecurityBand::Secured) { Notify(TEXT("Secured worlds are Accord-held.")); return true; }
			if (!Sys.Bodies.IsValidIndex(Landed->BodyIndex)) return true;
			const FHDBody& B = Sys.Bodies[Landed->BodyIndex];
			if (B.Type == EHDBodyType::GasGiant || B.Type == EHDBodyType::Star) { Notify(TEXT("Can't settle that.")); return true; }
			if (!Ship->Cargo || Ship->Cargo->Remove(TEXT("kit_colony"), 1) < 1) { Notify(TEXT("Needs a kit_colony in your hold.")); return true; }
			FHDColony Col;
			Col.Id = UHDTerritorySubsystem::ColonyKey(Ship->SystemIndex, Landed->BodyIndex);
			Col.Corp = PS->Corp;
			Col.Owner = PS->PilotId;
			Col.System = Ship->SystemIndex;
			Col.Body = Landed->BodyIndex;
			Col.Name = Args.Num() > 1 ? TerrCmdRest(Args, 1) : B.Name;
			Col.Buildings.Add(TEXT("habitat"), 1);
			Col.NextWeekUnix = Now + 7.0 * 86400.0;
			Terr->Colonies.Add(Col.Id, Col);
			Notify(FString::Printf(TEXT("Colony %s founded on %s (minerals %.2f, water %.2f, fertility %.2f, volatiles %.2f). Build directly or hire a governor."),
				*Col.Name, *B.Name, B.Minerals, B.Water, B.Fertility, B.Volatiles));
			return true;
		}
		if (!Here) { Notify(TEXT("Land on one of your colonies (colony found <name> to settle, colony list).")); return true; }
		if (Here->Corp != PS->Corp) { Notify(FString::Printf(TEXT("%s belongs to %s."), *Here->Name, *Here->Corp.ToString())); return true; }
		const FHDBody& Body = U->GetSystem(Here->System).Bodies[Here->Body];
		FHDCorp* ColCorp = Terr->Corps.Find(Here->Corp);
		if (Verb == TEXT("colony"))
		{
			if (Sub == TEXT("build"))
			{
				// Direct mode: you pay and it goes up now. Same gates the governors face.
				const FName Bld = Args.Num() > 1 ? FName(*Args[1].ToLower()) : NAME_None;
				FString Why;
				if (!UHDTerritorySubsystem::CanBuild(Body, Bld, Why)) { Notify(Why); return true; }
				const int64 Cost = UHDTerritorySubsystem::BuildingCost(Bld);
				if (!ColCorp || ColCorp->Treasury < Cost) { Notify(FString::Printf(TEXT("Treasury needs %lld cr."), Cost)); return true; }
				ColCorp->Treasury -= Cost;
				Here->Buildings.FindOrAdd(Bld) += 1;
				Notify(FString::Printf(TEXT("Built %s (%lld cr from the treasury)."), *Bld.ToString(), Cost));
				return true;
			}
			if (Sub == TEXT("take"))
			{
				// colony take: load the colony's stockpile into your hold.
				if (!Ship->Cargo) return true;
				int32 Loaded = 0;
				for (int32 I = PS->Storage.Num() - 1; I >= 0; --I)
				{
					const FHDStoredStack Stack = PS->Storage[I];
					if (Stack.Station != Here->Id) continue;
					const int32 Put = Ship->Cargo->Add(Stack.Item, Stack.Qty, Stack.Flags);
					if (Put > 0) { PS->RemoveStorage(Here->Id, Stack.Item, Put); Loaded += Put; }
				}
				Notify(Loaded > 0 ? FString::Printf(TEXT("Loaded %d units from the colony stockpile."), Loaded) : FString(TEXT("Nothing in the stockpile (or your hold is full).")));
				return true;
			}
			FString Blds;
			for (const TPair<FName, int32>& K : Here->Buildings) Blds += FString::Printf(TEXT(" %s x%d"), *K.Key.ToString(), K.Value);
			Notify(FString::Printf(TEXT("%s: pop %.0f | buildings:%s | ~%.0f cr/day | governor %s"), *Here->Name, Here->Population, *Blds, Terr->ColonyDailyOutput(*Here, Body),
				Here->GovernorQuality > 0.f ? *FString::Printf(TEXT("%s (%.0f%%, focus %s, budget %lld/week)"), *Here->Governor, Here->GovernorQuality * 100.f, *Here->Focus.ToString(), Here->WeeklyBudget) : TEXT("none (direct)")));
			return true;
		}
		// governor
		if (Sub == TEXT("hire"))
		{
			const FString Grade = Args.Num() > 1 ? Args[1].ToLower() : FString(TEXT("good"));
			Here->GovernorQuality = Grade == TEXT("cheap") ? 0.3f : Grade == TEXT("great") ? 0.9f : 0.6f;
			Here->GovernorSalary = Grade == TEXT("cheap") ? 2000 : Grade == TEXT("great") ? 25000 : 8000;
			static const TCHAR* Names[] = { TEXT("Ilse Varro"), TEXT("Tomas Quell"), TEXT("Anwen Strake"), TEXT("Duro Pasch"), TEXT("Meret Okan"), TEXT("Sable Imri") };
			Here->Governor = Names[FMath::RandRange(0, 5)];
			FRandomStream R(FMath::Rand());
			Here->Queue = Terr->ProposeQueue(*Here, Body, R);
			Notify(FString::Printf(TEXT("Governor %s hired (%lld cr/week). Proposed queue: %d builds. Edit with governor queue|add|remove|focus|budget."), *Here->Governor, Here->GovernorSalary, Here->Queue.Num()));
			return true;
		}
		if (Sub == TEXT("fire")) { Here->GovernorQuality = 0.f; Here->Governor.Reset(); Here->Queue.Reset(); Notify(TEXT("Governor dismissed: direct mode.")); return true; }
		if (Here->GovernorQuality <= 0.f) { Notify(TEXT("No governor: governor hire cheap|good|great")); return true; }
		if (Sub == TEXT("focus"))
		{
			static const TCHAR* Focuses[] = { TEXT("Balanced"), TEXT("Extraction"), TEXT("Refining"), TEXT("Industry"), TEXT("Trade"), TEXT("Residency"), TEXT("Defense"), TEXT("Research") };
			const FString Want = Args.Num() > 1 ? Args[1] : FString();
			for (const TCHAR* F : Focuses) { if (Want.Equals(F, ESearchCase::IgnoreCase)) { Here->Focus = FName(F); Notify(FString::Printf(TEXT("Focus: %s"), F)); return true; } }
			Notify(TEXT("governor focus Balanced|Extraction|Refining|Industry|Trade|Residency|Defense|Research"));
			return true;
		}
		if (Sub == TEXT("budget")) { Here->WeeklyBudget = Args.Num() > 1 ? FMath::Max<int64>(0, FCString::Atoi64(*Args[1])) : Here->WeeklyBudget; Notify(FString::Printf(TEXT("Weekly build budget %lld cr"), Here->WeeklyBudget)); return true; }
		if (Sub == TEXT("propose")) { FRandomStream R(FMath::Rand()); Here->Queue = Terr->ProposeQueue(*Here, Body, R); }
		else if (Sub == TEXT("add") && Args.Num() > 1) Here->Queue.Add(FName(*Args[1].ToLower()));
		else if (Sub == TEXT("remove") && Args.Num() > 1) { const int32 Idx = FCString::Atoi(*Args[1]) - 1; if (Here->Queue.IsValidIndex(Idx)) Here->Queue.RemoveAt(Idx); }
#if !UE_BUILD_SHIPPING
		else if (Sub == TEXT("week")) { NotifyLines(Terr->RunColonyWeek(World, *Here)); return true; }
#endif
		FString Q;
		for (int32 I = 0; I < Here->Queue.Num(); ++I) Q += FString::Printf(TEXT(" %d.%s(%lld)"), I + 1, *Here->Queue[I].ToString(), UHDTerritorySubsystem::BuildingCost(Here->Queue[I]));
		Notify(FString::Printf(TEXT("%s's queue:%s"), *Here->Governor, Q.IsEmpty() ? TEXT(" empty") : *Q));
		return true;
	}

#if !UE_BUILD_SHIPPING
	if (Verb == TEXT("siegenow"))
	{
		// siegenow: jump the nearest reinforced structure (any owner) straight to its vulnerability window.
		if (!Ship) return true;
		FHDStructure* St = TerrCmdNearestOwn(Terr, Ship, NAME_None, 5.0e7);
		if (!St || St->Siege != EHDSiegeState::Reinforced) { Notify(TEXT("No reinforced structure within 500 km.")); return true; }
		St->SiegeUntilUnix = Now;
		Notify(TEXT("Reinforcement timer expired: vulnerable on the next tick."));
		return true;
	}
#endif
	return true;
}
