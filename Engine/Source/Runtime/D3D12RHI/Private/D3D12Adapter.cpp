// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
D3D12Adapter.cpp:D3D12 Adapter implementation.
=============================================================================*/

#include "D3D12RHIPrivate.h"
#include "Misc/CommandLine.h"
#include "Misc/EngineVersion.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Windows/AllowWindowsPlatformTypes.h"
#include "GenericPlatform/GenericPlatformCrashContext.h"
#if PLATFORM_WINDOWS
#include "Windows/WindowsPlatformMisc.h"
#include "Windows/WindowsPlatformStackWalk.h"
#endif
#include "Modules/ModuleManager.h"

#if !PLATFORM_CPU_ARM_FAMILY && (PLATFORM_WINDOWS)
	#include "amd_ags.h"
#endif
#include "Windows/HideWindowsPlatformTypes.h"

#if ENABLE_RESIDENCY_MANAGEMENT
bool GEnableResidencyManagement = true;
static TAutoConsoleVariable<int32> CVarResidencyManagement(
	TEXT("D3D12.ResidencyManagement"),
	1,
	TEXT("Controls whether D3D12 resource residency management is active (default = on)."),
	ECVF_ReadOnly
);
#endif // ENABLE_RESIDENCY_MANAGEMENT

#if D3D12_SUBMISSION_GAP_RECORDER
int32 GEnableGapRecorder = 0;
bool GGapRecorderActiveOnBeginFrame = false;
static FAutoConsoleVariableRef CVarEnableGapRecorder(
	TEXT("D3D12.EnableGapRecorder"),
	GEnableGapRecorder,
	TEXT("Controls whether D3D12 gap recorder (cpu bubbles) is active (default = on)."),
	ECVF_RenderThreadSafe
);

int32 GGapRecorderUseBlockingCall = 0;
static FAutoConsoleVariableRef CVarGapRecorderUseBlockingCall(
	TEXT("D3D12.GapRecorderUseBlockingCall"),
	GGapRecorderUseBlockingCall,
	TEXT("Controls whether D3D12 gap recorder (cpu bubbles) uses a blocking call or not."),
	ECVF_RenderThreadSafe
);
#endif

#if TRACK_RESOURCE_ALLOCATIONS
int32 GTrackedReleasedAllocationFrameRetention = 100;
static FAutoConsoleVariableRef CTrackedReleasedAllocationFrameRetention(
	TEXT("D3D12.TrackedReleasedAllocationFrameRetention"),
	GTrackedReleasedAllocationFrameRetention,
	TEXT("Amount of frames for which we keep freed allocation data around when resource tracking is enabled"),
	ECVF_RenderThreadSafe
);
#endif

#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS

#if UE_BUILD_SHIPPING || UE_BUILD_TEST
static int32 GD3D12EnableGPUBreadCrumbs = 0;
static int32 GD3D12EnableNvAftermath = 0;
static int32 GD3D12EnableDRED = 0;
#else
static int32 GD3D12EnableGPUBreadCrumbs = 1;
static int32 GD3D12EnableNvAftermath = 1;
static int32 GD3D12EnableDRED = 0;
#endif // UE_BUILD_SHIPPING || UE_BUILD_TEST

static FAutoConsoleVariableRef CVarD3D12EnableGPUBreadCrumbs(
	TEXT("r.D3D12.BreadCrumbs"),
	GD3D12EnableGPUBreadCrumbs,
	TEXT("Enable minimal overhead GPU Breadcrumbs to track the current GPU state and logs information what operations the GPU executed last.\n"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static FAutoConsoleVariableRef CVarD3D12EnableNvAftermath(
	TEXT("r.D3D12.NvAfterMath"),
	GD3D12EnableNvAftermath,
	TEXT("Enable NvAftermath to track the current GPU state and logs information what operations the GPU executed last.\n")
	TEXT("Only works on nVidia hardware and will dump GPU crashdumps as well.\n"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static FAutoConsoleVariableRef CVarD3D12EnableDRED(
	TEXT("r.D3D12.DRED"),
	GD3D12EnableDRED,
	TEXT("Enable DRED GPU Crash debugging mode to track the current GPU state and logs information what operations the GPU executed last.")
	TEXT("Has GPU overhead but gives the most information on the current GPU state when it crashes or hangs.\n"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

bool GD3D12TrackAllAlocations = false;
static TAutoConsoleVariable<int32> CVarD3D12TrackAllAllocations(
	TEXT("D3D12.TrackAllAllocations"),
	GD3D12TrackAllAlocations,
	TEXT("Controls whether D3D12 RHI should track all allocation information (default = off)."),
	ECVF_ReadOnly
);

static bool CheckD3DStoredMessages()
{
	bool bResult = false;

	TRefCountPtr<ID3D12Debug> d3dDebug;
	if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), (void**)d3dDebug.GetInitReference())))
	{
		FD3D12DynamicRHI* D3D12RHI = FD3D12DynamicRHI::GetD3DRHI();
		TRefCountPtr<ID3D12InfoQueue> d3dInfoQueue;
		if (SUCCEEDED(D3D12RHI->GetAdapter().GetD3DDevice()->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)d3dInfoQueue.GetInitReference())))
		{
			D3D12_MESSAGE* d3dMessage = nullptr;
			SIZE_T AllocateSize = 0;

			int StoredMessageCount = d3dInfoQueue->GetNumStoredMessagesAllowedByRetrievalFilter();
			for (int MessageIndex = 0; MessageIndex < StoredMessageCount; MessageIndex++)
			{
				SIZE_T MessageLength = 0;
				HRESULT hr = d3dInfoQueue->GetMessage(MessageIndex, nullptr, &MessageLength);

				// Ideally the exception handler should not allocate any memory because it could fail
				// and can cause another exception to be triggered and possible even cause a deadlock.
				// But for these D3D error message it should be fine right now because they are requested
				// exceptions when making an error against the API.
				// Not allocating memory for the messages is easy (cache memory in Adapter), but ANSI_TO_TCHAR
				// and UE_LOG will also allocate memory and aren't that easy to fix.

				// realloc the message
				if (MessageLength > AllocateSize)
				{
					if (d3dMessage)
					{
						FMemory::Free(d3dMessage);
						d3dMessage = nullptr;
						AllocateSize = 0;
					}

					d3dMessage = (D3D12_MESSAGE*)FMemory::Malloc(MessageLength);
					AllocateSize = MessageLength;
				}

				if (d3dMessage)
				{
					// get the actual message data from the queue
					hr = d3dInfoQueue->GetMessage(MessageIndex, d3dMessage, &MessageLength);

					if (d3dMessage->Severity == D3D12_MESSAGE_SEVERITY_ERROR)
					{
						UE_LOG(LogD3D12RHI, Error, TEXT("%s"), ANSI_TO_TCHAR(d3dMessage->pDescription));
					}
					else if (d3dMessage->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
					{
						UE_LOG(LogD3D12RHI, Warning, TEXT("%s"), ANSI_TO_TCHAR(d3dMessage->pDescription));
					}
					else 
					{
						UE_LOG(LogD3D12RHI, Log, TEXT("%s"), ANSI_TO_TCHAR(d3dMessage->pDescription));
					}
				}

				// we got messages
				bResult = true;
			}

			if (AllocateSize > 0)
			{
				FMemory::Free(d3dMessage);
			}
		}
	}

	return bResult;
}

/** Handle d3d messages and write them to the log file **/
static LONG __stdcall D3DVectoredExceptionHandler(EXCEPTION_POINTERS* InInfo)
{
	// Only handle D3D error codes here
	if (InInfo->ExceptionRecord->ExceptionCode == _FACDXGI)
	{
		if (CheckD3DStoredMessages())
		{
			if (FPlatformMisc::IsDebuggerPresent())
			{
				// when we get here, then it means that BreakOnSeverity was set for this error message, so request the debug break here as well
				// when the debugger is attached
				UE_DEBUG_BREAK();
			}
		}

		// Handles the exception
		return EXCEPTION_CONTINUE_EXECUTION;
	}

	// continue searching
	return EXCEPTION_CONTINUE_SEARCH;
}

#endif // #if PLATFORM_WINDOWS || PLATFORM_HOLOLENS


FTransientUniformBufferAllocator::~FTransientUniformBufferAllocator()
{
	if (Adapter)
	{
		Adapter->ReleaseTransientUniformBufferAllocator(this);
	}
}

void FTransientUniformBufferAllocator::Cleanup()
{
	ClearResource();
	Adapter = nullptr;
}


FD3D12Adapter::FD3D12Adapter(FD3D12AdapterDesc& DescIn)
	: OwningRHI(nullptr)
	, bDepthBoundsTestSupported(false)
	, bHeapNotZeroedSupported(false)
	, VRSTileSize(0)
	, bDebugDevice(false)
	, GPUCrashDebuggingModes(ED3D12GPUCrashDebuggingModes::None)	
	, bDeviceRemoved(false)
	, Desc(DescIn)
	, RootSignatureManager(this)
	, PipelineStateCache(this)
	, FenceCorePool(this)
	, DeferredDeletionQueue(this)
	, DefaultContextRedirector(this, ED3D12CommandQueueType::Direct, true)
	, DefaultAsyncComputeContextRedirector(this, ED3D12CommandQueueType::Async, true)
	, FrameCounter(0)
	, DebugFlags(0)
{
	FMemory::Memzero(&UploadHeapAllocator, sizeof(UploadHeapAllocator));
	FMemory::Memzero(&Devices, sizeof(Devices));

	uint32 MaxGPUCount = 1; // By default, multi-gpu is disabled.
#if WITH_MGPU
	if (!FParse::Value(FCommandLine::Get(), TEXT("MaxGPUCount="), MaxGPUCount))
	{
		// If there is a mode token in the command line, enable multi-gpu.
		if (FParse::Param(FCommandLine::Get(), TEXT("AFR")))
		{
			MaxGPUCount = MAX_NUM_GPUS;
		}
	}
	if (FParse::Param(FCommandLine::Get(), TEXT("VMGPU")))
	{
		GVirtualMGPU = 1;
		UE_LOG(LogD3D12RHI, Log, TEXT("Enabling virtual multi-GPU mode"), Desc.NumDeviceNodes);
	}
#endif

	if (GVirtualMGPU)
	{
		Desc.NumDeviceNodes = FMath::Min<uint32>(MaxGPUCount, MAX_NUM_GPUS);
	}
	else
	{
		Desc.NumDeviceNodes = FMath::Min3<uint32>(Desc.NumDeviceNodes, MaxGPUCount, (uint32)MAX_NUM_GPUS);
	}
}

void FD3D12Adapter::Initialize(FD3D12DynamicRHI* RHI)
{
	OwningRHI = RHI;
}

/** Callback function called when the GPU crashes, when Aftermath is enabled */
static void D3D12AftermathCrashCallback(const void* InGPUCrashDump, const uint32_t InGPUCrashDumpSize, void* InUserData)
{
	// Forward to shared function which is also called when DEVICE_LOST return value is given
	D3D12RHI::TerminateOnGPUCrash(nullptr, InGPUCrashDump, InGPUCrashDumpSize);
}


void FD3D12Adapter::CreateRootDevice(bool bWithDebug)
{
	const bool bAllowVendorDevice = !FParse::Param(FCommandLine::Get(), TEXT("novendordevice"));

	// -d3ddebug is always allowed on Windows, but only allowed in non-shipping builds on other platforms.
	// -gpuvalidation is only supported on Windows.
#if PLATFORM_WINDOWS || !UE_BUILD_SHIPPING
	bool bWithGPUValidation = PLATFORM_WINDOWS && (FParse::Param(FCommandLine::Get(), TEXT("d3d12gpuvalidation")) || FParse::Param(FCommandLine::Get(), TEXT("gpuvalidation")));
	// If GPU validation is requested, automatically enable the debug layer.
	bWithDebug |= bWithGPUValidation;
	if (bWithDebug)
	{
		TRefCountPtr<ID3D12Debug> DebugController;
		if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(DebugController.GetInitReference()))))
		{
			DebugController->EnableDebugLayer();
			bDebugDevice = true;

#if PLATFORM_WINDOWS
			if (bWithGPUValidation)
			{
				TRefCountPtr<ID3D12Debug1> DebugController1;
				VERIFYD3D12RESULT(DebugController->QueryInterface(IID_PPV_ARGS(DebugController1.GetInitReference())));
				DebugController1->SetEnableGPUBasedValidation(true);
				SetEmitDrawEvents(true);
			}
#endif
		}
		else
		{
			UE_LOG(LogD3D12RHI, Fatal, TEXT("The debug interface requires the D3D12 SDK Layers. Please install the Graphics Tools for Windows. See: https://docs.microsoft.com/en-us/windows/uwp/gaming/use-the-directx-runtime-and-visual-studio-graphics-diagnostic-features"));
		}
	}

	FGenericCrashContext::SetEngineData(TEXT("RHI.D3DDebug"), bWithDebug ? TEXT("true") : TEXT("false"));
	UE_LOG(LogD3D12RHI, Log, TEXT("InitD3DDevice: -D3DDebug = %s -D3D12GPUValidation = %s"), bWithDebug ? TEXT("on") : TEXT("off"), bWithGPUValidation ? TEXT("on") : TEXT("off"));
#endif

#if PLATFORM_WINDOWS || (PLATFORM_HOLOLENS && !UE_BUILD_SHIPPING && WITH_PIX_EVENT_RUNTIME)
	
	// Multiple ways to enable the different D3D12 crash debugging modes:
	// - via RHI independent r.GPUCrashDebugging cvar: by default enable low overhead breadcrumbs and NvAftermath are enabled
	// - via 'gpucrashdebugging' command line argument: enable all possible GPU crash debug modes (minor performance impact)
	// - via 'r.D3D12.BreadCrumbs', 'r.D3D12.AfterMath' or 'r.D3D12.Dred' each type of GPU crash debugging mode can be enabled
	// - via '-gpubreadcrumbs(=0)', '-nvaftermath(=0)' or '-dred(=0)' command line argument: each type of gpu crash debugging mode can enabled/disabled
	if (FParse::Param(FCommandLine::Get(), TEXT("gpucrashdebugging")))
	{
		GPUCrashDebuggingModes = ED3D12GPUCrashDebuggingModes::All;
	}
	else
	{
		// Parse the specific GPU crash debugging cvars and enable the different modes
		const auto ParseCVar = [this](const TCHAR* CVarName, ED3D12GPUCrashDebuggingModes DebuggingMode)
		{
			IConsoleVariable* ConsoleVariable = IConsoleManager::Get().FindConsoleVariable(CVarName);
			if (ConsoleVariable && ConsoleVariable->GetInt() > 0)
			{
				EnumAddFlags(GPUCrashDebuggingModes, DebuggingMode);
			}
		};
		ParseCVar(TEXT("r.GPUCrashDebugging"), ED3D12GPUCrashDebuggingModes((int)ED3D12GPUCrashDebuggingModes::BreadCrumbs | (int)ED3D12GPUCrashDebuggingModes::NvAftermath));
		ParseCVar(TEXT("r.D3D12.BreadCrumbs"), ED3D12GPUCrashDebuggingModes::BreadCrumbs);
		ParseCVar(TEXT("r.D3D12.NvAfterMath"), ED3D12GPUCrashDebuggingModes::NvAftermath);
		ParseCVar(TEXT("r.D3D12.DRED"), ED3D12GPUCrashDebuggingModes::DRED);

		// Enable/disable specific crash debugging modes if requested via command line argument
		const auto ParseCommandLine = [this](const TCHAR* CommandLineArgument, ED3D12GPUCrashDebuggingModes DebuggingMode)
		{
			int32 Value = 0;
			if (FParse::Value(FCommandLine::Get(), *FString::Printf(TEXT("%s="), CommandLineArgument), Value))
			{
				if (Value > 0)
				{
					EnumAddFlags(GPUCrashDebuggingModes, DebuggingMode);
				}
				else
				{
					EnumRemoveFlags(GPUCrashDebuggingModes, DebuggingMode);
				}
			}
			else  if (FParse::Param(FCommandLine::Get(), CommandLineArgument))
			{
				EnumAddFlags(GPUCrashDebuggingModes, DebuggingMode);
			}
		};
		ParseCommandLine(TEXT("gpubreadcrumbs"), ED3D12GPUCrashDebuggingModes::BreadCrumbs);
		ParseCommandLine(TEXT("nvaftermath"), ED3D12GPUCrashDebuggingModes::NvAftermath);
		ParseCommandLine(TEXT("dred"), ED3D12GPUCrashDebuggingModes::DRED);
	}

	// Submit draw events when any crash debugging mode is enabled
	if (GPUCrashDebuggingModes != ED3D12GPUCrashDebuggingModes::None)
	{
		SetEmitDrawEvents(true);
	}

	bool bBreadcrumbs = EnumHasAnyFlags(GPUCrashDebuggingModes, ED3D12GPUCrashDebuggingModes::BreadCrumbs);
	FGenericCrashContext::SetEngineData(TEXT("RHI.Breadcrumbs"), bBreadcrumbs ? TEXT("true") : TEXT("false"));

#if NV_AFTERMATH
	if (IsRHIDeviceNVIDIA() && GDX12NVAfterMathModuleLoaded)
	{
		// GPUcrash dump handler must be attached prior to device creation
		if (EnumHasAnyFlags(GPUCrashDebuggingModes, ED3D12GPUCrashDebuggingModes::NvAftermath))
		{
			HANDLE CurrentThread = ::GetCurrentThread();

			GFSDK_Aftermath_Result Result = GFSDK_Aftermath_EnableGpuCrashDumps(
				GFSDK_Aftermath_Version_API,
				GFSDK_Aftermath_GpuCrashDumpWatchedApiFlags_DX,
				GFSDK_Aftermath_GpuCrashDumpFeatureFlags_Default,
				D3D12AftermathCrashCallback,
				nullptr, //Shader debug callback
				nullptr, // description callback
				CurrentThread); // user data

			if (Result == GFSDK_Aftermath_Result_Success)
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("[Aftermath] Aftermath crash dumping enabled"));

				// enable core Aftermath to set the init flags
				GDX12NVAfterMathEnabled = 1;
			}
			else
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("[Aftermath] Aftermath crash dumping failed to initialize (%x)"), Result);

				GDX12NVAfterMathEnabled = 0;
			}
		}
	}
#endif

	// Setup DRED if requested
	bool bDRED = false;
	bool bDREDContext = false;
	if (EnumHasAnyFlags(GPUCrashDebuggingModes, ED3D12GPUCrashDebuggingModes::DRED))
	{
		TRefCountPtr<ID3D12DeviceRemovedExtendedDataSettings> DredSettings;
		HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(DredSettings.GetInitReference()));

		// Can fail if not on correct Windows Version - needs 1903 or newer
		if (SUCCEEDED(hr))
		{
			// Turn on AutoBreadcrumbs and Page Fault reporting
			DredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			DredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);

			bDRED = true;
			UE_LOG(LogD3D12RHI, Log, TEXT("[DRED] Dred enabled"));
		}
		else
		{
			UE_LOG(LogD3D12RHI, Log, TEXT("[DRED] DRED requested but interface was not found, hresult: %x. DRED only works on Windows 10 1903+."), hr);
		}

#ifdef __ID3D12DeviceRemovedExtendedDataSettings1_INTERFACE_DEFINED__
		TRefCountPtr<ID3D12DeviceRemovedExtendedDataSettings1> DredSettings1;
		hr = D3D12GetDebugInterface(IID_PPV_ARGS(DredSettings1.GetInitReference()));
		if (SUCCEEDED(hr))
		{
			DredSettings1->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			bDREDContext = true;
			UE_LOG(LogD3D12RHI, Log, TEXT("[DRED] Dred breadcrumb context enabled"));
		}
#endif
	}

	FGenericCrashContext::SetEngineData(TEXT("RHI.DRED"), bDRED ? TEXT("true") : TEXT("false"));
	FGenericCrashContext::SetEngineData(TEXT("RHI.DREDContext"), bDREDContext ? TEXT("true") : TEXT("false"));

#endif // PLATFORM_WINDOWS || (PLATFORM_HOLOLENS && !UE_BUILD_SHIPPING && WITH_PIX_EVENT_RUNTIME)

#if USE_PIX
	UE_LOG(LogD3D12RHI, Log, TEXT("Emitting draw events for PIX profiling."));
	SetEmitDrawEvents(true);
#endif

	CreateDXGIFactory(bWithDebug);

	// QI for the Adapter
	TRefCountPtr<IDXGIAdapter> TempAdapter;
#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
	Desc.EnumAdapters(DxgiFactory, DxgiFactory6, TempAdapter.GetInitReference());
#else
	DxgiFactory->EnumAdapters(Desc.AdapterIndex, TempAdapter.GetInitReference());
#endif
	VERIFYD3D12RESULT(TempAdapter->QueryInterface(IID_PPV_ARGS(DxgiAdapter.GetInitReference())));

	bool bDeviceCreated = false;
#if !PLATFORM_CPU_ARM_FAMILY && (PLATFORM_WINDOWS)
	if (IsRHIDeviceAMD() && OwningRHI->GetAmdAgsContext())
	{
		auto* CVarShaderDevelopmentMode = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.ShaderDevelopmentMode"));
		auto* CVarDisableEngineAndAppRegistration = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.DisableEngineAndAppRegistration"));

		const bool bDisableEngineRegistration = (CVarShaderDevelopmentMode && CVarShaderDevelopmentMode->GetValueOnAnyThread() != 0) ||
			(CVarDisableEngineAndAppRegistration && CVarDisableEngineAndAppRegistration->GetValueOnAnyThread() != 0);
		const bool bDisableAppRegistration = bDisableEngineRegistration || !FApp::HasProjectName();

		// Creating the Direct3D device with AGS registration and extensions.
		AGSDX12DeviceCreationParams AmdDeviceCreationParams = {
			GetAdapter(),											// IDXGIAdapter*               pAdapter;
			__uuidof(**(RootDevice.GetInitReference())),			// IID                         iid;
			GetFeatureLevel(),										// D3D_FEATURE_LEVEL           FeatureLevel;
		};

		AGSDX12ExtensionParams AmdExtensionParams;
		FMemory::Memzero(&AmdExtensionParams, sizeof(AmdExtensionParams));

		// Register the engine name with the AMD driver, e.g. "UnrealEngine4.19", unless disabled
		// (note: to specify nothing for pEngineName below, you need to pass an empty string, not a null pointer)
		FString EngineName = FApp::GetEpicProductIdentifier() + FEngineVersion::Current().ToString(EVersionComponent::Minor);
		AmdExtensionParams.pEngineName = bDisableEngineRegistration ? TEXT("") : *EngineName;
		AmdExtensionParams.engineVersion = AGS_UNSPECIFIED_VERSION;

		// Register the project name with the AMD driver, unless disabled or no project name
		// (note: to specify nothing for pAppName below, you need to pass an empty string, not a null pointer)
		AmdExtensionParams.pAppName = bDisableAppRegistration ? TEXT("") : FApp::GetProjectName();
		AmdExtensionParams.appVersion = AGS_UNSPECIFIED_VERSION;

		// From Shaders\Shared\ThirdParty\AMD\ags_shader_intrinsics_dx12.h, the default dummy UAV used
		// to access shader intrinsics is declared as below:
		// RWByteAddressBuffer AmdExtD3DShaderIntrinsicsUAV : register(u0, AmdExtD3DShaderIntrinsicsSpaceId);
		// So, use slot 0 here to match.
		AmdExtensionParams.uavSlot = 0;

		AGSDX12ReturnedParams DeviceCreationReturnedParams;
		FMemory::Memzero(&DeviceCreationReturnedParams, sizeof(DeviceCreationReturnedParams));
		AGSReturnCode DeviceCreation = agsDriverExtensionsDX12_CreateDevice(
			OwningRHI->GetAmdAgsContext(),
			&AmdDeviceCreationParams,
			&AmdExtensionParams,
			&DeviceCreationReturnedParams
		);

		if (DeviceCreation == AGS_SUCCESS)
		{
			RootDevice = DeviceCreationReturnedParams.pDevice;
			{
				static_assert(sizeof(AGSDX12ReturnedParams::ExtensionsSupported) == sizeof(uint32));
				uint32 AMDSupportedExtensionFlags;
				FMemory::Memcpy(&AMDSupportedExtensionFlags, &DeviceCreationReturnedParams.extensionsSupported, sizeof(uint32));
				OwningRHI->SetAmdSupportedExtensionFlags(AMDSupportedExtensionFlags);
			}
			bDeviceCreated = true;
		}
	}
#endif

	if (!bDeviceCreated)
	{
		// Creating the Direct3D device.
		VERIFYD3D12RESULT(D3D12CreateDevice(
			GetAdapter(),
			GetFeatureLevel(),
			IID_PPV_ARGS(RootDevice.GetInitReference())
		));
	}

	// Detect availability of shader model 6.0 wave operations
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS1 Features = {};
		RootDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &Features, sizeof(Features));
		GRHISupportsWaveOperations = Features.WaveOps;
		GRHIMinimumWaveSize = Features.WaveLaneCountMin;
		GRHIMaximumWaveSize = Features.WaveLaneCountMax;
	}

#if ENABLE_RESIDENCY_MANAGEMENT
	if (!CVarResidencyManagement.GetValueOnAnyThread())
	{
		UE_LOG(LogD3D12RHI, Log, TEXT("D3D12 resource residency management is disabled."));
		GEnableResidencyManagement = false;
	}
#endif // ENABLE_RESIDENCY_MANAGEMENT

#if PLATFORM_WINDOWS
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS7 Features = {};
		if (SUCCEEDED(RootDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &Features, sizeof(Features))))
		{
			bHeapNotZeroedSupported = true;

			GRHISupportsMeshShadersTier1 = (GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM6) && (Features.MeshShaderTier == D3D12_MESH_SHADER_TIER_1);
			GRHISupportsMeshShadersTier0 = GRHISupportsMeshShadersTier1;
		}
	}
#endif

#if NV_AFTERMATH
	// Enable aftermath when GPU crash debugging is enabled
	if (EnumHasAnyFlags(GPUCrashDebuggingModes, ED3D12GPUCrashDebuggingModes::NvAftermath) && GDX12NVAfterMathEnabled)
	{
		if (IsRHIDeviceNVIDIA() && bAllowVendorDevice)
		{
			static IConsoleVariable* MarkersCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUCrashDebugging.Aftermath.Markers"));
			static IConsoleVariable* CallstackCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUCrashDebugging.Aftermath.Callstack"));
			static IConsoleVariable* ResourcesCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUCrashDebugging.Aftermath.ResourceTracking"));
			static IConsoleVariable* TrackAllCVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.GPUCrashDebugging.Aftermath.TrackAll"));

			const bool bEnableMarkers = FParse::Param(FCommandLine::Get(), TEXT("aftermathmarkers")) || (MarkersCVar && MarkersCVar->GetInt());
			const bool bEnableCallstack = FParse::Param(FCommandLine::Get(), TEXT("aftermathcallstack")) || (CallstackCVar && CallstackCVar->GetInt());
			const bool bEnableResources = FParse::Param(FCommandLine::Get(), TEXT("aftermathresources")) || (ResourcesCVar && ResourcesCVar->GetInt());
			const bool bEnableAll = FParse::Param(FCommandLine::Get(), TEXT("aftermathall")) || (TrackAllCVar && TrackAllCVar->GetInt());

			uint32 Flags = GFSDK_Aftermath_FeatureFlags_Minimum;

			Flags |= bEnableMarkers ? GFSDK_Aftermath_FeatureFlags_EnableMarkers : 0;
			Flags |= bEnableCallstack ? GFSDK_Aftermath_FeatureFlags_CallStackCapturing : 0;
			Flags |= bEnableResources ? GFSDK_Aftermath_FeatureFlags_EnableResourceTracking : 0;
			Flags |= bEnableAll ? GFSDK_Aftermath_FeatureFlags_Maximum : 0;

			GFSDK_Aftermath_Result Result = GFSDK_Aftermath_DX12_Initialize(GFSDK_Aftermath_Version_API, (GFSDK_Aftermath_FeatureFlags)Flags, RootDevice);
			if (Result == GFSDK_Aftermath_Result_Success)
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("[Aftermath] Aftermath enabled and primed"));
			}
			else
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("[Aftermath] Aftermath enabled but failed to initialize (%x)"), Result);
				GDX12NVAfterMathEnabled = 0;
			}

			if (GDX12NVAfterMathEnabled && (bEnableMarkers || bEnableAll))
			{
				SetEmitDrawEvents(true);
				GDX12NVAfterMathMarkers = 1;
			}

			GDX12NVAfterMathTrackResources = bEnableResources || bEnableAll;
			if (GDX12NVAfterMathEnabled && GDX12NVAfterMathTrackResources)
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("[Aftermath] Aftermath resource tracking enabled"));
			}
		}
		else
		{
			GDX12NVAfterMathEnabled = 0;
			UE_LOG(LogD3D12RHI, Warning, TEXT("[Aftermath] Skipping aftermath initialization on non-Nvidia device"));
		}
	}
	else
	{
		GDX12NVAfterMathEnabled = 0;
	}

	FGenericCrashContext::SetEngineData(TEXT("RHI.Aftermath"), GDX12NVAfterMathEnabled ? TEXT("true") : TEXT("false"));
#endif

#if PLATFORM_WINDOWS
	if (bWithDebug)
	{
		// add vectored exception handler to write the debug device warning & error messages to the log
		ExceptionHandlerHandle = AddVectoredExceptionHandler(1, D3DVectoredExceptionHandler);
	}
#endif // PLATFORM_WINDOWS

#if (PLATFORM_WINDOWS || PLATFORM_HOLOLENS)
	if (bWithDebug)
	{
		// Manually load dxgi debug if available
		HMODULE DxgiDebugDLL = (HMODULE)FPlatformProcess::GetDllHandle(TEXT("dxgidebug.dll"));
		if (DxgiDebugDLL)
		{
			typedef HRESULT(WINAPI* FDXGIGetDebugInterface)(REFIID, void**);
#pragma warning(push)
#pragma warning(disable: 4191) // disable the "unsafe conversion from 'FARPROC' to 'blah'" warning
			FDXGIGetDebugInterface DXGIGetDebugInterfaceFnPtr = (FDXGIGetDebugInterface)(GetProcAddress(DxgiDebugDLL, "DXGIGetDebugInterface"));
#pragma warning(pop)
			if (DXGIGetDebugInterfaceFnPtr != nullptr)
			{
				DXGIGetDebugInterfaceFnPtr(__uuidof(IDXGIDebug), (void**)DXGIDebug.GetInitReference());
			}

			FPlatformProcess::FreeDllHandle(DxgiDebugDLL);
		}
	}
#endif //  (PLATFORM_WINDOWS || PLATFORM_HOLOLENS)

#if UE_BUILD_DEBUG	&& (PLATFORM_WINDOWS || PLATFORM_HOLOLENS)
	//break on debug
	TRefCountPtr<ID3D12Debug> d3dDebug;
	if (SUCCEEDED(RootDevice->QueryInterface(__uuidof(ID3D12Debug), (void**)d3dDebug.GetInitReference())))
	{
		TRefCountPtr<ID3D12InfoQueue> d3dInfoQueue;
		if (SUCCEEDED(d3dDebug->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)d3dInfoQueue.GetInitReference())))
		{
			d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
			d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
			//d3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
		}
	}
#endif

#if !(UE_BUILD_SHIPPING && WITH_EDITOR) && (PLATFORM_WINDOWS || PLATFORM_HOLOLENS)
	// Add some filter outs for known debug spew messages (that we don't care about)
	if (bWithDebug)
	{
		ID3D12InfoQueue *pd3dInfoQueue = nullptr;
		VERIFYD3D12RESULT(RootDevice->QueryInterface(__uuidof(ID3D12InfoQueue), (void**)&pd3dInfoQueue));
		if (pd3dInfoQueue)
		{
			D3D12_INFO_QUEUE_FILTER NewFilter;
			FMemory::Memzero(&NewFilter, sizeof(NewFilter));

			// Turn off info msgs as these get really spewy
			D3D12_MESSAGE_SEVERITY DenySeverity = D3D12_MESSAGE_SEVERITY_INFO;
			NewFilter.DenyList.NumSeverities = 1;
			NewFilter.DenyList.pSeverityList = &DenySeverity;

			// Be sure to carefully comment the reason for any additions here!  Someone should be able to look at it later and get an idea of whether it is still necessary.
			TArray<D3D12_MESSAGE_ID, TInlineAllocator<16>> DenyIds = {

				// The Pixel Shader expects a Render Target View bound to slot 0, but the PSO indicates that none will be bound.
				// This typically happens when a non-depth-only pixel shader is used for depth-only rendering.
				D3D12_MESSAGE_ID_CREATEGRAPHICSPIPELINESTATE_RENDERTARGETVIEW_NOT_SET,

				// QUERY_BEGIN_ABANDONING_PREVIOUS_RESULTS - The RHI exposes the interface to make and issue queries and a separate interface to use that data.
				//		Currently there is a situation where queries are issued and the results may be ignored on purpose.  Filtering out this message so it doesn't
				//		swarm the debug spew and mask other important warnings
				//D3D12_MESSAGE_ID_QUERY_BEGIN_ABANDONING_PREVIOUS_RESULTS,
				//D3D12_MESSAGE_ID_QUERY_END_ABANDONING_PREVIOUS_RESULTS,

				// D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_EMPTY_LAYOUT - This is a warning that gets triggered if you use a null vertex declaration,
				//       which we want to do when the vertex shader is generating vertices based on ID.
				D3D12_MESSAGE_ID_CREATEINPUTLAYOUT_EMPTY_LAYOUT,

				// D3D12_MESSAGE_ID_COMMAND_LIST_DRAW_INDEX_BUFFER_TOO_SMALL - This warning gets triggered by Slate draws which are actually using a valid index range.
				//		The invalid warning seems to only happen when VS 2012 is installed.  Reported to MS.  
				//		There is now an assert in DrawIndexedPrimitive to catch any valid errors reading from the index buffer outside of range.
				D3D12_MESSAGE_ID_COMMAND_LIST_DRAW_INDEX_BUFFER_TOO_SMALL,

				// D3D12_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET - This warning gets triggered by shadow depth rendering because the shader outputs
				//		a color but we don't bind a color render target. That is safe as writes to unbound render targets are discarded.
				//		Also, batched elements triggers it when rendering outside of scene rendering as it outputs to the GBuffer containing normals which is not bound.
				//(D3D12_MESSAGE_ID)3146081, // D3D12_MESSAGE_ID_DEVICE_DRAW_RENDERTARGETVIEW_NOT_SET,
				// BUGBUG: There is a D3D12_MESSAGE_ID_DEVICE_DRAW_DEPTHSTENCILVIEW_NOT_SET, why not one for RT?

				// D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE/D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE - 
				//      This warning gets triggered by ClearDepthStencilView/ClearRenderTargetView because when the resource was created
				//      it wasn't passed an optimized clear color (see CreateCommitedResource). This shows up a lot and is very noisy.
				D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
				D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,

				// D3D12_MESSAGE_ID_EXECUTECOMMANDLISTS_GPU_WRITTEN_READBACK_RESOURCE_MAPPED - This warning gets triggered by ExecuteCommandLists.
				//		if it contains a readback resource that still has mapped subresources when executing a command list that performs a copy operation to the resource.
				//		This may be ok if any data read from the readback resources was flushed by calling Unmap() after the resourcecopy operation completed.
				//		We intentionally keep the readback resources persistently mapped.
				D3D12_MESSAGE_ID_EXECUTECOMMANDLISTS_GPU_WRITTEN_READBACK_RESOURCE_MAPPED,

				// This shows up a lot and is very noisy. It would require changes to the resource tracking system
				// but will hopefully be resolved when the RHI switches to use the engine's resource tracking system.
				D3D12_MESSAGE_ID_RESOURCE_BARRIER_DUPLICATE_SUBRESOURCE_TRANSITIONS,

				// This error gets generated on the first run when you install a new driver. The code handles this error properly and resets the PipelineLibrary,
				// so we can safely ignore this message. It could possibly be avoided by adding driver version to the PSO cache filename, but an average user is unlikely
				// to be interested in keeping PSO caches associated with old drivers around on disk, so it's better to just reset.
				D3D12_MESSAGE_ID_CREATEPIPELINELIBRARY_DRIVERVERSIONMISMATCH,

				// D3D complain about overlapping GPU addresses when aliasing DataBuffers in the same command list when using the Transient Allocator - it looks like
				// it ignored the aliasing barriers to validate, and probably can't check them when called from IASetVertexBuffers because it only has GPU Virtual Addresses then
				D3D12_MESSAGE_ID_HEAP_ADDRESS_RANGE_INTERSECTS_MULTIPLE_BUFFERS,

				// Ignore draw vertex buffer not set or too small - these are warnings and if the shader doesn't read from it it's fine. This happens because vertex
				// buffers are not removed from the cache, but only get removed when another buffer is set at the same slot or when the buffer gets destroyed.
				D3D12_MESSAGE_ID_COMMAND_LIST_DRAW_VERTEX_BUFFER_NOT_SET,
				D3D12_MESSAGE_ID_COMMAND_LIST_DRAW_VERTEX_BUFFER_TOO_SMALL,

				// D3D12 complains when a buffer is created with a specific initial resource state while all buffers are currently created in COMMON state. The 
				// next transition is then done use state promotion. It's just a warning and we need to keep track of the correct initial state as well for upcoming
				// internal transitions.
				D3D12_MESSAGE_ID_CREATERESOURCE_STATE_IGNORED,

#if ENABLE_RESIDENCY_MANAGEMENT
				// TODO: Remove this when the debug layers work for executions which are guarded by a fence
				D3D12_MESSAGE_ID_INVALID_USE_OF_NON_RESIDENT_RESOURCE,
#endif
			};

#if PLATFORM_DESKTOP
			if (!FWindowsPlatformMisc::VerifyWindowsVersion(10, 0, 18363))
			{
				// Ignore a known false positive error due to a bug in validation layer in certain older Windows versions
				DenyIds.Add(D3D12_MESSAGE_ID_COPY_DESCRIPTORS_INVALID_RANGES);
			}
#endif // PLATFORM_DESKTOP

			NewFilter.DenyList.NumIDs = DenyIds.Num();
			NewFilter.DenyList.pIDList = DenyIds.GetData();

			pd3dInfoQueue->PushStorageFilter(&NewFilter);

			// Break on D3D debug errors.
			pd3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);

			// Enable this to break on a specific id in order to quickly get a callstack
			//pd3dInfoQueue->SetBreakOnID(D3D12_MESSAGE_ID_DEVICE_DRAW_CONSTANT_BUFFER_TOO_SMALL, true);

			if (FParse::Param(FCommandLine::Get(), TEXT("d3dbreakonwarning")))
			{
				pd3dInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);
			}

			pd3dInfoQueue->Release();
		}
	}
#endif

#if WITH_MGPU
	GNumExplicitGPUsForRendering = 1;
	if (Desc.NumDeviceNodes > 1)
	{
		// Can't access GAllowMultiGPUInEditor directly as its value is cached but hasn't been set by console manager due to module loading order
		static IConsoleVariable* AllowMultiGPUInEditor = IConsoleManager::Get().FindConsoleVariable(TEXT("r.AllowMultiGPUInEditor"));

		if (GIsEditor && AllowMultiGPUInEditor->GetInt() == 0)
		{
			UE_LOG(LogD3D12RHI, Log, TEXT("Multi-GPU is available, but skipping due to editor mode."));
		}
		else
		{
			GNumExplicitGPUsForRendering = Desc.NumDeviceNodes;
			UE_LOG(LogD3D12RHI, Log, TEXT("Enabling multi-GPU with %d nodes"), Desc.NumDeviceNodes);
		}
	}

	// Viewport ignores AFR if PresentGPU is specified.
	int32 Dummy;
	if (!FParse::Value(FCommandLine::Get(), TEXT("PresentGPU="), Dummy))
	{
		bool bWantsAFR = false;
		if (FParse::Value(FCommandLine::Get(), TEXT("NumAFRGroups="), GNumAlternateFrameRenderingGroups))
		{
			bWantsAFR = true;
		}
		else if (FParse::Param(FCommandLine::Get(), TEXT("AFR")))
		{
			bWantsAFR = true;
			GNumAlternateFrameRenderingGroups = GNumExplicitGPUsForRendering;
		}

		if (bWantsAFR)
		{
			if (GNumAlternateFrameRenderingGroups <= 1 || GNumAlternateFrameRenderingGroups > GNumExplicitGPUsForRendering)
			{
				UE_LOG(LogD3D12RHI, Error, TEXT("Cannot enable alternate frame rendering because NumAFRGroups (%u) must be > 1 and <= MaxGPUCount (%u)"), GNumAlternateFrameRenderingGroups, GNumExplicitGPUsForRendering);
				GNumAlternateFrameRenderingGroups = 1;
			}
			else if (GNumExplicitGPUsForRendering % GNumAlternateFrameRenderingGroups != 0)
			{
				UE_LOG(LogD3D12RHI, Error, TEXT("Cannot enable alternate frame rendering because MaxGPUCount (%u) must be evenly divisible by NumAFRGroups (%u)"), GNumExplicitGPUsForRendering, GNumAlternateFrameRenderingGroups);
				GNumAlternateFrameRenderingGroups = 1;
			}
			else
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("Enabling alternate frame rendering with %u AFR groups"), GNumAlternateFrameRenderingGroups);
			}
		}
	}
#endif
}

FD3D12TransientHeapCache& FD3D12Adapter::GetOrCreateTransientHeapCache()
{
	if (!TransientMemoryCache)
	{
		TransientMemoryCache = FD3D12TransientHeapCache::Create(this, FRHIGPUMask::All());
	}

	return static_cast<FD3D12TransientHeapCache&>(*TransientMemoryCache);
}

void FD3D12Adapter::InitializeDevices()
{
	check(IsInGameThread());

	// Wait for the rendering thread to go idle.
	SCOPED_SUSPEND_RENDERING_THREAD(false);

	// If the device we were using has been removed, release it and the resources we created for it.
	if (bDeviceRemoved)
	{
		check(RootDevice);

		HRESULT hRes = RootDevice->GetDeviceRemovedReason();

		const TCHAR* Reason = TEXT("?");
		switch (hRes)
		{
		case DXGI_ERROR_DEVICE_HUNG:			Reason = TEXT("HUNG"); break;
		case DXGI_ERROR_DEVICE_REMOVED:			Reason = TEXT("REMOVED"); break;
		case DXGI_ERROR_DEVICE_RESET:			Reason = TEXT("RESET"); break;
		case DXGI_ERROR_DRIVER_INTERNAL_ERROR:	Reason = TEXT("INTERNAL_ERROR"); break;
		case DXGI_ERROR_INVALID_CALL:			Reason = TEXT("INVALID_CALL"); break;
		}

		bDeviceRemoved = false;

		Cleanup();

		// We currently don't support removed devices because FTexture2DResource can't recreate its RHI resources from scratch.
		// We would also need to recreate the viewport swap chains from scratch.
		UE_LOG(LogD3D12RHI, Fatal, TEXT("The Direct3D 12 device that was being used has been removed (Error: %d '%s').  Please restart the game."), hRes, Reason);
	}

	// Use a debug device if specified on the command line.
	bool bWithD3DDebug = D3D12RHI_ShouldCreateWithD3DDebug();

	// If we don't have a device yet, either because this is the first viewport, or the old device was removed, create a device.
	if (!RootDevice)
	{
		CreateRootDevice(bWithD3DDebug);

		// See if we can get any newer device interfaces (to use newer D3D12 features).
		if (D3D12RHI_ShouldForceCompatibility())
		{
			UE_LOG(LogD3D12RHI, Log, TEXT("Forcing D3D12 compatibility."));
		}
		else
		{
#if D3D12_MAX_DEVICE_INTERFACE >= 1
			if (SUCCEEDED(RootDevice->QueryInterface(IID_PPV_ARGS(RootDevice1.GetInitReference()))))
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("ID3D12Device1 is supported."));
			}
#endif
#if D3D12_MAX_DEVICE_INTERFACE >= 2
			if (SUCCEEDED(RootDevice->QueryInterface(IID_PPV_ARGS(RootDevice2.GetInitReference()))))
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("ID3D12Device2 is supported."));
			}

			if (RootDevice1 == nullptr || RootDevice2 == nullptr)
			{
				// Note: we require Windows 1703 in FD3D12DynamicRHIModule::IsSupported()
				// If we still lack support, the user's drivers could be out of date.
				UE_LOG(LogD3D12RHI, Fatal, TEXT("Missing full support for Direct3D 12. Please update to the latest drivers."));
			}
#endif
#if D3D12_MAX_DEVICE_INTERFACE >= 3
			if (SUCCEEDED(RootDevice->QueryInterface(IID_PPV_ARGS(RootDevice3.GetInitReference()))))
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("ID3D12Device3 is supported."));
			}
#endif
#if D3D12_MAX_DEVICE_INTERFACE >= 4
			if (SUCCEEDED(RootDevice->QueryInterface(IID_PPV_ARGS(RootDevice4.GetInitReference()))))
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("ID3D12Device4 is supported."));
			}
#endif
#if D3D12_MAX_DEVICE_INTERFACE >= 5
			if (SUCCEEDED(RootDevice->QueryInterface(IID_PPV_ARGS(RootDevice5.GetInitReference()))))
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("ID3D12Device5 is supported."));
			}
#endif
#if D3D12_MAX_DEVICE_INTERFACE >= 6
			if (SUCCEEDED(RootDevice->QueryInterface(IID_PPV_ARGS(RootDevice6.GetInitReference()))))
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("ID3D12Device6 is supported."));
			}
#endif
#if D3D12_MAX_DEVICE_INTERFACE >= 7
			if (SUCCEEDED(RootDevice->QueryInterface(IID_PPV_ARGS(RootDevice7.GetInitReference()))))
			{
				UE_LOG(LogD3D12RHI, Log, TEXT("ID3D12Device7 is supported."));
			}
#endif

			D3D12_FEATURE_DATA_D3D12_OPTIONS D3D12Caps;
			FMemory::Memzero(&D3D12Caps, sizeof(D3D12Caps));
			VERIFYD3D12RESULT(RootDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &D3D12Caps, sizeof(D3D12Caps)));
			ResourceHeapTier = D3D12Caps.ResourceHeapTier;
			ResourceBindingTier = D3D12Caps.ResourceBindingTier;

#if D3D12_RHI_RAYTRACING
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 D3D12Caps5 = {};
			if (SUCCEEDED(RootDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &D3D12Caps5, sizeof(D3D12Caps5))))
			{
				if (D3D12Caps5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0
					&& D3D12Caps.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_2
					&& RootDevice5
					&& FDataDrivenShaderPlatformInfo::GetSupportsRayTracing(GMaxRHIShaderPlatform)
					&& !FParse::Param(FCommandLine::Get(), TEXT("noraytracing")))
				{
					UE_LOG(LogD3D12RHI, Log, TEXT("D3D12 ray tracing tier 1.0 is supported."));

					GRHISupportsRayTracing = RHISupportsRayTracing(GMaxRHIShaderPlatform);
					GRHISupportsRayTracingShaders = GRHISupportsRayTracing && RHISupportsRayTracingShaders(GMaxRHIShaderPlatform);

					if (D3D12Caps5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1
						&& RootDevice7)
					{
						UE_LOG(LogD3D12RHI, Log, TEXT("D3D12 ray tracing tier 1.1 is supported."));

						GRHISupportsRayTracingPSOAdditions = true;
						GRHISupportsInlineRayTracing = GRHISupportsRayTracing && RHISupportsInlineRayTracing(GMaxRHIShaderPlatform) && (GMaxRHIFeatureLevel >= ERHIFeatureLevel::SM6);
					}
				}
				else if (D3D12Caps5.RaytracingTier == D3D12_RAYTRACING_TIER_NOT_SUPPORTED 
					&& FModuleManager::Get().IsModuleLoaded("RenderDocPlugin")
					&& !FParse::Param(FCommandLine::Get(), TEXT("noraytracing")))
				{
					UE_LOG(LogD3D12RHI, Warning, TEXT("Ray Tracing is disabled because the RenderDoc plugin is currently not compatible with D3D12 ray tracing."));
				}
			}

			GRHIRayTracingAccelerationStructureAlignment = uint32(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			GRHIRayTracingScratchBufferAlignment = uint32(D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT);
			GRHIRayTracingShaderTableAlignment = uint32(D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT);
			GRHIRayTracingInstanceDescriptorSize = uint32(sizeof(D3D12_RAYTRACING_INSTANCE_DESC));

#endif // D3D12_RHI_RAYTRACING

#if PLATFORM_WINDOWS && D3D12_CORE_ENABLED
			{
				D3D12_FEATURE_DATA_D3D12_OPTIONS7 D3D12Caps7 = {};
				RootDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS7, &D3D12Caps7, sizeof(D3D12Caps7));

				D3D12_FEATURE_DATA_D3D12_OPTIONS9 D3D12Caps9 = {};
				RootDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS9, &D3D12Caps9, sizeof(D3D12Caps9));

				D3D12_FEATURE_DATA_D3D12_OPTIONS11 D3D12Caps11 = {};
				RootDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &D3D12Caps11, sizeof(D3D12Caps11));

				if (D3D12Caps7.MeshShaderTier >= D3D12_MESH_SHADER_TIER_1)
				{
					UE_LOG(LogD3D12RHI, Log, TEXT("Mesh shader tier 1.0 is supported"));
				}

				if (D3D12Caps9.AtomicInt64OnTypedResourceSupported)
				{
					UE_LOG(LogD3D12RHI, Log, TEXT("AtomicInt64OnTypedResource is supported"));
				}

				if (D3D12Caps9.AtomicInt64OnGroupSharedSupported)
				{
					UE_LOG(LogD3D12RHI, Log, TEXT("AtomicInt64OnGroupShared is supported"));
				}

				if (D3D12Caps11.AtomicInt64OnDescriptorHeapResourceSupported)
				{
					UE_LOG(LogD3D12RHI, Log, TEXT("AtomicInt64OnDescriptorHeapResource is supported"));
				}

				if (D3D12Caps9.AtomicInt64OnTypedResourceSupported && D3D12Caps11.AtomicInt64OnDescriptorHeapResourceSupported)
				{
					GRHISupportsDX12AtomicUInt64 = true;
				}

				if (GRHISupportsDX12AtomicUInt64)
				{
					UE_LOG(LogD3D12RHI, Log, TEXT("Shader Model 6.6 atomic64 is supported"));
				}
				else
				{
					UE_LOG(LogD3D12RHI, Log, TEXT("Shader Model 6.6 atomic64 is not supported"));
				}
			}
#endif // PLATFORM_WINDOWS && D3D12_CORE_ENABLED
		}

#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
		D3D12_FEATURE_DATA_D3D12_OPTIONS2 D3D12Caps2 = {};
		if (FAILED(RootDevice->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS2, &D3D12Caps2, sizeof(D3D12Caps2))))
		{
			D3D12Caps2.DepthBoundsTestSupported = false;
			D3D12Caps2.ProgrammableSamplePositionsTier = D3D12_PROGRAMMABLE_SAMPLE_POSITIONS_TIER_NOT_SUPPORTED;
		}
		bDepthBoundsTestSupported = !!D3D12Caps2.DepthBoundsTestSupported;
#endif

		D3D12_FEATURE_DATA_ROOT_SIGNATURE D3D12RootSignatureCaps = {};
		D3D12RootSignatureCaps.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_1;	// This is the highest version we currently support. If CheckFeatureSupport succeeds, the HighestVersion returned will not be greater than this.
		if (FAILED(RootDevice->CheckFeatureSupport(D3D12_FEATURE_ROOT_SIGNATURE, &D3D12RootSignatureCaps, sizeof(D3D12RootSignatureCaps))))
		{
			D3D12RootSignatureCaps.HighestVersion = D3D_ROOT_SIGNATURE_VERSION_1_0;
		}
		RootSignatureVersion = D3D12RootSignatureCaps.HighestVersion;

		FrameFence = new FD3D12ManualFence(this, FRHIGPUMask::All(), L"Adapter Frame Fence");
		FrameFence->CreateFence();

		StagingFence = new FD3D12Fence(this, FRHIGPUMask::All(), L"Staging Fence");
		StagingFence->CreateFence();

#if TRACK_RESOURCE_ALLOCATIONS
		// Set flag if we want to track all allocations - comes with some overhead and only possible when Tier 2 is available
		// (because we will create placed buffers for texture allocation to retrieve the GPU virtual addresses)
		bTrackAllAllocation = (GD3D12TrackAllAlocations || GPUCrashDebuggingModes == ED3D12GPUCrashDebuggingModes::All) && (ResourceHeapTier == D3D12_RESOURCE_HEAP_TIER_2);
#endif 

		CreateCommandSignatures();

		// Context redirectors allow RHI commands to be executed on multiple GPUs at the
		// same time in a multi-GPU system. Redirectors have a physical mask for the GPUs
		// they can support and an active mask which restricts commands to operate on a
		// subset of the physical GPUs. The default context redirectors used by the
		// immediate command list can support all physical GPUs, whereas context containers
		// used by the parallel command lists might only support a subset of GPUs in the
		// system.
		DefaultContextRedirector.SetPhysicalGPUMask(FRHIGPUMask::All());
		DefaultAsyncComputeContextRedirector.SetPhysicalGPUMask(FRHIGPUMask::All());

		// Create all of the FD3D12Devices.
		for (uint32 GPUIndex : FRHIGPUMask::All())
		{
			Devices[GPUIndex] = new FD3D12Device(FRHIGPUMask::FromIndex(GPUIndex), this);
			Devices[GPUIndex]->Initialize();

			// The redirectors allow to broadcast to any GPU set
			DefaultContextRedirector.SetPhysicalContext(&Devices[GPUIndex]->GetDefaultCommandContext());
			if (GEnableAsyncCompute)
			{
				DefaultAsyncComputeContextRedirector.SetPhysicalContext(&Devices[GPUIndex]->GetDefaultAsyncComputeContext());
			}
		}
		const FString Name(L"Upload Buffer Allocator");

		for (uint32 GPUIndex : FRHIGPUMask::All())
		{
			// Safe to init as we have a device;
			UploadHeapAllocator[GPUIndex] = new FD3D12UploadHeapAllocator(this,	Devices[GPUIndex], Name);
			UploadHeapAllocator[GPUIndex]->Init();
		}


		// ID3D12Device1::CreatePipelineLibrary() requires each blob to be specific to the given adapter. To do this we create a unique file name with from the adpater desc. 
		// Note that : "The uniqueness of an LUID is guaranteed only until the system is restarted" according to windows doc and thus can not be reused.
		const FString UniqueDeviceCachePath = FString::Printf(TEXT("V%d_D%d_S%d_R%d.ushaderprecache"), Desc.Desc.VendorId, Desc.Desc.DeviceId, Desc.Desc.SubSysId, Desc.Desc.Revision);
		FString GraphicsCacheFile = PIPELINE_STATE_FILE_LOCATION / FString::Printf(TEXT("D3DGraphics_%s"), *UniqueDeviceCachePath);
	    FString ComputeCacheFile = PIPELINE_STATE_FILE_LOCATION / FString::Printf(TEXT("D3DCompute_%s"), *UniqueDeviceCachePath);
		FString DriverBlobFilename = PIPELINE_STATE_FILE_LOCATION / FString::Printf(TEXT("D3DDriverByteCodeBlob_%s"), *UniqueDeviceCachePath);

		PipelineStateCache.Init(GraphicsCacheFile, ComputeCacheFile, DriverBlobFilename);

		ID3D12RootSignature* StaticGraphicsRS = (GetStaticGraphicsRootSignature()) ? GetStaticGraphicsRootSignature()->GetRootSignature() : nullptr;
		ID3D12RootSignature* StaticComputeRS = (GetStaticComputeRootSignature()) ? GetStaticComputeRootSignature()->GetRootSignature() : nullptr;

		PipelineStateCache.RebuildFromDiskCache(StaticGraphicsRS, StaticComputeRS);
	}
}

void FD3D12Adapter::InitializeRayTracing()
{
#if D3D12_RHI_RAYTRACING
	for (uint32 GPUIndex : FRHIGPUMask::All())
	{
		if (Devices[GPUIndex]->GetDevice5())
		{
			Devices[GPUIndex]->InitRayTracing();
		}
	}
#endif // D3D12_RHI_RAYTRACING
}

void FD3D12Adapter::CreateCommandSignatures()
{
	ID3D12Device* Device = GetD3DDevice();

	// ExecuteIndirect command signatures
	D3D12_COMMAND_SIGNATURE_DESC commandSignatureDesc = {};
	commandSignatureDesc.NumArgumentDescs = 1;
	commandSignatureDesc.ByteStride = 20;
	commandSignatureDesc.NodeMask = FRHIGPUMask::All().GetNative();

	D3D12_INDIRECT_ARGUMENT_DESC indirectParameterDesc[1] = {};
	commandSignatureDesc.pArgumentDescs = indirectParameterDesc;

	indirectParameterDesc[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
	commandSignatureDesc.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
	VERIFYD3D12RESULT(Device->CreateCommandSignature(&commandSignatureDesc, nullptr, IID_PPV_ARGS(DrawIndirectCommandSignature.GetInitReference())));

	indirectParameterDesc[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
	commandSignatureDesc.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
	VERIFYD3D12RESULT(Device->CreateCommandSignature(&commandSignatureDesc, nullptr, IID_PPV_ARGS(DrawIndexedIndirectCommandSignature.GetInitReference())));

	indirectParameterDesc[0].Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
	commandSignatureDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
	VERIFYD3D12RESULT(Device->CreateCommandSignature(&commandSignatureDesc, nullptr, IID_PPV_ARGS(DispatchIndirectGraphicsCommandSignature.GetInitReference())));

	checkf(DispatchIndirectComputeCommandSignature.IsValid(), TEXT("Indirect compute dispatch command signature is expected to be created by platform-specific D3D12 adapter implementation."))
}


void FD3D12Adapter::Cleanup()
{
	// Reset the RHI initialized flag.
	GIsRHIInitialized = false;

	for (auto& Viewport : Viewports)
	{
		Viewport->IssueFrameEvent();
		Viewport->WaitForFrameEventCompletion();
	}

	BlockUntilIdle();

#if D3D12_RHI_RAYTRACING
	for (uint32 GPUIndex : FRHIGPUMask::All())
	{
		Devices[GPUIndex]->CleanupRayTracing();
	}
#endif // D3D12_RHI_RAYTRACING

#if WITH_MGPU
	// Manually destroy the effects as we can't do it in their destructor.
	for (auto& Effect : TemporalEffectMap)
	{
		Effect.Value.Destroy();
	}
#endif

	// Ask all initialized FRenderResources to release their RHI resources.
	FRenderResource::ReleaseRHIForAllResources();

	{
		FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
		FRHIResource::FlushPendingDeletes(RHICmdList);
		RHICmdList.ImmediateFlush(EImmediateFlushType::FlushRHIThread);
	}

	// Release allocation data of all thread local transient uniform buffer allocators
	for (FTransientUniformBufferAllocator* Allocator : TransientUniformBufferAllocators)
	{
		Allocator->Cleanup();
	}
	TransientUniformBufferAllocators.Empty();

	// Cleanup resources
	DeferredDeletionQueue.ReleaseResources(true, true);

	// First clean up everything before deleting as there are shared resource location between devices.
	for (uint32 GPUIndex : FRHIGPUMask::All())
	{
		Devices[GPUIndex]->Cleanup();
	}	

	for (uint32 GPUIndex : FRHIGPUMask::All())
	{
		delete(Devices[GPUIndex]);
		Devices[GPUIndex] = nullptr;
	}

	Viewports.Empty();
	DrawingViewport = nullptr;

	for (uint32 GPUIndex : FRHIGPUMask::All())
	{
		UploadHeapAllocator[GPUIndex]->Destroy();
		delete(UploadHeapAllocator[GPUIndex]);
		UploadHeapAllocator[GPUIndex] = nullptr;
	}

	if (FrameFence)
	{
		FrameFence->Destroy();
		FrameFence.SafeRelease();
	}

	if (StagingFence)
	{
		StagingFence->Destroy();
		StagingFence.SafeRelease();
	}

	TransientMemoryCache.Reset();

	PipelineStateCache.Close();
	RootSignatureManager.Destroy();

	DrawIndirectCommandSignature.SafeRelease();
	DrawIndexedIndirectCommandSignature.SafeRelease();
	DispatchIndirectGraphicsCommandSignature.SafeRelease();
	DispatchIndirectComputeCommandSignature.SafeRelease();
	DispatchRaysIndirectCommandSignature.SafeRelease();

	FenceCorePool.Destroy();

#if (PLATFORM_WINDOWS || PLATFORM_HOLOLENS)
	// trace all leak D3D resource
	if (DXGIDebug != nullptr)
	{
		DXGIDebug->ReportLiveObjects(
			GUID{ 0xe48ae283, 0xda80, 0x490b, { 0x87, 0xe6, 0x43, 0xe9, 0xa9, 0xcf, 0xda, 0x8 } }, // DXGI_DEBUG_ALL
			DXGI_DEBUG_RLO_FLAGS(DXGI_DEBUG_RLO_DETAIL | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
		DXGIDebug.SafeRelease();

		CheckD3DStoredMessages();
	}
#endif //  (PLATFORM_WINDOWS || PLATFORM_HOLOLENS)

#if PLATFORM_WINDOWS
	if (ExceptionHandlerHandle != INVALID_HANDLE_VALUE)
	{
		RemoveVectoredExceptionHandler(ExceptionHandlerHandle);
	}
#endif //  PLATFORM_WINDOWS
}

void FD3D12Adapter::CreateDXGIFactory(bool bWithDebug)
{
#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
	uint32 Flags = bWithDebug ? DXGI_CREATE_FACTORY_DEBUG : 0;

#if PLATFORM_WINDOWS
	typedef HRESULT(WINAPI* FCreateDXGIFactory2)(UINT, REFIID, void**);
	FCreateDXGIFactory2 CreateDXGIFactory2FnPtr = nullptr;

	// Dynamically load this otherwise Win7 fails to boot as it's missing on that DLL
	HMODULE DxgiDLL = (HMODULE)FPlatformProcess::GetDllHandle(TEXT("dxgi.dll"));
	check(DxgiDLL);
#pragma warning(push)
#pragma warning(disable: 4191) // disable the "unsafe conversion from 'FARPROC' to 'blah'" warning
	CreateDXGIFactory2FnPtr = (FCreateDXGIFactory2)(GetProcAddress(DxgiDLL, "CreateDXGIFactory2"));
	check(CreateDXGIFactory2FnPtr);
#pragma warning(pop)
	FPlatformProcess::FreeDllHandle(DxgiDLL);

	VERIFYD3D12RESULT(CreateDXGIFactory2FnPtr(Flags, IID_PPV_ARGS(DxgiFactory.GetInitReference())));

	DxgiFactory->QueryInterface(IID_PPV_ARGS(DxgiFactory6.GetInitReference()));
#elif PLATFORM_HOLOLENS
	VERIFYD3D12RESULT(::CreateDXGIFactory2(Flags, IID_PPV_ARGS(DxgiFactory.GetInitReference())));
#endif

	VERIFYD3D12RESULT(DxgiFactory->QueryInterface(IID_PPV_ARGS(DxgiFactory2.GetInitReference())));
#endif // #if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
}

#if D3D12_SUBMISSION_GAP_RECORDER
void FD3D12Adapter::SubmitGapRecorderTimestamps()
{
	FD3D12Device* Device = GetDevice(0);
	if (GEnableGapRecorder && GGapRecorderActiveOnBeginFrame)
	{
		FrameCounter++;
		uint64 TotalSubmitWaitGPUCycles = 0;

		int32 CurrentSlotIdx = Device->GetCmdListExecTimeQueryHeap()->GetNextFreeIdx();
		SubmissionGapRecorder.SetEndFrameSlotIdx(CurrentSlotIdx);

		TArray<FD3D12CommandListManager::FResolvedCmdListExecTime> TimingPairs;
		Device->GetCommandListManager().GetCommandListTimingResults(TimingPairs, !!GGapRecorderUseBlockingCall);

		const int32 NumTimingPairs = TimingPairs.Num();
		StartOfSubmissionTimestamps.Empty(NumTimingPairs);
		EndOfSubmissionTimestamps.Empty(NumTimingPairs);

		// Convert Timing Pairs to flat arrays would be good to refactor data structures to make this unnecessary
		for (int32 i = 0; i < NumTimingPairs; i++)
		{
			StartOfSubmissionTimestamps.Add(TimingPairs[i].StartTimestamp);
			EndOfSubmissionTimestamps.Add(TimingPairs[i].EndTimestamp);
		}

		UE_LOG(LogD3D12GapRecorder, Verbose, TEXT("EndFrame TimingPairs %d StartOfSubmissionTimestamp %d EndOfSubmissionTimestamp %d"), NumTimingPairs, StartOfSubmissionTimestamps.Num(), EndOfSubmissionTimestamps.Num());

		// Process the timestamp submission gaps for the previous frame
		if (NumTimingPairs > 0)
		{
			TotalSubmitWaitGPUCycles = SubmissionGapRecorder.SubmitSubmissionTimestampsForFrame(FrameCounter, StartOfSubmissionTimestamps, EndOfSubmissionTimestamps);
		}

		double TotalSubmitWaitTimeSeconds = TotalSubmitWaitGPUCycles / (float)FGPUTiming::GetTimingFrequency();
		uint32 TotalSubmitWaitCycles = FPlatformMath::TruncToInt(TotalSubmitWaitTimeSeconds / FPlatformTime::GetSecondsPerCycle());

		UE_LOG(LogD3D12GapRecorder, VeryVerbose, TEXT("EndFrame TimingFrequency %lu TotalSubmitWaitTimeSeconds %f TotalSubmitWaitGPUCycles %lu TotalSubmitWaitCycles %u SecondsPerCycle %f"),
			FGPUTiming::GetTimingFrequency(),
			TotalSubmitWaitTimeSeconds,
			TotalSubmitWaitGPUCycles,
			TotalSubmitWaitCycles,
			FPlatformTime::GetSecondsPerCycle());

		if (GGPUFrameTime > 0)
		{
			UE_LOG(LogD3D12GapRecorder, Verbose, TEXT("EndFrame Adjusting GGPUFrameTime by TotalSubmitWaitCycles %u"), TotalSubmitWaitCycles);
			GGPUFrameTime -= TotalSubmitWaitCycles;
		}

		StartOfSubmissionTimestamps.Reset();
		EndOfSubmissionTimestamps.Reset();

		GGapRecorderActiveOnBeginFrame = false;
	}
	else
	{
		if (GGapRecorderActiveOnBeginFrame)
		{
			GGapRecorderActiveOnBeginFrame = false;
			Device->GetCommandListManager().SetShouldTrackCmdListTime(false);
		}
	}
}
#endif

void FD3D12Adapter::EndFrame()
{
	for (uint32 GPUIndex : FRHIGPUMask::All())
	{
		uint64 FrameLag = 20;
		GetUploadHeapAllocator(GPUIndex).CleanUpAllocations(FrameLag);
	}
	GetDeferredDeletionQueue().ReleaseResources(false, false);

	if (TransientMemoryCache)
	{
		TransientMemoryCache->GarbageCollect();
	}

#if D3D12_SUBMISSION_GAP_RECORDER
	SubmitGapRecorderTimestamps();
#endif

#if TRACK_RESOURCE_ALLOCATIONS
	FScopeLock Lock(&TrackedAllocationDataCS); 

	// remove tracked released resources older than n amount of frames
	int32 ReleaseCount = 0;
	uint64 CurrentFrameID = GetFrameFence().GetCurrentFence();
	for (; ReleaseCount < ReleasedAllocationData.Num(); ++ReleaseCount)
	{
		if (ReleasedAllocationData[ReleaseCount].ReleasedFrameID + GTrackedReleasedAllocationFrameRetention > CurrentFrameID)
		{
			break;
		}
	}
	if (ReleaseCount > 0)
	{
		ReleasedAllocationData.RemoveAt(0, ReleaseCount, false);
	}
#endif
}

#if WITH_MGPU
FD3D12TemporalEffect* FD3D12Adapter::GetTemporalEffect(const FName& EffectName)
{
	FD3D12TemporalEffect* Effect = TemporalEffectMap.Find(EffectName);

	if (Effect == nullptr)
	{
		Effect = &TemporalEffectMap.Emplace(EffectName, FD3D12TemporalEffect(this, EffectName));
		Effect->Init();
	}

	check(Effect);
	return Effect;
}
#endif // WITH_MGPU

FD3D12FastConstantAllocator& FD3D12Adapter::GetTransientUniformBufferAllocator()
{
	// Multi-GPU support : is using device 0 always appropriate here?
	return FTransientUniformBufferAllocator::Get([this]() -> FTransientUniformBufferAllocator*
	{
		FTransientUniformBufferAllocator* Alloc = new FTransientUniformBufferAllocator(this, Devices[0], FRHIGPUMask::All());

		// Register so the underlying resource location can be freed during adapter cleanup instead of when thread local allocation is destroyed
		{
			FScopeLock Lock(&TransientUniformBufferAllocatorsCS);
			TransientUniformBufferAllocators.Add(Alloc);
		}

		return Alloc;
	});
}

void FD3D12Adapter::ReleaseTransientUniformBufferAllocator(FTransientUniformBufferAllocator* InAllocator)
{
	FScopeLock Lock(&TransientUniformBufferAllocatorsCS);
	verify(TransientUniformBufferAllocators.Remove(InAllocator) == 1);
}

void FD3D12Adapter::UpdateMemoryInfo()
{
#if PLATFORM_WINDOWS || PLATFORM_HOLOLENS
	const uint64 UpdateFrame = FrameFence != nullptr ? FrameFence->GetCurrentFence() : 0;

	// Avoid spurious query calls if we have already captured this frame.
	if (MemoryInfo.UpdateFrameNumber == UpdateFrame)
	{
		return;
	}

	// Update the frame number that the memory is captured from.
	MemoryInfo.UpdateFrameNumber = UpdateFrame;

	TRefCountPtr<IDXGIAdapter3> Adapter3;
	VERIFYD3D12RESULT(GetAdapter()->QueryInterface(IID_PPV_ARGS(Adapter3.GetInitReference())));

	VERIFYD3D12RESULT(Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &MemoryInfo.LocalMemoryInfo));
	VERIFYD3D12RESULT(Adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &MemoryInfo.NonLocalMemoryInfo));

	// Over budget?
	if (MemoryInfo.LocalMemoryInfo.CurrentUsage > MemoryInfo.LocalMemoryInfo.Budget)
	{
		MemoryInfo.AvailableLocalMemory = 0;
		MemoryInfo.DemotedLocalMemory = MemoryInfo.LocalMemoryInfo.CurrentUsage - MemoryInfo.LocalMemoryInfo.Budget;
	}
	else
	{
		MemoryInfo.AvailableLocalMemory = MemoryInfo.LocalMemoryInfo.Budget - MemoryInfo.LocalMemoryInfo.CurrentUsage;
		MemoryInfo.DemotedLocalMemory = 0;
	}

	// Update global RHI state (for warning output, etc.)
	GDemotedLocalMemorySize = MemoryInfo.DemotedLocalMemory;

	if (!GVirtualMGPU)
	{
		for (uint32 Index = 1; Index < GNumExplicitGPUsForRendering; ++Index)
		{
			DXGI_QUERY_VIDEO_MEMORY_INFO TempVideoMemoryInfo;
			VERIFYD3D12RESULT(Adapter3->QueryVideoMemoryInfo(Index, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &TempVideoMemoryInfo));

			DXGI_QUERY_VIDEO_MEMORY_INFO TempSystemMemoryInfo;
			VERIFYD3D12RESULT(Adapter3->QueryVideoMemoryInfo(Index, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &TempSystemMemoryInfo));
			
			MemoryInfo.LocalMemoryInfo.Budget = FMath::Min(MemoryInfo.LocalMemoryInfo.Budget, TempVideoMemoryInfo.Budget);
			MemoryInfo.LocalMemoryInfo.CurrentUsage = FMath::Min(MemoryInfo.LocalMemoryInfo.CurrentUsage, TempVideoMemoryInfo.CurrentUsage);

			MemoryInfo.NonLocalMemoryInfo.Budget = FMath::Min(MemoryInfo.NonLocalMemoryInfo.Budget, TempSystemMemoryInfo.Budget);
			MemoryInfo.NonLocalMemoryInfo.CurrentUsage = FMath::Min(MemoryInfo.NonLocalMemoryInfo.CurrentUsage, TempSystemMemoryInfo.CurrentUsage);
		}
	}
#endif
}

void FD3D12Adapter::BlockUntilIdle()
{
	for (uint32 GPUIndex : FRHIGPUMask::All())
	{
		GetDevice(GPUIndex)->BlockUntilIdle();
	}
}


void FD3D12Adapter::TrackAllocationData(FD3D12ResourceLocation* InAllocation, uint64 InAllocationSize, bool bCollectCallstack)
{
#if TRACK_RESOURCE_ALLOCATIONS
	FTrackedAllocationData AllocationData;
	AllocationData.ResourceAllocation = InAllocation;
	AllocationData.AllocationSize = InAllocationSize;
	if (bCollectCallstack)
	{
		AllocationData.StackDepth = FPlatformStackWalk::CaptureStackBackTrace(&AllocationData.Stack[0], FTrackedAllocationData::MaxStackDepth);
	}
	else 
	{
		AllocationData.StackDepth = 0;
	}

	FScopeLock Lock(&TrackedAllocationDataCS);
	check(!TrackedAllocationData.Contains(InAllocation));
	TrackedAllocationData.Add(InAllocation, AllocationData);
#endif
}

void FD3D12Adapter::ReleaseTrackedAllocationData(FD3D12ResourceLocation* InAllocation, bool bDefragFree)
{
#if TRACK_RESOURCE_ALLOCATIONS
	FScopeLock Lock(&TrackedAllocationDataCS);

	D3D12_GPU_VIRTUAL_ADDRESS GPUAddress = InAllocation->GetGPUVirtualAddress();
	if (GPUAddress != 0 || IsTrackingAllAllocations())
	{
		FReleasedAllocationData ReleasedData;
		ReleasedData.GPUVirtualAddress = GPUAddress;
		ReleasedData.AllocationSize = InAllocation->GetSize();
		ReleasedData.ResourceName = InAllocation->GetResource()->GetName();
		ReleasedData.ResourceDesc = InAllocation->GetResource()->GetDesc();
		ReleasedData.ReleasedFrameID = GetFrameFence().GetCurrentFence();
		ReleasedData.bDefragFree = bDefragFree;
		ReleasedData.bBackBuffer = InAllocation->GetResource()->IsBackBuffer();
		ReleasedData.bTransient = InAllocation->IsTransient();
		// Only the backbuffer doesn't have a valid gpu virtual address
		check(ReleasedData.GPUVirtualAddress != 0 || ReleasedData.bBackBuffer);
		ReleasedAllocationData.Add(ReleasedData);
	}

	verify(TrackedAllocationData.Remove(InAllocation) == 1);
#endif
}


void FD3D12Adapter::TrackHeapAllocation(FD3D12Heap* InHeap)
{
#if TRACK_RESOURCE_ALLOCATIONS
	FScopeLock Lock(&TrackedAllocationDataCS);
	check(!TrackedHeaps.Contains(InHeap));
	TrackedHeaps.Add(InHeap);
#endif
}

void FD3D12Adapter::ReleaseTrackedHeap(FD3D12Heap* InHeap)
{
#if TRACK_RESOURCE_ALLOCATIONS
	FScopeLock Lock(&TrackedAllocationDataCS);

	D3D12_GPU_VIRTUAL_ADDRESS GPUVirtualAddress = InHeap->GetGPUVirtualAddress();
	if (GPUVirtualAddress != 0 || IsTrackingAllAllocations())
	{
		FReleasedAllocationData ReleasedData;
		ReleasedData.GPUVirtualAddress	= GPUVirtualAddress;
		ReleasedData.AllocationSize		= InHeap->GetHeapDesc().SizeInBytes;
		ReleasedData.ResourceName		= InHeap->GetName();
		ReleasedData.ReleasedFrameID	= GetFrameFence().GetCurrentFence();
		ReleasedData.bHeap				= true;
		ReleasedAllocationData.Add(ReleasedData);
	}

	verify(TrackedHeaps.Remove(InHeap) == 1);
#endif
}

void FD3D12Adapter::FindResourcesNearGPUAddress(D3D12_GPU_VIRTUAL_ADDRESS InGPUVirtualAddress, uint64 InRange, TArray<FAllocatedResourceResult>& OutResources)
{
#if TRACK_RESOURCE_ALLOCATIONS
	FScopeLock Lock(&TrackedAllocationDataCS);

	TArray<FTrackedAllocationData> Allocations;
	TrackedAllocationData.GenerateValueArray(Allocations);
	FInt64Range TrackRange((int64)(InGPUVirtualAddress - InRange), (int64)(InGPUVirtualAddress + InRange));
	for (FTrackedAllocationData& AllocationData : Allocations)
	{
		D3D12_GPU_VIRTUAL_ADDRESS GPUAddress = AllocationData.ResourceAllocation->GetResource()->GetGPUVirtualAddress();
		FInt64Range AllocationRange((int64)GPUAddress, (int64)(GPUAddress + AllocationData.AllocationSize));
		if (TrackRange.Overlaps(AllocationRange))
		{
			bool bContainsAllocation = AllocationRange.Contains(InGPUVirtualAddress);
			int64 Distance = bContainsAllocation ? 0 : ((InGPUVirtualAddress < GPUAddress) ? GPUAddress - InGPUVirtualAddress : InGPUVirtualAddress - AllocationRange.GetUpperBoundValue());
			check(Distance >= 0);

			FAllocatedResourceResult Result;
			Result.Allocation = AllocationData.ResourceAllocation;
			Result.Distance = Distance;
			OutResources.Add(Result);
		}
	}

	// Sort the resources on distance from the requested address
	Algo::Sort(OutResources, [InGPUVirtualAddress](const FAllocatedResourceResult& InLHS, const FAllocatedResourceResult& InRHS)
		{
			return InLHS.Distance < InRHS.Distance;
		});
#endif
}

void FD3D12Adapter::FindHeapsContainingGPUAddress(D3D12_GPU_VIRTUAL_ADDRESS InGPUVirtualAddress, TArray<FD3D12Heap*>& OutHeaps)
{
#if TRACK_RESOURCE_ALLOCATIONS
	FScopeLock Lock(&TrackedAllocationDataCS);

	for (FD3D12Heap* AllocatedHeap : TrackedHeaps)
	{
		D3D12_GPU_VIRTUAL_ADDRESS GPUAddress = AllocatedHeap->GetGPUVirtualAddress();
		FInt64Range HeapRange((int64)GPUAddress, (int64)(GPUAddress + AllocatedHeap->GetHeapDesc().SizeInBytes));
		if (HeapRange.Contains(InGPUVirtualAddress))
		{			
			OutHeaps.Add(AllocatedHeap);
		}
	}
#endif
}

void FD3D12Adapter::FindReleasedAllocationData(D3D12_GPU_VIRTUAL_ADDRESS InGPUVirtualAddress, TArray<FReleasedAllocationData>& OutAllocationData)
{
#if TRACK_RESOURCE_ALLOCATIONS
	FScopeLock Lock(&TrackedAllocationDataCS);

	for (FReleasedAllocationData& AllocationData : ReleasedAllocationData)
	{
		if (InGPUVirtualAddress >= AllocationData.GPUVirtualAddress && InGPUVirtualAddress < (AllocationData.GPUVirtualAddress + AllocationData.AllocationSize))
		{
			// Add in reverse order, so last released resources at first in the array
			OutAllocationData.EmplaceAt(0, AllocationData);
		}
	}
#endif
}

#if TRACK_RESOURCE_ALLOCATIONS

static FAutoConsoleCommandWithOutputDevice GDumpTrackedD3D12AllocationsCmd(
	TEXT("D3D12.DumpTrackedAllocations"),
	TEXT("Dump all tracked d3d12 resource allocations."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& OutputDevice)
	{
		FD3D12DynamicRHI::GetD3DRHI()->GetAdapter().DumpTrackedAllocationData(OutputDevice, false, false);
	})
);

static FAutoConsoleCommandWithOutputDevice GDumpTrackedD3D12AllocationCallstacksCmd(
	TEXT("D3D12.DumpTrackedAllocationCallstacks"),
	TEXT("Dump all tracked d3d12 resource allocation callstacks."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& OutputDevice)
	{
		FD3D12DynamicRHI::GetD3DRHI()->GetAdapter().DumpTrackedAllocationData(OutputDevice, false, true);
	})
);

static FAutoConsoleCommandWithOutputDevice GDumpTrackedD3D12ResidentAllocationsCmd(
	TEXT("D3D12.DumpTrackedResidentAllocations"),
	TEXT("Dump all tracked resisdent d3d12 resource allocations."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& OutputDevice)
		{
			FD3D12DynamicRHI::GetD3DRHI()->GetAdapter().DumpTrackedAllocationData(OutputDevice, true, false);
		})
);

static FAutoConsoleCommandWithOutputDevice GDumpTrackedD3D12ResidentAllocationCallstacksCmd(
	TEXT("D3D12.DumpTrackedResidentAllocationCallstacks"),
	TEXT("Dump all tracked resident d3d12 resource allocation callstacks."),
	FConsoleCommandWithOutputDeviceDelegate::CreateStatic([](FOutputDevice& OutputDevice)
		{
			FD3D12DynamicRHI::GetD3DRHI()->GetAdapter().DumpTrackedAllocationData(OutputDevice, true, true);
		})
);

void FD3D12Adapter::DumpTrackedAllocationData(FOutputDevice& OutputDevice, bool bResidentOnly, bool bWithCallstack)
{
	FScopeLock Lock(&TrackedAllocationDataCS);

	TArray<FTrackedAllocationData> Allocations;
	TrackedAllocationData.GenerateValueArray(Allocations);
	Allocations.Sort([](const FTrackedAllocationData& InLHS, const FTrackedAllocationData& InRHS)
		{
			return InLHS.AllocationSize > InRHS.AllocationSize;
		});

	TArray<FTrackedAllocationData> BufferAllocations;	
	TArray<FTrackedAllocationData> TextureAllocations;
	uint64 TotalAllocatedBufferSize = 0;
	uint64 TotalResidentBufferSize = 0;
	uint64 TotalAllocatedTextureSize = 0;
	uint64 TotalResidentTextureSize = 0;
	for (FTrackedAllocationData& AllocationData : Allocations)
	{
		D3D12_RESOURCE_DESC ResourceDesc = AllocationData.ResourceAllocation->GetResource()->GetDesc();
		if (ResourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		{
			BufferAllocations.Add(AllocationData);
			TotalAllocatedBufferSize += AllocationData.AllocationSize;			
#if ENABLE_RESIDENCY_MANAGEMENT
			TotalResidentBufferSize += (AllocationData.ResourceAllocation->GetResidencyHandle()->ResidencyStatus == D3DX12Residency::ManagedObject::RESIDENCY_STATUS::RESIDENT) ? AllocationData.AllocationSize : 0;
#else
			TotalResidentBufferSize += AllocationData.AllocationSize;
#endif 
		}
		else
		{
			TextureAllocations.Add(AllocationData);
			TotalAllocatedTextureSize += AllocationData.AllocationSize;
#if ENABLE_RESIDENCY_MANAGEMENT
			TotalResidentTextureSize += (AllocationData.ResourceAllocation->GetResidencyHandle()->ResidencyStatus == D3DX12Residency::ManagedObject::RESIDENCY_STATUS::RESIDENT) ? AllocationData.AllocationSize : 0;
#else
			TotalResidentTextureSize += AllocationData.AllocationSize;
#endif 
		}
	}

	const size_t STRING_SIZE = 16 * 1024;
	ANSICHAR StackTrace[STRING_SIZE];

	FString OutputData;
	OutputData += FString::Printf(TEXT("\n%d Tracked Texture Allocations (Total size: %4.3fMB - Resident: %4.3fMB):\n"), TextureAllocations.Num(), TotalAllocatedTextureSize / (1024.0f * 1024), TotalResidentTextureSize / (1024.0f * 1024));
	for (const FTrackedAllocationData& AllocationData : TextureAllocations)
	{
		D3D12_RESOURCE_DESC ResourceDesc = AllocationData.ResourceAllocation->GetResource()->GetDesc();

		bool bResident = true;
#if ENABLE_RESIDENCY_MANAGEMENT
		bResident = AllocationData.ResourceAllocation->GetResidencyHandle()->ResidencyStatus == D3DX12Residency::ManagedObject::RESIDENCY_STATUS::RESIDENT;
#endif 
		if (!bResident && bResidentOnly)
		{
			continue;
		}

		FString Flags;
		if (EnumHasAnyFlags(ResourceDesc.Flags, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET))
		{
			Flags += "RT";
		}
		else if (EnumHasAnyFlags(ResourceDesc.Flags, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
		{
			Flags += "DS";
		}
		if (EnumHasAnyFlags(ResourceDesc.Flags, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
		{
			Flags += EnumHasAnyFlags(ResourceDesc.Flags, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) ? "|UAV" : "UAV";
		}

		OutputData += FString::Printf(TEXT("\tName: %s - Size: %3.3fMB - Width: %d - Height: %d - DepthOrArraySize: %d - MipLevels: %d - Flags: %s - Resident: %s\n"), 
			*AllocationData.ResourceAllocation->GetResource()->GetName().ToString(), 
			AllocationData.AllocationSize / (1024.0f * 1024),
			ResourceDesc.Width, ResourceDesc.Height, ResourceDesc.DepthOrArraySize, ResourceDesc.MipLevels,
			Flags.IsEmpty() ? TEXT("None") : *Flags,
			bResident ? TEXT("Yes") : TEXT("No"));

		if (bWithCallstack)
		{
			static uint32 EntriesToSkip = 3;
			for (uint32 Index = EntriesToSkip; Index < AllocationData.StackDepth; ++Index)
			{
				StackTrace[0] = 0;
				FPlatformStackWalk::ProgramCounterToHumanReadableString(Index, AllocationData.Stack[Index], StackTrace, STRING_SIZE, nullptr);
				OutputData += FString::Printf(TEXT("\t\t%d %s\n"), Index - EntriesToSkip, ANSI_TO_TCHAR(StackTrace));
			}
		}
	}

	OutputData += FString::Printf(TEXT("\n\n%d Tracked Buffer Allocations (Total size: %4.3fMB - Resident: %4.3fMB):\n"), BufferAllocations.Num(), TotalAllocatedBufferSize / (1024.0f * 1024), TotalResidentBufferSize / (1024.0f * 1024));
	for (const FTrackedAllocationData& AllocationData : BufferAllocations)
	{
		D3D12_RESOURCE_DESC ResourceDesc = AllocationData.ResourceAllocation->GetResource()->GetDesc();

		bool bResident = true;
#if ENABLE_RESIDENCY_MANAGEMENT
		bResident = AllocationData.ResourceAllocation->GetResidencyHandle()->ResidencyStatus == D3DX12Residency::ManagedObject::RESIDENCY_STATUS::RESIDENT;
#endif 
		if (!bResident && bResidentOnly)
		{
			continue;
		}

		OutputData += FString::Printf(TEXT("\tName: %s - Size: %3.3fMB - Width: %d - UAV: %s - Resident: %s\n"), 
			*AllocationData.ResourceAllocation->GetResource()->GetName().ToString(), 
			AllocationData.AllocationSize / (1024.0f * 1024),
			ResourceDesc.Width,
			EnumHasAnyFlags(ResourceDesc.Flags, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? TEXT("Yes") : TEXT("No"),
			bResident ? TEXT("Yes") : TEXT("No"));

		if (bWithCallstack)
		{
			static uint32 EntriesToSkip = 3;
			for (uint32 Index = EntriesToSkip; Index < AllocationData.StackDepth; ++Index)
			{
				StackTrace[0] = 0;
				FPlatformStackWalk::ProgramCounterToHumanReadableString(Index, AllocationData.Stack[Index], StackTrace, STRING_SIZE, nullptr);
				OutputData += FString::Printf(TEXT("\t\t%d %s\n"), Index - EntriesToSkip, ANSI_TO_TCHAR(StackTrace));
			}
		}
	}

	OutputDevice.Log(OutputData);
}

#endif // TRACK_RESOURCE_ALLOCATIONS
