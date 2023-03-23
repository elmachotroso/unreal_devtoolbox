// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	CookOnTheFlyServer.cpp: handles polite cook requests via network ;)
=============================================================================*/

#include "CookOnTheSide/CookOnTheFlyServer.h"

#include "Algo/AnyOf.h"
#include "Algo/Find.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetRegistryState.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "Commandlets/AssetRegistryGenerator.h"
#include "Commandlets/ShaderPipelineCacheToolsCommandlet.h"
#include "Containers/RingBuffer.h"
#include "Cooker/AsyncIODelete.h"
#include "Cooker/CookedSavePackageValidator.h"
#include "Cooker/CookOnTheFlyServerInterface.h"
#include "Cooker/CookPackageData.h"
#include "Cooker/CookPlatformManager.h"
#include "Cooker/CookProfiling.h"
#include "Cooker/CookRequestCluster.h"
#include "Cooker/CookRequests.h"
#include "Cooker/CookTypes.h"
#include "Cooker/DiffPackageWriter.h"
#include "Cooker/IoStoreCookOnTheFlyRequestManager.h"
#include "Cooker/LooseCookedPackageWriter.h"
#include "Cooker/NetworkFileCookOnTheFlyRequestManager.h"
#include "Cooker/PackageTracker.h"
#include "CookerSettings.h"
#include "CookPackageSplitter.h"
#include "DerivedDataCacheInterface.h"
#include "DistanceFieldAtlas.h"
#include "Editor.h"
#include "Editor/UnrealEdEngine.h"
#include "EditorDomain/EditorDomain.h"
#include "Engine/AssetManager.h"
#include "Engine/Level.h"
#include "Engine/LevelStreaming.h"
#include "Engine/Texture.h"
#include "Engine/TextureLODSettings.h"
#include "Engine/WorldComposition.h"
#include "EngineGlobals.h"
#include "FileServerMessages.h"
#include "GameDelegates.h"
#include "GlobalShader.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "IMessageContext.h"
#include "INetworkFileServer.h"
#include "INetworkFileSystemModule.h"
#include "Interfaces/IAudioFormat.h"
#include "Interfaces/IPluginManager.h"
#include "Interfaces/IProjectManager.h"
#include "Interfaces/IShaderFormat.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITextureFormat.h"
#include "Internationalization/Culture.h"
#include "Internationalization/PackageLocalizationManager.h"
#include "IPAddress.h"
#include "LocalizationChunkDataGenerator.h"
#include "Logging/MessageLog.h"
#include "Logging/TokenizedMessage.h"
#include "Materials/Material.h"
#include "Materials/MaterialInterface.h"
#include "MeshCardRepresentation.h"
#include "MessageEndpoint.h"
#include "MessageEndpointBuilder.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/LocalTimestampDirectoryVisitor.h"
#include "Misc/NetworkVersion.h"
#include "Misc/PackageAccessTrackingOps.h"
#include "Misc/PackageName.h"
#include "Misc/PathViews.h"
#include "Misc/RedirectCollector.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "PackageHelperFunctions.h"
#include "PlatformInfo.h"
#include "ProfilingDebugging/CookStats.h"
#include "ProfilingDebugging/PlatformFileTrace.h"
#include "ProjectDescriptor.h"
#include "SceneUtils.h"
#include "Serialization/ArchiveStackTrace.h"
#include "Serialization/ArchiveUObject.h"
#include "Serialization/ArrayReader.h"
#include "Serialization/ArrayWriter.h"
#include "Serialization/CustomVersion.h"
#include "Settings/LevelEditorPlaySettings.h"
#include "Settings/ProjectPackagingSettings.h"
#include "ShaderCodeLibrary.h"
#include "ShaderCompiler.h"
#include "ShaderLibraryChunkDataGenerator.h"
#include "String/Find.h"
#include "String/ParseLines.h"
#include "TargetDomain/TargetDomainUtils.h"
#include "UnrealEdGlobals.h"
#include "UObject/Class.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/GarbageCollection.h"
#include "UObject/MetaData.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/Package.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/SavePackage.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectIterator.h"
#include "ZenStoreWriter.h"

#define LOCTEXT_NAMESPACE "Cooker"

DEFINE_LOG_CATEGORY(LogCook);

int32 GCookProgressDisplay = (int32)ECookProgressDisplayMode::RemainingPackages;
static FAutoConsoleVariableRef CVarCookDisplayMode(
	TEXT("cook.displaymode"),
	GCookProgressDisplay,
	TEXT("Controls the display for cooker logging of packages:\n")
	TEXT("  0: No display\n")
	TEXT("  1: Display the Count of packages remaining\n")
	TEXT("  2: Display each package by Name\n")
	TEXT("  3: Display Names and Count\n")
	TEXT("  4: Display the Instigator of each package\n")
	TEXT("  5: Display Instigators and Count\n")
	TEXT("  6: Display Instigators and Names\n")
	TEXT("  7: Display Instigators and Names and Count\n"),
	ECVF_Default);

float GCookProgressUpdateTime = 2.0f;
static FAutoConsoleVariableRef CVarCookDisplayUpdateTime(
	TEXT("cook.display.updatetime"),
	GCookProgressUpdateTime,
	TEXT("Controls the time before the cooker will send a new progress message.\n"),
	ECVF_Default);

float GCookProgressDiagnosticTime = 30.0f;
static FAutoConsoleVariableRef CVarCookDisplayDiagnosticTime(
	TEXT("Cook.display.diagnostictime"),
	GCookProgressDiagnosticTime,
	TEXT("Controls the time between cooker diagnostics messages.\n"),
	ECVF_Default);

float GCookProgressRepeatTime = 5.0f;
static FAutoConsoleVariableRef CVarCookDisplayRepeatTime(
	TEXT("cook.display.repeattime"),
	GCookProgressRepeatTime,
	TEXT("Controls the time before the cooker will repeat the same progress message.\n"),
	ECVF_Default);

float GCookProgressRetryBusyTime = 1.0f;
static FAutoConsoleVariableRef CVarCookRetryBusyTime(
	TEXT("Cook.retrybusytime"),
	GCookProgressRetryBusyTime,
	TEXT("Controls the time between retry attempts at save and load when the save and load queues are busy.\n"),
	ECVF_Default);

float GCookProgressWarnBusyTime = 120.0f;
static FAutoConsoleVariableRef CVarCookDisplayWarnBusyTime(
	TEXT("Cook.display.warnbusytime"),
	GCookProgressWarnBusyTime,
	TEXT("Controls the time before the cooker will issue a warning that there is a deadlock in a busy queue.\n"),
	ECVF_Default);

////////////////////////////////////////////////////////////////
/// Cook on the fly server
///////////////////////////////////////////////////////////////

constexpr float TickCookableObjectsFrameTime = .100f;

namespace UE
{
namespace Cook
{
const TCHAR* GeneratedPackageSubPath = TEXT("_Generated_");
}
}

/* helper structs functions
 *****************************************************************************/

/**
 * Return the release asset registry filename for the release version supplied
 */
static FString GetReleaseVersionAssetRegistryPath(const FString& ReleaseVersion, const FString& PlatformName, const FString& RootOverride)
{
	// cache the part of the path which is static because getting the ProjectDir is really slow and also string manipulation
	const FString* ReleasesRoot;
	if (RootOverride.IsEmpty())
	{
		const static FString DefaultReleasesRoot = FPaths::ProjectDir() / FString(TEXT("Releases"));
		ReleasesRoot = &DefaultReleasesRoot;
	}
	else
	{
		ReleasesRoot = &RootOverride;
	}
	return (*ReleasesRoot) / ReleaseVersion / PlatformName;
}

template<typename T>
struct FOneTimeCommandlineReader
{
	T Value;
	FOneTimeCommandlineReader(const TCHAR* Match)
	{
		FParse::Value(FCommandLine::Get(), Match, Value);
	}
};

static FString GetCreateReleaseVersionAssetRegistryPath(const FString& ReleaseVersion, const FString& PlatformName)
{
	static FOneTimeCommandlineReader<FString> CreateReleaseVersionRoot(TEXT("-createreleaseversionroot="));
	return GetReleaseVersionAssetRegistryPath(ReleaseVersion, PlatformName, CreateReleaseVersionRoot.Value);
}

static FString GetBasedOnReleaseVersionAssetRegistryPath(const FString& ReleaseVersion, const FString& PlatformName)
{
	static FOneTimeCommandlineReader<FString> BasedOnReleaseVersionRoot(TEXT("-basedonreleaseversionroot="));
	return GetReleaseVersionAssetRegistryPath(ReleaseVersion, PlatformName, BasedOnReleaseVersionRoot.Value);
}

const FString& GetAssetRegistryFilename()
{
	static const FString AssetRegistryFilename = FString(TEXT("AssetRegistry.bin"));
	return AssetRegistryFilename;
}

/**
 * Uses the FMessageLog to log a message
 * 
 * @param Message to log
 * @param Severity of the message
 */
void LogCookerMessage( const FString& MessageText, EMessageSeverity::Type Severity)
{
	FMessageLog MessageLog("LogCook");

	TSharedRef<FTokenizedMessage> Message = FTokenizedMessage::Create(Severity);

	Message->AddToken( FTextToken::Create( FText::FromString(MessageText) ) );
	// Message->AddToken(FTextToken::Create(MessageLogTextDetail)); 
	// Message->AddToken(FDocumentationToken::Create(TEXT("https://docs.unrealengine.com/latest/INT/Platforms/iOS/QuickStart/6/index.html"))); 
	MessageLog.AddMessage(Message);

	MessageLog.Notify(FText(), EMessageSeverity::Warning, false);
}

//////////////////////////////////////////////////////////////////////////
// Cook on the fly server interface adapter

class UCookOnTheFlyServer::FCookOnTheFlyServerInterface final
	: public UE::Cook::ICookOnTheFlyServer
{
public:
	FCookOnTheFlyServerInterface(UCookOnTheFlyServer& InCooker)
		: Cooker(InCooker)
		, FlushCompletedEvent(FPlatformProcess::GetSynchEventFromPool(true))
	{
		FlushCompletedEvent->Trigger();
	}

	virtual ~FCookOnTheFlyServerInterface()
	{
		FPlatformProcess::ReturnSynchEventToPool(FlushCompletedEvent);
	}

	virtual FString GetSandboxDirectory() const override
	{
		return Cooker.SandboxFile->GetSandboxDirectory();
	}

	virtual const ITargetPlatform* AddPlatform(const FName& PlatformName) override
	{
		const ITargetPlatform* Result;
		{
			UE::Cook::FPlatformManager::FReadScopeLock PlatformScopeLock(Cooker.PlatformManager->ReadLockPlatforms());
			Result = Cooker.AddCookOnTheFlyPlatform(PlatformName);
			if (!Result)
			{
				UE_LOG(LogCook, Warning, TEXT("Trying to add invalid platform '%s' on the fly"), *PlatformName.ToString());
				return nullptr;
			}

			Cooker.PlatformManager->AddRefCookOnTheFlyPlatform(PlatformName, Cooker);
		}

		return Result;
	}

	virtual void RemovePlatform(const FName& PlatformName) override
	{
		UE::Cook::FPlatformManager::FReadScopeLock PlatformScopeLock(Cooker.PlatformManager->ReadLockPlatforms());
		Cooker.PlatformManager->ReleaseCookOnTheFlyPlatform(PlatformName);
	}

	virtual void GetUnsolicitedFiles(const FName& PlatformName, const FString& Filename, const bool bIsCookable, TArray<FString>& OutUnsolicitedFiles) override
	{
		while (true)
		{
			{
				UE::Cook::FPlatformManager::FReadScopeLock PlatformsScopeLock(Cooker.PlatformManager->ReadLockPlatforms());
				const ITargetPlatform* TargetPlatform = Cooker.AddCookOnTheFlyPlatform(PlatformName);
				if (!TargetPlatform)
				{
					break;
				}
				if (Cooker.PlatformManager->IsPlatformInitialized(TargetPlatform))
				{
					Cooker.GetCookOnTheFlyUnsolicitedFiles(TargetPlatform, PlatformName.ToString(), OutUnsolicitedFiles, Filename, bIsCookable);
					break;
				}
			}
			// Wait for the Platform to be added if this is the first time; it is not legal to call GetCookOnTheFlyUnsolicitedFiles until after the platform has been added
			FPlatformProcess::Sleep(0.001f);
		}
	}

	virtual bool EnqueueCookRequest(UE::Cook::FCookPackageRequest CookPackageRequest) override
	{
		using namespace UE::Cook;
		FPlatformManager::FReadScopeLock PlatformsScopeLock(Cooker.PlatformManager->ReadLockPlatforms());

		if (const ITargetPlatform* TargetPlatform = Cooker.AddCookOnTheFlyPlatform(CookPackageRequest.PlatformName))
		{
			Cooker.CookOnTheFlyDeferredInitialize();
			Cooker.PlatformManager->AddRefCookOnTheFlyPlatform(CookPackageRequest.PlatformName, Cooker);
			FName StandardFileName(*FPaths::CreateStandardFilename(CookPackageRequest.Filename));
			FRequest* Request = AllocRequest();
			Request->CookPackageRequest = MoveTemp(CookPackageRequest);

			FCompletionCallback CookCompleted = [this, Request](FPackageData* PackageData)
			{
				const FPlatformData* PlatformData = Cooker.PlatformManager->GetPlatformDataByName(Request->CookPackageRequest.PlatformName);
				check(PlatformData);
				const ECookResult CookResult = PackageData
					? PackageData->GetCookResults(PlatformData->TargetPlatform)
					: ECookResult::Failed;
				
				UE_LOG(LogCook, Verbose, TEXT("Cook request completed, Filename='%s', Platform='%s', CookResult='%d'"),
					*Request->CookPackageRequest.Filename, *Request->CookPackageRequest.PlatformName.ToString(), uint32(CookResult));
				
				if (Request->CookPackageRequest.CompletionCallback)
				{
					Request->CookPackageRequest.CompletionCallback(CookResult);
				}
				
				Cooker.PlatformManager->ReleaseCookOnTheFlyPlatform(Request->CookPackageRequest.PlatformName);
				FreeRequest(Request);
			};

			UE_LOG(LogCook, Verbose, TEXT("Enqueing cook request, Filename='%s', Platform='%s'"), *Request->CookPackageRequest.Filename, *Request->CookPackageRequest.PlatformName.ToString());
			Cooker.ExternalRequests->EnqueueUnique(
				FFilePlatformRequest(StandardFileName, EInstigator::CookOnTheFly, TargetPlatform, MoveTemp(CookCompleted)),
				true);
			if (Cooker.ExternalRequests->CookRequestEvent)
			{
				Cooker.ExternalRequests->CookRequestEvent->Trigger();
			}

			return true;
		}
		else
		{
			UE_LOG(LogCook, Warning, TEXT("Trying to cook package on the fly for invalid platform '%s'"), *CookPackageRequest.PlatformName.ToString());

			return false;
		}
	};

	virtual bool EnqueueRecompileShaderRequest(UE::Cook::FRecompileShaderRequest RecompileShaderRequest) override
	{
		UE_LOG(LogCook, Verbose, TEXT("Enqueing recompile shader(s) request, Platform='%s', ShaderPlatform='%d'"),
			*RecompileShaderRequest.RecompileArguments.PlatformName, RecompileShaderRequest.RecompileArguments.ShaderPlatform);

		Cooker.PackageTracker->RecompileRequests.Enqueue(MoveTemp(RecompileShaderRequest));

		return true;
	}

	virtual ICookedPackageWriter& GetPackageWriter(const ITargetPlatform* TargetPlatform) override
	{
		return Cooker.FindOrCreatePackageWriter(TargetPlatform);
	}

	virtual double WaitForPendingFlush() override
	{
		const double StartTime = bIsFlushing ? FPlatformTime::Seconds() : 0.0;

		FlushCompletedEvent->Wait();

		return StartTime > 0.0 ? FPlatformTime::Seconds() - StartTime : 0.0;
	}

	void Flush()
	{
		bIsFlushing = true;
		FlushCompletedEvent->Reset();

		UE_LOG(LogCook, Log, TEXT("Flushing..."));

		for (UE::Cook::FCookSavePackageContext* Context : Cooker.SavePackageContexts)
		{
			Context->PackageWriter->Flush();
		}

		UE_LOG(LogCook, Log, TEXT("Flush completed"));

		bIsFlushing = false;
		FlushCompletedEvent->Trigger();
	}

private:
	struct FRequest
	{
		FRequest* NextFree = nullptr;
		UE::Cook::FCookPackageRequest CookPackageRequest;
	};

	FRequest* AllocRequest()
	{
		FScopeLock _(&RequestsCriticalSection);
		if (FRequest* Request = FirstFree)
		{
			FirstFree = Request->NextFree;
			Request->NextFree = nullptr;
			return Request;
		}
		else
		{
			const int32 RequestIndex = Requests.Add();
			return &Requests[RequestIndex];
		}
	}

	void FreeRequest(FRequest* Request)
	{
		FScopeLock _(&RequestsCriticalSection);
		check(Request->NextFree == nullptr);
		Request->NextFree = FirstFree;
		FirstFree = Request;
	}

	UCookOnTheFlyServer& Cooker;
	FEvent* FlushCompletedEvent;
	FCriticalSection FlushCriticalSection;
	FCriticalSection RequestsCriticalSection;
	TChunkedArray<FRequest> Requests;
	FRequest* FirstFree = nullptr;
	TAtomic<bool> bIsFlushing = {false};
};

/* UCookOnTheFlyServer functions
 *****************************************************************************/

UCookOnTheFlyServer::UCookOnTheFlyServer(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer),
	CurrentCookMode(ECookMode::CookOnTheFly),
	CookByTheBookOptions(nullptr),
	CookFlags(ECookInitializationFlags::None),
	bIsSavingPackage(false),
	AssetRegistry(nullptr)
{
}

UCookOnTheFlyServer::UCookOnTheFlyServer(FVTableHelper& Helper) :Super(Helper) {}

UCookOnTheFlyServer::~UCookOnTheFlyServer()
{
	ClearPackageStoreContexts();

	FCoreDelegates::OnFConfigCreated.RemoveAll(this);
	FCoreDelegates::OnFConfigDeleted.RemoveAll(this);
	FCoreUObjectDelegates::GetPreGarbageCollectDelegate().RemoveAll(this);
	FCoreUObjectDelegates::GetPostGarbageCollect().RemoveAll(this);
	GetTargetPlatformManager()->GetOnTargetPlatformsInvalidatedDelegate().RemoveAll(this);

	delete CookByTheBookOptions;
	CookByTheBookOptions = nullptr;
}

// This tick only happens in the editor.  The cook commandlet directly calls tick on the side.
void UCookOnTheFlyServer::Tick(float DeltaTime)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UCookOnTheFlyServer::Tick);

	check(IsCookingInEditor());

	if (IsCookByTheBookMode() && !IsCookByTheBookRunning() && !GIsSlowTask)
	{
		// if we are in the editor then precache some stuff ;)
		TArray<const ITargetPlatform*> CacheTargetPlatforms;
		const ULevelEditorPlaySettings* PlaySettings = GetDefault<ULevelEditorPlaySettings>();
		if (PlaySettings && (PlaySettings->LastExecutedLaunchModeType == LaunchMode_OnDevice))
		{
			FString DeviceName = PlaySettings->LastExecutedLaunchDevice.Left(PlaySettings->LastExecutedLaunchDevice.Find(TEXT("@")));
			CacheTargetPlatforms.Add(GetTargetPlatformManager()->FindTargetPlatform(DeviceName));
		}
		if (CacheTargetPlatforms.Num() > 0)
		{
			// early out all the stuff we don't care about 
			if (!IsCookFlagSet(ECookInitializationFlags::BuildDDCInBackground))
			{
				return;
			}
			TickPrecacheObjectsForPlatforms(0.001, CacheTargetPlatforms);
		}
	}

	uint32 CookedPackagesCount = 0;
	const static float CookOnTheSideTimeSlice = 0.1f; // seconds
	TickCookOnTheSide( CookOnTheSideTimeSlice, CookedPackagesCount);
	TickRecompileShaderRequests();
}

bool UCookOnTheFlyServer::IsTickable() const 
{ 
	return IsCookFlagSet(ECookInitializationFlags::AutoTick); 
}

TStatId UCookOnTheFlyServer::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UCookServer, STATGROUP_Tickables);
}

bool UCookOnTheFlyServer::StartCookOnTheFly(FCookOnTheFlyOptions InCookOnTheFlyOptions)
{
#if WITH_COTF
	check(IsCookOnTheFlyMode());
	//GetDerivedDataCacheRef().WaitForQuiescence(false);

#if PROFILE_NETWORK
	NetworkRequestEvent = FPlatformProcess::GetSynchEventFromPool();
#endif

	CookOnTheFlyOptions = MoveTemp(InCookOnTheFlyOptions);
	CookOnTheFlyServerInterface = MakeUnique<UCookOnTheFlyServer::FCookOnTheFlyServerInterface>(*this);
	ExternalRequests->CookRequestEvent = FPlatformProcess::GetSynchEventFromPool();

	// Precreate the map of all possible target platforms so we can access the collection of existing platforms in a threadsafe manner
	// Each PlatformData in the map will be uninitialized until we call AddCookOnTheFlyPlatform for the platform
	ITargetPlatformManagerModule& TPM = GetTargetPlatformManagerRef();
	for (const ITargetPlatform* TargetPlatform : TPM.GetTargetPlatforms())
	{
		PlatformManager->CreatePlatformData(TargetPlatform);
	}
	PlatformManager->SetArePlatformsPrepopulated(true);

	SetBeginCookConfigSettings();
	CreateSandboxFile();
	GenerateAssetRegistry();
	for (FName NeverCookPackage : GetNeverCookPackageFileNames(TArrayView<const FString>()))
	{
		PackageTracker->NeverCookPackageList.Add(NeverCookPackage);
	}

	GRedirectCollector.OnStartupPackageLoadComplete();

	{
		UE::Cook::FPlatformManager::FReadScopeLock PlatformScopeLock(PlatformManager->ReadLockPlatforms());
		for (ITargetPlatform* TargetPlatform : CookOnTheFlyOptions.TargetPlatforms)
		{
			AddCookOnTheFlyPlatform(FName(*TargetPlatform->PlatformName()));
		}
	}

	UE_LOG(LogCook, Display, TEXT("Starting '%s' cook-on-the-fly server"),
		CookOnTheFlyOptions.bZenStore ? TEXT("Zen") : TEXT("Network File"));

	if (CookOnTheFlyOptions.bZenStore)
	{
		UE::Cook::FIoStoreCookOnTheFlyServerOptions ServerOptions;
		ServerOptions.Port = -1; // Use default
		CookOnTheFlyRequestManager = UE::Cook::MakeIoStoreCookOnTheFlyRequestManager(*CookOnTheFlyServerInterface, AssetRegistry, MoveTemp(ServerOptions));
	}
	else
	{
		FNetworkFileServerOptions ServerOptions;
		ServerOptions.Protocol = NFSP_Tcp;
		ServerOptions.Port = CookOnTheFlyOptions.bBindAnyPort ? 0 : -1;
		CookOnTheFlyRequestManager = UE::Cook::MakeNetworkFileCookOnTheFlyRequestManager(*CookOnTheFlyServerInterface, MoveTemp(ServerOptions));
	}

	const bool bInitialized = CookOnTheFlyRequestManager->Initialize();
	return bInitialized;
#else
	return false;
#endif
}

void UCookOnTheFlyServer::CookOnTheFlyDeferredInitialize()
{
	if (bHasDeferredInitializeCookOnTheFly)
	{
		return;
	}
	bHasDeferredInitializeCookOnTheFly = true;
	BlockOnAssetRegistry();
	PackageDatas->BeginCook();
}

const ITargetPlatform* UCookOnTheFlyServer::AddCookOnTheFlyPlatform(const FName& PlatformName)
{
	const UE::Cook::FPlatformData* PlatformData = PlatformManager->GetPlatformDataByName(PlatformName);
	if (!PlatformData)
	{
		UE_LOG(LogCook, Warning, TEXT("Target platform %s wasn't found."), *PlatformName.ToString());
		return nullptr;
	}

	if (PlatformData->bIsSandboxInitialized)
	{
		// Platform has already been added by this function or by StartCookByTheBook
		return PlatformData->TargetPlatform;
	}
	TrySetDefaultAsyncIODeletePlatform(PlatformData->TargetPlatform->PlatformName());

	if (IsInGameThread())
	{
		AddCookOnTheFlyPlatformFromGameThread(PlatformData->TargetPlatform);
	}
	else
	{
		FEvent* PlatformInitializedEvent = FPlatformProcess::GetSynchEventFromPool();
		
		// Registering a new platform is not thread safe; queue the command for TickCookOnTheSide to execute
		ExternalRequests->AddCallback([this, PlatformName, PlatformInitializedEvent]()
			{
				const UE::Cook::FPlatformData* PlatformData = PlatformManager->GetPlatformDataByName(PlatformName);
				check(PlatformData);
				AddCookOnTheFlyPlatformFromGameThread(PlatformData->TargetPlatform);
			PlatformInitializedEvent->Trigger();
			});

		if (ExternalRequests->CookRequestEvent)
		{
			ExternalRequests->CookRequestEvent->Trigger();
		}
		
		PlatformInitializedEvent->Wait();
		FPlatformProcess::ReturnSynchEventToPool(PlatformInitializedEvent);
	}
	return PlatformData->TargetPlatform;
}

void UCookOnTheFlyServer::InitializeShadersForCookOnTheFly(const TArrayView<ITargetPlatform* const>& NewTargetPlatforms)
{
	for (const ITargetPlatform* TargetPlatform : NewTargetPlatforms)
	{
		const FString& PlatformName = TargetPlatform->PlatformName();
		UE_LOG(LogCook, Display, TEXT("Initializing shaders for cook-on-the-fly on platform '%s'"), *PlatformName);

		TArray<uint8> GlobalShaderMap;
		FShaderRecompileData RecompileArgs(PlatformName, SP_NumPlatforms, ODSCRecompileCommand::Global, nullptr, nullptr, &GlobalShaderMap);
		RecompileShadersForRemote(RecompileArgs, GetSandboxDirectory(PlatformName));
	}
}

void UCookOnTheFlyServer::AddCookOnTheFlyPlatformFromGameThread(ITargetPlatform* TargetPlatform)
{
	check(!!(CookFlags & ECookInitializationFlags::GeneratedAssetRegistry)); // GenerateAssetRegistry should have been called in StartNetworkFileServer

	UE::Cook::FPlatformData* PlatformData = PlatformManager->GetPlatformData(TargetPlatform);
	check(PlatformData != nullptr); // should have been checked by the caller
	if (PlatformData->bIsSandboxInitialized)
	{
		return;
	}

	TArrayView<ITargetPlatform* const> NewTargetPlatforms(&TargetPlatform,1);

	RefreshPlatformAssetRegistries(NewTargetPlatforms);
	FindOrCreateSaveContexts(NewTargetPlatforms);
	BeginCookSandbox(NewTargetPlatforms);
	InitializeTargetPlatforms(NewTargetPlatforms);
	InitializeShadersForCookOnTheFly(NewTargetPlatforms);

	// When cooking on the fly the full registry is saved at the beginning
	// in cook by the book asset registry is saved after the cook is finished
	FAssetRegistryGenerator& Generator = *PlatformData->RegistryGenerator;
	Generator.SaveAssetRegistry(GetSandboxAssetRegistryFilename(), true);
	check(PlatformData->bIsSandboxInitialized); // This should have been set by BeginCookSandbox, and it is what we use to determine whether a platform has been initialized

	FindOrCreatePackageWriter(TargetPlatform).BeginCook();
}

void UCookOnTheFlyServer::OnTargetPlatformsInvalidated()
{
	check(IsInGameThread());
	TMap<ITargetPlatform*, ITargetPlatform*> Remap = PlatformManager->RemapTargetPlatforms();

	if (CookByTheBookOptions)
	{
		RemapMapKeys(CookByTheBookOptions->MapDependencyGraphs, Remap);
	}
	PackageDatas->RemapTargetPlatforms(Remap);
	PackageTracker->RemapTargetPlatforms(Remap);
	ExternalRequests->RemapTargetPlatforms(Remap);

	if (PlatformManager->GetArePlatformsPrepopulated())
	{
		for (const ITargetPlatform* TargetPlatform : GetTargetPlatformManager()->GetTargetPlatforms())
		{
			PlatformManager->CreatePlatformData(TargetPlatform);
		}
	}
}

bool UCookOnTheFlyServer::BroadcastFileserverPresence( const FGuid &InstanceId )
{
	
	TArray<FString> AddressStringList;

	for ( int i = 0; i < NetworkFileServers.Num(); ++i )
	{
		TArray<TSharedPtr<FInternetAddr> > AddressList;
		INetworkFileServer *NetworkFileServer = NetworkFileServers[i];
		if ((NetworkFileServer == NULL || !NetworkFileServer->IsItReadyToAcceptConnections() || !NetworkFileServer->GetAddressList(AddressList)))
		{
			LogCookerMessage( FString(TEXT("Failed to create network file server")), EMessageSeverity::Error );
			continue;
		}

		// broadcast our presence
		if (InstanceId.IsValid())
		{
			for (int32 AddressIndex = 0; AddressIndex < AddressList.Num(); ++AddressIndex)
			{
				AddressStringList.Add(FString::Printf( TEXT("%s://%s"), *NetworkFileServer->GetSupportedProtocol(),  *AddressList[AddressIndex]->ToString(true)));
			}

		}
	}

	TSharedPtr<FMessageEndpoint, ESPMode::ThreadSafe> MessageEndpoint = FMessageEndpoint::Builder("UCookOnTheFlyServer").Build();

	if (MessageEndpoint.IsValid())
	{
		MessageEndpoint->Publish(FMessageEndpoint::MakeMessage<FFileServerReady>(AddressStringList, InstanceId), EMessageScope::Network);
	}		
	
	return true;
}

/*----------------------------------------------------------------------------
	FArchiveFindReferences.
----------------------------------------------------------------------------*/
/**
 * Archive for gathering all the object references to other objects
 */
class FArchiveFindReferences : public FArchiveUObject
{
private:
	/**
	 * I/O function.  Called when an object reference is encountered.
	 *
	 * @param	Obj		a pointer to the object that was encountered
	 */
	FArchive& operator<<( UObject*& Obj ) override
	{
		if( Obj )
		{
			FoundObject( Obj );
		}
		return *this;
	}

	virtual FArchive& operator<< (struct FSoftObjectPtr& Value) override
	{
		if ( Value.Get() )
		{
			Value.Get()->Serialize( *this );
		}
		return *this;
	}
	virtual FArchive& operator<< (struct FSoftObjectPath& Value) override
	{
		if ( Value.ResolveObject() )
		{
			Value.ResolveObject()->Serialize( *this );
		}
		return *this;
	}


	void FoundObject( UObject* Object )
	{
		if ( RootSet.Find(Object) == NULL )
		{
			if ( Exclude.Find(Object) == INDEX_NONE )
			{
				// remove this check later because don't want this happening in development builds
				//check(RootSetArray.Find(Object)==INDEX_NONE);

				RootSetArray.Add( Object );
				RootSet.Add(Object);
				Found.Add(Object);
			}
		}
	}


	/**
	 * list of Outers to ignore;  any objects encountered that have one of
	 * these objects as an Outer will also be ignored
	 */
	TArray<UObject*> &Exclude;

	/** list of objects that have been found */
	TSet<UObject*> &Found;
	
	/** the objects to display references to */
	TArray<UObject*> RootSetArray;
	/** Reflection of the rootsetarray */
	TSet<UObject*> RootSet;

public:

	/**
	 * Constructor
	 * 
	 * @param	inOutputAr		archive to use for logging results
	 * @param	inOuter			only consider objects that do not have this object as its Outer
	 * @param	inSource		object to show references for
	 * @param	inExclude		list of objects that should be ignored if encountered while serializing SourceObject
	 */
	FArchiveFindReferences( TSet<UObject*> InRootSet, TSet<UObject*> &inFound, TArray<UObject*> &inExclude )
		: Exclude(inExclude)
		, Found(inFound)
		, RootSet(InRootSet)
	{
		ArIsObjectReferenceCollector = true;
		this->SetIsSaving(true);

		for ( UObject* Object : RootSet )
		{
			RootSetArray.Add( Object );
		}
		
		// loop through all the objects in the root set and serialize them
		for ( int RootIndex = 0; RootIndex < RootSetArray.Num(); ++RootIndex )
		{
			UObject* SourceObject = RootSetArray[RootIndex];

			// quick sanity check
			check(SourceObject);
			check(SourceObject->IsValidLowLevel());

			SourceObject->Serialize( *this );
		}

	}

	/**
	 * Returns the name of the Archive.  Useful for getting the name of the package a struct or object
	 * is in when a loading error occurs.
	 *
	 * This is overridden for the specific Archive Types
	 **/
	virtual FString GetArchiveName() const override { return TEXT("FArchiveFindReferences"); }
};

void UCookOnTheFlyServer::GetDependentPackages(const TSet<UPackage*>& RootPackages, TSet<FName>& FoundPackages)
{
	TSet<FName> RootPackageFNames;
	for (const UPackage* RootPackage : RootPackages)
	{
		RootPackageFNames.Add(RootPackage->GetFName());
	}


	GetDependentPackages(RootPackageFNames, FoundPackages);

}


void UCookOnTheFlyServer::GetDependentPackages( const TSet<FName>& RootPackages, TSet<FName>& FoundPackages )
{
	TArray<FName> FoundPackagesArray;
	for (const FName& RootPackage : RootPackages)
	{
		FoundPackagesArray.Add(RootPackage);
		FoundPackages.Add(RootPackage);
	}

	int FoundPackagesCounter = 0;
	while ( FoundPackagesCounter < FoundPackagesArray.Num() )
	{
		TArray<FName> PackageDependencies;
		if (AssetRegistry->GetDependencies(FoundPackagesArray[FoundPackagesCounter], PackageDependencies, UE::AssetRegistry::EDependencyCategory::Package) == false)
		{
			// this could happen if we are in the editor and the dependency list is not up to date

			if (IsCookingInEditor() == false)
			{
				UE_LOG(LogCook, Fatal, TEXT("Unable to find package %s in asset registry.  Can't generate cooked asset registry"), *FoundPackagesArray[FoundPackagesCounter].ToString());
			}
			else
			{
				UE_LOG(LogCook, Warning, TEXT("Unable to find package %s in asset registry, cooked asset registry information may be invalid "), *FoundPackagesArray[FoundPackagesCounter].ToString());
			}
		}
		++FoundPackagesCounter;
		for ( const FName& OriginalPackageDependency : PackageDependencies )
		{
			// check(PackageDependency.ToString().StartsWith(TEXT("/")));
			FName PackageDependency = OriginalPackageDependency;
			FString PackageDependencyString = PackageDependency.ToString();

			FText OutReason;
			const bool bIncludeReadOnlyRoots = true; // Dependency packages are often script packages (read-only)
			if (!FPackageName::IsValidLongPackageName(PackageDependencyString, bIncludeReadOnlyRoots, &OutReason))
			{
				const FText FailMessage = FText::Format(LOCTEXT("UnableToGeneratePackageName", "Unable to generate long package name for {0}. {1}"),
					FText::FromString(PackageDependencyString), OutReason);

				LogCookerMessage(FailMessage.ToString(), EMessageSeverity::Warning);
				continue;
			}
			else if (FPackageName::IsScriptPackage(PackageDependencyString) || FPackageName::IsMemoryPackage(PackageDependencyString))
			{
				continue;
			}

			if ( FoundPackages.Contains(PackageDependency) == false )
			{
				FoundPackages.Add(PackageDependency);
				FoundPackagesArray.Add( PackageDependency );
			}
		}
	}

}

bool UCookOnTheFlyServer::ContainsMap(const FName& PackageName) const
{
	TArray<FAssetData> Assets;
	ensure(AssetRegistry->GetAssetsByPackageName(PackageName, Assets, true));

	for (const FAssetData& Asset : Assets)
	{
		UClass* AssetClass = Asset.GetClass();
		if (AssetClass && (AssetClass->IsChildOf(UWorld::StaticClass()) || AssetClass->IsChildOf(ULevel::StaticClass())))
		{
			return true;
		}
	}
	return false;
}

bool UCookOnTheFlyServer::ContainsRedirector(const FName& PackageName, TMap<FName, FName>& RedirectedPaths) const
{
	bool bFoundRedirector = false;
	TArray<FAssetData> Assets;
	ensure(AssetRegistry->GetAssetsByPackageName(PackageName, Assets, true));

	for (const FAssetData& Asset : Assets)
	{
		if (Asset.IsRedirector())
		{
			FName RedirectedPath;
			FString RedirectedPathString;
			if (Asset.GetTagValue("DestinationObject", RedirectedPathString))
			{
				ConstructorHelpers::StripObjectClass(RedirectedPathString);
				RedirectedPath = FName(*RedirectedPathString);
				FAssetData DestinationData = AssetRegistry->GetAssetByObjectPath(RedirectedPath, true);
				TSet<FName> SeenPaths;

				SeenPaths.Add(RedirectedPath);

				// Need to follow chain of redirectors
				while (DestinationData.IsRedirector())
				{
					if (DestinationData.GetTagValue("DestinationObject", RedirectedPathString))
					{
						ConstructorHelpers::StripObjectClass(RedirectedPathString);
						RedirectedPath = FName(*RedirectedPathString);

						if (SeenPaths.Contains(RedirectedPath))
						{
							// Recursive, bail
							DestinationData = FAssetData();
						}
						else
						{
							SeenPaths.Add(RedirectedPath);
							DestinationData = AssetRegistry->GetAssetByObjectPath(RedirectedPath, true);
						}
					}
					else
					{
						// Can't extract
						DestinationData = FAssetData();						
					}
				}

				// DestinationData may be invalid if this is a subobject, check package as well
				bool bDestinationValid = DestinationData.IsValid();

				if (!bDestinationValid)
				{
					if (RedirectedPath != NAME_None)
					{
						FName StandardPackageName = PackageDatas->GetFileNameByPackageName(FName(*FPackageName::ObjectPathToPackageName(RedirectedPathString)));
						if (!StandardPackageName.IsNone())
						{
							bDestinationValid = true;
						}
					}
				}

				if (bDestinationValid)
				{
					RedirectedPaths.Add(Asset.ObjectPath, RedirectedPath);
				}
				else
				{
					RedirectedPaths.Add(Asset.ObjectPath, NAME_None);
					UE_LOG(LogCook, Log, TEXT("Found redirector in package %s pointing to deleted object %s"), *PackageName.ToString(), *RedirectedPathString);
				}

				bFoundRedirector = true;
			}
		}
	}
	return bFoundRedirector;
}

bool UCookOnTheFlyServer::IsCookingInEditor() const
{
	return CurrentCookMode == ECookMode::CookByTheBookFromTheEditor || CurrentCookMode == ECookMode::CookOnTheFlyFromTheEditor;
}

bool UCookOnTheFlyServer::IsRealtimeMode() const 
{
	return CurrentCookMode == ECookMode::CookByTheBookFromTheEditor || CurrentCookMode == ECookMode::CookOnTheFlyFromTheEditor;
}

bool UCookOnTheFlyServer::IsCookByTheBookMode() const
{
	return CurrentCookMode == ECookMode::CookByTheBookFromTheEditor || CurrentCookMode == ECookMode::CookByTheBook;
}

bool UCookOnTheFlyServer::IsUsingShaderCodeLibrary() const
{
	return IsCookByTheBookMode() && AllowShaderCompiling();
}

bool UCookOnTheFlyServer::IsUsingZenStore() const
{
	return (IsCookByTheBookMode() && CookByTheBookOptions->bZenStore) || (IsCookOnTheFlyMode() && CookOnTheFlyOptions.bZenStore);
}

bool UCookOnTheFlyServer::IsCookOnTheFlyMode() const
{
	return CurrentCookMode == ECookMode::CookOnTheFly || CurrentCookMode == ECookMode::CookOnTheFlyFromTheEditor; 
}

bool UCookOnTheFlyServer::IsCreatingReleaseVersion()
{
	if (CookByTheBookOptions)
	{
		return !CookByTheBookOptions->CreateReleaseVersion.IsEmpty();
	}

	return false;
}

bool UCookOnTheFlyServer::IsCookingDLC() const
{
	// can only cook DLC in cook by the book
	// we are cooking DLC when the DLC name is setup
	
	if (CookByTheBookOptions)
	{
		return !CookByTheBookOptions->DlcName.IsEmpty();
	}

	return false;
}

bool UCookOnTheFlyServer::IsCookingAgainstFixedBase() const
{
	return IsCookingDLC() && CookByTheBookOptions && CookByTheBookOptions->bCookAgainstFixedBase;
}

bool UCookOnTheFlyServer::ShouldPopulateFullAssetRegistry() const
{
	return !IsCookingDLC() || (CookByTheBookOptions && CookByTheBookOptions->bDlcLoadMainAssetRegistry);
}

FString UCookOnTheFlyServer::GetBaseDirectoryForDLC() const
{
	TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(CookByTheBookOptions->DlcName);
	if (Plugin.IsValid())
	{
		return Plugin->GetBaseDir();
	}

	return FPaths::ProjectPluginsDir() / CookByTheBookOptions->DlcName;
}

FString UCookOnTheFlyServer::GetContentDirectoryForDLC() const
{
	return GetBaseDirectoryForDLC() / TEXT("Content");
}

// allow for a command line to start async preloading a Development AssetRegistry if requested
static FEventRef GPreloadAREvent(EEventMode::ManualReset);
static FEventRef GPreloadARInfoEvent(EEventMode::ManualReset);
static FAssetRegistryState GPreloadedARState;
static FString GPreloadedARPath;
static FDelayedAutoRegisterHelper GPreloadARHelper(EDelayedRegisterRunPhase::EarliestPossiblePluginsLoaded, []()
	{
		// if we don't want to preload, then do nothing here
		if (!FParse::Param(FCommandLine::Get(), TEXT("PreloadDevAR")))
		{
			GPreloadAREvent->Trigger();
			GPreloadARInfoEvent->Trigger();
			return;
		}

		// kick off a thread to preload the DevelopmentAssetRegistry
		Async(EAsyncExecution::Thread, []()
			{
				FString BasedOnReleaseVersion;
				FString DevelopmentAssetRegistryPlatformOverride;
				// some manual commandline processing - we don't have the cooker params set properly yet - but this is not a generic solution, it is opt-in
				if (FParse::Value(FCommandLine::Get(), TEXT("BasedOnReleaseVersion="), BasedOnReleaseVersion) &&
					FParse::Value(FCommandLine::Get(), TEXT("DevelopmentAssetRegistryPlatformOverride="), DevelopmentAssetRegistryPlatformOverride))
				{
					const bool bIsCookingAgainstFixedBase = FParse::Param(FCommandLine::Get(), TEXT("CookAgainstFixedBase "));
					const bool bVerifyPackagesExist = !bIsCookingAgainstFixedBase;

					// get the AR file path and see if it exists
					GPreloadedARPath = GetBasedOnReleaseVersionAssetRegistryPath(BasedOnReleaseVersion, DevelopmentAssetRegistryPlatformOverride) / TEXT("Metadata") / GetDevelopmentAssetRegistryFilename();

					// now that the info has been set, we can allow the other side of this code to check the ARPath
					GPreloadARInfoEvent->Trigger();

					TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*GPreloadedARPath));
					if (Reader)
					{
						GPreloadedARState.Serialize(*Reader.Get(), FAssetRegistrySerializationOptions());
					}
				}

				GPreloadAREvent->Trigger();
			}
		);
	}
);


COREUOBJECT_API extern bool GOutputCookingWarnings;

void UCookOnTheFlyServer::WaitForRequests(int TimeoutMs)
{
	if (ExternalRequests->CookRequestEvent)
	{
		ExternalRequests->CookRequestEvent->Wait(TimeoutMs, true);
	}
}

bool UCookOnTheFlyServer::HasRemainingWork() const
{ 
	return ExternalRequests->HasRequests() || PackageDatas->GetMonitor().GetNumInProgress() > 0;
}

bool UCookOnTheFlyServer::RequestPackage(const FName& StandardFileName, const TArrayView<const ITargetPlatform* const>& TargetPlatforms, const bool bForceFrontOfQueue)
{
	using namespace UE::Cook;

	if (!IsCookByTheBookMode())
	{
		bCookOnTheFlyExternalRequests = true;
		for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
		{
			AddCookOnTheFlyPlatformFromGameThread(const_cast<ITargetPlatform*>(TargetPlatform));
			PlatformManager->AddRefCookOnTheFlyPlatform(FName(*TargetPlatform->PlatformName()), *this);
		}
	}

	ExternalRequests->EnqueueUnique(
		FFilePlatformRequest(StandardFileName, EInstigator::RequestPackageFunction, TargetPlatforms),
		bForceFrontOfQueue);
	return true;
}

bool UCookOnTheFlyServer::RequestPackage(const FName& StandardFileName, const TArrayView<const FName>& TargetPlatformNames, const bool bForceFrontOfQueue)
{
	TArray<const ITargetPlatform*> TargetPlatforms;
	ITargetPlatformManagerModule& TPM = GetTargetPlatformManagerRef();
	for (const FName& TargetPlatformName : TargetPlatformNames)
	{
		const ITargetPlatform* TargetPlatform = TPM.FindTargetPlatform(TargetPlatformName.ToString());
		if (TargetPlatform)
		{
			TargetPlatforms.Add(TargetPlatform);
		}
	}
	return RequestPackage(StandardFileName, TargetPlatforms, bForceFrontOfQueue);
}

bool UCookOnTheFlyServer::RequestPackage(const FName& StandardPackageFName, const bool bForceFrontOfQueue)
{
	check(IsCookByTheBookMode()); // Invalid to call RequestPackage without a list of TargetPlatforms unless we are in cook by the book mode
	return RequestPackage(StandardPackageFName, PlatformManager->GetSessionPlatforms(), bForceFrontOfQueue);
}

uint32 UCookOnTheFlyServer::TickCookOnTheSide(const float TimeSlice, uint32 &CookedPackageCount, ECookTickFlags TickFlags)
{
	TickCancels();
	TickNetwork();
	if (!IsInSession())
	{
		return COSR_None;
	}

	if (IsCookByTheBookMode() && CookByTheBookOptions->bRunning && CookByTheBookOptions->bFullLoadAndSave)
	{
		uint32 Result = FullLoadAndSave(CookedPackageCount);

		CookByTheBookFinished();

		return Result;
	}

	COOK_STAT(FScopedDurationTimer TickTimer(DetailedCookStats::TickCookOnTheSideTimeSec));

	{
		if (AssetRegistry == nullptr || AssetRegistry->IsLoadingAssets())
		{
			// early out
			return COSR_None;
		}
	}

	UE::Cook::FTickStackData StackData(TimeSlice, IsRealtimeMode(), TickFlags);
	bool bCookComplete = false;

	{
		UE_SCOPED_HIERARCHICAL_COOKTIMER(TickCookOnTheSide); // Make sure no UE_SCOPED_HIERARCHICAL_COOKTIMERs are around CookByTheBookFinishes, as that function deletes memory for them

		bool bContinueTick = true;
		while (bContinueTick && (!IsEngineExitRequested() || CurrentCookMode == ECookMode::CookByTheBook))
		{
			TickCookStatus(StackData);

			ECookAction CookAction = DecideNextCookAction(StackData);
			int32 NumPushed;
			bool bBusy;
			switch (CookAction)
			{
			case ECookAction::Request:
				PumpRequests(StackData, NumPushed);
				if (NumPushed > 0)
				{
					SetLoadBusy(false);
				}
				break;
			case ECookAction::Load:
				PumpLoads(StackData, 0, NumPushed, bBusy);
				SetLoadBusy(bBusy && NumPushed == 0); // Mark as busy if pump was blocked and we did not make any progress
				if (NumPushed > 0)
				{
					SetSaveBusy(false);
				}
				break;
			case ECookAction::LoadLimited:
				PumpLoads(StackData, DesiredLoadQueueLength, NumPushed, bBusy);
				SetLoadBusy(bBusy && NumPushed == 0); // Mark as busy if pump was blocked and we did not make any progress
				if (NumPushed > 0)
				{
					SetSaveBusy(false);
				}
				break;
			case ECookAction::Save:
				PumpSaves(StackData, 0, NumPushed, bBusy);
				SetSaveBusy(bBusy && NumPushed == 0); // Mark as busy if pump was blocked and we did not make any progress
				break;
			case ECookAction::SaveLimited:
				PumpSaves(StackData, DesiredSaveQueueLength, NumPushed, bBusy);
				SetSaveBusy(bBusy && NumPushed == 0); // Mark as busy if pump was blocked and we did not make any progress
				break;
			case ECookAction::Done:
				bContinueTick = false;
				bCookComplete = true;
				break;
			case ECookAction::YieldTick:
				bContinueTick = false;
				break;
			case ECookAction::Cancel:
				CancelCookByTheBook();
				bContinueTick = false;
				break;
			default:
				check(false);
				break;
			}
		}
	}

	if (IsCookOnTheFlyMode() && (IsCookingInEditor() == false))
	{
		static int32 TickCounter = 0;
		++TickCounter;
		if (TickCounter > 50)
		{
			// dump stats every 50 ticks or so
			DumpStats();
			TickCounter = 0;
		}
	}

	if (CookByTheBookOptions)
	{
		CookByTheBookOptions->CookTime += StackData.Timer.GetTimeTillNow();
	}

	if (IsCookByTheBookRunning() && bCookComplete)
	{
		check(IsCookByTheBookMode());

		// if we are out of stuff and we are in cook by the book from the editor mode then we finish up
		UpdateDisplay(TickFlags, true /* bForceDisplay */);
		CookByTheBookFinished();
	}

	if (IsCookOnTheFlyMode() && bCookComplete)
	{
		CookOnTheFlyServerInterface->Flush();
	}

	CookedPackageCount += StackData.CookedPackageCount;
	return StackData.ResultFlags;
}

void UCookOnTheFlyServer::TickCookStatus(UE::Cook::FTickStackData& StackData)
{
	UE_SCOPED_COOKTIMER(TickCookStatus);

	// TODO: Calculate CurrentTime once per status update and share it with e.g. StackData.Timer
	double CurrentTime = FPlatformTime::Seconds();
	if (LastCookableObjectTickTime + TickCookableObjectsFrameTime <= CurrentTime)
	{
		UE_SCOPED_COOKTIMER(TickCookableObjects);
		FTickableCookObject::TickObjects(CurrentTime - LastCookableObjectTickTime, false /* bTickComplete */);
		LastCookableObjectTickTime = CurrentTime;
	}

	UpdateDisplay(StackData.TickFlags, false /* bForceDisplay */);
	// prevent autosave from happening until we are finished cooking
	// causes really bad hitches
	if (GUnrealEd)
	{
		const static float SecondsWarningTillAutosave = 10.0f;
		GUnrealEd->GetPackageAutoSaver().ForceMinimumTimeTillAutoSave(SecondsWarningTillAutosave);
	}

	ProcessUnsolicitedPackages();
	UpdatePackageFilter();
	PumpExternalRequests(StackData.Timer);
}

void UCookOnTheFlyServer::SetSaveBusy(bool bInBusy)
{
	using namespace UE::Cook;

	if (bSaveBusy != bInBusy)
	{
		bSaveBusy = bInBusy;
		if (bSaveBusy)
		{
			SaveBusyTimeStarted = FPlatformTime::Seconds();
			SaveBusyTimeLastRetry = SaveBusyTimeStarted;
		}
		else
		{
			SaveBusyTimeStarted = 0;
			SaveBusyTimeLastRetry = 0;
		}
	}
	else if (bSaveBusy)
	{
		const float CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - SaveBusyTimeStarted > GCookProgressWarnBusyTime)
		{
			// Issue a status update. For each UObject we're still waiting on, check whether the long duration is expected using type-specific checks
			// Make the status update a warning if the long duration is not reported as expected.
			TArray<UObject*> NonExpectedObjects;
			TSet<UPackage*> NonExpectedPackages;
			TArray<UObject*> ExpectedObjects;
			TSet<UPackage*> ExpectedPackages;
			FPackageDataQueue& SaveQueue = PackageDatas->GetSaveQueue();
			const TArray<FPendingCookedPlatformData>& PendingCookedPlatformDatas = PackageDatas->GetPendingCookedPlatformDatas();
			TArray<UClass*> CompilationUsers({ UMaterialInterface::StaticClass(), FindObject<UClass>(ANY_PACKAGE, TEXT("NiagaraScript")) });

			for (const FPendingCookedPlatformData& Data : PendingCookedPlatformDatas)
			{
				UObject* Object = Data.Object.Get();
				if (!Object)
				{
					continue;
				}
				bool bCompilationUser = false;
				for (UClass* CompilationUserClass : CompilationUsers)
				{
					if (Object->IsA(CompilationUserClass))
					{
						bCompilationUser = true;
						break;
					}
				}
				if (bCompilationUser && GShaderCompilingManager->IsCompiling())
				{
					ExpectedObjects.Add(Object);
					ExpectedPackages.Add(Object->GetPackage());
				}
				else
				{
					NonExpectedObjects.Add(Object);
					NonExpectedPackages.Add(Object->GetPackage());
				}
			}
			TArray<UPackage*> RemovePackages;
			for (UPackage* Package : ExpectedPackages)
			{
				if (NonExpectedPackages.Contains(Package))
				{
					RemovePackages.Add(Package);
				}
			}
			for (UPackage* Package : RemovePackages)
			{
				ExpectedPackages.Remove(Package);
			}
			
			FString Message = FString::Printf(TEXT("Cooker has been blocked from saving the current packages for %f seconds."), GCookProgressWarnBusyTime);
			if (NonExpectedObjects.IsEmpty())
			{
				UE_LOG(LogCook, Display, TEXT("%s"), *Message);
			}
			else
			{
				UE_LOG(LogCook, Warning, TEXT("%s"), *Message);
			}

			UE_LOG(LogCook, Display, TEXT("%d packages in the savequeue: "), SaveQueue.Num());
			int DisplayCount = 0;
			const int DisplayMax = 10;
			for (TSet<UPackage*>* PackageSet : { &NonExpectedPackages, &ExpectedPackages })
			{
				for (UPackage* Package : *PackageSet)
				{
					if (DisplayCount == DisplayMax)
					{
						UE_LOG(LogCook, Display, TEXT("    ..."));
						break;
					}
					UE_LOG(LogCook, Display, TEXT("    %s"), *Package->GetName());
					++DisplayCount;
				}
			}
			if (DisplayCount == 0)
			{
				UE_LOG(LogCook, Display, TEXT("    <None>"));
			}

			UE_LOG(LogCook, Display, TEXT("%d objects that have not yet returned true from IsCachedCookedPlatformDataLoaded:"), PackageDatas->GetPendingCookedPlatformDatas().Num());
			DisplayCount = 0;
			for (TArray<UObject*>* ObjectArray : {  &NonExpectedObjects, &ExpectedObjects })
			{
				for (UObject* Object : *ObjectArray)
				{
					if (DisplayCount == DisplayMax)
					{
						UE_LOG(LogCook, Display, TEXT("    ..."));
						break;
					}
					UE_LOG(LogCook, Display, TEXT("    %s"), *Object->GetFullName());
					++DisplayCount;
				}
			}
			if (DisplayCount == 0)
			{
				UE_LOG(LogCook, Display, TEXT("    <None>"));
			}

			SaveBusyTimeStarted = CurrentTime;
		}
	}
}

void UCookOnTheFlyServer::SetLoadBusy(bool bInLoadBusy)
{
	using namespace UE::Cook;

	if (bLoadBusy != bInLoadBusy)
	{
		bLoadBusy = bInLoadBusy;
		if (bLoadBusy)
		{
			LoadBusyTimeStarted = FPlatformTime::Seconds();
			LoadBusyTimeLastRetry = LoadBusyTimeStarted;
		}
		else
		{
			LoadBusyTimeStarted = 0;
			LoadBusyTimeLastRetry = 0;
		}
	}
	else if (bLoadBusy)
	{
		const float CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LoadBusyTimeStarted > GCookProgressWarnBusyTime)
		{
			int DisplayCount = 0;
			const int DisplayMax = 10;
			FLoadPrepareQueue& LoadPrepareQueue = PackageDatas->GetLoadPrepareQueue();
			UE_LOG(LogCook, Warning, TEXT("Cooker has been blocked from loading the current packages for %f seconds. %d packages in the loadqueue:"), GCookProgressWarnBusyTime, LoadPrepareQueue.PreloadingQueue.Num() + LoadPrepareQueue.EntryQueue.Num());
			for (FPackageData* PackageData : LoadPrepareQueue.PreloadingQueue)
			{
				if (DisplayCount == DisplayMax)
				{
					UE_LOG(LogCook, Display, TEXT("    ..."));
					break;
				}
				UE_LOG(LogCook, Display, TEXT("    %s"), *PackageData->GetFileName().ToString());
				++DisplayCount;
			}
			for (FPackageData* PackageData : LoadPrepareQueue.EntryQueue)
			{
				if (DisplayCount == DisplayMax)
				{
					UE_LOG(LogCook, Display, TEXT("    ..."));
					break;
				}
				UE_LOG(LogCook, Display, TEXT("    %s"), *PackageData->GetFileName().ToString());
				++DisplayCount;
			}
			if (DisplayCount == 0)
			{
				UE_LOG(LogCook, Display, TEXT("    <None>"));
			}
			LoadBusyTimeStarted = CurrentTime;
		}
	}
}

void UCookOnTheFlyServer::UpdateDisplay(ECookTickFlags TickFlags, bool bForceDisplay)
{
	using namespace UE::Cook;

	const float CurrentTime = FPlatformTime::Seconds();
	const float DeltaProgressDisplayTime = CurrentTime - LastProgressDisplayTime;
	const int32 CookedPackagesCount = PackageDatas->GetNumCooked();
	const int32 CookPendingCount = ExternalRequests->GetNumRequests() + PackageDatas->GetMonitor().GetNumInProgress();
	if (bForceDisplay ||
		(DeltaProgressDisplayTime >= GCookProgressUpdateTime && CookPendingCount != 0 &&
			(LastCookedPackagesCount != CookedPackagesCount || LastCookPendingCount != CookPendingCount || DeltaProgressDisplayTime > GCookProgressRepeatTime)))
	{
		UE_CLOG(!(TickFlags & ECookTickFlags::HideProgressDisplay) && (GCookProgressDisplay & (int32)ECookProgressDisplayMode::RemainingPackages),
			LogCook,
			Display,
			TEXT("Cooked packages %d Packages Remain %d Total %d"),
			CookedPackagesCount,
			CookPendingCount,
			CookedPackagesCount + CookPendingCount);

		LastCookedPackagesCount = CookedPackagesCount;
		LastCookPendingCount = CookPendingCount;
		LastProgressDisplayTime = CurrentTime;
	}
	const float DeltaDiagnosticsDisplayTime = CurrentTime - LastDiagnosticsDisplayTime;
	if (bForceDisplay || DeltaDiagnosticsDisplayTime > GCookProgressDiagnosticTime)
	{
		uint32 OpenFileHandles = 0;
#if PLATFORMFILETRACE_ENABLED
		OpenFileHandles = FPlatformFileTrace::GetOpenFileHandleCount();
#endif
		UE_CLOG(!(TickFlags & ECookTickFlags::HideProgressDisplay) && (GCookProgressDisplay != (int32) ECookProgressDisplayMode::Nothing),
			LogCook, Display,
			TEXT("Cook Diagnostics: OpenFileHandles=%d, VirtualMemory=%dMiB"),
			OpenFileHandles, FPlatformMemory::GetStats().UsedVirtual / 1024 / 1024);
		LastDiagnosticsDisplayTime = CurrentTime;
	}
}

UCookOnTheFlyServer::ECookAction UCookOnTheFlyServer::DecideNextCookAction(UE::Cook::FTickStackData& StackData)
{
	if (IsCookByTheBookMode() && CookByTheBookOptions->bCancel)
	{
		return ECookAction::Cancel;
	}

	if (StackData.ResultFlags & COSR_RequiresGC)
	{
		// if we just cooked a map then don't process anything the rest of this tick
		return ECookAction::YieldTick;
	}
	else if (StackData.Timer.IsTimeUp())
	{
		return ECookAction::YieldTick;
	}

	UE::Cook::FRequestQueue& RequestQueue = PackageDatas->GetRequestQueue();
	if (RequestQueue.GetRequestClusters().Num() != 0 || RequestQueue.GetUnclusteredRequests().Num() != 0)
	{
		return ECookAction::Request;
	}

	UE::Cook::FPackageDataMonitor& Monitor = PackageDatas->GetMonitor();
	if (Monitor.GetNumUrgent() > 0)
	{
		if (Monitor.GetNumUrgent(UE::Cook::EPackageState::Save) > 0)
		{
			return ECookAction::Save;
		}
		else if (Monitor.GetNumUrgent(UE::Cook::EPackageState::LoadPrepare) > 0)
		{
			return ECookAction::Load;
		}
		else if (Monitor.GetNumUrgent(UE::Cook::EPackageState::LoadReady) > 0)
		{
			return ECookAction::Load;
		}
		else if (Monitor.GetNumUrgent(UE::Cook::EPackageState::Request) > 0)
		{
			return ECookAction::Request;
		}
		else
		{
			checkf(false, TEXT("Urgent request is in state not yet handled by DecideNextCookAction"));
		}
	}

	int32 NumSaves = PackageDatas->GetSaveQueue().Num();
	bool bSaveAvailable = ((!bSaveBusy) & (NumSaves > 0)) != 0;
	if (bSaveAvailable & (NumSaves > static_cast<int32>(DesiredSaveQueueLength)))
	{
		return ECookAction::SaveLimited;
	}

	int32 NumLoads = PackageDatas->GetLoadReadyQueue().Num() + PackageDatas->GetLoadPrepareQueue().Num();
	bool bLoadAvailable = ((!bLoadBusy) & (NumLoads > 0)) != 0;
	if (bLoadAvailable & (NumLoads > static_cast<int32>(DesiredLoadQueueLength)))
	{
		return ECookAction::LoadLimited;
	}

	if (!RequestQueue.IsReadyRequestsEmpty())
	{
		return ECookAction::Request;
	}

	if (bSaveAvailable)
	{
		return ECookAction::Save;
	}

	if (bLoadAvailable)
	{
		return ECookAction::Load;
	}

	if (bSaveBusy & (NumSaves > 0))
	{
		const float CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - SaveBusyTimeLastRetry > GCookProgressRetryBusyTime)
		{
			SaveBusyTimeLastRetry = CurrentTime;
			return ECookAction::Save;
		}
	}
	if (bLoadBusy & (NumLoads > 0))
	{
		const float CurrentTime = FPlatformTime::Seconds();
		if (CurrentTime - LoadBusyTimeLastRetry > GCookProgressRetryBusyTime)
		{
			LoadBusyTimeLastRetry = CurrentTime;
			return ECookAction::Load;
		}
	}

	if (PackageDatas->GetMonitor().GetNumInProgress() > 0)
	{
		return ECookAction::YieldTick;
	}

	return ECookAction::Done;
}

void UCookOnTheFlyServer::PumpExternalRequests(const UE::Cook::FCookerTimer& CookerTimer)
{
	if (!ExternalRequests->HasRequests())
	{
		return;
	}
	UE_SCOPED_COOKTIMER(PumpExternalRequests);

	TArray<UE::Cook::FFilePlatformRequest> BuildRequests;
	TArray<UE::Cook::FSchedulerCallback> SchedulerCallbacks;
	UE::Cook::EExternalRequestType RequestType;
	while (!CookerTimer.IsTimeUp())
	{
		BuildRequests.Reset();
		SchedulerCallbacks.Reset();
		RequestType = ExternalRequests->DequeueNextCluster(SchedulerCallbacks, BuildRequests);
		if (RequestType == UE::Cook::EExternalRequestType::None)
		{
			// No more requests to process
			break;
		}
		else if (RequestType == UE::Cook::EExternalRequestType::Callback)
		{
			// An array of TickCommands to process; execute through them all
			for (UE::Cook::FSchedulerCallback& SchedulerCallback : SchedulerCallbacks)
			{
				SchedulerCallback();
			}
		}
		else
		{
			check(RequestType == UE::Cook::EExternalRequestType::Cook && BuildRequests.Num() > 0);
#if PROFILE_NETWORK
			if (NetworkRequestEvent)
			{
				NetworkRequestEvent->Trigger();
			}
#endif
			bool bRequestsAreUrgent = IsCookOnTheFlyMode();
			TRingBuffer<UE::Cook::FRequestCluster>& RequestClusters = PackageDatas->GetRequestQueue().GetRequestClusters();
			UE::Cook::FRequestCluster::AddClusters(*this, MoveTemp(BuildRequests), bRequestsAreUrgent, RequestClusters);
		}
	}
}

bool UCookOnTheFlyServer::TryCreateRequestCluster(UE::Cook::FPackageData& PackageData)
{
	using namespace UE::Cook;
	if (!PackageData.AreAllRequestedPlatformsExplored())
	{
		PackageData.SendToState(EPackageState::Request, ESendFlags::QueueAdd);
		return true;
	}
	return false;
}

void UCookOnTheFlyServer::PumpRequests(UE::Cook::FTickStackData& StackData, int32& OutNumPushed)
{
	UE_SCOPED_COOKTIMER(PumpRequests);
	using namespace UE::Cook;

	OutNumPushed = 0;
	FRequestQueue& RequestQueue = PackageDatas->GetRequestQueue();
	FPackageDataSet& UnclusteredRequests = RequestQueue.GetUnclusteredRequests();
	TRingBuffer<FRequestCluster>& RequestClusters = RequestQueue.GetRequestClusters();
	const FCookerTimer& CookerTimer = StackData.Timer;
	if (!UnclusteredRequests.IsEmpty())
	{
		FRequestCluster::AddClusters(*this, UnclusteredRequests, RequestClusters, RequestQueue);
		UnclusteredRequests.Empty();
		if (CookerTimer.IsTimeUp())
		{
			return;
		}
	}

	while (RequestClusters.Num() > 0)
	{
		FRequestCluster& RequestCluster = RequestClusters.First();
		bool bComplete = false;
		RequestCluster.Process(CookerTimer, bComplete);
		if (bComplete)
		{
			TArray<FPackageData*> RequestsToLoad;
			TArray<FPackageData*> RequestsToDemote;
			RequestCluster.ClearAndDetachOwnedPackageDatas(RequestsToLoad, RequestsToDemote);
			for (FPackageData* PackageData : RequestsToLoad)
			{
				RequestQueue.AddReadyRequest(PackageData);
			}
			for (FPackageData* PackageData : RequestsToDemote)
			{
				PackageData->SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
			}
			RequestClusters.PopFront();
			if (IsCookByTheBookMode() && bHybridIterativeDebug)
			{
				for (FPackageData* PackageData : *PackageDatas)
				{
					if (!PackageData->AreAllRequestedPlatformsExplored())
					{
						UE_LOG(LogCook, Warning, TEXT("Missing dependency: existing requested Package %s was not explored in the first cluster."),
							*WriteToString<256>(PackageData->GetPackageName()));
					}
				}
				PackageDatas->SetLogDiscoveredPackages(true); // Turn this on for the rest of the cook after the initial cluster
			}
		}
		if (CookerTimer.IsTimeUp())
		{
			return;
		}
	}

	COOK_STAT(DetailedCookStats::PeakRequestQueueSize = FMath::Max(DetailedCookStats::PeakRequestQueueSize, static_cast<int32>(RequestQueue.ReadyRequestsNum())));
	uint32 NumInBatch = 0;
	while (!RequestQueue.IsReadyRequestsEmpty() && NumInBatch < RequestBatchSize)
	{
		FPackageData* PackageData = RequestQueue.PopReadyRequest();
		FPoppedPackageDataScope Scope(*PackageData);
		// Exploring dependencies in ReadRequests is legacy behavior and is allowed even if bPreexploreDependenciesEnabled  is false
		if (TryCreateRequestCluster(*PackageData))
		{
			continue;
		}
		if (PackageData->AreAllRequestedPlatformsCooked(true /* bAllowFailedCooks */))
		{
#if DEBUG_COOKONTHEFLY
			UE_LOG(LogCook, Display, TEXT("Package for platform already cooked %s, discarding request"), *PackageData.GetFileName().ToString());
#endif
			PackageData->SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
			continue;
		}
		PackageData->SendToState(EPackageState::LoadPrepare, ESendFlags::QueueAdd);
		++NumInBatch;
	}
	OutNumPushed += NumInBatch;
}

void UCookOnTheFlyServer::PumpLoads(UE::Cook::FTickStackData& StackData, uint32 DesiredQueueLength, int32& OutNumPushed, bool& bOutBusy)
{
	using namespace UE::Cook;
	FPackageDataQueue& LoadReadyQueue = PackageDatas->GetLoadReadyQueue();
	FLoadPrepareQueue& LoadPrepareQueue = PackageDatas->GetLoadPrepareQueue();
	FPackageDataMonitor& Monitor = PackageDatas->GetMonitor();
	bool bIsUrgentInProgress = Monitor.GetNumUrgent() > 0;
	OutNumPushed = 0;
	bOutBusy = false;

	// Process loads until we reduce the queue size down to the desired size or we hit the max number of loads per batch
	// We do not want to load too many packages without saving because if we hit the memory limit and GC every package
	// we load will have to be loaded again
	while (LoadReadyQueue.Num() + LoadPrepareQueue.Num() > static_cast<int32>(DesiredQueueLength) &&
		OutNumPushed < LoadBatchSize)
	{
		if (StackData.Timer.IsTimeUp())
		{
			return;
		}
		if (bIsUrgentInProgress && !Monitor.GetNumUrgent(EPackageState::LoadPrepare) && !Monitor.GetNumUrgent(EPackageState::LoadReady))
		{
			return;
		}
		COOK_STAT(DetailedCookStats::PeakLoadQueueSize = FMath::Max(DetailedCookStats::PeakLoadQueueSize, LoadPrepareQueue.Num() + LoadReadyQueue.Num()));
		PumpPreloadStarts(); // PumpPreloadStarts after every load so that we keep adding preloads ahead of our need for them

		if (LoadReadyQueue.IsEmpty())
		{
			PumpPreloadCompletes();
			if (LoadReadyQueue.IsEmpty())
			{
				if (!LoadPrepareQueue.IsEmpty())
				{
					bOutBusy = true;
				}
				break;
			}
		}

		FPackageData& PackageData(*LoadReadyQueue.PopFrontValue());
		FPoppedPackageDataScope Scope(PackageData);
		if (bPreexploreDependenciesEnabled && TryCreateRequestCluster(PackageData))
		{
			continue;
		}

		int32 NumPushed;
		LoadPackageInQueue(PackageData, StackData.ResultFlags, NumPushed);
		OutNumPushed += NumPushed;
		ProcessUnsolicitedPackages(); // May add new packages into the LoadQueue

		if (HasExceededMaxMemory())
		{
			StackData.ResultFlags |= COSR_RequiresGC;
			return;
		}
	}
}

void UCookOnTheFlyServer::PumpPreloadCompletes()
{
	using namespace UE::Cook;

	FPackageDataQueue& PreloadingQueue = PackageDatas->GetLoadPrepareQueue().PreloadingQueue;
	const bool bLocalPreloadingEnabled = bPreloadingEnabled;
	while (!PreloadingQueue.IsEmpty())
	{
		FPackageData* PackageData = PreloadingQueue.First();
		if (!bLocalPreloadingEnabled || PackageData->TryPreload())
		{
			// Ready to go
			PreloadingQueue.PopFront();
			PackageData->SendToState(EPackageState::LoadReady, ESendFlags::QueueAdd);
			continue;
		}
		break;
	}
}

void UCookOnTheFlyServer::PumpPreloadStarts()
{
	using namespace UE::Cook;

	FPackageDataMonitor& Monitor = PackageDatas->GetMonitor();
	FLoadPrepareQueue& LoadPrepareQueue = PackageDatas->GetLoadPrepareQueue();
	FPackageDataQueue& PreloadingQueue = LoadPrepareQueue.PreloadingQueue;
	FPackageDataQueue& EntryQueue = LoadPrepareQueue.EntryQueue;

	const bool bLocalPreloadingEnabled = bPreloadingEnabled;
	while (!EntryQueue.IsEmpty() && Monitor.GetNumPreloadAllocated() < static_cast<int32>(MaxPreloadAllocated))
	{
		FPackageData* PackageData = EntryQueue.PopFrontValue();
		if (bPreexploreDependenciesEnabled  && TryCreateRequestCluster(*PackageData))
		{
			continue;
		}
		if (bLocalPreloadingEnabled)
		{
			PackageData->TryPreload();
		}
		PreloadingQueue.Add(PackageData);
	}
}

namespace UE::Cook
{
struct FPopulatePackageContext
{
	explicit FPopulatePackageContext(FGeneratorPackage* InGeneratorStruct, UPackage* InOwnerPackage)
		: GeneratorStruct(InGeneratorStruct)
		, OwnerPackage(InOwnerPackage)
	{
	}

	// Input variables
	FGeneratorPackage* GeneratorStruct = nullptr;
	const UPackage* OwnerPackage = nullptr;
	FGeneratorPackage::FGeneratedStruct* GeneratedStruct = nullptr;

	// Cache of initialized-on-demand variables
	UObject* OwnerObject = nullptr;
	FString GeneratorPackagePath;
	FString GeneratorPackageShortName;
	FString GeneratedCookedRootPath;
};

}
void UCookOnTheFlyServer::LoadPackageInQueue(UE::Cook::FPackageData& PackageData, uint32& ResultFlags, int32& OutNumPushed)
{
	using namespace UE::Cook;

	UPackage* LoadedPackage = nullptr;
	OutNumPushed = 0;

	FName PackageFileName(PackageData.GetFileName());
	if (!PackageData.IsGenerated())
	{
		bool bLoadFullySuccessful = LoadPackageForCooking(PackageData, LoadedPackage);
		if (!bLoadFullySuccessful)
		{
			ResultFlags |= COSR_ErrorLoadingPackage;
			UE_LOG(LogCook, Verbose, TEXT("Not cooking package %s"), *PackageFileName.ToString());
			RejectPackageToLoad(PackageData, TEXT("failed to load"));
			return;
		}
		check(LoadedPackage != nullptr && LoadedPackage->IsFullyLoaded());

		if (LoadedPackage->GetFName() != PackageData.GetPackageName())
		{
			// The PackageName is not the name that we loaded. This can happen due to CoreRedirects.
			// We refuse to cook requests for packages that no longer exist in PumpExternalRequests, but it is possible
			// that a CoreRedirect exists from a (externally requested or requested as a reference) package that still exists.
			// Mark the original PackageName as cooked for all platforms and send a request to cook the new FileName
			FPackageData& OtherPackageData = PackageDatas->AddPackageDataByPackageNameChecked(LoadedPackage->GetFName());
			UE_LOG(LogCook, Verbose, TEXT("Request for %s received going to save %s"), *PackageFileName.ToString(),
				*OtherPackageData.GetFileName().ToString());
			TArray<const ITargetPlatform*, TInlineAllocator<ExpectedMaxNumPlatforms>> RequestedPlatforms;
			PackageData.GetRequestedPlatforms(RequestedPlatforms);
			OtherPackageData.UpdateRequestData(RequestedPlatforms, PackageData.GetIsUrgent(), FCompletionCallback(),
				FInstigator(PackageData.GetInstigator()));

			PackageData.SetPlatformsCooked(PlatformManager->GetSessionPlatforms(), true);
			RejectPackageToLoad(PackageData, TEXT("is redirected to another filename"));
			return;
		}
	}
	else
	{
		FGeneratorPackage* GeneratorStruct = PackageData.GetGeneratedOwner();
		if (!GeneratorStruct)
		{
			UE_LOG(LogCook, Error, TEXT("Package %s is an out-of-date generated package with a no-longer-available generator. It can not be loaded."), *PackageFileName.ToString());
			RejectPackageToLoad(PackageData, TEXT("is an orphaned generated package"));
			return;
		}
		FGeneratorPackage::FGeneratedStruct* GeneratedStruct = GeneratorStruct->FindGeneratedStruct(&PackageData);
		if (!GeneratedStruct)
		{
			UE_LOG(LogCook, Error, TEXT("Package %s is a generated package but its generator no longer has a record of it. It can not be loaded."), *PackageFileName.ToString());
			RejectPackageToLoad(PackageData, TEXT("is an orphaned generated package"));
			return;
		}

		FPackageData& OwnerPackageData = const_cast<FPackageData&>(GeneratorStruct->GetOwner());
		UPackage* OwnerPackage;
		bool bLoadFullySuccessful = LoadPackageForCooking(OwnerPackageData, OwnerPackage, nullptr, &PackageData);
		if (!bLoadFullySuccessful)
		{
			ResultFlags |= COSR_ErrorLoadingPackage;
			UE_LOG(LogCook, Error, TEXT("Package %s is a generated package and we could not load it's generator package %s. It can not be loaded."),
				*PackageFileName.ToString(), *OwnerPackageData.GetFileName().ToString());
			RejectPackageToLoad(PackageData, TEXT("is a generated package which could not load its generator"));
			return;
		}
		FPopulatePackageContext Context(GeneratorStruct, OwnerPackage);
		Context.GeneratedStruct = GeneratedStruct;
		LoadedPackage = TryPopulateGeneratedPackage(Context);
		if (!LoadedPackage)
		{
			RejectPackageToLoad(PackageData, TEXT("is a generated package which could not be populated"));
			return;
		}
	}

	if (PackageData.AreAllRequestedPlatformsCooked(true))
	{
		// Already cooked. This can happen if we needed to load a package that was previously cooked and garbage collected because it is a loaddependency of a new request.
		// Send the package back to idle, nothing further to do with it.
		PackageData.SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
	}
	else
	{
		PackageData.SetPackage(LoadedPackage);
		PackageData.SendToState(EPackageState::Save, ESendFlags::QueueAdd);
		++OutNumPushed;
	}
}

void UCookOnTheFlyServer::RejectPackageToLoad(UE::Cook::FPackageData& PackageData, const TCHAR* Reason)
{
	// make sure this package doesn't exist
	for (const TPair<const ITargetPlatform*, UE::Cook::FPackageData::FPlatformData>& Pair : PackageData.GetPlatformDatas())
	{
		if (!Pair.Value.bRequested)
		{
			continue;
		}
		const ITargetPlatform* TargetPlatform = Pair.Key;

		const FString SandboxFilename = ConvertToFullSandboxPath(PackageData.GetFileName().ToString(), true, TargetPlatform->PlatformName());
		if (IFileManager::Get().FileExists(*SandboxFilename))
		{
			// if we find the file this means it was cooked on a previous cook, however source package can't be found now. 
			// this could be because the source package was deleted or renamed, and we are using iterative cooking
			// perhaps in this case we should delete it?
			UE_LOG(LogCook, Warning, TEXT("Found cooked file '%s' which shouldn't exist as it %s."), *SandboxFilename, Reason);
			IFileManager::Get().Delete(*SandboxFilename);
		}
	}
	PackageData.SendToState(UE::Cook::EPackageState::Idle, UE::Cook::ESendFlags::QueueAdd);
}

//////////////////////////////////////////////////////////////////////////

UE::Cook::FPackageData* UCookOnTheFlyServer::QueueDiscoveredPackage(UPackage* Package,
	UE::Cook::FInstigator&& Instigator, bool* bOutWasInProgress)
{
	check(Package != nullptr);

	UE::Cook::FPackageData* PackageData = PackageDatas->TryAddPackageDataByPackageName(Package->GetFName());
	if (!PackageData)
	{
		return nullptr;	// Getting the PackageData will fail if e.g. it is a script package
	}

	if (bOutWasInProgress)
	{
		*bOutWasInProgress = PackageData->IsInProgress();
	}
	QueueDiscoveredPackageData(*PackageData, MoveTemp(Instigator), true /* bIsLoadReady */);
	return PackageData;
}

void UCookOnTheFlyServer::QueueDiscoveredPackageData(UE::Cook::FPackageData& PackageData,
	UE::Cook::FInstigator&& Instigator, bool bLoadReady)
{
	const TArray<const ITargetPlatform*>& TargetPlatforms = PlatformManager->GetSessionPlatforms();
	if (PackageData.HasAllCookedPlatforms(TargetPlatforms, true /* bIncludeFailed */))
	{
		// All SessionPlatforms have already been cooked for the package, so we don't need to save it again
		return;
	}

	if (!PackageData.IsInProgress() &&
		(PackageData.IsGenerated() || !IsCookByTheBookMode() || !CookByTheBookOptions->bSkipHardReferences))
	{
		PackageData.SetRequestData(TargetPlatforms, /*bIsUrgent*/ false, UE::Cook::FCompletionCallback(),
			MoveTemp(Instigator));
		if (bLoadReady)
		{
			// Send this package into the LoadReadyQueue to fully load it and send it on to the SaveQueue
			PackageData.SendToState(UE::Cook::EPackageState::LoadReady, UE::Cook::ESendFlags::QueueRemove);
			// Send it to the front of the LoadReadyQueue since it is mostly loaded already
			PackageDatas->GetLoadReadyQueue().AddFront(&PackageData);
		}
		else
		{
			PackageData.SendToState(UE::Cook::EPackageState::Request, UE::Cook::ESendFlags::QueueAddAndRemove);
		}
	}
}

FName GInstigatorUpdatePackageFilter(TEXT("UpdatePackageFilter"));

void UCookOnTheFlyServer::UpdatePackageFilter()
{
	if (!bPackageFilterDirty)
	{
		return;
	}
	bPackageFilterDirty = false;

	UE_SCOPED_COOKTIMER(UpdatePackageFilter);
	const TArray<const ITargetPlatform*>& TargetPlatforms = PlatformManager->GetSessionPlatforms();
	for (UPackage* Package : PackageTracker->LoadedPackages)
	{
		UE::Cook::FPackageData* PackageData = QueueDiscoveredPackage(Package,
			UE::Cook::FInstigator(UE::Cook::EInstigator::Unspecified, GInstigatorUpdatePackageFilter));
		if (PackageData && PackageData->IsInProgress())
		{
			PackageData->UpdateRequestData(TargetPlatforms, /*bIsUrgent*/ false, UE::Cook::FCompletionCallback(),
				UE::Cook::FInstigator(UE::Cook::EInstigator::Unspecified, GInstigatorUpdatePackageFilter));
		}
	}
}

void UCookOnTheFlyServer::OnRemoveSessionPlatform(const ITargetPlatform* TargetPlatform)
{
	PackageDatas->OnRemoveSessionPlatform(TargetPlatform);
	ExternalRequests->OnRemoveSessionPlatform(TargetPlatform);
}

void UCookOnTheFlyServer::TickNetwork()
{
	// Only CookOnTheFly handles network requests
	// It is not safe to call PruneUnreferencedSessionPlatforms in CookByTheBook because StartCookByTheBook does not AddRef its session platforms
	if (IsCookOnTheFlyMode())
	{
		if (IsInSession() && !bCookOnTheFlyExternalRequests)
		{
			PlatformManager->PruneUnreferencedSessionPlatforms(*this);
		}
		else
		{
			// Process callbacks in case there is a callback pending that needs to create a session
			TArray<UE::Cook::FSchedulerCallback> Callbacks;
			if (ExternalRequests->DequeueCallbacks(Callbacks))
			{
				for (UE::Cook::FSchedulerCallback& Callback : Callbacks)
				{
					Callback();
				}
			}
		}
	}
}

UE::Cook::Private::FRegisteredCookPackageSplitter* UCookOnTheFlyServer::TryGetRegisteredCookPackageSplitter(UE::Cook::FPackageData& PackageData, UObject*& OutSplitDataObject, bool& bOutError)
{
	bOutError = false;
	OutSplitDataObject = nullptr;

	UE::Cook::Private::FRegisteredCookPackageSplitter* FoundSplitter = nullptr;
	UObject* FoundSplitDataObject = nullptr;

	TArray<UE::Cook::Private::FRegisteredCookPackageSplitter*> FoundRegisteredSplitters;

	for (FWeakObjectPtr& WeakObj : PackageData.GetCachedObjectsInOuter())
	{
		UObject* Obj = WeakObj.Get();
		if (!Obj)
		{
			continue;
		}
	
		FoundRegisteredSplitters.Reset();
		RegisteredSplitDataClasses.MultiFind(Obj->GetClass(), FoundRegisteredSplitters);

		for (UE::Cook::Private::FRegisteredCookPackageSplitter* Splitter : FoundRegisteredSplitters)
		{
			if (Splitter && Splitter->ShouldSplitPackage(Obj))
			{
				if (!Obj->HasAnyFlags(RF_Public))
				{
					UE_LOG(LogCook, Error, TEXT("SplitterData object %s must be publicly referenceable so we can keep them from being garbage collected"), *Obj->GetFullName());
					bOutError = true;
					return nullptr;
				}

				if (FoundSplitter)
				{
					UE_LOG(LogCook, Error, TEXT("Found more than one registered Cook Package Splitter for package %s."), *PackageData.GetPackageName().ToString());
					bOutError = true;
					return nullptr;
				}

				FoundSplitter = Splitter;
				FoundSplitDataObject = Obj;
			}
		}
	}

	OutSplitDataObject = FoundSplitDataObject;
	return FoundSplitter;
}

UE::Cook::FGeneratorPackage* UCookOnTheFlyServer::CreateGeneratorPackage(UE::Cook::FPackageData& PackageData, UObject* SplitDataObject, UE::Cook::Private::FRegisteredCookPackageSplitter* Splitter)
{
	check(Splitter);
	check(!PackageData.GetGeneratorPackage());

	// TODO: Add support for cooking in the editor. Possibly moot since we plan to deprecate cooking in the editor.
	if (IsCookingInEditor())
	{
		// CookPackageSplitters allow destructive changes to the generator package. e.g. moving UObjects out
		// of it into the streaming packages. To allow its use in the editor, we will need to make it non-destructive
		// (by e.g. copying to new packages), or restore the package after the changes have been made.
		UE_LOG(LogCook, Error, TEXT("Cooking in editor doesn't support Cook Package Splitters."));
		return nullptr;
	}

	UE_LOG(LogCook, Display, TEXT("Splitting Package %s with class %s acting on object %s."), *PackageData.GetPackageName().ToString(), *Splitter->GetSplitDataClass()->GetName(), *SplitDataObject->GetFullName());

	// Create instance of CookPackageSplitter class
	ICookPackageSplitter* SplitterInstance = Splitter->CreateInstance(SplitDataObject);
	if (!SplitterInstance)
	{
		UE_LOG(LogCook, Error, TEXT("Error instantiating Cook Package Splitter for object %s."), *SplitDataObject->GetFullName());
		return nullptr;
	}

	// Create a FGeneratorPackage helper object using this CookPackageSplitter instance and return it
	PackageData.CreateGeneratorPackage(SplitDataObject, SplitterInstance);
	UE::Cook::FGeneratorPackage* GeneratorPackage = PackageData.GetGeneratorPackage();
	return GeneratorPackage;
}

void UCookOnTheFlyServer::SplitPackage(UE::Cook::FGeneratorPackage* GeneratorStruct, bool& bOutCompleted, bool& bOutError)
{
	using namespace UE::Cook;

	bOutCompleted = false;
	bOutError = false;
	check(GeneratorStruct);
	ICookPackageSplitter* Splitter = GeneratorStruct->GetCookPackageSplitterInstance();
	UObject* SplitObject = GeneratorStruct->FindSplitDataObject();
	if (!SplitObject)
	{
		UE_LOG(LogCook, Error, TEXT("Could not find SplitDataObject %s"), *GeneratorStruct->GetSplitDataObjectName().ToString());
		bOutError = true;
		return;
	}

	if (!GeneratorStruct->HasGeneratedList())
	{
		// Call the splitter to generate the list
		if (!GeneratorStruct->TryGenerateList(SplitObject, *PackageDatas))
		{
			bOutError = true;
			return;
		}
		GeneratorStruct->SetGeneratedList();
	}

	if (!GeneratorStruct->HasClearedOldPackages())
	{
		for (const FGeneratorPackage::FGeneratedStruct& GeneratedStruct : GeneratorStruct->GetPackagesToGenerate())
		{
			const FString GeneratedPackageName = GeneratedStruct.PackageData->GetPackageName().ToString();
			if (FindObject<UPackage>(nullptr, *GeneratedPackageName))
			{
				if (GeneratorStruct->HasClearedOldPackagesWithGC())
				{
					UE_LOG(LogCook, Error, TEXT("PackageSplitter was unable to construct new generated packages because an old version of the package is already in memory and GC did not remove it. Splitter=%s, Generated=%s."),
						*GeneratorStruct->GetSplitDataObjectName().ToString(), *GeneratedStruct.RelativePath);
					bOutError = true;
					return;
				}
				else
				{
					GeneratorStruct->SetClearedOldPackagesWithGC();
					return;
				}
			}
		}

		GeneratorStruct->SetClearedOldPackages();
	}

	UPackage* Owner = GeneratorStruct->GetOwner().GetPackage();
	FName OwnerName = Owner->GetFName();
	if (!GeneratorStruct->HasQueuedGeneratedPackages())
	{
		for (const FGeneratorPackage::FGeneratedStruct& GeneratedStruct : GeneratorStruct->GetPackagesToGenerate())
		{
			FPackageData* GeneratedPackageData = GeneratedStruct.PackageData;
			GeneratedPackageData->ClearCookedPlatformData();
			QueueDiscoveredPackageData(*GeneratedPackageData, FInstigator(EInstigator::GeneratedPackage, OwnerName));
		}
		GeneratorStruct->SetQueuedGeneratedPackages();
	}

	TArrayView<FGeneratorPackage::FGeneratedStruct> PackagesToGenerate = GeneratorStruct->GetPackagesToGenerate();
	TArray<ICookPackageSplitter::FGeneratedPackageForPreSave> SplitterDatas;
	SplitterDatas.Reserve(PackagesToGenerate.Num());
	for (FGeneratorPackage::FGeneratedStruct& GeneratedStruct : PackagesToGenerate)
	{
		ICookPackageSplitter::FGeneratedPackageForPreSave& SplitterData = SplitterDatas.Emplace_GetRef();
		SplitterData.RelativePath = GeneratedStruct.RelativePath;
		SplitterData.bCreatedAsMap = GeneratedStruct.bCreateAsMap;

		const FString GeneratedPackageName = GeneratedStruct.PackageData->GetPackageName().ToString();
		SplitterData.Package = FindObject<UPackage>(nullptr, *GeneratedPackageName);
		if (!SplitterData.Package)
		{
			SplitterData.Package = GeneratorStruct->CreateGeneratedUPackage(GeneratedStruct, Owner, *GeneratedPackageName);
		}
	}

	UPackage* OwnerPackage = GeneratorStruct->GetOwner().GetPackage();
	check(OwnerPackage);
	GeneratorStruct->GetCookPackageSplitterInstance()->PreSaveGeneratorPackage(OwnerPackage, SplitObject, SplitterDatas);

	bOutCompleted = true;
}

UPackage* UCookOnTheFlyServer::TryPopulateGeneratedPackage(UE::Cook::FPopulatePackageContext& Context)
{
	using namespace UE::Cook;

	FGeneratorPackage* GeneratorStruct = Context.GeneratorStruct;
	FGeneratorPackage::FGeneratedStruct& GeneratedStruct = *Context.GeneratedStruct;
	const UPackage* OwnerPackage = Context.OwnerPackage;
	UE::Cook::FPackageData* GeneratedPackageData = Context.GeneratedStruct->PackageData;
	const FString GeneratedPackageName = GeneratedPackageData->GetPackageName().ToString();

	if (Context.OwnerObject == nullptr)
	{
		Context.OwnerObject = GeneratorStruct->FindSplitDataObject();
		if (!Context.OwnerObject)
		{
			UE_LOG(LogCook, Error, TEXT("PopulateGeneratedPacakge could not find the original splitting object. Generated package can not be created. Splitter=%s, Generated=%s."),
				*GeneratorStruct->GetSplitDataObjectName().ToString(), *GeneratedPackageName);
			return nullptr;
		}
	}

	if (Context.GeneratorPackagePath.IsEmpty())
	{
		Context.GeneratorPackagePath = FPackageName::GetLongPackagePath(Context.OwnerPackage->GetPathName());
		Context.GeneratorPackageShortName = FPackageName::GetShortName(Context.OwnerPackage->GetName());
		Context.GeneratedCookedRootPath = FPaths::RemoveDuplicateSlashes(FString::Printf(TEXT("/%s/%s/%s/"),
			*Context.GeneratorPackagePath, *Context.GeneratorPackageShortName, UE::Cook::GeneratedPackageSubPath));
	}

	check(GeneratedPackageData); // Caller already checked this

	ICookPackageSplitter* Splitter = GeneratorStruct->GetCookPackageSplitterInstance();

	// Create package
	UPackage* GeneratedPackage = FindObject<UPackage>(nullptr, *GeneratedPackageName);
	bool bPopulatedByPreSave = false;
	if (GeneratedPackage)
	{
		if (!GeneratedStruct.bHasCreatedPackage)
		{
			UE_LOG(LogCook, Error, TEXT("PackageSplitter found an existing copy of a package it was trying to populate;")
				TEXT("this is unexpected since garbage has been collected and the package should have been unreferenced so it should have been collected.")
				TEXT("Splitter=%s, Generated=%s."),
				*GeneratorStruct->GetSplitDataObjectName().ToString(), *GeneratedPackageName);
			EReferenceChainSearchMode SearchMode = EReferenceChainSearchMode::Shortest
				| EReferenceChainSearchMode::PrintAllResults
				| EReferenceChainSearchMode::FullChain;
			FReferenceChainSearch RefChainSearch(GeneratedPackage, SearchMode);
			return nullptr;
		}
		// Otherwise this is the package that was created and passed to presave, and it is still valid because there has not been a GC since
		// we created it. Mark its state and use it
		bPopulatedByPreSave = true;
	}
	else
	{
		GeneratedPackage = GeneratorStruct->CreateGeneratedUPackage(GeneratedStruct, OwnerPackage, *GeneratedPackageName);
	}

	// Populate package using CookPackageSplitterInstance and pass GeneratedPackage's cooked name for it to
	// properly setup any internal reference to this package (SoftObjectPaths or others)
	ICookPackageSplitter::FGeneratedPackageForPopulate PopulateData;
	PopulateData.RelativePath = GeneratedStruct.RelativePath;
	PopulateData.Package = GeneratedPackage;
	PopulateData.bPopulatedByPreSave = bPopulatedByPreSave;
	PopulateData.bCreatedAsMap = GeneratedStruct.bCreateAsMap;
	bool bWasOwnerReloaded = GeneratorStruct->GetWasOwnerReloaded();
	GeneratorStruct->ClearWasOwnerReloaded();
	if (!Splitter->TryPopulatePackage(OwnerPackage, Context.OwnerObject, PopulateData, bWasOwnerReloaded))
	{
		UE_LOG(LogCook, Error, TEXT("PackageSplitter returned false from TryPopulatePackage. Splitter=%s, Generated=%s."),
			*GeneratorStruct->GetSplitDataObjectName().ToString(), *GeneratedPackageName);
		return nullptr;
	}
	bool bPackageIsMap = GeneratedPackage->ContainsMap();
	if (bPackageIsMap != GeneratedStruct.bCreateAsMap)
	{
		UE_LOG(LogCook, Error, TEXT("PackageSplitter specified generated package is %s in GetGenerateList results, but then in TryPopulatePackage created it as %s. Splitter=%s, Generated=%s."),
			(GeneratedStruct.bCreateAsMap ? TEXT("map") : TEXT("uasset")), (bPackageIsMap ? TEXT("map") : TEXT("uasset")),
			*GeneratorStruct->GetSplitDataObjectName().ToString(), *GeneratedPackageName);
		return nullptr;
	}
	GeneratedPackage->MarkAsFullyLoaded();

	return GeneratedPackage;
}


bool UCookOnTheFlyServer::BeginPrepareSave(UE::Cook::FPackageData& PackageData, UE::Cook::FCookerTimer& Timer, bool bIsPreCaching)
{
	if (PackageData.GetCookedPlatformDataCalled())
	{
		return true;
	}

	if (PackageData.GetHasBeginPrepareSaveFailed())
	{
		return false;
	}

	if (!PackageData.GetCookedPlatformDataStarted())
	{
		if (PackageData.GetNumPendingCookedPlatformData() > 0)
		{
			// A previous Save was started and deleted after some calls to BeginCacheForCookedPlatformData occurred, and some of those objects have still not returned true for IsCachedCookedPlatformDataLoaded
			// We need to wait for all of pending async calls from the cancelled save to finish before we start the new ones
			return false;
		}
		PackageData.SetCookedPlatformDataStarted(true);
	}

	UE_SCOPED_HIERARCHICAL_COOKTIMER_AND_DURATION(BeginPrepareSave, DetailedCookStats::TickCookOnTheSideBeginPrepareSaveTimeSec);

#if DEBUG_COOKONTHEFLY 
	UE_LOG(LogCook, Display, TEXT("Caching objects for package %s"), *PackageData.GetPackageName().ToString());
#endif
	UPackage* Package = PackageData.GetPackage();
	check(Package && Package->IsFullyLoaded());
	check(PackageData.GetState() == UE::Cook::EPackageState::Save);
	PackageData.CreateObjectCache();

	if (!PackageData.HasCompletedGeneration())
	{
		// Check for Splitting the package; this needs to happen before we make any of the BeginCacheForCookedPlatformData calls in the package
		bool bError = false;

		UE::Cook::FGeneratorPackage* Generator = nullptr;
		if (!PackageData.HasInitializedGeneratorSave())
		{
			PackageData.SetInitializedGeneratorSave(true);
			PackageData.DestroyGeneratorPackage();
			UObject* SplitDataObject = nullptr;
			UE::Cook::Private::FRegisteredCookPackageSplitter* Splitter = TryGetRegisteredCookPackageSplitter(PackageData, SplitDataObject, bError);
			if (bError)
			{
				PackageData.SetHasBeginPrepareSaveFailed(true);
				return false;
			}

			if (Splitter)
			{
				// Found a splitter, create generator package
				Generator = CreateGeneratorPackage(PackageData, SplitDataObject, Splitter);
				if (!Generator)
				{
					PackageData.SetHasBeginPrepareSaveFailed(true);
					return false;
				}
			}
		}
		else
		{
			Generator = PackageData.GetGeneratorPackage();
		}
		if (Generator)
		{
			// Don't split a generator package when precaching
			if (bIsPreCaching)
			{
				return false;
			}
			bool bCompleted = false;
			SplitPackage(Generator, bCompleted, bError);
			if (bError)
			{
				// SplitPackage failure marks the package data for PumpSaves to handle this as an error and to put it in idle state
				PackageData.SetHasBeginPrepareSaveFailed(true);
				return false;
			}
			if (!bCompleted)
			{
				return false;
			}
		}
		PackageData.SetCompletedGeneration(true);
	}

	// Note that we cache cooked data for all requested platforms, rather than only for the requested platforms that have not cooked yet.  This allows
	// us to avoid the complexity of needing to cancel the Save and keep track of the old list of uncooked platforms whenever the cooked platforms change
	// while BeginPrepareSave is active.
	// Currently this does not cause significant cost since saving new platforms with some platforms already saved is a rare operation.

	int32& CookedPlatformDataNextIndex = PackageData.GetCookedPlatformDataNextIndex();
	if (CookedPlatformDataNextIndex == 0)
	{
		if (!BuildDefinitions->TryRemovePendingBuilds(PackageData.GetPackageName()))
		{
			// Builds are in progress; wait for them to complete
			return false;
		}
	}

	TArray<FWeakObjectPtr>& CachedObjectsInOuter = PackageData.GetCachedObjectsInOuter();
	FWeakObjectPtr* CachedObjectsInOuterData = CachedObjectsInOuter.GetData();
	TArray<const ITargetPlatform*, TInlineAllocator<ExpectedMaxNumPlatforms>> TargetPlatforms;
	PackageData.GetRequestedPlatforms(TargetPlatforms);
	int NumPlatforms = TargetPlatforms.Num();
	int NumIndexes = CachedObjectsInOuter.Num() * NumPlatforms;
	UE_TRACK_REFERENCING_PACKAGE_SCOPED(Package, PackageAccessTrackingOps::NAME_CookerBuildObject);
	while (CookedPlatformDataNextIndex < NumIndexes)
	{
		int ObjectIndex = CookedPlatformDataNextIndex / NumPlatforms;
		int PlatformIndex = CookedPlatformDataNextIndex - ObjectIndex * NumPlatforms;
		UObject* Obj = CachedObjectsInOuterData[ObjectIndex].Get();
		if (!Obj)
		{
			// Objects can be marked as pending kill even without a garbage collect, and our weakptr.get will return null for them, so we have to always check the WeakPtr before using it
			// Treat objects that have been marked as pending kill or deleted as no-longer-required for BeginCacheForCookedPlatformData and ClearAllCachedCookedPlatformData
			CachedObjectsInOuterData[ObjectIndex] = nullptr; // If the weakptr is merely pendingkill, set it to null explicitly so we don't think that we've called BeginCacheForCookedPlatformData on it if it gets unmarked pendingkill later
			++CookedPlatformDataNextIndex;
			continue;
		}
		const ITargetPlatform* TargetPlatform = TargetPlatforms[PlatformIndex];

		if (Obj->IsA(UMaterialInterface::StaticClass()))
		{
			if (GShaderCompilingManager->GetNumRemainingJobs() + 1 > MaxConcurrentShaderJobs)
			{
#if DEBUG_COOKONTHEFLY
				UE_LOG(LogCook, Display, TEXT("Delaying shader compilation of material %s"), *Obj->GetFullName());
#endif
				return false;
			}
		}

		const FName ClassFName = Obj->GetClass()->GetFName();
		int32* CurrentAsyncCache = CurrentAsyncCacheForType.Find(ClassFName);
		if (CurrentAsyncCache != nullptr)
		{
			if (*CurrentAsyncCache < 1)
			{
				return false;
			}
			*CurrentAsyncCache -= 1;
		}

		Obj->BeginCacheForCookedPlatformData(TargetPlatform);
		++CookedPlatformDataNextIndex;
		if (Obj->IsCachedCookedPlatformDataLoaded(TargetPlatform))
		{
			if (CurrentAsyncCache)
			{
				*CurrentAsyncCache += 1;
			}
		}
		else
		{
			bool bNeedsResourceRelease = CurrentAsyncCache != nullptr;
			PackageDatas->GetPendingCookedPlatformDatas().Emplace(Obj, TargetPlatform, PackageData, bNeedsResourceRelease, *this);
		}

		if (Timer.IsTimeUp())
		{
#if DEBUG_COOKONTHEFLY
			UE_LOG(LogCook, Display, TEXT("Object %s took too long to cache"), *Obj->GetFullName());
#endif
			return false;
		}
	}

	PackageData.SetCookedPlatformDataCalled(true);
	return true;
}

bool UCookOnTheFlyServer::FinishPrepareSave(UE::Cook::FPackageData& PackageData, UE::Cook::FCookerTimer& Timer)
{
	if (PackageData.GetCookedPlatformDataComplete())
	{
		return true;
	}

	if (!PackageData.GetCookedPlatformDataCalled())
	{
		if (!BeginPrepareSave(PackageData, Timer, /*bIsPreCaching*/ false))
		{
			return false;
		}
		check(PackageData.GetCookedPlatformDataCalled());
	}

	if (PackageData.GetNumPendingCookedPlatformData() > 0)
	{
		return false;
	}

	PackageData.SetCookedPlatformDataComplete(true);
	return true;
}

void UCookOnTheFlyServer::ReleaseCookedPlatformData(UE::Cook::FPackageData& PackageData, bool bCompletedSave)
{
	using namespace UE::Cook;

	if (!PackageData.GetCookedPlatformDataStarted())
	{
		PackageData.CheckCookedPlatformDataEmpty();
		return;
	}

	// For every Object on which we called BeginCacheForCookedPlatformData, we need to call ClearAllCachedCookedPlatformData
	if (PackageData.GetCookedPlatformDataComplete() && bCompletedSave)
	{
		// Since we have completed CookedPlatformData, we know we called BeginCacheForCookedPlatformData on all objects in the package, and none are pending
		if (!IsCookingInEditor()) // ClearAllCachedCookedPlatformData and WillNeverCacheCookedPlatformDataAgain calls are only used when not in editor
		{
			UE_SCOPED_HIERARCHICAL_COOKTIMER(ClearAllCachedCookedPlatformData);
			for (FWeakObjectPtr& WeakPtr: PackageData.GetCachedObjectsInOuter())
			{
				UObject* Object = WeakPtr.Get();
				if (Object)
				{
					Object->ClearAllCachedCookedPlatformData();
					if (CurrentCookMode == ECookMode::CookByTheBook)
					{
						Object->WillNeverCacheCookedPlatformDataAgain();
					}
				}
			}
		}
	}
	else 
	{
		// This is an exceptional flow handling case; we are releasing the CookedPlatformData before we called SavePackage
		// Note that even after we return from this function, some objects with pending IsCachedCookedPlatformDataLoaded calls may still exist for this Package in PackageDatas->GetPendingCookedPlatformDatas(),
		// and this PackageData may therefore still have GetNumPendingCookedPlatformData > 0
		if (!IsCookingInEditor()) // ClearAllCachedCookedPlatformData calls are only used when not in editor.
		{
			int32 NumPlatforms = PackageData.GetNumRequestedPlatforms();
			if (NumPlatforms > 0) // Shouldn't happen because PumpSaves checks for this, but avoid a divide by 0 if it does.
			{
				// We have only called BeginCacheForCookedPlatformData on Object,Platform pairs up to GetCookedPlatformDataNextIndex.
				// Further, some of those calls might still be pending.

				// Find all pending BeginCacheForCookedPlatformData for this FPackageData
				TMap<UObject*, TArray<FPendingCookedPlatformData*>> PendingObjects;
				for (FPendingCookedPlatformData& PendingCookedPlatformData : PackageDatas->GetPendingCookedPlatformDatas())
				{
					if (&PendingCookedPlatformData.PackageData == &PackageData && !PendingCookedPlatformData.PollIsComplete())
					{
						UObject* Object = PendingCookedPlatformData.Object.Get();
						check(Object); // Otherwise PollIsComplete would have returned true
						check(!PendingCookedPlatformData.bHasReleased); // bHasReleased should be false since PollIsComplete returned false
						PendingObjects.FindOrAdd(Object).Add(&PendingCookedPlatformData);
					}
				}

				// Iterate over all objects in the FPackageData up to GetCookedPlatformDataNextIndex
				TArray<FWeakObjectPtr>& CachedObjects = PackageData.GetCachedObjectsInOuter();
				int32 NumIndexes = PackageData.GetCookedPlatformDataNextIndex();
				check(NumIndexes <= NumPlatforms * CachedObjects.Num());
				// GetCookedPlatformDataNextIndex is a value in an inline iteration over the two-dimensional array of Objects x Platforms, in Object-major order.
				// We take the ceiling of NextIndex/NumPlatforms to get the number of objects.
				int32 NumObjects = (NumIndexes + NumPlatforms - 1) / NumPlatforms;
				for (int32 ObjectIndex = 0; ObjectIndex < NumObjects; ++ObjectIndex)
				{
					UObject* Object = CachedObjects[ObjectIndex].Get();
					if (!Object)
					{
						continue;
					}
					TArray<FPendingCookedPlatformData*>* PendingDatas = PendingObjects.Find(Object);
					if (!PendingDatas || PendingDatas->Num() == 0)
					{
						// No pending BeginCacheForCookedPlatformData calls for this object; clear it now.
						Object->ClearAllCachedCookedPlatformData();
					}
					else
					{
						// For any pending Objects, we add a CancelManager to the FPendingCookedPlatformData to call ClearAllCachedCookedPlatformData when the pending Object,Platform pairs for that object complete.
						FPendingCookedPlatformDataCancelManager* CancelManager = new FPendingCookedPlatformDataCancelManager();
						CancelManager->NumPendingPlatforms = PendingDatas->Num();
						for (FPendingCookedPlatformData* PendingCookedPlatformData : *PendingDatas)
						{
							// We never start a new package until after the previous cancel finished, so all of the FPendingCookedPlatformData for the PlatformData we are cancelling can not have been cancelled before.  We would leak the CancelManager if we overwrote it here.
							check(PendingCookedPlatformData->CancelManager == nullptr);
							// If bHasReleaased on the PendingCookedPlatformData were already true, we would leak the CancelManager because the PendingCookedPlatformData would never call Release on it.
							check(!PendingCookedPlatformData->bHasReleased);
							PendingCookedPlatformData->CancelManager = CancelManager;
						}
					}
				}
			}
		}
	}

	if (bCompletedSave)
	{
		FGeneratorPackage* GeneratorPackage = PackageData.GetGeneratorPackage();
		if (GeneratorPackage && PackageData.HasCompletedGeneration())
		{
			UObject* SplitObject = GeneratorPackage->FindSplitDataObject();
			if (!SplitObject || !PackageData.GetPackage())
			{
				UE_LOG(LogCook, Error, TEXT("PackageSplitter: %s was GarbageCollected before we finished saving it. This will cause a failure later when we attempt to save it again and generate a second time. Splitter=%s."),
					(!PackageData.GetPackage() ? TEXT("UPackage") : TEXT("SplitDataObject")), *GeneratorPackage->GetSplitDataObjectName().ToString());
			}
			else
			{
				GeneratorPackage->GetCookPackageSplitterInstance()->PostSaveGeneratorPackage(PackageData.GetPackage(), SplitObject);
			}

			PackageData.ResetGenerationProgress();
			if (IsCookByTheBookMode() && GeneratorPackage->IsComplete())
			{
				PackageData.DestroyGeneratorPackage();
			}
		}
		else
		{
			PackageData.ResetGenerationProgress();
		}

		FGeneratorPackage* OwnerGeneratorPackage = PackageData.GetGeneratedOwner();
		if (OwnerGeneratorPackage)
		{
			OwnerGeneratorPackage->SetGeneratedSaved(PackageData);
		}
	}
	PackageData.ClearCookedPlatformData();

	if (bCompletedSave)
	{
		if (CurrentCookMode == ECookMode::CookByTheBook)
		{
			UPackage* Package = PackageData.GetPackage();
			if (Package && Package->GetLinker())
			{
				// Loaders and their handles can have large buffers held in process memory and in the system file cache from the
				// data that was loaded.  Keeping this for the lifetime of the cook is costly, so we try and unload it here.
				Package->GetLinker()->FlushCache();
			}
		}
	}
}

void UCookOnTheFlyServer::TickCancels()
{
	PackageDatas->PollPendingCookedPlatformDatas();
}

bool UCookOnTheFlyServer::LoadPackageForCooking(UE::Cook::FPackageData& PackageData, UPackage*& OutPackage, FString* LoadFromFilename,
	UE::Cook::FPackageData* ReportingPackageData)
{
	UE_SCOPED_HIERARCHICAL_COOKTIMER_AND_DURATION(LoadPackageForCooking, DetailedCookStats::TickCookOnTheSideLoadPackagesTimeSec);

	check(PackageTracker->LoadingPackageData == nullptr);
	PackageTracker->LoadingPackageData = &PackageData;
	ON_SCOPE_EXIT
	{
		PackageTracker->LoadingPackageData = nullptr;
	};

	FString PackageName = PackageData.GetPackageName().ToString();
	OutPackage = FindObject<UPackage>(nullptr, *PackageName);

	FString FileName(LoadFromFilename ? *LoadFromFilename : PackageData.GetFileName().ToString());
	FString ReportingFileName(ReportingPackageData ? ReportingPackageData->GetFileName().ToString() : FileName);
#if DEBUG_COOKONTHEFLY
	UE_LOG(LogCook, Display, TEXT("Processing request %s"), *ReportingFileName);
#endif
	static TSet<FString> CookWarningsList;
	if (CookWarningsList.Contains(FileName) == false)
	{
		CookWarningsList.Add(FileName);
		GOutputCookingWarnings = IsCookFlagSet(ECookInitializationFlags::OutputVerboseCookerWarnings);
	}

	bool bSuccess = true;
	//  if the package is not yet fully loaded then fully load it
	if (!IsValid(OutPackage) || !OutPackage->IsFullyLoaded())
	{
		bool bWasPartiallyLoaded = OutPackage != nullptr;
		GIsCookerLoadingPackage = true;
		UPackage* LoadIntoPackage = nullptr;
		if (LoadFromFilename)
		{
			// When loading from a specified filename, we provide the packagename to load into via LoadPackage's first argument - the InOuter package
			if (OutPackage)
			{
				LoadIntoPackage = OutPackage;
			}
			else
			{
				LoadIntoPackage = CreatePackage(*PackageName);
			}
		}

		UPackage* LoadedPackage = LoadPackage(LoadIntoPackage, *FileName, LOAD_None);
		if (IsValid(LoadedPackage) && LoadedPackage->IsFullyLoaded())
		{
			OutPackage = LoadedPackage;

			if (bWasPartiallyLoaded)
			{
				// If fully loading has caused a blueprint to be regenerated, make sure we eliminate all meta data outside the package
				UMetaData* MetaData = LoadedPackage->GetMetaData();
				MetaData->RemoveMetaDataOutsidePackage();
			}
		}
		else
		{
			bSuccess = false;
		}

		++this->StatLoadedPackageCount;

		GIsCookerLoadingPackage = false;
	}
#if DEBUG_COOKONTHEFLY
	else
	{
		UE_LOG(LogCook, Display, TEXT("Package already loaded %s avoiding reload"), *ReportingFileName);
	}
#endif

	if (!bSuccess)
	{
		if ((!IsCookOnTheFlyMode()) || (!IsCookingInEditor()))
		{
			LogCookerMessage(FString::Printf(TEXT("Error loading %s!"), *ReportingFileName), EMessageSeverity::Error);
		}
	}
	GOutputCookingWarnings = false;
	return bSuccess;
}

void UCookOnTheFlyServer::ProcessUnsolicitedPackages(TArray<FName>* OutDiscoveredPackageNames,
	TMap<FName, UE::Cook::FInstigator>* OutInstigators)
{
	using namespace UE::Cook;

	// Ensure sublevels are loaded by iterating all recently loaded packages and invoking
	// PostLoadPackageFixup

	{
		UE_SCOPED_HIERARCHICAL_COOKTIMER(PostLoadPackageFixup);

		TMap<UPackage*, UE::Cook::FInstigator> NewPackages = PackageTracker->GetNewPackages();

		for (auto& PackageWithInstigator : NewPackages)
		{
			UPackage* Package = PackageWithInstigator.Key;
			FInstigator& Instigator = PackageWithInstigator.Value;
			if (!IsCookByTheBookMode() || !CookByTheBookOptions->bSkipSoftReferences)
			{
				PostLoadPackageFixup(Package, OutDiscoveredPackageNames, OutInstigators);
			}

			bool bWasInProgress;
			FPackageData* PackageData = QueueDiscoveredPackage(Package, FInstigator(Instigator), &bWasInProgress);
			if (OutDiscoveredPackageNames && PackageData && !bWasInProgress)
			{
				FInstigator& Existing = OutInstigators->FindOrAdd(PackageData->GetPackageName());
				if (Existing.Category == EInstigator::InvalidCategory)
				{
					OutDiscoveredPackageNames->Add(PackageData->GetPackageName());
					Existing = Instigator;
				}
			}
		}
	}
}

namespace UE::Cook
{

/** Local parameters and helper functions used by SaveCookedPackage */
class FSaveCookedPackageContext
{
private: // Used only by UCookOnTheFlyServer, which has private access

	FSaveCookedPackageContext(UCookOnTheFlyServer& InCOTFS, UE::Cook::FPackageData& InPackageData,
		TArrayView<const ITargetPlatform*> InPlatformsForPackage, UE::Cook::FTickStackData& StackData);

	void SetupPackage();
	void SetupPlatform(const ITargetPlatform* InTargetPlatform, bool bFirstPlatform);
	void FinishPlatform();
	void FinishPackage();

	// General Package Data
	UCookOnTheFlyServer& COTFS;
	UE::Cook::FPackageData& PackageData;
	TArrayView<const ITargetPlatform*> PlatformsForPackage;
	FTickStackData& StackData;
	UPackage* Package;
	const FString PackageName;
	FString Filename;
	uint32 SaveFlags = 0;
	bool bReferencedOnlyByEditorOnlyData = false;
	bool bHasFirstPlatformResults = false;

	// General Package Data that is delay-loaded the first time we save a platform
	UWorld* World = nullptr;
	EObjectFlags FlagsToCook = RF_Public;
	bool bHasDelayLoaded = false;
	bool bContainsMap = false;

	// Per-platform data, only valid in PerPlatform callbacks
	const ITargetPlatform* TargetPlatform = nullptr;
	FCookSavePackageContext* CookContext = nullptr;
	FSavePackageContext* SavePackageContext = nullptr;
	ICookedPackageWriter* PackageWriter = nullptr;
	FString PlatFilename;
	FSavePackageResultStruct SavePackageResult;
	bool bPlatformSetupSuccessful = false;
	bool bEndianSwap = false;

	friend class ::UCookOnTheFlyServer;
};
}

void UCookOnTheFlyServer::PumpSaves(UE::Cook::FTickStackData& StackData, uint32 DesiredQueueLength, int32& OutNumPushed, bool& bOutBusy)
{
	using namespace UE::Cook;
	OutNumPushed = 0;
	bOutBusy = false;

	UE_SCOPED_HIERARCHICAL_COOKTIMER(SavingPackages);
	check(IsInGameThread());
	ON_SCOPE_EXIT
	{
		if (HasExceededMaxMemory())
		{
			StackData.ResultFlags |= COSR_RequiresGC;
		}
	};

	// save as many packages as we can during our time slice
	FPackageDataQueue& SaveQueue = PackageDatas->GetSaveQueue();
	const uint32 OriginalPackagesToSaveCount = SaveQueue.Num();
	uint32 HandledCount = 0;
	TArray<const ITargetPlatform*, TInlineAllocator<ExpectedMaxNumPlatforms>> PlatformsForPackage;
	COOK_STAT(DetailedCookStats::PeakSaveQueueSize = FMath::Max(DetailedCookStats::PeakSaveQueueSize, SaveQueue.Num()));
	while (SaveQueue.Num() > static_cast<int32>(DesiredQueueLength))
	{
		FPackageData& PackageData(*SaveQueue.PopFrontValue());
		if (bPreexploreDependenciesEnabled  && TryCreateRequestCluster(PackageData))
		{
			continue;
		}

		FPoppedPackageDataScope PoppedScope(PackageData);
		UPackage* Package = PackageData.GetPackage();
		
		check(Package != nullptr);
		++HandledCount;

#if DEBUG_COOKONTHEFLY
		UE_LOG(LogCook, Display, TEXT("Processing save for package %s"), *Package->GetName());
#endif

		if (Package->IsLoadedByEditorPropertiesOnly() && PackageTracker->UncookedEditorOnlyPackages.Contains(Package->GetFName()))
		{
			// We already attempted to cook this package and it's still not referenced by any non editor-only properties.
			PackageData.SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
			++OutNumPushed;
			continue;
		}

		// This package is valid, so make sure it wasn't previously marked as being an uncooked editor only package or it would get removed from the
		// asset registry at the end of the cook
		PackageTracker->UncookedEditorOnlyPackages.Remove(Package->GetFName());

		if (PackageTracker->NeverCookPackageList.Contains(PackageData.GetFileName()))
		{
			// refuse to save this package, it's clearly one of the undesirables
			PackageData.SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
			++OutNumPushed;
			continue;
		}

		// Cook only the session platforms that have not yet been cooked for the given package
		PackageData.GetUncookedPlatforms(PlatformsForPackage);
		if (PlatformsForPackage.Num() == 0)
		{
			// We've already saved all possible platforms for this package; this should not be possible.
			// All places that add a package to the save queue check for existence of incomplete platforms before adding
			UE_LOG(LogCook, Warning, TEXT("Package '%s' in SaveQueue has no more platforms left to cook; this should not be possible!"), *PackageData.GetFileName().ToString());
			PackageData.SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
			++OutNumPushed;
			continue;
		}

		bool bShouldFinishTick = false;
		if (IsCookOnTheFlyMode())
		{
			if (!PackageData.GetIsUrgent())
			{
				if (ExternalRequests->HasRequests() || PackageDatas->GetMonitor().GetNumUrgent() > 0)
				{
					bShouldFinishTick = true;
				}
				if (StackData.Timer.IsTimeUp())
				{
					// our timeslice is up
					bShouldFinishTick = true;
				}
			}
			else
			{
				if (IsRealtimeMode())
				{
					if (StackData.Timer.IsTimeUp())
					{
						// our timeslice is up
						bShouldFinishTick = true;
					}
				}
				else
				{
					// if we are cook on the fly and not in the editor then save the requested package as fast as we can because the client is waiting on it
					// Until we are blocked on async work, ignore the timer
				}
			}
		}
		else // !IsCookOnTheFlyMode
		{
			check(IsCookByTheBookMode());
			if (StackData.Timer.IsTimeUp())
			{
				// our timeslice is up
				bShouldFinishTick = true;
			}
		}
		if (bShouldFinishTick)
		{
			SaveQueue.AddFront(&PackageData);
			return;
		}

		// Release any completed pending CookedPlatformDatas, so that slots in the per-class limits on calls to BeginCacheForCookedPlatformData are freed up for new objects to use
		PackageDatas->PollPendingCookedPlatformDatas();

		// Always wait for FinishPrepareSave before attempting to save the package
		bool AllObjectsCookedDataCached = FinishPrepareSave(PackageData, StackData.Timer);

		// If the CookPlatformData is not ready then postpone the package, exit, or wait for it as appropriate
		if (!AllObjectsCookedDataCached)
		{
			if (PackageData.GetHasBeginPrepareSaveFailed())
			{
				ReleaseCookedPlatformData(PackageData, true /* bCompletedSave */);
				PackageData.SetPlatformsCooked(PlatformsForPackage, false /* bSucceeded */);
				PackageData.SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
				++OutNumPushed;
				continue;
			}

			// GC is required
			if (PackageData.GeneratorPackageRequiresGC())
			{
				StackData.ResultFlags |= COSR_RequiresGC;
				SaveQueue.AddFront(&PackageData);
				return;
			}

			// Can we postpone?
			if (!PackageData.GetIsUrgent())
			{
				bool HasCheckedAllPackagesAreCached = HandledCount >= OriginalPackagesToSaveCount;
				if (!HasCheckedAllPackagesAreCached)
				{
					SaveQueue.Add(&PackageData);
					continue;
				}
			}
			// Should we wait?
			if (PackageData.GetIsUrgent() && !IsRealtimeMode())
			{
				UE_SCOPED_HIERARCHICAL_COOKTIMER(WaitingForCachedCookedPlatformData);
				do
				{
					// FinishPrepareSave might block on pending CookedPlatformDatas, and it might block on BeginPrepareSave, which can
					// block on resources held by other CookedPlatformDatas. Calling PollPendingCookedPlatformDatas should handle pumping all of those.
					check(PackageDatas->GetPendingCookedPlatformDatas().Num() || !PackageData.GetCookedPlatformDataCalled()); // FinishPrepareSave can only return false in one of these cases
					// sleep for a bit
					FPlatformProcess::Sleep(0.0f);
					// Poll the results again and check whether we are now done
					PackageDatas->PollPendingCookedPlatformDatas();
					AllObjectsCookedDataCached = FinishPrepareSave(PackageData, StackData.Timer);
				} while (!StackData.Timer.IsTimeUp() && !AllObjectsCookedDataCached);
			}
			// If we couldn't postpone or wait, then we need to exit and try again later
			if (!AllObjectsCookedDataCached)
			{
				StackData.ResultFlags |= COSR_WaitingOnCache;
				bOutBusy = true;
				SaveQueue.AddFront(&PackageData);
				return;
			}
		}
		check(AllObjectsCookedDataCached == true); // We are not allowed to save until FinishPrepareSave returns true.  We should have early exited above if it didn't

		// precache the next few packages
		if (!IsCookOnTheFlyMode() && SaveQueue.Num() != 0)
		{
			UE_SCOPED_HIERARCHICAL_COOKTIMER(PrecachePlatformDataForNextPackage);
			const int32 NumberToPrecache = 2;
			int32 LeftToPrecache = NumberToPrecache;
			for (FPackageData* NextData : SaveQueue)
			{
				if (LeftToPrecache == 0)
				{
					break;
				}
				--LeftToPrecache;
				BeginPrepareSave(*NextData, StackData.Timer, /*bIsPreCaching*/ true);
			}

			// If we're in RealTimeMode, check whether the precaching overflowed our timer and if so exit before we do the potentially expensive SavePackage
			// For non-realtime, overflowing the timer is not a critical issue.
			if (IsRealtimeMode() && StackData.Timer.IsTimeUp())
			{
				SaveQueue.AddFront(&PackageData);
				return;
			}
		}

		FSaveCookedPackageContext Context(*this, PackageData, PlatformsForPackage, StackData);
		SaveCookedPackage(Context);

		ReleaseCookedPlatformData(PackageData, true /* bCompletedSave */);
		PackageData.SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
		++OutNumPushed;
	}
}

void UCookOnTheFlyServer::PostLoadPackageFixup(UPackage* Package, TArray<FName>* OutDiscoveredPackageNames,
	TMap<FName, UE::Cook::FInstigator>* OutInstigators)
{
	if (Package->ContainsMap() == false)
	{
		return;
	}
	UWorld* World = UWorld::FindWorldInPackage(Package);
	if (!World)
	{
		return;
	}

	// Ensure we only process the package once
	if (PackageTracker->PostLoadFixupPackages.Find(Package) != nullptr)
	{
		return;
	}
	PackageTracker->PostLoadFixupPackages.Add(Package);

	// Perform special processing for UWorld
	World->PersistentLevel->HandleLegacyMapBuildData();

	if (IsCookByTheBookMode() == false)
	{
		return;
	}

	GIsCookerLoadingPackage = true;
	if (World->GetStreamingLevels().Num())
	{
		UE_SCOPED_COOKTIMER(PostLoadPackageFixup_LoadSecondaryLevels);
		TSet<FName> NeverCookPackageNames;
		PackageTracker->NeverCookPackageList.GetValues(NeverCookPackageNames);

		UE_LOG(LogCook, Display, TEXT("Loading secondary levels for package '%s'"), *World->GetName());

		World->LoadSecondaryLevels(true, &NeverCookPackageNames);
	}
	GIsCookerLoadingPackage = false;

	TArray<FString> NewPackagesToCook;

	// Collect world composition tile packages to cook
	if (World->WorldComposition)
	{
		World->WorldComposition->CollectTilesToCook(NewPackagesToCook);
	}

	FName OwnerName = Package->GetFName();
	for (const FString& PackageName : NewPackagesToCook)
	{
		FName PackageFName(*PackageName);
		UE::Cook::FPackageData* NewPackageData = PackageDatas->TryAddPackageDataByPackageName(PackageFName);
		if (NewPackageData && !NewPackageData->IsInProgress())
		{
			bool bIsUrgent = false;
			NewPackageData->UpdateRequestData(PlatformManager->GetSessionPlatforms(), bIsUrgent, UE::Cook::FCompletionCallback(),
				UE::Cook::FInstigator(UE::Cook::EInstigator::Dependency, OwnerName));
			if (OutDiscoveredPackageNames)
			{
				UE::Cook::FInstigator& Existing = OutInstigators->FindOrAdd(PackageFName);
				if (Existing.Category == UE::Cook::EInstigator::InvalidCategory)
				{
					OutDiscoveredPackageNames->Add(PackageFName);
					Existing = UE::Cook::FInstigator(UE::Cook::EInstigator::Dependency, OwnerName);
				}
			}
		}
	}
}

void UCookOnTheFlyServer::TickPrecacheObjectsForPlatforms(const float TimeSlice, const TArray<const ITargetPlatform*>& TargetPlatforms) 
{
	SCOPE_CYCLE_COUNTER(STAT_TickPrecacheCooking);


	UE::Cook::FCookerTimer Timer(TimeSlice, true);

	if (LastUpdateTick > 50 ||
		((CachedMaterialsToCacheArray.Num() == 0) && (CachedTexturesToCacheArray.Num() == 0)))
	{
		LastUpdateTick = 0;
		TArray<UObject*> Materials;
		GetObjectsOfClass(UMaterial::StaticClass(), Materials, true);
		for (UObject* Material : Materials)
		{
			if ( Material->GetOutermost() == GetTransientPackage())
				continue;

			CachedMaterialsToCacheArray.Add(Material);
		}
		TArray<UObject*> Textures;
		GetObjectsOfClass(UTexture::StaticClass(), Textures, true);
		for (UObject* Texture : Textures)
		{
			if (Texture->GetOutermost() == GetTransientPackage())
				continue;

			CachedTexturesToCacheArray.Add(Texture);
		}
	}
	++LastUpdateTick;

	if (Timer.IsTimeUp())
	{
		return;
	}

	bool AllMaterialsCompiled = true;
	// queue up some shaders for compilation

	while (CachedMaterialsToCacheArray.Num() > 0)
	{
		UMaterial* Material = (UMaterial*)(CachedMaterialsToCacheArray[0].Get());
		CachedMaterialsToCacheArray.RemoveAtSwap(0, 1, false);

		if (Material == nullptr)
		{
			continue;
		}

		UE_TRACK_REFERENCING_PACKAGE_SCOPED(Material->GetPackage(), PackageAccessTrackingOps::NAME_CookerBuildObject);
		for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
		{
			if (!TargetPlatform)
			{
				continue;
			}
			if (!Material->IsCachedCookedPlatformDataLoaded(TargetPlatform))
			{
				Material->BeginCacheForCookedPlatformData(TargetPlatform);
				AllMaterialsCompiled = false;
			}
		}

		if (Timer.IsTimeUp())
		{
			return;
		}

		if (GShaderCompilingManager->GetNumRemainingJobs() > MaxPrecacheShaderJobs)
		{
			return;
		}
	}


	if (!AllMaterialsCompiled)
	{
		return;
	}

	while (CachedTexturesToCacheArray.Num() > 0)
	{
		UTexture* Texture = (UTexture*)(CachedTexturesToCacheArray[0].Get());
		CachedTexturesToCacheArray.RemoveAtSwap(0, 1, false);

		if (Texture == nullptr)
		{
			continue;
		}

		UE_TRACK_REFERENCING_PACKAGE_SCOPED(Texture->GetPackage(), PackageAccessTrackingOps::NAME_CookerBuildObject);
		for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
		{
			if (!TargetPlatform)
			{
				continue;
			}
			if (!Texture->IsCachedCookedPlatformDataLoaded(TargetPlatform))
			{
				Texture->BeginCacheForCookedPlatformData(TargetPlatform);
			}
		}
		if (Timer.IsTimeUp())
		{
			return;
		}
	}

}

bool UCookOnTheFlyServer::HasExceededMaxMemory() const
{
	if (IsCookByTheBookMode() && CookByTheBookOptions->bFullLoadAndSave)
	{
		// FullLoadAndSave does the entire cook in one tick, so there is no need to GC after
		return false;
	}

#if UE_GC_TRACK_OBJ_AVAILABLE
	if (GUObjectArray.GetObjectArrayEstimatedAvailable() < MinFreeUObjectIndicesBeforeGC)
	{
		UE_LOG(LogCook, Display, TEXT("Running out of available UObject indices (%d remaining)"), GUObjectArray.GetObjectArrayEstimatedAvailable());
		static bool bPerformedObjListWhenNearMaxObjects = false;
		if (GEngine && !bPerformedObjListWhenNearMaxObjects)
		{
			UE_LOG(LogCook, Display, TEXT("Performing 'obj list' to show counts of types of objects due to low availability of UObject indices."));
			GEngine->Exec(nullptr, TEXT("OBJ LIST -COUNTSORT -SKIPMEMORYSIZE"));
			bPerformedObjListWhenNearMaxObjects = true;
		}
		return true;
	}
#endif // UE_GC_TRACK_OBJ_AVAILABLE


	// Only report exceeded memory if all the active memory usage triggers have fired
	int ActiveTriggers = 0;
	int FiredTriggers = 0;

	TStringBuilder<256> TriggerMessages;
	const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	if (MemoryMinFreeVirtual > 0 || MemoryMinFreePhysical > 0)
	{
		++ActiveTriggers;
		bool bFired = false;
		if (MemoryMinFreeVirtual > 0 && MemStats.AvailableVirtual < MemoryMinFreeVirtual)
		{
			TriggerMessages.Appendf(TEXT("\n  CookSettings.MemoryMinFreeVirtual: Available virtual memory %dMiB is less than %dMiB."),
				static_cast<uint32>(MemStats.AvailableVirtual / 1024 / 1024), static_cast<uint32>(MemoryMinFreeVirtual / 1024 / 1024));
			bFired = true;
		}
		if (MemoryMinFreePhysical > 0 && MemStats.AvailablePhysical < MemoryMinFreePhysical)
		{
			TriggerMessages.Appendf(TEXT("\n  CookSettings.MemoryMinFreePhysical: Available physical memory %dMiB is less than %dMiB."),
				static_cast<uint32>(MemStats.AvailablePhysical / 1024 / 1024), static_cast<uint32>(MemoryMinFreePhysical / 1024 / 1024));
			bFired = true;
		}
		if (bFired)
		{
			++FiredTriggers;
		}
	}

	if (MemoryMaxUsedVirtual > 0 || MemoryMaxUsedPhysical > 0)
	{
		++ActiveTriggers;
		bool bFired = false;
		if (MemoryMaxUsedVirtual > 0 && MemStats.UsedVirtual >= MemoryMaxUsedVirtual)
		{
			TriggerMessages.Appendf(TEXT("\n  CookSettings.MemoryMaxUsedVirtual: Used virtual memory %dMiB is greater than %dMiB."),
				static_cast<uint32>(MemStats.UsedVirtual / 1024 / 1024), static_cast<uint32>(MemoryMaxUsedVirtual / 1024 / 1024));
			bFired = true;
		}
		if (MemoryMaxUsedPhysical > 0 && MemStats.UsedPhysical >= MemoryMaxUsedPhysical)
		{
			TriggerMessages.Appendf(TEXT("\n  CookSettings.MemoryMaxUsedPhysical: Used physical memory %dMiB is greater than %dMiB."),
				static_cast<uint32>(MemStats.UsedPhysical / 1024 / 1024), static_cast<uint32>(MemoryMaxUsedPhysical / 1024 / 1024));
			bFired = true;
		}
		if (bFired)
		{
			++FiredTriggers;
		}
	}

	if (ActiveTriggers > 0 && FiredTriggers == ActiveTriggers)
	{
		UE_LOG(LogCook, Display, TEXT("Exceeded max memory on all configured triggers:%s"), TriggerMessages.ToString());
		return true;
	}
	else
	{
		return false;
	}
}

TArray<UPackage*> UCookOnTheFlyServer::GetUnsolicitedPackages(const TArray<const ITargetPlatform*>& TargetPlatforms) const
{
	// No longer supported
	return TArray<UPackage*>();
}

void UCookOnTheFlyServer::OnObjectModified( UObject *ObjectMoving )
{
	if (IsGarbageCollecting())
	{
		return;
	}
	OnObjectUpdated( ObjectMoving );
}

void UCookOnTheFlyServer::OnObjectPropertyChanged(UObject* ObjectBeingModified, FPropertyChangedEvent& PropertyChangedEvent)
{
	if (IsGarbageCollecting())
	{
		return;
	}
	if ( PropertyChangedEvent.Property == nullptr && 
		PropertyChangedEvent.MemberProperty == nullptr )
	{
		// probably nothing changed... 
		return;
	}

	OnObjectUpdated( ObjectBeingModified );
}

void UCookOnTheFlyServer::OnObjectSaved( UObject* ObjectSaved, FObjectPreSaveContext SaveContext)
{
	if (SaveContext.IsProceduralSave() )
	{
		// This is a procedural save (e.g. our own saving of the cooked package) rather than a user save, ignore
		return;
	}

	UPackage* Package = ObjectSaved->GetOutermost();
	if (Package == nullptr || Package == GetTransientPackage())
	{
		return;
	}

	MarkPackageDirtyForCooker(Package);

	// Register the package filename as modified. We don't use the cache because the file may not exist on disk yet at this point
	const FString PackageFilename = FPackageName::LongPackageNameToFilename(Package->GetName(), Package->ContainsMap() ? FPackageName::GetMapPackageExtension() : FPackageName::GetAssetPackageExtension());
	ModifiedAssetFilenames.Add(FName(*PackageFilename));
}

void UCookOnTheFlyServer::OnObjectUpdated( UObject *Object )
{
	// get the outer of the object
	UPackage *Package = Object->GetOutermost();

	MarkPackageDirtyForCooker( Package );
}

void UCookOnTheFlyServer::MarkPackageDirtyForCooker(UPackage* Package, bool bAllowInSession)
{
	if (Package->RootPackageHasAnyFlags(PKG_PlayInEditor))
	{
		return;
	}

	if (Package->HasAnyPackageFlags(PKG_PlayInEditor | PKG_ContainsScript | PKG_InMemoryOnly) == true && !GetClass()->HasAnyClassFlags(CLASS_DefaultConfig | CLASS_Config))
	{
		return;
	}

	if (Package == GetTransientPackage())
	{
		return;
	}

	if (Package->GetOuter() != nullptr)
	{
		return;
	}

	FName PackageName = Package->GetFName();
	if (FPackageName::IsMemoryPackage(PackageName.ToString()))
	{
		return;
	}

	if (bIsSavingPackage)
	{
		return;
	}

	if (IsInSession() && !bAllowInSession && !(IsCookByTheBookMode() && CookByTheBookOptions->bFullLoadAndSave))
	{
		ExternalRequests->AddCallback([this, PackageName]() { MarkPackageDirtyForCookerFromSchedulerThread(PackageName); });
	}
	else
	{
		MarkPackageDirtyForCookerFromSchedulerThread(PackageName);
	}
}

FName GInstigatorMarkPackageDirty(TEXT("MarkPackageDirtyForCooker"));
void UCookOnTheFlyServer::MarkPackageDirtyForCookerFromSchedulerThread(const FName& PackageName)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(MarkPackageDirtyForCooker);

	// could have just cooked a file which we might need to write
	UPackage::WaitForAsyncFileWrites();

	// Update the package's FileName if it has changed
	UE::Cook::FPackageData* PackageData = PackageDatas->UpdateFileName(PackageName);

	// force the package to be recooked
	UE_LOG(LogCook, Verbose, TEXT("Modification detected to package %s"), *PackageName.ToString());
	if ( PackageData && IsCookingInEditor() )
	{
		check(IsInGameThread()); // We're editing scheduler data, which is only allowable from the scheduler thread
		bool bHadCookedPlatforms = PackageData->HasAnyCookedPlatform();
		PackageData->SetPlatformsNotCooked();
		if (PackageData->IsInProgress())
		{
			PackageData->SendToState(UE::Cook::EPackageState::Request, UE::Cook::ESendFlags::QueueAddAndRemove);
		}
		else if (IsCookByTheBookRunning() && bHadCookedPlatforms)
		{
			PackageData->UpdateRequestData(PlatformManager->GetSessionPlatforms(), false /* bIsUrgent */, UE::Cook::FCompletionCallback(),
				UE::Cook::FInstigator(UE::Cook::EInstigator::Unspecified, GInstigatorMarkPackageDirty));
		}

		if ( IsCookOnTheFlyMode() && FileModifiedDelegate.IsBound())
		{
			FString PackageFileNameString = PackageData->GetFileName().ToString();
			FileModifiedDelegate.Broadcast(PackageFileNameString);
			if (PackageFileNameString.EndsWith(".uasset") || PackageFileNameString.EndsWith(".umap"))
			{
				FileModifiedDelegate.Broadcast( FPaths::ChangeExtension(PackageFileNameString, TEXT(".uexp")) );
				FileModifiedDelegate.Broadcast( FPaths::ChangeExtension(PackageFileNameString, TEXT(".ubulk")) );
				FileModifiedDelegate.Broadcast( FPaths::ChangeExtension(PackageFileNameString, TEXT(".ufont")) );
			}
		}
	}
}

bool UCookOnTheFlyServer::IsInSession() const
{
	return IsCookByTheBookRunning() || (IsCookOnTheFlyMode() && PlatformManager->HasSelectedSessionPlatforms());
}

void UCookOnTheFlyServer::ShutdownCookOnTheFly()
{
	if (CookOnTheFlyRequestManager.IsValid())
	{
		UE_LOG(LogCook, Display, TEXT("Shutting down cook on the fly server"));
		CookOnTheFlyRequestManager->Shutdown();
		CookOnTheFlyRequestManager.Reset();
		ClearHierarchyTimers();
	}
}

uint32 UCookOnTheFlyServer::GetPackagesPerGC() const
{
	return PackagesPerGC;
}

uint32 UCookOnTheFlyServer::GetPackagesPerPartialGC() const
{
	return MaxNumPackagesBeforePartialGC;
}


double UCookOnTheFlyServer::GetIdleTimeToGC() const
{
	return IdleTimeToGC;
}

uint64 UCookOnTheFlyServer::GetMaxMemoryAllowance() const
{
	return MemoryMaxUsedPhysical;
}

const TArray<FName>& UCookOnTheFlyServer::GetFullPackageDependencies(const FName& PackageName ) const
{
	TArray<FName>* PackageDependencies = CachedFullPackageDependencies.Find(PackageName);
	if ( !PackageDependencies )
	{
		static const FName NAME_CircularReference(TEXT("CircularReference"));
		static int32 UniqueArrayCounter = 0;
		++UniqueArrayCounter;
		FName CircularReferenceArrayName = FName(NAME_CircularReference,UniqueArrayCounter);
		{
			// can't initialize the PackageDependencies array here because we call GetFullPackageDependencies below and that could recurse and resize CachedFullPackageDependencies
			TArray<FName>& TempPackageDependencies = CachedFullPackageDependencies.Add(PackageName); // IMPORTANT READ ABOVE COMMENT
			// initialize TempPackageDependencies to a dummy dependency so that we can detect circular references
			TempPackageDependencies.Add(CircularReferenceArrayName);
			// when someone finds the circular reference name they look for this array name in the CachedFullPackageDependencies map
			// and add their own package name to it, so that they can get fixed up 
			CachedFullPackageDependencies.Add(CircularReferenceArrayName);
		}

		TArray<FName> ChildDependencies;
		if ( AssetRegistry->GetDependencies(PackageName, ChildDependencies, UE::AssetRegistry::EDependencyCategory::Package) )
		{
			TArray<FName> Dependencies = ChildDependencies;
			Dependencies.AddUnique(PackageName);
			for ( const FName& ChildDependency : ChildDependencies)
			{
				const TArray<FName>& ChildPackageDependencies = GetFullPackageDependencies(ChildDependency);
				for ( const FName& ChildPackageDependency : ChildPackageDependencies )
				{
					if ( ChildPackageDependency == CircularReferenceArrayName )
					{
						continue;
					}

					if ( ChildPackageDependency.GetComparisonIndex() == NAME_CircularReference.GetComparisonIndex() )
					{
						// add our self to the package which we are circular referencing
						TArray<FName>& TempCircularReference = CachedFullPackageDependencies.FindChecked(ChildPackageDependency);
						TempCircularReference.AddUnique(PackageName); // add this package name so that it's dependencies get fixed up when the outer loop returns
					}

					Dependencies.AddUnique(ChildPackageDependency);
				}
			}

			// all these packages referenced us apparently so fix them all up
			const TArray<FName>& PackagesForFixup = CachedFullPackageDependencies.FindChecked(CircularReferenceArrayName);
			for ( const FName& FixupPackage : PackagesForFixup )
			{
				TArray<FName> &FixupList = CachedFullPackageDependencies.FindChecked(FixupPackage);
				// check( FixupList.Contains( CircularReferenceArrayName) );
				ensure( FixupList.Remove(CircularReferenceArrayName) == 1 );
				for( const FName& AdditionalDependency : Dependencies )
				{
					FixupList.AddUnique(AdditionalDependency);
					if ( AdditionalDependency.GetComparisonIndex() == NAME_CircularReference.GetComparisonIndex() )
					{
						// add our self to the package which we are circular referencing
						TArray<FName>& TempCircularReference = CachedFullPackageDependencies.FindChecked(AdditionalDependency);
						TempCircularReference.AddUnique(FixupPackage); // add this package name so that it's dependencies get fixed up when the outer loop returns
					}
				}
			}
			CachedFullPackageDependencies.Remove(CircularReferenceArrayName);

			PackageDependencies = CachedFullPackageDependencies.Find(PackageName);
			check(PackageDependencies);

			Swap(*PackageDependencies, Dependencies);
		}
		else
		{
			PackageDependencies = CachedFullPackageDependencies.Find(PackageName);
			PackageDependencies->Add(PackageName);
		}
	}

	return *PackageDependencies;
}

void UCookOnTheFlyServer::PreGarbageCollect()
{
	using namespace UE::Cook;
	if (!IsInSession())
	{
		return;
	}

#if COOK_CHECKSLOW_PACKAGEDATA
	// Verify that only packages in the save state have pointers to objects
	for (const FPackageData* PackageData : *PackageDatas.Get())
	{
		check(PackageData->GetState() == EPackageState::Save || !PackageData->HasReferencedObjects());
	}
#endif
	if (SavingPackageData)
	{
		check(SavingPackageData->GetPackage());
		GCKeepObjects.Add(SavingPackageData->GetPackage());
	}

	TArray<UPackage*> GCKeepPackages;
	for (FPackageData* PackageData : PackageDatas->GetSaveQueue())
	{
		// Generator packages that have started generation and have not yet finished saving should not be garbage
		// collected because we do not want to kick them out of save and then have to reexecute their generation 
		// on the next save, so keep them loaded.
		if (PackageData->GetGeneratorPackage())
		{
			GCKeepPackages.Add(PackageData->GetPackage());
		}
	}

	// Find the packages that are waiting on async jobs to finish cooking data
	// and make sure that they are not garbage collected until the jobs have
	// completed.
	{
		TSet<UPackage*> UniquePendingPackages;
		for (FPendingCookedPlatformData& PendingData : PackageDatas->GetPendingCookedPlatformDatas())
		{
			if (UObject* Object = PendingData.Object.Get())
			{	
				if (UPackage* Package = Object->GetPackage())
				{
					UniquePendingPackages.Add(Package);
				}	
			}
		}

		GCKeepPackages.Reserve(GCKeepPackages.Num() + UniquePendingPackages.Num());
		for (UPackage* Package : UniquePendingPackages)
		{
			GCKeepPackages.Add(Package);
		}	
	}

	const bool bPartialGC = IsCookFlagSet(ECookInitializationFlags::EnablePartialGC);
	if (bPartialGC)
	{
		GCKeepObjects.Empty(1000);

		// Keep all inprogress packages (including packages that have only made it to the request list) that have been partially loaded
		// Additionally, keep all partially loaded packages that are transitively dependended on by any inprogress packages
		// Keep all UObjects that have been loaded so far under these packages
		TMap<const FPackageData*, int32> DependenciesCount;

		TSet<FName> KeepPackages;
		for (const FPackageData* PackageData : *PackageDatas)
		{
			if (PackageData->GetState() == UE::Cook::EPackageState::Save)
			{
				// already handled above
				continue;
			}
			const TArray<FName>& NeededPackages = GetFullPackageDependencies(PackageData->GetPackageName());
			DependenciesCount.Add(PackageData, NeededPackages.Num());
			KeepPackages.Append(NeededPackages);
		}

		TSet<FName> LoadedPackages;
		for (UPackage* Package : PackageTracker->LoadedPackages)
		{
			const FName& PackageName = Package->GetFName();
			if (KeepPackages.Contains(PackageName))
			{
				LoadedPackages.Add(PackageName);
				GCKeepPackages.Add(Package);
			}
		}

		FRequestQueue& RequestQueue = PackageDatas->GetRequestQueue();
		TArray<FPackageData*> Requests;
		Requests.Reserve(RequestQueue.ReadyRequestsNum());
		while (!RequestQueue.IsReadyRequestsEmpty())
		{
			Requests.Add(RequestQueue.PopReadyRequest());
		}
		// We are not looking at UnclusteredRequests or RequestClusters from the RequestQueue. These are supposed to 
		// be quickly processed and should usually be empty, so failing to account for them will only rarely cause
		// a performance issue (and will not cause a behavior issue)
		
		// Sort the cook requests by the packages which are loaded first
		// then sort by the number of dependencies which are referenced by the package
		// we want to process the packages with the highest dependencies so that they can
		// be evicted from memory and are likely to be able to be released on next GC pass
		Algo::Sort(Requests, [&DependenciesCount, &LoadedPackages](const FPackageData* A, const FPackageData* B)
			{
				int32 ADependencies = DependenciesCount.FindChecked(A);
				int32 BDependencies = DependenciesCount.FindChecked(B);
				bool ALoaded = LoadedPackages.Contains(A->GetPackageName());
				bool BLoaded = LoadedPackages.Contains(B->GetPackageName());
				return (ALoaded == BLoaded) ? (ADependencies > BDependencies) : ALoaded > BLoaded;
			}
		);
		for (FPackageData* Request : Requests)
		{
			RequestQueue.AddRequest(Request); // Urgent requests will still be moved to the front of the RequestQueue by AddRequest
		}
	}

	// Add packages and all RF_Public objects outered to them to GCKeepObjects
	TArray<UObject*> ObjectsWithOuter;
	for (UPackage* Package : GCKeepPackages)
	{
		GCKeepObjects.Add(Package);
		ObjectsWithOuter.Reset();
		GetObjectsWithOuter(Package, ObjectsWithOuter);
		for (UObject* Obj : ObjectsWithOuter)
		{
			if (Obj->HasAnyFlags(RF_Public))
			{
				GCKeepObjects.Add(Obj);
			}
		}
	}
}

void UCookOnTheFlyServer::CookerAddReferencedObjects(FReferenceCollector& Collector)
{
	using namespace UE::Cook;

	// GCKeepObjects are the objects that we want to keep loaded but we only have a WeakPtr to
	Collector.AddReferencedObjects(GCKeepObjects);
}

void UCookOnTheFlyServer::PostGarbageCollect()
{
	using namespace UE::Cook;

	// If any PackageDatas with ObjectPointers had any of their object pointers deleted out from under them, demote them back to request
	TArray<FPackageData*> Demotes;
	for (FPackageData* PackageData : PackageDatas->GetSaveQueue())
	{
		if (PackageData->IsSaveInvalidated())
		{
			Demotes.Add(PackageData);
		}
	}
	for (FPackageData* PackageData : Demotes)
	{
		PackageData->SendToState(EPackageState::Request, ESendFlags::QueueRemove);
		PackageDatas->GetRequestQueue().AddRequest(PackageData, /* bForceUrgent */ true);
	}

	// If there was a GarbageCollect while we are saving a package, some of the WeakObjectPtr in SavingPackageData->CachedObjectPointers may have been deleted and set to null
	// We need to handle nulls in that array at any point after calling SavePackage. We do not want to declare them as references and prevent their GC, in case there is 
	// the expectation by some licensee code that removing references to an object will cause it to not be saved
	// However, if garbage collection deleted the package WHILE WE WERE SAVING IT, then we have problems.
	check(!SavingPackageData || SavingPackageData->GetPackage() != nullptr);

	GCKeepObjects.Empty();

	for (FPackageData* PackageData : *PackageDatas.Get())
	{
		if (FGeneratorPackage* GeneratorPackage = PackageData->GetGeneratorPackage())
		{
			GeneratorPackage->PostGarbageCollect();
		}
	}
}

void UCookOnTheFlyServer::BeginDestroy()
{
	ShutdownCookOnTheFly();

	Super::BeginDestroy();
}

void UCookOnTheFlyServer::TickRecompileShaderRequests()
{
	// try to pull off a request
	UE::Cook::FRecompileShaderRequest RecompileShaderRequest;
	if (PackageTracker->RecompileRequests.Dequeue(&RecompileShaderRequest))
	{
		RecompileShadersForRemote(RecompileShaderRequest.RecompileArguments, GetSandboxDirectory(RecompileShaderRequest.RecompileArguments.PlatformName));
		RecompileShaderRequest.CompletionCallback();
	}
}

bool UCookOnTheFlyServer::HasRecompileShaderRequests() const 
{ 
	return PackageTracker->RecompileRequests.HasItems();
}

class FDiffModeCookServerUtils
{
public:
	void InitializePackageWriter(ICookedPackageWriter*& CookedPackageWriter)
	{
		Initialize();
		if (!bDiffEnabled && !bLinkerDiffEnabled)
		{
			return;
		}

		ICookedPackageWriter::FCookCapabilities Capabilities = CookedPackageWriter->GetCookCapabilities();
		if (!Capabilities.bDiffModeSupported)
		{
			const TCHAR* CommandLineArg = bDiffEnabled ? TEXT("-DIFFONLY") : TEXT("-LINKERDIFF");
			UE_LOG(LogCook, Fatal, TEXT("%s was enabled, but -iostore is also enabled and iostore PackageWriters do not support %s."),
				CommandLineArg, CommandLineArg);
		}

		if (bDiffEnabled)
		{
			// Wrap the incoming writer inside a FDiffPackageWriter
			CookedPackageWriter = new FDiffPackageWriter(TUniquePtr<ICookedPackageWriter>(CookedPackageWriter));
		}
		else
		{
			check(bLinkerDiffEnabled);
			CookedPackageWriter = new FLinkerDiffPackageWriter(TUniquePtr<ICookedPackageWriter>(CookedPackageWriter));
		}
	}

private:
	void Initialize()
	{
		if (bInitialized)
		{
			return;
		}

		bDiffEnabled = FParse::Param(FCommandLine::Get(), TEXT("DIFFONLY"));
		FString Value;
		bLinkerDiffEnabled = FParse::Value(FCommandLine::Get(), TEXT("-LINKERDIFF="), Value);
		if (bDiffEnabled && bLinkerDiffEnabled)
		{
			UE_LOG(LogCook, Fatal, TEXT("-DiffOnly and -LinkerDiff are mutually exclusive."));
		}

		bInitialized = true;
	}

	bool bInitialized = false;
	bool bDiffEnabled = false;
	bool bLinkerDiffEnabled = false;
};

#if OUTPUT_COOKTIMING

UE_TRACE_EVENT_BEGIN(UE_CUSTOM_COOKTIMER_LOG, SaveCookedPackage, NoSync)
	UE_TRACE_EVENT_FIELD(UE::Trace::WideString, PackageName)
UE_TRACE_EVENT_END()

#endif //OUTPUT_COOKTIMING

void UCookOnTheFlyServer::SaveCookedPackage(UE::Cook::FSaveCookedPackageContext& Context)
{
	using namespace UE::Cook;

	UE_SCOPED_HIERARCHICAL_CUSTOM_COOKTIMER_AND_DURATION(SaveCookedPackage, DetailedCookStats::TickCookOnTheSideSaveCookedPackageTimeSec)
		UE_ADD_CUSTOM_COOKTIMER_META(SaveCookedPackage, PackageName, *WriteToString<256>(Context.PackageData.GetFileName()));

	UPackage* Package = Context.Package;
	uint32 OriginalPackageFlags = Package->GetPackageFlags();

	Context.SetupPackage();

	ITargetPlatformManagerModule& TPM = GetTargetPlatformManagerRef();
	TGuardValue<bool> ScopedOutputCookerWarnings(GOutputCookingWarnings, IsCookFlagSet(ECookInitializationFlags::OutputVerboseCookerWarnings));
	// SavePackage can CollectGarbage, so we need to store the currently-unqueued PackageData in a separate variable that we register for garbage collection
	TGuardValue<UE::Cook::FPackageData*> ScopedSavingPackageData(SavingPackageData, &Context.PackageData);
	TGuardValue<bool> ScopedIsSavingPackage(bIsSavingPackage, true);
	// For legacy reasons we set GIsCookerLoadingPackage == true during save. Some classes use it to conditionally execute cook operations in both save and load
	TGuardValue<bool> ScopedIsCookerLoadingPackage(GIsCookerLoadingPackage, true);
	ON_SCOPE_EXIT{ Package->SetPackageFlagsTo(OriginalPackageFlags); };

	bool bFirstPlatform = true;
	for (const ITargetPlatform* TargetPlatform : Context.PlatformsForPackage)
	{
		Context.SetupPlatform(TargetPlatform, bFirstPlatform);
		if (Context.bPlatformSetupSuccessful)
		{
			UE_SCOPED_HIERARCHICAL_COOKTIMER(GEditorSavePackage);
			UE_TRACK_REFERENCING_PLATFORM_SCOPED(TargetPlatform);

			FSavePackageArgs SaveArgs;
			SaveArgs.TopLevelFlags = Context.FlagsToCook;
			SaveArgs.bForceByteSwapping = Context.bEndianSwap;
			SaveArgs.bWarnOfLongFilename = false;
			SaveArgs.SaveFlags = Context.SaveFlags;
			SaveArgs.TargetPlatform = TargetPlatform;
			SaveArgs.bSlowTask = false;
			SaveArgs.SavePackageContext = Context.SavePackageContext;

			Context.PackageWriter->UpdateSaveArguments(SaveArgs);
			do
			{
				try
				{
					Context.SavePackageResult = GEditor->Save(Package, Context.World, *Context.PlatFilename, SaveArgs);
				}
				catch (std::exception&)
				{
					UE_LOG(LogCook, Warning, TEXT("Tried to save package %s for target platform %s but threw an exception"),
						*Package->GetName(), *TargetPlatform->PlatformName());
					Context.SavePackageResult = ESavePackageResult::Error;
				}
			} while (Context.PackageWriter->IsAnotherSaveNeeded(Context.SavePackageResult, SaveArgs));

			// If package was actually saved check with asset manager to make sure it wasn't excluded for being a
			// development or never cook package. But skip sending the warnings from this check if it was editor-only.
			if (Context.SavePackageResult == ESavePackageResult::Success && UAssetManager::IsValid())
			{
				UE_SCOPED_HIERARCHICAL_COOKTIMER(VerifyCanCookPackage);
				if (!UAssetManager::Get().VerifyCanCookPackage(this, Package->GetFName()))
				{
					Context.SavePackageResult = ESavePackageResult::Error;
				}
			}

			++this->StatSavedPackageCount;
		}

		Context.FinishPlatform();
		bFirstPlatform = false;
	}

	Context.FinishPackage();
	Context.StackData.Timer.SavedPackage();
}

namespace UE::Cook
{
FSaveCookedPackageContext::FSaveCookedPackageContext(UCookOnTheFlyServer& InCOTFS, UE::Cook::FPackageData& InPackageData,
	TArrayView<const ITargetPlatform*> InPlatformsForPackage, UE::Cook::FTickStackData& InStackData)
	: COTFS(InCOTFS)
	, PackageData(InPackageData)
	, PlatformsForPackage(InPlatformsForPackage)
	, StackData(InStackData)
	, Package(PackageData.GetPackage())
	, PackageName(Package ? Package->GetName() : FString())
	, Filename(PackageData.GetFileName().ToString())
{
}

void FSaveCookedPackageContext::SetupPackage()
{
	check(Package && Package->IsFullyLoaded()); // PackageData should not be in the save state if Package is not fully loaded
	check(Package->GetPathName().Equals(PackageName)); // We should only be saving outermost packages, so the path name should be the same as the package name
	check(!Filename.IsEmpty()); // PackageData guarantees FileName is non-empty; if not found it should never make it into save state
	if (Package->HasAnyPackageFlags(PKG_ReloadingForCooker))
	{
		UE_LOG(LogCook, Warning, TEXT("Package %s marked as reloading for cook by was requested to save"), *PackageName);
		UE_LOG(LogCook, Fatal, TEXT("Package %s marked as reloading for cook by was requested to save"), *PackageName);
	}

	SaveFlags = SAVE_KeepGUID | SAVE_Async
		| (COTFS.IsCookFlagSet(ECookInitializationFlags::Unversioned) ? SAVE_Unversioned : 0);

	// removing editor only packages only works when cooking in commandlet and non iterative cooking
	// also doesn't work in multiprocess cooking
	bool bKeepEditorOnlyPackages = !(COTFS.IsCookByTheBookMode() && !COTFS.IsCookingInEditor());
	bKeepEditorOnlyPackages |= COTFS.IsCookFlagSet(ECookInitializationFlags::Iterative);
	SaveFlags |= bKeepEditorOnlyPackages ? SAVE_KeepEditorOnlyCookedPackages : SAVE_None;
	SaveFlags |= COTFS.CookByTheBookOptions ? SAVE_ComputeHash : SAVE_None;

	// Use SandboxFile to do path conversion to properly handle sandbox paths (outside of standard paths in particular).
	Filename = COTFS.ConvertToFullSandboxPath(*Filename, true);
}

void FSaveCookedPackageContext::SetupPlatform(const ITargetPlatform* InTargetPlatform, bool bFirstPlatform)
{
	TargetPlatform = InTargetPlatform;
	PlatFilename = Filename.Replace(TEXT("[Platform]"), *TargetPlatform->PlatformName());
	bPlatformSetupSuccessful = false;

	// don't save Editor resources from the Engine if the target doesn't have editoronly data
	if (COTFS.IsCookFlagSet(ECookInitializationFlags::SkipEditorContent) &&
		(PackageName.StartsWith(TEXT("/Engine/Editor")) || PackageName.StartsWith(TEXT("/Engine/VREditor"))) &&
		!TargetPlatform->HasEditorOnlyData())
	{
		SavePackageResult = ESavePackageResult::ContainsEditorOnlyData;
		return;
	}
	// Check whether or not game-specific behaviour should prevent this package from being cooked for the target platform
	else if (UAssetManager::IsValid() && !UAssetManager::Get().ShouldCookForPlatform(Package, TargetPlatform))
	{
		SavePackageResult = ESavePackageResult::ContainsEditorOnlyData;
		UE_LOG(LogCook, Display, TEXT("Excluding %s -> %s"), *PackageName, *PlatFilename);
		return;
	}
	// check if this package is unsupported for the target platform (typically plugin content)
	else
	{
		TSet<FName>* NeverCookPackages = COTFS.PackageTracker->PlatformSpecificNeverCookPackages.Find(TargetPlatform);
		if (NeverCookPackages && NeverCookPackages->Find(Package->GetFName()))
		{
			SavePackageResult = ESavePackageResult::ContainsEditorOnlyData;
			UE_LOG(LogCook, Display, TEXT("Excluding %s -> %s"), *PackageName, *PlatFilename);
			return;
		}
	}

	const FString FullFilename = FPaths::ConvertRelativePathToFull(PlatFilename);
	if (FullFilename.Len() >= FPlatformMisc::GetMaxPathLength())
	{
		LogCookerMessage(FString::Printf(TEXT("Couldn't save package, filename is too long (%d >= %d): %s"),
			FullFilename.Len(), FPlatformMisc::GetMaxPathLength(), *FullFilename), EMessageSeverity::Error);
		SavePackageResult = ESavePackageResult::Error;
		return;
	}

	if (!bHasDelayLoaded)
	{
		// look for a world object in the package (if there is one, there's a map)
		World = UWorld::FindWorldInPackage(Package);
		if (World)
		{
			FlagsToCook = RF_NoFlags;
		}
		bContainsMap = Package->ContainsMap();
		bHasDelayLoaded = true;
	}

	UE_CLOG((GCookProgressDisplay & (int32)ECookProgressDisplayMode::Instigators) && bFirstPlatform, LogCook, Display,
		TEXT("Cooking %s, Instigator: { %s }"), *PackageName, *(PackageData.GetInstigator().ToString()));
	UE_CLOG(GCookProgressDisplay & (int32)ECookProgressDisplayMode::PackageNames, LogCook, Display,
		TEXT("Cooking %s -> %s"), *PackageName, *PlatFilename);

	bEndianSwap = (!TargetPlatform->IsLittleEndian()) ^ (!PLATFORM_LITTLE_ENDIAN);

	if (!TargetPlatform->HasEditorOnlyData())
	{
		Package->SetPackageFlags(PKG_FilterEditorOnly);
	}
	else
	{
		Package->ClearPackageFlags(PKG_FilterEditorOnly);
	}

	if (World)
	{
		// Fixup legacy lightmaps before saving
		// This should be done after loading, but Core loads UWorlds with LoadObject so there's no opportunity to handle this fixup on load
		World->PersistentLevel->HandleLegacyMapBuildData();
	}

	CookContext = &COTFS.FindOrCreateSaveContext(TargetPlatform);
	SavePackageContext = &CookContext->SaveContext;
	PackageWriter = CookContext->PackageWriter;
	ICookedPackageWriter::FBeginPackageInfo Info;
	Info.PackageName = Package->GetFName();
	Info.LooseFilePath = PlatFilename;
	PackageWriter->BeginPackage(Info);

	// Indicate Setup was successful
	bPlatformSetupSuccessful = true;
	SavePackageResult = ESavePackageResult::Success;
}

void FSaveCookedPackageContext::FinishPlatform()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FSaveCookedPackageContext::FinishPlatform);

	bool bSuccessful = SavePackageResult.IsSuccessful();
	bool bLocalReferencedOnlyByEditorOnlyData = SavePackageResult == ESavePackageResult::ReferencedOnlyByEditorOnlyData;

	TFuture<FMD5Hash> CookedHash;
	FAssetRegistryGenerator& Generator = *(COTFS.PlatformManager->GetPlatformData(TargetPlatform)->RegistryGenerator);
	if (bPlatformSetupSuccessful)
	{
		FAssetPackageData* AssetPackageData = Generator.GetAssetPackageData(Package->GetFName());
		check(AssetPackageData);

		// TODO_BuildDefinitionList: Calculate and store BuildDefinitionList on the PackageData, or collect it here from some other source.
		TArray<UE::DerivedData::FBuildDefinition> BuildDefinitions;
		FCbObject BuildDefinitionList = UE::TargetDomain::BuildDefinitionListToObject(BuildDefinitions);
		FCbObject TargetDomainDependencies;
		{
			UE_SCOPED_HIERARCHICAL_COOKTIMER(TargetDomainDependencies);
			TargetDomainDependencies = UE::TargetDomain::CollectDependenciesObject(Package, TargetPlatform, nullptr /* ErrorMessage */);
		}

		ICookedPackageWriter::FCommitPackageInfo Info;
		Info.bSucceeded = bSuccessful;
		Info.PackageName = Package->GetFName();
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		Info.PackageGuid = AssetPackageData->PackageGuid;
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		Info.Attachments.Add({ "Dependencies", TargetDomainDependencies });
		// TODO: Reenable BuildDefinitionList once FCbPackage support for empty FCbObjects is in
		//Info.Attachments.Add({ "BuildDefinitionList", BuildDefinitionList });
		Info.WriteOptions = IPackageWriter::EWriteOptions::Write;
		if (!!(SaveFlags & SAVE_ComputeHash))
		{
			Info.WriteOptions |= IPackageWriter::EWriteOptions::ComputeHash;
		}

		CookedHash = PackageWriter->CommitPackage(MoveTemp(Info));
	}

	// Update asset registry
	if (COTFS.CookByTheBookOptions)
	{
		Generator.UpdateAssetRegistryPackageData(*Package, SavePackageResult, CookedHash);
	}

	// In success or failure we want to mark the package as cooked, unless the failure was due to being referenced only
	// by editor-only data; we may need to save it later when new content loads it through non editor-only references.
	if (!bLocalReferencedOnlyByEditorOnlyData)
	{
		PackageData.SetPlatformCooked(TargetPlatform, bSuccessful);
	}

	// Update flags used to determine garbage collection.
	if (bSuccessful)
	{
		if (bContainsMap)
		{
			StackData.ResultFlags |= UCookOnTheFlyServer::COSR_CookedMap;
		}
		else
		{
			++StackData.CookedPackageCount;
			StackData.ResultFlags |= UCookOnTheFlyServer::COSR_CookedPackage;
		}
	}

	// Accumulate results for SaveCookedPackage_Finish
	if (bHasFirstPlatformResults && bLocalReferencedOnlyByEditorOnlyData != bReferencedOnlyByEditorOnlyData)
	{
		UE_LOG(LogCook, Error, TEXT("Package %s had different values for IsReferencedOnlyByEditorOnlyData from multiple platforms. ")
			TEXT("Treading all platforms as IsReferencedOnlyByEditorOnlyData = true; this will cause the package to be ignored on the platforms that need it."),
			*Package->GetName());
		bReferencedOnlyByEditorOnlyData = true;
	}
	else
	{
		bReferencedOnlyByEditorOnlyData = bLocalReferencedOnlyByEditorOnlyData;
	}
	bHasFirstPlatformResults = true;
}

void FSaveCookedPackageContext::FinishPackage()
{
	// Add soft references discovered from the package
	FName PackageFName = Package->GetFName();
	if (!COTFS.IsCookByTheBookMode() || !COTFS.CookByTheBookOptions->bSkipSoftReferences)
	{
		// Also request any localized variants of this package
		if (COTFS.IsCookByTheBookMode() && !FPackageName::IsLocalizedPackage(Package->GetName()))
		{
			const TArray<FName>* LocalizedVariants = COTFS.CookByTheBookOptions->SourceToLocalizedPackageVariants.Find(PackageFName);
			if (LocalizedVariants)
			{
				for (const FName& LocalizedPackageName : *LocalizedVariants)
				{
					UE::Cook::FPackageData* LocalizedPackageData = COTFS.PackageDatas->TryAddPackageDataByPackageName(LocalizedPackageName);
					if (LocalizedPackageData)
					{
						bool bIsUrgent = false;
						LocalizedPackageData->UpdateRequestData(PlatformsForPackage, bIsUrgent, UE::Cook::FCompletionCallback(),
							UE::Cook::FInstigator(UE::Cook::EInstigator::SoftDependency, PackageFName));
					}
				}
			}
		}

		// Add SoftObjectPaths to the cook. This has to be done after the package save to catch any SoftObjectPaths that were added during save.
		TSet<FName> SoftObjectPackages;
		GRedirectCollector.ProcessSoftObjectPathPackageList(PackageFName, false, SoftObjectPackages);
		for (FName SoftObjectPackage : SoftObjectPackages)
		{
			TMap<FName, FName> RedirectedPaths;

			// If this is a redirector, extract destination from asset registry
			if (COTFS.ContainsRedirector(SoftObjectPackage, RedirectedPaths))
			{
				for (TPair<FName, FName>& RedirectedPath : RedirectedPaths)
				{
					GRedirectCollector.AddAssetPathRedirection(RedirectedPath.Key, RedirectedPath.Value);
				}
			}

			if (COTFS.IsCookByTheBookMode())
			{
				UE::Cook::FPackageData* SoftObjectPackageData = COTFS.PackageDatas->TryAddPackageDataByPackageName(SoftObjectPackage);
				if (SoftObjectPackageData)
				{
					bool bIsUrgent = false;
					SoftObjectPackageData->UpdateRequestData(PlatformsForPackage, bIsUrgent, UE::Cook::FCompletionCallback(),
						UE::Cook::FInstigator(UE::Cook::EInstigator::SoftDependency, PackageFName));
				}
			}
		}
	}

	if (!bReferencedOnlyByEditorOnlyData)
	{
		if ((COTFS.CurrentCookMode == ECookMode::CookOnTheFly) && !PackageData.GetIsUrgent())
		{
			// this is an unsolicited package
			if (FPaths::FileExists(Filename))
			{
				COTFS.PackageTracker->UnsolicitedCookedPackages.AddCookedPackage(
					FFilePlatformRequest(PackageData.GetFileName(), EInstigator::Unspecified, PlatformsForPackage));

#if DEBUG_COOKONTHEFLY
				UE_LOG(LogCook, Display, TEXT("UnsolicitedCookedPackages: %s"), *Filename);
#endif
			}
		}
	}
	else
	{
		COTFS.PackageTracker->UncookedEditorOnlyPackages.AddUnique(Package->GetFName());
	}
}

} // namespace UE::Cook

void UCookOnTheFlyServer::Initialize( ECookMode::Type DesiredCookMode, ECookInitializationFlags InCookFlags, const FString &InOutputDirectoryOverride )
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UCookOnTheFlyServer::Initialize);

	PackageDatas = MakeUnique<UE::Cook::FPackageDatas>(*this);
	PlatformManager = MakeUnique<UE::Cook::FPlatformManager>();
	ExternalRequests = MakeUnique<UE::Cook::FExternalRequests>();
	PackageTracker = MakeUnique<UE::Cook::FPackageTracker>(*PackageDatas.Get());
	DiffModeHelper = MakeUnique<FDiffModeCookServerUtils>();

	BuildDefinitions.Reset(new UE::Cook::FBuildDefinitions());
	UE::Cook::InitializeTls();
	UE::Cook::FPlatformManager::InitializeTls();

	OutputDirectoryOverride = InOutputDirectoryOverride;
	CurrentCookMode = DesiredCookMode;
	CookFlags = InCookFlags;

	FCoreUObjectDelegates::GetPreGarbageCollectDelegate().AddUObject(this, &UCookOnTheFlyServer::PreGarbageCollect);
	FCoreUObjectDelegates::GetPostGarbageCollect().AddUObject(this, &UCookOnTheFlyServer::PostGarbageCollect);

	if (IsCookingInEditor())
	{
		FCoreUObjectDelegates::OnObjectPropertyChanged.AddUObject(this, &UCookOnTheFlyServer::OnObjectPropertyChanged);
		FCoreUObjectDelegates::OnObjectModified.AddUObject(this, &UCookOnTheFlyServer::OnObjectModified);
		FCoreUObjectDelegates::OnObjectPreSave.AddUObject(this, &UCookOnTheFlyServer::OnObjectSaved);

		FCoreDelegates::OnTargetPlatformChangedSupportedFormats.AddUObject(this, &UCookOnTheFlyServer::OnTargetPlatformChangedSupportedFormats);
	}

	FCoreDelegates::OnFConfigCreated.AddUObject(this, &UCookOnTheFlyServer::OnFConfigCreated);
	FCoreDelegates::OnFConfigDeleted.AddUObject(this, &UCookOnTheFlyServer::OnFConfigDeleted);

	GetTargetPlatformManager()->GetOnTargetPlatformsInvalidatedDelegate().AddUObject(this, &UCookOnTheFlyServer::OnTargetPlatformsInvalidated);

	MaxPrecacheShaderJobs = FPlatformMisc::NumberOfCores() - 1; // number of cores -1 is a good default allows the editor to still be responsive to other shader requests and allows cooker to take advantage of multiple processors while the editor is running
	GConfig->GetInt(TEXT("CookSettings"), TEXT("MaxPrecacheShaderJobs"), MaxPrecacheShaderJobs, GEditorIni);

	MaxConcurrentShaderJobs = FPlatformMisc::NumberOfCores() * 4; // TODO: document why number of cores * 4 is a good default
	GConfig->GetInt(TEXT("CookSettings"), TEXT("MaxConcurrentShaderJobs"), MaxConcurrentShaderJobs, GEditorIni);

	PackagesPerGC = 500;
	int32 ConfigPackagesPerGC = 0;
	if (GConfig->GetInt( TEXT("CookSettings"), TEXT("PackagesPerGC"), ConfigPackagesPerGC, GEditorIni ))
	{
		// Going unsigned. Make negative values 0
		PackagesPerGC = ConfigPackagesPerGC > 0 ? ConfigPackagesPerGC : 0;
	}

	IdleTimeToGC = 20.0;
	GConfig->GetDouble( TEXT("CookSettings"), TEXT("IdleTimeToGC"), IdleTimeToGC, GEditorIni );

	auto ReadMemorySetting = [](const TCHAR* SettingName, uint64& TargetVariable)
	{
		int32 ValueInMB = 0;
		if (GConfig->GetInt(TEXT("CookSettings"), SettingName, ValueInMB, GEditorIni))
		{
			ValueInMB = FMath::Max(ValueInMB, 0);
			TargetVariable = ValueInMB * 1024LL * 1024LL;
			return true;
		}
		return false;
	};
	MemoryMaxUsedVirtual = 0;
	MemoryMaxUsedPhysical = 0;
	MemoryMinFreeVirtual = 0;
	MemoryMinFreePhysical = 0;
	ReadMemorySetting(TEXT("MemoryMaxUsedVirtual"), MemoryMaxUsedVirtual);
	ReadMemorySetting(TEXT("MemoryMaxUsedPhysical"), MemoryMaxUsedPhysical);
	ReadMemorySetting(TEXT("MemoryMinFreeVirtual"), MemoryMinFreeVirtual);
	ReadMemorySetting(TEXT("MemoryMinFreePhysical"), MemoryMinFreePhysical);

	uint64 MaxMemoryAllowance;
	if (ReadMemorySetting(TEXT("MaxMemoryAllowance"), MaxMemoryAllowance))
	{ 
		UE_LOG(LogCook, Warning, TEXT("CookSettings.MaxMemoryAllowance is deprecated. Use CookSettings.MemoryMaxUsedPhysical instead."));
		MemoryMaxUsedPhysical = MaxMemoryAllowance;
	}
	uint64 MinMemoryBeforeGC;
	if (ReadMemorySetting(TEXT("MinMemoryBeforeGC"), MinMemoryBeforeGC))
	{
		UE_LOG(LogCook, Warning, TEXT("CookSettings.MinMemoryBeforeGC is deprecated. Use CookSettings.MemoryMaxUsedVirtual instead."));
		MemoryMaxUsedVirtual = MinMemoryBeforeGC;
	}
	uint64 MinFreeMemory;
	if (ReadMemorySetting(TEXT("MinFreeMemory"), MinFreeMemory))
	{
		UE_LOG(LogCook, Warning, TEXT("CookSettings.MinFreeMemory is deprecated. Use CookSettings.MemoryMinFreePhysical instead."));
		MemoryMinFreePhysical = MinFreeMemory;
	}
	uint64 MinReservedMemory;
	if (ReadMemorySetting(TEXT("MinReservedMemory"), MinReservedMemory))
	{
		UE_LOG(LogCook, Warning, TEXT("CookSettings.MinReservedMemory is deprecated. Use CookSettings.MemoryMinFreePhysical instead."));
		MemoryMinFreePhysical = MinReservedMemory;
	}
	
	MaxPreloadAllocated = 16;
	DesiredSaveQueueLength = 8;
	DesiredLoadQueueLength = 8;
	LoadBatchSize = 16;
	RequestBatchSize = 16;

	MinFreeUObjectIndicesBeforeGC = 100000;
	GConfig->GetInt(TEXT("CookSettings"), TEXT("MinFreeUObjectIndicesBeforeGC"), MinFreeUObjectIndicesBeforeGC, GEditorIni);
	MinFreeUObjectIndicesBeforeGC = FMath::Max(MinFreeUObjectIndicesBeforeGC, 0);

	MaxNumPackagesBeforePartialGC = 400;
	GConfig->GetInt(TEXT("CookSettings"), TEXT("MaxNumPackagesBeforePartialGC"), MaxNumPackagesBeforePartialGC, GEditorIni);
	
	GConfig->GetArray(TEXT("CookSettings"), TEXT("CookOnTheFlyConfigSettingDenyList"), ConfigSettingDenyList, GEditorIni);

	UE_LOG(LogCook, Display, TEXT("CookSettings for Memory: MemoryMaxUsedVirtual %dMiB, MemoryMaxUsedPhysical %dMiB, MemoryMinFreeVirtual %dMiB, MemoryMinFreePhysical %dMiB"),
		MemoryMaxUsedVirtual / 1024 / 1024, MemoryMaxUsedPhysical / 1024 / 1024, MemoryMinFreeVirtual / 1024 / 1024, MemoryMinFreePhysical / 1024 / 1024);

	if (IsCookByTheBookMode() && !IsCookingInEditor() &&
		FPlatformMisc::SupportsMultithreadedFileHandles() && // Preloading moves file handles between threads
		!GAllowCookedDataInEditorBuilds // // Use of preloaded files is not yet implemented when GAllowCookedDataInEditorBuilds is on, see FLinkerLoad::CreateLoader
		)
	{
		bPreloadingEnabled = true;
		FLinkerLoad::SetPreloadingEnabled(true);
	}
	GConfig->GetBool(TEXT("CookSettings"), TEXT("PreexploreDependenciesEnabled"), bPreexploreDependenciesEnabled, GEditorIni);

	{
		const FConfigSection* CacheSettings = GConfig->GetSectionPrivate(TEXT("CookPlatformDataCacheSettings"), false, true, GEditorIni);
		if ( CacheSettings )
		{
			for ( const auto& CacheSetting : *CacheSettings )
			{
				
				const FString& ReadString = CacheSetting.Value.GetValue();
				int32 ReadValue = FCString::Atoi(*ReadString);
				int32 Count = FMath::Max( 2,  ReadValue );
				MaxAsyncCacheForType.Add( CacheSetting.Key,  Count );
			}
		}
		CurrentAsyncCacheForType = MaxAsyncCacheForType;
	}


	if (IsCookByTheBookMode())
	{
		CookByTheBookOptions = new FCookByTheBookOptions();
		for (TObjectIterator<UPackage> It; It; ++It)
		{
			if ((*It) != GetTransientPackage())
			{
				CookByTheBookOptions->StartupPackages.Add(It->GetFName());
				UE_LOG(LogCook, Verbose, TEXT("Cooker startup package %s"), *It->GetName());
			}
		}
		bHybridIterativeDebug = FParse::Param(FCommandLine::Get(), TEXT("hybriditerativedebug"));
	}
	
	UE_LOG(LogCook, Display, TEXT("Mobile HDR setting %d"), IsMobileHDR());

	// See if there are any plugins that need to be remapped for the sandbox
	const FProjectDescriptor* Project = IProjectManager::Get().GetCurrentProject();
	if (Project != nullptr)
	{
		PluginsToRemap = IPluginManager::Get().GetEnabledPlugins();
		TArray<FString> AdditionalPluginDirs = Project->GetAdditionalPluginDirectories();
		// Remove any plugin that is in the additional directories since they are handled normally and don't need remapping
		for (int32 Index = PluginsToRemap.Num() - 1; Index >= 0; Index--)
		{
			bool bRemove = true;
			for (const FString& PluginDir : AdditionalPluginDirs)
			{
				// If this plugin is in a directory that needs remapping
				if (PluginsToRemap[Index]->GetBaseDir().StartsWith(PluginDir))
				{
					bRemove = false;
					break;
				}
			}
			if (bRemove)
			{
				PluginsToRemap.RemoveAt(Index);
			}
		}
	}

	// Prepare a map SplitDataClass to FRegisteredCookPackageSplitter* for TryGetRegisteredCookPackageSplitter to use
	RegisteredSplitDataClasses.Reset();
	UE::Cook::Private::FRegisteredCookPackageSplitter::ForEach([this](UE::Cook::Private::FRegisteredCookPackageSplitter* RegisteredCookPackageSplitter)
	{
		UClass* SplitDataClass = RegisteredCookPackageSplitter->GetSplitDataClass();
		for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			if (ClassIt->IsChildOf(SplitDataClass) && !ClassIt->HasAnyClassFlags(CLASS_Abstract | CLASS_Deprecated | CLASS_NewerVersionExists))
			{
				RegisteredSplitDataClasses.Add(SplitDataClass, RegisteredCookPackageSplitter);
			}
		}
	});
}

bool UCookOnTheFlyServer::Exec(class UWorld* InWorld, const TCHAR* Cmd, FOutputDevice& Ar)
{
	if (FParse::Command(&Cmd, TEXT("package")))
	{
		FString PackageName;
		if (!FParse::Value(Cmd, TEXT("name="), PackageName))
		{
			Ar.Logf(TEXT("Required package name for cook package function. \"cook package name=<name> platform=<platform>\""));
			return true;
		}

		FString PlatformName;
		if (!FParse::Value(Cmd, TEXT("platform="), PlatformName))
		{
			Ar.Logf(TEXT("Required package name for cook package function. \"cook package name=<name> platform=<platform>\""));
			return true;
		}

		if (FPackageName::IsShortPackageName(PackageName))
		{
			FString OutFilename;
			if (FPackageName::SearchForPackageOnDisk(PackageName, NULL, &OutFilename))
			{
				PackageName = OutFilename;
			}
		}

		FName RawPackageName(*PackageName);
		TArray<FName> PackageNames;
		PackageNames.Add(RawPackageName);
		TMap<FName, UE::Cook::FInstigator> Instigators;
		Instigators.Add(RawPackageName, UE::Cook::EInstigator::ConsoleCommand);

		GenerateLongPackageNames(PackageNames, Instigators);

		ITargetPlatformManagerModule& TPM = GetTargetPlatformManagerRef();
		ITargetPlatform* TargetPlatform = TPM.FindTargetPlatform(PlatformName);
		if (TargetPlatform == nullptr)
		{
			Ar.Logf(TEXT("Target platform %s wasn't found."), *PlatformName);
			return true;
		}

		FCookByTheBookStartupOptions StartupOptions;

		StartupOptions.TargetPlatforms.Add(TargetPlatform);
		for (const FName& StandardPackageName : PackageNames)
		{
			FName PackageFileFName = PackageDatas->GetFileNameByPackageName(StandardPackageName);
			if (!PackageFileFName.IsNone())
			{
				StartupOptions.CookMaps.Add(StandardPackageName.ToString());
			}
		}
		StartupOptions.CookOptions = ECookByTheBookOptions::NoAlwaysCookMaps | ECookByTheBookOptions::NoDefaultMaps | ECookByTheBookOptions::NoGameAlwaysCookPackages | ECookByTheBookOptions::SkipSoftReferences | ECookByTheBookOptions::ForceDisableSaveGlobalShaders;
		
		StartCookByTheBook(StartupOptions);
	}
	else if (FParse::Command(&Cmd, TEXT("clearall")))
	{
		StopAndClearCookedData();
	}
	else if (FParse::Command(&Cmd, TEXT("stats")))
	{
		DumpStats();
	}

	return false;
}

UE::Cook::FInstigator UCookOnTheFlyServer::GetInstigator(FName PackageName)
{
	using namespace UE::Cook;

	FPackageData* PackageData = PackageDatas->FindPackageDataByPackageName(PackageName);
	if (!PackageData)
	{
		return FInstigator(EInstigator::NotYetRequested);
	}
	return PackageData->GetInstigator();
}

TArray<UE::Cook::FInstigator> UCookOnTheFlyServer::GetInstigatorChain(FName PackageName)
{
	using namespace UE::Cook;
	TArray<FInstigator> Result;
	TSet<FName> NamesOnChain;
	NamesOnChain.Add(PackageName);

	for (;;)
	{
		FPackageData* PackageData = PackageDatas->FindPackageDataByPackageName(PackageName);
		if (!PackageData)
		{
			Result.Add(FInstigator(EInstigator::NotYetRequested));
			return Result;
		}
		const FInstigator& Last = Result.Add_GetRef(PackageData->GetInstigator());
		bool bGetNext = false;
		switch (Last.Category)
		{
			case EInstigator::Dependency: bGetNext = true; break;
			case EInstigator::HardDependency: bGetNext = true; break;
			case EInstigator::SoftDependency: bGetNext = true; break;
			case EInstigator::Unsolicited: bGetNext = true; break;
			case EInstigator::GeneratedPackage: bGetNext = true; break;
			default: break;
		}
		if (!bGetNext)
		{
			return Result;
		}
		PackageName = Last.Referencer;
		if (PackageName.IsNone())
		{
			return Result;
		}
		bool bAlreadyExists = false;
		NamesOnChain.Add(PackageName, &bAlreadyExists);
		if (bAlreadyExists)
		{
			return Result;
		}
	}
	return Result; // Unreachable
}

void UCookOnTheFlyServer::DumpStats()
{
	UE_LOG(LogCook, Display, TEXT("IntStats:"));
	UE_LOG(LogCook, Display, TEXT("  %s=%d"), L"LoadPackage", this->StatLoadedPackageCount);
	UE_LOG(LogCook, Display, TEXT("  %s=%d"), L"SavedPackage", this->StatSavedPackageCount);

	OutputHierarchyTimers();
#if PROFILE_NETWORK
	UE_LOG(LogCook, Display, TEXT("Network Stats \n"
		"TimeTillRequestStarted %f\n"
		"TimeTillRequestForfilled %f\n"
		"TimeTillRequestForfilledError %f\n"
		"WaitForAsyncFilesWrites %f\n"),
		TimeTillRequestStarted,
		TimeTillRequestForfilled,
		TimeTillRequestForfilledError,

		WaitForAsyncFilesWrites);
#endif
}

uint32 UCookOnTheFlyServer::NumConnections() const
{
	int Result= 0;
	for ( int i = 0; i < NetworkFileServers.Num(); ++i )
	{
		INetworkFileServer *NetworkFileServer = NetworkFileServers[i];
		if ( NetworkFileServer )
		{
			Result += NetworkFileServer->NumConnections();
		}
	}
	return Result;
}

FString UCookOnTheFlyServer::GetOutputDirectoryOverride() const
{
	FString OutputDirectory = OutputDirectoryOverride;
	// Output directory override.	
	if (OutputDirectory.Len() <= 0)
	{
		if ( IsCookingDLC() )
		{
			check( IsCookByTheBookMode() );
			OutputDirectory = FPaths::Combine(*GetBaseDirectoryForDLC(), TEXT("Saved"), TEXT("Cooked"), TEXT("[Platform]"));
		}
		else if ( IsCookingInEditor() )
		{
			// Full path so that the sandbox wrapper doesn't try to re-base it under Sandboxes
			OutputDirectory = FPaths::Combine(*FPaths::ProjectDir(), TEXT("Saved"), TEXT("EditorCooked"), TEXT("[Platform]"));
		}
		else
		{
			// Full path so that the sandbox wrapper doesn't try to re-base it under Sandboxes
			OutputDirectory = FPaths::Combine(*FPaths::ProjectDir(), TEXT("Saved"), TEXT("Cooked"), TEXT("[Platform]"));
		}
		
		OutputDirectory = FPaths::ConvertRelativePathToFull(OutputDirectory);
	}
	else if (!OutputDirectory.Contains(TEXT("[Platform]"), ESearchCase::IgnoreCase, ESearchDir::FromEnd) )
	{
		// Output directory needs to contain [Platform] token to be able to cook for multiple targets.
		if ( IsCookByTheBookMode() )
		{
			checkf(PlatformManager->GetSessionPlatforms().Num() == 1,
				TEXT("If OutputDirectoryOverride is provided when cooking multiple platforms, it must include [Platform] in the text, to be replaced with the name of each of the requested Platforms.") );
		}
		else
		{
			// In cook on the fly mode we always add a [Platform] subdirectory rather than requiring the command-line user to include it in their path it because we assume they 
			// don't know which platforms they are cooking for up front
			OutputDirectory = FPaths::Combine(*OutputDirectory, TEXT("[Platform]"));
		}
	}
	FPaths::NormalizeDirectoryName(OutputDirectory);

	return OutputDirectory;
}

template<class T>
void GetVersionFormatNumbersForIniVersionStrings( TArray<FString>& IniVersionStrings, const FString& FormatName, const TArray<const T> &FormatArray )
{
	for ( const T& Format : FormatArray )
	{
		TArray<FName> SupportedFormats;
		Format->GetSupportedFormats(SupportedFormats);
		for ( const FName& SupportedFormat : SupportedFormats )
		{
			int32 VersionNumber = Format->GetVersion(SupportedFormat);
			FString IniVersionString = FString::Printf( TEXT("%s:%s:VersionNumber%d"), *FormatName, *SupportedFormat.ToString(), VersionNumber);
			IniVersionStrings.Emplace( IniVersionString );
		}
	}
}




template<class T>
void GetVersionFormatNumbersForIniVersionStrings(TMap<FString, FString>& IniVersionMap, const FString& FormatName, const TArray<T> &FormatArray)
{
	for (const T& Format : FormatArray)
	{
		TArray<FName> SupportedFormats;
		Format->GetSupportedFormats(SupportedFormats);
		for (const FName& SupportedFormat : SupportedFormats)
		{
			int32 VersionNumber = Format->GetVersion(SupportedFormat);
			FString IniVersionString = FString::Printf(TEXT("%s:%s:VersionNumber"), *FormatName, *SupportedFormat.ToString());
			IniVersionMap.Add(IniVersionString, FString::Printf(TEXT("%d"), VersionNumber));
		}
	}
}


void GetAdditionalCurrentIniVersionStrings( const ITargetPlatform* TargetPlatform, TMap<FString, FString>& IniVersionMap )
{
	FConfigFile EngineSettings;
	FConfigCacheIni::LoadLocalIniFile(EngineSettings, TEXT("Engine"), true, *TargetPlatform->IniPlatformName());

	TArray<FString> VersionedRValues;
	EngineSettings.GetArray(TEXT("/Script/UnrealEd.CookerSettings"), TEXT("VersionedIntRValues"), VersionedRValues);

	for (const FString& RValue : VersionedRValues)
	{
		const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(*RValue);
		if (CVar)
		{
			IniVersionMap.Add(*RValue, FString::Printf(TEXT("%d"), CVar->GetValueOnGameThread()));
		}
	}

	// save off the ddc version numbers also
	ITargetPlatformManagerModule* TPM = GetTargetPlatformManager();
	check(TPM);

	{
		TArray<FName> AllWaveFormatNames;
		TargetPlatform->GetAllWaveFormats(AllWaveFormatNames);
		TArray<const IAudioFormat*> SupportedWaveFormats;
		for ( const auto& WaveName : AllWaveFormatNames )
		{
			const IAudioFormat* AudioFormat = TPM->FindAudioFormat(WaveName);
			if (AudioFormat)
			{
				SupportedWaveFormats.Add(AudioFormat);
			}
			else
			{
				UE_LOG(LogCook, Warning, TEXT("Unable to find audio format \"%s\" which is required by \"%s\""), *WaveName.ToString(), *TargetPlatform->PlatformName());
			}
			
		}
		GetVersionFormatNumbersForIniVersionStrings(IniVersionMap, TEXT("AudioFormat"), SupportedWaveFormats);
	}

	{
		TArray<FName> AllTextureFormats;
		TargetPlatform->GetAllTextureFormats(AllTextureFormats);
		TArray<const ITextureFormat*> SupportedTextureFormats;
		for (const auto& TextureName : AllTextureFormats)
		{
			const ITextureFormat* TextureFormat = TPM->FindTextureFormat(TextureName);
			if ( TextureFormat )
			{
				SupportedTextureFormats.Add(TextureFormat);
			}
			else
			{
				UE_LOG(LogCook, Warning, TEXT("Unable to find texture format \"%s\" which is required by \"%s\""), *TextureName.ToString(), *TargetPlatform->PlatformName());
			}
		}
		GetVersionFormatNumbersForIniVersionStrings(IniVersionMap, TEXT("TextureFormat"), SupportedTextureFormats);
	}

	if (AllowShaderCompiling())
	{
		TArray<FName> AllFormatNames;
		TargetPlatform->GetAllTargetedShaderFormats(AllFormatNames);
		TArray<const IShaderFormat*> SupportedFormats;
		for (const auto& FormatName : AllFormatNames)
		{
			const IShaderFormat* Format = TPM->FindShaderFormat(FormatName);
			if ( Format )
			{
				SupportedFormats.Add(Format);
			}
			else
			{
				UE_LOG(LogCook, Warning, TEXT("Unable to find shader \"%s\" which is required by format \"%s\""), *FormatName.ToString(), *TargetPlatform->PlatformName());
			}
		}
		GetVersionFormatNumbersForIniVersionStrings(IniVersionMap, TEXT("ShaderFormat"), SupportedFormats);
	}


	// TODO: Add support for physx version tracking, currently this happens so infrequently that invalidating a cook based on it is not essential
	//GetVersionFormatNumbersForIniVersionStrings(IniVersionMap, TEXT("PhysXCooking"), TPM->GetPhysXCooking());


	if ( FParse::Param( FCommandLine::Get(), TEXT("fastcook") ) )
	{
		IniVersionMap.Add(TEXT("fastcook"));
	}

	FCustomVersionContainer AllCurrentVersions = FCurrentCustomVersions::GetAll();
	for (const FCustomVersion& CustomVersion : AllCurrentVersions.GetAllVersions())
	{
		FString CustomVersionString = FString::Printf(TEXT("%s:%s"), *CustomVersion.GetFriendlyName().ToString(), *CustomVersion.Key.ToString());
		FString CustomVersionValue = FString::Printf(TEXT("%d"), CustomVersion.Version);
		IniVersionMap.Add(CustomVersionString, CustomVersionValue);
	}

	IniVersionMap.Add(TEXT("PackageFileVersionUE4"), FString::Printf(TEXT("%d"), GPackageFileUEVersion.FileVersionUE4));
	IniVersionMap.Add(TEXT("PackageFileVersionUE5"), FString::Printf(TEXT("%d"), GPackageFileUEVersion.FileVersionUE5));
	IniVersionMap.Add(TEXT("PackageLicenseeVersion"), FString::Printf(TEXT("%d"), GPackageFileLicenseeUEVersion));

	/*FString UE4EngineVersionCompatibleName = TEXT("EngineVersionCompatibleWith");
	FString UE4EngineVersionCompatible = FEngineVersion::CompatibleWith().ToString();
	
	if ( UE4EngineVersionCompatible.Len() )
	{
		IniVersionMap.Add(UE4EngineVersionCompatibleName, UE4EngineVersionCompatible);
	}*/

	IniVersionMap.Add(TEXT("MaterialShaderMapDDCVersion"), *GetMaterialShaderMapDDCKey());
	IniVersionMap.Add(TEXT("GlobalDDCVersion"), *GetGlobalShaderMapDDCKey());
}



bool UCookOnTheFlyServer::GetCurrentIniVersionStrings( const ITargetPlatform* TargetPlatform, FIniSettingContainer& IniVersionStrings ) const
{
	IniVersionStrings = AccessedIniStrings;

	// this should be called after the cook is finished
	TArray<FString> IniFiles;
	GConfig->GetConfigFilenames(IniFiles);

	TMap<FString, int32> MultiMapCounter;

	for ( const FString& ConfigFilename : IniFiles )
	{
		if ( ConfigFilename.Contains(TEXT("CookedIniVersion.txt")) )
		{
			continue;
		}

		const FConfigFile *ConfigFile = GConfig->FindConfigFile(ConfigFilename);
		ProcessAccessedIniSettings(ConfigFile, IniVersionStrings);
		
	}

	for (const FConfigFile* ConfigFile : OpenConfigFiles)
	{
		ProcessAccessedIniSettings(ConfigFile, IniVersionStrings);
	}


	// remove any which are filtered out
	FString EditorPrefix(TEXT("Editor."));
	for ( const FString& Filter : ConfigSettingDenyList )
	{
		TArray<FString> FilterArray;
		Filter.ParseIntoArray( FilterArray, TEXT(":"));

		FString *ConfigFileName = nullptr;
		FString *SectionName = nullptr;
		FString *ValueName = nullptr;
		switch ( FilterArray.Num() )
		{
		case 3:
			ValueName = &FilterArray[2];
		case 2:
			SectionName = &FilterArray[1];
		case 1:
			ConfigFileName = &FilterArray[0];
			break;
		default:
			continue;
		}

		if ( ConfigFileName )
		{
			for ( auto ConfigFile = IniVersionStrings.CreateIterator(); ConfigFile; ++ConfigFile )
			{
				// Some deny list entries are written as *.Engine, and are intended to affect the platform-less Editor Engine.ini, which is just "Engine"
				// To make *.Engine match the editor-only config files as well, we check whether the wildcard matches either Engine or Editor.Engine for the editor files
				FString IniVersionStringFilename = ConfigFile.Key().ToString();
				if (IniVersionStringFilename.MatchesWildcard(*ConfigFileName) ||
					(!IniVersionStringFilename.Contains(TEXT(".")) && (EditorPrefix + IniVersionStringFilename).MatchesWildcard(*ConfigFileName)))
				{
					if ( SectionName )
					{
						for ( auto Section = ConfigFile.Value().CreateIterator(); Section; ++Section )
						{
							if ( Section.Key().ToString().MatchesWildcard(*SectionName))
							{
								if (ValueName)
								{
									for ( auto Value = Section.Value().CreateIterator(); Value; ++Value )
									{
										if ( Value.Key().ToString().MatchesWildcard(*ValueName))
										{
											Value.RemoveCurrent();
										}
									}
								}
								else
								{
									Section.RemoveCurrent();
								}
							}
						}
					}
					else
					{
						ConfigFile.RemoveCurrent();
					}
				}
			}
		}
	}
	return true;
}


bool UCookOnTheFlyServer::GetCookedIniVersionStrings(const ITargetPlatform* TargetPlatform, FIniSettingContainer& OutIniSettings, TMap<FString,FString>& OutAdditionalSettings) const
{
	const FString EditorIni = FPaths::ProjectDir() / TEXT("Metadata") / TEXT("CookedIniVersion.txt");
	const FString SandboxEditorIni = ConvertToFullSandboxPath(*EditorIni, true);


	const FString PlatformSandboxEditorIni = SandboxEditorIni.Replace(TEXT("[Platform]"), *TargetPlatform->PlatformName());

	TArray<FString> SavedIniVersionedParams;

	FConfigFile ConfigFile;
	ConfigFile.Read(*PlatformSandboxEditorIni);

	

	const static FName NAME_UsedSettings(TEXT("UsedSettings")); 
	const FConfigSection* UsedSettings = ConfigFile.Find(NAME_UsedSettings.ToString());
	if (UsedSettings == nullptr)
	{
		return false;
	}


	const static FName NAME_AdditionalSettings(TEXT("AdditionalSettings"));
	const FConfigSection* AdditionalSettings = ConfigFile.Find(NAME_AdditionalSettings.ToString());
	if (AdditionalSettings == nullptr)
	{
		return false;
	}


	for (const auto& UsedSetting : *UsedSettings )
	{
		FName Key = UsedSetting.Key;
		const FConfigValue& UsedValue = UsedSetting.Value;

		TArray<FString> SplitString;
		Key.ToString().ParseIntoArray(SplitString, TEXT(":"));

		if (SplitString.Num() != 4)
		{
			UE_LOG(LogCook, Warning, TEXT("Found unparsable ini setting %s for platform %s, invalidating cook."), *Key.ToString(), *TargetPlatform->PlatformName());
			return false;
		}


		check(SplitString.Num() == 4); // We generate this ini file in SaveCurrentIniSettings
		const FString& Filename = SplitString[0];
		const FString& SectionName = SplitString[1];
		const FString& ValueName = SplitString[2];
		const int32 ValueIndex = FCString::Atoi(*SplitString[3]);

		auto& OutFile = OutIniSettings.FindOrAdd(FName(*Filename));
		auto& OutSection = OutFile.FindOrAdd(FName(*SectionName));
		auto& ValueArray = OutSection.FindOrAdd(FName(*ValueName));
		if ( ValueArray.Num() < (ValueIndex+1) )
		{
			ValueArray.AddZeroed( ValueIndex - ValueArray.Num() +1 );
		}
		ValueArray[ValueIndex] = UsedValue.GetSavedValue();
	}



	for (const auto& AdditionalSetting : *AdditionalSettings)
	{
		const FName& Key = AdditionalSetting.Key;
		const FString& Value = AdditionalSetting.Value.GetSavedValue();
		OutAdditionalSettings.Add(Key.ToString(), Value);
	}

	return true;
}



void UCookOnTheFlyServer::OnFConfigCreated(const FConfigFile* Config)
{
	FScopeLock Lock(&ConfigFileCS);
	if (IniSettingRecurse)
	{
		return;
	}

	OpenConfigFiles.Add(Config);
}

void UCookOnTheFlyServer::OnFConfigDeleted(const FConfigFile* Config)
{
	FScopeLock Lock(&ConfigFileCS);
	if (IniSettingRecurse)
	{
		return;
	}

	ProcessAccessedIniSettings(Config, AccessedIniStrings);

	OpenConfigFiles.Remove(Config);
}

void UCookOnTheFlyServer::ProcessAccessedIniSettings(const FConfigFile* Config, FIniSettingContainer& OutAccessedIniStrings) const
{	
	if (Config->Name == NAME_None)
	{
		return;
	}

	// try to figure out if this config file is for a specific platform 
	FString PlatformName;
	bool bFoundPlatformName = false;

	if (GConfig->ContainsConfigFile(Config))
	{
		// If the ConfigFile is in GConfig, then it is the editor's config and is not platform specific
	}
	else if (Config->bHasPlatformName)
	{
		// The platform that was passed to LoadExternalIniFile
		PlatformName = Config->PlatformName;
		bFoundPlatformName = !PlatformName.IsEmpty();
	}
	else
	{
		// For the config files not in GConfig, we assume they were loaded from LoadConfigFile, and we match these to a platform
		// By looking for a platform-specific filepath in their SourceIniHierarchy.
		// Examples:
		// (1) ROOT\Engine\Config\Windows\WindowsEngine.ini
		// (2) ROOT\Engine\Config\Android\DataDrivePlatformInfo.ini
		// (3) ROOT\Engine\Config\Android\AndroidWindowsCompatability.ini
		// 
		// Note that for config files of form #3, we want them to be matched to Android rather than windows;
		// we assume that an exact match on a directory component is more definitive than a substring match
		bool bFoundPlatformGuess = false;
		for (auto It : FDataDrivenPlatformInfoRegistry::GetAllPlatformInfos())
		{
			const FString CurrentPlatformName = It.Key.ToString();
			TStringBuilder<128> PlatformDirString;
			PlatformDirString.Appendf(TEXT("/%s/"), *CurrentPlatformName);
			for (const auto& SourceIni : Config->SourceIniHierarchy)
			{
				// Look for platform in the path, rating a full subdirectory name match (/Android/ or /Windows/) higher than a partial filename match (AndroidEngine.ini or WindowsEngine.ini)
				bool bFoundPlatformDir = UE::String::FindFirst(SourceIni.Value.Filename, PlatformDirString, ESearchCase::IgnoreCase) != INDEX_NONE;
				bool bFoundPlatformSubstring = UE::String::FindFirst(SourceIni.Value.Filename, CurrentPlatformName, ESearchCase::IgnoreCase) != INDEX_NONE;
				if (bFoundPlatformDir)
				{
					PlatformName = CurrentPlatformName;
					bFoundPlatformName = true;
					break;
				}
				else if (!bFoundPlatformGuess && bFoundPlatformSubstring)
				{
					PlatformName = CurrentPlatformName;
					bFoundPlatformGuess = true;
				}
			}
			if (bFoundPlatformName)
			{
				break;
			}
		}
		bFoundPlatformName = bFoundPlatformName || bFoundPlatformGuess;
	}

	TStringBuilder<128> ConfigName;
	if (bFoundPlatformName)
	{
		ConfigName << PlatformName;
		ConfigName << TEXT(".");
	}
	Config->Name.AppendString(ConfigName);
	const FName& ConfigFName = FName(ConfigName);
	TSet<FName> ProcessedValues;
	TCHAR PlainNameString[NAME_SIZE];
	TArray<const FConfigValue*> ValueArray;
	for ( auto& ConfigSection : *Config )
	{
		ProcessedValues.Reset();
		const FName SectionName = FName(*ConfigSection.Key);

		SectionName.GetPlainNameString(PlainNameString);
		if ( TCString<TCHAR>::Strstr(PlainNameString, TEXT(":")) )
		{
			UE_LOG(LogCook, Verbose, TEXT("Ignoring ini section checking for section name %s because it contains ':'"), PlainNameString);
			continue;
		}

		for ( auto& ConfigValue : ConfigSection.Value )
		{
			const FName& ValueName = ConfigValue.Key;
			if ( ProcessedValues.Contains(ValueName) )
				continue;

			ProcessedValues.Add(ValueName);

			ValueName.GetPlainNameString(PlainNameString);
			if (TCString<TCHAR>::Strstr(PlainNameString, TEXT(":")))
			{
				UE_LOG(LogCook, Verbose, TEXT("Ignoring ini section checking for section name %s because it contains ':'"), PlainNameString);
				continue;
			}

			
			ValueArray.Reset();
			ConfigSection.Value.MultiFindPointer( ValueName, ValueArray, true );

			bool bHasBeenAccessed = false;
			for (const FConfigValue* ValueArrayEntry : ValueArray)
			{
				if (ValueArrayEntry->HasBeenRead())
				{
					bHasBeenAccessed = true;
					break;
				}
			}

			if ( bHasBeenAccessed )
			{
				auto& AccessedConfig = OutAccessedIniStrings.FindOrAdd(ConfigFName);
				auto& AccessedSection = AccessedConfig.FindOrAdd(SectionName);
				auto& AccessedKey = AccessedSection.FindOrAdd(ValueName);
				AccessedKey.Empty(ValueArray.Num());
				for (const FConfigValue* ValueArrayEntry : ValueArray )
				{
					FString RemovedColon = ValueArrayEntry->GetSavedValue().Replace(TEXT(":"), TEXT(""));
					AccessedKey.Add(MoveTemp(RemovedColon));
				}
			}
			
		}
	}
}

bool UCookOnTheFlyServer::IniSettingsOutOfDate(const ITargetPlatform* TargetPlatform) const
{
	TGuardValue<bool> A(IniSettingRecurse, true);

	FIniSettingContainer OldIniSettings;
	TMap<FString, FString> OldAdditionalSettings;
	if ( GetCookedIniVersionStrings(TargetPlatform, OldIniSettings, OldAdditionalSettings) == false)
	{
		UE_LOG(LogCook, Display, TEXT("Unable to read previous cook inisettings for platform %s invalidating cook"), *TargetPlatform->PlatformName());
		return true;
	}

	// compare against current settings
	TMap<FString, FString> CurrentAdditionalSettings;
	GetAdditionalCurrentIniVersionStrings(TargetPlatform, CurrentAdditionalSettings);

	for ( const auto& OldIniSetting : OldAdditionalSettings)
	{
		const FString* CurrentValue = CurrentAdditionalSettings.Find(OldIniSetting.Key);
		if ( !CurrentValue )
		{
			UE_LOG(LogCook, Display, TEXT("Previous cook had additional ini setting: %s current cook is missing this setting."), *OldIniSetting.Key);
			return true;
		}

		if ( *CurrentValue != OldIniSetting.Value )
		{
			UE_LOG(LogCook, Display, TEXT("Additional Setting from previous cook %s doesn't match %s vs %s"), *OldIniSetting.Key, **CurrentValue, *OldIniSetting.Value );
			return true;
		}
	}

	for (const auto& OldIniFile : OldIniSettings)
	{
		FName ConfigNameKey = OldIniFile.Key;

		TArray<FString> ConfigNameArray;
		ConfigNameKey.ToString().ParseIntoArray(ConfigNameArray, TEXT("."));
		FString Filename;
		FString PlatformName;
		// The input NameKey is of the form 
		//   Platform.ConfigName:Section:Key:ArrayIndex=Value
		// The Platform is optional and will not be present if the configfile was an editor config file rather than a platform-specific config file
		bool bFoundPlatformName = false;
		if (ConfigNameArray.Num() <= 1)
		{
			Filename = ConfigNameKey.ToString();
		}
		else if (ConfigNameArray.Num() == 2)
		{
			PlatformName = ConfigNameArray[0];
			Filename = ConfigNameArray[1];
			bFoundPlatformName = true;
		}
		else
		{
			UE_LOG(LogCook, Warning, TEXT("Found invalid file name in old ini settings file Filename %s settings file %s"), *ConfigNameKey.ToString(), *TargetPlatform->PlatformName());
			return true;
		}
		
		const FConfigFile* ConfigFile = nullptr;
		FConfigFile Temp;
		if (bFoundPlatformName)
		{
			// For the platform-specific old ini files, load them using LoadLocalIniFiles; this matches the assumption in SaveCurrentIniSettings
			// that the platform-specific ini files were loaded by LoadLocalIniFiles
			FConfigCacheIni::LoadLocalIniFile(Temp, *Filename, true, *PlatformName);
			ConfigFile = &Temp;
		}
		else
		{
			// For the platform-agnostic old ini files, read them from GConfig; this matches where we loaded them from in SaveCurrentIniSettings
			// The ini files may have been saved by fullpath or by shortname; search first for a fullpath match using FindConfigFile and
			// if that fails search for the shortname match by iterating over all files in GConfig
			ConfigFile = GConfig->FindConfigFile(Filename);
		}
		if (!ConfigFile)
		{
			FName FileFName = FName(*Filename);
			for (const FString& ConfigFilename : GConfig->GetFilenames())
			{
				FConfigFile* File = GConfig->FindConfigFile(ConfigFilename);
				if (File->Name == FileFName)
				{
					ConfigFile = File;
					break;
				}
			}
			if (!ConfigFile)
			{
				UE_LOG(LogCook, Display, TEXT("Unable to find config file %s invalidating inisettings"), *FString::Printf(TEXT("%s %s"), *PlatformName, *Filename));
				return true;
			}
		}
		for ( const auto& OldIniSection : OldIniFile.Value )
		{
			const FName& SectionName = OldIniSection.Key;
			const FConfigSection* IniSection = ConfigFile->Find( SectionName.ToString() );
			const FString DenyListSetting = FString::Printf(TEXT("%s%s%s:%s"), *PlatformName, bFoundPlatformName ? TEXT(".") : TEXT(""), *Filename, *SectionName.ToString());

			if ( IniSection == nullptr )
			{
				UE_LOG(LogCook, Display, TEXT("Inisetting is different for %s, Current section doesn't exist"), 
					*FString::Printf(TEXT("%s %s %s"), *PlatformName, *Filename, *SectionName.ToString()));
				UE_LOG(LogCook, Display, TEXT("To avoid this add a deny list setting to DefaultEditor.ini [CookSettings] %s"), *DenyListSetting);
				return true;
			}

			for ( const auto& OldIniValue : OldIniSection.Value )
			{
				const FName& ValueName = OldIniValue.Key;

				TArray<FConfigValue> CurrentValues;
				IniSection->MultiFind( ValueName, CurrentValues, true );

				if ( CurrentValues.Num() != OldIniValue.Value.Num() )
				{
					UE_LOG(LogCook, Display, TEXT("Inisetting is different for %s, missmatched num array elements %d != %d "), *FString::Printf(TEXT("%s %s %s %s"),
						*PlatformName, *Filename, *SectionName.ToString(), *ValueName.ToString()), CurrentValues.Num(), OldIniValue.Value.Num());
					UE_LOG(LogCook, Display, TEXT("To avoid this add a deny list setting to DefaultEditor.ini [CookSettings] %s"), *DenyListSetting);
					return true;
				}
				for ( int Index = 0; Index < CurrentValues.Num(); ++Index )
				{
					const FString FilteredCurrentValue = CurrentValues[Index].GetSavedValue().Replace(TEXT(":"), TEXT(""));
					if ( FilteredCurrentValue != OldIniValue.Value[Index] )
					{
						UE_LOG(LogCook, Display, TEXT("Inisetting is different for %s, value %s != %s invalidating cook"),
							*FString::Printf(TEXT("%s %s %s %s %d"),*PlatformName, *Filename, *SectionName.ToString(), *ValueName.ToString(), Index),
							*CurrentValues[Index].GetSavedValue(), *OldIniValue.Value[Index] );
						UE_LOG(LogCook, Display, TEXT("To avoid this add a deny list setting to DefaultEditor.ini [CookSettings] %s"), *DenyListSetting);
						return true;
					}
				}
			}
		}
	}

	return false;
}

bool UCookOnTheFlyServer::SaveCurrentIniSettings(const ITargetPlatform* TargetPlatform) const
{
	TGuardValue<bool> S(IniSettingRecurse, true);

	TMap<FString, FString> AdditionalIniSettings;
	GetAdditionalCurrentIniVersionStrings(TargetPlatform, AdditionalIniSettings);

	FIniSettingContainer CurrentIniSettings;
	GetCurrentIniVersionStrings(TargetPlatform, CurrentIniSettings);

	const FString EditorIni = FPaths::ProjectDir() / TEXT("Metadata") / TEXT("CookedIniVersion.txt");
	const FString SandboxEditorIni = ConvertToFullSandboxPath(*EditorIni, true);


	const FString PlatformSandboxEditorIni = SandboxEditorIni.Replace(TEXT("[Platform]"), *TargetPlatform->PlatformName());


	FConfigFile ConfigFile;
	// ConfigFile.Read(*PlatformSandboxEditorIni);

	ConfigFile.Dirty = true;
	const static FName NAME_UsedSettings(TEXT("UsedSettings"));
	ConfigFile.Remove(NAME_UsedSettings.ToString());
	FConfigSection& UsedSettings = ConfigFile.FindOrAdd(NAME_UsedSettings.ToString());


	{
		UE_SCOPED_HIERARCHICAL_COOKTIMER(ProcessingAccessedStrings)
		for (const auto& CurrentIniFilename : CurrentIniSettings)
		{
			const FName& Filename = CurrentIniFilename.Key;
			for ( const auto& CurrentSection : CurrentIniFilename.Value )
			{
				const FName& Section = CurrentSection.Key;
				for ( const auto& CurrentValue : CurrentSection.Value )
				{
					const FName& ValueName = CurrentValue.Key;
					const TArray<FString>& Values = CurrentValue.Value;

					for ( int Index = 0; Index < Values.Num(); ++Index )
					{
						FString NewKey = FString::Printf(TEXT("%s:%s:%s:%d"), *Filename.ToString(), *Section.ToString(), *ValueName.ToString(), Index);
						UsedSettings.Add(FName(*NewKey), Values[Index]);
					}
				}
			}
		}
	}


	const static FName NAME_AdditionalSettings(TEXT("AdditionalSettings"));
	ConfigFile.Remove(NAME_AdditionalSettings.ToString());
	FConfigSection& AdditionalSettings = ConfigFile.FindOrAdd(NAME_AdditionalSettings.ToString());

	for (const auto& AdditionalIniSetting : AdditionalIniSettings)
	{
		AdditionalSettings.Add( FName(*AdditionalIniSetting.Key), AdditionalIniSetting.Value );
	}

	ConfigFile.Write(PlatformSandboxEditorIni);


	return true;

}

void UCookOnTheFlyServer::TrySetDefaultAsyncIODeletePlatform(const FString& PlatformName)
{
	if (DefaultAsyncIODeletePlatformName.IsEmpty())
	{
		DefaultAsyncIODeletePlatformName = PlatformName;
	}
}

FAsyncIODelete& UCookOnTheFlyServer::GetAsyncIODelete()
{
	if (AsyncIODelete)
	{
		return *AsyncIODelete;
	}

	// The PlatformName is used construct a directory that we can be sure no other process is using (because a sandbox can only be cooked by one process at a time)
	// The TempRoot we will delete into is a sibling of the the Platform-specific sandbox directory, with name [PlatformDir]AsyncDelete
	// Note that two UnrealEd-Cmd processes cooking to the same sandbox at the same time will therefore cause an error since FAsyncIODelete doesn't handle multiple processes sharing TempRoots.
	// That simultaneous-cook behavior is also not supported in other assumptions throughout the cooker.
	check(!DefaultAsyncIODeletePlatformName.IsEmpty());
	FString DeleteDirectory = GetSandboxDirectory(DefaultAsyncIODeletePlatformName);
	FPaths::NormalizeDirectoryName(DeleteDirectory);
	DeleteDirectory += TEXT("_Del");

	AsyncIODelete = MakeUnique<FAsyncIODelete>(DeleteDirectory);
	return *AsyncIODelete;
}

void UCookOnTheFlyServer::PopulateCookedPackages(TArrayView<const ITargetPlatform* const> TargetPlatforms)
{
	using namespace UE::Cook;
	TRACE_CPUPROFILER_EVENT_SCOPE(UCookOnTheFlyServer::PopulateCookedPackages);

	// TODO: NumPackagesIterativelySkipped is only counted for the first platform; to count all platforms we would
	// have to check whether each one is already cooked.
	bool bFirstPlatform = true;
	COOK_STAT(DetailedCookStats::NumPackagesIterativelySkipped = 0);
	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		FAssetRegistryGenerator& PlatformAssetRegistry = *(PlatformManager->GetPlatformData(TargetPlatform)->RegistryGenerator);
		FCookSavePackageContext& CookSavePackageContext = FindOrCreateSaveContext(TargetPlatform);
		ICookedPackageWriter& PackageWriter = *CookSavePackageContext.PackageWriter;
		UE_LOG(LogCook, Display, TEXT("Populating cooked package(s) from %s package store on platform '%s'"),
			*CookSavePackageContext.WriterDebugName, *TargetPlatform->PlatformName());

		TUniquePtr<FAssetRegistryState> PreviousAssetRegistry(PackageWriter.LoadPreviousAssetRegistry());
		int32 NumPreviousPackages = PreviousAssetRegistry ? PreviousAssetRegistry->GetNumPackages() : 0;
		UE_LOG(LogCook, Display, TEXT("Found '%d' cooked package(s) in package store"), NumPreviousPackages);
		if (NumPreviousPackages == 0)
		{
			continue;
		}

		if (bHybridIterativeEnabled)
		{
			// HybridIterative does the equivalent operation of bRecurseModifications=true and bRecurseScriptModifications=true,
			// but checks for out-of-datedness are done by FRequestCluster using the TargetDomainKey (which is built
			// from dependencies), so we do not need to check for out-of-datedness here

			// Remove packages that no longer exist in the WorkspaceDomain from the TargetDomain. We have to
			// check this for all packages in the previous cook rather than just the currently referenced
			// set because when packages are removed the references to them are usually also removed.
			TArray<FName> TombstonePackages;
			PlatformAssetRegistry.ComputePackageRemovals(*PreviousAssetRegistry, TombstonePackages);
			PackageWriter.RemoveCookedPackages(TombstonePackages);
		}
		else
		{
			// Without hybrid iterative, we use the AssetRegistry graph of dependencies to find out of date packages
			// We also implement other legacy -iterate behaviors:
			//  *) Remove modified packages from the PackageWriter in addition to the no-longer-exist packages
			//  *) Skip packages that failed to cook on the previous cook
			//  *) Cook all modified packages even if the requested cook packages don't reference them
			FAssetRegistryGenerator::FComputeDifferenceOptions Options;
			Options.bRecurseModifications = true;
			Options.bRecurseScriptModifications = !IsCookFlagSet(ECookInitializationFlags::IgnoreScriptPackagesOutOfDate);
			FAssetRegistryGenerator::FAssetRegistryDifference Difference;
			PlatformAssetRegistry.ComputePackageDifferences(Options, *PreviousAssetRegistry, Difference);

			TArray<FName> PackagesToRemove;
			PackagesToRemove.Reserve(Difference.ModifiedPackages.Num() + Difference.RemovedPackages.Num() + Difference.IdenticalUncookedPackages.Num());
			for (FName PackageToRemove : Difference.ModifiedPackages)
			{
				PackagesToRemove.Add(PackageToRemove);
			}
			for (FName PackageToRemove : Difference.RemovedPackages)
			{
				PackagesToRemove.Add(PackageToRemove);
			}
			for (FName PackageToRemove : Difference.IdenticalUncookedPackages)
			{
				PackagesToRemove.Add(PackageToRemove);
			}

			UE_LOG(LogCook, Display, TEXT("Keeping '%d' and removing '%d' cooked package(s)"), Difference.IdenticalCookedPackages.Num(), PackagesToRemove.Num());

			PackageWriter.RemoveCookedPackages(PackagesToRemove);

			if (bFirstPlatform)
			{
				COOK_STAT(DetailedCookStats::NumPackagesIterativelySkipped += Difference.IdenticalCookedPackages.Num());
				bFirstPlatform = false;
			}
			for (const FName& IdenticalPackage : Difference.IdenticalCookedPackages)
			{
				// Mark this package as cooked so that we don't unnecessarily try to cook it again
				FPackageData* PackageData = PackageDatas->TryAddPackageDataByPackageName(IdenticalPackage);
				if (PackageData)
				{
					PackageData->SetPlatformCooked(TargetPlatform, true /* bSucceeded */);
				}

				// Declare the package to the EDLCookInfo verification so we don't warn about missing exports from it
				UE::SavePackageUtilities::EDLCookInfoAddIterativelySkippedPackage(IdenticalPackage);
			}
			PackageWriter.MarkPackagesUpToDate(Difference.IdenticalCookedPackages.Array());
			for (FName UncookedPackage : Difference.IdenticalUncookedPackages)
			{
				// Mark this failed-to-cook package as cooked so that we don't unnecessarily try to cook it again
				FPackageData* PackageData = PackageDatas->TryAddPackageDataByPackageName(UncookedPackage);
				if (PackageData)
				{
					ensure(!PackageData->HasAnyCookedPlatforms({ TargetPlatform }, /* bIncludeFailed */ false));
					PackageData->SetPlatformCooked(TargetPlatform, false /* bSucceeded */);
				}
			}
			if (IsCookByTheBookMode())
			{
				for (const FName& RemovePackageName : Difference.ModifiedPackages)
				{
					// cook on the fly will requeue this package when it wants it, but for cook by the book we force cook the modified file
					// so that the output set of packages is up to date (even if the user is currently cooking only a subset)
					FPackageData* PackageData = PackageDatas->TryAddPackageDataByPackageName(RemovePackageName);
					if (PackageData)
					{
						ExternalRequests->EnqueueUnique(FFilePlatformRequest(PackageData->GetFileName(),
							EInstigator::IterativeCook, TConstArrayView<const ITargetPlatform*>{ TargetPlatform }));
					}
				}
			}
		}

		PlatformAssetRegistry.SetPreviousAssetRegistry(MoveTemp(PreviousAssetRegistry));
	}
}

const FString ExtractPackageNameFromObjectPath( const FString ObjectPath )
{
	// get the path 
	int32 Beginning = ObjectPath.Find(TEXT("'"), ESearchCase::CaseSensitive);
	if ( Beginning == INDEX_NONE )
	{
		return ObjectPath;
	}
	int32 End = ObjectPath.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, Beginning + 1);
	if (End == INDEX_NONE )
	{
		End = ObjectPath.Find(TEXT("'"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Beginning + 1);
	}
	if ( End == INDEX_NONE )
	{
		// one more use case is that the path is "Class'Path" example "OrionBoostItemDefinition'/Game/Misc/Boosts/XP_1Win" dunno why but this is actually dumb
		if ( ObjectPath[Beginning+1] == '/' )
		{
			return ObjectPath.Mid(Beginning+1);
		}
		return ObjectPath;
	}
	return ObjectPath.Mid(Beginning + 1, End - Beginning - 1);
}

#if ASSET_REGISTRY_STATE_DUMPING_ENABLED
void DumpAssetRegistryForCooker(IAssetRegistry* AssetRegistry)
{
	FString DumpDir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() + TEXT("Reports/AssetRegistryStatePages"));
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	FAsyncIODelete DeleteReportDir(DumpDir + TEXT("_Del"));
	DeleteReportDir.DeleteDirectory(DumpDir);
	PlatformFile.CreateDirectoryTree(*DumpDir);
	TArray<FString> Pages;
	TArray<FString> Arguments({ TEXT("ObjectPath"),TEXT("PackageName"),TEXT("Path"),TEXT("Class"),TEXT("Tag"), TEXT("DependencyDetails"), TEXT("PackageData"), TEXT("LegacyDependencies") });
	AssetRegistry->DumpState(Arguments, Pages, 10000 /* LinesPerPage */);
	int PageIndex = 0;
	TStringBuilder<256> FileName;
	for (FString& PageText : Pages)
	{
		FileName.Reset();
		FileName.Appendf(TEXT("%s_%05d.txt"), *(DumpDir / TEXT("Page")), PageIndex++);
		PageText.ToLowerInline();
		FFileHelper::SaveStringToFile(PageText, *FileName);
	}
}
#endif


void UCookOnTheFlyServer::GenerateAssetRegistry()
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UCookOnTheFlyServer::GenerateAssetRegistry);

	// Cache asset registry for later
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	AssetRegistry = &AssetRegistryModule.Get();

	// Mark package as dirty for the last ones saved
	for (FName AssetFilename : ModifiedAssetFilenames)
	{
		const FString AssetPathOnDisk = AssetFilename.ToString();
		if (FPaths::FileExists(AssetPathOnDisk))
		{
			const FString PackageName = FPackageName::FilenameToLongPackageName(AssetPathOnDisk);
			FSoftObjectPath SoftPackage(PackageName);
			if (UPackage* Package = Cast<UPackage>(SoftPackage.ResolveObject()))
			{
				MarkPackageDirtyForCooker(Package, true);
			}
		}
	}

	if (!!(CookFlags & ECookInitializationFlags::GeneratedAssetRegistry))
	{
		UE_LOG(LogCook, Display, TEXT("Updating asset registry"));

		// Force a rescan of modified package files
		TArray<FString> ModifiedPackageFileList;

		for (FName ModifiedPackage : ModifiedAssetFilenames)
		{
			ModifiedPackageFileList.Add(ModifiedPackage.ToString());
		}

		AssetRegistry->ScanModifiedAssetFiles(ModifiedPackageFileList);
	}
	else
	{
		CookFlags |= ECookInitializationFlags::GeneratedAssetRegistry;
		UE_LOG(LogCook, Display, TEXT("Creating asset registry"));

		ModifiedAssetFilenames.Reset();
		UE::Cook::FPackageDatas::OnAssetRegistryGenerated(*AssetRegistry);
	}
}

void UCookOnTheFlyServer::BlockOnAssetRegistry()
{
	if (bHasBlockedOnAssetRegistry)
	{
		return;
	}
	bHasBlockedOnAssetRegistry = true;
	if (ShouldPopulateFullAssetRegistry())
	{
		// Trigger or waitfor completion the primary AssetRegistry scan.
		// Additionally scan any cook-specific paths from ini
		TArray<FString> ScanPaths;
		GConfig->GetArray(TEXT("AssetRegistry"), TEXT("PathsToScanForCook"), ScanPaths, GEngineIni);
		AssetRegistry->ScanPathsSynchronous(ScanPaths);
		if (AssetRegistry->IsSearchAsync() && AssetRegistry->IsSearchAllAssets())
		{
			AssetRegistry->WaitForCompletion();
		}
		else
		{
			AssetRegistry->SearchAllAssets(true /* bSynchronousSearch */);
		}
	}
	else if (IsCookingDLC())
	{
		TArray<FString> ScanPaths;
		ScanPaths.Add(FString::Printf(TEXT("/%s/"), *CookByTheBookOptions->DlcName));
		AssetRegistry->ScanPathsSynchronous(ScanPaths);
	}

#if ASSET_REGISTRY_STATE_DUMPING_ENABLED
	if (FParse::Param(FCommandLine::Get(), TEXT("DumpAssetRegistry")))
	{
		DumpAssetRegistryForCooker(AssetRegistry);
	}
#endif
}

void UCookOnTheFlyServer::RefreshPlatformAssetRegistries(const TArrayView<const ITargetPlatform* const>& TargetPlatforms)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UCookOnTheFlyServer::RefreshPlatformAssetRegistries);

	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		FName PlatformName = FName(*TargetPlatform->PlatformName());

		UE::Cook::FPlatformData* PlatformData = PlatformManager->GetPlatformData(TargetPlatform);
		FAssetRegistryGenerator* RegistryGenerator = PlatformData->RegistryGenerator.Get();
		if (!RegistryGenerator)
		{
			RegistryGenerator = new FAssetRegistryGenerator(TargetPlatform);
			PlatformData->RegistryGenerator = TUniquePtr<FAssetRegistryGenerator>(RegistryGenerator);
			RegistryGenerator->CleanManifestDirectories();
		}
		RegistryGenerator->Initialize(CookByTheBookOptions ? CookByTheBookOptions->StartupPackages : TArray<FName>());
	}
}

void UCookOnTheFlyServer::GenerateLongPackageNames(TArray<FName>& FilesInPath, TMap<FName, UE::Cook::FInstigator>& Instigators)
{
	TSet<FName> FilesInPathSet;
	TArray<FName> FilesInPathReverse;
	TMap<FName, UE::Cook::FInstigator> NewInstigators;
	FilesInPathSet.Reserve(FilesInPath.Num());
	FilesInPathReverse.Reserve(FilesInPath.Num());
	NewInstigators.Reserve(Instigators.Num());

	for (int32 FileIndex = 0; FileIndex < FilesInPath.Num(); FileIndex++)
	{
		const FName& FileInPathFName = FilesInPath[FilesInPath.Num() - FileIndex - 1];
		const FString& FileInPath = FileInPathFName.ToString();
		UE::Cook::FInstigator& Instigator = Instigators.FindChecked(FileInPathFName);
		if (FPackageName::IsValidLongPackageName(FileInPath))
		{
			bool bIsAlreadyAdded;
			FilesInPathSet.Add(FileInPathFName, &bIsAlreadyAdded);
			if (!bIsAlreadyAdded)
			{
				FilesInPathReverse.Add(FileInPathFName);
				NewInstigators.Add(FileInPathFName, MoveTemp(Instigator));
			}
		}
		else
		{
			FString LongPackageName;
			FPackageName::EErrorCode FailureReason;
			if (FPackageName::TryConvertToMountedPath(FileInPath, nullptr /* LocalPath */, &LongPackageName,
				nullptr /* ObjectName */, nullptr /* SubObjectName */, nullptr /* Extension */,
				nullptr /* FlexNameType */, &FailureReason)
				||
				(FPackageName::IsShortPackageName(FileInPath) &&
					FPackageName::SearchForPackageOnDisk(FileInPath, &LongPackageName, nullptr))
				)
			{
				const FName LongPackageFName(*LongPackageName);
				bool bIsAlreadyAdded;
				FilesInPathSet.Add(LongPackageFName, &bIsAlreadyAdded);
				if (!bIsAlreadyAdded)
				{
					FilesInPathReverse.Add(LongPackageFName);
					NewInstigators.Add(LongPackageFName, MoveTemp(Instigator));
				}
			}
			else
			{
				LogCookerMessage(FString::Printf(TEXT("Unable to generate long package name, %s. %s"), *FileInPath,
					*FPackageName::FormatErrorAsString(FileInPath, FailureReason)), EMessageSeverity::Warning);
			}
		}
	}
	FilesInPath.Empty(FilesInPathReverse.Num());
	FilesInPath.Append(FilesInPathReverse);
	Swap(Instigators, NewInstigators);
}

void UCookOnTheFlyServer::AddFileToCook( TArray<FName>& InOutFilesToCook,
	TMap<FName, UE::Cook::FInstigator>& InOutInstigators,
	const FString &InFilename, const UE::Cook::FInstigator& Instigator) const
{ 
	using namespace UE::Cook;

	if (!FPackageName::IsScriptPackage(InFilename) && !FPackageName::IsMemoryPackage(InFilename))
	{
		FName InFilenameName = FName(*InFilename);
		if (InFilenameName.IsNone())
		{
			return;
		}

		FInstigator& ExistingInstigator = InOutInstigators.FindOrAdd(InFilenameName);
		if (ExistingInstigator.Category == EInstigator::InvalidCategory)
		{
			InOutFilesToCook.Add(InFilenameName);
			ExistingInstigator = Instigator;
		}
	}
}

const TCHAR* GCookRequestUsageMessage = TEXT(
	"By default, the cooker does not cook any packages. Packages must be requested by one of the following methods.\n"
	"All transitive dependencies of a requested package are also cooked. Packages can be specified by LocalFilename/Filepath\n"
	"or by LongPackagename/LongPackagePath.\n"
	"	RecommendedMethod:\n"
	"		Use the AssetManager's default behavior of PrimaryAssetTypesToScan rules\n"
	"			Engine.ini:[/Script/Engine.AssetManagerSettings]:+PrimaryAssetTypesToScan\n"
	"	Commandline:\n"
	"		-package=<PackageName>\n"
	"			Request the given package.\n"
	"		-cookdir=<PackagePath>\n"
	"			Request all packages in the given directory.\n"
	"		-mapinisection=<SectionNameInEditorIni>	\n"
	"			Specify an ini section of packages to cook, in the style of AlwaysCookMaps.\n"
	"	Ini:\n"
	"		Editor.ini\n"
	"			[AlwaysCookMaps]\n"
	"				+Map=<PackageName>\n"
	"					; Request the package on every cook. Repeatable.\n"
	"			[AllMaps]\n"
	"				+Map=<PackageName>\n"
	"					; Request the package on default cooks. Not used if commandline, AlwaysCookMaps, or MapsToCook are present.\n"
	"		Game.ini\n"
	"			[/Script/UnrealEd.ProjectPackagingSettings]\n"
	"				+MapsToCook=(FilePath=\"<PackageName>\")\n"
	"					; Request the package in default cooks. Repeatable.\n"
	"					; Not used if commandline packages or AlwaysCookMaps are present.\n"
	"				DirectoriesToAlwaysCook=(Path=\"<PackagePath>\")\n"
	"					; Request the array of packages in every cook. Repeatable.\n"
	"				bCookAll=true\n"
	"					; \n"
	"		Engine.ini\n"
	"			[/Script/EngineSettings.GameMapsSettings]\n"
	"				GameDefaultMap=<PackageName>\n"
	"				; And other default types; see GameMapsSettings.\n"
	"	C++API\n"
	"		FAssetManager::ModifyCook\n"
	"			// Subclass FAssetManager (Engine.ini:[/Script/Engine.Engine]:AssetManagerClassName) and override this hook.\n"
	"           // The default AssetManager behavior cooks all packages specified by PrimaryAssetTypesToScan rules from ini.\n"
	"		FGameDelegates::Get().GetModifyCookDelegate()\n"
	"			// Subscribe to this delegate during your module startup.\n"
	"		ITargetPlatform::GetExtraPackagesToCook\n"
	"			// Override this hook on a given TargetPlatform.\n"
);
void UCookOnTheFlyServer::CollectFilesToCook(TArray<FName>& FilesInPath, TMap<FName, UE::Cook::FInstigator>& Instigators,
	const TArray<FString>& CookMaps, const TArray<FString>& InCookDirectories,
	const TArray<FString> &IniMapSections, ECookByTheBookOptions FilesToCookFlags, const TArrayView<const ITargetPlatform* const>& TargetPlatforms,
	const TMap<FName, TArray<FName>>& GameDefaultObjects)
{
	UE_SCOPED_HIERARCHICAL_COOKTIMER(CollectFilesToCook);
	using namespace UE::Cook;

	if (FParse::Param(FCommandLine::Get(), TEXT("helpcookusage")))
	{
		UE::String::ParseLines(GCookRequestUsageMessage, [](FStringView Line)
			{
				UE_LOG(LogCook, Warning, TEXT("%.*s"), Line.Len(), Line.GetData());
			});
	}
	UProjectPackagingSettings* PackagingSettings = Cast<UProjectPackagingSettings>(UProjectPackagingSettings::StaticClass()->GetDefaultObject());

	bool bCookAll = (!!(FilesToCookFlags & ECookByTheBookOptions::CookAll)) || PackagingSettings->bCookAll;
	bool bMapsOnly = (!!(FilesToCookFlags & ECookByTheBookOptions::MapsOnly)) || PackagingSettings->bCookMapsOnly;
	bool bNoDev = !!(FilesToCookFlags & ECookByTheBookOptions::NoDevContent);

	int32 InitialNum = FilesInPath.Num();
	struct FNameWithInstigator
	{
		FInstigator Instigator;
		FName Name;
	};
	TArray<FNameWithInstigator> CookDirectories;
	for (const FString& InCookDirectory : InCookDirectories)
	{
		FName InCookDirectoryName(*InCookDirectory);
		CookDirectories.Add(FNameWithInstigator{
			FInstigator(EInstigator::CommandLineDirectory, InCookDirectoryName), InCookDirectoryName });
	}
	
	if (!IsCookingDLC() && 
		!(FilesToCookFlags & ECookByTheBookOptions::NoAlwaysCookMaps))
	{

		{
			TArray<FString> MapList;
			// Add the default map section
			GEditor->LoadMapListFromIni(TEXT("AlwaysCookMaps"), MapList);

			for (int32 MapIdx = 0; MapIdx < MapList.Num(); MapIdx++)
			{
				UE_LOG(LogCook, Verbose, TEXT("Maplist contains has %s "), *MapList[MapIdx]);
				AddFileToCook(FilesInPath, Instigators, MapList[MapIdx], EInstigator::AlwaysCookMap);
			}
		}


		bool bFoundMapsToCook = CookMaps.Num() > 0;

		{
			TArray<FString> MapList;
			for (const FString& IniMapSection : IniMapSections)
			{
				UE_LOG(LogCook, Verbose, TEXT("Loading map ini section %s"), *IniMapSection);
				MapList.Reset();
				GEditor->LoadMapListFromIni(*IniMapSection, MapList);
				FName MapSectionName(*IniMapSection);
				for (const FString& MapName : MapList)
				{
					UE_LOG(LogCook, Verbose, TEXT("Maplist contains %s"), *MapName);
					AddFileToCook(FilesInPath, Instigators, MapName,
						FInstigator(EInstigator::IniMapSection, MapSectionName));
					bFoundMapsToCook = true;
				}
			}
		}

		// If we didn't find any maps look in the project settings for maps
		if (bFoundMapsToCook == false)
		{
			for (const FFilePath& MapToCook : PackagingSettings->MapsToCook)
			{
				UE_LOG(LogCook, Verbose, TEXT("Maps to cook list contains %s"), *MapToCook.FilePath);
				AddFileToCook(FilesInPath, Instigators, MapToCook.FilePath, EInstigator::PackagingSettingsMapToCook);
				bFoundMapsToCook = true;
			}
		}

		// If we didn't find any maps, cook the AllMaps section
		if (bFoundMapsToCook == false)
		{
			UE_LOG(LogCook, Verbose, TEXT("Loading default map ini section AllMaps"));
			TArray<FString> AllMapsSection;
			GEditor->LoadMapListFromIni(TEXT("AllMaps"), AllMapsSection);
			for (const FString& MapName : AllMapsSection)
			{
				UE_LOG(LogCook, Verbose, TEXT("Maplist contains %s"), *MapName);
				AddFileToCook(FilesInPath, Instigators, MapName, EInstigator::IniAllMaps);
			}
		}

		// Also append any cookdirs from the project ini files; these dirs are relative to the game content directory or start with a / root
		{
			for (const FDirectoryPath& DirToCook : PackagingSettings->DirectoriesToAlwaysCook)
			{
				FString LocalPath;
				if (FPackageName::TryConvertGameRelativePackagePathToLocalPath(DirToCook.Path, LocalPath))
				{
					UE_LOG(LogCook, Verbose, TEXT("Loading directory to always cook %s"), *DirToCook.Path);
					FName LocalPathFName(*LocalPath);
					CookDirectories.Add(FNameWithInstigator{ FInstigator(EInstigator::DirectoryToAlwaysCook, LocalPathFName), LocalPathFName });
				}
				else
				{
					UE_LOG(LogCook, Warning, TEXT("'ProjectSettings -> Directories to never cook -> Directories to always cook' has invalid element '%s'"), *DirToCook.Path);
				}
			}
		}
	}

	TSet<FName> ScratchNewFiles;
	TArray<FName> ScratchRemoveFiles;
	auto UpdateInstigators = [&FilesInPath, &Instigators, &ScratchNewFiles, &ScratchRemoveFiles](const FInstigator& InInstigator)
	{
		ScratchNewFiles.Reset();
		ScratchNewFiles.Reserve(FilesInPath.Num());
		for (FName FileInPath : FilesInPath)
		{
			ScratchNewFiles.Add(FileInPath);
			FInstigator& Existing = Instigators.FindOrAdd(FileInPath);
			if (Existing.Category == EInstigator::InvalidCategory)
			{
				Existing = InInstigator;
			}
		}
		ScratchRemoveFiles.Reset();
		for (const TPair<FName, FInstigator>& Pair : Instigators)
		{
			if (!ScratchNewFiles.Contains(Pair.Key))
			{
				ScratchRemoveFiles.Add(Pair.Key);
			}
		}
		for (FName RemoveFile : ScratchRemoveFiles)
		{
			Instigators.Remove(RemoveFile);
		}
	};

	if (!(FilesToCookFlags & ECookByTheBookOptions::NoGameAlwaysCookPackages))
	{
		UE_SCOPED_HIERARCHICAL_COOKTIMER_AND_DURATION(CookModificationDelegate, DetailedCookStats::GameCookModificationDelegateTimeSec);

		TArray<FString> FilesInPathStrings;
		PRAGMA_DISABLE_DEPRECATION_WARNINGS;
		FGameDelegates::Get().GetCookModificationDelegate().ExecuteIfBound(FilesInPathStrings);
		PRAGMA_ENABLE_DEPRECATION_WARNINGS;
		for (const FString& FileString : FilesInPathStrings)
		{
			AddFileToCook(FilesInPath, Instigators, FileString, EInstigator::CookModificationDelegate);
		}

		FModifyCookDelegate& ModifyCookDelegate = FGameDelegates::Get().GetModifyCookDelegate();
		if (UAssetManager::IsValid() || ModifyCookDelegate.IsBound())
		{
			TArray<FName> PackagesToNeverCook;

			if (UAssetManager::IsValid())
			{
				// allow the AssetManager to fill out the asset registry, as well as get a list of objects to always cook
				UAssetManager::Get().ModifyCook(TargetPlatforms, FilesInPath, PackagesToNeverCook);
				UpdateInstigators(EInstigator::AssetManagerModifyCook);
			}
			if (ModifyCookDelegate.IsBound())
			{
				// allow game or plugins to fill out the asset registry, as well as get a list of objects to always cook
				ModifyCookDelegate.Broadcast(TargetPlatforms, FilesInPath, PackagesToNeverCook);
				UpdateInstigators(EInstigator::ModifyCookDelegate);
			}

			for (FName NeverCookPackage : PackagesToNeverCook)
			{
				const FName StandardPackageFilename = PackageDatas->GetFileNameByFlexName(NeverCookPackage);

				if (!StandardPackageFilename.IsNone())
				{
					PackageTracker->NeverCookPackageList.Add(StandardPackageFilename);
				}
			}
		}
	}

	for ( const FString& CurrEntry : CookMaps )
	{
		UE_SCOPED_HIERARCHICAL_COOKTIMER(SearchForPackageOnDisk);
		if (FPackageName::IsShortPackageName(CurrEntry))
		{
			FString OutFilename;
			if (FPackageName::SearchForPackageOnDisk(CurrEntry, NULL, &OutFilename) == false)
			{
				LogCookerMessage( FString::Printf(TEXT("Unable to find package for map %s."), *CurrEntry), EMessageSeverity::Warning);
			}
			else
			{
				AddFileToCook(FilesInPath, Instigators, OutFilename, EInstigator::CommandLinePackage);
			}
		}
		else
		{
			AddFileToCook(FilesInPath, Instigators, CurrEntry, EInstigator::CommandLinePackage);
		}
	}
	if (IsCookingDLC())
	{
		TArray<FName> PackagesToNeverCook;
		UAssetManager::Get().ModifyDLCCook(CookByTheBookOptions->DlcName, TargetPlatforms, FilesInPath, PackagesToNeverCook);
		UpdateInstigators(EInstigator::AssetManagerModifyDLCCook);

		for (FName NeverCookPackage : PackagesToNeverCook)
		{
			FName StandardPackageFilename = PackageDatas->GetFileNameByFlexName(NeverCookPackage);

			if (!StandardPackageFilename.IsNone())
			{
				PackageTracker->NeverCookPackageList.Add(StandardPackageFilename);
			}
		}
	}

	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		TargetPlatform->GetExtraPackagesToCook(FilesInPath);
	}
	UpdateInstigators(EInstigator::TargetPlatformExtraPackagesToCook);

	if (!(FilesToCookFlags & ECookByTheBookOptions::SkipSoftReferences))
	{
		const FString ExternalMountPointName(TEXT("/Game/"));
		for (const FNameWithInstigator& CurrEntry : CookDirectories)
		{
			TArray<FString> Files;
			FString DirectoryName = CurrEntry.Name.ToString();
			IFileManager::Get().FindFilesRecursive(Files, *DirectoryName, *(FString(TEXT("*")) + FPackageName::GetAssetPackageExtension()), true, false);
			for (int32 Index = 0; Index < Files.Num(); Index++)
			{
				FString StdFile = Files[Index];
				FPaths::MakeStandardFilename(StdFile);
				AddFileToCook(FilesInPath, Instigators, StdFile, CurrEntry.Instigator);

				// this asset may not be in our currently mounted content directories, so try to mount a new one now
				FString LongPackageName;
				if (!FPackageName::IsValidLongPackageName(StdFile) && !FPackageName::TryConvertFilenameToLongPackageName(StdFile, LongPackageName))
				{
					FPackageName::RegisterMountPoint(ExternalMountPointName, DirectoryName);
				}
			}
		}

		// Keep the old behavior of cooking all by default until we implement good feedback in the editor about the missing setting
		constexpr bool bCookAllByDefault = true;

		// If no packages were explicitly added by command line or game callback, add all maps
		if (bCookAll || (bCookAllByDefault && FilesInPath.Num() == InitialNum))
		{
			TArray<FString> Tokens;
			Tokens.Empty(2);
			Tokens.Add(FString("*") + FPackageName::GetAssetPackageExtension());
			Tokens.Add(FString("*") + FPackageName::GetMapPackageExtension());

			uint8 PackageFilter = NORMALIZE_DefaultFlags | NORMALIZE_ExcludeEnginePackages | NORMALIZE_ExcludeLocalizedPackages;
			if (bMapsOnly)
			{
				PackageFilter |= NORMALIZE_ExcludeContentPackages;
			}

			if (bNoDev)
			{
				PackageFilter |= NORMALIZE_ExcludeDeveloperPackages;
			}

			// assume the first token is the map wildcard/pathname
			TArray<FString> Unused;
			for (int32 TokenIndex = 0; TokenIndex < Tokens.Num(); TokenIndex++)
			{
				TArray<FString> TokenFiles;
				if (!NormalizePackageNames(Unused, TokenFiles, Tokens[TokenIndex], PackageFilter))
				{
					UE_LOG(LogCook, Display, TEXT("No packages found for parameter %i: '%s'"), TokenIndex, *Tokens[TokenIndex]);
					continue;
				}

				for (int32 TokenFileIndex = 0; TokenFileIndex < TokenFiles.Num(); ++TokenFileIndex)
				{
					AddFileToCook(FilesInPath, Instigators, TokenFiles[TokenFileIndex], EInstigator::FullDepotSearch);
				}
			}
		}
		else if (FilesInPath.Num() == InitialNum)
		{
			LogCookerMessage(TEXT("No package requests specified on -run=Cook commandline or ini. ")
				TEXT("Set the flag 'Edit->Project Settings->Project/Packaging->Packaging/Advanced->Cook Everything in the Project Content Directory'. ")
				TEXT("Or launch 'UnrealEditor -run=cook -helpcookusage' to see all package request options."), EMessageSeverity::Warning);
		}
	}

	if (!(FilesToCookFlags & ECookByTheBookOptions::NoDefaultMaps))
	{
		for (const auto& GameDefaultSet : GameDefaultObjects)
		{
			if (GameDefaultSet.Key == FName(TEXT("ServerDefaultMap")) && !IsCookFlagSet(ECookInitializationFlags::IncludeServerMaps))
			{
				continue;
			}

			for (FName PackagePath : GameDefaultSet.Value)
			{
				TArray<FAssetData> Assets;
				if (!AssetRegistry->GetAssetsByPackageName(PackagePath, Assets))
				{
					const FText ErrorMessage = FText::Format(LOCTEXT("GameMapSettingsMissing", "{0} contains a path to a missing asset '{1}'. The intended asset will fail to load in a packaged build. Select the intended asset again in Project Settings to fix this issue."),
						FText::FromName(GameDefaultSet.Key), FText::FromName(PackagePath));
					LogCookerMessage(ErrorMessage.ToString(), EMessageSeverity::Error);
				}
				else if (Algo::AnyOf(Assets, [](const FAssetData& Asset) { return Asset.IsRedirector(); }))
				{
					const FText ErrorMessage = FText::Format(LOCTEXT("GameMapSettingsRedirectorDetected", "{0} contains a redirected reference '{1}'. The intended asset will fail to load in a packaged build. Select the intended asset again in Project Settings to fix this issue."),
						FText::FromName(GameDefaultSet.Key), FText::FromName(PackagePath));
					LogCookerMessage(ErrorMessage.ToString(), EMessageSeverity::Error);
				}

				AddFileToCook(FilesInPath, Instigators, PackagePath.ToString(),
					FInstigator(EInstigator::GameDefaultObject, GameDefaultSet.Key));
			}
		}
	}

	if (!(FilesToCookFlags & ECookByTheBookOptions::NoInputPackages))
	{
		// make sure we cook any extra assets for the default touch interface
		// @todo need a better approach to cooking assets which are dynamically loaded by engine code based on settings
		FConfigFile InputIni;
		FString InterfaceFile;
		FConfigCacheIni::LoadLocalIniFile(InputIni, TEXT("Input"), true);
		if (InputIni.GetString(TEXT("/Script/Engine.InputSettings"), TEXT("DefaultTouchInterface"), InterfaceFile))
		{
			if (InterfaceFile != TEXT("None") && InterfaceFile != TEXT(""))
			{
				AddFileToCook(FilesInPath, Instigators, InterfaceFile, EInstigator::InputSettingsIni);
			}
		}
	}

	{
		TArray<FString> UIContentPaths;
		if (GConfig->GetArray(TEXT("UI"), TEXT("ContentDirectories"), UIContentPaths, GEditorIni) > 0)
		{
			UE_LOG(LogCook, Warning, TEXT("The [UI]ContentDirectories is deprecated. You may use DirectoriesToAlwaysCook in your project settings instead."));
		}
	}
}

void UCookOnTheFlyServer::GetGameDefaultObjects(const TArray<ITargetPlatform*>& TargetPlatforms, TMap<FName, TArray<FName>>& OutGameDefaultObjects)
{
	// Collect all default objects from all cooked platforms engine configurations.
	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		// load the platform specific ini to get its DefaultMap
		FConfigFile PlatformEngineIni;
		FConfigCacheIni::LoadLocalIniFile(PlatformEngineIni, TEXT("Engine"), true, *TargetPlatform->IniPlatformName());

		FConfigSection* MapSettingsSection = PlatformEngineIni.Find(TEXT("/Script/EngineSettings.GameMapsSettings"));

		if (MapSettingsSection == nullptr)
		{
			continue;
		}

		auto AddDefaultObject = [&OutGameDefaultObjects, &PlatformEngineIni, MapSettingsSection](FName PropertyName)
		{
			const FConfigValue* PairString = MapSettingsSection->Find(PropertyName);
			if (PairString == nullptr)
			{
				return;
			}
			FString ObjectPath = PairString->GetValue();
			if (ObjectPath.IsEmpty())
			{
				return;
			}

			FSoftObjectPath Path(ObjectPath);
			FName PackageName = Path.GetLongPackageFName();
			if (PackageName.IsNone())
			{
				return;
			}
			OutGameDefaultObjects.FindOrAdd(PropertyName).AddUnique(PackageName);
		};

		// get the server and game default maps/modes and cook them
		AddDefaultObject(FName(TEXT("GameDefaultMap")));
		AddDefaultObject(FName(TEXT("ServerDefaultMap")));
		AddDefaultObject(FName(TEXT("GlobalDefaultGameMode")));
		AddDefaultObject(FName(TEXT("GlobalDefaultServerGameMode")));
		AddDefaultObject(FName(TEXT("GameInstanceClass")));
	}
}

bool UCookOnTheFlyServer::IsCookByTheBookRunning() const
{
	return CookByTheBookOptions && CookByTheBookOptions->bRunning;
}


void UCookOnTheFlyServer::SaveGlobalShaderMapFiles(const TArrayView<const ITargetPlatform* const>& Platforms)
{
	// we don't support this behavior
	check( !IsCookingDLC() );
	for (int32 Index = 0; Index < Platforms.Num(); Index++)
	{
		TArray<FString> Files;
		TArray<uint8> GlobalShaderMap;

		// make sure global shaders are up to date!
		FShaderRecompileData RecompileData(Platforms[Index]->PlatformName(), SP_NumPlatforms, ODSCRecompileCommand::Changed, &Files, nullptr, &GlobalShaderMap);

		check( IsInGameThread() );

		FString OutputDir = GetSandboxDirectory(RecompileData.PlatformName);

		UE_LOG(LogCook, Display, TEXT("Checking global shaders for platform %s"), *RecompileData.PlatformName);

		RecompileShadersForRemote(RecompileData, OutputDir);
	}
}

FString UCookOnTheFlyServer::GetSandboxDirectory( const FString& PlatformName ) const
{
	FString Result;
	Result = SandboxFile->GetSandboxDirectory();

	Result.ReplaceInline(TEXT("[Platform]"), *PlatformName);

	/*if ( IsCookingDLC() )
	{
		check( IsCookByTheBookRunning() );
		Result.ReplaceInline(TEXT("/Cooked/"), *FString::Printf(TEXT("/CookedDLC_%s/"), *CookByTheBookOptions->DlcName) );
	}*/
	return Result;
}

FString UCookOnTheFlyServer::ConvertToFullSandboxPath( const FString &FileName, bool bForWrite ) const
{
	check( SandboxFile );

	FString Result;
	if (bForWrite)
	{
		// Ideally this would be in the Sandbox File but it can't access the project or plugin
		if (PluginsToRemap.Num() > 0)
		{
			// Handle remapping of plugins
			for (TSharedRef<IPlugin> Plugin : PluginsToRemap)
			{
				// If these match, then this content is part of plugin that gets remapped when packaged/staged
				if (FileName.StartsWith(Plugin->GetContentDir()))
				{
					FString SearchFor;
					SearchFor /= Plugin->GetName() / TEXT("Content");
					int32 FoundAt = FileName.Find(SearchFor, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
					check(FoundAt != -1);
					// Strip off everything but <PluginName/Content/<remaing path to file>
					FString SnippedOffPath = FileName.RightChop(FoundAt);
					// Put this is in <sandbox path>/RemappedPlugins/<PluginName>/Content/<remaing path to file>
					FString RemappedPath = SandboxFile->GetSandboxDirectory();
					RemappedPath /= REMAPPED_PLUGINS;
					Result = RemappedPath / SnippedOffPath;
					return Result;
				}
			}
		}
		Result = SandboxFile->ConvertToAbsolutePathForExternalAppForWrite(*FileName);
	}
	else
	{
		Result = SandboxFile->ConvertToAbsolutePathForExternalAppForRead(*FileName);
	}

	/*if ( IsCookingDLC() )
	{
		check( IsCookByTheBookRunning() );
		Result.ReplaceInline(TEXT("/Cooked/"), *FString::Printf(TEXT("/CookedDLC_%s/"), *CookByTheBookOptions->DlcName) );
	}*/
	return Result;
}

FString UCookOnTheFlyServer::ConvertToFullSandboxPath( const FString &FileName, bool bForWrite, const FString& PlatformName ) const
{
	FString Result = ConvertToFullSandboxPath( FileName, bForWrite );
	Result.ReplaceInline(TEXT("[Platform]"), *PlatformName);
	return Result;
}

const FString UCookOnTheFlyServer::GetSandboxAssetRegistryFilename()
{
	static const FString RegistryFilename = FPaths::ProjectDir() / GetAssetRegistryFilename();

	if (IsCookingDLC())
	{
		check(IsCookByTheBookMode());
		const FString DLCRegistryFilename = FPaths::Combine(*GetBaseDirectoryForDLC(), GetAssetRegistryFilename());
		return ConvertToFullSandboxPath(*DLCRegistryFilename, true);
	}

	const FString SandboxRegistryFilename = ConvertToFullSandboxPath(*RegistryFilename, true);
	return SandboxRegistryFilename;
}

const FString UCookOnTheFlyServer::GetCookedAssetRegistryFilename(const FString& PlatformName )
{
	const FString CookedAssetRegistryFilename = GetSandboxAssetRegistryFilename().Replace(TEXT("[Platform]"), *PlatformName);
	return CookedAssetRegistryFilename;
}

void UCookOnTheFlyServer::InitShaderCodeLibrary(void)
{
    const UProjectPackagingSettings* const PackagingSettings = GetDefault<UProjectPackagingSettings>();
	bool const bCacheShaderLibraries = IsUsingShaderCodeLibrary();
    if (bCacheShaderLibraries && PackagingSettings->bShareMaterialShaderCode)
    {
		FShaderLibraryCooker::InitForCooking(PackagingSettings->bSharedMaterialNativeLibraries);
        
		bool bAllPlatformsNeedStableKeys = false;
		// support setting without Hungarian prefix for the compatibility, but allow newer one to override
		GConfig->GetBool(TEXT("DevOptions.Shaders"), TEXT("NeedsShaderStableKeys"), bAllPlatformsNeedStableKeys, GEngineIni);
		GConfig->GetBool(TEXT("DevOptions.Shaders"), TEXT("bNeedsShaderStableKeys"), bAllPlatformsNeedStableKeys, GEngineIni);

        for (const ITargetPlatform* TargetPlatform : PlatformManager->GetSessionPlatforms())
        {
			// Find out if this platform requires stable shader keys, by reading the platform setting file.
			// Stable shader keys are needed if we are going to create a PSO cache.
			bool bNeedShaderStableKeys = bAllPlatformsNeedStableKeys;
			FConfigFile PlatformIniFile;
			FConfigCacheIni::LoadLocalIniFile(PlatformIniFile, TEXT("Engine"), true, *TargetPlatform->IniPlatformName());
			PlatformIniFile.GetBool(TEXT("DevOptions.Shaders"), TEXT("NeedsShaderStableKeys"), bNeedShaderStableKeys);
			PlatformIniFile.GetBool(TEXT("DevOptions.Shaders"), TEXT("bNeedsShaderStableKeys"), bNeedShaderStableKeys);

			bool bNeedsDeterministicOrder = PackagingSettings->bDeterministicShaderCodeOrder;
			FConfigFile PlatformGameIniFile;
			FConfigCacheIni::LoadLocalIniFile(PlatformGameIniFile, TEXT("Game"), true, *TargetPlatform->IniPlatformName());
			PlatformGameIniFile.GetBool(TEXT("/Script/UnrealEd.ProjectPackagingSettings"), TEXT("bDeterministicShaderCodeOrder"), bNeedsDeterministicOrder);

            TArray<FName> ShaderFormats;
            TargetPlatform->GetAllTargetedShaderFormats(ShaderFormats);
			TArray<FShaderLibraryCooker::FShaderFormatDescriptor> ShaderFormatsWithStableKeys;
			for (FName& Format : ShaderFormats)
			{
				FShaderLibraryCooker::FShaderFormatDescriptor NewDesc;
				NewDesc.ShaderFormat = Format;
				NewDesc.bNeedsStableKeys = bNeedShaderStableKeys;
				NewDesc.bNeedsDeterministicOrder = bNeedsDeterministicOrder;
				ShaderFormatsWithStableKeys.Push(NewDesc);
			}

            if (ShaderFormats.Num() > 0)
			{
				FShaderLibraryCooker::CookShaderFormats(ShaderFormatsWithStableKeys);
			}
        }
    }
}

static FString GenerateShaderCodeLibraryName(FString const& Name, bool bIsIterateSharedBuild)
{
	FString ActualName = (!bIsIterateSharedBuild) ? Name : Name + TEXT("_SC");
	return ActualName;
}

void UCookOnTheFlyServer::OpenGlobalShaderLibrary()
{
	const UProjectPackagingSettings* const PackagingSettings = GetDefault<UProjectPackagingSettings>();
	bool const bCacheShaderLibraries = IsUsingShaderCodeLibrary();
	if (bCacheShaderLibraries && PackagingSettings->bShareMaterialShaderCode)
	{
		const TCHAR* GlobalShaderLibName = TEXT("Global");
		FString ActualName = GenerateShaderCodeLibraryName(GlobalShaderLibName, IsCookFlagSet(ECookInitializationFlags::IterateSharedBuild));

		// The shader code library directory doesn't matter while cooking
		FShaderLibraryCooker::BeginCookingLibrary(ActualName);
	}
}

void UCookOnTheFlyServer::OpenShaderLibrary(FString const& Name)
{
	const UProjectPackagingSettings* const PackagingSettings = GetDefault<UProjectPackagingSettings>();
	bool const bCacheShaderLibraries = IsUsingShaderCodeLibrary();
	if (bCacheShaderLibraries && PackagingSettings->bShareMaterialShaderCode)
	{
		FString ActualName = GenerateShaderCodeLibraryName(Name, IsCookFlagSet(ECookInitializationFlags::IterateSharedBuild));

		// The shader code library directory doesn't matter while cooking
		FShaderLibraryCooker::BeginCookingLibrary(ActualName);
	}
}

void UCookOnTheFlyServer::CreatePipelineCache(const ITargetPlatform* TargetPlatform, const FString& LibraryName)
{
	// make sure we have a registry generated for all the platforms 
	const FString TargetPlatformName = TargetPlatform->PlatformName();
	TArray<FString>* SCLCSVPaths = OutSCLCSVPaths.Find(FName(TargetPlatformName));
	if (SCLCSVPaths && SCLCSVPaths->Num())
	{
		TArray<FName> ShaderFormats;
		TargetPlatform->GetAllTargetedShaderFormats(ShaderFormats);
		for (FName ShaderFormat : ShaderFormats)
		{
			const FString StablePCDir = FPaths::ProjectDir() / TEXT("Build") / TargetPlatform->IniPlatformName() / TEXT("PipelineCaches");
			// look for the new binary format for stable pipeline cache - spc
			const FString StablePCBinary = StablePCDir / FString::Printf(TEXT("*%s_%s.spc"), *LibraryName, *ShaderFormat.ToString());

			bool bBinaryStablePipelineCacheFilesFound = [&StablePCBinary]()
			{
				TArray<FString> ExpandedFiles;
				IFileManager::Get().FindFilesRecursive(ExpandedFiles, *FPaths::GetPath(StablePCBinary), *FPaths::GetCleanFilename(StablePCBinary), true, false, false);
				return ExpandedFiles.Num() > 0;
			}();

			// for now, also look for the older *stablepc.csv or *stablepc.csv.compressed
			const FString StablePCTextual = StablePCDir / FString::Printf(TEXT("*%s_%s.stablepc.csv"), *LibraryName, *ShaderFormat.ToString());
			const FString StablePCTextualCompressed = StablePCTextual + TEXT(".compressed");

			bool bTextualStablePipelineCacheFilesFound = [&StablePCTextual, &StablePCTextualCompressed]()
			{
				TArray<FString> ExpandedFiles;
				IFileManager::Get().FindFilesRecursive(ExpandedFiles, *FPaths::GetPath(StablePCTextual), *FPaths::GetCleanFilename(StablePCTextual), true, false, false);
				IFileManager::Get().FindFilesRecursive(ExpandedFiles, *FPaths::GetPath(StablePCTextualCompressed), *FPaths::GetCleanFilename(StablePCTextualCompressed), true, false, false);
				return ExpandedFiles.Num() > 0;
			}();

			// because of the compute shaders that are cached directly from stable shader keys files, we need to run this also if we have stable keys (which is pretty much always)
			static const IConsoleVariable* CVarIncludeComputePSOsDuringCook = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ShaderPipelineCacheTools.IncludeComputePSODuringCook"));
			const bool bIncludeComputePSOsDuringCook = CVarIncludeComputePSOsDuringCook && CVarIncludeComputePSOsDuringCook->GetInt() >= 1;
			if (!bBinaryStablePipelineCacheFilesFound && !bTextualStablePipelineCacheFilesFound && !bIncludeComputePSOsDuringCook)
			{
				UE_LOG(LogCook, Display, TEXT("---- NOT Running UShaderPipelineCacheToolsCommandlet for platform %s  shader format %s, no files found at %s, and either no stable keys or not including compute PSOs during the cook"), *TargetPlatformName, *ShaderFormat.ToString(), *StablePCDir);
			}
			else
			{
				UE_LOG(LogCook, Display, TEXT("---- Running UShaderPipelineCacheToolsCommandlet for platform %s  shader format %s"), *TargetPlatformName, *ShaderFormat.ToString());

				const FString OutFilename = FString::Printf(TEXT("%s_%s.stable.upipelinecache"), *LibraryName, *ShaderFormat.ToString());
				const FString PCUncookedPath = FPaths::ProjectDir() / TEXT("Content") / TEXT("PipelineCaches") / TargetPlatform->IniPlatformName() / OutFilename;

				if (IFileManager::Get().FileExists(*PCUncookedPath))
				{
					UE_LOG(LogCook, Warning, TEXT("Deleting %s, cooked data doesn't belong here."), *PCUncookedPath);
					IFileManager::Get().Delete(*PCUncookedPath, false, true);
				}

				const FString PCCookedPath = ConvertToFullSandboxPath(*PCUncookedPath, true);
				const FString PCPath = PCCookedPath.Replace(TEXT("[Platform]"), *TargetPlatformName);


				FString Args(TEXT("build "));
				if (bBinaryStablePipelineCacheFilesFound)
				{
					Args += TEXT("\"");
					Args += StablePCBinary;
					Args += TEXT("\" ");
				}
				if (bTextualStablePipelineCacheFilesFound)
				{
					Args += TEXT("\"");
					Args += StablePCTextual;
					Args += TEXT("\" ");
				}

				int32 NumMatched = 0;
				for (int32 Index = 0; Index < SCLCSVPaths->Num(); Index++)
				{
					if (!(*SCLCSVPaths)[Index].Contains(ShaderFormat.ToString()))
					{
						continue;
					}
					NumMatched++;
					Args += TEXT(" ");
					Args += TEXT("\"");
					Args += (*SCLCSVPaths)[Index];
					Args += TEXT("\"");
				}
				if (!NumMatched)
				{
					UE_LOG(LogCook, Warning, TEXT("Shader format %s for platform %s had stable pipeline cache files, but no stable keys files."), *ShaderFormat.ToString(), *TargetPlatformName);
					for (int32 Index = 0; Index < SCLCSVPaths->Num(); Index++)
					{
						UE_LOG(LogCook, Warning, TEXT("    stable keys file: %s"), *((*SCLCSVPaths)[Index]));
					}							
					continue;
				}

				Args += TEXT(" ");
				Args += TEXT("\"");
				Args += PCPath;
				Args += TEXT("\"");
				UE_LOG(LogCook, Display, TEXT("  With Args: %s"), *Args);

				int32 Result = UShaderPipelineCacheToolsCommandlet::StaticMain(Args);

				if (Result)
				{
					LogCookerMessage(FString::Printf(TEXT("UShaderPipelineCacheToolsCommandlet failed %d"), Result), EMessageSeverity::Error);
				}
				else
				{
					UE_LOG(LogCook, Display, TEXT("---- Done running UShaderPipelineCacheToolsCommandlet for platform %s"), *TargetPlatformName);

					// copy the resulting file to metadata for easier examination later
					if (IFileManager::Get().FileExists(*PCPath))
					{
						const FString RootPipelineCacheMetadataPath = FPaths::ProjectDir() / TEXT("Metadata") / TEXT("PipelineCaches");
						const FString PipelineCacheMetadataPathSB = ConvertToFullSandboxPath(*RootPipelineCacheMetadataPath, true);
						const FString PipelineCacheMetadataPath = PipelineCacheMetadataPathSB.Replace(TEXT("[Platform]"), *TargetPlatform->PlatformName());
						const FString PipelineCacheMetadataFileName = PipelineCacheMetadataPath / OutFilename;

						UE_LOG(LogCook, Display, TEXT("Copying the binary PSO cache file %s to %s."), *PCPath, *PipelineCacheMetadataFileName);
						if (IFileManager::Get().Copy(*PipelineCacheMetadataFileName, *PCPath) != COPY_OK)
						{
							UE_LOG(LogCook, Warning, TEXT("Failed to copy the binary PSO cache file %s to %s."), *PCPath, *PipelineCacheMetadataFileName);
						}
					}
				}
			}
		}
	}
}

void UCookOnTheFlyServer::SaveAndCloseGlobalShaderLibrary()
{
	const TCHAR* GlobalShaderLibName = TEXT("Global");
	FString ActualName = GenerateShaderCodeLibraryName(GlobalShaderLibName, IsCookFlagSet(ECookInitializationFlags::IterateSharedBuild));

	const UProjectPackagingSettings* const PackagingSettings = GetDefault<UProjectPackagingSettings>();
	bool const bCacheShaderLibraries = IsUsingShaderCodeLibrary();
	if (bCacheShaderLibraries && PackagingSettings->bShareMaterialShaderCode)
	{
		// Save shader code map - cleaning directories is deliberately a separate loop here as we open the cache once per shader platform and we don't assume that they can't be shared across target platforms.
		for (const ITargetPlatform* TargetPlatform : PlatformManager->GetSessionPlatforms())
		{
			SaveShaderLibrary(TargetPlatform, GlobalShaderLibName);
		}

		FShaderLibraryCooker::EndCookingLibrary(ActualName);
	}
}

void UCookOnTheFlyServer::SaveShaderLibrary(const ITargetPlatform* TargetPlatform, FString const& Name)
{
	TArray<FName> ShaderFormats;
	TargetPlatform->GetAllTargetedShaderFormats(ShaderFormats);
	if (ShaderFormats.Num() > 0)
	{
		FString ActualName = GenerateShaderCodeLibraryName(Name, IsCookFlagSet(ECookInitializationFlags::IterateSharedBuild));
		FString BasePath = !IsCookingDLC() ? FPaths::ProjectContentDir() : GetContentDirectoryForDLC();

		FString ShaderCodeDir = ConvertToFullSandboxPath(*BasePath, true, TargetPlatform->PlatformName());

		const FString RootMetaDataPath = FPaths::ProjectDir() / TEXT("Metadata") / TEXT("PipelineCaches");
		const FString MetaDataPathSB = ConvertToFullSandboxPath(*RootMetaDataPath, true);
		const FString MetaDataPath = MetaDataPathSB.Replace(TEXT("[Platform]"), *TargetPlatform->PlatformName());

		TArray<FString>& PlatformSCLCSVPaths = OutSCLCSVPaths.FindOrAdd(FName(TargetPlatform->PlatformName()));
		const UProjectPackagingSettings* const PackagingSettings = GetDefault<UProjectPackagingSettings>();
		FString ErrorString;
		if (!FShaderLibraryCooker::SaveShaderLibraryWithoutChunking(TargetPlatform, Name, ShaderCodeDir, MetaDataPath, PlatformSCLCSVPaths, ErrorString))
		{
			// This is fatal - In this case we should cancel any launch on device operation or package write but we don't want to assert and crash the editor
			LogCookerMessage(FString::Printf(TEXT("%s"), *ErrorString), EMessageSeverity::Error);
		}
		else
		{
			for (const FString& Item : PlatformSCLCSVPaths)
			{
				UE_LOG(LogCook, Display, TEXT("Saved scl.csv %s for platform %s, %d bytes"), *Item, *TargetPlatform->PlatformName(),
					IFileManager::Get().FileSize(*Item));
			}
		}
	}
}

void UCookOnTheFlyServer::CleanShaderCodeLibraries()
{
	const UProjectPackagingSettings* const PackagingSettings = GetDefault<UProjectPackagingSettings>();
    bool const bCacheShaderLibraries = IsUsingShaderCodeLibrary();
	ITargetPlatformManagerModule& TPM = GetTargetPlatformManagerRef();
	bool bIterativeCook = IsCookFlagSet(ECookInitializationFlags::Iterative) ||	PackageDatas->GetNumCooked() != 0;

	// If not iterative then clean up our temporary files
	if (bCacheShaderLibraries && PackagingSettings->bShareMaterialShaderCode && !bIterativeCook)
	{
		for (const ITargetPlatform* TargetPlatform : PlatformManager->GetSessionPlatforms())
		{
			TArray<FName> ShaderFormats;
			TargetPlatform->GetAllTargetedShaderFormats(ShaderFormats);
			if (ShaderFormats.Num() > 0)
			{
				FShaderLibraryCooker::CleanDirectories(ShaderFormats);
			}
		}
	}
}

void UCookOnTheFlyServer::CookByTheBookFinished()
{
	using namespace UE::Cook;

	check( IsInGameThread() );
	check( IsCookByTheBookMode() );
	check( CookByTheBookOptions->bRunning == true );
	check(PackageDatas->GetRequestQueue().IsEmpty());
	check(PackageDatas->GetLoadPrepareQueue().IsEmpty());
	check(PackageDatas->GetLoadReadyQueue().IsEmpty());
	check(PackageDatas->GetSaveQueue().IsEmpty());

	UE_LOG(LogCook, Display, TEXT("Finishing up..."));

	{
		UE_SCOPED_COOKTIMER(TickCookableObjects);
		float CurrentTime = FPlatformTime::Seconds();
		FTickableCookObject::TickObjects(CurrentTime - LastCookableObjectTickTime, true /* bTickComplete */);
		LastCookableObjectTickTime = CurrentTime;
	}

	UPackage::WaitForAsyncFileWrites();
	BuildDefinitions->Wait();
	
	GetDerivedDataCacheRef().WaitForQuiescence(true);
	
	UCookerSettings const* CookerSettings = GetDefault<UCookerSettings>();

	const UProjectPackagingSettings* const PackagingSettings = GetDefault<UProjectPackagingSettings>();
	bool const bCacheShaderLibraries = IsUsingShaderCodeLibrary();
	FString LibraryName = !IsCookingDLC() ? FApp::GetProjectName() : CookByTheBookOptions->DlcName;

	{
		// Save modified asset registry with all streaming chunk info generated during cook
		const FString& SandboxRegistryFilename = GetSandboxAssetRegistryFilename();

		// previously shader library was saved at this spot, but it's too early to know the chunk assignments, we need to BuildChunkManifest in the asset registry first

		{
			UE_SCOPED_HIERARCHICAL_COOKTIMER(SavingCurrentIniSettings)
			for (const ITargetPlatform* TargetPlatform : PlatformManager->GetSessionPlatforms() )
			{
				SaveCurrentIniSettings(TargetPlatform);
			}
		}

		if (!FParse::Param(FCommandLine::Get(), TEXT("SkipSaveAssetRegistry")))
		{
			UE_SCOPED_HIERARCHICAL_COOKTIMER(SavingAssetRegistry);
			for (const ITargetPlatform* TargetPlatform : PlatformManager->GetSessionPlatforms())
			{
				FPlatformData* PlatformData = PlatformManager->GetPlatformData(TargetPlatform);
				FAssetRegistryGenerator& Generator = *PlatformData->RegistryGenerator;
				TArray<FPackageData*> CookedPackageDatas;
				TArray<FPackageData*> IgnorePackageDatas;

				const FName& PlatformName = FName(*TargetPlatform->PlatformName());
				FString PlatformNameString = PlatformName.ToString();

				PackageDatas->GetCookedPackagesForPlatform(TargetPlatform, CookedPackageDatas, false, /* include successful */ true);

				// ignore any packages which failed to cook
				PackageDatas->GetCookedPackagesForPlatform(TargetPlatform, IgnorePackageDatas, /* include failed */ true, false);

				bool bForceNoFilterAssetsFromAssetRegistry = false;

				if (IsCookingDLC())
				{
					TMap<FName, FPackageData*> CookedPackagesMap;
					CookedPackagesMap.Reserve(CookedPackageDatas.Num());
					for (FPackageData* PackageData : CookedPackageDatas)
					{
						CookedPackagesMap.Add(PackageData->GetFileName(), PackageData);
					}
					bForceNoFilterAssetsFromAssetRegistry = true;
					// remove the previous release cooked packages from the new asset registry, add to ignore list
					UE_SCOPED_HIERARCHICAL_COOKTIMER(RemovingOldManifestEntries);

					const TArray<FName>* PreviousReleaseCookedPackages = CookByTheBookOptions->BasedOnReleaseCookedPackages.Find(PlatformName);
					if (PreviousReleaseCookedPackages)
					{
						for (FName PreviousReleaseCookedPackage : *PreviousReleaseCookedPackages)
						{
							FPackageData* PackageData;
							if (!CookedPackagesMap.RemoveAndCopyValue(PreviousReleaseCookedPackage, PackageData))
							{
								PackageData = PackageDatas->FindPackageDataByFileName(PreviousReleaseCookedPackage);
							}
							if (PackageData)
							{
								IgnorePackageDatas.Add(PackageData);
							}
						}
					}
					CookedPackageDatas.Reset();
					for (TPair<FName, FPackageData*>& Pair : CookedPackagesMap)
					{
						CookedPackageDatas.Add(Pair.Value);
					}
				}

				TSet<FName> CookedPackageNames;
				for (FPackageData* PackageData : CookedPackageDatas)
				{
					CookedPackageNames.Add(PackageData->GetPackageName());
				}

				TSet<FName> IgnorePackageNames;
				for (FPackageData* PackageData : IgnorePackageDatas)
				{
					IgnorePackageNames.Add(PackageData->GetPackageName());
				}

				// ignore packages that weren't cooked because they were only referenced by editor-only properties
				TSet<FName> UncookedEditorOnlyPackageNames;
				PackageTracker->UncookedEditorOnlyPackages.GetValues(UncookedEditorOnlyPackageNames);
				for (FName UncookedEditorOnlyPackage : UncookedEditorOnlyPackageNames)
				{
					IgnorePackageNames.Add(UncookedEditorOnlyPackage);
				}
				{
					Generator.PreSave(CookedPackageNames);
				}
				{
					UE_SCOPED_HIERARCHICAL_COOKTIMER(BuildChunkManifest);
					Generator.FinalizeChunkIDs(CookedPackageNames, IgnorePackageNames, *SandboxFile,
						CookByTheBookOptions->bGenerateStreamingInstallManifests);
				}
				{
					UE_SCOPED_HIERARCHICAL_COOKTIMER(SaveManifests);
					if (!Generator.SaveManifests(*SandboxFile))
					{
						UE_LOG(LogCook, Warning, TEXT("Failed to save chunk manifest"));
					}

					int64 ExtraFlavorChunkSize;
					if (FParse::Value(FCommandLine::Get(), TEXT("ExtraFlavorChunkSize="), ExtraFlavorChunkSize) && ExtraFlavorChunkSize > 0)
					{
						// ExtraFlavor is a legacy term for this override; etymology unknown. Override the chunksize specified by the platform,
						// and write the manifest files created with that chunksize into a separate subdirectory.
						const TCHAR* ManifestSubDir = TEXT("ExtraFlavor");
						if (!Generator.SaveManifests(*SandboxFile, ExtraFlavorChunkSize, ManifestSubDir))
						{
							UE_LOG(LogCook, Warning, TEXT("Failed to save chunk manifest"));
						}
					}
				}
				{
					UE_SCOPED_HIERARCHICAL_COOKTIMER(SaveRealAssetRegistry);
					Generator.SaveAssetRegistry(SandboxRegistryFilename, true, bForceNoFilterAssetsFromAssetRegistry);
				}
				{
					Generator.PostSave();
				}
				{
					UE_SCOPED_HIERARCHICAL_COOKTIMER(WriteCookerOpenOrder);
					if (!IsCookFlagSet(ECookInitializationFlags::Iterative))
					{
						Generator.WriteCookerOpenOrder(*SandboxFile);
					}
				}
				// now that we have the asset registry and cooking open order, we have enough information to split the shader library
				// into parts for each chunk and (possibly) lay out the code in accordance with the file order
				if (bCacheShaderLibraries && PackagingSettings->bShareMaterialShaderCode)
				{
					// Save shader code map
					if (LibraryName.Len() > 0)
					{
						SaveShaderLibrary(TargetPlatform, LibraryName);

						CreatePipelineCache(TargetPlatform, LibraryName);
					}
				}
				{
					if (FParse::Param(FCommandLine::Get(), TEXT("fastcook")))
					{
						FFileHelper::SaveStringToFile(FString(), *(GetSandboxDirectory(PlatformNameString) / TEXT("fastcook.txt")));
					}
				}
				if (IsCreatingReleaseVersion())
				{
					const FString VersionedRegistryPath = GetCreateReleaseVersionAssetRegistryPath(CookByTheBookOptions->CreateReleaseVersion, PlatformNameString);
					IFileManager::Get().MakeDirectory(*VersionedRegistryPath, true);
					const FString VersionedRegistryFilename = VersionedRegistryPath / GetAssetRegistryFilename();
					const FString CookedAssetRegistryFilename = SandboxRegistryFilename.Replace(TEXT("[Platform]"), *PlatformNameString);
					IFileManager::Get().Copy(*VersionedRegistryFilename, *CookedAssetRegistryFilename, true, true);

					// Also copy development registry if it exists
					FString DevelopmentAssetRegistryRelativePath = FString::Printf(TEXT("Metadata/%s"), GetDevelopmentAssetRegistryFilename());
					const FString DevVersionedRegistryFilename = VersionedRegistryFilename.Replace(TEXT("AssetRegistry.bin"), *DevelopmentAssetRegistryRelativePath);
					const FString DevCookedAssetRegistryFilename = CookedAssetRegistryFilename.Replace(TEXT("AssetRegistry.bin"), *DevelopmentAssetRegistryRelativePath);
					IFileManager::Get().Copy(*DevVersionedRegistryFilename, *DevCookedAssetRegistryFilename, true, true);
				}
			}
		}
	}

	FString ActualLibraryName = GenerateShaderCodeLibraryName(LibraryName, IsCookFlagSet(ECookInitializationFlags::IterateSharedBuild));
	FShaderLibraryCooker::EndCookingLibrary(ActualLibraryName);
	FShaderLibraryCooker::Shutdown();

	if (CookByTheBookOptions->bGenerateDependenciesForMaps)
	{
		UE_SCOPED_HIERARCHICAL_COOKTIMER(GenerateMapDependencies);
		for (auto& MapDependencyGraphIt : CookByTheBookOptions->MapDependencyGraphs)
		{
			BuildMapDependencyGraph(MapDependencyGraphIt.Key);
			WriteMapDependencyGraph(MapDependencyGraphIt.Key);
		}
	}

	FinalizePackageStore();

	CookByTheBookOptions->BasedOnReleaseCookedPackages.Empty();
	CookByTheBookOptions->bRunning = false;
	CookByTheBookOptions->bFullLoadAndSave = false;

	if (!IsCookingInEditor())
	{
		FCoreUObjectDelegates::PackageCreatedForLoad.RemoveAll(this);
	}
	PlatformManager->ClearSessionPlatforms();

	PrintFinishStats();

	OutputHierarchyTimers();
	ClearHierarchyTimers();

	UE_LOG(LogCook, Display, TEXT("Done!"));
}

void UCookOnTheFlyServer::PrintFinishStats()
{
	const float TotalCookTime = (float)(FPlatformTime::Seconds() - CookByTheBookOptions->CookStartTime);
	UE_LOG(LogCook, Display, TEXT("Cook by the book total time in tick %fs total time %f"), CookByTheBookOptions->CookTime, TotalCookTime);

	const FPlatformMemoryStats MemStats = FPlatformMemory::GetStats();
	UE_LOG(LogCook, Display, TEXT("Peak Used virtual %u MiB Peak Used physical %u MiB"), MemStats.PeakUsedVirtual / 1024 / 1024, MemStats.PeakUsedPhysical / 1024 / 1024);

	COOK_STAT(UE_LOG(LogCook, Display, TEXT("Packages Cooked: %d, Packages Iteratively Skipped: %d, Total Packages: %d"),
		UE::SavePackageUtilities::GetNumPackagesSaved(), DetailedCookStats::NumPackagesIterativelySkipped, PackageDatas->GetNumCooked()));
}

void UCookOnTheFlyServer::BuildMapDependencyGraph(const ITargetPlatform* TargetPlatform)
{
	auto& MapDependencyGraph = CookByTheBookOptions->MapDependencyGraphs.FindChecked(TargetPlatform);

	TArray<UE::Cook::FPackageData*> PlatformCookedPackages;
	PackageDatas->GetCookedPackagesForPlatform(TargetPlatform, PlatformCookedPackages, /* include failed */ true, /* include successful */ true);

	// assign chunks for all the map packages
	for (const UE::Cook::FPackageData* const CookedPackage : PlatformCookedPackages)
	{
		TArray<FAssetData> PackageAssets;
		FName Name = CookedPackage->GetPackageName();

		if (!ContainsMap(Name))
		{
			continue;
		}

		TSet<FName> DependentPackages;
		TSet<FName> Roots; 

		Roots.Add(Name);

		GetDependentPackages(Roots, DependentPackages);

		MapDependencyGraph.Add(Name, DependentPackages);
	}
}

void UCookOnTheFlyServer::WriteMapDependencyGraph(const ITargetPlatform* TargetPlatform)
{
	auto& MapDependencyGraph = CookByTheBookOptions->MapDependencyGraphs.FindChecked(TargetPlatform);

	FString MapDependencyGraphFile = FPaths::ProjectDir() / TEXT("MapDependencyGraph.json");
	// dump dependency graph. 
	FString DependencyString;
	DependencyString += "{";
	for (auto& Ele : MapDependencyGraph)
	{
		TSet<FName>& Deps = Ele.Value;
		FName MapName = Ele.Key;
		DependencyString += TEXT("\t\"") + MapName.ToString() + TEXT("\" : \n\t[\n ");
		for (FName& Val : Deps)
		{
			DependencyString += TEXT("\t\t\"") + Val.ToString() + TEXT("\",\n");
		}
		DependencyString.RemoveFromEnd(TEXT(",\n"));
		DependencyString += TEXT("\n\t],\n");
	}
	DependencyString.RemoveFromEnd(TEXT(",\n"));
	DependencyString += "\n}";

	FString CookedMapDependencyGraphFilePlatform = ConvertToFullSandboxPath(MapDependencyGraphFile, true).Replace(TEXT("[Platform]"), *TargetPlatform->PlatformName());
	FFileHelper::SaveStringToFile(DependencyString, *CookedMapDependencyGraphFilePlatform, FFileHelper::EEncodingOptions::ForceUnicode);
}

void UCookOnTheFlyServer::QueueCancelCookByTheBook()
{
	if ( IsCookByTheBookMode() )
	{
		check( CookByTheBookOptions != NULL );
		CookByTheBookOptions->bCancel = true;
	}
}

void UCookOnTheFlyServer::CancelCookByTheBook()
{
	if ( IsCookByTheBookMode() && CookByTheBookOptions->bRunning )
	{
		check(CookByTheBookOptions);
		check( IsInGameThread() );

		CancelAllQueues();

		CookByTheBookOptions->bRunning = false;
		SandboxFile = nullptr;

		PrintFinishStats();
	} 
}

void UCookOnTheFlyServer::StopAndClearCookedData()
{
	if ( IsCookByTheBookMode() )
	{
		check( CookByTheBookOptions != NULL );
		CancelCookByTheBook();
	}
	else
	{
		CancelAllQueues();
	}

	PackageTracker->RecompileRequests.Empty();
	PackageTracker->UnsolicitedCookedPackages.Empty();
	PackageDatas->ClearCookedPlatforms();
}

void UCookOnTheFlyServer::ClearAllCookedData()
{
	// if we are going to clear the cooked packages it is conceivable that we will recook the packages which we just cooked 
	// that means it's also conceivable that we will recook the same package which currently has an outstanding async write request
	UPackage::WaitForAsyncFileWrites();

	PackageTracker->UnsolicitedCookedPackages.Empty();
	PackageDatas->ClearCookedPlatforms();
}

void UCookOnTheFlyServer::CancelAllQueues()
{
	// Discard the external build requests, but execute any pending SchedulerCallbacks since these might have important teardowns
	TArray<UE::Cook::FSchedulerCallback> SchedulerCallbacks;
	TArray<UE::Cook::FFilePlatformRequest> UnusedRequests;
	ExternalRequests->DequeueAll(SchedulerCallbacks, UnusedRequests);
	for (UE::Cook::FSchedulerCallback& SchedulerCallback : SchedulerCallbacks)
	{
		SchedulerCallback();
	}

	using namespace UE::Cook;
	// Remove all elements from all Queues and send them to Idle
	FPackageDataQueue& SaveQueue = PackageDatas->GetSaveQueue();
	while (!SaveQueue.IsEmpty())
	{
		SaveQueue.PopFrontValue()->SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
	}
	FPackageDataQueue& LoadReadyQueue = PackageDatas->GetLoadReadyQueue();
	while (!LoadReadyQueue.IsEmpty())
	{
		LoadReadyQueue.PopFrontValue()->SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
	}
	FLoadPrepareQueue& LoadPrepareQueue = PackageDatas->GetLoadPrepareQueue();
	while (!LoadPrepareQueue.IsEmpty())
	{
		LoadPrepareQueue.PopFront()->SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
	}
	FRequestQueue& RequestQueue = PackageDatas->GetRequestQueue();
	FPackageDataSet& UnclusteredRequests = RequestQueue.GetUnclusteredRequests();
	for (FPackageData* PackageData : UnclusteredRequests)
	{
		PackageData->SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
	}
	UnclusteredRequests.Empty();
	TRingBuffer<FRequestCluster>& RequestClusters = RequestQueue.GetRequestClusters();
	for (FRequestCluster& RequestCluster : RequestClusters)
	{
		TArray<FPackageData*> RequestsToLoad;
		TArray<FPackageData*> RequestsToDemote;
		RequestCluster.ClearAndDetachOwnedPackageDatas(RequestsToLoad, RequestsToDemote);
		for (FPackageData* PackageData : RequestsToLoad)
		{
			PackageData->SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
		}
		for (FPackageData* PackageData : RequestsToDemote)
		{
			PackageData->SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
		}
	}
	RequestClusters.Empty();

	while (!RequestQueue.IsReadyRequestsEmpty())
	{
		RequestQueue.PopReadyRequest()->SendToState(EPackageState::Idle, ESendFlags::QueueAdd);
	}

	SetLoadBusy(false);
	SetSaveBusy(false);
}


void UCookOnTheFlyServer::ClearPlatformCookedData(const ITargetPlatform* TargetPlatform)
{
	if (!TargetPlatform)
	{
		return;
	}
	ResetCook({ TPair<const ITargetPlatform*,bool>{TargetPlatform, true /* bResetResults */}});

	FindOrCreatePackageWriter(TargetPlatform).RemoveCookedPackages();
}

void UCookOnTheFlyServer::ResetCook(TConstArrayView<TPair<const ITargetPlatform*, bool>> TargetPlatforms)
{
	for (UE::Cook::FPackageData* PackageData : *PackageDatas.Get())
	{
		for (const TPair<const ITargetPlatform*, bool>& Pair : TargetPlatforms)
		{
			const ITargetPlatform* TargetPlatform = Pair.Key;
			UE::Cook::FPackageData::FPlatformData* PlatformData = PackageData->FindPlatformData(TargetPlatform);
			if (PlatformData)
			{
				bool bResetResults = Pair.Value;
				PlatformData->bExplored = false;
				if (bResetResults)
				{
					PlatformData->bCookAttempted = false;
					PlatformData->bCookSucceeded = false;
				}
			}
		}
	}

	TArray<FName> PackageNames;
	for (const TPair<const ITargetPlatform*, bool>& Pair : TargetPlatforms)
	{
		const ITargetPlatform* TargetPlatform = Pair.Key;
		bool bResetResults = Pair.Value;
		if (bResetResults)
		{
			PackageNames.Reset();
			PackageTracker->UnsolicitedCookedPackages.GetPackagesForPlatformAndRemove(TargetPlatform, PackageNames);
		}
	}
}

void UCookOnTheFlyServer::ClearPlatformCookedData(const FString& PlatformName)
{
	ClearPlatformCookedData(GetTargetPlatformManagerRef().FindTargetPlatform(PlatformName));
}

void UCookOnTheFlyServer::ClearCachedCookedPlatformDataForPlatform(const ITargetPlatform* TargetPlatform)
{
	if (TargetPlatform)
	{
		for (TObjectIterator<UObject> It; It; ++It)
		{
			It->ClearCachedCookedPlatformData(TargetPlatform);
		}
	}
}

void UCookOnTheFlyServer::ClearCachedCookedPlatformDataForPlatform(const FName& PlatformName)
{
	ITargetPlatformManagerModule& TPM = GetTargetPlatformManagerRef();
	const ITargetPlatform* TargetPlatform = TPM.FindTargetPlatform(PlatformName.ToString());
	return ClearCachedCookedPlatformDataForPlatform(TargetPlatform);
}

void UCookOnTheFlyServer::OnTargetPlatformChangedSupportedFormats(const ITargetPlatform* TargetPlatform)
{
	for (TObjectIterator<UObject> It; It; ++It)
	{
		It->ClearCachedCookedPlatformData(TargetPlatform);
	}
}

void UCookOnTheFlyServer::CreateSandboxFile()
{
	// initialize the sandbox file after determining if we are cooking dlc
	// Local sandbox file wrapper. This will be used to handle path conversions,
	// but will not be used to actually write/read files so we can safely
	// use [Platform] token in the sandbox directory name and then replace it
	// with the actual platform name.
	check(SandboxFile == nullptr);
	SandboxFile = FSandboxPlatformFile::Create(false);

	// Output directory override.	
	FString OutputDirectory = GetOutputDirectoryOverride();

	// Use SandboxFile to do path conversion to properly handle sandbox paths (outside of standard paths in particular).
	SandboxFile->Initialize(&FPlatformFileManager::Get().GetPlatformFile(), *FString::Printf(TEXT("-sandbox=\"%s\""), *OutputDirectory));
}

void UCookOnTheFlyServer::SetBeginCookConfigSettings()
{
	GConfig->GetBool(TEXT("CookSettings"), TEXT("HybridIterativeEnabled"), bHybridIterativeEnabled, GEditorIni);
	// TODO: HybridIterative is not yet implemented for DLC
	bHybridIterativeEnabled &= !IsCookingDLC();
	// HybridIterative uses TargetDomain storage of dependencies which is only implemented in ZenStore
	bHybridIterativeEnabled &= IsUsingZenStore();
}

void UCookOnTheFlyServer::BeginCookSandbox(TConstArrayView<const ITargetPlatform*> TargetPlatforms)
{
#if OUTPUT_COOKTIMING
	double CleanSandboxTime = 0.0;
#endif
	{
		UE_SCOPED_HIERARCHICAL_COOKTIMER_AND_DURATION(CleanSandbox, CleanSandboxTime);

		if (SandboxFile == nullptr)
		{
			CreateSandboxFile();
		}

		const bool bIsDiffOnly = FParse::Param(FCommandLine::Get(), TEXT("DIFFONLY"));
		const bool bIterative = !FParse::Param(FCommandLine::Get(), TEXT("fullcook")) && (bHybridIterativeEnabled || IsCookFlagSet(ECookInitializationFlags::Iterative));
		const bool bIsSharedIterativeCook = IsCookFlagSet(ECookInitializationFlags::IterateSharedBuild);
		TArray<TPair<const ITargetPlatform*,bool>, TInlineAllocator<ExpectedMaxNumPlatforms>> ResetPlatforms;
		TArray<const ITargetPlatform*, TInlineAllocator<ExpectedMaxNumPlatforms>> PopulatePlatforms;
		TArray<const ITargetPlatform*, TInlineAllocator<ExpectedMaxNumPlatforms>> AlreadyCookedPlatforms;

		for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
		{
			UE::Cook::FPlatformData* PlatformData = PlatformManager->GetPlatformData(TargetPlatform);
			ICookedPackageWriter& PackageWriter = FindOrCreatePackageWriter(TargetPlatform);
			bool bIterateSharedBuild = false;
			if (bIterative && bIsSharedIterativeCook && !PlatformData->bIsSandboxInitialized)
			{
				// see if the shared build is newer then the current cooked content in the local directory
				FString SharedCookedAssetRegistry = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("SharedIterativeBuild"),
					*TargetPlatform->PlatformName(), TEXT("Metadata"), GetDevelopmentAssetRegistryFilename());
				
				FDateTime PreviousLocalCookedBuild = PackageWriter.GetPreviousCookTime();
				FDateTime PreviousSharedCookedBuild = IFileManager::Get().GetTimeStamp(*SharedCookedAssetRegistry);
				if (PreviousSharedCookedBuild != FDateTime::MinValue() &&
					PreviousSharedCookedBuild >= PreviousLocalCookedBuild)
				{
					// copy the ini settings from the shared cooked build. 
					const FString SharedCookedIniFile = FPaths::Combine(*FPaths::ProjectSavedDir(), TEXT("SharedIterativeBuild"),
						*TargetPlatform->PlatformName(), TEXT("Metadata"), TEXT("CookedIniVersion.txt"));
					FString SandboxCookedIniFile = FPaths::ProjectDir() / TEXT("Metadata") / TEXT("CookedIniVersion.txt");
					SandboxCookedIniFile = ConvertToFullSandboxPath(*SandboxCookedIniFile, true);
					SandboxCookedIniFile.ReplaceInline(TEXT("[Platform]"), *TargetPlatform->PlatformName());
					IFileManager::Get().Copy(*SandboxCookedIniFile, *SharedCookedIniFile);
					bIterateSharedBuild = true;
					UE_LOG(LogCook, Display, TEXT("Shared iterative build is newer then local cooked build, iteratively cooking from shared build."));
				}
				else
				{
					UE_LOG(LogCook, Display, TEXT("Local cook is newer then shared cooked build, iteratively cooking from local build."));
				}
			}
			const bool bIsIniSettingsOutOfDate = IniSettingsOutOfDate(TargetPlatform); // needs to be executed for side effects even if non-iterative

			bool bShouldClearCookedContent = true;
			if (bIsDiffOnly)
			{
				// When looking for deterministic cooking differences in cooked packages, don't delete the packages on disk
				bShouldClearCookedContent = false;
			}
			else if (bIterative || PlatformData->bIsSandboxInitialized)
			{
				if (!bIsIniSettingsOutOfDate)
				{
					bShouldClearCookedContent = false;
				}
				else
				{
					if (!IsCookFlagSet(ECookInitializationFlags::IgnoreIniSettingsOutOfDate))
					{
						UE_LOG(LogCook, Display, TEXT("Cook invalidated for platform %s ini settings don't match from last cook, clearing all cooked content"), *TargetPlatform->PlatformName());
						bShouldClearCookedContent = true;
					}
					else
					{
						UE_LOG(LogCook, Display, TEXT("Inisettings were out of date for platform %s but we are going with it anyway because IgnoreIniSettingsOutOfDate is set"), *TargetPlatform->PlatformName());
						bShouldClearCookedContent = false;
					}
				}
			}
			else
			{
				// In non-iterative cooks we will be replacing every cooked package and so should wipe the cooked directory
				UE_LOG(LogCook, Display, TEXT("Clearing all cooked content for platform %s"), *TargetPlatform->PlatformName());
				bShouldClearCookedContent = true;
			}

			ICookedPackageWriter::FCookInfo CookInfo;
			CookInfo.CookMode = IsCookOnTheFlyMode() ? ICookedPackageWriter::FCookInfo::CookOnTheFlyMode : ICookedPackageWriter::FCookInfo::CookByTheBookMode;
			CookInfo.bFullBuild = false;
			CookInfo.bIterateSharedBuild = bIterateSharedBuild && !bShouldClearCookedContent;
			bool bResetResults = false;
			if (bShouldClearCookedContent)
			{
				CookInfo.bFullBuild = true;
				bResetResults = true;
			}
			else if (!PlatformData->bIsSandboxInitialized)
			{
				bResetResults = true;
				// Reset but don't populate if we are looking for deterministic cooking differences; start from an empty list of cooked packages
				if (!bIsDiffOnly)
				{
					PopulatePlatforms.Add(TargetPlatform);
				}
			}
			else
			{
				AlreadyCookedPlatforms.Add(TargetPlatform);
			}
			PackageWriter.Initialize(CookInfo);
			PlatformData->bFullBuild = CookInfo.bFullBuild;
			PlatformData->bIsSandboxInitialized = true;
			ResetPlatforms.Emplace(TargetPlatform, bResetResults);
		}

		ResetCook(ResetPlatforms);
		if (PopulatePlatforms.Num())
		{
			PopulateCookedPackages(PopulatePlatforms);
		}
		else if (AlreadyCookedPlatforms.Num())
		{
			// Set the NumPackagesIterativelySkipped field to include all of the already CookedPackages
			COOK_STAT(DetailedCookStats::NumPackagesIterativelySkipped = 0);
			const ITargetPlatform* TargetPlatform = AlreadyCookedPlatforms[0];
			for (UE::Cook::FPackageData* PackageData : *PackageDatas)
			{
				if (PackageData->HasCookedPlatform(TargetPlatform, true /* bIncludeFailed */))
				{
					COOK_STAT(++DetailedCookStats::NumPackagesIterativelySkipped);
				}
			}
		}
	}

#if OUTPUT_COOKTIMING
	FString PlatformNames;
	for (const ITargetPlatform* Target : TargetPlatforms)
	{
		PlatformNames += Target->PlatformName() + TEXT(" ");
	}
	PlatformNames.TrimEndInline();
	UE_LOG(LogCook, Display, TEXT("Sandbox cleanup took %5.3f seconds for platforms %s"), CleanSandboxTime, *PlatformNames);
#endif
}

UE::Cook::FCookSavePackageContext* UCookOnTheFlyServer::CreateSaveContext(const ITargetPlatform* TargetPlatform)
{
	using namespace UE::Cook;

	if (SandboxFile == nullptr)
	{
		CreateSandboxFile();
	}

	const FString RootPathSandbox = ConvertToFullSandboxPath(FPaths::RootDir(), true);
	FString MetadataPathSandbox;
	if (IsCookingDLC())
	{
		MetadataPathSandbox = ConvertToFullSandboxPath(GetBaseDirectoryForDLC() / "Metadata", true);
	}
	else
	{
		MetadataPathSandbox = ConvertToFullSandboxPath(FPaths::ProjectDir() / "Metadata", true);
	}
	const FString PlatformString = TargetPlatform->PlatformName();
	const FString ResolvedRootPath = RootPathSandbox.Replace(TEXT("[Platform]"), *PlatformString);
	const FString ResolvedMetadataPath = MetadataPathSandbox.Replace(TEXT("[Platform]"), *PlatformString);

	ICookedPackageWriter* PackageWriter = nullptr;
	FString WriterDebugName;
	if (IsUsingZenStore())
	{
		PackageWriter = new FZenStoreWriter(ResolvedRootPath, ResolvedMetadataPath, TargetPlatform);
		WriterDebugName = TEXT("ZenStore");
	}
	else
	{
		PackageWriter = new FLooseCookedPackageWriter(ResolvedRootPath, ResolvedMetadataPath, TargetPlatform,
			GetAsyncIODelete(), *PackageDatas, PluginsToRemap);
		WriterDebugName = TEXT("DirectoryWriter");
	}

	DiffModeHelper->InitializePackageWriter(PackageWriter);

	FConfigFile PlatformEngineIni;
	FConfigCacheIni::LoadLocalIniFile(PlatformEngineIni, TEXT("Engine"), true, *TargetPlatform->IniPlatformName());
		
	bool bLegacyBulkDataOffsets = false;
	PlatformEngineIni.GetBool(TEXT("Core.System"), TEXT("LegacyBulkDataOffsets"), bLegacyBulkDataOffsets);
	if (bLegacyBulkDataOffsets)
	{
		UE_LOG(LogCook, Warning, TEXT("Engine.ini:[Core.System]:LegacyBulkDataOffsets is no longer supported in UE5. The intended use was to reduce patch diffs, but UE5 changed cooked bytes in every package for other reasons, so removing support for this flag does not cause additional patch diffs."));
	}

	FCookSavePackageContext* Context = new FCookSavePackageContext(TargetPlatform, PackageWriter, WriterDebugName);
	Context->SaveContext.SetValidator(MakeUnique<FCookedSavePackageValidator>(TargetPlatform, *this));
	return Context;

}

void UCookOnTheFlyServer::FinalizePackageStore()
{
	UE_SCOPED_HIERARCHICAL_COOKTIMER(FinalizePackageStore);

	UE_LOG(LogCook, Display, TEXT("Finalize package store(s)..."));
	for (const ITargetPlatform* TargetPlatform : PlatformManager->GetSessionPlatforms())
	{
		FindOrCreatePackageWriter(TargetPlatform).EndCook();
	}
	UE_LOG(LogCook, Display, TEXT("Done finalizing package store(s)"));
}

void UCookOnTheFlyServer::ClearPackageStoreContexts()
{
	for (UE::Cook::FCookSavePackageContext* Context : SavePackageContexts)
	{
		delete Context;
	}

	SavePackageContexts.Empty();
}

void UCookOnTheFlyServer::InitializeTargetPlatforms(const TArrayView<ITargetPlatform* const>& NewTargetPlatforms)
{
	//allow each platform to update its internals before cooking
	for (ITargetPlatform* TargetPlatform : NewTargetPlatforms)
	{
		TargetPlatform->RefreshSettings();
	}
}

void UCookOnTheFlyServer::DiscoverPlatformSpecificNeverCookPackages(
	const TArrayView<const ITargetPlatform* const>& TargetPlatforms, const TArray<FString>& UBTPlatformStrings)
{
	TArray<const ITargetPlatform*> PluginUnsupportedTargetPlatforms;
	TArray<FAssetData> PluginAssets;
	FARFilter PluginARFilter;
	FString PluginPackagePath;

	TArray<TSharedRef<IPlugin>> AllContentPlugins = IPluginManager::Get().GetEnabledPluginsWithContent();
	for (TSharedRef<IPlugin> Plugin : AllContentPlugins)
	{
		const FPluginDescriptor& Descriptor = Plugin->GetDescriptor();

		// we are only interested in plugins that does not support all platforms
		if (Descriptor.SupportedTargetPlatforms.Num() == 0 && !Descriptor.bHasExplicitPlatforms)
		{
			continue;
		}

		// find any unsupported target platforms for this plugin
		PluginUnsupportedTargetPlatforms.Reset();
		for (int32 I = 0, Count = TargetPlatforms.Num(); I < Count; ++I)
		{
			if (!Descriptor.SupportedTargetPlatforms.Contains(UBTPlatformStrings[I]))
			{
				PluginUnsupportedTargetPlatforms.Add(TargetPlatforms[I]);
			}
		}

		// if there are unsupported target platforms,
		// then add all packages for this plugin for these platforms to the PlatformSpecificNeverCookPackages map
		if (PluginUnsupportedTargetPlatforms.Num() > 0)
		{
			PluginPackagePath.Reset(127);
			PluginPackagePath.AppendChar(TEXT('/'));
			PluginPackagePath.Append(Plugin->GetName());

			PluginARFilter.bRecursivePaths = true;
			PluginARFilter.bIncludeOnlyOnDiskAssets = true;
			PluginARFilter.PackagePaths.Reset(1);
			PluginARFilter.PackagePaths.Emplace(*PluginPackagePath);

			PluginAssets.Reset();
			AssetRegistry->GetAssets(PluginARFilter, PluginAssets);

			for (const ITargetPlatform* TargetPlatform: PluginUnsupportedTargetPlatforms)
			{
				TSet<FName>& NeverCookPackages = PackageTracker->PlatformSpecificNeverCookPackages.FindOrAdd(TargetPlatform);
				for (const FAssetData& Asset : PluginAssets)
				{
					NeverCookPackages.Add(Asset.PackageName);
				}
			}
		}
	}
}

void UCookOnTheFlyServer::TermSandbox()
{
	ClearAllCookedData();
	SandboxFile = nullptr;
}

void UCookOnTheFlyServer::StartCookByTheBook( const FCookByTheBookStartupOptions& CookByTheBookStartupOptions )
{
	UE_SCOPED_COOKTIMER(StartCookByTheBook);

	const TArray<FString>& CookMaps = CookByTheBookStartupOptions.CookMaps;
	const TArray<FString>& CookDirectories = CookByTheBookStartupOptions.CookDirectories;
	const TArray<FString>& IniMapSections = CookByTheBookStartupOptions.IniMapSections;
	const ECookByTheBookOptions& CookOptions = CookByTheBookStartupOptions.CookOptions;
	const FString& DLCName = CookByTheBookStartupOptions.DLCName;

	const FString& CreateReleaseVersion = CookByTheBookStartupOptions.CreateReleaseVersion;
	const FString& BasedOnReleaseVersion = CookByTheBookStartupOptions.BasedOnReleaseVersion;

	check( IsInGameThread() );
	check( IsCookByTheBookMode() );

	//force precache objects to refresh themselves before cooking anything
	LastUpdateTick = INT_MAX;

	CookByTheBookOptions->bCancel = false;
	CookByTheBookOptions->CookTime = 0.0f;
	CookByTheBookOptions->CookStartTime = FPlatformTime::Seconds();
	CookByTheBookOptions->bGenerateStreamingInstallManifests = CookByTheBookStartupOptions.bGenerateStreamingInstallManifests;
	CookByTheBookOptions->bGenerateDependenciesForMaps = CookByTheBookStartupOptions.bGenerateDependenciesForMaps;
	CookByTheBookOptions->CreateReleaseVersion = CreateReleaseVersion;
	CookByTheBookOptions->bSkipHardReferences = !!(CookOptions & ECookByTheBookOptions::SkipHardReferences);
	CookByTheBookOptions->bSkipSoftReferences = !!(CookOptions & ECookByTheBookOptions::SkipSoftReferences);
	CookByTheBookOptions->bFullLoadAndSave = !!(CookOptions & ECookByTheBookOptions::FullLoadAndSave);
	CookByTheBookOptions->bZenStore = !!(CookOptions & ECookByTheBookOptions::ZenStore);
	CookByTheBookOptions->bCookAgainstFixedBase = !!(CookOptions & ECookByTheBookOptions::CookAgainstFixedBase);
	CookByTheBookOptions->bDlcLoadMainAssetRegistry = !!(CookOptions & ECookByTheBookOptions::DlcLoadMainAssetRegistry);
	CookByTheBookOptions->bErrorOnEngineContentUse = CookByTheBookStartupOptions.bErrorOnEngineContentUse;

	// if we are going to change the state of dlc, we need to clean out our package filename cache (the generated filename cache is dependent on this key). This has to happen later on, but we want to set the DLC State earlier.
	const bool bDlcStateChanged = CookByTheBookOptions->DlcName != DLCName;
	CookByTheBookOptions->DlcName = DLCName;
	if (CookByTheBookOptions->bSkipHardReferences && !CookByTheBookOptions->bSkipSoftReferences)
	{
		UE_LOG(LogCook, Warning, TEXT("Setting bSkipSoftReferences to true since bSkipHardReferences is true and skipping hard references requires skipping soft references."));
		CookByTheBookOptions->bSkipSoftReferences = true;
	}
	SetBeginCookConfigSettings();
	COOK_STAT(UE::SavePackageUtilities::ResetCookStats());

	GenerateAssetRegistry();
	BlockOnAssetRegistry();
	PackageDatas->BeginCook();
	if (!IsCookingInEditor())
	{
		FCoreUObjectDelegates::PackageCreatedForLoad.AddUObject(this, &UCookOnTheFlyServer::MaybeMarkPackageAsAlreadyLoaded);
	}

	// SelectSessionPlatforms does not check for uniqueness and non-null, and we rely on those properties for performance, so ensure it here before calling SelectSessionPlatforms
	TArray<ITargetPlatform*> TargetPlatforms;
	TargetPlatforms.Reserve(CookByTheBookStartupOptions.TargetPlatforms.Num());
	for (ITargetPlatform* TargetPlatform : CookByTheBookStartupOptions.TargetPlatforms)
	{
		if (TargetPlatform)
		{
			TargetPlatforms.AddUnique(TargetPlatform);
		}
	}
	PlatformManager->SelectSessionPlatforms(TargetPlatforms);
	bPackageFilterDirty = true;
	check(PlatformManager->GetSessionPlatforms().Num() == TargetPlatforms.Num());

	if (ensure(TargetPlatforms.Num() > 0))
	{
		TrySetDefaultAsyncIODeletePlatform(TargetPlatforms[0]->PlatformName());
	}

	// We want to set bRunning = true as early as possible, but it implies that session platforms have been selected so this is the earliest point we can set it
	CookByTheBookOptions->bRunning = true;

	RefreshPlatformAssetRegistries(TargetPlatforms);

	const UProjectPackagingSettings* const PackagingSettings = GetDefault<UProjectPackagingSettings>();

	// Find all the localized packages and map them back to their source package
	{
		TArray<FString> AllCulturesToCook = CookByTheBookStartupOptions.CookCultures;
		for (const FString& CultureName : CookByTheBookStartupOptions.CookCultures)
		{
			const TArray<FString> PrioritizedCultureNames = FInternationalization::Get().GetPrioritizedCultureNames(CultureName);
			for (const FString& PrioritizedCultureName : PrioritizedCultureNames)
			{
				AllCulturesToCook.AddUnique(PrioritizedCultureName);
			}
		}
		AllCulturesToCook.Sort();

		UE_LOG(LogCook, Display, TEXT("Discovering localized assets for cultures: %s"), *FString::Join(AllCulturesToCook, TEXT(", ")));

		TArray<FString> RootPaths;
		FPackageName::QueryRootContentPaths(RootPaths);

		FARFilter Filter;
		Filter.bRecursivePaths = true;
		Filter.bIncludeOnlyOnDiskAssets = false;
		Filter.PackagePaths.Reserve(AllCulturesToCook.Num() * RootPaths.Num());
		for (const FString& RootPath : RootPaths)
		{
			for (const FString& CultureName : AllCulturesToCook)
			{
				FString LocalizedPackagePath = RootPath / TEXT("L10N") / CultureName;
				Filter.PackagePaths.Add(*LocalizedPackagePath);
			}
		}

		TArray<FAssetData> AssetDataForCultures;
		AssetRegistry->GetAssets(Filter, AssetDataForCultures);

		for (const FAssetData& AssetData : AssetDataForCultures)
		{
			const FName LocalizedPackageName = AssetData.PackageName;
			const FName SourcePackageName = *FPackageName::GetSourcePackagePath(LocalizedPackageName.ToString());

			TArray<FName>& LocalizedPackageNames = CookByTheBookOptions->SourceToLocalizedPackageVariants.FindOrAdd(SourcePackageName);
			LocalizedPackageNames.AddUnique(LocalizedPackageName);
		}

		// Get the list of localization targets to chunk, and remove any targets that we've been asked not to stage
		TArray<FString> LocalizationTargetsToChunk = PackagingSettings->LocalizationTargetsToChunk;
		{
			TArray<FString> BlacklistLocalizationTargets;
			GConfig->GetArray(TEXT("Staging"), TEXT("BlacklistLocalizationTargets"), BlacklistLocalizationTargets, GGameIni);
			if (BlacklistLocalizationTargets.Num() > 0)
			{
				LocalizationTargetsToChunk.RemoveAll([&BlacklistLocalizationTargets](const FString& InLocalizationTarget)
				{
					return BlacklistLocalizationTargets.Contains(InLocalizationTarget);
				});
			}
		}

		if (LocalizationTargetsToChunk.Num() > 0 && AllCulturesToCook.Num() > 0)
		{
			for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
			{
				FAssetRegistryGenerator& RegistryGenerator = *(PlatformManager->GetPlatformData(TargetPlatform)->RegistryGenerator);
				RegistryGenerator.RegisterChunkDataGenerator(MakeShared<FLocalizationChunkDataGenerator>(RegistryGenerator.GetPakchunkIndex(PackagingSettings->LocalizationTargetCatchAllChunkId), LocalizationTargetsToChunk, AllCulturesToCook));
			}
		}
	}

	PackageTracker->NeverCookPackageList.Empty();
	for (FName NeverCookPackage : GetNeverCookPackageFileNames(CookByTheBookStartupOptions.NeverCookDirectories))
	{
		PackageTracker->NeverCookPackageList.Add(NeverCookPackage);
	}

	// use temp list of UBT platform strings to discover PlatformSpecificNeverCookPackages
	{
		TArray<FString> UBTPlatformStrings;
		UBTPlatformStrings.Reserve(TargetPlatforms.Num());
		for (const ITargetPlatform* Platform : TargetPlatforms)
		{
			FString UBTPlatformName;
			Platform->GetPlatformInfo().UBTPlatformName.ToString(UBTPlatformName);
			UBTPlatformStrings.Emplace(MoveTemp(UBTPlatformName));
		}

		DiscoverPlatformSpecificNeverCookPackages(TargetPlatforms, UBTPlatformStrings);
	}

	if (bDlcStateChanged)
	{
		// If we changed the DLC State earlier on, we must clear out the package name cache
		TermSandbox();
	}
	
	FindOrCreateSaveContexts(TargetPlatforms);
	// TODO: Fix the elements of StartCookByTheBook that rely on BeginCookSandbox being called early, and remove this call to BeginCookSandbox
	if (!bHybridIterativeEnabled)
	{
		BeginCookSandbox(TargetPlatforms); // This will either delete the sandbox or iteratively clean it
	}
	InitializeTargetPlatforms(TargetPlatforms);

	if (CurrentCookMode == ECookMode::CookByTheBook && !IsCookFlagSet(ECookInitializationFlags::Iterative))
	{
		UE::SavePackageUtilities::StartSavingEDLCookInfoForVerification();
	}

	{
		if (CookByTheBookOptions->bGenerateDependenciesForMaps)
		{
			for (const ITargetPlatform* Platform : TargetPlatforms)
			{
				CookByTheBookOptions->MapDependencyGraphs.Add(Platform);
			}
		}
	}
	
	// start shader code library cooking
	InitShaderCodeLibrary();
	CleanShaderCodeLibraries();
	
	if ( IsCookingDLC() )
	{
		// If we're cooking against a fixed base, we don't need to verify the packages exist on disk, we simply want to use the Release Data 
		const bool bVerifyPackagesExist = !IsCookingAgainstFixedBase();

		// if we are cooking dlc we must be based on a release version cook
		check( !BasedOnReleaseVersion.IsEmpty() );

		auto ReadDevelopmentAssetRegistry = [this, &BasedOnReleaseVersion, bVerifyPackagesExist]
			(TArray<UE::Cook::FConstructPackageData>& OutPackageList, const FString& InPlatformName)
		{
			TArray<FString> AttemptedNames;
			FString OriginalSandboxRegistryFilename = GetBasedOnReleaseVersionAssetRegistryPath(BasedOnReleaseVersion, InPlatformName ) / TEXT("Metadata") / GetDevelopmentAssetRegistryFilename();
			AttemptedNames.Add(OriginalSandboxRegistryFilename);

			// if this check fails probably because the asset registry can't be found or read
			bool bSucceeded = GetAllPackageFilenamesFromAssetRegistry(OriginalSandboxRegistryFilename, bVerifyPackagesExist, OutPackageList);
			if (!bSucceeded)
			{
				OriginalSandboxRegistryFilename = GetBasedOnReleaseVersionAssetRegistryPath(BasedOnReleaseVersion, InPlatformName) / GetAssetRegistryFilename();
				AttemptedNames.Add(OriginalSandboxRegistryFilename);
				bSucceeded = GetAllPackageFilenamesFromAssetRegistry(OriginalSandboxRegistryFilename, bVerifyPackagesExist, OutPackageList);
			}

			if (!bSucceeded)
			{
				const PlatformInfo::FTargetPlatformInfo* PlatformInfo = PlatformInfo::FindPlatformInfo(*InPlatformName);
				if (PlatformInfo)
				{
					for (const PlatformInfo::FTargetPlatformInfo* PlatformFlavor : PlatformInfo->Flavors)
					{
						OriginalSandboxRegistryFilename = GetBasedOnReleaseVersionAssetRegistryPath(BasedOnReleaseVersion, PlatformFlavor->Name.ToString()) / GetAssetRegistryFilename();
						AttemptedNames.Add(OriginalSandboxRegistryFilename);
						bSucceeded = GetAllPackageFilenamesFromAssetRegistry(OriginalSandboxRegistryFilename, bVerifyPackagesExist, OutPackageList);
						if (bSucceeded)
						{
							break;
						}
					}
				}
			}

			if (bSucceeded)
			{
				UE_LOG(LogCook, Log, TEXT("Loaded assetregistry: %s"), *OriginalSandboxRegistryFilename);
			}
			else
			{
				UE_LOG(LogCook, Log, TEXT("Failed to load DevelopmentAssetRegistry for platform %s. Attempted the following names:\n%s"), *InPlatformName, *FString::Join(AttemptedNames, TEXT("\n")));
			}
			return bSucceeded;
		};

		TArray<UE::Cook::FConstructPackageData> OverridePackageList;
		FString DevelopmentAssetRegistryPlatformOverride;
		const bool bUsingDevRegistryOverride = FParse::Value(FCommandLine::Get(), TEXT("DevelopmentAssetRegistryPlatformOverride="), DevelopmentAssetRegistryPlatformOverride);
		if (bUsingDevRegistryOverride)
		{
			// Read the contents of the asset registry for the overriden platform. We'll use this for all requested platforms so we can just keep one copy of it here
			bool bReadSucceeded = ReadDevelopmentAssetRegistry(OverridePackageList, *DevelopmentAssetRegistryPlatformOverride);
			if (!bReadSucceeded || OverridePackageList.Num() == 0)
			{
				UE_LOG(LogCook, Fatal, TEXT("%s based-on AssetRegistry file %s for DevelopmentAssetRegistryPlatformOverride %s. ")
					TEXT("When cooking DLC, if DevelopmentAssetRegistryPlatformOverride is specified %s is expected to exist under Release/<override> and contain some valid data. Terminating the cook."),
					!bReadSucceeded ? TEXT("Could not find") : TEXT("Empty"),
					*(GetBasedOnReleaseVersionAssetRegistryPath(BasedOnReleaseVersion, DevelopmentAssetRegistryPlatformOverride) / TEXT("Metadata") / GetAssetRegistryFilename()),
					*DevelopmentAssetRegistryPlatformOverride, *GetAssetRegistryFilename());
			}
		}

		for ( const ITargetPlatform* TargetPlatform: TargetPlatforms )
		{
			SCOPED_BOOT_TIMING("AddCookedPlatforms");
			TArray<UE::Cook::FConstructPackageData> PackageList;
			FString PlatformNameString = TargetPlatform->PlatformName();
			FName PlatformName(*PlatformNameString);

			if (!bUsingDevRegistryOverride)
			{
				bool bReadSucceeded = ReadDevelopmentAssetRegistry(PackageList, PlatformNameString);
				if (!bReadSucceeded)
				{
					UE_LOG(LogCook, Fatal, TEXT("Could not find based-on AssetRegistry file %s for platform %s. ")
						TEXT("When cooking DLC, %s is expected to exist Release/<platform> for each platform being cooked. (Or use DevelopmentAssetRegistryPlatformOverride=<PlatformName> to specify an override platform that all platforms should use to find the %s file). Terminating the cook."),
						*(GetBasedOnReleaseVersionAssetRegistryPath(BasedOnReleaseVersion, PlatformNameString) / TEXT("Metadata") / GetAssetRegistryFilename()),
						*PlatformNameString, *GetAssetRegistryFilename(), *GetAssetRegistryFilename());
				}
			}

			TArray<UE::Cook::FConstructPackageData>& ActivePackageList = OverridePackageList.Num() > 0 ? OverridePackageList : PackageList;
			if (ActivePackageList.Num() > 0)
			{
				SCOPED_BOOT_TIMING("AddPackageDataByFileNamesForPlatform");
				PackageDatas->AddExistingPackageDatasForPlatform(ActivePackageList, TargetPlatform);
			}

			TArray<FName>& PlatformBasedPackages = CookByTheBookOptions->BasedOnReleaseCookedPackages.FindOrAdd(PlatformName);
			PlatformBasedPackages.Reset(ActivePackageList.Num());
			for (UE::Cook::FConstructPackageData& PackageData : ActivePackageList)
			{
				PlatformBasedPackages.Add(PackageData.NormalizedFileName);
			}
		}
	}
	
	// add shader library chunkers
	if (PackagingSettings->bShareMaterialShaderCode && IsUsingShaderCodeLibrary())
	{
		for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
		{
			FAssetRegistryGenerator& RegistryGenerator = *(PlatformManager->GetPlatformData(TargetPlatform)->RegistryGenerator);
			RegistryGenerator.RegisterChunkDataGenerator(MakeShared<FShaderLibraryChunkDataGenerator>(TargetPlatform));
		}
	}

	// don't resave the global shader map files in dlc
	if (!IsCookingDLC() && !(CookByTheBookStartupOptions.CookOptions & ECookByTheBookOptions::ForceDisableSaveGlobalShaders))
	{
		OpenGlobalShaderLibrary();

		SaveGlobalShaderMapFiles(TargetPlatforms);

		SaveAndCloseGlobalShaderLibrary();
	}
	
	// Open the shader code library for the current project or the current DLC pack, depending on which we are cooking
    {
		FString LibraryName = !IsCookingDLC() ? FApp::GetProjectName() : CookByTheBookOptions->DlcName;
		if (LibraryName.Len() > 0)
		{
			OpenShaderLibrary(LibraryName);
		}
	}

	TSet<FName> StartupSoftObjectPackages;
	if (!IsCookByTheBookMode() || !CookByTheBookOptions->bSkipSoftReferences)
	{
		// Get the list of soft references, for both empty package and all startup packages
		GRedirectCollector.ProcessSoftObjectPathPackageList(NAME_None, false, StartupSoftObjectPackages);

		for (const FName& StartupPackage : CookByTheBookOptions->StartupPackages)
		{
			GRedirectCollector.ProcessSoftObjectPathPackageList(StartupPackage, false, StartupSoftObjectPackages);
		}
	}
	GRedirectCollector.OnStartupPackageLoadComplete();

	TMap<FName, TArray<FName>> GameDefaultObjects;
	GetGameDefaultObjects(TargetPlatforms, GameDefaultObjects);

	// Strip out the default maps from SoftObjectPaths collected from startup packages. They will be added to the cook if necessary by CollectFilesToCook.
	for (const auto& GameDefaultSet : GameDefaultObjects)
	{
		for (FName AssetName : GameDefaultSet.Value)
		{
			StartupSoftObjectPackages.Remove(AssetName);
		}
	}
	
	TArray<FName> FilesInPath;
	TMap<FName, UE::Cook::FInstigator> FilesInPathInstigators;
	CollectFilesToCook(FilesInPath, FilesInPathInstigators, CookMaps, CookDirectories, IniMapSections, CookOptions, TargetPlatforms, GameDefaultObjects);

	// Add soft/hard startup references after collecting requested files and handling empty requests
	if (bPreexploreDependenciesEnabled && !CookByTheBookOptions->bSkipHardReferences && !CookByTheBookOptions->bFullLoadAndSave)
	{
		ProcessUnsolicitedPackages(&FilesInPath, &FilesInPathInstigators);
	}
	for (FName SoftObjectPackage : StartupSoftObjectPackages)
	{
		TMap<FName, FName> RedirectedPaths;

		// If this is a redirector, extract destination from asset registry
		if (ContainsRedirector(SoftObjectPackage, RedirectedPaths))
		{
			for (TPair<FName, FName>& RedirectedPath : RedirectedPaths)
			{
				GRedirectCollector.AddAssetPathRedirection(RedirectedPath.Key, RedirectedPath.Value);
			}
		}

		if (!CookByTheBookOptions->bSkipSoftReferences)
		{
			AddFileToCook(FilesInPath, FilesInPathInstigators, SoftObjectPackage.ToString(),
				UE::Cook::EInstigator::StartupSoftObjectPath);
		}
	}
	
	if (FilesInPath.Num() == 0)
	{
		LogCookerMessage(FString::Printf(TEXT("No files found to cook.")), EMessageSeverity::Warning);
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("RANDOMPACKAGEORDER")) || 
		(FParse::Param(FCommandLine::Get(), TEXT("DIFFONLY")) && !FParse::Param(FCommandLine::Get(), TEXT("DIFFNORANDCOOK"))))
	{
		UE_LOG(LogCook, Log, TEXT("Randomizing package order."));
		//randomize the array, taking the Array_Shuffle approach, in order to help bring cooking determinism issues to the surface.
		for (int32 FileIndex = 0; FileIndex < FilesInPath.Num(); ++FileIndex)
		{
			FilesInPath.Swap(FileIndex, FMath::RandRange(FileIndex, FilesInPath.Num() - 1));
		}
	}

	{
		UE_SCOPED_HIERARCHICAL_COOKTIMER(GenerateLongPackageName);
		GenerateLongPackageNames(FilesInPath, FilesInPathInstigators);
	}
	// add all the files to the cook list for the requested platforms
	for (FName PackageName : FilesInPath)
	{
		if (PackageName.IsNone())
		{
			continue;
		}

		const FName PackageFileFName = PackageDatas->GetFileNameByPackageName(PackageName);
		
		if (!PackageFileFName.IsNone())
		{
			UE::Cook::FInstigator& Instigator = FilesInPathInstigators.FindChecked(PackageName);
			ExternalRequests->EnqueueUnique(UE::Cook::FFilePlatformRequest(PackageFileFName, MoveTemp(Instigator), TargetPlatforms));
		}
		else if (!FLinkerLoad::IsKnownMissingPackage(PackageName))
		{
			LogCookerMessage( FString::Printf(TEXT("Unable to find package for cooking %s"), *PackageName.ToString()),
				EMessageSeverity::Warning );
		}
	}

	if (!IsCookingDLC())
	{
		// if we are not cooking dlc then basedOnRelease version just needs to make sure that we cook all the packages which are in the previous release (as well as the new ones)
		if ( !BasedOnReleaseVersion.IsEmpty() )
		{
			// if we are based on a release and we are not cooking dlc then we should always be creating a new one (note that we could be creating the same one we are based on).
			// note that we might erroneously enter here if we are generating a patch instead and we accidentally passed in BasedOnReleaseVersion to the cooker instead of to unrealpak
			UE_CLOG(CreateReleaseVersion.IsEmpty(), LogCook, Fatal, TEXT("-BasedOnReleaseVersion must be used together with either -dlcname or -CreateReleaseVersion."));

			for ( const ITargetPlatform* TargetPlatform : TargetPlatforms )
			{
				// if we are based of a cook and we are creating a new one we need to make sure that at least all the old packages are cooked as well as the new ones
				FString OriginalAssetRegistryPath = GetBasedOnReleaseVersionAssetRegistryPath( BasedOnReleaseVersion, TargetPlatform->PlatformName() ) / GetAssetRegistryFilename();

				TArray<UE::Cook::FConstructPackageData> BasedOnReleaseDatas;
				verify( GetAllPackageFilenamesFromAssetRegistry(OriginalAssetRegistryPath, true, BasedOnReleaseDatas) );

				TArray<const ITargetPlatform*, TInlineAllocator<1>> RequestPlatforms;
				RequestPlatforms.Add(TargetPlatform);
				for ( const UE::Cook::FConstructPackageData& PackageData : BasedOnReleaseDatas)
				{
					ExternalRequests->EnqueueUnique(
						UE::Cook::FFilePlatformRequest(PackageData.NormalizedFileName,
							UE::Cook::EInstigator::PreviousAssetRegistry, RequestPlatforms));
				}
			}
		}
	}

	if (bHybridIterativeEnabled)
	{
		BeginCookSandbox(TargetPlatforms);
	}
	if (bHybridIterativeDebug)
	{
		// Discoveries during the processing of the initial cluster are expected, so LogDiscoveredPackages must be off.
		PackageDatas->SetLogDiscoveredPackages(false);
	}

	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		FindOrCreatePackageWriter(TargetPlatform).BeginCook();
	}
}

TArray<FName> UCookOnTheFlyServer::GetNeverCookPackageFileNames(TArrayView<const FString> ExtraNeverCookDirectories)
{
	TArray<FString> NeverCookDirectories(ExtraNeverCookDirectories);

	auto AddDirectoryPathArray = [&NeverCookDirectories](const TArray<FDirectoryPath>& DirectoriesToNeverCook, const TCHAR* SettingName)
	{
		for (const FDirectoryPath& DirToNotCook : DirectoriesToNeverCook)
		{
			FString LocalPath;
			if (FPackageName::TryConvertGameRelativePackagePathToLocalPath(DirToNotCook.Path, LocalPath))
			{
				NeverCookDirectories.Add(LocalPath);
			}
			else
			{
				UE_LOG(LogCook, Warning, TEXT("'%s' has invalid element '%s'"), SettingName, *DirToNotCook.Path);
			}
		}

	};
	const UProjectPackagingSettings* const PackagingSettings = GetDefault<UProjectPackagingSettings>();

	if (IsCookByTheBookMode())
	{
		// Respect the packaging settings nevercook directories for CookByTheBook
		AddDirectoryPathArray(PackagingSettings->DirectoriesToNeverCook, TEXT("ProjectSettings -> Project -> Packaging -> Directories to never cook"));
		AddDirectoryPathArray(PackagingSettings->TestDirectoriesToNotSearch, TEXT("ProjectSettings -> Project -> Packaging -> Test directories to not search"));
	}

	// For all modes, never cook External Actors; they are handled by the parent map
	NeverCookDirectories.Add(FString::Printf(TEXT("/Game/%s"), ULevel::GetExternalActorsFolderName()));

	TArray<FString> NeverCookPackagesPaths;
	FPackageName::FindPackagesInDirectories(NeverCookPackagesPaths, NeverCookDirectories);

	TArray<FName> NeverCookNormalizedFileNames;
	for (const FString& NeverCookPackagePath : NeverCookPackagesPaths)
	{
		NeverCookNormalizedFileNames.Add(UE::Cook::FPackageDatas::GetStandardFileName(NeverCookPackagePath));
	}
	return NeverCookNormalizedFileNames;
}

bool UCookOnTheFlyServer::RecompileChangedShaders(const TArray<const ITargetPlatform*>& TargetPlatforms)
{
	bool bShadersRecompiled = false;
	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		bShadersRecompiled |= RecompileChangedShadersForPlatform(TargetPlatform->PlatformName());
	}
	return bShadersRecompiled;
}

bool UCookOnTheFlyServer::RecompileChangedShaders(const TArray<FName>& TargetPlatformNames)
{
	bool bShadersRecompiled = false;
	for (const FName& TargetPlatformName : TargetPlatformNames)
	{
		bShadersRecompiled |= RecompileChangedShadersForPlatform(TargetPlatformName.ToString());
	}
	return bShadersRecompiled;
}

/* UCookOnTheFlyServer callbacks
 *****************************************************************************/

void UCookOnTheFlyServer::MaybeMarkPackageAsAlreadyLoaded(UPackage *Package)
{
	// can't use this optimization while cooking in editor
	check(IsCookingInEditor()==false);
	check(IsCookByTheBookMode());

	// if the package is already fully loaded then we are not going to mark it up anyway
	if ( Package->IsFullyLoaded() )
	{
		return;
	}

	bool bShouldMarkAsAlreadyProcessed = false;

	TArray<const ITargetPlatform*> CookedPlatforms;
	UE::Cook::FPackageData* PackageData = PackageDatas->FindPackageDataByPackageName(Package->GetFName());
	if (!PackageData)
	{
		return;
	}
	FName StandardName = PackageData->GetFileName();
	if (PackageData->HasAnyCookedPlatform())
	{
		bShouldMarkAsAlreadyProcessed = PackageData->HasAllCookedPlatforms(PlatformManager->GetSessionPlatforms(), true /* bIncludeFailed */);

		FString Platforms;
		for (const TPair<const ITargetPlatform*, UE::Cook::FPackageData::FPlatformData>& Pair : PackageData->GetPlatformDatas())
		{
			if (Pair.Value.bCookAttempted)
			{
				Platforms += TEXT(" ");
				Platforms += Pair.Key->PlatformName();
			}
		}
		if (IsCookFlagSet(ECookInitializationFlags::LogDebugInfo))
		{
			if (!bShouldMarkAsAlreadyProcessed)
			{
				UE_LOG(LogCook, Display, TEXT("Reloading package %s slowly because it wasn't cooked for all platforms %s."), *StandardName.ToString(), *Platforms);
			}
			else
			{
				UE_LOG(LogCook, Display, TEXT("Marking %s as reloading for cooker because it's been cooked for platforms %s."), *StandardName.ToString(), *Platforms);
			}
		}
	}

	check(IsInGameThread());
	if (PackageTracker->NeverCookPackageList.Contains(StandardName))
	{
		bShouldMarkAsAlreadyProcessed = true;
		UE_LOG(LogCook, Verbose, TEXT("Marking %s as reloading for cooker because it was requested as never cook package."), *StandardName.ToString());
	}

	if (bShouldMarkAsAlreadyProcessed)
	{
		if (Package->IsFullyLoaded() == false)
		{
			Package->SetPackageFlags(PKG_ReloadingForCooker);
		}
	}
}

static void AppendExistingPackageSidecarFiles(const FString& PackageSandboxFilename, const FString& PackageStandardFilename, TArray<FString>& OutPackageSidecarFiles)
{
	const TCHAR* const PackageSidecarExtensions[] =
	{
		TEXT(".uexp"),
		// TODO: re-enable this once the client-side of the NetworkPlatformFile isn't prone to becoming overwhelmed by slow writing of unsolicited files
		//TEXT(".ubulk"),
		//TEXT(".uptnl"),
		//TEXT(".m.ubulk")
	};

	for (const TCHAR* PackageSidecarExtension : PackageSidecarExtensions)
	{
		const FString SidecarSandboxFilename = FPathViews::ChangeExtension(PackageSandboxFilename, PackageSidecarExtension);
		if (IFileManager::Get().FileExists(*SidecarSandboxFilename))
		{
			OutPackageSidecarFiles.Add(FPathViews::ChangeExtension(PackageStandardFilename, PackageSidecarExtension));
		}
	}
}

void UCookOnTheFlyServer::GetCookOnTheFlyUnsolicitedFiles(const ITargetPlatform* TargetPlatform, const FString& PlatformName, TArray<FString>& UnsolicitedFiles, const FString& Filename, bool bIsCookable)
{
	UPackage::WaitForAsyncFileWrites();

	if (bIsCookable)
		AppendExistingPackageSidecarFiles(ConvertToFullSandboxPath(*Filename, true, PlatformName), Filename, UnsolicitedFiles);

	TArray<FName> UnsolicitedFilenames;
	PackageTracker->UnsolicitedCookedPackages.GetPackagesForPlatformAndRemove(TargetPlatform, UnsolicitedFilenames);

	for (const FName& UnsolicitedFile : UnsolicitedFilenames)
	{
		FString StandardFilename = UnsolicitedFile.ToString();
		FPaths::MakeStandardFilename(StandardFilename);

		// check that the sandboxed file exists... if it doesn't then don't send it back
		// this can happen if the package was saved but the async writer thread hasn't finished writing it to disk yet

		FString SandboxFilename = ConvertToFullSandboxPath(*StandardFilename, true, PlatformName);
		if (IFileManager::Get().FileExists(*SandboxFilename))
		{
			UnsolicitedFiles.Add(StandardFilename);
			if (FPackageName::IsPackageExtension(*FPaths::GetExtension(StandardFilename, true)))
				AppendExistingPackageSidecarFiles(SandboxFilename, StandardFilename, UnsolicitedFiles);
		}
		else
		{
			UE_LOG(LogCook, Warning, TEXT("Unsolicited file doesn't exist in sandbox, ignoring %s"), *StandardFilename);
		}
	}
}

bool UCookOnTheFlyServer::GetAllPackageFilenamesFromAssetRegistry(const FString& AssetRegistryPath,
	bool bVerifyPackagesExist, TArray<UE::Cook::FConstructPackageData>& OutPackageDatas) const
{
	using namespace UE::Cook;

	UE_SCOPED_COOKTIMER(GetAllPackageFilenamesFromAssetRegistry);
	SCOPED_BOOT_TIMING("GetAllPackageFilenamesFromAssetRegistry");
	TUniquePtr<FArchive> Reader(IFileManager::Get().CreateFileReader(*AssetRegistryPath));
	if (Reader)
	{
		// is there a matching preloaded AR?
		GPreloadARInfoEvent->Wait();

		bool bHadPreloadedAR = false;
		if (AssetRegistryPath == GPreloadedARPath)
		{
			// make sure the Serialize call is done
			double Start = FPlatformTime::Seconds();
			GPreloadAREvent->Wait();
			double TimeWaiting = FPlatformTime::Seconds() - Start;
			UE_LOG(LogCook, Display, TEXT("Blocked %.4f ms waiting for AR to finish loading"), TimeWaiting * 1000.0);
			
			// if something went wrong, the num assets may be zero, in which case we do the normal load 
			bHadPreloadedAR = GPreloadedARState.GetNumAssets() > 0;
		}

		// if we didn't preload an AR, then we need to do a blocking load now
		if (!bHadPreloadedAR)
		{
			GPreloadedARState.Serialize(*Reader.Get(), FAssetRegistrySerializationOptions());
		}

		// possibly use the preloaded state
		const TMap<FName, const FAssetData*>& RegistryDataMap = GPreloadedARState.GetObjectPathToAssetDataMap();

		check(OutPackageDatas.Num() == 0);

		int32 NumPackages = RegistryDataMap.Num();
		TArray<const FAssetData*> AssetDatas;
		TSet<FName> PackageNames;
		AssetDatas.Reserve(NumPackages);
		OutPackageDatas.Reserve(NumPackages);

		// Convert the Map of RegistryData into an Array of FAssetData and populate PackageNames in the output array
		for (const TPair<FName, const FAssetData*>& RegistryData : RegistryDataMap)
		{
			FName PackageName = RegistryData.Value->PackageName;
			bool bPackageAlreadyAdded;
			PackageNames.Add(PackageName, &bPackageAlreadyAdded);
			if (bPackageAlreadyAdded)
			{
				continue;
			}
			if (FPackageName::GetPackageMountPoint(PackageName.ToString()).IsNone())
			{
				// Skip any packages that are not currently mounted; if we tried to find their FileNames below
				// we would get log spam
				continue;
			}

			AssetDatas.Add(RegistryData.Value);
			FConstructPackageData& PackageData = OutPackageDatas.Emplace_GetRef();
			PackageData.PackageName = PackageName;

			// For any PackageNames that already have PackageDatas, mark them ahead of the loop to
			// skip the effort of checking whether they exist on disk inside the loop
			FPackageData* ExistingPackageData = PackageDatas->FindPackageDataByPackageName(PackageName);
			if (ExistingPackageData)
			{
				PackageData.NormalizedFileName = ExistingPackageData->GetFileName();
			}
		}
		NumPackages = AssetDatas.Num();

		ParallelFor(NumPackages,
			[&AssetRegistryPath, &OutPackageDatas, &AssetDatas, this, bVerifyPackagesExist](int32 AssetIndex)
			{
				FConstructPackageData& PackageData = OutPackageDatas[AssetIndex];
				if (!PackageData.NormalizedFileName.IsNone())
				{
					return;
				}
				const FName PackageName = PackageData.PackageName;

				// TODO ICookPackageSplitter: Need to handle GeneratedPackages that exist in the cooked AssetRegistry we are 
				// reading, but do not exist in WorkspaceDomain and so are not found when we look them up here.
				FName PackageFileName = FPackageDatas::LookupFileNameOnDisk(PackageName, true /* bRequireExists */);
				if (!PackageFileName.IsNone())
				{
					PackageData.NormalizedFileName = PackageFileName;
				}
				else
				{
					if (bVerifyPackagesExist)
					{
						UE_LOG(LogCook, Warning, TEXT("Could not resolve package %s from %s"),
							*PackageName.ToString(), *AssetRegistryPath);
					}
					else
					{
						const bool bContainsMap = AssetDatas[AssetIndex]->PackageFlags & PKG_ContainsMap;
						PackageFileName = FPackageDatas::LookupFileNameOnDisk(PackageName,
							false /* bRequireExists */, bContainsMap);
						if (!PackageFileName.IsNone())
						{
							PackageData.NormalizedFileName = PackageFileName;
						}
					}
				}
			});

		OutPackageDatas.RemoveAllSwap([](FConstructPackageData& PackageData)
			{
				return PackageData.NormalizedFileName.IsNone();
			}, false /* bAllowShrinking */);
		return true;
	}

	return false;
}

uint32 UCookOnTheFlyServer::FullLoadAndSave(uint32& CookedPackageCount)
{
	UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave);
	check(CurrentCookMode == ECookMode::CookByTheBook);
	check(CookByTheBookOptions);
	check(IsInGameThread());

	uint32 Result = 0;

	const TArray<const ITargetPlatform*>& TargetPlatforms = PlatformManager->GetSessionPlatforms();

	{
		UE_LOG(LogCook, Display, TEXT("Loading requested packages..."));
		UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_RequestedLoads);
		while (ExternalRequests->HasRequests())
		{
			TArray<UE::Cook::FFilePlatformRequest> BuildRequests;
			TArray<UE::Cook::FSchedulerCallback> SchedulerCallbacks;
			UE::Cook::EExternalRequestType RequestType = ExternalRequests->DequeueNextCluster(SchedulerCallbacks, BuildRequests);
			if (RequestType == UE::Cook::EExternalRequestType::Callback)
			{
				for (UE::Cook::FSchedulerCallback& SchedulerCallback : SchedulerCallbacks)
				{
					SchedulerCallback();
				}
				continue;
			}
			check(RequestType == UE::Cook::EExternalRequestType::Cook && BuildRequests.Num() > 0);
			for (UE::Cook::FFilePlatformRequest& ToBuild : BuildRequests)
			{
				const FName BuildFilenameFName = ToBuild.GetFilename();
				if (!PackageTracker->NeverCookPackageList.Contains(BuildFilenameFName))
				{
					const FString BuildFilename = BuildFilenameFName.ToString();
					GIsCookerLoadingPackage = true;
					UE_SCOPED_HIERARCHICAL_COOKTIMER(LoadPackage);
					LoadPackage(nullptr, *BuildFilename, LOAD_None);
					if (GShaderCompilingManager)
					{
						GShaderCompilingManager->ProcessAsyncResults(true, false);
					}
					FAssetCompilingManager::Get().ProcessAsyncTasks(true);
					GIsCookerLoadingPackage = false;
				}
			}
		}
	}

	const bool bSaveConcurrent = FParse::Param(FCommandLine::Get(), TEXT("ConcurrentSave"));
	uint32 SaveFlags = SAVE_KeepGUID | SAVE_Async | SAVE_ComputeHash | (IsCookFlagSet(ECookInitializationFlags::Unversioned) ? SAVE_Unversioned : 0);
	if (bSaveConcurrent)
	{
		SaveFlags |= SAVE_Concurrent;
	}
	TArray<UE::Cook::FPackageData*> PackagesToSave;
	PackagesToSave.Reserve(65536);

	TSet<UPackage*> ProcessedPackages;
	ProcessedPackages.Reserve(65536);

	TMap<UWorld*, TArray<bool>> WorldsToPostSaveRoot;
	WorldsToPostSaveRoot.Reserve(1024);

	TArray<UObject*> ObjectsToWaitForCookedPlatformData;
	ObjectsToWaitForCookedPlatformData.Reserve(65536);

	TArray<FString> PackagesToLoad;

	FObjectSaveContextData ObjectSaveContext(nullptr, nullptr, TEXT(""), SaveFlags);

	do
	{
		PackagesToLoad.Reset();

		{
			UE_LOG(LogCook, Display, TEXT("Caching platform data and discovering string referenced assets..."));
			UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_CachePlatformDataAndDiscoverNewAssets);
			for (TObjectIterator<UPackage> It; It; ++It)
			{
				UPackage* Package = *It;
				check(Package);

				if (ProcessedPackages.Contains(Package))
				{
					continue;
				}

				ProcessedPackages.Add(Package);

				if (Package->HasAnyPackageFlags(PKG_CompiledIn | PKG_ForDiffing | PKG_EditorOnly | PKG_Compiling | PKG_PlayInEditor | PKG_ContainsScript | PKG_ReloadingForCooker))
				{
					continue;
				}

				if (Package == GetTransientPackage())
				{
					continue;
				}

				FName PackageName = Package->GetFName();
				if (PackageTracker->NeverCookPackageList.Contains(PackageDatas->GetFileNameByPackageName(PackageName)))
				{
					// refuse to save this package
					continue;
				}

				if (!FPackageName::IsValidLongPackageName(PackageName.ToString()))
				{
					continue;
				}

				if (Package->GetOuter() != nullptr)
				{
					UE_LOG(LogCook, Warning, TEXT("Skipping package %s with outermost %s"), *Package->GetName(), *Package->GetOutermost()->GetName());
					continue;
				}

				UE::Cook::FPackageData* PackageData = PackageDatas->TryAddPackageDataByPackageName(PackageName);
				// Legacy behavior: if TryAddPackageDataByPackageName failed, we will still try to load the Package, but we will not try to save it.
				if (PackageData)
				{
					PackageData->SetPackage(Package);
					PackagesToSave.Add(PackageData);
				}


				{
					UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_PerObjectLogic);
					TSet<UObject*> ProcessedObjects;
					ProcessedObjects.Reserve(64);
					bool bObjectsMayHaveBeenCreated = false;
					do
					{
						bObjectsMayHaveBeenCreated = false;
						TArray<UObject*> ObjsInPackage;
						{
							UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_GetObjectsWithOuter);
							GetObjectsWithOuter(Package, ObjsInPackage, true);
						}
						for (UObject* Obj : ObjsInPackage)
						{
							if (Obj->HasAnyFlags(RF_Transient))
							{
								continue;
							}

							if (ProcessedObjects.Contains(Obj))
							{
								continue;
							}

							bObjectsMayHaveBeenCreated = true;
							ProcessedObjects.Add(Obj);

							UWorld* World = Cast<UWorld>(Obj);
							bool bInitializedPhysicsSceneForSave = false;
							bool bForceInitializedWorld = false;

							bool bAllPlatformDataLoaded = true;
							bool bIsTexture = Obj->IsA(UTexture::StaticClass());
							bool bFirstPlatform = true;
							for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
							{
								ObjectSaveContext.TargetPlatform = TargetPlatform;
								ObjectSaveContext.bOuterConcurrentSave = bFirstPlatform;
								bFirstPlatform = false;

								if (bSaveConcurrent)
								{
									GIsCookerLoadingPackage = true;
									if (World)
									{
										UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_SettingUpWorlds);
										// We need a physics scene at save time in case code does traces during onsave events.
										if (ObjectSaveContext.bOuterConcurrentSave)
										{
											bInitializedPhysicsSceneForSave = GEditor->InitializePhysicsSceneForSaveIfNecessary(World, bForceInitializedWorld);
										}

										{
											UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_PreSaveWorld);
											GEditor->OnPreSaveWorld(World, FObjectPreSaveContext(ObjectSaveContext));
										}
										{
											UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_PreSaveRoot);
											UE::SavePackageUtilities::CallPreSaveRoot(World, ObjectSaveContext);
											WorldsToPostSaveRoot.FindOrAdd(World).Add(ObjectSaveContext.bCleanupRequired);
										}
									}

									{
										UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_PreSave);
										UE::SavePackageUtilities::CallPreSave(Obj, ObjectSaveContext);
									}
									GIsCookerLoadingPackage = false;
								}

								if (!bIsTexture || bSaveConcurrent)
								{
									UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_BeginCache);
									UE_TRACK_REFERENCING_PACKAGE_SCOPED(Package, PackageAccessTrackingOps::NAME_CookerBuildObject);
									Obj->BeginCacheForCookedPlatformData(TargetPlatform);
									if (!Obj->IsCachedCookedPlatformDataLoaded(TargetPlatform))
									{
										bAllPlatformDataLoaded = false;
									}
								}
							}

							if (!bAllPlatformDataLoaded)
							{
								ObjectsToWaitForCookedPlatformData.Add(Obj);
							}

							if (World && bInitializedPhysicsSceneForSave)
							{
								UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_CleaningUpWorlds);
								GEditor->CleanupPhysicsSceneThatWasInitializedForSave(World, bForceInitializedWorld);
							}
						}
					} while (bObjectsMayHaveBeenCreated);

					if (bSaveConcurrent)
					{
						UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_MiscPrep);
						// Precache the metadata so we don't risk rehashing the map in the parallelfor below
						Package->GetMetaData();
					}
				}

				if (!IsCookByTheBookMode() || !CookByTheBookOptions->bSkipSoftReferences)
				{
					UE_SCOPED_HIERARCHICAL_COOKTIMER(ResolveStringReferences);
					TSet<FName> StringAssetPackages;
					GRedirectCollector.ProcessSoftObjectPathPackageList(PackageName, false, StringAssetPackages);

					for (FName StringAssetPackage : StringAssetPackages)
					{
						TMap<FName, FName> RedirectedPaths;

						// If this is a redirector, extract destination from asset registry
						if (ContainsRedirector(StringAssetPackage, RedirectedPaths))
						{
							for (TPair<FName, FName>& RedirectedPath : RedirectedPaths)
							{
								GRedirectCollector.AddAssetPathRedirection(RedirectedPath.Key, RedirectedPath.Value);
								PackagesToLoad.Add(FPackageName::ObjectPathToPackageName(RedirectedPath.Value.ToString()));
							}
						}
						else
						{
							PackagesToLoad.Add(StringAssetPackage.ToString());
						}
					}
				}
			}
		}

		{
			UE_LOG(LogCook, Display, TEXT("Loading string referenced assets..."));
			UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_LoadStringReferencedAssets);
			GIsCookerLoadingPackage = true;
			for (const FString& ToLoad : PackagesToLoad)
			{
				FName BuildFilenameFName = PackageDatas->GetFileNameByPackageName(FName(*ToLoad));
				if (!PackageTracker->NeverCookPackageList.Contains(BuildFilenameFName))
				{
					LoadPackage(nullptr, *ToLoad, LOAD_None);
					if (GShaderCompilingManager)
					{
						GShaderCompilingManager->ProcessAsyncResults(true, false);
					}
					FAssetCompilingManager::Get().ProcessAsyncTasks(true);
				}
			}
			GIsCookerLoadingPackage = false;
		}
	} while (PackagesToLoad.Num() > 0);

	ProcessedPackages.Empty();

	// When saving concurrently, before starting the concurrent saves, do tasks which are normally done in SavePackage that
	// cannot be done from other threads or during FScopedSavingFlag in SavePackage.
	if (bSaveConcurrent)
	{
		UE_LOG(LogCook, Display, TEXT("Flushing async loading..."));
		UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_FlushAsyncLoading);
		FlushAsyncLoading();

		FPackageLocalizationManager::Get().ConditionalUpdateCache();
	}

	if (bSaveConcurrent)
	{
		UE_LOG(LogCook, Display, TEXT("Waiting for async tasks..."));
		UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_ProcessThreadUntilIdle);
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
	}

	if (FAssetCompilingManager::Get().GetNumRemainingAssets())
	{
		UE_LOG(LogCook, Display, TEXT("Waiting for async compilation..."));
		// Wait for all assets to finish compiling
		FAssetCompilingManager::Get().FinishAllCompilation();
	}

	// Wait for all platform data to be loaded
	{
		UE_LOG(LogCook, Display, TEXT("Waiting for cooked platform data..."));
		UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_WaitForCookedPlatformData);
		while (ObjectsToWaitForCookedPlatformData.Num() > 0)
		{
			for (int32 ObjIdx = ObjectsToWaitForCookedPlatformData.Num() - 1; ObjIdx >= 0; --ObjIdx)
			{
				UObject* Obj = ObjectsToWaitForCookedPlatformData[ObjIdx];
				bool bAllPlatformDataLoaded = true;
				UE_TRACK_REFERENCING_PACKAGE_SCOPED(Obj->GetPackage(), PackageAccessTrackingOps::NAME_CookerBuildObject);
				for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
				{
					if (!Obj->IsCachedCookedPlatformDataLoaded(TargetPlatform))
					{
						bAllPlatformDataLoaded = false;
						break;
					}
				}

				if (bAllPlatformDataLoaded)
				{
					ObjectsToWaitForCookedPlatformData.RemoveAtSwap(ObjIdx, 1, false);
				}
			}

			FAssetCompilingManager::Get().ProcessAsyncTasks(true);
			FPlatformProcess::Sleep(0.001f);
		}

		ObjectsToWaitForCookedPlatformData.Empty();
	}

	{
		UE_LOG(LogCook, Display, TEXT("Saving packages..."));
		UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_Save);
		check(bIsSavingPackage == false);
		bIsSavingPackage = true;

		if (bSaveConcurrent)
		{
			GIsSavingPackage = true;
		}

		TArray<FAssetRegistryGenerator*> Generators;
		Generators.Reserve(TargetPlatforms.Num());
		for (const ITargetPlatform* Target : TargetPlatforms)
		{
			Generators.Add(PlatformManager->GetPlatformData(Target)->RegistryGenerator.Get());
		}

		int64 ParallelSavedPackages = 0;
		ParallelFor(PackagesToSave.Num(),
			[this, &PackagesToSave, &TargetPlatforms, &Generators, &ParallelSavedPackages, SaveFlags, bSaveConcurrent](int32 PackageIdx)
		{
			UE::Cook::FPackageData& PackageData = *PackagesToSave[PackageIdx];
			UPackage* Package = PackageData.GetPackage();
			check(Package);

			// when concurrent saving is supported, precaching will need to be refactored for concurrency
			if (!bSaveConcurrent)
			{
				// precache texture platform data ahead of save
				const int32 PrecacheOffset = 512;
				UPackage* PrecachePackage = PackageIdx + PrecacheOffset < PackagesToSave.Num() ? PackagesToSave[PackageIdx + PrecacheOffset]->GetPackage() : nullptr;
				if (PrecachePackage)
				{
					TArray<UObject*> ObjsInPackage;
					{
						GetObjectsWithOuter(PrecachePackage, ObjsInPackage, false);
					}

					UE_TRACK_REFERENCING_PACKAGE_SCOPED(PrecachePackage, PackageAccessTrackingOps::NAME_CookerBuildObject);
					for (UObject* Obj : ObjsInPackage)
					{
						if (Obj->HasAnyFlags(RF_Transient) || !Obj->IsA(UTexture::StaticClass()))
						{
							continue;
						}

						for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
						{
							Obj->BeginCacheForCookedPlatformData(TargetPlatform);
						}
					}
				}
			}

			const FName& PackageFileName = PackageData.GetFileName();
			if (!PackageFileName.IsNone())
			{
				// Use SandboxFile to do path conversion to properly handle sandbox paths (outside of standard paths in particular).
				FString Filename = ConvertToFullSandboxPath(PackageFileName.ToString(), true);

				// look for a world object in the package (if there is one, there's a map)
				EObjectFlags FlagsToCook = RF_Public;
				TArray<UObject*> ObjsInPackage;
				UWorld* World = nullptr;
				{
					//UE_SCOPED_HIERARCHICAL_COOKTIMER(SaveCookedPackage_FindWorldInPackage);
					GetObjectsWithOuter(Package, ObjsInPackage, false);
					for (UObject* Obj : ObjsInPackage)
					{
						World = Cast<UWorld>(Obj);
						if (World)
						{
							FlagsToCook = RF_NoFlags;
							break;
						}
					}
				}

				const FName& PackageName = PackageData.GetPackageName();
				FString PackageNameStr = PackageName.ToString();
				bool bExcludeFromNonEditorTargets = IsCookFlagSet(ECookInitializationFlags::SkipEditorContent) && (PackageNameStr.StartsWith(TEXT("/Engine/Editor")) || PackageNameStr.StartsWith(TEXT("/Engine/VREditor")));

				uint32 OriginalPackageFlags = Package->GetPackageFlags();

				TArray<bool> SavePackageSuccessPerPlatform;
				SavePackageSuccessPerPlatform.SetNum(TargetPlatforms.Num());
				for (int32 PlatformIndex = 0; PlatformIndex < TargetPlatforms.Num(); ++PlatformIndex)
				{
					const ITargetPlatform* Target = TargetPlatforms[PlatformIndex];
					FAssetRegistryGenerator& Generator = *Generators[PlatformIndex]; 


					// don't save Editor resources from the Engine if the target doesn't have editoronly data
					bool bCookPackage = (!bExcludeFromNonEditorTargets || Target->HasEditorOnlyData());
					if (UAssetManager::IsValid() && !UAssetManager::Get().ShouldCookForPlatform(Package, Target))
					{
						bCookPackage = false;
					}

					if (bCookPackage)
					{
						FString PlatFilename = Filename.Replace(TEXT("[Platform]"), *Target->PlatformName());

						UE_CLOG(GCookProgressDisplay & (int32)ECookProgressDisplayMode::PackageNames, LogCook, Display, TEXT("Cooking %s -> %s"), *Package->GetName(), *PlatFilename);

						bool bSwap = (!Target->IsLittleEndian()) ^ (!PLATFORM_LITTLE_ENDIAN);
						if (!Target->HasEditorOnlyData())
						{
							Package->SetPackageFlags(PKG_FilterEditorOnly);
						}
						else
						{
							Package->ClearPackageFlags(PKG_FilterEditorOnly);
						}
								
						GIsCookerLoadingPackage = true;
						check(SavePackageContexts.Num() > PlatformIndex);
						FSavePackageContext& SavePackageContext = SavePackageContexts[PlatformIndex]->SaveContext;
						IPackageWriter::FBeginPackageInfo BeginInfo;
						BeginInfo.PackageName = Package->GetFName();
						BeginInfo.LooseFilePath = PlatFilename;
						SavePackageContext.PackageWriter->BeginPackage(BeginInfo);
						FSavePackageArgs SaveArgs;
						SaveArgs.TopLevelFlags = FlagsToCook;
						SaveArgs.bForceByteSwapping = bSwap;
						SaveArgs.bWarnOfLongFilename = false;
						SaveArgs.SaveFlags = SaveFlags;
						SaveArgs.TargetPlatform = Target;
						SaveArgs.bSlowTask = false;
						SaveArgs.SavePackageContext = &SavePackageContext;
						FSavePackageResultStruct SaveResult = GEditor->Save(Package, World, *PlatFilename, SaveArgs);
						GIsCookerLoadingPackage = false;

						if (SaveResult == ESavePackageResult::Success && UAssetManager::IsValid())
						{
							if (!UAssetManager::Get().VerifyCanCookPackage(this, Package->GetFName()))
							{
								SaveResult = ESavePackageResult::Error;
							}
						}

						// Update asset registry
						Generator.UpdateAssetRegistryPackageData(*Package, SaveResult, SaveResult.CookedHash);
						FAssetPackageData* AssetPackageData = Generator.GetAssetPackageData(Package->GetFName());
						check(AssetPackageData);

						ICookedPackageWriter::FCommitPackageInfo CommitInfo;
						CommitInfo.bSucceeded = SaveResult.IsSuccessful();
						CommitInfo.PackageName = Package->GetFName();
						PRAGMA_DISABLE_DEPRECATION_WARNINGS;
						CommitInfo.PackageGuid = AssetPackageData->PackageGuid;
						PRAGMA_ENABLE_DEPRECATION_WARNINGS;
						CommitInfo.WriteOptions = IPackageWriter::EWriteOptions::Write;
						SavePackageContext.PackageWriter->CommitPackage(MoveTemp(CommitInfo));

						if (SaveResult.IsSuccessful())
						{
							FPlatformAtomics::InterlockedIncrement(&ParallelSavedPackages);
						}

						if (SaveResult != ESavePackageResult::ReferencedOnlyByEditorOnlyData)
						{
							SavePackageSuccessPerPlatform[PlatformIndex] = true;
						}
						else
						{
							SavePackageSuccessPerPlatform[PlatformIndex] = false;
						}
					}
					else
					{
						SavePackageSuccessPerPlatform[PlatformIndex] = false;
					}
				}

				PackageData.SetPlatformsCooked(TargetPlatforms, SavePackageSuccessPerPlatform);

				if (SavePackageSuccessPerPlatform.Contains(false))
				{
					PackageTracker->UncookedEditorOnlyPackages.Add(PackageName);
				}

				Package->SetPackageFlagsTo(OriginalPackageFlags);
			}
		}, !bSaveConcurrent);

		if (bSaveConcurrent)
		{
			GIsSavingPackage = false;
		}

		CookedPackageCount += ParallelSavedPackages;
		if (ParallelSavedPackages > 0)
		{
			Result |= COSR_CookedPackage;
		}

		check(bIsSavingPackage == true);
		bIsSavingPackage = false;
	}

	if (bSaveConcurrent)
	{
		UE_LOG(LogCook, Display, TEXT("Calling PostSaveRoot on worlds..."));
		UE_SCOPED_HIERARCHICAL_COOKTIMER(FullLoadAndSave_PostSaveRoot);
		for (const TPair<UWorld*,TArray<bool>>& WorldIt : WorldsToPostSaveRoot)
		{
			UWorld* World = WorldIt.Key;
			const TArray<bool>& PlatformNeedsCleanup = WorldIt.Value;
			check(World);
			check(PlatformNeedsCleanup.Num() == TargetPlatforms.Num());
			for (int PlatformIndex = TargetPlatforms.Num() - 1; PlatformIndex >= 0; --PlatformIndex)
			{
				ObjectSaveContext.TargetPlatform = TargetPlatforms[PlatformIndex];
				ObjectSaveContext.bOuterConcurrentSave = PlatformIndex == 0;
				UE::SavePackageUtilities::CallPostSaveRoot(World, ObjectSaveContext, PlatformNeedsCleanup[PlatformIndex]);
			}
		}
	}

	return Result;
}

ICookedPackageWriter& UCookOnTheFlyServer::FindOrCreatePackageWriter(const ITargetPlatform* TargetPlatform)
{
	return *FindOrCreateSaveContext(TargetPlatform).PackageWriter;
}

void UCookOnTheFlyServer::FindOrCreateSaveContexts(TConstArrayView<const ITargetPlatform*> TargetPlatforms)
{
	for (const ITargetPlatform* TargetPlatform : TargetPlatforms)
	{
		FindOrCreateSaveContext(TargetPlatform);
	}
}

UE::Cook::FCookSavePackageContext& UCookOnTheFlyServer::FindOrCreateSaveContext(const ITargetPlatform* TargetPlatform)
{
	for (UE::Cook::FCookSavePackageContext* Context : SavePackageContexts)
	{
		if (Context->SaveContext.TargetPlatform == TargetPlatform)
		{
			return *Context;
		}
	}
	return *SavePackageContexts.Add_GetRef(CreateSaveContext(TargetPlatform));
}


#undef LOCTEXT_NAMESPACE
