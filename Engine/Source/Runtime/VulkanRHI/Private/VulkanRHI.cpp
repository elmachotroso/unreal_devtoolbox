// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	VulkanRHI.cpp: Vulkan device RHI implementation.
=============================================================================*/

#include "VulkanRHIPrivate.h"
#include "BuildSettings.h"
#include "HardwareInfo.h"
#include "VulkanShaderResources.h"
#include "VulkanResources.h"
#include "VulkanPendingState.h"
#include "VulkanContext.h"
#include "VulkanBarriers.h"
#include "Misc/CommandLine.h"
#include "GenericPlatform/GenericPlatformDriver.h"
#include "Modules/ModuleManager.h"
#include "VulkanPipelineState.h"
#include "Misc/FileHelper.h"
#include "VulkanLLM.h"
#include "Misc/EngineVersion.h"
#include "GlobalShader.h"
#include "RHIValidation.h"
#include "IHeadMountedDisplayModule.h"
#include "VulkanRenderpass.h"
#include "VulkanTransientResourceAllocator.h"

static_assert(sizeof(VkStructureType) == sizeof(int32), "ZeroVulkanStruct() assumes VkStructureType is int32!");

extern RHI_API bool GUseTexture3DBulkDataRHI;

#if NV_AFTERMATH
bool GVulkanNVAftermathModuleLoaded = false;
#endif

TAtomic<uint64> GVulkanBufferHandleIdCounter{ 0 };
TAtomic<uint64> GVulkanBufferViewHandleIdCounter{ 0 };
TAtomic<uint64> GVulkanImageViewHandleIdCounter{ 0 };
TAtomic<uint64> GVulkanSamplerHandleIdCounter{ 0 };
TAtomic<uint64> GVulkanDSetLayoutHandleIdCounter{ 0 };

#if VULKAN_ENABLE_DESKTOP_HMD_SUPPORT
#include "Runtime/HeadMountedDisplay/Public/IHeadMountedDisplayModule.h"
#endif

#define LOCTEXT_NAMESPACE "VulkanRHI"

#ifdef VK_API_VERSION
// Check the SDK is least the API version we want to use
static_assert(VK_API_VERSION >= UE_VK_API_VERSION, "Vulkan SDK is older than the version we want to support (UE_VK_API_VERSION). Please update your SDK.");
#elif !defined(VK_HEADER_VERSION)
	#error No VulkanSDK defines?
#endif

#if defined(VK_API_VERSION)
#if defined(VK_HEADER_VERSION) && VK_HEADER_VERSION < 8 && (VK_API_VERSION < VK_MAKE_VERSION(1, 0, 3))
	#include <vulkan/vk_ext_debug_report.h>
#endif
#endif

///////////////////////////////////////////////////////////////////////////////

TAutoConsoleVariable<int32> GRHIThreadCvar(
	TEXT("r.Vulkan.RHIThread"),
	1,
	TEXT("0 to only use Render Thread\n")
	TEXT("1 to use ONE RHI Thread\n")
	TEXT("2 to use multiple RHI Thread\n")
);

int32 GVulkanInputAttachmentShaderRead = 0;
static FAutoConsoleVariableRef GCVarInputAttachmentShaderRead(
	TEXT("r.Vulkan.InputAttachmentShaderRead"),
	GVulkanInputAttachmentShaderRead,
	TEXT("Whether to use VK_ACCESS_SHADER_READ_BIT an input attachments to workaround rendering issues\n")
	TEXT("0 use: VK_ACCESS_INPUT_ATTACHMENT_READ_BIT (default)\n")
	TEXT("1 use: VK_ACCESS_INPUT_ATTACHMENT_READ_BIT | VK_ACCESS_SHADER_READ_BIT\n"),
	ECVF_ReadOnly
);

int32 GVulkanEnableTransientResourceAllocator = 1;
static FAutoConsoleVariableRef GCVarEnableTransientResourceAllocator(
	TEXT("r.Vulkan.EnableTransientResourceAllocator"),
	GVulkanEnableTransientResourceAllocator,
	TEXT("Whether to enable the TransientResourceAllocator to reduce memory usage\n")
	TEXT("0 to disabled (default)\n")
	TEXT("1 to enable\n"),
	ECVF_ReadOnly
);

bool GGPUCrashDebuggingEnabled = false;


extern TAutoConsoleVariable<int32> GRHIAllowAsyncComputeCvar;

// All shader stages supported by VK device - VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, FRAGMENT etc
uint32 GVulkanDeviceShaderStageBits = 0;

#if VULKAN_HAS_VALIDATION_FEATURES
static inline TArray<VkValidationFeatureEnableEXT> GetValidationFeaturesEnabled(bool bEnableValidation)
{
	TArray<VkValidationFeatureEnableEXT> Features;
	extern TAutoConsoleVariable<int32> GGPUValidationCvar;
	int32 GPUValidationValue = GGPUValidationCvar.GetValueOnAnyThread();
	if (bEnableValidation && GPUValidationValue > 0)
	{
		Features.Add(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_EXT);
		if (GPUValidationValue > 1)
		{
			Features.Add(VK_VALIDATION_FEATURE_ENABLE_GPU_ASSISTED_RESERVE_BINDING_SLOT_EXT);
		}
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("vulkanbestpractices")))
	{
		Features.Add(VK_VALIDATION_FEATURE_ENABLE_BEST_PRACTICES_EXT);
	}

	return Features;
}
#endif

DEFINE_LOG_CATEGORY(LogVulkan)

bool FVulkanDynamicRHIModule::IsSupported()
{
	return FVulkanPlatform::IsSupported();
}

FDynamicRHI* FVulkanDynamicRHIModule::CreateRHI(ERHIFeatureLevel::Type InRequestedFeatureLevel)
{
	FVulkanPlatform::SetupMaxRHIFeatureLevelAndShaderPlatform(InRequestedFeatureLevel);
	check(GMaxRHIFeatureLevel != ERHIFeatureLevel::Num);

	GVulkanRHI = new FVulkanDynamicRHI();
	FDynamicRHI* FinalRHI = GVulkanRHI;

#if ENABLE_RHI_VALIDATION
	if (FParse::Param(FCommandLine::Get(), TEXT("RHIValidation")))
	{
		FinalRHI = new FValidationRHI(FinalRHI);
	}
#endif

	return FinalRHI;
}

IMPLEMENT_MODULE(FVulkanDynamicRHIModule, VulkanRHI);


FVulkanCommandListContext::FVulkanCommandListContext(FVulkanDynamicRHI* InRHI, FVulkanDevice* InDevice, FVulkanQueue* InQueue, FVulkanCommandListContext* InImmediate)
	: RHI(InRHI)
	, Immediate(InImmediate)
	, Device(InDevice)
	, Queue(InQueue)
	, bSubmitAtNextSafePoint(false)
	, UniformBufferUploader(nullptr)
	, TempFrameAllocationBuffer(InDevice)
	, CommandBufferManager(nullptr)
	, PendingGfxState(nullptr)
	, PendingComputeState(nullptr)
	, FrameCounter(0)
	, GpuProfiler(this, InDevice)
{
	FrameTiming = new FVulkanGPUTiming(this, InDevice);

	// Create CommandBufferManager, contain all active buffers
	CommandBufferManager = new FVulkanCommandBufferManager(InDevice, this);
	CommandBufferManager->Init(this);
	FrameTiming->Initialize();
	if (IsImmediate())
	{
		// Insert the Begin frame timestamp query. On EndDrawingViewport() we'll insert the End and immediately after a new Begin()
		WriteBeginTimestamp(CommandBufferManager->GetActiveCmdBuffer());

		// Flush the cmd buffer immediately to ensure a valid
		// 'Last submitted' cmd buffer exists at frame 0.
		CommandBufferManager->SubmitActiveCmdBuffer();
		CommandBufferManager->PrepareForNewActiveCommandBuffer();
	}

	// Create Pending state, contains pipeline states such as current shader and etc..
	PendingGfxState = new FVulkanPendingGfxState(Device, *this);
	PendingComputeState = new FVulkanPendingComputeState(Device, *this);

	UniformBufferUploader = new FVulkanUniformBufferUploader(Device);

	GlobalUniformBuffers.AddZeroed(FUniformBufferStaticSlotRegistry::Get().GetSlotCount());
}

void FVulkanCommandListContext::ReleasePendingState()
{
	delete PendingGfxState;
	PendingGfxState = nullptr;
	
	delete PendingComputeState;
	PendingComputeState = nullptr;
}

FVulkanCommandListContext::~FVulkanCommandListContext()
{
	if (FVulkanPlatform::SupportsTimestampRenderQueries())
	{
		FrameTiming->Release();
		delete FrameTiming;
		FrameTiming = nullptr;
	}

	check(CommandBufferManager != nullptr);
	delete CommandBufferManager;
	CommandBufferManager = nullptr;

	LayoutManager.Destroy(*Device, Immediate ? &LayoutManager : nullptr);

	delete UniformBufferUploader;
	delete PendingGfxState;
	delete PendingComputeState;

	TempFrameAllocationBuffer.Destroy();
}


FVulkanCommandListContextImmediate::FVulkanCommandListContextImmediate(FVulkanDynamicRHI* InRHI, FVulkanDevice* InDevice, FVulkanQueue* InQueue)
	: FVulkanCommandListContext(InRHI, InDevice, InQueue, nullptr)
{
}


FVulkanDynamicRHI::FVulkanDynamicRHI()
	: Instance(VK_NULL_HANDLE)
	, Device(nullptr)
	, DrawingViewport(nullptr)
{
	// This should be called once at the start 
	check(IsInGameThread());
	check(!GIsThreadedRendering);

	GPoolSizeVRAMPercentage = 0;
	GTexturePoolSize = 0;
	GRHISupportsMultithreading = true;
	GRHISupportsPipelineFileCache = true;
	GRHITransitionPrivateData_SizeInBytes = sizeof(FVulkanPipelineBarrier);
	GRHITransitionPrivateData_AlignInBytes = alignof(FVulkanPipelineBarrier);
	GConfig->GetInt(TEXT("TextureStreaming"), TEXT("PoolSizeVRAMPercentage"), GPoolSizeVRAMPercentage, GEngineIni);

	// Copy source requires its own image layout.
	EnumRemoveFlags(GRHIMergeableAccessMask, ERHIAccess::CopySrc);
}

void FVulkanDynamicRHI::Init()
{
	// Setup the validation requests ready before we load dlls
	SetupValidationRequests();

	if (!FVulkanPlatform::LoadVulkanLibrary())
	{
#if PLATFORM_LINUX
		// be more verbose on Linux
		FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, *LOCTEXT("UnableToInitializeVulkanLinux", "Unable to load Vulkan library and/or acquire the necessary function pointers. Make sure an up-to-date libvulkan.so.1 is installed.").ToString(),
									 *LOCTEXT("UnableToInitializeVulkanLinuxTitle", "Unable to initialize Vulkan.").ToString());
#endif // PLATFORM_LINUX
		UE_LOG(LogVulkanRHI, Fatal, TEXT("Failed to find all required Vulkan entry points; make sure your driver supports Vulkan!"));
	}

	{
		IConsoleVariable* GPUCrashDebuggingCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUCrashDebugging"));
		GGPUCrashDebuggingEnabled = (GPUCrashDebuggingCVar && GPUCrashDebuggingCVar->GetInt() != 0) || FParse::Param(FCommandLine::Get(), TEXT("gpucrashdebugging"));
	}

	InitInstance();

#if VULKAN_USE_LLM
	LLM(VulkanLLM::Initialize());
#endif

	bIsStandaloneStereoDevice = IHeadMountedDisplayModule::IsAvailable() && IHeadMountedDisplayModule::Get().IsStandaloneStereoOnlyDevice();

	static const auto CVarStreamingTexturePoolSize = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Streaming.PoolSize"));
	int32 StreamingPoolSizeValue = CVarStreamingTexturePoolSize->GetValueOnAnyThread();
			
	if (GPoolSizeVRAMPercentage > 0)
	{
		const uint64 TotalGPUMemory = Device->GetDeviceMemoryManager().GetTotalMemory(true);

		float PoolSize = float(GPoolSizeVRAMPercentage) * 0.01f * float(TotalGPUMemory);

		// Truncate GTexturePoolSize to MB (but still counted in bytes)
		GTexturePoolSize = int64(FGenericPlatformMath::TruncToFloat(PoolSize / 1024.0f / 1024.0f)) * 1024 * 1024;

		UE_LOG(LogRHI, Log, TEXT("Texture pool is %llu MB (%d%% of %llu MB)"),
			GTexturePoolSize / 1024 / 1024,
			GPoolSizeVRAMPercentage,
			TotalGPUMemory / 1024 / 1024);
	}
	else if (StreamingPoolSizeValue > 0)
	{
		GTexturePoolSize = (int64)StreamingPoolSizeValue * 1024 * 1024;

		const uint64 TotalGPUMemory = Device->GetDeviceMemoryManager().GetTotalMemory(true);
		UE_LOG(LogRHI,Log,TEXT("Texture pool is %llu MB (of %llu MB total graphics mem)"),
				GTexturePoolSize / 1024 / 1024,
				TotalGPUMemory / 1024 / 1024);
	}
}

void FVulkanDynamicRHI::PostInit()
{
	//work around layering violation
	TShaderMapRef<FNULLPS>(GetGlobalShaderMap(GMaxRHIFeatureLevel)).GetPixelShader();

#if VULKAN_RHI_RAYTRACING
	if (GRHISupportsRayTracing)
	{
		Device->InitializeRayTracing();
	}
#endif // VULKAN_RHI_RAYTRACING
}

void FVulkanDynamicRHI::Shutdown()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("savevulkanpsocacheonexit")))
	{
		SavePipelineCache();
	}

	check(IsInGameThread() && IsInRenderingThread());
	check(Device);

	Device->PrepareForDestroy();

	EmptyCachedBoundShaderStates();

	FVulkanVertexDeclaration::EmptyCache();

	if (GIsRHIInitialized)
	{
		// Reset the RHI initialized flag.
		GIsRHIInitialized = false;

		FVulkanPlatform::OverridePlatformHandlers(false);

		GRHINeedsExtraDeletionLatency = false;

		check(!GIsCriticalError);

		// Ask all initialized FRenderResources to release their RHI resources.
		FRenderResource::ReleaseRHIForAllResources();

		{
			for (auto& Pair : Device->SamplerMap)
			{
				FVulkanSamplerState* SamplerState = (FVulkanSamplerState*)Pair.Value.GetReference();
				VulkanRHI::vkDestroySampler(Device->GetInstanceHandle(), SamplerState->Sampler, VULKAN_CPU_ALLOCATOR);
			}
			Device->SamplerMap.Empty();
		}

#if VULKAN_RHI_RAYTRACING
		Device->CleanUpRayTracing();
#endif // VULKAN_RHI_RAYTRACING

		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();

		// Flush all pending deletes before destroying the device.
		FRHIResource::FlushPendingDeletes(RHICmdList);
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);

		// And again since some might get on a pending queue
		FRHIResource::FlushPendingDeletes(RHICmdList);
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
	}

	Device->Destroy();

	delete Device;
	Device = nullptr;

	// Release the early HMD interface used to query extra extensions - if any was used
	HMDVulkanExtensions = nullptr;

#if VULKAN_HAS_DEBUGGING_ENABLED
	RemoveDebugLayerCallback();
#endif

	VulkanRHI::vkDestroyInstance(Instance, VULKAN_CPU_ALLOCATOR);

	IConsoleManager::Get().UnregisterConsoleObject(SavePipelineCacheCmd);
	IConsoleManager::Get().UnregisterConsoleObject(RebuildPipelineCacheCmd);

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
	IConsoleManager::Get().UnregisterConsoleObject(DumpMemoryCmd);
	IConsoleManager::Get().UnregisterConsoleObject(DumpMemoryFullCmd);
	IConsoleManager::Get().UnregisterConsoleObject(DumpStagingMemoryCmd);
	IConsoleManager::Get().UnregisterConsoleObject(DumpLRUCmd);
	IConsoleManager::Get().UnregisterConsoleObject(TrimLRUCmd);
#endif

	FVulkanPlatform::FreeVulkanLibrary();

#if VULKAN_ENABLE_DUMP_LAYER
	VulkanRHI::FlushDebugWrapperLog();
#endif
}

void FVulkanDynamicRHI::CreateInstance()
{
	// Engine registration can be disabled via console var. Also disable automatically if ShaderDevelopmentMode is on.
	auto* CVarShaderDevelopmentMode = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.ShaderDevelopmentMode"));
	auto* CVarDisableEngineAndAppRegistration = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.DisableEngineAndAppRegistration"));
	bool bDisableEngineRegistration = (CVarDisableEngineAndAppRegistration && CVarDisableEngineAndAppRegistration->GetValueOnAnyThread() != 0) ||
		(CVarShaderDevelopmentMode && CVarShaderDevelopmentMode->GetValueOnAnyThread() != 0);

	// EngineName will be of the form "UnrealEngine4.21", with the minor version ("21" in this example)
	// updated with every quarterly release
	FString EngineName = FApp::GetEpicProductIdentifier() + FEngineVersion::Current().ToString(EVersionComponent::Minor);
	FTCHARToUTF8 EngineNameConverter(*EngineName);
	FTCHARToUTF8 ProjectNameConverter(FApp::GetProjectName());

	VkApplicationInfo AppInfo;
	ZeroVulkanStruct(AppInfo, VK_STRUCTURE_TYPE_APPLICATION_INFO);
	AppInfo.pApplicationName = bDisableEngineRegistration ? nullptr : ProjectNameConverter.Get();
	AppInfo.applicationVersion = static_cast<uint32>(BuildSettings::GetCurrentChangelist()) | (BuildSettings::IsLicenseeVersion() ? 0x80000000 : 0);
	AppInfo.pEngineName = bDisableEngineRegistration ? nullptr : EngineNameConverter.Get();
	AppInfo.engineVersion = FEngineVersion::Current().GetMinor();
	AppInfo.apiVersion = UE_VK_API_VERSION;

	VkInstanceCreateInfo InstInfo;
	ZeroVulkanStruct(InstInfo, VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
	InstInfo.pApplicationInfo = &AppInfo;

	GetInstanceLayersAndExtensions(InstanceExtensions, InstanceLayers, bSupportsDebugUtilsExt);

	InstInfo.enabledExtensionCount = InstanceExtensions.Num();
	InstInfo.ppEnabledExtensionNames = InstInfo.enabledExtensionCount > 0 ? (const ANSICHAR* const*)InstanceExtensions.GetData() : nullptr;
	
	InstInfo.enabledLayerCount = InstanceLayers.Num();
	InstInfo.ppEnabledLayerNames = InstInfo.enabledLayerCount > 0 ? InstanceLayers.GetData() : nullptr;
#if VULKAN_HAS_DEBUGGING_ENABLED
	bSupportsDebugCallbackExt = !bSupportsDebugUtilsExt && InstanceExtensions.ContainsByPredicate([](const ANSICHAR* Key)
		{ 
			return Key && !FCStringAnsi::Strcmp(Key, VK_EXT_DEBUG_REPORT_EXTENSION_NAME); 
		});

#if VULKAN_HAS_VALIDATION_FEATURES
	bool bHasGPUValidation = InstanceExtensions.ContainsByPredicate([](const ANSICHAR* Key)
		{
			return Key && !FCStringAnsi::Strcmp(Key, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
		});
	VkValidationFeaturesEXT ValidationFeatures;
	TArray<VkValidationFeatureEnableEXT> ValidationFeaturesEnabled = GetValidationFeaturesEnabled(bHasGPUValidation);
	if (bHasGPUValidation)
	{
		ZeroVulkanStruct(ValidationFeatures, VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT);
		ValidationFeatures.pNext = InstInfo.pNext;
		ValidationFeatures.enabledValidationFeatureCount = (uint32)ValidationFeaturesEnabled.Num();
		ValidationFeatures.pEnabledValidationFeatures = ValidationFeaturesEnabled.GetData();
		InstInfo.pNext = &ValidationFeatures;
	}
#endif
#endif

	VkResult Result = VulkanRHI::vkCreateInstance(&InstInfo, VULKAN_CPU_ALLOCATOR, &Instance);
	
	if (Result == VK_ERROR_INCOMPATIBLE_DRIVER)
	{
		FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, TEXT(
			"Cannot find a compatible Vulkan driver (ICD).\n\nPlease look at the Getting Started guide for "
			"additional information."), TEXT("Incompatible Vulkan driver found!"));
		FPlatformMisc::RequestExitWithStatus(true, 1);
		// unreachable
		return;
	}
	else if(Result == VK_ERROR_EXTENSION_NOT_PRESENT)
	{
		// Check for missing extensions 
		FString MissingExtensions;

		uint32_t PropertyCount;
		VulkanRHI::vkEnumerateInstanceExtensionProperties(nullptr, &PropertyCount, nullptr);

		TArray<VkExtensionProperties> Properties;
		Properties.SetNum(PropertyCount);
		VulkanRHI::vkEnumerateInstanceExtensionProperties(nullptr, &PropertyCount, Properties.GetData());

		for (const ANSICHAR* Extension : InstanceExtensions)
		{
			bool bExtensionFound = false;

			for (uint32_t PropertyIndex = 0; PropertyIndex < PropertyCount; PropertyIndex++)
			{
				const char* PropertyExtensionName = Properties[PropertyIndex].extensionName;

				if (!FCStringAnsi::Strcmp(PropertyExtensionName, Extension))
				{
					bExtensionFound = true;
					break;
				}
			}

			if (!bExtensionFound)
			{
				FString ExtensionStr = ANSI_TO_TCHAR(Extension);
				UE_LOG(LogVulkanRHI, Error, TEXT("Missing required Vulkan extension: %s"), *ExtensionStr);
				MissingExtensions += ExtensionStr + TEXT("\n");
			}
		}

		FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, *FString::Printf(TEXT(
			"Vulkan driver doesn't contain specified extensions:\n%s;\n\
			make sure your layers path is set appropriately."), *MissingExtensions), TEXT("Incomplete Vulkan driver found!"));
	}
	else if (Result != VK_SUCCESS)
	{
		FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, TEXT(
			"Vulkan failed to create instance (apiVersion=0x%x)\n\nDo you have a compatible Vulkan "
			 "driver (ICD) installed?\nPlease look at "
			 "the Getting Started guide for additional information."), TEXT("No Vulkan driver found!"));
		FPlatformMisc::RequestExitWithStatus(true, 1);
		// unreachable
		return;
	}

	VERIFYVULKANRESULT(Result);

	if (!FVulkanPlatform::LoadVulkanInstanceFunctions(Instance))
	{
		FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, TEXT(
			"Failed to find all required Vulkan entry points! Try updating your driver."), TEXT("No Vulkan entry points found!"));
	}

#if VULKAN_HAS_DEBUGGING_ENABLED
	SetupDebugLayerCallback();
#endif

	OptionalInstanceExtensions.Setup(InstanceExtensions);
}

//#todo-rco: Common RHI should handle this...
static inline int32 PreferAdapterVendor()
{
	if (FParse::Param(FCommandLine::Get(), TEXT("preferAMD")))
	{
		return 0x1002;
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("preferIntel")))
	{
		return 0x8086;
	}

	if (FParse::Param(FCommandLine::Get(), TEXT("preferNvidia")))
	{
		return 0x10DE;
	}

	return -1;
}

void FVulkanDynamicRHI::SelectAndInitDevice()
{
	uint32 GpuCount = 0;
	VkResult Result = VulkanRHI::vkEnumeratePhysicalDevices(Instance, &GpuCount, nullptr);
	if (Result == VK_ERROR_INITIALIZATION_FAILED)
	{
		FPlatformMisc::MessageBoxExt(EAppMsgType::Ok, TEXT("Cannot find a compatible Vulkan device or driver. Try updating your video driver to a more recent version and make sure your video card supports Vulkan.\n\n"), TEXT("Vulkan device not available"));
		FPlatformMisc::RequestExitWithStatus(true, 1);
	}
	VERIFYVULKANRESULT_EXPANDED(Result);
	checkf(GpuCount >= 1, TEXT("No GPU(s)/Driver(s) that support Vulkan were found! Make sure your drivers are up to date and that you are not pending a reboot."));

	TArray<VkPhysicalDevice> PhysicalDevices;
	PhysicalDevices.AddZeroed(GpuCount);
	VERIFYVULKANRESULT_EXPANDED(VulkanRHI::vkEnumeratePhysicalDevices(Instance, &GpuCount, PhysicalDevices.GetData()));
	checkf(GpuCount >= 1, TEXT("Couldn't enumerate physical devices! Make sure your drivers are up to date and that you are not pending a reboot."));

	FVulkanDevice* HmdDevice = nullptr;
	uint32 HmdDeviceIndex = 0;
	struct FDeviceInfo
	{
		FVulkanDevice* Device;
		uint32 DeviceIndex;
	};
	TArray<FDeviceInfo> DiscreteDevices;
	TArray<FDeviceInfo> IntegratedDevices;

	TArray<FDeviceInfo> OriginalOrderedDevices;

#if VULKAN_ENABLE_DESKTOP_HMD_SUPPORT
	// Allow HMD to override which graphics adapter is chosen, so we pick the adapter where the HMD is connected
	uint64 HmdGraphicsAdapterLuid  = IHeadMountedDisplayModule::IsAvailable() ? IHeadMountedDisplayModule::Get().GetGraphicsAdapterLuid() : 0;
#endif

	UE_LOG(LogVulkanRHI, Display, TEXT("Found %d device(s)"), GpuCount);
	for (uint32 Index = 0; Index < GpuCount; ++Index)
	{
		FVulkanDevice* NewDevice = new FVulkanDevice(this, PhysicalDevices[Index]);
		Devices.Add(NewDevice);

		bool bIsDiscrete = NewDevice->QueryGPU(Index);

#if VULKAN_ENABLE_DESKTOP_HMD_SUPPORT
		if (!HmdDevice && HmdGraphicsAdapterLuid != 0 &&
			NewDevice->GetOptionalExtensions().HasKHRGetPhysicalDeviceProperties2 &&
			FMemory::Memcmp(&HmdGraphicsAdapterLuid, &NewDevice->GetDeviceIdProperties().deviceLUID, VK_LUID_SIZE_KHR) == 0)
		{
			HmdDevice = NewDevice;
			HmdDeviceIndex = Index;
		}
#endif
		if (bIsDiscrete)
		{
			DiscreteDevices.Add({NewDevice, Index});
		}
		else
		{
			IntegratedDevices.Add({NewDevice, Index});
		}

		OriginalOrderedDevices.Add({NewDevice, Index});
	}

	uint32 DeviceIndex = -1;
#if VULKAN_ENABLE_DESKTOP_HMD_SUPPORT
	if (HmdDevice)
	{
		Device = HmdDevice;
		DeviceIndex = HmdDeviceIndex;
	}
#endif

	// Add all integrated to the end of the list
	DiscreteDevices.Append(IntegratedDevices);

	// Non-static as it is used only a few times
	auto* CVarGraphicsAdapter = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.GraphicsAdapter"));
	int32 CVarExplicitAdapterValue = CVarGraphicsAdapter ? CVarGraphicsAdapter->GetValueOnAnyThread() : -1;
	FParse::Value(FCommandLine::Get(), TEXT("graphicsadapter="), CVarExplicitAdapterValue);

	// If HMD didn't choose one... (disable static analysis that DeviceIndex is always -1)
	if (DeviceIndex == -1)	//-V547
	{
		if (CVarExplicitAdapterValue >= (int32)GpuCount)
		{
			UE_LOG(LogVulkanRHI, Warning, TEXT("Tried to use r.GraphicsAdapter=%d, but only %d Adapter(s) found. Falling back to first device..."), CVarExplicitAdapterValue, GpuCount);
			CVarExplicitAdapterValue = 0;
		}
		
		if (CVarExplicitAdapterValue >= 0)
		{
			DeviceIndex = OriginalOrderedDevices[CVarExplicitAdapterValue].DeviceIndex;
			Device = OriginalOrderedDevices[CVarExplicitAdapterValue].Device;
		}
		else
		{
			if (CVarExplicitAdapterValue == -2)
			{
				DeviceIndex = OriginalOrderedDevices[0].DeviceIndex;
				Device = OriginalOrderedDevices[0].Device;
			}
			else if (DiscreteDevices.Num() > 0 && CVarExplicitAdapterValue == -1)
			{
				int32 PreferredVendor = PreferAdapterVendor();
				if (DiscreteDevices.Num() > 1 && PreferredVendor != -1)
				{
					// Check for preferred
					for (int32 Index = 0; Index < DiscreteDevices.Num(); ++Index)
					{
						if (DiscreteDevices[Index].Device->GpuProps.vendorID == PreferredVendor)
						{
							DeviceIndex = DiscreteDevices[Index].DeviceIndex;
							Device = DiscreteDevices[Index].Device;
							break;
						}
					}
				}

				if (DeviceIndex == -1)
				{
					Device = DiscreteDevices[0].Device;
					DeviceIndex = DiscreteDevices[0].DeviceIndex;
				}
			}
			else
			{
				checkf(0, TEXT("No devices found!"));
				DeviceIndex = 0;
			}
		}
	}

	const VkPhysicalDeviceProperties& Props = Device->GetDeviceProperties();
	bool bUseVendorIdAsIs = true;
	if (Props.vendorID > 0xffff)
	{
		bUseVendorIdAsIs = false;
		VkVendorId VendorId = (VkVendorId)Props.vendorID;
		switch (VendorId)
		{
		case VK_VENDOR_ID_VIV:		GRHIVendorId = (uint32)EGpuVendorId::Vivante; break;
		case VK_VENDOR_ID_VSI:		GRHIVendorId = (uint32)EGpuVendorId::VeriSilicon; break;
		case VK_VENDOR_ID_KAZAN:	GRHIVendorId = (uint32)EGpuVendorId::Kazan; break;
		case VK_VENDOR_ID_CODEPLAY:	GRHIVendorId = (uint32)EGpuVendorId::Codeplay; break;
		case VK_VENDOR_ID_MESA:		GRHIVendorId = (uint32)EGpuVendorId::Mesa; break;
		default:
			// Unhandled case
			UE_LOG(LogVulkanRHI, Warning, TEXT("Unhandled VkVendorId %d"), (int32)VendorId);
			bUseVendorIdAsIs = true;
			break;
		}
	}

	if (bUseVendorIdAsIs)
	{
		GRHIVendorId = Props.vendorID;
	}
	GRHIAdapterName = ANSI_TO_TCHAR(Props.deviceName);

	FVulkanPlatform::CheckDeviceDriver(DeviceIndex, Device->GetVendorId(), Props);

	Device->InitGPU(DeviceIndex);

	auto ReadVulkanDriverVersionFromProps = [](FVulkanDevice* CurrentDevice) {

		const VkPhysicalDeviceProperties& Props = CurrentDevice->GetDeviceProperties();

		if (CurrentDevice->GetVendorId() == EGpuVendorId::Nvidia)
		{
			UNvidiaDriverVersion NvidiaVersion;
			static_assert(sizeof(NvidiaVersion) == sizeof(Props.driverVersion), "Mismatched Nvidia pack driver version!");
			NvidiaVersion.Packed = Props.driverVersion;
			GRHIAdapterUserDriverVersion = FString::Printf(TEXT("%d.%02d"), NvidiaVersion.Major, NvidiaVersion.Minor);
		}
		else
		{
			GRHIAdapterUserDriverVersion = FString::Printf(TEXT("%d.%d.%d"), VK_VERSION_MAJOR(Props.driverVersion), VK_VERSION_MINOR(Props.driverVersion), VK_VERSION_PATCH(Props.driverVersion), Props.driverVersion);
		}

		GRHIDeviceId = Props.deviceID;
		GRHIAdapterInternalDriverVersion = GRHIAdapterUserDriverVersion;
		GRHIAdapterDriverDate = TEXT("01-01-01");  // Unused on unix systems, pick a date that will fail test if compared but passes IsValid() check

		UE_LOG(LogVulkanRHI, Log, TEXT("    Adapter Name: %s"), *GRHIAdapterName);
		UE_LOG(LogVulkanRHI, Log, TEXT("     API Version: %d.%d.%d"), VK_VERSION_MAJOR(Props.apiVersion), VK_VERSION_MINOR(Props.apiVersion), VK_VERSION_PATCH(Props.apiVersion));
		UE_LOG(LogVulkanRHI, Log, TEXT("  Driver Version: %s"), *GRHIAdapterUserDriverVersion);
	};

	if (PLATFORM_ANDROID)
	{
		GRHIAdapterName.Append(TEXT(" Vulkan"));
		GRHIAdapterInternalDriverVersion = FString::Printf(TEXT("%d.%d.%d"), VK_VERSION_MAJOR(Props.apiVersion), VK_VERSION_MINOR(Props.apiVersion), VK_VERSION_PATCH(Props.apiVersion));
	}
	else if (PLATFORM_WINDOWS)
	{
		GRHIDeviceId = Props.deviceID;
		FGPUDriverInfo GPUDriverInfo = FPlatformMisc::GetGPUDriverInfo(GRHIAdapterName);

		if (GPUDriverInfo.InternalDriverVersion != TEXT("Unknown"))
		{
		GRHIAdapterUserDriverVersion = GPUDriverInfo.UserDriverVersion;
		GRHIAdapterInternalDriverVersion = GPUDriverInfo.InternalDriverVersion;
		GRHIAdapterDriverDate = GPUDriverInfo.DriverDate;

		UE_LOG(LogVulkanRHI, Log, TEXT("    Adapter Name: %s"), *GRHIAdapterName);
		UE_LOG(LogVulkanRHI, Log, TEXT("     API Version: %d.%d.%d"), VK_VERSION_MAJOR(Props.apiVersion), VK_VERSION_MINOR(Props.apiVersion), VK_VERSION_PATCH(Props.apiVersion));
			UE_LOG(LogVulkanRHI, Log, TEXT("  Driver Version: %s (0x%X)"), *GRHIAdapterUserDriverVersion, Props.driverVersion);
		UE_LOG(LogVulkanRHI, Log, TEXT("Internal Version: %s"), *GRHIAdapterInternalDriverVersion);
		UE_LOG(LogVulkanRHI, Log, TEXT("     Driver Date: %s"), *GRHIAdapterDriverDate);
	}
		else
	{
			// If we failed to read from the registry, then use the values provided by Vulkan props
			ReadVulkanDriverVersionFromProps(Device);
		}
		}
	else if(PLATFORM_UNIX)
		{
		ReadVulkanDriverVersionFromProps(Device);
	}

	GRHIPersistentThreadGroupCount = 1440; // TODO: Revisit based on vendor/adapter/perf query
}

void FVulkanDynamicRHI::InitInstance()
{
	check(IsInGameThread());

	// Wait for the rendering thread to go idle.
	SCOPED_SUSPEND_RENDERING_THREAD(false);

	if (!Device)
	{
		check(!GIsRHIInitialized);

		FVulkanPlatform::OverridePlatformHandlers(true);

		GRHISupportsAsyncTextureCreation = false;
		GEnableAsyncCompute = false;

		CreateInstance();
		SelectAndInitDevice();

#if VULKAN_HAS_DEBUGGING_ENABLED
		if (GRenderDocFound)
		{
			EnableIdealGPUCaptureOptions(true);
		}
#endif
		//bool bDeviceSupportsTessellation = Device->GetPhysicalFeatures().tessellationShader != 0;

		const VkPhysicalDeviceProperties& Props = Device->GetDeviceProperties();
		const VkPhysicalDeviceLimits& Limits = Device->GetLimits();

		// Initialize the RHI capabilities.
		GRHISupportsFirstInstance = true;
		GRHISupportsDynamicResolution = FVulkanPlatform::SupportsDynamicResolution();
		GRHISupportsFrameCyclesBubblesRemoval = true;
		GSupportsDepthBoundsTest = Device->GetPhysicalFeatures().depthBounds != 0;
		GSupportsRenderTargetFormat_PF_G8 = false;	// #todo-rco
		GRHISupportsTextureStreaming = true;
		GSupportsTimestampRenderQueries = FVulkanPlatform::SupportsTimestampRenderQueries();
#if VULKAN_SUPPORTS_MULTIVIEW
		GSupportsMobileMultiView = Device->GetMultiviewFeatures().multiview == VK_TRUE ? true : false;
#endif
#if VULKAN_RHI_RAYTRACING
		GRHISupportsRayTracing = RHISupportsRayTracing(GMaxRHIShaderPlatform) && Device->GetOptionalExtensions().HasRaytracingExtensions();

		if (GRHISupportsRayTracing)
		{
			GRHISupportsRayTracingShaders = RHISupportsRayTracingShaders(GMaxRHIShaderPlatform);
			GRHISupportsInlineRayTracing = RHISupportsInlineRayTracing(GMaxRHIShaderPlatform) && Device->GetOptionalExtensions().HasRayQuery;

			const FRayTracingProperties& RayTracingProps = Device->GetRayTracingProperties();
			GRHIRayTracingAccelerationStructureAlignment = 256; // TODO (currently handled by FVulkanAccelerationStructureBuffer)
			GRHIRayTracingScratchBufferAlignment = RayTracingProps.AccelerationStructure.minAccelerationStructureScratchOffsetAlignment;
			GRHIRayTracingInstanceDescriptorSize = uint32(sizeof(VkAccelerationStructureInstanceKHR));
		}
#endif
#if VULKAN_ENABLE_DUMP_LAYER
		// Disable RHI thread by default if the dump layer is enabled
		GRHISupportsRHIThread = false;
		GRHISupportsParallelRHIExecute = false;
#else
		GRHISupportsRHIThread = GRHIThreadCvar->GetInt() != 0;
		GRHISupportsParallelRHIExecute = GRHIThreadCvar->GetInt() > 1;
#endif
		GSupportsParallelOcclusionQueries = true;

		// Some platforms might only have CPU for an RHI thread, but not for parallel tasks
		GSupportsParallelRenderingTasksWithSeparateRHIThread = GRHISupportsRHIThread ? FVulkanPlatform::SupportParallelRenderingTasks() : false;

		//#todo-rco: Add newer Nvidia also
		GSupportsEfficientAsyncCompute = (Device->ComputeContext != Device->ImmediateContext) && ((Device->GetVendorId() == EGpuVendorId::Amd) || FParse::Param(FCommandLine::Get(), TEXT("ForceAsyncCompute")));

		GSupportsVolumeTextureRendering = FVulkanPlatform::SupportsVolumeTextureRendering();

		// Indicate that the RHI needs to use the engine's deferred deletion queue.
		GRHINeedsExtraDeletionLatency = true;

		GRHISupportsCopyToTextureMultipleMips = true;

		GMaxShadowDepthBufferSizeX =  FPlatformMath::Min<int32>(Props.limits.maxImageDimension2D, GMaxShadowDepthBufferSizeX);
		GMaxShadowDepthBufferSizeY =  FPlatformMath::Min<int32>(Props.limits.maxImageDimension2D, GMaxShadowDepthBufferSizeY);
		GMaxTextureDimensions = Props.limits.maxImageDimension2D;
		GMaxBufferDimensions = Props.limits.maxTexelBufferElements;
		GMaxComputeSharedMemory = Props.limits.maxComputeSharedMemorySize;
		GMaxTextureMipCount = FPlatformMath::CeilLogTwo( GMaxTextureDimensions ) + 1;
		GMaxTextureMipCount = FPlatformMath::Min<int32>( MAX_TEXTURE_MIP_COUNT, GMaxTextureMipCount );
		GMaxCubeTextureDimensions = Props.limits.maxImageDimensionCube;
		GMaxVolumeTextureDimensions = Props.limits.maxImageDimension3D;
		GMaxWorkGroupInvocations = Props.limits.maxComputeWorkGroupInvocations;
		GMaxTextureArrayLayers = Props.limits.maxImageArrayLayers;
		GRHISupportsBaseVertexIndex = true;
		GSupportsSeparateRenderTargetBlendState = true;

		GRHIMaxDispatchThreadGroupsPerDimension.X = FMath::Min<uint32>(Limits.maxComputeWorkGroupCount[0], 0x7fffffff);
		GRHIMaxDispatchThreadGroupsPerDimension.Y = FMath::Min<uint32>(Limits.maxComputeWorkGroupCount[1], 0x7fffffff);
		GRHIMaxDispatchThreadGroupsPerDimension.Z = FMath::Min<uint32>(Limits.maxComputeWorkGroupCount[2], 0x7fffffff);

#if VULKAN_SUPPORTS_SHADER_VIEWPORT_INDEX_LAYER
		GRHISupportsArrayIndexFromAnyShader = GetDevice()->GetOptionalExtensions().HasEXTShaderViewportIndexLayer != 0ULL;
#endif

#if VULKAN_SUPPORTS_FRAGMENT_DENSITY_MAP
		GRHISupportsAttachmentVariableRateShading = (GetDevice()->GetOptionalExtensions().HasEXTFragmentDensityMap && Device->GetFragmentDensityMapFeatures().fragmentDensityMap);
#endif

#if VULKAN_SUPPORTS_FRAGMENT_DENSITY_MAP2
		GRHISupportsLateVariableRateShadingUpdate = GetDevice()->GetOptionalExtensions().HasEXTFragmentDensityMap2 && Device->GetFragmentDensityMap2Features().fragmentDensityMapDeferred;
#endif

#if VULKAN_SUPPORTS_FRAGMENT_SHADING_RATE
		GRHISupportsAttachmentVariableRateShading |= GetDevice()->GetOptionalExtensions().HasKHRFragmentShadingRate && Device->GetFragmentShadingRateFeatures().attachmentFragmentShadingRate;
#endif

		FVulkanPlatform::SetupFeatureLevels();

		GRHIRequiresRenderTargetForPixelShaderUAVs = true;

		GUseTexture3DBulkDataRHI = false;

		// these are supported by all devices
		GVulkanDeviceShaderStageBits = 	VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | 
										VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
										VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
		// optional shader stages
		if (Device->GetPhysicalFeatures().geometryShader) 
		{
			GVulkanDeviceShaderStageBits|= VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT;
		}
		
		FHardwareInfo::RegisterHardwareInfo(NAME_RHI, TEXT("Vulkan"));

		SavePipelineCacheCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("r.Vulkan.SavePipelineCache"),
			TEXT("Save pipeline cache."),
			FConsoleCommandDelegate::CreateStatic(SavePipelineCache),
			ECVF_Default
			);

		RebuildPipelineCacheCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("r.Vulkan.RebuildPipelineCache"),
			TEXT("Rebuilds pipeline cache."),
			FConsoleCommandDelegate::CreateStatic(RebuildPipelineCache),
			ECVF_Default
			);

#if VULKAN_SUPPORTS_VALIDATION_CACHE
#if VULKAN_HAS_DEBUGGING_ENABLED
		if (GValidationCvar.GetValueOnAnyThread() > 0)
		{
			SaveValidationCacheCmd = IConsoleManager::Get().RegisterConsoleCommand(
				TEXT("r.Vulkan.SaveValidationCache"),
				TEXT("Save validation cache."),
				FConsoleCommandDelegate::CreateStatic(SaveValidationCache),
				ECVF_Default
				);
		}
#endif
#endif

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
		DumpMemoryCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("r.Vulkan.DumpMemory"),
			TEXT("Dumps memory map."),
			FConsoleCommandDelegate::CreateStatic(DumpMemory),
			ECVF_Default
			);
		DumpMemoryFullCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("r.Vulkan.DumpMemoryFull"),
			TEXT("Dumps full memory map."),
			FConsoleCommandDelegate::CreateStatic(DumpMemoryFull),
			ECVF_Default
		);
		DumpStagingMemoryCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("r.Vulkan.DumpStagingMemory"),
			TEXT("Dumps staging memory map."),
			FConsoleCommandDelegate::CreateStatic(DumpStagingMemory),
			ECVF_Default
		);

		DumpLRUCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("r.Vulkan.DumpPSOLRU"),
			TEXT("Dumps Vulkan PSO LRU."),
			FConsoleCommandDelegate::CreateStatic(DumpLRU),
			ECVF_Default
		);
		TrimLRUCmd = IConsoleManager::Get().RegisterConsoleCommand(
			TEXT("r.Vulkan.TrimPSOLRU"),
			TEXT("Trim Vulkan PSO LRU."),
			FConsoleCommandDelegate::CreateStatic(TrimLRU),
			ECVF_Default
		);

#endif

		// Command lists need the validation RHI context if enabled, so call the global scope version of RHIGetDefaultContext() and RHIGetDefaultAsyncComputeContext().
		GRHICommandList.GetImmediateCommandList().SetContext(::RHIGetDefaultContext());
		GRHICommandList.GetImmediateAsyncComputeCommandList().SetComputeContext(::RHIGetDefaultAsyncComputeContext());

		FRenderResource::InitPreRHIResources();
		GIsRHIInitialized = true;
	}
}

void FVulkanCommandListContext::RHIBeginFrame()
{
	check(IsImmediate());
	RHIPrivateBeginFrame();

	extern uint32 GVulkanRHIDeletionFrameNumber;
	++GVulkanRHIDeletionFrameNumber;

	GpuProfiler.BeginFrame();
}


void FVulkanCommandListContext::RHIBeginScene()
{
	//FRCLog::Printf(FString::Printf(TEXT("FVulkanCommandListContext::RHIBeginScene()")));
}

void FVulkanCommandListContext::RHIEndScene()
{
	//FRCLog::Printf(FString::Printf(TEXT("FVulkanCommandListContext::RHIEndScene()")));
}

void FVulkanCommandListContext::RHIBeginDrawingViewport(FRHIViewport* ViewportRHI, FRHITexture* RenderTargetRHI)
{
	//FRCLog::Printf(FString::Printf(TEXT("FVulkanCommandListContext::RHIBeginDrawingViewport\n")));
	check(ViewportRHI);
	FVulkanViewport* Viewport = ResourceCast(ViewportRHI);
	RHI->DrawingViewport = Viewport;

	FRHICustomPresent* CustomPresent = Viewport->GetCustomPresent();
	if (CustomPresent)
	{
		CustomPresent->BeginDrawing();
	}
}

void FVulkanCommandListContext::RHIEndDrawingViewport(FRHIViewport* ViewportRHI, bool bPresent, bool bLockToVsync)
{
	LLM_SCOPE_VULKAN(ELLMTagVulkan::VulkanMisc);
	//FRCLog::Printf(FString::Printf(TEXT("FVulkanCommandListContext::RHIEndDrawingViewport()")));
	check(IsImmediate());
	FVulkanViewport* Viewport = ResourceCast(ViewportRHI);
	check(Viewport == RHI->DrawingViewport);

	//#todo-rco: Unbind all pending state
/*
	check(bPresent);
	RHI->Present();
*/
	FVulkanCmdBuffer* CmdBuffer = CommandBufferManager->GetActiveCmdBuffer();
	check(!CmdBuffer->HasEnded() && !CmdBuffer->IsInsideRenderPass());

	WriteEndTimestamp(CmdBuffer);

	bool bNativePresent = Viewport->Present(this, CmdBuffer, Queue, Device->GetPresentQueue(), bLockToVsync);
	if (bNativePresent)
	{
		//#todo-rco: Check for r.FinishCurrentFrame
	}

	RHI->DrawingViewport = nullptr;

	WriteBeginTimestamp(CommandBufferManager->GetActiveCmdBuffer());
}

void FVulkanCommandListContext::RHIEndFrame()
{
	check(IsImmediate());
	//FRCLog::Printf(FString::Printf(TEXT("FVulkanCommandListContext::RHIEndFrame()")));
	
	ReadAndCalculateGPUFrameTime();

	GetGPUProfiler().EndFrame();

	GetCommandBufferManager()->FreeUnusedCmdBuffers();

	Device->GetStagingManager().ProcessPendingFree(false, true);
	Device->GetMemoryManager().ReleaseFreedPages(*this);
	Device->GetDeferredDeletionQueue().ReleaseResources();

	if (UseVulkanDescriptorCache())
	{
		Device->GetDescriptorSetCache().GC();
	}
	Device->GetDescriptorPoolsManager().GC();

	Device->ReleaseUnusedOcclusionQueryPools();

	Device->GetPipelineStateCache()->TickLRU();

	++FrameCounter;
}

void FVulkanCommandListContext::RHIPushEvent(const TCHAR* Name, FColor Color)
{
#if VULKAN_ENABLE_DRAW_MARKERS
#if 0//VULKAN_SUPPORTS_DEBUG_UTILS
	if (auto CmdBeginLabel = Device->GetCmdBeginDebugLabel())
	{
		FTCHARToUTF8 Converter(Name);
		VkDebugUtilsLabelEXT Label;
		ZeroVulkanStruct(Label, VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT);
		Label.pLabelName = Converter.Get();
		FLinearColor LColor(Color);
		Label.color[0] = LColor.R;
		Label.color[1] = LColor.G;
		Label.color[2] = LColor.B;
		Label.color[3] = LColor.A;
		CmdBeginLabel(GetCommandBufferManager()->GetActiveCmdBuffer()->GetHandle(), &Label);
	}
	else
#endif
	if (auto CmdDbgMarkerBegin = Device->GetCmdDbgMarkerBegin())
	{
		FTCHARToUTF8 Converter(Name);
		VkDebugMarkerMarkerInfoEXT Info;
		ZeroVulkanStruct(Info, VK_STRUCTURE_TYPE_DEBUG_MARKER_MARKER_INFO_EXT);
		Info.pMarkerName = Converter.Get();
		FLinearColor LColor(Color);
		Info.color[0] = LColor.R;
		Info.color[1] = LColor.G;
		Info.color[2] = LColor.B;
		Info.color[3] = LColor.A;
		CmdDbgMarkerBegin(GetCommandBufferManager()->GetActiveCmdBuffer()->GetHandle(), &Info);
	}
#endif

#if VULKAN_SUPPORTS_GPU_CRASH_DUMPS
	if (GpuProfiler.bTrackingGPUCrashData)
	{
		GpuProfiler.PushMarkerForCrash(GetCommandBufferManager()->GetActiveCmdBuffer()->GetHandle(), Device->GetCrashMarkerBuffer(), Name);
	}
#endif

	//only valid on immediate context currently.  needs to be fixed for parallel rhi execute
	if (IsImmediate())
	{
#if VULKAN_ENABLE_DUMP_LAYER
		VulkanRHI::DumpLayerPushMarker(Name);
#endif

		GpuProfiler.PushEvent(Name, Color);
	}
}

void FVulkanCommandListContext::RHIPopEvent()
{
#if VULKAN_ENABLE_DRAW_MARKERS
#if 0//VULKAN_SUPPORTS_DEBUG_UTILS
	if (auto CmdEndLabel = Device->GetCmdEndDebugLabel())
	{
		CmdEndLabel(GetCommandBufferManager()->GetActiveCmdBuffer()->GetHandle());
	}
	else
#endif
	if (auto CmdDbgMarkerEnd = Device->GetCmdDbgMarkerEnd())
	{
		CmdDbgMarkerEnd(GetCommandBufferManager()->GetActiveCmdBuffer()->GetHandle());
	}
#endif

#if VULKAN_SUPPORTS_GPU_CRASH_DUMPS
	if (GpuProfiler.bTrackingGPUCrashData)
	{
		GpuProfiler.PopMarkerForCrash(GetCommandBufferManager()->GetActiveCmdBuffer()->GetHandle(), Device->GetCrashMarkerBuffer());
	}
#endif

	//only valid on immediate context currently.  needs to be fixed for parallel rhi execute
	if (IsImmediate())
	{
#if VULKAN_ENABLE_DUMP_LAYER
		VulkanRHI::DumpLayerPopMarker();
#endif

		GpuProfiler.PopEvent();
	}
}

void FVulkanDynamicRHI::RHIGetSupportedResolution( uint32 &Width, uint32 &Height )
{
}

bool FVulkanDynamicRHI::RHIGetAvailableResolutions(FScreenResolutionArray& Resolutions, bool bIgnoreRefreshRate)
{
	return false;
}

void FVulkanDynamicRHI::RHIFlushResources()
{
}

void FVulkanDynamicRHI::RHIAcquireThreadOwnership()
{
}

void FVulkanDynamicRHI::RHIReleaseThreadOwnership()
{
}

void* FVulkanDynamicRHI::RHIGetNativeDevice()
{
	return (void*)Device->GetInstanceHandle();
}

void* FVulkanDynamicRHI::RHIGetNativePhysicalDevice()
{
	return (void*)Device->GetPhysicalHandle();
}

void* FVulkanDynamicRHI::RHIGetNativeGraphicsQueue()
{
	return (void*)Device->GetGraphicsQueue()->GetHandle();
}

void* FVulkanDynamicRHI::RHIGetNativeComputeQueue()
{
	return (void*)Device->GetComputeQueue()->GetHandle();
}

void* FVulkanDynamicRHI::RHIGetNativeInstance()
{
	return (void*)GetInstance();
}

IRHICommandContext* FVulkanDynamicRHI::RHIGetDefaultContext()
{
	return &Device->GetImmediateContext();
}

IRHIComputeContext* FVulkanDynamicRHI::RHIGetDefaultAsyncComputeContext()
{
	return &Device->GetImmediateComputeContext();
}

uint64 FVulkanDynamicRHI::RHIGetMinimumAlignmentForBufferBackedSRV(EPixelFormat Format)
{
	const VkPhysicalDeviceLimits& Limits = Device->GetLimits();
	return Limits.minTexelBufferOffsetAlignment;
}

IRHICommandContextContainer* FVulkanDynamicRHI::RHIGetCommandContextContainer(int32 Index, int32 Num)
{
	if (GRHIThreadCvar.GetValueOnAnyThread() > 1)
	{
		return new FVulkanCommandContextContainer(Device);
	}

	return nullptr;
}

void FVulkanDynamicRHI::RHISubmitCommandsAndFlushGPU()
{
	Device->SubmitCommandsAndFlushGPU();
}

FTexture2DRHIRef FVulkanDynamicRHI::RHICreateTexture2DFromResource(EPixelFormat Format, uint32 SizeX, uint32 SizeY, uint32 NumMips, uint32 NumSamples, VkImage Resource, ETextureCreateFlags Flags)
{
	const FRHIResourceCreateInfo ResourceCreateInfo(TEXT("FVulkanTexture2D"), EnumHasAnyFlags(Flags, TexCreate_DepthStencilTargetable) ? FClearValueBinding::DepthZero : FClearValueBinding::Transparent);
	return new FVulkanTexture2D(*Device, Format, SizeX, SizeY, NumMips, NumSamples, Resource, Flags, ResourceCreateInfo);
}

FTexture2DRHIRef FVulkanDynamicRHI::RHICreateTexture2DFromResource(EPixelFormat Format, uint32 SizeX, uint32 SizeY, uint32 NumMips, uint32 NumSamples, VkImage Resource, FSamplerYcbcrConversionInitializer& ConversionInitializer, ETextureCreateFlags Flags)
{
	const FRHIResourceCreateInfo ResourceCreateInfo(TEXT("FVulkanTexture2D"), EnumHasAnyFlags(Flags, TexCreate_DepthStencilTargetable) ? FClearValueBinding::DepthZero : FClearValueBinding::Transparent);
	return new FVulkanTexture2D(*Device, Format, SizeX, SizeY, NumMips, NumSamples, Resource, ConversionInitializer, Flags, ResourceCreateInfo);
}

FTexture2DArrayRHIRef FVulkanDynamicRHI::RHICreateTexture2DArrayFromResource(EPixelFormat Format, uint32 SizeX, uint32 SizeY, uint32 ArraySize, uint32 NumMips, uint32 NumSamples, VkImage Resource, ETextureCreateFlags Flags)
{
	const FRHIResourceCreateInfo ResourceCreateInfo(TEXT("FVulkanTexture2DArray"), EnumHasAnyFlags(Flags, TexCreate_DepthStencilTargetable) ? FClearValueBinding::DepthZero : FClearValueBinding::Transparent);
	return new FVulkanTexture2DArray(*Device, Format, SizeX, SizeY, ArraySize, NumMips, NumSamples, Resource, Flags, ResourceCreateInfo);
}

FTextureCubeRHIRef FVulkanDynamicRHI::RHICreateTextureCubeFromResource(EPixelFormat Format, uint32 Size, bool bArray, uint32 ArraySize, uint32 NumMips, VkImage Resource, ETextureCreateFlags Flags)
{
	const FRHIResourceCreateInfo ResourceCreateInfo(TEXT("FVulkanTextureCube"), EnumHasAnyFlags(Flags, TexCreate_DepthStencilTargetable) ? FClearValueBinding::DepthZero : FClearValueBinding::Transparent);
	return new FVulkanTextureCube(*Device, Format, Size, bArray, ArraySize, NumMips, Resource, Flags, ResourceCreateInfo);
}

void FVulkanDynamicRHI::RHIAliasTextureResources(FRHITexture* DestTextureRHI, FRHITexture* SrcTextureRHI)
{
	check(false);
}

FTextureRHIRef FVulkanDynamicRHI::RHICreateAliasedTexture(FRHITexture* SourceTexture)
{
	check(false);
	return nullptr;
}

void FVulkanDynamicRHI::RHIAliasTextureResources(FTextureRHIRef& DestTextureRHI, FTextureRHIRef& SrcTextureRHI)
{
	if (DestTextureRHI && SrcTextureRHI)
	{
		FVulkanTextureBase* DestTextureBase = (FVulkanTextureBase*)DestTextureRHI->GetTextureBaseRHI();
		FVulkanTextureBase* SrcTextureBase = (FVulkanTextureBase*)SrcTextureRHI->GetTextureBaseRHI();

		if (DestTextureBase && SrcTextureBase)
		{
			DestTextureBase->AliasTextureResources(SrcTextureRHI);
		}
	}
}

FTextureRHIRef FVulkanDynamicRHI::RHICreateAliasedTexture(FTextureRHIRef& SourceTextureRHI)
{
	FVulkanTextureBase* SourceTexture = (FVulkanTextureBase*)SourceTextureRHI->GetTextureBaseRHI();
	FTextureRHIRef AliasedTexture;
	if (SourceTextureRHI->GetTexture2D() != nullptr)
	{
		AliasedTexture = new FVulkanTexture2D(SourceTextureRHI, (FVulkanTexture2D*)SourceTexture);
	}
	else if (SourceTextureRHI->GetTexture2DArray() != nullptr)
	{
		AliasedTexture = new FVulkanTexture2DArray(SourceTextureRHI, (FVulkanTexture2DArray*)SourceTexture);
	}
	else if (SourceTextureRHI->GetTextureCube() != nullptr)
	{
		AliasedTexture = new FVulkanTextureCube(SourceTextureRHI, (FVulkanTextureCube*)SourceTexture);
	}
	else
	{
		UE_LOG(LogRHI, Error, TEXT("Currently FVulkanDynamicRHI::RHICreateAliasedTexture only supports 2D, 2D Array and Cube textures."));
	}

	return AliasedTexture;
}

void FVulkanDynamicRHI::RHICopySubTextureRegion(FRHITexture2D* SourceTexture, FRHITexture2D* DestinationTexture, FBox2D SourceBox, FBox2D DestinationBox)
{
	FRHICopyTextureInfo CopyInfo;

	CopyInfo.Size.X = (int32)(SourceBox.Max.X - SourceBox.Min.X);
	CopyInfo.Size.Y = (int32)(SourceBox.Max.Y - SourceBox.Min.Y);

	CopyInfo.SourcePosition.X = (int32)(SourceBox.Min.X);
	CopyInfo.SourcePosition.Y = (int32)(SourceBox.Min.Y);
	CopyInfo.DestPosition.X = (int32)(DestinationBox.Min.X);
	CopyInfo.DestPosition.Y = (int32)(DestinationBox.Min.Y);

	RHIGetDefaultContext()->RHICopyTexture(SourceTexture, DestinationTexture, CopyInfo);
}

FVulkanDescriptorSetsLayout::FVulkanDescriptorSetsLayout(FVulkanDevice* InDevice) :
	Device(InDevice)
{
}

FVulkanDescriptorSetsLayout::~FVulkanDescriptorSetsLayout()
{
	// Handles are owned by FVulkanPipelineStateCacheManager
	LayoutHandles.Reset(0);
}

// Increments a value and asserts on overflow.
// FSetInfo uses narrow integer types for descriptor counts,
// which may feasibly overflow one day (for example if we add bindless resources).
template <typename T> 
static void IncrementChecked(T& Value)
{
	check(Value < TNumericLimits<T>::Max());
	++Value;
}

void FVulkanDescriptorSetsLayoutInfo::AddDescriptor(int32 DescriptorSetIndex, const VkDescriptorSetLayoutBinding& Descriptor)
{
	// Increment type usage
	if (LayoutTypes.Contains(Descriptor.descriptorType))
	{
		LayoutTypes[Descriptor.descriptorType]++;
	}
	else
	{
		LayoutTypes.Add(Descriptor.descriptorType, 1);
	}

	if (DescriptorSetIndex >= SetLayouts.Num())
	{
		SetLayouts.SetNum(DescriptorSetIndex + 1, false);
	}

	FSetLayout& DescSetLayout = SetLayouts[DescriptorSetIndex];

	VkDescriptorSetLayoutBinding* Binding = new(DescSetLayout.LayoutBindings) VkDescriptorSetLayoutBinding;
	*Binding = Descriptor;

	const FDescriptorSetRemappingInfo::FSetInfo& SetInfo = RemappingInfo.SetInfos[DescriptorSetIndex];
	check(SetInfo.Types[Descriptor.binding] == Descriptor.descriptorType);
	switch (Descriptor.descriptorType)
	{
	case VK_DESCRIPTOR_TYPE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
	case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
	case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
	case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
		IncrementChecked(RemappingInfo.SetInfos[DescriptorSetIndex].NumImageInfos);
		break;
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
	case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
		IncrementChecked(RemappingInfo.SetInfos[DescriptorSetIndex].NumBufferInfos);
		break;
#if VULKAN_RHI_RAYTRACING
	case VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR:
		IncrementChecked(RemappingInfo.SetInfos[DescriptorSetIndex].NumAccelerationStructures);
		break;
#endif
	case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
	case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
		break;
	default:
		checkf(0, TEXT("Unsupported descriptor type %d"), (int32)Descriptor.descriptorType);
		break;
	}
}

void FVulkanDescriptorSetsLayoutInfo::GenerateHash(const TArrayView<FRHISamplerState*>& InImmutableSamplers)
{
	const int32 LayoutCount = SetLayouts.Num();
	Hash = FCrc::MemCrc32(&TypesUsageID, sizeof(uint32), LayoutCount);

	for (int32 layoutIndex = 0; layoutIndex < LayoutCount; ++layoutIndex)
	{
		SetLayouts[layoutIndex].GenerateHash();
		Hash = FCrc::MemCrc32(&SetLayouts[layoutIndex].Hash, sizeof(uint32), Hash);
	}

	for (uint32 RemapingIndex = 0; RemapingIndex < ShaderStage::NumStages; ++RemapingIndex)
	{
		Hash = FCrc::MemCrc32(&RemappingInfo.StageInfos[RemapingIndex].PackedUBDescriptorSet, sizeof(uint16), Hash);
		Hash = FCrc::MemCrc32(&RemappingInfo.StageInfos[RemapingIndex].Pad0, sizeof(uint16), Hash);

		TArray<FDescriptorSetRemappingInfo::FRemappingInfo>& Globals = RemappingInfo.StageInfos[RemapingIndex].Globals;
		Hash = FCrc::MemCrc32(Globals.GetData(), sizeof(FDescriptorSetRemappingInfo::FRemappingInfo) * Globals.Num(), Hash);

		TArray<FDescriptorSetRemappingInfo::FUBRemappingInfo>& UniformBuffers = RemappingInfo.StageInfos[RemapingIndex].UniformBuffers;
		Hash = FCrc::MemCrc32(UniformBuffers.GetData(), sizeof(FDescriptorSetRemappingInfo::FUBRemappingInfo) * UniformBuffers.Num(), Hash);

		TArray<uint16>& PackedUBBindingIndices = RemappingInfo.StageInfos[RemapingIndex].PackedUBBindingIndices;
		Hash = FCrc::MemCrc32(PackedUBBindingIndices.GetData(), sizeof(uint16) * PackedUBBindingIndices.Num(), Hash);
	}

#if VULKAN_SUPPORTS_COLOR_CONVERSIONS
	VkSampler ImmutableSamplers[MaxImmutableSamplers];
	VkSampler* ImmutableSamplerPtr = ImmutableSamplers;
	for (int32 Index = 0; Index < InImmutableSamplers.Num(); ++Index)
	{
		FRHISamplerState* SamplerState = InImmutableSamplers[Index];
		*ImmutableSamplerPtr++ = SamplerState ? ResourceCast(SamplerState)->Sampler : VK_NULL_HANDLE;
	}
	FMemory::Memzero(ImmutableSamplerPtr, (MaxImmutableSamplers - InImmutableSamplers.Num()));
	Hash = FCrc::MemCrc32(ImmutableSamplers, sizeof(VkSampler) * MaxImmutableSamplers, Hash);
#endif
}

static FCriticalSection GTypesUsageCS;
void FVulkanDescriptorSetsLayoutInfo::CompileTypesUsageID()
{
	FScopeLock ScopeLock(&GTypesUsageCS);

	static TMap<uint32, uint32> GTypesUsageHashMap;
	static uint32 GUniqueID = 1;

	LayoutTypes.KeySort([](VkDescriptorType A, VkDescriptorType B)
	{
		return static_cast<uint32>(A) < static_cast<uint32>(B);
	});

	uint32 TypesUsageHash = 0;
	for (const auto& Elem : LayoutTypes)
	{
		TypesUsageHash = FCrc::MemCrc32(&Elem.Value, sizeof(uint32), TypesUsageHash);
	}

	uint32* UniqueID = GTypesUsageHashMap.Find(TypesUsageHash);
	if (UniqueID == nullptr)
	{
		TypesUsageID = GTypesUsageHashMap.Add(TypesUsageHash, GUniqueID++);
	}
	else
	{
		TypesUsageID = *UniqueID;
	}
}

void FVulkanDescriptorSetsLayout::Compile(FVulkanDescriptorSetLayoutMap& DSetLayoutMap)
{
	check(LayoutHandles.Num() == 0);

	// Check if we obey limits
	const VkPhysicalDeviceLimits& Limits = Device->GetLimits();
	
	// Check for maxDescriptorSetSamplers
	check(		LayoutTypes[VK_DESCRIPTOR_TYPE_SAMPLER]
			+	LayoutTypes[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER]
			<=	Limits.maxDescriptorSetSamplers);

	// Check for maxDescriptorSetUniformBuffers
	check(		LayoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER]
			+	LayoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC]
			<=	Limits.maxDescriptorSetUniformBuffers);

	// Check for maxDescriptorSetUniformBuffersDynamic
	check(Device->GetVendorId() == EGpuVendorId::Amd ||
				LayoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC]
			<=	Limits.maxDescriptorSetUniformBuffersDynamic);

	// Check for maxDescriptorSetStorageBuffers
	check(		LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER]
			+	LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC]
			<=	Limits.maxDescriptorSetStorageBuffers);

	// Check for maxDescriptorSetStorageBuffersDynamic
	check(		LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC]
			<=	Limits.maxDescriptorSetStorageBuffersDynamic);

	// Check for maxDescriptorSetSampledImages
	check(		LayoutTypes[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER]
			+	LayoutTypes[VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE]
			+	LayoutTypes[VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER]
			<=	Limits.maxDescriptorSetSampledImages);

	// Check for maxDescriptorSetStorageImages
	check(		LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_IMAGE]
			+	LayoutTypes[VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER]
			<=	Limits.maxDescriptorSetStorageImages);

	check(LayoutTypes[VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT] <= Limits.maxDescriptorSetInputAttachments);

#if VULKAN_RHI_RAYTRACING
	if (GRHISupportsRayTracing)
	{
		check(LayoutTypes[VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR] < Device->GetRayTracingProperties().AccelerationStructure.maxDescriptorSetAccelerationStructures);
	}
#endif
	
	LayoutHandles.Empty(SetLayouts.Num());

	if (UseVulkanDescriptorCache())
	{
		LayoutHandleIds.Empty(SetLayouts.Num());
	}
				
	for (FSetLayout& Layout : SetLayouts)
	{
		VkDescriptorSetLayout* LayoutHandle = new(LayoutHandles) VkDescriptorSetLayout;

		uint32* LayoutHandleId = nullptr;
		if (UseVulkanDescriptorCache())
		{
			LayoutHandleId = new(LayoutHandleIds) uint32;
		}
			
		if (FVulkanDescriptorSetLayoutEntry* Found = DSetLayoutMap.Find(Layout))
		{
			*LayoutHandle = Found->Handle;
			if (LayoutHandleId)
			{
				*LayoutHandleId = Found->HandleId;
			}
			continue;
		}

		VkDescriptorSetLayoutCreateInfo DescriptorLayoutInfo;
		ZeroVulkanStruct(DescriptorLayoutInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
		DescriptorLayoutInfo.bindingCount = Layout.LayoutBindings.Num();
		DescriptorLayoutInfo.pBindings = Layout.LayoutBindings.GetData();

		VERIFYVULKANRESULT(VulkanRHI::vkCreateDescriptorSetLayout(Device->GetInstanceHandle(), &DescriptorLayoutInfo, VULKAN_CPU_ALLOCATOR, LayoutHandle));

		if (LayoutHandleId)
		{
			*LayoutHandleId = ++GVulkanDSetLayoutHandleIdCounter;
		}

		FVulkanDescriptorSetLayoutEntry DescriptorSetLayoutEntry;
		DescriptorSetLayoutEntry.Handle = *LayoutHandle;
		DescriptorSetLayoutEntry.HandleId = LayoutHandleId ? *LayoutHandleId : 0;
				
		DSetLayoutMap.Add(Layout, DescriptorSetLayoutEntry);
	}

	if (TypesUsageID == ~0)
	{
		CompileTypesUsageID();
	}

	ZeroVulkanStruct(DescriptorSetAllocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
	DescriptorSetAllocateInfo.descriptorSetCount = LayoutHandles.Num();
	DescriptorSetAllocateInfo.pSetLayouts = LayoutHandles.GetData();
}


void FVulkanBufferView::Create(FVulkanResourceMultiBuffer* Buffer, EPixelFormat Format, uint32 InOffset, uint32 InSize)
{
	check(Format != PF_Unknown);
	VkFormat BufferFormat = GVulkanBufferFormat[Format];
	check(BufferFormat != VK_FORMAT_UNDEFINED);
	Create(BufferFormat, Buffer, InOffset, InSize);
}

void FVulkanBufferView::Create(VkFormat Format, FVulkanResourceMultiBuffer* Buffer, uint32 InOffset, uint32 InSize)
{
	Offset = InOffset;
	Size = InSize;
	check(Format != VK_FORMAT_UNDEFINED);

	VkBufferViewCreateInfo ViewInfo;
	ZeroVulkanStruct(ViewInfo, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO);
	ViewInfo.buffer = Buffer->GetHandle();
	ViewInfo.format = Format;
	ViewInfo.offset = Offset;

	//#todo-rco: Revisit this if buffer views become VK_BUFFER_USAGE_STORAGE_BUFFER_BIT instead of VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT
	const VkPhysicalDeviceLimits& Limits = Device->GetLimits();
	const uint64 MaxSize = (uint64)Limits.maxTexelBufferElements * GetNumBitsPerPixel(Format) / 8;
	ViewInfo.range = FMath::Min<uint64>(Size, MaxSize);
	// TODO: add a check() for exceeding MaxSize, to catch code which blindly makes views without checking the platform limits.

	Flags = Buffer->GetBufferUsageFlags() & (VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT);
	check(Flags);
	check(IsAligned(Offset, Limits.minTexelBufferOffsetAlignment));

	VERIFYVULKANRESULT(VulkanRHI::vkCreateBufferView(GetParent()->GetInstanceHandle(), &ViewInfo, VULKAN_CPU_ALLOCATOR, &View));
	
	bVolatile = Buffer->IsVolatile();
	if (!bVolatile && UseVulkanDescriptorCache())
	{
		ViewId = ++GVulkanBufferViewHandleIdCounter;
	}

	INC_DWORD_STAT(STAT_VulkanNumBufferViews);
}

void FVulkanBufferView::Destroy()
{
	if (View != VK_NULL_HANDLE)
	{
		DEC_DWORD_STAT(STAT_VulkanNumBufferViews);
		Device->GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue2::EType::BufferView, View);
		View = VK_NULL_HANDLE;
		ViewId = 0;
		bVolatile = false;
	}
}

FVulkanRenderPass::FVulkanRenderPass(FVulkanDevice& InDevice, const FVulkanRenderTargetLayout& InRTLayout) :
	Layout(InRTLayout),
	RenderPass(VK_NULL_HANDLE),
	NumUsedClearValues(InRTLayout.GetNumUsedClearValues()),
	Device(InDevice)
{
	INC_DWORD_STAT(STAT_VulkanNumRenderPasses);
	RenderPass = CreateVulkanRenderPass(InDevice, InRTLayout);
}

FVulkanRenderPass::~FVulkanRenderPass()
{
	DEC_DWORD_STAT(STAT_VulkanNumRenderPasses);

	Device.GetDeferredDeletionQueue().EnqueueResource(FDeferredDeletionQueue2::EType::RenderPass, RenderPass);
	RenderPass = VK_NULL_HANDLE;
}

FVulkanRingBuffer::FVulkanRingBuffer(FVulkanDevice* InDevice, uint64 TotalSize, VkFlags Usage, VkMemoryPropertyFlags MemPropertyFlags)
	: VulkanRHI::FDeviceChild(InDevice)
	, BufferSize(TotalSize)
	, BufferOffset(0)
	, MinAlignment(0)
{
	check(TotalSize <= (uint64)MAX_uint32);
	InDevice->GetMemoryManager().AllocateBufferPooled(Allocation, this, TotalSize, Usage, MemPropertyFlags, EVulkanAllocationMetaRingBuffer, __FILE__, __LINE__);
	MinAlignment = Allocation.GetBufferAlignment(Device);
	// Start by wrapping around to set up the correct fence
	BufferOffset = TotalSize;
}

FVulkanRingBuffer::~FVulkanRingBuffer()
{
	Device->GetMemoryManager().FreeVulkanAllocation(Allocation);
}

uint64 FVulkanRingBuffer::WrapAroundAllocateMemory(uint64 Size, uint32 Alignment, FVulkanCmdBuffer* InCmdBuffer)
{
	CA_ASSUME(InCmdBuffer != nullptr); // Suppress static analysis warning
	uint64 AllocationOffset = Align<uint64>(BufferOffset, Alignment);
	ensure(AllocationOffset + Size > BufferSize);

	// Check to see if we can wrap around the ring buffer
	if (FenceCmdBuffer)
	{
		if (FenceCounter == FenceCmdBuffer->GetFenceSignaledCounterI())
		{
			//if (FenceCounter == FenceCmdBuffer->GetSubmittedFenceCounter())
			{
				//UE_LOG(LogVulkanRHI, Error, TEXT("Ringbuffer overflow during the same cmd buffer!"));
			}
			//else
			{
				//UE_LOG(LogVulkanRHI, Error, TEXT("Wrapped around the ring buffer! Waiting for the GPU..."));
				//Device->GetImmediateContext().GetCommandBufferManager()->WaitForCmdBuffer(FenceCmdBuffer, 0.5f);
			}
		}
	}

	BufferOffset = Size;

	FenceCmdBuffer = InCmdBuffer;
	FenceCounter = InCmdBuffer->GetSubmittedFenceCounter();

	return 0;
}

void FVulkanDynamicRHI::SavePipelineCache()
{
	FString CacheFile = GetPipelineCacheFilename();

	GVulkanRHI->Device->PipelineStateCache->Save(CacheFile);
}

void FVulkanDynamicRHI::RebuildPipelineCache()
{
	GVulkanRHI->Device->PipelineStateCache->RebuildCache();
}

#if VULKAN_SUPPORTS_VALIDATION_CACHE
void FVulkanDynamicRHI::SaveValidationCache()
{
	VkValidationCacheEXT ValidationCache = GVulkanRHI->Device->GetValidationCache();
	if (ValidationCache != VK_NULL_HANDLE)
	{
		VkDevice Device = GVulkanRHI->Device->GetInstanceHandle();
		PFN_vkGetValidationCacheDataEXT vkGetValidationCacheData = (PFN_vkGetValidationCacheDataEXT)(void*)VulkanRHI::vkGetDeviceProcAddr(Device, "vkGetValidationCacheDataEXT");
		check(vkGetValidationCacheData);
		size_t CacheSize = 0;
		VkResult Result = vkGetValidationCacheData(Device, ValidationCache, &CacheSize, nullptr);
		if (Result == VK_SUCCESS)
		{
			if (CacheSize > 0)
			{
				TArray<uint8> Data;
				Data.AddUninitialized(CacheSize);
				Result = vkGetValidationCacheData(Device, ValidationCache, &CacheSize, Data.GetData());
				if (Result == VK_SUCCESS)
				{
					FString CacheFilename = GetValidationCacheFilename();
					if (FFileHelper::SaveArrayToFile(Data, *CacheFilename))
					{
						UE_LOG(LogVulkanRHI, Display, TEXT("Saved validation cache file '%s', %d bytes"), *CacheFilename, Data.Num());
					}
				}
				else
				{
					UE_LOG(LogVulkanRHI, Warning, TEXT("Failed to query Vulkan validation cache data, VkResult=%d"), Result);
				}
			}
		}
		else
		{
			UE_LOG(LogVulkanRHI, Warning, TEXT("Failed to query Vulkan validation cache size, VkResult=%d"), Result);
		}
	}
}
#endif

#if UE_BUILD_DEBUG || UE_BUILD_DEVELOPMENT
void FVulkanDynamicRHI::DumpMemory()
{
	GVulkanRHI->Device->GetMemoryManager().DumpMemory(false);
}
void FVulkanDynamicRHI::DumpMemoryFull()
{
	GVulkanRHI->Device->GetMemoryManager().DumpMemory(true);
}
void FVulkanDynamicRHI::DumpStagingMemory()
{
	GVulkanRHI->Device->GetStagingManager().DumpMemory();
}
void FVulkanDynamicRHI::DumpLRU()
{
	GVulkanRHI->Device->PipelineStateCache->LRUDump();
}
void FVulkanDynamicRHI::TrimLRU()
{
	GVulkanRHI->Device->PipelineStateCache->LRUDebugEvictAll();
}
#endif

void FVulkanDynamicRHI::DestroySwapChain()
{
	if (IsInGameThread())
	{
		FlushRenderingCommands();
	}

	TArray<FVulkanViewport*> Viewports = GVulkanRHI->Viewports;
	ENQUEUE_RENDER_COMMAND(VulkanDestroySwapChain)(
		[Viewports](FRHICommandListImmediate& RHICmdList)
	{
		UE_LOG(LogVulkanRHI, Log, TEXT("Destroy swapchain ... "));
		
		for (FVulkanViewport* Viewport : Viewports)
		{
			Viewport->DestroySwapchain(nullptr);
		}
	});

	if (IsInGameThread())
	{
		FlushRenderingCommands();
	}
}

void FVulkanDynamicRHI::RecreateSwapChain(void* NewNativeWindow)
{
	if (NewNativeWindow)
	{
		if (IsInGameThread())
		{
			FlushRenderingCommands();
		}

		TArray<FVulkanViewport*> Viewports = GVulkanRHI->Viewports;
		ENQUEUE_RENDER_COMMAND(VulkanRecreateSwapChain)(
			[Viewports, NewNativeWindow](FRHICommandListImmediate& RHICmdList)
		{
			UE_LOG(LogVulkanRHI, Log, TEXT("Recreate swapchain ... "));
			
			for (FVulkanViewport* Viewport : Viewports)
			{
				Viewport->RecreateSwapchain(NewNativeWindow);
			}
		});

		if (IsInGameThread())
		{
			FlushRenderingCommands();
		}
	}
}

void FVulkanDynamicRHI::VulkanSetImageLayout( VkCommandBuffer CmdBuffer, VkImage Image, VkImageLayout OldLayout, VkImageLayout NewLayout, const VkImageSubresourceRange& SubresourceRange )
{
	::VulkanSetImageLayout( CmdBuffer, Image, OldLayout, NewLayout, SubresourceRange );
}

IRHITransientResourceAllocator* FVulkanDynamicRHI::RHICreateTransientResourceAllocator()
{
#if VULKAN_SUPPORTS_TRANSIENT_RESOURCE_ALLOCATOR
	// Only use transient heap on desktop platforms for now
	// Not compatible with VulkanDescriptorCache for now because it hashes using the 32bit BufferId instead of the VulkanHandle.
	if (GVulkanEnableTransientResourceAllocator && IsPCPlatform(GMaxRHIShaderPlatform) && !UseVulkanDescriptorCache())
	{
		return new FVulkanTransientResourceAllocator(Device->GetOrCreateTransientHeapCache());
	}
#endif
	return nullptr;
}

#undef LOCTEXT_NAMESPACE
