// Copyright Epic Games, Inc. All Rights Reserved.
#include "AssetImporters/ProgressiveImportSurfaces.h"
#include "Utilities/MiscUtils.h"
#include "Utilities/MaterialUtils.h"
#include "MSSettings.h"

#include "Misc/FileHelper.h"
#include "Misc/ScopedSlowTask.h"
#include "JsonObjectConverter.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/Paths.h"

#include "UObject/SoftObjectPath.h"
#include "Engine/StreamableManager.h"
#include "Engine/AssetManager.h"

#include "EditorViewportClient.h"
#include "UnrealClient.h"
#include "Engine/StaticMesh.h"

#include "MaterialEditingLibrary.h"

#include "Async/AsyncWork.h"
#include "Async/Async.h"

#include "Kismet/GameplayStatics.h"
#include "GameFramework/Actor.h"
#include "Components/StaticMeshComponent.h"





TSharedPtr<FImportProgressiveSurfaces> FImportProgressiveSurfaces::ImportProgressiveSurfacesInst;



TSharedPtr<FImportProgressiveSurfaces> FImportProgressiveSurfaces::Get()
{
	if (!ImportProgressiveSurfacesInst.IsValid())
	{
		ImportProgressiveSurfacesInst = MakeShareable(new FImportProgressiveSurfaces);
	}
	return ImportProgressiveSurfacesInst;
}


void FImportProgressiveSurfaces::ImportAsset(TSharedPtr<FJsonObject> AssetImportJson, float LocationOffset, bool bIsNormal)
{

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	FStreamableManager& Streamable = UAssetManager::GetStreamableManager();

	TSharedPtr<FUAssetData> ImportData = JsonUtils::ParseUassetJson(AssetImportJson);

	/*FString UassetMetaString;
	FFileHelper::LoadFileToString(UassetMetaString, *ImportData->ImportJsonPath);*/

	FUAssetMeta AssetMetaData = AssetUtils::GetAssetMetaData(ImportData->ImportJsonPath);
	//FJsonObjectConverter::JsonObjectStringToUStruct(UassetMetaString, &AssetMetaData);

	FString DestinationPath = AssetMetaData.assetRootPath;
	FString DestinationFolder = FPaths::Combine(FPaths::ProjectContentDir(), DestinationPath.Replace(TEXT("/Game/"), TEXT("")));

	CopyUassetFiles(ImportData->FilePaths, DestinationFolder);

	if (bIsNormal)
	{

		FString MInstancePath = AssetMetaData.materialInstances[0].instancePath;
		FAssetData MInstanceData = AssetRegistry.GetAssetByObjectPath(FName(*MInstancePath));

		if (!MInstanceData.IsValid()) return;


		FSoftObjectPath ItemToStream = MInstanceData.ToSoftObjectPath();
		Streamable.RequestAsyncLoad(ItemToStream, FStreamableDelegate::CreateRaw(this, &FImportProgressiveSurfaces::HandleNormalMaterialLoad, MInstanceData, AssetMetaData,  LocationOffset));


		return;
	}

	if (AssetMetaData.assetSubType == TEXT("imperfection") && ImportData->ProgressiveStage == 3)
	{
		ImportData->ProgressiveStage = 4;
	}
	
	if (ImportData->ProgressiveStage != 1 && !PreviewDetails.Contains(ImportData->AssetId))
	{
		return;
	}

	if (!PreviewDetails.Contains(ImportData->AssetId) )
	{
		TSharedPtr< FProgressiveSurfaces> ProgressiveDetails = MakeShareable(new FProgressiveSurfaces);
		PreviewDetails.Add(ImportData->AssetId, ProgressiveDetails);
	}


	if (ImportData->ProgressiveStage == 1)
	{
		FString MInstancePath = AssetMetaData.materialInstances[0].instancePath;
		FAssetData MInstanceData = AssetRegistry.GetAssetByObjectPath(FName(*MInstancePath));

		if (!MInstanceData.IsValid())
		{
			PreviewDetails[ImportData->AssetId]->PreviewInstance = nullptr;
			return;
		}

		FSoftObjectPath ItemToStream = MInstanceData.ToSoftObjectPath();
		Streamable.RequestAsyncLoad(ItemToStream, FStreamableDelegate::CreateRaw(this, &FImportProgressiveSurfaces::HandlePreviewInstanceLoad, MInstanceData, ImportData->AssetId, LocationOffset));

	}
	else if (ImportData->ProgressiveStage == 2)
	{
		FString TexturePath = TEXT("");
		FString TextureType = TEXT("");
		if (AssetMetaData.assetSubType == TEXT("imperfection"))
		{
			TextureType = TEXT("roughness");
		}
		else
		{
			TextureType = TEXT("albedo");
		}

		for (FTexturesList TextureMeta : AssetMetaData.textureSets)
		{
			if (TextureMeta.type == TextureType)
			{
				TexturePath = TextureMeta.path;
			}
		}		

		FAssetData TextureData = AssetRegistry.GetAssetByObjectPath(FName(*TexturePath));

		if (!TextureData.IsValid()) return;

		FSoftObjectPath ItemToStream = TextureData.ToSoftObjectPath();
		Streamable.RequestAsyncLoad(ItemToStream, FStreamableDelegate::CreateRaw(this, &FImportProgressiveSurfaces::HandlePreviewTextureLoad, TextureData, ImportData->AssetId, TextureType));


	}

	else if (ImportData->ProgressiveStage == 3)
	{

		FString NormalPath = TEXT("");
		FString TextureType = TEXT("normal");

		for (FTexturesList TextureMeta : AssetMetaData.textureSets)
		{
			if (TextureMeta.type == TextureType)
			{
				NormalPath = TextureMeta.path;
			}
		}		

		FAssetData NormalData = AssetRegistry.GetAssetByObjectPath(FName(*NormalPath));

		if (!NormalData.IsValid()) return;

		FSoftObjectPath ItemToStream = NormalData.ToSoftObjectPath();
		Streamable.RequestAsyncLoad(ItemToStream, FStreamableDelegate::CreateRaw(this, &FImportProgressiveSurfaces::HandlePreviewTextureLoad, NormalData, ImportData->AssetId, TextureType));

	}

	else if (ImportData->ProgressiveStage == 4)
	{	
		FString MInstanceHighPath = AssetMetaData.materialInstances[0].instancePath;
		FAssetData MInstanceHighData = AssetRegistry.GetAssetByObjectPath(FName(*MInstanceHighPath));

		if (!MInstanceHighData.IsValid()) return;

		FSoftObjectPath ItemToStream = MInstanceHighData.ToSoftObjectPath();
		Streamable.RequestAsyncLoad(ItemToStream, FStreamableDelegate::CreateRaw(this, &FImportProgressiveSurfaces::HandleHighInstanceLoad, MInstanceHighData, ImportData->AssetId, AssetMetaData));

	}


}

void FImportProgressiveSurfaces::HandlePreviewTextureLoad(FAssetData TextureData, FString AssetID, FString Type)
{
	if (!IsValid(PreviewDetails[AssetID]->PreviewInstance))
	{
		return;
	}

	UTexture* PreviewTexture = Cast<UTexture>(TextureData.GetAsset());
	UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(PreviewDetails[AssetID]->PreviewInstance, FName(*Type), PreviewTexture);
	AssetUtils::SavePackage(PreviewDetails[AssetID]->PreviewInstance);
}



void FImportProgressiveSurfaces::HandlePreviewInstanceLoad(FAssetData PreviewInstanceData, FString AssetID, float LocationOffset)
{
	PreviewDetails[AssetID]->PreviewInstance = Cast<UMaterialInstanceConstant>(PreviewInstanceData.GetAsset());
	SpawnMaterialPreviewActor(AssetID, LocationOffset);
}

void FImportProgressiveSurfaces::SpawnMaterialPreviewActor(FString AssetID, float LocationOffset, bool bIsNormal, FAssetData MInstanceData)
{
	const UMegascansSettings* MegascansSettings = GetDefault<UMegascansSettings>();

	if (MegascansSettings->bApplyToSelection)
	{
		if (bIsNormal)
		{


			FMaterialUtils::ApplyMaterialToSelection(MInstanceData.GetPackage()->GetPathName());
			//PreviewDetails[AssetID]->ActorsInLevel = FMaterialUtils::ApplyMaterialToSelection(MInstanceData.GetPackage()->GetPathName());
		}
		else
		{
			PreviewDetails[AssetID]->ActorsInLevel = FMaterialUtils::ApplyMaterialToSelection(PreviewDetails[AssetID]->PreviewInstance->GetPathName());
		}

		return;

	}


	IAssetRegistry& AssetRegistry = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
	FString SphereMeshPath = TEXT("/Engine/BasicShapes/Sphere.Sphere");

	FAssetData PreviewerMeshData = AssetRegistry.GetAssetByObjectPath(FName(*SphereMeshPath));

	FViewport* ActiveViewport = GEditor->GetActiveViewport();
	FEditorViewportClient* EditorViewClient = (FEditorViewportClient*)ActiveViewport->GetClient();

	FVector SpawnLocation;

	UWorld* CurrentWorld = GEngine->GetWorldContexts()[0].World();
	UStaticMesh* SourceMesh = Cast<UStaticMesh>(PreviewerMeshData.GetAsset());
	FTransform InitialTransform(SpawnLocation);

	AStaticMeshActor* SMActor = Cast<AStaticMeshActor>(CurrentWorld->SpawnActor(AStaticMeshActor::StaticClass(), &InitialTransform));
	SMActor->GetStaticMeshComponent()->SetStaticMesh(SourceMesh);
	if (bIsNormal)
	{
		UMaterialInstanceConstant* MInstance = Cast<UMaterialInstanceConstant>(MInstanceData.GetAsset());
		SMActor->GetStaticMeshComponent()->SetMaterial(0, MInstance);

	}
	else
	{
		SMActor->GetStaticMeshComponent()->SetMaterial(0, CastChecked<UMaterialInterface>(PreviewDetails[AssetID]->PreviewInstance));
	}
	//SMActor->Rename(TEXT("MyStaticMeshInTheWorld"));
	SMActor->SetActorLabel(AssetID);

	GEditor->SelectActor(SMActor, true, false);

	GEditor->EditorUpdateComponents();
	CurrentWorld->UpdateWorldComponents(true, false);
	SMActor->RerunConstructionScripts();
	if (bIsNormal)
	{
		FBridgeDragDrop::Instance->OnAddProgressiveStageDataDelegate.ExecuteIfBound(MInstanceData, AssetID, SMActor);
		return;
	}

	if (PreviewDetails.Contains(AssetID))
	{
		PreviewDetails[AssetID]->ActorsInLevel.Add(SMActor);
		FBridgeDragDrop::Instance->OnAddProgressiveStageDataDelegate.ExecuteIfBound(PreviewerMeshData, AssetID, SMActor);
	}
}

void FImportProgressiveSurfaces::HandleHighInstanceLoad(FAssetData HighInstanceData, FString AssetID, FUAssetMeta AssetMetaData)
{

	AssetUtils::ConvertToVT(AssetMetaData);


	if (FMaterialUtils::ShouldOverrideMaterial(AssetMetaData.assetType))
	{
		AssetUtils::DeleteAsset(AssetMetaData.materialInstances[0].instancePath);
		UMaterialInstanceConstant* OverridenInstance = FMaterialUtils::CreateMaterialOverride(AssetMetaData);
		FMaterialUtils::ApplyMaterialInstance(AssetMetaData, OverridenInstance);

	}


	if (!PreviewDetails.Contains(AssetID)) return;
	if (PreviewDetails[AssetID]->ActorsInLevel.Num() == 0)
	{
		PreviewDetails.Remove(AssetID);
		return;
	}

	

	for (AStaticMeshActor* UsedActor : PreviewDetails[AssetID]->ActorsInLevel)
	{
		if (!IsValid(UsedActor)) continue;
		if (!UsedActor) continue;
		if (UsedActor == nullptr) continue;			

		AssetUtils::ManageImportSettings(AssetMetaData);		

		//UMaterialInstanceConstant* HighInstance = Cast<UMaterialInstanceConstant>(HighInstanceData.GetAsset());		
		UsedActor->GetStaticMeshComponent()->SetMaterial(0, CastChecked<UMaterialInterface>(HighInstanceData.GetAsset()));
	}
	PreviewDetails.Remove(AssetID);
}


//Handle normal surfaces/decals/imperfections import through drag.
void FImportProgressiveSurfaces::HandleNormalMaterialLoad(FAssetData AssetInstanceData, FUAssetMeta AssetMetaData, float LocationOffset)
{	
	if (FMaterialUtils::ShouldOverrideMaterial(AssetMetaData.assetType))
	{
		AssetUtils::DeleteAsset(AssetMetaData.materialInstances[0].instancePath);
		UMaterialInstanceConstant* OverridenInstance = FMaterialUtils::CreateMaterialOverride(AssetMetaData);

		FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>("AssetRegistry");
		IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();
		FAssetData OverridenInstanceData = AssetRegistry.GetAssetByObjectPath(FName(*OverridenInstance->GetPathName()));
		SpawnMaterialPreviewActor(AssetMetaData.assetID, LocationOffset, true, OverridenInstanceData);
		return;
		

	}
	SpawnMaterialPreviewActor(AssetMetaData.assetID, LocationOffset, true, AssetInstanceData);

}

