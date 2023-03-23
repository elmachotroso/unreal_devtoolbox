// Copyright Epic Games, Inc. All Rights Reserved.

#include "AssetTools.h"
#include "Factories/Factory.h"
#include "Misc/MessageDialog.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopedSlowTask.h"
#include "UObject/GCObjectScopeGuard.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Exporters/Exporter.h"
#include "Editor/EditorEngine.h"
#include "SourceControlOperations.h"
#include "ISourceControlModule.h"
#include "SourceControlHelpers.h"
#include "Editor/UnrealEdEngine.h"
#include "Settings/EditorLoadingSavingSettings.h"
#include "ThumbnailRendering/ThumbnailManager.h"
#include "Editor.h"
#include "EditorDirectories.h"
#include "FileHelpers.h"
#include "UnrealEdGlobals.h"
#include "AssetToolsLog.h"
#include "AssetToolsModule.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ToolMenus.h"
#include "IClassTypeActions.h"
#include "AssetTypeActions/AssetTypeActions_Actor.h"
#include "AssetTypeActions/AssetTypeActions_ActorFolder.h"
#include "AssetTypeActions/AssetTypeActions_Blueprint.h"
#include "AssetTypeActions/AssetTypeActions_BlueprintGeneratedClass.h"
#include "AssetTypeActions/AssetTypeActions_Curve.h"
#include "AssetTypeActions/AssetTypeActions_MaterialInterface.h"
#include "AssetTypeActions/AssetTypeActions_SkeletalMesh.h"
#include "AssetTypeActions/AssetTypeActions_FbxSceneImportData.h"
#include "AssetTypeActions/AssetTypeActions_Texture.h"
#include "AssetTypeActions/AssetTypeActions_TextureRenderTarget.h"
#include "AssetTypeActions/AssetTypeActions_VectorField.h"
#include "AssetTypeActions/AssetTypeActions_AnimationAsset.h"
#include "AssetTypeActions/AssetTypeActions_AnimBlueprint.h"
#include "AssetTypeActions/AssetTypeActions_AnimBlueprintGeneratedClass.h"
#include "AssetTypeActions/AssetTypeActions_AnimBoneCompressionSettings.h"
#include "AssetTypeActions/AssetTypeActions_AnimComposite.h"
#include "AssetTypeActions/AssetTypeActions_AnimStreamable.h"
#include "AssetTypeActions/AssetTypeActions_AnimCurveCompressionSettings.h"
#include "AssetTypeActions/AssetTypeActions_AnimMontage.h"
#include "AssetTypeActions/AssetTypeActions_AnimSequence.h"
#include "AssetTypeActions/AssetTypeActions_BlendSpace.h"
#include "AssetTypeActions/AssetTypeActions_AimOffset.h"
#include "AssetTypeActions/AssetTypeActions_BlendSpace1D.h"
#include "AssetTypeActions/AssetTypeActions_AimOffset1D.h"
#include "AssetTypeActions/AssetTypeActions_CameraAnim.h"
#include "AssetTypeActions/AssetTypeActions_TextureRenderTarget2D.h"
#include "AssetTypeActions/AssetTypeActions_CanvasRenderTarget2D.h"
#include "AssetTypeActions/AssetTypeActions_CurveFloat.h"
#include "AssetTypeActions/AssetTypeActions_CurveTable.h"
#include "AssetTypeActions/AssetTypeActions_CompositeCurveTable.h"
#include "AssetTypeActions/AssetTypeActions_CurveVector.h"
#include "AssetTypeActions/AssetTypeActions_CurveLinearColor.h"
#include "AssetTypeActions/AssetTypeActions_CurveLinearColorAtlas.h"
#include "AssetTypeActions/AssetTypeActions_DataAsset.h"
#include "AssetTypeActions/AssetTypeActions_DataTable.h"
#include "AssetTypeActions/AssetTypeActions_CompositeDataTable.h"
#include "AssetTypeActions/AssetTypeActions_Enum.h"
#include "AssetTypeActions/AssetTypeActions_Class.h"
#include "AssetTypeActions/AssetTypeActions_Struct.h"
#include "AssetTypeActions/AssetTypeActions_Font.h"
#include "AssetTypeActions/AssetTypeActions_FontFace.h"
#include "AssetTypeActions/AssetTypeActions_ForceFeedbackAttenuation.h"
#include "AssetTypeActions/AssetTypeActions_ForceFeedbackEffect.h"
#include "AssetTypeActions/AssetTypeActions_HapticFeedback.h"
#include "AssetTypeActions/AssetTypeActions_HLODProxy.h"
#include "AssetTypeActions/AssetTypeActions_SubsurfaceProfile.h"
#include "AssetTypeActions/AssetTypeActions_ActorFoliageSettings.h"
#include "AssetTypeActions/AssetTypeActions_InstancedFoliageSettings.h"
#include "AssetTypeActions/AssetTypeActions_InterpData.h"
#include "AssetTypeActions/AssetTypeActions_LandscapeLayer.h"
#include "AssetTypeActions/AssetTypeActions_LandscapeGrassType.h"
#include "AssetTypeActions/AssetTypeActions_LightWeightInstance.h"
#include "AssetTypeActions/AssetTypeActions_Material.h"
#include "AssetTypeActions/AssetTypeActions_MaterialFunction.h"
#include "AssetTypeActions/AssetTypeActions_MaterialFunctionInstance.h"
#include "AssetTypeActions/AssetTypeActions_MaterialInstanceConstant.h"
#include "AssetTypeActions/AssetTypeActions_MaterialInstanceDynamic.h"
#include "AssetTypeActions/AssetTypeActions_MaterialParameterCollection.h"
#include "AssetTypeActions/AssetTypeActions_MirrorDataTable.h"
#include "AssetTypeActions/AssetTypeActions_ObjectLibrary.h"
#include "AssetTypeActions/AssetTypeActions_ParticleSystem.h"
#include "AssetTypeActions/AssetTypeActions_PhysicalMaterial.h"
#include "AssetTypeActions/AssetTypeActions_PhysicalMaterialMask.h"
#include "AssetTypeActions/AssetTypeActions_PhysicsAsset.h"
#include "AssetTypeActions/AssetTypeActions_PoseAsset.h"
#include "AssetTypeActions/AssetTypeActions_PreviewMeshCollection.h"
#include "AssetTypeActions/AssetTypeActions_ProceduralFoliageSpawner.h"
#include "AssetTypeActions/AssetTypeActions_Redirector.h"
#include "AssetTypeActions/AssetTypeActions_Rig.h"
#include "AssetTypeActions/AssetTypeActions_Skeleton.h"
#include "AssetTypeActions/AssetTypeActions_SlateBrush.h"
#include "AssetTypeActions/AssetTypeActions_SlateWidgetStyle.h"
#include "AssetTypeActions/AssetTypeActions_StaticMesh.h"
#include "AssetTypeActions/AssetTypeActions_SubUVAnimation.h"
#include "AssetTypeActions/AssetTypeActions_Texture2D.h"
#include "AssetTypeActions/AssetTypeActions_Texture2DArray.h"
#include "AssetTypeActions/AssetTypeActions_TextureCube.h"
#include "AssetTypeActions/AssetTypeActions_TextureCubeArray.h"
#include "AssetTypeActions/AssetTypeActions_VolumeTexture.h"
#include "AssetTypeActions/AssetTypeActions_TextureRenderTarget2DArray.h"
#include "AssetTypeActions/AssetTypeActions_TextureRenderTargetCube.h"
#include "AssetTypeActions/AssetTypeActions_TextureRenderTargetVolume.h"
#include "AssetTypeActions/AssetTypeActions_TextureLightProfile.h"
#include "AssetTypeActions/AssetTypeActions_TouchInterface.h"
#include "AssetTypeActions/AssetTypeActions_VectorFieldAnimated.h"
#include "AssetTypeActions/AssetTypeActions_VectorFieldStatic.h"
#include "AssetTypeActions/AssetTypeActions_World.h"
#include "SDiscoveringAssetsDialog.h"
#include "AssetFixUpRedirectors.h"
#include "ObjectTools.h"
#include "PackageTools.h"
#include "AssetRegistryModule.h"
#include "DesktopPlatformModule.h"
#include "IContentBrowserSingleton.h"
#include "ContentBrowserModule.h"
#include "SPackageReportDialog.h"
#include "EngineAnalytics.h"
#include "AnalyticsEventAttribute.h"
#include "Interfaces/IAnalyticsProvider.h"
#include "Logging/MessageLog.h"
#include "UnrealExporter.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Framework/Application/SlateApplication.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "AutomatedAssetImportData.h"
#include "AssetImportTask.h"
#include "Dialogs/DlgPickPath.h"
#include "Misc/FeedbackContext.h"
#include "BusyCursor.h"
#include "AssetExportTask.h"
#include "Containers/UnrealString.h"
#include "Serialization/ArchiveReplaceObjectRef.h"
#include "AdvancedCopyCustomization.h"
#include "SAdvancedCopyReportDialog.h"
#include "AssetToolsSettings.h"
#include "AssetVtConversion.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/NamePermissionList.h"
#include "InterchangeManager.h"
#include "InterchangeProjectSettings.h"
#include "Engine/World.h"
#include "Engine/Level.h"
#include "UObject/SavePackage.h"
#include "Dialogs/Dialogs.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Layout/SSpacer.h"

#if WITH_EDITOR
#include "Subsystems/AssetEditorSubsystem.h"
#include "Settings/EditorExperimentalSettings.h"
#endif

#define LOCTEXT_NAMESPACE "AssetTools"

TScriptInterface<IAssetTools> UAssetToolsHelpers::GetAssetTools()
{
	return &UAssetToolsImpl::Get();
}

/** UInterface constructor */
UAssetTools::UAssetTools(const class FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

UAssetToolsImpl::UAssetToolsImpl(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, AssetRenameManager(MakeShareable(new FAssetRenameManager))
	, AssetFixUpRedirectors(MakeShareable(new FAssetFixUpRedirectors))
	, NextUserCategoryBit(EAssetTypeCategories::FirstUser)
	, AssetClassPermissionList(MakeShared<FNamePermissionList>())
	, FolderPermissionList(MakeShared<FPathPermissionList>())
	, WritableFolderPermissionList(MakeShared<FPathPermissionList>())
{
	TArray<FString> SupportedTypesArray;
	GConfig->GetArray(TEXT("AssetTools"), TEXT("SupportedAssetTypes"), SupportedTypesArray, GEditorIni);
	for (const FString& Type : SupportedTypesArray)
	{
		AssetClassPermissionList->AddAllowListItem("AssetToolsConfigFile", *Type);
	}
	AssetClassPermissionList->OnFilterChanged().AddUObject(this, &UAssetToolsImpl::AssetClassPermissionListChanged);

	TArray<FString> BlacklistedViewPath;
	GConfig->GetArray(TEXT("AssetTools"), TEXT("BlacklistAssetPaths"), BlacklistedViewPath, GEditorIni);
	for (const FString& Path : BlacklistedViewPath)
	{
		FolderPermissionList->AddDenyListItem("AssetToolsConfigFile", Path);
	}

	GConfig->GetArray(TEXT("AssetTools"), TEXT("BlacklistContentSubPaths"), SubContentBlacklistPaths, GEditorIni);
	TArray<FString> ContentRoots;
	FPackageName::QueryRootContentPaths(ContentRoots);
	for (const FString& ContentRoot : ContentRoots)
	{
		AddSubContentBlacklist(ContentRoot);
	}
	FPackageName::OnContentPathMounted().AddUObject(this, &UAssetToolsImpl::OnContentPathMounted);

	// Register the built-in advanced categories
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_0"), FAdvancedAssetCategory(EAssetTypeCategories::Animation, LOCTEXT("AnimationAssetCategory", "Animation")));
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_1"), FAdvancedAssetCategory(EAssetTypeCategories::Blueprint, LOCTEXT("BlueprintAssetCategory", "Blueprints")));
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_2"), FAdvancedAssetCategory(EAssetTypeCategories::Materials, LOCTEXT("MaterialAssetCategory", "Materials")));
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_3"), FAdvancedAssetCategory(EAssetTypeCategories::Sounds, LOCTEXT("SoundAssetCategory", "Sounds")));
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_4"), FAdvancedAssetCategory(EAssetTypeCategories::Physics, LOCTEXT("PhysicsAssetCategory", "Physics")));
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_5"), FAdvancedAssetCategory(EAssetTypeCategories::UI, LOCTEXT("UserInterfaceAssetCategory", "User Interface")));
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_6"), FAdvancedAssetCategory(EAssetTypeCategories::Misc, LOCTEXT("MiscellaneousAssetCategory", "Miscellaneous")));
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_7"), FAdvancedAssetCategory(EAssetTypeCategories::Gameplay, LOCTEXT("GameplayAssetCategory", "Gameplay")));
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_8"), FAdvancedAssetCategory(EAssetTypeCategories::Media, LOCTEXT("MediaAssetCategory", "Media")));
	AllocatedCategoryBits.Add(TEXT("_BuiltIn_9"), FAdvancedAssetCategory(EAssetTypeCategories::Textures, LOCTEXT("TextureAssetCategory", "Textures")));

	EAssetTypeCategories::Type BlendablesCategoryBit = RegisterAdvancedAssetCategory(FName(TEXT("Blendables")), LOCTEXT("BlendablesAssetCategory", "Blendables"));
	EAssetTypeCategories::Type FoliageCategoryBit = RegisterAdvancedAssetCategory(FName(TEXT("Foliage")), LOCTEXT("FoliageAssetCategory", "Foliage"));

	// Register the built-in asset type actions
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Actor));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_ActorFolder));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AnimationAsset));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AnimBlueprint));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AnimBlueprintGeneratedClass));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AnimBoneCompressionSettings));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AnimComposite));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AnimStreamable));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AnimCurveCompressionSettings));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AnimMontage));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AnimSequence));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AimOffset));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_AimOffset1D));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_BlendSpace));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_PoseAsset));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_BlendSpace1D));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Blueprint));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_BlueprintGeneratedClass));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_CameraAnim));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_CanvasRenderTarget2D));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Curve));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_CurveFloat));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_CurveTable));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_CompositeCurveTable));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_CurveVector));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_CurveLinearColor));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_CurveLinearColorAtlas));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_DataAsset));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_DataTable));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_CompositeDataTable));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Enum));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Class));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Struct));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_SceneImportData));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Font));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_FontFace));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_ForceFeedbackAttenuation));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_ForceFeedbackEffect));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_HLODProxy));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_SubsurfaceProfile));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_InstancedFoliageSettings(FoliageCategoryBit)));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_ActorFoliageSettings(FoliageCategoryBit)));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_InterpData));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_LandscapeLayer));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_LandscapeGrassType(FoliageCategoryBit)));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_LightWeightInstance));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Material(BlendablesCategoryBit)));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialFunction));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialFunctionLayer));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialFunctionLayerInstance));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialFunctionLayerBlend));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialFunctionLayerBlendInstance));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialFunctionInstance));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialInstanceConstant(BlendablesCategoryBit)));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialInstanceDynamic));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialInterface));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MaterialParameterCollection));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_MirrorDataTable));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_ObjectLibrary));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_ParticleSystem));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_SubUVAnimation));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_PhysicalMaterial));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_PhysicalMaterialMask));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_PhysicsAsset));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_PreviewMeshCollection));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_ProceduralFoliageSpawner(FoliageCategoryBit)));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Redirector));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Rig));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_SkeletalMesh));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Skeleton));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_SlateBrush));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_SlateWidgetStyle));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_StaticMesh));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_SubUVAnimation));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Texture));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Texture2D));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_TextureCube));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_Texture2DArray));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_TextureCubeArray));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_VolumeTexture));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_TextureRenderTarget));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_TextureRenderTarget2D));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_TextureRenderTarget2DArray));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_TextureRenderTargetCube));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_TextureRenderTargetVolume));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_TextureLightProfile));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_TouchInterface));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_VectorField));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_VectorFieldAnimated));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_VectorFieldStatic));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_World));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_HapticFeedbackEffectBuffer));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_HapticFeedbackEffectCurve));
	RegisterAssetTypeActions(MakeShareable(new FAssetTypeActions_HapticFeedbackEffectSoundWave));

	// Note: Please don't add any more actions here!  They belong in an editor-only module that is more tightly
	// coupled to your new system, and you should not create a dependency on your new system from AssetTools.
}

void UAssetToolsImpl::RegisterAssetTypeActions(const TSharedRef<IAssetTypeActions>& NewActions)
{
	bool bSupported = false;
	if (const UClass* SupportedClass = NewActions->GetSupportedClass())
	{
		bSupported = AssetClassPermissionList->PassesFilter(SupportedClass->GetFName());
	}
	else
	{
		bSupported = !NewActions->GetFilterName().IsNone();
	}

	NewActions->SetSupported(bSupported);

	AssetTypeActionsList.Add(NewActions);
}

void UAssetToolsImpl::UnregisterAssetTypeActions(const TSharedRef<IAssetTypeActions>& ActionsToRemove)
{
	AssetTypeActionsList.Remove(ActionsToRemove);
}

void UAssetToolsImpl::GetAssetTypeActionsList( TArray<TWeakPtr<IAssetTypeActions>>& OutAssetTypeActionsList ) const
{
	for (auto ActionsIt = AssetTypeActionsList.CreateConstIterator(); ActionsIt; ++ActionsIt)
	{
		OutAssetTypeActionsList.Add(*ActionsIt);
	}
}

TWeakPtr<IAssetTypeActions> UAssetToolsImpl::GetAssetTypeActionsForClass(const UClass* Class) const
{
	TSharedPtr<IAssetTypeActions> MostDerivedAssetTypeActions;

	for (int32 TypeActionsIdx = 0; TypeActionsIdx < AssetTypeActionsList.Num(); ++TypeActionsIdx)
	{
		TSharedRef<IAssetTypeActions> TypeActions = AssetTypeActionsList[TypeActionsIdx];
		UClass* SupportedClass = TypeActions->GetSupportedClass();

		if ( Class->IsChildOf(SupportedClass) )
		{
			if ( !MostDerivedAssetTypeActions.IsValid() || SupportedClass->IsChildOf( MostDerivedAssetTypeActions->GetSupportedClass() ) )
			{
				MostDerivedAssetTypeActions = TypeActions;
			}
		}
	}

	return MostDerivedAssetTypeActions;
}

TArray<TWeakPtr<IAssetTypeActions>> UAssetToolsImpl::GetAssetTypeActionsListForClass(const UClass* Class) const
{
	TArray<TWeakPtr<IAssetTypeActions>> ResultAssetTypeActionsList;

	for (int32 TypeActionsIdx = 0; TypeActionsIdx < AssetTypeActionsList.Num(); ++TypeActionsIdx)
	{
		TSharedRef<IAssetTypeActions> TypeActions = AssetTypeActionsList[TypeActionsIdx];
		UClass* SupportedClass = TypeActions->GetSupportedClass();

		if (Class->IsChildOf(SupportedClass))
		{
			ResultAssetTypeActionsList.Add(TypeActions);
		}
	}

	return ResultAssetTypeActionsList;
}

EAssetTypeCategories::Type UAssetToolsImpl::RegisterAdvancedAssetCategory(FName CategoryKey, FText CategoryDisplayName)
{
	EAssetTypeCategories::Type Result = FindAdvancedAssetCategory(CategoryKey);
	if (Result == EAssetTypeCategories::Misc)
	{
		if (NextUserCategoryBit != 0)
		{
			// Register the category
			Result = (EAssetTypeCategories::Type)NextUserCategoryBit;
			AllocatedCategoryBits.Add(CategoryKey, FAdvancedAssetCategory(Result, CategoryDisplayName));

			// Advance to the next bit, or store that we're out
			if (NextUserCategoryBit == EAssetTypeCategories::LastUser)
			{
				NextUserCategoryBit = 0;
			}
			else
			{
				NextUserCategoryBit = NextUserCategoryBit << 1;
			}
		}
		else
		{
			UE_LOG(LogAssetTools, Warning, TEXT("RegisterAssetTypeCategory(\"%s\", \"%s\") failed as all user bits have been exhausted (placing into the Misc category instead)"), *CategoryKey.ToString(), *CategoryDisplayName.ToString());
		}
	}

	return Result;
}

EAssetTypeCategories::Type UAssetToolsImpl::FindAdvancedAssetCategory(FName CategoryKey) const
{
	if (const FAdvancedAssetCategory* ExistingCategory = AllocatedCategoryBits.Find(CategoryKey))
	{
		return ExistingCategory->CategoryType;
	}
	else
	{
		return EAssetTypeCategories::Misc;
	}
}

void UAssetToolsImpl::GetAllAdvancedAssetCategories(TArray<FAdvancedAssetCategory>& OutCategoryList) const
{
	AllocatedCategoryBits.GenerateValueArray(OutCategoryList);
}

void UAssetToolsImpl::RegisterClassTypeActions(const TSharedRef<IClassTypeActions>& NewActions)
{
	ClassTypeActionsList.Add(NewActions);
}

void UAssetToolsImpl::UnregisterClassTypeActions(const TSharedRef<IClassTypeActions>& ActionsToRemove)
{
	ClassTypeActionsList.Remove(ActionsToRemove);
}

void UAssetToolsImpl::GetClassTypeActionsList( TArray<TWeakPtr<IClassTypeActions>>& OutClassTypeActionsList ) const
{
	for (auto ActionsIt = ClassTypeActionsList.CreateConstIterator(); ActionsIt; ++ActionsIt)
	{
		OutClassTypeActionsList.Add(*ActionsIt);
	}
}

TWeakPtr<IClassTypeActions> UAssetToolsImpl::GetClassTypeActionsForClass( UClass* Class ) const
{
	TSharedPtr<IClassTypeActions> MostDerivedClassTypeActions;

	for (int32 TypeActionsIdx = 0; TypeActionsIdx < ClassTypeActionsList.Num(); ++TypeActionsIdx)
	{
		TSharedRef<IClassTypeActions> TypeActions = ClassTypeActionsList[TypeActionsIdx];
		UClass* SupportedClass = TypeActions->GetSupportedClass();

		if ( Class->IsChildOf(SupportedClass) )
		{
			if ( !MostDerivedClassTypeActions.IsValid() || SupportedClass->IsChildOf( MostDerivedClassTypeActions->GetSupportedClass() ) )
			{
				MostDerivedClassTypeActions = TypeActions;
			}
		}
	}

	return MostDerivedClassTypeActions;
}

UObject* UAssetToolsImpl::CreateAsset(const FString& AssetName, const FString& PackagePath, UClass* AssetClass, UFactory* Factory, FName CallingContext)
{
	FGCObjectScopeGuard DontGCFactory(Factory);

	// Verify the factory class
	if ( !ensure(AssetClass || Factory) )
	{
		FMessageDialog::Open( EAppMsgType::Ok, LOCTEXT("MustSupplyClassOrFactory", "The new asset wasn't created due to a problem finding the appropriate factory or class for the new asset.") );
		return nullptr;
	}

	if ( AssetClass && Factory && !ensure(AssetClass->IsChildOf(Factory->GetSupportedClass())) )
	{
		FMessageDialog::Open( EAppMsgType::Ok, LOCTEXT("InvalidFactory", "The new asset wasn't created because the supplied factory does not support the supplied class.") );
		return nullptr;
	}

	const FString PackageName = UPackageTools::SanitizePackageName(PackagePath + TEXT("/") + AssetName);

	// Make sure we can create the asset without conflicts
	if ( !CanCreateAsset(AssetName, PackageName, LOCTEXT("CreateANewObject", "Create a new object")) )
	{
		return nullptr;
	}

	UClass* ClassToUse = AssetClass ? AssetClass : (Factory ? Factory->GetSupportedClass() : nullptr);

	UPackage* Pkg = CreatePackage(*PackageName);
	UObject* NewObj = nullptr;
	EObjectFlags Flags = RF_Public|RF_Standalone|RF_Transactional;
	if ( Factory )
	{
		NewObj = Factory->FactoryCreateNew(ClassToUse, Pkg, FName( *AssetName ), Flags, nullptr, GWarn, CallingContext);
	}
	else if ( AssetClass )
	{
		NewObj = NewObject<UObject>(Pkg, ClassToUse, FName(*AssetName), Flags);
	}

	if( NewObj )
	{
		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(NewObj);

		// analytics create record
		UAssetToolsImpl::OnNewCreateRecord(AssetClass, false);

		// Mark the package dirty...
		Pkg->MarkPackageDirty();
	}

	return NewObj;
}

UObject* UAssetToolsImpl::CreateAssetWithDialog(UClass* AssetClass, UFactory* Factory, FName CallingContext)
{
	if (Factory != nullptr)
	{
		// Determine the starting path. Try to use the most recently used directory
		FString AssetPath;

		const FString DefaultFilesystemDirectory = FEditorDirectories::Get().GetLastDirectory(ELastDirectory::NEW_ASSET);
		if (DefaultFilesystemDirectory.IsEmpty() || !FPackageName::TryConvertFilenameToLongPackageName(DefaultFilesystemDirectory, AssetPath))
		{
			// No saved path, just use the game content root
			AssetPath = TEXT("/Game");
		}

		FString PackageName;
		FString AssetName;
		CreateUniqueAssetName(AssetPath / Factory->GetDefaultNewAssetName(), TEXT(""), PackageName, AssetName);

		return CreateAssetWithDialog(AssetName, AssetPath, AssetClass, Factory, CallingContext);
	}

	return nullptr;
}


UObject* UAssetToolsImpl::CreateAssetWithDialog(const FString& AssetName, const FString& PackagePath, UClass* AssetClass, UFactory* Factory, FName CallingContext, const bool bCallConfigureProperties)
{
	FGCObjectScopeGuard DontGCFactory(Factory);
	if(Factory)
	{
		FSaveAssetDialogConfig SaveAssetDialogConfig;
		SaveAssetDialogConfig.DialogTitleOverride = LOCTEXT("SaveAssetDialogTitle", "Save Asset As");
		SaveAssetDialogConfig.DefaultPath = PackagePath;
		SaveAssetDialogConfig.DefaultAssetName = AssetName;
		SaveAssetDialogConfig.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::AllowButWarn;

		FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
		FString SaveObjectPath = ContentBrowserModule.Get().CreateModalSaveAssetDialog(SaveAssetDialogConfig);
		if (!SaveObjectPath.IsEmpty())
		{
			bool bCreateAsset = true;
			if (bCallConfigureProperties)
			{
				FEditorDelegates::OnConfigureNewAssetProperties.Broadcast(Factory);
				bCreateAsset = Factory->ConfigureProperties();
			}

			if (bCreateAsset)
			{
				const FString SavePackageName = FPackageName::ObjectPathToPackageName(SaveObjectPath);
				const FString SavePackagePath = FPaths::GetPath(SavePackageName);
				const FString SaveAssetName = FPaths::GetBaseFilename(SavePackageName);
				FEditorDirectories::Get().SetLastDirectory(ELastDirectory::NEW_ASSET, SavePackagePath);

				return CreateAsset(SaveAssetName, SavePackagePath, AssetClass, Factory, CallingContext);
			}
		}
	}

	return nullptr;
}

UObject* UAssetToolsImpl::DuplicateAssetWithDialog(const FString& AssetName, const FString& PackagePath, UObject* OriginalObject)
{
	return DuplicateAssetWithDialogAndTitle(AssetName, PackagePath, OriginalObject, LOCTEXT("DuplicateAssetDialogTitle", "Duplicate Asset As"));
}

UObject* UAssetToolsImpl::DuplicateAssetWithDialogAndTitle(const FString& AssetName, const FString& PackagePath, UObject* OriginalObject, FText DialogTitle)
{
	FSaveAssetDialogConfig SaveAssetDialogConfig;
	SaveAssetDialogConfig.DialogTitleOverride = DialogTitle;
	SaveAssetDialogConfig.DefaultPath = PackagePath;
	SaveAssetDialogConfig.DefaultAssetName = AssetName;
	SaveAssetDialogConfig.ExistingAssetPolicy = ESaveAssetDialogExistingAssetPolicy::AllowButWarn;

	FContentBrowserModule& ContentBrowserModule = FModuleManager::LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	FString SaveObjectPath = ContentBrowserModule.Get().CreateModalSaveAssetDialog(SaveAssetDialogConfig);
	if (!SaveObjectPath.IsEmpty())
	{
		const FString SavePackageName = FPackageName::ObjectPathToPackageName(SaveObjectPath);
		const FString SavePackagePath = FPaths::GetPath(SavePackageName);
		const FString SaveAssetName = FPaths::GetBaseFilename(SavePackageName);
		FEditorDirectories::Get().SetLastDirectory(ELastDirectory::NEW_ASSET, SavePackagePath);

		return PerformDuplicateAsset(SaveAssetName, SavePackagePath, OriginalObject, true);
	}

	return nullptr;
}

UObject* UAssetToolsImpl::DuplicateAsset(const FString& AssetName, const FString& PackagePath, UObject* OriginalObject)
{
	return PerformDuplicateAsset(AssetName, PackagePath, OriginalObject, false);
}

UObject* UAssetToolsImpl::PerformDuplicateAsset(const FString& AssetName, const FString& PackagePath, UObject* OriginalObject, bool bWithDialog)
{
	// Verify the source object
	if ( !OriginalObject )
	{
		FMessageDialog::Open( EAppMsgType::Ok, LOCTEXT("InvalidSourceObject", "The new asset wasn't created due to a problem finding the object to duplicate.") );
		return nullptr;
	}

	const FString PackageName = PackagePath / AssetName;

	// Make sure we can create the asset without conflicts
	if ( !CanCreateAsset(AssetName, PackageName, LOCTEXT("DuplicateAnObject", "Duplicate an object")) )
	{
		return nullptr;
	}

	ObjectTools::FPackageGroupName PGN;
	PGN.PackageName = PackageName;
	PGN.GroupName = TEXT("");
	PGN.ObjectName = AssetName;

	TSet<UPackage*> ObjectsUserRefusedToFullyLoad;
	bool bPromtToOverwrite = bWithDialog;
	UObject* NewObject = ObjectTools::DuplicateSingleObject(OriginalObject, PGN, ObjectsUserRefusedToFullyLoad, bPromtToOverwrite);
	if(NewObject != nullptr)
	{
		// Assets must have RF_Public and RF_Standalone
		NewObject->SetFlags(RF_Public | RF_Standalone);

		if ( ISourceControlModule::Get().IsEnabled() )
		{
			// Save package here if SCC is enabled because the user can use SCC to revert a change
			TArray<UPackage*> OutermostPackagesToSave;
			OutermostPackagesToSave.Add(NewObject->GetOutermost());

			const bool bCheckDirty = false;
			const bool bPromptToSave = false;
			FEditorFileUtils::PromptForCheckoutAndSave(OutermostPackagesToSave, bCheckDirty, bPromptToSave);

			// now attempt to branch, we can do this now as we should have a file on disk
			SourceControlHelpers::BranchPackage(NewObject->GetOutermost(), OriginalObject->GetOutermost());
		}

		// Notify the asset registry
		FAssetRegistryModule::AssetCreated(NewObject);

		// analytics create record
		UAssetToolsImpl::OnNewCreateRecord(NewObject->GetClass(), true);
	}

	return NewObject;
}

void UAssetToolsImpl::GenerateAdvancedCopyDestinations(FAdvancedCopyParams& InParams, const TArray<FName>& InPackageNamesToCopy, const UAdvancedCopyCustomization* CopyCustomization, TMap<FString, FString>& OutPackagesAndDestinations) const
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	FString DestinationFolder = InParams.GetDropLocationForAdvancedCopy();

	if (ensure(DesktopPlatform))
	{
		FPaths::NormalizeFilename(DestinationFolder);
	}
	else
	{
		// Not on a platform that supports desktop functionality
		return;
	}

	bool bGenerateRelativePaths = CopyCustomization->GetShouldGenerateRelativePaths();

	for (auto PackageNameIt = InPackageNamesToCopy.CreateConstIterator(); PackageNameIt; ++PackageNameIt)
	{
		FName PackageName = *PackageNameIt;

		const FString& PackageNameString = PackageName.ToString();
		FString SrcFilename;
		if (FPackageName::DoesPackageExist(PackageNameString, &SrcFilename))
		{
			bool bFileOKToCopy = true;

			FString DestFilename = DestinationFolder;

			FString SubFolder;
			if (SrcFilename.Split(TEXT("/Content/"), nullptr, &SubFolder))
			{
				DestFilename += *SubFolder;
			}
			else
			{
				// Couldn't find Content folder in source path
				bFileOKToCopy = false;
			}

			if (bFileOKToCopy)
			{
				FString Parent = FString();
				if (bGenerateRelativePaths)
				{
					FString RootFolder = UAdvancedCopyCustomization::StaticClass()->GetDefaultObject<UAdvancedCopyCustomization>()->GetPackageThatInitiatedCopy();
					if (RootFolder != PackageNameString)
					{
						FString BaseParent = FString();
						int32 MinLength = RootFolder.Len() < PackageNameString.Len() ? RootFolder.Len() : PackageNameString.Len();
						for (int Char = 0; Char < MinLength; Char++)
						{
							if (RootFolder[Char] == PackageNameString[Char])
							{
								BaseParent += RootFolder[Char];
							}
							else
							{
								break;
							}
						}

						// If we are in the root content folder, don't break down the folder string
						if (BaseParent == TEXT("/Game"))
						{
							Parent = BaseParent;
						}
						else
						{
							BaseParent.Split(TEXT("/"), &Parent, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
						}
					}
				}

				const FString DestinationPackageName = UAssetToolsImpl::GenerateAdvancedCopyDestinationPackageName(PackageNameString, Parent, DestinationFolder);
				OutPackagesAndDestinations.Add(PackageNameString, DestinationPackageName);
			}
		}
	}
}

FString UAssetToolsImpl::GenerateAdvancedCopyDestinationPackageName(const FString& SourcePackage, const FString& SourcePath, const FString& DestinationFolder)
{
	FString DestinationPackageName;

	const bool bIsRelativeOperation = SourcePath.Len() && DestinationFolder.Len() && SourcePackage.StartsWith(SourcePath);
	if (bIsRelativeOperation)
	{
		// Folder copy/move.

		// Collect the relative path then use it to determine the new location
		// For example, if SourcePath = /Game/MyPath and SourcePackage = /Game/MyPath/MySubPath/MyAsset
		//     /Game/MyPath/MySubPath/MyAsset -> /MySubPath/

		const int32 ShortPackageNameLen = FPackageName::GetShortName(SourcePackage).Len();
		const int32 RelativePathLen = SourcePackage.Len() - ShortPackageNameLen - SourcePath.Len();
		const FString RelativeDestPath = SourcePackage.Mid(SourcePath.Len(), RelativePathLen);

		DestinationPackageName = DestinationFolder + RelativeDestPath + FPackageName::GetShortName(SourcePackage);
	}
	else if (DestinationFolder.Len())
	{
		// Use the passed in default path
		// Normal path
		DestinationPackageName = DestinationFolder + "/" + FPackageName::GetShortName(SourcePackage);
	}
	else
	{
		// Use the path from the old package
		DestinationPackageName = SourcePackage;
	}

	return DestinationPackageName;
}

bool UAssetToolsImpl::FlattenAdvancedCopyDestinations(const TArray<TMap<FString, FString>> PackagesAndDestinations, TMap<FString, FString>& FlattenedPackagesAndDestinations) const
{
	FString CopyErrors;
	for (TMap<FString, FString> PackageAndDestinationMap : PackagesAndDestinations)
	{
		for (auto It = PackageAndDestinationMap.CreateConstIterator(); It; ++It)
		{
			const FString& PackageName = It.Key();
			const FString& DestFilename = It.Value();

			if (FlattenedPackagesAndDestinations.Contains(PackageName))
			{
				const FString* ExistingDestinationPtr = FlattenedPackagesAndDestinations.Find(PackageName);
				const FString ExistingDestination = *ExistingDestinationPtr;
				if (ExistingDestination != DestFilename)
				{
					FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("AdvancedCopy_DuplicateDestinations", "Advanced Copy failed because {0} was being duplicated in two locations, {1} and {2}."),
						FText::FromString(PackageName),
						FText::FromString(FPaths::GetPath(ExistingDestination)),
						FText::FromString(FPaths::GetPath(DestFilename))));
					return false;
				}
			}
			
			// File passed all error conditions above, add it to valid flattened list
			FlattenedPackagesAndDestinations.Add(PackageName, DestFilename);
		}
	}
	// All files passed all validation tests
	return true;
}

bool UAssetToolsImpl::ValidateFlattenedAdvancedCopyDestinations(const TMap<FString, FString>& FlattenedPackagesAndDestinations) const
{
	FString CopyErrors;

	for (auto It = FlattenedPackagesAndDestinations.CreateConstIterator(); It; ++It)
	{
		const FString& PackageName = It.Key();
		const FString& DestFilename = It.Value();

		// Check for source/destination collisions
		if (PackageName == FPaths::GetPath(DestFilename))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("AdvancedCopy_DuplicatedSource", "Advanced Copy failed because {0} was being copied over itself."), FText::FromString(PackageName)));
			return false;
		}
		else if (FlattenedPackagesAndDestinations.Contains(FPaths::GetPath(DestFilename)))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("AdvancedCopy_DestinationEqualsSource", "Advanced Copy failed because {0} was being copied over the source file {1}."),
				FText::FromString(PackageName),
				FText::FromString(FPaths::GetPath(DestFilename))));
			return false;
		}

		// Check for valid copy locations
		FString SrcFilename;
		if (!FPackageName::DoesPackageExist(PackageName, &SrcFilename))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("AdvancedCopyPackages_PackageMissing", "{0} does not exist on disk."), FText::FromString(PackageName)));
			return false;
		}
		else if (SrcFilename.Contains(FPaths::EngineContentDir()))
		{
			const FString LeafName = SrcFilename.Replace(*FPaths::EngineContentDir(), TEXT("Engine/"));
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("AdvancedCopyPackages_EngineContent", "Unable to copy Engine asset {0}. Engine assets cannot be copied using Advanced Copy."),
				FText::FromString(LeafName)));
			return false;
		}
	}
	
	// All files passed all validation tests
	return true;
}

void UAssetToolsImpl::GetAllAdvancedCopySources(FName SelectedPackage, FAdvancedCopyParams& CopyParams, TArray<FName>& OutPackageNamesToCopy, TMap<FName, FName>& DependencyMap, const UAdvancedCopyCustomization* CopyCustomization) const
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FString CurrentRoot = FString();
	TArray<FName> CurrentDependencies;
	if (!OutPackageNamesToCopy.Contains(SelectedPackage))
	{
		TArray<FAssetData> SourceAssetData;
		AssetRegistry.GetAssetsByPackageName(SelectedPackage, SourceAssetData);
		// Check if this is a folder before using the filter to exclude assets
		bool bIsFolder = SourceAssetData.Num() == 0;
		FARFilter ExclusionFilter = CopyCustomization->GetARFilter();
		AssetRegistry.UseFilterToExcludeAssets(SourceAssetData, ExclusionFilter);
		// If this is a valid asset
		if (SourceAssetData.Num() > 0 || bIsFolder)
		{
			CurrentDependencies.Add(SelectedPackage);
		}

		// If we should check for dependencies OR we are currently checking a folder
		// Folders should ALWAYS get checked for assets and subfolders
		if ((CopyParams.bShouldCheckForDependencies && SourceAssetData.Num() > 0) || bIsFolder)
		{
			RecursiveGetDependenciesAdvanced(SelectedPackage, CopyParams, CurrentDependencies, DependencyMap, CopyCustomization, SourceAssetData);
		}
		OutPackageNamesToCopy.Append(CurrentDependencies);
	}
}

bool UAssetToolsImpl::AdvancedCopyPackages(const TMap<FString, FString>& SourceAndDestPackages, const bool bForceAutosave, const bool bCopyOverAllDestinationOverlaps) const
{
	if (ValidateFlattenedAdvancedCopyDestinations(SourceAndDestPackages))
	{
		TArray<FString> SuccessfullyCopiedDestinationFiles;
		TArray<FName> SuccessfullyCopiedSourcePackages;
		TArray<TMap<TSoftObjectPtr<UObject>, TSoftObjectPtr<UObject>>> DuplicatedObjectsForEachPackage;
		TSet<UObject*> ExistingObjectSet;
		TSet<UObject*> NewObjectSet;
		FString CopyErrors;

		SuccessfullyCopiedDestinationFiles.Reserve(SourceAndDestPackages.Num());
		SuccessfullyCopiedSourcePackages.Reserve(SourceAndDestPackages.Num());
		DuplicatedObjectsForEachPackage.Reserve(SourceAndDestPackages.Num());
		ExistingObjectSet.Reserve(SourceAndDestPackages.Num());
		NewObjectSet.Reserve(SourceAndDestPackages.Num());

		FScopedSlowTask LoopProgress(SourceAndDestPackages.Num(), LOCTEXT("AdvancedCopying", "Copying files and dependencies..."));
		LoopProgress.MakeDialog();

		for (const auto& Package : SourceAndDestPackages)
		{
			const FString& PackageName = Package.Key;
			const FString& DestFilename = Package.Value;
			FString SrcFilename;

			if (FPackageName::DoesPackageExist(PackageName, &SrcFilename))
			{
				LoopProgress.EnterProgressFrame();
				UPackage* Pkg = LoadPackage(nullptr, *PackageName, LOAD_None);
				if (Pkg)
				{
					FString Name = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(SrcFilename));
					UObject* ExistingObject = StaticFindObject(UObject::StaticClass(), Pkg, *Name);
					if (ExistingObject)
					{
						TSet<UPackage*> ObjectsUserRefusedToFullyLoad;
						ObjectTools::FMoveDialogInfo MoveDialogInfo;
						MoveDialogInfo.bOkToAll = bCopyOverAllDestinationOverlaps;
						// The default value for save packages is true if SCC is enabled because the user can use SCC to revert a change
						MoveDialogInfo.bSavePackages = ISourceControlModule::Get().IsEnabled() || bForceAutosave;
						MoveDialogInfo.PGN.GroupName = TEXT("");
						MoveDialogInfo.PGN.ObjectName = FPaths::GetBaseFilename(DestFilename);
						MoveDialogInfo.PGN.PackageName = DestFilename;
						const bool bShouldPromptForDestinationConflict = !bCopyOverAllDestinationOverlaps;
						TMap<TSoftObjectPtr<UObject>, TSoftObjectPtr<UObject>> DuplicatedObjects;

						if (UObject* NewObject = ObjectTools::DuplicateSingleObject(ExistingObject, MoveDialogInfo.PGN, ObjectsUserRefusedToFullyLoad, bShouldPromptForDestinationConflict, &DuplicatedObjects))
						{
							ExistingObjectSet.Add(ExistingObject);
							NewObjectSet.Add(NewObject);
							DuplicatedObjectsForEachPackage.Add(MoveTemp(DuplicatedObjects));
							SuccessfullyCopiedSourcePackages.Add(FName(*PackageName));
							SuccessfullyCopiedDestinationFiles.Add(DestFilename);
						}
					}
				}
			}
		}

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

		TSet<UObject*> ObjectsAndSubObjectsToReplaceWithin;
		ObjectTools::GatherSubObjectsForReferenceReplacement(NewObjectSet, ExistingObjectSet, ObjectsAndSubObjectsToReplaceWithin);

		TArray<FName> Dependencies;
		TArray<UObject*> ObjectsToReplace;
		for (FName SuccessfullyCopiedPackage : SuccessfullyCopiedSourcePackages)
		{
			Dependencies.Reset();
			AssetRegistryModule.Get().GetDependencies(SuccessfullyCopiedPackage, Dependencies);
			for (FName Dependency : Dependencies)
			{
				const int32 DependencyIndex = SuccessfullyCopiedSourcePackages.IndexOfByKey(Dependency);
				if (DependencyIndex != INDEX_NONE)
				{
					for (const auto& DuplicatedObjectPair : DuplicatedObjectsForEachPackage[DependencyIndex])
					{
						UObject* SourceObject = DuplicatedObjectPair.Key.Get();
						UObject* NewObject = DuplicatedObjectPair.Value.Get();
						if (SourceObject && NewObject)
						{
							ObjectsToReplace.Reset();
							ObjectsToReplace.Add(SourceObject);
							ObjectTools::ConsolidateObjects(NewObject, ObjectsToReplace, ObjectsAndSubObjectsToReplaceWithin, ExistingObjectSet, false);
						}
					}
				}
			}
		}

		ObjectTools::CompileBlueprintsAfterRefUpdate(NewObjectSet.Array());

		FString SourceControlErrors;

		if (SuccessfullyCopiedDestinationFiles.Num() > 0)
		{
			// attempt to add files to source control (this can quite easily fail, but if it works it is very useful)
			if (GetDefault<UEditorLoadingSavingSettings>()->bSCCAutoAddNewFiles)
			{
				if (ISourceControlModule::Get().IsEnabled())
				{
					ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
					if (SourceControlProvider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), SuccessfullyCopiedDestinationFiles) == ECommandResult::Failed)
					{
						for (auto FileIt(SuccessfullyCopiedDestinationFiles.CreateConstIterator()); FileIt; FileIt++)
						{
							if (!SourceControlProvider.GetState(*FileIt, EStateCacheUsage::Use)->IsAdded())
							{
								SourceControlErrors += FText::Format(LOCTEXT("AdvancedCopyPackages_SourceControlError", "{0} could not be added to source control"), FText::FromString(*FileIt)).ToString();
								SourceControlErrors += LINE_TERMINATOR;
							}
						}
					}
				}
			}
		}

		FMessageLog AdvancedCopyLog("AssetTools");
		FText LogMessage = FText::FromString(TEXT("Advanced content copy completed successfully!"));
		EMessageSeverity::Type Severity = EMessageSeverity::Info;
		if (SourceControlErrors.Len() > 0)
		{
			FString ErrorMessage;
			Severity = EMessageSeverity::Error;
			if (SourceControlErrors.Len() > 0)
			{
				AdvancedCopyLog.NewPage(LOCTEXT("AdvancedCopyPackages_SourceControlErrorsListPage", "Source Control Errors"));
				AdvancedCopyLog.Error(FText::FromString(*SourceControlErrors));
				ErrorMessage += LINE_TERMINATOR;
				ErrorMessage += LOCTEXT("AdvancedCopyPackages_SourceControlErrorsList", "Some files reported source control errors.").ToString();
			}
			if (SuccessfullyCopiedSourcePackages.Num() > 0)
			{
				AdvancedCopyLog.NewPage(LOCTEXT("AdvancedCopyPackages_CopyErrorsSuccesslistPage", "Copied Successfully"));
				AdvancedCopyLog.Info(FText::FromString(*SourceControlErrors));
				ErrorMessage += LINE_TERMINATOR;
				ErrorMessage += LOCTEXT("AdvancedCopyPackages_CopyErrorsSuccesslist", "Some files were copied successfully.").ToString();
				for (auto FileIt = SuccessfullyCopiedSourcePackages.CreateConstIterator(); FileIt; ++FileIt)
				{
					if (!FileIt->IsNone())
					{
						AdvancedCopyLog.Info(FText::FromName(*FileIt));
					}
				}
			}
			LogMessage = FText::FromString(ErrorMessage);
		}
		else
		{
			AdvancedCopyLog.NewPage(LOCTEXT("AdvancedCopyPackages_CompletePage", "Advanced content copy completed successfully!"));
			for (auto FileIt = SuccessfullyCopiedSourcePackages.CreateConstIterator(); FileIt; ++FileIt)
			{
				if (!FileIt->IsNone())
				{
					AdvancedCopyLog.Info(FText::FromName(*FileIt));
				}
			}
		}
		AdvancedCopyLog.Notify(LogMessage, Severity, true);
		return true;
	}
	return false;
}

bool UAssetToolsImpl::AdvancedCopyPackages(const FAdvancedCopyParams& CopyParams, const TArray<TMap<FString, FString>> PackagesAndDestinations) const
{
	TMap<FString, FString> FlattenedDestinationMap;
	if (FlattenAdvancedCopyDestinations(PackagesAndDestinations, FlattenedDestinationMap))
	{
		return AdvancedCopyPackages(FlattenedDestinationMap, CopyParams.bShouldForceSave, CopyParams.bCopyOverAllDestinationOverlaps);
	}
	return false;
}

bool UAssetToolsImpl::RenameAssets(const TArray<FAssetRenameData>& AssetsAndNames)
{
	return AssetRenameManager->RenameAssets(AssetsAndNames);
}

EAssetRenameResult UAssetToolsImpl::RenameAssetsWithDialog(const TArray<FAssetRenameData>& AssetsAndNames, bool bAutoCheckout)
{
	return AssetRenameManager->RenameAssetsWithDialog(AssetsAndNames, bAutoCheckout);
}

void UAssetToolsImpl::FindSoftReferencesToObject(FSoftObjectPath TargetObject, TArray<UObject*>& ReferencingObjects)
{
	AssetRenameManager->FindSoftReferencesToObject(TargetObject, ReferencingObjects);
}

void UAssetToolsImpl::FindSoftReferencesToObjects(const TArray<FSoftObjectPath>& TargetObjects, TMap<FSoftObjectPath, TArray<UObject*>>& ReferencingObjects)
{
	AssetRenameManager->FindSoftReferencesToObjects(TargetObjects, ReferencingObjects);
}

void UAssetToolsImpl::RenameReferencingSoftObjectPaths(const TArray<UPackage *> PackagesToCheck, const TMap<FSoftObjectPath, FSoftObjectPath>& AssetRedirectorMap)
{
	AssetRenameManager->RenameReferencingSoftObjectPaths(PackagesToCheck, AssetRedirectorMap);
}

TArray<UObject*> UAssetToolsImpl::ImportAssetsWithDialog(const FString& DestinationPath)
{
	const bool bAllowAsyncImport = false;
	return ImportAssetsWithDialogImplementation(DestinationPath, bAllowAsyncImport);
}

void UAssetToolsImpl::ImportAssetsWithDialogAsync(const FString& DestinationPath)
{
	const bool bAllowAsyncImport = true;
	ImportAssetsWithDialogImplementation(DestinationPath, bAllowAsyncImport);
}

TArray<UObject*> UAssetToolsImpl::ImportAssetsAutomated(const UAutomatedAssetImportData* ImportData)
{
	check(ImportData);

	FAssetImportParams Params;

	Params.bAutomated = true;
	Params.bForceOverrideExisting = ImportData->bReplaceExisting;
	Params.bSyncToBrowser = false;
	Params.SpecifiedFactory = TStrongObjectPtr<UFactory>(ImportData->Factory);
	Params.ImportData = ImportData;

	return ImportAssetsInternal(ImportData->Filenames, ImportData->DestinationPath, nullptr, Params);
}

void UAssetToolsImpl::ImportAssetTasks(const TArray<UAssetImportTask*>& ImportTasks)
{
	FScopedSlowTask SlowTask(ImportTasks.Num(), LOCTEXT("ImportSlowTask", "Importing"));
	SlowTask.MakeDialog();

	FAssetImportParams Params;
	Params.bSyncToBrowser = false;

	TArray<FString> Filenames;
	Filenames.Add(TEXT(""));
	TArray<UPackage*> PackagesToSave;
	for (UAssetImportTask* ImportTask : ImportTasks)
	{
		if (!ImportTask)
		{
			UE_LOG(LogAssetTools, Warning, TEXT("ImportAssetTasks() supplied an empty task"));
			continue;
		}

		SlowTask.EnterProgressFrame(1, FText::Format(LOCTEXT("Import_ImportingFile", "Importing \"{0}\"..."), FText::FromString(FPaths::GetBaseFilename(ImportTask->Filename))));

		Params.AssetImportTask = ImportTask;
		Params.bForceOverrideExisting = ImportTask->bReplaceExisting;
		Params.bAutomated = ImportTask->bAutomated;
		Params.SpecifiedFactory = TStrongObjectPtr<UFactory>(ImportTask->Factory);
		Filenames[0] = ImportTask->Filename;
		TArray<UObject*> ImportedObjects = ImportAssetsInternal(Filenames, ImportTask->DestinationPath, nullptr, Params);

		PackagesToSave.Reset(1); 
		for (UObject* Object : ImportedObjects)
		{
			ImportTask->ImportedObjectPaths.Add(Object->GetPathName());
			if (ImportTask->bSave)
			{
				PackagesToSave.AddUnique(Object->GetOutermost());
			}
		}

		if (ImportTask->bSave)
		{
			UEditorLoadingAndSavingUtils::SavePackages(PackagesToSave, true);
		}
	}
}

void UAssetToolsImpl::ExportAssets(const TArray<FString>& AssetsToExport, const FString& ExportPath)
{
	TArray<UObject*> AssetObjectsToExport;
	AssetObjectsToExport.Reserve(AssetsToExport.Num());

	for (const FString& AssetStr : AssetsToExport)
	{
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetStr);
		if (Asset)
		{
			AssetObjectsToExport.Add(Asset);
		}
		else
		{
			UE_LOG(LogAssetTools, Error, TEXT("Could not load asset '%s' to export it"), *AssetStr);
		}
	}

	const bool bPromptIndividualFilenames = false;
	ExportAssetsInternal(AssetObjectsToExport, bPromptIndividualFilenames, ExportPath);
}

void UAssetToolsImpl::ExportAssets(const TArray<UObject*>& AssetsToExport, const FString& ExportPath) const
{
	const bool bPromptIndividualFilenames = false;
	ExportAssetsInternal(AssetsToExport, bPromptIndividualFilenames, ExportPath);
}

void UAssetToolsImpl::ExportAssetsWithDialog(const TArray<UObject*>& AssetsToExport, bool bPromptForIndividualFilenames)
{
	ExportAssetsInternal(AssetsToExport, bPromptForIndividualFilenames, TEXT(""));
}

void UAssetToolsImpl::ExportAssetsWithDialog(const TArray<FString>& AssetsToExport, bool bPromptForIndividualFilenames)
{
	TArray<UObject*> AssetObjectsToExport;
	AssetObjectsToExport.Reserve(AssetsToExport.Num());

	for (const FString& AssetStr : AssetsToExport)
	{
		UObject* Asset = LoadObject<UObject>(nullptr, *AssetStr);
		if (Asset)
		{
			AssetObjectsToExport.Add(Asset);
		}
		else
		{
			UE_LOG(LogAssetTools, Error, TEXT("Could not load asset '%s' to export it"), *AssetStr);
		}
	}

	ExportAssetsInternal(AssetObjectsToExport, bPromptForIndividualFilenames, TEXT(""));
}

void UAssetToolsImpl::ExpandDirectories(const TArray<FString>& Files, const FString& DestinationPath, TArray<TPair<FString, FString>>& FilesAndDestinations) const
{
	// Iterate through all files in the list, if any folders are found, recurse and expand them.
	for ( int32 FileIdx = 0; FileIdx < Files.Num(); ++FileIdx )
	{
		const FString& Filename = Files[FileIdx];

		// If the file being imported is a directory, just include all sub-files and skip the directory.
		if ( IFileManager::Get().DirectoryExists(*Filename) )
		{
			FString FolderName = FPaths::GetCleanFilename(Filename);

			// Get all files & folders in the folder.
			FString SearchPath = Filename / FString(TEXT("*"));
			TArray<FString> SubFiles;
			IFileManager::Get().FindFiles(SubFiles, *SearchPath, true, true);

			// FindFiles just returns file and directory names, so we need to tack on the root path to get the full path.
			TArray<FString> FullPathItems;
			for ( FString& SubFile : SubFiles )
			{
				FullPathItems.Add(Filename / SubFile);
			}

			// Expand any sub directories found.
			FString NewSubDestination = DestinationPath / FolderName;
			ExpandDirectories(FullPathItems, NewSubDestination, FilesAndDestinations);
		}
		else
		{
			// Add any files and their destination path.
			FilesAndDestinations.Emplace(Filename, DestinationPath);
		}
	}
}
TArray<UObject*> UAssetToolsImpl::ImportAssets(const TArray<FString>& Files, const FString& DestinationPath, UFactory* ChosenFactory, bool bSyncToBrowser /* = true */, TArray<TPair<FString, FString>>* FilesAndDestinations /* = nullptr */, bool bAllowAsyncImport /* = false */) const
{
	const bool bForceOverrideExisting = false;

	FAssetImportParams Params;

	Params.bAutomated = false;
	Params.bForceOverrideExisting = false;
	Params.bSyncToBrowser = bSyncToBrowser;
	Params.SpecifiedFactory = TStrongObjectPtr<UFactory>(ChosenFactory);
	Params.bAllowAsyncImport = bAllowAsyncImport;

	return ImportAssetsInternal(Files, DestinationPath, FilesAndDestinations, Params);
}

void UAssetToolsImpl::CreateUniqueAssetName(const FString& InBasePackageName, const FString& InSuffix, FString& OutPackageName, FString& OutAssetName)
{
	const FString SanitizedBasePackageName = UPackageTools::SanitizePackageName(InBasePackageName);

	const FString PackagePath = FPackageName::GetLongPackagePath(SanitizedBasePackageName);
	const FString BaseAssetNameWithSuffix = FPackageName::GetLongPackageAssetName(SanitizedBasePackageName) + InSuffix;
	const FString SanitizedBaseAssetName = ObjectTools::SanitizeObjectName(BaseAssetNameWithSuffix);

	int32 IntSuffix = 0;
	bool bObjectExists = false;

	int32 CharIndex = SanitizedBaseAssetName.Len() - 1;
	while (CharIndex >= 0 && SanitizedBaseAssetName[CharIndex] >= TEXT('0') && SanitizedBaseAssetName[CharIndex] <= TEXT('9'))
	{
		--CharIndex;
	}
	FString TrailingInteger;
	FString TrimmedBaseAssetName = SanitizedBaseAssetName;
	if (SanitizedBaseAssetName.Len() > 0 && CharIndex == -1)
	{
		// This is the all numeric name, in this case we'd like to append _number, because just adding a number isn't great
		TrimmedBaseAssetName += TEXT("_");
		IntSuffix = 2;
	}
	if (CharIndex >= 0 && CharIndex < SanitizedBaseAssetName.Len() - 1)
	{
		TrailingInteger = SanitizedBaseAssetName.RightChop(CharIndex + 1);
		TrimmedBaseAssetName = SanitizedBaseAssetName.Left(CharIndex + 1);
		IntSuffix = FCString::Atoi(*TrailingInteger);
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));

	do
	{
		bObjectExists = false;
		if ( IntSuffix < 1 )
		{
			OutAssetName = SanitizedBaseAssetName;
		}
		else
		{
			FString Suffix = FString::Printf(TEXT("%d"), IntSuffix);
			while (Suffix.Len() < TrailingInteger.Len())
			{
				Suffix = TEXT("0") + Suffix;
			}
			OutAssetName = FString::Printf(TEXT("%s%s"), *TrimmedBaseAssetName, *Suffix);
		}
	
		OutPackageName = PackagePath + TEXT("/") + OutAssetName;
		FString ObjectPath = OutPackageName + TEXT(".") + OutAssetName;

		// Use the asset registry if possible to find existing assets without loading them
		if ( !AssetRegistryModule.Get().IsLoadingAssets() )
		{
			FAssetData AssetData = AssetRegistryModule.Get().GetAssetByObjectPath(*ObjectPath);
			if(AssetData.IsValid())
			{
				bObjectExists = true;
			}
		}
		else
		{
			bObjectExists = LoadObject<UObject>(nullptr, *ObjectPath, nullptr, LOAD_NoWarn | LOAD_NoRedirects) != nullptr;
		}
		IntSuffix++;
	}
	while (bObjectExists != false);
}

bool UAssetToolsImpl::AssetUsesGenericThumbnail( const FAssetData& AssetData ) const
{
	if ( !AssetData.IsValid() )
	{
		// Invalid asset, assume it does not use a shared thumbnail
		return false;
	}

	if( AssetData.IsAssetLoaded() )
	{
		// Loaded asset, see if there is a rendering info for it
		UObject* Asset = AssetData.GetAsset();
		FThumbnailRenderingInfo* RenderInfo = GUnrealEd->GetThumbnailManager()->GetRenderingInfo( Asset );
		return !RenderInfo || !RenderInfo->Renderer;
	}

	if ( AssetData.AssetClass == UBlueprint::StaticClass()->GetFName() )
	{
		// Unloaded blueprint asset
		// It would be more correct here to find the rendering info for the generated class,
		// but instead we are simply seeing if there is a thumbnail saved on disk for this asset
		FString PackageFilename;
		if ( FPackageName::DoesPackageExist(AssetData.PackageName.ToString(), &PackageFilename) )
		{
			TSet<FName> ObjectFullNames;
			FThumbnailMap ThumbnailMap;

			FName ObjectFullName = FName(*AssetData.GetFullName());
			ObjectFullNames.Add(ObjectFullName);

			ThumbnailTools::LoadThumbnailsFromPackage(PackageFilename, ObjectFullNames, ThumbnailMap);

			FObjectThumbnail* ThumbnailPtr = ThumbnailMap.Find(ObjectFullName);
			if (ThumbnailPtr)
			{
				return (*ThumbnailPtr).IsEmpty();
			}

			return true;
		}
	}
	else
	{
		// Unloaded non-blueprint asset. See if the class has a rendering info.
		UClass* Class = FindObject<UClass>(ANY_PACKAGE, *AssetData.AssetClass.ToString());

		UObject* ClassCDO = nullptr;
		if (Class != nullptr)
		{
			ClassCDO = Class->GetDefaultObject();
		}

		// Get the rendering info for this object
		FThumbnailRenderingInfo* RenderInfo = nullptr;
		if (ClassCDO != nullptr)
		{
			RenderInfo = GUnrealEd->GetThumbnailManager()->GetRenderingInfo( ClassCDO );
		}

		return !RenderInfo || !RenderInfo->Renderer;
	}

	return false;
}

void UAssetToolsImpl::DiffAgainstDepot( UObject* InObject, const FString& InPackagePath, const FString& InPackageName ) const
{
	check( InObject );

	// Make sure our history is up to date
	ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
	TSharedRef<FUpdateStatus, ESPMode::ThreadSafe> UpdateStatusOperation = ISourceControlOperation::Create<FUpdateStatus>();
	UpdateStatusOperation->SetUpdateHistory(true);
	SourceControlProvider.Execute(UpdateStatusOperation, SourceControlHelpers::PackageFilename(InPackagePath));

	// Get the SCC state
	FSourceControlStatePtr SourceControlState = SourceControlProvider.GetState(SourceControlHelpers::PackageFilename(InPackagePath), EStateCacheUsage::Use);

	// If we have an asset and its in SCC..
	if( SourceControlState.IsValid() && InObject != nullptr && SourceControlState->IsSourceControlled() )
	{
		// Get the file name of package
		FString RelativeFileName;
		if(FPackageName::DoesPackageExist(InPackagePath, &RelativeFileName))
		{
			if(SourceControlState->GetHistorySize() > 0)
			{
				TSharedPtr<ISourceControlRevision, ESPMode::ThreadSafe> Revision = SourceControlState->GetHistoryItem(0);
				check(Revision.IsValid());

				// Get the head revision of this package from source control
				FString AbsoluteFileName = FPaths::ConvertRelativePathToFull(RelativeFileName);
				FString TempFileName;
				if(Revision->Get(TempFileName))
				{
					// Try and load that package
					UPackage* TempPackage = LoadPackage(nullptr, *TempFileName, LOAD_ForDiff|LOAD_DisableCompileOnLoad);
					if(TempPackage != nullptr)
					{
						// Grab the old asset from that old package
						UObject* OldObject = FindObject<UObject>(TempPackage, *InPackageName);

						// Recovery for package names that don't match
						if (OldObject == nullptr)
						{
							OldObject = TempPackage->FindAssetInPackage();
						}

						if(OldObject != nullptr)
						{
							/* Set the revision information*/
							FRevisionInfo OldRevision;
							OldRevision.Changelist = Revision->GetCheckInIdentifier();
							OldRevision.Date = Revision->GetDate();
							OldRevision.Revision = Revision->GetRevision();

							FRevisionInfo NewRevision; 
							NewRevision.Revision = TEXT("");
							DiffAssets(OldObject, InObject, OldRevision, NewRevision);
						}
					}
				}
			}
		} 
	}
}

void UAssetToolsImpl::DiffAssets(UObject* OldAsset, UObject* NewAsset, const struct FRevisionInfo& OldRevision, const struct FRevisionInfo& NewRevision) const
{
	if(OldAsset == nullptr || NewAsset == nullptr)
	{
		UE_LOG(LogAssetTools, Warning, TEXT("DiffAssets: One of the supplied assets was nullptr."));
		return;
	}

	// Get class of both assets 
	UClass* OldClass = OldAsset->GetClass();
	UClass* NewClass = NewAsset->GetClass();
	// If same class..
	if(OldClass == NewClass)
	{
		// Get class-specific actions
		TWeakPtr<IAssetTypeActions> Actions = GetAssetTypeActionsForClass( NewClass );
		if(Actions.IsValid())
		{
			// And use that to perform the Diff
			Actions.Pin()->PerformAssetDiff(OldAsset, NewAsset, OldRevision, NewRevision);
		}
	}
	else
	{
		UE_LOG(LogAssetTools, Warning, TEXT("DiffAssets: Classes were not the same."));
	}
}

FString UAssetToolsImpl::DumpAssetToTempFile(UObject* Asset) const
{
	check(Asset);

	// Clear the mark state for saving.
	UnMarkAllObjects(EObjectMark(OBJECTMARK_TagExp | OBJECTMARK_TagImp));

	FStringOutputDevice Archive;
	const FExportObjectInnerContext Context;

	// Export asset to archive
	UExporter::ExportToOutputDevice(&Context, Asset, nullptr, Archive, TEXT("copy"), 0, PPF_ExportsNotFullyQualified|PPF_Copy|PPF_Delimited, false, Asset->GetOuter());

	// Used to generate unique file names during a run
	static int TempFileNum = 0;

	// Build name for temp text file
	FString RelTempFileName = FString::Printf(TEXT("%sText%s-%d.txt"), *FPaths::DiffDir(), *Asset->GetName(), TempFileNum++);
	FString AbsoluteTempFileName = FPaths::ConvertRelativePathToFull(RelTempFileName);

	// Save text into temp file
	if( !FFileHelper::SaveStringToFile( Archive, *AbsoluteTempFileName ) )
	{
		//UE_LOG(LogAssetTools, Warning, TEXT("DiffAssets: Could not write %s"), *AbsoluteTempFileName);
		return TEXT("");
	}
	else
	{
		return AbsoluteTempFileName;
	}
}

FString WrapArgument(const FString& Argument)
{
	// Wrap the passed in argument so it changes from Argument to "Argument"
	return FString::Printf(TEXT("%s%s%s"),	(Argument.StartsWith("\"")) ? TEXT(""): TEXT("\""),
											*Argument,
											(Argument.EndsWith("\"")) ? TEXT(""): TEXT("\""));
}

bool UAssetToolsImpl::CreateDiffProcess(const FString& DiffCommand,  const FString& OldTextFilename,  const FString& NewTextFilename, const FString& DiffArgs) const
{
	// Construct Arguments
	FString Arguments = FString::Printf( TEXT("%s %s %s"),*WrapArgument(OldTextFilename), *WrapArgument(NewTextFilename), *DiffArgs );

	bool bTryRunDiff = true;
	FString NewDiffCommand = DiffCommand;

	while (bTryRunDiff)
	{
		// Fire process
		if (FPlatformProcess::CreateProc(*NewDiffCommand, *Arguments, true, false, false, nullptr, 0, nullptr, nullptr).IsValid())
		{
			return true;
		}
		else
		{
			const FText Message = FText::Format(NSLOCTEXT("AssetTools", "DiffFail", "The currently set diff tool '{0}' could not be run. Would you like to set a new diff tool?"), FText::FromString(DiffCommand));
			EAppReturnType::Type Response = FMessageDialog::Open(EAppMsgType::YesNo, Message);
			if (Response == EAppReturnType::No)
			{
				bTryRunDiff = false;
			}
			else
			{
				IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
				check(DesktopPlatform);

				const FText FileFilterType = NSLOCTEXT("AssetTools", "Executables", "Executables");
#if PLATFORM_WINDOWS
				const FString FileFilterText = FString::Printf(TEXT("%s (*.exe)|*.exe"), *FileFilterType.ToString());
#elif PLATFORM_MAC
				const FString FileFilterText = FString::Printf(TEXT("%s (*.app)|*.app"), *FileFilterType.ToString());
#else
				const FString FileFilterText = FString::Printf(TEXT("%s"), *FileFilterType.ToString());
#endif

				TArray<FString> OutFiles;
				if (DesktopPlatform->OpenFileDialog(
					nullptr,
					NSLOCTEXT("AssetTools", "ChooseDiffTool", "Choose Diff Tool").ToString(),
					TEXT(""),
					TEXT(""),
					FileFilterText,
					EFileDialogFlags::None,
					OutFiles))
				{
					UEditorLoadingSavingSettings& Settings = *GetMutableDefault<UEditorLoadingSavingSettings>();
					Settings.TextDiffToolPath.FilePath = OutFiles[0];
					Settings.SaveConfig();
					NewDiffCommand = OutFiles[0];
				}
			}
		}
	}

	return false;
}

void UAssetToolsImpl::MigratePackages(const TArray<FName>& PackageNamesToMigrate) const
{
	// Packages must be saved for the migration to work
	const bool bPromptUserToSave = true;
	const bool bSaveMapPackages = true;
	const bool bSaveContentPackages = true;
	if ( FEditorFileUtils::SaveDirtyPackages( bPromptUserToSave, bSaveMapPackages, bSaveContentPackages ) )
	{
		FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
		if ( AssetRegistryModule.Get().IsLoadingAssets() )
		{
			// Open a dialog asking the user to wait while assets are being discovered
			SDiscoveringAssetsDialog::OpenDiscoveringAssetsDialog(
				SDiscoveringAssetsDialog::FOnAssetsDiscovered::CreateUObject(this, &UAssetToolsImpl::PerformMigratePackages, PackageNamesToMigrate)
			);
		}
		else
		{
			// Assets are already discovered, perform the migration now
			PerformMigratePackages(PackageNamesToMigrate);
		}
	}
}

void UAssetToolsImpl::OnNewImportRecord(UClass* AssetType, const FString& FileExtension, bool bSucceeded, bool bWasCancelled, const FDateTime& StartTime)
{
	// Don't attempt to report usage stats if analytics isn't available
	if(AssetType != nullptr && FEngineAnalytics::IsAvailable())
	{
		TArray<FAnalyticsEventAttribute> Attribs;
		Attribs.Add(FAnalyticsEventAttribute(TEXT("AssetType"), AssetType->GetName()));
		Attribs.Add(FAnalyticsEventAttribute(TEXT("FileExtension"), FileExtension));
		Attribs.Add(FAnalyticsEventAttribute(TEXT("Outcome"), bSucceeded ? TEXT("Success") : (bWasCancelled ? TEXT("Cancelled") : TEXT("Failed"))));
		FTimespan TimeTaken = FDateTime::UtcNow() - StartTime;
		Attribs.Add(FAnalyticsEventAttribute(TEXT("TimeTaken.Seconds"), (float)TimeTaken.GetTotalSeconds()));

		FEngineAnalytics::GetProvider().RecordEvent(TEXT("Editor.Usage.ImportAsset"), Attribs);
	}
}

void UAssetToolsImpl::OnNewCreateRecord(UClass* AssetType, bool bDuplicated)
{
	// Don't attempt to report usage stats if analytics isn't available
	if(AssetType != nullptr && FEngineAnalytics::IsAvailable())
	{
		TArray<FAnalyticsEventAttribute> Attribs;
		Attribs.Add(FAnalyticsEventAttribute(TEXT("AssetType"), AssetType->GetName()));
		Attribs.Add(FAnalyticsEventAttribute(TEXT("Duplicated"), bDuplicated? TEXT("Yes") : TEXT("No")));

		FEngineAnalytics::GetProvider().RecordEvent(TEXT("Editor.Usage.CreateAsset"), Attribs);
	}
}

TArray<UObject*> UAssetToolsImpl::ImportAssetsInternal(const TArray<FString>& Files, const FString& RootDestinationPath, TArray<TPair<FString, FString>> *FilesAndDestinationsPtr, const FAssetImportParams& Params) const
{
	TGuardValue<bool> UnattendedScriptGuard(GIsRunningUnattendedScript, GIsRunningUnattendedScript || Params.bAutomated);

	UFactory* SpecifiedFactory = Params.SpecifiedFactory.Get();
	const bool bForceOverrideExisting = Params.bForceOverrideExisting;
	const bool bSyncToBrowser = Params.bSyncToBrowser;
	const bool bAutomatedImport = Params.bAutomated || GIsAutomationTesting;

	TArray<UObject*> ReturnObjects;
	TArray<FString> ValidFiles;
	ValidFiles.Reserve(Files.Num());
	for (int32 FileIndex = 0; FileIndex < Files.Num(); ++FileIndex)
	{
		if (!Files[FileIndex].IsEmpty())
		{
			FString InputFile = Files[FileIndex];
			FPaths::NormalizeDirectoryName(InputFile);
			ValidFiles.Add(InputFile);
		}
	}
	TMap< FString, TArray<UFactory*> > ExtensionToFactoriesMap;

	FScopedSlowTask SlowTask(ValidFiles.Num(), LOCTEXT("ImportSlowTask", "Importing"));

	bool bUseInterchangeFramework = false;
	bool bUseInterchangeFrameworkForTextureOnly = false;
	UInterchangeManager& InterchangeManager = UInterchangeManager::GetInterchangeManager();
#if WITH_EDITOR
	const UEditorExperimentalSettings* EditorExperimentalSettings = GetDefault<UEditorExperimentalSettings>();
	bUseInterchangeFramework = EditorExperimentalSettings->bEnableInterchangeFramework;

	if (bUseInterchangeFramework && Params.SpecifiedFactory)
	{
		if (Params.SpecifiedFactory->GetClass()->IsChildOf(USceneImportFactory::StaticClass()))
		{
			bUseInterchangeFramework = GetDefault<UInterchangeProjectSettings>()->bUseInterchangeWhenImportingIntoLevel;
		}
	}

	bUseInterchangeFrameworkForTextureOnly = (!bUseInterchangeFramework) && EditorExperimentalSettings->bEnableInterchangeFrameworkForTextureOnly;
	bUseInterchangeFramework |= bUseInterchangeFrameworkForTextureOnly;
#endif

	// Block interchange use if the user is not aware that is import can be async. Otherwise, we don't return the imported object and we can't mimic the SpecifiedFactory settings.
	bUseInterchangeFramework &= Params.bAllowAsyncImport;

	if (!bUseInterchangeFramework && ValidFiles.Num() > 1)
	{	
		//Always allow user to cancel the import task if they are importing multiple ValidFiles.
		//If we're importing a single file, then the factory policy will dictate if the import if cancelable.
		SlowTask.MakeDialog(true);
	}

	TArray<TPair<FString, FString>> FilesAndDestinations;
	if (FilesAndDestinationsPtr == nullptr)
	{
		ExpandDirectories(ValidFiles, RootDestinationPath, FilesAndDestinations);
	}
	else
	{
		FilesAndDestinations = (*FilesAndDestinationsPtr);
	}

	if(SpecifiedFactory == nullptr)
	{
		// First instantiate one factory for each file extension encountered that supports the extension
		// @todo import: gmp: show dialog in case of multiple matching factories
		for(TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
		{
			if(!(*ClassIt)->IsChildOf(UFactory::StaticClass()) || ((*ClassIt)->HasAnyClassFlags(CLASS_Abstract)) || (*ClassIt)->IsChildOf(USceneImportFactory::StaticClass()))
			{
				continue;
			}

			UFactory* Factory = Cast<UFactory>((*ClassIt)->GetDefaultObject());

			if(!Factory->bEditorImport)
			{
				continue;
			}

			TArray<FString> FactoryExtensions;
			Factory->GetSupportedFileExtensions(FactoryExtensions);

			for(auto& FileDest : FilesAndDestinations)
			{
				const FString FileExtension = FPaths::GetExtension(FileDest.Key);

				// Case insensitive string compare with supported formats of this factory
				if(FactoryExtensions.Contains(FileExtension))
				{
					TArray<UFactory*>& ExistingFactories = ExtensionToFactoriesMap.FindOrAdd(FileExtension);

					// Do not remap extensions, just reuse the existing UFactory.
					// There may be multiple UFactories, so we will keep track of all of them
					bool bFactoryAlreadyInMap = false;
					for(auto FoundFactoryIt = ExistingFactories.CreateConstIterator(); FoundFactoryIt; ++FoundFactoryIt)
					{
						if((*FoundFactoryIt)->GetClass() == Factory->GetClass())
						{
							bFactoryAlreadyInMap = true;
							break;
						}
					}

					if(!bFactoryAlreadyInMap)
					{
						// We found a factory for this file, it can be imported!
						// Create a new factory of the same class and make sure it doesn't get GCed.
						// The object will be removed from the root set at the end of this function.
						UFactory* NewFactory = NewObject<UFactory>(GetTransientPackage(), Factory->GetClass());
						if(NewFactory->ConfigureProperties())
						{
							NewFactory->AddToRoot();
							ExistingFactories.Add(NewFactory);
						}
					}
				}
			}
		}
	}
	else if(SpecifiedFactory->bEditorImport && !bAutomatedImport) 
	{

		TArray<FString> FactoryExtensions;
		SpecifiedFactory->GetSupportedFileExtensions(FactoryExtensions);

		for(auto FileIt = ValidFiles.CreateConstIterator(); FileIt; ++FileIt)
		{
			const FString FileExtension = FPaths::GetExtension(*FileIt);

			// Case insensitive string compare with supported formats of this factory
			if(!FactoryExtensions.Contains(FileExtension))
			{
				continue;
			}

			TArray<UFactory*>& ExistingFactories = ExtensionToFactoriesMap.FindOrAdd(FileExtension);

			// Do not remap extensions, just reuse the existing UFactory.
			// There may be multiple UFactories, so we will keep track of all of them
			bool bFactoryAlreadyInMap = false;
			for(auto FoundFactoryIt = ExistingFactories.CreateConstIterator(); FoundFactoryIt; ++FoundFactoryIt)
			{
				if((*FoundFactoryIt)->GetClass() == SpecifiedFactory->GetClass())
				{
					bFactoryAlreadyInMap = true;
					break;
				}
			}

			if(!bFactoryAlreadyInMap)
			{
				// We found a factory for this file, it can be imported!
				// Create a new factory of the same class and make sure it doesnt get GCed.
				// The object will be removed from the root set at the end of this function.
				UFactory* NewFactory = NewObject<UFactory>(GetTransientPackage(), SpecifiedFactory->GetClass());
				if(NewFactory->ConfigureProperties())
				{
					NewFactory->AddToRoot();
					ExistingFactories.Add(NewFactory);
				}
			}
		}
	}

	// We need to sort the factories so that they get tested in priority order
	for(auto& ExtensionToFactories : ExtensionToFactoriesMap)
	{
		ExtensionToFactories.Value.Sort(&UFactory::SortFactoriesByPriority);
	}

	// Some flags to keep track of what the user decided when asked about overwriting or replacing
	bool bOverwriteAll = false;
	bool bReplaceAll = false;
	bool bDontOverwriteAny = false;
	bool bDontReplaceAny = false;
	if (bAutomatedImport)
	{
		bOverwriteAll = bReplaceAll = bForceOverrideExisting;
		bDontOverwriteAny = bDontReplaceAny = !bForceOverrideExisting;
	}

	TArray<UFactory*> UsedFactories;
	bool bImportWasCancelled = false;
	bool bOnlyInterchangeImport = bUseInterchangeFramework;
	if(bUseInterchangeFramework)
	{
		for (int32 FileIdx = 0; FileIdx < FilesAndDestinations.Num(); ++FileIdx)
		{
			// Filename will need to get sanitized before we create an asset out of them as they
			// can be created out of sources that contain spaces and other invalid characters. Filename cannot be sanitized
			// until other checks are done that rely on looking at the actual source file so sanitation is delayed.
			const FString& Filename = FilesAndDestinations[FileIdx].Key;
			{
				UE::Interchange::FScopedSourceData ScopedSourceData(Filename);

				if (bUseInterchangeFrameworkForTextureOnly)
				{
					UInterchangeTranslatorBase* Translator = InterchangeManager.GetTranslatorForSourceData(ScopedSourceData.GetSourceData());
					if (!Translator || !InterchangeManager.IsTranslatorClassForTextureOnly(Translator->GetClass()))
					{
						bOnlyInterchangeImport = false;
						break;
					}
				}
				else if (!InterchangeManager.CanTranslateSourceData(ScopedSourceData.GetSourceData()))
				{
					bOnlyInterchangeImport = false;
					break;
				}
			}
		}

		if (!bOnlyInterchangeImport)
		{
			if (Files.Num() > 1)
			{	
				//Always allow user to cancel the import task if they are importing multiple files.
				//If we're importing a single file, then the factory policy will dictate if the import if cancelable.
				SlowTask.MakeDialog(true);
			}
		}
		else
		{
			//Complete the slow task
			SlowTask.CompletedWork = FilesAndDestinations.Num();
		}
	}

	struct FInterchangeImportStatus
	{
		explicit FInterchangeImportStatus(int32 NumFiles)
			: InterchangeResultsContainer(NewObject<UInterchangeResultsContainer>(GetTransientPackage())),
			  ImportCount(NumFiles)
		{}

		TStrongObjectPtr<UInterchangeResultsContainer> InterchangeResultsContainer;
		TArray<TWeakObjectPtr<UObject>> ImportedObjects;
		std::atomic<int32> ImportCount;
	};

	TSharedPtr<FInterchangeImportStatus, ESPMode::ThreadSafe> ImportStatus = MakeShared<FInterchangeImportStatus>(FilesAndDestinations.Num());

	// Now iterate over the input files and use the same factory object for each file with the same extension
	for(int32 FileIdx = 0; FileIdx < FilesAndDestinations.Num() && !bImportWasCancelled; ++FileIdx)
	{
		// Filename and DestinationPath will need to get santized before we create an asset out of them as they
		// can be created out of sources that contain spaces and other invalid characters. Filename cannot be sanitized
		// until other checks are done that rely on looking at the actual source file so sanitation is delayed.
		const FString& Filename = FilesAndDestinations[FileIdx].Key;

		FString DestinationPath;
		FString ErrorMsg;
		if (!FPackageName::TryConvertFilenameToLongPackageName(ObjectTools::SanitizeObjectPath(FilesAndDestinations[FileIdx].Value), DestinationPath, &ErrorMsg))
		{
			const FText Message = FText::Format(LOCTEXT("CannotConvertDestinationPath", "Can't import the file '{0}' because the destination path '{1}' cannot be converted to a package path."), FText::FromString(Filename), FText::FromString(DestinationPath));
			if (!bAutomatedImport)
			{
				FMessageDialog::Open(EAppMsgType::Ok, Message);
			}

			UE_LOG(LogAssetTools, Warning, TEXT("%s"), *ErrorMsg);
			UE_LOG(LogAssetTools, Warning, TEXT("%s"), *Message.ToString());

			continue;
		}

		if (bUseInterchangeFramework)
		{
			UE::Interchange::FScopedSourceData ScopedSourceData(Filename);

			bool bUseATextureTranslator = false;
			if (bUseInterchangeFrameworkForTextureOnly)
			{
				UInterchangeTranslatorBase* Translator = InterchangeManager.GetTranslatorForSourceData(ScopedSourceData.GetSourceData());
				if (Translator && InterchangeManager.IsTranslatorClassForTextureOnly(Translator->GetClass()))
				{
					bUseATextureTranslator = true;
				}
			}

			if (bUseATextureTranslator || (!bUseInterchangeFrameworkForTextureOnly && InterchangeManager.CanTranslateSourceData(ScopedSourceData.GetSourceData())))
			{
				FImportAssetParameters ImportAssetParameters;
				ImportAssetParameters.bIsAutomated = bAutomatedImport;
				ImportAssetParameters.ReimportAsset = nullptr;


				TFunction<void(UE::Interchange::FImportResult&)> AppendImportResult =
					// Note: ImportStatus captured by value so that the lambda keeps the shared ptr alive
					[ImportStatus](UE::Interchange::FImportResult& Result)
					{
						ImportStatus->InterchangeResultsContainer->Append(Result.GetResults());
						ImportStatus->ImportedObjects.Append(Result.GetImportedObjects());

					};

				TFunction<void(UE::Interchange::FImportResult&)> AppendAndBroadcastImportResultIfNeeded =
					// Note: ImportStatus captured by value so that the lambda keeps the shared ptr alive
					[ImportStatus, AppendImportResult, bSyncToBrowser](UE::Interchange::FImportResult& Result)
					{
						AppendImportResult(Result);

						if (--ImportStatus->ImportCount == 0)
						{
							UInterchangeManager& InterchangeManager = UInterchangeManager::GetInterchangeManager();
							InterchangeManager.OnBatchImportComplete.Broadcast(ImportStatus->InterchangeResultsContainer);

							if (bSyncToBrowser)
							{
								// Only sync the content browser when the full import is done. Otherwise it can be annoying for the user.
								// UX suggestion : Maybe we could move this into the post import window as button ("Select Imported Asset(s)") so it would be even less disruptive for the users.
								TArray<UObject*> ImportedObjects;
								ImportedObjects.Reserve(ImportStatus->ImportedObjects.Num());
								for (const TWeakObjectPtr<UObject>& WeakObject : ImportStatus->ImportedObjects)
								{
									ImportedObjects.Add(WeakObject.Get());
								}

								UAssetToolsImpl::Get().SyncBrowserToAssets(ImportedObjects);
							}
						}
					};

				if (Params.SpecifiedFactory && Params.SpecifiedFactory->GetClass()->IsChildOf(USceneImportFactory::StaticClass()))
				{
					TPair<UE::Interchange::FAssetImportResultRef, UE::Interchange::FSceneImportResultRef> InterchangeResults =
						InterchangeManager.ImportSceneAsync(DestinationPath, ScopedSourceData.GetSourceData(), ImportAssetParameters);

					InterchangeResults.Key->OnDone(AppendImportResult);;
					InterchangeResults.Value->OnDone(AppendAndBroadcastImportResultIfNeeded);
				}
				else
				{
					UE::Interchange::FAssetImportResultRef InterchangeResult = (InterchangeManager.ImportAssetAsync(DestinationPath, ScopedSourceData.GetSourceData(), ImportAssetParameters));
					InterchangeResult->OnDone(AppendAndBroadcastImportResultIfNeeded);
				}

				//Import done, iterate the next file and destination
				
				//If we do not import only interchange file, update the progress for each interchange task
				if (!bOnlyInterchangeImport)
				{
					SlowTask.EnterProgressFrame(1, FText::Format(LOCTEXT("Import_ImportingFile", "Importing \"{0}\"..."), FText::FromString(FPaths::GetBaseFilename(Filename))));
				}
				continue;
			}
		}
		FString FileExtension = FPaths::GetExtension(Filename);
		const TArray<UFactory*>* FactoriesPtr = ExtensionToFactoriesMap.Find(FileExtension);
		UFactory* Factory = nullptr;
		SlowTask.EnterProgressFrame(1, FText::Format(LOCTEXT("Import_ImportingFile", "Importing \"{0}\"..."), FText::FromString(FPaths::GetBaseFilename(Filename))));

		// Assume that for automated import, the user knows exactly what factory to use if it exists
		if(bAutomatedImport && SpecifiedFactory && SpecifiedFactory->FactoryCanImport(Filename))
		{
			Factory = SpecifiedFactory;
		}
		else if(FactoriesPtr)
		{
			const TArray<UFactory*>& Factories = *FactoriesPtr;

			// Handle the potential of multiple factories being found
			if(Factories.Num() > 0)
			{
				Factory = Factories[0];

				for(auto FactoryIt = Factories.CreateConstIterator(); FactoryIt; ++FactoryIt)
				{
					UFactory* TestFactory = *FactoryIt;
					if(TestFactory->FactoryCanImport(Filename))
					{
						Factory = TestFactory;
						break;
					}
				}
			}
		}
		else
		{
			if(FEngineAnalytics::IsAvailable())
			{
				TArray<FAnalyticsEventAttribute> Attribs;
				Attribs.Add(FAnalyticsEventAttribute(TEXT("FileExtension"), FileExtension));

				FEngineAnalytics::GetProvider().RecordEvent(TEXT("Editor.Usage.ImportFailed"), Attribs);
			}

			const FText Message = FText::Format(LOCTEXT("ImportFailed_UnknownExtension", "Failed to import '{0}'. Unknown extension '{1}'."), FText::FromString(Filename), FText::FromString(FileExtension));
			FNotificationInfo Info(Message);
			Info.ExpireDuration = 3.0f;
			Info.bUseLargeFont = false;
			Info.bFireAndForget = true;
			Info.bUseSuccessFailIcons = true;
			FSlateNotificationManager::Get().AddNotification(Info)->SetCompletionState(SNotificationItem::CS_Fail);

			UE_LOG(LogAssetTools, Warning, TEXT("%s"), *Message.ToString());
		}

		if(Factory != nullptr)
		{
			if (FilesAndDestinations.Num() == 1)
			{
				SlowTask.MakeDialog(Factory->CanImportBeCanceled());
			}

			// Reset the 'Do you want to overwrite the existing object?' Yes to All / No to All prompt, to make sure the
			// user gets a chance to select something when the factory is first used during this import
			if (!UsedFactories.Contains(Factory))
			{
				Factory->ResetState();
				UsedFactories.AddUnique(Factory);
			}

			UClass* ImportAssetType = Factory->SupportedClass;
			bool bImportSucceeded = false;
			FDateTime ImportStartTime = FDateTime::UtcNow();

			FString Name;
			if (Params.AssetImportTask && !Params.AssetImportTask->DestinationName.IsEmpty())
			{
				Name = Params.AssetImportTask->DestinationName;
			}
			else
			{
				Name = FPaths::GetBaseFilename(Filename);
			}
			Name = ObjectTools::SanitizeObjectName(Name);

			FString PackageName = ObjectTools::SanitizeInvalidChars(FPaths::Combine(*DestinationPath, *Name), INVALID_LONGPACKAGE_CHARACTERS);

			// We can not create assets that share the name of a map file in the same location
			if(FEditorFileUtils::IsMapPackageAsset(PackageName))
			{
				const FText Message = FText::Format(LOCTEXT("AssetNameInUseByMap", "You can not create an asset named '{0}' because there is already a map file with this name in this folder."), FText::FromString(Name));
				if(!bAutomatedImport)
				{
					FMessageDialog::Open(EAppMsgType::Ok, Message);
				}
				UE_LOG(LogAssetTools, Warning, TEXT("%s"), *Message.ToString());
				OnNewImportRecord(ImportAssetType, FileExtension, bImportSucceeded, bImportWasCancelled, ImportStartTime);
				continue;
			}

			UPackage* Pkg = CreatePackage( *PackageName);
			if(!ensure(Pkg))
			{
				// Failed to create the package to hold this asset for some reason
				OnNewImportRecord(ImportAssetType, FileExtension, bImportSucceeded, bImportWasCancelled, ImportStartTime);
				continue;
			}

			// Make sure the destination package is loaded
			Pkg->FullyLoad();

			// Check for an existing object
			UObject* ExistingObject = StaticFindObject(UObject::StaticClass(), Pkg, *Name);
			if(ExistingObject != nullptr)
			{
				// If the existing object is one of the imports we've just created we can't replace or overwrite it
				if(ReturnObjects.Contains(ExistingObject))
				{
					if(ImportAssetType == nullptr)
					{
						// The factory probably supports multiple types and cant be determined yet without asking the user or actually loading it
						// We just need to generate an unused name so object should do fine.
						ImportAssetType = UObject::StaticClass();
					}
					// generate a unique name for this import
					Name = MakeUniqueObjectName(Pkg, ImportAssetType, *Name).ToString();
				}
				else
				{
					// If the object is supported by the factory we are using, ask if we want to overwrite the asset
					// Otherwise, prompt to replace the object
					if(Factory->DoesSupportClass(ExistingObject->GetClass()))
					{
						// The factory can overwrite this object, ask if that is okay, unless "Yes To All" or "No To All" was already selected
						EAppReturnType::Type UserResponse;

						if(bForceOverrideExisting || bOverwriteAll || GIsAutomationTesting)
						{
							UserResponse = EAppReturnType::YesAll;
						}
						else if(bDontOverwriteAny)
						{
							UserResponse = EAppReturnType::NoAll;
						}
						else
						{
							UserResponse = FMessageDialog::Open(
								EAppMsgType::YesNoYesAllNoAll,
								FText::Format(LOCTEXT("ImportObjectAlreadyExists_SameClass", "Do you want to overwrite the existing asset?\n\nAn asset already exists at the import location: {0}"), FText::FromString(PackageName)));

							bOverwriteAll = UserResponse == EAppReturnType::YesAll;
							bDontOverwriteAny = UserResponse == EAppReturnType::NoAll;
						}

						const bool bWantOverwrite = UserResponse == EAppReturnType::Yes || UserResponse == EAppReturnType::YesAll;

						if(!bWantOverwrite)
						{
							// User chose not to replace the package
							bImportWasCancelled = true;
							OnNewImportRecord(ImportAssetType, FileExtension, bImportSucceeded, bImportWasCancelled, ImportStartTime);
							continue;
						}
					}
					else if(!bAutomatedImport)
					{
						// The factory can't overwrite this asset, ask if we should delete the object then import the new one. Only do this if "Yes To All" or "No To All" was not already selected.
						EAppReturnType::Type UserResponse;

						if(bReplaceAll)
						{
							UserResponse = EAppReturnType::YesAll;
						}
						else if(bDontReplaceAny)
						{
							UserResponse = EAppReturnType::NoAll;
						}
						else
						{
							UserResponse = FMessageDialog::Open(
								EAppMsgType::YesNoYesAllNoAll,
								FText::Format(LOCTEXT("ImportObjectAlreadyExists_DifferentClass", "Do you want to replace the existing asset?\n\nAn asset already exists at the import location: {0}"), FText::FromString(PackageName)));

							bReplaceAll = UserResponse == EAppReturnType::YesAll;
							bDontReplaceAny = UserResponse == EAppReturnType::NoAll;
						}

						const bool bWantReplace = UserResponse == EAppReturnType::Yes || UserResponse == EAppReturnType::YesAll;

						if(bWantReplace)
						{
							// Delete the existing object
							int32 NumObjectsDeleted = 0;
							TArray< UObject* > ObjectsToDelete;
							ObjectsToDelete.Add(ExistingObject);

							// If the user forcefully deletes the package, all sorts of things could become invalidated,
							// the Pkg pointer might be killed even though it was added to the root.
							TWeakObjectPtr<UPackage> WeakPkg(Pkg);

							// Dont let the package get garbage collected (just in case we are deleting the last asset in the package)
							Pkg->AddToRoot();
							NumObjectsDeleted = ObjectTools::DeleteObjects(ObjectsToDelete, /*bShowConfirmation=*/false);

							// If the weak package ptr is still valid, it should then be safe to remove it from the root.
							if(WeakPkg.IsValid())
							{
								Pkg->RemoveFromRoot();
							}

							const FString QualifiedName = PackageName + TEXT(".") + Name;
							FText Reason;
							if(NumObjectsDeleted == 0 || !IsUniqueObjectName(*QualifiedName, ANY_PACKAGE, Reason))
							{
								// Original object couldn't be deleted
								const FText Message = FText::Format(LOCTEXT("ImportDeleteFailed", "Failed to delete '{0}'. The asset is referenced by other content."), FText::FromString(PackageName));
								FMessageDialog::Open(EAppMsgType::Ok, Message);
								UE_LOG(LogAssetTools, Warning, TEXT("%s"), *Message.ToString());
								OnNewImportRecord(ImportAssetType, FileExtension, bImportSucceeded, bImportWasCancelled, ImportStartTime);
								continue;
							}
							else
							{
								// succeed, recreate package since it has been deleted
								Pkg = CreatePackage( *PackageName);
								Pkg->MarkAsFullyLoaded();
							}
						}
						else
						{
							// User chose not to replace the package
							bImportWasCancelled = true;
							OnNewImportRecord(ImportAssetType, FileExtension, bImportSucceeded, bImportWasCancelled, ImportStartTime);
							continue;
						}
					}
				}
			}

			// Check for a package that was marked for delete in source control
			if(!CheckForDeletedPackage(Pkg))
			{
				OnNewImportRecord(ImportAssetType, FileExtension, bImportSucceeded, bImportWasCancelled, ImportStartTime);
				continue;
			}

			Factory->SetAutomatedAssetImportData(Params.ImportData);
			Factory->SetAssetImportTask(Params.AssetImportTask);

			ImportAssetType = Factory->ResolveSupportedClass();
			UObject* Result = Factory->ImportObject(ImportAssetType, Pkg, FName(*Name), RF_Public | RF_Standalone | RF_Transactional, Filename, nullptr, bImportWasCancelled);

			Factory->SetAutomatedAssetImportData(nullptr);
			Factory->SetAssetImportTask(nullptr);

			// Do not report any error if the operation was canceled.
			if(!bImportWasCancelled)
			{
				if(Result)
				{
					ReturnObjects.Add(Result);

					// Notify the asset registry
					FAssetRegistryModule::AssetCreated(Result);
					GEditor->BroadcastObjectReimported(Result);

					for (UObject* AdditionalResult : Factory->GetAdditionalImportedObjects())
					{
						ReturnObjects.Add(AdditionalResult);
					}

					bImportSucceeded = true;
				}
				else
				{
					const FText Message = FText::Format(LOCTEXT("ImportFailed_Generic", "Failed to import '{0}'. Failed to create asset '{1}'.\nPlease see Output Log for details."), FText::FromString(Filename), FText::FromString(PackageName));
					if(!bAutomatedImport)
					{
						FMessageDialog::Open(EAppMsgType::Ok, Message);
					}
					UE_LOG(LogAssetTools, Warning, TEXT("%s"), *Message.ToString());
				}
			}

			// Refresh the supported class.  Some factories (e.g. FBX) only resolve their type after reading the file
			ImportAssetType = Factory->ResolveSupportedClass();
			OnNewImportRecord(ImportAssetType, FileExtension, bImportSucceeded, bImportWasCancelled, ImportStartTime);
		}
		else
		{
			// A factory or extension was not found. The extension warning is above. If a factory was not found, the user likely canceled a factory configuration dialog.
		}

		bImportWasCancelled |= SlowTask.ShouldCancel();
		if (bImportWasCancelled)
		{
			UE_LOG(LogAssetTools, Log, TEXT("The import task was canceled."));
		}
	}

	// Clean up and remove the factories we created from the root set
	for(auto ExtensionIt = ExtensionToFactoriesMap.CreateConstIterator(); ExtensionIt; ++ExtensionIt)
	{
		for(auto FactoryIt = ExtensionIt.Value().CreateConstIterator(); FactoryIt; ++FactoryIt)
		{
			(*FactoryIt)->CleanUp();
			(*FactoryIt)->RemoveFromRoot();
		}
	}

	// Sync content browser to the newly created assets
	if(ReturnObjects.Num() && (bSyncToBrowser != false))
	{
		UAssetToolsImpl::Get().SyncBrowserToAssets(ReturnObjects);
	}

	return ReturnObjects;
}

void UAssetToolsImpl::ExportAssetsInternal(const TArray<UObject*>& ObjectsToExport, bool bPromptIndividualFilenames, const FString& ExportPath) const
{
	FString LastExportPath = !ExportPath.IsEmpty() ? ExportPath : FEditorDirectories::Get().GetLastDirectory(ELastDirectory::GENERIC_EXPORT);

	if (ObjectsToExport.Num() == 0)
	{
		return;
	}

	FString SelectedExportPath;
	if (!bPromptIndividualFilenames)
	{
		if (ExportPath.IsEmpty())
		{
			// If not prompting individual files, prompt the user to select a target directory.
			IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
			if (DesktopPlatform)
			{
				FString FolderName;
				const FString Title = NSLOCTEXT("UnrealEd", "ChooseADirectory", "Choose A Directory").ToString();
				const bool bFolderSelected = DesktopPlatform->OpenDirectoryDialog(
					FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
					Title,
					LastExportPath,
					FolderName
				);

				if (bFolderSelected)
				{
					SelectedExportPath = FolderName;
				}
			}
		}
		else
		{
			SelectedExportPath = ExportPath;
		}

		// Copy off the selected path for future export operations.
		LastExportPath = SelectedExportPath;
	}

	GWarn->BeginSlowTask(NSLOCTEXT("UnrealEd", "Exporting", "Exporting"), true);

	// Create an array of all available exporters.
	TArray<UExporter*> Exporters;
	ObjectTools::AssembleListOfExporters(Exporters);

	//Array to control the batch mode and the show options for the exporters that will be use by the selected assets
	TArray<UExporter*> UsedExporters;

	// Export the objects.
	bool bAnyObjectMissingSourceData = false;
	for (int32 Index = 0; Index < ObjectsToExport.Num(); Index++)
	{
		GWarn->StatusUpdate(Index, ObjectsToExport.Num(), FText::Format(NSLOCTEXT("UnrealEd", "Exportingf", "Exporting ({0} of {1})"), FText::AsNumber(Index), FText::AsNumber(ObjectsToExport.Num())));

		UObject* ObjectToExport = ObjectsToExport[Index];
		if (!ObjectToExport)
		{
			continue;
		}

		if (ObjectToExport->GetOutermost()->HasAnyPackageFlags(PKG_DisallowExport))
		{
			continue;
		}

		// Find all the exporters that can export this type of object and construct an export file dialog.
		TArray<FString> AllFileTypes;
		TArray<FString> AllExtensions;
		TArray<FString> PreferredExtensions;

		// Iterate in reverse so the most relevant file formats are considered first.
		for (int32 ExporterIndex = Exporters.Num() - 1; ExporterIndex >= 0; --ExporterIndex)
		{
			UExporter* Exporter = Exporters[ExporterIndex];
			if (Exporter->SupportedClass)
			{
				const bool bObjectIsSupported = Exporter->SupportsObject(ObjectToExport);
				if (bObjectIsSupported)
				{
					// Get a string representing of the exportable types.
					check(Exporter->FormatExtension.Num() == Exporter->FormatDescription.Num());
					check(Exporter->FormatExtension.IsValidIndex(Exporter->PreferredFormatIndex));
					for (int32 FormatIndex = Exporter->FormatExtension.Num() - 1; FormatIndex >= 0; --FormatIndex)
					{
						const FString& FormatExtension = Exporter->FormatExtension[FormatIndex];
						const FString& FormatDescription = Exporter->FormatDescription[FormatIndex];

						if (FormatIndex == Exporter->PreferredFormatIndex)
						{
							PreferredExtensions.Add(FormatExtension);
						}
						AllFileTypes.Add(FString::Printf(TEXT("%s (*.%s)|*.%s"), *FormatDescription, *FormatExtension, *FormatExtension));
						AllExtensions.Add(FString::Printf(TEXT("*.%s"), *FormatExtension));
					}
				}
			}
		}

		// Skip this object if no exporter found for this resource type.
		if (PreferredExtensions.Num() == 0)
		{
			continue;
		}

		// If FBX is listed, make that the most preferred option
		const FString PreferredExtension = TEXT("FBX");
		int32 ExtIndex = PreferredExtensions.Find(PreferredExtension);
		if (ExtIndex > 0)
		{
			PreferredExtensions.RemoveAt(ExtIndex);
			PreferredExtensions.Insert(PreferredExtension, 0);
		}
		FString FirstExtension = PreferredExtensions[0];

		// If FBX is listed, make that the first option here too, then compile them all into one string
		check(AllFileTypes.Num() == AllExtensions.Num())
			for (ExtIndex = 1; ExtIndex < AllFileTypes.Num(); ++ExtIndex)
			{
				const FString FileType = AllFileTypes[ExtIndex];
				if (FileType.Contains(PreferredExtension))
				{
					AllFileTypes.RemoveAt(ExtIndex);
					AllFileTypes.Insert(FileType, 0);

					const FString Extension = AllExtensions[ExtIndex];
					AllExtensions.RemoveAt(ExtIndex);
					AllExtensions.Insert(Extension, 0);
				}
			}
		FString FileTypes;
		FString Extensions;
		for (ExtIndex = 0; ExtIndex < AllFileTypes.Num(); ++ExtIndex)
		{
			if (FileTypes.Len())
			{
				FileTypes += TEXT("|");
			}
			FileTypes += AllFileTypes[ExtIndex];

			if (Extensions.Len())
			{
				Extensions += TEXT(";");
			}
			Extensions += AllExtensions[ExtIndex];
		}
		FileTypes = FString::Printf(TEXT("%s|All Files (%s)|%s"), *FileTypes, *Extensions, *Extensions);

		FString SaveFileName;
		if (bPromptIndividualFilenames)
		{
			TArray<FString> SaveFilenames;
			IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
			bool bSave = false;
			if (DesktopPlatform)
			{
				bSave = DesktopPlatform->SaveFileDialog(
					FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr),
					FText::Format(NSLOCTEXT("UnrealEd", "Save_F", "Save: {0}"), FText::FromString(ObjectToExport->GetName())).ToString(),
					*LastExportPath,
					*ObjectToExport->GetName(),
					*FileTypes,
					EFileDialogFlags::None,
					SaveFilenames
				);
			}

			if (!bSave)
			{
				int32 NumObjectsLeftToExport = ObjectsToExport.Num() - Index - 1;
				if (NumObjectsLeftToExport > 0)
				{
					const FText ConfirmText = FText::Format(NSLOCTEXT("UnrealEd", "AssetTools_ExportObjects_CancelRemaining", "Would you like to cancel exporting the next {0} files as well?"), FText::AsNumber(NumObjectsLeftToExport));
					if (EAppReturnType::Yes == FMessageDialog::Open(EAppMsgType::YesNo, ConfirmText))
					{
						break;
					}
				}
				continue;
			}
			SaveFileName = FString(SaveFilenames[0]);

			// Copy off the selected path for future export operations.
			LastExportPath = SaveFileName;
		}
		else
		{
			// Assemble a filename from the export directory and the object path.
			SaveFileName = SelectedExportPath;

			if (!FPackageName::IsShortPackageName(ObjectToExport->GetOutermost()->GetFName()))
			{
				// Determine the save file name from the long package name
				FString PackageName = ObjectToExport->GetOutermost()->GetName();
				if (PackageName.Left(1) == TEXT("/"))
				{
					// Trim the leading slash so the file manager doesn't get confused
					PackageName.MidInline(1, MAX_int32, false);
				}

				FPaths::NormalizeFilename(PackageName);
				SaveFileName /= PackageName;
			}
			else
			{
				// Assemble the path from the package name.
				SaveFileName /= ObjectToExport->GetOutermost()->GetName();
				SaveFileName /= ObjectToExport->GetName();
			}
			SaveFileName += FString::Printf(TEXT(".%s"), *FirstExtension);
			UE_LOG(LogAssetTools, Log, TEXT("Exporting \"%s\" to \"%s\""), *ObjectToExport->GetPathName(), *SaveFileName);
		}

		// Create the path, then make sure the target file is not read-only.
		const FString ObjectExportPath(FPaths::GetPath(SaveFileName));
		const bool bFileInSubdirectory = ObjectExportPath.Contains(TEXT("/"));
		if (bFileInSubdirectory && (!IFileManager::Get().MakeDirectory(*ObjectExportPath, true)))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(NSLOCTEXT("UnrealEd", "Error_FailedToMakeDirectory", "Failed to make directory {0}"), FText::FromString(ObjectExportPath)));
		}
		else if (IFileManager::Get().IsReadOnly(*SaveFileName))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(NSLOCTEXT("UnrealEd", "Error_CouldntWriteToFile_F", "Couldn't write to file '{0}'. Maybe file is read-only?"), FText::FromString(SaveFileName)));
		}
		else
		{
			// We have a writeable file.  Now go through that list of exporters again and find the right exporter and use it.
			TArray<UExporter*>	ValidExporters;

			for (int32 ExporterIndex = 0; ExporterIndex < Exporters.Num(); ++ExporterIndex)
			{
				UExporter* Exporter = Exporters[ExporterIndex];
				if (Exporter->SupportsObject(ObjectToExport))
				{
					check(Exporter->FormatExtension.Num() == Exporter->FormatDescription.Num());
					for (int32 FormatIndex = 0; FormatIndex < Exporter->FormatExtension.Num(); ++FormatIndex)
					{
						const FString& FormatExtension = Exporter->FormatExtension[FormatIndex];
						if (FCString::Stricmp(*FormatExtension, *FPaths::GetExtension(SaveFileName)) == 0 ||
							FCString::Stricmp(*FormatExtension, TEXT("*")) == 0)
						{
							ValidExporters.Add(Exporter);
							break;
						}
					}
				}
			}

			// Handle the potential of multiple exporters being found
			UExporter* ExporterToUse = NULL;
			if (ValidExporters.Num() == 1)
			{
				ExporterToUse = ValidExporters[0];
			}
			else if (ValidExporters.Num() > 1)
			{
				// Set up the first one as default
				ExporterToUse = ValidExporters[0];

				// ...but search for a better match if available
				for (int32 ExporterIdx = 0; ExporterIdx < ValidExporters.Num(); ExporterIdx++)
				{
					if (ValidExporters[ExporterIdx]->GetClass()->GetFName() == ObjectToExport->GetExporterName())
					{
						ExporterToUse = ValidExporters[ExporterIdx];
						break;
					}
				}
			}

			// If an exporter was found, use it.
			if (ExporterToUse)
			{
				const FScopedBusyCursor BusyCursor;

				if (!UsedExporters.Contains(ExporterToUse))
				{
					ExporterToUse->SetBatchMode(ObjectsToExport.Num() > 1 && !bPromptIndividualFilenames);
					ExporterToUse->SetCancelBatch(false);
					ExporterToUse->SetShowExportOption(true);
					ExporterToUse->AddToRoot();
					UsedExporters.Add(ExporterToUse);
				}

				UAssetExportTask* ExportTask = NewObject<UAssetExportTask>();
				FGCObjectScopeGuard ExportTaskGuard(ExportTask);
				ExportTask->Object = ObjectToExport;
				ExportTask->Exporter = ExporterToUse;
				ExportTask->Filename = SaveFileName;
				ExportTask->bSelected = false;
				ExportTask->bReplaceIdentical = true;
				ExportTask->bPrompt = false;
				ExportTask->bUseFileArchive = ObjectToExport->IsA(UPackage::StaticClass());
				ExportTask->bWriteEmptyFiles = false;

				UExporter::RunAssetExportTask(ExportTask);

				if (ExporterToUse->GetBatchMode() && ExporterToUse->GetCancelBatch())
				{
					//Exit the export file loop when there is a cancel all
					break;
				}
			}
		}
	}

	//Set back the default value for the all used exporters
	for (UExporter* UsedExporter : UsedExporters)
	{
		UsedExporter->SetBatchMode(false);
		UsedExporter->SetCancelBatch(false);
		UsedExporter->SetShowExportOption(true);
		UsedExporter->RemoveFromRoot();
	}
	UsedExporters.Empty();

	if (bAnyObjectMissingSourceData)
	{
		FMessageDialog::Open(EAppMsgType::Ok, NSLOCTEXT("UnrealEd", "Exporter_Error_SourceDataUnavailable", "No source data available for some objects.  See the log for details."));
	}

	GWarn->EndSlowTask();

	FEditorDirectories::Get().SetLastDirectory(ELastDirectory::GENERIC_EXPORT, LastExportPath);
}

UAssetToolsImpl& UAssetToolsImpl::Get()
{
	FAssetToolsModule& Module = FModuleManager::GetModuleChecked<FAssetToolsModule>("AssetTools");
	return static_cast<UAssetToolsImpl&>(Module.Get());
}

void UAssetToolsImpl::SyncBrowserToAssets(const TArray<UObject*>& AssetsToSync)
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	ContentBrowserModule.Get().SyncBrowserToAssets( AssetsToSync, /*bAllowLockedBrowsers=*/true );
}

void UAssetToolsImpl::SyncBrowserToAssets(const TArray<FAssetData>& AssetsToSync)
{
	FContentBrowserModule& ContentBrowserModule = FModuleManager::Get().LoadModuleChecked<FContentBrowserModule>("ContentBrowser");
	ContentBrowserModule.Get().SyncBrowserToAssets( AssetsToSync, /*bAllowLockedBrowsers=*/true );
}

bool UAssetToolsImpl::CheckForDeletedPackage(const UPackage* Package) const
{
	if ( ISourceControlModule::Get().IsEnabled() )
	{
		ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
		if ( SourceControlProvider.IsAvailable() )
		{
			FSourceControlStatePtr SourceControlState = SourceControlProvider.GetState(Package, EStateCacheUsage::ForceUpdate);
			if ( SourceControlState.IsValid() && SourceControlState->IsDeleted() )
			{
				// Creating an asset in a package that is marked for delete - revert the delete and check out the package
				if (!SourceControlProvider.Execute(ISourceControlOperation::Create<FRevert>(), Package))
				{
					// Failed to revert file which was marked for delete
					FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("RevertDeletedFileFailed", "Failed to revert package which was marked for delete."));
					return false;
				}

				if (!SourceControlProvider.Execute(ISourceControlOperation::Create<FCheckOut>(), Package))
				{
					// Failed to check out file
					FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("CheckOutFileFailed", "Failed to check out package"));
					return false;
				}
			}
		}
		else
		{
			FMessageLog EditorErrors("EditorErrors");
			EditorErrors.Warning(LOCTEXT( "DeletingNoSCCConnection", "Could not check for deleted file. No connection to source control available!"));
			EditorErrors.Notify();
		}
	}

	return true;
}

bool UAssetToolsImpl::CanCreateAsset(const FString& AssetName, const FString& PackageName, const FText& OperationText) const
{
	// @todo: These 'reason' messages are not localized strings!
	FText Reason;
	if (!FName(*AssetName).IsValidObjectName( Reason )
		|| !FPackageName::IsValidLongPackageName( PackageName, /*bIncludeReadOnlyRoots=*/false, &Reason ) )
	{
		FMessageDialog::Open( EAppMsgType::Ok, Reason );
		return false;
	}

	// We can not create assets that share the name of a map file in the same location
	if ( FEditorFileUtils::IsMapPackageAsset(PackageName) )
	{
		FMessageDialog::Open( EAppMsgType::Ok, FText::Format( LOCTEXT("AssetNameInUseByMap", "You can not create an asset named '{0}' because there is already a map file with this name in this folder."), FText::FromString( AssetName ) ) );
		return false;
	}

	// Find (or create!) the desired package for this object
	UPackage* Pkg = CreatePackage(*PackageName);

	// Handle fully loading packages before creating new objects.
	TArray<UPackage*> TopLevelPackages;
	TopLevelPackages.Add( Pkg );
	if( !UPackageTools::HandleFullyLoadingPackages( TopLevelPackages, OperationText ) )
	{
		// User aborted.
		return false;
	}

	// We need to test again after fully loading.
	if (!FName(*AssetName).IsValidObjectName( Reason )
		||	!FPackageName::IsValidLongPackageName( PackageName, /*bIncludeReadOnlyRoots=*/false, &Reason ) )
	{
		FMessageDialog::Open( EAppMsgType::Ok, Reason );
		return false;
	}

	// Check for an existing object
	UObject* ExistingObject = StaticFindObject( UObject::StaticClass(), Pkg, *AssetName );
	if( ExistingObject != nullptr )
	{
		// Object already exists in either the specified package or another package.  Check to see if the user wants
		// to replace the object.
		bool bWantReplace =
			EAppReturnType::Yes == FMessageDialog::Open(
				EAppMsgType::YesNo,
				EAppReturnType::No,
				FText::Format(
					NSLOCTEXT("UnrealEd", "ReplaceExistingObjectInPackage_F", "An object [{0}] of class [{1}] already exists in file [{2}].  Do you want to replace the existing object?  If you click 'Yes', the existing object will be deleted.  Otherwise, click 'No' and choose a unique name for your new object." ),
					FText::FromString(AssetName), FText::FromString(ExistingObject->GetClass()->GetName()), FText::FromString(PackageName) ) );

		if( bWantReplace )
		{
			// Replacing an object.  Here we go!
			// Delete the existing object
			bool bDeleteSucceeded = ObjectTools::DeleteSingleObject( ExistingObject );

			if (bDeleteSucceeded)
			{
				// Force GC so we can cleanly create a new asset (and not do an 'in place' replacement)
				CollectGarbage( GARBAGE_COLLECTION_KEEPFLAGS );

				// Old package will be GC'ed... create a new one here
				Pkg = CreatePackage(*PackageName);
				Pkg->MarkAsFullyLoaded();
			}
			else
			{
				// Notify the user that the operation failed b/c the existing asset couldn't be deleted
				FMessageDialog::Open( EAppMsgType::Ok,	FText::Format( NSLOCTEXT("DlgNewGeneric", "ContentBrowser_CannotDeleteReferenced", "{0} wasn't created.\n\nThe asset is referenced by other content."), FText::FromString( AssetName ) ) );
			}

			if( !bDeleteSucceeded || !IsUniqueObjectName( *AssetName, Pkg, Reason ) )
			{
				// Original object couldn't be deleted
				return false;
			}
		}
		else
		{
			// User chose not to replace the object; they'll need to enter a new name
			return false;
		}
	}

	// Check for a package that was marked for delete in source control
	if ( !CheckForDeletedPackage(Pkg) )
	{
		return false;
	}

	return true;
}

void UAssetToolsImpl::PerformMigratePackages(TArray<FName> PackageNamesToMigrate) const
{
	// Form a full list of packages to move by including the dependencies of the supplied packages
	TSet<FName> AllPackageNamesToMove;
	TSet<FString> ExternalObjectsPaths;
	{
		FScopedSlowTask SlowTask( PackageNamesToMigrate.Num(), LOCTEXT( "MigratePackages_GatheringDependencies", "Gathering Dependencies..." ) );
		SlowTask.MakeDialog();

		for ( auto PackageIt = PackageNamesToMigrate.CreateConstIterator(); PackageIt; ++PackageIt )
		{
			SlowTask.EnterProgressFrame();

			if ( !AllPackageNamesToMove.Contains(*PackageIt) )
			{
				AllPackageNamesToMove.Add(*PackageIt);
				RecursiveGetDependencies(*PackageIt, AllPackageNamesToMove, ExternalObjectsPaths);
			}
		}
	}
	
	// Confirm that there is at least one package to move 
	if (AllPackageNamesToMove.Num() == 0)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("MigratePackages_NoFilesToMove", "No files were found to move"));
		return;
	}

	// Prompt the user displaying all assets that are going to be migrated
	{
		const FText ReportMessage = LOCTEXT("MigratePackagesReportTitle", "The following assets will be migrated to another content folder.");
		TSharedPtr<TArray<ReportPackageData>> ReportPackages = MakeShareable(new TArray<ReportPackageData>);
		for ( auto PackageIt = AllPackageNamesToMove.CreateConstIterator(); PackageIt; ++PackageIt )
		{
			ReportPackages.Get()->Add({ (*PackageIt).ToString(), true });
		}
		SPackageReportDialog::FOnReportConfirmed OnReportConfirmed = SPackageReportDialog::FOnReportConfirmed::CreateUObject(this, &UAssetToolsImpl::MigratePackages_ReportConfirmed, ReportPackages);
		SPackageReportDialog::OpenPackageReportDialog(ReportMessage, *ReportPackages.Get(), OnReportConfirmed);
	}
}

void UAssetToolsImpl::MigratePackages_ReportConfirmed(TSharedPtr<TArray<ReportPackageData>> PackageDataToMigrate) const
{
	// Choose a destination folder
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	FString DestinationFolder;
	if ( ensure(DesktopPlatform) )
	{
		const void* ParentWindowWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);

		const FString Title = LOCTEXT("MigrateToFolderTitle", "Choose a destination Content folder").ToString();
		bool bFolderAccepted = false;
		while (!bFolderAccepted)
		{
			const bool bFolderSelected = DesktopPlatform->OpenDirectoryDialog(
				ParentWindowWindowHandle,
				Title,
				FEditorDirectories::Get().GetLastDirectory(ELastDirectory::GENERIC_EXPORT),
				DestinationFolder
				);

			if ( !bFolderSelected )
			{
				// User canceled, return
				return;
			}

			FEditorDirectories::Get().SetLastDirectory(ELastDirectory::GENERIC_EXPORT, DestinationFolder);
			FPaths::NormalizeFilename(DestinationFolder);
			if ( !DestinationFolder.EndsWith(TEXT("/")) )
			{
				DestinationFolder += TEXT("/");
			}

			// Verify that it is a content folder
			if ( DestinationFolder.EndsWith(TEXT("/Content/")) )
			{
				bFolderAccepted = true;
			}
			else
			{
				// The user chose a non-content folder. Confirm that this was their intention.
				const FText Message = FText::Format(LOCTEXT("MigratePackages_NonContentFolder", "{0} does not appear to be a game Content folder. Migrated content will only work properly if placed in a Content folder. Would you like to place your content here anyway?"), FText::FromString(DestinationFolder));
				EAppReturnType::Type Response = FMessageDialog::Open(EAppMsgType::YesNo, Message);
				bFolderAccepted = (Response == EAppReturnType::Yes);
			}
		}
	}
	else
	{
		// Not on a platform that supports desktop functionality
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("NoDesktopPlatform", "Error: This platform does not support a file dialog."));
		return;
	}

	// Build a list of packages to handle
	TSet<FName> AllPackageNamesToMove;
	for (auto PackageDataIt = PackageDataToMigrate->CreateConstIterator(); PackageDataIt; ++PackageDataIt)
	{
		if (PackageDataIt->bShouldMigratePackage)
		{
			AllPackageNamesToMove.Add(FName(PackageDataIt->Name));
		}
	}

	bool bAbort = false;
	FMessageLog MigrateLog("AssetTools");

	// Determine if the destination is a project content folder or a plugin content folder
	TArray<FString> ProjectFiles;
	IFileManager::Get().FindFiles(ProjectFiles, *(DestinationFolder + TEXT("../")), TEXT("uproject"));
	bool bIsDestinationAProject = !ProjectFiles.IsEmpty();

	// Associate each Content folder in the target Plugin hierarchy to a content root string in UFS
	TMap<FName, FString> DestContentRootsToFolders;

	// Assets in /Game always map directly to the destination
	DestContentRootsToFolders.Add(FName(TEXT("/Game")), DestinationFolder);

	// If our destination is a project, it could have plugins...
	if (bIsDestinationAProject)
	{
		// Find all "Content" folders under the destination ../Plugins directory
		TArray<FString> ContentFolders;
		IFileManager::Get().FindFilesRecursive(ContentFolders, *(DestinationFolder + TEXT("../Plugins/")), TEXT("Content"), false, true);

		for (const FString& Folder : ContentFolders)
		{
			// Parse the parent folder of .../Content from "Folder"
			FString Path, Content, Root;
			bool bSplitContent = Folder.Split(TEXT("/"), &Path, &Content, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			bool bSplitParentPath = Path.Split(TEXT("/"), nullptr, &Root, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
			if (!bSplitContent || !bSplitParentPath)
			{
				MigrateLog.Error(FText::Format(LOCTEXT("MigratePackages_NoMountPointFolder", "Unable to determine mount point for folder {0}"), FText::FromString(Folder)));
				bAbort = true;
				continue;
			}

			// Determine this folder name to be a content root in the destination
			FString DestContentRoot = FString(TEXT("/")) + Root;
			FString DestContentFolder = FPaths::ConvertRelativePathToFull(Folder + TEXT("/"));
			DestContentRootsToFolders.Add(FName(DestContentRoot), DestContentFolder);
		}
	}

	if (bAbort)
	{
		MigrateLog.Notify();
		return;
	}

	// Check if the root of any of the packages to migrate have no destination
	TArray<FName> LostPackages;
	TSet<FName> LostPackageRoots;
	for (const FName& PackageName : AllPackageNamesToMove)
	{
		// Acquire the mount point for this package
		FString Folder = TEXT("/") + FPackageName::GetPackageMountPoint(PackageName.ToString()).ToString();

		// If this is /Game package, it doesn't need special handling and is simply bound for the destination, continue
		if (Folder == TEXT("/Game"))
		{
			continue;
		}

		// Resolve the disk folder of this package's mount point so we compare directory names directly
		//  We do this FileSystem to FileSystem compare instead of mount point (UFS) to content root (FileSystem)
		//  The mount point name likely comes from the FriendlyName in the uplugin, or the uplugin basename
		//  What's important here is that we succeed in finding _the same plugin_ between a copy/pasted project
		if (!FPackageName::TryConvertLongPackageNameToFilename(Folder, Folder))
		{
			MigrateLog.Error(FText::Format(LOCTEXT("MigratePackages_NoContentFolder", "Unable to determine content folder for asset {0}"), FText::FromString(PackageName.ToString())));
			bAbort = true;
			continue;
		}
		Folder.RemoveFromEnd(TEXT("/"));

		// Parse the parent folder of .../Content from "Folder"
		FString Path, Content, Root;
		bool bSplitContent = Folder.Split(TEXT("/"), &Path, &Content, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		bool bSplitParentPath = Path.Split(TEXT("/"), nullptr, &Root, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (!bSplitContent || !bSplitParentPath)
		{
			MigrateLog.Error(FText::Format(LOCTEXT("MigratePackages_NoMountPointPackage", "Unable to determine mount point for package {0}"), FText::FromString(PackageName.ToString())));
			bAbort = true;
			continue;
		}

		// Check to see if the content root exists in the destination, otherwise it's "Lost"
		FString SrcContentRoot = FString(TEXT("/")) + Root;
		FName SrcContentRootName(SrcContentRoot);
		if (!DestContentRootsToFolders.Find(SrcContentRootName))
		{
			LostPackages.Add(PackageName);
			LostPackageRoots.Add(SrcContentRootName);
		}
	}

	if (bAbort)
	{
		MigrateLog.Notify();
		return;
	}

	// If some packages don't have a matching content root in the destination, prompt for desired behavior
	if (!LostPackages.IsEmpty())
	{
		FString LostPackageRootsString;
		for (const FName& PackageRoot : LostPackageRoots)
		{
			LostPackageRootsString += FString(TEXT("\n\t")) + PackageRoot.ToString();
		}

		// Prompt to consolidate to a migration folder
		FText Prompt = FText::Format(LOCTEXT("MigratePackages_ConsolidateToTemp", "Some selected assets don't have a corresponding content root in the destination.{0}\n\nWould you like to migrate all selected assets into a folder with consolidated references? Without migrating into a folder the assets in the above roots will not be migrated."), FText::FromString(LostPackageRootsString));
		switch (FMessageDialog::Open(EAppMsgType::YesNoCancel, Prompt))
		{
		case EAppReturnType::Yes:
			// No op
			break;
		case EAppReturnType::No:
			LostPackages.Reset();
			break;
		case EAppReturnType::Cancel:
			return;
		}
	}

	// This will be used to tidy up the temp folder after we copy the migrated assets
	FString SrcDiskFolderFilename;

	// Fixing up references requires resaving packages to a temporary location
	if (!LostPackages.IsEmpty())
	{
		// Resolve the packages to migrate to assets
		TArray<UObject*> SrcObjects;
		for (const FName& SrcPackage : AllPackageNamesToMove)
		{
			UPackage* LoadedPackage = UPackageTools::LoadPackage(SrcPackage.ToString());
			if (!LoadedPackage)
			{
				MigrateLog.Error(FText::Format(LOCTEXT("MigratePackages_FailedToLoadPackage", "Failed to load package {0}"), FText::FromString(SrcPackage.ToString())));
				bAbort = true;
				continue;
			}

			UObject* Asset = LoadedPackage->FindAssetInPackage();
			if (Asset)
			{
				SrcObjects.Add(Asset);
			}
			else
			{
				MigrateLog.Warning(FText::Format(LOCTEXT("MigratePackages_PackageHasNoAsset", "Package {0} has no asset in it"), FText::FromString(SrcPackage.ToString())));
			}
		}

		if (bAbort)
		{
			MigrateLog.Notify();
			return;
		}

		// Query the user for a folder to migrate assets into. This folder will exist temporary in this project, and is the destination for migration in the target project
		FString FolderName;
		bool bIsOkButtonEnabled = true;
		TSharedRef<SEditableTextBox> EditableTextBox = SNew(SEditableTextBox)
			.Text(FText::FromString(TEXT("Migrated")))
			.OnVerifyTextChanged_Lambda([&bIsOkButtonEnabled](const FText& InNewText, FText& OutErrorMessage) -> bool
				{
					if (InNewText.ToString().Contains(TEXT("/")))
					{
						OutErrorMessage = LOCTEXT("Migrated_CannotContainSlashes", "Cannot use a slash in a folder name.");

						// Disable Ok if the string is invalid
						bIsOkButtonEnabled = false;
						return false;
					}

					// Enable Ok if the string is valid
					bIsOkButtonEnabled = true;
					return true;
				})
			.OnTextCommitted_Lambda([&FolderName](const FText& NewValue, ETextCommit::Type)
				{
					// Set the result if they modified the text
					FolderName = NewValue.ToString();
				});

		// Set the result if they just click Ok
		SGenericDialogWidget::FArguments FolderDialogArguments;
		FolderDialogArguments.OnOkPressed_Lambda([&EditableTextBox, &FolderName]()
			{
				FolderName = EditableTextBox->GetText().ToString();
			});

		// Present the Dialog
		SGenericDialogWidget::OpenDialog(LOCTEXT("MigratePackages_FolderName", "Folder for Migrated Assets"),
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(5.0f)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("MigratePackages_SpecifyConsolidateFolder", "Please specify a new folder name to consolidate the assets into."))
			]
			+ SVerticalBox::Slot()
				.Padding(5.0f)
			[
				SNew(SSpacer)
			]
			+ SVerticalBox::Slot()
				.AutoHeight()
				.Padding(5.0f)
			[
				EditableTextBox
			],
			FolderDialogArguments, true);

		// Sanity the user input
		if (FolderName.IsEmpty())
		{
			return;
		}

		// Remove forbidden characters
		FolderName = FolderName.Replace(TEXT("/"), TEXT(""));

		// Verify that we don't have any assets that exist where we want to perform our consolidation
		//  We could do the same check at the destination, but the package copy code will happily overwrite,
		//  and it seems reasonable someone would want to Migrate several times if snapshotting changes from another project.
		//  So for now we don't necessitate that the destination content folder be missing as well.
		FString SrcUfsFolderName = FolderName;
		SrcUfsFolderName.InsertAt(0, TEXT("/Game/"));
		SrcDiskFolderFilename = FPackageName::LongPackageNameToFilename(SrcUfsFolderName, TEXT(""));
		if (IFileManager::Get().DirectoryExists(*SrcDiskFolderFilename))
		{
			const FText Message = FText::Format(LOCTEXT("MigratePackages_InvalidMigrateFolder", "{0} exists on disk in the source project, and cannot be used to consolidate assets."), FText::FromString(SrcDiskFolderFilename));
			EAppReturnType::Type Response = FMessageDialog::Open(EAppMsgType::Ok, Message);
			return;
		}

		// To handle complex references and assets in different Plugins, we must first duplicate to temp packages, then migrate those temps
		TArray<UObject*> TempObjects;
		ObjectTools::DuplicateObjects(SrcObjects, TEXT(""), SrcUfsFolderName, /*bOpenDialog=*/false, &TempObjects);
		TMap<UObject*, UObject*> ReplacementMap;
		for (int i=0; i<SrcObjects.Num(); ++i)
		{
			ReplacementMap.Add(SrcObjects[i], TempObjects[i]);
		}

		// Save fixed up packages to the migrated folder, and update the set of files to copy to be those migrated packages
		{
			TSet<FName> NewPackageNamesToMove;
			for (int i=0; i<TempObjects.Num(); ++i)
			{
				UObject* TempObject = TempObjects[i];

				// Fixup references in each package, save them, and update the source of our copy operation
				FArchiveReplaceObjectRef<UObject> ReplaceAr(TempObject, ReplacementMap, EArchiveReplaceObjectFlags::IgnoreOuterRef | EArchiveReplaceObjectFlags::IgnoreArchetypeRef);

				// Calculate the file path to the new, migrated package
				FString const TempPackageName = TempObject->GetPackage()->GetName();
				FString const TempPackageFilename = FPackageName::LongPackageNameToFilename(TempPackageName, FPackageName::GetAssetPackageExtension());

				// Save it
				FSavePackageArgs SaveArgs;
				GEditor->Save(TempObject->GetPackage(), /*InAsset=*/nullptr, *TempPackageFilename, SaveArgs);

				NewPackageNamesToMove.Add(FName(TempPackageName));
			}

			AllPackageNamesToMove = NewPackageNamesToMove;
		}
	}

	bool bUserCanceled = false;

	// Copy all specified assets and their dependencies to the destination folder
	FScopedSlowTask SlowTask( 2, LOCTEXT( "MigratePackages_CopyingFiles", "Copying Files..." ) );
	SlowTask.MakeDialog();

	EAppReturnType::Type LastResponse = EAppReturnType::Yes;
	TArray<FString> SuccessfullyCopiedFiles;
	TArray<FString> SuccessfullyCopiedPackages;
	FString CopyErrors;

	SlowTask.EnterProgressFrame();
	{
		FScopedSlowTask LoopProgress(PackageDataToMigrate.Get()->Num());
		for ( const FName& PackageNameToMove : AllPackageNamesToMove )
		{
			LoopProgress.EnterProgressFrame();

			const FString& PackageName = PackageNameToMove.ToString();
			FString SrcFilename;
			
			if (!FPackageName::DoesPackageExist(PackageName, &SrcFilename))
			{
				const FText ErrorMessage = FText::Format(LOCTEXT("MigratePackages_PackageMissing", "{0} does not exist on disk."), FText::FromString(PackageName));
				UE_LOG(LogAssetTools, Warning, TEXT("%s"), *ErrorMessage.ToString());
				CopyErrors += ErrorMessage.ToString() + LINE_TERMINATOR;
			}
			else if (SrcFilename.Contains(FPaths::EngineContentDir()))
			{
				const FString LeafName = SrcFilename.Replace(*FPaths::EngineContentDir(), TEXT("Engine/"));
				CopyErrors += FText::Format(LOCTEXT("MigratePackages_EngineContent", "Unable to migrate Engine asset {0}. Engine assets cannot be migrated."), FText::FromString(LeafName)).ToString() + LINE_TERMINATOR;
			}
			else
			{
				bool bFileOKToCopy = true;

				FString Path = PackageNameToMove.ToString();
				FString PackageRoot;
				Path.RemoveFromStart(TEXT("/"));
				Path.Split("/", &PackageRoot, &Path, ESearchCase::IgnoreCase, ESearchDir::FromStart);
				PackageRoot = TEXT("/") + PackageRoot;

				FString DestFilename;
				TMap<FName, FString>::ValueType* DestRootFolder = DestContentRootsToFolders.Find(FName(PackageRoot));
				if (ensure(DestRootFolder))
				{
					DestFilename = *DestRootFolder;
				}
				else
				{
					continue;
				}

				FString SubFolder;
				if ( SrcFilename.Split( TEXT("/Content/"), nullptr, &SubFolder ) )
				{
					DestFilename += *SubFolder;

					if ( IFileManager::Get().FileSize(*DestFilename) > 0 )
					{
						// The destination file already exists! Ask the user what to do.
						EAppReturnType::Type Response;
						if ( LastResponse == EAppReturnType::YesAll || LastResponse == EAppReturnType::NoAll )
						{
							Response = LastResponse;
						}
						else
						{
							const FText Message = FText::Format( LOCTEXT("MigratePackages_AlreadyExists", "An asset already exists at location {0} would you like to overwrite it?"), FText::FromString(DestFilename) );
							Response = FMessageDialog::Open( EAppMsgType::YesNoYesAllNoAllCancel, Message );
							if ( Response == EAppReturnType::Cancel )
							{
								// The user chose to cancel mid-operation. Break out.
								bUserCanceled = true;
								break;
							}
							LastResponse = Response;
						}

						const bool bWantOverwrite = Response == EAppReturnType::Yes || Response == EAppReturnType::YesAll;
						if( !bWantOverwrite )
						{
							// User chose not to replace the package
							bFileOKToCopy = false;
						}
					}
				}
				else
				{
					// Couldn't find Content folder in source path
					bFileOKToCopy = false;
				}

				if ( bFileOKToCopy )
				{
					if ( IFileManager::Get().Copy(*DestFilename, *SrcFilename) == COPY_OK )
					{
						SuccessfullyCopiedPackages.Add(PackageName);
						SuccessfullyCopiedFiles.Add(DestFilename);
					}
					else
					{
						UE_LOG(LogAssetTools, Warning, TEXT("Failed to copy %s to %s while migrating assets"), *SrcFilename, *DestFilename);
						CopyErrors += SrcFilename + LINE_TERMINATOR;
					}
				}
			}
		}
	}

	// If we are consolidating lost packages, we are copying temporary packages, so clean them up.
	if (!LostPackages.IsEmpty())
	{
		TArray<FAssetData> AssetsToDelete;
		for (const FName& PackageNameToMove : AllPackageNamesToMove)
		{
			AssetsToDelete.Add(FAssetData(UPackageTools::LoadPackage(PackageNameToMove.ToString())));
		}

		ObjectTools::DeleteAssets(AssetsToDelete, /*bShowConfirmation=*/false);

		if (!IFileManager::Get().DeleteDirectory(*SrcDiskFolderFilename))
		{
			UE_LOG(LogAssetTools, Warning, TEXT("Failed to delete temporary directory %s while migrating assets"), *SrcDiskFolderFilename);
			CopyErrors += SrcDiskFolderFilename + LINE_TERMINATOR;
		}
	}

	FString SourceControlErrors;
	SlowTask.EnterProgressFrame();

	if ( !bUserCanceled && SuccessfullyCopiedFiles.Num() > 0 )
	{
		// attempt to add files to source control (this can quite easily fail, but if it works it is very useful)
		if(GetDefault<UEditorLoadingSavingSettings>()->bSCCAutoAddNewFiles)
		{
			if(ISourceControlModule::Get().IsEnabled())
			{
				ISourceControlProvider& SourceControlProvider = ISourceControlModule::Get().GetProvider();
				if(SourceControlProvider.Execute(ISourceControlOperation::Create<FMarkForAdd>(), SuccessfullyCopiedFiles) == ECommandResult::Failed)
				{
					FScopedSlowTask LoopProgress(SuccessfullyCopiedFiles.Num());

					for(auto FileIt(SuccessfullyCopiedFiles.CreateConstIterator()); FileIt; FileIt++)
					{
						LoopProgress.EnterProgressFrame();
						if(!SourceControlProvider.GetState(*FileIt, EStateCacheUsage::Use)->IsAdded())
						{
							SourceControlErrors += FText::Format(LOCTEXT("MigratePackages_SourceControlError", "{0} could not be added to source control"), FText::FromString(*FileIt)).ToString();
							SourceControlErrors += LINE_TERMINATOR;
						}
					}
				}
			}
		}
	}

	FText LogMessage = FText::FromString(TEXT("Content migration completed successfully!"));
	EMessageSeverity::Type Severity = EMessageSeverity::Info;
	if ( CopyErrors.Len() > 0 || SourceControlErrors.Len() > 0 )
	{
		FString ErrorMessage;
		Severity = EMessageSeverity::Error;
		if( CopyErrors.Len() > 0 )
		{
			MigrateLog.NewPage( LOCTEXT("MigratePackages_CopyErrorsPage", "Copy Errors") );
			MigrateLog.Error(FText::FromString(*CopyErrors));
			ErrorMessage += FText::Format(LOCTEXT( "MigratePackages_CopyErrors", "Copied {0} files. Some content could not be copied."), FText::AsNumber(SuccessfullyCopiedPackages.Num())).ToString();
		}
		if( SourceControlErrors.Len() > 0 )
		{
			MigrateLog.NewPage( LOCTEXT("MigratePackages_SourceControlErrorsListPage", "Source Control Errors") );
			MigrateLog.Error(FText::FromString(*SourceControlErrors));
			ErrorMessage += LINE_TERMINATOR;
			ErrorMessage += LOCTEXT( "MigratePackages_SourceControlErrorsList", "Some files reported source control errors.").ToString();
		}
		if ( SuccessfullyCopiedPackages.Num() > 0 )
		{
			MigrateLog.NewPage( LOCTEXT("MigratePackages_CopyErrorsSuccesslistPage", "Copied Successfully") );
			MigrateLog.Info(FText::FromString(*SourceControlErrors));
			ErrorMessage += LINE_TERMINATOR;
			ErrorMessage += LOCTEXT( "MigratePackages_CopyErrorsSuccesslist", "Some files were copied successfully.").ToString();
			for ( auto FileIt = SuccessfullyCopiedPackages.CreateConstIterator(); FileIt; ++FileIt )
			{
				if(FileIt->Len()>0)
				{
					MigrateLog.Info(FText::FromString(*FileIt));
				}
			}
		}
		LogMessage = FText::FromString(ErrorMessage);
	}
	else if (bUserCanceled)
	{
		LogMessage = LOCTEXT("MigratePackages_CanceledPage", "Content migration was canceled.");
	}
	else if ( !bUserCanceled )
	{
		MigrateLog.NewPage( LOCTEXT("MigratePackages_CompletePage", "Content migration completed successfully!") );
		for ( auto FileIt = SuccessfullyCopiedPackages.CreateConstIterator(); FileIt; ++FileIt )
		{
			if(FileIt->Len()>0)
			{
				MigrateLog.Info(FText::FromString(*FileIt));
			}
		}
	}
	MigrateLog.Notify(LogMessage, Severity, true);
}

void UAssetToolsImpl::RecursiveGetDependencies(const FName& PackageName, TSet<FName>& AllDependencies, TSet<FString>& OutExternalObjectsPaths) const
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FName> Dependencies;
	AssetRegistryModule.Get().GetDependencies(PackageName, Dependencies);
	
	for ( auto DependsIt = Dependencies.CreateConstIterator(); DependsIt; ++DependsIt )
	{
		FString DependencyName = (*DependsIt).ToString();

		const bool bIsEnginePackage = DependencyName.StartsWith(TEXT("/Engine"));
		const bool bIsScriptPackage = DependencyName.StartsWith(TEXT("/Script"));
		if ( !bIsEnginePackage && !bIsScriptPackage )
		{
			if (!AllDependencies.Contains(*DependsIt))
			{
				AllDependencies.Add(*DependsIt);
				RecursiveGetDependencies(*DependsIt, AllDependencies, OutExternalObjectsPaths);
			}
		}
	}

	// Handle Specific External Actors use case (only used for the Migrate path for now)
	// todo: revisit how to handle those in a more generic way.
	TArray<FAssetData> Assets;
	if (AssetRegistryModule.Get().GetAssetsByPackageName(PackageName, Assets))
	{
		for (const FAssetData& AssetData : Assets)
		{
			if (AssetData.GetClass() && AssetData.GetClass()->IsChildOf<UWorld>())
			{
				TArray<FString> ExternalObjectsPaths = ULevel::GetExternalObjectsPaths(PackageName.ToString());
				for (const FString& ExternalObjectsPath : ExternalObjectsPaths)
				{
					if (!ExternalObjectsPath.IsEmpty() && !OutExternalObjectsPaths.Contains(ExternalObjectsPath))
					{
						OutExternalObjectsPaths.Add(ExternalObjectsPath);
						AssetRegistryModule.Get().ScanPathsSynchronous({ ExternalObjectsPath }, /*bForceRescan*/true, /*bIgnoreBlackListScanFilters*/true);

						TArray<FAssetData> ExternalObjectAssets;
						AssetRegistryModule.Get().GetAssetsByPath(FName(*ExternalObjectsPath), ExternalObjectAssets, /*bRecursive*/true);

						for (const FAssetData& ExternalObjectAsset : ExternalObjectAssets)
						{
							AllDependencies.Add(ExternalObjectAsset.PackageName);
							RecursiveGetDependencies(ExternalObjectAsset.PackageName, AllDependencies, OutExternalObjectsPaths);
						}
					}
				}
			}
		}
	}
}

void UAssetToolsImpl::RecursiveGetDependenciesAdvanced(const FName& PackageName, FAdvancedCopyParams& CopyParams, TArray<FName>& AllDependencies, TMap<FName, FName>& DependencyMap, const UAdvancedCopyCustomization* CopyCustomization, TArray<FAssetData>& OptionalAssetData) const
{
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::Get().LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FName> Dependencies;
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
	// We found an asset
	if (OptionalAssetData.Num() > 0)
	{
		AssetRegistryModule.Get().GetDependencies(PackageName, Dependencies);
		for (const FName& Dep : Dependencies)
		{
			if (!AllDependencies.Contains(Dep) && FPackageName::IsValidLongPackageName(Dep.ToString(), false))
			{
				TArray<FAssetData> DependencyAssetData;
				AssetRegistry.GetAssetsByPackageName(Dep, DependencyAssetData, true);
				FARFilter ExclusionFilter = CopyCustomization->GetARFilter();
				AssetRegistry.UseFilterToExcludeAssets(DependencyAssetData, ExclusionFilter);
				if (DependencyAssetData.Num() > 0)
				{
					AllDependencies.Add(Dep);
					DependencyMap.Add(Dep, PackageName);
					RecursiveGetDependenciesAdvanced(Dep, CopyParams, AllDependencies, DependencyMap, CopyCustomization, DependencyAssetData);
				}
			}
		}
	}
	else
	{
		TArray<FAssetData> PathAssetData;
		// We found a folder containing assets
		if (AssetRegistry.HasAssets(PackageName) && AssetRegistry.GetAssetsByPath(PackageName, PathAssetData))
		{
			FARFilter ExclusionFilter = UAdvancedCopyCustomization::StaticClass()->GetDefaultObject<UAdvancedCopyCustomization>()->GetARFilter();
			AssetRegistry.UseFilterToExcludeAssets(PathAssetData, ExclusionFilter);
			for(const FAssetData& Asset : PathAssetData)
			{
				AllDependencies.Add(*Asset.GetPackage()->GetName());
				// If we should check the assets we found for dependencies
				if (CopyParams.bShouldCheckForDependencies)
				{
					RecursiveGetDependenciesAdvanced(FName(*Asset.GetPackage()->GetName()), CopyParams, AllDependencies, DependencyMap, CopyCustomization, PathAssetData);
				}
			}
		}
		
		// Always get subpaths
		{
			TArray<FString> SubPaths;
			AssetRegistry.GetSubPaths(PackageName.ToString(), SubPaths, false);
			for (const FString& SubPath : SubPaths)
			{
				TArray<FAssetData> EmptyArray;
				RecursiveGetDependenciesAdvanced(FName(*SubPath), CopyParams, AllDependencies, DependencyMap, CopyCustomization, EmptyArray);
			}

		}
	}
}

void UAssetToolsImpl::FixupReferencers(const TArray<UObjectRedirector*>& Objects, bool bCheckoutDialogPrompt, ERedirectFixupMode FixupMode) const
{
	AssetFixUpRedirectors->FixupReferencers(Objects, bCheckoutDialogPrompt, FixupMode);
}

bool UAssetToolsImpl::IsFixupReferencersInProgress() const
{
	return AssetFixUpRedirectors->IsFixupReferencersInProgress();
}

void UAssetToolsImpl::OpenEditorForAssets(const TArray<UObject*>& Assets)
{
#if WITH_EDITOR
	GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAssets(Assets);
#endif
}

void UAssetToolsImpl::ConvertVirtualTextures(const TArray<UTexture2D *>& Textures, bool bConvertBackToNonVirtual, const TArray<UMaterial *>* RelatedMaterials /* = nullptr */) const
{
	FVTConversionWorker VirtualTextureConversionWorker(bConvertBackToNonVirtual);
	VirtualTextureConversionWorker.UserTextures = Textures;
	//We want all given texture to be added, so we put a minimum texture size of 0
	VirtualTextureConversionWorker.FilterList(0);
	if (RelatedMaterials)
	{
		VirtualTextureConversionWorker.Materials.Append(*RelatedMaterials);
	}

	VirtualTextureConversionWorker.DoConvert();
}

void UAssetToolsImpl::BeginAdvancedCopyPackages(const TArray<FName>& InputNamesToCopy, const FString& TargetPath) const
{
	// Packages must be saved for the migration to work
	const bool bPromptUserToSave = true;
	const bool bSaveMapPackages = true;
	const bool bSaveContentPackages = true;
	if (FEditorFileUtils::SaveDirtyPackages(bPromptUserToSave, bSaveMapPackages, bSaveContentPackages))
	{
		IAssetRegistry& AssetRegistry = FAssetRegistryModule::GetRegistry();
		if (AssetRegistry.IsLoadingAssets())
		{
			// Open a dialog asking the user to wait while assets are being discovered
			SDiscoveringAssetsDialog::OpenDiscoveringAssetsDialog(
				SDiscoveringAssetsDialog::FOnAssetsDiscovered::CreateUObject(this, &UAssetToolsImpl::PerformAdvancedCopyPackages, InputNamesToCopy, TargetPath)
			);
		}
		else
		{
			// Assets are already discovered, perform the migration now
			PerformAdvancedCopyPackages(InputNamesToCopy, TargetPath);
		}
	}
}

TArray<FName> UAssetToolsImpl::ExpandAssetsAndFoldersToJustAssets(TArray<FName> SelectedAssetAndFolderNames) const
{
	IAssetRegistry& AssetRegistry = FAssetRegistryModule::GetRegistry();

	TSet<FName> ExpandedAssets;
	for (auto NameIt = SelectedAssetAndFolderNames.CreateIterator(); NameIt; ++NameIt)
	{
		FName OriginalName = *NameIt;
		const FString& OriginalNameString = OriginalName.ToString();
		if (!FPackageName::DoesPackageExist(OriginalNameString))
		{
			TArray<FAssetData> AssetsInFolder;
			AssetRegistry.GetAssetsByPath(OriginalName, AssetsInFolder, true, false);
			for (const FAssetData& Asset : AssetsInFolder)
			{
				ExpandedAssets.Add(Asset.PackageName);
			}

			NameIt.RemoveCurrent();
		}
		else
		{
			ExpandedAssets.Add(OriginalName);
		}
	}

	TArray<FName> ExpandedAssetArray;
	for (FName ExpandedAsset : ExpandedAssets)
	{
		ExpandedAssetArray.Add(ExpandedAsset);
	}

	return ExpandedAssetArray;
}

void UAssetToolsImpl::PerformAdvancedCopyPackages(TArray<FName> SelectedAssetAndFolderNames, FString TargetPath) const
{	
	TargetPath.RemoveFromEnd(TEXT("/"));

	//TArray<FName> ExpandedAssets = ExpandAssetsAndFoldersToJustAssets(SelectedAssetAndFolderNames);
	FAdvancedCopyParams CopyParams = FAdvancedCopyParams(SelectedAssetAndFolderNames, TargetPath);
	CopyParams.bShouldCheckForDependencies = true;

	// Suppress UI if we're running in unattended mode
	if (FApp::IsUnattended())
	{
		CopyParams.bShouldSuppressUI = true;
	}

	for (auto NameIt = SelectedAssetAndFolderNames.CreateConstIterator(); NameIt; ++NameIt)
	{
		UAdvancedCopyCustomization* CopyCustomization = nullptr;

		const UAssetToolsSettings* Settings = GetDefault<UAssetToolsSettings>();
		FName OriginalName = *NameIt;
		const FString& OriginalNameString = OriginalName.ToString();
		FString SrcFilename;
		UObject* ExistingObject = nullptr;

		if (FPackageName::DoesPackageExist(OriginalNameString, &SrcFilename))
		{
			UPackage* Pkg = LoadPackage(nullptr, *OriginalNameString, LOAD_None);
			if (Pkg)
			{
				FString Name = ObjectTools::SanitizeObjectName(FPaths::GetBaseFilename(SrcFilename));
				ExistingObject = StaticFindObject(UObject::StaticClass(), Pkg, *Name);
			}
		}

		if (ExistingObject)
		{
			// Try to find the customization in the settings
			for (const FAdvancedCopyMap& Customization : Settings->AdvancedCopyCustomizations)
			{
				if (Customization.ClassToCopy.GetAssetPathString() == ExistingObject->GetClass()->GetPathName())
				{
					UClass* CustomizationClass = Customization.AdvancedCopyCustomization.TryLoadClass<UAdvancedCopyCustomization>();
					if (CustomizationClass)
					{
						CopyCustomization = CustomizationClass->GetDefaultObject<UAdvancedCopyCustomization>();
					}
				}
			}
		}

		// If not able to find class in settings, fall back to default customization
		// by default, folders will use the default customization
		if (CopyCustomization == nullptr)
		{
			CopyCustomization = UAdvancedCopyCustomization::StaticClass()->GetDefaultObject<UAdvancedCopyCustomization>();
		}

		CopyParams.AddCustomization(CopyCustomization);
	}

	InitAdvancedCopyFromCopyParams(CopyParams);
}


void UAssetToolsImpl::InitAdvancedCopyFromCopyParams(FAdvancedCopyParams CopyParams) const
{
	TArray<TMap<FName, FName>> CompleteDependencyMap;
	TArray<TMap<FString, FString>> CompleteDestinationMap;

	TArray<FName> SelectedPackageNames = CopyParams.GetSelectedPackageOrFolderNames();

	FScopedSlowTask SlowTask(SelectedPackageNames.Num(), LOCTEXT("AdvancedCopyPrepareSlowTask", "Preparing Files for Advanced Copy"));
	SlowTask.MakeDialog();

	TArray<UAdvancedCopyCustomization*> CustomizationsToUse = CopyParams.GetCustomizationsToUse();

	for (int32 CustomizationIndex = 0; CustomizationIndex < CustomizationsToUse.Num(); CustomizationIndex++)
	{
		FName Package = SelectedPackageNames[CustomizationIndex];
		SlowTask.EnterProgressFrame(1, FText::Format(LOCTEXT("AdvancedCopy_PreparingDependencies", "Preparing dependencies for {0}"), FText::FromString(Package.ToString())));

		UAdvancedCopyCustomization* CopyCustomization = CustomizationsToUse[CustomizationIndex];
		// Give the customization a chance to edit the copy parameters
		CopyCustomization->EditCopyParams(CopyParams);
		TMap<FName, FName> DependencyMap = TMap<FName, FName>();
		TArray<FName> PackageNamesToCopy;

		// Get all packages to be copied
		GetAllAdvancedCopySources(Package, CopyParams, PackageNamesToCopy, DependencyMap, CopyCustomization);
		
		// Allow the customization to apply any additional filters
		CopyCustomization->ApplyAdditionalFiltering(PackageNamesToCopy);
		CopyCustomization->SetPackageThatInitiatedCopy(Package.ToString());

		TMap<FString, FString> DestinationMap = TMap<FString, FString>();
		GenerateAdvancedCopyDestinations(CopyParams, PackageNamesToCopy, CopyCustomization, DestinationMap);
		CopyCustomization->TransformDestinationPaths(DestinationMap);
		CompleteDestinationMap.Add(DestinationMap);
		CompleteDependencyMap.Add(DependencyMap);
	}

	// Confirm that there is at least one package to move 
	if (CompleteDestinationMap.Num() == 0)
	{
		FMessageDialog::Open(EAppMsgType::Ok, LOCTEXT("AdvancedCopyPackages_NoFilesToMove", "No files were found to move"));
		return;
	}

	// Prompt the user displaying all assets that are going to be migrated
	if (CopyParams.bShouldSuppressUI)
	{
		AdvancedCopyPackages_ReportConfirmed(CopyParams, CompleteDestinationMap);
	}
	else
	{
		const FText ReportMessage = FText::FromString(CopyParams.GetDropLocationForAdvancedCopy());

		SAdvancedCopyReportDialog::FOnReportConfirmed OnReportConfirmed = SAdvancedCopyReportDialog::FOnReportConfirmed::CreateUObject(this, &UAssetToolsImpl::AdvancedCopyPackages_ReportConfirmed);
		SAdvancedCopyReportDialog::OpenPackageReportDialog(CopyParams, ReportMessage, CompleteDestinationMap, CompleteDependencyMap, OnReportConfirmed);
	}
}

void UAssetToolsImpl::AdvancedCopyPackages_ReportConfirmed(FAdvancedCopyParams CopyParams, TArray<TMap<FString, FString>> DestinationMap) const
{
	TArray<UAdvancedCopyCustomization*> CustomizationsToUse = CopyParams.GetCustomizationsToUse();
	for (int32 CustomizationIndex = 0; CustomizationIndex < CustomizationsToUse.Num(); CustomizationIndex++)
	{
		if (!CustomizationsToUse[CustomizationIndex]->CustomCopyValidate(DestinationMap[CustomizationIndex]))
		{
			FMessageDialog::Open(EAppMsgType::Ok, FText::Format(LOCTEXT("AdvancedCopy_FailedCustomValidate", "Advanced Copy failed because the validation rules set in {0} failed."),
				FText::FromString(CustomizationsToUse[CustomizationIndex]->GetName())));

			return;
		}
	}
	AdvancedCopyPackages(CopyParams, DestinationMap);
}

bool UAssetToolsImpl::IsAssetClassSupported(const UClass* AssetClass) const
{
	TWeakPtr<IAssetTypeActions> AssetTypeActions = GetAssetTypeActionsForClass(AssetClass);
	if (!AssetTypeActions.IsValid())
	{
		return false;
	}

	if (AssetTypeActions.Pin()->IsSupported() == false)
	{
		return false;
	}

	return true;
}

TArray<UFactory*> UAssetToolsImpl::GetNewAssetFactories() const
{
	TArray<UFactory*> Factories;

	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* Class = *It;
		if (Class->IsChildOf(UFactory::StaticClass()) &&
			!Class->HasAnyClassFlags(CLASS_Abstract))
		{
			UFactory* Factory = Class->GetDefaultObject<UFactory>();

			if (Factory->ShouldShowInNewMenu() &&
				ensure(!Factory->GetDisplayName().IsEmpty()) &&
				IsAssetClassSupported(Factory->GetSupportedClass()))
			{
				Factories.Add(Factory);
			}
		}
	}

	return MoveTemp(Factories);
}

TSharedRef<FNamePermissionList>& UAssetToolsImpl::GetAssetClassPermissionList()
{
	return AssetClassPermissionList;
}

void UAssetToolsImpl::AssetClassPermissionListChanged()
{
	for (TSharedRef<IAssetTypeActions>& ActionsIt : AssetTypeActionsList)
	{
		bool bSupported = false;
		if (const UClass* SupportedClass = ActionsIt->GetSupportedClass())
		{
			bSupported = AssetClassPermissionList->PassesFilter(SupportedClass->GetFName());
		}
		else
		{
			bSupported = !ActionsIt->GetFilterName().IsNone();
		}

		ActionsIt->SetSupported(bSupported);
	}
}

void UAssetToolsImpl::AddSubContentBlacklist(const FString& InMount)
{
	for (const FString& SubContentPath : SubContentBlacklistPaths)
	{
		FolderPermissionList->AddDenyListItem("AssetToolsConfigFile", InMount / SubContentPath);
	}
}

void UAssetToolsImpl::OnContentPathMounted(const FString& InAssetPath, const FString& FileSystemPath)
{
	AddSubContentBlacklist(InAssetPath);
}

TArray<UObject*> UAssetToolsImpl::ImportAssetsWithDialogImplementation(const FString& DestinationPath, bool bAllowAsyncImport)
{
	if (!GetWritableFolderPermissionList()->PassesStartsWithFilter(DestinationPath))
	{
		NotifyBlockedByWritableFolderFilter();
		return TArray<UObject*>();
	}

	TArray<UObject*> ReturnObjects;
	FString FileTypes, AllExtensions;
	TArray<UFactory*> Factories;

	// Get the list of valid factories
	for (TObjectIterator<UClass> It; It; ++It)
	{
		UClass* CurrentClass = (*It);

		if (CurrentClass->IsChildOf(UFactory::StaticClass()) && !(CurrentClass->HasAnyClassFlags(CLASS_Abstract)))
		{
			UFactory* Factory = Cast<UFactory>(CurrentClass->GetDefaultObject());
			if (Factory->bEditorImport)
			{
				Factories.Add(Factory);
			}
		}
	}

	TMultiMap<uint32, UFactory*> FilterIndexToFactory;

	// Generate the file types and extensions represented by the selected factories
	ObjectTools::GenerateFactoryFileExtensions(Factories, FileTypes, AllExtensions, FilterIndexToFactory);

	FileTypes = FString::Printf(TEXT("All Files (%s)|%s|%s"), *AllExtensions, *AllExtensions, *FileTypes);

	// Prompt the user for the filenames
	TArray<FString> OpenFilenames;
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	bool bOpened = false;
	int32 FilterIndex = -1;

	if (DesktopPlatform)
	{
		const void* ParentWindowWindowHandle = FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);

		bOpened = DesktopPlatform->OpenFileDialog(
			ParentWindowWindowHandle,
			LOCTEXT("ImportDialogTitle", "Import").ToString(),
			FEditorDirectories::Get().GetLastDirectory(ELastDirectory::GENERIC_IMPORT),
			TEXT(""),
			FileTypes,
			EFileDialogFlags::Multiple,
			OpenFilenames,
			FilterIndex
		);
	}

	if (bOpened)
	{
		if (OpenFilenames.Num() > 0)
		{
			UFactory* ChosenFactory = nullptr;
			if (FilterIndex > 0)
			{
				ChosenFactory = *FilterIndexToFactory.Find(FilterIndex);
			}


			FEditorDirectories::Get().SetLastDirectory(ELastDirectory::GENERIC_IMPORT, OpenFilenames[0]);
			const bool bSyncToBrowser = false;
			TArray<TPair<FString, FString>>* FilesAndDestination = nullptr;
			ReturnObjects = ImportAssets(OpenFilenames, DestinationPath, ChosenFactory, bSyncToBrowser, FilesAndDestination, bAllowAsyncImport);
		}
	}

	return ReturnObjects;
}

TSharedRef<FPathPermissionList>& UAssetToolsImpl::GetFolderPermissionList()
{
	return FolderPermissionList;
}

TSharedRef<FPathPermissionList>& UAssetToolsImpl::GetWritableFolderPermissionList()
{
	return WritableFolderPermissionList;
}

bool UAssetToolsImpl::AllPassWritableFolderFilter(const TArray<FString>& InPaths) const
{
	if (WritableFolderPermissionList->HasFiltering())
	{
		for (const FString& Path : InPaths)
		{
			if (!WritableFolderPermissionList->PassesStartsWithFilter(Path))
			{
				return false;
			}
		}
	}

	return true;
}

void UAssetToolsImpl::NotifyBlockedByWritableFolderFilter() const
{
	FSlateNotificationManager::Get().AddNotification(FNotificationInfo(LOCTEXT("NotifyBlockedByWritableFolderFilter", "Folder is locked")));
}

#undef LOCTEXT_NAMESPACE
