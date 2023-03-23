// Copyright Epic Games, Inc. All Rights Reserved.

#include "VirtualizationDDCBackend.h"

#include "Misc/Parse.h"
#include "DerivedDataCache.h"
#include "DerivedDataCacheRecord.h"
#include "DerivedDataRequestOwner.h"
#include "DerivedDataValue.h"

namespace UE::Virtualization
{

/** Utility function to help convert from UE::Virtualization::FIoHash to UE::DerivedData::FValueId */
static UE::DerivedData::FValueId ToDerivedDataValueId(const FIoHash& Id)
{
	return UE::DerivedData::FValueId::FromHash(Id);
}

FDDCBackend::FDDCBackend(FStringView ConfigName, FStringView InDebugName)
: IVirtualizationBackend(ConfigName, InDebugName, EOperations::Both)
, BucketName(TEXT("BulkData"))
, TransferPolicy(UE::DerivedData::ECachePolicy::None)
, QueryPolicy(UE::DerivedData::ECachePolicy::None)
{
	
}

bool FDDCBackend::Initialize(const FString& ConfigEntry)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(Initialize::Initialize);

	if (!FParse::Value(*ConfigEntry, TEXT("Bucket="), BucketName))
	{
		UE_LOG(LogVirtualization, Fatal, TEXT("[%s] 'Bucket=' not found in the config file"), *GetDebugName());
	}

	bool bAllowLocal = true;
	if (FParse::Bool(*ConfigEntry, TEXT("LocalStorage="), bAllowLocal))
	{
		UE_LOG(LogVirtualization, Log, TEXT("[%s] Use of local storage set to '%s"), *GetDebugName(), bAllowLocal ? TEXT("true") : TEXT("false"));
	}

	bool bAllowRemote = true;
	if (FParse::Bool(*ConfigEntry, TEXT("RemoteStorage="), bAllowRemote))
	{
		UE_LOG(LogVirtualization, Log, TEXT("[%s] Use of remote storage set to '%s"), *GetDebugName(), bAllowRemote ? TEXT("true") : TEXT("false"));
	}

	if (!bAllowLocal && !bAllowRemote)
	{
		UE_LOG(LogVirtualization, Fatal, TEXT("[%s] LocalStorage and RemoteStorage cannot both be disabled"), *GetDebugName());
		return false;
	}

	if (bAllowLocal)
	{
		TransferPolicy |= UE::DerivedData::ECachePolicy::Local;
		QueryPolicy |= UE::DerivedData::ECachePolicy::QueryLocal;
	}

	if (bAllowRemote)
	{
		TransferPolicy |= UE::DerivedData::ECachePolicy::Remote;
		QueryPolicy |= UE::DerivedData::ECachePolicy::QueryRemote;
	}

	Bucket = UE::DerivedData::FCacheBucket(BucketName);

	return true;	
}

EPushResult FDDCBackend::PushData(const FIoHash& Id, const FCompressedBuffer& Payload, const FString& PackageContext)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FDDCBackend::PushData);

	if (DoesPayloadExist(Id))
	{
		UE_LOG(LogVirtualization, Verbose, TEXT("[%s] Already has a copy of the payload '%s'."), *GetDebugName(), *LexToString(Id));
		return EPushResult::PayloadAlreadyExisted;
	}

	UE::DerivedData::ICache& Cache = UE::DerivedData::GetCache();
	
	UE::DerivedData::FCacheKey Key;
	Key.Bucket = Bucket;
	Key.Hash = Id;

	UE::DerivedData::FValue DerivedDataValue(Payload);
	check(DerivedDataValue.GetRawHash() == Id);

	UE::DerivedData::FCacheRecordBuilder RecordBuilder(Key);
	RecordBuilder.AddValue(ToDerivedDataValueId(Id), DerivedDataValue);

	UE::DerivedData::FRequestOwner Owner(UE::DerivedData::EPriority::Blocking);

	UE::DerivedData::FCachePutResponse Result;
	auto Callback = [&Result](UE::DerivedData::FCachePutResponse&& Response)
	{
		Result = Response;
	};

	// TODO: Improve the name when we start passing more context to this function
	Cache.Put({{{TEXT("Mirage")}, RecordBuilder.Build(), TransferPolicy}}, Owner, MoveTemp(Callback));

	Owner.Wait();

	if (Result.Status == UE::DerivedData::EStatus::Ok)
	{
		return EPushResult::Success;
	}
	else
	{
		return EPushResult::Failed;
	}
}

FCompressedBuffer FDDCBackend::PullData(const FIoHash& Id)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FDDCBackend::PullData);

	UE::DerivedData::ICache& Cache = UE::DerivedData::GetCache();

	UE::DerivedData::FCacheKey Key;
	Key.Bucket = Bucket; 
	Key.Hash = Id;

	UE::DerivedData::FRequestOwner Owner(UE::DerivedData::EPriority::Blocking);

	FCompressedBuffer ResultData;
	UE::DerivedData::EStatus ResultStatus;

	auto Callback = [&Id, &ResultData, &ResultStatus](UE::DerivedData::FCacheGetResponse&& Response)
	{
		ResultStatus = Response.Status;
		if (ResultStatus == UE::DerivedData::EStatus::Ok)
		{
			ResultData = Response.Record.GetValue(ToDerivedDataValueId(Id)).GetData();
		}
	};

	// TODO: Improve the name when we start passing more context to this function
	Cache.Get({{{TEXT("Mirage")}, Key, TransferPolicy}}, Owner, MoveTemp(Callback));

	Owner.Wait();

	return ResultData;
}

bool FDDCBackend::DoesPayloadExist(const FIoHash& Id)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FDDCBackend::DoesPayloadExist);

	UE::DerivedData::ICache& Cache = UE::DerivedData::GetCache();

	UE::DerivedData::FCacheKey Key;
	Key.Bucket = Bucket;
	Key.Hash = Id;

	UE::DerivedData::FRequestOwner Owner(UE::DerivedData::EPriority::Blocking);
	
	UE::DerivedData::EStatus ResultStatus;
	auto Callback = [&ResultStatus](UE::DerivedData::FCacheGetResponse&& Response)
	{
		ResultStatus = Response.Status;
	};

	// TODO: Improve the name when we start passing more context to this function
	Cache.Get({{{TEXT("Mirage")}, Key, QueryPolicy | UE::DerivedData::ECachePolicy::SkipData}}, Owner, MoveTemp(Callback));

	Owner.Wait();

	return ResultStatus == UE::DerivedData::EStatus::Ok;
}

UE_REGISTER_VIRTUALIZATION_BACKEND_FACTORY(FDDCBackend, DDCBackend);
} // namespace UE::Virtualization