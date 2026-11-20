#include "HDGameMode.h"
#include "HDEconomy.h"
#include "HDTerritory.h"
#include "HDFittingComponent.h"
#include "HDCargoComponent.h"
#include "TimerManager.h"
#include "JsonObjectConverter.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"
#include "HDShipPawn.h"
#include "HDPlayerController.h"
#include "HDPlayerState.h"
#include "HDHUD.h"
#include "HDBackendClient.h"
#include "HDSystemDirector.h"
#include "HDUniverseSubsystem.h"
#include "HeliosDrift.h"
#include "GameFramework/PlayerState.h"
#include "Engine/World.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

AHDGameMode::AHDGameMode()
{
	DefaultPawnClass = AHDShipPawn::StaticClass();
	PlayerControllerClass = AHDPlayerController::StaticClass();
	PlayerStateClass = AHDPlayerState::StaticClass();
	GameStateClass = AHDGameState::StaticClass();
	HUDClass = AHDHUD::StaticClass();
	FString Sys;
	if (FParse::Value(FCommandLine::Get(), TEXT("SystemId="), Sys)) { SystemId = FName(*Sys); }
}

void AHDGameMode::StartPlay()
{
	EnsureDirector();
	if (AHDGameState* GS = GetGameState<AHDGameState>())
	{
		if (UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this))
		{
			GS->UniverseSeed = U->GetSeed();
			GS->UniverseSystemCount = U->NumSystems();
		}
	}
	Super::StartPlay();
	if (HasAuthority())
	{
		LoadWorldState();
		GetWorldTimerManager().SetTimer(SaveTimer, this, &AHDGameMode::SaveWorldState, FMath::Max(10.f, SaveIntervalSeconds), true);
	}
}

void AHDGameMode::EnsureDirector()
{
	if (Director) return;
	UWorld* World = GetWorld();
	if (!World) return;
	FActorSpawnParameters P;
	P.Name = TEXT("HDSystemDirector");
	P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Director = World->SpawnActor<AHDSystemDirector>(AHDSystemDirector::StaticClass(), FTransform::Identity, P);
	if (UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this))
	{
		Director->EnsureActive(GetStartSystemFor(nullptr)); // warm the start system so the first pilot spawns into a ready world
		UE_LOG(LogHelios, Log, TEXT("Nullpoint universe ready: %d systems. Start system %s."), U->NumSystems(), *U->GetSystemSkeleton(GetStartSystemFor(nullptr)).Name);
	}
}

int32 AHDGameMode::GetStartSystemFor(const AController* C) const
{
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	if (!U) return 0;
	const int32 FromCmd = U->FindSystem(SystemId.ToString());
	return FromCmd >= 0 ? FromCmd : U->GetStartSystem();
}

FString AHDGameMode::PilotIdFor(const AController* C)
{
	const APlayerState* PS = C ? C->PlayerState : nullptr;
	if (!PS) return TEXT("pilot-unknown");
	if (PS->GetUniqueId().IsValid()) { return PS->GetUniqueId().ToString(); }
	return PS->GetPlayerName();
}

void AHDGameMode::RestartPlayer(AController* NewPlayer)
{
	Super::RestartPlayer(NewPlayer);
	AHDShipPawn* Ship = NewPlayer ? Cast<AHDShipPawn>(NewPlayer->GetPawn()) : nullptr;
	if (Ship) { PlaceNewShip(NewPlayer, Ship); }
	// A pilot state that arrived before the pawn existed: apply the fit and hold now.
	const FString Pilot = PilotIdFor(NewPlayer);
	if (const FString* Json = PendingPilotJson.Find(Pilot))
	{
		if (Ship && NewPlayer)
		{
			ApplyPilotJson(nullptr, Ship, *Json);
			PendingPilotJson.Remove(Pilot);
		}
	}
}

void AHDGameMode::PlaceNewShip(AController* C, AHDShipPawn* Ship)
{
	EnsureDirector();
	if (!Director || !Ship) return;
	const FString Pilot = PilotIdFor(C);
	int32 Sys = GetStartSystemFor(C);
	FVector Local = Director->GetArrivalLocal(Sys);
	FRotator Rot = FRotator::ZeroRotator;
	if (const int32* Saved = PendingSystem.Find(Pilot))
	{
		Sys = *Saved;
		Local = PendingLocal.FindRef(Pilot);
		Rot = PendingRot.FindRef(Pilot);
		PendingSystem.Remove(Pilot);
	}
	// Spread simultaneous arrivals so ships don't spawn inside each other.
	Local += FVector(0.f, FMath::FRandRange(-4000.f, 4000.f), FMath::FRandRange(-2000.f, 2000.f));
	if (!Director->PlaceShip(Ship, Sys, Local, Rot))
	{
		UE_LOG(LogHelios, Warning, TEXT("Could not place %s in system %d"), *Pilot, Sys);
	}
}

void AHDGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	const FString Pilot = PilotIdFor(NewPlayer);
	if (AHDPlayerState* HPS = NewPlayer ? NewPlayer->GetPlayerState<AHDPlayerState>() : nullptr)
	{
		HPS->PilotId = Pilot;
	}
	// Saved wallet / standings / storage / fit first, then anything owed while offline (ClaimPending runs after the load).
	LoadPilot(NewPlayer);
	UE_LOG(LogHelios, Log, TEXT("Pilot %s joined"), *Pilot);

	TWeakObjectPtr<APlayerController> WeakPC(NewPlayer);
	TWeakObjectPtr<AHDGameMode> WeakGM(this);
	FHDBackendClient::LoadShip(Pilot, FHDOnShipLoaded::CreateLambda([WeakPC, WeakGM](bool bFound, const FHDShipRecord& Rec)
	{
		if (!bFound || !WeakPC.IsValid() || !WeakGM.IsValid()) return;
		AHDGameMode* GM = WeakGM.Get();
		AHDShipPawn* Ship = Cast<AHDShipPawn>(WeakPC->GetPawn());
		UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(GM);
		const int32 Sys = U ? U->FindSystem(Rec.SystemId) : -1;
		const bool bValidPos = !Rec.Location.ContainsNaN() && !Rec.Location.IsNearlyZero();
		if (Ship)
		{
			if (Rec.HullHP > 0.f) { Ship->HullHP = FMath::Min(Rec.HullHP, Ship->MaxHullHP); }
			if (Sys >= 0 && bValidPos && GM->GetDirector()) { GM->GetDirector()->PlaceShip(Ship, Sys, Rec.Location, Rec.Rotation); }
		}
		else if (Sys >= 0 && bValidPos)
		{
			const FString LoadedPilot = PilotIdFor(WeakPC.Get());
			GM->PendingSystem.Add(LoadedPilot, Sys);
			GM->PendingLocal.Add(LoadedPilot, Rec.Location);
			GM->PendingRot.Add(LoadedPilot, Rec.Rotation);
		}
		UE_LOG(LogHelios, Log, TEXT("Loaded ship for %s: hull=%s hp=%.0f system=%s"), *Rec.PilotId, *Rec.Hull, Rec.HullHP, *Rec.SystemId);
	}));
}

void AHDGameMode::Logout(AController* Exiting)
{
	const AHDShipPawn* Ship = Exiting ? Cast<AHDShipPawn>(Exiting->GetPawn()) : nullptr;
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	if (Ship && Ship->HullHP > 0.f && !Ship->IsPendingKillPending() && U && U->IsValidSystem(Ship->SystemIndex))
	{
		FHDShipRecord Rec;
		Rec.PilotId = PilotIdFor(Exiting);
		Rec.SystemId = U->GetSystemSkeleton(Ship->SystemIndex).Id.ToString();
		Rec.Location = Ship->GetLocalPosition(); // system-local: slots differ between server runs
		Rec.Rotation = Ship->GetActorRotation();
		Rec.HullHP = Ship->HullHP;
		FHDBackendClient::SaveShip(Rec);
	}
	SavePilot(Exiting);
	if (Exiting) { const FString Gone = PilotIdFor(Exiting); PilotLoaded.Remove(Gone); PendingPilotJson.Remove(Gone); }
	Super::Logout(Exiting);
}

// ---------------------------------------------------------------------------------------------
// M7 persistence
// ---------------------------------------------------------------------------------------------
namespace
{
	const TCHAR* GWorldKinds[] = { TEXT("economy"), TEXT("territory"), TEXT("rules") };

	FString PersistJsonString(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	template <typename T>
	TArray<TSharedPtr<FJsonValue>> PersistStructArray(const TArray<T>& In)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const T& Item : In)
		{
			TSharedRef<FJsonObject> Obj = MakeShared<FJsonObject>();
			if (FJsonObjectConverter::UStructToJsonObject(T::StaticStruct(), &Item, Obj, 0, 0)) Out.Add(MakeShared<FJsonValueObject>(Obj));
		}
		return Out;
	}

	template <typename T>
	bool PersistReadStructArray(const TSharedPtr<FJsonObject>& Root, const TCHAR* Field, TArray<T>& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Root.IsValid() || !Root->TryGetArrayField(Field, Arr) || !Arr) return false;
		Out.Reset();
		for (const TSharedPtr<FJsonValue>& V : *Arr)
		{
			T Item;
			if (V.IsValid() && V->AsObject().IsValid() && FJsonObjectConverter::JsonObjectToUStruct(V->AsObject().ToSharedRef(), &Item, 0, 0)) Out.Add(Item);
		}
		return true;
	}
}

FString AHDGameMode::ExportWorldDoc(const FString& Kind) const
{
	if (Kind == TEXT("economy"))
	{
		const UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(this);
		return Econ ? Econ->ExportJson() : FString();
	}
	if (Kind == TEXT("territory"))
	{
		const UHDTerritorySubsystem* Terr = UHDTerritorySubsystem::Get(this);
		return Terr ? Terr->ExportJson() : FString();
	}
	if (Kind == TEXT("rules"))
	{
		const AHDGameState* GS = GetGameState<AHDGameState>();
		if (!GS) return FString();
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetArrayField(TEXT("wars"), PersistStructArray(GS->Wars));
		return PersistJsonString(Root);
	}
	return FString();
}

void AHDGameMode::ApplyWorldDoc(const FString& Kind, const FString& Json)
{
	if (Kind == TEXT("economy"))
	{
		if (UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(this)) Econ->ImportJson(Json);
	}
	else if (Kind == TEXT("territory"))
	{
		UHDTerritorySubsystem* Terr = UHDTerritorySubsystem::Get(this);
		if (!Terr) return;
		Terr->ImportJson(Json);
		// Systems already running get their structures, lanes and station proxies now.
		if (Director)
		{
			TArray<int32> Systems;
			for (const FHDActiveSystem& Act : Director->GetActiveSystems()) Systems.Add(Act.System);
			for (int32 Sys : Systems) { if (FHDActiveSystem* Act = Director->FindActive(Sys)) Terr->SpawnForSystem(*Act); }
		}
	}
	else if (Kind == TEXT("rules"))
	{
		AHDGameState* GS = GetGameState<AHDGameState>();
		TSharedPtr<FJsonObject> Root;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
		if (GS && FJsonSerializer::Deserialize(Reader, Root) && Root.IsValid()) PersistReadStructArray(Root, TEXT("wars"), GS->Wars);
	}
}

void AHDGameMode::LoadWorldState()
{
	TWeakObjectPtr<AHDGameMode> WeakGM(this);
	for (const TCHAR* KindName : GWorldKinds)
	{
		const FString Kind(KindName);
		if (WorldVersions.Contains(Kind)) continue;
		FHDBackendClient::LoadWorldDoc(Kind, FHDOnDocLoaded::CreateLambda([WeakGM, Kind](int32 Status, int64 Version, const FString& Data)
		{
			AHDGameMode* GM = WeakGM.Get();
			if (!GM || GM->WorldVersions.Contains(Kind)) return;
			if (Status == 200)
			{
				GM->ApplyWorldDoc(Kind, Data);
				GM->WorldVersions.Add(Kind, Version);
				UE_LOG(LogHelios, Log, TEXT("World %s restored (v%lld)"), *Kind, Version);
			}
			else if (Status == 404)
			{
				GM->WorldVersions.Add(Kind, 0); // fresh world: our first save creates it
			}
			else if (!GM->GetWorldTimerManager().IsTimerActive(GM->WorldRetryTimer))
			{
				// Unreachable or erroring backend: never save over state we haven't seen. Retry the load.
				GM->GetWorldTimerManager().SetTimer(GM->WorldRetryTimer, GM, &AHDGameMode::LoadWorldState, 30.f, false);
			}
		}));
	}
}

void AHDGameMode::SaveWorldState()
{
	TWeakObjectPtr<AHDGameMode> WeakGM(this);
	for (const TCHAR* KindName : GWorldKinds)
	{
		const FString Kind(KindName);
		const int64* Known = WorldVersions.Find(Kind);
		if (!Known || WorldBlocked.Contains(Kind) || WorldSaving.Contains(Kind)) continue;
		const FString Json = ExportWorldDoc(Kind);
		if (Json.IsEmpty()) continue;
		WorldSaving.Add(Kind);
		FHDBackendClient::SaveWorldDoc(Kind, Json, *Known, FHDOnDocSaved::CreateLambda([WeakGM, Kind](int32 Status, int64 Version)
		{
			AHDGameMode* GM = WeakGM.Get();
			if (!GM) return;
			GM->WorldSaving.Remove(Kind);
			if (Status == 200) GM->WorldVersions.Add(Kind, Version);
			else if (Status == 409)
			{
				// Another server owns this document now. Stop writing rather than fight over it.
				GM->WorldBlocked.Add(Kind);
				UE_LOG(LogHelios, Error, TEXT("World %s was written by another server (v%lld): this server stops saving it"), *Kind, Version);
			}
		}));
	}
	AGameStateBase* GSB = GetGameState<AGameStateBase>();
	if (!GSB) return;
	for (APlayerState* P : GSB->PlayerArray)
	{
		if (AController* C = P ? Cast<AController>(P->GetOwner()) : nullptr) SavePilot(C);
	}
}

FString AHDGameMode::DescribePersistence() const
{
	FString S = FString::Printf(TEXT("Backend %s | save every %.0f s |"), *FHDBackendClient::GetBaseUrl(), SaveIntervalSeconds);
	for (const TCHAR* KindName : GWorldKinds)
	{
		const FString Kind(KindName);
		const int64* V = WorldVersions.Find(Kind);
		S += FString::Printf(TEXT(" %s:%s"), *Kind, WorldBlocked.Contains(Kind) ? TEXT("BLOCKED") : V ? *FString::Printf(TEXT("v%lld"), *V) : TEXT("not loaded"));
	}
	S += FString::Printf(TEXT(" | %d pilots loaded"), PilotLoaded.Num());
	return S;
}

void AHDGameMode::LoadPilot(APlayerController* PC)
{
	AHDPlayerState* HPS = PC ? PC->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!HPS) return;
	const FString Pilot = PilotIdFor(PC);
	TWeakObjectPtr<APlayerController> WeakPC(PC);
	TWeakObjectPtr<AHDGameMode> WeakGM(this);
	FHDBackendClient::LoadPilotState(Pilot, FHDOnDocLoaded::CreateLambda([WeakPC, WeakGM, Pilot](int32 Status, int64 Version, const FString& Data)
	{
		AHDGameMode* GM = WeakGM.Get();
		APlayerController* LoadedPC = WeakPC.Get();
		AHDPlayerState* LoadedPS = LoadedPC ? LoadedPC->GetPlayerState<AHDPlayerState>() : nullptr;
		if (!GM || !LoadedPS) return;
		if (Status == 200)
		{
			AHDShipPawn* Ship = Cast<AHDShipPawn>(LoadedPC->GetPawn());
			ApplyPilotJson(LoadedPS, Ship, Data);
			if (!Ship) GM->PendingPilotJson.Add(Pilot, Data);
			UE_LOG(LogHelios, Log, TEXT("Pilot %s restored (v%lld)"), *Pilot, Version);
		}
		// Only a confirmed load (or confirmed absence) lets this session write the pilot back.
		if (Status == 200 || Status == 404) GM->PilotLoaded.Add(Pilot);
		if (UHDEconomySubsystem* Econ = UHDEconomySubsystem::Get(GM)) Econ->ClaimPending(LoadedPS);
	}));
}

void AHDGameMode::SavePilot(AController* C)
{
	AHDPlayerState* HPS = C ? C->GetPlayerState<AHDPlayerState>() : nullptr;
	if (!HPS) return;
	const FString Pilot = PilotIdFor(C);
	if (!PilotLoaded.Contains(Pilot)) return;
	const AHDShipPawn* Ship = Cast<AHDShipPawn>(C->GetPawn());
	FHDBackendClient::SavePilotState(Pilot, BuildPilotJson(HPS, Ship && Ship->HullHP > 0.f ? Ship : nullptr), FHDOnDocSaved());
}

FString AHDGameMode::BuildPilotJson(const AHDPlayerState* PS, const AHDShipPawn* Ship)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	if (!PS) return PersistJsonString(Root);
	Root->SetNumberField(TEXT("credits"), (double)PS->Credits);
	Root->SetStringField(TEXT("corp"), PS->Corp.IsNone() ? FString() : PS->Corp.ToString());
	Root->SetNumberField(TEXT("criminalUntil"), PS->CriminalUntil);
	Root->SetBoolField(TEXT("starterGiven"), PS->bStarterGiven);
	TArray<TSharedPtr<FJsonValue>> Revealed;
	for (const FName& Id : PS->Revealed) Revealed.Add(MakeShared<FJsonValueString>(Id.ToString()));
	Root->SetArrayField(TEXT("revealed"), Revealed);
	Root->SetArrayField(TEXT("standings"), PersistStructArray(PS->Standings));
	Root->SetArrayField(TEXT("skills"), PersistStructArray(PS->Skills));
	Root->SetArrayField(TEXT("storage"), PersistStructArray(PS->Storage));
	Root->SetArrayField(TEXT("hangar"), PersistStructArray(PS->Hangar));
	// A destroyed ship isn't saved: you log back in with whatever the respawn gives you.
	if (Ship && Ship->Fitting)
	{
		TSharedRef<FJsonObject> Fit = MakeShared<FJsonObject>();
		Fit->SetStringField(TEXT("hull"), Ship->Fitting->HullId.ToString());
		TArray<TSharedPtr<FJsonValue>> Mods;
		for (const FName& M : Ship->Fitting->Modules) Mods.Add(MakeShared<FJsonValueString>(M.ToString()));
		Fit->SetArrayField(TEXT("modules"), Mods);
		Fit->SetBoolField(TEXT("recovered"), Ship->Fitting->bRecovered);
		Root->SetObjectField(TEXT("fit"), Fit);
		if (Ship->Cargo) Root->SetArrayField(TEXT("cargo"), PersistStructArray(Ship->Cargo->Items));
	}
	return PersistJsonString(Root);
}

void AHDGameMode::ApplyPilotJson(AHDPlayerState* PS, AHDShipPawn* Ship, const FString& Json)
{
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Json);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid()) return;
	if (PS)
	{
		double Num = 0.0;
		if (Root->TryGetNumberField(TEXT("credits"), Num) && FMath::IsFinite(Num)) PS->Credits = FMath::Max<int64>(0, (int64)Num);
		FString CorpStr;
		// Company membership itself is territory state; this only fills the replicated tag early.
		if (Root->TryGetStringField(TEXT("corp"), CorpStr) && !CorpStr.IsEmpty()) PS->Corp = FName(*CorpStr);
		if (Root->TryGetNumberField(TEXT("criminalUntil"), Num)) PS->CriminalUntil = Num;
		Root->TryGetBoolField(TEXT("starterGiven"), PS->bStarterGiven);
		const TArray<TSharedPtr<FJsonValue>>* Revealed = nullptr;
		if (Root->TryGetArrayField(TEXT("revealed"), Revealed) && Revealed)
		{
			PS->Revealed.Reset();
			for (const TSharedPtr<FJsonValue>& V : *Revealed) { if (V.IsValid()) PS->Revealed.AddUnique(FName(*V->AsString())); }
		}
		PersistReadStructArray(Root, TEXT("standings"), PS->Standings);
		PersistReadStructArray(Root, TEXT("skills"), PS->Skills);
		PersistReadStructArray(Root, TEXT("storage"), PS->Storage);
		PersistReadStructArray(Root, TEXT("hangar"), PS->Hangar);
	}
	const TSharedPtr<FJsonObject>* Fit = nullptr;
	if (Ship && Ship->Fitting && Root->TryGetObjectField(TEXT("fit"), Fit) && Fit && Fit->IsValid())
	{
		FString Hull;
		(*Fit)->TryGetStringField(TEXT("hull"), Hull);
		TArray<FName> Mods;
		const TArray<TSharedPtr<FJsonValue>>* ModVals = nullptr;
		if ((*Fit)->TryGetArrayField(TEXT("modules"), ModVals) && ModVals) { for (const TSharedPtr<FJsonValue>& V : *ModVals) { if (V.IsValid()) Mods.Add(FName(*V->AsString())); } }
		FString Err;
		if (!Hull.IsEmpty() && !Ship->Fitting->SetFitAuthority(FName(*Hull), Mods, Err))
		{
			UE_LOG(LogHelios, Warning, TEXT("Saved fit rejected (%s): keeping the default ship"), *Err);
		}
		else
		{
			bool bRecovered = false;
			if ((*Fit)->TryGetBoolField(TEXT("recovered"), bRecovered)) Ship->Fitting->bRecovered = bRecovered;
			if (Ship->Cargo) PersistReadStructArray(Root, TEXT("cargo"), Ship->Cargo->Items);
		}
	}
}
