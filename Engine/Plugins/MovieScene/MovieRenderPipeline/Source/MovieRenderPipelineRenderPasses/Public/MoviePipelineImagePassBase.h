// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "MoviePipelineRenderPass.h"
#include "MovieRenderPipelineDataTypes.h"
#include "SceneTypes.h"
#include "SceneView.h"
#include "MoviePipelineSurfaceReader.h"
#include "UObject/GCObject.h"
#include "Async/AsyncWork.h"
#include "Async/TaskGraphInterfaces.h"
#include "Templates/Function.h"
#include "Stats/Stats.h"
#include "CanvasTypes.h"
#include "Stats/Stats2.h"
#include "MovieRenderPipelineCoreModule.h"
#include "OpenColorIODisplayExtension.h"

#include "MoviePipelineImagePassBase.generated.h"

class UTextureRenderTarget2D;
struct FImageOverlappedAccumulator;
class FSceneViewFamily;
class FSceneView;
struct FAccumulatorPool;

class FMoviePipelineBackgroundAccumulateTask
{
public:
	FGraphEventRef LastCompletionEvent;

public:
	FGraphEventRef Execute(TUniqueFunction<void()> InFunctor)
	{
		if (LastCompletionEvent)
		{
			LastCompletionEvent = FFunctionGraphTask::CreateAndDispatchWhenReady(MoveTemp(InFunctor), GetStatId(), LastCompletionEvent);
		}
		else
		{
			LastCompletionEvent = FFunctionGraphTask::CreateAndDispatchWhenReady(MoveTemp(InFunctor), GetStatId());
		}
		return LastCompletionEvent;
	}

	FORCEINLINE TStatId GetStatId() const
	{
		RETURN_QUICK_DECLARE_CYCLE_STAT(FMoviePipelineBackgroundAccumulateTask, STATGROUP_ThreadPoolAsyncTasks);
	}
};

namespace MoviePipeline
{
	struct FImageSampleAccumulationArgs
	{
	public:
		TWeakPtr<FImageOverlappedAccumulator, ESPMode::ThreadSafe> ImageAccumulator;
		TWeakPtr<IMoviePipelineOutputMerger, ESPMode::ThreadSafe> OutputMerger;
		bool bAccumulateAlpha;
	};

	void MOVIERENDERPIPELINERENDERPASSES_API AccumulateSample_TaskThread(TUniquePtr<FImagePixelData>&& InPixelData, const MoviePipeline::FImageSampleAccumulationArgs& InParams);
}


UCLASS(BlueprintType, Abstract)
class MOVIERENDERPIPELINERENDERPASSES_API UMoviePipelineImagePassBase : public UMoviePipelineRenderPass
{
	GENERATED_BODY()

public:
	UMoviePipelineImagePassBase()
		: UMoviePipelineRenderPass()
	{
		PassIdentifier = FMoviePipelinePassIdentifier("ImagePassBase");
	}

	/* Dummy interface to allow classes with overriden functiosn to pass their own data around. */
	struct IViewCalcPayload {};
protected:

	// UMoviePipelineRenderPass API
	virtual void GatherOutputPassesImpl(TArray<FMoviePipelinePassIdentifier>& ExpectedRenderPasses) override;
	virtual void SetupImpl(const MoviePipeline::FMoviePipelineRenderPassInitSettings& InPassInitSettings) override;
	virtual void TeardownImpl() override;
	// ~UMovieRenderPassAPI

	// FGCObject Interface
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);
	// ~FGCObject Interface

	FVector4 CalculatePrinciplePointOffsetForTiling(const FMoviePipelineRenderPassMetrics& InSampleState) const;
	void ModifyProjectionMatrixForTiling(const FMoviePipelineRenderPassMetrics& InSampleState, FMatrix& InOutProjectionMatrix, float& OutDoFSensorScale) const;


protected:
	virtual void GetViewShowFlags(FEngineShowFlags& OutShowFlag, EViewModeIndex& OutViewModeIndex) const;
	virtual TSharedPtr<FSceneViewFamilyContext> CalculateViewFamily(FMoviePipelineRenderPassMetrics& InOutSampleState, IViewCalcPayload* OptPayload = nullptr);
	virtual void BlendPostProcessSettings(FSceneView* InView);
	virtual void SetupViewForViewModeOverride(FSceneView* View);
	virtual void MoviePipelineRenderShowFlagOverride(FEngineShowFlags& OutShowFlag) {}
	virtual bool IsScreenPercentageSupported() const { return true; }	
	virtual bool IsAntiAliasingSupported() const { return true; }
	virtual int32 GetOutputFileSortingOrder() const { return -1; }
	virtual FSceneViewStateInterface* GetSceneViewStateInterface(IViewCalcPayload* OptPayload = nullptr) { return ViewState.GetReference(); }
	virtual UTextureRenderTarget2D* GetViewRenderTarget(IViewCalcPayload* OptPayload = nullptr) const { return TileRenderTarget.Get(); }
	virtual void AddViewExtensions(FSceneViewFamilyContext& InContext, FMoviePipelineRenderPassMetrics& InOutSampleState) { }
	virtual bool IsAutoExposureAllowed(const FMoviePipelineRenderPassMetrics& InSampleState) const { return true; }
	virtual FSceneView* GetSceneViewForSampleState(FSceneViewFamily* ViewFamily, FMoviePipelineRenderPassMetrics& InOutSampleState, IViewCalcPayload* OptPayload = nullptr);
public:
	

protected:
	/** A temporary render target that we render the view to. */
	TWeakObjectPtr<UTextureRenderTarget2D> TileRenderTarget;

	/** The history for the view */
	FSceneViewStateReference ViewState;

	/** A queue of surfaces that the render targets can be copied to. If no surface is available the game thread should hold off on submitting more samples. */
	TSharedPtr<FMoviePipelineSurfaceQueue> SurfaceQueue;

	FMoviePipelinePassIdentifier PassIdentifier;

	/** Accessed by the Render Thread when starting up a new task. */
	FGraphEventArray OutstandingTasks;
};

struct MOVIERENDERPIPELINERENDERPASSES_API FAccumulatorPool : public TSharedFromThis<FAccumulatorPool>
{
	struct FAccumulatorInstance
	{
		FAccumulatorInstance(TSharedPtr<MoviePipeline::IMoviePipelineOverlappedAccumulator, ESPMode::ThreadSafe> InAccumulator)
		{
			Accumulator = InAccumulator;
			ActiveFrameNumber = INDEX_NONE;
			bIsActive = false;
		}


		bool IsActive() const;
		void SetIsActive(const bool bInIsActive);

		TSharedPtr<MoviePipeline::IMoviePipelineOverlappedAccumulator, ESPMode::ThreadSafe> Accumulator;
		int32 ActiveFrameNumber;
		FMoviePipelinePassIdentifier ActivePassIdentifier;
		FThreadSafeBool bIsActive;
		FGraphEventRef TaskPrereq;
	};

	TArray<TSharedPtr<FAccumulatorInstance, ESPMode::ThreadSafe>> Accumulators;
	FCriticalSection CriticalSection;


	TSharedPtr<FAccumulatorPool::FAccumulatorInstance, ESPMode::ThreadSafe> BlockAndGetAccumulator_GameThread(int32 InFrameNumber, const FMoviePipelinePassIdentifier& InPassIdentifier);
};

template<typename AccumulatorType>
struct TAccumulatorPool : FAccumulatorPool
{
	TAccumulatorPool(int32 InNumAccumulators)
		: FAccumulatorPool()
	{
		for (int32 Index = 0; Index < InNumAccumulators; Index++)
		{
			// Create a new instance of the accumulator
			TSharedPtr<MoviePipeline::IMoviePipelineOverlappedAccumulator, ESPMode::ThreadSafe> Accumulator = MakeShared<AccumulatorType, ESPMode::ThreadSafe>();
			Accumulators.Add(MakeShared<FAccumulatorInstance, ESPMode::ThreadSafe>(Accumulator));
		}

	}
};


DECLARE_CYCLE_STAT(TEXT("STAT_MoviePipeline_WaitForAvailableAccumulator"), STAT_MoviePipeline_WaitForAvailableAccumulator, STATGROUP_MoviePipeline);
DECLARE_CYCLE_STAT(TEXT("STAT_MoviePipeline_WaitForAvailableSurface"), STAT_MoviePipeline_WaitForAvailableSurface, STATGROUP_MoviePipeline);
