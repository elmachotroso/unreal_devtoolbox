// Copyright 2023 Andrei Victor. All rights reserved.


#include "Utils/UdtLog.h"
#include "Engine/Engine.h" // for GEngine
#include "UnrealDevToolboxSettings.h"

DEFINE_LOG_CATEGORY(LogUnrealDevToolbox);

#if WITH_EDITOR

#include "Misc/MessageDialog.h"
#define __UDT_MESSAGE_DIALOG(FString_Message) FMessageDialog::Open(EAppMsgType::Ok, FText::FromString((FString_Message)), nullptr)

#else

#define __UDT_MESSAGE_DIALOG(...)

#endif //WITH_EDITOR

#define __UDT_LOG(FString_Message, Verbosity_Type, FColor_Color, float_Duration, bool_bShowOnScreen, bool_bShowDialogBox) \
	{ \
		UE_LOG(LogUnrealDevToolbox, Verbosity_Type, TEXT("%s"), *(FString_Message)); \
		auto UdtSettings = GetSettings(); \
		if ((bool_bShowOnScreen) && GEngine && UdtSettings && UdtSettings->bShowLogsOnScreen) \
		{ \
			GEngine->AddOnScreenDebugMessage(INDEX_NONE, (float_Duration), (FColor_Color), (FString_Message)); \
		} \
		if ((bool_bShowDialogBox) && UdtSettings && UdtSettings->bAllowDialogBoxesInEditor) \
		{ \
			__UDT_MESSAGE_DIALOG((FString_Message)); \
		} \
	}

namespace UDT
{
	TObjectPtr<const UUnrealDevToolboxSettings> GetSettings()
	{
		return GetDefault<UUnrealDevToolboxSettings>();
	}

	void Log(const FString & Message, float Duration /*= DEFAULT_LOG_DURATION*/, bool bShowOnScreen /*= DEFAULT_SHOW_ON_SCREEN*/, bool bShowDialogBox /*= DEFAULT_SHOW_DIALOG_BOX*/)
	{
		__UDT_LOG(Message, Log, FColor::White, Duration, bShowOnScreen, bShowDialogBox);
	}

	void LogWarn(const FString & Message, float Duration /*= DEFAULT_LOG_DURATION*/, bool bShowOnScreen /*= DEFAULT_SHOW_ON_SCREEN*/, bool bShowDialogBox /*= DEFAULT_SHOW_DIALOG_BOX*/)
	{
		__UDT_LOG(Message, Warning, FColor::Yellow, Duration, bShowOnScreen, bShowDialogBox);
	}

	void LogError(const FString & Message, float Duration /*= DEFAULT_LOG_DURATION*/, bool bShowOnScreen /*= DEFAULT_SHOW_ON_SCREEN*/, bool bShowDialogBox /*= DEFAULT_SHOW_DIALOG_BOX*/)
	{
		auto Settings = GetSettings();
		if (Settings && Settings->bShowErrorsAsDialogBoxes)
		{
			bShowDialogBox = true;
		}
		
		__UDT_LOG(Message, Error, FColor::Red, Duration, bShowOnScreen, bShowDialogBox);
	}
}
