// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	ShadowDepthRendering.cpp: Shadow depth rendering implementation
=============================================================================*/

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "Misc/MemStack.h"
#include "RHIDefinitions.h"
#include "HAL/IConsoleManager.h"
#include "Async/TaskGraphInterfaces.h"
#include "RHI.h"
#include "HitProxies.h"
#include "ShaderParameters.h"
#include "RenderResource.h"
#include "RendererInterface.h"
#include "PrimitiveViewRelevance.h"
#include "UniformBuffer.h"
#include "Shader.h"
#include "StaticBoundShaderState.h"
#include "SceneUtils.h"
#include "Materials/Material.h"
#include "RHIStaticStates.h"
#include "PostProcess/SceneRenderTargets.h"
#include "GlobalShader.h"
#include "MaterialShaderType.h"
#include "MaterialShader.h"
#include "MeshMaterialShader.h"
#include "ShaderBaseClasses.h"
#include "ShadowRendering.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "PostProcess/SceneFilterRendering.h"
#include "ScreenRendering.h"
#include "ClearQuad.h"
#include "PipelineStateCache.h"
#include "MeshPassProcessor.inl"
#include "VisualizeTexture.h"
#include "GPUScene.h"
#include "SceneTextureReductions.h"
#include "RendererModule.h"
#include "PixelShaderUtils.h"
#include "VirtualShadowMaps/VirtualShadowMapCacheManager.h"
#include "VirtualShadowMaps/VirtualShadowMapClipmap.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Rendering/NaniteResources.h"
#include "Rendering/NaniteStreamingManager.h"

DECLARE_GPU_DRAWCALL_STAT_NAMED(ShadowDepths, TEXT("Shadow Depths"));

IMPLEMENT_STATIC_UNIFORM_BUFFER_STRUCT(FShadowDepthPassUniformParameters, "ShadowDepthPass", SceneTextures);
IMPLEMENT_STATIC_UNIFORM_BUFFER_STRUCT(FMobileShadowDepthPassUniformParameters, "MobileShadowDepthPass", SceneTextures);

template<bool bUsingVertexLayers = false>
class TScreenVSForGS : public FScreenVS
{
public:
	DECLARE_SHADER_TYPE(TScreenVSForGS, Global);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) && (!bUsingVertexLayers || (!RHISupportsGeometryShaders(Parameters.Platform) && RHISupportsVertexShaderLayer(Parameters.Platform)));
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FScreenVS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("USING_LAYERS"), (uint32)(bUsingVertexLayers ? 1 : 0));
		if (!bUsingVertexLayers)
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_VertexToGeometryShader);
		}
	}

	TScreenVSForGS() = default;
	TScreenVSForGS(const ShaderMetaType::CompiledShaderInitializerType& Initializer) :
		FScreenVS(Initializer)
	{}
};

IMPLEMENT_SHADER_TYPE(template<>, TScreenVSForGS<false>, TEXT("/Engine/Private/ScreenVertexShader.usf"), TEXT("MainForGS"), SF_Vertex);
IMPLEMENT_SHADER_TYPE(template<>, TScreenVSForGS<true>, TEXT("/Engine/Private/ScreenVertexShader.usf"), TEXT("MainForGS"), SF_Vertex);


static TAutoConsoleVariable<int32> CVarShadowForceSerialSingleRenderPass(
	TEXT("r.Shadow.ForceSerialSingleRenderPass"),
	0,
	TEXT("Force Serial shadow passes to render in 1 pass."),
	ECVF_RenderThreadSafe);

TAutoConsoleVariable<int32> CVarNaniteShadows(
	TEXT("r.Shadow.Nanite"),
	1,
	TEXT("Enables shadows from Nanite meshes."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNaniteShadowsUseHZB(
	TEXT("r.Shadow.NaniteUseHZB"),
	1,
	TEXT("Enables HZB for Nanite shadows."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarShadowsVirtualUseHZB(
	TEXT("r.Shadow.Virtual.UseHZB"),
	1,
	TEXT("Enables HZB for Virtual Shadow Maps."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarNaniteShadowsLODBias(
	TEXT("r.Shadow.NaniteLODBias"),
	1.0f,
	TEXT("LOD bias for nanite geometry in shadows. 0 = full detail. >0 = reduced detail."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarNaniteShadowsUpdateStreaming(
	TEXT("r.Shadow.NaniteUpdateStreaming"),
	1,
	TEXT("Produce Nanite geometry streaming requests from shadow map rendering."),
	ECVF_RenderThreadSafe);

extern int32 GNaniteShowStats;
extern int32 GEnableNonNaniteVSM;

namespace Nanite
{
	extern bool IsStatFilterActive(const FString& FilterName);
	extern bool IsStatFilterActiveForLight(const FLightSceneProxy* LightProxy);
	extern FString GetFilterNameForLight(const FLightSceneProxy* LightProxy);
}

// Multiply PackedView.LODScale by return value when rendering Nanite shadows
static float ComputeNaniteShadowsLODScaleFactor()
{
	return FMath::Pow(2.0f, -CVarNaniteShadowsLODBias.GetValueOnRenderThread());
}

void SetupShadowDepthPassUniformBuffer(
	const FProjectedShadowInfo* ShadowInfo,
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FShadowDepthPassUniformParameters& ShadowDepthPassParameters)
{
	static const auto CSMCachingCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Shadow.CSMCaching"));
	const bool bCSMCachingEnabled = CSMCachingCVar && CSMCachingCVar->GetValueOnAnyThread() != 0;

	SetupSceneTextureUniformParameters(GraphBuilder, View.FeatureLevel, ESceneTextureSetupMode::None, ShadowDepthPassParameters.SceneTextures);

	ShadowDepthPassParameters.ProjectionMatrix = FTranslationMatrix44f(FVector3f(ShadowInfo->PreShadowTranslation - View.ViewMatrices.GetPreViewTranslation())) * ShadowInfo->TranslatedWorldToClipOuterMatrix;		// LWC_TDOO: Precision loss?
	ShadowDepthPassParameters.ViewMatrix = FMatrix44f(ShadowInfo->TranslatedWorldToView);	// LWC_TODO: Precision loss

	// Disable the SlopeDepthBias because we couldn't reconstruct the depth offset if it is not 0.0f when scrolling the cached shadow map.
	ShadowDepthPassParameters.ShadowParams = FVector4f(ShadowInfo->GetShaderDepthBias(), bCSMCachingEnabled ? 0.0f : ShadowInfo->GetShaderSlopeDepthBias(), ShadowInfo->GetShaderMaxSlopeDepthBias(), ShadowInfo->bOnePassPointLightShadow ? 1 : ShadowInfo->InvMaxSubjectDepth);
	ShadowDepthPassParameters.bClampToNearPlane = ShadowInfo->ShouldClampToNearPlane() ? 1.0f : 0.0f;

	if (ShadowInfo->bOnePassPointLightShadow)
	{
		check(ShadowInfo->BorderSize == 0);

		// offset from translated world space to (pre translated) shadow space 
		const FMatrix Translation = FTranslationMatrix(ShadowInfo->PreShadowTranslation - View.ViewMatrices.GetPreViewTranslation());

		for (int32 FaceIndex = 0; FaceIndex < 6; FaceIndex++)
		{
			ShadowDepthPassParameters.ShadowViewProjectionMatrices[FaceIndex] = FMatrix44f(Translation * ShadowInfo->OnePassShadowViewProjectionMatrices[FaceIndex]);		// LWC_TODO: Precision loss?
			ShadowDepthPassParameters.ShadowViewMatrices[FaceIndex] = FMatrix44f(Translation * ShadowInfo->OnePassShadowViewMatrices[FaceIndex]);
		}
	}

	ShadowDepthPassParameters.bRenderToVirtualShadowMap = false;
	ShadowDepthPassParameters.VirtualSmPageTable = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(uint32)));
	ShadowDepthPassParameters.PackedNaniteViews = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(Nanite::FPackedView)));
	ShadowDepthPassParameters.PageRectBounds = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultStructuredBuffer(GraphBuilder, sizeof(FIntVector4)));

	FRDGTextureRef DepthBuffer = GraphBuilder.CreateTexture( FRDGTextureDesc::Create2D( FIntPoint(4,4), PF_R32_UINT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_UAV ), TEXT("Dummy-OutDepthBuffer") );

	ShadowDepthPassParameters.OutDepthBuffer = GraphBuilder.CreateUAV( DepthBuffer );
}

void SetupShadowDepthPassUniformBuffer(
	const FProjectedShadowInfo* ShadowInfo,
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FMobileShadowDepthPassUniformParameters& ShadowDepthPassParameters)
{
	SetupMobileSceneTextureUniformParameters(GraphBuilder, EMobileSceneTextureSetupMode::None, ShadowDepthPassParameters.SceneTextures);

	ShadowDepthPassParameters.ProjectionMatrix = FTranslationMatrix44f(FVector3f(ShadowInfo->PreShadowTranslation - View.ViewMatrices.GetPreViewTranslation())) * ShadowInfo->TranslatedWorldToClipOuterMatrix;		// LWC_TODO: Precision loss
	ShadowDepthPassParameters.ViewMatrix = FMatrix44f(ShadowInfo->TranslatedWorldToView);

	ShadowDepthPassParameters.ShadowParams = FVector4f(ShadowInfo->GetShaderDepthBias(), ShadowInfo->GetShaderSlopeDepthBias(), ShadowInfo->GetShaderMaxSlopeDepthBias(), ShadowInfo->InvMaxSubjectDepth);
	ShadowDepthPassParameters.bClampToNearPlane = ShadowInfo->ShouldClampToNearPlane() ? 1.0f : 0.0f;
}

void AddClearShadowDepthPass(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture)
{
	// Clear atlas depth, but ignore stencil.
	auto* PassParameters = GraphBuilder.AllocParameters<FRenderTargetParameters>();
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(Texture, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);
	GraphBuilder.AddPass(RDG_EVENT_NAME("ClearShadowDepth"), PassParameters, ERDGPassFlags::Raster, [](FRHICommandList&) {});
}

void AddClearShadowDepthPass(FRDGBuilder& GraphBuilder, FRDGTextureRef Texture, const FProjectedShadowInfo* ProjectedShadowInfo)
{
	// Clear atlas depth, but ignore stencil.
	auto* PassParameters = GraphBuilder.AllocParameters<FRenderTargetParameters>();
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(Texture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthWrite_StencilNop);
	GraphBuilder.AddPass(RDG_EVENT_NAME("ClearShadowDepthTile"), PassParameters, ERDGPassFlags::Raster, [ProjectedShadowInfo](FRHICommandList& RHICmdList)
	{
		ProjectedShadowInfo->ClearDepth(RHICmdList);
	});
}

class FShadowDepthShaderElementData : public FMeshMaterialShaderElementData
{
public:
	int32 LayerId;
	int32 bUseGpuSceneInstancing;
};

/**
* A vertex shader for rendering the depth of a mesh.
*/
class FShadowDepthVS : public FMeshMaterialShader
{
public:
	DECLARE_INLINE_TYPE_LAYOUT(FShadowDepthVS, NonVirtual);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return false;
	}

	void GetShaderBindings(
		const FScene* Scene,
		ERHIFeatureLevel::Type FeatureLevel,
		const FPrimitiveSceneProxy* PrimitiveSceneProxy,
		const FMaterialRenderProxy& MaterialRenderProxy,
		const FMaterial& Material,
		const FMeshPassProcessorRenderState& DrawRenderState,
		const FShadowDepthShaderElementData& ShaderElementData,
		FMeshDrawSingleShaderBindings& ShaderBindings) const
	{
		FMeshMaterialShader::GetShaderBindings(Scene, FeatureLevel, PrimitiveSceneProxy, MaterialRenderProxy, Material, DrawRenderState, ShaderElementData, ShaderBindings);

		ShaderBindings.Add(LayerId, ShaderElementData.LayerId);
		ShaderBindings.Add(bUseGpuSceneInstancing, ShaderElementData.bUseGpuSceneInstancing);
	}

	FShadowDepthVS() = default;
	FShadowDepthVS(const ShaderMetaType::CompiledShaderInitializerType & Initializer) :
		FMeshMaterialShader(Initializer)
	{
		LayerId.Bind(Initializer.ParameterMap, TEXT("LayerId"));
		bUseGpuSceneInstancing.Bind(Initializer.ParameterMap, TEXT("bUseGpuSceneInstancing"));
	}

private:
	LAYOUT_FIELD(FShaderParameter, LayerId);
	LAYOUT_FIELD(FShaderParameter, bUseGpuSceneInstancing);
};

enum EShadowDepthVertexShaderMode
{
	VertexShadowDepth_PerspectiveCorrect,
	VertexShadowDepth_OutputDepth,
	VertexShadowDepth_OnePassPointLight,
	VertexShadowDepth_VirtualShadowMap,
};

static TAutoConsoleVariable<int32> CVarSupportPointLightWholeSceneShadows(
	TEXT("r.SupportPointLightWholeSceneShadows"),
	1,
	TEXT("Enables shadowcasting point lights."),
	ECVF_ReadOnly | ECVF_RenderThreadSafe);

static bool MobileUsesPerspectiveCorrectShadowPermutation(EShaderPlatform ShaderPlatform)
{
	// Required only for spotlight shadows on mobile
	static FShaderPlatformCachedIniValue<bool> MobileEnableMovableSpotlightsShadowIniValue(TEXT("r.Mobile.EnableMovableSpotlightsShadow"));
	const bool bMobileEnableMovableSpotlightsShadow = (MobileEnableMovableSpotlightsShadowIniValue.Get(ShaderPlatform) != 0);
	return bMobileEnableMovableSpotlightsShadow;
}


static TAutoConsoleVariable<int32> CVarDetectVertexShaderLayerAtRuntime(
	TEXT("r.Shadow.DetectVertexShaderLayerAtRuntime"),
	0,
	TEXT("Forces the compilation of the vslayer shader permutation even if the platform (RHI) does not declare compile-time support through RHISupportsVertexShaderLayer.")
	TEXT("Enabled by default for windows/SM5 as DX11 almost universally supports this at runtime."),
	ECVF_ReadOnly | ECVF_RenderThreadSafe);


/**
* A vertex shader for rendering the depth of a mesh.
*/
template <EShadowDepthVertexShaderMode ShaderMode, bool bUsePositionOnlyStream>
class TShadowDepthVS : public FShadowDepthVS
{
public:
	DECLARE_SHADER_TYPE(TShadowDepthVS, MeshMaterial);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		const EShaderPlatform Platform = Parameters.Platform;

		static const auto SupportAllShaderPermutationsVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.SupportAllShaderPermutations"));
		const bool bForceAllPermutations = SupportAllShaderPermutationsVar && SupportAllShaderPermutationsVar->GetValueOnAnyThread() != 0;
		const bool bSupportPointLightWholeSceneShadows = CVarSupportPointLightWholeSceneShadows.GetValueOnAnyThread() != 0 || bForceAllPermutations;

		// Mobile only needs OutputDepth, and optionally PerspectiveCorrect
		if (!IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5) &&
			!(ShaderMode == VertexShadowDepth_OutputDepth || 
				(ShaderMode == VertexShadowDepth_PerspectiveCorrect && MobileUsesPerspectiveCorrectShadowPermutation(Platform))))
		{
			return false;
		}

		// Compile VS layer permutation if RHI supports it unconditionally OR we have forced it on (default for DX11&12 at SM5)
		static FShaderPlatformCachedIniValue<bool> DetectVertexShaderLayerRuntimeIniValue(TEXT("r.Shadow.DetectVertexShaderLayerAtRuntime"));
		const bool bShouldCompileVSLayer = RHISupportsVertexShaderLayer(Platform) || DetectVertexShaderLayerRuntimeIniValue.Get(Platform) != 0;
		if (ShaderMode == VertexShadowDepth_OnePassPointLight && !bShouldCompileVSLayer)
		{
			return false;
		}

		if (ShaderMode == VertexShadowDepth_VirtualShadowMap &&
			(!IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5) || !UseGPUScene(Platform) || GEnableNonNaniteVSM == 0))
		{
			return false;
		}


		//Note: This logic needs to stay in sync with OverrideWithDefaultMaterialForShadowDepth!
		return (Parameters.MaterialParameters.bIsSpecialEngineMaterial
			// Masked and WPO materials need their shaders but cannot be used with a position only stream.
			|| ((!Parameters.MaterialParameters.bWritesEveryPixelShadowPass || Parameters.MaterialParameters.bMaterialMayModifyMeshPosition) && !bUsePositionOnlyStream))
			// Only compile one pass point light shaders for feature levels >= SM5
				&& (ShaderMode != VertexShadowDepth_OnePassPointLight || IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5))
			// Only compile position-only shaders for vertex factories that support it. (Note: this assumes that a vertex factor which supports PositionOnly, supports also PositionAndNormalOnly)
			&& (!bUsePositionOnlyStream || Parameters.VertexFactoryType->SupportsPositionOnly())
			// Don't render ShadowDepth for translucent unlit materials
			&& Parameters.MaterialParameters.bShouldCastDynamicShadows;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FShadowDepthVS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("PERSPECTIVE_CORRECT_DEPTH"), (uint32)(ShaderMode == VertexShadowDepth_PerspectiveCorrect));
		OutEnvironment.SetDefine(TEXT("ONEPASS_POINTLIGHT_SHADOW"), (uint32)(ShaderMode == VertexShadowDepth_OnePassPointLight));
		OutEnvironment.SetDefine(TEXT("POSITION_ONLY"), (uint32)bUsePositionOnlyStream);

		bool bEnableNonNaniteVSM = (ShaderMode == VertexShadowDepth_VirtualShadowMap);
		OutEnvironment.SetDefine(TEXT("ENABLE_NON_NANITE_VSM"), bEnableNonNaniteVSM ? 1 : 0);
		if (bEnableNonNaniteVSM)
		{
			FVirtualShadowMapArray::SetShaderDefines(OutEnvironment);
		}

		if (ShaderMode == VertexShadowDepth_OnePassPointLight)
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_VertexUseAutoCulling);
		}
	}

	TShadowDepthVS() = default;
	TShadowDepthVS(const ShaderMetaType::CompiledShaderInitializerType & Initializer)
		: FShadowDepthVS(Initializer)
	{}
};

#define IMPLEMENT_SHADOW_DEPTH_SHADERMODE_SHADERS(ShaderMode) \
	typedef TShadowDepthVS<ShaderMode, false> TShadowDepthVS##ShaderMode; \
	IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVS##ShaderMode, TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("Main"),       SF_Vertex);

IMPLEMENT_SHADOW_DEPTH_SHADERMODE_SHADERS(VertexShadowDepth_PerspectiveCorrect);
IMPLEMENT_SHADOW_DEPTH_SHADERMODE_SHADERS(VertexShadowDepth_OutputDepth);
IMPLEMENT_SHADOW_DEPTH_SHADERMODE_SHADERS(VertexShadowDepth_OnePassPointLight);
IMPLEMENT_SHADOW_DEPTH_SHADERMODE_SHADERS(VertexShadowDepth_VirtualShadowMap);

// Position only vertex shaders.
typedef TShadowDepthVS<VertexShadowDepth_PerspectiveCorrect, true> TShadowDepthVSVertexShadowDepth_PerspectiveCorrectPositionOnly;
typedef TShadowDepthVS<VertexShadowDepth_OutputDepth,        true> TShadowDepthVSVertexShadowDepth_OutputDepthPositionOnly;
typedef TShadowDepthVS<VertexShadowDepth_OnePassPointLight,  true> TShadowDepthVSVertexShadowDepth_OnePassPointLightPositionOnly;
typedef TShadowDepthVS<VertexShadowDepth_VirtualShadowMap,   true> TShadowDepthVSVertexShadowDepth_VirtualShadowMapPositionOnly;
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_PerspectiveCorrectPositionOnly, TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMain"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_OutputDepthPositionOnly,        TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMain"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_OnePassPointLightPositionOnly,  TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMain"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, TShadowDepthVSVertexShadowDepth_VirtualShadowMapPositionOnly,   TEXT("/Engine/Private/ShadowDepthVertexShader.usf"), TEXT("PositionOnlyMain"), SF_Vertex);


/**
* A pixel shader for rendering the depth of a mesh.
*/
class FShadowDepthBasePS : public FMeshMaterialShader
{
	DECLARE_INLINE_TYPE_LAYOUT(FShadowDepthBasePS, NonVirtual);
public:

	FShadowDepthBasePS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		const ERHIFeatureLevel::Type FeatureLevel = GetMaxSupportedFeatureLevel((EShaderPlatform)Initializer.Target.Platform);

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Deferred)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}

		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Mobile)
		{
			PassUniformBuffer.Bind(Initializer.ParameterMap, FMobileShadowDepthPassUniformParameters::StaticStructMetadata.GetShaderVariableName());
		}
	}

	FShadowDepthBasePS() = default;
};

enum EShadowDepthPixelShaderMode
{
	PixelShadowDepth_NonPerspectiveCorrect,
	PixelShadowDepth_PerspectiveCorrect,
	PixelShadowDepth_OnePassPointLight,
	PixelShadowDepth_VirtualShadowMap
};

template <EShadowDepthPixelShaderMode ShaderMode>
class TShadowDepthPS : public FShadowDepthBasePS
{
	DECLARE_SHADER_TYPE(TShadowDepthPS, MeshMaterial);
public:

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		const EShaderPlatform Platform = Parameters.Platform;
		
		// Mobile only needs NonPerspectiveCorrect, and optionally PerspectiveCorrect
		if (!IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5) &&
			!(ShaderMode == PixelShadowDepth_NonPerspectiveCorrect || 
				(ShaderMode == PixelShadowDepth_PerspectiveCorrect && MobileUsesPerspectiveCorrectShadowPermutation(Platform))))
		{
			return false;
		}

		if (ShaderMode == PixelShadowDepth_VirtualShadowMap &&
			(!IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5) || !UseGPUScene(Platform) || GEnableNonNaniteVSM == 0))
		{
			return false;
		}

		bool bModeRequiresPS = ShaderMode == PixelShadowDepth_PerspectiveCorrect || ShaderMode == PixelShadowDepth_VirtualShadowMap;

		//Note: This logic needs to stay in sync with OverrideWithDefaultMaterialForShadowDepth!
		return (Parameters.MaterialParameters.bIsSpecialEngineMaterial
			// Only compile for masked or lit translucent materials
			|| !Parameters.MaterialParameters.bWritesEveryPixelShadowPass
			|| (Parameters.MaterialParameters.bMaterialMayModifyMeshPosition && Parameters.MaterialParameters.bIsUsedWithInstancedStaticMeshes)
			// This mode needs a pixel shader and WPO materials can't be overridden with default material.
			|| (bModeRequiresPS && Parameters.MaterialParameters.bMaterialMayModifyMeshPosition))
			// Don't render ShadowDepth for translucent unlit materials
			&& Parameters.MaterialParameters.bShouldCastDynamicShadows;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FShadowDepthBasePS::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("PERSPECTIVE_CORRECT_DEPTH"), (uint32)(ShaderMode == PixelShadowDepth_PerspectiveCorrect));
		OutEnvironment.SetDefine(TEXT("ONEPASS_POINTLIGHT_SHADOW"), (uint32)(ShaderMode == PixelShadowDepth_OnePassPointLight));
		OutEnvironment.SetDefine(TEXT("VIRTUAL_TEXTURE_TARGET"), (uint32)(ShaderMode == PixelShadowDepth_VirtualShadowMap));

		bool bEnableNonNaniteVSM = (ShaderMode == PixelShadowDepth_VirtualShadowMap);
		OutEnvironment.SetDefine(TEXT("ENABLE_NON_NANITE_VSM"), bEnableNonNaniteVSM ? 1 : 0);
		if (bEnableNonNaniteVSM != 0)
		{
			FVirtualShadowMapArray::SetShaderDefines(OutEnvironment);
		}
	}

	TShadowDepthPS()
	{
	}

	TShadowDepthPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FShadowDepthBasePS(Initializer)
	{
	}
};

// typedef required to get around macro expansion failure due to commas in template argument list for TShadowDepthPixelShader
#define IMPLEMENT_SHADOWDEPTHPASS_PIXELSHADER_TYPE(ShaderMode) \
	typedef TShadowDepthPS<ShaderMode> TShadowDepthPS##ShaderMode; \
	IMPLEMENT_MATERIAL_SHADER_TYPE(template<>,TShadowDepthPS##ShaderMode,TEXT("/Engine/Private/ShadowDepthPixelShader.usf"),TEXT("Main"),SF_Pixel);

IMPLEMENT_SHADOWDEPTHPASS_PIXELSHADER_TYPE(PixelShadowDepth_NonPerspectiveCorrect);
IMPLEMENT_SHADOWDEPTHPASS_PIXELSHADER_TYPE(PixelShadowDepth_PerspectiveCorrect);
IMPLEMENT_SHADOWDEPTHPASS_PIXELSHADER_TYPE(PixelShadowDepth_OnePassPointLight);
IMPLEMENT_SHADOWDEPTHPASS_PIXELSHADER_TYPE(PixelShadowDepth_VirtualShadowMap);

/**
* Overrides a material used for shadow depth rendering with the default material when appropriate.
* Overriding in this manner can reduce state switches and the number of shaders that have to be compiled.
* This logic needs to stay in sync with shadow depth shader ShouldCache logic.
*/
void OverrideWithDefaultMaterialForShadowDepth(
	const FMaterialRenderProxy*& InOutMaterialRenderProxy,
	const FMaterial*& InOutMaterialResource,
	ERHIFeatureLevel::Type InFeatureLevel)
{
	// Override with the default material when possible.
	if (InOutMaterialResource->WritesEveryPixel(true) &&						// Don't override masked materials.
		!InOutMaterialResource->MaterialModifiesMeshPosition_RenderThread())	// Don't override materials using world position offset.
	{
		const FMaterialRenderProxy* DefaultProxy = UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
		const FMaterial* DefaultMaterialResource = DefaultProxy->GetMaterialNoFallback(InFeatureLevel);
		check(DefaultMaterialResource);

		// Override with the default material for opaque materials that don't modify mesh position.
		InOutMaterialRenderProxy = DefaultProxy;
		InOutMaterialResource = DefaultMaterialResource;
	}
}

bool GetShadowDepthPassShaders(
	const FMaterial& Material,
	const FVertexFactory* VertexFactory,
	ERHIFeatureLevel::Type FeatureLevel,
	bool bDirectionalLight,
	bool bOnePassPointLightShadow,
	bool bPositionOnlyVS,
	bool bUsePerspectiveCorrectShadowDepths,
	bool bVirtualShadowMap,
	TShaderRef<FShadowDepthVS>& VertexShader,
	TShaderRef<FShadowDepthBasePS>& PixelShader)
{
	const FVertexFactoryType* VFType = VertexFactory->GetType();

	FMaterialShaderTypes ShaderTypes;

	// Vertex related shaders
	if (bOnePassPointLightShadow)
	{
		if (DoesRuntimeSupportOnePassPointLightShadows(GShaderPlatformForFeatureLevel[FeatureLevel]))
		{
			if (bPositionOnlyVS)
			{
				ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_OnePassPointLight, true>>();
			}
			else
			{
				ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_OnePassPointLight, false>>();
			}
		}
		else
		{
			return false;
		}
	}
	else if (bVirtualShadowMap)
	{
		if (bPositionOnlyVS)
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_VirtualShadowMap, true>>();
		}
		else
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_VirtualShadowMap, false>>();
		}
	}
	else if (bUsePerspectiveCorrectShadowDepths)
	{
		if (bPositionOnlyVS)
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_PerspectiveCorrect, true>>();
		}
		else
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_PerspectiveCorrect, false>>();
		}
	}
	else
	{
		if (bPositionOnlyVS)
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_OutputDepth, true>>();
		}
		else
		{
			ShaderTypes.AddShaderType<TShadowDepthVS<VertexShadowDepth_OutputDepth, false>>();
		}
	}

	// Pixel shaders
	const bool bNullPixelShader = Material.WritesEveryPixel(true) && !bUsePerspectiveCorrectShadowDepths && !bVirtualShadowMap && VertexFactory->SupportsNullPixelShader();
	if (!bNullPixelShader)
	{
		if (bVirtualShadowMap)
		{
			ShaderTypes.AddShaderType<TShadowDepthPS<PixelShadowDepth_VirtualShadowMap>>();
		}
		else if (bUsePerspectiveCorrectShadowDepths)
		{
			ShaderTypes.AddShaderType<TShadowDepthPS<PixelShadowDepth_PerspectiveCorrect>>();
		}
		else if (bOnePassPointLightShadow)
		{
			ShaderTypes.AddShaderType<TShadowDepthPS<PixelShadowDepth_OnePassPointLight>>();
		}
		else
		{
			ShaderTypes.AddShaderType<TShadowDepthPS<PixelShadowDepth_NonPerspectiveCorrect>>();
		}
	}

	FMaterialShaders Shaders;
	if (!Material.TryGetShaders(ShaderTypes, VFType, Shaders))
	{
		return false;
	}

	Shaders.TryGetVertexShader(VertexShader);
	Shaders.TryGetPixelShader(PixelShader);
	return true;
}

/*-----------------------------------------------------------------------------
FProjectedShadowInfo
-----------------------------------------------------------------------------*/

static void CheckShadowDepthMaterials(const FMaterialRenderProxy* InRenderProxy, const FMaterial* InMaterial, ERHIFeatureLevel::Type InFeatureLevel)
{
	const FMaterialRenderProxy* RenderProxy = InRenderProxy;
	const FMaterial* Material = InMaterial;
	OverrideWithDefaultMaterialForShadowDepth(RenderProxy, Material, InFeatureLevel);
	check(RenderProxy == InRenderProxy);
	check(Material == InMaterial);
}

void FProjectedShadowInfo::ClearDepth(FRHICommandList& RHICmdList) const
{
	check(RHICmdList.IsInsideRenderPass());

	const uint32 ViewportMinX = X;
	const uint32 ViewportMinY = Y;
	const float ViewportMinZ = 0.0f;
	const uint32 ViewportMaxX = X + BorderSize * 2 + ResolutionX;
	const uint32 ViewportMaxY = Y + BorderSize * 2 + ResolutionY;
	const float ViewportMaxZ = 1.0f;

	// Clear depth only.
	const int32 NumClearColors = 1;
	const bool bClearColor = false;
	const FLinearColor Colors[] = { FLinearColor::White };

	// Translucent shadows use draw call clear
	check(!bTranslucentShadow);

	RHICmdList.SetViewport(
		ViewportMinX,
		ViewportMinY,
		ViewportMinZ,
		ViewportMaxX,
		ViewportMaxY,
		ViewportMaxZ
	);

	DrawClearQuadMRT(RHICmdList, bClearColor, NumClearColors, Colors, true, 1.0f, false, 0);
}

void FProjectedShadowInfo::SetStateForView(FRHICommandList& RHICmdList) const
{
	check(bAllocated);

	RHICmdList.SetViewport(
		X,
		Y,
		0.0f,
		X + ResolutionX + 2 * BorderSize,
		Y + ResolutionY + 2 * BorderSize,
		1.0f
	);
}

void SetStateForShadowDepth(bool bOnePassPointLightShadow, bool bDirectionalLight, FMeshPassProcessorRenderState& DrawRenderState, EMeshPass::Type InMeshPassTargetType)
{
	// Disable color writes
	DrawRenderState.SetBlendState(TStaticBlendState<CW_NONE>::GetRHI());

	if( InMeshPassTargetType == EMeshPass::VSMShadowDepth )
	{
		DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
	}
	else if (bOnePassPointLightShadow || InMeshPassTargetType == EMeshPass::VSMShadowDepth)
	{
		// Point lights use reverse Z depth maps
		DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
	}
	else
	{
		DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_LessEqual>::GetRHI());
	}
}

static TAutoConsoleVariable<int32> CVarParallelShadows(
	TEXT("r.ParallelShadows"),
	1,
	TEXT("Toggles parallel shadow rendering. Parallel rendering must be enabled for this to have an effect."),
	ECVF_RenderThreadSafe
);
static TAutoConsoleVariable<int32> CVarParallelShadowsNonWholeScene(
	TEXT("r.ParallelShadowsNonWholeScene"),
	0,
	TEXT("Toggles parallel shadow rendering for non whole-scene shadows. r.ParallelShadows must be enabled for this to have an effect."),
	ECVF_RenderThreadSafe
);

static TAutoConsoleVariable<int32> CVarRHICmdFlushRenderThreadTasksShadowPass(
	TEXT("r.RHICmdFlushRenderThreadTasksShadowPass"),
	0,
	TEXT("Wait for completion of parallel render thread tasks at the end of each shadow pass.  A more granular version of r.RHICmdFlushRenderThreadTasks. If either r.RHICmdFlushRenderThreadTasks or r.RHICmdFlushRenderThreadTasksShadowPass is > 0 we will flush."));

DECLARE_CYCLE_STAT(TEXT("Shadow"), STAT_CLP_Shadow, STATGROUP_ParallelCommandListMarkers);

class FShadowParallelCommandListSet final : public FParallelCommandListSet
{
public:
	FShadowParallelCommandListSet(
		FRHICommandListImmediate& InParentCmdList,
		const FViewInfo& InView,
		const FProjectedShadowInfo& InProjectedShadowInfo,
		const FParallelCommandListBindings& InBindings)
		: FParallelCommandListSet(GET_STATID(STAT_CLP_Shadow), InView, InParentCmdList)
		, ProjectedShadowInfo(InProjectedShadowInfo)
		, Bindings(InBindings)
	{}

	~FShadowParallelCommandListSet() override
	{
		Dispatch();
	}

	void SetStateOnCommandList(FRHICommandList& RHICmdList) override
	{
		FParallelCommandListSet::SetStateOnCommandList(RHICmdList);
		Bindings.SetOnCommandList(RHICmdList);
		ProjectedShadowInfo.SetStateForView(RHICmdList);
	}

private:
	const FProjectedShadowInfo& ProjectedShadowInfo;
	FParallelCommandListBindings Bindings;
};

class FCopyShadowMapsCubeGS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCopyShadowMapsCubeGS);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return RHISupportsGeometryShaders(Parameters.Platform) && IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}

	FCopyShadowMapsCubeGS() = default;
	FCopyShadowMapsCubeGS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{}
};

IMPLEMENT_GLOBAL_SHADER(FCopyShadowMapsCubeGS, "/Engine/Private/CopyShadowMaps.usf", "CopyCubeDepthGS", SF_Geometry);

class FCopyShadowMapsCubePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCopyShadowMapsCubePS);
	SHADER_USE_PARAMETER_STRUCT(FCopyShadowMapsCubePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(TextureCube, ShadowDepthCubeTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ShadowDepthSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCopyShadowMapsCubePS, "/Engine/Private/CopyShadowMaps.usf", "CopyCubeDepthPS", SF_Pixel);

class FCopyShadowMaps2DPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCopyShadowMaps2DPS);
	SHADER_USE_PARAMETER_STRUCT(FCopyShadowMaps2DPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ShadowDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ShadowDepthSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FCopyShadowMaps2DPS, "/Engine/Private/CopyShadowMaps.usf", "Copy2DDepthPS", SF_Pixel);

/** */
class FScrollingShadowMaps2DPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FScrollingShadowMaps2DPS);
	SHADER_USE_PARAMETER_STRUCT(FScrollingShadowMaps2DPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ShadowDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, ShadowDepthSampler)
		SHADER_PARAMETER(FVector4f, DepthOffsetScale)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return true;
	}
};

IMPLEMENT_GLOBAL_SHADER(FScrollingShadowMaps2DPS, "/Engine/Private/CopyShadowMaps.usf", "Scrolling2DDepthPS", SF_Pixel);

void FProjectedShadowInfo::CopyCachedShadowMap(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FSceneRenderer* SceneRenderer,
	const FRenderTargetBindingSlots& RenderTargetBindingSlots,
	const FMeshPassProcessorRenderState& DrawRenderState)
{
	check(CacheMode == SDCM_MovablePrimitivesOnly || CacheMode == SDCM_CSMScrolling);
	const FCachedShadowMapData& CachedShadowMapData = SceneRenderer->Scene->GetCachedShadowMapDataRef(GetLightSceneInfo().Id, FMath::Max(CascadeSettings.ShadowSplitIndex, 0));

	if (CachedShadowMapData.bCachedShadowMapHasPrimitives && CachedShadowMapData.ShadowMap.IsValid())
	{
		FRDGTextureRef ShadowDepthTexture = GraphBuilder.RegisterExternalTexture(CachedShadowMapData.ShadowMap.DepthTarget);
		const FIntPoint ShadowDepthExtent = ShadowDepthTexture->Desc.Extent;

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		DrawRenderState.ApplyToPSO(GraphicsPSOInit);
		const uint32 StencilRef = DrawRenderState.GetStencilRef();

		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		// No depth tests, so we can replace the clear
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_Always>::GetRHI();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		extern TGlobalResource<FFilterVertexDeclaration> GFilterVertexDeclaration;
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;

		if (CacheMode == SDCM_MovablePrimitivesOnly)
		{
			if (bOnePassPointLightShadow)
			{
				TShaderRef<FScreenVS> ScreenVertexShader;
				TShaderMapRef<FCopyShadowMapsCubePS> PixelShader(View.ShaderMap);
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

				int32 InstanceCount = 1;

	#if PLATFORM_SUPPORTS_GEOMETRY_SHADERS
				if (RHISupportsGeometryShaders(GShaderPlatformForFeatureLevel[SceneRenderer->FeatureLevel]))
				{
					TShaderMapRef<TScreenVSForGS<false>> VertexShader(View.ShaderMap);
					TShaderMapRef<FCopyShadowMapsCubeGS> GeometryShader(View.ShaderMap);
					GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
					GraphicsPSOInit.BoundShaderState.SetGeometryShader(GeometryShader.GetGeometryShader());
					ScreenVertexShader = VertexShader;
				}
				else
	#endif
				{
					check(RHISupportsVertexShaderLayer(GShaderPlatformForFeatureLevel[SceneRenderer->FeatureLevel]));
					TShaderMapRef<TScreenVSForGS<true>> VertexShader(View.ShaderMap);
					GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
					ScreenVertexShader = VertexShader;

					InstanceCount = 6;
				}

				auto* PassParameters = GraphBuilder.AllocParameters<FCopyShadowMapsCubePS::FParameters>();
				PassParameters->RenderTargets = RenderTargetBindingSlots;
				PassParameters->ShadowDepthCubeTexture = ShadowDepthTexture;
				PassParameters->ShadowDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("CopyCachedShadowMap"),
					PassParameters,
					ERDGPassFlags::Raster,
					[this, ScreenVertexShader, PixelShader, GraphicsPSOInit, PassParameters, ShadowDepthExtent, InstanceCount, StencilRef](FRHICommandList& RHICmdList) mutable
				{
					SetStateForView(RHICmdList);
					RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
					SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, StencilRef);
					SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

					FIntPoint ResolutionWithBorder = FIntPoint(ResolutionX + 2 * BorderSize, ResolutionY + 2 * BorderSize);

					DrawRectangle(
						RHICmdList,
						0, 0,
						ResolutionWithBorder.X, ResolutionWithBorder.Y,
						0, 0,
						ResolutionWithBorder.X, ResolutionWithBorder.Y,
						ResolutionWithBorder,
						ShadowDepthExtent,
						ScreenVertexShader,
						EDRF_Default,
						InstanceCount);
				});
			}
			else
			{
				TShaderMapRef<FScreenVS> ScreenVertexShader(View.ShaderMap);
				TShaderMapRef<FCopyShadowMaps2DPS> PixelShader(View.ShaderMap);
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ScreenVertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

				auto* PassParameters = GraphBuilder.AllocParameters<FCopyShadowMaps2DPS::FParameters>();
				PassParameters->RenderTargets = RenderTargetBindingSlots;
				PassParameters->ShadowDepthTexture = ShadowDepthTexture;
				PassParameters->ShadowDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("CopyCachedShadowMap"),
					PassParameters,
					ERDGPassFlags::Raster,
					[this, ScreenVertexShader, PixelShader, GraphicsPSOInit, PassParameters, ShadowDepthExtent, StencilRef](FRHICommandList& RHICmdList) mutable
				{
					SetStateForView(RHICmdList);
					RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
					SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, StencilRef);
					SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

					FIntPoint ResolutionWithBorder = FIntPoint(ResolutionX + 2 * BorderSize, ResolutionY + 2 * BorderSize);

					DrawRectangle(
						RHICmdList,
						0, 0,
						ResolutionWithBorder.X, ResolutionWithBorder.Y,
						0, 0,
						ResolutionWithBorder.X, ResolutionWithBorder.Y,
						ResolutionWithBorder,
						ShadowDepthExtent,
						ScreenVertexShader,
						EDRF_Default);
				});
			}
		}
		else // CacheMode == SDCM_CSMScrolling
		{
			TShaderMapRef<FScreenVS> ScreenVertexShader(View.ShaderMap);
			TShaderMapRef<FScrollingShadowMaps2DPS> PixelShader(View.ShaderMap);
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = ScreenVertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

			auto* PassParameters = GraphBuilder.AllocParameters<FScrollingShadowMaps2DPS::FParameters>();
			PassParameters->RenderTargets = RenderTargetBindingSlots;
			PassParameters->ShadowDepthTexture = ShadowDepthTexture;
			PassParameters->ShadowDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			/** According to the implementation in ShadowDepthVertexShader.usf, the formula is DeviceZ = 1 - ((MaxZ - SceneDepth) / (MaxZ - MinZ)) * InvMaxSubjectDepth + SlopeDepthBias * Slope + ConstantDepthBias;
			For short C0 = InvMaxSubjectDepth; C1 = 1 + SlopeDepthBias * Slope + ConstantDepthBias;
			So the SceneDepth0 = MaxZ0 - (C1 - DeviceZ0) * (MaxZ0 - MinZ0) / C0 ;
			SceneDepth1 = SceneDepth0 + ZOffset;
			The reconstruct DeviceZ1 = C1 - ((MaxZ1 - SceneDepth1) / (MaxZ1 - MinZ1)) * C0;
			So DeviceZ1 = DeviceZ0 * (MaxZ0 - MinZ0) / (MaxZ1 - MinZ1) + (C0 * (MaxZ0 + ZOffset - MaxZ1) - C1 * (MaxZ0 - MinZ0)) / (MaxZ1 - MinZ1) + C1;
			*/
			float MaxZ0MinusMinZ0 = CachedShadowMapData.MaxSubjectZ - CachedShadowMapData.MinSubjectZ;
			float MaxZ1MinusMinZ1 = MaxSubjectZ - MinSubjectZ;
			float MaxZ0PlusZOffsetMinusMaxZ1 = CachedShadowMapData.MaxSubjectZ + CSMScrollingZOffset - MaxSubjectZ;
			float C1 = 1 + GetShaderDepthBias();
			PassParameters->DepthOffsetScale = FVector4f((InvMaxSubjectDepth * MaxZ0PlusZOffsetMinusMaxZ1 - C1 * MaxZ0MinusMinZ0) / MaxZ1MinusMinZ1 + C1, MaxZ0MinusMinZ0 / MaxZ1MinusMinZ1, 0.0f, 0.0f);

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("ScrollingCachedWholeSceneDirectionalShadowMap"),
				PassParameters,
				ERDGPassFlags::Raster,
				[this, ScreenVertexShader, PixelShader, GraphicsPSOInit, PassParameters, ShadowDepthExtent, StencilRef](FRHICommandList& RHICmdList) mutable
			{
				checkSlow(OverlappedUVOnCachedShadowMap != FVector4f(-1.0f, -1.0f, -1.0f, -1.0f));
				checkSlow(OverlappedUVOnCurrentShadowMap != FVector4f(-1.0f, -1.0f, -1.0f, -1.0f));

				FIntPoint ResolutionWithBorder = FIntPoint(ResolutionX + 2 * BorderSize, ResolutionY + 2 * BorderSize);

				uint32 UStart = OverlappedUVOnCachedShadowMap.X * ResolutionWithBorder.X + 0.5f;
				uint32 USize = (OverlappedUVOnCachedShadowMap.Z - OverlappedUVOnCachedShadowMap.X) * ResolutionWithBorder.X + 0.5f;

				uint32 VStart = OverlappedUVOnCachedShadowMap.Y * ResolutionWithBorder.Y + 0.5f;
				uint32 VSize = (OverlappedUVOnCachedShadowMap.W - OverlappedUVOnCachedShadowMap.Y) * ResolutionWithBorder.Y + 0.5f;

				FIntVector4 OutputViewport = FIntVector4(OverlappedUVOnCurrentShadowMap.X * ResolutionWithBorder.X + 0.5f, OverlappedUVOnCurrentShadowMap.Y * ResolutionWithBorder.Y + 0.5f, OverlappedUVOnCurrentShadowMap.Z * ResolutionWithBorder.X + 0.5f, OverlappedUVOnCurrentShadowMap.W * ResolutionWithBorder.Y + 0.5f);

				SetStateForView(RHICmdList);
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, StencilRef);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);

				DrawRectangle(
					RHICmdList,
					OutputViewport.X, OutputViewport.Y,
					OutputViewport.Z - OutputViewport.X, OutputViewport.W - OutputViewport.Y,
					UStart, VStart,
					USize, VSize,
					ResolutionWithBorder,
					ShadowDepthExtent,
					ScreenVertexShader,
					EDRF_Default);
			});
		}
	}
}

void FProjectedShadowInfo::BeginRenderView(FRDGBuilder& GraphBuilder, FScene* Scene)
{
	if (DependentView)
	{
		const ERHIFeatureLevel::Type FeatureLevel = ShadowDepthView->FeatureLevel;
		if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Deferred)
		{
			extern TSet<IPersistentViewUniformBufferExtension*> PersistentViewUniformBufferExtensions;

			for (IPersistentViewUniformBufferExtension* Extension : PersistentViewUniformBufferExtensions)
			{
				Extension->BeginRenderView(DependentView);
			}
		}
	}
}

static bool IsShadowDepthPassWaitForTasksEnabled()
{
	return CVarRHICmdFlushRenderThreadTasksShadowPass.GetValueOnRenderThread() > 0 || CVarRHICmdFlushRenderThreadTasks.GetValueOnRenderThread() > 0;
}

BEGIN_SHADER_PARAMETER_STRUCT(FShadowDepthPassParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FMobileShadowDepthPassUniformParameters, MobilePassUniformBuffer)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FShadowDepthPassUniformParameters, DeferredPassUniformBuffer)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualShadowMapUniformParameters, VirtualShadowMap)
	SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

#if WITH_MGPU
void CopyCachedShadowMapCrossGPU(FRHICommandList& RHICmdList, FRHITexture* ShadowDepthTexture, FRHIGPUMask SourceGPUMask)
{
	if (SourceGPUMask != FRHIGPUMask::All())
	{
		uint32 SourceGPUIndex = SourceGPUMask.GetFirstIndex();

		TArray<FTransferResourceParams, TFixedAllocator<MAX_NUM_GPUS>> CrossGPUTransferBuffers;
		for (uint32 DestGPUIndex : FRHIGPUMask::All())
		{
			if (!SourceGPUMask.Contains(DestGPUIndex))
			{
				CrossGPUTransferBuffers.Add(FTransferResourceParams(ShadowDepthTexture, SourceGPUIndex, DestGPUIndex, false, false));
			}
		}

		RHICmdList.TransferResources(CrossGPUTransferBuffers);
	}
}
#endif  // WITH_MGPU

void FProjectedShadowInfo::RenderDepth(
	FRDGBuilder& GraphBuilder,
	const FSceneRenderer* SceneRenderer,
	FRDGTextureRef ShadowDepthTexture,
	bool bDoParallelDispatch,
	bool bDoCrossGPUCopy)
{
#if WANTS_DRAW_MESH_EVENTS
	FString EventName;

	if (GetEmitDrawEvents())
	{
		GetShadowTypeNameForDrawEvent(EventName);
		EventName += FString(TEXT(" ")) + FString::FromInt(ResolutionX) + TEXT("x") + FString::FromInt(ResolutionY);
	}

	RDG_EVENT_SCOPE(GraphBuilder, "%s", *EventName);
#endif

	CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_RenderWholeSceneShadowDepthsTime, bWholeSceneShadow);
	CONDITIONAL_SCOPE_CYCLE_COUNTER(STAT_RenderPerObjectShadowDepthsTime, !bWholeSceneShadow);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_RenderShadowDepth);

	FScene* Scene = SceneRenderer->Scene;
	const ERHIFeatureLevel::Type FeatureLevel = ShadowDepthView->FeatureLevel;
	BeginRenderView(GraphBuilder, Scene);

	FShadowDepthPassParameters* PassParameters = GraphBuilder.AllocParameters<FShadowDepthPassParameters>();
	PassParameters->View = ShadowDepthView->ViewUniformBuffer;
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		ShadowDepthTexture,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ENoAction,
		FExclusiveDepthStencil::DepthWrite_StencilNop);

	if (CacheMode == SDCM_MovablePrimitivesOnly || CacheMode == SDCM_CSMScrolling)
	{
		// Copy in depths of static primitives before we render movable primitives.
		FMeshPassProcessorRenderState DrawRenderState;
		SetStateForShadowDepth(bOnePassPointLightShadow, bDirectionalLight, DrawRenderState, MeshPassTargetType);
		CopyCachedShadowMap(GraphBuilder, *ShadowDepthView, SceneRenderer, PassParameters->RenderTargets, DrawRenderState);
	}

	PassParameters->VirtualShadowMap = SceneRenderer->VirtualShadowMapArray.GetUniformBuffer(GraphBuilder);

	switch (FSceneInterface::GetShadingPath(FeatureLevel))
	{
	case EShadingPath::Deferred:
	{
		auto* ShadowDepthPassParameters = GraphBuilder.AllocParameters<FShadowDepthPassUniformParameters>();
		SetupShadowDepthPassUniformBuffer(this, GraphBuilder, *ShadowDepthView, *ShadowDepthPassParameters);
		PassParameters->DeferredPassUniformBuffer = GraphBuilder.CreateUniformBuffer(ShadowDepthPassParameters);
	}
	break;
	case EShadingPath::Mobile:
	{
		auto* ShadowDepthPassParameters = GraphBuilder.AllocParameters<FMobileShadowDepthPassUniformParameters>();
		SetupShadowDepthPassUniformBuffer(this, GraphBuilder, *ShadowDepthView, *ShadowDepthPassParameters);
		PassParameters->MobilePassUniformBuffer = GraphBuilder.CreateUniformBuffer(ShadowDepthPassParameters);
	}
	break;
	default:
		checkNoEntry();
	}

	ShadowDepthPass.BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);

#if WITH_MGPU
	// Need to fetch GPU mask outside "AddPass", as it's not updated during pass execution
	FRHIGPUMask GPUMask = GraphBuilder.RHICmdList.GetGPUMask();
#endif

	if (bDoParallelDispatch)
	{
		RDG_WAIT_FOR_TASKS_CONDITIONAL(GraphBuilder, IsShadowDepthPassWaitForTasksEnabled());

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ShadowDepthPassParallel"),
			PassParameters,
			ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass,
			[this, PassParameters
#if WITH_MGPU
			, ShadowDepthTexture, GPUMask, bDoCrossGPUCopy
#endif
			](FRHICommandListImmediate& RHICmdList)
		{
			FShadowParallelCommandListSet ParallelCommandListSet(RHICmdList, *ShadowDepthView, *this, FParallelCommandListBindings(PassParameters));
			ShadowDepthPass.DispatchDraw(&ParallelCommandListSet, RHICmdList, &PassParameters->InstanceCullingDrawParams);

#if WITH_MGPU
			if (bDoCrossGPUCopy)
			{
				CopyCachedShadowMapCrossGPU(RHICmdList, ShadowDepthTexture->GetRHI(), GPUMask);
			}
#endif
		});
	}
	else
	{
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("ShadowDepthPass"),
			PassParameters,
			ERDGPassFlags::Raster,
			[this, PassParameters
#if WITH_MGPU
			, ShadowDepthTexture, GPUMask, bDoCrossGPUCopy
#endif
			](FRHICommandList& RHICmdList)
		{
			SetStateForView(RHICmdList);
			ShadowDepthPass.DispatchDraw(nullptr, RHICmdList, &PassParameters->InstanceCullingDrawParams);

#if WITH_MGPU
			if (bDoCrossGPUCopy)
			{
				CopyCachedShadowMapCrossGPU(RHICmdList, ShadowDepthTexture->GetRHI(), GPUMask);
			}
#endif
		});
	}
}

void FProjectedShadowInfo::ModifyViewForShadow(FViewInfo* FoundView) const
{
	FIntRect OriginalViewRect = FoundView->ViewRect;
	FoundView->ViewRect = GetOuterViewRect();

	FoundView->ViewMatrices.HackRemoveTemporalAAProjectionJitter();

	if (CascadeSettings.bFarShadowCascade)
	{
		(int32&)FoundView->DrawDynamicFlags |= (int32)EDrawDynamicFlags::FarShadowCascade;
	}

	// Don't do material texture mip biasing in shadow maps.
	FoundView->MaterialTextureMipBias = 0;

	FoundView->CachedViewUniformShaderParameters = MakeUnique<FViewUniformShaderParameters>();

	// Override the view matrix so that billboarding primitives will be aligned to the light
	FoundView->ViewMatrices.HackOverrideViewMatrixForShadows(TranslatedWorldToView);
	FBox VolumeBounds[TVC_MAX];
	FoundView->SetupUniformBufferParameters(
		VolumeBounds,
		TVC_MAX,
		*FoundView->CachedViewUniformShaderParameters);

	FoundView->ViewUniformBuffer = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(*FoundView->CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);

	// we are going to set this back now because we only want the correct view rect for the uniform buffer. For LOD calculations, we want the rendering viewrect and proj matrix.
	FoundView->ViewRect = OriginalViewRect;

	extern int32 GPreshadowsForceLowestLOD;

	if (bPreShadow && GPreshadowsForceLowestLOD)
	{
		(int32&)FoundView->DrawDynamicFlags |= EDrawDynamicFlags::ForceLowestLOD;
	}
}

FViewInfo* FProjectedShadowInfo::FindViewForShadow(FSceneRenderer* SceneRenderer) const
{
	// Choose an arbitrary view where this shadow's subject is relevant.
	FViewInfo* FoundView = NULL;
	for (int32 ViewIndex = 0; ViewIndex < SceneRenderer->Views.Num(); ViewIndex++)
	{
		FViewInfo* CheckView = &SceneRenderer->Views[ViewIndex];
		const FVisibleLightViewInfo& VisibleLightViewInfo = CheckView->VisibleLightInfos[LightSceneInfo->Id];
		FPrimitiveViewRelevance ViewRel = VisibleLightViewInfo.ProjectedShadowViewRelevanceMap[ShadowId];
		if (ViewRel.bShadowRelevance)
		{
			FoundView = CheckView;
			break;
		}
	}
	check(FoundView);
	return FoundView;
}

void FProjectedShadowInfo::SetupShadowDepthView(FSceneRenderer* SceneRenderer)
{
	FViewInfo* FoundView = FindViewForShadow(SceneRenderer);
	check(FoundView && IsInRenderingThread());
	FViewInfo* DepthPassView = FoundView->CreateSnapshot();
	// We are starting a new collection of dynamic primitives for the shadow views.
	DepthPassView->DynamicPrimitiveCollector = FGPUScenePrimitiveCollector(&SceneRenderer->GetGPUSceneDynamicContext());

	ModifyViewForShadow(DepthPassView);
	ShadowDepthView = DepthPassView;
}

void FProjectedShadowInfo::GetShadowTypeNameForDrawEvent(FString& TypeName) const
{
	const FName ParentName = ParentSceneInfo ? ParentSceneInfo->Proxy->GetOwnerName() : NAME_None;

	if (bWholeSceneShadow)
	{
		if (CascadeSettings.ShadowSplitIndex >= 0)
		{
			TypeName = FString(TEXT("WholeScene split")) + FString::FromInt(CascadeSettings.ShadowSplitIndex);
		}
		else
		{
			if (CacheMode == SDCM_MovablePrimitivesOnly)
			{
				TypeName = FString(TEXT("WholeScene MovablePrimitives"));
			}
			else if (CacheMode == SDCM_StaticPrimitivesOnly)
			{
				TypeName = FString(TEXT("WholeScene StaticPrimitives"));
			}
			else
			{
				TypeName = FString(TEXT("WholeScene"));
			}
		}
	}
	else if (bPreShadow)
	{
		TypeName = FString(TEXT("PreShadow ")) + ParentName.ToString();
	}
	else
	{
		TypeName = FString(TEXT("PerObject ")) + ParentName.ToString();
	}
}

#if WITH_MGPU
// Shadows that are cached need to be copied to other GPUs after they render
bool FSceneRenderer::IsShadowCached(FProjectedShadowInfo* ProjectedShadowInfo) const
{
	// Preshadows that are going to be cached this frame should be copied to other GPUs.
	if (ProjectedShadowInfo->bPreShadow)
	{
		return !ProjectedShadowInfo->bDepthsCached && ProjectedShadowInfo->bAllocatedInPreshadowCache;
	}
	// SDCM_StaticPrimitivesOnly shadows don't update every frame so we need to copy
	// their depths to all possible GPUs.
	else if (!ProjectedShadowInfo->IsWholeSceneDirectionalShadow() && ProjectedShadowInfo->CacheMode == SDCM_StaticPrimitivesOnly)
	{
		// Cached whole scene shadows shouldn't be view dependent.
		checkSlow(ProjectedShadowInfo->DependentView == nullptr);

		return true;
	}
	return false;
}

FRHIGPUMask FSceneRenderer::GetGPUMaskForShadow(FProjectedShadowInfo* ProjectedShadowInfo) const
{
	// View dependent shadows only need to render depths on their view's GPUs.
	if (ProjectedShadowInfo->DependentView != nullptr)
	{
		return ProjectedShadowInfo->DependentView->GPUMask;
	}
	else
	{
		return AllViewsGPUMask;
	}
}
#endif // WITH_MGPU

static void UpdatePackedViewParamsFromPrevShadowState(Nanite::FPackedViewParams& Params, const FPersistentShadowState* PrevShadowState)
{
	if(PrevShadowState)
	{
		Params.PrevViewMatrices = PrevShadowState->ViewMatrices;
		Params.HZBTestViewRect = PrevShadowState->HZBTestViewRect;
		Params.Flags |= NANITE_VIEW_FLAG_HZBTEST;
	}
}

static void UpdateCurrentFrameHZB(FLightSceneInfo& LightSceneInfo, const FPersistentShadowStateKey& ShadowKey, const FProjectedShadowInfo* ProjectedShadowInfo, const TRefCountPtr<IPooledRenderTarget>& HZB, int32 CubeFaceIndex = -1)
{
	FPersistentShadowState State;
	State.ViewMatrices = ProjectedShadowInfo->GetShadowDepthRenderingViewMatrices(CubeFaceIndex);
	State.HZBTestViewRect = ProjectedShadowInfo->GetInnerViewRect();
	State.HZB = HZB;
	LightSceneInfo.PersistentShadows.Add(ShadowKey, State);
}

static void RenderShadowDepthAtlasNanite(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	FScene& Scene,
	const FViewInfo& SceneView,
	const FSortedShadowMapAtlas& ShadowMapAtlas,
	const int32 AtlasIndex)
{
	const FIntPoint AtlasSize = ShadowMapAtlas.RenderTargets.DepthTarget->GetDesc().Extent;

	const bool bUseHZB = (CVarNaniteShadowsUseHZB.GetValueOnRenderThread() != 0);
	TArray<TRefCountPtr<IPooledRenderTarget>>&	PrevAtlasHZBs = Scene.PrevAtlasHZBs;

	bool bWantsNearClip = false;
	bool bWantsNoNearClip = false;
	TArray<Nanite::FPackedView, SceneRenderingAllocator> PackedViews;
	TArray<Nanite::FPackedView, SceneRenderingAllocator> PackedViewsNoNearClip;
	TArray<FProjectedShadowInfo*, SceneRenderingAllocator> ShadowsToEmit;
	for (int32 ShadowIndex = 0; ShadowIndex < ShadowMapAtlas.Shadows.Num(); ShadowIndex++)
	{
		FProjectedShadowInfo* ProjectedShadowInfo = ShadowMapAtlas.Shadows[ShadowIndex];

		// TODO: We avoid rendering Nanite geometry into both movable AND static cached shadows, but has a side effect
		// that if there is *only* a movable cached shadow map (and not static), it won't rendering anything.
		// Logic around Nanite and the cached shadows is fuzzy in a bunch of places and the whole thing needs some rethinking
		// so leaving this like this for now as it is unlikely to happen in realistic scenes.
		if (!ProjectedShadowInfo->bNaniteGeometry ||
			ProjectedShadowInfo->CacheMode == SDCM_MovablePrimitivesOnly)
		{
			continue;
		}

		Nanite::FPackedViewParams Initializer;
		Initializer.ViewMatrices = ProjectedShadowInfo->GetShadowDepthRenderingViewMatrices();
		Initializer.ViewRect = ProjectedShadowInfo->GetOuterViewRect();
		Initializer.RasterContextSize = AtlasSize;
		Initializer.LODScaleFactor = ComputeNaniteShadowsLODScaleFactor();
		Initializer.PrevViewMatrices = Initializer.ViewMatrices;
		Initializer.HZBTestViewRect = ProjectedShadowInfo->GetInnerViewRect();
		Initializer.Flags = 0;

		FLightSceneInfo& LightSceneInfo = ProjectedShadowInfo->GetLightSceneInfo();
		
		FPersistentShadowStateKey ShadowKey;
		ShadowKey.AtlasIndex = AtlasIndex;
		ShadowKey.ProjectionId = ProjectedShadowInfo->ProjectionIndex;
		ShadowKey.SubjectPrimitiveComponentIndex = ProjectedShadowInfo->SubjectPrimitiveComponentIndex;

		FPersistentShadowState* PrevShadowState = LightSceneInfo.PrevPersistentShadows.Find(ShadowKey);

		UpdatePackedViewParamsFromPrevShadowState(Initializer, PrevShadowState);
		UpdateCurrentFrameHZB(LightSceneInfo, ShadowKey, ProjectedShadowInfo, nullptr);

		// Orthographic shadow projections want depth clamping rather than clipping
		if (ProjectedShadowInfo->ShouldClampToNearPlane())
		{
			PackedViewsNoNearClip.Add(Nanite::CreatePackedView(Initializer));
		}
		else
		{
			PackedViews.Add(Nanite::CreatePackedView(Initializer));
		}

		ShadowsToEmit.Add(ProjectedShadowInfo);
	}

	if (PackedViews.Num() > 0 || PackedViewsNoNearClip.Num() > 0)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Nanite Shadows");

		Nanite::FSharedContext SharedContext{};
		SharedContext.FeatureLevel = Scene.GetFeatureLevel();
		SharedContext.ShaderMap = GetGlobalShaderMap(SharedContext.FeatureLevel);
		SharedContext.Pipeline = Nanite::EPipeline::Shadows;

		// NOTE: Rendering into an atlas like this is not going to work properly with HZB, but we are not currently using HZB here.
		// It might be worthwhile going through the virtual SM rendering path even for "dense" cases even just for proper handling of all the details.
		FIntRect FullAtlasViewRect(FIntPoint(0, 0), AtlasSize);
		TRefCountPtr<IPooledRenderTarget> PrevAtlasHZB = bUseHZB ? PrevAtlasHZBs[AtlasIndex] : nullptr;

		Nanite::FCullingContext::FConfiguration CullingConfig = { 0 };
		CullingConfig.bTwoPassOcclusion			= true;
		CullingConfig.bSupportsMultiplePasses	= (PackedViews.Num() > 0 && PackedViewsNoNearClip.Num() > 0); // Need separate passes for near clip on/off currently
		CullingConfig.bUpdateStreaming			= CVarNaniteShadowsUpdateStreaming.GetValueOnRenderThread() != 0;
		CullingConfig.SetViewFlags(SceneView);

		Nanite::FCullingContext CullingContext = Nanite::InitCullingContext(GraphBuilder, SharedContext, Scene, PrevAtlasHZB, FullAtlasViewRect, CullingConfig);
		Nanite::FRasterContext RasterContext = Nanite::InitRasterContext(GraphBuilder, SharedContext, AtlasSize, false, Nanite::EOutputBufferMode::DepthOnly);

		bool bExtractStats = false;		
		if (GNaniteShowStats != 0)
		{
			FString AtlasFilterName = FString::Printf(TEXT("ShadowAtlas%d"), AtlasIndex);
			bExtractStats = Nanite::IsStatFilterActive(AtlasFilterName);
		}

		if (PackedViews.Num() > 0)
		{
			Nanite::FRasterState RasterState;
			RasterState.bNearClip = true;

			Nanite::CullRasterize(
				GraphBuilder,
				Scene,
				SceneView,
				PackedViews,
				SharedContext,
				CullingContext,
				RasterContext,
				RasterState,
				nullptr,	// InstanceDraws
				bExtractStats
			);
		}

		if (PackedViewsNoNearClip.Num() > 0)
		{
			Nanite::FRasterState RasterState;
			RasterState.bNearClip = false;

			Nanite::CullRasterize(
				GraphBuilder,
				Scene,
				SceneView,
				PackedViewsNoNearClip,
				SharedContext,
				CullingContext,
				RasterContext,
				RasterState,
				nullptr,	// InstanceDraws
				bExtractStats
			);
		}

		if (bUseHZB)
		{
			FRDGTextureRef FurthestHZBTexture;
			BuildHZBFurthest(
				GraphBuilder,
				GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy),
				RasterContext.DepthBuffer,
				FullAtlasViewRect,
				FeatureLevel,
				Scene.GetShaderPlatform(),
				TEXT("Shadow.AtlasHZB"),
				/* OutFurthestHZBTexture = */ &FurthestHZBTexture,
				PF_R32_FLOAT);
			PrevAtlasHZBs[AtlasIndex] = GraphBuilder.ConvertToExternalTexture(FurthestHZBTexture);
		}
		else
		{
			PrevAtlasHZBs[AtlasIndex] = nullptr;
		}

		FRDGTextureRef ShadowMap = GraphBuilder.RegisterExternalTexture(ShadowMapAtlas.RenderTargets.DepthTarget);

		for (FProjectedShadowInfo* ProjectedShadowInfo : ShadowsToEmit)
		{
			const FIntRect AtlasViewRect = ProjectedShadowInfo->GetOuterViewRect();

			Nanite::EmitShadowMap(
				GraphBuilder,
				SharedContext,
				RasterContext,
				ShadowMap,
				AtlasViewRect,
				AtlasViewRect.Min,
				ProjectedShadowInfo->GetShadowDepthRenderingViewMatrices().GetProjectionMatrix(),
				ProjectedShadowInfo->GetShaderDepthBias(),
				ProjectedShadowInfo->bDirectionalLight
			);
		}
	}
}

bool IsParallelDispatchEnabled(const FProjectedShadowInfo* ProjectedShadowInfo, EShaderPlatform ShaderPlatform)
{
	return GRHICommandList.UseParallelAlgorithms() && CVarParallelShadows.GetValueOnRenderThread()
		&& (ProjectedShadowInfo->IsWholeSceneDirectionalShadow() || CVarParallelShadowsNonWholeScene.GetValueOnRenderThread())
		// Parallel dispatch is not supported on mobile platform
		&& !IsMobilePlatform(ShaderPlatform);
}

void FSceneRenderer::RenderShadowDepthMapAtlases(FRDGBuilder& GraphBuilder)
{
	const bool bNaniteEnabled = 
		UseNanite(ShaderPlatform) &&
		ViewFamily.EngineShowFlags.NaniteMeshes &&
		CVarNaniteShadows.GetValueOnRenderThread() != 0 &&
		Nanite::GStreamingManager.HasResourceEntries();

	Scene->PrevAtlasHZBs.SetNum(SortedShadowsForShadowDepthPass.ShadowMapAtlases.Num());

	FRDGResourceAccessFinalizer ResourceAccessFinalizer;

	for (int32 AtlasIndex = 0; AtlasIndex < SortedShadowsForShadowDepthPass.ShadowMapAtlases.Num(); AtlasIndex++)
	{
		FSortedShadowMapAtlas& ShadowMapAtlas = SortedShadowsForShadowDepthPass.ShadowMapAtlases[AtlasIndex];
		FRDGTextureRef AtlasDepthTexture = GraphBuilder.RegisterExternalTexture(ShadowMapAtlas.RenderTargets.DepthTarget);
		const FIntPoint AtlasSize = AtlasDepthTexture->Desc.Extent;

		RDG_EVENT_SCOPE(GraphBuilder, "Atlas%u %ux%u", AtlasIndex, AtlasSize.X, AtlasSize.Y);

		TArray<FProjectedShadowInfo*, SceneRenderingAllocator> ParallelShadowPasses;
		TArray<FProjectedShadowInfo*, SceneRenderingAllocator> SerialShadowPasses;

		// Gather our passes here to minimize switching render passes
		for (FProjectedShadowInfo* ProjectedShadowInfo : ShadowMapAtlas.Shadows)
		{
			if (IsParallelDispatchEnabled(ProjectedShadowInfo, ShaderPlatform))
			{
				ParallelShadowPasses.Add(ProjectedShadowInfo);
			}
			else
			{
				SerialShadowPasses.Add(ProjectedShadowInfo);
			}
		}

	#if WANTS_DRAW_MESH_EVENTS
		FLightSceneProxy* CurrentLightForDrawEvent = nullptr;
		FDrawEvent LightEvent;
	#endif

		const auto SetLightEventForShadow = [&](FProjectedShadowInfo* ProjectedShadowInfo)
		{
		#if WANTS_DRAW_MESH_EVENTS
			if (!CurrentLightForDrawEvent || ProjectedShadowInfo->GetLightSceneInfo().Proxy != CurrentLightForDrawEvent)
			{
				if (CurrentLightForDrawEvent)
				{
					GraphBuilder.EndEventScope();
				}

				CurrentLightForDrawEvent = ProjectedShadowInfo->GetLightSceneInfo().Proxy;
				FString LightNameWithLevel;
				GetLightNameForDrawEvent(CurrentLightForDrawEvent, LightNameWithLevel);
				GraphBuilder.BeginEventScope(RDG_EVENT_NAME("%s", *LightNameWithLevel));
			}
		#endif
		};

		const auto EndLightEvent = [&]()
		{
		#if WANTS_DRAW_MESH_EVENTS
			if (CurrentLightForDrawEvent)
			{
				GraphBuilder.EndEventScope();
				CurrentLightForDrawEvent = nullptr;
			}
		#endif
		};

		AddClearShadowDepthPass(GraphBuilder, AtlasDepthTexture);

		if (ParallelShadowPasses.Num() > 0)
		{
			for (FProjectedShadowInfo* ProjectedShadowInfo : ParallelShadowPasses)
			{
				RDG_GPU_MASK_SCOPE(GraphBuilder, GetGPUMaskForShadow(ProjectedShadowInfo));
				SetLightEventForShadow(ProjectedShadowInfo);

				const bool bParallelDispatch = true;
				bool bDoCrossGPUCopy = false;
#if WITH_MGPU
				bDoCrossGPUCopy = IsShadowCached(ProjectedShadowInfo);
#endif
				ProjectedShadowInfo->RenderDepth(GraphBuilder, this, AtlasDepthTexture, bParallelDispatch, bDoCrossGPUCopy);
			}
		}

		EndLightEvent();

		if (SerialShadowPasses.Num() > 0)
		{
			for (FProjectedShadowInfo* ProjectedShadowInfo : SerialShadowPasses)
			{
				RDG_GPU_MASK_SCOPE(GraphBuilder, GetGPUMaskForShadow(ProjectedShadowInfo));
				SetLightEventForShadow(ProjectedShadowInfo);

				const bool bParallelDispatch = false;
				bool bDoCrossGPUCopy = false;
#if WITH_MGPU
				bDoCrossGPUCopy = IsShadowCached(ProjectedShadowInfo);
#endif
				ProjectedShadowInfo->RenderDepth(GraphBuilder, this, AtlasDepthTexture, bParallelDispatch, bDoCrossGPUCopy);
			}
		}

		EndLightEvent();

		if (bNaniteEnabled)
		{
			const FViewInfo& SceneView = Views[0];
			RenderShadowDepthAtlasNanite(GraphBuilder, FeatureLevel, *Scene, SceneView, ShadowMapAtlas, AtlasIndex);
		}

		// Make readable because AtlasDepthTexture is not tracked via RDG yet
		// On mobile CSM atlas sampled only in pixel shaders
		ERHIAccess AtlasDepthTextureAccessFinal = (FeatureLevel == ERHIFeatureLevel::ES3_1 ? ERHIAccess::SRVGraphics : ERHIAccess::SRVMask);
		ShadowMapAtlas.RenderTargets.DepthTarget = ConvertToFinalizedExternalTexture(GraphBuilder, ResourceAccessFinalizer, AtlasDepthTexture, AtlasDepthTextureAccessFinal);
	}

	ResourceAccessFinalizer.Finalize(GraphBuilder);
}

void FSceneRenderer::RenderVirtualShadowMaps(FRDGBuilder& GraphBuilder, bool bNaniteEnabled)
{

	if (SortedShadowsForShadowDepthPass.VirtualShadowMapShadows.Num() == 0 &&
		SortedShadowsForShadowDepthPass.VirtualShadowMapClipmaps.Num() == 0)
	{
		return;
	}

	FVirtualShadowMapArrayCacheManager *CacheManager = Scene->VirtualShadowMapArrayCacheManager;

	// TODO: Separate out the decision about nanite using HZB and stuff like HZB culling invalidations?
	const bool bVSMUseHZB = (CVarShadowsVirtualUseHZB.GetValueOnRenderThread() != 0);

	const FIntPoint VirtualShadowSize = VirtualShadowMapArray.GetPhysicalPoolSize();
	const FIntRect VirtualShadowViewRect = FIntRect(0, 0, VirtualShadowSize.X, VirtualShadowSize.Y);

	Nanite::FSharedContext SharedContext{};
	SharedContext.FeatureLevel = FeatureLevel;
	SharedContext.ShaderMap = GetGlobalShaderMap(SharedContext.FeatureLevel);
	SharedContext.Pipeline = Nanite::EPipeline::Shadows;

	if (bNaniteEnabled)
	{
		const TRefCountPtr<IPooledRenderTarget> PrevHZBPhysical = bVSMUseHZB ? CacheManager->PrevBuffers.HZBPhysical : nullptr;

		{
			RDG_EVENT_SCOPE(GraphBuilder, "RenderVirtualShadowMaps(Nanite)");

			check(VirtualShadowMapArray.PhysicalPagePoolRDG != nullptr);

			Nanite::FRasterContext RasterContext = Nanite::InitRasterContext(
				GraphBuilder,
				SharedContext,
				VirtualShadowSize,
				false,
				Nanite::EOutputBufferMode::DepthOnly,
				false,	// Clear entire texture
				nullptr, 0,
				VirtualShadowMapArray.PhysicalPagePoolRDG );

			const bool bUpdateStreaming = CVarNaniteShadowsUpdateStreaming.GetValueOnRenderThread() != 0;

			const FViewInfo& SceneView = Views[0];

			auto FilterAndRenderVirtualShadowMaps = [
				&SortedShadowsForShadowDepthPass = SortedShadowsForShadowDepthPass,
					&SharedContext,
					&RasterContext,
					&VirtualShadowMapArray = VirtualShadowMapArray,
					&GraphBuilder,
					&SceneView,
					bUpdateStreaming,
					bVSMUseHZB,
					Scene = Scene,
					CacheManager = CacheManager,
					PrevHZBPhysical
			](bool bShouldClampToNearPlane, const FString &VirtualFilterName)
			{
				TArray<Nanite::FPackedView, SceneRenderingAllocator> VirtualShadowViews;

				// Add any clipmaps first to the ortho rendering pass
				if (bShouldClampToNearPlane)
				{
					for (const TSharedPtr<FVirtualShadowMapClipmap>& Clipmap : SortedShadowsForShadowDepthPass.VirtualShadowMapClipmaps)
					{
						VirtualShadowMapArray.AddRenderViews(
							Clipmap,
							ComputeNaniteShadowsLODScaleFactor(),
							PrevHZBPhysical.IsValid(),
							bVSMUseHZB,
							VirtualShadowViews);
					}
				}

				for (FProjectedShadowInfo* ProjectedShadowInfo : SortedShadowsForShadowDepthPass.VirtualShadowMapShadows)
				{
					if (ProjectedShadowInfo->ShouldClampToNearPlane() == bShouldClampToNearPlane && ProjectedShadowInfo->HasVirtualShadowMap())
					{
						VirtualShadowMapArray.AddRenderViews(
							ProjectedShadowInfo, 
							ComputeNaniteShadowsLODScaleFactor(),
							PrevHZBPhysical.IsValid(),
							bVSMUseHZB,
							VirtualShadowViews);
					}
				}

				if (VirtualShadowViews.Num() > 0)
				{
					int32 NumPrimaryViews = VirtualShadowViews.Num();
					VirtualShadowMapArray.CreateMipViews( VirtualShadowViews );

					Nanite::FRasterState RasterState;
					if (bShouldClampToNearPlane)
					{
						RasterState.bNearClip = false;
					}

					Nanite::FCullingContext::FConfiguration CullingConfig = { 0 };
					CullingConfig.bUpdateStreaming			= CVarNaniteShadowsUpdateStreaming.GetValueOnRenderThread() != 0;
					CullingConfig.SetViewFlags(SceneView);

					Nanite::FCullingContext CullingContext = Nanite::InitCullingContext(
						GraphBuilder,
						SharedContext,
						*Scene,
						PrevHZBPhysical,
						FIntRect(),
						CullingConfig
					);

					const bool bExtractStats = Nanite::IsStatFilterActive(VirtualFilterName);

					Nanite::CullRasterize(
						GraphBuilder,
						*Scene,
						SceneView,
						VirtualShadowViews,
						NumPrimaryViews,
						SharedContext,
						CullingContext,
						RasterContext,
						RasterState,
						nullptr,
						&VirtualShadowMapArray,
						bExtractStats
					);
				}
			};

			{
				RDG_EVENT_SCOPE(GraphBuilder, "DirectionalLights");
				static FString VirtualFilterName = TEXT("VSM_Directional");
				FilterAndRenderVirtualShadowMaps(true, VirtualFilterName);
			}

			{
				RDG_EVENT_SCOPE(GraphBuilder, "LocalLights");
				static FString VirtualFilterName = TEXT("VSM_Local");
				FilterAndRenderVirtualShadowMaps(false, VirtualFilterName);
			}

			if (bVSMUseHZB)
			{
				VirtualShadowMapArray.HZBPhysical = VirtualShadowMapArray.BuildHZBFurthest(GraphBuilder);
			}
		}
	}

	if (UseNonNaniteVirtualShadowMaps(ShaderPlatform, FeatureLevel))
	{
		VirtualShadowMapArray.RenderVirtualShadowMapsNonNanite(GraphBuilder, SortedShadowsForShadowDepthPass.VirtualShadowMapShadows, *Scene, Views);
	}

	// If separate static/dynamic caching is enabled, we may need to merge some pages after rendering
	VirtualShadowMapArray.MergeStaticPhysicalPages(GraphBuilder);
}

void FSceneRenderer::RenderShadowDepthMaps(FRDGBuilder& GraphBuilder, FInstanceCullingManager &InstanceCullingManager)
{
	ensureMsgf(!bShadowDepthRenderCompleted, TEXT("RenderShadowDepthMaps called twice in the same frame"));

	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderShadows);

	TRACE_CPUPROFILER_EVENT_SCOPE(FSceneRenderer::RenderShadowDepthMaps);
	SCOPED_NAMED_EVENT(FSceneRenderer_RenderShadowDepthMaps, FColor::Emerald);

	RDG_EVENT_SCOPE(GraphBuilder, "ShadowDepths");
	RDG_GPU_STAT_SCOPE(GraphBuilder, ShadowDepths);

	// Ensure all shadow view dynamic primitives are uploaded before shadow-culling batching pass.
	// TODO: automate this such that:
	//  1. we only process views that need it (have dynamic primitives)
	//  2. it is integrated in the GPU-scene (it already collects the dynamic primives and know about them...)
	//  3. BUT: we need to touch the views to update the GPUScene buffer references in the FViewInfo
	//          so need to refactor that into its own binding point, probably. Or something.
	for (FSortedShadowMapAtlas& ShadowMapAtlas : SortedShadowsForShadowDepthPass.ShadowMapAtlases)
	{
		for (FProjectedShadowInfo* ProjectedShadowInfo : ShadowMapAtlas.Shadows)
		{
			Scene->GPUScene.UploadDynamicPrimitiveShaderDataForView(GraphBuilder, Scene, *ProjectedShadowInfo->ShadowDepthView, true);
		}
	}
	for (FSortedShadowMapAtlas& ShadowMap : SortedShadowsForShadowDepthPass.ShadowMapCubemaps)
	{
		check(ShadowMap.Shadows.Num() == 1);
		FProjectedShadowInfo* ProjectedShadowInfo = ShadowMap.Shadows[0];
		Scene->GPUScene.UploadDynamicPrimitiveShaderDataForView(GraphBuilder, Scene, *ProjectedShadowInfo->ShadowDepthView, true);
	}
	for (FProjectedShadowInfo* ProjectedShadowInfo : SortedShadowsForShadowDepthPass.PreshadowCache.Shadows)
	{
		if (!ProjectedShadowInfo->bDepthsCached)
		{
			Scene->GPUScene.UploadDynamicPrimitiveShaderDataForView(GraphBuilder, Scene, *ProjectedShadowInfo->ShadowDepthView, true);
		}
	}
	for (const FSortedShadowMapAtlas& ShadowMapAtlas : SortedShadowsForShadowDepthPass.TranslucencyShadowMapAtlases)
	{
		for (FProjectedShadowInfo* ProjectedShadowInfo : ShadowMapAtlas.Shadows)
		{
			Scene->GPUScene.UploadDynamicPrimitiveShaderDataForView(GraphBuilder, Scene, *ProjectedShadowInfo->ShadowDepthView, true);
		}
	}
	for (FProjectedShadowInfo* ProjectedShadowInfo : SortedShadowsForShadowDepthPass.VirtualShadowMapShadows)
	{
		Scene->GPUScene.UploadDynamicPrimitiveShaderDataForView(GraphBuilder, Scene, *ProjectedShadowInfo->ShadowDepthView, true);
	}

	// Begin new deferred culling bacthing scope to catch shadow render passes, as there can use dynamic primitives that have not been uploaded before 
	// the previous batching scope. Also flushes the culling views registered during the setup (in InitViewsAfterPrepass) that are referenced in the shadow view
	// culling.
	InstanceCullingManager.BeginDeferredCulling(GraphBuilder, Scene->GPUScene);

	const bool bNaniteEnabled = 
		UseNanite(ShaderPlatform) &&
		ViewFamily.EngineShowFlags.NaniteMeshes &&
		Nanite::GStreamingManager.HasResourceEntries();

	RenderVirtualShadowMaps(GraphBuilder, bNaniteEnabled);

	// Render non-VSM shadows
	RenderShadowDepthMapAtlases(GraphBuilder);

	const bool bUseGeometryShader = !GRHISupportsArrayIndexFromAnyShader;

	FRDGResourceAccessFinalizer ResourceAccessFinalizer;

	for (int32 CubemapIndex = 0; CubemapIndex < SortedShadowsForShadowDepthPass.ShadowMapCubemaps.Num(); CubemapIndex++)
	{
		FSortedShadowMapAtlas& ShadowMap = SortedShadowsForShadowDepthPass.ShadowMapCubemaps[CubemapIndex];
		FRDGTextureRef ShadowDepthTexture = GraphBuilder.RegisterExternalTexture(ShadowMap.RenderTargets.DepthTarget);
		const FIntPoint TargetSize = ShadowDepthTexture->Desc.Extent;

		check(ShadowMap.Shadows.Num() == 1);
		FProjectedShadowInfo* ProjectedShadowInfo = ShadowMap.Shadows[0];
		RDG_GPU_MASK_SCOPE(GraphBuilder, GetGPUMaskForShadow(ProjectedShadowInfo));

		FString LightNameWithLevel;
		GetLightNameForDrawEvent(ProjectedShadowInfo->GetLightSceneInfo().Proxy, LightNameWithLevel);
		RDG_EVENT_SCOPE(GraphBuilder, "Cubemap %s %u^2", *LightNameWithLevel, TargetSize.X, TargetSize.Y);

		// Only clear when we're not copying from a cached shadow map.
		if (ProjectedShadowInfo->CacheMode != SDCM_MovablePrimitivesOnly || !Scene->GetCachedShadowMapDataRef(ProjectedShadowInfo->GetLightSceneInfo().Id, FMath::Max(ProjectedShadowInfo->CascadeSettings.ShadowSplitIndex, 0)).bCachedShadowMapHasPrimitives)
		{
			AddClearShadowDepthPass(GraphBuilder, ShadowDepthTexture);
		}

		{
			const bool bDoParallelDispatch = IsParallelDispatchEnabled(ProjectedShadowInfo, ShaderPlatform);
			const bool bDoCrossGPUCopy = false;
			ProjectedShadowInfo->RenderDepth(GraphBuilder, this, ShadowDepthTexture, bDoParallelDispatch, bDoCrossGPUCopy);
		}

		if (bNaniteEnabled &&
			CVarNaniteShadows.GetValueOnRenderThread() &&
			ProjectedShadowInfo->bNaniteGeometry &&
			ProjectedShadowInfo->CacheMode != SDCM_MovablePrimitivesOnly		// See note in RenderShadowDepthMapAtlases
			)
		{
			const bool bUseHZB = (CVarNaniteShadowsUseHZB.GetValueOnRenderThread() != 0);

			FString LightName;
			GetLightNameForDrawEvent(ProjectedShadowInfo->GetLightSceneInfo().Proxy, LightName);

			{
				RDG_EVENT_SCOPE( GraphBuilder, "Nanite Cubemap %s %ux%u", *LightName, ProjectedShadowInfo->ResolutionX, ProjectedShadowInfo->ResolutionY );
				
				FRDGTextureRef RDGShadowMap = GraphBuilder.RegisterExternalTexture( ShadowMap.RenderTargets.DepthTarget, TEXT("ShadowDepthBuffer") );

				// Cubemap shadows reverse the cull mode due to the face matrices (see FShadowDepthPassMeshProcessor::AddMeshBatch)
				Nanite::FRasterState RasterState;
				RasterState.CullMode = CM_CCW;

				const bool bUpdateStreaming = CVarNaniteShadowsUpdateStreaming.GetValueOnRenderThread() != 0;

				FLightSceneInfo& LightSceneInfo = ProjectedShadowInfo->GetLightSceneInfo();

				FString CubeFilterName;
				if (GNaniteShowStats != 0)
				{
					// Get the base light filter name.
					CubeFilterName = Nanite::GetFilterNameForLight(LightSceneInfo.Proxy);
					CubeFilterName.Append(TEXT("_Face_"));
				}

				for (int32 CubemapFaceIndex = 0; CubemapFaceIndex < 6; CubemapFaceIndex++)
				{
					RDG_EVENT_SCOPE( GraphBuilder, "Face %u", CubemapFaceIndex );
					
					// We always render to a whole face at once
					const FIntRect ShadowViewRect = FIntRect(0, 0, TargetSize.X, TargetSize.Y);
					check(ProjectedShadowInfo->X == ShadowViewRect.Min.X);
					check(ProjectedShadowInfo->Y == ShadowViewRect.Min.Y);
					check(ProjectedShadowInfo->ResolutionX == ShadowViewRect.Max.X);
					check(ProjectedShadowInfo->ResolutionY == ShadowViewRect.Max.Y);
					check(ProjectedShadowInfo->BorderSize == 0);

					FPersistentShadowStateKey ShadowKey;
					ShadowKey.ProjectionId = CubemapFaceIndex;
					ShadowKey.SubjectPrimitiveComponentIndex = 0;

					FPersistentShadowState* PrevShadowState = LightSceneInfo.PrevPersistentShadows.Find(ShadowKey);

					const FViewInfo& SceneView = Views[0];

					Nanite::FSharedContext SharedContext{};
					SharedContext.FeatureLevel = Scene->GetFeatureLevel();
					SharedContext.ShaderMap = GetGlobalShaderMap(SharedContext.FeatureLevel);
					SharedContext.Pipeline = Nanite::EPipeline::Shadows;
					
					TRefCountPtr<IPooledRenderTarget> PrevHZB = (PrevShadowState && bUseHZB) ? PrevShadowState->HZB : nullptr;

					Nanite::FCullingContext::FConfiguration CullingConfig = { 0 };
					CullingConfig.bTwoPassOcclusion			= true;
					CullingConfig.bUpdateStreaming			= bUpdateStreaming;
					CullingConfig.SetViewFlags(SceneView);

					Nanite::FCullingContext CullingContext = Nanite::InitCullingContext(GraphBuilder, SharedContext, *Scene, PrevHZB, ShadowViewRect, CullingConfig);
					Nanite::FRasterContext RasterContext = Nanite::InitRasterContext(GraphBuilder, SharedContext, TargetSize, false, Nanite::EOutputBufferMode::DepthOnly);

					// Setup packed view
					TArray<Nanite::FPackedView, SceneRenderingAllocator> PackedViews;
					{
						Nanite::FPackedViewParams Params;
						Params.ViewMatrices = ProjectedShadowInfo->GetShadowDepthRenderingViewMatrices(CubemapFaceIndex);
						Params.ViewRect = ShadowViewRect;
						Params.RasterContextSize = TargetSize;
						Params.LODScaleFactor = ComputeNaniteShadowsLODScaleFactor();
						Params.PrevViewMatrices = Params.ViewMatrices;
						Params.HZBTestViewRect = ShadowViewRect;
						Params.Flags = 0;
						UpdatePackedViewParamsFromPrevShadowState(Params, PrevShadowState);

						PackedViews.Add(Nanite::CreatePackedView(Params));
					}

					FString CubeFaceFilterName;
					if (GNaniteShowStats != 0)
					{
						CubeFaceFilterName = CubeFilterName;
						CubeFaceFilterName.AppendInt(CubemapFaceIndex);
					}

					const bool bExtractStats = Nanite::IsStatFilterActive(CubeFaceFilterName);

					Nanite::CullRasterize(
						GraphBuilder,
						*Scene,
						SceneView,
						PackedViews,
						SharedContext,
						CullingContext,
						RasterContext,
						RasterState,
						nullptr,
						bExtractStats
					);

					Nanite::EmitCubemapShadow(
						GraphBuilder,
						SharedContext,
						RasterContext,
						RDGShadowMap,
						ShadowViewRect,
						CubemapFaceIndex,
						bUseGeometryShader);
										
					TRefCountPtr<IPooledRenderTarget> HZB;
					if (bUseHZB)
					{
						FRDGTextureRef FurthestHZBTexture;
						BuildHZBFurthest(
							GraphBuilder,
							GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackDummy),
							RasterContext.DepthBuffer,
							ShadowViewRect,
							FeatureLevel,
							ShaderPlatform,
							TEXT("Shadow.CubemapHZB"),
							/* OutFurthestHZBTexture = */ &FurthestHZBTexture);

						HZB = GraphBuilder.ConvertToExternalTexture(FurthestHZBTexture);
					}
					UpdateCurrentFrameHZB(LightSceneInfo, ShadowKey, ProjectedShadowInfo, HZB, CubemapFaceIndex);
				}
			}
		}

		// Make readable because ShadowDepthTexture is not tracked via RDG yet
		ShadowMap.RenderTargets.DepthTarget = ConvertToFinalizedExternalTexture(GraphBuilder, ResourceAccessFinalizer, ShadowDepthTexture);
	}

	ResourceAccessFinalizer.Finalize(GraphBuilder);

	if (SortedShadowsForShadowDepthPass.PreshadowCache.Shadows.Num() > 0)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "PreshadowCache");

		FRDGTextureRef PreshadowCacheTexture = GraphBuilder.RegisterExternalTexture(SortedShadowsForShadowDepthPass.PreshadowCache.RenderTargets.DepthTarget);

		for (FProjectedShadowInfo* ProjectedShadowInfo : SortedShadowsForShadowDepthPass.PreshadowCache.Shadows)
		{
			if (!ProjectedShadowInfo->bDepthsCached)
			{
				RDG_GPU_MASK_SCOPE(GraphBuilder, GetGPUMaskForShadow(ProjectedShadowInfo));
				AddClearShadowDepthPass(GraphBuilder, PreshadowCacheTexture, ProjectedShadowInfo);

				const bool bParallelDispatch = IsParallelDispatchEnabled(ProjectedShadowInfo, ShaderPlatform);
				const bool bDoCrossGPUCopy = true;
				ProjectedShadowInfo->RenderDepth(GraphBuilder, this, PreshadowCacheTexture, bParallelDispatch, bDoCrossGPUCopy);
				ProjectedShadowInfo->bDepthsCached = true;
			}
		}
	}

	for (int32 AtlasIndex = 0; AtlasIndex < SortedShadowsForShadowDepthPass.TranslucencyShadowMapAtlases.Num(); AtlasIndex++)
	{
		const FSortedShadowMapAtlas& ShadowMapAtlas = SortedShadowsForShadowDepthPass.TranslucencyShadowMapAtlases[AtlasIndex];

		FRDGTextureRef ColorTarget0 = GraphBuilder.RegisterExternalTexture(ShadowMapAtlas.RenderTargets.ColorTargets[0]);
		FRDGTextureRef ColorTarget1 = GraphBuilder.RegisterExternalTexture(ShadowMapAtlas.RenderTargets.ColorTargets[1]);
		const FIntPoint TargetSize  = ColorTarget0->Desc.Extent;

		FRenderTargetBindingSlots RenderTargets;
		RenderTargets[0] = FRenderTargetBinding(ColorTarget0, ERenderTargetLoadAction::ELoad);
		RenderTargets[1] = FRenderTargetBinding(ColorTarget1, ERenderTargetLoadAction::ELoad);

		RDG_EVENT_SCOPE(GraphBuilder, "TranslucencyAtlas%u %u^2", AtlasIndex, TargetSize.X, TargetSize.Y);

		for (FProjectedShadowInfo* ProjectedShadowInfo : ShadowMapAtlas.Shadows)
		{
			RDG_GPU_MASK_SCOPE(GraphBuilder, GetGPUMaskForShadow(ProjectedShadowInfo));
			ProjectedShadowInfo->RenderTranslucencyDepths(GraphBuilder, this, RenderTargets, InstanceCullingManager);
		}
	}

	// Move current persistent shadow state to previous and clear current.
	// TODO: This could be very slow.
	for (const FLightSceneInfoCompact& Light : Scene->Lights)
	{
		Light.LightSceneInfo->PrevPersistentShadows = Light.LightSceneInfo->PersistentShadows;
		Light.LightSceneInfo->PersistentShadows.Empty();
	}

	bShadowDepthRenderCompleted = true;
}

bool FShadowDepthPassMeshProcessor::Process(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	int32 StaticMeshId,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
	const FMaterial& RESTRICT MaterialResource,
	ERasterizerFillMode MeshFillMode,
	ERasterizerCullMode MeshCullMode)
{
	const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;

	TMeshProcessorShaders<
		FShadowDepthVS,
		FShadowDepthBasePS> ShadowDepthPassShaders;

	const bool bUsePositionOnlyVS =
		   VertexFactory->SupportsPositionAndNormalOnlyStream()
		&& MaterialResource.WritesEveryPixel(true)
		&& !MaterialResource.MaterialModifiesMeshPosition_RenderThread();

	// Use perspective correct shadow depths for shadow types which typically render low poly meshes into the shadow depth buffer.
	// Depth will be interpolated to the pixel shader and written out, which disables HiZ and double speed Z.
	// Directional light shadows use an ortho projection and can use the non-perspective correct path without artifacts.
	// One pass point lights don't output a linear depth, so they are already perspective correct.
	bool bUsePerspectiveCorrectShadowDepths = !ShadowDepthType.bDirectionalLight && !ShadowDepthType.bOnePassPointLightShadow;
	bool bOnePassPointLightShadow = ShadowDepthType.bOnePassPointLightShadow;
	
	bool bVirtualShadowMap = MeshPassTargetType == EMeshPass::VSMShadowDepth;
	if (bVirtualShadowMap)
	{
		bUsePerspectiveCorrectShadowDepths = false;
		bOnePassPointLightShadow = false;
	}

	if (!GetShadowDepthPassShaders(
		MaterialResource,
		VertexFactory,
		FeatureLevel,
		ShadowDepthType.bDirectionalLight,
		bOnePassPointLightShadow,
		bUsePositionOnlyVS,
		bUsePerspectiveCorrectShadowDepths,
		bVirtualShadowMap,
		ShadowDepthPassShaders.VertexShader,
		ShadowDepthPassShaders.PixelShader))
	{
		return false;
	}

	FShadowDepthShaderElementData ShaderElementData;
	ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

	const FMeshDrawCommandSortKey SortKey = CalculateMeshStaticSortKey(ShadowDepthPassShaders.VertexShader, ShadowDepthPassShaders.PixelShader);

	const bool bUseGpuSceneInstancing = UseGPUScene(GShaderPlatformForFeatureLevel[FeatureLevel], FeatureLevel) && VertexFactory->SupportsGPUScene(FeatureLevel);
	
	// Need to replicate for cube faces on host if GPU-scene is not available (for this draw).
	const bool bPerformHostCubeFaceReplication = ShadowDepthType.bOnePassPointLightShadow && !bUseGpuSceneInstancing;
	const uint32 InstanceFactor = bPerformHostCubeFaceReplication ? 6 : 1;

	for (uint32 i = 0; i < InstanceFactor; i++)
	{
		ShaderElementData.LayerId = i;
		ShaderElementData.bUseGpuSceneInstancing = bUseGpuSceneInstancing;

		BuildMeshDrawCommands(
			MeshBatch,
			BatchElementMask,
			PrimitiveSceneProxy,
			MaterialRenderProxy,
			MaterialResource,
			PassDrawRenderState,
			ShadowDepthPassShaders,
			MeshFillMode,
			MeshCullMode,
			SortKey,
			bUsePositionOnlyVS ? EMeshPassFeatures::PositionAndNormalOnly : EMeshPassFeatures::Default,
			ShaderElementData);
	}

	return true;
}

bool FShadowDepthPassMeshProcessor::TryAddMeshBatch(const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	const FMaterialRenderProxy& MaterialRenderProxy,
	const FMaterial& Material)
{
	const EBlendMode BlendMode = Material.GetBlendMode();
	const bool bShouldCastShadow = Material.ShouldCastDynamicShadows();

	const FMeshDrawingPolicyOverrideSettings OverrideSettings = ComputeMeshOverrideSettings(MeshBatch);
	const ERasterizerFillMode MeshFillMode = ComputeMeshFillMode(MeshBatch, Material, OverrideSettings);

	ERasterizerCullMode FinalCullMode;

	{
		const ERasterizerCullMode MeshCullMode = ComputeMeshCullMode(MeshBatch, Material, OverrideSettings);

		const bool bTwoSided = Material.IsTwoSided() || PrimitiveSceneProxy->CastsShadowAsTwoSided();
		// Invert culling order when mobile HDR == false.
		auto ShaderPlatform = GShaderPlatformForFeatureLevel[FeatureLevel];
		static auto* MobileHDRCvar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileHDR"));
		check(MobileHDRCvar);
		const bool bPlatformReversesCulling = (RHINeedsToSwitchVerticalAxis(ShaderPlatform) && MobileHDRCvar->GetValueOnAnyThread() == 0);

		const bool bRenderSceneTwoSided = bTwoSided;
		const bool bShadowReversesCulling = MeshPassTargetType == EMeshPass::VSMShadowDepth ? false : ShadowDepthType.bOnePassPointLightShadow;
		const bool bReverseCullMode = XOR(bPlatformReversesCulling, bShadowReversesCulling);

		FinalCullMode = bRenderSceneTwoSided ? CM_None : bReverseCullMode ? InverseCullMode(MeshCullMode) : MeshCullMode;
	}

	bool bResult = true;
	if (bShouldCastShadow
		&& ShouldIncludeDomainInMeshPass(Material.GetMaterialDomain())
		&& ShouldIncludeMaterialInDefaultOpaquePass(Material)
		&& EnumHasAnyFlags(MeshSelectionMask, MeshBatch.VertexFactory->SupportsGPUScene(FeatureLevel) ? EShadowMeshSelection::VSM : EShadowMeshSelection::SM))
	{
		const FMaterialRenderProxy* EffectiveMaterialRenderProxy = &MaterialRenderProxy;
		const FMaterial* EffectiveMaterial = &Material;

		OverrideWithDefaultMaterialForShadowDepth(EffectiveMaterialRenderProxy, EffectiveMaterial, FeatureLevel);

		bResult = Process(MeshBatch, BatchElementMask, StaticMeshId, PrimitiveSceneProxy, *EffectiveMaterialRenderProxy, *EffectiveMaterial, MeshFillMode, FinalCullMode);
	}

	return bResult;
}

void FShadowDepthPassMeshProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	if (MeshBatch.CastShadow)
	{
		const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
		while (MaterialRenderProxy)
		{
			const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
			if (Material)
			{
				if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, *MaterialRenderProxy, *Material))
				{
					break;
				}
			}
			MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
		}
	}
}

FShadowDepthPassMeshProcessor::FShadowDepthPassMeshProcessor(
	const FScene* Scene,
	const FSceneView* InViewIfDynamicMeshCommand,
	FShadowDepthType InShadowDepthType,
	FMeshPassDrawListContext* InDrawListContext,
	EMeshPass::Type InMeshPassTargetType)
	: FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext)
	, ShadowDepthType(InShadowDepthType)
	, MeshPassTargetType(InMeshPassTargetType)
{
	if (UseNonNaniteVirtualShadowMaps(Scene->GetShaderPlatform(), Scene->GetFeatureLevel()))
	{
		// set up mesh filtering.
		MeshSelectionMask = MeshPassTargetType == EMeshPass::VSMShadowDepth ? EShadowMeshSelection::VSM : EShadowMeshSelection::SM;
	}
	else
	{
		// If VSMs are disabled, pipe all kinds of draws into the regular SMs
		MeshSelectionMask = EShadowMeshSelection::All;
	}
	SetStateForShadowDepth(ShadowDepthType.bOnePassPointLightShadow, ShadowDepthType.bDirectionalLight, PassDrawRenderState, MeshPassTargetType);
}

FShadowDepthType CSMShadowDepthType(true, false);

FMeshPassProcessor* CreateCSMShadowDepthPassProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	return new(FMemStack::Get()) FShadowDepthPassMeshProcessor(
		Scene,
		InViewIfDynamicMeshCommand,
		CSMShadowDepthType,
		InDrawListContext,
		EMeshPass::CSMShadowDepth);
}

FRegisterPassProcessorCreateFunction RegisterCSMShadowDepthPass(&CreateCSMShadowDepthPassProcessor, EShadingPath::Deferred, EMeshPass::CSMShadowDepth, EMeshPassFlags::CachedMeshCommands);

FMeshPassProcessor* CreateVSMShadowDepthPassProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	// Only create the mesh pass processor if VSMs are not enabled as this prevents wasting time caching the SM draw commands
	if (UseNonNaniteVirtualShadowMaps(Scene->GetShaderPlatform(), Scene->GetFeatureLevel()))
	{
		return new(FMemStack::Get()) FShadowDepthPassMeshProcessor(
			Scene,
			InViewIfDynamicMeshCommand,
			CSMShadowDepthType,
			InDrawListContext,
			EMeshPass::VSMShadowDepth);
	}

	return nullptr;
}
FRegisterPassProcessorCreateFunction RegisterVSMShadowDepthPass(&CreateVSMShadowDepthPassProcessor, EShadingPath::Deferred, EMeshPass::VSMShadowDepth, EMeshPassFlags::CachedMeshCommands);


FRegisterPassProcessorCreateFunction RegisterMobileCSMShadowDepthPass(&CreateCSMShadowDepthPassProcessor, EShadingPath::Mobile, EMeshPass::CSMShadowDepth, EMeshPassFlags::CachedMeshCommands);
