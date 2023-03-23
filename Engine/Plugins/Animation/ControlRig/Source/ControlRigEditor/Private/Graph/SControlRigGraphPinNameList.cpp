// Copyright Epic Games, Inc. All Rights Reserved.


#include "Graph/SControlRigGraphPinNameList.h"
#include "Graph/SControlRigGraphPinNameListValueWidget.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Images/SImage.h"
#include "EdGraphSchema_K2.h"
#include "ScopedTransaction.h"
#include "DetailLayoutBuilder.h"
#include "Graph/ControlRigGraph.h"

namespace SControlRigGraphPinNameListDefs
{
	// Active foreground pin alpha
	static const float ActivePinForegroundAlpha = 1.f;
	// InActive foreground pin alpha
	static const float InactivePinForegroundAlpha = 0.15f;
	// Active background pin alpha
	static const float ActivePinBackgroundAlpha = 0.8f;
	// InActive background pin alpha
	static const float InactivePinBackgroundAlpha = 0.4f;
};

void SControlRigGraphPinNameList::Construct(const FArguments& InArgs, UEdGraphPin* InGraphPinObj)
{
	this->ModelPin = InArgs._ModelPin;
	this->OnGetNameListContent = InArgs._OnGetNameListContent;
	this->OnGetNameFromSelection = InArgs._OnGetNameFromSelection;
	this->bMarkupInvalidItems = InArgs._MarkupInvalidItems;

	CurrentList = GetNameList();
	SGraphPin::Construct(SGraphPin::FArguments(), InGraphPinObj);
}

TSharedRef<SWidget>	SControlRigGraphPinNameList::GetDefaultValueWidget()
{
	TSharedPtr<FString> InitialSelected;
	const TArray<TSharedPtr<FString>>* List = GetNameList();
	for (TSharedPtr<FString> Item : (*List))
	{
		if (Item->Equals(GetNameListText().ToString()))
		{
			InitialSelected = Item;
		}
	}

	return SNew(SBox)
		.MinDesiredWidth(150)
		[
			SNew(SHorizontalBox)
			+SHorizontalBox::Slot()
			[
				SAssignNew(NameListComboBox, SControlRigGraphPinNameListValueWidget)
					.Visibility(this, &SGraphPin::GetDefaultValueVisibility)
					.OptionsSource(CurrentList)
					.OnGenerateWidget(this, &SControlRigGraphPinNameList::MakeNameListItemWidget)
					.OnSelectionChanged(this, &SControlRigGraphPinNameList::OnNameListChanged)
					.OnComboBoxOpening(this, &SControlRigGraphPinNameList::OnNameListComboBox)
					.InitiallySelectedItem(InitialSelected)
					.Content()
					[
						SNew(STextBlock)
						.Text(this, &SControlRigGraphPinNameList::GetNameListText)
						.ColorAndOpacity(this, &SControlRigGraphPinNameList::GetNameColor)
						.Font( FEditorStyle::GetFontStyle( TEXT("PropertyWindow.NormalFont") ) )
					]
			]
		
			// Use button
			+SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(1,0)
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle( FEditorStyle::Get(), "NoBorder" )
				.ButtonColorAndOpacity( this, &SControlRigGraphPinNameList::OnGetWidgetBackground )
				.OnClicked(this, &SControlRigGraphPinNameList::OnGetSelectedClicked)
				.ContentPadding(1.f)
				.ToolTipText(NSLOCTEXT("ControlRigElementDetails", "ObjectGraphPin_Use_Tooltip", "Use item selected"))
				[
					SNew(SImage)
					.ColorAndOpacity( this, &SControlRigGraphPinNameList::OnGetWidgetForeground )
					.Image(FEditorStyle::GetBrush("Icons.CircleArrowLeft"))
				]
			]

			// Browse button
			+SHorizontalBox::Slot()
			.AutoWidth()
			.Padding(1,0)
			.VAlign(VAlign_Center)
			[
				SNew(SButton)
				.ButtonStyle( FEditorStyle::Get(), "NoBorder" )
				.ButtonColorAndOpacity( this, &SControlRigGraphPinNameList::OnGetWidgetBackground )
				.OnClicked(this, &SControlRigGraphPinNameList::OnBrowseClicked)
				.ContentPadding(0)
				.ToolTipText(NSLOCTEXT("SControlRigGraphPinNameList", "ObjectGraphPin_Browse_Tooltip", "Browse"))
				[
					SNew(SImage)
					.ColorAndOpacity( this, &SControlRigGraphPinNameList::OnGetWidgetForeground )
					.Image(FEditorStyle::GetBrush("Icons.Search"))
				]
			]
				
		];
}

const TArray<TSharedPtr<FString>>* SControlRigGraphPinNameList::GetNameList() const
{
	const TArray<TSharedPtr<FString>>* Result = nullptr;
	if (OnGetNameListContent.IsBound())
	{
		Result = OnGetNameListContent.Execute(ModelPin);
	}

	if(Result == nullptr)
	{
		Result = &EmptyList;
	}

	return Result;
}

FText SControlRigGraphPinNameList::GetNameListText() const
{
	return FText::FromString( GraphPinObj->GetDefaultAsString() );
}

void SControlRigGraphPinNameList::SetNameListText(const FText& NewTypeInValue, ETextCommit::Type /*CommitInfo*/)
{
	if(!GraphPinObj->GetDefaultAsString().Equals(NewTypeInValue.ToString()))
	{
		const FScopedTransaction Transaction( NSLOCTEXT("GraphEditor", "ChangeElementNameListPinValue", "Change Element Name Pin Value" ) );
		GraphPinObj->Modify();
		GraphPinObj->GetSchema()->TrySetDefaultValue(*GraphPinObj, NewTypeInValue.ToString());
	}
}

FSlateColor SControlRigGraphPinNameList::GetNameColor() const
{
	if(bMarkupInvalidItems)
	{
		FString CurrentItem = GetNameListText().ToString();
		
		bool bFound = false;
		for (TSharedPtr<FString> Item : (*CurrentList))
		{
			if (Item->Equals(CurrentItem))
			{
				bFound = true;
				break;
			}
		}

		if(!bFound || CurrentItem.IsEmpty() || CurrentItem == FName(NAME_None).ToString())
		{
			return FSlateColor(FLinearColor::Red);
		}
	}
	return FSlateColor::UseForeground();
}

TSharedRef<SWidget> SControlRigGraphPinNameList::MakeNameListItemWidget(TSharedPtr<FString> InItem)
{
	return 	SNew(STextBlock).Text(FText::FromString(*InItem)).Font(FEditorStyle::GetFontStyle(TEXT("PropertyWindow.NormalFont")));
}

void SControlRigGraphPinNameList::OnNameListChanged(TSharedPtr<FString> NewSelection, ESelectInfo::Type SelectInfo)
{
	if (SelectInfo != ESelectInfo::Direct)
	{
		FString NewValue = FName(NAME_None).ToString();
		if (NewSelection.IsValid())
		{
			NewValue = *NewSelection.Get();
		}
		SetNameListText(FText::FromString(NewValue), ETextCommit::OnEnter);
	}
}

void SControlRigGraphPinNameList::OnNameListComboBox()
{
	CurrentList = GetNameList();
	TSharedPtr<FString> CurrentlySelected;
	for (TSharedPtr<FString> Item : (*CurrentList))
	{
		if (Item->Equals(GetNameListText().ToString()))
		{
			CurrentlySelected = Item;
			break;
		}
	}
	NameListComboBox->SetOptionsSource(CurrentList);
	NameListComboBox->SetSelectedItem(CurrentlySelected);
}

FSlateColor SControlRigGraphPinNameList::OnGetWidgetForeground() const
{
	float Alpha = IsHovered() ? SControlRigGraphPinNameListDefs::ActivePinForegroundAlpha : SControlRigGraphPinNameListDefs::InactivePinForegroundAlpha;
	return FSlateColor(FLinearColor(1.f, 1.f, 1.f, Alpha));
}

FSlateColor SControlRigGraphPinNameList::OnGetWidgetBackground() const
{
	float Alpha = IsHovered() ? SControlRigGraphPinNameListDefs::ActivePinBackgroundAlpha : SControlRigGraphPinNameListDefs::InactivePinBackgroundAlpha;
	return FSlateColor(FLinearColor(1.f, 1.f, 1.f, Alpha));
}

FReply SControlRigGraphPinNameList::OnGetSelectedClicked()
{
	if (UControlRigGraph* Graph = Cast<UControlRigGraph>(GetPinObj()->GetOwningNode()->GetGraph()))
	{
		if (OnGetNameFromSelection.IsBound())
		{
			const TArray<TSharedPtr<FString>> Result = OnGetNameFromSelection.Execute();
			if (Result.Num() > 0)
			{
				if (Result[0].IsValid() && Result[0] != nullptr)
				{
					const FString DefaultValue = *Result[0].Get();
					
					if(ModelPin->GetCustomWidgetName() == TEXT("ElementName"))
					{
						if (URigVMPin* ParentPin = ModelPin->GetParentPin())
						{
							Graph->GetController()->SetPinDefaultValue(ParentPin->GetPinPath(), DefaultValue, true, true, false, true);
							CurrentList = GetNameList();
						}
					}

					else if (ModelPin->GetCustomWidgetName() == TEXT("BoneName"))
					{
						URigHierarchy* Hierarchy = Graph->GetBlueprint()->Hierarchy;
						TArray<FRigElementKey> Keys = Hierarchy->GetSelectedKeys();
						FRigBaseElement* Element = Hierarchy->FindChecked(Keys[0]);
						if (Element->GetType() == ERigElementType::Bone)
						{
							Graph->GetController()->SetPinDefaultValue(ModelPin->GetPinPath(), Keys[0].Name.ToString(), true, true, false, true);
							CurrentList = GetNameList();
						}
					}
					
					// if we don't have a key pin - this is just a plain name.
					// let's derive the type of element this node deals with from its name.
					// there's nothing better in place for now.
					else if(const URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(ModelPin->GetNode()))
					{
						const int32 LastIndex = StaticEnum<ERigElementType>()->GetIndexByName(TEXT("Last")); 
						const FString UnitName = UnitNode->GetScriptStruct()->GetStructCPPName();
						for(int32 EnumIndex = 0; EnumIndex < LastIndex; EnumIndex++)
						{
							const FString EnumDisplayName = StaticEnum<ERigElementType>()->GetDisplayNameTextByIndex(EnumIndex).ToString();
							if(UnitName.Contains(EnumDisplayName))
							{
								const ERigElementType ElementType = (ERigElementType)StaticEnum<ERigElementType>()->GetValueByIndex(EnumIndex);

								FRigElementKey Key;
								FRigElementKey::StaticStruct()->ImportText(*DefaultValue, &Key, nullptr, EPropertyPortFlags::PPF_None, nullptr, FRigElementKey::StaticStruct()->GetName(), true);
								if (Key.IsValid())
								{
									if(Key.Type == ElementType)
									{
										Graph->GetController()->SetPinDefaultValue(ModelPin->GetPinPath(), Key.Name.ToString(), true, true, false, true);
										CurrentList = GetNameList();
									}
								}
								break;
							}
						}
					}
				}
			}
		}
	}
	return FReply::Handled();
}

FReply SControlRigGraphPinNameList::OnBrowseClicked()
{
	if (UControlRigGraph* Graph = Cast<UControlRigGraph>(GetPinObj()->GetOwningNode()->GetGraph()))
	{
		TSharedPtr<FString> Selected = NameListComboBox->GetSelectedItem();
		if (Selected.IsValid() && ModelPin)
		{
			URigVMPin* KeyPin = ModelPin->GetParentPin();
			if(KeyPin && KeyPin->GetCPPTypeObject() == FRigElementKey::StaticStruct())
			{
				// browse to rig element key
				FString DefaultValue = ModelPin->GetParentPin()->GetDefaultValue();
				if (!DefaultValue.IsEmpty())
				{
					FRigElementKey Key;
					FRigElementKey::StaticStruct()->ImportText(*DefaultValue, &Key, nullptr, EPropertyPortFlags::PPF_None, nullptr, FRigElementKey::StaticStruct()->GetName(), true);
					if (Key.IsValid())
					{
						Graph->GetBlueprint()->GetHierarchyController()->SetSelection({Key});
					}
				}
			}
			else if (ModelPin->GetCustomWidgetName() == TEXT("BoneName"))
			{
				// browse to named bone
				const FString DefaultValue = ModelPin->GetDefaultValue();
				FRigElementKey Key(*DefaultValue, ERigElementType::Bone);
				Graph->GetBlueprint()->GetHierarchyController()->SetSelection({Key});
			}
			else
			{
				// if we don't have a key pin - this is just a plain name.
				// let's derive the type of element this node deals with from its name.
				// there's nothing better in place for now.
				if(const URigVMUnitNode* UnitNode = Cast<URigVMUnitNode>(ModelPin->GetNode()))
				{
					const int32 LastIndex = StaticEnum<ERigElementType>()->GetIndexByName(TEXT("Last")); 
					const FString UnitName = UnitNode->GetScriptStruct()->GetStructCPPName();
					for(int32 EnumIndex = 0; EnumIndex < LastIndex; EnumIndex++)
					{
						const FString EnumDisplayName = StaticEnum<ERigElementType>()->GetDisplayNameTextByIndex(EnumIndex).ToString();
						if(UnitName.Contains(EnumDisplayName))
						{
							const FString DefaultValue = ModelPin->GetDefaultValue();
							const ERigElementType ElementType = (ERigElementType)StaticEnum<ERigElementType>()->GetValueByIndex(EnumIndex);
							FRigElementKey Key(*DefaultValue, ElementType);
							Graph->GetBlueprint()->GetHierarchyController()->SetSelection({Key});
							break;
						}
					}
				}
			}
		}
	}

	return FReply::Handled();
}
