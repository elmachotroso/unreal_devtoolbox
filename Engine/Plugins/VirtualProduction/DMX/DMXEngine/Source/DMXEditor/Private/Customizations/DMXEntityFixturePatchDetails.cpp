// Copyright Epic Games, Inc. All Rights Reserved.

#include "Customizations/DMXEntityFixturePatchDetails.h"

#include "DMXEditorUtils.h"
#include "Library/DMXEntityFixturePatch.h"
#include "Widgets/SDMXEntityDropdownMenu.h"

#include "DetailLayoutBuilder.h"
#include "DetailWidgetRow.h"
#include "IPropertyUtilities.h"
#include "ScopedTransaction.h"


#define LOCTEXT_NAMESPACE "DMXEntityFixturePatchFixtureSettingsDetails"

FDMXEntityFixturePatchDetails::FDMXEntityFixturePatchDetails(TWeakPtr<FDMXEditor> InDMXEditorPtr)
	: DMXEditorPtr(InDMXEditorPtr)
{}

TSharedRef<IDetailCustomization> FDMXEntityFixturePatchDetails::MakeInstance(TWeakPtr<FDMXEditor> InDMXEditorPtr)
{
	return MakeShared<FDMXEntityFixturePatchDetails>(InDMXEditorPtr);
}

void FDMXEntityFixturePatchDetails::CustomizeDetails(IDetailLayoutBuilder& DetailBuilder)
{
	PropertyUtilities = DetailBuilder.GetPropertyUtilities();

	AutoAssignAddressHandle = DetailBuilder.GetProperty(UDMXEntityFixturePatch::GetAutoAssignAddressPropertyNameChecked());
	ParentFixtureTypeHandle = DetailBuilder.GetProperty(UDMXEntityFixturePatch::GetParentFixtureTypeTemplatePropertyNameChecked());
	ActiveModeHandle = DetailBuilder.GetProperty(UDMXEntityFixturePatch::GetActiveModePropertyNameChecked());

	// Bind to auto assign address changes to assign channels when it gets enabled
	FSimpleDelegate OnAutoAssignAddressChangedDelegate = FSimpleDelegate::CreateSP(this, &FDMXEntityFixturePatchDetails::OnAutoAssignAddressChanged);
	AutoAssignAddressHandle->SetOnPropertyValueChanged(OnAutoAssignAddressChangedDelegate);

	// Handle mode changes of the parent fixture type
	UDMXEntityFixtureType::GetOnFixtureTypeChanged().AddSP(this, &FDMXEntityFixturePatchDetails::OnFixtureTypeChanged);

	// Make a Fixture Types dropdown for the Fixture Type template property
	DetailBuilder.EditDefaultProperty(ParentFixtureTypeHandle)->CustomWidget(false)
		.NameContent()
		[
			ParentFixtureTypeHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MinDesiredWidth(200.0f)
		.MaxDesiredWidth(400.0f)
		[
			SNew(SDMXEntityPickerButton<UDMXEntityFixtureType>)
			.DMXEditor(DMXEditorPtr)
			.CurrentEntity(this, &FDMXEntityFixturePatchDetails::GetParentFixtureType)
			.OnEntitySelected(this, &FDMXEntityFixturePatchDetails::OnParentFixtureTypeChanged)
			.HasMultipleValues(this, &FDMXEntityFixturePatchDetails::IsParentFixtureTypeMultipleValues)
		];

	// Make a modes dropdown to select the active Fixture Type Mode, if a valid Fixture Type is selected
	TSharedPtr<uint32> DefaultSelectedActiveMode = nullptr;
	GenerateActiveModesSource();

	int32 ActiveMode;
	if (ensure(ActiveModeHandle->GetValue(ActiveMode) == FPropertyAccess::Success))
	{
		const bool bActiveModeExists = ActiveModesSource.ContainsByPredicate([ActiveMode](TSharedPtr<uint32> Option) 
			{
				return Option.IsValid() && *Option == ActiveMode;
			});

		if (!bActiveModeExists)
		{
			SetActiveMode(0);
		}
	}

	DetailBuilder.EditDefaultProperty(ActiveModeHandle)->CustomWidget(false)
		.NameContent()
		[
			ActiveModeHandle->CreatePropertyNameWidget()
		]
		.ValueContent()
		.MaxDesiredWidth(160.0f)
		[
			SAssignNew(ActiveModeComboBox, SComboBox<TSharedPtr<uint32>>)
			.IsEnabled(this, &FDMXEntityFixturePatchDetails::IsActiveModeEditable)
			.OptionsSource(&ActiveModesSource)
			.OnGenerateWidget(this, &FDMXEntityFixturePatchDetails::GenerateActiveModeWidget)
			.OnSelectionChanged(this, &FDMXEntityFixturePatchDetails::OnActiveModeChanged)
			.InitiallySelectedItem(DefaultSelectedActiveMode)
			[
				SNew(STextBlock)
				.MinDesiredWidth(50.0f)
				.Text(this, &FDMXEntityFixturePatchDetails::GetCurrentActiveModeLabel)
				.Font(DetailBuilder.GetDetailFont())
			]
		];
}

TSharedRef<SWidget> FDMXEntityFixturePatchDetails::GenerateActiveModeWidget(const TSharedPtr<uint32> InMode) const
{
	UObject* Object = nullptr;

	if (ParentFixtureTypeHandle->GetValue(Object) == FPropertyAccess::Success && Object != nullptr)
	{
		if (UDMXEntityFixtureType* Patch = Cast<UDMXEntityFixtureType>(Object))
		{
			if (InMode.IsValid() && 
				Patch->Modes.IsValidIndex(*InMode))
			{
				return 
					SNew(STextBlock)
					.Text(FText::FromString(Patch->Modes[*InMode].ModeName));
			}
		}
	}

	return SNullWidget::NullWidget;
}

void FDMXEntityFixturePatchDetails::OnParentFixtureTypeChanged(UDMXEntity* NewTemplate) const
{
	ParentFixtureTypeHandle->SetValue(Cast<UDMXEntityFixtureType>(NewTemplate));
}

void FDMXEntityFixturePatchDetails::OnFixtureTypeChanged(const UDMXEntityFixtureType* FixtureType)
{
	if (IsValid(FixtureType) && !FixtureType->HasAnyFlags(RF_Transactional))
	{
		// Keep the active mode valid
		int32 ActiveMode;
		if (ActiveModeHandle->GetValue(ActiveMode) == FPropertyAccess::Success)
		{
			if (!FixtureType->Modes.IsValidIndex(ActiveMode))
			{
				const int32 NewActiveMode = FixtureType->Modes.Num() > 0 ? 0 : INDEX_NONE;
				ActiveModeHandle->SetValue(NewActiveMode);
			}
		}
	
		PropertyUtilities->ForceRefresh();
	}
}

void FDMXEntityFixturePatchDetails::OnActiveModeChanged(const TSharedPtr<uint32> InSelectedMode, ESelectInfo::Type SelectInfo)
{
	if (InSelectedMode.IsValid())
	{
		ActiveModeHandle->SetValue(*InSelectedMode);
	}
}

void FDMXEntityFixturePatchDetails::OnAutoAssignAddressChanged()
{
	bool bAutoAssignAddress;
	if (ensure(AutoAssignAddressHandle->GetValue(bAutoAssignAddress) == FPropertyAccess::Success))
	{
		if (bAutoAssignAddress)
		{
			TArray<UObject*> OuterObjects;
			AutoAssignAddressHandle->GetOuterObjects(OuterObjects);

			TArray<UDMXEntityFixturePatch*> FixturePatches;
			for (UObject* Object : OuterObjects)
			{
				UDMXEntityFixturePatch* Patch = CastChecked<UDMXEntityFixturePatch>(Object);
				FixturePatches.Add(Patch);
			}

			FDMXEditorUtils::AutoAssignedAddresses(FixturePatches);
		}
	}
}

void FDMXEntityFixturePatchDetails::GenerateActiveModesSource()
{
	ActiveModesSource.Reset();

	UObject* Object = nullptr;
	if (ParentFixtureTypeHandle->GetValue(Object) == FPropertyAccess::Success)
	{
		if (UDMXEntityFixtureType* Fixture = Cast<UDMXEntityFixtureType>(Object))
		{
			const uint32 NumModes = Fixture->Modes.Num();
			for (uint32 ModeIndex = 0; ModeIndex < NumModes; ++ModeIndex)
			{
				ActiveModesSource.Add(MakeShared<uint32>(ModeIndex));
			}
		}
	}
}

TWeakObjectPtr<UDMXEntityFixtureType> FDMXEntityFixturePatchDetails::GetParentFixtureType() const
{
	UObject* Object;
	if (ParentFixtureTypeHandle->GetValue(Object) == FPropertyAccess::Success)
	{
		return Cast<UDMXEntityFixtureType>(Object);
	}
	return nullptr;
}


bool FDMXEntityFixturePatchDetails::IsParentFixtureTypeMultipleValues() const
{
	UObject* Object;
	return ParentFixtureTypeHandle->GetValue(Object) == FPropertyAccess::MultipleValues;
}

bool FDMXEntityFixturePatchDetails::IsActiveModeEditable() const
{
	UObject* Object = nullptr;
	if (ParentFixtureTypeHandle->GetValue(Object) == FPropertyAccess::Success && Object)
	{
		if (UDMXEntityFixtureType* Fixture = Cast<UDMXEntityFixtureType>(Object))
		{
			return Fixture->Modes.Num() > 0;
		}

		return false;
	}

	return false;
}

FText FDMXEntityFixturePatchDetails::GetCurrentActiveModeLabel() const
{
	static const FText MultipleValuesLabel = LOCTEXT("MultipleValuesLabel", "Multiple Values");
	static const FText NullTypeLabel = LOCTEXT("NullFixtureTypeLabel", "No Fixture Type selected");
	static const FText MultipleTypesLabel = LOCTEXT("MultipleFixtureTypesLabel", "Multiple Types Selected");
	static const FText NoModesLabel = LOCTEXT("NoModesLabel", "No modes in Fixture Type");

	UObject* Object = nullptr;
	const FPropertyAccess::Result FixtureTemplateAccessResult = ParentFixtureTypeHandle->GetValue(Object);
	UDMXEntityFixtureType* FixtureTemplate = Cast<UDMXEntityFixtureType>(Object);

	// Is only one type of Fixture Type selected?
	if (FixtureTemplateAccessResult == FPropertyAccess::Success)
	{
		// Is this type valid?
		if (FixtureTemplate != nullptr)
		{
			// We can try to get the mode, although it could be a different one for each of the templates
			int32 ModeValue = 0;
			if (ActiveModeHandle->GetValue(ModeValue) == FPropertyAccess::Success)
			{
				const TArray<FDMXFixtureMode>& CurrentModes = FixtureTemplate->Modes;
				if (CurrentModes.Num() > 0 && CurrentModes.IsValidIndex(ModeValue))
				{
					return FText::FromString(
						CurrentModes[ModeValue].ModeName
					);
				}
				else
				{
					return NoModesLabel;
				}
			}

			return MultipleValuesLabel;
		}

		return NullTypeLabel;
	}

	return MultipleTypesLabel;
}

void FDMXEntityFixturePatchDetails::SetActiveMode(int32 ModeIndex)
{
	TArray<UObject*> OuterObjects;
	ActiveModeHandle->GetOuterObjects(OuterObjects);

	if (OuterObjects.Num() > 0)
	{
		const FScopedTransaction Transaction(LOCTEXT("SetFixturePatchActiveModeTransaction", "Set DMX Fixture Patch Active Mode"));

		for (UObject* Object : OuterObjects)
		{
			UDMXEntityFixturePatch* Patch = CastChecked<UDMXEntityFixturePatch>(Object);
			Patch->Modify();
			Patch->PreEditChange(UDMXEntityFixturePatch::StaticClass()->FindPropertyByName(UDMXEntityFixturePatch::GetActiveModePropertyNameChecked()));

			Patch->SetActiveModeIndex(ModeIndex);

			Patch->PostEditChange();
		}
	}
}

#undef LOCTEXT_NAMESPACE
