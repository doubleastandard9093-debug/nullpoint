#include "HDHUD.h"
#include "HDShipPawn.h"
#include "HDShipTypes.h"
#include "HDShipMovementComponent.h"
#include "HDShipPowerComponent.h"
#include "HDNavComponent.h"
#include "HDFittingComponent.h"
#include "HDCargoComponent.h"
#include "HDPlayerController.h"
#include "HDPlayerState.h"
#include "HDWorldActors.h"
#include "Engine/Canvas.h"
#include "HDUniverseSubsystem.h"

namespace
{
	const TCHAR* ModeName(EHDTravelMode M)
	{
		switch (M)
		{
		case EHDTravelMode::Spooling: return TEXT("SPOOLING");
		case EHDTravelMode::Slipstream: return TEXT("SLIPSTREAM");
		case EHDTravelMode::Cruise: return TEXT("CRUISE");
		case EHDTravelMode::Glide: return TEXT("GLIDE");
		case EHDTravelMode::TradeLane: return TEXT("TRADE LANE");
		case EHDTravelMode::Docked: return TEXT("DOCKED");
		case EHDTravelMode::Landed: return TEXT("LANDED");
		case EHDTravelMode::Jumping: return TEXT("JUMPING");
		default: return TEXT("NORMAL");
		}
	}
	const FLinearColor Cyan(0.4f, 0.9f, 1.f);
	const FLinearColor Amber(1.f, 0.85f, 0.4f);
	const FLinearColor Warn(1.f, 0.4f, 0.3f);
}

FString AHDHUD::FmtDist(double Cm)
{
	const double Km = Cm / 1.0e5;
	if (Km < 1.0) return FString::Printf(TEXT("%.0f m"), Cm / 100.0);
	if (Km < 1.0e5) return FString::Printf(TEXT("%.1f km"), Km);
	return FString::Printf(TEXT("%.2f AU"), Cm / 1.0e10);
}

FString AHDHUD::FmtSpeed(double CmPerSec)
{
	const double Ms = CmPerSec / 100.0;
	if (Ms < 10000.0) return FString::Printf(TEXT("%.0f m/s"), Ms);
	if (Ms < 2.0e7) return FString::Printf(TEXT("%.0f km/s"), Ms / 1000.0);
	return FString::Printf(TEXT("%.2f c"), Ms / 2.998e8);
}

void AHDHUD::DrawHUD()
{
	Super::DrawHUD();
	const AHDShipPawn* Ship = Cast<AHDShipPawn>(GetOwningPawn());
	if (!Ship || !Canvas || !Ship->Movement || !Ship->Power || !Ship->Shields) return;
	AHDPlayerController* PC = Cast<AHDPlayerController>(GetOwningPlayerController());
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);

	const UHDShipMovementComponent* M = Ship->Movement;
	const UHDShipPowerComponent* P = Ship->Power;
	const UHDShieldComponent* S = Ship->Shields;
	const UHDNavComponent* N = Ship->Nav;
	const EHDTravelMode Mode = M->GetTravelMode();

	TArray<FString> Lines;
	TArray<FLinearColor> Colors;
	auto Add = [&Lines, &Colors](const FString& L, const FLinearColor& C) { Lines.Add(L); Colors.Add(C); };

	if (U && U->IsValidSystem(Ship->SystemIndex))
	{
		const FHDStarSystem& Sys = U->GetSystemSkeleton(Ship->SystemIndex);
		Add(FString::Printf(TEXT("%s  %s  [%s]"), *Sys.Id.ToString(), *Sys.Name, HDUniverse::BandName(Sys.Band)), HDUniverse::BandColor(Sys.Band));
	}
	const double Speed = M->IsInSubSpace() ? M->GetShipVelocity().Size() : FMath::Max((double)M->GetTravelSpeed(), M->GetShipVelocity().Size());
	Add(FString::Printf(TEXT("SPEED %s  (cap %s)"), *FmtSpeed(Speed), *FmtSpeed(Mode == EHDTravelMode::Cruise ? M->GetCruiseLimitAt(Ship->GetActorLocation()) : M->GetSpeedCap())), Cyan);
	Add(FString::Printf(TEXT("FLIGHT ASSIST %s   TRAVEL %s   HULL CLASS %s"), M->bFlightAssist ? TEXT("ON") : TEXT("OFF"), ModeName(Mode), HDShips::ClassName(Ship->HullClass)), Cyan);
	Add(FString::Printf(TEXT("POWER  W %.0f  S %.0f  E %.0f  A %.0f"),
		P->GetPower(EHDSubsystem::Weapons), P->GetPower(EHDSubsystem::Shields), P->GetPower(EHDSubsystem::Engines), P->GetPower(EHDSubsystem::Auxiliary)), Cyan);
	Add(FString::Printf(TEXT("SHIELDS  F %.0f  A %.0f  P %.0f  S %.0f"),
		S->GetFacing(EHDShieldFacing::Fore), S->GetFacing(EHDShieldFacing::Aft), S->GetFacing(EHDShieldFacing::Port), S->GetFacing(EHDShieldFacing::Starboard)), Cyan);
	Add(FString::Printf(TEXT("HULL %.0f / %.0f"), Ship->HullHP, Ship->MaxHullHP), Ship->HullHP < Ship->MaxHullHP * 0.3f ? Warn : Cyan);
	if (const UHDFittingComponent* F = Ship->Fitting)
	{
		if (const FHDHullDef* H = F->GetHullDef())
		{
			Add(FString::Printf(TEXT("SHIP %s  power %.0f/%.0f MW  stability %.0f/%.0f%s"), *H->Name, F->Stats.PowerUsed, F->Stats.PowerMax, F->Stats.Stability, F->Stats.StabilityMax,
				F->bRecovered ? TEXT("  [recovered]") : TEXT("")), Cyan);
		}
	}
	if (const UHDCargoComponent* C = Ship->Cargo)
	{
		float G = 0.f, O = 0.f;
		C->GetUsed(G, O);
		Add(FString::Printf(TEXT("CARGO %.0f/%.0f m3%s"), G, C->Capacity, C->OreCapacity > 0.f ? *FString::Printf(TEXT("  ore %.0f/%.0f m3"), O, C->OreCapacity) : TEXT("")), Cyan);
	}
	if (const AHDPlayerState* PS = PC ? PC->GetPlayerState<AHDPlayerState>() : nullptr)
	{
		Add(FString::Printf(TEXT("CREDITS %lld%s"), PS->Credits, PS->IsCriminal() ? TEXT("   [CRIMINAL]") : TEXT("")), PS->IsCriminal() ? Warn : Cyan);
	}
	if (N)
	{
		Add(FString::Printf(TEXT("FUEL %.1f / %.0f t"), N->ShipFuel, N->MaxShipFuel), N->ShipFuel < N->MaxShipFuel * 0.15f ? Warn : Cyan);
		if (HDShips::IsCapitalSize(Ship->HullClass))
		{
			Add(FString::Printf(TEXT("JUMP DRIVE  range %.2f ly  fuel %.0f / %.0f t  strain %.0f min  spool-down %.1f min"),
				N->GetJumpRangeLy(), N->JumpFuel, N->MaxJumpFuel, N->GetStrainMinutes(), N->GetCooldownMinutes()), Cyan);
			if (U && U->IsValidSystem(N->JumpTarget))
			{
				const float Ly = U->DistanceLy(Ship->SystemIndex, N->JumpTarget);
				Add(FString::Printf(TEXT("JUMP TARGET %s  %.2f ly  needs %.0f t%s"), *U->GetSystemSkeleton(N->JumpTarget).Name, Ly, N->GetJumpFuelFor(Ly),
					Ly > N->GetJumpRangeLy() ? TEXT("  OUT OF RANGE") : TEXT("")), Ly > N->GetJumpRangeLy() ? Warn : Amber);
			}
		}
		if (N->Pending != EHDPendingJump::None) Add(FString::Printf(TEXT("JUMP IN %.1f s"), N->JumpCountdown), Amber);
		if (N->ScanCountdown > 0.f) Add(FString::Printf(TEXT("SCANNING %.1f s"), N->ScanCountdown), Amber);
		if (const AHDSpaceObject* D = Cast<AHDSpaceObject>(N->DockedAt)) Add(FString::Printf(TEXT("DOCKED AT %s  (G / hold B to undock)"), *D->DisplayName), Amber);
		if (const AHDSpaceObject* L = Cast<AHDSpaceObject>(N->LandedOn)) Add(FString::Printf(TEXT("LANDED ON %s  (L / hold B to lift off)"), *L->DisplayName), Amber);

		// Planetary approach readout (doc 17): altitude, pitch, gravity, corridor.
		double Alt = 0.0;
		if (const AHDCelestialBody* B = N->FindNearestBody(Alt))
		{
			if (!B->IsStar() && Alt < 5.0e7)
			{
				Add(FString::Printf(TEXT("%s  %s  alt %s  pitch %.0f deg  %.2f g%s"), *B->DisplayName, HDUniverse::BodyTypeName(B->Body.Type), *FmtDist(Alt),
					M->GetPitchToHorizon(Alt), B->Body.Gravity, B->Body.bLandable ? TEXT("  landable") : TEXT("")), Cyan);
				const FHDCorridorStatus C = UHDNavComponent::CheckCorridor(B, Ship->GetActorLocation(), AHDPlayerState::NowUnix());
				if (C.bRestricted)
				{
					Add(FString::Printf(TEXT("LANDING CORRIDOR %d  %s  (%.0f deg off)%s"), C.ActiveCorridor + 1, C.bOpen ? TEXT("ALIGNED") : TEXT("NOT ALIGNED"), C.DegreesOff,
						C.SecondsLeft > 0.f ? *FString::Printf(TEXT("  rotates in %.0f s"), C.SecondsLeft) : TEXT("")), C.bOpen ? Cyan : Warn);
				}
			}
		}
	}
	if (PC && U && PC->Route.Num() >= 2 && U->IsValidSystem(PC->RouteDestination))
	{
		const int32 Here = PC->Route.Find(Ship->SystemIndex);
		const int32 Next = Here != INDEX_NONE && PC->Route.IsValidIndex(Here + 1) ? PC->Route[Here + 1] : -1;
		if (Ship->SystemIndex == PC->RouteDestination) Add(TEXT("ROUTE: arrived"), Amber);
		else Add(FString::Printf(TEXT("ROUTE to %s: %d hops left, next %s"), *U->GetSystemSkeleton(PC->RouteDestination).Name,
			Here != INDEX_NONE ? PC->Route.Num() - 1 - Here : -1, Next >= 0 ? *U->GetSystemSkeleton(Next).Name : TEXT("(off route: replot)")), Amber);
	}

	float Y = 40.f;
	for (int32 I = 0; I < Lines.Num(); ++I)
	{
		DrawText(Lines[I], Colors[I], 40.f, Y);
		Y += 20.f;
	}

	// Context prompt under the crosshair.
	const float CX = Canvas->ClipX * 0.5f, CY = Canvas->ClipY * 0.5f;
	if (N)
	{
		FString Prompt;
		N->FindInteractable(Prompt);
		if (!Prompt.IsEmpty()) DrawText(FString::Printf(TEXT("[G / hold B]  %s"), *Prompt), Amber, CX - 120.f, CY + 60.f);
	}

	if (PC)
	{
		DrawTarget(Ship, PC);
		if (PC->GetMapMode() == EHDMapMode::Galaxy && U) DrawGalaxyMap(Ship, PC, U);
		else if (PC->GetMapMode() == EHDMapMode::System && U) DrawSystemMap(Ship, PC, U);
	}

	if (PC && PC->IsMenuOpen())
	{
		static const TCHAR* Help[] = {
			TEXT("CONTROLS  (Tab / Menu to close)        keyboard  |  Xbox"),
			TEXT("Thrust/strafe: W S A D  | left stick, RT fwd, LT reverse"),
			TEXT("Up/down: R F  | hold L3 up, hold R3 down"),
			TEXT("Pitch/yaw: mouse, arrows  | right stick    Roll: Q E  | LB RB"),
			TEXT("Fire: LMB, Space  | A      Flight assist: Z  | tap B"),
			TEXT("Shields equalize: X  | tap X     Scan pulse: K  | hold X"),
			TEXT("Slipstream: J  | tap Y     Cruise: C  | hold Y"),
			TEXT("Interact (dock/land/jump/lane/undock): G  | hold B"),
			TEXT("Dock: H   Land / lift off: L   Jump: B"),
			TEXT("Map (galaxy -> system -> off): M  | hold View"),
			TEXT("Map cursor: PgUp PgDn / 1 3  | D-pad up/down   zoom: 2 4  | D-pad right/left   select: Enter  | A"),
			TEXT("Next target: T  | tap View (flat)"),
			TEXT("Power Bal/Atk/Tank/Run: 1 2 3 4  | D-pad up/right/down/left"),
			TEXT("Recenter VR: Home  | tap View (in VR)"),
		};
		Y += 20.f;
		for (const TCHAR* L : Help)
		{
			DrawText(FString(L), Amber, 40.f, Y);
			Y += 20.f;
		}
	}

	// Messages from the server (bottom left)
	if (PC)
	{
		float MY = Canvas->ClipY - 40.f - 20.f * PC->Messages.Num();
		for (const FString& Msg : PC->Messages) { DrawText(Msg, FLinearColor(0.9f, 0.9f, 0.6f), 40.f, MY); MY += 20.f; }
	}

	// crosshair
	DrawLine(CX - 10.f, CY, CX + 10.f, CY, FLinearColor::White);
	DrawLine(CX, CY - 10.f, CX, CY + 10.f, FLinearColor::White);
}

void AHDHUD::DrawTarget(const AHDShipPawn* Ship, AHDPlayerController* PC)
{
	AActor* T = PC->GetSelectedTarget();
	if (!T) return;
	const double Dist = FVector::Dist(T->GetActorLocation(), Ship->GetActorLocation());
	FString Name = T->GetName(), Kind = TEXT("Ship");
	if (const AHDSpaceObject* O = Cast<AHDSpaceObject>(T)) { Name = O->DisplayName; Kind = O->GetKindLabel(); }
	const float X = Canvas->ClipX - 420.f;
	DrawText(FString::Printf(TEXT("TARGET %s"), *Name), Amber, X, 40.f);
	DrawText(FString::Printf(TEXT("%s  %s"), *Kind, *FmtDist(Dist)), Amber, X, 60.f);
	// On-screen bracket (only when in front of us).
	const FVector Dir = (T->GetActorLocation() - Ship->GetActorLocation()).GetSafeNormal();
	if (FVector::DotProduct(Dir, Ship->GetActorForwardVector()) > 0.05)
	{
		const FVector Scr = Project(Ship->GetActorLocation() + Dir * 1.0e5, false);
		const float SX = (float)Scr.X, SY = (float)Scr.Y, R = 14.f;
		DrawLine(SX - R, SY - R, SX + R, SY - R, Amber); DrawLine(SX - R, SY + R, SX + R, SY + R, Amber);
		DrawLine(SX - R, SY - R, SX - R, SY + R, Amber); DrawLine(SX + R, SY - R, SX + R, SY + R, Amber);
	}
}

void AHDHUD::DrawGalaxyMap(const AHDShipPawn* Ship, AHDPlayerController* PC, UHDUniverseSubsystem* U)
{
	if (!U->IsValidSystem(Ship->SystemIndex)) return;
	const float Sz = FMath::Min(Canvas->ClipY * 0.75f, 760.f);
	const float CX = Canvas->ClipX * 0.5f, CY = Canvas->ClipY * 0.5f;
	DrawRect(FLinearColor(0.f, 0.02f, 0.05f, 0.75f), CX - Sz * 0.5f, CY - Sz * 0.5f, Sz, Sz);
	const FVector Center = U->GetSystemSkeleton(Ship->SystemIndex).PositionLy;
	const float Scale = Sz * 0.5f / FMath::Max(PC->MapRadiusLy, 1.f);
	auto ToScreen = [&](const FVector& Ly) { return FVector2D(CX + (Ly.X - Center.X) * Scale, CY - (Ly.Y - Center.Y) * Scale); };
	auto Inside = [&](const FVector2D& P) { return FMath::Abs(P.X - CX) < Sz * 0.5f && FMath::Abs(P.Y - CY) < Sz * 0.5f; };

	TSet<int32> InView;
	for (const FHDMapEntry& E : PC->GalaxyEntries) InView.Add(E.System);
	// Links (visible ones only; hidden wormholes need scanning first).
	for (const FHDMapEntry& E : PC->GalaxyEntries)
	{
		const TArray<FHDJumpLink> Links = U->GetLinks(E.System);
		for (const FHDJumpLink& L : Links)
		{
			if (L.bHidden || !InView.Contains(L.To) || L.To < E.System) continue;
			const FVector2D A = ToScreen(E.PositionLy), B = ToScreen(U->GetSystemSkeleton(L.To).PositionLy);
			const FLinearColor C = L.Type == EHDLinkType::LargeGate ? FLinearColor(0.9f, 0.9f, 0.9f, 0.8f)
				: L.Type == EHDLinkType::Gate ? FLinearColor(0.4f, 0.45f, 0.5f, 0.6f) : FLinearColor(0.8f, 0.4f, 1.f, 0.8f);
			if (Inside(A) && Inside(B)) DrawLine((float)A.X, (float)A.Y, (float)B.X, (float)B.Y, C);
		}
	}
	// Route
	for (int32 I = 0; I + 1 < PC->Route.Num(); ++I)
	{
		const FVector2D A = ToScreen(U->GetSystemSkeleton(PC->Route[I]).PositionLy), B = ToScreen(U->GetSystemSkeleton(PC->Route[I + 1]).PositionLy);
		DrawLine((float)A.X, (float)A.Y, (float)B.X, (float)B.Y, FLinearColor(1.f, 0.9f, 0.2f), 2.f);
	}
	// Capital jump range ring.
	if (HDShips::IsCapitalSize(Ship->HullClass) && Ship->Nav)
	{
		const float R = Ship->Nav->GetJumpRangeLy() * Scale;
		const int32 Seg = 48;
		for (int32 I = 0; I < Seg; ++I)
		{
			const float A0 = 2.f * PI * I / Seg, A1 = 2.f * PI * (I + 1) / Seg;
			DrawLine(CX + R * FMath::Cos(A0), CY + R * FMath::Sin(A0), CX + R * FMath::Cos(A1), CY + R * FMath::Sin(A1), FLinearColor(0.3f, 1.f, 0.5f, 0.6f));
		}
	}
	// Systems
	for (int32 I = 0; I < PC->GalaxyEntries.Num(); ++I)
	{
		const FHDMapEntry& E = PC->GalaxyEntries[I];
		const FVector2D Pt = ToScreen(E.PositionLy);
		if (!Inside(Pt)) continue;
		const float D = E.bSettled ? 5.f : 3.f;
		DrawRect(HDUniverse::BandColor(E.Band), (float)Pt.X - D * 0.5f, (float)Pt.Y - D * 0.5f, D, D);
		if (I == PC->MapCursor)
		{
			const float R = 9.f;
			DrawLine((float)Pt.X - R, (float)Pt.Y - R, (float)Pt.X + R, (float)Pt.Y - R, Amber); DrawLine((float)Pt.X - R, (float)Pt.Y + R, (float)Pt.X + R, (float)Pt.Y + R, Amber);
			DrawLine((float)Pt.X - R, (float)Pt.Y - R, (float)Pt.X - R, (float)Pt.Y + R, Amber); DrawLine((float)Pt.X + R, (float)Pt.Y - R, (float)Pt.X + R, (float)Pt.Y + R, Amber);
		}
		if (E.System == Ship->SystemIndex) DrawText(TEXT("YOU"), Cyan, (float)Pt.X + 6.f, (float)Pt.Y - 6.f, nullptr, 0.8f);
	}
	DrawText(FString::Printf(TEXT("GALAXY MAP  radius %.0f ly   (cursor PgUp/PgDn or D-pad, zoom 2/4 or D-pad L/R, Enter / A = plot route)"), PC->MapRadiusLy), Amber, CX - Sz * 0.5f, CY - Sz * 0.5f - 24.f);
	if (PC->GalaxyEntries.IsValidIndex(PC->MapCursor))
	{
		const FHDMapEntry& E = PC->GalaxyEntries[PC->MapCursor];
		DrawText(FString::Printf(TEXT("%s  [%s]  %.1f ly  %s"), *E.Name, HDUniverse::BandName(E.Band), E.DistanceLy, E.bSettled ? TEXT("settled") : TEXT("unsettled")),
			HDUniverse::BandColor(E.Band), CX - Sz * 0.5f, CY + Sz * 0.5f + 6.f);
	}
}

void AHDHUD::DrawSystemMap(const AHDShipPawn* Ship, AHDPlayerController* PC, UHDUniverseSubsystem* U)
{
	const float Sz = FMath::Min(Canvas->ClipY * 0.75f, 760.f);
	const float CX = Canvas->ClipX * 0.5f, CY = Canvas->ClipY * 0.5f;
	DrawRect(FLinearColor(0.f, 0.02f, 0.05f, 0.75f), CX - Sz * 0.5f, CY - Sz * 0.5f, Sz, Sz);
	double MaxR = 1.0e10;
	for (const TWeakObjectPtr<AHDSpaceObject>& W : PC->SystemEntries)
	{
		if (const AHDSpaceObject* O = W.Get()) MaxR = FMath::Max(MaxR, (O->GetActorLocation() - Ship->SystemOrigin).Size2D());
	}
	const double Scale = Sz * 0.48 / MaxR;
	auto ToScreen = [&](const FVector& World) { const FVector L = World - Ship->SystemOrigin; return FVector2D(CX + L.X * Scale, CY - L.Y * Scale); };
	DrawRect(FLinearColor(1.f, 0.9f, 0.5f), CX - 4.f, CY - 4.f, 8.f, 8.f);
	const FVector2D Me = ToScreen(Ship->GetActorLocation());
	DrawRect(Cyan, (float)Me.X - 3.f, (float)Me.Y - 3.f, 6.f, 6.f);
	float LY = CY - Sz * 0.5f;
	const float LX = CX + Sz * 0.5f + 12.f;
	const int32 First = FMath::Max(0, PC->MapCursor - 12);
	for (int32 I = 0; I < PC->SystemEntries.Num(); ++I)
	{
		const AHDSpaceObject* O = PC->SystemEntries[I].Get();
		if (!O) continue;
		const FVector2D Pt = ToScreen(O->GetActorLocation());
		const bool bCur = I == PC->MapCursor;
		const FLinearColor C = Cast<AHDStationActor>(O) ? FLinearColor(0.4f, 1.f, 0.4f) : Cast<AHDJumpPointActor>(O) ? FLinearColor(0.8f, 0.5f, 1.f)
			: Cast<AHDSiteActor>(O) ? Warn : FLinearColor(0.7f, 0.7f, 0.8f);
		DrawRect(bCur ? Amber : C, (float)Pt.X - 2.5f, (float)Pt.Y - 2.5f, 5.f, 5.f);
		if (I >= First && I < First + 26)
		{
			DrawText(FString::Printf(TEXT("%s%s  %s  %s"), bCur ? TEXT("> ") : TEXT("  "), *O->DisplayName, *O->GetKindLabel(), *FmtDist(FVector::Dist(O->GetActorLocation(), Ship->GetActorLocation()))),
				bCur ? Amber : C, LX, LY, nullptr, 0.9f);
			LY += 18.f;
		}
	}
	FString Title = TEXT("SYSTEM MAP");
	if (U->IsValidSystem(Ship->SystemIndex)) Title += TEXT("  ") + U->GetSystemSkeleton(Ship->SystemIndex).Name;
	DrawText(Title + TEXT("   (cursor PgUp/PgDn or D-pad, Enter / A = set target)"), Amber, CX - Sz * 0.5f, CY - Sz * 0.5f - 24.f);
}
