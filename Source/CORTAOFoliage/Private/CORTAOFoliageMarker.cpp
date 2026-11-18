// CORTAO for Unreal Engine. Copyright (c) 2026 Ananta Studio. MIT License, see LICENSE.
// See CORTAOFoliageMarker.h.

#include "CORTAOFoliageMarker.h"

#include "Components/InstancedStaticMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/IConsoleManager.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectIterator.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, CORTAOFoliage);

DEFINE_LOG_CATEGORY_STATIC(LogCORTAOFoliage, Log, All);

namespace CORTAOFoliage
{
	static TAutoConsoleVariable<int32> CVarMarker(
		TEXT("r.CORTAO.FoliageMarker"), 1,
		TEXT("While r.CORTAO is on, put foliage cards (every material two-sided and masked, or listed in Project Settings > Plugins > ")
		TEXT("CORTAO Foliage) on the CORTAO marker lighting channel as they load. 0: mark nothing new (already marked components stay ")
		TEXT("marked; r.CORTAO.FoliageChannels 0 makes the renderer ignore the marker)."),
		ECVF_Default);

	static FAutoConsoleCommandWithWorld CmdRescan(
		TEXT("CORTAO.FoliageMarker.Rescan"),
		TEXT("Scan every level of this world for foliage cards again and mark them for CORTAO."),
		FConsoleCommandWithWorldDelegate::CreateLambda([](UWorld* World)
		{
			if (World)
			{
				if (UCORTAOFoliageMarkerSubsystem* Subsystem = World->GetSubsystem<UCORTAOFoliageMarkerSubsystem>())
				{
					const int32 Marked = Subsystem->ScanWorld();
					UE_LOG(LogCORTAOFoliage, Display, TEXT("CORTAO foliage marker: %d components marked in %s"), Marked, *World->GetName());
				}
			}
		}));

	/** The marker works only while CORTAO itself is on: a project that has the plugin but not the effect keeps its lighting channels. */
	static bool IsMarkerActive()
	{
		if (CVarMarker.GetValueOnGameThread() == 0)
		{
			return false;
		}
		// The renderer module's switch. It registers at PostConfigInit, long before any world exists; where that module is
		// not built (another platform) there is nothing to mark for.
		static const IConsoleVariable* EnableCVar = nullptr;
		if (!EnableCVar)
		{
			EnableCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CORTAO"));
		}
		return EnableCVar && EnableCVar->GetInt() != 0;
	}
}

UCORTAOFoliageSettings::UCORTAOFoliageSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("CORTAO Foliage");
}

bool UCORTAOFoliageMarkerSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE || WorldType == EWorldType::Editor;
}

void UCORTAOFoliageMarkerSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	const UCORTAOFoliageSettings* Settings = GetDefault<UCORTAOFoliageSettings>();
	for (const TSoftObjectPtr<UMaterialInterface>& Entry : Settings->AlwaysFoliage)
	{
		Always.Add(Entry.ToSoftObjectPath());
	}
	for (const TSoftObjectPtr<UMaterialInterface>& Entry : Settings->NeverFoliage)
	{
		Never.Add(Entry.ToSoftObjectPath());
	}

	LevelAddedHandle = FWorldDelegates::LevelAddedToWorld.AddUObject(this, &UCORTAOFoliageMarkerSubsystem::OnLevelAdded);
}

void UCORTAOFoliageMarkerSubsystem::PostInitialize()
{
	Super::PostInitialize();

	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}
	ActorSpawnedHandle = World->AddOnActorSpawnedHandler(
		FOnActorSpawned::FDelegate::CreateUObject(this, &UCORTAOFoliageMarkerSubsystem::OnActorSpawned));
	// Instanced components appear without an actor being spawned (landscape grass, a foliage type painted for the first
	// time): a light pass over them once a second. The same tick notices CORTAO being switched on later.
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UCORTAOFoliageMarkerSubsystem::Tick), 1.0f);

	if (World->WorldType == EWorldType::Editor)
	{
		ScanWorld();
	}
}

void UCORTAOFoliageMarkerSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);
	const int32 Marked = ScanWorld();
	UE_LOG(LogCORTAOFoliage, Log, TEXT("CORTAO foliage marker: %d components marked in %s"), Marked, *InWorld.GetName());
}

void UCORTAOFoliageMarkerSubsystem::Deinitialize()
{
	FWorldDelegates::LevelAddedToWorld.Remove(LevelAddedHandle);
	if (UWorld* World = GetWorld())
	{
		World->RemoveOnActorSpawnedHandler(ActorSpawnedHandle);
	}
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	Super::Deinitialize();
}

int32 UCORTAOFoliageMarkerSubsystem::ScanWorld()
{
	Seen.Reset();
	MaterialCache.Reset();
	bScannedOnce = true;
	bWasActive = CORTAOFoliage::IsMarkerActive();
	int32 Marked = 0;
	if (UWorld* World = GetWorld())
	{
		for (ULevel* Level : World->GetLevels())
		{
			Marked += ScanLevel(Level);
		}
	}
	return Marked;
}

int32 UCORTAOFoliageMarkerSubsystem::ScanLevel(ULevel* Level)
{
	int32 Marked = 0;
	if (Level)
	{
		for (AActor* Actor : Level->Actors)
		{
			Marked += ScanActor(Actor);
		}
	}
	return Marked;
}

int32 UCORTAOFoliageMarkerSubsystem::ScanActor(AActor* Actor)
{
	if (!IsValid(Actor) || !CORTAOFoliage::IsMarkerActive())
	{
		return 0;
	}
	TInlineComponentArray<UStaticMeshComponent*> Components(Actor);
	int32 Marked = 0;
	for (UStaticMeshComponent* Component : Components)
	{
		Marked += ScanComponent(Component) ? 1 : 0;
	}
	return Marked;
}

bool UCORTAOFoliageMarkerSubsystem::ScanComponent(UStaticMeshComponent* Component)
{
	if (!IsValid(Component) || Component->IsTemplate() || Component->GetWorld() != GetWorld() || !Component->GetStaticMesh())
	{
		return false;
	}
	bool bAlreadySeen = false;
	Seen.Add(Component, &bAlreadySeen);
	if (bAlreadySeen)
	{
		return false;
	}

	const int32 Channel = FMath::Clamp(GetDefault<UCORTAOFoliageSettings>()->MarkerChannel, 1, 2);
	const FLightingChannels& Channels = Component->LightingChannels;
	if ((Channel == 1 && Channels.bChannel1) || (Channel == 2 && Channels.bChannel2))
	{
		return false;
	}

	const int32 NumMaterials = Component->GetNumMaterials();
	if (NumMaterials == 0)
	{
		return false;
	}
	for (int32 Index = 0; Index < NumMaterials; ++Index)
	{
		if (!IsFoliageMaterial(Component->GetMaterial(Index)))
		{
			return false;
		}
	}

	// A light is on channel 0 unless the project moved it, so a component on 0 + the marker is lit exactly as before.
	Component->SetLightingChannels(Channels.bChannel0, Channel == 1 ? true : Channels.bChannel1, Channel == 2 ? true : Channels.bChannel2);
	return true;
}

bool UCORTAOFoliageMarkerSubsystem::IsFoliageMaterial(UMaterialInterface* Material)
{
	if (!Material)
	{
		return false;
	}
	if (const bool* Cached = MaterialCache.Find(Material))
	{
		return *Cached;
	}

	const UMaterial* Base = Material->GetBaseMaterial();
	auto Listed = [Material, Base](const TSet<FSoftObjectPath>& Set)
	{
		return Set.Contains(FSoftObjectPath(Material)) || (Base && Set.Contains(FSoftObjectPath(Base)));
	};

	bool bFoliage = false;
	if (Listed(Never))
	{
		bFoliage = false;
	}
	else if (Listed(Always))
	{
		bFoliage = true;
	}
	else
	{
		bFoliage = GetDefault<UCORTAOFoliageSettings>()->bMarkTwoSidedMaskedCards
			&& Material->IsTwoSided() && Material->GetBlendMode() == BLEND_Masked;
	}
	MaterialCache.Add(Material, bFoliage);
	return bFoliage;
}

bool UCORTAOFoliageMarkerSubsystem::Tick(float DeltaTime)
{
	UWorld* World = GetWorld();
	if (!World || !bScannedOnce)
	{
		return true;
	}
	const bool bActive = CORTAOFoliage::IsMarkerActive();
	if (bActive && !bWasActive)
	{
		// CORTAO (or the marker) was switched on after this world was scanned: everything in it, once.
		const int32 Marked = ScanWorld();
		UE_LOG(LogCORTAOFoliage, Log, TEXT("CORTAO foliage marker: %d components marked in %s"), Marked, *World->GetName());
	}
	bWasActive = bActive;
	if (!bActive)
	{
		return true;
	}
	for (TObjectIterator<UInstancedStaticMeshComponent> It; It; ++It)
	{
		if (It->GetWorld() == World)
		{
			ScanComponent(*It);
		}
	}
	if (Seen.Num() > 4096)
	{
		for (auto It = Seen.CreateIterator(); It; ++It)
		{
			if (!It->IsValid())
			{
				It.RemoveCurrent();
			}
		}
	}
	return true;
}

void UCORTAOFoliageMarkerSubsystem::OnLevelAdded(ULevel* Level, UWorld* InWorld)
{
	if (InWorld == GetWorld())
	{
		ScanLevel(Level);
	}
}

void UCORTAOFoliageMarkerSubsystem::OnActorSpawned(AActor* Actor)
{
	ScanActor(Actor);
}
