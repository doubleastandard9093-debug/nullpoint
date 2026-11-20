#include "HDPlayerState.h"
#include "HDUniverseSubsystem.h"
#include "HeliosDrift.h"
#include "Net/UnrealNetwork.h"
#include "Misc/DateTime.h"

double AHDPlayerState::NowUnix()
{
	return (double)FDateTime::UtcNow().ToUnixTimestamp();
}

void AHDPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDPlayerState, Credits);
	DOREPLIFETIME(AHDPlayerState, Corp);
	DOREPLIFETIME(AHDPlayerState, bDuelFlag);
	DOREPLIFETIME(AHDPlayerState, CriminalUntil);
	DOREPLIFETIME_CONDITION(AHDPlayerState, Revealed, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AHDPlayerState, Standings, COND_OwnerOnly);
	DOREPLIFETIME_CONDITION(AHDPlayerState, Skills, COND_OwnerOnly);
}

float AHDPlayerState::GetStanding(const FName& Faction) const
{
	for (const FHDStanding& S : Standings) { if (S.Faction == Faction) return S.Value; }
	return 0.f;
}

void AHDPlayerState::AddStanding(const FName& Faction, float Delta)
{
	if (Faction.IsNone()) return;
	for (FHDStanding& S : Standings)
	{
		if (S.Faction == Faction) { S.Value = FMath::Clamp(S.Value + Delta, -10.f, 10.f); return; }
	}
	FHDStanding N; N.Faction = Faction; N.Value = FMath::Clamp(Delta, -10.f, 10.f);
	Standings.Add(N);
}

int32 AHDPlayerState::GetSkill(const FName& Skill) const
{
	for (const FHDSkill& S : Skills) { if (S.Skill == Skill) return S.Level; }
	return 0;
}

void AHDPlayerState::SetSkill(const FName& Skill, int32 Level)
{
	Level = FMath::Clamp(Level, 0, 5);
	for (FHDSkill& S : Skills) { if (S.Skill == Skill) { S.Level = Level; return; } }
	FHDSkill N; N.Skill = Skill; N.Level = Level;
	Skills.Add(N);
}

bool AHDPlayerState::IsCriminal() const { return CriminalUntil > NowUnix(); }

void AHDPlayerState::MarkCriminal(float Seconds)
{
	CriminalUntil = FMath::Max(CriminalUntil, NowUnix() + Seconds);
}

bool AHDPlayerState::TrySpend(int64 Amount)
{
	if (Amount <= 0) return true;
	if (Credits < Amount) return false;
	Credits -= Amount;
	return true;
}

// ---------------------------------------------------------------------------------------------

void AHDGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AHDGameState, UniverseSeed);
	DOREPLIFETIME(AHDGameState, UniverseSystemCount);
	DOREPLIFETIME(AHDGameState, WeekOverride);
	DOREPLIFETIME(AHDGameState, Wars);
}

void AHDGameState::OnRep_Universe()
{
	UHDUniverseSubsystem* U = UHDUniverseSubsystem::Get(this);
	if (!U || UniverseSystemCount <= 0) return;
	if (U->GetSeed() != UniverseSeed || U->NumSystems() != UniverseSystemCount)
	{
		UE_LOG(LogHelios, Log, TEXT("Client universe differs from server (seed %d/%d, systems %d/%d): regenerating"), U->GetSeed(), UniverseSeed, U->NumSystems(), UniverseSystemCount);
		U->Generate(UniverseSeed, UniverseSystemCount);
	}
	U->SetWeekOverride(WeekOverride);
}

bool AHDGameState::AreAtWar(const FName& CorpA, const FName& CorpB) const
{
	if (CorpA.IsNone() || CorpB.IsNone() || CorpA == CorpB) return false;
	const double Now = AHDPlayerState::NowUnix();
	for (const FHDWar& W : Wars)
	{
		const bool bPair = (W.Aggressor == CorpA && W.Defender == CorpB) || (W.Aggressor == CorpB && W.Defender == CorpA);
		if (bPair && Now >= W.LiveAt) return true;
	}
	return false;
}

void AHDGameState::DeclareWar(const FName& Aggressor, const FName& Defender)
{
	if (Aggressor.IsNone() || Defender.IsNone() || Aggressor == Defender) return;
	for (const FHDWar& W : Wars) { if (W.Aggressor == Aggressor && W.Defender == Defender) return; }
	FHDWar W;
	W.Aggressor = Aggressor;
	W.Defender = Defender;
	W.LiveAt = AHDPlayerState::NowUnix() + WarNoticeSeconds;
	Wars.Add(W);
}
