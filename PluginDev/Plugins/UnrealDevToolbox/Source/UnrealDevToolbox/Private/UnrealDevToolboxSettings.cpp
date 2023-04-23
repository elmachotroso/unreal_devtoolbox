// Copyright 2023 Andrei Victor. All rights reserved.


#include "UnrealDevToolboxSettings.h"

UUnrealDevToolboxSettings::UUnrealDevToolboxSettings()
	: Super()
	, bShowLogsOnScreen(false)
	, bAllowDialogBoxesInEditor(true)
	, bShowErrorsAsDialogBoxes(true)
{
	CategoryName = TEXT("Plugins");
}


