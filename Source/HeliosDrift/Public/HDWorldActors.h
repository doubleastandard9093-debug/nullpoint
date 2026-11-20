#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "HDUniverseTypes.h"
#include "HDWorldActors.generated.h"

class UStaticMeshComponent;
class USphereComponent;
class UBoxComponent;
class UStaticMesh;
class UMaterialInterface;
class APlayerController;

/**
 * Base for everything the system director spawns: bodies, stations, gates, sites, lanes, asteroids.
 * Replicated, static (no replicated movement), relevant across a whole star system.
 * Objects with bRequiresScan are hidden on a client until that pilot has scanned them down.
 */
UCLASS(Abstract)
class HELIOSDRIFT_API AHDSpaceObject : public AActor
{
	GENERATED_BODY()
public:
	AHDSpaceObject();
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Universe system index this object belongs to. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Space") int32 SystemIndex = -1;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Space") FName ObjectId;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Space") FString DisplayName;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Space") bool bRequiresScan = false;
	/** Scan signature 0..1; higher is easier to resolve. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Space") float Signature = 1.f;
	/** Dock / jump / interact distance (cm). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Space") float InteractRadius = 1000000.f;

	/** Short label for the HUD / target list, e.g. "Jump gate". */
	virtual FString GetKindLabel() const { return TEXT("Object"); }
	/** Whether the HUD should list it as a target. */
	virtual bool IsTargetable() const { return true; }
	/** True if no scan is needed or the viewing pilot has revealed it. */
	bool IsRevealedTo(const APlayerController* PC) const;

	static UStaticMesh* BasicMesh(const TCHAR* Shape);
	static void Tint(UStaticMeshComponent* Mesh, const FLinearColor& Color);

protected:
	/** Rebuild visuals from replicated data (called on server at spawn and on clients on rep). */
	virtual void ApplyVisual() {}
	UFUNCTION() void OnRep_Visual() { ApplyVisual(); }
	void SetSystemRelevancy();
	float RevealCheckTimer = 0.f;
};

/** Star, planet, moon, gas giant or large asteroid. */
UCLASS()
class HELIOSDRIFT_API AHDCelestialBody : public AHDSpaceObject
{
	GENERATED_BODY()
public:
	AHDCelestialBody();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Body") TObjectPtr<USphereComponent> Collision;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Body") TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Body") FHDBody Body;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Body") int32 BodyIndex = -1;
	/** Gas giants: this week's cloud density tier. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Body") EHDGasDensity Density = EHDGasDensity::Consistent;

	void InitBody(int32 InSystem, int32 InIndex, const FHDBody& InBody, EHDGasDensity InDensity);

	/** Rendered radius in cm (surface). */
	double GetRadiusCm() const { return Body.VisualRadiusCm; }
	/** Altitude above the surface (cm). Gas giants: above the cloud tops. */
	double GetAltitude(const FVector& WorldLoc) const { return FVector::Dist(WorldLoc, GetActorLocation()) - Body.VisualRadiusCm; }
	bool IsGasGiant() const { return Body.Type == EHDBodyType::GasGiant; }
	bool IsStar() const { return Body.Type == EHDBodyType::Star; }

	virtual FString GetKindLabel() const override;

protected:
	virtual void ApplyVisual() override;
};

/** Space station or gas-giant fuel platform (doc 19 §7D.0 / §7D.1b). */
UCLASS()
class HELIOSDRIFT_API AHDStationActor : public AHDSpaceObject
{
	GENERATED_BODY()
public:
	AHDStationActor();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station") TObjectPtr<UBoxComponent> Collision;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station") TObjectPtr<UStaticMeshComponent> Core;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station") TObjectPtr<UStaticMeshComponent> Ring;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Station") TObjectPtr<UStaticMeshComponent> Spire;

	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Station") FHDStation Info;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Station") int32 StationIndex = -1;
	/** Company that owns it (player-built or captured pirate base). None = NPC. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Station") FName OwnerCorp;
	/** Fuel platforms: jump/ship fuel in stock (t). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Station") float FuelStock = 0.f;

	void InitStation(int32 InSystem, int32 InIndex, const FHDStation& InInfo);
	virtual FString GetKindLabel() const override;

	/** Where a docked ship is held (world). */
	FVector GetDockPoint() const;

protected:
	virtual void ApplyVisual() override;
};

/** Jump gate, large gate, wormhole mouth, stellar anomaly, galactic ruin or instance jump lane (doc 13). */
UCLASS()
class HELIOSDRIFT_API AHDJumpPointActor : public AHDSpaceObject
{
	GENERATED_BODY()
public:
	AHDJumpPointActor();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Jump") TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Jump") FHDJumpLink Link;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Jump") FString DestinationName;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Jump") EHDSecurityBand DestinationBand = EHDSecurityBand::Secured;

	void InitLink(int32 InSystem, const FHDJumpLink& InLink, const FString& InDestName, EHDSecurityBand InDestBand);
	virtual FString GetKindLabel() const override;

protected:
	virtual void ApplyVisual() override;
};

/** Scannable site beacon: encounter anomaly, hulk, deep core, pirate base, sentinel vault, derelict. Contents are streamed by the director. */
UCLASS()
class HELIOSDRIFT_API AHDSiteActor : public AHDSpaceObject
{
	GENERATED_BODY()
public:
	AHDSiteActor();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Site") TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Site") FHDSite Site;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Site") int32 SiteIndex = -1;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Site") EHDSecurityBand Band = EHDSecurityBand::Secured;
	/** Cleared sites stop spawning until they respawn. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Site") bool bCleared = false;
	/** Claim owner who may toll this site (Borderland, doc 20). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Site") FName TollOwner;

	void InitSite(int32 InSystem, int32 InIndex, const FHDSite& InSite, EHDSecurityBand InBand);
	virtual FString GetKindLabel() const override;

	/** Server: spawned contents (NPCs, wrecks, crates, rocks). */
	UPROPERTY() TArray<TObjectPtr<AActor>> Contents;
	bool bStreamedIn = false;
	float EmptyTime = 0.f;

protected:
	virtual void ApplyVisual() override;
};

/** NPC or player trade lane (doc 13): enter at either end, travel fast to the other. */
UCLASS()
class HELIOSDRIFT_API AHDTradeLaneActor : public AHDSpaceObject
{
	GENERATED_BODY()
public:
	AHDTradeLaneActor();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lane") TObjectPtr<UStaticMeshComponent> Beam;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lane") TObjectPtr<UStaticMeshComponent> StartRing;
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Lane") TObjectPtr<UStaticMeshComponent> EndRing;
	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Lane") FHDTradeLane Lane;
	/** World-space ends. */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Lane") FVector StartWorld = FVector::ZeroVector;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Lane") FVector EndWorld = FVector::ZeroVector;
	/** Owner-bought suppression stops interdiction on this lane (doc 15). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Lane") bool bSuppressed = false;

	void InitLane(int32 InSystem, const FHDTradeLane& InLane, const FVector& Origin);
	virtual FString GetKindLabel() const override { return Lane.bNPC ? TEXT("Trade lane (Accord)") : TEXT("Trade lane (player)"); }
	/** Distance from a point to the nearer lane end, and which end (true = start). */
	double DistanceToEnd(const FVector& P, bool& bOutStart) const;

protected:
	virtual void ApplyVisual() override;
};

/** One minable rock, ice chunk or gas pocket (doc 16 §7A). Ore is finite per rock. */
UCLASS()
class HELIOSDRIFT_API AHDAsteroidActor : public AHDSpaceObject
{
	GENERATED_BODY()
public:
	AHDAsteroidActor();
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Deposit") TObjectPtr<UStaticMeshComponent> Mesh;
	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Deposit") EHDFieldType FieldType = EHDFieldType::AsteroidBelt;
	UPROPERTY(ReplicatedUsing = OnRep_Visual, BlueprintReadOnly, Category = "Deposit") float RadiusCm = 3000.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Deposit") FName ItemId;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Deposit") float Remaining = 1000.f;
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Deposit") float MaxUnits = 1000.f;
	/** Deep-core rock: richer, guarded (doc 16). */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Deposit") bool bDeepCore = false;
	/** Field this rock belongs to (for regional consumption tracking). */
	UPROPERTY() FName FieldId;
	int32 RegionIndex = -1;

	void InitDeposit(int32 InSystem, EHDFieldType InType, const FName& InItem, float Units, float InRadiusCm, bool bInDeepCore);
	/** Server: remove up to Amount units; returns what was taken. */
	float Extract(float Amount);
	bool IsDepleted() const { return Remaining <= 0.f; }
	virtual FString GetKindLabel() const override;

protected:
	virtual void ApplyVisual() override;
};
