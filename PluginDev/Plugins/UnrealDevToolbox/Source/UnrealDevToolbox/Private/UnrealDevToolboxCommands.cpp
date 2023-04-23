// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealDevToolboxCommands.h"

#define LOCTEXT_NAMESPACE "FUnrealDevToolboxModule"

void FUnrealDevToolboxCommands::RegisterCommands()
{
	UI_COMMAND(PluginAction, "UnrealDevToolbox", "Execute UnrealDevToolbox action", EUserInterfaceActionType::Button, FInputChord());
}

#undef LOCTEXT_NAMESPACE
