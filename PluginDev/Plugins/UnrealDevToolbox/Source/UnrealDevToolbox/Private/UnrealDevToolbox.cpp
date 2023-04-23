// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealDevToolbox.h"
#include "UnrealDevToolboxStyle.h"
#include "UnrealDevToolboxCommands.h"
#include "Misc/MessageDialog.h"
#include "ToolMenus.h"
#include "Utils/UdtLog.h"

#define LOCTEXT_NAMESPACE "FUnrealDevToolboxModule"

static const FName UnrealDevToolboxTabName("UnrealDevToolbox");

void FUnrealDevToolboxModule::StartupModule()
{
	// This code will execute after your module is loaded into memory; the exact timing is specified in the .uplugin file per-module
	UDT::Log(TEXT("Initializing UnrealDevToolbox..."), DEFAULT_LOG_DURATION, false, false);
	FUnrealDevToolboxStyle::Initialize();
	FUnrealDevToolboxStyle::ReloadTextures();

	FUnrealDevToolboxCommands::Register();
	
	PluginCommands = MakeShareable(new FUICommandList);

	PluginCommands->MapAction(
		FUnrealDevToolboxCommands::Get().PluginAction,
		FExecuteAction::CreateRaw(this, &FUnrealDevToolboxModule::PluginButtonClicked),
		FCanExecuteAction());

	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FUnrealDevToolboxModule::RegisterMenus));
	UDT::Log(TEXT("UnrealDevToolbox Initialized!"), DEFAULT_LOG_DURATION, false, false);
}

void FUnrealDevToolboxModule::ShutdownModule()
{
	// This function may be called during shutdown to clean up your module.  For modules that support dynamic reloading,
	// we call this function before unloading the module.
	UDT::Log(TEXT("Shutting down UnrealDevToolbox..."), DEFAULT_LOG_DURATION, false, false);
	
	UToolMenus::UnRegisterStartupCallback(this);

	UToolMenus::UnregisterOwner(this);

	FUnrealDevToolboxStyle::Shutdown();

	FUnrealDevToolboxCommands::Unregister();
	
	UDT::Log(TEXT("UnrealDevToolbox Shutdown!"), DEFAULT_LOG_DURATION, false, false);
}

void FUnrealDevToolboxModule::PluginButtonClicked()
{
	// Put your "OnButtonClicked" stuff here
	FText DialogText = FText::Format(
							LOCTEXT("PluginButtonDialogText", "Add code to {0} in {1} to override this button's actions"),
							FText::FromString(TEXT("FUnrealDevToolboxModule::PluginButtonClicked()")),
							FText::FromString(TEXT("UnrealDevToolbox.cpp"))
					   );
	FMessageDialog::Open(EAppMsgType::Ok, DialogText);
}

void FUnrealDevToolboxModule::RegisterMenus()
{
	// Owner will be used for cleanup in call to UToolMenus::UnregisterOwner
	FToolMenuOwnerScoped OwnerScoped(this);

	{
		UToolMenu* Menu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Window");
		{
			FToolMenuSection& Section = Menu->FindOrAddSection("WindowLayout");
			Section.AddMenuEntryWithCommandList(FUnrealDevToolboxCommands::Get().PluginAction, PluginCommands);
		}
	}

	{
		UToolMenu* ToolbarMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.PlayToolBar");
		{
			FToolMenuSection& Section = ToolbarMenu->FindOrAddSection("PluginTools");
			{
				FToolMenuEntry& Entry = Section.AddEntry(FToolMenuEntry::InitToolBarButton(FUnrealDevToolboxCommands::Get().PluginAction));
				Entry.SetCommandList(PluginCommands);
			}
		}
	}
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FUnrealDevToolboxModule, UnrealDevToolbox)
