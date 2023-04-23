// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Framework/Commands/Commands.h"
#include "UnrealDevToolboxStyle.h"

class FUnrealDevToolboxCommands : public TCommands<FUnrealDevToolboxCommands>
{
public:

	FUnrealDevToolboxCommands()
		: TCommands<FUnrealDevToolboxCommands>(TEXT("UnrealDevToolbox"), NSLOCTEXT("Contexts", "UnrealDevToolbox", "UnrealDevToolbox Plugin"), NAME_None, FUnrealDevToolboxStyle::GetStyleSetName())
	{
	}

	// TCommands<> interface
	virtual void RegisterCommands() override;

public:
	TSharedPtr< FUICommandInfo > PluginAction;
};
