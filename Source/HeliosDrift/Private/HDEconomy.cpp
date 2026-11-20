#include "HDEconomy.h"
#include "HDTerritory.h"
#include "HDCatalog.h"
#include "HDWorldActors.h"
#include "HDWreckActor.h"
#include "HDPlayerState.h"
#include "HDPlayerController.h"
#include "HDUniverseSubsystem.h"
#include "HDSystemDirector.h"
#include "HeliosDrift.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "GameFramework/GameStateBase.h"
#include "EngineUtils.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	constexpr float NpcStockTarget = 1000.f;
	AHDPlayerState* FindOnlinePilot(UWorld* World, const FString& Pilot)
	{
		AGameStateBase* GS = World ? World->GetGameState() : nullptr;
		if (!GS) return nullptr;
		for (APlayerState* P : GS->PlayerArray)
		{
			AHDPlayerState* H = Cast<AHDPlayerState>(P);
			if (H && H->PilotId == Pilot) return H;
		}
		return nullptr;
	}
}

UHDEconomySubsystem* UHDEconomySubsystem::Get(const UObject* WorldContext)
{
	if (!WorldContext || !GEngine) return nullptr;
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UHDEconomySubsystem>() : nullptr;
}

// ---------------------------------------------------------------------------------------------
// Mining consumption (doc 16: belt respawn scales to regional consumption)
// ---------------------------------------------------------------------------------------------
void UHDEconomySubsystem::AddMined(int32 Region, float Units)
{
	if (Region < 0 || Units <= 0.f) return;
	RegionConsumed.FindOrAdd(Region) += Units;
}

float UHDEconomySubsystem::GetRegionRichness(int32 Region) const
{
	return FMath::Clamp(1.f - GetRegionConsumed(Region) / FMath::Max(1.f, RegionDepletionUnits), 0.2f, 1.f);
}

// ---------------------------------------------------------------------------------------------
// NPC market
// ---------------------------------------------------------------------------------------------
FString UHDEconomySubsystem::StockKey(const AHDStationActor* St, const FName& Item)
{
	return St->ObjectId.ToString() + TEXT("|") + Item.ToString();
}

float UHDEconomySubsystem::NpcStockOf(const AHDStationActor* St, const FName& Item) const
{
	const float* V = NpcStock.Find(StockKey(St, Item));
	return V ? *V : NpcStockTarget;
}

void UHDEconomySubsystem::NpcStockDelta(const AHDStationActor* St, const FName& Item, int32 Delta)
{
	if (!St) return;
	float& V = NpcStock.FindOrAdd(StockKey(St, Item), NpcStockTarget);
	V = FMath::Max(0.f, V + Delta);
}

int32 UHDEconomySubsystem::TradeBias(const AHDStationActor* St, const FName& Item)
{
	const uint32 H = HDUniverse::Mix(GetTypeHash(St->ObjectId), GetTypeHash(Item));
	const uint32 R = H % 3u;
	return R == 0 ? 1 : R == 1 ? -1 : 0;
}

float UHDEconomySubsystem::SalesTax(const AHDStationActor* St) const
{
	if (!St) return 0.02f;
	switch (St->Info.Family)
	{
	case EHDStationFamily::AccordCivic: return 0.01f;
	case EHDStationFamily::PirateKitbash: return 0.f;
	default: return 0.02f;
	}
}

FHDQuote UHDEconomySubsystem::Quote(const AHDStationActor* St, const FName& Item) const
{
	FHDQuote Q;
	const UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(this);
	const FHDItemDef* D = Cat ? Cat->FindItem(Item) : nullptr;
	if (!St || !D || !St->Info.bMarket) return Q;
	const bool bPirate = St->Info.Family == EHDStationFamily::PirateKitbash;
	const bool bIndustrial = St->Info.Family == EHDStationFamily::FactionIndustrial || St->Info.bRefinery;
	float SellMul = 0.f, BuyMul = 0.f; // relative to the reference price
	const FName Cat_ = D->Category;
	if (Cat_ == TEXT("Ore") || Cat_ == TEXT("Ice") || Cat_ == TEXT("Gas"))
	{
		BuyMul = 0.6f;                      // NPC buy floor for raw materials (doc 03)
		SellMul = bIndustrial ? 1.3f : 0.f; // NPCs sell ore
	}
	else if (Cat_ == TEXT("Refined")) { BuyMul = 0.7f; SellMul = bIndustrial ? 1.2f : 0.f; }
	else if (Cat_ == TEXT("Component")) { BuyMul = 0.75f; }  // NPCs buy components; players make them
	else if (Cat_ == TEXT("Salvage")) { BuyMul = 0.7f; }
	else if (Cat_ == TEXT("Trade"))
	{
		const int32 Bias = TradeBias(St, Item);
		SellMul = Bias > 0 ? 0.8f : Bias < 0 ? 1.4f : 1.05f;
		BuyMul = Bias > 0 ? 0.6f : Bias < 0 ? 1.2f : 0.85f;
	}
	else if (Cat_ == TEXT("Contraband")) { if (bPirate) { SellMul = 1.f; BuyMul = 0.8f; } }
	else if (Cat_ == TEXT("Bounty")) { if (St->Info.Family == EHDStationFamily::AccordCivic) BuyMul = 1.f; }
	else if (Cat_ == TEXT("Module")) { BuyMul = 0.3f; }
	if (bPirate)
	{
		// Pirate markets: buy anything, no questions; sell cheap stolen goods that carry a legal mark (doc 18).
		BuyMul = BuyMul > 0.f ? BuyMul * 0.8f : 0.5f;
		if (SellMul <= 0.f && (Cat_ == TEXT("Trade") || Cat_ == TEXT("Refined") || Cat_ == TEXT("Component"))) SellMul = 0.6f;
		Q.bStolenGoods = Cat_ != TEXT("Contraband");
	}
	// Stock drift: scarce goods cost more, gluts pay less.
	const float Stock = NpcStockOf(St, Item);
	const float Drift = FMath::Clamp(1.f + 0.5f * (NpcStockTarget - Stock) / NpcStockTarget, 0.6f, 1.8f);
	// Cut lanes reroute trade through gates: imported goods jump in price (doc 13).
	const float LaneMul = LaneCutSystems.Contains(St->SystemIndex) && Cat_ == TEXT("Trade") ? 1.25f : 1.f;
	if (SellMul > 0.f) Q.NpcSells = FMath::Max(1, FMath::RoundToInt(D->Price * SellMul * Drift * LaneMul));
	if (BuyMul > 0.f) Q.NpcBuys = FMath::Max(1, FMath::RoundToInt(D->Price * BuyMul * Drift * LaneMul));
	if (Q.NpcSells > 0 && Q.NpcBuys >= Q.NpcSells) Q.NpcBuys = Q.NpcSells - 1; // no free money at one station
	return Q;
}

int32 UHDEconomySubsystem::JumpFuelPrice(const AHDStationActor* St) const
{
	if (!St || !St->Info.bFuelStation) return 0;
	const UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	const EHDGasDensity D = U ? U->GetGasDensity(St->SystemIndex, St->Info.Body) : EHDGasDensity::Consistent;
	// Weekly cloud density sets the price: Rare weeks are expensive, Abundant cheap (doc 19).
	const float Mul = D == EHDGasDensity::Rare ? 1.6f : D == EHDGasDensity::Abundant ? 0.7f : 1.f;
	return FMath::Max(1, FMath::RoundToInt(120.f * Mul));
}

int32 UHDEconomySubsystem::ShipFuelPrice(const AHDStationActor* St) const
{
	return St && St->Info.bFuelStation ? 25 : 40;
}

// ---------------------------------------------------------------------------------------------
// Player orders and settlement
// ---------------------------------------------------------------------------------------------
int32 UHDEconomySubsystem::PlaceOrder(const FHDMarketOrder& O)
{
	FHDMarketOrder N = O;
	N.Id = NextOrderId++;
	Orders.Add(N);
	return N.Id;
}

void UHDEconomySubsystem::PayPilot(UWorld* World, const FString& Pilot, int64 Amount)
{
	if (Amount <= 0) return;
	if (AHDPlayerState* PS = FindOnlinePilot(World, Pilot))
	{
		PS->AddCredits(Amount);
		if (AHDPlayerController* PC = Cast<AHDPlayerController>(PS->GetOwner())) PC->Notify(FString::Printf(TEXT("Market: +%lld cr"), Amount));
		return;
	}
	PendingCredits.FindOrAdd(Pilot) += Amount;
}

void UHDEconomySubsystem::DeliverToStorage(UWorld* World, const FString& Pilot, const FName& Station, const FName& Item, int32 Qty, uint8 Flags)
{
	if (Qty <= 0) return;
	if (AHDPlayerState* PS = FindOnlinePilot(World, Pilot))
	{
		PS->AddStorage(Station, Item, Qty, Flags);
		return;
	}
	FHDStoredStack S;
	S.Station = Station;
	S.Item = Item;
	S.Qty = Qty;
	S.Flags = Flags;
	PendingGoodsList.Add(S);
	PendingGoodsOwner.Add(Pilot);
}

void UHDEconomySubsystem::ClaimPending(AHDPlayerState* PS)
{
	if (!PS) return;
	if (const int64* C = PendingCredits.Find(PS->PilotId)) { PS->AddCredits(*C); PendingCredits.Remove(PS->PilotId); }
	for (int32 I = PendingGoodsList.Num() - 1; I >= 0; --I)
	{
		if (PendingGoodsOwner[I] != PS->PilotId) continue;
		const FHDStoredStack& S = PendingGoodsList[I];
		PS->AddStorage(S.Station, S.Item, S.Qty, S.Flags);
		PendingGoodsList.RemoveAt(I);
		PendingGoodsOwner.RemoveAt(I);
	}
}

// ---------------------------------------------------------------------------------------------
// Wrecks
// ---------------------------------------------------------------------------------------------
AHDWreckActor* UHDEconomySubsystem::SpawnWreck(UWorld* World, int32 System, const FVector& Loc, const FName& Hull, const TArray<FName>& Modules, int32 Grade, int32 HulkClass, bool bStation)
{
	if (!World) return nullptr;
	const FTransform T(FRotator(FMath::FRandRange(-30.f, 30.f), FMath::FRandRange(0.f, 360.f), FMath::FRandRange(-60.f, 60.f)), Loc);
	AHDWreckActor* W = World->SpawnActorDeferred<AHDWreckActor>(AHDWreckActor::StaticClass(), T, nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (!W) return nullptr;
	const EHDWreckAge Age = HulkClass <= 0 ? EHDWreckAge::Fresh : Grade >= 3 ? EHDWreckAge::Ancient : EHDWreckAge::Cooled;
	W->InitWreck(System, Hull, Modules, Grade, HulkClass, Age, bStation);
	W->FinishSpawning(T);
	if (AHDSystemDirector* D = AHDSystemDirector::Get(World)) D->AddTransient(System, W);
	return W;
}

// ---------------------------------------------------------------------------------------------
// Tick: stock relaxation, consumption recovery, fuel platforms, jobs
// ---------------------------------------------------------------------------------------------
void UHDEconomySubsystem::Tick(float Dt, UWorld* World)
{
	// Regional consumption recovers exponentially.
	const float Decay = FMath::Exp(-Dt / FMath::Max(60.f, RegionRecoveryHours * 3600.f));
	for (TPair<int32, float>& P : RegionConsumed) P.Value *= Decay;
	const float TradeDecay = FMath::Exp(-Dt / (6.f * 3600.f));
	for (TPair<int32, float>& P : RegionTrade) P.Value *= TradeDecay;

	StockRelaxTimer += Dt;
	if (StockRelaxTimer >= 10.f)
	{
		// NPC stock drifts 5% per minute back toward target.
		const float K = FMath::Clamp(0.05f * StockRelaxTimer / 60.f, 0.f, 1.f);
		for (TPair<FString, float>& P : NpcStock) P.Value += (NpcStockTarget - P.Value) * K;
		StockRelaxTimer = 0.f;
	}

	FuelTimer += Dt;
	if (FuelTimer >= 5.f && World)
	{
		const UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(World);
		for (TActorIterator<AHDStationActor> It(World); It; ++It)
		{
			AHDStationActor* St = *It;
			if (!St->Info.bFuelStation || !St->OwnerCorp.IsNone()) continue; // player platforms are fed by their owners' siphons
			const EHDGasDensity D = U ? U->GetGasDensity(St->SystemIndex, St->Info.Body) : EHDGasDensity::Consistent;
			const float Rate = 2.f * UHDUniverseSubsystem::DensityYieldMultiplier(D); // t per second
			St->FuelStock = FMath::Min(20000.f, St->FuelStock + Rate * FuelTimer);
		}
		FuelTimer = 0.f;
	}

	// Jobs
	const double Now = AHDPlayerState::NowUnix();
	const UHDCatalogSubsystem* Cat = UHDCatalogSubsystem::Get(World);
	for (int32 I = Jobs.Num() - 1; I >= 0; --I)
	{
		const FHDJob& J = Jobs[I];
		if (Now < J.FinishUnix) continue;
		const FHDRecipeDef* R = Cat ? Cat->FindRecipe(J.Recipe) : nullptr;
		if (R)
		{
			const FString Out = R->Output.ToString();
			if (Out.StartsWith(TEXT("hull_")))
			{
				// Hulls come out as bare ships in the builder's hangar at that station.
				AHDPlayerState* PS = FindOnlinePilot(World, J.Owner);
				for (int32 K = 0; K < J.Runs * R->Qty; ++K)
				{
					if (PS)
					{
						FHDStoredShip S;
						S.Station = J.Station;
						S.Hull = FName(*Out.RightChop(5));
						PS->Hangar.Add(S);
					}
					else
					{
						DeliverToStorage(World, J.Owner, J.Station, R->Output, 1, 0); // claimed as packaged hull
					}
				}
			}
			else
			{
				DeliverToStorage(World, J.Owner, J.Station, R->Output, R->Qty * J.Runs, 0);
			}
			// Industrial output feeds the Defense Budget of a claim in the job's system (doc 15).
			if (UHDTerritorySubsystem* Terr = UHDTerritorySubsystem::Get(World))
			{
				const FHDItemDef* OutDef = Cat->FindItem(R->Output);
				if (OutDef && J.System >= 0) Terr->AddIndustrialOutput(J.System, (double)OutDef->Price * R->Qty * J.Runs);
			}
			if (AHDPlayerState* PS = FindOnlinePilot(World, J.Owner))
			{
				if (AHDPlayerController* PC = Cast<AHDPlayerController>(PS->GetOwner()))
				{
					PC->Notify(FString::Printf(TEXT("Job done at %s: %d x %s"), *J.Station.ToString(), R->Qty * J.Runs, Cat ? *Cat->GetName(R->Output) : *R->Output.ToString()));
				}
			}
		}
		Jobs.RemoveAt(I);
	}
}

// ---------------------------------------------------------------------------------------------
// Persistence (backend world state, M7)
// ---------------------------------------------------------------------------------------------
FString UHDEconomySubsystem::ExportJson() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> OrderVals;
	for (const FHDMarketOrder& O : Orders)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		if (FJsonObjectConverter::UStructToJsonObject(FHDMarketOrder::StaticStruct(), &O, J, 0, 0)) OrderVals.Add(MakeShared<FJsonValueObject>(J));
	}
	Root->SetArrayField(TEXT("orders"), OrderVals);
	TArray<TSharedPtr<FJsonValue>> JobVals;
	for (const FHDJob& Jb : Jobs)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		if (FJsonObjectConverter::UStructToJsonObject(FHDJob::StaticStruct(), &Jb, J, 0, 0))
		{
			J->SetNumberField(TEXT("finishUnix"), Jb.FinishUnix);
			JobVals.Add(MakeShared<FJsonValueObject>(J));
		}
	}
	Root->SetArrayField(TEXT("jobs"), JobVals);
	TSharedRef<FJsonObject> Credits = MakeShared<FJsonObject>();
	for (const TPair<FString, int64>& P : PendingCredits) Credits->SetNumberField(P.Key, (double)P.Value);
	Root->SetObjectField(TEXT("pendingCredits"), Credits);
	TSharedRef<FJsonObject> Mined = MakeShared<FJsonObject>();
	for (const TPair<int32, float>& P : RegionConsumed) Mined->SetNumberField(FString::FromInt(P.Key), P.Value);
	Root->SetObjectField(TEXT("regionConsumed"), Mined);
	Root->SetNumberField(TEXT("nextOrderId"), NextOrderId);
	FString Out;
	const TSharedRef<TJsonWriter<>> W = TJsonWriterFactory<>::Create(&Out);
	FJsonSerializer::Serialize(Root, W);
	return Out;
}

void UHDEconomySubsystem::ImportJson(const FString& Json)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> R = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(R, Root) || !Root.IsValid()) return;
	Orders.Reset();
	const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
	if (Root->TryGetArrayField(TEXT("orders"), A) && A)
	{
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			FHDMarketOrder O;
			if (V.IsValid() && V->AsObject().IsValid() && FJsonObjectConverter::JsonObjectToUStruct(V->AsObject().ToSharedRef(), &O, 0, 0)) Orders.Add(O);
		}
	}
	Jobs.Reset();
	if (Root->TryGetArrayField(TEXT("jobs"), A) && A)
	{
		for (const TSharedPtr<FJsonValue>& V : *A)
		{
			FHDJob J;
			if (V.IsValid() && V->AsObject().IsValid() && FJsonObjectConverter::JsonObjectToUStruct(V->AsObject().ToSharedRef(), &J, 0, 0))
			{
				double F = 0.0;
				V->AsObject()->TryGetNumberField(TEXT("finishUnix"), F);
				J.FinishUnix = F;
				Jobs.Add(J);
			}
		}
	}
	const TSharedPtr<FJsonObject>* O = nullptr;
	if (Root->TryGetObjectField(TEXT("pendingCredits"), O) && O)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*O)->Values) PendingCredits.Add(P.Key, (int64)P.Value->AsNumber());
	}
	if (Root->TryGetObjectField(TEXT("regionConsumed"), O) && O)
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*O)->Values) RegionConsumed.Add(FCString::Atoi(*P.Key), (float)P.Value->AsNumber());
	}
	double Next = 1.0;
	if (Root->TryGetNumberField(TEXT("nextOrderId"), Next)) NextOrderId = FMath::Max(NextOrderId, (int32)Next);
}
