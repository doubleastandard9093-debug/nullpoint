#pragma once
#include "CoreMinimal.h"
#include "HDWorldActors.h"
#include "HDTerritory.h"
#include "HDStructureActor.generated.h"

/**
 * World presence of a player structure (doc 19). Data lives in UHDTerritorySubsystem::Structures; this actor mirrors
 * the replicated readout, takes damage, and runs gun / platform fire and interdictor fields on the server.
 */
UCLASS()
class HELIOSDRIFT_API AHDStructureActor : public AHDSpaceObject
{
	GENERATED_BODY()
public:
	AHDStructureActor();
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual float TakeDamage(float Damage, struct FDamageEvent const& Event, AController* Instigator, AActor* Causer) override;
	virtual FString GetKindLabel() const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Structure") TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Structure") EHDStructureType Type = EHDStructureType::Beacon;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Structure") FName OwnerCorp;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Structure") float Integrity = 1.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Structure") float MaxIntegrity = 1.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Structure") EHDSiegeState Siege = EHDSiegeState::Normal;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Structure") int32 SectionCount = 0;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Structure") bool bOutOfFuel = false;

	void InitStructure(const FHDStructure& S);
	void SyncFrom(const FHDStructure& S);

protected:
	virtual void ApplyVisual() override;
	float FireTimer = 0.f;
};
