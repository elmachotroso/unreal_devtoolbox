// Copyright Epic Games, Inc. All Rights Reserved.

#include "USDConversionBlueprintLibrary.h"

#include "LevelExporterUSD.h"
#include "UnrealUSDWrapper.h"
#include "USDConversionUtils.h"
#include "USDLayerUtils.h"
#include "USDLog.h"

#include "UsdWrappers/SdfLayer.h"
#include "UsdWrappers/SdfPath.h"
#include "UsdWrappers/UsdPrim.h"
#include "UsdWrappers/UsdStage.h"

#include "Engine/Engine.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/World.h"
#include "ISequencer.h"
#include "LevelEditorSequencerIntegration.h"

namespace UE
{
	namespace UsdConversionBlueprintLibraryImpl
	{
		namespace Private
		{
			void StreamInLevels( ULevel* Level, const TSet<FString>& LevelsToIgnore )
			{
				UWorld* InnerWorld = Level->GetTypedOuter<UWorld>();
				if ( !InnerWorld || InnerWorld->GetStreamingLevels().Num() == 0 )
				{
					return;
				}

				// Ensure our world to export has a context so that level streaming doesn't crash.
				// This is needed exclusively to be able to load other levels from python scripts using just `load_asset`
				// and have them be exportable.
				bool bCreatedContext = false;
				FWorldContext* Context = GEngine->GetWorldContextFromWorld( InnerWorld );
				if ( !Context )
				{
					FWorldContext& NewContext = GEngine->CreateNewWorldContext( EWorldType::EditorPreview );
					NewContext.SetCurrentWorld( InnerWorld );

					bCreatedContext = true;
				}

				// Mark all sublevels that we need to be loaded
				for ( ULevelStreaming* StreamingLevel : InnerWorld->GetStreamingLevels() )
				{
					if ( StreamingLevel )
					{
						// Note how we will always load the sublevel's package even if will ignore this level. This is a workaround
						// to a level streaming bug/quirk:
						// As soon as we first load the sublevels's package the component scene proxies will be (incorrectly?) placed on
						// the vestigial worlds' FScene. When exporting sublevels though, we need the scene proxies to be on the owning
						// world, especially for landscapes as their materials will be baked by essentially taking a camera screenshot
						// from the FScene.
						// Because of that, we have to always at least load the sublevels here so that the FlushLevelStreaming call below can
						// call World->RemoveFromWorld from within ULevelStreaming::UpdateStreamingState when we call FlushLevelStreaming
						// below and leave the loaded level's component's unregistered.
						// This ensures that if we later try exporting this same sublevel, we won't get the components first registered
						// during the process of actually first loading the sublevel (which would have placed them on the vestigial world), but
						// instead, since the level *is already loaded*, a future call to to FlushLevelStreaming below can trigger the landscape
						// components to be registered on the owning world by the World->AddToWorld call.
						const FString StreamingLevelWorldAssetPackageName = StreamingLevel->GetWorldAssetPackageName();
						const FName StreamingLevelWorldAssetPackageFName = StreamingLevel->GetWorldAssetPackageFName();
						ULevel::StreamedLevelsOwningWorld.Add( StreamingLevelWorldAssetPackageFName, InnerWorld );
						UPackage* Package = LoadPackage( nullptr, *StreamingLevelWorldAssetPackageName, LOAD_None );
						ULevel::StreamedLevelsOwningWorld.Remove( StreamingLevelWorldAssetPackageFName );

						const FString LevelName = FPaths::GetBaseFilename( StreamingLevel->GetWorldAssetPackageName() );
						if ( LevelsToIgnore.Contains( LevelName ) )
						{
							continue;
						}

						const bool bInShouldBeLoaded = true;
						StreamingLevel->SetShouldBeLoaded( bInShouldBeLoaded );

						// This is a workaround to a level streaming bug/quirk:
						// We must set these to false here in order to get both our streaming level's current and target
						// state to LoadedNotVisible. If the level is already visible when we call FlushLevelStreaming,
						// our level will go directly to the LoadedVisible state.
						// This may seem desirable, but it means the second FlushLevelStreaming call below won't really do anything.
						// We need ULevelStreaming::UpdateStreamingState to call World->RemoveFromWorld and World->AddToWorld though,
						// as that is the only thing that will force the sublevel components' scene proxies to being added to the
						// *owning* world's FScene, as opposed to being left at the vestigial world's FScenes instead.
						// If we don't do this, we may get undesired effects when exporting anything that relies on the actual FScene,
						// like baking landscape materials (see UE-126953)
						{
							const bool bShouldBeVisible = false;
							StreamingLevel->SetShouldBeVisible( bShouldBeVisible );
							StreamingLevel->SetShouldBeVisibleInEditor( bShouldBeVisible );
						}
					}
				}

				// Synchronously stream in levels
				InnerWorld->FlushLevelStreaming( EFlushLevelStreamingType::Full );

				// Mark all sublevels that we need to be made visible
				// For whatever reason this needs to be done with two separate flushes: One for loading, one for
				// turning visible. If we do this with a single flush the levels will not be synchronously loaded *and* made visible
				// on this same exact frame, and if we're e.g. baking a landscape immediately after this,
				// its material will be baked incorrectly (check test_export_level_landscape_bake.py)
				for ( ULevelStreaming* StreamingLevel : InnerWorld->GetStreamingLevels() )
				{
					if ( StreamingLevel )
					{
						const FString LevelName = FPaths::GetBaseFilename( StreamingLevel->GetWorldAssetPackageName() );
						if ( LevelsToIgnore.Contains( LevelName ) )
						{
							continue;
						}

						const bool bInShouldBeVisible = true;
						StreamingLevel->SetShouldBeVisible( bInShouldBeVisible );
						StreamingLevel->SetShouldBeVisibleInEditor( bInShouldBeVisible );
					}
				}

				// Synchronously show levels right now
				InnerWorld->FlushLevelStreaming( EFlushLevelStreamingType::Visibility );

				if ( bCreatedContext )
				{
					GEngine->DestroyWorldContext( InnerWorld );
				}
			}
		}
	}
}

void UUsdConversionBlueprintLibrary::StreamInRequiredLevels( UWorld* World, const TSet<FString>& LevelsToIgnore )
{
	if ( !World )
	{
		return;
	}

	if ( ULevel* PersistentLevel = World->PersistentLevel )
	{
		UE::UsdConversionBlueprintLibraryImpl::Private::StreamInLevels( PersistentLevel, LevelsToIgnore );
	}
}

void UUsdConversionBlueprintLibrary::RevertSequencerAnimations()
{
	for ( const TWeakPtr<ISequencer>& Sequencer : FLevelEditorSequencerIntegration::Get().GetSequencers() )
	{
		if ( TSharedPtr<ISequencer> PinnedSequencer = Sequencer.Pin() )
		{
			PinnedSequencer->EnterSilentMode();
			PinnedSequencer->RestorePreAnimatedState();
		}
	}
}

void UUsdConversionBlueprintLibrary::ReapplySequencerAnimations()
{
	for ( const TWeakPtr<ISequencer>& Sequencer : FLevelEditorSequencerIntegration::Get().GetSequencers() )
	{
		if ( TSharedPtr<ISequencer> PinnedSequencer = Sequencer.Pin() )
		{
			PinnedSequencer->InvalidateCachedData();
			PinnedSequencer->ForceEvaluate();
			PinnedSequencer->ExitSilentMode();
		}
	}
}

TArray<FString> UUsdConversionBlueprintLibrary::GetLoadedLevelNames( UWorld* World )
{
	TArray<FString> Result;

	for ( ULevelStreaming* StreamingLevel : World->GetStreamingLevels() )
	{
		if ( StreamingLevel && StreamingLevel->IsLevelLoaded() )
		{
			Result.Add( StreamingLevel->GetWorldAssetPackageName() );
		}
	}

	return Result;
}

TArray<FString> UUsdConversionBlueprintLibrary::GetVisibleInEditorLevelNames( UWorld* World )
{
	TArray<FString> Result;

	for ( ULevelStreaming* StreamingLevel : World->GetStreamingLevels() )
	{
		if ( StreamingLevel && StreamingLevel->GetShouldBeVisibleInEditor() )
		{
			Result.Add( StreamingLevel->GetWorldAssetPackageName() );
		}
	}

	return Result;
}

void UUsdConversionBlueprintLibrary::StreamOutLevels( UWorld* OwningWorld, const TArray<FString>& LevelNamesToStreamOut, const TArray<FString>& LevelNamesToHide )
{
	if ( LevelNamesToStreamOut.Num() == 0 && LevelNamesToHide.Num() == 0 )
	{
		return;
	}

	bool bCreatedContext = false;
	FWorldContext* Context = GEngine->GetWorldContextFromWorld( OwningWorld );
	if ( !Context )
	{
		FWorldContext& NewContext = GEngine->CreateNewWorldContext( EWorldType::EditorPreview );
		NewContext.SetCurrentWorld( OwningWorld );

		bCreatedContext = true;
	}

	for ( ULevelStreaming* StreamingLevel : OwningWorld->GetStreamingLevels() )
	{
		if ( StreamingLevel )
		{
			const FString& LevelName = StreamingLevel->GetWorldAssetPackageName();

			if ( LevelNamesToHide.Contains( LevelName ) )
			{
				const bool bInShouldBeVisible = false;
				StreamingLevel->SetShouldBeVisible( bInShouldBeVisible );
				StreamingLevel->SetShouldBeVisibleInEditor( bInShouldBeVisible );
			}

			if ( LevelNamesToStreamOut.Contains( LevelName ) )
			{
				const bool bInShouldBeLoaded = false;
				StreamingLevel->SetShouldBeVisible( bInShouldBeLoaded );
				StreamingLevel->SetShouldBeLoaded( bInShouldBeLoaded );
			}
		}
	}

	if ( bCreatedContext )
	{
		GEngine->DestroyWorldContext( OwningWorld );
	}
}

TSet<AActor*> UUsdConversionBlueprintLibrary::GetActorsToConvert( UWorld* World )
{
	TSet<AActor*> Result;
	if ( !World )
	{
		return Result;
	}

	auto CollectActors = [ &Result ]( ULevel* Level )
	{
		if ( !Level )
		{
			return;
		}

		Result.Append( Level->Actors );
	};

	CollectActors( World->PersistentLevel );

	for ( ULevelStreaming* StreamingLevel : World->GetStreamingLevels() )
	{
		if ( StreamingLevel && StreamingLevel->IsLevelLoaded() && StreamingLevel->GetShouldBeVisibleInEditor() )
		{
			if ( ULevel* Level = StreamingLevel->GetLoadedLevel() )
			{
				CollectActors( Level );
			}
		}
	}

	Result.Remove( nullptr );
	return Result;
}

FString UUsdConversionBlueprintLibrary::MakePathRelativeToLayer( const FString& AnchorLayerPath, const FString& PathToMakeRelative )
{
#if USE_USD_SDK
	if ( UE::FSdfLayer Layer = UE::FSdfLayer::FindOrOpen( *AnchorLayerPath ) )
	{
		FString Path = PathToMakeRelative;
		UsdUtils::MakePathRelativeToLayer( Layer, Path );
		return Path;
	}
	else
	{
		UE_LOG(LogUsd, Error, TEXT("Failed to find a layer with path '%s' to make the path '%s' relative to"), *AnchorLayerPath, *PathToMakeRelative );
		return PathToMakeRelative;
	}
#else
	return FString();
#endif // USE_USD_SDK
}

void UUsdConversionBlueprintLibrary::InsertSubLayer( const FString& ParentLayerPath, const FString& SubLayerPath, int32 Index /*= -1 */ )
{
#if USE_USD_SDK
	if ( ParentLayerPath.IsEmpty() || SubLayerPath.IsEmpty() )
	{
		return;
	}

	if ( UE::FSdfLayer Layer = UE::FSdfLayer::FindOrOpen( *ParentLayerPath ) )
	{
		UsdUtils::InsertSubLayer( Layer, *SubLayerPath, Index );
	}
	else
	{
		UE_LOG( LogUsd, Error, TEXT( "Failed to find a parent layer '%s' when trying to insert sublayer '%s'" ), *ParentLayerPath, *SubLayerPath );
	}
#endif // USE_USD_SDK
}

void UUsdConversionBlueprintLibrary::AddPayload( const FString& ReferencingStagePath, const FString& ReferencingPrimPath, const FString& TargetStagePath )
{
#if USE_USD_SDK
	TArray<UE::FUsdStage> PreviouslyOpenedStages = UnrealUSDWrapper::GetAllStagesFromCache();

	// Open using the stage cache as it's very likely this stage is already in there anyway
	UE::FUsdStage ReferencingStage = UnrealUSDWrapper::OpenStage( *ReferencingStagePath, EUsdInitialLoadSet::LoadAll );
	if ( ReferencingStage )
	{
		if ( UE::FUsdPrim ReferencingPrim = ReferencingStage.GetPrimAtPath( UE::FSdfPath( *ReferencingPrimPath ) ) )
		{
			UsdUtils::AddPayload( ReferencingPrim, *TargetStagePath );
		}
	}

	// Cleanup or else the stage cache will keep these stages open forever
	if ( !PreviouslyOpenedStages.Contains( ReferencingStage ) )
	{
		UnrealUSDWrapper::EraseStageFromCache( ReferencingStage );
	}
#endif // USE_USD_SDK
}

FString UUsdConversionBlueprintLibrary::GetPrimPathForObject( const UObject* ActorOrComponent, const FString& ParentPrimPath, bool bUseActorFolders )
{
#if USE_USD_SDK
	return UsdUtils::GetPrimPathForObject( ActorOrComponent, ParentPrimPath, bUseActorFolders );
#else
	return {};
#endif // USE_USD_SDK
}

FString UUsdConversionBlueprintLibrary::GetSchemaNameForComponent( const USceneComponent* Component )
{
#if USE_USD_SDK
	if ( Component )
	{
		return UsdUtils::GetSchemaNameForComponent( *Component );
	}
#endif // USE_USD_SDK

	return {};
}
