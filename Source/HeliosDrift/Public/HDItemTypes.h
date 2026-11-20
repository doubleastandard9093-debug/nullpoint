#pragma once
#include "CoreMinimal.h"
#include "HDItemTypes.generated.h"

/** Item provenance / legal flags carried with a stack. */
namespace HDItemFlags
{
	constexpr uint8 Stolen = 1 << 0;     // pirate-market goods: a legal mark in lawful space (doc 18)
	constexpr uint8 Recovered = 1 << 1;  // rebuilt from salvage (doc 16)
	constexpr uint8 Contraband = 1 << 2;
}

/** A stack of one item in a hold. */
USTRUCT(BlueprintType)
struct FHDItemStack
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Items") FName Item;
	UPROPERTY(BlueprintReadOnly, Category = "Items") int32 Qty = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Items") uint8 Flags = 0;
};

/** Items stored at a station (station hangar storage). */
USTRUCT(BlueprintType)
struct FHDStoredStack
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Items") FName Station;
	UPROPERTY(BlueprintReadOnly, Category = "Items") FName Item;
	UPROPERTY(BlueprintReadOnly, Category = "Items") int32 Qty = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Items") uint8 Flags = 0;
};

/** A ship parked in a station hangar. */
USTRUCT(BlueprintType)
struct FHDStoredShip
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Items") FName Station;
	UPROPERTY(BlueprintReadOnly, Category = "Items") FName Hull;
	UPROPERTY(BlueprintReadOnly, Category = "Items") TArray<FName> Modules;
	UPROPERTY(BlueprintReadOnly, Category = "Items") bool bRecovered = false;
};
