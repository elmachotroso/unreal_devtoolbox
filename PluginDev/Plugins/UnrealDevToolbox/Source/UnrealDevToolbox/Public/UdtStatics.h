// Copyright 2023 Andrei Victor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "UdtStatics.generated.h"

/**
 *  Collection of static UFUNCTIONS for Blueprint access.
 */
UCLASS()
class UNREALDEVTOOLBOX_API UUdtStatics : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
	
public:
	/** Returns true when running with Editor. */
	UFUNCTION(BlueprintPure, Category = "UnrealDevToolbox|FunctionLibrary")
	static bool IsWithEditor();
	
	/** Returns true when running with PIE. */
	UFUNCTION(BlueprintPure, Category = "UnrealDevToolbox|FunctionLibrary")
	static bool IsRunningPIE();
	
	/** Returns true when running with a shipping build. */
	UFUNCTION(BlueprintPure, Category = "UnrealDevToolbox|FunctionLibrary")
	static bool IsShippingBuild();
	
	/** Returns true when HMD is present or in use. This can be used to detect whether you are in VRPreview */
	UFUNCTION(BlueprintPure, Category = "UnrealDevToolbox|FunctionLibrary")
	static bool IsUsingHMD();
};
