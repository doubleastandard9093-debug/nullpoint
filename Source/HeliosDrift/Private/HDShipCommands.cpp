// Milestone 3: hulls, modules, fitting, station storage and hangar (doc 14).
#include "HDPlayerController.h"
#include "HDShipPawn.h"
#include "HDShipTypes.h"
#include "HDCatalog.h"
#include "HDFittingComponent.h"
#include "HDCargoComponent.h"
#include "HDNavComponent.h"
#include "HDPlayerState.h"
#include "HDWorldActors.h"

namespace
{
	/** Standing needed to buy a faction's signature hull (doc 14: standing-gated). */
	constexpr float FactionHullStanding = 3.f;
	/** NPC soft-floor markup on the small Tier 1 modules they stock. */
	constexpr float NpcModuleMarkup = 1.25f;
	/** NPC buy-back for bare baseline hulls. */
	constexpr float HullBuyback = 0.5f;

	bool IsNpcStation(const AHDStationActor* St) { return St && St->OwnerCorp.IsNone(); }
}

bool AHDPlayerController::HandleShipCommand(const FString& Verb, const FString& Arg, AActor* Target)
{
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	AHDPlayerState* PS = GetPlayerState<AHDPlayerState>();
	UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(this);
	if (!Cat) return false;
	UHDFittingComponent* Fit = Ship ? Ship->Fitting.Get() : nullptr;
	UHDCargoComponent* Cargo = Ship ? Ship->Cargo.Get() : nullptr;
	AHDStationActor* Docked = (Ship && Ship->Nav) ? Cast<AHDStationActor>(Ship->Nav->DockedAt) : nullptr;
	TArray<FString> A;
	Arg.ParseIntoArrayWS(A);
	auto RequireDock = [this, Docked]() -> bool
	{
		if (!Docked) Notify(TEXT("Dock at a station first."));
		return Docked != nullptr;
	};

	if (Verb == TEXT("fitting"))
	{
		if (Fit) NotifyLines(Fit->DescribeFit());
		return true;
	}
	if (Verb == TEXT("cargo"))
	{
		if (Cargo) Notify(Cargo->Describe());
		return true;
	}
	if (Verb == TEXT("catalog"))
	{
		// catalog hulls|modules|items [filter]
		const FString Kind = A.Num() > 0 ? A[0] : TEXT("hulls");
		const FString Filter = A.Num() > 1 ? A[1] : FString();
		int32 Shown = 0;
		if (Kind == TEXT("hulls"))
		{
			for (const TPair<FName, FHDHullDef>& P : Cat->GetHulls())
			{
				const FHDHullDef& H = P.Value;
				if (!Filter.IsEmpty() && !H.Id.ToString().Contains(Filter) && !FString(HDShips::ClassName(H.Class)).Contains(Filter)) continue;
				Notify(FString::Printf(TEXT("%s: %s %s %s, %s, tier %d, %d cr%s%s"), *H.Id.ToString(), *H.Name, HDShips::ClassName(H.Class), HDShips::RoleName(H.Role),
					*StaticEnum<EHDSupplyTier>()->GetNameStringByValue((int64)H.Supply), H.Tier, H.Price, H.Faction.IsNone() ? TEXT("") : *(TEXT(" ") + H.Faction.ToString()),
					H.Quirk.IsEmpty() ? TEXT("") : *(TEXT(" (") + H.Quirk + TEXT(")"))));
				if (++Shown >= 40) break;
			}
		}
		else if (Kind == TEXT("modules"))
		{
			for (const TPair<FName, FHDModuleDef>& P : Cat->GetModules())
			{
				const FHDModuleDef& M = P.Value;
				if (!Filter.IsEmpty() && !M.Id.ToString().Contains(Filter)) continue;
				Notify(FString::Printf(TEXT("%s: %s [%s, size %d, T%d] %.0f MW, stability %.0f, %d cr"), *M.Id.ToString(), *M.Name,
					*StaticEnum<EHDSlotType>()->GetNameStringByValue((int64)M.Slot), M.Size, M.Tier, M.Power, M.Stability, M.Price));
				if (++Shown >= 40) break;
			}
		}
		else
		{
			for (const TPair<FName, FHDItemDef>& P : Cat->GetItems())
			{
				const FHDItemDef& I = P.Value;
				if (I.Category == TEXT("Module") || I.Category == TEXT("Hull")) continue;
				if (!Filter.IsEmpty() && !I.Id.ToString().Contains(Filter) && I.Category.ToString() != Filter) continue;
				Notify(FString::Printf(TEXT("%s: %s [%s] %.2f m3, ~%d cr%s"), *I.Id.ToString(), *I.Name, *I.Category.ToString(), I.Volume, I.Price, I.bLegal ? TEXT("") : TEXT(" ILLEGAL")));
				if (++Shown >= 40) break;
			}
		}
		if (Shown == 0) Notify(TEXT("Nothing matches. catalog hulls|modules|items [filter]"));
		return true;
	}

	if (Verb == TEXT("storage"))
	{
		if (!PS) return true;
		int32 N = 0;
		for (const FHDStoredStack& S : PS->Storage)
		{
			if (Docked && S.Station != Docked->ObjectId) continue;
			Notify(FString::Printf(TEXT("%s x%d%s"), *Cat->GetName(S.Item), S.Qty, Docked ? TEXT("") : *FString::Printf(TEXT(" @ %s"), *S.Station.ToString())));
			++N;
		}
		if (N == 0) Notify(Docked ? TEXT("Nothing stored at this station.") : TEXT("Nothing in storage."));
		return true;
	}
	if (Verb == TEXT("store") || Verb == TEXT("retrieve"))
	{
		// store <item> [qty] / retrieve <item> [qty]: cargo <-> station storage
		if (!RequireDock() || !Cargo || !PS || A.Num() < 1) { if (A.Num() < 1) Notify(TEXT("store|retrieve <item> [qty]")); return true; }
		const FName Item(*A[0]);
		const int32 Want = A.Num() > 1 ? FMath::Max(1, FCString::Atoi(*A[1])) : MAX_int32;
		uint8 Flags = 0;
		if (Verb == TEXT("store"))
		{
			const int32 N = Cargo->Remove(Item, FMath::Min(Want, Cargo->Count(Item)), &Flags);
			PS->AddStorage(Docked->ObjectId, Item, N, Flags);
			Notify(FString::Printf(TEXT("Stored %d %s"), N, *Cat->GetName(Item)));
		}
		else
		{
			const int32 Have = PS->StorageCount(Docked->ObjectId, Item);
			const int32 N = FMath::Min3(Want, Have, Cargo->SpaceFor(Item));
			PS->RemoveStorage(Docked->ObjectId, Item, N, &Flags);
			Cargo->Add(Item, N, Flags);
			Notify(FString::Printf(TEXT("Loaded %d %s%s"), N, *Cat->GetName(Item), N < FMath::Min(Want, Have) ? TEXT(" (hold full)") : TEXT("")));
		}
		return true;
	}

	if (Verb == TEXT("fit"))
	{
		// fit <slot> <module>: modules come from station storage here, then the hold. Needs a station with repair or shipyard (doc 14).
		if (!RequireDock() || !Fit || !PS) return true;
		if (A.Num() < 2) { Notify(TEXT("fit <slot> <module id>  (see HDCmd fitting)")); return true; }
		if (!Docked->Info.bRepair && !Docked->Info.bShipyard) { Notify(TEXT("No fitting facilities at this station.")); return true; }
		const int32 Slot = FCString::Atoi(*A[0]);
		const FName Mod(*A[1]);
		const bool bInStorage = PS->StorageCount(Docked->ObjectId, Mod) > 0;
		const bool bInCargo = Cargo && Cargo->Count(Mod) > 0;
		if (!bInStorage && !bInCargo) { Notify(FString::Printf(TEXT("You have no %s here (buy, build or salvage one)."), *Cat->GetName(Mod))); return true; }
		const FName Old = Fit->Modules.IsValidIndex(Slot) ? Fit->Modules[Slot] : NAME_None;
		FString Err;
		if (!Fit->FitModuleAuthority(Slot, Mod, Err)) { Notify(TEXT("Can't fit: ") + Err); return true; }
		if (bInStorage) PS->RemoveStorage(Docked->ObjectId, Mod, 1); else Cargo->Remove(Mod, 1);
		if (!Old.IsNone()) PS->AddStorage(Docked->ObjectId, Old, 1);
		Notify(FString::Printf(TEXT("Fitted %s in slot %d. Power %.0f/%.0f MW, stability %.0f/%.0f"), *Cat->GetName(Mod), Slot,
			Fit->Stats.PowerUsed, Fit->Stats.PowerMax, Fit->Stats.Stability, Fit->Stats.StabilityMax));
		return true;
	}
	if (Verb == TEXT("unfit"))
	{
		if (!RequireDock() || !Fit || !PS || A.Num() < 1) return true;
		const FName Old = Fit->UnfitAuthority(FCString::Atoi(*A[0]));
		if (Old.IsNone()) { Notify(TEXT("Slot empty, or removing it would break power.")); return true; }
		PS->AddStorage(Docked->ObjectId, Old, 1);
		Notify(FString::Printf(TEXT("Removed %s to station storage."), *Cat->GetName(Old)));
		return true;
	}
	if (Verb == TEXT("buymodule"))
	{
		// NPC stations stock small Tier 1 modules at a soft-floor markup; everything bigger is player-made (doc 03/14).
		if (!RequireDock() || !PS || A.Num() < 1) return true;
		const FHDModuleDef* M = Cat->FindModule(FName(*A[0]));
		const int32 Qty = A.Num() > 1 ? FMath::Clamp(FCString::Atoi(*A[1]), 1, 50) : 1;
		if (!M) { Notify(TEXT("Unknown module")); return true; }
		if (!IsNpcStation(Docked) || M->Tier > 1 || M->Size > 2) { Notify(TEXT("Not stocked here: NPCs only sell small Tier 1 modules. Build it or buy from players.")); return true; }
		const int64 Cost = (int64)(M->Price * NpcModuleMarkup) * Qty;
		if (!PS->TrySpend(Cost)) { Notify(FString::Printf(TEXT("Need %lld cr"), Cost)); return true; }
		PS->AddStorage(Docked->ObjectId, M->Id, Qty);
		Notify(FString::Printf(TEXT("Bought %d x %s for %lld cr (in station storage)"), Qty, *M->Name, Cost));
		return true;
	}

	// ---------------- Hulls and hangar ----------------
	if (Verb == TEXT("buyhull"))
	{
		if (!RequireDock() || !PS || A.Num() < 1) return true;
		const FHDHullDef* H = Cat->FindHull(FName(*A[0]));
		if (!H) { Notify(TEXT("Unknown hull (HDCmd catalog hulls)")); return true; }
		if (!IsNpcStation(Docked)) { Notify(TEXT("Player stations sell through their market (M4).")); return true; }
		if (H->Supply == EHDSupplyTier::Advanced) { Notify(TEXT("Player-built only: fabricate it at a shipyard (HDCmd build) or buy from a player.")); return true; }
		if (!H->Faction.IsNone() && HDShips::FactionKey(Docked->Info.Faction) != HDShips::FactionKey(H->Faction)) { Notify(FString::Printf(TEXT("Only %s stations sell the %s."), *H->Faction.ToString(), *H->Name)); return true; }
		if (H->Supply == EHDSupplyTier::Faction && PS->GetStanding(H->Faction) < FactionHullStanding)
		{
			Notify(FString::Printf(TEXT("Needs standing %.0f with %s (you: %.1f)."), FactionHullStanding, *H->Faction.ToString(), PS->GetStanding(H->Faction)));
			return true;
		}
		if (H->Supply == EHDSupplyTier::Baseline && H->Class >= EHDHullClass::Cruiser && !Docked->Info.bShipyard) { Notify(TEXT("Cruisers and up need a shipyard station.")); return true; }
		if (!PS->TrySpend(H->Price)) { Notify(FString::Printf(TEXT("Need %d cr"), H->Price)); return true; }
		FHDStoredShip S;
		S.Station = Docked->ObjectId;
		S.Hull = H->Id;
		PS->Hangar.Add(S);
		Notify(FString::Printf(TEXT("Bought a bare %s for %d cr. It's in your hangar here: HDCmd board %d"), *H->Name, H->Price, PS->Hangar.Num() - 1));
		return true;
	}
	if (Verb == TEXT("hangar"))
	{
		if (!PS) return true;
		if (PS->Hangar.Num() == 0) Notify(TEXT("Hangar empty."));
		for (int32 I = 0; I < PS->Hangar.Num(); ++I)
		{
			const FHDStoredShip& S = PS->Hangar[I];
			const FHDHullDef* H = Cat->FindHull(S.Hull);
			int32 Fitted = 0;
			for (const FName& M : S.Modules) { if (!M.IsNone()) ++Fitted; }
			Notify(FString::Printf(TEXT("[%d] %s @ %s, %d modules%s"), I, H ? *H->Name : *S.Hull.ToString(), *S.Station.ToString(), Fitted, S.bRecovered ? TEXT(" [recovered]") : TEXT("")));
		}
		return true;
	}
	if (Verb == TEXT("board"))
	{
		// Swap ships: the current ship (and its fit) is parked here, cargo goes to station storage.
		if (!RequireDock() || !PS || !Fit || A.Num() < 1) return true;
		const int32 Index = FCString::Atoi(*A[0]);
		if (!PS->Hangar.IsValidIndex(Index) || PS->Hangar[Index].Station != Docked->ObjectId) { Notify(TEXT("No such ship in this station's hangar.")); return true; }
		const FHDStoredShip Next = PS->Hangar[Index];
		FString Err;
		FHDFitStats Tmp;
		if (!UHDFittingComponent::ComputeStats(Cat, Next.Hull, Next.Modules, Tmp, Err)) { Notify(TEXT("That ship's fit is invalid: ") + Err); return true; }
		FHDStoredShip Cur;
		Cur.Station = Docked->ObjectId;
		Cur.Hull = Fit->HullId;
		Cur.Modules = Fit->Modules;
		Cur.bRecovered = Fit->bRecovered;
		if (Cargo)
		{
			for (const FHDItemStack& I : Cargo->Items) PS->AddStorage(Docked->ObjectId, I.Item, I.Qty, I.Flags);
			Cargo->Items.Reset();
		}
		PS->Hangar.RemoveAt(Index);
		PS->Hangar.Add(Cur);
		Fit->SetFitAuthority(Next.Hull, Next.Modules, Err);
		Fit->bRecovered = Next.bRecovered;
		Ship->HullHP = Ship->MaxHullHP;
		if (Ship->Nav) Ship->Nav->ShipFuel = Ship->Nav->MaxShipFuel;
		Notify(FString::Printf(TEXT("Boarded the %s. Your previous ship is in the hangar; cargo moved to storage."), Fit->GetHullDef() ? *Fit->GetHullDef()->Name : TEXT("ship")));
		return true;
	}
	if (Verb == TEXT("sellhull"))
	{
		if (!RequireDock() || !PS || A.Num() < 1) return true;
		const int32 Index = FCString::Atoi(*A[0]);
		if (!PS->Hangar.IsValidIndex(Index) || PS->Hangar[Index].Station != Docked->ObjectId) { Notify(TEXT("No such ship here.")); return true; }
		const FHDHullDef* H = Cat->FindHull(PS->Hangar[Index].Hull);
		if (!H) return true;
		if (H->Supply == EHDSupplyTier::Faction) { Notify(TEXT("Faction hulls can't be sold. Gift it, fly it, or lose it.")); return true; }
		if (!IsNpcStation(Docked)) { Notify(TEXT("Sell to players through a market order (M4).")); return true; }
		const int64 Value = (int64)(H->Price * HullBuyback * (PS->Hangar[Index].bRecovered ? 0.6f : 1.f));
		for (const FName& M : PS->Hangar[Index].Modules) { if (!M.IsNone()) PS->AddStorage(Docked->ObjectId, M, 1); }
		PS->Hangar.RemoveAt(Index);
		PS->AddCredits(Value);
		Notify(FString::Printf(TEXT("Sold the %s hull for %lld cr; its modules went to storage."), *H->Name, Value));
		return true;
	}

#if !UE_BUILD_SHIPPING
	if (Verb == TEXT("give"))
	{
		// give <item|hull_id> [qty]: test helper. Items go to the hold, or station storage when docked.
		if (!PS || A.Num() < 1) return true;
		const FName Item(*A[0]);
		const int32 Qty = A.Num() > 1 ? FMath::Max(1, FCString::Atoi(*A[1])) : 1;
		if (const FHDHullDef* H = Cat->FindHull(Item))
		{
			if (!RequireDock()) return true;
			FHDStoredShip S;
			S.Station = Docked->ObjectId;
			S.Hull = H->Id;
			S.Modules = UHDFittingComponent::SuggestFit(Cat, H->Id);
			PS->Hangar.Add(S);
			Notify(FString::Printf(TEXT("Hangar +%s (fitted). HDCmd board %d"), *H->Name, PS->Hangar.Num() - 1));
			return true;
		}
		if (!Cat->FindItem(Item)) { Notify(TEXT("Unknown item")); return true; }
		if (Docked) { PS->AddStorage(Docked->ObjectId, Item, Qty); Notify(FString::Printf(TEXT("Storage +%d %s"), Qty, *Cat->GetName(Item))); }
		else if (Cargo) { Notify(FString::Printf(TEXT("Hold +%d %s"), Cargo->Add(Item, Qty), *Cat->GetName(Item))); }
		return true;
	}
	if (Verb == TEXT("credits"))
	{
		if (PS) { PS->AddCredits(A.Num() > 0 ? FCString::Atoi64(*A[0]) : 1000000); Notify(FString::Printf(TEXT("Credits: %lld"), PS->Credits)); }
		return true;
	}
	if (Verb == TEXT("standing"))
	{
		// standing <faction> <value>
		if (PS && A.Num() >= 2) { PS->AddStanding(FName(*A[0]), FCString::Atof(*A[1]) - PS->GetStanding(FName(*A[0]))); Notify(FString::Printf(TEXT("Standing %s = %.1f"), *A[0], PS->GetStanding(FName(*A[0])))); }
		return true;
	}
	if (Verb == TEXT("reloadcatalog"))
	{
		Cat->Load();
		Notify(TEXT("Catalog reloaded"));
		return true;
	}
#endif
	return false;
}
