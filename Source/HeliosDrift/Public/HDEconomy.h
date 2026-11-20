#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HDItemTypes.h"
#include "HDEconomy.generated.h"

class AHDStationActor;
class AHDPlayerState;
class AHDShipPawn;
class AHDWreckActor;
struct FHDItemDef;

/** A player market order at a station. Goods don't teleport: an order fills only at its station (doc 03). */
USTRUCT(BlueprintType)
struct FHDMarketOrder
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Market") int32 Id = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Market") int32 Region = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Market") int32 System = -1;
	UPROPERTY(BlueprintReadOnly, Category = "Market") FName Station;
	UPROPERTY(BlueprintReadOnly, Category = "Market") FString Owner;
	UPROPERTY(BlueprintReadOnly, Category = "Market") FName Item;
	UPROPERTY(BlueprintReadOnly, Category = "Market") int32 Qty = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Market") int32 Price = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Market") bool bBuy = false;
	UPROPERTY(BlueprintReadOnly, Category = "Market") uint8 Flags = 0;
};

/** Running fabrication / refining job (doc 03). */
USTRUCT(BlueprintType)
struct FHDJob
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Industry") FString Owner;
	UPROPERTY(BlueprintReadOnly, Category = "Industry") FName Station;
	UPROPERTY(BlueprintReadOnly, Category = "Industry") FName Recipe;
	UPROPERTY(BlueprintReadOnly, Category = "Industry") int32 Runs = 1;
	UPROPERTY() double FinishUnix = 0.0;
	/** System of the station (claim output accounting). */
	UPROPERTY() int32 System = -1;
};

/** NPC price quote for one item at one station. Price 0 = not traded that way here. */
struct FHDQuote
{
	int32 NpcSells = 0;  // what you pay
	int32 NpcBuys = 0;   // what you get
	bool bStolenGoods = false;
};

/**
 * Server economy (doc 03, 16, 19): NPC station prices with stock-driven drift, player order books per station
 * (visible region-wide), sales tax and broker fees, regional mining consumption that throttles belt respawn,
 * gas-giant fuel platforms priced by weekly cloud density, wreck spawning, and fabrication jobs.
 * Lives on the game instance; ticked by the system director (server only).
 */
UCLASS()
class HELIOSDRIFT_API UHDEconomySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	static UHDEconomySubsystem* Get(const UObject* WorldContext);
	void Tick(float Dt, UWorld* World);

	// ---- Mining ----
	void AddMined(int32 Region, float Units);
	/** 1 = untouched region, down to 0.2 when heavily mined (belt respawn scales to regional consumption). */
	float GetRegionRichness(int32 Region) const;
	float GetRegionConsumed(int32 Region) const { const float* V = RegionConsumed.Find(Region); return V ? *V : 0.f; }
	UPROPERTY(EditAnywhere, Category = "Mining") float RegionDepletionUnits = 40000.f;
	UPROPERTY(EditAnywhere, Category = "Mining") float RegionRecoveryHours = 12.f;

	// ---- Regional wealth (drives antagonist spawn pressure, doc 18) ----
	void AddTrade(int32 Region, int64 Credits) { if (Region >= 0 && Credits > 0) RegionTrade.FindOrAdd(Region) += (float)Credits; }
	float GetRegionTrade(int32 Region) const { const float* V = RegionTrade.Find(Region); return V ? *V : 0.f; }

	// ---- NPC market ----
	FHDQuote Quote(const AHDStationActor* St, const FName& Item) const;
	/** Server: move NPC stock after a trade (positive = NPC received goods). */
	void NpcStockDelta(const AHDStationActor* St, const FName& Item, int32 Delta);
	float SalesTax(const AHDStationActor* St) const;
	UPROPERTY(EditAnywhere, Category = "Market") float BrokerFee = 0.01f;
	/** Systems whose trade lanes are cut (doc 13: prices move within the hour). Set by territory. */
	TSet<int32> LaneCutSystems;

	// ---- Player orders ----
	TArray<FHDMarketOrder> Orders;
	int32 PlaceOrder(const FHDMarketOrder& O);
	/** Pay a pilot (online now, or held until they log in). */
	void PayPilot(UWorld* World, const FString& Pilot, int64 Amount);
	/** Deliver goods to a pilot's station storage (online or held). */
	void DeliverToStorage(UWorld* World, const FString& Pilot, const FName& Station, const FName& Item, int32 Qty, uint8 Flags);
	/** Apply held credits/goods when a pilot logs in. */
	void ClaimPending(AHDPlayerState* PS);
	TMap<FString, int64> PendingCredits;
	TArray<FHDStoredStack> PendingGoodsList;
	TArray<FString> PendingGoodsOwner;

	// ---- Fuel platforms ----
	/** Jump fuel price at a station (fuel platforms only; 0 = not sold). Cheaper when the gas giant is Abundant this week. */
	int32 JumpFuelPrice(const AHDStationActor* St) const;
	int32 ShipFuelPrice(const AHDStationActor* St) const;

	// ---- Industry ----
	TArray<FHDJob> Jobs;
	/** Server: start a job; inputs must already be removed. */
	void StartJob(const FHDJob& J) { Jobs.Add(J); }

	// ---- Wrecks ----
	static AHDWreckActor* SpawnWreck(UWorld* World, int32 System, const FVector& Loc, const FName& Hull, const TArray<FName>& Modules, int32 Grade, int32 HulkClass, bool bStation);

	// ---- Persistence hooks (M7) ----
	FString ExportJson() const;
	void ImportJson(const FString& Json);

private:
	TMap<int32, float> RegionConsumed;
	TMap<int32, float> RegionTrade;
	/** NPC stock per "station|item". */
	TMap<FString, float> NpcStock;
	int32 NextOrderId = 1;
	float StockRelaxTimer = 0.f;
	float FuelTimer = 0.f;
	float NpcStockOf(const AHDStationActor* St, const FName& Item) const;
	static FString StockKey(const AHDStationActor* St, const FName& Item);
	/** Deterministic: does this station produce (+1), consume (-1) or neither (0) a trade good? */
	static int32 TradeBias(const AHDStationActor* St, const FName& Item);
};
