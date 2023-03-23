// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldPartitionChangelistValidator.h"
#include "DataValidationChangelist.h"

#include "AssetData.h"
#include "AssetRegistryModule.h"
#include "ISourceControlProvider.h"
#include "ISourceControlModule.h"
#include "SourceControlHelpers.h"

#include "Engine/World.h"
#include "WorldPartition/WorldPartitionActorDesc.h"
#include "WorldPartition/ActorDescContainer.h"
#include "WorldPartition/WorldPartition.h"

#define LOCTEXT_NAMESPACE "WorldPartitionChangelistValidation"

bool UWorldPartitionChangelistValidator::CanValidateAsset_Implementation(UObject* InAsset) const
{
	return (InAsset != nullptr) && (UDataValidationChangelist::StaticClass() == InAsset->GetClass());
}

EDataValidationResult UWorldPartitionChangelistValidator::ValidateLoadedAsset_Implementation(UObject* InAsset, TArray<FText>& ValidationErrors)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UWorldPartitionChangelistValidator::ValidateLoadedAsset_Implementation);

	UDataValidationChangelist* ChangeList = CastChecked<UDataValidationChangelist>(InAsset);
	
	Errors = &ValidationErrors;
	
	EDataValidationResult Result = ValidateActorsListFromChangeList(ChangeList);

	if (Result == EDataValidationResult::Invalid)
	{
		AssetFails(InAsset, LOCTEXT("WorldPartitionValidationFail", "This changelist contains modifications that aren't valid at the World Partition level. Please see Source Control log and correct the errors."), ValidationErrors);
	}
	else
	{
		AssetPasses(InAsset);
	}
		
	return Result;
}

FString GetPrettyPackageName(const FWorldPartitionActorDescView& Desc)
{
	FString AssetPath = Desc.GetActorPath().ToString();
	
	int32 LastDot = -1;
	if (AssetPath.FindLastChar('.', LastDot))
	{
		AssetPath.LeftInline(LastDot);
	}

	FString AssetName = Desc.GetActorLabel().ToString();
	
	if (AssetName.Len() == 0)
	{
		AssetName = Desc.GetActorName().ToString();
	}
	
	return AssetPath + TEXT(".") + AssetName;
}

// Extract all Actors/Map from Changelist (in OFPA this should be one Actor per Package, and we'll discard all Actors from non WorldPartition maps)
// and add them to a Map of World->Files[] so that we can do one validation per world. Once Worlds are identified, we either the UActorDescContainer 
// from memory (if loaded) or request it to be loaded, we then build a Set of objects that interest us from the Actors in the CL 
EDataValidationResult UWorldPartitionChangelistValidator::ValidateActorsListFromChangeList(UDataValidationChangelist* Changelist)
{
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	FSourceControlChangelistStatePtr ChangelistState = SourceControlProvider.GetState(Changelist->Changelist->AsShared(), EStateCacheUsage::Use);
		 	
	// Figure out which world(s) those assets are in and split the files per world
	TMap<FName, TArray<FAssetData>>	MapToFiles;

	for (const FSourceControlStateRef& File : ChangelistState->GetFilesStates())
	{		
		// Skip deleted files since we're not validating references in this validator 
		if (File->IsDeleted())
		{
			continue;
		}

		FString PackageName;
		if (FPackageName::TryConvertFilenameToLongPackageName(File->GetFilename(), PackageName))
		{						
			// Extract Actor & Map from package
			TArray<FAssetData> PackageAssetsData;
			USourceControlHelpers::GetAssetDataFromPackage(PackageName, PackageAssetsData);
			
			for (FAssetData& AssetData : PackageAssetsData)
			{
				// Check that the asset is an actor
				static FName NAME_ActorMetaDataClass(TEXT("ActorMetaDataClass"));
				if (AssetData.TagsAndValues.Contains(NAME_ActorMetaDataClass)) // Could check AssetData.GetClass()->IsChildOf<AActor>() but this doesn't handle blueprints, all OFPA/WP Actors will have this tag
				{
					// WorldPartition actors are all in OFPA mode so they're external
					// Extract the MapName from the ObjectPath (<PathToPackage>.<mapName>:<level>.<actorName>)
					FSoftObjectPath ActorPath = FSoftObjectPath(AssetData.ObjectPath);
					FName MapAssetName = ActorPath.GetAssetPathName();

					TArray<FAssetData>* Files = MapToFiles.Find(MapAssetName);

					if (!Files)
					{
						if (ULevel::GetIsLevelPartitionedFromPackage(ActorPath.GetLongPackageFName()))
						{
							Files = &MapToFiles.Add(MapAssetName);
						}
					}

					if (Files)	// A null Files indicates a World not using World Partition and OFPA 
					{
						Files->Add(AssetData);
					}
				}
			}
		}
	}

	// For Each world 
	for (TTuple<FName, TArray<FAssetData>>& It : MapToFiles)
	{
		const FName& MapName = It.Get<0>();
		const TArray<FAssetData>& ActorsData = It.Get<1>();

		// Find/Load the ActorDescContainer
		UWorld* World = FindObject<UWorld>(nullptr, *MapName.ToString(), true);

		UActorDescContainer* ActorDescContainer = nullptr;
		
		if (World != nullptr)
		{
			// World is Loaded reuse the ActorDescContainer of the World
			ActorDescContainer = World->GetWorldPartition();
		}
		else
		{
			// Find in memory failed, load the ActorDescContainer
			FSoftObjectPath MapPath = FSoftObjectPath(MapName);

			ActorDescContainer = NewObject<UActorDescContainer>();
			ActorDescContainer->Initialize(nullptr, MapPath.GetLongPackageFName());
		}

		// Build a set of Relevant Actor Guids to scope error messages to what's contained in the CL 
		RelevantActorGuids.Reset();

		for (const FAssetData& ActorData : ActorsData)
		{
			// Get the FWorldPartitionActor			
			const FWorldPartitionActorDesc* ActorDesc = ActorDescContainer->GetActorDesc(ActorData.AssetName.ToString());

			if (ActorDesc != nullptr)
			{
				RelevantActorGuids.Add(ActorDesc->GetGuid());
			}
		}

		// Invoke static WorldPartition Validation from the ActorDescContainer
		UWorldPartition::CheckForErrors(this, ActorDescContainer);		
	}

	if (Errors->Num())
	{
		return EDataValidationResult::Invalid;
	}
		
	return EDataValidationResult::Valid;
	
}

bool UWorldPartitionChangelistValidator::Filter(const FWorldPartitionActorDescView& ActorDescView)
{
	if (RelevantActorGuids.Find(ActorDescView.GetGuid()))
	{
		return true;
	}

	return false;
}

void UWorldPartitionChangelistValidator::OnInvalidReference(const FWorldPartitionActorDescView& ActorDescView, const FGuid& ReferenceGuid)
{
	if (Filter(ActorDescView))
	{
		FText CurrentError = FText::Format(LOCTEXT("DataValidation.Changelist.WorldPartition.InvalidReference", "Actor {0} has a missing reference to %s"),
											FText::FromString(GetPrettyPackageName(ActorDescView)), 
											FText::FromString(ReferenceGuid.ToString()));

		Errors->Add(CurrentError);
	}

}

void UWorldPartitionChangelistValidator::OnInvalidReferenceGridPlacement(const FWorldPartitionActorDescView& ActorDescView, const FWorldPartitionActorDescView& ReferenceActorDescView)
{
	if (Filter(ActorDescView) || Filter(ReferenceActorDescView))
	{
		const FString SpatiallyLoadedActor(TEXT("Spatially loaded actor"));
		const FString NonSpatiallyLoadedActor(TEXT("Non-spatially loaded loaded actor"));

		FText CurrentError = FText::Format(LOCTEXT("DataValidation.Changelist.WorldPartition.InvalidReferenceGridPlacement", "{0} {1} is referencing {2} {3}."),
											FText::FromString(ActorDescView.GetIsSpatiallyLoaded() ? *SpatiallyLoadedActor : *NonSpatiallyLoadedActor),
											FText::FromString(GetPrettyPackageName(ActorDescView)),
											FText::FromString(ReferenceActorDescView.GetIsSpatiallyLoaded() ? *SpatiallyLoadedActor : *NonSpatiallyLoadedActor),
											FText::FromString(GetPrettyPackageName(ReferenceActorDescView)));

		Errors->Add(CurrentError);
	}
}

void UWorldPartitionChangelistValidator::OnInvalidReferenceDataLayers(const FWorldPartitionActorDescView& ActorDescView, const FWorldPartitionActorDescView& ReferenceActorDescView)
{	
	if (Filter(ActorDescView) || Filter(ReferenceActorDescView))
	{
		FText CurrentError = FText::Format(LOCTEXT("DataValidation.Changelist.WorldPartition.DataLayerError", "{0} is referencing {1} but both Actors are using a different set of DataLayers."),
											FText::FromString(GetPrettyPackageName(ActorDescView)),
											FText::FromString(GetPrettyPackageName(ReferenceActorDescView)));

		Errors->Add(CurrentError);
	}
}

void UWorldPartitionChangelistValidator::OnInvalidReferenceRuntimeGrid(const FWorldPartitionActorDescView& ActorDescView, const FWorldPartitionActorDescView& ReferenceActorDescView)
{
	if (Filter(ActorDescView) || Filter(ReferenceActorDescView))
	{
		FText CurrentError = FText::Format(LOCTEXT("DataValidation.Changelist.WorldPartition.RuntimeGridError", "{0} is referencing {1} but both Actors are using a different Runtime Grid."),
			FText::FromString(GetPrettyPackageName(ActorDescView)),
			FText::FromString(GetPrettyPackageName(ReferenceActorDescView)));

		Errors->Add(CurrentError);
	}
}

void UWorldPartitionChangelistValidator::OnInvalidReferenceLevelScriptStreamed(const FWorldPartitionActorDescView& ActorDescView)
{
	if (Filter(ActorDescView))
	{		
		FText CurrentError = FText::Format(LOCTEXT("DataValidation.Changelist.WorldPartition.InvalidReferenceLevelScriptStreamed", "Level Script Blueprint references streamed actor {0}."),
											FText::FromString(GetPrettyPackageName(ActorDescView)));
		
		Errors->Add(CurrentError);
	}
}

void UWorldPartitionChangelistValidator::OnInvalidReferenceLevelScriptDataLayers(const FWorldPartitionActorDescView& ActorDescView)
{
	if (Filter(ActorDescView))
	{
		FText CurrentError = FText::Format(LOCTEXT("DataValidation.Changelist.WorldPartition.InvalidReferenceLevelScriptDataLayers", "Level Script Blueprint references streamed actor {0} with a non empty set of data layers"),
											FText::FromString(GetPrettyPackageName(ActorDescView)));

		Errors->Add(CurrentError);
	}
}



#undef LOCTEXT_NAMESPACE