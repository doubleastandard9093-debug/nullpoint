#pragma once
#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "GameFramework/GameStateBase.h"
#include "HDPlayerState.generated.h"

/** Standing with one NPC faction (-10..10). */
USTRUCT(BlueprintType)
struct FHDStanding
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Pilot") FName Faction;
	UPROPERTY(BlueprintReadOnly, Category = "Pilot") float Value = 0.f;
};

/** Pilot skill level 0..5. */
USTRUCT(BlueprintType)
struct FHDSkill
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Pilot") FName Skill;
	UPROPERTY(BlueprintReadOnly, Category = "Pilot") int32 Level = 0;
};

/** Pilot-level persistent state: wallet, company, standings, skills, scanned signatures, law flags. */
UCLASS()
class HELIOSDRIFT_API AHDPlayerState : public APlayerState
{
	GENERATED_BODY()
public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Pilot") int64 Credits = 25000;
	/** Company (corp) id. Empty = solo pilot. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Pilot") FName Corp;
	/** Opt-in duel flag: the only PVP allowed in Secured space (doc 15). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Pilot") bool bDuelFlag = false;
	/** Unix time until which this pilot is a criminal (attacked a lawful target in Secured/Restricted). */
	UPROPERTY(Replicated) double CriminalUntil = 0.0;

	/** Signatures (wormholes, sites) this pilot has scanned down. Owner only. */
	UPROPERTY(Replicated) TArray<FName> Revealed;
	UPROPERTY(Replicated) TArray<FHDStanding> Standings;
	UPROPERTY(Replicated) TArray<FHDSkill> Skills;

	bool HasRevealed(const FName& Id) const { return Revealed.Contains(Id); }
	void Reveal(const FName& Id) { if (!Id.IsNone()) Revealed.AddUnique(Id); }

	float GetStanding(const FName& Faction) const;
	void AddStanding(const FName& Faction, float Delta);
	int32 GetSkill(const FName& Skill) const;
	void SetSkill(const FName& Skill, int32 Level);

	bool IsCriminal() const;
	void MarkCriminal(float Seconds);

	/** Server: spend credits if available. */
	bool TrySpend(int64 Amount);
	void AddCredits(int64 Amount) { Credits = FMath::Max<int64>(0, Credits + Amount); }

	static double NowUnix();
};

/** Company wars (Restricted PVP needs a declared war with 48 h notice, doc 15). */
USTRUCT(BlueprintType)
struct FHDWar
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category = "Rules") FName Aggressor;
	UPROPERTY(BlueprintReadOnly, Category = "Rules") FName Defender;
	/** Unix time the war becomes live (declaration + notice). */
	UPROPERTY() double LiveAt = 0.0;
};

/** Replicates the universe seed so clients build the same galaxy, plus global rule state. */
UCLASS()
class HELIOSDRIFT_API AHDGameState : public AGameStateBase
{
	GENERATED_BODY()
public:
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(ReplicatedUsing = OnRep_Universe, BlueprintReadOnly, Category = "Universe") int32 UniverseSeed = 0;
	UPROPERTY(ReplicatedUsing = OnRep_Universe, BlueprintReadOnly, Category = "Universe") int32 UniverseSystemCount = 0;
	/** Week index override for testing re-rolls (MIN = live clock). */
	UPROPERTY(ReplicatedUsing = OnRep_Universe, BlueprintReadOnly, Category = "Universe") int64 WeekOverride = MIN_int64;

	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Rules") TArray<FHDWar> Wars;

	/** Notice period before a declared war goes live (seconds). Default 48 h. */
	UPROPERTY(EditAnywhere, Category = "Rules") double WarNoticeSeconds = 48.0 * 3600.0;

	bool AreAtWar(const FName& CorpA, const FName& CorpB) const;
	void DeclareWar(const FName& Aggressor, const FName& Defender);

protected:
	UFUNCTION() void OnRep_Universe();
};
