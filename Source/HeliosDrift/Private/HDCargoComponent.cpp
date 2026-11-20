#include "HDCargoComponent.h"
#include "HDCatalog.h"
#include "Net/UnrealNetwork.h"

UHDCargoComponent::UHDCargoComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UHDCargoComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME_CONDITION(UHDCargoComponent, Items, COND_OwnerOnly);
	DOREPLIFETIME(UHDCargoComponent, Capacity);
	DOREPLIFETIME(UHDCargoComponent, OreCapacity);
}

bool UHDCargoComponent::IsRaw(const FName& Item) const
{
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	const FHDItemDef* D = C ? C->FindItem(Item) : nullptr;
	return D && D->IsRaw();
}

float UHDCargoComponent::Vol(const FName& Item) const
{
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	return FMath::Max(0.001f, C ? C->GetVolume(Item) : 1.f);
}

int32 UHDCargoComponent::Count(const FName& Item) const
{
	int32 N = 0;
	for (const FHDItemStack& S : Items) { if (S.Item == Item) N += S.Qty; }
	return N;
}

void UHDCargoComponent::GetUsed(float& OutGeneral, float& OutOre) const
{
	float Raw = 0.f, Other = 0.f;
	for (const FHDItemStack& S : Items)
	{
		const float V = Vol(S.Item) * S.Qty;
		if (IsRaw(S.Item)) Raw += V; else Other += V;
	}
	OutOre = FMath::Min(Raw, OreCapacity);
	OutGeneral = Other + FMath::Max(0.f, Raw - OreCapacity);
}

int32 UHDCargoComponent::SpaceFor(const FName& Item) const
{
	float G = 0.f, O = 0.f;
	GetUsed(G, O);
	float Free = FMath::Max(0.f, Capacity - G);
	if (IsRaw(Item)) Free += FMath::Max(0.f, OreCapacity - O);
	return FMath::FloorToInt(Free / Vol(Item) + 1.0e-4f);
}

int32 UHDCargoComponent::Add(const FName& Item, int32 Qty, uint8 Flags)
{
	if (Item.IsNone() || Qty <= 0) return 0;
	const int32 N = FMath::Min(Qty, SpaceFor(Item));
	if (N <= 0) return 0;
	for (FHDItemStack& S : Items)
	{
		if (S.Item == Item && S.Flags == Flags) { S.Qty += N; return N; }
	}
	FHDItemStack S;
	S.Item = Item;
	S.Qty = N;
	S.Flags = Flags;
	Items.Add(S);
	return N;
}

int32 UHDCargoComponent::Remove(const FName& Item, int32 Qty, uint8* OutFlags)
{
	int32 Left = Qty;
	for (int32 I = Items.Num() - 1; I >= 0 && Left > 0; --I)
	{
		FHDItemStack& S = Items[I];
		if (S.Item != Item) continue;
		const int32 Take = FMath::Min(Left, S.Qty);
		S.Qty -= Take;
		Left -= Take;
		if (OutFlags) *OutFlags |= S.Flags;
		if (S.Qty <= 0) Items.RemoveAt(I);
	}
	return Qty - Left;
}

bool UHDCargoComponent::HasFlagged(uint8 Mask) const
{
	for (const FHDItemStack& S : Items) { if (S.Flags & Mask) return true; }
	return false;
}

int64 UHDCargoComponent::GetValue() const
{
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	int64 V = 0;
	for (const FHDItemStack& S : Items) { V += (int64)(C ? C->GetPrice(S.Item) : 1) * S.Qty; }
	return V;
}

FString UHDCargoComponent::Describe() const
{
	const UHDCatalogSubsystem* C = UHDCatalogSubsystem::Get(this);
	float G = 0.f, O = 0.f;
	GetUsed(G, O);
	FString Out = FString::Printf(TEXT("Hold %.0f/%.0f m3, ore hold %.0f/%.0f m3"), G, Capacity, O, OreCapacity);
	for (const FHDItemStack& S : Items)
	{
		Out += FString::Printf(TEXT(" | %s x%d%s"), C ? *C->GetName(S.Item) : *S.Item.ToString(), S.Qty,
			(S.Flags & HDItemFlags::Stolen) ? TEXT(" [stolen]") : (S.Flags & HDItemFlags::Recovered) ? TEXT(" [recovered]") : TEXT(""));
	}
	return Out;
}
