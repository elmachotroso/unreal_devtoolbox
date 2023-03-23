// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "RenderGraphResources.h"
#include "RenderGraphEvent.h"
#include "Containers/SortedMap.h"

using FRDGTransitionQueue = TArray<const FRHITransition*, TInlineAllocator<8>>;

struct FRDGBarrierBatchBeginId
{
	FRDGBarrierBatchBeginId() = default;

	bool operator==(FRDGBarrierBatchBeginId Other) const
	{
		return Passes == Other.Passes && PipelinesAfter == Other.PipelinesAfter;
	}

	bool operator!=(FRDGBarrierBatchBeginId Other) const
	{
		return !(*this == Other);
	}

	friend uint32 GetTypeHash(FRDGBarrierBatchBeginId Id)
	{
		static_assert(sizeof(Id.Passes) == 4);
		uint32 Hash = *(const uint32*)Id.Passes.GetData();
		return (Hash << GetRHIPipelineCount()) | uint32(Id.PipelinesAfter);
	}

	FRDGPassHandlesByPipeline Passes;
	ERHIPipeline PipelinesAfter = ERHIPipeline::None;
};

/** Barrier location controls where the barrier is 'Ended' relative to the pass lambda being executed.
 *  Most barrier locations are done in the prologue prior to the executing lambda. But certain cases
 *  like an aliasing discard operation need to be done *after* the pass being invoked. Therefore, when
 *  adding a transition the user can specify where to place the barrier.
 */
enum class ERDGBarrierLocation : uint8
{
	/** The barrier occurs in the prologue of the pass (before execution). */
	Prologue,

	/** The barrier occurs in the epilogue of the pass (after execution). */
	Epilogue
};

struct FRDGBarrierBatchEndId
{
	FRDGBarrierBatchEndId() = default;
	FRDGBarrierBatchEndId(FRDGPassHandle InPassHandle, ERDGBarrierLocation InBarrierLocation)
		: PassHandle(InPassHandle)
		, BarrierLocation(InBarrierLocation)
	{}

	bool operator == (FRDGBarrierBatchEndId Other) const
	{
		return PassHandle == Other.PassHandle && BarrierLocation == Other.BarrierLocation;
	}

	bool operator != (FRDGBarrierBatchEndId Other) const
	{
		return *this == Other;
	}

	FRDGPassHandle PassHandle;
	ERDGBarrierLocation BarrierLocation = ERDGBarrierLocation::Epilogue;
};

class RENDERCORE_API FRDGBarrierBatchBegin
{
public:
	FRDGBarrierBatchBegin(ERHIPipeline PipelinesToBegin, ERHIPipeline PipelinesToEnd, const TCHAR* DebugName, FRDGPass* DebugPass);
	FRDGBarrierBatchBegin(ERHIPipeline PipelinesToBegin, ERHIPipeline PipelinesToEnd, const TCHAR* DebugName, FRDGPassesByPipeline DebugPasses);

	void AddTransition(FRDGParentResourceRef Resource, const FRHITransitionInfo& Info);

	void AddAlias(FRDGParentResourceRef Resource, const FRHITransientAliasingInfo& Info);

	void SetUseCrossPipelineFence()
	{
		TransitionFlags = ERHITransitionCreateFlags::None;
		bTransitionNeeded = true;
	}

	void CreateTransition();

	void Submit(FRHIComputeCommandList& RHICmdList, ERHIPipeline Pipeline);
	void Submit(FRHIComputeCommandList& RHICmdList, ERHIPipeline Pipeline, FRDGTransitionQueue& TransitionsToBegin);

	void Reserve(uint32 TransitionCount)
	{
		Transitions.Reserve(TransitionCount);
	}

	bool IsTransitionNeeded() const
	{
		return bTransitionNeeded;
	}

private:
	const FRHITransition* Transition = nullptr;
	TArray<FRHITransitionInfo, FRDGArrayAllocator> Transitions;
	TArray<FRHITransientAliasingInfo, FRDGArrayAllocator> Aliases;
	ERHITransitionCreateFlags TransitionFlags = ERHITransitionCreateFlags::NoFence;
	ERHIPipeline PipelinesToBegin;
	ERHIPipeline PipelinesToEnd;
	TRHIPipelineArray<FRDGBarrierBatchEndId> BarriersToEnd;
	bool bTransitionNeeded = false;

#if RDG_ENABLE_DEBUG
	FRDGPassesByPipeline DebugPasses;
	TArray<FRDGParentResource*, FRDGArrayAllocator> DebugTransitionResources;
	TArray<FRDGParentResource*, FRDGArrayAllocator> DebugAliasingResources;
	const TCHAR* DebugName;
	ERHIPipeline DebugPipelinesToBegin;
	ERHIPipeline DebugPipelinesToEnd;
#endif

	friend class FRDGBarrierBatchEnd;
	friend class FRDGBarrierValidation;
};

using FRDGTransitionCreateQueue = TArray<FRDGBarrierBatchBegin*, FRDGArrayAllocator>;

class RENDERCORE_API FRDGBarrierBatchEnd
{
public:
	FRDGBarrierBatchEnd(FRDGPass* InPass, ERDGBarrierLocation InBarrierLocation)
		: Pass(InPass)
		, BarrierLocation(InBarrierLocation)
	{}

	/** Inserts a dependency on a begin batch. A begin batch can be inserted into more than one end batch. */
	void AddDependency(FRDGBarrierBatchBegin* BeginBatch);

	void Submit(FRHIComputeCommandList& RHICmdList, ERHIPipeline Pipeline);

	void Reserve(uint32 TransitionBatchCount)
	{
		Dependencies.Reserve(TransitionBatchCount);
	}

private:
	TArray<FRDGBarrierBatchBegin*, TInlineAllocator<4, FRDGArrayAllocator>> Dependencies;
	FRDGPass* Pass;
	ERDGBarrierLocation BarrierLocation;

	friend class FRDGBarrierBatchBegin;
	friend class FRDGBarrierValidation;
};

/** Base class of a render graph pass. */
class RENDERCORE_API FRDGPass
{
public:
	FRDGPass(FRDGEventName&& InName, FRDGParameterStruct InParameterStruct, ERDGPassFlags InFlags);
	FRDGPass(const FRDGPass&) = delete;
	virtual ~FRDGPass() = default;

#if RDG_ENABLE_DEBUG
	const TCHAR* GetName() const;
#else
	FORCEINLINE const TCHAR* GetName() const
	{
		return Name.GetTCHAR();
	}
#endif

	FORCEINLINE const FRDGEventName& GetEventName() const
	{
		return Name;
	}

	FORCEINLINE ERDGPassFlags GetFlags() const
	{
		return Flags;
	}

	FORCEINLINE ERHIPipeline GetPipeline() const
	{
		return Pipeline;
	}

	FORCEINLINE FRDGParameterStruct GetParameters() const
	{
		return ParameterStruct;
	}

	FORCEINLINE FRDGPassHandle GetHandle() const
	{
		return Handle;
	}

	bool IsParallelExecuteAllowed() const
	{
		return bParallelExecuteAllowed;
	}

	bool IsMergedRenderPassBegin() const
	{
		return !bSkipRenderPassBegin && bSkipRenderPassEnd;
	}

	bool IsMergedRenderPassEnd() const
	{
		return bSkipRenderPassBegin && !bSkipRenderPassEnd;
	}

	bool SkipRenderPassBegin() const
	{
		return bSkipRenderPassBegin;
	}

	bool SkipRenderPassEnd() const
	{
		return bSkipRenderPassEnd;
	}

	bool IsAsyncCompute() const
	{
		return Pipeline == ERHIPipeline::AsyncCompute;
	}

	bool IsAsyncComputeBegin() const
	{
		return bAsyncComputeBegin;
	}

	bool IsAsyncComputeEnd() const
	{
		return bAsyncComputeEnd;
	}

	bool IsGraphicsFork() const
	{
		return bGraphicsFork;
	}

	bool IsGraphicsJoin() const
	{
		return bGraphicsJoin;
	}

	bool IsCulled() const
	{
		return bCulled;
	}

	bool IsSentinel() const
	{
		return bSentinel;
	}

	const FRDGPassHandleArray& GetProducers() const
	{
		return Producers;
	}

	/** Returns the producer pass on the other pipeline, if it exists. */
	FRDGPassHandle GetCrossPipelineProducer() const
	{
		return CrossPipelineProducer;
	}

	/** Returns the consumer pass on the other pipeline, if it exists. */
	FRDGPassHandle GetCrossPipelineConsumer() const
	{
		return CrossPipelineConsumer;
	}

	/** Returns the graphics pass responsible for forking the async interval this pass is in. */
	FRDGPassHandle GetGraphicsForkPass() const
	{
		return GraphicsForkPass;
	}

	/** Returns the graphics pass responsible for joining the async interval this pass is in. */
	FRDGPassHandle GetGraphicsJoinPass() const
	{
		return GraphicsJoinPass;
	}

#if RDG_CPU_SCOPES
	FRDGCPUScopes GetCPUScopes() const
	{
		return CPUScopes;
	}
#endif

#if RDG_GPU_SCOPES
	FRDGGPUScopes GetGPUScopes() const
	{
		return GPUScopes;
	}
#endif

#if WITH_MGPU
	FRHIGPUMask GetGPUMask() const
	{
		return GPUMask;
	}
#endif

protected:
	FRDGBarrierBatchBegin& GetPrologueBarriersToBegin(FRDGAllocator& Allocator, FRDGTransitionCreateQueue& CreateQueue);
	FRDGBarrierBatchBegin& GetEpilogueBarriersToBeginForGraphics(FRDGAllocator& Allocator, FRDGTransitionCreateQueue& CreateQueue);
	FRDGBarrierBatchBegin& GetEpilogueBarriersToBeginForAsyncCompute(FRDGAllocator& Allocator, FRDGTransitionCreateQueue& CreateQueue);
	FRDGBarrierBatchBegin& GetEpilogueBarriersToBeginForAll(FRDGAllocator& Allocator, FRDGTransitionCreateQueue& CreateQueue);

	FRDGBarrierBatchBegin& GetEpilogueBarriersToBeginFor(FRDGAllocator& Allocator, FRDGTransitionCreateQueue& CreateQueue, ERHIPipeline PipelineForEnd)
	{
		switch (PipelineForEnd)
		{
		default: 
			checkNoEntry();
			// fall through

		case ERHIPipeline::Graphics:
			return GetEpilogueBarriersToBeginForGraphics(Allocator, CreateQueue);

		case ERHIPipeline::AsyncCompute:
			return GetEpilogueBarriersToBeginForAsyncCompute(Allocator, CreateQueue);

		case ERHIPipeline::All:
			return GetEpilogueBarriersToBeginForAll(Allocator, CreateQueue);
		}
	}

	FRDGBarrierBatchEnd& GetPrologueBarriersToEnd(FRDGAllocator& Allocator);
	FRDGBarrierBatchEnd& GetEpilogueBarriersToEnd(FRDGAllocator& Allocator);

	virtual void Execute(FRHIComputeCommandList& RHICmdList) {}

	// When r.RDG.Debug is enabled, this will include a full namespace path with event scopes included.
	IF_RDG_ENABLE_DEBUG(FString FullPathIfDebug);

	const FRDGEventName Name;
	const FRDGParameterStruct ParameterStruct;
	const ERDGPassFlags Flags;
	const ERHIPipeline Pipeline;
	FRDGPassHandle Handle;

	union
	{
		struct
		{
			/** Whether the render pass begin / end should be skipped. */
			uint32 bSkipRenderPassBegin : 1;
			uint32 bSkipRenderPassEnd : 1;

			/** (AsyncCompute only) Whether this is the first / last async compute pass in an async interval. */
			uint32 bAsyncComputeBegin : 1;
			uint32 bAsyncComputeEnd : 1;

			/** (Graphics only) Whether this is a graphics fork / join pass. */
			uint32 bGraphicsFork : 1;
			uint32 bGraphicsJoin : 1;

			/** Whether the pass only writes to resources in its render pass. */
			uint32 bRenderPassOnlyWrites : 1;

			/** Whether the pass is allowed to execute in parallel. */
			uint32 bParallelExecuteAllowed : 1;

			/** Whether this pass has non-RDG UAV outputs. */
			uint32 bHasExternalOutputs : 1;

			/** Whether this pass is a sentinel (prologue / epilogue) pass. */
			uint32 bSentinel : 1;

			/** Whether this pass has been culled. */
			uint32 bCulled : 1;

			/** Whether this pass does not contain parameters. */
			uint32 bEmptyParameters : 1;

			/** If set, dispatches to the RHI thread before executing this pass. */
			uint32 bDispatchAfterExecute : 1;

			/** If set, the pass should set its command list stat. */
			uint32 bSetCommandListStat : 1;

			/** If set, the pass will wait on the assigned mGPU temporal effect. */
			uint32 bWaitForTemporalEffect : 1;
		};
		uint32 PackedBits1 = 0;
	};

	union
	{
		// Task-specific bits which are written in a task in parallel with reads from the other set.
		struct
		{
			/** If set, marks the begin / end of a span of passes executed in parallel in a task. */
			uint32 bParallelExecuteBegin : 1;
			uint32 bParallelExecuteEnd : 1;

			/** If set, marks that a pass is executing in parallel. */
			uint32 bParallelExecute : 1;
		};
		uint32 PacketBits2 = 0;
	};

	/** Handle of the latest cross-pipeline producer and earliest cross-pipeline consumer. */
	FRDGPassHandle CrossPipelineProducer;
	FRDGPassHandle CrossPipelineConsumer;

	/** (AsyncCompute only) Graphics passes which are the fork / join for async compute interval this pass is in. */
	FRDGPassHandle GraphicsForkPass;
	FRDGPassHandle GraphicsJoinPass;

	/** The passes which are handling the epilogue / prologue barriers meant for this pass. */
	FRDGPassHandle PrologueBarrierPass;
	FRDGPassHandle EpilogueBarrierPass;

	/** Lists of producer passes. */
	FRDGPassHandleArray Producers;

	struct FTextureState
	{
		FTextureState() = default;

		FTextureState(FRDGTextureRef InTexture)
			: Texture(InTexture)
		{
			const uint32 SubresourceCount = Texture->GetSubresourceCount();
			State.Reserve(SubresourceCount);
			State.SetNum(SubresourceCount);
			MergeState.Reserve(SubresourceCount);
			MergeState.SetNum(SubresourceCount);
		}

		FRDGTextureRef Texture = nullptr;
		FRDGTextureTransientSubresourceState State;
		FRDGTextureTransientSubresourceStateIndirect MergeState;
		uint16 ReferenceCount = 0;
	};

	struct FBufferState
	{
		FBufferState() = default;

		FBufferState(FRDGBufferRef InBuffer)
			: Buffer(InBuffer)
		{}

		FRDGBufferRef Buffer = nullptr;
		FRDGSubresourceState State;
		FRDGSubresourceState* MergeState = nullptr;
		uint16 ReferenceCount = 0;
	};

	/** Maps textures / buffers to information on how they are used in the pass. */
	TArray<FTextureState, FRDGArrayAllocator> TextureStates;
	TArray<FBufferState, FRDGArrayAllocator> BufferStates;
	TArray<FRDGViewHandle, FRDGArrayAllocator> Views;
	TArray<FRDGUniformBufferHandle, FRDGArrayAllocator> UniformBuffers;

	/** Lists of pass parameters scheduled for begin during execution of this pass. */
	TArray<FRDGPass*, TInlineAllocator<1, FRDGArrayAllocator>> ResourcesToBegin;
	TArray<FRDGPass*, TInlineAllocator<1, FRDGArrayAllocator>> ResourcesToEnd;

	/** Split-barrier batches at various points of execution of the pass. */
	FRDGBarrierBatchBegin* PrologueBarriersToBegin = nullptr;
	FRDGBarrierBatchEnd PrologueBarriersToEnd;
	FRDGBarrierBatchBegin EpilogueBarriersToBeginForGraphics;
	FRDGBarrierBatchBegin* EpilogueBarriersToBeginForAsyncCompute = nullptr;
	FRDGBarrierBatchBegin* EpilogueBarriersToBeginForAll = nullptr;
	TArray<FRDGBarrierBatchBegin*, FRDGArrayAllocator> SharedEpilogueBarriersToBegin;
	FRDGBarrierBatchEnd* EpilogueBarriersToEnd = nullptr;

	EAsyncComputeBudget AsyncComputeBudget = EAsyncComputeBudget::EAll_4;

	uint16 ParallelPassSetIndex = 0;

#if WITH_MGPU
	FRHIGPUMask GPUMask;
#endif

	IF_RDG_CMDLIST_STATS(TStatId CommandListStat);

#if RDG_CPU_SCOPES
	FRDGCPUScopes CPUScopes;
	FRDGCPUScopeOpArrays CPUScopeOps;
#endif

#if RDG_GPU_SCOPES
	FRDGGPUScopes GPUScopes;
	FRDGGPUScopeOpArrays GPUScopeOpsPrologue;
	FRDGGPUScopeOpArrays GPUScopeOpsEpilogue;
#endif

#if RDG_GPU_SCOPES && RDG_ENABLE_TRACE
	const FRDGEventScope* TraceEventScope = nullptr;
#endif

#if RDG_ENABLE_TRACE
	TArray<FRDGTextureHandle, FRDGArrayAllocator> TraceTextures;
	TArray<FRDGBufferHandle, FRDGArrayAllocator> TraceBuffers;
#endif

	friend FRDGBuilder;
	friend FRDGPassRegistry;
	friend FRDGTrace;
	friend FRDGUserValidation;
};

/** Render graph pass with lambda execute function. */
template <typename ParameterStructType, typename ExecuteLambdaType>
class TRDGLambdaPass
	: public FRDGPass
{
	// Verify that the amount of stuff captured by the pass lambda is reasonable.
	static constexpr int32 kMaximumLambdaCaptureSize = 1024;
	static_assert(sizeof(ExecuteLambdaType) <= kMaximumLambdaCaptureSize, "The amount of data of captured for the pass looks abnormally high.");

	template <typename T>
	struct TLambdaTraits
		: TLambdaTraits<decltype(&T::operator())>
	{};
	template <typename ReturnType, typename ClassType, typename ArgType>
	struct TLambdaTraits<ReturnType(ClassType::*)(ArgType&) const>
	{
		using TRHICommandList = ArgType;
	};
	template <typename ReturnType, typename ClassType, typename ArgType>
	struct TLambdaTraits<ReturnType(ClassType::*)(ArgType&)>
	{
		using TRHICommandList = ArgType;
	};
	using TRHICommandList = typename TLambdaTraits<ExecuteLambdaType>::TRHICommandList;

public:
	static const bool kSupportsAsyncCompute = TIsSame<TRHICommandList, FRHIComputeCommandList>::Value;
	static const bool kSupportsRaster = TIsDerivedFrom<TRHICommandList, FRHICommandList>::IsDerived;

	TRDGLambdaPass(
		FRDGEventName&& InName,
		const FShaderParametersMetadata* InParameterMetadata,
		const ParameterStructType* InParameterStruct,
		ERDGPassFlags InPassFlags,
		ExecuteLambdaType&& InExecuteLambda)
		: FRDGPass(MoveTemp(InName), FRDGParameterStruct(InParameterStruct, InParameterMetadata), InPassFlags)
		, ExecuteLambda(MoveTemp(InExecuteLambda))
#if RDG_ENABLE_DEBUG
		, DebugParameterStruct(InParameterStruct)
#endif
	{
		checkf(kSupportsAsyncCompute || !EnumHasAnyFlags(InPassFlags, ERDGPassFlags::AsyncCompute),
			TEXT("Pass %s is set to use 'AsyncCompute', but the pass lambda's first argument is not FRHIComputeCommandList&."), GetName());

		bParallelExecuteAllowed = !TIsSame<TRHICommandList, FRHICommandListImmediate>::Value && !EnumHasAnyFlags(InPassFlags, ERDGPassFlags::NeverParallel);
	}

private:
	void Execute(FRHIComputeCommandList& RHICmdList) override
	{
		check(!kSupportsRaster || RHICmdList.IsGraphics());
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FRDGPass_Execute);
		RHICmdList.SetStaticUniformBuffers(ParameterStruct.GetStaticUniformBuffers());
		ExecuteLambda(static_cast<TRHICommandList&>(RHICmdList));
	}

	ExecuteLambdaType ExecuteLambda;

	IF_RDG_ENABLE_DEBUG(const ParameterStructType* DebugParameterStruct);
};

template <typename ExecuteLambdaType>
class TRDGEmptyLambdaPass
	: public TRDGLambdaPass<FEmptyShaderParameters, ExecuteLambdaType>
{
public:
	TRDGEmptyLambdaPass(FRDGEventName&& InName, ERDGPassFlags InPassFlags, ExecuteLambdaType&& InExecuteLambda)
		: TRDGLambdaPass<FEmptyShaderParameters, ExecuteLambdaType>(MoveTemp(InName), FEmptyShaderParameters::FTypeInfo::GetStructMetadata(), &EmptyShaderParameters, InPassFlags, MoveTemp(InExecuteLambda))
	{}

private:
	FEmptyShaderParameters EmptyShaderParameters;
	friend class FRDGBuilder;
};

/** Render graph pass used for the prologue / epilogue passes. */
class FRDGSentinelPass final
	: public FRDGPass
{
public:
	FRDGSentinelPass(FRDGEventName&& Name, ERDGPassFlags InPassFlagsToAdd = ERDGPassFlags::None)
		: FRDGPass(MoveTemp(Name), FRDGParameterStruct(&EmptyShaderParameters, FEmptyShaderParameters::FTypeInfo::GetStructMetadata()), ERDGPassFlags::NeverCull | InPassFlagsToAdd)
	{
		bSentinel = 1;
	}

private:
	FEmptyShaderParameters EmptyShaderParameters;
};

#include "RenderGraphParameters.inl"