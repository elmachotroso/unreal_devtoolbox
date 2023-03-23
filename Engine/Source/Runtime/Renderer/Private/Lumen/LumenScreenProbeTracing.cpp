// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LumenScreenProbeTracing.cpp
=============================================================================*/

#include "LumenScreenProbeGather.h"
#include "RendererPrivate.h"
#include "ScenePrivate.h"
#include "SceneUtils.h"
#include "PipelineStateCache.h"
#include "ShaderParameterStruct.h"
#include "PixelShaderUtils.h"
#include "ReflectionEnvironment.h"
#include "DistanceFieldAmbientOcclusion.h"
#include "HairStrands/HairStrandsData.h"

int32 GLumenScreenProbeGatherScreenTraces = 1;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherScreenTraces(
	TEXT("r.Lumen.ScreenProbeGather.ScreenTraces"),
	GLumenScreenProbeGatherScreenTraces,
	TEXT("Whether to trace against the screen before falling back to other tracing methods."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherHierarchicalScreenTraces = 1;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherHierarchicalScreenTraces(
	TEXT("r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal"),
	GLumenScreenProbeGatherHierarchicalScreenTraces,
	TEXT("Whether to use HZB tracing for SSGI instead of fixed step count intersection.  HZB tracing is much more accurate, in particular not missing thin features, but is about ~3x slower."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherHierarchicalScreenTracesMaxIterations = 50;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherHierarchicalScreenTracesMaxIterations(
	TEXT("r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.MaxIterations"),
	GLumenScreenProbeGatherHierarchicalScreenTracesMaxIterations,
	TEXT("Max iterations for HZB tracing."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenScreenProbeGatherRelativeDepthThickness = .02f;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherRelativeDepthThickness(
	TEXT("r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.RelativeDepthThickness"),
	GLumenScreenProbeGatherRelativeDepthThickness,
	TEXT("Determines depth thickness of objects hit by HZB tracing, as a relative depth threshold."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenScreenProbeGatherHistoryDepthTestRelativeThickness = .1f;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherHistoryDepthTestRelativeThickness(
	TEXT("r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.HistoryDepthTestRelativeThickness"),
	GLumenScreenProbeGatherHistoryDepthTestRelativeThickness,
	TEXT("Distance between HZB trace hit and previous frame scene depth from which to allow hits, as a relative depth threshold."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherNumThicknessStepsToDetermineCertainty = 4;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherNumThicknessStepsToDetermineCertainty(
	TEXT("r.Lumen.ScreenProbeGather.ScreenTraces.HZBTraversal.NumThicknessStepsToDetermineCertainty"),
	GLumenScreenProbeGatherNumThicknessStepsToDetermineCertainty,
	TEXT("Number of linear search steps to determine if a hit feature is thin and should be ignored."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherVisualizeTraces = 0;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherVisualizeTraces(
	TEXT("r.Lumen.ScreenProbeGather.VisualizeTraces"),
	GLumenScreenProbeGatherVisualizeTraces,
	TEXT("Whether to visualize traces for the center screen probe, useful for debugging"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherVisualizeTracesFreeze = 0;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherVisualizeTracesFreeze(
	TEXT("r.Lumen.ScreenProbeGather.VisualizeTracesFreeze"),
	GLumenScreenProbeGatherVisualizeTracesFreeze,
	TEXT("Whether to freeze updating the visualize trace data.  Note that no changes to cvars or shaders will propagate until unfrozen."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherHairStrands_VoxelTrace = 1;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherHairStrands_VoxelTrace(
	TEXT("r.Lumen.ScreenProbeGather.HairStrands.VoxelTrace"),
	GLumenScreenProbeGatherHairStrands_VoxelTrace,
	TEXT("Whether to trace against hair voxel structure for hair casting shadow onto opaques."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherHairStrands_ScreenTrace = 1;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherHairStrands_ScreenTrace(
	TEXT("r.Lumen.ScreenProbeGather.HairStrands.ScreenTrace"),
	GLumenScreenProbeGatherHairStrands_ScreenTrace,
	TEXT("Whether to trace against hair depth for hair casting shadow onto opaques."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherScreenTracesMinimumOccupancy = 0;
FAutoConsoleVariableRef CVarLumenScreenProbeGatherScreenTraceMinimumOccupancy(
	TEXT("r.Lumen.ScreenProbeGather.ScreenTraces.MinimumOccupancy"),
	GLumenScreenProbeGatherScreenTracesMinimumOccupancy,
	TEXT("Minimum number of threads still tracing before aborting the trace.  Can be used for scalability to abandon traces that have a disproportionate cost."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

class FClearTracesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FClearTracesCS)
	SHADER_USE_PARAMETER_STRUCT(FClearTracesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FClearTracesCS, "/Engine/Private/Lumen/LumenScreenProbeTracing.usf", "ClearTracesCS", SF_Compute);


class FScreenProbeTraceScreenTexturesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeTraceScreenTexturesCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeTraceScreenTexturesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenHZBScreenTraceParameters, HZBScreenTraceParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FurthestHZBTexture)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, LightingChannelsTexture)
		SHADER_PARAMETER(float, MaxHierarchicalScreenTraceIterations)
		SHADER_PARAMETER(float, RelativeDepthThickness)
		SHADER_PARAMETER(float, HistoryDepthTestRelativeThickness)
		SHADER_PARAMETER(float, NumThicknessStepsToDetermineCertainty)
		SHADER_PARAMETER(uint32, MinimumTracingThreadOccupancy)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
	END_SHADER_PARAMETER_STRUCT()

	class FRadianceCache : SHADER_PERMUTATION_BOOL("RADIANCE_CACHE");
	class FHierarchicalScreenTracing : SHADER_PERMUTATION_BOOL("HIERARCHICAL_SCREEN_TRACING");
	class FStructuredImportanceSampling : SHADER_PERMUTATION_BOOL("STRUCTURED_IMPORTANCE_SAMPLING");
	class FHairStrands : SHADER_PERMUTATION_BOOL("USE_HAIRSTRANDS_SCREEN");
	class FTerminateOnLowOccupancy : SHADER_PERMUTATION_BOOL("TERMINATE_ON_LOW_OCCUPANCY");
	
	using FPermutationDomain = TShaderPermutationDomain<FStructuredImportanceSampling, FHierarchicalScreenTracing, FRadianceCache, FHairStrands, FTerminateOnLowOccupancy>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FTerminateOnLowOccupancy>() && !RHISupportsWaveOperations(Parameters.Platform))
		{
			return false;
		}

		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);

		FPermutationDomain PermutationVector(Parameters.PermutationId);

		if (PermutationVector.Get<FTerminateOnLowOccupancy>())
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_WaveOperations);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeTraceScreenTexturesCS, "/Engine/Private/Lumen/LumenScreenProbeTracing.usf", "ScreenProbeTraceScreenTexturesCS", SF_Compute);

class FScreenProbeCompactTracesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeCompactTracesCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeCompactTracesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
		SHADER_PARAMETER(float, CompactionTracingEndDistanceFromCamera)
		SHADER_PARAMETER(float, CompactionMaxTraceDistance)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWCompactedTraceTexelAllocator)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint>, RWCompactedTraceTexelData)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeCompactTracesCS, "/Engine/Private/Lumen/LumenScreenProbeTracing.usf", "ScreenProbeCompactTracesCS", SF_Compute);


class FSetupCompactedTracesIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSetupCompactedTracesIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FSetupCompactedTracesIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWScreenProbeCompactTracingIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint>, CompactedTraceTexelAllocator)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSetupCompactedTracesIndirectArgsCS, "/Engine/Private/Lumen/LumenScreenProbeTracing.usf", "SetupCompactedTracesIndirectArgsCS", SF_Compute);

class FScreenProbeTraceMeshSDFsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeTraceMeshSDFsCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeTraceMeshSDFsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenMeshSDFGridParameters, MeshSDFGridParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, HairStrandsVoxel)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCompactedTraceParameters, CompactedTraceParameters)
	END_SHADER_PARAMETER_STRUCT()
		
	class FStructuredImportanceSampling : SHADER_PERMUTATION_BOOL("STRUCTURED_IMPORTANCE_SAMPLING");
	class FHairStrands : SHADER_PERMUTATION_BOOL("USE_HAIRSTRANDS_VOXEL");
	class FTraceMeshSDFs : SHADER_PERMUTATION_BOOL("SCENE_TRACE_MESH_SDFS");
	class FTraceHeightfields : SHADER_PERMUTATION_BOOL("SCENE_TRACE_HEIGHTFIELDS");
	using FPermutationDomain = TShaderPermutationDomain<FStructuredImportanceSampling, FHairStrands, FTraceMeshSDFs, FTraceHeightfields>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);

		// Get data from GPUSceneParameters rather than View.
		OutEnvironment.SetDefine(TEXT("USE_GLOBAL_GPU_SCENE_DATA"), 1);
		OutEnvironment.SetDefine(TEXT("VF_SUPPORTS_PRIMITIVE_SCENE_DATA"), 1);
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeTraceMeshSDFsCS, "/Engine/Private/Lumen/LumenScreenProbeTracing.usf", "ScreenProbeTraceMeshSDFsCS", SF_Compute);


class FScreenProbeTraceVoxelsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeTraceVoxelsCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeTraceVoxelsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenCardTracingParameters, TracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenIndirectTracingParameters, IndirectTracingParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheInterpolationParameters, RadianceCacheParameters)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FVirtualVoxelParameters, HairStrandsVoxel)
		SHADER_PARAMETER_STRUCT_INCLUDE(FCompactedTraceParameters, CompactedTraceParameters)
	END_SHADER_PARAMETER_STRUCT()

	class FDynamicSkyLight : SHADER_PERMUTATION_BOOL("ENABLE_DYNAMIC_SKY_LIGHT");
	class FTraceDistantScene : SHADER_PERMUTATION_BOOL("TRACE_DISTANT_SCENE");
	class FRadianceCache : SHADER_PERMUTATION_BOOL("RADIANCE_CACHE");
	class FStructuredImportanceSampling : SHADER_PERMUTATION_BOOL("STRUCTURED_IMPORTANCE_SAMPLING");
	class FHairStrands : SHADER_PERMUTATION_BOOL("USE_HAIRSTRANDS_VOXEL");
	class FTraceVoxels : SHADER_PERMUTATION_BOOL("TRACE_VOXELS");
	using FPermutationDomain = TShaderPermutationDomain<FDynamicSkyLight, FTraceDistantScene, FRadianceCache, FStructuredImportanceSampling, FHairStrands, FTraceVoxels>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);

		// Workaround for an internal PC FXC compiler crash when compiling with disabled optimizations
		if (Parameters.Platform == SP_PCD3D_SM5)
		{
			OutEnvironment.CompilerFlags.Add(CFLAG_ForceOptimization);
		}
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeTraceVoxelsCS, "/Engine/Private/Lumen/LumenScreenProbeTracing.usf", "ScreenProbeTraceVoxelsCS", SF_Compute);



class FScreenProbeSetupVisualizeTracesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeSetupVisualizeTracesCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeSetupVisualizeTracesCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<float4>, RWVisualizeTracesData)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
	END_SHADER_PARAMETER_STRUCT()
		
	class FStructuredImportanceSampling : SHADER_PERMUTATION_BOOL("STRUCTURED_IMPORTANCE_SAMPLING");
	using FPermutationDomain = TShaderPermutationDomain<FStructuredImportanceSampling>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static int32 GetGroupSize() 
	{
		return 8;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeSetupVisualizeTracesCS, "/Engine/Private/Lumen/LumenScreenProbeTracing.usf", "ScreenProbeSetupVisualizeTraces", SF_Compute);

TRefCountPtr<FRDGPooledBuffer> GVisualizeTracesData;

void SetupVisualizeTraces(
	FRDGBuilder& GraphBuilder,
	const FScene* Scene,
	const FViewInfo& View,
	const FScreenProbeParameters& ScreenProbeParameters)
{
	FRDGBufferRef VisualizeTracesData = nullptr;

	if (GVisualizeTracesData.IsValid())
	{
		VisualizeTracesData = GraphBuilder.RegisterExternalBuffer(GVisualizeTracesData);
	}
	
	const int32 VisualizeBufferNumElements = ScreenProbeParameters.ScreenProbeTracingOctahedronResolution * ScreenProbeParameters.ScreenProbeTracingOctahedronResolution * 3;
	bool bShouldUpdate = !GLumenScreenProbeGatherVisualizeTracesFreeze;

	if (!VisualizeTracesData || VisualizeTracesData->Desc.NumElements != VisualizeBufferNumElements)
	{
		VisualizeTracesData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateBufferDesc(sizeof(FVector4f), VisualizeBufferNumElements), TEXT("VisualizeTracesData"));
		bShouldUpdate = true;
	}

	if (bShouldUpdate)
	{
		FScreenProbeSetupVisualizeTracesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeSetupVisualizeTracesCS::FParameters>();
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->ScreenProbeParameters = ScreenProbeParameters;
		PassParameters->RWVisualizeTracesData = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(VisualizeTracesData, PF_A32B32G32R32F));

		FScreenProbeSetupVisualizeTracesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FScreenProbeSetupVisualizeTracesCS::FStructuredImportanceSampling >(LumenScreenProbeGather::UseImportanceSampling(View));
		auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeSetupVisualizeTracesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetupVisualizeTraces"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(FIntPoint(ScreenProbeParameters.ScreenProbeTracingOctahedronResolution), FScreenProbeSetupVisualizeTracesCS::GetGroupSize()));

		GVisualizeTracesData = GraphBuilder.ConvertToExternalBuffer(VisualizeTracesData);
	}
}

void GetScreenProbeVisualizeTracesBuffer(TRefCountPtr<FRDGPooledBuffer>& VisualizeTracesData)
{
	if (GLumenScreenProbeGatherVisualizeTraces && GVisualizeTracesData.IsValid())
	{
		VisualizeTracesData = GVisualizeTracesData;
	}
}

FCompactedTraceParameters CompactTraces(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View, 
	const FScreenProbeParameters& ScreenProbeParameters,
	float CompactionTracingEndDistanceFromCamera,
	float CompactionMaxTraceDistance)
{
	const FIntPoint ScreenProbeTraceBufferSize = ScreenProbeParameters.ScreenProbeAtlasBufferSize * ScreenProbeParameters.ScreenProbeTracingOctahedronResolution;
	FCompactedTraceParameters CompactedTraceParameters;
	FRDGBufferRef CompactedTraceTexelAllocator = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("Lumen.ScreenProbeGather.CompactedTraceTexelAllocator"));
	const int32 NumCompactedTraceTexelDataElements = ScreenProbeTraceBufferSize.X * ScreenProbeTraceBufferSize.Y;
	FRDGBufferRef CompactedTraceTexelData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32) * 2, NumCompactedTraceTexelDataElements), TEXT("Lumen.ScreenProbeGather.CompactedTraceTexelData"));

	CompactedTraceParameters.IndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>(1), TEXT("Lumen.ScreenProbeGather.CompactTracingIndirectArgs"));

	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(CompactedTraceTexelAllocator, PF_R32_UINT), 0);

	{
		FScreenProbeCompactTracesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeCompactTracesCS::FParameters>();
		PassParameters->ScreenProbeParameters = ScreenProbeParameters;
		PassParameters->RWCompactedTraceTexelAllocator = GraphBuilder.CreateUAV(CompactedTraceTexelAllocator, PF_R32_UINT);
		PassParameters->RWCompactedTraceTexelData = GraphBuilder.CreateUAV(CompactedTraceTexelData, PF_R32G32_UINT);
		PassParameters->CompactionTracingEndDistanceFromCamera = CompactionTracingEndDistanceFromCamera;
		PassParameters->CompactionMaxTraceDistance = CompactionMaxTraceDistance;

		auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeCompactTracesCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("CompactTraces"),
			ComputeShader,
			PassParameters,
			ScreenProbeParameters.ProbeIndirectArgs,
			(uint32)EScreenProbeIndirectArgs::ThreadPerTrace * sizeof(FRHIDispatchIndirectParameters));
	}

	{
		FSetupCompactedTracesIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSetupCompactedTracesIndirectArgsCS::FParameters>();
		PassParameters->ScreenProbeParameters = ScreenProbeParameters;
		PassParameters->RWScreenProbeCompactTracingIndirectArgs = GraphBuilder.CreateUAV(CompactedTraceParameters.IndirectArgs, PF_R32_UINT);
		PassParameters->CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelAllocator, PF_R32_UINT));

		auto ComputeShader = View.ShaderMap->GetShader<FSetupCompactedTracesIndirectArgsCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetupCompactedTracesIndirectArgs"),
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1));
	}

	CompactedTraceParameters.CompactedTraceTexelAllocator = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelAllocator, PF_R32_UINT));
	CompactedTraceParameters.CompactedTraceTexelData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(CompactedTraceTexelData, PF_R32G32_UINT));

	return CompactedTraceParameters;
}

void TraceScreenProbes(
	FRDGBuilder& GraphBuilder, 
	const FScene* Scene,
	const FViewInfo& View, 
	bool bTraceMeshObjects,
	const FSceneTextures& SceneTextures,
	FRDGTextureRef LightingChannelsTexture,
	const FLumenCardTracingInputs& TracingInputs,
	const LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters,
	FScreenProbeParameters& ScreenProbeParameters,
	FLumenMeshSDFGridParameters& MeshSDFGridParameters)
{
	const FSceneTextureParameters& SceneTextureParameters = GetSceneTextureParameters(GraphBuilder, SceneTextures);

	{
		FClearTracesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FClearTracesCS::FParameters>();
		PassParameters->ScreenProbeParameters = ScreenProbeParameters;

		auto ComputeShader = View.ShaderMap->GetShader<FClearTracesCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("ClearTraces %ux%u", ScreenProbeParameters.ScreenProbeTracingOctahedronResolution, ScreenProbeParameters.ScreenProbeTracingOctahedronResolution),
			ComputeShader,
			PassParameters,
			ScreenProbeParameters.ProbeIndirectArgs,
			(uint32)EScreenProbeIndirectArgs::ThreadPerTrace * sizeof(FRHIDispatchIndirectParameters));
	}

	FLumenIndirectTracingParameters IndirectTracingParameters;
	SetupLumenDiffuseTracingParameters(View, IndirectTracingParameters);

	extern int32 GLumenVisualizeIndirectDiffuse;
	const bool bTraceScreen = View.PrevViewInfo.ScreenSpaceRayTracingInput.IsValid()
		&& GLumenScreenProbeGatherScreenTraces != 0
		&& GLumenVisualizeIndirectDiffuse == 0
		&& View.Family->EngineShowFlags.LumenScreenTraces;

	if (bTraceScreen)
	{
		FScreenProbeTraceScreenTexturesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeTraceScreenTexturesCS::FParameters>();

		PassParameters->HZBScreenTraceParameters = SetupHZBScreenTraceParameters(GraphBuilder, View, SceneTextures);
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTextures = SceneTextureParameters;

		if (PassParameters->HZBScreenTraceParameters.PrevSceneColorTexture == SceneTextures.Color.Resolve || !PassParameters->SceneTextures.GBufferVelocityTexture)
		{
			PassParameters->SceneTextures.GBufferVelocityTexture = GSystemTextures.GetBlackDummy(GraphBuilder);
		}

		PassParameters->FurthestHZBTexture = View.HZB;
		PassParameters->LightingChannelsTexture = LightingChannelsTexture;
		PassParameters->MaxHierarchicalScreenTraceIterations = GLumenScreenProbeGatherHierarchicalScreenTracesMaxIterations;
		PassParameters->RelativeDepthThickness = GLumenScreenProbeGatherRelativeDepthThickness;
		PassParameters->HistoryDepthTestRelativeThickness = GLumenScreenProbeGatherHistoryDepthTestRelativeThickness;
		PassParameters->NumThicknessStepsToDetermineCertainty = GLumenScreenProbeGatherNumThicknessStepsToDetermineCertainty;
		PassParameters->MinimumTracingThreadOccupancy = GLumenScreenProbeGatherScreenTracesMinimumOccupancy;

		PassParameters->ScreenProbeParameters = ScreenProbeParameters;
		PassParameters->IndirectTracingParameters = IndirectTracingParameters;
		PassParameters->RadianceCacheParameters = RadianceCacheParameters;

		const bool bHasHairStrands = HairStrands::HasViewHairStrandsData(View) && GLumenScreenProbeGatherHairStrands_ScreenTrace > 0;
		if (bHasHairStrands)
		{
			PassParameters->HairStrands = HairStrands::BindHairStrandsViewUniformParameters(View);
		}

		const bool bTerminateOnLowOccupancy = GLumenScreenProbeGatherScreenTracesMinimumOccupancy > 0
			&& GRHISupportsWaveOperations 
			&& GRHIMinimumWaveSize >= 32 
			&& RHISupportsWaveOperations(View.GetShaderPlatform());

		FScreenProbeTraceScreenTexturesCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FScreenProbeTraceScreenTexturesCS::FRadianceCache >(LumenScreenProbeGather::UseRadianceCache(View));
		PermutationVector.Set< FScreenProbeTraceScreenTexturesCS::FHierarchicalScreenTracing >(GLumenScreenProbeGatherHierarchicalScreenTraces != 0);
		PermutationVector.Set< FScreenProbeTraceScreenTexturesCS::FStructuredImportanceSampling >(LumenScreenProbeGather::UseImportanceSampling(View));
		PermutationVector.Set< FScreenProbeTraceScreenTexturesCS::FHairStrands>(bHasHairStrands);
		PermutationVector.Set< FScreenProbeTraceScreenTexturesCS::FTerminateOnLowOccupancy>(bTerminateOnLowOccupancy);
		
		auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeTraceScreenTexturesCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("TraceScreen(%s)", bHasHairStrands ? TEXT("Scene, HairStrands") : TEXT("Scene")),
			ComputeShader,
			PassParameters,
			ScreenProbeParameters.ProbeIndirectArgs,
			(uint32)EScreenProbeIndirectArgs::ThreadPerTrace * sizeof(FRHIDispatchIndirectParameters));
	}

	bool bNeedTraceHairVoxel = HairStrands::HasViewHairStrandsVoxelData(View) && GLumenScreenProbeGatherHairStrands_VoxelTrace > 0;
	const bool bUseHardwareRayTracing = Lumen::UseHardwareRayTracedScreenProbeGather();

	if (bUseHardwareRayTracing)
	{
		FCompactedTraceParameters CompactedTraceParameters = CompactTraces(
			GraphBuilder,
			View,
			ScreenProbeParameters,
			Lumen::MaxTracingEndDistanceFromCamera,
			IndirectTracingParameters.MaxTraceDistance);

		RenderHardwareRayTracingScreenProbe(GraphBuilder,
			Scene,
			SceneTextureParameters,
			ScreenProbeParameters,
			View,
			TracingInputs,
			IndirectTracingParameters,
			RadianceCacheParameters,
			CompactedTraceParameters);
	}
	else if (bTraceMeshObjects)
	{
		CullForCardTracing(
			GraphBuilder,
			Scene, View,
			TracingInputs,
			IndirectTracingParameters,
			/* out */ MeshSDFGridParameters);

		const bool bTraceMeshSDFs = MeshSDFGridParameters.TracingParameters.DistanceFieldObjectBuffers.NumSceneObjects > 0;
		const bool bTraceHeightfields = Lumen::UseHeightfieldTracing(*View.Family, *Scene->LumenSceneData);

		if (bTraceMeshSDFs || bTraceHeightfields)
		{
			FCompactedTraceParameters CompactedTraceParameters = CompactTraces(
				GraphBuilder,
				View,
				ScreenProbeParameters,
				IndirectTracingParameters.CardTraceEndDistanceFromCamera,
				IndirectTracingParameters.MaxMeshSDFTraceDistance);

			{
				FScreenProbeTraceMeshSDFsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeTraceMeshSDFsCS::FParameters>();
				GetLumenCardTracingParameters(View, TracingInputs, PassParameters->TracingParameters);
				PassParameters->MeshSDFGridParameters = MeshSDFGridParameters;
				PassParameters->ScreenProbeParameters = ScreenProbeParameters;
				PassParameters->IndirectTracingParameters = IndirectTracingParameters;
				PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
				PassParameters->CompactedTraceParameters = CompactedTraceParameters;
				if (bNeedTraceHairVoxel)
				{
					PassParameters->HairStrandsVoxel = HairStrands::BindHairStrandsVoxelUniformParameters(View);
				}

				FScreenProbeTraceMeshSDFsCS::FPermutationDomain PermutationVector;
				PermutationVector.Set< FScreenProbeTraceMeshSDFsCS::FStructuredImportanceSampling >(LumenScreenProbeGather::UseImportanceSampling(View));
				PermutationVector.Set< FScreenProbeTraceMeshSDFsCS::FHairStrands >(bNeedTraceHairVoxel);
				PermutationVector.Set< FScreenProbeTraceMeshSDFsCS::FTraceMeshSDFs >(bTraceMeshSDFs);
				PermutationVector.Set< FScreenProbeTraceMeshSDFsCS::FTraceHeightfields >(bTraceHeightfields);
				auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeTraceMeshSDFsCS>(PermutationVector);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("TraceMeshSDFs(%s)", bNeedTraceHairVoxel ? TEXT("Scene, HairStrands") : TEXT("Scene")),
					ComputeShader,
					PassParameters,
					CompactedTraceParameters.IndirectArgs,
					0);
				bNeedTraceHairVoxel = false;
			}
		}
	}

	FCompactedTraceParameters CompactedTraceParameters = CompactTraces(
		GraphBuilder,
		View,
		ScreenProbeParameters,
		Lumen::MaxTracingEndDistanceFromCamera,
		// Make sure the shader runs on all misses to apply radiance cache + skylight
		IndirectTracingParameters.MaxTraceDistance * 2);

	{
		const bool bRadianceCache = LumenScreenProbeGather::UseRadianceCache(View);

		FScreenProbeTraceVoxelsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeTraceVoxelsCS::FParameters>();
		PassParameters->RadianceCacheParameters = RadianceCacheParameters;
		GetLumenCardTracingParameters(View, TracingInputs, PassParameters->TracingParameters);
		PassParameters->ScreenProbeParameters = ScreenProbeParameters;
		PassParameters->IndirectTracingParameters = IndirectTracingParameters;
		PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
		PassParameters->CompactedTraceParameters = CompactedTraceParameters;
		if (bNeedTraceHairVoxel)
		{
			PassParameters->HairStrandsVoxel = HairStrands::BindHairStrandsVoxelUniformParameters(View);
		}

		FScreenProbeTraceVoxelsCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FScreenProbeTraceVoxelsCS::FDynamicSkyLight >(Lumen::ShouldHandleSkyLight(Scene, *View.Family));
		PermutationVector.Set< FScreenProbeTraceVoxelsCS::FTraceDistantScene >(Scene->LumenSceneData->DistantCardIndices.Num() > 0);
		PermutationVector.Set< FScreenProbeTraceVoxelsCS::FRadianceCache >(bRadianceCache);
		PermutationVector.Set< FScreenProbeTraceVoxelsCS::FStructuredImportanceSampling >(LumenScreenProbeGather::UseImportanceSampling(View));
		PermutationVector.Set< FScreenProbeTraceVoxelsCS::FHairStrands>(bNeedTraceHairVoxel);
		PermutationVector.Set< FScreenProbeTraceVoxelsCS::FTraceVoxels>(!bUseHardwareRayTracing && Lumen::UseGlobalSDFTracing(*View.Family));
		auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeTraceVoxelsCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("%s%s", bUseHardwareRayTracing ? TEXT("RadianceCacheInterpolate") : TEXT("TraceVoxels"), bNeedTraceHairVoxel ? TEXT(" and HairStrands") : TEXT("")),
			ComputeShader,
			PassParameters,
			CompactedTraceParameters.IndirectArgs,
			0);
		bNeedTraceHairVoxel = false;
	}

	if (GLumenScreenProbeGatherVisualizeTraces)
	{
		SetupVisualizeTraces(GraphBuilder, Scene, View, ScreenProbeParameters);
	}
}
