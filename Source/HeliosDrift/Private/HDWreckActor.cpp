#include "HDWreckActor.h"
#include "HDShipPawn.h"
#include "HDShipTypes.h"
#include "HDCatalog.h"
#include "HDPlayerState.h"
#include "HDPlayerController.h"
#include "HDCargoComponent.h"
#include "HDSystemDirector.h"
#include "HDGameMode.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "UObject/ConstructorHelpers.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"

namespace
{
	constexpr double DayS = 86400.0;
	/** Share of remaining modules recovered by one pass at each grade (doc 16: G-A yields 80-95%). */
	float ModuleChance(int32 Grade)
	{
		switch (Grade)
		{
		case 0: return FMath::FRandRange(0.80f, 0.95f);
		case 1: return 0.6f;
		case 2: return 0.4f;
		case 3: return 0.2f;
		default: return 0.05f;
		}
	}
	/** Open pays ~2x Solo (doc 16). */
	float FlagYield(EHDHulkFlag F) { return F == EHDHulkFlag::Open ? 2.f : F == EHDHulkFlag::Corp ? 1.4f : 1.f; }
	/** Decay window after breach: Solo is a short stability clock, Open a long one. */
	double FlagWindow(EHDHulkFlag F) { return F == EHDHulkFlag::Open ? 4.0 * 3600.0 : F == EHDHulkFlag::Corp ? 2.0 * 3600.0 : 40.0 * 60.0; }
}

AHDWreckActor::AHDWreckActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.f;
	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> Cube(TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (Cube.Succeeded()) Mesh->SetStaticMesh(Cube.Object);
	Mesh->SetCollisionProfileName(TEXT("BlockAll"));
	InteractRadius = 300000.f; // salvage / loot within 3 km
	SetNetCullDistanceSquared(5.0e7f * 5.0e7f);
}

void AHDWreckActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDWreckActor, HullId);
	DOREPLIFETIME(AHDWreckActor, Grade);
	DOREPLIFETIME(AHDWreckActor, HulkClass);
	DOREPLIFETIME(AHDWreckActor, Age);
	DOREPLIFETIME(AHDWreckActor, bStationDerelict);
	DOREPLIFETIME(AHDWreckActor, Flag);
	DOREPLIFETIME(AHDWreckActor, FlagHolder);
	DOREPLIFETIME(AHDWreckActor, ModulesLeft);
	DOREPLIFETIME(AHDWreckActor, PassProgress);
	DOREPLIFETIME(AHDWreckActor, VictimPilot);
	DOREPLIFETIME(AHDWreckActor, bCrate);
	DOREPLIFETIME(AHDWreckActor, bNoRebuild);
}

const TCHAR* AHDWreckActor::GradeName(int32 G)
{
	static const TCHAR* Names[] = { TEXT("G-A"), TEXT("G-B"), TEXT("G-C"), TEXT("G-D"), TEXT("G-E"), TEXT("stripped") };
	return Names[FMath::Clamp(G, 0, 5)];
}

FString AHDWreckActor::FlagName(EHDHulkFlag F)
{
	switch (F)
	{
	case EHDHulkFlag::Solo: return TEXT("Solo");
	case EHDHulkFlag::Open: return TEXT("Open");
	case EHDHulkFlag::Corp: return TEXT("Corp");
	default: return TEXT("unflagged");
	}
}

FString AHDWreckActor::GetKindLabel() const
{
	if (bCrate) return FString::Printf(TEXT("Cargo container, %d stacks"), Loot.Num());
	const TCHAR* AgeName = Age == EHDWreckAge::Fresh ? TEXT("Wreck") : Age == EHDWreckAge::Cooled ? TEXT("Hulk") : TEXT("Ancient derelict");
	FString S = FString::Printf(TEXT("%s%s %s, %d modules"), bStationDerelict ? TEXT("Station ") : TEXT(""), AgeName, GradeName(Grade), ModulesLeft);
	if (HulkClass > 0) S += FString::Printf(TEXT(", Class %d"), HulkClass);
	if (Flag != EHDHulkFlag::None) S += FString::Printf(TEXT(", %s flag (%s)"), *FlagName(Flag), *FlagHolder);
	return S;
}

int32 AHDWreckActor::SizeFactor() const
{
	if (bStationDerelict) return 6;
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	const FHDHullDef* H = C ? C->FindHull(HullId) : nullptr;
	if (!H) return 1 + HulkClass;
	switch (H->Class)
	{
	case EHDHullClass::Fighter: case EHDHullClass::Corvette: case EHDHullClass::Frigate: return 1;
	case EHDHullClass::Destroyer: return 2;
	case EHDHullClass::Cruiser: case EHDHullClass::Battlecruiser: return 3;
	case EHDHullClass::Battleship: return 4;
	case EHDHullClass::Supercapital: return 6;
	default: return 5;
	}
}

void AHDWreckActor::InitWreck(int32 InSystem, const FName& InHull, const TArray<FName>& InModules, int32 InGrade, int32 InHulkClass, EHDWreckAge InAge, bool bInStation)
{
	SystemIndex = InSystem;
	HullId = InHull;
	Modules.Reset();
	for (const FName& M : InModules) { if (!M.IsNone()) Modules.Add(M); }
	ModulesLeft = Modules.Num();
	Grade = FMath::Clamp(InGrade, 0, 4);
	HulkClass = InHulkClass;
	Age = InAge;
	bStationDerelict = bInStation;
	LastTouchedUnix = AHDPlayerState::NowUnix();
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	const FHDHullDef* H = C ? C->FindHull(HullId) : nullptr;
	DisplayName = bStationDerelict ? TEXT("Derelict station") : FString::Printf(TEXT("%s of a %s"), InAge == EHDWreckAge::Fresh ? TEXT("Wreck") : TEXT("Hulk"), H ? *H->Name : TEXT("ship"));
	ObjectId = FName(*FString::Printf(TEXT("wreck-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Short)));
	ApplyVisual();
}

void AHDWreckActor::ApplyVisual()
{
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	const FHDHullDef* H = C ? C->FindHull(HullId) : nullptr;
	FVector Size = H ? H->SizeM : FVector(40.f, 18.f, 8.f);
	if (bStationDerelict) Size = FVector(600.f, 400.f, 300.f);
	if (bCrate) Size = FVector(5.f, 3.f, 3.f);
	if (Mesh)
	{
		Mesh->SetRelativeScale3D(Size * 0.9);
		Tint(Mesh, Age == EHDWreckAge::Ancient ? FLinearColor(0.25f, 0.28f, 0.3f) : FLinearColor(0.3f, 0.18f, 0.1f));
	}
}

bool AHDWreckActor::IsFlagAllowed(EHDHulkFlag F) const
{
	switch (HulkClass)
	{
	case 1: return F == EHDHulkFlag::Solo || F == EHDHulkFlag::Corp;   // PVE by rule
	case 2: return F != EHDHulkFlag::None;
	case 3: return F == EHDHulkFlag::Open || F == EHDHulkFlag::Corp;
	case 4: return F == EHDHulkFlag::Open;                              // contested, no private claim
	default: return false;                                               // field wrecks: no breach flags
	}
}

bool AHDWreckActor::Breach(AHDShipPawn* By, EHDHulkFlag F, FString& OutWhy)
{
	if (HulkClass <= 0) { OutWhy = TEXT("Field wrecks have no interior to breach; salvage or loot directly."); return false; }
	if (Flag != EHDHulkFlag::None) { OutWhy = FString::Printf(TEXT("Already breached: %s flag"), *FlagName(Flag)); return false; }
	if (!IsFlagAllowed(F)) { OutWhy = FString::Printf(TEXT("Class %d hulks don't allow a %s flag."), HulkClass, *FlagName(F)); return false; }
	const AHDPlayerState* PS = By ? By->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!PS) return false;
	if (F == EHDHulkFlag::Corp && PS->Corp.IsNone()) { OutWhy = TEXT("You're not in a company."); return false; }
	Flag = F;
	FlagHolder = F == EHDHulkFlag::Corp ? PS->Corp.ToString() : PS->PilotId;
	FlagCorp = PS->Corp;
	CollapseAtUnix = AHDPlayerState::NowUnix() + FlagWindow(F);
	// The flag is broadcast at scanner range (doc 16).
	const double Range = 12.0 * 1.0e10;
	for (TActorIterator<AHDShipPawn> It(GetWorld()); It; ++It)
	{
		if (It->SystemIndex != SystemIndex || FVector::Dist(It->GetActorLocation(), GetActorLocation()) > Range) continue;
		if (AHDPlayerController* PC = Cast<AHDPlayerController>(It->GetController()))
		{
			PC->Notify(FString::Printf(TEXT("Hulk breached: %s, Class %d, %s flag by %s"), *DisplayName, HulkClass, *FlagName(F), *FlagHolder));
		}
	}
	return true;
}

bool AHDWreckActor::CanWork(const AHDShipPawn* By, FString& OutWhy) const
{
	const AHDPlayerState* PS = By ? By->GetPlayerState<AHDPlayerState>() : nullptr;
	if (Grade > 4) { OutWhy = TEXT("Picked clean"); return false; }
	if (HulkClass > 0 && Flag == EHDHulkFlag::None) { OutWhy = TEXT("Breach it first: HDCmd breach solo|open|corp"); return false; }
	if (Flag == EHDHulkFlag::Solo && (!PS || PS->PilotId != FlagHolder)) { OutWhy = TEXT("Solo flag: someone else holds this hulk"); return false; }
	if (Flag == EHDHulkFlag::Corp && (!PS || PS->Corp != FlagCorp)) { OutWhy = TEXT("Corp flag: company members only"); return false; }
	return true;
}

float AHDWreckActor::PassCost() const
{
	return 8.f * SizeFactor() * (1.f + 0.5f * HulkClass);
}

FString AHDWreckActor::Work(AHDShipPawn* By, float Amount)
{
	FString Why;
	if (!CanWork(By, Why)) return Why;
	LastTouchedUnix = AHDPlayerState::NowUnix();
	PassProgress += Amount / PassCost();
	if (PassProgress < 1.f) return FString();
	PassProgress = 0.f;

	// One full pass: modules, materials, then the grade drops a step (a hulk looted twice is a worse hulk).
	UHDCargoComponent* Cargo = By ? By->Cargo.Get() : nullptr;
	const float Mul = FlagYield(Flag);
	TArray<FString> Got;
	auto Give = [&Got, Cargo](const FName& Item, int32 Qty)
	{
		if (Qty <= 0) return;
		const int32 N = Cargo ? Cargo->Add(Item, Qty) : 0;
		if (N > 0) Got.Add(FString::Printf(TEXT("%s x%d"), *Item.ToString(), N));
		if (N < Qty) Got.Add(TEXT("(hold full)"));
	};
	const float Chance = ModuleChance(Grade);
	for (int32 I = Modules.Num() - 1; I >= 0; --I)
	{
		if (FMath::FRand() < Chance)
		{
			Give(Modules[I], 1);
			Modules.RemoveAt(I);
		}
	}
	ModulesLeft = Modules.Num();
	const int32 Size = SizeFactor();
	const float GradeMul = 1.f - 0.18f * Grade;
	if (bStationDerelict)
	{
		Give(TEXT("habitat_block"), FMath::RandRange(0, 2));
		Give(TEXT("dock_section"), FMath::RandRange(0, 1));
	}
	Give(TEXT("salvage_scrap"), FMath::RoundToInt(10.f * Size * Mul));
	Give(TEXT("salvage_alloy"), FMath::RoundToInt(4.f * Size * GradeMul * Mul));
	Give(TEXT("salvage_electronics"), FMath::RoundToInt(2.f * Size * GradeMul * Mul));
	// Sealed containers: only with an Open flag (full room count) or on ancient derelicts.
	if ((Flag == EHDHulkFlag::Open || Age == EHDWreckAge::Ancient) && FMath::FRand() < 0.15f * Mul) Give(TEXT("ancient_relic"), 1);
	++TimesWorked;
	Grade = FMath::Min(Grade + 1, 5);
	const FString Msg = FString::Printf(TEXT("Salvage pass complete: %s. Now %s."), Got.Num() ? *FString::Join(Got, TEXT(", ")) : TEXT("nothing usable"), GradeName(Grade));
	if (Grade >= 5 && !TowedBy.IsValid()) { SetLifeSpan(30.f); }
	return Msg;
}

void AHDWreckActor::MakeCrate(const FString& Name, bool bInChart)
{
	bCrate = true;
	bNoRebuild = true;
	bChart = bInChart;
	Grade = 5; // nothing to salvage: loot only
	Modules.Reset();
	ModulesLeft = 0;
	DisplayName = Name;
	ApplyVisual();
}

int64 AHDWreckActor::RebuildCost() const
{
	if (bStationDerelict || bNoRebuild || bCrate || Grade > 3) return -1;
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	const FHDHullDef* H = C ? C->FindHull(HullId) : nullptr;
	if (!H) return -1;
	static const float Frac[] = { 0.30f, 0.45f, 0.60f, 0.80f };
	return (int64)(H->Price * Frac[FMath::Clamp(Grade, 0, 3)]);
}

void AHDWreckActor::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (!HasAuthority()) return;
	const double Now = AHDPlayerState::NowUnix();
	// Untouched wrecks lose a grade per day.
	if (Grade < 5 && Now - LastTouchedUnix >= DayS)
	{
		++Grade;
		LastTouchedUnix = Now;
		if (Age == EHDWreckAge::Fresh) Age = EHDWreckAge::Cooled;
	}
	if (CollapseAtUnix > 0.0 && Now > CollapseAtUnix) { Destroy(); return; }
	// Towing: follow behind the tug, across jumps too.
	if (AHDShipPawn* Tug = TowedBy.Get())
	{
		if (Tug->SystemIndex != SystemIndex)
		{
			if (AHDSystemDirector* D = AHDSystemDirector::Get(this)) D->AddTransient(Tug->SystemIndex, this);
			SystemIndex = Tug->SystemIndex;
		}
		const FVector Behind = Tug->GetActorLocation() - Tug->GetActorForwardVector() * (Tug->GetSimpleCollisionRadius() + GetSimpleCollisionRadius() + 5000.f);
		SetActorLocation(FMath::VInterpTo(GetActorLocation(), Behind, DeltaSeconds, 2.f));
		if (FVector::DistSquared(GetActorLocation(), Behind) > 1.0e7 * 1.0e7) SetActorLocation(Behind); // after a jump
	}
}
