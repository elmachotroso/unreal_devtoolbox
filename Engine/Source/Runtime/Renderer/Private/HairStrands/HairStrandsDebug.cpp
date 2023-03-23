// Copyright Epic Games, Inc. All Rights Reserved.

#include "HairStrandsDebug.h"
#include "HairStrandsInterface.h"
#include "HairStrandsCluster.h"
#include "HairStrandsDeepShadow.h"
#include "HairStrandsUtils.h"
#include "HairStrandsVoxelization.h"
#include "HairStrandsRendering.h"
#include "HairStrandsVisibility.h"
#include "HairStrandsInterface.h"
#include "HairStrandsMeshProjection.h"
#include "HairStrandsTile.h"
#include "HairStrandsData.h"

#include "Shader.h"
#include "GlobalShader.h"
#include "ShaderParameters.h"
#include "ShaderParameterStruct.h"
#include "RenderGraphUtils.h"
#include "PostProcessing.h"
#include "SceneTextureParameters.h"
#include "DynamicPrimitiveDrawing.h"
#include "RenderTargetTemp.h"
#include "CanvasTypes.h"
#include "ShaderPrintParameters.h"
#include "RenderGraphUtils.h"
#include "ShaderDebug.h"
#include "ShaderPrint.h"
#include "ScreenPass.h"

static int32 GDeepShadowDebugIndex = 0;
static float GDeepShadowDebugScale = 20;

static FAutoConsoleVariableRef CVarDeepShadowDebugDomIndex(TEXT("r.HairStrands.DeepShadow.DebugDOMIndex"), GDeepShadowDebugIndex, TEXT("Index of the DOM texture to draw"));
static FAutoConsoleVariableRef CVarDeepShadowDebugDomScale(TEXT("r.HairStrands.DeepShadow.DebugDOMScale"), GDeepShadowDebugScale, TEXT("Scaling value for the DeepOpacityMap when drawing the deep shadow stats"));

static int32 GHairStrandsDebugMode = 0;
static FAutoConsoleVariableRef CVarDeepShadowStats(TEXT("r.HairStrands.DebugMode"), GHairStrandsDebugMode, TEXT("Draw various stats/debug mode about hair rendering"));

static int32 GHairStrandsDebugStrandsMode = 0;
static FAutoConsoleVariableRef CVarDebugPhysicsStrand(TEXT("r.HairStrands.StrandsMode"), GHairStrandsDebugStrandsMode, TEXT("Render debug mode for hair strands. 0:off, 1:simulation strands, 2:render strands with colored simulation strands influence, 3:hair UV, 4:hair root UV, 5: hair seed, 6: dimensions"));

static int32 GHairStrandsDebugPlotBsdf = 0;
static FAutoConsoleVariableRef CVarHairStrandsDebugBSDF(TEXT("r.HairStrands.PlotBsdf"), GHairStrandsDebugPlotBsdf, TEXT("Debug view for visualizing hair BSDF."));

static float GHairStrandsDebugPlotBsdfRoughness = 0.3f;
static FAutoConsoleVariableRef CVarHairStrandsDebugBSDFRoughness(TEXT("r.HairStrands.PlotBsdf.Roughness"), GHairStrandsDebugPlotBsdfRoughness, TEXT("Change the roughness of the debug BSDF plot."));

static float GHairStrandsDebugPlotBsdfBaseColor = 1;
static FAutoConsoleVariableRef CVarHairStrandsDebugBSDFAbsorption(TEXT("r.HairStrands.PlotBsdf.BaseColor"), GHairStrandsDebugPlotBsdfBaseColor, TEXT("Change the base color / absorption of the debug BSDF plot."));

static float GHairStrandsDebugPlotBsdfExposure = 1.1f;
static FAutoConsoleVariableRef CVarHairStrandsDebugBSDFExposure(TEXT("r.HairStrands.PlotBsdf.Exposure"), GHairStrandsDebugPlotBsdfExposure, TEXT("Change the exposure of the plot."));

static int GHairStrandsDebugSampleIndex = -1;
static FAutoConsoleVariableRef CVarHairStrandsDebugMaterialSampleIndex(TEXT("r.HairStrands.DebugMode.SampleIndex"), GHairStrandsDebugSampleIndex, TEXT("Debug value for a given sample index (default:-1, i.e., average sample information)."));

static int32 GHairStrandsClusterDebug = 0;
static FAutoConsoleVariableRef CVarHairStrandsDebugClusterAABB(TEXT("r.HairStrands.Cluster.Debug"), GHairStrandsClusterDebug, TEXT("Draw debug the world bounding box of hair clusters used for culling optimisation (0:off, 1:visible cluster, 2:culled cluster, 3:colored LOD, 4:LOD info)."));

static int32 GHairTangentDebug = 0;
static int32 GHairTangentDebug_TileSize = 8;
static FAutoConsoleVariableRef CVarHairTangentDebug(TEXT("r.HairStrands.DebugMode.Tangent"), GHairTangentDebug, TEXT("Draw debug tangent for hair strands and hair cards."));
static FAutoConsoleVariableRef CVarHairTangentDebug_TileSize(TEXT("r.HairStrands.DebugMode.Tangent.TileSize"), GHairTangentDebug_TileSize, TEXT("Draw debug tangent - Grid size for drawing debug tangent"));

static int32 GHairVirtualVoxel_DrawDebugPage = 0;
static FAutoConsoleVariableRef CVarHairVirtualVoxel_DrawDebugPage(TEXT("r.HairStrands.Voxelization.Virtual.DrawDebugPage"), GHairVirtualVoxel_DrawDebugPage, TEXT("When voxel debug rendering is enable 1: render the page bounds, instead of the voxel 2: the occupancy within the page (i.e., 8x8x8 brick)"));
static int32 GHairVirtualVoxel_ForceMipLevel = -1;
static FAutoConsoleVariableRef CVarHairVirtualVoxel_ForceMipLevel(TEXT("r.HairStrands.Voxelization.Virtual.ForceMipLevel"), GHairVirtualVoxel_ForceMipLevel, TEXT("Force a particular mip-level"));
static int32 GHairVirtualVoxel_DebugTraversalType = 0;
static FAutoConsoleVariableRef CVarHairVirtualVoxel_DebugTraversalType(TEXT("r.HairStrands.Voxelization.Virtual.DebugTraversalType"), GHairVirtualVoxel_DebugTraversalType, TEXT("Traversal mode (0:linear, 1:mip) for debug voxel visualization."));

static bool TryEnableShaderDrawAndShaderPrint(const FViewInfo& View, uint32 ResquestedShaderDrawElements, uint32 RequestedShaderPrintElements)
{
	const EShaderPlatform Platform = View.Family->GetShaderPlatform();
	if (!ShaderDrawDebug::IsSupported(Platform) || !ShaderPrint::IsSupported(Platform))
	{
		return false;
	}

	if (!ShaderPrint::IsEnabled(View))
	{
		ShaderPrint::SetEnabled(true);
	}
	ShaderPrint::RequestSpaceForCharacters(RequestedShaderPrintElements);

	if (!ShaderDrawDebug::IsEnabled(View))
	{
		ShaderDrawDebug::SetEnabled(true);
	}
	ShaderDrawDebug::RequestSpaceForElements(ResquestedShaderDrawElements);
	return true;
}

static bool IsDebugDrawAndDebugPrintEnabled(const FViewInfo& View)
{
	return ShaderDrawDebug::IsEnabled(View) && ShaderPrint::IsEnabled(View);
}

bool IsHairStrandsClusterDebugEnable()
{
	return GHairStrandsClusterDebug > 0;
}

bool IsHairStrandsClusterDebugAABBEnable()
{
	return GHairStrandsClusterDebug > 1;
}

FHairStrandsDebugData::Data FHairStrandsDebugData::CreateData(FRDGBuilder& GraphBuilder)
{
	FHairStrandsDebugData::Data Out;
	Out.ShadingPointBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(ShadingInfo), MaxShadingPointCount), TEXT("Hair.DebugShadingPoint"));
	Out.ShadingPointCounter = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("Hair.DebugShadingPointCounter"));
	Out.SampleBuffer = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(Sample), MaxSampleCount), TEXT("Hair.DebugSample"));
	Out.SampleCounter = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(uint32), 1), TEXT("Hair.DebugSampleCounter"));
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Out.ShadingPointCounter, PF_R32_UINT), 0u);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Out.SampleCounter, PF_R32_UINT), 0u);
	return Out;
}

void FHairStrandsDebugData::SetParameters(FRDGBuilder& GraphBuilder, const FHairStrandsDebugData::Data& In, FHairStrandsDebugData::FWriteParameters& Out)
{
	Out.Debug_MaxSampleCount = FHairStrandsDebugData::MaxSampleCount;
	Out.Debug_MaxShadingPointCount = FHairStrandsDebugData::MaxShadingPointCount;
	Out.Debug_ShadingPointBuffer = GraphBuilder.CreateUAV(In.ShadingPointBuffer);
	Out.Debug_ShadingPointCounter = GraphBuilder.CreateUAV(In.ShadingPointCounter, PF_R32_UINT);
	Out.Debug_SampleBuffer = GraphBuilder.CreateUAV(In.SampleBuffer);
	Out.Debug_SampleCounter = GraphBuilder.CreateUAV(In.SampleCounter, PF_R32_UINT);
}

void FHairStrandsDebugData::SetParameters(FRDGBuilder& GraphBuilder, const FHairStrandsDebugData::Data& In, FHairStrandsDebugData::FReadParameters& Out)
{
	Out.Debug_MaxSampleCount = FHairStrandsDebugData::MaxSampleCount;
	Out.Debug_MaxShadingPointCount = FHairStrandsDebugData::MaxShadingPointCount;
	Out.Debug_ShadingPointBuffer = GraphBuilder.CreateSRV(In.ShadingPointBuffer);
	Out.Debug_ShadingPointCounter = GraphBuilder.CreateSRV(In.ShadingPointCounter, PF_R32_UINT);
	Out.Debug_SampleBuffer = GraphBuilder.CreateSRV(In.SampleBuffer);
	Out.Debug_SampleCounter = GraphBuilder.CreateSRV(In.SampleCounter, PF_R32_UINT);
}

EHairDebugMode GetHairStrandsDebugMode()
{
	switch (GHairStrandsDebugMode)
	{
	case 0:  return EHairDebugMode::None;
	case 1:  return EHairDebugMode::MacroGroups;
	case 2:  return EHairDebugMode::LightBounds;
	case 3:  return EHairDebugMode::MacroGroupScreenRect;
	case 4:  return EHairDebugMode::DeepOpacityMaps;
	case 5:  return EHairDebugMode::SamplePerPixel;
	case 6:  return EHairDebugMode::TAAResolveType;
	case 7:  return EHairDebugMode::CoverageType;
	case 8:  return EHairDebugMode::VoxelsDensity;
	case 9:  return EHairDebugMode::VoxelsTangent;
	case 10: return EHairDebugMode::VoxelsBaseColor;
	case 11: return EHairDebugMode::VoxelsRoughness;
	case 12: return EHairDebugMode::MeshProjection;
	case 13: return EHairDebugMode::Coverage;
	case 14: return EHairDebugMode::MaterialDepth;
	case 15: return EHairDebugMode::MaterialBaseColor;
	case 16: return EHairDebugMode::MaterialRoughness;
	case 17: return EHairDebugMode::MaterialSpecular;
	case 18: return EHairDebugMode::MaterialTangent;
	case 19: return EHairDebugMode::Tile;
	default: return EHairDebugMode::None;
	};
}

static const TCHAR* ToString(EHairDebugMode DebugMode)
{
	switch (DebugMode)
	{
	case EHairDebugMode::None: return TEXT("None");
	case EHairDebugMode::MacroGroups: return TEXT("Macro groups info");
	case EHairDebugMode::LightBounds: return TEXT("All DOMs light bounds");
	case EHairDebugMode::MacroGroupScreenRect: return TEXT("Screen projected macro groups");
	case EHairDebugMode::DeepOpacityMaps: return TEXT("Deep opacity maps");
	case EHairDebugMode::SamplePerPixel: return TEXT("Sub-pixel sample count");
	case EHairDebugMode::TAAResolveType: return TEXT("TAA resolve type (regular/responsive)");
	case EHairDebugMode::CoverageType: return TEXT("Type of hair coverage - Fully covered : Green / Partially covered : Red");
	case EHairDebugMode::VoxelsDensity: return TEXT("Hair density volume");
	case EHairDebugMode::VoxelsTangent: return TEXT("Hair tangent volume");
	case EHairDebugMode::VoxelsBaseColor: return TEXT("Hair base color volume");
	case EHairDebugMode::VoxelsRoughness: return TEXT("Hair roughness volume");
	case EHairDebugMode::MeshProjection: return TEXT("Hair mesh projection");
	case EHairDebugMode::Coverage: return TEXT("Hair coverage");
	case EHairDebugMode::MaterialDepth: return TEXT("Hair material depth");
	case EHairDebugMode::MaterialBaseColor: return TEXT("Hair material base color");
	case EHairDebugMode::MaterialRoughness: return TEXT("Hair material roughness");
	case EHairDebugMode::MaterialSpecular: return TEXT("Hair material specular");
	case EHairDebugMode::MaterialTangent: return TEXT("Hair material tangent");
	case EHairDebugMode::Tile: return TEXT("Hair tile cotegorization");
	default: return TEXT("None");
	};
}


EHairStrandsDebugMode GetHairStrandsDebugStrandsMode()
{
	switch (GHairStrandsDebugStrandsMode)
	{
	case  0:  return EHairStrandsDebugMode::NoneDebug;
	case  1:  return EHairStrandsDebugMode::SimHairStrands;
	case  2:  return EHairStrandsDebugMode::RenderHairStrands;
	case  3:  return EHairStrandsDebugMode::RenderHairRootUV;
	case  4:  return EHairStrandsDebugMode::RenderHairRootUDIM;
	case  5:  return EHairStrandsDebugMode::RenderHairUV;
	case  6:  return EHairStrandsDebugMode::RenderHairSeed;
	case  7:  return EHairStrandsDebugMode::RenderHairDimension;
	case  8:  return EHairStrandsDebugMode::RenderHairRadiusVariation;
	case  9:  return EHairStrandsDebugMode::RenderHairBaseColor;
	case 10:  return EHairStrandsDebugMode::RenderHairRoughness;
	case 11:  return EHairStrandsDebugMode::RenderVisCluster;
	case 12:  return EHairStrandsDebugMode::RenderHairTangent;
	case 13:  return EHairStrandsDebugMode::RenderHairControlPoints;
	case 14:  return EHairStrandsDebugMode::RenderHairGroup;
	default:  return EHairStrandsDebugMode::NoneDebug;
	};
}

static const TCHAR* ToString(EHairStrandsDebugMode DebugMode)
{
	switch (DebugMode)
	{
	case EHairStrandsDebugMode::NoneDebug						: return TEXT("None");
	case EHairStrandsDebugMode::SimHairStrands				: return TEXT("Simulation strands");
	case EHairStrandsDebugMode::RenderHairStrands			: return TEXT("Rendering strands influences");
	case EHairStrandsDebugMode::RenderHairRootUV			: return TEXT("Roots UV");
	case EHairStrandsDebugMode::RenderHairRootUDIM			: return TEXT("Roots UV UDIM texture index");
	case EHairStrandsDebugMode::RenderHairUV				: return TEXT("Hair UV");
	case EHairStrandsDebugMode::RenderHairSeed				: return TEXT("Hair seed");
	case EHairStrandsDebugMode::RenderHairDimension			: return TEXT("Hair dimensions");
	case EHairStrandsDebugMode::RenderHairRadiusVariation	: return TEXT("Hair radius variation");
	case EHairStrandsDebugMode::RenderHairTangent			: return TEXT("Hair tangent");
	case EHairStrandsDebugMode::RenderHairControlPoints		: return TEXT("Hair control points");
	case EHairStrandsDebugMode::RenderHairBaseColor			: return TEXT("Hair vertices color");
	case EHairStrandsDebugMode::RenderHairRoughness			: return TEXT("Hair vertices roughness");
	case EHairStrandsDebugMode::RenderVisCluster			: return TEXT("Hair visility clusters");
	default													: return TEXT("None");
	};
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairPrintLODInfoCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairPrintLODInfoCS);
	SHADER_USE_PARAMETER_STRUCT(FHairPrintLODInfoCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, MaxResolution)
		SHADER_PARAMETER(FVector3f, GroupColor)
		SHADER_PARAMETER(uint32, GroupIndex)
		SHADER_PARAMETER(uint32, GeometryType)
		SHADER_PARAMETER(float, ScreenSize)
		SHADER_PARAMETER(float, LOD)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderPrint::FShaderParameters, ShaderPrintUniformBuffer)
		END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::All, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		// Skip optimization for avoiding long compilation time due to large UAV writes
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Debug);
		OutEnvironment.SetDefine(TEXT("SHADER_LOD_INFO"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairPrintLODInfoCS, "/Engine/Private/HairStrands/HairStrandsDebugPrint.usf", "MainCS", SF_Compute);

static void AddPrintLODInfoPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairGroupPublicData* Data)
{
	if (!ShaderPrint::IsSupported(View.Family->GetShaderPlatform()))
	{
		return;
	}

	if (!ShaderPrint::IsEnabled(View))
	{
		ShaderPrint::SetEnabled(true);
		ShaderPrint::RequestSpaceForCharacters(2000);
	}

	const uint32 GroupIndex = Data->GetGroupIndex();
	const FLinearColor GroupColor = Data->DebugGroupColor;
	const uint32 IntLODIndex = Data->LODIndex;

	FHairPrintLODInfoCS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairPrintLODInfoCS::FParameters>();
	Parameters->MaxResolution = FIntPoint(View.ViewRect.Width(), View.ViewRect.Height());
	Parameters->GroupIndex = GroupIndex;
	Parameters->LOD = Data->LODIndex;
	Parameters->GroupColor = FVector3f(GroupColor.R, GroupColor.G, GroupColor.B);
	Parameters->ScreenSize = Data->DebugScreenSize;
	switch (Data->VFInput.GeometryType)
	{
	case EHairGeometryType::Strands: Parameters->GeometryType = 0; break;
	case EHairGeometryType::Cards  : Parameters->GeometryType = 1; break;
	case EHairGeometryType::Meshes : Parameters->GeometryType = 2; break;
	}
	ShaderPrint::SetParameters(GraphBuilder, View, Parameters->ShaderPrintUniformBuffer);
	TShaderMapRef<FHairPrintLODInfoCS> ComputeShader(View.ShaderMap);

	ClearUnusedGraphResources(ComputeShader, Parameters);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("HairStrands::PrintLODInfo(%d/%d)", Parameters->GroupIndex, Parameters->GroupIndex),
		ComputeShader,
		Parameters,
		FIntVector(1, 1, 1));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairDebugPrintCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairDebugPrintCS);
	SHADER_USE_PARAMETER_STRUCT(FHairDebugPrintCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, GroupSize)
		SHADER_PARAMETER(FIntPoint, PixelCoord)
		SHADER_PARAMETER(FIntPoint, MaxResolution)
		SHADER_PARAMETER(uint32, FastResolveMask)
		SHADER_PARAMETER(uint32, HairMacroGroupCount)
		SHADER_PARAMETER(uint32, HairVisibilityNodeGroupSize)
		SHADER_PARAMETER(uint32, AllocatedSampleCount)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCountTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCountUintTexture)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer<uint>, HairVisibilityIndirectArgsBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, HairMacroGroupAABBBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, StencilTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderPrint::FShaderParameters, ShaderPrintUniformBuffer)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderDrawDebug::FShaderParameters, ShaderDrawUniformBuffer)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Tool, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		// Skip optimization for avoiding long compilation time due to large UAV writes
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Debug);
		OutEnvironment.SetDefine(TEXT("SHADER_PRINT"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairDebugPrintCS, "/Engine/Private/HairStrands/HairStrandsDebugPrint.usf", "MainCS", SF_Compute);

static void AddDebugHairPrintPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo* View,
	const EHairDebugMode InDebugMode,
	const FHairStrandsVisibilityData& VisibilityData,
	const FHairStrandsMacroGroupResources& MacroGroupResources,
	FRDGTextureSRVRef InStencilTexture)
{
	if (!View || !View->HairStrandsViewData.UniformBuffer || !InStencilTexture || !ShaderDrawDebug::IsEnabled(*View)) return;

	FRDGTextureRef ViewHairCountTexture = VisibilityData.ViewHairCountTexture ? VisibilityData.ViewHairCountTexture : GSystemTextures.GetBlackDummy(GraphBuilder);
	FRDGTextureRef ViewHairCountUintTexture = VisibilityData.ViewHairCountUintTexture ? VisibilityData.ViewHairCountUintTexture : GSystemTextures.GetBlackDummy(GraphBuilder);

	const FIntRect Viewport = View->ViewRect;
	const FIntPoint Resolution(Viewport.Width(), Viewport.Height());

	FHairDebugPrintCS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairDebugPrintCS::FParameters>();
	Parameters->GroupSize = GetVendorOptimalGroupSize2D();
	Parameters->ViewUniformBuffer = View->ViewUniformBuffer;
	Parameters->MaxResolution = VisibilityData.CoverageTexture ? VisibilityData.CoverageTexture->Desc.Extent : FIntPoint(0,0);
	Parameters->PixelCoord = View->CursorPos;
	Parameters->AllocatedSampleCount = VisibilityData.MaxNodeCount;
	Parameters->FastResolveMask = STENCIL_TEMPORAL_RESPONSIVE_AA_MASK;
	Parameters->HairCountTexture = ViewHairCountTexture;
	Parameters->HairCountUintTexture = ViewHairCountUintTexture;
	Parameters->HairVisibilityIndirectArgsBuffer = GraphBuilder.CreateSRV(VisibilityData.NodeIndirectArg, PF_R32_UINT);
	Parameters->HairVisibilityNodeGroupSize = VisibilityData.NodeGroupSize;
	Parameters->StencilTexture = InStencilTexture;
	Parameters->LinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->HairMacroGroupCount = MacroGroupResources.MacroGroupCount;
	Parameters->HairStrands = View->HairStrandsViewData.UniformBuffer;
	Parameters->HairMacroGroupAABBBuffer = GraphBuilder.CreateSRV(MacroGroupResources.MacroGroupAABBsBuffer, PF_R32_SINT);
	ShaderPrint::SetParameters(GraphBuilder, *View, Parameters->ShaderPrintUniformBuffer);
	ShaderDrawDebug::SetParameters(GraphBuilder, View->ShaderDrawData, Parameters->ShaderDrawUniformBuffer);
	TShaderMapRef<FHairDebugPrintCS> ComputeShader(View->ShaderMap);

	ClearUnusedGraphResources(ComputeShader, Parameters);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("HairStrands::DebugPrint"),
		ComputeShader,
		Parameters,
		FIntVector(1, 1, 1));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairDebugPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairDebugPS);
	SHADER_USE_PARAMETER_STRUCT(FHairDebugPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FVector2f, OutputResolution)
		SHADER_PARAMETER(uint32, FastResolveMask)
		SHADER_PARAMETER(uint32, DebugMode)
		SHADER_PARAMETER(int32, SampleIndex)
		SHADER_PARAMETER(uint32, MaxSampleCount)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCountTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HairCountUintTexture)
		SHADER_PARAMETER_RDG_TEXTURE_SRV(Texture2D, DepthStencilTexture)
		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Tool, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_DEBUG_MODE"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairDebugPS, "/Engine/Private/HairStrands/HairStrandsDebug.usf", "MainPS", SF_Pixel);

static void AddDebugHairPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo* View,
	const EHairDebugMode InDebugMode,
	const FHairStrandsVisibilityData& VisibilityData,
	FRDGTextureSRVRef InDepthStencilTexture,
	FRDGTextureRef& OutTarget)
{
	check(OutTarget);
	check(InDebugMode == EHairDebugMode::TAAResolveType || 
		InDebugMode == EHairDebugMode::SamplePerPixel || 
		InDebugMode == EHairDebugMode::CoverageType || 
		InDebugMode == EHairDebugMode::Coverage ||
		InDebugMode == EHairDebugMode::MaterialDepth ||
		InDebugMode == EHairDebugMode::MaterialBaseColor ||
		InDebugMode == EHairDebugMode::MaterialRoughness ||
		InDebugMode == EHairDebugMode::MaterialSpecular ||
		InDebugMode == EHairDebugMode::MaterialTangent);

	if (!VisibilityData.CoverageTexture || !VisibilityData.NodeIndex || !VisibilityData.NodeData) return;
	if (InDebugMode == EHairDebugMode::TAAResolveType && !InDepthStencilTexture) return;

	const FIntRect Viewport = View->ViewRect;
	const FIntPoint Resolution(Viewport.Width(), Viewport.Height());

	uint32 InternalDebugMode = 0;
	switch (InDebugMode)
	{
		case EHairDebugMode::SamplePerPixel:	InternalDebugMode = 0; break;
		case EHairDebugMode::CoverageType:		InternalDebugMode = 1; break;
		case EHairDebugMode::TAAResolveType:	InternalDebugMode = 2; break;
		case EHairDebugMode::Coverage:			InternalDebugMode = 3; break;
		case EHairDebugMode::MaterialDepth:		InternalDebugMode = 4; break;
		case EHairDebugMode::MaterialBaseColor:	InternalDebugMode = 5; break;
		case EHairDebugMode::MaterialRoughness:	InternalDebugMode = 6; break;
		case EHairDebugMode::MaterialSpecular:	InternalDebugMode = 7; break;
		case EHairDebugMode::MaterialTangent:	InternalDebugMode = 8; break;
	};

	FHairDebugPS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairDebugPS::FParameters>();
	Parameters->ViewUniformBuffer = View->ViewUniformBuffer;
	Parameters->OutputResolution = Resolution;
	Parameters->FastResolveMask = STENCIL_TEMPORAL_RESPONSIVE_AA_MASK;
	Parameters->HairStrands = View->HairStrandsViewData.UniformBuffer;
	Parameters->DepthStencilTexture = InDepthStencilTexture;
	Parameters->LinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->DebugMode = InternalDebugMode;
	Parameters->SampleIndex = GHairStrandsDebugSampleIndex;
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutTarget, ERenderTargetLoadAction::ELoad, 0);
	TShaderMapRef<FPostProcessVS> VertexShader(View->ShaderMap);

	TShaderMapRef<FHairDebugPS> PixelShader(View->ShaderMap);

	ClearUnusedGraphResources(PixelShader, Parameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrands::DebugMode(%s)", ToString(InDebugMode)),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, VertexShader, PixelShader, Viewport, Resolution](FRHICommandList& RHICmdList)
	{
		FGraphicsPipelineStateInitializer GraphicsPSOInit;
		RHICmdList.ApplyCachedRenderTargets(GraphicsPSOInit);
		GraphicsPSOInit.BlendState = TStaticBlendState<CW_RGBA, BO_Add, BF_SourceAlpha, BF_InverseSourceAlpha, BO_Add, BF_One, BF_Zero>::GetRHI();
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
class FDeepShadowVisualizePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDeepShadowVisualizePS);
	SHADER_USE_PARAMETER_STRUCT(FDeepShadowVisualizePS, FGlobalShader);

	class FOutputType : SHADER_PERMUTATION_INT("PERMUTATION_OUTPUT_TYPE", 2);
	using FPermutationDomain = TShaderPermutationDomain<FOutputType>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(float, DomScale)
		SHADER_PARAMETER(FVector2f, DomAtlasOffset)
		SHADER_PARAMETER(FVector2f, DomAtlasScale)
		SHADER_PARAMETER(FVector2f, OutputResolution)
		SHADER_PARAMETER(FVector2f, InvOutputResolution)
		SHADER_PARAMETER(FIntVector4, HairViewRect)

		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DeepShadowDepthTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DeepShadowLayerTexture)

		SHADER_PARAMETER_SAMPLER(SamplerState, LinearSampler)

		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		RENDER_TARGET_BINDING_SLOTS()
		END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Tool, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_VISUALIZEDOM"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDeepShadowVisualizePS, "/Engine/Private/HairStrands/HairStrandsDeepShadowDebug.usf", "VisualizeDomPS", SF_Pixel);

static void AddDebugDeepShadowTexturePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo* View,
	const FIntRect& HairViewRect,
	const FHairStrandsDeepShadowData* ShadowData,
	const FHairStrandsDeepShadowResources* Resources,
	FRDGTextureRef& OutTarget)
{
	check(OutTarget);

	FIntPoint AtlasResolution(0, 0);
	FVector2f AltasOffset(0, 0);
	FVector2f AltasScale(0, 0);
	if (ShadowData && Resources)
	{
		AtlasResolution = FIntPoint(Resources->DepthAtlasTexture->Desc.Extent.X, Resources->DepthAtlasTexture->Desc.Extent.Y);
		AltasOffset = FVector2f(ShadowData->AtlasRect.Min.X / float(AtlasResolution.X), ShadowData->AtlasRect.Min.Y / float(AtlasResolution.Y));
		AltasScale = FVector2f((ShadowData->AtlasRect.Max.X - ShadowData->AtlasRect.Min.X) / float(AtlasResolution.X), (ShadowData->AtlasRect.Max.Y - ShadowData->AtlasRect.Min.Y) / float(AtlasResolution.Y));
	}

	const FIntRect Viewport = View->ViewRect;
	const FIntPoint Resolution(Viewport.Width(), Viewport.Height());

	FDeepShadowVisualizePS::FParameters* Parameters = GraphBuilder.AllocParameters<FDeepShadowVisualizePS::FParameters>();
	Parameters->DomScale = GDeepShadowDebugScale;
	Parameters->DomAtlasOffset = AltasOffset;
	Parameters->DomAtlasScale = AltasScale;
	Parameters->OutputResolution = Resolution;
	Parameters->InvOutputResolution = FVector2f(1.f / Resolution.X, 1.f / Resolution.Y);
	Parameters->DeepShadowDepthTexture = Resources ? Resources->DepthAtlasTexture : nullptr;
	Parameters->DeepShadowLayerTexture = Resources ? Resources->LayersAtlasTexture : nullptr;
	Parameters->LinearSampler = TStaticSamplerState<SF_Bilinear, AM_Clamp, AM_Clamp, AM_Clamp>::GetRHI();
	Parameters->HairViewRect = FIntVector4(HairViewRect.Min.X, HairViewRect.Min.Y, HairViewRect.Width(), HairViewRect.Height());
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutTarget, ERenderTargetLoadAction::ELoad, 0);
	TShaderMapRef<FPostProcessVS> VertexShader(View->ShaderMap);
	FDeepShadowVisualizePS::FPermutationDomain PermutationVector;
	PermutationVector.Set<FDeepShadowVisualizePS::FOutputType>(ShadowData ? 0 : 1);
	TShaderMapRef<FDeepShadowVisualizePS> PixelShader(View->ShaderMap, PermutationVector);

	ClearUnusedGraphResources(PixelShader, Parameters);

	GraphBuilder.AddPass(
		ShadowData ? RDG_EVENT_NAME("DebugDeepShadowTexture") : RDG_EVENT_NAME("DebugHairViewRect"),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, VertexShader, PixelShader, Viewport, Resolution](FRHICommandList& RHICmdList)
	{
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
class FDeepShadowInfoCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDeepShadowInfoCS);
	SHADER_USE_PARAMETER_STRUCT(FDeepShadowInfoCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderDrawDebug::FShaderParameters, ShaderDrawParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderPrint::FShaderParameters, ShaderPrintParameters)
		SHADER_PARAMETER(FVector2f, OutputResolution)
		SHADER_PARAMETER(uint32, AllocatedSlotCount)
		SHADER_PARAMETER(uint32, MacroGroupCount)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, MacroGroupAABBBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ShadowTranslatedWorldToLightTransformBuffer)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutputTexture)
		END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Tool, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_DOMINFO"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDeepShadowInfoCS, "/Engine/Private/HairStrands/HairStrandsDeepShadowDebug.usf", "MainCS", SF_Compute);

static void AddDeepShadowInfoPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsDeepShadowResources& DeepShadowResources,
	const FHairStrandsMacroGroupResources& MacroGroupResources,
	FRDGTextureRef& OutputTexture)
{
	if (DeepShadowResources.TotalAtlasSlotCount == 0)
	{
		return;
	}

	if (!TryEnableShaderDrawAndShaderPrint(View, DeepShadowResources.TotalAtlasSlotCount * 64, 2000))
	{
		return;
	}

	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

	const FIntPoint Resolution(OutputTexture->Desc.Extent);
	FDeepShadowInfoCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDeepShadowInfoCS::FParameters>();
	Parameters->ViewUniformBuffer = View.ViewUniformBuffer;
	Parameters->OutputResolution = Resolution;
	Parameters->AllocatedSlotCount = DeepShadowResources.TotalAtlasSlotCount;
	Parameters->MacroGroupCount = MacroGroupResources.MacroGroupCount;
	Parameters->SceneTextures = SceneTextures;
	Parameters->MacroGroupAABBBuffer = GraphBuilder.CreateSRV(MacroGroupResources.MacroGroupAABBsBuffer, PF_R32_SINT);
	Parameters->ShadowTranslatedWorldToLightTransformBuffer = GraphBuilder.CreateSRV(DeepShadowResources.DeepShadowTranslatedWorldToLightTransforms);
	ShaderDrawDebug::SetParameters(GraphBuilder, View.ShaderDrawData, Parameters->ShaderDrawParameters);
	ShaderPrint::SetParameters(GraphBuilder, View, Parameters->ShaderPrintParameters);
	Parameters->OutputTexture = GraphBuilder.CreateUAV(OutputTexture);

	TShaderMapRef<FDeepShadowInfoCS> ComputeShader(View.ShaderMap);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HairStrands::DeepShadowDebugInfo"), ComputeShader, Parameters, FIntVector(1, 1, 1));
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FVoxelVirtualRaymarchingCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FVoxelVirtualRaymarchingCS);
	SHADER_USE_PARAMETER_STRUCT(FVoxelVirtualRaymarchingCS, FGlobalShader);

	class FTraversalType : SHADER_PERMUTATION_INT("PERMUTATION_TRAVERSAL", 2);
	using FPermutationDomain = TShaderPermutationDomain<FTraversalType>;
	
	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderDrawDebug::FShaderParameters, ShaderDrawParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderPrint::FShaderParameters, ShaderPrintParameters)
		SHADER_PARAMETER(FVector2f, OutputResolution)
		SHADER_PARAMETER( int32, ForcedMipLevel)
		SHADER_PARAMETER(uint32, bDrawPage)
		SHADER_PARAMETER(uint32, MacroGroupId)
		SHADER_PARAMETER(uint32, MacroGroupCount)
		SHADER_PARAMETER(uint32, MaxTotalPageIndexCount)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, VirtualVoxel)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, TotalValidPageCounter)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, OutputTexture)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Strands, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		// Skip optimization for avoiding long compilation time due to large UAV writes
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Debug);
	}
};

IMPLEMENT_GLOBAL_SHADER(FVoxelVirtualRaymarchingCS, "/Engine/Private/HairStrands/HairStrandsVoxelPageRayMarching.usf", "MainCS", SF_Compute);

static void AddVoxelPageRaymarchingPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsMacroGroupDatas& MacroGroupDatas,
	const FHairStrandsVoxelResources& VoxelResources,
	FRDGTextureRef& OutputTexture)
{
	if (!TryEnableShaderDrawAndShaderPrint(View, 4000, 2000))
	{
		return;
	}

	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

	const FIntPoint Resolution(OutputTexture->Desc.Extent);
	for (const FHairStrandsMacroGroupData& MacroGroupData : MacroGroupDatas)
	{
		FVoxelVirtualRaymarchingCS::FParameters* Parameters = GraphBuilder.AllocParameters<FVoxelVirtualRaymarchingCS::FParameters>();
		Parameters->ViewUniformBuffer		= View.ViewUniformBuffer;
		Parameters->OutputResolution		= Resolution;
		Parameters->SceneTextures			= SceneTextures;
		Parameters->bDrawPage				= FMath::Clamp(GHairVirtualVoxel_DrawDebugPage, 0, 2);
		Parameters->ForcedMipLevel			= FMath::Clamp(GHairVirtualVoxel_ForceMipLevel, -1, 5);
		Parameters->MacroGroupId			= MacroGroupData.MacroGroupId;
		Parameters->MacroGroupCount			= MacroGroupDatas.Num();
		Parameters->MaxTotalPageIndexCount  = VoxelResources.Parameters.Common.PageIndexCount;
		Parameters->VirtualVoxel			= VoxelResources.UniformBuffer;
		Parameters->TotalValidPageCounter	= GraphBuilder.CreateSRV(VoxelResources.PageIndexGlobalCounter, PF_R32_UINT);
		ShaderDrawDebug::SetParameters(GraphBuilder, View.ShaderDrawData, Parameters->ShaderDrawParameters);
		ShaderPrint::SetParameters(GraphBuilder, View, Parameters->ShaderPrintParameters);
		Parameters->OutputTexture			= GraphBuilder.CreateUAV(OutputTexture);

		FVoxelVirtualRaymarchingCS::FPermutationDomain PermutationVector;
		PermutationVector.Set<FVoxelVirtualRaymarchingCS::FTraversalType>(GHairVirtualVoxel_DebugTraversalType > 0 ? 1 : 0);
		TShaderMapRef<FVoxelVirtualRaymarchingCS> ComputeShader(View.ShaderMap, PermutationVector);

		const FIntVector DispatchCount = DispatchCount.DivideAndRoundUp(FIntVector(OutputTexture->Desc.Extent.X, OutputTexture->Desc.Extent.Y, 1), FIntVector(8, 8, 1));
		FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HairStrands::VoxelVirtualRaymarching"), ComputeShader, Parameters, DispatchCount);
	}
}
///////////////////////////////////////////////////////////////////////////////////////////////////

class FDebugHairTangentCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDebugHairTangentCS);
	SHADER_USE_PARAMETER_STRUCT(FDebugHairTangentCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderDrawDebug::FShaderParameters, ShaderDraw)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderPrint::FShaderParameters, ShaderPrint)
		SHADER_PARAMETER(FVector2f, OutputResolution)
		SHADER_PARAMETER(FIntPoint, TileCount)
		SHADER_PARAMETER(uint32, TileSize)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_SAMPLER(SamplerState, BilinearTextureSampler)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Tool, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_TANGENT"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDebugHairTangentCS, "/Engine/Private/HairStrands/HairStrandsDebugPrint.usf", "MainCS", SF_Compute);

static void AddDebugHairTangentPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FSceneTextures& SceneTextures,
	FRDGTextureRef& OutputTexture)
{
	ShaderDrawDebug::SetEnabled(true);

	FDebugHairTangentCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDebugHairTangentCS::FParameters>();
	Parameters->ViewUniformBuffer		= View.ViewUniformBuffer;
	Parameters->HairStrands				= View.HairStrandsViewData.UniformBuffer;
	Parameters->OutputResolution		= OutputTexture->Desc.Extent;
	Parameters->TileSize				= FMath::Clamp(GHairTangentDebug_TileSize, 4, 32);
	Parameters->TileCount				= FIntPoint(FMath::FloorToInt(Parameters->OutputResolution.X / Parameters->TileSize), FMath::FloorToInt(Parameters->OutputResolution.X / Parameters->TileSize));
	Parameters->SceneTextures			= SceneTextures.UniformBuffer;
	Parameters->BilinearTextureSampler	= TStaticSamplerState<SF_Bilinear>::GetRHI();
	ShaderDrawDebug::SetParameters(GraphBuilder, View.ShaderDrawData, Parameters->ShaderDraw);
	ShaderPrint::SetParameters(GraphBuilder, View, Parameters->ShaderPrint);

	const FIntVector DispatchCount = DispatchCount.DivideAndRoundUp(FIntVector(OutputTexture->Desc.Extent.X, OutputTexture->Desc.Extent.Y, 1), FIntVector(8, 8, 1));
	ShaderDrawDebug::RequestSpaceForElements(DispatchCount.X * DispatchCount.Y);

	TShaderMapRef<FDebugHairTangentCS> ComputeShader(View.ShaderMap);
	FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HairStrands::DebugTangentCS"), ComputeShader, Parameters, DispatchCount);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
class FHairStrandsPlotBSDFPS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairStrandsPlotBSDFPS);
	SHADER_USE_PARAMETER_STRUCT(FHairStrandsPlotBSDFPS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, InputCoord)
		SHADER_PARAMETER(FIntPoint, OutputOffset)
		SHADER_PARAMETER(FIntPoint, OutputResolution)
		SHADER_PARAMETER(FIntPoint, MaxResolution)
		SHADER_PARAMETER(uint32, HairComponents)
		SHADER_PARAMETER(float, Roughness)
		SHADER_PARAMETER(float, BaseColor)
		SHADER_PARAMETER(float, Exposure)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Tool, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_PLOTBSDF"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairStrandsPlotBSDFPS, "/Engine/Private/HairStrands/HairStrandsBsdfPlot.usf", "MainPS", SF_Pixel);

static void AddPlotBSDFPass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	FRDGTextureRef& OutputTexture)
{
	
	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

	const FIntPoint Resolution(OutputTexture->Desc.Extent);
	FHairStrandsPlotBSDFPS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairStrandsPlotBSDFPS::FParameters>();
	Parameters->InputCoord = View.CursorPos;
	Parameters->OutputOffset = FIntPoint(10,100);
	Parameters->OutputResolution = FIntPoint(256, 256);
	Parameters->MaxResolution = OutputTexture->Desc.Extent;
	Parameters->HairComponents = ToBitfield(GetHairComponents());
	Parameters->Roughness = GHairStrandsDebugPlotBsdfRoughness;
	Parameters->BaseColor = GHairStrandsDebugPlotBsdfBaseColor;
	Parameters->Exposure = GHairStrandsDebugPlotBsdfExposure;
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ELoad);

	const FIntPoint OutputResolution = SceneTextures.SceneDepthTexture->Desc.Extent;
	TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FHairStrandsPlotBSDFPS> PixelShader(View.ShaderMap);
	const FGlobalShaderMap* GlobalShaderMap = View.ShaderMap;
	const FIntRect Viewport = View.ViewRect;

	ClearUnusedGraphResources(PixelShader, Parameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrands::BsdfPlot"),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, VertexShader, PixelShader, Viewport, Resolution](FRHICommandList& RHICmdList)
	{
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
class FHairStrandsPlotSamplePS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairStrandsPlotSamplePS);
	SHADER_USE_PARAMETER_STRUCT(FHairStrandsPlotSamplePS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FHairStrandsDebugData::FReadParameters, DebugData)
		SHADER_PARAMETER(FIntPoint, OutputOffset)
		SHADER_PARAMETER(FIntPoint, OutputResolution)
		SHADER_PARAMETER(FIntPoint, MaxResolution)
		SHADER_PARAMETER(uint32, HairComponents)
		SHADER_PARAMETER(float, Exposure)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		RENDER_TARGET_BINDING_SLOTS()
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Tool, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_PLOTSAMPLE"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FHairStrandsPlotSamplePS, "/Engine/Private/HairStrands/HairStrandsBsdfPlot.usf", "MainPS", SF_Pixel);

static void AddPlotSamplePass(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FHairStrandsDebugData::Data& DebugData,
	FRDGTextureRef& OutputTexture)
{
	FSceneTextureParameters SceneTextures = GetSceneTextureParameters(GraphBuilder);

	const FIntPoint Resolution(OutputTexture->Desc.Extent);
	FHairStrandsPlotSamplePS::FParameters* Parameters = GraphBuilder.AllocParameters<FHairStrandsPlotSamplePS::FParameters>();

	FHairStrandsDebugData::SetParameters(GraphBuilder, DebugData, Parameters->DebugData);
	Parameters->ViewUniformBuffer = View.ViewUniformBuffer;
	Parameters->OutputOffset = FIntPoint(100, 100);
	Parameters->OutputResolution = FIntPoint(256, 256);
	Parameters->MaxResolution = OutputTexture->Desc.Extent;
	Parameters->HairComponents = ToBitfield(GetHairComponents());
	Parameters->Exposure = GHairStrandsDebugPlotBsdfExposure;
	Parameters->RenderTargets[0] = FRenderTargetBinding(OutputTexture, ERenderTargetLoadAction::ELoad);

	const FIntPoint OutputResolution = SceneTextures.SceneDepthTexture->Desc.Extent;
	TShaderMapRef<FPostProcessVS> VertexShader(View.ShaderMap);
	TShaderMapRef<FHairStrandsPlotSamplePS> PixelShader(View.ShaderMap);
	const FGlobalShaderMap* GlobalShaderMap = View.ShaderMap;
	const FIntRect Viewport = View.ViewRect;

	ClearUnusedGraphResources(PixelShader, Parameters);

	GraphBuilder.AddPass(
		RDG_EVENT_NAME("HairStrands::SamplePlot"),
		Parameters,
		ERDGPassFlags::Raster,
		[Parameters, VertexShader, PixelShader, Viewport, Resolution](FRHICommandList& RHICmdList)
	{
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

class FHairVisibilityDebugPPLLCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FHairVisibilityDebugPPLLCS);
	SHADER_USE_PARAMETER_STRUCT(FHairVisibilityDebugPPLLCS, FGlobalShader);

	using FPermutationDomain = TShaderPermutationDomain<>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER(float, PPLLMeanListElementCountPerPixel)
		SHADER_PARAMETER(float, PPLLMaxTotalListElementCount)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PPLLCounter)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, PPLLNodeIndex)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, PPLLNodeData)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D, SceneColorTextureUAV)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderPrint::FShaderParameters, ShaderPrintParameters)
	END_SHADER_PARAMETER_STRUCT()

		static FPermutationDomain RemapPermutation(FPermutationDomain PermutationVector)
	{
		return PermutationVector;
	}

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return IsHairStrandsSupported(EHairStrandsShaderType::Tool, Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("DEBUG_PPLL_PS"), 1);
		// Skip optimization for avoiding long compilation time due to large UAV writes
		OutEnvironment.CompilerFlags.Add(CFLAG_Debug);
	}
};
IMPLEMENT_GLOBAL_SHADER(FHairVisibilityDebugPPLLCS, "/Engine/Private/HairStrands/HairStrandsVisibilityPPLLDebug.usf", "VisibilityDebugPPLLCS", SF_Compute);

///////////////////////////////////////////////////////////////////////////////////////////////////

class FDrawDebugClusterAABBCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FDrawDebugClusterAABBCS);
	SHADER_USE_PARAMETER_STRUCT(FDrawDebugClusterAABBCS, FGlobalShader);

	class FDebugAABBBuffer : SHADER_PERMUTATION_INT("PERMUTATION_DEBUGAABBBUFFER", 2);
	using FPermutationDomain = TShaderPermutationDomain<FDebugAABBBuffer>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, ViewUniformBuffer)
		SHADER_PARAMETER_SRV(Buffer, ClusterAABBBuffer)
		SHADER_PARAMETER_SRV(Buffer, GroupAABBBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(Buffer, CulledDispatchIndirectParametersClusterCountBuffer)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer, ClusterDebugInfoBuffer)
		SHADER_PARAMETER_SRV(Buffer, CulledDrawIndirectParameters)
		SHADER_PARAMETER(uint32, ClusterCount)
		SHADER_PARAMETER(uint32, TriangleCount)
		SHADER_PARAMETER(uint32, HairGroupId)
		SHADER_PARAMETER(int32, ClusterDebugMode)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderDrawDebug::FShaderParameters, ShaderDrawParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(ShaderPrint::FShaderParameters, ShaderPrintParameters)
	END_SHADER_PARAMETER_STRUCT()

public:
	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters) { return IsHairStrandsSupported(EHairStrandsShaderType::Tool, Parameters.Platform); }
	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("SHADER_DRAWDEBUGAABB"), 1);

		// Skip optimization for avoiding long compilation time due to large UAV writes
		OutEnvironment.CompilerFlags.Add(CFLAG_Debug);
	}
};

IMPLEMENT_GLOBAL_SHADER(FDrawDebugClusterAABBCS, "/Engine/Private/HairStrands/HairStrandsClusterCulling.usf", "MainDrawDebugAABBCS", SF_Compute);

bool IsHairStrandsClusterCullingEnable();

static void AddDrawDebugClusterPass(
	FRDGBuilder& GraphBuilder,
	const FHairStrandClusterData& HairClusterData,
	const FViewInfo& View)
{
	const bool bDebugEnable = IsHairStrandsClusterDebugAABBEnable();
	const bool bCullingEnable = IsHairStrandsClusterCullingEnable();
	if (!bDebugEnable || !bCullingEnable)
	{
		return;
	}
	
	if (!TryEnableShaderDrawAndShaderPrint(View, 5000, 2000))
	{
		return;
	}

	{
		{
			for (const FHairStrandsMacroGroupData& MacroGroupData : View.HairStrandsViewData.MacroGroupDatas)
			{
				const bool bDebugAABB = IsHairStrandsClusterDebugAABBEnable();

				for (const FHairStrandsMacroGroupData::PrimitiveInfo& PrimitiveInfo : MacroGroupData.PrimitivesInfos)
				{
					check(PrimitiveInfo.Mesh && PrimitiveInfo.Mesh->Elements.Num() > 0);

					for (int DataIndex = 0; DataIndex < HairClusterData.HairGroups.Num(); ++DataIndex)
					{
						const FHairStrandClusterData::FHairGroup& HairGroupClusters = HairClusterData.HairGroups[DataIndex];

						// Find a better/less hacky way
						if (PrimitiveInfo.PublicDataPtr != HairGroupClusters.HairGroupPublicPtr)
							continue;

						if (ShaderDrawDebug::IsEnabled(View) && HairGroupClusters.CulledClusterCountBuffer)
						{
							FRDGExternalBuffer& DrawIndirectBuffer = HairGroupClusters.HairGroupPublicPtr->GetDrawIndirectBuffer();

							FDrawDebugClusterAABBCS::FPermutationDomain Permutation;
							Permutation.Set<FDrawDebugClusterAABBCS::FDebugAABBBuffer>(bDebugAABB ? 1 : 0);
							TShaderMapRef<FDrawDebugClusterAABBCS> ComputeShader(View.ShaderMap, Permutation);

							FDrawDebugClusterAABBCS::FParameters* Parameters = GraphBuilder.AllocParameters<FDrawDebugClusterAABBCS::FParameters>();
							Parameters->ViewUniformBuffer = View.ViewUniformBuffer;
							Parameters->ClusterCount = HairGroupClusters.ClusterCount;
							Parameters->TriangleCount = HairGroupClusters.VertexCount * 2; // VertexCount is actually the number of control points
							Parameters->HairGroupId = DataIndex;
							Parameters->ClusterDebugMode = GHairStrandsClusterDebug;
							Parameters->ClusterAABBBuffer = HairGroupClusters.ClusterAABBBuffer->SRV;
							Parameters->CulledDispatchIndirectParametersClusterCountBuffer = GraphBuilder.CreateSRV(HairGroupClusters.CulledClusterCountBuffer, EPixelFormat::PF_R32_UINT);
							Parameters->CulledDrawIndirectParameters = DrawIndirectBuffer.SRV;
							Parameters->GroupAABBBuffer = HairGroupClusters.GroupAABBBuffer->SRV;

							if (HairGroupClusters.ClusterDebugInfoBuffer && bDebugAABB)
							{
								FRDGBufferRef ClusterDebugInfoBuffer = GraphBuilder.RegisterExternalBuffer(HairGroupClusters.ClusterDebugInfoBuffer);
								Parameters->ClusterDebugInfoBuffer = GraphBuilder.CreateSRV(ClusterDebugInfoBuffer);
							}
							ShaderDrawDebug::SetParameters(GraphBuilder, View.ShaderDrawData, Parameters->ShaderDrawParameters);
							ShaderPrint::SetParameters(GraphBuilder, View, Parameters->ShaderPrintParameters);

							check(Parameters->ClusterCount / 64 <= 65535);
							const FIntVector DispatchCount = DispatchCount.DivideAndRoundUp(FIntVector(Parameters->ClusterCount, 1, 1), FIntVector(64, 1, 1));// FIX ME, this could get over 65535
							FComputeShaderUtils::AddPass(
								GraphBuilder, RDG_EVENT_NAME("DrawDebugClusterAABB"),
								ComputeShader, Parameters, DispatchCount);
						}
					}
				}
			}
		}
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////
uint32 GetHairStrandsMeanSamplePerPixel();
static void InternalRenderHairStrandsDebugInfo(
	FRDGBuilder& GraphBuilder,
	FScene* Scene,
	FViewInfo& View,
	const struct FHairStrandClusterData& HairClusterData,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef SceneDepthTexture)
{
	FHairStrandsBookmarkParameters Params = CreateHairStrandsBookmarkParameters(Scene, View);
	Params.SceneColorTexture = SceneColorTexture;
	Params.SceneDepthTexture = SceneDepthTexture;
	if (!Params.HasInstances())
	{
		return;
	}

	const float YStep = 14;
	const float ColumnWidth = 200;

	RDG_EVENT_SCOPE(GraphBuilder, "HairStrandsDebug");

	// Only render debug information for the main view
	const FSceneTextures& SceneTextures = FSceneTextures::Get(GraphBuilder);

	// Bookmark for calling debug rendering from the plugin
	{
		RunHairStrandsBookmark(GraphBuilder, EHairStrandsBookmark::ProcessDebug, Params);
	}

	// Display tangent vector for strands/cards/meshes
	{
		// Check among the hair instances, if hair tangent debug mode is requested
		bool bTangentEnabled = GHairTangentDebug > 0;
		if (!bTangentEnabled)
		{
			for (const FMeshBatchAndRelevance& Mesh : View.HairStrandsMeshElements)
			{
				const FHairGroupPublicData* GroupData = HairStrands::GetHairData(Mesh.Mesh);
				if (GroupData->DebugMode == EHairStrandsDebugMode::RenderHairTangent)
				{
					bTangentEnabled = true;
					break;
				}
			}
		}
		if (!bTangentEnabled)
		{
			for (const FMeshBatchAndRelevance& Mesh : View.HairCardsMeshElements)
			{
				const FHairGroupPublicData* GroupData = HairStrands::GetHairData(Mesh.Mesh);
				if (GroupData->DebugMode == EHairStrandsDebugMode::RenderHairTangent)
				{
					bTangentEnabled = true;
					break;
				}
			}
		}
		if (bTangentEnabled)
		{
			AddDebugHairTangentPass(GraphBuilder, View, SceneTextures, SceneColorTexture);
		}
	}

	// Draw LOD info 
	for (const FMeshBatchAndRelevance& Mesh : View.HairStrandsMeshElements)
	{
		const FHairGroupPublicData* GroupData = HairStrands::GetHairData(Mesh.Mesh);
		if (GroupData->bDebugDrawLODInfo)
		{
			AddPrintLODInfoPass(GraphBuilder, View, GroupData);
		}
	}
	for (const FMeshBatchAndRelevance& Mesh : View.HairCardsMeshElements)
	{
		const FHairGroupPublicData* GroupData = HairStrands::GetHairData(Mesh.Mesh);
		if (GroupData->bDebugDrawLODInfo)
		{
			AddPrintLODInfoPass(GraphBuilder, View, GroupData);
		}
	}


	// Pass this point, all debug rendering concern only hair strands data
	if (!HairStrands::HasViewHairStrandsData(View))
	{
		return;
	}

	const FScreenPassRenderTarget SceneColor(SceneColorTexture, View.ViewRect, ERenderTargetLoadAction::ELoad);

	// Debug mode name only
	const EHairStrandsDebugMode StrandsDebugMode = GetHairStrandsDebugStrandsMode();
	const EHairDebugMode HairDebugMode = GetHairStrandsDebugMode();

	
	const FHairStrandsViewData& HairData = View.HairStrandsViewData;

	if (GHairStrandsDebugPlotBsdf > 0 || HairData.DebugData.IsPlotDataValid())
	{
		if (GHairStrandsDebugPlotBsdf > 0)
		{
			AddPlotBSDFPass(GraphBuilder, View, SceneColorTexture);
		}
		if (HairData.DebugData.IsPlotDataValid())
		{
			AddPlotSamplePass(GraphBuilder, View, HairData.DebugData.Resources, SceneColorTexture);
		}	
	}

	float ClusterY = 38;

	if (HairDebugMode == EHairDebugMode::MacroGroups)
	{
		{		
			AddDebugHairPrintPass(GraphBuilder, &View, HairDebugMode, HairData.VisibilityData, HairData.MacroGroupResources, SceneTextures.Stencil);
		}

		// CPU bound of macro groups
		FViewElementPDI ShadowFrustumPDI(&View, nullptr, nullptr);
		if (HairData.VirtualVoxelResources.IsValid())
		{
			for (const FHairStrandsMacroGroupData& MacroGroupData : HairData.MacroGroupDatas)
			{
				const FBox Bound(MacroGroupData.VirtualVoxelNodeDesc.TranslatedWorldMinAABB, MacroGroupData.VirtualVoxelNodeDesc.TranslatedWorldMaxAABB);
				DrawWireBox(&ShadowFrustumPDI, Bound, FColor::Red, 0);
			}
		}
		#if 0
		AddDrawCanvasPass(GraphBuilder, {}, View, SceneColor, [ClusterY, &MacroGroupDatas, YStep] (FCanvas& Canvas)
		{
			float X = 20;
			float Y = ClusterY;
			FLinearColor InactiveColor(0.5, 0.5, 0.5);
			FLinearColor DebugColor(1, 1, 0);
			FString Line;

			Line = FString::Printf(TEXT("----------------------------------------------------------------"));
			Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);

			Line = FString::Printf(TEXT("Macro group count : %d"), HairData.MacroGroupDatas.Num());
			Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			for (const FHairStrandsMacroGroupData& MacroGroupData : HairData.MacroGroupDatas)
			{
				Line = FString::Printf(TEXT(" %d - Bound Radus: %f.2m (%dx%d)"), MacroGroupData.MacroGroupId, MacroGroupData.Bounds.GetSphere().W);
				Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			}
		});
		#endif
	}

	if (HairDebugMode == EHairDebugMode::DeepOpacityMaps)
	{
		{
			for (const FHairStrandsMacroGroupData& MacroGroup : HairData.MacroGroupDatas)
			{
				if (!HairData.DeepShadowResources.DepthAtlasTexture || !HairData.DeepShadowResources.LayersAtlasTexture)
				{
					continue;
				}

				for (const FHairStrandsDeepShadowData& DeepShadowData : MacroGroup.DeepShadowDatas)
				{
					const uint32 DomIndex = GDeepShadowDebugIndex;
					if (DeepShadowData.AtlasSlotIndex != DomIndex)
						continue;

					AddDebugDeepShadowTexturePass(GraphBuilder, &View, FIntRect(), &DeepShadowData, &HairData.DeepShadowResources, SceneColorTexture);
				}
			}
		}
	}

	// View Rect
	if (IsHairStrandsViewRectOptimEnable() && HairDebugMode == EHairDebugMode::MacroGroupScreenRect)
	{
		{
			for (const FHairStrandsMacroGroupData& MacroGroupData : HairData.MacroGroupDatas)
			{
				AddDebugDeepShadowTexturePass(GraphBuilder, &View, MacroGroupData.ScreenRect, nullptr, nullptr, SceneColorTexture);
			}

			const FIntRect TotalRect = ComputeVisibleHairStrandsMacroGroupsRect(View.ViewRect, HairData.MacroGroupDatas);
			AddDebugDeepShadowTexturePass(GraphBuilder, &View, TotalRect, nullptr, nullptr, SceneColorTexture);
		}
	}
	
	const bool bIsVoxelMode = HairDebugMode == EHairDebugMode::VoxelsDensity || HairDebugMode == EHairDebugMode::VoxelsTangent || HairDebugMode == EHairDebugMode::VoxelsBaseColor || HairDebugMode == EHairDebugMode::VoxelsRoughness;

	// Render Frustum for all lights & macro groups 
	{
		if ((HairDebugMode == EHairDebugMode::LightBounds || HairDebugMode == EHairDebugMode::DeepOpacityMaps))
		{
			{
				if (HairData.DeepShadowResources.bIsGPUDriven)
				{
					AddDeepShadowInfoPass(GraphBuilder, View, HairData.DeepShadowResources, HairData.MacroGroupResources, SceneColorTexture);
				}
			}
		}

		FViewElementPDI ShadowFrustumPDI(&View, nullptr, nullptr);

		// Do not draw the CPU boud. Only GPU bound are drawn using ShaderDrawDebug shader functions.
		#if 0
		// All DOMs
		if (HairDebugMode == EHairDebugMode::LightBounds)
		{
			if (!HairData.DeepShadowResources.bIsGPUDriven)
			{
				for (const FHairStrandsMacroGroupData& MacroGroupData : HairData.MacroGroupDatas)
				{
					for (const FHairStrandsDeepShadowData& DomData : MacroGroupData.DeepShadowDatas)
					{
						DrawFrustumWireframe(&ShadowFrustumPDI, DomData.CPU_TranslatedWorldToLightTransform.Inverse(), FColor::Emerald, 0);
						DrawWireBox(&ShadowFrustumPDI, DomData.Bounds.GetBox(), FColor::Yellow, 0);
					}
				}
			}
		}

		// Current DOM
		if (HairDebugMode == EHairDebugMode::DeepOpacityMaps)
		{
			if (!HairData.DeepShadowResources.bIsGPUDriven)
			{
				const int32 CurrentIndex = FMath::Max(0, GDeepShadowDebugIndex);
				for (const FHairStrandsMacroGroupData& MacroGroupData : HairData.MacroGroupDatas)
				{
					for (const FHairStrandsDeepShadowData& DomData : MacroGroupData.DeepShadowDatas)
					{
						if (DomData.AtlasSlotIndex == CurrentIndex)
						{
							DrawFrustumWireframe(&ShadowFrustumPDI, DomData.CPU_TranslatedWorldToLightTransform.Inverse(), FColor::Emerald, 0);
							DrawWireBox(&ShadowFrustumPDI, DomData.Bounds.GetBox(), FColor::Yellow, 0);
						}
					}
				}
			}
		}

		// Voxelization
		if (bIsVoxelMode)
		{
			if (HairData.VirtualVoxelResources.IsValid())
			{
				for (const FHairStrandsMacroGroupData& MacroGroupData : HairData.MacroGroupDatas)
				{
					const FBox Bound(MacroGroupData.VirtualVoxelNodeDesc.TranslatedWorldMinAABB, MacroGroupData.VirtualVoxelNodeDesc.TranslatedWorldMaxAABB);
					DrawWireBox(&ShadowFrustumPDI, Bound, FColor::Red, 0);
					DrawFrustumWireframe(&ShadowFrustumPDI, MacroGroupData.VirtualVoxelNodeDesc.TranslatedWorldToClip.Inverse(), FColor::Purple, 0);
				}
			}
		}
		#endif
	}
	
	const bool bRunDebugPass =
		HairDebugMode == EHairDebugMode::TAAResolveType ||
		HairDebugMode == EHairDebugMode::SamplePerPixel ||
		HairDebugMode == EHairDebugMode::CoverageType ||
		HairDebugMode == EHairDebugMode::Coverage ||
		HairDebugMode == EHairDebugMode::MaterialDepth ||
		HairDebugMode == EHairDebugMode::MaterialBaseColor ||
		HairDebugMode == EHairDebugMode::MaterialRoughness ||
		HairDebugMode == EHairDebugMode::MaterialSpecular ||
		HairDebugMode == EHairDebugMode::MaterialTangent;
	if (bRunDebugPass)
	{
		{
			AddDebugHairPass(GraphBuilder, &View, HairDebugMode, HairData.VisibilityData, SceneTextures.Stencil, SceneColorTexture);
			AddDebugHairPrintPass(GraphBuilder, &View, HairDebugMode, HairData.VisibilityData, HairData.MacroGroupResources, SceneTextures.Stencil);
		}
	}
	else if (HairDebugMode == EHairDebugMode::Tile && HairData.VisibilityData.TileData.IsValid())
	{
		AddHairStrandsDebugTilePass(GraphBuilder, View, SceneColorTexture, HairData.VisibilityData.TileData);
	}

	if (bIsVoxelMode)
	{
		{
			if (HairData.VirtualVoxelResources.IsValid())
			{
				AddVoxelPageRaymarchingPass(GraphBuilder, View, HairData.MacroGroupDatas, HairData.VirtualVoxelResources, SceneColorTexture);
			}
		}
	}

	{
		if (HairData.DebugData.IsPPLLDataValid()) // Check if PPLL rendering is used and its debug view is enabled.
		{
			const FIntPoint PPLLResolution = HairData.DebugData.PPLLNodeIndexTexture->Desc.Extent;
			FHairVisibilityDebugPPLLCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FHairVisibilityDebugPPLLCS::FParameters>();
			PassParameters->PPLLMeanListElementCountPerPixel = GetHairStrandsMeanSamplePerPixel();
			PassParameters->PPLLMaxTotalListElementCount = HairData.DebugData.PPLLNodeDataBuffer->Desc.NumElements;
			PassParameters->PPLLCounter = HairData.DebugData.PPLLNodeCounterTexture;
			PassParameters->PPLLNodeIndex = HairData.DebugData.PPLLNodeIndexTexture;
			PassParameters->PPLLNodeData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(HairData.DebugData.PPLLNodeDataBuffer));
			PassParameters->ViewUniformBuffer = View.ViewUniformBuffer;
			PassParameters->SceneColorTextureUAV = GraphBuilder.CreateUAV(SceneColorTexture);
			ShaderPrint::SetParameters(GraphBuilder, View, PassParameters->ShaderPrintParameters);

			FHairVisibilityDebugPPLLCS::FPermutationDomain PermutationVector;
			TShaderMapRef<FHairVisibilityDebugPPLLCS> ComputeShader(View.ShaderMap, PermutationVector);
			FIntVector TextureSize = SceneColorTexture->Desc.GetSize(); TextureSize.Z = 1;
			FComputeShaderUtils::AddPass(GraphBuilder, RDG_EVENT_NAME("HairStrands::PPLLDebug"), ComputeShader, PassParameters,
				FIntVector::DivideAndRoundUp(TextureSize, FIntVector(8, 8, 1)));
		}
	}

	if (GHairStrandsClusterDebug > 0)
	{
		AddDrawDebugClusterPass(GraphBuilder, HairClusterData, View);
	}

	// Text
	if (HairDebugMode == EHairDebugMode::LightBounds || HairDebugMode == EHairDebugMode::DeepOpacityMaps)
	{
		AddDrawCanvasPass(GraphBuilder, {}, View, SceneColor, [&HairData, YStep, StrandsDebugMode, HairDebugMode](FCanvas& Canvas)
		{
			const uint32 AtlasTotalSlotCount = FHairStrandsDeepShadowResources::MaxAtlasSlotCount;
			uint32 AtlasAllocatedSlot = 0;
			FIntPoint AtlasSlotResolution = FIntPoint(0, 0);
			FIntPoint AtlasResolution = FIntPoint(0, 0);
			bool bIsGPUDriven = false;
			{
				const FHairStrandsDeepShadowResources& Resources = HairData.DeepShadowResources;
				AtlasResolution = Resources.DepthAtlasTexture ? Resources.DepthAtlasTexture->Desc.Extent : FIntPoint(0, 0);
				AtlasAllocatedSlot = Resources.TotalAtlasSlotCount;
				bIsGPUDriven = Resources.bIsGPUDriven;
			}

			const uint32 DomTextureIndex = GDeepShadowDebugIndex;

			float X = 20;
			float Y = 38;

			FLinearColor DebugColor(1, 1, 0);
			FString Line;

			const FHairComponent HairComponent = GetHairComponents();
			Line = FString::Printf(TEXT("Hair Components : (R=%d, TT=%d, TRT=%d, GS=%d, LS=%d)"), HairComponent.R, HairComponent.TT, HairComponent.TRT, HairComponent.GlobalScattering, HairComponent.LocalScattering);
			Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			Line = FString::Printf(TEXT("----------------------------------------------------------------"));						Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			Line = FString::Printf(TEXT("Debug strands mode : %s"), ToString(StrandsDebugMode));									Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			Line = FString::Printf(TEXT("Voxelization : %s"), IsHairStrandsVoxelizationEnable() ? TEXT("On") : TEXT("Off"));		Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			Line = FString::Printf(TEXT("View rect optim.: %s"), IsHairStrandsViewRectOptimEnable() ? TEXT("On") : TEXT("Off"));	Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			Line = FString::Printf(TEXT("----------------------------------------------------------------"));						Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			Line = FString::Printf(TEXT("DOM Atlas resolution  : %dx%d"), AtlasResolution.X, AtlasResolution.Y);					Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			Line = FString::Printf(TEXT("DOM Atlas slot        : %d/%d"), AtlasAllocatedSlot, AtlasTotalSlotCount);					Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			Line = FString::Printf(TEXT("DOM Texture Index     : %d/%d"), DomTextureIndex, AtlasAllocatedSlot);						Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
			Line = FString::Printf(TEXT("DOM GPU driven        : %s"), bIsGPUDriven ? TEXT("On") : TEXT("Off"));					Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);

			{
				for (const FHairStrandsMacroGroupData& MacroGroupData : HairData.MacroGroupDatas)
				{
					for (const FHairStrandsDeepShadowData& DomData : MacroGroupData.DeepShadowDatas)
					{
						Line = FString::Printf(TEXT(" %d - Bound Radus: %f.2m (%dx%d)"), DomData.AtlasSlotIndex, DomData.Bounds.GetSphere().W / 10.f, DomData.ShadowResolution.X, DomData.ShadowResolution.Y);
						Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), DebugColor);
					}
				}
			}
		});
	}

	if (StrandsDebugMode != EHairStrandsDebugMode::NoneDebug || HairDebugMode != EHairDebugMode::None)
	{
		AddDrawCanvasPass(GraphBuilder, {}, View, SceneColor, [&View, &StrandsDebugMode, YStep, HairDebugMode](FCanvas& Canvas)
		{
			float X = 40;
			float Y = View.ViewRect.Height() - YStep * 3.f;
			FString Line;
			if (StrandsDebugMode != EHairStrandsDebugMode::NoneDebug)
				Line = FString::Printf(TEXT("Hair Debug mode - %s"), ToString(StrandsDebugMode));
			else if (HairDebugMode != EHairDebugMode::None)
				Line = FString::Printf(TEXT("Hair Debug mode - %s"), ToString(HairDebugMode));

			Canvas.DrawShadowedString(X, Y += YStep, *Line, GetStatsFont(), FLinearColor(1, 1, 0));
		});
	}
}

void RenderHairStrandsDebugInfo(
	FRDGBuilder& GraphBuilder,
	FScene* Scene,
	TArrayView<FViewInfo> Views,
	const struct FHairStrandClusterData& HairClusterData,
	FRDGTextureRef SceneColorTexture,
	FRDGTextureRef SceneDepthTexture)
{
	bool bHasHairData = false;
	for (FViewInfo& View : Views)
	{
		InternalRenderHairStrandsDebugInfo(GraphBuilder, Scene, View, HairClusterData, SceneColorTexture, SceneDepthTexture);
	}
}