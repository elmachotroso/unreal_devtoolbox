// Copyright Epic Games, Inc. All Rights Reserved.
#include "LevelInstanceEditorModule.h"
#include "LevelInstanceActorDetails.h"
#include "LevelInstancePivotDetails.h"
#include "LevelInstance/LevelInstanceSubsystem.h"
#include "LevelInstance/LevelInstanceActor.h"
#include "PackedLevelActor/PackedLevelActor.h"
#include "LevelInstanceEditorSettings.h"
#include "ToolMenus.h"
#include "Editor.h"
#include "EditorModeManager.h"
#include "EditorModeRegistry.h"
#include "LevelInstanceEditorMode.h"
#include "LevelInstanceEditorModeCommands.h"
#include "LevelEditorMenuContext.h"
#include "ContentBrowserMenuContexts.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "LevelEditor.h"
#include "Engine/Selection.h"
#include "PropertyEditorModule.h"
#include "EditorLevelUtils.h"
#include "Modules/ModuleManager.h"
#include "Misc/MessageDialog.h"
#include "NewLevelDialogModule.h"
#include "Interfaces/IMainFrameModule.h"
#include "Editor/EditorEngine.h"
#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Factories/BlueprintFactory.h"
#include "ClassViewerModule.h"
#include "ClassViewerFilter.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Misc/ScopeExit.h"
#include "Widgets/Input/SNumericEntryBox.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/SWindow.h"
#include "SNewLevelInstanceDialog.h"
#include "MessageLogModule.h"
#include "Settings/EditorExperimentalSettings.h"

IMPLEMENT_MODULE( FLevelInstanceEditorModule, LevelInstanceEditor );

#define LOCTEXT_NAMESPACE "LevelInstanceEditor"

namespace LevelInstanceMenuUtils
{
	bool IsExperimentalSettingEnabled(ALevelInstance* LevelInstance)
	{
		if (LevelInstance->IsA<APackedLevelActor>() && !GetDefault<UEditorExperimentalSettings>()->bPackedLevelActor)
		{
			return false;
		}

		return GetDefault<UEditorExperimentalSettings>()->bLevelInstance;
	}

	FToolMenuSection& CreateLevelSection(UToolMenu* Menu)
	{
		const FName LevelSectionName = TEXT("Level");
		FToolMenuSection* SectionPtr = Menu->FindSection(LevelSectionName);
		if (!SectionPtr)
		{
			SectionPtr = &(Menu->AddSection(LevelSectionName, LOCTEXT("LevelSectionLabel", "Level")));
		}
		FToolMenuSection& Section = *SectionPtr;
		return Section;
	}

	void CreateEditMenuEntry(FToolMenuSection& Section, ALevelInstance* LevelInstance, AActor* ContextActor, bool bSingleEntry)
	{
		FToolUIAction LevelInstanceEditAction;
		FText EntryDesc;
		const bool bCanEdit = LevelInstance->CanEdit(&EntryDesc);

		LevelInstanceEditAction.ExecuteAction.BindLambda([LevelInstance, ContextActor](const FToolMenuContext&)
		{
			LevelInstance->Edit(ContextActor);
		});
		LevelInstanceEditAction.CanExecuteAction.BindLambda([bCanEdit](const FToolMenuContext&)
		{
			return bCanEdit;
		});

		FText EntryLabel = bSingleEntry ? LOCTEXT("EditLevelInstances", "Edit") : FText::FromString(LevelInstance->GetWorldAsset().GetAssetName());
		if (bCanEdit)
		{
			EntryDesc = FText::Format(LOCTEXT("LevelInstanceName", "{0}:{1}"), FText::FromString(LevelInstance->GetActorLabel()), FText::FromString(LevelInstance->GetWorldAssetPackage()));
		}
		Section.AddMenuEntry(NAME_None, EntryLabel, EntryDesc, FSlateIcon(), LevelInstanceEditAction);
	}

	void CreateEditSubMenu(UToolMenu* Menu, TArray<ALevelInstance*> LevelInstanceHierarchy, AActor* ContextActor)
	{
		FToolMenuSection& Section = Menu->AddSection(NAME_None, LOCTEXT("LevelInstanceContextEditSection", "Context"));
		for (ALevelInstance* LevelInstance : LevelInstanceHierarchy)
		{
			CreateEditMenuEntry(Section, LevelInstance, ContextActor, false);
		}
	}
		
	void MoveSelectionToLevelInstance(ALevelInstance* DestinationLevelInstance)
	{
		if (ULevelInstanceSubsystem* LevelInstanceSubsystem = DestinationLevelInstance->GetLevelInstanceSubsystem())
		{
			TArray<AActor*> ActorsToMove;
			ActorsToMove.Reserve(GEditor->GetSelectedActorCount());
			for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
			{
				if (AActor* Actor = Cast<AActor>(*It))
				{
					ActorsToMove.Add(Actor);
				}
			}
			
			LevelInstanceSubsystem->MoveActorsTo(DestinationLevelInstance, ActorsToMove);
		}
	}
		
	void CreateEditMenu(UToolMenu* Menu, AActor* ContextActor)
	{
		if (ULevelInstanceSubsystem* LevelInstanceSubsystem = ContextActor->GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
		{
			TArray<ALevelInstance*> LevelInstanceHierarchy;
			LevelInstanceSubsystem->ForEachLevelInstanceAncestorsAndSelf(ContextActor, [&LevelInstanceHierarchy](ALevelInstance* AncestorLevelInstance)
			{
				if (IsExperimentalSettingEnabled(AncestorLevelInstance))
				{
					LevelInstanceHierarchy.Add(AncestorLevelInstance);
				}
				return true;
			});

			// Don't create sub menu if only one Level Instance is available to edit
			if (LevelInstanceHierarchy.Num() == 1)
			{
				FToolMenuSection& Section = CreateLevelSection(Menu);
				CreateEditMenuEntry(Section, LevelInstanceHierarchy[0], ContextActor, true);
			}
			else if(LevelInstanceHierarchy.Num() > 1)
			{
				FToolMenuSection& Section = CreateLevelSection(Menu);
				Section.AddSubMenu(
					"EditLevelInstances",
					LOCTEXT("EditLevelInstances", "Edit"),
					TAttribute<FText>(),
					FNewToolMenuDelegate::CreateStatic(&CreateEditSubMenu, MoveTemp(LevelInstanceHierarchy), ContextActor)
				);
			}
		}
	}
		
	void CreateCommitDiscardMenu(UToolMenu* Menu, AActor* ContextActor)
	{
		ALevelInstance* LevelInstanceEdit = nullptr;
		if (ContextActor)
		{
			if (ULevelInstanceSubsystem* LevelInstanceSubsystem = ContextActor->GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
			{
				LevelInstanceEdit = LevelInstanceSubsystem->GetEditingLevelInstance();
			}
		}

		if (!LevelInstanceEdit)
		{
			if (ULevelInstanceSubsystem* LevelInstanceSubsystem = GEditor->GetEditorWorldContext().World()->GetSubsystem<ULevelInstanceSubsystem>())
			{
				LevelInstanceEdit = LevelInstanceSubsystem->GetEditingLevelInstance();
			}
		}

		if (LevelInstanceEdit)
		{
			FToolMenuSection& Section = CreateLevelSection(Menu);

			FText CommitTooltip;
			const bool bCanCommit = LevelInstanceEdit->CanCommit(&CommitTooltip);

			FToolUIAction CommitAction;
			CommitAction.ExecuteAction.BindLambda([LevelInstanceEdit](const FToolMenuContext&) { LevelInstanceEdit->Commit(); });
			CommitAction.CanExecuteAction.BindLambda([bCanCommit](const FToolMenuContext&) { return bCanCommit; });
			Section.AddMenuEntry(NAME_None, LOCTEXT("LevelInstanceCommitLabel", "Commit"), CommitTooltip, FSlateIcon(), CommitAction);

			FText DiscardTooltip;
			const bool bCanDiscard = LevelInstanceEdit->CanDiscard(&DiscardTooltip);

			FToolUIAction DiscardAction;
			DiscardAction.ExecuteAction.BindLambda([LevelInstanceEdit](const FToolMenuContext&) { LevelInstanceEdit->Discard(); });
			DiscardAction.CanExecuteAction.BindLambda([bCanDiscard](const FToolMenuContext&) { return bCanDiscard; });
			Section.AddMenuEntry(NAME_None, LOCTEXT("LevelInstanceDiscardLabel", "Discard"), DiscardTooltip, FSlateIcon(), DiscardAction);
		}
	}

	void CreateSetCurrentMenu(UToolMenu* Menu, AActor* ContextActor)
	{
		ALevelInstance* LevelInstanceEdit = nullptr;
		if (ULevelInstanceSubsystem* LevelInstanceSubsystem = ContextActor->GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
		{
			LevelInstanceEdit = LevelInstanceSubsystem->GetEditingLevelInstance();

			if (LevelInstanceEdit)
			{
				FToolUIAction LevelInstanceSetCurrentAction;
				LevelInstanceSetCurrentAction.ExecuteAction.BindLambda([LevelInstanceEdit](const FToolMenuContext&)
					{
						LevelInstanceEdit->SetCurrent();
					});

				FToolMenuSection& Section = CreateLevelSection(Menu);
				Section.AddMenuEntry(NAME_None, LOCTEXT("LevelInstanceSetCurrent", "Set Current Level"), TAttribute<FText>(), FSlateIcon(), LevelInstanceSetCurrentAction);
			}
		}
	}

	void CreateMoveSelectionToMenu(UToolMenu* Menu)
	{
		if (GEditor->GetSelectedActorCount() > 0)
		{
			ALevelInstance* LevelInstanceEdit = nullptr;
			ULevelInstanceSubsystem* LevelInstanceSubsystem = GEditor->GetEditorWorldContext().World()->GetSubsystem<ULevelInstanceSubsystem>();
			if (LevelInstanceSubsystem)
			{
				LevelInstanceEdit = LevelInstanceSubsystem->GetEditingLevelInstance();
			}
			
			if (LevelInstanceEdit)
			{
				FToolUIAction LevelInstanceMoveSelectionAction;

				LevelInstanceMoveSelectionAction.CanExecuteAction.BindLambda([LevelInstanceEdit, LevelInstanceSubsystem](const FToolMenuContext& MenuContext)
					{
						for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
						{
							if (AActor* Actor = Cast<AActor>(*It))
							{
								if (Actor->GetLevel() == LevelInstanceSubsystem->GetLevelInstanceLevel(LevelInstanceEdit))
								{
									return false;
								}
							}
						}

						return GEditor->GetSelectedActorCount() > 0;
					});

				LevelInstanceMoveSelectionAction.ExecuteAction.BindLambda([LevelInstanceEdit](const FToolMenuContext&)
					{
						MoveSelectionToLevelInstance(LevelInstanceEdit);
					});

				FToolMenuSection& Section = CreateLevelSection(Menu);
				Section.AddMenuEntry(NAME_None, LOCTEXT("LevelInstanceMoveSelectionTo", "Move Selection to"), TAttribute<FText>(), FSlateIcon(), LevelInstanceMoveSelectionAction);

			}
		}
	}

	void CreateLevelInstanceFromSelection(ULevelInstanceSubsystem* LevelInstanceSubsystem, ELevelInstanceCreationType CreationType)
	{
		TArray<AActor*> ActorsToMove;
		ActorsToMove.Reserve(GEditor->GetSelectedActorCount());
		for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
		{
			if (AActor* Actor = Cast<AActor>(*It))
			{
				ActorsToMove.Add(Actor);
			}
		}

		IMainFrameModule& MainFrameModule = FModuleManager::GetModuleChecked<IMainFrameModule>("MainFrame");

		TSharedPtr<SWindow> NewLevelInstanceWindow =
			SNew(SWindow)
			.Title(FText::Format(LOCTEXT("NewLevelInstanceWindowTitle", "New {0}"), StaticEnum<ELevelInstanceCreationType>()->GetDisplayNameTextByValue((int64)CreationType)))
			.SupportsMinimize(false)
			.SupportsMaximize(false)
			.SizingRule(ESizingRule::Autosized);

		TSharedRef<SNewLevelInstanceDialog> NewLevelInstanceDialog =
			SNew(SNewLevelInstanceDialog)
			.ParentWindow(NewLevelInstanceWindow)
			.PivotActors(ActorsToMove);

		const bool bForceExternalActors = LevelInstanceSubsystem->GetWorld()->IsPartitionedWorld();
		FNewLevelInstanceParams& DialogParams = NewLevelInstanceDialog->GetCreationParams();
		DialogParams.Type = CreationType;
		DialogParams.HideCreationType();
		DialogParams.SetForceExternalActors(bForceExternalActors);
		NewLevelInstanceWindow->SetContent(NewLevelInstanceDialog);

		FSlateApplication::Get().AddModalWindow(NewLevelInstanceWindow.ToSharedRef(), MainFrameModule.GetParentWindow());


		if (NewLevelInstanceDialog->ClickedOk())
		{
			FNewLevelInstanceParams CreationParams(NewLevelInstanceDialog->GetCreationParams());

			FNewLevelDialogModule& NewLevelDialogModule = FModuleManager::LoadModuleChecked<FNewLevelDialogModule>("NewLevelDialog");
			FString TemplateMapPackage;
			bool bOutIsPartitionedWorld = false;
			const bool bShowPartitionedTemplates = false;
			if (!GetMutableDefault<ULevelInstanceEditorSettings>()->TemplateMapInfos.Num() || NewLevelDialogModule.CreateAndShowTemplateDialog(MainFrameModule.GetParentWindow(), LOCTEXT("LevelInstanceTemplateDialog", "Choose Level Instance Template..."), GetMutableDefault<ULevelInstanceEditorSettings>()->TemplateMapInfos, TemplateMapPackage, bShowPartitionedTemplates, bOutIsPartitionedWorld))
			{
				UPackage* TemplatePackage = !TemplateMapPackage.IsEmpty() ? LoadPackage(nullptr, *TemplateMapPackage, LOAD_None) : nullptr;
				
				CreationParams.TemplateWorld = TemplatePackage ? UWorld::FindWorldInPackage(TemplatePackage) : nullptr;

				if (!LevelInstanceSubsystem->CreateLevelInstanceFrom(ActorsToMove, CreationParams))
				{
					FText FailedTitle = LOCTEXT("CreateFromSelectionFailTitle", "Create from selection failed");
					FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("CreateFromSelectionFailMsg", "Failed to create from selection. Check log for details."), &FailedTitle);
				}
			}
		}
	}

	void CreateCreateMenu(UToolMenu* ToolMenu)
	{
		if (ULevelInstanceSubsystem* LevelInstanceSubsystem = GEditor->GetEditorWorldContext().World()->GetSubsystem<ULevelInstanceSubsystem>())
		{
			if (GEditor->GetSelectedActorCount() > 0)
			{
				FToolMenuSection& Section = ToolMenu->AddSection("ActorSelectionSectionName", LOCTEXT("ActorSelectionSectionLabel", "Actor Selection"));

				if (GetDefault<UEditorExperimentalSettings>()->bLevelInstance)
				{
					Section.AddMenuEntry(
						NAME_None,
						FText::Format(LOCTEXT("CreateFromSelectionLabel", "Create {0}..."), StaticEnum<ELevelInstanceCreationType>()->GetDisplayNameTextByValue((int64)ELevelInstanceCreationType::LevelInstance)),
						TAttribute<FText>(),
						FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.LevelInstance"),
						FExecuteAction::CreateStatic(&LevelInstanceMenuUtils::CreateLevelInstanceFromSelection, LevelInstanceSubsystem, ELevelInstanceCreationType::LevelInstance));
				}

				if (GetDefault<UEditorExperimentalSettings>()->bPackedLevelActor)
				{
					Section.AddMenuEntry(
						NAME_None,
						FText::Format(LOCTEXT("CreateFromSelectionLabel", "Create {0}..."), StaticEnum<ELevelInstanceCreationType>()->GetDisplayNameTextByValue((int64)ELevelInstanceCreationType::PackedLevelActor)),
						TAttribute<FText>(),
						FSlateIcon(FAppStyle::GetAppStyleSetName(), "ClassIcon.PackedLevelActor"),
						FExecuteAction::CreateStatic(&LevelInstanceMenuUtils::CreateLevelInstanceFromSelection, LevelInstanceSubsystem, ELevelInstanceCreationType::PackedLevelActor));
				}
			}
		}
	}
		
	void CreateBreakSubMenu(UToolMenu* Menu, ALevelInstance* ContextLevelInstance)
	{
		static int32 BreakLevels = 1;

		check(ContextLevelInstance);

		if (ULevelInstanceSubsystem* LevelInstanceSubsystem = ContextLevelInstance->GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
		{
			FToolMenuSection& Section = Menu->AddSection(NAME_None, LOCTEXT("LevelInstanceBreakSection", "Break Level Instance"));
			TSharedRef<SWidget> MenuWidget =
				SNew(SVerticalBox)

				+SVerticalBox::Slot()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot()
					[
						SNew(SNumericEntryBox<int32>)
						.MinValue(1)
						.Value_Lambda([]() { return BreakLevels; })
						.OnValueChanged_Lambda([](int32 InValue) { BreakLevels = InValue; })
						.LabelPadding(0)
						.Label()
						[
							SNumericEntryBox<int32>::BuildLabel(LOCTEXT("BreakLevelsLabel", "Levels"), FLinearColor::White, SNumericEntryBox<int32>::BlueLabelBackgroundColor)
						]
					]
				]

				+SVerticalBox::Slot()
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Center)
				.Padding(0, 5, 0, 0)
				[
					SNew(SButton)
					.HAlign(HAlign_Center)
					.ContentPadding(FEditorStyle::GetMargin("StandardDialog.ContentPadding"))
					.OnClicked_Lambda([ContextLevelInstance, LevelInstanceSubsystem]() 
						{
							const FText LevelInstanceBreakWarning = LOCTEXT("BreakingLevelInstance", "You are about to break the level instance. This action cannot be undone. Are you sure ?");
							if (FMessageDialog::Open(EAppMsgType::YesNo, LevelInstanceBreakWarning) == EAppReturnType::Yes)
							{
								LevelInstanceSubsystem->BreakLevelInstance(ContextLevelInstance, BreakLevels);
							}
							return FReply::Handled();
						})
					.Text(LOCTEXT("BreakLevelInstances_BreakLevelInstanceButton", "Break Level Instance"))
				];

			Section.AddEntry(FToolMenuEntry::InitWidget("SetBreakLevels", MenuWidget, FText::GetEmpty(), false));
		}
	}

	void CreateBreakMenu(UToolMenu* Menu, AActor* ContextActor)
	{
		check(ContextActor);

		if (ULevelInstanceSubsystem* LevelInstanceSubsystem = ContextActor->GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
		{
			ALevelInstance* ContextLevelInstance = nullptr;

			// Find the top level LevelInstance
			LevelInstanceSubsystem->ForEachLevelInstanceAncestorsAndSelf(ContextActor, [LevelInstanceSubsystem, ContextActor, &ContextLevelInstance](ALevelInstance* Ancestor)
				{
					if (Ancestor->GetLevel() == ContextActor->GetWorld()->GetCurrentLevel())
					{
						ContextLevelInstance = Ancestor;
						return false;
					}
					return true;
				});

			if (ContextLevelInstance && IsExperimentalSettingEnabled(ContextLevelInstance) && !ContextLevelInstance->IsEditing() && !LevelInstanceSubsystem->LevelInstanceHasLevelScriptBlueprint(ContextLevelInstance))
			{
				FToolMenuSection& Section = CreateLevelSection(Menu);

				Section.AddSubMenu(
					"BreakLevelInstances",
					LOCTEXT("BreakLevelInstances", "Break..."),
					TAttribute<FText>(),
					FNewToolMenuDelegate::CreateStatic(&CreateBreakSubMenu, ContextLevelInstance)
				);
			}
		}
	}

	void CreatePackedBlueprintMenu(UToolMenu* Menu, AActor* ContextActor)
	{
		if (ULevelInstanceSubsystem* LevelInstanceSubsystem = ContextActor->GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
		{
			ALevelInstance* ContextLevelInstance = nullptr;

			// Find the top level LevelInstance
			LevelInstanceSubsystem->ForEachLevelInstanceAncestorsAndSelf(ContextActor, [LevelInstanceSubsystem, ContextActor, &ContextLevelInstance](ALevelInstance* Ancestor)
			{
				if (Ancestor->GetLevel() == ContextActor->GetWorld()->GetCurrentLevel())
				{
					ContextLevelInstance = Ancestor;
					return false;
				}
				return true;
			});
						
			if (ContextLevelInstance && IsExperimentalSettingEnabled(ContextLevelInstance) && !ContextLevelInstance->IsEditing())
			{
				FToolMenuSection& Section = CreateLevelSection(Menu);
				;
				if (APackedLevelActor* PackedLevelActor = Cast<APackedLevelActor>(ContextLevelInstance))
				{
					if (TSoftObjectPtr<UBlueprint> BlueprintAsset = PackedLevelActor->BlueprintAsset)
					{
						FToolUIAction UIAction;
						UIAction.ExecuteAction.BindLambda([ContextLevelInstance, BlueprintAsset](const FToolMenuContext& MenuContext)
							{
								APackedLevelActor::CreateOrUpdateBlueprint(ContextLevelInstance->GetWorldAsset(), BlueprintAsset);
							});
						UIAction.CanExecuteAction.BindLambda([](const FToolMenuContext& MenuContext)
							{
								return GEditor->GetSelectedActorCount() > 0;
							});

						Section.AddMenuEntry(
							"UpdatePackedBlueprint",
							LOCTEXT("UpdatePackedBlueprint", "Update Packed Blueprint"),
							TAttribute<FText>(),
							TAttribute<FSlateIcon>(),
							UIAction);
					}
				}
			}
		}
	}

	class FLevelInstanceClassFilter : public IClassViewerFilter
	{
	public:
		
		virtual bool IsClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const UClass* InClass, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override
		{
			return InClass && InClass->IsChildOf(ALevelInstance::StaticClass()) && !InClass->IsChildOf(APackedLevelActor::StaticClass()) && !InClass->HasAnyClassFlags(CLASS_Deprecated);
		}

		virtual bool IsUnloadedClassAllowed(const FClassViewerInitializationOptions& InInitOptions, const TSharedRef< const IUnloadedBlueprintData > InUnloadedClassData, TSharedRef< FClassViewerFilterFuncs > InFilterFuncs) override
		{
			return InUnloadedClassData->IsChildOf(ALevelInstance::StaticClass()) && !InUnloadedClassData->IsChildOf(APackedLevelActor::StaticClass())  && !InUnloadedClassData->HasAnyClassFlags(CLASS_Deprecated);
		}
	};

	void CreateBlueprintFromWorld(UWorld* WorldAsset)
	{
		TSoftObjectPtr<UWorld> LevelInstancePtr(WorldAsset);

		int32 LastSlashIndex = 0;
		FString LongPackageName = LevelInstancePtr.GetLongPackageName();
		LongPackageName.FindLastChar('/', LastSlashIndex);
		
		FString PackagePath = LongPackageName.Mid(0, LastSlashIndex == INDEX_NONE ? MAX_int32 : LastSlashIndex);
		FString AssetName = LevelInstancePtr.GetAssetName() + "_LevelInstance";
		IAssetTools& AssetTools = FAssetToolsModule::GetModule().Get();

		UBlueprintFactory* BlueprintFactory = NewObject<UBlueprintFactory>();
		BlueprintFactory->AddToRoot();
		BlueprintFactory->OnConfigurePropertiesDelegate.BindLambda([](FClassViewerInitializationOptions* Options)
		{
			Options->bShowDefaultClasses = false;
			Options->bIsBlueprintBaseOnly = false;
			Options->InitiallySelectedClass = ALevelInstance::StaticClass();
			Options->bIsActorsOnly = true;
			Options->ClassFilters.Add(MakeShareable(new FLevelInstanceClassFilter));
		});
		ON_SCOPE_EXIT
		{
			BlueprintFactory->OnConfigurePropertiesDelegate.Unbind();
			BlueprintFactory->RemoveFromRoot();
		};

		if (UBlueprint* NewBlueprint = Cast<UBlueprint>(AssetTools.CreateAssetWithDialog(AssetName, PackagePath, UBlueprint::StaticClass(), BlueprintFactory, FName("Create LevelInstance Blueprint"))))
		{
			ALevelInstance* CDO = CastChecked<ALevelInstance>(NewBlueprint->GeneratedClass->GetDefaultObject());
			CDO->SetWorldAsset(LevelInstancePtr);
			FBlueprintEditorUtils::MarkBlueprintAsModified(NewBlueprint);
			
			FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
			TArray<UObject*> Assets;
			Assets.Add(NewBlueprint);
			ContentBrowserModule.Get().SyncBrowserToAssets(Assets);
		}		
	}

	void CreateBlueprintFromMenu(UToolMenu* Menu, UWorld* WorldAsset)
	{
		FToolMenuSection& Section = CreateLevelSection(Menu);
		FToolUIAction UIAction;
		UIAction.ExecuteAction.BindLambda([WorldAsset](const FToolMenuContext& MenuContext)
		{
			CreateBlueprintFromWorld(WorldAsset);
		});

		Section.AddMenuEntry(
			"CreateLevelInstanceBlueprint",
			LOCTEXT("CreateLevelInstanceBlueprint", "New Blueprint..."),
			TAttribute<FText>(),
			TAttribute<FSlateIcon>(),
			UIAction);
	}
};

void FLevelInstanceEditorModule::StartupModule()
{
	ExtendContextMenu();

	FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	PropertyModule.RegisterCustomClassLayout("LevelInstance", FOnGetDetailCustomizationInstance::CreateStatic(&FLevelInstanceActorDetails::MakeInstance));
	PropertyModule.RegisterCustomClassLayout("LevelInstancePivot", FOnGetDetailCustomizationInstance::CreateStatic(&FLevelInstancePivotDetails::MakeInstance));
	PropertyModule.NotifyCustomizationModuleChanged();

	// GEditor needs to be set before this module is loaded
	check(GEditor);
	GEditor->OnLevelActorDeleted().AddRaw(this, &FLevelInstanceEditorModule::OnLevelActorDeleted);
	
	EditorLevelUtils::CanMoveActorToLevelDelegate.AddRaw(this, &FLevelInstanceEditorModule::CanMoveActorToLevel);

	FMessageLogModule& MessageLogModule = FModuleManager::LoadModuleChecked<FMessageLogModule>("MessageLog");
	FMessageLogInitializationOptions InitOptions;
	InitOptions.bShowFilters = true;
	InitOptions.bShowPages = false;
	InitOptions.bAllowClear = true;
	MessageLogModule.RegisterLogListing("PackedLevelActor", LOCTEXT("PackedLevelActorLog", "Packed Level Actor Log"), InitOptions);
		
	FLevelInstanceEditorModeCommands::Register();

	if (!IsRunningCommandlet())
	{
		GLevelEditorModeTools().OnEditorModeIDChanged().AddRaw(this, &FLevelInstanceEditorModule::OnEditorModeIDChanged);
	}
}

void FLevelInstanceEditorModule::ShutdownModule()
{
	if (GEditor)
	{
		GEditor->OnLevelActorDeleted().RemoveAll(this);
	}

	EditorLevelUtils::CanMoveActorToLevelDelegate.RemoveAll(this);

	if (!IsRunningCommandlet() && GLevelEditorModeToolsIsValid())
	{
		GLevelEditorModeTools().OnEditorModeIDChanged().RemoveAll(this);
	}
}

void FLevelInstanceEditorModule::OnEditorModeIDChanged(const FEditorModeID& InModeID, bool bIsEnteringMode)
{
	if (InModeID == ULevelInstanceEditorMode::EM_LevelInstanceEditorModeId && !bIsEnteringMode)
	{
		ExitEditorModeEvent.Broadcast();
	}
}

void FLevelInstanceEditorModule::BroadcastTryExitEditorMode() 
{
	TryExitEditorModeEvent.Broadcast();
}

void FLevelInstanceEditorModule::ActivateEditorMode()
{
	if (!GLevelEditorModeTools().IsModeActive(ULevelInstanceEditorMode::EM_LevelInstanceEditorModeId))
	{
		GLevelEditorModeTools().ActivateMode(ULevelInstanceEditorMode::EM_LevelInstanceEditorModeId);
	}
}
void FLevelInstanceEditorModule::DeactivateEditorMode()
{
	if (GLevelEditorModeTools().IsModeActive(ULevelInstanceEditorMode::EM_LevelInstanceEditorModeId))
	{
		GLevelEditorModeTools().DeactivateMode(ULevelInstanceEditorMode::EM_LevelInstanceEditorModeId);
	}
}

void FLevelInstanceEditorModule::OnLevelActorDeleted(AActor* Actor)
{
	if (ULevelInstanceSubsystem* LevelInstanceSubsystem = Actor->GetWorld()->GetSubsystem<ULevelInstanceSubsystem>())
	{
		LevelInstanceSubsystem->OnActorDeleted(Actor);
	}
}

void FLevelInstanceEditorModule::CanMoveActorToLevel(const AActor* ActorToMove, const ULevel* DestLevel, bool& bOutCanMove)
{
	if (UWorld* World = ActorToMove->GetWorld())
	{
		if (ULevelInstanceSubsystem* LevelInstanceSubsystem = World->GetSubsystem<ULevelInstanceSubsystem>())
		{
			if (!LevelInstanceSubsystem->CanMoveActorToLevel(ActorToMove))
			{
				bOutCanMove = false;
				return;
			}
		}
	}
}

void FLevelInstanceEditorModule::ExtendContextMenu()
{
	if (UToolMenu* BuildMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Build"))
	{
		FToolMenuSection& Section = BuildMenu->AddSection("LevelEditorLevelInstance", LOCTEXT("PackedLevelActorsHeading", "Packed Level Actor"));
		FUIAction PackAction(
			FExecuteAction::CreateLambda([]() 
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (ULevelInstanceSubsystem* LevelInstanceSubsystem = World->GetSubsystem<ULevelInstanceSubsystem>())
				{
					LevelInstanceSubsystem->PackAllLoadedActors();
				}
			}), 
			FCanExecuteAction::CreateLambda([]()
			{
				UWorld* World = GEditor->GetEditorWorldContext().World();
				if (ULevelInstanceSubsystem* LevelInstanceSubsystem = World->GetSubsystem<ULevelInstanceSubsystem>())
				{
					return LevelInstanceSubsystem->CanPackAllLoadedActors();
				}
				return false;
			}),
			FIsActionChecked(),
			FIsActionButtonVisible::CreateLambda([]() 
			{
				return GetDefault<UEditorExperimentalSettings>()->bPackedLevelActor;
			}));

		FToolMenuEntry& Entry = Section.AddMenuEntry(NAME_None, LOCTEXT("PackLevelActorsTitle", "Pack Level Actors"),
			LOCTEXT("PackLevelActorsTooltip", "Update packed level actor blueprints"), FSlateIcon(), PackAction, EUserInterfaceActionType::Button);
	}

	auto AddDynamicSection = [](UToolMenu* ToolMenu)
	{				
		if (ULevelEditorContextMenuContext* LevelEditorMenuContext = ToolMenu->Context.FindContext<ULevelEditorContextMenuContext>())
		{
			// Use the actor under the cursor if available (e.g. right-click menu).
			// Otherwise use the first selected actor if there's one (e.g. Actor pulldown menu or outliner).
			AActor* ContextActor = LevelEditorMenuContext->HitProxyActor;
			if (!ContextActor && GEditor->GetSelectedActorCount() != 0)
			{
				ContextActor = Cast<AActor>(GEditor->GetSelectedActors()->GetSelectedObject(0));
			}

			if (ContextActor)
			{
				LevelInstanceMenuUtils::CreateEditMenu(ToolMenu, ContextActor);
				LevelInstanceMenuUtils::CreateCommitDiscardMenu(ToolMenu, ContextActor);
				LevelInstanceMenuUtils::CreateBreakMenu(ToolMenu, ContextActor);
				LevelInstanceMenuUtils::CreatePackedBlueprintMenu(ToolMenu, ContextActor);
				LevelInstanceMenuUtils::CreateSetCurrentMenu(ToolMenu, ContextActor);
			}

			LevelInstanceMenuUtils::CreateMoveSelectionToMenu(ToolMenu);
		}

		LevelInstanceMenuUtils::CreateCreateMenu(ToolMenu);
	};

	if (UToolMenu* ToolMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.ActorContextMenu.LevelSubMenu"))
	{
		ToolMenu->AddDynamicSection("LevelInstanceEditorModuleDynamicSection", FNewToolMenuDelegate::CreateLambda(AddDynamicSection));
	}

	if (UToolMenu* ToolMenu = UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorSceneOutliner.ContextMenu.LevelSubMenu"))
	{
		ToolMenu->AddDynamicSection("LevelInstanceEditorModuleDynamicSection", FNewToolMenuDelegate::CreateLambda(AddDynamicSection));
	}
		
	if (UToolMenu* WorldAssetMenu = UToolMenus::Get()->ExtendMenu("ContentBrowser.AssetContextMenu.World"))
	{
		FToolMenuSection& Section = WorldAssetMenu->AddDynamicSection("ActorLevelInstance", FNewToolMenuDelegate::CreateLambda([this](UToolMenu* ToolMenu)
		{
			if(!GetDefault<UEditorExperimentalSettings>()->bLevelInstance)
			{
				return;
			}

			if (ToolMenu)
			{
				if (UContentBrowserAssetContextMenuContext* AssetMenuContext = ToolMenu->Context.FindContext<UContentBrowserAssetContextMenuContext>())
				{
					if (AssetMenuContext->SelectedObjects.Num() != 1)
					{
						return;
					}
					// World is already loaded by the AssetContextMenu code
					if (UWorld* WorldAsset = Cast<UWorld>(AssetMenuContext->SelectedObjects[0].Get()))
					{
						LevelInstanceMenuUtils::CreateBlueprintFromMenu(ToolMenu, WorldAsset);
					}
				}
			}
		}), FToolMenuInsert(NAME_None, EToolMenuInsertType::Default));
	}
	
}

#undef LOCTEXT_NAMESPACE

