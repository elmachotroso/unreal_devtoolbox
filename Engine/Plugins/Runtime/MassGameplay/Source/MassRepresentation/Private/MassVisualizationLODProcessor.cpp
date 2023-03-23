// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassVisualizationLODProcessor.h"

namespace UE::MassRepresentation
{
	int32 bDebugRepresentationLOD = 0;
	FAutoConsoleVariableRef CVarDebugRepresentationLOD(TEXT("ai.debug.RepresentationLOD"), bDebugRepresentationLOD, TEXT("Debug representation LOD"), ECVF_Cheat);
} // UE::MassRepresentation


UMassVisualizationLODProcessor::UMassVisualizationLODProcessor()
{
	bAutoRegisterWithProcessingPhases = false;

	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::LOD;
	ExecutionOrder.ExecuteAfter.Add(UE::Mass::ProcessorGroupNames::LODCollector);
}

void UMassVisualizationLODProcessor::ConfigureQueries()
{
	FMassEntityQuery BaseQuery;
	BaseQuery.AddRequirement<FMassViewerInfoFragment>(EMassFragmentAccess::ReadOnly);
	BaseQuery.AddRequirement<FMassRepresentationLODFragment>(EMassFragmentAccess::ReadWrite);
	BaseQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	BaseQuery.AddConstSharedRequirement<FMassVisualizationLODParameters>();
	BaseQuery.AddSharedRequirement<FMassVisualizationLODSharedFragment>(EMassFragmentAccess::ReadWrite);

	CloseEntityQuery = BaseQuery;
	CloseEntityQuery.AddTagRequirement<FMassVisibilityCulledByDistanceTag>(EMassFragmentPresence::None);
	
	CloseEntityAdjustDistanceQuery = CloseEntityQuery;
	CloseEntityAdjustDistanceQuery.SetArchetypeFilter([](const FMassExecutionContext& Context)
	{
		const FMassVisualizationLODSharedFragment& LODSharedFragment = Context.GetSharedFragment<FMassVisualizationLODSharedFragment>();
		return LODSharedFragment.bHasAdjustedDistancesFromCount;
	});

	FarEntityQuery = BaseQuery;
	FarEntityQuery.AddTagRequirement<FMassVisibilityCulledByDistanceTag>(EMassFragmentPresence::All);
	FarEntityQuery.AddChunkRequirement<FMassVisualizationChunkFragment>(EMassFragmentAccess::ReadOnly);
	FarEntityQuery.SetChunkFilter(&FMassVisualizationChunkFragment::ShouldUpdateVisualizationForChunk);

	DebugEntityQuery = BaseQuery;
}

void UMassVisualizationLODProcessor::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	if (bForceOFFLOD)
	{
		CloseEntityQuery.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context)
		{
			FMassVisualizationLODSharedFragment& LODSharedFragment = Context.GetMutableSharedFragment<FMassVisualizationLODSharedFragment>();
			TArrayView<FMassRepresentationLODFragment> RepresentationLODList = Context.GetMutableFragmentView<FMassRepresentationLODFragment>();
			LODSharedFragment.LODCalculator.ForceOffLOD(Context, RepresentationLODList);
		});
		return;
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(PrepareExecution)
		check(LODSubsystem);
		const TArray<FViewerInfo>& Viewers = LODSubsystem->GetViewers();
		EntitySubsystem.ForEachSharedFragment<FMassVisualizationLODSharedFragment>([this, &Viewers](FMassVisualizationLODSharedFragment& LODSharedFragment)
		{
			if (FilterTag == LODSharedFragment.FilterTag)
			{
				LODSharedFragment.LODCalculator.PrepareExecution(Viewers);
			}
		});
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(CalculateLOD)

		auto CalculateLOD = [this](FMassExecutionContext& Context)
		{
			FMassVisualizationLODSharedFragment& LODSharedFragment = Context.GetMutableSharedFragment<FMassVisualizationLODSharedFragment>();
			TArrayView<FMassRepresentationLODFragment> RepresentationLODList = Context.GetMutableFragmentView<FMassRepresentationLODFragment>();
			TConstArrayView<FMassViewerInfoFragment> ViewerInfoList = Context.GetFragmentView<FMassViewerInfoFragment>();
			LODSharedFragment.LODCalculator.CalculateLOD(Context, ViewerInfoList, RepresentationLODList);
		};
		CloseEntityQuery.ForEachEntityChunk(EntitySubsystem, Context, CalculateLOD);
		FarEntityQuery.ForEachEntityChunk(EntitySubsystem, Context, CalculateLOD);
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(AdjustDistanceAndLODFromCount)
		EntitySubsystem.ForEachSharedFragment<FMassVisualizationLODSharedFragment>([this](FMassVisualizationLODSharedFragment& LODSharedFragment)
		{
			if (FilterTag == LODSharedFragment.FilterTag)
			{
				LODSharedFragment.bHasAdjustedDistancesFromCount = LODSharedFragment.LODCalculator.AdjustDistancesFromCount();
			}
		});

		CloseEntityAdjustDistanceQuery.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context)
		{
			FMassVisualizationLODSharedFragment& LODSharedFragment = Context.GetMutableSharedFragment<FMassVisualizationLODSharedFragment>();
			TConstArrayView<FMassViewerInfoFragment> ViewerInfoList = Context.GetFragmentView<FMassViewerInfoFragment>();
			TArrayView<FMassRepresentationLODFragment> RepresentationLODList = Context.GetMutableFragmentView<FMassRepresentationLODFragment>();
			LODSharedFragment.LODCalculator.AdjustLODFromCount(Context, ViewerInfoList, RepresentationLODList);
		});
		// Far entities do not need to maximize count
	}

	// Optional debug display
	if (UE::MassRepresentation::bDebugRepresentationLOD)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(DebugDisplayLOD)
		DebugEntityQuery.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context)
		{
			FMassVisualizationLODSharedFragment& LODSharedFragment = Context.GetMutableSharedFragment<FMassVisualizationLODSharedFragment>();
			TConstArrayView<FMassRepresentationLODFragment> RepresentationLODList = Context.GetFragmentView<FMassRepresentationLODFragment>();
			TConstArrayView<FTransformFragment> TransformList = Context.GetFragmentView<FTransformFragment>();
			LODSharedFragment.LODCalculator.DebugDisplayLOD(Context, RepresentationLODList, TransformList, World);
		});
	}
}
