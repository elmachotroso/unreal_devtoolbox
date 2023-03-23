// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	VirtualShadowMapArray.h:
=============================================================================*/
#pragma once

#include "CoreMinimal.h"
#include "VirtualShadowMapArray.h"
#include "SceneManagement.h"
#include "InstanceCulling/InstanceCullingLoadBalancer.h"
#include "GPUScene.h"
#include "GPUMessaging.h"

class FRHIGPUBufferReadback;
class FGPUScene;

#define VSM_LOG_INVALIDATIONS 0

class FVirtualShadowMapCacheEntry
{
public:
	// True if the cache has been (re)populated, set to false on init and set to true once the cache update process has happened.
	// Also set to false whenever key data was not valid and all cached data is invalidated.
	bool IsValid() { return PrevVirtualShadowMapId != INDEX_NONE && bPrevRendered; }

	void UpdateLocal(int32 VirtualShadowMapId,
		const FWholeSceneProjectedShadowInitializer &InCacheValidKey);

	void UpdateClipmap(int32 VirtualShadowMapId,
		const FMatrix &WorldToLight,
		FIntPoint PageSpaceLocation,
		double LevelRadius,
		double ViewCenterZ,
		double ViewRadiusZ);

	void MarkRendered() { bCurrentRendered = true; }

	// Previous frame data
	FIntPoint PrevPageSpaceLocation = FIntPoint(0, 0);
	int32 PrevVirtualShadowMapId = INDEX_NONE;
	bool bPrevRendered = false;

	// Current frame data
	FIntPoint CurrentPageSpaceLocation = FIntPoint(0, 0);
	int32 CurrentVirtualShadowMapId = INDEX_NONE;
	bool bCurrentRendered = false;

	// TODO: Potentially refactor this to decouple the cache key details
	FWholeSceneProjectedShadowInitializer LocalCacheValidKey;

	struct FClipmapInfo
	{
		FMatrix	WorldToLight;
		double ViewCenterZ;
		double ViewRadiusZ;
	};
	FClipmapInfo Clipmap;
};

class FVirtualShadowMapPerLightCacheEntry
{
public:
	FVirtualShadowMapPerLightCacheEntry(int32 MaxPersistentScenePrimitiveIndex)
		: RenderedPrimitives(false, MaxPersistentScenePrimitiveIndex)
		, CachedPrimitives(false, MaxPersistentScenePrimitiveIndex)
	{
	}

	TSharedPtr<FVirtualShadowMapCacheEntry> FindCreateShadowMapEntry(int32 Index);

	void OnPrimitiveRendered(const FPrimitiveSceneInfo* PrimitiveSceneInfo);

	// Primitives that have been rendered (not culled) the previous frame, when a primitive transitions from being culled to not it must be rendered into the VSM
	// Key culling reasons are small size or distance cutoff.
	TBitArray<> RenderedPrimitives;

	// Primitives that have been rendered (not culled) _some_ previous frame, tracked so we can invalidate when they move/are removed (and not otherwise).
	TBitArray<> CachedPrimitives;

	// One entry represents the cached state of a given shadow map in the set of either a clipmap(N), one cube map(6) or a regular VSM (1)
	TArray< TSharedPtr<FVirtualShadowMapCacheEntry> > ShadowMapEntries;

	// TODO: refactor this to not ne stored in the cache entry when we move (some) invalidaitons to the end of frame rather than in the scene primitive updates.
	struct FInstanceRange
	{
		int32 InstanceSceneDataOffset;
		int32 NumInstanceSceneDataEntries;
	};

	TArray<FInstanceRange> PrimitiveInstancesToInvalidate;
};

// Persistent buffers that we ping pong frame by frame
struct FVirtualShadowMapArrayFrameData
{
	TRefCountPtr<FRDGPooledBuffer>				PageTable;
	TRefCountPtr<FRDGPooledBuffer>				PageFlags;

	TRefCountPtr<FRDGPooledBuffer>				ProjectionData;
	TRefCountPtr<FRDGPooledBuffer>				PageRectBounds;

	TRefCountPtr<FRDGPooledBuffer>				DynamicCasterPageFlags;

	TRefCountPtr<FRDGPooledBuffer>				PhysicalPageMetaData;

	TRefCountPtr<IPooledRenderTarget>			HZBPhysical;
	TMap<int32, FVirtualShadowMapHZBMetadata>	HZBMetadata;

	TRefCountPtr<FRDGPooledBuffer>				InvalidatingInstancesBuffer;
	int32										NumInvalidatingInstanceSlots = 0;
};

class FVirtualShadowMapArrayCacheManager
{
public:
	FVirtualShadowMapArrayCacheManager(FScene *InScene);
	~FVirtualShadowMapArrayCacheManager();

	// Enough for er lots...
	static constexpr uint32 MaxStatFrames = 512*1024U;

	// Called by VirtualShadowMapArray to potentially resize the physical pool
	// If the requested size is not already the size, all cache data is dropped and the pool is resized.
	TRefCountPtr<IPooledRenderTarget> SetPhysicalPoolSize(FRDGBuilder& GraphBuilder, FIntPoint RequestedSize);

	void FreePhysicalPool();

	// Invalidate the cache for all shadows, causing any pages to be rerendered
	void Invalidate();

	/**
	 * Call at end of frame to extract resouces from the virtual SM array to preserve to next frame.
	 * If bCachingEnabled is false, all previous frame data is dropped and cache (and HZB!) data will not be available for the next frame.
	 */ 
	void ExtractFrameData(FRDGBuilder& GraphBuilder,
		FVirtualShadowMapArray &VirtualShadowMapArray,
		const FSceneRenderer& SceneRenderer,
		bool bEnableCaching);

	/**
	 * Finds an existing cache entry and moves to the active set or creates a fresh one.
	 */
	TSharedPtr<FVirtualShadowMapPerLightCacheEntry> FindCreateLightCacheEntry(int32 LightSceneId);

	/**
	 * Finds an existing cache entry and moves to the active set or creates a fresh one.
	 */
	TSharedPtr<FVirtualShadowMapCacheEntry> FindCreateCacheEntry(int32 LightSceneId, int32 Index = 0);

	/*
	 * Returns true if cached data is available.
	 */
	bool IsValid();

	bool IsAccumulatingStats();

	using FInstanceGPULoadBalancer = TInstanceCullingLoadBalancer<SceneRenderingAllocator>;

	/**
	 * Helper to collect primitives that need invalidation, filters out redundant adds and also those that are not yet known to the GPU
	 */
	class FInvalidatingPrimitiveCollector
	{
	public:
		FInvalidatingPrimitiveCollector(FVirtualShadowMapArrayCacheManager* InVirtualShadowMapArrayCacheManager);

		/**
		 * Add a primitive to invalidate the instances for, the function filters redundant primitive adds, and thus expects valid IDs (so can't be called for primitives that have not yet been added)
		 * and unchanging IDs (so can't be used over a span that include any scene mutation).
		 */
		void Add(const FPrimitiveSceneInfo* PrimitiveSceneInfo);

		bool IsEmpty() const { return LoadBalancer.IsEmpty(); }

		TBitArray<SceneRenderingAllocator> AlreadyAddedPrimitives;
		FInstanceGPULoadBalancer LoadBalancer;
		int32 TotalInstanceCount = 0;
#if VSM_LOG_INVALIDATIONS
		FString RangesStr;
#endif
		const FScene& Scene;
		FGPUScene& GPUScene;
		FVirtualShadowMapArrayCacheManager& Manager;
	};

	/**
	 * This must to be executed before the instances are actually removed / updated, otherwise the wrong position will be used. 
	 * In particular, it must be processed before the Scene primitive IDs are updated/compacted as part of the removal.
	 * Invalidate pages that are touched by (the instances of) the removed primitives. 
	 */
	void ProcessRemovedOrUpdatedPrimitives(FRDGBuilder& GraphBuilder, const FGPUScene& GPUScene, FInvalidatingPrimitiveCollector& InvalidatingPrimitiveCollector);

	/**
	 * Allow the cache manager to track scene changes, in particular track resizing of primitive tracking data.
	 */
	void OnSceneChange();

	TRDGUniformBufferRef<FVirtualShadowMapUniformParameters> GetPreviousUniformBuffer(FRDGBuilder& GraphBuilder) const;

	FVirtualShadowMapArrayFrameData PrevBuffers;
	FVirtualShadowMapUniformParameters PrevUniformParameters;
		
	void SetHZBViewParams(int32 HZBKey, Nanite::FPackedViewParams& OutParams);


	GPUMessage::FSocket StatusFeedbackSocket;

private:
	void ProcessInvalidations(FRDGBuilder& GraphBuilder, FInstanceGPULoadBalancer& Instances, int32 TotalInstanceCount, const FGPUScene& GPUScene);

	void ProcessGPUInstanceInvalidations(FRDGBuilder& GraphBuilder, const FGPUScene& GPUScene);

	void ExtractStats(FRDGBuilder& GraphBuilder, FVirtualShadowMapArray &VirtualShadowMapArray);

	// The actual physical texture data is stored here rather than in VirtualShadowMapArray (which is recreated each frame)
	// This allows us to (optionally) persist cached pages between frames. Regardless of whether caching is enabled,
	// we store the physical pool here.
	TRefCountPtr<IPooledRenderTarget> PhysicalPagePool;

	// Index the Cache entries by the light ID
	TMap< int32, TSharedPtr<FVirtualShadowMapPerLightCacheEntry> > CacheEntries;
	TMap< int32, TSharedPtr<FVirtualShadowMapPerLightCacheEntry> > PrevCacheEntries;

	// Stores stats over frames when activated.
	TRefCountPtr<FRDGPooledBuffer> AccumulatedStatsBuffer;
	bool bAccumulatingStats = false;
	FRHIGPUBufferReadback* GPUBufferReadback = nullptr;
#if !UE_BUILD_SHIPPING
	FDelegateHandle ScreenMessageDelegate;
	int32 LastOverflowFrame = -1;
	bool bLoggedPageOverflow = false;
#endif
	FScene* Scene;
};
