// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include <atomic>

#include "CoreTypes.h"
#include "Misc/AssertionMacros.h"
#include "HAL/UnrealMemory.h"
#include "Containers/Array.h"
#include "Misc/Crc.h"
#include "Containers/UnrealString.h"
#include "UObject/NameTypes.h"
#include "Math/Color.h"
#include "Containers/StaticArray.h"
#include "HAL/ThreadSafeCounter.h"
#include "RHIDefinitions.h"
#include "Templates/RefCounting.h"
#include "PixelFormat.h"
#include "TextureProfiler.h"
#include "Containers/LockFreeList.h"
#include "Misc/SecureHash.h"
#include "Hash/CityHash.h"
#include "Async/TaskGraphInterfaces.h"
#include "Serialization/MemoryImage.h"
#include "Experimental/Containers/HazardPointer.h"
#include "Containers/ClosableMpscQueue.h"
#include "Misc/CoreDelegates.h"

// RHI_WANT_RESOURCE_INFO should be controlled by the RHI module.
#ifndef RHI_WANT_RESOURCE_INFO
#define RHI_WANT_RESOURCE_INFO 0
#endif

// RHI_FORCE_DISABLE_RESOURCE_INFO can be defined anywhere else, like in GlobalDefinitions.
#ifndef RHI_FORCE_DISABLE_RESOURCE_INFO
#define RHI_FORCE_DISABLE_RESOURCE_INFO 0
#endif

#define RHI_ENABLE_RESOURCE_INFO (RHI_WANT_RESOURCE_INFO && !RHI_FORCE_DISABLE_RESOURCE_INFO)

#ifndef RHI_RESOURCE_LIFETIME_VALIDATION
#define RHI_RESOURCE_LIFETIME_VALIDATION 0
#endif


class FRHICommandListImmediate;
struct FClearValueBinding;
struct FRHIResourceInfo;
struct FGenerateMipsStruct;
enum class EClearBinding;

/** The base type of RHI resources. */
class RHI_API FRHIResource
{
public:
	UE_DEPRECATED(5.0, "FRHIResource(bool) is deprecated, please use FRHIResource(ERHIResourceType)")
	FRHIResource(bool InbDoNotDeferDelete=false)
		: ResourceType(RRT_None)
	{
	}

	FRHIResource(ERHIResourceType InResourceType)
		: ResourceType(InResourceType)
	{
#if RHI_ENABLE_RESOURCE_INFO
		BeginTrackingResource(this);
#endif
	}

	virtual ~FRHIResource()
	{
		check(IsEngineExitRequested() || CurrentlyDeleting == this);
		check(AtomicFlags.GetNumRefs(std::memory_order_relaxed) == 0); // this should not have any outstanding refs
		CurrentlyDeleting = nullptr;

#if RHI_ENABLE_RESOURCE_INFO
		EndTrackingResource(this);
#endif
	}

	FORCEINLINE_DEBUGGABLE uint32 AddRef() const
	{
		int32 NewValue = AtomicFlags.AddRef(std::memory_order_acquire);
		checkSlow(NewValue > 0); 
		return uint32(NewValue);
	}

private:
	// Separate function to avoid force inlining this everywhere. Helps both for code size and performance.
	inline void Destroy() const
	{
		if (!AtomicFlags.MarkForDelete(std::memory_order_release))
		{
			while (true)
			{
				auto HP = MakeHazardPointer(PendingDeletes, PendingDeletesHPC);
				TClosableMpscQueue<FRHIResource*>* PendingDeletesPtr = HP.Get();
				if (PendingDeletesPtr->Enqueue(const_cast<FRHIResource*>(this)))
				{
					break;
				}
			}
		}
	}

public:
	FORCEINLINE_DEBUGGABLE uint32 Release() const
	{
		int32 NewValue = AtomicFlags.Release(std::memory_order_release);
		check(NewValue >= 0);

		if (NewValue == 0)
		{
			Destroy();
		}
		checkSlow(NewValue >= 0);
		return uint32(NewValue);
	}

	FORCEINLINE_DEBUGGABLE uint32 GetRefCount() const
	{
		int32 CurrentValue = AtomicFlags.GetNumRefs(std::memory_order_relaxed);
		checkSlow(CurrentValue >= 0); 
		return uint32(CurrentValue);
	}

	static int32 FlushPendingDeletes(FRHICommandListImmediate& RHICmdList);

	static bool Bypass();

	bool IsValid() const
	{
		return AtomicFlags.IsValid(std::memory_order_relaxed);
	}

	void Delete()
	{
		verify(!AtomicFlags.MarkForDelete(std::memory_order_acquire));
		CurrentlyDeleting = this;
		delete this;
	}

	inline ERHIResourceType GetType() const { return ResourceType; }

#if RHI_ENABLE_RESOURCE_INFO
	// Get resource info if available.
	// Should return true if the ResourceInfo was filled with data.
	virtual bool GetResourceInfo(FRHIResourceInfo& OutResourceInfo) const
	{
		OutResourceInfo = FRHIResourceInfo{};
		return false;
	}

	static void BeginTrackingResource(FRHIResource* InResource);
	static void EndTrackingResource(FRHIResource* InResource);
	static void StartTrackingAllResources();
	static void StopTrackingAllResources();
#endif

private:
	class FAtomicFlags
	{
#if RHI_RESOURCE_LIFETIME_VALIDATION
		struct FPacked
		{
			FPacked(uint64 InNumRefs, bool InMarkedForDelete, bool InDeleting) : NumRefs(InNumRefs), MarkedForDelete(InMarkedForDelete), Deleting(InDeleting)
			{
			}

			FPacked() : NumRefs(0), MarkedForDelete(0), Deleting(0)
			{
			}

			uint32 NumRefs			: 30;
			uint32 MarkedForDelete	: 1;
			uint32 Deleting			: 1;
		};
		std::atomic<FPacked> Packed = {};
#else
		std::atomic_int NumRefs = { 0 };
		std::atomic_bool MarkedForDelete = { 0 };
#endif

	public:
		int32 AddRef(std::memory_order MemoryOrder)
		{
#if RHI_RESOURCE_LIFETIME_VALIDATION
			FPacked Old = Packed.load(std::memory_order_relaxed);
			while(true)
			{
				check(Old.Deleting == false);
				if(Packed.compare_exchange_weak(Old, FPacked(Old.NumRefs + 1, Old.MarkedForDelete, false), MemoryOrder, std::memory_order_relaxed))
				{
					return Old.NumRefs + 1;
				}
			}
#else
			return NumRefs.fetch_add(1, MemoryOrder) + 1;
#endif
		}

		int32 Release(std::memory_order MemoryOrder)
		{
#if RHI_RESOURCE_LIFETIME_VALIDATION
			FPacked Old = Packed.load(std::memory_order_relaxed);
			while(true)
			{
				check(Old.Deleting == false);
				if(Packed.compare_exchange_weak(Old, FPacked(Old.NumRefs - 1, Old.MarkedForDelete, false), MemoryOrder, std::memory_order_relaxed))
				{
					return Old.NumRefs - 1;
				}
			}
#else
			return NumRefs.fetch_sub(1, MemoryOrder) - 1;
#endif
		}

		bool MarkForDelete(std::memory_order MemoryOrder)
		{
#if RHI_RESOURCE_LIFETIME_VALIDATION
			FPacked Old = Packed.load(std::memory_order_relaxed);
			while(true)
			{
				check(Old.Deleting == false);
				if(Packed.compare_exchange_weak(Old, FPacked(Old.NumRefs, true, false), MemoryOrder, std::memory_order_relaxed))
				{
					return Old.MarkedForDelete;
				}
			}
#else
			return MarkedForDelete.exchange(true, MemoryOrder);
#endif
		}

		bool UnmarkForDelete(std::memory_order MemoryOrder)
		{
#if RHI_RESOURCE_LIFETIME_VALIDATION
			FPacked Old = Packed.load(std::memory_order_relaxed);
			while(true)
			{
				check(Old.Deleting == false); 
				check(Old.MarkedForDelete == true);
				if(Packed.compare_exchange_weak(Old, FPacked(Old.NumRefs, false, false), MemoryOrder, std::memory_order_relaxed))
				{
					return Old.MarkedForDelete;
				}
			}
#else
			bool Old = MarkedForDelete.exchange(false, MemoryOrder);
			check(Old == true);
			return Old;
#endif
		}

		bool Deleteing()
		{
#if RHI_RESOURCE_LIFETIME_VALIDATION
			FPacked Old = Packed.load(std::memory_order_relaxed);
			while(true)
			{
				check(Old.Deleting == false); 
				check(Old.MarkedForDelete == true);
				if(Old.NumRefs == 0)
				{
					if(Packed.compare_exchange_weak(Old, FPacked(0, true, true), std::memory_order_acquire, std::memory_order_relaxed))
					{
						return true;
					}
				}
				else
				{
					if(Packed.compare_exchange_weak(Old, FPacked(Old.NumRefs, false, false), std::memory_order_release, std::memory_order_relaxed))
					{
						return false;
					}
				}
			}
#else
			check(MarkedForDelete.load(std::memory_order_relaxed) == 1);
			if (NumRefs.load(std::memory_order_acquire) == 0) // caches can bring dead objects back to life
			{
				return true;
			}
			else
			{
				verify(MarkedForDelete.exchange(false, std::memory_order_release));	
				return false;
			}
#endif
		}

		bool IsValid(std::memory_order MemoryOrder)
		{
#if RHI_RESOURCE_LIFETIME_VALIDATION
			FPacked Old = Packed.load(MemoryOrder);
			return !Old.MarkedForDelete && Old.NumRefs > 0;
#else
			return !MarkedForDelete.load(MemoryOrder) && NumRefs.load(MemoryOrder) > 0;
#endif
		}

		int32 GetNumRefs(std::memory_order MemoryOrder)
		{
#if RHI_RESOURCE_LIFETIME_VALIDATION
			FPacked Old = Packed.load(MemoryOrder);
			return Old.NumRefs;
#else
			return NumRefs.load(MemoryOrder);
#endif
		}
	};
	mutable FAtomicFlags AtomicFlags;

	const ERHIResourceType ResourceType;
	bool bCommitted { true };
#if RHI_ENABLE_RESOURCE_INFO
	bool bBeingTracked { false };
#endif

	static std::atomic<TClosableMpscQueue<FRHIResource*>*> PendingDeletes;
	static FHazardPointerCollection PendingDeletesHPC;
	static FRHIResource* CurrentlyDeleting;

	// Some APIs don't do internal reference counting, so we have to wait an extra couple of frames before deleting resources
	// to ensure the GPU has completely finished with them. This avoids expensive fences, etc.
	struct ResourcesToDelete
	{
		TArray<FRHIResource*>	Resources;
		uint32					FrameDeleted{};
	};
};

class FExclusiveDepthStencil
{
public:
	enum Type
	{
		// don't use those directly, use the combined versions below
		// 4 bits are used for depth and 4 for stencil to make the hex value readable and non overlapping
		DepthNop = 0x00,
		DepthRead = 0x01,
		DepthWrite = 0x02,
		DepthMask = 0x0f,
		StencilNop = 0x00,
		StencilRead = 0x10,
		StencilWrite = 0x20,
		StencilMask = 0xf0,

		// use those:
		DepthNop_StencilNop = DepthNop + StencilNop,
		DepthRead_StencilNop = DepthRead + StencilNop,
		DepthWrite_StencilNop = DepthWrite + StencilNop,
		DepthNop_StencilRead = DepthNop + StencilRead,
		DepthRead_StencilRead = DepthRead + StencilRead,
		DepthWrite_StencilRead = DepthWrite + StencilRead,
		DepthNop_StencilWrite = DepthNop + StencilWrite,
		DepthRead_StencilWrite = DepthRead + StencilWrite,
		DepthWrite_StencilWrite = DepthWrite + StencilWrite,
	};

private:
	Type Value;

public:
	// constructor
	FExclusiveDepthStencil(Type InValue = DepthNop_StencilNop)
		: Value(InValue)
	{
	}

	inline bool IsUsingDepthStencil() const
	{
		return Value != DepthNop_StencilNop;
	}
	inline bool IsUsingDepth() const
	{
		return (ExtractDepth() != DepthNop);
	}
	inline bool IsUsingStencil() const
	{
		return (ExtractStencil() != StencilNop);
	}
	inline bool IsDepthWrite() const
	{
		return ExtractDepth() == DepthWrite;
	}
	inline bool IsDepthRead() const
	{
		return ExtractDepth() == DepthRead;
	}
	inline bool IsStencilWrite() const
	{
		return ExtractStencil() == StencilWrite;
	}
	inline bool IsStencilRead() const
	{
		return ExtractStencil() == StencilRead;
	}

	inline bool IsAnyWrite() const
	{
		return IsDepthWrite() || IsStencilWrite();
	}

	inline void SetDepthWrite()
	{
		Value = (Type)(ExtractStencil() | DepthWrite);
	}
	inline void SetStencilWrite()
	{
		Value = (Type)(ExtractDepth() | StencilWrite);
	}
	inline void SetDepthStencilWrite(bool bDepth, bool bStencil)
	{
		Value = DepthNop_StencilNop;

		if (bDepth)
		{
			SetDepthWrite();
		}
		if (bStencil)
		{
			SetStencilWrite();
		}
	}
	bool operator==(const FExclusiveDepthStencil& rhs) const
	{
		return Value == rhs.Value;
	}

	bool operator != (const FExclusiveDepthStencil& RHS) const
	{
		return Value != RHS.Value;
	}

	inline bool IsValid(FExclusiveDepthStencil& Current) const
	{
		Type Depth = ExtractDepth();

		if (Depth != DepthNop && Depth != Current.ExtractDepth())
		{
			return false;
		}

		Type Stencil = ExtractStencil();

		if (Stencil != StencilNop && Stencil != Current.ExtractStencil())
		{
			return false;
		}

		return true;
	}

	inline void GetAccess(ERHIAccess& DepthAccess, ERHIAccess& StencilAccess) const
	{
		DepthAccess = ERHIAccess::None;

		// SRV access is allowed whilst a depth stencil target is "readable".
		constexpr ERHIAccess DSVReadOnlyMask =
			ERHIAccess::DSVRead;

		// If write access is required, only the depth block can access the resource.
		constexpr ERHIAccess DSVReadWriteMask =
			ERHIAccess::DSVRead |
			ERHIAccess::DSVWrite;

		if (IsUsingDepth())
		{
			DepthAccess = IsDepthWrite() ? DSVReadWriteMask : DSVReadOnlyMask;
		}

		StencilAccess = ERHIAccess::None;

		if (IsUsingStencil())
		{
			StencilAccess = IsStencilWrite() ? DSVReadWriteMask : DSVReadOnlyMask;
		}
	}

	template <typename TFunction>
	inline void EnumerateSubresources(TFunction Function) const
	{
		if (!IsUsingDepthStencil())
		{
			return;
		}

		ERHIAccess DepthAccess = ERHIAccess::None;
		ERHIAccess StencilAccess = ERHIAccess::None;
		GetAccess(DepthAccess, StencilAccess);

		// Same depth / stencil state; single subresource.
		if (DepthAccess == StencilAccess)
		{
			Function(DepthAccess, FRHITransitionInfo::kAllSubresources);
		}
		// Separate subresources for depth / stencil.
		else
		{
			if (DepthAccess != ERHIAccess::None)
			{
				Function(DepthAccess, FRHITransitionInfo::kDepthPlaneSlice);
			}
			if (StencilAccess != ERHIAccess::None)
			{
				Function(StencilAccess, FRHITransitionInfo::kStencilPlaneSlice);
			}
		}
	}

	/**
	* Returns a new FExclusiveDepthStencil to be used to transition a depth stencil resource to readable.
	* If the depth or stencil is already in a readable state, that particular component is returned as Nop,
	* to avoid unnecessary subresource transitions.
	*/
	inline FExclusiveDepthStencil GetReadableTransition() const
	{
		FExclusiveDepthStencil::Type NewDepthState = IsDepthWrite()
			? FExclusiveDepthStencil::DepthRead
			: FExclusiveDepthStencil::DepthNop;

		FExclusiveDepthStencil::Type NewStencilState = IsStencilWrite()
			? FExclusiveDepthStencil::StencilRead
			: FExclusiveDepthStencil::StencilNop;

		return (FExclusiveDepthStencil::Type)(NewDepthState | NewStencilState);
	}

	/**
	* Returns a new FExclusiveDepthStencil to be used to transition a depth stencil resource to readable.
	* If the depth or stencil is already in a readable state, that particular component is returned as Nop,
	* to avoid unnecessary subresource transitions.
	*/
	inline FExclusiveDepthStencil GetWritableTransition() const
	{
		FExclusiveDepthStencil::Type NewDepthState = IsDepthRead()
			? FExclusiveDepthStencil::DepthWrite
			: FExclusiveDepthStencil::DepthNop;

		FExclusiveDepthStencil::Type NewStencilState = IsStencilRead()
			? FExclusiveDepthStencil::StencilWrite
			: FExclusiveDepthStencil::StencilNop;

		return (FExclusiveDepthStencil::Type)(NewDepthState | NewStencilState);
	}

	uint32 GetIndex() const
	{
		// Note: The array to index has views created in that specific order.

		// we don't care about the Nop versions so less views are needed
		// we combine Nop and Write
		switch (Value)
		{
		case DepthWrite_StencilNop:
		case DepthNop_StencilWrite:
		case DepthWrite_StencilWrite:
		case DepthNop_StencilNop:
			return 0; // old DSAT_Writable

		case DepthRead_StencilNop:
		case DepthRead_StencilWrite:
			return 1; // old DSAT_ReadOnlyDepth

		case DepthNop_StencilRead:
		case DepthWrite_StencilRead:
			return 2; // old DSAT_ReadOnlyStencil

		case DepthRead_StencilRead:
			return 3; // old DSAT_ReadOnlyDepthAndStencil
		}
		// should never happen
		check(0);
		return -1;
	}
	static const uint32 MaxIndex = 4;

private:
	inline Type ExtractDepth() const
	{
		return (Type)(Value & DepthMask);
	}
	inline Type ExtractStencil() const
	{
		return (Type)(Value & StencilMask);
	}
};

//
// State blocks
//

class FRHISamplerState : public FRHIResource 
{
public:
	FRHISamplerState() : FRHIResource(RRT_SamplerState) {}
	virtual bool IsImmutable() const { return false; }
};

class FRHIRasterizerState : public FRHIResource
{
public:
	FRHIRasterizerState() : FRHIResource(RRT_RasterizerState) {}
	virtual bool GetInitializer(struct FRasterizerStateInitializerRHI& Init) { return false; }
};

class FRHIDepthStencilState : public FRHIResource
{
public:
	FRHIDepthStencilState() : FRHIResource(RRT_DepthStencilState) {}
#if ENABLE_RHI_VALIDATION
	FExclusiveDepthStencil ActualDSMode;
#endif
	virtual bool GetInitializer(struct FDepthStencilStateInitializerRHI& Init) { return false; }
};

class FRHIBlendState : public FRHIResource
{
public:
	FRHIBlendState() : FRHIResource(RRT_BlendState) {}
	virtual bool GetInitializer(class FBlendStateInitializerRHI& Init) { return false; }
};

//
// Shader bindings
//

typedef TArray<struct FVertexElement,TFixedAllocator<MaxVertexElementCount> > FVertexDeclarationElementList;
class FRHIVertexDeclaration : public FRHIResource
{
public:
	FRHIVertexDeclaration() : FRHIResource(RRT_VertexDeclaration) {}
	virtual bool GetInitializer(FVertexDeclarationElementList& Init) { return false; }
};

class FRHIBoundShaderState : public FRHIResource
{
public:
	FRHIBoundShaderState() : FRHIResource(RRT_BoundShaderState) {}
};

//
// Shaders
//

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	#define RHI_INCLUDE_SHADER_DEBUG_DATA 1
#else
	#define RHI_INCLUDE_SHADER_DEBUG_DATA 0
#endif

class FRHIShader : public FRHIResource
{
public:
	void SetHash(FSHAHash InHash) { Hash = InHash; }
	FSHAHash GetHash() const { return Hash; }

#if RHI_INCLUDE_SHADER_DEBUG_DATA
	// for debugging only e.g. MaterialName:ShaderFile.usf or ShaderFile.usf/EntryFunc
	FString ShaderName;
	FORCEINLINE const TCHAR* GetShaderName() const { return *ShaderName; }
#else
	FORCEINLINE const TCHAR* GetShaderName() const { return TEXT(""); }
#endif

	FRHIShader() = delete;
	FRHIShader(ERHIResourceType InResourceType, EShaderFrequency InFrequency)
		: FRHIResource(InResourceType)
		, Frequency(InFrequency)
	{
	}

	inline EShaderFrequency GetFrequency() const
	{
		return Frequency;
	}

private:
	FSHAHash Hash;
	EShaderFrequency Frequency;
};

class FRHIGraphicsShader : public FRHIShader
{
public:
	explicit FRHIGraphicsShader(ERHIResourceType InResourceType, EShaderFrequency InFrequency)
		: FRHIShader(InResourceType, InFrequency) {}
};

class FRHIVertexShader : public FRHIGraphicsShader
{
public:
	FRHIVertexShader() : FRHIGraphicsShader(RRT_VertexShader, SF_Vertex) {}
};

class FRHIMeshShader : public FRHIGraphicsShader
{
public:
	FRHIMeshShader() : FRHIGraphicsShader(RRT_MeshShader, SF_Mesh) {}
};

class FRHIAmplificationShader : public FRHIGraphicsShader
{
public:
	FRHIAmplificationShader() : FRHIGraphicsShader(RRT_AmplificationShader, SF_Amplification) {}
};

class FRHIPixelShader : public FRHIGraphicsShader
{
public:
	FRHIPixelShader() : FRHIGraphicsShader(RRT_PixelShader, SF_Pixel) {}
};

class FRHIGeometryShader : public FRHIGraphicsShader
{
public:
	FRHIGeometryShader() : FRHIGraphicsShader(RRT_GeometryShader, SF_Geometry) {}
};

class FRHIRayTracingShader : public FRHIShader
{
public:
	explicit FRHIRayTracingShader(EShaderFrequency InFrequency) : FRHIShader(RRT_RayTracingShader, InFrequency) {}
};

class FRHIRayGenShader : public FRHIRayTracingShader
{
public:
	FRHIRayGenShader() : FRHIRayTracingShader(SF_RayGen) {}
};

class FRHIRayMissShader : public FRHIRayTracingShader
{
public:
	FRHIRayMissShader() : FRHIRayTracingShader(SF_RayMiss) {}
};

class FRHIRayCallableShader : public FRHIRayTracingShader
{
public:
	FRHIRayCallableShader() : FRHIRayTracingShader(SF_RayCallable) {}
};

class FRHIRayHitGroupShader : public FRHIRayTracingShader
{
public:
	FRHIRayHitGroupShader() : FRHIRayTracingShader(SF_RayHitGroup) {}
};

class RHI_API FRHIComputeShader : public FRHIShader
{
public:
	FRHIComputeShader() : FRHIShader(RRT_ComputeShader, SF_Compute), Stats(nullptr) {}
	
	inline void SetStats(struct FPipelineStateStats* Ptr) { Stats = Ptr; }
	void UpdateStats();
	
private:
	struct FPipelineStateStats* Stats;
};

//
// Pipeline States
//

class FRHIGraphicsPipelineState : public FRHIResource 
{
public:
	FRHIGraphicsPipelineState() : FRHIResource(RRT_GraphicsPipelineState) {}

	inline void SetSortKey(uint64 InSortKey) { SortKey = InSortKey; }
	inline uint64 GetSortKey() const { return SortKey; }

private:
	uint64 SortKey = 0;

#if ENABLE_RHI_VALIDATION
	friend class FValidationContext;
	friend class FValidationRHI;
	FExclusiveDepthStencil DSMode;
#endif
};
class FRHIComputePipelineState : public FRHIResource
{
public:
	FRHIComputePipelineState() : FRHIResource(RRT_ComputePipelineState) {}
};
class FRHIRayTracingPipelineState : public FRHIResource
{
public:
	FRHIRayTracingPipelineState() : FRHIResource(RRT_RayTracingPipelineState) {}
};

//
// Buffers
//

// Whether to assert in cases where the layout is released before uniform buffers created with that layout
#define VALIDATE_UNIFORM_BUFFER_LAYOUT_LIFETIME 0

// Whether to assert when a uniform buffer is being deleted while still referenced by a mesh draw command
// Enabling this requires -norhithread to work correctly since FRHIResource lifetime is managed by both the RT and RHIThread
#define VALIDATE_UNIFORM_BUFFER_LIFETIME 0

/** Data structure to store information about resource parameter in a shader parameter structure. */
struct FRHIUniformBufferResource
{
	DECLARE_EXPORTED_TYPE_LAYOUT(FRHIUniformBufferResource, RHI_API, NonVirtual);

	/** Byte offset to each resource in the uniform buffer memory. */
	LAYOUT_FIELD(uint16, MemberOffset);

	/** Type of the member that allow (). */
	LAYOUT_FIELD(EUniformBufferBaseType, MemberType);
};

inline FArchive& operator<<(FArchive& Ar, FRHIUniformBufferResource& Ref)
{
	uint8 Type = (uint8)Ref.MemberType;
	Ar << Ref.MemberOffset;
	Ar << Type;
	Ref.MemberType = (EUniformBufferBaseType)Type;
	return Ar;
}

/** Compare two uniform buffer layout resources. */
inline bool operator==(const FRHIUniformBufferResource& A, const FRHIUniformBufferResource& B)
{
	return A.MemberOffset == B.MemberOffset
		&& A.MemberType == B.MemberType;
}

static constexpr uint16 kUniformBufferInvalidOffset = TNumericLimits<uint16>::Max();

/** Initializer for the layout of a uniform buffer in memory. */
struct FRHIUniformBufferLayoutInitializer
{
	DECLARE_EXPORTED_TYPE_LAYOUT(FRHIUniformBufferLayoutInitializer, RHI_API, NonVirtual);

	FRHIUniformBufferLayoutInitializer() = default;

	explicit FRHIUniformBufferLayoutInitializer(const TCHAR * InName)
		: Name(InName)
	{}
	explicit FRHIUniformBufferLayoutInitializer(const TCHAR * InName, uint32 InConstantBufferSize)
		: Name(InName)
		, ConstantBufferSize(InConstantBufferSize)
	{
		ComputeHash();
	}

	inline uint32 GetHash() const
	{
		checkSlow(Hash != 0);
		return Hash;
	}

	void ComputeHash()
	{
		// Static slot is not stable. Just track whether we have one at all.
		uint32 TmpHash = ConstantBufferSize << 16 | static_cast<uint32>(BindingFlags) << 8 | static_cast<uint32>(StaticSlot != MAX_UNIFORM_BUFFER_STATIC_SLOTS);

		for (int32 ResourceIndex = 0; ResourceIndex < Resources.Num(); ResourceIndex++)
		{
			// Offset and therefore hash must be the same regardless of pointer size
			checkSlow(Resources[ResourceIndex].MemberOffset == Align(Resources[ResourceIndex].MemberOffset, SHADER_PARAMETER_POINTER_ALIGNMENT));
			TmpHash ^= Resources[ResourceIndex].MemberOffset;
		}

		uint32 N = Resources.Num();
		while (N >= 4)
		{
			TmpHash ^= (Resources[--N].MemberType << 0);
			TmpHash ^= (Resources[--N].MemberType << 8);
			TmpHash ^= (Resources[--N].MemberType << 16);
			TmpHash ^= (Resources[--N].MemberType << 24);
		}
		while (N >= 2)
		{
			TmpHash ^= Resources[--N].MemberType << 0;
			TmpHash ^= Resources[--N].MemberType << 16;
		}
		while (N > 0)
		{
			TmpHash ^= Resources[--N].MemberType;
		}
		Hash = TmpHash;
	}

	void CopyFrom(const FRHIUniformBufferLayoutInitializer& Source)
	{
		ConstantBufferSize = Source.ConstantBufferSize;
		StaticSlot = Source.StaticSlot;
		BindingFlags = Source.BindingFlags;
		Resources = Source.Resources;
		Name = Source.Name;
		Hash = Source.Hash;
	}

	const FMemoryImageString& GetDebugName() const
	{
		return Name;
	}

	bool HasRenderTargets() const
	{
		return RenderTargetsOffset != kUniformBufferInvalidOffset;
	}

	bool HasExternalOutputs() const
	{
		return bHasNonGraphOutputs;
	}

	bool HasStaticSlot() const
	{
		return IsUniformBufferStaticSlotValid(StaticSlot);
	}

	friend FArchive& operator<<(FArchive& Ar, FRHIUniformBufferLayoutInitializer& Ref)
	{
		Ar << Ref.ConstantBufferSize;
		Ar << Ref.StaticSlot;
		Ar << Ref.RenderTargetsOffset;
		Ar << Ref.bHasNonGraphOutputs;
		Ar << Ref.BindingFlags;
		Ar << Ref.Resources;
		Ar << Ref.GraphResources;
		Ar << Ref.GraphTextures;
		Ar << Ref.GraphBuffers;
		Ar << Ref.GraphUniformBuffers;
		Ar << Ref.UniformBuffers;
		Ar << Ref.Name;
		Ar << Ref.Hash;
		return Ar;
	}

private:
	// for debugging / error message
	LAYOUT_FIELD(FMemoryImageString, Name);

public:
	/** The list of all resource inlined into the shader parameter structure. */
	LAYOUT_FIELD(TMemoryImageArray<FRHIUniformBufferResource>, Resources);

	/** The list of all RDG resource references inlined into the shader parameter structure. */
	LAYOUT_FIELD(TMemoryImageArray<FRHIUniformBufferResource>, GraphResources);

	/** The list of all RDG texture references inlined into the shader parameter structure. */
	LAYOUT_FIELD(TMemoryImageArray<FRHIUniformBufferResource>, GraphTextures);

	/** The list of all RDG buffer references inlined into the shader parameter structure. */
	LAYOUT_FIELD(TMemoryImageArray<FRHIUniformBufferResource>, GraphBuffers);

	/** The list of all RDG uniform buffer references inlined into the shader parameter structure. */
	LAYOUT_FIELD(TMemoryImageArray<FRHIUniformBufferResource>, GraphUniformBuffers);

	/** The list of all non-RDG uniform buffer references inlined into the shader parameter structure. */
	LAYOUT_FIELD(TMemoryImageArray<FRHIUniformBufferResource>, UniformBuffers);

private:
	LAYOUT_FIELD_INITIALIZED(uint32, Hash, 0);

public:
	/** The size of the constant buffer in bytes. */
	LAYOUT_FIELD_INITIALIZED(uint32, ConstantBufferSize, 0);

	/** The render target binding slots offset, if it exists. */
	LAYOUT_FIELD_INITIALIZED(uint16, RenderTargetsOffset, kUniformBufferInvalidOffset);

	/** The static slot (if applicable). */
	LAYOUT_FIELD_INITIALIZED(FUniformBufferStaticSlot, StaticSlot, MAX_UNIFORM_BUFFER_STATIC_SLOTS);

	/** The binding flags describing how this resource can be bound to the RHI. */
	LAYOUT_FIELD_INITIALIZED(EUniformBufferBindingFlags, BindingFlags, EUniformBufferBindingFlags::Shader);

	/** Whether this layout may contain non-render-graph outputs (e.g. RHI UAVs). */
	LAYOUT_FIELD_INITIALIZED(bool, bHasNonGraphOutputs, false);
};

/** Compare two uniform buffer layout initializers. */
inline bool operator==(const FRHIUniformBufferLayoutInitializer& A, const FRHIUniformBufferLayoutInitializer& B)
{
	return A.ConstantBufferSize == B.ConstantBufferSize
		&& A.StaticSlot == B.StaticSlot
		&& A.BindingFlags == B.BindingFlags
		&& A.Resources == B.Resources;
}

/** The layout of a uniform buffer in memory. */
struct FRHIUniformBufferLayout : public FRHIResource
{
	FRHIUniformBufferLayout() = delete;

	explicit FRHIUniformBufferLayout(const FRHIUniformBufferLayoutInitializer& Initializer)
		: FRHIResource(RRT_UniformBufferLayout)
		, Name(Initializer.GetDebugName())
		, Resources(Initializer.Resources)
		, GraphResources(Initializer.GraphResources)
		, GraphTextures(Initializer.GraphTextures)
		, GraphBuffers(Initializer.GraphBuffers)
		, GraphUniformBuffers(Initializer.GraphUniformBuffers)
		, UniformBuffers(Initializer.UniformBuffers)
		, Hash(Initializer.GetHash())
		, ConstantBufferSize(Initializer.ConstantBufferSize)
		, RenderTargetsOffset(Initializer.RenderTargetsOffset)
		, StaticSlot(Initializer.StaticSlot)
		, BindingFlags(Initializer.BindingFlags)
		, bHasNonGraphOutputs(Initializer.bHasNonGraphOutputs)
	{}

	inline const FString& GetDebugName() const
	{
		return Name;
	}

	inline uint32 GetHash() const
	{
		checkSlow(Hash != 0);
		return Hash;
	}

	inline bool HasRenderTargets() const
	{
		return RenderTargetsOffset != kUniformBufferInvalidOffset;
	}

	inline bool HasExternalOutputs() const
	{
		return bHasNonGraphOutputs;
	}

	inline bool HasStaticSlot() const
	{
		return IsUniformBufferStaticSlotValid(StaticSlot);
	}

	const FString Name;

	/** The list of all resource inlined into the shader parameter structure. */
	const TArray<FRHIUniformBufferResource> Resources;

	/** The list of all RDG resource references inlined into the shader parameter structure. */
	const TArray<FRHIUniformBufferResource> GraphResources;

	/** The list of all RDG texture references inlined into the shader parameter structure. */
	const TArray<FRHIUniformBufferResource> GraphTextures;

	/** The list of all RDG buffer references inlined into the shader parameter structure. */
	const TArray<FRHIUniformBufferResource> GraphBuffers;

	/** The list of all RDG uniform buffer references inlined into the shader parameter structure. */
	const TArray<FRHIUniformBufferResource> GraphUniformBuffers;

	/** The list of all non-RDG uniform buffer references inlined into the shader parameter structure. */
	const TArray<FRHIUniformBufferResource> UniformBuffers;

	const uint32 Hash;

	/** The size of the constant buffer in bytes. */
	const uint32 ConstantBufferSize;

	/** The render target binding slots offset, if it exists. */
	const uint16 RenderTargetsOffset;

	/** The static slot (if applicable). */
	const FUniformBufferStaticSlot StaticSlot;

	/** The binding flags describing how this resource can be bound to the RHI. */
	const EUniformBufferBindingFlags BindingFlags;

	/** Whether this layout may contain non-render-graph outputs (e.g. RHI UAVs). */
	const bool bHasNonGraphOutputs;
};

/** Compare two uniform buffer layouts. */
inline bool operator==(const FRHIUniformBufferLayout& A, const FRHIUniformBufferLayout& B)
{
	return A.ConstantBufferSize == B.ConstantBufferSize
		&& A.StaticSlot == B.StaticSlot
		&& A.BindingFlags == B.BindingFlags
		&& A.Resources == B.Resources;
}

class FRHIUniformBuffer : public FRHIResource
#if ENABLE_RHI_VALIDATION
	, public RHIValidation::FUniformBufferResource
#endif
{
public:
	FRHIUniformBuffer() = delete;

	/** Initialization constructor. */
	FRHIUniformBuffer(const FRHIUniformBufferLayout* InLayout)
	: FRHIResource(RRT_UniformBuffer)
	, Layout(InLayout)
	, LayoutConstantBufferSize(InLayout->ConstantBufferSize)
	{}

	FORCEINLINE_DEBUGGABLE uint32 Release() const
	{
		const FRHIUniformBufferLayout* LocalLayout = Layout;

#if VALIDATE_UNIFORM_BUFFER_LIFETIME
		int32 LocalNumMeshCommandReferencesForDebugging = NumMeshCommandReferencesForDebugging;
#endif

		uint32 NewRefCount = FRHIResource::Release();

		if (NewRefCount == 0)
		{
#if VALIDATE_UNIFORM_BUFFER_LIFETIME
			check(LocalNumMeshCommandReferencesForDebugging == 0 || IsEngineExitRequested());
#endif
		}

		return NewRefCount;
	}

	/** @return The number of bytes in the uniform buffer. */
	uint32 GetSize() const
	{
		check(LayoutConstantBufferSize == Layout->ConstantBufferSize);
		return LayoutConstantBufferSize;
	}
	const FRHIUniformBufferLayout& GetLayout() const { return *Layout; }
	const FRHIUniformBufferLayout* GetLayoutPtr() const { return Layout; }

#if VALIDATE_UNIFORM_BUFFER_LIFETIME
	mutable int32 NumMeshCommandReferencesForDebugging = 0;
#endif

private:
	/** Layout of the uniform buffer. */
	TRefCountPtr<const FRHIUniformBufferLayout> Layout;

	uint32 LayoutConstantBufferSize;
};

class FRHIBuffer : public FRHIResource
#if ENABLE_RHI_VALIDATION
	, public RHIValidation::FBufferResource
#endif
{
public:
	FRHIBuffer() : FRHIResource(RRT_Buffer) {}

	/** Initialization constructor. */
	FRHIBuffer(uint32 InSize, EBufferUsageFlags InUsage, uint32 InStride)
		: FRHIResource(RRT_Buffer)
		, Size(InSize)
		, Stride(InStride)
		, Usage(InUsage)
	{
	}

	/** @return The number of bytes in the buffer. */
	uint32 GetSize() const { return Size; }

	/** @return The stride in bytes of the buffer. */
	uint32 GetStride() const { return Stride; }

	/** @return The usage flags used to create the buffer. */
	EBufferUsageFlags GetUsage() const { return Usage; }

	void SetName(const FName& InName) { BufferName = InName; }

	FName GetName() const { return BufferName; }

	virtual uint32 GetParentGPUIndex() const { return 0; }

protected:
	void Swap(FRHIBuffer& Other)
	{
		::Swap(Stride, Other.Stride);
		::Swap(Size, Other.Size);
		::Swap(Usage, Other.Usage);
	}

	// Used by RHI implementations that may adjust internal usage flags during object construction.
	void SetUsage(EBufferUsageFlags InUsage)
	{
		Usage = InUsage;
	}

	void ReleaseUnderlyingResource()
	{
		Stride = Size = 0;
		Usage = EBufferUsageFlags::None;
	}

private:
	uint32 Size{};
	uint32 Stride{};
	EBufferUsageFlags Usage{};
	FName BufferName;
};

UE_DEPRECATED(5.0, "FRHIIndexBuffer is deprecated, please use FRHIBuffer.")      typedef class FRHIBuffer FRHIIndexBuffer;
UE_DEPRECATED(5.0, "FRHIVertexBuffer is deprecated, please use FRHIBuffer.")     typedef class FRHIBuffer FRHIVertexBuffer;
UE_DEPRECATED(5.0, "FRHIStructuredBuffer is deprecated, please use FRHIBuffer.") typedef class FRHIBuffer FRHIStructuredBuffer;

//
// Textures
//

class RHI_API FLastRenderTimeContainer
{
public:
	FLastRenderTimeContainer() : LastRenderTime(-FLT_MAX) {}

	double GetLastRenderTime() const { return LastRenderTime; }
	FORCEINLINE_DEBUGGABLE void SetLastRenderTime(double InLastRenderTime) 
	{ 
		// avoid dirty caches from redundant writes
		if (LastRenderTime != InLastRenderTime)
		{
			LastRenderTime = InLastRenderTime;
		}
	}

private:
	/** The last time the resource was rendered. */
	double LastRenderTime;
};

class RHI_API FRHITexture : public FRHIResource
#if ENABLE_RHI_VALIDATION
	, public RHIValidation::FTextureResource
#endif
{
public:
	
	/** Initialization constructor. */
	FRHITexture(ERHIResourceType InResourceType, uint32 InNumMips, uint32 InNumSamples, EPixelFormat InFormat, ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue)
		: FRHIResource(InResourceType)
		, ClearValue(InClearValue)
		, NumMips(InNumMips)
		, NumSamples(InNumSamples)
		, Format(InFormat)
		, Flags(InFlags)
	{}

	UE_DEPRECATED(5.0, "The InLastRenderTime parameter will be removed in the future")
	FRHITexture(ERHIResourceType InResourceType, uint32 InNumMips, uint32 InNumSamples, EPixelFormat InFormat, ETextureCreateFlags InFlags, FLastRenderTimeContainer* InLastRenderTime, const FClearValueBinding& InClearValue)
		: FRHITexture(InResourceType, InNumMips, InNumSamples, InFormat, InFlags, InClearValue)
	{}

	// Dynamic cast methods.
	virtual class FRHITexture2D* GetTexture2D() { return NULL; }
	virtual class FRHITexture2DArray* GetTexture2DArray() { return NULL; }
	virtual class FRHITexture3D* GetTexture3D() { return NULL; }
	virtual class FRHITextureCube* GetTextureCube() { return NULL; }
	virtual class FRHITextureReference* GetTextureReference() { return NULL; }
	
	// Slower method to get Size X, Y & Z information. Prefer sub-classes' GetSizeX(), etc
	virtual FIntVector GetSizeXYZ() const = 0;

	/**
	 * Returns access to the platform-specific native resource pointer.  This is designed to be used to provide plugins with access
	 * to the underlying resource and should be used very carefully or not at all.
	 *
	 * @return	The pointer to the native resource or NULL if it not initialized or not supported for this resource type for some reason
	 */
	virtual void* GetNativeResource() const
	{
		// Override this in derived classes to expose access to the native texture resource
		return nullptr;
	}

	/**
	 * Returns access to the platform-specific native shader resource view pointer.  This is designed to be used to provide plugins with access
	 * to the underlying resource and should be used very carefully or not at all.
	 *
	 * @return	The pointer to the native resource or NULL if it not initialized or not supported for this resource type for some reason
	 */
	virtual void* GetNativeShaderResourceView() const
	{
		// Override this in derived classes to expose access to the native texture resource
		return nullptr;
	}
	
	/**
	 * Returns access to the platform-specific RHI texture baseclass.  This is designed to provide the RHI with fast access to its base classes in the face of multiple inheritance.
	 * @return	The pointer to the platform-specific RHI texture baseclass or NULL if it not initialized or not supported for this RHI
	 */
	virtual void* GetTextureBaseRHI()
	{
		// Override this in derived classes to expose access to the native texture resource
		return nullptr;
	}

	/**
	 * Returns the dimensions (i.e. the actual number of texels in each dimension) of the specified mip. ArraySize is ignored.
	 * The Z component will always be 1 for 2D/cube resources and will contain depth for volume textures.
	 * This differs from GetSizeXYZ() which returns ArraySize in Z for 2D arrays.
	 */
	virtual FIntVector GetMipDimensions(uint8 MipIndex) const
	{
		FIntVector Size = GetSizeXYZ();
		return FIntVector(
			FMath::Max(Size.X >> MipIndex, 1),
			FMath::Max(Size.Y >> MipIndex, 1),
			FMath::Max(Size.Z >> MipIndex, 1)
		);
	}

	/** @return The number of mip-maps in the texture. */
	uint32 GetNumMips() const { return NumMips; }

	/** @return The format of the pixels in the texture. */
	EPixelFormat GetFormat() const { return Format; }

	/** @return The flags used to create the texture. */
	ETextureCreateFlags GetFlags() const { return Flags; }

	/* @return the number of samples for multi-sampling. */
	uint32 GetNumSamples() const { return NumSamples; }

	/** @return Whether the texture is multi sampled. */
	bool IsMultisampled() const { return NumSamples > 1; }		

	/** sets the last time this texture was cached in a resource table. */
	FORCEINLINE_DEBUGGABLE void SetLastRenderTime(float InLastRenderTime)
	{
		LastRenderTime.SetLastRenderTime(InLastRenderTime);
	}

	double GetLastRenderTime() const
	{
		return LastRenderTime.GetLastRenderTime();
	}

	/** Returns the last render time container, or NULL if none were specified at creation. */
	UE_DEPRECATED(5.0, "GetLastRenderTimeContainer is deprecated and will be removed in the future")
	FLastRenderTimeContainer* GetLastRenderTimeContainer()
	{
		return nullptr;
	}

	UE_DEPRECATED(5.0, "SetDefaultLastRenderTimeContainer is deprecated and will be removed in the future")
	FORCEINLINE_DEBUGGABLE void SetDefaultLastRenderTimeContainer()
	{
	}

	void SetName(const FName& InName)
	{
		TextureName = InName;

#if TEXTURE_PROFILER_ENABLED
		FTextureProfiler::Get()->UpdateTextureName(this);
#endif
	}

	FName GetName() const
	{
		return TextureName;
	}

	bool HasClearValue() const
	{
		return ClearValue.ColorBinding != EClearBinding::ENoneBound;
	}

	FLinearColor GetClearColor() const
	{
		return ClearValue.GetClearColor();
	}

	void GetDepthStencilClearValue(float& OutDepth, uint32& OutStencil) const
	{
		return ClearValue.GetDepthStencil(OutDepth, OutStencil);
	}

	float GetDepthClearValue() const
	{
		float Depth;
		uint32 Stencil;
		ClearValue.GetDepthStencil(Depth, Stencil);
		return Depth;
	}

	uint32 GetStencilClearValue() const
	{
		float Depth;
		uint32 Stencil;
		ClearValue.GetDepthStencil(Depth, Stencil);
		return Stencil;
	}

	const FClearValueBinding GetClearBinding() const
	{
		return ClearValue;
	}

	virtual void GetWriteMaskProperties(void*& OutData, uint32& OutSize)
	{
		OutData = nullptr;
		OutSize = 0;
	}

private:
	FClearValueBinding ClearValue;
	uint32 NumMips;
	uint32 NumSamples;
	EPixelFormat Format;
	ETextureCreateFlags Flags;
	FLastRenderTimeContainer LastRenderTime;
	FName TextureName;
};

class RHI_API FRHITexture2D : public FRHITexture
{
public:
	
	/** Initialization constructor. */
	FRHITexture2D(uint32 InSizeX,uint32 InSizeY,uint32 InNumMips,uint32 InNumSamples,EPixelFormat InFormat,ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue, ERHIResourceType InResourceTypeOverride = RRT_None)
	: FRHITexture(InResourceTypeOverride != RRT_None ? InResourceTypeOverride : RRT_Texture2D, InNumMips, InNumSamples, InFormat, InFlags, InClearValue)
	, SizeX(InSizeX)
	, SizeY(InSizeY)
	{}
	
	// Dynamic cast methods.
	virtual FRHITexture2D* GetTexture2D() { return this; }

	/** @return The width of the texture. */
	uint32 GetSizeX() const { return SizeX; }
	
	/** @return The height of the texture. */
	uint32 GetSizeY() const { return SizeY; }

	inline FIntPoint GetSizeXY() const
	{
		return FIntPoint(SizeX, SizeY);
	}

	virtual FIntVector GetSizeXYZ() const override
	{
		return FIntVector(SizeX, SizeY, 1);
	}

private:

	uint32 SizeX;
	uint32 SizeY;
};

class RHI_API FRHITexture2DArray : public FRHITexture2D
{
public:
	
	/** Initialization constructor. */
	FRHITexture2DArray(uint32 InSizeX,uint32 InSizeY,uint32 InSizeZ,uint32 InNumMips,uint32 NumSamples, EPixelFormat InFormat,ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue)
	: FRHITexture2D(InSizeX, InSizeY, InNumMips,NumSamples,InFormat,InFlags, InClearValue, RRT_Texture2DArray)
	, SizeZ(InSizeZ)
	{
		check(InSizeZ != 0);
	}
	
	// Dynamic cast methods.
	virtual FRHITexture2DArray* GetTexture2DArray() { return this; }

	virtual FRHITexture2D* GetTexture2D() { return NULL; }

	/** @return The number of textures in the array. */
	uint32 GetSizeZ() const { return SizeZ; }

	virtual FIntVector GetSizeXYZ() const final override
	{
		return FIntVector(GetSizeX(), GetSizeY(), SizeZ);
	}

	// Because GetSizeXYZ() returns ArraySize in Z, we need to override this function to return 1 instead.
	// @todo: Unify the meaning of "Z" in all texture resources
	virtual FIntVector GetMipDimensions(uint8 MipIndex) const final override
	{
		return FIntVector(
			FMath::Max(GetSizeX() >> MipIndex, 1u),
			FMath::Max(GetSizeY() >> MipIndex, 1u),
			1
		);
	}

private:

	uint32 SizeZ;
};

class RHI_API FRHITexture3D : public FRHITexture
{
public:
	
	/** Initialization constructor. */
	FRHITexture3D(uint32 InSizeX,uint32 InSizeY,uint32 InSizeZ,uint32 InNumMips,EPixelFormat InFormat,ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue)
	: FRHITexture(RRT_Texture3D, InNumMips,1,InFormat,InFlags, InClearValue)
	, SizeX(InSizeX)
	, SizeY(InSizeY)
	, SizeZ(InSizeZ)
	{}
	
	// Dynamic cast methods.
	virtual FRHITexture3D* GetTexture3D() { return this; }
	
	/** @return The width of the texture. */
	uint32 GetSizeX() const { return SizeX; }
	
	/** @return The height of the texture. */
	uint32 GetSizeY() const { return SizeY; }

	/** @return The depth of the texture. */
	uint32 GetSizeZ() const { return SizeZ; }

	virtual FIntVector GetSizeXYZ() const final override
	{
		return FIntVector(SizeX, SizeY, SizeZ);
	}

private:

	uint32 SizeX;
	uint32 SizeY;
	uint32 SizeZ;
};

class RHI_API FRHITextureCube : public FRHITexture
{
public:
	
	/** Initialization constructor. */
	FRHITextureCube(uint32 InSize,uint32 InNumMips,EPixelFormat InFormat,ETextureCreateFlags InFlags, const FClearValueBinding& InClearValue)
	: FRHITexture(RRT_TextureCube, InNumMips,1,InFormat,InFlags, InClearValue)
	, Size(InSize)
	{}
	
	// Dynamic cast methods.
	virtual FRHITextureCube* GetTextureCube() { return this; }
	
	/** @return The width and height of each face of the cubemap. */
	uint32 GetSize() const { return Size; }

	virtual FIntVector GetSizeXYZ() const final override
	{
		return FIntVector(Size, Size, 1);
	}

private:

	uint32 Size;
};

class RHI_API FRHITextureReference final : public FRHITexture
{
public:
	explicit FRHITextureReference()
		: FRHITexture(RRT_TextureReference, 0, 0, PF_Unknown, TexCreate_None, FClearValueBinding())
	{
		check(DefaultTexture);
		ReferencedTexture = DefaultTexture;
	}

	UE_DEPRECATED(5.0, "The InLastRenderTime parameter will be removed in the future")
	explicit FRHITextureReference(FLastRenderTimeContainer* InLastRenderTime)
		: FRHITextureReference()
	{}

	virtual class FRHITextureReference* GetTextureReference() override
	{
		return this;
	}

	virtual FIntVector GetSizeXYZ() const override 
	{
		check(ReferencedTexture);
		return ReferencedTexture->GetSizeXYZ();
	}

	virtual void* GetNativeResource() const override 
	{
		check(ReferencedTexture);
		return ReferencedTexture->GetNativeResource();
	}

	virtual void* GetNativeShaderResourceView() const override
	{
		check(ReferencedTexture);
		return ReferencedTexture->GetNativeShaderResourceView();
	}

	virtual void* GetTextureBaseRHI() override 
	{
		check(ReferencedTexture);
		return ReferencedTexture->GetTextureBaseRHI();
	}

	virtual void GetWriteMaskProperties(void*& OutData, uint32& OutSize) override
	{
		check(ReferencedTexture);
		return ReferencedTexture->GetWriteMaskProperties(OutData, OutSize);
	}

#if ENABLE_RHI_VALIDATION
	virtual RHIValidation::FResource* GetTrackerResource() final override
	{
		check(ReferencedTexture);
		return ReferencedTexture->GetTrackerResource();
	}
#endif

	inline FRHITexture* GetReferencedTexture() const
	{
		return ReferencedTexture.GetReference();
	}

private:
	friend class FRHICommandListImmediate;
	// Called only from FRHICommandListImmediate::UpdateTextureReference()
	void SetReferencedTexture(FRHITexture* InTexture)
	{
		ReferencedTexture = InTexture
			? InTexture
			: DefaultTexture.GetReference();
	}

	TRefCountPtr<FRHITexture> ReferencedTexture;

	// This pointer is set by the InitRHI() function on the FBlackTextureWithSRV global resource,
	// to allow FRHITextureReference to use the global black texture when the reference is nullptr.
	// A pointer is required since FBlackTextureWithSRV is defined in RenderCore.
	friend class FBlackTextureWithSRV;
	static TRefCountPtr<FRHITexture> DefaultTexture;
};

//
// Misc
//

class RHI_API FRHITimestampCalibrationQuery : public FRHIResource
{
public:
	FRHITimestampCalibrationQuery() : FRHIResource(RRT_TimestampCalibrationQuery) {}
	uint64 GPUMicroseconds[MAX_NUM_GPUS] = {};
	uint64 CPUMicroseconds[MAX_NUM_GPUS] = {};
};

/*
* Generic GPU fence class.
* Granularity differs depending on backing RHI - ie it may only represent command buffer granularity.
* RHI specific fences derive from this to implement real GPU->CPU fencing.
* The default implementation always returns false for Poll until the next frame from the frame the fence was inserted
* because not all APIs have a GPU/CPU sync object, we need to fake it.
*/
class RHI_API FRHIGPUFence : public FRHIResource
{
public:
	FRHIGPUFence(FName InName) : FRHIResource(RRT_GPUFence), FenceName(InName) {}
	virtual ~FRHIGPUFence() {}

	virtual void Clear() = 0;

	/**
	 * Poll the fence to see if the GPU has signaled it.
	 * @returns True if and only if the GPU fence has been inserted and the GPU has signaled the fence.
	 */
	virtual bool Poll() const = 0;

	/**
	 * Poll on a subset of the GPUs that this fence supports.
	 */
	virtual bool Poll(FRHIGPUMask GPUMask) const { return Poll(); }

	const FName& GetFName() const { return FenceName; }

	FThreadSafeCounter NumPendingWriteCommands;

protected:
	FName FenceName;
};

// Generic implementation of FRHIGPUFence
class RHI_API FGenericRHIGPUFence : public FRHIGPUFence
{
public:
	FGenericRHIGPUFence(FName InName);

	virtual void Clear() final override;

	/** @discussion RHI implementations must be thread-safe and must correctly handle being called before RHIInsertFence if an RHI thread is active. */
	virtual bool Poll() const final override;

	void WriteInternal();

private:
	uint32 InsertedFrameNumber;
};

class FRHIRenderQuery : public FRHIResource
{
public:
	FRHIRenderQuery() : FRHIResource(RRT_RenderQuery) {}
};

class FRHIRenderQueryPool;
class RHI_API FRHIPooledRenderQuery
{
	TRefCountPtr<FRHIRenderQuery> Query;
	FRHIRenderQueryPool* QueryPool = nullptr;

public:
	FRHIPooledRenderQuery() = default;
	FRHIPooledRenderQuery(FRHIRenderQueryPool* InQueryPool, TRefCountPtr<FRHIRenderQuery>&& InQuery);
	~FRHIPooledRenderQuery();

	FRHIPooledRenderQuery(const FRHIPooledRenderQuery&) = delete;
	FRHIPooledRenderQuery& operator=(const FRHIPooledRenderQuery&) = delete;
	FRHIPooledRenderQuery(FRHIPooledRenderQuery&&) = default;
	FRHIPooledRenderQuery& operator=(FRHIPooledRenderQuery&&) = default;

	bool IsValid() const
	{
		return Query.IsValid();
	}

	FRHIRenderQuery* GetQuery() const
	{
		return Query;
	}

	void ReleaseQuery();
};

class RHI_API FRHIRenderQueryPool : public FRHIResource
{
public:
	FRHIRenderQueryPool() : FRHIResource(RRT_RenderQueryPool) {}
	virtual ~FRHIRenderQueryPool() {};
	virtual FRHIPooledRenderQuery AllocateQuery() = 0;

private:
	friend class FRHIPooledRenderQuery;
	virtual void ReleaseQuery(TRefCountPtr<FRHIRenderQuery>&& Query) = 0;
};

inline FRHIPooledRenderQuery::FRHIPooledRenderQuery(FRHIRenderQueryPool* InQueryPool, TRefCountPtr<FRHIRenderQuery>&& InQuery) 
	: Query(MoveTemp(InQuery))
	, QueryPool(InQueryPool)
{
	check(IsInParallelRenderingThread());
}

inline void FRHIPooledRenderQuery::ReleaseQuery()
{
	if (QueryPool && Query.IsValid())
	{
		QueryPool->ReleaseQuery(MoveTemp(Query));
		QueryPool = nullptr;
	}
	check(!Query.IsValid());
}

inline FRHIPooledRenderQuery::~FRHIPooledRenderQuery()
{
	check(IsInParallelRenderingThread());
	ReleaseQuery();
}

class FRHIComputeFence final : public FRHIResource
{
public:

	FRHIComputeFence(FName InName)
		: FRHIResource(RRT_ComputeFence)
		, Name(InName)
	{}

	FORCEINLINE FName GetName() const
	{
		return Name;
	}

	FORCEINLINE bool GetWriteEnqueued() const
	{
		return Transition != nullptr;
	}

private:
	//debug name of the label.
	FName Name;

public:
	const FRHITransition* Transition = nullptr;
};

class FRHIViewport : public FRHIResource 
{
public:
	FRHIViewport() : FRHIResource(RRT_Viewport) {}

	/**
	 * Returns access to the platform-specific native resource pointer.  This is designed to be used to provide plugins with access
	 * to the underlying resource and should be used very carefully or not at all.
	 *
	 * @return	The pointer to the native resource or NULL if it not initialized or not supported for this resource type for some reason
	 */
	virtual void* GetNativeSwapChain() const { return nullptr; }
	/**
	 * Returns access to the platform-specific native resource pointer to a backbuffer texture.  This is designed to be used to provide plugins with access
	 * to the underlying resource and should be used very carefully or not at all.
	 *
	 * @return	The pointer to the native resource or NULL if it not initialized or not supported for this resource type for some reason
	 */
	virtual void* GetNativeBackBufferTexture() const { return nullptr; }
	/**
	 * Returns access to the platform-specific native resource pointer to a backbuffer rendertarget. This is designed to be used to provide plugins with access
	 * to the underlying resource and should be used very carefully or not at all.
	 *
	 * @return	The pointer to the native resource or NULL if it not initialized or not supported for this resource type for some reason
	 */
	virtual void* GetNativeBackBufferRT() const { return nullptr; }

	/**
	 * Returns access to the platform-specific native window. This is designed to be used to provide plugins with access
	 * to the underlying resource and should be used very carefully or not at all. 
	 *
	 * @return	The pointer to the native resource or NULL if it not initialized or not supported for this resource type for some reason.
	 * AddParam could represent any additional platform-specific data (could be null).
	 */
	virtual void* GetNativeWindow(void** AddParam = nullptr) const { return nullptr; }

	/**
	 * Sets custom Present handler on the viewport
	 */
	virtual void SetCustomPresent(class FRHICustomPresent*) {}

	/**
	 * Returns currently set custom present handler.
	 */
	virtual class FRHICustomPresent* GetCustomPresent() const { return nullptr; }


	/**
	 * Ticks the viewport on the Game thread
	 */
	virtual void Tick(float DeltaTime) {}

	virtual void WaitForFrameEventCompletion() { }

	virtual void IssueFrameEvent() { }
};

//
// Views
//

class FRHIUnorderedAccessView : public FRHIResource
#if ENABLE_RHI_VALIDATION
	, public RHIValidation::FUnorderedAccessView
#endif
{
public:
	FRHIUnorderedAccessView() : FRHIResource(RRT_UnorderedAccessView) {}
};

class FRHIShaderResourceView : public FRHIResource 
#if ENABLE_RHI_VALIDATION
	, public RHIValidation::FShaderResourceView
#endif
{
public:
	FRHIShaderResourceView() : FRHIResource(RRT_ShaderResourceView) {}
};


typedef TRefCountPtr<FRHISamplerState> FSamplerStateRHIRef;
typedef TRefCountPtr<FRHIRasterizerState> FRasterizerStateRHIRef;
typedef TRefCountPtr<FRHIDepthStencilState> FDepthStencilStateRHIRef;
typedef TRefCountPtr<FRHIBlendState> FBlendStateRHIRef;
typedef TRefCountPtr<FRHIVertexDeclaration> FVertexDeclarationRHIRef;
typedef TRefCountPtr<FRHIVertexShader> FVertexShaderRHIRef;
typedef TRefCountPtr<FRHIMeshShader> FMeshShaderRHIRef;
typedef TRefCountPtr<FRHIAmplificationShader> FAmplificationShaderRHIRef;
typedef TRefCountPtr<FRHIPixelShader> FPixelShaderRHIRef;
typedef TRefCountPtr<FRHIGeometryShader> FGeometryShaderRHIRef;
typedef TRefCountPtr<FRHIComputeShader> FComputeShaderRHIRef;
typedef TRefCountPtr<FRHIRayTracingShader>          FRayTracingShaderRHIRef;
typedef TRefCountPtr<FRHIComputeFence>	FComputeFenceRHIRef;
typedef TRefCountPtr<FRHIBoundShaderState> FBoundShaderStateRHIRef;
typedef TRefCountPtr<const FRHIUniformBufferLayout> FUniformBufferLayoutRHIRef;
typedef TRefCountPtr<FRHIUniformBuffer> FUniformBufferRHIRef;
typedef TRefCountPtr<FRHIBuffer> FBufferRHIRef;
UE_DEPRECATED(5.0, "FIndexBufferRHIRef is deprecated, please use FBufferRHIRef.")      typedef FBufferRHIRef FIndexBufferRHIRef;
UE_DEPRECATED(5.0, "FVertexBufferRHIRef is deprecated, please use FBufferRHIRef.")     typedef FBufferRHIRef FVertexBufferRHIRef;
UE_DEPRECATED(5.0, "FStructuredBufferRHIRef is deprecated, please use FBufferRHIRef.") typedef FBufferRHIRef FStructuredBufferRHIRef;
typedef TRefCountPtr<FRHITexture> FTextureRHIRef;
typedef TRefCountPtr<FRHITexture2D> FTexture2DRHIRef;
typedef TRefCountPtr<FRHITexture2DArray> FTexture2DArrayRHIRef;
typedef TRefCountPtr<FRHITexture3D> FTexture3DRHIRef;
typedef TRefCountPtr<FRHITextureCube> FTextureCubeRHIRef;
typedef TRefCountPtr<FRHITextureReference> FTextureReferenceRHIRef;
typedef TRefCountPtr<FRHIRenderQuery> FRenderQueryRHIRef;
typedef TRefCountPtr<FRHIRenderQueryPool> FRenderQueryPoolRHIRef;
typedef TRefCountPtr<FRHITimestampCalibrationQuery> FTimestampCalibrationQueryRHIRef;
typedef TRefCountPtr<FRHIGPUFence>	FGPUFenceRHIRef;
typedef TRefCountPtr<FRHIViewport> FViewportRHIRef;
typedef TRefCountPtr<FRHIUnorderedAccessView> FUnorderedAccessViewRHIRef;
typedef TRefCountPtr<FRHIShaderResourceView> FShaderResourceViewRHIRef;
typedef TRefCountPtr<FRHIGraphicsPipelineState> FGraphicsPipelineStateRHIRef;
typedef TRefCountPtr<FRHIComputePipelineState> FComputePipelineStateRHIRef;
typedef TRefCountPtr<FRHIRayTracingPipelineState> FRayTracingPipelineStateRHIRef;


//
// Ray tracing resources
//

enum class ERayTracingInstanceFlags : uint8
{
	None = 0,
	TriangleCullDisable = 1 << 1, // No back face culling. Triangle is visible from both sides.
	TriangleCullReverse = 1 << 2, // Makes triangle front-facing if its vertices are counterclockwise from ray origin.
	ForceOpaque = 1 << 3, // Disable any-hit shader invocation for this instance.
	ForceNonOpaque = 1 << 4, // Force any-hit shader invocation even if geometries inside the instance were marked opaque.
};
ENUM_CLASS_FLAGS(ERayTracingInstanceFlags);

class FRHIRayTracingGeometry;
/**
* High level descriptor of one or more instances of a mesh in a ray tracing scene.
* All instances covered by this descriptor will share shader bindings, but may have different transforms and user data.
*/
struct FRayTracingGeometryInstance
{
	// TODO: UE-130819 Ref counting is a temporary workaround for a very rare streaming crash.
	TRefCountPtr<FRHIRayTracingGeometry> GeometryRHI = nullptr;

	// A single physical mesh may be duplicated many times in the scene with different transforms and user data.
	// All copies share the same shader binding table entries and therefore will have the same material and shader resources.
	TArrayView<const FMatrix> Transforms;

	TArrayView<const uint32> InstanceSceneDataOffsets;

	// Optional buffer that stores GPU transforms. Used instead of CPU-side transform data.
	FShaderResourceViewRHIRef GPUTransformsSRV = nullptr;

	// Conservative number of instances. Some of the actual instances may be made inactive if GPU transforms are used.
	// Must be less or equal to number of entries in Transforms view if CPU transform data is used.
	// Must be less or equal to number of entries in GPUTransformsSRV if it is non-null.
	uint32 NumTransforms = 0;

	// Each geometry copy can receive a user-provided integer, which can be used to retrieve extra shader parameters or customize appearance.
	// This data can be retrieved using GetInstanceUserData() in closest/any hit shaders.
	// If UserData view is empty, then DefaultUserData value will be used for all instances.
	// If UserData view is used, then it must have the same number of entries as NumInstances.
	uint32 DefaultUserData = 0;
	TArrayView<const uint32> UserData;

	// Each geometry copy can have one bit to make it individually deactivated (removed from TLAS while maintaining hit group indexing). Useful for culling.
	TArrayView<const uint32> ActivationMask;

	// Mask that will be tested against one provided to TraceRay() in shader code.
	// If binary AND of instance mask with ray mask is zero, then the instance is considered not intersected / invisible.
	uint8 Mask = 0xFF;

	// Flags to control triangle back face culling, whether to allow any-hit shaders, etc.
	ERayTracingInstanceFlags Flags = ERayTracingInstanceFlags::None;
};

enum ERayTracingGeometryType
{
	// Indexed or non-indexed triangle list with fixed function ray intersection.
	// Vertex buffer must contain vertex positions as VET_Float3.
	// Vertex stride must be at least 12 bytes, but may be larger to support custom per-vertex data.
	// Index buffer may be provided for indexed triangle lists. Implicit triangle list is assumed otherwise.
	RTGT_Triangles,

	// Custom primitive type that requires an intersection shader.
	// Vertex buffer for procedural geometry must contain one AABB per primitive as {float3 MinXYZ, float3 MaxXYZ}.
	// Vertex stride must be at least 24 bytes, but may be larger to support custom per-primitive data.
	// Index buffers can't be used with procedural geometry.
	RTGT_Procedural,
};
DECLARE_INTRINSIC_TYPE_LAYOUT(ERayTracingGeometryType);

enum class ERayTracingGeometryInitializerType
{
	// Fully initializes the RayTracingGeometry object: creates underlying buffer and initializes shader parameters.
	Rendering,

	// Does not create underlying buffer or shader parameters. Used by the streaming system as an object that is streamed into. 
	StreamingDestination,

	// Creates buffers but does not create shader parameters. Used for intermediate objects in the streaming system.
	StreamingSource,
};
DECLARE_INTRINSIC_TYPE_LAYOUT(ERayTracingGeometryInitializerType);

struct FRayTracingGeometrySegment
{
	DECLARE_TYPE_LAYOUT(FRayTracingGeometrySegment, NonVirtual);
public:
	LAYOUT_FIELD_INITIALIZED(FBufferRHIRef, VertexBuffer, nullptr);
	LAYOUT_FIELD_INITIALIZED(EVertexElementType, VertexBufferElementType, VET_Float3);

	// Offset in bytes from the base address of the vertex buffer.
	LAYOUT_FIELD_INITIALIZED(uint32, VertexBufferOffset, 0);

	// Number of bytes between elements of the vertex buffer (sizeof VET_Float3 by default).
	// Must be equal or greater than the size of the position vector.
	LAYOUT_FIELD_INITIALIZED(uint32, VertexBufferStride, 12);

	// Number of vertices (positions) in VertexBuffer.
	// If an index buffer is present, this must be at least the maximum index value in the index buffer + 1.
	LAYOUT_FIELD_INITIALIZED(uint32, MaxVertices, 0);

	// Primitive range for this segment.
	LAYOUT_FIELD_INITIALIZED(uint32, FirstPrimitive, 0);
	LAYOUT_FIELD_INITIALIZED(uint32, NumPrimitives, 0);

	// Indicates whether any-hit shader could be invoked when hitting this geometry segment.
	// Setting this to `false` turns off any-hit shaders, making the section "opaque" and improving ray tracing performance.
	LAYOUT_FIELD_INITIALIZED(bool, bForceOpaque, false);

	// Any-hit shader may be invoked multiple times for the same primitive during ray traversal.
	// Setting this to `false` guarantees that only a single instance of any-hit shader will run per primitive, at some performance cost.
	LAYOUT_FIELD_INITIALIZED(bool, bAllowDuplicateAnyHitShaderInvocation, true);

	// Indicates whether this section is enabled and should be taken into account during acceleration structure creation
	LAYOUT_FIELD_INITIALIZED(bool, bEnabled, true);
};

struct FRayTracingGeometryInitializer
{
	DECLARE_EXPORTED_TYPE_LAYOUT(FRayTracingGeometryInitializer, RHI_API, NonVirtual);
public:
	LAYOUT_FIELD_INITIALIZED(FBufferRHIRef, IndexBuffer, nullptr);

	// Offset in bytes from the base address of the index buffer.
	LAYOUT_FIELD_INITIALIZED(uint32, IndexBufferOffset, 0);

	LAYOUT_FIELD_INITIALIZED(ERayTracingGeometryType, GeometryType, RTGT_Triangles);

	// Total number of primitives in all segments of the geometry. Only used for validation.
	LAYOUT_FIELD_INITIALIZED(uint32, TotalPrimitiveCount, 0);

	// Partitions of geometry to allow different shader and resource bindings.
	// All ray tracing geometries must have at least one segment.
	LAYOUT_FIELD(TMemoryImageArray<FRayTracingGeometrySegment>, Segments);

	// Offline built geometry data. If null, the geometry will be built by the RHI at runtime.
	LAYOUT_FIELD_INITIALIZED(FResourceArrayInterface*, OfflineData, nullptr);

	// Pointer to an existing ray tracing geometry which the new geometry is built from.
	LAYOUT_FIELD_INITIALIZED(FRHIRayTracingGeometry*, SourceGeometry, nullptr);

	LAYOUT_FIELD_INITIALIZED(bool, bFastBuild, false);
	LAYOUT_FIELD_INITIALIZED(bool, bAllowUpdate, false);
	LAYOUT_FIELD_INITIALIZED(bool, bAllowCompaction, true);
	LAYOUT_FIELD_INITIALIZED(ERayTracingGeometryInitializerType, Type, ERayTracingGeometryInitializerType::Rendering);

	LAYOUT_FIELD(FName, DebugName);
};

enum ERayTracingSceneLifetime
{
	// Scene may only be used during the frame when it was created.
	RTSL_SingleFrame,

	// Scene may be constructed once and used in any number of later frames (not currently implemented).
	// RTSL_MultiFrame,
};

enum class ERayTracingAccelerationStructureFlags
{
	None = 0,
	AllowUpdate = 1 << 0,
	AllowCompaction = 1 << 1,
	FastTrace = 1 << 2,
	FastBuild = 1 << 3,
	MinimizeMemory = 1 << 4,
};
ENUM_CLASS_FLAGS(ERayTracingAccelerationStructureFlags);

struct FRayTracingSceneInitializer
{
	TArrayView<FRayTracingGeometryInstance> Instances;

	// This value controls how many elements will be allocated in the shader binding table per geometry segment.
	// Changing this value allows different hit shaders to be used for different effects.
	// For example, setting this to 2 allows one hit shader for regular material evaluation and a different one for shadows.
	// Desired hit shader can be selected by providing appropriate RayContributionToHitGroupIndex to TraceRay() function.
	// Use ShaderSlot argument in SetRayTracingHitGroup() to assign shaders and resources for specific part of the shder binding table record.
	uint32 ShaderSlotsPerGeometrySegment = 1;

	// Defines how many different callable shaders with unique resource bindings can be bound to this scene.
	// Shaders and resources are assigned to slots in the scene using SetRayTracingCallableShader().
	uint32 NumCallableShaderSlots = 0;

	// At least one miss shader must be present in a ray tracing scene.
	// Default miss shader is always in slot 0. Default shader must not use local resources.
	// Custom miss shaders can be bound to other slots using SetRayTracingMissShader().
	uint32 NumMissShaderSlots = 1;

	// Defines whether data in this scene should persist between frames.
	// Currently only single-frame lifetime is supported.
	ERayTracingSceneLifetime Lifetime = RTSL_SingleFrame;

	FName DebugName;
};

struct FRayTracingSceneInitializer2
{
	// Unique list of geometries referenced by all instances in this scene.
	// Any referenced geometry is kept alive while the scene is alive.
	TArray<TRefCountPtr<FRHIRayTracingGeometry>> ReferencedGeometries;
	// One entry per instance
	TArray<FRHIRayTracingGeometry*> PerInstanceGeometries;
	// Exclusive prefix sum of `Instance.NumTransforms` for all instances in this scene. Used to emulate SV_InstanceID in hit shaders.
	TArray<uint32> BaseInstancePrefixSum;
	// Exclusive prefix sum of instance geometry segments is used to calculate SBT record address from instance and segment indices.
	TArray<uint32> SegmentPrefixSum;

	// Total flattened number of ray tracing geometry instances (a single FRayTracingGeometryInstance may represent many).
	uint32 NumNativeInstances = 0;

	uint32 NumTotalSegments = 0;

	// This value controls how many elements will be allocated in the shader binding table per geometry segment.
	// Changing this value allows different hit shaders to be used for different effects.
	// For example, setting this to 2 allows one hit shader for regular material evaluation and a different one for shadows.
	// Desired hit shader can be selected by providing appropriate RayContributionToHitGroupIndex to TraceRay() function.
	// Use ShaderSlot argument in SetRayTracingHitGroup() to assign shaders and resources for specific part of the shder binding table record.
	uint32 ShaderSlotsPerGeometrySegment = 1;

	// Defines how many different callable shaders with unique resource bindings can be bound to this scene.
	// Shaders and resources are assigned to slots in the scene using SetRayTracingCallableShader().
	uint32 NumCallableShaderSlots = 0;

	// At least one miss shader must be present in a ray tracing scene.
	// Default miss shader is always in slot 0. Default shader must not use local resources.
	// Custom miss shaders can be bound to other slots using SetRayTracingMissShader().
	uint32 NumMissShaderSlots = 1;

	// Defines whether data in this scene should persist between frames.
	// Currently only single-frame lifetime is supported.
	ERayTracingSceneLifetime Lifetime = RTSL_SingleFrame;

	FName DebugName;
};

struct FRayTracingAccelerationStructureSize
{
	uint64 ResultSize = 0;
	uint64 BuildScratchSize = 0;
	uint64 UpdateScratchSize = 0;
};

class FRHIRayTracingAccelerationStructure
	: public FRHIResource
#if ENABLE_RHI_VALIDATION
	, public RHIValidation::FAccelerationStructureResource
#endif
{
public:
	FRHIRayTracingAccelerationStructure() : FRHIResource(RRT_RayTracingAccelerationStructure) {}
};

using FRayTracingAccelerationStructureAddress = uint64;

/** Bottom level ray tracing acceleration structure (contains triangles). */
class FRHIRayTracingGeometry : public FRHIRayTracingAccelerationStructure
{
public:
	FRHIRayTracingGeometry() = default;
	FRHIRayTracingGeometry(const FRayTracingGeometryInitializer& InInitializer)
		: Initializer(InInitializer)
		, InitializedType(InInitializer.Type)
	{}

	virtual FRayTracingAccelerationStructureAddress GetAccelerationStructureAddress(uint64 GPUIndex) const = 0;
	virtual void SetInitializer(const FRayTracingGeometryInitializer& Initializer) = 0;

	const FRayTracingGeometryInitializer& GetInitializer() const
	{
		return Initializer;
	}

	uint32 GetNumSegments() const 
	{ 
		return Initializer.Segments.Num(); 
	}

	FRayTracingAccelerationStructureSize GetSizeInfo() const
	{
		return SizeInfo;
	};
protected:
	FRayTracingAccelerationStructureSize SizeInfo = {};
	FRayTracingGeometryInitializer Initializer = {};
	ERayTracingGeometryInitializerType InitializedType = ERayTracingGeometryInitializerType::Rendering;
};

typedef TRefCountPtr<FRHIRayTracingGeometry>     FRayTracingGeometryRHIRef;

/** Top level ray tracing acceleration structure (contains instances of meshes). */
class FRHIRayTracingScene : public FRHIRayTracingAccelerationStructure
{
public:
	virtual const FRayTracingSceneInitializer2& GetInitializer() const = 0;

	// Returns a buffer view for RHI-specific system parameters associated with this scene.
	// This may be needed to access ray tracing geometry data in shaders that use ray queries.
	// Returns NULL if current RHI does not require this buffer.
	virtual FRHIShaderResourceView* GetMetadataBufferSRV() const
	{
		return nullptr;
	}
};

typedef TRefCountPtr<FRHIRayTracingScene>        FRayTracingSceneRHIRef;


/* Generic staging buffer class used by FRHIGPUMemoryReadback
* RHI specific staging buffers derive from this
*/
class RHI_API FRHIStagingBuffer : public FRHIResource
{
public:
	FRHIStagingBuffer()
		: FRHIResource(RRT_StagingBuffer)
		, bIsLocked(false)
	{}

	virtual ~FRHIStagingBuffer() {}

	virtual void *Lock(uint32 Offset, uint32 NumBytes) = 0;
	virtual void Unlock() = 0;
protected:
	bool bIsLocked;
};

class RHI_API FGenericRHIStagingBuffer : public FRHIStagingBuffer
{
public:
	FGenericRHIStagingBuffer()
		: FRHIStagingBuffer()
	{}

	~FGenericRHIStagingBuffer() {}

	virtual void* Lock(uint32 Offset, uint32 NumBytes) final override;
	virtual void Unlock() final override;
	FBufferRHIRef ShadowBuffer;
	uint32 Offset;
};

typedef TRefCountPtr<FRHIStagingBuffer>	FStagingBufferRHIRef;

class FRHIRenderTargetView
{
public:
	FRHITexture* Texture;
	uint32 MipIndex;

	/** Array slice or texture cube face.  Only valid if texture resource was created with TexCreate_TargetArraySlicesIndependently! */
	uint32 ArraySliceIndex;
	
	ERenderTargetLoadAction LoadAction;
	ERenderTargetStoreAction StoreAction;

	FRHIRenderTargetView() : 
		Texture(NULL),
		MipIndex(0),
		ArraySliceIndex(-1),
		LoadAction(ERenderTargetLoadAction::ENoAction),
		StoreAction(ERenderTargetStoreAction::ENoAction)
	{}

	FRHIRenderTargetView(const FRHIRenderTargetView& Other) :
		Texture(Other.Texture),
		MipIndex(Other.MipIndex),
		ArraySliceIndex(Other.ArraySliceIndex),
		LoadAction(Other.LoadAction),
		StoreAction(Other.StoreAction)
	{}

	//common case
	explicit FRHIRenderTargetView(FRHITexture* InTexture, ERenderTargetLoadAction InLoadAction) :
		Texture(InTexture),
		MipIndex(0),
		ArraySliceIndex(-1),
		LoadAction(InLoadAction),
		StoreAction(ERenderTargetStoreAction::EStore)
	{}

	//common case
	explicit FRHIRenderTargetView(FRHITexture* InTexture, ERenderTargetLoadAction InLoadAction, uint32 InMipIndex, uint32 InArraySliceIndex) :
		Texture(InTexture),
		MipIndex(InMipIndex),
		ArraySliceIndex(InArraySliceIndex),
		LoadAction(InLoadAction),
		StoreAction(ERenderTargetStoreAction::EStore)
	{}
	
	explicit FRHIRenderTargetView(FRHITexture* InTexture, uint32 InMipIndex, uint32 InArraySliceIndex, ERenderTargetLoadAction InLoadAction, ERenderTargetStoreAction InStoreAction) :
		Texture(InTexture),
		MipIndex(InMipIndex),
		ArraySliceIndex(InArraySliceIndex),
		LoadAction(InLoadAction),
		StoreAction(InStoreAction)
	{}

	bool operator==(const FRHIRenderTargetView& Other) const
	{
		return 
			Texture == Other.Texture &&
			MipIndex == Other.MipIndex &&
			ArraySliceIndex == Other.ArraySliceIndex &&
			LoadAction == Other.LoadAction &&
			StoreAction == Other.StoreAction;
	}
};

class FRHIDepthRenderTargetView
{
public:
	FRHITexture* Texture;

	ERenderTargetLoadAction		DepthLoadAction;
	ERenderTargetStoreAction	DepthStoreAction;
	ERenderTargetLoadAction		StencilLoadAction;

private:
	ERenderTargetStoreAction	StencilStoreAction;
	FExclusiveDepthStencil		DepthStencilAccess;
public:

	// accessor to prevent write access to StencilStoreAction
	ERenderTargetStoreAction GetStencilStoreAction() const { return StencilStoreAction; }
	// accessor to prevent write access to DepthStencilAccess
	FExclusiveDepthStencil GetDepthStencilAccess() const { return DepthStencilAccess; }

	explicit FRHIDepthRenderTargetView() :
		Texture(nullptr),
		DepthLoadAction(ERenderTargetLoadAction::ENoAction),
		DepthStoreAction(ERenderTargetStoreAction::ENoAction),
		StencilLoadAction(ERenderTargetLoadAction::ENoAction),
		StencilStoreAction(ERenderTargetStoreAction::ENoAction),
		DepthStencilAccess(FExclusiveDepthStencil::DepthNop_StencilNop)
	{
		Validate();
	}

	//common case
	explicit FRHIDepthRenderTargetView(FRHITexture* InTexture, ERenderTargetLoadAction InLoadAction, ERenderTargetStoreAction InStoreAction) :
		Texture(InTexture),
		DepthLoadAction(InLoadAction),
		DepthStoreAction(InStoreAction),
		StencilLoadAction(InLoadAction),
		StencilStoreAction(InStoreAction),
		DepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilWrite)
	{
		Validate();
	}

	explicit FRHIDepthRenderTargetView(FRHITexture* InTexture, ERenderTargetLoadAction InLoadAction, ERenderTargetStoreAction InStoreAction, FExclusiveDepthStencil InDepthStencilAccess) :
		Texture(InTexture),
		DepthLoadAction(InLoadAction),
		DepthStoreAction(InStoreAction),
		StencilLoadAction(InLoadAction),
		StencilStoreAction(InStoreAction),
		DepthStencilAccess(InDepthStencilAccess)
	{
		Validate();
	}

	explicit FRHIDepthRenderTargetView(FRHITexture* InTexture, ERenderTargetLoadAction InDepthLoadAction, ERenderTargetStoreAction InDepthStoreAction, ERenderTargetLoadAction InStencilLoadAction, ERenderTargetStoreAction InStencilStoreAction) :
		Texture(InTexture),
		DepthLoadAction(InDepthLoadAction),
		DepthStoreAction(InDepthStoreAction),
		StencilLoadAction(InStencilLoadAction),
		StencilStoreAction(InStencilStoreAction),
		DepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilWrite)
	{
		Validate();
	}

	explicit FRHIDepthRenderTargetView(FRHITexture* InTexture, ERenderTargetLoadAction InDepthLoadAction, ERenderTargetStoreAction InDepthStoreAction, ERenderTargetLoadAction InStencilLoadAction, ERenderTargetStoreAction InStencilStoreAction, FExclusiveDepthStencil InDepthStencilAccess) :
		Texture(InTexture),
		DepthLoadAction(InDepthLoadAction),
		DepthStoreAction(InDepthStoreAction),
		StencilLoadAction(InStencilLoadAction),
		StencilStoreAction(InStencilStoreAction),
		DepthStencilAccess(InDepthStencilAccess)
	{
		Validate();
	}

	void Validate() const
	{
		// VK and Metal MAY leave the attachment in an undefined state if the StoreAction is DontCare. So we can't assume read-only implies it should be DontCare unless we know for sure it will never be used again.
		// ensureMsgf(DepthStencilAccess.IsDepthWrite() || DepthStoreAction == ERenderTargetStoreAction::ENoAction, TEXT("Depth is read-only, but we are performing a store.  This is a waste on mobile.  If depth can't change, we don't need to store it out again"));
		/*ensureMsgf(DepthStencilAccess.IsStencilWrite() || StencilStoreAction == ERenderTargetStoreAction::ENoAction, TEXT("Stencil is read-only, but we are performing a store.  This is a waste on mobile.  If stencil can't change, we don't need to store it out again"));*/
	}

	bool operator==(const FRHIDepthRenderTargetView& Other) const
	{
		return
			Texture == Other.Texture &&
			DepthLoadAction == Other.DepthLoadAction &&
			DepthStoreAction == Other.DepthStoreAction &&
			StencilLoadAction == Other.StencilLoadAction &&
			StencilStoreAction == Other.StencilStoreAction &&
			DepthStencilAccess == Other.DepthStencilAccess;
	}
};

class FRHISetRenderTargetsInfo
{
public:
	// Color Render Targets Info
	FRHIRenderTargetView ColorRenderTarget[MaxSimultaneousRenderTargets];	
	int32 NumColorRenderTargets;
	bool bClearColor;

	// Color Render Targets Info
	FRHIRenderTargetView ColorResolveRenderTarget[MaxSimultaneousRenderTargets];	
	bool bHasResolveAttachments;

	// Depth/Stencil Render Target Info
	FRHIDepthRenderTargetView DepthStencilRenderTarget;	
	bool bClearDepth;
	bool bClearStencil;

	FRHITexture* ShadingRateTexture;
	EVRSRateCombiner ShadingRateTextureCombiner;

	uint8 MultiViewCount;

	FRHISetRenderTargetsInfo() :
		NumColorRenderTargets(0),
		bClearColor(false),
		bHasResolveAttachments(false),
		bClearDepth(false),
		ShadingRateTexture(nullptr),
		MultiViewCount(0)
	{}

	FRHISetRenderTargetsInfo(int32 InNumColorRenderTargets, const FRHIRenderTargetView* InColorRenderTargets, const FRHIDepthRenderTargetView& InDepthStencilRenderTarget) :
		NumColorRenderTargets(InNumColorRenderTargets),
		bClearColor(InNumColorRenderTargets > 0 && InColorRenderTargets[0].LoadAction == ERenderTargetLoadAction::EClear),
		bHasResolveAttachments(false),
		DepthStencilRenderTarget(InDepthStencilRenderTarget),		
		bClearDepth(InDepthStencilRenderTarget.Texture && InDepthStencilRenderTarget.DepthLoadAction == ERenderTargetLoadAction::EClear),
		ShadingRateTexture(nullptr),
		ShadingRateTextureCombiner(VRSRB_Passthrough)
	{
		check(InNumColorRenderTargets <= 0 || InColorRenderTargets);
		for (int32 Index = 0; Index < InNumColorRenderTargets; ++Index)
		{
			ColorRenderTarget[Index] = InColorRenderTargets[Index];			
		}
	}
	// @todo metal mrt: This can go away after all the cleanup is done
	void SetClearDepthStencil(bool bInClearDepth, bool bInClearStencil = false)
	{
		if (bInClearDepth)
		{
			DepthStencilRenderTarget.DepthLoadAction = ERenderTargetLoadAction::EClear;
		}
		if (bInClearStencil)
		{
			DepthStencilRenderTarget.StencilLoadAction = ERenderTargetLoadAction::EClear;
		}
		bClearDepth = bInClearDepth;		
		bClearStencil = bInClearStencil;		
	}

	uint32 CalculateHash() const
	{
		// Need a separate struct so we can memzero/remove dependencies on reference counts
		struct FHashableStruct
		{
			// *2 for color and resolves, depth goes in the second-to-last slot, shading rate goes in the last slot
			FRHITexture* Texture[MaxSimultaneousRenderTargets*2 + 2];
			uint32 MipIndex[MaxSimultaneousRenderTargets];
			uint32 ArraySliceIndex[MaxSimultaneousRenderTargets];
			ERenderTargetLoadAction LoadAction[MaxSimultaneousRenderTargets];
			ERenderTargetStoreAction StoreAction[MaxSimultaneousRenderTargets];

			ERenderTargetLoadAction		DepthLoadAction;
			ERenderTargetStoreAction	DepthStoreAction;
			ERenderTargetLoadAction		StencilLoadAction;
			ERenderTargetStoreAction	StencilStoreAction;
			FExclusiveDepthStencil		DepthStencilAccess;

			bool bClearDepth;
			bool bClearStencil;
			bool bClearColor;
			bool bHasResolveAttachments;
			FRHIUnorderedAccessView* UnorderedAccessView[MaxSimultaneousUAVs];
			uint8 MultiViewCount;

			void Set(const FRHISetRenderTargetsInfo& RTInfo)
			{
				FMemory::Memzero(*this);
				for (int32 Index = 0; Index < RTInfo.NumColorRenderTargets; ++Index)
				{
					Texture[Index] = RTInfo.ColorRenderTarget[Index].Texture;
					Texture[MaxSimultaneousRenderTargets+Index] = RTInfo.ColorResolveRenderTarget[Index].Texture;
					MipIndex[Index] = RTInfo.ColorRenderTarget[Index].MipIndex;
					ArraySliceIndex[Index] = RTInfo.ColorRenderTarget[Index].ArraySliceIndex;
					LoadAction[Index] = RTInfo.ColorRenderTarget[Index].LoadAction;
					StoreAction[Index] = RTInfo.ColorRenderTarget[Index].StoreAction;
				}

				Texture[MaxSimultaneousRenderTargets] = RTInfo.DepthStencilRenderTarget.Texture;
				Texture[MaxSimultaneousRenderTargets + 1] = RTInfo.ShadingRateTexture;
				DepthLoadAction = RTInfo.DepthStencilRenderTarget.DepthLoadAction;
				DepthStoreAction = RTInfo.DepthStencilRenderTarget.DepthStoreAction;
				StencilLoadAction = RTInfo.DepthStencilRenderTarget.StencilLoadAction;
				StencilStoreAction = RTInfo.DepthStencilRenderTarget.GetStencilStoreAction();
				DepthStencilAccess = RTInfo.DepthStencilRenderTarget.GetDepthStencilAccess();

				bClearDepth = RTInfo.bClearDepth;
				bClearStencil = RTInfo.bClearStencil;
				bClearColor = RTInfo.bClearColor;
				bHasResolveAttachments = RTInfo.bHasResolveAttachments;
				MultiViewCount = RTInfo.MultiViewCount;
			}
		};

		FHashableStruct RTHash;
		FMemory::Memzero(RTHash);
		RTHash.Set(*this);
		return FCrc::MemCrc32(&RTHash, sizeof(RTHash));
	}
};

class FRHICustomPresent : public FRHIResource
{
public:
	FRHICustomPresent() : FRHIResource(RRT_CustomPresent) {}
	
	virtual ~FRHICustomPresent() {} // should release any references to D3D resources.
	
	// Called when viewport is resized.
	virtual void OnBackBufferResize() = 0;

	// Called from render thread to see if a native present will be requested for this frame.
	// @return	true if native Present will be requested for this frame; false otherwise.  Must
	// match value subsequently returned by Present for this frame.
	virtual bool NeedsNativePresent() = 0;
	// In come cases we want to use custom present but still let the native environment handle 
	// advancement of the backbuffer indices.
	// @return true if backbuffer index should advance independently from CustomPresent.
	virtual bool NeedsAdvanceBackbuffer() { return false; };

	// Called from RHI thread when the engine begins drawing to the viewport.
	virtual void BeginDrawing() {};

	// Called from RHI thread to perform custom present.
	// @param InOutSyncInterval - in out param, indicates if vsync is on (>0) or off (==0).
	// @return	true if native Present should be also be performed; false otherwise. If it returns
	// true, then InOutSyncInterval could be modified to switch between VSync/NoVSync for the normal 
	// Present.  Must match value previously returned by NeedsNormalPresent for this frame.
	virtual bool Present(int32& InOutSyncInterval) = 0;

	// Called from RHI thread after native Present has been called
	virtual void PostPresent() {};

	// Called when rendering thread is acquired
	virtual void OnAcquireThreadOwnership() {}
	// Called when rendering thread is released
	virtual void OnReleaseThreadOwnership() {}
};


typedef TRefCountPtr<FRHICustomPresent> FCustomPresentRHIRef;

// Templates to convert an FRHI*Shader to its enum
template<typename TRHIShader> struct TRHIShaderToEnum {};
template<> struct TRHIShaderToEnum<FRHIVertexShader>           { enum { ShaderFrequency = SF_Vertex        }; };
template<> struct TRHIShaderToEnum<FRHIMeshShader>             { enum { ShaderFrequency = SF_Mesh          }; };
template<> struct TRHIShaderToEnum<FRHIAmplificationShader>    { enum { ShaderFrequency = SF_Amplification }; };
template<> struct TRHIShaderToEnum<FRHIPixelShader>            { enum { ShaderFrequency = SF_Pixel         }; };
template<> struct TRHIShaderToEnum<FRHIGeometryShader>         { enum { ShaderFrequency = SF_Geometry      }; };
template<> struct TRHIShaderToEnum<FRHIComputeShader>          { enum { ShaderFrequency = SF_Compute       }; };
template<> struct TRHIShaderToEnum<FRHIVertexShader*>          { enum { ShaderFrequency = SF_Vertex        }; };
template<> struct TRHIShaderToEnum<FRHIMeshShader*>            { enum { ShaderFrequency = SF_Mesh          }; };
template<> struct TRHIShaderToEnum<FRHIAmplificationShader*>   { enum { ShaderFrequency = SF_Amplification }; };
template<> struct TRHIShaderToEnum<FRHIPixelShader*>           { enum { ShaderFrequency = SF_Pixel         }; };
template<> struct TRHIShaderToEnum<FRHIGeometryShader*>        { enum { ShaderFrequency = SF_Geometry      }; };
template<> struct TRHIShaderToEnum<FRHIComputeShader*>         { enum { ShaderFrequency = SF_Compute       }; };
template<> struct TRHIShaderToEnum<FVertexShaderRHIRef>        { enum { ShaderFrequency = SF_Vertex        }; };
template<> struct TRHIShaderToEnum<FMeshShaderRHIRef>          { enum { ShaderFrequency = SF_Mesh          }; };
template<> struct TRHIShaderToEnum<FAmplificationShaderRHIRef> { enum { ShaderFrequency = SF_Amplification }; };
template<> struct TRHIShaderToEnum<FPixelShaderRHIRef>         { enum { ShaderFrequency = SF_Pixel         }; };
template<> struct TRHIShaderToEnum<FGeometryShaderRHIRef>      { enum { ShaderFrequency = SF_Geometry      }; };
template<> struct TRHIShaderToEnum<FComputeShaderRHIRef>       { enum { ShaderFrequency = SF_Compute       }; };

template<typename TRHIShaderType>
inline const TCHAR* GetShaderFrequencyString(bool bIncludePrefix = true)
{
	return GetShaderFrequencyString(static_cast<EShaderFrequency>(TRHIShaderToEnum<TRHIShaderType>::ShaderFrequency), bIncludePrefix);
}

struct FBoundShaderStateInput
{
	inline FBoundShaderStateInput() {}

	inline FBoundShaderStateInput
	(
		FRHIVertexDeclaration* InVertexDeclarationRHI
		, FRHIVertexShader* InVertexShaderRHI
		, FRHIPixelShader* InPixelShaderRHI
#if PLATFORM_SUPPORTS_GEOMETRY_SHADERS
		, FRHIGeometryShader* InGeometryShaderRHI
#endif
	)
		: VertexDeclarationRHI(InVertexDeclarationRHI)
		, VertexShaderRHI(InVertexShaderRHI)
		, PixelShaderRHI(InPixelShaderRHI)
#if PLATFORM_SUPPORTS_GEOMETRY_SHADERS
		, GeometryShaderRHI(InGeometryShaderRHI)
#endif
	{
	}

#if PLATFORM_SUPPORTS_MESH_SHADERS
	inline FBoundShaderStateInput(
		FRHIMeshShader* InMeshShaderRHI,
		FRHIAmplificationShader* InAmplificationShader,
		FRHIPixelShader* InPixelShaderRHI)
		: PixelShaderRHI(InPixelShaderRHI)
		, MeshShaderRHI(InMeshShaderRHI)
		, AmplificationShaderRHI(InAmplificationShader)
	{
	}
#endif

	void AddRefResources()
	{
		if (GetMeshShader())
		{
			check(VertexDeclarationRHI == nullptr);
			check(VertexShaderRHI == nullptr);
			GetMeshShader()->AddRef();

			if (GetAmplificationShader())
			{
				GetAmplificationShader()->AddRef();
			}
		}
		else
		{
			check(VertexDeclarationRHI);
			VertexDeclarationRHI->AddRef();

			check(VertexShaderRHI);
			VertexShaderRHI->AddRef();
		}

		if (PixelShaderRHI)
		{
			PixelShaderRHI->AddRef();
		}

		if (GetGeometryShader())
		{
			GetGeometryShader()->AddRef();
		}
	}

	void ReleaseResources()
	{
		if (GetMeshShader())
		{
			check(VertexDeclarationRHI == nullptr);
			check(VertexShaderRHI == nullptr);
			GetMeshShader()->Release();

			if (GetAmplificationShader())
			{
				GetAmplificationShader()->Release();
			}
		}
		else
		{
			check(VertexDeclarationRHI);
			VertexDeclarationRHI->Release();

			check(VertexShaderRHI);
			VertexShaderRHI->Release();
		}

		if (PixelShaderRHI)
		{
			PixelShaderRHI->Release();
		}

		if (GetGeometryShader())
		{
			GetGeometryShader()->Release();
		}
	}

	FRHIVertexShader* GetVertexShader() const { return VertexShaderRHI; }
	FRHIPixelShader* GetPixelShader() const { return PixelShaderRHI; }

#if PLATFORM_SUPPORTS_MESH_SHADERS
	FRHIMeshShader* GetMeshShader() const { return MeshShaderRHI; }
	void SetMeshShader(FRHIMeshShader* InMeshShader) { MeshShaderRHI = InMeshShader; }
	FRHIAmplificationShader* GetAmplificationShader() const { return AmplificationShaderRHI; }
	void SetAmplificationShader(FRHIAmplificationShader* InAmplificationShader) { AmplificationShaderRHI = InAmplificationShader; }
#else
	constexpr FRHIMeshShader* GetMeshShader() const { return nullptr; }
	void SetMeshShader(FRHIMeshShader*) {}
	constexpr FRHIAmplificationShader* GetAmplificationShader() const { return nullptr; }
	void SetAmplificationShader(FRHIAmplificationShader*) {}
#endif

#if PLATFORM_SUPPORTS_GEOMETRY_SHADERS
	FRHIGeometryShader* GetGeometryShader() const { return GeometryShaderRHI; }
	void SetGeometryShader(FRHIGeometryShader* InGeometryShader) { GeometryShaderRHI = InGeometryShader; }
#else
	constexpr FRHIGeometryShader* GetGeometryShader() const { return nullptr; }
	void SetGeometryShader(FRHIGeometryShader*) {}
#endif

	FRHIVertexDeclaration* VertexDeclarationRHI = nullptr;
	FRHIVertexShader* VertexShaderRHI = nullptr;
	FRHIPixelShader* PixelShaderRHI = nullptr;
private:
#if PLATFORM_SUPPORTS_MESH_SHADERS
	FRHIMeshShader* MeshShaderRHI = nullptr;
	FRHIAmplificationShader* AmplificationShaderRHI = nullptr;
#endif
#if PLATFORM_SUPPORTS_GEOMETRY_SHADERS
	FRHIGeometryShader* GeometryShaderRHI = nullptr;
#endif
};

struct FImmutableSamplerState
{
	using TImmutableSamplers = TStaticArray<FRHISamplerState*, MaxImmutableSamplers>;

	FImmutableSamplerState()
		: ImmutableSamplers(InPlace, nullptr)
	{}

	void Reset()
	{
		for (uint32 Index = 0; Index < MaxImmutableSamplers; ++Index)
		{
			ImmutableSamplers[Index] = nullptr;
		}
	}

	bool operator==(const FImmutableSamplerState& rhs) const
	{
		return ImmutableSamplers == rhs.ImmutableSamplers;
	}

	bool operator!=(const FImmutableSamplerState& rhs) const
	{
		return ImmutableSamplers != rhs.ImmutableSamplers;
	}

	TImmutableSamplers ImmutableSamplers;
};

// Hints for some RHIs that support subpasses
enum class ESubpassHint : uint8
{
	// Regular rendering
	None,

	// Render pass has depth reading subpass
	DepthReadSubpass,

	// Mobile defferred shading subpass
	DeferredShadingSubpass,
};

enum class EConservativeRasterization : uint8
{
	Disabled,
	Overestimated,
};

struct FGraphicsPipelineRenderTargetsInfo
{
	FGraphicsPipelineRenderTargetsInfo()
		: RenderTargetFormats(InPlace, UE_PIXELFORMAT_TO_UINT8(PF_Unknown))
		, RenderTargetFlags(InPlace, TexCreate_None)
		, DepthStencilAccess(FExclusiveDepthStencil::DepthNop_StencilNop)
	{
	}

	uint32															RenderTargetsEnabled = 0;
	TStaticArray<uint8, MaxSimultaneousRenderTargets>				RenderTargetFormats;
	TStaticArray<ETextureCreateFlags, MaxSimultaneousRenderTargets>	RenderTargetFlags;
	EPixelFormat													DepthStencilTargetFormat = PF_Unknown;
	ETextureCreateFlags												DepthStencilTargetFlag = ETextureCreateFlags::None;
	ERenderTargetLoadAction											DepthTargetLoadAction = ERenderTargetLoadAction::ENoAction;
	ERenderTargetStoreAction										DepthTargetStoreAction = ERenderTargetStoreAction::ENoAction;
	ERenderTargetLoadAction											StencilTargetLoadAction = ERenderTargetLoadAction::ENoAction;
	ERenderTargetStoreAction										StencilTargetStoreAction = ERenderTargetStoreAction::ENoAction;
	FExclusiveDepthStencil											DepthStencilAccess;
	uint16															NumSamples = 0;
	uint8															MultiViewCount = 0;
	bool															bHasFragmentDensityAttachment = false;
};


class FGraphicsPipelineStateInitializer
{
public:
	// Can't use TEnumByte<EPixelFormat> as it changes the struct to be non trivially constructible, breaking memset
	using TRenderTargetFormats		= TStaticArray<uint8/*EPixelFormat*/, MaxSimultaneousRenderTargets>;
	using TRenderTargetFlags		= TStaticArray<ETextureCreateFlags, MaxSimultaneousRenderTargets>;

	FGraphicsPipelineStateInitializer()
		: BlendState(nullptr)
		, RasterizerState(nullptr)
		, DepthStencilState(nullptr)
		, RenderTargetsEnabled(0)
		, RenderTargetFormats(InPlace, UE_PIXELFORMAT_TO_UINT8(PF_Unknown))
		, RenderTargetFlags(InPlace, TexCreate_None)
		, DepthStencilTargetFormat(PF_Unknown)
		, DepthStencilTargetFlag(TexCreate_None)
		, DepthTargetLoadAction(ERenderTargetLoadAction::ENoAction)
		, DepthTargetStoreAction(ERenderTargetStoreAction::ENoAction)
		, StencilTargetLoadAction(ERenderTargetLoadAction::ENoAction)
		, StencilTargetStoreAction(ERenderTargetStoreAction::ENoAction)
		, NumSamples(0)
		, SubpassHint(ESubpassHint::None)
		, SubpassIndex(0)
		, ConservativeRasterization(EConservativeRasterization::Disabled)
		, bDepthBounds(false)
		, MultiViewCount(0)
		, bHasFragmentDensityAttachment(false)
		, ShadingRate(EVRSShadingRate::VRSSR_1x1)
		, Flags(0)
	{
#if PLATFORM_WINDOWS
		static_assert(sizeof(TRenderTargetFormats::ElementType) == sizeof(uint8/*EPixelFormat*/), "Change TRenderTargetFormats's uint8 to EPixelFormat's size!");
#endif
		static_assert(PF_MAX < MAX_uint8, "TRenderTargetFormats assumes EPixelFormat can fit in a uint8!");
	}

	FGraphicsPipelineStateInitializer(
		FBoundShaderStateInput		InBoundShaderState,
		FRHIBlendState*				InBlendState,
		FRHIRasterizerState*		InRasterizerState,
		FRHIDepthStencilState*		InDepthStencilState,
		FImmutableSamplerState		InImmutableSamplerState,
		EPrimitiveType				InPrimitiveType,
		uint32						InRenderTargetsEnabled,
		const TRenderTargetFormats&	InRenderTargetFormats,
		const TRenderTargetFlags&	InRenderTargetFlags,
		EPixelFormat				InDepthStencilTargetFormat,
		ETextureCreateFlags			InDepthStencilTargetFlag,
		ERenderTargetLoadAction		InDepthTargetLoadAction,
		ERenderTargetStoreAction	InDepthTargetStoreAction,
		ERenderTargetLoadAction		InStencilTargetLoadAction,
		ERenderTargetStoreAction	InStencilTargetStoreAction,
		FExclusiveDepthStencil		InDepthStencilAccess,
		uint16						InNumSamples,
		ESubpassHint				InSubpassHint,
		uint8						InSubpassIndex,
		EConservativeRasterization	InConservativeRasterization,
		uint16						InFlags,
		bool						bInDepthBounds,
		uint8						InMultiViewCount,
		bool						bInHasFragmentDensityAttachment,
		EVRSShadingRate				InShadingRate)
		: BoundShaderState(InBoundShaderState)
		, BlendState(InBlendState)
		, RasterizerState(InRasterizerState)
		, DepthStencilState(InDepthStencilState)
		, ImmutableSamplerState(InImmutableSamplerState)
		, PrimitiveType(InPrimitiveType)
		, RenderTargetsEnabled(InRenderTargetsEnabled)
		, RenderTargetFormats(InRenderTargetFormats)
		, RenderTargetFlags(InRenderTargetFlags)
		, DepthStencilTargetFormat(InDepthStencilTargetFormat)
		, DepthStencilTargetFlag(InDepthStencilTargetFlag)
		, DepthTargetLoadAction(InDepthTargetLoadAction)
		, DepthTargetStoreAction(InDepthTargetStoreAction)
		, StencilTargetLoadAction(InStencilTargetLoadAction)
		, StencilTargetStoreAction(InStencilTargetStoreAction)
		, DepthStencilAccess(InDepthStencilAccess)
		, NumSamples(InNumSamples)
		, SubpassHint(InSubpassHint)
		, SubpassIndex(InSubpassIndex)
		, ConservativeRasterization(EConservativeRasterization::Disabled)
		, bDepthBounds(bInDepthBounds)
		, MultiViewCount(InMultiViewCount)
		, bHasFragmentDensityAttachment(bInHasFragmentDensityAttachment)
		, ShadingRate(InShadingRate)
		, Flags(InFlags)
	{
	}

	bool operator==(const FGraphicsPipelineStateInitializer& rhs) const
	{
		if (BoundShaderState.VertexDeclarationRHI != rhs.BoundShaderState.VertexDeclarationRHI ||
			BoundShaderState.VertexShaderRHI != rhs.BoundShaderState.VertexShaderRHI ||
			BoundShaderState.PixelShaderRHI != rhs.BoundShaderState.PixelShaderRHI ||
			BoundShaderState.GetMeshShader() != rhs.BoundShaderState.GetMeshShader() ||
			BoundShaderState.GetAmplificationShader() != rhs.BoundShaderState.GetAmplificationShader() ||
			BoundShaderState.GetGeometryShader() != rhs.BoundShaderState.GetGeometryShader() ||
			BlendState != rhs.BlendState ||
			RasterizerState != rhs.RasterizerState ||
			DepthStencilState != rhs.DepthStencilState ||
			ImmutableSamplerState != rhs.ImmutableSamplerState ||
			PrimitiveType != rhs.PrimitiveType ||
			bDepthBounds != rhs.bDepthBounds ||
			MultiViewCount != rhs.MultiViewCount ||
			ShadingRate != rhs.ShadingRate ||
			bHasFragmentDensityAttachment != rhs.bHasFragmentDensityAttachment ||
			RenderTargetsEnabled != rhs.RenderTargetsEnabled ||
			RenderTargetFormats != rhs.RenderTargetFormats || 
			RenderTargetFlags != rhs.RenderTargetFlags || 
			DepthStencilTargetFormat != rhs.DepthStencilTargetFormat || 
			DepthStencilTargetFlag != rhs.DepthStencilTargetFlag ||
			DepthTargetLoadAction != rhs.DepthTargetLoadAction ||
			DepthTargetStoreAction != rhs.DepthTargetStoreAction ||
			StencilTargetLoadAction != rhs.StencilTargetLoadAction ||
			StencilTargetStoreAction != rhs.StencilTargetStoreAction || 
			DepthStencilAccess != rhs.DepthStencilAccess ||
			NumSamples != rhs.NumSamples ||
			SubpassHint != rhs.SubpassHint ||
			SubpassIndex != rhs.SubpassIndex ||
			ConservativeRasterization != rhs.ConservativeRasterization)
		{
			return false;
		}

		return true;
	}

	uint32 ComputeNumValidRenderTargets() const
	{
		// Get the count of valid render targets (ignore those at the end of the array with PF_Unknown)
		if (RenderTargetsEnabled > 0)
		{
			int32 LastValidTarget = -1;
			for (int32 i = (int32)RenderTargetsEnabled - 1; i >= 0; i--)
			{
				if (RenderTargetFormats[i] != PF_Unknown)
				{
					LastValidTarget = i;
					break;
				}
			}
			return uint32(LastValidTarget + 1);
		}
		return RenderTargetsEnabled;
	}

	FBoundShaderStateInput			BoundShaderState;
	FRHIBlendState*					BlendState;
	FRHIRasterizerState*			RasterizerState;
	FRHIDepthStencilState*			DepthStencilState;
	FImmutableSamplerState			ImmutableSamplerState;

	EPrimitiveType					PrimitiveType;
	uint32							RenderTargetsEnabled;
	TRenderTargetFormats			RenderTargetFormats;
	TRenderTargetFlags				RenderTargetFlags;
	EPixelFormat					DepthStencilTargetFormat;
	ETextureCreateFlags				DepthStencilTargetFlag;
	ERenderTargetLoadAction			DepthTargetLoadAction;
	ERenderTargetStoreAction		DepthTargetStoreAction;
	ERenderTargetLoadAction			StencilTargetLoadAction;
	ERenderTargetStoreAction		StencilTargetStoreAction;
	FExclusiveDepthStencil			DepthStencilAccess;
	uint16							NumSamples;
	ESubpassHint					SubpassHint;
	uint8							SubpassIndex;
	EConservativeRasterization		ConservativeRasterization;
	bool							bDepthBounds;
	uint8							MultiViewCount;
	bool							bHasFragmentDensityAttachment;
	EVRSShadingRate					ShadingRate;
	
	// Note: these flags do NOT affect compilation of this PSO.
	// The resulting object is invariant with respect to whatever is set here, they are
	// behavior hints.
	// They do not participate in equality comparisons or hashing.
	union
	{
		struct
		{
			uint16					Reserved			: 15;
			uint16					bFromPSOFileCache	: 1;
		};
		uint16						Flags;
	};
};

class FRayTracingPipelineStateSignature
{
public:

	uint32 MaxPayloadSizeInBytes = 24; // sizeof FDefaultPayload declared in RayTracingCommon.ush
	bool bAllowHitGroupIndexing = true;

	// NOTE: GetTypeHash(const FRayTracingPipelineStateInitializer& Initializer) should also be updated when changing this function
	bool operator==(const FRayTracingPipelineStateSignature& rhs) const
	{
		return MaxPayloadSizeInBytes == rhs.MaxPayloadSizeInBytes
			&& bAllowHitGroupIndexing == rhs.bAllowHitGroupIndexing
			&& RayGenHash == rhs.RayGenHash
			&& MissHash == rhs.MissHash
			&& HitGroupHash == rhs.HitGroupHash
			&& CallableHash == rhs.CallableHash;
	}

	friend uint32 GetTypeHash(const FRayTracingPipelineStateSignature& Initializer)
	{
		return GetTypeHash(Initializer.MaxPayloadSizeInBytes) ^
			GetTypeHash(Initializer.bAllowHitGroupIndexing) ^
			GetTypeHash(Initializer.GetRayGenHash()) ^
			GetTypeHash(Initializer.GetRayMissHash()) ^
			GetTypeHash(Initializer.GetHitGroupHash()) ^
			GetTypeHash(Initializer.GetCallableHash());
	}

	uint64 GetHitGroupHash() const { return HitGroupHash; }
	uint64 GetRayGenHash()   const { return RayGenHash; }
	uint64 GetRayMissHash()  const { return MissHash; }
	uint64 GetCallableHash() const { return CallableHash; }

protected:

	uint64 RayGenHash = 0;
	uint64 MissHash = 0;
	uint64 HitGroupHash = 0;
	uint64 CallableHash = 0;
};

class FRayTracingPipelineStateInitializer : public FRayTracingPipelineStateSignature
{
public:

	FRayTracingPipelineStateInitializer() = default;

	// Partial ray tracing pipelines can be used for run-time asynchronous shader compilation, but not for rendering.
	// Any number of shaders for any stage may be provided when creating partial pipelines, but 
	// at least one shader must be present in total (completely empty pipelines are not allowed).
	bool bPartial = false;

	// Ray tracing pipeline may be created by deriving from the existing base.
	// Base pipeline will be extended by adding new shaders into it, potentially saving substantial amount of CPU time.
	// Depends on GRHISupportsRayTracingPSOAdditions support at runtime (base pipeline is simply ignored if it is unsupported).
	FRayTracingPipelineStateRHIRef BasePipeline;

	const TArrayView<FRHIRayTracingShader*>& GetRayGenTable()   const { return RayGenTable; }
	const TArrayView<FRHIRayTracingShader*>& GetMissTable()     const { return MissTable; }
	const TArrayView<FRHIRayTracingShader*>& GetHitGroupTable() const { return HitGroupTable; }
	const TArrayView<FRHIRayTracingShader*>& GetCallableTable() const { return CallableTable; }

	// Shaders used as entry point to ray tracing work. At least one RayGen shader must be provided.
	void SetRayGenShaderTable(const TArrayView<FRHIRayTracingShader*>& InRayGenShaders, uint64 Hash = 0)
	{
		RayGenTable = InRayGenShaders;
		RayGenHash = Hash ? Hash : ComputeShaderTableHash(InRayGenShaders);
	}

	// Shaders that will be invoked if a ray misses all geometry.
	// If this table is empty, then a built-in default miss shader will be used that sets HitT member of FMinimalPayload to -1.
	// Desired miss shader can be selected by providing MissShaderIndex to TraceRay() function.
	void SetMissShaderTable(const TArrayView<FRHIRayTracingShader*>& InMissShaders, uint64 Hash = 0)
	{
		MissTable = InMissShaders;
		MissHash = Hash ? Hash : ComputeShaderTableHash(InMissShaders);
	}

	// Shaders that will be invoked when ray intersects geometry.
	// If this table is empty, then a built-in default shader will be used for all geometry, using FDefaultPayload.
	void SetHitGroupTable(const TArrayView<FRHIRayTracingShader*>& InHitGroups, uint64 Hash = 0)
	{
		HitGroupTable = InHitGroups;
		HitGroupHash = Hash ? Hash : ComputeShaderTableHash(HitGroupTable);
	}

	// Shaders that can be explicitly invoked from RayGen shaders by their Shader Binding Table (SBT) index.
	// SetRayTracingCallableShader() command must be used to fill SBT slots before a shader can be called.
	void SetCallableTable(const TArrayView<FRHIRayTracingShader*>& InCallableShaders, uint64 Hash = 0)
	{
		CallableTable = InCallableShaders;
		CallableHash = Hash ? Hash : ComputeShaderTableHash(CallableTable);
	}

private:

	uint64 ComputeShaderTableHash(const TArrayView<FRHIRayTracingShader*>& ShaderTable, uint64 InitialHash = 5699878132332235837ull)
	{
		uint64 CombinedHash = InitialHash;
		for (FRHIRayTracingShader* ShaderRHI : ShaderTable)
		{
			uint64 ShaderHash; // 64 bits from the shader SHA1
			FMemory::Memcpy(&ShaderHash, ShaderRHI->GetHash().Hash, sizeof(ShaderHash));

			// 64 bit hash combination as per boost::hash_combine_impl
			CombinedHash ^= ShaderHash + 0x9e3779b9 + (CombinedHash << 6) + (CombinedHash >> 2);
		}

		return CombinedHash;
	}

	TArrayView<FRHIRayTracingShader*> RayGenTable;
	TArrayView<FRHIRayTracingShader*> MissTable;
	TArrayView<FRHIRayTracingShader*> HitGroupTable;
	TArrayView<FRHIRayTracingShader*> CallableTable;
};

// This PSO is used as a fallback for RHIs that dont support PSOs. It is used to set the graphics state using the legacy state setting APIs
class FRHIGraphicsPipelineStateFallBack : public FRHIGraphicsPipelineState
{
public:
	FRHIGraphicsPipelineStateFallBack() {}

	FRHIGraphicsPipelineStateFallBack(const FGraphicsPipelineStateInitializer& Init)
		: Initializer(Init)
	{
	}

	FGraphicsPipelineStateInitializer Initializer;
};

class FRHIComputePipelineStateFallback : public FRHIComputePipelineState
{
public:
	FRHIComputePipelineStateFallback(FRHIComputeShader* InComputeShader)
		: ComputeShader(InComputeShader)
	{
		check(InComputeShader);
	}

	FRHIComputeShader* GetComputeShader()
	{
		return ComputeShader;
	}

protected:
	TRefCountPtr<FRHIComputeShader> ComputeShader;
};

//
// Shader Library
//

class FRHIShaderLibrary : public FRHIResource
{
public:
	FRHIShaderLibrary(EShaderPlatform InPlatform, FString const& InName) : FRHIResource(RRT_ShaderLibrary), Platform(InPlatform), LibraryName(InName), LibraryId(GetTypeHash(InName)) {}
	virtual ~FRHIShaderLibrary() {}
	
	FORCEINLINE EShaderPlatform GetPlatform(void) const { return Platform; }
	FORCEINLINE const FString& GetName(void) const { return LibraryName; }
	FORCEINLINE uint32 GetId(void) const { return LibraryId; }
	
	virtual bool IsNativeLibrary() const = 0;
	virtual int32 GetNumShaderMaps() const = 0;
	virtual int32 GetNumShaders() const = 0;
	virtual int32 GetNumShadersForShaderMap(int32 ShaderMapIndex) const = 0;
	virtual int32 GetShaderIndex(int32 ShaderMapIndex, int32 i) const = 0;
	virtual int32 FindShaderMapIndex(const FSHAHash& Hash) = 0;
	virtual int32 FindShaderIndex(const FSHAHash& Hash) = 0;
	virtual bool PreloadShader(int32 ShaderIndex, FGraphEventArray& OutCompletionEvents) { return false; }
	virtual bool PreloadShaderMap(int32 ShaderMapIndex, FGraphEventArray& OutCompletionEvents) { return false; }
	virtual bool PreloadShaderMap(int32 ShaderMapIndex, FCoreDelegates::FAttachShaderReadRequestFunc AttachShaderReadRequestFunc) { return false; }
	virtual void ReleasePreloadedShader(int32 ShaderIndex) {}

	virtual TRefCountPtr<FRHIShader> CreateShader(int32 ShaderIndex) { return nullptr; }
	virtual void Teardown() {};

protected:
	EShaderPlatform Platform;
	FString LibraryName;
	uint32 LibraryId;
};

typedef TRefCountPtr<FRHIShaderLibrary>	FRHIShaderLibraryRef;

class FRHIPipelineBinaryLibrary : public FRHIResource
{
public:
	FRHIPipelineBinaryLibrary(EShaderPlatform InPlatform, FString const& FilePath) : FRHIResource(RRT_PipelineBinaryLibrary), Platform(InPlatform) {}
	virtual ~FRHIPipelineBinaryLibrary() {}
	
	FORCEINLINE EShaderPlatform GetPlatform(void) const { return Platform; }
	
protected:
	EShaderPlatform Platform;
};

typedef TRefCountPtr<FRHIPipelineBinaryLibrary>	FRHIPipelineBinaryLibraryRef;

enum class ERenderTargetActions : uint8
{
	LoadOpMask = 2,

#define RTACTION_MAKE_MASK(Load, Store) (((uint8)ERenderTargetLoadAction::Load << (uint8)LoadOpMask) | (uint8)ERenderTargetStoreAction::Store)

	DontLoad_DontStore =	RTACTION_MAKE_MASK(ENoAction, ENoAction),

	DontLoad_Store =		RTACTION_MAKE_MASK(ENoAction, EStore),
	Clear_Store =			RTACTION_MAKE_MASK(EClear, EStore),
	Load_Store =			RTACTION_MAKE_MASK(ELoad, EStore),

	Clear_DontStore =		RTACTION_MAKE_MASK(EClear, ENoAction),
	Load_DontStore =		RTACTION_MAKE_MASK(ELoad, ENoAction),
	Clear_Resolve =			RTACTION_MAKE_MASK(EClear, EMultisampleResolve),
	Load_Resolve =			RTACTION_MAKE_MASK(ELoad, EMultisampleResolve),

#undef RTACTION_MAKE_MASK
};

inline ERenderTargetActions MakeRenderTargetActions(ERenderTargetLoadAction Load, ERenderTargetStoreAction Store)
{
	return (ERenderTargetActions)(((uint8)Load << (uint8)ERenderTargetActions::LoadOpMask) | (uint8)Store);
}

inline ERenderTargetLoadAction GetLoadAction(ERenderTargetActions Action)
{
	return (ERenderTargetLoadAction)((uint8)Action >> (uint8)ERenderTargetActions::LoadOpMask);
}

inline ERenderTargetStoreAction GetStoreAction(ERenderTargetActions Action)
{
	return (ERenderTargetStoreAction)((uint8)Action & ((1 << (uint8)ERenderTargetActions::LoadOpMask) - 1));
}

enum class EDepthStencilTargetActions : uint8
{
	DepthMask = 4,

#define RTACTION_MAKE_MASK(Depth, Stencil) (((uint8)ERenderTargetActions::Depth << (uint8)DepthMask) | (uint8)ERenderTargetActions::Stencil)

	DontLoad_DontStore =						RTACTION_MAKE_MASK(DontLoad_DontStore, DontLoad_DontStore),
	DontLoad_StoreDepthStencil =				RTACTION_MAKE_MASK(DontLoad_Store, DontLoad_Store),
	DontLoad_StoreStencilNotDepth =				RTACTION_MAKE_MASK(DontLoad_DontStore, DontLoad_Store),
	ClearDepthStencil_StoreDepthStencil =		RTACTION_MAKE_MASK(Clear_Store, Clear_Store),
	LoadDepthStencil_StoreDepthStencil =		RTACTION_MAKE_MASK(Load_Store, Load_Store),
	LoadDepthNotStencil_StoreDepthNotStencil =	RTACTION_MAKE_MASK(Load_Store, DontLoad_DontStore),
	LoadDepthNotStencil_DontStore =				RTACTION_MAKE_MASK(Load_DontStore, DontLoad_DontStore),
	LoadDepthStencil_StoreStencilNotDepth =		RTACTION_MAKE_MASK(Load_DontStore, Load_Store),

	ClearDepthStencil_DontStoreDepthStencil =	RTACTION_MAKE_MASK(Clear_DontStore, Clear_DontStore),
	LoadDepthStencil_DontStoreDepthStencil =	RTACTION_MAKE_MASK(Load_DontStore, Load_DontStore),
	ClearDepthStencil_StoreDepthNotStencil =	RTACTION_MAKE_MASK(Clear_Store, Clear_DontStore),
	ClearDepthStencil_StoreStencilNotDepth =	RTACTION_MAKE_MASK(Clear_DontStore, Clear_Store),
	ClearDepthStencil_ResolveDepthNotStencil =	RTACTION_MAKE_MASK(Clear_Resolve, Clear_DontStore),
	ClearDepthStencil_ResolveStencilNotDepth =	RTACTION_MAKE_MASK(Clear_DontStore, Clear_Resolve),
	LoadDepthClearStencil_StoreDepthStencil  =  RTACTION_MAKE_MASK(Load_Store, Clear_Store),

	ClearStencilDontLoadDepth_StoreStencilNotDepth = RTACTION_MAKE_MASK(DontLoad_DontStore, Clear_Store),

#undef RTACTION_MAKE_MASK
};

inline constexpr EDepthStencilTargetActions MakeDepthStencilTargetActions(const ERenderTargetActions Depth, const ERenderTargetActions Stencil)
{
	return (EDepthStencilTargetActions)(((uint8)Depth << (uint8)EDepthStencilTargetActions::DepthMask) | (uint8)Stencil);
}

inline ERenderTargetActions GetDepthActions(EDepthStencilTargetActions Action)
{
	return (ERenderTargetActions)((uint8)Action >> (uint8)EDepthStencilTargetActions::DepthMask);
}

inline ERenderTargetActions GetStencilActions(EDepthStencilTargetActions Action)
{
	return (ERenderTargetActions)((uint8)Action & ((1 << (uint8)EDepthStencilTargetActions::DepthMask) - 1));
}

struct FRHIRenderPassInfo
{
	struct FColorEntry
	{
		FRHITexture* RenderTarget;
		FRHITexture* ResolveTarget;
		int32 ArraySlice;
		uint8 MipIndex;
		ERenderTargetActions Action;
	};
	FColorEntry ColorRenderTargets[MaxSimultaneousRenderTargets];

	struct FDepthStencilEntry
	{
		FRHITexture* DepthStencilTarget;
		FRHITexture* ResolveTarget;
		EDepthStencilTargetActions Action;
		FExclusiveDepthStencil ExclusiveDepthStencil;
	};
	FDepthStencilEntry DepthStencilRenderTarget;

	// Parameters for resolving a multisampled image
	// When doing raster-only passes with no render targets bound to the pass, use DestRect to describe render area
	FResolveParams ResolveParameters;

	// Some RHIs can use a texture to control the sampling and/or shading resolution of different areas 
	FTextureRHIRef ShadingRateTexture = nullptr;
	EVRSRateCombiner ShadingRateTextureCombiner = VRSRB_Passthrough;

	// Some RHIs require a hint that occlusion queries will be used in this render pass
	uint32 NumOcclusionQueries = 0;
	bool bOcclusionQueries = false;

	// Some RHIs need to know if this render pass is going to be reading and writing to the same texture in the case of generating mip maps for partial resource transitions
	bool bGeneratingMips = false;

	// if this renderpass should be multiview, and if so how many views are required
	uint8 MultiViewCount = 0;

	// Hint for some RHI's that renderpass will have specific sub-passes 
	ESubpassHint SubpassHint = ESubpassHint::None;

	// TODO: Remove once FORT-162640 is solved
	bool bTooManyUAVs = false;


	// Color, no depth, optional resolve, optional mip, optional array slice
	explicit FRHIRenderPassInfo(FRHITexture* ColorRT, ERenderTargetActions ColorAction, FRHITexture* ResolveRT = nullptr, uint8 InMipIndex = 0, int32 InArraySlice = -1)
	{
		check(ColorRT);
		ColorRenderTargets[0].RenderTarget = ColorRT;
		ColorRenderTargets[0].ResolveTarget = ResolveRT;
		ColorRenderTargets[0].ArraySlice = InArraySlice;
		ColorRenderTargets[0].MipIndex = InMipIndex;
		ColorRenderTargets[0].Action = ColorAction;
		DepthStencilRenderTarget.DepthStencilTarget = nullptr;
		DepthStencilRenderTarget.Action = EDepthStencilTargetActions::DontLoad_DontStore;
		DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthNop_StencilNop;
		DepthStencilRenderTarget.ResolveTarget = nullptr;
		bIsMSAA = ColorRT->GetNumSamples() > 1;
		FMemory::Memzero(&ColorRenderTargets[1], sizeof(FColorEntry) * (MaxSimultaneousRenderTargets - 1));
	}

	// Color MRTs, no depth
	explicit FRHIRenderPassInfo(int32 NumColorRTs, FRHITexture* ColorRTs[], ERenderTargetActions ColorAction)
	{
		check(NumColorRTs > 0);
		for (int32 Index = 0; Index < NumColorRTs; ++Index)
		{
			check(ColorRTs[Index]);
			ColorRenderTargets[Index].RenderTarget = ColorRTs[Index];
			ColorRenderTargets[Index].ResolveTarget = nullptr;
			ColorRenderTargets[Index].ArraySlice = -1;
			ColorRenderTargets[Index].MipIndex = 0;
			ColorRenderTargets[Index].Action = ColorAction;
		}
		DepthStencilRenderTarget.DepthStencilTarget = nullptr;
		DepthStencilRenderTarget.Action = EDepthStencilTargetActions::DontLoad_DontStore;
		DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthNop_StencilNop;
		DepthStencilRenderTarget.ResolveTarget = nullptr;
		if (NumColorRTs < MaxSimultaneousRenderTargets)
		{
			FMemory::Memzero(&ColorRenderTargets[NumColorRTs], sizeof(FColorEntry) * (MaxSimultaneousRenderTargets - NumColorRTs));
		}
	}

	// Color MRTs, no depth
	explicit FRHIRenderPassInfo(int32 NumColorRTs, FRHITexture* ColorRTs[], ERenderTargetActions ColorAction, FRHITexture* ResolveTargets[])
	{
		check(NumColorRTs > 0);
		for (int32 Index = 0; Index < NumColorRTs; ++Index)
		{
			check(ColorRTs[Index]);
			ColorRenderTargets[Index].RenderTarget = ColorRTs[Index];
			ColorRenderTargets[Index].ResolveTarget = ResolveTargets[Index];
			ColorRenderTargets[Index].ArraySlice = -1;
			ColorRenderTargets[Index].MipIndex = 0;
			ColorRenderTargets[Index].Action = ColorAction;
		}
		DepthStencilRenderTarget.DepthStencilTarget = nullptr;
		DepthStencilRenderTarget.Action = EDepthStencilTargetActions::DontLoad_DontStore;
		DepthStencilRenderTarget.ExclusiveDepthStencil = FExclusiveDepthStencil::DepthNop_StencilNop;
		DepthStencilRenderTarget.ResolveTarget = nullptr;
		if (NumColorRTs < MaxSimultaneousRenderTargets)
		{
			FMemory::Memzero(&ColorRenderTargets[NumColorRTs], sizeof(FColorEntry) * (MaxSimultaneousRenderTargets - NumColorRTs));
		}
	}

	// Color MRTs and depth
	explicit FRHIRenderPassInfo(int32 NumColorRTs, FRHITexture* ColorRTs[], ERenderTargetActions ColorAction, FRHITexture* DepthRT, EDepthStencilTargetActions DepthActions, FExclusiveDepthStencil InEDS = FExclusiveDepthStencil::DepthWrite_StencilWrite)
	{
		check(NumColorRTs > 0);
		for (int32 Index = 0; Index < NumColorRTs; ++Index)
		{
			check(ColorRTs[Index]);
			ColorRenderTargets[Index].RenderTarget = ColorRTs[Index];
			ColorRenderTargets[Index].ResolveTarget = nullptr;
			ColorRenderTargets[Index].ArraySlice = -1;
			ColorRenderTargets[Index].MipIndex = 0;
			ColorRenderTargets[Index].Action = ColorAction;
		}
		check(DepthRT);
		DepthStencilRenderTarget.DepthStencilTarget = DepthRT;
		DepthStencilRenderTarget.ResolveTarget = nullptr;
		DepthStencilRenderTarget.Action = DepthActions;
		DepthStencilRenderTarget.ExclusiveDepthStencil = InEDS;
		bIsMSAA = DepthRT->GetNumSamples() > 1;
		if (NumColorRTs < MaxSimultaneousRenderTargets)
		{
			FMemory::Memzero(&ColorRenderTargets[NumColorRTs], sizeof(FColorEntry) * (MaxSimultaneousRenderTargets - NumColorRTs));
		}
	}

	// Color MRTs and depth
	explicit FRHIRenderPassInfo(int32 NumColorRTs, FRHITexture* ColorRTs[], ERenderTargetActions ColorAction, FRHITexture* ResolveRTs[], FRHITexture* DepthRT, EDepthStencilTargetActions DepthActions, FRHITexture* ResolveDepthRT, FExclusiveDepthStencil InEDS = FExclusiveDepthStencil::DepthWrite_StencilWrite)
	{
		check(NumColorRTs > 0);
		for (int32 Index = 0; Index < NumColorRTs; ++Index)
		{
			check(ColorRTs[Index]);
			ColorRenderTargets[Index].RenderTarget = ColorRTs[Index];
			ColorRenderTargets[Index].ResolveTarget = ResolveRTs[Index];
			ColorRenderTargets[Index].ArraySlice = -1;
			ColorRenderTargets[Index].MipIndex = 0;
			ColorRenderTargets[Index].Action = ColorAction;
		}
		check(DepthRT);
		DepthStencilRenderTarget.DepthStencilTarget = DepthRT;
		DepthStencilRenderTarget.ResolveTarget = ResolveDepthRT;
		DepthStencilRenderTarget.Action = DepthActions;
		DepthStencilRenderTarget.ExclusiveDepthStencil = InEDS;
		bIsMSAA = DepthRT->GetNumSamples() > 1;
		if (NumColorRTs < MaxSimultaneousRenderTargets)
		{
			FMemory::Memzero(&ColorRenderTargets[NumColorRTs], sizeof(FColorEntry) * (MaxSimultaneousRenderTargets - NumColorRTs));
		}
	}

	// Depth, no color
	explicit FRHIRenderPassInfo(FRHITexture* DepthRT, EDepthStencilTargetActions DepthActions, FRHITexture* ResolveDepthRT = nullptr, FExclusiveDepthStencil InEDS = FExclusiveDepthStencil::DepthWrite_StencilWrite)
	{
		check(DepthRT);
		DepthStencilRenderTarget.DepthStencilTarget = DepthRT;
		DepthStencilRenderTarget.ResolveTarget = ResolveDepthRT;
		DepthStencilRenderTarget.Action = DepthActions;
		DepthStencilRenderTarget.ExclusiveDepthStencil = InEDS;
		bIsMSAA = DepthRT->GetNumSamples() > 1;
		FMemory::Memzero(ColorRenderTargets, sizeof(FColorEntry) * MaxSimultaneousRenderTargets);
	}

	// Depth, no color, occlusion queries
	explicit FRHIRenderPassInfo(FRHITexture* DepthRT, uint32 InNumOcclusionQueries, EDepthStencilTargetActions DepthActions, FRHITexture* ResolveDepthRT = nullptr, FExclusiveDepthStencil InEDS = FExclusiveDepthStencil::DepthWrite_StencilWrite)
		: NumOcclusionQueries(InNumOcclusionQueries)
		, bOcclusionQueries(true)
	{
		check(DepthRT);
		DepthStencilRenderTarget.DepthStencilTarget = DepthRT;
		DepthStencilRenderTarget.ResolveTarget = ResolveDepthRT;
		DepthStencilRenderTarget.Action = DepthActions;
		DepthStencilRenderTarget.ExclusiveDepthStencil = InEDS;
		bIsMSAA = DepthRT->GetNumSamples() > 1;
		FMemory::Memzero(ColorRenderTargets, sizeof(FColorEntry) * MaxSimultaneousRenderTargets);
	}

	// Color and depth
	explicit FRHIRenderPassInfo(FRHITexture* ColorRT, ERenderTargetActions ColorAction, FRHITexture* DepthRT, EDepthStencilTargetActions DepthActions, FExclusiveDepthStencil InEDS = FExclusiveDepthStencil::DepthWrite_StencilWrite)
	{
		check(ColorRT);
		ColorRenderTargets[0].RenderTarget = ColorRT;
		ColorRenderTargets[0].ResolveTarget = nullptr;
		ColorRenderTargets[0].ArraySlice = -1;
		ColorRenderTargets[0].MipIndex = 0;
		ColorRenderTargets[0].Action = ColorAction;
		bIsMSAA = ColorRT->GetNumSamples() > 1;
		check(DepthRT);
		DepthStencilRenderTarget.DepthStencilTarget = DepthRT;
		DepthStencilRenderTarget.ResolveTarget = nullptr;
		DepthStencilRenderTarget.Action = DepthActions;
		DepthStencilRenderTarget.ExclusiveDepthStencil = InEDS;
		FMemory::Memzero(&ColorRenderTargets[1], sizeof(FColorEntry) * (MaxSimultaneousRenderTargets - 1));
	}

	// Color and depth with resolve
	explicit FRHIRenderPassInfo(FRHITexture* ColorRT, ERenderTargetActions ColorAction, FRHITexture* ResolveColorRT,
		FRHITexture* DepthRT, EDepthStencilTargetActions DepthActions, FRHITexture* ResolveDepthRT, FExclusiveDepthStencil InEDS = FExclusiveDepthStencil::DepthWrite_StencilWrite)
	{
		check(ColorRT);
		ColorRenderTargets[0].RenderTarget = ColorRT;
		ColorRenderTargets[0].ResolveTarget = ResolveColorRT;
		ColorRenderTargets[0].ArraySlice = -1;
		ColorRenderTargets[0].MipIndex = 0;
		ColorRenderTargets[0].Action = ColorAction;
		bIsMSAA = ColorRT->GetNumSamples() > 1;
		check(DepthRT);
		DepthStencilRenderTarget.DepthStencilTarget = DepthRT;
		DepthStencilRenderTarget.ResolveTarget = ResolveDepthRT;
		DepthStencilRenderTarget.Action = DepthActions;
		DepthStencilRenderTarget.ExclusiveDepthStencil = InEDS;
		FMemory::Memzero(&ColorRenderTargets[1], sizeof(FColorEntry) * (MaxSimultaneousRenderTargets - 1));
	}

	// Color and depth with resolve and optional sample density
	explicit FRHIRenderPassInfo(FRHITexture* ColorRT, ERenderTargetActions ColorAction, FRHITexture* ResolveColorRT,
		FRHITexture* DepthRT, EDepthStencilTargetActions DepthActions, FRHITexture* ResolveDepthRT, 
		FRHITexture* InShadingRateTexture, EVRSRateCombiner InShadingRateTextureCombiner,
		FExclusiveDepthStencil InEDS = FExclusiveDepthStencil::DepthWrite_StencilWrite)
	{
		check(ColorRT);
		ColorRenderTargets[0].RenderTarget = ColorRT;
		ColorRenderTargets[0].ResolveTarget = ResolveColorRT;
		ColorRenderTargets[0].ArraySlice = -1;
		ColorRenderTargets[0].MipIndex = 0;
		ColorRenderTargets[0].Action = ColorAction;
		bIsMSAA = ColorRT->GetNumSamples() > 1;
		check(DepthRT);
		DepthStencilRenderTarget.DepthStencilTarget = DepthRT;
		DepthStencilRenderTarget.ResolveTarget = ResolveDepthRT;
		DepthStencilRenderTarget.Action = DepthActions;
		DepthStencilRenderTarget.ExclusiveDepthStencil = InEDS;
		ShadingRateTexture = InShadingRateTexture;
		ShadingRateTextureCombiner = InShadingRateTextureCombiner;
		FMemory::Memzero(&ColorRenderTargets[1], sizeof(FColorEntry) * (MaxSimultaneousRenderTargets - 1));
	}

	enum ENoRenderTargets
	{
		NoRenderTargets,
	};
	explicit FRHIRenderPassInfo(ENoRenderTargets Dummy)
	{
		(void)Dummy;
		FMemory::Memzero(*this);
	}

	inline int32 GetNumColorRenderTargets() const
	{
		int32 ColorIndex = 0;
		for (; ColorIndex < MaxSimultaneousRenderTargets; ++ColorIndex)
		{
			const FColorEntry& Entry = ColorRenderTargets[ColorIndex];
			if (!Entry.RenderTarget)
			{
				break;
			}
		}

		return ColorIndex;
	}

	explicit FRHIRenderPassInfo()
	{
		FMemory::Memzero(*this);
	}

	inline bool IsMSAA() const
	{
		return bIsMSAA;
	}

	FGraphicsPipelineRenderTargetsInfo ExtractRenderTargetsInfo() const
	{
		FGraphicsPipelineRenderTargetsInfo RenderTargetsInfo;

		RenderTargetsInfo.NumSamples = 1;
		int32 RenderTargetIndex = 0;

		for (; RenderTargetIndex < MaxSimultaneousRenderTargets; ++RenderTargetIndex)
		{
			FRHITexture* RenderTarget = ColorRenderTargets[RenderTargetIndex].RenderTarget;
			if (!RenderTarget)
			{
				break;
			}

			RenderTargetsInfo.RenderTargetFormats[RenderTargetIndex] = (uint8)RenderTarget->GetFormat();
			RenderTargetsInfo.RenderTargetFlags[RenderTargetIndex] = RenderTarget->GetFlags();
			RenderTargetsInfo.NumSamples |= RenderTarget->GetNumSamples();
		}

		RenderTargetsInfo.RenderTargetsEnabled = RenderTargetIndex;
		for (; RenderTargetIndex < MaxSimultaneousRenderTargets; ++RenderTargetIndex)
		{
			RenderTargetsInfo.RenderTargetFormats[RenderTargetIndex] = PF_Unknown;
		}

		if (DepthStencilRenderTarget.DepthStencilTarget)
		{
			RenderTargetsInfo.DepthStencilTargetFormat = DepthStencilRenderTarget.DepthStencilTarget->GetFormat();
			RenderTargetsInfo.DepthStencilTargetFlag = DepthStencilRenderTarget.DepthStencilTarget->GetFlags();
			RenderTargetsInfo.NumSamples |= DepthStencilRenderTarget.DepthStencilTarget->GetNumSamples();
		}
		else
		{
			RenderTargetsInfo.DepthStencilTargetFormat = PF_Unknown;
		}

		const ERenderTargetActions DepthActions = GetDepthActions(DepthStencilRenderTarget.Action);
		const ERenderTargetActions StencilActions = GetStencilActions(DepthStencilRenderTarget.Action);
		RenderTargetsInfo.DepthTargetLoadAction = GetLoadAction(DepthActions);
		RenderTargetsInfo.DepthTargetStoreAction = GetStoreAction(DepthActions);
		RenderTargetsInfo.StencilTargetLoadAction = GetLoadAction(StencilActions);
		RenderTargetsInfo.StencilTargetStoreAction = GetStoreAction(StencilActions);
		RenderTargetsInfo.DepthStencilAccess = DepthStencilRenderTarget.ExclusiveDepthStencil;

		RenderTargetsInfo.MultiViewCount = MultiViewCount;
		RenderTargetsInfo.bHasFragmentDensityAttachment = ShadingRateTexture != nullptr;

		return RenderTargetsInfo;
	}

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	RHI_API void Validate() const;
#else
	RHI_API void Validate() const {}
#endif
	RHI_API void ConvertToRenderTargetsInfo(FRHISetRenderTargetsInfo& OutRTInfo) const;

#if 0 // FORT-162640
	FRHIRenderPassInfo& operator = (const FRHIRenderPassInfo& In)
	{
		FMemory::Memcpy(*this, In);
		return *this;
	}
#endif

	bool bIsMSAA = false;

private:
	RHI_API void OnVerifyNumUAVsFailed(int32 InNumUAVs);
};

/** Descriptor used to create a texture resource */
struct RHI_API FRHITextureCreateInfo
{
	static FRHITextureCreateInfo Create2D(
		FIntPoint InExtent,
		EPixelFormat InFormat,
		FClearValueBinding InClearValue,
		ETextureCreateFlags InFlags,
		uint8 InNumMips = 1,
		uint8 InNumSamples = 1)
	{
		return FRHITextureCreateInfo(ETextureDimension::Texture2D, InFlags, InFormat, InExtent, InClearValue, 1, 1, InNumMips, InNumSamples);
	}

	static FRHITextureCreateInfo Create2DArray(
		FIntPoint InExtent,
		EPixelFormat InFormat,
		FClearValueBinding InClearValue,
		ETextureCreateFlags InFlags,
		uint16 InArraySize,
		uint8 InNumMips = 1,
		uint8 InNumSamples = 1)
	{
		return FRHITextureCreateInfo(ETextureDimension::Texture2DArray, InFlags, InFormat, InExtent, InClearValue, 1, InArraySize, InNumMips, InNumSamples);
	}

	static FRHITextureCreateInfo Create3D(
		FIntVector InSize,
		EPixelFormat InFormat,
		FClearValueBinding InClearValue,
		ETextureCreateFlags InFlags,
		uint8 InNumMips = 1)
	{
		checkf(InSize.Z >= 0 && InSize.Z <= TNumericLimits<uint16>::Max(), TEXT("Depth parameter (InSize.Z) exceeds valid range"));
		return FRHITextureCreateInfo(ETextureDimension::Texture3D, InFlags, InFormat, FIntPoint(InSize.X, InSize.Y), InClearValue, (uint16)InSize.Z, 1, InNumMips);
	}

	static FRHITextureCreateInfo CreateCube(
		uint32 InSizeInPixels,
		EPixelFormat InFormat,
		FClearValueBinding InClearValue,
		ETextureCreateFlags InFlags,
		uint8 InNumMips = 1,
		uint8 InNumSamples = 1)
	{
		return FRHITextureCreateInfo(ETextureDimension::TextureCube, InFlags, InFormat, FIntPoint(InSizeInPixels, InSizeInPixels), InClearValue, 1, 1, InNumMips, InNumSamples);
	}

	static FRHITextureCreateInfo CreateCubeArray(
		uint32 InSizeInPixels,
		EPixelFormat InFormat,
		FClearValueBinding InClearValue,
		ETextureCreateFlags InFlags,
		uint16 InArraySize,
		uint8 InNumMips = 1,
		uint8 InNumSamples = 1)
	{
		return FRHITextureCreateInfo(ETextureDimension::TextureCubeArray, InFlags, InFormat, FIntPoint(InSizeInPixels, InSizeInPixels), InClearValue, 1, InArraySize, InNumMips, InNumSamples);
	}

	FRHITextureCreateInfo() = default;
	FRHITextureCreateInfo(
		ETextureDimension InDimension,
		ETextureCreateFlags InFlags,
		EPixelFormat InFormat,
		FIntPoint InExtent,
		FClearValueBinding InClearValue,
		uint16 InDepth = 1,
		uint16 InArraySize = 1,
		uint8 InNumMips = 1,
		uint8 InNumSamples = 1)
		: ClearValue(InClearValue)
		, Dimension(InDimension)
		, Flags(InFlags)
		, Format(InFormat)
		, Extent(InExtent)
		, Depth(InDepth)
		, ArraySize(InArraySize)
		, NumMips(InNumMips)
		, NumSamples(InNumSamples)
	{}

	bool operator == (const FRHITextureCreateInfo& Other) const
	{
		return Dimension == Other.Dimension
			&& Flags == Other.Flags
			&& Format == Other.Format
			&& UAVFormat == Other.UAVFormat
			&& Extent == Other.Extent
			&& Depth == Other.Depth
			&& ArraySize == Other.ArraySize
			&& NumMips == Other.NumMips
			&& NumSamples == Other.NumSamples
			&& ClearValue == Other.ClearValue;
	}

	bool operator != (const FRHITextureCreateInfo& Other) const
	{
		return !(*this == Other);
	}

	bool IsTexture2D() const
	{
		return Dimension == ETextureDimension::Texture2D || Dimension == ETextureDimension::Texture2DArray;
	}

	bool IsTexture3D() const
	{
		return Dimension == ETextureDimension::Texture3D;
	}

	bool IsTextureCube() const
	{
		return Dimension == ETextureDimension::TextureCube || Dimension == ETextureDimension::TextureCubeArray;
	}

	bool IsTextureArray() const
	{
		return Dimension == ETextureDimension::Texture2DArray || Dimension == ETextureDimension::TextureCubeArray;
	}

	bool IsMipChain() const
	{
		return NumMips > 1;
	}

	bool IsMultisample() const
	{
		return NumSamples > 1;
	}

	FIntVector GetSize() const
	{
		return FIntVector(Extent.X, Extent.Y, Depth);
	}

	void Reset()
	{
		// Usually we don't want to propagate MSAA samples.
		NumSamples = 1;

		// Remove UAV flag for textures that don't need it (some formats are incompatible).
		Flags |= TexCreate_RenderTargetable;
		Flags &= ~(TexCreate_UAV | TexCreate_ResolveTargetable | TexCreate_DepthStencilResolveTarget);
	}

	/** Returns whether this descriptor conforms to requirements. */
	inline bool IsValid() const
	{
		return FRHITextureCreateInfo::Validate(*this, /* Name = */ TEXT(""), /* bFatal = */ false);
	}

	/** Clear value to use when fast-clearing the texture. */
	FClearValueBinding ClearValue;

	/** Texture dimension to use when creating the RHI texture. */
	ETextureDimension Dimension = ETextureDimension::Texture2D;

	/** Texture flags passed on to RHI texture. */
	ETextureCreateFlags Flags = TexCreate_None;

	/** Pixel format used to create RHI texture. */
	EPixelFormat Format = PF_Unknown;

	/** Texture format used when creating the UAV. PF_Unknown means to use the default one (same as Format). */
	EPixelFormat UAVFormat = PF_Unknown;

	/** Extent of the texture in x and y. */
	FIntPoint Extent = FIntPoint(1, 1);

	/** Depth of the texture if the dimension is 3D. */
	uint16 Depth = 1;

	/** The number of array elements in the texture. (Keep at 1 if dimension is 3D). */
	uint16 ArraySize = 1;

	/** Number of mips in the texture mip-map chain. */
	uint8 NumMips = 1;

	/** Number of samples in the texture. >1 for MSAA. */
	uint8 NumSamples = 1;

	/** Check the validity. */
	static bool CheckValidity(const FRHITextureCreateInfo& Desc, const TCHAR* Name)
	{
		return FRHITextureCreateInfo::Validate(Desc, Name, /* bFatal = */ true);
	}

private:
	static bool Validate(const FRHITextureCreateInfo& Desc, const TCHAR* Name, bool bFatal);
};

/** Used to specify a texture metadata plane when creating a view. */
enum class ERHITextureMetaDataAccess : uint8
{
	/** The primary plane is used with default compression behavior. */
	None = 0,

	/** The primary plane is used without decompressing it. */
	CompressedSurface,

	/** The depth plane is used with default compression behavior. */
	Depth,

	/** The stencil plane is used with default compression behavior. */
	Stencil,

	/** The HTile plane is used. */
	HTile,

	/** the FMask plane is used. */
	FMask,

	/** the CMask plane is used. */
	CMask
};

enum ERHITextureSRVOverrideSRGBType
{
	SRGBO_Default,
	SRGBO_ForceDisable,
};

struct FRHITextureSRVCreateInfo
{
	explicit FRHITextureSRVCreateInfo(uint8 InMipLevel = 0u, uint8 InNumMipLevels = 1u, EPixelFormat InFormat = PF_Unknown)
		: Format(InFormat)
		, MipLevel(InMipLevel)
		, NumMipLevels(InNumMipLevels)
		, SRGBOverride(SRGBO_Default)
		, FirstArraySlice(0)
		, NumArraySlices(0)
	{}

	explicit FRHITextureSRVCreateInfo(uint8 InMipLevel, uint8 InNumMipLevels, uint32 InFirstArraySlice, uint32 InNumArraySlices, EPixelFormat InFormat = PF_Unknown)
		: Format(InFormat)
		, MipLevel(InMipLevel)
		, NumMipLevels(InNumMipLevels)
		, SRGBOverride(SRGBO_Default)
		, FirstArraySlice(InFirstArraySlice)
		, NumArraySlices(InNumArraySlices)
	{}

	/** View the texture with a different format. Leave as PF_Unknown to use original format. Useful when sampling stencil */
	EPixelFormat Format;

	/** Specify the mip level to use. Useful when rendering to one mip while sampling from another */
	uint8 MipLevel;

	/** Create a view to a single, or multiple mip levels */
	uint8 NumMipLevels;

	/** Potentially override the texture's sRGB flag */
	ERHITextureSRVOverrideSRGBType SRGBOverride;

	/** Specify first array slice index. By default 0. */
	uint32 FirstArraySlice;

	/** Specify number of array slices. If FirstArraySlice and NumArraySlices are both zero, the SRV is created for all array slices. By default 0. */
	uint32 NumArraySlices;

	/** Specify the metadata plane to use when creating a view. */
	ERHITextureMetaDataAccess MetaData = ERHITextureMetaDataAccess::None;

	FORCEINLINE bool operator==(const FRHITextureSRVCreateInfo& Other)const
	{
		return (
			Format == Other.Format &&
			MipLevel == Other.MipLevel &&
			NumMipLevels == Other.NumMipLevels &&
			SRGBOverride == Other.SRGBOverride &&
			FirstArraySlice == Other.FirstArraySlice &&
			NumArraySlices == Other.NumArraySlices &&
			MetaData == Other.MetaData);
	}

	FORCEINLINE bool operator!=(const FRHITextureSRVCreateInfo& Other)const
	{
		return !(*this == Other);
	}
};

FORCEINLINE uint32 GetTypeHash(const FRHITextureSRVCreateInfo& Var)
{
	uint32 Hash0 = uint32(Var.Format) | uint32(Var.MipLevel) << 8 | uint32(Var.NumMipLevels) << 16 | uint32(Var.SRGBOverride) << 24;
	return HashCombine(HashCombine(GetTypeHash(Hash0), GetTypeHash(Var.FirstArraySlice)), GetTypeHash(Var.NumArraySlices));
}

struct FRHITextureUAVCreateInfo
{
public:
	FRHITextureUAVCreateInfo() = default;

	explicit FRHITextureUAVCreateInfo(uint8 InMipLevel, EPixelFormat InFormat = PF_Unknown, uint16 InFirstArraySlice = 0, uint16 InNumArraySlices = 0)
		: Format(InFormat)
		, MipLevel(InMipLevel)
		, FirstArraySlice(InFirstArraySlice)
		, NumArraySlices(InNumArraySlices)
	{}

	explicit FRHITextureUAVCreateInfo(ERHITextureMetaDataAccess InMetaData)
		: MetaData(InMetaData)
	{}

	FORCEINLINE bool operator==(const FRHITextureUAVCreateInfo& Other)const
	{
		return Format == Other.Format && MipLevel == Other.MipLevel && MetaData == Other.MetaData && FirstArraySlice == Other.FirstArraySlice && NumArraySlices == Other.NumArraySlices;
	}

	FORCEINLINE bool operator!=(const FRHITextureUAVCreateInfo& Other)const
	{
		return !(*this == Other);
	}

	EPixelFormat Format = PF_Unknown;
	uint8 MipLevel = 0;
	uint16 FirstArraySlice = 0;
	uint16 NumArraySlices = 0;	// When 0, the default behavior will be used, e.g. all slices mapped.
	ERHITextureMetaDataAccess MetaData = ERHITextureMetaDataAccess::None;
};

/** Descriptor used to create a buffer resource */
struct FRHIBufferCreateInfo
{
	bool operator == (const FRHIBufferCreateInfo& Other) const
	{
		return (
			Size == Other.Size &&
			Stride == Other.Stride &&
			Usage == Other.Usage);
	}

	bool operator != (const FRHIBufferCreateInfo& Other) const
	{
		return !(*this == Other);
	}

	/** Total size of the buffer. */
	uint32 Size = 1;

	/** Stride in bytes */
	uint32 Stride = 1;

	/** Bitfields describing the uses of that buffer. */
	EBufferUsageFlags Usage = BUF_None;
};

struct FRHIBufferSRVCreateInfo
{
	explicit FRHIBufferSRVCreateInfo() = default;

	explicit FRHIBufferSRVCreateInfo(EPixelFormat InFormat)
		: Format(InFormat)
	{
		if (InFormat != PF_Unknown)
		{
			BytesPerElement = GPixelFormats[Format].BlockBytes;
		}
	}

	FORCEINLINE bool operator==(const FRHIBufferSRVCreateInfo& Other)const
	{
		return BytesPerElement == Other.BytesPerElement && Format == Other.Format;
	}

	FORCEINLINE bool operator!=(const FRHIBufferSRVCreateInfo& Other)const
	{
		return !(*this == Other);
	}

	/** Number of bytes per element. */
	uint32 BytesPerElement = 1;

	/** Encoding format for the element. */
	EPixelFormat Format = PF_Unknown;
};

struct FRHIBufferUAVCreateInfo
{
	FRHIBufferUAVCreateInfo() = default;

	explicit FRHIBufferUAVCreateInfo(EPixelFormat InFormat)
		: Format(InFormat)
	{}

	FORCEINLINE bool operator==(const FRHIBufferUAVCreateInfo& Other)const
	{
		return Format == Other.Format && bSupportsAtomicCounter == Other.bSupportsAtomicCounter && bSupportsAppendBuffer == Other.bSupportsAppendBuffer;
	}

	FORCEINLINE bool operator!=(const FRHIBufferUAVCreateInfo& Other)const
	{
		return !(*this == Other);
	}

	/** Number of bytes per element (used for typed buffers). */
	EPixelFormat Format = PF_Unknown;

	/** Whether the uav supports atomic counter or append buffer operations (used for structured buffers) */
	bool bSupportsAtomicCounter = false;
	bool bSupportsAppendBuffer = false;
};

class RHI_API FRHITextureViewCache
{
public:
	// Finds a UAV matching the descriptor in the cache or creates a new one and updates the cache.
	FRHIUnorderedAccessView* GetOrCreateUAV(FRHITexture* Texture, const FRHITextureUAVCreateInfo& CreateInfo);

	// Finds a SRV matching the descriptor in the cache or creates a new one and updates the cache.
	FRHIShaderResourceView* GetOrCreateSRV(FRHITexture* Texture, const FRHITextureSRVCreateInfo& CreateInfo);

	// Sets the debug name of the RHI view resources.
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	void SetDebugName(const TCHAR* DebugName);
#else
	void SetDebugName(const TCHAR* DebugName) {}
#endif

private:
	TArray<TPair<FRHITextureUAVCreateInfo, FUnorderedAccessViewRHIRef>, TInlineAllocator<1>> UAVs;
	TArray<TPair<FRHITextureSRVCreateInfo, FShaderResourceViewRHIRef>, TInlineAllocator<1>> SRVs;
};

class RHI_API FRHIBufferViewCache
{
public:
	// Finds a UAV matching the descriptor in the cache or creates a new one and updates the cache.
	FRHIUnorderedAccessView* GetOrCreateUAV(FRHIBuffer* Buffer, const FRHIBufferUAVCreateInfo& CreateInfo);

	// Finds a SRV matching the descriptor in the cache or creates a new one and updates the cache.
	FRHIShaderResourceView* GetOrCreateSRV(FRHIBuffer* Buffer, const FRHIBufferSRVCreateInfo& CreateInfo);

	// Sets the debug name of the RHI view resources.
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	void SetDebugName(const TCHAR* DebugName);
#else
	void SetDebugName(const TCHAR* DebugName) {}
#endif

private:
	TArray<TPair<FRHIBufferUAVCreateInfo, FUnorderedAccessViewRHIRef>, TInlineAllocator<1>> UAVs;
	TArray<TPair<FRHIBufferSRVCreateInfo, FShaderResourceViewRHIRef>, TInlineAllocator<1>> SRVs;
};