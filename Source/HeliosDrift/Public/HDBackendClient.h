#pragma once
#include "CoreMinimal.h"

/** Ship record as stored by the backend (/v1/pilots/{id}/ship). */
struct FHDShipRecord
{
	FString PilotId;
	FString Hull = TEXT("skiff");
	FString SystemId;
	FVector Location = FVector::ZeroVector;
	FRotator Rotation = FRotator::ZeroRotator;
	float HullHP = 1000.f;
};

DECLARE_DELEGATE_TwoParams(FHDOnShipLoaded, bool /*bFound*/, const FHDShipRecord& /*Record*/);
/** Versioned state document (M7). Status: 200 found, 404 none stored yet, 0 unreachable, else the HTTP error. */
DECLARE_DELEGATE_ThreeParams(FHDOnDocLoaded, int32 /*Status*/, int64 /*Version*/, const FString& /*DataJson*/);
/** Status: 200 saved (Version = new version), 409 someone else wrote first (Version = theirs), 0 unreachable, else the HTTP error. */
DECLARE_DELEGATE_TwoParams(FHDOnDocSaved, int32 /*Status*/, int64 /*Version*/);

/** Minimal HTTP client for the persistent backend. Server-side only. Config: [HeliosDrift.Backend] BaseUrl / ServerKey in DefaultEngine.ini */
class HELIOSDRIFT_API FHDBackendClient
{
public:
	static FString GetBaseUrl();
	static void LoadShip(const FString& PilotId, FHDOnShipLoaded OnDone);
	static void SaveShip(const FHDShipRecord& Record);

	// ---- M7 world persistence: /v1/world/{economy|territory|rules} and /v1/pilots/{id}/state ----
	static void LoadWorldDoc(const FString& Kind, FHDOnDocLoaded OnDone);
	/** IfVersion: the version this server loaded (0 = must not exist yet, -1 = overwrite unconditionally). */
	static void SaveWorldDoc(const FString& Kind, const FString& DataJson, int64 IfVersion, FHDOnDocSaved OnDone);
	static void LoadPilotState(const FString& PilotId, FHDOnDocLoaded OnDone);
	static void SavePilotState(const FString& PilotId, const FString& DataJson, FHDOnDocSaved OnDone);

private:
	static void LoadDoc(const FString& Url, const FString& What, FHDOnDocLoaded OnDone);
	static void SaveDoc(const FString& Url, const FString& What, const FString& DataJson, int64 IfVersion, FHDOnDocSaved OnDone);
};
