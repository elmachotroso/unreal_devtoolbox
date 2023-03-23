// Copyright Epic Games, Inc. All Rights Reserved.

#include "SmartObjectCollection.h"
#include "SmartObjectTypes.h"
#include "SmartObjectSubsystem.h"
#include "SmartObjectComponent.h"
#include "Engine/World.h"
#include "VisualLogger/VisualLogger.h"

//----------------------------------------------------------------------//
// FSmartObjectCollectionEntry 
//----------------------------------------------------------------------//
FSmartObjectCollectionEntry::FSmartObjectCollectionEntry(const FSmartObjectHandle& SmartObjectHandle, const USmartObjectComponent& SmartObjectComponent, const uint32 DefinitionIndex)
	: Handle(SmartObjectHandle)
	, Path(&SmartObjectComponent)
	, Transform(SmartObjectComponent.GetComponentTransform())
	, Bounds(SmartObjectComponent.GetSmartObjectBounds())
	, DefinitionIdx(DefinitionIndex)
{
}

USmartObjectComponent* FSmartObjectCollectionEntry::GetComponent() const
{
	return CastChecked<USmartObjectComponent>(Path.ResolveObject(), ECastCheckedType::NullAllowed);
}


//----------------------------------------------------------------------//
// ASmartObjectCollection 
//----------------------------------------------------------------------//
ASmartObjectCollection::ASmartObjectCollection(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
#if WITH_EDITORONLY_DATA
	bLockLocation = true;
	bActorLabelEditable = false;
#endif

	PrimaryActorTick.bCanEverTick = false;
	bNetLoadOnClient = false;
	SetCanBeDamaged(false);
}

void ASmartObjectCollection::Destroyed()
{
	// Handle editor delete.
	UnregisterWithSubsystem(ANSI_TO_TCHAR(__FUNCTION__));
	Super::Destroyed();
}

void ASmartObjectCollection::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// Handle Level unload, PIE end, SIE end, game end.
	UnregisterWithSubsystem(ANSI_TO_TCHAR(__FUNCTION__));
	Super::EndPlay(EndPlayReason);
}

void ASmartObjectCollection::PostActorCreated()
{
	// Register after being initially spawned.
	Super::PostActorCreated();
	RegisterWithSubsystem(ANSI_TO_TCHAR(__FUNCTION__));
}

void ASmartObjectCollection::PreRegisterAllComponents()
{
	Super::PreRegisterAllComponents();

	// Handle UWorld::AddToWorld(), i.e. turning on level visibility
	if (const ULevel* Level = GetLevel())
	{
		// This function gets called in editor all the time, we're only interested the case where level is being added to world.
		if (Level->bIsAssociatingLevel)
		{
			RegisterWithSubsystem(ANSI_TO_TCHAR(__FUNCTION__));
		}
	}
}

void ASmartObjectCollection::PostUnregisterAllComponents()
{
	// Handle UWorld::RemoveFromWorld(), i.e. turning off level visibility
	if (const ULevel* Level = GetLevel())
	{
		// This function gets called in editor all the time, we're only interested the case where level is being removed from world.
		if (Level->bIsDisassociatingLevel)
		{
			UnregisterWithSubsystem(ANSI_TO_TCHAR(__FUNCTION__));
		}
	}

	Super::PostUnregisterAllComponents();
}

bool ASmartObjectCollection::RegisterWithSubsystem(const FString& Context)
{
	if (bRegistered)
	{
		UE_VLOG_UELOG(this, LogSmartObject, Log, TEXT("'%s' %s - Failed: already registered"), *GetFullName(), *Context);
		return false;
	}

	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		UE_VLOG_UELOG(this, LogSmartObject, Log, TEXT("'%s' %s - Failed: ignoring default object"), *GetFullName(), *Context);
		return false;
	}

	USmartObjectSubsystem* SmartObjectSubsystem = USmartObjectSubsystem::GetCurrent(GetWorld());
	if (SmartObjectSubsystem == nullptr)
	{
		// Collection might attempt to register before the subsystem is created. At its initialization the subsystem gathers
		// all collections and registers them. For this reason we use a log instead of an error.
		UE_VLOG_UELOG(this, LogSmartObject, Log, TEXT("'%s' %s - Failed: unable to find smart object subsystem"), *GetFullName(), *Context);
		return false;
	}

	const ESmartObjectCollectionRegistrationResult Result = SmartObjectSubsystem->RegisterCollection(*this);
	UE_VLOG_UELOG(this, LogSmartObject, Log, TEXT("'%s' %s - %s"), *GetFullName(), *Context, *UEnum::GetValueAsString(Result));
	return true;
}

bool ASmartObjectCollection::UnregisterWithSubsystem(const FString& Context)
{
	if (!bRegistered)
	{
		UE_VLOG_UELOG(this, LogSmartObject, Log, TEXT("'%s' %s - Failed: not registered"), *GetFullName(), *Context);
		return false;
	}

	USmartObjectSubsystem* SmartObjectSubsystem = USmartObjectSubsystem::GetCurrent(GetWorld());
	if (SmartObjectSubsystem == nullptr)
	{
		UE_VLOG_UELOG(this, LogSmartObject, Log, TEXT("'%s' %s - Failed: unable to find smart object subsystem"), *GetFullName(), *Context);
		return false;
	}

	SmartObjectSubsystem->UnregisterCollection(*this);
	UE_VLOG_UELOG(this, LogSmartObject, Log, TEXT("'%s' %s - Succeeded"), *GetFullName(), *Context);
	return true;
}

bool ASmartObjectCollection::AddSmartObject(USmartObjectComponent& SOComponent)
{
	const UWorld* World = GetWorld();
	if (World == nullptr)
	{
		UE_VLOG_UELOG(this, LogSmartObject, Error, TEXT("'%s' can't be registered to collection '%s': no associated world"),
			*GetNameSafe(SOComponent.GetOwner()), *GetFullName());
		return false;
	}

	const FSoftObjectPath ObjectPath = &SOComponent;
	FString AssetPathString = ObjectPath.GetAssetPathString();

	// We are not using asset path for partitioned world since they are not stable between editor and runtime.
	// SubPathString should be enough since all actors are part of the main level.
	if (World->IsPartitionedWorld())
	{
		AssetPathString.Reset();
	}
#if WITH_EDITOR
	else if (World->WorldType == EWorldType::PIE)
	{
		AssetPathString = UWorld::RemovePIEPrefix(ObjectPath.GetAssetPathString());
	}
#endif // WITH_EDITOR

	// Compute hash manually from strings since GetTypeHash(FSoftObjectPath) relies on a FName which implements run-dependent hash computations.
	FSmartObjectHandle Handle = FSmartObjectHandle(HashCombine(GetTypeHash(AssetPathString), GetTypeHash(ObjectPath.GetSubPathString())));
	SOComponent.SetRegisteredHandle(Handle);

	const FSmartObjectCollectionEntry* ExistingEntry = CollectionEntries.FindByPredicate([Handle](const FSmartObjectCollectionEntry& Entry)
	{
		return Entry.Handle == Handle;
	});

	if (ExistingEntry != nullptr)
	{
		UE_VLOG_UELOG(this, LogSmartObject, VeryVerbose, TEXT("'%s[%s]' already registered to collection '%s'"),
			*GetNameSafe(SOComponent.GetOwner()), *LexToString(Handle), *GetFullName());
		return false;
	}

	const USmartObjectDefinition* Definition = SOComponent.GetDefinition();
	ensureMsgf(Definition != nullptr, TEXT("Shouldn't reach this point with an invalid definition asset"));
	uint32 DefinitionIndex = Definitions.AddUnique(Definition);

	UE_VLOG_UELOG(this, LogSmartObject, Verbose, TEXT("Adding '%s[%s]' to collection '%s'"), *GetNameSafe(SOComponent.GetOwner()), *LexToString(Handle), *GetFullName());
	CollectionEntries.Emplace(Handle, SOComponent, DefinitionIndex);
	RegisteredIdToObjectMap.Add(Handle, ObjectPath);
	return true;
}

bool ASmartObjectCollection::RemoveSmartObject(USmartObjectComponent& SOComponent)
{
	FSmartObjectHandle Handle = SOComponent.GetRegisteredHandle();
	if (!Handle.IsValid())
	{
		return false;
	}

	UE_VLOG_UELOG(this, LogSmartObject, Verbose, TEXT("Removing '%s[%s]' from collection '%s'"), *GetNameSafe(SOComponent.GetOwner()), *LexToString(Handle), *GetFullName());
	const int32 Index = CollectionEntries.IndexOfByPredicate(
		[&Handle](const FSmartObjectCollectionEntry& Entry)
		{
			return Entry.GetHandle() == Handle;
		});

	if (Index != INDEX_NONE)
	{
		CollectionEntries.RemoveAt(Index);
		RegisteredIdToObjectMap.Remove(Handle);
	}

	SOComponent.SetRegisteredHandle(FSmartObjectHandle::Invalid);

	return Index != INDEX_NONE;
}

USmartObjectComponent* ASmartObjectCollection::GetSmartObjectComponent(const FSmartObjectHandle& SmartObjectHandle) const
{
	const FSoftObjectPath* Path = RegisteredIdToObjectMap.Find(SmartObjectHandle);
	return Path != nullptr ? CastChecked<USmartObjectComponent>(Path->ResolveObject(), ECastCheckedType::NullAllowed) : nullptr;
}

const USmartObjectDefinition* ASmartObjectCollection::GetDefinitionForEntry(const FSmartObjectCollectionEntry& Entry) const
{
	const bool bIsValidIndex = Definitions.IsValidIndex(Entry.GetDefinitionIndex());
	if (!bIsValidIndex)
	{
		UE_VLOG_UELOG(this, LogSmartObject, Error, TEXT("Using invalid index (%d) to retrieve definition from collection '%s'"), Entry.GetDefinitionIndex(), *GetFullName());
		return nullptr;
	}

	const USmartObjectDefinition* Definition = Definitions[Entry.GetDefinitionIndex()];
	ensureMsgf(Definition != nullptr, TEXT("Collection is expected to contain only valid definition entries"));
	return Definition;
}

void ASmartObjectCollection::OnRegistered()
{
	bRegistered = true;
}

void ASmartObjectCollection::OnUnregistered()
{
	bRegistered = false;
}

void ASmartObjectCollection::ValidateDefinitions()
{
	for (const USmartObjectDefinition* Definition : Definitions)
	{
		if (ensureMsgf(Definition != nullptr, TEXT("Collection is expected to contain only valid definition entries")))
		{
			Definition->Validate();
		}
	}
}

#if WITH_EDITOR
void ASmartObjectCollection::PostEditUndo()
{
	Super::PostEditUndo();

	if (IsPendingKillPending())
	{
		UnregisterWithSubsystem(ANSI_TO_TCHAR(__FUNCTION__));
	}
	else
	{
		RegisterWithSubsystem(ANSI_TO_TCHAR(__FUNCTION__));
	}
}

void ASmartObjectCollection::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property)
	{
		const FName PropName = PropertyChangedEvent.Property->GetFName();
		if (PropName == GET_MEMBER_NAME_CHECKED(ASmartObjectCollection, bBuildOnDemand))
		{
			if (!bBuildOnDemand)
			{
				RebuildCollection();
			}
		}
	}
}

void ASmartObjectCollection::RebuildCollection()
{
	if (USmartObjectSubsystem* SmartObjectSubsystem = USmartObjectSubsystem::GetCurrent(GetWorld()))
	{
		SmartObjectSubsystem->RebuildCollection(*this);

		// Dirty package since this is an explicit user action
		MarkPackageDirty();
	}
}

void ASmartObjectCollection::RebuildCollection(const TConstArrayView<USmartObjectComponent*> Components)
{
	UE_VLOG_UELOG(this, LogSmartObject, Log, TEXT("Rebuilding collection '%s' from component list"), *GetFullName());

	ResetCollection(Components.Num());

	for (USmartObjectComponent* const Component : Components)
	{
		if (Component != nullptr)
		{
			AddSmartObject(*Component);
		}
	}

	CollectionEntries.Shrink();
	RegisteredIdToObjectMap.Shrink();
	Definitions.Shrink();
}

void ASmartObjectCollection::ResetCollection(const int32 ExpectedNumElements)
{
	UE_VLOG_UELOG(this, LogSmartObject, Log, TEXT("Reseting collection '%s'"), *GetFullName());

	CollectionEntries.Reset(ExpectedNumElements);
	RegisteredIdToObjectMap.Empty(ExpectedNumElements);
	Definitions.Reset();
}
#endif // WITH_EDITOR
