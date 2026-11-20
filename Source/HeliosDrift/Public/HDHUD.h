#pragma once
#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "HDHUD.generated.h"

class AHDShipPawn;
class AHDPlayerController;
class UHDUniverseSubsystem;

/** Debug flight HUD (canvas text + simple maps). Replaced by diegetic UMG panels later. */
UCLASS()
class HELIOSDRIFT_API AHDHUD : public AHUD
{
	GENERATED_BODY()
public:
	virtual void DrawHUD() override;

private:
	void DrawTarget(const AHDShipPawn* Ship, AHDPlayerController* PC);
	void DrawGalaxyMap(const AHDShipPawn* Ship, AHDPlayerController* PC, UHDUniverseSubsystem* U);
	void DrawSystemMap(const AHDShipPawn* Ship, AHDPlayerController* PC, UHDUniverseSubsystem* U);
	static FString FmtDist(double Cm);
	static FString FmtSpeed(double CmPerSec);
};
