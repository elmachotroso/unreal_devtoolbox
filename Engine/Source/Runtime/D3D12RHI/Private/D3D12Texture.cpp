// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	D3D12Texture.cpp: D3D texture RHI implementation.
	=============================================================================*/

#include "D3D12RHIPrivate.h"
#include "D3D12RHIBridge.h"
#include "TextureProfiler.h"

int64 FD3D12GlobalStats::GDedicatedVideoMemory = 0;
int64 FD3D12GlobalStats::GDedicatedSystemMemory = 0;
int64 FD3D12GlobalStats::GSharedSystemMemory = 0;
int64 FD3D12GlobalStats::GTotalGraphicsMemory = 0;

int32 GAdjustTexturePoolSizeBasedOnBudget = 0;
static FAutoConsoleVariableRef CVarAdjustTexturePoolSizeBasedOnBudget(
	TEXT("D3D12.AdjustTexturePoolSizeBasedOnBudget"),
	GAdjustTexturePoolSizeBasedOnBudget,
	TEXT("Indicates if the RHI should lower the texture pool size when the application is over the memory budget provided by the OS. This can result in lower quality textures (but hopefully improve performance).")
	);

static TAutoConsoleVariable<int32> CVarD3D12Texture2DRHIFlush(
	TEXT("D3D12.LockTexture2DRHIFlush"),
	0,
	TEXT("If enabled, we do RHIThread flush on LockTexture2D. Likely not required on any platform, but keeping just for testing for now")
	TEXT(" 0: off (default)\n")
	TEXT(" 1: on"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarUseUpdateTexture3DComputeShader(
	TEXT("D3D12.UseUpdateTexture3DComputeShader"),
	0,
	TEXT("If enabled, use a compute shader for UpdateTexture3D. Avoids alignment restrictions")
	TEXT(" 0: off (default)\n")
	TEXT(" 1: on"),
	ECVF_RenderThreadSafe );

static TAutoConsoleVariable<bool> CVarTexturePoolOnlyAccountStreamableTexture(
	TEXT("D3D12.TexturePoolOnlyAccountStreamableTexture"),
	0,
	TEXT("Texture streaming pool size only account streamable texture .\n")
	TEXT(" - 0: All texture types are counted in the pool (legacy, default).\n")
	TEXT(" - 1: Only streamable textures are counted in the pool.\n")
	TEXT("When enabling the new behaviour, r.Streaming.PoolSize will need to be re-adjusted.\n"),
	ECVF_ReadOnly
);

// Forward Decls for template types
template TD3D12Texture2D<FD3D12BaseTexture2D>::TD3D12Texture2D(class FD3D12Device* InParent, uint32 InSizeX, uint32 InSizeY, uint32 InSizeZ, uint32 InNumMips, uint32 InNumSamples,
	EPixelFormat InFormat, bool bInCubemap, ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue, const FD3D12TextureLayout* InTextureLayout
#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
	, void* InRawTextureMemory
#endif
	);
template TD3D12Texture2D<FD3D12BaseTexture2DArray>::TD3D12Texture2D(class FD3D12Device* InParent, uint32 InSizeX, uint32 InSizeY, uint32 InSizeZ, uint32 InNumMips, uint32 InNumSamples,
	EPixelFormat InFormat, bool bInCubemap, ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue, const FD3D12TextureLayout* InTextureLayout
#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
	, void* InRawTextureMemory
#endif
	);
template TD3D12Texture2D<FD3D12BaseTextureCube>::TD3D12Texture2D(class FD3D12Device* InParent, uint32 InSizeX, uint32 InSizeY, uint32 InSizeZ, uint32 InNumMips, uint32 InNumSamples,
	EPixelFormat InFormat, bool bInCubemap, ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue, const FD3D12TextureLayout* InTextureLayout
#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
	, void* InRawTextureMemory
#endif
	);

template TD3D12Texture2D<FD3D12BaseTexture2D>::~TD3D12Texture2D();
template TD3D12Texture2D<FD3D12BaseTexture2DArray>::~TD3D12Texture2D();
template TD3D12Texture2D<FD3D12BaseTextureCube>::~TD3D12Texture2D();


/// @cond DOXYGEN_WARNINGS

template void FD3D12TextureStats::D3D12TextureAllocated(TD3D12Texture2D<FD3D12BaseTexture2D>& Texture, const D3D12_RESOURCE_DESC *Desc);
template void FD3D12TextureStats::D3D12TextureAllocated(TD3D12Texture2D<FD3D12BaseTexture2DArray>& Texture, const D3D12_RESOURCE_DESC *Desc);
template void FD3D12TextureStats::D3D12TextureAllocated(TD3D12Texture2D<FD3D12BaseTextureCube>& Texture, const D3D12_RESOURCE_DESC *Desc);

template void FD3D12TextureStats::D3D12TextureDeleted(TD3D12Texture2D<FD3D12BaseTexture2D>& Texture);
template void FD3D12TextureStats::D3D12TextureDeleted(TD3D12Texture2D<FD3D12BaseTexture2DArray>& Texture);
template void FD3D12TextureStats::D3D12TextureDeleted(TD3D12Texture2D<FD3D12BaseTextureCube>& Texture);

template void TD3D12Texture2D<FD3D12BaseTexture2D>::GetReadBackHeapDesc(D3D12_PLACED_SUBRESOURCE_FOOTPRINT& OutFootprint, uint32 Subresource) const;
template void TD3D12Texture2D<FD3D12BaseTexture2DArray>::GetReadBackHeapDesc(D3D12_PLACED_SUBRESOURCE_FOOTPRINT& OutFootprint, uint32 Subresource) const;
template void TD3D12Texture2D<FD3D12BaseTextureCube>::GetReadBackHeapDesc(D3D12_PLACED_SUBRESOURCE_FOOTPRINT& OutFootprint, uint32 Subresource) const;

/// @endcond
struct FRHICommandUpdateTextureString
{
	static const TCHAR* TStr() { return TEXT("FRHICommandUpdateTexture"); }
};
struct FRHICommandUpdateTexture final : public FRHICommand<FRHICommandUpdateTexture, FRHICommandUpdateTextureString>
{
	FD3D12TextureBase* TextureBase;
	uint32 MipIndex;
	uint32 DestX;
	uint32 DestY;
	uint32 DestZ;
	D3D12_TEXTURE_COPY_LOCATION SourceCopyLocation;
	FD3D12ResourceLocation Source;

	FORCEINLINE_DEBUGGABLE FRHICommandUpdateTexture(FD3D12TextureBase* InTextureBase,
		uint32 InMipIndex, uint32 InDestX, uint32 InDestY, uint32 InDestZ,
		const D3D12_TEXTURE_COPY_LOCATION& InSourceCopyLocation, FD3D12ResourceLocation* InSource)
		: TextureBase(InTextureBase)
		, MipIndex(InMipIndex)
		, DestX(InDestX)
		, DestY(InDestY)
		, DestZ(InDestZ)
		, SourceCopyLocation(InSourceCopyLocation)
		, Source(nullptr)
	{
		if (InSource)
		{
			FD3D12ResourceLocation::TransferOwnership(Source, *InSource);
		}
	}

	~FRHICommandUpdateTexture()
	{
	}

	void Execute(FRHICommandListBase& CmdList)
	{
		TextureBase->UpdateTexture(MipIndex, DestX, DestY, DestZ, SourceCopyLocation);
	}
};

struct FRHICommandCopySubTextureRegionString
{
	static const TCHAR* TStr() { return TEXT("FRHICommandCopySubTextureRegion"); }
};
struct FRHICommandCopySubTextureRegion final : public FRHICommand<FRHICommandCopySubTextureRegion, FRHICommandCopySubTextureRegionString>
{
	FD3D12TextureBase* DestTexture;
	uint32 DestX;
	uint32 DestY;
	uint32 DestZ;
	FD3D12TextureBase* SourceTexture;
	D3D12_BOX SourceBox;

	FORCEINLINE_DEBUGGABLE FRHICommandCopySubTextureRegion(FD3D12TextureBase* InDestTexture, uint32 InDestX, uint32 InDestY, uint32 InDestZ, FD3D12TextureBase* InSourceTexture, const D3D12_BOX& InSourceBox)
		: DestTexture(InDestTexture)
		, DestX(InDestX)
		, DestY(InDestY)
		, DestZ(InDestZ)
		, SourceTexture(InSourceTexture)
		, SourceBox(InSourceBox)
	{
	}

	~FRHICommandCopySubTextureRegion()
	{
	}

	void Execute(FRHICommandListBase& CmdList)
	{
		DestTexture->CopyTextureRegion(DestX, DestY, DestZ, SourceTexture, SourceBox);
	}
};

struct FD3D12RHICommandInitializeTextureString
{
	static const TCHAR* TStr() { return TEXT("FD3D12RHICommandInitializeTexture"); }
};
struct FD3D12RHICommandInitializeTexture final : public FRHICommand<FD3D12RHICommandInitializeTexture, FD3D12RHICommandInitializeTextureString>
{
	FD3D12TextureBase* TextureBase;
	FD3D12ResourceLocation SrcResourceLoc;
	uint32 NumSubresources;
	D3D12_RESOURCE_STATES DestinationState;

	FORCEINLINE_DEBUGGABLE FD3D12RHICommandInitializeTexture(FD3D12TextureBase* InTexture, FD3D12ResourceLocation& InSrcResourceLoc, uint32 InNumSubresources, D3D12_RESOURCE_STATES InDestinationState)
		: TextureBase(InTexture)
		, SrcResourceLoc(InSrcResourceLoc.GetParentDevice())
		, NumSubresources(InNumSubresources)
		, DestinationState(InDestinationState)
	{
		FD3D12ResourceLocation::TransferOwnership(SrcResourceLoc, InSrcResourceLoc);
	}

	void Execute(FRHICommandListBase& /* unused */)
	{
		ExecuteNoCmdList();
	}

	void ExecuteNoCmdList()
	{
		size_t MemSize = NumSubresources * (sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) + sizeof(UINT) + sizeof(UINT64));
		const bool bAllocateOnStack = (MemSize < 4096);
		void* Mem = bAllocateOnStack? FMemory_Alloca(MemSize) : FMemory::Malloc(MemSize);

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Footprints = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT*) Mem;
		check(Footprints);
		UINT* Rows = (UINT*) (Footprints + NumSubresources);
		UINT64* RowSizeInBytes = (UINT64*) (Rows + NumSubresources);

		uint64 Size = 0;
		const D3D12_RESOURCE_DESC& Desc = TextureBase->GetResource()->GetDesc();
		TextureBase->GetParentDevice()->GetDevice()->GetCopyableFootprints(&Desc, 0, NumSubresources, SrcResourceLoc.GetOffsetFromBaseOfResource(), Footprints, Rows, RowSizeInBytes, &Size);

		D3D12_TEXTURE_COPY_LOCATION Src;
		Src.pResource = SrcResourceLoc.GetResource()->GetResource();
		Src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;

		// Initialize all the textures in the chain
		for (FD3D12TextureBase& CurrentTexture : *TextureBase)
		{
			FD3D12Device* Device = CurrentTexture.GetParentDevice();
			FD3D12Resource* Resource = CurrentTexture.GetResource();

			FD3D12CommandListHandle& hCommandList = Device->GetDefaultCommandContext().CommandListHandle;
			hCommandList.GetCurrentOwningContext()->numInitialResourceCopies += NumSubresources;

			// resource should be in copy dest already, because it's created like that, so no transition required here
			
			ID3D12GraphicsCommandList* CmdList = hCommandList.GraphicsCommandList();

			D3D12_TEXTURE_COPY_LOCATION Dst;
			Dst.pResource = Resource->GetResource();
			Dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

			for (uint32 Subresource = 0; Subresource < NumSubresources; Subresource++)
			{
				Dst.SubresourceIndex = Subresource;
				Src.PlacedFootprint = Footprints[Subresource];
				CmdList->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
			}			

			// Update the resource state after the copy has been done (will take care of updating the residency as well)
			hCommandList.AddTransitionBarrier(Resource, D3D12_RESOURCE_STATE_COPY_DEST, DestinationState, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);

			if (Resource->RequiresResourceStateTracking())
			{
				// Update the tracked resource state of this resource in the command list
				CResourceState& ResourceState = hCommandList.GetResourceState(Resource);
				ResourceState.SetResourceState(DestinationState);
				Resource->GetResourceState().SetResourceState(DestinationState);

				// Add dummy pending barrier, because the end state needs to be updated after execture command list with tracked state in the command list
				hCommandList.AddPendingResourceBarrier(Resource, D3D12_RESOURCE_STATE_TBD, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
			}
			else
			{
				check(Resource->GetDefaultResourceState() == DestinationState);
			}

			Device->GetDefaultCommandContext().ConditionalFlushCommandList();

			// Texture is now written and ready, so unlock the block (locked after creation and can be defragmented if needed)
			CurrentTexture.ResourceLocation.UnlockPoolData();
		}

		if (!bAllocateOnStack)
		{
			FMemory::Free(Mem);
		}
	}
};

///////////////////////////////////////////////////////////////////////////////////////////
// Texture Stats
///////////////////////////////////////////////////////////////////////////////////////////

bool FD3D12TextureStats::ShouldCountAsTextureMemory(D3D12_RESOURCE_FLAGS MiscFlags)
{
	// Shouldn't be used for DEPTH, RENDER TARGET, or UNORDERED ACCESS
	return !EnumHasAnyFlags(MiscFlags, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
}

// @param b3D true:3D, false:2D or cube map
TStatId FD3D12TextureStats::GetRHIStatEnum(D3D12_RESOURCE_FLAGS MiscFlags, bool bCubeMap, bool b3D)
{
#if STATS
	if (ShouldCountAsTextureMemory(MiscFlags))
	{
		// normal texture
		if (bCubeMap)
		{
			return GET_STATID(STAT_TextureMemoryCube);
		}
		else if (b3D)
		{
			return GET_STATID(STAT_TextureMemory3D);
		}
		else
		{
			return GET_STATID(STAT_TextureMemory2D);
		}
	}
	else
	{
		// render target
		if (bCubeMap)
		{
			return GET_STATID(STAT_RenderTargetMemoryCube);
		}
		else if (b3D)
		{
			return GET_STATID(STAT_RenderTargetMemory3D);
		}
		else
		{
			return GET_STATID(STAT_RenderTargetMemory2D);
		}
	}
#endif
	return TStatId();
}

TStatId FD3D12TextureStats::GetD3D12StatEnum(D3D12_RESOURCE_FLAGS MiscFlags)
{
#if STATS
	if (EnumHasAnyFlags(MiscFlags, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
	{
		return GET_STATID(STAT_D3D12RenderTargets);
	}
	else if (EnumHasAnyFlags(MiscFlags, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
	{
		return GET_STATID(STAT_D3D12UAVTextures);
	}
	else
	{
		return GET_STATID(STAT_D3D12Textures);
	}
#endif
	return TStatId();
}

// Note: This function can be called from many different threads
// @param TextureSize >0 to allocate, <0 to deallocate
// @param b3D true:3D, false:2D or cube map
template<typename TD3D12Texture>
void FD3D12TextureStats::UpdateD3D12TextureStats(TD3D12Texture& Texture, const D3D12_RESOURCE_DESC& Desc, int64 TextureSize, bool b3D, bool bCubeMap, bool bStreamable, bool bNewTexture)
{

#if TEXTURE_PROFILER_ENABLED

	if (!bNewTexture && 
		!Texture.ResourceLocation.IsTransient() 
		&& !EnumHasAnyFlags(Texture.GetFlags(), TexCreate_Virtual)
		&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eAliased
		&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eHeapAliased)
	{
		uint64 SafeSize = (uint64)(TextureSize >= 0 ? TextureSize : 0);
		FTextureProfiler::Get()->UpdateTextureAllocation(&Texture, SafeSize, Desc.Alignment, 0);
	}
#endif

	if (TextureSize == 0)
	{
		return;
	}

	const int64 AlignedSize = (TextureSize > 0) ? Align(TextureSize, 1024) / 1024 : -(Align(-TextureSize, 1024) / 1024);
	if (ShouldCountAsTextureMemory(Desc.Flags))
	{
		bool bOnlyStreamableTextureAccounted = CVarTexturePoolOnlyAccountStreamableTexture.GetValueOnAnyThread();

		if (!bOnlyStreamableTextureAccounted || bStreamable)
		{
			FPlatformAtomics::InterlockedAdd(&GCurrentTextureMemorySize, AlignedSize);
		}
	}
	else
	{
		FPlatformAtomics::InterlockedAdd(&GCurrentRendertargetMemorySize, AlignedSize);
	}

	INC_MEMORY_STAT_BY_FName(GetD3D12StatEnum(Desc.Flags).GetName(), TextureSize);
	INC_MEMORY_STAT_BY_FName(GetRHIStatEnum(Desc.Flags, bCubeMap, b3D).GetName(), TextureSize);
	INC_MEMORY_STAT_BY(STAT_D3D12MemoryCurrentTotal, TextureSize);

	if (TextureSize > 0)
	{
		INC_DWORD_STAT(STAT_D3D12TexturesAllocated);
	}
	else
	{
		INC_DWORD_STAT(STAT_D3D12TexturesReleased);
	}
}

template<typename BaseResourceType>
void FD3D12TextureStats::D3D12TextureAllocated(TD3D12Texture2D<BaseResourceType>& Texture, const D3D12_RESOURCE_DESC *Desc)
{
	FD3D12Resource* D3D12Texture2D = Texture.GetResource();

	if (D3D12Texture2D)
	{
		// Don't update state for virtual or transient textures	
		if (!EnumHasAnyFlags(Texture.Flags, TexCreate_Virtual) && !Texture.ResourceLocation.IsTransient())
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(D3D12RHI::UpdateTextureStats);

			if (!Desc)
			{
				Desc = &D3D12Texture2D->GetDesc();
			}

			const D3D12_RESOURCE_ALLOCATION_INFO AllocationInfo = Texture.GetParentDevice()->GetDevice()->GetResourceAllocationInfo(0, 1, Desc);
			const int64 TextureSize = AllocationInfo.SizeInBytes;

			Texture.SetMemorySize(TextureSize);

			UpdateD3D12TextureStats(Texture, *Desc, TextureSize, false, Texture.IsCubemap(), Texture.IsStreamable(), true);
		}
		else
		{
			Texture.SetMemorySize(Texture.ResourceLocation.GetSize());
		}

#if TEXTURE_PROFILER_ENABLED
		if (!EnumHasAnyFlags(Texture.GetFlags(), TexCreate_Virtual)
			&& !Texture.ResourceLocation.IsTransient()
			&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eAliased
			&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eHeapAliased)
		{
			size_t Size = Texture.GetMemorySize();
			uint32 Alignment = Desc->Alignment;
			FTextureProfiler::Get()->AddTextureAllocation(&Texture, Size, Alignment, 0);
		}
#endif
	}
}

template<typename BaseResourceType>
void FD3D12TextureStats::D3D12TextureDeleted(TD3D12Texture2D<BaseResourceType>& Texture)
{
	FD3D12Resource* D3D12Texture2D = Texture.GetResource();

	if (D3D12Texture2D)
	{
		// Don't update state for transient textures	
		if (!Texture.ResourceLocation.IsTransient())
		{
			const D3D12_RESOURCE_DESC& Desc = D3D12Texture2D->GetDesc();
			const int64 TextureSize = Texture.GetMemorySize();
			ensure(TextureSize > 0 || EnumHasAnyFlags(Texture.Flags, TexCreate_Virtual) || Texture.GetAliasingSourceTexture() != nullptr);

			UpdateD3D12TextureStats(Texture, Desc, -TextureSize, false, Texture.IsCubemap(), Texture.IsStreamable(), false);

#if TEXTURE_PROFILER_ENABLED
			if (!EnumHasAnyFlags(Texture.GetFlags(), TexCreate_Virtual)
				&& !Texture.ResourceLocation.IsTransient()
				&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eAliased
				&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eHeapAliased)
			{
				FTextureProfiler::Get()->RemoveTextureAllocation(&Texture);
			}
#endif
		}
	}
}

void FD3D12TextureStats::D3D12TextureAllocated2D(FD3D12Texture2D& Texture)
{
	D3D12TextureAllocated(Texture);
}

void FD3D12TextureStats::D3D12TextureAllocated(FD3D12Texture3D& Texture)
{
	FD3D12Resource* D3D12Texture3D = Texture.GetResource();

	if (D3D12Texture3D)
	{
		const D3D12_RESOURCE_DESC& Desc = D3D12Texture3D->GetDesc();
		// Don't update state for virtual or transient textures	
		if (!EnumHasAnyFlags(Texture.GetFlags(), TexCreate_Virtual) && !Texture.ResourceLocation.IsTransient())
		{
			const D3D12_RESOURCE_ALLOCATION_INFO AllocationInfo = Texture.GetParentDevice()->GetDevice()->GetResourceAllocationInfo(0, 1, &Desc);
			const int64 TextureSize = AllocationInfo.SizeInBytes;

			Texture.SetMemorySize(TextureSize);

			UpdateD3D12TextureStats(Texture, Desc, TextureSize, true, false, Texture.IsStreamable(), true);
		}
		else
		{
			Texture.SetMemorySize(Texture.ResourceLocation.GetSize());
		}

#if TEXTURE_PROFILER_ENABLED
		if (!EnumHasAnyFlags(Texture.GetFlags(), TexCreate_Virtual)
			&& !Texture.ResourceLocation.IsTransient()
			&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eAliased
			&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eHeapAliased)
		{
			size_t Size = Texture.GetMemorySize();
			uint32 Alignment = Desc.Alignment;
			FTextureProfiler::Get()->AddTextureAllocation(&Texture, Size, Alignment, 0);
		}
#endif
	}
}

void FD3D12TextureStats::D3D12TextureDeleted(FD3D12Texture3D& Texture)
{
	FD3D12Resource* D3D12Texture3D = Texture.GetResource();

	if (D3D12Texture3D)
	{
		// Don't update state for transient textures	
		if (!Texture.ResourceLocation.IsTransient())
		{
			const D3D12_RESOURCE_DESC& Desc = D3D12Texture3D->GetDesc();
			const int64 TextureSize = Texture.GetMemorySize();
			if (TextureSize > 0)
			{
				UpdateD3D12TextureStats(Texture, Desc, -TextureSize, true, false, Texture.IsStreamable(), false);
			}

#if TEXTURE_PROFILER_ENABLED
			if (!EnumHasAnyFlags(Texture.GetFlags(), TexCreate_Virtual)
				&& !Texture.ResourceLocation.IsTransient()
				&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eAliased
				&& Texture.ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eHeapAliased)
			{
				FTextureProfiler::Get()->RemoveTextureAllocation(&Texture);
			}
#endif
		}
	}
}

using namespace D3D12RHI;

template<typename BaseResourceType>
TD3D12Texture2D<BaseResourceType>::~TD3D12Texture2D()
{
	if (IsHeadLink())
	{
		// Only call this once for a LDA chain
		FD3D12TextureStats::D3D12TextureDeleted(*this);
	}
#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
	GetParentDevice()->GetOwningRHI()->DestroyVirtualTexture(BaseResourceType::GetFlags(), GetRawTextureMemory(), GetRawTextureBlock(), GetMemorySize());
#endif
}

FD3D12Texture3D::~FD3D12Texture3D()
{
	if (IsHeadLink())
	{
		// Only call this once for a LDA chain
		FD3D12TextureStats::D3D12TextureDeleted(*this);
	}
}

uint64 FD3D12DynamicRHI::RHICalcTexture2DPlatformSize(uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, const FRHIResourceCreateInfo& CreateInfo, uint32& OutAlign)
{
	D3D12_RESOURCE_DESC Desc = {};
	Desc.DepthOrArraySize = 1;
	Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	Desc.Format = (DXGI_FORMAT)GPixelFormats[Format].PlatformFormat;
	Desc.Height = SizeY;
	Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	Desc.MipLevels = NumMips;
	Desc.SampleDesc.Count = NumSamples;
	Desc.Width = SizeX;

	// Check if the 4K aligment is possible
	Desc.Alignment = TextureCanBe4KAligned(Desc, (EPixelFormat)Format) ? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT : 0;

	const D3D12_RESOURCE_ALLOCATION_INFO AllocationInfo = GetAdapter().GetD3DDevice()->GetResourceAllocationInfo(0, 1, &Desc);
	OutAlign = static_cast<uint32>(AllocationInfo.Alignment);

	return AllocationInfo.SizeInBytes;
}

uint64 FD3D12DynamicRHI::RHICalcTexture2DArrayPlatformSize(uint32 SizeX, uint32 SizeY, uint32 ArraySize, uint8 Format, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, const FRHIResourceCreateInfo& CreateInfo, uint32& OutAlign)
{
	D3D12_RESOURCE_DESC Desc = {};
	Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	Desc.Format = (DXGI_FORMAT)GPixelFormats[Format].PlatformFormat;
	Desc.Height = SizeY;
	Desc.DepthOrArraySize = ArraySize;
	Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	Desc.MipLevels = NumMips;
	Desc.SampleDesc.Count = NumSamples;
	Desc.Width = SizeX;

	// Check if the 4K aligment is possible
	Desc.Alignment = TextureCanBe4KAligned(Desc, (EPixelFormat)Format) ? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT : 0;

	const D3D12_RESOURCE_ALLOCATION_INFO AllocationInfo = GetAdapter().GetD3DDevice()->GetResourceAllocationInfo(0, 1, &Desc);
	OutAlign = static_cast<uint32>(AllocationInfo.Alignment);

	return AllocationInfo.SizeInBytes;
}

uint64 FD3D12DynamicRHI::RHICalcTexture3DPlatformSize(uint32 SizeX, uint32 SizeY, uint32 SizeZ, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, const FRHIResourceCreateInfo& CreateInfo, uint32& OutAlign)
{
	D3D12_RESOURCE_DESC Desc = {};
	Desc.DepthOrArraySize = SizeZ;
	Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
	Desc.Format = (DXGI_FORMAT)GPixelFormats[Format].PlatformFormat;
	Desc.Height = SizeY;
	Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	Desc.MipLevels = NumMips;
	Desc.SampleDesc.Count = 1;
	Desc.Width = SizeX;

	// Check if the 4K aligment is possible
	Desc.Alignment = TextureCanBe4KAligned(Desc, (EPixelFormat)Format) ? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT : 0;

	const D3D12_RESOURCE_ALLOCATION_INFO AllocationInfo = GetAdapter().GetD3DDevice()->GetResourceAllocationInfo(0, 1, &Desc);
	OutAlign = static_cast<uint32>(AllocationInfo.Alignment);

	return AllocationInfo.SizeInBytes;
}

uint64 FD3D12DynamicRHI::RHICalcTextureCubePlatformSize(uint32 Size, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, const FRHIResourceCreateInfo& CreateInfo, uint32& OutAlign)
{
	D3D12_RESOURCE_DESC Desc = {};
	Desc.DepthOrArraySize = 6;
	Desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	Desc.Format = (DXGI_FORMAT)GPixelFormats[Format].PlatformFormat;
	Desc.Height = Size;
	Desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	Desc.MipLevels = NumMips;
	Desc.SampleDesc.Count = 1;
	Desc.Width = Size;

	// Check if the 4K aligment is possible
	Desc.Alignment = TextureCanBe4KAligned(Desc, (EPixelFormat)Format) ? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT : 0;

	const D3D12_RESOURCE_ALLOCATION_INFO AllocationInfo = GetAdapter().GetD3DDevice()->GetResourceAllocationInfo(0, 1, &Desc);
	OutAlign = static_cast<uint32>(AllocationInfo.Alignment);

	return AllocationInfo.SizeInBytes;
}

/**
 * Retrieves texture memory stats.
 */
void FD3D12DynamicRHI::RHIGetTextureMemoryStats(FTextureMemoryStats& OutStats)
{
	OutStats.DedicatedVideoMemory = FD3D12GlobalStats::GDedicatedVideoMemory;
	OutStats.DedicatedSystemMemory = FD3D12GlobalStats::GDedicatedSystemMemory;
	OutStats.SharedSystemMemory = FD3D12GlobalStats::GSharedSystemMemory;
	OutStats.TotalGraphicsMemory = FD3D12GlobalStats::GTotalGraphicsMemory ? FD3D12GlobalStats::GTotalGraphicsMemory : -1;

	OutStats.AllocatedMemorySize = int64(GCurrentTextureMemorySize) * 1024;
	OutStats.LargestContiguousAllocation = OutStats.AllocatedMemorySize;
	OutStats.TexturePoolSize = GTexturePoolSize;
	OutStats.PendingMemoryAdjustment = 0;

#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
	if (GAdjustTexturePoolSizeBasedOnBudget)
	{
		GetAdapter().UpdateMemoryInfo();
		const DXGI_QUERY_VIDEO_MEMORY_INFO& LocalVideoMemoryInfo = GetAdapter().GetMemoryInfo().LocalMemoryInfo;

		// Applications must explicitly manage their usage of physical memory and keep usage within the budget 
		// assigned to the application process. Processes that cannot keep their usage within their assigned budgets 
		// will likely experience stuttering, as they are intermittently frozen and paged out to allow other processes to run.
		const int64 TargetBudget = LocalVideoMemoryInfo.Budget * 0.90f;	// Target using 90% of our budget to account for some fragmentation.
		OutStats.TotalGraphicsMemory = TargetBudget;

		const int64 BudgetPadding = TargetBudget * 0.05f;
		const int64 AvailableSpace = TargetBudget - int64(LocalVideoMemoryInfo.CurrentUsage);	// Note: AvailableSpace can be negative
		const int64 PreviousTexturePoolSize = RequestedTexturePoolSize;
		const bool bOverbudget = AvailableSpace < 0;

		// Only change the pool size if overbudget, or a reasonable amount of memory is available
		const int64 MinTexturePoolSize = int64(100 * 1024 * 1024);
		if (bOverbudget)
		{
			// Attempt to lower the texture pool size to meet the budget.
			const bool bOverActualBudget = LocalVideoMemoryInfo.CurrentUsage > LocalVideoMemoryInfo.Budget;
			UE_CLOG(bOverActualBudget, LogD3D12RHI, Warning,
				TEXT("Video memory usage is overbudget by %llu MB (using %lld MB/%lld MB budget). Usage breakdown: %lld MB (Textures), %lld MB (Render targets). Last requested texture pool size is %lld MB. This can cause stuttering due to paging."),
				(LocalVideoMemoryInfo.CurrentUsage - LocalVideoMemoryInfo.Budget) / 1024ll / 1024ll,
				LocalVideoMemoryInfo.CurrentUsage / 1024ll / 1024ll,
				LocalVideoMemoryInfo.Budget / 1024ll / 1024ll,
				GCurrentTextureMemorySize / 1024ll,
				GCurrentRendertargetMemorySize / 1024ll,
				PreviousTexturePoolSize / 1024ll / 1024ll);

			const int64 DesiredTexturePoolSize = PreviousTexturePoolSize + AvailableSpace - BudgetPadding;
			OutStats.TexturePoolSize = FMath::Max(DesiredTexturePoolSize, MinTexturePoolSize);

			UE_CLOG(bOverActualBudget && (OutStats.TexturePoolSize >= PreviousTexturePoolSize) && (OutStats.TexturePoolSize > MinTexturePoolSize), LogD3D12RHI, Fatal,
				TEXT("Video memory usage is overbudget by %llu MB and the texture pool size didn't shrink."),
				(LocalVideoMemoryInfo.CurrentUsage - LocalVideoMemoryInfo.Budget) / 1024ll / 1024ll);
		}
		else if (AvailableSpace > BudgetPadding)
		{
			// Increase the texture pool size to improve quality if we have a reasonable amount of memory available.
			int64 DesiredTexturePoolSize = PreviousTexturePoolSize + AvailableSpace - BudgetPadding;
			if (GPoolSizeVRAMPercentage > 0)
			{
				// The texture pool size is a percentage of GTotalGraphicsMemory.
				const float PoolSize = float(GPoolSizeVRAMPercentage) * 0.01f * float(OutStats.TotalGraphicsMemory);

				// Truncate texture pool size to MB (but still counted in bytes).
				DesiredTexturePoolSize = int64(FGenericPlatformMath::TruncToFloat(PoolSize / 1024.0f / 1024.0f)) * 1024 * 1024;
			}

			// Make sure the desired texture pool size doesn't make us go overbudget.
			const bool bIsLimitedTexturePoolSize = GTexturePoolSize > 0;
			const int64 LimitedMaxTexturePoolSize = bIsLimitedTexturePoolSize ? GTexturePoolSize : INT64_MAX;
			const int64 MaxTexturePoolSize = FMath::Min(PreviousTexturePoolSize + AvailableSpace - BudgetPadding, LimitedMaxTexturePoolSize);	// Max texture pool size without going overbudget or the pre-defined max.
			OutStats.TexturePoolSize = FMath::Min(DesiredTexturePoolSize, MaxTexturePoolSize);
		}
		else
		{
			// Keep the previous requested texture pool size.
			OutStats.TexturePoolSize = PreviousTexturePoolSize;
		}

		check(OutStats.TexturePoolSize >= MinTexturePoolSize);
	}

	// Cache the last requested texture pool size.
	RequestedTexturePoolSize = OutStats.TexturePoolSize;
#endif // PLATFORM_WINDOWS || PLATFORM_HOLOLENS
}

/**
 * Fills a texture with to visualize the texture pool memory.
 *
 * @param	TextureData		Start address
 * @param	SizeX			Number of pixels along X
 * @param	SizeY			Number of pixels along Y
 * @param	Pitch			Number of bytes between each row
 * @param	PixelSize		Number of bytes each pixel represents
 *
 * @return true if successful, false otherwise
 */
bool FD3D12DynamicRHI::RHIGetTextureMemoryVisualizeData(FColor* /*TextureData*/, int32 /*SizeX*/, int32 /*SizeY*/, int32 /*Pitch*/, int32 /*PixelSize*/)
{
	// currently only implemented for console (Note: Keep this function for further extension. Talk to NiklasS for more info.)
	return false;
}

/**
 * Creates a 2D texture optionally guarded by a structured exception handler.
 */
void SafeCreateTexture2D(FD3D12Device* pDevice, 
	FD3D12Adapter* Adapter,
	const FD3D12ResourceDesc& TextureDesc,
	const D3D12_CLEAR_VALUE* ClearValue, 
	FD3D12ResourceLocation* OutTexture2D, 
	FD3D12BaseShaderResource* Owner,
	EPixelFormat Format,
	ETextureCreateFlags Flags,
	D3D12_RESOURCE_STATES InitialState,
	const TCHAR* Name)
{

#if GUARDED_TEXTURE_CREATES
	bool bDriverCrash = true;
	__try
	{
#endif // #if GUARDED_TEXTURE_CREATES

		const D3D12_HEAP_TYPE HeapType = EnumHasAnyFlags(Flags, TexCreate_CPUReadback) ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT;

		switch (HeapType)
		{
		case D3D12_HEAP_TYPE_READBACK:
			{
				uint64 Size = 0;
				pDevice->GetDevice()->GetCopyableFootprints(&TextureDesc, 0, TextureDesc.MipLevels * TextureDesc.DepthOrArraySize, 0, nullptr, nullptr, nullptr, &Size);

				FD3D12Resource* Resource = nullptr;
				VERIFYD3D12CREATETEXTURERESULT(Adapter->CreateBuffer(HeapType, pDevice->GetGPUMask(), pDevice->GetVisibilityMask(), Size, &Resource, Name), TextureDesc, pDevice->GetDevice());
				OutTexture2D->AsStandAlone(Resource);
			}
			break;

		case D3D12_HEAP_TYPE_DEFAULT:
		{
			VERIFYD3D12CREATETEXTURERESULT(pDevice->GetTextureAllocator().AllocateTexture(TextureDesc, ClearValue, Format, *OutTexture2D, InitialState, Name), TextureDesc, pDevice->GetDevice());
			OutTexture2D->SetOwner(Owner);
			break;
		}

		default:
			check(false);	// Need to create a resource here
		}

#if GUARDED_TEXTURE_CREATES
		bDriverCrash = false;
	}
	__finally
	{
		if (bDriverCrash)
		{
			UE_LOG(LogD3D12RHI, Error,
				TEXT("Driver crashed while creating texture: %ux%ux%u %s(0x%08x) with %u mips"),
				TextureDesc.Width,
				TextureDesc.Height,
				TextureDesc.DepthOrArraySize,
				GetD3D12TextureFormatString(TextureDesc.Format),
				(uint32)TextureDesc.Format,
				TextureDesc.MipLevels
				);
		}
	}
#endif // #if GUARDED_TEXTURE_CREATES
}

void CreateUAVAliasResource(FD3D12Adapter* Adapter, D3D12_CLEAR_VALUE* ClearValuePtr, const TCHAR* DebugName, FD3D12ResourceLocation& Location)
{
	FD3D12Resource* SourceResource = Location.GetResource();

	const FD3D12ResourceDesc& SourceDesc = SourceResource->GetDesc();
	const FD3D12Heap* const ResourceHeap = SourceResource->GetHeap();

	const EPixelFormat SourceFormat = SourceDesc.PixelFormat;
	const EPixelFormat AliasTextureFormat = SourceDesc.UAVAliasPixelFormat;

	if (ensure(ResourceHeap != nullptr) && ensure(SourceFormat != PF_Unknown) && SourceFormat != AliasTextureFormat)
	{
		const uint64 SourceOffset = Location.GetOffsetFromBaseOfResource();

		FD3D12ResourceDesc AliasTextureDesc = SourceDesc;
		AliasTextureDesc.Format = (DXGI_FORMAT)GPixelFormats[AliasTextureFormat].PlatformFormat;
		AliasTextureDesc.Width = SourceDesc.Width / GPixelFormats[SourceFormat].BlockSizeX;
		AliasTextureDesc.Height = SourceDesc.Height / GPixelFormats[SourceFormat].BlockSizeY;
		// layout of UAV must match source resource
		AliasTextureDesc.Layout = SourceResource->GetResource()->GetDesc().Layout;

		EnumAddFlags(AliasTextureDesc.Flags, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);

		AliasTextureDesc.UAVAliasPixelFormat = PF_Unknown;

		TRefCountPtr<ID3D12Resource> pAliasResource;
		HRESULT AliasHR = Adapter->GetD3DDevice()->CreatePlacedResource(
			ResourceHeap->GetHeap(),
			SourceOffset,
			&AliasTextureDesc,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
			ClearValuePtr,
			IID_PPV_ARGS(pAliasResource.GetInitReference()));

		if (pAliasResource && DebugName)
		{
			TCHAR NameBuffer[512]{};
			FCString::Snprintf(NameBuffer, UE_ARRAY_COUNT(NameBuffer), TEXT("%s UAVAlias"), DebugName);
			SetName(pAliasResource, NameBuffer);
		}

		if (SUCCEEDED(AliasHR))
		{
			SourceResource->SetUAVAccessResource(pAliasResource);
		}
	}
}

static void DetermineTexture2DResourceFlagsAndLayout(uint32 SizeX, uint32 SizeY, uint32 SizeZ, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, EPixelFormat Format, D3D12_RESOURCE_FLAGS& OutResourceFlags, D3D12_TEXTURE_LAYOUT& OutLayout, bool& bOutCreateRTV, bool& bOutCreateDSV, bool& bOutCreateSRV)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(DetermineTexture2DResourceFlagsAndLayout);

	OutResourceFlags = D3D12_RESOURCE_FLAG_NONE;
	OutLayout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	bOutCreateRTV = false;
	bOutCreateDSV = false;
	bOutCreateSRV = true;

	if (EnumHasAllFlags(Flags, TexCreate_CPUReadback))
	{
		check(!EnumHasAnyFlags(Flags, TexCreate_RenderTargetable | TexCreate_DepthStencilTargetable | TexCreate_ShaderResource));
		bOutCreateSRV = false;
	}

	if (EnumHasAnyFlags(Flags, TexCreate_DisableSRVCreation))
	{
		bOutCreateSRV = false;
	}

	if (EnumHasAnyFlags(Flags, TexCreate_Shared))
	{
		OutResourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
	}

	if (EnumHasAnyFlags(Flags, TexCreate_RenderTargetable))
	{
		check(!EnumHasAnyFlags(Flags, TexCreate_DepthStencilTargetable | TexCreate_ResolveTargetable));
		OutResourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		bOutCreateRTV = true;
	}
	else if (EnumHasAnyFlags(Flags, TexCreate_DepthStencilTargetable))
	{
		check(!EnumHasAnyFlags(Flags, TexCreate_RenderTargetable | TexCreate_ResolveTargetable));
		OutResourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		bOutCreateDSV = true;
	}
	else if (EnumHasAnyFlags(Flags, TexCreate_ResolveTargetable))
	{
		check(!EnumHasAnyFlags(Flags, TexCreate_RenderTargetable | TexCreate_DepthStencilTargetable));
		if (Format == PF_DepthStencil || Format == PF_ShadowDepth || Format == PF_D24)
		{
			OutResourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			bOutCreateDSV = true;
		}
		else
		{
			OutResourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			bOutCreateRTV = true;
		}
	}

	if (EnumHasAnyFlags(Flags, TexCreate_UAV))
	{
		OutResourceFlags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}

	if (bOutCreateDSV && !EnumHasAnyFlags(Flags, TexCreate_ShaderResource))
	{
		// Only deny shader resources if it's a depth resource that will never be used as SRV
		OutResourceFlags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
		bOutCreateSRV = false;
	}
}

template<typename BaseResourceType>
TD3D12Texture2D<BaseResourceType>* FD3D12DynamicRHI::CreateD3D12Texture2D(FRHICommandListImmediate* RHICmdList, uint32 SizeX, uint32 SizeY, uint32 SizeZ, bool bTextureArray, bool bCubeTexture, EPixelFormat Format,
	uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo, ED3D12ResourceTransientMode TransientMode, ID3D12ResourceAllocator* ResourceAllocator)
{
#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
	
	TRACE_CPUPROFILER_EVENT_SCOPE(D3D12RHI::CreateD3D12Texture2D);

	check(SizeX > 0 && SizeY > 0 && NumMips > 0);

	if (bCubeTexture)
	{
		check(SizeX <= GetMaxCubeTextureDimension());
		check(SizeX == SizeY);
	}
	else
	{
		check(SizeX <= GetMax2DTextureDimension());
		check(SizeY <= GetMax2DTextureDimension());
	}

	if (bTextureArray)
	{
		check(SizeZ > 0 && SizeZ <= GetMaxTextureArrayLayers());
	}

	SCOPE_CYCLE_COUNTER(STAT_D3D12CreateTextureTime);

	const bool bSRGB = EnumHasAnyFlags(Flags, TexCreate_SRGB);

	const DXGI_FORMAT PlatformResourceFormat = GetPlatformTextureResourceFormat((DXGI_FORMAT)GPixelFormats[Format].PlatformFormat, Flags);
	const DXGI_FORMAT PlatformShaderResourceFormat = FindShaderResourceDXGIFormat(PlatformResourceFormat, bSRGB);
	const DXGI_FORMAT PlatformRenderTargetFormat = FindShaderResourceDXGIFormat(PlatformResourceFormat, bSRGB);
	const DXGI_FORMAT PlatformDepthStencilFormat = FindDepthStencilDXGIFormat(PlatformResourceFormat);

	uint32 ActualMSAACount = NumSamples;
	uint32 ActualMSAAQuality = GetMaxMSAAQuality(ActualMSAACount);

	// 0xffffffff means not supported
	if (ActualMSAAQuality == 0xffffffff || EnumHasAnyFlags(Flags, TexCreate_Shared))
	{
		// no MSAA
		ActualMSAACount = 1;
		ActualMSAAQuality = 0;
	}
	const bool bIsMultisampled = ActualMSAACount > 1;

	// Describe the texture.
	FD3D12ResourceDesc TextureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		PlatformResourceFormat,
		SizeX,
		SizeY,
		SizeZ,  // Array size
		NumMips,
		ActualMSAACount,
		ActualMSAAQuality,
		D3D12_RESOURCE_FLAG_NONE);  // Add misc flags later

	TextureDesc.PixelFormat = Format;

	bool bBCTextureNeedsUAVAlias = EnumHasAnyFlags(Flags, TexCreate_UAV) && IsBlockCompressedFormat(Format);
	if (bBCTextureNeedsUAVAlias)
	{
		EnumRemoveFlags(Flags, TexCreate_UAV);
		TextureDesc.UAVAliasPixelFormat = GetBlockCompressedFormatUAVAliasFormat(Format);
	}

#if D3D12RHI_NEEDS_VENDOR_EXTENSIONS
	TextureDesc.bRequires64BitAtomicSupport = EnumHasAnyFlags(Flags, ETextureCreateFlags::Atomic64Compatible);
#endif

	// Set up the texture bind flags.
	bool bCreateRTV;
	bool bCreateDSV;
	bool bCreateShaderResource;
	DetermineTexture2DResourceFlagsAndLayout(SizeX, SizeY, SizeZ, NumMips, ActualMSAACount, Flags, Format, TextureDesc.Flags, TextureDesc.Layout, bCreateRTV, bCreateDSV, bCreateShaderResource);

	// Virtual textures currently not supported in default D3D12
	Flags &= ~TexCreate_Virtual;

	FD3D12Adapter* Adapter = &GetAdapter();

	D3D12_CLEAR_VALUE *ClearValuePtr = nullptr;
	D3D12_CLEAR_VALUE ClearValue;
	if (bCreateDSV && CreateInfo.ClearValueBinding.ColorBinding == EClearBinding::EDepthStencilBound)
	{
		ClearValue = CD3DX12_CLEAR_VALUE(PlatformDepthStencilFormat, CreateInfo.ClearValueBinding.Value.DSValue.Depth, (uint8)CreateInfo.ClearValueBinding.Value.DSValue.Stencil);
		ClearValuePtr = &ClearValue;
	}
	else if (bCreateRTV && CreateInfo.ClearValueBinding.ColorBinding == EClearBinding::EColorBound)
	{
		ClearValue = CD3DX12_CLEAR_VALUE(PlatformRenderTargetFormat, CreateInfo.ClearValueBinding.Value.Color);
		ClearValuePtr = &ClearValue;
	}

	if (Format == PF_NV12)
	{
		bCreateRTV = false;
		bCreateShaderResource = false;
	}

	// The state this resource will be in when it leaves this function
	const FD3D12Resource::FD3D12ResourceTypeHelper Type(TextureDesc, D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_STATES InitialState = Type.GetOptimalInitialState(InResourceState, false);
	const D3D12_RESOURCE_STATES CreateState = (CreateInfo.BulkData != nullptr) ? D3D12_RESOURCE_STATE_COPY_DEST : InitialState;

	TD3D12Texture2D<BaseResourceType>* D3D12TextureOut = Adapter->CreateLinkedObject<TD3D12Texture2D<BaseResourceType>>(CreateInfo.GPUMask, [&](FD3D12Device* Device)
	{
		TD3D12Texture2D<BaseResourceType>* NewTexture = new TD3D12Texture2D<BaseResourceType>(Device,
			SizeX,
			SizeY,
			SizeZ,
			NumMips,
			ActualMSAACount,
			(EPixelFormat)Format,
			bCubeTexture,
			Flags,
			CreateInfo.ClearValueBinding);		

#if NAME_OBJECTS
		if (CreateInfo.DebugName)
		{
			TRACE_CPUPROFILER_EVENT_SCOPE(D3D12RHI::SetDebugName);
			NewTexture->SetName(CreateInfo.DebugName);
		}
#endif // NAME_OBJECTS

		FD3D12ResourceLocation& Location = NewTexture->ResourceLocation;

		if (ResourceAllocator)
		{
			const D3D12_HEAP_TYPE HeapType = EnumHasAnyFlags(Flags, TexCreate_CPUReadback) ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT;
			ResourceAllocator->AllocateTexture(Device->GetGPUIndex(), HeapType, TextureDesc, (EPixelFormat)Format, ED3D12ResourceStateMode::Default, CreateState, ClearValuePtr, CreateInfo.DebugName, Location);
			Location.SetOwner(NewTexture);
		}
		else
		{
			SafeCreateTexture2D(Device,
				Adapter,
				TextureDesc,
				ClearValuePtr,
				&Location,
				NewTexture,
				Format,
				Flags,
				CreateState,
				CreateInfo.DebugName);
		}

		// Unlock immediately if no initial data
		if (CreateInfo.BulkData == nullptr)
		{
			Location.UnlockPoolData();
		}

		check(Location.IsValid());

		if (bBCTextureNeedsUAVAlias)
		{
			CreateUAVAliasResource(Adapter, ClearValuePtr, CreateInfo.DebugName, Location);
		}

		uint32 RTVIndex = 0;

		{
			TRACE_CPUPROFILER_EVENT_SCOPE(D3D12RHI::CreateViews);
			if (bCreateRTV)
			{
				const bool bCreateRTVsPerSlice = EnumHasAnyFlags(Flags, TexCreate_TargetArraySlicesIndependently) && (bTextureArray || bCubeTexture);
				NewTexture->SetNumRenderTargetViews(bCreateRTVsPerSlice ? NumMips * TextureDesc.DepthOrArraySize : NumMips);

				// Create a render target view for each mip
				for (uint32 MipIndex = 0; MipIndex < NumMips; MipIndex++)
				{
					if (bCreateRTVsPerSlice)
					{
						NewTexture->SetCreatedRTVsPerSlice(true, TextureDesc.DepthOrArraySize);

						for (uint32 SliceIndex = 0; SliceIndex < TextureDesc.DepthOrArraySize; SliceIndex++)
						{
							D3D12_RENDER_TARGET_VIEW_DESC RTVDesc;
							FMemory::Memzero(RTVDesc);

							RTVDesc.Format = PlatformRenderTargetFormat;
							RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
							RTVDesc.Texture2DArray.FirstArraySlice = SliceIndex;
							RTVDesc.Texture2DArray.ArraySize = 1;
							RTVDesc.Texture2DArray.MipSlice = MipIndex;
							RTVDesc.Texture2DArray.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, RTVDesc.Format);

							NewTexture->SetRenderTargetViewIndex(new FD3D12RenderTargetView(Device, RTVDesc, NewTexture), RTVIndex++);
						}
					}
					else
					{
						D3D12_RENDER_TARGET_VIEW_DESC RTVDesc;
						FMemory::Memzero(RTVDesc);

						RTVDesc.Format = PlatformRenderTargetFormat;

						if (bTextureArray || bCubeTexture)
						{
							if (bIsMultisampled)
							{
								RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
								RTVDesc.Texture2DMSArray.FirstArraySlice = 0;
								RTVDesc.Texture2DMSArray.ArraySize = TextureDesc.DepthOrArraySize;
							}
							else
							{
								RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
								RTVDesc.Texture2DArray.FirstArraySlice = 0;
								RTVDesc.Texture2DArray.ArraySize = TextureDesc.DepthOrArraySize;
								RTVDesc.Texture2DArray.MipSlice = MipIndex;
								RTVDesc.Texture2DArray.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, RTVDesc.Format);
							}
						}
						else
						{
							if (bIsMultisampled)
							{
								RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
								// Nothing to set
							}
							else
							{
								RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
								RTVDesc.Texture2D.MipSlice = MipIndex;
								RTVDesc.Texture2D.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, RTVDesc.Format);
							}
						}

						NewTexture->SetRenderTargetViewIndex(new FD3D12RenderTargetView(Device, RTVDesc, NewTexture), RTVIndex++);
					}
				}
			}

			if (bCreateDSV)
			{
				// Create a depth-stencil-view for the texture.
				D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc = {};
				DSVDesc.Format = FindDepthStencilDXGIFormat(PlatformResourceFormat);
				if (bTextureArray || bCubeTexture)
				{
					if (bIsMultisampled)
					{
						DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMSARRAY;
						DSVDesc.Texture2DMSArray.FirstArraySlice = 0;
						DSVDesc.Texture2DMSArray.ArraySize = TextureDesc.DepthOrArraySize;
					}
					else
					{
						DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
						DSVDesc.Texture2DArray.FirstArraySlice = 0;
						DSVDesc.Texture2DArray.ArraySize = TextureDesc.DepthOrArraySize;
						DSVDesc.Texture2DArray.MipSlice = 0;
					}
				}
				else
				{
					if (bIsMultisampled)
					{
						DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
						// Nothing to set
					}
					else
					{
						DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
						DSVDesc.Texture2D.MipSlice = 0;
					}
				}

				const bool HasStencil = HasStencilBits(DSVDesc.Format);
				for (uint32 AccessType = 0; AccessType < FExclusiveDepthStencil::MaxIndex; ++AccessType)
				{
					// Create a read-only access views for the texture.
					DSVDesc.Flags = (AccessType & FExclusiveDepthStencil::DepthRead_StencilWrite) ? D3D12_DSV_FLAG_READ_ONLY_DEPTH : D3D12_DSV_FLAG_NONE;
					if (HasStencil)
					{
						DSVDesc.Flags |= (AccessType & FExclusiveDepthStencil::DepthWrite_StencilRead) ? D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_NONE;
					}

					NewTexture->SetDepthStencilView(new FD3D12DepthStencilView(Device, DSVDesc, NewTexture, HasStencil), AccessType);
				}
			}

			// Create a shader resource view for the texture.
			if (bCreateShaderResource)
			{
				D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
				SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
				SRVDesc.Format = PlatformShaderResourceFormat;

				if (bCubeTexture && bTextureArray)
				{
					SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
					SRVDesc.TextureCubeArray.MostDetailedMip = 0;
					SRVDesc.TextureCubeArray.MipLevels = NumMips;
					SRVDesc.TextureCubeArray.First2DArrayFace = 0;
					SRVDesc.TextureCubeArray.NumCubes = SizeZ / 6;
				}
				else if (bCubeTexture)
				{
					SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
					SRVDesc.TextureCube.MostDetailedMip = 0;
					SRVDesc.TextureCube.MipLevels = NumMips;
				}
				else if (bTextureArray)
				{
					if (bIsMultisampled)
					{
						SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
						SRVDesc.Texture2DMSArray.FirstArraySlice = 0;
						SRVDesc.Texture2DMSArray.ArraySize = TextureDesc.DepthOrArraySize;
						//SRVDesc.Texture2DArray.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, SRVDesc.Format);
					}
					else
					{
						SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
						SRVDesc.Texture2DArray.MostDetailedMip = 0;
						SRVDesc.Texture2DArray.MipLevels = NumMips;
						SRVDesc.Texture2DArray.FirstArraySlice = 0;
						SRVDesc.Texture2DArray.ArraySize = TextureDesc.DepthOrArraySize;
						SRVDesc.Texture2DArray.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, SRVDesc.Format);
					}
				}
				else
				{
					if (bIsMultisampled)
					{
						SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
						// Nothing to set
					}
					else
					{
						SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
						SRVDesc.Texture2D.MostDetailedMip = 0;
						SRVDesc.Texture2D.MipLevels = NumMips;
						SRVDesc.Texture2D.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, SRVDesc.Format);
					}
				}

				NewTexture->SetShaderResourceView(new FD3D12ShaderResourceView(Device, SRVDesc, NewTexture));
			}
		}

		return NewTexture;
	});

	FD3D12TextureStats::D3D12TextureAllocated(*D3D12TextureOut);

	// Initialize if data is given
	if (CreateInfo.BulkData != nullptr)
	{
		D3D12TextureOut->InitializeTextureData(RHICmdList, CreateInfo.BulkData->GetResourceBulkData(), CreateInfo.BulkData->GetResourceBulkDataSize(), SizeX, SizeY, 1, SizeZ, NumMips, Format, InitialState);

		CreateInfo.BulkData->Discard();
	}

	return D3D12TextureOut;
#else
	checkf(false, TEXT("XBOX_CODE_MERGE : Removed. The Xbox platform version should be used."));
	return nullptr;
#endif // PLATFORM_WINDOWS || PLATFORM_HOLOLENS
}

FD3D12Texture3D* FD3D12DynamicRHI::CreateD3D12Texture3D(FRHICommandListImmediate* RHICmdList, uint32 SizeX, uint32 SizeY, uint32 SizeZ, EPixelFormat Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo, ED3D12ResourceTransientMode TransientMode, ID3D12ResourceAllocator* ResourceAllocator)
{
#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
	SCOPE_CYCLE_COUNTER(STAT_D3D12CreateTextureTime);

	const bool bSRGB = EnumHasAnyFlags(Flags, TexCreate_SRGB);

	const DXGI_FORMAT PlatformResourceFormat = (DXGI_FORMAT)GPixelFormats[Format].PlatformFormat;
	const DXGI_FORMAT PlatformShaderResourceFormat = FindShaderResourceDXGIFormat(PlatformResourceFormat, bSRGB);
	const DXGI_FORMAT PlatformRenderTargetFormat = FindShaderResourceDXGIFormat(PlatformResourceFormat, bSRGB);

	// Describe the texture.
	D3D12_RESOURCE_DESC TextureDesc = CD3DX12_RESOURCE_DESC::Tex3D(
		PlatformResourceFormat,
		SizeX,
		SizeY,
		SizeZ,
		NumMips);

	if (EnumHasAnyFlags(Flags, TexCreate_UAV))
	{
		TextureDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}

	bool bCreateRTV = false;

	if (EnumHasAnyFlags(Flags, TexCreate_RenderTargetable))
	{
		TextureDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		bCreateRTV = true;
	}

	// Set up the texture bind flags.
	check(!EnumHasAnyFlags(Flags, TexCreate_DepthStencilTargetable | TexCreate_ResolveTargetable));
	check(EnumHasAllFlags(Flags, TexCreate_ShaderResource));

	D3D12_CLEAR_VALUE *ClearValuePtr = nullptr;
	D3D12_CLEAR_VALUE ClearValue;
	if (bCreateRTV && CreateInfo.ClearValueBinding.ColorBinding == EClearBinding::EColorBound)
	{
		ClearValue = CD3DX12_CLEAR_VALUE(PlatformRenderTargetFormat, CreateInfo.ClearValueBinding.Value.Color);
		ClearValuePtr = &ClearValue;
	}

	// The state this resource will be in when it leaves this function
	const FD3D12Resource::FD3D12ResourceTypeHelper Type(TextureDesc, D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_STATES InitialState = Type.GetOptimalInitialState(InResourceState, false);

	FD3D12Adapter* Adapter = &GetAdapter();
	FD3D12Texture3D* D3D12TextureOut = Adapter->CreateLinkedObject<FD3D12Texture3D>(CreateInfo.GPUMask, [&](FD3D12Device* Device)
	{
		FD3D12Texture3D* Texture3D = new FD3D12Texture3D(Device, SizeX, SizeY, SizeZ, NumMips, Format, Flags, CreateInfo.ClearValueBinding);

		if (CreateInfo.DebugName)
		{
			Texture3D->SetName(CreateInfo.DebugName);
		}

		if (ResourceAllocator)
		{
			ResourceAllocator->AllocateTexture(Device->GetGPUIndex(), D3D12_HEAP_TYPE_DEFAULT, TextureDesc, Format, ED3D12ResourceStateMode::Default, InitialState, ClearValuePtr, CreateInfo.DebugName, Texture3D->ResourceLocation);
		}
		else
		{
			VERIFYD3D12CREATETEXTURERESULT(Device->GetTextureAllocator().AllocateTexture(TextureDesc, ClearValuePtr, Format, Texture3D->ResourceLocation, (CreateInfo.BulkData != nullptr) ? D3D12_RESOURCE_STATE_COPY_DEST : InitialState, CreateInfo.DebugName), TextureDesc, Device->GetDevice());
		}
		Texture3D->ResourceLocation.SetOwner(Texture3D);

		// Unlock immediately if no initial data
		if (CreateInfo.BulkData == nullptr)
		{
			Texture3D->ResourceLocation.UnlockPoolData();
		}

		if (bCreateRTV)
		{
			// Create a render-target-view for the texture.
			D3D12_RENDER_TARGET_VIEW_DESC RTVDesc;
			FMemory::Memzero(&RTVDesc, sizeof(RTVDesc));
			RTVDesc.Format = PlatformRenderTargetFormat;
			RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
			RTVDesc.Texture3D.MipSlice = 0;
			RTVDesc.Texture3D.FirstWSlice = 0;
			RTVDesc.Texture3D.WSize = SizeZ;

			Texture3D->SetRenderTargetView(new FD3D12RenderTargetView(Device, RTVDesc, Texture3D));
		}

		// Create a shader resource view for the texture.
		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Format = PlatformShaderResourceFormat;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
		SRVDesc.Texture3D.MipLevels = NumMips;
		SRVDesc.Texture3D.MostDetailedMip = 0;

		Texture3D->SetShaderResourceView(new FD3D12ShaderResourceView(Device, SRVDesc, Texture3D));

		return Texture3D;
	}); 

	// Intialize if data given
	if (D3D12TextureOut)
	{
		if (CreateInfo.BulkData != nullptr)
		{
			D3D12TextureOut->InitializeTextureData(RHICmdList, CreateInfo.BulkData->GetResourceBulkData(), CreateInfo.BulkData->GetResourceBulkDataSize(), SizeX, SizeY, SizeZ, 1, NumMips, Format, InitialState);
		}

		FD3D12TextureStats::D3D12TextureAllocated(*D3D12TextureOut);
	}

	if (CreateInfo.BulkData != nullptr)
	{
		CreateInfo.BulkData->Discard();
	}

	return D3D12TextureOut;
#else
	checkf(false, TEXT("XBOX_CODE_MERGE : Removed. The Xbox platform version should be used."));
	return nullptr;
#endif // PLATFORM_WINDOWS || PLATFORM_HOLOLENS
}

FRHITexture* FD3D12DynamicRHI::CreateTexture(const FRHITextureCreateInfo& CreateInfo, const TCHAR* DebugName, ERHIAccess InitialState, ED3D12ResourceTransientMode TransientMode, ID3D12ResourceAllocator* ResourceAllocator)
{
	FRHIResourceCreateInfo ResourceCreateInfo(DebugName, CreateInfo.ClearValue);

	const bool bTextureArray = CreateInfo.IsTextureArray();
	const bool bTextureCube = CreateInfo.IsTextureCube();

	switch (CreateInfo.Dimension)
	{
	case ETextureDimension::Texture2D:
		return CreateD3D12Texture2D<FD3D12BaseTexture2D>(nullptr, CreateInfo.Extent.X, CreateInfo.Extent.Y, 1, bTextureArray, bTextureCube, CreateInfo.Format, CreateInfo.NumMips, CreateInfo.NumSamples, CreateInfo.Flags, InitialState, ResourceCreateInfo, TransientMode, ResourceAllocator);

	case ETextureDimension::Texture2DArray:
		return CreateD3D12Texture2D<FD3D12BaseTexture2DArray>(nullptr, CreateInfo.Extent.X, CreateInfo.Extent.Y, CreateInfo.ArraySize, bTextureArray, bTextureCube, CreateInfo.Format, CreateInfo.NumMips, CreateInfo.NumSamples, CreateInfo.Flags, InitialState, ResourceCreateInfo, TransientMode, ResourceAllocator);

	case ETextureDimension::TextureCube:
	case ETextureDimension::TextureCubeArray:
		return CreateD3D12Texture2D<FD3D12BaseTextureCube>(nullptr, CreateInfo.Extent.X, CreateInfo.Extent.Y, 6 * CreateInfo.ArraySize, bTextureArray, bTextureCube, CreateInfo.Format, CreateInfo.NumMips, CreateInfo.NumSamples, CreateInfo.Flags, InitialState, ResourceCreateInfo, TransientMode, ResourceAllocator);

	case ETextureDimension::Texture3D:
		return CreateD3D12Texture3D(nullptr, CreateInfo.Extent.X, CreateInfo.Extent.Y, CreateInfo.Depth, CreateInfo.Format, CreateInfo.NumMips, CreateInfo.Flags, InitialState, ResourceCreateInfo, TransientMode, ResourceAllocator);

	default:
		checkNoEntry();
	}

	return nullptr;
}

/*-----------------------------------------------------------------------------
	2D texture support.
	-----------------------------------------------------------------------------*/

FTexture2DRHIRef FD3D12DynamicRHI::RHICreateTexture2D_RenderThread(class FRHICommandListImmediate& RHICmdList, uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	return CreateD3D12Texture2D<FD3D12BaseTexture2D>(&RHICmdList, SizeX, SizeY, 1, false, false, (EPixelFormat) Format, NumMips, NumSamples, Flags, InResourceState, CreateInfo);
}

FTexture2DRHIRef FD3D12DynamicRHI::RHICreateTexture2D(uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	return CreateD3D12Texture2D<FD3D12BaseTexture2D>(nullptr, SizeX, SizeY, 1, false, false, (EPixelFormat) Format, NumMips, NumSamples, Flags, InResourceState, CreateInfo);
}

FTexture2DRHIRef FD3D12DynamicRHI::RHIAsyncCreateTexture2D(uint32 SizeX, uint32 SizeY, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, void** InitialMipData, uint32 NumInitialMips)
{
	check(GRHISupportsAsyncTextureCreation);
	
	const ETextureCreateFlags InvalidFlags = TexCreate_RenderTargetable | TexCreate_ResolveTargetable | TexCreate_DepthStencilTargetable | TexCreate_GenerateMipCapable | TexCreate_UAV | TexCreate_Presentable | TexCreate_CPUReadback;
	check(!EnumHasAnyFlags(Flags, InvalidFlags));

	const DXGI_FORMAT PlatformResourceFormat = (DXGI_FORMAT)GPixelFormats[Format].PlatformFormat;
	const DXGI_FORMAT PlatformShaderResourceFormat = FindShaderResourceDXGIFormat(PlatformResourceFormat, EnumHasAnyFlags(Flags, TexCreate_SRGB));
	const D3D12_RESOURCE_DESC TextureDesc = CD3DX12_RESOURCE_DESC::Tex2D(
		PlatformResourceFormat,
		SizeX,
		SizeY,
		1,
		NumMips,
		1,  // Sample count
		0);  // Sample quality

	D3D12_SUBRESOURCE_DATA SubResourceData[MAX_TEXTURE_MIP_COUNT] = { };
	for (uint32 MipIndex = 0; MipIndex < NumInitialMips; ++MipIndex)
	{
		uint32 NumBlocksX = FMath::Max<uint32>(1, (SizeX >> MipIndex) / GPixelFormats[Format].BlockSizeX);
		uint32 NumBlocksY = FMath::Max<uint32>(1, (SizeY >> MipIndex) / GPixelFormats[Format].BlockSizeY);

		SubResourceData[MipIndex].pData = InitialMipData[MipIndex];
		SubResourceData[MipIndex].RowPitch = NumBlocksX * GPixelFormats[Format].BlockBytes;
		SubResourceData[MipIndex].SlicePitch = NumBlocksX * NumBlocksY * GPixelFormats[Format].BlockBytes;
	}

	void* TempBuffer = ZeroBuffer;
	uint32 TempBufferSize = ZeroBufferSize;
	for (uint32 MipIndex = NumInitialMips; MipIndex < NumMips; ++MipIndex)
	{
		uint32 NumBlocksX = FMath::Max<uint32>(1, (SizeX >> MipIndex) / GPixelFormats[Format].BlockSizeX);
		uint32 NumBlocksY = FMath::Max<uint32>(1, (SizeY >> MipIndex) / GPixelFormats[Format].BlockSizeY);
		uint32 MipSize = NumBlocksX * NumBlocksY * GPixelFormats[Format].BlockBytes;

		if (MipSize > TempBufferSize)
		{
			UE_LOG(LogD3D12RHI, Display, TEXT("Temp texture streaming buffer not large enough, needed %d bytes"), MipSize);
			check(TempBufferSize == ZeroBufferSize);
			TempBufferSize = MipSize;
			TempBuffer = FMemory::Malloc(TempBufferSize);
			FMemory::Memzero(TempBuffer, TempBufferSize);
		}

		SubResourceData[MipIndex].pData = TempBuffer;
		SubResourceData[MipIndex].RowPitch = NumBlocksX * GPixelFormats[Format].BlockBytes;
		SubResourceData[MipIndex].SlicePitch = MipSize;
	}

	// All resources used in a COPY command list must begin in the COMMON state. 
	// COPY_SOURCE and COPY_DEST are "promotable" states. You can create async texture resources in the COMMON state and still avoid any state transitions by relying on state promotion. 
	// Also remember that ALL touched resources in a COPY command list decay to COMMON after ExecuteCommandLists completes.
	const D3D12_RESOURCE_STATES InitialState = D3D12_RESOURCE_STATE_COMMON;

	FD3D12Adapter* Adapter = &GetAdapter();
	FD3D12Texture2D* TextureOut = Adapter->CreateLinkedObject<FD3D12Texture2D>(FRHIGPUMask::All(), [&](FD3D12Device* Device)
	{
		FD3D12Texture2D* NewTexture = new FD3D12Texture2D(Device,
			SizeX,
			SizeY,
			0,
			NumMips,
			/*ActualMSAACount=*/ 1,
			(EPixelFormat)Format,
			/*bInCubemap=*/ false,
			Flags,
			FClearValueBinding());

		SafeCreateTexture2D(Device,
			Adapter,
			TextureDesc,
			nullptr,
			&NewTexture->ResourceLocation,
			NewTexture,
			(EPixelFormat)Format,
			Flags,
			InitialState,
			nullptr);

		D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
		SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		SRVDesc.Format = PlatformShaderResourceFormat;
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		SRVDesc.Texture2D.MostDetailedMip = 0;
		SRVDesc.Texture2D.MipLevels = NumMips;
		SRVDesc.Texture2D.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, SRVDesc.Format);

		// Create a wrapper for the SRV and set it on the texture
		NewTexture->SetShaderResourceView(new FD3D12ShaderResourceView(Device, SRVDesc, NewTexture));

		return NewTexture;
	});

	if (TextureOut)
	{
		// SubResourceData is only used in async texture creation (RHIAsyncCreateTexture2D). We need to manually transition the resource to
		// its 'default state', which is what the rest of the RHI (including InitializeTexture2DData) expects for SRV-only resources.

		check((TextureDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0);

		FD3D12FastAllocator& FastAllocator = TextureOut->GetParentDevice()->GetDefaultFastAllocator();
		uint64 Size = GetRequiredIntermediateSize(TextureOut->GetResource()->GetResource(), 0, NumMips);
		uint64 SizeLowMips;

		FD3D12ResourceLocation TempResourceLocation(FastAllocator.GetParentDevice());
		FD3D12ResourceLocation TempResourceLocationLowMips(FastAllocator.GetParentDevice());

		// The allocator work in pages of 4MB. Increasing page size is undesirable from a hitching point of view because there's a performance cliff above 4MB
		// where creation time of new pages can increase by an order of magnitude. Most allocations are smaller than 4MB, but a common exception is
		// 2048x2048 BC3 textures with mips, which takes 5.33MB. To avoid this case falling into the standalone allocations fallback path and risking hitching badly,
		// we split the top mip into a separate allocation, allowing it to fit within 4MB.
		const bool bSplitAllocation = (Size > 4 * 1024 * 1024) && (NumMips > 1);

		if (bSplitAllocation)
		{
			Size = GetRequiredIntermediateSize(TextureOut->GetResource()->GetResource(), 0, 1);
			SizeLowMips = GetRequiredIntermediateSize(TextureOut->GetResource()->GetResource(), 1, NumMips - 1);

			FastAllocator.Allocate(Size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &TempResourceLocation);
			FastAllocator.Allocate(SizeLowMips, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &TempResourceLocationLowMips);
			TempResourceLocationLowMips.GetResource()->AddRef();
		}
		else
		{
			FastAllocator.Allocate(Size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &TempResourceLocation);
		}
		// We AddRef() the resource here to make sure it doesn't get recycled prematurely. We are likely to be done with it during the frame,
		// but lifetime of the allocation is not strictly tied to the frame because we're using the copy queue here. Because we're waiting
		// on the GPU before returning here, this protection is safe, even if we end up straddling frame boundaries.
		TempResourceLocation.GetResource()->AddRef();

		for (FD3D12TextureBase& CurrentTextureBase : *TextureOut)
		{
			FD3D12Texture2D& CurrentTexture = static_cast<FD3D12Texture2D&>(CurrentTextureBase);
			FD3D12Device* Device = CurrentTexture.GetParentDevice();
			FD3D12Resource* Resource = CurrentTexture.GetResource();

			FD3D12CommandAllocatorManager& CommandAllocatorManager = Device->GetTextureStreamingCommandAllocatorManager();
			FD3D12CommandAllocator* CurrentCommandAllocator = CommandAllocatorManager.ObtainCommandAllocator();
			FD3D12CommandListHandle hCopyCommandList = Device->GetCopyCommandListManager().ObtainCommandList(*CurrentCommandAllocator);
			hCopyCommandList.SetCurrentOwningContext(&Device->GetDefaultCommandContext());

			// NB: Do not increment numCopies because that will count as work on the direct
			// queue, not the copy queue, possibly causing it to flush prematurely. We are
			// explicitly submitting the copy command list so there's no need to increment any
			// work counters.

			if (bSplitAllocation)
			{
				UpdateSubresources(
					(ID3D12GraphicsCommandList*)hCopyCommandList.CommandList(),
					Resource->GetResource(),
					TempResourceLocation.GetResource()->GetResource(),
					TempResourceLocation.GetOffsetFromBaseOfResource(),
					0, 1,
					SubResourceData);

				UpdateSubresources(
					(ID3D12GraphicsCommandList*)hCopyCommandList.CommandList(),
					Resource->GetResource(),
					TempResourceLocationLowMips.GetResource()->GetResource(),
					TempResourceLocationLowMips.GetOffsetFromBaseOfResource(),
					1, NumMips - 1,
					SubResourceData + 1);
			}
			else
			{
				UpdateSubresources(
					(ID3D12GraphicsCommandList*)hCopyCommandList.CommandList(),
					Resource->GetResource(),
					TempResourceLocation.GetResource()->GetResource(),
					TempResourceLocation.GetOffsetFromBaseOfResource(),
					0, NumMips,
					SubResourceData);
			}

			hCopyCommandList.UpdateResidency(Resource);

			// Wait for the copy context to finish before continuing as this function is only expected to return once all the texture streaming has finished.
			hCopyCommandList.Close();

			bool bWaitForCompletion = true;

			D3D12RHI::ExecuteCodeWithCopyCommandQueueUsage([Device, bWaitForCompletion, &hCopyCommandList](ID3D12CommandQueue* D3DCommandQueue) -> void
			{
				Device->GetCopyCommandListManager().ExecuteCommandListNoCopyQueueSync(hCopyCommandList, bWaitForCompletion);
			});

			CommandAllocatorManager.ReleaseCommandAllocator(CurrentCommandAllocator);
		}

		FD3D12TextureStats::D3D12TextureAllocated(*TextureOut);

		// These are clear to be recycled now because GPU is done with it at this point. We wait on GPU in ExecuteCommandList() above.
		// No defer delete required but can be reused immediately
		TempResourceLocation.GetResource()->DoNotDeferDelete();
		TempResourceLocation.GetResource()->Release();
		if (bSplitAllocation)
		{
			TempResourceLocationLowMips.GetResource()->DoNotDeferDelete();
			TempResourceLocationLowMips.GetResource()->Release();
		}
	}

	if (TempBufferSize != ZeroBufferSize)
	{
		FMemory::Free(TempBuffer);
	}


	return TextureOut;
}

void FD3D12DynamicRHI::RHICopySharedMips(FRHITexture2D* DestTexture2DRHI, FRHITexture2D* SrcTexture2DRHI)
{
	FD3D12Texture2D*  DestTexture2D = FD3D12DynamicRHI::ResourceCast(DestTexture2DRHI);
	FD3D12Texture2D*  SrcTexture2D = FD3D12DynamicRHI::ResourceCast(SrcTexture2DRHI);

	// Use the GPU to asynchronously copy the old mip-maps into the new texture.
	const uint32 NumSharedMips = FMath::Min(DestTexture2D->GetNumMips(), SrcTexture2D->GetNumMips());
	const uint32 SourceMipOffset = SrcTexture2D->GetNumMips() - NumSharedMips;
	const uint32 DestMipOffset = DestTexture2D->GetNumMips() - NumSharedMips;

	uint32 srcSubresource = 0;
	uint32 destSubresource = 0;

	FD3D12Adapter* Adapter = &GetAdapter();

	for (FD3D12Texture2D::FDualLinkedObjectIterator It(DestTexture2D, SrcTexture2D); It; ++It)
	{
		DestTexture2D = static_cast<FD3D12Texture2D*>(It.GetFirst());
		SrcTexture2D = static_cast<FD3D12Texture2D*>(It.GetSecond());

		FD3D12Device* Device = DestTexture2D->GetParentDevice();

		FD3D12CommandListHandle& hCommandList = Device->GetDefaultCommandContext().CommandListHandle;

		{
			FScopeResourceBarrier ScopeResourceBarrierDest(hCommandList, DestTexture2D->GetResource(), DestTexture2D->GetResource()->GetDefaultResourceState(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
			FScopeResourceBarrier ScopeResourceBarrierSrc(hCommandList, SrcTexture2D->GetResource(), SrcTexture2D->GetResource()->GetDefaultResourceState(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
			hCommandList.FlushResourceBarriers();

			for (uint32 MipIndex = 0; MipIndex < NumSharedMips; ++MipIndex)
			{
				// Use the GPU to copy between mip-maps.
				srcSubresource = CalcSubresource(MipIndex + SourceMipOffset, 0, SrcTexture2D->GetNumMips());
				destSubresource = CalcSubresource(MipIndex + DestMipOffset, 0, DestTexture2D->GetNumMips());

				CD3DX12_TEXTURE_COPY_LOCATION DestCopyLocation(DestTexture2D->GetResource()->GetResource(), destSubresource);
				CD3DX12_TEXTURE_COPY_LOCATION SourceCopyLocation(SrcTexture2D->GetResource()->GetResource(), srcSubresource);

				Device->GetDefaultCommandContext().numCopies++;
				hCommandList->CopyTextureRegion(
					&DestCopyLocation,
					0, 0, 0,
					&SourceCopyLocation,
					nullptr);

				hCommandList.UpdateResidency(DestTexture2D->GetResource());
				hCommandList.UpdateResidency(SrcTexture2D->GetResource());
			}
		}

		// unlock the pool allocated resource because all data has been written
		DestTexture2D->ResourceLocation.UnlockPoolData();

		Device->GetDefaultCommandContext().ConditionalFlushCommandList();

		DEBUG_EXECUTE_COMMAND_CONTEXT(Device->GetDefaultCommandContext());
	}
}

FTexture2DArrayRHIRef FD3D12DynamicRHI::RHICreateTexture2DArray_RenderThread(class FRHICommandListImmediate& RHICmdList, uint32 SizeX, uint32 SizeY, uint32 SizeZ, uint8 Format, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	check(SizeZ >= 1);

	return CreateD3D12Texture2D<FD3D12BaseTexture2DArray>(&RHICmdList, SizeX, SizeY, SizeZ, true, false, (EPixelFormat) Format, NumMips, NumSamples, Flags, InResourceState, CreateInfo);
}

FTexture2DArrayRHIRef FD3D12DynamicRHI::RHICreateTexture2DArray(uint32 SizeX, uint32 SizeY, uint32 SizeZ, uint8 Format, uint32 NumMips, uint32 NumSamples, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	check(SizeZ >= 1);

	return CreateD3D12Texture2D<FD3D12BaseTexture2DArray>(nullptr, SizeX, SizeY, SizeZ, true, false, (EPixelFormat) Format, NumMips, NumSamples, Flags, InResourceState, CreateInfo);
}

FTexture3DRHIRef FD3D12DynamicRHI::RHICreateTexture3D_RenderThread(class FRHICommandListImmediate& RHICmdList, uint32 SizeX, uint32 SizeY, uint32 SizeZ, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	return CreateD3D12Texture3D(&RHICmdList, SizeX, SizeY, SizeZ, (EPixelFormat) Format, NumMips, Flags, InResourceState, CreateInfo);
}

FTexture3DRHIRef FD3D12DynamicRHI::RHICreateTexture3D(uint32 SizeX, uint32 SizeY, uint32 SizeZ, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	check(SizeZ >= 1);
#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
	return CreateD3D12Texture3D(nullptr, SizeX, SizeY, SizeZ, (EPixelFormat) Format, NumMips, Flags, InResourceState, CreateInfo);
#else
	checkf(false, TEXT("XBOX_CODE_MERGE : Removed. The Xbox platform version should be used."));
	return nullptr;
#endif // PLATFORM_WINDOWS || PLATFORM_HOLOLENS
}

/**
 * Computes the size in memory required by a given texture.
 *
 * @param	TextureRHI		- Texture we want to know the size of
 * @return					- Size in Bytes
 */
uint32 FD3D12DynamicRHI::RHIComputeMemorySize(FRHITexture* TextureRHI)
{
	if (!TextureRHI)
	{
		return 0;
	}

	FD3D12TextureBase* Texture = GetD3D12TextureFromRHITexture(TextureRHI);
	return Texture->GetMemorySize();
}


static void DoAsyncReallocateTexture2D(FD3D12Texture2D* Texture2D, FD3D12Texture2D* NewTexture2D, int32 NewMipCount, int32 NewSizeX, int32 NewSizeY, FThreadSafeCounter* RequestStatus)
{
	// Use the GPU to asynchronously copy the old mip-maps into the new texture.
	const uint32 NumSharedMips = FMath::Min(Texture2D->GetNumMips(), NewTexture2D->GetNumMips());
	const uint32 SourceMipOffset = Texture2D->GetNumMips() - NumSharedMips;
	const uint32 DestMipOffset = NewTexture2D->GetNumMips() - NumSharedMips;

	uint32 destSubresource = 0;
	uint32 srcSubresource = 0;

	for (FD3D12Texture2D::FDualLinkedObjectIterator It(Texture2D, NewTexture2D); It; ++It)
	{
		Texture2D = static_cast<FD3D12Texture2D*>(It.GetFirst());
		NewTexture2D = static_cast<FD3D12Texture2D*>(It.GetSecond());

		FD3D12Device* Device = Texture2D->GetParentDevice();

		FD3D12CommandListHandle& hCommandList = Device->GetDefaultCommandContext().CommandListHandle;

		FScopeResourceBarrier ScopeResourceBarrierDest(hCommandList, NewTexture2D->GetResource(), NewTexture2D->GetResource()->GetDefaultResourceState(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
		FScopeResourceBarrier ScopeResourceBarrierSource(hCommandList, Texture2D->GetResource(), Texture2D->GetResource()->GetDefaultResourceState(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES);
		hCommandList.FlushResourceBarriers();	// Must flush so the desired state is actually set.

		for (uint32 MipIndex = 0; MipIndex < NumSharedMips; ++MipIndex)
		{
			// Use the GPU to copy between mip-maps.
			// This is serialized with other D3D commands, so it isn't necessary to increment Counter to signal a pending asynchronous copy.

			srcSubresource = CalcSubresource(MipIndex + SourceMipOffset, 0, Texture2D->GetNumMips());
			destSubresource = CalcSubresource(MipIndex + DestMipOffset, 0, NewTexture2D->GetNumMips());

			CD3DX12_TEXTURE_COPY_LOCATION DestCopyLocation(NewTexture2D->GetResource()->GetResource(), destSubresource);
			CD3DX12_TEXTURE_COPY_LOCATION SourceCopyLocation(Texture2D->GetResource()->GetResource(), srcSubresource);

			Device->GetDefaultCommandContext().numCopies++;
			hCommandList->CopyTextureRegion(
				&DestCopyLocation,
				0, 0, 0,
				&SourceCopyLocation,
				nullptr);

			hCommandList.UpdateResidency(NewTexture2D->GetResource());
			hCommandList.UpdateResidency(Texture2D->GetResource());

			Device->GetDefaultCommandContext().ConditionalFlushCommandList();

			DEBUG_EXECUTE_COMMAND_CONTEXT(Device->GetDefaultCommandContext());
		}
	}

	// Decrement the thread-safe counter used to track the completion of the reallocation, since D3D handles sequencing the
	// async mip copies with other D3D calls.
	RequestStatus->Decrement();
}


void FRHICommandD3D12AsyncReallocateTexture2D::Execute(FRHICommandListBase& RHICmdList)
{
	DoAsyncReallocateTexture2D(OldTexture, NewTexture, NewMipCount, NewSizeX, NewSizeY, RequestStatus);
}


FTexture2DRHIRef FD3D12DynamicRHI::AsyncReallocateTexture2D_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2D* Texture2DRHI, int32 NewMipCount, int32 NewSizeX, int32 NewSizeY, FThreadSafeCounter* RequestStatus)
{
	if (RHICmdList.Bypass())
	{
		return FDynamicRHI::AsyncReallocateTexture2D_RenderThread(RHICmdList, Texture2DRHI, NewMipCount, NewSizeX, NewSizeY, RequestStatus);
	}

	FD3D12Texture2D* Texture2D = FD3D12DynamicRHI::ResourceCast(Texture2DRHI);
	
	// Allocate a new texture.
	FRHIResourceCreateInfo CreateInfo(TEXT("AsyncReallocateTexture2D_RenderThread"));
	ERHIAccess RHIAccess = ERHIAccess::Unknown;
	FD3D12Texture2D* NewTexture2D = CreateD3D12Texture2D<FD3D12BaseTexture2D>(nullptr, NewSizeX, NewSizeY, 1, false, false, Texture2DRHI->GetFormat(), NewMipCount, 1, Texture2DRHI->GetFlags(), RHIAccess, CreateInfo);
	
	ALLOC_COMMAND_CL(RHICmdList, FRHICommandD3D12AsyncReallocateTexture2D)(Texture2D, NewTexture2D, NewMipCount, NewSizeX, NewSizeY, RequestStatus);

	return NewTexture2D;
}


/**
 * Starts an asynchronous texture reallocation. It may complete immediately if the reallocation
 * could be performed without any reshuffling of texture memory, or if there isn't enough memory.
 * The specified status counter will be decremented by 1 when the reallocation is complete (success or failure).
 *
 * Returns a new reference to the texture, which will represent the new mip count when the reallocation is complete.
 * RHIGetAsyncReallocateTexture2DStatus() can be used to check the status of an ongoing or completed reallocation.
 *
 * @param Texture2D		- Texture to reallocate
 * @param NewMipCount	- New number of mip-levels
 * @param NewSizeX		- New width, in pixels
 * @param NewSizeY		- New height, in pixels
 * @param RequestStatus	- Will be decremented by 1 when the reallocation is complete (success or failure).
 * @return				- New reference to the texture, or an invalid reference upon failure
 */
FTexture2DRHIRef FD3D12DynamicRHI::RHIAsyncReallocateTexture2D(FRHITexture2D* Texture2DRHI, int32 NewMipCount, int32 NewSizeX, int32 NewSizeY, FThreadSafeCounter* RequestStatus)
{
	FD3D12Texture2D*  Texture2D = FD3D12DynamicRHI::ResourceCast(Texture2DRHI);

	// Allocate a new texture.
	FRHIResourceCreateInfo CreateInfo(TEXT("RHIAsyncReallocateTexture2D"));
	ERHIAccess RHIAccess = ERHIAccess::Unknown;
	FD3D12Texture2D* NewTexture2D = CreateD3D12Texture2D<FD3D12BaseTexture2D>(nullptr, NewSizeX, NewSizeY, 1, false, false, Texture2DRHI->GetFormat(), NewMipCount, 1, Texture2DRHI->GetFlags(), RHIAccess, CreateInfo);
	
	DoAsyncReallocateTexture2D(Texture2D, NewTexture2D, NewMipCount, NewSizeX, NewSizeY, RequestStatus);

	return NewTexture2D;
}

/**
 * Returns the status of an ongoing or completed texture reallocation:
 *	TexRealloc_Succeeded	- The texture is ok, reallocation is not in progress.
 *	TexRealloc_Failed		- The texture is bad, reallocation is not in progress.
 *	TexRealloc_InProgress	- The texture is currently being reallocated async.
 *
 * @param Texture2D		- Texture to check the reallocation status for
 * @return				- Current reallocation status
 */
ETextureReallocationStatus FD3D12DynamicRHI::RHIFinalizeAsyncReallocateTexture2D(FRHITexture2D* Texture2D, bool bBlockUntilCompleted)
{
	return TexRealloc_Succeeded;
}

/**
 * Cancels an async reallocation for the specified texture.
 * This should be called for the new texture, not the original.
 *
 * @param Texture				Texture to cancel
 * @param bBlockUntilCompleted	If true, blocks until the cancellation is fully completed
 * @return						Reallocation status
 */
ETextureReallocationStatus FD3D12DynamicRHI::RHICancelAsyncReallocateTexture2D(FRHITexture2D* Texture2D, bool bBlockUntilCompleted)
{
	return TexRealloc_Succeeded;
}

template<typename RHIResourceType>
void* TD3D12Texture2D<RHIResourceType>::Lock(class FRHICommandListImmediate* RHICmdList, uint32 MipIndex, uint32 ArrayIndex, EResourceLockMode LockMode, uint32& DestStride)
{
	SCOPE_CYCLE_COUNTER(STAT_D3D12LockTextureTime);

	FD3D12Device* Device = GetParentDevice();
	FD3D12Adapter* Adapter = Device->GetParentAdapter();

	// Calculate the subresource index corresponding to the specified mip-map.
	const uint32 Subresource = CalcSubresource(MipIndex, ArrayIndex, this->GetNumMips());

	check(LockedMap.Find(Subresource) == nullptr);
	FD3D12LockedResource* LockedResource = new FD3D12LockedResource(Device);

	// Calculate the dimensions of the mip-map.
	const uint32 BlockSizeX = GPixelFormats[this->GetFormat()].BlockSizeX;
	const uint32 BlockSizeY = GPixelFormats[this->GetFormat()].BlockSizeY;
	const uint32 BlockBytes = GPixelFormats[this->GetFormat()].BlockBytes;
	const uint32 MipSizeX = FMath::Max(this->GetSizeX() >> MipIndex, BlockSizeX);
	const uint32 MipSizeY = FMath::Max(this->GetSizeY() >> MipIndex, BlockSizeY);
	const uint32 NumBlocksX = (MipSizeX + BlockSizeX - 1) / BlockSizeX;
	const uint32 NumBlocksY = (MipSizeY + BlockSizeY - 1) / BlockSizeY;

	const uint32 XBytesAligned = Align(NumBlocksX * BlockBytes, FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const uint32 MipBytesAligned = XBytesAligned * NumBlocksY;

	FD3D12CommandListHandle& hCommandList = Device->GetDefaultCommandContext().CommandListHandle;

#if !PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
	void* RawTextureMemory = (void*)ResourceLocation.GetGPUVirtualAddress();
#endif

	void* Data = nullptr;

	if (GetParentDevice()->GetOwningRHI()->HandleSpecialLock(Data, MipIndex, ArrayIndex, RHIResourceType::GetFlags(), LockMode, GetTextureLayout(), RawTextureMemory, DestStride))
	{
		// nothing left to do...
		check(Data != nullptr);
	}
	else
	if (LockMode == RLM_WriteOnly)
	{
		// If we're writing to the texture, allocate a system memory buffer to receive the new contents.
		// Use an upload heap to copy data to a default resource.
		//const uint32 bufferSize = (uint32)GetRequiredIntermediateSize(this->GetResource()->GetResource(), Subresource, 1);
		const uint32 bufferSize = Align(MipBytesAligned, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

		void* pData = Device->GetDefaultFastAllocator().Allocate(bufferSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &LockedResource->ResourceLocation);
		if (nullptr == pData)
		{
			check(false);
			return nullptr;
		}

		DestStride = XBytesAligned;
		LockedResource->LockedPitch = XBytesAligned;

		check(LockedResource->LockedPitch % FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);

		Data = LockedResource->ResourceLocation.GetMappedBaseAddress();
	}
	else
	{
		LockedResource->bLockedForReadOnly = true;

		//TODO: Make this work for AFR (it's probably a very rare occurance though)
		ensure(GNumExplicitGPUsForRendering == 1);

		// If we're reading from the texture, we create a staging resource, copy the texture contents to it, and map it.

		// Create the staging texture.
		const D3D12_RESOURCE_DESC& StagingTextureDesc = GetResource()->GetDesc();
		FD3D12Resource* StagingTexture = nullptr;

		const FRHIGPUMask Node = Device->GetGPUMask();
		VERIFYD3D12RESULT(Adapter->CreateBuffer(D3D12_HEAP_TYPE_READBACK, Node, Node, MipBytesAligned, &StagingTexture, nullptr));

		LockedResource->ResourceLocation.AsStandAlone(StagingTexture, MipBytesAligned);

		// Copy the mip-map data from the real resource into the staging resource
		D3D12_SUBRESOURCE_FOOTPRINT destSubresource;
		destSubresource.Depth = 1;
		destSubresource.Height = MipSizeY;
		destSubresource.Width = MipSizeX;
		destSubresource.Format = StagingTextureDesc.Format;
		destSubresource.RowPitch = XBytesAligned;
		check(destSubresource.RowPitch % FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT placedTexture2D = { 0 };
		placedTexture2D.Offset = 0;
		placedTexture2D.Footprint = destSubresource;

		CD3DX12_TEXTURE_COPY_LOCATION DestCopyLocation(StagingTexture->GetResource(), placedTexture2D);
		CD3DX12_TEXTURE_COPY_LOCATION SourceCopyLocation(GetResource()->GetResource(), Subresource);

		const auto& pfnCopyTextureRegion = [&]()
		{
			FScopeResourceBarrier ScopeResourceBarrierSource(hCommandList, GetResource(), GetResource()->GetDefaultResourceState(), D3D12_RESOURCE_STATE_COPY_SOURCE, SourceCopyLocation.SubresourceIndex);

			Device->GetDefaultCommandContext().numCopies++;
			hCommandList.FlushResourceBarriers();
			hCommandList->CopyTextureRegion(
				&DestCopyLocation,
				0, 0, 0,
				&SourceCopyLocation,
				nullptr);

			hCommandList.UpdateResidency(GetResource());
		};

		if (RHICmdList != nullptr)
		{
			check(IsInRHIThread() == false);

			RHICmdList->ImmediateFlush(EImmediateFlushType::FlushRHIThread);
			pfnCopyTextureRegion();
		}
		else
		{
			check(IsInRHIThread());

			pfnCopyTextureRegion();
		}

		// We need to execute the command list so we can read the data from the map below
		Device->GetDefaultCommandContext().FlushCommands(true);

		LockedResource->LockedPitch = XBytesAligned;
		DestStride = XBytesAligned;
		check(LockedResource->LockedPitch % FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);
		check(DestStride == XBytesAligned);

		Data = LockedResource->ResourceLocation.GetMappedBaseAddress();
	}

	LockedMap.Add(Subresource, LockedResource);

	check(Data != nullptr);
	return Data;
}

D3D12_RESOURCE_DESC FD3D12DynamicRHI::GetResourceDesc(const FRHITextureCreateInfo& CreateInfo) const
{
	D3D12_RESOURCE_DESC Desc;

	const DXGI_FORMAT Format = GetPlatformTextureResourceFormat((DXGI_FORMAT)GPixelFormats[CreateInfo.Format].PlatformFormat, CreateInfo.Flags);

	if (CreateInfo.Dimension != ETextureDimension::Texture3D)
	{
		if (CreateInfo.IsTextureCube())
		{
			check(CreateInfo.Extent.X <= (int32)GetMaxCubeTextureDimension());
			check(CreateInfo.Extent.X == CreateInfo.Extent.Y);
		}
		else
		{
			check(CreateInfo.Extent.X <= (int32)GetMax2DTextureDimension());
			check(CreateInfo.Extent.Y <= (int32)GetMax2DTextureDimension());
		}

		if (CreateInfo.IsTextureArray())
		{
			check(CreateInfo.ArraySize <= (int32)GetMaxTextureArrayLayers());
		}

		uint32 ActualMSAACount = CreateInfo.NumSamples;
		uint32 ActualMSAAQuality = GetMaxMSAAQuality(ActualMSAACount);

		// 0xffffffff means not supported
		if (ActualMSAAQuality == 0xffffffff || EnumHasAnyFlags(CreateInfo.Flags, TexCreate_Shared))
		{
			// no MSAA
			ActualMSAACount = 1;
			ActualMSAAQuality = 0;
		}

		Desc = CD3DX12_RESOURCE_DESC::Tex2D(
			Format,
			CreateInfo.Extent.X,
			CreateInfo.Extent.Y,
			CreateInfo.ArraySize * (CreateInfo.IsTextureCube() ? 6 : 1),  // Array size
			CreateInfo.NumMips,
			ActualMSAACount,
			ActualMSAAQuality,
			D3D12_RESOURCE_FLAG_NONE);  // Add misc flags later

		if (EnumHasAnyFlags(CreateInfo.Flags, TexCreate_Shared))
		{
			Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;
		}

		if (EnumHasAnyFlags(CreateInfo.Flags, TexCreate_RenderTargetable))
		{
			check(!EnumHasAnyFlags(CreateInfo.Flags, TexCreate_DepthStencilTargetable | TexCreate_ResolveTargetable));
			Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		}
		else if (EnumHasAnyFlags(CreateInfo.Flags, TexCreate_DepthStencilTargetable))
		{
			check(!EnumHasAnyFlags(CreateInfo.Flags, TexCreate_RenderTargetable | TexCreate_ResolveTargetable));
			Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
		}
		else if (EnumHasAnyFlags(CreateInfo.Flags, TexCreate_ResolveTargetable))
		{
			check(!EnumHasAnyFlags(CreateInfo.Flags, TexCreate_RenderTargetable | TexCreate_DepthStencilTargetable));
			if (CreateInfo.Format == PF_DepthStencil || CreateInfo.Format == PF_ShadowDepth || CreateInfo.Format == PF_D24)
			{
				Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
			}
			else
			{
				Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			}
		}

		if (EnumHasAnyFlags(CreateInfo.Flags, TexCreate_UAV))
		{
			Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		}

		if (EnumHasAnyFlags(CreateInfo.Flags, TexCreate_DepthStencilTargetable) && !EnumHasAnyFlags(CreateInfo.Flags, TexCreate_ShaderResource))
		{
			// Only deny shader resources if it's a depth resource that will never be used as SRV
			Desc.Flags |= D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
		}
	}
	else // ETextureDimension::Texture3D
	{
		check(CreateInfo.Dimension == ETextureDimension::Texture3D)
		check(!EnumHasAnyFlags(CreateInfo.Flags, TexCreate_DepthStencilTargetable | TexCreate_ResolveTargetable));
		check(EnumHasAnyFlags(CreateInfo.Flags, TexCreate_ShaderResource));

		Desc = CD3DX12_RESOURCE_DESC::Tex3D(
			Format,
			CreateInfo.Extent.X,
			CreateInfo.Extent.Y,
			CreateInfo.Depth,
			CreateInfo.NumMips);

		if (EnumHasAnyFlags(CreateInfo.Flags, TexCreate_UAV))
		{
			Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
		}

		if (EnumHasAnyFlags(CreateInfo.Flags, TexCreate_RenderTargetable))
		{
			Desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		}
	}

	Desc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

	return Desc;
}

void FD3D12TextureBase::UpdateTexture(uint32 MipIndex, uint32 DestX, uint32 DestY, uint32 DestZ, const D3D12_TEXTURE_COPY_LOCATION& SourceCopyLocation)
{
	LLM_SCOPE_BYNAME(TEXT("D3D12CopyTextureRegion"));
	FD3D12CommandContext& DefaultContext = GetParentDevice()->GetDefaultCommandContext();
	FD3D12CommandListHandle& hCommandList = DefaultContext.CommandListHandle;

	FScopedResourceBarrier ScopeResourceBarrierDest(hCommandList, GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, MipIndex, FD3D12DynamicRHI::ETransitionMode::Apply);
	// Don't need to transition upload heaps

	CD3DX12_TEXTURE_COPY_LOCATION DestCopyLocation(GetResource()->GetResource(), MipIndex);

	DefaultContext.numCopies++;
	hCommandList.FlushResourceBarriers();
	hCommandList->CopyTextureRegion(
		&DestCopyLocation,
		DestX, DestY, DestZ,
		&SourceCopyLocation,
		nullptr);

	hCommandList.UpdateResidency(GetResource());
	
	DefaultContext.ConditionalFlushCommandList();

	DEBUG_EXECUTE_COMMAND_CONTEXT(DefaultContext);
}

void FD3D12TextureBase::CopyTextureRegion(uint32 DestX, uint32 DestY, uint32 DestZ, FD3D12TextureBase* SourceTexture, const D3D12_BOX& SourceBox)
{
	FD3D12CommandContext& DefaultContext = GetParentDevice()->GetDefaultCommandContext();
	FD3D12CommandListHandle& CommandListHandle = DefaultContext.CommandListHandle;

	CD3DX12_TEXTURE_COPY_LOCATION DestCopyLocation(GetResource()->GetResource(), 0);
	CD3DX12_TEXTURE_COPY_LOCATION SourceCopyLocation(SourceTexture->GetResource()->GetResource(), 0);

	FScopedResourceBarrier ConditionalScopeResourceBarrierDest(CommandListHandle, GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, DestCopyLocation.SubresourceIndex, FD3D12DynamicRHI::ETransitionMode::Apply);
	FScopedResourceBarrier ConditionalScopeResourceBarrierSource(CommandListHandle, SourceTexture->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, SourceCopyLocation.SubresourceIndex, FD3D12DynamicRHI::ETransitionMode::Apply);

	CommandListHandle.FlushResourceBarriers();
	CommandListHandle->CopyTextureRegion(
		&DestCopyLocation,
		DestX, DestY, DestZ,
		&SourceCopyLocation,
		&SourceBox);

	CommandListHandle.UpdateResidency(SourceTexture->GetResource());
	CommandListHandle.UpdateResidency(GetResource());
}

void FD3D12TextureBase::InitializeTextureData(FRHICommandListImmediate* RHICmdList, const void* InitData, uint32 InitDataSize, uint32 SizeX, uint32 SizeY, uint32 SizeZ, uint32 NumSlices, uint32 NumMips, EPixelFormat Format, D3D12_RESOURCE_STATES DestinationState)
{
	// each mip of each array slice counts as a subresource
	uint32 NumSubresources = NumMips * NumSlices;

	FD3D12Device* Device = GetParentDevice();

	size_t MemSize = NumSubresources * (sizeof(D3D12_PLACED_SUBRESOURCE_FOOTPRINT) + sizeof(UINT) + sizeof(UINT64));
	const bool bAllocateOnStack = (MemSize < 4096);
	void* Mem = bAllocateOnStack? FMemory_Alloca(MemSize) : FMemory::Malloc(MemSize);

	D3D12_PLACED_SUBRESOURCE_FOOTPRINT* Footprints = (D3D12_PLACED_SUBRESOURCE_FOOTPRINT*) Mem;
	check(Footprints);
	UINT* Rows = (UINT*) (Footprints + NumSubresources);
	check(Rows);
	UINT64* RowSizeInBytes = (UINT64*) (Rows + NumSubresources);
	check(RowSizeInBytes);

	uint64 Size = 0;
	const D3D12_RESOURCE_DESC& Desc = GetResource()->GetDesc();
	Device->GetDevice()->GetCopyableFootprints(&Desc, 0, NumSubresources, 0, Footprints, Rows, RowSizeInBytes, &Size);

	FD3D12ResourceLocation SrcResourceLoc(Device);
	uint8* DstDataBase = (uint8*) Device->GetDefaultFastAllocator().Allocate(Size, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &SrcResourceLoc);

	const uint8* SrcData = (const uint8*) InitData;
	for (uint32 Subresource = 0; Subresource < NumSubresources; Subresource++)
	{
		uint8* DstData = DstDataBase + Footprints[Subresource].Offset;

		const uint32 NumRows = Rows[Subresource] * Footprints[Subresource].Footprint.Depth;
		const uint32 SrcRowPitch = RowSizeInBytes[Subresource];
		const uint32 DstRowPitch = Footprints[Subresource].Footprint.RowPitch;

		// If src and dst pitch are aligned, which is typically the case for the bulk of the data (most large mips, POT textures), we can use a single large memcpy()
		if (SrcRowPitch == DstRowPitch)
		{
			memcpy(DstData, SrcData, SrcRowPitch * NumRows);
			SrcData += SrcRowPitch * NumRows;
		}
		else
		{
			for (uint32 Row = 0; Row < NumRows; ++Row)
			{
				memcpy(DstData, SrcData, SrcRowPitch);

				SrcData += SrcRowPitch;
				DstData += DstRowPitch;
			}
		}
	}

	check(SrcData == (uint8*) InitData + InitDataSize);

	if (ShouldDeferCmdListOperation(RHICmdList))
	{
		ALLOC_COMMAND_CL(*RHICmdList, FD3D12RHICommandInitializeTexture)(this, SrcResourceLoc, NumSubresources, DestinationState);
	}
	else
	{
		FD3D12RHICommandInitializeTexture Command(this, SrcResourceLoc, NumSubresources, DestinationState);
		Command.ExecuteNoCmdList();
	}

	if (!bAllocateOnStack)
	{
		FMemory::Free(Mem);
	}
}

template<typename RHIResourceType>
void TD3D12Texture2D<RHIResourceType>::Unlock(class FRHICommandListImmediate* RHICmdList, uint32 MipIndex, uint32 ArrayIndex)
{
	SCOPE_CYCLE_COUNTER(STAT_D3D12UnlockTextureTime);

	UnlockInternal(RHICmdList, ++FLinkedObjectIterator(this), MipIndex, ArrayIndex);
}

template<typename RHIResourceType>
void TD3D12Texture2D<RHIResourceType>::UnlockInternal(class FRHICommandListImmediate* RHICmdList, FLinkedObjectIterator NextObject, uint32 MipIndex, uint32 ArrayIndex)
{
	// Calculate the subresource index corresponding to the specified mip-map.
	const uint32 Subresource = CalcSubresource(MipIndex, ArrayIndex, this->GetNumMips());

	// Calculate the dimensions of the mip-map.
	const uint32 BlockSizeX = GPixelFormats[this->GetFormat()].BlockSizeX;
	const uint32 BlockSizeY = GPixelFormats[this->GetFormat()].BlockSizeY;
	const uint32 BlockBytes = GPixelFormats[this->GetFormat()].BlockBytes;
	const uint32 MipSizeX = FMath::Max(this->GetSizeX() >> MipIndex, BlockSizeX);
	const uint32 MipSizeY = FMath::Max(this->GetSizeY() >> MipIndex, BlockSizeY);

	auto* FirstObject = static_cast<TD3D12Texture2D<RHIResourceType>*>(GetFirstLinkedObject());
	TMap<uint32, FD3D12LockedResource*>& Map = FirstObject->LockedMap;
	FD3D12LockedResource* LockedResource = Map[Subresource];

	check(LockedResource);

#if !PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
	void* RawTextureMemory = (void*)ResourceLocation.GetGPUVirtualAddress();
#endif

	if (GetParentDevice()->GetOwningRHI()->HandleSpecialUnlock(RHICmdList, MipIndex, RHIResourceType::GetFlags(), GetTextureLayout(), RawTextureMemory))
	{
		// nothing left to do...
	}
	else
	{
		if (!LockedResource->bLockedForReadOnly)
		{
			FD3D12Resource* Resource = GetResource();
			FD3D12ResourceLocation& UploadLocation = LockedResource->ResourceLocation;

			// Copy the mip-map data from the real resource into the staging resource
			const D3D12_RESOURCE_DESC& ResourceDesc = Resource->GetDesc();
			D3D12_SUBRESOURCE_FOOTPRINT BufferPitchDesc;
			BufferPitchDesc.Depth = 1;
			BufferPitchDesc.Height = MipSizeY;
			BufferPitchDesc.Width = MipSizeX;
			BufferPitchDesc.Format = ResourceDesc.Format;
			BufferPitchDesc.RowPitch = LockedResource->LockedPitch;
			check(BufferPitchDesc.RowPitch % FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);

			D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedTexture2D = { 0 };
			PlacedTexture2D.Offset = UploadLocation.GetOffsetFromBaseOfResource();
			PlacedTexture2D.Footprint = BufferPitchDesc;

			CD3DX12_TEXTURE_COPY_LOCATION SourceCopyLocation(UploadLocation.GetResource()->GetResource(), PlacedTexture2D);

			FD3D12CommandListHandle& hCommandList = GetParentDevice()->GetDefaultCommandContext().CommandListHandle;

			// If we are on the render thread, queue up the copy on the RHIThread so it happens at the correct time.
			if (ShouldDeferCmdListOperation(RHICmdList))
			{
				// Same FD3D12ResourceLocation is used for all resources in the chain, therefore only the last command must be responsible for releasing it.
				FD3D12ResourceLocation* Source = NextObject ? nullptr : &UploadLocation;
				ALLOC_COMMAND_CL(*RHICmdList, FRHICommandUpdateTexture)(this, Subresource, 0, 0, 0, SourceCopyLocation, Source);
			}
			else
			{
				UpdateTexture(Subresource, 0, 0, 0, SourceCopyLocation);
			}

			// Recurse to update all of the resources in the LDA chain
			if (NextObject)
			{
				// We pass the first link in the chain as that's the guy that got locked
				((TD3D12Texture2D<RHIResourceType>*)NextObject.Get())->UnlockInternal(RHICmdList, ++NextObject, MipIndex, ArrayIndex);
			}
		}
	}

	if (FirstObject == this)
	{
		// Remove the lock from the outstanding lock list.
		delete(LockedResource);
		Map.Remove(Subresource);
	}
}

template<typename RHIResourceType>
void TD3D12Texture2D<RHIResourceType>::UpdateTexture2D(class FRHICommandListImmediate* RHICmdList, uint32 MipIndex, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, const uint8* SourceData)
{
	const FPixelFormatInfo& FormatInfo = GPixelFormats[this->GetFormat()];
	check(UpdateRegion.Width  %	FormatInfo.BlockSizeX == 0);
	check(UpdateRegion.Height % FormatInfo.BlockSizeY == 0);
	check(UpdateRegion.DestX  %	FormatInfo.BlockSizeX == 0);
	check(UpdateRegion.DestY  %	FormatInfo.BlockSizeY == 0);
	check(UpdateRegion.SrcX   %	FormatInfo.BlockSizeX == 0);
	check(UpdateRegion.SrcY   %	FormatInfo.BlockSizeY == 0);

	const uint32 WidthInBlocks = UpdateRegion.Width / FormatInfo.BlockSizeX;
	const uint32 HeightInBlocks = UpdateRegion.Height / FormatInfo.BlockSizeY;

	const uint32 AlignedSourcePitch = Align(SourcePitch, FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
	const uint32 bufferSize = Align(HeightInBlocks*AlignedSourcePitch, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

	for (FD3D12TextureBase& TextureBase : *this)
	{
		FD3D12Texture2D& Texture = static_cast<FD3D12Texture2D&>(TextureBase);
		FD3D12ResourceLocation UploadHeapResourceLocation(GetParentDevice());
		void* pData = GetParentDevice()->GetDefaultFastAllocator().Allocate(bufferSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, &UploadHeapResourceLocation);
		check(nullptr != pData);

		byte* pRowData = (byte*)pData;
		const byte* pSourceRowData = (byte*)SourceData;
		const uint32 CopyPitch = WidthInBlocks * FormatInfo.BlockBytes;
		check(CopyPitch <= SourcePitch);
		for (uint32 i = 0; i < HeightInBlocks; i++)
		{
			FMemory::Memcpy(pRowData, pSourceRowData, CopyPitch);
			pSourceRowData += SourcePitch;
			pRowData += AlignedSourcePitch;
		}

		D3D12_SUBRESOURCE_FOOTPRINT SourceSubresource;
		SourceSubresource.Depth = 1;
		SourceSubresource.Height = UpdateRegion.Height;
		SourceSubresource.Width = UpdateRegion.Width;
		SourceSubresource.Format = (DXGI_FORMAT)FormatInfo.PlatformFormat;
		SourceSubresource.RowPitch = AlignedSourcePitch;
		check(SourceSubresource.RowPitch % FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);

		D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedTexture2D = { 0 };
		PlacedTexture2D.Offset = UploadHeapResourceLocation.GetOffsetFromBaseOfResource();
		PlacedTexture2D.Footprint = SourceSubresource;

		CD3DX12_TEXTURE_COPY_LOCATION SourceCopyLocation(UploadHeapResourceLocation.GetResource()->GetResource(), PlacedTexture2D);

		// If we are on the render thread, queue up the copy on the RHIThread so it happens at the correct time.
		if (ShouldDeferCmdListOperation(RHICmdList))
		{
			ALLOC_COMMAND_CL(*RHICmdList, FRHICommandUpdateTexture)(&Texture, MipIndex, UpdateRegion.DestX, UpdateRegion.DestY, 0, SourceCopyLocation, &UploadHeapResourceLocation);
		}
		else
		{
			Texture.UpdateTexture(MipIndex, UpdateRegion.DestX, UpdateRegion.DestY, 0, SourceCopyLocation);
		}
	}
}

static void GetReadBackHeapDescImpl(D3D12_PLACED_SUBRESOURCE_FOOTPRINT& OutFootprint, ID3D12Device* InDevice, D3D12_RESOURCE_DESC const& InResourceDesc, uint32 InSubresource)
{
	uint64 Offset = 0;
	if (InSubresource > 0)
	{
		InDevice->GetCopyableFootprints(&InResourceDesc, 0, InSubresource, 0, nullptr, nullptr, nullptr, &Offset);
		Offset = Align(Offset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
	}
	InDevice->GetCopyableFootprints(&InResourceDesc, InSubresource, 1, Offset, &OutFootprint, nullptr, nullptr, nullptr);

	check(OutFootprint.Footprint.Width > 0 && OutFootprint.Footprint.Height > 0);
}

template<typename RHIResourceType>
void TD3D12Texture2D<RHIResourceType>::GetReadBackHeapDesc(D3D12_PLACED_SUBRESOURCE_FOOTPRINT& OutFootprint, uint32 InSubresource) const
{
	check(EnumHasAnyFlags(RHIResourceType::GetFlags(), TexCreate_CPUReadback));

	if (InSubresource == 0 && FirstSubresourceFootprint)
	{
		OutFootprint = *FirstSubresourceFootprint;
		return;
	}

	FIntVector TextureSize = RHIResourceType::GetSizeXYZ();

	D3D12_RESOURCE_DESC Desc = {};
	Desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	Desc.Width            = TextureSize.X;
	Desc.Height           = TextureSize.Y;
	Desc.DepthOrArraySize = TextureSize.Z;
	Desc.MipLevels        = RHIResourceType::GetNumMips();
	Desc.Format           = (DXGI_FORMAT) GPixelFormats[RHIResourceType::GetFormat()].PlatformFormat;
	Desc.SampleDesc.Count = RHIResourceType::GetNumSamples();

	GetReadBackHeapDescImpl(OutFootprint, GetParentDevice()->GetDevice(), Desc, InSubresource);

	if (InSubresource == 0)
	{
		FirstSubresourceFootprint = MakeUnique<D3D12_PLACED_SUBRESOURCE_FOOTPRINT>();
		*FirstSubresourceFootprint = OutFootprint;
	}
}

void* FD3D12DynamicRHI::LockTexture2D_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2D* TextureRHI, uint32 MipIndex, EResourceLockMode LockMode, uint32& DestStride, bool bLockWithinMiptail, bool bNeedsDefaultRHIFlush)
{
	if (CVarD3D12Texture2DRHIFlush.GetValueOnRenderThread() && bNeedsDefaultRHIFlush)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_RHIMETHOD_LockTexture2D_Flush);
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		return RHILockTexture2D(TextureRHI, MipIndex, LockMode, DestStride, bLockWithinMiptail);
	}

	check(TextureRHI);
	FD3D12Texture2D* Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	return Texture->Lock(&RHICmdList, MipIndex, 0, LockMode, DestStride);
}

void* FD3D12DynamicRHI::RHILockTexture2D(FRHITexture2D* TextureRHI, uint32 MipIndex, EResourceLockMode LockMode, uint32& DestStride, bool bLockWithinMiptail)
{
	check(TextureRHI);
	FD3D12Texture2D*  Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	return Texture->Lock(nullptr, MipIndex, 0, LockMode, DestStride);
}

void FD3D12DynamicRHI::UnlockTexture2D_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2D* TextureRHI, uint32 MipIndex, bool bLockWithinMiptail, bool bNeedsDefaultRHIFlush)
{
	if (CVarD3D12Texture2DRHIFlush.GetValueOnRenderThread() && bNeedsDefaultRHIFlush)
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_RHIMETHOD_UnlockTexture2D_Flush);
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
		RHIUnlockTexture2D(TextureRHI, MipIndex, bLockWithinMiptail);
		return;
	}

	check(TextureRHI);
	FD3D12Texture2D* Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	Texture->Unlock(&RHICmdList, MipIndex, 0);
}

void FD3D12DynamicRHI::RHIUnlockTexture2D(FRHITexture2D* TextureRHI, uint32 MipIndex, bool bLockWithinMiptail)
{
	check(TextureRHI);
	FD3D12Texture2D*  Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	Texture->Unlock(nullptr, MipIndex, 0);
}

void* FD3D12DynamicRHI::LockTexture2DArray_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2DArray* TextureRHI, uint32 TextureIndex, uint32 MipIndex, EResourceLockMode LockMode, uint32& DestStride, bool bLockWithinMiptail)
{
	check(TextureRHI);
	FD3D12Texture2DArray* Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	return Texture->Lock(&RHICmdList, MipIndex, TextureIndex, LockMode, DestStride);
}

void* FD3D12DynamicRHI::RHILockTexture2DArray(FRHITexture2DArray* TextureRHI, uint32 TextureIndex, uint32 MipIndex, EResourceLockMode LockMode, uint32& DestStride, bool bLockWithinMiptail)
{
	check(TextureRHI);
	FD3D12Texture2DArray*  Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	return Texture->Lock(nullptr, MipIndex, TextureIndex, LockMode, DestStride);
}

void FD3D12DynamicRHI::UnlockTexture2DArray_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2DArray* TextureRHI, uint32 TextureIndex, uint32 MipIndex, bool bLockWithinMiptail)
{
	check(TextureRHI);
	FD3D12Texture2DArray* Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	Texture->Unlock(&RHICmdList, MipIndex, TextureIndex);
}

void FD3D12DynamicRHI::RHIUnlockTexture2DArray(FRHITexture2DArray* TextureRHI, uint32 TextureIndex, uint32 MipIndex, bool bLockWithinMiptail)
{
	check(TextureRHI);
	FD3D12Texture2DArray*  Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	Texture->Unlock(nullptr, MipIndex, TextureIndex);
}

void FD3D12DynamicRHI::UpdateTexture2D_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture2D* TextureRHI, uint32 MipIndex, const struct FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, const uint8* SourceData)
{
	check(TextureRHI);
	FD3D12Texture2D* Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	Texture->UpdateTexture2D(&RHICmdList, MipIndex, UpdateRegion, SourcePitch, SourceData);
}

void FD3D12DynamicRHI::RHIUpdateTexture2D(FRHITexture2D* TextureRHI, uint32 MipIndex, const FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, const uint8* SourceData)
{
	check(TextureRHI);
	FD3D12Texture2D* Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	Texture->UpdateTexture2D(nullptr, MipIndex, UpdateRegion, SourcePitch, SourceData);
}

FUpdateTexture3DData FD3D12DynamicRHI::BeginUpdateTexture3D_RenderThread(class FRHICommandListImmediate& RHICmdList, FRHITexture3D* Texture, uint32 MipIndex, const struct FUpdateTextureRegion3D& UpdateRegion)
{
	check(IsInRenderingThread());
	// This stall could potentially be removed, provided the fast allocator is thread-safe. However we 
	// currently need to stall in the End method anyway (see below)
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
	return BeginUpdateTexture3D_Internal(Texture, MipIndex, UpdateRegion);
}

void FD3D12DynamicRHI::EndUpdateTexture3D_RenderThread(class FRHICommandListImmediate& RHICmdList, FUpdateTexture3DData& UpdateData)
{
	check(IsInRenderingThread());
	// TODO: move this command entirely to the RHI thread so we can remove these stalls
	// and fix potential ordering issue with non-compute-shader version
	RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
	EndUpdateTexture3D_Internal(UpdateData);
}

struct FD3D12RHICmdEndMultiUpdateTexture3DString
{
	static const TCHAR* TStr() { return TEXT("FD3D12RHICmdEndMultiUpdateTexture3D"); }
};
class FD3D12RHICmdEndMultiUpdateTexture3D : public FRHICommand<FD3D12RHICmdEndMultiUpdateTexture3D, FD3D12RHICmdEndMultiUpdateTexture3DString>
{
public:
	FD3D12RHICmdEndMultiUpdateTexture3D(TArray<FUpdateTexture3DData>& UpdateDataArray) :
		MipIdx(UpdateDataArray[0].MipIndex),
		DstTexture(UpdateDataArray[0].Texture)
	{
		const int32 NumUpdates = UpdateDataArray.Num();
		UpdateInfos.Empty(NumUpdates);
		UpdateInfos.AddZeroed(NumUpdates);

		for (int32 Idx = 0; Idx < UpdateInfos.Num(); ++Idx)
		{
			FUpdateInfo& UpdateInfo = UpdateInfos[Idx];
			FUpdateTexture3DData& UpdateData = UpdateDataArray[Idx];

			UpdateInfo.DstStartX = UpdateData.UpdateRegion.DestX;
			UpdateInfo.DstStartY = UpdateData.UpdateRegion.DestY;
			UpdateInfo.DstStartZ = UpdateData.UpdateRegion.DestZ;

			D3D12_SUBRESOURCE_FOOTPRINT& SubresourceFootprint = UpdateInfo.PlacedSubresourceFootprint.Footprint;
			SubresourceFootprint.Depth = UpdateData.UpdateRegion.Depth;
			SubresourceFootprint.Height = UpdateData.UpdateRegion.Height;
			SubresourceFootprint.Width = UpdateData.UpdateRegion.Width;
			SubresourceFootprint.Format = static_cast<DXGI_FORMAT>(GPixelFormats[DstTexture->GetFormat()].PlatformFormat);
			SubresourceFootprint.RowPitch = UpdateData.RowPitch;
			check(SubresourceFootprint.RowPitch % FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);

			FD3D12UpdateTexture3DData* UpdateDataD3D12 =
				reinterpret_cast<FD3D12UpdateTexture3DData*>(&UpdateData.PlatformData[0]);

			UpdateInfo.SrcResourceLocation = UpdateDataD3D12->UploadHeapResourceLocation;
			UpdateInfo.PlacedSubresourceFootprint.Offset = UpdateInfo.SrcResourceLocation->GetOffsetFromBaseOfResource();
		}
	}

	virtual ~FD3D12RHICmdEndMultiUpdateTexture3D()
	{
		for (int32 Idx = 0; Idx < UpdateInfos.Num(); ++Idx)
		{
			const FUpdateInfo& UpdateInfo = UpdateInfos[Idx];
			if (UpdateInfo.SrcResourceLocation)
			{
				delete UpdateInfo.SrcResourceLocation;
			}
		}
		UpdateInfos.Empty();
	}

	void Execute(FRHICommandListBase& RHICmdList)
	{
		FD3D12Texture3D* NativeTexture = FD3D12DynamicRHI::ResourceCast(DstTexture.GetReference());

		for (FD3D12TextureBase& TextureLinkBase : *NativeTexture)
		{
			FD3D12Texture3D& TextureLink = static_cast<FD3D12Texture3D&>(TextureLinkBase);
			FD3D12Device* Device = TextureLink.GetParentDevice();
			FD3D12CommandListHandle& NativeCmdList = Device->GetDefaultCommandContext().CommandListHandle;

			CD3DX12_TEXTURE_COPY_LOCATION DestCopyLocation(TextureLink.GetResource()->GetResource(), MipIdx);

			FScopedResourceBarrier ScopeResourceBarrierDest(
				NativeCmdList,
				TextureLink.GetResource(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				DestCopyLocation.SubresourceIndex,
				FD3D12DynamicRHI::ETransitionMode::Apply);

			NativeCmdList.FlushResourceBarriers();
			Device->GetDefaultCommandContext().numCopies += UpdateInfos.Num();

			for (int32 Idx = 0; Idx < UpdateInfos.Num(); ++Idx)
			{
				const FUpdateInfo& UpdateInfo = UpdateInfos[Idx];
				FD3D12Resource* UploadBuffer = UpdateInfo.SrcResourceLocation->GetResource();
				CD3DX12_TEXTURE_COPY_LOCATION SourceCopyLocation(UploadBuffer->GetResource(), UpdateInfo.PlacedSubresourceFootprint);
#if USE_PIX
				if (FD3D12DynamicRHI::GetD3DRHI()->IsPixEventEnabled())
				{
					PIXBeginEvent(NativeCmdList.GraphicsCommandList(), PIX_COLOR(255, 255, 255), TEXT("EndMultiUpdateTexture3D"));
				}
#endif
				NativeCmdList->CopyTextureRegion(
					&DestCopyLocation,
					UpdateInfo.DstStartX,
					UpdateInfo.DstStartY,
					UpdateInfo.DstStartZ,
					&SourceCopyLocation,
					nullptr);

				NativeCmdList.UpdateResidency(TextureLink.GetResource());
				DEBUG_EXECUTE_COMMAND_CONTEXT(Device->GetDefaultCommandContext());
#if USE_PIX
				if (FD3D12DynamicRHI::GetD3DRHI()->IsPixEventEnabled())
				{
					PIXEndEvent(NativeCmdList.GraphicsCommandList());
				}
#endif
			}

			Device->GetDefaultCommandContext().ConditionalFlushCommandList();
		}
	}

private:
	struct FUpdateInfo
	{
		uint32 DstStartX;
		uint32 DstStartY;
		uint32 DstStartZ;
		FD3D12ResourceLocation* SrcResourceLocation;
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedSubresourceFootprint;
	};

	uint32 MipIdx;
	FTexture3DRHIRef DstTexture;
	TArray<FUpdateInfo> UpdateInfos;
};

// Single pair of transition barriers instead of one pair for each update
void FD3D12DynamicRHI::EndMultiUpdateTexture3D_RenderThread(class FRHICommandListImmediate& RHICmdList, TArray<FUpdateTexture3DData>& UpdateDataArray)
{
	check(IsInRenderingThread());
	check(UpdateDataArray.Num() > 0);
	check(GFrameNumberRenderThread == UpdateDataArray[0].FrameNumber);
#if DO_CHECK
	for (FUpdateTexture3DData& UpdateData : UpdateDataArray)
	{
		check(UpdateData.FrameNumber == UpdateDataArray[0].FrameNumber);
		check(UpdateData.MipIndex == UpdateDataArray[0].MipIndex);
		check(UpdateData.Texture == UpdateDataArray[0].Texture);
		FD3D12UpdateTexture3DData* UpdateDataD3D12 =
			reinterpret_cast<FD3D12UpdateTexture3DData*>(&UpdateData.PlatformData[0]);
		check(!!UpdateDataD3D12->UploadHeapResourceLocation);
		check(UpdateDataD3D12->bComputeShaderCopy ==
			reinterpret_cast<FD3D12UpdateTexture3DData*>(&UpdateDataArray[0].PlatformData[0])->bComputeShaderCopy);
	}
#endif

	bool bComputeShaderCopy = reinterpret_cast<FD3D12UpdateTexture3DData*>(&UpdateDataArray[0].PlatformData[0])->bComputeShaderCopy;

	if (bComputeShaderCopy)
	{
		// TODO: implement proper EndMultiUpdate for the compute shader path
		for (int32 Idx = 0; Idx < UpdateDataArray.Num(); ++Idx)
		{
			FUpdateTexture3DData& UpdateData = UpdateDataArray[Idx];
			FD3D12UpdateTexture3DData* UpdateDataD3D12 =
				reinterpret_cast<FD3D12UpdateTexture3DData*>(&UpdateData.PlatformData[0]);
			EndUpdateTexture3D_ComputeShader(UpdateData, UpdateDataD3D12);
		}
	}
	else
	{
		if (RHICmdList.Bypass())
		{
			FD3D12RHICmdEndMultiUpdateTexture3D RHICmd(UpdateDataArray);
			RHICmd.Execute(RHICmdList);
		}
		else
		{
			new (RHICmdList.AllocCommand<FD3D12RHICmdEndMultiUpdateTexture3D>()) FD3D12RHICmdEndMultiUpdateTexture3D(UpdateDataArray);
		}
	}
}

void FD3D12DynamicRHI::RHIUpdateTexture3D(FRHITexture3D* TextureRHI, uint32 MipIndex, const FUpdateTextureRegion3D& InUpdateRegion, uint32 SourceRowPitch, uint32 SourceDepthPitch, const uint8* SourceData)
{
	check(IsInRenderingThread());

	FD3D12Texture3D* Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	const FPixelFormatInfo& FormatInfo = GPixelFormats[Texture->GetFormat()];

	// Need to round up the height and with by block size.
	FUpdateTextureRegion3D UpdateRegion = InUpdateRegion;
	UpdateRegion.Width = FMath::DivideAndRoundUp<int32>(UpdateRegion.Width, FormatInfo.BlockSizeX) * FormatInfo.BlockSizeX;
	UpdateRegion.Height = FMath::DivideAndRoundUp<int32>(UpdateRegion.Height, FormatInfo.BlockSizeY) * FormatInfo.BlockSizeY;

	FUpdateTexture3DData UpdateData = BeginUpdateTexture3D_Internal(TextureRHI, MipIndex, UpdateRegion);

	// Copy the data into the UpdateData destination buffer
	check(nullptr != UpdateData.Data);
	check(SourceRowPitch <= UpdateData.RowPitch);
	check(SourceDepthPitch <= UpdateData.DepthPitch);

	const uint32 NumRows = UpdateRegion.Height / (uint32)FormatInfo.BlockSizeY;

	for (uint32 i = 0; i < UpdateRegion.Depth; i++)
	{
		uint8* DestRowData = UpdateData.Data + UpdateData.DepthPitch * i;
		const uint8* SourceRowData = SourceData + SourceDepthPitch * i;

		for (uint32 j = 0; j < NumRows; j++)
		{
			FMemory::Memcpy(DestRowData, SourceRowData, SourceRowPitch);
			SourceRowData += SourceRowPitch;
			DestRowData += UpdateData.RowPitch;
		}
	}

	EndUpdateTexture3D_Internal(UpdateData);
}


FUpdateTexture3DData FD3D12DynamicRHI::BeginUpdateTexture3D_Internal(FRHITexture3D* TextureRHI, uint32 MipIndex, const struct FUpdateTextureRegion3D& UpdateRegion)
{
	check(IsInRenderingThread());
	FUpdateTexture3DData UpdateData(TextureRHI, MipIndex, UpdateRegion, 0, 0, nullptr, 0, GFrameNumberRenderThread);

	// Initialize the platform data
	static_assert(sizeof(FD3D12UpdateTexture3DData) < sizeof(UpdateData.PlatformData), "Platform data in FUpdateTexture3DData too small to support D3D12");
	FD3D12UpdateTexture3DData* UpdateDataD3D12 = new (&UpdateData.PlatformData[0]) FD3D12UpdateTexture3DData;
	UpdateDataD3D12->bComputeShaderCopy = false;
	UpdateDataD3D12->UploadHeapResourceLocation = nullptr;

	FD3D12Texture3D* Texture = FD3D12DynamicRHI::ResourceCast(TextureRHI);
	const FPixelFormatInfo& FormatInfo = GPixelFormats[Texture->GetFormat()];
	check(FormatInfo.BlockSizeZ == 1);

	bool bDoComputeShaderCopy = false; // Compute shader can not cast compressed formats into uint
	if (CVarUseUpdateTexture3DComputeShader.GetValueOnRenderThread() != 0 && FormatInfo.BlockSizeX == 1 && FormatInfo.BlockSizeY == 1 && Texture->ResourceLocation.GetGPUVirtualAddress() && !EnumHasAnyFlags(Texture->GetFlags(), TexCreate_OfflineProcessed))
	{
		// Try a compute shader update. This does a memory allocation internally
		bDoComputeShaderCopy = BeginUpdateTexture3D_ComputeShader(UpdateData, UpdateDataD3D12);
	}

	if (!bDoComputeShaderCopy)
	{
	
		const int32 NumBlockX = FMath::DivideAndRoundUp<int32>(UpdateRegion.Width, FormatInfo.BlockSizeX);
		const int32 NumBlockY = FMath::DivideAndRoundUp<int32>(UpdateRegion.Height, FormatInfo.BlockSizeY);

		UpdateData.RowPitch = Align(NumBlockX * FormatInfo.BlockBytes, FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		UpdateData.DepthPitch = Align(UpdateData.RowPitch * NumBlockY, FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
		const uint32 BufferSize = Align(UpdateRegion.Depth * UpdateData.DepthPitch, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		UpdateData.DataSizeBytes = BufferSize;

		// This is a system memory heap so it doesn't matter which device we use.
		const uint32 HeapGPUIndex = 0;
		UpdateDataD3D12->UploadHeapResourceLocation = new FD3D12ResourceLocation(GetRHIDevice(HeapGPUIndex));

		//@TODO Probably need to use the TextureAllocator here to get correct tiling.
		// Currently the texture are allocated in linear, see hanlding around bVolume in FXboxOneTextureFormat::CompressImage(). 
		UpdateData.Data = (uint8*)GetRHIDevice(HeapGPUIndex)->GetDefaultFastAllocator().Allocate(BufferSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, UpdateDataD3D12->UploadHeapResourceLocation);

		check(UpdateData.Data != nullptr);
	}
	return UpdateData;
}

struct FD3D12RHICmdEndUpdateTexture3DString
{
	static const TCHAR* TStr() { return TEXT("FD3D12RHICmdEndUpdateTexture3D"); }
};
class FD3D12RHICmdEndUpdateTexture3D : public FRHICommand<FD3D12RHICmdEndUpdateTexture3D, FD3D12RHICmdEndUpdateTexture3DString>
{
public:
	FD3D12RHICmdEndUpdateTexture3D(FUpdateTexture3DData& UpdateData) :
		MipIdx(UpdateData.MipIndex),
		DstStartX(UpdateData.UpdateRegion.DestX),
		DstStartY(UpdateData.UpdateRegion.DestY),
		DstStartZ(UpdateData.UpdateRegion.DestZ),
		DstTexture(UpdateData.Texture)
	{
		FMemory::Memset(&PlacedSubresourceFootprint, 0, sizeof(PlacedSubresourceFootprint));

		D3D12_SUBRESOURCE_FOOTPRINT& SubresourceFootprint = PlacedSubresourceFootprint.Footprint;
		SubresourceFootprint.Depth = UpdateData.UpdateRegion.Depth;
		SubresourceFootprint.Height = UpdateData.UpdateRegion.Height;
		SubresourceFootprint.Width = UpdateData.UpdateRegion.Width;
		SubresourceFootprint.Format = static_cast<DXGI_FORMAT>(GPixelFormats[DstTexture->GetFormat()].PlatformFormat);
		SubresourceFootprint.RowPitch = UpdateData.RowPitch;
		check(SubresourceFootprint.RowPitch % FD3D12_TEXTURE_DATA_PITCH_ALIGNMENT == 0);

		FD3D12UpdateTexture3DData* UpdateDataD3D12 =
			reinterpret_cast<FD3D12UpdateTexture3DData*>(&UpdateData.PlatformData[0]);

		SrcResourceLocation = UpdateDataD3D12->UploadHeapResourceLocation;
		PlacedSubresourceFootprint.Offset = SrcResourceLocation->GetOffsetFromBaseOfResource();
	}

	virtual ~FD3D12RHICmdEndUpdateTexture3D()
	{
		if (SrcResourceLocation)
		{
			delete SrcResourceLocation;
			SrcResourceLocation = nullptr;
		}
	}

	void Execute(FRHICommandListBase& RHICmdList)
	{
		FD3D12Texture3D* NativeTexture = FD3D12DynamicRHI::ResourceCast(DstTexture.GetReference());
		FD3D12Resource* UploadBuffer = SrcResourceLocation->GetResource();

		for (FD3D12TextureBase& TextureLinkBase : *NativeTexture)
		{
			FD3D12Texture3D& TextureLink = static_cast<FD3D12Texture3D&>(TextureLinkBase);
			FD3D12Device* Device = TextureLink.GetParentDevice();
			FD3D12CommandListHandle& NativeCmdList = Device->GetDefaultCommandContext().CommandListHandle;
#if USE_PIX
			if (FD3D12DynamicRHI::GetD3DRHI()->IsPixEventEnabled())
			{
				PIXBeginEvent(NativeCmdList.GraphicsCommandList(), PIX_COLOR(255, 255, 255), TEXT("EndUpdateTexture3D"));
			}
#endif
			CD3DX12_TEXTURE_COPY_LOCATION DestCopyLocation(TextureLink.GetResource()->GetResource(), MipIdx);
			CD3DX12_TEXTURE_COPY_LOCATION SourceCopyLocation(UploadBuffer->GetResource(), PlacedSubresourceFootprint);

			FScopedResourceBarrier ScopeResourceBarrierDest(
				NativeCmdList,
				TextureLink.GetResource(),
				D3D12_RESOURCE_STATE_COPY_DEST,
				DestCopyLocation.SubresourceIndex,
				FD3D12DynamicRHI::ETransitionMode::Apply);

			Device->GetDefaultCommandContext().numCopies++;
			NativeCmdList.FlushResourceBarriers();
			NativeCmdList->CopyTextureRegion(
				&DestCopyLocation,
				DstStartX,
				DstStartY,
				DstStartZ,
				&SourceCopyLocation,
				nullptr);

			NativeCmdList.UpdateResidency(TextureLink.GetResource());

			Device->GetDefaultCommandContext().ConditionalFlushCommandList();
			DEBUG_EXECUTE_COMMAND_CONTEXT(Device->GetDefaultCommandContext());
#if USE_PIX
			if (FD3D12DynamicRHI::GetD3DRHI()->IsPixEventEnabled())
			{
				PIXEndEvent(NativeCmdList.GraphicsCommandList());
			}
#endif
		}

		delete SrcResourceLocation;
		SrcResourceLocation = nullptr;
	}

private:
	uint32 MipIdx;
	uint32 DstStartX;
	uint32 DstStartY;
	uint32 DstStartZ;
	FTexture3DRHIRef DstTexture;
	FD3D12ResourceLocation* SrcResourceLocation;
	D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedSubresourceFootprint;
};

void FD3D12DynamicRHI::EndUpdateTexture3D_Internal(FUpdateTexture3DData& UpdateData)
{
	check(IsInRenderingThread());
	check(GFrameNumberRenderThread == UpdateData.FrameNumber);

	FD3D12UpdateTexture3DData* UpdateDataD3D12 = reinterpret_cast<FD3D12UpdateTexture3DData*>(&UpdateData.PlatformData[0]);
	check( UpdateDataD3D12->UploadHeapResourceLocation != nullptr );

	if (UpdateDataD3D12->bComputeShaderCopy)
	{
		EndUpdateTexture3D_ComputeShader(UpdateData, UpdateDataD3D12);
	}
	else
	{
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		if (RHICmdList.Bypass())
		{
			FD3D12RHICmdEndUpdateTexture3D RHICmd(UpdateData);
			RHICmd.Execute(RHICmdList);
		}
		else
		{
			ALLOC_COMMAND_CL(RHICmdList, FD3D12RHICmdEndUpdateTexture3D)(UpdateData);
		}
	}
}

/*-----------------------------------------------------------------------------
	Cubemap texture support.
	-----------------------------------------------------------------------------*/
FTextureCubeRHIRef FD3D12DynamicRHI::RHICreateTextureCube_RenderThread(class FRHICommandListImmediate& RHICmdList, uint32 Size, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	return CreateD3D12Texture2D<FD3D12BaseTextureCube>(&RHICmdList, Size, Size, 6, false, true, (EPixelFormat) Format, NumMips, 1, Flags, InResourceState, CreateInfo);
}

FTextureCubeRHIRef FD3D12DynamicRHI::RHICreateTextureCube(uint32 Size, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	return CreateD3D12Texture2D<FD3D12BaseTextureCube>(nullptr, Size, Size, 6, false, true, (EPixelFormat) Format, NumMips, 1, Flags, InResourceState, CreateInfo);
}

FTextureCubeRHIRef FD3D12DynamicRHI::RHICreateTextureCubeArray_RenderThread(class FRHICommandListImmediate& RHICmdList, uint32 Size, uint32 ArraySize, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	return CreateD3D12Texture2D<FD3D12BaseTextureCube>(&RHICmdList, Size, Size, 6 * ArraySize, true, true, (EPixelFormat) Format, NumMips, 1, Flags, InResourceState, CreateInfo);
}

FTextureCubeRHIRef FD3D12DynamicRHI::RHICreateTextureCubeArray(uint32 Size, uint32 ArraySize, uint8 Format, uint32 NumMips, ETextureCreateFlags Flags, ERHIAccess InResourceState, FRHIResourceCreateInfo& CreateInfo)
{
	return CreateD3D12Texture2D<FD3D12BaseTextureCube>(nullptr, Size, Size, 6 * ArraySize, true, true, (EPixelFormat) Format, NumMips, 1, Flags, InResourceState, CreateInfo);
}

void* FD3D12DynamicRHI::RHILockTextureCubeFace(FRHITextureCube* TextureCubeRHI, uint32 FaceIndex, uint32 ArrayIndex, uint32 MipIndex, EResourceLockMode LockMode, uint32& DestStride, bool bLockWithinMiptail)
{
	FD3D12TextureCube*  TextureCube = FD3D12DynamicRHI::ResourceCast(TextureCubeRHI);
	for (uint32 GPUIndex : TextureCube->GetLinkedObjectsGPUMask())
	{
		GetRHIDevice(GPUIndex)->GetDefaultCommandContext().ConditionalClearShaderResource(&TextureCube->ResourceLocation);
	}
	uint32 D3DFace = GetD3D12CubeFace((ECubeFace)FaceIndex);
	return TextureCube->Lock(nullptr, MipIndex, D3DFace + ArrayIndex * 6, LockMode, DestStride);
}
void FD3D12DynamicRHI::RHIUnlockTextureCubeFace(FRHITextureCube* TextureCubeRHI, uint32 FaceIndex, uint32 ArrayIndex, uint32 MipIndex, bool bLockWithinMiptail)
{
	FD3D12TextureCube*  TextureCube = FD3D12DynamicRHI::ResourceCast(TextureCubeRHI);
	uint32 D3DFace = GetD3D12CubeFace((ECubeFace)FaceIndex);
	TextureCube->Unlock(nullptr, MipIndex, D3DFace + ArrayIndex * 6);
}

void FD3D12DynamicRHI::RHIBindDebugLabelName(FRHITexture* TextureRHI, const TCHAR* Name)
{
#if NAME_OBJECTS
	FD3D12TextureBase::FLinkedObjectIterator BaseTexture(GetD3D12TextureFromRHITexture(TextureRHI));

	if (GNumExplicitGPUsForRendering > 1)
	{
		// Generate string of the form "Name (GPU #)" -- assumes GPU index is a single digit.  This is called many times
		// a frame, so we want to avoid any string functions which dynamically allocate, to reduce perf overhead.
		static_assert(MAX_NUM_GPUS <= 10);

		static const TCHAR NameSuffix[] = TEXT(" (GPU #)");
		constexpr int32 NameSuffixLengthWithTerminator = (int32)UE_ARRAY_COUNT(NameSuffix);
		constexpr int32 NameBufferLength = 256;
		constexpr int32 GPUIndexSuffixOffset = 6;		// Offset of '#' character

		// Combine Name and suffix in our string buffer (clamping the length for bounds checking).  We'll replace the GPU index
		// with the appropriate digit in the loop.
		int32 NameLength = FMath::Min(FCString::Strlen(Name), NameBufferLength - NameSuffixLengthWithTerminator);
		int32 GPUIndexOffset = NameLength + GPUIndexSuffixOffset;

		TCHAR DebugName[NameBufferLength];
		FMemory::Memcpy(&DebugName[0], Name, NameLength * sizeof(TCHAR));
		FMemory::Memcpy(&DebugName[NameLength], NameSuffix, NameSuffixLengthWithTerminator * sizeof(TCHAR));

		for (; BaseTexture; ++BaseTexture)
		{
			FD3D12Resource* Resource = BaseTexture->GetResource();

			DebugName[GPUIndexOffset] = TEXT('0') + BaseTexture->GetParentDevice()->GetGPUIndex();

			SetName(Resource, DebugName);
		}
	}
	else
	{
		SetName(BaseTexture->GetResource(), Name);
	}
#endif

	// Also set on RHI object
	TextureRHI->SetName(Name);

#if TEXTURE_PROFILER_ENABLED

	FD3D12TextureBase* D3D12Texture = GetD3D12TextureFromRHITexture(TextureRHI);
	
	if (!EnumHasAnyFlags(TextureRHI->GetFlags(), TexCreate_Virtual)
		&& !D3D12Texture->ResourceLocation.IsTransient()
		&& D3D12Texture->ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eAliased
		&& D3D12Texture->ResourceLocation.GetType() != FD3D12ResourceLocation::ResourceLocationType::eHeapAliased)
	{
		FTextureProfiler::Get()->UpdateTextureName(TextureRHI);
	}
	
#endif
}

void FD3D12DynamicRHI::RHIVirtualTextureSetFirstMipInMemory(FRHITexture2D* TextureRHI, uint32 FirstMip)
{
}

void FD3D12DynamicRHI::RHIVirtualTextureSetFirstMipVisible(FRHITexture2D* TextureRHI, uint32 FirstMip)
{
}

ID3D12CommandQueue* FD3D12DynamicRHI::RHIGetD3DCommandQueue()
{
	// Multi-GPU support : any code using this function needs validation.
	return GetAdapter().GetDevice(0)->GetCommandListManager().GetD3DCommandQueue();
}


template<typename BaseResourceType>
TD3D12Texture2D<BaseResourceType>* FD3D12DynamicRHI::CreateTextureFromResource(bool bTextureArray, bool bCubeTexture, EPixelFormat Format, ETextureCreateFlags TexCreateFlags, const FClearValueBinding& ClearValueBinding, ID3D12Resource* Resource)
{
	check(Resource);
	FD3D12Adapter* Adapter = &GetAdapter();

	D3D12_RESOURCE_DESC TextureDesc = Resource->GetDesc();
	TextureDesc.Alignment = 0;

	uint32 SizeX = TextureDesc.Width;
	uint32 SizeY = TextureDesc.Height;
	uint32 SizeZ = TextureDesc.DepthOrArraySize;
	uint32 NumMips = TextureDesc.MipLevels;
	uint32 NumSamples = TextureDesc.SampleDesc.Count;
	
	check(TextureDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);
	check(bTextureArray || (!bCubeTexture && SizeZ == 1) || (bCubeTexture && SizeZ == 6));

	//TODO: Somehow Oculus is creating a Render Target with 4k alignment with ovr_GetTextureSwapChainBufferDX
	//      This is invalid and causes our size calculation to fail. Oculus SDK bug?
	if (TextureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
	{
		TextureDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	}

	SCOPE_CYCLE_COUNTER(STAT_D3D12CreateTextureTime);

	const bool bSRGB = EnumHasAnyFlags(TexCreateFlags, TexCreate_SRGB);

	const DXGI_FORMAT PlatformResourceFormat = TextureDesc.Format;
	const DXGI_FORMAT PlatformShaderResourceFormat = FindShaderResourceDXGIFormat(PlatformResourceFormat, bSRGB);
	const DXGI_FORMAT PlatformRenderTargetFormat = FindShaderResourceDXGIFormat(PlatformResourceFormat, bSRGB);

	// Set up the texture bind flags.
	bool bCreateRTV = (TextureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
	bool bCreateDSV = (TextureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
	bool bCreateShaderResource = (TextureDesc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) == 0;

	// DXGI_FORMAT_NV12 allows us to create RTV and SRV but only with other formats, so we should block creation here.
	if (Format == PF_NV12)
	{
		bCreateRTV = false;
		bCreateShaderResource = false;
	}

	// The state this resource will be in when it leaves this function
	const FD3D12Resource::FD3D12ResourceTypeHelper Type(TextureDesc, D3D12_HEAP_TYPE_DEFAULT);
	const D3D12_RESOURCE_STATES DestinationState = Type.GetOptimalInitialState(ERHIAccess::Unknown, !EnumHasAnyFlags(TexCreateFlags, TexCreate_Shared));

	FD3D12Device* Device = Adapter->GetDevice(0);
	FD3D12Resource* TextureResource = new FD3D12Resource(Device, Device->GetGPUMask(), Resource, DestinationState, TextureDesc);
	TextureResource->AddRef();

	TD3D12Texture2D<BaseResourceType>* Texture2D = Adapter->CreateLinkedObject<TD3D12Texture2D<BaseResourceType>>(Device->GetGPUMask(), [&](FD3D12Device* Device)
	{
		return new TD3D12Texture2D<BaseResourceType>(Device, SizeX, SizeY, SizeZ, NumMips, NumSamples, Format, false, TexCreateFlags, ClearValueBinding);
	});

	FD3D12ResourceLocation& Location = Texture2D->ResourceLocation;
	Location.SetType(FD3D12ResourceLocation::ResourceLocationType::eAliased);
	Location.SetResource(TextureResource);
	Location.SetGPUVirtualAddress(TextureResource->GetGPUVirtualAddress());

	uint32 RTVIndex = 0;

	if (bCreateRTV)
	{
		const bool bCreateRTVsPerSlice = EnumHasAnyFlags(TexCreateFlags, TexCreate_TargetArraySlicesIndependently) && (bTextureArray || bCubeTexture);
		Texture2D->SetNumRenderTargetViews(bCreateRTVsPerSlice ? NumMips * TextureDesc.DepthOrArraySize : NumMips);

		// Create a render target view for each mip
		for (uint32 MipIndex = 0; MipIndex < NumMips; MipIndex++)
		{
			if (bCreateRTVsPerSlice)
			{
				Texture2D->SetCreatedRTVsPerSlice(true, TextureDesc.DepthOrArraySize);

				for (uint32 SliceIndex = 0; SliceIndex < TextureDesc.DepthOrArraySize; SliceIndex++)
				{
					D3D12_RENDER_TARGET_VIEW_DESC RTVDesc;
					FMemory::Memzero(&RTVDesc, sizeof(RTVDesc));
					RTVDesc.Format = PlatformRenderTargetFormat;
					RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
					RTVDesc.Texture2DArray.FirstArraySlice = SliceIndex;
					RTVDesc.Texture2DArray.ArraySize = 1;
					RTVDesc.Texture2DArray.MipSlice = MipIndex;
					RTVDesc.Texture2DArray.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, RTVDesc.Format);

					Texture2D->SetRenderTargetViewIndex(new FD3D12RenderTargetView(Device, RTVDesc, Texture2D), RTVIndex++);
				}
			}
			else
			{
				D3D12_RENDER_TARGET_VIEW_DESC RTVDesc;
				FMemory::Memzero(&RTVDesc, sizeof(RTVDesc));
				RTVDesc.Format = PlatformRenderTargetFormat;
				if (bTextureArray || bCubeTexture)
				{
					RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
					RTVDesc.Texture2DArray.FirstArraySlice = 0;
					RTVDesc.Texture2DArray.ArraySize = TextureDesc.DepthOrArraySize;
					RTVDesc.Texture2DArray.MipSlice = MipIndex;
					RTVDesc.Texture2DArray.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, RTVDesc.Format);
				}
				else if (NumSamples == 1)
				{
					RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
					RTVDesc.Texture2D.MipSlice = MipIndex;
					RTVDesc.Texture2D.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, RTVDesc.Format);
				}
				else
				{
					RTVDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
				}

				Texture2D->SetRenderTargetViewIndex(new FD3D12RenderTargetView(Device, RTVDesc, Texture2D), RTVIndex++);
			}
		}
	}

	if (bCreateDSV)
	{
		// Create a depth-stencil-view for the texture.
		D3D12_DEPTH_STENCIL_VIEW_DESC DSVDesc = {};
		DSVDesc.Format = FindDepthStencilDXGIFormat(PlatformResourceFormat);
		if (bTextureArray || bCubeTexture)
		{
			DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
			DSVDesc.Texture2DArray.FirstArraySlice = 0;
			DSVDesc.Texture2DArray.ArraySize = TextureDesc.DepthOrArraySize;
			DSVDesc.Texture2DArray.MipSlice = 0;
		}
		else if (NumSamples == 1)
		{
			DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			DSVDesc.Texture2D.MipSlice = 0;
		}
		else
		{
			DSVDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
		}

		const bool HasStencil = HasStencilBits(DSVDesc.Format);
		for (uint32 AccessType = 0; AccessType < FExclusiveDepthStencil::MaxIndex; ++AccessType)
		{
			// Create a read-only access views for the texture.
			DSVDesc.Flags = (AccessType & FExclusiveDepthStencil::DepthRead_StencilWrite) ? D3D12_DSV_FLAG_READ_ONLY_DEPTH : D3D12_DSV_FLAG_NONE;
			if (HasStencil)
			{
				DSVDesc.Flags |= (AccessType & FExclusiveDepthStencil::DepthWrite_StencilRead) ? D3D12_DSV_FLAG_READ_ONLY_STENCIL : D3D12_DSV_FLAG_NONE;
			}

			Texture2D->SetDepthStencilView(new FD3D12DepthStencilView(Device, DSVDesc, Texture2D, HasStencil), AccessType);
		}
	}

	// Create a shader resource view for the texture.
	D3D12_SHADER_RESOURCE_VIEW_DESC SRVDesc = {};
	SRVDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
	SRVDesc.Format = PlatformShaderResourceFormat;

	if (bCubeTexture && bTextureArray)
	{
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBEARRAY;
		SRVDesc.TextureCubeArray.MostDetailedMip = 0;
		SRVDesc.TextureCubeArray.MipLevels = NumMips;
		SRVDesc.TextureCubeArray.ResourceMinLODClamp = 0.0f;
		SRVDesc.TextureCubeArray.First2DArrayFace = 0;
		SRVDesc.TextureCubeArray.NumCubes = SizeZ / 6;
	}
	else if (bCubeTexture)
	{
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
		SRVDesc.TextureCube.MostDetailedMip = 0;
		SRVDesc.TextureCube.MipLevels = NumMips;
		SRVDesc.TextureCube.ResourceMinLODClamp = 0.0f;
	}
	else if (bTextureArray)
	{
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
		SRVDesc.Texture2DArray.MostDetailedMip = 0;
		SRVDesc.Texture2DArray.MipLevels = NumMips;
		SRVDesc.Texture2DArray.FirstArraySlice = 0;
		SRVDesc.Texture2DArray.ArraySize = SizeZ;
		SRVDesc.Texture2DArray.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, SRVDesc.Format);
	}
	else if (NumSamples == 1)
	{
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		SRVDesc.Texture2D.MostDetailedMip = 0;
		SRVDesc.Texture2D.MipLevels = NumMips;
		SRVDesc.Texture2D.PlaneSlice = GetPlaneSliceFromViewFormat(PlatformResourceFormat, SRVDesc.Format);
	}
	else
	{
		SRVDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
	}

	// Create a wrapper for the SRV and set it on the texture
	if (bCreateShaderResource)
	{
		Texture2D->SetShaderResourceView(new FD3D12ShaderResourceView(Device, SRVDesc, Texture2D));
	}

	FD3D12TextureStats::D3D12TextureAllocated(*Texture2D);

	return Texture2D;
}

FTexture2DRHIRef FD3D12DynamicRHI::RHICreateTexture2DFromResource(EPixelFormat Format, ETextureCreateFlags TexCreateFlags, const FClearValueBinding& ClearValueBinding, ID3D12Resource* Resource)
{
	return CreateTextureFromResource<FD3D12BaseTexture2D>(false, false, Format, TexCreateFlags, ClearValueBinding, Resource);
}

FTexture2DRHIRef FD3D12DynamicRHI::RHICreateTexture2DArrayFromResource(EPixelFormat Format, ETextureCreateFlags TexCreateFlags, const FClearValueBinding& ClearValueBinding, ID3D12Resource* Resource)
{
	return CreateTextureFromResource<FD3D12BaseTexture2DArray>(true, false, Format, TexCreateFlags, ClearValueBinding, Resource);
}

FTextureCubeRHIRef FD3D12DynamicRHI::RHICreateTextureCubeFromResource(EPixelFormat Format, ETextureCreateFlags TexCreateFlags, const FClearValueBinding& ClearValueBinding, ID3D12Resource* Resource)
{
	return CreateTextureFromResource<FD3D12BaseTextureCube>(false, true, Format, TexCreateFlags, ClearValueBinding, Resource);
}

void FD3D12DynamicRHI::RHIAliasTextureResources(FRHITexture* DestTextureRHI, FRHITexture* SrcTextureRHI)
{
	FD3D12TextureBase* DestTexture = GetD3D12TextureFromRHITexture(DestTextureRHI);
	FD3D12TextureBase* SrcTexture = GetD3D12TextureFromRHITexture(SrcTextureRHI);

	// This path will potentially cause crashes, if the source texture is destroyed and we're still being used. This
	// API path will be deprecated post 4.25. To avoid issues, use the version that takes FTextureRHIRef references instead.
	check(false);

	for (FD3D12TextureBase::FDualLinkedObjectIterator It(DestTexture, SrcTexture); It; ++It)
	{
		DestTexture = It.GetFirst();
		SrcTexture = It.GetSecond();

		DestTexture->AliasResources(SrcTexture);
	}
}

void FD3D12DynamicRHI::RHIAliasTextureResources(FTextureRHIRef& DestTextureRHI, FTextureRHIRef& SrcTextureRHI)
{
	FD3D12TextureBase* DestTexture = GetD3D12TextureFromRHITexture(DestTextureRHI);
	FD3D12TextureBase* SrcTexture = GetD3D12TextureFromRHITexture(SrcTextureRHI);

	// Make sure we keep a reference to the source texture we're aliasing, so we don't lose it if all other references
	// go away but we're kept around.
	DestTexture->SetAliasingSource(SrcTextureRHI);

	for (FD3D12TextureBase::FDualLinkedObjectIterator It(DestTexture, SrcTexture); It; ++It)
	{
		FD3D12TextureBase* DestLinkedTexture = It.GetFirst();
		FD3D12TextureBase* SrcLinkedTexture = It.GetSecond();

		DestLinkedTexture->AliasResources(SrcLinkedTexture);
	}
}

template<typename BaseResourceType>
TD3D12Texture2D<BaseResourceType>* FD3D12DynamicRHI::CreateAliasedD3D12Texture2D(TD3D12Texture2D<BaseResourceType>* SourceTexture)
{
	FD3D12Adapter* Adapter = &GetAdapter();

	D3D12_RESOURCE_DESC TextureDesc = SourceTexture->GetResource()->GetDesc();
	TextureDesc.Alignment = 0;

	uint32 SizeX = TextureDesc.Width;
	uint32 SizeY = TextureDesc.Height;
	uint32 SizeZ = TextureDesc.DepthOrArraySize;
	uint32 NumMips = TextureDesc.MipLevels;
	uint32 NumSamples = TextureDesc.SampleDesc.Count;

	check(TextureDesc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D);

	//TODO: Somehow Oculus is creating a Render Target with 4k alignment with ovr_GetTextureSwapChainBufferDX
	//      This is invalid and causes our size calculation to fail. Oculus SDK bug?
	if (TextureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)
	{
		TextureDesc.Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;
	}

	SCOPE_CYCLE_COUNTER(STAT_D3D12CreateTextureTime);

	FD3D12Device* Device = Adapter->GetDevice(0);

	const bool bSRGB = EnumHasAnyFlags(SourceTexture->GetFlags(), TexCreate_SRGB);

	const DXGI_FORMAT PlatformResourceFormat = TextureDesc.Format;
	const DXGI_FORMAT PlatformShaderResourceFormat = FindShaderResourceDXGIFormat(PlatformResourceFormat, bSRGB);
	const DXGI_FORMAT PlatformRenderTargetFormat = FindShaderResourceDXGIFormat(PlatformResourceFormat, bSRGB);

	TD3D12Texture2D<BaseResourceType>* Texture2D = Adapter->CreateLinkedObject<TD3D12Texture2D<BaseResourceType>>(Device->GetGPUMask(), [&](FD3D12Device* Device)
	{
		return new TD3D12Texture2D<BaseResourceType>(Device, SizeX, SizeY, SizeZ, NumMips, NumSamples, SourceTexture->GetFormat(), false, SourceTexture->GetFlags(), SourceTexture->GetClearBinding());
	});

	// Set up the texture bind flags.
	bool bCreateRTV = (TextureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
	bool bCreateDSV = (TextureDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;

	const D3D12_RESOURCE_STATES State = D3D12_RESOURCE_STATE_COMMON;

	bool bCreatedRTVPerSlice = false;
	const bool bCubeTexture = SourceTexture->IsCubemap();
	const bool bTextureArray = !bCubeTexture && TextureDesc.DepthOrArraySize > 1;

	if (bCreateRTV)
	{
		Texture2D->SetCreatedRTVsPerSlice(false, NumMips);
		Texture2D->SetNumRenderTargetViews(NumMips);

		// Create a render target view for each array index and mip index.
		for (uint32 MipIndex = 0; MipIndex < TextureDesc.MipLevels; MipIndex++)
		{
			// These are null because we'll be aliasing them shortly.
			if (EnumHasAnyFlags(SourceTexture->Flags, TexCreate_TargetArraySlicesIndependently) && (bTextureArray || bCubeTexture))
			{
				bCreatedRTVPerSlice = true;

				for (uint32 SliceIndex = 0; SliceIndex < TextureDesc.DepthOrArraySize; SliceIndex++)
				{
					Texture2D->SetRenderTargetViewIndex(nullptr, SliceIndex * NumMips + MipIndex);
				}
			}
			else
			{
				Texture2D->SetRenderTargetViewIndex(nullptr, MipIndex);
			}
		}
	}

	if (bCreateDSV)
	{
		// Create a depth-stencil-view for the texture.
		for (uint32 AccessType = 0; AccessType < FExclusiveDepthStencil::MaxIndex; ++AccessType)
		{
			Texture2D->SetDepthStencilView(nullptr, AccessType);
		}
	}

	RHIAliasTextureResources((FTextureRHIRef&)Texture2D, (FTextureRHIRef&)SourceTexture);

	return Texture2D;
}

FTextureRHIRef FD3D12DynamicRHI::RHICreateAliasedTexture(FRHITexture* SourceTextureRHI)
{
	FD3D12TextureBase* SourceTexture = GetD3D12TextureFromRHITexture(SourceTextureRHI);
	if (SourceTextureRHI->GetTexture2D() != nullptr)
	{
		return CreateAliasedD3D12Texture2D<FD3D12BaseTexture2D>(ResourceCast(SourceTextureRHI->GetTexture2D()));
	}
	else if (SourceTextureRHI->GetTexture2DArray() != nullptr)
	{
		return CreateAliasedD3D12Texture2D<FD3D12BaseTexture2DArray>(ResourceCast(SourceTextureRHI->GetTexture2DArray()));
	}
	else if (SourceTextureRHI->GetTextureCube() != nullptr)
	{
		return CreateAliasedD3D12Texture2D<FD3D12BaseTextureCube>(ResourceCast(SourceTextureRHI->GetTextureCube()));
	}

	UE_LOG(LogD3D12RHI, Error, TEXT("Currently FD3D12DynamicRHI::RHICreateAliasedTexture only supports 2D, 2D Array and Cube textures."));
	return nullptr;
}

FTextureRHIRef FD3D12DynamicRHI::RHICreateAliasedTexture(FTextureRHIRef& SourceTextureRHI)
{
	FD3D12TextureBase* SourceTexture = GetD3D12TextureFromRHITexture(SourceTextureRHI);
	FTextureRHIRef ReturnTexture;
	if (SourceTextureRHI->GetTexture2D() != nullptr)
	{
		ReturnTexture = CreateAliasedD3D12Texture2D<FD3D12BaseTexture2D>(static_cast<FD3D12Texture2D*>(SourceTextureRHI->GetTexture2D()));
	}
	else if (SourceTextureRHI->GetTexture2DArray() != nullptr)
	{
		ReturnTexture = CreateAliasedD3D12Texture2D<FD3D12BaseTexture2DArray>(static_cast<FD3D12Texture2DArray*>(SourceTextureRHI->GetTexture2DArray()));
	}
	else if (SourceTextureRHI->GetTextureCube() != nullptr)
	{
		ReturnTexture = CreateAliasedD3D12Texture2D<FD3D12BaseTextureCube>(static_cast<FD3D12TextureCube*>(SourceTextureRHI->GetTextureCube()));
	}

	if (ReturnTexture == nullptr)
	{
		UE_LOG(LogD3D12RHI, Error, TEXT("Currently FD3D12DynamicRHI::RHICreateAliasedTexture only supports 2D, 2D Array and Cube textures."));
		return nullptr;
	}

	FD3D12TextureBase* DestTexture = GetD3D12TextureFromRHITexture(ReturnTexture);
	DestTexture->SetAliasingSource(SourceTextureRHI);

	return ReturnTexture;
}

void FD3D12DynamicRHI::RHICopySubTextureRegion(FRHITexture2D* SourceTextureRHI, FRHITexture2D* DestTextureRHI, FBox2D SourceBox, FBox2D DestinationBox)
{
	FD3D12TextureBase* SourceTexture = GetD3D12TextureFromRHITexture(SourceTextureRHI);
	FD3D12TextureBase* DestTexture = GetD3D12TextureFromRHITexture(DestTextureRHI);

	const uint32 XOffset = (uint32)(DestinationBox.Min.X);
	const uint32 YOffset = (uint32)(DestinationBox.Min.Y);
	const uint32 Width = (uint32)(SourceBox.Max.X - SourceBox.Min.X);
	const uint32 Height = (uint32)(SourceBox.Max.Y - SourceBox.Min.Y);

	const CD3DX12_BOX SourceBoxD3D((LONG)SourceBox.Min.X, (LONG)SourceBox.Min.Y, (LONG)SourceBox.Max.X, (LONG)SourceBox.Max.Y);

	CD3DX12_TEXTURE_COPY_LOCATION DestCopyLocation(DestTexture->GetResource()->GetResource(), 0);
	CD3DX12_TEXTURE_COPY_LOCATION SourceCopyLocation(SourceTexture->GetResource()->GetResource(), 0);

	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	if (RHICmdList.Bypass())
	{
		FRHICommandCopySubTextureRegion RHICmd(DestTexture, XOffset, YOffset, 0, SourceTexture, SourceBoxD3D);
		RHICmd.Execute(RHICmdList);
	}
	else
	{
		ALLOC_COMMAND_CL(RHICmdList, FRHICommandCopySubTextureRegion)(DestTexture, XOffset, YOffset, 0, SourceTexture, SourceBoxD3D);
	}
}

void FD3D12CommandContext::RHICopyTexture(FRHITexture* SourceTextureRHI, FRHITexture* DestTextureRHI, const FRHICopyTextureInfo& CopyInfo)
{
	FD3D12TextureBase* SourceTexture = RetrieveTextureBase(SourceTextureRHI);
	FD3D12TextureBase* DestTexture = RetrieveTextureBase(DestTextureRHI);

	FScopedResourceBarrier ConditionalScopeResourceBarrierSource(CommandListHandle, SourceTexture->GetResource(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, FD3D12DynamicRHI::ETransitionMode::Validate);
	FScopedResourceBarrier ConditionalScopeResourceBarrierDest(CommandListHandle, DestTexture->GetResource(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, FD3D12DynamicRHI::ETransitionMode::Validate);

	numCopies++;
	CommandListHandle.FlushResourceBarriers();

	const bool bReadback = EnumHasAnyFlags(DestTextureRHI->GetFlags(), TexCreate_CPUReadback);

	if (CopyInfo.Size != FIntVector::ZeroValue || bReadback)
	{
		// Interpret zero size as source size
		const FIntVector CopySize = CopyInfo.Size == FIntVector::ZeroValue ? SourceTextureRHI->GetSizeXYZ() : CopyInfo.Size;

		// Copy sub texture regions
		const CD3DX12_BOX SourceBoxD3D(
			CopyInfo.SourcePosition.X,
			CopyInfo.SourcePosition.Y,
			CopyInfo.SourcePosition.Z,
			CopyInfo.SourcePosition.X + CopySize.X,
			CopyInfo.SourcePosition.Y + CopySize.Y,
			CopyInfo.SourcePosition.Z + CopySize.Z
		);

		D3D12_TEXTURE_COPY_LOCATION Src;
		Src.pResource = SourceTexture->GetResource()->GetResource();
		Src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

		D3D12_TEXTURE_COPY_LOCATION Dst;
		Dst.pResource = DestTexture->GetResource()->GetResource();
		Dst.Type = bReadback ? D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT : D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;

		const FPixelFormatInfo& SourcePixelFormatInfo = GPixelFormats[SourceTextureRHI->GetFormat()];
		const FPixelFormatInfo& DestPixelFormatInfo = GPixelFormats[DestTextureRHI->GetFormat()];

		D3D12_RESOURCE_DESC DstDesc = {};
		FIntVector TextureSize = DestTextureRHI->GetSizeXYZ();
		DstDesc.Dimension = DestTextureRHI->GetTexture3D() ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D; 
		DstDesc.Width = TextureSize.X;
		DstDesc.Height = TextureSize.Y;
		DstDesc.DepthOrArraySize = TextureSize.Z;
		DstDesc.MipLevels = DestTextureRHI->GetNumMips();
		DstDesc.Format = (DXGI_FORMAT)DestPixelFormatInfo.PlatformFormat;
		DstDesc.SampleDesc.Count = DestTextureRHI->GetNumSamples();

		for (uint32 SliceIndex = 0; SliceIndex < CopyInfo.NumSlices; ++SliceIndex)
		{
			uint32 SourceSliceIndex = CopyInfo.SourceSliceIndex + SliceIndex;
			uint32 DestSliceIndex   = CopyInfo.DestSliceIndex   + SliceIndex;

			for (uint32 MipIndex = 0; MipIndex < CopyInfo.NumMips; ++MipIndex)
			{
				uint32 SourceMipIndex = CopyInfo.SourceMipIndex + MipIndex;
				uint32 DestMipIndex   = CopyInfo.DestMipIndex   + MipIndex;

				CD3DX12_BOX MipSourceBoxD3D(
					SourceBoxD3D.left  >> MipIndex,
					SourceBoxD3D.top   >> MipIndex,
					SourceBoxD3D.front >> MipIndex,
					// Align to block size to pad the copy when processing the last surface texels.
					// This will give inconsistent results otherwise between different RHI.
					AlignArbitrary<uint32>(FMath::Max<uint32>(SourceBoxD3D.right  >> MipIndex, 1), SourcePixelFormatInfo.BlockSizeX),
					AlignArbitrary<uint32>(FMath::Max<uint32>(SourceBoxD3D.bottom >> MipIndex, 1), SourcePixelFormatInfo.BlockSizeY),
					AlignArbitrary<uint32>(FMath::Max<uint32>(SourceBoxD3D.back   >> MipIndex, 1), SourcePixelFormatInfo.BlockSizeZ)
					);

				const uint32 DestX = CopyInfo.DestPosition.X >> MipIndex;
				const uint32 DestY = CopyInfo.DestPosition.Y >> MipIndex;
				const uint32 DestZ = CopyInfo.DestPosition.Z >> MipIndex;

				// RHICopyTexture is allowed to copy mip regions only if are aligned on the block size to prevent unexpected / inconsistent results.
				ensure(MipSourceBoxD3D.left % SourcePixelFormatInfo.BlockSizeX == 0 && MipSourceBoxD3D.top % SourcePixelFormatInfo.BlockSizeY == 0 && MipSourceBoxD3D.front % SourcePixelFormatInfo.BlockSizeZ == 0);
				ensure(DestX % DestPixelFormatInfo.BlockSizeX == 0 && DestY % DestPixelFormatInfo.BlockSizeY == 0 && DestZ % DestPixelFormatInfo.BlockSizeZ == 0);

				Src.SubresourceIndex = CalcSubresource(SourceMipIndex, SourceSliceIndex, SourceTextureRHI->GetNumMips());
				Dst.SubresourceIndex = CalcSubresource(DestMipIndex, DestSliceIndex, DestTextureRHI->GetNumMips());

				if (bReadback)
				{
					GetReadBackHeapDescImpl(Dst.PlacedFootprint, GetParentDevice()->GetDevice(), DstDesc, Dst.SubresourceIndex);
				}

				CommandListHandle->CopyTextureRegion(
					&Dst, 
					DestX, DestY, DestZ,
					&Src,
					&MipSourceBoxD3D
				);
			}
		}
	}
	else
	{
		// Copy whole texture
		CommandListHandle->CopyResource(DestTexture->GetResource()->GetResource(), SourceTexture->GetResource()->GetResource());
	}

	CommandListHandle.UpdateResidency(SourceTexture->GetResource());
	CommandListHandle.UpdateResidency(DestTexture->GetResource());
	
	ConditionalFlushCommandList();

	// Save the command list handle. This lets us check when this command list is complete. Note: This must be saved before we execute the command list
	DestTexture->SetReadBackListHandle(CommandListHandle);
}

FRHITexture* FD3D12BackBufferReferenceTexture2D::GetBackBufferTexture()
{
	return bIsSDR ? Viewport->GetSDRBackBuffer_RHIThread() : Viewport->GetBackBuffer_RHIThread();
}
