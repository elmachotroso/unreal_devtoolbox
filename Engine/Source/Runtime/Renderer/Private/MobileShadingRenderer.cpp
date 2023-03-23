// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	MobileShadingRenderer.cpp: Scene rendering code for ES3/3.1 feature level.
=============================================================================*/

#include "CoreMinimal.h"
#include "Stats/Stats.h"
#include "Misc/MemStack.h"
#include "HAL/IConsoleManager.h"
#include "EngineGlobals.h"
#include "RHIDefinitions.h"
#include "RHI.h"
#include "RenderResource.h"
#include "RendererInterface.h"
#include "SceneUtils.h"
#include "UniformBuffer.h"
#include "Engine/BlendableInterface.h"
#include "ShaderParameters.h"
#include "RHIStaticStates.h"
#include "Shader.h"
#include "StaticBoundShaderState.h"
#include "PostProcess/SceneRenderTargets.h"
#include "GlobalShader.h"
#include "SceneRendering.h"
#include "ScenePrivate.h"
#include "PostProcess/SceneFilterRendering.h"
#include "FXSystem.h"
#include "PostProcess/PostProcessing.h"
#include "PostProcess/PostProcessMobile.h"
#include "PostProcess/PostProcessUpscale.h"
#include "PostProcess/PostProcessCompositeEditorPrimitives.h"
#include "PostProcess/PostProcessHMD.h"
#include "PostProcess/PostProcessPixelProjectedReflectionMobile.h"
#include "PostProcess/PostProcessAmbientOcclusionMobile.h"
#include "IHeadMountedDisplay.h"
#include "IXRTrackingSystem.h"
#include "SceneViewExtension.h"
#include "ScreenRendering.h"
#include "PipelineStateCache.h"
#include "ClearQuad.h"
#include "MobileSeparateTranslucencyPass.h"
#include "MobileDistortionPass.h"
#include "VisualizeTexturePresent.h"
#include "RendererModule.h"
#include "EngineModule.h"
#include "GPUScene.h"
#include "MaterialSceneTextureId.h"
#include "SkyAtmosphereRendering.h"
#include "VisualizeTexture.h"
#include "VT/VirtualTextureFeedback.h"
#include "VT/VirtualTextureSystem.h"
#include "GPUSortManager.h"
#include "MobileDeferredShadingPass.h"
#include "PlanarReflectionSceneProxy.h"
#include "InstanceCulling/InstanceCullingManager.h"
#include "SceneOcclusion.h"
#include "VariableRateShadingImageManager.h"
#include "SceneTextureReductions.h"
#include "GPUMessaging.h"

uint32 GetShadowQuality();

static TAutoConsoleVariable<int32> CVarMobileForceDepthResolve(
	TEXT("r.Mobile.ForceDepthResolve"),
	0,
	TEXT("0: Depth buffer is resolved by switching out render targets. (Default)\n")
	TEXT("1: Depth buffer is resolved by switching out render targets and drawing with the depth texture.\n"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMobileAdrenoOcclusionMode(
	TEXT("r.Mobile.AdrenoOcclusionMode"),
	0,
	TEXT("0: Render occlusion queries after the base pass (default).\n")
	TEXT("1: Render occlusion queries after translucency and a flush, which can help Adreno devices in GL mode."),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarMobileCustomDepthForTranslucency(
	TEXT("r.Mobile.CustomDepthForTranslucency"),
	1,
	TEXT(" Whether to render custom depth/stencil if any tranclucency in the scene uses it. \n")
	TEXT(" 0 = Off \n")
	TEXT(" 1 = On [default]"),
	ECVF_Scalability | ECVF_RenderThreadSafe);

DECLARE_GPU_STAT_NAMED(MobileSceneRender, TEXT("Mobile Scene Render"));

DECLARE_CYCLE_STAT(TEXT("SceneStart"), STAT_CLMM_SceneStart, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("SceneEnd"), STAT_CLMM_SceneEnd, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("InitViews"), STAT_CLMM_InitViews, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Opaque"), STAT_CLMM_Opaque, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Occlusion"), STAT_CLMM_Occlusion, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Post"), STAT_CLMM_Post, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Translucency"), STAT_CLMM_Translucency, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Shadows"), STAT_CLMM_Shadows, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("SceneSimulation"), STAT_CLMM_SceneSim, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("PrePass"), STAT_CLM_MobilePrePass, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("Velocity"), STAT_CLMM_Velocity, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("AfterVelocity"), STAT_CLMM_AfterVelocity, STATGROUP_CommandListMarkers);
DECLARE_CYCLE_STAT(TEXT("TranslucentVelocity"), STAT_CLMM_TranslucentVelocity, STATGROUP_CommandListMarkers);

FGlobalDynamicIndexBuffer FMobileSceneRenderer::DynamicIndexBuffer;
FGlobalDynamicVertexBuffer FMobileSceneRenderer::DynamicVertexBuffer;
TGlobalResource<FGlobalDynamicReadBuffer> FMobileSceneRenderer::DynamicReadBuffer;

extern bool IsMobileEyeAdaptationEnabled(const FViewInfo& View);

static bool UsesCustomDepthStencilLookup(const FViewInfo& View)
{
	bool bUsesCustomDepthStencil = false;

	// Find out whether CustomDepth/Stencil used in translucent materials
	if (View.bUsesCustomDepthStencilInTranslucentMaterials && CVarMobileCustomDepthForTranslucency.GetValueOnAnyThread() != 0)
	{
		bUsesCustomDepthStencil = true;
	}
	else
	{
		// Find out whether post-process materials use CustomDepth/Stencil lookups
		const FBlendableManager& BlendableManager = View.FinalPostProcessSettings.BlendableManager;
		FBlendableEntry* BlendableIt = nullptr;

		while (FPostProcessMaterialNode* DataPtr = BlendableManager.IterateBlendables<FPostProcessMaterialNode>(BlendableIt))
		{
			if (DataPtr->IsValid())
			{
				FMaterialRenderProxy* Proxy = DataPtr->GetMaterialInterface()->GetRenderProxy();
				check(Proxy);

				const FMaterial& Material = Proxy->GetIncompleteMaterialWithFallback(View.GetFeatureLevel());
				if (Material.IsStencilTestEnabled())
				{
					bUsesCustomDepthStencil = true;
					break;
				}

				const FMaterialShaderMap* MaterialShaderMap = Material.GetRenderingThreadShaderMap();
				if (MaterialShaderMap->UsesSceneTexture(PPI_CustomDepth) || MaterialShaderMap->UsesSceneTexture(PPI_CustomStencil))
				{
					bUsesCustomDepthStencil = true;
					break;
				}
			}
		}
	}

	// Find out whether there are primitives will render in custom depth pass or just always render custom depth
	bUsesCustomDepthStencil &= (View.bHasCustomDepthPrimitives || GetCustomDepthMode() == ECustomDepthMode::EnabledWithStencil);

	return bUsesCustomDepthStencil;
}

static void RenderOpaqueFX(
	FRDGBuilder& GraphBuilder,
	TArrayView<const FViewInfo> Views,
	FFXSystemInterface* FXSystem,
	TRDGUniformBufferRef<FMobileSceneTextureUniformParameters> MobileSceneTexturesUniformBuffer)
{
	// Notify the FX system that opaque primitives have been rendered and we now have a valid depth buffer.
	if (FXSystem && Views.Num() > 0)
	{
		FXSystem->PostRenderOpaque(GraphBuilder, Views, true /*bAllowGPUParticleUpdate*/);

		if (FGPUSortManager* GPUSortManager = FXSystem->GetGPUSortManager())
		{
			GPUSortManager->OnPostRenderOpaque(GraphBuilder);
		}
	}
}

BEGIN_SHADER_PARAMETER_STRUCT(FMobileRenderPassParameters, RENDERER_API)
	SHADER_PARAMETER_STRUCT_INCLUDE(FViewShaderParameters, View)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FMobileBasePassUniformParameters, MobileBasePass)
	RDG_BUFFER_ACCESS_ARRAY(DrawIndirectArgsBuffers)
	RDG_BUFFER_ACCESS_ARRAY(InstanceIdOffsetBuffers)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

static bool PostProcessUsesSceneDepth(const FViewInfo& View)
{
	// Find out whether post-process materials use CustomDepth/Stencil lookups
	const FBlendableManager& BlendableManager = View.FinalPostProcessSettings.BlendableManager;
	FBlendableEntry* BlendableIt = nullptr;

	while (FPostProcessMaterialNode* DataPtr = BlendableManager.IterateBlendables<FPostProcessMaterialNode>(BlendableIt))
	{
		if (DataPtr->IsValid())
		{
			FMaterialRenderProxy* Proxy = DataPtr->GetMaterialInterface()->GetRenderProxy();
			check(Proxy);

			const FMaterial& Material = Proxy->GetIncompleteMaterialWithFallback(View.GetFeatureLevel());
			const FMaterialShaderMap* MaterialShaderMap = Material.GetRenderingThreadShaderMap();
			if (MaterialShaderMap->UsesSceneTexture(PPI_SceneDepth))
			{
				return true;
			}
		}
	}
	return false;
}

static void SetupGBufferFlags(FSceneTexturesConfig& SceneTexturesConfig, bool bRequiresMultiPass)
{
	ETextureCreateFlags AddFlags = TexCreate_InputAttachmentRead;
	if (!bRequiresMultiPass)
	{
		// use memoryless GBuffer when possible
		AddFlags|= TexCreate_Memoryless;
	}
	
	SceneTexturesConfig.GBufferA.Flags|= AddFlags;
	SceneTexturesConfig.GBufferB.Flags|= AddFlags;
	SceneTexturesConfig.GBufferC.Flags|= AddFlags;
	SceneTexturesConfig.GBufferD.Flags|= AddFlags;
	SceneTexturesConfig.GBufferE.Flags|= AddFlags;
	
	// Mobile uses FBF/subpassLoad to fetch data from GBuffer, and FBF does not always work with sRGB targets 
	SceneTexturesConfig.GBufferA.Flags &= (~TexCreate_SRGB);
	SceneTexturesConfig.GBufferB.Flags &= (~TexCreate_SRGB);
	SceneTexturesConfig.GBufferC.Flags &= (~TexCreate_SRGB);
	SceneTexturesConfig.GBufferD.Flags &= (~TexCreate_SRGB);
	SceneTexturesConfig.GBufferE.Flags &= (~TexCreate_SRGB);
}

FMobileSceneRenderer::FMobileSceneRenderer(const FSceneViewFamily* InViewFamily,FHitProxyConsumer* HitProxyConsumer)
	: FSceneRenderer(InViewFamily, HitProxyConsumer)
	, bGammaSpace(!IsMobileHDR())
	, bDeferredShading(IsMobileDeferredShadingEnabled(ShaderPlatform))
	, bUseVirtualTexturing(UseVirtualTexturing(FeatureLevel))
{
	bRenderToSceneColor = false;
	bRequiresMultiPass = false;
	bKeepDepthContent = false;
	bModulatedShadowsInUse = false;
	bShouldRenderCustomDepth = false;
	bRequiresPixelProjectedPlanarRelfectionPass = false;
	bRequiresAmbientOcclusionPass = false;
	bIsFullDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_AllOpaque;
	bIsMaskedOnlyDepthPrepassEnabled = Scene->EarlyZPassMode == DDM_MaskedOnly;
	bRequiresSceneDepthAux = MobileRequiresSceneDepthAux(ShaderPlatform);
	
	StandardTranslucencyPass = ViewFamily.AllowTranslucencyAfterDOF() ? ETranslucencyPass::TPT_StandardTranslucency : ETranslucencyPass::TPT_AllTranslucency;
	StandardTranslucencyMeshPass = TranslucencyPassToMeshPass(StandardTranslucencyPass);

	// Don't do occlusion queries when doing scene captures
	for (FViewInfo& View : Views)
	{
		if (View.bIsSceneCapture)
		{
			View.bDisableQuerySubmissions = true;
			View.bIgnoreExistingQueries = true;
		}
	}

	FMemory::Memzero(MeshPassInstanceCullingDrawParams);

	NumMSAASamples = GetDefaultMSAACount(ERHIFeatureLevel::ES3_1);
}

class FMobileDirLightShaderParamsRenderResource : public FRenderResource
{
public:
	using MobileDirLightUniformBufferRef = TUniformBufferRef<FMobileDirectionalLightShaderParameters>;

	virtual void InitRHI() override
	{
		UniformBufferRHI =
			MobileDirLightUniformBufferRef::CreateUniformBufferImmediate(
				FMobileDirectionalLightShaderParameters(),
				UniformBuffer_MultiFrame);
	}

	virtual void ReleaseRHI() override
	{
		UniformBufferRHI.SafeRelease();
	}

	MobileDirLightUniformBufferRef UniformBufferRHI;
};

TUniformBufferRef<FMobileDirectionalLightShaderParameters>& GetNullMobileDirectionalLightShaderParameters()
{
	static TGlobalResource<FMobileDirLightShaderParamsRenderResource>* NullLightParams;
	if (!NullLightParams)
	{
		NullLightParams = new TGlobalResource<FMobileDirLightShaderParamsRenderResource>();
	}
	check(!!NullLightParams->UniformBufferRHI);
	return NullLightParams->UniformBufferRHI;
}

void FMobileSceneRenderer::PrepareViewVisibilityLists()
{
	// Prepare view's visibility lists.
	// TODO: only do this when CSM + static is required.
	for (auto& View : Views)
	{
		FMobileCSMVisibilityInfo& MobileCSMVisibilityInfo = View.MobileCSMVisibilityInfo;
		// Init list of primitives that can receive Dynamic CSM.
		MobileCSMVisibilityInfo.MobilePrimitiveCSMReceiverVisibilityMap.Init(false, View.PrimitiveVisibilityMap.Num());

		// Init static mesh visibility info for CSM drawlist
		MobileCSMVisibilityInfo.MobileCSMStaticMeshVisibilityMap.Init(false, View.StaticMeshVisibilityMap.Num());

		// Init static mesh visibility info for default drawlist that excludes meshes in CSM only drawlist.
		MobileCSMVisibilityInfo.MobileNonCSMStaticMeshVisibilityMap = View.StaticMeshVisibilityMap;
	}
}

void FMobileSceneRenderer::SetupMobileBasePassAfterShadowInit(FExclusiveDepthStencil::Type BasePassDepthStencilAccess, FViewVisibleCommandsPerView& ViewCommandsPerView, FInstanceCullingManager& InstanceCullingManager)
{
	// Sort front to back on all platforms, even HSR benefits from it
	//const bool bWantsFrontToBackSorting = (GHardwareHiddenSurfaceRemoval == false);

	// compute keys for front to back sorting and dispatch pass setup.
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		FViewInfo& View = Views[ViewIndex];
		FViewCommands& ViewCommands = ViewCommandsPerView[ViewIndex];

		PassProcessorCreateFunction CreateFunction = FPassProcessorManager::GetCreateFunction(EShadingPath::Mobile, EMeshPass::BasePass);
		FMeshPassProcessor* MeshPassProcessor = CreateFunction(Scene, &View, nullptr);

		PassProcessorCreateFunction BasePassCSMCreateFunction = FPassProcessorManager::GetCreateFunction(EShadingPath::Mobile, EMeshPass::MobileBasePassCSM);
		FMeshPassProcessor* BasePassCSMMeshPassProcessor = BasePassCSMCreateFunction(Scene, &View, nullptr);

		TArray<int32, TInlineAllocator<2> > ViewIds;
		ViewIds.Add(View.GPUSceneViewId);
		// Only apply instancing for ISR to main view passes
		EInstanceCullingMode InstanceCullingMode = View.IsInstancedStereoPass() ? EInstanceCullingMode::Stereo : EInstanceCullingMode::Normal;
		if (InstanceCullingMode == EInstanceCullingMode::Stereo)
		{
			check(View.GetInstancedView() != nullptr);
			ViewIds.Add(View.GetInstancedView()->GPUSceneViewId);
		}

		// Run sorting on BasePass, as it's ignored inside FSceneRenderer::SetupMeshPass, so it can be done after shadow init on mobile.
		FParallelMeshDrawCommandPass& Pass = View.ParallelMeshDrawCommandPasses[EMeshPass::BasePass];
		if (ShouldDumpMeshDrawCommandInstancingStats())
		{
			Pass.SetDumpInstancingStats(GetMeshPassName(EMeshPass::BasePass));
		}

		Pass.DispatchPassSetup(
			Scene,
			View,
			FInstanceCullingContext(FeatureLevel, &InstanceCullingManager, ViewIds, nullptr, InstanceCullingMode),
			EMeshPass::BasePass,
			BasePassDepthStencilAccess,
			MeshPassProcessor,
			View.DynamicMeshElements,
			&View.DynamicMeshElementsPassRelevance,
			View.NumVisibleDynamicMeshElements[EMeshPass::BasePass],
			ViewCommands.DynamicMeshCommandBuildRequests[EMeshPass::BasePass],
			ViewCommands.NumDynamicMeshCommandBuildRequestElements[EMeshPass::BasePass],
			ViewCommands.MeshCommands[EMeshPass::BasePass],
			BasePassCSMMeshPassProcessor,
			&ViewCommands.MeshCommands[EMeshPass::MobileBasePassCSM]);
	}
}

/**
 * Initialize scene's views.
 * Check visibility, sort translucent items, etc.
 */
void FMobileSceneRenderer::InitViews(FRDGBuilder& GraphBuilder, FSceneTexturesConfig& SceneTexturesConfig, FInstanceCullingManager& InstanceCullingManager)
{
	FRHICommandListImmediate& RHICmdList = GraphBuilder.RHICmdList;
	RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_InitViews));

	SCOPED_DRAW_EVENT(RHICmdList, InitViews);

	SCOPE_CYCLE_COUNTER(STAT_InitViewsTime);
	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(InitViews_Scene);

	check(Scene);

	// Create GPU-side reprensenation of the view for instance culling.
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		Views[ViewIndex].GPUSceneViewId = InstanceCullingManager.RegisterView(Views[ViewIndex]);
	}

	if (bUseVirtualTexturing)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, VirtualTextureUpdate);
		// AllocateResources needs to be called before RHIBeginScene
		FVirtualTextureSystem::Get().AllocateResources(GraphBuilder, FeatureLevel);
		FVirtualTextureSystem::Get().CallPendingCallbacks();
		VirtualTextureFeedbackBegin(GraphBuilder, Views, SceneTexturesConfig.Extent);
	}

	FILCUpdatePrimTaskData ILCTaskData;
	FViewVisibleCommandsPerView ViewCommandsPerView;
	ViewCommandsPerView.SetNum(Views.Num());

	const FExclusiveDepthStencil::Type BasePassDepthStencilAccess = FExclusiveDepthStencil::DepthWrite_StencilWrite;

	PreVisibilityFrameSetup(GraphBuilder, SceneTexturesConfig);
	if (FXSystem && FXSystem->RequiresEarlyViewUniformBuffer() && Views.IsValidIndex(0))
	{
		// This is to init the ViewUniformBuffer before rendering for the Niagara compute shader.
		// This needs to run before ComputeViewVisibility() is called, but the views normally initialize the ViewUniformBuffer after that (at the end of this method).
		Views[0].InitRHIResources();
		FXSystem->PostInitViews(GraphBuilder, Views, !ViewFamily.EngineShowFlags.HitProxies);
	}
	ComputeViewVisibility(RHICmdList, BasePassDepthStencilAccess, ViewCommandsPerView, DynamicIndexBuffer, DynamicVertexBuffer, DynamicReadBuffer, InstanceCullingManager);
	PostVisibilityFrameSetup(ILCTaskData);

	const FIntPoint RenderTargetSize = (ViewFamily.RenderTarget->GetRenderTargetTexture().IsValid()) ? ViewFamily.RenderTarget->GetRenderTargetTexture()->GetSizeXY() : ViewFamily.RenderTarget->GetSizeXY();
	const bool bRequiresUpscale = ((int32)RenderTargetSize.X > FamilySize.X || (int32)RenderTargetSize.Y > FamilySize.Y) || IsMobilePropagateAlphaEnabled(ViewFamily.GetShaderPlatform());
	// ES requires that the back buffer and depth match dimensions.
	// For the most part this is not the case when using scene captures. Thus scene captures always render to scene color target.
	const bool bShouldCompositeEditorPrimitives = FSceneRenderer::ShouldCompositeEditorPrimitives(Views[0]);
	const bool bStereoRenderingAndHMD = ViewFamily.EngineShowFlags.StereoRendering && ViewFamily.EngineShowFlags.HMDDistortion;
	bRenderToSceneColor = !bGammaSpace || bStereoRenderingAndHMD || bRequiresUpscale || bShouldCompositeEditorPrimitives || Views[0].bIsSceneCapture || Views[0].bIsReflectionCapture;
	const FPlanarReflectionSceneProxy* PlanarReflectionSceneProxy = Scene ? Scene->GetForwardPassGlobalPlanarReflection() : nullptr;

	bRequiresPixelProjectedPlanarRelfectionPass = IsUsingMobilePixelProjectedReflection(ShaderPlatform)
		&& PlanarReflectionSceneProxy != nullptr
		&& PlanarReflectionSceneProxy->RenderTarget != nullptr
		&& !Views[0].bIsReflectionCapture
		&& !ViewFamily.EngineShowFlags.HitProxies
		&& ViewFamily.EngineShowFlags.Lighting
		&& !ViewFamily.EngineShowFlags.VisualizeLightCulling
		&& !ViewFamily.UseDebugViewPS();

	bRequiresAmbientOcclusionPass = IsUsingMobileAmbientOcclusion(ShaderPlatform)
		&& Views[0].FinalPostProcessSettings.AmbientOcclusionIntensity > 0
		&& (Views[0].FinalPostProcessSettings.AmbientOcclusionStaticFraction >= 1 / 100.0f || (Scene && Scene->SkyLight && Scene->SkyLight->ProcessedTexture && Views[0].Family->EngineShowFlags.SkyLighting))
		&& ViewFamily.EngineShowFlags.Lighting
		&& !Views[0].bIsReflectionCapture
		&& !Views[0].bIsPlanarReflection
		&& !ViewFamily.EngineShowFlags.HitProxies
		&& !ViewFamily.EngineShowFlags.VisualizeLightCulling
		&& !ViewFamily.UseDebugViewPS();

	bShouldRenderVelocities = ShouldRenderVelocities();

	bRequiresDistanceField = IsMobileDistanceFieldEnabled(ShaderPlatform)
		&& ViewFamily.EngineShowFlags.Lighting
		&& !Views[0].bIsReflectionCapture
		&& !Views[0].bIsPlanarReflection
		&& !ViewFamily.EngineShowFlags.HitProxies
		&& !ViewFamily.EngineShowFlags.VisualizeLightCulling
		&& !ViewFamily.UseDebugViewPS();

	bShouldRenderHZB = ShouldRenderHZB();
		

	// Whether we need to store depth for post-processing
	// On PowerVR we see flickering of shadows and depths not updating correctly if targets are discarded.
	const bool bForceDepthResolve = (CVarMobileForceDepthResolve.GetValueOnRenderThread() == 1);
	const bool bSeparateTranslucencyActive = IsMobileSeparateTranslucencyActive(Views.GetData(), Views.Num()); 
	const bool bPostProcessUsesSceneDepth = PostProcessUsesSceneDepth(Views[0]) || IsMobileDistortionActive(Views[0]);
	const bool bRequireSeparateViewPass = Views.Num() > 1 && !Views[0].bIsMobileMultiViewEnabled;
	bRequiresMultiPass = RequiresMultiPass(RHICmdList, Views[0]);
	bKeepDepthContent =
		bRequiresMultiPass ||
		bForceDepthResolve ||
		bRequiresAmbientOcclusionPass ||
		bRequiresDistanceField ||
		bRequiresPixelProjectedPlanarRelfectionPass ||
		bSeparateTranslucencyActive ||
		Views[0].bIsReflectionCapture ||
		(bDeferredShading && bPostProcessUsesSceneDepth) ||
		bShouldRenderVelocities ||
		bRequireSeparateViewPass ||
		bIsFullDepthPrepassEnabled ||
		GraphBuilder.IsDumpingFrame();
	// never keep MSAA depth
	bKeepDepthContent = ((NumMSAASamples > 1) && bRequiresSceneDepthAux) ? false : bKeepDepthContent;

	// Depth is needed for Editor Primitives
	if (bShouldCompositeEditorPrimitives)
	{
		bKeepDepthContent = true;
	}

	// In the editor RHIs may split a render-pass into several cmd buffer submissions, so all targets need to Store
	if (IsSimulatedPlatform(ShaderPlatform))
	{
		bKeepDepthContent = true;
	}

	// Update the bKeepDepthContent based on the mobile renderer status.
	SceneTexturesConfig.bKeepDepthContent = bKeepDepthContent;
	
	if (bDeferredShading) 
	{
		SetupGBufferFlags(SceneTexturesConfig, bRequiresMultiPass || GraphBuilder.IsDumpingFrame());
	}

	// Update the pixel projected reflection extent according to the settings in the PlanarReflectionComponent.
	if (bRequiresPixelProjectedPlanarRelfectionPass)
	{
		SceneTexturesConfig.MobilePixelProjectedReflectionExtent = PlanarReflectionSceneProxy->RenderTarget->GetSizeXY();
	}
	else
	{
		SceneTexturesConfig.MobilePixelProjectedReflectionExtent = FIntPoint::ZeroValue;
	}
	
	// Finalize and set the scene textures config.
	FSceneTexturesConfig::Set(SceneTexturesConfig);

	// Initialise Sky/View resources before the view global uniform buffer is built.
	if (ShouldRenderSkyAtmosphere(Scene, ViewFamily.EngineShowFlags))
	{
		InitSkyAtmosphereForViews(RHICmdList);
	}

	if (bRequiresDistanceField)
	{
		InitMobileSDFShadowingOutputs(RHICmdList, SceneTexturesConfig.Extent);
	}
	else
	{
		ReleaseMobileSDFShadowingOutputs();
	}

	// Find out whether custom depth pass should be rendered.
	{
		bool bCouldUseCustomDepthStencil = !bGammaSpace && (!Scene->World || (Scene->World->WorldType != EWorldType::EditorPreview && Scene->World->WorldType != EWorldType::Inactive));
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			Views[ViewIndex].bCustomDepthStencilValid = bCouldUseCustomDepthStencil && UsesCustomDepthStencilLookup(Views[ViewIndex]);
			bShouldRenderCustomDepth |= Views[ViewIndex].bCustomDepthStencilValid;
		}
	}
	
	const bool bDynamicShadows = ViewFamily.EngineShowFlags.DynamicShadows;
	
	if (bDynamicShadows && !IsSimpleForwardShadingEnabled(ShaderPlatform))
	{
		// Setup dynamic shadows.
		InitDynamicShadows(RHICmdList, InstanceCullingManager);
	}
	else
	{
		// TODO: only do this when CSM + static is required.
		PrepareViewVisibilityLists();
	}

	/** Before SetupMobileBasePassAfterShadowInit, we need to update the uniform buffer and shadow info for all movable point lights.*/
	UpdateMovablePointLightUniformBufferAndShadowInfo();

	SetupMobileBasePassAfterShadowInit(BasePassDepthStencilAccess, ViewCommandsPerView, InstanceCullingManager);

	// if we kicked off ILC update via task, wait and finalize.
	if (ILCTaskData.TaskRef.IsValid())
	{
		Scene->IndirectLightingCache.FinalizeCacheUpdates(Scene, *this, ILCTaskData);
	}

	// initialize per-view uniform buffer.  Pass in shadow info as necessary.
	for (int32 ViewIndex = Views.Num() - 1; ViewIndex >= 0; --ViewIndex)
	{
		FViewInfo& View = Views[ViewIndex];
		
		if (View.ViewState)
		{
			View.ViewState->UpdatePreExposure(View);
		}

		// Initialize the view's RHI resources.
		View.InitRHIResources();
	}

	Scene->GPUScene.Update(GraphBuilder, *Scene);
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		Scene->GPUScene.UploadDynamicPrimitiveShaderDataForView(GraphBuilder, Scene, Views[ViewIndex]);
	}

	if (bRequiresDistanceField)
	{
		PrepareDistanceFieldScene(GraphBuilder, false);
	}
	
	InstanceCullingManager.BeginDeferredCulling(GraphBuilder, Scene->GPUScene);

	extern TSet<IPersistentViewUniformBufferExtension*> PersistentViewUniformBufferExtensions;

	for (IPersistentViewUniformBufferExtension* Extension : PersistentViewUniformBufferExtensions)
	{
		Extension->BeginFrame();

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			// Must happen before RHI thread flush so any tasks we dispatch here can land in the idle gap during the flush
			Extension->PrepareView(&Views[ViewIndex]);
		}
	}

	if (bDeferredShading)
	{
		SetupSceneReflectionCaptureBuffer(RHICmdList);
	}
	UpdateSkyReflectionUniformBuffer();

	// Now that the indirect lighting cache is updated, we can update the uniform buffers.
	UpdatePrimitiveIndirectLightingCacheBuffers();
	
	OnStartRender(RHICmdList);
}

static void BuildMeshPassInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, const FGPUScene& GPUScene, FParallelMeshDrawCommandPass& MeshPass, FMobileRenderPassParameters& PassParameters, FInstanceCullingDrawParams& InstanceCullingDrawParams)
{
	// This manual resource handling is need since we can't include multiple FInstanceCullingDrawParams into RDG pass parameters,
	// because FInstanceCullingDrawParams includes a global UB
	MeshPass.BuildRenderingCommands(GraphBuilder, GPUScene, InstanceCullingDrawParams);
	
	if (InstanceCullingDrawParams.DrawIndirectArgsBuffer)
	{
		PassParameters.DrawIndirectArgsBuffers.Emplace(InstanceCullingDrawParams.DrawIndirectArgsBuffer, ERHIAccess::IndirectArgs);
	}
	
	if (InstanceCullingDrawParams.InstanceIdOffsetBuffer)
	{
		PassParameters.InstanceIdOffsetBuffers.Emplace(InstanceCullingDrawParams.InstanceIdOffsetBuffer, ERHIAccess::VertexOrIndexBuffer);
	}
}

/*
* Renders the Full Depth Prepass
*/
void FMobileSceneRenderer::RenderFullDepthPrepass(FRDGBuilder& GraphBuilder, FSceneTextures& SceneTextures)
{
	FRenderTargetBindingSlots BasePassRenderTargets;
	BasePassRenderTargets.DepthStencil = FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
	BasePassRenderTargets.NumOcclusionQueries = ComputeNumOcclusionQueriesToBatch();

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];
		if (!View.ShouldRenderView())
		{
			continue;
		}

		if (ViewIndex > 0)
		{
			BasePassRenderTargets.DepthStencil.SetDepthLoadAction(ERenderTargetLoadAction::ELoad);
			BasePassRenderTargets.DepthStencil.SetStencilLoadAction(ERenderTargetLoadAction::ELoad);
			BasePassRenderTargets.DepthStencil.SetDepthStencilAccess(FExclusiveDepthStencil::DepthWrite_StencilWrite);
		}

		View.BeginRenderView();

		auto* PassParameters = GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
		PassParameters->RenderTargets = BasePassRenderTargets;
		PassParameters->View = View.GetShaderParameters();

		if (Scene->GPUScene.IsEnabled())
		{
			BuildMeshPassInstanceCullingDrawParams(GraphBuilder, Scene->GPUScene, View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass], *PassParameters, MeshPassInstanceCullingDrawParams[EMeshPass::DepthPass]);
		}

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("FullDepthPrepass"),
			PassParameters,
			ERDGPassFlags::Raster,
			[this, PassParameters, &View](FRHICommandListImmediate& RHICmdList)
			{
				RenderPrePass(RHICmdList, View);

				// Issue occlusion queries
				RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
				RenderOcclusion(RHICmdList, View);
			});
	}
}

void FMobileSceneRenderer::RenderMaskedPrePass(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
	if (bIsMaskedOnlyDepthPrepassEnabled)
	{
		RenderPrePass(RHICmdList, View);
	}
}

/** 
* Renders the view family. 
*/
void FMobileSceneRenderer::Render(FRDGBuilder& GraphBuilder)
{
	GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLMM_SceneStart));

	RDG_RHI_EVENT_SCOPE(GraphBuilder, MobileSceneRender);
	RDG_RHI_GPU_STAT_SCOPE(GraphBuilder, MobileSceneRender);

	Scene->UpdateAllPrimitiveSceneInfos(GraphBuilder);

	GPU_MESSAGE_SCOPE(GraphBuilder);

	// Establish scene primitive count (must be done after UpdateAllPrimitiveSceneInfos)
	FGPUSceneScopeBeginEndHelper GPUSceneScopeBeginEndHelper(Scene->GPUScene, GPUSceneDynamicContext, Scene);

	PrepareViewRectsForRendering(GraphBuilder.RHICmdList);

	if (ShouldRenderSkyAtmosphere(Scene, ViewFamily.EngineShowFlags))
	{
		for (int32 LightIndex = 0; LightIndex < NUM_ATMOSPHERE_LIGHTS; ++LightIndex)
		{
			if (Scene->AtmosphereLights[LightIndex])
			{
				PrepareSunLightProxy(*Scene->GetSkyAtmosphereSceneInfo(), LightIndex, *Scene->AtmosphereLights[LightIndex]);
			}
		}
	}
	else
	{
		Scene->ResetAtmosphereLightsProperties();
	}

	CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderOther);
	QUICK_SCOPE_CYCLE_COUNTER(STAT_FMobileSceneRenderer_Render);

	if (!ViewFamily.EngineShowFlags.Rendering)
	{
		return;
	}

	WaitOcclusionTests(GraphBuilder.RHICmdList);
	FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();
	GraphBuilder.RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);

	FSceneTexturesConfig SceneTexturesConfig = FSceneTexturesConfig::Create(ViewFamily);

	// Initialize global system textures (pass-through if already initialized).
	GSystemTextures.InitializeTextures(GraphBuilder.RHICmdList, FeatureLevel);

	// Force the subsurface profile texture to be updated.
	UpdateSubsurfaceProfileTexture(GraphBuilder, ShaderPlatform);

	FRDGSystemTextures::Create(GraphBuilder);

	FInstanceCullingManager& InstanceCullingManager = *GraphBuilder.AllocObject<FInstanceCullingManager>(Scene->GPUScene.IsEnabled(), GraphBuilder);

	// Important that this uses consistent logic throughout the frame, so evaluate once and pass in the flag from here
	// NOTE: Must be done after  system texture initialization
	VirtualShadowMapArray.Initialize(GraphBuilder, Scene->VirtualShadowMapArrayCacheManager, UseVirtualShadowMaps(ShaderPlatform, FeatureLevel));

	// Find the visible primitives and prepare targets and buffers for rendering
	InitViews(GraphBuilder, SceneTexturesConfig, InstanceCullingManager);

	if (GRHINeedsExtraDeletionLatency || !GRHICommandList.Bypass())
	{
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FMobileSceneRenderer_PostInitViewsFlushDel);
		// we will probably stall on occlusion queries, so might as well have the RHI thread and GPU work while we wait.
		// Also when doing RHI thread this is the only spot that will process pending deletes
		FRHICommandListExecutor::GetImmediateCommandList().PollOcclusionQueries();
		FRHICommandListExecutor::GetImmediateCommandList().ImmediateFlush(EImmediateFlushType::FlushRHIThreadFlushResources);
	}

	GEngine->GetPreRenderDelegateEx().Broadcast(GraphBuilder);

	// Global dynamic buffers need to be committed before rendering.
	DynamicIndexBuffer.Commit();
	DynamicVertexBuffer.Commit();
	DynamicReadBuffer.Commit();
	GraphBuilder.RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);

	GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLMM_SceneSim));

	FSceneTextures& SceneTextures = FSceneTextures::Create(GraphBuilder, SceneTexturesConfig);

	if (bUseVirtualTexturing)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, VirtualTextureUpdate);
		FVirtualTextureSystem::Get().Update(GraphBuilder, FeatureLevel, Scene);
	}

	FSortedLightSetSceneInfo& SortedLightSet = *GraphBuilder.AllocObject<FSortedLightSetSceneInfo>();
	if (bDeferredShading)
	{
		GatherAndSortLights(SortedLightSet);
		int32 NumReflectionCaptures = Views[0].NumBoxReflectionCaptures + Views[0].NumSphereReflectionCaptures;
		bool bCullLightsToGrid = (NumReflectionCaptures > 0 || GMobileUseClusteredDeferredShading != 0);
		ComputeLightGrid(GraphBuilder, bCullLightsToGrid, SortedLightSet);
	}

	// Generate the Sky/Atmosphere look up tables
	const bool bShouldRenderSkyAtmosphere = ShouldRenderSkyAtmosphere(Scene, ViewFamily.EngineShowFlags);
	if (bShouldRenderSkyAtmosphere)
	{
		RenderSkyAtmosphereLookUpTables(GraphBuilder);
	}

	// Notify the FX system that the scene is about to be rendered.
	if (FXSystem && ViewFamily.EngineShowFlags.Particles)
	{
		FXSystem->PreRender(GraphBuilder, Views, true /*bAllowGPUParticleUpdate*/);
		if (FGPUSortManager* GPUSortManager = FXSystem->GetGPUSortManager())
		{
			GPUSortManager->OnPreRender(GraphBuilder);
		}
	}

	const auto PollOcclusionQueries = [](FRDGBuilder& GraphBuilder)
	{
		AddPass(GraphBuilder, RDG_EVENT_NAME("PollOcclusionQueries"), [](FRHICommandListImmediate& RHICmdList)
		{
			RHICmdList.PollOcclusionQueries();
			RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
		});
	};

	PollOcclusionQueries(GraphBuilder);

	GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLMM_Shadows));
	RenderShadowDepthMaps(GraphBuilder, InstanceCullingManager);

	PollOcclusionQueries(GraphBuilder);

	// Custom depth
	// bShouldRenderCustomDepth has been initialized in InitViews on mobile platform
	if (bShouldRenderCustomDepth)
	{
		SceneTextures.MobileSetupMode = EMobileSceneTextureSetupMode::None;
		SceneTextures.MobileUniformBuffer = CreateMobileSceneTextureUniformBuffer(GraphBuilder, SceneTextures.MobileSetupMode);

		RenderCustomDepthPass(GraphBuilder, SceneTextures.CustomDepth, SceneTextures.GetSceneTextureShaderParameters(FeatureLevel));
	}

	FRDGTextureRef ViewFamilyTexture = TryCreateViewFamilyTexture(GraphBuilder, ViewFamily);

	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);
	
	if (bIsFullDepthPrepassEnabled)
	{
		RenderFullDepthPrepass(GraphBuilder, SceneTextures);

		if (!bRequiresSceneDepthAux)
		{
			AddResolveSceneDepthPass(GraphBuilder, Views, SceneTextures.Depth);
		}

		SceneTextures.MobileSetupMode = EMobileSceneTextureSetupMode::SceneDepth;
		SceneTextures.MobileUniformBuffer = CreateMobileSceneTextureUniformBuffer(GraphBuilder, SceneTextures.MobileSetupMode);

		if (bRequiresDistanceField)
		{
			CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderMobileShadowProjections);
			RDG_GPU_STAT_SCOPE(GraphBuilder, ShadowProjection);
			RenderMobileShadowProjections(GraphBuilder, SceneTextures.Depth.Resolve);
		}

		if (bShouldRenderHZB)
		{
			RenderHZB(GraphBuilder, SceneTextures.Depth.Resolve);
		}

		if (bRequiresAmbientOcclusionPass)
		{
			RenderAmbientOcclusion(GraphBuilder, SceneTextures.Depth.Resolve, SceneTextures.ScreenSpaceAO);
		}
	}

	if (bDeferredShading)
	{
		RenderDeferred(GraphBuilder, SortedLightSet, ViewFamilyTexture, SceneTextures);
	}
	else
	{
		RenderForward(GraphBuilder, ViewFamilyTexture, SceneTextures);
	}

	SceneTextures.MobileSetupMode = EMobileSceneTextureSetupMode::All;
	SceneTextures.MobileSetupMode &= ~EMobileSceneTextureSetupMode::SceneVelocity;
	SceneTextures.MobileUniformBuffer = CreateMobileSceneTextureUniformBuffer(GraphBuilder, SceneTextures.MobileSetupMode);

	if (bShouldRenderVelocities)
	{
		// Render the velocities of movable objects
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLMM_Velocity));
		RenderVelocities(GraphBuilder, SceneTextures, EVelocityPass::Opaque, false);
		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLMM_AfterVelocity));

		GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLMM_TranslucentVelocity));
		RenderVelocities(GraphBuilder, SceneTextures, EVelocityPass::Translucent, false);

		SceneTextures.MobileSetupMode = EMobileSceneTextureSetupMode::All;
		SceneTextures.MobileUniformBuffer = CreateMobileSceneTextureUniformBuffer(GraphBuilder, SceneTextures.MobileSetupMode);
	}

	FRendererModule& RendererModule = static_cast<FRendererModule&>(GetRendererModule());
	RendererModule.RenderPostOpaqueExtensions(GraphBuilder, Views, SceneTextures);

	RenderOpaqueFX(GraphBuilder, Views, FXSystem, SceneTextures.MobileUniformBuffer);

	if (bRequiresPixelProjectedPlanarRelfectionPass)
	{
		const FPlanarReflectionSceneProxy* PlanarReflectionSceneProxy = Scene ? Scene->GetForwardPassGlobalPlanarReflection() : nullptr;

		RenderPixelProjectedReflection(GraphBuilder, SceneTextures.Color.Resolve, SceneTextures.Depth.Resolve, SceneTextures.PixelProjectedReflection, PlanarReflectionSceneProxy);
	}

	GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLMM_Post));

	if (bUseVirtualTexturing)
	{
		RDG_GPU_STAT_SCOPE(GraphBuilder, VirtualTextureUpdate);
		VirtualTextureFeedbackEnd(GraphBuilder);
	}
	
	if (ViewFamily.bResolveScene)
	{
		if (!bGammaSpace || bRenderToSceneColor)
		{
			// Finish rendering for each view, or the full stereo buffer if enabled
			{
				RDG_EVENT_SCOPE(GraphBuilder, "PostProcessing");
				SCOPE_CYCLE_COUNTER(STAT_FinishRenderViewTargetTime);

				FMobilePostProcessingInputs PostProcessingInputs;
				PostProcessingInputs.ViewFamilyTexture = ViewFamilyTexture;
				PostProcessingInputs.SceneTextures = CreateMobileSceneTextureUniformBuffer(GraphBuilder, EMobileSceneTextureSetupMode::All);

				for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
				{
					RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);
					AddMobilePostProcessingPasses(GraphBuilder, Scene, Views[ViewIndex], PostProcessingInputs, InstanceCullingManager);
				}
			}
		}
	}

	GEngine->GetPostRenderDelegateEx().Broadcast(GraphBuilder);

	GraphBuilder.SetCommandListStat(GET_STATID(STAT_CLMM_SceneEnd));

	RenderFinish(GraphBuilder, ViewFamilyTexture);

	PollOcclusionQueries(GraphBuilder);

	QueueSceneTextureExtractions(GraphBuilder, SceneTextures);
}

void FMobileSceneRenderer::BuildInstanceCullingDrawParams(FRDGBuilder& GraphBuilder, FViewInfo& View, FMobileRenderPassParameters* PassParameters)
{
	if (Scene->GPUScene.IsEnabled())
	{
		if (!bIsFullDepthPrepassEnabled)
		{
			BuildMeshPassInstanceCullingDrawParams(GraphBuilder, Scene->GPUScene, View.ParallelMeshDrawCommandPasses[EMeshPass::DepthPass], *PassParameters, MeshPassInstanceCullingDrawParams[EMeshPass::DepthPass]);
		}
		
		const EMeshPass::Type BaseMeshPasses[] = 
		{
			EMeshPass::BasePass,
			EMeshPass::SkyPass,
			StandardTranslucencyMeshPass,
			EMeshPass::DebugViewMode
		};

		for (int32 MeshPassIdx = 0; MeshPassIdx < UE_ARRAY_COUNT(BaseMeshPasses); ++MeshPassIdx)
		{
			const EMeshPass::Type PassType = BaseMeshPasses[MeshPassIdx];
			BuildMeshPassInstanceCullingDrawParams(GraphBuilder, Scene->GPUScene, View.ParallelMeshDrawCommandPasses[PassType], *PassParameters, MeshPassInstanceCullingDrawParams[PassType]);
		}
	}
}

void FMobileSceneRenderer::RenderForward(FRDGBuilder& GraphBuilder, FRDGTextureRef ViewFamilyTexture, FSceneTextures& SceneTextures)
{
	const FViewInfo& MainView = Views[0];

	FRDGTextureRef SceneColor = nullptr;
	FRDGTextureRef SceneColorResolve = nullptr;
	FRDGTextureRef SceneDepth = nullptr;

	// Verify using both MSAA sample count AND the scene color surface sample count, since on GLES you can't have MSAA color targets,
	// so the color target would be created without MSAA, and MSAA is achieved through magical means (the framebuffer, being MSAA,
	// tells the GPU "execute this renderpass as MSAA, and when you're done, automatically resolve and copy into this non-MSAA texture").
	bool bMobileMSAA = NumMSAASamples > 1 && SceneTextures.Config.NumSamples > 1;

	static const auto CVarMobileMultiView = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("vr.MobileMultiView"));
	const bool bIsMultiViewApplication = (CVarMobileMultiView && CVarMobileMultiView->GetValueOnAnyThread() != 0);
	
	if (bGammaSpace && !bRenderToSceneColor)
	{
		if (bMobileMSAA)
		{
			SceneColor = SceneTextures.Color.Target;
			SceneColorResolve = ViewFamilyTexture;
		}
		else
		{
			SceneColor = ViewFamilyTexture;
		}
		SceneDepth = SceneTextures.Depth.Target;
	}
	else
	{
		SceneColor = SceneTextures.Color.Target;
		SceneColorResolve = bMobileMSAA ? SceneTextures.Color.Resolve : nullptr;
		SceneDepth = SceneTextures.Depth.Target;
	}

	TRefCountPtr<IPooledRenderTarget> ShadingRateTarget = GVRSImageManager.GetMobileVariableRateShadingImage(ViewFamily);

	FRenderTargetBindingSlots BasePassRenderTargets;
	BasePassRenderTargets[0] = FRenderTargetBinding(SceneColor, SceneColorResolve, ERenderTargetLoadAction::EClear);
	if (bRequiresSceneDepthAux)
	{
		BasePassRenderTargets[1] = FRenderTargetBinding(SceneTextures.DepthAux.Target, SceneTextures.DepthAux.Resolve, ERenderTargetLoadAction::EClear);
	}
	BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ? 
		FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) : 
		FDepthStencilBinding(SceneDepth, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
	BasePassRenderTargets.ShadingRateTexture = (!MainView.bIsSceneCapture && !MainView.bIsReflectionCapture && ShadingRateTarget.IsValid()) ? RegisterExternalTexture(GraphBuilder, ShadingRateTarget->GetRenderTargetItem().ShaderResourceTexture, TEXT("ShadingRateTexture")) : nullptr;
	BasePassRenderTargets.SubpassHint = ESubpassHint::DepthReadSubpass;
	if (!bIsFullDepthPrepassEnabled)
	{
		BasePassRenderTargets.NumOcclusionQueries = ComputeNumOcclusionQueriesToBatch();
	}

	//if the scenecolor isn't multiview but the app is, need to render as a single-view multiview due to shaders
	BasePassRenderTargets.MultiViewCount = MainView.bIsMobileMultiViewEnabled ? 2 : (bIsMultiViewApplication ? 1 : 0);

	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];

		SCOPED_GPU_MASK(GraphBuilder.RHICmdList, !View.IsInstancedStereoPass() ? View.GPUMask : (View.GPUMask | View.GetInstancedView()->GPUMask));
		SCOPED_CONDITIONAL_DRAW_EVENTF(GraphBuilder.RHICmdList, EventView, Views.Num() > 1, TEXT("View%d"), ViewIndex);

		if (!View.ShouldRenderView())
		{
			continue;
		}

		if (ViewIndex > 0)
		{
			BasePassRenderTargets[0].SetLoadAction(ERenderTargetLoadAction::ELoad);
			if (bRequiresSceneDepthAux)
			{
				BasePassRenderTargets[1].SetLoadAction(ERenderTargetLoadAction::ELoad);
			}
			BasePassRenderTargets.DepthStencil.SetDepthLoadAction(ERenderTargetLoadAction::ELoad);
			BasePassRenderTargets.DepthStencil.SetStencilLoadAction(ERenderTargetLoadAction::ELoad);
			BasePassRenderTargets.DepthStencil.SetDepthStencilAccess(bIsFullDepthPrepassEnabled ? FExclusiveDepthStencil::DepthRead_StencilWrite : FExclusiveDepthStencil::DepthWrite_StencilWrite);
		}

		View.BeginRenderView();

		UpdateDirectionalLightUniformBuffers(GraphBuilder, View);

		FMobileBasePassTextures MobileBasePassTextures{};
		MobileBasePassTextures.ScreenSpaceAO = bRequiresAmbientOcclusionPass ? SceneTextures.ScreenSpaceAO : SystemTextures.White;

		EMobileSceneTextureSetupMode SetupMode = EMobileSceneTextureSetupMode::CustomDepth;
		FMobileRenderPassParameters* PassParameters = GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
		PassParameters->View = View.GetShaderParameters();
		PassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Opaque, SetupMode, MobileBasePassTextures);
		PassParameters->RenderTargets = BasePassRenderTargets;
	
		BuildInstanceCullingDrawParams(GraphBuilder, View, PassParameters);

		// Split if we need to render translucency in a separate render pass
		if (bRequiresMultiPass)
		{
			RenderForwardMultiPass(GraphBuilder, PassParameters, BasePassRenderTargets, ViewIndex, View, SceneTextures);
		}
		else
		{
			RenderForwardSinglePass(GraphBuilder, PassParameters, ViewIndex, View, SceneTextures);
		}
	}
}

void FMobileSceneRenderer::RenderForwardSinglePass(FRDGBuilder& GraphBuilder, FMobileRenderPassParameters* PassParameters, int32 ViewIndex, FViewInfo& View, FSceneTextures& SceneTextures)
{
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SceneColorRendering"),
		PassParameters,
		// the second view pass should not be merged with the first view pass on mobile since the subpass would not work properly.
		ERDGPassFlags::Raster | ERDGPassFlags::NeverMerge,
		[this, PassParameters, ViewIndex, &View, &SceneTextures](FRHICommandListImmediate& RHICmdList)
	{
		if (GIsEditor && !View.bIsSceneCapture && ViewIndex == 0)
		{
			DrawClearQuad(RHICmdList, View.BackgroundColor);
		}

		// Depth pre-pass
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
		RenderMaskedPrePass(RHICmdList, View);
		// Opaque and masked
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
		RenderMobileBasePass(RHICmdList, View);
		RenderMobileDebugView(RHICmdList, View);
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
		const bool bAdrenoOcclusionMode = (CVarMobileAdrenoOcclusionMode.GetValueOnRenderThread() != 0 && IsOpenGLPlatform(ShaderPlatform));
		if (!bAdrenoOcclusionMode)
		{
			// Issue occlusion queries
			RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
			RenderOcclusion(RHICmdList, View);
		}
		PostRenderBasePass(RHICmdList, View);
		// scene depth is read only and can be fetched
		RHICmdList.NextSubpass();
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
		RenderDecals(RHICmdList, View);
		RenderModulatedShadowProjections(RHICmdList, ViewIndex, View);
		RenderFog(RHICmdList, View);
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
		if (bAdrenoOcclusionMode)
		{
			RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
			// flush
			RHICmdList.SubmitCommandsHint();
			// Issue occlusion queries
			RenderOcclusion(RHICmdList, View);
		}
		// Pre-tonemap before MSAA resolve (iOS only)
		PreTonemapMSAA(RHICmdList, SceneTextures);
	});
}

void FMobileSceneRenderer::RenderForwardMultiPass(FRDGBuilder& GraphBuilder, FMobileRenderPassParameters* PassParameters, FRenderTargetBindingSlots& BasePassRenderTargets, int32 ViewIndex, FViewInfo& View, FSceneTextures& SceneTextures)
{
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SceneColorRendering"),
		PassParameters,
		ERDGPassFlags::Raster,
		[this, PassParameters, ViewIndex, &View, &SceneTextures](FRHICommandListImmediate& RHICmdList)
	{
		if (GIsEditor && !View.bIsSceneCapture && ViewIndex == 0)
		{
			DrawClearQuad(RHICmdList, View.BackgroundColor);
		}

		// Depth pre-pass
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
		RenderMaskedPrePass(RHICmdList, View);
		// Opaque and masked
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
		RenderMobileBasePass(RHICmdList, View);
		RenderMobileDebugView(RHICmdList, View);
		// Issue occlusion queries
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
		RenderOcclusion(RHICmdList, View);
		PostRenderBasePass(RHICmdList, View);
	});

	// resolve MSAA depth for translucency
	AddResolveSceneDepthPass(GraphBuilder, View, SceneTextures.Depth);

	FExclusiveDepthStencil::Type ExclusiveDepthStencil = FExclusiveDepthStencil::DepthRead_StencilRead;
	if (bModulatedShadowsInUse)
	{
		// FIXME: modulated shadows write to stencil
		ExclusiveDepthStencil = FExclusiveDepthStencil::DepthRead_StencilWrite;
	}

	BasePassRenderTargets[0].SetLoadAction(ERenderTargetLoadAction::ELoad);
	if (bRequiresSceneDepthAux)
	{
		BasePassRenderTargets[1].SetLoadAction(ERenderTargetLoadAction::ELoad);
	}
	BasePassRenderTargets.DepthStencil.SetDepthLoadAction(ERenderTargetLoadAction::ELoad);
	BasePassRenderTargets.DepthStencil.SetStencilLoadAction(ERenderTargetLoadAction::ELoad);
	BasePassRenderTargets.DepthStencil.SetDepthStencilAccess(ExclusiveDepthStencil);
	BasePassRenderTargets.NumOcclusionQueries = 0;
	BasePassRenderTargets.SubpassHint = ESubpassHint::DepthReadSubpass;

	EMobileSceneTextureSetupMode SetupMode = EMobileSceneTextureSetupMode::SceneDepth | EMobileSceneTextureSetupMode::SceneDepthAux | EMobileSceneTextureSetupMode::CustomDepth;
	FMobileRenderPassParameters* SecondPassParameters = GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
	*SecondPassParameters = *PassParameters;
	SecondPassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Translucent, SetupMode);
	SecondPassParameters->RenderTargets = BasePassRenderTargets;

	GraphBuilder.AddPass(
		{},
		SecondPassParameters,
		ERDGPassFlags::Raster,
		[this, SecondPassParameters, ViewIndex, &View, &SceneTextures](FRHICommandListImmediate& RHICmdList)
	{
		// scene depth is read only and can be fetched
		RHICmdList.NextSubpass();
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Translucency));
		RenderDecals(RHICmdList, View);
		RenderModulatedShadowProjections(RHICmdList, ViewIndex, View);
		RenderFog(RHICmdList, View);
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
		// Pre-tonemap before MSAA resolve (iOS only)
		PreTonemapMSAA(RHICmdList, SceneTextures);
	});
}

class FMobileDeferredCopyPLSPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FMobileDeferredCopyPLSPS, Global);

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsMobilePlatform(Parameters.Platform) && IsMobileDeferredShadingEnabled(Parameters.Platform);
	}

	/** Default constructor. */
	FMobileDeferredCopyPLSPS() {}

	/** Initialization constructor. */
	FMobileDeferredCopyPLSPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
	}
};

IMPLEMENT_SHADER_TYPE(, FMobileDeferredCopyPLSPS, TEXT("/Engine/Private/MobileDeferredUtils.usf"), TEXT("MobileDeferredCopyPLSPS"), SF_Pixel);

class FMobileDeferredCopyDepthPS : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FMobileDeferredCopyDepthPS, Global);

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsMobilePlatform(Parameters.Platform) && IsMobileDeferredShadingEnabled(Parameters.Platform);
	}

	/** Default constructor. */
	FMobileDeferredCopyDepthPS() {}

	/** Initialization constructor. */
	FMobileDeferredCopyDepthPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
	}
};

IMPLEMENT_SHADER_TYPE(, FMobileDeferredCopyDepthPS, TEXT("/Engine/Private/MobileDeferredUtils.usf"), TEXT("MobileDeferredCopyDepthPS"), SF_Pixel);

template<class T>
void MobileDeferredCopyBuffer(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	// Shade only MSM_DefaultLit pixels
	uint8 StencilRef = GET_STENCIL_MOBILE_SM_MASK(MSM_DefaultLit);
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI(); // 4 bits for shading models

	TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
	TShaderMapRef<T> PixelShader(View.ShaderMap);

	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;
	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, StencilRef);

	DrawRectangle(
		RHICmdList,
		0, 0,
		View.ViewRect.Width(), View.ViewRect.Height(),
		View.ViewRect.Min.X, View.ViewRect.Min.Y,
		View.ViewRect.Width(), View.ViewRect.Height(),
		FIntPoint(View.ViewRect.Width(), View.ViewRect.Height()),
		FSceneTexturesConfig::Get().Extent,
		VertexShader);
}

void FMobileSceneRenderer::RenderDeferred(FRDGBuilder& GraphBuilder, const FSortedLightSetSceneInfo& SortedLightSet, FRDGTextureRef ViewFamilyTexture, FSceneTextures& SceneTextures)
{
	TArray<FRDGTextureRef, TInlineAllocator<5>> ColorTargets;

	// If we are using GL and don't have FBF support, use PLS
	bool bUsingPixelLocalStorage = IsAndroidOpenGLESPlatform(ShaderPlatform) && GSupportsPixelLocalStorage;

	if (bUsingPixelLocalStorage)
	{
		ColorTargets.Add(SceneTextures.Color.Target);
	}
	else
	{
		ColorTargets.Add(SceneTextures.Color.Target);
		ColorTargets.Add(SceneTextures.GBufferA);
		ColorTargets.Add(SceneTextures.GBufferB);
		ColorTargets.Add(SceneTextures.GBufferC);
		if(bRequiresSceneDepthAux)
		{
			ColorTargets.Add(SceneTextures.DepthAux.Target);
		}
	}

	TArrayView<FRDGTextureRef> BasePassTexturesView = MakeArrayView(ColorTargets);

	FRenderTargetBindingSlots BasePassRenderTargets = GetRenderTargetBindings(ERenderTargetLoadAction::ENoAction, BasePassTexturesView);
	BasePassRenderTargets.DepthStencil = bIsFullDepthPrepassEnabled ? 
		FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite) : 
		FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::EClear, ERenderTargetLoadAction::EClear, FExclusiveDepthStencil::DepthWrite_StencilWrite);
	BasePassRenderTargets.SubpassHint = ESubpassHint::DeferredShadingSubpass;
	if (!bIsFullDepthPrepassEnabled)
	{
		BasePassRenderTargets.NumOcclusionQueries = ComputeNumOcclusionQueriesToBatch();
	}
	BasePassRenderTargets.ShadingRateTexture = nullptr;
	BasePassRenderTargets.MultiViewCount = 0;

	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);

	int32 NumViews = Views.Num();

	for (int32 ViewIndex = 0; ViewIndex < NumViews; ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];

		SCOPED_GPU_MASK(GraphBuilder.RHICmdList, !View.IsInstancedStereoPass() ? View.GPUMask : (View.GPUMask | View.GetInstancedView()->GPUMask));
		SCOPED_CONDITIONAL_DRAW_EVENTF(GraphBuilder.RHICmdList, EventView, NumViews > 1, TEXT("View%d"), ViewIndex);

		if (!View.ShouldRenderView())
		{
			continue;
		}

		View.BeginRenderView();

		UpdateDirectionalLightUniformBuffers(GraphBuilder, View);

		FMobileBasePassTextures MobileBasePassTextures{};
		MobileBasePassTextures.ScreenSpaceAO = bRequiresAmbientOcclusionPass ? SceneTextures.ScreenSpaceAO : SystemTextures.White;

		EMobileSceneTextureSetupMode SetupMode = EMobileSceneTextureSetupMode::CustomDepth;
		auto* PassParameters = GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
		PassParameters->View = View.GetShaderParameters();
		PassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Opaque, SetupMode, MobileBasePassTextures);
		PassParameters->RenderTargets = BasePassRenderTargets;
		
		BuildInstanceCullingDrawParams(GraphBuilder, View, PassParameters);

		if (bRequiresMultiPass)
		{
			RenderDeferredMultiPass(GraphBuilder, PassParameters, BasePassRenderTargets, ColorTargets.Num(), ViewIndex, NumViews, View, SceneTextures, SortedLightSet);
		}
		else
		{
			RenderDeferredSinglePass(GraphBuilder, PassParameters, ViewIndex, NumViews, View, SceneTextures, SortedLightSet, bUsingPixelLocalStorage);
		}
	}
}

void FMobileSceneRenderer::RenderDeferredSinglePass(FRDGBuilder& GraphBuilder, class FMobileRenderPassParameters* PassParameters, int32 ViewIndex, int32 NumViews, FViewInfo& View, FSceneTextures& SceneTextures, const FSortedLightSetSceneInfo& SortedLightSet, bool bUsingPixelLocalStorage)
{
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SceneColorRendering"),
		PassParameters,
		ERDGPassFlags::Raster,
		[this, PassParameters, ViewIndex, NumViews, &View, &SceneTextures, &SortedLightSet, bUsingPixelLocalStorage](FRHICommandListImmediate& RHICmdList)
	{
		if (GIsEditor && !View.bIsSceneCapture)
		{
			DrawClearQuad(RHICmdList, View.BackgroundColor);
		}
		// Depth pre-pass
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
		RenderMaskedPrePass(RHICmdList, View);
		// Opaque and masked
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
		RenderMobileBasePass(RHICmdList, View);
		RHICmdList.ImmediateFlush(EImmediateFlushType::DispatchToRHIThread);
		
		// Issue occlusion queries
		if (!bUsingPixelLocalStorage)
		{
			RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
			RenderOcclusion(RHICmdList, View);
		}

		PostRenderBasePass(RHICmdList, View);
		// SceneColor + GBuffer write, SceneDepth is read only
		RHICmdList.NextSubpass();
		RenderDecals(RHICmdList, View);
		// SceneColor write, SceneDepth is read only
		RHICmdList.NextSubpass();
		MobileDeferredShadingPass(RHICmdList, ViewIndex, NumViews, View, *Scene, SortedLightSet);

		if (bUsingPixelLocalStorage)
		{
			MobileDeferredCopyBuffer<FMobileDeferredCopyPLSPS>(RHICmdList, View);
			// SceneColor write, SceneDepth is read only
			RHICmdList.NextSubpass();

			RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
			RenderOcclusion(RHICmdList, View);
		}
		RenderFog(RHICmdList, View);
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
	});
}

void FMobileSceneRenderer::RenderDeferredMultiPass(FRDGBuilder& GraphBuilder, class FMobileRenderPassParameters* PassParameters, FRenderTargetBindingSlots& BasePassRenderTargets, int32 NumColorTargets, int32 ViewIndex, int32 NumViews, FViewInfo& View, FSceneTextures& SceneTextures, const FSortedLightSetSceneInfo& SortedLightSet)
{
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("SceneColorRendering"),
		PassParameters,
		ERDGPassFlags::Raster,
		[this, PassParameters, &View, &SceneTextures](FRHICommandListImmediate& RHICmdList)
	{
		if (GIsEditor && !View.bIsSceneCapture)
		{
			DrawClearQuad(RHICmdList, View.BackgroundColor);
		}

		// Depth pre-pass
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLM_MobilePrePass));
		RenderMaskedPrePass(RHICmdList, View);
		// Opaque and masked
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Opaque));
		RenderMobileBasePass(RHICmdList, View);
		// Issue occlusion queries
		RHICmdList.SetCurrentStat(GET_STATID(STAT_CLMM_Occlusion));
		RenderOcclusion(RHICmdList, View);
		PostRenderBasePass(RHICmdList, View);
	});

	// SceneColor + GBuffer write, SceneDepth is read only
	for (int32 i = 0; i < NumColorTargets; ++i)
	{
		BasePassRenderTargets[i].SetLoadAction(ERenderTargetLoadAction::ELoad);
	}

	BasePassRenderTargets.DepthStencil.SetDepthLoadAction(ERenderTargetLoadAction::ELoad);
	BasePassRenderTargets.DepthStencil.SetStencilLoadAction(ERenderTargetLoadAction::ELoad);
	BasePassRenderTargets.DepthStencil.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilRead);
	BasePassRenderTargets.SubpassHint = ESubpassHint::None;
	BasePassRenderTargets.NumOcclusionQueries = 0;

	EMobileSceneTextureSetupMode SetupMode = EMobileSceneTextureSetupMode::SceneDepth | EMobileSceneTextureSetupMode::SceneDepthAux | EMobileSceneTextureSetupMode::CustomDepth;
	auto* SecondPassParameters = GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
	*SecondPassParameters = *PassParameters;
	SecondPassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Translucent, SetupMode);
	SecondPassParameters->RenderTargets = BasePassRenderTargets;

	GraphBuilder.AddPass(
		{},
		SecondPassParameters,
		ERDGPassFlags::Raster,
		[this, SecondPassParameters, &View](FRHICommandListImmediate& RHICmdList)
	{
		RHICmdList.NextSubpass();
		RenderDecals(RHICmdList, View);
	});

	// SceneColor write, SceneDepth is read only
	for (int32 i = 1; i < NumColorTargets; ++i)
	{
		BasePassRenderTargets[i] = FRenderTargetBinding();
	}
	BasePassRenderTargets.DepthStencil.SetDepthStencilAccess(FExclusiveDepthStencil::DepthRead_StencilWrite);

	SetupMode = EMobileSceneTextureSetupMode::SceneDepth | EMobileSceneTextureSetupMode::SceneDepthAux | EMobileSceneTextureSetupMode::GBuffers | EMobileSceneTextureSetupMode::CustomDepth;
	auto* ThirdPassParameters = GraphBuilder.AllocParameters<FMobileRenderPassParameters>();
	*ThirdPassParameters = *PassParameters;
	ThirdPassParameters->MobileBasePass = CreateMobileBasePassUniformBuffer(GraphBuilder, View, EMobileBasePass::Translucent, SetupMode);
	ThirdPassParameters->RenderTargets = BasePassRenderTargets;

	GraphBuilder.AddPass(
		{},
		ThirdPassParameters,
		ERDGPassFlags::Raster,
		[this, ThirdPassParameters, ViewIndex, NumViews, &View, &SceneTextures, &SortedLightSet](FRHICommandListImmediate& RHICmdList)
	{
		RHICmdList.NextSubpass();
		MobileDeferredShadingPass(RHICmdList, ViewIndex, NumViews, View, *Scene, SortedLightSet);
		RenderFog(RHICmdList, View);
		// Draw translucency.
		RenderTranslucency(RHICmdList, View);
	});
}

void FMobileSceneRenderer::PostRenderBasePass(FRHICommandListImmediate& RHICmdList, FViewInfo& View)
{
	if (ViewFamily.ViewExtensions.Num() > 1)
	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(ViewExtensionPostRenderBasePass);
		QUICK_SCOPE_CYCLE_COUNTER(STAT_FMobileSceneRenderer_ViewExtensionPostRenderBasePass);
		for (int32 ViewExt = 0; ViewExt < ViewFamily.ViewExtensions.Num(); ++ViewExt)
		{
			ViewFamily.ViewExtensions[ViewExt]->PostRenderBasePass_RenderThread(RHICmdList, View);
		}
	}
}

void FMobileSceneRenderer::RenderMobileDebugView(FRHICommandListImmediate& RHICmdList, const FViewInfo& View)
{
#if WITH_DEBUG_VIEW_MODES
	if (ViewFamily.UseDebugViewPS())
	{
		CSV_SCOPED_TIMING_STAT_EXCLUSIVE(RenderDebugView);
		SCOPED_DRAW_EVENT(RHICmdList, MobileDebugView);
		SCOPE_CYCLE_COUNTER(STAT_BasePassDrawTime);

		// Here we use the base pass depth result to get z culling for opaque and masque.
		// The color needs to be cleared at this point since shader complexity renders in additive.
		DrawClearQuad(RHICmdList, FLinearColor::Black);

		RHICmdList.SetViewport(View.ViewRect.Min.X, View.ViewRect.Min.Y, 0, View.ViewRect.Max.X, View.ViewRect.Max.Y, 1);
		View.ParallelMeshDrawCommandPasses[EMeshPass::DebugViewMode].DispatchDraw(nullptr, RHICmdList, &MeshPassInstanceCullingDrawParams[EMeshPass::DebugViewMode]);
	}
#endif // WITH_DEBUG_VIEW_MODES
}

int32 FMobileSceneRenderer::ComputeNumOcclusionQueriesToBatch() const
{
	int32 NumQueriesForBatch = 0;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];
		const FSceneViewState* ViewState = (FSceneViewState*)View.State;
#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
		if (!ViewState || (!ViewState->HasViewParent() && !ViewState->bIsFrozen))
#endif
		{
			NumQueriesForBatch += View.IndividualOcclusionQueries.GetNumBatchOcclusionQueries();
			NumQueriesForBatch += View.GroupedOcclusionQueries.GetNumBatchOcclusionQueries();
		}
	}
	
	return NumQueriesForBatch;
}

// Whether we need a separate render-passes for translucency, decals etc
bool FMobileSceneRenderer::RequiresMultiPass(FRHICommandListImmediate& RHICmdList, const FViewInfo& View) const
{
	// Vulkan uses subpasses
	if (IsVulkanPlatform(ShaderPlatform))
	{
		return false;
	}

	// All iOS support frame_buffer_fetch
	if (IsMetalMobilePlatform(ShaderPlatform) && GSupportsShaderFramebufferFetch)
	{
		return false;
	}

	// Some Androids support frame_buffer_fetch
	if (IsAndroidOpenGLESPlatform(ShaderPlatform) && (GSupportsShaderFramebufferFetch || GSupportsShaderDepthStencilFetch))
	{
		return false;
	}

	// Only Vulkan, iOS and some GL can do a single pass deferred shading, otherwise multipass
	if (IsMobileDeferredShadingEnabled(ShaderPlatform))
	{
		return true;
	}
		
	// Always render LDR in single pass
	if (!IsMobileHDR())
	{
		return false;
	}

	// MSAA depth can't be sampled or resolved, unless we are on PC (no vulkan)
	if (NumMSAASamples > 1 && !IsSimulatedPlatform(ShaderPlatform))
	{
		return false;
	}

	return true;
}

void FMobileSceneRenderer::UpdateDirectionalLightUniformBuffers(FRDGBuilder& GraphBuilder, const FViewInfo& View)
{
	if (CachedView == &View)
	{
		return;
	}
	CachedView = &View;

	AddPass(GraphBuilder, RDG_EVENT_NAME("UpdateDirectionalLightUniformBuffers"), [this, &View](FRHICommandListImmediate&)
	{
		const bool bDynamicShadows = ViewFamily.EngineShowFlags.DynamicShadows;
		// Fill in the other entries based on the lights
		for (int32 ChannelIdx = 0; ChannelIdx < UE_ARRAY_COUNT(Scene->MobileDirectionalLights); ChannelIdx++)
		{
			FMobileDirectionalLightShaderParameters Params;
			SetupMobileDirectionalLightUniformParameters(*Scene, View, VisibleLightInfos, ChannelIdx, bDynamicShadows, Params);
			Scene->UniformBuffers.MobileDirectionalLightUniformBuffers[ChannelIdx + 1].UpdateUniformBufferImmediate(Params);
		}
	});
}

void FMobileSceneRenderer::UpdateSkyReflectionUniformBuffer()
{
	FSkyLightSceneProxy* SkyLight = nullptr;
	if (Scene->SkyLight
		&& Scene->SkyLight->ProcessedTexture
		&& Scene->SkyLight->ProcessedTexture->TextureRHI
		// Don't use skylight reflection if it is a static sky light for keeping coherence with PC.
		&& !Scene->SkyLight->bHasStaticLighting)
	{
		SkyLight = Scene->SkyLight;
	}

	FMobileReflectionCaptureShaderParameters Parameters;
	SetupMobileSkyReflectionUniformParameters(SkyLight, Parameters);
	Scene->UniformBuffers.MobileSkyReflectionUniformBuffer.UpdateUniformBufferImmediate(Parameters);
}

class FPreTonemapMSAA_Mobile : public FGlobalShader
{
	DECLARE_SHADER_TYPE(FPreTonemapMSAA_Mobile, Global);

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsMetalMobilePlatform(Parameters.Platform);
	}	

	FPreTonemapMSAA_Mobile() {}

public:
	FPreTonemapMSAA_Mobile(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FGlobalShader(Initializer)
	{
	}
};

IMPLEMENT_SHADER_TYPE(, FPreTonemapMSAA_Mobile,TEXT("/Engine/Private/PostProcessMobile.usf"),TEXT("PreTonemapMSAA_Mobile"),SF_Pixel);

void FMobileSceneRenderer::PreTonemapMSAA(FRHICommandListImmediate& RHICmdList, const FMinimalSceneTextures& SceneTextures)
{
	// iOS only
	bool bOnChipPP = GSupportsRenderTargetFormat_PF_FloatRGBA && GSupportsShaderFramebufferFetch &&	ViewFamily.EngineShowFlags.PostProcessing;
	bool bOnChipPreTonemapMSAA = bOnChipPP && IsMetalMobilePlatform(ViewFamily.GetShaderPlatform()) && (NumMSAASamples > 1);
	if (!bOnChipPreTonemapMSAA || bGammaSpace)
	{
		return;
	}

	const FIntPoint TargetSize = SceneTextures.Config.Extent;

	const auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
	TShaderMapRef<FPreTonemapMSAA_Mobile> PixelShader(ShaderMap);

	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	RHICmdList.SetViewport(0, 0, 0.0f, TargetSize.X, TargetSize.Y, 1.0f);

	DrawRectangle(
		RHICmdList,
		0, 0,
		TargetSize.X, TargetSize.Y,
		0, 0,
		TargetSize.X, TargetSize.Y,
		TargetSize,
		TargetSize,
		VertexShader,
		EDRF_UseTriangleOptimization);
}

/** Before SetupMobileBasePassAfterShadowInit, we need to update the uniform buffer and shadow info for all movable point lights.*/
void FMobileSceneRenderer::UpdateMovablePointLightUniformBufferAndShadowInfo()
{
	static auto* MobileNumDynamicPointLightsCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MobileNumDynamicPointLights"));
	const int32 MobileNumDynamicPointLights = MobileNumDynamicPointLightsCVar->GetValueOnRenderThread();

	static auto* MobileEnableMovableSpotlightsCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.EnableMovableSpotlights"));
	const int32 MobileEnableMovableSpotlights = MobileEnableMovableSpotlightsCVar->GetValueOnRenderThread();

	static auto* EnableMovableSpotlightShadowsCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.EnableMovableSpotlightsShadow"));
	const int32 EnableMovableSpotlightShadows = EnableMovableSpotlightShadowsCVar->GetValueOnRenderThread();

	if (MobileNumDynamicPointLights > 0)
	{
		bool bShouldDynamicShadows = ViewFamily.EngineShowFlags.DynamicShadows
			&& !IsSimpleForwardShadingEnabled(ShaderPlatform)
			&& GetShadowQuality() > 0
			&& EnableMovableSpotlightShadows != 0;

		for (auto LightIt = Scene->Lights.CreateConstIterator(); LightIt; ++LightIt)
		{
			const FLightSceneInfoCompact& LightSceneInfoCompact = *LightIt;
			FLightSceneInfo* LightSceneInfo = LightSceneInfoCompact.LightSceneInfo;

			FLightSceneProxy* LightProxy = LightSceneInfo->Proxy;
			const uint8 LightType = LightProxy->GetLightType();

			const bool bIsValidLightType =
				LightType == LightType_Point
				|| LightType == LightType_Rect
				|| (LightType == LightType_Spot && MobileEnableMovableSpotlights);

			if (bIsValidLightType && LightProxy->IsMovable())
			{
				LightSceneInfo->ConditionalUpdateMobileMovablePointLightUniformBuffer(this);

				bool bDynamicShadows = bShouldDynamicShadows
					&& LightType == LightType_Spot
					&& VisibleLightInfos[LightSceneInfo->Id].AllProjectedShadows.Num() > 0
					&& VisibleLightInfos[LightSceneInfo->Id].AllProjectedShadows.Last()->bAllocated;

				if (bDynamicShadows)
				{
					FProjectedShadowInfo *ProjectedShadowInfo = VisibleLightInfos[LightSceneInfo->Id].AllProjectedShadows.Last();
					checkSlow(ProjectedShadowInfo && ProjectedShadowInfo->CacheMode != SDCM_StaticPrimitivesOnly);

					const FIntPoint ShadowBufferResolution = ProjectedShadowInfo->GetShadowBufferResolution();
					
					for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
					{
						FViewInfo& View = Views[ViewIndex];

						FMobileMovableSpotLightsShadowInfo& MobileMovableSpotLightsShadowInfo = View.MobileMovableSpotLightsShadowInfo;

						checkSlow(MobileMovableSpotLightsShadowInfo.ShadowDepthTexture == nullptr
							|| MobileMovableSpotLightsShadowInfo.ShadowDepthTexture == ProjectedShadowInfo->RenderTargets.DepthTarget->GetRenderTargetItem().ShaderResourceTexture.GetReference());

						if (MobileMovableSpotLightsShadowInfo.ShadowDepthTexture == nullptr)
						{
							MobileMovableSpotLightsShadowInfo.ShadowDepthTexture = ProjectedShadowInfo->RenderTargets.DepthTarget->GetRenderTargetItem().ShaderResourceTexture.GetReference();
							MobileMovableSpotLightsShadowInfo.ShadowBufferSize = FVector4f(ShadowBufferResolution.X, ShadowBufferResolution.Y, 1.0f / ShadowBufferResolution.X, 1.0f / ShadowBufferResolution.Y);
						}
					}
				}
			}
		}
	}
}

bool FMobileSceneRenderer::ShouldRenderHZB()
{
	static const auto MobileAmbientOcclusionTechniqueCVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.AmbientOcclusionTechnique"));

	// Mobile SSAO requests HZB
	bool bIsFeatureRequested = bRequiresAmbientOcclusionPass && MobileAmbientOcclusionTechniqueCVar->GetValueOnRenderThread() == 1;

	bool bNeedsHZB = bIsFeatureRequested;

	return bNeedsHZB;
}

void FMobileSceneRenderer::RenderHZB(FRHICommandListImmediate& RHICmdList, const TRefCountPtr<IPooledRenderTarget>& SceneDepthZ)
{
	checkSlow(bShouldRenderHZB);

	FRDGBuilder GraphBuilder(RHICmdList);
	{
		FRDGTextureRef SceneDepthTexture = GraphBuilder.RegisterExternalTexture(SceneDepthZ, TEXT("SceneDepthTexture"));

		RenderHZB(GraphBuilder, SceneDepthTexture);
	}
	GraphBuilder.Execute();
}

void FMobileSceneRenderer::RenderHZB(FRDGBuilder& GraphBuilder, FRDGTextureRef SceneDepthTexture)
{
	RDG_GPU_STAT_SCOPE(GraphBuilder, HZB);

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];

		RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
		{
			RDG_EVENT_SCOPE(GraphBuilder, "BuildHZB(ViewId=%d)", ViewIndex);
			
			FRDGTextureRef FurthestHZBTexture = nullptr;

			BuildHZBFurthest(
				GraphBuilder,
				SceneDepthTexture,
				/* VisBufferTexture = */ nullptr,
				View.ViewRect,
				View.GetFeatureLevel(),
				View.GetShaderPlatform(),
				TEXT("MobileHZBFurthest"),
				&FurthestHZBTexture);

			View.HZBMipmap0Size = FurthestHZBTexture->Desc.Extent;
			View.HZB = FurthestHZBTexture;
		}
	}
}
