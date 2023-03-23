// Copyright Epic Games, Inc. All Rights Reserved.

#include "MassCrowdServerRepresentationLODProcessor.h"
#include "MassCommonFragments.h"
#include "MassRepresentationFragments.h"

namespace UE::MassCrowd
{
	int32 bDebugCrowdServerRepresentationLOD = 0;
	FAutoConsoleVariableRef CVarDebugServerRepresentationLODTest(TEXT("ai.debug.CrowdServerRepresentationLOD"), bDebugCrowdServerRepresentationLOD, TEXT("Debug Crowd ServerRepresentation LOD"), ECVF_Cheat);
} // UE::MassCrowd

UMassCrowdServerRepresentationLODProcessor::UMassCrowdServerRepresentationLODProcessor()
{
	ExecutionFlags = (int32)EProcessorExecutionFlags::Server;

	ExecutionOrder.ExecuteInGroup = UE::Mass::ProcessorGroupNames::LOD;
	ExecutionOrder.ExecuteAfter.Add(UE::Mass::ProcessorGroupNames::LODCollector);

	LODDistance[EMassLOD::High] = 0.0f;
	LODDistance[EMassLOD::Medium] = 5000.0f;
	LODDistance[EMassLOD::Low] = 5000.0f;
	LODDistance[EMassLOD::Off] = 5000.0f;
	
	LODMaxCount[EMassLOD::High] = 50;
	LODMaxCount[EMassLOD::Medium] = 0;
	LODMaxCount[EMassLOD::Low] = 0;
	LODMaxCount[EMassLOD::Off] = INT_MAX;
}

void UMassCrowdServerRepresentationLODProcessor::ConfigureQueries()
{
	EntityQuery.AddTagRequirement<FMassCrowdTag>(EMassFragmentPresence::All);
	EntityQuery.AddRequirement<FTransformFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassViewerInfoFragment>(EMassFragmentAccess::ReadOnly);
	EntityQuery.AddRequirement<FMassRepresentationLODFragment>(EMassFragmentAccess::ReadWrite);
}

void UMassCrowdServerRepresentationLODProcessor::Initialize(UObject& InOwner)
{
	LODCalculator.Initialize(LODDistance, BufferHysteresisOnDistancePercentage / 100.0f, LODMaxCount);

	Super::Initialize(InOwner);
}

void UMassCrowdServerRepresentationLODProcessor::Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("CrowdServerRepresentationLOD"))

	check(LODSubsystem);
	const TArray<FViewerInfo>& Viewers = LODSubsystem->GetViewers();
	LODCalculator.PrepareExecution(Viewers);
	
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("CalculateLOD"))
		
		EntityQuery.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context)
		{
			const TConstArrayView<FMassViewerInfoFragment> ViewersInfoList = Context.GetFragmentView<FMassViewerInfoFragment>();
			const TArrayView<FMassRepresentationLODFragment> RepresentationLODFragments = Context.GetMutableFragmentView<FMassRepresentationLODFragment>();
			LODCalculator.CalculateLOD(Context, ViewersInfoList, RepresentationLODFragments);
		});
	}

	{
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("AdjustDistancesAndLODFromCount"))
		
		if (LODCalculator.AdjustDistancesFromCount())
		{
			EntityQuery.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context)
			{
				const TConstArrayView<FMassViewerInfoFragment> ViewersInfoList = Context.GetFragmentView<FMassViewerInfoFragment>();
				const TArrayView<FMassRepresentationLODFragment> RepresentationLODFragments = Context.GetMutableFragmentView<FMassRepresentationLODFragment>();
				LODCalculator.AdjustLODFromCount(Context, ViewersInfoList, RepresentationLODFragments);
			});
		}
	}

	// Optional debug display
	if (UE::MassCrowd::bDebugCrowdServerRepresentationLOD)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(TEXT("DebugDisplayLOD"))
		
		EntityQuery.ForEachEntityChunk(EntitySubsystem, Context, [this](FMassExecutionContext& Context)
		{
			const TConstArrayView<FTransformFragment> LocationList = Context.GetFragmentView<FTransformFragment>();
			const TConstArrayView<FMassRepresentationLODFragment> RepresentationLODFragments = Context.GetFragmentView<FMassRepresentationLODFragment>();
			LODCalculator.DebugDisplayLOD(Context, RepresentationLODFragments, LocationList, World);
		});
	}
}