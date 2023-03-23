// Copyright Epic Games, Inc. All Rights Reserved.

#include "SUSDLayersTreeView.h"

#include "SUSDStageEditorStyle.h"
#include "USDLayersViewModel.h"
#include "USDLayerUtils.h"
#include "USDMemory.h"
#include "USDStageActor.h"
#include "USDStageModule.h"
#include "USDTypesConversion.h"

#include "UsdWrappers/SdfLayer.h"
#include "UsdWrappers/UsdStage.h"

#include "EditorStyleSet.h"
#include "Engine/World.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Modules/ModuleManager.h"
#include "Styling/SlateTypes.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SToolTip.h"

#if USE_USD_SDK

#define LOCTEXT_NAMESPACE "SUSDLayersTreeView"

class FUsdLayerNameColumn : public FUsdTreeViewColumn
{
public:
	virtual TSharedRef< SWidget > GenerateWidget( const TSharedPtr< IUsdTreeViewItem > InTreeItem, const TSharedPtr< ITableRow > TableRow ) override
	{
		FUsdLayerViewModelRef TreeItem = StaticCastSharedRef< FUsdLayerViewModel >( InTreeItem.ToSharedRef() );
		TWeakPtr<FUsdLayerViewModel> TreeItemWeak = TreeItem;

		return SNew( SBox )
			.VAlign( VAlign_Center )
			[
				SNew(STextBlock)
				.Text( TreeItem, &FUsdLayerViewModel::GetDisplayName )
				.ToolTipText_Lambda( [TreeItemWeak]
				{
					if ( TSharedPtr<FUsdLayerViewModel> PinnedTreeItem = TreeItemWeak.Pin() )
					{
						return FText::FromString( PinnedTreeItem->LayerIdentifier );
					}

					return FText::GetEmpty();
				})
			];
	}
};

class FUsdLayerMutedColumn : public FUsdTreeViewColumn, public TSharedFromThis< FUsdLayerMutedColumn >
{
public:
	FReply OnClicked( const FUsdLayerViewModelRef TreeItem )
	{
		ToggleMuteLayer( TreeItem );

		return FReply::Handled();
	}

	const FSlateBrush* GetBrush( const FUsdLayerViewModelRef TreeItem, const TSharedPtr< SButton > Button ) const
	{
		const bool bIsButtonHovered = Button.IsValid() && Button->IsHovered();

		if ( !CanMuteLayer( TreeItem ) )
		{
			return nullptr;
		}
		else if ( TreeItem->LayerModel->bIsMuted )
		{
			return bIsButtonHovered
				? FEditorStyle::GetBrush( "Level.NotVisibleHighlightIcon16x" )
				: FEditorStyle::GetBrush( "Level.NotVisibleIcon16x" );
		}
		else
		{
			return bIsButtonHovered
				? FEditorStyle::GetBrush( "Level.VisibleHighlightIcon16x" )
				: FEditorStyle::GetBrush( "Level.VisibleIcon16x" );
		}
	}

	FSlateColor GetForegroundColor( const FUsdLayerViewModelRef TreeItem, const TSharedPtr< ITableRow > TableRow, const TSharedPtr< SButton > Button ) const
	{
		if ( !TableRow.IsValid() || !Button.IsValid() )
		{
			return FSlateColor::UseForeground();
		}

		const bool bIsRowHovered = TableRow->AsWidget()->IsHovered();
		const bool bIsButtonHovered = Button->IsHovered();
		const bool bIsRowSelected = TableRow->IsItemSelected();
		const bool bIsLayerMuted = TreeItem->IsLayerMuted();

		if ( !bIsLayerMuted && !bIsRowHovered && !bIsRowSelected )
		{
			return FLinearColor::Transparent;
		}
		else if ( bIsButtonHovered && !bIsRowSelected )
		{
			return FEditorStyle::GetSlateColor( TEXT( "Colors.ForegroundHover" ) );
		}

		return FSlateColor::UseForeground();
	}

	virtual TSharedRef< SWidget > GenerateWidget( const TSharedPtr< IUsdTreeViewItem > InTreeItem, const TSharedPtr< ITableRow > TableRow ) override
	{
		if ( !InTreeItem )
		{
			return SNullWidget::NullWidget;
		}

		FUsdLayerViewModelRef TreeItem = StaticCastSharedRef< FUsdLayerViewModel >( InTreeItem.ToSharedRef() );
		const float ItemSize = FUsdStageEditorStyle::Get()->GetFloat( "UsdStageEditor.ListItemHeight" );

		if ( !TreeItem->CanMuteLayer() )
		{
			return SNew( SBox )
				.HeightOverride( ItemSize )
				.WidthOverride( ItemSize )
				.Visibility( EVisibility::Visible )
				.ToolTip( SNew( SToolTip ).Text( LOCTEXT( "CantMuteLayerTooltip", "This layer cannot be muted!" ) ) );
		}

		TSharedPtr<SButton> Button = SNew( SButton )
			.ContentPadding( 0 )
			.ButtonStyle( FUsdStageEditorStyle::Get(), TEXT("NoBorder") )
			.OnClicked( this, &FUsdLayerMutedColumn::OnClicked, TreeItem )
			.ToolTip( SNew( SToolTip ).Text( LOCTEXT( "MuteLayerTooltip", "Mute or unmute this layer" ) ) )
			.HAlign( HAlign_Center )
			.VAlign( VAlign_Center );

		TSharedPtr<SImage> Image = SNew( SImage )
			.Image( this, &FUsdLayerMutedColumn::GetBrush, TreeItem, Button )
			.ColorAndOpacity( this, &FUsdLayerMutedColumn::GetForegroundColor, TreeItem, TableRow, Button );

		Button->SetContent( Image.ToSharedRef() );

		return SNew( SBox )
			.HeightOverride( ItemSize )
			.WidthOverride( ItemSize )
			.Visibility( EVisibility::Visible )
			[
				Button.ToSharedRef()
			];
	}

protected:
	bool CanMuteLayer( FUsdLayerViewModelRef LayerItem ) const
	{
		if ( !LayerItem->IsValid() )
		{
			return false;
		}

		return LayerItem->CanMuteLayer();
	}

	void ToggleMuteLayer( FUsdLayerViewModelRef LayerItem )
	{
		if ( !LayerItem->IsValid() || !CanMuteLayer( LayerItem ) )
		{
			return;
		}

		LayerItem->ToggleMuteLayer();
	}
};

class FUsdLayerEditColumn : public FUsdTreeViewColumn, public TSharedFromThis< FUsdLayerEditColumn >
{
public:
	const FSlateBrush* GetCheckedImage( const FUsdLayerViewModelRef InTreeItem ) const
	{
		return InTreeItem->LayerModel->bIsEditTarget ?
			&FEditorStyle::Get().GetWidgetStyle< FCheckBoxStyle >( "Checkbox" ).CheckedImage :
			nullptr;
	}

	virtual TSharedRef< SWidget > GenerateWidget( const TSharedPtr< IUsdTreeViewItem > InTreeItem, const TSharedPtr< ITableRow > TableRow ) override
	{
		const FUsdLayerViewModelRef TreeItem = StaticCastSharedRef< FUsdLayerViewModel >( InTreeItem.ToSharedRef() );

		TSharedRef< SWidget > Item =
			SNew(SImage)
				.Image( this, &FUsdLayerEditColumn::GetCheckedImage, TreeItem )
				.ColorAndOpacity( FSlateColor::UseForeground() );

		float ItemSize = FUsdStageEditorStyle::Get()->GetFloat( "UsdStageEditor.ListItemHeight" );

		return SNew( SBox )
			.HeightOverride( ItemSize )
			.WidthOverride( ItemSize )
			.HAlign( HAlign_Center )
			.VAlign( VAlign_Center )
			[
				Item
			];
	}
};

void SUsdLayersTreeView::Construct( const FArguments& InArgs, AUsdStageActor* UsdStageActor )
{
	SUsdTreeView::Construct( SUsdTreeView::FArguments() );

	OnContextMenuOpening = FOnContextMenuOpening::CreateSP( this, &SUsdLayersTreeView::ConstructLayerContextMenu );

	BuildUsdLayersEntries( UsdStageActor );
}

void SUsdLayersTreeView::Refresh( AUsdStageActor* UsdStageActor, bool bResync )
{
	if ( bResync )
	{
		BuildUsdLayersEntries( UsdStageActor );
	}
	else
	{
		for ( FUsdLayerViewModelRef TreeItem :  RootItems )
		{
			TreeItem->RefreshData();
		}
	}

	RequestTreeRefresh();
}

TSharedRef< ITableRow > SUsdLayersTreeView::OnGenerateRow( FUsdLayerViewModelRef InDisplayNode, const TSharedRef< STableViewBase >& OwnerTable )
{
	return SNew( SUsdTreeRow< FUsdLayerViewModelRef >, InDisplayNode, OwnerTable, SharedData );
}

void SUsdLayersTreeView::OnGetChildren( FUsdLayerViewModelRef InParent, TArray< FUsdLayerViewModelRef >& OutChildren ) const
{
	for ( const FUsdLayerViewModelRef& Child : InParent->GetChildren() )
	{
		OutChildren.Add( Child );
	}
}

void SUsdLayersTreeView::BuildUsdLayersEntries( AUsdStageActor* UsdStageActor )
{
	RootItems.Empty();

	if ( !UsdStageActor )
	{
		return;
	}

	// The cast here forces us to use the const version of GetUsdStage, that won't force-load the stage in case it isn't opened yet
	if ( const UE::FUsdStage& UsdStage = const_cast< const AUsdStageActor* >( UsdStageActor )->GetUsdStage() )
	{
		RootItems.Add( MakeSharedUnreal< FUsdLayerViewModel >( nullptr, UsdStage, UsdStage.GetRootLayer().GetIdentifier() ) );
		RootItems.Add( MakeSharedUnreal< FUsdLayerViewModel >( nullptr, UsdStage, UsdStage.GetSessionLayer().GetIdentifier() ) );
	}
}

void SUsdLayersTreeView::SetupColumns()
{
	HeaderRowWidget->ClearColumns();

	SHeaderRow::FColumn::FArguments LayerMutedColumnArguments;
	LayerMutedColumnArguments.FixedWidth( 24.f );

	TSharedRef< FUsdLayerMutedColumn > LayerMutedColumn = MakeShared< FUsdLayerMutedColumn >();
	AddColumn( TEXT("Mute"), FText(), LayerMutedColumn, LayerMutedColumnArguments );

	TSharedRef< FUsdLayerNameColumn > LayerNameColumn = MakeShared< FUsdLayerNameColumn >();
	LayerNameColumn->bIsMainColumn = true;

	AddColumn( TEXT("Layers"), LOCTEXT( "Layers", "Layers" ), LayerNameColumn );

	TSharedRef< FUsdLayerEditColumn > LayerEditColumn = MakeShared< FUsdLayerEditColumn >();
	AddColumn( TEXT("Edit"), LOCTEXT( "Edit", "Edit" ), LayerEditColumn );
}

TSharedPtr< SWidget > SUsdLayersTreeView::ConstructLayerContextMenu()
{
	TSharedRef< SWidget > MenuWidget = SNullWidget::NullWidget;

	FMenuBuilder LayerOptions( true, nullptr );
	LayerOptions.BeginSection( "Layer", LOCTEXT("Layer", "Layer") );
	{
		LayerOptions.AddMenuEntry(
			LOCTEXT("EditLayer", "Edit"),
			LOCTEXT("EditLayer_ToolTip", "Sets the layer as the edit target"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP( this, &SUsdLayersTreeView::OnEditSelectedLayer ),
				FCanExecuteAction::CreateSP( this, &SUsdLayersTreeView::CanEditSelectedLayer )
			),
			NAME_None,
			EUserInterfaceActionType::Button
		);
	}
	LayerOptions.EndSection();

	LayerOptions.BeginSection( "SubLayers", LOCTEXT("SubLayers", "SubLayers") );
	{
		LayerOptions.AddMenuEntry(
			LOCTEXT("AddExistingSubLayer", "Add Existing"),
			LOCTEXT("AddExistingSubLayer_ToolTip", "Adds a sublayer from an existing file to this layer"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP( this, &SUsdLayersTreeView::OnAddSubLayer ),
				FCanExecuteAction::CreateSP( this, &SUsdLayersTreeView::CanAddSubLayer )
			),
			NAME_None,
			EUserInterfaceActionType::Button
		);

		LayerOptions.AddMenuEntry(
			LOCTEXT("AddNewSubLayer", "Add New"),
			LOCTEXT("AddNewSubLayer_ToolTip", "Adds a sublayer using a new file to this layer"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP( this, &SUsdLayersTreeView::OnNewSubLayer ),
				FCanExecuteAction::CreateSP( this, &SUsdLayersTreeView::CanAddSubLayer )
			),
			NAME_None,
			EUserInterfaceActionType::Button
		);

		LayerOptions.AddMenuEntry(
			LOCTEXT("RemoveSubLayer", "Remove"),
			LOCTEXT("RemoveSubLayer_ToolTip", "Removes the sublayer from its owner"),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateSP( this, &SUsdLayersTreeView::OnRemoveSelectedLayers ),
				FCanExecuteAction::CreateSP( this, &SUsdLayersTreeView::CanRemoveSelectedLayers )
			),
			NAME_None,
			EUserInterfaceActionType::Button
		);
	}
	LayerOptions.EndSection();

	MenuWidget = LayerOptions.MakeWidget();

	return MenuWidget;
}

bool SUsdLayersTreeView::CanEditSelectedLayer() const
{
	bool bHasEditableLayer = false;

	TArray< FUsdLayerViewModelRef > MySelectedItems = GetSelectedItems();

	for ( FUsdLayerViewModelRef SelectedItem : MySelectedItems )
	{
		if ( SelectedItem->CanEditLayer() )
		{
			bHasEditableLayer = true;
			break;
		}
	}

	return bHasEditableLayer;
}

void SUsdLayersTreeView::OnEditSelectedLayer()
{
	TArray< FUsdLayerViewModelRef > MySelectedItems = GetSelectedItems();

	for ( FUsdLayerViewModelRef SelectedItem : MySelectedItems )
	{
		if ( SelectedItem->EditLayer() )
		{
			break;
		}
	}
}

bool SUsdLayersTreeView::CanAddSubLayer() const
{
	return GetSelectedItems().Num() > 0;
}

void SUsdLayersTreeView::OnAddSubLayer()
{
	TOptional< FString > SubLayerFile = UsdUtils::BrowseUsdFile( UsdUtils::EBrowseFileMode::Open, AsShared() );

	if ( !SubLayerFile )
	{
		return;
	}

	TArray< FUsdLayerViewModelRef > MySelectedItems = GetSelectedItems();

	for ( FUsdLayerViewModelRef SelectedItem : MySelectedItems )
	{
		SelectedItem->AddSubLayer( *SubLayerFile.GetValue() );
		break;
	}

	RequestTreeRefresh();
}

void SUsdLayersTreeView::OnNewSubLayer()
{
	TOptional< FString > SubLayerFile = UsdUtils::BrowseUsdFile( UsdUtils::EBrowseFileMode::Save, AsShared() );

	if ( !SubLayerFile )
	{
		return;
	}

	TArray< FUsdLayerViewModelRef > MySelectedItems = GetSelectedItems();

	{
		FScopedUsdAllocs UsdAllocs;
		for ( FUsdLayerViewModelRef SelectedItem : MySelectedItems )
		{
			SelectedItem->NewSubLayer( *SubLayerFile.GetValue() );
			break;
		}
	}

	RequestTreeRefresh();
}

bool SUsdLayersTreeView::CanRemoveLayer( FUsdLayerViewModelRef LayerItem ) const
{
	// We can't remove root layers
	return ( LayerItem->IsValid() && LayerItem->ParentItem && LayerItem->ParentItem->IsValid() );
}

bool SUsdLayersTreeView::CanRemoveSelectedLayers() const
{
	bool bHasRemovableLayer = false;

	TArray< FUsdLayerViewModelRef > SelectedLayers = GetSelectedItems();

	for ( FUsdLayerViewModelRef SelectedLayer : SelectedLayers )
	{
		// We can't remove root layers
		if ( CanRemoveLayer( SelectedLayer ) )
		{
			bHasRemovableLayer = true;
			break;
		}
	}

	return bHasRemovableLayer;
}

void SUsdLayersTreeView::OnRemoveSelectedLayers()
{
	bool bLayerRemoved = false;

	TArray< FUsdLayerViewModelRef > SelectedLayers = GetSelectedItems();

	for ( FUsdLayerViewModelRef SelectedLayer : SelectedLayers )
	{
		if ( !CanRemoveLayer( SelectedLayer ) )
		{
			continue;
		}

		{
			FScopedUsdAllocs UsdAllocs;

			int32 SubLayerIndex = 0;
			for ( FUsdLayerViewModelRef Child : SelectedLayer->ParentItem->Children )
			{
				if ( Child->LayerIdentifier == SelectedLayer->LayerIdentifier )
				{
					if ( SelectedLayer->ParentItem )
					{
						bLayerRemoved = SelectedLayer->ParentItem->RemoveSubLayer( SubLayerIndex );
					}
					break;
				}

				++SubLayerIndex;
			}
		}
	}

	if ( bLayerRemoved )
	{
		RequestTreeRefresh();
	}
}

#undef LOCTEXT_NAMESPACE

#endif // #if USE_USD_SDK
