#include "HDStructureActor.h"
#include "HDTerritory.h"
#include "HDShipPawn.h"
#include "HDNpcController.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"
#include "Net/UnrealNetwork.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/DamageType.h"
#include "EngineUtils.h"

AHDStructureActor::AHDStructureActor()
{
	PrimaryActorTick.bCanEverTick = true;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cyl.Succeeded()) Mesh->SetStaticMesh(Cyl.Object);
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
	InteractRadius = 500000.f;
}

void AHDStructureActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDStructureActor, Type);
	DOREPLIFETIME(AHDStructureActor, OwnerCorp);
	DOREPLIFETIME(AHDStructureActor, Integrity);
	DOREPLIFETIME(AHDStructureActor, MaxIntegrity);
	DOREPLIFETIME(AHDStructureActor, Siege);
	DOREPLIFETIME(AHDStructureActor, SectionCount);
	DOREPLIFETIME(AHDStructureActor, bOutOfFuel);
}

void AHDStructureActor::InitStructure(const FHDStructure& S)
{
	SystemIndex = S.System;
	ObjectId = S.Id;
	Type = S.Type;
	DisplayName = FString::Printf(TEXT("%s [%s]"), UHDTerritorySubsystem::TypeName(S.Type), *S.Corp.ToString());
	SyncFrom(S);
	ApplyVisual();
}

void AHDStructureActor::SyncFrom(const FHDStructure& S)
{
	OwnerCorp = S.Corp;
	Integrity = S.Integrity;
	MaxIntegrity = S.MaxIntegrity;
	Siege = S.Siege;
	SectionCount = S.Sections.Num();
	bOutOfFuel = S.Fuel <= 0.f;
}

FString AHDStructureActor::GetKindLabel() const
{
	const TCHAR* SiegeName = Siege == EHDSiegeState::Reinforced ? TEXT(", REINFORCED") : Siege == EHDSiegeState::Vulnerable ? TEXT(", VULNERABLE") : TEXT("");
	return FString::Printf(TEXT("%s, integrity %.0f%%%s%s%s"), UHDTerritorySubsystem::TypeName(Type), 100.f * Integrity / FMath::Max(1.f, MaxIntegrity), SiegeName,
		bOutOfFuel ? TEXT(", OUT OF FUEL") : TEXT(""), SectionCount > 0 ? *FString::Printf(TEXT(", %d sections"), SectionCount) : TEXT(""));
}

void AHDStructureActor::ApplyVisual()
{
	FVector Scale(4.f, 4.f, 2.f);
	FLinearColor C(0.6f, 0.65f, 0.7f);
	switch (Type)
	{
	case EHDStructureType::Beacon: Scale = FVector(6.f, 6.f, 30.f); C = FLinearColor(0.2f, 0.6f, 1.f); break;
	case EHDStructureType::GunPlacement: Scale = FVector(3.f, 3.f, 2.f); C = FLinearColor(0.8f, 0.3f, 0.2f); break;
	case EHDStructureType::DefensePlatform: Scale = FVector(12.f, 12.f, 3.f); C = FLinearColor(0.8f, 0.4f, 0.2f); break;
	case EHDStructureType::Habitat: Scale = FVector(30.f, 30.f, 4.f); C = FLinearColor(0.7f, 0.8f, 0.6f); break;
	case EHDStructureType::Refinery: case EHDStructureType::Factory: Scale = FVector(20.f, 20.f, 12.f); C = FLinearColor(0.6f, 0.5f, 0.3f); break;
	case EHDStructureType::FuelPlatform: Scale = FVector(25.f, 25.f, 6.f); C = FLinearColor(0.4f, 0.7f, 0.4f); break;
	case EHDStructureType::Station: Scale = FVector(40.f, 40.f, 40.f); C = FLinearColor(0.5f, 0.55f, 0.65f); break;
	case EHDStructureType::Interdictor: Scale = FVector(8.f, 8.f, 8.f); C = FLinearColor(0.7f, 0.2f, 0.7f); break;
	default: break;
	}
	if (Mesh)
	{
		Mesh->SetRelativeScale3D(Scale);
		Tint(Mesh, C);
	}
}

float AHDStructureActor::TakeDamage(float Damage, FDamageEvent const& Event, AController* Inst, AActor* Causer)
{
	if (!HasAuthority() || Damage <= 0.f) return 0.f;
	UHDTerritorySubsystem* T = UHDTerritorySubsystem::Get(this);
	return T ? T->DamageStructure(this, Damage, Causer) : 0.f;
}

void AHDStructureActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority()) return;
	FireTimer -= DeltaSeconds;
	if (FireTimer > 0.f) return;
	FireTimer = 1.f;
	UHDTerritorySubsystem* T = UHDTerritorySubsystem::Get(this);
	FHDStructure* S = T ? T->FindStructure(ObjectId) : nullptr;
	if (!S) return;
	SyncFrom(*S);
	const bool bGun = S->Type == EHDStructureType::GunPlacement || S->Type == EHDStructureType::DefensePlatform
		|| (S->Type == EHDStructureType::Station && S->Sections.Contains(FName(TEXT("hardpoint"))));
	if (!bGun || S->bDerelict || S->Fuel <= 0.f) return;
	// Tier 1/2 hardware: autonomous, cheap per unit, hungry for ammo (doc 15).
	const float AmmoPerShot = S->Type == EHDStructureType::DefensePlatform ? 3.f : 1.f;
	if (S->Ammo < AmmoPerShot) return;
	const double Range = S->Type == EHDStructureType::DefensePlatform ? 3.0e6 : 2.0e6;
	AHDShipPawn* Best = nullptr;
	double BestD = Range;
	for (TActorIterator<AHDShipPawn> It(GetWorld()); It; ++It)
	{
		if (It->SystemIndex != SystemIndex || It->HullHP <= 0.f || !T->IsHostileToCorp(S->Corp, *It)) continue;
		const double D = FVector::Dist(It->GetActorLocation(), GetActorLocation());
		if (D < BestD) { BestD = D; Best = *It; }
	}
	if (!Best) return;
	// Diminishing returns past the first few guns in a claim (anti-bloat).
	int32 GunsInSystem = 0;
	for (const TPair<FName, FHDStructure>& P : T->Structures)
	{
		if (P.Value.System == S->System && P.Value.Type == EHDStructureType::GunPlacement && !P.Value.bDerelict) ++GunsInSystem;
	}
	const float Dimin = S->Type == EHDStructureType::GunPlacement && GunsInSystem > 4 ? FMath::Pow(0.85f, (float)(GunsInSystem - 4)) : 1.f;
	float Dps = S->Type == EHDStructureType::DefensePlatform ? 220.f : S->Type == EHDStructureType::Station ? 120.f * S->Sections.FilterByPredicate([](const FName& N) { return N == FName(TEXT("hardpoint")); }).Num() : 70.f;
	if (S->Type == EHDStructureType::Station) Dps *= UHDTerritorySubsystem::RoutingEfficiency(*S);
	S->Ammo -= AmmoPerShot;
	UGameplayStatics::ApplyDamage(Best, Dps * Dimin, nullptr, this, UDamageType::StaticClass());
}
