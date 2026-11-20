#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "HDUniverseTypes.h"
#include "HDNavComponent.generated.h"

class AHDShipPawn;
class AHDSpaceObject;
class AHDCelestialBody;
class AHDStationActor;
class AHDJumpPointActor;
class AHDTradeLaneActor;
class AHDPlayerController;

DECLARE_MULTICAST_DELEGATE_TwoParams(FHDOnTollPaid, FName /*Owner*/, int32 /*Amount*/);
DECLARE_MULTICAST_DELEGATE_OneParam(FHDOnArrived, int32 /*System*/);

UENUM(BlueprintType)
enum class EHDPendingJump : uint8 { None, Gate, Wormhole, Anomaly, InstanceLane, CapitalDrive };

/** Landing-corridor readout for the HUD (doc 17 §7C.2a). */
USTRUCT(BlueprintType)
struct FHDCorridorStatus
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Landing") bool bRestricted = false;
	UPROPERTY(BlueprintReadOnly, Category = "Landing") bool bOpen = true;
	UPROPERTY(BlueprintReadOnly, Category = "Landing") int32 ActiveCorridor = 0;
	UPROPERTY(BlueprintReadOnly, Category = "Landing") float DegreesOff = 0.f;
	UPROPERTY(BlueprintReadOnly, Category = "Landing") float SecondsLeft = 0.f;
};

/**
 * Travel orchestration for a ship (doc 13, 17): fuel, docking, landing, trade lanes, gate/wormhole/anomaly/lane jumps,
 * capital self-jumps with fuel per ly and drive strain, and signature scanning. Server-authoritative; the movement
 * component does the flying, this component decides mode changes.
 */
UCLASS(ClassGroup = (Ship), meta = (BlueprintSpawnableComponent))
class HELIOSDRIFT_API UHDNavComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	UHDNavComponent();
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	// ---- Server requests (from AHDPlayerController commands or AI) ----
	void RequestDock(AActor* Target);
	void RequestLand();
	void RequestCruise();
	/** Context action: undock / take off / dock / gate or lane / capital jump, in that priority. */
	void RequestInteract(AActor* Target);
	void RequestJump(AActor* Target);
	void RequestLane(AHDTradeLaneActor* Lane);
	void RequestScan();
	void SetJumpTarget(int32 System);
	/** Server: refuel from a docked station (fuel platforms sell jump fuel). Fraction 0..1 of capacity. */
	void Refuel(float Fraction);

	// ---- Replicated state ----
	/** In-system fuel (t): burned by cruise and slipstream. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Fuel") float ShipFuel = 60.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Fuel") float MaxShipFuel = 60.f;
	/** Capital jump fuel (t). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Fuel") float JumpFuel = 0.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Fuel") float MaxJumpFuel = 0.f;
	/** Unix time drive strain runs out (strain decays in real time, cap 5 h). */
	UPROPERTY(Replicated) double StrainUntil = 0.0;
	/** Unix time the jump drive can fire again (spool-down, cap 30 min). */
	UPROPERTY(Replicated) double CooldownUntil = 0.0;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Jump") int32 JumpTarget = -1;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Jump") EHDPendingJump Pending = EHDPendingJump::None;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Jump") float JumpCountdown = 0.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Dock") TObjectPtr<AActor> DockedAt;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Dock") TObjectPtr<AActor> LandedOn;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Scan") float ScanCountdown = 0.f;

	// ---- Tuning ----
	UPROPERTY(EditAnywhere, Category = "Fuel") float CruiseFuelPerSecond = 0.02f;
	UPROPERTY(EditAnywhere, Category = "Fuel") float SlipFuelPerSecond = 0.01f;
	UPROPERTY(EditAnywhere, Category = "Jump") float GateSpoolSeconds = 3.f;
	UPROPERTY(EditAnywhere, Category = "Jump") float DriveSpoolSeconds = 10.f;
	UPROPERTY(EditAnywhere, Category = "Dock") float MaxDockSpeed = 20000.f; // 200 m/s
	UPROPERTY(EditAnywhere, Category = "Landing") float LandMaxAltitude = 150000.f; // 1.5 km
	UPROPERTY(EditAnywhere, Category = "Landing") float LandMaxSpeed = 10000.f; // 100 m/s
	UPROPERTY(EditAnywhere, Category = "Lane") float LaneSpeed = 1.0e9f; // 10,000 km/s
	UPROPERTY(EditAnywhere, Category = "Scan") float ScanSeconds = 6.f;
	/** Scan range in AU at strength 1. */
	UPROPERTY(EditAnywhere, Category = "Scan") float ScanRangeAU = 12.f;
	/** Multiplier from scanner modules (set by fitting). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Scan") float ScanStrength = 1.f;

	// ---- Queries (both sides) ----
	float GetStrainMinutes() const;
	float GetCooldownMinutes() const;
	/** Max self-jump range (ly) after skills; 0 for gate-bound hulls. */
	float GetJumpRangeLy() const;
	/** Jump fuel needed for Ly after skills. */
	float GetJumpFuelFor(float Ly) const;
	/** Nearest object we could interact with right now, and a prompt like "Dock at X". */
	AHDSpaceObject* FindInteractable(FString& OutPrompt) const;
	/** Nearest celestial body in our system and our altitude above it (cm). */
	AHDCelestialBody* FindNearestBody(double& OutAltitude) const;

	/** Corridor check for developed worlds. NowUnix keeps server and clients on the same published cycle. */
	static FHDCorridorStatus CheckCorridor(const AHDCelestialBody* Body, const FVector& ShipWorld, double NowUnix);
	UPROPERTY(EditAnywhere, Category = "Landing") float CorridorCycleSeconds = 180.f;

	/** Server events (territory toll income, missions, AI). */
	FHDOnTollPaid OnTollPaid;
	FHDOnArrived OnArrived;

	/** Server: called by movement when the ship enters/leaves glide etc. */
	void Notify(const FString& Msg) const;

private:
	AHDShipPawn* Ship() const;
	void CompleteJump();
	void CompleteScan();
	bool CanUseLink(const AHDJumpPointActor* J, FString& Why) const;
	void ChargeToll(int32 Toll, const FName& TollCorp);

	/** Server-only: pending jump details. */
	int32 PendingSystem = -1;
	FName PendingLinkId;
	FVector PendingLocal = FVector::ZeroVector;
	float PendingLy = 0.f;
	/** Server-only: partial scan progress per signature. */
	TMap<FName, float> ScanProgress;
};
