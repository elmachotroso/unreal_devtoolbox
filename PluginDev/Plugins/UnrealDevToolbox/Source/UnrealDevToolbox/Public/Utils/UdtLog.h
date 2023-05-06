// Copyright 2023 Andrei Victor. All rights reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/Engine.h" // for GEngine
#include "UnrealDevToolboxSettings.h"

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

#if WITH_EDITOR

#include "Misc/MessageDialog.h"
#define __UDT_MESSAGE_DIALOG(FString_Message) FMessageDialog::Open(EAppMsgType::Ok, FText::FromString((FString_Message)), nullptr)

#else

#define __UDT_MESSAGE_DIALOG(...)

#endif //WITH_EDITOR

#define __UDT_LOG(LogCategory, FString_Message, Verbosity_Type, FColor_Color, float_Duration, bool_bShowOnScreen, bool_bShowDialogBox) \
{ \
	UE_LOG(LogCategory, Verbosity_Type, TEXT("%s"), *(FString_Message)); \
	auto UdtSettings = GetDefault<UUnrealDevToolboxSettings>(); \
	bool bShouldShowOnScreen = UdtSettings && UdtSettings->bShowLogsOnScreen && (bool_bShowOnScreen); \
	bool bShouldShowDialogBox = UdtSettings && UdtSettings->bAllowDialogBoxesInEditor && (bShowDialogBox); \
	if (UdtSettings && UdtSettings->bShowErrorsAsDialogBoxes && ELogVerbosity::Verbosity_Type == ELogVerbosity::Error) \
	{ \
		bShouldShowDialogBox = true; \
	} \
	if (bShouldShowOnScreen) \
	{ \
		GEngine->AddOnScreenDebugMessage(INDEX_NONE, (float_Duration), (FColor_Color), (FString_Message)); \
	} \
	if (bShouldShowDialogBox) \
	{ \
		__UDT_MESSAGE_DIALOG((FString_Message)); \
	} \
}

#define DECLARE_LOG_CATEGORY_FUNCTIONS(CategoryName) \
	void Log(const FString & Message, float Duration = DEFAULT_LOG_DURATION, bool bShowOnScreen = DEFAULT_SHOW_ON_SCREEN, bool bShowDialogBox = DEFAULT_SHOW_DIALOG_BOX); \
	void LogWarn(const FString & Message, float Duration = DEFAULT_LOG_DURATION, bool bShowOnScreen = DEFAULT_SHOW_ON_SCREEN, bool bShowDialogBox = DEFAULT_SHOW_DIALOG_BOX); \
	void LogError(const FString & Message, float Duration = DEFAULT_LOG_DURATION, bool bShowOnScreen = DEFAULT_SHOW_ON_SCREEN, bool bShowDialogBox = DEFAULT_SHOW_DIALOG_BOX);

#define DEFINE_LOG_CATEGORY_FUNCTIONS(CategoryName) \
	void Log(const FString & Message, float Duration, bool bShowOnScreen, bool bShowDialogBox) \
	{ \
		__UDT_LOG(CategoryName, Message, Log, FColor::White, Duration, bShowOnScreen, bShowDialogBox); \
	} \
	void LogWarn(const FString & Message, float Duration, bool bShowOnScreen, bool bShowDialogBox) \
	{ \
		__UDT_LOG(CategoryName, Message, Warning, FColor::Yellow, Duration, bShowOnScreen, bShowDialogBox); \
	} \
	void LogError(const FString & Message, float Duration, bool bShowOnScreen, bool bShowDialogBox) \
	{ \
		__UDT_LOG(CategoryName, Message, Error, FColor::Red, Duration, bShowOnScreen, bShowDialogBox); \
	}

/**
 *	Logging functions internally used by the plugin to log and report diagnostic information and issues.
 */
namespace UDT
{
	DECLARE_LOG_CATEGORY_FUNCTIONS(LogUnrealDevToolbox);
}
