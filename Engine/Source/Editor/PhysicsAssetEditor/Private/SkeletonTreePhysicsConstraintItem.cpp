// Copyright Epic Games, Inc. All Rights Reserved.

#include "SkeletonTreePhysicsConstraintItem.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Images/SImage.h"
#include "EditorStyleSet.h"

#define LOCTEXT_NAMESPACE "FSkeletonTreePhysicsConstraintItem"

FSkeletonTreePhysicsConstraintItem::FSkeletonTreePhysicsConstraintItem(UPhysicsConstraintTemplate* InConstraint, int32 InConstraintIndex, const FName& InBoneName, bool bInIsConstraintOnParentBody, const TSharedRef<class ISkeletonTree>& InSkeletonTree)
	: FSkeletonTreeItem(InSkeletonTree)
	, Constraint(InConstraint)
	, ConstraintIndex(InConstraintIndex)
	, bIsConstraintOnParentBody(bInIsConstraintOnParentBody)
{
	const FConstraintInstance& ConstraintInstance = Constraint->DefaultInstance;
	FText Label = FText::Format(LOCTEXT("ConstraintNameFormat", "[ {0} -> {1} ] Constraint"), FText::FromName(ConstraintInstance.ConstraintBone2), FText::FromName(ConstraintInstance.ConstraintBone1));
	DisplayName = *Label.ToString();
}

void FSkeletonTreePhysicsConstraintItem::GenerateWidgetForNameColumn( TSharedPtr< SHorizontalBox > Box, const TAttribute<FText>& FilterText, FIsSelected InIsSelected )
{
	Box->AddSlot()
	.AutoWidth()
	.Padding(FMargin(0.0f, 1.0f))
	[
		SNew( SImage )
		.ColorAndOpacity(FSlateColor::UseForeground())
		.Image(FEditorStyle::GetBrush("PhysicsAssetEditor.Tree.Constraint"))
	];

	const FConstraintInstance& ConstraintInstance = Constraint->DefaultInstance;

	Box->AddSlot()
		.AutoWidth()
		.Padding(2, 0, 0, 0)
		[
			SNew(STextBlock)
			.ColorAndOpacity(this, &FSkeletonTreePhysicsConstraintItem::GetConstraintTextColor)
			.Text(FText::FromName(DisplayName))
			.HighlightText(FilterText)
			.Font(FEditorStyle::GetFontStyle("PhysicsAssetEditor.Tree.Font"))
			.ToolTipText(FText::Format(LOCTEXT("ConstraintTooltip", "Constraint linking child body [{0}] to parent body [{1}]"), FText::FromName(ConstraintInstance.ConstraintBone1), FText::FromName(ConstraintInstance.ConstraintBone2)))
	];
}

TSharedRef< SWidget > FSkeletonTreePhysicsConstraintItem::GenerateWidgetForDataColumn(const FName& DataColumnName, FIsSelected InIsSelected)
{
	return SNullWidget::NullWidget;
}

FSlateColor FSkeletonTreePhysicsConstraintItem::GetConstraintTextColor() const
{
	const FLinearColor Color(1.0f, 1.0f, 1.0f);
	const bool bInCurrentProfile = Constraint->GetCurrentConstraintProfileName() == NAME_None || Constraint->ContainsConstraintProfile(Constraint->GetCurrentConstraintProfileName());
	if(bInCurrentProfile)
	{
		return FSlateColor(Color);
	}
	else
	{
		return FSlateColor(Color.Desaturate(0.5f));
	}
}

#undef LOCTEXT_NAMESPACE
