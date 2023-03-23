// Copyright Epic Games, Inc. All Rights Reserved.

#include "CookPackageData.h"

#include "Algo/AnyOf.h"
#include "Algo/Count.h"
#include "Algo/Find.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Async/ParallelFor.h"
#include "Cooker/CookPlatformManager.h"
#include "Cooker/CookRequestCluster.h"
#include "CookOnTheSide/CookOnTheFlyServer.h"
#include "Containers/StringView.h"
#include "EditorDomain/EditorDomain.h"
#include "Engine/Console.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreMiscDefines.h"
#include "Misc/PackageAccessTrackingOps.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/PreloadableFile.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "ShaderCompiler.h"
#include "UObject/Object.h"
#include "UObject/Package.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectHash.h"
#include "UObject/ObjectRedirector.h"

namespace UE::Cook
{

float GPollAsyncPeriod = .100f;
static FAutoConsoleVariableRef CVarPollAsyncPeriod(
	TEXT("cook.PollAsyncPeriod"),
	GPollAsyncPeriod,
	TEXT("Minimum time in seconds between PollPendingCookedPlatformDatas."),
	ECVF_Default);
	
//////////////////////////////////////////////////////////////////////////
// FPackageData
FPackageData::FPlatformData::FPlatformData()
	: bRequested(false), bCookAttempted(false), bCookSucceeded(false), bExplored(false)
{
}

FPackageData::FPackageData(FPackageDatas& PackageDatas, const FName& InPackageName, const FName& InFileName)
	: GeneratedOwner(nullptr), PackageName(InPackageName), FileName(InFileName), PackageDatas(PackageDatas)
	, Instigator(EInstigator::NotYetRequested), bIsUrgent(0)
	, bIsVisited(0), bIsPreloadAttempted(0)
	, bIsPreloaded(0), bHasSaveCache(0), bHasBeginPrepareSaveFailed(0), bCookedPlatformDataStarted(0)
	, bCookedPlatformDataCalled(0), bCookedPlatformDataComplete(0), bMonitorIsCooked(0)
	, bInitializedGeneratorSave(0), bCompletedGeneration(0), bGenerated(0)
{
	SetState(EPackageState::Idle);
	SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
}

FPackageData::~FPackageData()
{
	// ClearReferences should have been called earlier, but call it here in case it was missed
	ClearReferences();
	// We need to send OnLastCookedPlatformRemoved message to the monitor, so call SetPlatformsNotCooked
	SetPlatformsNotCooked();
	// Update the monitor's counters and call exit functions
	SendToState(EPackageState::Idle, ESendFlags::QueueNone);
}

void FPackageData::ClearReferences()
{
	DestroyGeneratorPackage();
}

const FName& FPackageData::GetPackageName() const
{
	return PackageName;
}

const FName& FPackageData::GetFileName() const
{
	return FileName;
}

void FPackageData::SetFileName(const FName& InFileName)
{
	FileName = InFileName;
}

int32 FPackageData::GetNumRequestedPlatforms() const
{
	int32 Result = 0;
	for (const TPair<const ITargetPlatform*, FPlatformData>& Pair : PlatformDatas)
	{
		Result += Pair.Value.bRequested ? 1 : 0;
	}
	return Result;
}

void FPackageData::SetPlatformsRequested(TConstArrayView<const ITargetPlatform*> TargetPlatforms, bool bRequested)
{
	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		PlatformDatas.FindOrAdd(TargetPlatform).bRequested = true;
	}
}

void FPackageData::ClearRequestedPlatforms()
{
	for (TPair<const ITargetPlatform*, FPlatformData>& Pair : PlatformDatas)
	{
		Pair.Value.bRequested = false;
	}
}

bool FPackageData::HasAllRequestedPlatforms(const TArrayView<const ITargetPlatform* const>& Platforms) const
{
	if (Platforms.Num() == 0)
	{
		return true;
	}
	if (PlatformDatas.Num() == 0)
	{
		return false;
	}

	for (const ITargetPlatform* QueryPlatform : Platforms)
	{
		const FPlatformData* PlatformData = PlatformDatas.Find(QueryPlatform);
		if (!PlatformData || !PlatformData->bRequested)
		{
			return false;
		}
	}
	return true;
}

bool FPackageData::AreAllRequestedPlatformsCooked(bool bAllowFailedCooks) const
{
	for (const TPair<const ITargetPlatform*, FPlatformData>& Pair : PlatformDatas)
	{
		if (Pair.Value.bRequested && (!Pair.Value.bCookAttempted || (!bAllowFailedCooks && !Pair.Value.bCookSucceeded)))
		{
			return false;
		}
	}
	return true;
}

bool FPackageData::AreAllRequestedPlatformsExplored() const
{
	for (const TPair<const ITargetPlatform*, FPlatformData>& Pair : PlatformDatas)
	{
		if (Pair.Value.bRequested && !Pair.Value.bExplored)
		{
			return false;
		}
	}
	return true;
}

bool FPackageData::HasAllExploredPlatforms(const TArrayView<const ITargetPlatform* const>& Platforms) const
{
	if (Platforms.Num() == 0)
	{
		return true;
	}
	if (PlatformDatas.Num() == 0)
	{
		return false;
	}

	for (const ITargetPlatform* QueryPlatform : Platforms)
	{
		const FPlatformData* PlatformData = FindPlatformData(QueryPlatform);
		if (!PlatformData || !PlatformData->bExplored)
		{
			return false;
		}
	}
	return true;
}

void FPackageData::SetIsUrgent(bool Value)
{
	bool OldValue = static_cast<bool>(bIsUrgent);
	if (OldValue != Value)
	{
		bIsUrgent = Value != 0;
		PackageDatas.GetMonitor().OnUrgencyChanged(*this);
	}
}

void FPackageData::UpdateRequestData(const TConstArrayView<const ITargetPlatform*> InRequestedPlatforms,
	bool bInIsUrgent, FCompletionCallback&& InCompletionCallback, FInstigator&& InInstigator, bool bAllowUpdateUrgency)
{
	if (IsInProgress())
	{
		AddCompletionCallback(MoveTemp(InCompletionCallback));

		bool bUrgencyChanged = false;
		if (bInIsUrgent && !GetIsUrgent())
		{
			bUrgencyChanged = true;
			SetIsUrgent(true);
		}

		if (!HasAllRequestedPlatforms(InRequestedPlatforms))
		{
			// Send back to the Request state (canceling any current operations) and then add the new platforms
			if (GetState() != EPackageState::Request)
			{
				SendToState(EPackageState::Request, ESendFlags::QueueAddAndRemove);
			}
			SetPlatformsRequested(InRequestedPlatforms, true);
		}
		else if (bUrgencyChanged && bAllowUpdateUrgency)
		{
			SendToState(GetState(), ESendFlags::QueueAddAndRemove);
		}
	}
	else if (InRequestedPlatforms.Num() > 0)
	{
		SetRequestData(InRequestedPlatforms, bInIsUrgent, MoveTemp(InCompletionCallback), MoveTemp(InInstigator));
		SendToState(EPackageState::Request, ESendFlags::QueueAddAndRemove);
	}
}

void FPackageData::SetRequestData(const TArrayView<const ITargetPlatform* const>& InRequestedPlatforms,
	bool bInIsUrgent, FCompletionCallback&& InCompletionCallback, FInstigator&& InInstigator)
{
	check(!CompletionCallback);
	check(GetNumRequestedPlatforms() == 0)
	check(!bIsUrgent);

	check(InRequestedPlatforms.Num() != 0);
	SetPlatformsRequested(InRequestedPlatforms, true);
	SetIsUrgent(bInIsUrgent);
	AddCompletionCallback(MoveTemp(InCompletionCallback));
	if (Instigator.Category == EInstigator::NotYetRequested)
	{
		Instigator = MoveTemp(InInstigator);
		PackageDatas.DebugInstigator(*this);
	}
}

void FPackageData::ClearInProgressData()
{
	ClearRequestedPlatforms();
	SetIsUrgent(false);
	CompletionCallback = FCompletionCallback();
}

void FPackageData::SetPlatformsCooked(const TConstArrayView<const ITargetPlatform*> TargetPlatforms,
	const TConstArrayView<bool> Succeeded)
{
	check(TargetPlatforms.Num() == Succeeded.Num());
	for (int32 n = 0; n < TargetPlatforms.Num(); ++n)
	{
		SetPlatformCooked(TargetPlatforms[n], Succeeded[n]);
	}
}

void FPackageData::SetPlatformsCooked(const TConstArrayView<const ITargetPlatform*> TargetPlatforms,
	bool bSucceeded)
{
	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		SetPlatformCooked(TargetPlatform, bSucceeded);
	}
}

void FPackageData::SetPlatformCooked(const ITargetPlatform* TargetPlatform, bool bSucceeded)
{
	bool bHasAnyOthers = false;
	bool bModified = false;
	bool bExists = false;
	for (TPair<const ITargetPlatform*, FPlatformData>& Pair : PlatformDatas)
	{
		if (Pair.Key == TargetPlatform)
		{
			bExists = true;
			bModified = bModified | (Pair.Value.bCookAttempted == false);
			Pair.Value.bCookAttempted = true;
			Pair.Value.bCookSucceeded = bSucceeded;
		}
		else
		{
			bHasAnyOthers = bHasAnyOthers | (Pair.Value.bCookAttempted != false);
		}
	}
	if (!bExists)
	{
		FPlatformData& Value = PlatformDatas.FindOrAdd(TargetPlatform);
		Value.bCookAttempted = true;
		Value.bCookSucceeded = bSucceeded;
		bModified = true;
	}
	if (bModified && !bHasAnyOthers)
	{
		PackageDatas.GetMonitor().OnFirstCookedPlatformAdded(*this);
	}
}

void FPackageData::SetPlatformsNotCooked(const TConstArrayView<const ITargetPlatform*> TargetPlatforms)
{
	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		SetPlatformNotCooked(TargetPlatform);
	}
}

void FPackageData::SetPlatformsNotCooked()
{
	bool bModified = false;
	for (TPair<const ITargetPlatform*, FPlatformData>& Pair : PlatformDatas)
	{
		bModified = bModified | (Pair.Value.bCookAttempted != false);
		Pair.Value.bCookAttempted = false;
		Pair.Value.bCookSucceeded = false;
	}
	if (bModified)
	{
		PackageDatas.GetMonitor().OnLastCookedPlatformRemoved(*this);
	}
}

void FPackageData::SetPlatformNotCooked(const ITargetPlatform* TargetPlatform)
{
	bool bHasAnyOthers = false;
	bool bModified = false;
	for (TPair<const ITargetPlatform*, FPlatformData>& Pair : PlatformDatas)
	{
		if (Pair.Key == TargetPlatform)
		{
			bModified = bModified | (Pair.Value.bCookAttempted != false);
			Pair.Value.bCookAttempted = false;
			Pair.Value.bCookSucceeded = false;
		}
		else
		{
			bHasAnyOthers = bHasAnyOthers | (Pair.Value.bCookAttempted != false);
		}
	}
	if (bModified && !bHasAnyOthers)
	{
		PackageDatas.GetMonitor().OnLastCookedPlatformRemoved(*this);
	}
}

const TSortedMap<const ITargetPlatform*, FPackageData::FPlatformData>& FPackageData::GetPlatformDatas() const
{
	return PlatformDatas;
}

FPackageData::FPlatformData& FPackageData::FindOrAddPlatformData(const ITargetPlatform* TargetPlatform)
{
	return PlatformDatas.FindOrAdd(TargetPlatform);
}

FPackageData::FPlatformData* FPackageData::FindPlatformData(const ITargetPlatform* TargetPlatform)
{
	return PlatformDatas.Find(TargetPlatform);
}

const FPackageData::FPlatformData* FPackageData::FindPlatformData(const ITargetPlatform* TargetPlatform) const
{
	return PlatformDatas.Find(TargetPlatform);
}

bool FPackageData::HasAnyCookedPlatform() const
{
	return Algo::AnyOf(PlatformDatas,
		[](const TPair<const ITargetPlatform*, FPlatformData>& Pair) { return Pair.Value.bCookAttempted; });
}

bool FPackageData::HasAnyCookedPlatforms(const TArrayView<const ITargetPlatform* const>& Platforms,
	bool bIncludeFailed) const
{
	if (PlatformDatas.Num() == 0)
	{
		return false;
	}

	for (const ITargetPlatform* QueryPlatform : Platforms)
	{
		if (HasCookedPlatform(QueryPlatform, bIncludeFailed))
		{
			return true;
		}
	}
	return false;
}

bool FPackageData::HasAllCookedPlatforms(const TArrayView<const ITargetPlatform* const>& Platforms,
	bool bIncludeFailed) const
{
	if (Platforms.Num() == 0)
	{
		return true;
	}
	if (PlatformDatas.Num() == 0)
	{
		return false;
	}

	for (const ITargetPlatform* QueryPlatform : Platforms)
	{
		if (!HasCookedPlatform(QueryPlatform, bIncludeFailed))
		{
			return false;
		}
	}
	return true;
}

bool FPackageData::HasCookedPlatform(const ITargetPlatform* Platform, bool bIncludeFailed) const
{
	ECookResult Result = GetCookResults(Platform);
	return (Result == ECookResult::Succeeded) | ((Result == ECookResult::Failed) & (bIncludeFailed != 0));
}

ECookResult FPackageData::GetCookResults(const ITargetPlatform* Platform) const
{
	const FPlatformData* PlatformData = PlatformDatas.Find(Platform);
	if (PlatformData && PlatformData->bCookAttempted)
	{
		return PlatformData->bCookSucceeded ? ECookResult::Succeeded : ECookResult::Failed;
	}
	return ECookResult::Unseen;
}

UPackage* FPackageData::GetPackage() const
{
	return Package.Get();
}

void FPackageData::SetPackage(UPackage* InPackage)
{
	Package = InPackage;
}

EPackageState FPackageData::GetState() const
{
	return static_cast<EPackageState>(State);
}

/** Boilerplate-reduction struct that defines all multi-state properties and sets them based on the given state. */
struct FStateProperties
{
	EPackageStateProperty Properties;
	explicit FStateProperties(EPackageState InState)
	{
		switch (InState)
		{
		case EPackageState::Idle:
			Properties = EPackageStateProperty::None;
			break;
		case EPackageState::Request:
			Properties = EPackageStateProperty::InProgress;
			break;
		case EPackageState::LoadPrepare:
			Properties = EPackageStateProperty::InProgress | EPackageStateProperty::Loading;
			break;
		case EPackageState::LoadReady:
			Properties = EPackageStateProperty::InProgress | EPackageStateProperty::Loading;
			break;
		// TODO_SaveQueue: When we add state PrepareForSave, it will also have bHasPackage = true, 
		case EPackageState::Save:
			Properties = EPackageStateProperty::InProgress | EPackageStateProperty::HasPackage;
			break;
		default:
			check(false);
			Properties = EPackageStateProperty::None;
			break;
		}
	}
};

void FPackageData::SendToState(EPackageState NextState, ESendFlags SendFlags)
{
	EPackageState OldState = GetState();
	switch (OldState)
	{
	case EPackageState::Idle:
		OnExitIdle();
		break;
	case EPackageState::Request:
		if (!!(SendFlags & ESendFlags::QueueRemove))
		{
			ensure(PackageDatas.GetRequestQueue().Remove(this) == 1);
		}
		OnExitRequest();
		break;
	case EPackageState::LoadPrepare:
		if (!!(SendFlags & ESendFlags::QueueRemove))
		{
			ensure(PackageDatas.GetLoadPrepareQueue().Remove(this) == 1);
		}
		OnExitLoadPrepare();
		break;
	case EPackageState::LoadReady:
		if (!!(SendFlags & ESendFlags::QueueRemove))
		{
			ensure(PackageDatas.GetLoadReadyQueue().Remove(this) == 1);
		}
		OnExitLoadReady();
		break;
	case EPackageState::Save:
		if (!!(SendFlags & ESendFlags::QueueRemove))
		{
			ensure(PackageDatas.GetSaveQueue().Remove(this) == 1);
		}
		OnExitSave();
		break;
	default:
		check(false);
		break;
	}

	FStateProperties OldProperties(OldState);
	FStateProperties NewProperties(NextState);
	// Exit state properties from highest to lowest; enter state properties from lowest to highest.
	// This ensures that properties that rely on earlier properties are constructed later and torn down earlier
	// than the earlier properties.
	for (EPackageStateProperty Iterator = EPackageStateProperty::Max;
		Iterator >= EPackageStateProperty::Min;
		Iterator = static_cast<EPackageStateProperty>(static_cast<uint32>(Iterator) >> 1))
	{
		if (((OldProperties.Properties & Iterator) != EPackageStateProperty::None) &
			((NewProperties.Properties & Iterator) == EPackageStateProperty::None))
		{
			switch (Iterator)
			{
			case EPackageStateProperty::InProgress:
				OnExitInProgress();
				break;
			case EPackageStateProperty::Loading:
				OnExitLoading();
				break;
			case EPackageStateProperty::HasPackage:
				OnExitHasPackage();
				break;
			default:
				check(false);
				break;
			}
		}
	}
	for (EPackageStateProperty Iterator = EPackageStateProperty::Min;
		Iterator <= EPackageStateProperty::Max;
		Iterator = static_cast<EPackageStateProperty>(static_cast<uint32>(Iterator) << 1))
	{
		if (((OldProperties.Properties & Iterator) == EPackageStateProperty::None) &
			((NewProperties.Properties & Iterator) != EPackageStateProperty::None))
		{
			switch (Iterator)
			{
			case EPackageStateProperty::InProgress:
				OnEnterInProgress();
				break;
			case EPackageStateProperty::Loading:
				OnEnterLoading();
				break;
			case EPackageStateProperty::HasPackage:
				OnEnterHasPackage();
				break;
			default:
				check(false);
				break;
			}
		}
	}


	SetState(NextState);
	switch (NextState)
	{
	case EPackageState::Idle:
		OnEnterIdle();
		break;
	case EPackageState::Request:
		OnEnterRequest();
		if (((SendFlags & ESendFlags::QueueAdd) != ESendFlags::QueueNone))
		{
			PackageDatas.GetRequestQueue().AddRequest(this);
		}
		break;
	case EPackageState::LoadPrepare:
		OnEnterLoadPrepare();
		if ((SendFlags & ESendFlags::QueueAdd) != ESendFlags::QueueNone)
		{
			if (GetIsUrgent())
			{
				PackageDatas.GetLoadPrepareQueue().AddFront(this);
			}
			else
			{
				PackageDatas.GetLoadPrepareQueue().Add(this);
			}
		}
		break;
	case EPackageState::LoadReady:
		OnEnterLoadReady();
		if ((SendFlags & ESendFlags::QueueAdd) != ESendFlags::QueueNone)
		{
			if (GetIsUrgent())
			{
				PackageDatas.GetLoadReadyQueue().AddFront(this);
			}
			else
			{
				PackageDatas.GetLoadReadyQueue().Add(this);
			}
		}
		break;
	case EPackageState::Save:
		OnEnterSave();
		if (((SendFlags & ESendFlags::QueueAdd) != ESendFlags::QueueNone))
		{
			if (GetIsUrgent())
			{
				PackageDatas.GetSaveQueue().AddFront(this);
			}
			else
			{
				PackageDatas.GetSaveQueue().Add(this);
			}
		}
		break;
	default:
		check(false);
		break;
	}

	PackageDatas.GetMonitor().OnStateChanged(*this, OldState);
}

void FPackageData::CheckInContainer() const
{
	switch (GetState())
	{
	case EPackageState::Idle:
		break;
	case EPackageState::Request:
		check(PackageDatas.GetRequestQueue().Contains(this));
		break;
	case EPackageState::LoadPrepare:
		check(PackageDatas.GetLoadPrepareQueue().Contains(this));
		break;
	case EPackageState::LoadReady:
		check(Algo::Find(PackageDatas.GetLoadReadyQueue(), this) != nullptr);
		break;
	case EPackageState::Save:
		// The save queue is huge and often pushed at end. Check last element first and then scan.
		check(PackageDatas.GetSaveQueue().Num() && (PackageDatas.GetSaveQueue().Last() == this || Algo::Find(PackageDatas.GetSaveQueue(), this)));
		break;
	default:
		check(false);
		break;
	}
}

bool FPackageData::IsInProgress() const
{
	return IsInStateProperty(EPackageStateProperty::InProgress);
}

bool FPackageData::IsInStateProperty(EPackageStateProperty Property) const
{
	return (FStateProperties(GetState()).Properties & Property) != EPackageStateProperty::None;
}

void FPackageData::OnEnterIdle()
{
	// Note that this might be on construction of the PackageData
}

void FPackageData::OnExitIdle()
{
	if (PackageDatas.GetLogDiscoveredPackages())
	{
		UE_LOG(LogCook, Warning, TEXT("Missing dependency: Package %s discovered after initial dependency search."), *WriteToString<256>(PackageName));
	}
}

void FPackageData::OnEnterRequest()
{
	// It is not valid to enter the request state without requested platforms; it indicates a bug due to e.g.
	// calling SendToState without UpdateRequestData from Idle
	check(GetNumRequestedPlatforms() > 0);
}

void FPackageData::OnExitRequest()
{
}

void FPackageData::OnEnterLoadPrepare()
{
}

void FPackageData::OnExitLoadPrepare()
{
}

void FPackageData::OnEnterLoadReady()
{
}

void FPackageData::OnExitLoadReady()
{
}

void FPackageData::OnEnterSave()
{
	check(GetPackage() != nullptr && GetPackage()->IsFullyLoaded());

	check(!GetHasBeginPrepareSaveFailed());
	CheckObjectCacheEmpty();
	CheckCookedPlatformDataEmpty();
}

void FPackageData::OnExitSave()
{
	PackageDatas.GetCookOnTheFlyServer().ReleaseCookedPlatformData(*this, false /* bCompletedSave */);
	ClearObjectCache();
	SetHasBeginPrepareSaveFailed(false);
}

void FPackageData::OnEnterInProgress()
{
	PackageDatas.GetMonitor().OnInProgressChanged(*this, true);
}

void FPackageData::OnExitInProgress()
{
	PackageDatas.GetMonitor().OnInProgressChanged(*this, false);
	UE::Cook::FCompletionCallback LocalCompletionCallback(MoveTemp(GetCompletionCallback()));
	if (LocalCompletionCallback)
	{
		LocalCompletionCallback(this);
	}
	ClearInProgressData();
}

void FPackageData::OnEnterLoading()
{
	CheckPreloadEmpty();
}

void FPackageData::OnExitLoading()
{
	ClearPreload();
}

void FPackageData::OnEnterHasPackage()
{
}

void FPackageData::OnExitHasPackage()
{
	SetPackage(nullptr);
}

void FPackageData::SetState(EPackageState NextState)
{
	State = static_cast<uint32>(NextState);
}

FCompletionCallback& FPackageData::GetCompletionCallback()
{
	return CompletionCallback;
}

void FPackageData::AddCompletionCallback(FCompletionCallback&& InCompletionCallback)
{
	if (InCompletionCallback)
	{
		// We don't yet have a mechanism for calling two completion callbacks.
		// CompletionCallbacks only come from external requests, and it should not be possible to request twice,
		// so a failed check here shouldn't happen.
		check(!CompletionCallback);
		CompletionCallback = MoveTemp(InCompletionCallback);
	}
}

bool FPackageData::TryPreload()
{
	check(IsInStateProperty(EPackageStateProperty::Loading));
	if (GetIsPreloadAttempted())
	{
		return true;
	}
	if (FindObjectFast<UPackage>(nullptr, GetPackageName()))
	{
		// If the package has already loaded, then there is no point in further preloading
		ClearPreload();
		SetIsPreloadAttempted(true);
		return true;
	}
	if (IsGenerated())
	{
		// Deferred populate generated packages are loaded from their generator, not from disk
		ClearPreload();
		SetIsPreloadAttempted(true);
		return true;
	}
	if (!PreloadableFile.Get())
	{
		if (FEditorDomain* EditorDomain = FEditorDomain::Get())
		{
			EditorDomain->PrecachePackageDigest(GetPackageName());
		}
		TStringBuilder<NAME_SIZE> FileNameString;
		GetFileName().ToString(FileNameString);
		PreloadableFile.Set(MakeShared<FPreloadableArchive>(FileNameString.ToString()), *this);
		PreloadableFile.Get()->InitializeAsync([this]()
			{
				TStringBuilder<NAME_SIZE> FileNameString;
				// Note this async callback has an read of this->GetFilename and a write of PreloadableFileOpenResult
				// outside of a critical section. This read and write is allowed because GetFilename does
				// not change until this is destructed, and the destructor does not run and other threads do not read
				// or write PreloadableFileOpenResult until after PreloadableFile.Get() has finished initialization
				// and this callback is therefore complete.
				// The code that accomplishes that waiting is in TryPreload (IsInitialized) and ClearPreload (ReleaseCache)
				this->GetFileName().ToString(FileNameString);
				FPackagePath PackagePath = FPackagePath::FromLocalPath(FileNameString);
				FOpenPackageResult Result = IPackageResourceManager::Get().OpenReadPackage(PackagePath);
				if (Result.Archive)
				{
					this->PreloadableFileOpenResult.CopyMetaData(Result);
				}
				return Result.Archive.Release();
			},
			FPreloadableFile::Flags::PreloadHandle | FPreloadableFile::Flags::Prime);
	}
	const TSharedPtr<FPreloadableArchive>& FilePtr = PreloadableFile.Get();
	if (!FilePtr->IsInitialized())
	{
		if (GetIsUrgent())
		{
			// For urgent requests, wait on them to finish preloading rather than letting them run asynchronously
			// and coming back to them later
			FilePtr->WaitForInitialization();
			check(FilePtr->IsInitialized());
		}
		else
		{
			return false;
		}
	}
	if (FilePtr->TotalSize() < 0)
	{
		UE_LOG(LogCook, Warning, TEXT("Failed to find file when preloading %s."), *GetFileName().ToString());
		SetIsPreloadAttempted(true);
		PreloadableFile.Reset(*this);
		PreloadableFileOpenResult = FOpenPackageResult();
		return true;
	}

	TStringBuilder<NAME_SIZE> FileNameString;
	GetFileName().ToString(FileNameString);
	if (!IPackageResourceManager::TryRegisterPreloadableArchive(FPackagePath::FromLocalPath(FileNameString),
		FilePtr, PreloadableFileOpenResult))
	{
		UE_LOG(LogCook, Warning, TEXT("Failed to register %s for preload."), *GetFileName().ToString());
		SetIsPreloadAttempted(true);
		PreloadableFile.Reset(*this);
		PreloadableFileOpenResult = FOpenPackageResult();
		return true;
	}

	SetIsPreloaded(true);
	SetIsPreloadAttempted(true);
	return true;
}

void FPackageData::FTrackedPreloadableFilePtr::Set(TSharedPtr<FPreloadableArchive>&& InPtr, FPackageData& Owner)
{
	Reset(Owner);
	if (InPtr)
	{
		Ptr = MoveTemp(InPtr);
		Owner.PackageDatas.GetMonitor().OnPreloadAllocatedChanged(Owner, true);
	}
}

void FPackageData::FTrackedPreloadableFilePtr::Reset(FPackageData& Owner)
{
	if (Ptr)
	{
		Owner.PackageDatas.GetMonitor().OnPreloadAllocatedChanged(Owner, false);
		Ptr.Reset();
	}
}

void FPackageData::ClearPreload()
{
	const TSharedPtr<FPreloadableArchive>& FilePtr = PreloadableFile.Get();
	if (GetIsPreloaded())
	{
		check(FilePtr);
		TStringBuilder<NAME_SIZE> FileNameString;
		GetFileName().ToString(FileNameString);
		if (IPackageResourceManager::UnRegisterPreloadableArchive(FPackagePath::FromLocalPath(FileNameString)))
		{
			UE_LOG(LogCook, Display, TEXT("PreloadableFile was created for %s but never used. This is wasteful and bad for cook performance."),
				*PackageName.ToString());
		}
		FilePtr->ReleaseCache(); // ReleaseCache to conserve memory if the Linker still has a pointer to it
	}
	else
	{
		check(!FilePtr || !FilePtr->IsCacheAllocated());
	}

	PreloadableFile.Reset(*this);
	PreloadableFileOpenResult = FOpenPackageResult();
	SetIsPreloaded(false);
	SetIsPreloadAttempted(false);
}

void FPackageData::CheckPreloadEmpty()
{
	check(!GetIsPreloadAttempted());
	check(!PreloadableFile.Get());
	check(!GetIsPreloaded());
}

TArray<FWeakObjectPtr>& FPackageData::GetCachedObjectsInOuter()
{
	return CachedObjectsInOuter;
}

void FPackageData::CheckObjectCacheEmpty() const
{
	check(CachedObjectsInOuter.Num() == 0);
	check(!GetHasSaveCache());
}

void FPackageData::CreateObjectCache()
{
	if (GetHasSaveCache())
	{
		return;
	}

	UPackage* LocalPackage = GetPackage();
	if (LocalPackage && LocalPackage->IsFullyLoaded())
	{
		PackageName = LocalPackage->GetFName();
		TArray<UObject*> ObjectsInOuter;
		GetObjectsWithOuter(LocalPackage, ObjectsInOuter);
		CachedObjectsInOuter.Reset(ObjectsInOuter.Num());
		for (UObject* Object : ObjectsInOuter)
		{
			FWeakObjectPtr ObjectWeakPointer(Object);
			// ignore pending kill objects; they will not be serialized out so we don't need to call
			// BeginCacheForCookedPlatformData on them
			if (!ObjectWeakPointer.Get())
			{
				continue;
			}
			CachedObjectsInOuter.Emplace(MoveTemp(ObjectWeakPointer));
		}
		SetHasSaveCache(true);
	}
	else
	{
		check(false);
	}
}

void FPackageData::ClearObjectCache()
{
	CachedObjectsInOuter.Empty();
	SetHasSaveCache(false);
}

const int32& FPackageData::GetNumPendingCookedPlatformData() const
{
	return NumPendingCookedPlatformData;
}

int32& FPackageData::GetNumPendingCookedPlatformData()
{
	return NumPendingCookedPlatformData;
}

const int32& FPackageData::GetCookedPlatformDataNextIndex() const
{
	return CookedPlatformDataNextIndex;
}

int32& FPackageData::GetCookedPlatformDataNextIndex()
{
	return CookedPlatformDataNextIndex;
}

void FPackageData::CheckCookedPlatformDataEmpty() const
{
	check(GetCookedPlatformDataNextIndex() == 0);
	check(!GetCookedPlatformDataStarted());
	check(!GetCookedPlatformDataCalled());
	check(!GetCookedPlatformDataComplete());
}

void FPackageData::ClearCookedPlatformData()
{
	CookedPlatformDataNextIndex = 0;
	// Note that GetNumPendingCookedPlatformData is not cleared; it persists across Saves and CookSessions
	SetCookedPlatformDataStarted(false);
	SetCookedPlatformDataCalled(false);
	SetCookedPlatformDataComplete(false);
}

void FPackageData::ResetGenerationProgress()
{
	SetInitializedGeneratorSave(false);
	SetCompletedGeneration(false);
}

void FPackageData::OnRemoveSessionPlatform(const ITargetPlatform* Platform)
{
	PlatformDatas.Remove(Platform);
}

bool FPackageData::HasReferencedObjects() const
{
	return Package != nullptr || CachedObjectsInOuter.Num() > 0;
}

void FPackageData::RemapTargetPlatforms(const TMap<ITargetPlatform*, ITargetPlatform*>& Remap)
{
	typedef TSortedMap<const ITargetPlatform*, FPlatformData> MapType;
	MapType NewPlatformDatas;
	NewPlatformDatas.Reserve(PlatformDatas.Num());
	for (TPair<const ITargetPlatform*, FPlatformData>& ExistingPair : PlatformDatas)
	{
		ITargetPlatform* NewKey = Remap[ExistingPair.Key];
		NewPlatformDatas.FindOrAdd(NewKey) = MoveTemp(ExistingPair.Value);
	}

	// The save state (and maybe more in the future) depend on the order of the request platforms remaining
	// unchanged, due to CookedPlatformDataNextIndex. If we change that order due to the remap, we need to
	// demote back to request.
	if (IsInProgress() && GetState() != EPackageState::Request)
	{
		bool bDemote = true;
		MapType::TConstIterator OldIter = PlatformDatas.CreateConstIterator();
		MapType::TConstIterator NewIter = NewPlatformDatas.CreateConstIterator();
		for (; OldIter; ++OldIter, ++NewIter)
		{
			if (OldIter.Key() != NewIter.Key())
			{
				bDemote = true;
			}
		}
		if (bDemote)
		{
			SendToState(EPackageState::Request, ESendFlags::QueueAddAndRemove);
		}
	}
	PlatformDatas = MoveTemp(NewPlatformDatas);
}

bool FPackageData::IsSaveInvalidated() const
{
	if (GetState() != EPackageState::Save)
	{
		return false;
	}

	return GetPackage() == nullptr || !GetPackage()->IsFullyLoaded() ||
		Algo::AnyOf(CachedObjectsInOuter, [](const FWeakObjectPtr& WeakPtr)
			{
				// TODO: Keep track of which objects were public, and only invalidate the save if the object
				// that has been deleted or marked pending kill was public
				// Until we make that change, we will unnecessarily invalidate and demote some packages after a
				// garbage collect
				return WeakPtr.Get() == nullptr;
			});
}

void FPackageData::SetGeneratedOwner(FGeneratorPackage* InGeneratedOwner)
{
	check(IsGenerated());
	check(!(GeneratedOwner && InGeneratedOwner));
	GeneratedOwner = InGeneratedOwner;
}

bool FPackageData::GeneratorPackageRequiresGC() const
{
	// We consider that if a FPackageData has valid GeneratorPackage helper object,
	// this means that COTFS's process of generating packages was not completed 
	// either due to an error or because it has exceeded a maximum memory threshold. 
	return IsGenerating() && !GetHasBeginPrepareSaveFailed();
}

UE::Cook::FGeneratorPackage* FPackageData::CreateGeneratorPackage(const UObject* InSplitDataObject,
	ICookPackageSplitter* InCookPackageSplitterInstance)
{
	check(!GetGeneratorPackage());
	GeneratorPackage.Reset(new UE::Cook::FGeneratorPackage(*this, InSplitDataObject,
		InCookPackageSplitterInstance));
	return GetGeneratorPackage();
}
	
//////////////////////////////////////////////////////////////////////////
// FGeneratorPackage

FGeneratorPackage::FGeneratorPackage(UE::Cook::FPackageData& InOwner, const UObject* InSplitDataObject,
	ICookPackageSplitter* InCookPackageSplitterInstance)
: Owner(InOwner)
, SplitDataObjectName(*InSplitDataObject->GetFullName())
{
	check(InCookPackageSplitterInstance);
	CookPackageSplitterInstance.Reset(InCookPackageSplitterInstance);
}

FGeneratorPackage::~FGeneratorPackage()
{
	ClearGeneratedPackages();
}

void FGeneratorPackage::ClearGeneratedPackages()
{
	for (FGeneratedStruct& GeneratedStruct : PackagesToGenerate)
	{
		if (GeneratedStruct.PackageData)
		{
			check(GeneratedStruct.PackageData->GetGeneratedOwner() == this);
			GeneratedStruct.PackageData->SetGeneratedOwner(nullptr);
			GeneratedStruct.PackageData = nullptr;
		}
	}
}

bool FGeneratorPackage::TryGenerateList(UObject* OwnerObject, FPackageDatas& PackageDatas)
{
	UPackage* OwnerPackage = Owner.GetPackage();
	check(OwnerPackage);
	TArray<ICookPackageSplitter::FGeneratedPackage> GeneratorDatas =
		CookPackageSplitterInstance->GetGenerateList(OwnerPackage, OwnerObject);
	PackagesToGenerate.Reset(GeneratorDatas.Num());
	for (ICookPackageSplitter::FGeneratedPackage& SplitterData : GeneratorDatas)
	{
		FGeneratedStruct& GeneratedStruct = PackagesToGenerate.Emplace_GetRef();
		GeneratedStruct.RelativePath = MoveTemp(SplitterData.RelativePath);
		GeneratedStruct.Dependencies = MoveTemp(SplitterData.Dependencies);
		FString PackageName = FPaths::RemoveDuplicateSlashes(FString::Printf(TEXT("%s/%s/%s"),
			*this->Owner.GetPackageName().ToString(), GeneratedPackageSubPath, *GeneratedStruct.RelativePath));

		if (!SplitterData.GetCreateAsMap().IsSet())
		{
			UE_LOG(LogCook, Error, TEXT("PackageSplitter did not specify whether CreateAsMap is true for generated package. Splitter=%s, Generated=%s."),
				*this->GetSplitDataObjectName().ToString(), *PackageName);
			return false;
		}
		GeneratedStruct.bCreateAsMap = *SplitterData.GetCreateAsMap();

		const FName PackageFName(*PackageName);
		UE::Cook::FPackageData* PackageData = PackageDatas.TryAddPackageDataByPackageName(PackageFName,
			false /* bRequireExists */, GeneratedStruct.bCreateAsMap);
		if (!PackageData)
		{
			UE_LOG(LogCook, Error, TEXT("PackageSplitter could not find mounted filename for generated packagepath. Splitter=%s, Generated=%s."),
				*this->GetSplitDataObjectName().ToString(), *PackageName);
			return false;
		}
		if (IFileManager::Get().FileExists(*PackageData->GetFileName().ToString()))
		{
			UE_LOG(LogCook, Warning, TEXT("PackageSplitter specified a generated package that already exists in the workspace domain. Splitter=%s, Generated=%s."),
				*this->GetSplitDataObjectName().ToString(), *PackageName);
			return false;
		}
		GeneratedStruct.PackageData = PackageData;
		PackageData->SetGenerated(true);
		// No package should be generated by two different splitters. If an earlier run of this splitter generated
		// the package, the package's owner should have been reset to null when we called ClearGeneratedPackages
		// between then and now
		check(PackageData->GetGeneratedOwner() == nullptr); 
		PackageData->SetGeneratedOwner(this);
	}
	RemainingToPopulate = GeneratorDatas.Num();
	return true;
}

FGeneratorPackage::FGeneratedStruct* FGeneratorPackage::FindGeneratedStruct(FPackageData* PackageData)
{
	for (FGeneratedStruct& GeneratedStruct : PackagesToGenerate)
	{
		if (GeneratedStruct.PackageData == PackageData)
		{
			return &GeneratedStruct;
		}
	}
	return nullptr;
}

UObject* FGeneratorPackage::FindSplitDataObject() const
{
	FString ObjectPath = GetSplitDataObjectName().ToString();

	// SplitDataObjectName is a FullObjectPath; strip off the leading <ClassName> in
	// "<ClassName> <Package>.<Object>:<SubObject>"
	int32 ClassDelimiterIndex = -1;
	if (ObjectPath.FindChar(' ', ClassDelimiterIndex))
	{
		ObjectPath.RightChopInline(ClassDelimiterIndex + 1);
	}
	return FindObject<UObject>(nullptr, *ObjectPath);
}

void FGeneratorPackage::PostGarbageCollect()
{
	if (!bGeneratedList)
	{
		return;
	}
	if (Owner.GetState() == EPackageState::Save)
	{
		// UCookOnTheFlyServer::PreCollectGarbage adds references for the Generator package and all its public
		// objects, so it should still be loaded
		if (!Owner.GetPackage() || !FindSplitDataObject())
		{
			UE_LOG(LogCook, Error, TEXT("PackageSplitter object was deleted by garbage collection while generation was still ongoing. This will break the generation.")
				TEXT("\n\tSplitter=%s."), *GetSplitDataObjectName().ToString());
		}
	}
	else
	{
		// After the Generator Package is saved, we drop its referenced and it can be garbage collected
		// If we have any packages left to populate, our splitter contract requires that it be garbage collected;
		// we promise that the package is not partially GC'd during calls to TryPopulateGeneratedPackage
		// The splitter can opt-out of this contract and keep it referenced itself if it desires.
		UPackage* OwnerPackage = FindObject<UPackage>(nullptr, *Owner.GetPackageName().ToString());
		if (OwnerPackage)
		{
			if (RemainingToPopulate > 0 &&
				!CookPackageSplitterInstance->UseInternalReferenceToAvoidGarbageCollect())
			{
				UE_LOG(LogCook, Error, TEXT("PackageSplitter found the Generator package still in memory after it should have been deleted by GC.")
					TEXT("\n\tThis is unexpected since garbage has been collected and the package should have been unreferenced so it should have been collected, and will break population of Generated packages.")
					TEXT("\n\tSplitter=%s"), *GetSplitDataObjectName().ToString());
				EReferenceChainSearchMode SearchMode = EReferenceChainSearchMode::Shortest
					| EReferenceChainSearchMode::PrintAllResults
					| EReferenceChainSearchMode::FullChain;
				FReferenceChainSearch RefChainSearch(OwnerPackage, SearchMode);
			}
		}
		else
		{
			bWasOwnerReloaded = true;
		}
	}

	bool bHasIssuedWarning = false;
	for (FGeneratedStruct& GeneratedStruct : PackagesToGenerate)
	{
		GeneratedStruct.bHasCreatedPackage = false;
		if (!GeneratedStruct.bHasSaved && !bHasIssuedWarning)
		{
			if (FindObject<UPackage>(nullptr, *GeneratedStruct.PackageData->GetPackageName().ToString()))
			{
				UE_LOG(LogCook, Warning, TEXT("PackageSplitter found a package it generated that was not removed from memory during garbage collection. This will cause errors later during population.")
					TEXT("\n\tSplitter=%s, Generated=%s."), *GetSplitDataObjectName().ToString(), *GeneratedStruct.PackageData->GetPackageName().ToString());
				bHasIssuedWarning = true; // Only issue the warning once per GC
			}
		}
	}
}

UPackage* FGeneratorPackage::CreateGeneratedUPackage(FGeneratorPackage::FGeneratedStruct& GeneratedStruct,
	const UPackage* OwnerPackage, const TCHAR* GeneratedPackageName)
{
	UPackage* GeneratedPackage = CreatePackage(GeneratedPackageName);
	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	GeneratedPackage->SetGuid(OwnerPackage->GetGuid());
	PRAGMA_ENABLE_DEPRECATION_WARNINGS
	GeneratedPackage->SetPersistentGuid(OwnerPackage->GetPersistentGuid());
	GeneratedStruct.bHasCreatedPackage = true;
	return GeneratedPackage;
}

void FGeneratorPackage::SetGeneratedSaved(FPackageData& PackageData)
{
	FGeneratedStruct* GeneratedStruct = FindGeneratedStruct(&PackageData);
	if (!GeneratedStruct)
	{
		UE_LOG(LogCook, Warning, TEXT("PackageSplitter called SetGeneratedSaved on a package that does not belong to the splitter.")
			TEXT("\n\tSplitter=%s, Generated=%s."), *GetSplitDataObjectName().ToString(),
			*PackageData.GetPackageName().ToString());
		return;
	}
	if (GeneratedStruct->bHasSaved)
	{
		return;
	}
	GeneratedStruct->bHasSaved = true;
	--RemainingToPopulate;
	check(RemainingToPopulate >= 0);
}

bool FGeneratorPackage::IsComplete() const
{
	return bGeneratedList && RemainingToPopulate == 0;
}

void FGeneratorPackage::GetIntermediateMountPoint(FString& OutPackagePath, FString& OutLocalFilePath) const
{
	const FString OwnerShortName = FPackageName::GetShortName(Owner.GetPackageName().ToString());
	OutPackagePath = FPaths::RemoveDuplicateSlashes(FString::Printf(TEXT("/%s%s/"),
		*OwnerShortName, UE::Cook::GeneratedPackageSubPath));
	OutLocalFilePath = FPaths::RemoveDuplicateSlashes(FString::Printf(TEXT("%s/Cooked/%s/%s/"),
		*FPaths::ProjectIntermediateDir(), *OwnerShortName, UE::Cook::GeneratedPackageSubPath));

}
FString FGeneratorPackage::GetIntermediateLocalPath(const FGeneratorPackage::FGeneratedStruct& GeneratedStruct) const
{
	FString UnusedPackagePath;
	FString MountLocalFilePath;
	GetIntermediateMountPoint(UnusedPackagePath, MountLocalFilePath);
	FString Extension = FPaths::GetExtension(GeneratedStruct.PackageData->GetFileName().ToString(),
		true /* bIncludeDot */);
	return FPaths::RemoveDuplicateSlashes(FString::Printf(TEXT("%s/%s%s"),
		*MountLocalFilePath, *GeneratedStruct.RelativePath, *Extension));
}

//////////////////////////////////////////////////////////////////////////
// FPendingCookedPlatformData


FPendingCookedPlatformData::FPendingCookedPlatformData(UObject* InObject, const ITargetPlatform* InTargetPlatform,
	FPackageData& InPackageData, bool bInNeedsResourceRelease, UCookOnTheFlyServer& InCookOnTheFlyServer)
	: Object(InObject), TargetPlatform(InTargetPlatform), PackageData(InPackageData)
	, CookOnTheFlyServer(InCookOnTheFlyServer),	CancelManager(nullptr), ClassName(InObject->GetClass()->GetFName())
	, bHasReleased(false), bNeedsResourceRelease(bInNeedsResourceRelease)
{
	check(InObject);
	PackageData.GetNumPendingCookedPlatformData() += 1;
}

FPendingCookedPlatformData::FPendingCookedPlatformData(FPendingCookedPlatformData&& Other)
	: Object(Other.Object), TargetPlatform(Other.TargetPlatform), PackageData(Other.PackageData)
	, CookOnTheFlyServer(Other.CookOnTheFlyServer), CancelManager(Other.CancelManager), ClassName(Other.ClassName)
	, bHasReleased(Other.bHasReleased), bNeedsResourceRelease(Other.bNeedsResourceRelease)
{
	Other.Object = nullptr;
}

FPendingCookedPlatformData::~FPendingCookedPlatformData()
{
	Release();
}

bool FPendingCookedPlatformData::PollIsComplete()
{
	if (bHasReleased)
	{
		return true;
	}

	UObject* LocalObject = Object.Get();
	if (!LocalObject)
	{
		Release();
		return true;
	}
	UE_TRACK_REFERENCING_PACKAGE_SCOPED(LocalObject->GetPackage(), PackageAccessTrackingOps::NAME_CookerBuildObject);
	if (LocalObject->IsCachedCookedPlatformDataLoaded(TargetPlatform))
	{
		Release();
		return true;
	}
	else
	{
#if DEBUG_COOKONTHEFLY
		UE_LOG(LogCook, Display, TEXT("Object %s isn't cached yet"), *LocalObject->GetFullName());
#endif
		/*if ( LocalObject->IsA(UMaterial::StaticClass()) )
		{
			if (GShaderCompilingManager->HasShaderJobs() == false)
			{
				UE_LOG(LogCook, Warning, TEXT("Shader compiler is in a bad state!  Shader %s is finished compile but shader compiling manager did not notify shader.  "),
					*LocalObject->GetPathName());
			}
		}*/
		return false;
	}
}

void FPendingCookedPlatformData::Release()
{
	if (bHasReleased)
	{
		return;
	}

	if (bNeedsResourceRelease)
	{
		int32* CurrentAsyncCache = CookOnTheFlyServer.CurrentAsyncCacheForType.Find(ClassName);
		// bNeedsRelease should not have been set if the AsyncCache does not have an entry for the class
		check(CurrentAsyncCache != nullptr);
		*CurrentAsyncCache += 1;
	}

	PackageData.GetNumPendingCookedPlatformData() -= 1;
	check(PackageData.GetNumPendingCookedPlatformData() >= 0);
	if (CancelManager)
	{
		CancelManager->Release(*this);
		CancelManager = nullptr;
	}

	Object = nullptr;
	bHasReleased = true;
}

void FPendingCookedPlatformData::RemapTargetPlatforms(const TMap<ITargetPlatform*, ITargetPlatform*>& Remap)
{
	TargetPlatform = Remap[TargetPlatform];
}


//////////////////////////////////////////////////////////////////////////
// FPendingCookedPlatformDataCancelManager


void FPendingCookedPlatformDataCancelManager::Release(FPendingCookedPlatformData& Data)
{
	--NumPendingPlatforms;
	if (NumPendingPlatforms <= 0)
	{
		check(NumPendingPlatforms == 0);
		UObject* LocalObject = Data.Object.Get();
		if (LocalObject)
		{
			LocalObject->ClearAllCachedCookedPlatformData();
		}
		delete this;
	}
}


//////////////////////////////////////////////////////////////////////////
// FPackageDataMonitor
FPackageDataMonitor::FPackageDataMonitor()
{
	FMemory::Memset(NumUrgentInState, 0);
}

int32 FPackageDataMonitor::GetNumUrgent() const
{
	int32 NumUrgent = 0;
	for (EPackageState State = EPackageState::Min;
		State <= EPackageState::Max;
		State = static_cast<EPackageState>(static_cast<uint32>(State) + 1))
	{
		NumUrgent += NumUrgentInState[static_cast<uint32>(State) - static_cast<uint32>(EPackageState::Min)];
	}
	return NumUrgent;
}

int32 FPackageDataMonitor::GetNumUrgent(EPackageState InState) const
{
	check(EPackageState::Min <= InState && InState <= EPackageState::Max);
	return NumUrgentInState[static_cast<uint32>(InState) - static_cast<uint32>(EPackageState::Min)];
}

int32 FPackageDataMonitor::GetNumPreloadAllocated() const
{
	return NumPreloadAllocated;
}

int32 FPackageDataMonitor::GetNumInProgress() const
{
	return NumInProgress;
}

int32 FPackageDataMonitor::GetNumCooked() const
{
	return NumCooked;
}

void FPackageDataMonitor::OnInProgressChanged(FPackageData& PackageData, bool bInProgress)
{
	NumInProgress += bInProgress ? 1 : -1;
	check(NumInProgress >= 0);
}

void FPackageDataMonitor::OnPreloadAllocatedChanged(FPackageData& PackageData, bool bPreloadAllocated)
{
	NumPreloadAllocated += bPreloadAllocated ? 1 : -1;
	check(NumPreloadAllocated >= 0);
}

void FPackageDataMonitor::OnFirstCookedPlatformAdded(FPackageData& PackageData)
{
	if (!PackageData.GetMonitorIsCooked())
	{
		++NumCooked;
		PackageData.SetMonitorIsCooked(true);
	}
}

void FPackageDataMonitor::OnLastCookedPlatformRemoved(FPackageData& PackageData)
{
	if (PackageData.GetMonitorIsCooked())
	{
		--NumCooked;
		PackageData.SetMonitorIsCooked(false);
	}
}

void FPackageDataMonitor::OnUrgencyChanged(FPackageData& PackageData)
{
	int32 Delta = PackageData.GetIsUrgent() ? 1 : -1;
	TrackUrgentRequests(PackageData.GetState(), Delta);
}

void FPackageDataMonitor::OnStateChanged(FPackageData& PackageData, EPackageState OldState)
{
	if (!PackageData.GetIsUrgent())
	{
		return;
	}

	TrackUrgentRequests(OldState, -1);
	TrackUrgentRequests(PackageData.GetState(), 1);
}

void FPackageDataMonitor::TrackUrgentRequests(EPackageState State, int32 Delta)
{
	check(EPackageState::Min <= State && State <= EPackageState::Max);
	NumUrgentInState[static_cast<uint32>(State) - static_cast<uint32>(EPackageState::Min)] += Delta;
	check(NumUrgentInState[static_cast<uint32>(State) - static_cast<uint32>(EPackageState::Min)] >= 0);
}


//////////////////////////////////////////////////////////////////////////
// FPackageDatas

IAssetRegistry* FPackageDatas::AssetRegistry = nullptr;

FPackageDatas::FPackageDatas(UCookOnTheFlyServer& InCookOnTheFlyServer)
	: CookOnTheFlyServer(InCookOnTheFlyServer)
	, LastPollAsyncTime(0)
{
}

void FPackageDatas::BeginCook()
{
	FString FileOrPackageName;
	ShowInstigatorPackageData = nullptr;
	if (FParse::Value(FCommandLine::Get(), TEXT("-CookShowInstigator="), FileOrPackageName))
	{
		FString LocalPath;
		FString PackageName;
		if (!FPackageName::TryConvertToMountedPath(FileOrPackageName, &LocalPath, &PackageName, nullptr, nullptr, nullptr))
		{
			UE_LOG(LogCook, Fatal, TEXT("-CookShowInstigator argument %s is not a mounted filename or packagename"),
				*FileOrPackageName);
		}
		else
		{
			FName PackageFName(*PackageName);
			ShowInstigatorPackageData = TryAddPackageDataByPackageName(PackageFName);
			if (!ShowInstigatorPackageData)
			{
				UE_LOG(LogCook, Fatal, TEXT("-CookShowInstigator argument %s could not be found on disk"),
					*FileOrPackageName);
			}
		}
	}
}

FPackageDatas::~FPackageDatas()
{
	Clear();
}

void FPackageDatas::OnAssetRegistryGenerated(IAssetRegistry& InAssetRegistry)
{
	AssetRegistry = &InAssetRegistry;
}

FString FPackageDatas::GetReferencerName() const
{
	return TEXT("FPackageDatas");
}

void FPackageDatas::AddReferencedObjects(FReferenceCollector& Collector)
{
	return CookOnTheFlyServer.CookerAddReferencedObjects(Collector);
}

FPackageDataMonitor& FPackageDatas::GetMonitor()
{
	return Monitor;
}

UCookOnTheFlyServer& FPackageDatas::GetCookOnTheFlyServer()
{
	return CookOnTheFlyServer;
}

FRequestQueue& FPackageDatas::GetRequestQueue()
{
	return RequestQueue;
}

FPackageDataQueue& FPackageDatas::GetSaveQueue()
{
	return SaveQueue;
}

FPackageData& FPackageDatas::FindOrAddPackageData(const FName& PackageName, const FName& NormalizedFileName)
{
	{
		FReadScopeLock ExistenceReadLock(ExistenceLock);
		FPackageData** PackageDataMapAddr = PackageNameToPackageData.Find(PackageName);
		if (PackageDataMapAddr != nullptr)
		{
			FPackageData** FileNameMapAddr = FileNameToPackageData.Find(NormalizedFileName);
			checkf(FileNameMapAddr, TEXT("Package %s is being added with filename %s, but it already exists with filename %s, ")
				TEXT("and it is not present in FileNameToPackageData map under the new name."),
				*PackageName.ToString(), *NormalizedFileName.ToString(), *(*PackageDataMapAddr)->GetFileName().ToString());
			checkf(*FileNameMapAddr == *PackageDataMapAddr,
				TEXT("Package %s is being added with filename %s, but that filename maps to a different package %s."),
				*PackageName.ToString(), *NormalizedFileName.ToString(), *(*FileNameMapAddr)->GetPackageName().ToString());
			return **PackageDataMapAddr;
		}

		checkf(FileNameToPackageData.Find(NormalizedFileName) == nullptr,
			TEXT("Package \"%s\" and package \"%s\" share the same filename \"%s\"."),
			*PackageName.ToString(), *(*FileNameToPackageData.Find(NormalizedFileName))->GetPackageName().ToString(),
			*NormalizedFileName.ToString());
	}
	return CreatePackageData(PackageName, NormalizedFileName);
}

FPackageData* FPackageDatas::FindPackageDataByPackageName(const FName& PackageName)
{
	if (PackageName.IsNone())
	{
		return nullptr;
	}

	FReadScopeLock ExistenceReadLock(ExistenceLock);
	FPackageData** PackageDataMapAddr = PackageNameToPackageData.Find(PackageName);
	return PackageDataMapAddr ? *PackageDataMapAddr : nullptr;
}

FPackageData* FPackageDatas::TryAddPackageDataByPackageName(const FName& PackageName, bool bRequireExists,
	bool bCreateAsMap)
{
	if (PackageName.IsNone())
	{
		return nullptr;
	}

	{
		FReadScopeLock ExistenceReadLock(ExistenceLock);
		FPackageData** PackageDataMapAddr = PackageNameToPackageData.Find(PackageName);
		if (PackageDataMapAddr != nullptr)
		{
			return *PackageDataMapAddr;
		}
	}

	FName FileName = LookupFileNameOnDisk(PackageName, bRequireExists, bCreateAsMap);
	if (FileName.IsNone())
	{
		// This will happen if PackageName does not exist on disk
		return nullptr;
	}
	{
		FReadScopeLock ExistenceReadLock(ExistenceLock);
		checkf(FileNameToPackageData.Find(FileName) == nullptr,
			TEXT("Package \"%s\" and package \"%s\" share the same filename \"%s\"."),
			*PackageName.ToString(), *(*FileNameToPackageData.Find(FileName))->GetPackageName().ToString(),
			*FileName.ToString());
	}
	return &CreatePackageData(PackageName, FileName);
}

FPackageData& FPackageDatas::AddPackageDataByPackageNameChecked(const FName& PackageName, bool bRequireExists,
	bool bCreateAsMap)
{
	FPackageData* PackageData = TryAddPackageDataByPackageName(PackageName, bRequireExists, bCreateAsMap);
	check(PackageData);
	return *PackageData;
}

FPackageData* FPackageDatas::FindPackageDataByFileName(const FName& InFileName)
{
	FName FileName(GetStandardFileName(InFileName));
	if (FileName.IsNone())
	{
		return nullptr;
	}

	FReadScopeLock ExistenceReadLock(ExistenceLock);
	FPackageData** PackageDataMapAddr = FileNameToPackageData.Find(FileName);
	return PackageDataMapAddr ? *PackageDataMapAddr : nullptr;
}

FPackageData* FPackageDatas::TryAddPackageDataByFileName(const FName& InFileName)
{
	return TryAddPackageDataByStandardFileName(GetStandardFileName(InFileName));
}

FPackageData* FPackageDatas::TryAddPackageDataByStandardFileName(const FName& FileName, bool bExactMatchRequired,
	FName* OutFoundFileName)
{
	FName FoundFileName = FileName;
	ON_SCOPE_EXIT{ if (OutFoundFileName) { *OutFoundFileName = FoundFileName; } };
	if (FileName.IsNone())
	{
		return nullptr;
	}

	{
		FReadScopeLock ExistenceReadLock(ExistenceLock);
		FPackageData** PackageDataMapAddr = FileNameToPackageData.Find(FileName);
		if (PackageDataMapAddr != nullptr)
		{
			return *PackageDataMapAddr;
		}
	}

	FName ExistingFileName;
	FName PackageName = LookupPackageNameOnDisk(FileName, bExactMatchRequired, ExistingFileName);
	if (PackageName.IsNone())
	{
		return nullptr;
	}
	if (ExistingFileName.IsNone())
	{
		if (!bExactMatchRequired)
		{
			FReadScopeLock ExistenceReadLock(ExistenceLock);
			FPackageData** PackageDataMapAddr = PackageNameToPackageData.Find(PackageName);
			if (PackageDataMapAddr != nullptr)
			{
				FoundFileName = (*PackageDataMapAddr)->GetFileName();
				return *PackageDataMapAddr;
			}
		}
		UE_LOG(LogCook, Warning, TEXT("Unexpected failure to cook filename '%s'. It is mapped to PackageName '%s', but does not exist on disk and we cannot verify the extension."),
			*FileName.ToString(), *PackageName.ToString());
		return nullptr;
	}
	FoundFileName = ExistingFileName;
	return &CreatePackageData(PackageName, ExistingFileName);
}

FPackageData& FPackageDatas::CreatePackageData(FName PackageName, FName FileName)
{
	check(!PackageName.IsNone());
	check(!FileName.IsNone());
	FPackageData* PackageData = new FPackageData(*this, PackageName, FileName);

	FWriteScopeLock ExistenceWriteLock(ExistenceLock);
	FPackageData*& ExistingByPackageName = PackageNameToPackageData.FindOrAdd(PackageName);
	FPackageData*& ExistingByFileName = FileNameToPackageData.FindOrAdd(FileName);
	if (ExistingByPackageName)
	{
		// The other CreatePackageData call should have added the FileName as well
		check(ExistingByFileName == ExistingByPackageName);
		delete PackageData;
		return *ExistingByPackageName;
	}
	// If no other CreatePackageData added the PackageName, then they should not have added
	// the FileName either
	check(!ExistingByFileName);
	ExistingByPackageName = PackageData;
	ExistingByFileName = PackageData;
	PackageDatas.Add(PackageData);
	return *PackageData;
}

FPackageData& FPackageDatas::AddPackageDataByFileNameChecked(const FName& FileName)
{
	FPackageData* PackageData = TryAddPackageDataByFileName(FileName);
	check(PackageData);
	return *PackageData;
}

FName FPackageDatas::GetFileNameByPackageName(FName PackageName, bool bRequireExists, bool bCreateAsMap)
{
	FPackageData* PackageData = TryAddPackageDataByPackageName(PackageName, bRequireExists, bCreateAsMap);
	return PackageData ? PackageData->GetFileName() : NAME_None;
}

FName FPackageDatas::GetFileNameByFlexName(FName PackageOrFileName, bool bRequireExists, bool bCreateAsMap)
{
	FString Buffer = PackageOrFileName.ToString();
	if (!FPackageName::TryConvertFilenameToLongPackageName(Buffer, Buffer))
	{
		return NAME_None;
	}
	return GetFileNameByPackageName(FName(Buffer), bRequireExists, bCreateAsMap);
}

FName FPackageDatas::LookupFileNameOnDisk(FName PackageName, bool bRequireExists, bool bCreateAsMap)
{
	FString FilenameOnDisk;
	if (TryLookupFileNameOnDisk(PackageName, FilenameOnDisk))
	{
	}
	else if (!bRequireExists)
	{
		FString Extension = bCreateAsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName.ToString(), FilenameOnDisk, Extension))
		{
			return NAME_None;
		}
	}
	else
	{
		return NAME_None;
	}
	FilenameOnDisk = FPaths::ConvertRelativePathToFull(FilenameOnDisk);
	FPaths::MakeStandardFilename(FilenameOnDisk);
	return FName(FilenameOnDisk);
}

bool FPackageDatas::TryLookupFileNameOnDisk(FName PackageName, FString& OutFileName)
{
	FString PackageNameStr = PackageName.ToString();

	// Verse packages are editor-generated in-memory packages which don't have a corresponding 
	// asset file (yet). However, we still want to cook these packages out, producing cooked 
	// asset files for packaged projects.
	if (FPackageName::IsVersePackage(PackageNameStr))
	{
		if (FindPackage(/*Outer =*/nullptr, *PackageNameStr))
		{
			return FPackageName::TryConvertLongPackageNameToFilename(PackageNameStr, OutFileName,
				FPackageName::GetAssetPackageExtension());
		}
		// else, the cooker could be responding to a NotifyUObjectCreated() event, and the object hasn't
		// been fully constructed yet (missing from the FindObject() list) -- in this case, we've found 
		// that the linker loader is creating a dummy object to fill a referencing import slot, not loading
		// the proper object (which means we want to ignore it).
	}

	if (!AssetRegistry)
	{
		return FPackageName::DoesPackageExist(PackageNameStr, &OutFileName, false /* InAllowTextFormats */);
	}
	else
	{
		TArray<FAssetData> Assets;
		AssetRegistry->GetAssetsByPackageName(PackageName, Assets, /*bIncludeOnlyDiskAssets =*/true);

		if (Assets.Num() <= 0)
		{
			if (FPackageName::DoesPackageExist(PackageNameStr, &OutFileName, false /* InAllowTextFormats */))
			{
				if (AssetRegistry->GetAssetPackageDataCopy(PackageName))
				{
					// The AssetRegistry knows the package exists, but it has 0 assets.
				}
				else
				{
					UE_LOG(LogCook, Warning, TEXT("Package %s exists on disk but does not exist in the AssetRegistry"), *PackageNameStr);
				}
				return true;
			}
			return false;
		}
		else
		{
			// Temporary fix for packages added during cook: GetAssetsByPackageName is returning true for these generated
			// files that do not exist on disk, since they get added into the AssetRegistryState in UAssetRegistryImpl::ProcessLoadedAssetsToUpdateCache
			// The current known cases where this is a problem is when cooking WorldPartition maps, which create temporary World packages in
			// //Temp. So for /Temp files, use the slower disk check rather than the AssetRegistry.
			bool bForceDiskCheck = PackageNameStr.StartsWith(TEXT("/Temp/"));
			if (bForceDiskCheck)
			{
				if (!FPackageName::DoesPackageExist(PackageNameStr, &OutFileName, false /* InAllowTextFormats */))
				{
					return false;
				}
				return true;
			}
		}

		FName ClassRedirector = UObjectRedirector::StaticClass()->GetFName();
		bool bContainsMap = false;
		bool bContainsRedirector = false;
		for (const FAssetData& Asset : Assets)
		{
			bContainsMap = bContainsMap | ((Asset.PackageFlags & PKG_ContainsMap) != 0);
			bContainsRedirector = bContainsRedirector | (Asset.AssetClass == ClassRedirector);
		}
		if (!bContainsMap && bContainsRedirector)
		{
			// presence of map -> .umap
			// But we can only assume lack of map -> .uasset if we know the type of every object in the package.
			// If we don't, because there was a redirector, we have to check the package on disk
			// TODO: Have the AssetRegistry store the extension of the package so that we don't have to look it up
			// Guessing the extension based on map vs non-map also does not support text assets and maps which have a different extension
			return FPackageName::DoesPackageExist(PackageNameStr, &OutFileName, false /* InAllowTextFormats */);
		}
		const FString& PackageExtension = bContainsMap ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension();
		return FPackageName::TryConvertLongPackageNameToFilename(PackageNameStr, OutFileName, PackageExtension);
	}
}

FName FPackageDatas::LookupPackageNameOnDisk(FName NormalizedFileName, bool bExactMatchRequired, FName& FoundFileName)
{
	FoundFileName = NormalizedFileName;
	if (NormalizedFileName.IsNone())
	{
		return NAME_None;
	}
	FString Buffer = NormalizedFileName.ToString();
	if (!FPackageName::TryConvertFilenameToLongPackageName(Buffer, Buffer))
	{
		return NAME_None;
	}
	FName PackageName = FName(*Buffer);

	FName DiscoveredFileName = LookupFileNameOnDisk(PackageName, true /* bRequireExists */, false /* bCreateAsMap */);
	if (DiscoveredFileName == NormalizedFileName || !bExactMatchRequired)
	{
		FoundFileName = DiscoveredFileName;
		return PackageName;
	}
	else
	{
		// Either the file does not exist on disk or NormalizedFileName did not match its format or extension
		return NAME_None;
	}
}

FName FPackageDatas::GetStandardFileName(FName FileName)
{
	FString FileNameString(FileName.ToString());
	FPaths::MakeStandardFilename(FileNameString);
	return FName(FileNameString);
}

FName FPackageDatas::GetStandardFileName(FStringView InFileName)
{
	FString FileName(InFileName);
	FPaths::MakeStandardFilename(FileName);
	return FName(FileName);
}

void FPackageDatas::AddExistingPackageDatasForPlatform(TConstArrayView<FConstructPackageData> ExistingPackages,
	const ITargetPlatform* TargetPlatform)
{
	int32 NumPackages = ExistingPackages.Num();

	// Make the list unique
	TMap<FName, FName> UniquePackages;
	UniquePackages.Reserve(NumPackages);
	for (const FConstructPackageData& PackageToAdd : ExistingPackages)
	{
		FName& AddedFileName = UniquePackages.FindOrAdd(PackageToAdd.PackageName, PackageToAdd.NormalizedFileName);
		check(AddedFileName == PackageToAdd.NormalizedFileName);
	}
	TArray<FConstructPackageData> UniqueArray;
	if (UniquePackages.Num() != NumPackages)
	{
		NumPackages = UniquePackages.Num();
		UniqueArray.Reserve(NumPackages);
		for (TPair<FName, FName>& Pair : UniquePackages)
		{
			UniqueArray.Add(FConstructPackageData{ Pair.Key, Pair.Value });
		}
		ExistingPackages = UniqueArray;
	}

	// parallelize the read-only operations (and write NewPackageDataObjects by index which has no threading issues)
	TArray<FPackageData*> NewPackageDataObjects;
	NewPackageDataObjects.AddZeroed(NumPackages);
	FWriteScopeLock ExistenceWriteLock(ExistenceLock);
	ParallelFor(NumPackages,
		[&ExistingPackages, TargetPlatform, &NewPackageDataObjects, this](int Index)
	{
		FName PackageName = ExistingPackages[Index].PackageName;
		FName NormalizedFileName = ExistingPackages[Index].NormalizedFileName;
		check(!PackageName.IsNone());
		check(!NormalizedFileName.IsNone());

		FPackageData** PackageDataMapAddr = FileNameToPackageData.Find(NormalizedFileName);
		FPackageData* PackageData = nullptr;
		if (PackageDataMapAddr != nullptr)
		{
			PackageData = *PackageDataMapAddr;
		}
		else
		{
			// create the package data and remember it for updating caches after the the ParallelFor
			PackageData = new FPackageData(*this, PackageName, NormalizedFileName);
			NewPackageDataObjects[Index] = PackageData;
		}
		PackageData->SetPlatformCooked(TargetPlatform, true /* Succeeded */);
	});

	// update cache for all newly created objects (copied from CreatePackageData)
	for (FPackageData* PackageData : NewPackageDataObjects)
	{
		if (PackageData)
		{
			FPackageData* ExistingByFileName = FileNameToPackageData.Add(PackageData->FileName, PackageData);
			// We looked up by FileName in the loop; it should still have been unset before the write we just did
			check(ExistingByFileName == PackageData);
			FPackageData* ExistingByPackageName = PackageNameToPackageData.FindOrAdd(PackageData->PackageName, PackageData);
			// If no other CreatePackageData added the FileName, then they should not have added
			// the PackageName either
			check(ExistingByPackageName == PackageData);
			PackageDatas.Add(PackageData);
		}
	}
}

FPackageData* FPackageDatas::UpdateFileName(FName PackageName)
{
	FWriteScopeLock ExistenceWriteLock(ExistenceLock);

	FPackageData** PackageDataAddr = PackageNameToPackageData.Find(PackageName);
	if (!PackageDataAddr)
	{
		FName NewFileName = LookupFileNameOnDisk(PackageName);
		check(NewFileName.IsNone() || !FileNameToPackageData.Find(NewFileName));
		return nullptr;
	}
	FPackageData* PackageData = *PackageDataAddr;
	FName OldFileName = PackageData->GetFileName();
	bool bIsMap = FPackageName::IsMapPackageExtension(*FPaths::GetExtension(OldFileName.ToString()));
	FName NewFileName = LookupFileNameOnDisk(PackageName, false /* bRequireExists */, bIsMap);
	if (OldFileName == NewFileName)
	{
		return PackageData;
	}
	if (NewFileName.IsNone())
	{
		UE_LOG(LogCook, Error, TEXT("Cannot update FileName for package %s because the package is no longer mounted."),
			*PackageName.ToString())
			return PackageData;
	}

	check(!OldFileName.IsNone());
	FPackageData* ExistingByFileName;
	ensure(FileNameToPackageData.RemoveAndCopyValue(OldFileName, ExistingByFileName));
	check(ExistingByFileName == PackageData);

	PackageData->SetFileName(NewFileName);
	FPackageData* AddedByFileName = FileNameToPackageData.FindOrAdd(NewFileName, PackageData);
	check(AddedByFileName == PackageData);

	return PackageData;
}

int32 FPackageDatas::GetNumCooked()
{
	return Monitor.GetNumCooked();
}

void FPackageDatas::GetCookedPackagesForPlatform(const ITargetPlatform* Platform, TArray<FPackageData*>& CookedPackages,
	bool bGetFailedCookedPackages, bool bGetSuccessfulCookedPackages)
{
	for (FPackageData* PackageData : PackageDatas)
	{
		ECookResult CookResults = PackageData->GetCookResults(Platform);
		if (((CookResults == ECookResult::Succeeded) & (bGetSuccessfulCookedPackages != 0)) |
			((CookResults == ECookResult::Failed) & (bGetFailedCookedPackages != 0)))
		{
			CookedPackages.Add(PackageData);
		}
	}
}

void FPackageDatas::Clear()
{
	FWriteScopeLock ExistenceWriteLock(ExistenceLock);
	PendingCookedPlatformDatas.Empty(); // These destructors will dereference PackageDatas
	RequestQueue.Empty();
	SaveQueue.Empty();
	PackageNameToPackageData.Empty();
	FileNameToPackageData.Empty();
	for (FPackageData* PackageData : PackageDatas)
	{
		PackageData->ClearReferences();
	}
	for (FPackageData* PackageData : PackageDatas)
	{
		delete PackageData;
	}
	PackageDatas.Empty();
	ShowInstigatorPackageData = nullptr;
}

void FPackageDatas::ClearCookedPlatforms()
{
	for (FPackageData* PackageData : PackageDatas)
	{
		PackageData->SetPlatformsNotCooked();
	}
}

void FPackageDatas::OnRemoveSessionPlatform(const ITargetPlatform* TargetPlatform)
{
	for (FPackageData* PackageData : PackageDatas)
	{
		PackageData->OnRemoveSessionPlatform(TargetPlatform);
	}
}

TArray<FPendingCookedPlatformData>& FPackageDatas::GetPendingCookedPlatformDatas()
{
	return PendingCookedPlatformDatas;
}

void FPackageDatas::PollPendingCookedPlatformDatas()
{
	if (PendingCookedPlatformDatas.Num() == 0)
	{
		return;
	}

	// ProcessAsyncResults and IsCachedCookedPlatformDataLoaded can be expensive to call
	// Cap the frequency at which we call them.
	double CurrentTime = FPlatformTime::Seconds();
	if (CurrentTime < LastPollAsyncTime + GPollAsyncPeriod)
	{
		return;
	}
	LastPollAsyncTime = CurrentTime;

	GShaderCompilingManager->ProcessAsyncResults(true /* bLimitExecutionTime */,
		false /* bBlockOnGlobalShaderCompletion */);
	FAssetCompilingManager::Get().ProcessAsyncTasks(true);

	FPendingCookedPlatformData* Datas = PendingCookedPlatformDatas.GetData();
	for (int Index = 0; Index < PendingCookedPlatformDatas.Num();)
	{
		if (Datas[Index].PollIsComplete())
		{
			PendingCookedPlatformDatas.RemoveAtSwap(Index, 1 /* Count */, false /* bAllowShrinking */);
		}
		else
		{
			++Index;
		}
	}
}

TArray<FPackageData*>::RangedForIteratorType FPackageDatas::begin()
{
	return PackageDatas.begin();
}

TArray<FPackageData*>::RangedForIteratorType FPackageDatas::end()
{
	return PackageDatas.end();
}

void FPackageDatas::RemapTargetPlatforms(const TMap<ITargetPlatform*, ITargetPlatform*>& Remap)
{
	for (FPackageData* PackageData : PackageDatas)
	{
		PackageData->RemapTargetPlatforms(Remap);
	}
	for (FPendingCookedPlatformData& CookedPlatformData : PendingCookedPlatformDatas)
	{
		CookedPlatformData.RemapTargetPlatforms(Remap);
	}
}

void FPackageDatas::DebugInstigator(FPackageData& PackageData)
{
	if (ShowInstigatorPackageData != &PackageData)
	{
		return;
	}

	TArray<FInstigator> Chain = CookOnTheFlyServer.GetInstigatorChain(PackageData.GetPackageName());
	TStringBuilder<256> ChainText;
	if (Chain.Num() == 0)
	{
		ChainText << TEXT("<NoInstigator>");
	}
	bool bFirst = true;
	for (FInstigator& Instigator : Chain)
	{
		if (!bFirst) ChainText << TEXT(" <- ");
		ChainText << TEXT("{ ") << Instigator.ToString() << TEXT(" }");
		bFirst = false;
	}
	UE_LOG(LogCook, Display, TEXT("Instigator chain of %s: %s"), *PackageData.GetPackageName().ToString(), ChainText.ToString());
}

void FRequestQueue::Empty()
{
	NormalRequests.Empty();
	UrgentRequests.Empty();
}

bool FRequestQueue::IsEmpty() const
{
	return Num() == 0;
}

uint32 FRequestQueue::Num() const
{
	uint32 Count = UnclusteredRequests.Num() + ReadyRequestsNum();
	for (const FRequestCluster& RequestCluster : RequestClusters)
	{
		Count += RequestCluster.NumPackageDatas();
	}
	return Count;
}

bool FRequestQueue::Contains(const FPackageData* InPackageData) const
{
	FPackageData* PackageData = const_cast<FPackageData*>(InPackageData);
	if (UnclusteredRequests.Contains(PackageData) || NormalRequests.Contains(PackageData) || UrgentRequests.Contains(PackageData))
	{
		return true;
	}
	for (const FRequestCluster& RequestCluster : RequestClusters)
	{
		if (RequestCluster.Contains(PackageData))
		{
			return true;
		}
	}
	return false;
}

uint32 FRequestQueue::RemoveRequest(FPackageData* PackageData)
{
	uint32 OriginalNum = Num();
	UnclusteredRequests.Remove(PackageData);
	NormalRequests.Remove(PackageData);
	UrgentRequests.Remove(PackageData);
	for (FRequestCluster& RequestCluster : RequestClusters)
	{
		RequestCluster.RemovePackageData(PackageData);
	}
	uint32 Result = OriginalNum - Num();
	check(Result == 0 || Result == 1);
	return Result;
}

uint32 FRequestQueue::Remove(FPackageData* PackageData)
{
	return RemoveRequest(PackageData);
}

bool FRequestQueue::IsReadyRequestsEmpty() const
{
	return ReadyRequestsNum() == 0;
}

uint32 FRequestQueue::ReadyRequestsNum() const
{
	return UrgentRequests.Num() + NormalRequests.Num();
}

FPackageData* FRequestQueue::PopReadyRequest()
{
	for (FPackageData* PackageData : UrgentRequests)
	{
		UrgentRequests.Remove(PackageData);
		return PackageData;
	}
	for (FPackageData* PackageData : NormalRequests)
	{
		NormalRequests.Remove(PackageData);
		return PackageData;
	}
	return nullptr;
}

void FRequestQueue::AddRequest(FPackageData* PackageData, bool bForceUrgent)
{
	if (!PackageData->AreAllRequestedPlatformsExplored())
	{
		UnclusteredRequests.Add(PackageData);
	}
	else
	{
		AddReadyRequest(PackageData, bForceUrgent);
	}
}

void FRequestQueue::AddReadyRequest(FPackageData* PackageData, bool bForceUrgent)
{
	if (bForceUrgent || PackageData->GetIsUrgent())
	{
		UrgentRequests.Add(PackageData);
	}
	else
	{
		NormalRequests.Add(PackageData);
	}
}

bool FLoadPrepareQueue::IsEmpty()
{
	return Num() == 0;
}

int32 FLoadPrepareQueue::Num() const
{
	return PreloadingQueue.Num() + EntryQueue.Num();
}

FPackageData* FLoadPrepareQueue::PopFront()
{
	if (!PreloadingQueue.IsEmpty())
	{
		return PreloadingQueue.PopFrontValue();
	}
	else
	{
		return EntryQueue.PopFrontValue();
	}
}

void FLoadPrepareQueue::Add(FPackageData* PackageData)
{
	EntryQueue.Add(PackageData);
}

void FLoadPrepareQueue::AddFront(FPackageData* PackageData)
{
	PreloadingQueue.AddFront(PackageData);
}

bool FLoadPrepareQueue::Contains(const FPackageData* PackageData) const
{
	return (Algo::Find(PreloadingQueue, PackageData) != nullptr) ||
		(Algo::Find(EntryQueue, PackageData) != nullptr);
}

uint32 FLoadPrepareQueue::Remove(FPackageData* PackageData)
{
	return PreloadingQueue.Remove(PackageData) + EntryQueue.Remove(PackageData);
}

FPoppedPackageDataScope::FPoppedPackageDataScope(FPackageData& InPackageData)
#if COOK_CHECKSLOW_PACKAGEDATA
	: PackageData(InPackageData)
#endif
{
}

#if COOK_CHECKSLOW_PACKAGEDATA
FPoppedPackageDataScope::~FPoppedPackageDataScope()
{
	PackageData.CheckInContainer();
}
#endif

}
