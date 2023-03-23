// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MassEntitySubsystem.h"
#include "MassArchetypeTypes.h"

struct FMassEntityQuery;
struct FMassExecutionContext;
class FOutputDevice;
struct FMassArchetypeSubChunks;

namespace UE::MassEntity
{
	constexpr int32 ChunkSize = 128*1024;
}

// This is one chunk within an archetype
struct FMassArchetypeChunk
{
private:
	uint8* RawMemory = nullptr;
	int32 AllocSize = 0;
	int32 NumInstances = 0;
	int32 SerialModificationNumber = 0;
	TArray<FInstancedStruct> ChunkFragmentData;

public:
	explicit FMassArchetypeChunk(int32 InAllocSize, TConstArrayView<FInstancedStruct> InChunkFragmentTemplates)
		: AllocSize(InAllocSize)
		, ChunkFragmentData(InChunkFragmentTemplates)
	{
		RawMemory = (uint8*)FMemory::Malloc(AllocSize);
	}

	~FMassArchetypeChunk()
	{
		// Only release memory if it was not done already.
		if (RawMemory != nullptr)
		{
			FMemory::Free(RawMemory);
			RawMemory = nullptr;
		}
	}

	// Returns the Entity array element at the specified index
	FMassEntityHandle& GetEntityArrayElementRef(int32 ChunkBase, int32 IndexWithinChunk)
	{
		return ((FMassEntityHandle*)(RawMemory + ChunkBase))[IndexWithinChunk];
	}

	uint8* GetRawMemory() const
	{
		return RawMemory;
	}

	int32 GetNumInstances() const
	{
		return NumInstances;
	}

	void AddMultipleInstances(uint32 Count)
	{
		NumInstances += Count;
		SerialModificationNumber++;
	}

	void RemoveMultipleInstances(uint32 Count)
	{
		NumInstances -= Count;
		check(NumInstances >= 0);
		SerialModificationNumber++;

		// Because we only remove trailing chunks to avoid messing up the absolute indices in the entities map,
		// We are freeing the memory here to save memory
		if (NumInstances == 0)
		{
			FMemory::Free(RawMemory);
			RawMemory = nullptr;
		}
	}

	void AddInstance()
	{
		AddMultipleInstances(1);
	}

	void RemoveInstance()
	{
		RemoveMultipleInstances(1);
	}

	int32 GetSerialModificationNumber() const
	{
		return SerialModificationNumber;
	}

	FStructView GetMutableChunkFragmentViewChecked(const int32 Index) { return FStructView(ChunkFragmentData[Index]); }

	FInstancedStruct* FindMutableChunkFragment(const UScriptStruct* Type)
	{
		return ChunkFragmentData.FindByPredicate([Type](const FInstancedStruct& Element)
			{
				return Element.GetScriptStruct()->IsChildOf(Type);
			});
	}

	void Recycle(TConstArrayView<FInstancedStruct> InChunkFragmentsTemplate)
	{
		checkf(NumInstances == 0, TEXT("Recycling a chunk that is not empty."));
		SerialModificationNumber++;
		ChunkFragmentData = InChunkFragmentsTemplate;
		
		// If this chunk previously had entity and it does not anymore, we might have to reallocate the memory as it was freed to save memory
		if (RawMemory == nullptr)
		{
			RawMemory = (uint8*)FMemory::Malloc(AllocSize);
		}
	}

#if WITH_MASSENTITY_DEBUG
	int32 DebugGetChunkFragmentCount() const { return ChunkFragmentData.Num(); }
#endif // WITH_MASSENTITY_DEBUG
};

// Information for a single fragment type in an archetype
struct FMassArchetypeFragmentConfig
{
	const UScriptStruct* FragmentType = nullptr;
	int32 ArrayOffsetWithinChunk = 0;

	void* GetFragmentData(uint8* ChunkBase, int32 IndexWithinChunk) const
	{
		return ChunkBase + ArrayOffsetWithinChunk + (IndexWithinChunk * FragmentType->GetStructureSize());
	}
};

// An archetype is defined by a collection of unique fragment types (no duplicates).
// Order doesn't matter, there will only ever be one FMassArchetypeData per unique set of fragment types per entity manager subsystem
struct FMassArchetypeData
{
private:
	// One-stop-shop variable describing the archetype's fragment and tag composition 
	FMassArchetypeCompositionDescriptor CompositionDescriptor;
	FMassArchetypeSharedFragmentValues SharedFragmentValues;

	// Pre-created default chunk fragment templates
	TArray<FInstancedStruct> ChunkFragmentsTemplate;

	TArray<FMassArchetypeFragmentConfig, TInlineAllocator<16>> FragmentConfigs;
	
	TArray<FMassArchetypeChunk> Chunks;

	// Entity ID to index within archetype
	//@TODO: Could be folded into FEntityData in the entity manager at the expense of a bit
	// of loss of encapsulation and extra complexity during archetype changes
	TMap<int32, int32> EntityMap;
	
	TMap<const UScriptStruct*, int32> FragmentIndexMap;

	int32 NumEntitiesPerChunk;
	int32 TotalBytesPerEntity;
	int32 EntityListOffsetWithinChunk;

	friend FMassEntityQuery;
	friend FMassArchetypeSubChunks;

public:
	TConstArrayView<FMassArchetypeFragmentConfig> GetFragmentConfigs() const { return FragmentConfigs; }
	const FMassFragmentBitSet& GetFragmentBitSet() const { return CompositionDescriptor.Fragments; }
	const FMassTagBitSet& GetTagBitSet() const { return CompositionDescriptor.Tags; }
	const FMassChunkFragmentBitSet& GetChunkFragmentBitSet() const { return CompositionDescriptor.ChunkFragments; }
	const FMassSharedFragmentBitSet& GetSharedFragmentBitSet() const { return CompositionDescriptor.SharedFragments; }

	const FMassArchetypeCompositionDescriptor& GetCompositionDescriptor() const { return CompositionDescriptor; }
	const FMassArchetypeSharedFragmentValues& GetSharedFragmentValues() const { return SharedFragmentValues; }

	/** Method to iterate on all the fragment types */
	void ForEachFragmentType(TFunction< void(const UScriptStruct* /*FragmentType*/)> Function) const;
	bool HasFragmentType(const UScriptStruct* FragmentType) const;
	bool HasTagType(const UScriptStruct* FragmentType) const { check(FragmentType); return CompositionDescriptor.Tags.Contains(*FragmentType); }

	bool IsEquivalent(const FMassArchetypeCompositionDescriptor& OtherCompositionDescriptor, const FMassArchetypeSharedFragmentValues& OtherSharedFragmentValues) const
	{
		return CompositionDescriptor.IsEquivalent(OtherCompositionDescriptor) && SharedFragmentValues.IsEquivalent(OtherSharedFragmentValues);
	}

	void Initialize(const FMassArchetypeCompositionDescriptor& InCompositionDescriptor, const FMassArchetypeSharedFragmentValues& InSharedFragmentValues);

	/** 
	 * A special way of initializing an archetype resulting in a copy of SiblingArchetype's setup with OverrideTags
	 * replacing original tags of SiblingArchetype
	 */
	void InitializeWithSibling(const FMassArchetypeData& SiblingArchetype, const FMassTagBitSet& OverrideTags);

	void AddEntity(FMassEntityHandle Entity);
	void RemoveEntity(FMassEntityHandle Entity);
	void BatchDestroyEntityChunks(FMassArchetypeSubChunks::FConstSubChunkArrayView SubChunkContainer, TArray<FMassEntityHandle>& OutEntitiesRemoved);

	bool HasFragmentDataForEntity(const UScriptStruct* FragmentType, int32 EntityIndex) const;
	void* GetFragmentDataForEntityChecked(const UScriptStruct* FragmentType, int32 EntityIndex) const;
	void* GetFragmentDataForEntity(const UScriptStruct* FragmentType, int32 EntityIndex) const;

	FORCEINLINE int32 GetInternalIndexForEntity(const int32 EntityIndex) const { return EntityMap.FindChecked(EntityIndex); }
	int32 GetNumEntitiesPerChunk() const { return NumEntitiesPerChunk; }

	int32 GetNumEntities() const { return EntityMap.Num(); }

	int32 GetChunkAllocSize() const { return UE::MassEntity::ChunkSize; }

	int32 GetChunkCount() const { return Chunks.Num(); }

	void ExecuteFunction(FMassExecutionContext& RunContext, const FMassExecuteFunction& Function, const FMassQueryRequirementIndicesMapping& RequirementMapping, FMassArchetypeSubChunks::FConstSubChunkArrayView SubChunkContainer);
	void ExecuteFunction(FMassExecutionContext& RunContext, const FMassExecuteFunction& Function, const FMassQueryRequirementIndicesMapping& RequirementMapping, const FMassArchetypeConditionFunction& ArchetypeCondition, const FMassChunkConditionFunction& ChunkCondition);

	void ExecutionFunctionForChunk(FMassExecutionContext RunContext, const FMassExecuteFunction& Function, const FMassQueryRequirementIndicesMapping& RequirementMapping, const FMassArchetypeSubChunks::FSubChunkInfo& ChunkInfo, const FMassChunkConditionFunction& ChunkCondition = FMassChunkConditionFunction());

	/**
	 * Compacts entities to fill up chunks as much as possible
	 */
	void CompactEntities(const double TimeAllowed);

	/**
	 * Moves the entity from this archetype to another, will only copy all matching fragment types
	 * @param Entity is the entity to move
	 * @param NewArchetype the archetype to move to
	 */
	void MoveEntityToAnotherArchetype(const FMassEntityHandle Entity, FMassArchetypeData& NewArchetype);

	/**
	 * Set all fragment sources data on specified entity, will check if there are fragment sources type that does not exist in the archetype
	 * @param Entity is the entity to set the data of all fragments
	 * @param FragmentSources are the fragments to copy the data from
	 */
	void SetFragmentsData(const FMassEntityHandle Entity, TArrayView<const FInstancedStruct> FragmentSources);

	/** For all entities indicated by ChunkCollection the function sets the value of fragment of type
	 *  FragmentSource.GetScriptStruct to the value represented by FragmentSource.GetMemory */
	void SetFragmentData(FMassArchetypeSubChunks::FConstSubChunkArrayView SubChunkContainer, const FInstancedStruct& FragmentSource);

	/** Returns conversion from given Requirements to archetype's fragment indices */
	void GetRequirementsFragmentMapping(TConstArrayView<FMassFragmentRequirement> Requirements, FMassFragmentIndicesMapping& OutFragmentIndices);

	/** Returns conversion from given ChunkRequirements to archetype's chunk fragment indices */
	void GetRequirementsChunkFragmentMapping(TConstArrayView<FMassFragmentRequirement> ChunkRequirements, FMassFragmentIndicesMapping& OutFragmentIndices);

	/** Returns conversion from given const shared requirements to archetype's const shared fragment indices */
	void GetRequirementsConstSharedFragmentMapping(TConstArrayView<FMassFragmentRequirement> Requirements, FMassFragmentIndicesMapping& OutFragmentIndices);

	/** Returns conversion from given shared requirements to archetype's shared fragment indices */
	void GetRequirementsSharedFragmentMapping(TConstArrayView<FMassFragmentRequirement> Requirements, FMassFragmentIndicesMapping& OutFragmentIndices);

	SIZE_T GetAllocatedSize() const;

	// Converts the list of fragments into a user-readable debug string
	FString DebugGetDescription() const;

#if WITH_MASSENTITY_DEBUG
	/**
	 * Prints out debug information about the archetype
	 */
	void DebugPrintArchetype(FOutputDevice& Ar);

	/**
	 * Prints out fragment's values for the specified entity. 
	 * @param Entity The entity for which we want to print fragment values
	 * @param Ar The output device
	 * @param InPrefix Optional prefix to remove from fragment names
	 */
	void DebugPrintEntity(FMassEntityHandle Entity, FOutputDevice& Ar, const TCHAR* InPrefix = TEXT("")) const;
#endif // WITH_MASSENTITY_DEBUG

	void REMOVEME_GetArrayViewForFragmentInChunk(int32 ChunkIndex, const UScriptStruct* FragmentType, void*& OutChunkBase, int32& OutNumEntities);

	//////////////////////////////////////////////////////////////////////
	// low level api
	FORCEINLINE const int32* GetFragmentIndex(const UScriptStruct* FragmentType) const { return FragmentIndexMap.Find(FragmentType); }
	FORCEINLINE int32 GetFragmentIndexChecked(const UScriptStruct* FragmentType) const { return FragmentIndexMap.FindChecked(FragmentType); }

	FORCEINLINE void* GetFragmentData(const int32 FragmentIndex, const FMassRawEntityInChunkData EntityIndex) const
	{
		return FragmentConfigs[FragmentIndex].GetFragmentData(EntityIndex.ChunkRawMemory, EntityIndex.IndexWithinChunk);
	}

	FORCEINLINE FMassRawEntityInChunkData MakeEntityHandle(int32 EntityIndex) const
	{
		const int32 AbsoluteIndex = EntityMap.FindChecked(EntityIndex);
		const int32 ChunkIndex = AbsoluteIndex / NumEntitiesPerChunk;
	
		return FMassRawEntityInChunkData(Chunks[ChunkIndex].GetRawMemory(), AbsoluteIndex % NumEntitiesPerChunk); 
	}

	FORCEINLINE FMassRawEntityInChunkData MakeEntityHandle(FMassEntityHandle Entity) const
	{
		return MakeEntityHandle(Entity.Index); 
	}

	bool IsInitialized() const { return TotalBytesPerEntity > 0 && FragmentConfigs.IsEmpty() == false; }

protected:
	FORCEINLINE void* GetFragmentData(const int32 FragmentIndex, uint8* ChunkRawMemory, const int32 IndexWithinChunk) const
	{
		return FragmentConfigs[FragmentIndex].GetFragmentData(ChunkRawMemory, IndexWithinChunk);
	}

	void BindEntityRequirements(FMassExecutionContext& RunContext, const FMassFragmentIndicesMapping& EntityFragmentsMapping, FMassArchetypeChunk& Chunk, const int32 SubchunkStart, const int32 SubchunkLength);
	void BindChunkFragmentRequirements(FMassExecutionContext& RunContext, const FMassFragmentIndicesMapping& ChunkFragmentsMapping, FMassArchetypeChunk& Chunk);
	void BindConstSharedFragmentRequirements(FMassExecutionContext& RunContext, const FMassFragmentIndicesMapping& ChunkFragmentsMapping);
	void BindSharedFragmentRequirements(FMassExecutionContext& RunContext, const FMassFragmentIndicesMapping& ChunkFragmentsMapping);

private:
	int32 AddEntityInternal(FMassEntityHandle Entity, const bool bInitializeFragments);
	void RemoveEntityInternal(const int32 AbsoluteIndex, const bool bDestroyFragments);
};
