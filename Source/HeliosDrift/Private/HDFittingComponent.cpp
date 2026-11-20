#include "HDFittingComponent.h"
#include "HDShipPawn.h"
#include "HDShipMovementComponent.h"
#include "HDShipPowerComponent.h"
#include "HDLaserComponent.h"
#include "HDNavComponent.h"
#include "HDCargoComponent.h"
#include "HDWorldActors.h"
#include "HeliosDrift.h"
#include "Components/BoxComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Net/UnrealNetwork.h"

UHDFittingComponent::UHDFittingComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UHDFittingComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UHDFittingComponent, HullId);
	DOREPLIFETIME(UHDFittingComponent, Modules);
	DOREPLIFETIME(UHDFittingComponent, bRecovered);
}

void UHDFittingComponent::BeginPlay()
{
	Super::BeginPlay();
	if (!HullId.IsNone()) OnRep_Fit();
}

const FHDHullDef* UHDFittingComponent::GetHullDef() const
{
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	return C ? C->FindHull(HullId) : nullptr;
}

bool UHDFittingComponent::ComputeStats(const UHDCatalogSubsystem* Catalog, const FName& Hull, const TArray<FName>& InModules, FHDFitStats& Out, FString& OutError)
{
	Out = FHDFitStats();
	const FHDHullDef* H = Catalog ? Catalog->FindHull(Hull) : nullptr;
	if (!H) { OutError = FString::Printf(TEXT("Unknown hull '%s'"), *Hull.ToString()); return false; }
	if (InModules.Num() > H->NumSlots()) { OutError = TEXT("More modules than slots"); return false; }

	Out.PowerMax = H->Power;
	Out.StabilityMax = H->Stability;
	Out.HP = H->HP;
	Out.Shield = H->Shield;
	Out.Cargo = H->Cargo;
	Out.ShipFuel = H->ShipFuel;
	Out.JumpFuel = H->JumpFuel;
	Out.Scan = H->Scan;
	float SpeedMul = 1.f, TurnMul = 1.f, StabUsed = 0.f, Dps = 0.f, MinInterval = 10.f, WeaponRange = 0.f;
	bool bEngine = false;
	for (int32 I = 0; I < InModules.Num(); ++I)
	{
		if (InModules[I].IsNone()) continue;
		const FHDModuleDef* M = Catalog->FindModule(InModules[I]);
		if (!M) { OutError = FString::Printf(TEXT("Unknown module '%s'"), *InModules[I].ToString()); return false; }
		const EHDSlotType Slot = H->SlotTypeAt(I);
		if (M->Slot != Slot)
		{
			OutError = FString::Printf(TEXT("%s needs a %s slot (slot %d is %s)"), *M->Name, *StaticEnum<EHDSlotType>()->GetNameStringByValue((int64)M->Slot), I,
				*StaticEnum<EHDSlotType>()->GetNameStringByValue((int64)Slot));
			return false;
		}
		if (M->Size > H->MaxModuleSize) { OutError = FString::Printf(TEXT("%s is too large for a %s"), *M->Name, HDShips::ClassName(H->Class)); return false; }
		if (M->Fx(TEXT("jumpDrive")) > 0.f && !HDShips::IsCapitalSize(H->Class)) { OutError = TEXT("Jump drives fit capital-size hulls only"); return false; }

		Out.PowerUsed += M->Power;
		StabUsed += M->Stability;
		Out.PowerMax += M->Fx(TEXT("powerOut"));
		Out.HP += M->Fx(TEXT("hull"));
		Out.Shield += M->Fx(TEXT("shield"));
		if (M->Fx(TEXT("oreOnly")) > 0.f) Out.OreCargo += M->Fx(TEXT("cargo")); else Out.Cargo += M->Fx(TEXT("cargo"));
		Out.ShipFuel += M->Fx(TEXT("shipFuel"));
		Out.JumpFuel += M->Fx(TEXT("jumpFuel"));
		SpeedMul += M->Fx(TEXT("speed"));
		TurnMul += M->Fx(TEXT("turn"));
		Out.Scan += M->Fx(TEXT("scan"));
		Out.Tow = FMath::Max(Out.Tow, M->Fx(TEXT("tow")));
		Out.bJumpDrive |= M->Fx(TEXT("jumpDrive")) > 0.f;
		Out.bInterdictor |= M->Fx(TEXT("interdict")) > 0.f;
		Out.bCloak |= M->Fx(TEXT("cloak")) > 0.f;
		Out.CommandBoost = FMath::Max(Out.CommandBoost, M->Fx(TEXT("command")));
		Out.Fighters += (int32)M->Fx(TEXT("fighters"));
		Out.Mining += M->Fx(TEXT("mining")) * H->Mining;
		Out.Salvage += M->Fx(TEXT("salvage")) * H->Salvage;
		Out.Gas += M->Fx(TEXT("gas")) * H->Gas;
		bEngine |= Slot == EHDSlotType::Engine;
		const float Dmg = M->Fx(TEXT("damage"));
		if (Dmg > 0.f)
		{
			const float Interval = FMath::Max(M->Fx(TEXT("interval")), 0.05f);
			Dps += Dmg / Interval;
			MinInterval = FMath::Min(MinInterval, Interval);
			WeaponRange = FMath::Max(WeaponRange, M->Fx(TEXT("range")));
			++Out.Weapons;
		}
	}
	Out.Stability = H->Stability - StabUsed;
	if (Out.PowerUsed > Out.PowerMax + 0.01f) { OutError = FString::Printf(TEXT("Not enough power: %.0f / %.0f MW"), Out.PowerUsed, Out.PowerMax); return false; }
	if (Out.Stability < 0.f) { OutError = FString::Printf(TEXT("Unstable build: stability %.0f / %.0f"), Out.Stability, Out.StabilityMax); return false; }

	// Low stability costs handling (doc 14: you balance stability in the buildout).
	const float StabFrac = Out.StabilityMax > 0.f ? FMath::Clamp(Out.Stability / Out.StabilityMax, 0.f, 1.f) : 1.f;
	TurnMul *= FMath::Lerp(0.7f, 1.f, StabFrac);
	const float DriveMul = bEngine ? 1.f : 0.25f; // no drive: station-keeping thrusters only
	Out.Speed = H->Speed * FMath::Max(0.3f, SpeedMul) * DriveMul;
	Out.Accel = H->Accel * FMath::Max(0.3f, SpeedMul) * DriveMul;
	Out.Turn = H->Turn * FMath::Max(0.3f, TurnMul);
	Out.Interval = Dps > 0.f ? FMath::Clamp(MinInterval, 0.05f, 3.f) : 0.15f;
	Out.Damage = Dps * Out.Interval;
	Out.Range = (WeaponRange > 0.f ? WeaponRange : 5000.f) * 100.f;
	return true;
}

TArray<FName> UHDFittingComponent::SuggestFit(const UHDCatalogSubsystem* Catalog, const FName& Hull, bool bCombat)
{
	TArray<FName> Out;
	const FHDHullDef* H = Catalog ? Catalog->FindHull(Hull) : nullptr;
	if (!H) return Out;
	static const TCHAR* SizeSuffix[] = { TEXT("s"), TEXT("s"), TEXT("m"), TEXT("l"), TEXT("capital") };
	const int32 Sz = FMath::Clamp(H->MaxModuleSize, 1, 4);
	auto Pick = [Catalog](const FString& Base, int32 Size) -> FName
	{
		for (int32 S = Size; S >= 1; --S)
		{
			const FName Id(*FString::Printf(TEXT("%s_%s"), *Base, SizeSuffix[S]));
			if (Catalog->FindModule(Id)) return Id;
		}
		return NAME_None;
	};
	const bool bMiner = !bCombat && (H->Role == EHDHullRole::Miner || H->Role == EHDHullRole::IndustrialCommand);
	const bool bSalvager = !bCombat && H->Role == EHDHullRole::Salvager;
	const bool bGas = !bCombat && H->Role == EHDHullRole::Extractor;
	const bool bCapital = HDShips::IsCapitalSize(H->Class);
	int32 SupportK = 0;
	for (int32 I = 0; I < H->NumSlots(); ++I)
	{
		int32 K = 0;
		switch (H->SlotTypeAt(I, &K))
		{
		case EHDSlotType::Weapon:
			Out.Add(bMiner && K < 2 ? Pick(TEXT("mining_laser"), Sz) : bSalvager && K == 0 ? Pick(TEXT("salvager"), Sz) : Pick(K % 3 == 2 ? TEXT("railgun") : TEXT("pulse_laser"), Sz));
			break;
		case EHDSlotType::Utility:
			Out.Add(bGas && K == 0 ? Pick(TEXT("gas_siphon"), Sz) : K == 0 ? Pick(TEXT("scanner_array"), FMath::Min(Sz, 3)) : NAME_None);
			break;
		case EHDSlotType::Support:
			if (bCapital && SupportK == 0 && Catalog->FindModule(TEXT("jump_drive"))) Out.Add(FName(TEXT("jump_drive")));
			else if (bCapital && SupportK == 1 && Catalog->FindModule(TEXT("jump_fuel_bay"))) Out.Add(FName(TEXT("jump_fuel_bay")));
			else Out.Add(SupportK % 2 == 0 ? Pick(TEXT("shield_extender"), Sz) : Pick(TEXT("armor_plate"), Sz));
			++SupportK;
			break;
		case EHDSlotType::Engine:
			Out.Add(Pick(TEXT("drive"), Sz));
			break;
		case EHDSlotType::Hold:
			Out.Add(bMiner || bGas ? Pick(TEXT("ore_hold"), Sz) : bSalvager ? Pick(TEXT("salvage_bay"), Sz) : Pick(TEXT("freight_hold"), Sz));
			break;
		}
	}
	// Trim from the back (never the drive or the jump drive) until power and stability fit.
	FHDFitStats Tmp;
	FString Err;
	for (int32 Guard = 0; Guard < 64 && !ComputeStats(Catalog, Hull, Out, Tmp, Err); ++Guard)
	{
		bool bRemoved = false;
		for (int32 I = Out.Num() - 1; I >= 0; --I)
		{
			if (Out[I].IsNone() || Out[I] == FName(TEXT("jump_drive")) || H->SlotTypeAt(I) == EHDSlotType::Engine) continue;
			Out[I] = NAME_None;
			bRemoved = true;
			break;
		}
		if (!bRemoved) { Out.Reset(); break; }
	}
	return Out;
}

bool UHDFittingComponent::SetFitAuthority(const FName& Hull, const TArray<FName>& InModules, FString& OutError)
{
	FHDFitStats NewStats;
	if (!ComputeStats(UHDCatalogSubsystem::Get(this), Hull, InModules, NewStats, OutError)) return false;
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwner());
	const float HpFrac = (Ship && Ship->MaxHullHP > 0.f && HullId == Hull) ? FMath::Clamp(Ship->HullHP / Ship->MaxHullHP, 0.f, 1.f) : 1.f;
	HullId = Hull;
	Modules = InModules;
	if (const FHDHullDef* H = GetHullDef()) Modules.SetNum(H->NumSlots());
	OnRep_Fit();
	if (Ship) Ship->HullHP = Ship->MaxHullHP * HpFrac;
	return true;
}

bool UHDFittingComponent::FitModuleAuthority(int32 Slot, const FName& Module, FString& OutError)
{
	const FHDHullDef* H = GetHullDef();
	if (!H || Slot < 0 || Slot >= H->NumSlots()) { OutError = TEXT("No such slot"); return false; }
	TArray<FName> Candidate = Modules;
	Candidate.SetNum(H->NumSlots());
	Candidate[Slot] = Module;
	return SetFitAuthority(HullId, Candidate, OutError);
}

FName UHDFittingComponent::UnfitAuthority(int32 Slot)
{
	if (!Modules.IsValidIndex(Slot) || Modules[Slot].IsNone()) return NAME_None;
	const FName Old = Modules[Slot];
	TArray<FName> Candidate = Modules;
	Candidate[Slot] = NAME_None;
	FString Err;
	if (!SetFitAuthority(HullId, Candidate, Err)) return NAME_None; // removing a reactor can break power: refuse
	return Old;
}

FString UHDFittingComponent::DescribeFit() const
{
	const FHDHullDef* H = GetHullDef();
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	if (!H || !C) return TEXT("No hull");
	FString Out = FString::Printf(TEXT("%s (%s %s, %s, tier %d)%s%s | power %.0f/%.0f MW | stability %.0f/%.0f | %.0f m/s | hull %.0f | shields %.0f | cargo %.0f+%.0f ore m3"),
		*H->Name, HDShips::ClassName(H->Class), HDShips::RoleName(H->Role), *StaticEnum<EHDSupplyTier>()->GetNameStringByValue((int64)H->Supply), H->Tier,
		bRecovered ? TEXT(" [recovered]") : TEXT(""), H->Quirk.IsEmpty() ? TEXT("") : *FString::Printf(TEXT(" quirk: %s"), *H->Quirk),
		Stats.PowerUsed, Stats.PowerMax, Stats.Stability, Stats.StabilityMax, Stats.Speed, Stats.HP, Stats.Shield, Stats.Cargo, Stats.OreCargo);
	for (int32 I = 0; I < H->NumSlots(); ++I)
	{
		const FName Id = Modules.IsValidIndex(I) ? Modules[I] : NAME_None;
		const FHDModuleDef* M = C->FindModule(Id);
		Out += FString::Printf(TEXT("\n  [%d] %s: %s"), I, *StaticEnum<EHDSlotType>()->GetNameStringByValue((int64)H->SlotTypeAt(I)), M ? *M->Name : TEXT("-"));
	}
	return Out;
}

void UHDFittingComponent::OnRep_Fit()
{
	FString Err;
	if (!ComputeStats(UHDCatalogSubsystem::Get(this), HullId, Modules, Stats, Err))
	{
		// Mid-replication (hull arrived before modules) or stale data: fall back to the bare hull.
		const TArray<FName> Bare;
		if (!ComputeStats(UHDCatalogSubsystem::Get(this), HullId, Bare, Stats, Err)) return;
	}
	ApplyStats();
}

void UHDFittingComponent::ApplyStats()
{
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwner());
	const FHDHullDef* H = GetHullDef();
	if (!Ship || !H) return;
	Ship->HullClass = H->Class;
	Ship->MaxHullHP = Stats.HP;
	if (Ship->HasAuthority()) Ship->HullHP = FMath::Min(Ship->HullHP, Ship->MaxHullHP);
	if (Ship->Movement)
	{
		Ship->Movement->BaseMaxSpeed = Stats.Speed * 100.f;
		Ship->Movement->LinearAccel = Stats.Accel * 100.f;
		Ship->Movement->MaxAngularRate = FVector(Stats.Turn * 1.33f, Stats.Turn, Stats.Turn * 2.f);
	}
	if (Ship->Shields) Ship->Shields->MaxPerFacing = Stats.Shield;
	if (Ship->Laser)
	{
		Ship->Laser->Damage = Stats.Damage;
		Ship->Laser->Range = Stats.Range;
		Ship->Laser->FireInterval = Stats.Interval;
		Ship->Laser->MiningRate = Stats.Mining;
		Ship->Laser->SalvageRate = Stats.Salvage;
		Ship->Laser->MuzzleOffset = (float)H->SizeM.X * 50.f + 100.f;
	}
	if (Ship->Nav)
	{
		Ship->Nav->MaxShipFuel = Stats.ShipFuel;
		Ship->Nav->MaxJumpFuel = Stats.JumpFuel;
		Ship->Nav->ScanStrength = Stats.Scan;
		if (Ship->HasAuthority())
		{
			Ship->Nav->ShipFuel = FMath::Min(Ship->Nav->ShipFuel, Ship->Nav->MaxShipFuel);
			Ship->Nav->JumpFuel = FMath::Min(Ship->Nav->JumpFuel, Ship->Nav->MaxJumpFuel);
		}
	}
	if (Ship->Cargo)
	{
		Ship->Cargo->Capacity = Stats.Cargo;
		Ship->Cargo->OreCapacity = Stats.OreCargo;
	}
	const FVector SizeCm = H->SizeM * 100.0;
	if (Ship->Collision) Ship->Collision->SetBoxExtent(SizeCm * 0.5, true);
	if (Ship->Hull) Ship->Hull->SetRelativeScale3D(H->SizeM); // engine cube is 1 m
	if (Ship->CockpitRoot) Ship->CockpitRoot->SetRelativeLocation(FVector(SizeCm.X * 0.4, 0.0, SizeCm.Z * 0.3));
	if (Ship->Hull) AHDSpaceObject::Tint(Ship->Hull, H->Color);
	RebuildVisuals();
}

void UHDFittingComponent::RebuildVisuals()
{
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwner());
	const FHDHullDef* H = GetHullDef();
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	for (UStaticMeshComponent* P : Parts) { if (P) P->DestroyComponent(); }
	Parts.Reset();
	if (!Ship || !H || !C || !Ship->Collision || GetNetMode() == NM_DedicatedServer) return;

	const FVector S = H->SizeM * 100.0; // cm
	auto AddPart = [this, Ship](const FHDModuleDef& M, const FVector& Loc, const FRotator& Rot, const FVector& ScaleM)
	{
		UStaticMeshComponent* P = NewObject<UStaticMeshComponent>(Ship);
		P->SetupAttachment(Ship->Collision);
		P->SetStaticMesh(AHDSpaceObject::BasicMesh(M.Shape.IsNone() ? TEXT("Cube") : *M.Shape.ToString()));
		P->SetRelativeLocationAndRotation(Loc, Rot);
		P->SetRelativeScale3D(ScaleM);
		P->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		P->SetCastShadow(false);
		P->SetOwnerNoSee(true);
		P->RegisterComponent();
		AHDSpaceObject::Tint(P, M.Color);
		Parts.Add(P);
	};
	const FRotator Forward(-90.f, 0.f, 0.f); // basic cylinders/cones run along Z: point them along +X
	for (int32 I = 0; I < Modules.Num() && I < H->NumSlots(); ++I)
	{
		const FHDModuleDef* M = C->FindModule(Modules[I]);
		if (!M) continue;
		int32 K = 0;
		const EHDSlotType T = H->SlotTypeAt(I, &K);
		const FVector V = M->VisualScale;
		switch (T)
		{
		case EHDSlotType::Weapon:
		{
			// Hardpoints in pairs along the dorsal spine, front to back.
			const double Side = (K % 2 == 0 ? -1.0 : 1.0) * S.Y * 0.3;
			const double X = S.X * (0.35 - 0.1 * (K / 2));
			AddPart(*M, FVector(X, Side, S.Z * 0.5 + V.X * 50.0), Forward, V);
			break;
		}
		case EHDSlotType::Utility:
			AddPart(*M, FVector(S.X * (-0.05 - 0.12 * K), 0.0, S.Z * 0.5 + V.Z * 50.0), FRotator::ZeroRotator, V);
			break;
		case EHDSlotType::Support:
		{
			// Side pods: each one makes the silhouette wider.
			const double Side = (K % 2 == 0 ? -1.0 : 1.0) * (S.Y * 0.5 + V.Y * 50.0);
			AddPart(*M, FVector(S.X * (0.15 - 0.2 * (K / 2)), Side, 0.0), M->Shape == FName(TEXT("Cube")) ? FRotator::ZeroRotator : Forward, V);
			break;
		}
		case EHDSlotType::Engine:
		{
			const int32 Nozzles = FMath::Clamp(1 + (int32)(H->SizeM.Y / 40.0), 1, 4);
			const double Spacing = Nozzles > 1 ? S.Y * 0.6 / (Nozzles - 1) : 0.0;
			const FVector Scale = V * FMath::Max(1.0, H->SizeM.Z / 8.0);
			for (int32 N = 0; N < Nozzles; ++N)
			{
				const double Y = (N - (Nozzles - 1) * 0.5) * Spacing;
				AddPart(*M, FVector(-S.X * 0.5 - Scale.Z * 25.0, Y, 0.0), Forward, Scale);
			}
			break;
		}
		case EHDSlotType::Hold:
			// A hold replaces the rear section: a bulkier box over the aft third.
			AddPart(*M, FVector(-S.X * 0.3, 0.0, 0.0), FRotator::ZeroRotator, FVector(H->SizeM.X * 0.38, H->SizeM.Y * 1.15, H->SizeM.Z * 1.2));
			break;
		}
	}
}
