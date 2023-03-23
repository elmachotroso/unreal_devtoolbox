// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Online/CoreOnline.h"
#include "OnlineSubsystemTypes.h"

#include "Misc/ScopeRWLock.h"

namespace UE::Online {

/**
 * A net id registry suitable for use with OSS FUniqueNetIds
 */
class FOnlineUniqueNetIdRegistry : public IOnlineAccountIdRegistry
{
public:
	FOnlineUniqueNetIdRegistry(EOnlineServices InOnlineServicesType)
		: OnlineServicesType(InOnlineServicesType)
	{
	}

	virtual ~FOnlineUniqueNetIdRegistry() {}

	FOnlineAccountIdHandle FindOrAddHandle(const FUniqueNetIdRef& IdValue)
	{
		FOnlineAccountIdHandle Handle;
		// Take a read lock and check if we already have a handle
		{
			FReadScopeLock ReadLock(Lock);
			if (const uint32* FoundHandle = IdValueToHandleMap.Find(IdValue))
			{
				Handle = FOnlineAccountIdHandle(OnlineServicesType, *FoundHandle);
			}
		}

		if (!Handle.IsValid())
		{
			// Take a write lock, check again if we already have a handle, or insert a new element.
			FWriteScopeLock WriteLock(Lock);
			if (const uint32* FoundHandle = IdValueToHandleMap.Find(IdValue))
			{
				Handle = FOnlineAccountIdHandle(OnlineServicesType, *FoundHandle);
			}

			if (!Handle.IsValid())
			{
				IdValues.Emplace(IdValue);
				Handle = FOnlineAccountIdHandle(OnlineServicesType, IdValues.Num());
				IdValueToHandleMap.Emplace(IdValue, Handle.GetHandle());
			}
		}

		return Handle;
	}

	// Returns a copy as it's not thread safe to return a pointer/ref to an element of an array that can be relocated by another thread.
	FUniqueNetIdRef GetIdValue(const FOnlineAccountIdHandle Handle) const
	{
		if (Handle.GetOnlineServicesType() == OnlineServicesType
			&& Handle.IsValid()
			&& IdValues.IsValidIndex(Handle.GetHandle() - 1))
		{
			FReadScopeLock ReadLock(Lock);
			return IdValues[Handle.GetHandle() - 1];
		}
		return FUniqueNetIdString::EmptyId();
	}

	FUniqueNetIdRef GetIdValueChecked(const FOnlineAccountIdHandle Handle) const
	{
		check(Handle.GetOnlineServicesType() == OnlineServicesType
			&& Handle.IsValid()
			&& IdValues.IsValidIndex(Handle.GetHandle() - 1));
		return GetIdValue(Handle);
	}

	// Begin IOnlineAccountIdRegistry
	virtual FString ToLogString(const FOnlineAccountIdHandle& Handle) const override
	{
		return GetIdValue(Handle)->ToDebugString();
	}

	virtual TArray<uint8> ToReplicationData(const FOnlineAccountIdHandle& Handle) const
	{
		return TArray<uint8>();
	}

	virtual FOnlineAccountIdHandle FromReplicationData(const TArray<uint8>& Handle)
	{
		return FOnlineAccountIdHandle();
	}

	// End IOnlineAccountIdRegistry

private:
	mutable FRWLock Lock;

	EOnlineServices OnlineServicesType;
	TArray<FUniqueNetIdRef> IdValues;
	TUniqueNetIdMap<uint32> IdValueToHandleMap;
};

/* UE::Online */ }
