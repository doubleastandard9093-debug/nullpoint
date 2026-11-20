#include "HDPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputAction.h"
#include "InputModifiers.h"
#include "InputTriggers.h"
#include "InputCoreTypes.h"
#include "Engine/LocalPlayer.h"
#include "Engine/DirectionalLight.h"
#include "Components/LightComponent.h"
#include "Engine/World.h"
#include "HDShipPawn.h"
#include "HDShipTypes.h"
#include "HDNavComponent.h"
#include "HDWorldActors.h"
#include "HDPlayerState.h"
#include "HDUniverseSubsystem.h"
#include "EngineUtils.h"

namespace
{
	UInputAction* MakeAction(UObject* Outer, const TCHAR* Name, EInputActionValueType Type)
	{
		UInputAction* A = NewObject<UInputAction>(Outer, FName(Name));
		A->ValueType = Type;
		return A;
	}

	FEnhancedActionKeyMapping& MapKeyTo(UInputMappingContext* Ctx, UInputAction* Action, const FKey& Key, float Scale = 1.f)
	{
		FEnhancedActionKeyMapping& M = Ctx->MapKey(Action, Key);
		if (!FMath::IsNearlyEqual(Scale, 1.f))
		{
			UInputModifierScalar* S = NewObject<UInputModifierScalar>(Ctx);
			S->Scalar = FVector(Scale, Scale, Scale);
			M.Modifiers.Add(S);
		}
		return M;
	}

	/** Gamepad tap: fires on release within Threshold seconds. */
	void MapTap(UInputMappingContext* Ctx, UInputAction* Action, const FKey& Key, float Threshold = 0.3f)
	{
		FEnhancedActionKeyMapping& M = Ctx->MapKey(Action, Key);
		UInputTriggerTap* Tap = NewObject<UInputTriggerTap>(Ctx);
		Tap->TapReleaseTimeThreshold = Threshold;
		M.Triggers.Add(Tap);
	}

	/** Gamepad hold: fires once after Threshold seconds held. */
	void MapHold(UInputMappingContext* Ctx, UInputAction* Action, const FKey& Key, float Threshold = 0.45f)
	{
		FEnhancedActionKeyMapping& M = Ctx->MapKey(Action, Key);
		UInputTriggerHold* Hold = NewObject<UInputTriggerHold>(Ctx);
		Hold->HoldTimeThreshold = Threshold;
		Hold->bIsOneShot = true;
		M.Triggers.Add(Hold);
	}

	/** Analog key: deadzone first, then response curve, then optional sign/scale. */
	void MapAnalog(UInputMappingContext* Ctx, UInputAction* Action, const FKey& Key, float DeadZone, float CurveExp, EDeadZoneType DZType, FVector Scale = FVector::OneVector)
	{
		FEnhancedActionKeyMapping& M = Ctx->MapKey(Action, Key);
		UInputModifierDeadZone* DZ = NewObject<UInputModifierDeadZone>(Ctx);
		DZ->LowerThreshold = DeadZone;
		DZ->UpperThreshold = 1.f;
		DZ->Type = DZType;
		M.Modifiers.Add(DZ);
		if (!FMath::IsNearlyEqual(CurveExp, 1.f))
		{
			UInputModifierResponseCurveExponential* Curve = NewObject<UInputModifierResponseCurveExponential>(Ctx);
			Curve->CurveExponent = FVector(CurveExp, CurveExp, 1.f);
			M.Modifiers.Add(Curve);
		}
		if (!Scale.Equals(FVector::OneVector))
		{
			UInputModifierScalar* S = NewObject<UInputModifierScalar>(Ctx);
			S->Scalar = Scale;
			M.Modifiers.Add(S);
		}
	}
}

void AHDPlayerController::BeginPlay()
{
	Super::BeginPlay();
	if (IsLocalController())
	{
		GetFlightInput();
		if (UWorld* World = GetWorld())
		{
			FActorSpawnParameters P;
			P.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			P.ObjectFlags |= RF_Transient;
			StarLight = World->SpawnActor<ADirectionalLight>(ADirectionalLight::StaticClass(), FTransform::Identity, P);
			if (StarLight && StarLight->GetLightComponent())
			{
				StarLight->GetLightComponent()->SetMobility(EComponentMobility::Movable);
				StarLight->GetLightComponent()->SetIntensity(4.f);
			}
		}
	}
}

void AHDPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);
	if (StarLight)
	{
		if (const AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn()))
		{
			const FVector Dir = (Ship->GetActorLocation() - Ship->SystemOrigin).GetSafeNormal();
			if (!Dir.IsNearlyZero()) { StarLight->SetActorRotation(Dir.Rotation()); }
		}
	}
	const float Now = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f;
	while (MessageTimes.Num() > 0 && (Now - MessageTimes[0] > 8.f || MessageTimes.Num() > 6))
	{
		MessageTimes.RemoveAt(0);
		if (Messages.Num() > 0) Messages.RemoveAt(0);
	}
}

void AHDPlayerController::Cmd(const FString& Verb, const FString& Arg, AActor* Target)
{
	ServerCommand(Verb, Arg, Target);
}

void AHDPlayerController::ServerCommand_Implementation(const FString& Verb, const FString& Arg, AActor* Target)
{
	if (Verb.Len() > 32 || Arg.Len() > 256) return;
	HandleCommand(Verb.ToLower(), Arg, Target);
}

void AHDPlayerController::ClientNotify_Implementation(const FString& Msg)
{
	Messages.Add(Msg);
	MessageTimes.Add(GetWorld() ? GetWorld()->GetTimeSeconds() : 0.f);
	UE_LOG(LogTemp, Display, TEXT("[Nullpoint] %s"), *Msg);
}

void AHDPlayerController::Notify(const FString& Msg)
{
	ClientNotify(Msg); // on a listen-server host this runs locally
}

void AHDPlayerController::HDGoto(const FString& System) { Cmd(TEXT("goto"), System); }
void AHDPlayerController::HDSystemInfo() { Cmd(TEXT("systeminfo")); }
void AHDPlayerController::HDGalaxy(const FString& RadiusLy) { Cmd(TEXT("galaxy"), RadiusLy); }
void AHDPlayerController::HDFind(const FString& Name) { Cmd(TEXT("find"), Name); }
void AHDPlayerController::HDWeek(const FString& Week) { Cmd(TEXT("week"), Week); }
void AHDPlayerController::HDRevealAll() { Cmd(TEXT("revealall")); }
void AHDPlayerController::HDGas() { Cmd(TEXT("gas")); }
void AHDPlayerController::HDCmd(const FString& Verb, const FString& Arg) { Cmd(Verb, Arg); }
void AHDPlayerController::HDMap() { CycleMap(); }

void AHDPlayerController::HDRoute(const FString& System)
{
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	const int32 Dest = U ? U->FindSystem(System) : -1;
	if (Dest < 0) { ClientNotify(TEXT("Unknown system")); return; }
	PlotRoute(Dest);
}

// ---------------------------------------------------------------------------------------------
// Maps and targeting (local only)
// ---------------------------------------------------------------------------------------------
void AHDPlayerController::CycleMap()
{
	MapMode = MapMode == EHDMapMode::Closed ? EHDMapMode::Galaxy : MapMode == EHDMapMode::Galaxy ? EHDMapMode::System : EHDMapMode::Closed;
	MapCursor = 0;
	RefreshMapEntries();
}

void AHDPlayerController::RefreshMapEntries()
{
	GalaxyEntries.Reset();
	SystemEntries.Reset();
	const AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	if (!Ship || !U || !U->IsValidSystem(Ship->SystemIndex)) return;
	if (MapMode == EHDMapMode::Galaxy)
	{
		GalaxyEntries = U->GetSystemsNear(Ship->SystemIndex, MapRadiusLy, 48);
		// Start the cursor on the current destination if it's in view.
		for (int32 I = 0; I < GalaxyEntries.Num(); ++I) { if (GalaxyEntries[I].System == RouteDestination) { MapCursor = I; } }
	}
	else if (MapMode == EHDMapMode::System)
	{
		const AHDPlayerState* PS = GetPlayerState<AHDPlayerState>();
		TArray<TPair<double, AHDSpaceObject*>> Sorted;
		for (TActorIterator<AHDSpaceObject> It(GetWorld()); It; ++It)
		{
			AHDSpaceObject* O = *It;
			if (O->SystemIndex != Ship->SystemIndex || Cast<AHDAsteroidActor>(O)) continue;
			if (O->bRequiresScan && !(PS && PS->HasRevealed(O->ObjectId))) continue;
			Sorted.Add(TPair<double, AHDSpaceObject*>(FVector::DistSquared(O->GetActorLocation(), Ship->GetActorLocation()), O));
		}
		Sorted.Sort([](const TPair<double, AHDSpaceObject*>& A, const TPair<double, AHDSpaceObject*>& B) { return A.Key < B.Key; });
		for (const TPair<double, AHDSpaceObject*>& P : Sorted) { SystemEntries.Add(P.Value); }
	}
	const int32 N = MapMode == EHDMapMode::Galaxy ? GalaxyEntries.Num() : SystemEntries.Num();
	MapCursor = N > 0 ? FMath::Clamp(MapCursor, 0, N - 1) : 0;
}

void AHDPlayerController::MapStep(int32 Dir)
{
	const int32 N = MapMode == EHDMapMode::Galaxy ? GalaxyEntries.Num() : SystemEntries.Num();
	if (N <= 0) return;
	MapCursor = (MapCursor + Dir + N) % N;
}

void AHDPlayerController::MapZoom(int32 Dir)
{
	if (MapMode != EHDMapMode::Galaxy) return;
	MapRadiusLy = FMath::Clamp(MapRadiusLy * (Dir > 0 ? 0.5f : 2.f), 5.f, 160.f);
	RefreshMapEntries();
}

void AHDPlayerController::MapSelect()
{
	if (MapMode == EHDMapMode::Galaxy && GalaxyEntries.IsValidIndex(MapCursor))
	{
		PlotRoute(GalaxyEntries[MapCursor].System);
	}
	else if (MapMode == EHDMapMode::System && SystemEntries.IsValidIndex(MapCursor))
	{
		if (AHDSpaceObject* O = SystemEntries[MapCursor].Get())
		{
			SelectedTarget = O;
			ClientNotify(FString::Printf(TEXT("Target: %s (%s)"), *O->DisplayName, *O->GetKindLabel()));
		}
	}
}

void AHDPlayerController::PlotRoute(int32 Dest)
{
	const AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	if (!Ship || !U || !U->IsValidSystem(Dest)) return;
	RouteDestination = Dest;
	const bool bCapital = HDShips::IsCapitalSize(Ship->HullClass);
	const float Range = Ship->Nav ? Ship->Nav->GetJumpRangeLy() : 0.f;
	Route = bCapital ? U->FindJumpRoute(Ship->SystemIndex, Dest, Range) : U->FindGateRoute(Ship->SystemIndex, Dest, false);
	const FHDStarSystem& D = U->GetSystemSkeleton(Dest);
	if (Route.Num() < 2)
	{
		ClientNotify(FString::Printf(TEXT("No %s route to %s"), bCapital ? TEXT("jump-drive") : TEXT("gate"), *D.Name));
	}
	else
	{
		ClientNotify(FString::Printf(TEXT("Route to %s: %d %s, %.1f ly direct"), *D.Name, Route.Num() - 1, bCapital ? TEXT("jumps") : TEXT("gates"), U->DistanceLy(Ship->SystemIndex, Dest)));
	}
	// Capitals: aim the drive at the next hop. Gate ships: target the gate that leads to the next hop.
	if (Route.Num() >= 2)
	{
		if (bCapital) { Cmd(TEXT("jumptarget"), FString::FromInt(Route[1])); }
		for (TActorIterator<AHDJumpPointActor> It(GetWorld()); It; ++It)
		{
			if (It->SystemIndex == Ship->SystemIndex && It->Link.To == Route[1]) { SelectedTarget = *It; break; }
		}
	}
}

void AHDPlayerController::CycleTarget()
{
	const AHDShipPawn* Ship = Cast<AHDShipPawn>(GetPawn());
	if (!Ship || !GetWorld()) return;
	const AHDPlayerState* PS = GetPlayerState<AHDPlayerState>();
	TArray<TPair<double, AActor*>> Cands;
	for (TActorIterator<AHDSpaceObject> It(GetWorld()); It; ++It)
	{
		AHDSpaceObject* O = *It;
		if (O->SystemIndex != Ship->SystemIndex || Cast<AHDAsteroidActor>(O)) continue;
		if (O->bRequiresScan && !(PS && PS->HasRevealed(O->ObjectId))) continue;
		Cands.Add(TPair<double, AActor*>(FVector::DistSquared(O->GetActorLocation(), Ship->GetActorLocation()), O));
	}
	for (TActorIterator<APawn> It(GetWorld()); It; ++It)
	{
		if (*It == Ship || It->IsHidden()) continue; // cloaked / unrevealed shadow ships
		const double D2 = FVector::DistSquared(It->GetActorLocation(), Ship->GetActorLocation());
		if (D2 < 2.0e7 * 2.0e7) { Cands.Add(TPair<double, AActor*>(D2, *It)); }
	}
	if (Cands.Num() == 0) { SelectedTarget = nullptr; return; }
	Cands.Sort([](const TPair<double, AActor*>& A, const TPair<double, AActor*>& B) { return A.Key < B.Key; });
	int32 Next = 0;
	for (int32 I = 0; I < Cands.Num(); ++I) { if (Cands[I].Value == SelectedTarget.Get()) { Next = (I + 1) % Cands.Num(); break; } }
	SelectedTarget = Cands[Next].Value;
}

void AHDPlayerController::HDStickSensitivity(float Value)
{
	StickSensitivity = FMath::Clamp(Value, 0.1f, 3.f);
}

void AHDPlayerController::HDMouseSensitivity(float Value)
{
	MouseDegreesPerCount = FMath::Clamp(Value, 0.001f, 2.f);
}

const FHDFlightInput& AHDPlayerController::GetFlightInput()
{
	if (!bFlightBuilt && IsLocalController())
	{
		BuildFlightInput();
		if (UEnhancedInputLocalPlayerSubsystem* Sub = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
		{
			Sub->AddMappingContext(Flight.Context, 0);
			if (FlightMapping) { Sub->AddMappingContext(FlightMapping, 1); }
			bFlightBuilt = true;
		}
	}
	return Flight;
}

void AHDPlayerController::BuildFlightInput()
{
	if (Flight.Context) return;
	using T = EInputActionValueType;
	UInputMappingContext* C = NewObject<UInputMappingContext>(this, TEXT("IMC_Flight_Runtime"));
	Flight.Context = C;

	Flight.Thrust    = MakeAction(this, TEXT("IA_Thrust"), T::Axis1D);
	Flight.Strafe    = MakeAction(this, TEXT("IA_Strafe"), T::Axis1D);
	Flight.Vertical  = MakeAction(this, TEXT("IA_Vertical"), T::Axis1D);
	Flight.Pitch     = MakeAction(this, TEXT("IA_Pitch"), T::Axis1D);
	Flight.Yaw       = MakeAction(this, TEXT("IA_Yaw"), T::Axis1D);
	Flight.Roll      = MakeAction(this, TEXT("IA_Roll"), T::Axis1D);
	Flight.LookStick = MakeAction(this, TEXT("IA_LookStick"), T::Axis2D);
	Flight.MouseAim  = MakeAction(this, TEXT("IA_MouseAim"), T::Axis2D);
	Flight.Fire          = MakeAction(this, TEXT("IA_Fire"), T::Boolean);
	Flight.ToggleFA      = MakeAction(this, TEXT("IA_ToggleFA"), T::Boolean);
	Flight.Slipstream    = MakeAction(this, TEXT("IA_Slipstream"), T::Boolean);
	Flight.PowerBalanced = MakeAction(this, TEXT("IA_PowerBalanced"), T::Boolean);
	Flight.PowerAttack   = MakeAction(this, TEXT("IA_PowerAttack"), T::Boolean);
	Flight.PowerTank     = MakeAction(this, TEXT("IA_PowerTank"), T::Boolean);
	Flight.PowerRun      = MakeAction(this, TEXT("IA_PowerRun"), T::Boolean);
	Flight.EqualizeShields = MakeAction(this, TEXT("IA_EqualizeShields"), T::Boolean);
	Flight.RecenterVR    = MakeAction(this, TEXT("IA_RecenterVR"), T::Boolean);
	Flight.Menu          = MakeAction(this, TEXT("IA_Menu"), T::Boolean);
	Flight.Cruise        = MakeAction(this, TEXT("IA_Cruise"), T::Boolean);
	Flight.Interact      = MakeAction(this, TEXT("IA_Interact"), T::Boolean);
	Flight.Dock          = MakeAction(this, TEXT("IA_Dock"), T::Boolean);
	Flight.Land          = MakeAction(this, TEXT("IA_Land"), T::Boolean);
	Flight.Jump          = MakeAction(this, TEXT("IA_Jump"), T::Boolean);
	Flight.Map           = MakeAction(this, TEXT("IA_Map"), T::Boolean);
	Flight.Scan          = MakeAction(this, TEXT("IA_Scan"), T::Boolean);
	Flight.TargetNext    = MakeAction(this, TEXT("IA_TargetNext"), T::Boolean);
	Flight.MapNext       = MakeAction(this, TEXT("IA_MapNext"), T::Boolean);
	Flight.MapPrev       = MakeAction(this, TEXT("IA_MapPrev"), T::Boolean);
	Flight.MapSelect     = MakeAction(this, TEXT("IA_MapSelect"), T::Boolean);
	Flight.PadYTap       = MakeAction(this, TEXT("IA_PadYTap"), T::Boolean);
	Flight.PadYHold      = MakeAction(this, TEXT("IA_PadYHold"), T::Boolean);
	Flight.PadBTap       = MakeAction(this, TEXT("IA_PadBTap"), T::Boolean);
	Flight.PadBHold      = MakeAction(this, TEXT("IA_PadBHold"), T::Boolean);
	Flight.PadXTap       = MakeAction(this, TEXT("IA_PadXTap"), T::Boolean);
	Flight.PadXHold      = MakeAction(this, TEXT("IA_PadXHold"), T::Boolean);
	Flight.PadViewTap    = MakeAction(this, TEXT("IA_PadViewTap"), T::Boolean);
	Flight.PadViewHold   = MakeAction(this, TEXT("IA_PadViewHold"), T::Boolean);

	// Keyboard + mouse
	MapKeyTo(C, Flight.Thrust, EKeys::W);            MapKeyTo(C, Flight.Thrust, EKeys::S, -1.f);
	MapKeyTo(C, Flight.Strafe, EKeys::D);            MapKeyTo(C, Flight.Strafe, EKeys::A, -1.f);
	MapKeyTo(C, Flight.Vertical, EKeys::R);          MapKeyTo(C, Flight.Vertical, EKeys::F, -1.f);
	MapKeyTo(C, Flight.Roll, EKeys::E);              MapKeyTo(C, Flight.Roll, EKeys::Q, -1.f);
	MapKeyTo(C, Flight.MouseAim, EKeys::Mouse2D);    // raw counts; converted to rotation in the movement component
	MapKeyTo(C, Flight.Pitch, EKeys::Up);            MapKeyTo(C, Flight.Pitch, EKeys::Down, -1.f);
	MapKeyTo(C, Flight.Yaw, EKeys::Right);           MapKeyTo(C, Flight.Yaw, EKeys::Left, -1.f);
	MapKeyTo(C, Flight.Fire, EKeys::LeftMouseButton); MapKeyTo(C, Flight.Fire, EKeys::SpaceBar);
	MapKeyTo(C, Flight.ToggleFA, EKeys::Z);
	MapKeyTo(C, Flight.Slipstream, EKeys::J);
	MapKeyTo(C, Flight.PowerBalanced, EKeys::One);
	MapKeyTo(C, Flight.PowerAttack, EKeys::Two);
	MapKeyTo(C, Flight.PowerTank, EKeys::Three);
	MapKeyTo(C, Flight.PowerRun, EKeys::Four);
	MapKeyTo(C, Flight.EqualizeShields, EKeys::X);
	MapKeyTo(C, Flight.RecenterVR, EKeys::Home);
	MapKeyTo(C, Flight.Menu, EKeys::Tab);
	MapKeyTo(C, Flight.Cruise, EKeys::C);
	MapKeyTo(C, Flight.Interact, EKeys::G);
	MapKeyTo(C, Flight.Dock, EKeys::H);
	MapKeyTo(C, Flight.Land, EKeys::L);
	MapKeyTo(C, Flight.Jump, EKeys::B);
	MapKeyTo(C, Flight.Map, EKeys::M);
	MapKeyTo(C, Flight.Scan, EKeys::K);
	MapKeyTo(C, Flight.TargetNext, EKeys::T);
	MapKeyTo(C, Flight.MapNext, EKeys::PageDown);
	MapKeyTo(C, Flight.MapPrev, EKeys::PageUp);
	MapKeyTo(C, Flight.MapSelect, EKeys::Enter);

	// Xbox controller (XInput). Multiple keys on one Axis1D action resolve by highest
	// absolute value, so left stick Y and the triggers never fight: the larger input wins.
	const float DZ = FMath::Clamp(StickDeadZone, 0.f, 0.5f);
	const float Exp = FMath::Clamp(StickCurveExponent, 1.f, 4.f);
	MapAnalog(C, Flight.Thrust, EKeys::Gamepad_LeftY, DZ, Exp, EDeadZoneType::Axial);
	MapAnalog(C, Flight.Strafe, EKeys::Gamepad_LeftX, DZ, Exp, EDeadZoneType::Axial);
	MapAnalog(C, Flight.Thrust, EKeys::Gamepad_RightTriggerAxis, DZ * 0.5f, 1.f, EDeadZoneType::Axial);
	MapAnalog(C, Flight.Thrust, EKeys::Gamepad_LeftTriggerAxis, DZ * 0.5f, 1.f, EDeadZoneType::Axial, FVector(-1.f, -1.f, -1.f));
	// Right stick as one 2D input with a radial deadzone (no square-gate diagonals).
	MapAnalog(C, Flight.LookStick, EKeys::Gamepad_Right2D, DZ, Exp, EDeadZoneType::Radial);
	MapKeyTo(C, Flight.Roll, EKeys::Gamepad_RightShoulder); MapKeyTo(C, Flight.Roll, EKeys::Gamepad_LeftShoulder, -1.f);
	MapKeyTo(C, Flight.Vertical, EKeys::Gamepad_LeftThumbstick);           // L3 hold = up
	MapKeyTo(C, Flight.Vertical, EKeys::Gamepad_RightThumbstick, -1.f);    // R3 hold = down
	MapKeyTo(C, Flight.Fire, EKeys::Gamepad_FaceButton_Bottom);            // A
	// Face buttons: tap keeps the M1 action, hold adds the travel action.
	MapTap(C, Flight.PadBTap, EKeys::Gamepad_FaceButton_Right);           // B tap: flight assist
	MapHold(C, Flight.PadBHold, EKeys::Gamepad_FaceButton_Right);         // B hold: interact (dock / land / jump / lane / undock)
	MapTap(C, Flight.PadXTap, EKeys::Gamepad_FaceButton_Left);            // X tap: equalize shields
	MapHold(C, Flight.PadXHold, EKeys::Gamepad_FaceButton_Left);          // X hold: scan pulse
	MapTap(C, Flight.PadYTap, EKeys::Gamepad_FaceButton_Top);             // Y tap: slipstream
	MapHold(C, Flight.PadYHold, EKeys::Gamepad_FaceButton_Top);           // Y hold: cruise
	MapKeyTo(C, Flight.PowerBalanced, EKeys::Gamepad_DPad_Up);
	MapKeyTo(C, Flight.PowerAttack, EKeys::Gamepad_DPad_Right);
	MapKeyTo(C, Flight.PowerTank, EKeys::Gamepad_DPad_Down);
	MapKeyTo(C, Flight.PowerRun, EKeys::Gamepad_DPad_Left);
	MapTap(C, Flight.PadViewTap, EKeys::Gamepad_Special_Left);            // View tap: recenter VR (VR) / next target (flat)
	MapHold(C, Flight.PadViewHold, EKeys::Gamepad_Special_Left);          // View hold: map (galaxy -> system -> closed)
	MapKeyTo(C, Flight.Menu, EKeys::Gamepad_Special_Right);                // Menu
}
