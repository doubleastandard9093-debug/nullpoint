#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "HDShipTypes.h"
#include "HDShipPawn.generated.h"

class UBoxComponent;
class UStaticMeshComponent;
class UHDShipPowerComponent;
class UHDShieldComponent;
class UHDShipMovementComponent;
class UHDLaserComponent;
class UHDNavComponent;
class AHDPlayerController;
class USceneComponent;
class UCameraComponent;
struct FInputActionValue;

/** Player ship: 6DoF movement, power, 4-facing shields, laser, cockpit camera (flat + VR). */
UCLASS()
class HELIOSDRIFT_API AHDShipPawn : public APawn
{
	GENERATED_BODY()
public:
	AHDShipPawn();
	virtual float TakeDamage(float Damage, struct FDamageEvent const& Event, AController* Instigator, AActor* Causer) override;
	virtual void SetupPlayerInputComponent(UInputComponent* PlayerInputComponent) override;
	virtual FVector GetVelocity() const override;
	virtual void BeginPlay() override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship") TObjectPtr<UBoxComponent> Collision;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship") TObjectPtr<UStaticMeshComponent> Hull;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship") TObjectPtr<UHDShipPowerComponent> Power;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship") TObjectPtr<UHDShieldComponent> Shields;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship") TObjectPtr<UHDShipMovementComponent> Movement;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship") TObjectPtr<UHDLaserComponent> Laser;
	/** Fuel, docking, landing, lanes, jumps, scanning (M2). */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Ship") TObjectPtr<UHDNavComponent> Nav;
	/** Hull size class: decides gates vs. jump drive (doc 13/14). Set by fitting / hull swap. */
	UPROPERTY(EditDefaultsOnly, Replicated, BlueprintReadOnly, Category = "Ship") EHDHullClass HullClass = EHDHullClass::Frigate;
	UPROPERTY(EditDefaultsOnly, Replicated, BlueprintReadOnly, Category = "Ship") float HullHP = 1000.f;
	UPROPERTY(EditDefaultsOnly, Category = "Ship") float MaxHullHP = 1000.f;

	/** Power presets (W, S, E, A), 0..100 each. Keys 1-4 / D-pad up, right, down, left. */
	UPROPERTY(EditDefaultsOnly, Category = "Power") FVector4 PresetBalanced = FVector4(50.f, 50.f, 50.f, 50.f);
	UPROPERTY(EditDefaultsOnly, Category = "Power") FVector4 PresetAttack = FVector4(100.f, 50.f, 25.f, 25.f);
	UPROPERTY(EditDefaultsOnly, Category = "Power") FVector4 PresetTank = FVector4(25.f, 100.f, 50.f, 25.f);
	UPROPERTY(EditDefaultsOnly, Category = "Power") FVector4 PresetRun = FVector4(25.f, 50.f, 100.f, 25.f);

	/** Pilot eye anchor in the cockpit; HMD pose is applied relative to this. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cockpit") TObjectPtr<USceneComponent> CockpitRoot;
	/** Locked to HMD in VR; fixed at the eye anchor on flat screens. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Cockpit") TObjectPtr<UCameraComponent> CockpitCamera;

	/** Reset HMD orientation/position to the seat. No-op without an HMD. */
	UFUNCTION(BlueprintCallable, Category = "VR") void RecenterVR();
	UFUNCTION(BlueprintPure, Category = "VR") static bool IsVRActive();

	virtual void NotifyControllerChanged() override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Universe system this ship is in (-1 = none yet). Set by the system director. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Universe") int32 SystemIndex = -1;
	/** World position of that system's star (systems share the world in slots). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Universe") FVector SystemOrigin = FVector::ZeroVector;
	/** Server: move bookkeeping to a new system (the director positions the ship). */
	void SetSystem(int32 InSystem, const FVector& InOrigin) { SystemIndex = InSystem; SystemOrigin = InOrigin; }
	/** Position relative to the current system's star. */
	FVector GetLocalPosition() const { return GetActorLocation() - SystemOrigin; }

private:
	void OnThrust(const FInputActionValue& V);
	void OnStrafe(const FInputActionValue& V);
	void OnVertical(const FInputActionValue& V);
	void OnPitch(const FInputActionValue& V);
	void OnYaw(const FInputActionValue& V);
	void OnRoll(const FInputActionValue& V);
	void OnLookStick(const FInputActionValue& V);
	void OnMouseAim(const FInputActionValue& V);
	void OnMenu(const FInputActionValue& V);
	void OnFireStart(const FInputActionValue& V);
	void OnFireStop(const FInputActionValue& V);
	void OnToggleFA(const FInputActionValue& V);
	void OnSlipstream(const FInputActionValue& V);
	void OnPowerBalanced(const FInputActionValue& V);
	void OnPowerAttack(const FInputActionValue& V);
	void OnPowerTank(const FInputActionValue& V);
	void OnPowerRun(const FInputActionValue& V);
	void OnEqualize(const FInputActionValue& V);
	void OnRecenter(const FInputActionValue& V);
	void SetPower(const FVector4& WSEA);
	// Travel (M2)
	void OnCruise(const FInputActionValue& V);
	void OnInteract(const FInputActionValue& V);
	void OnDock(const FInputActionValue& V);
	void OnLand(const FInputActionValue& V);
	void OnJump(const FInputActionValue& V);
	void OnMap(const FInputActionValue& V);
	void OnScan(const FInputActionValue& V);
	void OnTargetNext(const FInputActionValue& V);
	void OnMapNext(const FInputActionValue& V);
	void OnMapPrev(const FInputActionValue& V);
	void OnMapSelect(const FInputActionValue& V);
	void OnViewTap(const FInputActionValue& V);
	/** Local helper: send a command through our controller. */
	void SendCmd(const TCHAR* Verb, AActor* Target = nullptr);
	AHDPlayerController* GetHDController() const;
};
