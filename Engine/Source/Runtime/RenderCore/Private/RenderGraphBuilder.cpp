// Copyright Epic Games, Inc. All Rights Reserved.

#include "RenderGraphBuilder.h"
#include "RenderGraphPrivate.h"
#include "RenderGraphTrace.h"
#include "RenderTargetPool.h"
#include "RenderGraphResourcePool.h"
#include "VisualizeTexture.h"
#include "ProfilingDebugging/CsvProfiler.h"


#if ENABLE_RHI_VALIDATION

inline void GatherPassUAVsForOverlapValidation(const FRDGPass* Pass, TArray<FRHIUnorderedAccessView*, TInlineAllocator<MaxSimultaneousUAVs, FRDGArrayAllocator>>& OutUAVs)
{
	// RHI validation tracking of Begin/EndUAVOverlaps happens on the underlying resource, so we need to be careful about not
	// passing multiple UAVs that refer to the same resource, otherwise we get double-Begin and double-End validation errors.
	// Filter UAVs to only those with unique parent resources.
	TArray<FRDGParentResource*, TInlineAllocator<MaxSimultaneousUAVs, FRDGArrayAllocator>> UniqueParents;
	Pass->GetParameters().Enumerate([&](FRDGParameter Parameter)
	{
		if (Parameter.IsUAV())
		{
			if (FRDGUnorderedAccessViewRef UAV = Parameter.GetAsUAV())
			{
				FRDGParentResource* Parent = UAV->GetParent();

				// Check if we've already seen this parent.
				bool bFound = false;
				for (int32 Index = 0; !bFound && Index < UniqueParents.Num(); ++Index)
				{
					bFound = UniqueParents[Index] == Parent;
				}

				if (!bFound)
				{
					UniqueParents.Add(Parent);
					OutUAVs.Add(UAV->GetRHI());
				}
			}
		}
	});
}

#endif

inline void BeginUAVOverlap(const FRDGPass* Pass, FRHIComputeCommandList& RHICmdList)
{
#if ENABLE_RHI_VALIDATION
	TArray<FRHIUnorderedAccessView*, TInlineAllocator<MaxSimultaneousUAVs, FRDGArrayAllocator>> UAVs;
	GatherPassUAVsForOverlapValidation(Pass, UAVs);

	if (UAVs.Num())
	{
		RHICmdList.BeginUAVOverlap(UAVs);
	}
#endif
}

inline void EndUAVOverlap(const FRDGPass* Pass, FRHIComputeCommandList& RHICmdList)
{
#if ENABLE_RHI_VALIDATION
	TArray<FRHIUnorderedAccessView*, TInlineAllocator<MaxSimultaneousUAVs, FRDGArrayAllocator>> UAVs;
	GatherPassUAVsForOverlapValidation(Pass, UAVs);

	if (UAVs.Num())
	{
		RHICmdList.EndUAVOverlap(UAVs);
	}
#endif
}

inline ERHIAccess MakeValidAccess(ERHIAccess Access)
{
	// If we find any write states in the access mask, remove all read-only states. This mainly exists
	// to allow RDG uniform buffers to contain read-only parameters which are also bound for write on the
	// pass. Often times these uniform buffers are created and only relevant things are accessed. If an
	// invalid access does occur, the RHI validation layer will catch it.
	return IsWritableAccess(Access) ? (Access & ~ERHIAccess::ReadOnlyExclusiveMask) : Access;
}

inline void GetPassAccess(ERDGPassFlags PassFlags, ERHIAccess& SRVAccess, ERHIAccess& UAVAccess)
{
	SRVAccess = ERHIAccess::Unknown;
	UAVAccess = ERHIAccess::Unknown;

	if (EnumHasAnyFlags(PassFlags, ERDGPassFlags::Raster))
	{
		SRVAccess |= ERHIAccess::SRVGraphics;
		UAVAccess |= ERHIAccess::UAVGraphics;
	}

	if (EnumHasAnyFlags(PassFlags, ERDGPassFlags::AsyncCompute | ERDGPassFlags::Compute))
	{
		SRVAccess |= ERHIAccess::SRVCompute;
		UAVAccess |= ERHIAccess::UAVCompute;
	}

	if (EnumHasAnyFlags(PassFlags, ERDGPassFlags::Copy))
	{
		SRVAccess |= ERHIAccess::CopySrc;
	}
}

enum class ERDGTextureAccessFlags
{
	None = 0,

	// Access is within the fixed-function render pass.
	RenderTarget = 1 << 0
};
ENUM_CLASS_FLAGS(ERDGTextureAccessFlags);

/** Enumerates all texture accesses and provides the access and subresource range info. This results in
 *  multiple invocations of the same resource, but with different access / subresource range.
 */
template <typename TAccessFunction>
void EnumerateTextureAccess(FRDGParameterStruct PassParameters, ERDGPassFlags PassFlags, TAccessFunction AccessFunction)
{
	const ERDGTextureAccessFlags NoneFlags = ERDGTextureAccessFlags::None;

	ERHIAccess SRVAccess, UAVAccess;
	GetPassAccess(PassFlags, SRVAccess, UAVAccess);

	PassParameters.EnumerateTextures([&](FRDGParameter Parameter)
	{
		switch (Parameter.GetType())
		{
		case UBMT_RDG_TEXTURE:
			if (FRDGTextureRef Texture = Parameter.GetAsTexture())
			{
				AccessFunction(nullptr, Texture, SRVAccess, NoneFlags, Texture->GetSubresourceRangeSRV());
			}
		break;
		case UBMT_RDG_TEXTURE_ACCESS:
		{
			if (FRDGTextureAccess TextureAccess = Parameter.GetAsTextureAccess())
			{
				AccessFunction(nullptr, TextureAccess.GetTexture(), TextureAccess.GetAccess(), NoneFlags, TextureAccess->GetSubresourceRange());
			}
		}
		break;
		case UBMT_RDG_TEXTURE_ACCESS_ARRAY:
		{
			const FRDGTextureAccessArray& TextureAccessArray = Parameter.GetAsTextureAccessArray();

			for (FRDGTextureAccess TextureAccess : TextureAccessArray)
			{
				AccessFunction(nullptr, TextureAccess.GetTexture(), TextureAccess.GetAccess(), NoneFlags, TextureAccess->GetSubresourceRange());
			}
		}
		break;
		case UBMT_RDG_TEXTURE_SRV:
			if (FRDGTextureSRVRef SRV = Parameter.GetAsTextureSRV())
			{
				AccessFunction(SRV, SRV->GetParent(), SRVAccess, NoneFlags, SRV->GetSubresourceRange());
			}
		break;
		case UBMT_RDG_TEXTURE_UAV:
			if (FRDGTextureUAVRef UAV = Parameter.GetAsTextureUAV())
			{
				AccessFunction(UAV, UAV->GetParent(), UAVAccess, NoneFlags, UAV->GetSubresourceRange());
			}
		break;
		case UBMT_RENDER_TARGET_BINDING_SLOTS:
		{
			const ERDGTextureAccessFlags RenderTargetAccess = ERDGTextureAccessFlags::RenderTarget;

			const ERHIAccess RTVAccess = ERHIAccess::RTV;

			const FRenderTargetBindingSlots& RenderTargets = Parameter.GetAsRenderTargetBindingSlots();

			RenderTargets.Enumerate([&](FRenderTargetBinding RenderTarget)
			{
				FRDGTextureRef Texture = RenderTarget.GetTexture();
				FRDGTextureRef ResolveTexture = RenderTarget.GetResolveTexture();

				FRDGTextureSubresourceRange Range(Texture->GetSubresourceRange());
				Range.MipIndex = RenderTarget.GetMipIndex();
				Range.NumMips = 1;

				if (RenderTarget.GetArraySlice() != -1)
				{
					Range.ArraySlice = RenderTarget.GetArraySlice();
					Range.NumArraySlices = 1;
				}

				AccessFunction(nullptr, Texture, RTVAccess, RenderTargetAccess, Range);

				if (ResolveTexture && ResolveTexture != Texture)
				{
					// Resolve targets must use the RTV|ResolveDst flag combination when the resolve is performed through the render
					// pass. The ResolveDst flag must be used alone only when the resolve is performed using RHICopyToResolveTarget.
					AccessFunction(nullptr, ResolveTexture, ERHIAccess::RTV | ERHIAccess::ResolveDst, RenderTargetAccess, Range);
				}
			});

			const FDepthStencilBinding& DepthStencil = RenderTargets.DepthStencil;

			if (FRDGTextureRef Texture = DepthStencil.GetTexture())
			{
				DepthStencil.GetDepthStencilAccess().EnumerateSubresources([&](ERHIAccess NewAccess, uint32 PlaneSlice)
				{
					FRDGTextureSubresourceRange Range = Texture->GetSubresourceRange();

					// Adjust the range to use a single plane slice if not using of them all.
					if (PlaneSlice != FRHITransitionInfo::kAllSubresources)
					{
						Range.PlaneSlice = PlaneSlice;
						Range.NumPlaneSlices = 1;
					}

					AccessFunction(nullptr, Texture, NewAccess, RenderTargetAccess, Range);
				});
			}

			if (FRDGTextureRef Texture = RenderTargets.ShadingRateTexture)
			{
				AccessFunction(nullptr, Texture, ERHIAccess::ShadingRateSource, RenderTargetAccess, Texture->GetSubresourceRangeSRV());
			}
		}
		break;
		}
	});
}

/** Enumerates all buffer accesses and provides the access info. */
template <typename TAccessFunction>
void EnumerateBufferAccess(FRDGParameterStruct PassParameters, ERDGPassFlags PassFlags, TAccessFunction AccessFunction)
{
	ERHIAccess SRVAccess, UAVAccess;
	GetPassAccess(PassFlags, SRVAccess, UAVAccess);

	PassParameters.EnumerateBuffers([&](FRDGParameter Parameter)
	{
		switch (Parameter.GetType())
		{
		case UBMT_RDG_BUFFER_ACCESS:
			if (FRDGBufferAccess BufferAccess = Parameter.GetAsBufferAccess())
			{
				AccessFunction(nullptr, BufferAccess.GetBuffer(), BufferAccess.GetAccess());
			}
		break;
		case UBMT_RDG_BUFFER_ACCESS_ARRAY:
		{
			const FRDGBufferAccessArray& BufferAccessArray = Parameter.GetAsBufferAccessArray();

			for (FRDGBufferAccess BufferAccess : BufferAccessArray)
			{
				AccessFunction(nullptr, BufferAccess.GetBuffer(), BufferAccess.GetAccess());
			}
		}
		break;
		case UBMT_RDG_BUFFER_SRV:
			if (FRDGBufferSRVRef SRV = Parameter.GetAsBufferSRV())
			{
				FRDGBufferRef Buffer = SRV->GetParent();
				ERHIAccess BufferAccess = SRVAccess;

				if (EnumHasAnyFlags(Buffer->Desc.Usage, BUF_AccelerationStructure))
				{
					BufferAccess = ERHIAccess::BVHRead;
				}

				AccessFunction(SRV, Buffer, BufferAccess);
			}
		break;
		case UBMT_RDG_BUFFER_UAV:
			if (FRDGBufferUAVRef UAV = Parameter.GetAsBufferUAV())
			{
				AccessFunction(UAV, UAV->GetParent(), UAVAccess);
			}
		break;
		}
	});
}

inline FRDGViewHandle GetHandleIfNoUAVBarrier(FRDGViewRef Resource)
{
	if (Resource && (Resource->Type == ERDGViewType::BufferUAV || Resource->Type == ERDGViewType::TextureUAV))
	{
		if (EnumHasAnyFlags(static_cast<FRDGUnorderedAccessViewRef>(Resource)->Flags, ERDGUnorderedAccessViewFlags::SkipBarrier))
		{
			return Resource->GetHandle();
		}
	}
	return FRDGViewHandle::Null;
}

inline EResourceTransitionFlags GetTextureViewTransitionFlags(FRDGViewRef Resource, FRDGTextureRef Texture)
{
	if (Resource)
	{
		switch (Resource->Type)
		{
		case ERDGViewType::TextureUAV:
		{
			FRDGTextureUAVRef UAV = static_cast<FRDGTextureUAVRef>(Resource);
			if (UAV->Desc.MetaData != ERDGTextureMetaDataAccess::None)
			{
				return EResourceTransitionFlags::MaintainCompression;
			}
		}
		break;
		case ERDGViewType::TextureSRV:
		{
			FRDGTextureSRVRef SRV = static_cast<FRDGTextureSRVRef>(Resource);
			if (SRV->Desc.MetaData != ERDGTextureMetaDataAccess::None)
			{
				return EResourceTransitionFlags::MaintainCompression;
			}
		}
		break;
		}
	}
	else
	{
		if (EnumHasAnyFlags(Texture->Flags, ERDGTextureFlags::MaintainCompression))
		{
			return EResourceTransitionFlags::MaintainCompression;
		}
	}
	return EResourceTransitionFlags::None;
}

void FRDGBuilder::SetFlushResourcesRHI()
{
	if (GRHINeedsExtraDeletionLatency || !GRHICommandList.Bypass())
	{
		checkf(!bFlushResourcesRHI, TEXT("SetFlushRHIResources has been already been called. It may only be called once."));
		bFlushResourcesRHI = true;

		if (IsImmediateMode())
		{
			BeginFlushResourcesRHI();
			EndFlushResourcesRHI();
		}
	}
}

void FRDGBuilder::BeginFlushResourcesRHI()
{
	if (!bFlushResourcesRHI)
	{
		return;
	}

	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(STAT_RDG_FlushResourcesRHI);
	SCOPED_NAMED_EVENT(BeginFlushResourcesRHI, FColor::Emerald);
	RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
}

void FRDGBuilder::EndFlushResourcesRHI()
{
	if (!bFlushResourcesRHI)
	{
		return;
	}

	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(STAT_RDG_FlushResourcesRHI);
	SCOPED_NAMED_EVENT(EndFlushResourcesRHI, FColor::Emerald);
	RHICmdList.WaitForDispatch();
	RHICmdList.WaitForRHIThreadTasks();
	RHICmdList.WaitForTasks(true /* bKnownToBeComplete */);
	PipelineStateCache::FlushResources();
	FRHIResource::FlushPendingDeletes(RHICmdList);
}

void FRDGBuilder::TickPoolElements()
{
	GRenderGraphResourcePool.TickPoolElements();

#if RDG_ENABLE_DEBUG
	if (GRDGDumpGraph)
	{
		--GRDGDumpGraph;
	}
	if (GRDGTransitionLog > 0)
	{
		--GRDGTransitionLog;
	}
	GRDGDumpGraphUnknownCount = 0;
#endif

#if STATS
	SET_DWORD_STAT(STAT_RDG_PassCount, GRDGStatPassCount);
	SET_DWORD_STAT(STAT_RDG_PassWithParameterCount, GRDGStatPassWithParameterCount);
	SET_DWORD_STAT(STAT_RDG_PassCullCount, GRDGStatPassCullCount);
	SET_DWORD_STAT(STAT_RDG_RenderPassMergeCount, GRDGStatRenderPassMergeCount);
	SET_DWORD_STAT(STAT_RDG_PassDependencyCount, GRDGStatPassDependencyCount);
	SET_DWORD_STAT(STAT_RDG_TextureCount, GRDGStatTextureCount);
	SET_DWORD_STAT(STAT_RDG_TextureReferenceCount, GRDGStatTextureReferenceCount);
	SET_FLOAT_STAT(STAT_RDG_TextureReferenceAverage, (float)(GRDGStatTextureReferenceCount / FMath::Max((float)GRDGStatTextureCount, 1.0f)));
	SET_DWORD_STAT(STAT_RDG_BufferCount, GRDGStatBufferCount);
	SET_DWORD_STAT(STAT_RDG_BufferReferenceCount, GRDGStatBufferReferenceCount);
	SET_FLOAT_STAT(STAT_RDG_BufferReferenceAverage, (float)(GRDGStatBufferReferenceCount / FMath::Max((float)GRDGStatBufferCount, 1.0f)));
	SET_DWORD_STAT(STAT_RDG_ViewCount, GRDGStatViewCount);
	SET_DWORD_STAT(STAT_RDG_TransientTextureCount, GRDGStatTransientTextureCount);
	SET_DWORD_STAT(STAT_RDG_TransientBufferCount, GRDGStatTransientBufferCount);
	SET_DWORD_STAT(STAT_RDG_TransitionCount, GRDGStatTransitionCount);
	SET_DWORD_STAT(STAT_RDG_AliasingCount, GRDGStatAliasingCount);
	SET_DWORD_STAT(STAT_RDG_TransitionBatchCount, GRDGStatTransitionBatchCount);
	SET_MEMORY_STAT(STAT_RDG_MemoryWatermark, int64(GRDGStatMemoryWatermark));
	GRDGStatPassCount = 0;
	GRDGStatPassWithParameterCount = 0;
	GRDGStatPassCullCount = 0;
	GRDGStatRenderPassMergeCount = 0;
	GRDGStatPassDependencyCount = 0;
	GRDGStatTextureCount = 0;
	GRDGStatTextureReferenceCount = 0;
	GRDGStatBufferCount = 0;
	GRDGStatBufferReferenceCount = 0;
	GRDGStatViewCount = 0;
	GRDGStatTransientTextureCount = 0;
	GRDGStatTransientBufferCount = 0;
	GRDGStatTransitionCount = 0;
	GRDGStatAliasingCount = 0;
	GRDGStatTransitionBatchCount = 0;
	GRDGStatMemoryWatermark = 0;
#endif
}

bool FRDGBuilder::IsImmediateMode()
{
	return ::IsImmediateMode();
}

ERDGPassFlags FRDGBuilder::OverridePassFlags(const TCHAR* PassName, ERDGPassFlags PassFlags, bool bAsyncComputeSupported)
{
	const bool bDebugAllowedForPass =
#if RDG_ENABLE_DEBUG
		IsDebugAllowedForPass(PassName);
#else
		true;
#endif

	const bool bGlobalForceAsyncCompute = (GRDGAsyncCompute == RDG_ASYNC_COMPUTE_FORCE_ENABLED && !IsImmediateMode() && bDebugAllowedForPass);

	if (EnumHasAnyFlags(PassFlags, ERDGPassFlags::Compute) && (bGlobalForceAsyncCompute))
	{
		PassFlags &= ~ERDGPassFlags::Compute;
		PassFlags |= ERDGPassFlags::AsyncCompute;
	}

	if (EnumHasAnyFlags(PassFlags, ERDGPassFlags::AsyncCompute) && (GRDGAsyncCompute == RDG_ASYNC_COMPUTE_DISABLED || IsImmediateMode() || !bAsyncComputeSupported))
	{
		PassFlags &= ~ERDGPassFlags::AsyncCompute;
		PassFlags |= ERDGPassFlags::Compute;
	}

	return PassFlags;
}

bool FRDGBuilder::IsTransient(FRDGBufferRef Buffer) const
{
	if (!IsTransientInternal(Buffer, EnumHasAnyFlags(Buffer->Desc.Usage, BUF_FastVRAM)))
	{
		return false;
	}

	if (!GRDGTransientIndirectArgBuffers && EnumHasAnyFlags(Buffer->Desc.Usage, BUF_DrawIndirect))
	{
		return false;
	}

	return EnumHasAnyFlags(Buffer->Desc.Usage, BUF_UnorderedAccess);
}

bool FRDGBuilder::IsTransient(FRDGTextureRef Texture) const
{
	return IsTransientInternal(Texture, EnumHasAnyFlags(Texture->Desc.Flags, ETextureCreateFlags::FastVRAM));
}

bool FRDGBuilder::IsTransientInternal(FRDGParentResourceRef Resource, bool bFastVRAM) const
{
	// Immediate mode can't use the transient allocator because we don't know if the user will extract the resource.
	if (!GRDGTransientAllocator || IsImmediateMode())
	{
		return false;
	}

	// FastVRAM resources are always transient regardless of extraction or other hints, since they are performance critical.
	if (!bFastVRAM || !FPlatformMemory::SupportsFastVRAMMemory())
	{
		if (GRDGTransientAllocator == 2)
		{
			return false;
		}
	
		if (Resource->bForceNonTransient)
		{
			return false;
		}

		if (Resource->bExtracted)
		{
			if (GRDGTransientExtractedResources == 0)
			{
				return false;
			}

			if (GRDGTransientExtractedResources == 1 && Resource->TransientExtractionHint == FRDGParentResource::ETransientExtractionHint::Disable)
			{
				return false;
			}
		}
	}

#if RDG_ENABLE_DEBUG
	if (GRDGDebugDisableTransientResources != 0 && IsDebugAllowedForResource(Resource->Name))
	{
		return false;
	}
#endif

	return true;
}

FRDGBuilder::FRDGBuilder(FRHICommandListImmediate& InRHICmdList, FRDGEventName InName, ERDGBuilderFlags InFlags)
	: RHICmdList(InRHICmdList)
	, Blackboard(Allocator)
	, RHICmdListAsyncCompute(FRHICommandListExecutor::GetImmediateAsyncComputeCommandList())
	, BuilderName(InName)
#if RDG_CPU_SCOPES
	, CPUScopeStacks(Allocator)
#endif
#if RDG_GPU_SCOPES
	, GPUScopeStacks(Allocator)
#endif
	, bParallelExecuteEnabled(IsParallelExecuteEnabled() && EnumHasAnyFlags(InFlags, ERDGBuilderFlags::AllowParallelExecute))
#if RDG_ENABLE_DEBUG
	, UserValidation(Allocator, bParallelExecuteEnabled)
	, BarrierValidation(&Passes, BuilderName)
	, LogFile(Passes)
#endif
	, TransientResourceAllocator(GRDGTransientResourceAllocator.Get())
{
	AddProloguePass();

#if RDG_EVENTS != RDG_EVENTS_NONE
	// This is polled once as a workaround for a race condition since the underlying global is not always changed on the render thread.
	GRDGEmitEvents = GetEmitDrawEvents();
#endif

#if RHI_WANT_BREADCRUMB_EVENTS
	if (bParallelExecuteEnabled)
	{
		BreadcrumbState = FRDGBreadcrumbState::Create(Allocator);
	}
#endif

	IF_RDG_ENABLE_DEBUG(LogFile.Begin(BuilderName));
}

FRDGBuilder::~FRDGBuilder()
{
	SCOPED_NAMED_EVENT(FRDGBuilder_Clear, FColor::Emerald);

	Passes.Clear();
	Buffers.Clear();
	UniformBuffers.Clear();
	Blackboard.Clear();
	ActivePooledTextures.Empty();
	ActivePooledBuffers.Empty();
}

const TRefCountPtr<FRDGPooledBuffer>& FRDGBuilder::ConvertToExternalBuffer(FRDGBufferRef Buffer)
{
	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateConvertToExternalResource(Buffer));
	if (!Buffer->bExternal)
	{
		Buffer->bExternal = 1;
		Buffer->bForceNonTransient = 1;
		Buffer->AccessFinal = kDefaultAccessFinal;
		BeginResourceRHI(GetProloguePassHandle(), Buffer);
		ExternalBuffers.Add(Buffer->PooledBuffer, Buffer);
	}
	return GetPooledBuffer(Buffer);
}

const TRefCountPtr<IPooledRenderTarget>& FRDGBuilder::ConvertToExternalTexture(FRDGTextureRef Texture)
{
	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateConvertToExternalResource(Texture));
	if (!Texture->bExternal)
	{
		Texture->bExternal = 1;
		Texture->bForceNonTransient = 1;
		Texture->AccessFinal = kDefaultAccessFinal;
		BeginResourceRHI(GetProloguePassHandle(), Texture);
		ExternalTextures.Add(Texture->GetRHIUnchecked(), Texture);
	}
	return GetPooledTexture(Texture);
}

BEGIN_SHADER_PARAMETER_STRUCT(FFinalizePassParameters, )
	RDG_TEXTURE_ACCESS_ARRAY(Textures)
	RDG_BUFFER_ACCESS_ARRAY(Buffers)
END_SHADER_PARAMETER_STRUCT()

void FRDGBuilder::FinalizeResourceAccess(FRDGTextureAccessArray&& InTextures, FRDGBufferAccessArray&& InBuffers)
{
	auto* PassParameters = AllocParameters<FFinalizePassParameters>();
	PassParameters->Textures = Forward<FRDGTextureAccessArray&&>(InTextures);
	PassParameters->Buffers = Forward<FRDGBufferAccessArray&&>(InBuffers);

	// Take reference to pass parameters version since we've moved the memory.
	const auto& LocalTextures = PassParameters->Textures;
	const auto& LocalBuffers = PassParameters->Buffers;

#if RDG_ENABLE_DEBUG
	{
		const FRDGPassHandle FinalizePassHandle(Passes.Num());

		for (FRDGTextureAccess TextureAccess : LocalTextures)
		{
			UserValidation.ValidateFinalize(TextureAccess.GetTexture(), TextureAccess.GetAccess(), FinalizePassHandle);
		}

		for (FRDGBufferAccess BufferAccess : LocalBuffers)
		{
			UserValidation.ValidateFinalize(BufferAccess.GetBuffer(), BufferAccess.GetAccess(), FinalizePassHandle);
		}
	}
#endif

	AddPass(
		RDG_EVENT_NAME("FinalizeResourceAccess(Textures: %d, Buffers: %d)", LocalTextures.Num(), LocalBuffers.Num()),
		PassParameters,
		// Use all of the work flags so that any access is valid.
		ERDGPassFlags::Copy | ERDGPassFlags::Compute | ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass |
		// We're not writing to anything, so we have to tell the pass not to cull.
		ERDGPassFlags::NeverCull,
		[](FRHICommandList&) {});

	// bFinalized must be set after adding the finalize pass, as future declarations of the resource will be ignored.

	for (FRDGTextureAccess TextureAccess : LocalTextures)
	{
		TextureAccess->bFinalizedAccess = 1;
	}

	for (FRDGBufferAccess BufferAccess : LocalBuffers)
	{
		BufferAccess->bFinalizedAccess = 1;
	}
}

FRDGTextureRef FRDGBuilder::RegisterExternalTexture(
	const TRefCountPtr<IPooledRenderTarget>& ExternalPooledTexture,
	ERDGTextureFlags Flags)
{
#if RDG_ENABLE_DEBUG
	checkf(ExternalPooledTexture.IsValid(), TEXT("Attempted to register NULL external texture."));
#endif

	const TCHAR* Name = ExternalPooledTexture->GetDesc().DebugName;
	if (!Name)
	{
		Name = TEXT("External");
	}
	return RegisterExternalTexture(ExternalPooledTexture, Name, Flags);
}

FRDGTextureRef FRDGBuilder::RegisterExternalTexture(
	const TRefCountPtr<IPooledRenderTarget>& ExternalPooledTexture,
	const TCHAR* Name,
	ERDGTextureFlags Flags)
{
	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateRegisterExternalTexture(ExternalPooledTexture, Name, Flags));
	FRHITexture* ExternalTextureRHI = ExternalPooledTexture->GetRHI();
	IF_RDG_ENABLE_DEBUG(checkf(ExternalTextureRHI, TEXT("Attempted to register texture %s, but its RHI texture is null."), Name));

	if (FRDGTextureRef FoundTexture = FindExternalTexture(ExternalTextureRHI))
	{
		return FoundTexture;
	}

	const FRDGTextureDesc Desc = Translate(ExternalPooledTexture->GetDesc());
	bool bFinalizedAccess = false;

	if (!EnumHasAnyFlags(Flags, ERDGTextureFlags::ForceTracking) &&
		!EnumHasAnyFlags(Desc.Flags, TexCreate_RenderTargetable | TexCreate_ResolveTargetable | TexCreate_DepthStencilTargetable | TexCreate_UAV | TexCreate_DepthStencilResolveTarget))
	{
		Flags |= ERDGTextureFlags::ReadOnly;
		bFinalizedAccess = true;
	}

	FRDGTextureRef Texture = Textures.Allocate(Allocator, Name, Desc, Flags);
	Texture->SetRHI(ExternalPooledTexture.GetReference());

	Texture->bExternal = true;
	Texture->AccessFinal = EnumHasAnyFlags(ExternalTextureRHI->GetFlags(), TexCreate_Foveation) ? ERHIAccess::ShadingRateSource : kDefaultAccessFinal;
	Texture->FirstPass = GetProloguePassHandle();

	// Textures that are created read-only are not transitioned by RDG.
	if (bFinalizedAccess)
	{
		// When in 'finalized access' mode, the access represents the valid set of states to touch the resource for
		// validation, not its final state after the graph executes. That's why it's okay to have a write state mixed
		// with read states.
		Texture->bFinalizedAccess = 1;
		Texture->AccessFinal = ERHIAccess::ReadOnlyExclusiveMask;

		if (EnumHasAnyFlags(Desc.Flags, TexCreate_CPUReadback))
		{
			Texture->AccessFinal |= ERHIAccess::CopyDest;
		}

		if (EnumHasAnyFlags(Desc.Flags, TexCreate_Foveation))
		{
			Texture->AccessFinal |= ERHIAccess::ShadingRateSource;
		}
	}

	FRDGTextureSubresourceState& TextureState = Texture->GetState();

	checkf(IsWholeResource(TextureState) && GetWholeResource(TextureState).Access == ERHIAccess::Unknown,
		TEXT("Externally registered texture '%s' has known RDG state. This means the graph did not sanitize it correctly, or ")
		TEXT("an IPooledRenderTarget reference was improperly held within a pass."), Texture->Name);

	ExternalTextures.Add(Texture->GetRHIUnchecked(), Texture);

	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateRegisterExternalTexture(Texture));
	IF_RDG_ENABLE_TRACE(Trace.AddResource(Texture));
	return Texture;
}

FRDGBufferRef FRDGBuilder::RegisterExternalBuffer(const TRefCountPtr<FRDGPooledBuffer>& ExternalPooledBuffer, ERDGBufferFlags Flags)
{
#if RDG_ENABLE_DEBUG
	checkf(ExternalPooledBuffer.IsValid(), TEXT("Attempted to register NULL external buffer."));
#endif

	const TCHAR* Name = ExternalPooledBuffer->Name;
	if (!Name)
	{
		Name = TEXT("External");
	}
	return RegisterExternalBuffer(ExternalPooledBuffer, Name, Flags);
}

FRDGBufferRef FRDGBuilder::RegisterExternalBuffer(
	const TRefCountPtr<FRDGPooledBuffer>& ExternalPooledBuffer,
	const TCHAR* Name,
	ERDGBufferFlags Flags)
{
	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateRegisterExternalBuffer(ExternalPooledBuffer, Name, Flags));

	if (FRDGBufferRef* FoundBufferPtr = ExternalBuffers.Find(ExternalPooledBuffer.GetReference()))
	{
		return *FoundBufferPtr;
	}

	const FRDGBufferDesc& Desc = ExternalPooledBuffer->Desc;
	bool bFinalizedAccess = false;

	if (!EnumHasAnyFlags(Flags, ERDGBufferFlags::ForceTracking) && !EnumHasAnyFlags(Desc.Usage, BUF_UnorderedAccess))
	{
		Flags |= ERDGBufferFlags::ReadOnly;
		bFinalizedAccess = true;
	}

	FRDGBufferRef Buffer = Buffers.Allocate(Allocator, Name, ExternalPooledBuffer->Desc, Flags);
	Buffer->SetRHI(ExternalPooledBuffer);

	Buffer->bExternal = true;
	Buffer->AccessFinal = kDefaultAccessFinal;
	Buffer->FirstPass = GetProloguePassHandle();

	// Buffers that are created read-only are not transitioned by RDG.
	if (bFinalizedAccess)
	{
		Buffer->bFinalizedAccess = 1;
		Buffer->AccessFinal = ERHIAccess::ReadOnlyExclusiveMask;
	}

	FRDGSubresourceState& BufferState = Buffer->GetState();
	checkf(BufferState.Access == ERHIAccess::Unknown,
		TEXT("Externally registered buffer '%s' has known RDG state. This means the graph did not sanitize it correctly, or ")
		TEXT("an FRDGPooledBuffer reference was improperly held within a pass."), Buffer->Name);

	ExternalBuffers.Add(ExternalPooledBuffer, Buffer);

	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateRegisterExternalBuffer(Buffer));
	IF_RDG_ENABLE_TRACE(Trace.AddResource(Buffer));
	return Buffer;
}

void FRDGBuilder::AddPassDependency(FRDGPassHandle ProducerHandle, FRDGPassHandle ConsumerHandle)
{
	FRDGPass* Consumer = Passes[ConsumerHandle];

	auto& Producers = Consumer->Producers;
	if (Producers.Find(ProducerHandle) == INDEX_NONE)
	{
		Producers.Add(ProducerHandle);
	}

#if STATS
	GRDGStatPassDependencyCount++;
#endif
}

void FRDGBuilder::Compile()
{
	SCOPE_CYCLE_COUNTER(STAT_RDG_CompileTime);
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE_CONDITIONAL(RDG_Compile, GRDGVerboseCSVStats != 0);
	SCOPED_NAMED_EVENT(Compile, FColor::Emerald);

	const FRDGPassHandle EpiloguePassHandle = GetEpiloguePassHandle();

	const uint32 CompilePassCount = Passes.Num();

	const bool bCullPasses = GRDGCullPasses > 0;

	TArray<FRDGPassHandle, FRDGArrayAllocator> PassStack;

	if (bCullPasses)
	{
		PassStack.Reserve(CompilePassCount);
	}

	TransitionCreateQueue.Reserve(CompilePassCount);

	FRDGPassBitArray PassesOnAsyncCompute(false, CompilePassCount);

	// Build producer / consumer dependencies across the graph and construct packed bit-arrays of metadata
	// for better cache coherency when searching for passes meeting specific criteria. Search roots are also
	// identified for culling. Passes with untracked RHI output (e.g. SHADER_PARAMETER_{BUFFER, TEXTURE}_UAV)
	// cannot be culled, nor can any pass which writes to an external resource. Resource extractions extend the
	// lifetime to the epilogue pass which is always a root of the graph. The prologue and epilogue are helper
	// passes and therefore never culled.

	if (bCullPasses || AsyncComputePassCount > 0)
	{
		SCOPED_NAMED_EVENT(PassDependencies, FColor::Emerald);

		const auto AddCullingDependency = [&](FRDGProducerStatesByPipeline& LastProducers, const FRDGProducerState& NextState, ERHIPipeline NextPipeline)
		{
			for (ERHIPipeline LastPipeline : GetRHIPipelines())
			{
				FRDGProducerState& LastProducer = LastProducers[LastPipeline];

				if (LastProducer.Access == ERHIAccess::Unknown)
				{
					continue;
				}

				if (FRDGProducerState::IsDependencyRequired(LastProducer, LastPipeline, NextState, NextPipeline))
				{
					AddPassDependency(LastProducer.PassHandle, NextState.PassHandle);
				}
			}

			if (IsWritableAccess(NextState.Access))
			{
				LastProducers[NextPipeline] = NextState;
			}
		};

		for (FRDGPassHandle PassHandle = ProloguePassHandle + 1; PassHandle < EpiloguePassHandle; ++PassHandle)
		{
			FRDGPass* Pass = Passes[PassHandle];
			const ERHIPipeline PassPipeline = Pass->Pipeline;

			bool bUntrackedOutputs = Pass->bHasExternalOutputs;

			for (auto& PassState : Pass->TextureStates)
			{
				FRDGTextureRef Texture = PassState.Texture;
				auto& LastProducers = Texture->LastProducers;

				for (uint32 Index = 0, Count = LastProducers.Num(); Index < Count; ++Index)
				{
					const auto& SubresourceState = PassState.State[Index];

					if (SubresourceState.Access == ERHIAccess::Unknown)
					{
						continue;
					}

					FRDGProducerState ProducerState;
					ProducerState.Access = SubresourceState.Access;
					ProducerState.PassHandle = PassHandle;
					ProducerState.NoUAVBarrierHandle = SubresourceState.NoUAVBarrierFilter.GetUniqueHandle();

					AddCullingDependency(LastProducers[Index], ProducerState, PassPipeline);
				}

				bUntrackedOutputs |= Texture->bExternal;
			}

			for (auto& PassState : Pass->BufferStates)
			{
				FRDGBufferRef Buffer = PassState.Buffer;
				const auto& SubresourceState = PassState.State;

				FRDGProducerState ProducerState;
				ProducerState.Access = SubresourceState.Access;
				ProducerState.PassHandle = PassHandle;
				ProducerState.NoUAVBarrierHandle = SubresourceState.NoUAVBarrierFilter.GetUniqueHandle();

				AddCullingDependency(Buffer->LastProducer, ProducerState, PassPipeline);
				bUntrackedOutputs |= Buffer->bExternal;
			}

			PassesOnAsyncCompute[PassHandle] = EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::AsyncCompute);
			Pass->bCulled = bCullPasses;

			if (bCullPasses && (bUntrackedOutputs || EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::NeverCull)))
			{
				PassStack.Emplace(PassHandle);
			}
		}

		for (const FExtractedTexture& ExtractedTexture : ExtractedTextures)
		{
			FRDGTextureRef Texture = ExtractedTexture.Texture;
			for (auto& LastProducer : Texture->LastProducers)
			{
				FRDGProducerState StateFinal;
				StateFinal.Access = Texture->AccessFinal;
				StateFinal.PassHandle = EpiloguePassHandle;

				AddCullingDependency(LastProducer, StateFinal, ERHIPipeline::Graphics);
			}
		}

		for (const FExtractedBuffer& ExtractedBuffer : ExtractedBuffers)
		{
			FRDGBufferRef Buffer = ExtractedBuffer.Buffer;

			FRDGProducerState StateFinal;
			StateFinal.Access = Buffer->AccessFinal;
			StateFinal.PassHandle = EpiloguePassHandle;

			AddCullingDependency(Buffer->LastProducer, StateFinal, ERHIPipeline::Graphics);
		}
	}

	// All dependencies in the raw graph have been specified; if enabled, all passes are marked as culled and a
	// depth first search is employed to find reachable regions of the graph. Roots of the search are those passes
	// with outputs leaving the graph or those marked to never cull.

	if (bCullPasses)
	{
		SCOPED_NAMED_EVENT(PassCulling, FColor::Emerald);

		PassStack.Emplace(EpiloguePassHandle);

		// Mark the epilogue pass as culled so that it is traversed.
		EpiloguePass->bCulled = 1;

		// Manually mark the prologue passes as not culled.
		ProloguePass->bCulled = 0;

		while (PassStack.Num())
		{
			FRDGPass* Pass = Passes[PassStack.Pop()];

			if (Pass->bCulled)
			{
				Pass->bCulled = 0;
				PassStack.Append(Pass->Producers);
			}
		}
	}

	// Walk the culled graph and compile barriers for each subresource. Certain transitions are redundant; read-to-read, for example.
	// We can avoid them by traversing and merging compatible states together. The merging states removes a transition, but the merging
	// heuristic is conservative and choosing not to merge doesn't necessarily mean a transition is performed. They are two distinct steps.
	// Merged states track the first and last pass interval. Pass references are also accumulated onto each resource. This must happen
	// after culling since culled passes can't contribute references.

	{
		SCOPED_NAMED_EVENT(CompileBarriers, FColor::Emerald);

		for (FRDGPassHandle PassHandle = ProloguePassHandle + 1; PassHandle < EpiloguePassHandle; ++PassHandle)
		{
			FRDGPass* Pass = Passes[PassHandle];

			if (Pass->bCulled || Pass->bEmptyParameters)
			{
				continue;
			}

			const bool bAsyncComputePass = PassesOnAsyncCompute[PassHandle];

			const ERHIPipeline PassPipeline = Pass->Pipeline;

			const auto MergeSubresourceStates = [&](ERDGParentResourceType ResourceType, FRDGSubresourceState*& PassMergeState, FRDGSubresourceState*& ResourceMergeState, const FRDGSubresourceState& PassState)
			{
				if (!ResourceMergeState || !FRDGSubresourceState::IsMergeAllowed(ResourceType, *ResourceMergeState, PassState))
				{
					// Cross-pipeline, non-mergable state changes require a new pass dependency for fencing purposes.
					if (ResourceMergeState)
					{
						for (ERHIPipeline Pipeline : GetRHIPipelines())
						{
							if (Pipeline != PassPipeline && ResourceMergeState->LastPass[Pipeline].IsValid())
							{
								// Add a dependency from the other pipe to this pass to join back.
								AddPassDependency(ResourceMergeState->LastPass[Pipeline], PassHandle);
							}
						}
					}

					// Allocate a new pending merge state and assign it to the pass state.
					ResourceMergeState = AllocSubresource(PassState);
				}
				else
				{
					// Merge the pass state into the merged state.
					ResourceMergeState->Access |= PassState.Access;

					FRDGPassHandle& FirstPassHandle = ResourceMergeState->FirstPass[PassPipeline];

					if (FirstPassHandle.IsNull())
					{
						FirstPassHandle = PassHandle;
					}

					ResourceMergeState->LastPass[PassPipeline] = PassHandle;
				}

				PassMergeState = ResourceMergeState;
			};

			for (auto& PassState : Pass->TextureStates)
			{
				FRDGTextureRef Texture = PassState.Texture;
				Texture->ReferenceCount += PassState.ReferenceCount;
				Texture->bUsedByAsyncComputePass |= bAsyncComputePass;
				Texture->bCulled = false;

				if (Texture->bSwapChain && !Texture->bSwapChainAlreadyMoved)
				{
					Texture->bSwapChainAlreadyMoved = 1;
					Texture->FirstPass = PassHandle;
					GetWholeResource(Texture->GetState()).SetPass(ERHIPipeline::Graphics, PassHandle);
				}

			#if STATS
				GRDGStatTextureReferenceCount += PassState.ReferenceCount;
			#endif

				for (int32 Index = 0; Index < PassState.State.Num(); ++Index)
				{
					if (PassState.State[Index].Access == ERHIAccess::Unknown)
					{
						continue;
					}

					MergeSubresourceStates(ERDGParentResourceType::Texture, PassState.MergeState[Index], Texture->MergeState[Index], PassState.State[Index]);
				}
			}

			for (auto& PassState : Pass->BufferStates)
			{
				FRDGBufferRef Buffer = PassState.Buffer;
				Buffer->ReferenceCount += PassState.ReferenceCount;
				Buffer->bUsedByAsyncComputePass |= bAsyncComputePass;
				Buffer->bCulled = false;

			#if STATS
				GRDGStatBufferReferenceCount += PassState.ReferenceCount;
			#endif

				MergeSubresourceStates(ERDGParentResourceType::Buffer, PassState.MergeState, Buffer->MergeState, PassState.State);
			}
		}
	}

	// Traverses passes on the graphics pipe and merges raster passes with the same render targets into a single RHI render pass.
	if (IsRenderPassMergeEnabled() && RasterPassCount > 0)
	{
		SCOPED_NAMED_EVENT(MergeRenderPasses, FColor::Emerald);

		TArray<FRDGPassHandle, TInlineAllocator<32, FRDGArrayAllocator>> PassesToMerge;
		FRDGPass* PrevPass = nullptr;
		const FRenderTargetBindingSlots* PrevRenderTargets = nullptr;

		const auto CommitMerge = [&]
		{
			if (PassesToMerge.Num())
			{
				const auto SetEpilogueBarrierPass = [&](FRDGPass* Pass, FRDGPassHandle EpilogueBarrierPassHandle)
				{
					Pass->EpilogueBarrierPass = EpilogueBarrierPassHandle;
					Pass->ResourcesToEnd.Reset();
					Passes[EpilogueBarrierPassHandle]->ResourcesToEnd.Add(Pass);
				};

				const auto SetPrologueBarrierPass = [&](FRDGPass* Pass, FRDGPassHandle PrologueBarrierPassHandle)
				{
					Pass->PrologueBarrierPass = PrologueBarrierPassHandle;
					Pass->ResourcesToBegin.Reset();
					Passes[PrologueBarrierPassHandle]->ResourcesToBegin.Add(Pass);
				};

				const FRDGPassHandle FirstPassHandle = PassesToMerge[0];
				const FRDGPassHandle LastPassHandle = PassesToMerge.Last();
				Passes[FirstPassHandle]->ResourcesToBegin.Reserve(PassesToMerge.Num());
				Passes[LastPassHandle]->ResourcesToEnd.Reserve(PassesToMerge.Num());

				// Given an interval of passes to merge into a single render pass: [B, X, X, X, X, E]
				//
				// The begin pass (B) and end (E) passes will call {Begin, End}RenderPass, respectively. Also,
				// begin will handle all prologue barriers for the entire merged interval, and end will handle all
				// epilogue barriers. This avoids transitioning of resources within the render pass and batches the
				// transitions more efficiently. This assumes we have filtered out dependencies between passes from
				// the merge set, which is done during traversal.

				// (B) First pass in the merge sequence.
				{
					FRDGPass* Pass = Passes[FirstPassHandle];
					Pass->bSkipRenderPassEnd = 1;
					SetEpilogueBarrierPass(Pass, LastPassHandle);
				}

				// (X) Intermediate passes.
				for (int32 PassIndex = 1, PassCount = PassesToMerge.Num() - 1; PassIndex < PassCount; ++PassIndex)
				{
					const FRDGPassHandle PassHandle = PassesToMerge[PassIndex];
					FRDGPass* Pass = Passes[PassHandle];
					Pass->bSkipRenderPassBegin = 1;
					Pass->bSkipRenderPassEnd = 1;
					SetPrologueBarrierPass(Pass, FirstPassHandle);
					SetEpilogueBarrierPass(Pass, LastPassHandle);
				}

				// (E) Last pass in the merge sequence.
				{
					FRDGPass* Pass = Passes[LastPassHandle];
					Pass->bSkipRenderPassBegin = 1;
					SetPrologueBarrierPass(Pass, FirstPassHandle);
				}

#if STATS
				GRDGStatRenderPassMergeCount += PassesToMerge.Num();
#endif
			}
			PassesToMerge.Reset();
			PrevPass = nullptr;
			PrevRenderTargets = nullptr;
		};

		for (FRDGPassHandle PassHandle = ProloguePassHandle + 1; PassHandle < EpiloguePassHandle; ++PassHandle)
		{
			FRDGPass* NextPass = Passes[PassHandle];

			if (NextPass->bCulled || NextPass->bEmptyParameters)
			{
				continue;
			}

			if (EnumHasAnyFlags(NextPass->Flags, ERDGPassFlags::Raster))
			{
				// A pass where the user controls the render pass or it is forced to skip pass merging can't merge with other passes
				if (EnumHasAnyFlags(NextPass->Flags, ERDGPassFlags::SkipRenderPass | ERDGPassFlags::NeverMerge))
				{
					CommitMerge();
					continue;
				}

				// A pass which writes to resources outside of the render pass introduces new dependencies which break merging.
				if (!NextPass->bRenderPassOnlyWrites)
				{
					CommitMerge();
					continue;
				}

				const FRenderTargetBindingSlots& RenderTargets = NextPass->GetParameters().GetRenderTargets();

				if (PrevPass)
				{
					check(PrevRenderTargets);

					if (PrevRenderTargets->CanMergeBefore(RenderTargets)
#if WITH_MGPU
						&& PrevPass->GPUMask == NextPass->GPUMask
#endif
						)
					{
						if (!PassesToMerge.Num())
						{
							PassesToMerge.Add(PrevPass->GetHandle());
						}
						PassesToMerge.Add(PassHandle);
					}
					else
					{
						CommitMerge();
					}
				}

				PrevPass = NextPass;
				PrevRenderTargets = &RenderTargets;
			}
			else if (!EnumHasAnyFlags(NextPass->Flags, ERDGPassFlags::AsyncCompute))
			{
				// A non-raster pass on the graphics pipe will invalidate the render target merge.
				CommitMerge();
			}
		}

		CommitMerge();
	}

	if (AsyncComputePassCount > 0)
	{
		SCOPED_NAMED_EVENT(AsyncComputeFences, FColor::Emerald);

		// Traverse the active passes in execution order to find latest cross-pipeline producer and the earliest
		// cross-pipeline consumer for each pass. This helps narrow the search space later when building async
		// compute overlap regions.

		const auto IsCrossPipeline = [&](FRDGPassHandle A, FRDGPassHandle B)
		{
			return PassesOnAsyncCompute[A] != PassesOnAsyncCompute[B];
		};

		FRDGPassBitArray PassesWithCrossPipelineProducer(false, Passes.Num());
		FRDGPassBitArray PassesWithCrossPipelineConsumer(false, Passes.Num());

		for (FRDGPassHandle PassHandle = ProloguePassHandle + 1; PassHandle < EpiloguePassHandle; ++PassHandle)
		{
			FRDGPass* Pass = Passes[PassHandle];

			if (Pass->bCulled || Pass->bEmptyParameters)
			{
				continue;
			}

			for (FRDGPassHandle ProducerHandle : Pass->GetProducers())
			{
				const FRDGPassHandle ConsumerHandle = PassHandle;

				if (!IsCrossPipeline(ProducerHandle, ConsumerHandle))
				{
					continue;
				}

				FRDGPass* Consumer = Pass;
				FRDGPass* Producer = Passes[ProducerHandle];

				// Finds the earliest consumer on the other pipeline for the producer.
				if (Producer->CrossPipelineConsumer.IsNull() || ConsumerHandle < Producer->CrossPipelineConsumer)
				{
					Producer->CrossPipelineConsumer = PassHandle;
					PassesWithCrossPipelineConsumer[ProducerHandle] = true;
				}

				// Finds the latest producer on the other pipeline for the consumer.
				if (Consumer->CrossPipelineProducer.IsNull() || ProducerHandle > Consumer->CrossPipelineProducer)
				{
					Consumer->CrossPipelineProducer = ProducerHandle;
					PassesWithCrossPipelineProducer[ConsumerHandle] = true;
				}
			}
		}

		// Establishes fork / join overlap regions for async compute. This is used for fencing as well as resource
		// allocation / deallocation. Async compute passes can't allocate / release their resource references until
		// the fork / join is complete, since the two pipes run in parallel. Therefore, all resource lifetimes on
		// async compute are extended to cover the full async region.

		const auto IsCrossPipelineProducer = [&](FRDGPassHandle A)
		{
			return PassesWithCrossPipelineConsumer[A];
		};

		const auto IsCrossPipelineConsumer = [&](FRDGPassHandle A)
		{
			return PassesWithCrossPipelineProducer[A];
		};

		const auto FindCrossPipelineProducer = [&](FRDGPassHandle PassHandle)
		{
			FRDGPassHandle LatestProducerHandle = ProloguePassHandle;
			FRDGPassHandle ConsumerHandle = PassHandle;

			// We want to find the latest producer on the other pipeline in order to establish a fork point.
			// Since we could be consuming N resources with N producer passes, we only care about the last one.
			while (ConsumerHandle != ProloguePassHandle)
			{
				if (IsCrossPipelineConsumer(ConsumerHandle) && !IsCrossPipeline(ConsumerHandle, PassHandle))
				{
					const FRDGPass* Consumer = Passes[ConsumerHandle];

					if (Consumer->CrossPipelineProducer > LatestProducerHandle && !Consumer->bCulled)
					{
						LatestProducerHandle = Consumer->CrossPipelineProducer;
					}
				}
				--ConsumerHandle;
			}

			return LatestProducerHandle;
		};

		const auto FindCrossPipelineConsumer = [&](FRDGPassHandle PassHandle)
		{
			FRDGPassHandle EarliestConsumerHandle = EpiloguePassHandle;
			FRDGPassHandle ProducerHandle = PassHandle;

			// We want to find the earliest consumer on the other pipeline, as this establishes a join point
			// between the pipes. Since we could be producing for N consumers on the other pipeline, we only
			// care about the first one to execute.
			while (ProducerHandle != EpiloguePassHandle)
			{
				if (IsCrossPipelineProducer(ProducerHandle) && !IsCrossPipeline(ProducerHandle, PassHandle))
				{
					const FRDGPass* Producer = Passes[ProducerHandle];

					if (Producer->CrossPipelineConsumer < EarliestConsumerHandle && !Producer->bCulled)
					{
						EarliestConsumerHandle = Producer->CrossPipelineConsumer;
					}
				}
				++ProducerHandle;
			}

			return EarliestConsumerHandle;
		};

		const auto InsertGraphicsToAsyncComputeFork = [&](FRDGPass* GraphicsPass, FRDGPass* AsyncComputePass)
		{
			FRDGBarrierBatchBegin& EpilogueBarriersToBeginForAsyncCompute = GraphicsPass->GetEpilogueBarriersToBeginForAsyncCompute(Allocator, TransitionCreateQueue);

			GraphicsPass->bGraphicsFork = 1;
			EpilogueBarriersToBeginForAsyncCompute.SetUseCrossPipelineFence();

			AsyncComputePass->bAsyncComputeBegin = 1;
			AsyncComputePass->GetPrologueBarriersToEnd(Allocator).AddDependency(&EpilogueBarriersToBeginForAsyncCompute);
		};

		const auto InsertAsyncComputeToGraphicsJoin = [&](FRDGPass* AsyncComputePass, FRDGPass* GraphicsPass)
		{
			FRDGBarrierBatchBegin& EpilogueBarriersToBeginForGraphics = AsyncComputePass->GetEpilogueBarriersToBeginForGraphics(Allocator, TransitionCreateQueue);

			AsyncComputePass->bAsyncComputeEnd = 1;
			EpilogueBarriersToBeginForGraphics.SetUseCrossPipelineFence();

			GraphicsPass->bGraphicsJoin = 1;
			GraphicsPass->GetPrologueBarriersToEnd(Allocator).AddDependency(&EpilogueBarriersToBeginForGraphics);
		};

		const auto AddResourcesToBegin = [this](FRDGPass* PassToBegin, FRDGPass* PassWithResources)
		{
			Passes[PassToBegin->PrologueBarrierPass]->ResourcesToBegin.Add(PassWithResources);
		};

		const auto AddResourcesToEnd = [this](FRDGPass* PassToEnd, FRDGPass* PassWithResources)
		{
			Passes[PassToEnd->EpilogueBarrierPass]->ResourcesToEnd.Add(PassWithResources);
		};

		FRDGPassHandle CurrentGraphicsForkPassHandle;

		for (FRDGPassHandle PassHandle = ProloguePassHandle + 1; PassHandle < EpiloguePassHandle; ++PassHandle)
		{
			if (!PassesOnAsyncCompute[PassHandle])
			{
				continue;
			}

			FRDGPass* AsyncComputePass = Passes[PassHandle];

			if (AsyncComputePass->bCulled)
			{
				continue;
			}

			const FRDGPassHandle GraphicsForkPassHandle = FindCrossPipelineProducer(PassHandle);

			FRDGPass* GraphicsForkPass = Passes[GraphicsForkPassHandle];

			AsyncComputePass->GraphicsForkPass = GraphicsForkPassHandle;
			AddResourcesToBegin(GraphicsForkPass, AsyncComputePass);

			if (CurrentGraphicsForkPassHandle != GraphicsForkPassHandle)
			{
				CurrentGraphicsForkPassHandle = GraphicsForkPassHandle;
				InsertGraphicsToAsyncComputeFork(GraphicsForkPass, AsyncComputePass);
			}
		}

		FRDGPassHandle CurrentGraphicsJoinPassHandle;

		for (FRDGPassHandle PassHandle = EpiloguePassHandle - 1; PassHandle > ProloguePassHandle; --PassHandle)
		{
			if (!PassesOnAsyncCompute[PassHandle])
			{
				continue;
			}

			FRDGPass* AsyncComputePass = Passes[PassHandle];

			if (AsyncComputePass->bCulled)
			{
				continue;
			}

			const FRDGPassHandle GraphicsJoinPassHandle = FindCrossPipelineConsumer(PassHandle);

			FRDGPass* GraphicsJoinPass = Passes[GraphicsJoinPassHandle];

			AsyncComputePass->GraphicsJoinPass = GraphicsJoinPassHandle;
			GraphicsJoinPass->ResourcesToEnd.Add(AsyncComputePass);

			if (CurrentGraphicsJoinPassHandle != GraphicsJoinPassHandle)
			{
				CurrentGraphicsJoinPassHandle = GraphicsJoinPassHandle;
				InsertAsyncComputeToGraphicsJoin(AsyncComputePass, GraphicsJoinPass);
			}
		}
	}
}

void FRDGBuilder::Execute()
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RDG);
	SCOPED_NAMED_EVENT_TEXT("FRDGBuilder::Execute", FColor::Magenta);

	GRDGTransientResourceAllocator.ReleasePendingDeallocations();

	// Create the epilogue pass at the end of the graph just prior to compilation.
	{
		bInDebugPassScope = true;
		SetupEmptyPass(EpiloguePass = Passes.Allocate<FRDGSentinelPass>(Allocator, RDG_EVENT_NAME("Graph Epilogue")));
		bInDebugPassScope = false;
	}

	const FRDGPassHandle EpiloguePassHandle = GetEpiloguePassHandle();

	FGraphEventArray AsyncCompileEvents;

	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateExecuteBegin());
	IF_RDG_ENABLE_DEBUG(GRDGAllowRHIAccess = true);

	if (!IsImmediateMode())
	{
		BeginFlushResourcesRHI();

		SetupBufferUploads();

		Compile();

		IF_RDG_GPU_SCOPES(GPUScopeStacks.ReserveOps(Passes.Num()));
		IF_RDG_CPU_SCOPES(CPUScopeStacks.ReserveOps());

		if (bParallelExecuteEnabled)
		{
		#if RHI_WANT_BREADCRUMB_EVENTS
			RHICmdList.ExportBreadcrumbState(*BreadcrumbState);
		#endif

			// Parallel execute setup can be done off the render thread and synced prior to dispatch.
			AsyncCompileEvents.Emplace(FFunctionGraphTask::CreateAndDispatchWhenReady(
				[this](ENamedThreads::Type, const FGraphEventRef&)
			{
				SetupParallelExecute();

			}, TStatId(), nullptr, ENamedThreads::AnyHiPriThreadHiPriTask));
		}

		{
			SCOPE_CYCLE_COUNTER(STAT_RDG_CollectResourcesTime);
			CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RDG_CollectResources);
			SCOPED_NAMED_EVENT_TEXT("FRDGBuilder::CollectResources", FColor::Magenta);

			EnumerateExtendedLifetimeResources(Textures, [](FRDGTexture* Texture)
			{
				++Texture->ReferenceCount;
			});

			EnumerateExtendedLifetimeResources(Buffers, [](FRDGBuffer* Buffer)
			{
				++Buffer->ReferenceCount;
			});

			for (FRDGPassHandle PassHandle = Passes.Begin(); PassHandle < ProloguePassHandle; ++PassHandle)
			{
				FRDGPass* Pass = Passes[PassHandle];

				if (!Pass->bCulled)
				{
					EndResourcesRHI(Pass, ProloguePassHandle);
				}
			}

			for (FRDGPassHandle PassHandle = ProloguePassHandle; PassHandle <= EpiloguePassHandle; ++PassHandle)
			{
				FRDGPass* Pass = Passes[PassHandle];

				if (!Pass->bCulled)
				{
					BeginResourcesRHI(Pass, PassHandle);
					EndResourcesRHI(Pass, PassHandle);
				}
			}

			EnumerateExtendedLifetimeResources(Textures, [&](FRDGTextureRef Texture)
			{
				EndResourceRHI(EpiloguePassHandle, Texture, 1);
			});

			EnumerateExtendedLifetimeResources(Buffers, [&](FRDGBufferRef Buffer)
			{
				EndResourceRHI(EpiloguePassHandle, Buffer, 1);
			});

			if (TransientResourceAllocator)
			{
			#if RDG_ENABLE_TRACE
				TransientResourceAllocator->Flush(RHICmdList, Trace.IsEnabled() ? &Trace.TransientAllocationStats : nullptr);
			#else
				TransientResourceAllocator->Flush(RHICmdList);
			#endif
			}
		}

		{
			SCOPED_NAMED_EVENT_TEXT("FRDGBuilder::CollectBarriers", FColor::Magenta);
			SCOPE_CYCLE_COUNTER(STAT_RDG_CollectBarriersTime);
			CSV_SCOPED_TIMING_STAT_EXCLUSIVE_CONDITIONAL(RDG_CollectBarriers, GRDGVerboseCSVStats != 0);

			for (FRDGPassHandle PassHandle = ProloguePassHandle + 1; PassHandle < EpiloguePassHandle; ++PassHandle)
			{
				FRDGPass* Pass = Passes[PassHandle];

				if (!Pass->bCulled && !Pass->bEmptyParameters)
				{
					CollectPassBarriers(Pass, PassHandle);
				}
			}
		}
	}

	{
		SCOPED_NAMED_EVENT_TEXT("FRDGBuilder::Finalize", FColor::Magenta);

#if RDG_ENABLE_DEBUG
		const auto LogResource = [&](auto* Resource, auto& Registry)
		{
			if (!Resource->bCulled)
			{
				if (!Resource->bLastOwner)
				{
					auto* NextOwner = Registry[Resource->NextOwner];
					LogFile.AddAliasEdge(Resource, Resource->LastPass, NextOwner, NextOwner->FirstPass);
				}
				LogFile.AddFirstEdge(Resource, Resource->FirstPass);
			}
		};
#endif

		ActivePooledTextures.Reserve(Textures.Num());
		Textures.Enumerate([&](FRDGTextureRef Texture)
		{
			if (Texture->HasRHI())
			{
				AddEpilogueTransition(Texture);
				Texture->Finalize(ActivePooledTextures);

				IF_RDG_ENABLE_DEBUG(LogResource(Texture, Textures));
			}
		});

		ActivePooledBuffers.Reserve(Buffers.Num());
		Buffers.Enumerate([&](FRDGBufferRef Buffer)
		{
			if (Buffer->HasRHI())
			{
				AddEpilogueTransition(Buffer);
				Buffer->Finalize(ActivePooledBuffers);

				IF_RDG_ENABLE_DEBUG(LogResource(Buffer, Buffers));
			}
		});
	}

	if (bParallelExecuteEnabled)
	{
		// Overlap pass barrier creation with other compilation tasks, since it's not required to run on the render thread.
		AsyncCompileEvents.Emplace(FFunctionGraphTask::CreateAndDispatchWhenReady(
			[this](ENamedThreads::Type, const FGraphEventRef&)
		{
			CreatePassBarriers();

		}, TStatId(), nullptr, ENamedThreads::AnyHiPriThreadHiPriTask));
	}
	else
	{
		CreatePassBarriers();
	}

	SubmitBufferUploads();

	CreateUniformBuffers();

	EndFlushResourcesRHI();

	IF_RDG_ENABLE_TRACE(Trace.OutputGraphBegin());

	IF_RDG_ENABLE_DEBUG(GRDGAllowRHIAccess = bParallelExecuteEnabled);

	const ENamedThreads::Type RenderThread = ENamedThreads::GetRenderThread_Local();

	FGraphEventRef DispatchParallelExecuteEvent;

	if (!IsImmediateMode())
	{
		SCOPED_NAMED_EVENT_TEXT("FRDGBuilder::ExecutePasses", FColor::Magenta);
		SCOPE_CYCLE_COUNTER(STAT_RDG_ExecuteTime);
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderOther);

		// Wait on all async compilation tasks before executing any passes.
		if (!AsyncCompileEvents.IsEmpty())
		{
			FTaskGraphInterface::Get().WaitUntilTasksComplete(AsyncCompileEvents, RenderThread);
		}

		if (bParallelExecuteEnabled)
		{
			DispatchParallelExecuteEvent = FFunctionGraphTask::CreateAndDispatchWhenReady(
				[this, &RHICmdContext = RHICmdList.GetContext()](ENamedThreads::Type, const FGraphEventRef&)
			{
				DispatchParallelExecute(&RHICmdContext);

			}, TStatId(), nullptr, ENamedThreads::AnyHiPriThreadHiPriTask);
		}

		for (FRDGPassHandle PassHandle = ProloguePassHandle; PassHandle <= EpiloguePassHandle; ++PassHandle)
		{
			FRDGPass* Pass = Passes[PassHandle];

			if (Pass->bCulled)
			{
			#if STATS
				GRDGStatPassCullCount++;
			#endif

				continue;
			}

			if (bParallelExecuteEnabled)
			{
				if (Pass->bParallelExecute)
				{
				#if RDG_CPU_SCOPES // CPU scopes are replayed on the render thread prior to executing the entire batch.
					Pass->CPUScopeOps.Execute();
				#endif

					if (Pass->bParallelExecuteBegin)
					{
						FParallelPassSet& ParallelPassSet = ParallelPassSets[Pass->ParallelPassSetIndex];

						// Busy wait until our pass set is ready. This will be set by the dispatch task.
						while (!FPlatformAtomics::AtomicRead(&ParallelPassSet.bInitialized)) {};

						check(ParallelPassSet.Event != nullptr && ParallelPassSet.RHICmdList != nullptr);
						RHICmdList.QueueRenderThreadCommandListSubmit(ParallelPassSet.Event, ParallelPassSet.RHICmdList);

						IF_RHI_WANT_BREADCRUMB_EVENTS(RHICmdList.ImportBreadcrumbState(*ParallelPassSet.BreadcrumbStateEnd));

						if (ParallelPassSet.bDispatchAfterExecute && IsRunningRHIInSeparateThread())
						{
							RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
						}
					}

					continue;
				}
			}
			else if (!Pass->bSentinel)
			{
				CompilePassOps(Pass);
			}

			FRHIComputeCommandList& RHICmdListPass = Pass->Pipeline == ERHIPipeline::AsyncCompute
				? static_cast<FRHIComputeCommandList&>(RHICmdListAsyncCompute)
				: RHICmdList;

			ExecutePass(Pass, RHICmdListPass);
		}
	}
	else
	{
		ExecutePass(EpiloguePass, RHICmdList);
	}

	// Wait for the parallel dispatch task before attempting to wait on the execute event array (the former mutates the array).
	if (DispatchParallelExecuteEvent)
	{
		DispatchParallelExecuteEvent->Wait(RenderThread);
	}

	RHICmdList.SetStaticUniformBuffers({});

#if WITH_MGPU
	if (NameForTemporalEffect != NAME_None)
	{
		TArray<FRHITexture*> BroadcastTexturesForTemporalEffect;
		for (const FExtractedTexture& ExtractedTexture : ExtractedTextures)
		{
			if (EnumHasAnyFlags(ExtractedTexture.Texture->Flags, ERDGTextureFlags::MultiFrame))
			{
				BroadcastTexturesForTemporalEffect.Add(ExtractedTexture.Texture->GetRHIUnchecked());
			}
		}
		RHICmdList.BroadcastTemporalEffect(NameForTemporalEffect, BroadcastTexturesForTemporalEffect);
	}

	if (bForceCopyCrossGPU)
	{
		ForceCopyCrossGPU();
	}
#endif

	// Wait on the actual parallel execute tasks in the Execute call. When draining is okay to let them overlap with other graph setup.
	// This also needs to be done before extraction of external resources to be consistent with non-parallel rendering.
	if (!ParallelExecuteEvents.IsEmpty())
	{
		FTaskGraphInterface::Get().WaitUntilTasksComplete(ParallelExecuteEvents, RenderThread);
	}

	for (const FExtractedTexture& ExtractedTexture : ExtractedTextures)
	{
		check(ExtractedTexture.Texture->PooledRenderTarget);
		*ExtractedTexture.PooledTexture = ExtractedTexture.Texture->PooledRenderTarget;
	}

	for (const FExtractedBuffer& ExtractedBuffer : ExtractedBuffers)
	{
		check(ExtractedBuffer.Buffer->PooledBuffer);
		*ExtractedBuffer.PooledBuffer = ExtractedBuffer.Buffer->PooledBuffer;
	}

	IF_RDG_ENABLE_TRACE(Trace.OutputGraphEnd(*this));

	IF_RDG_GPU_SCOPES(GPUScopeStacks.Graphics.EndExecute(RHICmdList));
	IF_RDG_GPU_SCOPES(GPUScopeStacks.AsyncCompute.EndExecute(RHICmdListAsyncCompute));
	IF_RDG_CPU_SCOPES(CPUScopeStacks.EndExecute());

	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateExecuteEnd());
	IF_RDG_ENABLE_DEBUG(LogFile.End());
	IF_RDG_ENABLE_DEBUG(GRDGAllowRHIAccess = false);

#if STATS
	GRDGStatBufferCount += Buffers.Num();
	GRDGStatTextureCount += Textures.Num();
	GRDGStatViewCount += Views.Num();
	GRDGStatMemoryWatermark = FMath::Max(GRDGStatMemoryWatermark, Allocator.GetByteCount());
#endif

	RasterPassCount = 0;
	AsyncComputePassCount = 0;

	// Flush any outstanding async compute commands at the end to get things moving down the pipe.
	if (RHICmdListAsyncCompute.HasCommands())
	{
		FRHIAsyncComputeCommandListImmediate::ImmediateDispatch(RHICmdListAsyncCompute);
	}
}

FRDGPass* FRDGBuilder::SetupPass(FRDGPass* Pass)
{
	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateAddPass(Pass, bInDebugPassScope));
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE_CONDITIONAL(RDGBuilder_SetupPass, GRDGVerboseCSVStats != 0);

	const FRDGParameterStruct PassParameters = Pass->GetParameters();
	const FRDGPassHandle PassHandle = Pass->Handle;
	const ERDGPassFlags PassFlags = Pass->Flags;
	const ERHIPipeline PassPipeline = Pass->Pipeline;

	bool bRenderPassOnlyWrites = true;

	const auto TryAddView = [&](FRDGViewRef View)
	{
		if (View && View->LastPass != PassHandle)
		{
			View->LastPass = PassHandle;
			Pass->Views.Add(View->Handle);
		}
	};

	Pass->Views.Reserve(PassParameters.GetBufferParameterCount() + PassParameters.GetTextureParameterCount());
	Pass->TextureStates.Reserve(PassParameters.GetTextureParameterCount() + (PassParameters.HasRenderTargets() ? (MaxSimultaneousRenderTargets + 1) : 0));
	EnumerateTextureAccess(PassParameters, PassFlags, [&](FRDGViewRef TextureView, FRDGTextureRef Texture, ERHIAccess Access, ERDGTextureAccessFlags AccessFlags, FRDGTextureSubresourceRange Range)
	{
		TryAddView(TextureView);

		if (Texture->bFinalizedAccess)
		{
			// Finalized resources expected to remain in the same state, so are ignored by the graph.
			// As only External | Extracted resources can be finalized by the user, the graph doesn't
			// need to track them any more for culling / transition purposes. Validation checks that these
			// invariants are true.
			IF_RDG_ENABLE_DEBUG(UserValidation.ValidateFinalizedAccess(Texture, Access, Pass));
			return;
		}

		const FRDGViewHandle NoUAVBarrierHandle = GetHandleIfNoUAVBarrier(TextureView);
		const EResourceTransitionFlags TransitionFlags = GetTextureViewTransitionFlags(TextureView, Texture);

		FRDGPass::FTextureState* PassState;

		if (Texture->LastPass != PassHandle)
		{
			Texture->LastPass = PassHandle;
			Texture->PassStateIndex = static_cast<uint16>(Pass->TextureStates.Num());

			PassState = &Pass->TextureStates.Emplace_GetRef(Texture);
		}
		else
		{
			PassState = &Pass->TextureStates[Texture->PassStateIndex];
		}

		PassState->ReferenceCount++;

		const auto AddSubresourceAccess = [&](FRDGSubresourceState& State)
		{
			State.Access = MakeValidAccess(State.Access | Access);
			State.Flags |= TransitionFlags;
			State.NoUAVBarrierFilter.AddHandle(NoUAVBarrierHandle);
			State.SetPass(PassPipeline, PassHandle);
		};

		if (IsWholeResource(PassState->State))
		{
			AddSubresourceAccess(GetWholeResource(PassState->State));
		}
		else
		{
			EnumerateSubresourceRange(PassState->State, Texture->Layout, Range, AddSubresourceAccess);
		}

		const bool bWritableAccess = IsWritableAccess(Access);
		bRenderPassOnlyWrites &= (!bWritableAccess || EnumHasAnyFlags(AccessFlags, ERDGTextureAccessFlags::RenderTarget));
		Texture->bProduced |= bWritableAccess;
	});

	Pass->BufferStates.Reserve(PassParameters.GetBufferParameterCount());
	EnumerateBufferAccess(PassParameters, PassFlags, [&](FRDGViewRef BufferView, FRDGBufferRef Buffer, ERHIAccess Access)
	{
		TryAddView(BufferView);

		if (Buffer->bFinalizedAccess)
		{
			// Finalized resources expected to remain in the same state, so are ignored by the graph.
			// As only External | Extracted resources can be finalized by the user, the graph doesn't
			// need to track them any more for culling / transition purposes. Validation checks that these
			// invariants are true.
			IF_RDG_ENABLE_DEBUG(UserValidation.ValidateFinalizedAccess(Buffer, Access, Pass));
			return;
		}

		const FRDGViewHandle NoUAVBarrierHandle = GetHandleIfNoUAVBarrier(BufferView);

		FRDGPass::FBufferState* PassState;

		if (Buffer->LastPass != PassHandle)
		{
			Buffer->LastPass = PassHandle;
			Buffer->PassStateIndex = Pass->BufferStates.Num();

			PassState = &Pass->BufferStates.Emplace_GetRef(Buffer);
		}
		else
		{
			PassState = &Pass->BufferStates[Buffer->PassStateIndex];
		}

		PassState->ReferenceCount++;
		PassState->State.Access = MakeValidAccess(PassState->State.Access | Access);
		PassState->State.NoUAVBarrierFilter.AddHandle(NoUAVBarrierHandle);
		PassState->State.SetPass(PassPipeline, PassHandle);

		const bool bWritableAccess = IsWritableAccess(Access);
		bRenderPassOnlyWrites &= !bWritableAccess;
		Buffer->bProduced |= bWritableAccess;
	});

	Pass->UniformBuffers.Reserve(PassParameters.GetUniformBufferParameterCount());
	PassParameters.EnumerateUniformBuffers([&](FRDGUniformBufferBinding UniformBuffer)
	{
		Pass->UniformBuffers.Emplace(UniformBuffer.GetUniformBuffer()->Handle);
	});

	Pass->bRenderPassOnlyWrites = bRenderPassOnlyWrites;
	Pass->bHasExternalOutputs = PassParameters.HasExternalOutputs();

	const bool bEmptyParameters = !Pass->TextureStates.Num() && !Pass->BufferStates.Num();
	SetupPassInternal(Pass, PassHandle, PassPipeline, bEmptyParameters);
	return Pass;
}

FRDGPass* FRDGBuilder::SetupEmptyPass(FRDGPass* Pass)
{
	const bool bEmptyParameters = true;
	SetupPassInternal(Pass, Pass->Handle, Pass->Pipeline, bEmptyParameters);
	return Pass;
}

void FRDGBuilder::CompilePassOps(FRDGPass* Pass)
{
#if WITH_MGPU
	if (!bWaitedForTemporalEffect && NameForTemporalEffect != NAME_None && Pass->Pipeline == ERHIPipeline::Graphics)
	{
		bWaitedForTemporalEffect = true;
		Pass->bWaitForTemporalEffect = 1;
	}

	FRHIGPUMask GPUMask = Pass->GPUMask;
#else
	FRHIGPUMask GPUMask = FRHIGPUMask::All();
#endif

#if RDG_CMDLIST_STATS
	if (CommandListStatState != Pass->CommandListStat && !Pass->bSentinel)
	{
		CommandListStatState = Pass->CommandListStat;
		Pass->bSetCommandListStat = 1;
	}
#endif

#if RDG_CPU_SCOPES
	Pass->CPUScopeOps = CPUScopeStacks.CompilePassPrologue(Pass);
#endif

#if RDG_GPU_SCOPES
	Pass->GPUScopeOpsPrologue = GPUScopeStacks.CompilePassPrologue(Pass, GPUMask);
	Pass->GPUScopeOpsEpilogue = GPUScopeStacks.CompilePassEpilogue(Pass);
#endif
}

void FRDGBuilder::SetupPassInternal(FRDGPass* Pass, FRDGPassHandle PassHandle, ERHIPipeline PassPipeline, bool bEmptyParameters)
{
	check(Pass->Handle == PassHandle);
	check(Pass->Pipeline == PassPipeline);

	Pass->bEmptyParameters = bEmptyParameters;
	Pass->bDispatchAfterExecute = bDispatchHint;
	Pass->GraphicsForkPass = PassHandle;
	Pass->GraphicsJoinPass = PassHandle;
	Pass->PrologueBarrierPass = PassHandle;
	Pass->EpilogueBarrierPass = PassHandle;

	bDispatchHint = false;

	if (!EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::AsyncCompute))
	{
		Pass->ResourcesToBegin.Add(Pass);
		Pass->ResourcesToEnd.Add(Pass);
	}

	AsyncComputePassCount += EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::AsyncCompute) ? 1 : 0;
	RasterPassCount       += EnumHasAnyFlags(Pass->Flags, ERDGPassFlags::Raster)       ? 1 : 0;

#if WITH_MGPU
	Pass->GPUMask = RHICmdList.GetGPUMask();
#endif

#if STATS
	Pass->CommandListStat = CommandListStatScope;

	GRDGStatPassCount++;
	GRDGStatPassWithParameterCount += !bEmptyParameters ? 1 : 0;
#endif

	IF_RDG_CPU_SCOPES(Pass->CPUScopes = CPUScopeStacks.GetCurrentScopes());
	IF_RDG_GPU_SCOPES(Pass->GPUScopes = GPUScopeStacks.GetCurrentScopes(PassPipeline));

#if RDG_GPU_SCOPES && RDG_ENABLE_TRACE
	Pass->TraceEventScope = GPUScopeStacks.GetCurrentScopes(ERHIPipeline::Graphics).Event;
#endif

#if RDG_GPU_SCOPES && RDG_ENABLE_DEBUG
	if (const FRDGEventScope* Scope = Pass->GPUScopes.Event)
	{
		Pass->FullPathIfDebug = Scope->GetPath(Pass->Name);
	}
#endif

	if (IsImmediateMode() && !Pass->bSentinel)
	{
		SCOPED_NAMED_EVENT(FRDGBuilder_ExecutePass, FColor::Emerald);
		RDG_ALLOW_RHI_ACCESS_SCOPE();

		// Trivially redirect the merge states to the pass states, since we won't be compiling the graph.
		for (auto& PassState : Pass->TextureStates)
		{
			const uint32 SubresourceCount = PassState.State.Num();
			PassState.MergeState.SetNum(SubresourceCount);
			for (uint32 Index = 0; Index < SubresourceCount; ++Index)
			{
				if (PassState.State[Index].Access != ERHIAccess::Unknown)
				{
					PassState.MergeState[Index] = &PassState.State[Index];
				}
			}

			PassState.Texture->bCulled = false;
		}

		for (auto& PassState : Pass->BufferStates)
		{
			PassState.MergeState = &PassState.State;

			PassState.Buffer->bCulled = false;
		}

		check(!EnumHasAnyFlags(PassPipeline, ERHIPipeline::AsyncCompute));

		SetupBufferUploads();
		SubmitBufferUploads();
		CompilePassOps(Pass);
		BeginResourcesRHI(Pass, PassHandle);
		CollectPassBarriers(Pass, PassHandle);
		CreatePassBarriers();
		CreateUniformBuffers();
		ExecutePass(Pass, RHICmdList);
	}

	IF_RDG_ENABLE_DEBUG(VisualizePassOutputs(Pass));

	#if RDG_DUMP_RESOURCES
		DumpResourcePassOutputs( Pass);
	#endif
}

void FRDGBuilder::SetupBufferUploads()
{
	SCOPED_NAMED_EVENT_TEXT("FRDGBuilder::PrepareBufferUploads", FColor::Magenta);

	for (FUploadedBuffer& UploadedBuffer : UploadedBuffers)
	{
		if (UploadedBuffer.bUseDataCallbacks)
		{
			UploadedBuffer.Data = UploadedBuffer.DataCallback();
			UploadedBuffer.DataSize = UploadedBuffer.DataSizeCallback();
		}

		if (UploadedBuffer.Data && UploadedBuffer.DataSize)
		{
			ConvertToExternalBuffer(UploadedBuffer.Buffer);
			check(UploadedBuffer.DataSize <= UploadedBuffer.Buffer->Desc.GetTotalNumBytes());
		}
	}
}

void FRDGBuilder::SubmitBufferUploads()
{
	SCOPED_NAMED_EVENT_TEXT("FRDGBuilder::SubmitBufferUploads", FColor::Magenta);

	for (const FUploadedBuffer& UploadedBuffer : UploadedBuffers)
	{
		if (UploadedBuffer.Data && UploadedBuffer.DataSize)
		{
#if PLATFORM_NEEDS_GPU_UAV_RESOURCE_INIT_WORKAROUND
			if (UploadedBuffer.Buffer->bUAVAccessed)
			{
				FRHIResourceCreateInfo CreateInfo(UploadedBuffer.Buffer->Name);
				FBufferRHIRef TempBuffer = RHICreateVertexBuffer(UploadedBuffer.DataSize, BUF_Static | BUF_ShaderResource, CreateInfo);
				void* DestPtr = RHICmdList.LockBuffer(TempBuffer, 0, UploadedBuffer.DataSize, RLM_WriteOnly);
				FMemory::Memcpy(DestPtr, UploadedBuffer.Data, UploadedBuffer.DataSize);
				RHICmdList.UnlockBuffer(TempBuffer);
				RHICmdList.Transition(
				{
					FRHITransitionInfo(TempBuffer, ERHIAccess::Unknown, ERHIAccess::CopySrc | ERHIAccess::SRVMask),
					FRHITransitionInfo(UploadedBuffer.Buffer->GetRHI(), ERHIAccess::Unknown, ERHIAccess::CopyDest)
				});
				RHICmdList.CopyBufferRegion(UploadedBuffer.Buffer->GetRHI(), 0, TempBuffer, 0, UploadedBuffer.DataSize);
			}
			else
#endif
			{
				void* DestPtr = RHICmdList.LockBuffer(UploadedBuffer.Buffer->GetRHI(), 0, UploadedBuffer.DataSize, RLM_WriteOnly);
				FMemory::Memcpy(DestPtr, UploadedBuffer.Data, UploadedBuffer.DataSize);
				RHICmdList.UnlockBuffer(UploadedBuffer.Buffer->GetRHI());
			}

			if (UploadedBuffer.bUseFreeCallbacks)
			{
				UploadedBuffer.DataFreeCallback(UploadedBuffer.Data);
			}
		}
	}
	UploadedBuffers.Reset();
}

void FRDGBuilder::SetupParallelExecute()
{
	SCOPED_NAMED_EVENT(SetupParallelExecute, FColor::Emerald);
	FTaskTagScope Scope(ETaskTag::EParallelRenderingThread);

	TArray<FRDGPass*, TInlineAllocator<64, FRDGArrayAllocator>> ParallelPassCandidates;
	int32 MergedRenderPassCandidates = 0;
	bool bDispatchAfterExecute = false;

	const auto FlushParallelPassCandidates = [&]()
	{
		if (ParallelPassCandidates.IsEmpty())
		{
			return;
		}

		int32 PassBeginIndex = 0;
		int32 PassEndIndex = ParallelPassCandidates.Num();

		// It's possible that the first pass is inside a merged RHI render pass region. If so, we must push it forward until after the render pass ends.
		if (const FRDGPass* FirstPass = ParallelPassCandidates[PassBeginIndex]; FirstPass->PrologueBarrierPass < FirstPass->Handle)
		{
			const FRDGPass* EpilogueBarrierPass = Passes[FirstPass->EpilogueBarrierPass];

			for (; PassBeginIndex < ParallelPassCandidates.Num(); ++PassBeginIndex)
			{
				if (ParallelPassCandidates[PassBeginIndex] == EpilogueBarrierPass)
				{
					++PassBeginIndex;
					break;
				}
			}
		}

		if (PassBeginIndex < PassEndIndex)
		{
			// It's possible that the last pass is inside a merged RHI render pass region. If so, we must push it backwards until after the render pass begins.
			if (FRDGPass* LastPass = ParallelPassCandidates.Last(); LastPass->EpilogueBarrierPass > LastPass->Handle)
			{
				FRDGPass* PrologueBarrierPass = Passes[LastPass->PrologueBarrierPass];

				while (PassEndIndex > PassBeginIndex)
				{
					if (ParallelPassCandidates[--PassEndIndex] == PrologueBarrierPass)
					{
						break;
					}
				}
			}
		}

		const int32 ParallelPassCandidateCount = PassEndIndex - PassBeginIndex;

		if (ParallelPassCandidateCount >= GRDGParallelExecutePassMin)
		{
			FRDGPass* PassBegin = ParallelPassCandidates[PassBeginIndex];
			PassBegin->bParallelExecuteBegin = 1;
			PassBegin->ParallelPassSetIndex = ParallelPassSets.Num();

			FRDGPass* PassEnd = ParallelPassCandidates[PassEndIndex - 1];
			PassEnd->bParallelExecuteEnd = 1;
			PassEnd->ParallelPassSetIndex = ParallelPassSets.Num();

			for (int32 PassIndex = PassBeginIndex; PassIndex < PassEndIndex; ++PassIndex)
			{
				ParallelPassCandidates[PassIndex]->bParallelExecute = 1;
			}

			FParallelPassSet& ParallelPassSet = ParallelPassSets.Emplace_GetRef();
			ParallelPassSet.Passes.Append(ParallelPassCandidates.GetData() + PassBeginIndex, ParallelPassCandidateCount);
			ParallelPassSet.bDispatchAfterExecute = bDispatchAfterExecute;
		}

		ParallelPassCandidates.Reset();
		MergedRenderPassCandidates = 0;
		bDispatchAfterExecute = false;
	};

	ParallelPassSets.Reserve(32);
	ParallelPassCandidates.Emplace(ProloguePass);

	for (FRDGPassHandle PassHandle = GetProloguePassHandle() + 1; PassHandle < GetEpiloguePassHandle(); ++PassHandle)
	{
		FRDGPass* Pass = Passes[PassHandle];

		if (Pass->bCulled)
		{
			continue;
		}

		CompilePassOps(Pass);

		if (Pass->Pipeline == ERHIPipeline::AsyncCompute)
		{
			if (Pass->bAsyncComputeEnd)
			{
				FlushParallelPassCandidates();
			}

			continue;
		}

		if (!Pass->bParallelExecuteAllowed)
		{
			FlushParallelPassCandidates();
			continue;
		}

		ParallelPassCandidates.Emplace(Pass);
		bDispatchAfterExecute |= Pass->bDispatchAfterExecute;

		// Don't count merged render passes for the maximum pass threshold. This avoids the case where
		// a large merged render pass span could end up forcing it back onto the render thread, since
		// it's not possible to launch a task for a subset of passes within a merged render pass.
		MergedRenderPassCandidates += Pass->bSkipRenderPassBegin | Pass->bSkipRenderPassEnd;

		if (ParallelPassCandidates.Num() - MergedRenderPassCandidates >= GRDGParallelExecutePassMax)
		{
			FlushParallelPassCandidates();
		}
	}

	ParallelPassCandidates.Emplace(EpiloguePass);
	FlushParallelPassCandidates();

#if RHI_WANT_BREADCRUMB_EVENTS
	SCOPED_NAMED_EVENT(BreadcrumbSetup, FColor::Emerald);

	for (FRDGPassHandle PassHandle = GetProloguePassHandle(); PassHandle <= GetEpiloguePassHandle(); ++PassHandle)
	{
		FRDGPass* Pass = Passes[PassHandle];

		if (Pass->bCulled)
		{
			continue;
		}

		if (Pass->bParallelExecuteBegin)
		{
			FParallelPassSet& ParallelPassSet = ParallelPassSets[Pass->ParallelPassSetIndex];
			ParallelPassSet.BreadcrumbStateBegin = BreadcrumbState->Copy(Allocator);
			ParallelPassSet.BreadcrumbStateEnd = ParallelPassSet.BreadcrumbStateBegin;
		}

		Pass->GPUScopeOpsPrologue.Event.Execute(*BreadcrumbState);
		Pass->GPUScopeOpsEpilogue.Event.Execute(*BreadcrumbState);

		if (Pass->bParallelExecuteEnd)
		{
			FParallelPassSet& ParallelPassSet = ParallelPassSets[Pass->ParallelPassSetIndex];

			if (ParallelPassSet.BreadcrumbStateEnd->Version != BreadcrumbState->Version)
			{
				ParallelPassSet.BreadcrumbStateEnd = BreadcrumbState->Copy(Allocator);
			}
		}
	}
#endif
}

void FRDGBuilder::DispatchParallelExecute(IRHICommandContext* RHICmdContext)
{
	SCOPED_NAMED_EVENT(DispatchParallelExecute, FColor::Emerald);
	ParallelExecuteEvents.Reserve(ParallelExecuteEvents.Num() + ParallelPassSets.Num());

	for (FParallelPassSet& ParallelPassSet : ParallelPassSets)
	{
		ParallelPassSet.RHICmdList = new FRHICommandList(FRHIGPUMask::All());
		ParallelPassSet.RHICmdList->SetContext(RHICmdContext);

		IF_RHI_WANT_BREADCRUMB_EVENTS(ParallelPassSet.RHICmdList->ImportBreadcrumbState(*ParallelPassSet.BreadcrumbStateBegin));

		// Avoid referencing the parallel pass struct directly in the task, as the set can resize.
		TArrayView<FRDGPass*> ParallelPasses = ParallelPassSet.Passes;

		ParallelPassSet.Event = FFunctionGraphTask::CreateAndDispatchWhenReady(
			[this, ParallelPasses, &RHICmdListPass = *ParallelPassSet.RHICmdList](ENamedThreads::Type, const FGraphEventRef& MyCompletionGraphEvent)
		{
			SCOPED_NAMED_EVENT(ParallelExecute, FColor::Emerald);
			FTaskTagScope Scope(ETaskTag::EParallelRenderingThread);
			FMemMark MemMark(FMemStack::Get());

			for (FRDGPass* Pass : ParallelPasses)
			{
				ExecutePass(Pass, RHICmdListPass);
			}

			RHICmdListPass.HandleRTThreadTaskCompletion(MyCompletionGraphEvent);

		}, TStatId(), nullptr, ENamedThreads::AnyHiPriThreadHiPriTask);

		// Mark this set as initialized so that it can be submitted.
		FPlatformAtomics::AtomicStore(&ParallelPassSet.bInitialized, 1);

		// Enqueue the event to be synced at the end of RDG execution.
		ParallelExecuteEvents.Emplace(ParallelPassSet.Event);
	}
}

void FRDGBuilder::CreateUniformBuffers()
{
	SCOPED_NAMED_EVENT_TEXT("FRDGBuilder::CreateUniformBuffers", FColor::Magenta);

	for (FRDGUniformBufferHandle UniformBufferHandle : UniformBuffersToCreate)
	{
		UniformBuffers[UniformBufferHandle]->InitRHI();
	}
	UniformBuffersToCreate.Reset();
}

void FRDGBuilder::AddProloguePass()
{
	bInDebugPassScope = true;
	ProloguePass = SetupEmptyPass(Passes.Allocate<FRDGSentinelPass>(Allocator, RDG_EVENT_NAME("Graph Prologue (Graphics)")));
	ProloguePassHandle = ProloguePass->Handle;
	bInDebugPassScope = false;
}

void FRDGBuilder::ExecutePassPrologue(FRHIComputeCommandList& RHICmdListPass, FRDGPass* Pass)
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE_CONDITIONAL(RDGBuilder_ExecutePassPrologue, GRDGVerboseCSVStats != 0);

	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateExecutePassBegin(Pass));

#if RDG_CMDLIST_STATS
	if (Pass->bSetCommandListStat)
	{
		RHICmdListPass.SetCurrentStat(Pass->CommandListStat);
	}
#endif

	const ERDGPassFlags PassFlags = Pass->Flags;
	const ERHIPipeline PassPipeline = Pass->Pipeline;

	if (Pass->PrologueBarriersToBegin)
	{
		IF_RDG_ENABLE_DEBUG(BarrierValidation.ValidateBarrierBatchBegin(Pass, *Pass->PrologueBarriersToBegin));
		Pass->PrologueBarriersToBegin->Submit(RHICmdListPass, PassPipeline);
	}

	IF_RDG_ENABLE_DEBUG(BarrierValidation.ValidateBarrierBatchEnd(Pass, Pass->PrologueBarriersToEnd));
	Pass->PrologueBarriersToEnd.Submit(RHICmdListPass, PassPipeline);

	if (PassPipeline == ERHIPipeline::AsyncCompute && !Pass->bSentinel && AsyncComputeBudgetState != Pass->AsyncComputeBudget)
	{
		AsyncComputeBudgetState = Pass->AsyncComputeBudget;
		RHICmdListPass.SetAsyncComputeBudget(Pass->AsyncComputeBudget);
	}

	if (EnumHasAnyFlags(PassFlags, ERDGPassFlags::Raster))
	{
		if (!EnumHasAnyFlags(PassFlags, ERDGPassFlags::SkipRenderPass) && !Pass->SkipRenderPassBegin())
		{
			static_cast<FRHICommandList&>(RHICmdListPass).BeginRenderPass(Pass->GetParameters().GetRenderPassInfo(), Pass->GetName());
		}
	}

	BeginUAVOverlap(Pass, RHICmdListPass);
}

void FRDGBuilder::ExecutePassEpilogue(FRHIComputeCommandList& RHICmdListPass, FRDGPass* Pass)
{
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE_CONDITIONAL(RDGBuilder_ExecutePassEpilogue, GRDGVerboseCSVStats != 0);

	EndUAVOverlap(Pass, RHICmdListPass);

	const ERDGPassFlags PassFlags = Pass->Flags;
	const ERHIPipeline PassPipeline = Pass->Pipeline;
	const FRDGParameterStruct PassParameters = Pass->GetParameters();

	if (EnumHasAnyFlags(PassFlags, ERDGPassFlags::Raster) && !EnumHasAnyFlags(PassFlags, ERDGPassFlags::SkipRenderPass) && !Pass->SkipRenderPassEnd())
	{
		static_cast<FRHICommandList&>(RHICmdListPass).EndRenderPass();
	}

	FRDGTransitionQueue Transitions;

	IF_RDG_ENABLE_DEBUG(BarrierValidation.ValidateBarrierBatchBegin(Pass, Pass->EpilogueBarriersToBeginForGraphics));
	Pass->EpilogueBarriersToBeginForGraphics.Submit(RHICmdListPass, PassPipeline, Transitions);

	if (Pass->EpilogueBarriersToBeginForAsyncCompute)
	{
		IF_RDG_ENABLE_DEBUG(BarrierValidation.ValidateBarrierBatchBegin(Pass, *Pass->EpilogueBarriersToBeginForAsyncCompute));
		Pass->EpilogueBarriersToBeginForAsyncCompute->Submit(RHICmdListPass, PassPipeline, Transitions);
	}

	if (Pass->EpilogueBarriersToBeginForAll)
	{
		IF_RDG_ENABLE_DEBUG(BarrierValidation.ValidateBarrierBatchBegin(Pass, *Pass->EpilogueBarriersToBeginForAll));
		Pass->EpilogueBarriersToBeginForAll->Submit(RHICmdListPass, PassPipeline, Transitions);
	}

	for (FRDGBarrierBatchBegin* BarriersToBegin : Pass->SharedEpilogueBarriersToBegin)
	{
		IF_RDG_ENABLE_DEBUG(BarrierValidation.ValidateBarrierBatchBegin(Pass, *BarriersToBegin));
		BarriersToBegin->Submit(RHICmdListPass, PassPipeline, Transitions);
	}

	if (!Transitions.IsEmpty())
	{
		RHICmdListPass.BeginTransitions(Transitions);
	}

	if (Pass->EpilogueBarriersToEnd)
	{
		IF_RDG_ENABLE_DEBUG(BarrierValidation.ValidateBarrierBatchEnd(Pass, *Pass->EpilogueBarriersToEnd));
		Pass->EpilogueBarriersToEnd->Submit(RHICmdListPass, PassPipeline);
	}

	IF_RDG_ENABLE_DEBUG(UserValidation.ValidateExecutePassEnd(Pass));
}

void FRDGBuilder::ExecutePass(FRDGPass* Pass, FRHIComputeCommandList& RHICmdListPass)
{
#if RDG_EVENTS != RDG_EVENTS_NONE
	SCOPED_NAMED_EVENT_TCHAR(Pass->GetName(), FColor::Magenta);
#endif

	// Note that we must do this before doing anything with RHICmdList for the pass.
	// For example, if this pass only executes on GPU 1 we want to avoid adding a
	// 0-duration event for this pass on GPU 0's time line.
	SCOPED_GPU_MASK(RHICmdListPass, Pass->GPUMask);

#if RDG_CPU_SCOPES
	if (!Pass->bParallelExecute)
	{
		Pass->CPUScopeOps.Execute();
	}
#endif

	IF_RDG_ENABLE_DEBUG(ConditionalDebugBreak(RDG_BREAKPOINT_PASS_EXECUTE, BuilderName.GetTCHAR(), Pass->GetName()));

#if WITH_MGPU
	if (Pass->bWaitForTemporalEffect)
	{
		static_cast<FRHICommandList&>(RHICmdListPass).WaitForTemporalEffect(NameForTemporalEffect);
	}
#endif

	ExecutePassPrologue(RHICmdListPass, Pass);

#if RDG_GPU_SCOPES
	Pass->GPUScopeOpsPrologue.Execute(RHICmdListPass);
#endif

#if RDG_DUMP_RESOURCES_AT_EACH_DRAW
	BeginPassDump(Pass);
#endif

	Pass->Execute(RHICmdListPass);

#if RDG_DUMP_RESOURCES_AT_EACH_DRAW
	EndPassDump(Pass);
#endif

#if RDG_GPU_SCOPES
	Pass->GPUScopeOpsEpilogue.Execute(RHICmdListPass);
#endif

	ExecutePassEpilogue(RHICmdListPass, Pass);

	if (Pass->bAsyncComputeEnd)
	{
		RHICmdListAsyncCompute.SetStaticUniformBuffers({});
		FRHIAsyncComputeCommandListImmediate::ImmediateDispatch(RHICmdListAsyncCompute);
	}

	if (!Pass->bParallelExecute && Pass->bDispatchAfterExecute && IsRunningRHIInSeparateThread())
	{
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
	}

	if (!bParallelExecuteEnabled)
	{
		if (GRDGDebugFlushGPU && !GRDGAsyncCompute)
		{
			RHICmdList.SubmitCommandsAndFlushGPU();
			RHICmdList.BlockUntilGPUIdle();
		}
	}
}

void FRDGBuilder::BeginResourcesRHI(FRDGPass* ResourcePass, FRDGPassHandle ExecutePassHandle)
{
	for (FRDGPass* PassToBegin : ResourcePass->ResourcesToBegin)
	{
		for (const auto& PassState : PassToBegin->TextureStates)
		{
			BeginResourceRHI(ExecutePassHandle, PassState.Texture);
		}

		for (const auto& PassState : PassToBegin->BufferStates)
		{
			BeginResourceRHI(ExecutePassHandle, PassState.Buffer);
		}

		for (FRDGUniformBufferHandle UniformBufferHandle : PassToBegin->UniformBuffers)
		{
			if (FRDGUniformBuffer* UniformBuffer = UniformBuffers[UniformBufferHandle]; !UniformBuffer->bQueuedForCreate)
			{
				UniformBuffer->bQueuedForCreate = true;
				UniformBuffersToCreate.Add(UniformBufferHandle);
			}
		}

		for (FRDGViewHandle ViewHandle : PassToBegin->Views)
		{
			BeginResourceRHI(ExecutePassHandle, Views[ViewHandle]);
		}
	}
}

void FRDGBuilder::EndResourcesRHI(FRDGPass* ResourcePass, FRDGPassHandle ExecutePassHandle)
{
	for (FRDGPass* PassToEnd : ResourcePass->ResourcesToEnd)
	{
		for (const auto& PassState : PassToEnd->TextureStates)
		{
			EndResourceRHI(ExecutePassHandle, PassState.Texture, PassState.ReferenceCount);
		}

		for (const auto& PassState : PassToEnd->BufferStates)
		{
			EndResourceRHI(ExecutePassHandle, PassState.Buffer, PassState.ReferenceCount);
		}
	}
}

void FRDGBuilder::CollectPassBarriers(FRDGPass* Pass, FRDGPassHandle PassHandle)
{
	IF_RDG_ENABLE_DEBUG(ConditionalDebugBreak(RDG_BREAKPOINT_PASS_COMPILE, BuilderName.GetTCHAR(), Pass->GetName()));

	for (const auto& PassState : Pass->TextureStates)
	{
		FRDGTextureRef Texture = PassState.Texture;
		AddTransition(PassHandle, Texture, PassState.MergeState);

		IF_RDG_ENABLE_TRACE(Trace.AddTexturePassDependency(Texture, Pass));
	}

	for (const auto& PassState : Pass->BufferStates)
	{
		FRDGBufferRef Buffer = PassState.Buffer;
		AddTransition(PassHandle, Buffer, *PassState.MergeState);

		IF_RDG_ENABLE_TRACE(Trace.AddBufferPassDependency(Buffer, Pass));
	}
}

void FRDGBuilder::CreatePassBarriers()
{
	SCOPED_NAMED_EVENT_TEXT("FRDGBuilder::CreatePassBarriers", FColor::Magenta);

	for (FRDGBarrierBatchBegin* BarrierBatchBegin : TransitionCreateQueue)
	{
		BarrierBatchBegin->CreateTransition();
	}
	TransitionCreateQueue.Reset();
}

void FRDGBuilder::AddEpilogueTransition(FRDGTextureRef Texture)
{
	if (!Texture->bLastOwner || Texture->bCulled || Texture->bFinalizedAccess)
	{
		return;
	}

	const FRDGPassHandle EpiloguePassHandle = GetEpiloguePassHandle();

	FRDGSubresourceState ScratchSubresourceState;

	// Texture is using the RHI transient allocator. Transition it back to Discard in the final pass it is used.
	if (Texture->bTransient && !Texture->TransientTexture->IsAcquired())
	{
		const TInterval<uint32> DiscardPasses = Texture->TransientTexture->GetDiscardPasses();
		const FRDGPassHandle MinDiscardPassHandle(DiscardPasses.Min);
		const FRDGPassHandle MaxDiscardPassHandle(FMath::Min<uint32>(DiscardPasses.Max, EpiloguePassHandle.GetIndex()));

		AddAliasingTransition(MinDiscardPassHandle, MaxDiscardPassHandle, Texture, FRHITransientAliasingInfo::Discard(Texture->GetRHIUnchecked()));

		ScratchSubresourceState.SetPass(ERHIPipeline::Graphics, MaxDiscardPassHandle);
		ScratchSubresourceState.Access = ERHIAccess::Discard;
		InitAsWholeResource(ScratchTextureState, &ScratchSubresourceState);
	}
	// A known final state means extraction from the graph (or an external texture).
	else if(Texture->AccessFinal != ERHIAccess::Unknown)
	{
		ScratchSubresourceState.SetPass(ERHIPipeline::Graphics, EpiloguePassHandle);
		ScratchSubresourceState.Access = Texture->AccessFinal;
		InitAsWholeResource(ScratchTextureState, &ScratchSubresourceState);
	}
	// Lifetime is within the graph, but a pass may have left the resource in an async compute state. We cannot
	// release the pooled texture back to the pool until we transition back to the graphics pipe.
	else if (Texture->bUsedByAsyncComputePass)
	{
		FRDGTextureSubresourceState& TextureState = Texture->GetState();
		ScratchTextureState.SetNumUninitialized(TextureState.Num(), false);

		for (uint32 Index = 0, Count = ScratchTextureState.Num(); Index < Count; ++Index)
		{
			FRDGSubresourceState SubresourceState = TextureState[Index];

			// Transition async compute back to the graphics pipe.
			if (SubresourceState.IsUsedBy(ERHIPipeline::AsyncCompute))
			{
				SubresourceState.SetPass(ERHIPipeline::Graphics, EpiloguePassHandle);

				ScratchTextureState[Index] = AllocSubresource(SubresourceState);
			}
			else
			{
				ScratchTextureState[Index] = nullptr;
			}
		}
	}
	// No need to transition; texture stayed on the graphics pipe and its lifetime stayed within the graph.
	else
	{
		return;
	}

	AddTransition(EpiloguePassHandle, Texture, ScratchTextureState);
	ScratchTextureState.Reset();
}

void FRDGBuilder::AddEpilogueTransition(FRDGBufferRef Buffer)
{
	if (!Buffer->bLastOwner || Buffer->bCulled || Buffer->bFinalizedAccess)
	{
		return;
	}

	const FRDGPassHandle EpiloguePassHandle = GetEpiloguePassHandle();

	if (Buffer->bTransient)
	{
		const TInterval<uint32> DiscardPasses = Buffer->TransientBuffer->GetDiscardPasses();
		const FRDGPassHandle MinDiscardPassHandle(DiscardPasses.Min);
		const FRDGPassHandle MaxDiscardPassHandle(FMath::Min<uint32>(DiscardPasses.Max, EpiloguePassHandle.GetIndex()));

		AddAliasingTransition(MinDiscardPassHandle, MaxDiscardPassHandle, Buffer, FRHITransientAliasingInfo::Discard(Buffer->GetRHIUnchecked()));

		FRDGSubresourceState StateFinal;
		StateFinal.SetPass(ERHIPipeline::Graphics, MaxDiscardPassHandle);
		StateFinal.Access = ERHIAccess::Discard;
		AddTransition(Buffer->LastPass, Buffer, StateFinal);
	}
	else
	{
		ERHIAccess AccessFinal = Buffer->AccessFinal;

		// Transition async compute back to the graphics pipe.
		if (AccessFinal == ERHIAccess::Unknown)
		{
			const FRDGSubresourceState State = Buffer->GetState();

			if (State.IsUsedBy(ERHIPipeline::AsyncCompute))
			{
				AccessFinal = State.Access;
			}
		}

		if (AccessFinal != ERHIAccess::Unknown)
		{
			FRDGSubresourceState StateFinal;
			StateFinal.SetPass(ERHIPipeline::Graphics, EpiloguePassHandle);
			StateFinal.Access = AccessFinal;
			AddTransition(EpiloguePassHandle, Buffer, StateFinal);
		}
	}
}

void FRDGBuilder::AddTransition(FRDGPassHandle PassHandle, FRDGTextureRef Texture, const FRDGTextureTransientSubresourceStateIndirect& StateAfter)
{
	const FRDGTextureSubresourceRange WholeRange = Texture->GetSubresourceRange();
	const FRDGTextureSubresourceLayout Layout = Texture->Layout;
	FRDGTextureSubresourceState& StateBefore = Texture->GetState();

	const auto AddSubresourceTransition = [&] (
		const FRDGSubresourceState& SubresourceStateBefore,
		const FRDGSubresourceState& SubresourceStateAfter,
		FRDGTextureSubresource* Subresource)
	{
		check(SubresourceStateAfter.Access != ERHIAccess::Unknown);

		if (FRDGSubresourceState::IsTransitionRequired(SubresourceStateBefore, SubresourceStateAfter))
		{
			FRHITransitionInfo Info;
			Info.Texture = Texture->GetRHIUnchecked();
			Info.Type = FRHITransitionInfo::EType::Texture;
			Info.Flags = SubresourceStateAfter.Flags;
			Info.AccessBefore = SubresourceStateBefore.Access;
			Info.AccessAfter = SubresourceStateAfter.Access;

			if (Info.AccessBefore == ERHIAccess::Discard)
			{
				Info.Flags |= EResourceTransitionFlags::Discard;
			}

			if (Subresource)
			{
				Info.MipIndex = Subresource->MipIndex;
				Info.ArraySlice = Subresource->ArraySlice;
				Info.PlaneSlice = Subresource->PlaneSlice;
			}

			AddTransition(Texture, SubresourceStateBefore, SubresourceStateAfter, Info);
		}

		if (Subresource)
		{
			IF_RDG_ENABLE_DEBUG(LogFile.AddTransitionEdge(PassHandle, SubresourceStateBefore, SubresourceStateAfter, Texture, *Subresource));
		}
		else
		{
			IF_RDG_ENABLE_DEBUG(LogFile.AddTransitionEdge(PassHandle, SubresourceStateBefore, SubresourceStateAfter, Texture));
		}
	};

	if (IsWholeResource(StateBefore))
	{
		// 1 -> 1
		if (IsWholeResource(StateAfter))
		{
			if (const FRDGSubresourceState* SubresourceStateAfter = GetWholeResource(StateAfter))
			{
				FRDGSubresourceState& SubresourceStateBefore = GetWholeResource(StateBefore);
				AddSubresourceTransition(SubresourceStateBefore, *SubresourceStateAfter, nullptr);
				SubresourceStateBefore = *SubresourceStateAfter;
			}
		}
		// 1 -> N
		else
		{
			const FRDGSubresourceState SubresourceStateBeforeWhole = GetWholeResource(StateBefore);
			InitAsSubresources(StateBefore, Layout, SubresourceStateBeforeWhole);
			WholeRange.EnumerateSubresources([&](FRDGTextureSubresource Subresource)
			{
				if (FRDGSubresourceState* SubresourceStateAfter = GetSubresource(StateAfter, Layout, Subresource))
				{
					AddSubresourceTransition(SubresourceStateBeforeWhole, *SubresourceStateAfter, &Subresource);
					FRDGSubresourceState& SubresourceStateBefore = GetSubresource(StateBefore, Layout, Subresource);
					SubresourceStateBefore = *SubresourceStateAfter;
				}
			});
		}
	}
	else
	{
		// N -> 1
		if (IsWholeResource(StateAfter))
		{
			if (const FRDGSubresourceState* SubresourceStateAfter = GetWholeResource(StateAfter))
			{
				WholeRange.EnumerateSubresources([&](FRDGTextureSubresource Subresource)
				{
					AddSubresourceTransition(GetSubresource(StateBefore, Layout, Subresource), *SubresourceStateAfter, &Subresource);
				});
				InitAsWholeResource(StateBefore);
				FRDGSubresourceState& SubresourceStateBefore = GetWholeResource(StateBefore);
				SubresourceStateBefore = *SubresourceStateAfter;
			}
		}
		// N -> N
		else
		{
			WholeRange.EnumerateSubresources([&](FRDGTextureSubresource Subresource)
			{
				if (FRDGSubresourceState* SubresourceStateAfter = GetSubresource(StateAfter, Layout, Subresource))
				{
					FRDGSubresourceState& SubresourceStateBefore = GetSubresource(StateBefore, Layout, Subresource);
					AddSubresourceTransition(SubresourceStateBefore, *SubresourceStateAfter, &Subresource);
					SubresourceStateBefore = *SubresourceStateAfter;
				}
			});
		}
	}
}

void FRDGBuilder::AddTransition(FRDGPassHandle PassHandle, FRDGBufferRef Buffer, FRDGSubresourceState StateAfter)
{
	check(StateAfter.Access != ERHIAccess::Unknown);

	FRDGSubresourceState& StateBefore = Buffer->GetState();

	if (FRDGSubresourceState::IsTransitionRequired(StateBefore, StateAfter))
	{
		FRHITransitionInfo Info;
		Info.Resource = Buffer->GetRHIUnchecked();
		Info.Type = FRHITransitionInfo::EType::Buffer;
		Info.Flags = StateAfter.Flags;
		Info.AccessBefore = StateBefore.Access;
		Info.AccessAfter = StateAfter.Access;

		AddTransition(Buffer, StateBefore, StateAfter, Info);
	}

	IF_RDG_ENABLE_DEBUG(LogFile.AddTransitionEdge(PassHandle, StateBefore, StateAfter, Buffer));
	StateBefore = StateAfter;
}

void FRDGBuilder::AddTransition(
	FRDGParentResource* Resource,
	FRDGSubresourceState StateBefore,
	FRDGSubresourceState StateAfter,
	const FRHITransitionInfo& TransitionInfo)
{
	const ERHIPipeline Graphics = ERHIPipeline::Graphics;
	const ERHIPipeline AsyncCompute = ERHIPipeline::AsyncCompute;

#if RDG_ENABLE_DEBUG
	StateBefore.Validate();
	StateAfter.Validate();
#endif

	if (IsImmediateMode())
	{
		// Immediate mode simply enqueues the barrier into the 'after' pass. Everything is on the graphics pipe.
		AddToPrologueBarriers(StateAfter.FirstPass[Graphics], [&](FRDGBarrierBatchBegin& Barriers)
		{
			Barriers.AddTransition(Resource, TransitionInfo);
		});
		return;
	}

	StateBefore.LastPass = ClampToPrologue(StateBefore.LastPass);

	ERHIPipeline PipelinesBefore = StateBefore.GetPipelines();
	ERHIPipeline PipelinesAfter = StateAfter.GetPipelines();

	// This may be the first use of the resource in the graph, so we assign the prologue as the previous pass.
	if (PipelinesBefore == ERHIPipeline::None)
	{
		StateBefore.SetPass(Graphics, GetProloguePassHandle());
		PipelinesBefore = Graphics;
	}

	check(PipelinesBefore != ERHIPipeline::None && PipelinesAfter != ERHIPipeline::None);
	checkf(StateBefore.GetLastPass() <= StateAfter.GetFirstPass(), TEXT("Submitted a state for '%s' that begins before our previous state has ended."), Resource->Name);

	const FRDGPassHandlesByPipeline& PassesBefore = StateBefore.LastPass;
	const FRDGPassHandlesByPipeline& PassesAfter = StateAfter.FirstPass;

	// 1-to-1 or 1-to-N pipe transition.
	if (PipelinesBefore != ERHIPipeline::All)
	{
		const FRDGPassHandle BeginPassHandle = StateBefore.GetLastPass();
		const FRDGPassHandle FirstEndPassHandle = StateAfter.GetFirstPass();

		FRDGPass* BeginPass = nullptr;
		FRDGBarrierBatchBegin* BarriersToBegin = nullptr;

		// Issue the begin in the epilogue of the begin pass if the barrier is being split across multiple passes or the barrier end is in the epilogue.
		if (BeginPassHandle < FirstEndPassHandle)
		{
			BeginPass = GetEpilogueBarrierPass(BeginPassHandle);
			BarriersToBegin = &BeginPass->GetEpilogueBarriersToBeginFor(Allocator, TransitionCreateQueue, PipelinesAfter);
		}
		// This is an immediate prologue transition in the same pass. Issue the begin in the prologue.
		else
		{
			checkf(PipelinesAfter == ERHIPipeline::Graphics,
				TEXT("Attempted to queue an immediate async pipe transition for %s. Pipelines: %s. Async transitions must be split."),
				Resource->Name, *GetRHIPipelineName(PipelinesAfter));

			BeginPass = GetPrologueBarrierPass(BeginPassHandle);
			BarriersToBegin = &BeginPass->GetPrologueBarriersToBegin(Allocator, TransitionCreateQueue);
		}

		BarriersToBegin->AddTransition(Resource, TransitionInfo);

		for (ERHIPipeline Pipeline : GetRHIPipelines())
		{
			/** If doing a 1-to-N transition and this is the same pipe as the begin, we end it immediately afterwards in the epilogue
			 *  of the begin pass. This is because we can't guarantee that the other pipeline won't join back before the end. This can
			 *  happen if the forking async compute pass joins back to graphics (via another independent transition) before the current
			 *  graphics transition is ended.
			 *
			 *  Async Compute Pipe:               EndA  BeginB
			 *                                   /            \
			 *  Graphics Pipe:            BeginA               EndB   EndA
			 *
			 *  A is our 1-to-N transition and B is a future transition of the same resource that we haven't evaluated yet. Instead, the
			 *  same pipe End is performed in the epilogue of the begin pass, which removes the spit barrier but simplifies the tracking:
			 *
			 *  Async Compute Pipe:               EndA  BeginB
			 *                                   /            \
			 *  Graphics Pipe:            BeginA  EndA         EndB
			 */
			if ((PipelinesBefore == Pipeline && PipelinesAfter == ERHIPipeline::All))
			{
				AddToEpilogueBarriersToEnd(BeginPassHandle, *BarriersToBegin);
			}
			else if (EnumHasAnyFlags(PipelinesAfter, Pipeline))
			{
				AddToPrologueBarriersToEnd(PassesAfter[Pipeline], *BarriersToBegin);
			}
		}
	}
	// N-to-1 or N-to-N transition.
	else
	{
		checkf(StateBefore.GetLastPass() != StateAfter.GetFirstPass(),
			TEXT("Attempted to queue a transition for resource '%s' from '%s' to '%s', but previous and next passes are the same on one pipe."),
			Resource->Name, *GetRHIPipelineName(PipelinesBefore), *GetRHIPipelineName(PipelinesAfter));

		FRDGBarrierBatchBeginId Id;
		Id.PipelinesAfter = PipelinesAfter;
		for (ERHIPipeline Pipeline : GetRHIPipelines())
		{
			Id.Passes[Pipeline] = GetEpilogueBarrierPassHandle(PassesBefore[Pipeline]);
		}

		FRDGBarrierBatchBegin*& BarriersToBegin = BarrierBatchMap.FindOrAdd(Id);

		if (!BarriersToBegin)
		{
			FRDGPassesByPipeline BarrierBatchPasses;
			BarrierBatchPasses[Graphics]     = Passes[Id.Passes[Graphics]];
			BarrierBatchPasses[AsyncCompute] = Passes[Id.Passes[AsyncCompute]];

			BarriersToBegin = Allocator.AllocNoDestruct<FRDGBarrierBatchBegin>(PipelinesBefore, PipelinesAfter, GetEpilogueBarriersToBeginDebugName(PipelinesAfter), BarrierBatchPasses);
			TransitionCreateQueue.Emplace(BarriersToBegin);

			for (FRDGPass* Pass : BarrierBatchPasses)
			{
				Pass->SharedEpilogueBarriersToBegin.Add(BarriersToBegin);
			}
		}

		BarriersToBegin->AddTransition(Resource, TransitionInfo);

		for (ERHIPipeline Pipeline : GetRHIPipelines())
		{
			if (EnumHasAnyFlags(PipelinesAfter, Pipeline))
			{
				AddToPrologueBarriersToEnd(PassesAfter[Pipeline], *BarriersToBegin);
			}
		}
	}
}

void FRDGBuilder::AddAliasingTransition(FRDGPassHandle BeginPassHandle, FRDGPassHandle EndPassHandle, FRDGParentResourceRef Resource, const FRHITransientAliasingInfo& Info)
{
	check(BeginPassHandle <= EndPassHandle);

	FRDGBarrierBatchBegin* BarriersToBegin{};
	FRDGPass* EndPass{};

	if (BeginPassHandle == EndPassHandle)
	{
		FRDGPass* BeginPass = Passes[BeginPassHandle];
		EndPass = BeginPass;

		check(GetPrologueBarrierPassHandle(BeginPassHandle) == BeginPassHandle);
		check(BeginPass->GetPipeline() == ERHIPipeline::Graphics);

		BarriersToBegin = &BeginPass->GetPrologueBarriersToBegin(Allocator, TransitionCreateQueue);
	}
	else
	{
		FRDGPass* BeginPass = GetEpilogueBarrierPass(BeginPassHandle);
		EndPass = Passes[EndPassHandle];

		check(GetPrologueBarrierPassHandle(EndPassHandle) == EndPassHandle);
		check(BeginPass->GetPipeline() == ERHIPipeline::Graphics);
		check(EndPass->GetPipeline() == ERHIPipeline::Graphics);

		BarriersToBegin = &BeginPass->GetEpilogueBarriersToBeginForGraphics(Allocator, TransitionCreateQueue);
	}

	BarriersToBegin->AddAlias(Resource, Info);
	EndPass->GetPrologueBarriersToEnd(Allocator).AddDependency(BarriersToBegin);
}

void FRDGBuilder::BeginResourceRHI(FRDGPassHandle PassHandle, FRDGTextureRef Texture)
{
	check(Texture);

	if (Texture->HasRHI())
	{
		return;
	}

	check(Texture->ReferenceCount > 0 || Texture->bExternal || IsImmediateMode());

#if RDG_ENABLE_DEBUG
	{
		FRDGPass* Pass = Passes[PassHandle];

		// Cannot begin a resource on an async compute pass.
		check(Pass->Pipeline == ERHIPipeline::Graphics);

		// Cannot begin a resource within a merged render pass region.
		checkf(GetPrologueBarrierPassHandle(PassHandle) == PassHandle,
			TEXT("Cannot begin a resource within a merged render pass. Pass (Handle: %d, Name: %s), Resource %s"), PassHandle, Pass->GetName(), Texture->Name);
	}
#endif

	if (TransientResourceAllocator && IsTransient(Texture))
	{
		if (FRHITransientTexture* TransientTexture = TransientResourceAllocator->CreateTexture(Texture->Desc, Texture->Name, PassHandle.GetIndex()))
		{
			if (Texture->bExternal || Texture->bExtracted)
			{
				Texture->SetRHI(GRDGTransientResourceAllocator.AllocateRenderTarget(TransientTexture));
			}
			else
			{
				Texture->SetRHI(TransientTexture, Allocator.AllocNoDestruct<FRDGTextureSubresourceState>());
			}

			const FRDGPassHandle MinAcquirePassHandle = ClampToPrologue(FRDGPassHandle(TransientTexture->GetAcquirePasses().Min));

			AddAliasingTransition(MinAcquirePassHandle, PassHandle, Texture, FRHITransientAliasingInfo::Acquire(TransientTexture->GetRHI(), TransientTexture->GetAliasingOverlaps()));

			FRDGSubresourceState InitialState;
			InitialState.SetPass(ERHIPipeline::Graphics, MinAcquirePassHandle);
			InitialState.Access = ERHIAccess::Discard;
			InitAsWholeResource(Texture->GetState(), InitialState);

		#if STATS
			GRDGStatTransientTextureCount++;
		#endif
		}
	}

	if (!Texture->ResourceRHI)
	{
		const bool bResetToUnknownState = false;
		Texture->SetRHI(GRenderTargetPool.FindFreeElementInternal(Texture->Desc, Texture->Name, bResetToUnknownState));
	}

	Texture->FirstPass = PassHandle;

	check(Texture->HasRHI());
}

void FRDGBuilder::BeginResourceRHI(FRDGPassHandle PassHandle, FRDGTextureSRVRef SRV)
{
	check(SRV);

	if (SRV->HasRHI())
	{
		return;
	}

	FRDGTextureRef Texture = SRV->Desc.Texture;
	FRHITexture* TextureRHI = Texture->GetRHIUnchecked();
	check(TextureRHI);

	SRV->ResourceRHI = Texture->ViewCache->GetOrCreateSRV(TextureRHI, SRV->Desc);
}

void FRDGBuilder::BeginResourceRHI(FRDGPassHandle PassHandle, FRDGTextureUAVRef UAV)
{
	check(UAV);

	if (UAV->HasRHI())
	{
		return;
	}

	FRDGTextureRef Texture = UAV->Desc.Texture;
	FRHITexture* TextureRHI = Texture->GetRHIUnchecked();
	check(TextureRHI);

	UAV->ResourceRHI = Texture->ViewCache->GetOrCreateUAV(TextureRHI, UAV->Desc);
}

void FRDGBuilder::BeginResourceRHI(FRDGPassHandle PassHandle, FRDGBufferRef Buffer)
{
	check(Buffer);

	if (Buffer->HasRHI())
	{
		return;
	}

	check(Buffer->ReferenceCount > 0 || Buffer->bExternal || IsImmediateMode());

#if RDG_ENABLE_DEBUG
	{
		const FRDGPass* Pass = Passes[PassHandle];

		// Cannot begin a resource on an async compute pass.
		check(Pass->Pipeline == ERHIPipeline::Graphics);

		// Cannot begin a resource within a merged render pass region.
		checkf(GetPrologueBarrierPassHandle(PassHandle) == PassHandle,
			TEXT("Cannot begin a resource within a merged render pass. Pass (Handle: %d, Name: %s), Resource %s"), PassHandle, Pass->GetName(), Buffer->Name);
	}
#endif
	Buffer->FinalizeDesc();

	// If transient then create the resource on the transient allocator. External or extracted resource can't be transient because of lifetime tracking issues.
	if (TransientResourceAllocator && IsTransient(Buffer))
	{
		if (FRHITransientBuffer* TransientBuffer = TransientResourceAllocator->CreateBuffer(Translate(Buffer->Desc), Buffer->Name, PassHandle.GetIndex()))
		{
			Buffer->SetRHI(TransientBuffer, Allocator);

			const FRDGPassHandle MinAcquirePassHandle = ClampToPrologue(FRDGPassHandle(TransientBuffer->GetAcquirePasses().Min));

			AddAliasingTransition(MinAcquirePassHandle, PassHandle, Buffer, FRHITransientAliasingInfo::Acquire(TransientBuffer->GetRHI(), TransientBuffer->GetAliasingOverlaps()));

			FRDGSubresourceState& InitialState = Buffer->GetState();
			InitialState.SetPass(ERHIPipeline::Graphics, MinAcquirePassHandle);
			InitialState.Access = ERHIAccess::Discard;

		#if STATS
			GRDGStatTransientBufferCount++;
		#endif
		}
	}

	if (!Buffer->bTransient)
	{
		Buffer->SetRHI(GRenderGraphResourcePool.FindFreeBufferInternal(RHICmdList, Buffer->Desc, Buffer->Name));
	}

	Buffer->FirstPass = PassHandle;

	check(Buffer->HasRHI());
}

void FRDGBuilder::BeginResourceRHI(FRDGPassHandle PassHandle, FRDGBufferSRVRef SRV)
{
	check(SRV);

	if (SRV->HasRHI())
	{
		return;
	}

	FRDGBufferRef Buffer = SRV->Desc.Buffer;
	FRHIBuffer* BufferRHI = Buffer->GetRHIUnchecked();
	check(BufferRHI);

	FRHIBufferSRVCreateInfo SRVCreateInfo = SRV->Desc;

	if (Buffer->Desc.UnderlyingType == FRDGBufferDesc::EUnderlyingType::StructuredBuffer)
	{
		// RDG allows structured buffer views to be typed, but the view creation logic requires that it
		// be unknown (as do platform APIs -- structured buffers are not typed). This could be validated
		// at the high level but the current API makes it confusing. For now, it's considered a no-op.
		SRVCreateInfo.Format = PF_Unknown;
	}

	SRV->ResourceRHI = Buffer->ViewCache->GetOrCreateSRV(BufferRHI, SRVCreateInfo);
}

void FRDGBuilder::BeginResourceRHI(FRDGPassHandle PassHandle, FRDGBufferUAV* UAV)
{
	check(UAV);

	if (UAV->HasRHI())
	{
		return;
	}

	FRDGBufferRef Buffer = UAV->Desc.Buffer;
	check(Buffer);

	FRHIBufferUAVCreateInfo UAVCreateInfo = UAV->Desc;

	if (Buffer->Desc.UnderlyingType == FRDGBufferDesc::EUnderlyingType::StructuredBuffer)
	{
		// RDG allows structured buffer views to be typed, but the view creation logic requires that it
		// be unknown (as do platform APIs -- structured buffers are not typed). This could be validated
		// at the high level but the current API makes it confusing. For now, it's considered a no-op.
		UAVCreateInfo.Format = PF_Unknown;
	}

	UAV->ResourceRHI = Buffer->ViewCache->GetOrCreateUAV(Buffer->GetRHIUnchecked(), UAVCreateInfo);
}

void FRDGBuilder::BeginResourceRHI(FRDGPassHandle PassHandle, FRDGView* View)
{
	if (View->HasRHI())
	{
		return;
	}

	switch (View->Type)
	{
	case ERDGViewType::TextureUAV:
		BeginResourceRHI(PassHandle, static_cast<FRDGTextureUAV*>(View));
		break;
	case ERDGViewType::TextureSRV:
		BeginResourceRHI(PassHandle, static_cast<FRDGTextureSRV*>(View));
		break;
	case ERDGViewType::BufferUAV:
		BeginResourceRHI(PassHandle, static_cast<FRDGBufferUAV*>(View));
		break;
	case ERDGViewType::BufferSRV:
		BeginResourceRHI(PassHandle, static_cast<FRDGBufferSRV*>(View));
		break;
	}
}

void FRDGBuilder::EndResourceRHI(FRDGPassHandle PassHandle, FRDGTextureRef Texture, uint32 ReferenceCount)
{
	check(Texture);
	check(Texture->ReferenceCount >= ReferenceCount || IsImmediateMode());
	Texture->ReferenceCount -= ReferenceCount;

	if (Texture->ReferenceCount == 0)
	{
		if (Texture->bTransient)
		{
			// Texture is using a transient external render target.
			if (Texture->PooledRenderTarget)
			{
				// This releases the reference without invoking a virtual function call.
				GRDGTransientResourceAllocator.Release(TRefCountPtr<FRDGTransientRenderTarget>(MoveTemp(Texture->Allocation)), PassHandle);
			}
			// Texture is using an internal transient texture.
			else
			{
				TransientResourceAllocator->DeallocateMemory(Texture->TransientTexture, PassHandle.GetIndex());
			}
		}
		else
		{
			// If this is a non-transient texture, it must be backed by a pooled render target.
			FPooledRenderTarget* RenderTarget = static_cast<FPooledRenderTarget*>(Texture->PooledRenderTarget);
			check(RenderTarget);

			// Only tracked render targets are released. Untracked ones persist until the end of the frame.
			if (RenderTarget->IsTracked())
			{
				// This releases the reference without invoking a virtual function call.
				TRefCountPtr<FPooledRenderTarget>(MoveTemp(Texture->Allocation));
			}
		}

		Texture->LastPass = PassHandle;
	}
}

void FRDGBuilder::EndResourceRHI(FRDGPassHandle PassHandle, FRDGBufferRef Buffer, uint32 ReferenceCount)
{
	check(Buffer);
	check(Buffer->ReferenceCount >= ReferenceCount || IsImmediateMode());
	Buffer->ReferenceCount -= ReferenceCount;

	if (Buffer->ReferenceCount == 0)
	{
		if (Buffer->bTransient)
		{
			TransientResourceAllocator->DeallocateMemory(Buffer->TransientBuffer, PassHandle.GetIndex());
		}
		else
		{
			Buffer->Allocation = nullptr;
		}

		Buffer->LastPass = PassHandle;
	}
}

#if RDG_ENABLE_DEBUG

void FRDGBuilder::VisualizePassOutputs(const FRDGPass* Pass)
{
#if SUPPORTS_VISUALIZE_TEXTURE
	if (bInDebugPassScope)
	{
		return;
	}

	bInDebugPassScope = true;


	Pass->GetParameters().EnumerateTextures([&](FRDGParameter Parameter)
	{
		switch (Parameter.GetType())
		{
		case UBMT_RDG_TEXTURE_ACCESS:
		{
			if (FRDGTextureAccess TextureAccess = Parameter.GetAsTextureAccess())
			{
				if (TextureAccess.GetAccess() == ERHIAccess::UAVCompute ||
					TextureAccess.GetAccess() == ERHIAccess::UAVGraphics ||
					TextureAccess.GetAccess() == ERHIAccess::RTV)
				{
					if (TOptional<uint32> CaptureId = GVisualizeTexture.ShouldCapture(TextureAccess->Name, /* MipIndex = */ 0))
					{
						GVisualizeTexture.CreateContentCapturePass(*this, TextureAccess.GetTexture(), *CaptureId);
					}
				}
			}
		}
		break;
		case UBMT_RDG_TEXTURE_UAV:
		{
			if (FRDGTextureUAVRef UAV = Parameter.GetAsTextureUAV())
			{
				FRDGTextureRef Texture = UAV->Desc.Texture;
				if (TOptional<uint32> CaptureId = GVisualizeTexture.ShouldCapture(Texture->Name, UAV->Desc.MipLevel))
				{
					GVisualizeTexture.CreateContentCapturePass(*this, Texture, *CaptureId);
				}
			}
		}
		break;
		case UBMT_RENDER_TARGET_BINDING_SLOTS:
		{
			const FRenderTargetBindingSlots& RenderTargets = Parameter.GetAsRenderTargetBindingSlots();

			RenderTargets.Enumerate([&](FRenderTargetBinding RenderTarget)
			{
				FRDGTextureRef Texture = RenderTarget.GetTexture();
				if (TOptional<uint32> CaptureId = GVisualizeTexture.ShouldCapture(Texture->Name, RenderTarget.GetMipIndex()))
				{
					GVisualizeTexture.CreateContentCapturePass(*this, Texture, *CaptureId);
				}
			});

			const FDepthStencilBinding& DepthStencil = RenderTargets.DepthStencil;

			if (FRDGTextureRef Texture = DepthStencil.GetTexture())
			{
				const bool bHasStoreAction = DepthStencil.GetDepthStencilAccess().IsAnyWrite();

				if (bHasStoreAction)
				{
					const uint32 MipIndex = 0;
					if (TOptional<uint32> CaptureId = GVisualizeTexture.ShouldCapture(Texture->Name, MipIndex))
					{
						GVisualizeTexture.CreateContentCapturePass(*this, Texture, *CaptureId);
					}
				}
			}
		}
		break;
		}
	});

	bInDebugPassScope = false;
#endif
}

void FRDGBuilder::ClobberPassOutputs(const FRDGPass* Pass)
{
	if (!GRDGClobberResources)
	{
		return;
	}

	if (bInDebugPassScope)
	{
		return;
	}
	bInDebugPassScope = true;

	RDG_EVENT_SCOPE(*this, "RDG ClobberResources");

	const FLinearColor ClobberColor = GetClobberColor();

	Pass->GetParameters().Enumerate([&](FRDGParameter Parameter)
	{
		switch (Parameter.GetType())
		{
		case UBMT_RDG_BUFFER_UAV:
		{
			if (FRDGBufferUAVRef UAV = Parameter.GetAsBufferUAV())
			{
				FRDGBufferRef Buffer = UAV->GetParent();

				if (UserValidation.TryMarkForClobber(Buffer))
				{
					AddClearUAVPass(*this, UAV, GetClobberBufferValue());
				}
			}
		}
		break;
		case UBMT_RDG_TEXTURE_ACCESS:
		{
			if (FRDGTextureAccess TextureAccess = Parameter.GetAsTextureAccess())
			{
				FRDGTextureRef Texture = TextureAccess.GetTexture();

				if (UserValidation.TryMarkForClobber(Texture))
				{
					if (EnumHasAnyFlags(TextureAccess.GetAccess(), ERHIAccess::UAVMask))
					{
						for (int32 MipLevel = 0; MipLevel < Texture->Desc.NumMips; MipLevel++)
						{
							AddClearUAVPass(*this, CreateUAV(FRDGTextureUAVDesc(Texture, MipLevel)), ClobberColor);
						}
					}
					else if (EnumHasAnyFlags(TextureAccess.GetAccess(), ERHIAccess::RTV))
					{
						AddClearRenderTargetPass(*this, Texture, ClobberColor);
					}
				}
			}
		}
		break;
		case UBMT_RDG_TEXTURE_UAV:
		{
			if (FRDGTextureUAVRef UAV = Parameter.GetAsTextureUAV())
			{
				FRDGTextureRef Texture = UAV->GetParent();

				if (UserValidation.TryMarkForClobber(Texture))
				{
					if (Texture->Desc.NumMips == 1)
					{
						AddClearUAVPass(*this, UAV, ClobberColor);
					}
					else
					{
						for (int32 MipLevel = 0; MipLevel < Texture->Desc.NumMips; MipLevel++)
						{
							AddClearUAVPass(*this, CreateUAV(FRDGTextureUAVDesc(Texture, MipLevel)), ClobberColor);
						}
					}
				}
			}
		}
		break;
		case UBMT_RENDER_TARGET_BINDING_SLOTS:
		{
			const FRenderTargetBindingSlots& RenderTargets = Parameter.GetAsRenderTargetBindingSlots();

			RenderTargets.Enumerate([&](FRenderTargetBinding RenderTarget)
			{
				FRDGTextureRef Texture = RenderTarget.GetTexture();

				if (UserValidation.TryMarkForClobber(Texture))
				{
					AddClearRenderTargetPass(*this, Texture, ClobberColor);
				}
			});

			if (FRDGTextureRef Texture = RenderTargets.DepthStencil.GetTexture())
			{
				if (UserValidation.TryMarkForClobber(Texture))
				{
					AddClearDepthStencilPass(*this, Texture, true, GetClobberDepth(), true, GetClobberStencil());
				}
			}
		}
		break;
		}
	});

	bInDebugPassScope = false;
}

#endif //! RDG_ENABLE_DEBUG

#if WITH_MGPU
void FRDGBuilder::ForceCopyCrossGPU()
{
	// Initialize set of external buffers
	TSet<FRHIBuffer*> ExternalBufferSet;
	ExternalBufferSet.Reserve(ExternalBuffers.Num());

	for (auto ExternalBufferIt = ExternalBuffers.CreateConstIterator(); ExternalBufferIt; ++ExternalBufferIt)
	{
		ExternalBufferSet.Emplace(ExternalBufferIt.Value()->GetRHIUnchecked());
	}

	// Generate list of cross GPU resources from all passes, and the GPU mask where they were last written 
	TMap<FRHIBuffer*, FRHIGPUMask> BuffersToTransfer;
	TMap<FRHITexture*, FRHIGPUMask> TexturesToTransfer;

	for (FRDGPassHandle PassHandle = GetProloguePassHandle(); PassHandle <= GetEpiloguePassHandle(); ++PassHandle)
	{
		FRDGPass* Pass = Passes[PassHandle];

		for (int32 BufferIndex = 0; BufferIndex < Pass->BufferStates.Num(); BufferIndex++)
		{
			FRHIBuffer* BufferRHI = Pass->BufferStates[BufferIndex].Buffer->GetRHIUnchecked();

			if (ExternalBufferSet.Contains(BufferRHI) &&
				!EnumHasAnyFlags(BufferRHI->GetUsage(), BUF_MultiGPUAllocate | BUF_MultiGPUGraphIgnore) &&
				EnumHasAnyFlags(Pass->BufferStates[BufferIndex].State.Access, ERHIAccess::WritableMask))
			{
				BuffersToTransfer.Emplace(BufferRHI) = Pass->GPUMask;
			}
		}

		for (int32 TextureIndex = 0; TextureIndex < Pass->TextureStates.Num(); TextureIndex++)
		{
			if (ExternalTextures.Contains(Pass->TextureStates[TextureIndex].Texture->GetRHIUnchecked()))
			{
				for (int32 StateIndex = 0; StateIndex < Pass->TextureStates[TextureIndex].State.Num(); StateIndex++)
				{
					FRHITexture* TextureRHI = Pass->TextureStates[TextureIndex].Texture->GetRHIUnchecked();

					if (TextureRHI &&
						!EnumHasAnyFlags(TextureRHI->GetFlags(), TexCreate_MultiGPUGraphIgnore) &&
						EnumHasAnyFlags(Pass->TextureStates[TextureIndex].State[StateIndex].Access, ERHIAccess::WritableMask))
					{
						TexturesToTransfer.Emplace(Pass->TextureStates[TextureIndex].Texture->GetRHIUnchecked()) = Pass->GPUMask;
					}
				}
			}
		}
	}

	// Now that we've got the list of external resources, and the GPU they were last written to, make a list of what needs to
	// be propagated to other GPUs.
	TArray<FTransferResourceParams> Transfers;
	const FRHIGPUMask AllGPUMask = FRHIGPUMask::All();
	const bool bPullData = false;
	const bool bLockstepGPUs = true;

	for (auto BufferIt = BuffersToTransfer.CreateConstIterator(); BufferIt; ++BufferIt)
	{
		FRHIBuffer* Buffer = BufferIt.Key();
		FRHIGPUMask GPUMask = BufferIt.Value();

		for (uint32 GPUIndex : AllGPUMask)
		{
			if (!GPUMask.Contains(GPUIndex))
			{
				Transfers.Add(FTransferResourceParams(Buffer, GPUMask.GetFirstIndex(), GPUIndex, bPullData, bLockstepGPUs));
			}
		}
	}

	for (auto TextureIt = TexturesToTransfer.CreateConstIterator(); TextureIt; ++TextureIt)
	{
		FRHITexture* Texture = TextureIt.Key();
		FRHIGPUMask GPUMask = TextureIt.Value();

		for (uint32 GPUIndex : AllGPUMask)
		{
			if (!GPUMask.Contains(GPUIndex))
			{
				Transfers.Add(FTransferResourceParams(Texture, GPUMask.GetFirstIndex(), GPUIndex, bPullData, bLockstepGPUs));
			}
		}
	}

	if (Transfers.Num())
	{
		RHICmdList.TransferResources(Transfers);
	}
}
#endif  // WITH_MGPU
