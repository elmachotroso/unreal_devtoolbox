// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
D3D12Texture.h: Implementation of D3D12 Texture
=============================================================================*/
#pragma once

/** If true, guard texture creates with SEH to log more information about a driver crash we are seeing during texture streaming. */
#define GUARDED_TEXTURE_CREATES (PLATFORM_WINDOWS && !(UE_BUILD_SHIPPING || UE_BUILD_TEST || PLATFORM_COMPILER_CLANG))

static bool TextureCanBe4KAligned(const FD3D12ResourceDesc& Desc, EPixelFormat UEFormat)
{
	// 4KB alignment is only available for read only textures
	if (!EnumHasAnyFlags(Desc.Flags, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
		!Desc.NeedsUAVAliasWorkarounds() && // UAV aliased resources are secretly writable.
		Desc.SampleDesc.Count == 1)
	{
		D3D12_TILE_SHAPE Tile = {};
		Get4KTileShape(&Tile, Desc.Format, UEFormat, Desc.Dimension, Desc.SampleDesc.Count);

		uint32 TilesNeeded = GetTilesNeeded(Desc.Width, Desc.Height, Desc.DepthOrArraySize, Tile);

		constexpr uint32 NUM_4K_BLOCKS_PER_64K_PAGE = 16;
		return TilesNeeded <= NUM_4K_BLOCKS_PER_64K_PAGE;
	}
	else
	{
		return false;
	}
}

void SafeCreateTexture2D(FD3D12Device* pDevice, 
	FD3D12Adapter* Adapter,
	const FD3D12ResourceDesc& TextureDesc,
	const D3D12_CLEAR_VALUE* ClearValue, 
	FD3D12ResourceLocation* OutTexture2D,
	FD3D12BaseShaderResource* Owner,
	EPixelFormat Format,
	ETextureCreateFlags Flags,
	D3D12_RESOURCE_STATES InitialState,
	const TCHAR* Name);

void CreateUAVAliasResource(
	FD3D12Adapter* Adapter,
	D3D12_CLEAR_VALUE* ClearValuePtr,
	const TCHAR* DebugName,
	FD3D12ResourceLocation& Location);

/** Texture base class. */
class FD3D12TextureBase : public FD3D12BaseShaderResource, public FD3D12LinkedAdapterObject<FD3D12TextureBase>
{
public:

	FD3D12TextureBase(class FD3D12Device* InParent)
		: FD3D12BaseShaderResource(InParent)
	{
	}

	virtual ~FD3D12TextureBase() {}

	inline void SetCreatedRTVsPerSlice(bool Value, int32 InRTVArraySize)
	{ 
		bCreatedRTVsPerSlice = Value;
		RTVArraySize = InRTVArraySize;
	}

	void SetNumRenderTargetViews(int32 InNumViews)
	{
		RenderTargetViews.Empty(InNumViews);
		RenderTargetViews.AddDefaulted(InNumViews);
	}

	void SetDepthStencilView(FD3D12DepthStencilView* View, uint32 SubResourceIndex)
	{
		if (SubResourceIndex < FExclusiveDepthStencil::MaxIndex)
		{
			DepthStencilViews[SubResourceIndex] = View;
		}
		else
		{
			check(false);
		}
	}

	void SetRenderTargetViewIndex(FD3D12RenderTargetView* View, uint32 SubResourceIndex)
	{
		if (SubResourceIndex < (uint32)RenderTargetViews.Num())
		{
			RenderTargetViews[SubResourceIndex] = View;
		}
		else
		{
			check(false);
		}
	}

	void SetRenderTargetView(FD3D12RenderTargetView* View)
	{
		RenderTargetViews.Empty(1);
		RenderTargetViews.Add(View);
	}

	int64 GetMemorySize() const
	{
		return MemorySize;
	}

	void SetMemorySize(int64 InMemorySize)
	{
		check(InMemorySize >= 0);
		MemorySize = InMemorySize;
	}

	void SetAliasingSource(FTextureRHIRef& SourceTextureRHI)
	{
		AliasingSourceTexture = SourceTextureRHI;
	}

	// Accessors.
	FD3D12Resource* GetResource() const { return ResourceLocation.GetResource(); }
	uint64 GetOffset() const { return ResourceLocation.GetOffsetFromBaseOfResource(); }
	FD3D12ShaderResourceView* GetShaderResourceView() const { return ShaderResourceView; }
	inline const FTextureRHIRef& GetAliasingSourceTexture() const { return AliasingSourceTexture; }

	void SetShaderResourceView(FD3D12ShaderResourceView* InShaderResourceView) { ShaderResourceView = InShaderResourceView; }

	static inline bool ShouldDeferCmdListOperation(FRHICommandList* RHICmdList)
	{
		if (RHICmdList == nullptr)
		{
			return false;
		}

		if (RHICmdList->Bypass() || !IsRunningRHIInSeparateThread())
		{
			return false;
		}

		return true;
	}

	void UpdateTexture(uint32 MipIndex, uint32 DestX, uint32 DestY, uint32 DestZ, const D3D12_TEXTURE_COPY_LOCATION& SourceCopyLocation);
	void CopyTextureRegion(uint32 DestX, uint32 DestY, uint32 DestZ, FD3D12TextureBase* SourceTexture, const D3D12_BOX& SourceBox);
	void InitializeTextureData(class FRHICommandListImmediate* RHICmdList, const void* InitData, uint32 InitDataSize, uint32 SizeX, uint32 SizeY, uint32 SizeZ, uint32 NumSlices, uint32 NumMips, EPixelFormat Format, D3D12_RESOURCE_STATES DestinationState);

	/**
	* Get the render target view for the specified mip and array slice.
	* An array slice of -1 is used to indicate that no array slice should be required.
	*/
	FD3D12RenderTargetView* GetRenderTargetView(int32 MipIndex, int32 ArraySliceIndex) const
	{
		int32 ArrayIndex = MipIndex;

		if (bCreatedRTVsPerSlice)
		{
			check(ArraySliceIndex >= 0);
			ArrayIndex = MipIndex * RTVArraySize + ArraySliceIndex;
			check(ArrayIndex < RenderTargetViews.Num());
		}
		else
		{
			// Catch attempts to use a specific slice without having created the texture to support it
			check(ArraySliceIndex == -1 || ArraySliceIndex == 0);
		}

		if (ArrayIndex < RenderTargetViews.Num())
		{
			return RenderTargetViews[ArrayIndex];
		}
		return 0;
	}
	FD3D12DepthStencilView* GetDepthStencilView(FExclusiveDepthStencil AccessType) const
	{
		return DepthStencilViews[AccessType.GetIndex()];
	}

	inline bool HasRenderTargetViews() const
	{
		return (RenderTargetViews.Num() > 0);
	}

#if PLATFORM_REQUIRES_TYPELESS_RESOURCE_DISCARD_WORKAROUND
	bool GetRequiresTypelessResourceDiscardWorkaround() const { return bRequiresTypelessResourceDiscardWorkaround; }
#endif // #if PLATFORM_REQUIRES_TYPELESS_RESOURCE_DISCARD_WORKAROUND

	void AliasResources(FD3D12TextureBase* Texture)
	{
		// Alias the location, will perform an addref underneath
		FD3D12ResourceLocation::Alias(ResourceLocation, Texture->ResourceLocation);

		ShaderResourceView = Texture->ShaderResourceView;

		for (uint32 Index = 0; Index < FExclusiveDepthStencil::MaxIndex; Index++)
		{
			DepthStencilViews[Index] = Texture->DepthStencilViews[Index];
		}
		for (int32 Index = 0; Index < Texture->RenderTargetViews.Num(); Index++)
		{
			RenderTargetViews[Index] = Texture->RenderTargetViews[Index];
		}
	}

	// Modifiers.
	void SetReadBackListHandle(FD3D12CommandListHandle listToWaitFor) { ReadBackSyncPoint = listToWaitFor; }
	FD3D12CLSyncPoint GetReadBackSyncPoint() const { return ReadBackSyncPoint; }

	FD3D12CLSyncPoint ReadBackSyncPoint;

#if PLATFORM_REQUIRES_TYPELESS_RESOURCE_DISCARD_WORKAROUND
	void SetRequiresTypelessResourceDiscardWorkaround(bool bInRequired) { bRequiresTypelessResourceDiscardWorkaround = bInRequired; }
#endif // #if PLATFORM_REQUIRES_TYPELESS_RESOURCE_DISCARD_WORKAROUND

protected:

	/** Amount of memory allocated by this texture, in bytes. */
	int64 MemorySize{};

	/** A shader resource view of the texture. */
	TRefCountPtr<FD3D12ShaderResourceView> ShaderResourceView;

	/** A render targetable view of the texture. */
	TArray<TRefCountPtr<FD3D12RenderTargetView>, TInlineAllocator<1>> RenderTargetViews;

	/** A depth-stencil targetable view of the texture. */
	TRefCountPtr<FD3D12DepthStencilView> DepthStencilViews[FExclusiveDepthStencil::MaxIndex];

	TMap<uint32, FD3D12LockedResource*> LockedMap;

	FTextureRHIRef AliasingSourceTexture;

	int32 RTVArraySize{};

	bool bCreatedRTVsPerSlice{ false };

#if PLATFORM_REQUIRES_TYPELESS_RESOURCE_DISCARD_WORKAROUND
	bool bRequiresTypelessResourceDiscardWorkaround = false;
#endif // #if PLATFORM_REQUIRES_TYPELESS_RESOURCE_DISCARD_WORKAROUND
};

#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
struct FD3D12TextureLayout {};
#endif

/** 2D texture (vanilla, cubemap or 2D array) */
template<typename BaseResourceType>
class TD3D12Texture2D : public BaseResourceType, public FD3D12TextureBase
{
public:

	/** Flags used when the texture was created */
	ETextureCreateFlags Flags;

	TD3D12Texture2D() = delete;

	/** Initialization constructor. */
	TD3D12Texture2D(
		class FD3D12Device* InParent,
		uint32 InSizeX,
		uint32 InSizeY,
		uint32 InSizeZ,
		uint32 InNumMips,
		uint32 InNumSamples,
		EPixelFormat InFormat,
		bool bInCubemap,
		ETextureCreateFlags InFlags,
		const FClearValueBinding& InClearValue,
		const FD3D12TextureLayout* InTextureLayout = nullptr
#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
		, void* InRawTextureMemory = nullptr
#endif
		)
		: BaseResourceType(
			InSizeX,
			InSizeY,
			InSizeZ,
			InNumMips,
			InNumSamples,
			InFormat,
			InFlags,
			InClearValue
			)
		, FD3D12TextureBase(InParent)
		, Flags(InFlags)
		, bCubemap(bInCubemap)
		, bStreamable(!!(InFlags & TexCreate_Streamable))
		, bMipOrderDescending(false)
#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
		, RawTextureMemory(InRawTextureMemory)
#endif
	{
		if (InTextureLayout == nullptr)
		{
			FMemory::Memzero(TextureLayout);
		}
		else
		{
			TextureLayout = *InTextureLayout;
#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
			bMipOrderDescending = InNumMips > 1u && TextureLayout.GetSubresourceOffset(0, 0, 0) > TextureLayout.GetSubresourceOffset(0, 1, 0);
#endif
		}
	}

	virtual ~TD3D12Texture2D();

	// FRHIResource overrides
#if RHI_ENABLE_RESOURCE_INFO
	bool GetResourceInfo(FRHIResourceInfo& OutResourceInfo) const override
	{
		OutResourceInfo = FRHIResourceInfo{};
		OutResourceInfo.Name = this->GetName();
		OutResourceInfo.Type = this->GetType();
		OutResourceInfo.VRamAllocation.AllocationSize = GetMemorySize();
		OutResourceInfo.IsTransient = this->ResourceLocation.IsTransient();
		return true;
	}
#endif

	/**
	* Locks one of the texture's mip-maps.
	* @return A pointer to the specified texture data.
	*/
	void* Lock(class FRHICommandListImmediate* RHICmdList, uint32 MipIndex, uint32 ArrayIndex, EResourceLockMode LockMode, uint32& DestStride);

	/** Unlocks a previously locked mip-map. */
	void Unlock(class FRHICommandListImmediate* RHICmdList, uint32 MipIndex, uint32 ArrayIndex);

	//* Update the contents of the Texture2D using a Copy command */
	void UpdateTexture2D(class FRHICommandListImmediate* RHICmdList, uint32 MipIndex, const struct FUpdateTextureRegion2D& UpdateRegion, uint32 SourcePitch, const uint8* SourceData);

	// Accessors.
	FD3D12Resource* GetResource() const { return (FD3D12Resource*)FD3D12TextureBase::GetResource(); }

	void GetReadBackHeapDesc(D3D12_PLACED_SUBRESOURCE_FOOTPRINT& OutFootprint, uint32 Subresource) const;

	bool IsCubemap() const { return bCubemap; }

	bool IsStreamable() const { return bStreamable; }

	bool IsLastMipFirst() const { return bMipOrderDescending; }

	/** FRHITexture override.  See FRHITexture::GetNativeResource() */
	virtual void* GetNativeResource() const override final
	{
		void* NativeResource = nullptr;
		FD3D12Resource* Resource = GetResource();
		if (Resource)
		{
			NativeResource = Resource->GetResource();
		}
		if (!NativeResource)
		{
			FD3D12TextureBase* Base = GetD3D12TextureFromRHITexture((FRHITexture*)this);
			if (Base)
			{
				Resource = Base->GetResource();
				if (Resource)
				{
					NativeResource = Resource->GetResource();
				}
			}
		}
		return NativeResource;
	}

	virtual void* GetTextureBaseRHI() override final
	{
		return static_cast<FD3D12TextureBase*>(this);
	}

	// IRefCountedObject interface.
	virtual uint32 AddRef() const
	{
		return FRHIResource::AddRef();
	}
	virtual uint32 Release() const
	{
		return FRHIResource::Release();
	}
	virtual uint32 GetRefCount() const
	{
		return FRHIResource::GetRefCount();
	}
#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
	void* GetRawTextureMemory() const
	{
		return RawTextureMemory;
	}

	void SetRawTextureMemory(void* Memory)
	{
		RawTextureMemory = Memory;
	}
	FPlatformMemory::FPlatformVirtualMemoryBlock& GetRawTextureBlock()
	{
		return RawTextureBlock;
	}
#endif

	const FD3D12TextureLayout& GetTextureLayout() const { return TextureLayout; }

private:
	/** Unlocks a previously locked mip-map. */
	void UnlockInternal(class FRHICommandListImmediate* RHICmdList, FLinkedObjectIterator NextObject, uint32 MipIndex, uint32 ArrayIndex);

	/** Whether the texture is a cube-map. */
	const uint32 bCubemap : 1;

	/** Whether the texture has been created with flag TexCreate_Streamable */
	const uint32 bStreamable : 1;

	/** Whether mips are ordered from the last to the first in memory */
	uint32 bMipOrderDescending : 1;

#if PLATFORM_SUPPORTS_VIRTUAL_TEXTURES
	void* RawTextureMemory;
	FPlatformMemory::FPlatformVirtualMemoryBlock RawTextureBlock;
#endif

	FD3D12TextureLayout TextureLayout;

	mutable TUniquePtr<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> FirstSubresourceFootprint;
};

/** 3D Texture */
class FD3D12Texture3D : public FRHITexture3D, public FD3D12TextureBase
{
public:
	FD3D12Texture3D() = delete;
	/** Initialization constructor. */
	FD3D12Texture3D(
	class FD3D12Device* InParent,
		uint32 InSizeX,
		uint32 InSizeY,
		uint32 InSizeZ,
		uint32 InNumMips,
		EPixelFormat InFormat,
		ETextureCreateFlags InFlags,
		const FClearValueBinding& InClearValue
		)
		: FRHITexture3D(InSizeX, InSizeY, InSizeZ, InNumMips, InFormat, InFlags, InClearValue)
		, FD3D12TextureBase(InParent)
		, bStreamable(!!(InFlags & TexCreate_Streamable))
	{
	}

	virtual ~FD3D12Texture3D();

	// FRHIResource overrides
#if RHI_ENABLE_RESOURCE_INFO
	bool GetResourceInfo(FRHIResourceInfo& OutResourceInfo) const override
	{
		OutResourceInfo = FRHIResourceInfo{};
		OutResourceInfo.Name = GetName();
		OutResourceInfo.Type = GetType();
		OutResourceInfo.VRamAllocation.AllocationSize = ResourceLocation.GetSize();
		OutResourceInfo.IsTransient = ResourceLocation.IsTransient();
		return true;
	}
#endif

	/** FRHITexture override.  See FRHITexture::GetNativeResource() */
	virtual void* GetNativeResource() const override final
	{
		FD3D12Resource* Resource = GetResource();
		return (Resource == nullptr) ? nullptr : Resource->GetResource();
	}

	// Accessors.
	FD3D12Resource* GetResource() const { return (FD3D12Resource*)FD3D12TextureBase::GetResource(); }

	virtual void* GetTextureBaseRHI() override final
	{
		return static_cast<FD3D12TextureBase*>(this);
	}

	// IRefCountedObject interface.
	virtual uint32 AddRef() const
	{
		return FRHIResource::AddRef();
	}
	virtual uint32 Release() const
	{
		return FRHIResource::Release();
	}
	virtual uint32 GetRefCount() const
	{
		return FRHIResource::GetRefCount();
	}

	bool IsStreamable() const { return bStreamable; }

private:
	/** Whether the texture has been created with flag TexCreate_Streamable */
	const uint32 bStreamable : 1;
};

class FD3D12BaseTexture2D : public FRHITexture2D, public FD3D12FastClearResource
{
public:
	FD3D12BaseTexture2D(uint32 InSizeX, uint32 InSizeY, uint32 InSizeZ, uint32 InNumMips, uint32 InNumSamples, EPixelFormat InFormat, ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue)
		: FRHITexture2D(InSizeX, InSizeY, InNumMips, InNumSamples, InFormat, InFlags, InClearValue)
	{}
	uint32 GetSizeZ() const { return 0; }

	virtual void GetWriteMaskProperties(void*& OutData, uint32& OutSize) override final
	{
		FD3D12FastClearResource::GetWriteMaskProperties(OutData, OutSize);
	}
};

class FD3D12BaseTexture2DArray : public FRHITexture2DArray, public FD3D12FastClearResource
{
public:
	FD3D12BaseTexture2DArray(uint32 InSizeX, uint32 InSizeY, uint32 InSizeZ, uint32 InNumMips, uint32 InNumSamples, EPixelFormat InFormat, ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue)
		: FRHITexture2DArray(InSizeX, InSizeY, InSizeZ, InNumMips, InNumSamples, InFormat, InFlags, InClearValue)
	{
		check(InNumSamples == 1);
	}
};

class FD3D12BaseTextureCube : public FRHITextureCube, public FD3D12FastClearResource
{
public:
	FD3D12BaseTextureCube(uint32 InSizeX, uint32 InSizeY, uint32 InSizeZ, uint32 InNumMips, uint32 InNumSamples, EPixelFormat InFormat, ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue)
		: FRHITextureCube(InSizeX, InNumMips, InFormat, InFlags, InClearValue)
		, SliceCount(InSizeZ)
	{
		check(InNumSamples == 1);
	}
	uint32 GetSizeX() const { return GetSize(); }
	uint32 GetSizeY() const { return GetSize(); }
	uint32 GetSizeZ() const { return SliceCount; }

private:
	uint32 SliceCount;
};

typedef TD3D12Texture2D<FD3D12BaseTexture2D>      FD3D12Texture2D;
typedef TD3D12Texture2D<FD3D12BaseTexture2DArray> FD3D12Texture2DArray;
typedef TD3D12Texture2D<FD3D12BaseTextureCube>    FD3D12TextureCube;

class FD3D12Viewport;

class FD3D12BackBufferReferenceTexture2D : public FD3D12Texture2D
{
public:

	FD3D12BackBufferReferenceTexture2D(
		FD3D12Viewport* InViewPort,
		bool bInIsSDR,
		FD3D12Device* InDevice,
		uint32 InSizeX,
		uint32 InSizeY,
		EPixelFormat InFormat) :
		FD3D12Texture2D(InDevice, InSizeX, InSizeY, 1, 1, 1, InFormat, false, TexCreate_RenderTargetable | TexCreate_Presentable, FClearValueBinding()),
		Viewport(InViewPort), bIsSDR(bInIsSDR)
	{
	}

	FD3D12Viewport* GetViewPort() { return Viewport; }
	bool IsSDR() const { return bIsSDR; }

	D3D12RHI_API FRHITexture* GetBackBufferTexture();

private:

	FD3D12Viewport* Viewport = nullptr;
	bool bIsSDR = false;
};

/** Given a pointer to a RHI texture that was created by the D3D12 RHI, returns a pointer to the FD3D12TextureBase it encapsulates. */
FORCEINLINE FD3D12TextureBase* GetD3D12TextureFromRHITexture(FRHITexture* Texture)
{
	if (!Texture)
	{
		return NULL;
	}
	
	// If it's the dummy backbuffer then swap with actual current RHI backbuffer right now
	FRHITexture* RHITexture = Texture;
	if (RHITexture && EnumHasAnyFlags(RHITexture->GetFlags(), TexCreate_Presentable))
	{
		FD3D12BackBufferReferenceTexture2D* BufferBufferReferenceTexture = (FD3D12BackBufferReferenceTexture2D*)RHITexture;
		RHITexture = BufferBufferReferenceTexture->GetBackBufferTexture();
	}

	FD3D12TextureBase* Result((FD3D12TextureBase*)RHITexture->GetTextureBaseRHI());
	check(Result);
	return Result;
}

FORCEINLINE FD3D12TextureBase* GetD3D12TextureFromRHITexture(FRHITexture* Texture, uint32 GPUIndex)
{
	FD3D12TextureBase* Result = GetD3D12TextureFromRHITexture(Texture);
	if (Result != nullptr)
	{
		Result = Result->GetLinkedObject(GPUIndex);
		check(Result);
		return Result;
	}
	else
	{
		return Result;
	}
}

class FD3D12TextureStats
{
public:

	static bool ShouldCountAsTextureMemory(D3D12_RESOURCE_FLAGS MiscFlags);

	// @param b3D true:3D, false:2D or cube map
	static TStatId GetRHIStatEnum(D3D12_RESOURCE_FLAGS MiscFlags, bool bCubeMap, bool b3D);
	static TStatId GetD3D12StatEnum(D3D12_RESOURCE_FLAGS MiscFlags);

	// Note: This function can be called from many different threads
	// @param TextureSize >0 to allocate, <0 to deallocate
	// @param b3D true:3D, false:2D or cube map
	// @param bStreamable true:Streamable, false:not streamable
	template<typename TD3D12Texture>
	static void UpdateD3D12TextureStats(TD3D12Texture& Texture, const D3D12_RESOURCE_DESC& Desc, int64 TextureSize, bool b3D, bool bCubeMap, bool bStreamable, bool bNewTexture = false);

	template<typename BaseResourceType>
	static void D3D12TextureAllocated(TD3D12Texture2D<BaseResourceType>& Texture, const D3D12_RESOURCE_DESC *Desc = nullptr);

	template<typename BaseResourceType>
	static void D3D12TextureDeleted(TD3D12Texture2D<BaseResourceType>& Texture);

	static void D3D12TextureAllocated2D(FD3D12Texture2D& Texture);

	static void D3D12TextureAllocated(FD3D12Texture3D& Texture);

	static void D3D12TextureDeleted(FD3D12Texture3D& Texture);
};

template<>
struct TD3D12ResourceTraits<FRHITexture3D>
{
	typedef FD3D12Texture3D TConcreteType;
};

template<>
struct TD3D12ResourceTraits<FRHITexture2D>
{
	typedef FD3D12Texture2D TConcreteType;
};

template<>
struct TD3D12ResourceTraits<FRHITexture2DArray>
{
	typedef FD3D12Texture2DArray TConcreteType;
};

template<>
struct TD3D12ResourceTraits<FRHITextureCube>
{
	typedef FD3D12TextureCube TConcreteType;
};

struct FRHICommandD3D12AsyncReallocateTexture2D final : public FRHICommand<FRHICommandD3D12AsyncReallocateTexture2D>
{
	FD3D12Texture2D* OldTexture;
	FD3D12Texture2D* NewTexture;
	int32 NewMipCount;
	int32 NewSizeX;
	int32 NewSizeY;
	FThreadSafeCounter* RequestStatus;

	FORCEINLINE_DEBUGGABLE FRHICommandD3D12AsyncReallocateTexture2D(FD3D12Texture2D* InOldTexture, FD3D12Texture2D* InNewTexture, int32 InNewMipCount, int32 InNewSizeX, int32 InNewSizeY, FThreadSafeCounter* InRequestStatus)
		: OldTexture(InOldTexture)
		, NewTexture(InNewTexture)
		, NewMipCount(InNewMipCount)
		, NewSizeX(InNewSizeX)
		, NewSizeY(InNewSizeY)
		, RequestStatus(InRequestStatus)
	{
	}

	void Execute(FRHICommandListBase& RHICmdList);
};