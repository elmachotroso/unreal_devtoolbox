// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Subsystems/WorldSubsystem.h"
#include "MassEntityTypes.h"
#include "MassProcessingTypes.h"
#include "InstancedStruct.h"
#include "MassEntityQuery.h"
#include "StructUtilsTypes.h"
#include "MassObserverManager.h"
#include "Containers/Queue.h"
#include "MassEntitySubsystem.generated.h"


class UMassEntitySubsystem;
struct FMassEntityQuery;
struct FMassExecutionContext;
struct FMassArchetypeData;
struct FMassCommandBuffer;
struct FMassArchetypeSubChunks;
class FOutputDevice;
enum class EMassFragmentAccess : uint8;

//@TODO: Comment this guy
UCLASS()
class MASSENTITY_API UMassEntitySubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

	friend struct FMassEntityQuery;
private:
	// Index 0 is reserved so we can treat that index as an invalid entity handle
	constexpr static int32 NumReservedEntities = 1;

	struct FEntityData
	{
		TSharedPtr<FMassArchetypeData> CurrentArchetype;
		int32 SerialNumber = 0;

		void Reset()
		{
			CurrentArchetype.Reset();
			SerialNumber = 0;
		}

		bool IsValid() const
		{
			return SerialNumber != 0 && CurrentArchetype.IsValid();
		}
	};
	
public:
	struct FScopedProcessing
	{
		explicit FScopedProcessing(std::atomic<int32>& InProcessingScopeCount) : ScopedProcessingCount(InProcessingScopeCount)
		{
			++ScopedProcessingCount;
		}
		~FScopedProcessing()
		{
			--ScopedProcessingCount;
		}
	private:
		std::atomic<int32>& ScopedProcessingCount;
	};

	const static FMassEntityHandle InvalidEntity;

	UMassEntitySubsystem();

	//~UObject interface
	virtual void GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize) override;
	//~End of UObject interface

	//~USubsystem interface
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void PostInitialize() override;
	virtual void Deinitialize() override;
	//~End of USubsystem interface
	
	/** 
	 * A special, relaxed but slower version of CreateArchetype functions that allows FragmentAngTagsList to contain 
	 * both fragments and tags. 
	 */
	FMassArchetypeHandle CreateArchetype(TConstArrayView<const UScriptStruct*> FragmentsAndTagsList);

	/**
	 * CreateArchetype from a composition descriptor and initial values
	 *
	 * @param Composition of fragment, tag and chunk fragment types
	 * @param SharedFragmentValues are the actual pointer to shared fragments
	 * @return a handle of a new archetype 
	 */
	FMassArchetypeHandle CreateArchetype(const FMassArchetypeCompositionDescriptor& Composition, const FMassArchetypeSharedFragmentValues& SharedFragmentValues);

	/** 
	 *  Creates an archetype like SourceArchetype + NewFragmentList. 
	 *  @param SourceArchetype the archetype used to initially populate the list of fragments of the archetype being created. 
	 *  @param NewFragmentList list of unique fragments to add to fragments fetched from SourceArchetype. Note that 
	 *   adding an empty list is not supported and doing so will result in failing a `check`
	 *  @return a handle of a new archetype
	 *  @note it's caller's responsibility to ensure that NewFragmentList is not empty and contains only fragment
	 *   types that SourceArchetype doesn't already have. If the caller cannot guarantee it use of AddFragment functions
	 *   family is recommended.
	 */
	FMassArchetypeHandle CreateArchetype(const TSharedPtr<FMassArchetypeData>& SourceArchetype, const FMassFragmentBitSet& NewFragmentList);

	FMassArchetypeHandle GetArchetypeForEntity(FMassEntityHandle Entity) const;
	/** Method to iterate on all the fragment types of an archetype */
	static void ForEachArchetypeFragmentType(const FMassArchetypeHandle Archetype, TFunction< void(const UScriptStruct* /*FragmentType*/)> Function);

	/**
	 * Go through all archetypes and compact entities
	 * @param TimeAllowed to do entity compaction, once it reach that time it will stop and return
	 */
	void DoEntityCompaction(const double TimeAllowed);

	/**
	 * Creates fully built entity ready to be used by the subsystem
	 * @param Archetype you want this entity to be
	 * @return FMassEntityHandle id of the newly created entity */
	FMassEntityHandle CreateEntity(const FMassArchetypeHandle Archetype);

	/**
	 * Creates fully built entity ready to be used by the subsystem
	 * @param FragmentInstanceList is the fragments to create the entity from and initialize values
	 * @return FMassEntityHandle id of the newly created entity */
	FMassEntityHandle CreateEntity(TConstArrayView<FInstancedStruct> FragmentInstanceList);

	/**
	 * A dedicated structure for ensuring the "on entities creation" observers get notified only once all other 
	 * initialization operations are done and this creation context instance gets released. */
	struct FEntityCreationContext
	{
		explicit FEntityCreationContext(const int32 InNumSpawned = 0)
			: NumberSpawned(InNumSpawned)
		{}
		~FEntityCreationContext() { if (OnSpawningFinished) OnSpawningFinished(*this); }

		const FMassArchetypeSubChunks& GetChunkCollection() const { return ChunkCollection; }
	private:
		friend UMassEntitySubsystem;
		int32 NumberSpawned;
		FMassArchetypeSubChunks ChunkCollection;
		TFunction<void(FEntityCreationContext&)> OnSpawningFinished;
	};

	/** A version of CreateEntity that's creating a number of entities (Count) at one go
	 *  @param Archetype you want this entity to be
	 *  @param Count number of entities to create
	 *  @param OutEntities the newly created entities are appended to given array, i.e. the pre-existing content of OutEntities won't be affected by the call
	 *  @return a creation context that will notify all the interested observers about newly created fragments once the context is released */
	TSharedRef<FEntityCreationContext> BatchCreateEntities(const FMassArchetypeHandle Archetype, const int32 Count, TArray<FMassEntityHandle>& OutEntities);

	/**
	 * Destroys a fully built entity, use ReleaseReservedEntity if entity was not yet built.
	 * @param Entity to destroy */
	void DestroyEntity(FMassEntityHandle Entity);

	/**
	 * Reserves an entity in the subsystem, the entity is still not ready to be used by the subsystem, need to call BuildEntity()
	 * @return FMassEntityHandle id of the reserved entity */
	FMassEntityHandle ReserveEntity();

	/**
	 * Builds an entity for it to be ready to be used by the subsystem
	 * @param Entity to build which was retrieved with ReserveEntity() method
	 * @param Archetype you want this entity to be*/
	void BuildEntity(FMassEntityHandle Entity, const FMassArchetypeHandle Archetype);

	/**
	 * Builds an entity for it to be ready to be used by the subsystem
	 * @param Entity to build which was retrieved with ReserveEntity() method
	 * @param FragmentInstanceList is the fragments to create the entity from and initialize values*/
	void BuildEntity(FMassEntityHandle Entity, TConstArrayView<FInstancedStruct> FragmentInstanceList, FMassArchetypeSharedFragmentValues SharedFragmentValues = {});

	/*
	 * Releases a previously reserved entity that was not yet built, otherwise call DestroyEntity
	 * @param Entity to release */
	void ReleaseReservedEntity(FMassEntityHandle Entity);

	/**
	 * Destroys all the entity in the provided array of entities
	 * @param InEntities to destroy
	 */
	void BatchDestroyEntities(TConstArrayView<FMassEntityHandle> InEntities);

	void BatchDestroyEntityChunks(const FMassArchetypeSubChunks& Chunks);

	void AddFragmentToEntity(FMassEntityHandle Entity, const UScriptStruct* FragmentType);

	/** 
	 *  Ensures that only unique fragments are added. 
	 *  @note It's caller's responsibility to ensure Entity's and FragmentList's validity. 
	 */
	void AddFragmentListToEntity(FMassEntityHandle Entity, TConstArrayView<const UScriptStruct*> FragmentList);

	void AddFragmentInstanceListToEntity(FMassEntityHandle Entity, TConstArrayView<FInstancedStruct> FragmentInstanceList);
	void RemoveFragmentFromEntity(FMassEntityHandle Entity, const UScriptStruct* FragmentType);
	void RemoveFragmentListFromEntity(FMassEntityHandle Entity, TConstArrayView<const UScriptStruct*> FragmentList);

	void AddTagToEntity(FMassEntityHandle Entity, const UScriptStruct* TagType);
	void RemoveTagFromEntity(FMassEntityHandle Entity, const UScriptStruct* TagType);
	void SwapTagsForEntity(FMassEntityHandle Entity, const UScriptStruct* FromFragmentType, const UScriptStruct* ToFragmentType);


	/**
	 * Adds fragments and tags indicated by InOutDescriptor to the Entity. The function also figures out which elements
	 * in InOutDescriptor are missing from the current composition of the given entity and then returns the resulting 
	 * delta via InOutDescriptor.
	 */
	void AddCompositionToEntity_GetDelta(FMassEntityHandle Entity, FMassArchetypeCompositionDescriptor& InOutDescriptor);
	void RemoveCompositionFromEntity(FMassEntityHandle Entity, const FMassArchetypeCompositionDescriptor& InDescriptor);

	const FMassArchetypeCompositionDescriptor& GetArchetypeComposition(const FMassArchetypeHandle& ArchetypeHandle) const;

	/** 
	 * Moves an entity over to a new archetype by copying over fragments common to both archetypes
	 * @param Entity is the entity to move 
	 * @param NewArchetypeHandle the handle to the new archetype
	 */
	void MoveEntityToAnotherArchetype(FMassEntityHandle Entity, FMassArchetypeHandle NewArchetypeHandle);

	/** Copies values from FragmentInstanceList over to Entity's fragment. Caller is responsible for ensuring that 
	 *  the given entity does have given fragments. Failing this assumption will cause a check-fail.*/
	void SetEntityFragmentsValues(FMassEntityHandle Entity, TArrayView<const FInstancedStruct> FragmentInstanceList);

	/** Copies values from FragmentInstanceList over to fragments of given entities collection. The caller is responsible 
	 *  for ensuring that the given entity archetype (FMassArchetypeSubChunks .Archetype) does have given fragments. 
	 *  Failing this assumption will cause a check-fail. */
	static void BatchSetEntityFragmentsValues(const FMassArchetypeSubChunks& SparseEntities, TArrayView<const FInstancedStruct> FragmentInstanceList);

	// Return true if it is an valid built entity
	bool IsEntityActive(FMassEntityHandle Entity) const 
	{
		return IsEntityValid(Entity) && IsEntityBuilt(Entity);
	}

	// Returns true if Entity is valid
	bool IsEntityValid(FMassEntityHandle Entity) const;

	// Returns true if Entity is has been fully built (expecting a valid Entity)
	bool IsEntityBuilt(FMassEntityHandle Entity) const;

	// Asserts that IsEntityValid
	void CheckIfEntityIsValid(FMassEntityHandle Entity) const;

	// Asserts that IsEntityBuilt
	void CheckIfEntityIsActive(FMassEntityHandle Entity) const;

	template <typename FragmentType>
	FragmentType& GetFragmentDataChecked(FMassEntityHandle Entity) const
	{
		return *((FragmentType*)InternalGetFragmentDataChecked(Entity, FragmentType::StaticStruct()));
	}

	template <typename FragmentType>
	FragmentType* GetFragmentDataPtr(FMassEntityHandle Entity) const
	{
		return (FragmentType*)InternalGetFragmentDataPtr(Entity, FragmentType::StaticStruct());
	}

	FStructView GetFragmentDataStruct(FMassEntityHandle Entity, const UScriptStruct* FragmentType) const
	{
		return FStructView(FragmentType, static_cast<uint8*>(InternalGetFragmentDataPtr(Entity, FragmentType)));
	}

	uint32 GetArchetypeDataVersion() const { return ArchetypeDataVersion; }

	/**
	 * Creates and initializes a FMassExecutionContext instance.
	 */
	FMassExecutionContext CreateExecutionContext(const float DeltaSeconds) const;

	FScopedProcessing NewProcessingScope() { return FScopedProcessing(ProcessingScopeCount); }
	bool IsProcessing() const { return ProcessingScopeCount > 0; }
	FMassCommandBuffer& Defer() const { return *DeferredCommandBuffer.Get(); }
	/** 
	 * @param InCommandBuffer if not set then the default command buffer will be flushed. If set and there's already 
	 *		a command buffer being flushed (be it the main one or a previously requested one) then this command buffer 
	 *		will be queue itself.
	 */
	void FlushCommands(const TSharedPtr<FMassCommandBuffer>& InCommandBuffer = TSharedPtr<FMassCommandBuffer>());

	/**
	 * Shared fragment creation methods
	 */
	template<typename T>
	FConstSharedStruct& GetOrCreateConstSharedFragment(const uint32 Hash, const T& Fragment)
	{
		static_assert(TIsDerivedFrom<T, FMassSharedFragment>::IsDerived, "Given struct doesn't represent a valid shared fragment type. Make sure to inherit from FMassSharedFragment or one of its child-types.");
		int32& Index = ConstSharedFragmentsMap.FindOrAddByHash(Hash, Hash, INDEX_NONE);
		if (Index == INDEX_NONE)
		{
			Index = ConstSharedFragments.Add(FSharedStruct::Make(Fragment));
		}
		return ConstSharedFragments[Index];
	}

	template<typename T, typename... TArgs>
	FSharedStruct& GetOrCreateSharedFragment(const uint32 Hash, TArgs&&... InArgs)
	{
		static_assert(TIsDerivedFrom<T, FMassSharedFragment>::IsDerived, "Given struct doesn't represent a valid shared fragment type. Make sure to inherit from FMassSharedFragment or one of its child-types.");

		int32& Index = SharedFragmentsMap.FindOrAddByHash(Hash, Hash, INDEX_NONE);
		if (Index == INDEX_NONE)
		{
			Index = SharedFragments.Add(FSharedStruct::Make<T>(Forward<TArgs>(InArgs)...));
		}

		return SharedFragments[Index];
	}

	template<typename T>
	void ForEachSharedFragment(TFunction< void(T& /*SharedFragment*/) > ExecuteFunction)
	{
		FStructTypeEqualOperator Predicate(T::StaticStruct());
		for (const FSharedStruct& Struct : SharedFragments)
		{
			if (Predicate(Struct))
			{
				ExecuteFunction(Struct.GetMutable<T>());
			}
		}
	}

	FMassObserverManager& GetObserverManager() { return ObserverManager; }

#if WITH_MASSENTITY_DEBUG
	void DebugPrintEntity(int32 Index, FOutputDevice& Ar, const TCHAR* InPrefix = TEXT("")) const;
	void DebugPrintEntity(FMassEntityHandle Entity, FOutputDevice& Ar, const TCHAR* InPrefix = TEXT("")) const;
	void DebugPrintArchetypes(FOutputDevice& Ar, const bool bIncludeEmpty = true) const;
	static void DebugGetStringDesc(const FMassArchetypeHandle& Archetype, FOutputDevice& Ar);
	void DebugGetArchetypesStringDetails(FOutputDevice& Ar, const bool bIncludeEmpty = true);
	void DebugGetArchetypeFragmentTypes(const FMassArchetypeHandle& Archetype, TArray<const UScriptStruct*>& InOutFragmentList) const;
	int32 DebugGetArchetypeEntitiesCount(const FMassArchetypeHandle& Archetype) const;
	int32 DebugGetArchetypeEntitiesCountPerChunk(const FMassArchetypeHandle& Archetype) const;
	int32 DebugGetEntityCount() const { return Entities.Num() - NumReservedEntities - EntityFreeIndexList.Num(); }
	int32 DebugGetArchetypesCount() const { return FragmentHashToArchetypeMap.Num(); }
	void DebugRemoveAllEntities();
	void DebugForceArchetypeDataVersionBump() { ++ArchetypeDataVersion; }
	void DebugGetArchetypeStrings(const FMassArchetypeHandle& Archetype, TArray<FName>& OutFragmentNames, TArray<FName>& OutTagNames);
	FMassEntityHandle DebugGetEntityIndexHandle(const int32 EntityIndex) const { return Entities.IsValidIndex(EntityIndex) ? FMassEntityHandle(EntityIndex, Entities[EntityIndex].SerialNumber) : FMassEntityHandle(); }
#endif // WITH_MASSENTITY_DEBUG

protected:
	void GetValidArchetypes(const FMassEntityQuery& Query, TArray<FMassArchetypeHandle>& OutValidArchetypes);
	
	FMassArchetypeHandle InternalCreateSiblingArchetype(const TSharedPtr<FMassArchetypeData>& SourceArchetype, const FMassTagBitSet& OverrideTags);

private:
	void InternalBuildEntity(FMassEntityHandle Entity, const FMassArchetypeHandle Archetype);
	void InternalReleaseEntity(FMassEntityHandle Entity);

	/** 
	 *  Adds fragments in FragmentList to Entity. Only the unique fragments will be added.
	 */
	void InternalAddFragmentListToEntityChecked(FMassEntityHandle Entity, const FMassFragmentBitSet& InFragments);

	/** 
	 *  Similar to InternalAddFragmentListToEntity but expects NewFragmentList not overlapping with current entity's
	 *  fragment list. It's callers responsibility to ensure that's true. Failing this will cause a `check` fail.
	 */
	void InternalAddFragmentListToEntity(FMassEntityHandle Entity, const FMassFragmentBitSet& NewFragments);
	void* InternalGetFragmentDataChecked(FMassEntityHandle Entity, const UScriptStruct* FragmentType) const;
	void* InternalGetFragmentDataPtr(FMassEntityHandle Entity, const UScriptStruct* FragmentType) const;

private:
	TChunkedArray<FEntityData> Entities;
	TArray<int32> EntityFreeIndexList;

	std::atomic<bool> bCommandBufferFlushingInProgress = false;
	TSharedPtr<FMassCommandBuffer> DeferredCommandBuffer;
	TQueue<TSharedPtr<FMassCommandBuffer>, EQueueMode::Mpsc> FlushedCommandBufferQueue;

	std::atomic<int32> SerialNumberGenerator;
	std::atomic<int32> ProcessingScopeCount;

	// the "version" number increased every time an archetype gets added
	uint32 ArchetypeDataVersion = 0;

	// Map of hash of sorted fragment list to archetypes with that hash
	TMap<uint32, TArray<TSharedPtr<FMassArchetypeData>>> FragmentHashToArchetypeMap;

	// Map to list of archetypes that contain the specified fragment type
	TMap<const UScriptStruct*, TArray<TSharedPtr<FMassArchetypeData>>> FragmentTypeToArchetypeMap;

	// Shared fragments
	UPROPERTY(Transient)
	TArray<FConstSharedStruct> ConstSharedFragments;
	// Hash/Index in array pair
	TMap<uint32, int32> ConstSharedFragmentsMap;

	UPROPERTY(Transient)
	TArray<FSharedStruct> SharedFragments;
	// Hash/Index in array pair
	TMap<uint32, int32> SharedFragmentsMap;

	UPROPERTY(Transient)
	FMassObserverManager ObserverManager;
};


//////////////////////////////////////////////////////////////////////
//

struct MASSENTITY_API FMassExecutionContext
{
private:

	template< typename ViewType >
	struct TFragmentView 
	{
		FMassFragmentRequirement Requirement;
		ViewType FragmentView;

		TFragmentView() {}
		explicit TFragmentView(const FMassFragmentRequirement& InRequirement) : Requirement(InRequirement) {}

		bool operator==(const UScriptStruct* FragmentType) const { return Requirement.StructType == FragmentType; }
	};
	using FFragmentView = TFragmentView<TArrayView<FMassFragment>>;
	TArray<FFragmentView, TInlineAllocator<8>> FragmentViews;

	using FChunkFragmentView = TFragmentView<FStructView>;
	TArray<FChunkFragmentView, TInlineAllocator<4>> ChunkFragmentViews;

	using FConstSharedFragmentView = TFragmentView<FConstStructView>;
	TArray<FConstSharedFragmentView, TInlineAllocator<4>> ConstSharedFragmentViews;

	using FSharedFragmentView = TFragmentView<FStructView>;
	TArray<FSharedFragmentView, TInlineAllocator<4>> SharedFragmentViews;
	
	// mz@todo make this shared ptr thread-safe and never auto-flush in MT environment. 
	TSharedPtr<FMassCommandBuffer> DeferredCommandBuffer;
	TArrayView<FMassEntityHandle> EntityListView;
	
	/** If set this indicates the exact archetype and its chunks to be processed. 
	 *  @todo this data should live somewhere else, preferably be just a parameter to Query.ForEachEntityChunk function */
	FMassArchetypeSubChunks ChunkCollection;
	
	/** @todo rename to "payload" */
	FInstancedStruct AuxData;
	float DeltaTimeSeconds = 0.0f;
	int32 ChunkSerialModificationNumber = -1;
	FMassTagBitSet CurrentArchetypesTagBitSet;

#if WITH_MASSENTITY_DEBUG
	FString DebugExecutionDescription;
#endif
	
	/** If true the EntitySystem will flush the deferred commands stored in DeferredCommandBuffer just after executing 
	 *  the given system. If False then the party calling UEntitySubsystem::ExecuteSystem is responsible for manually
	 *  calling MassEntitySubsystem.FlushCommands() */
	bool bFlushDeferredCommands = true;

	TArrayView<FFragmentView> GetMutableRequirements() { return FragmentViews; }
	TArrayView<FChunkFragmentView> GetMutableChunkRequirements() { return ChunkFragmentViews; }
	TArrayView<FConstSharedFragmentView> GetMutableConstSharedRequirements() { return ConstSharedFragmentViews; }
	TArrayView<FSharedFragmentView> GetMutableSharedRequirements() { return SharedFragmentViews; }

	friend FMassArchetypeData;
	friend FMassEntityQuery;

public:
	FMassExecutionContext() = default;
	explicit FMassExecutionContext(const float InDeltaTimeSeconds, const bool bInFlushDeferredCommands = true)
		: DeltaTimeSeconds(InDeltaTimeSeconds)
		, bFlushDeferredCommands(bInFlushDeferredCommands)
	{}

#if WITH_MASSENTITY_DEBUG
	const FString& DebugGetExecutionDesc() const { return DebugExecutionDescription; }
	void DebugSetExecutionDesc(const FString& Description) { DebugExecutionDescription = Description; }
#endif

	/** Sets bFlushDeferredCommands. Note that setting to True while the system is being executed doesn't result in
	 *  immediate commands flushing */
	void SetFlushDeferredCommands(const bool bNewFlushDeferredCommands) { bFlushDeferredCommands = bNewFlushDeferredCommands; } 
	void SetDeferredCommandBuffer(const TSharedPtr<FMassCommandBuffer>& InDeferredCommandBuffer) { DeferredCommandBuffer = InDeferredCommandBuffer; }
	void SetChunkCollection(const FMassArchetypeSubChunks& InChunkCollection);
	void SetChunkCollection(FMassArchetypeSubChunks&& InChunkCollection);
	void ClearChunkCollection() { ChunkCollection.Reset(); }
	void SetAuxData(const FInstancedStruct& InAuxData) { AuxData = InAuxData; }

	float GetDeltaTimeSeconds() const
	{
		return DeltaTimeSeconds;
	}

	TSharedPtr<FMassCommandBuffer> GetSharedDeferredCommandBuffer() const { return DeferredCommandBuffer; }
	FMassCommandBuffer& Defer() const { checkSlow(DeferredCommandBuffer.IsValid()); return *DeferredCommandBuffer.Get(); }

	TConstArrayView<FMassEntityHandle> GetEntities() const { return EntityListView; }
	int32 GetNumEntities() const { return EntityListView.Num(); }

	FMassEntityHandle GetEntity(const int32 Index) const
	{
		return EntityListView[Index];
	}

	template<typename T>
	bool DoesArchetypeHaveTag() const
	{
		static_assert(TIsDerivedFrom<T, FMassTag>::IsDerived, "Given struct is not of a valid fragment type.");
		return CurrentArchetypesTagBitSet.Contains<T>();
	}

	/** Chunk related operations */
	void SetCurrentChunkSerialModificationNumber(const int32 SerialModificationNumber) { ChunkSerialModificationNumber = SerialModificationNumber; }
	int32 GetChunkSerialModificationNumber() const { return ChunkSerialModificationNumber; }

	template<typename T>
	T* GetMutableChunkFragmentPtr() const
	{
		static_assert(TIsDerivedFrom<T, FMassChunkFragment>::IsDerived, "Given struct doesn't represent a valid chunk fragment type. Make sure to inherit from FMassChunkFragment or one of its child-types.");

		const UScriptStruct* Type = T::StaticStruct();
		const FChunkFragmentView* FoundChunkFragmentData = ChunkFragmentViews.FindByPredicate([Type](const FChunkFragmentView& Element) { return Element.Requirement.StructType == Type; } );
		return FoundChunkFragmentData ? FoundChunkFragmentData->FragmentView.GetMutablePtr<T>() : static_cast<T*>(nullptr);
	}
	
	template<typename T>
	T& GetMutableChunkFragment() const
	{
		T* ChunkFragment = GetMutableChunkFragmentPtr<T>();
		checkf(ChunkFragment, TEXT("Chunk Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *ChunkFragment;
	}

	template<typename T>
	const T* GetChunkFragmentPtr() const
	{
		return GetMutableChunkFragmentPtr<T>();
	}
	
	template<typename T>
	const T& GetChunkFragment() const
	{
		const T* ChunkFragment = GetMutableChunkFragmentPtr<T>();
		checkf(ChunkFragment, TEXT("Chunk Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *ChunkFragment;
	}

	/** Shared fragment related operations */
	template<typename T>
	const T* GetConstSharedFragmentPtr() const
	{
		static_assert(TIsDerivedFrom<T, FMassSharedFragment>::IsDerived, "Given struct doesn't represent a valid Shared fragment type. Make sure to inherit from FMassSharedFragment or one of its child-types.");

		const FConstSharedFragmentView* FoundSharedFragmentData = ConstSharedFragmentViews.FindByPredicate([](const FConstSharedFragmentView& Element) { return Element.Requirement.StructType == T::StaticStruct(); });
		return FoundSharedFragmentData ? FoundSharedFragmentData->FragmentView.GetPtr<T>() : static_cast<const T*>(nullptr);
	}

	template<typename T>
	const T& GetConstSharedFragment() const
	{
		const T* SharedFragment = GetConstSharedFragmentPtr<T>();
		checkf(SharedFragment, TEXT("Shared Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *SharedFragment;
	}


	template<typename T>
	T* GetMutableSharedFragmentPtr() const
	{
		static_assert(TIsDerivedFrom<T, FMassSharedFragment>::IsDerived, "Given struct doesn't represent a valid shared fragment type. Make sure to inherit from FMassSharedFragment or one of its child-types.");

		const FSharedFragmentView* FoundSharedFragmentData = SharedFragmentViews.FindByPredicate([](const FSharedFragmentView& Element) { return Element.Requirement.StructType == T::StaticStruct(); });
		return FoundSharedFragmentData ? FoundSharedFragmentData->FragmentView.GetMutablePtr<T>() : static_cast<T*>(nullptr);
	}

	template<typename T>
	T& GetMutableSharedFragment() const
	{
		T* SharedFragment = GetMutableSharedFragmentPtr<T>();
		checkf(SharedFragment, TEXT("Shared Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *SharedFragment;
	}

	template<typename T>
	const T* GetSharedFragmentPtr() const
	{
		return GetMutableSharedFragmentPtr<T>();
	}

	template<typename T>
	const T& GetSharedFragment() const
	{
		const T* SharedFragment = GetSharedFragmentPtr<T>();
		checkf(SharedFragment, TEXT("Shared Fragment requirement not found: %s"), *T::StaticStruct()->GetName());
		return *SharedFragment;
	}

	/* Fragments related operations */
	template<typename TFragment>
	TArrayView<TFragment> GetMutableFragmentView()
	{
		const UScriptStruct* FragmentType = TFragment::StaticStruct();
		const FFragmentView* View = FragmentViews.FindByPredicate([FragmentType](const FFragmentView& Element) { return Element.Requirement.StructType == FragmentType; });
		//checkfSlow(View != nullptr, TEXT("Requested fragment type not bound"));
		//checkfSlow(View->Requirement.AccessMode == EMassFragmentAccess::ReadWrite, TEXT("Requested fragment has not been bound for writing"));
		return MakeArrayView<TFragment>((TFragment*)View->FragmentView.GetData(), View->FragmentView.Num());
	}

	template<typename TFragment>
	TConstArrayView<TFragment> GetFragmentView() const
	{
		const UScriptStruct* FragmentType = TFragment::StaticStruct();
		const FFragmentView* View = FragmentViews.FindByPredicate([FragmentType](const FFragmentView& Element) { return Element.Requirement.StructType == FragmentType; });
		//checkfSlow(View != nullptr, TEXT("Requested fragment type not bound"));
		return TConstArrayView<TFragment>((const TFragment*)View->FragmentView.GetData(), View->FragmentView.Num());
	}

	TConstArrayView<FMassFragment> GetFragmentView(const UScriptStruct* FragmentType) const
	{
		const FFragmentView* View = FragmentViews.FindByPredicate([FragmentType](const FFragmentView& Element) { return Element.Requirement.StructType == FragmentType; });
		checkSlow(View);
		return TConstArrayView<FMassFragment>((const FMassFragment*)View->FragmentView.GetData(), View->FragmentView.Num());;
	}

	TArrayView<FMassFragment> GetMutableFragmentView(const UScriptStruct* FragmentType) 
	{
		const FFragmentView* View = FragmentViews.FindByPredicate([FragmentType](const FFragmentView& Element) { return Element.Requirement.StructType == FragmentType; });
		checkSlow(View);
		return View->FragmentView;
	}

	/** Sparse chunk related operation */
	const FMassArchetypeSubChunks& GetChunkCollection() const { return ChunkCollection; }

	const FInstancedStruct& GetAuxData() const { return AuxData; }
	FInstancedStruct& GetMutableAuxData() { return AuxData; }
	
	template<typename TFragment>
	bool ValidateAuxDataType() const
	{
		const UScriptStruct* FragmentType = GetAuxData().GetScriptStruct();
		return FragmentType != nullptr && FragmentType == TFragment::StaticStruct();
	}

	void FlushDeferred(UMassEntitySubsystem& EntitySystem) const;

	void ClearExecutionData();
	void SetCurrentArchetypesTagBitSet(const FMassTagBitSet& BitSet)
	{
		CurrentArchetypesTagBitSet = BitSet;
	}

protected:
	void SetRequirements(TConstArrayView<FMassFragmentRequirement> InRequirements, 
		TConstArrayView<FMassFragmentRequirement> InChunkRequirements, 
		TConstArrayView<FMassFragmentRequirement> InConstSharedRequirements, 
		TConstArrayView<FMassFragmentRequirement> InSharedRequirements);

	void ClearFragmentViews()
	{
		for (FFragmentView& View : FragmentViews)
		{
			View.FragmentView = TArrayView<FMassFragment>();
		}
		for (FChunkFragmentView& View : ChunkFragmentViews)
		{
			View.FragmentView.Reset();
		}
		for (FConstSharedFragmentView& View : ConstSharedFragmentViews)
		{
			View.FragmentView.Reset();
		}
		for (FSharedFragmentView& View : SharedFragmentViews)
		{
			View.FragmentView.Reset();
		}
	}
};
