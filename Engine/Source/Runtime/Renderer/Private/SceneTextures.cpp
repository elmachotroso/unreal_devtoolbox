// Copyright Epic Games, Inc. All Rights Reserved.

#include "SceneTextures.h"
#include "Shader.h"
#include "SceneUtils.h"
#include "SceneRenderTargetParameters.h"
#include "SceneTextureParameters.h"
#include "VelocityRendering.h"
#include "RenderUtils.h"
#include "EngineGlobals.h"
#include "UnrealEngine.h"
#include "RendererModule.h"
#include "StereoRendering.h"
#include "StereoRenderTargetManager.h"
#include "CompositionLighting/PostProcessAmbientOcclusion.h"
#include "PostProcessCompositeEditorPrimitives.h"
#include "ShaderCompiler.h"
#include "SystemTextures.h"
#include "PostProcess/PostProcessAmbientOcclusionMobile.h"
#include "PostProcess/PostProcessPixelProjectedReflectionMobile.h"
#include "IHeadMountedDisplayModule.h"
#include "Strata/Strata.h"

static TAutoConsoleVariable<int32> CVarSceneTargetsResizeMethod(
	TEXT("r.SceneRenderTargetResizeMethod"),
	0,
	TEXT("Control the scene render target resize method:\n")
	TEXT("(This value is only used in game mode and on windowing platforms unless 'r.SceneRenderTargetsResizingMethodForceOverride' is enabled.)\n")
	TEXT("0: Resize to match requested render size (Default) (Least memory use, can cause stalls when size changes e.g. ScreenPercentage)\n")
	TEXT("1: Fixed to screen resolution.\n")
	TEXT("2: Expands to encompass the largest requested render dimension. (Most memory use, least prone to allocation stalls.)"),	
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarSceneTargetsResizeMethodForceOverride(
	TEXT("r.SceneRenderTargetResizeMethodForceOverride"),
	0,
	TEXT("Forces 'r.SceneRenderTargetResizeMethod' to be respected on all configurations.\n")
	TEXT("0: Disabled.\n")
	TEXT("1: Enabled.\n"),
	ECVF_RenderThreadSafe
	);

static TAutoConsoleVariable<int32> CVarMSAACount(
	TEXT("r.MSAACount"),
	4,
	TEXT("Number of MSAA samples to use with the forward renderer.  Only used when MSAA is enabled in the rendering project settings.\n")
	TEXT("0: MSAA disabled (Temporal AA enabled)\n")
	TEXT("1: MSAA disabled\n")
	TEXT("2: Use 2x MSAA\n")
	TEXT("4: Use 4x MSAA")
	TEXT("8: Use 8x MSAA"),
	ECVF_RenderThreadSafe | ECVF_Scalability
	);

static TAutoConsoleVariable<int32> CVarGBufferFormat(
	TEXT("r.GBufferFormat"),
	1,
	TEXT("Defines the memory layout used for the GBuffer.\n")
	TEXT("(affects performance, mostly through bandwidth, quality of normals and material attributes).\n")
	TEXT(" 0: lower precision (8bit per component, for profiling)\n")
	TEXT(" 1: low precision (default)\n")
	TEXT(" 3: high precision normals encoding\n")
	TEXT(" 5: high precision"),
	ECVF_RenderThreadSafe);

static TAutoConsoleVariable<int32> CVarDefaultBackBufferPixelFormat(
	TEXT("r.DefaultBackBufferPixelFormat"),
	4,
	TEXT("Defines the default back buffer pixel format.\n")
	TEXT(" 0: 8bit RGBA\n")
	TEXT(" 1: 16bit RGBA\n")
	TEXT(" 2: Float RGB\n")
	TEXT(" 3: Float RGBA\n")
	TEXT(" 4: 10bit RGB, 2bit Alpha\n"),
	ECVF_ReadOnly);

int32 GAllowCustomMSAAResolves = 1;
static FAutoConsoleVariableRef CVarAllowCustomResolves(
   TEXT("r.MSAA.AllowCustomResolves"),
   GAllowCustomMSAAResolves,
   TEXT("Whether to use builtin HW resolve or allow custom shader MSAA resolves"),
   ECVF_RenderThreadSafe
   );

IMPLEMENT_STATIC_UNIFORM_BUFFER_SLOT(SceneTextures);
IMPLEMENT_STATIC_UNIFORM_BUFFER_STRUCT(FSceneTextureUniformParameters, "SceneTexturesStruct", SceneTextures);
IMPLEMENT_STATIC_UNIFORM_BUFFER_STRUCT(FMobileSceneTextureUniformParameters, "MobileSceneTextures", SceneTextures);

RDG_REGISTER_BLACKBOARD_STRUCT(FSceneTextures);

static EPixelFormat GetGBufferFFormat()
{
	const int32 GBufferFormat = CVarGBufferFormat.GetValueOnRenderThread();
	const bool bHighPrecisionGBuffers = (GBufferFormat >= EGBufferFormat::Force16BitsPerChannel);
	const bool bEnforce8BitPerChannel = (GBufferFormat == EGBufferFormat::Force8BitsPerChannel);
	EPixelFormat NormalGBufferFormat = bHighPrecisionGBuffers ? PF_FloatRGBA : PF_B8G8R8A8;

	if (bEnforce8BitPerChannel)
	{
		NormalGBufferFormat = PF_B8G8R8A8;
	}
	else if (GBufferFormat == EGBufferFormat::HighPrecisionNormals)
	{
		NormalGBufferFormat = PF_FloatRGBA;
	}

	return NormalGBufferFormat;
}

static EPixelFormat GetMobileSceneColorFormat(const FSceneViewFamily& ViewFamily)
{
	bool bRequiresAlphaChannel = IsMobilePropagateAlphaEnabled(ViewFamily.GetShaderPlatform());

	for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ViewIndex++)
	{
		// Planar reflections and scene captures use scene color alpha to keep track of where content has been rendered, for compositing into a different scene later
		if (ViewFamily.Views[ViewIndex]->bIsPlanarReflection || ViewFamily.Views[ViewIndex]->bIsSceneCapture)
		{
			bRequiresAlphaChannel = true;
		}
	}
	
	const EPixelFormat DefaultLowPrecisionFormat = IHeadMountedDisplayModule::IsAvailable() && IHeadMountedDisplayModule::Get().IsStandaloneStereoOnlyDevice()
		? PF_R8G8B8A8 : PF_B8G8R8A8;
	const EPixelFormat DefaultPrecisionFormat = bRequiresAlphaChannel ? PF_FloatRGBA : PF_FloatR11G11B10;

	EPixelFormat DefaultColorFormat = (!IsMobileHDR() || !GSupportsRenderTargetFormat_PF_FloatRGBA) ? DefaultLowPrecisionFormat : DefaultPrecisionFormat;

	check(GPixelFormats[DefaultColorFormat].Supported);

	EPixelFormat Format = DefaultColorFormat;
	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.SceneColorFormat"));
	int32 MobileSceneColor = CVar->GetValueOnRenderThread();
	switch (MobileSceneColor)
	{
	case 1:
		Format = PF_FloatRGBA; break;
	case 2:
		Format = PF_FloatR11G11B10; break;
	case 3:
		Format = DefaultLowPrecisionFormat; break;
	default:
		break;
	}

	return GPixelFormats[Format].Supported ? Format : DefaultColorFormat;
}

static EPixelFormat GetSceneColorFormat(const FSceneViewFamily& ViewFamily)
{
	bool bRequiresAlphaChannel = false;

	for (int32 ViewIndex = 0; ViewIndex < ViewFamily.Views.Num(); ViewIndex++)
	{
		// Planar reflections and scene captures use scene color alpha to keep track of where content has been rendered, for compositing into a different scene later
		if (ViewFamily.Views[ViewIndex]->bIsPlanarReflection || ViewFamily.Views[ViewIndex]->bIsSceneCapture)
		{
			bRequiresAlphaChannel = true;
		}
	}

	EPixelFormat Format = PF_FloatRGBA;

	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.SceneColorFormat"));

	switch (CVar->GetValueOnRenderThread())
	{
	case 0:
		Format = PF_R8G8B8A8; break;
	case 1:
		Format = PF_A2B10G10R10; break;
	case 2:
		Format = PF_FloatR11G11B10; break;
	case 3:
		Format = PF_FloatRGB; break;
	case 4:
		// default
		break;
	case 5:
		Format = PF_A32B32G32R32F; break;
	}

	// Fallback in case the scene color selected isn't supported.
	if (!GPixelFormats[Format].Supported)
	{
		Format = PF_FloatRGBA;
	}

	if (bRequiresAlphaChannel)
	{
		Format = PF_FloatRGBA;
	}

	return Format;
}

inline EPixelFormat GetMobileSceneDepthAuxPixelFormat(EShaderPlatform ShaderPlatform)
{
	if (IsMobileDeferredShadingEnabled(ShaderPlatform))
	{
		return PF_R32_FLOAT;
	}

	static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.Mobile.SceneDepthAux"));
	EPixelFormat Format = PF_R16F;
	switch (CVar->GetValueOnAnyThread())
	{
	case 1:
		Format =  PF_R16F;
		break;
	case 2:
		Format = PF_R32_FLOAT;
		break;
	}
	return Format;
}

static uint32 GetEditorPrimitiveNumSamples(ERHIFeatureLevel::Type FeatureLevel)
{
	uint32 SampleCount = 1;

	if (FeatureLevel >= ERHIFeatureLevel::SM5 && GRHISupportsMSAADepthSampleAccess)
	{
		static const auto CVar = IConsoleManager::Get().FindTConsoleVariableDataInt(TEXT("r.MSAA.CompositingSampleCount"));

		SampleCount = CVar->GetValueOnRenderThread();

		if (SampleCount <= 1)
		{
			SampleCount = 1;
		}
		else if (SampleCount <= 2)
		{
			SampleCount = 2;
		}
		else if (SampleCount <= 4)
		{
			SampleCount = 4;
		}
		else
		{
			SampleCount = 8;
		}
	}

	return SampleCount;
}

static IStereoRenderTargetManager* FindStereoRenderTargetManager()
{
	if (!GEngine->StereoRenderingDevice.IsValid() || !GEngine->StereoRenderingDevice->IsStereoEnabled())
	{
		return nullptr;
	}

	return GEngine->StereoRenderingDevice->GetRenderTargetManager();
}

static TRefCountPtr<FRHITexture2D> FindStereoDepthTexture(FIntPoint TextureExtent, uint32 NumSamples)
{
	if (IStereoRenderTargetManager* StereoRenderTargetManager = FindStereoRenderTargetManager())
	{
		TRefCountPtr<FRHITexture2D> DepthTex, SRTex;
		StereoRenderTargetManager->AllocateDepthTexture(0, TextureExtent.X, TextureExtent.Y, PF_DepthStencil, 1, TexCreate_None, TexCreate_DepthStencilTargetable | TexCreate_ShaderResource | TexCreate_InputAttachmentRead, DepthTex, SRTex, NumSamples);
		return MoveTemp(SRTex);
	}
	return nullptr;
}

/** Helper class used to track and compute a suitable scene texture extent for the renderer based on history / global configuration. */
class FSceneTextureExtentState
{
public:
	static FSceneTextureExtentState& Get()
	{
		static FSceneTextureExtentState Instance;
		return Instance;
	}

	FIntPoint Compute(const FSceneViewFamily& ViewFamily)
	{
		enum ESizingMethods { RequestedSize, ScreenRes, Grow, VisibleSizingMethodsCount };
		ESizingMethods SceneTargetsSizingMethod = Grow;

		bool bIsSceneCapture = false;
		bool bIsReflectionCapture = false;
		bool bIsVRScene = false;

		for (const FSceneView* View : ViewFamily.Views)
		{
			bIsSceneCapture |= View->bIsSceneCapture;
			bIsReflectionCapture |= View->bIsReflectionCapture;
			bIsVRScene |= (IStereoRendering::IsStereoEyeView(*View) && GEngine->XRSystem.IsValid());
		}

		FIntPoint DesiredExtent = FIntPoint::ZeroValue;
		FIntPoint DesiredFamilyExtent = FSceneRenderer::GetDesiredInternalBufferSize(ViewFamily);

		{
			bool bUseResizeMethodCVar = true;

			if (CVarSceneTargetsResizeMethodForceOverride.GetValueOnRenderThread() != 1)
			{
				if (!FPlatformProperties::SupportsWindowedMode() || bIsVRScene)
				{
					if (bIsVRScene)
					{
						if (!bIsSceneCapture && !bIsReflectionCapture)
						{
							// If this isn't a scene capture, and it's a VR scene, and the size has changed since the last time we
							// rendered a VR scene (or this is the first time), use the requested size method.
							if (DesiredFamilyExtent.X != LastStereoExtent.X || DesiredFamilyExtent.Y != LastStereoExtent.Y)
							{
								LastStereoExtent = DesiredFamilyExtent;
								SceneTargetsSizingMethod = RequestedSize;
								UE_LOG(LogRenderer, Warning, TEXT("Resizing VR buffer to %d by %d"), DesiredFamilyExtent.X, DesiredFamilyExtent.Y);
							}
							else
							{
								// Otherwise use the grow method.
								SceneTargetsSizingMethod = Grow;
							}
						}
						else
						{
							// If this is a scene capture, and it's smaller than the VR view size, then don't re-allocate buffers, just use the "grow" method.
							// If it's bigger than the VR view, then log a warning, and use resize method.
							if (DesiredFamilyExtent.X > LastStereoExtent.X || DesiredFamilyExtent.Y > LastStereoExtent.Y)
							{
								if (LastStereoExtent.X > 0 && bIsSceneCapture)
								{
									static bool DisplayedCaptureSizeWarning = false;
									if (!DisplayedCaptureSizeWarning)
									{
										DisplayedCaptureSizeWarning = true;
										UE_LOG(LogRenderer, Warning, TEXT("Scene capture of %d by %d is larger than the current VR target. If this is deliberate for a capture that is being done for multiple frames, consider the performance and memory implications. To disable this warning and ensure optimal behavior with this path, set r.SceneRenderTargetResizeMethod to 2, and r.SceneRenderTargetResizeMethodForceOverride to 1."), DesiredFamilyExtent.X, DesiredFamilyExtent.Y);
									}
								}
								SceneTargetsSizingMethod = RequestedSize;
							}
							else
							{
								SceneTargetsSizingMethod = Grow;
							}
						}
					}
					else
					{
						// Force ScreenRes on non windowed platforms.
						SceneTargetsSizingMethod = RequestedSize;
					}
					bUseResizeMethodCVar = false;
				}
				else if (GIsEditor)
				{
					// Always grow scene render targets in the editor.
					SceneTargetsSizingMethod = Grow;
					bUseResizeMethodCVar = false;
				}
			}

			if (bUseResizeMethodCVar)
			{
				// Otherwise use the setting specified by the console variable.
				SceneTargetsSizingMethod = (ESizingMethods)FMath::Clamp(CVarSceneTargetsResizeMethod.GetValueOnRenderThread(), 0, (int32)VisibleSizingMethodsCount);
			}
		}

		switch (SceneTargetsSizingMethod)
		{
		case RequestedSize:
			DesiredExtent = DesiredFamilyExtent;
			break;

		case ScreenRes:
			DesiredExtent = FIntPoint(GSystemResolution.ResX, GSystemResolution.ResY);
			break;

		case Grow:
			DesiredExtent = FIntPoint(
				FMath::Max((int32)LastExtent.X, DesiredFamilyExtent.X),
				FMath::Max((int32)LastExtent.Y, DesiredFamilyExtent.Y));
			break;

		default:
			checkNoEntry();
		}

		const uint32 FrameNumber = ViewFamily.FrameNumber;
		if (ThisFrameNumber != FrameNumber)
		{
			ThisFrameNumber = FrameNumber;
			if (++DesiredExtentIndex == ExtentHistoryCount)
			{
				DesiredExtentIndex -= ExtentHistoryCount;
			}
			// This allows the extent to shrink each frame (in game)
			LargestDesiredExtents[DesiredExtentIndex] = FIntPoint::ZeroValue;
			HistoryFlags[DesiredExtentIndex] = ERenderTargetHistory::None;
		}

		// this allows The extent to not grow below the SceneCapture requests (happen before scene rendering, in the same frame with a Grow request)
		FIntPoint& LargestDesiredExtentThisFrame = LargestDesiredExtents[DesiredExtentIndex];
		LargestDesiredExtentThisFrame = LargestDesiredExtentThisFrame.ComponentMax(DesiredExtent);
		bool bIsHighResScreenshot = GIsHighResScreenshot;
		UpdateHistoryFlags(HistoryFlags[DesiredExtentIndex], bIsSceneCapture, bIsReflectionCapture, bIsHighResScreenshot);

		// We want to shrink the buffer but as we can have multiple scene captures per frame we have to delay that a frame to get all size requests
		// Don't save buffer size in history while making high-res screenshot.
		// We have to use the requested size when allocating an hmd depth target to ensure it matches the hmd allocated render target size.
		bool bAllowDelayResize = !GIsHighResScreenshot && !bIsVRScene;

		// Don't consider the history buffer when the aspect ratio changes, the existing buffers won't make much sense at all.
		// This prevents problems when orientation changes on mobile in particular.
		// bIsReflectionCapture is explicitly checked on all platforms to prevent aspect ratio change detection from forcing the immediate buffer resize.
		// This ensures that 1) buffers are not resized spuriously during reflection rendering 2) all cubemap faces use the same render target size.
		if (bAllowDelayResize && !bIsReflectionCapture && !AnyCaptureRenderedRecently<ExtentHistoryCount>(ERenderTargetHistory::MaskAll))
		{
			const bool bAspectRatioChanged =
				!LastExtent.Y ||
				!FMath::IsNearlyEqual(
					(float)LastExtent.X / LastExtent.Y,
					(float)DesiredExtent.X / DesiredExtent.Y);

			if (bAspectRatioChanged)
			{
				bAllowDelayResize = false;

				// At this point we're assuming a simple output resize and forcing a hard swap so clear the history.
				// If we don't the next frame will fail this check as the allocated aspect ratio will match the new
				// frame's forced size so we end up looking through the history again, finding the previous old size
				// and reallocating. Only after a few frames can the results actually settle when the history clears 
				for (int32 i = 0; i < ExtentHistoryCount; ++i)
				{
					LargestDesiredExtents[i] = FIntPoint::ZeroValue;
					HistoryFlags[i] = ERenderTargetHistory::None;
				}
			}
		}
		const bool bAnyHighresScreenshotRecently = AnyCaptureRenderedRecently<ExtentHistoryCount>(ERenderTargetHistory::HighresScreenshot);
		if (bAnyHighresScreenshotRecently != GIsHighResScreenshot)
		{
			bAllowDelayResize = false;
		}

		if (bAllowDelayResize)
		{
			for (int32 i = 0; i < ExtentHistoryCount; ++i)
			{
				DesiredExtent = DesiredExtent.ComponentMax(LargestDesiredExtents[i]);
			}
		}

		check(DesiredExtent.X > 0 && DesiredExtent.Y > 0);
		QuantizeSceneBufferSize(DesiredExtent, DesiredExtent);
		LastExtent = DesiredExtent;
		return DesiredExtent;
	}

	void ResetHistory()
	{
		LastStereoExtent = FIntPoint(0, 0);
		LastExtent = FIntPoint(0, 0);
	}

private:
	enum class ERenderTargetHistory
	{
		None				= 0,
		SceneCapture		= 1 << 0,
		ReflectionCapture	= 1 << 1,
		HighresScreenshot	= 1 << 2,
		MaskAll				= 1 << 3,
	};
	FRIEND_ENUM_CLASS_FLAGS(ERenderTargetHistory);

	static void UpdateHistoryFlags(ERenderTargetHistory& Flags, bool bIsSceneCapture, bool bIsReflectionCapture, bool bIsHighResScreenShot)
	{
		Flags |= bIsSceneCapture ? ERenderTargetHistory::SceneCapture : ERenderTargetHistory::None;
		Flags |= bIsReflectionCapture ? ERenderTargetHistory::ReflectionCapture : ERenderTargetHistory::None;
		Flags |= bIsHighResScreenShot ? ERenderTargetHistory::HighresScreenshot : ERenderTargetHistory::None;
	}

	template <uint32 EntryCount>
	bool AnyCaptureRenderedRecently(ERenderTargetHistory Mask) const
	{
		ERenderTargetHistory Result = ERenderTargetHistory::None;
		for (uint32 EntryIndex = 0; EntryIndex < EntryCount; ++EntryIndex)
		{
			Result |= HistoryFlags[EntryIndex] & Mask;
		}
		return Result != ERenderTargetHistory::None;
	}

	FSceneTextureExtentState()
	{
		FMemory::Memset(LargestDesiredExtents, 0);
		FMemory::Memset(HistoryFlags, 0, sizeof(HistoryFlags));
	}

	FIntPoint LastStereoExtent = FIntPoint(0, 0);
	FIntPoint LastExtent = FIntPoint(0, 0);

	/** as we might get multiple extent requests each frame for SceneCaptures and we want to avoid reallocations we can only go as low as the largest request */
	static const uint32 ExtentHistoryCount = 3;
	uint32 DesiredExtentIndex = 0;
	FIntPoint LargestDesiredExtents[ExtentHistoryCount];
	ERenderTargetHistory HistoryFlags[ExtentHistoryCount];

	/** to detect when LargestDesiredSizeThisFrame is outdated */
	uint32 ThisFrameNumber = 0;
};

void ResetSceneTextureExtentHistory()
{
	FSceneTextureExtentState::Get().ResetHistory();
}

ENUM_CLASS_FLAGS(FSceneTextureExtentState::ERenderTargetHistory);

FSceneTexturesConfig FSceneTexturesConfig::GlobalInstance;

FSceneTexturesConfig FSceneTexturesConfig::Create(const FSceneViewFamily& ViewFamily)
{
	FSceneTexturesConfig Config;
	Config.FeatureLevel = ViewFamily.GetFeatureLevel();
	Config.ShadingPath = FSceneInterface::GetShadingPath(Config.FeatureLevel);
	Config.ShaderPlatform = GetFeatureLevelShaderPlatform(Config.FeatureLevel);
	Config.Extent = FSceneTextureExtentState::Get().Compute(ViewFamily);
	Config.NumSamples = GetDefaultMSAACount(Config.FeatureLevel, GDynamicRHI->RHIGetPlatformTextureMaxSampleCount());
	Config.EditorPrimitiveNumSamples = GetEditorPrimitiveNumSamples(Config.FeatureLevel);
	Config.ColorFormat = PF_Unknown;
	Config.ColorClearValue = FClearValueBinding::Black;
	Config.DepthClearValue = FClearValueBinding::DepthFar;
	Config.CustomDepthDownsampleFactor = GetCustomDepthDownsampleFactor(Config.FeatureLevel);
	Config.bRequireMultiView = ViewFamily.bRequireMultiView;
	Config.bIsUsingGBuffers = IsUsingGBuffers(Config.ShaderPlatform);

	switch (Config.ShadingPath)
	{
	case EShadingPath::Deferred:
	{
		Config.ColorFormat = GetSceneColorFormat(ViewFamily);
		break;
	}

	case EShadingPath::Mobile:
	{
		Config.ColorFormat = GetMobileSceneColorFormat(ViewFamily);
		break;
	}

	default:
		checkNoEntry();
	}

	if (Config.bIsUsingGBuffers)
	{
		const FGBufferParams GBufferParams = FShaderCompileUtilities::FetchGBufferParamsRuntime(Config.ShaderPlatform);

		// GBuffer configuration information is expensive to compute, the results are cached between runs.
		if (!IsSceneTexturesValid() || GlobalInstance.GBufferParams != GBufferParams)
		{
			const FGBufferInfo GBufferInfo = FetchFullGBufferInfo(GBufferParams);

			Config.GBufferA = FindGBufferBindingByName(GBufferInfo, TEXT("GBufferA"));
			Config.GBufferB = FindGBufferBindingByName(GBufferInfo, TEXT("GBufferB"));
			Config.GBufferC = FindGBufferBindingByName(GBufferInfo, TEXT("GBufferC"));
			Config.GBufferD = FindGBufferBindingByName(GBufferInfo, TEXT("GBufferD"));
			Config.GBufferE = FindGBufferBindingByName(GBufferInfo, TEXT("GBufferE"));
			Config.GBufferVelocity = FindGBufferBindingByName(GBufferInfo, TEXT("Velocity"));
		}
		// Same GBuffer configuration is the same, reuse results from previous config.
		else
		{
			Config.GBufferA = GlobalInstance.GBufferA;
			Config.GBufferB = GlobalInstance.GBufferB;
			Config.GBufferC = GlobalInstance.GBufferC;
			Config.GBufferD = GlobalInstance.GBufferD;
			Config.GBufferE = GlobalInstance.GBufferE;
			Config.GBufferVelocity = GlobalInstance.GBufferVelocity;
		}

		Config.GBufferParams = GBufferParams;
	}

	return Config;
}

void FSceneTexturesConfig::Set(const FSceneTexturesConfig& Config)
{
	GlobalInstance = Config;
}

const FSceneTexturesConfig& FSceneTexturesConfig::Get()
{
	return GlobalInstance;
}

FSceneTextures& FMinimalSceneTextures::Create(FRDGBuilder& GraphBuilder, const FSceneTexturesConfig& Config)
{
	checkf(IsSceneTexturesValid(), TEXT("Attempted to create scene textures with an empty config."));

	FSceneTextures& SceneTextures = GraphBuilder.Blackboard.Create<FSceneTextures>(Config);

	// Scene Depth

	// If not using MSAA, we need to make sure to grab the stereo depth texture if appropriate.
	FTexture2DRHIRef StereoDepthRHI;
	if (Config.NumSamples == 1 && (StereoDepthRHI = FindStereoDepthTexture(Config.Extent, Config.NumSamples)) != nullptr)
	{
		SceneTextures.Depth = RegisterExternalTexture(GraphBuilder, StereoDepthRHI, TEXT("SceneDepthZ"));
		SceneTextures.Stencil = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(SceneTextures.Depth.Target, PF_X24_G8));
	}
	else
	{
		ETextureCreateFlags Flags = TexCreate_DepthStencilTargetable | TexCreate_ShaderResource | TexCreate_InputAttachmentRead | GFastVRamConfig.SceneDepth;

		if (!Config.bKeepDepthContent)
		{
			Flags |= TexCreate_Memoryless;
		}

		if (Config.NumSamples == 1 && GRHISupportsDepthUAV)
		{
			Flags |= TexCreate_UAV;
		}

		// TODO: Array-size could be values > 2, on upcoming XR devices. This should be part of the config.
		FRDGTextureDesc Desc(Config.bRequireMultiView ?
							 FRDGTextureDesc::Create2DArray(SceneTextures.Config.Extent, PF_DepthStencil, Config.DepthClearValue, Flags, 2) :
							 FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_DepthStencil, Config.DepthClearValue, Flags));
		Desc.NumSamples = Config.NumSamples;
		SceneTextures.Depth = GraphBuilder.CreateTexture(Desc, TEXT("SceneDepthZ"));

		if (Desc.NumSamples > 1)
		{
			Desc.NumSamples = 1;

			if ((StereoDepthRHI = FindStereoDepthTexture(Config.Extent, Desc.NumSamples)) != nullptr)
			{
				SceneTextures.Depth.Resolve = RegisterExternalTexture(GraphBuilder, StereoDepthRHI, TEXT("SceneDepthZ"));
			}
			else
			{
				SceneTextures.Depth.Resolve = GraphBuilder.CreateTexture(Desc, TEXT("SceneDepthZ"));
			}
		}

		SceneTextures.Stencil = GraphBuilder.CreateSRV(FRDGTextureSRVDesc::CreateWithPixelFormat(SceneTextures.Depth.Target, PF_X24_G8));
	}

	// Scene Color
	{
		const bool bIsMobilePlatform = Config.ShadingPath == EShadingPath::Mobile;

		ETextureCreateFlags Flags = TexCreate_RenderTargetable | TexCreate_ShaderResource | GFastVRamConfig.SceneColor;
		const ETextureCreateFlags sRGBFlag = (bIsMobilePlatform && IsMobileColorsRGB()) ? TexCreate_SRGB : TexCreate_None;

		if (Config.FeatureLevel >= ERHIFeatureLevel::SM5 && Config.NumSamples == 1)
		{
			Flags |= TexCreate_UAV;
		}
		Flags |= sRGBFlag;

		const TCHAR* SceneColorName = TEXT("SceneColor");

		// Create the scene color.
		// TODO: Array-size could be values > 2, on upcoming XR devices. This should be part of the config.
		FRDGTextureDesc Desc(Config.bRequireMultiView ?
							 FRDGTextureDesc::Create2DArray(Config.Extent, Config.ColorFormat, Config.ColorClearValue, Flags, 2) :
							 FRDGTextureDesc::Create2D(Config.Extent, Config.ColorFormat, Config.ColorClearValue, Flags));
		Desc.NumSamples = Config.NumSamples;
		SceneTextures.Color = GraphBuilder.CreateTexture(Desc, SceneColorName);

		if (Desc.NumSamples > 1)
		{
			Desc.NumSamples = 1;
			Desc.Flags = TexCreate_ResolveTargetable | TexCreate_ShaderResource | GFastVRamConfig.SceneColor | sRGBFlag;

			SceneTextures.Color.Resolve = GraphBuilder.CreateTexture(Desc, SceneColorName);
		}
	}

	// Custom Depth
	SceneTextures.CustomDepth = FCustomDepthTextures::Create(GraphBuilder, Config.Extent, Config.FeatureLevel, Config.CustomDepthDownsampleFactor);

	return SceneTextures;
}

FSceneTextureShaderParameters FMinimalSceneTextures::GetSceneTextureShaderParameters(ERHIFeatureLevel::Type FeatureLevel) const
{
	FSceneTextureShaderParameters OutSceneTextureShaderParameters;
	if (FeatureLevel >= ERHIFeatureLevel::SM5)
	{
		OutSceneTextureShaderParameters.SceneTextures = UniformBuffer;
	}
	else
	{
		OutSceneTextureShaderParameters.MobileSceneTextures = MobileUniformBuffer;
	}
	return OutSceneTextureShaderParameters;
}

FSceneTextures& FSceneTextures::Create(FRDGBuilder& GraphBuilder, const FSceneTexturesConfig& Config)
{
	FSceneTextures& SceneTextures = FMinimalSceneTextures::Create(GraphBuilder, Config);

	if (Config.ShadingPath == EShadingPath::Deferred)
	{
		// Screen Space Ambient Occlusion
		SceneTextures.ScreenSpaceAO = CreateScreenSpaceAOTexture(GraphBuilder, Config.Extent);

		// Small Depth
		const FIntPoint SmallDepthExtent = GetDownscaledExtent(Config.Extent, Config.SmallDepthDownsampleFactor);
		const FRDGTextureDesc SmallDepthDesc(FRDGTextureDesc::Create2D(SmallDepthExtent, PF_DepthStencil, FClearValueBinding::None, TexCreate_DepthStencilTargetable | TexCreate_ShaderResource));
		SceneTextures.SmallDepth = GraphBuilder.CreateTexture(SmallDepthDesc, TEXT("SmallDepthZ"));
	}
	else
	{
		// Mobile Screen Space Ambient Occlusion
		SceneTextures.ScreenSpaceAO = CreateMobileScreenSpaceAOTexture(GraphBuilder, Config);

		if (Config.MobilePixelProjectedReflectionExtent != FIntPoint::ZeroValue)
		{
			SceneTextures.PixelProjectedReflection = CreateMobilePixelProjectedReflectionTexture(GraphBuilder, Config.MobilePixelProjectedReflectionExtent);
		}
	}

	// Velocity
	SceneTextures.Velocity = GraphBuilder.CreateTexture(FVelocityRendering::GetRenderTargetDesc(Config.ShaderPlatform, Config.Extent), TEXT("SceneVelocity"));

	if (Config.bIsUsingGBuffers)
	{
		ETextureCreateFlags FlagsToAdd = TexCreate_None;
		
		if (Config.GBufferA.Index >= 0)
		{
			const FRDGTextureDesc Desc(FRDGTextureDesc::Create2D(Config.Extent, Config.GBufferA.Format, FClearValueBinding::Transparent, Config.GBufferA.Flags | FlagsToAdd | GFastVRamConfig.GBufferA));
			SceneTextures.GBufferA = GraphBuilder.CreateTexture(Desc, TEXT("GBufferA"));
		}

		if (Config.GBufferB.Index >= 0)
		{
			const FRDGTextureDesc Desc(FRDGTextureDesc::Create2D(Config.Extent, Config.GBufferB.Format, FClearValueBinding::Transparent, Config.GBufferB.Flags | FlagsToAdd | GFastVRamConfig.GBufferB));
			SceneTextures.GBufferB = GraphBuilder.CreateTexture(Desc, TEXT("GBufferB"));
		}

		if (Config.GBufferC.Index >= 0)
		{
			const FRDGTextureDesc Desc(FRDGTextureDesc::Create2D(Config.Extent, Config.GBufferC.Format, FClearValueBinding::Transparent, Config.GBufferC.Flags | FlagsToAdd | GFastVRamConfig.GBufferC));
			SceneTextures.GBufferC = GraphBuilder.CreateTexture(Desc, TEXT("GBufferC"));
		}

		if (Config.GBufferD.Index >= 0)
		{
			const FRDGTextureDesc Desc(FRDGTextureDesc::Create2D(Config.Extent, Config.GBufferD.Format, FClearValueBinding::Transparent, Config.GBufferD.Flags | FlagsToAdd | GFastVRamConfig.GBufferD));
			SceneTextures.GBufferD = GraphBuilder.CreateTexture(Desc, TEXT("GBufferD"));
		}

		if (Config.GBufferE.Index >= 0)
		{
			const FRDGTextureDesc Desc(FRDGTextureDesc::Create2D(Config.Extent, Config.GBufferE.Format, FClearValueBinding::Transparent, Config.GBufferE.Flags | FlagsToAdd | GFastVRamConfig.GBufferE));
			SceneTextures.GBufferE = GraphBuilder.CreateTexture(Desc, TEXT("GBufferE"));
		}

		// GBufferF is not yet part of the data driven GBuffer info.
		if (Config.ShadingPath == EShadingPath::Deferred)
		{
			const FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(Config.Extent, GetGBufferFFormat(), FClearValueBinding({ 0.5f, 0.5f, 0.5f, 0.5f }), TexCreate_RenderTargetable | TexCreate_ShaderResource | FlagsToAdd | GFastVRamConfig.GBufferF);
			SceneTextures.GBufferF = GraphBuilder.CreateTexture(Desc, TEXT("GBufferF"));
		}
	}


	if(Config.ShadingPath == EShadingPath::Mobile && MobileRequiresSceneDepthAux(Config.ShaderPlatform))
	{
		const float FarDepth = (float)ERHIZBuffer::FarPlane;
		const FLinearColor FarDepthColor(FarDepth, FarDepth, FarDepth, FarDepth);
		ETextureCreateFlags FlagsToAdd = IsMobileDeferredShadingEnabled(Config.ShaderPlatform)? TexCreate_Memoryless : TexCreate_None;
		FRDGTextureDesc Desc = Config.bRequireMultiView ? 
			FRDGTextureDesc::Create2DArray(Config.Extent, GetMobileSceneDepthAuxPixelFormat(Config.ShaderPlatform), FClearValueBinding(FarDepthColor), TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_InputAttachmentRead | FlagsToAdd, 2) : 
			FRDGTextureDesc::Create2D(Config.Extent, GetMobileSceneDepthAuxPixelFormat(Config.ShaderPlatform), FClearValueBinding(FarDepthColor), TexCreate_RenderTargetable | TexCreate_ShaderResource | TexCreate_InputAttachmentRead| FlagsToAdd);
		Desc.NumSamples = Config.NumSamples;
		SceneTextures.DepthAux = GraphBuilder.CreateTexture(Desc, TEXT("SceneDepthAux"));
		
		if (Desc.NumSamples > 1)
		{
			Desc.NumSamples = 1;
			Desc.Flags = TexCreate_ResolveTargetable | TexCreate_ShaderResource;
			SceneTextures.DepthAux.Resolve = GraphBuilder.CreateTexture(Desc, TEXT("SceneDepthAux"));
		}
	}
#if WITH_EDITOR
	{
		const FRDGTextureDesc ColorDesc(FRDGTextureDesc::Create2D(Config.Extent, PF_B8G8R8A8, FClearValueBinding::Transparent, TexCreate_ShaderResource | TexCreate_RenderTargetable, 1, Config.EditorPrimitiveNumSamples));
		SceneTextures.EditorPrimitiveColor = GraphBuilder.CreateTexture(ColorDesc, TEXT("Editor.PrimitivesColor"));

		const FRDGTextureDesc DepthDesc(FRDGTextureDesc::Create2D(Config.Extent, PF_DepthStencil, FClearValueBinding::DepthFar, TexCreate_ShaderResource | TexCreate_DepthStencilTargetable, 1, Config.EditorPrimitiveNumSamples));
		SceneTextures.EditorPrimitiveDepth = GraphBuilder.CreateTexture(DepthDesc, TEXT("Editor.PrimitivesDepth"));
	}
#endif

#if WITH_DEBUG_VIEW_MODES
	if (AllowDebugViewShaderMode(DVSM_QuadComplexity, Config.ShaderPlatform, Config.FeatureLevel))
	{
		FIntPoint QuadOverdrawExtent;
		QuadOverdrawExtent.X = 2 * FMath::Max<uint32>((Config.Extent.X + 1) / 2, 1); // The size is time 2 since left side is QuadDescriptor, and right side QuadComplexity.
		QuadOverdrawExtent.Y =     FMath::Max<uint32>((Config.Extent.Y + 1) / 2, 1);

		const FRDGTextureDesc QuadOverdrawDesc(FRDGTextureDesc::Create2D(QuadOverdrawExtent, PF_R32_UINT, FClearValueBinding::None, TexCreate_ShaderResource | TexCreate_RenderTargetable | TexCreate_UAV));
		SceneTextures.QuadOverdraw = GraphBuilder.CreateTexture(QuadOverdrawDesc, TEXT("QuadOverdrawTexture"));
	}
#endif

	return SceneTextures;
}

const FSceneTextures& FSceneTextures::Get(FRDGBuilder& GraphBuilder)
{
	const FSceneTextures* SceneTextures = GraphBuilder.Blackboard.Get<FSceneTextures>();
	checkf(SceneTextures, TEXT("FSceneTextures was not initialized. Call FSceneTextures::Create() first."));
	return *SceneTextures;
}

uint32 FSceneTextures::GetGBufferRenderTargets(TStaticArray<FTextureRenderTargetBinding, MaxSimultaneousRenderTargets>& RenderTargets) const
{
	uint32 RenderTargetCount = 0;

	// All configurations use scene color in the first slot.
	RenderTargets[RenderTargetCount++] = FTextureRenderTargetBinding(Color.Target);

	if (Config.bIsUsingGBuffers)
	{
		struct FGBufferEntry
		{
			FGBufferEntry(const TCHAR* InName, FRDGTextureRef InTexture, int32 InIndex)
				: Name(InName)
				, Texture(InTexture)
				, Index(InIndex)
			{}

			const TCHAR* Name;
			FRDGTextureRef Texture;
			int32 Index;
		};

		const FGBufferEntry GBufferEntries[] =
		{
			{ TEXT("GBufferA"), GBufferA, Config.GBufferA.Index },
			{ TEXT("GBufferB"), GBufferB, Config.GBufferB.Index },
			{ TEXT("GBufferC"), GBufferC, Config.GBufferC.Index },
			{ TEXT("GBufferD"), GBufferD, Config.GBufferD.Index },
			{ TEXT("GBufferE"), GBufferE, Config.GBufferE.Index },
			{ TEXT("Velocity"), Velocity, Config.GBufferVelocity.Index }
		};

		for (const FGBufferEntry& Entry : GBufferEntries)
		{
			checkf(Entry.Index <= 0 || Entry.Texture != nullptr, TEXT("Texture '%s' was requested by FGBufferInfo, but it is null."), Entry.Name);
			if (Entry.Index > 0)
			{
				RenderTargets[Entry.Index] = FTextureRenderTargetBinding(Entry.Texture);
				RenderTargetCount = FMath::Max(RenderTargetCount, uint32(Entry.Index + 1));
			}
		}
	}
	// Forward shading path. Simple forward shading does not use velocity.
	else if (IsUsingBasePassVelocity(Config.ShaderPlatform) && !IsSimpleForwardShadingEnabled(Config.ShaderPlatform))
	{
		RenderTargets[RenderTargetCount++] = FTextureRenderTargetBinding(Velocity);
	}

	return RenderTargetCount;
}

uint32 FSceneTextures::GetGBufferRenderTargets(ERenderTargetLoadAction LoadAction, FRenderTargetBindingSlots& RenderTargetBindingSlots) const
{
	TStaticArray<FTextureRenderTargetBinding, MaxSimultaneousRenderTargets> RenderTargets;
	const uint32 RenderTargetCount = GetGBufferRenderTargets(RenderTargets);
	for (uint32 Index = 0; Index < RenderTargetCount; ++Index)
	{
		RenderTargetBindingSlots[Index] = FRenderTargetBinding(RenderTargets[Index].Texture, LoadAction);
	}
	return RenderTargetCount;
}

void FSceneTextureExtracts::QueueExtractions(FRDGBuilder& GraphBuilder, const FSceneTextures& SceneTextures)
{
	// Free up the memory for reuse during the RDG execution phase.
	Release();

	ESceneTextureSetupMode SetupMode = ESceneTextureSetupMode::None;

	const auto ExtractIfProduced = [&](FRDGTextureRef Texture, TRefCountPtr<IPooledRenderTarget>& OutTarget)
	{
		if (HasBeenProduced(Texture) && !EnumHasAnyFlags(Texture->Desc.Flags, TexCreate_Memoryless))
		{
			GraphBuilder.QueueTextureExtraction(Texture, &OutTarget, ERDGResourceExtractionFlags::AllowTransient);
		}
	};

	if (EnumHasAnyFlags(SceneTextures.Config.Extracts, ESceneTextureExtracts::Depth))
	{
		SetupMode |= ESceneTextureSetupMode::SceneDepth;
		ExtractIfProduced(SceneTextures.Depth.Resolve, Depth);
	}

	if (EnumHasAnyFlags(SceneTextures.Config.Extracts, ESceneTextureExtracts::CustomDepth))
	{
		SetupMode |= ESceneTextureSetupMode::CustomDepth;
		ExtractIfProduced(SceneTextures.CustomDepth.Depth, CustomDepth);
		ExtractIfProduced(SceneTextures.CustomDepth.MobileDepth, MobileCustomDepth);
		ExtractIfProduced(SceneTextures.CustomDepth.MobileStencil, MobileCustomStencil);
	}

	// Create and extract a scene texture uniform buffer for RHI code outside of the main render graph instance. This
	// uniform buffer will reference all extracted textures. No transitions will be required since the textures are left
	// in a shader resource state.
	auto* PassParameters = GraphBuilder.AllocParameters<FSceneTextureShaderParameters>();
	*PassParameters = CreateSceneTextureShaderParameters(GraphBuilder, SceneTextures.Config.FeatureLevel, SetupMode);

	// We want these textures in a SRV Compute | Raster state.
	const ERDGPassFlags PassFlags = ERDGPassFlags::Raster | ERDGPassFlags::SkipRenderPass | ERDGPassFlags::Compute | ERDGPassFlags::NeverCull;

	GraphBuilder.AddPass(RDG_EVENT_NAME("ExtractUniformBuffer"), PassParameters, PassFlags,
		[this, PassParameters, ShadingPath = SceneTextures.Config.ShadingPath](FRHICommandList&)
	{
		if (ShadingPath == EShadingPath::Deferred)
		{
			UniformBuffer = PassParameters->SceneTextures->GetRHIRef();
		}
		else
		{
			MobileUniformBuffer = PassParameters->MobileSceneTextures->GetRHIRef();
		}
	});
}

void FSceneTextureExtracts::Release()
{
	Depth = {};
	CustomDepth = {};
	MobileCustomDepth = {};
	MobileCustomStencil = {};
	UniformBuffer = {};
	MobileUniformBuffer = {};
}

static TGlobalResource<FSceneTextureExtracts> GSceneTextureExtracts;

const FSceneTextureExtracts& GetSceneTextureExtracts()
{
	return GSceneTextureExtracts;
}

void QueueSceneTextureExtractions(FRDGBuilder& GraphBuilder, const FSceneTextures& SceneTextures)
{
	return GSceneTextureExtracts.QueueExtractions(GraphBuilder, SceneTextures);
}

void SetupSceneTextureUniformParameters(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	ESceneTextureSetupMode SetupMode,
	FSceneTextureUniformParameters& SceneTextureParameters)
{
	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);

	SceneTextureParameters.PointClampSampler = TStaticSamplerState<SF_Point>::GetRHI();
	SceneTextureParameters.SceneColorTexture = SystemTextures.Black;
	SceneTextureParameters.SceneDepthTexture = SystemTextures.DepthDummy;
	SceneTextureParameters.GBufferATexture = SystemTextures.Black;
	SceneTextureParameters.GBufferBTexture = SystemTextures.Black;
	SceneTextureParameters.GBufferCTexture = SystemTextures.Black;
	SceneTextureParameters.GBufferDTexture = SystemTextures.Black;
	SceneTextureParameters.GBufferETexture = SystemTextures.Black;
	SceneTextureParameters.GBufferFTexture = SystemTextures.MidGrey;
	SceneTextureParameters.GBufferVelocityTexture = SystemTextures.Black;
	SceneTextureParameters.ScreenSpaceAOTexture = GetScreenSpaceAOFallback(SystemTextures);
	SceneTextureParameters.CustomDepthTexture = SystemTextures.DepthDummy;
	SceneTextureParameters.CustomStencilTexture = SystemTextures.StencilDummySRV;

	if (const FSceneTextures* SceneTextures = GraphBuilder.Blackboard.Get<FSceneTextures>())
	{
		const EShaderPlatform ShaderPlatform = SceneTextures->Config.ShaderPlatform;

		if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::SceneColor))
		{
			SceneTextureParameters.SceneColorTexture = SceneTextures->Color.Resolve;
		}

		if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::SceneDepth))
		{
			SceneTextureParameters.SceneDepthTexture = SceneTextures->Depth.Resolve;
		}

		if (IsUsingGBuffers(ShaderPlatform) || IsSimpleForwardShadingEnabled(ShaderPlatform))
		{
			if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::GBufferA) && HasBeenProduced(SceneTextures->GBufferA))
			{
				SceneTextureParameters.GBufferATexture = SceneTextures->GBufferA;
			}

			if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::GBufferB) && HasBeenProduced(SceneTextures->GBufferB))
			{
				SceneTextureParameters.GBufferBTexture = SceneTextures->GBufferB;
			}

			if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::GBufferC) && HasBeenProduced(SceneTextures->GBufferC))
			{
				SceneTextureParameters.GBufferCTexture = SceneTextures->GBufferC;
			}

			if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::GBufferD) && HasBeenProduced(SceneTextures->GBufferD))
			{
				SceneTextureParameters.GBufferDTexture = SceneTextures->GBufferD;
			}

			if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::GBufferE) && HasBeenProduced(SceneTextures->GBufferE))
			{
				SceneTextureParameters.GBufferETexture = SceneTextures->GBufferE;
			}

			if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::GBufferF) && HasBeenProduced(SceneTextures->GBufferF))
			{
				SceneTextureParameters.GBufferFTexture = SceneTextures->GBufferF;
			}
		}

		if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::SceneVelocity) && HasBeenProduced(SceneTextures->Velocity))
		{
			SceneTextureParameters.GBufferVelocityTexture = SceneTextures->Velocity;
		}

		if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::SSAO) && HasBeenProduced(SceneTextures->ScreenSpaceAO))
		{
			SceneTextureParameters.ScreenSpaceAOTexture = SceneTextures->ScreenSpaceAO;
		}

		if (EnumHasAnyFlags(SetupMode, ESceneTextureSetupMode::CustomDepth))
		{
			const FCustomDepthTextures& CustomDepthTextures = SceneTextures->CustomDepth;

			if (HasBeenProduced(CustomDepthTextures.Depth))
			{
				SceneTextureParameters.CustomDepthTexture = CustomDepthTextures.Depth;
				SceneTextureParameters.CustomStencilTexture = CustomDepthTextures.Stencil;
			}
		}
	}
}

TRDGUniformBufferRef<FSceneTextureUniformParameters> CreateSceneTextureUniformBuffer(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	ESceneTextureSetupMode SetupMode)
{
	FSceneTextureUniformParameters* SceneTextures = GraphBuilder.AllocParameters<FSceneTextureUniformParameters>();
	SetupSceneTextureUniformParameters(GraphBuilder, FeatureLevel, SetupMode, *SceneTextures);
	return GraphBuilder.CreateUniformBuffer(SceneTextures);
}

EMobileSceneTextureSetupMode Translate(ESceneTextureSetupMode InSetupMode)
{
	EMobileSceneTextureSetupMode OutSetupMode = EMobileSceneTextureSetupMode::None;
	if (EnumHasAnyFlags(InSetupMode, ESceneTextureSetupMode::GBuffers))
	{
		OutSetupMode |= EMobileSceneTextureSetupMode::SceneColor;
	}
	if (EnumHasAnyFlags(InSetupMode, ESceneTextureSetupMode::CustomDepth))
	{
		OutSetupMode |= EMobileSceneTextureSetupMode::CustomDepth;
	}
	return OutSetupMode;
}

void SetupMobileSceneTextureUniformParameters(
	FRDGBuilder& GraphBuilder,
	EMobileSceneTextureSetupMode SetupMode,
	FMobileSceneTextureUniformParameters& SceneTextureParameters)
{
	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);

	SceneTextureParameters.SceneColorTexture = SystemTextures.Black;
	SceneTextureParameters.SceneColorTextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.SceneDepthTexture = SystemTextures.DepthDummy;
	SceneTextureParameters.SceneDepthTextureSampler = TStaticSamplerState<>::GetRHI();
	// CustomDepthTexture is a color texture on mobile, with DeviceZ values
	SceneTextureParameters.CustomDepthTexture = SystemTextures.Black;
	SceneTextureParameters.CustomDepthTextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.MobileCustomStencilTexture = SystemTextures.Black;
	SceneTextureParameters.MobileCustomStencilTextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.SceneVelocityTexture = SystemTextures.Black;
	SceneTextureParameters.SceneVelocityTextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.GBufferATexture = SystemTextures.Black;
	SceneTextureParameters.GBufferBTexture = SystemTextures.Black;
	SceneTextureParameters.GBufferCTexture = SystemTextures.Black;
	SceneTextureParameters.GBufferDTexture = SystemTextures.Black;
	// SceneDepthAuxTexture is a color texture on mobile, with DeviceZ values
	SceneTextureParameters.SceneDepthAuxTexture = SystemTextures.Black;
	SceneTextureParameters.GBufferATextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.GBufferBTextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.GBufferCTextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.GBufferDTextureSampler = TStaticSamplerState<>::GetRHI();
	SceneTextureParameters.SceneDepthAuxTextureSampler = TStaticSamplerState<>::GetRHI();

	if (const FSceneTextures* SceneTextures = GraphBuilder.Blackboard.Get<FSceneTextures>())
	{
		if (EnumHasAnyFlags(SetupMode, EMobileSceneTextureSetupMode::SceneColor) && HasBeenProduced(SceneTextures->Color.Resolve))
		{
			SceneTextureParameters.SceneColorTexture = SceneTextures->Color.Resolve;
		}

		if (EnumHasAnyFlags(SetupMode, EMobileSceneTextureSetupMode::SceneDepth) && 
			HasBeenProduced(SceneTextures->Depth.Resolve) && 
			!EnumHasAnyFlags(SceneTextures->Depth.Resolve->Desc.Flags, TexCreate_Memoryless))
		{
			SceneTextureParameters.SceneDepthTexture = SceneTextures->Depth.Resolve;
		}

		if (SceneTextures->Config.bIsUsingGBuffers)
		{
			if (HasBeenProduced(SceneTextures->GBufferA))
			{
				SceneTextureParameters.GBufferATexture = SceneTextures->GBufferA;
			}

			if (HasBeenProduced(SceneTextures->GBufferB))
			{
				SceneTextureParameters.GBufferBTexture = SceneTextures->GBufferB;
			}

			if (HasBeenProduced(SceneTextures->GBufferC))
			{
				SceneTextureParameters.GBufferCTexture = SceneTextures->GBufferC;
			}

			if (HasBeenProduced(SceneTextures->GBufferD))
			{
				SceneTextureParameters.GBufferDTexture = SceneTextures->GBufferD;
			}
		}

		if (EnumHasAnyFlags(SetupMode, EMobileSceneTextureSetupMode::SceneDepthAux))
		{
			if (HasBeenProduced(SceneTextures->DepthAux.Resolve))
			{
				SceneTextureParameters.SceneDepthAuxTexture = SceneTextures->DepthAux.Resolve;
			}
		}

		if (EnumHasAnyFlags(SetupMode, EMobileSceneTextureSetupMode::CustomDepth))
		{
			const FCustomDepthTextures& CustomDepthTextures = SceneTextures->CustomDepth;

			if (HasBeenProduced(CustomDepthTextures.MobileDepth))
			{
				SceneTextureParameters.CustomDepthTexture = CustomDepthTextures.MobileDepth;
			}

			if (HasBeenProduced(CustomDepthTextures.MobileStencil) && !EnumHasAnyFlags(CustomDepthTextures.MobileStencil->Desc.Flags, TexCreate_Memoryless))
			{
				SceneTextureParameters.MobileCustomStencilTexture = CustomDepthTextures.MobileStencil;
			}
		}

		if (EnumHasAnyFlags(SetupMode, EMobileSceneTextureSetupMode::SceneVelocity))
		{
			if (HasBeenProduced(SceneTextures->Velocity))
			{
				SceneTextureParameters.SceneVelocityTexture = SceneTextures->Velocity;
			}
		}
	}
}

TRDGUniformBufferRef<FMobileSceneTextureUniformParameters> CreateMobileSceneTextureUniformBuffer(
	FRDGBuilder& GraphBuilder,
	EMobileSceneTextureSetupMode SetupMode)
{
	FMobileSceneTextureUniformParameters* SceneTextures = GraphBuilder.AllocParameters<FMobileSceneTextureUniformParameters>();
	SetupMobileSceneTextureUniformParameters(GraphBuilder, SetupMode, *SceneTextures);
	return GraphBuilder.CreateUniformBuffer(SceneTextures);
}

FSceneTextureShaderParameters CreateSceneTextureShaderParameters(
	FRDGBuilder& GraphBuilder,
	ERHIFeatureLevel::Type FeatureLevel,
	ESceneTextureSetupMode SetupMode)
{
	FSceneTextureShaderParameters Parameters;
	if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Deferred)
	{
		Parameters.SceneTextures = CreateSceneTextureUniformBuffer(GraphBuilder, FeatureLevel, SetupMode);
	}
	else if (FSceneInterface::GetShadingPath(FeatureLevel) == EShadingPath::Mobile)
	{
		Parameters.MobileSceneTextures = CreateMobileSceneTextureUniformBuffer(GraphBuilder, Translate(SetupMode));
	}
	return Parameters;
}

bool IsSceneTexturesValid()
{
	return FSceneTexturesConfig::Get().ShadingPath != EShadingPath::Num;
}

FIntPoint GetSceneTextureExtent()
{
	return FSceneTexturesConfig::Get().Extent;
}

ERHIFeatureLevel::Type GetSceneTextureFeatureLevel()
{
	return FSceneTexturesConfig::Get().FeatureLevel;
}

void CreateSystemTextures(FRDGBuilder& GraphBuilder)
{
	FRDGSystemTextures::Create(GraphBuilder);
}