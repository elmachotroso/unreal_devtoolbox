// Copyright 2023 Andrei Victor. All rights reserved.

#pragma once

#include "CoreMinimal.h"

DECLARE_LOG_CATEGORY_EXTERN(LogUnrealDevToolbox, Log, All);

#if WITH_EDITOR

#define DEFAULT_SHOW_ON_SCREEN false
#define DEFAULT_SHOW_DIALOG_BOX true
#define DEFAULT_LOG_DURATION 2.f

#else

#define DEFAULT_SHOW_ON_SCREEN false
#define DEFAULT_SHOW_DIALOG_BOX false
#define DEFAULT_LOG_DURATION 2.f

#endif //WITH_EDITOR

/**
 *	Logging functions internally used by the plugin to log and report diagnostic information and issues.
 */
namespace UDT
{
	/** Logs normally */
	void Log(const FString & Message, float Duration = DEFAULT_LOG_DURATION, bool bShowOnScreen = DEFAULT_SHOW_ON_SCREEN, bool bShowDialogBox = DEFAULT_SHOW_DIALOG_BOX);

	/** Logs warning messages (yellow) */
	void LogWarn(const FString & Message, float Duration = DEFAULT_LOG_DURATION, bool bShowOnScreen = DEFAULT_SHOW_ON_SCREEN, bool bShowDialogBox = DEFAULT_SHOW_DIALOG_BOX);

	/** Logs error messages (red) */
	void LogError(const FString & Message, float Duration = DEFAULT_LOG_DURATION, bool bShowOnScreen = DEFAULT_SHOW_ON_SCREEN, bool bShowDialogBox = DEFAULT_SHOW_DIALOG_BOX);
}
