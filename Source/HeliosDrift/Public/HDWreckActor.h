#pragma once
#include "CoreMinimal.h"
#include "HDWorldActors.h"
#include "HDItemTypes.h"
#include "HDWreckActor.generated.h"

class AHDShipPawn;
class AHDPlayerState;

/** Hulk entry flag chosen on breach (doc 16 §7B.2). */
UENUM(BlueprintType)
enum class EHDHulkFlag : uint8 { None, Solo, Open, Corp };

/** Fresh field wreck -> cooled hulk -> ancient derelict (doc 16). */
UENUM(BlueprintType)
enum class EHDWreckAge : uint8 { Fresh, Cooled, Ancient };

/**
 * A ship wreck or hulk. Damage grade A..E (0..4) drops one step per 24 h untouched and one step per salvage pass.
 * Salvaging yields fitted modules (G-A: 80-95%) and materials; a good hull can be towed to a shipyard and rebuilt
 * at ~30% of new-build cost (G-A), carrying a "recovered" tag. Station derelicts yield station modules only.
 */
UCLASS()
class HELIOSDRIFT_API AHDWreckActor : public AHDSpaceObject
{
	GENERATED_BODY()
public:
	AHDWreckActor();
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual FString GetKindLabel() const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Wreck") TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Wreck") FName HullId;
	/** 0 = G-A (intact) ... 4 = G-E; 5 = picked clean. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") int32 Grade = 1;
	/** 0 = field wreck from a kill; 1..4 = hulk site class (doc 16). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") int32 HulkClass = 0;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") EHDWreckAge Age = EHDWreckAge::Fresh;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") bool bStationDerelict = false;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") EHDHulkFlag Flag = EHDHulkFlag::None;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") FString FlagHolder;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") int32 ModulesLeft = 0;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") float PassProgress = 0.f;
	/** Pilot who died here (field wrecks): looting someone else's wreck in lawful space is theft. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") FString VictimPilot;
	/** Loose cargo container (encounter loot): no hull, loot only. */
	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Wreck") bool bCrate = false;
	/** NPC and encounter wrecks never rebuild (doc 20: anomalies never drop a rebuildable hull). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Wreck") bool bNoRebuild = false;
	/** Dead-signature crate holding a chart toward a hidden wormhole or ruin gate. */
	bool bChart = false;

	/** Server state. */
	TArray<FName> Modules;
	TArray<FHDItemStack> Loot;
	FName FlagCorp;
	double LastTouchedUnix = 0.0;
	double CollapseAtUnix = 0.0;
	int32 TimesWorked = 0;
	TWeakObjectPtr<AHDShipPawn> TowedBy;
	/** Claim owner collecting a salvage-rights toll on others' yield (doc 16 Open bonus 4). Set by territory. */
	FName SalvageTollOwner;

	/** Server: set up. Grade 0..4. */
	void InitWreck(int32 InSystem, const FName& InHull, const TArray<FName>& InModules, int32 InGrade, int32 InHulkClass, EHDWreckAge InAge, bool bInStation);

	/** Flags a class allows (doc 16): 1 Solo/Corp, 2 all, 3 Open/Corp, 4 Open only. Field wrecks: no flag. */
	bool IsFlagAllowed(EHDHulkFlag F) const;
	/** Server: breach with a flag. Broadcasts to pilots in scanner range. */
	bool Breach(AHDShipPawn* By, EHDHulkFlag F, FString& OutWhy);
	/** May this pilot work / loot it right now? */
	bool CanWork(const AHDShipPawn* By, FString& OutWhy) const;
	/** Server: apply salvage beam work. Returns a message when a pass completes (empty otherwise). */
	FString Work(AHDShipPawn* By, float Amount);
	/** Work units for one pass. */
	float PassCost() const;
	/** Credits to rebuild at a shipyard, or -1 if it can't be rebuilt. */
	int64 RebuildCost() const;
	static const TCHAR* GradeName(int32 G);
	/** Server: turn this into a loot container. */
	void MakeCrate(const FString& Name, bool bInChart);
	static FString FlagName(EHDHulkFlag F);
	/** Ship size factor (1 frigate ... 6 supercapital) from the hull class. */
	int32 SizeFactor() const;

protected:
	virtual void ApplyVisual() override;
};
