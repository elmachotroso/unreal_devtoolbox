// Copyright Epic Games, Inc. All Rights Reserved.

#include "VulkanRHIPrivate.h"
#include "VulkanContext.h"
#include "ClearReplacementShaders.h"

#if VULKAN_RHI_RAYTRACING
#include "VulkanRayTracing.h"
#endif // VULKAN_RHI_RAYTRACING

FVulkanShaderResourceView::FVulkanShaderResourceView(FVulkanDevice* Device, FRHIResource* InRHIBuffer, FVulkanResourceMultiBuffer* InSourceBuffer, uint32 InSize, EPixelFormat InFormat, uint32 InOffset)
	: VulkanRHI::FVulkanViewBase(Device)
	, BufferViewFormat(InFormat)
	, SourceTexture(nullptr)
	, SourceStructuredBuffer(nullptr)
	, Size(InSize)
	, Offset(InOffset)
	, SourceBuffer(InSourceBuffer)
	, SourceRHIBuffer(InRHIBuffer)
{
	check(Device);
	if(SourceBuffer)
	{
		int32 NumBuffers = SourceBuffer->IsVolatile() ? 1 : SourceBuffer->GetNumBuffers();
		BufferViews.AddZeroed(NumBuffers);
	}
	check(BufferViewFormat != PF_Unknown);
}


FVulkanShaderResourceView::FVulkanShaderResourceView(FVulkanDevice* Device, FRHITexture* InSourceTexture, const FRHITextureSRVCreateInfo& InCreateInfo)
	: VulkanRHI::FVulkanViewBase(Device)
	, BufferViewFormat((EPixelFormat)InCreateInfo.Format)
	, SRGBOverride(InCreateInfo.SRGBOverride)
	, SourceTexture(InSourceTexture)
	, SourceStructuredBuffer(nullptr)
	, MipLevel(InCreateInfo.MipLevel)
	, NumMips(InCreateInfo.NumMipLevels)
	, FirstArraySlice(InCreateInfo.FirstArraySlice)
	, NumArraySlices(InCreateInfo.NumArraySlices)
	, Size(0)
	, SourceBuffer(nullptr)
{
	FVulkanTextureBase* VulkanTexture = FVulkanTextureBase::Cast(InSourceTexture);
	VulkanTexture->AttachView(this);

}

FVulkanShaderResourceView::FVulkanShaderResourceView(FVulkanDevice* InDevice, FVulkanResourceMultiBuffer* InSourceBuffer, uint32 InOffset)
	: VulkanRHI::FVulkanViewBase(InDevice)
{
	check(InDevice && InSourceBuffer);

#if VULKAN_RHI_RAYTRACING
	if (EnumHasAnyFlags(InSourceBuffer->GetUsage(), BUF_AccelerationStructure))
	{
		SourceRHIBuffer = InSourceBuffer;

		VkAccelerationStructureCreateInfoKHR CreateInfo;
		ZeroVulkanStruct(CreateInfo, VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR);
		CreateInfo.buffer = InSourceBuffer->GetHandle();
		CreateInfo.offset = InOffset;
		CreateInfo.size = InSourceBuffer->GetSize() - InOffset;
		CreateInfo.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;

		VkDevice NativeDevice = InDevice->GetInstanceHandle();
		VERIFYVULKANRESULT(VulkanDynamicAPI::vkCreateAccelerationStructureKHR(NativeDevice, &CreateInfo, VULKAN_CPU_ALLOCATOR, &AccelerationStructureHandle));
	}
	else
#endif
	{
		SourceStructuredBuffer = InSourceBuffer;
		Size = InSourceBuffer->GetSize() - InOffset;
		Offset = InOffset;
	}
}

FVulkanShaderResourceView::~FVulkanShaderResourceView()
{
	FRHITexture* Texture = SourceTexture.GetReference();
	if(Texture)
	{
		FVulkanTextureBase* VulkanTexture = FVulkanTextureBase::Cast(Texture);
		VulkanTexture->DetachView(this);
	}
	Clear();
	Device = nullptr;
}

void FVulkanShaderResourceView::Clear()
{
#if VULKAN_RHI_RAYTRACING
	if (Device && AccelerationStructureHandle)
	{
		Device->GetDeferredDeletionQueue().EnqueueResource(VulkanRHI::FDeferredDeletionQueue2::EType::AccelerationStructure, AccelerationStructureHandle);
	}
#endif // VULKAN_RHI_RAYTRACING

	SourceRHIBuffer = nullptr;
	SourceBuffer = nullptr;
	BufferViews.Empty();
	SourceStructuredBuffer = nullptr;
	if (Device)
	{
		TextureView.Destroy(*Device);
	}
	SourceTexture = nullptr;

	VolatileBufferHandle = VK_NULL_HANDLE;
	VolatileLockCounter = MAX_uint32;
}

void FVulkanShaderResourceView::Rename(FRHIResource* InRHIBuffer, FVulkanResourceMultiBuffer* InSourceBuffer, uint32 InSize, EPixelFormat InFormat)
{
	check(Device);
	check(!Offset);

#if VULKAN_RHI_RAYTRACING
	checkf(!AccelerationStructureHandle, TEXT("Acceleration structure view renaming is currently not supported"));
#endif //VULKAN_RHI_RAYTRACING

	BufferViewFormat = InFormat;
	SourceTexture = nullptr;
	TextureView.Destroy(*Device);
	SourceStructuredBuffer = nullptr;
	MipLevel = 0;
	NumMips = -1;
	BufferViews.Reset();
	BufferViews.AddZeroed(InSourceBuffer->IsVolatile() ? 1 : InSourceBuffer->GetNumBuffers());
	BufferIndex = 0;
	Size = InSize;
	SourceBuffer = InSourceBuffer;
	SourceRHIBuffer = InRHIBuffer;
	VolatileBufferHandle = VK_NULL_HANDLE;
	VolatileLockCounter = MAX_uint32;
}
void FVulkanShaderResourceView::Invalidate()
{
	TextureView.Destroy(*Device);
}

void FVulkanShaderResourceView::UpdateView()
{
#if VULKAN_ENABLE_AGGRESSIVE_STATS
	SCOPE_CYCLE_COUNTER(STAT_VulkanSRVUpdateTime);
#endif

	// update the buffer view for dynamic backed buffers (or if it was never set)
	if (SourceBuffer != nullptr)
	{
		uint32 CurrentViewSize = Size;
		if (SourceBuffer->IsVolatile() && VolatileLockCounter != SourceBuffer->GetVolatileLockCounter())
		{
			VkBuffer SourceVolatileBufferHandle = SourceBuffer->GetHandle();

			// If the volatile buffer shrinks, make sure our size doesn't exceed the new limit.
			uint32 AvailableSize = SourceBuffer->GetVolatileLockSize();
			AvailableSize = Offset < AvailableSize ? AvailableSize - Offset : 0;
			CurrentViewSize = FMath::Min(CurrentViewSize, AvailableSize);

			// We might end up with the same BufferView, so do not recreate in that case
			if (!BufferViews[0]
				|| BufferViews[0]->Offset != (SourceBuffer->GetOffset() + Offset)
				|| BufferViews[0]->Size != CurrentViewSize
				|| VolatileBufferHandle != SourceVolatileBufferHandle)
			{
				BufferViews[0] = nullptr;
			}

			VolatileLockCounter = SourceBuffer->GetVolatileLockCounter();
			VolatileBufferHandle = SourceVolatileBufferHandle;
		}
		else if (SourceBuffer->IsDynamic())
		{
			BufferIndex = SourceBuffer->GetDynamicIndex();
		}

		if (!BufferViews[BufferIndex])
		{
			BufferViews[BufferIndex] = new FVulkanBufferView(Device);
			BufferViews[BufferIndex]->Create(SourceBuffer, BufferViewFormat, SourceBuffer->GetOffset() + Offset, CurrentViewSize);
		}
	}
	else if (SourceStructuredBuffer)
	{
		// Nothing...
	}
#if VULKAN_RHI_RAYTRACING
	else if (AccelerationStructureHandle)
	{
		// Nothing
	}
#endif //VULKAN_RHI_RAYTRACING
	else
	{
		if (TextureView.View == VK_NULL_HANDLE)
		{
			const bool bBaseSRGB = EnumHasAnyFlags(SourceTexture->GetFlags(), TexCreate_SRGB);
			const bool bSRGB = (SRGBOverride != SRGBO_ForceDisable) && bBaseSRGB;

			EPixelFormat Format = (BufferViewFormat == PF_Unknown) ? SourceTexture->GetFormat() : BufferViewFormat;
			if (FRHITexture2D* Tex2D = SourceTexture->GetTexture2D())
			{
				FVulkanTexture2D* VTex2D = ResourceCast(Tex2D);
				EPixelFormat OriginalFormat = Format;
				TextureView.Create(*Device, VTex2D->Surface.Image, VK_IMAGE_VIEW_TYPE_2D, VTex2D->Surface.GetPartialAspectMask(), Format, UEToVkTextureFormat(Format, bSRGB), MipLevel, NumMips, 0, 1);
			}
			else if (FRHITextureCube* TexCube = SourceTexture->GetTextureCube())
			{
				FVulkanTextureCube* VTexCube = ResourceCast(TexCube);
				TextureView.Create(*Device, VTexCube->Surface.Image, VK_IMAGE_VIEW_TYPE_CUBE, VTexCube->Surface.GetPartialAspectMask(), Format, UEToVkTextureFormat(Format, bSRGB), MipLevel, NumMips, 0, 1);
			}
			else if (FRHITexture3D* Tex3D = SourceTexture->GetTexture3D())
			{
				FVulkanTexture3D* VTex3D = ResourceCast(Tex3D);
				TextureView.Create(*Device, VTex3D->Surface.Image, VK_IMAGE_VIEW_TYPE_3D, VTex3D->Surface.GetPartialAspectMask(), Format, UEToVkTextureFormat(Format, bSRGB), MipLevel, NumMips, 0, 1);
			}
			else if (FRHITexture2DArray* Tex2DArray = SourceTexture->GetTexture2DArray())
			{
				FVulkanTexture2DArray* VTex2DArray = ResourceCast(Tex2DArray);
				TextureView.Create(
					*Device,
					VTex2DArray->Surface.Image,
					VK_IMAGE_VIEW_TYPE_2D_ARRAY,
					VTex2DArray->Surface.GetPartialAspectMask(),
					Format,
					UEToVkTextureFormat(Format, bSRGB),
					MipLevel,
					NumMips,
					FirstArraySlice,
					(NumArraySlices == 0 ? VTex2DArray->GetSizeZ() : NumArraySlices)
				);
			}
			else
			{
				ensure(0);
			}
		}
	}
}
FVulkanUnorderedAccessView::FVulkanUnorderedAccessView(FVulkanDevice* Device, FVulkanResourceMultiBuffer* Buffer, bool bUseUAVCounter, bool bAppendBuffer)
	: VulkanRHI::FVulkanViewBase(Device)
	, MipLevel(0)
	, FirstArraySlice(0)
	, NumArraySlices(0)
	, SourceBuffer(Buffer)
	, BufferViewFormat(PF_Unknown)
	, VolatileLockCounter(MAX_uint32)
{
}

FVulkanUnorderedAccessView::FVulkanUnorderedAccessView(FVulkanDevice* Device, FRHITexture* TextureRHI, uint32 MipLevel, uint16 InFirstArraySlice, uint16 InNumArraySlices)
	: VulkanRHI::FVulkanViewBase(Device)
	, SourceTexture(TextureRHI)
	, MipLevel(MipLevel)
	, FirstArraySlice(InFirstArraySlice)
	, NumArraySlices(InNumArraySlices)
	, BufferViewFormat(PF_Unknown)
	, VolatileLockCounter(MAX_uint32)
{
	FVulkanTextureBase* VulkanTexture = FVulkanTextureBase::Cast(TextureRHI);
	VulkanTexture->AttachView(this);
}


FVulkanUnorderedAccessView::FVulkanUnorderedAccessView(FVulkanDevice* Device, FVulkanResourceMultiBuffer* Buffer, EPixelFormat Format)
	: VulkanRHI::FVulkanViewBase(Device)
	, MipLevel(0)
	, FirstArraySlice(0)
	, NumArraySlices(0)
	, BufferViewFormat(Format)
	, VolatileLockCounter(MAX_uint32)
{
	SourceBuffer = Buffer;
}

void FVulkanUnorderedAccessView::Invalidate()
{
	check(SourceTexture);
	TextureView.Destroy(*Device);
}

FVulkanUnorderedAccessView::~FVulkanUnorderedAccessView()
{
	if (SourceTexture)
	{
		FVulkanTextureBase* VulkanTexture = FVulkanTextureBase::Cast(SourceTexture);
		VulkanTexture->DetachView(this);
	}

	TextureView.Destroy(*Device);
	BufferView = nullptr;
	SourceBuffer = nullptr;
	SourceTexture = nullptr;
	Device = nullptr;
}

void FVulkanUnorderedAccessView::UpdateView()
{
#if VULKAN_ENABLE_AGGRESSIVE_STATS
	SCOPE_CYCLE_COUNTER(STAT_VulkanUAVUpdateTime);
#endif

	// update the buffer view for dynamic VB backed buffers (or if it was never set)
	if (SourceBuffer != nullptr)
	{
		if (BufferViewFormat != PF_Unknown)
		{
			if (SourceBuffer->IsVolatile() && VolatileLockCounter != SourceBuffer->GetVolatileLockCounter())
			{
				BufferView = nullptr;
				VolatileLockCounter = SourceBuffer->GetVolatileLockCounter();
			}

			if (BufferView == nullptr || SourceBuffer->IsDynamic())
			{
				// thanks to ref counting, overwriting the buffer will toss the old view
				BufferView = new FVulkanBufferView(Device);
				BufferView->Create(SourceBuffer.GetReference(), BufferViewFormat, SourceBuffer->GetOffset(), SourceBuffer->GetSize());
			}
		}
	}
	else if (TextureView.View == VK_NULL_HANDLE)
	{
		EPixelFormat Format = (BufferViewFormat == PF_Unknown) ? SourceTexture->GetFormat() : BufferViewFormat;
		if (FRHITexture2D* Tex2D = SourceTexture->GetTexture2D())
		{
			FVulkanTexture2D* VTex2D = ResourceCast(Tex2D);
			TextureView.Create(*Device, VTex2D->Surface.Image, VK_IMAGE_VIEW_TYPE_2D, VTex2D->Surface.GetPartialAspectMask(), Format, UEToVkTextureFormat(Format, false), MipLevel, 1, 0, 1, true);
		}
		else if (FRHITextureCube* TexCube = SourceTexture->GetTextureCube())
		{
			FVulkanTextureCube* VTexCube = ResourceCast(TexCube);
			// RWTextureCube is defined as RWTexture2DArray in shader source, avoid validation errors by creating the appropriate VK_IMAGE_VIEW_TYPE_2D_ARRAY view (instead of VK_IMAGE_VIEW_TYPE_CUBE)
			TextureView.Create(*Device, VTexCube->Surface.Image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VTexCube->Surface.GetPartialAspectMask(), Format, UEToVkTextureFormat(Format, false), MipLevel, 1, 0, VTexCube->Surface.GetNumberOfArrayLevels(), true);
		}
		else if (FRHITexture3D* Tex3D = SourceTexture->GetTexture3D())
		{
			FVulkanTexture3D* VTex3D = ResourceCast(Tex3D);
			TextureView.Create(*Device, VTex3D->Surface.Image, VK_IMAGE_VIEW_TYPE_3D, VTex3D->Surface.GetPartialAspectMask(), Format, UEToVkTextureFormat(Format, false), MipLevel, 1, 0, VTex3D->GetSizeZ(), true);
		}
		else if (FRHITexture2DArray* Tex2DArray = SourceTexture->GetTexture2DArray())
		{
			FVulkanTexture2DArray* VTex2DArray = ResourceCast(Tex2DArray);
			TextureView.Create(*Device, VTex2DArray->Surface.Image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VTex2DArray->Surface.GetPartialAspectMask(), Format, UEToVkTextureFormat(Format, false), MipLevel, 1, 
				NumArraySlices == 0 ? 0 : FirstArraySlice, NumArraySlices == 0 ? VTex2DArray->GetSizeZ() : NumArraySlices, true);
		}
		else
		{
			ensure(0);
		}
	}
}

FUnorderedAccessViewRHIRef FVulkanDynamicRHI::RHICreateUnorderedAccessView(FRHIBuffer* BufferRHI, bool bUseUAVCounter, bool bAppendBuffer)
{
	FVulkanResourceMultiBuffer* Buffer = ResourceCast(BufferRHI);

	FVulkanUnorderedAccessView* UAV = new FVulkanUnorderedAccessView(Device, Buffer, bUseUAVCounter, bAppendBuffer);
	return UAV;
}

FUnorderedAccessViewRHIRef FVulkanDynamicRHI::RHICreateUnorderedAccessView(FRHITexture* TextureRHI, uint32 MipLevel, uint16 FirstArraySlice, uint16 NumArraySlices)
{
	FVulkanUnorderedAccessView* UAV = new FVulkanUnorderedAccessView(Device, TextureRHI, MipLevel, FirstArraySlice, NumArraySlices);
	return UAV;
}

FUnorderedAccessViewRHIRef FVulkanDynamicRHI::RHICreateUnorderedAccessView(FRHIBuffer* BufferRHI, uint8 Format)
{
	FVulkanResourceMultiBuffer* Buffer = ResourceCast(BufferRHI);

	FVulkanUnorderedAccessView* UAV = new FVulkanUnorderedAccessView(Device, Buffer, (EPixelFormat)Format);
	return UAV;
}

FShaderResourceViewRHIRef FVulkanDynamicRHI::RHICreateShaderResourceView(FRHIBuffer* BufferRHI, uint32 Stride, uint8 Format)
{	
	if (!BufferRHI)
	{
		return new FVulkanShaderResourceView(Device, nullptr, nullptr, 0, (EPixelFormat)Format);
	}
	FVulkanResourceMultiBuffer* Buffer = ResourceCast(BufferRHI);
	return new FVulkanShaderResourceView(Device, BufferRHI, Buffer, Buffer->GetSize(), (EPixelFormat)Format);
}

FShaderResourceViewRHIRef FVulkanDynamicRHI::RHICreateShaderResourceView(const FShaderResourceViewInitializer& Initializer)
{
	const FShaderResourceViewInitializer::FBufferShaderResourceViewInitializer Desc = Initializer.AsBufferSRV();
	FVulkanResourceMultiBuffer* Buffer = ResourceCast(Desc.Buffer);

	switch (Initializer.GetType())
	{
		case FShaderResourceViewInitializer::EType::VertexBufferSRV:
		{
			if (Desc.Buffer)
			{
				const uint32 Stride = GPixelFormats[Desc.Format].BlockBytes;
				uint32 Size = FMath::Min(Buffer->GetSize() - Desc.StartOffsetBytes, Desc.NumElements * Stride);
				return new FVulkanShaderResourceView(Device, Desc.Buffer, Buffer, Size, (EPixelFormat)Desc.Format, Desc.StartOffsetBytes);
			}
			else
			{
				return new FVulkanShaderResourceView(Device, nullptr, nullptr, 0, (EPixelFormat)Desc.Format, Desc.StartOffsetBytes);
			}
		}
		case FShaderResourceViewInitializer::EType::StructuredBufferSRV:
#if VULKAN_RHI_RAYTRACING
		case FShaderResourceViewInitializer::EType::AccelerationStructureSRV:
#endif
		{
			check(Desc.Buffer);
			return new FVulkanShaderResourceView(Device, Buffer, Desc.StartOffsetBytes);
		}			
		case FShaderResourceViewInitializer::EType::IndexBufferSRV:
		{
			check(Desc.Buffer);
			const uint32 Stride = Desc.Buffer->GetStride();
			check(Stride == 2 || Stride == 4);
			EPixelFormat Format = (Stride == 4) ? PF_R32_UINT : PF_R16_UINT;
			uint32 Size = FMath::Min(Buffer->GetSize() - Desc.StartOffsetBytes, Desc.NumElements * Stride);
			return new FVulkanShaderResourceView(Device, Desc.Buffer, Buffer, Size, Format, Desc.StartOffsetBytes);
		}
	}
	checkNoEntry();
	return nullptr;
}

FShaderResourceViewRHIRef FVulkanDynamicRHI::RHICreateShaderResourceView(FRHITexture* Texture, const FRHITextureSRVCreateInfo& CreateInfo)
{
	FVulkanShaderResourceView* SRV = new FVulkanShaderResourceView(Device, Texture, CreateInfo);
	return SRV;
}

FShaderResourceViewRHIRef FVulkanDynamicRHI::RHICreateShaderResourceView(FRHIBuffer* BufferRHI)
{
	if (BufferRHI && EnumHasAnyFlags(BufferRHI->GetUsage(), BUF_VertexBuffer | BUF_StructuredBuffer | BUF_AccelerationStructure))
	{
		FVulkanResourceMultiBuffer* Buffer = ResourceCast(BufferRHI);
		FVulkanShaderResourceView* SRV = new FVulkanShaderResourceView(Device, Buffer);
		return SRV;
	}
	else
	{
		if (!BufferRHI)
		{
			return new FVulkanShaderResourceView(Device, nullptr, nullptr, 0, PF_R16_UINT);
		}
		check(EnumHasAnyFlags(BufferRHI->GetUsage(), BUF_IndexBuffer));
		check(BufferRHI->GetStride() == 2 || BufferRHI->GetStride() == 4);
		FVulkanResourceMultiBuffer* Buffer = ResourceCast(BufferRHI);
		EPixelFormat Format = (BufferRHI->GetStride() == 4) ? PF_R32_UINT : PF_R16_UINT;
		FVulkanShaderResourceView* SRV = new FVulkanShaderResourceView(Device, BufferRHI, Buffer, Buffer->GetSize(), Format);
		return SRV;
	}
}

void FVulkanDynamicRHI::RHIUpdateShaderResourceView(FRHIShaderResourceView* SRV, FRHIBuffer* Buffer, uint32 Stride, uint8 Format)
{
	FVulkanShaderResourceView* SRVVk = ResourceCast(SRV);
	check(SRVVk && SRVVk->GetParent() == Device);
	if (!Buffer)
	{
		SRVVk->Clear();
	}
	else if (SRVVk->SourceRHIBuffer.GetReference() != Buffer)
	{
		FVulkanResourceMultiBuffer* BufferVk = ResourceCast(Buffer);
		SRVVk->Rename(Buffer, BufferVk, BufferVk->GetSize(), (EPixelFormat)Format);
	}
}

void FVulkanDynamicRHI::RHIUpdateShaderResourceView(FRHIShaderResourceView* SRV, FRHIBuffer* Buffer)
{
	FVulkanShaderResourceView* SRVVk = ResourceCast(SRV);
	check(SRVVk && SRVVk->GetParent() == Device);
	if (!Buffer)
	{
		SRVVk->Clear();
	}
	else if (SRVVk->SourceRHIBuffer.GetReference() != Buffer)
	{
		FVulkanResourceMultiBuffer* BufferVk = ResourceCast(Buffer);
		SRVVk->Rename(Buffer, BufferVk, BufferVk->GetSize(), BufferVk->GetStride() == 2u ? PF_R16_UINT : PF_R32_UINT);
	}
}

void FVulkanCommandListContext::ClearUAVFillBuffer(FVulkanUnorderedAccessView* UAV, uint32_t ClearValue)
{
	FVulkanCommandBufferManager* CmdBufferMgr = GVulkanRHI->GetDevice()->GetImmediateContext().GetCommandBufferManager();
	FVulkanCmdBuffer* CmdBuffer = CmdBufferMgr->GetActiveCmdBuffer();

	FVulkanResourceMultiBuffer* Buffer = UAV->SourceBuffer;
	VulkanRHI::vkCmdFillBuffer(CmdBuffer->GetHandle(), Buffer->GetHandle(), Buffer->GetOffset(), Buffer->GetCurrentSize(), ClearValue);
}

void FVulkanCommandListContext::ClearUAV(TRHICommandList_RecursiveHazardous<FVulkanCommandListContext>& RHICmdList, FVulkanUnorderedAccessView* UnorderedAccessView, const void* ClearValue, bool bFloat)
{
	struct FVulkanDynamicRHICmdFillBuffer final : public FRHICommand<FVulkanDynamicRHICmdFillBuffer>
	{
		FVulkanUnorderedAccessView* UAV;
		uint32_t ClearValue;

		FORCEINLINE_DEBUGGABLE FVulkanDynamicRHICmdFillBuffer(FVulkanUnorderedAccessView* InUAV, uint32_t InClearValue)
			: UAV(InUAV), ClearValue(InClearValue)
		{
		}

		void Execute(FRHICommandListBase& CmdList)
		{
			ClearUAVFillBuffer(UAV, ClearValue);
		}
	};

	EClearReplacementValueType ValueType;
	if (!bFloat)
	{
		EPixelFormat Format;
		if (UnorderedAccessView->SourceBuffer)
		{
			Format = UnorderedAccessView->BufferViewFormat;
		}
		else if (UnorderedAccessView->SourceTexture)
		{
			Format = UnorderedAccessView->SourceTexture->GetFormat();
		}
		else
		{
			Format = PF_Unknown;
		}

		switch (Format)
		{
		case PF_R32_SINT:
		case PF_R16_SINT:
		case PF_R16G16B16A16_SINT:
			ValueType = EClearReplacementValueType::Int32;
			break;
		default:
			ValueType = EClearReplacementValueType::Uint32;
			break;
		}
	}
	else
	{
		ValueType = EClearReplacementValueType::Float;
	}

	if (UnorderedAccessView->SourceBuffer)
	{
		TRefCountPtr<FVulkanResourceMultiBuffer> Buffer = UnorderedAccessView->SourceBuffer;
		bool bIsByteAddressBuffer = EnumHasAnyFlags(Buffer->GetUsage(), BUF_ByteAddressBuffer);

		// Byte address buffers only use the first component, so use vkCmdBufferFill
		if (UnorderedAccessView->BufferViewFormat == PF_Unknown || bIsByteAddressBuffer)
		{
			RHICmdList.Transition(FRHITransitionInfo(UnorderedAccessView, ERHIAccess::UAVCompute, ERHIAccess::CopyDest));

			if (RHICmdList.Bypass())
			{
				ClearUAVFillBuffer(UnorderedAccessView, *(const uint32_t*)ClearValue);
			}
			else
			{
				new (RHICmdList.AllocCommand<FVulkanDynamicRHICmdFillBuffer>()) FVulkanDynamicRHICmdFillBuffer(UnorderedAccessView, *(const uint32_t*)ClearValue);
			}

			RHICmdList.Transition(FRHITransitionInfo(UnorderedAccessView, ERHIAccess::CopyDest, ERHIAccess::UAVCompute));
		}
		else
		{
			const uint32 NumElements = Buffer->GetCurrentSize() / GPixelFormats[UnorderedAccessView->BufferViewFormat].BlockBytes;
			const uint32 ComputeWorkGroupCount = FMath::DivideAndRoundUp(NumElements, (uint32)ClearReplacementCS::TThreadGroupSize<EClearReplacementResourceType::Buffer>::X);
			FVulkanDevice* TargetDevice = FVulkanCommandListContext::GetVulkanContext(RHICmdList.GetContext()).GetDevice();
			const bool bOversizedBuffer = (ComputeWorkGroupCount > TargetDevice->GetLimits().maxComputeWorkGroupCount[0]);
			if (bOversizedBuffer)
			{
				ClearUAVShader_T<EClearReplacementResourceType::LargeBuffer, 4, false>(RHICmdList, UnorderedAccessView, NumElements, 1, 1, ClearValue, ValueType);
			}
			else
			{
				ClearUAVShader_T<EClearReplacementResourceType::Buffer, 4, false>(RHICmdList, UnorderedAccessView, NumElements, 1, 1, ClearValue, ValueType);
			}
		}
	}
	else if (UnorderedAccessView->SourceTexture)
	{
		FIntVector SizeXYZ = UnorderedAccessView->SourceTexture->GetSizeXYZ();

		if (FRHITexture2D* Texture2D = UnorderedAccessView->SourceTexture->GetTexture2D())
		{
			ClearUAVShader_T<EClearReplacementResourceType::Texture2D, 4, false>(RHICmdList, UnorderedAccessView, SizeXYZ.X, SizeXYZ.Y, SizeXYZ.Z, ClearValue, ValueType);
		}
		else if (FRHITexture2DArray* Texture2DArray = UnorderedAccessView->SourceTexture->GetTexture2DArray())
		{
			ClearUAVShader_T<EClearReplacementResourceType::Texture2DArray, 4, false>(RHICmdList, UnorderedAccessView, SizeXYZ.X, SizeXYZ.Y, SizeXYZ.Z, ClearValue, ValueType);
		}
		else if (FRHITexture3D* Texture3D = UnorderedAccessView->SourceTexture->GetTexture3D())
		{
			ClearUAVShader_T<EClearReplacementResourceType::Texture3D, 4, false>(RHICmdList, UnorderedAccessView, SizeXYZ.X, SizeXYZ.Y, SizeXYZ.Z, ClearValue, ValueType);
		}
		else if (FRHITextureCube* TextureCube = UnorderedAccessView->SourceTexture->GetTextureCube())
		{
			ClearUAVShader_T<EClearReplacementResourceType::Texture2DArray, 4, false>(RHICmdList, UnorderedAccessView, SizeXYZ.X, SizeXYZ.Y, SizeXYZ.Z, ClearValue, ValueType);
		}
		else
		{
			ensureMsgf(0, TEXT("SourceTexture of unknown type (Name=[%s], Format=%d, Flags=0x%x)!  Skipping ClearUAV..."), 
				*UnorderedAccessView->SourceTexture->GetName().ToString(), (uint32)UnorderedAccessView->SourceTexture->GetFormat(), (uint32)UnorderedAccessView->SourceTexture->GetFlags());
		}
	}
	else
	{
		ensureMsgf(0, TEXT("UnorderedAccessView has no source buffer or texture!  Skipping ClearUAV..."));
	}
}

void FVulkanCommandListContext::RHIClearUAVFloat(FRHIUnorderedAccessView* UnorderedAccessViewRHI, const FVector4f& Values)
{
	TRHICommandList_RecursiveHazardous<FVulkanCommandListContext> RHICmdList(this);
	ClearUAV(RHICmdList, ResourceCast(UnorderedAccessViewRHI), &Values, true);
}

void FVulkanCommandListContext::RHIClearUAVUint(FRHIUnorderedAccessView* UnorderedAccessViewRHI, const FUintVector4& Values)
{
	TRHICommandList_RecursiveHazardous<FVulkanCommandListContext> RHICmdList(this);
	ClearUAV(RHICmdList, ResourceCast(UnorderedAccessViewRHI), &Values, false);
}

void FVulkanGPUFence::Clear()
{
	CmdBuffer = nullptr;
	FenceSignaledCounter = MAX_uint64;
}

bool FVulkanGPUFence::Poll() const
{
	return (CmdBuffer && (FenceSignaledCounter < CmdBuffer->GetFenceSignaledCounter()));
}

FGPUFenceRHIRef FVulkanDynamicRHI::RHICreateGPUFence(const FName& Name)
{
	return new FVulkanGPUFence(Name);
}
