#include "HDShipMovementComponent.h"
#include "HDShipPowerComponent.h"
#include "GameFramework/Pawn.h"
#include "Net/UnrealNetwork.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "HDShipPawn.h"
#include "HDNavComponent.h"
#include "HDWorldActors.h"
#include "HDPlayerState.h"
#include "Kismet/GameplayStatics.h"
#include "GameFramework/DamageType.h"

UHDShipMovementComponent::UHDShipMovementComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickGroup = TG_PrePhysics;
	SetIsReplicatedByDefault(true);
}

void UHDShipMovementComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UHDShipMovementComponent, TravelMode);
	DOREPLIFETIME(UHDShipMovementComponent, TravelSpeed);
	DOREPLIFETIME(UHDShipMovementComponent, TravelDir);
	DOREPLIFETIME_CONDITION(UHDShipMovementComponent, ServerState, COND_SimulatedOnly);
}

float UHDShipMovementComponent::GetSpeedCap() const
{
	float Engines = 50.f;
	if (const AActor* Owner = GetOwner())
	{
		if (const UHDShipPowerComponent* Power = Owner->FindComponentByClass<UHDShipPowerComponent>())
		{
			Engines = Power->GetPower(EHDSubsystem::Engines);
		}
	}
	return BaseMaxSpeed * (0.5f + Engines / 100.f);
}

FHDShipState UHDShipMovementComponent::CaptureState(float Timestamp) const
{
	FHDShipState S;
	S.Timestamp = Timestamp;
	if (const AActor* Owner = GetOwner())
	{
		S.Location = Owner->GetActorLocation();
		S.Rotation = Owner->GetActorQuat();
	}
	S.Velocity = Velocity;
	S.AngularVelocity = AngularVelocity;
	return S;
}

void UHDShipMovementComponent::SimulateMove(const FHDShipMove& Move)
{
	AActor* Owner = GetOwner();
	const float Dt = Move.DeltaTime;
	if (!Owner || Dt <= 0.f) return;

	// Held modes: no motion at all.
	if (TravelMode == EHDTravelMode::Docked || TravelMode == EHDTravelMode::Landed || TravelMode == EHDTravelMode::Jumping)
	{
		Velocity = FVector::ZeroVector;
		AngularVelocity = FVector::ZeroVector;
		return;
	}

	const bool bSlip = TravelMode == EHDTravelMode::Slipstream;
	const bool bCruise = TravelMode == EHDTravelMode::Cruise;
	const bool bGlide = TravelMode == EHDTravelMode::Glide;
	const bool bLane = TravelMode == EHDTravelMode::TradeLane;
	const bool bFast = bSlip || bCruise || bGlide || bLane;
	const float TurnScale = bSlip ? SlipTurnScale : bCruise ? CruiseTurnScale : bGlide ? GlideTurnScale : bLane ? 0.25f : 1.f;
	const FVector RotIn = Move.Rot.BoundToCube(1.f) * TurnScale;
	const FVector LinIn = bFast ? FVector::ZeroVector : Move.Linear.BoundToCube(1.f);

	// Angular (deg/s in local pitch/yaw/roll)
	const FVector TargetAng = RotIn * MaxAngularRate;
	if (Move.bFlightAssist || bFast)
	{
		AngularVelocity = FMath::VInterpConstantTo(AngularVelocity, TargetAng, Dt, AngularAccel);
	}
	else
	{
		AngularVelocity += RotIn * AngularAccel * Dt;
		AngularVelocity = AngularVelocity.BoundToBox(-MaxAngularRate, MaxAngularRate);
	}
	const FQuat Delta = FQuat(FRotator(AngularVelocity.X * Dt, AngularVelocity.Y * Dt, AngularVelocity.Z * Dt));
	const FQuat NewQ = (Owner->GetActorQuat() * Delta).GetNormalized();

	// Linear
	if (bSlip)
	{
		Velocity = NewQ.GetForwardVector() * (TravelSpeed > 0.f ? TravelSpeed : SlipSpeed);
	}
	else if (bLane)
	{
		Velocity = TravelDir * TravelSpeed;
	}
	else if (bGlide)
	{
		// Unpowered glide holds 2.5 km/s along the nose (doc 17).
		Velocity = NewQ.GetForwardVector() * GlideSpeed;
	}
	else if (bCruise)
	{
		// Throttle is a rate: forward accelerates toward the local limit, reverse slows, neither holds speed.
		const float Limit = GetCruiseLimitAt(Owner->GetActorLocation());
		float Cur = (float)Velocity.Size();
		const float Approach = FMath::Min(1.f, Dt * CruiseAccelRate);
		if (Move.Linear.X > 0.1f) { Cur += (Limit - Cur) * Approach * Move.Linear.X + CruiseMinSpeed * Dt; }
		else if (Move.Linear.X < -0.1f) { Cur -= Cur * Approach * -Move.Linear.X; }
		if (Cur > Limit) { Cur -= (Cur - Limit) * FMath::Min(1.f, Dt * 3.f); } // wells pull you down fast
		Cur = FMath::Clamp(Cur, CruiseMinSpeed, CruiseMaxSpeed);
		Velocity = NewQ.GetForwardVector() * Cur;
	}
	else
	{
		const float Cap = GetSpeedCap();
		if (Move.bFlightAssist)
		{
			const FVector TargetVel = NewQ.RotateVector(LinIn * Cap);
			Velocity = FMath::VInterpConstantTo(Velocity, TargetVel, Dt, LinearAccel);
		}
		else
		{
			Velocity += NewQ.RotateVector(LinIn) * LinearAccel * Dt;
		}
		Velocity = Velocity.GetClampedToMaxSize(Cap);
		// Surface flight: gravity pulls after thrust, so weak thrusters sag at high g (doc 17).
		Velocity += GetGravityAt(Owner->GetActorLocation()) * Dt;
	}

	FHitResult Hit;
	Owner->SetActorLocationAndRotation(Owner->GetActorLocation() + Velocity * Dt, NewQ, true, &Hit);
	if (Hit.bBlockingHit)
	{
		const float ImpactSpeed = (float)FMath::Abs(FVector::DotProduct(Velocity, Hit.Normal));
		Velocity = FVector::VectorPlaneProject(Velocity, Hit.Normal) * 0.5f;
		if (Owner->HasAuthority())
		{
			if (bGlide || (bCruise && ImpactSpeed > GlideMaxEntrySpeed)) { EmergencyDrop(TEXT("Hard impact")); }
			else if (bFast) { DropOut(true); }
		}
	}
}

AHDCelestialBody* UHDShipMovementComponent::GetNearestBody(double& OutAltitude) const
{
	OutAltitude = TNumericLimits<double>::Max();
	const AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwner());
	UWorld* World = GetWorld();
	if (!Ship || !World) return nullptr;
	const float Now = World->GetTimeSeconds();
	const FVector L = Ship->GetActorLocation();
	if (!CachedBody.IsValid() || Now - CachedBodyTime > 0.5f || CachedBody->SystemIndex != Ship->SystemIndex)
	{
		CachedBodyTime = Now;
		CachedBody.Reset();
		double Best = TNumericLimits<double>::Max();
		for (TActorIterator<AHDCelestialBody> It(World); It; ++It)
		{
			if (It->SystemIndex != Ship->SystemIndex) continue;
			const double Alt = It->GetAltitude(L);
			if (Alt < Best) { Best = Alt; CachedBody = *It; }
		}
	}
	if (!CachedBody.IsValid()) return nullptr;
	OutAltitude = CachedBody->GetAltitude(L);
	return CachedBody.Get();
}

float UHDShipMovementComponent::GetCruiseLimitAt(const FVector& WorldLoc) const
{
	double Alt = 0.0;
	const AHDCelestialBody* B = GetNearestBody(Alt);
	if (!B) return CruiseMaxSpeed;
	// Stars push harder (bigger wells).
	const double Rate = B->IsStar() ? CruiseWellRate * 0.5 : CruiseWellRate;
	return (float)FMath::Clamp(FMath::Max(Alt, 0.0) * Rate, (double)CruiseMinSpeed, (double)CruiseMaxSpeed);
}

FVector UHDShipMovementComponent::GetGravityAt(const FVector& WorldLoc) const
{
	double Alt = 0.0;
	const AHDCelestialBody* B = GetNearestBody(Alt);
	if (!B || B->IsStar() || Alt > GravityAltitude) return FVector::ZeroVector;
	const FVector Down = (B->GetActorLocation() - WorldLoc).GetSafeNormal();
	return Down * (B->Body.Gravity * 980.665f * GravityScale);
}

float UHDShipMovementComponent::GetPitchToHorizon(double& OutAltitude) const
{
	const AHDCelestialBody* B = GetNearestBody(OutAltitude);
	const AActor* Owner = GetOwner();
	if (!B || !Owner) return 0.f;
	const FVector Up = (Owner->GetActorLocation() - B->GetActorLocation()).GetSafeNormal();
	return (float)FMath::RadiansToDegrees(FMath::Asin(FMath::Clamp((double)FVector::DotProduct(Owner->GetActorForwardVector(), Up), -1.0, 1.0)));
}

void UHDShipMovementComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	APawn* Pawn = Cast<APawn>(GetOwner());
	if (!Pawn) return;

	if (Pawn->HasAuthority()) { UpdateTravelAuthority(DeltaTime); }

	if (Pawn->IsLocallyControlled())
	{
		FHDShipMove M;
		M.DeltaTime = FMath::Min(DeltaTime, MaxMoveDeltaTime);
		ClientTime += M.DeltaTime;
		M.Timestamp = ClientTime;
		M.Linear = InputLinear.BoundToCube(1.f);

		// Rate inputs (keys, stick) are already -1..1 of max turn rate. Mouse is a delta in degrees:
		// convert to the rate that covers it in this tick, normalized by max rate. No double scaling.
		FVector Rot = InputRot;
		Rot.X += StickRot.X;
		Rot.Y += StickRot.Y;
		if (DeltaTime > KINDA_SMALL_NUMBER && !PendingMouseDegrees.IsZero())
		{
			if (MaxAngularRate.X > KINDA_SMALL_NUMBER) { Rot.X += PendingMouseDegrees.X / (DeltaTime * MaxAngularRate.X); }
			if (MaxAngularRate.Y > KINDA_SMALL_NUMBER) { Rot.Y += PendingMouseDegrees.Y / (DeltaTime * MaxAngularRate.Y); }
		}
		PendingMouseDegrees = FVector2D::ZeroVector;
		M.Rot = Rot.BoundToCube(1.f);
		M.bFlightAssist = bFlightAssist;
		SimulateMove(M);
		if (Pawn->HasAuthority())
		{
			ServerState = CaptureState(M.Timestamp);
		}
		else
		{
			Unacked.Add(M);
			if (Unacked.Num() > FMath::Max(1, MaxUnackedMoves)) { Unacked.RemoveAt(0); }
			ServerMove(M);
		}
	}
	else if (Pawn->HasAuthority())
	{
		// Remote-controlled ship on the server: grant real time the client may spend on moves.
		ServerTimeBudget = FMath::Min(ServerTimeBudget + DeltaTime, MaxClientTimeAhead);
	}
	else if (ServerState.Timestamp > 0.f)
	{
		// Simulated proxy: smooth toward server state (exponential, frame-rate independent).
		Velocity = ServerState.Velocity;
		AngularVelocity = ServerState.AngularVelocity;
		const FVector L = Pawn->GetActorLocation();
		if (FVector::DistSquared(L, ServerState.Location) > FMath::Square(ProxySnapDistance))
		{
			Pawn->SetActorLocationAndRotation(ServerState.Location, ServerState.Rotation);
		}
		else
		{
			const float Alpha = 1.f - FMath::Exp(-ProxySmoothingRate * DeltaTime);
			const FVector NewL = L + (ServerState.Location - L) * Alpha;
			const FQuat NewQ = FQuat::Slerp(Pawn->GetActorQuat(), ServerState.Rotation, Alpha);
			Pawn->SetActorLocationAndRotation(NewL, NewQ);
		}
	}
}

void UHDShipMovementComponent::ServerMove_Implementation(const FHDShipMove& Move)
{
	if (Move.Timestamp <= LastServerTimestamp) return; // stale/out of order
	if (!FMath::IsFinite(Move.Timestamp) || !FMath::IsFinite(Move.DeltaTime) || Move.Linear.ContainsNaN() || Move.Rot.ContainsNaN()) return;
	LastServerTimestamp = Move.Timestamp;
	FHDShipMove M = Move;
	// Clamp to max step and to the real time this client has been granted (stops speed hacks via fast clocks).
	M.DeltaTime = FMath::Clamp(M.DeltaTime, 0.f, FMath::Min(MaxMoveDeltaTime, FMath::Max(ServerTimeBudget, 0.f)));
	ServerTimeBudget -= M.DeltaTime;
	SimulateMove(M);
	ServerState = CaptureState(M.Timestamp);
	ClientAck(ServerState);
}

void UHDShipMovementComponent::ClientAck_Implementation(const FHDShipState& State)
{
	if (State.Timestamp <= LastAckTimestamp) return;
	LastAckTimestamp = State.Timestamp;
	Unacked.RemoveAll([&State](const FHDShipMove& M) { return M.Timestamp <= State.Timestamp; });

	AActor* Owner = GetOwner();
	if (!Owner) return;
	Owner->SetActorLocationAndRotation(State.Location, State.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
	Velocity = State.Velocity;
	AngularVelocity = State.AngularVelocity;
	for (const FHDShipMove& M : Unacked) { SimulateMove(M); }
}

void UHDShipMovementComponent::ServerToggleSlipstream_Implementation()
{
	if (TravelMode == EHDTravelMode::Spooling || TravelMode == EHDTravelMode::Slipstream || TravelMode == EHDTravelMode::Cruise) { DropOut(false); return; }
	if (TravelMode != EHDTravelMode::Normal || SlipCooldown > 0.f) return;
	if (const AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwner()))
	{
		if (Ship->Nav && Ship->Nav->ShipFuel <= 0.f) { Ship->Nav->Notify(TEXT("No fuel")); return; }
	}
	TravelMode = EHDTravelMode::Spooling;
	SpoolInto = EHDTravelMode::Slipstream;
	SpoolRemaining = SpoolTime;
}

void UHDShipMovementComponent::DropOut(bool bInterdicted)
{
	// Only the fast in-system modes drop out. Docked, landed, gliding and jump holds are left alone.
	if (TravelMode != EHDTravelMode::Spooling && TravelMode != EHDTravelMode::Slipstream && TravelMode != EHDTravelMode::Cruise && TravelMode != EHDTravelMode::TradeLane) return;
	if (TravelMode == EHDTravelMode::Slipstream || TravelMode == EHDTravelMode::Cruise || TravelMode == EHDTravelMode::TradeLane)
	{
		Velocity = Velocity.GetSafeNormal() * BaseMaxSpeed * DropOutSpeedFraction;
	}
	TravelMode = EHDTravelMode::Normal;
	TravelSpeed = 0.f;
	SpoolRemaining = 0.f;
	TravelTarget.Reset();
	if (bInterdicted) { SlipCooldown = InterdictCooldown; }
}

AActor* UHDShipMovementComponent::PickSlipTarget() const
{
	AActor* Owner = GetOwner();
	UWorld* World = GetWorld();
	if (!Owner || !World) return nullptr;
	AActor* Best = nullptr;
	double BestDist = TNumericLimits<double>::Max();
	const FVector Fwd = Owner->GetActorForwardVector();
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* A = *It;
		if (A == Owner || !A->ActorHasTag(TEXT("SlipTarget"))) continue;
		const FVector To = A->GetActorLocation() - Owner->GetActorLocation();
		const double Dist = To.Size();
		if (Dist < DropDistance * 2.f) continue;
		if (FVector::DotProduct(To / Dist, Fwd) < SlipTargetMinDot) continue;
		if (Dist < BestDist) { BestDist = Dist; Best = A; }
	}
	return Best;
}

void UHDShipMovementComponent::UpdateTravelAuthority(float DeltaTime)
{
	SlipCooldown = FMath::Max(0.f, SlipCooldown - DeltaTime);
	AActor* Owner = GetOwner();
	if (!Owner) return;
	if (TravelMode == EHDTravelMode::Spooling)
	{
		SpoolRemaining -= DeltaTime;
		if (SpoolRemaining <= 0.f)
		{
			if (SpoolInto == EHDTravelMode::Cruise)
			{
				TravelMode = EHDTravelMode::Cruise;
				Velocity = Owner->GetActorForwardVector() * FMath::Max((float)Velocity.Size(), CruiseMinSpeed);
			}
			else
			{
				TravelMode = EHDTravelMode::Slipstream;
				TravelTarget = PickSlipTarget();
				TravelSpeed = SlipSpeed;
			}
		}
	}
	else if (TravelMode == EHDTravelMode::Slipstream)
	{
		if (TravelTarget.IsValid())
		{
			const double Dist = FVector::Dist(TravelTarget->GetActorLocation(), Owner->GetActorLocation());
			TravelSpeed = (float)FMath::Clamp(Dist * SlipDistanceRate, (double)SlipSpeed, (double)SlipMaxSpeed);
			if (Dist < DropDistance + TravelSpeed * DeltaTime * 2.f) { DropOut(false); }
		}
		else
		{
			TravelSpeed = SlipSpeed;
		}
	}
	else if (TravelMode == EHDTravelMode::TradeLane)
	{
		const double Dist = FVector::Dist(LaneExit, Owner->GetActorLocation());
		if (Dist < TravelSpeed * DeltaTime * 3.f + 200000.0)
		{
			// Arrive exactly at the exit ring, at sub-space speed.
			TravelMode = EHDTravelMode::Normal;
			TravelSpeed = 0.f;
			TeleportAuthority(LaneExit, Owner->GetActorQuat(), false);
			Velocity = TravelDir * BaseMaxSpeed * 0.5f;
		}
	}
	if (TravelMode == EHDTravelMode::Cruise || TravelMode == EHDTravelMode::Glide) { UpdateLandingAuthority(); }
}

void UHDShipMovementComponent::UpdateLandingAuthority()
{
	double Alt = 0.0;
	const float Pitch = GetPitchToHorizon(Alt);
	AHDCelestialBody* B = GetNearestBody(Alt);
	AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwner());
	if (!B || !Ship || B->IsStar()) return;
	UHDNavComponent* Nav = Ship->Nav;

	if (TravelMode == EHDTravelMode::Cruise && Alt < OrbitalFloorAltitude)
	{
		if (B->IsGasGiant())
		{
			// Gas giants are orbital-only: drop to sub space at the cloud tops for scooping.
			DropOut(false);
			if (Nav) Nav->Notify(TEXT("Gas giant: dropped to sub space at the cloud tops"));
			return;
		}
		if (!B->Body.bLandable)
		{
			DropOut(true);
			if (Nav) Nav->Notify(FString::Printf(TEXT("%s is not landable (%.1f g)"), *B->DisplayName, B->Body.Gravity));
			return;
		}
		const float Speed = (float)Velocity.Size();
		const FHDCorridorStatus Corr = UHDNavComponent::CheckCorridor(B, Ship->GetActorLocation(), AHDPlayerState::NowUnix());
		if (Pitch > GlideMaxPitch || !Corr.bOpen)
		{
			// Too shallow, or outside the published corridor: bounce back to orbital cruise.
			const FVector Up = (Ship->GetActorLocation() - B->GetActorLocation()).GetSafeNormal();
			TeleportAuthority(B->GetActorLocation() + Up * (B->GetRadiusCm() + OrbitalFloorAltitude + 100000.0), Ship->GetActorQuat(), false);
			Velocity = Ship->GetActorForwardVector() * CruiseMinSpeed;
			if (Nav)
			{
				Nav->Notify(Corr.bOpen ? FString(TEXT("Too shallow: bounced to orbital cruise. Pitch between -5 and -60 degrees."))
					: FString::Printf(TEXT("Outside landing corridor %d (%.0f deg off). Bounced."), Corr.ActiveCorridor, Corr.DegreesOff));
			}
			return;
		}
		if (Pitch < GlideMinPitch || Speed > GlideMaxEntrySpeed)
		{
			EmergencyDrop(Speed > GlideMaxEntrySpeed ? TEXT("Too fast for glide") : TEXT("Too steep for glide"));
			return;
		}
		TravelMode = EHDTravelMode::Glide;
		if (Nav) Nav->Notify(FString::Printf(TEXT("Glide: %s, %.2f g"), *B->DisplayName, B->Body.Gravity));
	}
	else if (TravelMode == EHDTravelMode::Glide)
	{
		if (Alt > OrbitalFloorAltitude * 1.05) { TravelMode = EHDTravelMode::Cruise; if (Nav) Nav->Notify(TEXT("Climbed out: orbital cruise")); return; }
		if (Pitch < GlideMinPitch - 5.f) { EmergencyDrop(TEXT("Glide too steep")); return; }
		if (Alt < SurfaceFlightAltitude)
		{
			TravelMode = EHDTravelMode::Normal;
			Velocity = Velocity.GetClampedToMaxSize(BaseMaxSpeed);
			if (Nav) Nav->Notify(TEXT("Surface flight: thrusters engaged"));
		}
	}
}

void UHDShipMovementComponent::EmergencyDrop(const TCHAR* Why)
{
	AActor* Owner = GetOwner();
	TravelMode = EHDTravelMode::Normal;
	TravelSpeed = 0.f;
	SpoolRemaining = 0.f;
	SlipCooldown = InterdictCooldown;
	Velocity = Velocity.GetSafeNormal() * BaseMaxSpeed * 0.3f;
	if (Owner)
	{
		UGameplayStatics::ApplyDamage(Owner, EmergencyDropDamage, nullptr, nullptr, UDamageType::StaticClass());
		if (const AHDShipPawn* Ship = Cast<AHDShipPawn>(Owner)) { if (Ship->Nav) Ship->Nav->Notify(FString::Printf(TEXT("EMERGENCY DROP: %s"), Why)); }
	}
}

void UHDShipMovementComponent::ToggleCruiseAuthority()
{
	if (TravelMode == EHDTravelMode::Cruise || (TravelMode == EHDTravelMode::Spooling && SpoolInto == EHDTravelMode::Cruise)) { DropOut(false); return; }
	if (TravelMode != EHDTravelMode::Normal || SlipCooldown > 0.f) return;
	TravelMode = EHDTravelMode::Spooling;
	SpoolInto = EHDTravelMode::Cruise;
	SpoolRemaining = CruiseSpoolTime;
}

void UHDShipMovementComponent::SetModeAuthority(EHDTravelMode Mode)
{
	TravelMode = Mode;
	SpoolRemaining = 0.f;
	TravelTarget.Reset();
	if (Mode == EHDTravelMode::Docked || Mode == EHDTravelMode::Landed || Mode == EHDTravelMode::Jumping)
	{
		Velocity = FVector::ZeroVector;
		AngularVelocity = FVector::ZeroVector;
	}
}

void UHDShipMovementComponent::StartLaneAuthority(const FVector& Exit, float Speed)
{
	AActor* Owner = GetOwner();
	if (!Owner) return;
	LaneExit = Exit;
	TravelDir = (Exit - Owner->GetActorLocation()).GetSafeNormal();
	TravelSpeed = Speed;
	TravelMode = EHDTravelMode::TradeLane;
	SpoolRemaining = 0.f;
	Owner->SetActorRotation(TravelDir.Rotation());
}

void UHDShipMovementComponent::TeleportAuthority(const FVector& Location, const FQuat& Rotation, bool bResetVelocity)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority()) return;
	Owner->SetActorLocationAndRotation(Location, Rotation, false, nullptr, ETeleportType::TeleportPhysics);
	if (bResetVelocity) { Velocity = FVector::ZeroVector; AngularVelocity = FVector::ZeroVector; }
	ServerState = CaptureState(FMath::Max(LastServerTimestamp, 0.f));
	const APawn* Pawn = Cast<APawn>(Owner);
	if (Pawn && !Pawn->IsLocallyControlled()) { ClientTeleport(ServerState); }
}

void UHDShipMovementComponent::ClientTeleport_Implementation(const FHDShipState& State)
{
	AActor* Owner = GetOwner();
	if (!Owner) return;
	// Drop predicted moves: they were made from the old place.
	Unacked.Reset();
	Owner->SetActorLocationAndRotation(State.Location, State.Rotation, false, nullptr, ETeleportType::TeleportPhysics);
	Velocity = State.Velocity;
	AngularVelocity = State.AngularVelocity;
}
