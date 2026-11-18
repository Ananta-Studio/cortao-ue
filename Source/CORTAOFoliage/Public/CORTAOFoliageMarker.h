// CORTAO for Unreal Engine. Copyright (c) 2026 Ananta Studio. MIT License, see LICENSE.
//
// Which meshes CORTAO treats as foliage.
//
// CORTAO judges "what is above me" by the shading normal. Stylised grass lights its upright, two-sided blades with
// normals pointing up, so under CORTAO every blade occludes itself and comes out dark and speckled. The renderer side
// (CORTAOMainPass.usf) handles foliage differently -- but it must know which pixels are foliage. Two Sided Foliage
// materials say so in the GBuffer. Grass materials are often Default Lit, so this subsystem puts their components on an
// extra lighting channel (channel 2 by default; a light is on channel 0 unless a project moved it, so nothing is lit
// differently) and the renderer reads the engine's lighting-channel texture (r.CORTAO.FoliageChannels).
//
// What counts as foliage: a static mesh component whose EVERY material is two-sided and masked (an alpha-cut card: grass,
// plants, leaves), plus the AlwaysFoliage list, minus the NeverFoliage list (material or its base material). A tree whose
// bark is opaque, or a fence with plank sections, is not marked.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "Subsystems/WorldSubsystem.h"
#include "Containers/Ticker.h"
#include "CORTAOFoliageMarker.generated.h"

class UMaterialInterface;
class UPrimitiveComponent;
class UStaticMeshComponent;

/** Project Settings > Plugins > CORTAO Foliage. Stored in DefaultGame.ini, [/Script/CORTAOFoliage.CORTAOFoliageSettings]. */
UCLASS(config = Game, defaultconfig, meta = (DisplayName = "CORTAO Foliage"))
class CORTAOFOLIAGE_API UCORTAOFoliageSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UCORTAOFoliageSettings();

	/** Mark static mesh components whose every material is two-sided and masked (alpha-cut cards). */
	UPROPERTY(Config, EditAnywhere, Category = "CORTAO Foliage")
	bool bMarkTwoSidedMaskedCards = true;

	/** The lighting channel used as the marker: 1 or 2. Must match r.CORTAO.FoliageChannels (bit n = channel n: 2 for channel 1, 4 for channel 2). */
	UPROPERTY(Config, EditAnywhere, Category = "CORTAO Foliage", meta = (ClampMin = 1, ClampMax = 2))
	int32 MarkerChannel = 2;

	/** Materials (or base materials) always treated as foliage. */
	UPROPERTY(Config, EditAnywhere, Category = "CORTAO Foliage")
	TArray<TSoftObjectPtr<UMaterialInterface>> AlwaysFoliage;

	/** Materials (or base materials) never treated as foliage, even when two-sided and masked. */
	UPROPERTY(Config, EditAnywhere, Category = "CORTAO Foliage")
	TArray<TSoftObjectPtr<UMaterialInterface>> NeverFoliage;
};

/** Puts foliage components on the CORTAO marker channel in game, PIE and editor worlds, while r.CORTAO is on. */
UCLASS()
class CORTAOFOLIAGE_API UCORTAOFoliageMarkerSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	virtual void PostInitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

	/** Every level of this world, again (console: CORTAO.FoliageMarker.Rescan). Returns the components marked. */
	int32 ScanWorld();

protected:
	virtual bool DoesSupportWorldType(const EWorldType::Type WorldType) const override;

private:
	int32 ScanLevel(ULevel* Level);
	int32 ScanActor(AActor* Actor);
	bool ScanComponent(UStaticMeshComponent* Component);
	bool IsFoliageMaterial(UMaterialInterface* Material);
	bool Tick(float DeltaTime);

	void OnLevelAdded(ULevel* Level, UWorld* InWorld);
	void OnActorSpawned(AActor* Actor);

	TMap<TWeakObjectPtr<UMaterialInterface>, bool> MaterialCache;
	TSet<TWeakObjectPtr<UPrimitiveComponent>> Seen;
	TSet<FSoftObjectPath> Always;
	TSet<FSoftObjectPath> Never;

	FDelegateHandle LevelAddedHandle;
	FDelegateHandle ActorSpawnedHandle;
	FTSTicker::FDelegateHandle TickHandle;
	bool bScannedOnce = false;
	bool bWasActive = false;
};
