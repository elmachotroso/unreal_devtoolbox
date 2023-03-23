// Copyright Epic Games, Inc. All Rights Reserved.

#include "HairStrandsVisibility.h"
#include "HairStrandsCluster.h"
#include "HairStrandsUtils.h"
#include "HairStrandsInterface.h"
#include "HairStrandsTile.h"

#include "Shader.h"
#include "GlobalShader.h"
#include "ShaderParameters.h"
#include "ShaderParameterStruct.h"
#include "SceneTextureParameters.h"
#include "RenderGraphUtils.h"
#include "PostProcessing.h"
#include "MeshPassProcessor.inl"
#include "ScenePrivate.h"
#include "SceneTextureReductions.h"
#include "PixelShaderUtils.h"

DECLARE_GPU_STAT(HairStrandsVisibility);

/////////////////////////////////////////////////////////////////////////////////////////

static float GHairStrandsMaterialCompactionDepthThreshold = 1.f;
static float GHairStrandsMaterialCompactionTangentThreshold = 10.f;
static FAutoConsoleVariableRef CVarHairStrandsMaterialCompactionDepthThreshold(TEXT("r.HairStrands.MaterialCompaction.DepthThreshold"), GHairStrandsMaterialCompactionDepthThreshold, TEXT("Compaction threshold for depth value for material compaction (in centimeters). Default 1 cm."));
static FAutoConsoleVariableRef CVarHairStrandsMaterialCompactionTangentThreshold(TEXT("r.HairStrands.MaterialCompaction.TangentThreshold"), GHairStrandsMaterialCompactionTangentThreshold, TEXT("Compaciton threshold for tangent value for material compaction (in degrees). Default 10 deg."));

static int32 GHairVisibilityMSAA_MaxSamplePerPixel = 8;
static float GHairVisibilityMSAA_MeanSamplePerPixel = 0.75f;
static FAutoConsoleVariableRef CVarHairVisibilityMSAA_MaxSamplePerPixel(TEXT("r.HairStrands.Visibility.MSAA.SamplePerPixel"), GHairVisibilityMSAA_MaxSamplePerPixel, TEXT("Hair strands visibility sample count (2, 4, or 8)"), ECVF_Scalability | ECVF_RenderThreadSafe);
static FAutoConsoleVariableRef CVarHairVisibilityMSAA_MeanSamplePerPixel(TEXT("r.HairStrands.Visibility.MSAA.MeanSamplePerPixel"), GHairVisibilityMSAA_MeanSamplePerPixel, TEXT("Scale the numer of sampler per pixel for limiting memory allocation (0..1, default 0.5f)"));

static int32 GHairClearVisibilityBuffer = 0;
static FAutoConsoleVariableRef CVarHairClearVisibilityBuffer(TEXT("r.HairStrands.Visibility.Clear"), GHairClearVisibilityBuffer, TEXT("Clear hair strands visibility buffer"));

static TAutoConsoleVariable<int32> CVarHairVelocityMagnitudeScale(
	TEXT("r.HairStrands.VelocityMagnitudeScale"),
	100,  // Tuned by eye, based on heavy motion (strong head shack)
	TEXT("Velocity magnitude (in pixel) at which a hair will reach its pic velocity-rasterization-scale under motion to reduce aliasing. Default is 100."));

static int32 GHairVelocityType = 1; // default is 
static FAutoConsoleVariableRef CVarHairVelocityType(TEXT("r.HairStrands.VelocityType"), GHairVelocityType, TEXT("Type of velocity filtering (0:avg, 1:closest, 2:max). Default is 1."));

static int32 GHairVisibilityPPLL = 0;
static int32 GHairVisibilityPPLL_MaxSamplePerPixel = 16;
static float GHairVisibilityPPLL_MeanSamplePerPixel = 1;
static FAutoConsoleVariableRef CVarGHairVisibilityPPLL(TEXT("r.HairStrands.Visibility.PPLL"), GHairVisibilityPPLL, TEXT("Hair Visibility uses per pixel linked list"), ECVF_Scalability | ECVF_RenderThreadSafe);
static FAutoConsoleVariableRef CVarGHairVisibilityPPLL_MeanNodeCountPerPixel(TEXT("r.HairStrands.Visibility.PPLL.SamplePerPixel"), GHairVisibilityPPLL_MaxSamplePerPixel, TEXT("The maximum number of node allowed to be independently shaded and composited per pixel. Total amount of node will be width*height*VisibilityPPLLMaxRenderNodePerPixel. The last node is used to aggregate all furthest strands to shade into a single one."));
static FAutoConsoleVariableRef CVarGHairVisibilityPPLL_MeanSamplePerPixel(TEXT("r.HairStrands.Visibility.PPLL.MeanSamplePerPixel"), GHairVisibilityPPLL_MeanSamplePerPixel, TEXT("Scale the maximum number of node allowed for all linked list element (0..1, default 1). It will be width*height*SamplerPerPixel*Scale."));

static float GHairStrandsViewHairCountDepthDistanceThreshold = 30.f;
static FAutoConsoleVariableRef CVarHairStrandsViewHairCountDepthDistanceThreshold(TEXT("r.HairStrands.Visibility.HairCount.DistanceThreshold"), GHairStrandsViewHairCountDepthDistanceThreshold, TEXT("Distance threshold defining if opaque depth get injected into the 'view-hair-count' buffer."));

static int32 GHairVisibilityComputeRaster = 0;
static int32 GHairVisibilityComputeRaster_MaxSamplePerPixel = 1;
static float GHairVisibilityComputeRaster_MeanSamplePerPixel = 1;
static int32 GHairVisibilityComputeRaster_MaxPixelCount = 64;
static int32 GHairVisibilityComputeRaster_Stochastic = 0;
static FAutoConsoleVariableRef CVarHairStrandsVisibilityComputeRaster(TEXT("r.HairStrands.Visibility.ComputeRaster"), GHairVisibilityComputeRaster, TEXT("Hair Visiblity uses raster compute."), ECVF_Scalability | ECVF_RenderThreadSafe);
static FAutoConsoleVariableRef CVarHairStrandsVisibilityComputeRaster_MaxSamplePerPixel(TEXT("r.HairStrands.Visibility.ComputeRaster.SamplePerPixel"), GHairVisibilityComputeRaster_MaxSamplePerPixel, TEXT("Define the number of sampler per pixel using raster compute."));
static FAutoConsoleVariableRef CVarHairStrandsVisibilityComputeRaster_MaxPixelCount(TEXT("r.HairStrands.Visibility.ComputeRaster.MaxPixelCount"), GHairVisibilityComputeRaster_MaxPixelCount, TEXT("Define the maximal length rasterize in compute."));
static FAutoConsoleVariableRef CVarHairVisibilityComputeRaster_Stochastic(TEXT("r.HairStrands.Visibility.ComputeRaster.Stochastic"), GHairVisibilityComputeRaster_Stochastic, TEXT("Enable stochastic compute rasterization (faster, but more prone to aliasting). Experimental."));

static float GHairStrandsFullCoverageThreshold = 0.98f;
static FAutoConsoleVariableRef CVarHairStrandsFullCoverageThreshold(TEXT("r.HairStrands.Visibility.FullCoverageThreshold"), GHairStrandsFullCoverageThreshold, TEXT("Define the coverage threshold at which a pixel is considered fully covered."));

static float GHairStrandsWriteVelocityCoverageThreshold = 0.f;
static FAutoConsoleVariableRef CVarHairStrandsWriteVelocityCoverageThreshold(TEXT("r.HairStrands.Visibility.WriteVelocityCoverageThreshold"), GHairStrandsWriteVelocityCoverageThreshold, TEXT("Define the coverage threshold at which a pixel write its hair velocity (default: 0, i.e., write for all pixel)"));

static int32 GHairStrandsSortHairSampleByDepth = 0;
static FAutoConsoleVariableRef CVarHairStrandsSortHairSampleByDepth(TEXT("r.HairStrands.Visibility.SortByDepth"), GHairStrandsSortHairSampleByDepth, TEXT("Sort hair fragment by depth and update their coverage based on ordered transmittance."));

static int32 GHairStrandsHairCountToTransmittance = 0;
static FAutoConsoleVariableRef CVarHairStrandsHairCountToTransmittance(TEXT("r.HairStrands.Visibility.UseCoverageMappping"), GHairStrandsHairCountToTransmittance, TEXT("Use hair count to coverage transfer function."));

static int32 GHairStrandsDebugPPLL = 0;
static FAutoConsoleVariableRef CVarHairStrandsDebugPPLL(TEXT("r.HairStrands.Visibility.PPLL.Debug"), GHairStrandsDebugPPLL, TEXT("Draw debug per pixel light list rendering."));

static int32 GHairStrandsTile = 1;
static FAutoConsoleVariableRef CVarHairStrandsTile(TEXT("r.HairStrands.Tile"), GHairStrandsTile, TEXT("Enable tile generation & usage for hair strands."));

static int32 GHairStrandsLightSampleFormat = 1;
static FAutoConsoleVariableRef CVarHairStrandsLightSampleFormat(TEXT("r.HairStrands.LightSampleFormat"), GHairStrandsLightSampleFormat, TEXT("Define the format used for storing the lighting of hair samples (0: RGBA-16bits, 1: RGB-11.11.10bits)"));

static float GHairStrands_InvalidationPosition_Threshold = 0.05f;
static FAutoConsoleVariableRef CVarHairStrands_InvalidationPosition_Threshold(TEXT("r.HairStrands.PathTracing.InvalidationThreshold"), GHairStrands_InvalidationPosition_Threshold, TEXT("Define the minimal distance to invalidate path tracer output when groom changes (in cm, default: 0.5mm)\nSet to a negative value to disable this feature"));

static int32 GHairStrands_InvalidationPosition_Debug = 0;
static FAutoConsoleVariableRef CVarHairStrands_InvalidationPosition_Debug(TEXT("r.HairStrands.PathTracing.InvalidationDebug"), GHairStrands_InvalidationPosition_Debug, TEXT("Enable bounding box drawing for groom element causing path tracer invalidation"));

static float GHairStrands_Selection_CoverageThreshold = 0.0f;
static FAutoConsoleVariableRef CVarHairStrands_Selection_CoverageThreshold(TEXT("r.HairStrands.Selection.CoverageThreshold"), GHairStrands_Selection_CoverageThreshold, TEXT("Coverage threshold for making hair strands outline selection finer"));

/////////////////////////////////////////////////////////////////////////////////////////

namespace HairStrandsVisibilityInternal
{
	struct NodeData
	{
		uint32 Depth;
		uint32 PrimitiveId_MacroGroupId;
		uint32 Tangent_Coverage;
		uint32 BaseColor_Roughness;
		uint32 Specular;
	};

	// 64 bit alignment
	struct NodeVis
	{
		uint32 Depth_Coverage;
		uint32 PrimitiveId_MacroGroupId;
	};
}

enum EHairVisibilityRenderMode
{
	HairVisibilityRenderMode_Transmittance,
	HairVisibilityRenderMode_PPLL,
	HairVisibilityRenderMode_MSAA_Visibility,
	HairVisibilityRenderMode_TransmittanceAndHairCount,
	HairVisibilityRenderMode_ComputeRaster,
	HairVisibilityRenderModeCount
};

inline bool DoesSupportRasterCompute()
{
	return GRHISupportsAtomicUInt64;
}

inline EHairVisibilityRenderMode GetHairVisibilityRenderMode()
{
	if (GHairVisibilityPPLL > 0)
	{
		return HairVisibilityRenderMode_PPLL;
	}
	else if (GHairVisibilityComputeRaster > 0 && DoesSupportRasterCompute())
	{
		return HairVisibilityRenderMode_ComputeRaster;
	}
	else
	{
		return HairVisibilityRenderMode_MSAA_Visibility;
	}
}

inline bool IsMsaaEnabled()
{
	const EHairVisibilityRenderMode Mode = GetHairVisibilityRenderMode();
	return Mode == HairVisibilityRenderMode_MSAA_Visibility;
}

static uint32 GetMaxSamplePerPixel()
{
	switch (GetHairVisibilityRenderMode())
	{
		case HairVisibilityRenderMode_ComputeRaster:
		{
			if (GHairVisibilityComputeRaster_MaxSamplePerPixel <= 1)
			{
				return 1;
			}
			else if (GHairVisibilityComputeRaster_MaxSamplePerPixel < 4)
			{
				return 2;
			}
			else
			{
				return 4;
			}
		}
		case HairVisibilityRenderMode_MSAA_Visibility:
		{
			if (GHairVisibilityMSAA_MaxSamplePerPixel <= 1)
			{
				return 1;
			}
			else if (GHairVisibilityMSAA_MaxSamplePerPixel == 2)
			{
				return 2;
			}
			else if (GHairVisibilityMSAA_MaxSamplePerPixel <= 4)
			{
				return 4;
			}
			else
			{
				return 8;
			}
		}
		case HairVisibilityRenderMode_PPLL:
		{
			// The following must match the FPPLL permutation of FHairVisibilityPrimitiveIdCompactionCS.
			if (GHairVisibilityPPLL_MaxSamplePerPixel == 0)
			{
				return 0;
			}
			else if (GHairVisibilityPPLL_MaxSamplePerPixel <= 8)
			{
				return 8;
			}
			else if (GHairVisibilityPPLL_MaxSamplePerPixel <= 16)
			{
				return 16;
			}
			else //if (GHairVisibilityPPLL_MaxSamplePerPixel <= 32)
			{
				return 32;
			}
			// If more is needed: please check out EncodeNodeDesc from HairStrandsVisibilityCommon.ush to verify node count representation limitations.
		}
	}
	return 1;
}

inline uint32 GetMeanSamplePerPixel()
{
	const uint32 SamplePerPixel = GetMaxSamplePerPixel();
	switch (GetHairVisibilityRenderMode())
	{
	case HairVisibilityRenderMode_ComputeRaster:
		return FMath::Max(1, FMath::FloorToInt(SamplePerPixel * FMath::Clamp(GHairVisibilityComputeRaster_MeanSamplePerPixel, 0.f, 1.f)));
	case HairVisibilityRenderMode_MSAA_Visibility:
		return FMath::Max(1, FMath::FloorToInt(SamplePerPixel * FMath::Clamp(GHairVisibilityMSAA_MeanSamplePerPixel, 0.f, 1.f)));
	case HairVisibilityRenderMode_PPLL:
		return FMath::Max(1, FMath::FloorToInt(SamplePerPixel * FMath::Clamp(GHairVisibilityPPLL_MeanSamplePerPixel, 0.f, 10.f)));
	case HairVisibilityRenderMode_Transmittance:
	case HairVisibilityRenderMode_TransmittanceAndHairCount:
		return 1;
	}
	return 1;
}

uint32 GetHairStrandsMeanSamplePerPixel()
{
	return GetMeanSamplePerPixel();
}

struct FRasterComputeOutput
{
	FIntPoint BaseResolution;
	FIntPoint SuperResolution;
	uint32 ResolutionMultiplier = 1;

	FRDGTextureRef HairCountTexture = nullptr;
	FRDGTextureRef DepthTexture = nullptr;

	FRDGTextureRef VisibilityTexture0 = nullptr;
	FRDGTextureRef VisibilityTexture1 = nullptr;
	FRDGTextureRef VisibilityTexture2 = nullptr;
	FRDGTextureRef VisibilityTexture3 = nullptr;
};

static uint32 GetTotalSampleCountForAllocation(FIntPoint Resolution)
{
	return Resolution.X * Resolution.Y * GetMeanSamplePerPixel();
}

static void SetUpViewHairRenderInfo(const FViewInfo& ViewInfo, bool bEnableMSAA, FVector4f& OutHairRenderInfo, uint32& OutHairRenderInfoBits, uint32& OutHairComponents)
{
	FVector2f PixelVelocity(1.f / (ViewInfo.ViewRect.Width() * 2), 1.f / (ViewInfo.ViewRect.Height() * 2));
	const float VelocityMagnitudeScale = FMath::Clamp(CVarHairVelocityMagnitudeScale.GetValueOnAnyThread(), 0, 512) * FMath::Min(PixelVelocity.X, PixelVelocity.Y);

	// In the case we render coverage, we need to override some view uniform shader parameters to account for the change in MSAA sample count.
	const uint32 HairVisibilitySampleCount = bEnableMSAA ? GetMaxSamplePerPixel() : 1;	// The coverage pass does not use MSAA
	const float RasterizationScaleOverride = 0.0f;	// no override
	FMinHairRadiusAtDepth1 MinHairRadius = ComputeMinStrandRadiusAtDepth1(
		FIntPoint(ViewInfo.UnconstrainedViewRect.Width(), ViewInfo.UnconstrainedViewRect.Height()), ViewInfo.FOV, HairVisibilitySampleCount, RasterizationScaleOverride);

	OutHairRenderInfo = PackHairRenderInfo(MinHairRadius.Primary, MinHairRadius.Stable, MinHairRadius.Velocity, VelocityMagnitudeScale);
	OutHairRenderInfoBits = PackHairRenderInfoBits(!ViewInfo.IsPerspectiveProjection(), false);
	OutHairComponents = ToBitfield(GetHairComponents());
}

void SetUpViewHairRenderInfo(const FViewInfo& ViewInfo, FVector4f& OutHairRenderInfo, uint32& OutHairRenderInfoBits, uint32& OutHairComponents)
{
	SetUpViewHairRenderInfo(ViewInfo, IsMsaaEnabled(), OutHairRenderInfo, OutHairRenderInfoBits, OutHairComponents);
}

static bool IsCompatibleWithHairVisibility(const FMeshMaterialShaderPermutationParameters& Parameters)
{
	return IsCompatibleWithHairStrands(Parameters.Platform, Parameters.MaterialParameters);
}

float GetHairWriteVelocityCoverageThreshold()
{
	return FMath::Clamp(GHairStrandsWriteVelocityCoverageThreshold, 0.f, 1.f);
}

float GetHairStrandsFullCoverageThreshold()
{
	return FMath::Clamp(GHairStrandsFullCoverageThreshold, 0.1f, 1.f);
}
///////////////////////////////////////////////////////////////////////////////////////////////////

class FHairLightSampleClearVS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairLightSampleClearVS);
	SHADER_USE_PARAMETER_STRUCT(FHairLightSampleClearVS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, MaxViewportResolution)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairNodeCountTexture)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_VERTEX"), 1);
	}
};

class FHairLightSampleClearPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairLightSampleClearPS);
	SHADER_USE_PARAMETER_STRUCT(FHairLightSampleClearPS, FGlobalShader)

	class FOutputFormat : SHADER_PERMUTATION_INT("PERMUTATION_OUTPUT_FORMAT", 2);
	using FPermutationDomain = TShaderPermutationDomain<FOutputFormat>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, MaxViewportResolution)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairNodeCountTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static EPixelFormat GetHairLightSampleFormat()
	{
		EPixelFormat Format = PF_FloatRGBA;
		if (GHairStrandsLightSampleFormat > 0 && GPixelFormats[PF_FloatR11G11B10].Supported)
		{
			Format = PF_FloatR11G11B10;
		}
		return Format;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_CLEAR"), 1);

		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FOutputFormat>() == 0)
		{
			OutEnvironment.SetRenderTargetOutputFormat(0, PF_FloatRGBA);
		}
		else if (PermutationVector.Get<FOutputFormat>() == 1)
		{
			OutEnvironment.SetRenderTargetOutputFormat(0, PF_FloatR11G11B10);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairLightSampleClearVS, "/Engine/Private/HairStrands/HairStrandsLightSample.usf", "MainVS", SF_Vertex);
IMPLEMENT_GLOBAL_SHADER(FHairLightSampleClearPS, "/Engine/Private/HairStrands/HairStrandsLightSample.usf", "ClearPS", SF_Pixel);

static FRDGTextureRef AddClearLightSamplePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo* View,
	const uint32 MaxNodeCount,
	const FRDGTextureRef NodeCounter)
{	
	const EPixelFormat Format = FHairLightSampleClearPS::GetHairLightSampleFormat();

	const uint32 SampleTextureResolution = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(MaxNodeCount)));
	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(SampleTextureResolution, SampleTextureResolution), Format, FClearValueBinding::Black, TexCreate_UAV | TexCreate_ShaderResource | TexCreate_RenderTargetable);
	FRDGTextureRef Output = GraphBuilder.CreateTexture(Desc, TEXT("Hair.LightSample"));

	FHairLightSampleClearPS::FParameters* ParametersPS = GraphBuilder.AllocParameters<FHairLightSampleClearPS::FParameters>();
	ParametersPS->MaxViewportResolution = Desc.Extent;
	ParametersPS->HairNodeCountTexture = NodeCounter;
	
	FHairLightSampleClearPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairLightSampleClearPS::FOutputFormat>(Format == PF_FloatR11G11B10 ? 1 : 0);

	const FIntPoint ViewportResolution = Desc.Extent;
	TShaderMapRef<FHairLightSampleClearVS> VertexShader(View->ShaderMap);
	TShaderMapRef<FHairLightSampleClearPS> PixelShader(View->ShaderMap, PermutationVector);

	ParametersPS->RenderTargets[0] = FRenderTargetBinding(Output, ERenderTargetLoadAction::ENoAction);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrands::LightSampleClearPS"),
		ParametersPS,
		ERDGPassFlags::Raster,
		[ParametersPS, VertexShader, PixelShader, ViewportResolution](FRHICommandList& RHICmdList)
	{
		FHairLightSampleClearVS::FParameters ParametersVS;
		ParametersVS.MaxViewportResolution = ParametersPS->MaxViewportResolution;
		ParametersVS.HairNodeCountTexture = ParametersPS->HairNodeCountTexture;

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

		SetShaderParameters(RHICmdList, VertexShader, VertexShader.GetVertexShader(), ParametersVS);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *ParametersPS);

		RHICmdList.SetViewport(0, 0, 0.0f, ViewportResolution.X, ViewportResolution.Y, 1.0f);
		RHICmdList.SetStreamSource(0, nullptr, 0);
		RHICmdList.DrawPrimitive(0, 1, 1);
	});

	return Output;
}

/////////////////////////////////////////////////////////////////////////////////////////

class FHairMaterialVS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FHairMaterialVS, MeshMaterial);

protected:
	FHairMaterialVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		ERHIFeatureLevel::Type FeatureLevel = GetMaxSupportedFeatureLevel((EShaderPlatform)Initializer.Target.Platform);
		check(FSceneInterface::GetShadingPath(FeatureLevel) != EShadingPath::Mobile);
	}

	FHairMaterialVS() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		const bool bIsCompatible = IsCompatibleWithHairVisibility(Parameters) && Parameters.VertexFactoryType->GetFName() == FName(TEXT("FHairStrandsVertexFactory"));
		return bIsCompatible;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};
IMPLEMENT_MATERIAL_SHADER_TYPE(, FHairMaterialVS, TEXT("/Engine/Private/HairStrands/HairStrandsMaterialVS.usf"), TEXT("Main"), SF_Vertex);

/////////////////////////////////////////////////////////////////////////////////////////

class FHairMaterialShaderElementData : public FMeshMaterialShaderElementData
{
public:
	FHairMaterialShaderElementData(int32 MacroGroupId, int32 MaterialId, int32 PrimitiveId, uint32 LightChannelMask) : MaterialPass_MacroGroupId(MacroGroupId), MaterialPass_MaterialId(MaterialId), MaterialPass_PrimitiveId(PrimitiveId), MaterialPass_LightChannelMask(LightChannelMask){ }
	uint32 MaterialPass_MacroGroupId;
	uint32 MaterialPass_MaterialId;
	uint32 MaterialPass_PrimitiveId;
	uint32 MaterialPass_LightChannelMask;
};

#define HAIR_MATERIAL_DEBUG_OUTPUT 0
static bool IsPlatformRequiringRenderTargetForMaterialPass(EShaderPlatform Platform)
{
	return HAIR_MATERIAL_DEBUG_OUTPUT || FDataDrivenShaderPlatformInfo::GetRequiresRenderTargetDuringRaster(Platform); //#hair_todo: change to a proper RHI(Platform) function
}

class FHairMaterialPS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FHairMaterialPS, MeshMaterial);

public:
	FHairMaterialPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		ERHIFeatureLevel::Type FeatureLevel = GetMaxSupportedFeatureLevel((EShaderPlatform)Initializer.Target.Platform);
		check(FSceneInterface::GetShadingPath(FeatureLevel) != EShadingPath::Mobile);
		MaterialPass_MacroGroupId.Bind(Initializer.ParameterMap, TEXT("MaterialPass_MacroGroupId"));
		MaterialPass_MaterialId.Bind(Initializer.ParameterMap, TEXT("MaterialPass_MaterialId"));
		MaterialPass_PrimitiveId.Bind(Initializer.ParameterMap, TEXT("MaterialPass_PrimitiveId"));
		MaterialPass_LightChannelMask.Bind(Initializer.ParameterMap, TEXT("MaterialPass_LightChannelMask"));
	}

	FHairMaterialPS() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		const bool bIsCompatible = IsCompatibleWithHairStrands(Parameters.Platform, Parameters.MaterialParameters) && Parameters.VertexFactoryType->GetFName() == FName(TEXT("FHairStrandsVertexFactory"));
		return bIsCompatible;
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		const bool bPlatformRequireRenderTarget = IsPlatformRequiringRenderTargetForMaterialPass(Parameters.Platform);
		const bool bHasEmissiveConnected = Parameters.MaterialParameters.bHasEmissiveColorConnected;
		OutEnvironment.SetDefine(TEXT("HAIR_MATERIAL_EMISSIVE_OUTPUT"), (bHasEmissiveConnected || bPlatformRequireRenderTarget) ? 1 : 0);
		OutEnvironment.SetDefine(TEXT("HAIRSTRANDS_HAS_NORMAL_CONNECTED"), Parameters.MaterialParameters.bHasNormalConnected ? 1 : 0);

		const EPixelFormat Format = FHairLightSampleClearPS::GetHairLightSampleFormat();
		OutEnvironment.SetRenderTargetOutputFormat(0, Format);
	}

	void GetShaderBindings(
		const FScene* Scene,
		ERHIFeatureLevel::Type FeatureLevel,
		const FPrimitiveSceneProxy* PrimitiveSceneProxy,
		const FMaterialRenderProxy& MaterialRenderProxy,
		const FMaterial& Material,
		const FMeshPassProcessorRenderState& DrawRenderState,
		const FHairMaterialShaderElementData& ShaderElementData,
		FMeshDrawSingleShaderBindings& ShaderBindings) const
	{
		FMeshMaterialShader::GetShaderBindings(Scene, FeatureLevel, PrimitiveSceneProxy, MaterialRenderProxy, Material, DrawRenderState, ShaderElementData, ShaderBindings);
		ShaderBindings.Add(MaterialPass_MacroGroupId, ShaderElementData.MaterialPass_MacroGroupId);
		ShaderBindings.Add(MaterialPass_MaterialId, ShaderElementData.MaterialPass_MaterialId);
		ShaderBindings.Add(MaterialPass_PrimitiveId, ShaderElementData.MaterialPass_PrimitiveId);
		ShaderBindings.Add(MaterialPass_LightChannelMask, ShaderElementData.MaterialPass_LightChannelMask);
	}

private:
	LAYOUT_FIELD(FShaderParameter, MaterialPass_MacroGroupId);
	LAYOUT_FIELD(FShaderParameter, MaterialPass_MaterialId);
	LAYOUT_FIELD(FShaderParameter, MaterialPass_PrimitiveId);
	LAYOUT_FIELD(FShaderParameter, MaterialPass_LightChannelMask);
};
IMPLEMENT_MATERIAL_SHADER_TYPE(, FHairMaterialPS, TEXT("/Engine/Private/HairStrands/HairStrandsMaterialPS.usf"), TEXT("Main"), SF_Pixel);

/////////////////////////////////////////////////////////////////////////////////////////

class FHairMaterialProcessor : public FMeshPassProcessor
{
public:
	FHairMaterialProcessor(
		const FScene* Scene,
		const FSceneView* InViewIfDynamicMeshCommand,
		const FMeshPassProcessorRenderState& InPassDrawRenderState,
		FDynamicPassMeshDrawListContext* InDrawListContext);

	virtual void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId = -1) override final;
	void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId, int32 MacroGroupId, int32 HairMaterialId);

private:
	bool TryAddMeshBatch(
		const FMeshBatch& RESTRICT MeshBatch,
		uint64 BatchElementMask,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		int32 StaticMeshId,
		uint32 MacroGroupId,
		uint32 HairMaterialId,
		const FMaterialRenderProxy& MaterialRenderProxy,
		const FMaterial& Material);

	bool Process(
		const FMeshBatch& MeshBatch,
		uint64 BatchElementMask,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		int32 StaticMeshId,
		const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
		const FMaterial& RESTRICT MaterialResource,
		const int32 MacroGroupId,
		const int32 HairMaterialId,
		const int32 HairPrimitiveId,
		const uint32 HairPrimitiveLightChannelMask);

	FMeshPassProcessorRenderState PassDrawRenderState;
};

void FHairMaterialProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	AddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, 0, 0);
}

void FHairMaterialProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId, int32 MacroGroupId, int32 HairMaterialId)
{
	const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
	while (MaterialRenderProxy)
	{
		const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
		if (Material)
		{
			if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MacroGroupId, HairMaterialId, *MaterialRenderProxy, *Material))
			{
				break;
			}
		}

		MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
	}
}

bool FHairMaterialProcessor::TryAddMeshBatch(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	uint32 MacroGroupId,
	uint32 HairMaterialId,
	const FMaterialRenderProxy& MaterialRenderProxy,
	const FMaterial& Material)
{
	static const FVertexFactoryType* CompatibleVF = FVertexFactoryType::GetVFByName(TEXT("FHairStrandsVertexFactory"));

	// Determine the mesh's material and blend mode.
	const bool bIsCompatible = IsCompatibleWithHairStrands(&Material, FeatureLevel);
	const bool bIsHairStrandsFactory = MeshBatch.VertexFactory->GetType()->GetHashedName() == CompatibleVF->GetHashedName();
	const bool bShouldRender = (!PrimitiveSceneProxy && MeshBatch.Elements.Num() > 0) || (PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderInMainPass());

	if (bIsCompatible
		&& bIsHairStrandsFactory
		&& bShouldRender
		&& ShouldIncludeDomainInMeshPass(Material.GetMaterialDomain()))
	{
		// For the mesh patch to be rendered a single triangle triangle to spawn the necessary amount of thread
		FMeshBatch MeshBatchCopy = MeshBatch;
		for (uint32 ElementIt = 0, ElementCount = uint32(MeshBatch.Elements.Num()); ElementIt < ElementCount; ++ElementIt)
		{
			MeshBatchCopy.Elements[ElementIt].FirstIndex = 0;
			MeshBatchCopy.Elements[ElementIt].NumPrimitives = 1;
			MeshBatchCopy.Elements[ElementIt].NumInstances = 1;
			MeshBatchCopy.Elements[ElementIt].IndirectArgsBuffer = nullptr;
			MeshBatchCopy.Elements[ElementIt].IndirectArgsOffset = 0;
		}

		FPrimitiveSceneInfo* SceneInfo = PrimitiveSceneProxy ? PrimitiveSceneProxy->GetPrimitiveSceneInfo() : nullptr;
		FMeshDrawCommandPrimitiveIdInfo IdInfo = GetDrawCommandPrimitiveId(SceneInfo, MeshBatch.Elements[0]);
		uint32 LightChannelMask = PrimitiveSceneProxy ? PrimitiveSceneProxy->GetLightingChannelMask() : 0;

		return Process(MeshBatchCopy, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MaterialRenderProxy, Material, MacroGroupId, HairMaterialId, IdInfo.DrawPrimitiveId, LightChannelMask);
	}

	return true;
}

bool FHairMaterialProcessor::Process(
	const FMeshBatch& MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
	const FMaterial& RESTRICT MaterialResource,
	const int32 MacroGroupId,
	const int32 HairMaterialId,
	const int32 HairPrimitiveId,
	const uint32 HairPrimitiveLightChannelMask)
{
	const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;

	TMeshProcessorShaders<
		FHairMaterialVS,
		FHairMaterialPS> PassShaders;
	{
		FMaterialShaderTypes ShaderTypes;
		ShaderTypes.AddShaderType<FHairMaterialVS>();
		ShaderTypes.AddShaderType<FHairMaterialPS>();

		FVertexFactoryType* VertexFactoryType = VertexFactory->GetType();

		FMaterialShaders Shaders;
		if (!MaterialResource.TryGetShaders(ShaderTypes, VertexFactoryType, Shaders))
		{
			return false;
		}

		Shaders.TryGetVertexShader(PassShaders.VertexShader);
		Shaders.TryGetPixelShader(PassShaders.PixelShader);
	}

	FMeshPassProcessorRenderState DrawRenderState(PassDrawRenderState);
	FHairMaterialShaderElementData ShaderElementData(MacroGroupId, HairMaterialId, HairPrimitiveId, HairPrimitiveLightChannelMask);
	ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

	BuildMeshDrawCommands(
		MeshBatch,
		BatchElementMask,
		PrimitiveSceneProxy,
		MaterialRenderProxy,
		MaterialResource,
		DrawRenderState,
		PassShaders,
		ERasterizerFillMode::FM_Solid,
		ERasterizerCullMode::CM_CCW,
		FMeshDrawCommandSortKey::Default,
		EMeshPassFeatures::Default,
		ShaderElementData);

	return true;
}

FHairMaterialProcessor::FHairMaterialProcessor(
	const FScene* Scene,
	const FSceneView* InViewIfDynamicMeshCommand,
	const FMeshPassProcessorRenderState& InPassDrawRenderState,
	FDynamicPassMeshDrawListContext* InDrawListContext)
	: FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext)
	, PassDrawRenderState(InPassDrawRenderState)
{
}

/////////////////////////////////////////////////////////////////////////////////////////

BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVisibilityMaterialPassUniformParameters, )
	SHADER_PARAMETER(FIntPoint, MaxResolution)
	SHADER_PARAMETER(uint32, MaxSampleCount)
	SHADER_PARAMETER(uint32, NodeGroupSize)
	SHADER_PARAMETER(uint32, bUpdateSampleCoverage)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, NodeIndex)
	SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, TotalNodeCounter)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint2>, NodeCoord)
	SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPackedHairVis>, NodeVis)
	SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, IndirectArgs)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FPackedHairSample>, OutNodeData)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float2>, OutNodeVelocity)
END_GLOBAL_SHADER_PARAMETER_STRUCT()
IMPLEMENT_STATIC_UNIFORM_BUFFER_STRUCT(FVisibilityMaterialPassUniformParameters, "MaterialPassParameters", SceneTextures);

BEGIN_SHADER_PARAMETER_STRUCT(FVisibilityMaterialPassParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVisibilityMaterialPassUniformParameters, UniformBuffer)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

///////////////////////////////////////////////////////////////////////////////////////////////////
// Patch sample coverage
class FUpdateSampleCoverageCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FUpdateSampleCoverageCS);
	SHADER_USE_PARAMETER_STRUCT(FUpdateSampleCoverageCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, Resolution)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, NodeIndexAndOffset)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPackedHairSample>,  InNodeDataBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FPackedHairSample>, OutNodeDataBuffer)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform); }
};

IMPLEMENT_GLOBAL_SHADER(FUpdateSampleCoverageCS, "/Engine/Private/HairStrands/HairStrandsVisibilityComputeSampleCoverage.usf", "MainCS", SF_Compute);

static FRDGBufferRef AddUpdateSampleCoveragePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo* View,
	const FRDGTextureRef NodeIndexAndOffset,
	const FRDGBufferRef InNodeDataBuffer)
{
	FRDGBufferRef OutNodeDataBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(InNodeDataBuffer->Desc.BytesPerElement, InNodeDataBuffer->Desc.NumElements), TEXT("Hair.CompactNodeData"));

	FUpdateSampleCoverageCS::FParameters* Parameters = GraphBuilder.AllocParameters<FUpdateSampleCoverageCS::FParameters>();
	Parameters->Resolution = NodeIndexAndOffset->Desc.Extent;
	Parameters->NodeIndexAndOffset = NodeIndexAndOffset;
	Parameters->InNodeDataBuffer = GraphBuilder.CreateSRV(InNodeDataBuffer);
	Parameters->OutNodeDataBuffer = GraphBuilder.CreateUAV(OutNodeDataBuffer);

	TShaderMapRef<FUpdateSampleCoverageCS> ComputeShader(View->ShaderMap);

	// Add 64 threads permutation
	const uint32 GroupSizeX = 8;
	const uint32 GroupSizeY = 4;
	const FIntVector DispatchCount = FIntVector(
		(Parameters->Resolution.X + GroupSizeX-1) / GroupSizeX, 
		(Parameters->Resolution.Y + GroupSizeY-1) / GroupSizeY, 
		1);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("HairStrands::VisbilityUpdateCoverage"),
		ComputeShader,
		Parameters,
		DispatchCount);

	return OutNodeDataBuffer;
}
///////////////////////////////////////////////////////////////////////////////////////////////////
struct FMaterialPassOutput
{
	static const EPixelFormat VelocityFormat = PF_G16R16;
	FRDGBufferRef NodeData = nullptr;
	FRDGBufferRef NodeVelocity = nullptr;
	FRDGTextureRef SampleLightingTexture = nullptr;
};

static FMaterialPassOutput AddHairMaterialPass(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo* ViewInfo,
	const bool bUpdateSampleCoverage,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	FInstanceCullingManager& InstanceCullingManager,
	const uint32 NodeGroupSize,
	FRDGTextureRef CompactNodeIndex,
	FRDGBufferRef CompactNodeVis,
	FRDGBufferRef CompactNodeCoord,
	FRDGTextureRef CompactNodeCounter,
	FRDGBufferRef IndirectArgBuffer)
{
	if (!CompactNodeVis || !CompactNodeIndex)
		return FMaterialPassOutput();

	const uint32 MaxNodeCount = CompactNodeVis->Desc.NumElements;

	FMaterialPassOutput Output;
	Output.NodeData				 = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(HairStrandsVisibilityInternal::NodeData), MaxNodeCount), TEXT("Hair.CompactNodeData"));
	Output.NodeVelocity			 = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(4, CompactNodeVis->Desc.NumElements), TEXT("Hair.CompactNodeVelocity"));
	Output.SampleLightingTexture = AddClearLightSamplePass(GraphBuilder, ViewInfo, MaxNodeCount, CompactNodeCounter);

	const uint32 ResolutionDim = FMath::CeilToInt(FMath::Sqrt(static_cast<float>(MaxNodeCount)));
	const FIntPoint Resolution(ResolutionDim, ResolutionDim);

	enum class EHairMaterialPassFilter : uint8
	{
		All,
		EmissiveOnly,
		NonEmissiveOnly
	};

	const ERHIFeatureLevel::Type FeatureLevel = ViewInfo->FeatureLevel;

	// Find among the mesh batch, if any of them emit emissive data
	bool bHasEmissiveMaterial = false;
	for (const FHairStrandsMacroGroupData& MacroGroupData : MacroGroupDatas)
	{
		for (const FHairStrandsMacroGroupData::PrimitiveInfo& PrimitiveInfo : MacroGroupData.PrimitivesInfos)
		{
			if (const FMeshBatch* MeshBatch = PrimitiveInfo.Mesh)
			{
				if (MeshBatch->MaterialRenderProxy->GetIncompleteMaterialWithFallback(FeatureLevel).HasEmissiveColorConnected())
				{
					bHasEmissiveMaterial = true;
					break;
				}
			}
		}
	}

	// Generic material pass dispatch
	auto MaterialPass = [&](FRDGTextureRef RenderTarget, EHairMaterialPassFilter Filter)
	{
		// Add resources reference to the pass parameters, in order to get the resource lifetime extended to this pass
		FVisibilityMaterialPassParameters* PassParameters = GraphBuilder.AllocParameters<FVisibilityMaterialPassParameters>();

		{
			FVisibilityMaterialPassUniformParameters* UniformParameters = GraphBuilder.AllocParameters<FVisibilityMaterialPassUniformParameters>();

			UniformParameters->bUpdateSampleCoverage = bUpdateSampleCoverage ? 1 : 0;
			UniformParameters->MaxResolution = Resolution;
			UniformParameters->NodeGroupSize = NodeGroupSize;
			UniformParameters->MaxSampleCount = MaxNodeCount;
			UniformParameters->TotalNodeCounter = CompactNodeCounter;
			UniformParameters->NodeIndex = CompactNodeIndex;
			UniformParameters->NodeVis = GraphBuilder.CreateSRV(CompactNodeVis);
			UniformParameters->NodeCoord = GraphBuilder.CreateSRV(CompactNodeCoord, FHairStrandsVisibilityData::NodeCoordFormat);
			UniformParameters->IndirectArgs = GraphBuilder.CreateSRV(IndirectArgBuffer);
			UniformParameters->OutNodeData = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.NodeData));
			UniformParameters->OutNodeVelocity = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Output.NodeVelocity, FMaterialPassOutput::VelocityFormat));

			PassParameters->UniformBuffer = GraphBuilder.CreateUniformBuffer(UniformParameters);
		}

		{
			const bool bEnableMSAA = false;
			SetUpViewHairRenderInfo(*ViewInfo, bEnableMSAA, ViewInfo->CachedViewUniformShaderParameters->HairRenderInfo, ViewInfo->CachedViewUniformShaderParameters->HairRenderInfoBits, ViewInfo->CachedViewUniformShaderParameters->HairComponents);
			PassParameters->View = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(*ViewInfo->CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);
		}

		if (RenderTarget)
		{
			PassParameters->RenderTargets[0] = FRenderTargetBinding(RenderTarget, ERenderTargetLoadAction::EClear, 0);
		}

		AddSimpleMeshPass(
			GraphBuilder, 
			PassParameters, 
			Scene, 
			*ViewInfo, 
			&InstanceCullingManager, 
			RDG_EVENT_NAME("HairStrands::MaterialPass(Emissive=%s)", Filter == EHairMaterialPassFilter::All ? TEXT("On/Off") : (Filter == EHairMaterialPassFilter::EmissiveOnly ? TEXT("On") : TEXT("Off"))),
			FIntRect(0, 0, Resolution.X, Resolution.Y),
		[PassParameters, Scene = Scene, ViewInfo, &MacroGroupDatas, MaxNodeCount, Resolution, NodeGroupSize, bUpdateSampleCoverage, Filter, FeatureLevel](FDynamicPassMeshDrawListContext* ShadowContext)
		{
			FMeshPassProcessorRenderState DrawRenderState;
			if (Filter == EHairMaterialPassFilter::All || Filter == EHairMaterialPassFilter::EmissiveOnly)
			{
				DrawRenderState.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_One, BF_Zero>::GetRHI());
			}
			else
			{
				DrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());
			}
			DrawRenderState.SetDepthStencilState(TStaticDepthStencilState <false, CF_Always> ::GetRHI());
			FHairMaterialProcessor MeshProcessor(Scene, ViewInfo, DrawRenderState, ShadowContext);

			for (const FHairStrandsMacroGroupData& MacroGroupData : MacroGroupDatas)
			{
				for (const FHairStrandsMacroGroupData::PrimitiveInfo& PrimitiveInfo : MacroGroupData.PrimitivesInfos)
				{
					if (const FMeshBatch* MeshBatch = PrimitiveInfo.Mesh)
					{
						const uint64 BatchElementMask = ~0ull;
						bool bIsCompatible = true;
						if (Filter != EHairMaterialPassFilter::All)
						{
							const bool bHasEmissive = MeshBatch->MaterialRenderProxy->GetIncompleteMaterialWithFallback(FeatureLevel).HasEmissiveColorConnected();
							bIsCompatible = (bHasEmissive && Filter == EHairMaterialPassFilter::EmissiveOnly) || (!bHasEmissive && Filter == EHairMaterialPassFilter::NonEmissiveOnly);
						}

						if (bIsCompatible)
						{
							MeshProcessor.AddMeshBatch(*MeshBatch, BatchElementMask, PrimitiveInfo.PrimitiveSceneProxy, -1, MacroGroupData.MacroGroupId, PrimitiveInfo.MaterialId);
						}
					}
				}
			}
		});
	};


	const bool bIsPlatformRequireRenderTarget = IsPlatformRequiringRenderTargetForMaterialPass(Scene->GetShaderPlatform()) || GRHIRequiresRenderTargetForPixelShaderUAVs;

	// Output:
	// 1. Single pass: when the platform require an RT as output, render both emissive & non-emissive in a single pass
	// 2. Two passes : one pass for emissive material with an RT, one pass for regular/non-emissive material without an RT
	// 3. Single pass: when there is no emissive material, and platform does not require an RT
	if (bIsPlatformRequireRenderTarget)
	{
		MaterialPass(Output.SampleLightingTexture, EHairMaterialPassFilter::All);
	}
	else if (bHasEmissiveMaterial)
	{
		MaterialPass(Output.SampleLightingTexture, EHairMaterialPassFilter::EmissiveOnly);
		MaterialPass(nullptr, EHairMaterialPassFilter::NonEmissiveOnly);
	}
	else
	{
		MaterialPass(nullptr, EHairMaterialPassFilter::NonEmissiveOnly);
	}

	return Output;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairVelocityCS: public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairVelocityCS);
	SHADER_USE_PARAMETER_STRUCT(FHairVelocityCS, FGlobalShader);

	class FVelocity : SHADER_PERMUTATION_INT("PERMUTATION_VELOCITY", 4);
	class FOuputFormat : SHADER_PERMUTATION_INT("PERMUTATION_OUTPUT_FORMAT", 2);
	class FTile : SHADER_PERMUTATION_BOOL("PERMUTATION_TILE");
	using FPermutationDomain = TShaderPermutationDomain<FVelocity, FOuputFormat, FTile>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )

		SHADER_PARAMETER(FIntPoint, Resolution)
		SHADER_PARAMETER(FIntPoint, ResolutionOffset)
		SHADER_PARAMETER(float, VelocityThreshold)
		SHADER_PARAMETER(float, CoverageThreshold)
		SHADER_PARAMETER(uint32, bNeedClear)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CoverageTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NodeIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, NodeVelocity)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPackedHairVis>, NodeVis)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutVelocityTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutResolveMaskTexture)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)

		SHADER_PARAMETER(FIntPoint, TileCountXY)
		SHADER_PARAMETER(uint32, TileSize)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint2>, TileDataBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, HairTileCount)
		RDG_BUFFER_ACCESS(TileIndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairVelocityCS, "/Engine/Private/HairStrands/HairStrandsVelocity.usf", "MainCS", SF_Compute);

float GetHairFastResolveVelocityThreshold(const FIntPoint& Resolution);

static void AddHairVelocityPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	const FHairStrandsTiles& TileData,
	FRDGTextureRef& CoverageTexture,
	FRDGTextureRef& NodeIndex,
	FRDGBufferRef& NodeVis,
	FRDGBufferRef& NodeVelocity,
	FRDGTextureRef& OutVelocityTexture,
	FRDGTextureRef& OutResolveMaskTexture)
{
	const bool bWriteOutVelocity = OutVelocityTexture != nullptr;
	if (!bWriteOutVelocity)
		return;

	// If velocity texture has not been created by the base-pass, clear it here
	const bool bNeedClear = !HasBeenProduced(OutVelocityTexture);
	if (bNeedClear)
	{
		if (!TileData.IsValid())
		{
			AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutVelocityTexture), 0.f);
		}
		else
		{
			AddHairStrandsTileClearPass(GraphBuilder, View, TileData, FHairStrandsTiles::ETileType::Other, OutVelocityTexture);
		}
	}

	const bool bUseTile = TileData.IsValid();

	const FIntPoint Resolution = OutVelocityTexture->Desc.Extent;
	OutResolveMaskTexture = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Resolution, PF_R8_UINT, FClearValueBinding::None, TexCreate_UAV), TEXT("Hair.VelocityResolveMaskTexture"));

	check(OutVelocityTexture->Desc.Format == PF_G16R16 || OutVelocityTexture->Desc.Format == PF_A16B16G16R16);
	const bool bTwoChannelsOutput = OutVelocityTexture->Desc.Format == PF_G16R16;

	FHairVelocityCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairVelocityCS::FVelocity>(bWriteOutVelocity ? FMath::Clamp(GHairVelocityType + 1, 0, 3) : 0);
	PermutationVector.Set<FHairVelocityCS::FOuputFormat>(bTwoChannelsOutput ? 0 : 1);
	PermutationVector.Set<FHairVelocityCS::FTile>(bUseTile);

	FHairVelocityCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHairVelocityCS::FParameters>();
	PassParameters->bNeedClear = bNeedClear ? 1u : 0u;
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	PassParameters->VelocityThreshold = GetHairFastResolveVelocityThreshold(Resolution);
	PassParameters->CoverageThreshold = GetHairWriteVelocityCoverageThreshold();
	PassParameters->NodeIndex = NodeIndex;
	PassParameters->NodeVis = GraphBuilder.CreateSRV(NodeVis);
	PassParameters->NodeVelocity = GraphBuilder.CreateSRV(NodeVelocity, FMaterialPassOutput::VelocityFormat);
	PassParameters->CoverageTexture = CoverageTexture;
	PassParameters->OutVelocityTexture = GraphBuilder.CreateUAV(OutVelocityTexture);
	PassParameters->OutResolveMaskTexture = GraphBuilder.CreateUAV(OutResolveMaskTexture);

	if (bUseTile)
	{
		const FHairStrandsTiles::ETileType TileType = FHairStrandsTiles::ETileType::HairAll;

		PassParameters->ResolutionOffset	= FIntPoint(0,0);
		PassParameters->Resolution			= Resolution;
		PassParameters->TileCountXY			= TileData.TileCountXY;
		PassParameters->TileSize			= TileData.TileSize;
		PassParameters->TileCountBuffer		= TileData.TileCountSRV;
		PassParameters->TileDataBuffer		= TileData.GetTileBufferSRV(TileType);
		PassParameters->TileIndirectArgs	= TileData.TileIndirectDispatchBuffer;

		TShaderMapRef<FHairVelocityCS> ComputeShader(View.ShaderMap, PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrands::Velocity(Tile)"),
			ComputeShader,
			PassParameters,
			PassParameters->TileIndirectArgs, TileData.GetIndirectDispatchArgOffset(TileType));
	}
	else
	{
		// We don't use the CPU screen projection for running the velocity pass, as we need to clear the entire 
		// velocity mask through the UAV write, otherwise the mask will be partially invalid.
		const FIntRect TotalRect = View.ViewRect; 
		const FIntPoint RectResolution(TotalRect.Width(), TotalRect.Height());

		PassParameters->ResolutionOffset = FIntPoint(TotalRect.Min.X, TotalRect.Min.Y);
		PassParameters->Resolution		 = RectResolution;

		TShaderMapRef<FHairVelocityCS> ComputeShader(View.ShaderMap, PermutationVector);
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrands::Velocity(Screen)"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(RectResolution, FIntPoint(8, 8)));
	}
}

/////////////////////////////////////////////////////////////////////////////////////////
BEGIN_GLOBAL_SHADER_PARAMETER_STRUCT(FVisibilityPassUniformParameters, )
	SHADER_PARAMETER(uint32, MaxPPLLNodeCount)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, PPLLCounter)
	SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, PPLLNodeIndex)
	SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<FPPLLNodeData>, PPLLNodeData)
END_GLOBAL_SHADER_PARAMETER_STRUCT()
IMPLEMENT_STATIC_UNIFORM_BUFFER_STRUCT(FVisibilityPassUniformParameters, "HairVisibilityPass", SceneTextures);

BEGIN_SHADER_PARAMETER_STRUCT(FVisibilityPassParameters, )
	SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
	SHADER_PARAMETER_STRUCT_INCLUDE(FInstanceCullingDrawParams, InstanceCullingDrawParams)
	SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVisibilityPassUniformParameters, UniformBuffer)
	RENDER_TARGET_BINDING_SLOTS()
END_SHADER_PARAMETER_STRUCT()

// Example: 28bytes * 8spp = 224bytes per pixel = 442Mb @ 1080p
struct PPLLNodeData
{
	uint32 Depth;
	uint32 PrimitiveId_MacroGroupId;
	uint32 Tangent_Coverage;
	uint32 BaseColor_Roughness;
	uint32 Specular;
	uint32 NextNodeIndex;
	uint32 PackedVelocity;
};

TRDGUniformBufferRef<FVisibilityPassUniformParameters> CreatePassDummyTextures(FRDGBuilder& GraphBuilder)
{
	FVisibilityPassUniformParameters* UniformParameters = GraphBuilder.AllocParameters<FVisibilityPassUniformParameters>();

	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(1,1), PF_R32_UINT, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource);
	UniformParameters->PPLLCounter		= GraphBuilder.CreateUAV(GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityPPLLNodeCounter")));
	UniformParameters->PPLLNodeIndex	= GraphBuilder.CreateUAV(GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityPPLLNodeIndex")));
	UniformParameters->PPLLNodeData		= GraphBuilder.CreateUAV(GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(PPLLNodeData), 1), TEXT("Hair.DummyPPLLNodeData")));

	return GraphBuilder.CreateUniformBuffer(UniformParameters);
}

template<EHairVisibilityRenderMode RenderMode, bool bCullingEnable>
class FHairVisibilityVS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FHairVisibilityVS, MeshMaterial);

protected:

	FHairVisibilityVS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		: FMeshMaterialShader(Initializer)
	{
		ERHIFeatureLevel::Type FeatureLevel = GetMaxSupportedFeatureLevel((EShaderPlatform)Initializer.Target.Platform);
		check(FSceneInterface::GetShadingPath(FeatureLevel) != EShadingPath::Mobile);
	}

	FHairVisibilityVS() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		return IsCompatibleWithHairVisibility(Parameters) && Parameters.VertexFactoryType->GetFName() == FName(TEXT("FHairStrandsVertexFactory"));
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		const uint32 RenderModeValue = uint32(RenderMode);
		OutEnvironment.SetDefine(TEXT("HAIR_RENDER_MODE"), RenderModeValue);
		OutEnvironment.SetDefine(TEXT("USE_CULLED_CLUSTER"), bCullingEnable ? 1 : 0);
	}
};

typedef FHairVisibilityVS<HairVisibilityRenderMode_MSAA_Visibility, false >				THairVisiblityVS_MSAAVisibility_NoCulling;
typedef FHairVisibilityVS<HairVisibilityRenderMode_MSAA_Visibility, true >				THairVisiblityVS_MSAAVisibility_Culling;
typedef FHairVisibilityVS<HairVisibilityRenderMode_Transmittance, true >				THairVisiblityVS_Transmittance;
typedef FHairVisibilityVS<HairVisibilityRenderMode_TransmittanceAndHairCount, true >	THairVisiblityVS_TransmittanceAndHairCount;
typedef FHairVisibilityVS<HairVisibilityRenderMode_PPLL, true >							THairVisiblityVS_PPLL;

IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, THairVisiblityVS_MSAAVisibility_NoCulling,	TEXT("/Engine/Private/HairStrands/HairStrandsVisibilityVS.usf"), TEXT("Main"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, THairVisiblityVS_MSAAVisibility_Culling,		TEXT("/Engine/Private/HairStrands/HairStrandsVisibilityVS.usf"), TEXT("Main"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, THairVisiblityVS_Transmittance,				TEXT("/Engine/Private/HairStrands/HairStrandsVisibilityVS.usf"), TEXT("Main"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, THairVisiblityVS_TransmittanceAndHairCount,	TEXT("/Engine/Private/HairStrands/HairStrandsVisibilityVS.usf"), TEXT("Main"), SF_Vertex);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, THairVisiblityVS_PPLL,						TEXT("/Engine/Private/HairStrands/HairStrandsVisibilityVS.usf"), TEXT("Main"), SF_Vertex);

/////////////////////////////////////////////////////////////////////////////////////////

class FHairVisibilityShaderElementData : public FMeshMaterialShaderElementData
{
public:
	FHairVisibilityShaderElementData(uint32 InHairMacroGroupId, uint32 InHairMaterialId, uint32 InLightChannelMask) : HairMacroGroupId(InHairMacroGroupId), HairMaterialId(InHairMaterialId), LightChannelMask(InLightChannelMask) { }
	uint32 HairMacroGroupId;
	uint32 HairMaterialId;
	uint32 LightChannelMask;
};

template<EHairVisibilityRenderMode RenderMode>
class FHairVisibilityPS : public FMeshMaterialShader
{
	DECLARE_SHADER_TYPE(FHairVisibilityPS, MeshMaterial);

public:

	FHairVisibilityPS(const ShaderMetaType::CompiledShaderInitializerType& Initializer)
		:	FMeshMaterialShader(Initializer)
	{
		ERHIFeatureLevel::Type FeatureLevel = GetMaxSupportedFeatureLevel((EShaderPlatform)Initializer.Target.Platform);
		check(FSceneInterface::GetShadingPath(FeatureLevel) != EShadingPath::Mobile);
		HairVisibilityPass_HairMacroGroupIndex.Bind(Initializer.ParameterMap, TEXT("HairVisibilityPass_HairMacroGroupIndex"));
		HairVisibilityPass_HairMaterialId.Bind(Initializer.ParameterMap, TEXT("HairVisibilityPass_HairMaterialId"));
		HairVisibilityPass_LightChannelMask.Bind(Initializer.ParameterMap, TEXT("HairVisibilityPass_LightChannelMask"));
	}

	FHairVisibilityPS() {}

	static bool ShouldCompilePermutation(const FMeshMaterialShaderPermutationParameters& Parameters)
	{
		if (Parameters.VertexFactoryType->GetFName() != FName(TEXT("FHairStrandsVertexFactory")))
		{
			return false;
		}

		// Disable PPLL rendering for non-PC platform
		if (RenderMode == HairVisibilityRenderMode_PPLL)
		{
			return IsCompatibleWithHairStrands(Parameters.Platform, Parameters.MaterialParameters) && IsPCPlatform(Parameters.Platform) && !IsMobilePlatform(Parameters.Platform);
		}
		else
		{
			return IsCompatibleWithHairStrands(Parameters.Platform, Parameters.MaterialParameters);
		}
	}

	static void ModifyCompilationEnvironment(const FMaterialShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)	
	{
		FMaterialShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		const uint32 RenderModeValue = uint32(RenderMode);
		OutEnvironment.SetDefine(TEXT("HAIR_RENDER_MODE"), RenderModeValue);

		if (RenderMode == HairVisibilityRenderMode_MSAA_Visibility)
		{
			OutEnvironment.SetRenderTargetOutputFormat(0, PF_R32_UINT);
		}
		else if (RenderMode == HairVisibilityRenderMode_Transmittance)
		{
			OutEnvironment.SetRenderTargetOutputFormat(0, PF_R32_FLOAT);
		}
		else if (RenderMode == HairVisibilityRenderMode_TransmittanceAndHairCount)
		{
			OutEnvironment.SetRenderTargetOutputFormat(0, PF_R32_FLOAT);
			OutEnvironment.SetRenderTargetOutputFormat(1, PF_R32G32_UINT);
		}
	}

	void GetShaderBindings(
		const FScene* Scene,
		ERHIFeatureLevel::Type FeatureLevel,
		const FPrimitiveSceneProxy* PrimitiveSceneProxy,
		const FMaterialRenderProxy& MaterialRenderProxy,
		const FMaterial& Material,
		const FMeshPassProcessorRenderState& DrawRenderState,
		const FHairVisibilityShaderElementData& ShaderElementData,
		FMeshDrawSingleShaderBindings& ShaderBindings) const
	{
		FMeshMaterialShader::GetShaderBindings(Scene, FeatureLevel, PrimitiveSceneProxy, MaterialRenderProxy, Material, DrawRenderState, ShaderElementData, ShaderBindings);
		ShaderBindings.Add(HairVisibilityPass_HairMacroGroupIndex, ShaderElementData.HairMacroGroupId);
		ShaderBindings.Add(HairVisibilityPass_HairMaterialId, ShaderElementData.HairMaterialId);
		ShaderBindings.Add(HairVisibilityPass_LightChannelMask, ShaderElementData.LightChannelMask);
	}

	LAYOUT_FIELD(FShaderParameter, HairVisibilityPass_HairMacroGroupIndex);
	LAYOUT_FIELD(FShaderParameter, HairVisibilityPass_HairMaterialId);
	LAYOUT_FIELD(FShaderParameter, HairVisibilityPass_LightChannelMask);
};
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FHairVisibilityPS<HairVisibilityRenderMode_MSAA_Visibility>, TEXT("/Engine/Private/HairStrands/HairStrandsVisibilityPS.usf"), TEXT("MainVisibility"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FHairVisibilityPS<HairVisibilityRenderMode_Transmittance>, TEXT("/Engine/Private/HairStrands/HairStrandsVisibilityPS.usf"), TEXT("MainVisibility"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FHairVisibilityPS<HairVisibilityRenderMode_TransmittanceAndHairCount>, TEXT("/Engine/Private/HairStrands/HairStrandsVisibilityPS.usf"), TEXT("MainVisibility"), SF_Pixel);
IMPLEMENT_MATERIAL_SHADER_TYPE(template<>, FHairVisibilityPS<HairVisibilityRenderMode_PPLL>, TEXT("/Engine/Private/HairStrands/HairStrandsVisibilityPS.usf"), TEXT("MainVisibility"), SF_Pixel);

/////////////////////////////////////////////////////////////////////////////////////////

class FHairVisibilityProcessor : public FMeshPassProcessor
{
public:
	FHairVisibilityProcessor(
		const FScene* Scene,
		const FSceneView* InViewIfDynamicMeshCommand,
		const FMeshPassProcessorRenderState& InPassDrawRenderState,
		const EHairVisibilityRenderMode InRenderMode,
		FDynamicPassMeshDrawListContext* InDrawListContext);

	virtual void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId = -1) override final;
	void AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId, uint32 HairMacroGroupId, uint32 HairMaterialId, bool bCullingEnable);

private:
	bool TryAddMeshBatch(
		const FMeshBatch& RESTRICT MeshBatch,
		uint64 BatchElementMask,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		int32 StaticMeshId,
		uint32 HairMacroGroupId,
		uint32 HairMaterialId,
		bool bCullingEnable,
		const FMaterialRenderProxy& MaterialRenderProxy,
		const FMaterial& Material);

	template<EHairVisibilityRenderMode RenderMode, bool bCullingEnable=true>
	bool Process(
		const FMeshBatch& MeshBatch,
		uint64 BatchElementMask,
		const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
		int32 StaticMeshId,
		const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
		const FMaterial& RESTRICT MaterialResource,
		const uint32 HairMacroGroupId,
		const uint32 HairMaterialId,
		const uint32 LightChannelMask,
		ERasterizerFillMode MeshFillMode,
		ERasterizerCullMode MeshCullMode);

	const EHairVisibilityRenderMode RenderMode;
	FMeshPassProcessorRenderState PassDrawRenderState;
};

void FHairVisibilityProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId)
{
	AddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, 0, 0, false);
}

void FHairVisibilityProcessor::AddMeshBatch(const FMeshBatch& RESTRICT MeshBatch, uint64 BatchElementMask, const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy, int32 StaticMeshId, uint32 HairMacroGroupId, uint32 HairMaterialId, bool bCullingEnable)
{
	const FMaterialRenderProxy* MaterialRenderProxy = MeshBatch.MaterialRenderProxy;
	while (MaterialRenderProxy)
	{
		const FMaterial* Material = MaterialRenderProxy->GetMaterialNoFallback(FeatureLevel);
		if (Material)
		{
			if (TryAddMeshBatch(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, HairMacroGroupId, HairMaterialId, bCullingEnable, *MaterialRenderProxy, *Material))
			{
				break;
			}
		}

		MaterialRenderProxy = MaterialRenderProxy->GetFallback(FeatureLevel);
	}
}

bool FHairVisibilityProcessor::TryAddMeshBatch(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	uint32 HairMacroGroupId,
	uint32 HairMaterialId,
	bool bCullingEnable,
	const FMaterialRenderProxy& MaterialRenderProxy,
	const FMaterial& Material)
{
	static const FVertexFactoryType* CompatibleVF = FVertexFactoryType::GetVFByName(TEXT("FHairStrandsVertexFactory"));

	// Determine the mesh's material and blend mode.
	const bool bIsCompatible = IsCompatibleWithHairStrands(&Material, FeatureLevel);
	const bool bIsHairStrandsFactory = MeshBatch.VertexFactory->GetType()->GetHashedName() == CompatibleVF->GetHashedName();
	const bool bShouldRender = (!PrimitiveSceneProxy && MeshBatch.Elements.Num() > 0) || (PrimitiveSceneProxy && PrimitiveSceneProxy->ShouldRenderInMainPass());
	const uint32 LightChannelMask = PrimitiveSceneProxy ? PrimitiveSceneProxy->GetLightingChannelMask() : 0;

	if (bIsCompatible
		&& bIsHairStrandsFactory
		&& bShouldRender
		&& ShouldIncludeDomainInMeshPass(Material.GetMaterialDomain()))
	{
		const FMeshDrawingPolicyOverrideSettings OverrideSettings = ComputeMeshOverrideSettings(MeshBatch);
		const ERasterizerFillMode MeshFillMode = ComputeMeshFillMode(MeshBatch, Material, OverrideSettings);
		const ERasterizerCullMode MeshCullMode = ComputeMeshCullMode(MeshBatch, Material, OverrideSettings);
		if (RenderMode == HairVisibilityRenderMode_MSAA_Visibility && bCullingEnable)
			return Process<HairVisibilityRenderMode_MSAA_Visibility, true>(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MaterialRenderProxy, Material, HairMacroGroupId, HairMaterialId, LightChannelMask, MeshFillMode, MeshCullMode);
		else if (RenderMode == HairVisibilityRenderMode_MSAA_Visibility && !bCullingEnable)
			return Process<HairVisibilityRenderMode_MSAA_Visibility, false>(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MaterialRenderProxy, Material, HairMacroGroupId, HairMaterialId, LightChannelMask, MeshFillMode, MeshCullMode);
		else if (RenderMode == HairVisibilityRenderMode_Transmittance)
			return Process<HairVisibilityRenderMode_Transmittance>(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MaterialRenderProxy, Material, HairMacroGroupId, HairMaterialId, LightChannelMask, MeshFillMode, MeshCullMode);
		else if (RenderMode == HairVisibilityRenderMode_TransmittanceAndHairCount)
			return Process<HairVisibilityRenderMode_TransmittanceAndHairCount>(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MaterialRenderProxy, Material, HairMacroGroupId, HairMaterialId, LightChannelMask, MeshFillMode, MeshCullMode);
		else if (RenderMode == HairVisibilityRenderMode_PPLL)
			return Process<HairVisibilityRenderMode_PPLL>(MeshBatch, BatchElementMask, PrimitiveSceneProxy, StaticMeshId, MaterialRenderProxy, Material, HairMacroGroupId, HairMaterialId, LightChannelMask, MeshFillMode, MeshCullMode);
	}

	return true;
}

template<EHairVisibilityRenderMode TRenderMode, bool bCullingEnable>
bool FHairVisibilityProcessor::Process(
	const FMeshBatch& MeshBatch, 
	uint64 BatchElementMask, 
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	int32 StaticMeshId,
	const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
	const FMaterial& RESTRICT MaterialResource,
	const uint32 HairMacroGroupId,
	const uint32 HairMaterialId,
	const uint32 LightChannelMask,
	ERasterizerFillMode MeshFillMode,
	ERasterizerCullMode MeshCullMode)
{
	const FVertexFactory* VertexFactory = MeshBatch.VertexFactory;

	TMeshProcessorShaders<
		FHairVisibilityVS<TRenderMode, bCullingEnable>,
		FHairVisibilityPS<TRenderMode>> PassShaders;
	{
		FMaterialShaderTypes ShaderTypes;
		ShaderTypes.AddShaderType<FHairVisibilityVS<TRenderMode, bCullingEnable>>();
		ShaderTypes.AddShaderType<FHairVisibilityPS<TRenderMode>>();

		FVertexFactoryType* VertexFactoryType = VertexFactory->GetType();

		FMaterialShaders Shaders;
		if (!MaterialResource.TryGetShaders(ShaderTypes, VertexFactoryType, Shaders))
		{
			return false;
		}

		Shaders.TryGetVertexShader(PassShaders.VertexShader);
		Shaders.TryGetPixelShader(PassShaders.PixelShader);
	}

	FMeshPassProcessorRenderState DrawRenderState(PassDrawRenderState);
	FHairVisibilityShaderElementData ShaderElementData(HairMacroGroupId, HairMaterialId, LightChannelMask);
	ShaderElementData.InitializeMeshMaterialData(ViewIfDynamicMeshCommand, PrimitiveSceneProxy, MeshBatch, StaticMeshId, false);

	BuildMeshDrawCommands(
		MeshBatch,
		BatchElementMask,
		PrimitiveSceneProxy,
		MaterialRenderProxy,
		MaterialResource,
		DrawRenderState,
		PassShaders,
		MeshFillMode,
		MeshCullMode,
		FMeshDrawCommandSortKey::Default,
		EMeshPassFeatures::Default,
		ShaderElementData);

	return true;
}

FHairVisibilityProcessor::FHairVisibilityProcessor(
	const FScene* Scene,
	const FSceneView* InViewIfDynamicMeshCommand,
	const FMeshPassProcessorRenderState& InPassDrawRenderState,
	const EHairVisibilityRenderMode InRenderMode,
	FDynamicPassMeshDrawListContext* InDrawListContext)
	: FMeshPassProcessor(Scene, Scene->GetFeatureLevel(), InViewIfDynamicMeshCommand, InDrawListContext)
	, RenderMode(InRenderMode)
	, PassDrawRenderState(InPassDrawRenderState)
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Clear uint texture
class FClearUIntGraphicPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClearUIntGraphicPS);
	SHADER_USE_PARAMETER_STRUCT(FClearUIntGraphicPS, FGlobalShader);

	class FOutputFormat : SHADER_PERMUTATION_INT("PERMUTATION_OUTPUT_FORMAT", 2);
	using FPermutationDomain = TShaderPermutationDomain<FOutputFormat>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ClearValue)
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairStrandsTilePassVS::FParameters, TileData)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FOutputFormat>() == 0)
		{
			OutEnvironment.SetRenderTargetOutputFormat(0, PF_R32_UINT);
		}
		else if (PermutationVector.Get<FOutputFormat>() == 1)
		{
			OutEnvironment.SetRenderTargetOutputFormat(0, PF_R32G32_UINT);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FClearUIntGraphicPS, "/Engine/Private/HairStrands/HairStrandsVisibilityClearPS.usf", "ClearPS", SF_Pixel);

static void AddClearGraphicPass(
	FRDGBuilder& GraphBuilder,
	FRDGEventName&& PassName,
	const FViewInfo* View,
	const uint32 ClearValue,
	const FHairStrandsTiles& TileData,
	FRDGTextureRef& OutTarget)
{
	check(OutTarget);
	const bool bUseTile = TileData.IsValid();

	const FHairStrandsTiles::ETileType TileType = FHairStrandsTiles::ETileType::HairAll;

	FClearUIntGraphicPS::FParameters* Parameters = GraphBuilder.AllocParameters<FClearUIntGraphicPS::FParameters>();
	Parameters->ClearValue = ClearValue;
	Parameters->TileData = GetHairStrandsTileParameters(*View, TileData, TileType);
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutTarget, ERenderTargetLoadAction::ENoAction, 0);

	FClearUIntGraphicPS::FPermutationDomain PermutationVector;
	if (OutTarget->Desc.Format == PF_R32_UINT)
	{
		PermutationVector.Set<FClearUIntGraphicPS::FOutputFormat>(0);
	}
	else if (OutTarget->Desc.Format == PF_R32G32_UINT)
	{
		PermutationVector.Set<FClearUIntGraphicPS::FOutputFormat>(1);
	}

	TShaderMapRef<FPostProcessVS> ScreenVertexShader(View->ShaderMap);
	TShaderMapRef<FHairStrandsTilePassVS> TileVertexShader(View->ShaderMap);
	TShaderMapRef<FClearUIntGraphicPS> PixelShader(View->ShaderMap, PermutationVector);
	const FIntRect Viewport = bUseTile ? View->ViewRect : FIntRect(FIntPoint(0, 0), OutTarget->Desc.Extent);
	const FIntPoint Resolution = OutTarget->Desc.Extent;

	//ClearUnusedGraphResources(PixelShader, Parameters);

	GraphBuilder.AddPass(
		Forward<FRDGEventName>(PassName),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, ScreenVertexShader, TileVertexShader, PixelShader, Viewport, Resolution, bUseTile, TileType](FRHICommandList& RHICmdList)
	{
		FHairStrandsTilePassVS::FParameters ParametersVS = Parameters->TileData;

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();
		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = bUseTile ? TileVertexShader.GetVertexShader() : ScreenVertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = Parameters->TileData.bRectPrimitive > 0 ? PT_RectList : PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

		RHICmdList.SetViewport(Viewport.Min.X, Viewport.Min.Y, 0.0f, Viewport.Max.X, Viewport.Max.Y, 1.0f);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Parameters);
		if (bUseTile)
		{
			SetShaderParameters(RHICmdList, TileVertexShader, TileVertexShader.GetVertexShader(), ParametersVS);
			RHICmdList.SetStreamSource(0, nullptr, 0);
			RHICmdList.DrawPrimitiveIndirect(Parameters->TileData.TileIndirectBuffer->GetRHI(), FHairStrandsTiles::GetIndirectDrawArgOffset(TileType));
		}
		else
		{
			DrawRectangle(
				RHICmdList,
				0, 0,
				Viewport.Width(), Viewport.Height(),
				Viewport.Min.X, Viewport.Min.Y,
				Viewport.Width(), Viewport.Height(),
				Viewport.Size(),
				Resolution,
				ScreenVertexShader,
				EDRF_UseTriangleOptimization);
		}
	});
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Copy dispatch count into an indirect buffer 
class FCopyIndirectBufferCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCopyIndirectBufferCS);
	SHADER_USE_PARAMETER_STRUCT(FCopyIndirectBufferCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, ThreadGroupSize)
		SHADER_PARAMETER(uint32, ItemCountPerGroup)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CounterTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer, OutArgBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform); }
};

IMPLEMENT_GLOBAL_SHADER(FCopyIndirectBufferCS, "/Engine/Private/HairStrands/HairStrandsVisibilityCopyIndirectArg.usf", "CopyCS", SF_Compute);

static FRDGBufferRef AddCopyIndirectArgPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo* View,
	const uint32 ThreadGroupSize,
	const uint32 ItemCountPerGroup,
	FRDGTextureRef CounterTexture)
{
	check(CounterTexture);

	FRDGBufferRef OutBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(), TEXT("Hair.VisibilityIndirectArgBuffer"));

	FCopyIndirectBufferCS::FParameters* Parameters = GraphBuilder.AllocParameters<FCopyIndirectBufferCS::FParameters>();
	Parameters->ThreadGroupSize = ThreadGroupSize;
	Parameters->ItemCountPerGroup = ItemCountPerGroup;
	Parameters->CounterTexture = CounterTexture;
	Parameters->OutArgBuffer = GraphBuilder.CreateUAV(OutBuffer);

	TShaderMapRef<FCopyIndirectBufferCS> ComputeShader(View->ShaderMap);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("HairStrands::VisbilityCopyIndirectArgs"),
		ComputeShader,
		Parameters,
		FIntVector(1,1,1));

	return OutBuffer;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairVisibilityPrimitiveIdCompactionCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairVisibilityPrimitiveIdCompactionCS);
	SHADER_USE_PARAMETER_STRUCT(FHairVisibilityPrimitiveIdCompactionCS, FGlobalShader);

	class FGroupSize	: SHADER_PERMUTATION_SPARSE_INT("PERMUTATION_GROUPSIZE", 32, 64);
	class FVelocity		: SHADER_PERMUTATION_INT("PERMUTATION_VELOCITY", 2);
	class FTile			: SHADER_PERMUTATION_BOOL("PERMUTATION_TILE");
	class FPPLL 		: SHADER_PERMUTATION_SPARSE_INT("PERMUTATION_PPLL", 0, 8, 16, 32); // See GetPPLLMaxRenderNodePerPixel
	class FMSAACount 	: SHADER_PERMUTATION_SPARSE_INT("PERMUTATION_MSAACOUNT", 1, 2, 4, 8);
	using FPermutationDomain = TShaderPermutationDomain<FGroupSize, FVelocity, FTile, FPPLL, FMSAACount>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, OutputResolution)
		SHADER_PARAMETER(FIntPoint, ResolutionOffset)
		SHADER_PARAMETER(uint32, MaxNodeCount)
		SHADER_PARAMETER(uint32, bSortSampleByDepth)
		SHADER_PARAMETER(float, DepthTheshold)
		SHADER_PARAMETER(float, CosTangentThreshold)
		SHADER_PARAMETER(float, CoverageThreshold)
		SHADER_PARAMETER(uint32, VelocityType)

		SHADER_PARAMETER(FIntPoint, TileCountXY)
		SHADER_PARAMETER(uint32, TileSize)

		// Available for the MSAA path
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MSAA_DepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MSAA_IDTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MSAA_MaterialTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MSAA_AttributeTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, MSAA_VelocityTexture)
		// Available for the PPLL path
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PPLLCounter)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PPLLNodeIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, PPLLNodeData)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ViewTransmittanceTexture)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutCompactNodeCounter)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutCompactNodeIndex)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutCoverageTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, OutCompactNodeVis)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, OutCompactNodeData)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer, OutCompactNodeCoord)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutVelocityTexture)
		
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint2>, TileDataBuffer)						// Tile coords (RG16)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileCountBuffer)						// Tile total count (actual number of tiles)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		RDG_BUFFER_ACCESS(IndirectBufferArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

public:

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		if (PermutationVector.Get<FPPLL>() > 0)
		{
			PermutationVector.Set<FMSAACount>(1);
		}
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FPPLL>() > 0 && PermutationVector.Get<FMSAACount>() != 1)
		{
			return false;
		}
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairVisibilityPrimitiveIdCompactionCS, "/Engine/Private/HairStrands/HairStrandsVisibilityCompaction.usf", "MainCS", SF_Compute);

static void AddHairVisibilityPrimitiveIdCompactionPass(
	const bool bUsePPLL,
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FRDGTextureRef& SceneDepthTexture,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	const uint32 NodeGroupSize,
	const FHairStrandsTiles& TileData,
	FHairVisibilityPrimitiveIdCompactionCS::FParameters* PassParameters,
	FRDGTextureRef& OutCompactCounter,
	FRDGTextureRef& OutCompactNodeIndex,
	FRDGBufferRef& OutCompactNodeVis, // Or OutCompactNodeData for PPLL
	FRDGBufferRef& OutCompactNodeCoord,
	FRDGTextureRef& OutCoverageTexture,
	FRDGTextureRef OutVelocityTexture,
	FRDGBufferRef& OutIndirectArgsBuffer,
	uint32& OutMaxRenderNodeCount)
{
	FIntPoint Resolution;
	if (bUsePPLL)
	{
		check(PassParameters->PPLLCounter);
		check(PassParameters->PPLLNodeIndex);
		check(PassParameters->PPLLNodeData);
		Resolution = PassParameters->PPLLNodeIndex->Desc.Extent;
	}
	else
	{
		check(PassParameters->MSAA_DepthTexture->Desc.NumSamples == GetMaxSamplePerPixel());
		check(PassParameters->MSAA_DepthTexture);
		check(PassParameters->MSAA_IDTexture);
		Resolution = PassParameters->MSAA_DepthTexture->Desc.Extent;
	}

	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(1, 1), PF_R32_UINT, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource);
		OutCompactCounter = GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityCompactCounter"));
	}

	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(Resolution, PF_R32_UINT, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource);
		OutCompactNodeIndex = GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityCompactNodeIndex"));
	}

	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(Resolution, FHairStrandsVisibilityData::CoverageFormat, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource);
		OutCoverageTexture = GraphBuilder.CreateTexture(Desc, TEXT("Hair.CoverageTexture"));
	}

	const uint32 ClearValues[4] = { 0u,0u,0u,0u };
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutCompactCounter), ClearValues);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutCompactNodeIndex), ClearValues);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutCoverageTexture), 0.f);

	// Adapt the buffer allocation based on the bounding box of the hair macro groups. This allows to reduce the overall allocation size
	const FIntRect HairRect = ComputeVisibleHairStrandsMacroGroupsRect(View.ViewRect, MacroGroupDatas);
	const FIntPoint EffectiveResolution = bUsePPLL ? FIntPoint(View.ViewRect.Width(), View.ViewRect.Height()) : FIntPoint(HairRect.Width(), HairRect.Height());

	// Select render node count according to current mode
	const uint32 MSAASampleCount = GetHairVisibilityRenderMode() == HairVisibilityRenderMode_MSAA_Visibility ? GetMaxSamplePerPixel() : 1;
	const uint32 PPLLMaxRenderNodePerPixel = GetMaxSamplePerPixel();
	const uint32 MaxRenderNodeCount = GetTotalSampleCountForAllocation(EffectiveResolution);
	const bool bUseTile = TileData.IsValid();

	if (bUsePPLL)
	{
		// PPLL output directly the node data
		OutCompactNodeVis = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(HairStrandsVisibilityInternal::NodeData), MaxRenderNodeCount), TEXT("Hair.VisibilityNodeData"));
	}
	else
	{
		OutCompactNodeVis = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(HairStrandsVisibilityInternal::NodeVis), MaxRenderNodeCount), TEXT("Hair.VisibilityNodeVis"));
	}

	{
		// Pixel coord of the node. Stored as 2*R16_UINT
		OutCompactNodeCoord = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxRenderNodeCount), TEXT("Hair.VisibilityNodeCoord"));
	}

	const bool bWriteOutVelocity = OutVelocityTexture != nullptr && bUsePPLL; // Velocity write out is only support with PPLL
	const uint32 VelocityPermutation = bWriteOutVelocity ? FMath::Clamp(GHairVelocityType + 1, 0, 3) : 0;
	FHairVisibilityPrimitiveIdCompactionCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairVisibilityPrimitiveIdCompactionCS::FGroupSize>(bUseTile ? FHairStrandsTiles::GroupSize : GetVendorOptimalGroupSize1D());
	PermutationVector.Set<FHairVisibilityPrimitiveIdCompactionCS::FVelocity>(VelocityPermutation > 0 ? 1 : 0);
	PermutationVector.Set<FHairVisibilityPrimitiveIdCompactionCS::FTile>(bUseTile);
	PermutationVector.Set<FHairVisibilityPrimitiveIdCompactionCS::FPPLL>(bUsePPLL ? PPLLMaxRenderNodePerPixel : 0);
	PermutationVector.Set<FHairVisibilityPrimitiveIdCompactionCS::FMSAACount>(MSAASampleCount);
	PermutationVector = FHairVisibilityPrimitiveIdCompactionCS::RemapPermutation(PermutationVector);

	PassParameters->ResolutionOffset = FIntPoint(0,0);
	PassParameters->OutputResolution = Resolution;
	PassParameters->VelocityType = VelocityPermutation;
	PassParameters->MaxNodeCount = MaxRenderNodeCount;
	PassParameters->bSortSampleByDepth = GHairStrandsSortHairSampleByDepth > 0 ? 1 : 0;
	PassParameters->CoverageThreshold = GetHairStrandsFullCoverageThreshold();
	PassParameters->DepthTheshold = FMath::Clamp(GHairStrandsMaterialCompactionDepthThreshold, 0.f, 100.f);
	PassParameters->CosTangentThreshold = FMath::Cos(FMath::DegreesToRadians(FMath::Clamp(GHairStrandsMaterialCompactionTangentThreshold, 0.f, 90.f)));
	PassParameters->SceneDepthTexture = SceneDepthTexture;
	PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
	PassParameters->OutCompactNodeCounter = GraphBuilder.CreateUAV(OutCompactCounter);
	PassParameters->OutCompactNodeIndex = GraphBuilder.CreateUAV(OutCompactNodeIndex);
	if (bUsePPLL)
		PassParameters->OutCompactNodeData = GraphBuilder.CreateUAV(OutCompactNodeVis);
	else
		PassParameters->OutCompactNodeVis = GraphBuilder.CreateUAV(OutCompactNodeVis);
	PassParameters->OutCompactNodeCoord = GraphBuilder.CreateUAV(OutCompactNodeCoord, FHairStrandsVisibilityData::NodeCoordFormat);
	PassParameters->OutCoverageTexture = GraphBuilder.CreateUAV(OutCoverageTexture);

	if (bWriteOutVelocity)
	{
		PassParameters->OutVelocityTexture = GraphBuilder.CreateUAV(OutVelocityTexture);
	}

	const FHairStrandsTiles::ETileType TileType = FHairStrandsTiles::ETileType::HairAll;
	if (bUseTile)
	{
		PassParameters->TileCountXY			= TileData.TileCountXY;
		PassParameters->TileSize			= FHairStrandsTiles::TileSize;
		PassParameters->TileCountBuffer		= GraphBuilder.CreateSRV(TileData.TileCountBuffer, PF_R32_UINT);
		PassParameters->TileDataBuffer		= TileData.GetTileBufferSRV(TileType);
		PassParameters->IndirectBufferArgs	= TileData.TileIndirectDispatchBuffer;
	}
 
	TShaderMapRef<FHairVisibilityPrimitiveIdCompactionCS> ComputeShader(View.ShaderMap, PermutationVector);
	if (bUseTile)
	{
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrands::VisibilityCompaction(Tile)"),
			ComputeShader,
			PassParameters,
			TileData.TileIndirectDispatchBuffer,
			FHairStrandsTiles::GetIndirectDispatchArgOffset(TileType));
	}
	else
	{
		const FIntPoint GroupSize = GetVendorOptimalGroupSize2D();
		const FIntPoint RectResolution(View.ViewRect.Width(), View.ViewRect.Height());

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrands::VisibilityCompaction(Screen)"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(RectResolution, GroupSize));
	}

	OutIndirectArgsBuffer = AddCopyIndirectArgPass(GraphBuilder, &View, NodeGroupSize, 1, OutCompactCounter);
	OutMaxRenderNodeCount = MaxRenderNodeCount;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairVisibilityCompactionComputeRasterCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairVisibilityCompactionComputeRasterCS);
	SHADER_USE_PARAMETER_STRUCT(FHairVisibilityCompactionComputeRasterCS, FGlobalShader);

	class FGroupSize : SHADER_PERMUTATION_SPARSE_INT("PERMUTATION_GROUPSIZE", 32, 64);
	class FTile : SHADER_PERMUTATION_BOOL("PERMUTATION_TILE");
	using FPermutationDomain = TShaderPermutationDomain<FGroupSize, FTile>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, OutputResolution)
		SHADER_PARAMETER(uint32, MaxNodeCount)
		SHADER_PARAMETER(uint32, SamplerPerPixel)
		SHADER_PARAMETER(float, CoverageThreshold)

		SHADER_PARAMETER(FIntPoint, TileCountXY)
		SHADER_PARAMETER(uint32, TileSize)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<UlongType>, VisibilityTexture0)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<UlongType>, VisibilityTexture1)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<UlongType>, VisibilityTexture2)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<UlongType>, VisibilityTexture3)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, ViewTransmittanceTexture)

		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutCompactNodeCounter)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutCompactNodeIndex)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutCoverageTexture)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer, OutCompactNodeVis)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer, OutCompactNodeCoord)

		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint2>, TileDataBuffer)						// Tile coords (RG16)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, TileCountBuffer)						// Tile total count (actual number of tiles)

		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		RDG_BUFFER_ACCESS(IndirectBufferArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

public:

	static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairVisibilityCompactionComputeRasterCS, "/Engine/Private/HairStrands/HairStrandsVisibilityCompactionComputeRaster.usf", "MainCS", SF_Compute);

static void AddHairVisibilityCompactionComputeRasterPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const uint32 NodeGroupSize,
	const uint32 SamplerPerPixel,
	const FRasterComputeOutput& RasterComputeData,
	const FHairStrandsTiles& TileData,
	FRDGTextureRef& InTransmittanceTexture,
	FRDGTextureRef& OutCompactCounter,
	FRDGTextureRef& OutCompactNodeIndex,
	FRDGBufferRef&  OutCompactNodeVis,
	FRDGBufferRef&  OutCompactNodeCoord,
	FRDGTextureRef& OutCoverageTexture,
	FRDGBufferRef&  OutIndirectArgsBuffer,
	uint32& OutMaxRenderNodeCount)
{	
	FIntPoint Resolution = RasterComputeData.VisibilityTexture0->Desc.Extent;

	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(1,1), PF_R32_UINT, FClearValueBinding::None, TexCreate_UAV);
		OutCompactCounter = GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityCompactCounter"));
	}

	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(Resolution, PF_R32_UINT, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource);
		OutCompactNodeIndex = GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityCompactNodeIndex"));
	}

	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(Resolution, FHairStrandsVisibilityData::CoverageFormat, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource);
		OutCoverageTexture = GraphBuilder.CreateTexture(Desc, TEXT("Hair.CoverageTexture"));
	}

	const uint32 ClearValues[4] = { 0u,0u,0u,0u };
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutCompactCounter), ClearValues);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutCompactNodeIndex), ClearValues);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutCoverageTexture), 0.f);

	// Select render node count according to current mode
	const bool bUseTile = TileData.IsValid();
	const FHairStrandsTiles::ETileType TileType = FHairStrandsTiles::ETileType::HairAll;
	const uint32 MSAASampleCount = GetHairVisibilityRenderMode() == HairVisibilityRenderMode_MSAA_Visibility ? GetMaxSamplePerPixel() : 1;
	const uint32 PPLLMaxRenderNodePerPixel = GetMaxSamplePerPixel();
	const uint32 MaxRenderNodeCount = GetTotalSampleCountForAllocation(Resolution);
	OutCompactNodeVis = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(HairStrandsVisibilityInternal::NodeVis), MaxRenderNodeCount), TEXT("Hair.VisibilityPrimitiveIdCompactNodeData"));
	OutCompactNodeCoord = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), MaxRenderNodeCount), TEXT("Hair.VisibilityPrimitiveIdCompactNodeCoord"));

	FRDGTextureRef DefaultTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
	FHairVisibilityCompactionComputeRasterCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHairVisibilityCompactionComputeRasterCS::FParameters>();
	PassParameters->VisibilityTexture0		= RasterComputeData.VisibilityTexture0;
	PassParameters->VisibilityTexture1		= SamplerPerPixel > 1 ? RasterComputeData.VisibilityTexture1 : DefaultTexture;
	PassParameters->VisibilityTexture2		= SamplerPerPixel > 2 ? RasterComputeData.VisibilityTexture2 : DefaultTexture;
	PassParameters->VisibilityTexture3		= SamplerPerPixel > 3 ? RasterComputeData.VisibilityTexture3 : DefaultTexture;
	PassParameters->SamplerPerPixel			= SamplerPerPixel;
	PassParameters->ViewTransmittanceTexture= InTransmittanceTexture;
	PassParameters->OutputResolution		= Resolution;
	PassParameters->MaxNodeCount			= MaxRenderNodeCount;
	PassParameters->CoverageThreshold		= GetHairStrandsFullCoverageThreshold();
	PassParameters->ViewUniformBuffer		= View.ViewUniformBuffer;
	PassParameters->SceneTexturesStruct		= CreateSceneTextureUniformBuffer(GraphBuilder, View.FeatureLevel);
	PassParameters->OutCompactNodeCounter	= GraphBuilder.CreateUAV(OutCompactCounter);
	PassParameters->OutCompactNodeIndex		= GraphBuilder.CreateUAV(OutCompactNodeIndex);
	PassParameters->OutCompactNodeVis		= GraphBuilder.CreateUAV(OutCompactNodeVis);
	PassParameters->OutCompactNodeCoord		= GraphBuilder.CreateUAV(OutCompactNodeCoord, FHairStrandsVisibilityData::NodeCoordFormat);
	PassParameters->OutCoverageTexture		= GraphBuilder.CreateUAV(OutCoverageTexture);

	if (bUseTile)
	{
		PassParameters->TileCountXY = TileData.TileCountXY;
		PassParameters->TileSize = FHairStrandsTiles::TileSize;
		PassParameters->TileCountBuffer = GraphBuilder.CreateSRV(TileData.TileCountBuffer, PF_R32_UINT);
		PassParameters->TileDataBuffer = TileData.GetTileBufferSRV(TileType);
		PassParameters->IndirectBufferArgs = TileData.TileIndirectDispatchBuffer;
	}

	const FIntPoint GroupSize = GetVendorOptimalGroupSize2D();
	FHairVisibilityCompactionComputeRasterCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairVisibilityCompactionComputeRasterCS::FGroupSize>(bUseTile ? FHairStrandsTiles::GroupSize : GetVendorOptimalGroupSize1D());
	PermutationVector.Set<FHairVisibilityCompactionComputeRasterCS::FTile>(bUseTile);
	TShaderMapRef<FHairVisibilityCompactionComputeRasterCS> ComputeShader(View.ShaderMap, PermutationVector);

	if (bUseTile)
	{
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrands::VisibilityCompaction(Tile)"),
			ComputeShader,
			PassParameters,
			TileData.TileIndirectDispatchBuffer,
			FHairStrandsTiles::GetIndirectDispatchArgOffset(TileType));
	}
	else
	{
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("HairStrands::VisibilityCompaction(Screen)"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(Resolution, GroupSize));
	}

	OutIndirectArgsBuffer = AddCopyIndirectArgPass(GraphBuilder, &View, NodeGroupSize, 1, OutCompactCounter);
	OutMaxRenderNodeCount = MaxRenderNodeCount;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairVisibilityFillOpaqueDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairVisibilityFillOpaqueDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FHairVisibilityFillOpaqueDepthPS, FGlobalShader);

	class FTile : SHADER_PERMUTATION_BOOL("PERMUTATION_TILE");
	using FPermutationDomain = TShaderPermutationDomain<FTile>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VisibilityDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VisibilityIDTexture)
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairStrandsTilePassVS::FParameters, TileData)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform); }
};

IMPLEMENT_GLOBAL_SHADER(FHairVisibilityFillOpaqueDepthPS, "/Engine/Private/HairStrands/HairStrandsVisibilityFillOpaqueDepthPS.usf", "MainPS", SF_Pixel);

static FRDGTextureRef AddHairVisibilityFillOpaqueDepth(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FIntPoint& Resolution,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	const FHairStrandsTiles& TileData,
	const FRDGTextureRef& SceneDepthTexture)
{
	check(GetHairVisibilityRenderMode() == HairVisibilityRenderMode_MSAA_Visibility);

	const bool bUseTile = TileData.IsValid();
	const FHairStrandsTiles::ETileType TileType = FHairStrandsTiles::ETileType::HairAll;
	FRDGTextureRef OutVisibilityDepthTexture = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Resolution, PF_D24, FClearValueBinding::DepthFar, TexCreate_DepthStencilTargetable | TexCreate_ShaderResource, 1, GetMaxSamplePerPixel()), TEXT("Hair.VisibilityDepthTexture"));

	FHairVisibilityFillOpaqueDepthPS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairVisibilityFillOpaqueDepthPS::FParameters>();
	Parameters->TileData = GetHairStrandsTileParameters(View, TileData, TileType);
	Parameters->ViewUniformBuffer = View.ViewUniformBuffer;
	Parameters->SceneDepthTexture = SceneDepthTexture;
	Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		OutVisibilityDepthTexture,
		ERenderTargetLoadAction::EClear,
		ERenderTargetLoadAction::ENoAction,
		FExclusiveDepthStencil::DepthWrite_StencilNop);

	FHairVisibilityFillOpaqueDepthPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairVisibilityFillOpaqueDepthPS::FTile>(bUseTile);
	TShaderMapRef<FHairVisibilityFillOpaqueDepthPS> PixelShader(View.ShaderMap, PermutationVector);
	
	const FIntRect Viewport = View.ViewRect;
	if (bUseTile)
	{
		TShaderMapRef<FHairStrandsTilePassVS> TileVertexShader(View.ShaderMap);
		//ClearUnusedGraphResources(PixelShader, Parameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("HairStrands::FillVisibilityDepth(Tile)"),
			Parameters,
			ERDGPassFlags::Raster,
			[Parameters, TileVertexShader, PixelShader, Viewport, TileType](FRHICommandList& RHICmdList)
			{
				FHairStrandsTilePassVS::FParameters ParametersVS = Parameters->TileData;

				FGraphicsPipelineStateInitializer GraphicsPSOInit;
				RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
				GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>::GetRHI();
				GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
				GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();

				GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
				GraphicsPSOInit.BoundShaderState.VertexShaderRHI = TileVertexShader.GetVertexShader();
				GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
				GraphicsPSOInit.PrimitiveType = Parameters->TileData.bRectPrimitive > 0 ? PT_RectList : PT_TriangleList;
				SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

				SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Parameters);

				RHICmdList.SetViewport(Viewport.Min.X, Viewport.Min.Y, 0.0f, Viewport.Max.X, Viewport.Max.Y, 1.0f);
				SetShaderParameters(RHICmdList, TileVertexShader, TileVertexShader.GetVertexShader(), ParametersVS);
				RHICmdList.SetStreamSource(0, nullptr, 0);
				RHICmdList.DrawPrimitiveIndirect(Parameters->TileData.TileIndirectBuffer->GetRHI(), FHairStrandsTiles::GetIndirectDrawArgOffset(TileType));
			});

	}
	else
	{
		TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
		TArray<FIntRect> MacroGroupRects;
		if (IsHairStrandsViewRectOptimEnable())
		{
			for (const FHairStrandsMacroGroupData& MacroGroupData : MacroGroupDatas)
			{
				MacroGroupRects.Add(MacroGroupData.ScreenRect);
			}
		}
		else
		{
			MacroGroupRects.Add(Viewport);
		}

		ClearUnusedGraphResources(PixelShader, Parameters);

		GraphBuilder.AddPass(
			RDG_EVENT_NAME("HairStrands::FillVisibilityDepth(View)"),
			Parameters,
			ERDGPassFlags::Raster,
			[Parameters, VertexShader, PixelShader, MacroGroupRects, Viewport, Resolution](FRHICommandList& RHICmdList)
		{
			FGraphicsPipelineStateInitializer GraphicsPSOInit;
			RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
			GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>::GetRHI();
			GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
			GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI();

			GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
			GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
			GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
			GraphicsPSOInit.PrimitiveType = PT_TriangleList;
			SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

			SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Parameters);

			for (const FIntRect& ViewRect : MacroGroupRects)
			{
				RHICmdList.SetViewport(ViewRect.Min.X, ViewRect.Min.Y, 0.0f, ViewRect.Max.X, ViewRect.Max.Y, 1.0f);
				DrawRectangle(
					RHICmdList,
					0, 0,
					Viewport.Width(), Viewport.Height(),
					Viewport.Min.X,   Viewport.Min.Y,
					Viewport.Width(), Viewport.Height(),
					Viewport.Size(),
					Resolution,
					VertexShader,
					EDRF_UseTriangleOptimization);
			}
		});
	}

	// Ensure HTile is valid after manually feeding the scene depth value
	if (GRHISupportsResummarizeHTile)
	{
		AddResummarizeHTilePass(GraphBuilder, OutVisibilityDepthTexture);
	}

	return OutVisibilityDepthTexture;
}

///////////////////////////////////////////////////////////////////////////////////////////////////

static void AddHairCulledVertexResourcesTransitionPass(
	FRDGBuilder& GraphBuilder,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas)
{
	FBufferTransitionQueue TransitionQueue;
	for (const FHairStrandsMacroGroupData& MacroGroupData : MacroGroupDatas)
	{
		for (const FHairStrandsMacroGroupData::PrimitiveInfo& PrimitiveInfo : MacroGroupData.PrimitivesInfos)
		{
			if (PrimitiveInfo.PublicDataPtr)
			{
				if (FUnorderedAccessViewRHIRef UAV = PrimitiveInfo.PublicDataPtr->CulledVertexIdBuffer.UAV)
				{
					TransitionQueue.Add(UAV);
				}

				if (FUnorderedAccessViewRHIRef UAV = PrimitiveInfo.PublicDataPtr->CulledVertexRadiusScaleBuffer.UAV)
				{
					TransitionQueue.Add(UAV);
				}
			}
		}
	}
	TransitBufferToReadable(GraphBuilder, TransitionQueue);
}

static void AddHairVisibilityCommonPass(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo* ViewInfo,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	const EHairVisibilityRenderMode RenderMode,
	FVisibilityPassParameters* PassParameters,
	FInstanceCullingManager& InstanceCullingManager)
{
	auto GetPassName = [RenderMode]()
	{
		switch (RenderMode)
		{
		case HairVisibilityRenderMode_PPLL:						return RDG_EVENT_NAME("HairStrands::VisibilityPPLLPass");
		case HairVisibilityRenderMode_MSAA_Visibility:			return RDG_EVENT_NAME("HairStrands::VisibilityMSAAVisPass");
		case HairVisibilityRenderMode_Transmittance:			return RDG_EVENT_NAME("HairStrands::TransmittancePass");
		case HairVisibilityRenderMode_TransmittanceAndHairCount:return RDG_EVENT_NAME("HairStrands::TransmittanceAndHairCountPass");
		default:												return RDG_EVENT_NAME("Noname");
		}
	};

	AddHairCulledVertexResourcesTransitionPass(GraphBuilder, MacroGroupDatas);

	// Note: this reference needs to persistent until SubmitMeshDrawCommands() is called, as DrawRenderState does not ref count 
	// the view uniform buffer (raw pointer). It is only within the MeshProcessor that the uniform buffer get reference
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformShaderParameters;
	if (RenderMode == HairVisibilityRenderMode_Transmittance || RenderMode == HairVisibilityRenderMode_TransmittanceAndHairCount || RenderMode == HairVisibilityRenderMode_PPLL)
	{
		const bool bEnableMSAA = false;
		SetUpViewHairRenderInfo(*ViewInfo, bEnableMSAA, ViewInfo->CachedViewUniformShaderParameters->HairRenderInfo, ViewInfo->CachedViewUniformShaderParameters->HairRenderInfoBits, ViewInfo->CachedViewUniformShaderParameters->HairComponents);

		// Create and set the uniform buffer
		PassParameters->View = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(*ViewInfo->CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);
	}
	else
	{
		PassParameters->View = ViewInfo->ViewUniformBuffer;
	}

	AddSimpleMeshPass(GraphBuilder, PassParameters, Scene, *ViewInfo, &InstanceCullingManager, GetPassName(), ViewInfo->ViewRect,
		[PassParameters, Scene = Scene, ViewInfo, &MacroGroupDatas, RenderMode](FDynamicPassMeshDrawListContext* ShadowContext)
	{
		check(IsInRenderingThread());

		FMeshPassProcessorRenderState DrawRenderState;

		{
			if (RenderMode == HairVisibilityRenderMode_MSAA_Visibility)
			{
				DrawRenderState.SetBlendState(TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>::GetRHI());
				DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
			}
			else if (RenderMode == HairVisibilityRenderMode_Transmittance)
			{
				DrawRenderState.SetBlendState(TStaticBlendState<CW_RED, BO_Add, BF_DestColor, BF_Zero, BO_Add, BF_Zero, BF_Zero>::GetRHI());
				DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
			}
			else if (RenderMode == HairVisibilityRenderMode_TransmittanceAndHairCount)
			{
				DrawRenderState.SetBlendState(TStaticBlendState<
					CW_RED, BO_Add, BF_DestColor, BF_Zero, BO_Add, BF_Zero, BF_Zero,
					CW_RG, BO_Add, BF_One, BF_One, BO_Add, BF_Zero, BF_Zero>::GetRHI());
				DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
			}
			else if (RenderMode == HairVisibilityRenderMode_PPLL)
			{
				DrawRenderState.SetBlendState(TStaticBlendState<>::GetRHI());
				DrawRenderState.SetDepthStencilState(TStaticDepthStencilState<false, CF_DepthNearOrEqual>::GetRHI());
			}

			FHairVisibilityProcessor MeshProcessor(Scene, ViewInfo, DrawRenderState, RenderMode, ShadowContext);
			
			for (const FHairStrandsMacroGroupData& MacroGroupData : MacroGroupDatas)
			{
				for (const FHairStrandsMacroGroupData::PrimitiveInfo& PrimitiveInfo : MacroGroupData.PrimitivesInfos)
				{
					if (const FMeshBatch* MeshBatch = PrimitiveInfo.Mesh)
					{
						const uint64 BatchElementMask = ~0ull;
						MeshProcessor.AddMeshBatch(*MeshBatch, BatchElementMask, PrimitiveInfo.PrimitiveSceneProxy, -1, MacroGroupData.MacroGroupId, PrimitiveInfo.MaterialId, PrimitiveInfo.IsCullingEnable());
					}
				}
			}
		}
	});
}

static void AddHairVisibilityMSAAPass(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo* ViewInfo,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	const FIntPoint& Resolution,
	const FHairStrandsTiles& TileData,
	FInstanceCullingManager& InstanceCullingManager,
	FRDGTextureRef& OutVisibilityIdTexture,
	FRDGTextureRef& OutVisibilityDepthTexture)
{
	const uint32 MSAASampleCount = GetMaxSamplePerPixel();
	{
		{
			FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(Resolution, PF_R32_UINT, FClearValueBinding(EClearBinding::ENoneBound), TexCreate_NoFastClear | TexCreate_RenderTargetable | TexCreate_ShaderResource, 1, MSAASampleCount);
			OutVisibilityIdTexture = GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityIDTexture"));
		}

		AddClearGraphicPass(GraphBuilder, RDG_EVENT_NAME("HairStrands::ClearVisibilityMSAAIdTexture(%s)", TileData.IsValid()? TEXT("Tile") : TEXT("Screen")), ViewInfo, 0xFFFFFFFF, TileData, OutVisibilityIdTexture);

		FVisibilityPassParameters* PassParameters = GraphBuilder.AllocParameters<FVisibilityPassParameters>();
		PassParameters->UniformBuffer = CreatePassDummyTextures(GraphBuilder);
		PassParameters->RenderTargets[0] = FRenderTargetBinding(OutVisibilityIdTexture, ERenderTargetLoadAction::ELoad, 0);
		PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			OutVisibilityDepthTexture,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ENoAction,
			FExclusiveDepthStencil::DepthWrite_StencilNop);
		AddHairVisibilityCommonPass(GraphBuilder, Scene, ViewInfo, MacroGroupDatas, HairVisibilityRenderMode_MSAA_Visibility, PassParameters, InstanceCullingManager);
	}
}

static void AddHairVisibilityPPLLPass(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo* ViewInfo,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	const FIntPoint& Resolution,
	FInstanceCullingManager& InstanceCullingManager,
	FRDGTextureRef& InViewZDepthTexture,
	FRDGTextureRef& OutVisibilityPPLLNodeCounter,
	FRDGTextureRef& OutVisibilityPPLLNodeIndex,
	FRDGBufferRef&  OutVisibilityPPLLNodeData)
{
	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(FIntPoint(1,1), PF_R32_UINT, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource);
		OutVisibilityPPLLNodeCounter = GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityPPLLCounter"));
	}

	{
		FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(Resolution, PF_R32_UINT, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource);
		OutVisibilityPPLLNodeIndex = GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityPPLLNodeIndex"));
	}

	const FIntRect HairRect = ComputeVisibleHairStrandsMacroGroupsRect(ViewInfo->ViewRect, MacroGroupDatas);
	const FIntPoint EffectiveResolution(HairRect.Width(), HairRect.Height());

	const uint32 PPLLMaxTotalListElementCount = GetTotalSampleCountForAllocation(EffectiveResolution);
	{
		OutVisibilityPPLLNodeData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(PPLLNodeData), PPLLMaxTotalListElementCount), TEXT("Hair.VisibilityPPLLNodeData"));
	}
	const uint32 ClearValue0[4] = { 0,0,0,0 };
	const uint32 ClearValueInvalid[4] = { 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF };
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutVisibilityPPLLNodeCounter), ClearValue0);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(OutVisibilityPPLLNodeIndex), ClearValueInvalid);

	FVisibilityPassParameters* PassParameters = GraphBuilder.AllocParameters<FVisibilityPassParameters>();

	{
		FVisibilityPassUniformParameters* UniformParameters = GraphBuilder.AllocParameters<FVisibilityPassUniformParameters>();

		UniformParameters->PPLLCounter = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutVisibilityPPLLNodeCounter, 0));
		UniformParameters->PPLLNodeIndex = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OutVisibilityPPLLNodeIndex, 0));
		UniformParameters->PPLLNodeData = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(OutVisibilityPPLLNodeData));
		UniformParameters->MaxPPLLNodeCount = PPLLMaxTotalListElementCount;

		PassParameters->UniformBuffer = GraphBuilder.CreateUniformBuffer(UniformParameters);
	}

	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(InViewZDepthTexture, ERenderTargetLoadAction::ELoad, ERenderTargetLoadAction::ENoAction, FExclusiveDepthStencil::DepthRead_StencilNop);
	AddHairVisibilityCommonPass(GraphBuilder, Scene, ViewInfo, MacroGroupDatas, HairVisibilityRenderMode_PPLL, PassParameters, InstanceCullingManager);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

struct FHairPrimaryTransmittance
{
	FRDGTextureRef TransmittanceTexture = nullptr;
	FRDGTextureRef HairCountTexture = nullptr;

	FRDGTextureRef HairCountTextureUint = nullptr;
	FRDGTextureRef DepthTextureUint = nullptr;
};

static FHairPrimaryTransmittance AddHairViewTransmittancePass(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo* ViewInfo,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	const FIntPoint& Resolution,
	const bool bOutputHairCount,
	FRDGTextureRef SceneDepthTexture,
	FInstanceCullingManager& InstanceCullingManager)
{
	check(SceneDepthTexture->Desc.Extent == Resolution);
	const EHairVisibilityRenderMode RenderMode = bOutputHairCount ? HairVisibilityRenderMode_TransmittanceAndHairCount : HairVisibilityRenderMode_Transmittance;

	// Clear to transmittance 1
	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(Resolution, PF_R32_FLOAT, FClearValueBinding(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f)), TexCreate_RenderTargetable | TexCreate_ShaderResource);
	FVisibilityPassParameters* PassParameters = GraphBuilder.AllocParameters<FVisibilityPassParameters>();
	PassParameters->UniformBuffer = CreatePassDummyTextures(GraphBuilder);
	FHairPrimaryTransmittance Out;

	Out.TransmittanceTexture = GraphBuilder.CreateTexture(Desc, TEXT("Hair.ViewTransmittanceTexture"));
	PassParameters->RenderTargets[0] = FRenderTargetBinding(Out.TransmittanceTexture, ERenderTargetLoadAction::EClear, 0);

	if (RenderMode == HairVisibilityRenderMode_TransmittanceAndHairCount)
	{
		Desc.Format = PF_G32R32F;
		Desc.ClearValue = FClearValueBinding(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
		Out.HairCountTexture = GraphBuilder.CreateTexture(Desc, TEXT("Hair.ViewHairCountTexture"));
		PassParameters->RenderTargets[1] = FRenderTargetBinding(Out.HairCountTexture, ERenderTargetLoadAction::EClear, 0);
	}

	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(
		SceneDepthTexture,
		ERenderTargetLoadAction::ELoad,
		ERenderTargetLoadAction::ENoAction,
		FExclusiveDepthStencil::DepthRead_StencilNop);
	AddHairVisibilityCommonPass(GraphBuilder, Scene, ViewInfo, MacroGroupDatas, RenderMode, PassParameters, InstanceCullingManager);

	return Out;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Inject depth information into the view hair count texture, to block opaque occluder
class FHairViewTransmittanceDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairViewTransmittanceDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FHairViewTransmittanceDepthPS, FGlobalShader);

	class FOutputFormat : SHADER_PERMUTATION_INT("PERMUTATION_OUTPUT_FORMAT", 2);
	using FPermutationDomain = TShaderPermutationDomain<FOutputFormat>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, DistanceThreshold)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, CoverageTexture)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FOutputFormat>() == 0)
		{
			OutEnvironment.SetRenderTargetOutputFormat(0, PF_R32_FLOAT);
		}
		else if (PermutationVector.Get<FOutputFormat>() == 1)
		{
			OutEnvironment.SetRenderTargetOutputFormat(0, PF_G32R32F);
		}

	}
};

IMPLEMENT_GLOBAL_SHADER(FHairViewTransmittanceDepthPS, "/Engine/Private/HairStrands/HairStrandsVisibilityTransmittanceDepthPS.usf", "MainPS", SF_Pixel);

static void AddHairViewTransmittanceDepthPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FRDGTextureRef& CoverageTexture,
	const FRDGTextureRef& SceneDepthTexture,
	FRDGTextureRef& HairCountTexture)
{
	FHairViewTransmittanceDepthPS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairViewTransmittanceDepthPS::FParameters>();
	Parameters->DistanceThreshold = FMath::Max(1.f, GHairStrandsViewHairCountDepthDistanceThreshold);
	Parameters->CoverageTexture = CoverageTexture;
	Parameters->SceneDepthTexture = SceneDepthTexture;
	Parameters->ViewUniformBuffer = View.ViewUniformBuffer;
	Parameters->RenderTargets[0] = FRenderTargetBinding(HairCountTexture, ERenderTargetLoadAction::ELoad);

	FHairViewTransmittanceDepthPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairViewTransmittanceDepthPS::FOutputFormat>(HairCountTexture->Desc.Format == PF_G32R32F ? 1 : 0);

	TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FHairViewTransmittanceDepthPS> PixelShader(View.ShaderMap, PermutationVector);
	const FGlobalShaderMap* GlobalShaderMap = View.ShaderMap;
	const FIntRect Viewport = View.ViewRect;
	const FIntPoint Resolution = HairCountTexture->Desc.Extent;
	ClearUnusedGraphResources(PixelShader, Parameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrands::ViewTransmittanceDepth"),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, VertexShader, PixelShader, Viewport, Resolution](FRHICommandList& RHICmdList)
	{
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_One, BO_Add, BF_Zero, BF_Zero>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = TStaticDepthStencilState<false, CF_Always>::GetRHI();

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = VertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

		RHICmdList.SetViewport(Viewport.Min.X, Viewport.Min.Y, 0.0f, Viewport.Max.X, Viewport.Max.Y, 1.0f);
		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Parameters);
		DrawRectangle(
			RHICmdList,
			0, 0,
			Viewport.Width(), Viewport.Height(),
			Viewport.Min.X, Viewport.Min.Y,
			Viewport.Width(), Viewport.Height(),
			Viewport.Size(),
			Resolution,
			VertexShader,
			EDRF_UseTriangleOptimization);
	});
}


///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairVisibilityDepthPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairVisibilityDepthPS);
	SHADER_USE_PARAMETER_STRUCT(FHairVisibilityDepthPS, FGlobalShader);

	class FOutputType : SHADER_PERMUTATION_INT("PERMUTATION_OUTPUT_TYPE", 4);
	using FPermutationDomain = TShaderPermutationDomain<FOutputType>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairStrandsTilePassVS::FParameters, TileData)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER(uint32, bClear)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CoverageTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, HairSampleOffset)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPackedHairSample>, HairSampleData)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, OutLightChannelMaskTexture)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetRenderTargetOutputFormat(0, PF_B8G8R8A8);
		OutEnvironment.SetRenderTargetOutputFormat(1, PF_B8G8R8A8);
		OutEnvironment.SetRenderTargetOutputFormat(2, PF_FloatRGBA);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairVisibilityDepthPS, "/Engine/Private/HairStrands/HairStrandsVisibilityDepthPS.usf", "MainPS", SF_Pixel);

enum class EHairAuxilaryPassType
{
	GBufferPatch,
	GBufferPatch_LightChannelMask,
	LightChannelMask,
	DepthPatch,
	DepthClear
};

static void AddHairAuxilaryPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsTiles& TileData,
	const EHairAuxilaryPassType PassType,
	const FRDGTextureRef& CoverageTexture,
	const FRDGTextureRef& HairSampleOffset,
	const FRDGBufferRef& HairSampleData,
	FRDGTextureRef OutGBufferBTexture,
	FRDGTextureRef OutGBufferCTexture,
	FRDGTextureRef OutColorTexture,
	FRDGTextureRef OutDepthTexture,
	FRDGTextureRef OutLightChannelMaskTexture)
{
	const FHairStrandsTiles::ETileType TileType = (PassType == EHairAuxilaryPassType::DepthClear) ? FHairStrandsTiles::ETileType::Other : FHairStrandsTiles::ETileType::HairAll;

	FHairVisibilityDepthPS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairVisibilityDepthPS::FParameters>();
	Parameters->bClear = PassType == EHairAuxilaryPassType::DepthClear ? 1u : 0u;
	Parameters->TileData = GetHairStrandsTileParameters(View, TileData, TileType);
	Parameters->ViewUniformBuffer = View.ViewUniformBuffer;
	Parameters->CoverageTexture = CoverageTexture;
	Parameters->HairSampleOffset = HairSampleOffset;
	Parameters->HairSampleData = GraphBuilder.CreateSRV(HairSampleData);

	const bool bDepthTested = PassType != EHairAuxilaryPassType::LightChannelMask;
	if (bDepthTested)
	{
		Parameters->RenderTargets.DepthStencil = FDepthStencilBinding(
			OutDepthTexture,
			ERenderTargetLoadAction::ELoad,
			ERenderTargetLoadAction::ELoad,
			FExclusiveDepthStencil::DepthWrite_StencilNop);
	}

	if (PassType == EHairAuxilaryPassType::GBufferPatch || PassType == EHairAuxilaryPassType::GBufferPatch_LightChannelMask)
	{
		check(OutGBufferBTexture && OutGBufferCTexture && OutColorTexture);
		Parameters->RenderTargets[0] = FRenderTargetBinding(OutGBufferBTexture, ERenderTargetLoadAction::ELoad);
		Parameters->RenderTargets[1] = FRenderTargetBinding(OutGBufferCTexture, ERenderTargetLoadAction::ELoad);
		Parameters->RenderTargets[2] = FRenderTargetBinding(OutColorTexture, ERenderTargetLoadAction::ELoad);
	}

	if (PassType == EHairAuxilaryPassType::GBufferPatch_LightChannelMask || PassType == EHairAuxilaryPassType::LightChannelMask)
	{
		check(OutLightChannelMaskTexture);
		Parameters->OutLightChannelMaskTexture = GraphBuilder.CreateUAV(OutLightChannelMaskTexture);
	}

	uint32 OutputType = 0;
	const TCHAR* Method = nullptr;
	switch (PassType)
	{
		case EHairAuxilaryPassType::DepthPatch:						OutputType = 0; Method = TEXT("HairOnlyDepth"); break;
		case EHairAuxilaryPassType::DepthClear:						OutputType = 0; Method = TEXT("HairOnlyDepth:Clear"); break;
		case EHairAuxilaryPassType::GBufferPatch:					OutputType = 1; Method = TEXT("GBuffer"); break;
		case EHairAuxilaryPassType::LightChannelMask:				OutputType = 2; Method = TEXT("LightChannel"); break;
		case EHairAuxilaryPassType::GBufferPatch_LightChannelMask:	OutputType = 3; Method = TEXT("GBuffer, LightChannel"); break;
		default: 													OutputType = 0; Method = TEXT("Unknown"); break;
	};

	TShaderMapRef<FPostProcessVS> ScreenVertexShader(View.ShaderMap);
	TShaderMapRef<FHairStrandsTilePassVS> TileVertexShader(View.ShaderMap);

	FHairVisibilityDepthPS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairVisibilityDepthPS::FOutputType>(OutputType);
	TShaderMapRef<FHairVisibilityDepthPS> PixelShader(View.ShaderMap, PermutationVector);
	const FIntRect Viewport = View.ViewRect;
	const FIntPoint Resolution = OutDepthTexture->Desc.Extent; //-V522
	const bool bUseTile = TileData.IsValid();

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrands::AuxilaryPass(%s)(%s)", Method, bUseTile ? TEXT("Tile") : TEXT("Screen")),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, ScreenVertexShader, TileVertexShader, PixelShader, Viewport, Resolution, bUseTile, bDepthTested, TileType](FRHICommandList& RHICmdList)
	{
		FHairStrandsTilePassVS::FParameters ParametersVS = Parameters->TileData;

		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_One, BF_Zero, BO_Add, BF_One, BF_Zero>::GetRHI();
		GraphicsPSOInit.RasterizerState = TStaticRasterizerState<>::GetRHI();
		GraphicsPSOInit.DepthStencilState = bDepthTested ? 
			TStaticDepthStencilState<true, CF_Always>::GetRHI() : 
			TStaticDepthStencilState<false, CF_Always>::GetRHI();

		GraphicsPSOInit.BoundShaderState.VertexDeclarationRHI = GFilterVertexDeclaration.VertexDeclarationRHI;
		GraphicsPSOInit.BoundShaderState.VertexShaderRHI = bUseTile ? TileVertexShader.GetVertexShader() : ScreenVertexShader.GetVertexShader();
		GraphicsPSOInit.BoundShaderState.PixelShaderRHI = PixelShader.GetPixelShader();
		GraphicsPSOInit.PrimitiveType = Parameters->TileData.bRectPrimitive > 0 ? PT_RectList : PT_TriangleList;
		SetGraphicsPipelineState(RHICmdList, GraphicsPSOInit, 0);

		SetShaderParameters(RHICmdList, PixelShader, PixelShader.GetPixelShader(), *Parameters);

		RHICmdList.SetViewport(Viewport.Min.X, Viewport.Min.Y, 0.0f, Viewport.Max.X, Viewport.Max.Y, 1.0f);
		if (bUseTile)
		{
			SetShaderParameters(RHICmdList, TileVertexShader, TileVertexShader.GetVertexShader(), ParametersVS);
			RHICmdList.SetStreamSource(0, nullptr, 0);
			RHICmdList.DrawPrimitiveIndirect(Parameters->TileData.TileIndirectBuffer->GetRHI(), FHairStrandsTiles::GetIndirectDrawArgOffset(TileType));
		}
		else
		{
			DrawRectangle(
				RHICmdList,
				0, 0,
				Viewport.Width(), Viewport.Height(),
				Viewport.Min.X, Viewport.Min.Y,
				Viewport.Width(), Viewport.Height(),
				Viewport.Size(),
				Resolution,
				ScreenVertexShader,
				EDRF_UseTriangleOptimization);
		}
	});
}

#if RHI_RAYTRACING
static FRDGTextureRef CreateLigthtChannelMaskTexture(FRDGBuilder& GraphBuilder, const FIntPoint& Resolution)
{
	return GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Resolution, PF_R32_UINT, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource), TEXT("Hair.LightChannelMask"));
}

static FRDGTextureRef AddHairLightChannelMaskPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsTiles& TileData,
	const FRDGTextureRef& CoverageTexture,
	const FRDGTextureRef& HairSampleOffset,
	const FRDGBufferRef& HairSampleData,
	const FRDGTextureRef& SceneDepthTexture)
{
	check(IsRayTracingEnabled());
	FRDGTextureRef OutLightChannelMask = CreateLigthtChannelMaskTexture(GraphBuilder, View.ViewRect.Size());

	AddHairAuxilaryPass(
		GraphBuilder,
		View,
		TileData,
		EHairAuxilaryPassType::LightChannelMask,
		CoverageTexture,
		HairSampleOffset,
		HairSampleData,
		nullptr,
		nullptr,
		nullptr,
		SceneDepthTexture,
		OutLightChannelMask);
	return OutLightChannelMask;
}
#endif


static void AddHairGbufferPatchPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsTiles& TileData,
	const FRDGTextureRef& CoverageTexture,
	const FRDGTextureRef& HairSampleOffset,
	const FRDGBufferRef& HairSampleData,
	FRDGTextureRef& OutGBufferBTexture,
	FRDGTextureRef& OutGBufferCTexture,
	FRDGTextureRef& OutColorTexture,
	FRDGTextureRef& OutDepthTexture,
	FRDGTextureRef& OutLightChannelMask)
{
	if (!OutGBufferBTexture || !OutGBufferCTexture || !OutColorTexture || !OutDepthTexture)
	{
		return;
	}

#if RHI_RAYTRACING
	const bool bLightingChannel = IsRayTracingEnabled() && OutLightChannelMask == nullptr;
	if (bLightingChannel)
	{
		OutLightChannelMask = CreateLigthtChannelMaskTexture(GraphBuilder, View.ViewRect.Size());
	}
#else
	const bool bLightingChannel = false;
#endif

	AddHairAuxilaryPass(
		GraphBuilder,
		View,
		TileData,
		bLightingChannel ? EHairAuxilaryPassType::GBufferPatch_LightChannelMask : EHairAuxilaryPassType::GBufferPatch,
		CoverageTexture,
		HairSampleOffset,
		HairSampleData,
		OutGBufferBTexture,
		OutGBufferCTexture,
		OutColorTexture,
		OutDepthTexture,
		OutLightChannelMask);
}

static void AddHairOnlyDepthPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsTiles& TileData,
	const FRDGTextureRef& CoverageTexture,
	const FRDGTextureRef& HairSampleOffset,
	const FRDGBufferRef& HairSampleData,
	FRDGTextureRef& OutDepthTexture)
{
	if (!OutDepthTexture)
	{
		return;
	}

	// If tile data are available, we dispatch a complementary set of tile to clear non-hair tile
	// If tile data are not available, then the clearly is done prior to that.
	if (TileData.IsValid())
	{
		AddHairAuxilaryPass(
			GraphBuilder,
			View,
			TileData,
			EHairAuxilaryPassType::DepthClear,
			CoverageTexture,
			HairSampleOffset,
			HairSampleData,
			nullptr,
			nullptr,
			nullptr,
			OutDepthTexture,
			nullptr);
	}

	// Depth value
	AddHairAuxilaryPass(
		GraphBuilder,
		View,
		TileData,
		EHairAuxilaryPassType::DepthPatch,
		CoverageTexture,
		HairSampleOffset,
		HairSampleData,
		nullptr,
		nullptr,
		nullptr,
		OutDepthTexture,
		nullptr);
}

static void AddHairOnlyHZBPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef HairDepthTexture,
	FRDGTextureRef& OutClosestHZBTexture,
	FRDGTextureRef& OutFurthestHZBTexture)
{
	BuildHZB(
		GraphBuilder,
		HairDepthTexture,
		/* VisBufferTexture = */ nullptr,
		View.ViewRect,
		View.GetFeatureLevel(),
		View.GetShaderPlatform(),
		TEXT("HZBHairClosest"),
		&OutClosestHZBTexture,
		TEXT("HZBHairFurthest"),
		&OutFurthestHZBTexture);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

class FHairCountToCoverageCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairCountToCoverageCS);
	SHADER_USE_PARAMETER_STRUCT(FHairCountToCoverageCS, FGlobalShader);

	class FInputType : SHADER_PERMUTATION_INT("PERMUTATION_INPUT_TYPE", 2);
	using FPermutationDomain = TShaderPermutationDomain<FInputType>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, OutputResolution)
		SHADER_PARAMETER(float, LUT_HairCount)
		SHADER_PARAMETER(float, LUT_HairRadiusCount)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCoverageLUT)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCountTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairCountToCoverageCS, "/Engine/Private/HairStrands/HairStrandsCoverage.usf", "MainCS", SF_Compute);

static FRDGTextureRef AddHairHairCountToTransmittancePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& ViewInfo,
	const FRDGTextureRef HairCountTexture)
{
	const FIntPoint OutputResolution = HairCountTexture->Desc.Extent;

	check(HairCountTexture->Desc.Format == PF_R32_UINT || HairCountTexture->Desc.Format == PF_G32R32F)
	const bool bUseOneChannel = HairCountTexture->Desc.Format == PF_R32_UINT;

	FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(OutputResolution, PF_R32_FLOAT, FClearValueBinding(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f)), TexCreate_UAV | TexCreate_ShaderResource | TexCreate_RenderTargetable);
	FRDGTextureRef OutputTexture = GraphBuilder.CreateTexture(Desc, TEXT("Hair.VisibilityTexture"));
	FRDGTextureRef HairCoverageLUT = GetHairLUT(GraphBuilder, ViewInfo, HairLUTType_Coverage);

	FHairCountToCoverageCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHairCountToCoverageCS::FParameters>();
	PassParameters->LUT_HairCount = HairCoverageLUT->Desc.Extent.X;
	PassParameters->LUT_HairRadiusCount = HairCoverageLUT->Desc.Extent.Y;
	PassParameters->OutputResolution = OutputResolution;
	PassParameters->HairCoverageLUT = HairCoverageLUT;
	PassParameters->HairCountTexture = HairCountTexture;
	PassParameters->LinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	PassParameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);

	FHairCountToCoverageCS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FHairCountToCoverageCS::FInputType>(bUseOneChannel ? 1 : 0);
	TShaderMapRef<FHairCountToCoverageCS> ComputeShader(ViewInfo.ShaderMap, PermutationVector);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HairCountToTransmittancePass"), ComputeShader, PassParameters, FComputeShaderUtils::GetGroupCount(OutputResolution, FIntPoint(8,8)));

	return OutputTexture;
}

// Transit resources used during the MeshDraw passes
void AddMeshDrawTransitionPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& ViewInfo,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas)
{
	for (const FHairStrandsMacroGroupData& MacroGroup : MacroGroupDatas)
	{
		for (const FHairStrandsMacroGroupData::PrimitiveInfo& PrimitiveInfo : MacroGroup.PrimitivesInfos)
		{
			FHairGroupPublicData* HairGroupPublicData = PrimitiveInfo.PublicDataPtr;
			check(HairGroupPublicData);

			FRDGResourceAccessFinalizer ResourceAccessFinalizer;

			FHairGroupPublicData::FVertexFactoryInput& VFInput = HairGroupPublicData->VFInput;
			ResourceAccessFinalizer.AddBuffer(VFInput.Strands.PositionBuffer.Buffer,			ERHIAccess::SRVMask);
			ResourceAccessFinalizer.AddBuffer(VFInput.Strands.PrevPositionBuffer.Buffer,		ERHIAccess::SRVMask);
			ResourceAccessFinalizer.AddBuffer(VFInput.Strands.TangentBuffer.Buffer,				ERHIAccess::SRVMask);
			ResourceAccessFinalizer.AddBuffer(VFInput.Strands.Attribute0Buffer.Buffer,			ERHIAccess::SRVMask);
			ResourceAccessFinalizer.AddBuffer(VFInput.Strands.Attribute1Buffer.Buffer,			ERHIAccess::SRVMask);
			ResourceAccessFinalizer.AddBuffer(VFInput.Strands.MaterialBuffer.Buffer,			ERHIAccess::SRVMask);
			ResourceAccessFinalizer.AddBuffer(VFInput.Strands.PositionOffsetBuffer.Buffer,		ERHIAccess::SRVMask);
			ResourceAccessFinalizer.AddBuffer(VFInput.Strands.PrevPositionOffsetBuffer.Buffer,	ERHIAccess::SRVMask);

			FRDGBufferRef CulledVertexIdBuffer = Register(GraphBuilder, HairGroupPublicData->CulledVertexIdBuffer, ERDGImportedBufferFlags::None).Buffer;
			FRDGBufferRef CulledVertexRadiusScaleBuffer = Register(GraphBuilder, HairGroupPublicData->CulledVertexRadiusScaleBuffer, ERDGImportedBufferFlags::None).Buffer;
			FRDGBufferRef DrawIndirectBuffer = Register(GraphBuilder, HairGroupPublicData->DrawIndirectBuffer, ERDGImportedBufferFlags::None).Buffer;
			ResourceAccessFinalizer.AddBuffer(CulledVertexIdBuffer,								ERHIAccess::SRVMask);
			ResourceAccessFinalizer.AddBuffer(CulledVertexRadiusScaleBuffer,					ERHIAccess::SRVMask);
			ResourceAccessFinalizer.AddBuffer(DrawIndirectBuffer,								ERHIAccess::IndirectArgs);

			ResourceAccessFinalizer.Finalize(GraphBuilder);

			VFInput.Strands.PositionBuffer				= FRDGImportedBuffer();
			VFInput.Strands.PrevPositionBuffer			= FRDGImportedBuffer();
			VFInput.Strands.TangentBuffer				= FRDGImportedBuffer();
			VFInput.Strands.Attribute0Buffer			= FRDGImportedBuffer();
			VFInput.Strands.Attribute1Buffer			= FRDGImportedBuffer();
			VFInput.Strands.MaterialBuffer				= FRDGImportedBuffer();
			VFInput.Strands.PositionOffsetBuffer		= FRDGImportedBuffer();
			VFInput.Strands.PrevPositionOffsetBuffer	= FRDGImportedBuffer();
		}
	}
}
///////////////////////////////////////////////////////////////////////////////////////////////////

class FVisiblityRasterComputeCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVisiblityRasterComputeCS);
	SHADER_USE_PARAMETER_STRUCT(FVisiblityRasterComputeCS, FGlobalShader);

	class FGroupSize : SHADER_PERMUTATION_SPARSE_INT("PERMUTATION_GROUP_SIZE", 32, 64);
	class FRasterAtomic : SHADER_PERMUTATION_INT("PERMUTATION_RASTER_ATOMIC", 4);
	class FSPP : SHADER_PERMUTATION_SPARSE_INT("PERMUTATION_SPP", 1, 2, 4); 
	class FCulling : SHADER_PERMUTATION_BOOL("PERMUTATION_CULLING");
	class FStochastic : SHADER_PERMUTATION_BOOL("PERMUTATION_STOCHASTIC");
	using FPermutationDomain = TShaderPermutationDomain<FRasterAtomic, FSPP, FCulling, FStochastic, FGroupSize>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MacroGroupId)
		SHADER_PARAMETER(uint32, DispatchCountX)
		SHADER_PARAMETER(uint32, MaxRasterCount)
		SHADER_PARAMETER(uint32, FrameIdMod8)
		SHADER_PARAMETER(uint32, HairMaterialId)
		SHADER_PARAMETER(uint32, ResolutionMultiplier)
		SHADER_PARAMETER(FIntPoint, OutputResolution)
		SHADER_PARAMETER(uint32, HairStrandsVF_bIsCullingEnable)
		SHADER_PARAMETER(float, HairStrandsVF_Density)
		SHADER_PARAMETER(float, HairStrandsVF_Radius)
		SHADER_PARAMETER(float, HairStrandsVF_RootScale)
		SHADER_PARAMETER(float, HairStrandsVF_TipScale)
		SHADER_PARAMETER(float, HairStrandsVF_Length)
		SHADER_PARAMETER(uint32, HairStrandsVF_bUseStableRasterization)
		SHADER_PARAMETER(uint32, HairStrandsVF_VertexCount)
		SHADER_PARAMETER(FMatrix44f, HairStrandsVF_LocalToWorldPrimitiveTransform)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, HairStrandsVF_PositionBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, HairStrandsVF_PositionOffsetBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, HairStrandsVF_CullingIndirectBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, HairStrandsVF_CullingIndexBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, HairStrandsVF_CullingRadiusScaleBuffer)
		RDG_BUFFER_ACCESS(IndirectBufferArgs, ERHIAccess::IndirectArgs)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutHairCountTexture)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutVisibilityTexture0)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutVisibilityTexture1)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutVisibilityTexture2)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutVisibilityTexture3)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) 
	{ 
		//if (!FDataDrivenShaderPlatformInfo::GetSupportsUInt64ImageAtomics(Parameters.Platform))
		//{
		//	return false;
		//}
		if (IsVulkanPlatform(Parameters.Platform))
		{
			return false;
		}

		if (!IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform))
		{
			return false;
		}

		if (IsPCPlatform(Parameters.Platform))
		{
			FPermutationDomain PermutationVector(Parameters.PermutationId);
			return PermutationVector.Get<FRasterAtomic>() != 0;
		}
		else
		{
			FPermutationDomain PermutationVector(Parameters.PermutationId);
			return PermutationVector.Get<FRasterAtomic>() == 0;
		}
	}
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_RASTERCOMPUTE"), 1);
		// Need to force optimization for driver injection to work correctly.
		// https://developer.nvidia.com/unlocking-gpu-intrinsics-hlsl
		// https://gpuopen.com/gcn-shader-extensions-for-direct3d-and-vulkan/
		OutEnvironment.CompilerFlags.Add(CFLAG_ForceOptimization);

		FPermutationDomain PermutationVector(Parameters.PermutationId);
		if (PermutationVector.Get<FRasterAtomic>() == 3) // AMD, DX12
		{
			// Force shader model 6.0+
			OutEnvironment.CompilerFlags.Add(CFLAG_ForceDXC);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FVisiblityRasterComputeCS, "/Engine/Private/HairStrands/HairStrandsVisibilityRasterCompute.usf", "MainCS", SF_Compute);

static FRasterComputeOutput AddVisibilityComputeRasterPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& ViewInfo,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	const FIntPoint& InResolution,
	const uint32 SamplePerPixelCount,
	const FRDGTextureRef SceneDepthTexture)
{	
	check(DoesSupportRasterCompute());

	FRasterComputeOutput Out;

	Out.ResolutionMultiplier = 1;
	Out.BaseResolution		 = InResolution;
	Out.SuperResolution		 = InResolution * Out.ResolutionMultiplier;
	Out.VisibilityTexture0	 = nullptr;
	Out.VisibilityTexture1	 = nullptr;
	Out.VisibilityTexture2	 = nullptr;
	Out.VisibilityTexture3	 = nullptr;

	FRDGTextureDesc DescCount = FRDGTextureDesc::Create2D(Out.SuperResolution, PF_R32_UINT, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource | TexCreate_RenderTargetable);
	FRDGTextureDesc DescVis   = FRDGTextureDesc::Create2D(Out.SuperResolution, PF_R32G32_UINT, FClearValueBinding::None, TexCreate_UAV | TexCreate_ShaderResource | TexCreate_RenderTargetable);
	FRDGTextureUAVRef VisibilityTexture0UAV = nullptr;
	FRDGTextureUAVRef VisibilityTexture1UAV = nullptr;
	FRDGTextureUAVRef VisibilityTexture2UAV = nullptr;
	FRDGTextureUAVRef VisibilityTexture3UAV = nullptr;

	uint32 ClearValues[4] = { 0,0,0,0 };
	Out.HairCountTexture = GraphBuilder.CreateTexture(DescCount, TEXT("Hair.ViewTransmittanceTexture"));
	FRDGTextureUAVRef HairCountTextureUAV = GraphBuilder.CreateUAV(Out.HairCountTexture);
	AddClearUAVPass(GraphBuilder, HairCountTextureUAV, ClearValues);

	Out.VisibilityTexture0 = GraphBuilder.CreateTexture(DescVis, TEXT("Hair.VisibilityTexture0"));
	VisibilityTexture0UAV = GraphBuilder.CreateUAV(Out.VisibilityTexture0);
	AddClearUAVPass(GraphBuilder, VisibilityTexture0UAV, ClearValues);
	if (SamplePerPixelCount > 1)
	{
		Out.VisibilityTexture1 = GraphBuilder.CreateTexture(DescVis, TEXT("Hair.VisibilityTexture1"));
		VisibilityTexture1UAV = GraphBuilder.CreateUAV(Out.VisibilityTexture1);
		AddClearUAVPass(GraphBuilder, VisibilityTexture1UAV, ClearValues);
		if (SamplePerPixelCount > 2)
		{
			Out.VisibilityTexture2 = GraphBuilder.CreateTexture(DescVis, TEXT("Hair.VisibilityTexture2"));
			VisibilityTexture2UAV = GraphBuilder.CreateUAV(Out.VisibilityTexture2);
			AddClearUAVPass(GraphBuilder, VisibilityTexture2UAV, ClearValues);
			if (SamplePerPixelCount > 3)
			{
				Out.VisibilityTexture3 = GraphBuilder.CreateTexture(DescVis, TEXT("Hair.VisibilityTexture3"));
				VisibilityTexture3UAV = GraphBuilder.CreateUAV(Out.VisibilityTexture3);
				AddClearUAVPass(GraphBuilder, VisibilityTexture3UAV, ClearValues);
			}
		}
	}

	// Create and set the uniform buffer
	const bool bStochasticRaster = GHairVisibilityComputeRaster_Stochastic > 0;
	const bool bEnableMSAA = false;
	TUniformBufferRef<FViewUniformShaderParameters> ViewUniformShaderParameters;
	SetUpViewHairRenderInfo(ViewInfo, bEnableMSAA, ViewInfo.CachedViewUniformShaderParameters->HairRenderInfo, ViewInfo.CachedViewUniformShaderParameters->HairRenderInfoBits, ViewInfo.CachedViewUniformShaderParameters->HairComponents);
	ViewUniformShaderParameters = TUniformBufferRef<FViewUniformShaderParameters>::CreateUniformBufferImmediate(*ViewInfo.CachedViewUniformShaderParameters, UniformBuffer_SingleFrame);

	const uint32 FrameIdMode8 = ViewInfo.ViewState ? (ViewInfo.ViewState->GetFrameIndex() % 8) : 0;
	const uint32 GroupSize = GetVendorOptimalGroupSize1D();

	FVisiblityRasterComputeCS::FPermutationDomain PermutationVector0;
	FVisiblityRasterComputeCS::FPermutationDomain PermutationVector1;
#if PLATFORM_WINDOWS
	if (IsRHIDeviceNVIDIA())
	{
		PermutationVector0.Set<FVisiblityRasterComputeCS::FRasterAtomic>(1);
	}
	else if (IsRHIDeviceAMD())
	{
		static const bool bIsDx12 = FCString::Strcmp(GDynamicRHI->GetName(), TEXT("D3D12")) == 0;
		PermutationVector0.Set<FVisiblityRasterComputeCS::FRasterAtomic>(bIsDx12 ? 2 : 3);
	}
#else
	{
		PermutationVector0.Set<FVisiblityRasterComputeCS::FRasterAtomic>(0);
	}
#endif
	PermutationVector0.Set<FVisiblityRasterComputeCS::FStochastic>(bStochasticRaster);
	PermutationVector0.Set<FVisiblityRasterComputeCS::FSPP>(SamplePerPixelCount);
	PermutationVector0.Set<FVisiblityRasterComputeCS::FGroupSize>(GroupSize);
	PermutationVector1 = PermutationVector0; 

	PermutationVector0.Set<FVisiblityRasterComputeCS::FCulling>(false);
	PermutationVector1.Set<FVisiblityRasterComputeCS::FCulling>(true);
	TShaderMapRef<FVisiblityRasterComputeCS> ComputeShader_CullingOff(ViewInfo.ShaderMap, PermutationVector0);
	TShaderMapRef<FVisiblityRasterComputeCS> ComputeShader_CullingOn (ViewInfo.ShaderMap, PermutationVector1);

	for (const FHairStrandsMacroGroupData& MacroGroup : MacroGroupDatas)
	{
		const FHairStrandsMacroGroupData::TPrimitiveInfos& PrimitiveSceneInfos = MacroGroup.PrimitivesInfos;

		for (const FHairStrandsMacroGroupData::PrimitiveInfo& PrimitiveInfo : PrimitiveSceneInfos)
		{
			FVisiblityRasterComputeCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FVisiblityRasterComputeCS::FParameters>();
			PassParameters->OutputResolution = Out.SuperResolution;
			PassParameters->ResolutionMultiplier = Out.ResolutionMultiplier;
			PassParameters->MacroGroupId = MacroGroup.MacroGroupId;
			PassParameters->DispatchCountX = 1;
			PassParameters->MaxRasterCount = FMath::Clamp(GHairVisibilityComputeRaster_MaxPixelCount, 1, 256);			
			PassParameters->FrameIdMod8 = FrameIdMode8;
			PassParameters->HairMaterialId = PrimitiveInfo.MaterialId;
			PassParameters->ViewUniformBuffer = ViewUniformShaderParameters;
			PassParameters->SceneDepthTexture = SceneDepthTexture;
			PassParameters->OutHairCountTexture = HairCountTextureUAV;
			PassParameters->OutVisibilityTexture0 = VisibilityTexture0UAV;
			PassParameters->OutVisibilityTexture1 = VisibilityTexture1UAV;
			PassParameters->OutVisibilityTexture2 = VisibilityTexture2UAV;
			PassParameters->OutVisibilityTexture3 = VisibilityTexture3UAV;
			
			check(PrimitiveInfo.PublicDataPtr);
			const FHairGroupPublicData* HairGroupPublicData = PrimitiveInfo.PublicDataPtr;

			const FHairGroupPublicData::FVertexFactoryInput& VFInput = HairGroupPublicData->VFInput;
			PassParameters->HairStrandsVF_PositionBuffer	= VFInput.Strands.PositionBuffer.SRV;
			PassParameters->HairStrandsVF_PositionOffsetBuffer = VFInput.Strands.PositionOffsetBuffer.SRV;
			PassParameters->HairStrandsVF_VertexCount		= VFInput.Strands.VertexCount;
			PassParameters->HairStrandsVF_Radius			= VFInput.Strands.HairRadius;
			PassParameters->HairStrandsVF_RootScale			= VFInput.Strands.HairRootScale;
			PassParameters->HairStrandsVF_TipScale			= VFInput.Strands.HairTipScale;
			PassParameters->HairStrandsVF_Length			= VFInput.Strands.HairLength;
			PassParameters->HairStrandsVF_bUseStableRasterization = VFInput.Strands.bUseStableRasterization ? 1 : 0;
			PassParameters->HairStrandsVF_Density			= VFInput.Strands.HairDensity;
			PassParameters->HairStrandsVF_LocalToWorldPrimitiveTransform = FMatrix44f(VFInput.LocalToWorldTransform.ToMatrixWithScale()); // LWC_TODO: Precision loss

			const bool bCullingEnable = HairGroupPublicData->GetCullingResultAvailable();
			if (bCullingEnable)
			{
				FRDGImportedBuffer CullingIndirectBuffer = Register(GraphBuilder, HairGroupPublicData->GetDrawIndirectRasterComputeBuffer(), ERDGImportedBufferFlags::CreateSRV);
				PassParameters->HairStrandsVF_CullingIndirectBuffer		= CullingIndirectBuffer.SRV;
				PassParameters->HairStrandsVF_bIsCullingEnable			= bCullingEnable ? 1 : 0;
				PassParameters->HairStrandsVF_CullingIndexBuffer		= RegisterAsSRV(GraphBuilder, HairGroupPublicData->GetCulledVertexIdBuffer());
				PassParameters->HairStrandsVF_CullingRadiusScaleBuffer	= RegisterAsSRV(GraphBuilder, HairGroupPublicData->GetCulledVertexRadiusScaleBuffer());
				PassParameters->IndirectBufferArgs						= CullingIndirectBuffer.Buffer;

				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HairStrandsVisibilityComputeRaster(culling=on)"), ComputeShader_CullingOn, PassParameters, CullingIndirectBuffer.Buffer, 0);
			}
			else
			{
				const FIntVector DispatchCount = ComputeDispatchCount(PassParameters->HairStrandsVF_VertexCount, GroupSize);
				PassParameters->DispatchCountX = DispatchCount.X;
				FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HairStrandsVisibilityComputeRaster(culling=off)"), ComputeShader_CullingOff, PassParameters, DispatchCount);
			}
		}
	}

	return Out;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Selection outline

class FHairStrandsEmitSelectionPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairStrandsEmitSelectionPS);
	SHADER_USE_PARAMETER_STRUCT(FHairStrandsEmitSelectionPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, CoverageThreshold)
		SHADER_PARAMETER(FVector2f, InvViewportResolution)
		SHADER_PARAMETER(uint32, MaxMaterialCount)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VisNodeIndex)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<float>, CoverageTexture)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPackedHairVis>, VisNodeData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, SelectionMaterialIdBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

		static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_SELECTION"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FHairStrandsEmitSelectionPS, "/Engine/Private/HairStrands/HairStrandsHitProxy.usf", "EmitPS", SF_Pixel);

void AddHairStrandsSelectionOutlinePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FIntRect& ViewportRect,
	FRDGTextureRef VisNodeIndex,
	FRDGBufferRef VisNodeData,
	FRDGTextureRef CoverageTexture,
	FRDGTextureRef SelectionDepthTexture)
{
	if (View.HairStrandsMeshElements.Num() == 0 || !VisNodeData)
	{
		return;
	}

#if WITH_EDITOR
	// Create mapping table between PrimitiveId and BatchId
	TArray<uint32> SelectionMaterialId;
	SelectionMaterialId.Reserve(View.HairStrandsMeshElements.Num());
	for (const FMeshBatchAndRelevance& MeshBatch : View.HairStrandsMeshElements)
	{
		const uint32 bSelected = MeshBatch.PrimitiveSceneProxy->IsSelected() ? 1u : 0u;
		SelectionMaterialId.Add(bSelected);
	}

	FRDGBufferRef SelectionMaterialIdBuffer = CreateUploadBuffer(GraphBuilder, TEXT("Hair.MaterialIdToHitProxyIdBuffer"), sizeof(uint32), SelectionMaterialId.Num(), SelectionMaterialId.GetData(), sizeof(uint32) * SelectionMaterialId.Num());
	auto* PassParameters = GraphBuilder.AllocParameters<FHairStrandsEmitSelectionPS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->CoverageThreshold = FMath::Clamp(GHairStrands_Selection_CoverageThreshold, 0.f, 1.f);
	PassParameters->MaxMaterialCount = SelectionMaterialId.Num();
	PassParameters->InvViewportResolution = FVector2f(1.f/ViewportRect.Width(), 1.f/ViewportRect.Height());
	PassParameters->VisNodeIndex = VisNodeIndex;
	PassParameters->VisNodeData = GraphBuilder.CreateSRV(VisNodeData);
	PassParameters->CoverageTexture = CoverageTexture;
	PassParameters->SelectionMaterialIdBuffer = GraphBuilder.CreateSRV(SelectionMaterialIdBuffer, PF_R32_UINT);
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(SelectionDepthTexture, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthWrite_StencilWrite);

	auto PixelShader = View.ShaderMap->GetShader<FHairStrandsEmitSelectionPS>();

	const uint32 StencilRef = 3;
	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		View.ShaderMap,
		RDG_EVENT_NAME("HairStrands::EmitSelection"),
		PixelShader,
		PassParameters,
		ViewportRect,
		TStaticBlendState<>::GetRHI(),
		TStaticRasterizerState<>::GetRHI(),
		TStaticDepthStencilState<true, CF_DepthNearOrEqual, true, CF_Always, SO_Keep, SO_Keep, SO_Replace>::GetRHI(), 
		StencilRef);
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// HitProxyId

class FHairStrandsEmitHitProxyIdPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairStrandsEmitHitProxyIdPS);
	SHADER_USE_PARAMETER_STRUCT(FHairStrandsEmitHitProxyIdPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, MaxMaterialCount)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, VisNodeIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<FPackedHairVis>, VisNodeData)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, MaterialIdToHitProxyIdBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_HITPROXY_ID"), 1);
	}
};
IMPLEMENT_GLOBAL_SHADER(FHairStrandsEmitHitProxyIdPS, "/Engine/Private/HairStrands/HairStrandsHitProxy.usf", "EmitPS", SF_Pixel);

void AddHairStrandsHitProxyIdPass(
	FRDGBuilder& GraphBuilder,
	const FScene& Scene,
	const FViewInfo& View,
	FRDGTextureRef VisNodeIndex,
	FRDGBufferRef VisNodeData,
	FRDGTextureRef HitProxyTexture,
	FRDGTextureRef HitProxyDepthTexture)
{
#if WITH_EDITOR
	if (View.HairStrandsMeshElements.Num() == 0 || !VisNodeData)
	{
		return;
	}

	// Create mapping table between PrimitiveId and BatchId
	TArray<uint32> MaterialIdToHitProxyId;
	MaterialIdToHitProxyId.Reserve(View.HairStrandsMeshElements.Num());
	for (const FMeshBatchAndRelevance& MeshBatch : View.HairStrandsMeshElements)
	{
		uint32 HitColor = MeshBatch.Mesh->BatchHitProxyId.GetColor().DWColor();
		MaterialIdToHitProxyId.Add(HitColor);
	}

	FRDGBufferRef MaterialIdToHitProxyIdBuffer = CreateUploadBuffer(GraphBuilder, TEXT("Hair.MaterialIdToHitProxyIdBuffer"), sizeof(uint32), MaterialIdToHitProxyId.Num(), MaterialIdToHitProxyId.GetData(), sizeof(uint32) * MaterialIdToHitProxyId.Num());
	auto* PassParameters = GraphBuilder.AllocParameters<FHairStrandsEmitHitProxyIdPS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->MaxMaterialCount = MaterialIdToHitProxyId.Num();
	PassParameters->VisNodeIndex = VisNodeIndex;
	PassParameters->VisNodeData = GraphBuilder.CreateSRV(VisNodeData);
	PassParameters->MaterialIdToHitProxyIdBuffer = GraphBuilder.CreateSRV(MaterialIdToHitProxyIdBuffer, PF_R32_UINT);
	PassParameters->RenderTargets[0] = FRenderTargetBinding(HitProxyTexture, ERenderTargetLoadAction::ELoad);
	PassParameters->RenderTargets.DepthStencil = FDepthStencilBinding(HitProxyDepthTexture, ERenderTargetLoadAction::ELoad, FExclusiveDepthStencil::DepthWrite_StencilWrite);

	auto PixelShader = View.ShaderMap->GetShader<FHairStrandsEmitHitProxyIdPS>();

	FPixelShaderUtils::AddFullscreenPass(
		GraphBuilder,
		View.ShaderMap,
		RDG_EVENT_NAME("HairStrands::EmitHitProxyId"),
		PixelShader,
		PassParameters,
		View.ViewRect,
		TStaticBlendState<>::GetRHI(),
		TStaticRasterizerState<>::GetRHI(),
		TStaticDepthStencilState<true, CF_DepthNearOrEqual>::GetRHI());
#endif
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Groom comparison
class FHairStrandsPositionChangedCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairStrandsPositionChangedCS);
	SHADER_USE_PARAMETER_STRUCT(FHairStrandsPositionChangedCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(uint32, VertexCount)
		SHADER_PARAMETER(uint32, DispatchCountX)
		SHADER_PARAMETER(float, PositionThreshold2)
		SHADER_PARAMETER(uint32, HairStrandsVF_bIsCullingEnable)
		SHADER_PARAMETER(uint32, bDrawInvalidElement)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, HairStrandsVF_CullingIndexBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, HairStrandsVF_CullingIndirectBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint4>, CurrPositionBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint4>, PrevPositionBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<int>, GroupAABBBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, InvalidationBuffer)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, InvalidationPrintCounter)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderDrawDebug::FShaderParameters, ShaderDrawParameters)
	END_SHADER_PARAMETER_STRUCT()
public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		OutEnvironment.SetDefine(TEXT("SHADER_POSITION_CHANGED"), TEXT("1"));
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairStrandsPositionChangedCS, "/Engine/Private/HairStrands/HairStrandsRaytracingGeometry.usf", "MainCS", SF_Compute);

static void AddHairStrandsHasPositionChangedPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo* View,
	const FHairGroupPublicData* HairGroupPublicData,
	FRDGBufferUAVRef InvalidationBuffer)
{
	const uint32 VertexCount = HairGroupPublicData->VertexCount;
	const uint32 GroupSize = 64;
	const FIntVector DispatchCount = ComputeDispatchCount(VertexCount, GroupSize);

	FRDGBufferRef InvalidationPrintCounter = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("Hair.InvalidationPrintCounter"));
	FRDGBufferUAVRef InvalidationPrintCounterUAV = GraphBuilder.CreateUAV(InvalidationPrintCounter, PF_R32_UINT);
	AddClearUAVPass(GraphBuilder, InvalidationPrintCounterUAV, 0u);

	FHairStrandsPositionChangedCS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairStrandsPositionChangedCS::FParameters>();
	Parameters->VertexCount = VertexCount;
	Parameters->DispatchCountX = DispatchCount.X;
	Parameters->PositionThreshold2 = FMath::Square(GHairStrands_InvalidationPosition_Threshold);
	Parameters->bDrawInvalidElement = GHairStrands_InvalidationPosition_Debug > 0 ? 1u : 0u;
	Parameters->HairStrandsVF_bIsCullingEnable = 0u;
	Parameters->HairStrandsVF_CullingIndexBuffer = GraphBuilder.CreateSRV(GSystemTextures.GetDefaultBuffer(GraphBuilder, 4, 0u), PF_R32_UINT);
	Parameters->HairStrandsVF_CullingIndirectBuffer = Parameters->HairStrandsVF_CullingIndexBuffer;
	Parameters->CurrPositionBuffer = HairGroupPublicData->VFInput.Strands.PositionBuffer.SRV;
	Parameters->PrevPositionBuffer = HairGroupPublicData->VFInput.Strands.PrevPositionBuffer.SRV;
	Parameters->GroupAABBBuffer = Register(GraphBuilder, HairGroupPublicData->GetGroupAABBBuffer(), ERDGImportedBufferFlags::CreateSRV).SRV;
	Parameters->InvalidationBuffer = InvalidationBuffer;
	Parameters->InvalidationPrintCounter = InvalidationPrintCounterUAV;
	ShaderDrawDebug::SetParameters(GraphBuilder, View->ShaderDrawData, Parameters->ShaderDrawParameters);
	if (HairGroupPublicData->GetCullingResultAvailable())
	{
		Parameters->HairStrandsVF_CullingIndexBuffer = Register(GraphBuilder, HairGroupPublicData->GetCulledVertexIdBuffer(), ERDGImportedBufferFlags::CreateSRV).SRV;
		Parameters->HairStrandsVF_CullingIndirectBuffer = Register(GraphBuilder, HairGroupPublicData->GetDrawIndirectRasterComputeBuffer(), ERDGImportedBufferFlags::CreateSRV).SRV;
		Parameters->HairStrandsVF_bIsCullingEnable = 1;
	}

	TShaderMapRef<FHairStrandsPositionChangedCS> ComputeShader(View->ShaderMap);
	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("HairStrands::HasPositionChanged"),
		ComputeShader,
		Parameters,
		DispatchCount);
}

///////////////////////////////////////////////////////////////////////////////////////////////////

namespace HairStrands
{

// Draw hair strands depth value for outline selection
void DrawEditorSelection(FRDGBuilder& GraphBuilder, const FViewInfo& View, const FIntRect& ViewportRect, FRDGTextureRef SelectionDepthTexture)
{
	AddHairStrandsSelectionOutlinePass(
		GraphBuilder,
		View,
		ViewportRect,
		View.HairStrandsViewData.VisibilityData.NodeIndex,
		View.HairStrandsViewData.VisibilityData.NodeVisData,
		View.HairStrandsViewData.VisibilityData.CoverageTexture,
		SelectionDepthTexture);
}

// Draw hair strands hit proxy values
void DrawHitProxies(
	FRDGBuilder& GraphBuilder,
	const FScene& Scene,
	const FViewInfo& View,
	FInstanceCullingManager& InstanceCullingManager,
	FRDGTextureRef HitProxyTexture,
	FRDGTextureRef HitProxyDepthTexture)
{
	// Proxy rendering is only supported/compatible with MSAA-visibility rendering. 
	// PPLL is not supported, but it is supposed to be used only for final render.
	if (GetHairVisibilityRenderMode() != HairVisibilityRenderMode_MSAA_Visibility)
	{
		return;
	}

	// The hit proxy view reuse data generated by regular view. This means it assumes LOD selection, simulation, and interpolation has run. 
	// Geometry won't be updated for proxy view
	const FIntPoint Resolution = HitProxyTexture->Desc.Extent;
	FHairStrandsViewData HairStrandsViewData;
	CreateHairStrandsMacroGroups(GraphBuilder, &Scene, View, HairStrandsViewData);

	// We don't compute the transmittance texture as there is no need for picking.
	FRDGTextureRef DummyTransmittanceTexture = GraphBuilder.CreateTexture(FRDGTextureDesc::Create2D(Resolution, PF_R32_FLOAT, FClearValueBinding::White, ETextureCreateFlags::ShaderResource | ETextureCreateFlags::UAV), TEXT("Hair.DummyTransmittanceTextureForHitProxyId"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(DummyTransmittanceTexture), 1.0f);

	FHairStrandsTiles TileData;
	const FHairStrandsMacroGroupDatas& MacroGroupDatas = HairStrandsViewData.MacroGroupDatas;
	FHairPrimaryTransmittance ViewTransmittance;

	FRDGTextureRef SceneDepthTexture = HitProxyDepthTexture;
	FRDGTextureRef VisDepthTexture = AddHairVisibilityFillOpaqueDepth(
		GraphBuilder,
		View,
		Resolution,
		MacroGroupDatas,
		TileData,
		SceneDepthTexture);

	FRDGTextureRef VisIdTexture = nullptr;
	AddHairVisibilityMSAAPass(
		GraphBuilder,
		&Scene,
		&View,
		MacroGroupDatas,
		Resolution,
		TileData,
		InstanceCullingManager,
		VisIdTexture,
		VisDepthTexture);


	FHairVisibilityPrimitiveIdCompactionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHairVisibilityPrimitiveIdCompactionCS::FParameters>();
	PassParameters->MSAA_DepthTexture = VisDepthTexture;
	PassParameters->MSAA_IDTexture = VisIdTexture;
	PassParameters->ViewTransmittanceTexture = DummyTransmittanceTexture;

	const uint32 NodeGroupSize = GetVendorOptimalGroupSize1D();
	const uint32 MaxSampleCount = 4;

	FRDGTextureRef NodeCounter = nullptr;
	FRDGTextureRef VisNodeIndex = nullptr;
	FRDGBufferRef  VisNodeData = nullptr;
	FRDGBufferRef  VisNodeCoord = nullptr;
	FRDGBufferRef  IndirectArgsBuffer = nullptr;
	FRDGTextureRef ResolveMaskTexture = nullptr;
	FRDGTextureRef CoverageTexture = nullptr;

	uint32 OutMaxNodeCount = 0;
	AddHairVisibilityPrimitiveIdCompactionPass(
		false, // bUsePPLL
		GraphBuilder,
		View,
		SceneDepthTexture,
		MacroGroupDatas,
		NodeGroupSize,
		TileData,
		PassParameters,
		NodeCounter,
		VisNodeIndex,
		VisNodeData,
		VisNodeCoord,
		CoverageTexture,
		nullptr, // Velocity output is only needed for PPLL
		IndirectArgsBuffer,
		OutMaxNodeCount);

	AddHairStrandsHitProxyIdPass(GraphBuilder, Scene, View, VisNodeIndex, VisNodeData, HitProxyTexture, HitProxyDepthTexture);
}

// Check if any simulated/skinned-bound groom has its positions updated (e.g. for invalidating the path-tracer accumulation)
bool HasPositionsChanged(FRDGBuilder& GraphBuilder, const FViewInfo& View)
{
	if (View.HairStrandsMeshElements.IsEmpty())
	{
		// there are no hair strands in the scene
		return false;
	}

	if (GHairStrands_InvalidationPosition_Threshold < 0)
	{
		return false;
	}

	FHairStrandsViewStateData* HairStrandsViewStateData = const_cast<FHairStrandsViewStateData*>(&View.ViewState->HairStrandsViewStateData);
	if (!HairStrandsViewStateData->IsInit())
	{
		HairStrandsViewStateData->Init();
	}

	TArray<const FHairGroupPublicData*> GroupDatas;
	for (const FMeshBatchAndRelevance& Batch : View.HairStrandsMeshElements)
	{
		FHairGroupPublicData* HairGroupPublicData = GetHairData(Batch.Mesh);
		check(HairGroupPublicData);
		const int32 LODIndex = FMath::FloorToInt(HairGroupPublicData->LODIndex);
		const bool bHasSimulationOrSkinning = 
			HairGroupPublicData->GetGeometryType(LODIndex) == EHairGeometryType::Strands &&
			(HairGroupPublicData->IsSimulationEnable(LODIndex) || HairGroupPublicData->GetBindingType(LODIndex) == EHairBindingType::Skinning);
		if (bHasSimulationOrSkinning)
		{
			GroupDatas.Add(HairGroupPublicData);
		}
	}
	if (GroupDatas.IsEmpty())
	{
		// there are no strands currently being simulated or skinned
		return false;
	}

	FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1);
	Desc.Usage |= BUF_SourceCopy;
	FRDGBufferRef InvalidationBuffer = GraphBuilder.CreateBuffer(Desc, TEXT("Hair.HasSimulationRunningBuffer"));
	FRDGBufferUAVRef InvalidationUAV = GraphBuilder.CreateUAV(InvalidationBuffer, PF_R32_UINT);
	AddClearUAVPass(GraphBuilder, InvalidationUAV, 0u);

	// Compare current/previous and enqueue aggregated comparison
	for (const FHairGroupPublicData* GroupData : GroupDatas)
	{
		AddHairStrandsHasPositionChangedPass(GraphBuilder, &View, GroupData, InvalidationUAV);
	}

	// Pull a 'ready' previous frame value
	bool bHasPositionChanged = HairStrandsViewStateData->ReadPositionsChanged();

	// Enqueue new readback request
	HairStrandsViewStateData->EnqueuePositionsChanged(GraphBuilder, InvalidationBuffer);


	return bHasPositionChanged;
}

}

///////////////////////////////////////////////////////////////////////////////////////////////////
bool GetHairStrandsSkyLightingEnable();

void RenderHairStrandsVisibilityBuffer(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	FViewInfo& View,
	FRDGTextureRef SceneGBufferATexture,
	FRDGTextureRef SceneGBufferBTexture,
	FRDGTextureRef SceneGBufferCTexture,
	FRDGTextureRef SceneGBufferDTexture,
	FRDGTextureRef SceneGBufferETexture,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef SceneDepthTexture,
	FRDGTextureRef SceneVelocityTexture,
	FInstanceCullingManager& InstanceCullingManager)
{
	QUICK_SCOPE_CYCLE_COUNTER(STAT_CLM_RenderHairStrandsVisibility);
	RDG_EVENT_SCOPE(GraphBuilder, "HairStrandsVisibility");
	RDG_GPU_STAT_SCOPE(GraphBuilder, HairStrandsVisibility);

	FHairStrandsMacroGroupDatas& MacroGroupDatas = View.HairStrandsViewData.MacroGroupDatas;
	check(View.Family);
	check(MacroGroupDatas.Num() > 0);

	const bool bGenerateTile = GHairStrandsTile > 0;

	const FIntRect HairRect = ComputeVisibleHairStrandsMacroGroupsRect(View.ViewRect, MacroGroupDatas);
	const int32 HairPixelCount = HairRect.Width() * HairRect.Height();
	if (HairPixelCount <= 0)
	{
		View.HairStrandsViewData.VisibilityData = FHairStrandsVisibilityData();
		return;
	}

	{
		
		{
			FHairStrandsVisibilityData& VisibilityData = View.HairStrandsViewData.VisibilityData;
			VisibilityData.NodeGroupSize = GetVendorOptimalGroupSize1D();
			VisibilityData.MaxSampleCount = GetMaxSamplePerPixel();

			// Use the scene color for computing target resolution as the View.ViewRect, 
			// doesn't include the actual resolution padding which make buffer size 
			// mismatch, and create artifact (e.g. velocity computation)
			check(SceneDepthTexture);
			const FIntPoint Resolution = SceneDepthTexture->Desc.Extent;

			const bool bRunColorAndDepthPatching = SceneGBufferBTexture && SceneColorTexture;
			const EHairVisibilityRenderMode RenderMode = GetHairVisibilityRenderMode();
			check(RenderMode == HairVisibilityRenderMode_MSAA_Visibility || RenderMode == HairVisibilityRenderMode_PPLL || RenderMode == HairVisibilityRenderMode_ComputeRaster);

			FRDGTextureRef HairOnlyDepthTexture = GraphBuilder.CreateTexture(SceneDepthTexture->Desc, TEXT("Hair.HairOnlyDepthTexture"));
			FRDGTextureRef CoverageTexture = nullptr;
			FRDGTextureRef CompactNodeIndex = nullptr;
			FRDGBufferRef  CompactNodeData = nullptr;
			FRDGBufferRef  CompactNodeVis = nullptr;
			FRDGTextureRef NodeCounter = nullptr;

			if (RenderMode == HairVisibilityRenderMode_ComputeRaster)
			{
				FRasterComputeOutput RasterOutput = AddVisibilityComputeRasterPass(
					GraphBuilder,
					View,
					MacroGroupDatas,
					Resolution,
					VisibilityData.MaxSampleCount,
					SceneDepthTexture);

				// Merge this pass within the compaction pass
				FHairPrimaryTransmittance ViewTransmittance;
				{
					ViewTransmittance.TransmittanceTexture = AddHairHairCountToTransmittancePass(
						GraphBuilder,
						View,
						RasterOutput.HairCountTexture);

					ViewTransmittance.HairCountTextureUint = RasterOutput.HairCountTexture;
					VisibilityData.ViewHairCountUintTexture = ViewTransmittance.HairCountTextureUint;
				}

				// Generate Tile data
				if (ViewTransmittance.TransmittanceTexture && bGenerateTile)
				{
					VisibilityData.TileData = AddHairStrandsGenerateTilesPass(GraphBuilder, View, ViewTransmittance.TransmittanceTexture);
				}

				{
					{
						FRDGBufferRef CompactNodeCoord;
						FRDGBufferRef IndirectArgsBuffer;
						FRDGTextureRef ResolveMaskTexture = nullptr;
						AddHairVisibilityCompactionComputeRasterPass(
							GraphBuilder,
							View,
							VisibilityData.NodeGroupSize,
							VisibilityData.MaxSampleCount,
							RasterOutput,
							VisibilityData.TileData,
							ViewTransmittance.TransmittanceTexture, // TODO tile
							NodeCounter,
							CompactNodeIndex,
							CompactNodeVis,
							CompactNodeCoord,
							CoverageTexture,
							IndirectArgsBuffer,
							VisibilityData.MaxNodeCount);


						// Evaluate material based on the visiblity pass result
						// Output both complete sample data + per-sample velocity
						FMaterialPassOutput PassOutput = AddHairMaterialPass(
							GraphBuilder,
							Scene,
							&View,
							false,
							MacroGroupDatas,
							InstanceCullingManager,
							VisibilityData.NodeGroupSize,
							CompactNodeIndex,
							CompactNodeVis,
							CompactNodeCoord,
							NodeCounter,
							IndirectArgsBuffer);

						// Merge per-sample velocity into the scene velocity buffer
						AddHairVelocityPass(
							GraphBuilder,
							View,
							MacroGroupDatas,
							VisibilityData.TileData,
							CoverageTexture,
							CompactNodeIndex,
							CompactNodeVis,
							PassOutput.NodeVelocity,
							SceneVelocityTexture,
							ResolveMaskTexture);

						CompactNodeData = PassOutput.NodeData;

						VisibilityData.SampleLightingViewportResolution = PassOutput.SampleLightingTexture->Desc.Extent;
						VisibilityData.SampleLightingTexture = PassOutput.SampleLightingTexture;
						VisibilityData.NodeIndex = CompactNodeIndex;
						VisibilityData.CoverageTexture = CoverageTexture;
						VisibilityData.HairOnlyDepthTexture = HairOnlyDepthTexture;
						VisibilityData.NodeData = CompactNodeData;
						VisibilityData.NodeCoord = CompactNodeCoord;
						VisibilityData.NodeIndirectArg = IndirectArgsBuffer;
						VisibilityData.NodeCount = NodeCounter;
						VisibilityData.ResolveMaskTexture = ResolveMaskTexture;	
					}

					// For fully covered pixels, write: 
					// * black color into the scene color
					// * closest depth
					// * unlit shading model ID 
					if (bRunColorAndDepthPatching)
					{
						AddHairGbufferPatchPass(
							GraphBuilder,
							View,
							VisibilityData.TileData,
							CoverageTexture,
							CompactNodeIndex,
							CompactNodeData,
							SceneGBufferBTexture,
							SceneGBufferCTexture,
							SceneColorTexture,
							SceneDepthTexture,
							VisibilityData.LightChannelMaskTexture);
					}

					AddHairOnlyDepthPass(
						GraphBuilder,
						View,
						VisibilityData.TileData,
						CoverageTexture,
						CompactNodeIndex,
						CompactNodeData,
						HairOnlyDepthTexture);

					AddHairOnlyHZBPass(
						GraphBuilder,
						View,
						HairOnlyDepthTexture,
						VisibilityData.HairOnlyDepthClosestHZBTexture,
						VisibilityData.HairOnlyDepthFurthestHZBTexture);
				}
			}
			else if (RenderMode == HairVisibilityRenderMode_MSAA_Visibility)
			{
				// Run the view transmittance pass if needed (not in PPLL mode that is already a high quality render path)
				FHairPrimaryTransmittance ViewTransmittance;
				{
					// Note: Hair count is required for the sky lighting at the moment as it is used for the TT term
					// TT sampling is disable in hair sky lighting integrator 0. So the GetHairStrandsSkyLightingEnable() check is no longer needed
					const bool bOutputHairCount = GHairStrandsHairCountToTransmittance > 0;
					ViewTransmittance = AddHairViewTransmittancePass(
						GraphBuilder,
						Scene,
						&View,
						MacroGroupDatas,
						Resolution,
						bOutputHairCount,
						SceneDepthTexture,
						InstanceCullingManager);

					const bool bHairCountToTransmittance = GHairStrandsHairCountToTransmittance > 0;
					if (bHairCountToTransmittance)
					{
						ViewTransmittance.TransmittanceTexture = AddHairHairCountToTransmittancePass(
							GraphBuilder,
							View,
							ViewTransmittance.HairCountTexture);
					}

				}

				// Generate Tile data
				if (ViewTransmittance.TransmittanceTexture && bGenerateTile)
				{
					VisibilityData.TileData = AddHairStrandsGenerateTilesPass(GraphBuilder, View, ViewTransmittance.TransmittanceTexture);
				}

				struct FRDGMsaaVisibilityResources
				{
					FRDGTextureRef DepthTexture;
					FRDGTextureRef IdTexture;
				} MsaaVisibilityResources;

				MsaaVisibilityResources.DepthTexture = AddHairVisibilityFillOpaqueDepth(
					GraphBuilder,
					View,
					Resolution,
					MacroGroupDatas,
					VisibilityData.TileData,
					SceneDepthTexture);

				AddHairVisibilityMSAAPass(
					GraphBuilder,
					Scene,
					&View,
					MacroGroupDatas,
					Resolution,
					VisibilityData.TileData,
					InstanceCullingManager,
					MsaaVisibilityResources.IdTexture,
					MsaaVisibilityResources.DepthTexture);

				// This is used when compaction is not enabled.
				VisibilityData.MaxSampleCount = MsaaVisibilityResources.IdTexture->Desc.NumSamples;
				VisibilityData.HairOnlyDepthTexture = HairOnlyDepthTexture;
				
				{
					FHairVisibilityPrimitiveIdCompactionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHairVisibilityPrimitiveIdCompactionCS::FParameters>();
					PassParameters->MSAA_DepthTexture = MsaaVisibilityResources.DepthTexture;
					PassParameters->MSAA_IDTexture = MsaaVisibilityResources.IdTexture;
					PassParameters->ViewTransmittanceTexture = ViewTransmittance.TransmittanceTexture;

					FRDGBufferRef CompactNodeCoord;
					FRDGBufferRef IndirectArgsBuffer;
					FRDGTextureRef ResolveMaskTexture = nullptr;
					AddHairVisibilityPrimitiveIdCompactionPass(
						false, // bUsePPLL
						GraphBuilder,
						View,
						SceneDepthTexture,
						MacroGroupDatas,
						VisibilityData.NodeGroupSize,
						VisibilityData.TileData,
						PassParameters,
						NodeCounter,
						CompactNodeIndex,
						CompactNodeVis,
						CompactNodeCoord,
						CoverageTexture,
						nullptr, // Velocity output is only needed for PPLL
						IndirectArgsBuffer,
						VisibilityData.MaxNodeCount);


					{
						const bool bUpdateSampleCoverage = GHairStrandsSortHairSampleByDepth > 0;

						// Evaluate material based on the visiblity pass result
						// Output both complete sample data + per-sample velocity
						FMaterialPassOutput PassOutput = AddHairMaterialPass(
							GraphBuilder,
							Scene,
							&View,
							bUpdateSampleCoverage,
							MacroGroupDatas,
							InstanceCullingManager,
							VisibilityData.NodeGroupSize,
							CompactNodeIndex,
							CompactNodeVis,
							CompactNodeCoord,
							NodeCounter,
							IndirectArgsBuffer);

						// Merge per-sample velocity into the scene velocity buffer
						AddHairVelocityPass(
							GraphBuilder,
							View,
							MacroGroupDatas,
							VisibilityData.TileData,
							CoverageTexture,
							CompactNodeIndex,
							CompactNodeVis,
							PassOutput.NodeVelocity,
							SceneVelocityTexture,
							ResolveMaskTexture);

						if (bUpdateSampleCoverage)
						{
							PassOutput.NodeData = AddUpdateSampleCoveragePass(
								GraphBuilder,
								&View,
								CompactNodeIndex,
								PassOutput.NodeData);
						}

						CompactNodeData = PassOutput.NodeData;

						VisibilityData.SampleLightingViewportResolution = PassOutput.SampleLightingTexture->Desc.Extent;
						VisibilityData.SampleLightingTexture			= PassOutput.SampleLightingTexture;
					}

					VisibilityData.NodeIndex			= CompactNodeIndex;
					VisibilityData.CoverageTexture		= CoverageTexture;
					VisibilityData.HairOnlyDepthTexture	= HairOnlyDepthTexture;
					VisibilityData.NodeData				= CompactNodeData;
					VisibilityData.NodeVisData			= CompactNodeVis;
					VisibilityData.NodeCoord			= CompactNodeCoord;
					VisibilityData.NodeIndirectArg		= IndirectArgsBuffer;
					VisibilityData.NodeCount			= NodeCounter;
					VisibilityData.ResolveMaskTexture	= ResolveMaskTexture;

					// View transmittance depth test needs to happen before the scene depth is patched with the hair depth (for fully-covered-by-hair pixels)
					if (ViewTransmittance.HairCountTexture)
					{
						AddHairViewTransmittanceDepthPass(
							GraphBuilder,
							View,
							CoverageTexture,
							SceneDepthTexture,
							ViewTransmittance.HairCountTexture);
						VisibilityData.ViewHairCountTexture = ViewTransmittance.HairCountTexture;
					}

					// For fully covered pixels, write: 
					// * black color into the scene color
					// * closest depth
					// * unlit shading model ID 
					if (bRunColorAndDepthPatching)
					{
						AddHairGbufferPatchPass(
							GraphBuilder,
							View,
							VisibilityData.TileData,
							CoverageTexture,
							CompactNodeIndex,
							CompactNodeData,
							SceneGBufferBTexture,
							SceneGBufferCTexture,
							SceneColorTexture,
							SceneDepthTexture,
							VisibilityData.LightChannelMaskTexture);
					}

					AddHairOnlyDepthPass(
						GraphBuilder,
						View,
						VisibilityData.TileData,
						CoverageTexture,
						CompactNodeIndex,
						CompactNodeData,
						HairOnlyDepthTexture);

					AddHairOnlyHZBPass(
						GraphBuilder,
						View,
						HairOnlyDepthTexture,
						VisibilityData.HairOnlyDepthClosestHZBTexture,
						VisibilityData.HairOnlyDepthFurthestHZBTexture);
				}
			}
			else if (RenderMode == HairVisibilityRenderMode_PPLL)
			{
				// In this pas we reuse the scene depth buffer to cull hair pixels out.
				// Pixel data is accumulated in buffer containing data organized in a linked list with node scattered in memory according to pixel shader execution. 
				// This with up to width * height * GHairVisibilityPPLLGlobalMaxPixelNodeCount node total maximum.
				// After we have that a node sorting pass happening and we finally output all the data once into the common compaction node list.

				FRDGTextureRef PPLLNodeCounterTexture;
				FRDGTextureRef PPLLNodeIndexTexture;
				FRDGBufferRef PPLLNodeDataBuffer;
				FRDGTextureRef ViewZDepthTexture = SceneDepthTexture;

				// Linked list generation pass
				AddHairVisibilityPPLLPass(GraphBuilder, Scene, &View, MacroGroupDatas, Resolution, InstanceCullingManager, ViewZDepthTexture, PPLLNodeCounterTexture, PPLLNodeIndexTexture, PPLLNodeDataBuffer);

				// Generate Tile data
				if (PPLLNodeIndexTexture && bGenerateTile)
				{
					VisibilityData.TileData = AddHairStrandsGenerateTilesPass(GraphBuilder, View, PPLLNodeIndexTexture); 
				}

				// Linked list sorting pass and compaction into common representation
				{
					FHairVisibilityPrimitiveIdCompactionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHairVisibilityPrimitiveIdCompactionCS::FParameters>();
					PassParameters->PPLLCounter  = PPLLNodeCounterTexture;
					PassParameters->PPLLNodeIndex= PPLLNodeIndexTexture;
					PassParameters->PPLLNodeData = GraphBuilder.CreateSRV(PPLLNodeDataBuffer);
					PassParameters->ViewTransmittanceTexture = nullptr;

					FRDGBufferRef CompactNodeCoord;
					FRDGBufferRef IndirectArgsBuffer;
					AddHairVisibilityPrimitiveIdCompactionPass(
						true, // bUsePPLL
						GraphBuilder,
						View,
						SceneDepthTexture,
						MacroGroupDatas,
						VisibilityData.NodeGroupSize,
						VisibilityData.TileData,
						PassParameters,
						NodeCounter,
						CompactNodeIndex,
						CompactNodeData,
						CompactNodeCoord,
						CoverageTexture,
						SceneVelocityTexture,
						IndirectArgsBuffer,
						VisibilityData.MaxNodeCount);

					VisibilityData.MaxSampleCount = GetMaxSamplePerPixel();
					VisibilityData.NodeIndex = CompactNodeIndex;
					VisibilityData.CoverageTexture = CoverageTexture;
					VisibilityData.HairOnlyDepthTexture = HairOnlyDepthTexture;
					VisibilityData.NodeData = CompactNodeData;
					VisibilityData.NodeCoord = CompactNodeCoord;
					VisibilityData.NodeIndirectArg = IndirectArgsBuffer;
					VisibilityData.NodeCount = NodeCounter;
				}


				if (bRunColorAndDepthPatching)
				{
					AddHairGbufferPatchPass(
						GraphBuilder,
						View,
						VisibilityData.TileData,
						CoverageTexture,
						CompactNodeIndex,
						CompactNodeData,
						SceneGBufferBTexture,
						SceneGBufferCTexture,
						SceneColorTexture,
						SceneDepthTexture,
						VisibilityData.LightChannelMaskTexture);
				}

				AddHairOnlyDepthPass(
					GraphBuilder,
					View,
					VisibilityData.TileData,
					CoverageTexture,
					CompactNodeIndex,
					CompactNodeData,
					HairOnlyDepthTexture);

				// Allocate buffer for storing all the light samples
				VisibilityData.SampleLightingTexture = AddClearLightSamplePass(GraphBuilder, &View, VisibilityData.MaxNodeCount, NodeCounter);
				VisibilityData.SampleLightingViewportResolution = VisibilityData.SampleLightingTexture->Desc.Extent;

			#if WITH_EDITOR
				// Extract texture for debug visualization
				if (GHairStrandsDebugPPLL > 0)
				{
					View.HairStrandsViewData.DebugData.PPLLNodeCounterTexture = PPLLNodeCounterTexture;
					View.HairStrandsViewData.DebugData.PPLLNodeIndexTexture = PPLLNodeIndexTexture;
					View.HairStrandsViewData.DebugData.PPLLNodeDataBuffer = PPLLNodeDataBuffer;
				}
			#endif
			}

		#if RHI_RAYTRACING
			if (IsRayTracingEnabled() && VisibilityData.LightChannelMaskTexture == nullptr)
			{
				VisibilityData.LightChannelMaskTexture = AddHairLightChannelMaskPass(
					GraphBuilder,
					View,
					VisibilityData.TileData,
					CoverageTexture,
					CompactNodeIndex,
					CompactNodeData,
					SceneDepthTexture);
			}
		#endif
		}
	}
}