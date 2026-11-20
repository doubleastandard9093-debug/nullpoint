#include "HDWorldActors.h"
#include "HDPlayerState.h"
#include "HDUniverseSubsystem.h"
#include "Components/StaticMeshComponent.h"
#include "Components/SphereComponent.h"
#include "Components/BoxComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInterface.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "GameFramework/PlayerController.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	/** System content is relevant across a whole system (systems sit in slots ~2e12 cm apart). */
	constexpr float SystemCullDistance = 8.0e11f;

	void NoCollision(UStaticMeshComponent* M)
	{
		M->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		M->SetGenerateOverlapEvents(false);
		M->SetCanEverAffectNavigation(false);
	}
}

// ---------------------------------------------------------------------------------------------
// AHDSpaceObject
// ---------------------------------------------------------------------------------------------
AHDSpaceObject::AHDSpaceObject()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.5f;
	bReplicates = true;
	SetReplicateMovement(false);
	SetSystemRelevancy();
}

void AHDSpaceObject::SetSystemRelevancy()
{
	SetNetCullDistanceSquared(SystemCullDistance * SystemCullDistance);
	SetNetUpdateFrequency(2.f);
}

void AHDSpaceObject::BeginPlay()
{
	Super::BeginPlay();
	ApplyVisual();
}

void AHDSpaceObject::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDSpaceObject, SystemIndex);
	DOREPLIFETIME(AHDSpaceObject, ObjectId);
	DOREPLIFETIME(AHDSpaceObject, DisplayName);
	DOREPLIFETIME(AHDSpaceObject, bRequiresScan);
	DOREPLIFETIME(AHDSpaceObject, Signature);
	DOREPLIFETIME(AHDSpaceObject, InteractRadius);
}

bool AHDSpaceObject::IsRevealedTo(const APlayerController* PC) const
{
	if (!bRequiresScan) return true;
	const AHDPlayerState* PS = PC ? PC->GetPlayerState<AHDPlayerState>() : nullptr;
	return PS && PS->HasRevealed(ObjectId);
}

void AHDSpaceObject::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	// Client-side: hide unscanned signatures from this pilot. The server keeps them visible (it is not a viewer).
	if (GetNetMode() == NM_DedicatedServer || !bRequiresScan) return;
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC || !PC->IsLocalController()) return;
	const bool bShow = IsRevealedTo(PC);
	if (IsHidden() == bShow) { SetActorHiddenInGame(!bShow); }
}

UStaticMesh* AHDSpaceObject::BasicMesh(const TCHAR* Shape)
{
	const FString Path = FString::Printf(TEXT("/Engine/BasicShapes/%s.%s"), Shape, Shape);
	return LoadObject<UStaticMesh>(nullptr, *Path);
}

void AHDSpaceObject::Tint(UStaticMeshComponent* Mesh, const FLinearColor& Color)
{
	if (!Mesh) return;
	UMaterialInterface* Base = Mesh->GetMaterial(0);
	if (!Base) return;
	UMaterialInstanceDynamic* MID = Cast<UMaterialInstanceDynamic>(Base);
	if (!MID)
	{
		MID = UMaterialInstanceDynamic::Create(Base, Mesh);
		Mesh->SetMaterial(0, MID);
	}
	if (MID) { MID->SetVectorParameterValue(TEXT("Color"), Color); }
}

// ---------------------------------------------------------------------------------------------
// AHDCelestialBody
// ---------------------------------------------------------------------------------------------
AHDCelestialBody::AHDCelestialBody()
{
	PrimaryActorTick.bCanEverTick = false;
	Collision = CreateDefaultSubobject<USphereComponent>(TEXT("Collision"));
	Collision->SetSphereRadius(50000.f);
	Collision->SetCollisionProfileName(TEXT("BlockAll"));
	RootComponent = Collision;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Collision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded()) { Mesh->SetStaticMesh(Sphere.Object); }
	NoCollision(Mesh);
	Mesh->SetCastShadow(false);
}

void AHDCelestialBody::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDCelestialBody, Body);
	DOREPLIFETIME(AHDCelestialBody, BodyIndex);
	DOREPLIFETIME(AHDCelestialBody, Density);
}

void AHDCelestialBody::InitBody(int32 InSystem, int32 InIndex, const FHDBody& InBody, EHDGasDensity InDensity)
{
	SystemIndex = InSystem;
	BodyIndex = InIndex;
	Body = InBody;
	Density = InDensity;
	ObjectId = InBody.Id;
	DisplayName = InBody.Name;
	InteractRadius = (float)FMath::Min(InBody.VisualRadiusCm * 0.05 + 300000.0, 1.0e8);
	ApplyVisual();
}

FString AHDCelestialBody::GetKindLabel() const
{
	FString L = HDUniverse::BodyTypeName(Body.Type);
	if (IsGasGiant()) { L += FString::Printf(TEXT(" [%s]"), HDUniverse::DensityName(Density)); }
	else if (Body.bLandable) { L += FString::Printf(TEXT(" %.2fg"), Body.Gravity); }
	return L;
}

void AHDCelestialBody::ApplyVisual()
{
	const double R = FMath::Max(Body.VisualRadiusCm, 1000.0);
	// Gas giants: ships can descend into the upper cloud layer (scooping), so the hard surface sits deeper.
	const double CollideR = IsGasGiant() ? R * 0.9 : R;
	Collision->SetSphereRadius((float)CollideR);
	Mesh->SetRelativeScale3D(FVector(R / 50.0));
	FLinearColor C(0.5f, 0.5f, 0.5f);
	switch (Body.Type)
	{
	case EHDBodyType::Star: C = FLinearColor(4.f, 3.2f, 1.6f); break;
	case EHDBodyType::Terran: C = FLinearColor(0.2f, 0.5f, 0.25f); break;
	case EHDBodyType::Water: C = FLinearColor(0.1f, 0.3f, 0.8f); break;
	case EHDBodyType::Desert: C = FLinearColor(0.8f, 0.6f, 0.3f); break;
	case EHDBodyType::Lava: C = FLinearColor(0.7f, 0.15f, 0.05f); break;
	case EHDBodyType::Ice: C = FLinearColor(0.8f, 0.9f, 1.f); break;
	case EHDBodyType::GasGiant: C = FLinearColor(0.75f, 0.55f, 0.35f); break;
	case EHDBodyType::Moon: C = FLinearColor(0.45f, 0.45f, 0.45f); break;
	case EHDBodyType::LargeAsteroid: C = FLinearColor(0.35f, 0.3f, 0.25f); break;
	default: break;
	}
	Tint(Mesh, C);
}

// ---------------------------------------------------------------------------------------------
// AHDStationActor
// ---------------------------------------------------------------------------------------------
AHDStationActor::AHDStationActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Collision = CreateDefaultSubobject<UBoxComponent>(TEXT("Collision"));
	Collision->SetBoxExtent(FVector(50000.f, 50000.f, 50000.f)); // 1 km core
	Collision->SetCollisionProfileName(TEXT("BlockAll"));
	RootComponent = Collision;

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cone(TEXT("/Engine/BasicShapes/Cone.Cone"));
	Core = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Core"));
	Core->SetupAttachment(Collision);
	if (Cube.Succeeded()) Core->SetStaticMesh(Cube.Object);
	Core->SetRelativeScale3D(FVector(1000.f));
	Ring = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Ring"));
	Ring->SetupAttachment(Collision);
	if (Cyl.Succeeded()) Ring->SetStaticMesh(Cyl.Object);
	Ring->SetRelativeScale3D(FVector(2600.f, 2600.f, 60.f));
	Spire = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Spire"));
	Spire->SetupAttachment(Collision);
	if (Cone.Succeeded()) Spire->SetStaticMesh(Cone.Object);
	Spire->SetRelativeLocation(FVector(0.f, 0.f, 90000.f));
	Spire->SetRelativeScale3D(FVector(300.f, 300.f, 900.f));
	NoCollision(Core); NoCollision(Ring); NoCollision(Spire);
	InteractRadius = 500000.f; // dock within 5 km
	Tags.Add(TEXT("SlipTarget"));
}

void AHDStationActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDStationActor, Info);
	DOREPLIFETIME(AHDStationActor, StationIndex);
	DOREPLIFETIME(AHDStationActor, OwnerCorp);
	DOREPLIFETIME(AHDStationActor, FuelStock);
}

void AHDStationActor::InitStation(int32 InSystem, int32 InIndex, const FHDStation& InInfo)
{
	SystemIndex = InSystem;
	StationIndex = InIndex;
	Info = InInfo;
	ObjectId = InInfo.Id;
	DisplayName = InInfo.Name;
	FuelStock = InInfo.bFuelStation ? 2000.f : 500.f;
	ApplyVisual();
}

FString AHDStationActor::GetKindLabel() const
{
	if (Info.bFuelStation) return TEXT("Fuel platform");
	return FString::Printf(TEXT("Station (%s)"), HDUniverse::StationFamilyName(Info.Family));
}

FVector AHDStationActor::GetDockPoint() const
{
	return GetActorLocation() + GetActorForwardVector() * 80000.f;
}

void AHDStationActor::ApplyVisual()
{
	FLinearColor C(0.7f, 0.7f, 0.75f);
	FVector RingScale(2600.f, 2600.f, 60.f);
	switch (Info.Family)
	{
	case EHDStationFamily::AccordCivic: C = FLinearColor(0.85f, 0.9f, 1.f); break;
	case EHDStationFamily::FactionIndustrial: C = FLinearColor(0.6f, 0.55f, 0.45f); RingScale = FVector(1800.f, 1800.f, 200.f); break;
	case EHDStationFamily::CorporateModular: C = FLinearColor(0.4f, 0.6f, 0.7f); RingScale = FVector(2200.f, 1200.f, 120.f); break;
	case EHDStationFamily::PirateKitbash: C = FLinearColor(0.5f, 0.2f, 0.15f); RingScale = FVector(1500.f, 2900.f, 90.f); break;
	}
	if (Info.bFuelStation) { C = FLinearColor(0.9f, 0.6f, 0.1f); RingScale = FVector(1200.f, 1200.f, 400.f); }
	Ring->SetRelativeScale3D(RingScale);
	Tint(Core, C); Tint(Ring, C * 0.8f); Tint(Spire, C * 1.2f);
}

// ---------------------------------------------------------------------------------------------
// AHDJumpPointActor
// ---------------------------------------------------------------------------------------------
AHDJumpPointActor::AHDJumpPointActor()
{
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (Cyl.Succeeded()) Mesh->SetStaticMesh(Cyl.Object);
	NoCollision(Mesh);
	InteractRadius = 1000000.f; // 10 km
	Tags.Add(TEXT("SlipTarget"));
}

void AHDJumpPointActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDJumpPointActor, Link);
	DOREPLIFETIME(AHDJumpPointActor, DestinationName);
	DOREPLIFETIME(AHDJumpPointActor, DestinationBand);
}

void AHDJumpPointActor::InitLink(int32 InSystem, const FHDJumpLink& InLink, const FString& InDestName, EHDSecurityBand InDestBand)
{
	SystemIndex = InSystem;
	Link = InLink;
	ObjectId = InLink.Id;
	DestinationName = InDestName;
	DestinationBand = InDestBand;
	bRequiresScan = InLink.bHidden;
	Signature = InLink.Signature;
	DisplayName = InDestName.IsEmpty() ? FString(HDUniverse::LinkTypeName(InLink.Type)) : FString::Printf(TEXT("%s to %s"), HDUniverse::LinkTypeName(InLink.Type), *InDestName);
	InteractRadius = InLink.Type == EHDLinkType::LargeGate ? 2500000.f : 1000000.f;
	ApplyVisual();
}

FString AHDJumpPointActor::GetKindLabel() const
{
	return HDUniverse::LinkTypeName(Link.Type);
}

void AHDJumpPointActor::ApplyVisual()
{
	UStaticMesh* Shape = nullptr;
	FVector Scale(4000.f, 4000.f, 150.f); // 4 km ring
	FLinearColor C(0.6f, 0.8f, 1.f);
	switch (Link.Type)
	{
	case EHDLinkType::Gate: Shape = BasicMesh(TEXT("Cylinder")); break;
	case EHDLinkType::LargeGate: Shape = BasicMesh(TEXT("Cylinder")); Scale = FVector(12000.f, 12000.f, 400.f); C = FLinearColor(1.f, 0.85f, 0.4f); break;
	case EHDLinkType::Wormhole: Shape = BasicMesh(TEXT("Sphere")); Scale = FVector(2500.f); C = FLinearColor(0.05f, 0.f, 0.15f); break;
	case EHDLinkType::StellarAnomaly: Shape = BasicMesh(TEXT("Sphere")); Scale = FVector(3500.f, 3500.f, 1200.f); C = FLinearColor(0.8f, 0.2f, 1.f); break;
	case EHDLinkType::GalacticRuin: Shape = BasicMesh(TEXT("Cube")); Scale = FVector(9000.f, 1200.f, 9000.f); C = FLinearColor(0.3f, 0.3f, 0.28f); break;
	case EHDLinkType::InstanceLane: Shape = BasicMesh(TEXT("Cone")); Scale = FVector(600.f, 600.f, 1500.f); C = FLinearColor(0.3f, 1.f, 0.8f); break;
	}
	if (Shape) Mesh->SetStaticMesh(Shape);
	Mesh->SetRelativeScale3D(Scale);
	Tint(Mesh, C);
}

// ---------------------------------------------------------------------------------------------
// AHDSiteActor
// ---------------------------------------------------------------------------------------------
AHDSiteActor::AHDSiteActor()
{
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded()) Mesh->SetStaticMesh(Sphere.Object);
	NoCollision(Mesh);
	Mesh->SetRelativeScale3D(FVector(300.f)); // 150 m beacon
	InteractRadius = 3000000.f; // 30 km: the site's working area
	Tags.Add(TEXT("SlipTarget"));
}

void AHDSiteActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDSiteActor, Site);
	DOREPLIFETIME(AHDSiteActor, SiteIndex);
	DOREPLIFETIME(AHDSiteActor, Band);
	DOREPLIFETIME(AHDSiteActor, bCleared);
	DOREPLIFETIME(AHDSiteActor, TollOwner);
}

void AHDSiteActor::InitSite(int32 InSystem, int32 InIndex, const FHDSite& InSite, EHDSecurityBand InBand)
{
	SystemIndex = InSystem;
	SiteIndex = InIndex;
	Site = InSite;
	Band = InBand;
	ObjectId = InSite.Id;
	bRequiresScan = InSite.bHidden;
	Signature = InSite.Signature;
	DisplayName = InSite.HulkClass > 0 ? FString::Printf(TEXT("%s (Class %d)"), HDUniverse::SiteTypeName(InSite.Type), InSite.HulkClass) : FString(HDUniverse::SiteTypeName(InSite.Type));
	ApplyVisual();
}

FString AHDSiteActor::GetKindLabel() const
{
	return bCleared ? TEXT("Site (cleared)") : TEXT("Site");
}

void AHDSiteActor::ApplyVisual()
{
	FLinearColor C(1.f, 1.f, 1.f);
	switch (Site.Type)
	{
	case EHDSiteType::SignalFire: C = FLinearColor(1.f, 0.3f, 0.1f); break;
	case EHDSiteType::ContrabandDrift: C = FLinearColor(0.9f, 0.9f, 0.1f); break;
	case EHDSiteType::RogueNest: C = FLinearColor(0.8f, 0.1f, 0.1f); break;
	case EHDSiteType::DeadSignature: C = FLinearColor(0.4f, 0.4f, 0.6f); break;
	case EHDSiteType::Hulk: C = FLinearColor(0.6f, 0.6f, 0.6f); break;
	case EHDSiteType::DeepCore: C = FLinearColor(0.5f, 0.3f, 0.1f); break;
	case EHDSiteType::PirateBase: C = FLinearColor(0.6f, 0.f, 0.f); break;
	case EHDSiteType::SentinelVault: C = FLinearColor(0.2f, 1.f, 1.f); break;
	case EHDSiteType::DerelictStation: C = FLinearColor(0.3f, 0.3f, 0.3f); break;
	}
	Tint(Mesh, C);
}

// ---------------------------------------------------------------------------------------------
// AHDTradeLaneActor
// ---------------------------------------------------------------------------------------------
AHDTradeLaneActor::AHDTradeLaneActor()
{
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	RootComponent = Root;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cyl(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	Beam = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Beam"));
	Beam->SetupAttachment(Root);
	StartRing = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("StartRing"));
	StartRing->SetupAttachment(Root);
	EndRing = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("EndRing"));
	EndRing->SetupAttachment(Root);
	if (Cyl.Succeeded()) { Beam->SetStaticMesh(Cyl.Object); StartRing->SetStaticMesh(Cyl.Object); EndRing->SetStaticMesh(Cyl.Object); }
	NoCollision(Beam); NoCollision(StartRing); NoCollision(EndRing);
	Beam->SetAbsolute(true, true, true);
	StartRing->SetAbsolute(true, true, true);
	EndRing->SetAbsolute(true, true, true);
	InteractRadius = 2000000.f; // enter within 20 km of either end
}

void AHDTradeLaneActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDTradeLaneActor, Lane);
	DOREPLIFETIME(AHDTradeLaneActor, StartWorld);
	DOREPLIFETIME(AHDTradeLaneActor, EndWorld);
	DOREPLIFETIME(AHDTradeLaneActor, bSuppressed);
}

void AHDTradeLaneActor::InitLane(int32 InSystem, const FHDTradeLane& InLane, const FVector& Origin)
{
	SystemIndex = InSystem;
	Lane = InLane;
	ObjectId = InLane.Id;
	StartWorld = Origin + InLane.StartLocal;
	EndWorld = Origin + InLane.EndLocal;
	DisplayName = FString::Printf(TEXT("Lane %s"), *InLane.Id.ToString().Right(5));
	ApplyVisual();
}

double AHDTradeLaneActor::DistanceToEnd(const FVector& P, bool& bOutStart) const
{
	const double A = FVector::Dist(P, StartWorld), B = FVector::Dist(P, EndWorld);
	bOutStart = A <= B;
	return FMath::Min(A, B);
}

void AHDTradeLaneActor::ApplyVisual()
{
	const FVector Delta = EndWorld - StartWorld;
	const double Len = Delta.Size();
	if (Len < 1.0) return;
	// Cylinder axis is Z; rotate Z onto the lane direction.
	const FQuat Q = FQuat::FindBetweenNormals(FVector::UpVector, Delta / Len);
	Beam->SetWorldLocationAndRotation((StartWorld + EndWorld) * 0.5, Q);
	Beam->SetWorldScale3D(FVector(20.0, 20.0, Len / 100.0));
	StartRing->SetWorldLocationAndRotation(StartWorld, Q);
	StartRing->SetWorldScale3D(FVector(1500.0, 1500.0, 50.0));
	EndRing->SetWorldLocationAndRotation(EndWorld, Q);
	EndRing->SetWorldScale3D(FVector(1500.0, 1500.0, 50.0));
	const FLinearColor C = Lane.bNPC ? FLinearColor(0.3f, 0.8f, 1.f) : FLinearColor(1.f, 0.6f, 0.2f);
	Tint(Beam, C); Tint(StartRing, C); Tint(EndRing, C);
}

// ---------------------------------------------------------------------------------------------
// AHDAsteroidActor
// ---------------------------------------------------------------------------------------------
AHDAsteroidActor::AHDAsteroidActor()
{
	PrimaryActorTick.bCanEverTick = false;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (Sphere.Succeeded()) Mesh->SetStaticMesh(Sphere.Object);
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
	InteractRadius = 300000.f; // mining range 3 km
	// Rocks only matter near the player; keep them local to their field.
	SetNetCullDistanceSquared(5.0e7f * 5.0e7f);
}

void AHDAsteroidActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDAsteroidActor, FieldType);
	DOREPLIFETIME(AHDAsteroidActor, RadiusCm);
	DOREPLIFETIME(AHDAsteroidActor, ItemId);
	DOREPLIFETIME(AHDAsteroidActor, Remaining);
	DOREPLIFETIME(AHDAsteroidActor, MaxUnits);
	DOREPLIFETIME(AHDAsteroidActor, bDeepCore);
}

void AHDAsteroidActor::InitDeposit(int32 InSystem, EHDFieldType InType, const FName& InItem, float Units, float InRadiusCm, bool bInDeepCore)
{
	SystemIndex = InSystem;
	FieldType = InType;
	ItemId = InItem;
	MaxUnits = FMath::Max(1.f, Units);
	Remaining = MaxUnits;
	RadiusCm = FMath::Max(500.f, InRadiusCm);
	bDeepCore = bInDeepCore;
	DisplayName = FString::Printf(TEXT("%s (%s)"), InType == EHDFieldType::IceField ? TEXT("Ice chunk") : InType == EHDFieldType::GasCloud ? TEXT("Gas pocket") : TEXT("Asteroid"), *InItem.ToString());
	ApplyVisual();
}

float AHDAsteroidActor::Extract(float Amount)
{
	const float Taken = FMath::Clamp(Amount, 0.f, Remaining);
	Remaining -= Taken;
	if (Remaining <= 0.f) { Remaining = 0.f; }
	return Taken;
}

FString AHDAsteroidActor::GetKindLabel() const
{
	return FString::Printf(TEXT("%s %.0f/%.0f"), bDeepCore ? TEXT("Deep-core deposit") : TEXT("Deposit"), Remaining, MaxUnits);
}

void AHDAsteroidActor::ApplyVisual()
{
	Mesh->SetRelativeScale3D(FVector(RadiusCm / 50.f));
	// Gas pockets are siphoned while flying through: no hard collision.
	if (FieldType == EHDFieldType::GasCloud) { Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision); }
	FLinearColor C = FieldType == EHDFieldType::IceField ? FLinearColor(0.75f, 0.9f, 1.f) : FieldType == EHDFieldType::GasCloud ? FLinearColor(0.4f, 0.9f, 0.5f) : FLinearColor(0.35f, 0.3f, 0.27f);
	if (bDeepCore) C = FLinearColor(0.7f, 0.35f, 0.1f);
	Tint(Mesh, C);
}
