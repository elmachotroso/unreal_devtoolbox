// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
D3D12Resources.cpp: D3D RHI utility implementation.
=============================================================================*/

#include "D3D12RHIPrivate.h"
#include "EngineModule.h"
#include "HAL/LowLevelMemTracker.h"

#if INTEL_EXTENSIONS
	#define INTC_IGDEXT_D3D12 1

	THIRD_PARTY_INCLUDES_START
	#include "igdext.h"
	THIRD_PARTY_INCLUDES_END
#endif

D3D12RHI_API int32 GD3D12AsyncDeferredDeletion = ASYNC_DEFERRED_DELETION;

static FAutoConsoleVariableRef CVarAsyncDeferredDeletion(
	TEXT("D3D12.AsyncDeferredDeletion"),
	GD3D12AsyncDeferredDeletion,
	TEXT("Controls whether D3D12 resources will be released on a separate thread (default = ")
#if ASYNC_DEFERRED_DELETION
	TEXT("on")
#else
	TEXT("off")
#endif
	TEXT(")."),
	ECVF_ReadOnly
);

/////////////////////////////////////////////////////////////////////
//	FD3D12 Deferred Deletion Queue
/////////////////////////////////////////////////////////////////////

FD3D12DeferredDeletionQueue::FD3D12DeferredDeletionQueue(FD3D12Adapter* InParent) :
	FD3D12AdapterChild(InParent) {}

FD3D12DeferredDeletionQueue::~FD3D12DeferredDeletionQueue()
{
	FAsyncTask<FD3D12AsyncDeletionWorker>* DeleteTask = nullptr;
	while (DeleteTasks.Peek(DeleteTask))
	{
		DeleteTasks.Dequeue(DeleteTask);
		DeleteTask->EnsureCompletion(true);
		delete(DeleteTask);
	}
}

void FD3D12DeferredDeletionQueue::EnqueueResource(FD3D12Resource* pResource, FFenceList&& FenceList)
{
	check(pResource->ShouldDeferDelete());

	// Useful message for identifying when resources are released on the rendering thread.
	//UE_CLOG(IsInActualRenderingThread(), LogD3D12RHI, Display, TEXT("Rendering Thread: Deleting %#016llx when done with frame fence %llu"), pResource, Fence->GetCurrentFence());

	FencedObjectType FencedObject;
	FencedObject.RHIObject  = pResource;
	FencedObject.FenceList  = MoveTemp(FenceList);
	FencedObject.Type       = EObjectType::RHI;
	DeferredReleaseQueue.Enqueue(FencedObject);
}

void FD3D12DeferredDeletionQueue::EnqueueResource(ID3D12Object* pResource, FD3D12Fence* Fence)
{
	// Useful message for identifying when resources are released on the rendering thread.
	//UE_CLOG(IsInActualRenderingThread(), LogD3D12RHI, Display, TEXT("Rendering Thread: Deleting %#016llx when done with frame fence %llu"), pResource, Fence->GetCurrentFence());

	FencedObjectType FencedObject;
	FencedObject.D3DObject  = pResource;
	FencedObject.FenceList.Emplace(Fence, Fence->GetCurrentFence());
	FencedObject.Type       = EObjectType::D3D;
	DeferredReleaseQueue.Enqueue(FencedObject);
}

bool FD3D12DeferredDeletionQueue::ReleaseResources(bool bDeleteImmediately, bool bIsShutDown)
{
	FScopeLock ScopeLock(&DeleteTaskCS);

	FD3D12Adapter* Adapter = GetParentAdapter();

	if (GD3D12AsyncDeferredDeletion)
	{
		if (bDeleteImmediately)
		{
			// Wait for all deferred delete tasks to finish
			FAsyncTask<FD3D12AsyncDeletionWorker>* DeleteTask = nullptr;
			while (DeleteTasks.Peek(DeleteTask))
			{
				DeleteTasks.Dequeue(DeleteTask);
				DeleteTask->EnsureCompletion(true);
				delete(DeleteTask);
			}

			// current deferred release queue will be freed via non async deferred deletion code path below
		}
		else
		{
			// Clean up all previously finished delete tasks
			FAsyncTask<FD3D12AsyncDeletionWorker>* DeleteTask = nullptr;
			while (DeleteTasks.Peek(DeleteTask) && DeleteTask->IsDone())
			{
				DeleteTasks.Dequeue(DeleteTask);
				delete(DeleteTask);
			}

			// Create new delete task, which will only collect resources in the constructor for which the fence is complete, not the whole list!
			DeleteTask = new FAsyncTask<FD3D12AsyncDeletionWorker>(Adapter, &DeferredReleaseQueue);

			DeleteTask->StartBackgroundTask();
			DeleteTasks.Enqueue(DeleteTask);

			// Deferred release queue is not empty yet
			return false;
		}
	}

	FencedObjectType FenceObject;

	if (bIsShutDown)
	{
		// FORT-236194 - Output what we are releasing on exit to catch a crash on Release()
		UE_LOG(LogD3D12RHI, Display, TEXT("D3D12 ReleaseResources: %u items to release"), DeferredReleaseQueue.GetSize());

		while (DeferredReleaseQueue.Dequeue(FenceObject))
		{
			if (FenceObject.Type == EObjectType::RHI)
			{
				D3D12_RESOURCE_DESC Desc = FenceObject.RHIObject->GetDesc();
				FString Name = FenceObject.RHIObject->GetName().ToString();
				UE_LOG(LogD3D12RHI, Display, TEXT("D3D12 ReleaseResources: \"%s\", %llu x %u x %u, Mips: %u, Format: 0x%X, Flags: 0x%X"), *Name, Desc.Width, Desc.Height, Desc.DepthOrArraySize, Desc.MipLevels, Desc.Format, Desc.Flags);

				uint32 RefCount = FenceObject.RHIObject->Release();
				if (RefCount)
				{
					UE_LOG(LogD3D12RHI, Display, TEXT("RefCount was %u"), RefCount);
				}
			}
			else
			{
				UE_LOG(LogD3D12RHI, Display, TEXT("D3D12 ReleaseResources: 0x%llX"), FenceObject.D3DObject);

				uint32 RefCount = FenceObject.D3DObject->Release();
				if (RefCount)
				{
					UE_LOG(LogD3D12RHI, Display, TEXT("RefCount was %u"), RefCount);
				}
			}
		}
	}
	else
	{
		struct FDequeueFenceObject
		{
			bool operator() (FencedObjectType FenceObject) const
			{
				for (auto& FencePair : FenceObject.FenceList)
				{
					if (!FencePair.Key->IsFenceComplete(FencePair.Value))
					{
						return false;
					}
				}
				return true;
			}
		};

		while (DeferredReleaseQueue.Dequeue(FenceObject, FDequeueFenceObject()))
		{
			if (FenceObject.Type == EObjectType::RHI)
			{
				FenceObject.RHIObject->Release();
			}
			else
			{
				FenceObject.D3DObject->Release();
			}
		}
	}

	return DeferredReleaseQueue.IsEmpty();
}

FD3D12DeferredDeletionQueue::FD3D12AsyncDeletionWorker::FD3D12AsyncDeletionWorker(FD3D12Adapter* Adapter, FThreadsafeQueue<FencedObjectType>* DeletionQueue)
	: FD3D12AdapterChild(Adapter)
{
	struct FDequeueFenceObject
	{
		bool operator() (FencedObjectType FenceObject) const
		{
			for (auto& FencePair : FenceObject.FenceList)
			{
				if (!FencePair.Key->IsFenceComplete(FencePair.Value))
				{
					return false;
				}
			}
			return true;
		}
	};

	DeletionQueue->BatchDequeue(&Queue, FDequeueFenceObject(), 4096);
}

void FD3D12DeferredDeletionQueue::FD3D12AsyncDeletionWorker::DoWork()
{
	FencedObjectType ResourceToDelete;

	while (Queue.Dequeue(ResourceToDelete))
	{
		if (ResourceToDelete.Type == EObjectType::RHI)
		{
			// This should be a final release.
			check(ResourceToDelete.RHIObject->GetRefCount() == 1);
			ResourceToDelete.RHIObject->Release();
		}
		else
		{
			ResourceToDelete.D3DObject->Release();
		}
	}
}


/////////////////////////////////////////////////////////////////////
//	ID3D12ResourceAllocator
/////////////////////////////////////////////////////////////////////


void ID3D12ResourceAllocator::AllocateTexture(uint32 GPUIndex, D3D12_HEAP_TYPE InHeapType, const FD3D12ResourceDesc& InDesc, EPixelFormat InUEFormat, ED3D12ResourceStateMode InResourceStateMode,
	D3D12_RESOURCE_STATES InCreateState, const D3D12_CLEAR_VALUE* InClearValue, const TCHAR* InName, FD3D12ResourceLocation& ResourceLocation)
{
	// Check if texture can be 4K aligned
	FD3D12ResourceDesc Desc = InDesc;
	bool b4KAligment = TextureCanBe4KAligned(Desc, InUEFormat);
	Desc.Alignment = b4KAligment ? D3D12_SMALL_RESOURCE_PLACEMENT_ALIGNMENT : D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT;

	// Get the size and alignment for the allocation
	D3D12_RESOURCE_ALLOCATION_INFO Info = FD3D12DynamicRHI::GetD3DRHI()->GetAdapter().GetDevice(0)->GetResourceAllocationInfo(Desc);
	AllocateResource(GPUIndex, InHeapType, Desc, Info.SizeInBytes, Info.Alignment, InResourceStateMode, InCreateState, InClearValue, InName, ResourceLocation);
}


/////////////////////////////////////////////////////////////////////
//	FD3D12 Resource
/////////////////////////////////////////////////////////////////////

#if UE_BUILD_DEBUG
int64 FD3D12Resource::TotalResourceCount = 0;
int64 FD3D12Resource::NoStateTrackingResourceCount = 0;
#endif

FD3D12Resource::FD3D12Resource(FD3D12Device* ParentDevice,
	FRHIGPUMask VisibleNodes,
	ID3D12Resource* InResource,
	D3D12_RESOURCE_STATES InInitialState,
	const FD3D12ResourceDesc& InDesc,
	FD3D12Heap* InHeap,
	D3D12_HEAP_TYPE InHeapType) : 
	FD3D12Resource(ParentDevice, VisibleNodes, InResource, InInitialState, ED3D12ResourceStateMode::Default, D3D12_RESOURCE_STATE_TBD, InDesc, InHeap, InHeapType)
{
}

FD3D12Resource::FD3D12Resource(FD3D12Device* ParentDevice,
	FRHIGPUMask VisibleNodes,
	ID3D12Resource* InResource,
	D3D12_RESOURCE_STATES InInitialState,
	ED3D12ResourceStateMode InResourceStateMode,
	D3D12_RESOURCE_STATES InDefaultResourceState,
	const FD3D12ResourceDesc& InDesc,
	FD3D12Heap* InHeap,
	D3D12_HEAP_TYPE InHeapType)
	: FD3D12DeviceChild(ParentDevice)
	, FD3D12MultiNodeGPUObject(ParentDevice->GetGPUMask(), VisibleNodes)
	, Resource(InResource)
	, Heap(InHeap)
	, Desc(InDesc)
	, HeapType(InHeapType)
	, PlaneCount(::GetPlaneCount(Desc.Format))
	, bRequiresResourceStateTracking(true)
	, bDepthStencil(false)
	, bDeferDelete(true)
	, bBackBuffer(false)
{
#if UE_BUILD_DEBUG
	FPlatformAtomics::InterlockedIncrement(&TotalResourceCount);
#endif

	// On Windows it's sadly enough not possible to get the GPU virtual address from the resource directly
	if (Resource
#if PLATFORM_WINDOWS
		&& Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER
#endif
		)
	{
		GPUVirtualAddress = Resource->GetGPUVirtualAddress();
	}

	InitalizeResourceState(InInitialState, InResourceStateMode, InDefaultResourceState);

#if NV_AFTERMATH
	if (GDX12NVAfterMathTrackResources)
	{
		GFSDK_Aftermath_DX12_RegisterResource(InResource, &AftermathHandle);
	}
#endif
}

FD3D12Resource::~FD3D12Resource()
{
	if (D3DX12Residency::IsInitialized(ResidencyHandle))
	{
		D3DX12Residency::EndTrackingObject(GetParentDevice()->GetResidencyManager(), ResidencyHandle);
	}

#if NV_AFTERMATH
	if (GDX12NVAfterMathTrackResources)
	{
		GFSDK_Aftermath_DX12_UnregisterResource(AftermathHandle);
	}
#endif

	if (bBackBuffer)
	{
		// Don't make the windows association call and release back buffer at the same time (see notes on critical section)
		FScopeLock Lock(&FD3D12Viewport::DXGIBackBufferLock);
		bBackBuffer = false;
		Resource.SafeRelease();
	}
}

ID3D12Pageable* FD3D12Resource::GetPageable()
{
	if (IsPlacedResource())
	{
		return (ID3D12Pageable*)(GetHeap()->GetHeap());
	}
	else
	{
		return (ID3D12Pageable*)GetResource();
	}
}

void FD3D12Resource::StartTrackingForResidency()
{
#if ENABLE_RESIDENCY_MANAGEMENT
	check(IsGPUOnly(HeapType));	// This is checked at a higher level before calling this function.
	check(D3DX12Residency::IsInitialized(ResidencyHandle) == false);
	const D3D12_RESOURCE_DESC ResourceDesc = Resource->GetDesc();
	const D3D12_RESOURCE_ALLOCATION_INFO Info = GetParentDevice()->GetDevice()->GetResourceAllocationInfo(0, 1, &ResourceDesc);

	D3DX12Residency::Initialize(ResidencyHandle, Resource.GetReference(), Info.SizeInBytes);
	D3DX12Residency::BeginTrackingObject(GetParentDevice()->GetResidencyManager(), ResidencyHandle);
#endif
}

void FD3D12Resource::UpdateResidency(FD3D12CommandListHandle& CommandList)
{
#if ENABLE_RESIDENCY_MANAGEMENT
	if (IsPlacedResource())
	{
		Heap->UpdateResidency(CommandList);
	}
	else if (D3DX12Residency::IsInitialized(ResidencyHandle))
	{
		check(Heap == nullptr);
		D3DX12Residency::Insert(CommandList.GetResidencySet(), ResidencyHandle);
	}
#endif
}

void FD3D12Resource::DeferDelete()
{
	FD3D12Adapter* Adapter = GetParentDevice()->GetParentAdapter();

	// Upload heaps such as texture lock data can be referenced by multiple GPUs so we
	// must wait for all of them to finish before releasing.
	FD3D12DeferredDeletionQueue::FFenceList FenceList;
	if (HeapType == D3D12_HEAP_TYPE_UPLOAD)
	{
		for (uint32 GPUIndex : FRHIGPUMask::All())
		{
			FD3D12Fence* Fence = &Adapter->GetDevice(GPUIndex)->GetCommandListManager().GetFence();
			FenceList.Emplace(Fence, Fence->GetCurrentFence());
		}
	}
	else
	{
		FD3D12Fence* Fence = &GetParentDevice()->GetCommandListManager().GetFence();
		FenceList.Emplace(Fence, Fence->GetCurrentFence());
	}

	Adapter->GetDeferredDeletionQueue().EnqueueResource(this, MoveTemp(FenceList));
}

/////////////////////////////////////////////////////////////////////
//	FD3D12 Heap
/////////////////////////////////////////////////////////////////////

FD3D12Heap::FD3D12Heap(FD3D12Device* Parent, FRHIGPUMask VisibleNodes) :
	FD3D12DeviceChild(Parent),
	FD3D12MultiNodeGPUObject(Parent->GetGPUMask(), VisibleNodes),
	ResidencyHandle()
{
}

FD3D12Heap::~FD3D12Heap()
{
#if TRACK_RESOURCE_ALLOCATIONS
	FD3D12Adapter* Adapter = GetParentDevice()->GetParentAdapter();
	if (GPUVirtualAddress != 0 && bTrack)
	{
		Adapter->ReleaseTrackedHeap(this);
	}
#endif // TRACK_RESOURCE_ALLOCATIONS

#if ENABLE_RESIDENCY_MANAGEMENT
	if (D3DX12Residency::IsInitialized(ResidencyHandle))
	{
		D3DX12Residency::EndTrackingObject(GetParentDevice()->GetResidencyManager(), ResidencyHandle);
		ResidencyHandle = {};
	}
#endif // ENABLE_RESIDENCY_MANAGEMENT

	// Release actual d3d object
	Heap.SafeRelease();
}

void FD3D12Heap::SetHeap(ID3D12Heap* HeapIn, const TCHAR* const InName, bool bInTrack, bool bForceGetGPUAddress)
{
	*Heap.GetInitReference() = HeapIn; 
	bTrack = bInTrack;
	HeapName = InName;
	HeapDesc = Heap->GetDesc();

	SetName(HeapIn, InName);

	// Create a buffer placed resource on the heap to extract the gpu virtual address
	// if we are tracking all allocations
	FD3D12Adapter* Adapter = GetParentDevice()->GetParentAdapter();	
	if ((bForceGetGPUAddress || Adapter->IsTrackingAllAllocations())
		&& !(HeapDesc.Flags & D3D12_HEAP_FLAG_DENY_BUFFERS)
		&& HeapDesc.Properties.Type == D3D12_HEAP_TYPE_DEFAULT)
	{
		uint64 HeapSize = HeapDesc.SizeInBytes;
		TRefCountPtr<ID3D12Resource> TempResource;
		const D3D12_RESOURCE_DESC BufDesc = CD3DX12_RESOURCE_DESC::Buffer(HeapSize, D3D12_RESOURCE_FLAG_NONE);
		VERIFYD3D12RESULT(Adapter->GetD3DDevice()->CreatePlacedResource(Heap, 0, &BufDesc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(TempResource.GetInitReference())));
		GPUVirtualAddress = TempResource->GetGPUVirtualAddress();
				
#if TRACK_RESOURCE_ALLOCATIONS
		if (bTrack)
		{
			Adapter->TrackHeapAllocation(this);
		}
#endif
	}
}

void FD3D12Heap::UpdateResidency(FD3D12CommandListHandle& CommandList)
{
#if ENABLE_RESIDENCY_MANAGEMENT
	if (D3DX12Residency::IsInitialized(ResidencyHandle))
	{
		D3DX12Residency::Insert(CommandList.GetResidencySet(), ResidencyHandle);
	}
#endif
}

void FD3D12Heap::BeginTrackingResidency(uint64 Size)
{
#if ENABLE_RESIDENCY_MANAGEMENT
	D3DX12Residency::Initialize(ResidencyHandle, Heap.GetReference(), Size);
	D3DX12Residency::BeginTrackingObject(GetParentDevice()->GetResidencyManager(), ResidencyHandle);
#endif
}

/////////////////////////////////////////////////////////////////////
//	FD3D12 Adapter
/////////////////////////////////////////////////////////////////////

HRESULT FD3D12Adapter::CreateCommittedResource(const FD3D12ResourceDesc& InDesc, FRHIGPUMask CreationNode, const D3D12_HEAP_PROPERTIES& HeapProps, D3D12_RESOURCE_STATES InInitialState,
	ED3D12ResourceStateMode InResourceStateMode, D3D12_RESOURCE_STATES InDefaultState, const D3D12_CLEAR_VALUE* ClearValue, FD3D12Resource** ppOutResource, const TCHAR* Name, bool bVerifyHResult)
{
	if (!ppOutResource)
	{
		return E_POINTER;
	}

	TRACE_CPUPROFILER_EVENT_SCOPE(CreateCommittedResource);

	LLM_PLATFORM_SCOPE(ELLMTag::GraphicsPlatform);

	TRefCountPtr<ID3D12Resource> pResource;
	const bool bRequiresInitialization = (InDesc.Flags & (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) != 0;
	D3D12_HEAP_FLAGS HeapFlags = (bHeapNotZeroedSupported && !bRequiresInitialization) ? FD3D12_HEAP_FLAG_CREATE_NOT_ZEROED : D3D12_HEAP_FLAG_NONE;
	if (InDesc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)
	{
		HeapFlags |= D3D12_HEAP_FLAG_SHARED;
	}

	FD3D12ResourceDesc LocalDesc = InDesc;

#if D3D12_RHI_RAYTRACING
	if (InDefaultState == D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE)
	{
		LocalDesc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
	}
#endif // D3D12_RHI_RAYTRACING

	HRESULT hr = S_OK;
#if INTEL_EXTENSIONS
	if (InDesc.bRequires64BitAtomicSupport && IsRHIDeviceIntel() && GRHISupportsAtomicUInt64)
	{
		INTC_D3D12_RESOURCE_DESC_0001 IntelLocalDesc{};
		IntelLocalDesc.pD3D12Desc = &LocalDesc;
		IntelLocalDesc.EmulatedTyped64bitAtomics = true;

		hr = INTC_D3D12_CreateCommittedResource(FD3D12DynamicRHI::GetD3DRHI()->GetIntelExtensionContext(), &HeapProps, HeapFlags, &IntelLocalDesc, InInitialState, ClearValue, IID_PPV_ARGS(pResource.GetInitReference()));
	}
	else
#endif
	{
		hr = RootDevice->CreateCommittedResource(&HeapProps, HeapFlags, &LocalDesc, InInitialState, ClearValue, IID_PPV_ARGS(pResource.GetInitReference()));
	}
	if (SUCCEEDED(hr))
	{
		// Set the output pointer
		*ppOutResource = new FD3D12Resource(GetDevice(CreationNode.ToIndex()), CreationNode, pResource, InInitialState, InResourceStateMode, InDefaultState, InDesc, nullptr, HeapProps.Type);
		(*ppOutResource)->AddRef();

		// Set a default name (can override later).
		SetName(*ppOutResource, Name);

		// Only track resources that cannot be accessed on the CPU.
		if (IsGPUOnly(HeapProps.Type, &HeapProps))
		{
			(*ppOutResource)->StartTrackingForResidency();
		}
	}
	else	
	{
		UE_LOG(LogD3D12RHI, Display, TEXT("D3D12 CreateCommittedResource failed with params:\n\tHeap Type: %d\n\tHeap Flags: %d\n\tResource Dimension: %d\n\tResource Width: %d\n\tResource Height: %d\n\tFormat: %d\n\tResource Flags: %d"),
			HeapProps.Type, HeapFlags, LocalDesc.Dimension, LocalDesc.Width, LocalDesc.Height, LocalDesc.PixelFormat, LocalDesc.Flags);

		if (bVerifyHResult)
		{
			VERIFYD3D12RESULT_EX(hr, RootDevice);
		}
	}

	return hr;
}

HRESULT FD3D12Adapter::CreatePlacedResource(const FD3D12ResourceDesc& InDesc, FD3D12Heap* BackingHeap, uint64 HeapOffset, D3D12_RESOURCE_STATES InInitialState, ED3D12ResourceStateMode InResourceStateMode,
	D3D12_RESOURCE_STATES InDefaultState, const D3D12_CLEAR_VALUE* ClearValue, FD3D12Resource** ppOutResource, const TCHAR* Name, bool bVerifyHResult)
{
	if (!ppOutResource)
	{
		return E_POINTER;
	}

	ID3D12Heap* Heap = BackingHeap->GetHeap();

	TRefCountPtr<ID3D12Resource> pResource;
	HRESULT hr = S_OK;
#if INTEL_EXTENSIONS
	if (InDesc.bRequires64BitAtomicSupport && IsRHIDeviceIntel() && GRHISupportsAtomicUInt64)
	{
		FD3D12ResourceDesc LocalDesc = InDesc;
		INTC_D3D12_RESOURCE_DESC_0001 IntelLocalDesc{};
		IntelLocalDesc.pD3D12Desc = &LocalDesc;
		IntelLocalDesc.EmulatedTyped64bitAtomics = true;

		hr = INTC_D3D12_CreatePlacedResource(FD3D12DynamicRHI::GetD3DRHI()->GetIntelExtensionContext(), Heap, HeapOffset, &IntelLocalDesc, InInitialState, ClearValue, IID_PPV_ARGS(pResource.GetInitReference()));
	}
	else
#endif
	{
		hr = RootDevice->CreatePlacedResource(Heap, HeapOffset, &InDesc, InInitialState, ClearValue, IID_PPV_ARGS(pResource.GetInitReference()));
	}
	if (SUCCEEDED(hr))
	{
		FD3D12Device* Device = BackingHeap->GetParentDevice();
		const D3D12_HEAP_DESC HeapDesc = Heap->GetDesc();

		// Set the output pointer
		*ppOutResource = new FD3D12Resource(Device,
			Device->GetVisibilityMask(),
			pResource,
			InInitialState,
			InResourceStateMode,
			InDefaultState,
			InDesc,
			BackingHeap,
			HeapDesc.Properties.Type);

#if PLATFORM_WINDOWS
		if (IsTrackingAllAllocations() && BackingHeap->GetHeapDesc().Properties.Type == D3D12_HEAP_TYPE_DEFAULT)
		{
			// Manually set the GPU virtual address from the heap gpu virtual address & offset
			if (InDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
			{
				check(BackingHeap->GetGPUVirtualAddress() != 0);
				(*ppOutResource)->SetGPUVirtualAddress(BackingHeap->GetGPUVirtualAddress() + HeapOffset);
			}
			else
			{
				check((*ppOutResource)->GetGPUVirtualAddress() != 0);
				check((*ppOutResource)->GetGPUVirtualAddress() == BackingHeap->GetGPUVirtualAddress() + HeapOffset);
			}
		}
#endif		

		// Set a default name (can override later).
		SetName(*ppOutResource, Name);

		(*ppOutResource)->AddRef();
	}
	else
	{
		UE_LOG(LogD3D12RHI, Display, TEXT("D3D12 CreatePlacedResource failed with params:\n\tHeap Type: %d\n\tHeap Flags: %d\n\tResource Dimension: %d\n\tResource Width: %d\n\tResource Height: %d\n\tHeightFormat: %d\n\tResource Flags: %d"),
			BackingHeap->GetHeapDesc().Properties.Type, BackingHeap->GetHeapDesc().Flags, InDesc.Dimension, InDesc.Width, InDesc.Height, InDesc.PixelFormat, InDesc.Flags);

		if (bVerifyHResult)
		{
			VERIFYD3D12RESULT_EX(hr, RootDevice);
		}
	}

	return hr;
}

HRESULT FD3D12Adapter::CreateBuffer(D3D12_HEAP_TYPE HeapType, FRHIGPUMask CreationNode, FRHIGPUMask VisibleNodes, uint64 HeapSize, FD3D12Resource** ppOutResource, const TCHAR* Name, D3D12_RESOURCE_FLAGS Flags)
{
	const D3D12_HEAP_PROPERTIES HeapProps = CD3DX12_HEAP_PROPERTIES(HeapType, CreationNode.GetNative(), VisibleNodes.GetNative());
	const D3D12_RESOURCE_STATES InitialState = DetermineInitialResourceState(HeapProps.Type, &HeapProps);
	return CreateBuffer(HeapProps, CreationNode, InitialState, ED3D12ResourceStateMode::Default, D3D12_RESOURCE_STATE_TBD, HeapSize, ppOutResource, Name, Flags);
}

HRESULT FD3D12Adapter::CreateBuffer(D3D12_HEAP_TYPE HeapType, FRHIGPUMask CreationNode, FRHIGPUMask VisibleNodes, D3D12_RESOURCE_STATES InitialState, ED3D12ResourceStateMode ResourceStateMode, uint64 HeapSize, FD3D12Resource** ppOutResource, const TCHAR* Name, D3D12_RESOURCE_FLAGS Flags)
{
	const D3D12_HEAP_PROPERTIES HeapProps = CD3DX12_HEAP_PROPERTIES(HeapType, CreationNode.GetNative(), VisibleNodes.GetNative());
	return CreateBuffer(HeapProps, CreationNode, InitialState, ResourceStateMode, InitialState, HeapSize, ppOutResource, Name, Flags);
}

HRESULT FD3D12Adapter::CreateBuffer(const D3D12_HEAP_PROPERTIES& HeapProps,
	FRHIGPUMask CreationNode,
	D3D12_RESOURCE_STATES InitialState,
	ED3D12ResourceStateMode ResourceStateMode,
	D3D12_RESOURCE_STATES InDefaultState,
	uint64 HeapSize,
	FD3D12Resource** ppOutResource,
	const TCHAR* Name,
	D3D12_RESOURCE_FLAGS Flags)
{
	if (!ppOutResource)
	{
		return E_POINTER;
	}

	const D3D12_RESOURCE_DESC BufDesc = CD3DX12_RESOURCE_DESC::Buffer(HeapSize, Flags);
	return CreateCommittedResource(BufDesc,
		CreationNode,
		HeapProps,
		InitialState,
		ResourceStateMode,
		InDefaultState,
		nullptr,
		ppOutResource, Name);
}

/////////////////////////////////////////////////////////////////////
//	FD3D12 Resource Location
/////////////////////////////////////////////////////////////////////

FD3D12ResourceLocation::FD3D12ResourceLocation(FD3D12Device* Parent)
	: FD3D12DeviceChild(Parent)
	, Allocator(nullptr)
{
	FMemory::Memzero(AllocatorData);
}

FD3D12ResourceLocation::~FD3D12ResourceLocation()
{
	ReleaseResource();
}

void FD3D12ResourceLocation::Clear()
{
	InternalClear<true>();
}

template void FD3D12ResourceLocation::InternalClear<false>();
template void FD3D12ResourceLocation::InternalClear<true>();

template<bool bReleaseResource>
void FD3D12ResourceLocation::InternalClear()
{
	if (bReleaseResource)
	{
		ReleaseResource();
	}

	// Reset members
	Type = ResourceLocationType::eUndefined;
	UnderlyingResource = nullptr;
	MappedBaseAddress = nullptr;
	GPUVirtualAddress = 0;
	ResidencyHandle = nullptr;
	Size = 0;
	OffsetFromBaseOfResource = 0;
	FMemory::Memzero(AllocatorData);

	Allocator = nullptr;
	AllocatorType = AT_Unknown;
}

void FD3D12ResourceLocation::TransferOwnership(FD3D12ResourceLocation& Destination, FD3D12ResourceLocation& Source)
{
	// Clear out the destination
	Destination.Clear();

	FMemory::Memmove(&Destination, &Source, sizeof(FD3D12ResourceLocation));

	if (Source.GetAllocatorType() == FD3D12ResourceLocation::AT_Pool)
	{
		Source.GetPoolAllocator()->TransferOwnership(Source, Destination);
	}

	// update tracked allocation
#if !PLATFORM_WINDOWS && ENABLE_LOW_LEVEL_MEM_TRACKER
	if (Source.GetType() == ResourceLocationType::eSubAllocation && Source.AllocatorType != AT_SegList)
	{
		FLowLevelMemTracker::Get().OnLowLevelAllocMoved(ELLMTracker::Default, Destination.GetAddressForLLMTracking(), Source.GetAddressForLLMTracking());
	}
#endif

	// Destroy the source but don't invoke any resource destruction
	Source.InternalClear<false>();
}

void FD3D12ResourceLocation::Swap(FD3D12ResourceLocation& Other)
{
	// TODO: Probably shouldn't manually track suballocations. It's error-prone and inaccurate
#if !PLATFORM_WINDOWS && ENABLE_LOW_LEVEL_MEM_TRACKER
	const bool bRequiresManualTracking = GetType() == ResourceLocationType::eSubAllocation && AllocatorType != AT_SegList;
	const bool bOtherRequiresManualTracking = Other.GetType() == ResourceLocationType::eSubAllocation && Other.AllocatorType != AT_SegList;

	if (bRequiresManualTracking)
	{
		FLowLevelMemTracker::Get().OnLowLevelFree(ELLMTracker::Default, GetAddressForLLMTracking());
	}
	if (bOtherRequiresManualTracking)
	{
		FLowLevelMemTracker::Get().OnLowLevelAllocMoved(ELLMTracker::Default, GetAddressForLLMTracking(), Other.GetAddressForLLMTracking());
	}
	if (bRequiresManualTracking)
	{
		FLowLevelMemTracker::Get().OnLowLevelAlloc(ELLMTracker::Default, Other.GetAddressForLLMTracking(), GetSize());
	}
#endif

	if (Other.GetAllocatorType() == FD3D12ResourceLocation::AT_Pool)
	{
		check(GetAllocatorType() != FD3D12ResourceLocation::AT_Pool);
		
		// Cache the allocator data and reset before swap
		FD3D12PoolAllocatorPrivateData& PoolData = Other.GetPoolAllocatorPrivateData();
		FD3D12PoolAllocatorPrivateData TmpPoolData = PoolData;
		PoolData.Init();

		// Perform swap
		::Swap(*this, Other);

		// Restore allocator data and perform pool aware swap
		PoolData = TmpPoolData;

		// Reset the tmp pool data again, because it's not needed anymore - all data copied over
		TmpPoolData.Init();

		Other.SetPoolAllocator(GetPoolAllocator());
		GetPoolAllocator()->TransferOwnership(Other, *this);
	}
	else
	{
		::Swap(*this, Other);
	}
}

void FD3D12ResourceLocation::Alias(FD3D12ResourceLocation & Destination, FD3D12ResourceLocation & Source)
{
	// Should not be linked list allocated - otherwise internal linked list data needs to be updated as well in a threadsafe way
	check(Source.GetAllocatorType() != FD3D12ResourceLocation::AT_Pool);

	check(Source.GetResource() != nullptr);
	Destination.Clear();

	FMemory::Memmove(&Destination, &Source, sizeof(FD3D12ResourceLocation));
	Destination.SetType(ResourceLocationType::eAliased);
	Source.SetType(ResourceLocationType::eAliased);

	// Addref the source as another resource location references it
	Source.GetResource()->AddRef();
}

void FD3D12ResourceLocation::ReferenceNode(FD3D12Device* DestinationDevice, FD3D12ResourceLocation& Destination, FD3D12ResourceLocation& Source)
{
	check(Source.GetResource() != nullptr);
	Destination.Clear();

	FMemory::Memmove(&Destination, &Source, sizeof(FD3D12ResourceLocation));
	Destination.SetType(ResourceLocationType::eNodeReference);

	Destination.Parent = DestinationDevice;

	// Addref the source as another resource location references it
	Source.GetResource()->AddRef();

	if (Source.GetAllocatorType() == FD3D12ResourceLocation::AT_Pool)
	{
		Source.GetPoolAllocatorPrivateData().PoolData.AddAlias(
			&Destination.GetPoolAllocatorPrivateData().PoolData);
	}
}

void FD3D12ResourceLocation::ReleaseResource()
{
#if TRACK_RESOURCE_ALLOCATIONS
	if (IsTransient())
	{
		FD3D12Adapter* Adapter = GetParentDevice()->GetParentAdapter();
		if (Adapter->IsTrackingAllAllocations())
		{
			bool bDefragFree = false;
			Adapter->ReleaseTrackedAllocationData(this, bDefragFree);
		}
	}
#endif

	switch (Type)
	{
	case ResourceLocationType::eStandAlone:
	{
		bool bIncrement = false;
		UpdateStandAloneStats(bIncrement);

		// Multi-GPU support : because of references, several GPU nodes can refrence the same stand-alone resource.
		check(UnderlyingResource->GetRefCount() == 1 || GNumExplicitGPUsForRendering > 1);
		
		if (UnderlyingResource->ShouldDeferDelete())
		{
			UnderlyingResource->DeferDelete();
		}
		else
		{
			UnderlyingResource->Release();
		}
		break;
	}
	case ResourceLocationType::eSubAllocation:
	{
		check(Allocator != nullptr);
		if (AllocatorType == AT_SegList)
		{
			SegListAllocator->Deallocate(
				GetResource(),
				GetSegListAllocatorPrivateData().Offset,
				GetSize());
		}
		else if (AllocatorType == AT_Pool)
		{
			// Unlink any aliases -- the contents of aliases are cleaned up separately elsewhere via iteration over
			// the FD3D12LinkedAdapterObject.
			for (FRHIPoolAllocationData* Alias = GetPoolAllocatorPrivateData().PoolData.GetFirstAlias();
				 Alias;
				 Alias = GetPoolAllocatorPrivateData().PoolData.GetFirstAlias())
			{
				Alias->RemoveAlias();
			}

			PoolAllocator->DeallocateResource(*this);
		}
		else
		{
			Allocator->Deallocate(*this);
		}
		break;
	}
	case ResourceLocationType::eNodeReference:
	case ResourceLocationType::eAliased:
	{
		if (GetAllocatorType() == FD3D12ResourceLocation::AT_Pool)
		{
			GetPoolAllocatorPrivateData().PoolData.RemoveAlias();
		}

		if (UnderlyingResource->ShouldDeferDelete() && UnderlyingResource->GetRefCount() == 1)
		{
			UnderlyingResource->DeferDelete();
		}
		else
		{
			UnderlyingResource->Release();
		}
		break;
	}
	case ResourceLocationType::eHeapAliased:
	{
		check(UnderlyingResource->GetRefCount() == 1);
		if (UnderlyingResource->ShouldDeferDelete())
		{
			UnderlyingResource->DeferDelete();
		}
		else
		{
			UnderlyingResource->Release();
		}
		break;
	}
	case ResourceLocationType::eFastAllocation:
	case ResourceLocationType::eUndefined:
	default:
		// Fast allocations are volatile by default so no work needs to be done.
		break;
	}
}

void FD3D12ResourceLocation::UpdateStandAloneStats(bool bIncrement)
{
	if (UnderlyingResource->GetHeapType() == D3D12_HEAP_TYPE_DEFAULT)
	{
		D3D12_RESOURCE_DESC Desc = UnderlyingResource->GetDesc();
		bool bIsBuffer = (Desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
		bool bIsRenderTarget = (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET || Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
		bool bIsUAV = (Desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) > 0;

		// Get the desired size and allocated size for stand alone resources - allocated are very slow anyway
		D3D12_RESOURCE_ALLOCATION_INFO Info = UnderlyingResource->GetParentDevice()->GetDevice()->GetResourceAllocationInfo(0, 1, &Desc);

		int64 SizeInBytes = bIncrement ? Info.SizeInBytes : -(int64)Info.SizeInBytes;
		int32 Count = bIncrement ? 1 : -1;

		if (bIsBuffer)
		{
			if (bIsUAV)
			{
				INC_DWORD_STAT_BY(STAT_D3D12UAVBufferStandAloneCount, Count);
				INC_MEMORY_STAT_BY(STAT_D3D12UAVBufferStandAloneAllocated, SizeInBytes);
			}
			else
			{
				INC_DWORD_STAT_BY(STAT_D3D12BufferStandAloneCount, Count);
				INC_MEMORY_STAT_BY(STAT_D3D12BufferStandAloneAllocated, SizeInBytes);
			}
		}
		else
		{
			if (bIsRenderTarget)
			{
				INC_DWORD_STAT_BY(STAT_D3D12RenderTargetStandAloneCount, Count);
				INC_MEMORY_STAT_BY(STAT_D3D12RenderTargetStandAloneAllocated, SizeInBytes);
			}
			else if (bIsUAV)
			{
				INC_DWORD_STAT_BY(STAT_D3D12UAVTextureStandAloneCount, Count);
				INC_MEMORY_STAT_BY(STAT_D3D12UAVTextureStandAloneAllocated, SizeInBytes);
			}
			else
			{
				INC_DWORD_STAT_BY(STAT_D3D12TextureStandAloneCount, Count);
				INC_MEMORY_STAT_BY(STAT_D3D12TextureStandAloneAllocated, SizeInBytes);
			}
		}

		// Track all committed resource allocations
		if (bIncrement)
		{
			bool bCollectCallstack = true;
			UnderlyingResource->GetParentDevice()->GetParentAdapter()->TrackAllocationData(this, Info.SizeInBytes, bCollectCallstack);
		}
		else
		{
			bool bDefragFree = false;
			UnderlyingResource->GetParentDevice()->GetParentAdapter()->ReleaseTrackedAllocationData(this, bDefragFree);
		}
	}
}

void FD3D12ResourceLocation::SetResource(FD3D12Resource* Value)
{
	check(UnderlyingResource == nullptr);
	check(ResidencyHandle == nullptr);

	GPUVirtualAddress = Value->GetGPUVirtualAddress();

	UnderlyingResource = Value;
	ResidencyHandle = UnderlyingResource->GetResidencyHandle();
}


void FD3D12ResourceLocation::AsStandAlone(FD3D12Resource* Resource, uint64 InSize, bool bInIsTransient, const D3D12_HEAP_PROPERTIES* CustomHeapProperties)
{
	SetType(FD3D12ResourceLocation::ResourceLocationType::eStandAlone);
	SetResource(Resource);
	SetSize(InSize);

	if (IsCPUAccessible(Resource->GetHeapType(), CustomHeapProperties))
	{
		D3D12_RANGE range = { 0, IsCPUWritable(Resource->GetHeapType()) ? 0 : InSize };
		SetMappedBaseAddress(Resource->Map(&range));
	}
	SetGPUVirtualAddress(Resource->GetGPUVirtualAddress());
	SetTransient(bInIsTransient);

	bool bIncrement = true;
	UpdateStandAloneStats(bIncrement);
}


bool FD3D12ResourceLocation::OnAllocationMoved(FRHIPoolAllocationData* InNewData)
{
	// Assume linked list allocated for now - only defragging allocator
	FRHIPoolAllocationData& AllocationData = GetPoolAllocatorPrivateData().PoolData;
	check(InNewData == &AllocationData);
	check(AllocationData.IsAllocated()); // Should be allocated
	check(AllocationData.GetSize() == Size); // Same size
	check(Type == ResourceLocationType::eSubAllocation); // Suballocated
	check(GetMappedBaseAddress() == nullptr); // And VRAM only
	
	// Get the resource and the actual new allocator
	FD3D12Resource* CurrentResource = GetResource();
	FD3D12PoolAllocator* NewAllocator = GetPoolAllocator();

	// If sub allocated and not placed only update the internal data
	if (NewAllocator->GetAllocationStrategy() == EResourceAllocationStrategy::kManualSubAllocation)
	{
		check(!CurrentResource->IsPlacedResource());

		OffsetFromBaseOfResource = AllocationData.GetOffset();
		UnderlyingResource = NewAllocator->GetBackingResource(*this);
	}
	else
	{
		check(CurrentResource->IsPlacedResource());
		check(OffsetFromBaseOfResource == 0);

		// recreate the placed resource (ownership of current resource is already handled during the internal move)
		FD3D12HeapAndOffset HeapAndOffset = NewAllocator->GetBackingHeapAndAllocationOffsetInBytes(*this);

		D3D12_RESOURCE_STATES CreateState;
		ED3D12ResourceStateMode ResourceStateMode;
		if (CurrentResource->RequiresResourceStateTracking())
		{			
			CResourceState& ResourceState = CurrentResource->GetResourceState();
			if (ResourceState.AreAllSubresourcesSame())
			{
				// All resource states the same so we can just create the resource at that state and know everything is fine
				CreateState = ResourceState.GetSubresourceState(0);
			}
			else
			{
				// Force in the readable state when there are different states (or use just state of subresource 0?)
				// ideally restore all subresources in correct state? needed for when not using GUseInternalTransitions anymore because then it needs to match the set state from the engine
				check(GUseInternalTransitions);
				CreateState = CurrentResource->GetReadableState();
			}
			ResourceStateMode = ED3D12ResourceStateMode::MultiState;
		}
		else
		{
			CreateState = CurrentResource->GetDefaultResourceState();
			ResourceStateMode = ED3D12ResourceStateMode::Default;
		}

		// TODO: fix retrieval of ClearValue from owner (currently not a problem because not defragging RT/DS resource yet)
		D3D12_CLEAR_VALUE* ClearValue = nullptr;

		FName Name = CurrentResource->GetName();

		FD3D12Resource* NewResource = nullptr;
		VERIFYD3D12RESULT(CurrentResource->GetParentDevice()->GetParentAdapter()->CreatePlacedResource(CurrentResource->GetDesc(), HeapAndOffset.Heap, HeapAndOffset.Offset, CreateState, ResourceStateMode, D3D12_RESOURCE_STATE_TBD, ClearValue, &NewResource, *Name.ToString()));

		UnderlyingResource = NewResource;
	}

	GPUVirtualAddress = UnderlyingResource->GetGPUVirtualAddress() + OffsetFromBaseOfResource;
	ResidencyHandle = UnderlyingResource->GetResidencyHandle();

	// Refresh aliases
	for (FRHIPoolAllocationData* OtherAlias = AllocationData.GetFirstAlias(); OtherAlias; OtherAlias = OtherAlias->GetNext())
	{
		FD3D12ResourceLocation* OtherResourceLocation = (FD3D12ResourceLocation*)OtherAlias->GetOwner();

		OtherResourceLocation->OffsetFromBaseOfResource = OffsetFromBaseOfResource;
		OtherResourceLocation->UnderlyingResource = UnderlyingResource;
		OtherResourceLocation->GPUVirtualAddress = GPUVirtualAddress;
		OtherResourceLocation->ResidencyHandle = ResidencyHandle;
	}

	// Notify all the dependent resources about the change
	Owner->ResourceRenamed(this);

	return true;
}


void FD3D12ResourceLocation::UnlockPoolData()
{
	if (AllocatorType == AT_Pool)
	{
		GetPoolAllocatorPrivateData().PoolData.Unlock();
	}
}


/////////////////////////////////////////////////////////////////////
//	FD3D12 Resource Barrier Batcher
/////////////////////////////////////////////////////////////////////

// Workaround for FORT-357614. Flickering can be seen unless RTV-to-SRV barriers are separated
static int32 GD3D12SeparateRTV2SRVTransitions = 0;
static FAutoConsoleVariableRef CVarD3D12SeparateRTV2SRVTransitions(
	TEXT("d3d12.SeparateRTV2SRVTranstions"),
	GD3D12SeparateRTV2SRVTransitions,
	TEXT("Whether to submit RTV-to-SRV transition barriers through a separate API call"));

static void RecordResourceBarriersToCommandList(
	ID3D12GraphicsCommandList* pCommandList,
	const D3D12_RESOURCE_BARRIER* Barriers,
	int32 NumBarriers,
	int32 BarrierBatchMax)
{
	if (NumBarriers > BarrierBatchMax)
	{
		while (NumBarriers > 0)
		{
			int32 DispatchNum = FMath::Min(NumBarriers, BarrierBatchMax);
			pCommandList->ResourceBarrier(DispatchNum, Barriers);
			Barriers += BarrierBatchMax;
			NumBarriers -= BarrierBatchMax;
		}
	}
	else
	{
		pCommandList->ResourceBarrier(NumBarriers, Barriers);
	}
}

#if (PLATFORM_USE_BACKBUFFER_WRITE_TRANSITION_TRACKING == 0) && (PLATFORM_USE_SEPARATE_BACKBUFFER_WRITE_TRANSITION == 1)
#define LOCAL_USE_SEPARATE_BACKBUFFER_WRITE_TRANSITION 1
#else
#define LOCAL_USE_SEPARATE_BACKBUFFER_WRITE_TRANSITION 0
#endif

void ResourceBarriersSeparateRTV2SRV(
	ID3D12GraphicsCommandList* pCommandList,
	const TArray<D3D12_RESOURCE_BARRIER>& Barriers,
	int32 BarrierBatchMax)
{
	if (!GD3D12SeparateRTV2SRVTransitions)
	{
#if LOCAL_USE_SEPARATE_BACKBUFFER_WRITE_TRANSITION
		TArray<D3D12_RESOURCE_BARRIER, TInlineAllocator<4>> BackBufferBarriers;
		TArray<D3D12_RESOURCE_BARRIER, TInlineAllocator<8>> OtherBarriers;

		for (int32 Index = 0; Index < Barriers.Num(); ++Index)
		{
			const D3D12_RESOURCE_BARRIER& Barrier = Barriers[Index];
			if (Barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
				&& Barrier.Transition.StateBefore == D3D12_RESOURCE_STATE_PRESENT // can also be displayed as D3D12_RESOURCE_STATE_COMMON in pix
				&& Barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET)
			{
				BackBufferBarriers.Add(Barrier);
			}
			else
			{
				OtherBarriers.Add(Barrier);
			}
		}

		if (BackBufferBarriers.Num() > 0)
		{
			RecordResourceBarriersToCommandList(pCommandList, BackBufferBarriers.GetData(), BackBufferBarriers.Num(), BarrierBatchMax);
		}

		if (OtherBarriers.Num() > 0)
		{
			RecordResourceBarriersToCommandList(pCommandList, OtherBarriers.GetData(), OtherBarriers.Num(), BarrierBatchMax);
		}
#else
		RecordResourceBarriersToCommandList(pCommandList, Barriers.GetData(), Barriers.Num(), BarrierBatchMax);
#endif
	}
	else
	{
		TArray<D3D12_RESOURCE_BARRIER, TInlineAllocator<4>> RTV2SRVBarriers;
#if LOCAL_USE_SEPARATE_BACKBUFFER_WRITE_TRANSITION
		TArray<D3D12_RESOURCE_BARRIER, TInlineAllocator<4>> BackBufferBarriers;
#endif
		TArray<D3D12_RESOURCE_BARRIER, TInlineAllocator<8>> OtherBarriers;

		for (int32 Index = 0; Index < Barriers.Num(); ++Index)
		{
			const D3D12_RESOURCE_BARRIER& Barrier = Barriers[Index];

			if (Barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
				&& Barrier.Transition.StateBefore == D3D12_RESOURCE_STATE_RENDER_TARGET
				&& Barrier.Transition.StateAfter == (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
			{
				RTV2SRVBarriers.Add(Barrier);
			}
#if LOCAL_USE_SEPARATE_BACKBUFFER_WRITE_TRANSITION
			else if (Barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION
				&& Barrier.Transition.StateBefore == D3D12_RESOURCE_STATE_PRESENT // can also be displayed as D3D12_RESOURCE_STATE_COMMON in pix
				&& Barrier.Transition.StateAfter == D3D12_RESOURCE_STATE_RENDER_TARGET)
			{
				BackBufferBarriers.Add(Barrier);
			}
#endif
			else
			{
				OtherBarriers.Add(Barrier);
			}
		}

		if (RTV2SRVBarriers.Num() > 0)
		{
			RecordResourceBarriersToCommandList(pCommandList, RTV2SRVBarriers.GetData(), RTV2SRVBarriers.Num(), BarrierBatchMax);
		}

#if LOCAL_USE_SEPARATE_BACKBUFFER_WRITE_TRANSITION
		if (BackBufferBarriers.Num() > 0)
		{
			RecordResourceBarriersToCommandList(pCommandList, BackBufferBarriers.GetData(), BackBufferBarriers.Num(), BarrierBatchMax);
		}
#endif
		if (OtherBarriers.Num() > 0)
		{
			RecordResourceBarriersToCommandList(pCommandList, OtherBarriers.GetData(), OtherBarriers.Num(), BarrierBatchMax);
		}
	}
}
#undef LOCAL_USE_SEPARATE_BACKBUFFER_WRITE_TRANSITION

void FD3D12ResourceBarrierBatcher::Flush(FD3D12Device* Device, ID3D12GraphicsCommandList* pCommandList, int32 BarrierBatchMax)
{
	if (Barriers.Num())
	{
		check(pCommandList);
		ResourceBarriersSeparateRTV2SRV(pCommandList, Barriers, BarrierBatchMax);
	}

#if PLATFORM_USE_BACKBUFFER_WRITE_TRANSITION_TRACKING
	if (BackBufferBarriers.Num())
	{
		check(pCommandList);
		FD3D12ScopedTimedIntervalQuery BarrierScopeTimer(Device->GetBackBufferWriteBarrierTracker(), pCommandList);
		RecordResourceBarriersToCommandList(pCommandList, BackBufferBarriers.GetData(), BackBufferBarriers.Num(), BarrierBatchMax);
	}
#endif // #if PLATFORM_USE_BACKBUFFER_WRITE_TRANSITION_TRACKING

	Reset();
}

uint32 FD3D12Buffer::GetParentGPUIndex() const
{
	return Parent->GetGPUIndex();
}

