// Copyright Epic Games, Inc. All Rights Reserved.

#include "UnrealDevToolboxStyle.h"
#include "UnrealDevToolbox.h"
#include "Framework/Application/SlateApplication.h"
#include "Styling/SlateStyleRegistry.h"
#include "Slate/SlateGameResources.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"

#define RootToContentDir Style->RootToContentDir

TSharedPtr<FSlateStyleSet> FUnrealDevToolboxStyle::StyleInstance = nullptr;

void FUnrealDevToolboxStyle::Initialize()
{
	if (!StyleInstance.IsValid())
	{
		StyleInstance = Create();
		FSlateStyleRegistry::RegisterSlateStyle(*StyleInstance);
	}
}

void FUnrealDevToolboxStyle::Shutdown()
{
	FSlateStyleRegistry::UnRegisterSlateStyle(*StyleInstance);
	ensure(StyleInstance.IsUnique());
	StyleInstance.Reset();
}

FName FUnrealDevToolboxStyle::GetStyleSetName()
{
	static FName StyleSetName(TEXT("UnrealDevToolboxStyle"));
	return StyleSetName;
}


const FVector2D Icon16x16(16.0f, 16.0f);
const FVector2D Icon20x20(20.0f, 20.0f);

TSharedRef< FSlateStyleSet > FUnrealDevToolboxStyle::Create()
{
	TSharedRef< FSlateStyleSet > Style = MakeShareable(new FSlateStyleSet("UnrealDevToolboxStyle"));
	Style->SetContentRoot(IPluginManager::Get().FindPlugin("UnrealDevToolbox")->GetBaseDir() / TEXT("Resources"));

	Style->Set("UnrealDevToolbox.PluginAction", new IMAGE_BRUSH_SVG(TEXT("PlaceholderButtonIcon"), Icon20x20));
	return Style;
}

void FUnrealDevToolboxStyle::ReloadTextures()
{
	if (FSlateApplication::IsInitialized())
	{
		FSlateApplication::Get().GetRenderer()->ReloadTextureResources();
	}
}

const ISlateStyle& FUnrealDevToolboxStyle::Get()
{
	return *StyleInstance;
}
