// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "StateTreeViewModel.h"
#include "Widgets/Views/STreeView.h"

class UStateTreeEditorData;
class UStateTreeState;


class FActionTreeViewDragDrop : public FDragDropOperation
{
public:
	DRAG_DROP_OPERATOR_TYPE(FActionTreeViewDragDrop, FDragDropOperation);

	static TSharedRef<FActionTreeViewDragDrop> New(const UStateTreeState* InState)
	{
		return MakeShareable(new FActionTreeViewDragDrop(InState));
	}

	const UStateTreeState* GetDraggedState() const { return State; }

private:
	FActionTreeViewDragDrop(const UStateTreeState* InState)
		: State(InState)
	{
	}

	const UStateTreeState* State;
};


class SStateTreeView : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SStateTreeView) {}
	SLATE_END_ARGS()

	SStateTreeView();
	~SStateTreeView();

	void Construct(const FArguments& InArgs, TSharedRef<FStateTreeViewModel> StateTreeViewModel);

	void SavePersistentExpandedStates();

private:
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	void UpdateTree(bool bExpandPersistent = false);

	// ViewModel handlers
	void HandleModelAssetChanged();
	void HandleModelStatesRemoved(const TSet<UStateTreeState*>& AffectedParents);
	void HandleModelStatesMoved(const TSet<UStateTreeState*>& AffectedParents, const TSet<UStateTreeState*>& MovedStates);
	void HandleModelStateAdded(UStateTreeState* ParentState, UStateTreeState* NewState);
	void HandleModelStatesChanged(const TSet<UStateTreeState*>& AffectedStates, const FPropertyChangedEvent& PropertyChangedEvent);
	void HandleModelSelectionChanged(const TArray<UStateTreeState*>& SelectedStates);

	// Treeview handlers
	TSharedRef<ITableRow> HandleGenerateRow(UStateTreeState* InState, const TSharedRef<STableViewBase>& InOwnerTableView);
	void HandleGetChildren(UStateTreeState* InParent, TArray<UStateTreeState*>& OutChildren);
	void HandleTreeSelectionChanged(UStateTreeState* InSelectedItem, ESelectInfo::Type SelectionType);
	TSharedPtr<SWidget> HandleContextMenuOpening();

	// Action handlers
	FReply HandleAddStateButton();
	void HandleRenameState(UStateTreeState* State);
	void HandleAddState(UStateTreeState* AfterItem);
	void HandleAddChildState(UStateTreeState* ParentItem);
	void HandleDeleteItems();

	TSharedPtr<FStateTreeViewModel> StateTreeViewModel;

	TSharedPtr<STreeView<UStateTreeState*>> TreeView;
	TSharedPtr<SScrollBar> ExternalScrollbar;

	UStateTreeState* RequestedRenameState;
	bool bItemsDirty;
	bool bUpdatingSelection;
};
