// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#if WITH_EOS_SDK

#include "Features/IModularFeatures.h"

#if defined(EOS_PLATFORM_BASE_FILE_NAME)
#include EOS_PLATFORM_BASE_FILE_NAME
#endif
#include "eos_init.h"
#include "eos_types.h"

DECLARE_MULTICAST_DELEGATE_OneParam(FEOSSDKManagerOnPreInitializeSDK, EOS_InitializeOptions& Options);
DECLARE_MULTICAST_DELEGATE_OneParam(FEOSSDKManagerOnPreCreatePlatform, EOS_Platform_Options& Options);

class IEOSPlatformHandle
{
public:
	IEOSPlatformHandle(EOS_HPlatform InPlatformHandle) : PlatformHandle(InPlatformHandle) {}
	virtual ~IEOSPlatformHandle() = default;

	virtual void Tick() = 0;

	operator EOS_HPlatform() const { return PlatformHandle; }

protected:
	EOS_HPlatform PlatformHandle;
};
using IEOSPlatformHandlePtr = TSharedPtr<IEOSPlatformHandle, ESPMode::ThreadSafe>;

class IEOSSDKManager : public IModularFeature
{
public:
	static IEOSSDKManager* Get()
	{
		if (IModularFeatures::Get().IsModularFeatureAvailable(GetModularFeatureName()))
		{
			return &IModularFeatures::Get().GetModularFeature<IEOSSDKManager>(GetModularFeatureName());
		}
		return nullptr;
	}

	static FName GetModularFeatureName()
	{
		static const FName FeatureName = TEXT("EOSSDKManager");
		return FeatureName;
	}

	virtual ~IEOSSDKManager() = default;

	virtual EOS_EResult Initialize() = 0;
	virtual bool IsInitialized() const = 0;

	virtual IEOSPlatformHandlePtr CreatePlatform(EOS_Platform_Options& PlatformOptions) = 0;

	virtual FString GetProductName() const = 0;
	virtual FString GetProductVersion() const = 0;
	virtual FString GetCacheDirBase() const = 0;

	FEOSSDKManagerOnPreInitializeSDK OnPreInitializeSDK;
	FEOSSDKManagerOnPreCreatePlatform OnPreCreatePlatform;
};

#endif // WITH_EOS_SDK