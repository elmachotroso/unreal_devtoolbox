// Copyright Epic Games, Inc. All Rights Reserved.

#include "VelocityRendering.h"
#include "SceneUtils.h"
#include "Materials/Material.h"
#include "PostProcess/SceneRenderTargets.h"
#include "MaterialShaderType.h"
#include "MaterialShader.h"
#include "MeshMaterialShader.h"
#include "ShaderBaseClasses.h"
#include "SceneRendering.h"
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"
#include "ScreenSpaceRayTracing.h"
#include "PostProcess/PostProcessMotionBlur.h"
#include "UnrealEngine.h"
#if WITH_EDITOR
#include "Misc/CoreMisc.h"
#include "Interfaces/ITargetPlatform.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#endif
#include "VisualizeTexture.h"
#include "MeshPassProcessor.inl"
#include "DebugProbeRendering.h"
#include "RendererModule.h"

// Changing this causes a full shader recompile
static TAutoConsoleVariable<int32> CVarVelocityOutputPass(
	TEXT("r.VelocityOutputPass"),
	0,
	TEXT("When to write velocity buffer.\n") \
	TEXT(" 0: Renders during the depth pass. This splits the depth pass into 2 phases: with and without velocity.\n") \
	TEXT(" 1: Renders during the regular base pass. This adds an extra GBuffer target during base pass rendering.") \
	TEXT(" 2: Renders after the regular base pass.\n"), \
	ECVF_ReadOnly | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarBasePassOutputsVelocity(
	TEXT("r.BasePassOutputsVelocity"),
	-1,
	TEXT("Deprecated CVar. Use r.VelocityOutputPass instead.\n"),
	ECVF_ReadOnly | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarVertexDeformationOutputsVelocity(
	TEXT("r.VertexDeformationOutputsVelocity"),
	-1,
	TEXT("Deprecated CVar. Use r.Velocity.EnableVertexDeformation instead.\n"),
	ECVF_ReadOnly | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarParallelVelocity(
	TEXT("r.ParallelVelocity"),
	1,  
	TEXT("Toggles parallel velocity rendering. Parallel rendering must be enabled for this to have an effect."),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarRHICmdFlushRenderThreadTasksVelocityPass(
	TEXT("r.RHICmdFlushRenderThreadTasksVelocityPass"),
	0,
	TEXT("Wait for completion of parallel render thread tasks at the end of the velocity pass.  A more granular version of r.RHICmdFlushRenderThreadTasks. If either r.RHICmdFlushRenderThreadTasks or r.RHICmdFlushRenderThreadTasksVelocityPass is > 0 we will flush."));

DECLARE_GPU_STAT_NAMED(RenderVelocities, TEXT("Render Velocities"));

/** Validate that deprecated CVars are no longer set. */
inline void ValidateVelocityCVars()
{
#if !UE_BUILD_SHIPPING
	static bool bHasValidatedCVars = false;
	if (!bHasValidatedCVars)
	{
		{
			const int32 Value = CVarBasePassOutputsVelocity.GetValueOnAnyThread();
			if (Value != -1)
			{
				UE_LOG(LogRenderer, Warning, TEXT("Deprectaed CVar r.BasePassOutputsVelocity is set to %d. Remove and use r.VelocityOutputPass instead."), Value);
			}
		}
		{
			const int32 Value = CVarVertexDeformationOutputsVelocity.GetValueOnAnyThread();
			if (Value != -1)
			{
				UE_LOG(LogRenderer, Warning, TEXT("Deprectaed CVar r.VertexDeformationOutputsVelocity is set to %d. Remove and use r.Velocity.EnableVertexDeformation instead."), Value);
			}
		}
		bHasValidatedCVars = true;
	}
#endif
}

class FVelocityVS : public FMeshMaterialShader
{
public:
	DECLARE_SHADER_TYPE(FVelocityVS, MeshMaterial);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		// Compile for default material.
		const bool bIsDefault = Parameters.MaterialParameters.bIsSpecialEngineMaterial;

		// Compile for masked materials.
		const bool bIsMasked = !Parameters.MaterialParameters.bWritesEveryPixel;

		// Compile for opaque and two-sided materials.
		const bool bIsOpaqueAndTwoSided = (Parameters.MaterialParameters.bIsTwoSided && !IsTranslucentBlendMode(Parameters.MaterialParameters.BlendMode));

		// Compile for materials which modify meshes.
		const bool bMayModifyMeshes = Parameters.MaterialParameters.bMaterialMayModifyMeshPosition;

		const bool bHasPlatformSupport = PlatformSupportsVelocityRendering(Parameters.Platform);

		/**
		 * Any material with a vertex factory incompatible with base pass velocity generation must generate
		 * permutations for this shader. Shaders which don't fall into this set are considered "simple" enough
		 * to swap against the default material. This massively simplifies the calculations.
		 */
		const bool bIsSeparateVelocityPassRequired = (bIsDefault || bIsMasked || bIsOpaqueAndTwoSided || bMayModifyMeshes) &&
			FVelocityRendering::IsSeparateVelocityPassRequiredByVertexFactory(Parameters.Platform, Parameters.VertexFactoryType->SupportsStaticLighting());

		// The material may explicitly override and request that it be rendered into the velocity pass.
		const bool bIsSeparateVelocityPassRequiredByMaterial = Parameters.MaterialParameters.bIsTranslucencyWritingVelocity;

		return bHasPlatformSupport && (bIsSeparateVelocityPassRequired || bIsSeparateVelocityPassRequiredByMaterial);
	}

	FVelocityVS() = default;
	FVelocityVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{}
};

class FVelocityPS : public FMeshMaterialShader
{
public:
	DECLARE_SHADER_TYPE(FVelocityPS, MeshMaterial);

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return FVelocityVS::ShouldCompilePermutation(Parameters);
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetRenderTargetOutputFormat(0, PF_A16B16G16R16);

		// We support velocity on thin trnaslucent only with masking, and only if the material is only made of thin translucent shading model.
		OutEnvironment.SetDefine(TEXT("VELOCITY_THIN_TRANSLUCENT_MODE"), Parameters.MaterialParameters.ShadingModels.HasOnlyShadingModel(MSM_ThinTranslucent));
	}

	FVelocityPS() = default;
	FVelocityPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{}
};

IMPLEMENT_SHADER_TYPE(,FVelocityVS, TEXT("/Engine/Private/VelocityShader.usf"), TEXT("MainVertexShader"), SF_Vertex);
IMPLEMENT_SHADER_TYPE(,FVelocityPS, TEXT("/Engine/Private/VelocityShader.usf"), TEXT("MainPixelShader"), SF_Pixel);
IMPLEMENT_SHADERPIPELINE_TYPE_VSPS(VelocityPipeline, FVelocityVS, FVelocityPS, true);

EMeshPass::Type GetMeshPassFromVelocityPass(EVelocityPass VelocityPass)
{
	switch (VelocityPass)
	{
	case EVelocityPass::Opaque:
		return EMeshPass::Velocity;
	case EVelocityPass::Translucent:
		return EMeshPass::TranslucentVelocity;
	}
	check(false);
	return EMeshPass::Velocity;
}

DECLARE_CYCLE_STAT(TEXT("Velocity"), STAT_CLP_Velocity, STATGROUP_ParallelCommandListMarkers);

bool FDeferredShadingSceneRenderer::ShouldRenderVelocities() const
{
	if (!FVelocityRendering::IsVelocityPassSupported(ShaderPlatform) || ViewFamily.UseDebugViewPS())
	{
		return false;
	}
	if (FVelocityRendering::DepthPassCanOutputVelocity(Scene->GetFeatureLevel()))
	{
		// Always render velocity when it is part of the depth pass to avoid dropping things from the depth pass.
		// This means that we will pay the cost of velocity in the pass even if we don't really need it according to the view logic below.
		// But requiring velocity is by far the most common case.
		// And the alternative approach is for the depth pass to also incorporate the logic below to avoid dropping velocity primitives.
		return true;
	}

	bool bNeedsVelocity = false;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];

		bool bTemporalAA = IsTemporalAccumulationBasedMethod(View.AntiAliasingMethod) && !View.bCameraCut;
		bool bMotionBlur = IsMotionBlurEnabled(View);
		bool bVisualizeMotionblur = View.Family->EngineShowFlags.VisualizeMotionBlur;
		bool bDistanceFieldAO = ShouldPrepareForDistanceFieldAO();

		bool bSSRTemporal = ScreenSpaceRayTracing::ShouldRenderScreenSpaceReflections(View) && ScreenSpaceRayTracing::IsSSRTemporalPassRequired(View);

		bool bRayTracing = IsRayTracingEnabled();
		bool bDenoise = bRayTracing;

		const FPerViewPipelineState& ViewPipelineState = GetViewPipelineState(View);

		bool bSSGI = ViewPipelineState.DiffuseIndirectMethod == EDiffuseIndirectMethod::SSGI;
		bool bLumen = ViewPipelineState.DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen || ViewPipelineState.ReflectionsMethod == EReflectionsMethod::Lumen;
		
		bNeedsVelocity |= bVisualizeMotionblur || bMotionBlur || bTemporalAA || bDistanceFieldAO || bSSRTemporal || bDenoise || bSSGI || bLumen;
	}

	return bNeedsVelocity;
}

bool FMobileSceneRenderer::ShouldRenderVelocities() const
{
	if (!FVelocityRendering::IsVelocityPassSupported(ShaderPlatform) || ViewFamily.UseDebugViewPS() || !PlatformSupportsVelocityRendering(ShaderPlatform))
	{
		return false;
	}

	bool bNeedsVelocity = false;
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		const FViewInfo& View = Views[ViewIndex];

		bool bTemporalAA = IsTemporalAccumulationBasedMethod(View.AntiAliasingMethod) && !View.bCameraCut;

		bNeedsVelocity |= bTemporalAA;
	}

	return bNeedsVelocity;
}

BEGIN_SHADER_PARAMETER_STRUCT(FVelocityPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FViewShaderParameters, View)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
	SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

void FSceneRenderer::RenderVelocities(
	FRDGBuilder& GraphBuilder,
	const FSceneTextures& SceneTextures,
	EVelocityPass VelocityPass,
	bool bForceVelocity)
{
	if (!ShouldRenderVelocities())
	{
		return;
	}

	RDG_CSV_STAT_EXCLUSIVE_SCOPE(GraphBuilder, RenderVelocities);
	SCOPED_NAMED_EVENT(FSceneRenderer_RenderVelocities, FColor::Emerald);
	SCOPE_CYCLE_COUNTER(STAT_RenderVelocities);

	ERenderTargetLoadAction VelocityLoadAction = HasBeenProduced(SceneTextures.Velocity)
		? ERenderTargetLoadAction::ELoad
		: ERenderTargetLoadAction::EClear;

	RDG_GPU_STAT_SCOPE(GraphBuilder, RenderVelocities);
	RDG_WAIT_FOR_TASKS_CONDITIONAL(GraphBuilder, FVelocityRendering::IsVelocityWaitForTasksEnabled(ShaderPlatform));

	const EMeshPass::Type MeshPass = GetMeshPassFromVelocityPass(VelocityPass);
	FExclusiveDepthStencil ExclusiveDepthStencil = (VelocityPass == EVelocityPass::Opaque && !(Scene->EarlyZPassMode == DDM_AllOpaqueNoVelocity))
														? FExclusiveDepthStencil::DepthRead_StencilWrite
														: FExclusiveDepthStencil::DepthWrite_StencilWrite;

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];

		if (View.ShouldRenderView())
		{
			FParallelMeshDrawCommandPass& ParallelMeshPass = View.ParallelMeshDrawCommandPasses[MeshPass];

			const bool bHasAnyDraw = ParallelMeshPass.HasAnyDraw();
			if (!bHasAnyDraw && !bForceVelocity)
			{
				continue;
			}

			RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

			const bool bIsParallelVelocity = FVelocityRendering::IsParallelVelocity(ShaderPlatform);

			// Clear velocity render target explicitly when velocity rendering in parallel or no draw but force to.
			// Avoid adding a separate clear pass in non parallel rendering.
			const bool bExplicitlyClearVelocity = (VelocityLoadAction == ERenderTargetLoadAction::EClear) && (bIsParallelVelocity || (bForceVelocity && !bHasAnyDraw));

			if (bExplicitlyClearVelocity)
			{
				AddClearRenderTargetPass(GraphBuilder, SceneTextures.Velocity);

				// Parallel render need to use Load action in any case.
				VelocityLoadAction = ERenderTargetLoadAction::ELoad;
			}

			VelocityLoadAction = View.DecayLoadAction(VelocityLoadAction);

			if (!bHasAnyDraw)
			{
				continue;
			}

			View.BeginRenderView();

			FVelocityPassParameters* PassParameters = GraphBuilder.AllocParameters<FVelocityPassParameters>();
			PassParameters->View = View.GetShaderParameters();
			ParallelMeshPass.BuildRenderingCommands(GraphBuilder, Scene->GPUScene, PassParameters->InstanceCullingDrawParams);
			PassParameters->SceneTextures = SceneTextures.UniformBuffer;
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
				SceneTextures.Depth.Resolve,
				ERenderTargetLoadAction::ELoad,
				ERenderTargetLoadAction::ELoad,
				ExclusiveDepthStencil);

			PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneTextures.Velocity, ViewIndex > 0 ? ERenderTargetLoadAction::ELoad : VelocityLoadAction);

			if (bIsParallelVelocity)
			{
				GraphBuilder.AddPass(
					RDG_EVENT_NAME("VelocityParallel"),
					PassParameters,
					ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass,
					[this, &View, &ParallelMeshPass, VelocityPass, PassParameters](FRHICommandListImmediate& RHICmdList)
				{
					FRDGParallelCommandListSet ParallelCommandListSet(RHICmdList, GET_STATID(STAT_CLP_Velocity), *this, View, FParallelCommandListBindings(PassParameters));
					ParallelMeshPass.DispatchDraw(&ParallelCommandListSet, RHICmdList, &PassParameters->InstanceCullingDrawParams);
				});
			}
			else
			{

				GraphBuilder.AddPass(
					RDG_EVENT_NAME("Velocity"),
					PassParameters,
					ERDGPassFlags::Raster,
					[this, &View, &ParallelMeshPass, PassParameters](FRHICommandList& RHICmdList)
				{
					SetStereoViewport(RHICmdList, View);
					ParallelMeshPass.DispatchDraw(nullptr, RHICmdList, &PassParameters->InstanceCullingDrawParams);
				});
			}
		}
	}

#if !(UE_BUILD_SHIPPING)
	const bool bForwardShadingEnabled = IsForwardShadingEnabled(ShaderPlatform);
	if (!bForwardShadingEnabled)
	{
		FRenderTargetBindingSlots VelocityRenderTargets;
		VelocityRenderTargets[0] = FRenderTargetBinding(SceneTextures.Velocity, VelocityLoadAction);
		VelocityRenderTargets.DepthStencil = FDepthStencilBinding(
			SceneTextures.Depth.Resolve,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ELoad,
			ExclusiveDepthStencil);

		StampDeferredDebugProbeVelocityPS(GraphBuilder, Views, VelocityRenderTargets);
	}
#endif


}

EPixelFormat FVelocityRendering::GetFormat(EShaderPlatform ShaderPlatform)
{
	// Lumen needs velocity depth
	const bool bNeedVelocityDepth = (DoesProjectSupportDistanceFields() && FDataDrivenShaderPlatformInfo::GetSupportsLumenGI(ShaderPlatform)) 
		|| FDataDrivenShaderPlatformInfo::GetSupportsRayTracing(ShaderPlatform);

	// Android platform doesn't support unorm G16R16 format, use G16R16F instead.
	return bNeedVelocityDepth ? PF_A16B16G16R16 : (IsAndroidOpenGLESPlatform(ShaderPlatform) ? PF_G16R16F : PF_G16R16);
}

FRDGTextureDesc FVelocityRendering::GetRenderTargetDesc(EShaderPlatform ShaderPlatform, FIntPoint Extent)
{
	const ETextureCreateFlags FastVRamFlag = BasePassCanOutputVelocity(ShaderPlatform) ? GFastVRamConfig.GBufferVelocity : TexCreate_None;
	return FRDGTextureDesc::Create2D(Extent, GetFormat(ShaderPlatform), FClearValueBinding::Transparent, TexCreate_RenderTargetable | TexCreate_UAV | TexCreate_ShaderResource | FastVRamFlag);
}

bool FVelocityRendering::IsVelocityPassSupported(EShaderPlatform ShaderPlatform)
{
	ValidateVelocityCVars();

	return GPixelFormats[GetFormat(ShaderPlatform)].Supported;
}

bool FVelocityRendering::DepthPassCanOutputVelocity(ERHIFeatureLevel::Type FeatureLevel)
{
	static bool bRequestedDepthPassVelocity = CVarVelocityOutputPass.GetValueOnAnyThread() == 0;
	const bool bMSAAEnabled = GetDefaultMSAACount(FeatureLevel) > 1;
	return !bMSAAEnabled && bRequestedDepthPassVelocity;
}

bool FVelocityRendering::BasePassCanOutputVelocity(EShaderPlatform ShaderPlatform)
{
	return IsUsingBasePassVelocity(ShaderPlatform);
}

bool FVelocityRendering::BasePassCanOutputVelocity(ERHIFeatureLevel::Type FeatureLevel)
{
	EShaderPlatform ShaderPlatform = GetFeatureLevelShaderPlatform(FeatureLevel);
	return BasePassCanOutputVelocity(ShaderPlatform);
}

bool FVelocityRendering::IsSeparateVelocityPassRequiredByVertexFactory(EShaderPlatform ShaderPlatform, bool bVertexFactoryUsesStaticLighting)
{
	// A separate pass is required if the base pass can't do it.
	const bool bBasePassVelocityNotSupported = !BasePassCanOutputVelocity(ShaderPlatform);

	// Meshes with static lighting need a separate velocity pass, but only if we are using selective render target outputs.
	const bool bVertexFactoryRequiresSeparateVelocityPass = IsUsingSelectiveBasePassOutputs(ShaderPlatform) && bVertexFactoryUsesStaticLighting;

	return bBasePassVelocityNotSupported || bVertexFactoryRequiresSeparateVelocityPass;
}

bool FVelocityRendering::IsParallelVelocity(EShaderPlatform ShaderPlatform)
{
	return GRHICommandList.UseParallelAlgorithms() && CVarParallelVelocity.GetValueOnRenderThread()
		// Parallel dispatch is not supported on mobile platform
		&& !IsMobilePlatform(ShaderPlatform);
}

bool FVelocityRendering::IsVelocityWaitForTasksEnabled(EShaderPlatform ShaderPlatform)
{
	return FVelocityRendering::IsParallelVelocity(ShaderPlatform) && (CVarRHICmdFlushRenderThreadTasksVelocityPass.GetValueOnRenderThread() > 0 || CVarRHICmdFlushRenderThreadTasks.GetValueOnRenderThread() > 0);
}

bool FVelocityMeshProcessor::PrimitiveHasVelocityForView(const FViewInfo& View, const FPrimitiveSceneProxy* PrimitiveSceneProxy)
{
	// Skip camera cuts which effectively reset velocity for the new frame.
	if (View.bCameraCut && !View.PreviousViewTransform.IsSet())
	{
		return false;
	}
	// Velocity pass not rendered for debug views.
	if (View.Family->UseDebugViewPS())
	{
		return false;
	}

	const FBoxSphereBounds& PrimitiveBounds = PrimitiveSceneProxy->GetBounds();
	const float LODFactorDistanceSquared = (PrimitiveBounds.Origin - View.ViewMatrices.GetViewOrigin()).SizeSquared() * FMath::Square(View.LODDistanceFactor);

	// The minimum projected screen radius for a primitive to be drawn in the velocity pass, as a fraction of half the horizontal screen width.
	float MinScreenRadiusForVelocityPass = View.FinalPostProcessSettings.MotionBlurPerObjectSize * (2.0f / 100.0f);
	float MinScreenRadiusForVelocityPassSquared = FMath::Square(MinScreenRadiusForVelocityPass);

	// Skip primitives that only cover a small amount of screen space, motion blur on them won't be noticeable.
	if (FMath::Square(PrimitiveBounds.SphereRadius) <= MinScreenRadiusForVelocityPassSquared * LODFactorDistanceSquared)
	{
		return false;
	}

	return true;
}

bool FOpaqueVelocityMeshProcessor::PrimitiveCanHaveVelocity(EShaderPlatform ShaderPlatform, const FPrimitiveSceneProxy* PrimitiveSceneProxy)
{
	if (!FVelocityRendering::IsVelocityPassSupported(ShaderPlatform) || !PlatformSupportsVelocityRendering(ShaderPlatform))
	{
		return false;
	}

	if (!PrimitiveSceneProxy->DrawsVelocity())
	{
		return false;
	}

	/**
	 * Whether the vertex factory for this primitive requires that it render in the separate velocity pass, as opposed to the base pass.
	 * In cases where the base pass is rendering opaque velocity for a particular mesh batch, we want to filter it out from this pass,
	 * which performs a separate draw call to render velocity.
	 */
	const bool bIsSeparateVelocityPassRequiredByVertexFactory =
		FVelocityRendering::IsSeparateVelocityPassRequiredByVertexFactory(ShaderPlatform, PrimitiveSceneProxy->HasStaticLighting());

	if (!bIsSeparateVelocityPassRequiredByVertexFactory)
	{
		return false;
	}

	return true;
}

bool FOpaqueVelocityMeshProcessor::PrimitiveHasVelocityForFrame(const FPrimitiveSceneProxy* PrimitiveSceneProxy)
{
	if (!PrimitiveSceneProxy->AlwaysHasVelocity())
	{
		// Check if the primitive has moved.
		const FPrimitiveSceneInfo* PrimitiveSceneInfo = PrimitiveSceneProxy->GetPrimitiveSceneInfo();
		const FScene* Scene = PrimitiveSceneInfo->Scene;
		const FMatrix& LocalToWorld = PrimitiveSceneProxy->GetLocalToWorld();
		FMatrix PreviousLocalToWorld = LocalToWorld;
		Scene->VelocityData.GetComponentPreviousLocalToWorld(PrimitiveSceneInfo->PrimitiveComponentId, PreviousLocalToWorld);

		if (LocalToWorld.Equals(PreviousLocalToWorld, 0.0001f))
		{
			// Hasn't moved (treat as background by not rendering any special velocities)
			return false;
		}
	}

	return true;
}

bool FOpaqueVelocityMeshProcessor::TryAddMeshBatch(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	const FMaterialRenderProxy* MaterialRenderProxy,
	const FMaterial* Material)
{
	const EBlendMode BlendMode = Material->GetBlendMode();
	const bool bIsNotTranslucent = BlendMode == BLEND_Opaque || BlendMode == BLEND_Masked;

	bool bResult = true;
	if (MeshBatch.bUseForMaterial && bIsNotTranslucent && ShouldIncludeMaterialInDefaultOpaquePass(*Material))
	{
		// This is specifically done *before* the material swap, as swapped materials may have different fill / cull modes.
		const FMeshDrawingPolicyOverrideSettings OverrideSettings = ComputeMeshOverrideSettings(MeshBatch);
		const ERasterizerFillMode MeshFillMode = ComputeMeshFillMode(MeshBatch, *Material, OverrideSettings);
		const ERasterizerCullMode MeshCullMode = ComputeMeshCullMode(MeshBatch, *Material, OverrideSettings);

		/**
		 * Materials without masking or custom vertex modifications can be swapped out
		 * for the default material, which simplifies the shader. However, the default
		 * material also does not support being two-sided.
		 */
		const bool bSwapWithDefaultMaterial = Material->WritesEveryPixel() && !Material->IsTwoSided() && !Material->MaterialModifiesMeshPosition_RenderThread();

		if (bSwapWithDefaultMaterial)
		{
			MaterialRenderProxy = UMaterial::GetDefaultMaterial(MD_Surface)->GetRenderProxy();
			Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
		}

		check(Material && MaterialRenderProxy);

		bResult = Process(MeshBatch, BatchElementMask, StaticMeshId, PrimitiveSceneProxy, *MaterialRenderProxy, *Material, MeshFillMode, MeshCullMode);
	}
	return bResult;
}

void FOpaqueVelocityMeshProcessor::AddMeshBatch(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId)
{
	const EShaderPlatform ShaderPlatform = GetFeatureLevelShaderPlatform(FeatureLevel);

	if (!PrimitiveCanHaveVelocity(ShaderPlatform, PrimitiveSceneProxy))
	{
		return;
	}

	if (ViewIfDynamicMeshCommand)
	{
		if (!PrimitiveHasVelocityForFrame(PrimitiveSceneProxy))
		{
			return;
		}

		checkSlow(ViewIfDynamicMeshCommand->bIsViewInfo);
		FViewInfo* ViewInfo = (FViewInfo*)ViewIfDynamicMeshCommand;

		if (!PrimitiveHasVelocityForView(*ViewInfo, PrimitiveSceneProxy))
		{
			return;
		}
	}

	const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
	while (MaterialRenderProxy)
	{
		const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
		if (Material && Material->GetRenderingThreadShaderMap())
		{
			if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MaterialRenderProxy, Material))
			{
				break;
			}
		}

		MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
	}
}

bool FTranslucentVelocityMeshProcessor::PrimitiveCanHaveVelocity(EShaderPlatform ShaderPlatform, const FPrimitiveSceneProxy* PrimitiveSceneProxy)
{
	/**
	 * Velocity for translucency is always relevant because the pass also writes depth.
	 * Therefore, the primitive can't be filtered based on motion, or it will break post
	 * effects like depth of field which rely on depth information.
	 */
	return FVelocityRendering::IsVelocityPassSupported(ShaderPlatform) && PlatformSupportsVelocityRendering(ShaderPlatform);
}

bool FTranslucentVelocityMeshProcessor::PrimitiveHasVelocityForFrame(const FPrimitiveSceneProxy* PrimitiveSceneProxy)
{
	return true;
}

bool FTranslucentVelocityMeshProcessor::TryAddMeshBatch(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	const FMaterialRenderProxy* MaterialRenderProxy,
	const FMaterial* Material)
{
	// Whether the primitive is marked to write translucent velocity / depth.
	const bool bMaterialWritesVelocity = Material->IsTranslucencyWritingVelocity();

	bool bResult = true;
	if (MeshBatch.bUseForMaterial && bMaterialWritesVelocity)
	{
		const FMeshDrawingPolicyOverrideSettings OverrideSettings = ComputeMeshOverrideSettings(MeshBatch);
		const ERasterizerFillMode MeshFillMode = ComputeMeshFillMode(MeshBatch, *Material, OverrideSettings);
		const ERasterizerCullMode MeshCullMode = ComputeMeshCullMode(MeshBatch, *Material, OverrideSettings);

		bResult = Process(MeshBatch, BatchElementMask, StaticMeshId, PrimitiveSceneProxy, *MaterialRenderProxy, *Material, MeshFillMode, MeshCullMode);
	}
	return bResult;
}

void FTranslucentVelocityMeshProcessor::AddMeshBatch(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId)
{
	const EShaderPlatform ShaderPlatform = GetFeatureLevelShaderPlatform(FeatureLevel);

	if (!PrimitiveCanHaveVelocity(ShaderPlatform, PrimitiveSceneProxy))
	{
		return;
	}

	if (ViewIfDynamicMeshCommand)
	{
		if (!PrimitiveHasVelocityForFrame(PrimitiveSceneProxy))
		{
			return;
		}

		checkSlow(ViewIfDynamicMeshCommand->bIsViewInfo);
		FViewInfo* ViewInfo = (FViewInfo*)ViewIfDynamicMeshCommand;

		if (!PrimitiveHasVelocityForView(*ViewInfo, PrimitiveSceneProxy))
		{
			return;
		}
	}

	const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
	while (MaterialRenderProxy)
	{
		const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
		if (Material)
		{
			if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MaterialRenderProxy, Material))
			{
				break;
			}
		}

		MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
	}
}

bool GetVelocityPassShaders(
	const FMaterial& Material,
	FVertexFactoryType* VertexFactoryType,
	ERHIFeatureLevel::Type FeatureLevel,
	TShaderRef<FVelocityVS>& VertexShader,
	TShaderRef<FVelocityPS>& PixelShader)
{
	FMaterialShaderTypes ShaderTypes;

	// Don't use pipeline if we have hull/domain shaders
	ShaderTypes.PipelineType = &VelocityPipeline;

	ShaderTypes.AddShaderType<FVelocityVS>();
	ShaderTypes.AddShaderType<FVelocityPS>();

	FMaterialShaders Shaders;
	if (!Material.TryGetShaders(ShaderTypes, VertexFactoryType, Shaders))
	{
		return false;
	}

	Shaders.TryGetVertexShader(VertexShader);
	Shaders.TryGetPixelShader(PixelShader);
	return true;
}

bool FVelocityMeshProcessor::Process(
	const FMeshBatch& MeshBatch,
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
		FVelocityVS,
		FVelocityPS> VelocityPassShaders;

	if (!GetVelocityPassShaders(
		MaterialResource,
		VertexFactory->GetType(),
		FeatureLevel,
		VelocityPassShaders.VertexShader,
		VelocityPassShaders.PixelShader))
	{
		return false;
	}

	FMeshMaterialShaderElementData ShaderElementData;
	ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

	const FMeshDrawCommandSortKey SortKey = CalculateMeshStaticSortKey(VelocityPassShaders.VertexShader, VelocityPassShaders.PixelShader);

	BuildMeshDrawCommands(
		MeshBatch,
		BatchElementMask,
		PrimitiveSceneProxy,
		MaterialRenderProxy,
		MaterialResource,
		PassDrawRenderState,
		VelocityPassShaders,
		MeshFillMode,
		MeshCullMode,
		SortKey,
		EMeshPassFeatures::Default,
		ShaderElementData);

	return true;
}

FVelocityMeshProcessor::FVelocityMeshProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, const FMeshPassProcessorRenderState& InPassDrawRenderState, FMeshPassDrawListContext* InDrawListContext)
	: FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext)
	, PassDrawRenderState(InPassDrawRenderState)
{}

FOpaqueVelocityMeshProcessor::FOpaqueVelocityMeshProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, const FMeshPassProcessorRenderState& InPassDrawRenderState, FMeshPassDrawListContext* InDrawListContext)
	: FVelocityMeshProcessor(Scene, InViewIfDynamicMeshCommand, InPassDrawRenderState, InDrawListContext)
{}

FMeshPassProcessor* CreateVelocityPassProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState VelocityPassState;
	VelocityPassState.SetBlendState(TStaticBlendState<CW_RGBA>::GetRHI());
	VelocityPassState.SetDepthStencilState((Scene->EarlyZPassMode == DDM_AllOpaqueNoVelocity) // if the depth mode is all opaque except velocity, it relies on velocity to write the depth of the remaining meshes
										    ? TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI()
											: TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());

	return new(FMemStack::Get()) FOpaqueVelocityMeshProcessor(Scene, InViewIfDynamicMeshCommand, VelocityPassState, InDrawListContext);
}

FRegisterPassProcessorCreateFunction RegisterVelocityPass(&CreateVelocityPassProcessor, EShadingPath::Deferred,  EMeshPass::Velocity, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
FRegisterPassProcessorCreateFunction MobileRegisterVelocityPass(&CreateVelocityPassProcessor, EShadingPath::Mobile, EMeshPass::Velocity, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);

FTranslucentVelocityMeshProcessor::FTranslucentVelocityMeshProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, const FMeshPassProcessorRenderState& InPassDrawRenderState, FMeshPassDrawListContext* InDrawListContext)
	: FVelocityMeshProcessor(Scene, InViewIfDynamicMeshCommand, InPassDrawRenderState, InDrawListContext)
{}

FMeshPassProcessor* CreateTranslucentVelocityPassProcessor(const FScene* Scene, const FSceneView* InViewIfDynamicMeshCommand, FMeshPassDrawListContext* InDrawListContext)
{
	FMeshPassProcessorRenderState VelocityPassState;
	VelocityPassState.SetBlendState(TStaticBlendState<CW_RGBA>::GetRHI());
	VelocityPassState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());

	return new(FMemStack::Get()) FTranslucentVelocityMeshProcessor(Scene, InViewIfDynamicMeshCommand, VelocityPassState, InDrawListContext);
}

FRegisterPassProcessorCreateFunction RegisterTranslucentVelocityPass(&CreateTranslucentVelocityPassProcessor, EShadingPath::Deferred, EMeshPass::TranslucentVelocity, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
FRegisterPassProcessorCreateFunction RegisterMobileTranslucentVelocityPass(&CreateTranslucentVelocityPassProcessor, EShadingPath::Mobile, EMeshPass::TranslucentVelocity, EMeshPassFlags::CachedMeshCommands | EMeshPassFlags::MainView);
