#include "HDGameMode.h"
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
			const FString Pilot = PilotIdFor(WeakPC.Get());
			GM->PendingSystem.Add(Pilot, Sys);
			GM->PendingLocal.Add(Pilot, Rec.Location);
			GM->PendingRot.Add(Pilot, Rec.Rotation);
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
	Super::Logout(Exiting);
}
