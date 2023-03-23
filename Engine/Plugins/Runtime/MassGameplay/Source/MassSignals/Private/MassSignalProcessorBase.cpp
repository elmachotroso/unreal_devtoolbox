﻿// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassSignalProcessorBase.h"
#include "MassSignalSubsystem.h"
#include "MassArchetypeTypes.h"
#include "Engine/World.h"

void UMassSignalProcessorBase::Initialize(UObject& Owner)
{
	SignalSubsystem = UWorld::GetSubsystem<UMassSignalSubsystem>(Owner.GetWorld());
}

void UMassSignalProcessorBase::BeginDestroy()
{
	if (SignalSubsystem)
	{
		for (const FName& SignalName : RegisteredSignals)
		{
			SignalSubsystem->GetSignalDelegateByName(SignalName).RemoveAll(this);
		}
	}

	Super::BeginDestroy();
}

void UMassSignalProcessorBase::SubscribeToSignal(const FName SignalName)
{
	check(SignalSubsystem);
	check(!RegisteredSignals.Contains(SignalName));
	RegisteredSignals.Add(SignalName);
	SignalSubsystem->GetSignalDelegateByName(SignalName).AddUObject(this, &UMassSignalProcessorBase::OnSignalReceived);
}

void UMassSignalProcessorBase::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	QUICK_SCOPE_CYCLE_COUNTER(SignalEntities);

	// Frame buffer handling
	const int32 ProcessingFrameBufferIndex = CurrentFrameBufferIndex;
	CurrentFrameBufferIndex = (CurrentFrameBufferIndex + 1) % 2;
	FFrameReceivedSignals& ProcessingFrameBuffer = FrameReceivedSignals[ProcessingFrameBufferIndex];
	TArray<FEntitySignalRange>& ReceivedSignalRanges = ProcessingFrameBuffer.ReceivedSignalRanges;
	TArray<FMassEntityHandle>& SignaledEntities = ProcessingFrameBuffer.SignaledEntities;

	if (ReceivedSignalRanges.IsEmpty())
	{
		return;
	}

	EntityQuery.CacheArchetypes(EntitySubsystem);
	if (EntityQuery.GetArchetypes().Num() > 0)
	{
		// EntitySet stores unique array of entities per specified archetype.
		// FMassArchetypeSubChunks expects an array of entities, a set is used to detect unique ones.
		struct FEntitySet
		{
			void Reset()
			{
				Entities.Reset();
			}

			FMassArchetypeHandle Archetype;
			TArray<FMassEntityHandle> Entities;
		};
		TArray<FEntitySet> EntitySets;

		for (const FMassArchetypeHandle& Archetype : EntityQuery.GetArchetypes())
		{
			FEntitySet& Set = EntitySets.AddDefaulted_GetRef();
			Set.Archetype = Archetype;
		}

		// SignalNameLookup has limit of how many signals it can handle at once, we'll do passes until all signals are processed.
		int32 SignalsToProcess = ReceivedSignalRanges.Num();
		while(SignalsToProcess > 0)
		{
			SignalNameLookup.Reset();

			// Convert signals with entity ids into arrays of entities per archetype.
			for (FEntitySignalRange& Range : ReceivedSignalRanges)
			{
				if (Range.bProcessed)
				{
					continue;
				}
				// Get bitflag for the signal name
				const uint64 SignalFlag = SignalNameLookup.GetOrAddSignalName(Range.SignalName);
				if (SignalFlag == 0)
				{
					// Will process that signal in a second iteration
					continue;
				}

				// Entities for this signal
				TArrayView<FMassEntityHandle> Entities(&SignaledEntities[Range.Begin], Range.End - Range.Begin);
				FEntitySet* PrevSet = &EntitySets[0];
				for (const FMassEntityHandle Entity : Entities)
				{
					// Add to set of supported archetypes. Dont process if we don't care about the type.
					FMassArchetypeHandle Archetype = EntitySubsystem.GetArchetypeForEntity(Entity);
					FEntitySet* Set = PrevSet->Archetype == Archetype ? PrevSet : EntitySets.FindByPredicate([&Archetype](const FEntitySet& Set) { return Archetype == Set.Archetype; });
					if (Set != nullptr)
					{
						// We don't care about duplicates here, the FMassArchetypeSubChunks creation below will handle it
						Set->Entities.Add(Entity);
						SignalNameLookup.AddSignalToEntity(Entity, SignalFlag);
						PrevSet = Set;
					}
				}

				Range.bProcessed = true;
				SignalsToProcess--;
			}

			// Execute per archetype.
			for (FEntitySet& Set : EntitySets)
			{
				if (Set.Entities.Num() > 0)
				{
					Context.SetChunkCollection(FMassArchetypeSubChunks(Set.Archetype, Set.Entities, FMassArchetypeSubChunks::FoldDuplicates));
					SignalEntities(EntitySubsystem, Context, SignalNameLookup);
					Context.ClearChunkCollection();
				}
				Set.Reset();
			}
		}
	}

	ReceivedSignalRanges.Reset();
	SignaledEntities.Reset();
}

void UMassSignalProcessorBase::OnSignalReceived(FName SignalName, TConstArrayView<FMassEntityHandle> Entities)
{
	FFrameReceivedSignals& CurrentFrameBuffer = FrameReceivedSignals[CurrentFrameBufferIndex];

	FEntitySignalRange& Range = CurrentFrameBuffer.ReceivedSignalRanges.AddDefaulted_GetRef();
	Range.SignalName = SignalName;
	Range.Begin = CurrentFrameBuffer.SignaledEntities.Num();
	CurrentFrameBuffer.SignaledEntities.Append(Entities.GetData(), Entities.Num());
	Range.End = CurrentFrameBuffer.SignaledEntities.Num();
}
