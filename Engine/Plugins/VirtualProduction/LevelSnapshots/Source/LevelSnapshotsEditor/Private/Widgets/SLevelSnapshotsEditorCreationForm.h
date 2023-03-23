// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dialogs/CustomDialog.h"

DECLARE_DELEGATE_OneParam(FCloseCreationFormDelegate, const FText& /* Description */);

class ULevelSnapshotsSettings;
class ULevelSnapshotsEditorSettings;
class SWindow;

class SLevelSnapshotsEditorCreationForm : public SCustomDialog
{
public:

	static TSharedRef<SWindow> MakeAndShowCreationWindow(const FCloseCreationFormDelegate& CallOnClose);
	
	SLATE_BEGIN_ARGS(SLevelSnapshotsEditorCreationForm)
	{}
	SLATE_END_ARGS()

	void Construct(
		const FArguments& InArgs,
		TWeakPtr<SWindow> InWidgetWindow,
		const FCloseCreationFormDelegate& CallOnClose
		);

	~SLevelSnapshotsEditorCreationForm();

	TSharedRef<SWidget> MakeDataManagementSettingsDetailsWidget() const;

	FText GetNameOverrideText() const;

	void SetNameOverrideText(const FText& InNewText, ETextCommit::Type InCommitType);
	void SetDescriptionText(const FText& InNewText, ETextCommit::Type InCommitType);

	EVisibility GetNameDiffersFromDefaultAsVisibility() const;

	FReply OnResetNameClicked();
	FReply OnCreateButtonPressed();

	void OnWindowClosed(const TSharedRef<SWindow>& ParentWindow) const;

private:
	
	TWeakPtr< SWindow > WidgetWindow;
	TSharedPtr<SWidget> ResetPathButton;

	bool bNameDiffersFromDefault = false;
	bool bWasCreateSnapshotPressed = false;

	FText DescriptionText;

	FCloseCreationFormDelegate CallOnCloseDelegate;
};
