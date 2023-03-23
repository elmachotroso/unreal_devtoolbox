// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/Texture.h"

#include "Misc/App.h"
#include "Modules/ModuleManager.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FeedbackContext.h"
#include "Misc/ScopeExit.h"
#include "HAL/LowLevelMemTracker.h"
#include "UObject/UObjectIterator.h"
#include "UObject/ObjectSaveContext.h"
#include "TextureResource.h"
#include "Engine/Texture2D.h"
#include "Streaming/TextureMipDataProvider.h"
#include "Engine/TextureMipDataProviderFactory.h"
#include "ContentStreaming.h"
#include "EngineUtils.h"
#include "Engine/AssetUserData.h"
#include "EditorSupportDelegates.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "EngineGlobals.h"
#include "Engine/Engine.h"
#include "Interfaces/ITargetPlatform.h"
#include "Engine/TextureLODSettings.h"
#include "RenderUtils.h"
#include "Rendering/StreamableTextureResource.h"
#include "TextureDerivedDataTask.h"
#include "Interfaces/ITextureFormat.h"
#include "Interfaces/ITextureFormatModule.h"
#include "UObject/UE5MainStreamObjectVersion.h"
#include "Compression/OodleDataCompression.h"
#include "Engine/TextureCube.h"
#include "Engine/RendererSettings.h"
#include "ColorSpace.h"

#if WITH_EDITOR
#include "DerivedDataBuildVersion.h"
#include "TextureCompiler.h"
#include "Misc/ScopeRWLock.h"
#endif

#if WITH_EDITORONLY_DATA
	#include "EditorFramework/AssetImportData.h"
#endif

static TAutoConsoleVariable<int32> CVarVirtualTextures(
	TEXT("r.VirtualTextures"),
	0,
	TEXT("Is virtual texture streaming enabled?"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly);

static TAutoConsoleVariable<int32> CVarMobileVirtualTextures(
	TEXT("r.Mobile.VirtualTextures"),
	0,
	TEXT("Whether virtual texture streaming is enabled on mobile platforms. Requires r.VirtualTextures enabled as well. \n"),
	ECVF_RenderThreadSafe | ECVF_ReadOnly
);

static TAutoConsoleVariable<int32> CVarVirtualTexturesAutoImport(
	TEXT("r.VT.EnableAutoImport"),
	1,
	TEXT("Enable virtual texture on texture import"),
	ECVF_Default);

DEFINE_LOG_CATEGORY(LogTexture);

#if STATS
DECLARE_STATS_GROUP(TEXT("Texture Group"), STATGROUP_TextureGroup, STATCAT_Advanced);

// Declare the stats for each Texture Group.
#define DECLARETEXTUREGROUPSTAT(Group) DECLARE_MEMORY_STAT(TEXT(#Group),STAT_##Group,STATGROUP_TextureGroup);
FOREACH_ENUM_TEXTUREGROUP(DECLARETEXTUREGROUPSTAT)
#undef DECLARETEXTUREGROUPSTAT


// Initialize TextureGroupStatFNames array with the FNames for each stats.
FName FTextureResource::TextureGroupStatFNames[TEXTUREGROUP_MAX] =
	{
		#define ASSIGNTEXTUREGROUPSTATNAME(Group) GET_STATFNAME(STAT_##Group),
		FOREACH_ENUM_TEXTUREGROUP(ASSIGNTEXTUREGROUPSTATNAME)
		#undef ASSIGNTEXTUREGROUPSTATNAME
	};
#endif

// This is used to prevent the PostEditChange to automatically update the material dependencies & material context, in some case we want to manually control this
// to be more efficient.
ENGINE_API bool GDisableAutomaticTextureMaterialUpdateDependencies = false;

UTexture::FOnTextureSaved UTexture::PreSaveEvent;

static FName CachedGetLatestOodleSdkVersion();

UTexture::UTexture(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, PrivateResource(nullptr)
	, PrivateResourceRenderThread(nullptr)
#if WITH_TEXTURE_RESOURCE_DEPRECATIONS
	, Resource(
		[this]()-> FTextureResource* { return GetResource(); },
		[this](FTextureResource* InTextureResource) { SetResource(InTextureResource); })
#endif
{
	SRGB = true;
	Filter = TF_Default;
	MipLoadOptions = ETextureMipLoadOptions::Default;
#if WITH_EDITORONLY_DATA
	SourceColorSettings = FTextureSourceColorSettings();
	AdjustBrightness = 1.0f;
	AdjustBrightnessCurve = 1.0f;
	AdjustVibrance = 0.0f;
	AdjustSaturation = 1.0f;
	AdjustRGBCurve = 1.0f;
	AdjustHue = 0.0f;
	AdjustMinAlpha = 0.0f;
	AdjustMaxAlpha = 1.0f;
	MaxTextureSize = 0; // means no limitation
	MipGenSettings = TMGS_FromTextureGroup;
	CompositeTextureMode = CTM_NormalRoughnessToAlpha;
	CompositePower = 1.0f;
	bUseLegacyGamma = false;
	bIsImporting = false;
	bCustomPropertiesImported = false;
	bDoScaleMipsForAlphaCoverage = false;
	AlphaCoverageThresholds = FVector4(0, 0, 0, 0);
	PaddingColor = FColor::Black;
	ChromaKeyColor = FColorList::Magenta;
	ChromaKeyThreshold = 1.0f / 255.0f;
	VirtualTextureStreaming = 0;
	CompressionYCoCg = 0;
	Downscale = 0.f;
	DownscaleOptions = ETextureDownscaleOptions::Default;
#endif // #if WITH_EDITORONLY_DATA

	if (FApp::CanEverRender() && !IsTemplate())
	{
		TextureReference.BeginInit_GameThread();
	}
}

const FTextureResource* UTexture::GetResource() const
{
	if (IsInParallelGameThread() || IsInGameThread() || IsInSlateThread())
	{
		return PrivateResource;
	}
	else if (IsInParallelRenderingThread() || IsInRHIThread())
	{
		return PrivateResourceRenderThread;
	}

	ensureMsgf(false, TEXT("Attempted to access a texture resource from an unkown thread."));
	return nullptr;
}

FTextureResource* UTexture::GetResource()
{
	if (IsInParallelGameThread() || IsInGameThread() || IsInSlateThread())
	{
		return PrivateResource;
	}
	else if (IsInParallelRenderingThread() || IsInRHIThread())
	{
		return PrivateResourceRenderThread;
	}

	ensureMsgf(false, TEXT("Attempted to access a texture resource from an unkown thread."));
	return nullptr;
}

void UTexture::SetResource(FTextureResource* InResource)
{
	check (!IsInActualRenderingThread() && !IsInRHIThread());

	// Each PrivateResource value must be updated in it's own thread because any
	// rendering code trying to access the Resource from this UTexture will
	// crash if it suddenly sees nullptr or a new resource that has not had it's InitRHI called.

	PrivateResource = InResource;
	ENQUEUE_RENDER_COMMAND(SetResourceRenderThread)([this, InResource](FRHICommandListImmediate& RHICmdList)
	{
		PrivateResourceRenderThread = InResource;
	});
}

void UTexture::ReleaseResource()
{
	if (PrivateResource)
	{
		UnlinkStreaming();

		// When using PlatformData, the resource shouldn't be released before it is initialized to prevent threading issues
		// where the platform data could be updated at the same time InitRHI is reading it on the renderthread.
		if (GetRunningPlatformData())
		{
			WaitForPendingInitOrStreaming();
		}

		CachedSRRState.Clear();

		FTextureResource* ToDelete = PrivateResource;
		// Free the resource.
		SetResource(nullptr);
		ENQUEUE_RENDER_COMMAND(DeleteResource)([ToDelete](FRHICommandListImmediate& RHICmdList)
		{
			ToDelete->ReleaseResource();
			delete ToDelete;
		});
	}
}

void UTexture::UpdateResource()
{
	// Release the existing texture resource.
	ReleaseResource();

	//Dedicated servers have no texture internals
	if( FApp::CanEverRender() && !HasAnyFlags(RF_ClassDefaultObject) )
	{
		// Create a new texture resource.
		FTextureResource* NewResource = CreateResource();
		SetResource(NewResource);
		if (NewResource)
		{
			LLM_SCOPE(ELLMTag::Textures);
			if (FStreamableTextureResource* StreamableResource = NewResource->GetStreamableTextureResource())
			{
				// State the gamethread coherent resource state.
				CachedSRRState = StreamableResource->GetPostInitState();
				if (CachedSRRState.IsValid())
				{
					// Cache the pending InitRHI flag.
					CachedSRRState.bHasPendingInitHint = true;
				}
			}

			// Init the texture reference, which needs to be set from a render command, since TextureReference.TextureReferenceRHI is gamethread coherent.
			ENQUEUE_RENDER_COMMAND(SetTextureReference)([this, NewResource](FRHICommandListImmediate& RHICmdList)
			{
				NewResource->SetTextureReference(TextureReference.TextureReferenceRHI);
			});
			BeginInitResource(NewResource);
			// Now that the resource is ready for streaming, bind it to the streamer.
			LinkStreaming();
		}
	}
}

void UTexture::ExportCustomProperties(FOutputDevice& Out, uint32 Indent)
{
#if WITH_EDITOR
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		return;
	}

	// Texture source data export : first, make sure it is ready for export :
	FinishCachePlatformData();

	Source.ExportCustomProperties(Out, Indent);

	Out.Logf(TEXT("\r\n"));
#endif // WITH_EDITOR
}

void UTexture::ImportCustomProperties(const TCHAR* SourceText, FFeedbackContext* Warn)
{
#if WITH_EDITOR
	Source.ImportCustomProperties(SourceText, Warn);

	BeginCachePlatformData();

	bCustomPropertiesImported = true;
#endif // WITH_EDITOR
}

void UTexture::PostEditImport()
{
#if WITH_EDITOR
	bIsImporting = true;

	if (bCustomPropertiesImported)
	{
		FinishCachePlatformData();
	}
#endif // WITH_EDITOR
}

bool UTexture::IsPostLoadThreadSafe() const
{
	return false;
}

#if WITH_EDITOR

bool UTexture::IsDefaultTexture() const
{
	return false;
}

bool UTexture::Modify(bool bAlwaysMarkDirty)
{
	// Before applying any modification to the texture
	// make sure no compilation is still ongoing.
	if (!IsAsyncCacheComplete())
	{
		FinishCachePlatformData();
	}	
	
	if (IsDefaultTexture())
	{
		FTextureCompilingManager::Get().FinishCompilation({this});
	}

	return Super::Modify(bAlwaysMarkDirty);
}

bool UTexture::CanEditChange(const FProperty* InProperty) const
{
	if (InProperty)
	{
		const FName PropertyName = InProperty->GetFName();

		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(UTexture, AdjustVibrance))
		{
			return !HasHDRSource();
		}
		
		// Only enable chromatic adapation method when the white points differ.
		if (PropertyName == GET_MEMBER_NAME_STRING_CHECKED(FTextureSourceColorSettings, ChromaticAdaptationMethod))
		{
			if (SourceColorSettings.ColorSpace == ETextureColorSpace::TCS_None)
			{
				return false;
			}

			const URendererSettings* Settings = GetDefault<URendererSettings>();
			return !Settings->WhiteChromaticityCoordinate.Equals(SourceColorSettings.WhiteChromaticityCoordinate);
		}

		// Virtual Texturing is only supported for Texture2D 
		static const FName VirtualTextureStreamingName = GET_MEMBER_NAME_CHECKED(UTexture, VirtualTextureStreaming);
		if (PropertyName == VirtualTextureStreamingName)
		{
			return this->IsA<UTexture2D>();
		}
	}

	return true;
}

void UTexture::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UTexture_PostEditChangeProperty);

	Super::PostEditChangeProperty(PropertyChangedEvent);

#if WITH_EDITOR
	ON_SCOPE_EXIT
	{
		// PostEditChange is the last step in the import sequence (PreEditChange/PostEditImport/PostEditChange, called twice : see further details) so reset the import-related flags here:
		bIsImporting = false;
		bCustomPropertiesImported = false;
	};

	// When PostEditChange is called as part of the import process (PostEditImport has just been called), it may be called twice : once for the (sub-)object declaration, and once for the definition, the latter being 
	//  when ImportCustomProperties is called. Because texture bulk data is only being copied to in ImportCustomProperties, it's invalid to do anything the first time so we postpone it to the second call :
	if (bIsImporting && !bCustomPropertiesImported)
	{
		return;
	}
#endif // WITH_EDITOR

	SetLightingGuid();

	// Determine whether any property that requires recompression of the texture, or notification to Materials has changed.
	bool RequiresNotifyMaterials = false;
	bool DeferCompressionWasEnabled = false;
	bool bInvalidatesMaterialShaders = true;	// too conservative, but as to not change the current behavior

	FProperty* PropertyThatChanged = PropertyChangedEvent.Property;
	if( PropertyThatChanged )
	{
		static const FName CompressionSettingsName = GET_MEMBER_NAME_CHECKED(UTexture, CompressionSettings);
		static const FName LODGroupName = GET_MEMBER_NAME_CHECKED(UTexture, LODGroup);
		static const FName DeferCompressionName = GET_MEMBER_NAME_CHECKED(UTexture, DeferCompression);
		static const FName SrgbName = GET_MEMBER_NAME_CHECKED(UTexture, SRGB);
		static const FName VirtualTextureStreamingName = GET_MEMBER_NAME_CHECKED(UTexture, VirtualTextureStreaming);
#if WITH_EDITORONLY_DATA
		static const FName SourceColorSpaceName = GET_MEMBER_NAME_CHECKED(FTextureSourceColorSettings, ColorSpace);
		static const FName MaxTextureSizeName = GET_MEMBER_NAME_CHECKED(UTexture, MaxTextureSize);
		static const FName CompressionQualityName = GET_MEMBER_NAME_CHECKED(UTexture, CompressionQuality);
		static const FName OodleTextureSdkVersionName = GET_MEMBER_NAME_CHECKED(UTexture, OodleTextureSdkVersion);
#endif //WITH_EDITORONLY_DATA

		const FName PropertyName = PropertyThatChanged->GetFName();

		if ((PropertyName == CompressionSettingsName) ||
			(PropertyName == LODGroupName) ||
			(PropertyName == SrgbName))
		{
			RequiresNotifyMaterials = true;

			if (PropertyName == LODGroupName)
			{
				if (LODGroup == TEXTUREGROUP_8BitData)
				{
					CompressionSettings = TC_VectorDisplacementmap;
					SRGB = false;
					Filter = TF_Default;
					MipGenSettings = TMGS_FromTextureGroup;
				}
				else if (LODGroup == TEXTUREGROUP_16BitData)
				{
					CompressionSettings = TC_HDR;
					SRGB = false;
					Filter = TF_Default;
					MipGenSettings = TMGS_FromTextureGroup;
				}
			}
		}
		else if (PropertyName == DeferCompressionName)
		{
			DeferCompressionWasEnabled = DeferCompression;
		}
#if WITH_EDITORONLY_DATA
		else if (PropertyName == SourceColorSpaceName)
		{
			// Update the chromaticity coordinates member variables based on the color space choice (unless custom).
			if (SourceColorSettings.ColorSpace != ETextureColorSpace::TCS_Custom)
			{
				UE::Color::FColorSpace ColorSpace(static_cast<UE::Color::EColorSpace>(SourceColorSettings.ColorSpace));
				ColorSpace.GetChromaticities(SourceColorSettings.RedChromaticityCoordinate, SourceColorSettings.GreenChromaticityCoordinate, SourceColorSettings.BlueChromaticityCoordinate, SourceColorSettings.WhiteChromaticityCoordinate);
			}
		}
		else if (PropertyName == CompressionQualityName)
		{
			RequiresNotifyMaterials = true;
			bInvalidatesMaterialShaders = false;
		}
		else if (PropertyName == MaxTextureSizeName)
		{
			if (MaxTextureSize <= 0)
			{
				MaxTextureSize = 0;
			}
			else
			{
				MaxTextureSize = FMath::Min<int32>(FMath::RoundUpToPowerOfTwo(MaxTextureSize), GetMaximumDimension());
			}
		}
		else if (PropertyName == VirtualTextureStreamingName)
		{
			RequiresNotifyMaterials = true;
		}
		else if (PropertyName == OodleTextureSdkVersionName)
		{
			// if you write "latest" in editor it becomes the number of the latest version
			static const FName NameLatest("latest");
			static const FName NameCurrent("current");
			if ( OodleTextureSdkVersion == NameLatest || OodleTextureSdkVersion == NameCurrent )
			{
				OodleTextureSdkVersion = CachedGetLatestOodleSdkVersion();
			}
		}
#endif //WITH_EDITORONLY_DATA
	}

	const bool bPreventSRGB = (CompressionSettings == TC_Alpha || CompressionSettings == TC_Normalmap || CompressionSettings == TC_Masks || CompressionSettings == TC_HDR || CompressionSettings == TC_HDR_Compressed || CompressionSettings == TC_HalfFloat);
	if (bPreventSRGB && SRGB == true)
	{
		SRGB = false;
	}

	if (!PropertyThatChanged && !GDisableAutomaticTextureMaterialUpdateDependencies)
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateDependentMaterials);

		// Update any material that uses this texture and must force a recompile of cache resource
		TArray<UMaterial*> MaterialsToUpdate;
		TSet<UMaterial*> BaseMaterialsThatUseThisTexture;
		for (TObjectIterator<UMaterialInterface> It; It; ++It)
		{
			UMaterialInterface* MaterialInterface = *It;
			if (DoesMaterialUseTexture(MaterialInterface, this))
			{
				UMaterial* Material = MaterialInterface->GetMaterial();
				bool MaterialAlreadyCompute = false;
				BaseMaterialsThatUseThisTexture.Add(Material, &MaterialAlreadyCompute);
				if (!MaterialAlreadyCompute)
				{
					if (Material->IsTextureForceRecompileCacheRessource(this))
					{
						MaterialsToUpdate.Add(Material);
						Material->UpdateMaterialShaderCacheAndTextureReferences();
					}
				}
			}
		}

		if (MaterialsToUpdate.Num())
		{
			FMaterialUpdateContext UpdateContext;

			for (UMaterial* MaterialToUpdate: MaterialsToUpdate)
			{
				UpdateContext.AddMaterial(MaterialToUpdate);
			}
		}
	}

	NumCinematicMipLevels = FMath::Max<int32>( NumCinematicMipLevels, 0 );

	// Don't update the texture resource if we've turned "DeferCompression" on, as this 
	// would cause it to immediately update as an uncompressed texture
	if( !DeferCompressionWasEnabled && (PropertyChangedEvent.ChangeType & EPropertyChangeType::Interactive) == 0 )
	{
		// Update the texture resource. This will recache derived data if necessary
		// which may involve recompressing the texture.
		UpdateResource();
	}

	// Notify any loaded material instances if changed our compression format
	if (RequiresNotifyMaterials)
	{
		NotifyMaterials(bInvalidatesMaterialShaders ? ENotifyMaterialsEffectOnShaders::Default : ENotifyMaterialsEffectOnShaders::DoesNotInvalidate);
	}
		
#if WITH_EDITORONLY_DATA
	// any texture that is referencing this texture as AssociatedNormalMap needs to be informed
	{
		TRACE_CPUPROFILER_EVENT_SCOPE(UpdateDependentTextures);

		TArray<UTexture*> TexturesThatUseThisTexture;

		for (TObjectIterator<UTexture> It; It; ++It)
		{
			UTexture* Tex = *It;

			if(Tex != this && Tex->CompositeTexture == this && Tex->CompositeTextureMode != CTM_Disabled)
			{
				TexturesThatUseThisTexture.Add(Tex);
			}
		}
		for (int32 i = 0; i < TexturesThatUseThisTexture.Num(); ++i)
		{
			TexturesThatUseThisTexture[i]->PostEditChange();
		}
	}
#endif

	for (UAssetUserData* Datum : AssetUserData)
	{
		if (Datum != nullptr)
		{
			Datum->PostEditChangeOwner();
		}
	}
}
#endif // WITH_EDITOR

void UTexture::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FUE5MainStreamObjectVersion::GUID);

	Super::Serialize(Ar);
	
	FStripDataFlags StripFlags(Ar);

	/** Legacy serialization. */
#if WITH_EDITORONLY_DATA
	if (!StripFlags.IsEditorDataStripped())
	{
#if WITH_EDITOR
		FWriteScopeLock BulkDataExclusiveScope(Source.BulkDataLock.Get());
#endif

		if (Ar.IsLoading() && Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) < FUE5MainStreamObjectVersion::TextureDoScaleMipsForAlphaCoverage)
		{
			// bDoScaleMipsForAlphaCoverage was not transmitted in old versions
			//	and AlphaCoverageThresholds was being incorrectly set to (0,0,0,1)
			check( bDoScaleMipsForAlphaCoverage == false );
	
			if ( AlphaCoverageThresholds != FVector4(0,0,0,0) && AlphaCoverageThresholds != FVector4(0,0,0,1) )
			{
				// AlphaCoverageThresholds is a non-default value, assume that means they wanted it on
				bDoScaleMipsForAlphaCoverage = true;
			}
			else if ( AlphaCoverageThresholds == FVector4(0,0,0,1) )
			{
				// if value is (0,0,0,1)
				//	that was previously incorrectly being set by default and enabling alpha coverage processing
				// we don't want that, but to optionally preserve old behavior you can set a config option :

				static struct ReadConfigOnce
				{
					bool bBool;
					ReadConfigOnce()
					{
						bBool = false;
						GConfig->GetBool(TEXT("Texture"), TEXT("EnableLegacyAlphaCoverageThresholdScaling"), bBool, GEditorIni);
					}
				} ConfigValue;

				bDoScaleMipsForAlphaCoverage = ConfigValue.bBool;
			}
		}

		if (Ar.IsLoading() && Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) < FUE5MainStreamObjectVersion::VirtualizedBulkDataHaveUniqueGuids)
		{
			if (Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) < FUE5MainStreamObjectVersion::TextureSourceVirtualization)
			{
				FByteBulkData TempBulkData;
				TempBulkData.Serialize(Ar, this);

				FGuid LegacyPersistentId = Source.GetId();
				Source.BulkData.CreateFromBulkData(TempBulkData, LegacyPersistentId, this);
			}
			else
			{
				Source.BulkData.Serialize(Ar, this, false /* bAllowRegister */);
				Source.BulkData.CreateLegacyUniqueIdentifier(this);
			}

		}
		else
		{
			Source.BulkData.Serialize(Ar, this);
		}
	}

	if (Ar.IsLoading())
	{
		// Could potentially guard this with a new custom version, but overhead of just checking on every load should be very small
		Source.EnsureBlocksAreSorted();
	}

	if ( GetLinkerUEVersion() < VER_UE4_TEXTURE_LEGACY_GAMMA )
	{
		bUseLegacyGamma = true;
	}

	if (Ar.IsCooking() && VirtualTextureStreaming)
	{
		if (UseVirtualTexturing(GMaxRHIFeatureLevel, Ar.CookingTarget()) == false)
		{
			UE_LOG(LogTexture, Display, TEXT("%s is marked for virtual streaming but virtual texture streaming is not available."), *GetPathName());
		}
	}

#endif // #if WITH_EDITORONLY_DATA
}

void UTexture::PostInitProperties()
{
#if WITH_EDITORONLY_DATA
	if (!HasAnyFlags(RF_ClassDefaultObject | RF_NeedLoad))
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));

		// OodleTextureSdkVersion = get latest sdk version
		//	this needs to get the actual version number so it will be IO'd frozen (not just "latest")
		OodleTextureSdkVersion = CachedGetLatestOodleSdkVersion();
	}
#endif
	Super::PostInitProperties();
}

void UTexture::PostLoad()
{
	Super::PostLoad();

#if WITH_EDITORONLY_DATA
	if (AssetImportData == nullptr)
	{
		AssetImportData = NewObject<UAssetImportData>(this, TEXT("AssetImportData"));
	}

	if (!SourceFilePath_DEPRECATED.IsEmpty())
	{
		FAssetImportInfo Info;
		Info.Insert(FAssetImportInfo::FSourceFile(SourceFilePath_DEPRECATED));
		AssetImportData->SourceData = MoveTemp(Info);
	}
#endif

	if( !IsTemplate() )
	{
		// Update cached LOD bias.
		UpdateCachedLODBias();

		// The texture will be cached by the cubemap it is contained within on consoles.
		UTextureCube* CubeMap = Cast<UTextureCube>(GetOuter());
		if (CubeMap == NULL)
		{
			// Recreate the texture's resource.
			UpdateResource();
		}
	}
}

void UTexture::BeginFinalReleaseResource()
{
	check(!bAsyncResourceReleaseHasBeenStarted);
	// Send the rendering thread a release message for the texture's resource.
	if (GetResource())
	{
		BeginReleaseResource(GetResource());
	}
	if (TextureReference.IsInitialized_GameThread())
	{
		TextureReference.BeginRelease_GameThread();
	}
	ReleaseFence.BeginFence();
	// Keep track that we already kicked off the async release.
	bAsyncResourceReleaseHasBeenStarted = true;
}


void UTexture::BeginDestroy()
{
	Super::BeginDestroy();

	if (!HasPendingInitOrStreaming())
	{
		BeginFinalReleaseResource();
	}
}

bool UTexture::IsReadyForFinishDestroy()
{
#if WITH_EDITOR
	// We're being garbage collected and might still have async tasks pending
	if (!TryCancelCachePlatformData())
	{
		return false;
	}
#endif

	if (!Super::IsReadyForFinishDestroy())
	{
		return false;
	}
	if (!bAsyncResourceReleaseHasBeenStarted)
	{
		BeginFinalReleaseResource();
	}
	return ReleaseFence.IsFenceComplete();
}

void UTexture::FinishDestroy()
{
	Super::FinishDestroy();

	check(!bAsyncResourceReleaseHasBeenStarted || ReleaseFence.IsFenceComplete());
	check(TextureReference.IsInitialized_GameThread() == false);

	if(PrivateResource)
	{
		// Free the resource.
		delete PrivateResource;
		PrivateResource = NULL;
	}

	CleanupCachedRunningPlatformData();
#if WITH_EDITOR
	if (!GExitPurge)
	{
		ClearAllCachedCookedPlatformData();
	}
#endif
}

void UTexture::PreSave(const class ITargetPlatform* TargetPlatform)
{
	PRAGMA_DISABLE_DEPRECATION_WARNINGS;
	Super::PreSave(TargetPlatform);
	PRAGMA_ENABLE_DEPRECATION_WARNINGS;
}

void UTexture::PreSave(FObjectPreSaveContext ObjectSaveContext)
{
	PreSaveEvent.Broadcast(this);

	Super::PreSave(ObjectSaveContext);

#if WITH_EDITOR
	if (DeferCompression)
	{
		GWarn->StatusUpdate( 0, 0, FText::Format( NSLOCTEXT("UnrealEd", "SavingPackage_CompressingTexture", "Compressing texture:  {0}"), FText::FromString(GetName()) ) );
		DeferCompression = false;
		UpdateResource();
	}

	// Ensure that compilation has finished before saving the package
	// otherwise async compilation might try to read the bulkdata
	// while it's being serialized to the package.
	// This also needs to happen before the source is modified below
	// because it invalidates the texture build due to source hash change
	// and could cause another build to be triggered during PostCompilation
	// causing reentrancy problems.
	FTextureCompilingManager::Get().FinishCompilation({ this });

	if (!GEngine->IsAutosaving() && !ObjectSaveContext.IsProceduralSave())
	{
		GWarn->StatusUpdate(0, 0, FText::Format(NSLOCTEXT("UnrealEd", "SavingPackage_CompressingSourceArt", "Compressing source art for texture:  {0}"), FText::FromString(GetName())));
		Source.Compress();
	}
#endif // #if WITH_EDITOR
}

#if WITH_EDITORONLY_DATA
void UTexture::GetAssetRegistryTags(TArray<FAssetRegistryTag>& OutTags) const
{
	if (AssetImportData)
	{
		OutTags.Add( FAssetRegistryTag(SourceFileTagName(), AssetImportData->GetSourceData().ToJson(), FAssetRegistryTag::TT_Hidden) );
	}

	OutTags.Add(FAssetRegistryTag("SourceCompression", Source.GetSourceCompressionAsString(), FAssetRegistryTag::TT_Alphabetical));

	Super::GetAssetRegistryTags(OutTags);
}
#endif

FIoFilenameHash UTexture::GetMipIoFilenameHash(const int32 MipIndex) const
{
	FTexturePlatformData** PlatformData = const_cast<UTexture*>(this)->GetRunningPlatformData();
	if (PlatformData && *PlatformData)
	{
		const TIndirectArray<struct FTexture2DMipMap>& PlatformMips = (*PlatformData)->Mips;
		if (PlatformMips.IsValidIndex(MipIndex))
		{
			return PlatformMips[MipIndex].BulkData.GetIoFilenameHash();
		}
	}
	return INVALID_IO_FILENAME_HASH;
}

bool UTexture::DoesMipDataExist(const int32 MipIndex) const
{
	FTexturePlatformData** PlatformData = const_cast<UTexture*>(this)->GetRunningPlatformData();
	if (PlatformData && *PlatformData)
	{
		const TIndirectArray<struct FTexture2DMipMap>& PlatformMips = (*PlatformData)->Mips;
		if (PlatformMips.IsValidIndex(MipIndex))
		{
			return PlatformMips[MipIndex].BulkData.DoesExist();
		}
	}
	return false;
}

bool UTexture::HasPendingRenderResourceInitialization() const
{
	return GetResource() && !GetResource()->IsInitialized();
}

bool UTexture::HasPendingLODTransition() const
{
	return GetResource() && GetResource()->MipBiasFade.IsFading();
}

float UTexture::GetLastRenderTimeForStreaming() const
{
	float LastRenderTime = -FLT_MAX;
	if (GetResource())
	{
		// The last render time is the last time the resource was directly bound or the last
		// time the texture reference was cached in a resource table, whichever was later.
		LastRenderTime = FMath::Max<double>(GetResource()->LastRenderTime,TextureReference.GetLastRenderTime());
	}
	return LastRenderTime;
}

void UTexture::InvalidateLastRenderTimeForStreaming()
{
	if (GetResource())
	{
		GetResource()->LastRenderTime = -FLT_MAX;
	}
	TextureReference.InvalidateLastRenderTime();
}


bool UTexture::ShouldMipLevelsBeForcedResident() const
{
	if (LODGroup == TEXTUREGROUP_Skybox || Super::ShouldMipLevelsBeForcedResident())
	{
		return true;
	}
	return false;
}

void UTexture::CancelPendingTextureStreaming()
{
	for( TObjectIterator<UTexture> It; It; ++It )
	{
		UTexture* CurrentTexture = *It;
		CurrentTexture->CancelPendingStreamingRequest();
	}

	// No need to call FlushResourceStreaming(), since calling CancelPendingMipChangeRequest has an immediate effect.
}

float UTexture::GetAverageBrightness(bool bIgnoreTrueBlack, bool bUseGrayscale)
{
	// Indicate the action was not performed...
	return -1.0f;
}

/** Helper functions for text output of texture properties... */
#ifndef CASE_ENUM_TO_TEXT
#define CASE_ENUM_TO_TEXT(txt) case txt: return TEXT(#txt);
#endif

#ifndef TEXT_TO_ENUM
#define TEXT_TO_ENUM(eVal, txt)		if (FCString::Stricmp(TEXT(#eVal), txt) == 0)	return eVal;
#endif

const TCHAR* UTexture::GetTextureGroupString(TextureGroup InGroup)
{
	switch (InGroup)
	{
		FOREACH_ENUM_TEXTUREGROUP(CASE_ENUM_TO_TEXT)
	}

	return TEXT("TEXTUREGROUP_World");
}

const TCHAR* UTexture::GetMipGenSettingsString(TextureMipGenSettings InEnum)
{
	switch(InEnum)
	{
		default:
		FOREACH_ENUM_TEXTUREMIPGENSETTINGS(CASE_ENUM_TO_TEXT)
	}
}

TextureMipGenSettings UTexture::GetMipGenSettingsFromString(const TCHAR* InStr, bool bTextureGroup)
{
#define TEXT_TO_MIPGENSETTINGS(m) TEXT_TO_ENUM(m, InStr);
	FOREACH_ENUM_TEXTUREMIPGENSETTINGS(TEXT_TO_MIPGENSETTINGS)
#undef TEXT_TO_MIPGENSETTINGS

	// default for TextureGroup and Texture is different
	return bTextureGroup ? TMGS_SimpleAverage : TMGS_FromTextureGroup;
}

void UTexture::SetDeterministicLightingGuid()
{
#if WITH_EDITORONLY_DATA
	// Compute a 128-bit hash based on the texture name and use that as a GUID to fix this issue.
	FTCHARToUTF8 Converted(*GetFullName());
	FMD5 MD5Gen;
	MD5Gen.Update((const uint8*)Converted.Get(), Converted.Length());
	uint32 Digest[4];
	MD5Gen.Final((uint8*)Digest);

	// FGuid::NewGuid() creates a version 4 UUID (at least on Windows), which will have the top 4 bits of the
	// second field set to 0100. We'll set the top bit to 1 in the GUID we create, to ensure that we can never
	// have a collision with textures which use implicitly generated GUIDs.
	Digest[1] |= 0x80000000;
	FGuid TextureGUID(Digest[0], Digest[1], Digest[2], Digest[3]);

	LightingGuid = TextureGUID;
#else
	LightingGuid = FGuid(0, 0, 0, 0);
#endif // WITH_EDITORONLY_DATA
}

UEnum* UTexture::GetPixelFormatEnum()
{
	// Lookup the pixel format enum so that the pixel format can be serialized by name.
	static FName PixelFormatUnknownName(TEXT("PF_Unknown"));
	static UEnum* PixelFormatEnum = NULL;
	if (PixelFormatEnum == NULL)
	{
		check(IsInGameThread());
		UEnum::LookupEnumName(PixelFormatUnknownName, &PixelFormatEnum);
		check(PixelFormatEnum);
	}
	return PixelFormatEnum;
}

void UTexture::PostCDOContruct()
{
	GetPixelFormatEnum();
}

bool UTexture::ForceUpdateTextureStreaming()
{
	if (!IStreamingManager::HasShutdown())
	{
#if WITH_EDITOR
		for( TObjectIterator<UTexture2D> It; It; ++It )
		{
			UTexture* Texture = *It;

			// Update cached LOD bias.
			Texture->UpdateCachedLODBias();
		}
#endif // #if WITH_EDITOR

		// Make sure we iterate over all textures by setting it to high value.
		IStreamingManager::Get().SetNumIterationsForNextFrame( 100 );
		// Update resource streaming with updated texture LOD bias/ max texture mip count.
		IStreamingManager::Get().UpdateResourceStreaming( 0 );
		// Block till requests are finished.
		IStreamingManager::Get().BlockTillAllRequestsFinished();
	}

	return true;
}

void UTexture::AddAssetUserData(UAssetUserData* InUserData)
{
	if (InUserData != NULL)
	{
		UAssetUserData* ExistingData = GetAssetUserDataOfClass(InUserData->GetClass());
		if (ExistingData != NULL)
		{
			AssetUserData.Remove(ExistingData);
		}
		AssetUserData.Add(InUserData);
	}
}

UAssetUserData* UTexture::GetAssetUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIdx = 0; DataIdx < AssetUserData.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserData[DataIdx];
		if (Datum != NULL && Datum->IsA(InUserDataClass))
		{
			return Datum;
		}
	}
	return NULL;
}

void UTexture::RemoveUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIdx = 0; DataIdx < AssetUserData.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserData[DataIdx];
		if (Datum != NULL && Datum->IsA(InUserDataClass))
		{
			AssetUserData.RemoveAt(DataIdx);
			return;
		}
	}
}

const TArray<UAssetUserData*>* UTexture::GetAssetUserDataArray() const
{
	return &ToRawPtrTArrayUnsafe(AssetUserData);
}

#if WITH_EDITOR
// Based on target platform, returns wether texture is candidate to be streamed.
// This method is used to decide if PrimitiveComponent's bHasNoStreamableTextures flag can be set to true.
// See ULevel::MarkNoStreamableTexturesPrimitiveComponents for details.
bool UTexture::IsCandidateForTextureStreaming(const ITargetPlatform* InTargetPlatform) const
{
	const bool bIsVirtualTextureStreaming = InTargetPlatform->SupportsFeature(ETargetPlatformFeatures::VirtualTextureStreaming) ? VirtualTextureStreaming : false;
	const bool bIsCandidateForTextureStreaming = InTargetPlatform->SupportsFeature(ETargetPlatformFeatures::TextureStreaming) && !bIsVirtualTextureStreaming;

	if (bIsCandidateForTextureStreaming &&
		!NeverStream &&
		LODGroup != TEXTUREGROUP_UI &&
		MipGenSettings != TMGS_NoMipmaps)
	{
		// If bCookedIsStreamable flag was previously computed, use it.
		if (bCookedIsStreamable.IsSet())
		{
			return *bCookedIsStreamable;
		}
		return true;
	}
	return false;
}
#endif

FStreamableRenderResourceState UTexture::GetResourcePostInitState(const FTexturePlatformData* PlatformData, bool bAllowStreaming, int32 MinRequestMipCount, int32 MaxMipCount, bool bSkipCanBeLoaded) const
{
	// Create the resource with a mip count limit taking in consideration the asset LODBias.
	// This ensures that the mip count stays constant when toggling asset streaming at runtime.
	const int32 NumMips = [&]() -> int32 
	{
		const int32 ExpectedAssetLODBias = FMath::Clamp<int32>(GetCachedLODBias() - NumCinematicMipLevels, 0, PlatformData->Mips.Num() - 1);
		const int32 MaxRuntimeMipCount = FMath::Min<int32>(GMaxTextureMipCount, FStreamableRenderResourceState::MAX_LOD_COUNT);
		if (MaxMipCount > 0)
		{
			return FMath::Min3<int32>(PlatformData->Mips.Num() - ExpectedAssetLODBias, MaxMipCount, MaxRuntimeMipCount);
		}
		else
		{
			return FMath::Min<int32>(PlatformData->Mips.Num() - ExpectedAssetLODBias, MaxRuntimeMipCount);
		}
	}();

	const int32 NumOfNonOptionalMips = FMath::Min<int32>(NumMips, PlatformData->GetNumNonOptionalMips());
	const int32 NumOfNonStreamingMips = FMath::Min<int32>(NumMips, PlatformData->GetNumNonStreamingMips());
	const int32 AssetMipIdxForResourceFirstMip = FMath::Max<int32>(0, PlatformData->Mips.Num() - NumMips);

	bool bMakeStreamble = false;
	int32 NumRequestedMips = 0;

#if PLATFORM_SUPPORTS_TEXTURE_STREAMING
	bool bWillProvideMipDataWithoutDisk = false;

	// Check if any of the CustomMipData providers associated with this texture can provide mip data even without DDC or disk,
	// if so, enable streaming for this texture
	for (UAssetUserData* UserData : AssetUserData)
	{
		UTextureMipDataProviderFactory* CustomMipDataProviderFactory = Cast<UTextureMipDataProviderFactory>(UserData);
		if (CustomMipDataProviderFactory)
		{
			bWillProvideMipDataWithoutDisk = CustomMipDataProviderFactory->WillProvideMipDataWithoutDisk();
			if (bWillProvideMipDataWithoutDisk)
			{
				break;
			}
		}
	}

	if (!NeverStream && 
		NumOfNonStreamingMips < NumMips && 
		LODGroup != TEXTUREGROUP_UI && 
		bAllowStreaming &&
		(bSkipCanBeLoaded || PlatformData->CanBeLoaded() || bWillProvideMipDataWithoutDisk))
	{
		bMakeStreamble  = true;
	}
#endif

	if (bMakeStreamble && IStreamingManager::Get().IsRenderAssetStreamingEnabled(EStreamableRenderAssetType::Texture))
	{
		NumRequestedMips = NumOfNonStreamingMips;
	}
	else
	{
		// Adjust CachedLODBias so that it takes into account FStreamableRenderResourceState::AssetLODBias.
		const int32 ResourceLODBias = FMath::Max<int32>(0, GetCachedLODBias() - AssetMipIdxForResourceFirstMip);

		// Ensure NumMipsInTail is within valid range to safeguard on the above expressions. 
		const int32 NumMipsInTail = FMath::Clamp<int32>(PlatformData->GetNumMipsInTail(), 1, NumMips);

		// Bias is not allowed to shrink the mip count below NumMipsInTail.
		NumRequestedMips = FMath::Max<int32>(NumMips - ResourceLODBias, NumMipsInTail);

		// If trying to load optional mips, check if the first resource mip is available.
		if (NumRequestedMips > NumOfNonOptionalMips && !DoesMipDataExist(AssetMipIdxForResourceFirstMip))
		{
			NumRequestedMips = NumOfNonOptionalMips;
		}
	}

	if (NumRequestedMips < MinRequestMipCount && MinRequestMipCount < NumMips)
	{
		NumRequestedMips = MinRequestMipCount;
	}

	FStreamableRenderResourceState PostInitState;
	PostInitState.bSupportsStreaming = bMakeStreamble;
	PostInitState.NumNonStreamingLODs = (uint8)NumOfNonStreamingMips;
	PostInitState.NumNonOptionalLODs = (uint8)NumOfNonOptionalMips;
	PostInitState.MaxNumLODs = (uint8)NumMips;
	PostInitState.AssetLODBias = (uint8)AssetMipIdxForResourceFirstMip;
	PostInitState.NumResidentLODs = (uint8)NumRequestedMips;
	PostInitState.NumRequestedLODs = (uint8)NumRequestedMips;

	return PostInitState;
}

/*------------------------------------------------------------------------------
	Texture source data.
------------------------------------------------------------------------------*/

FTextureSource::FTextureSource()
	: NumLockedMips(0u)
	, LockState(ELockState::None)
#if WITH_EDITOR
	, bHasHadBulkDataCleared(false)
#endif
#if WITH_EDITORONLY_DATA
	, BaseBlockX(0)
	, BaseBlockY(0)
	, SizeX(0)
	, SizeY(0)
	, NumSlices(0)
	, NumMips(0)
	, NumLayers(1) // Default to 1 so old data has the correct value
	, bPNGCompressed(false)
	, bLongLatCubemap(false)
	, CompressionFormat(TSCF_None)
	, bGuidIsHash(false)
	, Format(TSF_Invalid)
#endif // WITH_EDITORONLY_DATA
{
}

FTextureSourceBlock::FTextureSourceBlock()
	: BlockX(0)
	, BlockY(0)
	, SizeX(0)
	, SizeY(0)
	, NumSlices(0)
	, NumMips(0)
{
}

int32 FTextureSource::GetBytesPerPixel(ETextureSourceFormat Format)
{
	int32 BytesPerPixel = 0;
	switch (Format)
	{
	case TSF_G8:		BytesPerPixel = 1; break;
	case TSF_G16:		BytesPerPixel = 2; break;
	case TSF_BGRA8:		BytesPerPixel = 4; break;
	case TSF_BGRE8:		BytesPerPixel = 4; break;
	case TSF_RGBA16:	BytesPerPixel = 8; break;
	case TSF_RGBA16F:	BytesPerPixel = 8; break;
	default:			BytesPerPixel = 0; break;
	}
	return BytesPerPixel;
}

#if WITH_EDITOR

void FTextureSource::InitBlocked(const ETextureSourceFormat* InLayerFormats,
	const FTextureSourceBlock* InBlocks,
	int32 InNumLayers,
	int32 InNumBlocks,
	const uint8** InDataPerBlock)
{
	InitBlockedImpl(InLayerFormats, InBlocks, InNumLayers, InNumBlocks);

	int64 TotalBytes = 0;
	for (int i = 0; i < InNumBlocks; ++i)
	{
		TotalBytes += CalcBlockSize(i);
	}

	FUniqueBuffer Buffer = FUniqueBuffer::Alloc(TotalBytes);
	uint8* DataPtr = (uint8*)Buffer.GetData();

	if (InDataPerBlock)
	{
		for (int i = 0; i < InNumBlocks; ++i)
		{
			const int64 BlockSize = CalcBlockSize(InBlocks[i]);
			if (InDataPerBlock[i])
			{
				FMemory::Memcpy(DataPtr, InDataPerBlock[i], BlockSize);
			}
			DataPtr += BlockSize;
		}
	}

	BulkData.UpdatePayload(Buffer.MoveToShared());
	BulkData.SetCompressionOptions(UE::Serialization::ECompressionOptions::Default);
}

void FTextureSource::InitBlocked(const ETextureSourceFormat* InLayerFormats,
	const FTextureSourceBlock* InBlocks,
	int32 InNumLayers,
	int32 InNumBlocks,
	UE::Serialization::FEditorBulkData::FSharedBufferWithID NewData)
{
	InitBlockedImpl(InLayerFormats, InBlocks, InNumLayers, InNumBlocks);

	BulkData.UpdatePayload(MoveTemp(NewData));
	BulkData.SetCompressionOptions(UE::Serialization::ECompressionOptions::Default);
}

void FTextureSource::InitLayered(
	int32 NewSizeX,
	int32 NewSizeY,
	int32 NewNumSlices,
	int32 NewNumLayers,
	int32 NewNumMips,
	const ETextureSourceFormat* NewLayerFormat,
	const uint8* NewData)
{
	InitLayeredImpl(
		NewSizeX,
		NewSizeY,
		NewNumSlices,
		NewNumLayers,
		NewNumMips,
		NewLayerFormat
		);

	int64 TotalBytes = 0;
	for (int i = 0; i < NewNumLayers; ++i)
	{
		TotalBytes += CalcLayerSize(0, i);
	}

	// TODO: Allocating an empty buffer if there is no data to copy from seems like an odd choice but the 
	// code logic has been doing this for almost a decade so I don't want to change it until I am sure that
	// it serves no purpose. Given a choice I'd assert on NewData == nullptr instead.
	if (NewData != nullptr)
	{
		BulkData.UpdatePayload(FSharedBuffer::Clone(NewData, TotalBytes));
	}
	else
	{
		BulkData.UpdatePayload(FUniqueBuffer::Alloc(TotalBytes).MoveToShared());
	}

	BulkData.SetCompressionOptions(UE::Serialization::ECompressionOptions::Default);
}

void FTextureSource::InitLayered(
	int32 NewSizeX,
	int32 NewSizeY,
	int32 NewNumSlices,
	int32 NewNumLayers,
	int32 NewNumMips,
	const ETextureSourceFormat* NewLayerFormat,
	UE::Serialization::FEditorBulkData::FSharedBufferWithID NewData)
{
	InitLayeredImpl(
		NewSizeX,
		NewSizeY,
		NewNumSlices,
		NewNumLayers,
		NewNumMips,
		NewLayerFormat
	);

	BulkData.UpdatePayload(MoveTemp(NewData));
	BulkData.SetCompressionOptions(UE::Serialization::ECompressionOptions::Default);
}

void FTextureSource::Init(
		int32 NewSizeX,
		int32 NewSizeY,
		int32 NewNumSlices,
		int32 NewNumMips,
		ETextureSourceFormat NewFormat,
		const uint8* NewData
		)
{
	InitLayered(NewSizeX, NewSizeY, NewNumSlices, 1, NewNumMips, &NewFormat, NewData);
}

void FTextureSource::Init(
	int32 NewSizeX,
	int32 NewSizeY,
	int32 NewNumSlices,
	int32 NewNumMips,
	ETextureSourceFormat NewFormat,
	UE::Serialization::FEditorBulkData::FSharedBufferWithID NewData
)
{
	InitLayered(NewSizeX, NewSizeY, NewNumSlices, 1, NewNumMips, &NewFormat, MoveTemp(NewData));
}

void FTextureSource::Init2DWithMipChain(
	int32 NewSizeX,
	int32 NewSizeY,
	ETextureSourceFormat NewFormat
	)
{
	int32 NewMipCount = FMath::Max(FMath::CeilLogTwo(NewSizeX),FMath::CeilLogTwo(NewSizeY)) + 1;
	Init(NewSizeX, NewSizeY, 1, NewMipCount, NewFormat);
}

void FTextureSource::InitLayered2DWithMipChain(
	int32 NewSizeX,
	int32 NewSizeY,
	int32 NewNumLayers,
	const ETextureSourceFormat* NewFormat)
{
	int32 NewMipCount = FMath::Max(FMath::CeilLogTwo(NewSizeX), FMath::CeilLogTwo(NewSizeY)) + 1;
	InitLayered(NewSizeX, NewSizeY, 1, NewNumLayers, NewMipCount, NewFormat);
}

void FTextureSource::InitCubeWithMipChain(
	int32 NewSizeX,
	int32 NewSizeY,
	ETextureSourceFormat NewFormat
	)
{
	int32 NewMipCount = FMath::Max(FMath::CeilLogTwo(NewSizeX),FMath::CeilLogTwo(NewSizeY)) + 1;
	Init(NewSizeX, NewSizeY, 6, NewMipCount, NewFormat);
}

void FTextureSource::InitWithCompressedSourceData(
	int32 NewSizeX,
	int32 NewSizeY,
	int32 NewNumMips,
	ETextureSourceFormat NewFormat,
	const TArrayView64<uint8> NewData,
	ETextureSourceCompressionFormat NewSourceFormat
)
{
	RemoveSourceData();

	SizeX = NewSizeX;
	SizeY = NewSizeY;

	NumLayers = 1;
	NumSlices = 1;
	NumMips = NewNumMips;

	Format = NewFormat;
	LayerFormat.SetNum(1, true);
	LayerFormat[0] = NewFormat;

	CompressionFormat = NewSourceFormat;

	BlockDataOffsets.Add(0);

	checkf(LockState == ELockState::None, TEXT("InitWithCompressedSourceData shouldn't be called in-between LockMip/UnlockMip"));

	BulkData.UpdatePayload(FSharedBuffer::Clone(NewData.GetData(), NewData.Num()));

	// Disable the internal bulkdata compression if the source data is already compressed
	if (CompressionFormat == TSCF_None)
	{
		BulkData.SetCompressionOptions(UE::Serialization::ECompressionOptions::Default);
	}
	else
	{
		BulkData.SetCompressionOptions(UE::Serialization::ECompressionOptions::Disabled);
	}
}

FTextureSource FTextureSource::CopyTornOff() const
{
	FTextureSource Result;
	// Set the Torn off flag on Result.BulkData so that the copy constructor below will not set it
	Result.BulkData.TearOff();
	// Use the default copy constructor to copy all the fields without having to write them manually
	Result = *this;
	return Result;
}

void FTextureSource::Compress()
{
	checkf(LockState == ELockState::None, TEXT("Compress shouldn't be called in-between LockMip/UnlockMip"));

#if WITH_EDITOR
	FWriteScopeLock BulkDataExclusiveScope(BulkDataLock.Get());
#endif

	// if bUseOodleOnPNGz0 , do PNG filters but then use Oodle instead of zlib back-end LZ
	//	should be faster to load and also smaller files (than traditional PNG+zlib)
	bool bUseOodleOnPNGz0 = true;

	// may already have bPNGCompressed or "CompressionFormat" set

	if (CanPNGCompress()) // Note that this will return false if the data is already a compressed PNG
	{
		FSharedBuffer Payload = BulkData.GetPayload().Get();

		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>( FName("ImageWrapper") );
		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper( EImageFormat::PNG );
		// TODO: TSF_BGRA8 is stored as RGBA, so the R and B channels are swapped in the internal png. Should we fix this?
		ERGBFormat RawFormat = (Format == TSF_G8 || Format == TSF_G16) ? ERGBFormat::Gray : ERGBFormat::RGBA;
		int RawBitsPerChannel = (Format == TSF_G16 || Format == TSF_RGBA16) ? 16 : 8;
		if ( ImageWrapper.IsValid() && ImageWrapper->SetRaw(Payload.GetData(), Payload.GetSize(), SizeX, SizeY, RawFormat, RawBitsPerChannel ) )
		{
			int32 PngQuality = (int32)EImageCompressionQuality::Default; // 0 means default 
			if ( bUseOodleOnPNGz0 )
			{
				PngQuality = (int32)EImageCompressionQuality::Uncompressed; // turn off zlib
			}
			TArray64<uint8> CompressedData = ImageWrapper->GetCompressed(PngQuality);
			if ( CompressedData.Num() > 0 )
			{
				BulkData.UpdatePayload(MakeSharedBufferFromArray(MoveTemp(CompressedData)));

				bPNGCompressed = true;
				CompressionFormat = TSCF_PNG;
			}
		}
	}

	// Fix up for packages that were saved before CompressionFormat was introduced. Can removed this when we deprecate bPNGCompressed!
	if (bPNGCompressed)
	{
		CompressionFormat = TSCF_PNG;
	}

	if ( ( CompressionFormat == TSCF_PNG && bUseOodleOnPNGz0 ) ||
		CompressionFormat == TSCF_None )
	{
		BulkData.SetCompressionOptions(ECompressedBufferCompressor::Kraken,ECompressedBufferCompressionLevel::Fast);
	}
	else
	{
		BulkData.SetCompressionOptions(UE::Serialization::ECompressionOptions::Disabled);
	}
}

FSharedBuffer FTextureSource::Decompress(IImageWrapperModule* ImageWrapperModule) const
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureSource::Decompress);

	if (CompressionFormat == TSCF_JPEG)
	{
		return TryDecompressJpegData(ImageWrapperModule);
	}
	else if (bPNGCompressed) // Change to CompressionFormat == TSCF_PNG once bPNGCompressed is deprecated
	{
		return TryDecompressPngData(ImageWrapperModule);
	}
	else
	{
		return BulkData.GetPayload().Get();
	}
}

const uint8* FTextureSource::LockMipReadOnly(int32 BlockIndex, int32 LayerIndex, int32 MipIndex)
{
	return LockMipInternal(BlockIndex, LayerIndex, MipIndex, ELockState::ReadOnly);
}

uint8* FTextureSource::LockMip(int32 BlockIndex, int32 LayerIndex, int32 MipIndex)
{
	return LockMipInternal(BlockIndex, LayerIndex, MipIndex, ELockState::ReadWrite);
}

uint8* FTextureSource::LockMipInternal(int32 BlockIndex, int32 LayerIndex, int32 MipIndex, ELockState RequestedLockState)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureSource::LockMip);

	checkf(RequestedLockState != ELockState::None, TEXT("Cannot call FTextureSource::LockMipInternal with a RequestedLockState of type ELockState::None"));

	uint8* MipData = nullptr;

	if (BlockIndex < GetNumBlocks() && LayerIndex < NumLayers && MipIndex < NumMips)
	{
		if (LockedMipData.IsNull())
		{
			checkf(NumLockedMips == 0, TEXT("Texture mips are locked but the LockedMipData is missing"));
			LockedMipData = Decompress(nullptr);
		}

		if (RequestedLockState == ELockState::ReadOnly)
		{
			MipData = const_cast<uint8*>(static_cast<const uint8*>(LockedMipData.GetDataReadOnly().GetData()));
		}
		else
		{
			MipData = LockedMipData.GetDataReadWrite();
		}
		
		MipData += CalcMipOffset(BlockIndex, LayerIndex, MipIndex);

		if (NumLockedMips == 0)
		{
			LockState = RequestedLockState;
		}
		else
		{
			checkf(LockState == RequestedLockState, TEXT("Cannot change the lock type until UnlockMip is called"));
		}

		++NumLockedMips;
	}

	return MipData;
}

void FTextureSource::UnlockMip(int32 BlockIndex, int32 LayerIndex, int32 MipIndex)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureSource::UnlockMip);

	check(BlockIndex < GetNumBlocks());
	check(LayerIndex < NumLayers);
	check(MipIndex < MAX_TEXTURE_MIP_COUNT);
	check(NumLockedMips > 0u);
	check(LockState != ELockState::None);

	--NumLockedMips;
	if (NumLockedMips == 0u)
	{
		// If the lock was for Read/Write then we need to assume that the decompressed copy
		// we returned (LockedMipData) was updated and should update the payload accordingly.
		// This will wipe the compression format that we used to have.
		if (LockState == ELockState::ReadWrite)
		{
			UE_CLOG(CompressionFormat == TSCF_JPEG, LogTexture, Warning, TEXT("Call to FTextureSource::UnlockMip will cause texture source to lose it's jpeg storage format"));

			BulkData.UpdatePayload(LockedMipData.Release());
			BulkData.SetCompressionOptions(UE::Serialization::ECompressionOptions::Default);

			bPNGCompressed = false;
			CompressionFormat = TSCF_None;

			// Need to unlock before calling UseHashAsGuid
			LockState = ELockState::None;
			UseHashAsGuid();
		}

		LockState = ELockState::None;
		LockedMipData.Reset();
	}
}

bool FTextureSource::GetMipData(TArray64<uint8>& OutMipData, int32 BlockIndex, int32 LayerIndex, int32 MipIndex, IImageWrapperModule* ImageWrapperModule)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureSource::GetMipData (TArray64));

	checkf(LockState == ELockState::None, TEXT("GetMipData (TArray64) shouldn't be called in-between LockMip/UnlockMip"));

	bool bSuccess = false;

	if (BlockIndex < GetNumBlocks() && LayerIndex < NumLayers && MipIndex < NumMips && HasPayloadData())
	{
#if WITH_EDITOR
		FWriteScopeLock BulkDataExclusiveScope(BulkDataLock.Get());
#endif

		checkf(NumLockedMips == 0, TEXT("Attempting to access a locked FTextureSource"));
		// LockedMipData should only be allocated if NumLockedMips > 0 so the following assert should have been caught
		// by the one above. If it fires then it indicates that there is a lock/unlock mismatch as well as invalid access!
		checkf(LockedMipData.IsNull(), TEXT("Attempting to access mip data while locked mip data is still allocated"));

		FSharedBuffer DecompressedData = Decompress(ImageWrapperModule);

		if (!DecompressedData.IsNull())
		{
			const int64 MipOffset = CalcMipOffset(BlockIndex, LayerIndex, MipIndex);
			const int64 MipSize = CalcMipSize(BlockIndex, LayerIndex, MipIndex);

			if ((int64)DecompressedData.GetSize() >= MipOffset + MipSize)
			{
				OutMipData.Empty(MipSize);
				OutMipData.AddUninitialized(MipSize);
				FMemory::Memcpy(
					OutMipData.GetData(),
					(const uint8*)DecompressedData.GetData() + MipOffset,
					MipSize
				);

				bSuccess = true;
			}
		}	
	}
	
	return bSuccess;
}

FTextureSource::FMipData FTextureSource::GetMipData(IImageWrapperModule* ImageWrapperModule)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(FTextureSource::GetMipData (FMipData));

	checkf(LockState == ELockState::None, TEXT("GetMipData (FMipData) shouldn't be called in-between LockMip/UnlockMip"));

	check(LockedMipData.IsNull());
	check(NumLockedMips == 0);

#if WITH_EDITOR
	FReadScopeLock _(BulkDataLock.Get());
#endif //WITH_EDITOR

	FSharedBuffer DecompressedData = Decompress(ImageWrapperModule);
	return FMipData(*this, DecompressedData);
}

int64 FTextureSource::CalcMipSize(int32 BlockIndex, int32 LayerIndex, int32 MipIndex) const
{
	FTextureSourceBlock Block;
	GetBlock(BlockIndex, Block);
	check(MipIndex < Block.NumMips);

	const int64 MipSizeX = FMath::Max(Block.SizeX >> MipIndex, 1);
	const int64 MipSizeY = FMath::Max(Block.SizeY >> MipIndex, 1);
	const int64 BytesPerPixel = GetBytesPerPixel(LayerIndex);
	return MipSizeX * MipSizeY * Block.NumSlices * BytesPerPixel;
}

int32 FTextureSource::GetBytesPerPixel(int32 LayerIndex) const
{
	return GetBytesPerPixel(GetFormat(LayerIndex));
}

bool FTextureSource::IsPowerOfTwo(int32 BlockIndex) const
{
	FTextureSourceBlock Block;
	GetBlock(BlockIndex, Block);
	return FMath::IsPowerOfTwo(Block.SizeX) && FMath::IsPowerOfTwo(Block.SizeY);
}

bool FTextureSource::IsValid() const
{
	return SizeX > 0 && SizeY > 0 && NumSlices > 0 && NumLayers > 0 && NumMips > 0 &&
		Format != TSF_Invalid && HasPayloadData();
}

void FTextureSource::GetBlock(int32 Index, FTextureSourceBlock& OutBlock) const
{
	check(Index < GetNumBlocks());
	if (Index == 0)
	{
		OutBlock.BlockX = BaseBlockX;
		OutBlock.BlockY = BaseBlockY;
		OutBlock.SizeX = SizeX;
		OutBlock.SizeY = SizeY;
		OutBlock.NumSlices = NumSlices;
		OutBlock.NumMips = NumMips;
	}
	else
	{
		OutBlock = Blocks[Index - 1];
	}
}

FIntPoint FTextureSource::GetLogicalSize() const
{
	const int32 NumBlocks = GetNumBlocks();
	int32 SizeInBlocksX = 0;
	int32 SizeInBlocksY = 0;
	int32 BlockSizeX = 0;
	int32 BlockSizeY = 0;
	for (int32 BlockIndex = 0; BlockIndex < NumBlocks; ++BlockIndex)
	{
		FTextureSourceBlock SourceBlock;
		GetBlock(BlockIndex, SourceBlock);
		SizeInBlocksX = FMath::Max(SizeInBlocksX, SourceBlock.BlockX + 1);
		SizeInBlocksY = FMath::Max(SizeInBlocksY, SourceBlock.BlockY + 1);
		BlockSizeX = FMath::Max(BlockSizeX, SourceBlock.SizeX);
		BlockSizeY = FMath::Max(BlockSizeY, SourceBlock.SizeY);
	}
	return FIntPoint(SizeInBlocksX * BlockSizeX, SizeInBlocksY * BlockSizeY);
}

FIntPoint FTextureSource::GetSizeInBlocks() const
{
	const int32 NumBlocks = GetNumBlocks();
	int32 SizeInBlocksX = 0;
	int32 SizeInBlocksY = 0;
	for (int32 BlockIndex = 0; BlockIndex < NumBlocks; ++BlockIndex)
	{
		FTextureSourceBlock SourceBlock;
		GetBlock(BlockIndex, SourceBlock);
		SizeInBlocksX = FMath::Max(SizeInBlocksX, SourceBlock.BlockX + 1);
		SizeInBlocksY = FMath::Max(SizeInBlocksY, SourceBlock.BlockY + 1);
	}
	return FIntPoint(SizeInBlocksX, SizeInBlocksY);
}

FString FTextureSource::GetIdString() const
{
	FString GuidString = GetId().ToString();
	if (bGuidIsHash)
	{
		GuidString += TEXT("X");
	}
	return GuidString;
}

ETextureSourceCompressionFormat FTextureSource::GetSourceCompression() const
{
	// Until we deprecate bPNGCompressed it might not be 100% in sync with CompressionFormat
	// so if it is set we should use that rather than the enum.
	if (bPNGCompressed)
	{
		return ETextureSourceCompressionFormat::TSCF_PNG;
	}

	return CompressionFormat;
}

FString FTextureSource::GetSourceCompressionAsString() const
{
	return StaticEnum<ETextureSourceCompressionFormat>()->GetDisplayNameTextByValue(GetSourceCompression()).ToString();
}

FSharedBuffer FTextureSource::TryDecompressPngData(IImageWrapperModule* ImageWrapperModule) const
{
	bool bCanPngCompressFormat = (Format == TSF_G8 || Format == TSF_G16 || Format == TSF_RGBA8 || Format == TSF_BGRA8 || Format == TSF_RGBA16);
	check(Blocks.Num() == 0 && NumLayers == 1 && NumSlices == 1 && bCanPngCompressFormat);

	FSharedBuffer Payload = BulkData.GetPayload().Get();

	if (ImageWrapperModule == nullptr) // Optional if called from the gamethread, see FModuleManager::WarnIfItWasntSafeToLoadHere()
	{
		ImageWrapperModule = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
	}

	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule->CreateImageWrapper(EImageFormat::PNG);
	if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(Payload.GetData(), Payload.GetSize()))
	{
		Payload.Reset(); // Image wrapper takes a copy so we can discard the payload now

		check(ImageWrapper->GetWidth() == SizeX);
		check(ImageWrapper->GetHeight() == SizeY);

		TArray64<uint8> RawData;
		// TODO: TSF_BGRA8 is stored as RGBA, so the R and B channels are swapped in the internal png. Should we fix this?
		ERGBFormat RawFormat = (Format == TSF_G8 || Format == TSF_G16) ? ERGBFormat::Gray : ERGBFormat::RGBA;
		if (ImageWrapper->GetRaw(RawFormat, (Format == TSF_G16 || Format == TSF_RGBA16) ? 16 : 8, RawData) && RawData.Num() > 0)
		{		
			return MakeSharedBufferFromArray(MoveTemp(RawData));
		}
		else
		{
			UE_LOG(LogTexture, Warning, TEXT("PNG decompression of source art failed"));
			return FSharedBuffer();
		}
	}
	else
	{
		UE_LOG(LogTexture, Log, TEXT("Only pngs are supported"));
		return FSharedBuffer();
	}
}

FSharedBuffer FTextureSource::TryDecompressJpegData(IImageWrapperModule* ImageWrapperModule) const
{
	if (NumLayers == 1 && NumSlices == 1 && Blocks.Num() == 0)
	{
		if (!ImageWrapperModule)
		{
			ImageWrapperModule = &FModuleManager::LoadModuleChecked<IImageWrapperModule>(FName("ImageWrapper"));
		}

		FSharedBuffer Payload = BulkData.GetPayload().Get();

		TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule->CreateImageWrapper(EImageFormat::JPEG);
		if (ImageWrapper.IsValid() && ImageWrapper->SetCompressed(Payload.GetData(), Payload.GetSize()))
		{
			Payload.Reset(); // Image wrapper takes a copy so we can discard the payload now

			TArray64<uint8> RawData;
			// The two formats we support for JPEG imports, see UTextureFactory::ImportImage
			const ERGBFormat JpegFormat = Format == TSF_G8 ? ERGBFormat::Gray : ERGBFormat::BGRA;
			if (ImageWrapper->GetRaw(JpegFormat, 8, RawData))
			{
				return MakeSharedBufferFromArray(MoveTemp(RawData));
			}
			else
			{
				UE_LOG(LogTexture, Warning, TEXT("JPEG decompression of source art failed to return uncompressed data"));
				return FSharedBuffer();
			}
		}
		else
		{
			UE_LOG(LogTexture, Warning, TEXT("JPEG decompression of source art failed initialization"));
			return FSharedBuffer();
		}
	}
	else
	{
		UE_LOG(LogTexture, Warning, TEXT("JPEG compressed source art is in an invalid format NumLayers:(%d) NumSlices:(%d) NumBlocks:(%d)"),
			NumLayers, NumSlices, Blocks.Num());
		return FSharedBuffer();
	}	
}

void FTextureSource::ExportCustomProperties(FOutputDevice& Out, uint32 Indent)
{
	check(LockState == ELockState::None);

	FSharedBuffer Payload = BulkData.GetPayload().Get();
	uint64 PayloadSize = Payload.GetSize();

	Out.Logf(TEXT("%sCustomProperties TextureSourceData "), FCString::Spc(Indent));

	Out.Logf(TEXT("PayloadSize=%llu "), PayloadSize);
	TArrayView<uint8> Buffer((uint8*)Payload.GetData(), PayloadSize);
	for (uint8 Element : Buffer)
	{
		Out.Logf(TEXT("%x "), Element);
	}
}

void FTextureSource::ImportCustomProperties(const TCHAR* SourceText, FFeedbackContext* Warn)
{
	check(LockState == ELockState::None);

	if (FParse::Command(&SourceText, TEXT("TextureSourceData")))
	{
		uint64 PayloadSize = 0;
		if (FParse::Value(SourceText, TEXT("PayloadSize="), PayloadSize))
		{
			while (*SourceText && (!FChar::IsWhitespace(*SourceText)))
			{
				++SourceText;
			}
			FParse::Next(&SourceText);
		}

		bool bSuccess = true;
		if (PayloadSize > (uint64)0)
		{
			TCHAR* StopStr;
			FUniqueBuffer Buffer = FUniqueBuffer::Alloc(PayloadSize);
			uint8* DestData = (uint8*)Buffer.GetData();
			if (DestData)
			{
				uint64 Index = 0;
				while (FChar::IsHexDigit(*SourceText))
				{
					if (Index < PayloadSize)
					{
						DestData[Index++] = (uint8)FCString::Strtoi(SourceText, &StopStr, 16);
						while (FChar::IsHexDigit(*SourceText))
						{
							SourceText++;
						}
					}
					FParse::Next(&SourceText);
				}

				if (Index != PayloadSize)
				{
					Warn->Log(*NSLOCTEXT("UnrealEd", "Importing_TextureSource_SyntaxError", "Syntax Error").ToString());
					bSuccess = false;
				}
			}
			else
			{
				Warn->Log(*NSLOCTEXT("UnrealEd", "Importing_TextureSource_BulkDataAllocFailure", "Couldn't allocate bulk data").ToString());
				bSuccess = false;
			}
			
			if (bSuccess)
			{
				BulkData.UpdatePayload(Buffer.MoveToShared());
			}
		}

		if (bSuccess)
		{
			if (!bGuidIsHash)
			{
				ForceGenerateGuid();
			}
		}
		else
		{
			BulkData.Reset();
		}
	}
	else
	{
		Warn->Log(*NSLOCTEXT("UnrealEd", "Importing_TextureSource_MissingTextureSourceDataCommand", "Missing TextureSourceData tag from import text.").ToString());
	}
}

bool FTextureSource::CanPNGCompress() const
{
	bool bCanPngCompressFormat = (Format == TSF_G8 || Format == TSF_G16 || Format == TSF_RGBA8 || Format == TSF_BGRA8 || Format == TSF_RGBA16);

	if (!bPNGCompressed &&
		NumLayers == 1 &&
		NumMips == 1 &&
		NumSlices == 1 &&
		Blocks.Num() == 0 &&
		SizeX > 4 &&
		SizeY > 4 &&
		HasPayloadData() &&
		bCanPngCompressFormat &&
		CompressionFormat == TSCF_None)
	{
		return true;
	}
	return false;
}

void FTextureSource::ForceGenerateGuid()
{
	Id = FGuid::NewGuid();
	bGuidIsHash = false;
}

void FTextureSource::ReleaseSourceMemory()
{
	bHasHadBulkDataCleared = true;
	BulkData.UnloadData();
}

void FTextureSource::RemoveSourceData()
{
	SizeX = 0;
	SizeY = 0;
	NumSlices = 0;
	NumLayers = 0;
	NumMips = 0;
	Format = TSF_Invalid;
	LayerFormat.Empty();
	Blocks.Empty();
	BlockDataOffsets.Empty();
	bPNGCompressed = false;
	CompressionFormat = TSCF_None;
	LockedMipData.Reset();
	NumLockedMips = 0u;
	LockState = ELockState::None;
	
	BulkData.UnloadData();

	ForceGenerateGuid();
}

int64 FTextureSource::CalcBlockSize(int32 BlockIndex) const
{
	FTextureSourceBlock Block;
	GetBlock(BlockIndex, Block);
	return CalcBlockSize(Block);
}

int64 FTextureSource::CalcLayerSize(int32 BlockIndex, int32 LayerIndex) const
{
	FTextureSourceBlock Block;
	GetBlock(BlockIndex, Block);
	return CalcLayerSize(Block, LayerIndex);
}

int64 FTextureSource::CalcBlockSize(const FTextureSourceBlock& Block) const
{
	int64 TotalSize = 0;
	for (int32 LayerIndex = 0; LayerIndex < GetNumLayers(); ++LayerIndex)
	{
		TotalSize += CalcLayerSize(Block, LayerIndex);
	}
	return TotalSize;
}

int64 FTextureSource::CalcLayerSize(const FTextureSourceBlock& Block, int32 LayerIndex) const
{
	int64 BytesPerPixel = GetBytesPerPixel(LayerIndex);
	int64 MipSizeX = Block.SizeX;
	int64 MipSizeY = Block.SizeY;

	int64 TotalSize = 0;
	for (int32 MipIndex = 0; MipIndex < Block.NumMips; ++MipIndex)
	{
		TotalSize += MipSizeX * MipSizeY * BytesPerPixel * Block.NumSlices;
		MipSizeX = FMath::Max<int64>(MipSizeX >> 1, 1);
		MipSizeY = FMath::Max<int64>(MipSizeY >> 1, 1);
	}
	return TotalSize;
}

int64 FTextureSource::CalcMipOffset(int32 BlockIndex, int32 LayerIndex, int32 MipIndex) const
{
	FTextureSourceBlock Block;
	GetBlock(BlockIndex, Block);
	check(MipIndex < Block.NumMips);

	int64 MipOffset = BlockDataOffsets[BlockIndex];

	// Skip over the initial layers within the tile
	for (int i = 0; i < LayerIndex; ++i)
	{
		MipOffset += CalcLayerSize(Block, i);
	}

	int64 BytesPerPixel = GetBytesPerPixel(LayerIndex);
	int64 MipSizeX = Block.SizeX;
	int64 MipSizeY = Block.SizeY;

	while (MipIndex-- > 0)
	{
		MipOffset += MipSizeX * MipSizeY * BytesPerPixel * Block.NumSlices;
		MipSizeX = FMath::Max<int64>(MipSizeX >> 1, 1);
		MipSizeY = FMath::Max<int64>(MipSizeY >> 1, 1);
	}

	return MipOffset;
}

void FTextureSource::UseHashAsGuid()
{
	if (HasPayloadData())
	{
		checkf(LockState == ELockState::None, TEXT("UseHashAsGuid shouldn't be called in-between LockMip/UnlockMip"));

		bGuidIsHash = true;
		Id = UE::Serialization::IoHashToGuid(BulkData.GetPayloadId());
	}
	else
	{
		Id.Invalidate();
	}
}

FGuid FTextureSource::GetId() const
{
	if (!bGuidIsHash)
	{
		return Id;
	}

	UE::DerivedData::FBuildVersionBuilder IdBuilder;
	IdBuilder << BaseBlockX;
	IdBuilder << BaseBlockX;
	IdBuilder << BaseBlockY;
	IdBuilder << SizeX;
	IdBuilder << SizeY;
	IdBuilder << NumSlices;
	IdBuilder << NumMips;
	IdBuilder << NumLayers;
	IdBuilder << bPNGCompressed;
	IdBuilder << bLongLatCubemap;
	IdBuilder << CompressionFormat;
	IdBuilder << bGuidIsHash;
	IdBuilder << static_cast<uint8>(Format.GetValue());
	IdBuilder.Serialize(const_cast<void*>(static_cast<const void*>(LayerFormat.GetData())), LayerFormat.Num());
	IdBuilder.Serialize(const_cast<void*>(static_cast<const void*>(Blocks.GetData())), Blocks.Num());
	IdBuilder.Serialize(const_cast<void*>(static_cast<const void*>(BlockDataOffsets.GetData())), BlockDataOffsets.Num());
	IdBuilder << const_cast<FGuid&>(Id);
	return IdBuilder.Build();
}

void FTextureSource::OperateOnLoadedBulkData(TFunctionRef<void(const FSharedBuffer& BulkDataBuffer)> Operation)
{
	checkf(LockState == ELockState::None, TEXT("OperateOnLoadedBulkData shouldn't be called in-between LockMip/UnlockMip"));

#if WITH_EDITOR
	FReadScopeLock BulkDataExclusiveScope(BulkDataLock.Get());
#endif

	FSharedBuffer Payload = BulkData.GetPayload().Get();
	Operation(Payload);
}

void FTextureSource::SetId(const FGuid& InId, bool bInGuidIsHash)
{
	Id = InId;
	bGuidIsHash = bInGuidIsHash;
}

uint32 UTexture::GetMaximumDimension() const
{
	return GetMax2DTextureDimension();
}

void UTexture::GetDefaultFormatSettings(FTextureFormatSettings& OutSettings) const
{
	OutSettings.CompressionSettings = CompressionSettings;
	OutSettings.CompressionNone = CompressionNone;
	OutSettings.CompressionNoAlpha = CompressionNoAlpha;
	OutSettings.CompressionYCoCg = CompressionYCoCg;
	OutSettings.SRGB = SRGB;
}

void UTexture::GetLayerFormatSettings(int32 LayerIndex, FTextureFormatSettings& OutSettings) const
{
	check(LayerIndex >= 0);
	if (LayerIndex < LayerFormatSettings.Num())
	{
		OutSettings = LayerFormatSettings[LayerIndex];
	}
	else
	{
		GetDefaultFormatSettings(OutSettings);
	}
}

void UTexture::SetLayerFormatSettings(int32 LayerIndex, const FTextureFormatSettings& InSettings)
{
	check(LayerIndex >= 0);
	if (LayerIndex == 0 && LayerFormatSettings.Num() == 0)
	{
		// Apply layer0 settings directly to texture properties
		CompressionSettings = InSettings.CompressionSettings;
		CompressionNone = InSettings.CompressionNone;
		CompressionNoAlpha = InSettings.CompressionNoAlpha;
		CompressionYCoCg = InSettings.CompressionYCoCg;
		SRGB = InSettings.SRGB;
	}
	else
	{
		if (LayerIndex >= LayerFormatSettings.Num())
		{
			FTextureFormatSettings DefaultSettings;
			GetDefaultFormatSettings(DefaultSettings);
			LayerFormatSettings.Reserve(LayerIndex + 1);
			while (LayerIndex >= LayerFormatSettings.Num())
			{
				LayerFormatSettings.Add(DefaultSettings);
			}
		}
		LayerFormatSettings[LayerIndex] = InSettings;
	}
}

int64 UTexture::GetBuildRequiredMemory() const
{
	// If you improve this estimate, please update EstimateTextureBuildMemoryUsage() as well

	// Compute the memory it should take to uncompress the bulkdata in memory
	int64 MemoryEstimate = 0;

	// Compute the amount of memory necessary to uncompress the bulkdata in memory
	for (int32 BlockIndex = 0; BlockIndex < Source.GetNumBlocks(); ++BlockIndex)
	{
		FTextureSourceBlock SourceBlock;
		Source.GetBlock(BlockIndex, SourceBlock);

		for (int32 LayerIndex = 0; LayerIndex < Source.GetNumLayers(); ++LayerIndex)
		{
			for (int32 MipIndex = 0; MipIndex < SourceBlock.NumMips; ++MipIndex)
			{
				MemoryEstimate += Source.CalcMipSize(BlockIndex, LayerIndex, MipIndex);
			}
		}
	}

	// Account for the multiple copies that are currently carried over during the compression phase
	return MemoryEstimate <= 0 ? -1 /* Unknown */ : MemoryEstimate * 5;
}

#endif // #if WITH_EDITOR

static FName GetLatestOodleTextureSdkVersion()
{

#if WITH_EDITOR
	// don't use AlternateTextureCompression pref
	//	just explicitly ask for new Oodle
	// in theory we could look for a "TextureCompressionFormatWithVersion" setting
	//	but to do that we need a target platform, since it could defer by target and not be set for current at all
	// and here we need something global, not per-target
	const TCHAR * TextureCompressionFormat = TEXT("TextureFormatOodle");

	ITextureFormatModule * TextureFormatModule = FModuleManager::LoadModulePtr<ITextureFormatModule>(TextureCompressionFormat);

	// TextureFormatModule can be null if TextureFormatOodle is disabled in this project
	//  then we will return None, which is correct

	if ( TextureFormatModule )
	{
		ITextureFormat* TextureFormat = TextureFormatModule->GetTextureFormat();
					
		FName LatestSdkVersion = TextureFormat->GetLatestSdkVersion();
			
		return LatestSdkVersion;
	}
#endif

	return NAME_None;
}

static FName CachedGetLatestOodleSdkVersion()
{
	static struct DoOnceGetLatestSdkVersion
	{
		FName Value;

		DoOnceGetLatestSdkVersion() : Value( GetLatestOodleTextureSdkVersion() )
		{
		}
	} Once;

	return Once.Value;
}

static FName ConditionalGetPrefixedFormat(FName TextureFormatName, const ITargetPlatform* TargetPlatform, bool bOodleTextureSdkVersionIsNone)
{
#if WITH_EDITOR

	// Prepend a texture format to allow a module to override the compression (Ex: this allows you to replace TextureFormatDXT with a different compressor)

	// TextureCompressionFormat is required, TextureCompressionFormatWithVersion is optional

	FString TextureCompressionFormat;
	bool bHasFormat = TargetPlatform->GetConfigSystem()->GetString(TEXT("AlternateTextureCompression"), TEXT("TextureCompressionFormat"), TextureCompressionFormat, GEngineIni);
	bHasFormat = bHasFormat && ! TextureCompressionFormat.IsEmpty();
	
	if ( bHasFormat )
	{
		//	new (optional) pref : TextureCompressionFormatWithVersion
		//	 if TextureCompressionFormatWithVersion is not set, TextureCompressionFormat is used for both cases (with version & without)
		if ( ! bOodleTextureSdkVersionIsNone )
		{
			FString TextureCompressionFormatWithVersion;
			bool bHasFormatWithVersion = TargetPlatform->GetConfigSystem()->GetString(TEXT("AlternateTextureCompression"), TEXT("TextureCompressionFormatWithVersion"), TextureCompressionFormatWithVersion, GEngineIni);
			bHasFormatWithVersion = bHasFormatWithVersion && ! TextureCompressionFormatWithVersion.IsEmpty();
			if ( bHasFormatWithVersion )
			{
				TextureCompressionFormat = TextureCompressionFormatWithVersion;
			}
			else
			{
				// if TextureCompressionFormatWithVersion is not set,
				// TextureCompressionFormatWithVersion is automatically set to "TextureFormatOodle"
				// new textures with version field will use TFO (if "TextureCompressionFormat" field exists)

				TextureCompressionFormat = TEXT("TextureFormatOodle");

				static bool LogOnce = true;
				// not a thread-safe atomic ONCE but no big deal here
				if ( LogOnce )
				{
					LogOnce = false;
					UE_LOG(LogTexture, Verbose, TEXT("AlternateTextureCompression/TextureCompressionFormatWithVersion not specified, using %s."), *TextureCompressionFormat);
				}
			}
		}

		ITextureFormatModule * TextureFormatModule = FModuleManager::LoadModulePtr<ITextureFormatModule>(*TextureCompressionFormat);

		if ( TextureFormatModule )
		{
			ITextureFormat* TextureFormat = TextureFormatModule->GetTextureFormat();
					
			FString FormatPrefix = TextureFormat->GetAlternateTextureFormatPrefix();
			check( ! FormatPrefix.IsEmpty() );
			
			FName NewFormatName(FormatPrefix + TextureFormatName.ToString());

			TArray<FName> SupportedFormats;
			TextureFormat->GetSupportedFormats(SupportedFormats);

			if (SupportedFormats.Contains(NewFormatName))
			{
				return NewFormatName;
			}
		}
	}
#endif

	return TextureFormatName;
}

FName GetDefaultTextureFormatName( const ITargetPlatform* TargetPlatform, const UTexture* Texture, int32 LayerIndex, bool bSupportDX11TextureFormats, bool bSupportCompressedVolumeTexture, int32 BlockSize )
{
	FName TextureFormatName = NAME_None;
	bool bOodleTextureSdkVersionIsNone = true;

	/**
	 * IF you add a format to this function don't forget to update GetAllDefaultTextureFormatNames 
	 */

#if WITH_EDITOR
	// Supported texture format names.
	static FName NameDXT1(TEXT("DXT1"));
	static FName NameDXT3(TEXT("DXT3"));
	static FName NameDXT5(TEXT("DXT5"));
	static FName NameDXT5n(TEXT("DXT5n"));
	static FName NameAutoDXT(TEXT("AutoDXT"));
	static FName NameBC4(TEXT("BC4"));
	static FName NameBC5(TEXT("BC5"));
	static FName NameBGRA8(TEXT("BGRA8"));
	static FName NameXGXR8(TEXT("XGXR8"));
	static FName NameG8(TEXT("G8"));
	static FName NameG16(TEXT("G16"));
	static FName NameVU8(TEXT("VU8"));
	static FName NameRGBA16F(TEXT("RGBA16F"));
	static FName NameR16F(TEXT("R16F"));
	static FName NameBC6H(TEXT("BC6H"));
	static FName NameBC7(TEXT("BC7"));
	static FName NameR5G6B5(TEXT("R5G6B5"));
	static FName NameA1RGB555(TEXT("A1RGB555"));
	

	check(TargetPlatform);

	static const auto CVarVirtualTexturesEnabled = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.VirtualTextures")); check(CVarVirtualTexturesEnabled);
	const bool bVirtualTextureStreaming = CVarVirtualTexturesEnabled->GetValueOnAnyThread() && TargetPlatform->SupportsFeature(ETargetPlatformFeatures::VirtualTextureStreaming) && Texture->VirtualTextureStreaming;

	FTextureFormatSettings FormatSettings;
	Texture->GetLayerFormatSettings(LayerIndex, FormatSettings);

	bool bNoCompression = FormatSettings.CompressionNone				// Code wants the texture uncompressed.
		|| (TargetPlatform->HasEditorOnlyData() && Texture->DeferCompression)	// The user wishes to defer compression, this is ok for the Editor only.
		|| (FormatSettings.CompressionSettings == TC_EditorIcon)
		|| (Texture->LODGroup == TEXTUREGROUP_ColorLookupTable)	// Textures in certain LOD groups should remain uncompressed.
		|| (Texture->LODGroup == TEXTUREGROUP_Bokeh)
		|| (Texture->LODGroup == TEXTUREGROUP_IESLightProfile)
		|| (Texture->GetMaterialType() == MCT_VolumeTexture && !bSupportCompressedVolumeTexture)
		|| FormatSettings.CompressionSettings == TC_EncodedReflectionCapture;

	if (!bNoCompression && Texture->PowerOfTwoMode == ETexturePowerOfTwoSetting::None)
	{
		uint32 SizeX = Texture->Source.GetSizeX();
		uint32 SizeY = Texture->Source.GetSizeY();
#if WITH_EDITORONLY_DATA
		const UTextureLODSettings& LODSettings = TargetPlatform->GetTextureLODSettings();
 		const uint32 LODBiasNoCinematics = FMath::Max<uint32>(LODSettings.CalculateLODBias(SizeX, SizeY, Texture->MaxTextureSize, Texture->LODGroup, Texture->LODBias, 0, Texture->MipGenSettings, bVirtualTextureStreaming), 0);
		SizeX = FMath::Max<uint32>(SizeX >> LODBiasNoCinematics, 1);
		SizeY = FMath::Max<uint32>(SizeY >> LODBiasNoCinematics, 1);
#endif
		// Don't compress textures smaller than the DXT block size.
		bNoCompression |= (SizeX < 4) || (SizeY < 4) || (SizeX % 4 != 0) || (SizeY % 4 != 0);
	}

	bool bUseDXT5NormalMap = false;

	FString UseDXT5NormalMapsString;

	if (TargetPlatform->GetConfigSystem()->GetString(TEXT("SystemSettings"), TEXT("Compat.UseDXT5NormalMaps"), UseDXT5NormalMapsString, GEngineIni))
	{
		bUseDXT5NormalMap = FCString::ToBool(*UseDXT5NormalMapsString);
	}

	const ETextureSourceFormat SourceFormat = Texture->Source.GetFormat(LayerIndex);

	// Determine the pixel format of the (un/)compressed texture
	if (bNoCompression)
	{
		if (Texture->HasHDRSource(LayerIndex))
		{
			TextureFormatName = NameRGBA16F;
		}
		else if (SourceFormat == TSF_G16)
		{
			TextureFormatName = NameG16;
		}
		else if (SourceFormat == TSF_G8 || FormatSettings.CompressionSettings == TC_Grayscale)
		{
			TextureFormatName = NameG8;
		}
		else if (FormatSettings.CompressionSettings == TC_Normalmap && bUseDXT5NormalMap)
		{
			TextureFormatName = NameXGXR8;
		}
		else
		{
			TextureFormatName = NameBGRA8;
		}
	}
	else if (FormatSettings.CompressionSettings == TC_LQ)
	{
		bool bLQCompressionSupported = TargetPlatform->SupportsLQCompressionTextureFormat();
		if(bLQCompressionSupported)
		{
			TextureFormatName = FormatSettings.CompressionNoAlpha ? NameR5G6B5 : NameA1RGB555;
		}
		else
		{
			TextureFormatName = FormatSettings.CompressionNoAlpha ? NameDXT1 : NameDXT5;
		}
	}
	else if (FormatSettings.CompressionSettings == TC_HDR)
	{
		TextureFormatName = NameRGBA16F;
	}
	else if (FormatSettings.CompressionSettings == TC_Normalmap)
	{
		TextureFormatName = bUseDXT5NormalMap ? NameDXT5n : NameBC5;
	}
	else if (FormatSettings.CompressionSettings == TC_Displacementmap)
	{
		if (SourceFormat == TSF_G16)
		{
			TextureFormatName = NameG16;
		}
		else
		{
			TextureFormatName = NameG8;
		}
	}
	else if (FormatSettings.CompressionSettings == TC_VectorDisplacementmap)
	{
		TextureFormatName = NameBGRA8;
	}
	else if (FormatSettings.CompressionSettings == TC_Grayscale)
	{
		if (SourceFormat == TSF_G16)
		{
			TextureFormatName = NameG16;
		}
		else
		{
			TextureFormatName = NameG8;
		}
	}
	else if ( FormatSettings.CompressionSettings == TC_Alpha)
	{
		TextureFormatName = NameBC4;
	}
	else if (FormatSettings.CompressionSettings == TC_DistanceFieldFont)
	{
		TextureFormatName = NameG8;
	}
	else if ( FormatSettings.CompressionSettings == TC_HDR_Compressed )
	{
		TextureFormatName = NameBC6H;
	}
	else if ( FormatSettings.CompressionSettings == TC_BC7 )
	{
		TextureFormatName = NameBC7;
	}
	else if (FormatSettings.CompressionSettings == TC_HalfFloat)
	{
		TextureFormatName = NameR16F;
	}
	else if (FormatSettings.CompressionNoAlpha)
	{
		TextureFormatName = NameDXT1;
	}
	else if (Texture->bDitherMipMapAlpha)
	{
		TextureFormatName = NameDXT5;
	}
	else
	{
		TextureFormatName = NameAutoDXT;
	}

	// Some PC GPUs don't support sRGB read from G8 textures (e.g. AMD DX10 cards on ShaderModel3.0)
	// This solution requires 4x more memory but a lot of PC HW emulate the format anyway
	if ((TextureFormatName == NameG8) && FormatSettings.SRGB && !TargetPlatform->SupportsFeature(ETargetPlatformFeatures::GrayscaleSRGB))
	{
		TextureFormatName = NameBGRA8;
	}

	// fallback to non-DX11 formats if one was chosen, but we can't use it
	if (!bSupportDX11TextureFormats)
	{
		if (TextureFormatName == NameBC6H)
		{
			TextureFormatName = NameRGBA16F;
		}
		else if (TextureFormatName == NameBC7)
		{
			TextureFormatName = NameBGRA8;
		}
	}
	
	bOodleTextureSdkVersionIsNone = Texture->OodleTextureSdkVersion == NAME_None;
#endif //WITH_EDITOR

	return ConditionalGetPrefixedFormat(TextureFormatName, TargetPlatform, bOodleTextureSdkVersionIsNone);
}

void GetDefaultTextureFormatNamePerLayer(TArray<FName>& OutFormatNames, const class ITargetPlatform* TargetPlatform, const class UTexture* Texture, bool bSupportDX11TextureFormats, bool bSupportCompressedVolumeTexture, int32 BlockSize)
{
#if WITH_EDITOR
	OutFormatNames.Reserve(Texture->Source.GetNumLayers());
	for (int32 LayerIndex = 0; LayerIndex < Texture->Source.GetNumLayers(); ++LayerIndex)
	{
		OutFormatNames.Add(GetDefaultTextureFormatName(TargetPlatform, Texture, LayerIndex, bSupportDX11TextureFormats, bSupportCompressedVolumeTexture, BlockSize));
	}
#endif // WITH_EDITOR
}

void GetAllDefaultTextureFormats(const class ITargetPlatform* TargetPlatform, TArray<FName>& OutFormats, bool bSupportDX11TextureFormats)
{
#if WITH_EDITOR
	static FName NameDXT1(TEXT("DXT1"));
	static FName NameDXT3(TEXT("DXT3"));
	static FName NameDXT5(TEXT("DXT5"));
	static FName NameDXT5n(TEXT("DXT5n"));
	static FName NameAutoDXT(TEXT("AutoDXT"));
	static FName NameBC4(TEXT("BC4"));
	static FName NameBC5(TEXT("BC5"));
	static FName NameBGRA8(TEXT("BGRA8"));
	static FName NameXGXR8(TEXT("XGXR8"));
	static FName NameG8(TEXT("G8"));
	static FName NameG16(TEXT("G16"));
	static FName NameVU8(TEXT("VU8"));
	static FName NameRGBA16F(TEXT("RGBA16F"));
	static FName NameR16F(TEXT("R16F"));
	static FName NameBC6H(TEXT("BC6H"));
	static FName NameBC7(TEXT("BC7"));

	OutFormats.Add(NameDXT1);
	OutFormats.Add(NameDXT3);
	OutFormats.Add(NameDXT5);
	OutFormats.Add(NameDXT5n);
	OutFormats.Add(NameAutoDXT);
	OutFormats.Add(NameBC4);
	OutFormats.Add(NameBC5);
	OutFormats.Add(NameBGRA8);
	OutFormats.Add(NameXGXR8);
	OutFormats.Add(NameG8);
	OutFormats.Add(NameG16);
	OutFormats.Add(NameVU8);
	OutFormats.Add(NameRGBA16F);
	OutFormats.Add(NameR16F);
	if (bSupportDX11TextureFormats)
	{
		OutFormats.Add(NameBC6H);
		OutFormats.Add(NameBC7);
	}

	// go over the original base formats only, and possibly add on to the end of the array if there is a prefix needed
	int NumBaseFormats = OutFormats.Num();
	for (int Index = 0; Index < NumBaseFormats; Index++)
	{
		OutFormats.AddUnique(ConditionalGetPrefixedFormat(OutFormats[Index], TargetPlatform, true));
		OutFormats.AddUnique(ConditionalGetPrefixedFormat(OutFormats[Index], TargetPlatform, false));
	}
#endif
}

#if WITH_EDITOR

void UTexture::NotifyMaterials(const ENotifyMaterialsEffectOnShaders EffectOnShaders)
{
	// Create a material update context to safely update materials.
	{
		FMaterialUpdateContext UpdateContext;

		// Notify any material that uses this texture
		TSet<UMaterial*> BaseMaterialsThatUseThisTexture;
		for (TObjectIterator<UMaterialInterface> It; It; ++It)
		{
			UMaterialInterface* MaterialInterface = *It;
			if (DoesMaterialUseTexture(MaterialInterface, this))
			{
				UpdateContext.AddMaterialInterface(MaterialInterface);
				// This is a bit tricky. We want to make sure all materials using this texture are
				// updated. Materials are always updated. Material instances may also have to be
				// updated and if they have static permutations their children must be updated
				// whether they use the texture or not! The safe thing to do is to add the instance's
				// base material to the update context causing all materials in the tree to update.
				BaseMaterialsThatUseThisTexture.Add(MaterialInterface->GetMaterial());
			}
		}

		// Go ahead and update any base materials that need to be.
		if (EffectOnShaders == ENotifyMaterialsEffectOnShaders::Default)
		{
			for (TSet<UMaterial*>::TConstIterator It(BaseMaterialsThatUseThisTexture); It; ++It)
			{
				(*It)->PostEditChange();
			}
		}
		else
		{
			FPropertyChangedEvent EmptyPropertyUpdateStruct(nullptr);
			for (TSet<UMaterial*>::TConstIterator It(BaseMaterialsThatUseThisTexture); It; ++It)
			{
				(*It)->PostEditChangePropertyInternal(EmptyPropertyUpdateStruct, UMaterial::EPostEditChangeEffectOnShaders::DoesNotInvalidate);
			}
		}
	}
}

#endif //WITH_EDITOR

#if WITH_EDITOR

FTextureSource::FMipData::FMipData(const FTextureSource& InSource, FSharedBuffer InData)
	: TextureSource(InSource)
	, MipData(InData)
{
}

bool FTextureSource::FMipData::GetMipData(TArray64<uint8>& OutMipData, int32 BlockIndex, int32 LayerIndex, int32 MipIndex) const
{
	if (BlockIndex < TextureSource.GetNumBlocks() && LayerIndex < TextureSource.GetNumLayers() && MipIndex < TextureSource.GetNumMips() && !MipData.IsNull())
	{
		const int64 MipOffset = TextureSource.CalcMipOffset(BlockIndex, LayerIndex, MipIndex);
		const int64 MipSize = TextureSource.CalcMipSize(BlockIndex, LayerIndex, MipIndex);

		if ((int64)MipData.GetSize() >= MipOffset + MipSize)
		{
			OutMipData.Empty(MipSize);
			OutMipData.AddUninitialized(MipSize);
			FMemory::Memcpy(
				OutMipData.GetData(),
				(const uint8*)MipData.GetData() + MipOffset,
				MipSize
			);
		}

		return true;
	}
	else
	{
		return false;
	}
}

#endif //WITH_EDITOR

#if WITH_EDITOR

FTextureSource::FMipAllocation::FMipAllocation(FSharedBuffer SrcData)
	: ReadOnlyReference(SrcData)
{
}

FTextureSource::FMipAllocation::FMipAllocation(FTextureSource::FMipAllocation&& Other)
{
	*this = MoveTemp(Other);
}

FTextureSource::FMipAllocation& FTextureSource::FMipAllocation::operator =(FTextureSource::FMipAllocation&& Other)
{
	ReadOnlyReference = MoveTemp(Other.ReadOnlyReference);
	ReadWriteBuffer = MoveTemp(Other.ReadWriteBuffer);

	return *this;
}

void FTextureSource::FMipAllocation::Reset()
{
	ReadOnlyReference.Reset();
	ReadWriteBuffer = nullptr;
}

uint8* FTextureSource::FMipAllocation::GetDataReadWrite()
{
	if (!ReadWriteBuffer.IsValid())
	{
		CreateReadWriteBuffer(ReadOnlyReference.GetData(), ReadOnlyReference.GetSize());
	}

	return ReadWriteBuffer.Get();
}

FSharedBuffer FTextureSource::FMipAllocation::Release()
{
	if (ReadWriteBuffer.IsValid())
	{
		const int64 DataSize = ReadOnlyReference.GetSize();
		ReadOnlyReference.Reset();
		return FSharedBuffer::TakeOwnership(ReadWriteBuffer.Release(), DataSize, FMemory::Free);
	}
	else
	{
		return MoveTemp(ReadOnlyReference);
	}
}

void FTextureSource::FMipAllocation::CreateReadWriteBuffer(const void* SrcData, int64 DataLength)
{
	if (DataLength > 0)
	{
		ReadWriteBuffer = TUniquePtr<uint8, FDeleterFree>((uint8*)FMemory::Malloc(DataLength));
		FMemory::Memcpy(ReadWriteBuffer.Get(), SrcData, DataLength);
	}

	ReadOnlyReference = FSharedBuffer::MakeView(ReadWriteBuffer.Get(), DataLength);
}

void FTextureSource::InitLayeredImpl(
	int32 NewSizeX,
	int32 NewSizeY,
	int32 NewNumSlices,
	int32 NewNumLayers,
	int32 NewNumMips,
	const ETextureSourceFormat* NewLayerFormat)
{
	RemoveSourceData();
	SizeX = NewSizeX;
	SizeY = NewSizeY;
	NumLayers = NewNumLayers;
	NumSlices = NewNumSlices;
	NumMips = NewNumMips;
	Format = NewLayerFormat[0];
	LayerFormat.SetNum(NewNumLayers, true);
	for (int i = 0; i < NewNumLayers; ++i)
	{
		LayerFormat[i] = NewLayerFormat[i];
	}

	BlockDataOffsets.Add(0);

	checkf(LockState == ELockState::None, TEXT("InitLayered shouldn't be called in-between LockMip/UnlockMip"));
}

void FTextureSource::InitBlockedImpl(const ETextureSourceFormat* InLayerFormats,
	const FTextureSourceBlock* InBlocks,
	int32 InNumLayers,
	int32 InNumBlocks)
{
	check(InNumBlocks > 0);
	check(InNumLayers > 0);

	RemoveSourceData();

	BaseBlockX = InBlocks[0].BlockX;
	BaseBlockY = InBlocks[0].BlockY;
	SizeX = InBlocks[0].SizeX;
	SizeY = InBlocks[0].SizeY;
	NumSlices = InBlocks[0].NumSlices;
	NumMips = InBlocks[0].NumMips;

	NumLayers = InNumLayers;
	Format = InLayerFormats[0];

	Blocks.Reserve(InNumBlocks - 1);
	for (int32 BlockIndex = 1; BlockIndex < InNumBlocks; ++BlockIndex)
	{
		Blocks.Add(InBlocks[BlockIndex]);
	}

	LayerFormat.SetNum(InNumLayers, true);
	for (int i = 0; i < InNumLayers; ++i)
	{
		LayerFormat[i] = InLayerFormats[i];
	}

	EnsureBlocksAreSorted();

	checkf(LockState == ELockState::None, TEXT("InitBlocked shouldn't be called in-between LockMip/UnlockMip"));
}

namespace
{
struct FSortedTextureSourceBlock
{
	FTextureSourceBlock Block;
	int64 DataOffset;
	int32 SourceBlockIndex;
	int32 SortKey;
};
inline bool operator<(const FSortedTextureSourceBlock& Lhs, const FSortedTextureSourceBlock& Rhs)
{
	return Lhs.SortKey < Rhs.SortKey;
}
} // namespace

bool FTextureSource::EnsureBlocksAreSorted()
{
	const int32 NumBlocks = GetNumBlocks();
	if (BlockDataOffsets.Num() == NumBlocks)
	{
		return false;
	}

	BlockDataOffsets.Empty(NumBlocks);
	if (NumBlocks > 1)
	{
		const FIntPoint SizeInBlocks = GetSizeInBlocks();

		TArray<FSortedTextureSourceBlock> SortedBlocks;
		SortedBlocks.Empty(NumBlocks);

		int64 CurrentDataOffset = 0;
		for (int32 BlockIndex = 0; BlockIndex < NumBlocks; ++BlockIndex)
		{
			FSortedTextureSourceBlock& SortedBlock = SortedBlocks.AddDefaulted_GetRef();
			GetBlock(BlockIndex, SortedBlock.Block);
			SortedBlock.SourceBlockIndex = BlockIndex;
			SortedBlock.DataOffset = CurrentDataOffset;
			SortedBlock.SortKey = SortedBlock.Block.BlockY * SizeInBlocks.X + SortedBlock.Block.BlockX;
			CurrentDataOffset += CalcBlockSize(SortedBlock.Block);
		}
		SortedBlocks.Sort();

		BlockDataOffsets.Add(SortedBlocks[0].DataOffset);
		BaseBlockX = SortedBlocks[0].Block.BlockX;
		BaseBlockY = SortedBlocks[0].Block.BlockY;
		SizeX = SortedBlocks[0].Block.SizeX;
		SizeY = SortedBlocks[0].Block.SizeY;
		NumSlices = SortedBlocks[0].Block.NumSlices;
		NumMips = SortedBlocks[0].Block.NumMips;
		for (int32 BlockIndex = 1; BlockIndex < NumBlocks; ++BlockIndex)
		{
			const FSortedTextureSourceBlock& SortedBlock = SortedBlocks[BlockIndex];
			BlockDataOffsets.Add(SortedBlock.DataOffset);
			Blocks[BlockIndex - 1] = SortedBlock.Block;
		}
	}
	else
	{
		BlockDataOffsets.Add(0);
	}

	return true;
}

#endif //WITH_EDITOR
