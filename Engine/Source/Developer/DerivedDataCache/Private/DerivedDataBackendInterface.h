// Copyright Epic Games, Inc. All Rights Reserved.


#pragma once

#include "CoreTypes.h"
#include "Containers/BitArray.h"
#include "Containers/StringView.h"
#include "DerivedDataCache.h"
#include "DerivedDataLegacyCacheStore.h"
#include "Stats/Stats.h"
#include "Templates/DontCopy.h"
#include "Templates/PimplPtr.h"

class FDerivedDataCacheUsageStats;
class FDerivedDataCacheStatsNode;

namespace UE::DerivedData { struct FCacheKey; }
namespace UE::DerivedData::Private { struct FBackendDebugMissState; }

DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Num Gets"),STAT_DDC_NumGets,STATGROUP_DDC, );
DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Num Puts"),STAT_DDC_NumPuts,STATGROUP_DDC, );
DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Num Build"),STAT_DDC_NumBuilds,STATGROUP_DDC, );
DECLARE_DWORD_ACCUMULATOR_STAT_EXTERN(TEXT("Num Exists"),STAT_DDC_NumExist,STATGROUP_DDC, );
DECLARE_FLOAT_ACCUMULATOR_STAT_EXTERN(TEXT("Sync Get Time"),STAT_DDC_SyncGetTime,STATGROUP_DDC, );
DECLARE_FLOAT_ACCUMULATOR_STAT_EXTERN(TEXT("ASync Wait Time"),STAT_DDC_ASyncWaitTime,STATGROUP_DDC, );
DECLARE_FLOAT_ACCUMULATOR_STAT_EXTERN(TEXT("Sync Put Time"),STAT_DDC_PutTime,STATGROUP_DDC, );
DECLARE_FLOAT_ACCUMULATOR_STAT_EXTERN(TEXT("Sync Build Time"),STAT_DDC_SyncBuildTime,STATGROUP_DDC, );
DECLARE_FLOAT_ACCUMULATOR_STAT_EXTERN(TEXT("Exists Time"),STAT_DDC_ExistTime,STATGROUP_DDC, );

namespace UE::DerivedData
{

enum class EBackendLegacyMode
{
	/** Use only GetValue/PutValue. */
	ValueOnly,
	/** Use GetValue/PutValue with a fallback to GetCachedData+PutValue for misses. */
	ValueWithLegacyFallback,
	/** Use only GetCachedData/PutCachedData. */
	LegacyOnly,
};

/**
 * Speed classes. Higher values are faster so > / < comparisons make sense.
 */
enum class EBackendSpeedClass
{
	Unknown,		/* Don't know yet*/
	Slow,			/* Slow, likely a remote drive. Some benefit but handle with care */
	Ok,				/* Ok but not great.  */
	Fast,			/* Fast but seek times still have an impact */
	Local			/* Little to no impact from seek times and extremely fast reads */
};

/** Debug options that can be applied to backends to simulate different behavior */
struct FBackendDebugOptions
{
	/** Percentage of requests that should result in random misses */
	int					RandomMissRate;

	/** Apply behavior of this speed class */
	EBackendSpeedClass	SpeedClass;

	/** Types of DDC entries that should always be a miss */
	TArray<FString>		SimulateMissTypes;

	/** State for simulated misses. */
	TDontCopy<TPimplPtr<Private::FBackendDebugMissState>> SimulateMissState;

	FBackendDebugOptions();

	/** Fill in the provided structure based on the name of the node (e.g. 'shared') and the provided token stream */
	static bool ParseFromTokens(FBackendDebugOptions& OutOptions, const TCHAR* InNodeName, const TCHAR* InTokens);

	/**
	 * Returns true if, according to the properties of this struct, the provided key should be treated as a miss.
	 * Implementing that miss and accounting for any behavior impact (e.g. skipping a subsequent put) is left to
	 * each backend.
	 */
	bool ShouldSimulateMiss(const TCHAR* CacheKey) { return ShouldSimulateGetMiss(CacheKey); }
	bool ShouldSimulateMiss(const FCacheKey& CacheKey) { return ShouldSimulateGetMiss(CacheKey); }

	bool ShouldSimulatePutMiss(const FCacheKey& Key);
	bool ShouldSimulateGetMiss(const FCacheKey& Key);

	bool ShouldSimulatePutMiss(const TCHAR* LegacyKey);
	bool ShouldSimulateGetMiss(const TCHAR* LegacyKey);
};

/**
 * Interface for cache server backends.
 * The entire API should be callable from any thread (except the singleton can be assumed to be called at least once before concurrent access).
 */
class FDerivedDataBackendInterface : public ILegacyCacheStore
{
public:
	using ESpeedClass = UE::DerivedData::EBackendSpeedClass;
	using FBackendDebugOptions = UE::DerivedData::FBackendDebugOptions;

	/** Status of a put operation. */
	enum class EPutStatus
	{
		/** The put is executing asynchronously. */
		Executing,
		/** The put completed synchronously and the data was not cached. */
		NotCached,
		/** The put completed synchronously and the data was cached. */
		Cached,
		/** The put was skipped and should not be retried. */
		Skipped,
	};

	virtual ~FDerivedDataBackendInterface()
	{
	}

	/** Return a name for this interface */
	virtual FString GetName() const = 0;

	/** return true if this cache is writable */
	virtual bool IsWritable() const = 0;

	/**
	 * Returns true if hits on this cache should propagate to lower cache level. Typically false for a PAK file.
	 * Caution! This generally isn't propagated, so the thing that returns false must be a direct child of the hierarchical cache.
	 */
	virtual bool BackfillLowerCacheLevels() const
	{
		return true;
	}

	/**
	 * Returns a class of speed for this interface
	 */
	virtual ESpeedClass GetSpeedClass() const = 0;

	/**
	 * Synchronous test for the existence of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @return				true if the data probably will be found, this can't be guaranteed because of concurrency in the backends, corruption, etc
	 */
	virtual bool CachedDataProbablyExists(const TCHAR* CacheKey)=0;

	/**
	 * Synchronous test for the existence of multiple cache items
	 *
	 * @param	CacheKeys	Alphanumeric+underscore key of the cache items
	 * @return				A bit array with bits indicating whether the data for the corresponding key will probably be found
	 */
	virtual TBitArray<> CachedDataProbablyExistsBatch(TConstArrayView<FString> CacheKeys)
	{
		TBitArray<> Result;
		Result.Reserve(CacheKeys.Num());
		for (const FString& Key : CacheKeys)
		{
			Result.Add(CachedDataProbablyExists(*Key));
		}
		return Result;
	}

	/**
	 * Synchronous retrieve of a cache item
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @param	OutData		Buffer to receive the results, if any were found
	 * @return				true if any data was found, and in this case OutData is non-empty
	 */
	virtual bool GetCachedData(const TCHAR* CacheKey, TArray<uint8>& OutData) = 0;

	/**
	 * Asynchronous, fire-and-forget placement of a cache item
	 *
	 * @param	CacheKey			Alphanumeric+underscore key of this cache item
	 * @param	InData				Buffer containing the data to cache, can be destroyed after the call returns, immediately
	 * @param	bPutEvenIfExists	If true, then do not attempt skip the put even if CachedDataProbablyExists returns true
	 */
	virtual EPutStatus PutCachedData(const TCHAR* CacheKey, TArrayView<const uint8> InData, bool bPutEvenIfExists) = 0;

	/**
	 * Remove data from cache (used in the event that corruption is detected at a higher level and possibly house keeping)
	 *
	 * @param	CacheKey	Alphanumeric+underscore key of this cache item
	 * @param	bTransient	true if the data is transient and it is up to the backend to decide when and if to remove cached data.
	 */
	virtual void RemoveCachedData(const TCHAR* CacheKey, bool bTransient)=0;

	/**
	 * Retrieve usage stats for this backend. If the backend holds inner backends, this is expected to be passed down recursively.
	 */
	virtual TSharedRef<FDerivedDataCacheStatsNode> GatherUsageStats() const = 0;

	/**
	 * Synchronous attempt to make sure the cached data will be available as optimally as possible.
	 *
	 * @param	CacheKeys	Alphanumeric+underscore keys of the cache items
	 * @return				A bit array with bits indicating whether the data for the corresponding key will probably be found in a fast backend on a future request.
	 */
	virtual TBitArray<> TryToPrefetch(TConstArrayView<FString> CacheKeys) = 0;

	/**
	 * Allows the DDC backend to determine if it wants to cache the provided data. Reasons for returning false could be a slow connection,
	 * a file size limit, etc.
	 */
	virtual bool WouldCache(const TCHAR* CacheKey, TArrayView<const uint8> InData) = 0;

	/**
	 * Ask a backend to apply debug behavior to simulate different conditions. Backends that don't support these options should return
	 * false which will result in a warning if an attempt is made to apply these options.
	 */
	virtual bool ApplyDebugOptions(FBackendDebugOptions& InOptions) = 0;

	virtual EBackendLegacyMode GetLegacyMode() const = 0;

	virtual void LegacyPut(
		TConstArrayView<FLegacyCachePutRequest> Requests,
		IRequestOwner& Owner,
		FOnLegacyCachePutComplete&& OnComplete) final;

	virtual void LegacyGet(
		TConstArrayView<FLegacyCacheGetRequest> Requests,
		IRequestOwner& Owner,
		FOnLegacyCacheGetComplete&& OnComplete) final;

	virtual void LegacyDelete(
		TConstArrayView<FLegacyCacheDeleteRequest> Requests,
		IRequestOwner& Owner,
		FOnLegacyCacheDeleteComplete&& OnComplete) final;

	virtual void LegacyStats(FDerivedDataCacheStatsNode& OutNode) final;

	virtual bool LegacyDebugOptions(FBackendDebugOptions& Options) final;
};

class FDerivedDataBackend
{
public:
	static FDerivedDataBackend* Create();

	/**
	 * Singleton to retrieve the GLOBAL backend
	 *
	 * @return Reference to the global cache backend
	 */
	static FDerivedDataBackend& Get();

	virtual ~FDerivedDataBackend() = default;

	/**
	 * Singleton to retrieve the root cache
	 * @return Reference to the global cache root
	 */
	virtual ILegacyCacheStore& GetRoot() = 0;

	virtual int32 GetMaxKeyLength() const = 0;

	//--------------------
	// System Interface, copied from FDerivedDataCacheInterface
	//--------------------

	virtual void NotifyBootComplete() = 0;
	virtual void AddToAsyncCompletionCounter(int32 Addend) = 0;
	virtual bool AnyAsyncRequestsRemaining() = 0;
	virtual bool IsShuttingDown() = 0;
	virtual void WaitForQuiescence(bool bShutdown = false) = 0;
	virtual void GetDirectories(TArray<FString>& OutResults) = 0;
	virtual bool GetUsingSharedDDC() const = 0;
	virtual const TCHAR* GetGraphName() const = 0;
	virtual const TCHAR* GetDefaultGraphName() const = 0;

	/**
	 * Mounts a read-only pak file.
	 *
	 * @param PakFilename Pak filename
	 */
	virtual FDerivedDataBackendInterface* MountPakFile(const TCHAR* PakFilename) = 0;

	/**
	 * Unmounts a read-only pak file.
	 *
	 * @param PakFilename Pak filename
	 */
	virtual bool UnmountPakFile(const TCHAR* PakFilename) = 0;

	/**
	 *  Gather the usage of the DDC hierarchically.
	 */
	virtual TSharedRef<FDerivedDataCacheStatsNode> GatherUsageStats() const = 0;
};

/** Lexical conversions from and to enums */

[[nodiscard]] inline const TCHAR* LexToString(const EBackendLegacyMode Value)
{
	switch (Value)
	{
	case EBackendLegacyMode::ValueOnly:
		return TEXT("ValueOnly");
	case EBackendLegacyMode::ValueWithLegacyFallback:
		return TEXT("ValueWithLegacyFallback");
	case EBackendLegacyMode::LegacyOnly:
		return TEXT("LegacyOnly");
	default:
		checkNoEntry();
		return TEXT("Unknown");
	}
}

[[nodiscard]] inline bool TryLexFromString(EBackendLegacyMode& OutValue, const FStringView String)
{
	if (String == TEXTVIEW("ValueOnly"))
	{
		OutValue = EBackendLegacyMode::ValueOnly;
		return true;
	}
	if (String == TEXTVIEW("ValueWithLegacyFallback"))
	{
		OutValue = EBackendLegacyMode::ValueWithLegacyFallback;
		return true;
	}
	if (String == TEXTVIEW("LegacyOnly"))
	{
		OutValue = EBackendLegacyMode::LegacyOnly;
		return true;
	}
	return false;
}

inline const TCHAR* LexToString(FDerivedDataBackendInterface::ESpeedClass SpeedClass)
{
	switch (SpeedClass)
	{
	case FDerivedDataBackendInterface::ESpeedClass::Unknown:
		return TEXT("Unknown");
	case FDerivedDataBackendInterface::ESpeedClass::Slow:
		return TEXT("Slow");
	case FDerivedDataBackendInterface::ESpeedClass::Ok:
		return TEXT("Ok");
	case FDerivedDataBackendInterface::ESpeedClass::Fast:
		return TEXT("Fast");
	case FDerivedDataBackendInterface::ESpeedClass::Local:
		return TEXT("Local");
	}

	checkNoEntry();
	return TEXT("Unknown value! (Update LexToString!)");
}

inline void LexFromString(FDerivedDataBackendInterface::ESpeedClass& OutValue, const TCHAR* Buffer)
{
	OutValue = FDerivedDataBackendInterface::ESpeedClass::Unknown;

	if (FCString::Stricmp(Buffer, TEXT("Slow")) == 0)
	{
		OutValue = FDerivedDataBackendInterface::ESpeedClass::Slow;
	}
	else if (FCString::Stricmp(Buffer, TEXT("Ok")) == 0)
	{
		OutValue = FDerivedDataBackendInterface::ESpeedClass::Ok;
	}
	else if (FCString::Stricmp(Buffer, TEXT("Fast")) == 0)
	{
		OutValue = FDerivedDataBackendInterface::ESpeedClass::Fast;
	}
	else if (FCString::Stricmp(Buffer, TEXT("Local")) == 0)
	{
		OutValue = FDerivedDataBackendInterface::ESpeedClass::Local;
	}
}

} // UE::DerivedData
