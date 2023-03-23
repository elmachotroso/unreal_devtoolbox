// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldPartition/WorldPartitionRuntimeSpatialHashCell.h"
#include "WorldPartition/WorldPartitionStreamingSource.h"
#include "HAL/IConsoleManager.h"

#include "Engine/Level.h"
#include "GameFramework/Actor.h"
#include "HAL/ConsoleManager.h"

static float GRuntimeSpatialHashCellToSourceAngleContributionToCellImportance = 0.4f; // Value between [0, 1]
static FAutoConsoleVariableRef CVarRuntimeSpatialHashCellToSourceAngleContributionToCellImportance(
	TEXT("wp.Runtime.RuntimeSpatialHashCellToSourceAngleContributionToCellImportance"),
	GRuntimeSpatialHashCellToSourceAngleContributionToCellImportance,
	TEXT("Value between 0 and 1 that modulates the contribution of the angle between streaming source-to-cell vector and source-forward vector to the cell importance. The closest to 0, the less the angle will contribute to the cell importance."));

UWorldPartitionRuntimeSpatialHashCell::UWorldPartitionRuntimeSpatialHashCell(const FObjectInitializer& ObjectInitializer)
: Super(ObjectInitializer)
, Level(0)
, CachedIsBlockingSource(false)
, CachedMinSquareDistanceToBlockingSource(MAX_FLT)
, CachedMinSquareDistanceToSource(MAX_FLT)
, CachedSourceSortingDistance(0.f)
{}

#if WITH_EDITOR
void UWorldPartitionRuntimeSpatialHashCell::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	if (UnsavedActorsContainer)
	{
		// Make sure actor container isn't under PIE World so those template actors will never be considered part of the world.
		UnsavedActorsContainer->Rename(nullptr, GetPackage());

		for (auto& ActorPair : UnsavedActorsContainer->Actors)
		{
			// Don't use AActor::Rename here since the actor is not par of the world, it's only a duplication template.
			ActorPair.Value->UObject::Rename(nullptr, UnsavedActorsContainer);
		}
	}
}
#endif

bool UWorldPartitionRuntimeSpatialHashCell::CacheStreamingSourceInfo(const UWorldPartitionRuntimeCell::FStreamingSourceInfo& Info) const
{
	const bool bWasCacheDirtied = Super::CacheStreamingSourceInfo(Info);
	if (bWasCacheDirtied)
	{
		CachedIsBlockingSource = false;
		CachedMinSquareDistanceToBlockingSource = MAX_FLT;
		CachedMinSquareDistanceToSource = MAX_FLT;
		CachedSourceModulatedDistances.Reset();
	}

	float AngleContribution = FMath::Clamp(GRuntimeSpatialHashCellToSourceAngleContributionToCellImportance, 0.f, 1.f);
	const float SquareDistance = FVector::DistSquared2D(Info.SourceShape.GetCenter(), Position);
	float AngleFactor = 1.f;
	if (!FMath::IsNearlyZero(AngleContribution))
	{
		const FBox Box(FVector(Position.X - Extent, Position.Y - Extent, 0.f), FVector(Position.X + Extent, Position.Y + Extent, 0.f));
		const FVector2D SourcePos(Info.SourceShape.GetCenter());
		const FVector StartVert(SourcePos, 0.f);
		const FVector EndVert(SourcePos + FVector2D(Info.SourceShape.GetScaledAxis()), 0.f);

		float Angle = 0.f;
		if (!FMath::LineBoxIntersection(Box, StartVert, EndVert, EndVert - StartVert))
		{
			// Find smallest angle using 4 corners and center of cell bounds
			const FVector2D Position2D(Position);
			float MaxDot = 0.f;
			FVector2D SourceForward(Info.SourceShape.GetAxis());
			SourceForward.Normalize();
			FVector2D CellPoints[5];
			CellPoints[0] = Position2D + FVector2D(-Extent, -Extent);
			CellPoints[1] = Position2D + FVector2D(-Extent, Extent);
			CellPoints[2] = Position2D + FVector2D(Extent, -Extent);
			CellPoints[3] = Position2D + FVector2D(Extent, Extent);
			CellPoints[4] = Position2D;
			for (const FVector2D& CellPoint : CellPoints)
			{
				const FVector2D SourceToCell(CellPoint - SourcePos);
				const float Dot = FVector2D::DotProduct(SourceForward, SourceToCell.GetSafeNormal());
				MaxDot = FMath::Max(MaxDot, Dot);
			}
			Angle = FMath::Abs(FMath::Acos(MaxDot) / PI);
		}
		const float NormalizedAngle = FMath::Clamp(Angle, PI/180.f, 1.f);
		AngleFactor = FMath::Pow(NormalizedAngle, AngleContribution);
	}
	// Modulate distance to cell by angle relative to source forward vector (to prioritize cells in front)
	const float SquareAngleFactor = AngleFactor * AngleFactor;
	const float ModulatedSquareDistance = SquareDistance * SquareAngleFactor;
	CachedSourceModulatedDistances.Add(ModulatedSquareDistance);
	int32 Count = CachedSourceModulatedDistances.Num();
	check(Count == CachedSourcePriorityWeights.Num());
	if (Count == 1)
	{
		CachedSourceSortingDistance = ModulatedSquareDistance;
	}
	else
	{
		float TotalSourcePriorityWeight = 0.f;
		for (int32 i = 0; i < Count; ++i)
		{
			TotalSourcePriorityWeight += CachedSourcePriorityWeights[i];
		}
		int32 HighestPrioMinDistIndex = 0;
		float WeightedModulatedDistance = 0.f;
		for (int32 i = 0; i < Count; ++i)
		{
			WeightedModulatedDistance += CachedSourceModulatedDistances[i] * CachedSourcePriorityWeights[i] / TotalSourcePriorityWeight;
			
			// Find highest priority source with the minimum modulated distance
			if ((i != 0) &&
				(CachedSourceModulatedDistances[i] < CachedSourceModulatedDistances[HighestPrioMinDistIndex]) &&
				(CachedSourcePriorityWeights[i] >= CachedSourcePriorityWeights[HighestPrioMinDistIndex]))
			{
				HighestPrioMinDistIndex = i;
			}
		}
		// Sorting distance is the minimum between these:
		// - the highest priority source with the minimum modulated distance 
		// - the weighted modulated distance
		CachedSourceSortingDistance = FMath::Min(CachedSourceModulatedDistances[HighestPrioMinDistIndex], WeightedModulatedDistance);
	}
	
	// Only consider blocking sources
	if (Info.Source.bBlockOnSlowLoading)
	{
		CachedIsBlockingSource = true;
		CachedMinSquareDistanceToBlockingSource = FMath::Min(SquareDistance, CachedMinSquareDistanceToBlockingSource);
	}

	CachedMinSquareDistanceToSource = FMath::Min(SquareDistance, CachedMinSquareDistanceToSource);
	return bWasCacheDirtied;
}

int32 UWorldPartitionRuntimeSpatialHashCell::SortCompare(const UWorldPartitionRuntimeCell* InOther) const
{
	int32 Result = Super::SortCompare(InOther);
	if (Result == 0)
	{
		const UWorldPartitionRuntimeSpatialHashCell* Other = Cast<const UWorldPartitionRuntimeSpatialHashCell>(InOther);
		check(Other);
		
		// Level (higher value is higher prio)
		Result = Other->Level - Level;
		if (Result == 0)
		{
			// Closest distance (lower value is higher prio)
			const float Diff = CachedSourceSortingDistance - Other->CachedSourceSortingDistance;
			if (FMath::IsNearlyZero(Diff))
			{
				const float RawDistanceDiff = CachedMinSquareDistanceToSource - Other->CachedMinSquareDistanceToSource;
				Result = RawDistanceDiff < 0.f ? -1 : (RawDistanceDiff > 0.f ? 1 : 0);
			}
			else
			{
				Result = Diff < 0.f ? -1 : (Diff > 0.f ? 1 : 0);
			}
		}
	}
	return Result;
}