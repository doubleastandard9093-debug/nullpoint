#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HDItemTypes.h"
#include "HDCargoComponent.generated.h"

/** Ship hold: general cargo plus an ore hold for raw ore, ice and gas (doc 14/16). Server-authoritative. */
UCLASS(ClassGroup = (Ship), meta = (BlueprintSpawnableComponent))
class HELIOSDRIFT_API UHDCargoComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHDCargoComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Cargo") TArray<FHDItemStack> Items;
	/** m3 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Cargo") float Capacity = 60.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Cargo") float OreCapacity = 0.f;

	int32 Count(const FName& Item) const;
	/** Used volume: ore-hold volume and general volume (raw goods overflow into general). */
	void GetUsed(float& OutGeneral, float& OutOre) const;
	float GetUsedTotal() const { float G, O; GetUsed(G, O); return G + O; }
	/** How many units of Item still fit. */
	int32 SpaceFor(const FName& Item) const;
	/** Server: add up to Qty; returns the number added. */
	int32 Add(const FName& Item, int32 Qty, uint8 Flags = 0);
	/** Server: remove up to Qty; returns the number removed. Flags of removed units are OR-ed into OutFlags. */
	int32 Remove(const FName& Item, int32 Qty, uint8* OutFlags = nullptr);
	bool HasFlagged(uint8 Mask) const;
	/** Total reference value (cr). */
	int64 GetValue() const;
	FString Describe() const;

private:
	bool IsRaw(const FName& Item) const;
	float Vol(const FName& Item) const;
};
