﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "Archive/ApplySnapshotToEditorArchive.h"

#include "Archive/ClassDefaults/ApplyClassDefaulDataArchive.h"
#include "Data/RestorationEvents/ApplySnapshotPropertiesScope.h"
#include "Data/Util/Property/PropertyUtil.h"
#include "Data/Util/SnapshotObjectUtil.h"
#include "Data/WorldSnapshotData.h"
#include "LevelSnapshotsLog.h"
#include "Selection/PropertySelection.h"

#include "Components/ActorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Serialization/ObjectReader.h"
#include "Serialization/ObjectWriter.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"
#include "Util/Restoration/WorldDataUtil.h"

namespace UE::LevelSnapshots::Private::Internal
{
	class FCopyProperties : public FObjectWriter
	{
		using Super = FObjectWriter;

		const FPropertySelection& PropertiesToSerialize;
		UObject* SnapshotObject;

		bool IsWorldObjectProperty(const FProperty* InProperty) const
		{
			const FObjectPropertyBase* ObjectProperty = CastField<FObjectPropertyBase>(InProperty);
			if (!ObjectProperty)
			{
				return false;
			}

			const bool bIsMarkedAsSubobject = InProperty->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference | CPF_PersistentInstance);
			const bool bIsActorOrComponentPtr = ObjectProperty->PropertyClass->IsChildOf(AActor::StaticClass()) || ObjectProperty->PropertyClass->IsChildOf(UActorComponent::StaticClass());
			if (bIsMarkedAsSubobject || bIsActorOrComponentPtr)
			{
				return true;
			}

			const FArchiveSerializedPropertyChain* PropertyChain = GetSerializedPropertyChain();
			const bool bContainsWorldReference = UE::LevelSnapshots::Private::FollowPropertyChainUntilPredicateIsTrue(SnapshotObject, PropertyChain, InProperty, [this, ObjectProperty](void* LeafValuePtr)
			{
				const UObject* ContainedPtr = ObjectProperty->GetObjectPropertyValue(LeafValuePtr);
				return LeafValuePtr ? ContainedPtr->IsInA(UWorld::StaticClass()) : false;
			});
			return bContainsWorldReference;
		}
				
	public:

		mutable TSet<const FTextProperty*> TextProperties;

		FCopyProperties(UObject* SnapshotObject, TArray<uint8>& SaveLocation, const FPropertySelection& PropertiesToSerialize)
			:
			Super(SaveLocation),
			PropertiesToSerialize(PropertiesToSerialize),
			SnapshotObject(SnapshotObject)
		{
			ArNoDelta = true;
		}

		virtual bool ShouldSkipProperty(const FProperty* InProperty) const override
		{
			// Do not copy object reference properties that have a world as outer: They will not be valid when copied from snapshot to editor world.
			// Hence, we only allow object references to external assets, e.g. UMaterials or UDataAssets
			if (IsWorldObjectProperty(InProperty))
			{
				return true;
			}

			const bool bIsPropertyAllowed = PropertiesToSerialize.ShouldSerializeProperty(GetSerializedPropertyChain(), InProperty);
			if (bIsPropertyAllowed)
			{
				if (const FTextProperty* TextProperty = CastField<FTextProperty>(InProperty))
				{
					TextProperties.Add(TextProperty);
					return false;
				}
			}
			return !bIsPropertyAllowed;
		}		
	};

	class FSerializeTextProperties : public FApplyClassDefaulDataArchive
	{
		TSet<const FTextProperty*>& TextProperties;
	public:
		
		FSerializeTextProperties(TSet<const FTextProperty*>& InTextProperties, FObjectSnapshotData& InObjectData, FWorldSnapshotData& InSharedData, UObject* InSerializedObject)
			:
			FApplyClassDefaulDataArchive(InObjectData, InSharedData, InSerializedObject, ESerialisationMode::RestoringChangedDefaults),
			TextProperties(InTextProperties)
		{}

		virtual bool ShouldSkipProperty(const FProperty* InProperty) const override
		{
			if (const FTextProperty* TextProperty = CastField<FTextProperty>(InProperty))
			{
				return !TextProperties.Contains(TextProperty);
			}
			return true;
		}
	};

	static TSet<const FTextProperty*> CopyPastePropertiesDifferentInCDO(const FPropertySelection& PropertiesLeftToSerialise, UObject* InOriginalObject, UObject* InDeserializedVersion)
	{
		if (!PropertiesLeftToSerialise.IsEmpty())
		{
			TArray<uint8> CopiedPropertyData;
			FCopyProperties CopySimpleProperties(InDeserializedVersion, CopiedPropertyData, PropertiesLeftToSerialise);
			InDeserializedVersion->Serialize(CopySimpleProperties);
		
			FObjectReader PasteSimpleProperties(InOriginalObject, CopiedPropertyData);
			return CopySimpleProperties.TextProperties;
		}
		return {};
	}

	static void FixUpTextPropertiesDifferentInCDO(TSet<const FTextProperty*> TextProperties, FWorldSnapshotData& InSharedData, UObject* InOriginalObject)
	{
		const bool bAreTextPropertiesLeft = TextProperties.Num() > 0;
		if (bAreTextPropertiesLeft)
		{
			FObjectSnapshotData* ClassDefaults = GetSerializedClassDefaults(InSharedData, InOriginalObject->GetClass()).Get(nullptr);
			if (ClassDefaults)
			{
				FSerializeTextProperties SerializeTextProperties(TextProperties, *ClassDefaults, InSharedData, InOriginalObject);
				InOriginalObject->Serialize(SerializeTextProperties);
			}
			else
			{
				UE_LOG(LogLevelSnapshots, Warning, TEXT("%d text properties have changed in class defaults since snapshot was taken but cannot be restored."), TextProperties.Num());
			}
		}
	}
}

void UE::LevelSnapshots::Private::FApplySnapshotToEditorArchive::ApplyToExistingEditorWorldObject(
	FObjectSnapshotData& InObjectData,
	FWorldSnapshotData& InSharedData,
	FSnapshotDataCache& Cache,
	UObject* InOriginalObject,
	UObject* InDeserializedVersion,
	const FPropertySelectionMap& InSelectionMapForResolvingSubobjects)
{
	const FPropertySelection* Selection = InSelectionMapForResolvingSubobjects.GetObjectSelection(InOriginalObject).GetPropertySelection();
	if (Selection && Selection->IsEmpty())
	{
		return;
	}

	UE_LOG(LogLevelSnapshots, Verbose, TEXT("Applying to existing object %s (class %s)"), *InOriginalObject->GetPathName(), *InOriginalObject->GetClass()->GetPathName());
	const FApplySnapshotPropertiesScope NotifySnapshotListeners({ InOriginalObject, InSelectionMapForResolvingSubobjects, Selection, true });
#if WITH_EDITOR
	InOriginalObject->Modify();
#endif
	
	// Step 1: Serialise  properties that were different from CDO at time of snapshotting and that are still different from CDO
	FApplySnapshotToEditorArchive ApplySavedData(InObjectData, InSharedData, InOriginalObject, InSelectionMapForResolvingSubobjects, Selection, Cache);
	InOriginalObject->Serialize(ApplySavedData);
	
	// Step 2: Serialise any remaining properties that were not covered: properties that were equal to the CDO value when the snapshot was taken but now are different from the CDO.
	TSet<const FTextProperty*> TextProperties = Internal::CopyPastePropertiesDifferentInCDO(ApplySavedData.PropertiesLeftToSerialize, InOriginalObject, InDeserializedVersion);

	// Step 3: Serialize FText properties that have changed in CDO since snapshot was taken. 
	Internal::FixUpTextPropertiesDifferentInCDO(MoveTemp(TextProperties), InSharedData, InOriginalObject);
}

void UE::LevelSnapshots::Private::FApplySnapshotToEditorArchive::ApplyToRecreatedEditorWorldObject(
	FObjectSnapshotData& InObjectData,
	FWorldSnapshotData& InSharedData,
	FSnapshotDataCache& Cache,
	UObject* InOriginalObject,
	const FPropertySelectionMap& InSelectionMapForResolvingSubobjects)
{
	UE_LOG(LogLevelSnapshots, Verbose, TEXT("Applying to recreated object %s (class %s)"), *InOriginalObject->GetPathName(), *InOriginalObject->GetClass()->GetPathName());
	const FApplySnapshotPropertiesScope NotifySnapshotListeners({ InOriginalObject, InSelectionMapForResolvingSubobjects, {}, true });
	
	// Apply all properties that we saved into the target actor.
	// We assume that InOriginalObject was already created with the snapshot CDO as template: we do not need Step 2 from ApplyToExistingWorldObject.
	FApplySnapshotToEditorArchive ApplySavedData(InObjectData, InSharedData, InOriginalObject, InSelectionMapForResolvingSubobjects, {}, Cache);
	InOriginalObject->Serialize(ApplySavedData);
}

bool UE::LevelSnapshots::Private::FApplySnapshotToEditorArchive::ShouldSkipProperty(const FProperty* InProperty) const
{
	SCOPED_SNAPSHOT_CORE_TRACE(ShouldSkipProperty);
	
	bool bShouldSkipProperty = Super::ShouldSkipProperty(InProperty);
	
	if (!bShouldSkipProperty && !ShouldSerializeAllProperties())
	{
		const bool bIsAllowed = SelectionSet.GetValue()->ShouldSerializeProperty(GetSerializedPropertyChain(), InProperty);  
		bShouldSkipProperty = !bIsAllowed;
	}
	
	return bShouldSkipProperty;
}

void UE::LevelSnapshots::Private::FApplySnapshotToEditorArchive::PushSerializedProperty(FProperty* InProperty, const bool bIsEditorOnlyProperty)
{
	// Do before call to super because super appends InProperty to property chain
	PropertiesLeftToSerialize.RemoveProperty(GetSerializedPropertyChain(), InProperty);
	
	Super::PushSerializedProperty(InProperty, bIsEditorOnlyProperty);
}

UObject* UE::LevelSnapshots::Private::FApplySnapshotToEditorArchive::ResolveObjectDependency(int32 ObjectIndex) const
{
	FString LocalizationNamespace;
#if USE_STABLE_LOCALIZATION_KEYS
	LocalizationNamespace = GetLocalizationNamespace();
#endif
	return UE::LevelSnapshots::Private::ResolveObjectDependencyForEditorWorld(GetSharedData(), Cache, ObjectIndex, LocalizationNamespace, SelectionMapForResolvingSubobjects);
}

UE::LevelSnapshots::Private::FApplySnapshotToEditorArchive::FApplySnapshotToEditorArchive(
	FObjectSnapshotData& InObjectData,
	FWorldSnapshotData& InSharedData,
	UObject* InOriginalObject,
	const FPropertySelectionMap& InSelectionMapForResolvingSubobjects,
	TOptional<const FPropertySelection*> InSelectionSet,
	FSnapshotDataCache& Cache)
        : Super(InObjectData, InSharedData, true, InOriginalObject)
		, SelectionMapForResolvingSubobjects(InSelectionMapForResolvingSubobjects)
		, SelectionSet(InSelectionSet)
		, Cache(Cache)
{
	if (SelectionSet)
	{
		PropertiesLeftToSerialize = *SelectionSet.GetValue();
	}
}

bool UE::LevelSnapshots::Private::FApplySnapshotToEditorArchive::ShouldSerializeAllProperties() const
{
	return !SelectionSet.IsSet();
}
