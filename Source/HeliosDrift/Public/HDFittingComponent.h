#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HDCatalog.h"
#include "HDFittingComponent.generated.h"

class UStaticMeshComponent;

/** Derived ship stats from hull + fitted modules (doc 14). Computed identically on server and clients. */
USTRUCT(BlueprintType)
struct FHDFitStats
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float PowerUsed = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float PowerMax = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Stability = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float StabilityMax = 0.f;
	/** m/s, m/s2, deg/s. */
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Speed = 300.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Accel = 150.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Turn = 45.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float HP = 1000.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Shield = 500.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Cargo = 60.f;
	/** Ore-hold volume (raw ore, ice and gas only). */
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float OreCargo = 0.f;
	/** Weapons, folded into one hitscan battery: damage per shot at Interval, range in cm. */
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Damage = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Interval = 0.15f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Range = 500000.f;
	/** Units per second. */
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Mining = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Salvage = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Gas = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Scan = 1.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float Tow = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float ShipFuel = 60.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float JumpFuel = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") bool bJumpDrive = false;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") bool bInterdictor = false;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") bool bCloak = false;
	/** Fleet mining boost given to others (industrial command, doc 16). */
	UPROPERTY(BlueprintReadOnly, Category = "Fit") float CommandBoost = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") int32 Fighters = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") int32 Weapons = 0;
};

/**
 * Hull + modules. Visible build readout: weapons render on hardpoints, support pods widen the hull, utility masts on top,
 * a hold module replaces the rear section, engine nozzles follow the drive (doc 14). Server-authoritative fit changes.
 */
UCLASS(ClassGroup = (Ship), meta = (BlueprintSpawnableComponent))
class HELIOSDRIFT_API UHDFittingComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHDFittingComponent();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(ReplicatedUsing = OnRep_Fit, BlueprintReadOnly, Category = "Fit") FName HullId;
	/** One entry per slot (weapons, utilities, supports, engines, holds); None = empty. */
	UPROPERTY(ReplicatedUsing = OnRep_Fit, BlueprintReadOnly, Category = "Fit") TArray<FName> Modules;
	/** Rebuilt from salvage (doc 16). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Fit") bool bRecovered = false;
	UPROPERTY(BlueprintReadOnly, Category = "Fit") FHDFitStats Stats;

	/** Server: replace hull and modules. Fails (no change) if the fit is invalid. */
	bool SetFitAuthority(const FName& Hull, const TArray<FName>& InModules, FString& OutError);
	/** Server: put Module in Slot (validated). */
	bool FitModuleAuthority(int32 Slot, const FName& Module, FString& OutError);
	/** Server: empty Slot; returns the removed module (None if empty). */
	FName UnfitAuthority(int32 Slot);

	/** Validate a candidate fit and compute its stats. */
	static bool ComputeStats(const UHDCatalogSubsystem* Catalog, const FName& Hull, const TArray<FName>& InModules, FHDFitStats& Out, FString& OutError);

	/** A sensible default fit for a hull (NPCs, test commands): role-appropriate tools, capital jump drive. Always valid. */
	static TArray<FName> SuggestFit(const UHDCatalogSubsystem* Catalog, const FName& Hull, bool bCombat = false);

	const FHDHullDef* GetHullDef() const;
	FString DescribeFit() const;

	/** Push Stats into movement, shields, weapons, nav, cargo, hull size (both sides). */
	void ApplyStats();
	void RebuildVisuals();

protected:
	virtual void BeginPlay() override;
	UFUNCTION() void OnRep_Fit();

	UPROPERTY(Transient) TArray<TObjectPtr<UStaticMeshComponent>> Parts;
};
