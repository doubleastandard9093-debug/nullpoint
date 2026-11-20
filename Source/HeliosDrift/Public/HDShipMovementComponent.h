#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HDShipMovementComponent.generated.h"

/**
 * Travel ladder (doc 13, 17). Normal = sub space (throttle). Slipstream = locked-target in-system travel.
 * Cruise = free in-system supercruise, speed limited by nearby gravity wells. Glide = unpowered descent
 * phase between 25 km and 3 km. TradeLane = riding a lane. Docked/Landed/Jumping hold the ship still.
 */
UENUM(BlueprintType)
enum class EHDTravelMode : uint8 { Normal, Spooling, Slipstream, Cruise, Glide, TradeLane, Docked, Landed, Jumping };

class AHDCelestialBody;

/** One client input frame, sent to the server. */
USTRUCT()
struct FHDShipMove
{
	GENERATED_BODY()
	UPROPERTY() float Timestamp = 0.f;
	UPROPERTY() float DeltaTime = 0.f;
	/** X forward, Y right, Z up, each -1..1 */
	UPROPERTY() FVector Linear = FVector::ZeroVector;
	/** X pitch, Y yaw, Z roll, each -1..1 */
	UPROPERTY() FVector Rot = FVector::ZeroVector;
	UPROPERTY() bool bFlightAssist = true;
};

/** Authoritative kinematic state. */
USTRUCT()
struct FHDShipState
{
	GENERATED_BODY()
	UPROPERTY() float Timestamp = 0.f;
	UPROPERTY() FVector Location = FVector::ZeroVector;
	UPROPERTY() FQuat Rotation = FQuat::Identity;
	UPROPERTY() FVector Velocity = FVector::ZeroVector;
	UPROPERTY() FVector AngularVelocity = FVector::ZeroVector;
};

/**
 * 6DoF ship movement (doc 02). Owning client predicts and sends moves; server simulates
 * authoritatively and acks; client rewinds to the ack and replays unacked moves.
 * Simulated proxies interpolate toward replicated ServerState.
 */
UCLASS(ClassGroup = (Ship), meta = (BlueprintSpawnableComponent))
class HELIOSDRIFT_API UHDShipMovementComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHDShipMovementComponent();
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Local rate input (-1..1), written by the pawn's input handlers (keyboard, triggers, left stick, arrows, bumpers). */
	FVector InputLinear = FVector::ZeroVector;
	FVector InputRot = FVector::ZeroVector;
	/** Right-stick rate input (X pitch, Y yaw, -1..1), already scaled by stick sensitivity. Rate-based: held deflection = held turn rate. */
	FVector2D StickRot = FVector2D::ZeroVector;

	/** Delta-based mouse aim: degrees of (pitch, yaw) requested since the last tick. Converted to a rate for exactly one tick. */
	void AddMouseAimDegrees(const FVector2D& PitchYawDegrees) { PendingMouseDegrees += PitchYawDegrees; }

	UPROPERTY(BlueprintReadOnly, Category = "Flight") bool bFlightAssist = true;
	UFUNCTION(BlueprintCallable, Category = "Flight") void ToggleFlightAssist() { bFlightAssist = !bFlightAssist; }

	UFUNCTION(Server, Reliable, BlueprintCallable, Category = "Flight") void ServerToggleSlipstream();
	/** Server: start spooling into Cruise (or drop out of Cruise). */
	void ToggleCruiseAuthority();
	/** Server: set a mode directly (dock, land, lane, jump hold). Clears spool state. */
	void SetModeAuthority(EHDTravelMode Mode);
	/** Server: ride a lane toward Exit at Speed. */
	void StartLaneAuthority(const FVector& Exit, float Speed);
	void SetVelocityAuthority(const FVector& NewVelocity) { Velocity = NewVelocity; }
	/** Can the ship fire / mine / scoop in its current mode? */
	bool IsInSubSpace() const { return TravelMode == EHDTravelMode::Normal; }
	/** Current speed limit in Cruise at a world location (gravity wells). */
	float GetCruiseLimitAt(const FVector& WorldLoc) const;
	/** Nearest body in this ship's system (cached ~0.5 s) and altitude above it. */
	AHDCelestialBody* GetNearestBody(double& OutAltitude) const;
	/** Gravity acceleration (cm/s^2, world) at a location in surface flight; zero far from bodies. */
	FVector GetGravityAt(const FVector& WorldLoc) const;
	/** Server: leave slipstream/spool. Interdicted adds a cooldown. */
	void DropOut(bool bInterdicted);

	/** Server: hard-move the ship (system change, dock, land) and tell the owning client to snap there. */
	void TeleportAuthority(const FVector& Location, const FQuat& Rotation, bool bResetVelocity);

	UFUNCTION(BlueprintPure, Category = "Flight") EHDTravelMode GetTravelMode() const { return TravelMode; }
	UFUNCTION(BlueprintPure, Category = "Flight") FVector GetShipVelocity() const { return Velocity; }
	UFUNCTION(BlueprintPure, Category = "Flight") float GetSpeedCap() const;
	UFUNCTION(BlueprintPure, Category = "Flight") float GetSpoolRemaining() const { return SpoolRemaining; }

	// Tuning (cm, cm/s, deg/s)
	UPROPERTY(EditAnywhere, Category = "Flight") float BaseMaxSpeed = 30000.f;
	UPROPERTY(EditAnywhere, Category = "Flight") float LinearAccel = 15000.f;
	UPROPERTY(EditAnywhere, Category = "Flight") FVector MaxAngularRate = FVector(60.f, 45.f, 90.f);
	UPROPERTY(EditAnywhere, Category = "Flight") float AngularAccel = 180.f;
	UPROPERTY(EditAnywhere, Category = "Travel") float SlipSpeed = 2000000.f;
	UPROPERTY(EditAnywhere, Category = "Travel") float SpoolTime = 4.f;
	UPROPERTY(EditAnywhere, Category = "Travel") float DropDistance = 50000.f;
	UPROPERTY(EditAnywhere, Category = "Travel") float InterdictCooldown = 10.f;
	/** Fraction of normal turn rate available in slipstream. */
	UPROPERTY(EditAnywhere, Category = "Travel", meta = (ClampMin = "0", ClampMax = "1")) float SlipTurnScale = 0.25f;
	/** Speed on dropping out of slipstream, as a fraction of BaseMaxSpeed. */
	UPROPERTY(EditAnywhere, Category = "Travel", meta = (ClampMin = "0", ClampMax = "1")) float DropOutSpeedFraction = 0.5f;
	UPROPERTY(EditAnywhere, Category = "Travel") float SlipMaxSpeed = 3.0e9f;
	/** Slipstream speed = distance to target * this (per second), clamped to SlipSpeed..SlipMaxSpeed. */
	UPROPERTY(EditAnywhere, Category = "Travel") float SlipDistanceRate = 0.5f;
	// Cruise (in-system supercruise)
	UPROPERTY(EditAnywhere, Category = "Cruise") float CruiseMinSpeed = 100000.f;   // 1 km/s
	UPROPERTY(EditAnywhere, Category = "Cruise") float CruiseMaxSpeed = 2.0e9f;     // 20,000 km/s
	/** Speed limit = altitude above the nearest body * this (per second). 0.1 gives 2.5 km/s at 25 km. */
	UPROPERTY(EditAnywhere, Category = "Cruise") float CruiseWellRate = 0.1f;
	UPROPERTY(EditAnywhere, Category = "Cruise") float CruiseAccelRate = 0.6f;
	UPROPERTY(EditAnywhere, Category = "Cruise") float CruiseTurnScale = 0.35f;
	UPROPERTY(EditAnywhere, Category = "Cruise") float CruiseSpoolTime = 3.f;
	// Landing (doc 17 first-pass numbers)
	UPROPERTY(EditAnywhere, Category = "Landing") float OrbitalFloorAltitude = 2500000.f; // 25 km
	UPROPERTY(EditAnywhere, Category = "Landing") float SurfaceFlightAltitude = 300000.f;  // 3 km
	UPROPERTY(EditAnywhere, Category = "Landing") float GlideSpeed = 250000.f;             // 2.5 km/s
	UPROPERTY(EditAnywhere, Category = "Landing") float GlideMaxEntrySpeed = 400000.f;     // arriving faster = emergency drop
	UPROPERTY(EditAnywhere, Category = "Landing") float GlideMinPitch = -60.f;
	UPROPERTY(EditAnywhere, Category = "Landing") float GlideMaxPitch = -5.f;
	UPROPERTY(EditAnywhere, Category = "Landing") float GlideTurnScale = 0.4f;
	UPROPERTY(EditAnywhere, Category = "Landing") float EmergencyDropDamage = 250.f;
	/** Gravity applies below this altitude (cm). */
	UPROPERTY(EditAnywhere, Category = "Landing") float GravityAltitude = 5000000.f;     // 50 km
	UPROPERTY(EditAnywhere, Category = "Landing") float GravityScale = 1.f;

	UFUNCTION(BlueprintPure, Category = "Flight") float GetTravelSpeed() const { return TravelSpeed; }
	/** Pitch (deg) relative to the local horizon of the nearest body; negative = nose down. */
	float GetPitchToHorizon(double& OutAltitude) const;

	/** Slipstream only locks targets within this cone (cosine of half-angle). */
	UPROPERTY(EditAnywhere, Category = "Travel", meta = (ClampMin = "0", ClampMax = "1")) float SlipTargetMinDot = 0.95f;

	// Networking
	/** Longest single simulation step (s). Longer frames are clamped. */
	UPROPERTY(EditAnywhere, Category = "Network") float MaxMoveDeltaTime = 0.1f;
	/** Max client moves kept for replay. */
	UPROPERTY(EditAnywhere, Category = "Network") int32 MaxUnackedMoves = 240;
	/** Simulated proxies snap instead of smoothing beyond this error (cm). */
	UPROPERTY(EditAnywhere, Category = "Network") float ProxySnapDistance = 100000.f;
	/** Simulated proxy smoothing speed (1/s); frame-rate independent. */
	UPROPERTY(EditAnywhere, Category = "Network") float ProxySmoothingRate = 12.f;
	/** Server: how far (s) a client's simulated time may run ahead of real time before moves are trimmed (anti speed-hack). */
	UPROPERTY(EditAnywhere, Category = "Network") float MaxClientTimeAhead = 0.25f;

protected:
	UFUNCTION(Server, Unreliable) void ServerMove(const FHDShipMove& Move);
	UFUNCTION(Client, Unreliable) void ClientAck(const FHDShipState& State);
	UFUNCTION(Client, Reliable) void ClientTeleport(const FHDShipState& State);

	UPROPERTY(Replicated) EHDTravelMode TravelMode = EHDTravelMode::Normal;
	/** Slipstream / lane speed, set by the server and replicated so the owner predicts the same motion. */
	UPROPERTY(Replicated) float TravelSpeed = 0.f;
	/** Lane direction (unit). */
	UPROPERTY(Replicated) FVector TravelDir = FVector::ForwardVector;
	UPROPERTY(Replicated) FHDShipState ServerState;

private:
	void SimulateMove(const FHDShipMove& Move);
	void UpdateTravelAuthority(float DeltaTime);
	AActor* PickSlipTarget() const;
	FHDShipState CaptureState(float Timestamp) const;

	FVector Velocity = FVector::ZeroVector;
	FVector AngularVelocity = FVector::ZeroVector;
	TArray<FHDShipMove> Unacked;
	float ClientTime = 0.f;
	float LastServerTimestamp = -1.f;
	float LastAckTimestamp = -1.f;
	float SpoolRemaining = 0.f;
	float SlipCooldown = 0.f;
	TWeakObjectPtr<AActor> TravelTarget;
	/** Mode Spooling will enter when done (server only). */
	EHDTravelMode SpoolInto = EHDTravelMode::Slipstream;
	FVector LaneExit = FVector::ZeroVector;
	void UpdateLandingAuthority();
	void EmergencyDrop(const TCHAR* Why);
	mutable TWeakObjectPtr<AHDCelestialBody> CachedBody;
	mutable float CachedBodyTime = -100.f;
	FVector2D PendingMouseDegrees = FVector2D::ZeroVector;
	/** Server: real time granted to the owning client minus simulated time it has used. */
	float ServerTimeBudget = 0.f;
};
