// Copyright Epic Games, Inc. All Rights Reserved.

#include "PoseSearch/PoseSearch.h"
#include "PoseSearchEigenHelper.h"

#include "Algo/BinarySearch.h"
#include "Async/ParallelFor.h"
#include "Templates/IdentityFunctor.h"
#include "Templates/Invoke.h"
#include "Templates/Less.h"
#include "Features/IModularFeatures.h"
#include "DrawDebugHelpers.h"
#include "Animation/AnimPoseSearchProvider.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimMetaData.h"
#include "Animation/AnimNodeBase.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimSequenceBase.h"
#include "Animation/AnimationPoseData.h"
#include "AnimationRuntime.h"
#include "BonePose.h"
#include "Trace/PoseSearchTraceLogger.h"
#include "UObject/ObjectSaveContext.h"
#include "Misc/MemStack.h"

IMPLEMENT_ANIMGRAPH_MESSAGE(UE::PoseSearch::IPoseHistoryProvider);


namespace UE { namespace PoseSearch {

DEFINE_LOG_CATEGORY(LogPoseSearch);

//////////////////////////////////////////////////////////////////////////
// Constants and utilities

constexpr float DrawDebugLineThickness = 2.0f;
constexpr float DrawDebugPointSize = 3.0f;
constexpr float DrawDebugVelocityScale = 0.08f;
constexpr float DrawDebugArrowSize = 30.0f;
constexpr float DrawDebugSphereSize = 3.0f;
constexpr int32 DrawDebugSphereSegments = 8;
constexpr float DrawDebugSphereLineThickness = 0.5f;

static bool IsSamplingRangeValid(FFloatInterval Range)
{
	return Range.IsValid() && (Range.Min >= 0.0f);
}

static FFloatInterval GetEffectiveSamplingRange(const UAnimSequenceBase* Sequence, FFloatInterval SamplingRange)
{
	const bool bSampleAll = (SamplingRange.Min == 0.0f) && (SamplingRange.Max == 0.0f);
	const float SequencePlayLength = Sequence->GetPlayLength();

	FFloatInterval Range;
	Range.Min = bSampleAll ? 0.0f : SamplingRange.Min;
	Range.Max = bSampleAll ? SequencePlayLength : FMath::Min(SequencePlayLength, SamplingRange.Max);
	return Range;
}

static inline float CompareFeatureVectors(int32 NumValues, const float* A, const float* B, const float* Weights)
{
	double Dissimilarity = 0.f;

	for (int32 ValueIdx = 0; ValueIdx != NumValues; ++ValueIdx)
	{
		const float Diff = A[ValueIdx] - B[ValueIdx];
		Dissimilarity += Weights[ValueIdx] * (Diff * Diff);
	}

	return (float)Dissimilarity;
}

static inline float CompareFeatureVectors(int32 NumValues, const float* A, const float* B)
{
	double Dissimilarity = 0.f;

	for (int32 ValueIdx = 0; ValueIdx != NumValues; ++ValueIdx)
	{
		const float Diff = A[ValueIdx] - B[ValueIdx];
		Dissimilarity += Diff * Diff;
	}

	return (float)Dissimilarity;
}

FLinearColor GetColorForFeature(FPoseSearchFeatureDesc Feature, const FPoseSearchFeatureVectorLayout* Layout)
{
	const float FeatureIdx = Layout->Features.IndexOfByKey(Feature);
	const float FeatureCountIdx = Layout->Features.Num() - 1;
	const float FeatureCountIdxHalf = FeatureCountIdx / 2.f;
	check(FeatureIdx != INDEX_NONE);

	const float Hue = FeatureIdx < FeatureCountIdxHalf
		? FMath::GetMappedRangeValueUnclamped({ 0.f, FeatureCountIdxHalf }, FVector2f(60.f, 0.f), FeatureIdx)
		: FMath::GetMappedRangeValueUnclamped({ FeatureCountIdxHalf, FeatureCountIdx }, FVector2f(280.f, 220.f), FeatureIdx);

	const FLinearColor ColorHSV(Hue, 1.f, 1.f);
	return ColorHSV.HSVToLinearRGB();
}

/**
* Algo::LowerBound adapted to TIndexedContainerIterator for use with indexable but not necessarily contiguous containers. Used here with TRingBuffer.
*
* Performs binary search, resulting in position of the first element >= Value using predicate
*
* @param First TIndexedContainerIterator beginning of range to search through, must be already sorted by SortPredicate
* @param Last TIndexedContainerIterator end of range
* @param Value Value to look for
* @param SortPredicate Predicate for sort comparison, defaults to <
*
* @returns Position of the first element >= Value, may be position after last element in range
*/
template <typename IteratorType, typename ValueType, typename ProjectionType, typename SortPredicateType>
FORCEINLINE auto LowerBound(IteratorType First, IteratorType Last, const ValueType& Value, ProjectionType Projection, SortPredicateType SortPredicate) -> decltype(First.GetIndex())
{
	using SizeType = decltype(First.GetIndex());

	check(First.GetIndex() <= Last.GetIndex());

	// Current start of sequence to check
	SizeType Start = First.GetIndex();

	// Size of sequence to check
	SizeType Size = Last.GetIndex() - Start;

	// With this method, if Size is even it will do one more comparison than necessary, but because Size can be predicted by the CPU it is faster in practice
	while (Size > 0)
	{
		const SizeType LeftoverSize = Size % 2;
		Size = Size / 2;

		const SizeType CheckIndex = Start + Size;
		const SizeType StartIfLess = CheckIndex + LeftoverSize;

		auto&& CheckValue = Invoke(Projection, *(First + CheckIndex));
		Start = SortPredicate(CheckValue, Value) ? StartIfLess : Start;
	}
	return Start;
}

template <typename IteratorType, typename ValueType, typename SortPredicateType>
FORCEINLINE auto LowerBound(IteratorType First, IteratorType Last, const ValueType& Value, SortPredicateType SortPredicate) -> decltype(First.GetIndex())
{
	return LowerBound(First, Last, Value, FIdentityFunctor(), SortPredicate);
}

template <typename IteratorType, typename ValueType>
FORCEINLINE auto LowerBound(IteratorType First, IteratorType Last, const ValueType& Value) -> decltype(First.GetIndex())
{
	return LowerBound(First, Last, Value, FIdentityFunctor(), TLess<>());
}


// Stopgap channel indices since schemas don't yet support explicit channels of data
constexpr int32 ChannelIdxPose = 0;
constexpr int32 ChannelIdxTrajectoryTime = 1;
constexpr int32 ChannelIdxTrajectoryDistance = 2;


//////////////////////////////////////////////////////////////////////////
// FFeatureTypeTraits

struct FFeatureTypeTraits
{
	EPoseSearchFeatureType Type = EPoseSearchFeatureType::Invalid;
	uint32 NumFloats = 0;
};

// Could upgrade to class objects in the future with value reader/writer functions
static constexpr FFeatureTypeTraits FeatureTypeTraits[] =
{
	{ EPoseSearchFeatureType::Position, 3 },
	{ EPoseSearchFeatureType::Rotation, 6 },
	{ EPoseSearchFeatureType::LinearVelocity, 3 },
	{ EPoseSearchFeatureType::AngularVelocity, 3 },
};

FFeatureTypeTraits GetFeatureTypeTraits(EPoseSearchFeatureType Type)
{
	// Could allow external registration to a TSet of traits in the future
	// For now just use a simple local array
	for (const FFeatureTypeTraits& Traits : FeatureTypeTraits)
	{
		if (Traits.Type == Type)
		{
			return Traits;
		}
	}

	return FFeatureTypeTraits();
}

}} // namespace UE::PoseSearch


//////////////////////////////////////////////////////////////////////////
// FPoseSearchFeatureDesc

bool FPoseSearchFeatureDesc::operator==(const FPoseSearchFeatureDesc& Other) const
{
	return
		(SchemaBoneIdx == Other.SchemaBoneIdx) &&
		(SubsampleIdx == Other.SubsampleIdx) &&
		(Type == Other.Type) &&
		(Domain == Other.Domain);
}


//////////////////////////////////////////////////////////////////////////
// FPoseSearchFeatureVectorLayout

void FPoseSearchFeatureVectorLayout::Init()
{
	uint32 FloatCount = 0;

	for (FPoseSearchFeatureDesc& Feature : Features)
	{
		Feature.ValueOffset = FloatCount;

		uint32 FeatureNumFloats = UE::PoseSearch::GetFeatureTypeTraits(Feature.Type).NumFloats;
		FloatCount += FeatureNumFloats;
	}

	NumFloats = FloatCount;
}

void FPoseSearchFeatureVectorLayout::Reset()
{
	Features.Reset();
	NumFloats = 0;
}

bool FPoseSearchFeatureVectorLayout::IsValid(int32 MaxNumBones) const
{
	if (NumFloats == 0)
	{
		return false;
	}

	for (const FPoseSearchFeatureDesc& Feature : Features)
	{
		if (Feature.SchemaBoneIdx >= MaxNumBones)
		{
			return false;
		}
	}

	return true;
}

bool FPoseSearchFeatureVectorLayout::EnumerateBy(int32 ChannelIdx, EPoseSearchFeatureType Type, int32& InOutFeatureIdx) const
{
	auto IsChannelMatch = [](int32 ChannelIdx, const FPoseSearchFeatureDesc& Feature) -> bool
	{
		bool bChannelMatch = true;
		if (ChannelIdx == UE::PoseSearch::ChannelIdxPose)
		{
			bChannelMatch = Feature.SchemaBoneIdx != FPoseSearchFeatureDesc::TrajectoryBoneIndex;
		}
		else if (ChannelIdx == UE::PoseSearch::ChannelIdxTrajectoryTime)
		{
			bChannelMatch = (Feature.SchemaBoneIdx == FPoseSearchFeatureDesc::TrajectoryBoneIndex) && (Feature.Domain == EPoseSearchFeatureDomain::Time);
		}
		else if (ChannelIdx == UE::PoseSearch::ChannelIdxTrajectoryDistance)
		{
			bChannelMatch = (Feature.SchemaBoneIdx == FPoseSearchFeatureDesc::TrajectoryBoneIndex) && (Feature.Domain == EPoseSearchFeatureDomain::Distance);
		}
		return bChannelMatch;
	};

	auto IsTypeMatch = [](EPoseSearchFeatureType Type, const FPoseSearchFeatureDesc& Feature) -> bool
	{
		bool bTypeMatch = true;
		if (Type != EPoseSearchFeatureType::Invalid)
		{
			bTypeMatch = Feature.Type == Type;
		}
		return bTypeMatch;
	};

	for (int32 Size = Features.Num(); ++InOutFeatureIdx < Size; )
	{
		const FPoseSearchFeatureDesc& Feature = Features[InOutFeatureIdx];

		bool bChannelMatch = IsChannelMatch(ChannelIdx, Feature);
		bool bTypeMatch = IsTypeMatch(Type, Feature);

		if (bChannelMatch && bTypeMatch)
		{
			return true;
		}
	}

	return false;
}

//////////////////////////////////////////////////////////////////////////
// UPoseSearchSchema

void UPoseSearchSchema::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	SampleRate = FMath::Clamp(SampleRate, 1, 60);
	SamplingInterval = 1.0f / SampleRate;

	PoseSampleTimes.Sort(TLess<>());
	TrajectorySampleTimes.Sort(TLess<>());
	TrajectorySampleDistances.Sort(TLess<>());

	GenerateLayout();
	ResolveBoneReferences();

	EffectiveDataPreprocessor = DataPreprocessor;
	if (EffectiveDataPreprocessor == EPoseSearchDataPreprocessor::Automatic)
	{
		EffectiveDataPreprocessor = EPoseSearchDataPreprocessor::Normalize;
	}

	Super::PreSave(ObjectSaveContext);
}

void UPoseSearchSchema::PostLoad()
{
	Super::PostLoad();

	ResolveBoneReferences();
}

bool UPoseSearchSchema::IsValid() const
{
	bool bValid = Skeleton != nullptr;

	for (const FBoneReference& BoneRef : Bones)
	{
		bValid &= BoneRef.HasValidSetup();
	}

	bValid &= (Bones.Num() == BoneIndices.Num());

	bValid &= Layout.IsValid(BoneIndices.Num());
	
	return bValid;
}

float UPoseSearchSchema::GetTrajectoryFutureTimeHorizon () const
{
	return TrajectorySampleTimes.Num() ? TrajectorySampleTimes.Last() : -1.0f;
}

float UPoseSearchSchema::GetTrajectoryPastTimeHorizon () const
{
	return TrajectorySampleTimes.Num() ? TrajectorySampleTimes[0] : 1.0f;
}

float UPoseSearchSchema::GetTrajectoryFutureDistanceHorizon () const
{
	return TrajectorySampleDistances.Num() ? TrajectorySampleDistances.Last() : -1.0f;
}

float UPoseSearchSchema::GetTrajectoryPastDistanceHorizon () const
{
	return TrajectorySampleDistances.Num() ? TrajectorySampleDistances[0] : 1.0f;
}

TArrayView<const float> UPoseSearchSchema::GetChannelSampleOffsets (int32 ChannelIdx) const
{
	if (ChannelIdx == UE::PoseSearch::ChannelIdxPose)
	{
		return PoseSampleTimes;
	}
	else if (ChannelIdx == UE::PoseSearch::ChannelIdxTrajectoryTime)
	{
		return TrajectorySampleTimes;
	}
	else if (ChannelIdx == UE::PoseSearch::ChannelIdxTrajectoryDistance)
	{
		return TrajectorySampleDistances;
	}

	return {};
}

void UPoseSearchSchema::GenerateLayout()
{
	Layout.Reset();

	// Time domain trajectory positions
	if (bUseTrajectoryPositions && TrajectorySampleTimes.Num())
	{
		FPoseSearchFeatureDesc Feature;
		Feature.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;
		Feature.Domain = EPoseSearchFeatureDomain::Time;
		Feature.Type = EPoseSearchFeatureType::Position;
		for (Feature.SubsampleIdx = 0; Feature.SubsampleIdx != TrajectorySampleTimes.Num(); ++Feature.SubsampleIdx)
		{
			Layout.Features.Add(Feature);
		}
	}

	// Time domain trajectory linear velocities
	if (bUseTrajectoryVelocities && TrajectorySampleTimes.Num())
	{
		FPoseSearchFeatureDesc Feature;
		Feature.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;
		Feature.Domain = EPoseSearchFeatureDomain::Time;
		Feature.Type = EPoseSearchFeatureType::LinearVelocity;
		for (Feature.SubsampleIdx = 0; Feature.SubsampleIdx != TrajectorySampleTimes.Num(); ++Feature.SubsampleIdx)
		{
			Layout.Features.Add(Feature);
		}
	}

	// Distance domain trajectory positions
	if (bUseTrajectoryPositions && TrajectorySampleDistances.Num())
	{
		FPoseSearchFeatureDesc Feature;
		Feature.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;
		Feature.Domain = EPoseSearchFeatureDomain::Distance;
		Feature.Type = EPoseSearchFeatureType::Position;
		for (Feature.SubsampleIdx = 0; Feature.SubsampleIdx != TrajectorySampleDistances.Num(); ++Feature.SubsampleIdx)
		{
			Layout.Features.Add(Feature);
		}
	}

	// Distance domain trajectory linear velocities
	if (bUseTrajectoryVelocities && TrajectorySampleDistances.Num())
	{
		FPoseSearchFeatureDesc Feature;
		Feature.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;
		Feature.Domain = EPoseSearchFeatureDomain::Distance;
		Feature.Type = EPoseSearchFeatureType::LinearVelocity;
		for (Feature.SubsampleIdx = 0; Feature.SubsampleIdx != TrajectorySampleDistances.Num(); ++Feature.SubsampleIdx)
		{
			Layout.Features.Add(Feature);
		}
	}

	// Time domain bone positions
	if (bUseBonePositions && PoseSampleTimes.Num())
	{
		FPoseSearchFeatureDesc Feature;
		Feature.Domain = EPoseSearchFeatureDomain::Time;
		Feature.Type = EPoseSearchFeatureType::Position;
		for (Feature.SubsampleIdx = 0; Feature.SubsampleIdx != PoseSampleTimes.Num(); ++Feature.SubsampleIdx)
		{
			for (Feature.SchemaBoneIdx = 0; Feature.SchemaBoneIdx != Bones.Num(); ++Feature.SchemaBoneIdx)
			{
				Layout.Features.Add(Feature);
			}
		}
	}

	// Time domain bone linear velocities
	if (bUseBoneVelocities && PoseSampleTimes.Num())
	{
		FPoseSearchFeatureDesc Feature;
		Feature.Domain = EPoseSearchFeatureDomain::Time;
		Feature.Type = EPoseSearchFeatureType::LinearVelocity;
		for (Feature.SubsampleIdx = 0; Feature.SubsampleIdx != PoseSampleTimes.Num(); ++Feature.SubsampleIdx)
		{
			for (Feature.SchemaBoneIdx = 0; Feature.SchemaBoneIdx != Bones.Num(); ++Feature.SchemaBoneIdx)
			{
				Layout.Features.Add(Feature);
			}
		}
	}

	Layout.Init();
}

void UPoseSearchSchema::ResolveBoneReferences()
{
	// Initialize references to obtain bone indices
	for (FBoneReference& BoneRef : Bones)
	{
		BoneRef.Initialize(Skeleton);
	}

	// Fill out bone index array and sort by bone index
	BoneIndices.SetNum(Bones.Num());
	for (int32 Index = 0; Index != Bones.Num(); ++Index)
	{
		BoneIndices[Index] = Bones[Index].BoneIndex;
	}
	BoneIndices.Sort();

	// Build separate index array with parent indices guaranteed to be present
	BoneIndicesWithParents = BoneIndices;
	if (Skeleton)
	{
		FAnimationRuntime::EnsureParentsPresent(BoneIndicesWithParents, Skeleton->GetReferenceSkeleton());
	}
}


//////////////////////////////////////////////////////////////////////////
// FPoseSearchChannelWeightParams

FPoseSearchChannelWeightParams::FPoseSearchChannelWeightParams()
{
	for (int32 Type = 0; Type != (int32)EPoseSearchFeatureType::Num; ++Type)
	{
		TypeWeights.Add((EPoseSearchFeatureType)Type, 1.0f);
	}
}


//////////////////////////////////////////////////////////////////////////
// FPoseSearchWeights

void FPoseSearchWeights::Init(const FPoseSearchWeightParams& WeightParams, const UPoseSearchSchema* Schema, const FPoseSearchDynamicWeightParams& DynamicWeightParams)
{
	using namespace UE::PoseSearch;
	using namespace Eigen;

	// Convenience enum for indexing by horizon
	enum EHorizon : int
	{
		History,
		Prediction,
		Num
	};

	// Initialize weights
	Weights.Reset();
	Weights.SetNumZeroed(Schema->Layout.NumFloats);

	// Completely disable weights if requested
	if (DynamicWeightParams.bDebugDisableWeights)
	{
		for (float& Weight: Weights)
		{
			Weight = 1.0f;
		}
		return;
	}

	
	FMemMark MemMark(FMemStack::Get());


	// Setup channel indexable weight params
	constexpr int ChannelNum = 3;
	const FPoseSearchChannelWeightParams* ChannelWeightParams[ChannelNum];
	const FPoseSearchChannelDynamicWeightParams* ChannelDynamicWeightParams[ChannelNum];

	ChannelWeightParams[ChannelIdxPose] = &WeightParams.PoseWeight;
	ChannelWeightParams[ChannelIdxTrajectoryTime] = &WeightParams.TrajectoryWeight;
	ChannelWeightParams[ChannelIdxTrajectoryDistance] = &WeightParams.TrajectoryWeight;

	ChannelDynamicWeightParams[ChannelIdxPose] = &DynamicWeightParams.PoseDynamicWeights;
	ChannelDynamicWeightParams[ChannelIdxTrajectoryTime] = &DynamicWeightParams.TrajectoryDynamicWeights;
	ChannelDynamicWeightParams[ChannelIdxTrajectoryDistance] = &DynamicWeightParams.TrajectoryDynamicWeights;


	// Normalize channel weights
	Array<float, ChannelNum, 1> NormalizedChannelWeights;
	for (int ChannelIdx = 0; ChannelIdx != ChannelNum; ++ChannelIdx)
	{
		NormalizedChannelWeights[ChannelIdx] = ChannelWeightParams[ChannelIdx]->ChannelWeight * ChannelDynamicWeightParams[ChannelIdx]->ChannelWeightScale;

		// Zero the channel weight if there are no features in this channel
		int32 FeatureIdx = INDEX_NONE;
		if (!Schema->Layout.EnumerateBy(ChannelIdx, EPoseSearchFeatureType::Invalid, FeatureIdx))
		{
			NormalizedChannelWeights[ChannelIdx] = 0.0f;
		}
	}

	const float ChannelWeightSum = NormalizedChannelWeights.sum();
	if (!FMath::IsNearlyZero(ChannelWeightSum))
	{
		NormalizedChannelWeights *= (1.0f / ChannelWeightSum);
	}	


	// Determine maximum number of channel sample offsets for allocation
	int32 MaxChannelSampleOffsets = 0;
	for (int ChannelIdx = 0; ChannelIdx != ChannelNum; ++ChannelIdx)
	{
		TArrayView<const float> ChannelSampleOffsets = Schema->GetChannelSampleOffsets(ChannelIdx);
		MaxChannelSampleOffsets = FMath::Max(MaxChannelSampleOffsets, ChannelSampleOffsets.Num());
	}

	// WeightsByFeature is indexed by FeatureIdx in a Layout
	TArray<float, TMemStackAllocator<>> WeightsByFeatureStorage;
	WeightsByFeatureStorage.SetNum(Schema->Layout.Features.Num());
	Map<ArrayXf> WeightsByFeature(WeightsByFeatureStorage.GetData(), WeightsByFeatureStorage.Num());

	// HorizonWeightsBySample is indexed by the channel's sample offsets in a Schema
	TArray<float, TMemStackAllocator<>> HorizonWeightsBySampleStorage;
	HorizonWeightsBySampleStorage.SetNum(MaxChannelSampleOffsets);
	Map<ArrayXf> HorizonWeightsBySample(HorizonWeightsBySampleStorage.GetData(), HorizonWeightsBySampleStorage.Num());

	// WeightsByType is indexed by feature type
	Array<float, (int)EPoseSearchFeatureType::Num, 1> WeightsByType;


	// Determine each channel's feature weights
	for (int ChannelIdx = 0; ChannelIdx != ChannelNum; ++ChannelIdx)
	{
		// Ignore this channel entirely if it has no weight
		if (FMath::IsNearlyZero(NormalizedChannelWeights[ChannelIdx]))
		{
			continue;
		}

		// Get channel info
		const FPoseSearchChannelWeightParams& ChannelWeights = *ChannelWeightParams[ChannelIdx];
		const FPoseSearchChannelDynamicWeightParams& ChannelDynamicWeights = *ChannelDynamicWeightParams[ChannelIdx];
		TArrayView<const float> ChannelSampleOffsets = Schema->GetChannelSampleOffsets(ChannelIdx);

		// Reset scratch weights
		WeightsByFeature.setConstant(0.0f);
		WeightsByType.setConstant(0.0f);
		HorizonWeightsBySample.setConstant(0.0f);


		// Initialize weights by type lookup
		for (int Type = 0; Type != (int)EPoseSearchFeatureType::Num; ++Type)
		{
			WeightsByType[Type] = ChannelWeights.TypeWeights.FindRef((EPoseSearchFeatureType)Type);

			// Zero the weight if this channel doesn't have any features using this type
			int32 FeatureIdx = INDEX_NONE;
			if (!Schema->Layout.EnumerateBy(ChannelIdx, (EPoseSearchFeatureType)Type, FeatureIdx))
			{
				WeightsByType[Type] = 0.0f;
			}
		}

		// Normalize type weights
		float TypeWeightsSum = WeightsByType.sum();
		if (!FMath::IsNearlyZero(TypeWeightsSum))
		{
			WeightsByType *= (1.0f / TypeWeightsSum);
		}
		else
		{
			// Ignore this channel entirely if there are no types contributing weight
			continue;
		}


		// Determine the range of sample offsets that make up the history and prediction horizons
		FInt32Range HorizonSampleIdxRanges[EHorizon::Num];
		{
			int32 IdxUpper = Algo::UpperBound(ChannelSampleOffsets, 0.0f);
			int32 IdxLower = ChannelSampleOffsets[0] <= 0.0f ? 0 : IdxUpper;
			HorizonSampleIdxRanges[EHorizon::History] = FInt32Range(IdxLower, IdxUpper);

			IdxLower = IdxUpper;
			IdxUpper = ChannelSampleOffsets.Num();
			HorizonSampleIdxRanges[EHorizon::Prediction] = FInt32Range(IdxLower, IdxUpper);
		}


		// Initialize horizon weights
		Array<float, 1, EHorizon::Num> NormalizedHorizonWeights;
		NormalizedHorizonWeights.setConstant(0.0f);

		if (!HorizonSampleIdxRanges[EHorizon::History].IsEmpty())
		{
			NormalizedHorizonWeights[EHorizon::History] = ChannelWeights.HistoryParams.Weight * ChannelDynamicWeights.HistoryWeightScale;
		}
		if (!HorizonSampleIdxRanges[EHorizon::Prediction].IsEmpty())
		{
			NormalizedHorizonWeights[EHorizon::Prediction] = ChannelWeights.PredictionParams.Weight * ChannelDynamicWeights.PredictionWeightScale;
		}
		
		// Normalize horizon weights
		float HorizonWeightSum = NormalizedHorizonWeights.sum();
		if (!FMath::IsNearlyZero(HorizonWeightSum))
		{
			NormalizedHorizonWeights *= (1.0f / HorizonWeightSum);
		}
		else
		{
			// Ignore this channel entirely if the horizons don't contribute any weight
			continue;
		}


		auto SetHorizonSampleWeights = [&HorizonWeightsBySample, &ChannelSampleOffsets](FInt32Range SampleIdxRange, const FPoseSearchChannelHorizonParams& HorizonParams)
		{
			// Segment length represents the number of sample offsets in the span that make up this horizon
			int32 SegmentLength = SampleIdxRange.Size<int32>();

			if (SegmentLength > 0)
			{
				int32 SegmentBegin = SampleIdxRange.GetLowerBoundValue();
				if (HorizonParams.bInterpolate && SegmentLength > 1)
				{
					// We'll map the range spanned by the horizon's sample offsets to the interpolation range
					// The interpolation range is 0 to 1 unless an initial value was set
					// The initial value allows the user to set a minimum weight or reverse the lerp direction
					// We'll normalize these weights in the next step
					FVector2f InputRange(ChannelSampleOffsets[SegmentBegin], ChannelSampleOffsets[SegmentBegin + SegmentLength - 1]);
					FVector2f OutputRange(HorizonParams.InitialValue, 1.0f - HorizonParams.InitialValue);

					for (int32 OffsetIdx = SegmentBegin; OffsetIdx != SegmentBegin + SegmentLength; ++OffsetIdx)
					{
						float SampleOffset = ChannelSampleOffsets[OffsetIdx];
						float Alpha = FMath::GetMappedRangeValueUnclamped(InputRange, OutputRange, SampleOffset);
						float Weight = FAlphaBlend::AlphaToBlendOption(Alpha, HorizonParams.InterpolationMethod);
						HorizonWeightsBySample[OffsetIdx] = Weight;
					}
				}
				else
				{
					// If we're not interpolating weights across this horizon, just give them all equal weight
					HorizonWeightsBySample.segment(SegmentBegin, SegmentLength).setConstant(1.0f);
				}

				// Normalize weights within the horizon's segment of sample offsets
				float HorizonSum = HorizonWeightsBySample.segment(SegmentBegin, SegmentLength).sum();
				if (!FMath::IsNearlyZero(HorizonSum))
				{
					HorizonWeightsBySample.segment(SegmentBegin, SegmentLength) *= 1.0f / HorizonSum;
				}
			}
		};

		SetHorizonSampleWeights(HorizonSampleIdxRanges[EHorizon::History], ChannelWeights.HistoryParams);
		SetHorizonSampleWeights(HorizonSampleIdxRanges[EHorizon::Prediction], ChannelWeights.PredictionParams);


		// Now set this channel's weights for every feature in each horizon
		Array<float, 1, EHorizon::Num> HorizonSums;
		HorizonSums = 0.0f;
		for (int FeatureIdx = INDEX_NONE; Schema->Layout.EnumerateBy(ChannelIdx, EPoseSearchFeatureType::Invalid, FeatureIdx); /*empty*/)
		{
			const FPoseSearchFeatureDesc& Feature = Schema->Layout.Features[FeatureIdx];

			for (int HorizonIdx = 0; HorizonIdx != EHorizon::Num; ++HorizonIdx)
			{
				if (HorizonSampleIdxRanges[HorizonIdx].Contains(Feature.SubsampleIdx))
				{
					int HorizonSize = HorizonSampleIdxRanges[HorizonIdx].Size<int>();
					WeightsByFeature[FeatureIdx] = HorizonWeightsBySample[Feature.SubsampleIdx] * (HorizonSize * WeightsByType[(int)Feature.Type]);
					HorizonSums[HorizonIdx] += WeightsByFeature[FeatureIdx];
					break;
				}
			}
		}

		// Scale feature weights within horizons so that they have the desired total horizon weight
		for (int FeatureIdx = INDEX_NONE; Schema->Layout.EnumerateBy(ChannelIdx, EPoseSearchFeatureType::Invalid, FeatureIdx); /*empty*/)
		{
			const FPoseSearchFeatureDesc& Feature = Schema->Layout.Features[FeatureIdx];

			for (int HorizonIdx = 0; HorizonIdx != EHorizon::Num; ++HorizonIdx)
			{
				if (HorizonSampleIdxRanges[HorizonIdx].Contains(Feature.SubsampleIdx))
				{
					float HorizonWeight = NormalizedHorizonWeights[HorizonIdx] / HorizonSums[HorizonIdx];
					WeightsByFeature[FeatureIdx] *= HorizonWeight;
					break;
				}
			}
		}

		// Scale all features in all horizons so they have the desired channel weight
		WeightsByFeature *= NormalizedChannelWeights[ChannelIdx];

		// Weights should sum to channel weight at this point
		ensure(FMath::IsNearlyEqual(WeightsByFeature.sum(), NormalizedChannelWeights[ChannelIdx], KINDA_SMALL_NUMBER));

		// Merge feature weights for channel into per-value weights buffer
		// Weights are replicated per feature dimension so the cost function can directly index weights by value index
		for (int FeatureIdx = INDEX_NONE; Schema->Layout.EnumerateBy(ChannelIdx, EPoseSearchFeatureType::Invalid, FeatureIdx); /*empty*/)
		{
			const FPoseSearchFeatureDesc& Feature = Schema->Layout.Features[FeatureIdx];
			int32 ValueSize = GetFeatureTypeTraits(Feature.Type).NumFloats;
			int32 ValueTerm = Feature.ValueOffset + ValueSize;
			for (int32 ValueIdx = Feature.ValueOffset; ValueIdx != ValueTerm; ++ValueIdx)
			{
				Weights[ValueIdx] = WeightsByFeature[FeatureIdx];
			}
		}
	}
}


//////////////////////////////////////////////////////////////////////////
// FPoseSearchWeightsContext

void FPoseSearchWeightsContext::Update(const FPoseSearchDynamicWeightParams& ActiveWeights, const UPoseSearchDatabase* ActiveDatabase)
{
	bool bRecomputeWeights = false;
	if (Database != ActiveDatabase)
	{
		Database = ActiveDatabase;
		bRecomputeWeights = true;
	}

	if (DynamicWeights != ActiveWeights)
	{
		DynamicWeights = ActiveWeights;
		bRecomputeWeights = true;
	}

	if (bRecomputeWeights)
	{
		int32 NumGroups = ActiveDatabase ? 1 : 0;
		ComputedGroupWeights.SetNum(NumGroups);
		for (FPoseSearchWeights& GroupWeights: ComputedGroupWeights)
		{
			GroupWeights.Init(Database->Weights, Database->Schema, ActiveWeights);
		}
	}
}

const FPoseSearchWeights* FPoseSearchWeightsContext::GetGroupWeights(int32 WeightsGroupIdx) const
{
	if (ComputedGroupWeights.IsValidIndex(WeightsGroupIdx))
	{
		return &ComputedGroupWeights[WeightsGroupIdx];
	}

	return nullptr;
}


//////////////////////////////////////////////////////////////////////////
// FPoseSearchIndex

bool FPoseSearchIndex::IsValid() const
{
	bool bSchemaValid = Schema && Schema->IsValid();
	bool bSearchIndexValid = bSchemaValid && (NumPoses * Schema->Layout.NumFloats == Values.Num());

	return bSearchIndexValid;
}

TArrayView<const float> FPoseSearchIndex::GetPoseValues(int32 PoseIdx) const
{
	check(PoseIdx < NumPoses);
	int32 ValueOffset = PoseIdx * Schema->Layout.NumFloats;
	return MakeArrayView(&Values[ValueOffset], Schema->Layout.NumFloats);
}

void FPoseSearchIndex::Reset()
{
	NumPoses = 0;
	Values.Reset();
	Schema = nullptr;
}

void FPoseSearchIndex::Normalize(TArrayView<float> InOutPoseVector) const
{
	using namespace Eigen;

	auto TransformationMtx = Map<const Matrix<float, Dynamic, Dynamic, ColMajor>>
	(
		PreprocessInfo.TransformationMatrix.GetData(),
		PreprocessInfo.NumDimensions,
		PreprocessInfo.NumDimensions
	);
	auto SampleMean = Map<const Matrix<float, Dynamic, 1, ColMajor>>
	(
		PreprocessInfo.SampleMean.GetData(),
		PreprocessInfo.NumDimensions
	);

	checkSlow(InOutPoseVector.Num() == PreprocessInfo.NumDimensions);

	auto PoseVector = Map<Matrix<float, Dynamic, 1, ColMajor>>
	(
		InOutPoseVector.GetData(),
		InOutPoseVector.Num()
	);

	PoseVector = TransformationMtx * (PoseVector - SampleMean);
}

void FPoseSearchIndex::InverseNormalize(TArrayView<float> InOutNormalizedPoseVector) const
{
	using namespace Eigen;

	auto InverseTransformationMtx = Map<const Matrix<float, Dynamic, Dynamic, ColMajor>>
	(
		PreprocessInfo.InverseTransformationMatrix.GetData(),
		PreprocessInfo.NumDimensions,
		PreprocessInfo.NumDimensions
	);
	auto SampleMean = Map<const Matrix<float, Dynamic, 1, ColMajor>>
	(
		PreprocessInfo.SampleMean.GetData(),
		PreprocessInfo.NumDimensions
	);

	checkSlow(InOutNormalizedPoseVector.Num() == PreprocessInfo.NumDimensions);

	auto NormalizedPoseVector = Map<Matrix<float, Dynamic, 1, ColMajor>>
	(
		InOutNormalizedPoseVector.GetData(),
		InOutNormalizedPoseVector.Num()
	);

	NormalizedPoseVector = (InverseTransformationMtx * NormalizedPoseVector) + SampleMean;
}


//////////////////////////////////////////////////////////////////////////
// UPoseSearchSequenceMetaData

void UPoseSearchSequenceMetaData::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	SearchIndex.Reset();

#if WITH_EDITOR
	if (!IsTemplate())
	{
		if (IsValidForIndexing())
		{
			UObject* Outer = GetOuter();
			if (UAnimSequence* Sequence = Cast<UAnimSequence>(Outer))
			{
				UE::PoseSearch::BuildIndex(Sequence, this);
			}
		}
	}
#endif

	Super::PreSave(ObjectSaveContext);
}

bool UPoseSearchSequenceMetaData::IsValidForIndexing() const
{
	return Schema && Schema->IsValid() && UE::PoseSearch::IsSamplingRangeValid(SamplingRange);
}

bool UPoseSearchSequenceMetaData::IsValidForSearch() const
{
	return IsValidForIndexing() && SearchIndex.IsValid();
}


//////////////////////////////////////////////////////////////////////////
// UPoseSearchDatabase

int32 UPoseSearchDatabase::FindSequenceForPose(int32 PoseIdx) const
{
	auto Predicate = [PoseIdx](const FPoseSearchDatabaseSequence& DbSequence)
	{
		return (PoseIdx >= DbSequence.FirstPoseIdx) && (PoseIdx < DbSequence.FirstPoseIdx + DbSequence.NumPoses);
	};

	return Sequences.IndexOfByPredicate(Predicate);
}

int32 UPoseSearchDatabase::GetPoseIndexFromAssetTime(int32 DbSequenceIdx, float AssetTime) const
{
	const FPoseSearchDatabaseSequence& DbSequence = Sequences[DbSequenceIdx];
	FFloatInterval Range = UE::PoseSearch::GetEffectiveSamplingRange(DbSequence.Sequence, DbSequence.SamplingRange);
	if (Range.Contains(AssetTime))
	{
		int32 PoseOffset = FMath::FloorToInt(Schema->SampleRate * (AssetTime - Range.Min));
		if (PoseOffset >= DbSequence.NumPoses)
		{
			if (DbSequence.bLoopAnimation)
			{
				PoseOffset -= DbSequence.NumPoses;
			}
			else
			{
				PoseOffset = DbSequence.NumPoses - 1;
			}
		}
		
		int32 PoseIdx = DbSequence.FirstPoseIdx + PoseOffset;
		return PoseIdx;
	}
	
	return INDEX_NONE;
}

FFloatInterval UPoseSearchDatabase::GetEffectiveSamplingRange(int32 DbSequenceIdx) const
{
	const FPoseSearchDatabaseSequence& DbSequence = Sequences[DbSequenceIdx];
	FFloatInterval Range = UE::PoseSearch::GetEffectiveSamplingRange(DbSequence.Sequence, DbSequence.SamplingRange);
	return Range;
}

float UPoseSearchDatabase::GetSequenceLength(int32 DbSequenceIdx) const
{
	return Sequences[DbSequenceIdx].Sequence->GetPlayLength();
}

bool UPoseSearchDatabase::DoesSequenceLoop(int32 DbSequenceIdx) const
{
	return Sequences[DbSequenceIdx].bLoopAnimation;
}

bool UPoseSearchDatabase::IsValidForIndexing() const
{
	bool bValid = Schema && Schema->IsValid() && !Sequences.IsEmpty();

	if (bValid)
	{
		bool bSequencesValid = true;
		for (const FPoseSearchDatabaseSequence& DbSequence : Sequences)
		{
			if (!DbSequence.Sequence)
			{
				bSequencesValid = false;
				break;
			}

			USkeleton* SeqSkeleton = DbSequence.Sequence->GetSkeleton();
			if (!SeqSkeleton || !SeqSkeleton->IsCompatible(Schema->Skeleton))
			{
				bSequencesValid = false;
				break;
			}
		}

		bValid = bSequencesValid;
	}

	return bValid;
}

bool UPoseSearchDatabase::IsValidForSearch() const
{
	return IsValidForIndexing() && SearchIndex.IsValid();
}

void UPoseSearchDatabase::CollectSimpleSequences()
{
	for (auto& SimpleSequence: SimpleSequences)
	{
		auto Predicate = [&SimpleSequence](FPoseSearchDatabaseSequence& DbSequence) -> bool
		{
			return DbSequence.Sequence == SimpleSequence;
		};

		if (!Sequences.ContainsByPredicate(Predicate))
		{
			FPoseSearchDatabaseSequence& DbSequence = Sequences.AddDefaulted_GetRef();
			DbSequence.Sequence = SimpleSequence;
		}
	}

	SimpleSequences.Reset();
}

void UPoseSearchDatabase::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	SearchIndex.Reset();

#if WITH_EDITOR
	if (!IsTemplate())
	{
		if (IsValidForIndexing())
		{
			UE::PoseSearch::BuildIndex(this);
		}
	}
#endif

	Super::PreSave(ObjectSaveContext);
}

#if WITH_EDITOR
void UPoseSearchDatabase::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UPoseSearchDatabase, SimpleSequences))
	{
		if (!SimpleSequences.IsEmpty())
		{
			CollectSimpleSequences();
		}
	}
}
#endif // WITH_EDITOR


//////////////////////////////////////////////////////////////////////////
// FPoseSearchFeatureVectorBuilder

void FPoseSearchFeatureVectorBuilder::Init(const UPoseSearchSchema* InSchema)
{
	check(InSchema && InSchema->IsValid());
	Schema = InSchema;
	ResetFeatures();
}

void FPoseSearchFeatureVectorBuilder::Reset()
{
	Schema = nullptr;
	Values.Reset(0);
	ValuesNormalized.Reset(0);
	NumFeaturesAdded = 0;
	FeaturesAdded.Reset();
}

void FPoseSearchFeatureVectorBuilder::ResetFeatures()
{
	Values.Reset(0);
	Values.SetNumZeroed(Schema->Layout.NumFloats);
	ValuesNormalized.Reset(0);
	ValuesNormalized.SetNumZeroed(Schema->Layout.NumFloats);
	NumFeaturesAdded = 0;
	FeaturesAdded.Init(false, Schema->Layout.Features.Num());
}

void FPoseSearchFeatureVectorBuilder::SetTransform(FPoseSearchFeatureDesc Element, const FTransform& Transform)
{
	SetPosition(Element, Transform.GetTranslation());
	SetRotation(Element, Transform.GetRotation());
}

void FPoseSearchFeatureVectorBuilder::SetTransformVelocity(FPoseSearchFeatureDesc Element, const FTransform& Transform, const FTransform& PrevTransform, float DeltaTime)
{
	SetLinearVelocity(Element, Transform, PrevTransform, DeltaTime);
	SetAngularVelocity(Element, Transform, PrevTransform, DeltaTime);
}

void FPoseSearchFeatureVectorBuilder::SetPosition(FPoseSearchFeatureDesc Element, const FVector& Position)
{
	Element.Type = EPoseSearchFeatureType::Position;
	SetVector(Element, Position);
}

void FPoseSearchFeatureVectorBuilder::SetRotation(FPoseSearchFeatureDesc Element, const FQuat& Rotation)
{
	Element.Type = EPoseSearchFeatureType::Rotation;
	int32 ElementIndex = Schema->Layout.Features.Find(Element);
	if (ElementIndex >= 0)
	{
		FVector X = Rotation.GetAxisX();
		FVector Y = Rotation.GetAxisY();

		const FPoseSearchFeatureDesc& FoundElement = Schema->Layout.Features[ElementIndex];

		Values[FoundElement.ValueOffset + 0] = X.X;
		Values[FoundElement.ValueOffset + 1] = X.Y;
		Values[FoundElement.ValueOffset + 2] = X.Z;
		Values[FoundElement.ValueOffset + 3] = Y.X;
		Values[FoundElement.ValueOffset + 4] = Y.Y;
		Values[FoundElement.ValueOffset + 5] = Y.Z;

		if (!FeaturesAdded[ElementIndex])
		{
			FeaturesAdded[ElementIndex] = true;
			++NumFeaturesAdded;
		}
	}
}

void FPoseSearchFeatureVectorBuilder::SetLinearVelocity(FPoseSearchFeatureDesc Element, const FTransform& Transform, const FTransform& PrevTransform, float DeltaTime)
{
	Element.Type = EPoseSearchFeatureType::LinearVelocity;
	FVector LinearVelocity = (Transform.GetTranslation() - PrevTransform.GetTranslation()) / DeltaTime;
	SetVector(Element, LinearVelocity);
}

void FPoseSearchFeatureVectorBuilder::SetAngularVelocity(FPoseSearchFeatureDesc Element, const FTransform& Transform, const FTransform& PrevTransform, float DeltaTime)
{
	Element.Type = EPoseSearchFeatureType::AngularVelocity;
	int32 ElementIndex = Schema->Layout.Features.Find(Element);
	if (ElementIndex >= 0)
	{
		FQuat Q0 = PrevTransform.GetRotation();
		FQuat Q1 = Transform.GetRotation();
		Q1.EnforceShortestArcWith(Q0);

		// Given angular velocity vector w, quaternion differentiation can be represented as
		//   dq/dt = (w * q)/2
		// Solve for w
		//   w = 2 * dq/dt * q^-1
		// And let dq/dt be expressed as the finite difference
		//   dq/dt = (q(t+h) - q(t)) / h
		FQuat DQDt = (Q1 - Q0) / DeltaTime;
		FQuat QInv = Q0.Inverse();
		FQuat W = (DQDt * QInv) * 2.0f;

		FVector AngularVelocity(W.X, W.Y, W.Z);

		const FPoseSearchFeatureDesc& FoundElement = Schema->Layout.Features[ElementIndex];

		Values[FoundElement.ValueOffset + 0] = AngularVelocity[0];
		Values[FoundElement.ValueOffset + 1] = AngularVelocity[1];
		Values[FoundElement.ValueOffset + 2] = AngularVelocity[2];

		if (!FeaturesAdded[ElementIndex])
		{
			FeaturesAdded[ElementIndex] = true;
			++NumFeaturesAdded;
		}
	}
}

void FPoseSearchFeatureVectorBuilder::SetVector(FPoseSearchFeatureDesc Element, const FVector& Vector)
{
	int32 ElementIndex = Schema->Layout.Features.Find(Element);
	if (ElementIndex >= 0)
	{
		const FPoseSearchFeatureDesc& FoundElement = Schema->Layout.Features[ElementIndex];

		Values[FoundElement.ValueOffset + 0] = Vector[0];
		Values[FoundElement.ValueOffset + 1] = Vector[1];
		Values[FoundElement.ValueOffset + 2] = Vector[2];

		if (!FeaturesAdded[ElementIndex])
		{
			FeaturesAdded[ElementIndex] = true;
			++NumFeaturesAdded;
		}
	}
}

bool FPoseSearchFeatureVectorBuilder::TrySetPoseFeatures(UE::PoseSearch::FPoseHistory* History)
{
	check(Schema.IsValid() && Schema->IsValid());
	check(History);

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = EPoseSearchFeatureDomain::Time;

	for (int32 SchemaSubsampleIdx = 0; SchemaSubsampleIdx != Schema->PoseSampleTimes.Num(); ++SchemaSubsampleIdx)
	{
		Feature.SubsampleIdx = SchemaSubsampleIdx;

		// Stop when we've reached future samples
		float SampleTime = Schema->PoseSampleTimes[SchemaSubsampleIdx];
		if (SampleTime > 0.0f)
		{
			break;
		}

		float SecondsAgo = -SampleTime;
		if (!History->TrySamplePose(SecondsAgo, Schema->Skeleton->GetReferenceSkeleton(), Schema->BoneIndicesWithParents))
		{
			return false;
		}

		TArrayView<const FTransform> ComponentPose = History->GetComponentPoseSample();
		TArrayView<const FTransform> ComponentPrevPose = History->GetPrevComponentPoseSample();
		for (int32 SchemaBoneIdx = 0; SchemaBoneIdx != Schema->BoneIndices.Num(); ++SchemaBoneIdx)
		{
			Feature.SchemaBoneIdx = SchemaBoneIdx;

			int32 SkeletonBoneIndex = Schema->BoneIndices[SchemaBoneIdx];
			const FTransform& Transform = ComponentPose[SkeletonBoneIndex];
			const FTransform& PrevTransform = ComponentPrevPose[SkeletonBoneIndex];
			SetTransform(Feature, Transform);
			SetTransformVelocity(Feature, Transform, PrevTransform, History->GetSampleTimeInterval());
		}
	}

	return true;
}

void FPoseSearchFeatureVectorBuilder::BuildFromTrajectoryTimeBased(const FTrajectorySampleRange& Trajectory)
{
	check(Schema.IsValid() && Schema->IsValid());

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = EPoseSearchFeatureDomain::Time;
	Feature.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;

	for (int32 Idx = 0, NextIterStartIdx = 0, Num = Schema->TrajectorySampleTimes.Num(); Idx < Num; ++Idx)
	{
		const float SampleTime = Schema->TrajectorySampleTimes[Idx];
		const FTrajectorySample Sample = FTrajectorySampleRange::IterSampleTrajectory(Trajectory.Samples, ETrajectorySampleDomain::Time, SampleTime, NextIterStartIdx);

		Feature.SubsampleIdx = Idx;

		Feature.Type = EPoseSearchFeatureType::LinearVelocity;
		SetVector(Feature, Sample.LocalLinearVelocity);

		Feature.Type = EPoseSearchFeatureType::Position;
		SetVector(Feature, Sample.Position);
	}
}

void FPoseSearchFeatureVectorBuilder::BuildFromTrajectoryDistanceBased(const FTrajectorySampleRange& Trajectory)
{
	check(Schema.IsValid() && Schema->IsValid());

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = EPoseSearchFeatureDomain::Distance;
	Feature.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;

	for (int32 Idx = 0, NextIterStartIdx = 0, Num = Schema->TrajectorySampleDistances.Num(); Idx < Num; ++Idx)
	{
		const float SampleDistance = Schema->TrajectorySampleDistances[Idx];
		const FTrajectorySample Sample = FTrajectorySampleRange::IterSampleTrajectory(Trajectory.Samples, ETrajectorySampleDomain::Distance, SampleDistance, NextIterStartIdx);

		Feature.SubsampleIdx = Idx;

		Feature.Type = EPoseSearchFeatureType::LinearVelocity;
		SetVector(Feature, Sample.LocalLinearVelocity);

		Feature.Type = EPoseSearchFeatureType::Position;
		SetVector(Feature, Sample.Position);
	} 
}

void FPoseSearchFeatureVectorBuilder::CopyFromSearchIndex(const FPoseSearchIndex& SearchIndex, int32 PoseIdx)
{
	check(Schema == SearchIndex.Schema);

	TArrayView<const float> FeatureVector = SearchIndex.GetPoseValues(PoseIdx);

	ValuesNormalized = FeatureVector;
	Values = FeatureVector;
	SearchIndex.InverseNormalize(Values);

	NumFeaturesAdded = Schema->Layout.Features.Num();
	FeaturesAdded.SetRange(0, FeaturesAdded.Num(), true);
}

void FPoseSearchFeatureVectorBuilder::CopyFeature(const FPoseSearchFeatureVectorBuilder& OtherBuilder, int32 FeatureIdx)
{
	check(IsCompatible(OtherBuilder));
	check(OtherBuilder.FeaturesAdded[FeatureIdx]);

	const FPoseSearchFeatureDesc& FeatureDesc = Schema->Layout.Features[FeatureIdx];
	const int32 FeatureNumFloats = UE::PoseSearch::GetFeatureTypeTraits(FeatureDesc.Type).NumFloats;
	const int32 FeatureValueOffset = FeatureDesc.ValueOffset;

	for(int32 FeatureValueIdx = FeatureValueOffset; FeatureValueIdx != FeatureValueOffset + FeatureNumFloats; ++FeatureValueIdx)
	{
		Values[FeatureValueIdx] = OtherBuilder.Values[FeatureValueIdx];
	}

	if (!FeaturesAdded[FeatureIdx])
	{
		FeaturesAdded[FeatureIdx] = true;
		++NumFeaturesAdded;
	}
}

void FPoseSearchFeatureVectorBuilder::MergeReplace(const FPoseSearchFeatureVectorBuilder& OtherBuilder)
{
	check(IsCompatible(OtherBuilder));

	for (TConstSetBitIterator<> Iter(OtherBuilder.FeaturesAdded); Iter; ++Iter)
	{
		CopyFeature(OtherBuilder, Iter.GetIndex());
	}
}

bool FPoseSearchFeatureVectorBuilder::IsInitialized() const
{
	return (Schema != nullptr) && (Values.Num() == Schema->Layout.NumFloats);
}

bool FPoseSearchFeatureVectorBuilder::IsInitializedForSchema(const UPoseSearchSchema* InSchema) const
{
	return (Schema == InSchema) && IsInitialized();
}

bool FPoseSearchFeatureVectorBuilder::IsComplete() const
{
	return NumFeaturesAdded == Schema->Layout.Features.Num();
}

bool FPoseSearchFeatureVectorBuilder::IsCompatible(const FPoseSearchFeatureVectorBuilder& OtherBuilder) const
{
	return IsInitialized() && (Schema == OtherBuilder.Schema);
}

void FPoseSearchFeatureVectorBuilder::Normalize(const FPoseSearchIndex& ForSearchIndex)
{
	ValuesNormalized = Values;
	ForSearchIndex.Normalize(ValuesNormalized);
}

void FPoseSearchFeatureVectorBuilder::BuildFromTrajectory(const FTrajectorySampleRange& Trajectory)
{
	BuildFromTrajectoryTimeBased(Trajectory);
	BuildFromTrajectoryDistanceBased(Trajectory);
}

namespace UE { namespace PoseSearch {

//////////////////////////////////////////////////////////////////////////
// FPoseHistory

/**
* Fills skeleton transforms with evaluated compact pose transforms.
* Bones that weren't evaluated are filled with the bone's reference pose.
*/
static void CopyCompactToSkeletonPose(const FCompactPose& Pose, TArray<FTransform>& OutLocalTransforms)
{
	const FBoneContainer& BoneContainer = Pose.GetBoneContainer();
	const FReferenceSkeleton& RefSkeleton = BoneContainer.GetReferenceSkeleton();
	TArrayView<const FTransform> RefSkeletonTransforms = MakeArrayView(RefSkeleton.GetRefBonePose());

	const int32 NumSkeletonBones = BoneContainer.GetNumBones();
	OutLocalTransforms.SetNum(NumSkeletonBones);

	for (auto SkeletonBoneIdx = FSkeletonPoseBoneIndex(0); SkeletonBoneIdx != NumSkeletonBones; ++SkeletonBoneIdx)
	{
		FCompactPoseBoneIndex CompactBoneIdx = BoneContainer.GetCompactPoseIndexFromSkeletonIndex(SkeletonBoneIdx.GetInt());
		OutLocalTransforms[SkeletonBoneIdx.GetInt()] = CompactBoneIdx.IsValid() ? Pose[CompactBoneIdx] : RefSkeletonTransforms[SkeletonBoneIdx.GetInt()];
	}
}

void FPoseHistory::Init(int32 InNumPoses, float InTimeHorizon)
{
	Poses.Reserve(InNumPoses);
	Knots.Reserve(InNumPoses);
	TimeHorizon = InTimeHorizon;
}

void FPoseHistory::Init(const FPoseHistory& History)
{
	Poses = History.Poses;
	Knots = History.Knots;
	TimeHorizon = History.TimeHorizon;
}

bool FPoseHistory::TrySampleLocalPose(float SecondsAgo, const TArray<FBoneIndexType>& RequiredBones, TArray<FTransform>& LocalPose)
{
	int32 NextIdx = LowerBound(Knots.begin(), Knots.end(), SecondsAgo, TGreater<>());
	if (NextIdx <= 0 || NextIdx >= Knots.Num())
	{
		return false;
	}

	int32 PrevIdx = NextIdx - 1;

	const FPose& PrevPose = Poses[PrevIdx];
	const FPose& NextPose = Poses[NextIdx];

	// Compute alpha between previous and next knots
	float Alpha = FMath::GetMappedRangeValueUnclamped(
		FVector2f(Knots[PrevIdx], Knots[NextIdx]),
		FVector2f(0.0f, 1.0f),
		SecondsAgo);

	// We may not have accumulated enough poses yet
	if (PrevPose.LocalTransforms.Num() != NextPose.LocalTransforms.Num())
	{
		return false;
	}

	if (RequiredBones.Num() > PrevPose.LocalTransforms.Num())
	{
		return false;
	}

	// Lerp between poses by alpha to produce output local pose at requested sample time
	LocalPose = PrevPose.LocalTransforms;
	FAnimationRuntime::LerpBoneTransforms(
		LocalPose,
		NextPose.LocalTransforms,
		Alpha,
		RequiredBones);

	return true;
}

bool FPoseHistory::TrySamplePose(float SecondsAgo, const FReferenceSkeleton& RefSkeleton, const TArray<FBoneIndexType>& RequiredBones)
{
	// Compute local space pose at requested time
	bool bSampled = TrySampleLocalPose(SecondsAgo, RequiredBones, SampledLocalPose);

	// Compute local space pose one sample interval in the past
	bSampled = bSampled && TrySampleLocalPose(SecondsAgo + GetSampleTimeInterval(), RequiredBones, SampledPrevLocalPose);

	// Convert local to component space
	if (bSampled)
	{
		FAnimationRuntime::FillUpComponentSpaceTransforms(RefSkeleton, SampledLocalPose, SampledComponentPose);
		FAnimationRuntime::FillUpComponentSpaceTransforms(RefSkeleton, SampledPrevLocalPose, SampledPrevComponentPose);
	}

	return bSampled;
}

void FPoseHistory::Update(float SecondsElapsed, const FPoseContext& PoseContext)
{
	// Age our elapsed times
	for (float& Knot : Knots)
	{
		Knot += SecondsElapsed;
	}

	if (Knots.Num() != Knots.Max())
	{
		// Consume every pose until the queue is full
		Knots.AddUninitialized();
		Poses.Emplace();
	}
	else
	{
		// Exercise pose retention policy. We must guarantee there is always one additional knot
		// beyond the time horizon so we can compute derivatives at the time horizon. We also
		// want to evenly distribute knots across the entire history buffer so we only push additional
		// poses when enough time has elapsed.

		const float SampleInterval = GetSampleTimeInterval();

		bool bCanEvictOldest = Knots[1] >= TimeHorizon + SampleInterval;
		bool bShouldPushNewest = Knots[Knots.Num() - 2] >= SampleInterval;

		if (bCanEvictOldest && bShouldPushNewest)
		{
			FPose PoseTemp = MoveTemp(Poses.First());
			Poses.PopFront();
			Poses.Emplace(MoveTemp(PoseTemp));

			Knots.PopFront();
			Knots.AddUninitialized();
		}
	}

	// Regardless of the retention policy, we always update the most recent pose
	Knots.Last() = 0.0f;
	FPose& CurrentPose = Poses.Last();
	CopyCompactToSkeletonPose(PoseContext.Pose, CurrentPose.LocalTransforms);
}

float FPoseHistory::GetSampleTimeInterval() const
{
	// Reserve one knot for computing derivatives at the time horizon
	return TimeHorizon / (Knots.Max() - 1);
}


//////////////////////////////////////////////////////////////////////////
// FPoseSearchFeatureVectorReader

void FFeatureVectorReader::Init(const FPoseSearchFeatureVectorLayout* InLayout)
{
	check(InLayout);
	Layout = InLayout;
}

void FFeatureVectorReader::SetValues(TArrayView<const float> InValues)
{
	check(Layout);
	check(Layout->NumFloats == InValues.Num());
	Values = InValues;
}

bool FFeatureVectorReader::IsValid() const
{
	return Layout && (Layout->NumFloats == Values.Num());
}

bool FFeatureVectorReader::GetTransform(FPoseSearchFeatureDesc Element, FTransform* OutTransform) const
{
	FVector Position;
	bool bResult = GetPosition(Element, &Position);

	FQuat Rotation;
	bResult |= GetRotation(Element, &Rotation);

	OutTransform->SetComponents(Rotation, Position, FVector::OneVector);
	return bResult;
}

bool FFeatureVectorReader::GetPosition(FPoseSearchFeatureDesc Element, FVector* OutPosition) const
{
	Element.Type = EPoseSearchFeatureType::Position;
	return GetVector(Element, OutPosition);
}

bool FFeatureVectorReader::GetRotation(FPoseSearchFeatureDesc Element, FQuat* OutRotation) const
{
	Element.Type = EPoseSearchFeatureType::Rotation;
	int32 ElementIndex = IsValid() ? Layout->Features.Find(Element) : -1;
	if (ElementIndex >= 0)
	{
		const FPoseSearchFeatureDesc& FoundElement = Layout->Features[ElementIndex];

		FVector X;
		FVector Y;

		X.X = Values[FoundElement.ValueOffset + 0];
		X.Y = Values[FoundElement.ValueOffset + 1];
		X.Z = Values[FoundElement.ValueOffset + 2];
		Y.X = Values[FoundElement.ValueOffset + 3];
		Y.Y = Values[FoundElement.ValueOffset + 4];
		Y.Z = Values[FoundElement.ValueOffset + 5];

		FVector Z = FVector::CrossProduct(X, Y);

		FMatrix M(FMatrix::Identity);
		M.SetColumn(0, X);
		M.SetColumn(1, Y);
		M.SetColumn(2, Z);

		*OutRotation = FQuat(M);
		return true;
	}

	*OutRotation = FQuat::Identity;
	return false;
}

bool FFeatureVectorReader::GetLinearVelocity(FPoseSearchFeatureDesc Element, FVector* OutLinearVelocity) const
{
	Element.Type = EPoseSearchFeatureType::LinearVelocity;
	return GetVector(Element, OutLinearVelocity);
}

bool FFeatureVectorReader::GetAngularVelocity(FPoseSearchFeatureDesc Element, FVector* OutAngularVelocity) const
{
	Element.Type = EPoseSearchFeatureType::AngularVelocity;
	return GetVector(Element, OutAngularVelocity);
}

bool FFeatureVectorReader::GetVector(FPoseSearchFeatureDesc Element, FVector* OutVector) const
{
	int32 ElementIndex = IsValid() ? Layout->Features.Find(Element) : -1;
	if (ElementIndex >= 0)
	{
		const FPoseSearchFeatureDesc& FoundElement = Layout->Features[ElementIndex];

		FVector V;
		V.X = Values[FoundElement.ValueOffset + 0];
		V.Y = Values[FoundElement.ValueOffset + 1];
		V.Z = Values[FoundElement.ValueOffset + 2];
		*OutVector = V;
		return true;
	}

	*OutVector = FVector::ZeroVector;
	return false;
}


//////////////////////////////////////////////////////////////////////////
// FDebugDrawParams

bool FDebugDrawParams::CanDraw() const
{
	if (!World)
	{
		return false;
	}

	const FPoseSearchIndex* SearchIndex = GetSearchIndex();
	if (!SearchIndex)
	{
		return false;
	}

	return SearchIndex->IsValid();
}

const FPoseSearchIndex* FDebugDrawParams::GetSearchIndex() const
{
	if (Database)
	{
		return &Database->SearchIndex;
	}

	if (SequenceMetaData)
	{
		return &SequenceMetaData->SearchIndex;
	}

	return nullptr;
}

const UPoseSearchSchema* FDebugDrawParams::GetSchema() const
{
	if (Database)
	{
		return Database->Schema;
	}

	if (SequenceMetaData)
	{
		return SequenceMetaData->Schema;
	}

	return nullptr;
}


//////////////////////////////////////////////////////////////////////////
// FSequenceSampler

struct FSequenceSampler
{
public:
	struct FInput
	{
		const UPoseSearchSchema* Schema = nullptr;
		const UAnimSequence* Sequence = nullptr;
		bool bLoopable = false;
		int32 DistanceSamplingRate = 60;
		FPoseSearchExtrapolationParameters ExtrapolationParameters;
	} Input;

	struct FOutput
	{
		TArray<float> AccumulatedRootDistance;

		int32 NumDistanceSamples = 0;
		float PlayLength = 0.0f;
		float TotalRootDistance = 0.0f;
		FTransform TotalRootMotion = FTransform::Identity;
	} Output;

	void Reset();
	void Init(const FInput& Input);
	void Process();


	// Extracts root transform at the given time, using the extremities of the sequence to extrapolate beyond the 
	// sequence limits when Time is less than zero or greater than the sequence length.
	FTransform ExtractRootTransform(float Time) const;

	// Extracts the accumulated root distance at the given time, using the extremities of the sequence to extrapolate 
	// beyond the sequence limits when Time is less than zero or greater than the sequence length
	float ExtractRootDistance(float Time) const;

private:
	void Reserve();
	void ProcessRootMotion();

	// Samples sequence and adjusts obtained root motion to ExtrapolationTime
	FTransform ExtrapolateRootMotion(float SampleStart, float SampleEnd, float ExtrapolationTime) const;

	// Uses distance delta between NextRootDistanceIndex and NextRootDistanceIndex - 1 and extrapolates it to 
	// ExtrapolationTime
	float ExtrapolateRootDistance(int32 NextRootDistanceIndex, float ExtrapolationTime) const;
};

void FSequenceSampler::Init(const FInput& InInput)
{
	check(InInput.Schema);
	check(InInput.Schema->IsValid());
	check(InInput.Sequence);

	Reset();

	Input = InInput;

	Output.PlayLength = Input.Sequence->GetPlayLength();
	Output.NumDistanceSamples = FMath::CeilToInt(Output.PlayLength * Input.DistanceSamplingRate) + 1;

	Reserve();
}

void FSequenceSampler::Reset()
{
	Input = FInput();

	Output.NumDistanceSamples = 0;
	Output.PlayLength = 0.0f;
	Output.TotalRootDistance = 0.0f;
	Output.AccumulatedRootDistance.Reset(0);
}

void FSequenceSampler::Reserve()
{
	Output.AccumulatedRootDistance.Reserve(Output.NumDistanceSamples);
}

void FSequenceSampler::Process()
{
	ProcessRootMotion();
}

FTransform FSequenceSampler::ExtractRootTransform(float Time) const
{
	if (Input.bLoopable)
	{
		FTransform LoopableRootTransform = Input.Sequence->ExtractRootMotion(0.0f, Time, true);
		return LoopableRootTransform;
	}

	const float ExtrapolationSampleTime = Input.ExtrapolationParameters.SampleTime;

	const float PlayLength = Input.Sequence->GetPlayLength();
	const float ClampedTime = FMath::Clamp(Time, 0.0f, PlayLength);
	const float ExtrapolationTime = Time - ClampedTime;

	FTransform RootTransform = FTransform::Identity;

	// If Time is less than zero, ExtrapolationTime will be negative. In this case, we extrapolate the beginning of the 
	// animation to estimate where the root would be at Time
	if (ExtrapolationTime < -SMALL_NUMBER)
	{
		const FTransform ExtrapolatedRootMotion = ExtrapolateRootMotion(
			0.0f, ExtrapolationSampleTime, 
			ExtrapolationTime);
		RootTransform = ExtrapolatedRootMotion;
	}
	else
	{
		RootTransform = Input.Sequence->ExtractRootMotionFromRange(0.0f, ClampedTime);

		// If Time is greater than PlayLength, ExtrapolationTIme will be a positive number. In this case, we extrapolate
		// the end of the animation to estimate where the root would be at Time
		if (ExtrapolationTime > SMALL_NUMBER)
		{
			const FTransform ExtrapolatedRootMotion = ExtrapolateRootMotion(
				PlayLength - ExtrapolationSampleTime, PlayLength,
				ExtrapolationTime);
			RootTransform = ExtrapolatedRootMotion * RootTransform;
		}
	}

	return RootTransform;
}

float FSequenceSampler::ExtractRootDistance(float Time) const
{
	const float ClampedTime = FMath::Clamp(Time, 0.0f, Output.PlayLength);

	// Find the distance sample that corresponds with the time and split into whole and partial parts
	float IntegralDistanceSample;
	float DistanceAlpha = FMath::Modf(ClampedTime * Input.DistanceSamplingRate, &IntegralDistanceSample);
	float DistanceIdx = (int32)IntegralDistanceSample;

	// Verify the distance offset and any residual portion would be in bounds
	check(DistanceIdx + (DistanceAlpha > 0.0f ? 1 : 0) < Output.AccumulatedRootDistance.Num());
	
	// Look up the distance and interpolate between distance samples if necessary
	float Distance = Output.AccumulatedRootDistance[DistanceIdx];
	if (DistanceAlpha > 0.0f)
	{
		float NextDistance = Output.AccumulatedRootDistance[DistanceIdx + 1];
		Distance = FMath::Lerp(Distance, NextDistance, DistanceAlpha);
	}

	const float ExtrapolationTime = Time - ClampedTime;

	if (ExtrapolationTime != 0.0f)
	{
		// If extrapolationTime is not zero, we extrapolate the beginning or the end of the animation to estimate
		// the root distance.
		const int32 DistIdx = (ExtrapolationTime > 0.0f) ? Output.AccumulatedRootDistance.Num() - 1 : 1;
		const float ExtrapolatedDistance = ExtrapolateRootDistance(DistIdx, ExtrapolationTime);
		Distance += ExtrapolatedDistance;
	}

	return Distance;
}

FTransform FSequenceSampler::ExtrapolateRootMotion(float SampleStart, float SampleEnd, float ExtrapolationTime) const
{
	const float SampleDelta = SampleEnd - SampleStart;
	check(!FMath::IsNearlyZero(SampleDelta));

	FTransform SampleToExtrapolate = Input.Sequence->ExtractRootMotionFromRange(SampleStart, SampleEnd);

	const FVector LinearVelocityToExtrapolate = SampleToExtrapolate.GetTranslation() / SampleDelta;
	const float LinearSpeedToExtrapolate = LinearVelocityToExtrapolate.Size();
	const bool bCanExtrapolateTranslation = 
		LinearSpeedToExtrapolate >= Input.ExtrapolationParameters.LinearSpeedThreshold;

	const float AngularSpeedToExtrapolateRad = SampleToExtrapolate.GetRotation().GetAngle() / SampleDelta;
	const bool bCanExtrapolateRotation = 
		FMath::RadiansToDegrees(AngularSpeedToExtrapolateRad) >= Input.ExtrapolationParameters.AngularSpeedThreshold;

	if (!bCanExtrapolateTranslation && !bCanExtrapolateRotation)
	{
		return FTransform::Identity;
	}

	if (!bCanExtrapolateTranslation)
	{
		SampleToExtrapolate.SetTranslation(FVector::ZeroVector);
	}

	if (!bCanExtrapolateRotation)
	{
		SampleToExtrapolate.SetRotation(FQuat::Identity);
	}

	// converting ExtrapolationTime to a positive number to avoid dealing with the negative extrapolation and inverting
	// transforms later on.
	const float AbsExtrapolationTime = FMath::Abs(ExtrapolationTime);
	const float AbsSampleDelta = FMath::Abs(SampleDelta);
	const FTransform AbsTimeSampleToExtrapolate = 
		ExtrapolationTime >= 0.0f ? SampleToExtrapolate : SampleToExtrapolate.Inverse();

	// because we're extrapolating rotation, the extrapolation must be integrated over time
	const float SampleMultiplier = AbsExtrapolationTime / AbsSampleDelta;
	float IntegralNumSamples;
	float RemainingSampleFraction = FMath::Modf(SampleMultiplier, &IntegralNumSamples);
	int32 NumSamples = (int32)IntegralNumSamples;

	// adding full samples to the extrapolated root motion
	FTransform ExtrapolatedRootMotion = FTransform::Identity;
	for (int i = 0; i < NumSamples; ++i)
	{
		ExtrapolatedRootMotion = AbsTimeSampleToExtrapolate * ExtrapolatedRootMotion;
	}
	
	// and a blend with identify for whatever is left
	FTransform RemainingExtrapolatedRootMotion;
	RemainingExtrapolatedRootMotion.Blend(
		FTransform::Identity, 
		AbsTimeSampleToExtrapolate, 
		RemainingSampleFraction);

	ExtrapolatedRootMotion = RemainingExtrapolatedRootMotion * ExtrapolatedRootMotion;
	return ExtrapolatedRootMotion;
}

float FSequenceSampler::ExtrapolateRootDistance(int32 NextRootDistanceIndex, float ExtrapolationTime) const
{
	check(NextRootDistanceIndex > 0 && NextRootDistanceIndex < Output.AccumulatedRootDistance.Num());

	const float DistanceDelta = 
		Output.AccumulatedRootDistance[NextRootDistanceIndex] - 
		Output.AccumulatedRootDistance[NextRootDistanceIndex - 1];
	const float Speed = DistanceDelta * Input.DistanceSamplingRate;
	const float ExtrapolationSpeed = Speed >= Input.ExtrapolationParameters.LinearSpeedThreshold ?
		Speed : 0.0f;
	const float ExtrapolatedDistance = ExtrapolationSpeed * ExtrapolationTime;

	return ExtrapolatedDistance;
}


void FSequenceSampler::ProcessRootMotion()
{
	// Note the distance sampling interval is independent of the schema's sampling interval
	const float DistanceSamplingInterval = 1.0f / Input.DistanceSamplingRate;

	const FTransform InitialRootTransform = Input.Sequence->ExtractRootTrackTransform(0.0f, nullptr);

	// Build a distance lookup table by sampling root motion at a fixed rate and accumulating
	// absolute translation deltas. During indexing we'll bsearch this table and interpolate
	// between samples in order to convert distance offsets to time offsets.
	// See also FSequenceIndexer::AddTrajectoryDistanceFeatures().
	double AccumulatedRootDistance = 0.0;
	FTransform LastRootTransform = InitialRootTransform;
	float SampleTime = 0.0f;
	for (int32 SampleIdx = 0; SampleIdx != Output.NumDistanceSamples; ++SampleIdx)
	{
		SampleTime = FMath::Min(SampleIdx * DistanceSamplingInterval, Output.PlayLength);

		FTransform RootTransform = Input.Sequence->ExtractRootTrackTransform(SampleTime, nullptr);
		FTransform LocalRootMotion = RootTransform.GetRelativeTransform(LastRootTransform);
		LastRootTransform = RootTransform;

		AccumulatedRootDistance += LocalRootMotion.GetTranslation().Size();
		Output.AccumulatedRootDistance.Add((float)AccumulatedRootDistance);
	}

	// Verify we sampled the final frame of the clip
	check(SampleTime == Input.Sequence->GetPlayLength());

	// Also emit root motion summary info to help with sample wrapping in 
	// FSequenceIndexer::GetSampleTimeFromDistance() and FSequenceIndexer::GetSampleInfo()
	Output.TotalRootMotion = LastRootTransform.GetRelativeTransform(InitialRootTransform);
	Output.TotalRootDistance = Output.AccumulatedRootDistance.Last();
}

struct FSamplingParam
{
	float WrappedParam = 0.0f;
	int32 NumCycles = 0;
	bool bClamped = false;
	
	// If the animation can't loop, WrappedParam contains the clamped value and whatever is left is stored here
	float Extrapolation = 0.0f;
};

static FSamplingParam WrapOrClampSamplingParam(bool bCanWrap, float SamplingParamExtent, float SamplingParam)
{
	// This is a helper function used by both time and distance sampling. A schema may specify time or distance
	// offsets that are multiple cycles of a clip away from the current pose being sampled.
	// And that time or distance offset may before the beginning of the clip (SamplingParam < 0.0f)
	// or after the end of the clip (SamplingParam > SamplingParamExtent). So this function
	// helps determine how many cycles need to be applied and what the wrapped value should be, clamping
	// if necessary.

	FSamplingParam Result;

	Result.WrappedParam = SamplingParam;

	if (bCanWrap)
	{
		if (SamplingParam < 0.0f)
		{
			while (Result.WrappedParam < 0.0f)
			{
				Result.WrappedParam += SamplingParamExtent;
				++Result.NumCycles;
			}
		}

		else
		{
			while (Result.WrappedParam > SamplingParamExtent)
			{
				Result.WrappedParam -= SamplingParamExtent;
				++Result.NumCycles;
			}
		}
	}

	float ParamClamped = FMath::Clamp(Result.WrappedParam, 0.0f, SamplingParamExtent);
	if (ParamClamped != Result.WrappedParam)
	{
		check(!bCanWrap);
		Result.Extrapolation = Result.WrappedParam - ParamClamped;
		Result.WrappedParam = ParamClamped;
		Result.bClamped = true;
	}
	
	return Result;
}


//////////////////////////////////////////////////////////////////////////
// FSequenceIndexer

class FSequenceIndexer
{
public:

	struct FInput
	{
		const FBoneContainer* BoneContainer = nullptr;
		const UPoseSearchSchema* Schema = nullptr;
		const FSequenceSampler* MainSequence = nullptr;
		const FSequenceSampler* LeadInSequence = nullptr;
		const FSequenceSampler* FollowUpSequence = nullptr;
		FFloatInterval RequestedSamplingRange = FFloatInterval(0.0f, 0.0f);
	} Input;

	struct FOutput
	{
		int32 FirstIndexedSample = 0;
		int32 LastIndexedSample = 0;
		int32 NumIndexedPoses = 0;
		TArray<float> FeatureVectorTable;
	} Output;

	void Reset();
	void Init(const FInput& Input);
	void Process();

private:
	FPoseSearchFeatureVectorBuilder FeatureVector;

	struct FSampleInfo
	{
		const FSequenceSampler* Clip = nullptr;
		FTransform RootTransform;
		float ClipTime = 0.0f;
		float RootDistance = 0.0f;

		bool IsValid() const { return Clip != nullptr; }
	};

	FSampleInfo GetSampleInfo(float SampleTime) const;
	FSampleInfo GetSampleInfoRelative(float SampleTime, const FSampleInfo& Origin) const;
	const float GetSampleTimeFromDistance(float Distance) const;

	void Reserve();
	void SampleBegin(int32 SampleIdx);
	void SampleEnd(int32 SampleIdx);
	void AddPoseFeatures(int32 SampleIdx);
	void AddTrajectoryTimeFeatures(int32 SampleIdx);
	void AddTrajectoryDistanceFeatures(int32 SampleIdx);
};

void FSequenceIndexer::Reset()
{
	Output.FirstIndexedSample = 0;
	Output.LastIndexedSample = 0;
	Output.NumIndexedPoses = 0;

	Output.FeatureVectorTable.Reset(0);
}

void FSequenceIndexer::Reserve()
{
	Output.FeatureVectorTable.SetNumZeroed(Input.Schema->Layout.NumFloats * Output.NumIndexedPoses);
}

void FSequenceIndexer::Init(const FInput& InSettings)
{
	check(InSettings.Schema);
	check(InSettings.Schema->IsValid());
	check(InSettings.MainSequence);

	Input = InSettings;

	const FFloatInterval SamplingRange = GetEffectiveSamplingRange(Input.MainSequence->Input.Sequence, Input.RequestedSamplingRange);

	Reset();
	Output.FirstIndexedSample = FMath::FloorToInt(SamplingRange.Min * Input.Schema->SampleRate);
	Output.LastIndexedSample = FMath::Max(0, FMath::CeilToInt(SamplingRange.Max * Input.Schema->SampleRate));
	Output.NumIndexedPoses = Output.LastIndexedSample - Output.FirstIndexedSample + 1;
	Reserve();
}

void FSequenceIndexer::Process()
{
	for (int32 SampleIdx = Output.FirstIndexedSample; SampleIdx <= Output.LastIndexedSample; ++SampleIdx)
	{
		FMemMark Mark(FMemStack::Get());

		SampleBegin(SampleIdx);

		AddPoseFeatures(SampleIdx);
		AddTrajectoryTimeFeatures(SampleIdx);
		AddTrajectoryDistanceFeatures(SampleIdx);

		SampleEnd(SampleIdx);
	}
}

void FSequenceIndexer::SampleBegin(int32 SampleIdx)
{
	FeatureVector.Init(Input.Schema);
}

void FSequenceIndexer::SampleEnd(int32 SampleIdx)
{
	check(FeatureVector.IsComplete());

	int32 FirstValueIdx = (SampleIdx - Output.FirstIndexedSample) * Input.Schema->Layout.NumFloats;
	TArrayView<float> WriteValues = MakeArrayView(&Output.FeatureVectorTable[FirstValueIdx], Input.Schema->Layout.NumFloats);

	TArrayView<const float> ReadValues = FeatureVector.GetValues();
	
	check(WriteValues.Num() == ReadValues.Num());
	FMemory::Memcpy(WriteValues.GetData(), ReadValues.GetData(), WriteValues.Num() * WriteValues.GetTypeSize());
}

const float FSequenceIndexer::GetSampleTimeFromDistance(float SampleDistance) const
{
	auto CanWrapDistanceSamples = [](const FSequenceSampler* Sampler) -> bool
	{
		constexpr float SMALL_ROOT_DISTANCE = 1.0f;
		return Sampler->Input.bLoopable && Sampler->Output.TotalRootDistance > SMALL_ROOT_DISTANCE;
	};

	auto ClipTimeFromDistance = [](const FSequenceSampler* Sampler, float ClipDistance) -> float
	{
		int32 NextSampleIdx = 1;
		int32 PrevSampleIdx = 0;
		if (ClipDistance > 0.0f)
		{
			// Search for the distance value. Because the values will be extrapolated if necessary
			// LowerBound might go past the end of the array, in which case the last valid index is used
			int32 ClipDistanceLowerBoundIndex = Algo::LowerBound(Sampler->Output.AccumulatedRootDistance, ClipDistance);
			NextSampleIdx = FMath::Min(
				ClipDistanceLowerBoundIndex,
				Sampler->Output.AccumulatedRootDistance.Num() - 1);

			// Compute distance interpolation amount
			PrevSampleIdx = FMath::Max(0, NextSampleIdx - 1);
		}

		float NextDistance = Sampler->Output.AccumulatedRootDistance[NextSampleIdx];
		float PrevDistance = Sampler->Output.AccumulatedRootDistance[PrevSampleIdx];
		float DistanceSampleAlpha = FMath::GetRangePct(PrevDistance, NextDistance, ClipDistance);

		// Convert to time
		float ClipTime = (float(NextSampleIdx) - (1.0f - DistanceSampleAlpha)) / Sampler->Input.DistanceSamplingRate;
		return ClipTime;
	};

	float MainTotalDistance = Input.MainSequence->Output.TotalRootDistance;
	bool bMainCanWrap = CanWrapDistanceSamples(Input.MainSequence);

	float SampleTime = MAX_flt;

	if (!bMainCanWrap)
	{
		// Use the lead in anim if we would have to clamp to the beginning of the main anim
		if (Input.LeadInSequence && (SampleDistance < 0.0f))
		{
			const FSequenceSampler::FOutput& ClipData = Input.LeadInSequence->Output;

			bool bLeadInCanWrap = CanWrapDistanceSamples(Input.LeadInSequence);
			float LeadRelativeDistance = SampleDistance + ClipData.TotalRootDistance;
			FSamplingParam SamplingParam = WrapOrClampSamplingParam(bLeadInCanWrap, ClipData.TotalRootDistance, LeadRelativeDistance);

			float ClipTime = ClipTimeFromDistance(
				Input.LeadInSequence, 
				SamplingParam.WrappedParam + SamplingParam.Extrapolation);

			// Make the lead in clip time relative to the main sequence again and unwrap
			SampleTime = -((SamplingParam.NumCycles * ClipData.PlayLength) + (ClipData.PlayLength - ClipTime));
		}

		// Use the follow up anim if we would have clamp to the end of the main anim
		else if (Input.FollowUpSequence && (SampleDistance > MainTotalDistance))
		{
			const FSequenceSampler::FOutput& ClipData = Input.FollowUpSequence->Output;

			bool bFollowUpCanWrap = CanWrapDistanceSamples(Input.FollowUpSequence);
			float FollowRelativeDistance = SampleDistance - MainTotalDistance;
			FSamplingParam SamplingParam = WrapOrClampSamplingParam(bFollowUpCanWrap, ClipData.TotalRootDistance, FollowRelativeDistance);

			float ClipTime = ClipTimeFromDistance(
				Input.FollowUpSequence, 
				SamplingParam.WrappedParam + SamplingParam.Extrapolation);

			// Make the follow up clip time relative to the main sequence again and unwrap
			SampleTime = Input.MainSequence->Output.PlayLength + SamplingParam.NumCycles * ClipData.PlayLength + ClipTime;
		}
	}

	// Use the main anim if we didn't use the lead-in or follow-up anims.
	// The main anim sample may have been wrapped or clamped
	if (SampleTime == MAX_flt)
	{
		const FSequenceSampler::FOutput& ClipData = Input.MainSequence->Output;

		float MainRelativeDistance = SampleDistance;
		if (SampleDistance < 0.0f && bMainCanWrap)
		{
			// In this case we're sampling a loop backwards, so MainRelativeDistance must adjust so the number of cycles 
			// is counted correctly.
			MainRelativeDistance += ClipData.TotalRootDistance;
		}

		FSamplingParam SamplingParam = WrapOrClampSamplingParam(bMainCanWrap, MainTotalDistance, MainRelativeDistance);
		float ClipTime = ClipTimeFromDistance(
			Input.MainSequence, 
			SamplingParam.WrappedParam + SamplingParam.Extrapolation);

		// Unwrap the main clip time
		if (bMainCanWrap)
		{
			if (SampleDistance < 0.0f)
			{
				SampleTime = -((SamplingParam.NumCycles * ClipData.PlayLength) + (ClipData.PlayLength - ClipTime));
			}
			else
			{
				SampleTime = SamplingParam.NumCycles * ClipData.PlayLength + ClipTime;
			}
		}
		else
		{
			SampleTime = ClipTime;
		}
	}

	return SampleTime;
}

FSequenceIndexer::FSampleInfo FSequenceIndexer::GetSampleInfo(float SampleTime) const
{
	FSampleInfo Sample;

	FTransform RootMotionLast = FTransform::Identity;
	FTransform RootMotionInitial = FTransform::Identity;

	float RootDistanceLast = 0.0f;
	float RootDistanceInitial = 0.0f;

	auto CanWrapTimeSamples = [](const FSequenceSampler* Sampler) -> bool
	{
		return Sampler->Input.bLoopable;
	};

	float MainPlayLength = Input.MainSequence->Output.PlayLength;
	bool bMainCanWrap = CanWrapTimeSamples(Input.MainSequence);

	FSamplingParam SamplingParam;
	if (!bMainCanWrap)
	{
		// Use the lead in anim if we would have to clamp to the beginning of the main anim
		if (Input.LeadInSequence && (SampleTime < 0.0f))
		{
			const FSequenceSampler::FOutput& ClipData = Input.LeadInSequence->Output;

			bool bLeadInCanWrap = CanWrapTimeSamples(Input.LeadInSequence);
			float LeadRelativeTime = SampleTime + ClipData.PlayLength;
			SamplingParam = WrapOrClampSamplingParam(bLeadInCanWrap, ClipData.PlayLength, LeadRelativeTime);

			Sample.Clip = Input.LeadInSequence;

			check(SamplingParam.Extrapolation <= 0.0f);
			if (SamplingParam.Extrapolation < 0.0f)
			{
				RootMotionInitial = Input.LeadInSequence->Output.TotalRootMotion.Inverse();
				RootDistanceInitial = -Input.LeadInSequence->Output.TotalRootDistance;
			}
			else
			{
				RootMotionInitial = FTransform::Identity;
				RootDistanceInitial = 0.0f;
			}

			RootMotionLast = Input.LeadInSequence->Output.TotalRootMotion;
			RootDistanceLast = Input.LeadInSequence->Output.TotalRootDistance;
		}

		// Use the follow up anim if we would have clamp to the end of the main anim
		else if (Input.FollowUpSequence && (SampleTime > MainPlayLength))
		{
			const FSequenceSampler::FOutput& ClipData = Input.FollowUpSequence->Output;

			bool bFollowUpCanWrap = CanWrapTimeSamples(Input.FollowUpSequence);
			float FollowRelativeTime = SampleTime - MainPlayLength;
			SamplingParam = WrapOrClampSamplingParam(bFollowUpCanWrap, ClipData.PlayLength, FollowRelativeTime);

			Sample.Clip = Input.FollowUpSequence;

			RootMotionInitial = Input.MainSequence->Output.TotalRootMotion;
			RootDistanceInitial = Input.MainSequence->Output.TotalRootDistance;

			RootMotionLast = Input.FollowUpSequence->Output.TotalRootMotion;
			RootDistanceLast = Input.FollowUpSequence->Output.TotalRootDistance;
		}
	}

	// Use the main anim if we didn't use the lead-in or follow-up anims.
	// The main anim sample may have been wrapped or clamped
	if (!Sample.IsValid())
	{
		float MainRelativeTime = SampleTime;
		if (SampleTime < 0.0f && bMainCanWrap)
		{
			// In this case we're sampling a loop backwards, so MainRelativeTime must adjust so the number of cycles is
			// counted correctly.
			MainRelativeTime += MainPlayLength;
		}

		SamplingParam = WrapOrClampSamplingParam(bMainCanWrap, MainPlayLength, MainRelativeTime);

		Sample.Clip = Input.MainSequence;

		RootMotionInitial = FTransform::Identity;
		RootDistanceInitial = 0.0f;

		RootMotionLast = Input.MainSequence->Output.TotalRootMotion;
		RootDistanceLast = Input.MainSequence->Output.TotalRootDistance;
	}


	if (FMath::Abs(SamplingParam.Extrapolation) > SMALL_NUMBER)
	{
		Sample.ClipTime = SamplingParam.WrappedParam + SamplingParam.Extrapolation;
		const FTransform ClipRootMotion = Sample.Clip->ExtractRootTransform(Sample.ClipTime);
		const float ClipDistance = Sample.Clip->ExtractRootDistance(Sample.ClipTime);

		Sample.RootTransform = ClipRootMotion * RootMotionInitial;
		Sample.RootDistance = RootDistanceInitial + ClipDistance;
	}
	else
	{
		Sample.ClipTime = SamplingParam.WrappedParam;

		// Determine how to accumulate motion for every cycle of the anim. If the sample
		// had to be clamped, this motion will end up not getting applied below.
		// Also invert the accumulation direction if the requested sample was wrapped backwards.
		FTransform RootMotionPerCycle = RootMotionLast;
		float RootDistancePerCycle = RootDistanceLast;
		if (SampleTime < 0.0f)
		{
			RootMotionPerCycle = RootMotionPerCycle.Inverse();
			RootDistancePerCycle *= -1;
		}

		// Find the remaining motion deltas after wrapping
		FTransform RootMotionRemainder = Sample.Clip->ExtractRootTransform(Sample.ClipTime);
		float RootDistanceRemainder = Sample.Clip->ExtractRootDistance(Sample.ClipTime);

		// Invert motion deltas if we wrapped backwards
		if (SampleTime < 0.0f)
		{
			RootMotionRemainder.SetToRelativeTransform(RootMotionLast);
			RootDistanceRemainder = -(RootDistanceLast - RootDistanceRemainder);
		}

		Sample.RootTransform = RootMotionInitial;
		Sample.RootDistance = RootDistanceInitial;

		// Note if the sample was clamped, no motion will be applied here because NumCycles will be zero
		int32 CyclesRemaining = SamplingParam.NumCycles;
		while (CyclesRemaining--)
		{
			Sample.RootTransform = RootMotionPerCycle * Sample.RootTransform;
         			Sample.RootDistance += RootDistancePerCycle;
		}

		Sample.RootTransform = RootMotionRemainder * Sample.RootTransform;
		Sample.RootDistance += RootDistanceRemainder;
	}

	return Sample;
}

FSequenceIndexer::FSampleInfo FSequenceIndexer::GetSampleInfoRelative(float SampleTime, const FSampleInfo& Origin) const
{
	FSampleInfo Sample = GetSampleInfo(SampleTime);
	Sample.RootTransform.SetToRelativeTransform(Origin.RootTransform);
	Sample.RootDistance = Origin.RootDistance - Sample.RootDistance;
	return Sample;
}

void FSequenceIndexer::AddPoseFeatures(int32 SampleIdx)
{
	// This function samples the instantaneous pose at time t as well as the pose's velocity and acceleration at time t.
	// Symmetric finite differences are used to approximate derivatives:
	//	First symmetric derivative:   f'(t) ~ (f(t+h) - f(t-h)) / 2h
	//	Second symmetric derivative: f''(t) ~ (f(t+h) - 2f(t) + f(t-h)) / h^2
	// Where h is a constant time delta
	// So this means three pose extractions are taken at time t-h, t, and t+h
	constexpr float FiniteDelta = 1 / 60.0f;
	constexpr int32 NumFiniteDiffTerms = 3;

	if (Input.Schema->Bones.IsEmpty() || Input.Schema->PoseSampleTimes.IsEmpty())
	{
		return;
	}

	FCompactPose Poses[NumFiniteDiffTerms];
	FCSPose<FCompactPose> ComponentSpacePoses[NumFiniteDiffTerms];
	FBlendedCurve Curves[NumFiniteDiffTerms];
	UE::Anim::FStackAttributeContainer Atrributes[NumFiniteDiffTerms];

	for(FCompactPose& Pose: Poses)
	{
		Pose.SetBoneContainer(Input.BoneContainer);
	}

	FAnimationPoseData AnimPoseData[NumFiniteDiffTerms] = 
	{
		{Poses[0], Curves[0], Atrributes[0]},
		{Poses[1], Curves[1], Atrributes[1]},
		{Poses[2], Curves[2], Atrributes[2]},
	};

	FAnimExtractContext ExtractionCtx;
	ExtractionCtx.bExtractRootMotion = true;

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = EPoseSearchFeatureDomain::Time;

	float SampleTime = FMath::Min(SampleIdx * Input.Schema->SamplingInterval, Input.MainSequence->Output.PlayLength);
	FSampleInfo Origin = GetSampleInfo(SampleTime);
	
	for (int32 SchemaSubsampleIdx = 0; SchemaSubsampleIdx != Input.Schema->PoseSampleTimes.Num(); ++SchemaSubsampleIdx)
	{
		Feature.SubsampleIdx = SchemaSubsampleIdx;

		float SubsampleTime = SampleTime + Input.Schema->PoseSampleTimes[SchemaSubsampleIdx];

		// For each pose subsample term, get the corresponding clip, accumulated root motion,
		// and wrap the time parameter based on the clip's length.
		FSampleInfo Samples[NumFiniteDiffTerms];
		Samples[0] = GetSampleInfoRelative(SubsampleTime - FiniteDelta, Origin);
		Samples[1] = GetSampleInfoRelative(SubsampleTime, Origin);
		Samples[2] = GetSampleInfoRelative(SubsampleTime + FiniteDelta, Origin);

		// Get pose samples
		for (int32 Term = 0; Term != NumFiniteDiffTerms; ++Term)
		{
			ExtractionCtx.CurrentTime = Samples[Term].ClipTime;
			Samples[Term].Clip->Input.Sequence->GetAnimationPose(AnimPoseData[Term], ExtractionCtx);
			ComponentSpacePoses[Term].InitPose(Poses[Term]);
		}

		// Get each bone's component transform, velocity, and acceleration and add accumulated root motion at this time offset
		// Think of this process as freezing the character in place (at SampleTime) and then tracing the paths of their joints
		// as they move through space from past to present to future (at times indicated by PoseSampleTimes).
		for (int32 SchemaBoneIndex = 0; SchemaBoneIndex != Input.Schema->NumBones(); ++SchemaBoneIndex)
		{
			Feature.SchemaBoneIdx = SchemaBoneIndex;

			FCompactPoseBoneIndex CompactBoneIndex = Input.BoneContainer->MakeCompactPoseIndex(FMeshPoseBoneIndex(Input.Schema->BoneIndices[SchemaBoneIndex]));

			FTransform BoneTransforms[NumFiniteDiffTerms];
			for (int32 Term = 0; Term != NumFiniteDiffTerms; ++Term)
			{
				BoneTransforms[Term] = ComponentSpacePoses[Term].GetComponentSpaceTransform(CompactBoneIndex);
				BoneTransforms[Term] = BoneTransforms[Term] * Samples[Term].RootTransform;
			}

			// Add properties to the feature vector for the pose at SampleIdx
			FeatureVector.SetTransform(Feature, BoneTransforms[1]);
			FeatureVector.SetTransformVelocity(Feature, BoneTransforms[2], BoneTransforms[0], 2.0f * FiniteDelta);
			//FeatureVector.SetTransformAccleration(Feature, BoneTransforms[2], BoneTransforms[1], BoneTransforms[0], FiniteDelta * FiniteDelta);
		}
	}
}

void FSequenceIndexer::AddTrajectoryTimeFeatures(int32 SampleIdx)
{
	// This function samples the instantaneous trajectory at time t as well as the trajectory's velocity and acceleration at time t.
	// Symmetric finite differences are used to approximate derivatives:
	//	First symmetric derivative:   f'(t) ~ (f(t+h) - f(t-h)) / 2h
	//	Second symmetric derivative: f''(t) ~ (f(t+h) - 2f(t) + f(t-h)) / h^2
	// Where h is a constant time delta
	// So this means three root motion extractions are taken at time t-h, t, and t+h
	constexpr float FiniteDelta = 1 / 60.0f;

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = EPoseSearchFeatureDomain::Time;
	Feature.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;

	float SampleTime = FMath::Min(SampleIdx * Input.Schema->SamplingInterval, Input.MainSequence->Output.PlayLength);
	FSampleInfo Origin = GetSampleInfo(SampleTime);
	
	for (int32 SchemaSubsampleIdx = 0; SchemaSubsampleIdx != Input.Schema->TrajectorySampleTimes.Num(); ++SchemaSubsampleIdx)
	{
		Feature.SubsampleIdx = SchemaSubsampleIdx;

		float SubsampleTime = SampleTime + Input.Schema->TrajectorySampleTimes[SchemaSubsampleIdx];

		// For each pose subsample term, get the corresponding clip, accumulated root motion,
		// and wrap the time parameter based on the clip's length.
		FSampleInfo Samples[3];
		Samples[0] = GetSampleInfoRelative(SubsampleTime - FiniteDelta, Origin);
		Samples[1] = GetSampleInfoRelative(SubsampleTime, Origin);
		Samples[2] = GetSampleInfoRelative(SubsampleTime + FiniteDelta, Origin);

		// Add properties to the feature vector for the pose at SampleIdx
		FeatureVector.SetTransform(Feature, Samples[1].RootTransform);
		FeatureVector.SetTransformVelocity(Feature, Samples[2].RootTransform, Samples[0].RootTransform, 2.0f * FiniteDelta);
		//FeatureVector.SetTransformAcceleration(Feature, Samples[2].RootTransform, Samples[1].RootTransform, Samples[0].RootTransform, FiniteDelta * FiniteDelta);
	}
}

void FSequenceIndexer::AddTrajectoryDistanceFeatures(int32 SampleIdx)
{
	// This function is very similar to AddTrajectoryTimeFeatures, but samples are taken in the distance domain
	// instead of time domain.
	constexpr float FiniteDelta = 1 / 60.0f;

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = EPoseSearchFeatureDomain::Distance;
	Feature.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;

	float SampleTime = FMath::Min(SampleIdx * Input.Schema->SamplingInterval, Input.MainSequence->Output.PlayLength);
	FSampleInfo Origin = GetSampleInfo(SampleTime);
	
	for (int32 SchemaSubsampleIdx = 0; SchemaSubsampleIdx != Input.Schema->TrajectorySampleDistances.Num(); ++SchemaSubsampleIdx)
	{
		Feature.SubsampleIdx = SchemaSubsampleIdx;

		// For distance based sampling of the trajectory, we first have to look up the time value
		// we're sampling given the desired travel distance of the root for this distance offset.
		// Once we know the time, we can then carry on just like time-based sampling.
		const float SubsampleDistance = Origin.RootDistance + Input.Schema->TrajectorySampleDistances[SchemaSubsampleIdx];
		float SubsampleTime = GetSampleTimeFromDistance(SubsampleDistance);

		// For each pose subsample term, get the corresponding clip, accumulated root motion,
		// and wrap the time parameter based on the clip's length.
		FSampleInfo Samples[3];
		Samples[0] = GetSampleInfoRelative(SubsampleTime - FiniteDelta, Origin);
		Samples[1] = GetSampleInfoRelative(SubsampleTime, Origin);
		Samples[2] = GetSampleInfoRelative(SubsampleTime + FiniteDelta, Origin);

		// Add properties to the feature vector for the pose at SampleIdx
		FeatureVector.SetTransform(Feature, Samples[1].RootTransform);
		FeatureVector.SetTransformVelocity(Feature, Samples[2].RootTransform, Samples[0].RootTransform, 2.0f * FiniteDelta);
		//FeatureVector.SetTransformAcceleration(Feature, Samples[0].RootTransform, Samples[1].RootTransform, Samples[2].RootTransform, FiniteDelta * FiniteDelta);
	}
}


//////////////////////////////////////////////////////////////////////////
// PoseSearch API

static void DrawTrajectoryFeatures(const FDebugDrawParams& DrawParams, const FFeatureVectorReader& Reader, EPoseSearchFeatureDomain Domain)
{
	const float LifeTime = DrawParams.DefaultLifeTime;
	const uint8 DepthPriority = ESceneDepthPriorityGroup::SDPG_Foreground + 2;
	const bool bPersistent = EnumHasAnyFlags(DrawParams.Flags, EDebugDrawFlags::Persistent);

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = Domain;
	Feature.SchemaBoneIdx = FPoseSearchFeatureDesc::TrajectoryBoneIndex;

	const int32 NumSubsamples = Domain == EPoseSearchFeatureDomain::Time ?
		DrawParams.GetSchema()->TrajectorySampleTimes.Num() :
		DrawParams.GetSchema()->TrajectorySampleDistances.Num();

	if (NumSubsamples == 0)
	{
		return;
	}

	for (int32 SchemaSubsampleIdx = 0; SchemaSubsampleIdx != NumSubsamples; ++SchemaSubsampleIdx)
	{
		Feature.SubsampleIdx = SchemaSubsampleIdx;

		FVector TrajectoryPos;
		if (Reader.GetPosition(Feature, &TrajectoryPos))
		{	
			Feature.Type = EPoseSearchFeatureType::Position;

			FLinearColor LinearColor = DrawParams.Color ? *DrawParams.Color : GetColorForFeature(Feature, Reader.GetLayout());
			FColor Color = LinearColor.ToFColor(true);

			TrajectoryPos = DrawParams.RootTransform.TransformPosition(TrajectoryPos);
			if (EnumHasAnyFlags(DrawParams.Flags, EDebugDrawFlags::DrawSearchIndex))
			{
				DrawDebugPoint(DrawParams.World, TrajectoryPos, DrawParams.PointSize, Color, bPersistent, DrawParams.DefaultLifeTime, DepthPriority);
			}
			else
			{
				DrawDebugSphere(DrawParams.World, TrajectoryPos, DrawDebugSphereSize, DrawDebugSphereSegments, Color, bPersistent, LifeTime, DepthPriority, DrawDebugSphereLineThickness);
			}
		}
		else
		{
			TrajectoryPos = DrawParams.RootTransform.GetTranslation();
		}

		FVector TrajectoryVel;
		if (Reader.GetLinearVelocity(Feature, &TrajectoryVel))
		{
			Feature.Type = EPoseSearchFeatureType::LinearVelocity;
			
			FLinearColor LinearColor = DrawParams.Color ? *DrawParams.Color : GetColorForFeature(Feature, Reader.GetLayout());
			FColor Color = LinearColor.ToFColor(true);

			TrajectoryVel *= DrawDebugVelocityScale;
			TrajectoryVel = DrawParams.RootTransform.TransformVector(TrajectoryVel);
			FVector TrajectoryVelDirection = TrajectoryVel.GetSafeNormal();
			if (EnumHasAnyFlags(DrawParams.Flags, EDebugDrawFlags::DrawSearchIndex))
			{
				DrawDebugPoint(DrawParams.World, TrajectoryVel, DrawParams.PointSize, Color, bPersistent, DrawParams.DefaultLifeTime, DepthPriority);
			}
			else
			{
				DrawDebugDirectionalArrow(DrawParams.World, TrajectoryPos + TrajectoryVelDirection * DrawDebugSphereSize, TrajectoryPos + TrajectoryVel, DrawDebugArrowSize, Color, bPersistent, LifeTime, DepthPriority, DrawDebugLineThickness);
			}
		}
	}
}

static void DrawPoseFeatures(const FDebugDrawParams& DrawParams, const FFeatureVectorReader& Reader)
{
	const UPoseSearchSchema* Schema = DrawParams.GetSchema();
	check(Schema && Schema->IsValid());

	const float LifeTime = DrawParams.DefaultLifeTime;
	const uint8 DepthPriority = ESceneDepthPriorityGroup::SDPG_Foreground + 2;
	const bool bPersistent = EnumHasAnyFlags(DrawParams.Flags, EDebugDrawFlags::Persistent);

	FPoseSearchFeatureDesc Feature;
	Feature.Domain = EPoseSearchFeatureDomain::Time;

	const int32 NumSubsamples = Schema->PoseSampleTimes.Num();
	const int32 NumBones = Schema->Bones.Num();

	if ((NumSubsamples * NumBones) == 0)
	{
		return;
	}

	for (int32 SchemaSubsampleIdx = 0; SchemaSubsampleIdx != NumSubsamples; ++SchemaSubsampleIdx)
	{
		Feature.SubsampleIdx = SchemaSubsampleIdx;
		
		for (int32 SchemaBoneIdx = 0; SchemaBoneIdx != NumBones; ++SchemaBoneIdx)
		{
			Feature.SchemaBoneIdx = SchemaBoneIdx;

			FVector BonePos;
			const bool bHaveBonePos = Reader.GetPosition(Feature, &BonePos);
			if (bHaveBonePos)
			{
				Feature.Type = EPoseSearchFeatureType::Position;
				
				FLinearColor LinearColor = DrawParams.Color ? *DrawParams.Color : GetColorForFeature(Feature, Reader.GetLayout());
				FColor Color = LinearColor.ToFColor(true);

				BonePos = DrawParams.RootTransform.TransformPosition(BonePos);
				if (EnumHasAnyFlags(DrawParams.Flags, EDebugDrawFlags::DrawSearchIndex))
				{
					DrawDebugPoint(DrawParams.World, BonePos, DrawParams.PointSize, Color, bPersistent, DrawParams.DefaultLifeTime, DepthPriority);
				}
				else
				{
					DrawDebugSphere(DrawParams.World, BonePos, DrawDebugSphereSize, DrawDebugSphereSegments, Color, bPersistent, LifeTime, DepthPriority, DrawDebugSphereLineThickness);
				}
			}

			FVector BoneVel;
			if (bHaveBonePos && Reader.GetLinearVelocity(Feature, &BoneVel))
			{
				Feature.Type = EPoseSearchFeatureType::LinearVelocity;
				
				FLinearColor LinearColor = DrawParams.Color ? *DrawParams.Color : GetColorForFeature(Feature, Reader.GetLayout());
				FColor Color = LinearColor.ToFColor(true);

				BoneVel *= DrawDebugVelocityScale;
				BoneVel = DrawParams.RootTransform.TransformVector(BoneVel);
				FVector BoneVelDirection = BoneVel.GetSafeNormal();
				if (EnumHasAnyFlags(DrawParams.Flags, EDebugDrawFlags::DrawSearchIndex))
				{
					DrawDebugPoint(DrawParams.World, BoneVel, DrawParams.PointSize, Color, bPersistent, DrawParams.DefaultLifeTime, DepthPriority);
				}
				else
				{
					DrawDebugDirectionalArrow(DrawParams.World, BonePos + BoneVelDirection * DrawDebugSphereSize, BonePos + BoneVel, DrawDebugArrowSize, Color, bPersistent, LifeTime, DepthPriority, DrawDebugLineThickness);
				}
			}
		}
	}
}

static void DrawFeatureVector(const FDebugDrawParams& DrawParams, const FFeatureVectorReader& Reader)
{
	if (EnumHasAnyFlags(DrawParams.Flags, EDebugDrawFlags::IncludePose))
	{
		DrawPoseFeatures(DrawParams, Reader);
	}

	if (EnumHasAnyFlags(DrawParams.Flags, EDebugDrawFlags::IncludeTrajectory))
	{
		DrawTrajectoryFeatures(DrawParams, Reader, EPoseSearchFeatureDomain::Time);
		DrawTrajectoryFeatures(DrawParams, Reader, EPoseSearchFeatureDomain::Distance);
	}
}

static void DrawFeatureVector(const FDebugDrawParams& DrawParams, TArrayView<const float> PoseVector)
{
	const UPoseSearchSchema* Schema = DrawParams.GetSchema();
	check(Schema)

	if (PoseVector.Num() != Schema->Layout.NumFloats)
	{
		return;
	}

	FFeatureVectorReader Reader;
	Reader.Init(&Schema->Layout);
	Reader.SetValues(PoseVector);
	DrawFeatureVector(DrawParams, Reader);
}

static void DrawSearchIndex(const FDebugDrawParams& DrawParams)
{
	const UPoseSearchSchema* Schema = DrawParams.GetSchema();
	const FPoseSearchIndex* SearchIndex = DrawParams.GetSearchIndex();
	check(Schema);
	check(SearchIndex);

	FFeatureVectorReader Reader;
	Reader.Init(&Schema->Layout);

	const int32 LastPoseIdx = SearchIndex->NumPoses;

	TArray<float> PoseVector;
	for (int32 PoseIdx = 0; PoseIdx != LastPoseIdx; ++PoseIdx)
	{
		PoseVector = SearchIndex->GetPoseValues(PoseIdx);
		SearchIndex->InverseNormalize(PoseVector);
		Reader.SetValues(PoseVector);
		DrawFeatureVector(DrawParams, Reader);
	}
}

void Draw(const FDebugDrawParams& DebugDrawParams)
{
	if (DebugDrawParams.CanDraw())
	{
		if (DebugDrawParams.PoseIdx != INDEX_NONE)
		{
			const FPoseSearchIndex* SearchIndex = DebugDrawParams.GetSearchIndex();
			check(SearchIndex);

			TArray<float> PoseVector;
			PoseVector = SearchIndex->GetPoseValues(DebugDrawParams.PoseIdx);
			SearchIndex->InverseNormalize(PoseVector);
			DrawFeatureVector(DebugDrawParams, PoseVector);
		}
		if (!DebugDrawParams.PoseVector.IsEmpty())
		{
			DrawFeatureVector(DebugDrawParams, DebugDrawParams.PoseVector);
		}
		if (EnumHasAnyFlags(DebugDrawParams.Flags, EDebugDrawFlags::DrawSearchIndex))
		{
			DrawSearchIndex(DebugDrawParams);
		}
	}
}

static void PreprocessSearchIndexNone(FPoseSearchIndex* SearchIndex)
{
	// This function leaves the data unmodified and simply outputs the transformation
	// and inverse transformation matrices as the identity matrix and the sample mean
	// as the zero vector.

	using namespace Eigen;

	check(SearchIndex->IsValid());

	FPoseSearchIndexPreprocessInfo& Info = SearchIndex->PreprocessInfo;
	Info.Reset();

	const FPoseSearchFeatureVectorLayout& Layout = SearchIndex->Schema->Layout;

	const int32 NumPoses = SearchIndex->NumPoses;
	const int32 NumDimensions = Layout.NumFloats;

	Info.NumDimensions = NumDimensions;
	Info.TransformationMatrix.SetNumZeroed(NumDimensions * NumPoses);
	Info.InverseTransformationMatrix.SetNumZeroed(NumDimensions * NumPoses);
	Info.SampleMean.SetNumZeroed(NumDimensions);

	// Map output transformation matrix
	auto TransformMap = Map<Matrix<float, Dynamic, Dynamic, ColMajor>>(
		Info.TransformationMatrix.GetData(),
		NumDimensions, NumPoses
	);

	// Map output inverse transformation matrix
	auto InverseTransformMap = Map<Matrix<float, Dynamic, Dynamic, ColMajor>>(
		Info.InverseTransformationMatrix.GetData(),
		NumDimensions, NumPoses
	);

	// Map output sample mean vector
	auto SampleMeanMap = Map<VectorXf>(Info.SampleMean.GetData(), NumDimensions);

	// Write the transformation matrices and sample mean
	TransformMap = MatrixXf::Identity(NumDimensions, NumPoses);
	InverseTransformMap = MatrixXf::Identity(NumDimensions, NumPoses);
	SampleMeanMap = VectorXf::Zero(NumDimensions);
}

inline Eigen::VectorXd ComputeFeatureMeanDeviations(const Eigen::MatrixXd& CenteredPoseMatrix, const FPoseSearchFeatureVectorLayout& Layout) {
	using namespace Eigen;

	const int32 NumPoses = CenteredPoseMatrix.cols();
	const int32 NumDimensions = CenteredPoseMatrix.rows();

	VectorXd MeanDeviations(NumDimensions);
	MeanDeviations.setConstant(1.0);
	for (const FPoseSearchFeatureDesc& Feature : Layout.Features)
	{
		int32 FeatureDims = GetFeatureTypeTraits(Feature.Type).NumFloats;

		// Construct a submatrix for the feature and find the average distance to the feature's centroid.
		// Since we've already mean centered the data, the average distance to the centroid is simply the average norm.
		double FeatureMeanDeviation = CenteredPoseMatrix.block(Feature.ValueOffset, 0, FeatureDims, NumPoses).colwise().norm().mean();

		// Fill the feature's corresponding scaling axes with the average distance
		// Avoid scaling by zero by leaving near-zero deviations as 1.0
		if (FeatureMeanDeviation > KINDA_SMALL_NUMBER)
		{
			MeanDeviations.segment(Feature.ValueOffset, FeatureDims).setConstant(FeatureMeanDeviation);
		}
	}

	return MeanDeviations;
}

static void PreprocessSearchIndexNormalize(FPoseSearchIndex* SearchIndex)
{
	// This function performs a modified z-score normalization where features are normalized
	// by mean absolute deviation rather than standard deviation. Both methods are preferable
	// here to min-max scaling because they preserve outliers.
	// 
	// Mean absolute deviation is preferred here over standard deviation because the latter
	// emphasizes outliers since squaring the distance from the mean increases variance 
	// exponentially rather than additively and square rooting the sum of squares does not 
	// remove that bias. [1]
	//
	// The pose matrix is transformed in place and the transformation matrix, its inverse,
	// and data mean vector are computed and stored along with it.
	//
	// N:	number of dimensions for input column vectors
	// P:	number of input column vectors
	// X:	NxP input matrix
	// x_p:	pth column vector of input matrix
	// u:   mean column vector of X
	//
	// S:	mean absolute deviations of X, as diagonal NxN matrix with average distances replicated for each feature's axes
	// s_n:	nth deviation
	//
	// Normalization by mean absolute deviation algorithm:
	//
	// 1) mean-center X
	//    x_p := x_p - u
	// 2) rescale X by inverse mean absolute deviation
	//    x_p := x_p * s_n^(-1)
	// 
	// Let S^(-1) be the inverse of S where the nth diagonal element is s_n^(-1)
	// then step 2 can be expressed as matrix multiplication:
	// X := S^(-1) * X
	//
	// By persisting the mean vector u and linear transform S, we can bring an input vector q
	// into the same space as the mean centered and scaled data matrix X:
	// q := S^(-1) * (q - u)
	//
	// This operation is invertible, a normalized data vector x can be unscaled via:
	// x := (S * x) + u
	//
	// References:
	// [1] Gorard, S. (2005), "Revisiting a 90-Year-Old Debate: The Advantages of the Mean Deviation."
	//     British Journal of Educational Studies, 53: 417-430.

	using namespace Eigen;

	check(SearchIndex->IsValid());

	FPoseSearchIndexPreprocessInfo& Info = SearchIndex->PreprocessInfo;
	Info.Reset();

	const FPoseSearchFeatureVectorLayout& Layout = SearchIndex->Schema->Layout;

	const int32 NumPoses = SearchIndex->NumPoses;
	const int32 NumDimensions = Layout.NumFloats;

	// Map input buffer
	auto PoseMatrixSourceMap = Map<Matrix<float, Dynamic, Dynamic, RowMajor>>(
		SearchIndex->Values.GetData(),
		NumPoses,		// rows
		NumDimensions	// cols
	);

	// Copy row major float matrix to column major double matrix
	MatrixXd PoseMatrix = PoseMatrixSourceMap.transpose().cast<double>();
	checkSlow(PoseMatrix.rows() == NumDimensions);
	checkSlow(PoseMatrix.cols() == NumPoses);

#if UE_POSE_SEARCH_EIGEN_DEBUG
	MatrixXd PoseMatrixOriginal = PoseMatrix;
#endif

	// Mean center
	VectorXd SampleMean = PoseMatrix.rowwise().mean();
	PoseMatrix = PoseMatrix.colwise() - SampleMean;

	// Compute per-feature average distances
	VectorXd MeanDeviations = ComputeFeatureMeanDeviations(PoseMatrix, Layout);

	// Construct a scaling matrix that uniformly scales each feature by its average distance from the mean
	MatrixXd ScalingMatrix = MeanDeviations.cwiseInverse().asDiagonal();

	// Construct the inverse scaling matrix
	MatrixXd InverseScalingMatrix = MeanDeviations.asDiagonal();

	// Rescale data by transforming it with the scaling matrix
	// Now each feature has an average Euclidean length = 1.
	PoseMatrix = ScalingMatrix * PoseMatrix;

	// Write normalized data back to source buffer, converting from column data back to row data
	PoseMatrixSourceMap = PoseMatrix.transpose().cast<float>();

	// Output preprocessing info
	Info.NumDimensions = NumDimensions;
	Info.TransformationMatrix.SetNumZeroed(ScalingMatrix.size());
	Info.InverseTransformationMatrix.SetNumZeroed(InverseScalingMatrix.size());
	Info.SampleMean.SetNumZeroed(SampleMean.size());

	auto TransformMap = Map<Matrix<float, Dynamic, Dynamic, ColMajor>>(
		Info.TransformationMatrix.GetData(),
		ScalingMatrix.rows(), ScalingMatrix.cols()
	);

	auto InverseTransformMap = Map<Matrix<float, Dynamic, Dynamic, ColMajor>>(
		Info.InverseTransformationMatrix.GetData(),
		InverseScalingMatrix.rows(), InverseScalingMatrix.cols()
	);

	auto SampleMeanMap = Map<VectorXf>(Info.SampleMean.GetData(), SampleMean.size());

	// Output scaling matrix, inverse scaling matrix, and mean vector
	TransformMap = ScalingMatrix.cast<float>();
	InverseTransformMap = InverseScalingMatrix.cast<float>();
	SampleMeanMap = SampleMean.cast<float>();

#if UE_POSE_SEARCH_EIGEN_DEBUG
	FString PoseMtxOriginalStr = EigenMatrixToString(PoseMatrixOriginal);
	FString PoseMtxStr = EigenMatrixToString(PoseMatrix);
	FString TransformationStr = EigenMatrixToString(TransformMap);
	FString InverseTransformationStr = EigenMatrixToString(InverseTransformMap);
	FString SampleMeanStr = EigenMatrixToString(SampleMeanMap);
#endif // UE_POSE_SEARCH_EIGEN_DEBUG
}

static void PreprocessSearchIndexSphere(FPoseSearchIndex* SearchIndex)
{
	// This function performs correlation based zero-phase component analysis sphering (ZCA-cor sphering)
	// The pose matrix is transformed in place and the transformation matrix, its inverse,
	// and data mean vector are computed and stored along with it.
	//
	// N:	number of dimensions for input column vectors
	// P:	number of input column vectors
	// X:	NxP input matrix
	// x_p:	pth column vector of input matrix
	// u:   mean column vector of X
	//
	// Eigendecomposition of correlation matrix of X:
	// cor(X) = (1/P) * X * X^T = V * D * V^T
	//
	// V:	eigenvectors of cor(X), stacked as columns in an orthogonal NxN matrix
	// D:	eigenvalues of cor(X), as diagonal NxN matrix
	// d_n:	nth eigenvalue
	// s_n: nth standard deviation
	// s_n^2 = d_n, the variance along the nth eigenvector
	// s_n   = d_n^(1/2)
	//
	// ZCA sphering algorithm:
	//
	// 1) mean-center X
	//    x_p := x_p - u
	// 2) align largest orthogonal directions of variance in X to coordinate axes (PCA rotate)
	//    x_p := V^T * x_p
	// 3) rescale X by inverse standard deviation
	//    x_p := x_p * d_n^(-1/2)
	// 4) return now rescaled X back to original rotation (inverse PCA rotate)
	//    x_p := V * x_p
	// 
	// Let D^(-1/2) be the inverse square root of D where the nth diagonal element is d_n^(-1/2)
	// then steps 2-4 can be expressed as a series of matrix multiplications:
	// Z = V * D^(-1/2) * V^T
	// X := Z * X
	//
	// By persisting the mean vector u and linear transform Z, we can bring an input vector q
	// into the same space as the sphered data matrix X:
	// q := Z * (q - u)
	//
	// This operation is invertible, a sphere standardized data vector x can be unscaled via:
	// Z^(-1) = V * D^(1/2) * V^T
	// x := (Z^(-1) * x) + u
	//
	// The sphering process allows nearest neighbor queries to use the Mahalanobis metric
	// which is unitless, scale-invariant, and accounts for feature correlation.
	// The Mahalanobis distance between two random vectors x and y in data matrix X is:
	// d(x,y) = ((x-y)^T * cov(X)^(-1) * (x-y))^(1/2)
	//
	// Because sphering transforms X into a new matrix with identity covariance, the Mahalanobis
	// distance equation above reduces to Euclidean distance since cov(X)^(-1) = I:
	// d(x,y) = ((x-y)^T * (x-y))^(1/2)
	// 
	// References:
	// Watt, Jeremy, et al. Machine Learning Refined: Foundations, Algorithms, and Applications.
	// 2nd ed., Cambridge University Press, 2020.
	// 
	// Kessy, Agnan, Alex Lewin, and Korbinian Strimmer. "Optimal whitening and decorrelation."
	// The American Statistician 72.4 (2018): 309-314.
	// 
	// https://en.wikipedia.org/wiki/Whitening_transformation
	// 
	// https://en.wikipedia.org/wiki/Mahalanobis_distance
	//
	// Note this sphering preprocessor needs more work and isn't yet exposed in the editor as an option.
	// Todo:
	// - Figure out apparent flipping behavior
	// - Try singular value decomposition in place of eigendecomposition
	// - Remove zero variance feature axes from data and search queries
	// - Support weighted Mahalanobis metric. User supplied weights need to be transformed to data's new basis.

#if UE_POSE_SEARCH_EIGEN_DEBUG
	double StartTime = FPlatformTime::Seconds();
#endif

	using namespace Eigen;

	check(SearchIndex->IsValid());

	FPoseSearchIndexPreprocessInfo& Info = SearchIndex->PreprocessInfo;
	Info.Reset();

	const FPoseSearchFeatureVectorLayout& Layout = SearchIndex->Schema->Layout;

	const int32 NumPoses = SearchIndex->NumPoses;
	const int32 NumDimensions = Layout.NumFloats;

	// Map input buffer
	auto PoseMatrixSourceMap = Map<Matrix<float, Dynamic, Dynamic, RowMajor>>(
		SearchIndex->Values.GetData(),
		NumPoses,		// rows
		NumDimensions	// cols
	);

	// Copy row major float matrix to column major double matrix
	MatrixXd PoseMatrix = PoseMatrixSourceMap.transpose().cast<double>();
	checkSlow(PoseMatrix.rows() == NumDimensions);
	checkSlow(PoseMatrix.cols() == NumPoses);

#if UE_POSE_SEARCH_EIGEN_DEBUG
	MatrixXd PoseMatrixOriginal = PoseMatrix;
#endif

	// Mean center
	VectorXd SampleMean = PoseMatrix.rowwise().mean();
	PoseMatrix = PoseMatrix.colwise() - SampleMean;

	// Compute per-feature average distances
	VectorXd MeanDeviations = ComputeFeatureMeanDeviations(PoseMatrix, Layout);

	// Rescale data by transforming it with the scaling matrix
	// Now each feature has an average Euclidean length = 1.
	MatrixXd PoseMatrixNormalized = MeanDeviations.cwiseInverse().asDiagonal() * PoseMatrix;

	// Compute sample covariance
	MatrixXd Covariance = ((1.0 / NumPoses) * (PoseMatrixNormalized * PoseMatrixNormalized.transpose())) + 1e-7 * MatrixXd::Identity(NumDimensions, NumDimensions);

	VectorXd StdDev = Covariance.diagonal().cwiseSqrt();
	VectorXd InvStdDev = StdDev.cwiseInverse();
	MatrixXd Correlation = InvStdDev.asDiagonal() * Covariance * InvStdDev.asDiagonal();

	// Compute eigenvalues and eigenvectors of correlation matrix
	SelfAdjointEigenSolver<MatrixXd> EigenDecomposition(Correlation, ComputeEigenvectors);

	VectorXd EigenValues = EigenDecomposition.eigenvalues();
	MatrixXd EigenVectors = EigenDecomposition.eigenvectors();

	// Sort eigenpairs by descending eigenvalue
	{
		const Eigen::Index n = EigenValues.size();
		for (Eigen::Index i = 0; i < n-1; ++i)
		{
			Index k;
			EigenValues.segment(i,n-i).maxCoeff(&k);
			if (k > 0)
			{
				std::swap(EigenValues[i], EigenValues[k+i]);
				EigenVectors.col(i).swap(EigenVectors.col(k+i));
			}
		}
	}

	// Regularize eigenvalues
	EigenValues = EigenValues.array() + 1e-7;

	// Compute ZCA-cor and ZCA-cor^(-1)
	MatrixXd ZCA = EigenVectors * EigenValues.cwiseInverse().cwiseSqrt().asDiagonal() * EigenVectors.transpose() * MeanDeviations.cwiseInverse().asDiagonal();
	MatrixXd ZCAInverse = MeanDeviations.asDiagonal() * EigenVectors * EigenValues.cwiseSqrt().asDiagonal() * EigenVectors.transpose();

	// Apply sphering transform to the data matrix
	PoseMatrix = ZCA * PoseMatrix;
	checkSlow(PoseMatrix.rows() == NumDimensions);
	checkSlow(PoseMatrix.cols() == NumPoses);

	// Write data back to source buffer, converting from column data back to row data
	PoseMatrixSourceMap = PoseMatrix.transpose().cast<float>();

	// Output preprocessing info
	Info.NumDimensions = NumDimensions;
	Info.TransformationMatrix.SetNumZeroed(ZCA.size());
	Info.InverseTransformationMatrix.SetNumZeroed(ZCAInverse.size());
	Info.SampleMean.SetNumZeroed(SampleMean.size());

	auto TransformMap = Map<Matrix<float, Dynamic, Dynamic, ColMajor>>(
		Info.TransformationMatrix.GetData(),
		ZCA.rows(), ZCA.cols()
	);

	auto InverseTransformMap = Map<Matrix<float, Dynamic, Dynamic, ColMajor>>(
		Info.InverseTransformationMatrix.GetData(),
		ZCAInverse.rows(), ZCAInverse.cols()
	);

	auto SampleMeanMap = Map<VectorXf>(Info.SampleMean.GetData(), SampleMean.size());

	// Output sphering matrix, inverse sphering matrix, and mean vector
	TransformMap = ZCA.cast<float>();
	InverseTransformMap = ZCAInverse.cast<float>();
	SampleMeanMap = SampleMean.cast<float>();

#if UE_POSE_SEARCH_EIGEN_DEBUG
	double ElapsedTime = FPlatformTime::Seconds() - StartTime;

	FString EigenValuesStr = EigenMatrixToString(EigenValues);
	FString EigenVectorsStr = EigenMatrixToString(EigenVectors);

	FString CovarianceStr = EigenMatrixToString(Covariance);
	FString CorrelationStr = EigenMatrixToString(Correlation);

	FString ZCAStr = EigenMatrixToString(ZCA);
	FString ZCAInverseStr = EigenMatrixToString(ZCAInverse);

	FString PoseMatrixSphereStr = EigenMatrixToString(PoseMatrix);
	MatrixXd PoseMatrixUnsphered = ZCAInverse * PoseMatrix;
	PoseMatrixUnsphered = PoseMatrixUnsphered.colwise() + SampleMean;
	FString PoseMatrixUnspheredStr = EigenMatrixToString(PoseMatrixUnsphered);
	FString PoseMatrixOriginalStr = EigenMatrixToString(PoseMatrixOriginal);

	FString OutputPoseMatrixStr = EigenMatrixToString(PoseMatrixSourceMap);

	FString TransformStr = EigenMatrixToString(TransformMap);
	FString InverseTransformStr = EigenMatrixToString(InverseTransformMap);
	FString SampleMeanStr = EigenMatrixToString(SampleMeanMap);
#endif // UE_POSE_SEARCH_EIGEN_DEBUG
}

static void PreprocessSearchIndex(FPoseSearchIndex* SearchIndex)
{
	switch (SearchIndex->Schema->EffectiveDataPreprocessor)
	{
		case EPoseSearchDataPreprocessor::Normalize:
			PreprocessSearchIndexNormalize(SearchIndex);
		break;

		case EPoseSearchDataPreprocessor::Sphere:
			PreprocessSearchIndexSphere(SearchIndex);
		break;

		case EPoseSearchDataPreprocessor::None:
			PreprocessSearchIndexNone(SearchIndex);
		break;

		case EPoseSearchDataPreprocessor::Invalid:
			checkNoEntry();
		break;
	}
}

bool BuildIndex(const UAnimSequence* Sequence, UPoseSearchSequenceMetaData* SequenceMetaData)
{
	check(Sequence);
	check(SequenceMetaData);

	if (!SequenceMetaData->IsValidForIndexing())
	{
		return false;
	}

	USkeleton* SeqSkeleton = Sequence->GetSkeleton();
	if (!SeqSkeleton || !SeqSkeleton->IsCompatible(SequenceMetaData->Schema->Skeleton))
	{
		return false;
	}

	FBoneContainer BoneContainer;
	BoneContainer.InitializeTo(SequenceMetaData->Schema->BoneIndicesWithParents, FCurveEvaluationOption(false), *SeqSkeleton);

	FSequenceSampler Sampler;
	FSequenceSampler::FInput SamplerInput;
	SamplerInput.Schema = SequenceMetaData->Schema;
	SamplerInput.ExtrapolationParameters = SequenceMetaData->ExtrapolationParameters;
	SamplerInput.Sequence = Sequence;
	SamplerInput.bLoopable = false;
	Sampler.Init(SamplerInput);
	Sampler.Process();

	FSequenceIndexer Indexer;
	FSequenceIndexer::FInput IndexerInput;
	IndexerInput.MainSequence = &Sampler;
	IndexerInput.Schema = SequenceMetaData->Schema;
	IndexerInput.RequestedSamplingRange = SequenceMetaData->SamplingRange;
	IndexerInput.BoneContainer = &BoneContainer;
	Indexer.Init(IndexerInput);
	Indexer.Process();

	SequenceMetaData->SearchIndex.Values = Indexer.Output.FeatureVectorTable;
	SequenceMetaData->SearchIndex.NumPoses = Indexer.Output.NumIndexedPoses;
	SequenceMetaData->SearchIndex.Schema = SequenceMetaData->Schema;

	PreprocessSearchIndex(&SequenceMetaData->SearchIndex);

	return true;
}

bool BuildIndex(UPoseSearchDatabase* Database)
{
	check(Database);

	if (!Database->IsValidForIndexing())
	{
		return false;
	}

	FBoneContainer BoneContainer;
	BoneContainer.InitializeTo(Database->Schema->BoneIndicesWithParents, FCurveEvaluationOption(false), *Database->Schema->Skeleton);

	// Prepare animation preprocessing tasks
	TArray<FSequenceSampler> SequenceSamplers;
	TMap<const UAnimSequence*, int32> SequenceSamplerMap;

	auto AddSampler = [&](const UAnimSequence* Sequence, bool bLoopable)
	{
		if (!SequenceSamplerMap.Contains(Sequence))
		{
			int32 SequenceSamplerIdx = SequenceSamplers.AddDefaulted();
			SequenceSamplerMap.Add(Sequence, SequenceSamplerIdx);

			FSequenceSampler::FInput Input;
			Input.Schema = Database->Schema;
			Input.ExtrapolationParameters = Database->ExtrapolationParameters;
			Input.Sequence = Sequence;
			Input.bLoopable = bLoopable;
			SequenceSamplers[SequenceSamplerIdx].Init(Input);
		}
	};

	for (const FPoseSearchDatabaseSequence& DbSequence : Database->Sequences)
	{
		if (DbSequence.Sequence)
		{
			AddSampler(DbSequence.Sequence, DbSequence.bLoopAnimation);
		}

		if (DbSequence.LeadInSequence)
		{
			AddSampler(DbSequence.LeadInSequence, DbSequence.bLoopLeadInAnimation);
		}

		if (DbSequence.FollowUpSequence)
		{
			AddSampler(DbSequence.FollowUpSequence, DbSequence.bLoopFollowUpAnimation);
		}
	}

	// Preprocess animations independently
	ParallelFor(SequenceSamplers.Num(), [&SequenceSamplers](int32 SamplerIdx){ SequenceSamplers[SamplerIdx].Process(); });


	auto GetSampler = [&](const UAnimSequence* Sequence) -> const FSequenceSampler*
	{
		return Sequence ? &SequenceSamplers[SequenceSamplerMap[Sequence]] : nullptr;
	};

	// Prepare animation indexing tasks
	TArray<FSequenceIndexer> Indexers;
	Indexers.SetNum(Database->Sequences.Num());
	for (int32 SequenceIdx = 0; SequenceIdx != Database->Sequences.Num(); ++SequenceIdx)
	{
		const FPoseSearchDatabaseSequence& DbSequence = Database->Sequences[SequenceIdx];
		FSequenceIndexer& Indexer = Indexers[SequenceIdx];

		FSequenceIndexer::FInput Input;
		Input.BoneContainer = &BoneContainer;
		Input.MainSequence = GetSampler(DbSequence.Sequence);
		Input.LeadInSequence = GetSampler(DbSequence.LeadInSequence);
		Input.FollowUpSequence = GetSampler(DbSequence.FollowUpSequence);
		Input.Schema = Database->Schema;
		Input.RequestedSamplingRange = DbSequence.SamplingRange;
		Indexer.Init(Input);
	}

	// Index animations independently
	ParallelFor(Indexers.Num(), [&Indexers](int32 SequenceIdx){ Indexers[SequenceIdx].Process(); });


	// Write index info to sequence and count up total poses and storage required
	int32 TotalPoses = 0;
	int32 TotalFloats = 0;
	for (int32 SequenceIdx = 0; SequenceIdx != Database->Sequences.Num(); ++SequenceIdx)
	{
		FPoseSearchDatabaseSequence& DbSequence = Database->Sequences[SequenceIdx];
		const FSequenceIndexer::FOutput& Output = Indexers[SequenceIdx].Output;
		DbSequence.NumPoses = Output.NumIndexedPoses;
		DbSequence.FirstPoseIdx = TotalPoses;
		TotalPoses += Output.NumIndexedPoses;
		TotalFloats += Output.FeatureVectorTable.Num();
	}

	// Join animation data into a single search index
	Database->SearchIndex.Values.Reset(TotalFloats);
	for (const FSequenceIndexer& Indexer : Indexers)
	{
		const FSequenceIndexer::FOutput& Output = Indexer.Output;
		Database->SearchIndex.Values.Append(Output.FeatureVectorTable.GetData(), Output.FeatureVectorTable.Num());
	}

	Database->SearchIndex.NumPoses = TotalPoses;
	Database->SearchIndex.Schema = Database->Schema;

	PreprocessSearchIndex(&Database->SearchIndex);

	return true;
}

static FSearchResult Search(const FPoseSearchIndex& SearchIndex, TArrayView<const float> Query, const FPoseSearchWeightsContext* WeightsContext = nullptr, const TSet<int32>& ExcludedIndices = TSet<int32>())
{
	FSearchResult Result;
	if (!ensure(SearchIndex.IsValid()))
	{
		return Result;
	}

	if (!ensure(Query.Num() == SearchIndex.Schema->Layout.NumFloats))
	{
		return Result;
	}

	float BestPoseDissimilarity = MAX_flt;
	int32 BestPoseIdx = INDEX_NONE;

	for (int32 PoseIdx = 0; PoseIdx != SearchIndex.NumPoses; ++PoseIdx)
	{
		if (ExcludedIndices.Contains(PoseIdx))
		{
			continue;
		}

		const float PoseDissimilarity = ComparePoses(SearchIndex, PoseIdx, Query, WeightsContext);
		
		if (PoseDissimilarity < BestPoseDissimilarity)
		{
			BestPoseDissimilarity = PoseDissimilarity;
			BestPoseIdx = PoseIdx;
		}
	}

	ensure(BestPoseIdx != INDEX_NONE);

	Result.Dissimilarity = BestPoseDissimilarity;
	Result.PoseIdx = BestPoseIdx;
	// Result.TimeOffsetSeconds is set by caller

	return Result;
}

FSearchResult Search(const UAnimSequenceBase* Sequence, TArrayView<const float> Query, FDebugDrawParams DebugDrawParams)
{
	const UPoseSearchSequenceMetaData* MetaData = Sequence ? Sequence->FindMetaDataByClass<UPoseSearchSequenceMetaData>() : nullptr;
	if (!MetaData || !MetaData->IsValidForSearch())
	{
		return FSearchResult();
	}

	const FPoseSearchIndex& SearchIndex = MetaData->SearchIndex;

	FSearchResult Result = Search(SearchIndex, Query);
	if (!Result.IsValid())
	{
		return Result;
	}

	const FFloatInterval SamplingRange = GetEffectiveSamplingRange(Sequence, MetaData->SamplingRange);
	Result.TimeOffsetSeconds = SamplingRange.Min + (SearchIndex.Schema->SamplingInterval * Result.PoseIdx);

	// Do debug visualization
	DebugDrawParams.SequenceMetaData = MetaData;
	DebugDrawParams.PoseVector = Query;
	DebugDrawParams.PoseIdx = Result.PoseIdx;
	Draw(DebugDrawParams);

	return Result;
}

FDbSearchResult Search(
	const UPoseSearchDatabase* Database, 
	TArrayView<const float> Query, 
	const FPoseSearchWeightsContext* WeightsContext, 
	const float EndTimeToExclude, FDebugDrawParams DebugDrawParams)
{
	if (!ensure(Database && Database->IsValidForSearch()))
	{
		return FDbSearchResult();
	}

	const FPoseSearchIndex& SearchIndex = Database->SearchIndex;

	const int32 NumSequenceIndicesToExclude = Database->Schema->SampleRate * EndTimeToExclude;
	TSet<int32> ExcludedIndices;
	if (NumSequenceIndicesToExclude > 0)
	{
		for (const FPoseSearchDatabaseSequence& Sequence : Database->Sequences)
		{
			// leaving at least one sample per sequence
			const int32 ExclusionStartIndex = Sequence.FirstPoseIdx + FMath::Max(1, Sequence.NumPoses - NumSequenceIndicesToExclude);
			const int32 ExclusionEndIndex = Sequence.FirstPoseIdx + Sequence.NumPoses;
			for (int32 ExcludedIndex = ExclusionStartIndex; ExcludedIndex < ExclusionEndIndex; ++ExcludedIndex)
			{
				ExcludedIndices.Add(ExcludedIndex);
			}
		}
	}
	
	FDbSearchResult Result = Search(SearchIndex, Query, WeightsContext, ExcludedIndices);
	if (!Result.IsValid())
	{
		return FDbSearchResult();
	}

	int32 DbSequenceIdx = Database->FindSequenceForPose(Result.PoseIdx);
	if (DbSequenceIdx == INDEX_NONE)
	{
		return FDbSearchResult();
	}
	
	const FPoseSearchDatabaseSequence& DbSequence = Database->Sequences[DbSequenceIdx];
	const FFloatInterval SamplingRange = GetEffectiveSamplingRange(DbSequence.Sequence, DbSequence.SamplingRange);

	Result.DbSequenceIdx = DbSequenceIdx;
	Result.TimeOffsetSeconds = SamplingRange.Min + SearchIndex.Schema->SamplingInterval * (Result.PoseIdx - DbSequence.FirstPoseIdx);

	// Do debug visualization
	DebugDrawParams.Database = Database;
	DebugDrawParams.PoseVector = Query;
	DebugDrawParams.PoseIdx = Result.PoseIdx;
	Draw(DebugDrawParams);

	return Result;
}

float ComparePoses(const FPoseSearchIndex& SearchIndex, int32 PoseIdx, TArrayView<const float> Query, const FPoseSearchWeightsContext* WeightsContext)
{
	TArrayView<const float> PoseValues = SearchIndex.GetPoseValues(PoseIdx);
	check(PoseValues.Num() == Query.Num());

	float Dissimilarity;
	if (WeightsContext)
	{
		const FPoseSearchWeights* WeightsSet = WeightsContext->GetGroupWeights(0);
		Dissimilarity = CompareFeatureVectors(PoseValues.Num(), PoseValues.GetData(), Query.GetData(), WeightsSet->Weights.GetData());
	}
	else
	{
		Dissimilarity = CompareFeatureVectors(PoseValues.Num(), PoseValues.GetData(), Query.GetData());
	}

	return Dissimilarity;
}


//////////////////////////////////////////////////////////////////////////
// FModule

class FModule : public IModuleInterface, public UE::Anim::IPoseSearchProvider
{
public: // IModuleInterface
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

public: // IPoseSearchProvider
	virtual UE::Anim::IPoseSearchProvider::FSearchResult Search(const FAnimationBaseContext& GraphContext, const UAnimSequenceBase* Sequence) override;
};

void FModule::StartupModule()
{
	IModularFeatures::Get().RegisterModularFeature(UE::Anim::IPoseSearchProvider::ModularFeatureName, this);

#if UE_POSE_SEARCH_TRACE_ENABLED
	// Enable the PoseSearch trace channel
	UE::Trace::ToggleChannel(*FTraceLogger::Name.ToString(), true);
#endif
}

void FModule::ShutdownModule()
{
	IModularFeatures::Get().UnregisterModularFeature(UE::Anim::IPoseSearchProvider::ModularFeatureName, this);
}

UE::Anim::IPoseSearchProvider::FSearchResult FModule::Search(const FAnimationBaseContext& GraphContext, const UAnimSequenceBase* Sequence)
{
	UE::Anim::IPoseSearchProvider::FSearchResult ProviderResult;

	const UPoseSearchSequenceMetaData* MetaData = Sequence ? Sequence->FindMetaDataByClass<UPoseSearchSequenceMetaData>() : nullptr;
	if (!MetaData || !MetaData->IsValidForSearch())
	{
		return ProviderResult;
	}

	IPoseHistoryProvider* PoseHistoryProvider = GraphContext.GetMessage<IPoseHistoryProvider>();
	if (!PoseHistoryProvider)
	{
		return ProviderResult;
	}

	FPoseHistory& PoseHistory = PoseHistoryProvider->GetPoseHistory();
	FPoseSearchFeatureVectorBuilder& QueryBuilder = PoseHistory.GetQueryBuilder();

	QueryBuilder.Init(MetaData->Schema);
	if (!QueryBuilder.TrySetPoseFeatures(&PoseHistory))
	{
		return ProviderResult;
	}

	QueryBuilder.Normalize(MetaData->SearchIndex);

	::UE::PoseSearch::FSearchResult Result = ::UE::PoseSearch::Search(Sequence, QueryBuilder.GetNormalizedValues());

	ProviderResult.Dissimilarity = Result.Dissimilarity;
	ProviderResult.PoseIdx = Result.PoseIdx;
	ProviderResult.TimeOffsetSeconds = Result.TimeOffsetSeconds;
	return ProviderResult;
}

}} // namespace UE::PoseSearch

IMPLEMENT_MODULE(UE::PoseSearch::FModule, PoseSearch)