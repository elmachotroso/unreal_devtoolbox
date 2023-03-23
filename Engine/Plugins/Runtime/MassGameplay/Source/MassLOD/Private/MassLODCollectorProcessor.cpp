// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassLODCollectorProcessor.h"
#include "MassLODUtils.h"
#include "MassCommonFragments.h"
#include "Engine/World.h"
#include "MassSimulationLOD.h"

UMassLODCollectorProcessor::UMassLODCollectorProcessor()
{
	bAutoRegisterWithProcessingPhases = false;

	ExecutionFlags = (int32)EProcessorExecutionFlags::All;

	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::LODCollector;
	ExecutionOrder.ExecuteAfter.Add(UE::Mass::ProcessorGroupNames::SyncWorldToMass);
}

void UMassLODCollectorProcessor::ConfigureQueries()
{
	FMassEntityQuery BaseQuery;
	BaseQuery.AddTagRequirement<FMassCollectLODViewerInfoTag>(EMassFragmentPresence::All);
	BaseQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	BaseQuery.AddRequirement<FMassViewerInfoFragment>(EMassFragmentAccess::ReadWrite);
	BaseQuery.AddChunkRequirement<FMassSimulationVariableTickChunkFragment>(EMassFragmentAccess::ReadOnly, EMassFragmentPresence::Optional);
	BaseQuery.AddChunkRequirement<FMassVisualizationChunkFragment>(EMassFragmentAccess::ReadOnly, EMassFragmentPresence::Optional);
	BaseQuery.SetChunkFilter([](const FMassExecutionContext& Context)
	{
		return FMassVisualizationChunkFragment::IsChunkHandledThisFrame(Context)
			|| FMassSimulationVariableTickChunkFragment::IsChunkHandledThisFrame(Context);
	});

	EntityQuery_VisibleRangeAndOnLOD = BaseQuery;
	EntityQuery_VisibleRangeAndOnLOD.AddTagRequirement<FMassVisibilityCulledByDistanceTag>(EMassFragmentPresence::None);
	EntityQuery_VisibleRangeAndOnLOD.AddTagRequirement<FMassOffLODTag>(EMassFragmentPresence::None);

	EntityQuery_VisibleRangeOnly = BaseQuery;
	EntityQuery_VisibleRangeOnly.AddTagRequirement<FMassVisibilityCulledByDistanceTag>(EMassFragmentPresence::None);
	EntityQuery_VisibleRangeOnly.AddTagRequirement<FMassOffLODTag>(EMassFragmentPresence::All);

	EntityQuery_OnLODOnly = BaseQuery;
	EntityQuery_OnLODOnly.AddTagRequirement<FMassVisibilityCulledByDistanceTag>(EMassFragmentPresence::All);
	EntityQuery_OnLODOnly.AddTagRequirement<FMassOffLODTag>(EMassFragmentPresence::None);

	EntityQuery_NotVisibleRangeAndOffLOD = BaseQuery;
	EntityQuery_NotVisibleRangeAndOffLOD.AddTagRequirement<FMassVisibilityCulledByDistanceTag>(EMassFragmentPresence::All);
	EntityQuery_NotVisibleRangeAndOffLOD.AddTagRequirement<FMassOffLODTag>(EMassFragmentPresence::All);
}

template <bool bLocalViewersOnly>
void UMassLODCollectorProcessor::CollectLODForChunk(FMassExecutionContext& Context)
{
	TConstArrayView<FTransformFragment> LocationList = Context.GetFragmentView<FTransformFragment>();
	TArrayView<FMassViewerInfoFragment> ViewerInfoList = Context.GetMutableFragmentView<FMassViewerInfoFragment>();

	Collector.CollectLODInfo<FTransformFragment, FMassViewerInfoFragment, bLocalViewersOnly, true/*bCollectDistanceToViewer*/>(Context, LocationList, ViewerInfoList);
}

template <bool bLocalViewersOnly>
void UMassLODCollectorProcessor::ExecuteInternal(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("Close"));
		EntityQuery_VisibleRangeAndOnLOD.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context) { CollectLODForChunk<bLocalViewersOnly>(Context); });
		EntityQuery_VisibleRangeOnly.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context) { CollectLODForChunk<bLocalViewersOnly>(Context); });
		EntityQuery_OnLODOnly.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context) { CollectLODForChunk<bLocalViewersOnly>(Context); });
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("Far"));
		EntityQuery_NotVisibleRangeAndOffLOD.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context) { CollectLODForChunk<bLocalViewersOnly>(Context); });
	}
}

void UMassLODCollectorProcessor::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	check(LODSubsystem);
	const TArray<FViewerInfo>& Viewers = LODSubsystem->GetViewers();
	Collector.PrepareExecution(Viewers);

	check(World);
	if (World->IsNetMode(NM_DedicatedServer))
	{
		ExecuteInternal<false/*bLocalViewersOnly*/>(EntitySubsystem, Context);
	}
	else
	{
		ExecuteInternal<true/*bLocalViewersOnly*/>(EntitySubsystem, Context);
	}

}
