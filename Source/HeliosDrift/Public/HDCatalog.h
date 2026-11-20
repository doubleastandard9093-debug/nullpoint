#pragma once
#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "HDShipTypes.h"
#include "HDCatalog.generated.h"

UENUM(BlueprintType)
enum class EHDSlotType : uint8 { Weapon, Utility, Support, Engine, Hold };

USTRUCT(BlueprintType)
struct FHDItemDef
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FString Name;
	/** Ore, Ice, Gas, Refined, Component, Salvage, Trade, Contraband, Bounty, Module, Hull. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FName Category;
	/** m3 per unit. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Volume = 1.f;
	/** Reference price (cr); markets move around this. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") int32 Price = 1;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") bool bLegal = true;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") int32 Tier = 1;

	bool IsRaw() const { return Category == TEXT("Ore") || Category == TEXT("Ice") || Category == TEXT("Gas"); }
};

USTRUCT(BlueprintType)
struct FHDModuleDef
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") EHDSlotType Slot = EHDSlotType::Weapon;
	/** 1 S, 2 M, 3 L, 4 Capital. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") int32 Size = 1;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") int32 Tier = 1;
	/** MW drawn from the hull's reactor. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Power = 0.f;
	/** Stability consumed (doc 14: hardpoints and modules change ship stability). */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Stability = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") int32 Price = 0;
	/** Visible part: Cube, Cylinder, Cone, Sphere; scale in meters; tint. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FName Shape = TEXT("Cube");
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FVector VisualScale = FVector::OneVector;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FLinearColor Color = FLinearColor::Gray;
	/** Effects by key: damage, range (m), interval, mining, salvage, gas, shield, hull, cargo, powerOut, shipFuel, jumpFuel, speed, turn, scan, tow, interdict, cloak, command, fighters, jumpDrive, oreOnly. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") TMap<FName, float> Effects;

	float Fx(const TCHAR* Key) const { const float* V = Effects.Find(FName(Key)); return V ? *V : 0.f; }
};

USTRUCT(BlueprintType)
struct FHDHullDef
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FString Name;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") EHDHullClass Class = EHDHullClass::Frigate;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") EHDHullRole Role = EHDHullRole::Combat;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") EHDSupplyTier Supply = EHDSupplyTier::Baseline;
	/** 1-3. All capitals are Tier 3 and player-built. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") int32 Tier = 1;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FName Faction;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") int32 Price = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FString Quirk;
	/** m/s, m/s2, deg/s (yaw). */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Speed = 300.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Accel = 150.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Turn = 45.f;
	/** Length, width, height in meters. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FVector SizeM = FVector(40.f, 18.f, 8.f);
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float HP = 1000.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Shield = 500.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Power = 100.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Stability = 100.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Cargo = 60.f;
	/** Weapon, Utility, Support, Engine, Hold counts. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") TArray<int32> SlotCounts;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") int32 MaxModuleSize = 1;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float ShipFuel = 60.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float JumpFuel = 0.f;
	/** Role multipliers. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Mining = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Salvage = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Gas = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Scan = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FLinearColor Color = FLinearColor(0.55f, 0.58f, 0.62f);

	int32 NumSlots() const { int32 N = 0; for (int32 C : SlotCounts) N += C; return N; }
	/** Slot index -> type; slots are ordered weapons, utilities, supports, engines, holds. */
	EHDSlotType SlotTypeAt(int32 Index, int32* OutIndexInType = nullptr) const;
};

/** Fabrication / refining recipe (doc 03). */
USTRUCT(BlueprintType)
struct FHDRecipeDef
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FName Id;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FName Output;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") int32 Qty = 1;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") TMap<FName, int32> Inputs;
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") float Seconds = 10.f;
	/** refinery, factory or shipyard. */
	UPROPERTY(BlueprintReadOnly, Category = "Catalog") FName Facility;
};

/**
 * Data catalog: hulls, modules, items and recipes from Content/Data/*.json (generated by tools/gen_catalog.py, hand-editable).
 * Falls back to a tiny built-in set if the files are missing so the game still runs.
 */
UCLASS()
class HELIOSDRIFT_API UHDCatalogSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()
public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	static UHDCatalogSubsystem* Get(const UObject* WorldContext);

	const FHDItemDef* FindItem(const FName& Id) const { return Items.Find(Id); }
	const FHDHullDef* FindHull(const FName& Id) const { return Hulls.Find(Id); }
	const FHDModuleDef* FindModule(const FName& Id) const { return Modules.Find(Id); }
	const FHDRecipeDef* FindRecipe(const FName& Id) const { return Recipes.Find(Id); }
	/** First recipe producing Output. */
	const FHDRecipeDef* FindRecipeFor(const FName& Output) const;
	float GetVolume(const FName& Item) const { const FHDItemDef* D = FindItem(Item); return D ? D->Volume : 1.f; }
	int32 GetPrice(const FName& Item) const { const FHDItemDef* D = FindItem(Item); return D ? D->Price : 1; }
	FString GetName(const FName& Item) const { const FHDItemDef* D = FindItem(Item); return D ? D->Name : Item.ToString(); }

	const TMap<FName, FHDItemDef>& GetItems() const { return Items; }
	const TMap<FName, FHDHullDef>& GetHulls() const { return Hulls; }
	const TMap<FName, FHDModuleDef>& GetModules() const { return Modules; }
	const TMap<FName, FHDRecipeDef>& GetRecipes() const { return Recipes; }

	UPROPERTY() FName StarterHull = TEXT("lance");
	UPROPERTY() TArray<FName> StarterModules;
	UPROPERTY() TMap<FName, int32> StarterStorage;

	/** Reload JSON (console: HDCmd reloadcatalog). */
	void Load();

	template <typename E> static bool ParseEnum(const FString& S, E& Out)
	{
		const UEnum* En = StaticEnum<E>();
		if (!En) return false;
		for (int32 I = 0; I < En->NumEnums() - 1; ++I)
		{
			if (En->GetNameStringByIndex(I).Equals(S, ESearchCase::IgnoreCase)) { Out = (E)En->GetValueByIndex(I); return true; }
		}
		return false;
	}

private:
	UPROPERTY() TMap<FName, FHDItemDef> Items;
	UPROPERTY() TMap<FName, FHDHullDef> Hulls;
	UPROPERTY() TMap<FName, FHDModuleDef> Modules;
	UPROPERTY() TMap<FName, FHDRecipeDef> Recipes;
	void AddFallback();
};
