// Milestone 4: markets, trading, industry, fuel hauling, wrecks and salvage (doc 03, 16, 19).
#include "HDPlayerController.h"
#include "HDShipPawn.h"
#include "HDShipTypes.h"
#include "HDCatalog.h"
#include "HDFittingComponent.h"
#include "HDCargoComponent.h"
#include "HDNavComponent.h"
#include "HDPlayerState.h"
#include "HDWorldActors.h"
#include "HDWreckActor.h"
#include "HDEconomy.h"
#include "HDUniverseSubsystem.h"
#include "EngineUtils.h"

namespace
{
	/** Wreck the pilot means: their target, else the nearest one within reach. */
	AHDWreckActor* FindWreck(AHDShipPawn* Ship, AActor* Target, double MaxDist)
	{
		if (!Ship) return nullptr;
		if (AHDWreckActor* W = Cast<AHDWreckActor>(Target))
		{
			if (FVector::Dist(W->GetActorLocation(), Ship->GetActorLocation()) <= MaxDist) return W;
		}
		AHDWreckActor* Best = nullptr;
		double BestD = MaxDist;
		for (TActorIterator<AHDWreckActor> It(Ship->GetWorld()); It; ++It)
		{
			if (It->SystemIndex != Ship->SystemIndex || It->IsHidden()) continue;
			const double D = FVector::Dist(It->GetActorLocation(), Ship->GetActorLocation());
			if (D <= BestD) { BestD = D; Best = *It; }
		}
		return Best;
	}

	bool HasFacility(const AHDStationActor* St, const FName& Facility)
	{
		if (!St) return false;
		if (Facility == FName(TEXT("refinery"))) return St->Info.bRefinery;
		if (Facility == FName(TEXT("shipyard"))) return St->Info.bShipyard;
		// Factories: industrial and corporate stations, or any shipyard.
		return St->Info.Family == EHDStationFamily::FactionIndustrial || St->Info.Family == EHDStationFamily::CorporateModular || St->Info.bShipyard;
	}

	int32 ParseQty(const TArray<FString>& A, int32 Index, int32 Default)
	{
		return A.IsValidIndex(Index) ? FMath::Max(1, FCString::Atoi(*A[Index])) : Default;
	}
}

bool AHDPlayerController::HandleEconomyCommand(const FString& Verb, const FString& Arg, AActor* Target)
{
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	AHDPlayerState* PS = GetPlayerState<AHDPlayerState>();
	UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(this);
	UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(this);
	if (!Cat || !Econ || !PS) return false;
	UHDCargoComponent* Cargo = Ship ? Ship->Cargo.Get() : nullptr;
	AHDStationActor* Docked = (Ship && Ship->Nav) ? Cast<AHDStationActor>(Ship->Nav->DockedAt) : nullptr;
	TArray<FString> A;
	Arg.ParseIntoArrayWS(A);
	auto RequireDock = [this, Docked]() -> bool
	{
		if (!Docked) Notify(TEXT("Dock at a station first."));
		return Docked != nullptr;
	};
	auto RequireItem = [this, Cat](const TArray<FString>& Args, int32 Index, FName& Out) -> bool
	{
		Out = Args.IsValidIndex(Index) ? FName(*Args[Index]) : NAME_None;
		if (Out.IsNone() || !Cat->FindItem(Out)) { Notify(TEXT("Unknown item. Try: catalog items <filter>")); return false; }
		return true;
	};
	UWorld* World = GetWorld();

	// ---------------- Markets ----------------
	if (Verb == TEXT("market"))
	{
		// market [filter]: NPC quotes and player orders at this station.
		if (!RequireDock()) return true;
		if (!Docked->Info.bMarket) { Notify(TEXT("No market here.")); return true; }
		const FString Filter = A.Num() > 0 ? A[0] : FString();
		Notify(FString::Printf(TEXT("%s market (tax %.0f%%, broker %.0f%%):"), *Docked->DisplayName, Econ->SalesTax(Docked) * 100.f, Econ->BrokerFee * 100.f));
		int32 Shown = 0;
		for (const TPair<FName, FHDItemDef>& P : Cat->GetItems())
		{
			const FHDItemDef& D = P.Value;
			if (!Filter.IsEmpty() && !D.Id.ToString().Contains(Filter) && !D.Category.ToString().Contains(Filter)) continue;
			const FHDQuote Q = Econ->Quote(Docked, D.Id);
			int32 BestSell = 0, BestBuy = 0;
			for (const FHDMarketOrder& O : Econ->Orders)
			{
				if (O.Station != Docked->ObjectId || O.Item != D.Id) continue;
				if (O.bBuy) BestBuy = FMath::Max(BestBuy, O.Price);
				else BestSell = BestSell == 0 ? O.Price : FMath::Min(BestSell, O.Price);
			}
			if (Q.NpcSells == 0 && Q.NpcBuys == 0 && BestSell == 0 && BestBuy == 0) continue;
			Notify(FString::Printf(TEXT("%s: NPC sells %s / buys %s | orders sell %s / buy %s"), *D.Id.ToString(),
				Q.NpcSells ? *FString::FromInt(Q.NpcSells) : TEXT("-"), Q.NpcBuys ? *FString::FromInt(Q.NpcBuys) : TEXT("-"),
				BestSell ? *FString::FromInt(BestSell) : TEXT("-"), BestBuy ? *FString::FromInt(BestBuy) : TEXT("-")));
			if (++Shown >= 40) { Notify(TEXT("(more: add a filter)")); break; }
		}
		return true;
	}
	if (Verb == TEXT("buy"))
	{
		// buy <item> [qty] [maxprice]: fills the cheapest player sell orders here first, then the NPC.
		if (!RequireDock() || !Cargo) return true;
		FName Item;
		if (!RequireItem(A, 0, Item)) return true;
		int32 Want = ParseQty(A, 1, 1);
		const int32 MaxPrice = A.IsValidIndex(2) ? FCString::Atoi(*A[2]) : MAX_int32;
		const FHDQuote Q = Econ->Quote(Docked, Item);
		const float Tax = Econ->SalesTax(Docked);
		int32 Got = 0;
		int64 Spent = 0;
		bool bToStorage = false;
		auto Receive = [&](int32 N, uint8 Flags)
		{
			const int32 InHold = FMath::Min(N, Cargo->SpaceFor(Item));
			if (InHold > 0) Cargo->Add(Item, InHold, Flags);
			if (N > InHold) { PS->AddStorage(Docked->ObjectId, Item, N - InHold, Flags); bToStorage = true; }
		};
		while (Want > 0)
		{
			int32 Best = INDEX_NONE;
			for (int32 I = 0; I < Econ->Orders.Num(); ++I)
			{
				const FHDMarketOrder& O = Econ->Orders[I];
				if (O.bBuy || O.Station != Docked->ObjectId || O.Item != Item || O.Price > MaxPrice || O.Owner == PS->PilotId) continue;
				if (Q.NpcSells > 0 && O.Price >= Q.NpcSells) continue;
				if (Best == INDEX_NONE || O.Price < Econ->Orders[Best].Price) Best = I;
			}
			if (Best == INDEX_NONE) break;
			FHDMarketOrder& O = Econ->Orders[Best];
			const int32 N = FMath::Min(Want, O.Qty);
			const int64 Cost = (int64)O.Price * N;
			if (!PS->TrySpend(Cost)) break;
			Receive(N, O.Flags);
			Econ->PayPilot(World, O.Owner, (int64)FMath::FloorToDouble(Cost * (1.0 - Tax)));
			O.Qty -= N; Want -= N; Got += N; Spent += Cost;
			if (O.Qty <= 0) Econ->Orders.RemoveAt(Best);
		}
		if (Want > 0 && Q.NpcSells > 0 && Q.NpcSells <= MaxPrice)
		{
			const int32 Afford = (int32)FMath::Min<int64>(Want, PS->Credits / FMath::Max(1, Q.NpcSells));
			if (Afford > 0 && PS->TrySpend((int64)Afford * Q.NpcSells))
			{
				Receive(Afford, Q.bStolenGoods ? HDItemFlags::Stolen : 0);
				Econ->NpcStockDelta(Docked, Item, -Afford);
				Got += Afford; Spent += (int64)Afford * Q.NpcSells; Want -= Afford;
			}
		}
		if (Got == 0) { Notify(Q.NpcSells == 0 ? TEXT("Nobody sells that here at that price. Place a buy order: order buy <item> <qty> <price>") : TEXT("Can't afford it.")); return true; }
		Notify(FString::Printf(TEXT("Bought %d x %s for %lld cr%s%s"), Got, *Cat->GetName(Item), Spent, bToStorage ? TEXT(" (overflow to station storage)") : TEXT(""),
			Q.bStolenGoods ? TEXT(" - stolen goods: carries a legal mark in lawful space") : TEXT("")));
		return true;
	}
	if (Verb == TEXT("sell"))
	{
		// sell <item> [qty|all]: from the hold, best player buy orders first, then the NPC.
		if (!RequireDock() || !Cargo) return true;
		FName Item;
		if (!RequireItem(A, 0, Item)) return true;
		const int32 Have = Cargo->Count(Item);
		int32 Want = A.IsValidIndex(1) && A[1] == TEXT("all") ? Have : FMath::Min(Have, ParseQty(A, 1, 1));
		if (Want <= 0) { Notify(TEXT("You don't carry that.")); return true; }
		const FHDQuote Q = Econ->Quote(Docked, Item);
		const float Tax = Econ->SalesTax(Docked);
		int32 Sold = 0;
		int64 Earned = 0;
		while (Want > 0)
		{
			int32 Best = INDEX_NONE;
			for (int32 I = 0; I < Econ->Orders.Num(); ++I)
			{
				const FHDMarketOrder& O = Econ->Orders[I];
				if (!O.bBuy || O.Station != Docked->ObjectId || O.Item != Item || O.Owner == PS->PilotId || O.Price <= Q.NpcBuys) continue;
				if (Best == INDEX_NONE || O.Price > Econ->Orders[Best].Price) Best = I;
			}
			if (Best == INDEX_NONE) break;
			FHDMarketOrder& O = Econ->Orders[Best];
			const int32 N = FMath::Min(Want, O.Qty);
			uint8 Flags = 0;
			Cargo->Remove(Item, N, &Flags);
			Econ->DeliverToStorage(World, O.Owner, O.Station, Item, N, Flags); // buy-order credits were escrowed
			const int64 Gross = (int64)O.Price * N;
			const int64 Net = (int64)FMath::FloorToDouble(Gross * (1.0 - Tax));
			PS->AddCredits(Net);
			O.Qty -= N; Want -= N; Sold += N; Earned += Net;
			if (O.Qty <= 0) Econ->Orders.RemoveAt(Best);
		}
		if (Want > 0 && Q.NpcBuys > 0)
		{
			Cargo->Remove(Item, Want);
			const int64 Net = (int64)FMath::FloorToDouble((double)Q.NpcBuys * Want * (1.0 - Tax));
			PS->AddCredits(Net);
			Econ->NpcStockDelta(Docked, Item, Want);
			Sold += Want; Earned += Net;
			// Accord stations pay bounty tags and give standing (doc 18).
			if (Docked->Info.Family == EHDStationFamily::AccordCivic && Item == FName(TEXT("pirate_tags"))) PS->AddStanding(TEXT("accord"), 0.02f * Want);
			Want = 0;
		}
		if (Sold == 0) { Notify(TEXT("Nobody buys that here. Place a sell order: order sell <item> <qty> <price>")); return true; }
		Notify(FString::Printf(TEXT("Sold %d x %s for %lld cr (after tax)"), Sold, *Cat->GetName(Item), Earned));
		return true;
	}
	if (Verb == TEXT("order"))
	{
		// order buy|sell <item> <qty> <price>. Sell orders escrow goods (hold, then storage); buy orders escrow credits.
		if (!RequireDock()) return true;
		if (A.Num() < 4 || (A[0] != TEXT("buy") && A[0] != TEXT("sell"))) { Notify(TEXT("order buy|sell <item> <qty> <price>")); return true; }
		FName Item;
		if (!RequireItem(A, 1, Item)) return true;
		const bool bBuy = A[0] == TEXT("buy");
		const int32 Qty = ParseQty(A, 2, 1);
		const int32 Price = FMath::Max(1, FCString::Atoi(*A[3]));
		const int64 Fee = FMath::Max<int64>(1, (int64)FMath::CeilToDouble((double)Qty * Price * Econ->BrokerFee));
		FHDMarketOrder O;
		O.Station = Docked->ObjectId;
		O.System = Docked->SystemIndex;
		if (UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this)) O.Region = U->GetSystem(Docked->SystemIndex).Region;
		O.Owner = PS->PilotId;
		O.Item = Item;
		O.Qty = Qty;
		O.Price = Price;
		O.bBuy = bBuy;
		if (bBuy)
		{
			if (!PS->TrySpend((int64)Qty * Price + Fee)) { Notify(TEXT("Not enough credits for escrow + broker fee.")); return true; }
		}
		else
		{
			const int32 InHold = Cargo ? Cargo->Count(Item) : 0;
			const int32 InStore = PS->StorageCount(Docked->ObjectId, Item);
			if (InHold + InStore < Qty) { Notify(TEXT("You don't have that many here (hold + station storage).")); return true; }
			if (!PS->TrySpend(Fee)) { Notify(TEXT("Can't pay the broker fee.")); return true; }
			int32 Left = Qty;
			uint8 Flags = 0;
			if (Cargo && InHold > 0) { const int32 N = FMath::Min(Left, InHold); Cargo->Remove(Item, N, &Flags); Left -= N; }
			if (Left > 0) { uint8 F2 = 0; PS->RemoveStorage(Docked->ObjectId, Item, Left, &F2); Flags |= F2; }
			O.Flags = Flags;
		}
		const int32 Id = Econ->PlaceOrder(O);
		Notify(FString::Printf(TEXT("Order #%d: %s %d x %s at %d cr (fee %lld cr). Fills only at %s."), Id, bBuy ? TEXT("buy") : TEXT("sell"), Qty, *Cat->GetName(Item), Price, Fee, *Docked->DisplayName));
		return true;
	}
	if (Verb == TEXT("orders"))
	{
		int32 N = 0;
		for (const FHDMarketOrder& O : Econ->Orders)
		{
			if (O.Owner != PS->PilotId) continue;
			Notify(FString::Printf(TEXT("#%d %s %d x %s @ %d at %s"), O.Id, O.bBuy ? TEXT("BUY") : TEXT("SELL"), O.Qty, *O.Item.ToString(), O.Price, *O.Station.ToString()));
			++N;
		}
		if (N == 0) Notify(TEXT("No open orders."));
		return true;
	}
	if (Verb == TEXT("cancelorder"))
	{
		const int32 Id = A.Num() > 0 ? FCString::Atoi(*A[0]) : -1;
		const int32 Idx = Econ->Orders.IndexOfByPredicate([&](const FHDMarketOrder& O) { return O.Id == Id && O.Owner == PS->PilotId; });
		if (Idx == INDEX_NONE) { Notify(TEXT("No such order of yours.")); return true; }
		const FHDMarketOrder O = Econ->Orders[Idx];
		Econ->Orders.RemoveAt(Idx);
		// Refunds: credits come back anywhere, goods go back to that station's storage (no fee refund).
		if (O.bBuy) PS->AddCredits((int64)O.Qty * O.Price);
		else PS->AddStorage(O.Station, O.Item, O.Qty, O.Flags);
		Notify(FString::Printf(TEXT("Order #%d cancelled; %s"), Id, O.bBuy ? TEXT("escrow refunded") : *FString::Printf(TEXT("goods returned to storage at %s"), *O.Station.ToString())));
		return true;
	}
	if (Verb == TEXT("regionorders"))
	{
		// regionorders <item>: orders are visible region-wide, but fill only at their station.
		FName Item;
		if (!RequireItem(A, 0, Item) || !Ship) return true;
		UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
		const int32 Region = (U && Ship->SystemIndex >= 0) ? U->GetSystem(Ship->SystemIndex).Region : -1;
		int32 N = 0;
		for (const FHDMarketOrder& O : Econ->Orders)
		{
			if (O.Item != Item || O.Region != Region) continue;
			Notify(FString::Printf(TEXT("%s %d @ %d at %s (system %d)"), O.bBuy ? TEXT("BUY") : TEXT("SELL"), O.Qty, O.Price, *O.Station.ToString(), O.System));
			if (++N >= 30) break;
		}
		if (N == 0) Notify(TEXT("No orders for that in this region."));
		return true;
	}

	// ---------------- Industry ----------------
	if (Verb == TEXT("recipes"))
	{
		const FString Filter = A.Num() > 0 ? A[0] : FString();
		int32 Shown = 0;
		for (const TPair<FName, FHDRecipeDef>& P : Cat->GetRecipes())
		{
			const FHDRecipeDef& R = P.Value;
			if (!Filter.IsEmpty() && !R.Id.ToString().Contains(Filter) && !R.Output.ToString().Contains(Filter) && !R.Facility.ToString().Contains(Filter)) continue;
			TArray<FString> In;
			for (const TPair<FName, int32>& I : R.Inputs) In.Add(FString::Printf(TEXT("%d %s"), I.Value, *I.Key.ToString()));
			Notify(FString::Printf(TEXT("%s [%s, %.0fs]: %s -> %d %s"), *R.Id.ToString(), *R.Facility.ToString(), R.Seconds, *FString::Join(In, TEXT(", ")), R.Qty, *R.Output.ToString()));
			if (++Shown >= 40) { Notify(TEXT("(more: add a filter)")); break; }
		}
		return true;
	}
	if (Verb == TEXT("refine") || Verb == TEXT("build"))
	{
		// refine|build <recipe or output> [runs]: inputs from station storage, then the hold.
		if (!RequireDock()) return true;
		const FName Key = A.Num() > 0 ? FName(*A[0]) : NAME_None;
		const FHDRecipeDef* R = Cat->FindRecipe(Key);
		if (!R) R = Cat->FindRecipeFor(Key);
		if (!R) { Notify(TEXT("No such recipe. Try: recipes <filter>")); return true; }
		if (!HasFacility(Docked, R->Facility)) { Notify(FString::Printf(TEXT("Needs a %s; this station doesn't have one."), *R->Facility.ToString())); return true; }
		const int32 Runs = FMath::Clamp(ParseQty(A, 1, 1), 1, 100);
		for (const TPair<FName, int32>& I : R->Inputs)
		{
			const int32 Have = PS->StorageCount(Docked->ObjectId, I.Key) + (Cargo ? Cargo->Count(I.Key) : 0);
			if (Have < I.Value * Runs) { Notify(FString::Printf(TEXT("Missing %d x %s"), I.Value * Runs - Have, *Cat->GetName(I.Key))); return true; }
		}
		for (const TPair<FName, int32>& I : R->Inputs)
		{
			int32 Need = I.Value * Runs;
			Need -= PS->RemoveStorage(Docked->ObjectId, I.Key, Need);
			if (Need > 0 && Cargo) Cargo->Remove(I.Key, Need);
		}
		FHDJob J;
		J.Owner = PS->PilotId;
		J.Station = Docked->ObjectId;
		J.Recipe = R->Id;
		J.Runs = Runs;
		J.FinishUnix = AHDPlayerState::NowUnix() + R->Seconds * Runs;
		Econ->StartJob(J);
		Notify(FString::Printf(TEXT("Job started: %d x %s, ready in %.0f s (output to storage here)"), R->Qty * Runs, *Cat->GetName(R->Output), R->Seconds * Runs));
		return true;
	}
	if (Verb == TEXT("jobs"))
	{
		const double Now = AHDPlayerState::NowUnix();
		int32 N = 0;
		for (const FHDJob& J : Econ->Jobs)
		{
			if (J.Owner != PS->PilotId) continue;
			Notify(FString::Printf(TEXT("%s x%d at %s: %.0f s left"), *J.Recipe.ToString(), J.Runs, *J.Station.ToString(), FMath::Max(0.0, J.FinishUnix - Now)));
			++N;
		}
		if (N == 0) Notify(TEXT("No running jobs."));
		return true;
	}

	// ---------------- Fuel hauling ----------------
	if (Verb == TEXT("buyfuel"))
	{
		// buyfuel <t>: jump fuel as cargo, to haul to stations or capital fleets (doc 19 fuel economy).
		if (!RequireDock() || !Cargo) return true;
		const int32 Price = Econ->JumpFuelPrice(Docked);
		if (Price <= 0) { Notify(TEXT("Jump fuel is sold at gas-giant fuel platforms.")); return true; }
		int32 T = FMath::Min(ParseQty(A, 0, 10), FMath::FloorToInt(Docked->FuelStock));
		T = FMath::Min(T, Cargo->SpaceFor(TEXT("jump_fuel")));
		if (T <= 0) { Notify(TEXT("No room in the hold or the platform is dry.")); return true; }
		if (!PS->TrySpend((int64)T * Price)) { Notify(TEXT("Can't afford it.")); return true; }
		Cargo->Add(TEXT("jump_fuel"), T);
		Docked->FuelStock -= T;
		Notify(FString::Printf(TEXT("Loaded %d t jump fuel cargo at %d cr/t"), T, Price));
		return true;
	}
	if (Verb == TEXT("loadfuel"))
	{
		// loadfuel [t]: move jump fuel cargo into the ship's jump tank.
		if (!Cargo || !Ship || !Ship->Nav) return true;
		const float Room = FMath::Max(0.f, Ship->Nav->MaxJumpFuel - Ship->Nav->JumpFuel);
		if (Room <= 0.f) { Notify(Ship->Nav->MaxJumpFuel <= 0.f ? TEXT("No jump fuel tank fitted.") : TEXT("Jump tank full.")); return true; }
		const int32 T = FMath::Min3(ParseQty(A, 0, MAX_int32), Cargo->Count(TEXT("jump_fuel")), FMath::FloorToInt(Room));
		if (T <= 0) { Notify(TEXT("No jump fuel in the hold.")); return true; }
		Cargo->Remove(TEXT("jump_fuel"), T);
		Ship->Nav->JumpFuel += T;
		Notify(FString::Printf(TEXT("Jump tank +%d t (%.0f / %.0f)"), T, Ship->Nav->JumpFuel, Ship->Nav->MaxJumpFuel));
		return true;
	}
	if (Verb == TEXT("fuelprice"))
	{
		if (!RequireDock()) return true;
		Notify(FString::Printf(TEXT("Ship fuel %d cr/t, jump fuel %s, platform stock %.0f t"), Econ->ShipFuelPrice(Docked),
			Econ->JumpFuelPrice(Docked) > 0 ? *FString::Printf(TEXT("%d cr/t"), Econ->JumpFuelPrice(Docked)) : TEXT("not sold"), Docked->FuelStock));
		return true;
	}

	// ---------------- Wrecks and salvage ----------------
	if (Verb == TEXT("salvageinfo"))
	{
		AHDWreckActor* W = FindWreck(Ship, Target, 5.0e7);
		if (!W) { Notify(TEXT("No wreck within 500 km.")); return true; }
		const int64 Rebuild = W->RebuildCost();
		Notify(FString::Printf(TEXT("%s: %s, %.0f%% through pass, %d loot stacks, %s"), *W->DisplayName, *W->GetKindLabel(), W->PassProgress * 100.f, W->Loot.Num(),
			Rebuild >= 0 ? *FString::Printf(TEXT("rebuild %lld cr"), Rebuild) : TEXT("not rebuildable")));
		if (W->HulkClass > 0 && W->Flag == EHDHulkFlag::None)
		{
			TArray<FString> Allowed;
			TArray<EHDHulkFlag> Flags;
			Flags.Add(EHDHulkFlag::Solo);
			Flags.Add(EHDHulkFlag::Open);
			Flags.Add(EHDHulkFlag::Corp);
			for (EHDHulkFlag F : Flags) { if (W->IsFlagAllowed(F)) Allowed.Add(AHDWreckActor::FlagName(F)); }
			Notify(FString::Printf(TEXT("Unbreached. Allowed flags: %s"), *FString::Join(Allowed, TEXT(", "))));
		}
		return true;
	}
	if (Verb == TEXT("breach"))
	{
		AHDWreckActor* W = FindWreck(Ship, Target, 300000.0);
		if (!W) { Notify(TEXT("Get within 3 km of a hulk.")); return true; }
		const FString F = A.Num() > 0 ? A[0].ToLower() : FString();
		const EHDHulkFlag Flag = F == TEXT("solo") ? EHDHulkFlag::Solo : F == TEXT("open") ? EHDHulkFlag::Open : F == TEXT("corp") ? EHDHulkFlag::Corp : EHDHulkFlag::None;
		if (Flag == EHDHulkFlag::None) { Notify(TEXT("breach solo|open|corp")); return true; }
		FString Why;
		if (!W->Breach(Ship, Flag, Why)) Notify(Why);
		return true;
	}
	if (Verb == TEXT("loot"))
	{
		AHDWreckActor* W = FindWreck(Ship, Target, 300000.0);
		if (!W || !Cargo) { Notify(TEXT("Get within 3 km of a wreck.")); return true; }
		FString Why;
		if (W->HulkClass > 0 && !W->CanWork(Ship, Why)) { Notify(Why); return true; }
		if (W->Loot.Num() == 0) { Notify(TEXT("Nothing loose to take; salvage it for modules and materials.")); return true; }
		const bool bTheft = !W->VictimPilot.IsEmpty() && W->VictimPilot != PS->PilotId;
		int32 Taken = 0;
		for (int32 I = W->Loot.Num() - 1; I >= 0; --I)
		{
			FHDItemStack& S = W->Loot[I];
			const int32 N = Cargo->Add(S.Item, S.Qty, S.Flags);
			Taken += N;
			S.Qty -= N;
			if (S.Qty <= 0) W->Loot.RemoveAt(I);
		}
		Notify(FString::Printf(TEXT("Looted %d units%s"), Taken, W->Loot.Num() ? TEXT(" (hold full, some left)") : TEXT("")));
		if (bTheft && Taken > 0) OnWreckLooted(W);
		return true;
	}
	if (Verb == TEXT("tow"))
	{
		if (!Ship || !Ship->Fitting) return true;
		const float TowCap = Ship->Fitting->Stats.Tow * 2.f;
		if (TowCap <= 0.f) { Notify(TEXT("Fit a tractor beam to tow.")); return true; }
		for (TActorIterator<AHDWreckActor> It(GetWorld()); It; ++It) { if (It->TowedBy.Get() == Ship) { Notify(TEXT("Already towing. release first.")); return true; } }
		AHDWreckActor* W = FindWreck(Ship, Target, 300000.0);
		if (!W) { Notify(TEXT("Get within 3 km of a wreck.")); return true; }
		FString Why;
		if (W->HulkClass > 0 && !W->CanWork(Ship, Why)) { Notify(Why); return true; }
		if (W->RebuildCost() < 0) { Notify(TEXT("Too broken to be worth towing (G-E or station derelict).")); return true; }
		if (W->SizeFactor() > TowCap) { Notify(FString::Printf(TEXT("Too big for your tractor (size %d, max %.0f)."), W->SizeFactor(), TowCap)); return true; }
		if (W->TowedBy.IsValid()) { Notify(TEXT("Someone else is towing it.")); return true; }
		W->TowedBy = Ship;
		W->SetActorEnableCollision(false);
		Notify(FString::Printf(TEXT("Towing %s. Take it to a shipyard: rebuild"), *W->DisplayName));
		return true;
	}
	if (Verb == TEXT("release"))
	{
		for (TActorIterator<AHDWreckActor> It(GetWorld()); It; ++It)
		{
			if (It->TowedBy.Get() != Ship) continue;
			It->TowedBy = nullptr;
			It->SetActorEnableCollision(true);
			Notify(TEXT("Tow released."));
			return true;
		}
		Notify(TEXT("Not towing anything."));
		return true;
	}
	if (Verb == TEXT("rebuild"))
	{
		// rebuild: at a shipyard with a towed wreck. Comes back as a bare-ish ship with a "recovered" tag.
		if (!RequireDock()) return true;
		if (!Docked->Info.bShipyard) { Notify(TEXT("Rebuilds need a shipyard.")); return true; }
		AHDWreckActor* W = nullptr;
		for (TActorIterator<AHDWreckActor> It(GetWorld()); It; ++It) { if (It->TowedBy.Get() == Ship) { W = *It; break; } }
		if (!W) { Notify(TEXT("Tow a wreck here first.")); return true; }
		const int64 Cost = W->RebuildCost();
		if (Cost < 0) { Notify(TEXT("Can't be rebuilt.")); return true; }
		if (!PS->TrySpend(Cost)) { Notify(FString::Printf(TEXT("Rebuild costs %lld cr."), Cost)); return true; }
		FHDStoredShip S;
		S.Station = Docked->ObjectId;
		S.Hull = W->HullId;
		S.Modules = W->Modules; // surviving modules stay fitted where valid; the fitting check runs on board
		S.bRecovered = true;
		PS->Hangar.Add(S);
		Notify(FString::Printf(TEXT("Rebuilt %s for %lld cr (recovered). It's in your hangar: hangar / board <n>"), *Cat->GetName(FName(*(TEXT("hull_") + W->HullId.ToString()))), Cost));
		W->Destroy();
		return true;
	}

#if !UE_BUILD_SHIPPING
	if (Verb == TEXT("spawnwreck"))
	{
		// spawnwreck [hull] [grade 0-4] [class 0-4]: test wreck 2 km ahead.
		if (!Ship) return true;
		const FName Hull = A.Num() > 0 ? FName(*A[0]) : FName(TEXT("lance"));
		if (!Cat->FindHull(Hull)) { Notify(TEXT("Unknown hull.")); return true; }
		const int32 Grade = A.IsValidIndex(1) ? FMath::Clamp(FCString::Atoi(*A[1]), 0, 4) : 0;
		const int32 Class = A.IsValidIndex(2) ? FMath::Clamp(FCString::Atoi(*A[2]), 0, 4) : 0;
		TArray<FName> Mods = UHDFittingComponent::SuggestFit(Cat, Hull, true);
		Mods.RemoveAll([](const FName& M) { return M.IsNone(); });
		AHDWreckActor* W = UHDEconomySubsystem::SpawnWreck(GetWorld(), Ship->SystemIndex, Ship->GetActorLocation() + Ship->GetActorForwardVector() * 200000.f, Hull, Mods, Grade, Class, false);
		if (W)
		{
			FHDItemStack L;
			L.Item = TEXT("salvage_alloy");
			L.Qty = 5;
			W->Loot.Add(L);
			Notify(FString::Printf(TEXT("Spawned %s"), *W->GetKindLabel()));
		}
		return true;
	}
	if (Verb == TEXT("econ"))
	{
		// econ: debug readout of regional mining and economy state.
		UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
		const int32 Region = (U && Ship && Ship->SystemIndex >= 0) ? U->GetSystem(Ship->SystemIndex).Region : -1;
		Notify(FString::Printf(TEXT("Region %d: mined %.0f, richness %.2f | orders %d, jobs %d"), Region, Econ->GetRegionConsumed(Region), Econ->GetRegionRichness(Region), Econ->Orders.Num(), Econ->Jobs.Num()));
		return true;
	}
	if (Verb == TEXT("mined"))
	{
		// mined <units>: simulate regional consumption to test belt thinning.
		UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
		const int32 Region = (U && Ship && Ship->SystemIndex >= 0) ? U->GetSystem(Ship->SystemIndex).Region : -1;
		Econ->AddMined(Region, A.Num() > 0 ? FCString::Atof(*A[0]) : 10000.f);
		Notify(FString::Printf(TEXT("Region %d richness now %.2f"), Region, Econ->GetRegionRichness(Region)));
		return true;
	}
#endif
	return false;
}
