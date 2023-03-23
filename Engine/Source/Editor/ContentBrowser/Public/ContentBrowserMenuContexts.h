// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/Object.h"
#include "ContentBrowserDelegates.h"
#include "AssetTypeCategories.h"

#include "ContentBrowserMenuContexts.generated.h"

class FAssetContextMenu;
class IAssetTypeActions;
class SAssetView;
class SContentBrowser;
class SFilterList;

UCLASS()
class CONTENTBROWSER_API UContentBrowserAssetContextMenuContext : public UObject
{
	GENERATED_BODY()

public:

	TWeakPtr<FAssetContextMenu> AssetContextMenu;

	TWeakPtr<IAssetTypeActions> CommonAssetTypeActions;
	
	UPROPERTY()
	TArray<TWeakObjectPtr<UObject>> SelectedObjects;

	UPROPERTY()
	TObjectPtr<UClass> CommonClass;

	UPROPERTY()
	bool bCanBeModified;

	UFUNCTION(BlueprintCallable, Category="Tool Menus")
	TArray<UObject*> GetSelectedObjects() const
	{
		TArray<UObject*> Result;
		Result.Reserve(SelectedObjects.Num());
		for (const TWeakObjectPtr<UObject>& Object : SelectedObjects)
		{
			Result.Add(Object.Get());
		}
		return Result;
	}
};

UCLASS()
class CONTENTBROWSER_API UContentBrowserAssetViewContextMenuContext : public UObject
{
	GENERATED_BODY()

public:
	TWeakPtr<SContentBrowser> OwningContentBrowser;
	TWeakPtr<SAssetView> AssetView;
};

UCLASS()
class CONTENTBROWSER_API UContentBrowserMenuContext : public UObject
{
	GENERATED_BODY()

public:

	TWeakPtr<SContentBrowser> ContentBrowser;
};

UCLASS()
class CONTENTBROWSER_API UContentBrowserFolderContext : public UContentBrowserMenuContext
{
	GENERATED_BODY()

public:

	UPROPERTY()
	bool bCanBeModified;

	UPROPERTY()
	bool bNoFolderOnDisk;

	UPROPERTY()
	int32 NumAssetPaths;

	UPROPERTY()
	int32 NumClassPaths;

	FOnCreateNewFolder OnCreateNewFolder;
};

UCLASS()
class CONTENTBROWSER_API UContentBrowserFilterListContext : public UObject
{
	GENERATED_BODY()

public:

	TWeakPtr<SFilterList> FilterList;

	EAssetTypeCategories::Type MenuExpansion;
};

UCLASS()
class CONTENTBROWSER_API UContentBrowserAddNewContextMenuContext : public UObject
{
	GENERATED_BODY()

public:

	TWeakPtr<SContentBrowser> ContentBrowser;
};

UCLASS()
class CONTENTBROWSER_API UContentBrowserToolbarMenuContext : public UObject
{
	GENERATED_BODY()

public:
	FName GetCurrentPath() const;
public:

	TWeakPtr<SContentBrowser> ContentBrowser;
};
