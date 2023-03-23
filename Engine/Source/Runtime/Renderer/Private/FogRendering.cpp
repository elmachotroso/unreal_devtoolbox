// Copyright Epic Games, Inc. All Rights Reserved.

#include "FogRendering.h"
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"
#include "Engine/TextureCube.h"
#include "PipelineStateCache.h"
#include "SingleLayerWaterRendering.h"
#include "ScreenPass.h"

DECLARE_GPU_STAT(Fog);

#if UE_ENABLE_DEBUG_DRAWING
static TAutoConsoleVariable<float> CVarFogStartDistance(
	TEXT("r.FogStartDistance"),
	-1.0f,
	TEXT("Allows to override the FogStartDistance setting (needs ExponentialFog in the level).\n")
	TEXT(" <0: use default settings (default: -1)\n")
	TEXT(">=0: override settings by the given value (in world units)"),
	ECVF_Cheat | ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarFogDensity(
	TEXT("r.FogDensity"),
	-1.0f,
	TEXT("Allows to override the FogDensity setting (needs ExponentialFog in the level).\n")
	TEXT("Using a strong value allows to quickly see which pixel are affected by fog.\n")
	TEXT("Using a start distance allows to cull pixels are can speed up rendering.\n")
	TEXT(" <0: use default settings (default: -1)\n")
	TEXT(">=0: override settings by the given value (0:off, 1=very dense fog)"),
	ECVF_Cheat | ECVF_RenderThreadSafe);
#endif

static TAutoConsoleVariable<int32> CVarFog(
	TEXT("r.Fog"),
	1,
	TEXT(" 0: disabled\n")
	TEXT(" 1: enabled (default)"),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<bool> CVarFogUseDepthBounds(
	TEXT("r.FogUseDepthBounds"),
	true,
	TEXT("Allows enable depth bounds optimization on fog full screen pass.\n")
	TEXT(" false: disabled\n")
	TEXT(" true: enabled (default)"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<float> CVarUpsampleJitterMultiplier(
	TEXT("r.VolumetricFog.UpsampleJitterMultiplier"),
		0.0f,
	TEXT("Multiplier for random offset value used to jitter the sample position of the 3D fog volume to hide fog pixelization due to sampling from a lower resolution texture."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

IMPLEMENT_GLOBAL_SHADER_PARAMETER_STRUCT(FFogUniformParameters, "FogStruct");

void SetupFogUniformParameters(FRDGBuilder& GraphBuilder, const FViewInfo& View, FFogUniformParameters& OutParameters)
{
	// Exponential Height Fog
	{
		const FTexture* Cubemap = GWhiteTextureCube;

		if (View.FogInscatteringColorCubemap)
		{
			Cubemap = View.FogInscatteringColorCubemap->GetResource();
		}

		OutParameters.ExponentialFogParameters = View.ExponentialFogParameters;
		OutParameters.ExponentialFogColorParameter = FVector4f(View.ExponentialFogColor, 1.0f - View.FogMaxOpacity);
		OutParameters.ExponentialFogParameters2 = View.ExponentialFogParameters2;
		OutParameters.ExponentialFogParameters3 = View.ExponentialFogParameters3;
		OutParameters.SinCosInscatteringColorCubemapRotation = View.SinCosInscatteringColorCubemapRotation;
		OutParameters.FogInscatteringTextureParameters = (FVector3f)View.FogInscatteringTextureParameters;
		OutParameters.InscatteringLightDirection = (FVector3f)View.InscatteringLightDirection;
		OutParameters.InscatteringLightDirection.W = View.bUseDirectionalInscattering ? FMath::Max(0.f, View.DirectionalInscatteringStartDistance) : -1.f;
		OutParameters.DirectionalInscatteringColor = FVector4f(FVector3f(View.DirectionalInscatteringColor), FMath::Clamp(View.DirectionalInscatteringExponent, 0.000001f, 1000.0f));
		OutParameters.FogInscatteringColorCubemap = Cubemap->TextureRHI;
		OutParameters.FogInscatteringColorSampler = TStaticSamplerState<SF_Trilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	}

	// Volumetric Fog
	{
		if (View.VolumetricFogResources.IntegratedLightScatteringTexture)
		{
			OutParameters.IntegratedLightScattering = View.VolumetricFogResources.IntegratedLightScatteringTexture;
			OutParameters.ApplyVolumetricFog = 1.0f;
		}
		else
		{
			const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);
			OutParameters.IntegratedLightScattering = SystemTextures.VolumetricBlackAlphaOne;
			OutParameters.ApplyVolumetricFog = 0.0f;
		}
		OutParameters.IntegratedLightScatteringSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	}
}

TRDGUniformBufferRef<FFogUniformParameters> CreateFogUniformBuffer(FRDGBuilder& GraphBuilder, const FViewInfo& View)
{
	auto* FogStruct = GraphBuilder.AllocParameters<FFogUniformParameters>();
	SetupFogUniformParameters(GraphBuilder, View, *FogStruct);
	return GraphBuilder.CreateUniformBuffer(FogStruct);
}

/** A vertex shader for rendering height fog. */
class FHeightFogVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHeightFogVS);
	SHADER_USE_PARAMETER_STRUCT(FHeightFogVS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHeightFogVS, "/Engine/Private/HeightFogVertexShader.usf", "Main", SF_Vertex);

/** A pixel shader for rendering exponential height fog. */
class FExponentialHeightFogPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FExponentialHeightFogPS);
	SHADER_USE_PARAMETER_STRUCT(FExponentialHeightFogPS, FGlobalShader);

	class FSupportFogInScatteringTexture : SHADER_PERMUTATION_BOOL("PERMUTATION_SUPPORT_FOG_INSCATTERING_TEXTURE");
	class FSupportFogDirectionalLightInScattering : SHADER_PERMUTATION_BOOL("PERMUTATION_SUPPORT_FOG_DIRECTIONAL_LIGHT_INSCATTERING");
	class FSupportVolumetricFog : SHADER_PERMUTATION_BOOL("PERMUTATION_SUPPORT_VOLUMETRIC_FOG");
	using FPermutationDomain = TShaderPermutationDomain<FSupportFogInScatteringTexture, FSupportFogDirectionalLightInScattering, FSupportVolumetricFog>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FFogUniformParameters, FogUniformBuffer)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, OcclusionTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, OcclusionSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, LinearDepthTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearDepthSampler)
		SHADER_PARAMETER(float, bOnlyOnRenderedOpaque)
		SHADER_PARAMETER(float, bUseLinearDepthTexture)
		SHADER_PARAMETER(float, UpsampleJitterMultiplier)
		SHADER_PARAMETER(FVector4f, LinearDepthTextureMinMaxUV)
		SHADER_PARAMETER(FVector4f, OcclusionTextureMinMaxUV)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsFeatureLevelSupported(Parameters.Platform, ERHIFeatureLevel::SM5);
	}
};

IMPLEMENT_GLOBAL_SHADER(FExponentialHeightFogPS, "/Engine/Private/HeightFogPixelShader.usf", "ExponentialPixelMain", SF_Pixel);

/** The fog vertex declaration resource type. */
class FFogVertexDeclaration : public FRenderResource
{
public:
	FVertexDeclarationRHIRef VertexDeclarationRHI;

	// Destructor
	virtual ~FFogVertexDeclaration() {}

	virtual void InitRHI() override
	{
		FVertexDeclarationElementList Elements;
		Elements.Add(FVertexElement(0, 0, VET_Float2, 0, sizeof(FVector2f)));
		VertexDeclarationRHI = PipelineStateCache::GetOrCreateVertexDeclaration(Elements);
	}

	virtual void ReleaseRHI() override
	{
		VertexDeclarationRHI.SafeRelease();
	}
};

/** Vertex declaration for the light function fullscreen 2D quad. */
TGlobalResource<FFogVertexDeclaration> GFogVertexDeclaration;

void FSceneRenderer::InitFogConstants()
{
	// console command override
	float FogDensityOverride = -1.0f;
	float FogStartDistanceOverride = -1.0f;

#if !(UE_BUILD_SHIPPING || UE_BUILD_TEST)
	{
		// console variable overrides
		FogDensityOverride = CVarFogDensity.GetValueOnAnyThread();
		FogStartDistanceOverride = CVarFogStartDistance.GetValueOnAnyThread();
	}
#endif // !(UE_BUILD_SHIPPING || UE_BUILD_TEST)

	for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
	{
		FViewInfo& View = Views[ViewIndex];

		// set fog consts based on height fog components
		if(ShouldRenderFog(*View.Family))
		{
			if (Scene->ExponentialFogs.Num() > 0)
			{
				const FExponentialHeightFogSceneInfo& FogInfo = Scene->ExponentialFogs[0];
				float CollapsedFogParameter[FExponentialHeightFogSceneInfo::NumFogs];
				static constexpr float MaxObserverHeightDifference = 65536.0f;
				float MaxObserverHeight = FLT_MAX;
				for (int i = 0; i < FExponentialHeightFogSceneInfo::NumFogs; i++)
				{
					// Only limit the observer height to fog if it has any density
					if (FogInfo.FogData[i].Density > 0.0f)
					{
						MaxObserverHeight = FMath::Min(MaxObserverHeight, FogInfo.FogData[i].Height + MaxObserverHeightDifference);
					}
				}
				
				// Clamping the observer height to avoid numerical precision issues in the height fog equation. The max observer height is relative to the fog height.
				const float ObserverHeight = FMath::Min<float>(View.ViewMatrices.GetViewOrigin().Z, MaxObserverHeight);

				for (int i = 0; i < FExponentialHeightFogSceneInfo::NumFogs; i++)
				{
					const float CollapsedFogParameterPower = FMath::Clamp(
						-FogInfo.FogData[i].HeightFalloff * (ObserverHeight - FogInfo.FogData[i].Height),
						-126.f + 1.f, // min and max exponent values for IEEE floating points (http://en.wikipedia.org/wiki/IEEE_floating_point)
						+127.f - 1.f
					);

					CollapsedFogParameter[i] = FogInfo.FogData[i].Density * FMath::Pow(2.0f, CollapsedFogParameterPower);
				}

				View.ExponentialFogParameters = FVector4f(CollapsedFogParameter[0], FogInfo.FogData[0].HeightFalloff, MaxObserverHeight, FogInfo.StartDistance);
				View.ExponentialFogParameters2 = FVector4f(CollapsedFogParameter[1], FogInfo.FogData[1].HeightFalloff, FogInfo.FogData[1].Density, FogInfo.FogData[1].Height);
				View.ExponentialFogColor = FVector3f(FogInfo.FogColor.R, FogInfo.FogColor.G, FogInfo.FogColor.B);
				View.FogMaxOpacity = FogInfo.FogMaxOpacity;
				View.ExponentialFogParameters3 = FVector4f(FogInfo.FogData[0].Density, FogInfo.FogData[0].Height, FogInfo.InscatteringColorCubemap ? 1.0f : 0.0f, FogInfo.FogCutoffDistance);
				View.SinCosInscatteringColorCubemapRotation = FVector2f(FMath::Sin(FogInfo.InscatteringColorCubemapAngle), FMath::Cos(FogInfo.InscatteringColorCubemapAngle));
				View.FogInscatteringColorCubemap = FogInfo.InscatteringColorCubemap;
				const float InvRange = 1.0f / FMath::Max(FogInfo.FullyDirectionalInscatteringColorDistance - FogInfo.NonDirectionalInscatteringColorDistance, .00001f);
				float NumMips = 1.0f;

				if (FogInfo.InscatteringColorCubemap)
				{
					NumMips = FogInfo.InscatteringColorCubemap->GetNumMips();
				}

				View.FogInscatteringTextureParameters = FVector(InvRange, -FogInfo.NonDirectionalInscatteringColorDistance * InvRange, NumMips);

				View.DirectionalInscatteringExponent = FogInfo.DirectionalInscatteringExponent;
				View.DirectionalInscatteringStartDistance = FogInfo.DirectionalInscatteringStartDistance;
				View.InscatteringLightDirection = FVector(0);
				FLightSceneInfo* SunLight = Scene->AtmosphereLights[0] ? Scene->AtmosphereLights[0] : Scene->SimpleDirectionalLight;	// Fog only takes into account a single atmosphere light with index 0, or the default scene directional light.
				if (SunLight)
				{
					View.InscatteringLightDirection = -SunLight->Proxy->GetDirection();
					View.DirectionalInscatteringColor = FogInfo.DirectionalInscatteringColor * SunLight->Proxy->GetColor().GetLuminance();
				}
				View.bUseDirectionalInscattering = SunLight != nullptr;
			}
		}
	}
}

BEGIN_SHADER_PARAMETER_STRUCT(FFogPassParameters, )
	SHADER_PARAMETER_STRUCT_INCLUDE(FHeightFogVS::FParameters, VS)
	SHADER_PARAMETER_STRUCT_INCLUDE(FExponentialHeightFogPS::FParameters, PS)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

static FFogPassParameters* CreateDefaultFogPassParameters(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesUniformbuffer,
	TRDGUniformBufferRef<FFogUniformParameters>& FogUniformBuffer,
	FRDGTextureRef LightShaftOcclusionTexture,
	const FScreenPassTextureViewportParameters LightShaftParameters)
{
	extern int32 GVolumetricFogGridPixelSize;

	FFogPassParameters* PassParameters = GraphBuilder.AllocParameters<FFogPassParameters>();
	PassParameters->SceneTextures = SceneTexturesUniformbuffer;
	PassParameters->VS.ViewUniformBuffer = GetShaderBinding(View.ViewUniformBuffer);
	PassParameters->PS.ViewUniformBuffer = GetShaderBinding(View.ViewUniformBuffer);
	PassParameters->PS.FogUniformBuffer = FogUniformBuffer;
	PassParameters->PS.OcclusionTexture = LightShaftOcclusionTexture != nullptr ? LightShaftOcclusionTexture : GSystemTextures.GetWhiteDummy(GraphBuilder);
	PassParameters->PS.OcclusionSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->PS.LinearDepthTexture = GSystemTextures.GetDepthDummy(GraphBuilder);
	PassParameters->PS.LinearDepthSampler = TStaticSamplerState<SF_Point, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->PS.OcclusionTextureMinMaxUV = FVector4f(LightShaftParameters.UVViewportBilinearMin, LightShaftParameters.UVViewportBilinearMax);
	PassParameters->PS.LinearDepthTextureMinMaxUV = FVector4f::Zero();
	PassParameters->PS.UpsampleJitterMultiplier = CVarUpsampleJitterMultiplier.GetValueOnRenderThread() * GVolumetricFogGridPixelSize;
	PassParameters->PS.bOnlyOnRenderedOpaque = View.bFogOnlyOnRenderedOpaque;
	PassParameters->PS.bUseLinearDepthTexture = 0.0f;
	return PassParameters;
}

static void RenderViewFog(
	FRHICommandList& RHICmdList, 
	const FViewInfo& View, 
	FIntRect ViewRect, 
	FFogPassParameters* PassParameters, 
	bool bShouldRenderVolumetricFog)
{
	FGraphicsPipelineStateInitializer GraphicsPSOInit;
	RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
	RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);

	GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
	GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
	GraphicsPSOInit.PrimitiveType = PT_TriangleList;

	// disable alpha writes in order to preserve scene depth values on PC
	GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGB, BO_Add, BF_One, BF_SourceAlpha>::GetRHI();

	TShaderMapRef<FHeightFogVS> VertexShader(View.ShaderMap);
	GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFogVertexDeclaration.VertexDeclarationRHI;
	GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();

	const bool bUseFogInscatteringColorCubemap = View.FogInscatteringColorCubemap != nullptr;
	FExponentialHeightFogPS::FPermutationDomain PsPermutationVector;
	PsPermutationVector.Set<FExponentialHeightFogPS::FSupportFogInScatteringTexture>(bUseFogInscatteringColorCubemap);
	PsPermutationVector.Set<FExponentialHeightFogPS::FSupportFogDirectionalLightInScattering>(!bUseFogInscatteringColorCubemap && View.bUseDirectionalInscattering);
	PsPermutationVector.Set<FExponentialHeightFogPS::FSupportVolumetricFog>(bShouldRenderVolumetricFog);
	TShaderMapRef<FExponentialHeightFogPS> PixelShader(View.ShaderMap, PsPermutationVector);
	GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();

	// Setup the depth bound optimization if possible on that platform.
	GraphicsPSOInit.bDepthBounds = GSupportsDepthBoundsTest && CVarFogUseDepthBounds.GetValueOnAnyThread() && !bShouldRenderVolumetricFog;
	if (GraphicsPSOInit.bDepthBounds)
	{
		// The fog can be set to start at a certain euclidean distance.
		// clamp the value to be behind the near plane z
		float FogStartDistance = FMath::Max(30.0f, View.ExponentialFogParameters.W);

		// Here we compute the nearest z value the fog can start
		// to skip shader execution on pixels that are closer.
		// This means with a bigger distance specified more pixels are
		// are culled and don't need to be rendered. This is faster if
		// there is opaque content nearer than the computed z.
		// This optimization is achieved using depth bound tests.
		// Mobile platforms typically does not support that feature 
		// but typically renders the world using forward shading 
		// with height fog evaluated as part of the material vertex or pixel shader.
		FMatrix InvProjectionMatrix = View.ViewMatrices.GetInvProjectionMatrix();
		FVector ViewSpaceCorner = InvProjectionMatrix.TransformFVector4(FVector4(1, 1, 1, 1));
		float Ratio = ViewSpaceCorner.Z / ViewSpaceCorner.Size();
		FVector ViewSpaceStartFogPoint(0.0f, 0.0f, FogStartDistance * Ratio);
		FVector4f ClipSpaceMaxDistance = (FVector4f)View.ViewMatrices.GetProjectionMatrix().TransformPosition(ViewSpaceStartFogPoint); // LWC_TODO: precision loss
		float FogClipSpaceZ = ClipSpaceMaxDistance.Z / ClipSpaceMaxDistance.W;

		if (bool(ERHIZBuffer::IsInverted))
		{
			RHICmdList.SetDepthBounds(0.0f, FogClipSpaceZ);
		}
		else
		{
			RHICmdList.SetDepthBounds(FogClipSpaceZ, 1.0f);
		}
	}

	SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

	ClearUnusedGraphResources(VertexShader, &PassParameters->VS);
	ClearUnusedGraphResources(PixelShader, &PassParameters->PS);

	SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), PassParameters->VS);
	SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), PassParameters->PS);

	// Draw a quad covering the view.
	RHICmdList.SetStreamSource(0, GScreenSpaceVertexBuffer.VertexBufferRHI, 0);
	RHICmdList.DrawIndexedPrimitive(GTwoTrianglesIndexBuffer.IndexBufferRHI, 0, 0, 4, 0, 2, 1);
}

void FDeferredShadingSceneRenderer::RenderFog(
	FRDGBuilder& GraphBuilder,
	const FMinimalSceneTextures& SceneTextures,
	FRDGTextureRef LightShaftOcclusionTexture)
{
	if (Scene->ExponentialFogs.Num() > 0 
		// Fog must be done in the base pass for MSAA to work
		&& !IsForwardShadingEnabled(ShaderPlatform))
	{
		RDG_EVENT_SCOPE(GraphBuilder, "ExponentialHeightFog");
		RDG_GPU_STAT_SCOPE(GraphBuilder, Fog);

		const bool bShouldRenderVolumetricFog = ShouldRenderVolumetricFog();

		for(int32 ViewIndex = 0;ViewIndex < Views.Num();ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];
			if (View.IsPerspectiveProjection())
			{
				RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);
				RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

				TRDGUniformBufferRef<FFogUniformParameters> FogUniformBuffer = CreateFogUniformBuffer(GraphBuilder, View);

				const FScreenPassTextureViewport SceneViewport(SceneTextures.Config.Extent, View.ViewRect);
				const FScreenPassTextureViewport OutputViewport(GetDownscaledViewport(SceneViewport, GetLightShaftDownsampleFactor()));
				const FScreenPassTextureViewportParameters LightShaftParameters = GetScreenPassTextureViewportParameters(OutputViewport);

				FFogPassParameters* PassParameters = CreateDefaultFogPassParameters(GraphBuilder, View, SceneTextures.UniformBuffer, FogUniformBuffer, LightShaftOcclusionTexture, LightShaftParameters);
				PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneTextures.Color.Target, ERenderTargetLoadAction::ELoad);
				PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SceneTextures.Depth.Target, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthRead_StencilWrite);

				GraphBuilder.AddPass(RDG_EVENT_NAME("Fog"), PassParameters, ERDGPassFlags::Raster, 
					[this, &View, PassParameters, bShouldRenderVolumetricFog](FRHICommandList& RHICmdList)
				{
					RenderViewFog(RHICmdList, View, View.ViewRect, PassParameters, bShouldRenderVolumetricFog);
				});
			}
		}
	}
}

void FDeferredShadingSceneRenderer::RenderUnderWaterFog(
	FRDGBuilder& GraphBuilder,
	const FSceneWithoutWaterTextures& SceneWithoutWaterTextures,
	TRDGUniformBufferRef<FSceneTextureUniformParameters> SceneTexturesWithDepth)
{
	if (Scene->ExponentialFogs.Num() > 0
		// Fog must be done in the base pass for MSAA to work
		&& !IsForwardShadingEnabled(ShaderPlatform))
	{
		RDG_EVENT_SCOPE(GraphBuilder, "ExponentialHeightFog");
		RDG_GPU_STAT_SCOPE(GraphBuilder, Fog);

		FRDGTextureRef LinearDepthTexture = SceneWithoutWaterTextures.DepthTexture;
		check(LinearDepthTexture);

		const bool bShouldRenderVolumetricFog = ShouldRenderVolumetricFog();

		// This must match SINGLE_LAYER_WATER_DEPTH_SCALE from SingleLayerWaterCommon.ush and SingleLayerWaterComposite.usf.
		const float kSingleLayerWaterDepthScale = 100.0f;

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];
			if (View.IsPerspectiveProjection())
			{
				RDG_EVENT_SCOPE_CONDITIONAL(GraphBuilder, Views.Num() > 1, "View%d", ViewIndex);
				RDG_GPU_MASK_SCOPE(GraphBuilder, View.GPUMask);

				const auto& SceneWithoutWaterView = SceneWithoutWaterTextures.Views[ViewIndex];

				TRDGUniformBufferRef<FFogUniformParameters> FogUniformBuffer = CreateFogUniformBuffer(GraphBuilder, View);

				// TODO add support for occlusion texture on water
				FRDGTextureRef LightShaftOcclusionTexture = nullptr;	
				FScreenPassTextureViewportParameters LightShaftParameters = FScreenPassTextureViewportParameters();

				FFogPassParameters* PassParameters = CreateDefaultFogPassParameters(GraphBuilder, View, SceneTexturesWithDepth, FogUniformBuffer, LightShaftOcclusionTexture, LightShaftParameters);
				PassParameters->PS.LinearDepthTexture = LinearDepthTexture;
				PassParameters->PS.bUseLinearDepthTexture = kSingleLayerWaterDepthScale;
				PassParameters->PS.LinearDepthTextureMinMaxUV = SceneWithoutWaterView.MinMaxUV;
				PassParameters->RenderTargets[0] = FRenderTargetBinding(SceneWithoutWaterTextures.ColorTexture, ERenderTargetLoadAction::ELoad);

				GraphBuilder.AddPass(RDG_EVENT_NAME("FogBehindWater"), PassParameters, ERDGPassFlags::Raster, [this, &View, SceneWithoutWaterView, PassParameters, bShouldRenderVolumetricFog](FRHICommandList& RHICmdList)
				{
					RenderViewFog(RHICmdList, View, SceneWithoutWaterView.ViewRect, PassParameters, bShouldRenderVolumetricFog);
				});
			}
		}
	}
}

bool ShouldRenderFog(const FSceneViewFamily& Family)
{
	const FEngineShowFlags EngineShowFlags = Family.EngineShowFlags;

	return EngineShowFlags.Fog
		&& EngineShowFlags.Materials 
		&& !Family.UseDebugViewPS()
		&& CVarFog.GetValueOnRenderThread() == 1
		&& !EngineShowFlags.StationaryLightOverlap 
		&& !EngineShowFlags.LightMapDensity;
}
