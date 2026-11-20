#pragma once
#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "HDUniverseTypes.h"
#include "HDPlayerController.generated.h"

class UInputMappingContext;
class UInputAction;
class ADirectionalLight;
class AHDSpaceObject;

/** Local map overlay state (M / hold View). */
enum class EHDMapMode : uint8 { Closed, Galaxy, System };

/** Runtime-built Enhanced Input actions (no .uasset needed). */
USTRUCT()
struct FHDFlightInput
{
	GENERATED_BODY()
	UPROPERTY(Transient) TObjectPtr<UInputMappingContext> Context;
	/** Rate axes (-1..1): keyboard, triggers, left stick. */
	UPROPERTY(Transient) TObjectPtr<UInputAction> Thrust;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Strafe;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Vertical;
	/** Rate axes (-1..1): arrow keys only. */
	UPROPERTY(Transient) TObjectPtr<UInputAction> Pitch;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Yaw;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Roll;
	/** Right stick (Axis2D, rate-based: X = yaw, Y = pitch). */
	UPROPERTY(Transient) TObjectPtr<UInputAction> LookStick;
	/** Mouse (Axis2D, delta-based: counts moved this frame). */
	UPROPERTY(Transient) TObjectPtr<UInputAction> MouseAim;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Fire;
	UPROPERTY(Transient) TObjectPtr<UInputAction> ToggleFA;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Slipstream;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PowerBalanced;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PowerAttack;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PowerTank;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PowerRun;
	UPROPERTY(Transient) TObjectPtr<UInputAction> EqualizeShields;
	UPROPERTY(Transient) TObjectPtr<UInputAction> RecenterVR;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Menu;
	// ---- Travel (M2). Keyboard actions fire on press. ----
	UPROPERTY(Transient) TObjectPtr<UInputAction> Cruise;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Interact;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Dock;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Land;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Jump;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Map;
	UPROPERTY(Transient) TObjectPtr<UInputAction> Scan;
	UPROPERTY(Transient) TObjectPtr<UInputAction> TargetNext;
	UPROPERTY(Transient) TObjectPtr<UInputAction> MapNext;
	UPROPERTY(Transient) TObjectPtr<UInputAction> MapPrev;
	UPROPERTY(Transient) TObjectPtr<UInputAction> MapSelect;
	// ---- Gamepad tap / hold pairs (bound on Triggered). Y, B, X, View. ----
	UPROPERTY(Transient) TObjectPtr<UInputAction> PadYTap;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PadYHold;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PadBTap;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PadBHold;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PadXTap;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PadXHold;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PadViewTap;
	UPROPERTY(Transient) TObjectPtr<UInputAction> PadViewHold;
};

UCLASS(Config = Game)
class HELIOSDRIFT_API AHDPlayerController : public APlayerController
{
	GENERATED_BODY()
public:
	/** Builds actions + mapping context on first call (local controller only) and registers the context. */
	const FHDFlightInput& GetFlightInput();

	/** Multiplier on right-stick look rate (1 = full stick gives the ship's max turn rate). Settable in DefaultGame.ini or with the console. */
	UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Input|Gamepad", meta = (ClampMin = "0.1", ClampMax = "3.0"))
	float StickSensitivity = 1.0f;

	/** Response-curve exponent for both sticks (1 = linear, 2 = fine control near centre). Applied when the context is built. */
	UPROPERTY(EditAnywhere, Config, Category = "Input|Gamepad", meta = (ClampMin = "1.0", ClampMax = "4.0"))
	float StickCurveExponent = 2.0f;

	/** Inner deadzone for sticks and triggers (0..1). Applied when the context is built. */
	UPROPERTY(EditAnywhere, Config, Category = "Input|Gamepad", meta = (ClampMin = "0.0", ClampMax = "0.5"))
	float StickDeadZone = 0.15f;

	UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Input|Gamepad")
	bool bInvertStickPitch = false;

	/** Degrees of ship rotation per mouse count (delta-based; independent of frame rate). */
	UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Input|Mouse", meta = (ClampMin = "0.001", ClampMax = "2.0"))
	float MouseDegreesPerCount = 0.15f;

	UPROPERTY(EditAnywhere, Config, BlueprintReadWrite, Category = "Input|Mouse")
	bool bInvertMousePitch = false;

	/** Console: HDStickSensitivity 1.5 */
	UFUNCTION(Exec) void HDStickSensitivity(float Value);
	/** Console: HDMouseSensitivity 0.15 (degrees per count) */
	UFUNCTION(Exec) void HDMouseSensitivity(float Value);

	/** Toggled by Menu / Esc. M1 shows the controls overlay in the HUD; a real menu comes later. */
	UFUNCTION(BlueprintCallable, Category = "UI") void ToggleMenu() { bMenuOpen = !bMenuOpen; }
	UFUNCTION(BlueprintPure, Category = "UI") bool IsMenuOpen() const { return bMenuOpen; }

	// ---- Server command channel: every gameplay request from input or console goes through here ----
	/** Client -> server. Cmd is a verb ("goto", "dock", "buy"...), Arg its free-form argument, Target an optional replicated actor. */
	UFUNCTION(Server, Reliable) void ServerCommand(const FString& Cmd, const FString& Arg, AActor* Target);
	/** Server -> owning client: on-screen message. */
	UFUNCTION(Client, Reliable) void ClientNotify(const FString& Msg);
	/** Server-side helper: message this pilot (works for listen-server hosts too). */
	void Notify(const FString& Msg);
	/** Local: send a command (runs ServerCommand). */
	void Cmd(const FString& Verb, const FString& Arg = FString(), AActor* Target = nullptr);

	/** Recent messages for the HUD (local only). */
	TArray<FString> Messages;
	TArray<float> MessageTimes;

	// ---- Console commands (` key). See docs/WORLD-TESTING.md ----
	/** HDGoto <system id, number or name>: teleport your ship to a system's arrival point. */
	UFUNCTION(Exec) void HDGoto(const FString& System);
	/** HDSystemInfo: print the current system (bodies, stations, links, sites, lanes, fields). */
	UFUNCTION(Exec) void HDSystemInfo();
	/** HDGalaxy <radius ly>: list systems near you. */
	UFUNCTION(Exec) void HDGalaxy(const FString& RadiusLy);
	/** HDFind <name>: find a system by name or id. */
	UFUNCTION(Exec) void HDFind(const FString& Name);
	/** HDWeek <n|live>: override the week index (gas-giant and wormhole re-rolls). */
	UFUNCTION(Exec) void HDWeek(const FString& Week);
	/** HDRevealAll: reveal every hidden signature in this system (testing). */
	UFUNCTION(Exec) void HDRevealAll();
	/** HDGas: list gas giants here with this week's density and the next 4 weeks' rolls. */
	UFUNCTION(Exec) void HDGas();
	/** HDCmd <verb> <arg>: send any server command directly. */
	UFUNCTION(Exec) void HDCmd(const FString& Verb, const FString& Arg);

	// ---- Maps and targeting (local) ----
	/** Closed -> galaxy -> system -> closed. */
	void CycleMap();
	void CloseMap() { MapMode = EHDMapMode::Closed; }
	bool IsMapOpen() const { return MapMode != EHDMapMode::Closed; }
	EHDMapMode GetMapMode() const { return MapMode; }
	/** Move the map cursor (+1 / -1). */
	void MapStep(int32 Dir);
	/** Galaxy map: zoom radius in/out. */
	void MapZoom(int32 Dir);
	/** Galaxy: set jump target + plot route. System: select the object as target. */
	void MapSelect();
	/** Cycle the selected target through visible objects in this system, nearest first. */
	void CycleTarget();
	AActor* GetSelectedTarget() const { return SelectedTarget.Get(); }
	void SetSelectedTarget(AActor* A) { SelectedTarget = A; }
	/** Galaxy map contents, refreshed when opened / zoomed. */
	TArray<FHDMapEntry> GalaxyEntries;
	/** System map contents (visible objects in our system). */
	TArray<TWeakObjectPtr<AHDSpaceObject>> SystemEntries;
	int32 MapCursor = 0;
	float MapRadiusLy = 20.f;
	/** Last plotted route (system indices, start first). */
	TArray<int32> Route;
	int32 RouteDestination = -1;
	/** HDMap: toggle the map from the console. HDRoute <system>: plot a route without the map. */
	UFUNCTION(Exec) void HDMap();
	UFUNCTION(Exec) void HDRoute(const FString& System);

	/** Server: handle one command. Implemented in HDCommands.cpp. */
	void HandleCommand(const FString& Verb, const FString& Arg, AActor* Target);
	/** Travel verbs (HDTravelCommands.cpp). Returns true if handled. */
	bool HandleTravelCommand(const FString& Verb, const FString& Arg, AActor* Target);
	/** Ships, economy, NPCs, territory verbs (HDGameplayCommands.cpp). Returns true if handled. */
	bool HandleGameplayCommand(const FString& Verb, const FString& Arg, AActor* Target);
	/** M3 fitting / hangar verbs (HDShipCommands.cpp). */
	bool HandleShipCommand(const FString& Verb, const FString& Arg, AActor* Target);
	/** M4 economy verbs (HDEconomyCommands.cpp). */
	bool HandleEconomyCommand(const FString& Verb, const FString& Arg, AActor* Target);
	/** M5 NPC / law verbs (HDNpcCommands.cpp). */
	bool HandleNpcCommand(const FString& Verb, const FString& Arg, AActor* Target);
	/** M6 territory / building verbs (HDTerritoryCommands.cpp). */
	bool HandleTerritoryCommand(const FString& Verb, const FString& Arg, AActor* Target);
	/** Multi-line helper. */
	void NotifyLines(const FString& Text);

protected:
	virtual void BeginPlay() override;
	virtual void PlayerTick(float DeltaTime) override;

	/** Local-only key light pointing from the current system's star (no level lighting needed). */
	UPROPERTY(Transient) TObjectPtr<ADirectionalLight> StarLight;

	/** Optional: assign an asset-based context in a BP subclass to add on top of / replace the runtime one. */
	UPROPERTY(EditDefaultsOnly, Category = "Input")
	TObjectPtr<UInputMappingContext> FlightMapping;

	UPROPERTY(Transient) FHDFlightInput Flight;

private:
	void BuildFlightInput();
	bool bFlightBuilt = false;
	bool bMenuOpen = false;
	EHDMapMode MapMode = EHDMapMode::Closed;
	TWeakObjectPtr<AActor> SelectedTarget;
	void RefreshMapEntries();
	void PlotRoute(int32 Dest);
};
