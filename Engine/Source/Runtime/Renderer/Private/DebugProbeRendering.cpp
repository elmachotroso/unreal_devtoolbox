// Copyright Epic Games, Inc. All Rights Reserved.

#include "DebugProbeRendering.h"
#include "PixelShaderUtils.h"
#include "ShaderParameterStruct.h"
#include "SceneRendering.h"
#include "SceneTextures.h"
#include "SceneTextureParameters.h"
#include "Strata/Strata.h"


// Changing this causes a full shader recompile
static TAutoConsoleVariable<int32> CVarVisualizeLightingOnProbes(
	TEXT("r.VisualizeLightingOnProbes"),
	0,
	TEXT("Enables debug probes rendering to visualise diffuse/specular lighting (direct and indirect) on simple sphere scattered in the world.") \
	TEXT(" 0: disabled.\n")
	TEXT(" 1: camera probes only.\n")
	TEXT(" 2: world probes only.\n")
	TEXT(" 3: camera and world probes.\n")
	,
	ECVF_RenderThreadSafe);


DECLARE_GPU_STAT(StampDeferredDebugProbe);


// Must match DebugProbes.usf
#define RENDER_DEPTHPREPASS  0
#define RENDER_BASEPASS	     1
#define RENDER_VELOCITYPASS  2


class FStampDeferredDebugProbePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FStampDeferredDebugProbePS);
	SHADER_USE_PARAMETER_STRUCT(FStampDeferredDebugProbePS, FGlobalShader);

	class FRenderPass : SHADER_PERMUTATION_RANGE_INT("PERMUTATION_PASS", 0, 3);
	using FPermutationDomain = TShaderPermutationDomain<FRenderPass>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2DArray<uint>, MaterialTextureArrayUAV)
		SHADER_PARAMETER(uint32, MaxBytesPerPixel)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
		SHADER_PARAMETER(int32, DebugProbesMode)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5) && EnumHasAllFlags(Parameters.Flags, EShaderPermutationFlags::HasEditorOnlyData);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FStampDeferredDebugProbePS, "/Engine/Private/DebugProbes.usf", "MainPS", SF_Pixel);

 
template<bool bEnableDepthWrite, ECompareFunction CompareFunction>
static void CommonStampDeferredDebugProbeDrawCall(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View, 
	FStampDeferredDebugProbePS::FParameters* PassParameters,
	int32 RenderPass)
{
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	PassParameters->MaterialTextureArrayUAV = View.StrataSceneData->MaterialTextureArrayUAV;
	PassParameters->MaxBytesPerPixel = View.StrataSceneData->MaxBytesPerPixel;
	PassParameters->DebugProbesMode = View.Family->EngineShowFlags.VisualizeLightingOnProbes ? 3 : FMath::Clamp(CVarVisualizeLightingOnProbes.GetValueOnRenderThread(), 0, 3);
		
	FStampDeferredDebugProbePS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FStampDeferredDebugProbePS::FRenderPass>(RenderPass);
	TShaderMapRef<FStampDeferredDebugProbePS> PixelShader(View.ShaderMap, PermutationVector);

	FPixelShaderUtils::AddFullscreenPass<FStampDeferredDebugProbePS>(
		GraphBuilder, View.ShaderMap, RDG_EVENT_NAME("StampDeferredDebugProbePS"),
		PixelShader, PassParameters, View.ViewRect,
		TStaticBlendState<>::GetRHI(),
		TStaticRasterizerState<FM_Solid, CM_None>::GetRHI(),
		TStaticDepthStencilState<bEnableDepthWrite, CompareFunction>::GetRHI());
}

void StampDeferredDebugProbeDepthPS(
	FRDGBuilder& GraphBuilder,
	TArrayView<const FViewInfo> Views,
	const FRDGTextureRef SceneDepthTexture)
{
	RDG_EVENT_SCOPE(GraphBuilder, "StampDeferredDebugProbeDepth");
	RDG_GPU_STAT_SCOPE(GraphBuilder, StampDeferredDebugProbe);

	const bool bVisualizeLightingOnProbes = CVarVisualizeLightingOnProbes.GetValueOnRenderThread() > 0;
	for (const FViewInfo& View : Views)
	{
		if (!(bVisualizeLightingOnProbes || View.Family->EngineShowFlags.VisualizeLightingOnProbes) || View.bIsReflectionCapture)
		{
			continue;
		}

		FStampDeferredDebugProbePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FStampDeferredDebugProbePS::FParameters>();
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthWrite_StencilWrite);

		CommonStampDeferredDebugProbeDrawCall<true, CF_DepthNearOrEqual>(GraphBuilder, View, PassParameters, RENDER_DEPTHPREPASS);
	}
}

void StampDeferredDebugProbeMaterialPS(
	FRDGBuilder& GraphBuilder,
	TArrayView<const FViewInfo> Views,
	const FRenderTargetBindingSlots& BasePassRenderTargets,
	const FMinimalSceneTextures& SceneTextures)
{
	RDG_EVENT_SCOPE(GraphBuilder, "StampDeferredDebugProbeMaterial");
	RDG_GPU_STAT_SCOPE(GraphBuilder, StampDeferredDebugProbe);

	const bool bVisualizeLightingOnProbes = CVarVisualizeLightingOnProbes.GetValueOnRenderThread() > 0;
	for (const FViewInfo& View : Views)
	{
		if (!(bVisualizeLightingOnProbes || View.Family->EngineShowFlags.VisualizeLightingOnProbes) || View.bIsReflectionCapture)
		{
			continue;
		}

		FStampDeferredDebugProbePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FStampDeferredDebugProbePS::FParameters>();
		PassParameters->RenderTargets = BasePassRenderTargets;
		if (Strata::IsStrataEnabled())
		{
			// Make sure we do not write depth so that we can safely read it from texture parameters
			PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding();
			PassParameters->SceneTextures = CreateSceneTextureShaderParameters(GraphBuilder, View.GetFeatureLevel(), ESceneTextureSetupMode::SceneDepth);

			CommonStampDeferredDebugProbeDrawCall<false, CF_Always>(GraphBuilder, View, PassParameters, RENDER_BASEPASS);
		}
		else
		{
			CommonStampDeferredDebugProbeDrawCall<false, CF_DepthNearOrEqual>(GraphBuilder, View, PassParameters, RENDER_BASEPASS);
		}

	}
}

void StampDeferredDebugProbeVelocityPS(
	FRDGBuilder& GraphBuilder,
	TArrayView<const FViewInfo> Views,
	const FRenderTargetBindingSlots& BasePassRenderTargets)
{
	RDG_EVENT_SCOPE(GraphBuilder, "StampDeferredDebugProbeVelocity");
	RDG_GPU_STAT_SCOPE(GraphBuilder, StampDeferredDebugProbe);

	const bool bVisualizeLightingOnProbes = CVarVisualizeLightingOnProbes.GetValueOnRenderThread() > 0;
	for (const FViewInfo& View : Views)
	{
		if (!(bVisualizeLightingOnProbes || View.Family->EngineShowFlags.VisualizeLightingOnProbes) || View.bIsReflectionCapture)
		{
			continue;
		}

		FStampDeferredDebugProbePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FStampDeferredDebugProbePS::FParameters>();
		PassParameters->RenderTargets = BasePassRenderTargets;

		const bool bRenderVelocity = true;
		CommonStampDeferredDebugProbeDrawCall<false, CF_DepthNearOrEqual>(GraphBuilder, View, PassParameters, RENDER_VELOCITYPASS);
	}
}

