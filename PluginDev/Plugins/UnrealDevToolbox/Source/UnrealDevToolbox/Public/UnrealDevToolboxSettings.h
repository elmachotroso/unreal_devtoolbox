// Copyright 2023 Andrei Victor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UnrealDevToolboxSettings.generated.h"

/**
 * 
 */
UCLASS(Config = UnrealDevToolbox, defaultconfig, meta = (DisplayName = "Unreal DevToolbox Settings"))
class UNREALDEVTOOLBOX_API UUnrealDevToolboxSettings : public UDeveloperSettings
{
	GENERATED_BODY()
	
public:
	/** Set to true if you want the logs to be printed on screen. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Logging")
	bool bShowLogsOnScreen;
	
	/** Set to true if you want to allow dialog box options on the log calls of the plugin. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Logging")
	bool bAllowDialogBoxesInEditor;
	
	/** Set to true if you want all errors be presented as a dialog box. This requires "Allow Dialog Boxes in Editor" to be set to true. */
	UPROPERTY(Config, EditAnywhere, BlueprintReadOnly, Category = "Logging")
	bool bShowErrorsAsDialogBoxes;
	
	UUnrealDevToolboxSettings();
};
