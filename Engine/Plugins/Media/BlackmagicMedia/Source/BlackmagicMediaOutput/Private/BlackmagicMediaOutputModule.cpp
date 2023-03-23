// Copyright Epic Games, Inc. All Rights Reserved.

#include "BlackmagicMediaOutputModule.h"

#include "BlackmagicLib.h"
#include "GenericPlatform/GenericPlatformDriver.h"
#include "RenderingThread.h"
#include "RHI.h"

DEFINE_LOG_CATEGORY(LogBlackmagicMediaOutput);

FBlackmagicMediaOutputModule& FBlackmagicMediaOutputModule::Get()
{
	return FModuleManager::LoadModuleChecked<FBlackmagicMediaOutputModule>(TEXT("BlackmagicMediaOutput"));
}

void FBlackmagicMediaOutputModule::StartupModule()
{
	const auto DMAInitializationFunc = [this]()
	{
		const FGPUDriverInfo GPUDriverInfo = FPlatformMisc::GetGPUDriverInfo(GRHIAdapterName);
		bIsGPUTextureTransferAvailable = GPUDriverInfo.IsNVIDIA() && !FModuleManager::Get().IsModuleLoaded("RenderDocPlugin");
		bIsGPUTextureTransferAvailable &= !GPUDriverInfo.DeviceDescription.Contains(TEXT("Tesla"));

		if (bIsGPUTextureTransferAvailable)
		{
			ENQUEUE_RENDER_COMMAND(BlackmagicMediaCaptureInitialize)(
				[this](FRHICommandListImmediate& RHICmdList) mutable
				{
					if (!GDynamicRHI)
					{
						return;
					}

					auto GetRHI = []()
					{
						FString RHIName = GDynamicRHI->GetName();
						if (RHIName == TEXT("D3D11"))
						{
							return BlackmagicDesign::ERHI::D3D11;
						}
						else if (RHIName == TEXT("D3D12"))
						{
							return BlackmagicDesign::ERHI::D3D12;
						}
						else if (RHIName == TEXT("Vulkan"))
						{
							return BlackmagicDesign::ERHI::Vulkan;
						}

						return  BlackmagicDesign::ERHI::Invalid;
					};


					BlackmagicDesign::FInitializeDMAArgs Args;
					BlackmagicDesign::ERHI RHI = GetRHI();
					Args.RHI = RHI;
					Args.RHIDevice = GDynamicRHI->RHIGetNativeDevice();
					Args.RHICommandQueue = GDynamicRHI->RHIGetNativeGraphicsQueue();

					bIsGPUTextureTransferAvailable = BlackmagicDesign::InitializeDMA(Args);
				});
		}
	};


	auto DMAUninitialize = [this]()
	{
		if (bIsGPUTextureTransferAvailable)
		{
			ENQUEUE_RENDER_COMMAND(BlackmagicMediaCaptureUninitialize)(
				[](FRHICommandListImmediate& RHICmdList) mutable
				{
					BlackmagicDesign::UninitializeDMA();
				});
		}
	};

	//Postpone initialization after all modules have been loaded to be sure Blackmagic library has been loaded
	FCoreDelegates::OnAllModuleLoadingPhasesComplete.AddLambda(DMAInitializationFunc);
	//Same for shutdown, uninitialize ourselves before library is unloaded
	FCoreDelegates::OnEnginePreExit.AddLambda(DMAUninitialize);
}

void FBlackmagicMediaOutputModule::ShutdownModule()
{
	
}

bool FBlackmagicMediaOutputModule::IsGPUTextureTransferAvailable() const
{
	return bIsGPUTextureTransferAvailable;
}

IMPLEMENT_MODULE(FBlackmagicMediaOutputModule, BlackmagicMediaOutput)
