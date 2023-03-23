// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreTypes.h"
#include "Containers/StringView.h"
#include "DerivedDataBackendInterface.h"
#include "DerivedDataCachePrivate.h"
#include "DerivedDataCacheStore.h"
#include "DerivedDataCacheUsageStats.h"
#include "DerivedDataRequestOwner.h"
#include "HAL/CriticalSection.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/ThreadSafeCounter.h"
#include "Internationalization/FastDecimalFormat.h"
#include "Math/BasicMathExpressionEvaluator.h"
#include "Math/UnitConversion.h"
#include "MemoryCacheStore.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/EngineBuildSettings.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Misc/StringBuilder.h"
#include "Modules/ModuleManager.h"
#include "PakFileCacheStore.h"
#include "ProfilingDebugging/CookStats.h"
#include "Serialization/CompactBinaryPackage.h"
#include <atomic>

DEFINE_LOG_CATEGORY(LogDerivedDataCache);

#define MAX_BACKEND_KEY_LENGTH (120)
#define LOCTEXT_NAMESPACE "DerivedDataBackendGraph"

static TAutoConsoleVariable<FString> GDerivedDataCacheGraphName(
	TEXT("DDC.Graph"),
	TEXT("Default"),
	TEXT("Name of the graph to use for the Derived Data Cache."),
	ECVF_ReadOnly);

namespace UE::DerivedData
{

ILegacyCacheStore* CreateCacheStoreAsync(ILegacyCacheStore* InnerBackend, ECacheStoreFlags InnerFlags, IMemoryCacheStore* MemoryCache);
ILegacyCacheStore* CreateCacheStoreHierarchy(ICacheStoreOwner*& OutOwner, IMemoryCacheStore* MemoryCache);
ILegacyCacheStore* CreateCacheStoreThrottle(ILegacyCacheStore* InnerCache, uint32 LatencyMS, uint32 MaxBytesPerSecond);
ILegacyCacheStore* CreateCacheStoreVerify(ILegacyCacheStore* InnerCache, bool bPutOnError);
ILegacyCacheStore* CreateFileSystemCacheStore(const TCHAR* CacheDirectory, const TCHAR* Params, const TCHAR* AccessLogFileName, ECacheStoreFlags& OutFlags);
ILegacyCacheStore* CreateHttpCacheStore(
	const TCHAR* NodeName,
	const TCHAR* ServiceUrl,
	const TCHAR* Namespace,
	const TCHAR* StructuredNamespace,
	const TCHAR* OAuthProvider,
	const TCHAR* OAuthClientId,
	const TCHAR* OAuthData, 
	const FDerivedDataBackendInterface::ESpeedClass* ForceSpeedClass,
	EBackendLegacyMode LegacyMode,
	bool bReadOnly);
IMemoryCacheStore* CreateMemoryCacheStore(const TCHAR* Name, int64 MaxCacheSize, bool bCanBeDisabled);
IPakFileCacheStore* CreatePakFileCacheStore(const TCHAR* Filename, bool bWriting, bool bCompressed);
ILegacyCacheStore* CreateS3CacheStore(const TCHAR* RootManifestPath, const TCHAR* BaseUrl, const TCHAR* Region, const TCHAR* CanaryObjectKey, const TCHAR* CachePath);
ILegacyCacheStore* CreateZenCacheStore(const TCHAR* NodeName, const TCHAR* ServiceUrl, const TCHAR* Namespace);

/**
 * This class is used to create a singleton that represents the derived data cache hierarchy and all of the wrappers necessary
 * ideally this would be data driven and the backends would be plugins...
 */
class FDerivedDataBackendGraph final : public FDerivedDataBackend
{
public:
	using FParsedNode = TPair<ILegacyCacheStore*, ECacheStoreFlags>;
	using FParsedNodeMap = TMap<FString, FParsedNode>;

	/**
	 * constructor, builds the cache tree
	 */
	FDerivedDataBackendGraph()
		: RootCache(nullptr)
		, MemoryCache(nullptr)
		, BootCache(nullptr)
		, WritePakCache(nullptr)
		, AsyncNode(nullptr)
		, Hierarchy(nullptr)
		, bUsingSharedDDC(false)
		, bIsShuttingDown(false)
		, MountPakCommand(
			TEXT("DDC.MountPak"),
			*LOCTEXT("CommandText_DDCMountPak", "Mounts read-only pak file").ToString(),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FDerivedDataBackendGraph::MountPakCommandHandler))
		, UnountPakCommand(
			TEXT("DDC.UnmountPak"),
			*LOCTEXT("CommandText_DDCUnmountPak", "Unmounts read-only pak file").ToString(),
			FConsoleCommandWithArgsDelegate::CreateRaw(this, &FDerivedDataBackendGraph::UnmountPakCommandHandler))
	{
		check(!StaticGraph);
		StaticGraph = this;

		check(IsInGameThread()); // we pretty much need this to be initialized from the main thread...it uses GConfig, etc
		check(GConfig && GConfig->IsReadyForUse());
		RootCache = nullptr;
		FParsedNodeMap ParsedNodes;

		FParsedNode RootNode(nullptr, ECacheStoreFlags::None);

		// Create the graph using ini settings. The string "default" forwards creation to use the default graph.

		if (!FParse::Value(FCommandLine::Get(), TEXT("-DDC="), GraphName))
		{
			GraphName = GDerivedDataCacheGraphName.GetValueOnGameThread();
		}

		if (GraphName == TEXT("None"))
		{
			UE_LOG(LogDerivedDataCache, Display, TEXT("Requested cache graph of 'None'. Every cache operation will fail."));
		}
		else
		{
			if (!GraphName.IsEmpty() && GraphName != TEXT("Default"))
			{
				RootNode = ParseNode(TEXT("Root"), GEngineIni, *GraphName, ParsedNodes);

				if (!RootNode.Key)
				{
					// Destroy any cache stores that have been created.
					ParsedNodes.Empty();
					DestroyCreatedBackends();
					UE_LOG(LogDerivedDataCache, Warning,
						TEXT("Unable to create cache graph using the requested graph settings (%s). "),
						TEXT("Reverting to the default graph."), *GraphName);
				}
			}

			if (!RootNode.Key)
			{
				// Try to use the default graph.
				GraphName = FApp::IsEngineInstalled() ? TEXT("InstalledDerivedDataBackendGraph") : TEXT("DerivedDataBackendGraph");
				FString Entry;
				if (!GConfig->GetString(*GraphName, TEXT("Root"), Entry, GEngineIni) || !Entry.Len())
				{
					UE_LOG(LogDerivedDataCache, Fatal,
						TEXT("Unable to create cache graph using the default graph settings (%s) ini=%s."),
						*GraphName, *GEngineIni);
				}
				RootNode = ParseNode(TEXT("Root"), GEngineIni, *GraphName, ParsedNodes);
				if (!RootNode.Key)
				{
					UE_LOG(LogDerivedDataCache, Fatal,
						TEXT("Unable to create cache graph using the default graph settings (%s) ini=%s. ")
						TEXT("At least one cache store in the graph must be available."),
						*GraphName, *GEngineIni);
				}
			}
		}

		// Hierarchy must exist in the graph.
		if (!Hierarchy)
		{
			ILegacyCacheStore* HierarchyStore = CreateCacheStoreHierarchy(Hierarchy, GetMemoryCache());
			if (RootNode.Key)
			{
				Hierarchy->Add(RootNode.Key, RootNode.Value);
			}
			CreatedNodes.AddUnique(HierarchyStore);
			RootNode.Key = HierarchyStore;
		}

		// Async must exist in the graph.
		if (!AsyncNode)
		{
			AsyncNode = CreateCacheStoreAsync(RootNode.Key, RootNode.Value, GetMemoryCache());
			CreatedNodes.AddUnique(AsyncNode);
			RootNode.Key = AsyncNode;
		}

		if (MaxKeyLength == 0)
		{
			MaxKeyLength = MAX_BACKEND_KEY_LENGTH;
		}

		RootCache = RootNode.Key;
	}

	/**
	 * Helper function to get the value of parsed bool as the return value
	 **/
	bool GetParsedBool( const TCHAR* Stream, const TCHAR* Match ) const
	{
		bool bValue = 0;
		FParse::Bool( Stream, Match, bValue );
		return bValue;
	}

	/**
	 * Parses backend graph node from ini settings
	 *
	 * @param NodeName Name of the node to parse
	 * @param IniFilename Ini filename
	 * @param IniSection Section in the ini file containing the graph definition
	 * @param InParsedNodes Map of parsed nodes and their names to be able to find already parsed nodes
	 * @return Derived data backend interface instance created from ini settings
	 */
	FParsedNode ParseNode(const FString& NodeName, const FString& IniFilename, const TCHAR* IniSection, FParsedNodeMap& InParsedNodes)
	{
		if (const FParsedNode* ParsedNode = InParsedNodes.Find(NodeName))
		{
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Node %s was referenced more than once in the graph. Nodes may not be shared."), *NodeName);
			return *ParsedNode;
		}

		FParsedNode ParsedNode(nullptr, ECacheStoreFlags::None);
		FString Entry;
		if (GConfig->GetString(IniSection, *NodeName, Entry, IniFilename))
		{
			Entry.TrimStartInline();
			Entry.RemoveFromStart(TEXT("("));
			Entry.RemoveFromEnd(TEXT(")"));

			FString	NodeType;
			if (FParse::Value(*Entry, TEXT("Type="), NodeType))
			{
				if (NodeType == TEXT("FileSystem"))
				{
					ParsedNode = ParseDataCache(*NodeName, *Entry);
				}
				else if (NodeType == TEXT("Boot"))
				{
					UE_LOG(LogDerivedDataCache, Display, TEXT("Boot nodes are deprecated. Please remove the Boot node from the cache graph."));
					if (BootCache == nullptr)
					{
						BootCache = ParseBootCache(*NodeName, *Entry);
						ParsedNode = MakeTuple(BootCache, ECacheStoreFlags::Local | ECacheStoreFlags::Query | ECacheStoreFlags::Store);
					}
					else
					{
						UE_LOG(LogDerivedDataCache, Warning, TEXT("Unable to create %s Boot cache because only one Boot node is supported."), *NodeName);
					}
				}
				else if (NodeType == TEXT("Memory"))
				{
					ParsedNode = MakeTuple(ParseMemoryCache(*NodeName, *Entry), ECacheStoreFlags::Local | ECacheStoreFlags::Query | ECacheStoreFlags::Store);
				}
				else if (NodeType == TEXT("Hierarchical"))
				{
					ParsedNode = ParseHierarchyNode(*NodeName, *Entry, IniFilename, IniSection, InParsedNodes);
				}
				else if (NodeType == TEXT("KeyLength"))
				{
					if (MaxKeyLength == 0)
					{
						ParsedNode = ParseKeyLength(*NodeName, *Entry, IniFilename, IniSection, InParsedNodes);
					}
					else
					{
						UE_LOG(LogDerivedDataCache, Warning, TEXT("Unable to create %s KeyLength node because only one KeyLength node is supported."), *NodeName);
					}
				}
				else if (NodeType == TEXT("AsyncPut"))
				{
					if (AsyncNode == nullptr)
					{
						ParsedNode = ParseAsyncNode(*NodeName, *Entry, IniFilename, IniSection, InParsedNodes);
						AsyncNode = ParsedNode.Key;
					}
					else
					{
						UE_LOG(LogDerivedDataCache, Warning, TEXT("Unable to create %s AsyncPut because only one AsyncPut node is supported."), *NodeName);
					}
				}
				else if (NodeType == TEXT("Verify"))
				{
					ParsedNode = ParseVerify(*NodeName, *Entry, IniFilename, IniSection, InParsedNodes);
				}
				else if (NodeType == TEXT("ReadPak"))
				{
					ParsedNode = MakeTuple(ParsePak(*NodeName, *Entry, /*bWriting*/ false), ECacheStoreFlags::Local | ECacheStoreFlags::Query | ECacheStoreFlags::StopStore);
				}
				else if (NodeType == TEXT("WritePak"))
				{
					ParsedNode = MakeTuple(ParsePak(*NodeName, *Entry, /*bWriting*/ true), ECacheStoreFlags::Local | ECacheStoreFlags::Store);
				}
				else if (NodeType == TEXT("S3"))
				{
					ParsedNode = MakeTuple(ParseS3Cache(*NodeName, *Entry), ECacheStoreFlags::Local | ECacheStoreFlags::Query | ECacheStoreFlags::StopStore);
				}
				else if (NodeType == TEXT("Http"))
				{
					ParsedNode = ParseHttpCache(*NodeName, *Entry, IniFilename, IniSection);
				}
				else if (NodeType == TEXT("Zen"))
				{
					ParsedNode = MakeTuple(ParseZenCache(*NodeName, *Entry), ECacheStoreFlags::Local | ECacheStoreFlags::Remote | ECacheStoreFlags::Query | ECacheStoreFlags::Store);
				}
				
				if (ParsedNode.Key)
				{
					// Add a throttling layer if parameters are found
					uint32 LatencyMS = 0;
					FParse::Value(*Entry, TEXT("LatencyMS="), LatencyMS);

					uint32 MaxBytesPerSecond = 0;
					FParse::Value(*Entry, TEXT("MaxBytesPerSecond="), MaxBytesPerSecond);

					if (LatencyMS != 0 || MaxBytesPerSecond != 0)
					{
						CreatedNodes.AddUnique(ParsedNode.Key);
						ParsedNode = MakeTuple(CreateCacheStoreThrottle(ParsedNode.Key, LatencyMS, MaxBytesPerSecond), ParsedNode.Value);
					}
				}
			}
		}

		if (ParsedNode.Key)
		{
			// Store this node so that we don't require any order of adding nodes
			InParsedNodes.Add(NodeName, ParsedNode);
			// Keep references to all created nodes.
			CreatedNodes.AddUnique(ParsedNode.Key);

			// Parse any debug options for this node. E.g. -DDC-<Name>-MissRate
			FDerivedDataBackendInterface::FBackendDebugOptions DebugOptions;
			if (FDerivedDataBackendInterface::FBackendDebugOptions::ParseFromTokens(DebugOptions, *NodeName, FCommandLine::Get()))
			{
				if (!ParsedNode.Key->LegacyDebugOptions(DebugOptions))
				{
					UE_LOG(LogDerivedDataCache, Warning, TEXT("Node %s is ignoring one or more -DDC-<NodeName>-Option debug options"), *NodeName);
				}
			}
		}

		return ParsedNode;
	}

	/**
	 * Creates Read/write Pak file interface from ini settings
	 *
	 * @param NodeName Node name
	 * @param Entry Node definition
	 * @param bWriting true to create pak interface for writing
	 * @return Pak file data backend interface instance or nullptr if unsuccessful
	 */
	ILegacyCacheStore* ParsePak( const TCHAR* NodeName, const TCHAR* Entry, const bool bWriting )
	{
		ILegacyCacheStore* PakNode = nullptr;
		FString PakFilename;
		FParse::Value( Entry, TEXT("Filename="), PakFilename );
		bool bCompressed = GetParsedBool(Entry, TEXT("Compressed="));

		if( !PakFilename.Len() )
		{
			UE_LOG( LogDerivedDataCache, Log, TEXT("FDerivedDataBackendGraph:  %s pak cache Filename not found in *engine.ini, will not use a pak cache."), NodeName );
		}
		else
		{
			// now add the pak read cache (if any) to the front of the cache hierarchy
			if ( bWriting )
			{
				FGuid Temp = FGuid::NewGuid();
				ReadPakFilename = PakFilename;
				WritePakFilename = PakFilename + TEXT(".") + Temp.ToString();
				WritePakCache = CreatePakFileCacheStore(*WritePakFilename, /*bWriting*/ true, bCompressed);
				PakNode = WritePakCache;
			}
			else
			{
				bool bReadPak = FPlatformFileManager::Get().GetPlatformFile().FileExists( *PakFilename );
				if( bReadPak )
				{
					IPakFileCacheStore* ReadPak = CreatePakFileCacheStore(*PakFilename, /*bWriting*/ false, bCompressed);
					ReadPakFilename = PakFilename;
					PakNode = ReadPak;
					ReadPakCache.Add(ReadPak);
				}
				else
				{
					UE_LOG( LogDerivedDataCache, Log, TEXT("FDerivedDataBackendGraph:  %s pak cache file %s not found, will not use a pak cache."), NodeName, *PakFilename );
				}
			}
		}
		return PakNode;
	}

	/**
	 * Creates Verify wrapper interface from ini settings.
	 *
	 * @param NodeName Node name.
	 * @param Entry Node definition.
	 * @param IniFilename ini filename.
	 * @param IniSection ini section containing graph definition
	 * @param InParsedNodes map of nodes and their names which have already been parsed
	 * @return Verify wrapper backend interface instance or nullptr if unsuccessful
	 */
	FParsedNode ParseVerify(const TCHAR* NodeName, const TCHAR* Entry, const FString& IniFilename, const TCHAR* IniSection, FParsedNodeMap& InParsedNodes)
	{
		FParsedNode InnerNode(nullptr, ECacheStoreFlags::None);
		FString InnerName;
		if (FParse::Value(Entry, TEXT("Inner="), InnerName))
		{
			InnerNode = ParseNode(InnerName, IniFilename, IniSection, InParsedNodes);
		}

		if (InnerNode.Key)
		{
			IFileManager::Get().DeleteDirectory(*(FPaths::ProjectSavedDir() / TEXT("VerifyDDC/")), /*bRequireExists*/ false, /*bTree*/ true);

			const bool bFix = GetParsedBool(Entry, TEXT("Fix="));
			InnerNode = MakeTuple(CreateCacheStoreVerify(InnerNode.Key, bFix), InnerNode.Value);
		}
		else
		{
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Unable to find inner node %s for Verify node %s. Verify node will not be created."), *InnerName, NodeName);
		}

		return InnerNode;
	}

	/**
	 * Creates AsyncPut wrapper interface from ini settings.
	 *
	 * @param NodeName Node name.
	 * @param Entry Node definition.
	 * @param IniFilename ini filename.
	 * @param IniSection ini section containing graph definition
	 * @param InParsedNodes map of nodes and their names which have already been parsed
	 * @return AsyncPut wrapper backend interface instance or nullptr if unsuccessful
	 */
	FParsedNode ParseAsyncNode(const TCHAR* NodeName, const TCHAR* Entry, const FString& IniFilename, const TCHAR* IniSection, FParsedNodeMap& InParsedNodes)
	{
		FParsedNode InnerNode(nullptr, ECacheStoreFlags::None);
		FString InnerName;
		if (FParse::Value(Entry, TEXT("Inner="), InnerName))
		{
			InnerNode = ParseNode(InnerName, IniFilename, IniSection, InParsedNodes);
		}

		if (InnerNode.Key)
		{
			if (!Hierarchy)
			{
				ILegacyCacheStore* HierarchyStore = CreateCacheStoreHierarchy(Hierarchy, GetMemoryCache());
				Hierarchy->Add(InnerNode.Key, InnerNode.Value);
				CreatedNodes.AddUnique(HierarchyStore);
				InnerNode.Key = HierarchyStore;
			}

			ILegacyCacheStore* AsyncStore = CreateCacheStoreAsync(InnerNode.Key, InnerNode.Value, GetMemoryCache());
			InnerNode = MakeTuple(AsyncStore, InnerNode.Value);
		}
		else
		{
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Unable to find inner node %s for AsyncPut node %s. AsyncPut node will not be created."), *InnerName, NodeName);
		}

		return InnerNode;
	}

	/**
	 * Creates KeyLength wrapper interface from ini settings.
	 *
	 * @param NodeName Node name.
	 * @param Entry Node definition.
	 * @param IniFilename ini filename.
	 * @param IniSection ini section containing graph definition
	 * @param InParsedNodes map of nodes and their names which have already been parsed
	 * @return KeyLength wrapper backend interface instance or nullptr if unsuccessful
	 */
	FParsedNode ParseKeyLength(const TCHAR* NodeName, const TCHAR* Entry, const FString& IniFilename, const TCHAR* IniSection, FParsedNodeMap& InParsedNodes)
	{
		if (MaxKeyLength)
		{
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Node %s is disabled because there may be only one key length node."), NodeName);
			return MakeTuple(nullptr, ECacheStoreFlags::None);
		}

		FParsedNode InnerNode(nullptr, ECacheStoreFlags::None);
		FString InnerName;
		if (FParse::Value(Entry, TEXT("Inner="), InnerName))
		{
			InnerNode = ParseNode(InnerName, IniFilename, IniSection, InParsedNodes);
		}

		if (InnerNode.Key)
		{
			int32 KeyLength = MAX_BACKEND_KEY_LENGTH;
			FParse::Value(Entry, TEXT("Length="), KeyLength);
			MaxKeyLength = FMath::Clamp(KeyLength, 0, MAX_BACKEND_KEY_LENGTH);
		}
		else
		{
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Unable to find inner node %s for KeyLength node %s. KeyLength node will not be created."), *InnerName, NodeName);
		}

		return InnerNode;
	}

	/**
	 * Creates Hierarchical interface from ini settings.
	 *
	 * @param NodeName Node name.
	 * @param Entry Node definition.
	 * @param IniFilename ini filename.
	 * @param IniSection ini section containing graph definition
	 * @param InParsedNodes map of nodes and their names which have already been parsed
	 * @return Hierarchical backend interface instance or nullptr if unsuccessful
	 */
	FParsedNode ParseHierarchyNode(const TCHAR* NodeName, const TCHAR* Entry, const FString& IniFilename, const TCHAR* IniSection, FParsedNodeMap& InParsedNodes)
	{
		const TCHAR* InnerMatch = TEXT("Inner=");
		const int32 InnerMatchLength = FCString::Strlen(InnerMatch);

		TArray<FParsedNode> InnerNodes;
		FString InnerName;
		while (FParse::Value(Entry, InnerMatch, InnerName))
		{
			FParsedNode InnerNode = ParseNode(InnerName, IniFilename, IniSection, InParsedNodes);
			if (InnerNode.Key)
			{
				InnerNodes.Add(InnerNode);
			}
			else
			{
				UE_LOG(LogDerivedDataCache, Log, TEXT("Unable to find inner node %s for hierarchy %s."), *InnerName, NodeName);
			}

			// Move the Entry pointer forward so that we can find more children
			Entry = FCString::Strifind(Entry, InnerMatch);
			Entry += InnerMatchLength;
		}

		if (InnerNodes.IsEmpty())
		{
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Hierarchical cache %s has no inner backends and will not be created."), NodeName);
			return MakeTuple(nullptr, ECacheStoreFlags::None);
		}

		if (Hierarchy)
		{
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Node %s is disabled because there may be only one hierarchy node. ")
				TEXT("Confirm there is only one hierarchy in the cache graph and that it is inside of any async node."), NodeName);
			return MakeTuple(nullptr, ECacheStoreFlags::None);
		}

		ILegacyCacheStore* HierarchyStore = CreateCacheStoreHierarchy(Hierarchy, GetMemoryCache());
		ECacheStoreFlags Flags = ECacheStoreFlags::None;
		for (const FParsedNode& Node : InnerNodes)
		{
			Hierarchy->Add(Node.Key, Node.Value);
			Flags |= Node.Value;
		}
		return MakeTuple(HierarchyStore, Flags & ~ECacheStoreFlags::StopStore);
	}

	/**
	 * Creates Filesystem data cache interface from ini settings.
	 *
	 * @param NodeName Node name.
	 * @param Entry Node definition.
	 * @return Filesystem data cache backend interface instance or nullptr if unsuccessful
	 */
	FParsedNode ParseDataCache( const TCHAR* NodeName, const TCHAR* Entry )
	{
		FParsedNode DataCache(nullptr, ECacheStoreFlags::None);

		// Parse Path by default, it may be overwritten by EnvPathOverride
		FString Path;
		FParse::Value( Entry, TEXT("Path="), Path );

		// Check the EnvPathOverride environment variable to allow persistent overriding of data cache path, eg for offsite workers.
		FString EnvPathOverride;
		if( FParse::Value( Entry, TEXT("EnvPathOverride="), EnvPathOverride ) )
		{
			FString FilesystemCachePathEnv = FPlatformMisc::GetEnvironmentVariable( *EnvPathOverride );
			if( FilesystemCachePathEnv.Len() > 0 )
			{
				Path = FilesystemCachePathEnv;
				UE_LOG( LogDerivedDataCache, Log, TEXT("Found environment variable %s=%s"), *EnvPathOverride, *Path );
			}
		}

		if (!EnvPathOverride.IsEmpty())
		{
			FString DDCPath;
			if (FPlatformMisc::GetStoredValue(TEXT("Epic Games"), TEXT("GlobalDataCachePath"), *EnvPathOverride, DDCPath))
			{
				if (DDCPath.Len() > 0)
				{
					Path = DDCPath;
					UE_LOG( LogDerivedDataCache, Log, TEXT("Found registry key GlobalDataCachePath %s=%s"), *EnvPathOverride, *Path );
				}
			}
		}

		// Check the CommandLineOverride argument to allow redirecting in build scripts
		FString CommandLineOverride;
		if( FParse::Value( Entry, TEXT("CommandLineOverride="), CommandLineOverride ) )
		{
			FString Value;
			if (FParse::Value(FCommandLine::Get(), *(CommandLineOverride + TEXT("=")), Value))
			{
				Path = Value;
				UE_LOG(LogDerivedDataCache, Log, TEXT("Found command line override %s=%s"), *CommandLineOverride, *Path);
			}
		}

		// Paths starting with a '?' are looked up from config
		if (Path.StartsWith(TEXT("?")) && !GConfig->GetString(TEXT("DerivedDataCacheSettings"), *Path + 1, Path, GEngineIni))
		{
			Path.Empty();
		}

		// Allow the user to override it from the editor
		FString EditorOverrideSetting;
		if(FParse::Value(Entry, TEXT("EditorOverrideSetting="), EditorOverrideSetting))
		{
			FString Setting = GConfig->GetStr(TEXT("/Script/UnrealEd.EditorSettings"), *EditorOverrideSetting, GEditorSettingsIni);
			if(Setting.Len() > 0)
			{
				FString SettingPath;
				if(FParse::Value(*Setting, TEXT("Path="), SettingPath))
				{
					SettingPath = SettingPath.TrimQuotes();
					if(SettingPath.Len() > 0)
					{
						Path = SettingPath;
					}
				}
			}
		}

		if( !Path.Len() )
		{
			UE_LOG( LogDerivedDataCache, Log, TEXT("%s data cache path not found in *engine.ini, will not use an %s cache."), NodeName, NodeName );
		}
		else if( Path == TEXT("None") )
		{
			UE_LOG( LogDerivedDataCache, Log, TEXT("Disabling %s data cache - path set to 'None'."), NodeName );
		}
		else
		{
			// Try to set up the shared drive, allow user to correct any issues that may exist.
			bool RetryOnFailure = false;
			do
			{
				RetryOnFailure = false;

				// Don't create the file system if shared data cache directory is not mounted
				bool bShared = FCString::Stricmp(NodeName, TEXT("Shared")) == 0;
				
				// parameters we read here from the ini file
				FString WriteAccessLog;
				bool bPromptIfMissing = false;

				FParse::Value(Entry, TEXT("WriteAccessLog="), WriteAccessLog);
				FParse::Bool(Entry, TEXT("PromptIfMissing="), bPromptIfMissing);

				ILegacyCacheStore* InnerFileSystem = nullptr;
				ECacheStoreFlags Flags;
				if (!bShared || IFileManager::Get().DirectoryExists(*Path))
				{
					InnerFileSystem = CreateFileSystemCacheStore(*Path, Entry, *WriteAccessLog, Flags);
				}

				if (InnerFileSystem)
				{
					bUsingSharedDDC |= bShared;
					DataCache = MakeTuple(InnerFileSystem, Flags);
					UE_LOG(LogDerivedDataCache, Log, TEXT("Using %s data cache path %s: %s"), NodeName, *Path, !EnumHasAnyFlags(Flags, ECacheStoreFlags::Store) ? TEXT("ReadOnly") : TEXT("Writable"));
					Directories.AddUnique(Path);
				}
				else
				{
					FString Message = FString::Printf(TEXT("%s data cache path (%s) is unavailable so cache will be disabled."), NodeName, *Path);
					
					UE_LOG(LogDerivedDataCache, Warning, TEXT("%s"), *Message);

					// Give the user a chance to retry incase they need to connect a network drive or something.
					if (bPromptIfMissing && !FApp::IsUnattended() && !IS_PROGRAM)
					{
						Message += FString::Printf(TEXT("\n\nRetry connection to %s?"), *Path);
						EAppReturnType::Type MessageReturn = FPlatformMisc::MessageBoxExt(EAppMsgType::YesNo, *Message, TEXT("Could not access DDC"));
						RetryOnFailure = MessageReturn == EAppReturnType::Yes;
					}
				}
			} while (RetryOnFailure);
		}

		return DataCache;
	}

	/**
	 * Creates an S3 data cache interface.
	 */
	ILegacyCacheStore* ParseS3Cache(const TCHAR* NodeName, const TCHAR* Entry)
	{
		FString ManifestPath;
		if (!FParse::Value(Entry, TEXT("Manifest="), ManifestPath))
		{
			UE_LOG(LogDerivedDataCache, Error, TEXT("Node %s does not specify 'Manifest'."), NodeName);
			return nullptr;
		}

		FString BaseUrl;
		if (!FParse::Value(Entry, TEXT("BaseUrl="), BaseUrl))
		{
			UE_LOG(LogDerivedDataCache, Error, TEXT("Node %s does not specify 'BaseUrl'."), NodeName);
			return nullptr;
		}

		FString CanaryObjectKey;
		FParse::Value(Entry, TEXT("Canary="), CanaryObjectKey);

		FString Region;
		if (!FParse::Value(Entry, TEXT("Region="), Region))
		{
			UE_LOG(LogDerivedDataCache, Error, TEXT("Node %s does not specify 'Region'."), NodeName);
			return nullptr;
		}

		// Check the EnvPathOverride environment variable to allow persistent overriding of data cache path, eg for offsite workers.
		FString EnvPathOverride;
		FString CachePath = FPaths::ProjectSavedDir() / TEXT("S3DDC");
		if (FParse::Value(Entry, TEXT("EnvPathOverride="), EnvPathOverride))
		{
			FString FilesystemCachePathEnv = FPlatformMisc::GetEnvironmentVariable(*EnvPathOverride);
			if (FilesystemCachePathEnv.Len() > 0)
			{
				if (FilesystemCachePathEnv == TEXT("None"))
				{
					UE_LOG(LogDerivedDataCache, Log, TEXT("Node %s disabled due to %s=None"), NodeName, *EnvPathOverride);
					return nullptr;
				}
				else
				{
					CachePath = FilesystemCachePathEnv;
					UE_LOG(LogDerivedDataCache, Log, TEXT("Found environment variable %s=%s"), *EnvPathOverride, *CachePath);
				}
			}

			if (!EnvPathOverride.IsEmpty())
			{
				FString DDCPath;
				if (FPlatformMisc::GetStoredValue(TEXT("Epic Games"), TEXT("GlobalDataCachePath"), *EnvPathOverride, DDCPath))
				{
					if ( DDCPath.Len() > 0 )
					{
						CachePath = DDCPath;			
						UE_LOG( LogDerivedDataCache, Log, TEXT("Found registry key GlobalDataCachePath %s=%s"), *EnvPathOverride, *CachePath );
					}
				}
			}
		}

		if (ILegacyCacheStore* Backend = CreateS3CacheStore(*ManifestPath, *BaseUrl, *Region, *CanaryObjectKey, *CachePath))
		{
			return Backend;
		}

		UE_LOG(LogDerivedDataCache, Log, TEXT("S3 backend is not supported on the current platform."));
		return nullptr;
	}

	void ParseHttpCacheParams(
		const TCHAR* NodeName,
		const TCHAR* Entry,
		const FString& IniFilename,
		const TCHAR* IniSection,
		FString& Host,
		FString& Namespace,
		FString& StructuredNamespace,
		FString& OAuthProvider,
		FString& OAuthClientId,
		FString& OAuthSecret,
		EBackendLegacyMode& LegacyMode,
		bool& bReadOnly)
	{
		FString ServerId;
		if (FParse::Value(Entry, TEXT("ServerID="), ServerId))
		{
			FString ServerEntry;
			const TCHAR* ServerSection = TEXT("HordeStorageServers");
			if (GConfig->GetString(ServerSection, *ServerId, ServerEntry, IniFilename))
			{
				ParseHttpCacheParams(NodeName, *ServerEntry, IniFilename, IniSection, Host, Namespace, StructuredNamespace, OAuthProvider, OAuthClientId, OAuthSecret, LegacyMode, bReadOnly);
			}
			else
			{
				UE_LOG(LogDerivedDataCache, Warning, TEXT("Node %s is using ServerID=%s which was not found in [%s]"), NodeName, *ServerId, ServerSection);
			}
		}

		FParse::Value(Entry, TEXT("Host="), Host);

		FString EnvHostOverride;
		if (FParse::Value(Entry, TEXT("EnvHostOverride="), EnvHostOverride))
		{
			FString HostEnv = FPlatformMisc::GetEnvironmentVariable(*EnvHostOverride);
			if (!HostEnv.IsEmpty())
			{
				Host = HostEnv;
				UE_LOG(LogDerivedDataCache, Log, TEXT("Node %s found environment variable for Host %s=%s"), NodeName, *EnvHostOverride, *Host);
			}
		}

		FString CommandLineOverride;
		if (FParse::Value(Entry, TEXT("CommandLineHostOverride="), CommandLineOverride))
		{
			if (FParse::Value(FCommandLine::Get(), *(CommandLineOverride + TEXT("=")), Host))
			{
				UE_LOG(LogDerivedDataCache, Log, TEXT("Node %s found command line override for Host %s=%s"), NodeName, *CommandLineOverride, *Host);
			}
		}

		FParse::Value(Entry, TEXT("Namespace="), Namespace);
		FParse::Value(Entry, TEXT("StructuredNamespace="), StructuredNamespace);
		FParse::Value(Entry, TEXT("OAuthProvider="), OAuthProvider);
		FParse::Value(Entry, TEXT("OAuthClientId="), OAuthClientId);
		FParse::Value(Entry, TEXT("OAuthSecret="), OAuthSecret);
		FParse::Bool(Entry, TEXT("ReadOnly="), bReadOnly);

		if (FString LegacyModeString; FParse::Value(Entry, TEXT("LegacyMode="), LegacyModeString))
		{
			if (!TryLexFromString(LegacyMode, LegacyModeString))
			{
				UE_LOG(LogDerivedDataCache, Warning, TEXT("%s: Ignoring unrecognized legacy mode '%s'"), NodeName, *LegacyModeString);
			}
		}
	}

	/**
	 * Creates a HTTP data cache interface.
	 */
	FParsedNode ParseHttpCache(
		const TCHAR* NodeName,
		const TCHAR* Entry,
		const FString& IniFilename,
		const TCHAR* IniSection)
	{
		FString Host;
		FString Namespace;
		FString StructuredNamespace;
		FString OAuthProvider;
		FString OAuthClientId;
		FString OAuthSecret;
		EBackendLegacyMode LegacyMode = EBackendLegacyMode::ValueOnly;
		bool bReadOnly = false;

		ParseHttpCacheParams(NodeName, Entry, IniFilename, IniSection, Host, Namespace, StructuredNamespace, OAuthProvider, OAuthClientId, OAuthSecret, LegacyMode, bReadOnly);

		if (Host.IsEmpty())
		{
			UE_LOG(LogDerivedDataCache, Error, TEXT("Node %s does not specify 'Host'"), NodeName);
			return MakeTuple(nullptr, ECacheStoreFlags::None);
		}

		if (Host == TEXT("None"))
		{
			UE_LOG(LogDerivedDataCache, Log, TEXT("Node %s is disabled because Host is set to 'None'"), NodeName);
			return MakeTuple(nullptr, ECacheStoreFlags::None);
		}

		if (Namespace.IsEmpty())
		{
			Namespace = FApp::GetProjectName();
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Node %s does not specify 'Namespace', falling back to '%s'"), NodeName, *Namespace);
		}

		if (StructuredNamespace.IsEmpty())
		{
			StructuredNamespace = Namespace;
		}

		if (OAuthProvider.IsEmpty())
		{
			UE_LOG(LogDerivedDataCache, Error, TEXT("Node %s does not specify 'OAuthProvider'"), NodeName);
			return MakeTuple(nullptr, ECacheStoreFlags::None);
		}

		if (OAuthClientId.IsEmpty())
		{
			UE_LOG(LogDerivedDataCache, Error, TEXT("Node %s does not specify 'OAuthClientId'"), NodeName);
			return MakeTuple(nullptr, ECacheStoreFlags::None);
		}

		if (OAuthSecret.IsEmpty())
		{
			UE_LOG(LogDerivedDataCache, Error, TEXT("Node %s does not specify 'OAuthSecret'"), NodeName);
			return MakeTuple(nullptr, ECacheStoreFlags::None);
		}

		FDerivedDataBackendInterface::ESpeedClass ForceSpeedClass = FDerivedDataBackendInterface::ESpeedClass::Unknown;
		FString ForceSpeedClassValue;
		if (FParse::Value(FCommandLine::Get(), TEXT("HttpForceSpeedClass="), ForceSpeedClassValue))
		{
			if (ForceSpeedClassValue == TEXT("Slow"))
			{
				ForceSpeedClass = FDerivedDataBackendInterface::ESpeedClass::Slow;
			}
			else if (ForceSpeedClassValue == TEXT("Ok"))
			{
				ForceSpeedClass = FDerivedDataBackendInterface::ESpeedClass::Ok;
			}
			else if (ForceSpeedClassValue == TEXT("Fast"))
			{
				ForceSpeedClass = FDerivedDataBackendInterface::ESpeedClass::Fast;
			}
			else if (ForceSpeedClassValue == TEXT("Local"))
			{
				ForceSpeedClass = FDerivedDataBackendInterface::ESpeedClass::Local;
			}
			else
			{
				UE_LOG(LogDerivedDataCache, Warning, TEXT("Node %s found unknown speed class override HttpForceSpeedClass=%s"), NodeName, *ForceSpeedClassValue);
			}
		}

		if (ForceSpeedClass != FDerivedDataBackendInterface::ESpeedClass::Unknown)
		{
			UE_LOG(LogDerivedDataCache, Log, TEXT("Node %s found speed class override ForceSpeedClass=%s"), NodeName, *ForceSpeedClassValue);
		}

		return MakeTuple(CreateHttpCacheStore(
			NodeName, *Host, *Namespace, *StructuredNamespace, *OAuthProvider, *OAuthClientId, *OAuthSecret,
			ForceSpeedClass == FDerivedDataBackendInterface::ESpeedClass::Unknown ? nullptr : &ForceSpeedClass, LegacyMode, bReadOnly),
			ECacheStoreFlags::Remote | ECacheStoreFlags::Query | (bReadOnly ? ECacheStoreFlags::None : ECacheStoreFlags::Store));
	}

	/**
	 * Creates a Zen structured data cache interface
	 */
	ILegacyCacheStore* ParseZenCache(const TCHAR* NodeName, const TCHAR* Entry)
	{
		FString ServiceUrl;
		FParse::Value(Entry, TEXT("Host="), ServiceUrl);

		FString Namespace;
		if (!FParse::Value(Entry, TEXT("Namespace="), Namespace))
		{
			Namespace = FApp::GetProjectName();
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Node %s does not specify 'Namespace', falling back to '%s'"), NodeName, *Namespace);
		}

		if (ILegacyCacheStore* Backend = CreateZenCacheStore(NodeName, *ServiceUrl, *Namespace))
		{
			return Backend;
		}
		
		UE_LOG(LogDerivedDataCache, Warning, TEXT("Zen backend is not yet supported in the current build configuration."));
		return nullptr;
	}

	/**
	 * Creates Boot data cache interface from ini settings.
	 *
	 * @param NodeName Node name.
	 * @param Entry Node definition.
	 * @param OutFilename filename specified for the cache
	 * @return Boot data cache backend interface instance or nullptr if unsuccessful
	 */
	IMemoryCacheStore* ParseBootCache(const TCHAR* NodeName, const TCHAR* Entry)
	{
		IMemoryCacheStore* Cache = nullptr;

		// Only allow boot cache with the editor. We don't want other tools and utilities (eg. SCW) writing to the same file.
#if WITH_EDITOR
		FString Filename;
		int64 MaxCacheSize = -1; // in MB
		const int64 MaxSupportedCacheSize = 2048; // 2GB

		FParse::Value(Entry, TEXT("MaxCacheSize="), MaxCacheSize);

		// make sure MaxCacheSize does not exceed 2GB
		MaxCacheSize = FMath::Min(MaxCacheSize, MaxSupportedCacheSize);

		UE_LOG(LogDerivedDataCache, Display, TEXT("%s: Max Cache Size: %d MB"), NodeName, MaxCacheSize);
		Cache = CreateMemoryCacheStore(TEXT("Boot"), MaxCacheSize * 1024 * 1024, /*bCanBeDisabled*/ true);
#endif

		return Cache;
	}

	/**
	 * Creates Memory data cache interface from ini settings.
	 *
	 * @param NodeName Node name.
	 * @param Entry Node definition.
	 * @return Memory data cache backend interface instance or nullptr if unsuccessful
	 */
	IMemoryCacheStore* ParseMemoryCache( const TCHAR* NodeName, const TCHAR* Entry )
	{
		FString Filename;
		FParse::Value(Entry, TEXT("Filename="), Filename);
		IMemoryCacheStore* Cache = CreateMemoryCacheStore(NodeName, /*MaxCacheSize*/ -1, /*bCanBeDisabled*/ false);
		if (Cache && Filename.Len())
		{
			UE_LOG(LogDerivedDataCache, Display, TEXT("Memory nodes that load from a file are deprecated. Please remove the filename from the cache configuration."));
		}
		return Cache;
	}

	IMemoryCacheStore* GetMemoryCache()
	{
		if (!MemoryCache)
		{
			MemoryCache = CreateMemoryCacheStore(TEXT("Memory"), 0, /*bCanBeDisabled*/ false);
			CreatedNodes.Add(MemoryCache);
		}
		return MemoryCache;
	}

	virtual ~FDerivedDataBackendGraph()
	{
		check(StaticGraph == this);
		RootCache = nullptr;
		DestroyCreatedBackends();
		StaticGraph = nullptr;
	}

	ILegacyCacheStore& GetRoot() override
	{
		check(RootCache);
		return *RootCache;
	}

	virtual int32 GetMaxKeyLength() const override
	{
		return MaxKeyLength;
	}

	virtual void NotifyBootComplete() override
	{
		check(RootCache);
		if (BootCache)
		{
			BootCache->Disable();
		}
	}

	virtual void WaitForQuiescence(bool bShutdown) override
	{
		double StartTime = FPlatformTime::Seconds();
		double LastPrint = StartTime;

		if (bShutdown)
		{
			bIsShuttingDown.store(true, std::memory_order_relaxed);
		}

		while (AsyncCompletionCounter.GetValue())
		{
			check(AsyncCompletionCounter.GetValue() >= 0);
			FPlatformProcess::Sleep(0.1f);
			if (FPlatformTime::Seconds() - LastPrint > 5.0)
			{
				UE_LOG(LogDerivedDataCache, Log, TEXT("Waited %ds for derived data cache to finish..."), int32(FPlatformTime::Seconds() - StartTime));
				LastPrint = FPlatformTime::Seconds();
			}
		}
		if (bShutdown)
		{
			FString MergePaks;
			if(WritePakCache && WritePakCache->IsWritable() && FParse::Value(FCommandLine::Get(), TEXT("MergePaks="), MergePaks))
			{
				TArray<FString> MergePakList;
				MergePaks.FString::ParseIntoArray(MergePakList, TEXT("+"));

				for(const FString& MergePakName : MergePakList)
				{
					TUniquePtr<IPakFileCacheStore> ReadPak(
						CreatePakFileCacheStore(*FPaths::Combine(*FPaths::GetPath(WritePakFilename), *MergePakName), /*bWriting*/ false, /*bCompressed*/ false));
					WritePakCache->MergeCache(ReadPak.Get());
				}
			}
			for (int32 ReadPakIndex = 0; ReadPakIndex < ReadPakCache.Num(); ReadPakIndex++)
			{
				ReadPakCache[ReadPakIndex]->Close();
			}
			if (WritePakCache && WritePakCache->IsWritable())
			{
				WritePakCache->Close();
				if (!FPlatformFileManager::Get().GetPlatformFile().FileExists(*WritePakFilename))
				{
					UE_LOG(LogDerivedDataCache, Error, TEXT("Pak file %s was not produced?"), *WritePakFilename);
				}
				else
				{
					if (FPlatformFileManager::Get().GetPlatformFile().FileExists(*ReadPakFilename))
					{
						FPlatformFileManager::Get().GetPlatformFile().SetReadOnly(*ReadPakFilename, false);
						if (!FPlatformFileManager::Get().GetPlatformFile().DeleteFile(*ReadPakFilename))
						{
							UE_LOG(LogDerivedDataCache, Error, TEXT("Could not delete the pak file %s to overwrite it with a new one."), *ReadPakFilename);
						}
					}
					if (!IPakFileCacheStore::SortAndCopy(WritePakFilename, ReadPakFilename))
					{
						UE_LOG(LogDerivedDataCache, Error, TEXT("Couldn't sort pak file (%s)"), *WritePakFilename);
					}
					else if (!IFileManager::Get().Delete(*WritePakFilename))
					{
						UE_LOG(LogDerivedDataCache, Error, TEXT("Couldn't delete pak file (%s)"), *WritePakFilename);
					}
					else
					{
						UE_LOG(LogDerivedDataCache, Display, TEXT("Sucessfully wrote %s."), *ReadPakFilename);
					}
				}
			}
		}
	}

	/** Get whether a shared cache is in use */
	virtual bool GetUsingSharedDDC() const override
	{
		return bUsingSharedDDC;
	}

	virtual const TCHAR* GetGraphName() const override
	{
		return *GraphName;
	}

	virtual const TCHAR* GetDefaultGraphName() const override
	{
		return FApp::IsEngineInstalled() ? TEXT("InstalledDerivedDataBackendGraph") : TEXT("DerivedDataBackendGraph");
	}

	virtual void AddToAsyncCompletionCounter(int32 Addend) override
	{
		AsyncCompletionCounter.Add(Addend);
		check(AsyncCompletionCounter.GetValue() >= 0);
	}

	virtual bool AnyAsyncRequestsRemaining() override
	{
		return AsyncCompletionCounter.GetValue() > 0;
	}

	virtual bool IsShuttingDown() override
	{
		return bIsShuttingDown.load(std::memory_order_relaxed);
	}

	virtual void GetDirectories(TArray<FString>& OutResults) override
	{
		OutResults = Directories;
	}

	static FORCEINLINE FDerivedDataBackendGraph& Get()
	{
		check(StaticGraph);
		return *StaticGraph;
	}

	virtual FDerivedDataBackendInterface* MountPakFile(const TCHAR* PakFilename) override
	{
		// Assumptions: there's at least one read-only pak backend in the hierarchy
		// and its parent is a hierarchical backend.
		IPakFileCacheStore* ReadPak = nullptr;
		if (Hierarchy && FPlatformFileManager::Get().GetPlatformFile().FileExists(PakFilename))
		{
			ReadPak = CreatePakFileCacheStore(PakFilename, /*bWriting*/ false, /*bCompressed*/ false);

			Hierarchy->Add(ReadPak, ECacheStoreFlags::Local | ECacheStoreFlags::Query | ECacheStoreFlags::StopStore);
			CreatedNodes.AddUnique(ReadPak);
			ReadPakCache.Add(ReadPak);
		}
		else
		{
			UE_LOG(LogDerivedDataCache, Warning, TEXT("Failed to add %s read-only pak DDC backend. Make sure it exists and there's at least one hierarchical backend in the cache tree."), PakFilename);
		}

		return ReadPak;
	}

	virtual bool UnmountPakFile(const TCHAR* PakFilename) override
	{
		for (int PakIndex = 0; PakIndex < ReadPakCache.Num(); ++PakIndex)
		{
			IPakFileCacheStore* ReadPak = ReadPakCache[PakIndex];
			if (ReadPak->GetFilename() == PakFilename)
			{
				check(Hierarchy);

				// Wait until all async requests are complete.
				WaitForQuiescence(false);

				Hierarchy->RemoveNotSafe(ReadPak);
				ReadPakCache.RemoveAt(PakIndex);
				CreatedNodes.Remove(ReadPak);
				ReadPak->Close();
				delete ReadPak;
				return true;
			}
		}
		return false;
	}

	virtual TSharedRef<FDerivedDataCacheStatsNode> GatherUsageStats() const override
	{
		TSharedRef<FDerivedDataCacheStatsNode> Stats = MakeShared<FDerivedDataCacheStatsNode>();
		if (RootCache)
		{
			RootCache->LegacyStats(Stats.Get());
		}
		return Stats;
	}

private:

	/** Delete all created backends in the reversed order they were created. */
	void DestroyCreatedBackends()
	{
		for (int32 BackendIndex = CreatedNodes.Num() - 1; BackendIndex >= 0; --BackendIndex)
		{
			delete CreatedNodes[BackendIndex];
		}
		CreatedNodes.Empty();
	}

	/** MountPak console command handler. */
	void UnmountPakCommandHandler(const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogDerivedDataCache, Log, TEXT("Usage: DDC.MountPak PakFilename"));
			return;
		}
		UnmountPakFile(*Args[0]);
	}

	/** UnmountPak console command handler. */
	void MountPakCommandHandler(const TArray<FString>& Args)
	{
		if (Args.Num() < 1)
		{
			UE_LOG(LogDerivedDataCache, Log, TEXT("Usage: DDC.UnmountPak PakFilename"));
			return;
		}
		MountPakFile(*Args[0]);
	}

	static inline FDerivedDataBackendGraph*			StaticGraph;

	FThreadSafeCounter								AsyncCompletionCounter;
	FString											GraphName;
	FString											ReadPakFilename;
	FString											WritePakFilename;

	/** Root of the graph */
	ILegacyCacheStore*					RootCache;

	/** References to all created backed interfaces */
	TArray< ILegacyCacheStore* > CreatedNodes;

	/** Instances of backend interfaces which exist in only one copy */
	IMemoryCacheStore* MemoryCache;
	IMemoryCacheStore*	BootCache;
	IPakFileCacheStore* WritePakCache;
	ILegacyCacheStore*	AsyncNode;
	ICacheStoreOwner* Hierarchy;
	/** Support for multiple read only pak files. */
	TArray<IPakFileCacheStore*> ReadPakCache;

	/** List of directories used by the DDC */
	TArray<FString> Directories;

	int32 MaxKeyLength = 0;

	/** Whether a shared cache is in use */
	bool bUsingSharedDDC;

	/** Whether a shutdown is pending */
	std::atomic<bool> bIsShuttingDown;

	/** MountPak console command */
	FAutoConsoleCommand MountPakCommand;
	/** UnmountPak console command */
	FAutoConsoleCommand UnountPakCommand;
};

} // UE::DerivedData

namespace UE::DerivedData
{

void FDerivedDataBackendInterface::LegacyPut(
	const TConstArrayView<FLegacyCachePutRequest> Requests,
	IRequestOwner& Owner,
	FOnLegacyCachePutComplete&& OnComplete)
{
	if (GetLegacyMode() != EBackendLegacyMode::LegacyOnly)
	{
		return ILegacyCacheStore::LegacyPut(Requests, Owner, MoveTemp(OnComplete));
	}

	for (const FLegacyCachePutRequest& Request : Requests)
	{
		FCompositeBuffer CompositeValue = Request.Value.GetRawData();
		Request.Key.WriteValueTrailer(CompositeValue);

		checkf(CompositeValue.GetSize() < MAX_int32,
			TEXT("Value is 2 GiB or greater, which is not supported for put of '%s' from '%s'"),
			*Request.Key.GetFullKey(), *Request.Name);
	
		UE_CLOG(Request.Key.HasShortKey(), LogDerivedDataCache, VeryVerbose,
			TEXT("ShortenKey %s -> %s"), *Request.Key.GetFullKey(), *Request.Key.GetShortKey());

		FSharedBuffer Value = MoveTemp(CompositeValue).ToShared();
		const TArrayView<const uint8> Data(MakeArrayView(static_cast<const uint8*>(Value.GetData()), int32(Value.GetSize())));
		const EPutStatus Status = PutCachedData(*Request.Key.GetShortKey(), Data, /*bPutEvenIfExists*/ false);
		OnComplete({Request.Name, Request.Key, Request.UserData, Status == EPutStatus::Cached ? EStatus::Ok : EStatus::Error});
	}
}

void FDerivedDataBackendInterface::LegacyGet(
	TConstArrayView<FLegacyCacheGetRequest> Requests,
	IRequestOwner& Owner,
	FOnLegacyCacheGetComplete&& OnComplete)
{
	const EBackendLegacyMode LegacyMode = GetLegacyMode();
	if (LegacyMode == EBackendLegacyMode::ValueOnly)
	{
		return ILegacyCacheStore::LegacyGet(Requests, Owner, MoveTemp(OnComplete));
	}

	// Make a blocking query to the value cache and fall back to the legacy cache for requests with errors.

	TArray<FLegacyCacheGetRequest> LegacyRequests;
	if (LegacyMode == EBackendLegacyMode::ValueWithLegacyFallback)
	{
		TArray<FLegacyCacheGetRequest, TInlineAllocator<8>> ValueRequests;
		ValueRequests.Reserve(Requests.Num());
		uint64 RequestIndex = 0;
		for (const FLegacyCacheGetRequest& Request : Requests)
		{
			ValueRequests.Add_GetRef(Request).UserData = RequestIndex++;
		}

		FRequestOwner BlockingOwner(EPriority::Blocking);
		ILegacyCacheStore::LegacyGet(ValueRequests, BlockingOwner, [this, &OnComplete, &Requests, &ValueRequests](FLegacyCacheGetResponse&& Response)
		{
			if (Response.Status != EStatus::Error)
			{
				const int32 Index = int32(Response.UserData);
				Response.UserData = Requests[Index].UserData;
				ValueRequests[Index].UserData = MAX_uint64;
				OnComplete(MoveTemp(Response));
			}
		});
		BlockingOwner.Wait();

		for (const FLegacyCacheGetRequest& Request : ValueRequests)
		{
			if (Request.UserData != MAX_uint64)
			{
				LegacyRequests.Add(Requests[int32(Request.UserData)]);
			}
		}
		if (LegacyRequests.IsEmpty())
		{
			return;
		}
		Requests = LegacyRequests;
	}

	// Query the legacy cache by translating the requests to legacy cache functions.

	FRequestOwner AsyncOwner(FPlatformMath::Min(Owner.GetPriority(), EPriority::Highest));
	FRequestBarrier Barrier(AsyncOwner);
	AsyncOwner.KeepAlive();

	TArray<FString, TInlineAllocator<8>> ExistsKeys;
	TArray<const FLegacyCacheGetRequest*, TInlineAllocator<8>> ExistsRequests;

	TArray<FString, TInlineAllocator<8>> PrefetchKeys;
	TArray<const FLegacyCacheGetRequest*, TInlineAllocator<8>> PrefetchRequests;

	for (const FLegacyCacheGetRequest& Request : Requests)
	{
		if (!EnumHasAnyFlags(Request.Policy, ECachePolicy::Query))
		{
			OnComplete({Request.Name, Request.Key, {}, Request.UserData, EStatus::Error});
		}
		else if (EnumHasAnyFlags(Request.Policy, ECachePolicy::SkipData))
		{
			const bool bExists = !EnumHasAnyFlags(Request.Policy, ECachePolicy::Store);
			(bExists ? ExistsKeys : PrefetchKeys).Emplace(Request.Key.GetShortKey());
			(bExists ? ExistsRequests : PrefetchRequests).Add(&Request);
		}
		else
		{
			FSharedBuffer Value;
			TArray<uint8> Data;
			if (const bool bGetOk = GetCachedData(*Request.Key.GetShortKey(), Data))
			{
				FCompositeBuffer CompositeValue(MakeSharedBufferFromArray(MoveTemp(Data)));
				if (const bool bKeyOk = Request.Key.ReadValueTrailer(CompositeValue))
				{
					Value = MoveTemp(CompositeValue).ToShared();
				}
			}
			const EStatus Status = Value ? EStatus::Ok : EStatus::Error;
			FLegacyCacheValue LegacyValue(FCompositeBuffer(MoveTemp(Value)));
			if (LegacyValue.HasData() && LegacyMode == EBackendLegacyMode::ValueWithLegacyFallback)
			{
				Private::ExecuteInCacheThreadPool(AsyncOwner, [this, Request, LegacyValue](IRequestOwner& AsyncOwner, bool bCancel)
				{
					ILegacyCacheStore::LegacyPut({{Request.Name, Request.Key, LegacyValue}}, AsyncOwner, [](auto&&){});
				});
			}
			OnComplete({Request.Name, Request.Key, MoveTemp(LegacyValue), Request.UserData, Status});
		}
	}

	if (!PrefetchKeys.IsEmpty())
	{
		int32 Index = 0;
		const TBitArray<> Exists = TryToPrefetch(PrefetchKeys);
		for (const FLegacyCacheGetRequest* const Request : PrefetchRequests)
		{
			OnComplete({Request->Name, Request->Key, {}, Request->UserData, Exists[Index] ? EStatus::Ok : EStatus::Error});
			++Index;
		}
	}

	if (!ExistsKeys.IsEmpty())
	{
		int32 Index = 0;
		const TBitArray<> Exists = CachedDataProbablyExistsBatch(ExistsKeys);
		for (const FLegacyCacheGetRequest* const Request : ExistsRequests)
		{
			OnComplete({Request->Name, Request->Key, {}, Request->UserData, Exists[Index] ? EStatus::Ok : EStatus::Error});
			++Index;
		}
	}
}

void FDerivedDataBackendInterface::LegacyDelete(
	const TConstArrayView<FLegacyCacheDeleteRequest> Requests,
	IRequestOwner& Owner,
	FOnLegacyCacheDeleteComplete&& OnComplete)
{
	if (GetLegacyMode() != EBackendLegacyMode::LegacyOnly)
	{
		return ILegacyCacheStore::LegacyDelete(Requests, Owner, MoveTemp(OnComplete));
	}

	for (const FLegacyCacheDeleteRequest& Request : Requests)
	{
		RemoveCachedData(*Request.Key.GetShortKey(), Request.bTransient);
		OnComplete({Request.Name, Request.Key, Request.UserData, EStatus::Ok});
	}
}

void FDerivedDataBackendInterface::LegacyStats(FDerivedDataCacheStatsNode& OutNode)
{
	OutNode = MoveTemp(GatherUsageStats().Get());
}

bool FDerivedDataBackendInterface::LegacyDebugOptions(FBackendDebugOptions& Options)
{
	return ApplyDebugOptions(Options);
}

FDerivedDataBackend* FDerivedDataBackend::Create()
{
	return new FDerivedDataBackendGraph();
}

FDerivedDataBackend& FDerivedDataBackend::Get()
{
	return FDerivedDataBackendGraph::Get();
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum class EBackendDebugKeyState
{
	None,
	HitGet,
	MissGet,
};

struct Private::FBackendDebugMissState
{
	FCriticalSection Lock;
	TMap<FCacheKey, EBackendDebugKeyState> Keys;
	TMap<FName, EBackendDebugKeyState> LegacyKeys;
};

FBackendDebugOptions::FBackendDebugOptions()
	: RandomMissRate(0)
	, SpeedClass(EBackendSpeedClass::Unknown)
{
	SimulateMissState.Get() = MakePimpl<Private::FBackendDebugMissState>();
}

/**
 * Parse debug options for the provided node name. Returns true if any options were specified
 */
bool FBackendDebugOptions::ParseFromTokens(FDerivedDataBackendInterface::FBackendDebugOptions& OutOptions, const TCHAR* InNodeName, const TCHAR* InInputTokens)
{
	// check if the input stream has any ddc options for this node
	FString PrefixKey = FString(TEXT("-ddc-")) + InNodeName;

	if (FCString::Stristr(InInputTokens, *PrefixKey) == nullptr)
	{
		// check if it has any -ddc-all- args
		PrefixKey = FString(TEXT("-ddc-all"));

		if (FCString::Stristr(InInputTokens, *PrefixKey) == nullptr)
		{
			return false;
		}
	}

	// turn -arg= into arg= for parsing
	PrefixKey.RightChopInline(1);

	/** types that can be set to ignored (-ddc-<name>-misstypes="foo+bar" etc) */
	// look for -ddc-local-misstype=AnimSeq+Audio -ddc-shared-misstype=AnimSeq+Audio 
	FString ArgName = FString::Printf(TEXT("%s-misstypes="), *PrefixKey);

	FString TempArg;
	FParse::Value(InInputTokens, *ArgName, TempArg);
	TempArg.ParseIntoArray(OutOptions.SimulateMissTypes, TEXT("+"), true);

	// look for -ddc-local-missrate=, -ddc-shared-missrate= etc
	ArgName = FString::Printf(TEXT("%s-missrate="), *PrefixKey);
	int MissRate = 0;
	FParse::Value(InInputTokens, *ArgName, OutOptions.RandomMissRate);

	// look for -ddc-local-speed=, -ddc-shared-speed= etc
	ArgName = FString::Printf(TEXT("%s-speed="), *PrefixKey);
	if (FParse::Value(InInputTokens, *ArgName, TempArg))
	{
		if (!TempArg.IsEmpty())
		{
			LexFromString(OutOptions.SpeedClass, *TempArg);
		}
	}

	return true;
}

bool FBackendDebugOptions::ShouldSimulatePutMiss(const TCHAR* LegacyKey)
{
	if (RandomMissRate == 0 && SimulateMissTypes.IsEmpty())
	{
		return false;
	}

	const FName Key(LegacyKey);

	Private::FBackendDebugMissState& State = *SimulateMissState.Get();
	const uint32 KeyHash = GetTypeHash(Key);

	FScopeLock Lock(&State.Lock);
	State.LegacyKeys.AddByHash(KeyHash, Key, EBackendDebugKeyState::HitGet);
	return false;
}

bool FBackendDebugOptions::ShouldSimulateGetMiss(const TCHAR* LegacyKey)
{
	if (RandomMissRate == 0 && SimulateMissTypes.IsEmpty())
	{
		return false;
	}

	const FStringView KeyView(LegacyKey);
	const FName Key(KeyView);

	bool bMiss = (RandomMissRate >= 100);
	if (!bMiss && !SimulateMissTypes.IsEmpty())
	{
		const FStringView Bucket = KeyView.Left(KeyView.Find(TEXT("_")));
		bMiss = SimulateMissTypes.Contains(Bucket);
	}
	if (!bMiss && RandomMissRate > 0)
	{
		bMiss = FMath::RandHelper(100) < RandomMissRate;
	}

	Private::FBackendDebugMissState& State = *SimulateMissState.Get();
	const uint32 KeyHash = GetTypeHash(Key);

	FScopeLock Lock(&State.Lock);
	const EBackendDebugKeyState KeyState = bMiss ? EBackendDebugKeyState::MissGet : EBackendDebugKeyState::HitGet;
	return State.LegacyKeys.FindOrAddByHash(KeyHash, Key, KeyState) == EBackendDebugKeyState::MissGet;
}

bool FBackendDebugOptions::ShouldSimulatePutMiss(const FCacheKey& Key)
{
	if (RandomMissRate == 0 && SimulateMissTypes.IsEmpty())
	{
		return false;
	}

	Private::FBackendDebugMissState& State = *SimulateMissState.Get();
	const uint32 KeyHash = GetTypeHash(Key);

	FScopeLock Lock(&State.Lock);
	State.Keys.AddByHash(KeyHash, Key, EBackendDebugKeyState::HitGet);
	return false;
}

bool FBackendDebugOptions::ShouldSimulateGetMiss(const FCacheKey& Key)
{
	if (RandomMissRate == 0 && SimulateMissTypes.IsEmpty())
	{
		return false;
	}

	bool bMiss = (RandomMissRate >= 100);
	if (!bMiss && !SimulateMissTypes.IsEmpty())
	{
		TStringBuilder<256> Bucket;
		Bucket << Key.Bucket;
		if (Bucket.ToView().StartsWith(TEXTVIEW("Legacy")))
		{
			Bucket.RemoveAt(0, TEXTVIEW("Legacy").Len());
		}
		bMiss = SimulateMissTypes.Contains(Bucket.ToView());
	}
	if (!bMiss && RandomMissRate > 0)
	{
		bMiss = FMath::RandHelper(100) < RandomMissRate;
	}

	Private::FBackendDebugMissState& State = *SimulateMissState.Get();
	const uint32 KeyHash = GetTypeHash(Key);

	FScopeLock Lock(&State.Lock);
	const EBackendDebugKeyState KeyState = bMiss ? EBackendDebugKeyState::MissGet : EBackendDebugKeyState::HitGet;
	return State.Keys.FindOrAddByHash(KeyHash, Key, KeyState) == EBackendDebugKeyState::MissGet;
}

} // UE::DerivedData

#undef LOCTEXT_NAMESPACE
