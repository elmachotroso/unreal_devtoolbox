// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraGpuComputeDispatch.h"

#include "Async/Async.h"
#include "CanvasTypes.h"
#include "ClearQuad.h"
#include "Engine/Canvas.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"

#include "NiagaraAsyncGpuTraceHelper.h"
#include "NiagaraDataInterfaceRW.h"
#if NIAGARA_COMPUTEDEBUG_ENABLED
#include "NiagaraGpuComputeDebug.h"
#endif
#include "NiagaraGPUProfilerInterface.h"
#include "NiagaraGpuReadbackManager.h"
#include "NiagaraRenderer.h"
#include "NiagaraScriptExecutionContext.h"
#include "NiagaraSystemGpuComputeProxy.h"
#include "NiagaraShader.h"
#include "NiagaraShaderParticleID.h"
#include "NiagaraSortingGPU.h"
#include "NiagaraStats.h"
#include "NiagaraRenderViewDataManager.h"
#include "NiagaraWorldManager.h"
#include "RHI.h"
#include "Runtime/Renderer/Private/SceneRendering.h"
#include "Runtime/Renderer/Private/PostProcess/SceneRenderTargets.h"
#include "PipelineStateCache.h"
#include "SceneUtils.h"
#include "ShaderParameterUtils.h"
#include "Renderer/Private/ScenePrivate.h"

DECLARE_CYCLE_STAT(TEXT("Niagara Dispatch Setup"), STAT_NiagaraGPUDispatchSetup_RT, STATGROUP_Niagara);
DECLARE_CYCLE_STAT(TEXT("GPU Emitter Dispatch [RT]"), STAT_NiagaraGPUSimTick_RT, STATGROUP_Niagara);
DECLARE_CYCLE_STAT(TEXT("GPU Data Readback [RT]"), STAT_NiagaraGPUReadback_RT, STATGROUP_Niagara);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Niagara GPU Sim"), STAT_GPU_NiagaraSim, STATGROUP_GPU);
DECLARE_DWORD_COUNTER_STAT(TEXT("# GPU Particles"), STAT_NiagaraGPUParticles, STATGROUP_Niagara);
DECLARE_DWORD_COUNTER_STAT(TEXT("# GPU Sorted Particles"), STAT_NiagaraGPUSortedParticles, STATGROUP_Niagara);
DECLARE_DWORD_COUNTER_STAT(TEXT("# GPU Sorted Buffers"), STAT_NiagaraGPUSortedBuffers, STATGROUP_Niagara);
DECLARE_DWORD_COUNTER_STAT(TEXT("Readback latency (frames)"), STAT_NiagaraReadbackLatency, STATGROUP_Niagara);
DECLARE_DWORD_COUNTER_STAT(TEXT("# GPU Dispatches"), STAT_NiagaraGPUDispatches, STATGROUP_Niagara);

DECLARE_GPU_STAT_NAMED(NiagaraGPU, TEXT("Niagara"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUSimulation, TEXT("Niagara GPU Simulation"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUClearIDTables, TEXT("NiagaraGPU Clear ID Tables"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUComputeFreeIDs, TEXT("Niagara GPU Compute All Free IDs"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUComputeFreeIDsEmitter, TEXT("Niagara GPU Compute Emitter Free IDs"));
DECLARE_GPU_STAT_NAMED(NiagaraGPUSorting, TEXT("Niagara GPU sorting"));

uint32 FNiagaraComputeExecutionContext::TickCounter = 0;

int32 GNiagaraGpuSubmitCommandHint = 0;
static FAutoConsoleVariableRef CVarNiagaraGpuSubmitCommandHint(
	TEXT("fx.NiagaraGpuSubmitCommandHint"),
	GNiagaraGpuSubmitCommandHint,
	TEXT("If greater than zero, we use this value to submit commands after the number of dispatches have been issued."),
	ECVF_Default
);

int32 GNiagaraGpuLowLatencyTranslucencyEnabled = 0;
static FAutoConsoleVariableRef CVarNiagaraGpuLowLatencyTranslucencyEnabled(
	TEXT("fx.NiagaraGpuLowLatencyTranslucencyEnabled"),
	GNiagaraGpuLowLatencyTranslucencyEnabled,
	TEXT("When enabled translucent materials can use the current frames simulation data no matter which tick pass Niagara uses.\n")
	TEXT("This can result in an additional data buffer being required but will reduce any latency when using view uniform buffer / depth buffer / distance fields / etc"),
	ECVF_Default
);

int32 GNiagaraBatcherFreeBufferEarly = 1;
static FAutoConsoleVariableRef CVarNiagaraBatcherFreeBufferEarly(
	TEXT("fx.NiagaraBatcher.FreeBufferEarly"),
	GNiagaraBatcherFreeBufferEarly,
	TEXT("Will take the path to release GPU buffers when possible.\n")
	TEXT("This will reduce memory pressure but can result in more allocations if you buffers ping pong from zero particles to many."),
	ECVF_Default
);

const FName FNiagaraGpuComputeDispatch::Name(TEXT("FNiagaraGpuComputeDispatch"));

namespace FNiagaraGpuComputeDispatchLocal
{
#if WITH_MGPU
	const FName TemporalEffectBuffersName("FNiagaraGpuComputeDispatch_Buffers");
	const FName TemporalEffectTexturesName("FNiagaraGpuComputeDispatch_Textures");
#endif // WITH_MGPU

	int32 GTickFlushMaxQueuedFrames = 10;
	static FAutoConsoleVariableRef CVarNiagaraTickFlushMaxQueuedFrames(
		TEXT("fx.Niagara.Batcher.TickFlush.MaxQueuedFrames"),
		GTickFlushMaxQueuedFrames,
		TEXT("The number of unprocessed frames with queued ticks before we process them.\n")
		TEXT("The larger the number the more data we process in a single frame, this is generally only a concern when the application does not have focus."),
		ECVF_Default
	);

	int32 GTickFlushMode = 1;
	static FAutoConsoleVariableRef CVarNiagaraTickFlushMode(
		TEXT("fx.Niagara.Batcher.TickFlush.Mode"),
		GTickFlushMode,
		TEXT("What to do when we go over our max queued frames.\n")
		TEXT("0 = Keep ticks queued, can result in a long pause when gaining focus again.\n")
		TEXT("1 = (Default) Process all queued ticks with dummy view / buffer data, may result in incorrect simulation due to missing depth collisions, etc.\n")
		TEXT("2 = Kill all pending ticks, may result in incorrect simulation due to missing frames of data, i.e. a particle reset.\n"),
		ECVF_Default
	);

	static int32 GAddDispatchGroupDrawEvent = 0;
	static FAutoConsoleVariableRef CVarAddDispatchGroupDrawEvent(
		TEXT("fx.Niagara.Batcher.AddDispatchGroupDrawEvent"),
		GAddDispatchGroupDrawEvent,
		TEXT("Add a draw event marker around each dispatch group."),
		ECVF_Default
	);

	#if !WITH_EDITOR
		constexpr int32 GDebugLogging = 0;
	#else
		static int32 GDebugLogging = 0;
		static FAutoConsoleVariableRef CVarNiagaraDebugLogging(
			TEXT("fx.Niagara.Batcher.DebugLogging"),
			GDebugLogging,
			TEXT("Enables a lot of spew to the log to debug the batcher."),
			ECVF_Default
		);
	#endif

	template<typename TTransitionArrayType>
	static void AddDataBufferTransitions(TTransitionArrayType& BeforeTransitionArray, TTransitionArrayType& AfterTransitionArray, FNiagaraDataBuffer* DestinationData, ERHIAccess BeforeState = ERHIAccess::SRVMask, ERHIAccess AfterState = ERHIAccess::UAVCompute)
	{
		if (FRHIUnorderedAccessView* FloatUAV = DestinationData->GetGPUBufferFloat().UAV )
		{
			BeforeTransitionArray.Emplace(FloatUAV, BeforeState, AfterState);
			AfterTransitionArray.Emplace(FloatUAV, AfterState, BeforeState);
		}
		if (FRHIUnorderedAccessView* HalfUAV = DestinationData->GetGPUBufferHalf().UAV)
		{
			BeforeTransitionArray.Emplace(HalfUAV, BeforeState, AfterState);
			AfterTransitionArray.Emplace(HalfUAV, AfterState, BeforeState);
		}
		if (FRHIUnorderedAccessView* IntUAV = DestinationData->GetGPUBufferInt().UAV)
		{
			BeforeTransitionArray.Emplace(IntUAV, BeforeState, AfterState);
			AfterTransitionArray.Emplace(IntUAV, AfterState, BeforeState);
		}
	}
}

FFXSystemInterface* FNiagaraGpuComputeDispatch::GetInterface(const FName& InName)
{
	return InName == Name ? this : nullptr;
}

FNiagaraGpuComputeDispatch::FNiagaraGpuComputeDispatch(ERHIFeatureLevel::Type InFeatureLevel, EShaderPlatform InShaderPlatform, FGPUSortManager* InGPUSortManager)
	: FNiagaraGpuComputeDispatchInterface(InShaderPlatform, InFeatureLevel)
	, GPUSortManager(InGPUSortManager)
	, CachedViewRect(0, 0, 64, 64)
{
	// Register the batcher callback in the GPUSortManager.
	// The callback is used to generate the initial keys and values for the GPU sort tasks,
	// the values being the sorted particle indices used by the Niagara renderers.
	// The registration also involves defining the list of flags possibly used in GPUSortManager::AddTask()
	if (GPUSortManager)
	{
		GPUSortManager->Register(
			FGPUSortKeyGenDelegate::CreateLambda(
				[this](FRHICommandListImmediate& RHICmdList, int32 BatchId, int32 NumElementsInBatch, EGPUSortFlags Flags, FRHIUnorderedAccessView* KeysUAV, FRHIUnorderedAccessView* ValuesUAV)
				{
					GenerateSortKeys(RHICmdList, BatchId, NumElementsInBatch, Flags, KeysUAV, ValuesUAV);
				}
			),
			EGPUSortFlags::AnyKeyPrecision | EGPUSortFlags::AnyKeyGenLocation | EGPUSortFlags::AnySortLocation | EGPUSortFlags::ValuesAsInt32,
			Name
		);

		if (FNiagaraUtilities::AllowComputeShaders(GetShaderPlatform()))
		{
			// Because of culled indirect draw args, we have to update the draw indirect buffer after the sort key generation
			GPUSortManager->PostPreRenderEvent.AddLambda(
				[this](FRHICommandListImmediate& RHICmdList)
				{
					GPUInstanceCounterManager.UpdateDrawIndirectBuffers(this, RHICmdList, ENiagaraGPUCountUpdatePhase::PreOpaque);
				#if WITH_MGPU
					TransferMultiGPUBufers(RHICmdList, ENiagaraGpuComputeTickStage::PreInitViews);
				#endif // WITH_MGPU
				}
			);

			GPUSortManager->PostPostRenderEvent.AddLambda
			(
				[this](FRHICommandListImmediate& RHICmdList)
				{
					GPUInstanceCounterManager.UpdateDrawIndirectBuffers(this, RHICmdList, ENiagaraGPUCountUpdatePhase::PostOpaque);
				#if WITH_MGPU
					TransferMultiGPUBufers(RHICmdList, ENiagaraGpuComputeTickStage::PostOpaqueRender);
				#endif // WITH_MGPU
				}
			);
		}
	}

	GlobalCBufferLayout  = new FNiagaraRHIUniformBufferLayout(TEXT("Niagara GPU Global CBuffer"),  sizeof(FNiagaraGlobalParameters));
	SystemCBufferLayout  = new FNiagaraRHIUniformBufferLayout(TEXT("Niagara GPU System CBuffer"),  sizeof(FNiagaraSystemParameters));
	OwnerCBufferLayout   = new FNiagaraRHIUniformBufferLayout(TEXT("Niagara GPU Owner CBuffer"),   sizeof(FNiagaraOwnerParameters));
	EmitterCBufferLayout = new FNiagaraRHIUniformBufferLayout(TEXT("Niagara GPU Emitter CBuffer"), sizeof(FNiagaraEmitterParameters));


	AsyncGpuTraceHelper.Reset(new FNiagaraAsyncGpuTraceHelper(InShaderPlatform, InFeatureLevel, this));

#if NIAGARA_COMPUTEDEBUG_ENABLED
	GpuComputeDebugPtr.Reset(new FNiagaraGpuComputeDebug(FeatureLevel));
#endif
#if WITH_NIAGARA_GPU_PROFILER
	GPUProfilerPtr = MakeUnique<FNiagaraGPUProfiler>(uintptr_t(static_cast<FNiagaraGpuComputeDispatchInterface*>(this)));
#endif
	GpuReadbackManagerPtr.Reset(new FNiagaraGpuReadbackManager());
	EmptyUAVPoolPtr.Reset(new FNiagaraEmptyUAVPool());
}

FNiagaraGpuComputeDispatch::~FNiagaraGpuComputeDispatch()
{
	FinishDispatches();

	AsyncGpuTraceHelper->Reset();

	GlobalCBufferLayout = nullptr;
	SystemCBufferLayout = nullptr;
	OwnerCBufferLayout = nullptr;
	EmitterCBufferLayout = nullptr;

	FPrimitiveSceneInfo::OnGPUSceneInstancesAllocated.RemoveAll(this);
	FPrimitiveSceneInfo::OnGPUSceneInstancesFreed.RemoveAll(this);
}

void FNiagaraGpuComputeDispatch::AddGpuComputeProxy(FNiagaraSystemGpuComputeProxy* ComputeProxy)
{
	check(ComputeProxy->ComputeDispatchIndex == INDEX_NONE);

	const ENiagaraGpuComputeTickStage::Type TickStage = ComputeProxy->GetComputeTickStage();
	ComputeProxy->ComputeDispatchIndex = ProxiesPerStage[TickStage].Num();
	ProxiesPerStage[TickStage].Add(ComputeProxy);

	NumProxiesThatRequireDistanceFieldData	+= ComputeProxy->RequiresDistanceFieldData() ? 1 : 0;
	NumProxiesThatRequireDepthBuffer		+= ComputeProxy->RequiresDepthBuffer() ? 1 : 0;
	NumProxiesThatRequireEarlyViewData		+= ComputeProxy->RequiresEarlyViewData() ? 1 : 0;
	NumProxiesThatRequireRayTracingScene	+= ComputeProxy->RequiresRayTracingScene() ? 1 : 0;
}

void FNiagaraGpuComputeDispatch::RemoveGpuComputeProxy(FNiagaraSystemGpuComputeProxy* ComputeProxy)
{
	check(ComputeProxy->ComputeDispatchIndex != INDEX_NONE);

	const int32 TickStage = int32(ComputeProxy->GetComputeTickStage());
	const int32 ProxyIndex = ComputeProxy->ComputeDispatchIndex;
	check(ProxiesPerStage[TickStage][ProxyIndex] == ComputeProxy);

	ProxiesPerStage[TickStage].RemoveAtSwap(ProxyIndex);
	if (ProxiesPerStage[TickStage].IsValidIndex(ProxyIndex))
	{
		ProxiesPerStage[TickStage][ProxyIndex]->ComputeDispatchIndex = ProxyIndex;
	}
	ComputeProxy->ComputeDispatchIndex = INDEX_NONE;

	NumProxiesThatRequireDistanceFieldData	-= ComputeProxy->RequiresDistanceFieldData() ? 1 : 0;
	NumProxiesThatRequireDepthBuffer		-= ComputeProxy->RequiresDepthBuffer() ? 1 : 0;
	NumProxiesThatRequireEarlyViewData		-= ComputeProxy->RequiresEarlyViewData() ? 1 : 0;
	NumProxiesThatRequireRayTracingScene	-= ComputeProxy->RequiresRayTracingScene() ? 1 : 0;

#if NIAGARA_COMPUTEDEBUG_ENABLED
	if (FNiagaraGpuComputeDebug* GpuComputeDebug = GpuComputeDebugPtr.Get())
	{
		GpuComputeDebug->OnSystemDeallocated(ComputeProxy->GetSystemInstanceID());
	}
#endif
#if !UE_BUILD_SHIPPING
	GpuDebugReadbackInfos.RemoveAll(
		[&](const FDebugReadbackInfo& Info)
		{
			// In the unlikely event we have one in the queue make sure it's marked as complete with no data in it
			if ( Info.InstanceID == ComputeProxy->GetSystemInstanceID())
			{
				Info.DebugInfo->Frame.CopyFromGPUReadback(nullptr, nullptr, nullptr, 0, 0, 0, 0, 0);
				Info.DebugInfo->bWritten = true;
			}
			return Info.InstanceID == ComputeProxy->GetSystemInstanceID();
		}
	);
#endif
}

void FNiagaraGpuComputeDispatch::BuildConstantBuffers(FNiagaraGPUSystemTick& Tick) const
{
	if (!Tick.InstanceCount)
	{
		return;
	}

	TArrayView<FNiagaraComputeInstanceData> InstanceData = Tick.GetInstances();

	// first go through and figure out if we need to support interpolated spawning
	bool HasInterpolationParameters = false;
	bool HasMultipleStages = false;
	for (const FNiagaraComputeInstanceData& Instance : InstanceData)
	{
		HasInterpolationParameters = HasInterpolationParameters || Instance.Context->HasInterpolationParameters;
		HasMultipleStages = HasMultipleStages || Instance.bHasMultipleStages;
	}

	int32 BoundParameterCounts[FNiagaraGPUSystemTick::UBT_NumTypes][2];
	FMemory::Memzero(BoundParameterCounts);

	for (const FNiagaraComputeInstanceData& Instance : InstanceData)
	{
		for (int32 InterpIt = 0; InterpIt < (HasInterpolationParameters ? 2 : 1); ++InterpIt)
		{
			BoundParameterCounts[FNiagaraGPUSystemTick::UBT_Global][InterpIt] += Instance.Context->GPUScript_RT->IsGlobalConstantBufferUsed_RenderThread(InterpIt) ? 1 : 0;
			BoundParameterCounts[FNiagaraGPUSystemTick::UBT_System][InterpIt] += Instance.Context->GPUScript_RT->IsSystemConstantBufferUsed_RenderThread(InterpIt) ? 1 : 0;
			BoundParameterCounts[FNiagaraGPUSystemTick::UBT_Owner][InterpIt] += Instance.Context->GPUScript_RT->IsOwnerConstantBufferUsed_RenderThread(InterpIt) ? 1 : 0;
		}
	}

	const int32 InterpScale = HasInterpolationParameters ? 2 : 1;
	const int32 BufferCount = InterpScale * (FNiagaraGPUSystemTick::UBT_NumSystemTypes + FNiagaraGPUSystemTick::UBT_NumInstanceTypes * Tick.InstanceCount);

	Tick.UniformBuffers.Empty(BufferCount);
	Tick.UniformBuffers.AddDefaulted(BufferCount);

	const FRHIUniformBufferLayout* SystemLayouts[FNiagaraGPUSystemTick::UBT_NumSystemTypes] =
	{
		GlobalCBufferLayout,
		SystemCBufferLayout,
		OwnerCBufferLayout
	};

	int32 CurrentBuffer = 0;
	for (int32 InterpIt=0; InterpIt < InterpScale; ++InterpIt)
	{
		for (int32 SystemTypeIt = FNiagaraGPUSystemTick::UBT_FirstSystemType; SystemTypeIt < FNiagaraGPUSystemTick::UBT_NumSystemTypes; ++SystemTypeIt)
		{
			if (BoundParameterCounts[SystemTypeIt][InterpIt])
			{
				Tick.UniformBuffers[CurrentBuffer] = RHICreateUniformBuffer(
					Tick.GetUniformBufferSource((FNiagaraGPUSystemTick::EUniformBufferType) SystemTypeIt, nullptr, !InterpIt),
					SystemLayouts[SystemTypeIt],
					((BoundParameterCounts[SystemTypeIt][InterpIt] > 1) || HasMultipleStages)
						? EUniformBufferUsage::UniformBuffer_SingleFrame
						: EUniformBufferUsage::UniformBuffer_SingleDraw);
			}
			++CurrentBuffer;
		}

		// Build emitter constant buffers
		for (const FNiagaraComputeInstanceData& Instance : InstanceData)
		{
			if (InterpIt == 0 || Instance.Context->HasInterpolationParameters)
			{
				if (Instance.Context->GPUScript_RT->IsEmitterConstantBufferUsed_RenderThread(InterpIt))
				{
					Tick.UniformBuffers[CurrentBuffer] = RHICreateUniformBuffer(
						Tick.GetUniformBufferSource(FNiagaraGPUSystemTick::UBT_Emitter, &Instance, !InterpIt),
						EmitterCBufferLayout,
						Instance.bHasMultipleStages ? EUniformBufferUsage::UniformBuffer_SingleFrame : EUniformBufferUsage::UniformBuffer_SingleDraw
					);
				}
			}
			++CurrentBuffer;
		}

		// Build external constant buffers
		for (const FNiagaraComputeInstanceData& Instance : InstanceData)
		{
			if (InterpIt == 0 || Instance.Context->HasInterpolationParameters)
			{
				FNiagaraRHIUniformBufferLayout* ExternalCBufferLayout = Instance.Context->ExternalCBufferLayout;
				if (Instance.Context->GPUScript_RT->IsExternalConstantBufferUsed_RenderThread(InterpIt))
				{
					if (ensure(ExternalCBufferLayout && (ExternalCBufferLayout->Resources.Num() > 0 || ExternalCBufferLayout->ConstantBufferSize > 0)))
					{
						Tick.UniformBuffers[CurrentBuffer] = RHICreateUniformBuffer(
							Tick.GetUniformBufferSource(FNiagaraGPUSystemTick::UBT_External, &Instance, !InterpIt),
							ExternalCBufferLayout,
							Instance.bHasMultipleStages ? EUniformBufferUsage::UniformBuffer_SingleFrame : EUniformBufferUsage::UniformBuffer_SingleDraw
						);
					}
				}
			}
			++CurrentBuffer;
		}
	}
}

void FNiagaraGpuComputeDispatch::Tick(UWorld* World, float DeltaTime)
{
	check(IsInGameThread());
	ENQUEUE_RENDER_COMMAND(NiagaraPumpBatcher)(
		[RT_NiagaraBatcher=this](FRHICommandListImmediate& RHICmdList)
		{
			RT_NiagaraBatcher->ProcessPendingTicksFlush(RHICmdList, false);
			RT_NiagaraBatcher->GetGPUInstanceCounterManager().FlushIndirectArgsPool();
		}
	);
}

void FNiagaraGpuComputeDispatch::FlushPendingTicks_GameThread()
{
	check(IsInGameThread());
	ENQUEUE_RENDER_COMMAND(NiagaraFlushPendingTicks)(
		[RT_NiagaraBatcher=this](FRHICommandListImmediate& RHICmdList)
		{
			RT_NiagaraBatcher->ProcessPendingTicksFlush(RHICmdList, true);
			RT_NiagaraBatcher->GetGPUInstanceCounterManager().FlushIndirectArgsPool();
		}
	);
}

void FNiagaraGpuComputeDispatch::ProcessPendingTicksFlush(FRHICommandListImmediate& RHICmdList, bool bForceFlush)
{
	// No ticks are pending
	bool bHasTicks = false;
	for ( int iTickStage=0; iTickStage < ENiagaraGpuComputeTickStage::Max; ++iTickStage)
	{
		if ( ProxiesPerStage[iTickStage].Num() > 0 )
		{
			bHasTicks = true;
			break;
		}
	}

	if ( !bHasTicks )
	{
		return;
	}

	// We have pending ticks increment our counter, once we cross the threshold we will perform the appropriate operation
	++FramesBeforeTickFlush;
	if (!bForceFlush && (FramesBeforeTickFlush < uint32(FNiagaraGpuComputeDispatchLocal::GTickFlushMaxQueuedFrames)) )
	{
		return;
	}
	FramesBeforeTickFlush = 0;

	switch (FNiagaraGpuComputeDispatchLocal::GTickFlushMode)
	{
		// Do nothing
		default:
		case 0:
		{
			//UE_LOG(LogNiagara, Log, TEXT("FNiagaraGpuComputeDispatch: Queued ticks (%d) are building up, this may cause a stall when released."), Ticks_RT.Num());
			break;
		}

		// Process all the pending ticks that have built up
		case 1:
		{
			//UE_LOG(LogNiagara, Log, TEXT("FNiagaraGpuComputeDispatch: Queued ticks are being Processed due to not rendering.  This may result in undesirable simulation artifacts."));

			// Make a temporary ViewInfo
			//-TODO: We could gather some more information here perhaps?
			FMemMark Mark(FMemStack::Get());

			FSceneViewFamily ViewFamily(FSceneViewFamily::ConstructionValues(nullptr, nullptr, FEngineShowFlags(ESFIM_Game))
				.SetTime(FGameTime())
				.SetGammaCorrection(1.0f));

			FSceneViewInitOptions ViewInitOptions;
			ViewInitOptions.ViewFamily = &ViewFamily;
			ViewInitOptions.SetViewRectangle(CachedViewRect);
			ViewInitOptions.ViewOrigin = FVector::ZeroVector;
			ViewInitOptions.ViewRotationMatrix = FMatrix::Identity;
			ViewInitOptions.ProjectionMatrix = FMatrix::Identity;

			FViewInfo* DummyView = new(FMemStack::Get()) FViewInfo(ViewInitOptions);

			DummyView->ViewRect = DummyView->UnscaledViewRect;
			DummyView->CachedViewUniformShaderParameters = MakeUnique<FViewUniformShaderParameters>();

			FBox UnusedVolumeBounds[TVC_MAX];
			DummyView->SetupUniformBufferParameters(UnusedVolumeBounds, TVC_MAX, *DummyView->CachedViewUniformShaderParameters);

			DummyView->ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(*DummyView->CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);

			TConstArrayView<FViewInfo> DummyViews = MakeArrayView(DummyView, 1);
			const bool bAllowGPUParticleUpdate = true;

			// Notify that we are about to begin rendering the 'scene' this is required because some RHIs will ClearState
			// in the event of submitting commands, i.e. when we write a fence, or indeed perform a manual flush.
			RHICmdList.BeginScene();

			// Execute all ticks that we can support without invalid simulations
			FRDGBuilder GraphBuilder(RHICmdList);
			CreateSystemTextures(GraphBuilder);
			PreInitViews(GraphBuilder, bAllowGPUParticleUpdate);
			AddPass(GraphBuilder, RDG_EVENT_NAME("UpdateDrawIndirectBuffers - PreOpaque"),
				[this](FRHICommandListImmediate& RHICmdList)
				{
					GPUInstanceCounterManager.UpdateDrawIndirectBuffers(this, RHICmdList, ENiagaraGPUCountUpdatePhase::PreOpaque);
				}
			);
			PostInitViews(GraphBuilder, DummyViews, bAllowGPUParticleUpdate);
			PostRenderOpaque(GraphBuilder, DummyViews, bAllowGPUParticleUpdate);
			AddPass(GraphBuilder, RDG_EVENT_NAME("UpdateDrawIndirectBuffers - PostOpaque"),
				[this](FRHICommandListImmediate& RHICmdList)
				{
					GPUInstanceCounterManager.UpdateDrawIndirectBuffers(this, RHICmdList, ENiagaraGPUCountUpdatePhase::PostOpaque);
				}
			);
			GraphBuilder.Execute();

			// Properly clear the reference to ViewUniformBuffer before memstack wipes the memory
			DummyView->~FViewInfo();

			// We have completed flushing the commands
			RHICmdList.EndScene();
			break;
		}

		// Kill all the pending ticks that have built up
		case 2:
		{
			//UE_LOG(LogNiagara, Log, TEXT("FNiagaraGpuComputeDispatch: Queued ticks are being Destroyed due to not rendering.  This may result in undesirable simulation artifacts."));

			FinishDispatches();
			AsyncGpuTraceHelper->Reset();

			break;
		}
	}
}

void FNiagaraGpuComputeDispatch::FinishDispatches()
{
	check(IsInRenderingThread());

	for (int iTickStage = 0; iTickStage < ENiagaraGpuComputeTickStage::Max; ++iTickStage)
	{
		for (FNiagaraSystemGpuComputeProxy* ComputeProxy : ProxiesPerStage[iTickStage])
		{
			ComputeProxy->ReleaseTicks(GPUInstanceCounterManager);
		}
	}

	for (FNiagaraGpuDispatchList& DispatchList : DispatchListPerStage)
	{
		DispatchList.DispatchGroups.Empty();
		if (DispatchList.CountsToRelease.Num() > 0)
		{
			GPUInstanceCounterManager.FreeEntryArray(DispatchList.CountsToRelease);
			DispatchList.CountsToRelease.Empty();
		}
	}
}

void FNiagaraGpuComputeDispatch::ResetDataInterfaces(FRHICommandList& RHICmdList, const FNiagaraGPUSystemTick& Tick, const FNiagaraComputeInstanceData& InstanceData) const
{
	// Note: All stages will contain the same bindings so if they are valid for one they are valid for all, this could change in the future
	const FNiagaraShaderRef& ComputeShader = InstanceData.Context->GPUScript_RT->GetShader(0);
	const TMemoryImageArray<FNiagaraDataInterfaceParamRef>& DIParameters = ComputeShader->GetDIParameters();

	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : InstanceData.DataInterfaceProxies)
	{
		const FNiagaraDataInterfaceParamRef& DIParam = DIParameters[InterfaceIndex];
		if (DIParam.Parameters.IsValid())
		{
			const FNiagaraDataInterfaceArgs TmpContext(Interface, Tick.SystemInstanceID, Tick.SystemGpuComputeProxy->GetSystemLWCTile(), this);
			Interface->ResetData(RHICmdList, TmpContext);
		}
		InterfaceIndex++;
	}
}

FNiagaraDataInterfaceProxyRW* FNiagaraGpuComputeDispatch::FindIterationInterface(FNiagaraComputeInstanceData* Instance, const uint32 SimulationStageIndex) const
{
	// Determine if the iteration is outputting to a custom data size
	return Instance->FindIterationInterface(SimulationStageIndex);
}

void FNiagaraGpuComputeDispatch::PreStageInterface(FRHICommandList& RHICmdList, const FNiagaraGPUSystemTick& Tick, const FNiagaraComputeInstanceData& InstanceData, const FNiagaraSimStageData& SimStageData, TSet<FNiagaraDataInterfaceProxy*>& ProxiesToFinalize) const
{
	// Note: All stages will contain the same bindings so if they are valid for one they are valid for all, this could change in the future
	const FNiagaraShaderRef& ComputeShader = InstanceData.Context->GPUScript_RT->GetShader(0);
	const TMemoryImageArray<FNiagaraDataInterfaceParamRef>& DIParameters = ComputeShader->GetDIParameters();

	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : InstanceData.DataInterfaceProxies)
	{
		const FNiagaraDataInterfaceParamRef& DIParam = DIParameters[InterfaceIndex];
		if (DIParam.Parameters.IsValid())
		{
			const FNiagaraDataInterfaceStageArgs TmpContext(Interface, Tick.SystemInstanceID, Tick.SystemGpuComputeProxy->GetSystemLWCTile(), this, &InstanceData, &SimStageData, InstanceData.IsOutputStage(Interface, SimStageData.StageIndex), InstanceData.IsIterationStage(Interface, SimStageData.StageIndex));
			Interface->PreStage(RHICmdList, TmpContext);

			if (Interface->RequiresPreStageFinalize())
			{
				ProxiesToFinalize.Add(Interface);
			}
		}
		InterfaceIndex++;
	}
}

void FNiagaraGpuComputeDispatch::PostStageInterface(FRHICommandList& RHICmdList, const FNiagaraGPUSystemTick& Tick, const FNiagaraComputeInstanceData& InstanceData, const FNiagaraSimStageData& SimStageData, TSet<FNiagaraDataInterfaceProxy*>& ProxiesToFinalize) const
{
	// Note: All stages will contain the same bindings so if they are valid for one they are valid for all, this could change in the future
	const FNiagaraShaderRef& ComputeShader = InstanceData.Context->GPUScript_RT->GetShader(0);
	const TMemoryImageArray<FNiagaraDataInterfaceParamRef>& DIParameters = ComputeShader->GetDIParameters();

	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : InstanceData.DataInterfaceProxies)
	{
		const FNiagaraDataInterfaceParamRef& DIParam = DIParameters[InterfaceIndex];
		if (DIParam.Parameters.IsValid())
		{
			const FNiagaraDataInterfaceStageArgs TmpContext(Interface, Tick.SystemInstanceID, Tick.SystemGpuComputeProxy->GetSystemLWCTile(), this, &InstanceData, &SimStageData, InstanceData.IsOutputStage(Interface, SimStageData.StageIndex), InstanceData.IsIterationStage(Interface, SimStageData.StageIndex));
			Interface->PostStage(RHICmdList, TmpContext);

			if (Interface->RequiresPostStageFinalize())
			{
				ProxiesToFinalize.Add(Interface);
			}
		}
		InterfaceIndex++;
	}
}

void FNiagaraGpuComputeDispatch::PostSimulateInterface(FRHICommandList& RHICmdList, const FNiagaraGPUSystemTick& Tick, const FNiagaraComputeInstanceData& InstanceData) const
{
	// Note: All stages will contain the same bindings so if they are valid for one they are valid for all, this could change in the future
	const FNiagaraShaderRef& ComputeShader = InstanceData.Context->GPUScript_RT->GetShader(0);
	const TMemoryImageArray<FNiagaraDataInterfaceParamRef>& DIParameters = ComputeShader->GetDIParameters();

	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : InstanceData.DataInterfaceProxies)
	{
		const FNiagaraDataInterfaceParamRef& DIParam = DIParameters[InterfaceIndex];
		if (DIParam.Parameters.IsValid())
		{
			const FNiagaraDataInterfaceArgs TmpContext(Interface, Tick.SystemInstanceID, Tick.SystemGpuComputeProxy->GetSystemLWCTile(), this);
			Interface->PostSimulate(RHICmdList, TmpContext);
		}
		InterfaceIndex++;
	}
}


void FNiagaraGpuComputeDispatch::UpdateFreeIDsListSizesBuffer(FRHICommandList& RHICmdList, uint32 NumInstances)
{
	if (NumInstances > NumAllocatedFreeIDListSizes)
	{
		constexpr uint32 ALLOC_CHUNK_SIZE = 128;
		NumAllocatedFreeIDListSizes = Align(NumInstances, ALLOC_CHUNK_SIZE);
		if (FreeIDListSizesBuffer.Buffer)
		{
			FreeIDListSizesBuffer.Release();
		}
		FreeIDListSizesBuffer.Initialize(TEXT("NiagaraFreeIDListSizes"), sizeof(uint32), NumAllocatedFreeIDListSizes, EPixelFormat::PF_R32_SINT, ERHIAccess::UAVCompute, BUF_Static);
	}

	{
		SCOPED_DRAW_EVENT(RHICmdList, NiagaraGPUComputeClearFreeIDListSizes);
		RHICmdList.Transition(FRHITransitionInfo(FreeIDListSizesBuffer.UAV, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute));
		NiagaraFillGPUIntBuffer(RHICmdList, FeatureLevel, FreeIDListSizesBuffer, 0);
	}
}

void FNiagaraGpuComputeDispatch::UpdateFreeIDBuffers(FRHICommandList& RHICmdList, TConstArrayView<FNiagaraComputeExecutionContext*> Instances)
{
	if (Instances.Num() == 0)
	{
		return;
	}

	SCOPED_DRAW_EVENT(RHICmdList, NiagaraGPUComputeFreeIDs);
	SCOPED_GPU_STAT(RHICmdList, NiagaraGPUComputeFreeIDs);

	TArray<FRHITransitionInfo, TMemStackAllocator<>> TransitionsBefore;
	TransitionsBefore.Emplace(FreeIDListSizesBuffer.UAV, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute);
	RHICmdList.Transition(TransitionsBefore);

	check((uint32)Instances.Num() <= NumAllocatedFreeIDListSizes);

	RHICmdList.BeginUAVOverlap(FreeIDListSizesBuffer.UAV);
	for (uint32 iInstance = 0; iInstance < (uint32)Instances.Num(); ++iInstance)
	{
		FNiagaraComputeExecutionContext* ComputeContext = Instances[iInstance];
		FNiagaraDataSet* MainDataSet = ComputeContext->MainDataSet;
		FNiagaraDataBuffer* CurrentData = ComputeContext->MainDataSet->GetCurrentData();

		SCOPED_DRAW_EVENTF(RHICmdList, NiagaraGPUComputeFreeIDsEmitter, TEXT("Update Free ID Buffer - %s"), ComputeContext->GetDebugSimName());
		NiagaraComputeGPUFreeIDs(RHICmdList, FeatureLevel, MainDataSet->GetGPUNumAllocatedIDs(), CurrentData->GetGPUIDToIndexTable().SRV, MainDataSet->GetGPUFreeIDs(), FreeIDListSizesBuffer, iInstance);
	}
	RHICmdList.EndUAVOverlap(FreeIDListSizesBuffer.UAV);
}

void FNiagaraGpuComputeDispatch::DumpDebugFrame()
{
	// Anything doing?
	bool bHasAnyWork = false;
	for (int iTickStage = 0; iTickStage < ENiagaraGpuComputeTickStage::Max; ++iTickStage)
	{
		bHasAnyWork |= DispatchListPerStage[iTickStage].HasWork();
	}
	if ( !bHasAnyWork )
	{
		return;
	}

	// Dump Frame
	UE_LOG(LogNiagara, Warning, TEXT("====== BatcherFrame(%d)"), GFrameNumberRenderThread);

	for (int iTickStage = 0; iTickStage < ENiagaraGpuComputeTickStage::Max; ++iTickStage)
	{
		if (!DispatchListPerStage[iTickStage].HasWork())
		{
			continue;
		}

		FNiagaraGpuDispatchList& DispatchList = DispatchListPerStage[iTickStage];
		UE_LOG(LogNiagara, Warning, TEXT("==== TickStage(%d) TotalGroups(%d)"), iTickStage, DispatchList.DispatchGroups.Num());

		for ( int iDispatchGroup=0; iDispatchGroup < DispatchList.DispatchGroups.Num(); ++iDispatchGroup )
		{
			const FNiagaraGpuDispatchGroup& DispatchGroup = DispatchListPerStage[iTickStage].DispatchGroups[iDispatchGroup];

			if (DispatchGroup.TicksWithPerInstanceData.Num() > 0)
			{
				UE_LOG(LogNiagara, Warning, TEXT("====== TicksWithPerInstanceData(%s)"), DispatchGroup.TicksWithPerInstanceData.Num());
				for (FNiagaraGPUSystemTick* Tick : DispatchGroup.TicksWithPerInstanceData)
				{
					for (const auto& Pair : Tick->DIInstanceData->InterfaceProxiesToOffsets)
					{
						FNiagaraDataInterfaceProxy* Proxy = Pair.Key;
						UE_LOG(LogNiagara, Warning, TEXT("Proxy(%s)"), *Proxy->SourceDIName.ToString());
					}
				}
			}

			UE_LOG(LogNiagara, Warning, TEXT("====== DispatchGroup(%d)"), iDispatchGroup);
			for ( const FNiagaraGpuDispatchInstance& DispatchInstance : DispatchGroup.DispatchInstances )
			{
				const FNiagaraSimStageData& SimStageData = DispatchInstance.SimStageData;
				const FNiagaraComputeInstanceData& InstanceData = DispatchInstance.InstanceData;

				TStringBuilder<512> Builder;
				Builder.Appendf(TEXT("Proxy(%p) "), DispatchInstance.Tick.SystemGpuComputeProxy);
				Builder.Appendf(TEXT("ComputeContext(%p) "), InstanceData.Context);
				Builder.Appendf(TEXT("Emitter(%s) "), InstanceData.Context->GetDebugSimName());
				Builder.Appendf(TEXT("Stage(%d | %s) "), SimStageData.StageIndex, *SimStageData.StageMetaData->SimulationStageName.ToString());

				if (InstanceData.bResetData)
				{
					Builder.Append(TEXT("ResetData "));
				}

				if (InstanceData.Context->MainDataSet->RequiresPersistentIDs())
				{
					Builder.Append(TEXT("HasPersistentIDs "));
				}

				if ( DispatchInstance.SimStageData.bFirstStage )
				{
					Builder.Append(TEXT("FirstStage "));
				}

				if ( DispatchInstance.SimStageData.bLastStage )
				{
					Builder.Append(TEXT("LastStage "));
				}

				if (DispatchInstance.SimStageData.bSetDataToRender)
				{
					Builder.Append(TEXT("SetDataToRender "));
				}
					

				if (InstanceData.Context->EmitterInstanceReadback.GPUCountOffset != INDEX_NONE)
				{
					if (InstanceData.Context->EmitterInstanceReadback.GPUCountOffset == SimStageData.SourceCountOffset)
					{
						Builder.Appendf(TEXT("ReadbackSource(%d) "), InstanceData.Context->EmitterInstanceReadback.CPUCount);
					}
				}
				Builder.Appendf(TEXT("Source(%p 0x%08x %d) "), SimStageData.Source, SimStageData.SourceCountOffset, SimStageData.SourceNumInstances);
				Builder.Appendf(TEXT("Destination(%p 0x%08x %d) "), SimStageData.Destination, SimStageData.DestinationCountOffset, SimStageData.DestinationNumInstances);
				Builder.Appendf(TEXT("Iteration(%d | %s) "), SimStageData.IterationIndex, SimStageData.AlternateIterationSource ? *SimStageData.AlternateIterationSource->SourceDIName.ToString() : TEXT("Particles"));
				UE_LOG(LogNiagara, Warning, TEXT("%s"), Builder.ToString());
			}

			if (DispatchGroup.FreeIDUpdates.Num() > 0)
			{
				UE_LOG(LogNiagara, Warning, TEXT("====== FreeIDUpdates"));
				for (FNiagaraComputeExecutionContext* ComputeContext : DispatchGroup.FreeIDUpdates)
				{
					UE_LOG(LogNiagara, Warning, TEXT("ComputeContext(%p) Emitter(%s)"), ComputeContext, ComputeContext->GetDebugSimName());
				}
			}
		}
		if (DispatchList.CountsToRelease.Num() > 0)
		{
			UE_LOG(LogNiagara, Warning, TEXT("====== CountsToRelease"));

			const int NumPerLine = 16;

			TStringBuilder<512> StringBuilder;
			for ( int32 i=0; i < DispatchList.CountsToRelease.Num(); ++i )
			{
				const bool bFirst = (i % NumPerLine) == 0;
				const bool bLast = ((i % NumPerLine) == NumPerLine - 1) || (i == DispatchList.CountsToRelease.Num() -1);

				if ( !bFirst )
				{
					StringBuilder.Append(TEXT(", "));
				}
				StringBuilder.Appendf(TEXT("0x%08x"), DispatchList.CountsToRelease[i]);

				if ( bLast )
				{
					UE_LOG(LogNiagara, Warning, TEXT("%s"), StringBuilder.ToString());
					StringBuilder.Reset();
				}
			}
		}
	}
}

void FNiagaraGpuComputeDispatch::UpdateInstanceCountManager(FRHICommandListImmediate& RHICmdList)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FNiagaraGpuComputeDispatch_UpdateInstanceCountManager);

	// Resize dispatch buffer count
	//-OPT: No need to iterate over all the ticks, we can store this as ticks are queued
	{
		int32 TotalDispatchCount = 0;
		for (int iTickStage = 0; iTickStage < ENiagaraGpuComputeTickStage::Max; ++iTickStage)
		{
			for (FNiagaraSystemGpuComputeProxy* ComputeProxy : ProxiesPerStage[iTickStage])
			{
				for (FNiagaraGPUSystemTick& Tick : ComputeProxy->PendingTicks)
				{
					TotalDispatchCount += (int32)Tick.TotalDispatches;

					for ( FNiagaraComputeInstanceData& InstanceData : Tick.GetInstances() )
					{
						if (InstanceData.bResetData)
						{
							InstanceData.Context->EmitterInstanceReadback.GPUCountOffset = INDEX_NONE;
						}
					}
				}
			}
		}
		GPUInstanceCounterManager.ResizeBuffers(RHICmdList, TotalDispatchCount);
	}

	// Consume any pending readbacks that are ready
	{
		SCOPE_CYCLE_COUNTER(STAT_NiagaraGPUReadback_RT);
		if ( const uint32* Counts = GPUInstanceCounterManager.GetGPUReadback() )
		{
			if (FNiagaraGpuComputeDispatchLocal::GDebugLogging)
			{
				UE_LOG(LogNiagara, Warning, TEXT("====== BatcherFrame(%d) Readback Complete"), GFrameNumberRenderThread);
			}

			for (int iTickStage=0; iTickStage < ENiagaraGpuComputeTickStage::Max; ++iTickStage)
			{
				for (FNiagaraSystemGpuComputeProxy* ComputeProxy : ProxiesPerStage[iTickStage])
				{
					for ( FNiagaraComputeExecutionContext* ComputeContext : ComputeProxy->ComputeContexts )
					{
						if (ComputeContext->EmitterInstanceReadback.GPUCountOffset == INDEX_NONE )
						{
							continue;
						}

						const uint32 DeadInstanceCount = ComputeContext->EmitterInstanceReadback.CPUCount - Counts[ComputeContext->EmitterInstanceReadback.GPUCountOffset];
						if ( DeadInstanceCount <= ComputeContext->CurrentNumInstances_RT )
						{
							ComputeContext->CurrentNumInstances_RT -= DeadInstanceCount;
						}
						if (FNiagaraGpuComputeDispatchLocal::GDebugLogging)
						{
							UE_LOG(LogNiagara, Warning, TEXT("ComputeContext(%p) Emitter(%s) DeadInstances(%d) CountReleased(0x%08x)"), ComputeContext, ComputeContext->GetDebugSimName(), DeadInstanceCount, ComputeContext->EmitterInstanceReadback.GPUCountOffset);
						}

						// Readback complete
						ComputeContext->EmitterInstanceReadback.GPUCountOffset = INDEX_NONE;
					}
				}
			}

			// Release the readback buffer
			GPUInstanceCounterManager.ReleaseGPUReadback();
		}
	}
}

void FNiagaraGpuComputeDispatch::PrepareTicksForProxy(FRHICommandListImmediate& RHICmdList, FNiagaraSystemGpuComputeProxy* ComputeProxy, FNiagaraGpuDispatchList& GpuDispatchList)
{
	for (FNiagaraComputeExecutionContext* ComputeContext : ComputeProxy->ComputeContexts)
	{
		ComputeContext->CurrentMaxInstances_RT = 0;
		ComputeContext->CurrentMaxAllocateInstances_RT = 0;
		ComputeContext->BufferSwapsThisFrame_RT = 0;
		ComputeContext->FinalDispatchGroup_RT = INDEX_NONE;
		ComputeContext->FinalDispatchGroupInstance_RT = INDEX_NONE;
	}

	if (ComputeProxy->PendingTicks.Num() == 0)
	{
		return;
	}

	const bool bEnqueueCountReadback = !GPUInstanceCounterManager.HasPendingGPUReadback();

	// Set final tick flag
	ComputeProxy->PendingTicks.Last().bIsFinalTick = true;

	// Process ticks
	int32 iTickStartDispatchGroup = 0;

	for (FNiagaraGPUSystemTick& Tick : ComputeProxy->PendingTicks)
	{
		int32 iInstanceStartDispatchGroup = iTickStartDispatchGroup;
		int32 iInstanceCurrDispatchGroup = iTickStartDispatchGroup;
		bool bHasFreeIDUpdates = false;

		// Track that we need to consume per instance data before executing the ticks
		//if (Tick.DIInstanceData)
		//{
		//	GpuDispatchList.PreAllocateGroups(iTickStartDispatchGroup + 1);
		//	GpuDispatchList.DispatchGroups[iTickStartDispatchGroup].TicksWithPerInstanceData.Add(&Tick);
		//}

		// Iterate over all instances preparing our number of instances
		for (FNiagaraComputeInstanceData& InstanceData : Tick.GetInstances())
		{
			FNiagaraComputeExecutionContext* ComputeContext = InstanceData.Context;

			// Instance requires a reset?
			if (InstanceData.bResetData)
			{
				ComputeContext->CurrentNumInstances_RT = 0;
				if (ComputeContext->CountOffset_RT != INDEX_NONE)
				{
					GpuDispatchList.CountsToRelease.Add(ComputeContext->CountOffset_RT);
					ComputeContext->CountOffset_RT = INDEX_NONE;
				}
			}

			// If shader is not ready don't do anything
			if (!ComputeContext->GPUScript_RT->IsShaderMapComplete_RenderThread())
			{
				continue;
			}

			// Nothing to dispatch?
			if ( InstanceData.TotalDispatches == 0 )
			{
				continue;
			}

#if WITH_EDITOR
			//-TODO: Validate feature level in the editor as when using the preview mode we can be using the wrong shaders for the renderer type.
			//       i.e. We may attempt to sample the gbuffer / depth using deferred scene textures rather than mobile which will crash.
			if (ComputeContext->GPUScript_RT->GetFeatureLevel() != FeatureLevel)
			{
				if (ComputeProxy->GetComputeTickStage() == ENiagaraGpuComputeTickStage::PostOpaqueRender)
				{
					if (bRaisedWarningThisFrame == false)
					{
						bRaisedWarningThisFrame = true;
						AsyncTask(
							ENamedThreads::GameThread,
							[MessageId=uint64(this), DebugSimName=ComputeContext->GetDebugSimFName()]()
							{
								GEngine->AddOnScreenDebugMessage(MessageId, 1.f, FColor::White, *FString::Printf(TEXT("GPU Simulation(%s) will not show in preview mode, as we may sample from wrong SceneTextures buffer."), *DebugSimName.ToString()));
							}
						);
					}
					continue;
				}
			}
#endif

			// Determine this instances start dispatch group, in the case of emitter dependencies (i.e. particle reads) we need to continue rather than starting again
			iInstanceStartDispatchGroup = InstanceData.bStartNewOverlapGroup ? iInstanceCurrDispatchGroup : iInstanceStartDispatchGroup;
			iInstanceCurrDispatchGroup = iInstanceStartDispatchGroup;

			// Pre-allocator groups
			GpuDispatchList.PreAllocateGroups(iInstanceCurrDispatchGroup + InstanceData.TotalDispatches);

			// Calculate instance counts
			const uint32 MaxBufferInstances = ComputeContext->MainDataSet->GetMaxInstanceCount();
			const uint32 PrevNumInstances = ComputeContext->CurrentNumInstances_RT;

			ComputeContext->CurrentNumInstances_RT = PrevNumInstances + InstanceData.SpawnInfo.SpawnRateInstances + InstanceData.SpawnInfo.EventSpawnTotal;
			ComputeContext->CurrentNumInstances_RT = FMath::Min(ComputeContext->CurrentNumInstances_RT, MaxBufferInstances);

			// Calculate new maximum count
			ComputeContext->CurrentMaxInstances_RT = FMath::Max(ComputeContext->CurrentMaxInstances_RT, ComputeContext->CurrentNumInstances_RT);

			if (!GNiagaraBatcherFreeBufferEarly || (ComputeContext->CurrentMaxInstances_RT > 0))
			{
				ComputeContext->CurrentMaxAllocateInstances_RT = FMath::Max3(ComputeContext->CurrentMaxAllocateInstances_RT, ComputeContext->CurrentMaxInstances_RT, InstanceData.SpawnInfo.MaxParticleCount);
			}
			else
			{
				ComputeContext->CurrentMaxAllocateInstances_RT = FMath::Max(ComputeContext->CurrentMaxAllocateInstances_RT, ComputeContext->CurrentMaxInstances_RT);
			}

			bHasFreeIDUpdates |= ComputeContext->MainDataSet->RequiresPersistentIDs();

			//-OPT: Do we need this test?  Can remove in favor of MaxUpdateIterations
			bool bFirstStage = true;
			for (int32 SimStageIndex=0; SimStageIndex < ComputeContext->SimStageInfo.Num(); ++SimStageIndex)
			{
				const FSimulationStageMetaData& SimStageMetaData = ComputeContext->SimStageInfo[SimStageIndex];
				if (InstanceData.NumIterationsPerStage[SimStageIndex] == 0)
				{
					continue;
				}

				FNiagaraDataInterfaceProxyRW* IterationInterface = InstanceData.FindIterationInterface(SimStageIndex);
				for ( int32 IterationIndex=0; IterationIndex < InstanceData.NumIterationsPerStage[SimStageIndex]; ++IterationIndex )
				{
					// Build SimStage data
					FNiagaraGpuDispatchGroup& DispatchGroup = GpuDispatchList.DispatchGroups[iInstanceCurrDispatchGroup++];
					FNiagaraGpuDispatchInstance& DispatchInstance = DispatchGroup.DispatchInstances.Emplace_GetRef(Tick, InstanceData);
					FNiagaraSimStageData& SimStageData = DispatchInstance.SimStageData;
					SimStageData.bFirstStage = bFirstStage;
					SimStageData.StageIndex = SimStageIndex;
					SimStageData.IterationIndex = IterationIndex;
					SimStageData.StageMetaData = &SimStageMetaData;
					SimStageData.AlternateIterationSource = IterationInterface;

					bFirstStage = false;

					FNiagaraDataBuffer* SourceData = ComputeContext->bHasTickedThisFrame_RT ? ComputeContext->GetPrevDataBuffer() : ComputeContext->MainDataSet->GetCurrentData();

					// This stage does not modify particle data, i.e. read only or not related to particles at all
					if (!SimStageData.StageMetaData->bWritesParticles)
					{
						SimStageData.Source = SourceData;
						SimStageData.SourceCountOffset = ComputeContext->CountOffset_RT;
						SimStageData.SourceNumInstances = ComputeContext->CurrentNumInstances_RT;
						SimStageData.Destination = nullptr;
						SimStageData.DestinationCountOffset = ComputeContext->CountOffset_RT;
						SimStageData.DestinationNumInstances = ComputeContext->CurrentNumInstances_RT;
					}
					// This stage writes particles but will not kill any, we can use the buffer as both source and destination
					else if (SimStageData.StageMetaData->bPartialParticleUpdate)
					{
						SimStageData.Source = nullptr;
						SimStageData.SourceCountOffset = ComputeContext->CountOffset_RT;
						SimStageData.SourceNumInstances = ComputeContext->CurrentNumInstances_RT;
						SimStageData.Destination = SourceData;
						SimStageData.DestinationCountOffset = ComputeContext->CountOffset_RT;
						SimStageData.DestinationNumInstances = ComputeContext->CurrentNumInstances_RT;
					}
					// This stage may kill particles, we need to allocate a new destination buffer
					else
					{
						SimStageData.Source = SourceData;
						SimStageData.SourceCountOffset = ComputeContext->CountOffset_RT;
						//-TODO: This is a little odd, perhaps we need to change the preallocate
						SimStageData.SourceNumInstances = SimStageIndex == 0 && IterationIndex == 0 ? PrevNumInstances : ComputeContext->CurrentNumInstances_RT;
						SimStageData.Destination = ComputeContext->GetNextDataBuffer();
						SimStageData.DestinationCountOffset = GPUInstanceCounterManager.AcquireEntry();
						SimStageData.DestinationNumInstances = ComputeContext->CurrentNumInstances_RT;

						ComputeContext->AdvanceDataBuffer();
						ComputeContext->CountOffset_RT = SimStageData.DestinationCountOffset;
						ComputeContext->bHasTickedThisFrame_RT = true;

						// If we are the last tick then we may want to enqueue for a readback
						// Note: Do not pull count from SimStageData as a reset tick will be INDEX_NONE
						check(SimStageData.SourceCountOffset != INDEX_NONE || SimStageData.SourceNumInstances == 0);
						if (SimStageData.SourceCountOffset != INDEX_NONE)
						{
							if ( bEnqueueCountReadback && Tick.bIsFinalTick && (ComputeContext->EmitterInstanceReadback.GPUCountOffset == INDEX_NONE))
							{
								bRequiresReadback = true;
								ComputeContext->EmitterInstanceReadback.CPUCount = SimStageData.SourceNumInstances;
								ComputeContext->EmitterInstanceReadback.GPUCountOffset = SimStageData.SourceCountOffset;
							}
							GpuDispatchList.CountsToRelease.Add(SimStageData.SourceCountOffset);
						}
					}
				}
			}

			// Set this as the last stage and store the final dispatch group / instance
			FNiagaraGpuDispatchGroup& FinalDispatchGroup = GpuDispatchList.DispatchGroups[iInstanceCurrDispatchGroup - 1];
			FinalDispatchGroup.DispatchInstances.Last().SimStageData.bLastStage = true;

			ComputeContext->FinalDispatchGroup_RT = iInstanceCurrDispatchGroup - 1;
			ComputeContext->FinalDispatchGroupInstance_RT = FinalDispatchGroup.DispatchInstances.Num() - 1;

			// Keep track of where the next set of dispatch should occur
			iTickStartDispatchGroup = FMath::Max(iTickStartDispatchGroup, iInstanceCurrDispatchGroup);
		}

		// Accumulate Free ID updates
		// Note: These must be done at the end of the tick due to the way spawned instances read from the free list
		if (bHasFreeIDUpdates)
		{
			FNiagaraGpuDispatchGroup& FinalDispatchGroup = GpuDispatchList.DispatchGroups[iInstanceCurrDispatchGroup - 1];
			for (FNiagaraComputeInstanceData& InstanceData : Tick.GetInstances())
			{
				FNiagaraComputeExecutionContext* ComputeContext = InstanceData.Context;
				if (!ComputeContext->GPUScript_RT->IsShaderMapComplete_RenderThread())
				{
					continue;
				}

				if ( ComputeContext->MainDataSet->RequiresPersistentIDs() )
				{
					FinalDispatchGroup.FreeIDUpdates.Add(ComputeContext);
				}
			}
		}

		// Build constant buffers for tick
		BuildConstantBuffers(Tick);
	}

	// Now that all ticks have been processed we can adjust our output buffers to the correct size
	// We will also set the translucent data to render, i.e. this frames data.
	for (FNiagaraComputeExecutionContext* ComputeContext : ComputeProxy->ComputeContexts)
	{
		if (!ComputeContext->bHasTickedThisFrame_RT)
		{
			continue;
		}

		// Ensure we set the data to render as the context may have been dropped during a multi-tick
		check(ComputeContext->FinalDispatchGroup_RT != INDEX_NONE);
		FNiagaraGpuDispatchGroup& FinalDispatchGroup = GpuDispatchList.DispatchGroups[ComputeContext->FinalDispatchGroup_RT];
		FinalDispatchGroup.DispatchInstances[ComputeContext->FinalDispatchGroupInstance_RT].SimStageData.bSetDataToRender = true;

		// We need to store the current data from the main data set as we will be temporarily stomping it during multi-ticking
		ComputeContext->DataSetOriginalBuffer_RT = ComputeContext->MainDataSet->GetCurrentData();

		//-OPT: We should allocate all GPU free IDs together since they require a transition
		if (ComputeContext->MainDataSet->RequiresPersistentIDs())
		{
			ComputeContext->MainDataSet->AllocateGPUFreeIDs(ComputeContext->CurrentMaxAllocateInstances_RT + 1, RHICmdList, FeatureLevel, ComputeContext->GetDebugSimName());
		}

		// Allocate space for the buffers we need to perform ticking.  In cases of multiple ticks or multiple write stages we need 3 buffers (current rendered and two simulation buffers).
		//-OPT: We can batch the allocation of persistent IDs together so the compute shaders overlap
		const uint32 NumBuffersToResize = FMath::Min<uint32>(ComputeContext->BufferSwapsThisFrame_RT, UE_ARRAY_COUNT(ComputeContext->DataBuffers_RT));
		for (uint32 i=0; i < NumBuffersToResize; ++i)
		{
			ComputeContext->DataBuffers_RT[i]->AllocateGPU(RHICmdList, ComputeContext->CurrentMaxAllocateInstances_RT + 1, FeatureLevel, ComputeContext->GetDebugSimName());
		}

		// Ensure we don't keep multi-tick buffers around longer than they are required by releasing them
		for (uint32 i=NumBuffersToResize; i < UE_ARRAY_COUNT(ComputeContext->DataBuffers_RT); ++i)
		{
			ComputeContext->DataBuffers_RT[i]->ReleaseGPU();
		}

		// RDG will defer the Niagara dispatches until the graph is executed.
		// Therefore we need to setup the DataToRender for MeshProcessors & sorting to use the correct data,
		// that is anything that happens before PostRenderOpaque
		if ((ComputeProxy->GetComputeTickStage() == ENiagaraGpuComputeTickStage::PreInitViews) || (ComputeProxy->GetComputeTickStage() == ENiagaraGpuComputeTickStage::PostInitViews))
		{
			FNiagaraDataBuffer* FinalBuffer = ComputeContext->GetPrevDataBuffer();
			FinalBuffer->SetGPUInstanceCountBufferOffset(ComputeContext->CountOffset_RT);
			FinalBuffer->SetNumInstances(ComputeContext->CurrentNumInstances_RT);
			FinalBuffer->SetGPUDataReadyStage(ComputeProxy->GetComputeTickStage());
			ComputeContext->SetDataToRender(FinalBuffer);
		}
		// When low latency translucency is enabled we can setup the final buffer / final count here.
		// This will allow our mesh processor commands to pickup the data for the same frame.
		// This allows simulations that use the depth buffer, for example, to execute with no latency
		else if ( GNiagaraGpuLowLatencyTranslucencyEnabled )
		{
			FNiagaraDataBuffer* FinalBuffer = ComputeContext->GetPrevDataBuffer();
			FinalBuffer->SetGPUInstanceCountBufferOffset(ComputeContext->CountOffset_RT);
			FinalBuffer->SetNumInstances(ComputeContext->CurrentNumInstances_RT);
			FinalBuffer->SetGPUDataReadyStage(ComputeProxy->GetComputeTickStage());
			ComputeContext->SetTranslucentDataToRender(FinalBuffer);
		}
	}
}

void FNiagaraGpuComputeDispatch::PrepareAllTicks(FRHICommandListImmediate& RHICmdList)
{
	for (int iTickStage=0; iTickStage < ENiagaraGpuComputeTickStage::Max; ++iTickStage)
	{
		for (FNiagaraSystemGpuComputeProxy* ComputeProxy : ProxiesPerStage[iTickStage])
		{
			PrepareTicksForProxy(RHICmdList, ComputeProxy, DispatchListPerStage[iTickStage]);
		}
	}
}

void FNiagaraGpuComputeDispatch::ExecuteTicks(FRHICommandList& RHICmdList, FRHIUniformBuffer* ViewUniformBuffer, ENiagaraGpuComputeTickStage::Type TickStage)
{
#if WITH_MGPU
	WaitForMultiGPUBuffers(RHICmdList, TickStage);
#endif // WITH_MGPU

	// Anything to execute for this stage
	FNiagaraGpuDispatchList& DispatchList = DispatchListPerStage[TickStage];
	if ( !DispatchList.HasWork() )
	{
		return;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(FNiagaraGpuComputeDispatch_ExecuteTicks);
	SCOPED_DRAW_EVENTF(RHICmdList, FNiagaraGpuComputeDispatch_ExecuteTicks, TEXT("FNiagaraGpuComputeDispatch_ExecuteTicks - TickStage(%d)"), TickStage);
	SCOPE_CYCLE_COUNTER(STAT_NiagaraGPUSimTick_RT);
	SCOPED_GPU_STAT(RHICmdList, NiagaraGPUSimulation);

	FMemMark Mark(FMemStack::Get());
	TArray<FRHITransitionInfo, TMemStackAllocator<>> TransitionsBefore;
	TArray<FRHITransitionInfo, TMemStackAllocator<>> TransitionsAfter;
	TArray<FNiagaraDataBuffer*, TMemStackAllocator<>> IDToIndexInit;

#if WITH_NIAGARA_GPU_PROFILER
	GPUProfilerPtr->BeginStage(RHICmdList, TickStage, DispatchList.DispatchGroups.Num());
	const int32 StageStartTotalDispatches = TotalDispatchesThisFrame;
#endif

	for ( const FNiagaraGpuDispatchGroup& DispatchGroup : DispatchList.DispatchGroups )
	{
		SCOPED_CONDITIONAL_DRAW_EVENT(RHICmdList, NiagaraDispatchGroup, FNiagaraGpuComputeDispatchLocal::GAddDispatchGroupDrawEvent);

		const bool bIsFirstGroup = &DispatchGroup == &DispatchList.DispatchGroups[0];
		const bool bIsLastGroup = &DispatchGroup == &DispatchList.DispatchGroups.Last();

		// Generate transitions and discover free / ID table updates
		TransitionsBefore.Reserve(DispatchGroup.DispatchInstances.Num() * 3);
		TransitionsAfter.Reserve(DispatchGroup.DispatchInstances.Num() * 3);
		for (const FNiagaraGpuDispatchInstance& DispatchInstance : DispatchGroup.DispatchInstances)
		{
			if (FNiagaraDataBuffer* DestinationBuffer = DispatchInstance.SimStageData.Destination)
			{
				FNiagaraGpuComputeDispatchLocal::AddDataBufferTransitions(TransitionsBefore, TransitionsAfter, DestinationBuffer);
			}

			FNiagaraComputeExecutionContext* ComputeContext = DispatchInstance.InstanceData.Context;
			const bool bRequiresPersistentIDs = ComputeContext->MainDataSet->RequiresPersistentIDs();
			if (bRequiresPersistentIDs)
			{
				if ( FNiagaraDataBuffer* IDtoIndexBuffer = DispatchInstance.SimStageData.Destination )
				{
					IDToIndexInit.Add(IDtoIndexBuffer);
					TransitionsBefore.Emplace(IDtoIndexBuffer->GetGPUIDToIndexTable().UAV, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);
					TransitionsAfter.Emplace(IDtoIndexBuffer->GetGPUIDToIndexTable().UAV, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
				}
			}
		}

		TransitionsBefore.Emplace(GPUInstanceCounterManager.GetInstanceCountBuffer().UAV, bIsFirstGroup ? FNiagaraGPUInstanceCountManager::kCountBufferDefaultState : ERHIAccess::UAVCompute, ERHIAccess::UAVCompute);
		if (bIsLastGroup)
		{
			TransitionsAfter.Emplace(GPUInstanceCounterManager.GetInstanceCountBuffer().UAV, ERHIAccess::UAVCompute, FNiagaraGPUInstanceCountManager::kCountBufferDefaultState);
		}

		if (DispatchGroup.FreeIDUpdates.Num() > 0)
		{
			for (FNiagaraComputeExecutionContext* ComputeContext : DispatchGroup.FreeIDUpdates )
			{
				TransitionsAfter.Emplace(ComputeContext->MainDataSet->GetGPUFreeIDs().UAV, ERHIAccess::SRVCompute, ERHIAccess::UAVCompute);
			}
		}

		// Consume per tick data from the game thread
		//for (FNiagaraGPUSystemTick* Tick : DispatchGroup.TicksWithPerInstanceData)
		//{
		//	uint8* BasePointer = reinterpret_cast<uint8*>(Tick->DIInstanceData->PerInstanceDataForRT);

		//	for (const auto& Pair : Tick->DIInstanceData->InterfaceProxiesToOffsets)
		//	{
		//		FNiagaraDataInterfaceProxy* Proxy = Pair.Key;
		//		uint8* InstanceDataPtr = BasePointer + Pair.Value;
		//		Proxy->ConsumePerInstanceDataFromGameThread(InstanceDataPtr, Tick->SystemInstanceID);
		//	}
		//}

		// Execute Before Transitions
		RHICmdList.Transition(TransitionsBefore);
		TransitionsBefore.Reset();

		// Initialize the IDtoIndex tables
		if (IDToIndexInit.Num() > 0)
		{
			SCOPED_DRAW_EVENT(RHICmdList, NiagaraGPUComputeClearIDToIndexBuffer);

			TArray<FRHITransitionInfo, TMemStackAllocator<>> IDToIndexTransitions;
			IDToIndexTransitions.Reserve(IDToIndexInit.Num());

			for (FNiagaraDataBuffer* IDtoIndexBuffer : IDToIndexInit)
			{
				NiagaraFillGPUIntBuffer(RHICmdList, FeatureLevel, IDtoIndexBuffer->GetGPUIDToIndexTable(), INDEX_NONE);
				IDToIndexTransitions.Emplace(IDtoIndexBuffer->GetGPUIDToIndexTable().UAV, ERHIAccess::UAVCompute, ERHIAccess::UAVCompute);
			}
			IDToIndexInit.Reset();
			RHICmdList.Transition(IDToIndexTransitions);
		}

		// Execute PreStage
		{
			TSet<FNiagaraDataInterfaceProxy*> ProxiesToFinalize;
			for (const FNiagaraGpuDispatchInstance& DispatchInstance : DispatchGroup.DispatchInstances)
			{
				PreStageInterface(RHICmdList, DispatchInstance.Tick, DispatchInstance.InstanceData, DispatchInstance.SimStageData, ProxiesToFinalize);
			}

			for (FNiagaraDataInterfaceProxy* ProxyToFinalize : ProxiesToFinalize)
			{
				ProxyToFinalize->FinalizePreStage(RHICmdList, this);
			}
		}

		// Execute Stage
		RHICmdList.BeginUAVOverlap(GPUInstanceCounterManager.GetInstanceCountBuffer().UAV);
		for (const FNiagaraGpuDispatchInstance& DispatchInstance : DispatchGroup.DispatchInstances)
		{
			++FNiagaraComputeExecutionContext::TickCounter;
			if (DispatchInstance.InstanceData.bResetData && DispatchInstance.SimStageData.bFirstStage )
			{
				ResetDataInterfaces(RHICmdList, DispatchInstance.Tick, DispatchInstance.InstanceData);
			}

			FNiagaraGpuProfileScope GpuProfileDispatchScope(RHICmdList, this, DispatchInstance);
			DispatchStage(RHICmdList, ViewUniformBuffer, DispatchInstance.Tick, DispatchInstance.InstanceData, DispatchInstance.SimStageData);
		}
		RHICmdList.EndUAVOverlap(GPUInstanceCounterManager.GetInstanceCountBuffer().UAV);

		// Execute PostStage
		{
			TSet<FNiagaraDataInterfaceProxy*> ProxiesToFinalize;
			for (const FNiagaraGpuDispatchInstance& DispatchInstance : DispatchGroup.DispatchInstances)
			{
				PostStageInterface(RHICmdList, DispatchInstance.Tick, DispatchInstance.InstanceData, DispatchInstance.SimStageData, ProxiesToFinalize);
				if ( DispatchInstance.SimStageData.bLastStage )
				{
					PostSimulateInterface(RHICmdList, DispatchInstance.Tick, DispatchInstance.InstanceData);

					// Update CurrentData with the latest information as things like ParticleReads can use this data
					FNiagaraComputeExecutionContext* ComputeContext = DispatchInstance.InstanceData.Context;
					const FNiagaraSimStageData& FinalSimStageData = DispatchInstance.SimStageData;
					FNiagaraDataBuffer* FinalSimStageDataBuffer = FinalSimStageData.Destination != nullptr ? FinalSimStageData.Destination : FinalSimStageData.Source;
					check(FinalSimStageDataBuffer != nullptr);

					// If we are setting the data to render we need to ensure we switch back to the original CurrentData then swap the GPU buffers into it
					if (DispatchInstance.SimStageData.bSetDataToRender)
					{
						check(ComputeContext->DataSetOriginalBuffer_RT != nullptr);
						FNiagaraDataBuffer* CurrentData = ComputeContext->DataSetOriginalBuffer_RT;
						ComputeContext->DataSetOriginalBuffer_RT = nullptr;

						ComputeContext->MainDataSet->CurrentData = CurrentData;
						CurrentData->SwapGPU(FinalSimStageDataBuffer);

						// Mark data as ready for anyone who picks up the buffer on the next frame
						CurrentData->SetGPUDataReadyStage(ENiagaraGpuComputeTickStage::First);
                    
						ComputeContext->SetTranslucentDataToRender(nullptr);
						ComputeContext->SetDataToRender(CurrentData);

#if WITH_MGPU
						if (bAFREnabled)
						{
							AddAFRBuffer(CurrentData->GetGPUBufferFloat().Buffer);
							AddAFRBuffer(CurrentData->GetGPUBufferHalf().Buffer);
							AddAFRBuffer(CurrentData->GetGPUBufferInt().Buffer);
							if (ComputeContext->MainDataSet->RequiresPersistentIDs())
							{
								AddAFRBuffer(ComputeContext->MainDataSet->GetGPUFreeIDs().Buffer);
							}
						}
						if (bCrossGPUTransferEnabled)
						{
							AddCrossGPUTransfer(RHICmdList, CurrentData->GetGPUBufferFloat().Buffer);
							AddCrossGPUTransfer(RHICmdList, CurrentData->GetGPUBufferHalf().Buffer);
							AddCrossGPUTransfer(RHICmdList, CurrentData->GetGPUBufferInt().Buffer);
						}
#endif // WITH_MGPU
					}
					// If this is not the final tick of the final stage we need set our temporary buffer for data interfaces, etc, that may snoop from CurrentData
					else
					{
						ComputeContext->MainDataSet->CurrentData = FinalSimStageDataBuffer;
					}
				}
			}

			for (FNiagaraDataInterfaceProxy* ProxyToFinalize : ProxiesToFinalize)
			{
				ProxyToFinalize->FinalizePostStage(RHICmdList, this);
			}
		}

		// Execute After Transitions
		RHICmdList.Transition(TransitionsAfter);
		TransitionsAfter.Reset();

		// Update free IDs
		if (DispatchGroup.FreeIDUpdates.Num() > 0)
		{
			UpdateFreeIDsListSizesBuffer(RHICmdList, DispatchGroup.FreeIDUpdates.Num());
			UpdateFreeIDBuffers(RHICmdList, DispatchGroup.FreeIDUpdates);

			for (FNiagaraComputeExecutionContext* ComputeContext : DispatchGroup.FreeIDUpdates)
			{
				TransitionsAfter.Emplace(ComputeContext->MainDataSet->GetGPUFreeIDs().UAV, ERHIAccess::UAVCompute, ERHIAccess::SRVCompute);
			}
			RHICmdList.Transition(TransitionsAfter);
			TransitionsAfter.Reset();
		}
	}

#if WITH_NIAGARA_GPU_PROFILER
	const int32 StageTotalDispatches = TotalDispatchesThisFrame - StageStartTotalDispatches;
	GPUProfilerPtr->EndStage(RHICmdList, TickStage, StageTotalDispatches);
#endif

	// Clear dispatch groups
	// We do not release the counts as we won't do that until we finish the dispatches
	DispatchList.DispatchGroups.Empty();
}

void FNiagaraGpuComputeDispatch::DispatchStage(FRHICommandList& RHICmdList, FRHIUniformBuffer* ViewUniformBuffer, const FNiagaraGPUSystemTick& Tick, const FNiagaraComputeInstanceData& InstanceData, const FNiagaraSimStageData& SimStageData)
{
	// Setup source buffer
	if ( SimStageData.Source != nullptr )
	{
		SimStageData.Source->SetNumInstances(SimStageData.SourceNumInstances);
		SimStageData.Source->SetGPUInstanceCountBufferOffset(SimStageData.SourceCountOffset);
	}

	// Setup destination buffer
	int32 InstancesToSpawn = 0;
	if ( SimStageData.Destination != nullptr )
	{
		SimStageData.Destination->SetNumInstances(SimStageData.DestinationNumInstances);
		SimStageData.Destination->SetGPUInstanceCountBufferOffset(SimStageData.DestinationCountOffset);
		SimStageData.Destination->SetIDAcquireTag(FNiagaraComputeExecutionContext::TickCounter);

		if ( SimStageData.bFirstStage )
		{
			check(SimStageData.DestinationNumInstances >= SimStageData.SourceNumInstances);
			InstancesToSpawn = SimStageData.DestinationNumInstances - SimStageData.SourceNumInstances;
		}
		SimStageData.Destination->SetNumSpawnedInstances(InstancesToSpawn);
	}

	// Get dispatch count
	ENiagaraGpuDispatchType DispatchType = ENiagaraGpuDispatchType::OneD;
	FIntVector DispatchCount = FIntVector(0, 0, 0);
	FIntVector DispatchNumThreads = FIntVector(0, 0, 0);
	if (SimStageData.AlternateIterationSource)
	{
		DispatchType = SimStageData.StageMetaData->GpuDispatchType;
		DispatchCount = SimStageData.AlternateIterationSource->GetElementCount(Tick.SystemInstanceID);
		DispatchNumThreads = SimStageData.StageMetaData->GpuDispatchNumThreads;

		// Verify the number of elements isn't higher that what we can handle
		checkf(uint64(DispatchCount.X) * uint64(DispatchCount.Y) * uint64(DispatchCount.Z) < uint64(TNumericLimits<int32>::Max()), TEXT("DispatchCount(%d, %d, %d) for IterationInterface(%s) overflows an int32 this is not allowed"), DispatchCount.X, DispatchCount.Y, DispatchCount.Z, *SimStageData.AlternateIterationSource->SourceDIName.ToString());

		// Data interfaces such as grids / render targets can choose to dispatch in either the correct dimensionality for the target (i.e. RT2D would choose 2D)
		// or run in linear mode if performance is not beneficial due to increased waves.  It is also possible the we may choose to override on the simulation stage.
		// Therefore we need to special case OneD and convert our element count back to linear.
		if (DispatchType == ENiagaraGpuDispatchType::OneD)
		{
			DispatchCount.X = DispatchCount.X * DispatchCount.Y * DispatchCount.Z;
			DispatchCount.Y = 1;
			DispatchCount.Z = 1;
		}
	}
	else
	{
		DispatchType = ENiagaraGpuDispatchType::OneD;
		DispatchCount = FIntVector(SimStageData.DestinationNumInstances, 1, 1);
		DispatchNumThreads = FNiagaraShader::GetDefaultThreadGroupSize(ENiagaraGpuDispatchType::OneD);
	}

	const int32 TotalDispatchCount = DispatchCount.X * DispatchCount.Y * DispatchCount.Z;
	if (TotalDispatchCount == 0)
	{
		return;
	}

	checkf(DispatchNumThreads.X * DispatchNumThreads.Y * DispatchNumThreads.Z > 0, TEXT("DispatchNumThreads(%d, %d, %d) is invalid"), DispatchNumThreads.X, DispatchNumThreads.Y, DispatchNumThreads.Z);

	// Get Shader
	const FNiagaraShaderRef ComputeShader = InstanceData.Context->GPUScript_RT->GetShader(SimStageData.StageIndex);
	FRHIComputeShader* RHIComputeShader = ComputeShader.GetComputeShader();
	SetComputePipelineState(RHICmdList, RHIComputeShader);

	SCOPED_DRAW_EVENTF(RHICmdList, NiagaraGPUSimulationCS,
		TEXT("NiagaraGpuSim(%s) DispatchCount(%dx%dx%d) Stage(%s %u) Iteration(%u) NumThreads(%dx%dx%d)"),
		InstanceData.Context->GetDebugSimName(),
		DispatchCount.X, DispatchCount.Y, DispatchCount.Z,
		*SimStageData.StageMetaData->SimulationStageName.ToString(),
		SimStageData.StageIndex,
		SimStageData.IterationIndex,
		DispatchNumThreads.X, DispatchNumThreads.Y, DispatchNumThreads.Z
	);
	FNiagaraEmptyUAVPoolScopedAccess UAVPoolAccessScope(GetEmptyUAVPool());

	// Set Parameters
	const bool bRequiresPersistentIDs = InstanceData.Context->MainDataSet->RequiresPersistentIDs();

	SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->SimStartParam, InstanceData.bResetData ? 1U : 0U);
	SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->EmitterTickCounterParam, FNiagaraComputeExecutionContext::TickCounter);
	SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->NumSpawnedInstancesParam, InstancesToSpawn);
	SetSRVParameter(RHICmdList, RHIComputeShader, ComputeShader->FreeIDBufferParam, bRequiresPersistentIDs ? InstanceData.Context->MainDataSet->GetGPUFreeIDs().SRV.GetReference() : FNiagaraRenderer::GetDummyIntBuffer());

	// Set spawn Information
	// This parameter is an array of structs with 2 floats and 2 ints on CPU, but a float4 array on GPU. The shader uses asint() to cast the integer values. To set the parameter,
	// we pass the structure array as a float* to SetShaderValueArray() and specify the number of floats (not float vectors).
	static_assert((sizeof(InstanceData.SpawnInfo.SpawnInfoStartOffsets) % SHADER_PARAMETER_ARRAY_ELEMENT_ALIGNMENT) == 0, "sizeof SpawnInfoStartOffsets should be a multiple of SHADER_PARAMETER_ARRAY_ELEMENT_ALIGNMENT");
	static_assert((sizeof(InstanceData.SpawnInfo.SpawnInfoParams) % SHADER_PARAMETER_ARRAY_ELEMENT_ALIGNMENT) == 0, "sizeof SpawnInfoParams should be a multiple of SHADER_PARAMETER_ARRAY_ELEMENT_ALIGNMENT");
	SetShaderValueArray(RHICmdList, RHIComputeShader, ComputeShader->EmitterSpawnInfoOffsetsParam, InstanceData.SpawnInfo.SpawnInfoStartOffsets, NIAGARA_MAX_GPU_SPAWN_INFOS);
	SetShaderValueArray(RHICmdList, RHIComputeShader, ComputeShader->EmitterSpawnInfoParamsParam, &InstanceData.SpawnInfo.SpawnInfoParams[0].IntervalDt, 4 * NIAGARA_MAX_GPU_SPAWN_INFOS);

	if (ComputeShader->ViewUniformBufferParam.IsBound())
	{
		if (ensureMsgf(ViewUniformBuffer != nullptr, TEXT("ViewUniformBuffer is required for '%s' but we do not have one to bind"), InstanceData.Context->GetDebugSimName()))
		{
			RHICmdList.SetShaderUniformBuffer(RHIComputeShader, ComputeShader->ViewUniformBufferParam.GetBaseIndex(), ViewUniformBuffer);
		}
	}

	// Setup instance counts
	if (ComputeShader->InstanceCountsParam.IsBound())
	{
		ComputeShader->InstanceCountsParam.SetBuffer(RHICmdList, RHIComputeShader, GPUInstanceCounterManager.GetInstanceCountBuffer());

		if (SimStageData.AlternateIterationSource)
		{
			SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->ReadInstanceCountOffsetParam, -1);
			SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->WriteInstanceCountOffsetParam, -1);
		}
		else
		{
			SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->ReadInstanceCountOffsetParam, SimStageData.SourceCountOffset);
			SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->WriteInstanceCountOffsetParam, SimStageData.DestinationCountOffset);
		}
	}

	// Simulation Stage Information
	// X = Count Buffer Instance Count Offset (INDEX_NONE == Use Instance Count)
	// Y = Instance Count
	// Z = Iteration Index
	// W = Num Iterations
	{
		FIntVector4 SimulationStageIterationInfo = { INDEX_NONE, -1, 0, 0 };
		float SimulationStageNormalizedIterationIndex = 0.0f;

		if (SimStageData.AlternateIterationSource != nullptr)
		{
			const uint32 IterationInstanceCountOffset = SimStageData.AlternateIterationSource->GetGPUInstanceCountOffset(Tick.SystemInstanceID);
			SimulationStageIterationInfo.X = IterationInstanceCountOffset;
			SimulationStageIterationInfo.Y = IterationInstanceCountOffset == INDEX_NONE ? TotalDispatchCount : 0;
		}

		const int32 NumIterations = InstanceData.NumIterationsPerStage[SimStageData.StageIndex];
		const int32 IterationIndex = SimStageData.IterationIndex;
		SimulationStageIterationInfo.Z = IterationIndex;
		SimulationStageIterationInfo.W = NumIterations;
		SimulationStageNormalizedIterationIndex = NumIterations > 1 ? float(IterationIndex) / float(NumIterations - 1) : 1.0f;

		SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->SimulationStageIterationInfoParam, SimulationStageIterationInfo);
		SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->SimulationStageNormalizedIterationIndexParam, SimulationStageNormalizedIterationIndex);
	}

	// Set particle iteration state info
	// Where X = Parameter Binding, YZ = Inclusive Range
	{
		int ParticleIterationStateInfo[3] = { SimStageData.StageMetaData->ParticleIterationStateComponentIndex, SimStageData.StageMetaData->ParticleIterationStateRange.X, SimStageData.StageMetaData->ParticleIterationStateRange.Y };
		SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->ParticleIterationStateInfoParam, ParticleIterationStateInfo);
	}

	// Set static input buffers
	SetSRVParameter(RHICmdList, RHIComputeShader, ComputeShader->StaticInputFloatParam, Tick.SystemGpuComputeProxy->StaticFloatBuffer);

	// Set Input & Output buffers
	FNiagaraDataBuffer::SetInputShaderParams(RHICmdList, ComputeShader.GetShader(), SimStageData.Source);
	FNiagaraDataBuffer::SetOutputShaderParams(RHICmdList, ComputeShader.GetShader(), SimStageData.Destination);

	// Set data interface parameters
	SetDataInterfaceParameters(RHICmdList, Tick, InstanceData, ComputeShader, SimStageData);

	// Execute the dispatch
	{
		// In the OneD case we can use the Y dimension to get higher particle counts
		if (DispatchType == ENiagaraGpuDispatchType::OneD)
		{
			const int32 TotalThreadGroups = FMath::DivideAndRoundUp(DispatchCount.X, DispatchNumThreads.X);
			DispatchCount.Y = FMath::DivideAndRoundUp<int32>(TotalThreadGroups, NiagaraMaxThreadGroupCountPerDimension);
			DispatchCount.X = FMath::DivideAndRoundUp(DispatchCount.X, DispatchNumThreads.Y);
		}

		FIntVector ThreadGroupCount;
		ThreadGroupCount.X = FMath::DivideAndRoundUp(DispatchCount.X, DispatchNumThreads.X);
		ThreadGroupCount.Y = FMath::DivideAndRoundUp(DispatchCount.Y, DispatchNumThreads.Y);
		ThreadGroupCount.Z = FMath::DivideAndRoundUp(DispatchCount.Z, DispatchNumThreads.Z);

		SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->DispatchThreadIdToLinearParam, FIntVector(1, DispatchCount.X, DispatchCount.X * DispatchCount.Y));
		SetShaderValue(RHICmdList, RHIComputeShader, ComputeShader->DispatchThreadIdBoundsParam, DispatchCount);

		SetConstantBuffers(RHICmdList, ComputeShader, Tick, &InstanceData);

		DispatchComputeShader(RHICmdList, ComputeShader, ThreadGroupCount.X, ThreadGroupCount.Y, ThreadGroupCount.Z);

		INC_DWORD_STAT(STAT_NiagaraGPUDispatches);
	}

	// Unset UAV parameters
	UnsetDataInterfaceParameters(RHICmdList, Tick, InstanceData, ComputeShader, SimStageData);
	FNiagaraDataBuffer::UnsetShaderParams(RHICmdList, ComputeShader.GetShader());
	ComputeShader->InstanceCountsParam.UnsetUAV(RHICmdList, RHIComputeShader);

	// Optionally submit commands to the GPU
	// This can be used to avoid accidental TDR detection in the editor especially when issuing multiple ticks in the same frame
	++TotalDispatchesThisFrame;
	if (GNiagaraGpuSubmitCommandHint > 0)
	{
		if ((TotalDispatchesThisFrame % GNiagaraGpuSubmitCommandHint) == 0)
		{
			RHICmdList.SubmitCommandsHint();
		}
	}
}

void FNiagaraGpuComputeDispatch::PreInitViews(FRDGBuilder& GraphBuilder, bool bAllowGPUParticleUpdate)
{
	bRequiresReadback = false;
	GNiagaraViewDataManager.ClearSceneTextureParameters();
#if WITH_EDITOR
	bRaisedWarningThisFrame = false;
#endif
#if WITH_MGPU
	bAFREnabled = GNumAlternateFrameRenderingGroups > 1;
	bCrossGPUTransferEnabled = !bAFREnabled && (GNumExplicitGPUsForRendering > 1);
	StageToTransferGPUBuffers = ENiagaraGpuComputeTickStage::Last;
	StageToWaitForGPUTransfers = ENiagaraGpuComputeTickStage::First;
#endif

	GpuReadbackManagerPtr->Tick();
#if NIAGARA_COMPUTEDEBUG_ENABLED
	if ( FNiagaraGpuComputeDebug* GpuComputeDebug = GetGpuComputeDebug() )
	{
		GpuComputeDebug->Tick(GraphBuilder.RHICmdList);
	}
#endif

	LLM_SCOPE(ELLMTag::Niagara);
	TotalDispatchesThisFrame = 0;

	// Add pass to begin the gpu profiler frame
#if WITH_NIAGARA_GPU_PROFILER
	AddPass(GraphBuilder, RDG_EVENT_NAME("Niagara::GPUProfiler_BeginFrame"), [this](FRHICommandListImmediate& RHICmdList)
	{
		GPUProfilerPtr->BeginFrame(RHICmdList);
	});
#endif

	// Reset the list of GPUSort tasks and release any resources they hold on to.
	// It might be worth considering doing so at the end of the render to free the resources immediately.
	// (note that currently there are no callback appropriate to do it)
	SimulationsToSort.Reset();

	if (FNiagaraUtilities::AllowGPUParticles(GetShaderPlatform()))
	{
		if ( bAllowGPUParticleUpdate )
		{
			FramesBeforeTickFlush = 0;

			UpdateInstanceCountManager(GraphBuilder.RHICmdList);
			PrepareAllTicks(GraphBuilder.RHICmdList);
		
		#if WITH_MGPU
			CalculateCrossGPUTransferLocation();
		#endif

			AsyncGpuTraceHelper->BeginFrame(GraphBuilder.RHICmdList, this);

			if ( FNiagaraGpuComputeDispatchLocal::GDebugLogging)
			{
				DumpDebugFrame();
			}

			FNiagaraSceneTextureParameters* PassParameters = GraphBuilder.AllocParameters<FNiagaraSceneTextureParameters>();
			GNiagaraViewDataManager.GetSceneTextureParameters(GraphBuilder, *PassParameters);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("Niagara::PreInitViews"),
				PassParameters,
				ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
				[this, PassParameters](FRHICommandListImmediate& RHICmdList)
			{
				NiagaraSceneTextures = PassParameters;
				ON_SCOPE_EXIT { NiagaraSceneTextures = nullptr; };

				ExecuteTicks(RHICmdList, nullptr, ENiagaraGpuComputeTickStage::PreInitViews);
			});
		}
	}
	else
	{
		GPUInstanceCounterManager.ResizeBuffers(GraphBuilder.RHICmdList, 0);
		FinishDispatches();
	}
}

void FNiagaraGpuComputeDispatch::PostInitViews(FRDGBuilder& GraphBuilder, TArrayView<const class FViewInfo> Views, bool bAllowGPUParticleUpdate)
{
	LLM_SCOPE(ELLMTag::Niagara);

	bAllowGPUParticleUpdate = bAllowGPUParticleUpdate && GetReferenceAllowGPUUpdate(Views);

	if (bAllowGPUParticleUpdate && FNiagaraUtilities::AllowGPUParticles(GetShaderPlatform()))
	{
		FNiagaraSceneTextureParameters* PassParameters = GraphBuilder.AllocParameters<FNiagaraSceneTextureParameters>();
		GNiagaraViewDataManager.GetSceneTextureParameters(GraphBuilder, *PassParameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("Niagara::PostInitViews"),
			PassParameters,
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			[this, PassParameters, Views](FRHICommandListImmediate& RHICmdList)
			{
				NiagaraSceneTextures = PassParameters;
				ON_SCOPE_EXIT{ NiagaraSceneTextures = nullptr; };

				FRHIUniformBuffer* ViewUniformBuffer = GetReferenceViewUniformBuffer(Views);
				ExecuteTicks(RHICmdList, ViewUniformBuffer, ENiagaraGpuComputeTickStage::PostInitViews);
			#if WITH_MGPU
				TransferMultiGPUBufers(RHICmdList, ENiagaraGpuComputeTickStage::PostInitViews);
			#endif // WITH_MGPU
			}
		);
	}
}

void FNiagaraGpuComputeDispatch::PostRenderOpaque(FRDGBuilder& GraphBuilder, TConstArrayView<FViewInfo> Views, bool bAllowGPUParticleUpdate)
{
	LLM_SCOPE(ELLMTag::Niagara);

	bAllowGPUParticleUpdate = bAllowGPUParticleUpdate && GetReferenceAllowGPUUpdate(Views);

	if ( bAllowGPUParticleUpdate && Views.IsValidIndex(0) )
	{
		CachedViewRect = Views[0].ViewRect;
	}

	if (bAllowGPUParticleUpdate && FNiagaraUtilities::AllowGPUParticles(GetShaderPlatform()))
	{
		FNiagaraSceneTextureParameters* PassParameters = GraphBuilder.AllocParameters<FNiagaraSceneTextureParameters>();
		// TODO: This will cause a fragment->compute barrier on a scene textures which could be costly especially on mobile GPUs
		// Will be nice to avoid executing this if we know that there are no simulations that require access to a scene textures
		GNiagaraViewDataManager.GetSceneTextureParameters(GraphBuilder, *PassParameters);

 		GraphBuilder.AddPass(
			RDG_EVENT_NAME("Niagara::PostRenderOpaque"),
			PassParameters,
			ERDGPassFlags::Compute | ERDGPassFlags::NeverCull,
			[this, PassParameters, Views](FRHICommandListImmediate& RHICmdList)
		{
			NiagaraSceneTextures = PassParameters;
			ON_SCOPE_EXIT{ NiagaraSceneTextures = nullptr; };

			AsyncGpuTraceHelper->PostRenderOpaque(RHICmdList, this, Views);

			CurrentPassViews = Views;

			FRHIUniformBuffer* ViewUniformBuffer = GetReferenceViewUniformBuffer(Views);

			// Setup new readback since if there is no pending request, there is no risk of having invalid data read (offset being allocated after the readback was sent).
			ExecuteTicks(RHICmdList, ViewUniformBuffer, ENiagaraGpuComputeTickStage::PostOpaqueRender);

			FinishDispatches();

			AsyncGpuTraceHelper->EndFrame(RHICmdList, this);

			// Clear CurrentPassViews
			CurrentPassViews = TConstArrayView<FViewInfo>();

			ProcessDebugReadbacks(RHICmdList, false);
		});
	}

	if (bRequiresReadback)
	{
		AddPass(GraphBuilder, RDG_EVENT_NAME("Niagara::GPUReadback"), [this](FRHICommandListImmediate& RHICmdList)
		{
			check(!GPUInstanceCounterManager.HasPendingGPUReadback());
			GPUInstanceCounterManager.EnqueueGPUReadback(RHICmdList);
		});
		bRequiresReadback = false;
	}

#if WITH_NIAGARA_GPU_PROFILER
	AddPass(GraphBuilder, RDG_EVENT_NAME("Niagara::GPUProfiler_EndFrame"), [this](FRHICommandListImmediate& RHICmdList)
	{
		GPUProfilerPtr->EndFrame(RHICmdList);
	});
#endif

	GNiagaraViewDataManager.ClearSceneTextureParameters();
}

void FNiagaraGpuComputeDispatch::ProcessDebugReadbacks(FRHICommandListImmediate& RHICmdList, bool bWaitCompletion)
{
#if !UE_BUILD_SHIPPING
	// Execute any pending readbacks as the ticks have now all been processed
	for (const FDebugReadbackInfo& DebugReadback : GpuDebugReadbackInfos)
	{
		FNiagaraDataBuffer* CurrentDataBuffer = DebugReadback.Context->MainDataSet->GetCurrentData();
		if ( CurrentDataBuffer == nullptr )
		{
			// Data is invalid
			DebugReadback.DebugInfo->Frame.CopyFromGPUReadback(nullptr, nullptr, nullptr, 0, 0, 0, 0, 0);
			DebugReadback.DebugInfo->bWritten = true;
			continue;
		}

		const uint32 CountOffset = CurrentDataBuffer->GetGPUInstanceCountBufferOffset();
		if (CountOffset == INDEX_NONE)
		{
			// Data is invalid
			DebugReadback.DebugInfo->Frame.CopyFromGPUReadback(nullptr, nullptr, nullptr, 0, 0, 0, 0, 0);
			DebugReadback.DebugInfo->bWritten = true;
			continue;
		}

		// Execute readback
		constexpr int32 MaxReadbackBuffers = 4;
		TArray<FRHIBuffer*, TInlineAllocator<MaxReadbackBuffers>> ReadbackBuffers;

		const int32 CountBufferIndex = ReadbackBuffers.Add(GPUInstanceCounterManager.GetInstanceCountBuffer().Buffer);
		const int32 FloatBufferIndex = (CurrentDataBuffer->GetGPUBufferFloat().NumBytes == 0) ? INDEX_NONE : ReadbackBuffers.Add(CurrentDataBuffer->GetGPUBufferFloat().Buffer);
		const int32 HalfBufferIndex = (CurrentDataBuffer->GetGPUBufferHalf().NumBytes == 0) ? INDEX_NONE : ReadbackBuffers.Add(CurrentDataBuffer->GetGPUBufferHalf().Buffer);
		const int32 IntBufferIndex = (CurrentDataBuffer->GetGPUBufferInt().NumBytes == 0) ? INDEX_NONE : ReadbackBuffers.Add(CurrentDataBuffer->GetGPUBufferInt().Buffer);

		const int32 FloatBufferStride = CurrentDataBuffer->GetFloatStride();
		const int32 HalfBufferStride = CurrentDataBuffer->GetHalfStride();
		const int32 IntBufferStride = CurrentDataBuffer->GetInt32Stride();

		GpuReadbackManagerPtr->EnqueueReadbacks(
			RHICmdList,
			MakeArrayView(ReadbackBuffers),
			[=, DebugInfo=DebugReadback.DebugInfo](TConstArrayView<TPair<void*, uint32>> BufferData)
			{
				checkf(4 + (CountOffset * 4) <= BufferData[CountBufferIndex].Value, TEXT("CountOffset %d is out of bounds"), CountOffset, BufferData[CountBufferIndex].Value);
				const int32 InstanceCount = reinterpret_cast<int32*>(BufferData[CountBufferIndex].Key)[CountOffset];
				const float* FloatDataBuffer = FloatBufferIndex == INDEX_NONE ? nullptr : reinterpret_cast<float*>(BufferData[FloatBufferIndex].Key);
				const FFloat16* HalfDataBuffer = HalfBufferIndex == INDEX_NONE ? nullptr : reinterpret_cast<FFloat16*>(BufferData[HalfBufferIndex].Key);
				const int32* IntDataBuffer = IntBufferIndex == INDEX_NONE ? nullptr : reinterpret_cast<int32*>(BufferData[IntBufferIndex].Key);

				DebugInfo->Frame.CopyFromGPUReadback(FloatDataBuffer, IntDataBuffer, HalfDataBuffer, 0, InstanceCount, FloatBufferStride, IntBufferStride, HalfBufferStride);
				DebugInfo->bWritten = true;
			}
		);
	}
	GpuDebugReadbackInfos.Empty();

	if (bWaitCompletion)
	{
		GpuReadbackManagerPtr->WaitCompletion(RHICmdList);
	}
#endif
}

bool FNiagaraGpuComputeDispatch::UsesGlobalDistanceField() const
{
	return NumProxiesThatRequireDistanceFieldData > 0;
}

bool FNiagaraGpuComputeDispatch::UsesDepthBuffer() const
{
	return NumProxiesThatRequireDepthBuffer > 0;
}

bool FNiagaraGpuComputeDispatch::RequiresEarlyViewUniformBuffer() const
{
	return NumProxiesThatRequireEarlyViewData > 0;
}

bool FNiagaraGpuComputeDispatch::RequiresRayTracingScene() const
{
	return NumProxiesThatRequireRayTracingScene > 0;
}

void FNiagaraGpuComputeDispatch::PreRender(FRDGBuilder& GraphBuilder, TConstArrayView<FViewInfo> Views, bool bAllowGPUParticleUpdate)
{
	if (!FNiagaraUtilities::AllowGPUParticles(GetShaderPlatform()))
	{
		return;
	}

	LLM_SCOPE(ELLMTag::Niagara);
}

void FNiagaraGpuComputeDispatch::OnDestroy()
{
	FNiagaraWorldManager::OnComputeDispatchInterfaceDestroyed(this);
	FFXSystemInterface::OnDestroy();
}

bool FNiagaraGpuComputeDispatch::AddSortedGPUSimulation(FNiagaraGPUSortInfo& SortInfo)
{
	if (GPUSortManager && GPUSortManager->AddTask(SortInfo.AllocationInfo, SortInfo.ParticleCount, SortInfo.SortFlags))
	{
		// It's not worth currently to have a map between SortInfo.AllocationInfo.SortBatchId and the relevant indices in SimulationsToSort
		// because the number of batches is expect to be very small (1 or 2). If this change, it might be worth reconsidering.
		SimulationsToSort.Add(SortInfo);
		return true;
	}
	else
	{
		return false;
	}
}

const FGlobalDistanceFieldParameterData* FNiagaraGpuComputeDispatch::GetGlobalDistanceFieldParameters() const
{ 
	check(CurrentPassViews.Num() > 0); 
	return &CurrentPassViews[0].GlobalDistanceFieldInfo.ParameterData; 
}

const FDistanceFieldSceneData* FNiagaraGpuComputeDispatch::GetMeshDistanceFieldParameters() const
{
	if (CurrentPassViews.Num() == 0 || CurrentPassViews[0].Family == nullptr || CurrentPassViews[0].Family->Scene == nullptr || CurrentPassViews[0].Family->Scene->GetRenderScene() == nullptr)
	{
		return nullptr;
	}

	return &CurrentPassViews[0].Family->Scene->GetRenderScene()->DistanceFieldSceneData;
}

void FNiagaraGpuComputeDispatch::GenerateSortKeys(FRHICommandListImmediate& RHICmdList, int32 BatchId, int32 NumElementsInBatch, EGPUSortFlags Flags, FRHIUnorderedAccessView* KeysUAV, FRHIUnorderedAccessView* ValuesUAV)
{
	const FGPUSortManager::FKeyGenInfo KeyGenInfo((uint32)NumElementsInBatch, EnumHasAnyFlags(Flags, EGPUSortFlags::HighPrecisionKeys));

	FNiagaraSortKeyGenCS::FPermutationDomain SortPermutationVector;
	SortPermutationVector.Set<FNiagaraSortKeyGenCS::FSortUsingMaxPrecision>(GNiagaraGPUSortingUseMaxPrecision != 0);
	SortPermutationVector.Set<FNiagaraSortKeyGenCS::FEnableCulling>(false);

	FNiagaraSortKeyGenCS::FPermutationDomain SortAndCullPermutationVector;
	SortAndCullPermutationVector.Set<FNiagaraSortKeyGenCS::FSortUsingMaxPrecision>(GNiagaraGPUSortingUseMaxPrecision != 0);
	SortAndCullPermutationVector.Set<FNiagaraSortKeyGenCS::FEnableCulling>(true);

	TShaderMapRef<FNiagaraSortKeyGenCS> SortKeyGenCS(GetGlobalShaderMap(FeatureLevel), SortPermutationVector);
	TShaderMapRef<FNiagaraSortKeyGenCS> SortAndCullKeyGenCS(GetGlobalShaderMap(FeatureLevel), SortAndCullPermutationVector);

	FRWBuffer* CulledCountsBuffer = GPUInstanceCounterManager.AcquireCulledCountsBuffer(RHICmdList);

	FNiagaraSortKeyGenCS::FParameters Params;
	Params.SortKeyMask = KeyGenInfo.SortKeyParams.X;
	Params.SortKeyShift = KeyGenInfo.SortKeyParams.Y;
	Params.SortKeySignBit = KeyGenInfo.SortKeyParams.Z;
	Params.OutKeys = KeysUAV;
	Params.OutParticleIndices = ValuesUAV;

	FRHIUnorderedAccessView* OverlapUAVs[3];
	uint32 NumOverlapUAVs = 0;

	OverlapUAVs[NumOverlapUAVs] = KeysUAV;
	++NumOverlapUAVs;
	OverlapUAVs[NumOverlapUAVs] = ValuesUAV;
	++NumOverlapUAVs;

	if (CulledCountsBuffer)
	{
		Params.OutCulledParticleCounts = CulledCountsBuffer->UAV;
		OverlapUAVs[NumOverlapUAVs] = CulledCountsBuffer->UAV;
		++NumOverlapUAVs;
	}
	else
	{
		// Note: We don't care that the buffer will be allowed to be reused
		FNiagaraEmptyUAVPoolScopedAccess UAVPoolAccessScope(GetEmptyUAVPool());
		Params.OutCulledParticleCounts = GetEmptyUAVFromPool(RHICmdList, PF_R32_UINT, ENiagaraEmptyUAVType::Buffer);
	}

	RHICmdList.BeginUAVOverlap(MakeArrayView(OverlapUAVs, NumOverlapUAVs));

	for (const FNiagaraGPUSortInfo& SortInfo : SimulationsToSort)
	{
		if (SortInfo.AllocationInfo.SortBatchId == BatchId)
		{
			Params.NiagaraParticleDataFloat = SortInfo.ParticleDataFloatSRV;
			Params.NiagaraParticleDataHalf = SortInfo.ParticleDataHalfSRV;
			Params.NiagaraParticleDataInt = SortInfo.ParticleDataIntSRV;
			Params.GPUParticleCountBuffer = SortInfo.GPUParticleCountSRV;
			Params.FloatDataStride = SortInfo.FloatDataStride;
			Params.HalfDataStride = SortInfo.HalfDataStride;
			Params.IntDataStride = SortInfo.IntDataStride;
			Params.ParticleCount = SortInfo.ParticleCount;
			Params.GPUParticleCountOffset = SortInfo.GPUParticleCountOffset;
			Params.CulledGPUParticleCountOffset = SortInfo.CulledGPUParticleCountOffset;
			Params.EmitterKey = (uint32)SortInfo.AllocationInfo.ElementIndex << KeyGenInfo.ElementKeyShift;
			Params.OutputOffset = SortInfo.AllocationInfo.BufferOffset;
			Params.CameraPosition = (FVector3f)SortInfo.ViewOrigin;
			Params.CameraDirection = (FVector3f)SortInfo.ViewDirection;
			Params.SortMode = (uint32)SortInfo.SortMode;
			Params.SortAttributeOffset = SortInfo.SortAttributeOffset;
			Params.CullPositionAttributeOffset = SortInfo.CullPositionAttributeOffset;
			Params.CullOrientationAttributeOffset = SortInfo.CullOrientationAttributeOffset;
			Params.CullScaleAttributeOffset = SortInfo.CullScaleAttributeOffset;
			Params.RendererVisibility = SortInfo.RendererVisibility;
			Params.RendererVisTagAttributeOffset = SortInfo.RendererVisTagAttributeOffset;
			Params.MeshIndex = SortInfo.MeshIndex;
			Params.MeshIndexAttributeOffset = SortInfo.MeshIndexAttributeOffset;
			Params.CullDistanceRangeSquared = SortInfo.DistanceCullRange * SortInfo.DistanceCullRange;
			Params.LocalBoundingSphere = FVector4f((FVector3f)SortInfo.LocalBSphere.Center, SortInfo.LocalBSphere.W);
			Params.CullingWorldSpaceOffset = (FVector3f)SortInfo.CullingWorldSpaceOffset;
			Params.SystemLWCTile = SortInfo.SystemLWCTile;

			Params.NumCullPlanes = 0;
			for (const FPlane& Plane : SortInfo.CullPlanes)
			{
				Params.CullPlanes[Params.NumCullPlanes++] = FVector4f(Plane.X, Plane.Y, Plane.Z, Plane.W);
			}

			// Choose the shader to bind
			TShaderMapRef<FNiagaraSortKeyGenCS> KeyGenCS = SortInfo.bEnableCulling ? SortAndCullKeyGenCS : SortKeyGenCS;
			SetComputePipelineState(RHICmdList, KeyGenCS.GetComputeShader());

			SetShaderParameters(RHICmdList, KeyGenCS, KeyGenCS.GetComputeShader(), Params);
			DispatchComputeShader(RHICmdList, KeyGenCS, FMath::DivideAndRoundUp(SortInfo.ParticleCount, NIAGARA_KEY_GEN_THREAD_COUNT), 1, 1);
			UnsetShaderUAVs(RHICmdList, KeyGenCS, KeyGenCS.GetComputeShader());
		}
	}

	RHICmdList.EndUAVOverlap(MakeArrayView(OverlapUAVs, NumOverlapUAVs));
}

FNiagaraAsyncGpuTraceHelper& FNiagaraGpuComputeDispatch::GetAsyncGpuTraceHelper() const
{
	check(AsyncGpuTraceHelper.IsValid());
	return *AsyncGpuTraceHelper.Get();
}

/* Set shader parameters for data interfaces
 */
void FNiagaraGpuComputeDispatch::SetDataInterfaceParameters(FRHICommandList& RHICmdList, const FNiagaraGPUSystemTick& Tick, const FNiagaraComputeInstanceData& InstanceData, const FNiagaraShaderRef& ComputeShader, const FNiagaraSimStageData& SimStageData) const
{
	// @todo-threadsafety This is a bit gross. Need to rethink this api.
	const FNiagaraShaderMapPointerTable& PointerTable = ComputeShader.GetPointerTable();

	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : InstanceData.DataInterfaceProxies)
	{
		const TMemoryImageArray<FNiagaraDataInterfaceParamRef>& DIParameters = ComputeShader->GetDIParameters();
		const FNiagaraDataInterfaceParamRef& DIParam = DIParameters[InterfaceIndex];
		if (DIParam.Parameters.IsValid())
		{
			FNiagaraDataInterfaceSetArgs Context(Interface, Tick.SystemInstanceID, Tick.SystemGpuComputeProxy->GetSystemLWCTile(), this, ComputeShader, &InstanceData, &SimStageData, InstanceData.IsOutputStage(Interface, SimStageData.StageIndex), InstanceData.IsIterationStage(Interface, SimStageData.StageIndex));
			DIParam.DIType.Get(PointerTable.DITypes)->SetParameters(DIParam.Parameters.Get(), RHICmdList, Context);
		}

		InterfaceIndex++;
	}
}

void FNiagaraGpuComputeDispatch::UnsetDataInterfaceParameters(FRHICommandList& RHICmdList, const FNiagaraGPUSystemTick& Tick, const FNiagaraComputeInstanceData& InstanceData, const FNiagaraShaderRef& ComputeShader, const FNiagaraSimStageData& SimStageData) const
{
	// @todo-threadsafety This is a bit gross. Need to rethink this api.
	const FNiagaraShaderMapPointerTable& PointerTable = ComputeShader.GetPointerTable();

	uint32 InterfaceIndex = 0;
	for (FNiagaraDataInterfaceProxy* Interface : InstanceData.DataInterfaceProxies)
	{
		const TMemoryImageArray<FNiagaraDataInterfaceParamRef>& DIParameters = ComputeShader->GetDIParameters();
		const FNiagaraDataInterfaceParamRef& DIParam = DIParameters[InterfaceIndex];
		if (DIParam.Parameters.IsValid())
		{
			FNiagaraDataInterfaceSetArgs Context(Interface, Tick.SystemInstanceID, Tick.SystemGpuComputeProxy->GetSystemLWCTile(), this, ComputeShader, &InstanceData, &SimStageData, InstanceData.IsOutputStage(Interface, SimStageData.StageIndex), InstanceData.IsIterationStage(Interface, SimStageData.StageIndex));
			DIParam.DIType.Get(PointerTable.DITypes)->UnsetParameters(DIParam.Parameters.Get(), RHICmdList, Context);
		}

		InterfaceIndex++;
	}
}

static void SetConstantBuffer(FRHICommandList &RHICmdList, FRHIComputeShader* ComputeShader, const FShaderUniformBufferParameter& BufferParam, const FUniformBufferRHIRef& UniformBuffer)
{
	if (BufferParam.IsBound() && UniformBuffer.IsValid())
	{
		RHICmdList.SetShaderUniformBuffer(ComputeShader, BufferParam.GetBaseIndex(), UniformBuffer);
	}
}

void FNiagaraGpuComputeDispatch::SetConstantBuffers(FRHICommandList &RHICmdList, const FNiagaraShaderRef& Shader, const FNiagaraGPUSystemTick& Tick, const FNiagaraComputeInstanceData* Instance)
{
	FNiagaraComputeExecutionContext* Context = Instance->Context;
	FRHIComputeShader* ComputeShader = Shader.GetComputeShader();

	SetConstantBuffer(RHICmdList, ComputeShader, Shader->GlobalConstantBufferParam[0], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_Global, nullptr, true));
	SetConstantBuffer(RHICmdList, ComputeShader, Shader->SystemConstantBufferParam[0], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_System, nullptr, true));
	SetConstantBuffer(RHICmdList, ComputeShader, Shader->OwnerConstantBufferParam[0], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_Owner, nullptr, true));
	SetConstantBuffer(RHICmdList, ComputeShader, Shader->EmitterConstantBufferParam[0], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_Emitter, Instance, true));
	SetConstantBuffer(RHICmdList, ComputeShader, Shader->ExternalConstantBufferParam[0], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_External, Instance, true));

	if (Context->HasInterpolationParameters)
	{
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->GlobalConstantBufferParam[1], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_Global, nullptr, false));
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->SystemConstantBufferParam[1], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_System, nullptr, false));
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->OwnerConstantBufferParam[1], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_Owner, nullptr, false));
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->EmitterConstantBufferParam[1], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_Emitter, Instance, false));
		SetConstantBuffer(RHICmdList, ComputeShader, Shader->ExternalConstantBufferParam[1], Tick.GetUniformBuffer(FNiagaraGPUSystemTick::UBT_External, Instance, false));
	}
}

FGPUSortManager* FNiagaraGpuComputeDispatch::GetGPUSortManager() const
{
	return GPUSortManager;
}

void FNiagaraGpuComputeDispatch::AddDebugReadback(FNiagaraSystemInstanceID InstanceID, TSharedPtr<struct FNiagaraScriptDebuggerInfo, ESPMode::ThreadSafe> DebugInfo, FNiagaraComputeExecutionContext* Context)
{
	FDebugReadbackInfo& ReadbackInfo = GpuDebugReadbackInfos.AddDefaulted_GetRef();
	ReadbackInfo.InstanceID = InstanceID;
	ReadbackInfo.DebugInfo = DebugInfo;
	ReadbackInfo.Context = Context;
}

bool FNiagaraGpuComputeDispatch::ShouldDebugDraw_RenderThread() const
{
#if NIAGARA_COMPUTEDEBUG_ENABLED
	if (FNiagaraGpuComputeDebug* GpuComputeDebug = GpuComputeDebugPtr.Get())
	{
		return GpuComputeDebug->ShouldDrawDebug();
	}
#endif
	return false;
}

void FNiagaraGpuComputeDispatch::DrawDebug_RenderThread(class FRDGBuilder& GraphBuilder, const class FViewInfo& View, const struct FScreenPassRenderTarget& Output)
{
#if NIAGARA_COMPUTEDEBUG_ENABLED
	if (FNiagaraGpuComputeDebug* GpuComputeDebug = GpuComputeDebugPtr.Get())
	{
		GpuComputeDebug->DrawDebug(GraphBuilder, View, Output);
	}
#endif
}

void FNiagaraGpuComputeDispatch::DrawSceneDebug_RenderThread(class FRDGBuilder& GraphBuilder, const class FViewInfo& View, FRDGTextureRef SceneColor, FRDGTextureRef SceneDepth)
{
#if NIAGARA_COMPUTEDEBUG_ENABLED
	if (FNiagaraGpuComputeDebug* GpuComputeDebug = GpuComputeDebugPtr.Get())
	{
		GpuComputeDebug->DrawSceneDebug(GraphBuilder, View, SceneColor, SceneDepth);
	}
#endif
}

#if WITH_MGPU
void FNiagaraGpuComputeDispatch::MultiGPUResourceModified(FRHICommandList& RHICmdList, FRHIBuffer* Buffer, bool bRequiredForSimulation, bool bRequiredForRendering) const
{
	if (bAFREnabled && bRequiredForSimulation)
	{
		const_cast<FNiagaraGpuComputeDispatch*>(this)->AddAFRBuffer(Buffer);
	}
	if (bCrossGPUTransferEnabled && bRequiredForRendering)
	{
		const_cast<FNiagaraGpuComputeDispatch*>(this)->AddCrossGPUTransfer(RHICmdList, Buffer);
	}
}

void FNiagaraGpuComputeDispatch::MultiGPUResourceModified(FRHICommandList& RHICmdList, FRHITexture* Texture, bool bRequiredForSimulation, bool bRequiredForRendering) const
{
	if (bAFREnabled && bRequiredForSimulation)
	{
		if (Texture)
		{
			const_cast<FNiagaraGpuComputeDispatch*>(this)->AFRTextures.Add(Texture);
		}
	}
	if (bCrossGPUTransferEnabled && bRequiredForRendering)
	{
		const bool bPullData = false;
		const bool bLockStep = false;

		const FRHIGPUMask GPUMask = RHICmdList.GetGPUMask();
		for (uint32 GPUIndex : FRHIGPUMask::All())
		{
			if (!GPUMask.Contains(GPUIndex))
			{
				const_cast<FNiagaraGpuComputeDispatch*>(this)->CrossGPUTransferBuffers.Emplace(Texture, GPUMask.GetFirstIndex(), GPUIndex, bPullData, bLockStep);
			}
		}
	}
}

void FNiagaraGpuComputeDispatch::AddAFRBuffer(FRHIBuffer* Buffer)
{
	check(bAFREnabled);
	if (Buffer)
	{
		AFRBuffers.Add(Buffer);
	}
}

void FNiagaraGpuComputeDispatch::AddCrossGPUTransfer(FRHICommandList& RHICmdList, FRHIBuffer* Buffer)
{
	check(bCrossGPUTransferEnabled);
	if (Buffer)
	{
		const bool bPullData = false;
		const bool bLockStep = false;

		const FRHIGPUMask GPUMask = RHICmdList.GetGPUMask();
		for (uint32 GPUIndex : FRHIGPUMask::All())
		{
			if (!GPUMask.Contains(GPUIndex))
			{
				CrossGPUTransferBuffers.Emplace(Buffer, GPUMask.GetFirstIndex(), GPUIndex, bPullData, bLockStep);
			}
		}
	}
}

void FNiagaraGpuComputeDispatch::CalculateCrossGPUTransferLocation()
{
	StageToTransferGPUBuffers = ENiagaraGpuComputeTickStage::Last;
	while (StageToTransferGPUBuffers > ENiagaraGpuComputeTickStage::First && !DispatchListPerStage[static_cast<int32>(StageToTransferGPUBuffers)].HasWork())
	{
		StageToTransferGPUBuffers = static_cast<ENiagaraGpuComputeTickStage::Type>(static_cast<int32>(StageToTransferGPUBuffers) - 1);
	}

	StageToWaitForGPUTransfers = ENiagaraGpuComputeTickStage::First;
	// If we're going to write to the instance count buffer after PreInitViews then
	// that needs to be the wait stage, regardless of whether or not we're ticking
	// anything in that stage.
	if (!GPUInstanceCounterManager.HasEntriesPendingFree())
	{
		while (StageToWaitForGPUTransfers < StageToTransferGPUBuffers && !DispatchListPerStage[static_cast<int32>(StageToWaitForGPUTransfers)].HasWork())
		{
			StageToWaitForGPUTransfers = static_cast<ENiagaraGpuComputeTickStage::Type>(static_cast<int32>(StageToWaitForGPUTransfers) + 1);
		}
	}
}

void FNiagaraGpuComputeDispatch::TransferMultiGPUBufers(FRHICommandList& RHICmdList, ENiagaraGpuComputeTickStage::Type TickStage)
{
	if (StageToTransferGPUBuffers != TickStage)
	{
		return;
	}

	// Transfer buffers for AFR rendering
	if (AFRBuffers.Num())
	{
		AddAFRBuffer(GPUInstanceCounterManager.GetInstanceCountBuffer().Buffer);
		RHICmdList.BroadcastTemporalEffect(FNiagaraGpuComputeDispatchLocal::TemporalEffectBuffersName, AFRBuffers);
		AFRBuffers.Reset();
	}
	if (AFRTextures.Num())
	{
		RHICmdList.BroadcastTemporalEffect(FNiagaraGpuComputeDispatchLocal::TemporalEffectTexturesName, AFRTextures);
		AFRTextures.Reset();
	}

	// Transfer buffers for cross GPU rendering
	if (CrossGPUTransferBuffers.Num())
	{
		AddCrossGPUTransfer(RHICmdList, GPUInstanceCounterManager.GetInstanceCountBuffer().Buffer);
		RHICmdList.TransferResources(CrossGPUTransferBuffers);
		CrossGPUTransferBuffers.Reset();
	}
}

void FNiagaraGpuComputeDispatch::WaitForMultiGPUBuffers(FRHICommandList& RHICmdList, ENiagaraGpuComputeTickStage::Type TickStage)
{
	if (StageToWaitForGPUTransfers == TickStage)
	{
		RHICmdList.WaitForTemporalEffect(FNiagaraGpuComputeDispatchLocal::TemporalEffectBuffersName);
		RHICmdList.WaitForTemporalEffect(FNiagaraGpuComputeDispatchLocal::TemporalEffectTexturesName);
	}
}
#endif // WITH_MGPU
