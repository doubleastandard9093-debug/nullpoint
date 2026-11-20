#include "HDCatalog.h"
#include "HeliosDrift.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedPtr<FJsonObject> LoadJsonFile(const TCHAR* Name)
	{
		const FString Path = FPaths::Combine(FPaths::ProjectContentDir(), TEXT("Data"), Name);
		FString Text;
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			UE_LOG(LogHelios, Warning, TEXT("Catalog: %s not found"), *Path);
			return nullptr;
		}
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
		{
			UE_LOG(LogHelios, Error, TEXT("Catalog: %s is not valid JSON"), *Path);
			return nullptr;
		}
		return Root;
	}

	float Num(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, float Default)
	{
		double V = Default;
		return O->TryGetNumberField(Key, V) ? (float)V : Default;
	}

	FString Str(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
	{
		FString V;
		O->TryGetStringField(Key, V);
		return V;
	}

	FVector Vec(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const FVector& Default)
	{
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (!O->TryGetArrayField(Key, A) || !A || A->Num() < 3) return Default;
		return FVector((*A)[0]->AsNumber(), (*A)[1]->AsNumber(), (*A)[2]->AsNumber());
	}

	FLinearColor Col(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const FLinearColor& Default)
	{
		const FVector V = Vec(O, Key, FVector(Default.R, Default.G, Default.B));
		return FLinearColor((float)V.X, (float)V.Y, (float)V.Z);
	}

	TArray<TSharedPtr<FJsonObject>> Objects(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key)
	{
		TArray<TSharedPtr<FJsonObject>> Out;
		const TArray<TSharedPtr<FJsonValue>>* A = nullptr;
		if (Root.IsValid() && Root->TryGetArrayField(Key, A) && A)
		{
			for (const TSharedPtr<FJsonValue>& V : *A)
			{
				const TSharedPtr<FJsonObject>* Obj = nullptr;
				if (V.IsValid() && V->TryGetObject(Obj) && Obj) Out.Add(*Obj);
			}
		}
		return Out;
	}
}

EHDSlotType FHDHullDef::SlotTypeAt(int32 Index, int32* OutIndexInType) const
{
	int32 Base = 0;
	for (int32 T = 0; T < SlotCounts.Num() && T < 5; ++T)
	{
		if (Index < Base + SlotCounts[T])
		{
			if (OutIndexInType) *OutIndexInType = Index - Base;
			return (EHDSlotType)T;
		}
		Base += SlotCounts[T];
	}
	if (OutIndexInType) *OutIndexInType = 0;
	return EHDSlotType::Hold;
}

UHDCatalogSubsystem* UHDCatalogSubsystem::Get(const UObject* WorldContext)
{
	if (!WorldContext || !GEngine) return nullptr;
	UWorld* World = GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull);
	UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
	return GI ? GI->GetSubsystem<UHDCatalogSubsystem>() : nullptr;
}

void UHDCatalogSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Load();
}

const FHDRecipeDef* UHDCatalogSubsystem::FindRecipeFor(const FName& Output) const
{
	for (const TPair<FName, FHDRecipeDef>& P : Recipes)
	{
		if (P.Value.Output == Output) return &P.Value;
	}
	return nullptr;
}

void UHDCatalogSubsystem::Load()
{
	Items.Reset();
	Hulls.Reset();
	Modules.Reset();
	Recipes.Reset();
	StarterModules.Reset();
	StarterStorage.Reset();

	// ---- Items ----
	for (const TSharedPtr<FJsonObject>& O : Objects(LoadJsonFile(TEXT("items.json")), TEXT("items")))
	{
		FHDItemDef D;
		D.Id = FName(*Str(O, TEXT("id")));
		D.Name = Str(O, TEXT("name"));
		D.Category = FName(*Str(O, TEXT("category")));
		D.Volume = Num(O, TEXT("volume"), 1.f);
		D.Price = (int32)Num(O, TEXT("price"), 1.f);
		bool bLegal = true;
		O->TryGetBoolField(TEXT("legal"), bLegal);
		D.bLegal = bLegal;
		D.Tier = (int32)Num(O, TEXT("tier"), 1.f);
		if (!D.Id.IsNone()) Items.Add(D.Id, D);
	}

	// ---- Modules ----
	for (const TSharedPtr<FJsonObject>& O : Objects(LoadJsonFile(TEXT("modules.json")), TEXT("modules")))
	{
		FHDModuleDef D;
		D.Id = FName(*Str(O, TEXT("id")));
		D.Name = Str(O, TEXT("name"));
		ParseEnum<EHDSlotType>(Str(O, TEXT("slot")), D.Slot);
		D.Size = (int32)Num(O, TEXT("size"), 1.f);
		D.Tier = (int32)Num(O, TEXT("tier"), 1.f);
		D.Power = Num(O, TEXT("power"), 0.f);
		D.Stability = Num(O, TEXT("stability"), 0.f);
		D.Price = (int32)Num(O, TEXT("price"), 0.f);
		const TSharedPtr<FJsonObject>* Vis = nullptr;
		if (O->TryGetObjectField(TEXT("visual"), Vis) && Vis)
		{
			D.Shape = FName(*Str(*Vis, TEXT("shape")));
			D.VisualScale = Vec(*Vis, TEXT("scale"), FVector::OneVector);
			D.Color = Col(*Vis, TEXT("color"), FLinearColor::Gray);
		}
		const TSharedPtr<FJsonObject>* Fx = nullptr;
		if (O->TryGetObjectField(TEXT("effects"), Fx) && Fx)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Fx)->Values)
			{
				if (!P.Value.IsValid()) continue;
				if (P.Value->Type == EJson::Boolean) D.Effects.Add(FName(*P.Key), P.Value->AsBool() ? 1.f : 0.f);
				else if (P.Value->Type == EJson::Number) D.Effects.Add(FName(*P.Key), (float)P.Value->AsNumber());
			}
		}
		if (!D.Id.IsNone()) Modules.Add(D.Id, D);
	}

	// ---- Hulls + starter kit ----
	const TSharedPtr<FJsonObject> HullRoot = LoadJsonFile(TEXT("hulls.json"));
	for (const TSharedPtr<FJsonObject>& O : Objects(HullRoot, TEXT("hulls")))
	{
		FHDHullDef D;
		D.Id = FName(*Str(O, TEXT("id")));
		D.Name = Str(O, TEXT("name"));
		ParseEnum<EHDHullClass>(Str(O, TEXT("cls")), D.Class);
		ParseEnum<EHDHullRole>(Str(O, TEXT("role")), D.Role);
		ParseEnum<EHDSupplyTier>(Str(O, TEXT("supply")), D.Supply);
		D.Tier = (int32)Num(O, TEXT("tier"), 1.f);
		const FString Fac = Str(O, TEXT("faction"));
		D.Faction = Fac.IsEmpty() ? NAME_None : FName(*Fac);
		D.Price = (int32)Num(O, TEXT("price"), 0.f);
		D.Quirk = Str(O, TEXT("quirk"));
		D.Speed = Num(O, TEXT("speed"), 300.f);
		D.Accel = Num(O, TEXT("accel"), 150.f);
		D.Turn = Num(O, TEXT("turn"), 45.f);
		D.SizeM = Vec(O, TEXT("size"), FVector(40.f, 18.f, 8.f));
		D.HP = Num(O, TEXT("hp"), 1000.f);
		D.Shield = Num(O, TEXT("shield"), 500.f);
		D.Power = Num(O, TEXT("power"), 100.f);
		D.Stability = Num(O, TEXT("stability"), 100.f);
		D.Cargo = Num(O, TEXT("cargo"), 60.f);
		D.MaxModuleSize = (int32)Num(O, TEXT("maxModuleSize"), 1.f);
		D.ShipFuel = Num(O, TEXT("shipFuel"), 60.f);
		D.JumpFuel = Num(O, TEXT("jumpFuel"), 0.f);
		D.Mining = Num(O, TEXT("mining"), 1.f);
		D.Salvage = Num(O, TEXT("salvage"), 1.f);
		D.Gas = Num(O, TEXT("gas"), 1.f);
		D.Scan = Num(O, TEXT("scan"), 1.f);
		D.Color = Col(O, TEXT("color"), D.Color);
		D.SlotCounts.Init(0, 5);
		const TSharedPtr<FJsonObject>* Slots = nullptr;
		if (O->TryGetObjectField(TEXT("slots"), Slots) && Slots)
		{
			D.SlotCounts[0] = (int32)Num(*Slots, TEXT("weapon"), 0.f);
			D.SlotCounts[1] = (int32)Num(*Slots, TEXT("utility"), 0.f);
			D.SlotCounts[2] = (int32)Num(*Slots, TEXT("support"), 0.f);
			D.SlotCounts[3] = (int32)Num(*Slots, TEXT("engine"), 1.f);
			D.SlotCounts[4] = (int32)Num(*Slots, TEXT("hold"), 0.f);
		}
		if (!D.Id.IsNone()) Hulls.Add(D.Id, D);
	}
	if (HullRoot.IsValid())
	{
		const TSharedPtr<FJsonObject>* Starter = nullptr;
		if (HullRoot->TryGetObjectField(TEXT("starter"), Starter) && Starter)
		{
			StarterHull = FName(*Str(*Starter, TEXT("hull")));
			const TArray<TSharedPtr<FJsonValue>>* Mods = nullptr;
			if ((*Starter)->TryGetArrayField(TEXT("modules"), Mods) && Mods)
			{
				for (const TSharedPtr<FJsonValue>& V : *Mods)
				{
					const FString S = V.IsValid() ? V->AsString() : FString();
					StarterModules.Add(S.IsEmpty() ? NAME_None : FName(*S));
				}
			}
			const TSharedPtr<FJsonObject>* Store = nullptr;
			if ((*Starter)->TryGetObjectField(TEXT("storage"), Store) && Store)
			{
				for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*Store)->Values)
				{
					if (P.Value.IsValid()) StarterStorage.Add(FName(*P.Key), (int32)P.Value->AsNumber());
				}
			}
		}
	}

	// ---- Recipes ----
	for (const TSharedPtr<FJsonObject>& O : Objects(LoadJsonFile(TEXT("recipes.json")), TEXT("recipes")))
	{
		FHDRecipeDef D;
		D.Id = FName(*Str(O, TEXT("id")));
		D.Output = FName(*Str(O, TEXT("output")));
		D.Qty = (int32)Num(O, TEXT("qty"), 1.f);
		D.Seconds = Num(O, TEXT("seconds"), 10.f);
		D.Facility = FName(*Str(O, TEXT("facility")));
		const TSharedPtr<FJsonObject>* In = nullptr;
		if (O->TryGetObjectField(TEXT("inputs"), In) && In)
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& P : (*In)->Values)
			{
				if (P.Value.IsValid()) D.Inputs.Add(FName(*P.Key), (int32)P.Value->AsNumber());
			}
		}
		if (!D.Id.IsNone()) Recipes.Add(D.Id, D);
	}

	if (Hulls.Num() == 0 || Modules.Num() == 0) AddFallback();
	if (!Hulls.Contains(StarterHull) && Hulls.Num() > 0)
	{
		TArray<FName> Keys;
		Hulls.GetKeys(Keys);
		StarterHull = Keys[0];
	}
	UE_LOG(LogHelios, Log, TEXT("Catalog: %d hulls, %d modules, %d items, %d recipes"), Hulls.Num(), Modules.Num(), Items.Num(), Recipes.Num());
}

void UHDCatalogSubsystem::AddFallback()
{
	UE_LOG(LogHelios, Warning, TEXT("Catalog: using built-in fallback (run tools/gen_catalog.py to create Content/Data)"));
	FHDHullDef H;
	H.Id = TEXT("lance");
	H.Name = TEXT("Lance");
	H.SlotCounts = { 2, 1, 1, 1, 1 };
	Hulls.Add(H.Id, H);
	FHDModuleDef L;
	L.Id = TEXT("pulse_laser_s");
	L.Name = TEXT("Pulse Laser S");
	L.Slot = EHDSlotType::Weapon;
	L.Power = 8.f;
	L.Stability = 6.f;
	L.Shape = TEXT("Cylinder");
	L.VisualScale = FVector(0.4f, 0.4f, 2.f);
	L.Effects.Add(TEXT("damage"), 40.f);
	L.Effects.Add(TEXT("range"), 5000.f);
	L.Effects.Add(TEXT("interval"), 0.15f);
	Modules.Add(L.Id, L);
	FHDModuleDef M = L;
	M.Id = TEXT("mining_laser_s");
	M.Name = TEXT("Mining Laser S");
	M.Shape = TEXT("Cone");
	M.Effects.Reset();
	M.Effects.Add(TEXT("mining"), 2.f);
	M.Effects.Add(TEXT("range"), 3000.f);
	Modules.Add(M.Id, M);
	const TArray<const FHDModuleDef*> Defs = { &L, &M };
	for (const FHDModuleDef* Def : Defs)
	{
		FHDItemDef I;
		I.Id = Def->Id;
		I.Name = Def->Name;
		I.Category = TEXT("Module");
		I.Volume = 2.f;
		I.Price = 8000;
		Items.Add(I.Id, I);
	}
	const TCHAR* Ores[] = { TEXT("ore_ferrite"), TEXT("ore_silicate"), TEXT("ore_crystal"), TEXT("ore_exotic"), TEXT("ice_water"), TEXT("ice_volatile"), TEXT("gas_nebular"), TEXT("gas_volatile") };
	for (const TCHAR* O : Ores)
	{
		FHDItemDef I;
		I.Id = O;
		I.Name = O;
		I.Category = FString(O).StartsWith(TEXT("ore")) ? TEXT("Ore") : FString(O).StartsWith(TEXT("ice")) ? TEXT("Ice") : TEXT("Gas");
		I.Volume = 0.1f;
		I.Price = 20;
		Items.Add(I.Id, I);
	}
	StarterHull = H.Id;
	StarterModules = { L.Id, M.Id, NAME_None, NAME_None, NAME_None, NAME_None };
}
