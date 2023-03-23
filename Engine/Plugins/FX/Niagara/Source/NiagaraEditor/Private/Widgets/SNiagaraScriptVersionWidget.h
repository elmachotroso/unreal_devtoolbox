﻿// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "NiagaraActions.h"
#include "EdGraph/EdGraphSchema.h"
#include "Misc/NotifyHook.h"
#include "Widgets/DeclarativeSyntaxSupport.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SWidget.h"

struct FCustomExpanderData;
struct FNiagaraAssetVersion;
class SGraphActionMenu;
class SExpanderArrow;
class STableViewBase;
class ITableRow;
class IDetailsView;
class UNiagaraVersionMetaData;
class UNiagaraScript;

DECLARE_DELEGATE_OneParam(FOnSwitchToVersionDelegate, FGuid);

struct FNiagaraVersionMenuAction : FNiagaraMenuAction
{
	FNiagaraVersionMenuAction() {}
	FNiagaraVersionMenuAction(FText InNodeCategory, FText InMenuDesc, FText InToolTip, const int32 InGrouping, FText InKeywords, FOnExecuteStackAction InAction, int32 InSectionID, FNiagaraAssetVersion InVersion);

	FNiagaraAssetVersion AssetVersion;
};

class NIAGARAEDITOR_API SNiagaraScriptVersionWidget : public SCompoundWidget, FNotifyHook
{
public:
	SLATE_BEGIN_ARGS(SNiagaraScriptVersionWidget)
	{}

	/** Called when the version data of the script was edited by the user */
	SLATE_EVENT(FSimpleDelegate, OnVersionDataChanged)

	/** Called when the user does something that prompts the editor to change the current active version, e.g. delete a version or add a new version */
    SLATE_EVENT(FOnSwitchToVersionDelegate, OnChangeToVersion)
	
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs, UNiagaraScript* InScript, UNiagaraVersionMetaData* InMetadata);

	//~ Begin FNotifyHook Interface
	virtual void NotifyPostChange(const FPropertyChangedEvent& PropertyChangedEvent, FProperty* PropertyThatChanged) override;
	// End of FNotifyHook

	/** See OnVersionDataChanged event */
	void SetOnVersionDataChanged(FSimpleDelegate InOnVersionDataChanged);
	
private:
	UNiagaraScript* Script = nullptr;
	bool bAssetVersionsChanged = false;
	UNiagaraVersionMetaData* VersionMetadata = nullptr;
	FGuid SelectedVersion;
	
	FSimpleDelegate OnVersionDataChanged;
	FOnSwitchToVersionDelegate OnChangeToVersion;
	TSharedPtr<IDetailsView> VersionSettingsDetails;
	TSharedPtr<SGraphActionMenu> VersionListWidget;

	void AddNewMajorVersion();
	void AddNewMinorVersion();
	
	FText FormatVersionLabel(const FNiagaraAssetVersion& Version) const;
	FText GetInfoHeaderText() const;
	TSharedRef<ITableRow> HandleVersionViewGenerateRow(TSharedRef<FNiagaraAssetVersion> Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnActionSelected(const TArray<TSharedPtr<FEdGraphSchemaAction>>& SelectedActions, ESelectInfo::Type InSelectionType);
	TSharedRef<SWidget> GetVersionSelectionHeaderWidget(TSharedRef<SWidget> RowWidget, int32 SectionID);
	void CollectAllVersionActions(FGraphActionListBuilderBase& OutAllActions);
	void VersionInListSelected(FNiagaraAssetVersion SelectedVersion);
	static TSharedRef<SExpanderArrow> CreateCustomActionExpander(const FCustomExpanderData& ActionMenuData);
	TSharedRef<SWidget> OnGetAddVersionMenu();
	TSharedPtr<SWidget> OnVersionContextMenuOpening();
	int32 GetDetailWidgetIndex() const;
	FReply EnableVersioning();

	// context menu actions
	bool CanExecuteDeleteAction(FNiagaraAssetVersion AssetVersion);
	bool CanExecuteExposeAction(FNiagaraAssetVersion AssetVersion);
	void ExecuteDeleteAction(FNiagaraAssetVersion AssetVersion);
	void ExecuteExposeAction(FNiagaraAssetVersion AssetVersion);
	void ExecuteSaveAsAssetAction(FNiagaraAssetVersion AssetVersion);
};
