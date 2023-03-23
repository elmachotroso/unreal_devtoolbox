// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	MaterialInterface.cpp: UMaterialInterface implementation.
=============================================================================*/

#include "Materials/MaterialInterface.h"

#include "RenderingThread.h"
#include "PrimitiveViewRelevance.h"
#include "MaterialShared.h"
#include "Materials/Material.h"
#include "UObject/ObjectSaveContext.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UE5ReleaseStreamObjectVersion.h"
#include "EditorFramework/AssetImportData.h"
#include "Engine/AssetUserData.h"
#include "Engine/Texture2D.h"
#include "ObjectCacheEventSink.h"
#include "Engine/SubsurfaceProfile.h"
#include "Engine/TextureStreamingTypes.h"
#include "Algo/BinarySearch.h"
#include "Interfaces/ITargetPlatform.h"
#include "Components.h"
#include "ContentStreaming.h"
#include "MeshBatch.h"
#include "TextureCompiler.h"
#include "MaterialShaderQualitySettings.h"
#include "ShaderPlatformQualitySettings.h"

#if WITH_EDITOR
#include "ObjectCacheEventSink.h"
#endif

/**
 * This is used to deprecate data that has been built with older versions.
 * To regenerate the data, commands like "BUILDMATERIALTEXTURESTREAMINGDATA" can be used in the editor.
 * Ideally the data would be stored the DDC instead of the asset, but this is not yet  possible because it requires the GPU.
 */
#define MATERIAL_TEXTURE_STREAMING_DATA_VERSION 1

//////////////////////////////////////////////////////////////////////////

UEnum* UMaterialInterface::SamplerTypeEnum = nullptr;

//////////////////////////////////////////////////////////////////////////

bool IsHairStrandsGeometrySupported(const EShaderPlatform Platform)
{
	check(Platform != SP_NumPlatforms);

	return FDataDrivenShaderPlatformInfo::GetSupportsHairStrandGeometry(Platform)
		&& IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
}

bool IsCompatibleWithHairStrands(const FMaterial* Material, const ERHIFeatureLevel::Type FeatureLevel)
{
	return
		ERHIFeatureLevel::SM5 <= FeatureLevel &&
		Material && Material->IsUsedWithHairStrands() && 
		(Material->GetBlendMode() == BLEND_Opaque || Material->GetBlendMode() == BLEND_Masked);
}

bool IsCompatibleWithHairStrands(EShaderPlatform Platform, const FMaterialShaderParameters& Parameters)
{
	return
		IsHairStrandsGeometrySupported(Platform) &&
		Parameters.bIsUsedWithHairStrands &&
		(Parameters.BlendMode == BLEND_Opaque || Parameters.BlendMode == BLEND_Masked);
}

static EMaterialGetParameterValueFlags MakeParameterValueFlags(bool bOveriddenOnly)
{
	EMaterialGetParameterValueFlags Result = EMaterialGetParameterValueFlags::CheckInstanceOverrides;
	if (!bOveriddenOnly)
	{
		Result |= EMaterialGetParameterValueFlags::CheckNonOverrides;
	}
	return Result;
}

//////////////////////////////////////////////////////////////////////////

/** Copies the material's relevance flags to a primitive's view relevance flags. */
void FMaterialRelevance::SetPrimitiveViewRelevance(FPrimitiveViewRelevance& OutViewRelevance) const
{
	OutViewRelevance.Raw = Raw;
}

//////////////////////////////////////////////////////////////////////////

UMaterialInterface::UMaterialInterface(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		MaterialDomainString(MD_Surface); // find the enum for this now before we start saving
#if USE_EVENT_DRIVEN_ASYNC_LOAD_AT_BOOT_TIME
		if (!GIsInitialLoad || !GEventDrivenLoaderEnabled)
#endif
		{
			InitDefaultMaterials();
			AssertDefaultMaterialsExist();
		}

		if (SamplerTypeEnum == nullptr)
		{
			SamplerTypeEnum = StaticEnum<EMaterialSamplerType>();
			check(SamplerTypeEnum);
		}

		SetLightingGuid();
	}
}

void UMaterialInterface::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FUE5ReleaseStreamObjectVersion::GUID);

	Super::Serialize(Ar);

	bool bSavedCachedExpressionData = false;
	if (Ar.CustomVer(FUE5ReleaseStreamObjectVersion::GUID) >= FUE5ReleaseStreamObjectVersion::MaterialInterfaceSavedCachedData)
	{
		// If we have editor data, up-to-date cached data can be regenerated on load
		// In that case, we only need to save cached data when cooking (since the target may not have editor data)
		// If we *don't* have editor data, then we always save our cached data...otherwise there won't be any way to regenerate it
#if WITH_EDITORONLY_DATA
		const bool bWantToSaveCachedData = Ar.IsCooking();
#else
		const bool bWantToSaveCachedData = Ar.IsSaving();
#endif
		if (bWantToSaveCachedData && (bool)CachedExpressionData)
		{
			bSavedCachedExpressionData = true;
		}

		Ar << bSavedCachedExpressionData;
	}

	if (bSavedCachedExpressionData)
	{
		if (Ar.IsLoading())
		{
			CachedExpressionData.Reset(new FMaterialCachedExpressionData());
			bLoadedCachedExpressionData = true;
		}
		check(CachedExpressionData);
		UScriptStruct* Struct = FMaterialCachedExpressionData::StaticStruct();
		Struct->SerializeTaggedProperties(Ar, (uint8*)CachedExpressionData.Get(), Struct, nullptr);

#if WITH_EDITOR
		FObjectCacheEventSink::NotifyReferencedTextureChanged_Concurrent(this);
#endif
	}
}

void UMaterialInterface::PostLoad()
{
	Super::PostLoad();
#if USE_EVENT_DRIVEN_ASYNC_LOAD_AT_BOOT_TIME
	if (!GEventDrivenLoaderEnabled)
#endif
	{
		PostLoadDefaultMaterials();
	}

#if WITH_EDITORONLY_DATA
	if (TextureStreamingDataVersion != MATERIAL_TEXTURE_STREAMING_DATA_VERSION)
	{
		TextureStreamingData.Empty();
	}
#endif
}

const FMaterialCachedExpressionData& UMaterialInterface::GetCachedExpressionData(TMicRecursionGuard) const
{
	const FMaterialCachedExpressionData* LocalData = CachedExpressionData.Get();
	return LocalData ? *LocalData : FMaterialCachedExpressionData::EmptyData;
}

void UMaterialInterface::GetQualityLevelUsage(TArray<bool, TInlineAllocator<EMaterialQualityLevel::Num> >& OutQualityLevelsUsed, EShaderPlatform ShaderPlatform, bool bCooking)
{
	OutQualityLevelsUsed = GetCachedExpressionData().QualityLevelsUsed;
	if (OutQualityLevelsUsed.Num() == 0)
	{
		OutQualityLevelsUsed.AddDefaulted(EMaterialQualityLevel::Num);
	}
	if (ShaderPlatform != SP_NumPlatforms)
	{
		const UShaderPlatformQualitySettings* MaterialQualitySettings = UMaterialShaderQualitySettings::Get()->GetShaderPlatformQualitySettings(ShaderPlatform);
		for (int32 Quality = 0; Quality < EMaterialQualityLevel::Num; ++Quality)
		{
			const FMaterialQualityOverrides& QualityOverrides = MaterialQualitySettings->GetQualityOverrides((EMaterialQualityLevel::Type)Quality);
			if (bCooking && QualityOverrides.bDiscardQualityDuringCook)
			{
				OutQualityLevelsUsed[Quality] = false;
			}
			else if (QualityOverrides.bEnableOverride &&
				QualityOverrides.HasAnyOverridesSet() &&
				QualityOverrides.CanOverride(ShaderPlatform))
			{
				OutQualityLevelsUsed[Quality] = true;
			}
		}
	}
}

TArrayView<const TObjectPtr<UObject>> UMaterialInterface::GetReferencedTextures() const
{
	return GetCachedExpressionData().ReferencedTextures;
}

#if WITH_EDITOR
void UMaterialInterface::GetReferencedTexturesAndOverrides(TSet<const UTexture*>& InOutTextures) const
{
	for (UObject* UsedObject : GetCachedExpressionData().ReferencedTextures)
	{
		if (const UTexture* UsedTexture = Cast<UTexture>(UsedObject))
		{
			InOutTextures.Add(UsedTexture);
		}
	}
}
#endif // WITH_EDITOR

void UMaterialInterface::GetUsedTexturesAndIndices(TArray<UTexture*>& OutTextures, TArray< TArray<int32> >& OutIndices, EMaterialQualityLevel::Type QualityLevel, ERHIFeatureLevel::Type FeatureLevel) const
{
	GetUsedTextures(OutTextures, QualityLevel, false, FeatureLevel, false);
	OutIndices.AddDefaulted(OutTextures.Num());
}

#if WITH_EDITORONLY_DATA
bool UMaterialInterface::GetStaticSwitchParameterValue(const FHashedMaterialParameterInfo& ParameterInfo, bool& OutValue, FGuid& OutExpressionGuid, bool bOveriddenOnly /*= false*/) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::StaticSwitch, ParameterInfo, Result, MakeParameterValueFlags(bOveriddenOnly)))
	{
		OutExpressionGuid = Result.ExpressionGuid;
		OutValue = Result.Value.AsStaticSwitch();
		return true;
	}
	return false;
}

bool UMaterialInterface::GetStaticComponentMaskParameterValue(const FHashedMaterialParameterInfo& ParameterInfo, bool& R, bool& G, bool& B, bool& A, FGuid& OutExpressionGuid, bool bOveriddenOnly /*= false*/) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::StaticSwitch, ParameterInfo, Result, MakeParameterValueFlags(bOveriddenOnly)))
	{
		OutExpressionGuid = Result.ExpressionGuid;
		R = Result.Value.Bool[0];
		G = Result.Value.Bool[1];
		B = Result.Value.Bool[2];
		A = Result.Value.Bool[3];
		return true;
	}
	return false;
}
#endif // WITH_EDITORONLY_DATA

FMaterialRelevance UMaterialInterface::GetRelevance_Internal(const UMaterial* Material, ERHIFeatureLevel::Type InFeatureLevel) const
{
	if(Material)
	{
		const FMaterialResource* MaterialResource = GetMaterialResource(InFeatureLevel);

		// If material is invalid e.g. unparented instance, fallback to the passed in material
		if (!MaterialResource && Material)
		{
			MaterialResource = Material->GetMaterialResource(InFeatureLevel);	
		}

		if (!MaterialResource)
		{
			return FMaterialRelevance();
		}

		const bool bIsMobile = InFeatureLevel <= ERHIFeatureLevel::ES3_1;
		const bool bUsesSingleLayerWaterMaterial = MaterialResource->GetShadingModels().HasShadingModel(MSM_SingleLayerWater);
		const bool IsSinglePassWaterTranslucent = bIsMobile && bUsesSingleLayerWaterMaterial;
		const bool bIsMobilePixelProjectedTranslucent = MaterialResource->IsUsingPlanarForwardReflections() 
														&& IsUsingMobilePixelProjectedReflection(GetFeatureLevelShaderPlatform(InFeatureLevel));

		// Note that even though XX_GameThread() api is called, this function can be called on non game thread via 
		// GetRelevance_Concurrent()
		bool bUsesAnisotropy = MaterialResource->GetShadingModels().HasAnyShadingModel({ MSM_DefaultLit, MSM_ClearCoat }) && 
			MaterialResource->MaterialUsesAnisotropy_GameThread();

		const EBlendMode BlendMode = (EBlendMode)GetBlendMode();
		const bool bIsTranslucent = IsTranslucentBlendMode(BlendMode) || IsSinglePassWaterTranslucent || bIsMobilePixelProjectedTranslucent; // We want meshes with water materials to be scheduled for translucent pass on mobile. And we also have to render the meshes used for mobile pixel projection reflection in translucent pass.

		EMaterialDomain Domain = (EMaterialDomain)MaterialResource->GetMaterialDomain();
		bool bDecal = (Domain == MD_DeferredDecal);

		// Determine the material's view relevance.
		FMaterialRelevance MaterialRelevance;

		MaterialRelevance.ShadingModelMask = GetShadingModels().GetShadingModelField();
		MaterialRelevance.bUsesCustomDepthStencil = MaterialResource->UsesCustomDepthStencil_GameThread();

		if(bDecal)
		{
			MaterialRelevance.bDecal = bDecal;
			// we rely on FMaterialRelevance defaults are 0
		}
		else
		{
			// Check whether the material can be drawn in the separate translucency pass as per FMaterialResource::IsTranslucencyAfterDOFEnabled and IsMobileSeparateTranslucencyEnabled
			EMaterialTranslucencyPass TranslucencyPass = MTP_BeforeDOF;
			const bool bSupportsSeparateTranslucency = Material->MaterialDomain != MD_UI && Material->MaterialDomain != MD_DeferredDecal;
			if (bIsTranslucent && bSupportsSeparateTranslucency)
			{
				if (bIsMobile)
				{
					if (Material->bEnableMobileSeparateTranslucency)
					{
						TranslucencyPass = MTP_AfterDOF;
					}
				}
				else
				{
					TranslucencyPass = Material->TranslucencyPass;
				}
			}			

			// If dual blending is supported, and we are rendering post-DOF translucency, then we also need to render a second pass to the modulation buffer.
			// The modulation buffer can also be used for regular modulation shaders after DoF.
			const bool bMaterialSeparateModulation =
				(MaterialResource->IsDualBlendingEnabled(GShaderPlatformForFeatureLevel[InFeatureLevel]) || BlendMode == BLEND_Modulate)
				&& TranslucencyPass == MTP_AfterDOF;

			MaterialRelevance.bOpaque = !bIsTranslucent;
			MaterialRelevance.bMasked = IsMasked();
			MaterialRelevance.bDistortion = MaterialResource->IsDistorted();
			MaterialRelevance.bHairStrands = IsCompatibleWithHairStrands(MaterialResource, InFeatureLevel);
			MaterialRelevance.bSeparateTranslucency = (TranslucencyPass == MTP_AfterDOF);
			MaterialRelevance.bSeparateTranslucencyModulate = bMaterialSeparateModulation;
			MaterialRelevance.bPostMotionBlurTranslucency = (TranslucencyPass == MTP_AfterMotionBlur);
			MaterialRelevance.bNormalTranslucency = bIsTranslucent && (TranslucencyPass == MTP_BeforeDOF);
			MaterialRelevance.bDisableDepthTest = bIsTranslucent && Material->bDisableDepthTest;		
			MaterialRelevance.bUsesSceneColorCopy = bIsTranslucent && MaterialResource->RequiresSceneColorCopy_GameThread();
			MaterialRelevance.bOutputsTranslucentVelocity = Material->IsTranslucencyWritingVelocity();
			MaterialRelevance.bUsesGlobalDistanceField = MaterialResource->UsesGlobalDistanceField_GameThread();
			MaterialRelevance.bUsesWorldPositionOffset = MaterialResource->UsesWorldPositionOffset_GameThread();
			ETranslucencyLightingMode TranslucencyLightingMode = MaterialResource->GetTranslucencyLightingMode();
			MaterialRelevance.bTranslucentSurfaceLighting = bIsTranslucent && (TranslucencyLightingMode == TLM_SurfacePerPixelLighting || TranslucencyLightingMode == TLM_Surface);
			MaterialRelevance.bUsesSceneDepth = MaterialResource->MaterialUsesSceneDepthLookup_GameThread();
			MaterialRelevance.bHasVolumeMaterialDomain = MaterialResource->IsVolumetricPrimitive();
			MaterialRelevance.bUsesDistanceCullFade = MaterialResource->MaterialUsesDistanceCullFade_GameThread();
			MaterialRelevance.bUsesSkyMaterial = Material->bIsSky;
			MaterialRelevance.bUsesSingleLayerWaterMaterial = bUsesSingleLayerWaterMaterial;
			MaterialRelevance.bUsesAnisotropy = bUsesAnisotropy;
		}
		return MaterialRelevance;
	}
	else
	{
		return FMaterialRelevance();
	}
}

FMaterialParameterInfo UMaterialInterface::GetParameterInfo(EMaterialParameterAssociation Association, FName ParameterName, UMaterialFunctionInterface* LayerFunction) const
{
	int32 Index = INDEX_NONE;
	if (Association != GlobalParameter)
	{
		if (LayerFunction)
		{
			FMaterialLayersFunctions MaterialLayers;
			if (GetMaterialLayers(MaterialLayers))
			{
				if (Association == BlendParameter) Index = MaterialLayers.Blends.Find(LayerFunction);
				else if (Association == LayerParameter) Index = MaterialLayers.Layers.Find(LayerFunction);
			}
		}
		if (Index == INDEX_NONE)
		{
			return FMaterialParameterInfo();
		}
	}

	return FMaterialParameterInfo(ParameterName, Association, Index);
}

FMaterialRelevance UMaterialInterface::GetRelevance(ERHIFeatureLevel::Type InFeatureLevel) const
{
	// Find the interface's concrete material.
	const UMaterial* Material = GetMaterial();
	return GetRelevance_Internal(Material, InFeatureLevel);
}

FMaterialRelevance UMaterialInterface::GetRelevance_Concurrent(ERHIFeatureLevel::Type InFeatureLevel) const
{
	// Find the interface's concrete material.
	const UMaterial* Material = GetMaterial_Concurrent();
	return GetRelevance_Internal(Material, InFeatureLevel);
}

int32 UMaterialInterface::GetWidth() const
{
	return ME_PREV_THUMBNAIL_SZ+(ME_STD_BORDER*2);
}

int32 UMaterialInterface::GetHeight() const
{
	return ME_PREV_THUMBNAIL_SZ+ME_CAPTION_HEIGHT+(ME_STD_BORDER*2);
}


void UMaterialInterface::SetForceMipLevelsToBeResident( bool OverrideForceMiplevelsToBeResident, bool bForceMiplevelsToBeResidentValue, float ForceDuration, int32 CinematicTextureGroups, bool bFastResponse )
{
	TArray<UTexture*> Textures;
	
	GetUsedTextures(Textures, EMaterialQualityLevel::Num, false, ERHIFeatureLevel::Num, true);
	
#if WITH_EDITOR
	FTextureCompilingManager::Get().FinishCompilation(Textures);
#endif

	for ( int32 TextureIndex=0; TextureIndex < Textures.Num(); ++TextureIndex )
	{
		UTexture2D* Texture = Cast<UTexture2D>(Textures[TextureIndex]);
		if ( Texture )
		{
			Texture->SetForceMipLevelsToBeResident( ForceDuration, CinematicTextureGroups );
			if (OverrideForceMiplevelsToBeResident)
			{
				Texture->bForceMiplevelsToBeResident = bForceMiplevelsToBeResidentValue;
			}

			if (bFastResponse && (ForceDuration > 0.f || Texture->bForceMiplevelsToBeResident))
			{
				static IConsoleVariable* CVarAllowFastForceResident = IConsoleManager::Get().FindConsoleVariable(TEXT("r.Streaming.AllowFastForceResident"));

				Texture->bIgnoreStreamingMipBias = CVarAllowFastForceResident && CVarAllowFastForceResident->GetInt();
				if (Texture->IsStreamable())
				{
					IStreamingManager::Get().GetRenderAssetStreamingManager().FastForceFullyResident(Texture);
				}
			}
		}
	}
}

void UMaterialInterface::RecacheAllMaterialUniformExpressions(bool bRecreateUniformBuffer)
{
	// For each interface, reacache its uniform parameters
	for( TObjectIterator<UMaterialInterface> MaterialIt; MaterialIt; ++MaterialIt )
	{
		MaterialIt->RecacheUniformExpressions(bRecreateUniformBuffer);
	}
}

bool UMaterialInterface::IsReadyForFinishDestroy()
{
	bool bIsReady = Super::IsReadyForFinishDestroy();
	bIsReady = bIsReady && ParentRefFence.IsFenceComplete(); 
	return bIsReady;
}

void UMaterialInterface::BeginDestroy()
{
	ParentRefFence.BeginFence();
	Super::BeginDestroy();

#if WITH_EDITOR
	// The object cache needs to be notified when we're getting destroyed
	FObjectCacheEventSink::NotifyMaterialDestroyed_Concurrent(this);
#endif
}

void UMaterialInterface::FinishDestroy()
{
	CachedExpressionData.Reset();
	Super::FinishDestroy();
}

void UMaterialInterface::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	UMaterialInterface* This = CastChecked<UMaterialInterface>(InThis);
	if (This->CachedExpressionData)
	{
		This->CachedExpressionData->AddReferencedObjects(Collector);
	}
	Super::AddReferencedObjects(This, Collector);
}

void UMaterialInterface::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	SetLightingGuid();
}

#if WITH_EDITOR
void UMaterialInterface::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	// flush the lighting guid on all changes
	SetLightingGuid();

	LightmassSettings.EmissiveBoost = FMath::Max(LightmassSettings.EmissiveBoost, 0.0f);
	LightmassSettings.DiffuseBoost = FMath::Max(LightmassSettings.DiffuseBoost, 0.0f);
	LightmassSettings.ExportResolutionScale = FMath::Clamp(LightmassSettings.ExportResolutionScale, 0.0f, 16.0f);

	for (UAssetUserData* Datum : AssetUserData)
	{
		if (Datum != nullptr)
		{
			Datum->PostEditChangeOwner();
		}
	}

	Super::PostEditChangeProperty(PropertyChangedEvent);
}

void UMaterialInterface::GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const
{
	if (AssetImportData)
	{
		OutTags.Add( FAssetRegistryTag(SourceFileTagName(), AssetImportData->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden) );
	}

	{
		const FMaterialCachedExpressionData& CachedData = GetCachedExpressionData();
		OutTags.Add(FAssetRegistryTag("HasSceneColor", CachedData.bHasSceneColor ? TEXT("True") : TEXT("False"), FAssetRegistryTag::TT_Alphabetical));
		OutTags.Add(FAssetRegistryTag("HasPerInstanceRandom", CachedData.bHasPerInstanceRandom ? TEXT("True") : TEXT("False"), FAssetRegistryTag::TT_Alphabetical));
		OutTags.Add(FAssetRegistryTag("HasPerInstanceCustomData", CachedData.bHasPerInstanceCustomData ? TEXT("True") : TEXT("False"), FAssetRegistryTag::TT_Alphabetical));
		OutTags.Add(FAssetRegistryTag("HasVertexInterpolator", CachedData.bHasVertexInterpolator ? TEXT("True") : TEXT("False"), FAssetRegistryTag::TT_Alphabetical));
	}

	Super::GetAssetRegistryTags(OutTags);
}
#endif // WITH_EDITOR

void UMaterialInterface::GetLightingGuidChain(bool bIncludeTextures, TArray<FGuid>& OutGuids) const
{
	const FMaterialCachedExpressionData& CachedData = GetCachedExpressionData();
	CachedData.AppendReferencedFunctionIdsTo(OutGuids);
	CachedData.AppendReferencedParameterCollectionIdsTo(OutGuids);

#if WITH_EDITORONLY_DATA
	OutGuids.Add(LightingGuid);
#endif // WITH_EDITORONLY_DATA
}

bool UMaterialInterface::GetVectorParameterValue(const FHashedMaterialParameterInfo& ParameterInfo, FLinearColor& OutValue, bool bOveriddenOnly) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::Vector, ParameterInfo, Result, MakeParameterValueFlags(bOveriddenOnly)))
	{
		OutValue = Result.Value.AsLinearColor();
		return true;
	}
	return false;
}

#if WITH_EDITOR
bool UMaterialInterface::IsVectorParameterUsedAsChannelMask(const FHashedMaterialParameterInfo& ParameterInfo, bool& OutValue) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::Vector, ParameterInfo, Result, EMaterialGetParameterValueFlags::CheckNonOverrides))
	{
		OutValue = Result.bUsedAsChannelMask;
		return true;
	}
	return false;
}

bool UMaterialInterface::GetVectorParameterChannelNames(const FHashedMaterialParameterInfo& ParameterInfo, FParameterChannelNames& OutValue) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::Vector, ParameterInfo, Result, EMaterialGetParameterValueFlags::CheckNonOverrides))
	{
		OutValue = Result.ChannelNames;
		return true;
	}
	return false;
}

bool UMaterialInterface::GetScalarParameterSliderMinMax(const FHashedMaterialParameterInfo& ParameterInfo, float& OutSliderMin, float& OutSliderMax) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::Scalar, ParameterInfo, Result, EMaterialGetParameterValueFlags::CheckNonOverrides))
	{
		OutSliderMin = Result.ScalarMin;
		OutSliderMax = Result.ScalarMax;
		return true;
	}
	return false;
}
#endif // WITH_EDITOR

bool UMaterialInterface::GetScalarParameterValue(const FHashedMaterialParameterInfo& ParameterInfo, float& OutValue, bool bOveriddenOnly) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::Scalar, ParameterInfo, Result, MakeParameterValueFlags(bOveriddenOnly)))
	{
		OutValue = Result.Value.AsScalar();
		return true;
	}
	return false;
}

bool UMaterialInterface::GetParameterValue(EMaterialParameterType Type, const FMemoryImageMaterialParameterInfo& ParameterInfo, FMaterialParameterMetadata& OutValue, EMaterialGetParameterValueFlags Flags) const
{
	return false;
}

#if WITH_EDITOR
bool UMaterialInterface::IsScalarParameterUsedAsAtlasPosition(const FHashedMaterialParameterInfo& ParameterInfo, bool& OutValue, TSoftObjectPtr<class UCurveLinearColor>& Curve, TSoftObjectPtr<class UCurveLinearColorAtlas>& Atlas) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::Scalar, ParameterInfo, Result, EMaterialGetParameterValueFlags::CheckNonOverrides))
	{
		OutValue = Result.bUsedAsAtlasPosition;
		Curve = Result.ScalarCurve;
		Atlas = Result.ScalarAtlas;
		return true;
	}
	return false;
}
#endif // WITH_EDITOR

bool UMaterialInterface::GetTextureParameterValue(const FHashedMaterialParameterInfo& ParameterInfo, UTexture*& OutValue, bool bOveriddenOnly) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::Texture, ParameterInfo, Result, MakeParameterValueFlags(bOveriddenOnly)))
	{
		OutValue = Result.Value.Texture;
		return true;
	}
	return false;
}

bool UMaterialInterface::GetRuntimeVirtualTextureParameterValue(const FHashedMaterialParameterInfo& ParameterInfo, URuntimeVirtualTexture*& OutValue, bool bOveriddenOnly) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::RuntimeVirtualTexture, ParameterInfo, Result, MakeParameterValueFlags(bOveriddenOnly)))
	{
		OutValue = Result.Value.RuntimeVirtualTexture;
		return true;
	}
	return false;
}

#if WITH_EDITOR
bool UMaterialInterface::GetTextureParameterChannelNames(const FHashedMaterialParameterInfo& ParameterInfo, FParameterChannelNames& OutValue) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::Texture, ParameterInfo, Result, EMaterialGetParameterValueFlags::CheckNonOverrides))
	{
		OutValue = Result.ChannelNames;
		return true;
	}
	return false;
}
#endif

bool UMaterialInterface::GetFontParameterValue(const FHashedMaterialParameterInfo& ParameterInfo, class UFont*& OutFontValue, int32& OutFontPage, bool bOveriddenOnly) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterValue(EMaterialParameterType::RuntimeVirtualTexture, ParameterInfo, Result, MakeParameterValueFlags(bOveriddenOnly)))
	{
		OutFontValue = Result.Value.Font.Value;
		OutFontPage = Result.Value.Font.Page;
		return true;
	}
	return false;
}

bool UMaterialInterface::GetParameterDefaultValue(EMaterialParameterType Type, const FMemoryImageMaterialParameterInfo& ParameterInfo, FMaterialParameterMetadata& OutValue) const
{
	return GetParameterValue(Type, ParameterInfo, OutValue, EMaterialGetParameterValueFlags::CheckNonOverrides);
}


bool UMaterialInterface::GetScalarParameterDefaultValue(const FHashedMaterialParameterInfo& ParameterInfo, float& OutValue) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterDefaultValue(EMaterialParameterType::Scalar, ParameterInfo, Result))
	{
		OutValue = Result.Value.AsScalar();
		return true;
	}
	return false;
}

bool UMaterialInterface::GetVectorParameterDefaultValue(const FHashedMaterialParameterInfo& ParameterInfo, FLinearColor& OutValue) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterDefaultValue(EMaterialParameterType::Vector, ParameterInfo, Result))
	{
		OutValue = Result.Value.AsLinearColor();
		return true;
	}
	return false;
}

bool UMaterialInterface::GetTextureParameterDefaultValue(const FHashedMaterialParameterInfo& ParameterInfo, class UTexture*& OutValue) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterDefaultValue(EMaterialParameterType::Texture, ParameterInfo, Result))
	{
		OutValue = Result.Value.Texture;
		return true;
	}
	return false;
}

bool UMaterialInterface::GetRuntimeVirtualTextureParameterDefaultValue(const FHashedMaterialParameterInfo& ParameterInfo, class URuntimeVirtualTexture*& OutValue) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterDefaultValue(EMaterialParameterType::RuntimeVirtualTexture, ParameterInfo, Result))
	{
		OutValue = Result.Value.RuntimeVirtualTexture;
		return true;
	}
	return false;
}

bool UMaterialInterface::GetFontParameterDefaultValue(const FHashedMaterialParameterInfo& ParameterInfo, class UFont*& OutFontValue, int32& OutFontPage) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterDefaultValue(EMaterialParameterType::RuntimeVirtualTexture, ParameterInfo, Result))
	{
		OutFontValue = Result.Value.Font.Value;
		OutFontPage = Result.Value.Font.Page;
		return true;
	}
	return false;
}


#if WITH_EDITOR
bool UMaterialInterface::GetStaticSwitchParameterDefaultValue(const FHashedMaterialParameterInfo& ParameterInfo, bool& OutValue, FGuid& OutExpressionGuid) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterDefaultValue(EMaterialParameterType::StaticSwitch, ParameterInfo, Result))
	{
		OutExpressionGuid = Result.ExpressionGuid;
		OutValue = Result.Value.AsStaticSwitch();
		return true;
	}
	return false;
}

bool UMaterialInterface::GetStaticComponentMaskParameterDefaultValue(const FHashedMaterialParameterInfo& ParameterInfo, bool& OutR, bool& OutG, bool& OutB, bool& OutA, FGuid& OutExpressionGuid) const
{
	FMaterialParameterMetadata Result;
	if (GetParameterDefaultValue(EMaterialParameterType::StaticComponentMask, ParameterInfo, Result))
	{
		OutExpressionGuid = Result.ExpressionGuid;
		OutR = Result.Value.Bool[0];
		OutG = Result.Value.Bool[1];
		OutB = Result.Value.Bool[2];
		OutA = Result.Value.Bool[3];
		return true;
	}
	return false;
}

#endif // WITH_EDITOR

void UMaterialInterface::GetAllParametersOfType(EMaterialParameterType Type, TMap<FMaterialParameterInfo, FMaterialParameterMetadata>& OutParameters) const
{
	OutParameters.Reset();
	GetCachedExpressionData().Parameters.GetAllParametersOfType(Type, OutParameters);
}

void UMaterialInterface::GetAllParameterInfoOfType(EMaterialParameterType Type, TArray<FMaterialParameterInfo>& OutParameterInfo, TArray<FGuid>& OutParameterIds) const
{
	OutParameterInfo.Reset();
	OutParameterIds.Reset();
	GetCachedExpressionData().Parameters.GetAllParameterInfoOfType(Type, OutParameterInfo, OutParameterIds);
}

void UMaterialInterface::GetAllScalarParameterInfo(TArray<FMaterialParameterInfo>& OutParameterInfo, TArray<FGuid>& OutParameterIds) const
{
	GetAllParameterInfoOfType(EMaterialParameterType::Scalar, OutParameterInfo, OutParameterIds);
}

void UMaterialInterface::GetAllVectorParameterInfo(TArray<FMaterialParameterInfo>& OutParameterInfo, TArray<FGuid>& OutParameterIds) const
{
	GetAllParameterInfoOfType(EMaterialParameterType::Vector, OutParameterInfo, OutParameterIds);
}

void UMaterialInterface::GetAllTextureParameterInfo(TArray<FMaterialParameterInfo>& OutParameterInfo, TArray<FGuid>& OutParameterIds) const
{
	GetAllParameterInfoOfType(EMaterialParameterType::Texture, OutParameterInfo, OutParameterIds);
}

void UMaterialInterface::GetAllRuntimeVirtualTextureParameterInfo(TArray<FMaterialParameterInfo>& OutParameterInfo, TArray<FGuid>& OutParameterIds) const
{
	GetAllParameterInfoOfType(EMaterialParameterType::RuntimeVirtualTexture, OutParameterInfo, OutParameterIds);
}

void UMaterialInterface::GetAllFontParameterInfo(TArray<FMaterialParameterInfo>& OutParameterInfo, TArray<FGuid>& OutParameterIds) const
{
	GetAllParameterInfoOfType(EMaterialParameterType::Font, OutParameterInfo, OutParameterIds);
}

#if WITH_EDITORONLY_DATA
void UMaterialInterface::GetAllStaticSwitchParameterInfo(TArray<FMaterialParameterInfo>&OutParameterInfo, TArray<FGuid>&OutParameterIds) const
{
	GetAllParameterInfoOfType(EMaterialParameterType::StaticSwitch, OutParameterInfo, OutParameterIds);
}

void UMaterialInterface::GetAllStaticComponentMaskParameterInfo(TArray<FMaterialParameterInfo>&OutParameterInfo, TArray<FGuid>&OutParameterIds) const
{
	GetAllParameterInfoOfType(EMaterialParameterType::StaticComponentMask, OutParameterInfo, OutParameterIds);
}
#endif // WITH_EDITOR

bool UMaterialInterface::GetRefractionSettings(float& OutBiasValue) const
{
	return false;
}

#if WITH_EDITOR
bool UMaterialInterface::GetParameterDesc(const FHashedMaterialParameterInfo& ParameterInfo, FString& OutDesc) const
{
	for (int32 TypeIndex = 0; TypeIndex < NumMaterialParameterTypes; ++TypeIndex)
	{
		FMaterialParameterMetadata Meta;
		if (GetParameterValue((EMaterialParameterType)TypeIndex, ParameterInfo, Meta, EMaterialGetParameterValueFlags::CheckNonOverrides))
		{
			OutDesc = Meta.Description;
			return true;
		}
	}
	return false;
}

bool UMaterialInterface::GetGroupName(const FHashedMaterialParameterInfo& ParameterInfo, FName& OutDesc) const
{
	for (int32 TypeIndex = 0; TypeIndex < NumMaterialParameterTypes; ++TypeIndex)
	{
		FMaterialParameterMetadata Meta;
		if (GetParameterValue((EMaterialParameterType)TypeIndex, ParameterInfo, Meta, EMaterialGetParameterValueFlags::CheckNonOverrides))
		{
			OutDesc = Meta.Group;
			return true;
		}
	}
	return false;
}

bool UMaterialInterface::GetParameterSortPriority(const FHashedMaterialParameterInfo& ParameterInfo, int32& OutSortPriority) const
{
	for (int32 TypeIndex = 0; TypeIndex < NumMaterialParameterTypes; ++TypeIndex)
	{
		FMaterialParameterMetadata Meta;
		if (GetParameterValue((EMaterialParameterType)TypeIndex, ParameterInfo, Meta, EMaterialGetParameterValueFlags::CheckNonOverrides))
		{
			OutSortPriority = Meta.SortPriority;
			return true;
		}
	}
	return false;
}
#endif // WITH_EDITOR

UMaterial* UMaterialInterface::GetBaseMaterial()
{
	return GetMaterial();
}

bool DoesMaterialUseTexture(const UMaterialInterface* Material,const UTexture* CheckTexture)
{
	//Do not care if we're running dedicated server
	if (FPlatformProperties::IsServerOnly())
	{
		return false;
	}

	TArray<UTexture*> Textures;
	Material->GetUsedTextures(Textures, EMaterialQualityLevel::Num, true, GMaxRHIFeatureLevel, true);
	for (int32 i = 0; i < Textures.Num(); i++)
	{
		if (Textures[i] == CheckTexture)
		{
			return true;
		}
	}
	return false;
}

float UMaterialInterface::GetOpacityMaskClipValue() const
{
	return 0.0f;
}

EBlendMode UMaterialInterface::GetBlendMode() const
{
	return BLEND_Opaque;
}

bool UMaterialInterface::IsTwoSided() const
{
	return false;
}

bool UMaterialInterface::IsDitheredLODTransition() const
{
	return false;
}

bool UMaterialInterface::IsTranslucencyWritingCustomDepth() const
{
	return false;
}

bool UMaterialInterface::IsTranslucencyWritingVelocity() const
{
	return false;
}

bool UMaterialInterface::IsMasked() const
{
	return false;
}

bool UMaterialInterface::IsDeferredDecal() const
{
	return false;
}
bool UMaterialInterface::GetCastDynamicShadowAsMasked() const
{
	return false;
}

FMaterialShadingModelField UMaterialInterface::GetShadingModels() const
{
	return MSM_DefaultLit;
}

bool UMaterialInterface::IsShadingModelFromMaterialExpression() const
{
	return false;
}

USubsurfaceProfile* UMaterialInterface::GetSubsurfaceProfile_Internal() const
{
	return NULL;
}

bool UMaterialInterface::CastsRayTracedShadows() const
{
	return true;
}

void UMaterialInterface::SetFeatureLevelToCompile(ERHIFeatureLevel::Type FeatureLevel, bool bShouldCompile)
{
	uint32 FeatureLevelBit = (1 << FeatureLevel);
	if (bShouldCompile)
	{
		FeatureLevelsToForceCompile |= FeatureLevelBit;
	}
	else
	{
		FeatureLevelsToForceCompile &= (~FeatureLevelBit);
	}
}

uint32 UMaterialInterface::FeatureLevelsForAllMaterials = 0;

void UMaterialInterface::SetGlobalRequiredFeatureLevel(ERHIFeatureLevel::Type FeatureLevel, bool bShouldCompile)
{
	uint32 FeatureLevelBit = (1 << FeatureLevel);
	if (bShouldCompile)
	{
		FeatureLevelsForAllMaterials |= FeatureLevelBit;
	}
	else
	{
		FeatureLevelsForAllMaterials &= (~FeatureLevelBit);
	}
}


uint32 UMaterialInterface::GetFeatureLevelsToCompileForRendering() const
{
	return FeatureLevelsToForceCompile | GetFeatureLevelsToCompileForAllMaterials();
}


void UMaterialInterface::UpdateMaterialRenderProxy(FMaterialRenderProxy& Proxy)
{
	// no 0 pointer
	check(&Proxy);

	FMaterialShadingModelField MaterialShadingModels = GetShadingModels();

	// for better performance we only update SubsurfaceProfileRT if the feature is used
	if (UseSubsurfaceProfile(MaterialShadingModels))
	{
		USubsurfaceProfile* LocalSubsurfaceProfile = GetSubsurfaceProfile_Internal();
		
		FSubsurfaceProfileStruct Settings;
		if (LocalSubsurfaceProfile)
		{
			Settings = LocalSubsurfaceProfile->Settings;
		}

		FMaterialRenderProxy* InProxy = &Proxy;
		ENQUEUE_RENDER_COMMAND(UpdateMaterialRenderProxySubsurface)(
			[Settings, LocalSubsurfaceProfile, InProxy](FRHICommandListImmediate& RHICmdList)
			{
				if (LocalSubsurfaceProfile)
				{
					const uint32 AllocationId = GSubsurfaceProfileTextureObject.AddOrUpdateProfile(Settings, LocalSubsurfaceProfile);
					check(AllocationId >= 0 && AllocationId <= 255);
				}
				InProxy->SetSubsurfaceProfileRT(LocalSubsurfaceProfile);
			});
	}
}

bool FMaterialTextureInfo::IsValid(bool bCheckTextureIndex) const
{ 
#if WITH_EDITORONLY_DATA
	if (bCheckTextureIndex && (TextureIndex < 0 || TextureIndex >= TEXSTREAM_MAX_NUM_TEXTURES_PER_MATERIAL))
	{
		return false;
	}
#endif
	return TextureName != NAME_None && SamplingScale > SMALL_NUMBER && UVChannelIndex >= 0 && UVChannelIndex < TEXSTREAM_MAX_NUM_UVCHANNELS; 
}

void UMaterialInterface::SortTextureStreamingData(bool bForceSort, bool bFinalSort)
{
#if WITH_EDITOR
	// In cook that was already done in the save.
	if (!bTextureStreamingDataSorted || bForceSort)
	{
		TSet<const UTexture*> UsedTextures;
		if (bFinalSort)
		{
			TSet<const UTexture*> UnfilteredUsedTextures;
			GetReferencedTexturesAndOverrides(UnfilteredUsedTextures);

			// Sort some of the conditions that could make the texture unstreamable, to make the data leaner.
			// Note that because we are cooking, UStreamableRenderAsset::bIsStreamable is not reliable here.
			for (const UTexture* UnfilteredTexture : UnfilteredUsedTextures)
			{
				if (UnfilteredTexture && !UnfilteredTexture->NeverStream && UnfilteredTexture->LODGroup != TEXTUREGROUP_UI && UnfilteredTexture->MipGenSettings != TMGS_NoMipmaps)
				{
					UsedTextures.Add(UnfilteredTexture);
				}
			}
		}

		for (int32 Index = 0; Index < TextureStreamingData.Num(); ++Index)
		{
			FMaterialTextureInfo& TextureData = TextureStreamingData[Index];
			UTexture* Texture = Cast<UTexture>(TextureData.TextureReference.ResolveObject());

			// Also, when cooking, only keep textures that are directly referenced by this material to prevent non-deterministic cooking.
			// This would happen if a texture reference resolves to a texture not used anymore by this material. The resolved object could then be valid or not.
			if (Texture && (!bFinalSort || UsedTextures.Contains(Texture)))
			{
				TextureData.TextureName = Texture->GetFName();
			}
			else if (bFinalSort) // In the final sort we remove null names as they will never match.
			{
				TextureStreamingData.RemoveAtSwap(Index);
				--Index;
			}
			else
			{
				TextureData.TextureName = NAME_None;
			}
		}

		// Sort by name to be compatible with FindTextureStreamingDataIndexRange
		TextureStreamingData.Sort([](const FMaterialTextureInfo& Lhs, const FMaterialTextureInfo& Rhs) 
		{ 
			// Sort by register indices when the name are the same, as when initially added in the streaming data.
			if (Lhs.TextureName == Rhs.TextureName)
			{
				return Lhs.TextureIndex < Rhs.TextureIndex;

			}
			return Lhs.TextureName.LexicalLess(Rhs.TextureName); 
		});
		bTextureStreamingDataSorted = true;
	}
#endif // WITH_EDITOR
}

extern 	TAutoConsoleVariable<int32> CVarStreamingUseMaterialData;

bool UMaterialInterface::FindTextureStreamingDataIndexRange(FName TextureName, int32& LowerIndex, int32& HigherIndex) const
{
#if WITH_EDITORONLY_DATA
	// Because of redirectors (when textures are renammed), the texture names might be invalid and we need to udpate the data at every load.
	// Normally we would do that in the post load, but since the process needs to resolve the SoftObjectPaths, this is forbidden at that place.
	// As a workaround, we do it on demand. Note that this is not required in cooked build as it is done in the presave.
	const_cast<UMaterialInterface*>(this)->SortTextureStreamingData(false, false);
#endif

	if (CVarStreamingUseMaterialData.GetValueOnGameThread() == 0 || CVarStreamingUseNewMetrics.GetValueOnGameThread() == 0)
	{
		return false;
	}

	const int32 MatchingIndex = Algo::BinarySearchBy(TextureStreamingData, TextureName, &FMaterialTextureInfo::TextureName, FNameLexicalLess());
	if (MatchingIndex != INDEX_NONE)
	{
		// Find the range of entries for this texture. 
		// This is possible because the same texture could be bound to several register and also be used with different sampling UV.
		LowerIndex = MatchingIndex;
		HigherIndex = MatchingIndex;
		while (HigherIndex + 1 < TextureStreamingData.Num() && TextureStreamingData[HigherIndex + 1].TextureName == TextureName)
		{
			++HigherIndex;
		}
		return true;
	}
	return false;
}

void UMaterialInterface::SetTextureStreamingData(const TArray<FMaterialTextureInfo>& InTextureStreamingData)
{
	TextureStreamingData = InTextureStreamingData;
#if WITH_EDITORONLY_DATA
	bTextureStreamingDataSorted = false;
	TextureStreamingDataVersion = InTextureStreamingData.Num() ? MATERIAL_TEXTURE_STREAMING_DATA_VERSION : 0;
	TextureStreamingDataMissingEntries.Empty();
#endif
	SortTextureStreamingData(true, false);
}

float UMaterialInterface::GetTextureDensity(FName TextureName, const FMeshUVChannelInfo& UVChannelData) const
{
	ensure(UVChannelData.bInitialized);

	int32 LowerIndex = INDEX_NONE;
	int32 HigherIndex = INDEX_NONE;
	if (FindTextureStreamingDataIndexRange(TextureName, LowerIndex, HigherIndex))
	{
		// Compute the max, at least one entry will be valid. 
		float MaxDensity = 0;
		for (int32 Index = LowerIndex; Index <= HigherIndex; ++Index)
		{
			const FMaterialTextureInfo& MatchingData = TextureStreamingData[Index];
			ensure(MatchingData.IsValid() && MatchingData.TextureName == TextureName);
			MaxDensity = FMath::Max<float>(UVChannelData.LocalUVDensities[MatchingData.UVChannelIndex] / MatchingData.SamplingScale, MaxDensity);
		}
		return MaxDensity;
	}

	// Otherwise return 0 to indicate the data is not found.
	return 0;
}

bool UMaterialInterface::UseAnyStreamingTexture() const
{
	TArray<UTexture*> Textures;
	GetUsedTextures(Textures, EMaterialQualityLevel::Num, true, ERHIFeatureLevel::Num, true);

	for (UTexture* Texture : Textures)
	{
		if (Texture && Texture->IsStreamable())
		{
			return true;
		}
	}
	return false;
}

void UMaterialInterface::PreSave(const class ITargetPlatform* TargetPlatform)
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS;
	Super::PreSave(TargetPlatform);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS;
}

void UMaterialInterface::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	Super::PreSave(ObjectSaveContext);
	const ITargetPlatform* TargetPlatform = ObjectSaveContext.GetTargetPlatform();
	if (TargetPlatform && TargetPlatform->RequiresCookedData())
	{
		SortTextureStreamingData(true, true);
	}
}

void UMaterialInterface::AddAssetUserData(UAssetUserData* InUserData)
{
	if (InUserData != nullptr)
	{
		UAssetUserData* ExistingData = GetAssetUserDataOfClass(InUserData->GetClass());
		if (ExistingData != nullptr)
		{
			AssetUserData.Remove(ExistingData);
		}
		AssetUserData.Add(InUserData);
	}
}

UAssetUserData* UMaterialInterface::GetAssetUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIdx = 0; DataIdx < AssetUserData.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserData[DataIdx];
		if (Datum != nullptr && Datum->IsA(InUserDataClass))
		{
			return Datum;
		}
	}
	return nullptr;
}

void UMaterialInterface::RemoveUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIdx = 0; DataIdx < AssetUserData.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserData[DataIdx];
		if (Datum != nullptr && Datum->IsA(InUserDataClass))
		{
			AssetUserData.RemoveAt(DataIdx);
			return;
		}
	}
}

