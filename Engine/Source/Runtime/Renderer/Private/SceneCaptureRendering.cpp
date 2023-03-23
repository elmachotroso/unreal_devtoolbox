// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	
=============================================================================*/
#include "SceneCaptureRendering.h"
#include "Containers/ArrayView.h"
#include "Misc/MemStack.h"
#include "EngineDefines.h"
#include "RHIDefinitions.h"
#include "RHI.h"
#include "RenderingThread.h"
#include "Engine/Scene.h"
#include "SceneInterface.h"
#include "LegacyScreenPercentageDriver.h"
#include "GameFramework/Actor.h"
#include "GameFramework/WorldSettings.h"
#include "RHIStaticStates.h"
#include "SceneView.h"
#include "Shader.h"
#include "TextureResource.h"
#include "SceneUtils.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SceneCaptureComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SceneCaptureComponentCube.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/TextureRenderTargetCube.h"
#include "PostProcess/SceneRenderTargets.h"
#include "GlobalShader.h"
#include "SceneRenderTargetParameters.h"
#include "SceneRendering.h"
#include "DeferredShadingRenderer.h"
#include "ScenePrivate.h"
#include "PostProcess/SceneFilterRendering.h"
#include "ScreenRendering.h"
#include "PipelineStateCache.h"
#include "RendererModule.h"
#include "Rendering/MotionVectorSimulation.h"
#include "SceneViewExtension.h"
#include "GenerateMips.h"

/** A pixel shader for capturing a component of the rendered scene for a scene capture.*/
class FSceneCapturePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FSceneCapturePS);
	SHADER_USE_PARAMETER_STRUCT(FSceneCapturePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureShaderParameters, SceneTextures)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	enum class ESourceMode : uint32
	{
		ColorAndOpacity,
		ColorNoAlpha,
		ColorAndSceneDepth,
		SceneDepth,
		DeviceDepth,
		Normal,
		BaseColor,
		MAX
	};

	class FSourceModeDimension : SHADER_PERMUTATION_ENUM_CLASS("SOURCE_MODE", ESourceMode);
	using FPermutationDomain = TShaderPermutationDomain<FSourceModeDimension>;

	static FPermutationDomain GetPermutationVector(ESceneCaptureSource CaptureSource, bool bIsMobilePlatform)
	{
		ESourceMode SourceMode = ESourceMode::MAX;
		switch (CaptureSource)
		{
		case SCS_SceneColorHDR:
			SourceMode = ESourceMode::ColorAndOpacity;
			break;
		case SCS_SceneColorHDRNoAlpha:
			SourceMode = ESourceMode::ColorNoAlpha;
			break;
		case SCS_SceneColorSceneDepth:
			SourceMode = ESourceMode::ColorAndSceneDepth;
			break;
		case SCS_SceneDepth:
			SourceMode = ESourceMode::SceneDepth;
			break;
		case SCS_DeviceDepth:
			SourceMode = ESourceMode::DeviceDepth;
			break;
		case SCS_Normal:
			SourceMode = ESourceMode::Normal;
			break;
		case SCS_BaseColor:
			SourceMode = ESourceMode::BaseColor;
			break;
		default:
			checkf(false, TEXT("SceneCaptureSource not implemented."));
		}

		if (bIsMobilePlatform && (SourceMode == ESourceMode::Normal || SourceMode == ESourceMode::BaseColor))
		{
			SourceMode = ESourceMode::ColorAndOpacity;
		}
		FPermutationDomain PermutationVector;
		PermutationVector.Set<FSourceModeDimension>(SourceMode);
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) 
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		auto SourceModeDim = PermutationVector.Get<FSourceModeDimension>();
		return !IsMobilePlatform(Parameters.Platform) || (SourceModeDim != ESourceMode::Normal && SourceModeDim != ESourceMode::BaseColor);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		static const TCHAR* ShaderSourceModeDefineName[] =
		{
			TEXT("SOURCE_MODE_SCENE_COLOR_AND_OPACITY"),
			TEXT("SOURCE_MODE_SCENE_COLOR_NO_ALPHA"),
			TEXT("SOURCE_MODE_SCENE_COLOR_SCENE_DEPTH"),
			TEXT("SOURCE_MODE_SCENE_DEPTH"),
			TEXT("SOURCE_MODE_DEVICE_DEPTH"),
			TEXT("SOURCE_MODE_NORMAL"),
			TEXT("SOURCE_MODE_BASE_COLOR")
		};
		static_assert(UE_ARRAY_COUNT(ShaderSourceModeDefineName) == (uint32)ESourceMode::MAX, "ESourceMode doesn't match define table.");

		const FPermutationDomain PermutationVector(Parameters.PermutationId);
		const uint32 SourceModeIndex = static_cast<uint32>(PermutationVector.Get<FSourceModeDimension>());
		OutEnvironment.SetDefine(ShaderSourceModeDefineName[SourceModeIndex], 1u);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSceneCapturePS, "/Engine/Private/SceneCapturePixelShader.usf", "Main", SF_Pixel);

class FODSCapturePS : public FGlobalShader
{
public:
	DECLARE_GLOBAL_SHADER(FODSCapturePS);
	SHADER_USE_PARAMETER_STRUCT(FODSCapturePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(TextureCube, LeftEyeTexture)
		SHADER_PARAMETER_RDG_TEXTURE(TextureCube, RightEyeTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LeftEyeTextureSampler)
		SHADER_PARAMETER_SAMPLER(SamplerState, RightEyeTextureSampler)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()
};

IMPLEMENT_GLOBAL_SHADER(FODSCapturePS, "/Engine/Private/ODSCapture.usf", "MainPS", SF_Pixel);

static bool CaptureNeedsSceneColor(ESceneCaptureSource CaptureSource)
{
	return CaptureSource != SCS_FinalColorLDR && CaptureSource != SCS_FinalColorHDR && CaptureSource != SCS_FinalToneCurveHDR;
}

static TFunction<void(FRHICommandList& RHICmdList)> CopyCaptureToTargetSetViewportFn = [](FRHICommandList& RHICmdList) {};

void CopySceneCaptureComponentToTarget(
	FRDGBuilder& GraphBuilder,
	const FMinimalSceneTextures& SceneTextures,
	FRDGTextureRef ViewFamilyTexture,
	const FSceneViewFamily& ViewFamily,
	const TArray<FViewInfo>& Views,
	bool bNeedsFlippedRenderTarget)
{
	ESceneCaptureSource SceneCaptureSource = ViewFamily.SceneCaptureSource;

	if (IsAnyForwardShadingEnabled(ViewFamily.GetShaderPlatform()) && (SceneCaptureSource == SCS_Normal || SceneCaptureSource == SCS_BaseColor))
	{
		SceneCaptureSource = SCS_SceneColorHDR;
	}

	if (CaptureNeedsSceneColor(SceneCaptureSource))
	{
		RDG_EVENT_SCOPE(GraphBuilder, "CaptureSceneComponent[%d]", SceneCaptureSource);

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<FM_Solid, CM_None>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

		if (SceneCaptureSource == SCS_SceneColorHDR && ViewFamily.SceneCaptureCompositeMode == SCCM_Composite)
		{
			// Blend with existing render target color. Scene capture color is already pre-multiplied by alpha.
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_SourceAlpha, BO_Add, BF_Zero, BF_SourceAlpha>::GetRHI();
		}
		else if (SceneCaptureSource == SCS_SceneColorHDR && ViewFamily.SceneCaptureCompositeMode == SCCM_Additive)
		{
			// Add to existing render target color. Scene capture color is already pre-multiplied by alpha.
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_Zero, BF_SourceAlpha>::GetRHI();
		}
		else
		{
			GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
		}

		const FSceneCapturePS::FPermutationDomain PixelPermutationVector = FSceneCapturePS::GetPermutationVector(SceneCaptureSource, IsMobilePlatform(ViewFamily.GetShaderPlatform()));

		for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ViewIndex++)
		{
			const FViewInfo& View = Views[ViewIndex];

			FSceneCapturePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSceneCapturePS::FParameters>();
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->SceneTextures = SceneTextures.GetSceneTextureShaderParameters(ViewFamily.GetFeatureLevel());
			PassParameters->RenderTargets[0] = FRenderTargetBinding(ViewFamilyTexture, ERenderTargetLoadAction::ENoAction);

			TShaderMapRef<FScreenVS> VertexShader(View.ShaderMap);
			TShaderMapRef<FSceneCapturePS> PixelShader(View.ShaderMap, PixelPermutationVector);

			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;

			GraphBuilder.AddPass(
				RDG_EVENT_NAME("View(%d)", ViewIndex),
				PassParameters,
				ERDGPassFlags::Raster,
				[PassParameters, GraphicsPSOInit, VertexShader, PixelShader, &View, bNeedsFlippedRenderTarget] (FRHICommandList& RHICmdList)
			{
				FGraphicsPipelineStateInitializer LocalGraphicsPSOInit = GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(LocalGraphicsPSOInit);
				SetGraphicsPipelineState(RHICmdList, LocalGraphicsPSOInit, 0);
				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *PassParameters);
				
				CopyCaptureToTargetSetViewportFn(RHICmdList);

				if (bNeedsFlippedRenderTarget)
				{
					CopyCaptureToTargetSetViewportFn(RHICmdList);
					DrawRectangle(
						RHICmdList,
						View.ViewRect.Min.X, View.ViewRect.Min.Y,
						View.ViewRect.Width(), View.ViewRect.Height(),
						View.ViewRect.Min.X, View.ViewRect.Height() - View.ViewRect.Min.Y,
						View.ViewRect.Width(), -View.ViewRect.Height(),
						View.UnconstrainedViewRect.Size(),
						GetSceneTextureExtent(),
						VertexShader,
						EDRF_UseTriangleOptimization);
				}
				else
				{
					CopyCaptureToTargetSetViewportFn(RHICmdList);
					DrawRectangle(
						RHICmdList,
						View.ViewRect.Min.X, View.ViewRect.Min.Y,
						View.ViewRect.Width(), View.ViewRect.Height(),
						View.ViewRect.Min.X, View.ViewRect.Min.Y,
						View.ViewRect.Width(), View.ViewRect.Height(),
						View.UnconstrainedViewRect.Size(),
						GetSceneTextureExtent(),
						VertexShader,
						EDRF_UseTriangleOptimization);
				}
			});
		}
	}
}

static void UpdateSceneCaptureContentDeferred_RenderThread(
	FRHICommandListImmediate& RHICmdList, 
	FSceneRenderer* SceneRenderer, 
	FRenderTarget* RenderTarget, 
	FTexture* RenderTargetTexture, 
	const FString& EventName, 
	const FResolveParams& ResolveParams,
	bool bGenerateMips,
	const FGenerateMipsParams& GenerateMipsParams,
	bool bClearRenderTarget,
	bool bOrthographicCamera
	)
{
	// We need to execute the pre-render view extensions before we do any view dependent work.
	FSceneRenderer::ViewExtensionPreRender_RenderThread(RHICmdList, SceneRenderer);

	SceneRenderer->RenderThreadBegin(RHICmdList);

	// update any resources that needed a deferred update
	FDeferredUpdateResource::UpdateResources(RHICmdList);
	{
		const ERHIFeatureLevel::Type FeatureLevel = SceneRenderer->FeatureLevel;

#if WANTS_DRAW_MESH_EVENTS
		SCOPED_DRAW_EVENTF(RHICmdList, SceneCapture, TEXT("SceneCapture %s"), *EventName);
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("SceneCapture %s", *EventName), FSceneRenderer::GetRDGParalelExecuteFlags(FeatureLevel));
#else
		SCOPED_DRAW_EVENT(RHICmdList, UpdateSceneCaptureContent_RenderThread);
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("SceneCapture"), FSceneRenderer::GetRDGParalelExecuteFlags(FeatureLevel));
#endif

		FRDGTextureRef TargetTexture = RegisterExternalTexture(GraphBuilder, RenderTarget->GetRenderTargetTexture(), TEXT("SceneCaptureTarget"));
		if (bClearRenderTarget)
		{
			AddClearRenderTargetPass(GraphBuilder, TargetTexture, FLinearColor::Black, SceneRenderer->Views[0].UnscaledViewRect);
		}

		if (ResolveParams.DestRect.IsValid())
		{
			CopyCaptureToTargetSetViewportFn = [ResolveParams](FRHICommandList& RHICmdList)
			{
				RHICmdList.SetScissorRect(false, 0, 0, 0, 0);
				RHICmdList.SetViewport
				(
					float(ResolveParams.DestRect.X1), 
					float(ResolveParams.DestRect.Y1), 
					0.0f, 
					float(ResolveParams.DestRect.X2), 
					float(ResolveParams.DestRect.Y2), 
					1.0f
				);
			};
		}
		else
		{
			CopyCaptureToTargetSetViewportFn = [](FRHICommandList& RHICmdList) {};
		}


		// Disable occlusion queries when in orthographic mode
		if (bOrthographicCamera)
		{
			FViewInfo& View = SceneRenderer->Views[0];
			View.bDisableQuerySubmissions = true;
			View.bIgnoreExistingQueries = true;
		}

		// Render the scene normally
		{
			RDG_RHI_EVENT_SCOPE(GraphBuilder, RenderScene);
			SceneRenderer->Render(GraphBuilder);
		}

			if (bGenerateMips)
			{
			FGenerateMips::Execute(GraphBuilder, TargetTexture, GenerateMipsParams);
			}

		FRDGTextureRef ResolveTexture = RegisterExternalTexture(GraphBuilder, RenderTargetTexture->TextureRHI, TEXT("SceneCaptureResolve"));
		AddCopyToResolveTargetPass(GraphBuilder, TargetTexture, ResolveTexture, ResolveParams);

		GraphBuilder.Execute();
	}

	SceneRenderer->RenderThreadEnd(RHICmdList);
}

void UpdateSceneCaptureContentMobile_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FSceneRenderer* SceneRenderer,
	FRenderTarget* RenderTarget,
	FTexture* RenderTargetTexture,
	const FString& EventName,
	const FResolveParams& ResolveParams,
	bool bGenerateMips,
	const FGenerateMipsParams& GenerateMipsParams,
	bool bDisableFlipCopyGLES)
{
	// We need to execute the pre-render view extensions before we do any view dependent work.
	FSceneRenderer::ViewExtensionPreRender_RenderThread(RHICmdList, SceneRenderer);

	SceneRenderer->RenderThreadBegin(RHICmdList);

	// update any resources that needed a deferred update
	FDeferredUpdateResource::UpdateResources(RHICmdList);
	bool bUseSceneTextures = SceneRenderer->ViewFamily.SceneCaptureSource != SCS_FinalColorLDR &&
		SceneRenderer->ViewFamily.SceneCaptureSource != SCS_FinalColorHDR;

	{
#if WANTS_DRAW_MESH_EVENTS
		SCOPED_DRAW_EVENTF(RHICmdList, SceneCaptureMobile, TEXT("SceneCaptureMobile %s"), *EventName);
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("SceneCaptureMobile %s", *EventName));
#else
		SCOPED_DRAW_EVENT(RHICmdList, UpdateSceneCaptureContentMobile_RenderThread);
		FRDGBuilder GraphBuilder(RHICmdList, RDG_EVENT_NAME("SceneCaptureMobile"));
#endif

		FViewInfo& View = SceneRenderer->Views[0];

		const bool bIsMobileHDR = IsMobileHDR();
		const bool bRHINeedsFlip = RHINeedsToSwitchVerticalAxis(GMaxRHIShaderPlatform) && !bDisableFlipCopyGLES;
		// note that GLES code will flip the image when:
		//	bIsMobileHDR && SceneCaptureSource == SCS_FinalColorLDR (flip performed during post processing)
		//	!bIsMobileHDR (rendering is flipped by vertex shader)
		// they need flipping again so it is correct for texture addressing.
		const bool bNeedsFlippedCopy = (!bIsMobileHDR || !bUseSceneTextures) && bRHINeedsFlip;
		const bool bNeedsFlippedFinalColor = bNeedsFlippedCopy && !bUseSceneTextures;

		// Intermediate render target that will need to be flipped (needed on !IsMobileHDR())
		FRDGTextureRef FlippedOutputTexture{};

		const FRenderTarget* Target = SceneRenderer->ViewFamily.RenderTarget;
		if (bNeedsFlippedFinalColor)
		{
			// We need to use an intermediate render target since the result will be flipped
			auto& RenderTargetRHI = Target->GetRenderTargetTexture();
			FRDGTextureDesc Desc(FRDGTextureDesc::Create2D(
				Target->GetSizeXY(),
				RenderTargetRHI.GetReference()->GetFormat(),
				RenderTargetRHI.GetReference()->GetClearBinding(),
				TexCreate_RenderTargetable));
			FlippedOutputTexture = GraphBuilder.CreateTexture(Desc, TEXT("SceneCaptureFlipped"));
		}

		// We don't support screen percentage in scene capture.
		FIntRect ViewRect = View.UnscaledViewRect;
		FIntRect UnconstrainedViewRect = View.UnconstrainedViewRect;

		if (bNeedsFlippedFinalColor)
		{
			AddClearRenderTargetPass(GraphBuilder, FlippedOutputTexture, FLinearColor::Black, ViewRect);
		}

		if (ResolveParams.DestRect.IsValid())
		{
			CopyCaptureToTargetSetViewportFn = [ResolveParams, bNeedsFlippedFinalColor, ViewRect, FlippedOutputTexture](FRHICommandList& RHICmdList)
			{
				RHICmdList.SetScissorRect(false, 0, 0, 0, 0);

				if (bNeedsFlippedFinalColor)
				{
					FResolveRect DestRect = ResolveParams.DestRect;
					int32 TileYID = DestRect.Y1 / ViewRect.Height();
					int32 TileYCount = (FlippedOutputTexture->Desc.GetSize().Y / ViewRect.Height()) - 1;
					DestRect.Y1 = (TileYCount - TileYID) * ViewRect.Height();
					DestRect.Y2 = DestRect.Y1 + ViewRect.Height();
					RHICmdList.SetViewport
					(
						float(DestRect.X1),
						float(DestRect.Y1),
						0.0f,
						float(DestRect.X2),
						float(DestRect.Y2),
						1.0f
					);
				}
				else
				{
					RHICmdList.SetViewport
					(
						float(ResolveParams.DestRect.X1),
						float(ResolveParams.DestRect.Y1),
						0.0f,
						float(ResolveParams.DestRect.X2),
						float(ResolveParams.DestRect.Y2),
						1.0f
					);
				}
			};
		}
		else
		{
			CopyCaptureToTargetSetViewportFn = [](FRHICommandList& RHICmdList) {};
		}

		// Render the scene normally
		{
			RDG_RHI_EVENT_SCOPE(GraphBuilder, RenderScene);

			if (bNeedsFlippedFinalColor)
			{
				// Helper class to allow setting render target
				struct FRenderTargetOverride : public FRenderTarget
				{
					FRenderTargetOverride(const FRenderTarget* TargetIn, FRHITexture2D* OverrideTexture)
					{
						RenderTargetTextureRHI = OverrideTexture;
						OriginalTarget = TargetIn;
					}

					virtual FIntPoint GetSizeXY() const override { return FIntPoint(RenderTargetTextureRHI->GetSizeX(), RenderTargetTextureRHI->GetSizeY()); }
					virtual float GetDisplayGamma() const override { return OriginalTarget->GetDisplayGamma(); }

					FTexture2DRHIRef GetTextureParamRef() { return RenderTargetTextureRHI; }
					const FRenderTarget* OriginalTarget;
				};

				// Hijack the render target
				FRHITexture2D* FlippedOutputTextureRHI = GraphBuilder.ConvertToExternalTexture(FlippedOutputTexture)->GetRHI()->GetTexture2D();
				SceneRenderer->ViewFamily.RenderTarget = GraphBuilder.AllocObject<FRenderTargetOverride>(Target, FlippedOutputTextureRHI); //-V506
			}

			SceneRenderer->Render(GraphBuilder);

			if (bNeedsFlippedFinalColor)
			{
				// And restore it
				SceneRenderer->ViewFamily.RenderTarget = Target;
			}
		}

		FRDGTextureRef OutputTexture = RegisterExternalTexture(GraphBuilder, Target->GetRenderTargetTexture(), TEXT("OutputTexture"));
		const FMinimalSceneTextures& SceneTextures = FSceneTextures::Get(GraphBuilder);

		const FIntPoint TargetSize(UnconstrainedViewRect.Width(), UnconstrainedViewRect.Height());
		{
			// We need to flip this texture upside down (since we depended on tonemapping to fix this on the hdr path)
			RDG_EVENT_SCOPE(GraphBuilder, "CaptureSceneColor");
			CopySceneCaptureComponentToTarget(
				GraphBuilder,
				SceneTextures,
				OutputTexture,
				SceneRenderer->ViewFamily,
				SceneRenderer->Views,
				bNeedsFlippedFinalColor);
		}

		if (bGenerateMips)
		{
			FGenerateMips::Execute(GraphBuilder, OutputTexture, GenerateMipsParams);
		}

		GraphBuilder.Execute();
	}

	SceneRenderer->RenderThreadEnd(RHICmdList);
}

static void ODSCapture_RenderThread(
	FRDGBuilder& GraphBuilder,
	FRDGTextureRef LeftEyeTexture,
	FRDGTextureRef RightEyeTexture,
	FRDGTextureRef OutputTexture,
	const ERHIFeatureLevel::Type FeatureLevel)
{
	FODSCapturePS::FParameters* PassParameters = GraphBuilder.AllocParameters<FODSCapturePS::FParameters>();
	PassParameters->LeftEyeTexture = LeftEyeTexture;
	PassParameters->RightEyeTexture = RightEyeTexture;
	PassParameters->LeftEyeTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->RightEyeTextureSampler = TStaticSamplerState<SF_Bilinear>::GetRHI();
	PassParameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ELoad);

	const auto ShaderMap = GetGlobalShaderMap(FeatureLevel);
	TShaderMapRef<FScreenVS> VertexShader(ShaderMap);
	TShaderMapRef<FODSCapturePS> PixelShader(ShaderMap);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("ODSCapture"),
		PassParameters,
		ERDGPassFlags::Raster,
		[VertexShader, PixelShader, OutputTexture](FRHICommandList& RHICmdList)
	{
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

		extern TGlobalResource<FFilterVertexDeclaration> GFilterVertexDeclaration;

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;

		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

		const FIntPoint& TargetSize = OutputTexture->Desc.Extent;
		RHICmdList.SetViewport(0, 0, 0.0f, TargetSize.X, TargetSize.Y, 1.0f);

		DrawRectangle(
			RHICmdList,
			0, 0,
			static_cast<float>(TargetSize.X), static_cast<float>(TargetSize.Y),
			0, 0,
			TargetSize.X, TargetSize.Y,
			TargetSize,
			TargetSize,
			VertexShader,
			EDRF_UseTriangleOptimization);
	});
}

static void UpdateSceneCaptureContent_RenderThread(
	FRHICommandListImmediate& RHICmdList,
	FSceneRenderer* SceneRenderer,
	FRenderTarget* RenderTarget,
	FTexture* RenderTargetTexture,
	const FString& EventName,
	const FResolveParams& ResolveParams,
	bool bGenerateMips,
	const FGenerateMipsParams& GenerateMipsParams,
	const bool bDisableFlipCopyLDRGLES,
	bool bClearRenderTarget,
	bool bOrthographicCamera)
{
	FMaterialRenderProxy::UpdateDeferredCachedUniformExpressions();

	switch (SceneRenderer->Scene->GetShadingPath())
	{
		case EShadingPath::Mobile:
		{
			UpdateSceneCaptureContentMobile_RenderThread(
				RHICmdList,
				SceneRenderer,
				RenderTarget,
				RenderTargetTexture,
				EventName,
				ResolveParams,
				bGenerateMips,
				GenerateMipsParams,
				bDisableFlipCopyLDRGLES);
			break;
		}
		case EShadingPath::Deferred:
		{
			UpdateSceneCaptureContentDeferred_RenderThread(
				RHICmdList,
				SceneRenderer,
				RenderTarget,
				RenderTargetTexture,
				EventName,
				ResolveParams,
				bGenerateMips,
				GenerateMipsParams,
				bClearRenderTarget,
				bOrthographicCamera);
			break;
		}
		default:
			checkNoEntry();
			break;
	}

	RHICmdList.Transition(FRHITransitionInfo(RenderTargetTexture->TextureRHI, ERHIAccess::Unknown, ERHIAccess::SRVMask));
}

static void BuildOrthoMatrix(FIntPoint InRenderTargetSize, float InOrthoWidth, int32 InTileID, int32 InNumXTiles, int32 InNumYTiles, FMatrix& OutProjectionMatrix)
{
	check((int32)ERHIZBuffer::IsInverted);
	float const XAxisMultiplier = 1.0f;
	float const YAxisMultiplier = InRenderTargetSize.X / float(InRenderTargetSize.Y);

	const float OrthoWidth = InOrthoWidth / 2.0f;
	const float OrthoHeight = InOrthoWidth / 2.0f * XAxisMultiplier / YAxisMultiplier;

	const float NearPlane = 0;
	const float FarPlane = WORLD_MAX / 8.0f;

	const float ZScale = 1.0f / (FarPlane - NearPlane);
	const float ZOffset = -NearPlane;

	if (InTileID == -1)
	{
		OutProjectionMatrix = FReversedZOrthoMatrix(
			OrthoWidth,
			OrthoHeight,
			ZScale,
			ZOffset
		);
		
		return;
	}

#if DO_CHECK
	check(InNumXTiles != 0 && InNumYTiles != 0);
	if (InNumXTiles == 0 || InNumYTiles == 0)
	{
		OutProjectionMatrix = FMatrix(EForceInit::ForceInitToZero);
		return;
	}
#endif

	const float XTileDividerRcp = 1.0f / float(InNumXTiles);
	const float YTileDividerRcp = 1.0f / float(InNumYTiles);

	const float TileX = float(InTileID % InNumXTiles);
	const float TileY = float(InTileID / InNumXTiles);

	float l = -OrthoWidth + TileX * InOrthoWidth * XTileDividerRcp;
	float r = l + InOrthoWidth * XTileDividerRcp;
	float t = OrthoHeight - TileY * InOrthoWidth * YTileDividerRcp;
	float b = t - InOrthoWidth * YTileDividerRcp;

	OutProjectionMatrix = FMatrix(
		FPlane(2.0f / (r-l), 0.0f, 0.0f, 0.0f),
		FPlane(0.0f, 2.0f / (t-b), 0.0f, 0.0f),
		FPlane(0.0f, 0.0f, -ZScale, 0.0f),
		FPlane(-((r+l)/(r-l)), -((t+b)/(t-b)), 1.0f - ZOffset * ZScale, 1.0f)
	);
}

void BuildProjectionMatrix(FIntPoint InRenderTargetSize, float InFOV, float InNearClippingPlane, FMatrix& OutProjectionMatrix)
{
	float const XAxisMultiplier = 1.0f;
	float const YAxisMultiplier = InRenderTargetSize.X / float(InRenderTargetSize.Y);

	if ((int32)ERHIZBuffer::IsInverted)
	{
		OutProjectionMatrix = FReversedZPerspectiveMatrix(
			InFOV,
			InFOV,
			XAxisMultiplier,
			YAxisMultiplier,
			InNearClippingPlane,
			InNearClippingPlane
			);
	}
	else
	{
		OutProjectionMatrix = FPerspectiveMatrix(
			InFOV,
			InFOV,
			XAxisMultiplier,
			YAxisMultiplier,
			InNearClippingPlane,
			InNearClippingPlane
			);
	}
}

void SetupViewFamilyForSceneCapture(
	FSceneViewFamily& ViewFamily,
	USceneCaptureComponent* SceneCaptureComponent,
	const TArrayView<const FSceneCaptureViewInfo> Views,
	float MaxViewDistance,
	bool bUseFauxOrthoViewPos,
	bool bCaptureSceneColor,
	bool bIsPlanarReflection,
	FPostProcessSettings* PostProcessSettings,
	float PostProcessBlendWeight,
	const AActor* ViewActor)
{
	check(!ViewFamily.GetScreenPercentageInterface());

	for (int32 ViewIndex = 0; ViewIndex < Views.Num(); ++ViewIndex)
	{
		const FSceneCaptureViewInfo& SceneCaptureViewInfo = Views[ViewIndex];

		FSceneViewInitOptions ViewInitOptions;
		ViewInitOptions.SetViewRectangle(SceneCaptureViewInfo.ViewRect);
		ViewInitOptions.ViewFamily = &ViewFamily;
		ViewInitOptions.ViewActor = ViewActor;
		ViewInitOptions.ViewOrigin = SceneCaptureViewInfo.ViewLocation;
		ViewInitOptions.ViewRotationMatrix = SceneCaptureViewInfo.ViewRotationMatrix;
		ViewInitOptions.BackgroundColor = FLinearColor::Black;
		ViewInitOptions.OverrideFarClippingPlaneDistance = MaxViewDistance;
		ViewInitOptions.StereoPass = SceneCaptureViewInfo.StereoPass;
		ViewInitOptions.SceneViewStateInterface = SceneCaptureComponent->GetViewState(ViewIndex);
		ViewInitOptions.ProjectionMatrix = SceneCaptureViewInfo.ProjectionMatrix;
		ViewInitOptions.LODDistanceFactor = FMath::Clamp(SceneCaptureComponent->LODDistanceFactor, .01f, 100.0f);
		ViewInitOptions.bUseFauxOrthoViewPos = bUseFauxOrthoViewPos;

		if (ViewFamily.Scene->GetWorld() != nullptr && ViewFamily.Scene->GetWorld()->GetWorldSettings() != nullptr)
		{
			ViewInitOptions.WorldToMetersScale = ViewFamily.Scene->GetWorld()->GetWorldSettings()->WorldToMeters;
		}
		ViewInitOptions.StereoIPD = SceneCaptureViewInfo.StereoIPD * (ViewInitOptions.WorldToMetersScale / 100.0f);

		if (bCaptureSceneColor)
		{
			ViewFamily.EngineShowFlags.PostProcessing = 0;
			ViewInitOptions.OverlayColor = FLinearColor::Black;
		}

		FSceneView* View = new FSceneView(ViewInitOptions);

		View->bIsSceneCapture = true;
		View->bSceneCaptureUsesRayTracing = SceneCaptureComponent->bUseRayTracingIfEnabled;
		// Note: this has to be set before EndFinalPostprocessSettings
		View->bIsPlanarReflection = bIsPlanarReflection;
        // Needs to be reconfigured now that bIsPlanarReflection has changed.
		View->SetupAntiAliasingMethod();

		check(SceneCaptureComponent);
		for (auto It = SceneCaptureComponent->HiddenComponents.CreateConstIterator(); It; ++It)
		{
			// If the primitive component was destroyed, the weak pointer will return NULL.
			UPrimitiveComponent* PrimitiveComponent = It->Get();
			if (PrimitiveComponent)
			{
				View->HiddenPrimitives.Add(PrimitiveComponent->ComponentId);
			}
		}

		for (auto It = SceneCaptureComponent->HiddenActors.CreateConstIterator(); It; ++It)
		{
			AActor* Actor = *It;

			if (Actor)
			{
				for (UActorComponent* Component : Actor->GetComponents())
				{
					if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Component))
					{
						View->HiddenPrimitives.Add(PrimComp->ComponentId);
					}
				}
			}
		}

		if (SceneCaptureComponent->PrimitiveRenderMode == ESceneCapturePrimitiveRenderMode::PRM_UseShowOnlyList)
		{
				View->ShowOnlyPrimitives.Emplace();

			for (auto It = SceneCaptureComponent->ShowOnlyComponents.CreateConstIterator(); It; ++It)
			{
				// If the primitive component was destroyed, the weak pointer will return NULL.
				UPrimitiveComponent* PrimitiveComponent = It->Get();
				if (PrimitiveComponent)
				{
					View->ShowOnlyPrimitives->Add(PrimitiveComponent->ComponentId);
				}
			}

			for (auto It = SceneCaptureComponent->ShowOnlyActors.CreateConstIterator(); It; ++It)
			{
				AActor* Actor = *It;

				if (Actor)
				{
					for (UActorComponent* Component : Actor->GetComponents())
					{
						if (UPrimitiveComponent* PrimComp = Cast<UPrimitiveComponent>(Component))
						{
							View->ShowOnlyPrimitives->Add(PrimComp->ComponentId);
						}
					}
				}
			}
		}
		else if (SceneCaptureComponent->ShowOnlyComponents.Num() > 0 || SceneCaptureComponent->ShowOnlyActors.Num() > 0)
		{
			static bool bWarned = false;

			if (!bWarned)
			{
				UE_LOG(LogRenderer, Log, TEXT("Scene Capture has ShowOnlyComponents or ShowOnlyActors ignored by the PrimitiveRenderMode setting! %s"), *SceneCaptureComponent->GetPathName());
				bWarned = true;
			}
		}

		ViewFamily.Views.Add(View);

		View->StartFinalPostprocessSettings(SceneCaptureViewInfo.ViewLocation);
		View->OverridePostProcessSettings(*PostProcessSettings, PostProcessBlendWeight);
		View->EndFinalPostprocessSettings(ViewInitOptions);
	}
}

static FSceneRenderer* CreateSceneRendererForSceneCapture(
	FScene* Scene,
	USceneCaptureComponent* SceneCaptureComponent,
	FRenderTarget* RenderTarget,
	FIntPoint RenderTargetSize,
	const FMatrix& ViewRotationMatrix,
	const FVector& ViewLocation,
	const FMatrix& ProjectionMatrix,
	bool bUseFauxOrthoViewPos,
	float MaxViewDistance,
	bool bCaptureSceneColor,
	FPostProcessSettings* PostProcessSettings,
	float PostProcessBlendWeight,
	const AActor* ViewActor, 
	const float StereoIPD = 0.0f)
{
	FSceneCaptureViewInfo SceneCaptureViewInfo;
	SceneCaptureViewInfo.ViewRotationMatrix = ViewRotationMatrix;
	SceneCaptureViewInfo.ViewLocation = ViewLocation;
	SceneCaptureViewInfo.ProjectionMatrix = ProjectionMatrix;
	SceneCaptureViewInfo.StereoPass = EStereoscopicPass::eSSP_FULL;
	SceneCaptureViewInfo.StereoViewIndex = INDEX_NONE;
	SceneCaptureViewInfo.StereoIPD = StereoIPD;
	SceneCaptureViewInfo.ViewRect = FIntRect(0, 0, RenderTargetSize.X, RenderTargetSize.Y);

	FSceneViewFamilyContext ViewFamily(FSceneViewFamily::ConstructionValues(
		RenderTarget,
		Scene,
		SceneCaptureComponent->ShowFlags)
		.SetResolveScene(!bCaptureSceneColor)
		.SetRealtimeUpdate(SceneCaptureComponent->bCaptureEveryFrame || SceneCaptureComponent->bAlwaysPersistRenderingState));

	FSceneViewExtensionContext ViewExtensionContext(Scene);
	ViewExtensionContext.bStereoDisabled = true;

	ViewFamily.ViewExtensions = GEngine->ViewExtensions->GatherActiveExtensions(ViewExtensionContext);
	
	SetupViewFamilyForSceneCapture(
		ViewFamily,
		SceneCaptureComponent,
		MakeArrayView(&SceneCaptureViewInfo, 1),
		MaxViewDistance, 
		bUseFauxOrthoViewPos,
		bCaptureSceneColor,
		/* bIsPlanarReflection = */ false,
		PostProcessSettings, 
		PostProcessBlendWeight,
		ViewActor);

	// Screen percentage is still not supported in scene capture.
	ViewFamily.EngineShowFlags.ScreenPercentage = false;
	ViewFamily.SetScreenPercentageInterface(new FLegacyScreenPercentageDriver(
		ViewFamily, /* GlobalResolutionFraction = */ 1.0f));

	return FSceneRenderer::CreateSceneRenderer(&ViewFamily, nullptr);
}

void FScene::UpdateSceneCaptureContents(USceneCaptureComponent2D* CaptureComponent)
{
	check(CaptureComponent);

	if (UTextureRenderTarget2D* TextureRenderTarget = CaptureComponent->TextureTarget)
	{
		FTransform Transform = CaptureComponent->GetComponentToWorld();
		FVector ViewLocation = Transform.GetTranslation();

		// Remove the translation from Transform because we only need rotation.
		Transform.SetTranslation(FVector::ZeroVector);
		Transform.SetScale3D(FVector::OneVector);
		FMatrix ViewRotationMatrix = Transform.ToInverseMatrixWithScale();

		// swap axis st. x=z,y=x,z=y (unreal coord space) so that z is up
		ViewRotationMatrix = ViewRotationMatrix * FMatrix(
			FPlane(0, 0, 1, 0),
			FPlane(1, 0, 0, 0),
			FPlane(0, 1, 0, 0),
			FPlane(0, 0, 0, 1));
		const float FOV = CaptureComponent->FOVAngle * (float)PI / 360.0f;
		FIntPoint CaptureSize(TextureRenderTarget->GetSurfaceWidth(), TextureRenderTarget->GetSurfaceHeight());

		const bool bUseSceneColorTexture = CaptureNeedsSceneColor(CaptureComponent->CaptureSource);
		const bool bEnableOrthographicTiling = (CaptureComponent->GetEnableOrthographicTiling() && CaptureComponent->ProjectionType == ECameraProjectionMode::Orthographic && bUseSceneColorTexture);
		bool bUseFauxOrthoViewPos = false;
		if (CaptureComponent->GetEnableOrthographicTiling() && CaptureComponent->ProjectionType == ECameraProjectionMode::Orthographic && !bUseSceneColorTexture)
		{
			UE_LOG(LogRenderer, Warning, TEXT("SceneCapture - Orthographic and tiling with CaptureSource not using SceneColor (i.e FinalColor) not compatible. SceneCapture render will not be tiled"));
		}
		
		const int32 TileID = CaptureComponent->TileID;
		const int32 NumXTiles = CaptureComponent->GetNumXTiles();
		const int32 NumYTiles = CaptureComponent->GetNumYTiles();

		FMatrix ProjectionMatrix;
		if (CaptureComponent->bUseCustomProjectionMatrix)
		{
			ProjectionMatrix = CaptureComponent->CustomProjectionMatrix;
		}
		else
		{
			if (CaptureComponent->ProjectionType == ECameraProjectionMode::Perspective)
			{
				const float ClippingPlane = (CaptureComponent->bOverride_CustomNearClippingPlane) ? CaptureComponent->CustomNearClippingPlane : GNearClippingPlane;
				BuildProjectionMatrix(CaptureSize, FOV, ClippingPlane, ProjectionMatrix);
			}
			else
			{
				bUseFauxOrthoViewPos = CaptureComponent->bUseFauxOrthoViewPos;
				if (bEnableOrthographicTiling)
				{
					BuildOrthoMatrix(CaptureSize, CaptureComponent->OrthoWidth, CaptureComponent->TileID, NumXTiles, NumYTiles, ProjectionMatrix);
					CaptureSize /= FIntPoint(NumXTiles, NumYTiles);
				}
				else
				{
					BuildOrthoMatrix(CaptureSize, CaptureComponent->OrthoWidth, -1, 0, 0, ProjectionMatrix);
				}
			}
		}

		FSceneRenderer* SceneRenderer = CreateSceneRendererForSceneCapture(
			this, 
			CaptureComponent, 
			TextureRenderTarget->GameThread_GetRenderTargetResource(), 
			CaptureSize, 
			ViewRotationMatrix, 
			ViewLocation, 
			ProjectionMatrix, 
			bUseFauxOrthoViewPos,
			CaptureComponent->MaxViewDistanceOverride, 
			bUseSceneColorTexture,
			&CaptureComponent->PostProcessSettings, 
			CaptureComponent->PostProcessBlendWeight,
			CaptureComponent->GetViewOwner());

		check(SceneRenderer != nullptr);

		SceneRenderer->Views[0].bFogOnlyOnRenderedOpaque = CaptureComponent->bConsiderUnrenderedOpaquePixelAsFullyTranslucent;

		SceneRenderer->ViewFamily.SceneCaptureSource = CaptureComponent->CaptureSource;
		SceneRenderer->ViewFamily.SceneCaptureCompositeMode = CaptureComponent->CompositeMode;

		// Ensure that the views for this scene capture reflect any simulated camera motion for this frame
		TOptional<FTransform> PreviousTransform = FMotionVectorSimulation::Get().GetPreviousTransform(CaptureComponent);

		// Process Scene View extensions for the capture component
		{
			FSceneViewExtensionContext ViewExtensionContext(SceneRenderer->Scene);
			ViewExtensionContext.bStereoDisabled = true;

			for (int32 Index = 0; Index < CaptureComponent->SceneViewExtensions.Num(); ++Index)
			{
				TSharedPtr<ISceneViewExtension, ESPMode::ThreadSafe> Extension = CaptureComponent->SceneViewExtensions[Index].Pin();
				if (Extension.IsValid())
				{
					if (Extension->IsActiveThisFrame(ViewExtensionContext))
					{
						SceneRenderer->ViewFamily.ViewExtensions.Add(Extension.ToSharedRef());
					}
				}
				else
				{
					CaptureComponent->SceneViewExtensions.RemoveAt(Index, 1, false);
					--Index;
				}
			}

			for (const TSharedRef<ISceneViewExtension, ESPMode::ThreadSafe>& Extension : SceneRenderer->ViewFamily.ViewExtensions)
			{
				Extension->SetupViewFamily(SceneRenderer->ViewFamily);
			}
		}

		{
			FPlane ClipPlane = FPlane(CaptureComponent->ClipPlaneBase, CaptureComponent->ClipPlaneNormal.GetSafeNormal());

			for (FSceneView& View : SceneRenderer->Views)
			{
				if (PreviousTransform.IsSet())
				{
					View.PreviousViewTransform = PreviousTransform.GetValue();
				}

				View.bCameraCut = CaptureComponent->bCameraCutThisFrame;

				if (CaptureComponent->bEnableClipPlane)
				{
					View.GlobalClippingPlane = ClipPlane;
					// Jitter can't be removed completely due to the clipping plane
					View.bAllowTemporalJitter = false;
				}

				for (const FSceneViewExtensionRef& Extension : SceneRenderer->ViewFamily.ViewExtensions)
				{
					Extension->SetupView(SceneRenderer->ViewFamily, View);
				}
			}
		}

		// Reset scene capture's camera cut.
		CaptureComponent->bCameraCutThisFrame = false;

		FTextureRenderTargetResource* TextureRenderTargetResource = TextureRenderTarget->GameThread_GetRenderTargetResource();

		FString EventName;
		if (!CaptureComponent->ProfilingEventName.IsEmpty())
		{
			EventName = CaptureComponent->ProfilingEventName;
		}
		else if (CaptureComponent->GetOwner())
		{
			CaptureComponent->GetOwner()->GetFName().ToString(EventName);
		}

		const bool bGenerateMips = TextureRenderTarget->bAutoGenerateMips;
		FGenerateMipsParams GenerateMipsParams{TextureRenderTarget->MipsSamplerFilter == TF_Nearest ? SF_Point : (TextureRenderTarget->MipsSamplerFilter == TF_Trilinear ? SF_Trilinear : SF_Bilinear),
			TextureRenderTarget->MipsAddressU == TA_Wrap ? AM_Wrap : (TextureRenderTarget->MipsAddressU == TA_Mirror ? AM_Mirror : AM_Clamp),
			TextureRenderTarget->MipsAddressV == TA_Wrap ? AM_Wrap : (TextureRenderTarget->MipsAddressV == TA_Mirror ? AM_Mirror : AM_Clamp)};

		const bool bDisableFlipCopyGLES = CaptureComponent->bDisableFlipCopyGLES;
		const bool bOrthographicCamera = CaptureComponent->ProjectionType == ECameraProjectionMode::Orthographic;


		// If capturing every frame, only render to the GPUs that are actually being used
		// this frame. Otherwise we will get poor performance in AFR. We can only determine
		// this by querying the viewport back buffer on the render thread, so pass that
		// along if it exists.
		FRenderTarget* GameViewportRT = nullptr;
		if (CaptureComponent->bCaptureEveryFrame)
		{
			if (GEngine->GameViewport != nullptr)
			{
				GameViewportRT = GEngine->GameViewport->Viewport;
			}
		}

		// Compositing feature is only active when using SceneColor as the source
		bool bIsCompositing = (CaptureComponent->CompositeMode != SCCM_Overwrite) && (CaptureComponent->CaptureSource == SCS_SceneColorHDR);

		ENQUEUE_RENDER_COMMAND(CaptureCommand)(
			[SceneRenderer, TextureRenderTargetResource, EventName, bGenerateMips, GenerateMipsParams, bDisableFlipCopyGLES, GameViewportRT, bEnableOrthographicTiling, bIsCompositing, bOrthographicCamera, NumXTiles, NumYTiles, TileID](FRHICommandListImmediate& RHICmdList)
			{
				if (GameViewportRT != nullptr)
				{
					const FRHIGPUMask GPUMask = AFRUtils::GetGPUMaskForGroup(GameViewportRT->GetGPUMask(RHICmdList));
					TextureRenderTargetResource->SetActiveGPUMask(GPUMask);
				}
				else
				{
					TextureRenderTargetResource->SetActiveGPUMask(FRHIGPUMask::All());
				}

				FResolveParams ResolveParams;

				if (bEnableOrthographicTiling)
				{
					const uint32 RTSizeX = TextureRenderTargetResource->GetSizeX() / NumXTiles;
					const uint32 RTSizeY = TextureRenderTargetResource->GetSizeY() / NumYTiles;
					const uint32 TileX = TileID % NumXTiles;
					const uint32 TileY = TileID / NumXTiles;
					ResolveParams.DestRect.X1 = TileX * RTSizeX;
					ResolveParams.DestRect.Y1 = TileY * RTSizeY;
					ResolveParams.DestRect.X2 = ResolveParams.DestRect.X1 + RTSizeX;
					ResolveParams.DestRect.Y2 = ResolveParams.DestRect.Y1 + RTSizeY;
				}

				// Don't clear the render target when compositing, or in a tiling mode that fills in the render target in multiple passes.
				bool bClearRenderTarget = !bIsCompositing && !bEnableOrthographicTiling;

				UpdateSceneCaptureContent_RenderThread(RHICmdList, SceneRenderer, TextureRenderTargetResource, TextureRenderTargetResource, EventName, ResolveParams, bGenerateMips, GenerateMipsParams, bDisableFlipCopyGLES, bClearRenderTarget, bOrthographicCamera);
			}
		);
	}
}

void FScene::UpdateSceneCaptureContents(USceneCaptureComponentCube* CaptureComponent)
{
	struct FLocal
	{
		/** Creates a transformation for a cubemap face, following the D3D cubemap layout. */
		static FMatrix CalcCubeFaceTransform(ECubeFace Face)
		{
			static const FVector XAxis(1.f, 0.f, 0.f);
			static const FVector YAxis(0.f, 1.f, 0.f);
			static const FVector ZAxis(0.f, 0.f, 1.f);

			// vectors we will need for our basis
			FVector vUp(YAxis);
			FVector vDir;
			switch (Face)
			{
				case CubeFace_PosX:
					vDir = XAxis;
					break;
				case CubeFace_NegX:
					vDir = -XAxis;
					break;
				case CubeFace_PosY:
					vUp = -ZAxis;
					vDir = YAxis;
					break;
				case CubeFace_NegY:
					vUp = ZAxis;
					vDir = -YAxis;
					break;
				case CubeFace_PosZ:
					vDir = ZAxis;
					break;
				case CubeFace_NegZ:
					vDir = -ZAxis;
					break;
			}
			// derive right vector
			FVector vRight(vUp ^ vDir);
			// create matrix from the 3 axes
			return FBasisVectorMatrix(vRight, vUp, vDir, FVector::ZeroVector);
		}
	} ;

	check(CaptureComponent);

	const bool bIsODS = CaptureComponent->TextureTargetLeft && CaptureComponent->TextureTargetRight && CaptureComponent->TextureTargetODS;
	const uint32 StartIndex = (bIsODS) ? 1 : 0;
	const uint32 EndIndex = (bIsODS) ? 3 : 1;
	
	UTextureRenderTargetCube* const TextureTargets[] = {
		CaptureComponent->TextureTarget, 
		CaptureComponent->TextureTargetLeft, 
		CaptureComponent->TextureTargetRight
	};

	FTransform Transform = CaptureComponent->GetComponentToWorld();
	const FVector ViewLocation = Transform.GetTranslation();

	if (CaptureComponent->bCaptureRotation)
	{
		// Remove the translation from Transform because we only need rotation.
		Transform.SetTranslation(FVector::ZeroVector);
		Transform.SetScale3D(FVector::OneVector);
	}

	for (uint32 CaptureIter = StartIndex; CaptureIter < EndIndex; ++CaptureIter)
	{
		UTextureRenderTargetCube* const TextureTarget = TextureTargets[CaptureIter];

		if (TextureTarget)
		{
			const float FOV = 90 * (float)PI / 360.0f;
			for (int32 faceidx = 0; faceidx < (int32)ECubeFace::CubeFace_MAX; faceidx++)
			{
				const ECubeFace TargetFace = (ECubeFace)faceidx;
				const FVector Location = CaptureComponent->GetComponentToWorld().GetTranslation();

				FMatrix ViewRotationMatrix;

				if (CaptureComponent->bCaptureRotation)
				{
					ViewRotationMatrix = Transform.ToInverseMatrixWithScale() * FLocal::CalcCubeFaceTransform(TargetFace);
				}
				else
				{
					ViewRotationMatrix = FLocal::CalcCubeFaceTransform(TargetFace);
				}
				FIntPoint CaptureSize(TextureTarget->GetSurfaceWidth(), TextureTarget->GetSurfaceHeight());
				FMatrix ProjectionMatrix;
				BuildProjectionMatrix(CaptureSize, FOV, GNearClippingPlane, ProjectionMatrix);
				FPostProcessSettings PostProcessSettings;

				float StereoIPD = 0.0f;
				if (bIsODS)
				{
					StereoIPD = (CaptureIter == 1) ? CaptureComponent->IPD * -0.5f : CaptureComponent->IPD * 0.5f;
				}

				bool bCaptureSceneColor = CaptureNeedsSceneColor(CaptureComponent->CaptureSource);

				FSceneRenderer* SceneRenderer = CreateSceneRendererForSceneCapture(this, CaptureComponent,
					TextureTarget->GameThread_GetRenderTargetResource(), CaptureSize, ViewRotationMatrix,
					Location, ProjectionMatrix, false, CaptureComponent->MaxViewDistanceOverride,
					bCaptureSceneColor, &PostProcessSettings, 0, CaptureComponent->GetViewOwner(), StereoIPD);

				SceneRenderer->ViewFamily.SceneCaptureSource = CaptureComponent->CaptureSource;

				FTextureRenderTargetCubeResource* TextureRenderTarget = static_cast<FTextureRenderTargetCubeResource*>(TextureTarget->GameThread_GetRenderTargetResource());
				FString EventName;
				if (!CaptureComponent->ProfilingEventName.IsEmpty())
				{
					EventName = CaptureComponent->ProfilingEventName;
				}
				else if (CaptureComponent->GetOwner())
				{
					CaptureComponent->GetOwner()->GetFName().ToString(EventName);
				}
				ENQUEUE_RENDER_COMMAND(CaptureCommand)(
					[SceneRenderer, TextureRenderTarget, EventName, TargetFace](FRHICommandListImmediate& RHICmdList)
					{
						UpdateSceneCaptureContent_RenderThread(RHICmdList, SceneRenderer, TextureRenderTarget, TextureRenderTarget, EventName, FResolveParams(FResolveRect(), TargetFace), false, FGenerateMipsParams(), false, true, false);
					}
				);
			}
		}
	}

	if (bIsODS)
	{
		const FTextureRenderTargetCubeResource* const LeftEye = static_cast<FTextureRenderTargetCubeResource*>(CaptureComponent->TextureTargetLeft->GameThread_GetRenderTargetResource());
		const FTextureRenderTargetCubeResource* const RightEye = static_cast<FTextureRenderTargetCubeResource*>(CaptureComponent->TextureTargetRight->GameThread_GetRenderTargetResource());
		FTextureRenderTargetResource* const RenderTarget = CaptureComponent->TextureTargetODS->GameThread_GetRenderTargetResource();
		const ERHIFeatureLevel::Type InFeatureLevel = FeatureLevel;

		ENQUEUE_RENDER_COMMAND(ODSCaptureCommand)(
			[LeftEye, RightEye, RenderTarget, InFeatureLevel](FRHICommandListImmediate& RHICmdList)
		{
			const ERHIAccess FinalAccess = ERHIAccess::RTV;

			FMemMark MemMark(FMemStack::Get());
			FRDGBuilder GraphBuilder(RHICmdList);
			FRDGTextureRef OutputTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(RenderTarget->GetRenderTargetTexture(), TEXT("Output")));
			FRDGTextureRef LeftEyeTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(LeftEye->TextureRHI, TEXT("LeftEye")));
			FRDGTextureRef RightEyeTexture = GraphBuilder.RegisterExternalTexture(CreateRenderTarget(RightEye->TextureRHI, TEXT("RightEye")));
			ODSCapture_RenderThread(GraphBuilder, LeftEyeTexture, RightEyeTexture, OutputTexture, InFeatureLevel);

			GraphBuilder.SetTextureAccessFinal(LeftEyeTexture, FinalAccess);
			GraphBuilder.SetTextureAccessFinal(RightEyeTexture, FinalAccess);
			GraphBuilder.Execute();
		});
	}
}
