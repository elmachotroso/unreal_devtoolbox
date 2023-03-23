// Copyright Epic Games, Inc. All Rights Reserved.

#include "Data/LevelSnapshot.h"

#include "CustomSerialization/CustomSerializationDataManager.h"
#include "Data/SnapshotCustomVersion.h"
#include "Data/Util/ActorHashUtil.h"
#include "Data/Util/EquivalenceUtil.h"
#include "Data/Util/SnapshotUtil.h"
#include "Data/Util/Restoration/ActorUtil.h"
#include "LevelSnapshotsSettings.h"
#include "LevelSnapshotsLog.h"
#include "LevelSnapshotsModule.h"
#include "Restorability/SnapshotRestorability.h"
#include "SnapshotConsoleVariables.h"
#include "Util/SortedScopedLog.h"
#include "Util/Restoration/WorldDataUtil.h"

#include "Algo/Accumulate.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "GameFramework/Actor.h"
#include "Misc/ScopeExit.h"
#include "Stats/StatsMisc.h"
#include "UObject/Package.h"
#if WITH_EDITOR && !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
#include "Logging/MessageLog.h"
#endif

void ULevelSnapshot::ApplySnapshotToWorld(UWorld* TargetWorld, const FPropertySelectionMap& SelectionSet)
{
	SCOPED_SNAPSHOT_CORE_TRACE(ApplyToWorld);
	if (TargetWorld == nullptr)
	{
		return;
	}
	
	UE_LOG(LogLevelSnapshots, Log, TEXT("Applying snapshot %s to world %s. %s"), *GetPathName(), *TargetWorld->GetPathName(), *GenerateDebugLogInfo());
	UE_CLOG(MapPath != FSoftObjectPath(TargetWorld), LogLevelSnapshots, Log, TEXT("Snapshot was taken for different world called '%s'"), *MapPath.ToString());
	ON_SCOPE_EXIT
	{
		UE_LOG(LogLevelSnapshots, Log, TEXT("Finished applying snapshot"));
	};
	
	EnsureWorldInitialised();
	UE::LevelSnapshots::Private::ApplyToWorld(SerializedData, Cache, TargetWorld, GetPackage(), SelectionSet);
}

bool ULevelSnapshot::SnapshotWorld(UWorld* TargetWorld)
{
	SCOPED_SNAPSHOT_CORE_TRACE(SnapshotWorld);
	
	if (!ensure(TargetWorld))
	{
		UE_LOG(LogLevelSnapshots, Warning, TEXT("Unable To Snapshot World as World was invalid"));
		return false;
	}

	if (TargetWorld->WorldType != EWorldType::Editor
		&& TargetWorld->WorldType != EWorldType::EditorPreview) // To support tests in editor preview maps
	{
#if WITH_EDITOR && !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		if (TargetWorld->IsPlayInEditor())
		{
			FMessageLog("PIE").Warning(
			NSLOCTEXT("LevelSnapshots", "IncompatibleWorlds", "Taking snapshots in PIE is an experimental feature. The snapshot will work in the same PIE session but may no longer work when you start a new PIE session.")
			);
		}
#endif // WITH_EDITOR
		UE_LOG(LogLevelSnapshots, Warning, TEXT("Level snapshots currently only support editors. Snapshots taken in other world types are experimental any may not function as expected."));
	}

	UE::LevelSnapshots::Private::FLevelSnapshotsModule& Module = UE::LevelSnapshots::Private::FLevelSnapshotsModule::GetInternalModuleInstance();
	if (!Module.CanTakeSnapshot({ this }))
	{
		return false;
	}
	Module.OnPreTakeSnapshot().Broadcast({this});

	EnsureWorldInitialised();
	MapPath = TargetWorld;
	CaptureTime = FDateTime::UtcNow();
	SerializedData = UE::LevelSnapshots::Private::SnapshotWorld(TargetWorld);

	Module.OnPostTakeSnapshot().Broadcast({ this });

	return true;
}

bool ULevelSnapshot::HasChangedSinceSnapshotWasTaken(AActor* WorldActor)
{
	SCOPED_SNAPSHOT_CORE_TRACE(HasChangedSinceSnapshotWasTaken);
	const FSoftObjectPath ActorPath(WorldActor);

	FActorSnapshotData* SavedActorData = SerializedData.ActorData.Find(ActorPath);
	if (!SavedActorData)
	{
		UE_LOG(LogLevelSnapshots, Warning, TEXT("No data found for actor %s"), *ActorPath.ToString());
		return false;
	}

#if WITH_EDITOR
	ECachedDiffResult& CachedDiffResult = CachedDiffedActors.FindOrAdd(WorldActor);
	if (CachedDiffResult != ECachedDiffResult::NotInitialised)
	{
		return CachedDiffResult == ECachedDiffResult::HadChanges;
	}
#endif

	const bool bHasChanged = [&]()
	{
		// Do not slow down old snapshots by computing hash if they had none saved
		const bool bHasHashInfo = SerializedData.SnapshotVersionInfo.GetSnapshotCustomVersion() >= UE::LevelSnapshots::Private::FSnapshotCustomVersion::ActorHash;
		// If the object is already loaded, there is no point in wasting time and computing the hash
		const bool bNeedsHash = Cache.ActorCache.Find(WorldActor) != nullptr;
		
		if (!bHasHashInfo || !bNeedsHash || !UE::LevelSnapshots::Private::HasMatchingHash(SavedActorData->Hash, WorldActor))
		{
			TOptional<TNonNullPtr<AActor>> DeserializedActor = GetDeserializedActor(WorldActor);
			return HasOriginalChangedPropertiesSinceSnapshotWasTaken(DeserializedActor.GetValue(), WorldActor);
		}

		return false;
	}();

#if WITH_EDITOR
	CachedDiffResult = bHasChanged ? ECachedDiffResult::HadChanges : ECachedDiffResult::HadNoChanges;
#endif
	return bHasChanged;
}

bool ULevelSnapshot::HasOriginalChangedPropertiesSinceSnapshotWasTaken(AActor* SnapshotActor, AActor* WorldActor)
{
	return UE::LevelSnapshots::Private::HasOriginalChangedPropertiesSinceSnapshotWasTaken(this, SnapshotActor, WorldActor);
}

FString ULevelSnapshot::GetActorLabel(const FSoftObjectPath& OriginalActorPath) const
{
#if WITH_EDITORONLY_DATA
	const FActorSnapshotData* SerializedActor = SerializedData.ActorData.Find(OriginalActorPath);
	if (SerializedActor && !SerializedActor->ActorLabel.IsEmpty())
	{
		return SerializedActor->ActorLabel;
	}
#endif

	return UE::LevelSnapshots::Private::ExtractLastSubobjectName(OriginalActorPath); 
}

TOptional<TNonNullPtr<AActor>> ULevelSnapshot::GetDeserializedActor(const FSoftObjectPath& OriginalActorPath)
{
	EnsureWorldInitialised();
	return UE::LevelSnapshots::Private::GetDeserializedActor(OriginalActorPath, SerializedData, Cache, GetPackage());
}

int32 ULevelSnapshot::GetNumSavedActors() const
{
	return SerializedData.ActorData.Num();
}

namespace UE::LevelSnapshots::Private::Internal
{
	FSoftObjectPath ExtractPathWithoutSubobjects(UObject* Object)
	{
		int32 ColonIndex;
		const FString Path = Object->GetPathName();
		Path.FindChar(':', ColonIndex);
		return Path.Left(ColonIndex);
	}

	void ConditionBreakOnActor(const FString& NameToSearchFor, const FSoftObjectPath& ActorPath)
	{
		if (!NameToSearchFor.IsEmpty() && ActorPath.ToString().Contains(NameToSearchFor))
		{
			UE_DEBUG_BREAK();
		}
	}
}

void ULevelSnapshot::DiffWorld(UWorld* World, FActorPathConsumer HandleMatchedActor, FActorPathConsumer HandleRemovedActor, FActorConsumer HandleAddedActor) const
{
	SCOPED_SNAPSHOT_CORE_TRACE(DiffWorld);
	
	if (!ensure(World && HandleMatchedActor.IsBound() && HandleRemovedActor.IsBound() && HandleAddedActor.IsBound()))
	{
		return;
	}
	UE_LOG(LogLevelSnapshots, Log, TEXT("Diffing snapshot %s in world %s. %s"), *GetPathName(), *World->GetPathName(), *GenerateDebugLogInfo());
	ON_SCOPE_EXIT
	{
		UE_LOG(LogLevelSnapshots, Log, TEXT("Finished diffing snapshot"));
	};


	// Find actors that are not present in the snapshot
	TSet<AActor*> AllActors;
	TSet<FSoftObjectPath> LoadedLevels;
	{
		SCOPED_SNAPSHOT_CORE_TRACE(DiffWorld_FindAllActors);

		const int32 NumActorsInWorld = Algo::Accumulate(World->GetLevels(), 0, [](int64 Size, const ULevel* Level){ return Size + Level->Actors.Num(); });
		AllActors.Reserve(NumActorsInWorld);
		for (ULevel* Level : World->GetLevels())
		{
			LoadedLevels.Add(UE::LevelSnapshots::Private::Internal::ExtractPathWithoutSubobjects(Level));
			
			for (AActor* ActorInLevel : Level->Actors)
			{
				AllActors.Add(ActorInLevel);
			
				// Warning: ActorInLevel can be null, e.g. when an actor was just removed from the world (and still in undo buffer)
				if (IsValid(ActorInLevel) && !SerializedData.HasMatchingSavedActor(ActorInLevel) && UE::LevelSnapshots::Restorability::ShouldConsiderNewActorForRemoval(ActorInLevel))
				{
					HandleAddedActor.Execute(ActorInLevel);
				}
			}
		}
	}
	


	// Try to find world actors and call appropriate callback
	{
		SCOPED_SNAPSHOT_CORE_TRACE(DiffWorld_IteratorAllActors);
		ULevelSnapshotsSettings* Settings = GetMutableDefault<ULevelSnapshotsSettings>();
		
		const bool bShouldLogDiffWorldTimes = UE::LevelSnapshots::ConsoleVariables::CVarLogTimeDiffingMatchedActors.GetValueOnAnyThread();
		const FString DebugActorName = UE::LevelSnapshots::ConsoleVariables::CVarBreakOnDiffMatchedActor.GetValueOnAnyThread();
		FConditionalSortedScopedLog SortedItems(bShouldLogDiffWorldTimes);
		
		SerializedData.ForEachOriginalActor([&HandleMatchedActor, &HandleRemovedActor, &HandleAddedActor, &AllActors, &LoadedLevels, Settings, &DebugActorName, &SortedItems](const FSoftObjectPath& OriginalActorPath, const FActorSnapshotData& SavedData)
		{
			const FSoftObjectPath LevelPath = OriginalActorPath.GetAssetPathString();
			if (!LoadedLevels.Contains(LevelPath))
			{
				UE_LOG(LogLevelSnapshots, Log, TEXT("Skipping actor %s because level %s is not loaded or does not exist (see Levels window)."), *OriginalActorPath.ToString(), *LevelPath.ToString());
				return;
			}
			
			UObject* ResolvedActor = OriginalActorPath.ResolveObject();
			// OriginalActorPath may still resolve to a live actor if it was just removed. We need to check the ULevel::Actors to see whether it was removed.
			const bool bWasRemovedFromWorld = ResolvedActor == nullptr || !AllActors.Contains(Cast<AActor>(ResolvedActor));
			// We do not need to call IsActorDesirableForCapture: it was already called when we took this snapshot
			if (bWasRemovedFromWorld)
			{
				HandleRemovedActor.Execute(OriginalActorPath);
				return;
			}

			UClass* ActorClass = SavedData.ActorClass.TryLoadClass<AActor>();
			if (!ActorClass)
			{
				UE_LOG(LogLevel, Warning, TEXT("Cannot find class %s. Saved actor %s will not be restored."), *SavedData.ActorClass.ToString(), *OriginalActorPath.ToString());
				return;
			}
			if (Settings->SkippedClasses.SkippedClasses.Contains(ActorClass))
			{
				return;
			}
			
			// Possible scenario: Right-click actor > Replace Selected Actors with; deletes the original and replaces it with new actor.
			if (ResolvedActor->GetClass() != ActorClass)
			{
				HandleRemovedActor.Execute(OriginalActorPath);
				HandleAddedActor.Execute(Cast<AActor>(ResolvedActor));
			}
			else
			{
				const FScopedLogItem Log = SortedItems.AddScopedLogItem(OriginalActorPath.ToString());
				UE::LevelSnapshots::Private::Internal::ConditionBreakOnActor(DebugActorName, OriginalActorPath);
				SCOPED_SNAPSHOT_CORE_TRACE(HandleMatchedActor)
				
				HandleMatchedActor.Execute(OriginalActorPath);
			}
		});
	}
}

void ULevelSnapshot::SetSnapshotName(const FName& InSnapshotName)
{
	SnapshotName = InSnapshotName;
}

void ULevelSnapshot::SetSnapshotDescription(const FString& InSnapshotDescription)
{
	SnapshotDescription = InSnapshotDescription;
}

void ULevelSnapshot::BeginDestroy()
{
	if (SnapshotContainerWorld)
	{
		DestroyWorld();
	}
	
	Super::BeginDestroy();
}

FString ULevelSnapshot::GenerateDebugLogInfo() const
{
	FSnapshotVersionInfo Current;
	Current.Initialize();
	
	return FString::Printf(TEXT("CaptureTime: %s. SnapshotVersionInfo: %s. Current engine version: %s."), *CaptureTime.ToString(), *GetSerializedData().SnapshotVersionInfo.ToString(), *Current.ToString());
}

void ULevelSnapshot::EnsureWorldInitialised()
{
	if (SnapshotContainerWorld == nullptr)
	{
		SnapshotContainerWorld = NewObject<UWorld>(GetTransientPackage(), NAME_None);
		SnapshotContainerWorld->WorldType = EWorldType::EditorPreview;

		// Note: Do NOT create a FWorldContext for this world.
		// If you do, the render thread will send render commands every tick (and crash cuz we do not init the scene below).
		SnapshotContainerWorld->InitializeNewWorld(UWorld::InitializationValues()
			.InitializeScenes(false)		// This is memory only world: no rendering
            .AllowAudioPlayback(false)
            .RequiresHitProxies(false)		
            .CreatePhysicsScene(false)
            .CreateNavigation(false)
            .CreateAISystem(false)
            .ShouldSimulatePhysics(false)
			.EnableTraceCollision(false)
            .SetTransactional(false)
            .CreateFXSystem(false)
            );

		// Destroy our temporary world when the editor (or game) world is destroyed. Reasons:
		// 1. After unloading a map checks for world GC leaks; it would fatally crash if we did not clear here.
		// 2. Our temp map stores a "copy" of actors from the original world: the original world is no longer relevant, so neither is our temp world.
		if (ensure(GEngine))
		{
			Handle = GEngine->OnWorldDestroyed().AddLambda([WeakThis = TWeakObjectPtr<ULevelSnapshot>(this)](UWorld* WorldBeingDestroyed)
	        {
				const bool bIsEditorOrGameMap = WorldBeingDestroyed->WorldType == EWorldType::Editor || WorldBeingDestroyed->WorldType == EWorldType::Game;
	            if (ensureAlways(WeakThis.IsValid()) && bIsEditorOrGameMap)
	            {
	                WeakThis->DestroyWorld();
	            }
	        });
		}
#if WITH_EDITOR
		FCoreUObjectDelegates::OnObjectModified.AddUObject(this, &ULevelSnapshot::ClearCachedDiffFlag);
#endif
	}

	SerializedData.SnapshotWorld = SnapshotContainerWorld;
}

void ULevelSnapshot::DestroyWorld()
{
	if (ensureAlwaysMsgf(SnapshotContainerWorld, TEXT("World was already destroyed.")))
	{
		if (ensure(GEngine))
		{
			GEngine->OnWorldDestroyed().Remove(Handle);
			Handle.Reset();
		}
				
		SerializedData.SnapshotWorld.Reset();
		ClearCache();
	
		SnapshotContainerWorld->CleanupWorld();
		SnapshotContainerWorld = nullptr;
	}
}

void ULevelSnapshot::ClearCache()
{
	Cache.ActorCache.Reset();
	Cache.SubobjectCache.Reset();
	Cache.ClassDefaultCache.Reset();
	
#if WITH_EDITOR
	CachedDiffedActors.Reset();
#endif
}

#if WITH_EDITOR
void ULevelSnapshot::ClearCachedDiffFlag(UObject* ModifiedObject)
{
	AActor* AsActor = ModifiedObject->IsA<AActor>() ? Cast<AActor>(ModifiedObject) : ModifiedObject->GetTypedOuter<AActor>(); 
	if (AsActor && SerializedData.HasMatchingSavedActor(AsActor))
	{
		CachedDiffedActors.FindOrAdd(AsActor) = ECachedDiffResult::NotInitialised;
	}
}
#endif
