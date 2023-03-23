// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NiagaraCommon.h"
#include "NiagaraEmptyUAVPool.h"
#include "NiagaraGPUInstanceCountManager.h"
#include "FXSystem.h"

class FNiagaraAsyncGpuTraceHelper;
struct FNiagaraComputeExecutionContext;
class FNiagaraGpuComputeDebug;
class FNiagaraGPUInstanceCountManager;
class FNiagaraGpuReadbackManager;
class FNiagaraRayTracingHelper;
struct FNiagaraScriptDebuggerInfo;
class FNiagaraSystemGpuComputeProxy;

// Public API for Niagara's Compute Dispatcher
// This is generally used with DataInterfaces or Custom Renderers
class NIAGARA_API FNiagaraGpuComputeDispatchInterface : public FFXSystemInterface
{
public:
	static FNiagaraGpuComputeDispatchInterface* Get(class UWorld* World);
	static FNiagaraGpuComputeDispatchInterface* Get(class FSceneInterface* Scene);
	static FNiagaraGpuComputeDispatchInterface* Get(class FFXSystemInterface* FXSceneInterface);

	explicit FNiagaraGpuComputeDispatchInterface(EShaderPlatform InShaderPlatform, ERHIFeatureLevel::Type InFeatureLevel);
	virtual ~FNiagaraGpuComputeDispatchInterface();

	/** Get ShaderPlatform the batcher is bound to */
	EShaderPlatform GetShaderPlatform() const { return ShaderPlatform; }
	/** Get FeatureLevel the batcher is bound to */
	ERHIFeatureLevel::Type GetFeatureLevel() const { return FeatureLevel; }

	/** Add system instance proxy to the batcher for tracking. */
	virtual void AddGpuComputeProxy(FNiagaraSystemGpuComputeProxy* ComputeProxy) = 0;
	/** Remove system instance proxy from the batcher. */
	virtual void RemoveGpuComputeProxy(FNiagaraSystemGpuComputeProxy* ComputeProxy) = 0;

	/**
	 * Register work for GPU sorting (using the GPUSortManager).
	 * The constraints of the sort request are defined in SortInfo.SortFlags.
	 * The sort task bindings are set in SortInfo.AllocationInfo.
	 * The initial keys and values are generated in the GenerateSortKeys() callback.
	 *
	 * Return true if the work was registered, or false it GPU sorting is not available or impossible.
	 */
	virtual bool AddSortedGPUSimulation(struct FNiagaraGPUSortInfo& SortInfo) = 0;

	/** Get access to the instance count manager. */
	FORCEINLINE FNiagaraGPUInstanceCountManager& GetGPUInstanceCounterManager() { check(IsInRenderingThread()); return GPUInstanceCounterManager; }
	FORCEINLINE const FNiagaraGPUInstanceCountManager& GetGPUInstanceCounterManager() const { check(IsInRenderingThread()); return GPUInstanceCounterManager; }

#if NIAGARA_COMPUTEDEBUG_ENABLED
	/** Get access to Niagara's GpuComputeDebug */
	FNiagaraGpuComputeDebug* GetGpuComputeDebug() const { return GpuComputeDebugPtr.Get(); }
#endif

#if WITH_NIAGARA_GPU_PROFILER
	/** Access to Niagara's GPU Profiler */
	virtual class FNiagaraGPUProfilerInterface* GetGPUProfiler() const = 0;
#endif

	/** Get access to Niagara's GpuReadbackManager. */
	FNiagaraGpuReadbackManager* GetGpuReadbackManager() const { return GpuReadbackManagerPtr.Get(); }

	/** Get access to Niagara's GpuReadbackManager. */
	FNiagaraEmptyUAVPool* GetEmptyUAVPool() const { return EmptyUAVPoolPtr.Get(); }

	/** Convenience wrapped to get a UAV from the poo. */
	FRHIUnorderedAccessView* GetEmptyUAVFromPool(FRHICommandList& RHICmdList, EPixelFormat Format, ENiagaraEmptyUAVType Type) const { return EmptyUAVPoolPtr->GetEmptyUAVFromPool(RHICmdList, Format, Type); }

	/**
	Call this to force all pending ticks to be flushed from the batcher.
	Doing so will execute them outside of a view context which may result in undesirable results.
	*/
	virtual void FlushPendingTicks_GameThread() = 0;

	/** Debug only function to readback data. */
	virtual void AddDebugReadback(FNiagaraSystemInstanceID InstanceID, TSharedPtr<FNiagaraScriptDebuggerInfo, ESPMode::ThreadSafe> DebugInfo, FNiagaraComputeExecutionContext* Context) = 0;

	/** Processes all pending debug readbacks */
	virtual void ProcessDebugReadbacks(FRHICommandListImmediate& RHICmdList, bool bWaitCompletion) = 0;

	virtual FNiagaraAsyncGpuTraceHelper& GetAsyncGpuTraceHelper() const = 0;

#if WITH_MGPU
	/**
	Notify that a GPU resource was modified that will impact MultiGPU rendering.
	bRequiredForSimulation	- When true we require this resource for simulation passes
	bRequiredForRendering	- When true we require this resource for rendering passes
	*/
	virtual void MultiGPUResourceModified(FRHICommandList& RHICmdList, FRHIBuffer* Buffer, bool bRequiredForSimulation, bool bRequiredForRendering) const = 0;
	virtual void MultiGPUResourceModified(FRHICommandList& RHICmdList, FRHITexture* Texture, bool bRequiredForSimulation, bool bRequiredForRendering) const = 0;
#else
	FORCEINLINE void MultiGPUResourceModified(FRHICommandList& RHICmdList, FRHIBuffer* Buffer, bool bRequiredForSimulation, bool bRequiredForRendering) const {}
	FORCEINLINE void MultiGPUResourceModified(FRHICommandList& RHICmdList, FRHITexture* Texture, bool bRequiredForSimulation, bool bRequiredForRendering) const {}
#endif

protected:
	EShaderPlatform							ShaderPlatform;
	ERHIFeatureLevel::Type					FeatureLevel;
#if NIAGARA_COMPUTEDEBUG_ENABLED
	TUniquePtr<FNiagaraGpuComputeDebug>		GpuComputeDebugPtr;
#endif
	TUniquePtr<FNiagaraGpuReadbackManager>	GpuReadbackManagerPtr;
	TUniquePtr<FNiagaraEmptyUAVPool>		EmptyUAVPoolPtr;

	// GPU emitter instance count buffer. Contains the actual particle / instance count generate in the GPU tick.
	FNiagaraGPUInstanceCountManager			GPUInstanceCounterManager;
};
