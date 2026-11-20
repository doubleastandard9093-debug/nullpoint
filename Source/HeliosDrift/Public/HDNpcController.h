#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Controller.h"
#include "HDNpcController.generated.h"

class AHDShipPawn;
class AHDSiteActor;

/** The four antagonist populations plus lawful traffic (doc 18). */
UENUM(BlueprintType)
enum class EHDNpcPop : uint8
{
	Accord,    // system authority / security response (lawful)
	Patrol,    // faction patrol (lawful)
	Trader,    // haulers and convoys (lawful)
	Rogue,     // opportunists who flee
	Pirate,    // organized: leader + wing, breaks when the leader dies
	Sentinel,  // ancient guards: never pursue, never retreat
	Shadow     // no signature until they choose to reveal
};

UENUM(BlueprintType)
enum class EHDNpcState : uint8 { Idle, Patrol, Travel, Engage, Flee, Guard };

/**
 * Server-only NPC pilot. Possesses an AHDShipPawn and flies it through the same movement component players use
 * (rate inputs on the server, replicated to clients like any ship). Simple state brain thinks twice a second.
 */
UCLASS()
class HELIOSDRIFT_API AHDNpcController : public AController
{
	GENERATED_BODY()
public:
	AHDNpcController();
	virtual void Tick(float DeltaSeconds) override;

	EHDNpcPop Pop = EHDNpcPop::Rogue;
	EHDNpcState State = EHDNpcState::Idle;
	/** Faction key (e.g. halvar_combine, black_tally, accord). */
	FName Faction;
	/** Tier 4 claim defenders: fight whatever is hostile to this company's claim (doc 15). */
	FName DefendCorp;
	/** Where it lives: patrol centre, guard anchor or spawn point (world). */
	FVector Home = FVector::ZeroVector;
	/** Sentinels / site guards never chase beyond this from Home (cm). */
	float GuardRadius = 5.0e6f;
	float AggroRange = 6.0e6f;
	/** Rogues flee under this hull fraction; sentinels never do. */
	float FleeHullFraction = 0.4f;
	/** Travel destinations (world); traders hop stations, patrols loop. */
	TArray<FVector> Waypoints;
	int32 WaypointIndex = 0;
	TWeakObjectPtr<AActor> Target;
	/** Pirate chain of command: wingmen break when the leader dies. */
	TWeakObjectPtr<AHDNpcController> Leader;
	bool bHadLeader = false;
	/** Accord response: the criminal this ship was dispatched for; despawns after. */
	TWeakObjectPtr<AHDShipPawn> Pursuit;
	/** Ambient NPCs may be culled when far from pilots; site and response NPCs are owned elsewhere. */
	bool bAmbient = true;
	/** Shadow ships stay hidden until they reveal. */
	bool bRevealed = true;
	TWeakObjectPtr<AHDSiteActor> Site;
	/** Last actor that damaged us and when. */
	TWeakObjectPtr<AActor> LastAttacker;
	float LastAttackedTime = -100.f;

	AHDShipPawn* GetShip() const;
	bool IsLawful() const { return Pop == EHDNpcPop::Accord || Pop == EHDNpcPop::Patrol || Pop == EHDNpcPop::Trader; }
	bool IsHostileTo(const AActor* Other) const;
	void OnDamaged(AActor* By);
	void Reveal();
	static const TCHAR* PopName(EHDNpcPop P);

protected:
	virtual void OnPossess(APawn* InPawn) override;

private:
	bool bEverPossessed = false;
	void Think();
	void Steer(float Dt);
	AActor* FindTarget() const;
	void FlyToward(const FVector& Point, float DesiredSpeedCm, float Dt);
	float ThinkTimer = 0.f;
	float Clock = 0.f;
	FVector FleeFrom = FVector::ZeroVector;
};
