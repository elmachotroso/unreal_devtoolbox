// Copyright Epic Games, Inc. All Rights Reserved.

#include "TranslucentRendering.h"
#include "DeferredShadingRenderer.h"
#include "BasePassRendering.h"
#include "DynamicPrimitiveDrawing.h"
#include "RendererModule.h"
#include "ScenePrivate.h"
#include "SceneTextureParameters.h"
#include "ScreenRendering.h"
#include "ScreenPass.h"
#include "MeshPassProcessor.inl"
#include "VolumetricRenderTarget.h"
#include "VariableRateShadingImageManager.h"
#include "Lumen/LumenTranslucencyVolumeLighting.h"
#include "VirtualShadowMaps/VirtualShadowMapArray.h"
#include "Strata/Strata.h"
#include "HairStrands/HairStrandsUtils.h"
#include "PixelShaderUtils.h"

DECLARE_CYCLE_STAT(TEXT("TranslucencyTimestampQueryFence Wait"), STAT_TranslucencyTimestampQueryFence_Wait, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("TranslucencyTimestampQuery Wait"), STAT_TranslucencyTimestampQuery_Wait, STATGROUP_SceneRendering);
DECLARE_CYCLE_STAT(TEXT("Translucency"), STAT_CLP_Translucency, STATGROUP_ParallelCommandListMarkers);
DECLARE_FLOAT_COUNTER_STAT(TEXT("Translucency GPU Time (MS)"), STAT_TranslucencyGPU, STATGROUP_SceneRendering);
DEFINE_GPU_DRAWCALL_STAT(Translucency);

static TAutoConsoleVariable<float> CVarSeparateTranslucencyScreenPercentage(
	TEXT("r.SeparateTranslucencyScreenPercentage"),
	100.0f,
	TEXT("Render separate translucency at this percentage of the full resolution.\n")
	TEXT("in percent, >0 and <=100, larger numbers are possible (supersampling).")
	TEXT("<0 is treated like 100."),
	ECVF_Scalability | ECVF_Default);

static TAutoConsoleVariable<int32> CVarSeparateTranslucencyAutoDownsample(
	TEXT("r.SeparateTranslucencyAutoDownsample"),
	0,
	TEXT("Whether to automatically downsample separate translucency based on last frame's GPU time.\n")
	TEXT("Automatic downsampling is only used when r.SeparateTranslucencyScreenPercentage is 100"),
	ECVF_Scalability | ECVF_Default);

static TAutoConsoleVariable<float> CVarSeparateTranslucencyDurationDownsampleThreshold(
	TEXT("r.SeparateTranslucencyDurationDownsampleThreshold"),
	1.5f,
	TEXT("When smoothed full-res translucency GPU duration is larger than this value (ms), the entire pass will be downsampled by a factor of 2 in each dimension."),
	ECVF_Scalability | ECVF_Default);

static TAutoConsoleVariable<float> CVarSeparateTranslucencyDurationUpsampleThreshold(
	TEXT("r.SeparateTranslucencyDurationUpsampleThreshold"),
	.5f,
	TEXT("When smoothed half-res translucency GPU duration is smaller than this value (ms), the entire pass will be restored to full resolution.\n")
	TEXT("This should be around 1/4 of r.SeparateTranslucencyDurationDownsampleThreshold to avoid toggling downsampled state constantly."),
	ECVF_Scalability | ECVF_Default);

static TAutoConsoleVariable<float> CVarSeparateTranslucencyMinDownsampleChangeTime(
	TEXT("r.SeparateTranslucencyMinDownsampleChangeTime"),
	1.0f,
	TEXT("Minimum time in seconds between changes to automatic downsampling state, used to prevent rapid swapping between half and full res."),
	ECVF_Scalability | ECVF_Default);

int32 GSeparateTranslucencyUpsampleMode = 1;
static FAutoConsoleVariableRef CVarSeparateTranslucencyUpsampleMode(
	TEXT("r.SeparateTranslucencyUpsampleMode"),
	GSeparateTranslucencyUpsampleMode,
	TEXT("Upsample method to use on separate translucency.  These are only used when r.SeparateTranslucencyScreenPercentage is less than 100.\n")
	TEXT("0: bilinear 1: Nearest-Depth Neighbor (only when r.SeparateTranslucencyScreenPercentage is 50)"),
	ECVF_Scalability | ECVF_Default);

static TAutoConsoleVariable<int32> CVarRHICmdFlushRenderThreadTasksTranslucentPass(
	TEXT("r.RHICmdFlushRenderThreadTasksTranslucentPass"),
	0,
	TEXT("Wait for completion of parallel render thread tasks at the end of the translucent pass.  A more granular version of r.RHICmdFlushRenderThreadTasks. If either r.RHICmdFlushRenderThreadTasks or r.RHICmdFlushRenderThreadTasksTranslucentPass is > 0 we will flush."));

static TAutoConsoleVariable<int32> CVarParallelTranslucency(
	TEXT("r.ParallelTranslucency"),
	1,
	TEXT("Toggles parallel translucency rendering. Parallel rendering must be enabled for this to have an effect."),
	ECVF_RenderThreadSafe);

static const TCHAR* kTranslucencyPassName[] = {
	TEXT("BeforeDistortion"),
	TEXT("AfterDOF"),
	TEXT("AfterDOFModulate"),
	TEXT("AfterMotionBlur"),
	TEXT("All"),
};
static_assert(UE_ARRAY_COUNT(kTranslucencyPassName) == int32(ETranslucencyPass::TPT_MAX), "Fix me");


static const TCHAR* TranslucencyPassToString(ETranslucencyPass::Type TranslucencyPass)
{
	return kTranslucencyPassName[TranslucencyPass];
}

EMeshPass::Type TranslucencyPassToMeshPass(ETranslucencyPass::Type TranslucencyPass)
{
	EMeshPass::Type TranslucencyMeshPass = EMeshPass::Num;

	switch (TranslucencyPass)
	{
	case ETranslucencyPass::TPT_StandardTranslucency: TranslucencyMeshPass = EMeshPass::TranslucencyStandard; break;
	case ETranslucencyPass::TPT_TranslucencyAfterDOF: TranslucencyMeshPass = EMeshPass::TranslucencyAfterDOF; break;
	case ETranslucencyPass::TPT_TranslucencyAfterDOFModulate: TranslucencyMeshPass = EMeshPass::TranslucencyAfterDOFModulate; break;
	case ETranslucencyPass::TPT_TranslucencyAfterMotionBlur: TranslucencyMeshPass = EMeshPass::TranslucencyAfterMotionBlur; break;
	case ETranslucencyPass::TPT_AllTranslucency: TranslucencyMeshPass = EMeshPass::TranslucencyAll; break;
	}

	check(TranslucencyMeshPass != EMeshPass::Num);

	return TranslucencyMeshPass;
}

ETranslucencyView GetTranslucencyView(const FViewInfo& View)
{
#if RHI_RAYTRACING
	if (ShouldRenderRayTracingTranslucency(View))
	{
		return ETranslucencyView::RayTracing;
	}
#endif
	return View.IsUnderwater() ? ETranslucencyView::UnderWater : ETranslucencyView::AboveWater;
}

ETranslucencyView GetTranslucencyViews(TArrayView<const FViewInfo> Views)
{
	ETranslucencyView TranslucencyViews = ETranslucencyView::None;
	for (const FViewInfo& View : Views)
	{
		TranslucencyViews |= GetTranslucencyView(View);
	}
	return TranslucencyViews;
}

/** Mostly used to know if debug rendering should be drawn in this pass */
static bool IsMainTranslucencyPass(ETranslucencyPass::Type TranslucencyPass)
{
	return TranslucencyPass == ETranslucencyPass::TPT_AllTranslucency || TranslucencyPass == ETranslucencyPass::TPT_StandardTranslucency;
}

static bool IsParallelTranslucencyEnabled()
{
	return GRHICommandList.UseParallelAlgorithms() && CVarParallelTranslucency.GetValueOnRenderThread();
}

static bool IsTranslucencyWaitForTasksEnabled()
{
	return IsParallelTranslucencyEnabled() && (CVarRHICmdFlushRenderThreadTasksTranslucentPass.GetValueOnRenderThread() > 0 || CVarRHICmdFlushRenderThreadTasks.GetValueOnRenderThread() > 0);
}

static bool IsSeparateTranslucencyEnabled(ETranslucencyPass::Type TranslucencyPass, float DownsampleScale)
{
	// Currently AfterDOF is rendered earlier in the frame and must be rendered in a separate texture.
	if (TranslucencyPass == ETranslucencyPass::TPT_TranslucencyAfterDOF
		|| TranslucencyPass == ETranslucencyPass::TPT_TranslucencyAfterDOFModulate
		|| TranslucencyPass == ETranslucencyPass::TPT_TranslucencyAfterMotionBlur
		)
	{
		return true;
	}

	// Otherwise it only gets rendered in the separate buffer if it is downsampled.
	if (DownsampleScale < 1.0f)
	{
		return true;
	}

	return false;
}

static int GetSSRQuality()
{
	static IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(TEXT("r.SSR.Quality"));
	int SSRQuality = CVar ? (CVar->GetInt()) : 0;
	return SSRQuality;
}

static bool ShouldRenderTranslucencyScreenSpaceReflections(const FViewInfo& View)
{
	// The screenspace reflection of translucency is not controlled by the postprocessing setting
	// or the raytracing overlay setting. It needs to be turned on/off dynamically to support
	// diffuse only
	if (!View.Family->EngineShowFlags.ScreenSpaceReflections)
	{
		return false;
	}

	int SSRQuality = GetSSRQuality();

	if (SSRQuality <= 0)
	{
		return false;
	}

	return true;
}

static void AddBeginTranslucencyTimerPass(FRDGBuilder& GraphBuilder, const FViewInfo& View)
{
#if STATS
	if (View.ViewState)
	{
		AddPass(GraphBuilder, RDG_EVENT_NAME("BeginTimer"), [&View](FRHICommandListImmediate& RHICmdList)
		{
			View.ViewState->TranslucencyTimer.Begin(RHICmdList);
		});
	}
#endif
}

static void AddEndTranslucencyTimerPass(FRDGBuilder& GraphBuilder, const FViewInfo& View)
{
#if STATS
	if (View.ViewState)
	{
		AddPass(GraphBuilder, RDG_EVENT_NAME("EndTimer"), [&View](FRHICommandListImmediate& RHICmdList)
		{
			View.ViewState->TranslucencyTimer.End(RHICmdList);
		});
	}
#endif
}

static bool HasSeparateTranslucencyTimer(const FViewInfo& View)
{
	return View.ViewState && GSupportsTimestampRenderQueries
#if !STATS
		&& (CVarSeparateTranslucencyAutoDownsample.GetValueOnRenderThread() != 0)
#endif
		;
}

static void AddBeginSeparateTranslucencyTimerPass(FRDGBuilder& GraphBuilder, const FViewInfo& View, ETranslucencyPass::Type TranslucencyPass)
{
	if (HasSeparateTranslucencyTimer(View))
	{
		AddPass(GraphBuilder, RDG_EVENT_NAME("BeginTimer"), [&View, TranslucencyPass](FRHICommandListImmediate& RHICmdList)
		{
			switch(TranslucencyPass)
			{
			case ETranslucencyPass::TPT_TranslucencyAfterDOF:
				View.ViewState->SeparateTranslucencyTimer.Begin(RHICmdList);
				break;
			case ETranslucencyPass::TPT_TranslucencyAfterDOFModulate:
				View.ViewState->SeparateTranslucencyModulateTimer.Begin(RHICmdList);
				break;
			case ETranslucencyPass::TPT_TranslucencyAfterMotionBlur:
				View.ViewState->PostMotionBlurTranslucencyTimer.Begin(RHICmdList);
				break;
			default:
				break;
			}
		});
	}
}

static void AddEndSeparateTranslucencyTimerPass(FRDGBuilder& GraphBuilder, const FViewInfo& View, ETranslucencyPass::Type TranslucencyPass)
{
	if (HasSeparateTranslucencyTimer(View))
	{
		AddPass(GraphBuilder, RDG_EVENT_NAME("EndTimer"), [&View, TranslucencyPass](FRHICommandListImmediate& RHICmdList)
		{
			switch(TranslucencyPass)
			{
			case ETranslucencyPass::TPT_TranslucencyAfterDOF:
				View.ViewState->SeparateTranslucencyTimer.End(RHICmdList);
				break;
			case ETranslucencyPass::TPT_TranslucencyAfterDOFModulate:			
				View.ViewState->SeparateTranslucencyModulateTimer.End(RHICmdList);
				break;
			case ETranslucencyPass::TPT_TranslucencyAfterMotionBlur:
				View.ViewState->PostMotionBlurTranslucencyTimer.End(RHICmdList);
				break;
			default:
				break;
			}
		});
	}
}

FSeparateTranslucencyDimensions UpdateTranslucencyTimers(FRHICommandListImmediate& RHICmdList, TArrayView<const FViewInfo> Views)
{
	bool bAnyViewWantsDownsampledSeparateTranslucency = false;

	const bool bSeparateTranslucencyAutoDownsample = CVarSeparateTranslucencyAutoDownsample.GetValueOnRenderThread() != 0;
	const bool bStatsEnabled = STATS != 0;

	if (GSupportsTimestampRenderQueries && (bSeparateTranslucencyAutoDownsample || bStatsEnabled))
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];
			SCOPED_GPU_MASK(RHICmdList, View.GPUMask);
			FSceneViewState* ViewState = View.ViewState;

			if (ViewState)
			{
				//We always tick the separate trans timer but only need the other timer for stats
				bool bSeparateTransTimerSuccess = ViewState->SeparateTranslucencyTimer.Tick(RHICmdList);
				bool bSeparateTransModulateTimerSuccess = ViewState->SeparateTranslucencyModulateTimer.Tick(RHICmdList);

				if (STATS)
				{
					ViewState->TranslucencyTimer.Tick(RHICmdList);
					//Stats are fed the most recent available time and so are lagged a little. 
					float MostRecentTotalTime = ViewState->TranslucencyTimer.GetTimeMS() +
						ViewState->SeparateTranslucencyTimer.GetTimeMS() +
						ViewState->SeparateTranslucencyModulateTimer.GetTimeMS();
					SET_FLOAT_STAT(STAT_TranslucencyGPU, MostRecentTotalTime);
				}

				if (bSeparateTranslucencyAutoDownsample && bSeparateTransTimerSuccess)
				{
					float LastFrameTranslucencyDurationMS = ViewState->SeparateTranslucencyTimer.GetTimeMS() + ViewState->SeparateTranslucencyModulateTimer.GetTimeMS();
					const bool bOriginalShouldAutoDownsampleTranslucency = ViewState->bShouldAutoDownsampleTranslucency;

					if (ViewState->bShouldAutoDownsampleTranslucency)
					{
						ViewState->SmoothedFullResTranslucencyGPUDuration = 0;
						const float LerpAlpha = ViewState->SmoothedHalfResTranslucencyGPUDuration == 0 ? 1.0f : .1f;
						ViewState->SmoothedHalfResTranslucencyGPUDuration = FMath::Lerp(ViewState->SmoothedHalfResTranslucencyGPUDuration, LastFrameTranslucencyDurationMS, LerpAlpha);

						// Don't re-asses switching for some time after the last switch
						if (View.Family->Time.GetRealTimeSeconds() - ViewState->LastAutoDownsampleChangeTime > CVarSeparateTranslucencyMinDownsampleChangeTime.GetValueOnRenderThread())
						{
							// Downsample if the smoothed time is larger than the threshold
							ViewState->bShouldAutoDownsampleTranslucency = ViewState->SmoothedHalfResTranslucencyGPUDuration > CVarSeparateTranslucencyDurationUpsampleThreshold.GetValueOnRenderThread();

							if (!ViewState->bShouldAutoDownsampleTranslucency)
							{
								// Do 'log LogRenderer verbose' to get these
								UE_LOG(LogRenderer, Verbose, TEXT("Upsample: %.1fms < %.1fms"), ViewState->SmoothedHalfResTranslucencyGPUDuration, CVarSeparateTranslucencyDurationUpsampleThreshold.GetValueOnRenderThread());
							}
						}
					}
					else
					{
						ViewState->SmoothedHalfResTranslucencyGPUDuration = 0;
						const float LerpAlpha = ViewState->SmoothedFullResTranslucencyGPUDuration == 0 ? 1.0f : .1f;
						ViewState->SmoothedFullResTranslucencyGPUDuration = FMath::Lerp(ViewState->SmoothedFullResTranslucencyGPUDuration, LastFrameTranslucencyDurationMS, LerpAlpha);

						if (View.Family->Time.GetRealTimeSeconds() - ViewState->LastAutoDownsampleChangeTime > CVarSeparateTranslucencyMinDownsampleChangeTime.GetValueOnRenderThread())
						{
							// Downsample if the smoothed time is larger than the threshold
							ViewState->bShouldAutoDownsampleTranslucency = ViewState->SmoothedFullResTranslucencyGPUDuration > CVarSeparateTranslucencyDurationDownsampleThreshold.GetValueOnRenderThread();

							if (ViewState->bShouldAutoDownsampleTranslucency)
							{
								UE_LOG(LogRenderer, Verbose, TEXT("Downsample: %.1fms > %.1fms"), ViewState->SmoothedFullResTranslucencyGPUDuration, CVarSeparateTranslucencyDurationDownsampleThreshold.GetValueOnRenderThread());
							}
						}
					}

					if (bOriginalShouldAutoDownsampleTranslucency != ViewState->bShouldAutoDownsampleTranslucency)
					{
						ViewState->LastAutoDownsampleChangeTime = View.Family->Time.GetRealTimeSeconds();
					}

					bAnyViewWantsDownsampledSeparateTranslucency = bAnyViewWantsDownsampledSeparateTranslucency || ViewState->bShouldAutoDownsampleTranslucency;
				}
			}
		}
	}

	float EffectiveScale = FMath::Clamp(CVarSeparateTranslucencyScreenPercentage.GetValueOnRenderThread() / 100.0f, 0.0f, 1.0f);

	// 'r.SeparateTranslucencyScreenPercentage' CVar wins over automatic downsampling
	if (FMath::IsNearlyEqual(EffectiveScale, 1.0f) && bAnyViewWantsDownsampledSeparateTranslucency)
	{
		EffectiveScale = 0.5f;
	}

	FSeparateTranslucencyDimensions Dimensions;
	Dimensions.Extent = GetScaledExtent(GetSceneTextureExtent(), EffectiveScale);
	Dimensions.NumSamples = GetSceneTextureNumSamples();
	Dimensions.Scale = EffectiveScale;
	return Dimensions;
}

FTranslucencyPassResourcesMap::FTranslucencyPassResourcesMap(int32 NumViews)
{
	Array.SetNum(NumViews);

	for (int32 ViewIndex = 0; ViewIndex < NumViews; ViewIndex++)
	{
		for (int32 i = 0; i < int32(ETranslucencyPass::TPT_MAX); i++)
		{
			Array[ViewIndex][i].Pass = ETranslucencyPass::Type(i);
		}
	}
}
/** Pixel shader used to copy scene color into another texture so that materials can read from scene color with a node. */
class FCopySceneColorPS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FCopySceneColorPS);
	SHADER_USE_PARAMETER_STRUCT(FCopySceneColorPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FCopySceneColorPS, "/Engine/Private/TranslucentLightingShaders.usf", "CopySceneColorMain", SF_Pixel);

static FRDGTextureRef AddCopySceneColorPass(FRDGBuilder& GraphBuilder, TArrayView<const FViewInfo> Views, FRDGTextureMSAA SceneColor)
{
	FRDGTextureRef SceneColorCopyTexture = nullptr;
	ERenderTargetLoadAction LoadAction = ERenderTargetLoadAction::ENoAction;

	RDG_EVENT_SCOPE(GraphBuilder, "CopySceneColor");

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		const FViewInfo& View = Views[ViewIndex];

		if (View.IsUnderwater())
		{
			continue;
		}

		bool bNeedsResolve = false;
		for (int32 TranslucencyPass = 0; TranslucencyPass < ETranslucencyPass::TPT_MAX; ++TranslucencyPass)
		{
			if (View.TranslucentPrimCount.UseSceneColorCopy((ETranslucencyPass::Type)TranslucencyPass))
			{
				bNeedsResolve = true;
				break;
			}
		}

		if (bNeedsResolve)
		{
			RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
			RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);

			AddCopyToResolveTargetPass(GraphBuilder, SceneColor.Target, SceneColor.Resolve, FResolveRect(View.ViewRect));

			const FIntPoint SceneColorExtent = SceneColor.Target->Desc.Extent;

			if (!SceneColorCopyTexture)
			{
				SceneColorCopyTexture = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(SceneColorExtent, PF_B8G8R8A8, FClearValueBinding::White, TexCreate_ShaderResource | TexCreate_RenderTargetable), TEXT("SceneColorCopy"));
			}

			const FScreenPassTextureViewport Viewport(SceneColorCopyTexture, View.ViewRect);

			TShaderMapRef<FScreenVS> VertexShader(View.ShaderMap);
			TShaderMapRef<FCopySceneColorPS> PixelShader(View.ShaderMap);

			auto* PassParameters = GraphBuilder.AllocParameters<FCopySceneColorPS::FParameters>();
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->SceneColorTexture = SceneColor.Resolve;
			PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorCopyTexture, LoadAction);

			if (!View.Family->bMultiGPUForkAndJoin)
			{
				LoadAction = ERenderTargetLoadAction::ELoad;
			}

			AddDrawScreenPass(GraphBuilder, {}, View, Viewport, Viewport, VertexShader, PixelShader, PassParameters);
		}
	}

	return SceneColorCopyTexture;
}

class FComposeSeparateTranslucencyPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FComposeSeparateTranslucencyPS);
	SHADER_USE_PARAMETER_STRUCT(FComposeSeparateTranslucencyPS, FGlobalShader);

	class FNearestDepthNeighborUpsampling : SHADER_PERMUTATION_BOOL("PERMUTATION_NEARESTDEPTHNEIGHBOR");
	using FPermutationDomain = TShaderPermutationDomain<FNearestDepthNeighborUpsampling>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FScreenTransform, ScreenPosToSceneColorUV)
		SHADER_PARAMETER(FScreenTransform, ScreenPosToSeparateTranslucencyUV)
		SHADER_PARAMETER(FVector2f, SeparateTranslucencyUVMin)
		SHADER_PARAMETER(FVector2f, SeparateTranslucencyUVMax)
		SHADER_PARAMETER(FVector2f, SeparateTranslucencyExtentInverse)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneColorTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState,  SceneColorSampler)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SeparateTranslucencyPointTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState,  SeparateTranslucencyPointSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SeparateModulationPointTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState,  SeparateModulationPointSampler)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SeparateTranslucencyBilinearTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState,  SeparateTranslucencyBilinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SeparateModulationBilinearTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState,  SeparateModulationBilinearSampler)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LowResDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LowResDepthSampler)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FullResDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, FullResDepthSampler)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

class FTranslucencyUpsampleResponsiveAAPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FTranslucencyUpsampleResponsiveAAPS);
	SHADER_USE_PARAMETER_STRUCT(FTranslucencyUpsampleResponsiveAAPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, StencilPixelPosMin)
		SHADER_PARAMETER(FIntPoint, StencilPixelPosMax)
		SHADER_PARAMETER(FScreenTransform, SvPositionToStencilPixelCoord)
		SHADER_PARAMETER(int32, StencilMask)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, StencilTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FComposeSeparateTranslucencyPS, "/Engine/Private/ComposeSeparateTranslucency.usf", "MainPS", SF_Pixel);
IMPLEMENT_GLOBAL_SHADER(FTranslucencyUpsampleResponsiveAAPS, "/Engine/Private/TranslucencyUpsampling.usf", "UpsampleResponsiveAAPS", SF_Pixel);


FScreenPassTexture FTranslucencyComposition::AddPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FTranslucencyPassResources& TranslucencyTextures) const
{
	// if nothing is rendered into the separate translucency, then just return the existing Scenecolor
	ensure(TranslucencyTextures.IsValid());
	if (!TranslucencyTextures.IsValid())
	{
		return SceneColor;
	}

	FRDGTextureRef SeparateModulationTexture = TranslucencyTextures.GetColorModulateForRead(GraphBuilder);
	FRDGTextureRef SeparateTranslucencyTexture = TranslucencyTextures.GetColorForRead(GraphBuilder);

	FScreenPassTextureViewport SceneColorViewport(FIntPoint(1, 1), FIntRect(0, 0, 1, 1));
	if (SceneColor.IsValid())
	{
		SceneColorViewport = FScreenPassTextureViewport(SceneColor);
	}

	FScreenPassTextureViewport TranslucencyViewport(FIntPoint(1, 1), FIntRect(0, 0, 1, 1));
	if (TranslucencyTextures.ColorTexture.IsValid())
	{
		TranslucencyViewport = FScreenPassTextureViewport(TranslucencyTextures.ColorTexture.Resolve, TranslucencyTextures.ViewRect);
	}
	else if (TranslucencyTextures.ColorModulateTexture.IsValid())
	{
		TranslucencyViewport = FScreenPassTextureViewport(TranslucencyTextures.ColorModulateTexture.Resolve, TranslucencyTextures.ViewRect);
	}

	bool bPostMotionBlur = TranslucencyTextures.Pass == ETranslucencyPass::TPT_TranslucencyAfterMotionBlur;
	if (bPostMotionBlur)
	{
		check(!bApplyModulateOnly);
	}
	else if (bApplyModulateOnly)
	{
		if (!TranslucencyTextures.ColorModulateTexture.IsValid())
		{
			return SceneColor;
		}

		SeparateTranslucencyTexture = GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackAlphaOneDummy);
	}

	const TCHAR* OpName = nullptr;
	FRHIBlendState* BlendState = nullptr;
	FRDGTextureRef NewSceneColor = nullptr;
	if (Operation == EOperation::UpscaleOnly)
	{
		check(!SceneColor.IsValid());
		ensure(!TranslucencyTextures.ColorModulateTexture.IsValid());

		OpName = TEXT("UpscaleTranslucency");

		FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
			OutputViewport.Extent,
			PF_FloatRGBA,
			FClearValueBinding::Black,
			TexCreate_RenderTargetable | TexCreate_ShaderResource);

		NewSceneColor = GraphBuilder.CreateTexture(
			OutputDesc,
			bPostMotionBlur ? TEXT("PostMotionBlurTranslucency.SceneColor") : TEXT("PostDOFTranslucency.SceneColor"));
	}
	else if (Operation == EOperation::ComposeToExistingSceneColor)
	{
		check(SceneColor.IsValid());
		ensure(!TranslucencyTextures.ColorModulateTexture.IsValid());

		OpName = TEXT("ComposeTranslucencyToExistingColor");
		BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_SourceAlpha>::GetRHI();

		ensure(SceneColor.Texture->Desc.Flags & TexCreate_RenderTargetable);
		NewSceneColor = SceneColor.Texture;
	}
	else if (Operation == EOperation::ComposeToNewSceneColor)
	{
		check(SceneColor.IsValid());

		OpName = TEXT("ComposeTranslucencyToNewSceneColor");

		FRDGTextureDesc OutputDesc = FRDGTextureDesc::Create2D(
			OutputViewport.Extent,
			OutputPixelFormat != PF_Unknown ? OutputPixelFormat : SceneColor.Texture->Desc.Format,
			FClearValueBinding::Black,
			TexCreate_RenderTargetable | TexCreate_ShaderResource);

		NewSceneColor = GraphBuilder.CreateTexture(
			OutputDesc,
			bPostMotionBlur ? TEXT("PostMotionBlurTranslucency.SceneColor") : TEXT("PostDOFTranslucency.SceneColor"));
	}
	else
	{
		unimplemented();
	}

	const FVector2f SeparateTranslucencyExtentInv = FVector2f(1.0f, 1.0f) / FVector2f(TranslucencyViewport.Extent);

	const bool bScaleSeparateTranslucency = OutputViewport.Rect.Size() != TranslucencyTextures.ViewRect.Size();
	const float DownsampleScale = float(TranslucencyTextures.ViewRect.Width()) / float(OutputViewport.Rect.Width());
	const bool DepthUpscampling = (
		bScaleSeparateTranslucency &&
		TranslucencyTextures.DepthTexture.IsValid() &&
		SceneDepth.IsValid() && 
		FMath::IsNearlyEqual(DownsampleScale, 0.5f) &&
		GSeparateTranslucencyUpsampleMode > 0);

	FScreenTransform SvPositionToViewportUV = FScreenTransform::SvPositionToViewportUV(OutputViewport.Rect);

	FComposeSeparateTranslucencyPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FComposeSeparateTranslucencyPS::FParameters>();
	PassParameters->ScreenPosToSceneColorUV = SvPositionToViewportUV * FScreenTransform::ChangeTextureBasisFromTo(
		SceneColorViewport, FScreenTransform::ETextureBasis::ViewportUV, FScreenTransform::ETextureBasis::TextureUV);
	PassParameters->ScreenPosToSeparateTranslucencyUV = SvPositionToViewportUV * FScreenTransform::ChangeTextureBasisFromTo(
		TranslucencyViewport, FScreenTransform::ETextureBasis::ViewportUV, FScreenTransform::ETextureBasis::TextureUV);

	PassParameters->SeparateTranslucencyUVMin = (FVector2f(TranslucencyViewport.Rect.Min) + FVector2f(0.5f, 0.5f)) * SeparateTranslucencyExtentInv;
	PassParameters->SeparateTranslucencyUVMax = (FVector2f(TranslucencyViewport.Rect.Max) - FVector2f(0.5f, 0.5f)) * SeparateTranslucencyExtentInv;
	PassParameters->SeparateTranslucencyExtentInverse = SeparateTranslucencyExtentInv;
	
	PassParameters->SceneColorTexture = Operation == EOperation::ComposeToNewSceneColor
		? SceneColor.Texture
		: GraphBuilder.RegisterExternalTexture(GSystemTextures.BlackAlphaOneDummy);
	PassParameters->SceneColorSampler = TStaticSamplerState<SF_Point>::GetRHI();

	PassParameters->SeparateTranslucencyPointTexture = SeparateTranslucencyTexture;
	PassParameters->SeparateTranslucencyPointSampler = TStaticSamplerState<SF_Point>::GetRHI();
	
	PassParameters->SeparateModulationPointTexture = SeparateModulationTexture;
	PassParameters->SeparateModulationPointSampler = TStaticSamplerState<SF_Point>::GetRHI();

	PassParameters->SeparateTranslucencyBilinearTexture = SeparateTranslucencyTexture;
	PassParameters->SeparateTranslucencyBilinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

	PassParameters->SeparateModulationBilinearTexture = SeparateModulationTexture;
	PassParameters->SeparateModulationBilinearSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();

	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;

	if (Operation == EOperation::ComposeToExistingSceneColor)
	{
		PassParameters->RenderTargets[0] = FRenderTargetBinding(NewSceneColor, ERenderTargetLoadAction::ELoad);
	}
	else
	{
		PassParameters->RenderTargets[0] = FRenderTargetBinding(NewSceneColor, ERenderTargetLoadAction::ENoAction);
	}

	if (DepthUpscampling)
	{
		PassParameters->LowResDepthTexture = TranslucencyTextures.GetDepthForRead(GraphBuilder);
		PassParameters->LowResDepthSampler = TStaticSamplerState<SF_Point>::GetRHI();
		PassParameters->FullResDepthTexture = SceneDepth.Texture;
		PassParameters->FullResDepthSampler = TStaticSamplerState<SF_Point>::GetRHI();
	}

	FComposeSeparateTranslucencyPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FComposeSeparateTranslucencyPS::FNearestDepthNeighborUpsampling>(DepthUpscampling);

	TShaderMapRef<FComposeSeparateTranslucencyPS> PixelShader(View.ShaderMap, PermutationVector);
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		View.ShaderMap,
		RDG_EVENT_NAME(
			"%s(%s%s%s) %dx%d -> %dx%d",
			OpName,
			kTranslucencyPassName[int32(TranslucencyTextures.Pass)],
			bApplyModulateOnly ? TEXT(" ModulateOnly") : TEXT(""),
			DepthUpscampling ? TEXT(" DepthUpscampling") : TEXT(""),
			TranslucencyTextures.ViewRect.Width(), TranslucencyTextures.ViewRect.Height(),
			OutputViewport.Rect.Width(), OutputViewport.Rect.Height()),
		PixelShader,
		PassParameters,
		OutputViewport.Rect,
		BlendState);

	return FScreenPassTexture(NewSceneColor, OutputViewport.Rect);
}

static void AddUpsampleResponsiveAAPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FScreenPassTexture DownsampledTranslucencyDepth,
	FRDGTextureRef OutputDepthTexture)
{
	FTranslucencyUpsampleResponsiveAAPS::FParameters* PassParameters = GraphBuilder.AllocParameters<FTranslucencyUpsampleResponsiveAAPS::FParameters>();
	PassParameters->StencilPixelPosMin = DownsampledTranslucencyDepth.ViewRect.Min;
	PassParameters->StencilPixelPosMax = DownsampledTranslucencyDepth.ViewRect.Max - 1;
	PassParameters->SvPositionToStencilPixelCoord = (FScreenTransform::Identity - View.ViewRect.Min) * (FVector2f(DownsampledTranslucencyDepth.ViewRect.Size()) / FVector2f(View.ViewRect.Size())) + DownsampledTranslucencyDepth.ViewRect.Min;
	PassParameters->StencilMask = STENCIL_TEMPORAL_RESPONSIVE_AA_MASK;
	PassParameters->StencilTexture = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(DownsampledTranslucencyDepth.Texture, PF_X24_G8));
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(OutputDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthNop_StencilWrite);

	TShaderMapRef<FScreenVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FTranslucencyUpsampleResponsiveAAPS> PixelShader(View.ShaderMap);

	FRHIDepthStencilState* DepthStencilState = TStaticDepthStencilState<false, CF_Always,
		true, CF_Always, SO_Keep, SO_Keep, SO_Replace,
		false, CF_Always, SO_Keep, SO_Keep, SO_Keep,
		0x00, STENCIL_TEMPORAL_RESPONSIVE_AA_MASK>::GetRHI();
	FRHIBlendState* BlendState = TStaticBlendState<CW_NONE>::GetRHI();

	const FScreenPassPipelineState PipelineState(VertexShader, PixelShader, BlendState, DepthStencilState, /* StencilRef = */ STENCIL_TEMPORAL_RESPONSIVE_AA_MASK);

	ClearUnusedGraphResources(PixelShader, PassParameters);
	GraphBuilder.AddPass(
		RDG_EVENT_NAME("UpsampleResponsiveAA %dx%d -> %dx%d",
			DownsampledTranslucencyDepth.ViewRect.Width(), DownsampledTranslucencyDepth.ViewRect.Height(),
			View.ViewRect.Width(), View.ViewRect.Height()),
		PassParameters,
		ERDGPassFlags::Raster,
		[&View, PipelineState, PixelShader, PassParameters](FRHICommandList& RHICmdList)
	{
		FScreenPassTextureViewport OutputViewport(PassParameters->RenderTargets.DepthStencil.GetTexture()->Desc.Extent, View.ViewRect);
		DrawScreenPass(RHICmdList, View, OutputViewport, OutputViewport, PipelineState, EScreenPassDrawFlags::None, [&](FRHICommandList&)
		{
			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);
		});
	});
}

bool FSceneRenderer::ShouldRenderTranslucency() const
{
	return  ViewFamily.EngineShowFlags.Translucency
		&& !ViewFamily.EngineShowFlags.VisualizeLightCulling
		&& !ViewFamily.UseDebugViewPS();
}

bool FSceneRenderer::ShouldRenderTranslucency(ETranslucencyPass::Type TranslucencyPass) const
{
	extern int32 GLightShaftRenderAfterDOF;

	// Change this condition to control where simple elements should be rendered.
	if (IsMainTranslucencyPass(TranslucencyPass))
	{
		for (const FViewInfo& View : Views)
		{
			if (View.bHasTranslucentViewMeshElements || View.SimpleElementCollector.BatchedElements.HasPrimsToDraw())
			{
				return true;
			}
		}
	}

	// If lightshafts are rendered in low res, we must reset the offscreen buffer in case is was also used in TPT_StandardTranslucency.
	if (GLightShaftRenderAfterDOF && TranslucencyPass == ETranslucencyPass::TPT_TranslucencyAfterDOF)
	{
		return true;
	}

	for (const FViewInfo& View : Views)
	{
		if (View.TranslucentPrimCount.Num(TranslucencyPass) > 0)
		{
			return true;
		}
	}

	return false;
}

FScreenPassTextureViewport FSeparateTranslucencyDimensions::GetInstancedStereoViewport(const FViewInfo& View, float InstancedStereoWidth) const
{
	FIntRect ViewRect = View.ViewRect;
	if (View.IsInstancedStereoPass() && !View.bIsMultiViewEnabled)
	{
		ViewRect.Max.X = ViewRect.Min.X + InstancedStereoWidth;
	}
	ViewRect = GetScaledRect(ViewRect, Scale);
	return FScreenPassTextureViewport(Extent, ViewRect);
}

void SetupPostMotionBlurTranslucencyViewParameters(const FViewInfo& View, FViewUniformShaderParameters& Parameters)
{
	// post-motionblur pass without down-sampling requires no Temporal AA jitter
	FBox VolumeBounds[TVC_MAX];
	FViewMatrices ModifiedViewMatrices = View.ViewMatrices;
	ModifiedViewMatrices.HackRemoveTemporalAAProjectionJitter();

	Parameters = *View.CachedViewUniformShaderParameters;
	View.SetupUniformBufferParameters(ModifiedViewMatrices, ModifiedViewMatrices, VolumeBounds, TVC_MAX, Parameters);
}

void SetupDownsampledTranslucencyViewParameters(
	const FViewInfo& View,
	FIntPoint TextureExtent,
	FIntRect ViewRect,
	ETranslucencyPass::Type TranslucencyPass,
	FViewUniformShaderParameters& DownsampledTranslucencyViewParameters)
{
	DownsampledTranslucencyViewParameters = *View.CachedViewUniformShaderParameters;

	FViewMatrices ViewMatrices = View.ViewMatrices;
	FViewMatrices PrevViewMatrices = View.PrevViewInfo.ViewMatrices;
	if (TranslucencyPass == ETranslucencyPass::TPT_TranslucencyAfterMotionBlur)
	{
		// Remove jitter from this pass
		ViewMatrices.HackRemoveTemporalAAProjectionJitter();
		PrevViewMatrices.HackRemoveTemporalAAProjectionJitter();
	}

	// Update the parts of DownsampledTranslucencyParameters which are dependent on the buffer size and view rect
	View.SetupViewRectUniformBufferParameters(
		DownsampledTranslucencyViewParameters,
		TextureExtent,
		ViewRect,
		ViewMatrices,
		PrevViewMatrices);

	// instead of using the expected ratio, use the actual dimensions to avoid rounding errors
	float ActualDownsampleX = float(ViewRect.Width()) / float(View.ViewRect.Width());
	float ActualDownsampleY = float(ViewRect.Height()) / float(View.ViewRect.Height());
	DownsampledTranslucencyViewParameters.LightProbeSizeRatioAndInvSizeRatio = FVector4f(ActualDownsampleX, ActualDownsampleY, 1.0f / ActualDownsampleX, 1.0f / ActualDownsampleY);
}

TRDGUniformBufferRef<FTranslucentBasePassUniformParameters> CreateTranslucentBasePassUniformBuffer(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const int32 ViewIndex,
	const FTranslucencyLightingVolumeTextures& TranslucencyLightingVolumeTextures,
	FRDGTextureRef SceneColorCopyTexture,
	const ESceneTextureSetupMode SceneTextureSetupMode,
	bool bLumenGIEnabled)
{
	FTranslucentBasePassUniformParameters& BasePassParameters = *GraphBuilder.AllocParameters<FTranslucentBasePassUniformParameters>();

	const auto GetRDG = [&](const TRefCountPtr<IPooledRenderTarget>& PooledRenderTarget, ERDGTextureFlags Flags = ERDGTextureFlags::None)
	{
		return GraphBuilder.RegisterExternalTexture(PooledRenderTarget, Flags);
	};

	SetupSharedBasePassParameters(GraphBuilder, View, bLumenGIEnabled, BasePassParameters.Shared);
	SetupSceneTextureUniformParameters(GraphBuilder, View.FeatureLevel, SceneTextureSetupMode, BasePassParameters.SceneTextures);
	Strata::BindStrataForwardPasslUniformParameters(GraphBuilder, View.StrataSceneData, BasePassParameters.Strata);

	const FLightSceneProxy* SelectedForwardDirectionalLightProxy = View.ForwardLightingResources.SelectedForwardDirectionalLightProxy;
	SetupLightCloudTransmittanceParameters(GraphBuilder, Scene, View, SelectedForwardDirectionalLightProxy ? SelectedForwardDirectionalLightProxy->GetLightSceneInfo() : nullptr, BasePassParameters.ForwardDirLightCloudShadow);

	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);

	// Material SSR
	{
		float PrevSceneColorPreExposureInvValue = 1.0f / View.PreExposure;

		if (View.HZB)
		{
			BasePassParameters.HZBTexture = View.HZB;
			BasePassParameters.HZBSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

			FRDGTextureRef PrevSceneColorTexture = SystemTextures.Black;
			FIntRect PrevSceneColorViewRect = FIntRect(0, 0, 1, 1);

			if (View.PrevViewInfo.CustomSSRInput.IsValid())
			{
				PrevSceneColorTexture = GetRDG(View.PrevViewInfo.CustomSSRInput.RT[0]);
				PrevSceneColorViewRect = View.PrevViewInfo.CustomSSRInput.ViewportRect;
				PrevSceneColorPreExposureInvValue = 1.0f / View.PrevViewInfo.SceneColorPreExposure;
			}
			else if (View.PrevViewInfo.TSRHistory.IsValid())
			{
				PrevSceneColorTexture = GetRDG(View.PrevViewInfo.TSRHistory.LowFrequency);
				PrevSceneColorViewRect = View.PrevViewInfo.TSRHistory.OutputViewportRect;
				PrevSceneColorPreExposureInvValue = 1.0f / View.PrevViewInfo.SceneColorPreExposure;
			}
			else if (View.PrevViewInfo.TemporalAAHistory.IsValid())
			{
				PrevSceneColorTexture = GetRDG(View.PrevViewInfo.TemporalAAHistory.RT[0]);
				PrevSceneColorViewRect = View.PrevViewInfo.TemporalAAHistory.ViewportRect;
				PrevSceneColorPreExposureInvValue = 1.0f / View.PrevViewInfo.SceneColorPreExposure;
			}
			else if (View.PrevViewInfo.ScreenSpaceRayTracingInput.IsValid())
			{
				PrevSceneColorTexture = GetRDG(View.PrevViewInfo.ScreenSpaceRayTracingInput);
				PrevSceneColorViewRect = View.PrevViewInfo.ViewRect;
				PrevSceneColorPreExposureInvValue = 1.0f / View.PrevViewInfo.SceneColorPreExposure;
			}

			BasePassParameters.PrevSceneColor = PrevSceneColorTexture;
			BasePassParameters.PrevSceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

			FScreenPassTextureViewportParameters PrevSceneColorParameters = GetScreenPassTextureViewportParameters(FScreenPassTextureViewport(PrevSceneColorTexture, PrevSceneColorViewRect));
			BasePassParameters.PrevSceneColorBilinearUVMin = PrevSceneColorParameters.UVViewportBilinearMin;
			BasePassParameters.PrevSceneColorBilinearUVMax = PrevSceneColorParameters.UVViewportBilinearMax;

			const FVector2f HZBUvFactor(
				float(View.ViewRect.Width()) / float(2 * View.HZBMipmap0Size.X),
				float(View.ViewRect.Height()) / float(2 * View.HZBMipmap0Size.Y)
			);
			const FVector4f HZBUvFactorAndInvFactorValue(
				HZBUvFactor.X,
				HZBUvFactor.Y,
				1.0f / HZBUvFactor.X,
				1.0f / HZBUvFactor.Y
			);

			BasePassParameters.HZBUvFactorAndInvFactor = HZBUvFactorAndInvFactorValue;
		}
		else
		{
			BasePassParameters.HZBTexture = SystemTextures.Black;
			BasePassParameters.HZBSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			BasePassParameters.PrevSceneColor = SystemTextures.Black;
			BasePassParameters.PrevSceneColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
			BasePassParameters.PrevSceneColorBilinearUVMin = FVector2f(0.0f, 0.0f);
			BasePassParameters.PrevSceneColorBilinearUVMax = FVector2f(1.0f, 1.0f);
		}

		BasePassParameters.ApplyVolumetricCloudOnTransparent = 0.0f;
		BasePassParameters.VolumetricCloudColor = nullptr;
		BasePassParameters.VolumetricCloudDepth = nullptr;
		BasePassParameters.VolumetricCloudColorSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		BasePassParameters.VolumetricCloudDepthSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
		if (IsVolumetricRenderTargetEnabled() && View.ViewState)
		{
			TRefCountPtr<IPooledRenderTarget> VolumetricReconstructRT = View.ViewState->VolumetricCloudRenderTarget.GetDstVolumetricReconstructRT();
			if (VolumetricReconstructRT.IsValid())
			{
				TRefCountPtr<IPooledRenderTarget> VolumetricReconstructRTDepth = View.ViewState->VolumetricCloudRenderTarget.GetDstVolumetricReconstructRTDepth();

				BasePassParameters.VolumetricCloudColor = VolumetricReconstructRT->GetRenderTargetItem().ShaderResourceTexture;
				BasePassParameters.VolumetricCloudDepth = VolumetricReconstructRTDepth->GetRenderTargetItem().ShaderResourceTexture;
				BasePassParameters.ApplyVolumetricCloudOnTransparent = 1.0f;
			}
		}
		if (BasePassParameters.VolumetricCloudColor == nullptr)
		{
			BasePassParameters.VolumetricCloudColor = GSystemTextures.BlackAlphaOneDummy->GetRenderTargetItem().ShaderResourceTexture;
			BasePassParameters.VolumetricCloudDepth = GSystemTextures.BlackDummy->GetRenderTargetItem().ShaderResourceTexture;
		}

		FIntPoint ViewportOffset = View.ViewRect.Min;
		FIntPoint ViewportExtent = View.ViewRect.Size();

		// Scene render targets might not exist yet; avoids NaNs.
		FIntPoint EffectiveBufferSize = GetSceneTextureExtent();
		EffectiveBufferSize.X = FMath::Max(EffectiveBufferSize.X, 1);
		EffectiveBufferSize.Y = FMath::Max(EffectiveBufferSize.Y, 1);

		if (View.PrevViewInfo.CustomSSRInput.IsValid())
		{
			ViewportOffset = View.PrevViewInfo.CustomSSRInput.ViewportRect.Min;
			ViewportExtent = View.PrevViewInfo.CustomSSRInput.ViewportRect.Size();
			EffectiveBufferSize = View.PrevViewInfo.CustomSSRInput.RT[0]->GetDesc().Extent;
		}
		else if (View.PrevViewInfo.TSRHistory.IsValid())
		{
			ViewportOffset = View.PrevViewInfo.TSRHistory.OutputViewportRect.Min;
			ViewportExtent = View.PrevViewInfo.TSRHistory.OutputViewportRect.Size();
			EffectiveBufferSize = View.PrevViewInfo.TSRHistory.LowFrequency->GetDesc().Extent;
		}
		else if (View.PrevViewInfo.TemporalAAHistory.IsValid())
		{
			ViewportOffset = View.PrevViewInfo.TemporalAAHistory.ViewportRect.Min;
			ViewportExtent = View.PrevViewInfo.TemporalAAHistory.ViewportRect.Size();
			EffectiveBufferSize = View.PrevViewInfo.TemporalAAHistory.RT[0]->GetDesc().Extent;
		}
		else if (View.PrevViewInfo.ScreenSpaceRayTracingInput.IsValid())
		{
			ViewportOffset = View.PrevViewInfo.ViewRect.Min;
			ViewportExtent = View.PrevViewInfo.ViewRect.Size();
			EffectiveBufferSize = View.PrevViewInfo.ScreenSpaceRayTracingInput->GetDesc().Extent;
		}

		FVector2f InvBufferSize(1.0f / float(EffectiveBufferSize.X), 1.0f / float(EffectiveBufferSize.Y));

		FVector4f ScreenPosToPixelValue(
			ViewportExtent.X * 0.5f * InvBufferSize.X,
			-ViewportExtent.Y * 0.5f * InvBufferSize.Y,
			(ViewportExtent.X * 0.5f + ViewportOffset.X) * InvBufferSize.X,
			(ViewportExtent.Y * 0.5f + ViewportOffset.Y) * InvBufferSize.Y);

		BasePassParameters.PrevScreenPositionScaleBias = ScreenPosToPixelValue;
		BasePassParameters.PrevSceneColorPreExposureInv = PrevSceneColorPreExposureInvValue;
		BasePassParameters.SSRQuality = ShouldRenderTranslucencyScreenSpaceReflections(View) ? GetSSRQuality() : 0;
	}

	// Translucency Lighting Volume
	BasePassParameters.TranslucencyLightingVolume = GetTranslucencyLightingVolumeParameters(GraphBuilder, TranslucencyLightingVolumeTextures, ViewIndex);
	BasePassParameters.LumenParameters = GetLumenTranslucencyLightingParameters(GraphBuilder, View.LumenTranslucencyGIVolume);

	const bool bLumenGIHandlingSkylight = bLumenGIEnabled
		&& BasePassParameters.LumenParameters.TranslucencyGIGridSize.Z > 0;

	BasePassParameters.Shared.UseBasePassSkylight = bLumenGIHandlingSkylight ? 0 : 1;

	BasePassParameters.SceneColorCopyTexture = SystemTextures.Black;
	BasePassParameters.SceneColorCopySampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	if (SceneColorCopyTexture)
	{
		BasePassParameters.SceneColorCopyTexture = SceneColorCopyTexture;
	}

	BasePassParameters.EyeAdaptationTexture = GetEyeAdaptationTexture(GraphBuilder, View);
	BasePassParameters.PreIntegratedGFTexture = GSystemTextures.PreintegratedGF->GetRenderTargetItem().ShaderResourceTexture;
	BasePassParameters.PreIntegratedGFSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();

	return GraphBuilder.CreateUniformBuffer(&BasePassParameters);
}

static FViewShaderParameters GetSeparateTranslucencyViewParameters(const FViewInfo& View, FIntPoint TextureExtent, float ViewportScale, ETranslucencyPass::Type TranslucencyPass)
{
	FViewShaderParameters ViewParameters;
	const bool bIsPostMotionBlur = (TranslucencyPass == ETranslucencyPass::TPT_TranslucencyAfterMotionBlur);

	if (ViewportScale == 1.0f && !bIsPostMotionBlur)
	{
		// We can use the existing view uniform buffers if no downsampling is required and is not in the post-motionblur pass
		ViewParameters = View.GetShaderParameters();
	}	
	else if (bIsPostMotionBlur)
	{
		// Full-scale post-motionblur pass
		FViewUniformShaderParameters ViewUniformParameters;
		SetupPostMotionBlurTranslucencyViewParameters(View, ViewUniformParameters);

		ViewParameters.View = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(ViewUniformParameters, UniformBuffer_SingleFrame);

		if (const FViewInfo* InstancedView = View.GetInstancedView())
		{
			SetupPostMotionBlurTranslucencyViewParameters(*InstancedView, ViewUniformParameters);

			ViewParameters.InstancedView = TUniformBufferRef<FInstancedViewUniformShaderParameters>::CreateUniformBufferImmediate(
				reinterpret_cast<const FInstancedViewUniformShaderParameters&>(ViewUniformParameters),
				UniformBuffer_SingleFrame);
		}
	}
	else
	{
		// Downsampled post-DOF or post-motionblur pass
		FViewUniformShaderParameters DownsampledTranslucencyViewParameters;
		SetupDownsampledTranslucencyViewParameters(
			View,
			TextureExtent,
			GetScaledRect(View.ViewRect, ViewportScale),
			TranslucencyPass,
			DownsampledTranslucencyViewParameters);

		ViewParameters.View = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(DownsampledTranslucencyViewParameters, UniformBuffer_SingleFrame);

		if (const FViewInfo* InstancedView = View.GetInstancedView())
		{
			SetupDownsampledTranslucencyViewParameters(
				*InstancedView,
				TextureExtent,
				GetScaledRect(InstancedView->ViewRect, ViewportScale),
				TranslucencyPass,
				DownsampledTranslucencyViewParameters);

			ViewParameters.InstancedView = TUniformBufferRef<FInstancedViewUniformShaderParameters>::CreateUniformBufferImmediate(
				reinterpret_cast<const FInstancedViewUniformShaderParameters&>(DownsampledTranslucencyViewParameters),
				UniformBuffer_SingleFrame);
		}
	}

	return ViewParameters;
}

static void RenderViewTranslucencyInner(
	FRHICommandListImmediate& RHICmdList,
	const FSceneRenderer& SceneRenderer,
	const FViewInfo& View,
	const FScreenPassTextureViewport Viewport,
	const float ViewportScale,
	ETranslucencyPass::Type TranslucencyPass,
	FRDGParallelCommandListSet* ParallelCommandListSet,
	const FInstanceCullingDrawParams& InstanceCullingDrawParams)
{
	FMeshPassProcessorRenderState DrawRenderState;
	if (TranslucencyPass == ETranslucencyPass::TPT_TranslucencyAfterMotionBlur)
	{
		// No depth test in post-motionblur translucency
		DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_Always>::GetRHI());
	}
	else
	{
		DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
	}
	
	SceneRenderer.SetStereoViewport(RHICmdList, View, ViewportScale);

	if (!View.Family->UseDebugViewPS())
	{
		QUICK_SCOPE_CYCLE_COUNTER(RenderTranslucencyParallel_Start_FDrawSortedTransAnyThreadTask);

		const EMeshPass::Type MeshPass = TranslucencyPassToMeshPass(TranslucencyPass);
		View.ParallelMeshDrawCommandPasses[MeshPass].DispatchDraw(ParallelCommandListSet, RHICmdList, &InstanceCullingDrawParams);
	}

	if (IsMainTranslucencyPass(TranslucencyPass))
	{
		if (ParallelCommandListSet)
		{
			ParallelCommandListSet->SetStateOnCommandList(RHICmdList);
		}

		View.SimpleElementCollector.DrawBatchedElements(RHICmdList, DrawRenderState, View, EBlendModeFilter::Translucent, SDPG_World);
		View.SimpleElementCollector.DrawBatchedElements(RHICmdList, DrawRenderState, View, EBlendModeFilter::Translucent, SDPG_Foreground);

		// editor and debug rendering
		if (View.bHasTranslucentViewMeshElements)
		{
			{
				QUICK_SCOPE_CYCLE_COUNTER(RenderTranslucencyParallel_SDPG_World);

				DrawDynamicMeshPass(View, RHICmdList,
					[&View, &DrawRenderState, TranslucencyPass](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
				{
					FBasePassMeshProcessor PassMeshProcessor(
						View.Family->Scene->GetRenderScene(),
						View.GetFeatureLevel(),
						&View,
						DrawRenderState,
						DynamicMeshPassContext,
						FBasePassMeshProcessor::EFlags::CanUseDepthStencil,
						TranslucencyPass);

					const uint64 DefaultBatchElementMask = ~0ull;

					for (int32 MeshIndex = 0; MeshIndex < View.ViewMeshElements.Num(); MeshIndex++)
					{
						const FMeshBatch& MeshBatch = View.ViewMeshElements[MeshIndex];
						PassMeshProcessor.AddMeshBatch(MeshBatch, DefaultBatchElementMask, nullptr);
					}
				});
			}

			{
				QUICK_SCOPE_CYCLE_COUNTER(RenderTranslucencyParallel_SDPG_Foreground);

				DrawDynamicMeshPass(View, RHICmdList,
					[&View, &DrawRenderState, TranslucencyPass](FDynamicPassMeshDrawListContext* DynamicMeshPassContext)
				{
					FBasePassMeshProcessor PassMeshProcessor(
						View.Family->Scene->GetRenderScene(),
						View.GetFeatureLevel(),
						&View,
						DrawRenderState,
						DynamicMeshPassContext,
						FBasePassMeshProcessor::EFlags::CanUseDepthStencil,
						TranslucencyPass);

					const uint64 DefaultBatchElementMask = ~0ull;

					for (int32 MeshIndex = 0; MeshIndex < View.TopViewMeshElements.Num(); MeshIndex++)
					{
						const FMeshBatch& MeshBatch = View.TopViewMeshElements[MeshIndex];
						PassMeshProcessor.AddMeshBatch(MeshBatch, DefaultBatchElementMask, nullptr);
					}
				});
			}
		}

		if (ParallelCommandListSet)
		{
			RHICmdList.EndRenderPass();
		}
	}
}

BEGIN_SHADER_PARAMETER_STRUCT(FTranslucentBasePassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FViewShaderParameters, View)
	SHADER_PARAMETER_STRUCT_REF(FReflectionCaptureShaderData, ReflectionCapture)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FTranslucentBasePassUniformParameters, BasePass)
	SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
	SHADER_PARAMETER_STRUCT_INCLUDE(FVirtualShadowMapSamplingParameters, VirtualShadowMapSamplingParameters)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

static void RenderTranslucencyViewInner(
	FRDGBuilder& GraphBuilder,
	const FSceneRenderer& SceneRenderer,
	FViewInfo& View,
	FScreenPassTextureViewport Viewport,
	float ViewportScale,
	FRDGTextureMSAA SceneColorTexture,
	ERenderTargetLoadAction SceneColorLoadAction,
	FRDGTextureRef SceneDepthTexture,
	TRDGUniformBufferRef<FTranslucentBasePassUniformParameters> BasePassParameters,
	ETranslucencyPass::Type TranslucencyPass,
	bool bResolveColorTexture,
	bool bRenderInParallel,
	FInstanceCullingManager& InstanceCullingManager)
{
	if (!View.ShouldRenderView())
	{
		return;
	}

	if (SceneColorLoadAction == ERenderTargetLoadAction::EClear)
	{
		AddClearRenderTargetPass(GraphBuilder, SceneColorTexture.Target);
	}

	View.BeginRenderView();

	FTranslucentBasePassParameters* PassParameters = GraphBuilder.AllocParameters<FTranslucentBasePassParameters>();
	PassParameters->View = GetSeparateTranslucencyViewParameters(View, Viewport.Extent, ViewportScale, TranslucencyPass);
	PassParameters->ReflectionCapture = View.ReflectionCaptureUniformBuffer;
	PassParameters->BasePass = BasePassParameters;
	PassParameters->VirtualShadowMapSamplingParameters = SceneRenderer.VirtualShadowMapArray.GetSamplingParameters(GraphBuilder);
	PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneColorTexture.Target, ERenderTargetLoadAction::ELoad);
	if (TranslucencyPass != ETranslucencyPass::TPT_TranslucencyAfterMotionBlur)
	{
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite);
	}
	PassParameters->RenderTargets.ShadingRateTexture = GVRSImageManager.GetVariableRateShadingImage(GraphBuilder, SceneRenderer.ViewFamily, nullptr);
	PassParameters->RenderTargets.ResolveRect = FResolveRect(Viewport.Rect);

	const EMeshPass::Type MeshPass = TranslucencyPassToMeshPass(TranslucencyPass);
	View.ParallelMeshDrawCommandPasses[MeshPass].BuildRenderingCommands(GraphBuilder, SceneRenderer.Scene->GPUScene, PassParameters->InstanceCullingDrawParams);

	if (bRenderInParallel)
	{
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("Translucency(%s Parallel) %dx%d",
				TranslucencyPassToString(TranslucencyPass),
				int32(View.ViewRect.Width() * ViewportScale),
				int32(View.ViewRect.Height() * ViewportScale)),
			PassParameters,
			ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass,
			[&SceneRenderer, &View, PassParameters, ViewportScale, Viewport, TranslucencyPass](FRHICommandListImmediate& RHICmdList)
		{
			FRDGParallelCommandListSet ParallelCommandListSet(RHICmdList, GET_STATID(STAT_CLP_Translucency), SceneRenderer, View, FParallelCommandListBindings(PassParameters), ViewportScale);
			RenderViewTranslucencyInner(RHICmdList, SceneRenderer, View, Viewport, ViewportScale, TranslucencyPass, &ParallelCommandListSet, PassParameters->InstanceCullingDrawParams);
		});
	}
	else
	{
		GraphBuilder.AddPass(
			RDG_EVENT_NAME("Translucency(%s) %dx%d",
				TranslucencyPassToString(TranslucencyPass),
				int32(View.ViewRect.Width() * ViewportScale),
				int32(View.ViewRect.Height() * ViewportScale)),
			PassParameters,
			ERDGPassFlags::Raster,
			[&SceneRenderer, &View, ViewportScale, Viewport, TranslucencyPass, PassParameters](FRHICommandListImmediate& RHICmdList)
		{
			RenderViewTranslucencyInner(RHICmdList, SceneRenderer, View, Viewport, ViewportScale, TranslucencyPass, nullptr, PassParameters->InstanceCullingDrawParams);
		});
	}

	if (bResolveColorTexture)
	{
		AddResolveSceneColorPass(GraphBuilder, View, SceneColorTexture);
	}
}

void FDeferredShadingSceneRenderer::RenderTranslucencyInner(
	FRDGBuilder& GraphBuilder,
	const FMinimalSceneTextures& SceneTextures,
	const FTranslucencyLightingVolumeTextures& TranslucentLightingVolumeTextures,
	FTranslucencyPassResourcesMap* OutTranslucencyResourceMap,
	FRDGTextureMSAA SharedDepthTexture,
	ETranslucencyView ViewsToRender,
	FRDGTextureRef SceneColorCopyTexture,
	ETranslucencyPass::Type TranslucencyPass,
	FInstanceCullingManager& InstanceCullingManager)
{
	if (!ShouldRenderTranslucency(TranslucencyPass))
	{
		return;
	}

	RDG_EVENT_SCOPE(GraphBuilder, "%s", TranslucencyPassToString(TranslucencyPass));
	RDG_GPU_STAT_SCOPE(GraphBuilder, Translucency);
	RDG_WAIT_FOR_TASKS_CONDITIONAL(GraphBuilder, IsTranslucencyWaitForTasksEnabled());

	const bool bIsModulate = TranslucencyPass == ETranslucencyPass::TPT_TranslucencyAfterDOFModulate;
	const bool bDepthTest = TranslucencyPass != ETranslucencyPass::TPT_TranslucencyAfterMotionBlur;
	const bool bRenderInParallel = IsParallelTranslucencyEnabled();
	const bool bIsScalingTranslucency = SeparateTranslucencyDimensions.Scale < 1.0f;
	const bool bRenderInSeparateTranslucency = IsSeparateTranslucencyEnabled(TranslucencyPass, SeparateTranslucencyDimensions.Scale);

	// Can't reference scene color in scene textures. Scene color copy is used instead.
	ESceneTextureSetupMode SceneTextureSetupMode = ESceneTextureSetupMode::All;
	EnumRemoveFlags(SceneTextureSetupMode, ESceneTextureSetupMode::SceneColor);

	if (bRenderInSeparateTranslucency)
	{
		// Create resources shared by each view (each view data is tiled into each of the render target resources)
		FRDGTextureMSAA SharedColorTexture;
		{
			static const TCHAR* kTranslucencyColorTextureName[] = {
				TEXT("Translucency.BeforeDistortion.Color"),
				TEXT("Translucency.AfterDOF.Color"),
				TEXT("Translucency.AfterDOF.Modulate"),
				TEXT("Translucency.AfterMotionBlur.Color"),
				TEXT("Translucency.All.Color"),
			};
			static_assert(UE_ARRAY_COUNT(kTranslucencyColorTextureName) == int32(ETranslucencyPass::TPT_MAX), "Fix me");

			const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
				SeparateTranslucencyDimensions.Extent,
				bIsModulate ? PF_FloatR11G11B10 : PF_FloatRGBA,
				bIsModulate ? FClearValueBinding::White : FClearValueBinding::Black,
				TexCreate_RenderTargetable | TexCreate_ShaderResource,
				1,
				SeparateTranslucencyDimensions.NumSamples);

			SharedColorTexture = CreateTextureMSAA(
				GraphBuilder, Desc,
				kTranslucencyColorTextureName[int32(TranslucencyPass)],
				bIsModulate ? GFastVRamConfig.SeparateTranslucencyModulate : GFastVRamConfig.SeparateTranslucency);
		}

		for (int32 ViewIndex = 0, NumProcessedViews = 0; ViewIndex < Views.Num(); ++ViewIndex, ++NumProcessedViews)
		{
			FViewInfo& View = Views[ViewIndex];
			const ETranslucencyView TranslucencyView = GetTranslucencyView(View);

			if (!EnumHasAnyFlags(TranslucencyView, ViewsToRender))
			{
				continue;
			}

			RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
			RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);

			FIntRect ScaledViewRect = GetScaledRect(View.ViewRect, SeparateTranslucencyDimensions.Scale);

			const FScreenPassTextureViewport SeparateTranslucencyViewport = SeparateTranslucencyDimensions.GetInstancedStereoViewport(View, View.InstancedStereoWidth);
			const bool bCompositeBackToSceneColor = IsMainTranslucencyPass(TranslucencyPass) || EnumHasAnyFlags(TranslucencyView, ETranslucencyView::UnderWater);
			const bool bLumenGIEnabled = GetViewPipelineState(View).DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen;

			/** Separate translucency color is either composited immediately or later during post processing. If done immediately, it's because the view doesn't support
			 *  compositing (e.g. we're rendering an underwater view) or because we're downsampling the main translucency pass. In this case, we use a local set of
			 *  textures instead of the external ones passed in.
			 */
			FRDGTextureMSAA SeparateTranslucencyColorTexture = SharedColorTexture;

			// NOTE: No depth test on post-motionblur translucency
			FRDGTextureMSAA SeparateTranslucencyDepthTexture;
			if (bDepthTest)
			{
				SeparateTranslucencyDepthTexture = SharedDepthTexture;
			}

			AddBeginSeparateTranslucencyTimerPass(GraphBuilder, View, TranslucencyPass);

			const ERenderTargetLoadAction SeparateTranslucencyColorLoadAction = NumProcessedViews == 0 || View.Family->bMultiGPUForkAndJoin
				? ERenderTargetLoadAction::EClear
				: ERenderTargetLoadAction::ELoad;

			RenderTranslucencyViewInner(
				GraphBuilder,
				*this,
				View,
				SeparateTranslucencyViewport,
				SeparateTranslucencyDimensions.Scale,
				SeparateTranslucencyColorTexture,
				SeparateTranslucencyColorLoadAction,
				SeparateTranslucencyDepthTexture.Target,
				CreateTranslucentBasePassUniformBuffer(GraphBuilder, Scene, View, ViewIndex, TranslucentLightingVolumeTextures, SceneColorCopyTexture, SceneTextureSetupMode, bLumenGIEnabled),
				TranslucencyPass,
				!bCompositeBackToSceneColor,
				bRenderInParallel,
				InstanceCullingManager);

			{
				FTranslucencyPassResources& TranslucencyPassResources = OutTranslucencyResourceMap->Get(ViewIndex, TranslucencyPass);
				TranslucencyPassResources.ViewRect = ScaledViewRect;
				TranslucencyPassResources.ColorTexture = SharedColorTexture;
				TranslucencyPassResources.DepthTexture = SharedDepthTexture;
			}

			if (bCompositeBackToSceneColor)
			{
				FRDGTextureRef SeparateTranslucencyDepthResolve = nullptr;
				FRDGTextureRef SceneDepthResolve = nullptr;
				if (TranslucencyPass != ETranslucencyPass::TPT_TranslucencyAfterMotionBlur)
				{
					::AddResolveSceneDepthPass(GraphBuilder, View, SeparateTranslucencyDepthTexture);

					SeparateTranslucencyDepthResolve = SeparateTranslucencyDepthTexture.Resolve;
					SceneDepthResolve = SceneTextures.Depth.Resolve;
				}

				FTranslucencyPassResources& TranslucencyPassResources = OutTranslucencyResourceMap->Get(ViewIndex, TranslucencyPass);

				FTranslucencyComposition TranslucencyComposition;
				TranslucencyComposition.Operation = FTranslucencyComposition::EOperation::ComposeToExistingSceneColor;
				TranslucencyComposition.SceneColor = FScreenPassTexture(SceneTextures.Color.Target, View.ViewRect);
				TranslucencyComposition.SceneDepth = FScreenPassTexture(SceneTextures.Depth.Resolve, View.ViewRect);
				TranslucencyComposition.OutputViewport = FScreenPassTextureViewport(SceneTextures.Depth.Resolve, View.ViewRect);

				FScreenPassTexture UpscaledTranslucency = TranslucencyComposition.AddPass(
					GraphBuilder, View, TranslucencyPassResources);

				ensure(View.ViewRect == UpscaledTranslucency.ViewRect);
				ensure(UpscaledTranslucency.Texture == SceneTextures.Color.Target);

				//Invalidate.
				TranslucencyPassResources = FTranslucencyPassResources();
			}
			else
			{
				if (TranslucencyPass == ETranslucencyPass::TPT_TranslucencyAfterDOFModulate)
				{
					FTranslucencyPassResources& TranslucencyPassResources = OutTranslucencyResourceMap->Get(ViewIndex, ETranslucencyPass::TPT_TranslucencyAfterDOF);
					ensure(TranslucencyPassResources.ViewRect == ScaledViewRect);
					ensure(TranslucencyPassResources.DepthTexture == SharedDepthTexture);
					TranslucencyPassResources.ColorModulateTexture = SharedColorTexture;
				}
				else
				{
					check(!bIsModulate);
				}
			}

			AddEndSeparateTranslucencyTimerPass(GraphBuilder, View, TranslucencyPass);
			++NumProcessedViews;
		}
	}
	else
	{
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			FViewInfo& View = Views[ViewIndex];
			const ETranslucencyView TranslucencyView = GetTranslucencyView(View);

			if (!EnumHasAnyFlags(TranslucencyView, ViewsToRender))
			{
				continue;
			}

			RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);
			RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);

			AddBeginTranslucencyTimerPass(GraphBuilder, View);

			const ERenderTargetLoadAction SceneColorLoadAction = ERenderTargetLoadAction::ELoad;
			const FScreenPassTextureViewport Viewport(SceneTextures.Color.Target, View.ViewRect);
			const float ViewportScale = 1.0f;
			const bool bResolveColorTexture = false;
			const bool bLumenGIEnabled = GetViewPipelineState(View).DiffuseIndirectMethod == EDiffuseIndirectMethod::Lumen;

			RenderTranslucencyViewInner(
				GraphBuilder,
				*this,
				View,
				Viewport,
				ViewportScale,
				SceneTextures.Color,
				SceneColorLoadAction,
				SceneTextures.Depth.Target,
				CreateTranslucentBasePassUniformBuffer(GraphBuilder, Scene, View, ViewIndex, TranslucentLightingVolumeTextures, SceneColorCopyTexture, SceneTextureSetupMode, bLumenGIEnabled),
				TranslucencyPass,
				bResolveColorTexture,
				bRenderInParallel,
				InstanceCullingManager);

			AddEndTranslucencyTimerPass(GraphBuilder, View);
		}
	}
}

void FDeferredShadingSceneRenderer::RenderTranslucency(
	FRDGBuilder& GraphBuilder,
	const FMinimalSceneTextures& SceneTextures,
	const FTranslucencyLightingVolumeTextures& TranslucentLightingVolumeTextures,
	FTranslucencyPassResourcesMap* OutTranslucencyResourceMap,
	ETranslucencyView ViewsToRender,
	FInstanceCullingManager& InstanceCullingManager)
{
	if (!EnumHasAnyFlags(ViewsToRender, ETranslucencyView::UnderWater | ETranslucencyView::AboveWater))
	{
		return;
	}

	FRDGTextureRef SceneColorCopyTexture = nullptr;

	if (EnumHasAnyFlags(ViewsToRender, ETranslucencyView::AboveWater))
	{
		SceneColorCopyTexture = AddCopySceneColorPass(GraphBuilder, Views, SceneTextures.Color);
	}

	const auto ShouldRenderView = [&](const FViewInfo& View, ETranslucencyView TranslucencyView)
	{
		return View.ShouldRenderView() && EnumHasAnyFlags(TranslucencyView, ViewsToRender);
	};

	// Create a shared depth texture at the correct resolution.
	FRDGTextureMSAA SharedDepthTexture;
	const bool bIsScalingTranslucency = SeparateTranslucencyDimensions.Scale < 1.0f;
	if (bIsScalingTranslucency)
	{
		const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
			SeparateTranslucencyDimensions.Extent,
			PF_DepthStencil,
			FClearValueBinding::DepthFar,
			TexCreate_DepthStencilTargetable | TexCreate_ShaderResource,
			1,
			SeparateTranslucencyDimensions.NumSamples);

		SharedDepthTexture = CreateTextureMSAA(
			GraphBuilder, Desc,
			TEXT("Translucency.Depth"),
			GFastVRamConfig.SeparateTranslucencyModulate); // TODO: this should be SeparateTranslucency, but is what the code was doing

		// Downscale the depth buffer for each individual view, but shared accross all translucencies.
		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
		{
			FViewInfo& View = Views[ViewIndex];
			const ETranslucencyView TranslucencyView = GetTranslucencyView(View);

			if (!ShouldRenderView(View, TranslucencyView))
			{
				continue;
			}

			const FScreenPassTextureViewport SeparateTranslucencyViewport = SeparateTranslucencyDimensions.GetInstancedStereoViewport(View, View.InstancedStereoWidth);
			AddDownsampleDepthPass(
				GraphBuilder, View,
				FScreenPassTexture(SceneTextures.Depth.Resolve, View.ViewRect),
				FScreenPassRenderTarget(SharedDepthTexture.Target, SeparateTranslucencyViewport.Rect, ViewIndex == 0 ? ERenderTargetLoadAction::EClear : ERenderTargetLoadAction::ELoad),
				EDownsampleDepthFilter::Point);
		}
	}
	else
	{
		// Uses the existing depth buffer for depth testing the translucency.
		SharedDepthTexture = SceneTextures.Depth;
	}

	if (ViewFamily.AllowTranslucencyAfterDOF())
	{
		RenderTranslucencyInner(GraphBuilder, SceneTextures, TranslucentLightingVolumeTextures, OutTranslucencyResourceMap, SharedDepthTexture, ViewsToRender, SceneColorCopyTexture, ETranslucencyPass::TPT_StandardTranslucency, InstanceCullingManager);
		if (GetHairStrandsComposition() == EHairStrandsCompositionType::AfterTranslucentBeforeTranslucentAfterDOF)
		{
			RenderHairComposition(GraphBuilder, Views, SceneTextures.Color.Target, SceneTextures.Depth.Target);
		}
		RenderTranslucencyInner(GraphBuilder, SceneTextures, TranslucentLightingVolumeTextures, OutTranslucencyResourceMap, SharedDepthTexture, ViewsToRender, SceneColorCopyTexture, ETranslucencyPass::TPT_TranslucencyAfterDOF, InstanceCullingManager);
		RenderTranslucencyInner(GraphBuilder, SceneTextures, TranslucentLightingVolumeTextures, OutTranslucencyResourceMap, SharedDepthTexture, ViewsToRender, SceneColorCopyTexture, ETranslucencyPass::TPT_TranslucencyAfterDOFModulate, InstanceCullingManager);
		RenderTranslucencyInner(GraphBuilder, SceneTextures, TranslucentLightingVolumeTextures, OutTranslucencyResourceMap, SharedDepthTexture, ViewsToRender, SceneColorCopyTexture, ETranslucencyPass::TPT_TranslucencyAfterMotionBlur, InstanceCullingManager);
	}
	else // Otherwise render translucent primitives in a single bucket.
	{
		RenderTranslucencyInner(GraphBuilder, SceneTextures, TranslucentLightingVolumeTextures, OutTranslucencyResourceMap, SharedDepthTexture, ViewsToRender, SceneColorCopyTexture, ETranslucencyPass::TPT_AllTranslucency, InstanceCullingManager);
	}

	bool bUpscalePostDOFTranslucency = true;
	FRDGTextureRef SharedUpscaledPostDOFTranslucencyColor = nullptr;
	if (bUpscalePostDOFTranslucency)
	{
		const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
			SceneTextures.Color.Resolve->Desc.Extent,
			PF_FloatRGBA,
			FClearValueBinding::Black,
			TexCreate_RenderTargetable | TexCreate_ShaderResource);

		SharedUpscaledPostDOFTranslucencyColor = GraphBuilder.CreateTexture(
			Desc, TEXT("Translucency.PostDOF.UpscaledColor"));
	}

	// Upscale to full res.
	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		FViewInfo& View = Views[ViewIndex];
		const ETranslucencyView TranslucencyView = GetTranslucencyView(View);

		if (!ShouldRenderView(View, TranslucencyView))
		{
			continue;
		}

		// Upscale the responsive AA into original depth buffer.
		bool bUpscaleResponsiveAA = (
			IsTemporalAccumulationBasedMethod(View.AntiAliasingMethod) &&
			SharedDepthTexture.Target != SceneTextures.Depth.Target);
		if (bUpscaleResponsiveAA)
		{
			const FScreenPassTextureViewport SeparateTranslucencyViewport = SeparateTranslucencyDimensions.GetInstancedStereoViewport(View, View.InstancedStereoWidth);
			AddUpsampleResponsiveAAPass(
				GraphBuilder,
				View,
				FScreenPassTexture(SharedDepthTexture.Target, SeparateTranslucencyViewport.Rect),
				/* OutputDepthTexture = */ SceneTextures.Depth.Target);
		}

		FTranslucencyPassResources& TranslucencyPassResources = OutTranslucencyResourceMap->Get(ViewIndex, ETranslucencyPass::TPT_TranslucencyAfterDOF);
		if (SharedUpscaledPostDOFTranslucencyColor && TranslucencyPassResources.IsValid() && TranslucencyPassResources.ViewRect.Size() != View.ViewRect.Size())
		{
			FTranslucencyComposition TranslucencyComposition;
			TranslucencyComposition.Operation = FTranslucencyComposition::EOperation::UpscaleOnly;
			TranslucencyComposition.SceneDepth = FScreenPassTexture(SceneTextures.Depth.Resolve, View.ViewRect);
			TranslucencyComposition.OutputViewport = FScreenPassTextureViewport(SceneTextures.Depth.Resolve, View.ViewRect);

			FScreenPassTexture UpscaledTranslucency = TranslucencyComposition.AddPass(
				GraphBuilder, View, TranslucencyPassResources);

			TranslucencyPassResources.ViewRect = UpscaledTranslucency.ViewRect;
			TranslucencyPassResources.ColorTexture = FRDGTextureMSAA(UpscaledTranslucency.Texture);
			TranslucencyPassResources.DepthTexture = FRDGTextureMSAA();
		}
	}
}
