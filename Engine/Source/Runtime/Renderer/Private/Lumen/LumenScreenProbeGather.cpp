// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	LumenScreenProbeGather.cpp
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
#include "ScreenSpaceDenoise.h"
#include "HairStrands/HairStrandsEnvironment.h"
#include "Strata/Strata.h"

extern FLumenGatherCvarState GLumenGatherCvars;

int32 GLumenScreenProbeGather = 1;
FAutoConsoleVariableRef GVarLumenScreenProbeGather(
	TEXT("r.Lumen.ScreenProbeGather"),
	GLumenScreenProbeGather,
	TEXT("Whether to use the Screen Probe Final Gather"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

FAutoConsoleVariableRef CVarLumenScreenProbeGatherTraceMeshSDFs(
	TEXT("r.Lumen.ScreenProbeGather.TraceMeshSDFs"),
	GLumenGatherCvars.TraceMeshSDFs,
	TEXT("Whether to trace against Mesh Signed Distance fields for Lumen's Screen Probe Gather."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherAdaptiveProbeMinDownsampleFactor = 4;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherAdaptiveProbeMinDownsampleFactor(
	TEXT("r.Lumen.ScreenProbeGather.AdaptiveProbeMinDownsampleFactor"),
	GLumenScreenProbeGatherAdaptiveProbeMinDownsampleFactor,
	TEXT("Screen probes will be placed where needed down to this downsample factor of the GBuffer."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenScreenProbeGatherAdaptiveProbeAllocationFraction = .5f;
FAutoConsoleVariableRef GVarAdaptiveProbeAllocationFraction(
	TEXT("r.Lumen.ScreenProbeGather.AdaptiveProbeAllocationFraction"),
	GLumenScreenProbeGatherAdaptiveProbeAllocationFraction,
	TEXT("Fraction of uniform probes to allow for adaptive probe placement."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeGatherReferenceMode = 0;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherReferenceMode(
	TEXT("r.Lumen.ScreenProbeGather.ReferenceMode"),
	GLumenScreenProbeGatherReferenceMode,
	TEXT("When enabled, traces 1024 uniform rays per probe with no filtering, Importance Sampling or Radiance Caching."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeTracingOctahedronResolution = 8;
FAutoConsoleVariableRef GVarLumenScreenProbeTracingOctahedronResolution(
	TEXT("r.Lumen.ScreenProbeGather.TracingOctahedronResolution"),
	GLumenScreenProbeTracingOctahedronResolution,
	TEXT("Resolution of the tracing octahedron.  Determines how many traces are done per probe."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenScreenProbeGatherOctahedronResolutionScale = 1.0f;
FAutoConsoleVariableRef GVarLumenScreenProbeGatherOctahedronResolutionScale(
	TEXT("r.Lumen.ScreenProbeGather.GatherOctahedronResolutionScale"),
	GLumenScreenProbeGatherOctahedronResolutionScale,
	TEXT("Resolution that probe filtering and integration will happen at, as a scale of TracingOctahedronResolution"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeDownsampleFactor = 16;
FAutoConsoleVariableRef GVarLumenScreenProbeDownsampleFactor(
	TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"),
	GLumenScreenProbeDownsampleFactor,
	TEXT("Pixel size of the screen tile that a screen probe will be placed on."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenOctahedralSolidAngleTextureSize = 16;
FAutoConsoleVariableRef CVarLumenScreenProbeOctahedralSolidAngleTextureSize(
	TEXT("r.Lumen.ScreenProbeGather.OctahedralSolidAngleTextureSize"),
	GLumenOctahedralSolidAngleTextureSize,
	TEXT("Resolution of the lookup texture to compute Octahedral Solid Angle."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

float GLumenScreenProbeFullResolutionJitterWidth = 1;
FAutoConsoleVariableRef GVarLumenScreenProbeFullResolutionJitterWidth(
	TEXT("r.Lumen.ScreenProbeGather.FullResolutionJitterWidth"),
	GLumenScreenProbeFullResolutionJitterWidth,
	TEXT("Size of the full resolution jitter applied to Screen Probe upsampling, as a fraction of a screen tile.  A width of 1 results in jittering by DownsampleFactor number of pixels."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeIntegrationTileClassification = 1;
FAutoConsoleVariableRef CVarLumenScreenProbeIntegrationTileClassification(
	TEXT("r.Lumen.ScreenProbeGather.IntegrationTileClassification"),
	GLumenScreenProbeIntegrationTileClassification,
	TEXT("Whether to use tile classification during diffuse integration.  Tile Classification splits compute dispatches by VGPRs for better occupancy, but can introduce errors if implemented incorrectly."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeDiffuseIntegralMethod = 0;
FAutoConsoleVariableRef CVarLumenScreenProbeDiffuseIntegralMethod(
	TEXT("r.Lumen.ScreenProbeGather.DiffuseIntegralMethod"),
	GLumenScreenProbeDiffuseIntegralMethod,
	TEXT("Spherical Harmonic = 0, Importance Sample BRDF = 1, Numerical Integral Reference = 2"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeTemporalFilter = 1;
FAutoConsoleVariableRef CVarLumenScreenProbeTemporalFilter(
	TEXT("r.Lumen.ScreenProbeGather.Temporal"),
	GLumenScreenProbeTemporalFilter,
	TEXT("Whether to use a temporal filter"),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GLumenScreenProbeClearHistoryEveryFrame = 0;
FAutoConsoleVariableRef CVarLumenScreenProbeClearHistoryEveryFrame(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.ClearHistoryEveryFrame"),
	GLumenScreenProbeClearHistoryEveryFrame,
	TEXT("Whether to clear the history every frame for debugging"),
	ECVF_RenderThreadSafe
	);

int32 GLumenScreenProbeUseHistoryNeighborhoodClamp = 0;
FAutoConsoleVariableRef CVarLumenScreenProbeUseHistoryNeighborhoodClamp(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.NeighborhoodClamp"),
	GLumenScreenProbeUseHistoryNeighborhoodClamp,
	TEXT("Whether to use a neighborhood clamp temporal filter instead of depth rejection.  Experimental."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenScreenProbeHistoryDistanceThreshold = .005f;
FAutoConsoleVariableRef CVarLumenScreenProbeHistoryDistanceThreshold(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.DistanceThreshold"),
	GLumenScreenProbeHistoryDistanceThreshold,
	TEXT("Relative distance threshold needed to discard last frame's lighting results.  Lower values reduce ghosting from characters when near a wall but increase flickering artifacts."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenScreenProbeFractionOfLightingMovingForFastUpdateMode = .1f;
FAutoConsoleVariableRef CVarLumenScreenProbeFractionOfLightingMovingForFastUpdateMode(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.FractionOfLightingMovingForFastUpdateMode"),
	GLumenScreenProbeFractionOfLightingMovingForFastUpdateMode,
	TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenScreenProbeTemporalMaxFastUpdateModeAmount = .9f;
FAutoConsoleVariableRef CVarLumenScreenProbeTemporalMaxFastUpdateModeAmount(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.MaxFastUpdateModeAmount"),
	GLumenScreenProbeTemporalMaxFastUpdateModeAmount,
	TEXT("Maximum amount of fast-responding temporal filter to use when traces hit a moving object.  Values closer to 1 cause more noise, but also faster reaction to scene changes."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GLumenScreenProbeTemporalFastUpdateModeUseNeighborhoodClamp = 0;
FAutoConsoleVariableRef CVarLumenScreenProbeTemporalFastUpdateModeUseNeighborhoodClamp(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.FastUpdateModeUseNeighborhoodClamp"),
	GLumenScreenProbeTemporalFastUpdateModeUseNeighborhoodClamp,
	TEXT("Whether to clamp history values to the current frame's screen space neighborhood, in areas around moving objects."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GLumenScreenProbeTemporalRejectBasedOnNormal = 1;
FAutoConsoleVariableRef CVarLumenScreenProbeTemporalRejectBasedOnNormal(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.RejectBasedOnNormal"),
	GLumenScreenProbeTemporalRejectBasedOnNormal,
	TEXT("Whether to reject history lighting based on their normal.  Increases cost of the temporal filter but can reduce streaking especially around character feet."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenScreenProbeRelativeSpeedDifferenceToConsiderLightingMoving = .005f;
FAutoConsoleVariableRef CVarLumenScreenProbeRelativeSpeedDifferenceToConsiderLightingMoving(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.RelativeSpeedDifferenceToConsiderLightingMoving"),
	GLumenScreenProbeRelativeSpeedDifferenceToConsiderLightingMoving,
	TEXT(""),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenScreenProbeTemporalMaxFramesAccumulated = 10.0f;
FAutoConsoleVariableRef CVarLumenScreenProbeTemporalMaxFramesAccumulated(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.MaxFramesAccumulated"),
	GLumenScreenProbeTemporalMaxFramesAccumulated,
	TEXT("Lower values cause the temporal filter to propagate lighting changes faster, but also increase flickering from noise."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenScreenProbeTemporalHistoryNormalThreshold = 45.0f;
FAutoConsoleVariableRef CVarLumenScreenProbeTemporalHistoryNormalThreshold(
	TEXT("r.Lumen.ScreenProbeGather.Temporal.NormalThreshold"),
	GLumenScreenProbeTemporalHistoryNormalThreshold,
	TEXT("Maximum angle that the history texel's normal can be from the current pixel to accept it's history lighting, in degrees."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

float GLumenScreenProbeScreenTracesThicknessScaleWhenNoFallback = 2;
FAutoConsoleVariableRef CVarLumenScreenProbeScreenTracesThicknessScaleWhenNoFallback(
	TEXT("r.Lumen.ScreenProbeGather.ScreenTraces.ThicknessScaleWhenNoFallback"),
	GLumenScreenProbeScreenTracesThicknessScaleWhenNoFallback,
	TEXT("Larger scales effectively treat depth buffer surfaces as thicker for screen traces when there is no Distance Field present to resume the occluded ray."),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GLumenScreenProbeSpatialFilter = 1;
FAutoConsoleVariableRef GVarLumenScreenProbeFilter(
	TEXT("r.Lumen.ScreenProbeGather.SpatialFilterProbes"),
	GLumenScreenProbeSpatialFilter,
	TEXT("Whether to spatially filter probe traces to reduce noise."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeTemporalFilterProbes = 0;
FAutoConsoleVariableRef GVarLumenScreenProbeTemporalFilter(
	TEXT("r.Lumen.ScreenProbeGather.TemporalFilterProbes"),
	GLumenScreenProbeTemporalFilterProbes,
	TEXT("Whether to temporally filter probe traces to reduce noise."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenSpaceBentNormal = 1;
FAutoConsoleVariableRef GVarLumenScreenSpaceBentNormal(
	TEXT("r.Lumen.ScreenProbeGather.ScreenSpaceBentNormal"),
	GLumenScreenSpaceBentNormal,
	TEXT("Whether to compute screen space directional occlusion to add high frequency occlusion (contact shadows) which Screen Probes lack due to downsampling."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenBentNormalApplyDuringIntegration = 0;
FAutoConsoleVariableRef CVarLumenScreenBentNormalApplyDuringIntegration(
	TEXT("r.Lumen.ScreenProbeGather.ScreenSpaceBentNormal.ApplyDuringIntegration"),
	GLumenScreenBentNormalApplyDuringIntegration,
	TEXT("Whether Screen Space Bent Normal should be applied during BRDF integration, which has higher quality but is before the temporal filter so causes streaking on moving objects."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeFixedJitterIndex = -1;
FAutoConsoleVariableRef CVarLumenScreenProbeUseJitter(
	TEXT("r.Lumen.ScreenProbeGather.FixedJitterIndex"),
	GLumenScreenProbeFixedJitterIndex,
	TEXT("If zero or greater, overrides the temporal jitter index with a fixed index.  Useful for debugging and inspecting sampling patterns."),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenRadianceCache = 1;
FAutoConsoleVariableRef CVarRadianceCache(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache"),
	GLumenRadianceCache,
	TEXT("Whether to enable the Persistent world space Radiance Cache"),
	ECVF_Scalability | ECVF_RenderThreadSafe
	);

int32 GLumenScreenProbeIrradianceFormat = 1;
FAutoConsoleVariableRef CVarLumenScreenProbeIrradianceFormat(
	TEXT("r.Lumen.ScreenProbeGather.IrradianceFormat"),
	GLumenScreenProbeIrradianceFormat,
	TEXT("Prefilter irradiance format\n")
	TEXT("0 - SH3 slower\n")
	TEXT("1 - Octahedral probe. Faster, but reverts to SH3 when ScreenSpaceBentNormal.ApplyDuringIntegration is enabled"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GLumenScreenProbeStochasticInterpolation = 1;
FAutoConsoleVariableRef CVarLumenScreenProbeStochasticInterpolation(
	TEXT("r.Lumen.ScreenProbeGather.StochasticInterpolation"),
	GLumenScreenProbeStochasticInterpolation,
	TEXT("Where to interpolate screen probes stochastically (1 sample) or bilinearly (4 samples)"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

namespace LumenScreenProbeGather 
{
	int32 GetTracingOctahedronResolution(const FViewInfo& View)
	{
		const float SqrtQuality = FMath::Sqrt(FMath::Max(View.FinalPostProcessSettings.LumenFinalGatherQuality, 0.0f));
		const int32 TracingOctahedronResolution = FMath::Clamp(FMath::RoundToInt(SqrtQuality * GLumenScreenProbeTracingOctahedronResolution), 4, 16);
		ensureMsgf(IsProbeTracingResolutionSupportedForImportanceSampling(TracingOctahedronResolution), TEXT("Tracing resolution %u requested that is not supported by importance sampling"), TracingOctahedronResolution);
		return GLumenScreenProbeGatherReferenceMode ? 32 : TracingOctahedronResolution;
	}

	int32 GetGatherOctahedronResolution(int32 TracingOctahedronResolution)
	{
		if (GLumenScreenProbeGatherReferenceMode)
		{
			return 8;
		}

		if (GLumenScreenProbeGatherOctahedronResolutionScale >= 1.0f)
		{
			const int32 Multiplier = FMath::RoundToInt(GLumenScreenProbeGatherOctahedronResolutionScale);
			return TracingOctahedronResolution * Multiplier;
		}
		else
		{
			const int32 Divisor = FMath::RoundToInt(1.0f / FMath::Max(GLumenScreenProbeGatherOctahedronResolutionScale, .1f));
			return TracingOctahedronResolution / Divisor;
		}
	}
	
	int32 GetScreenDownsampleFactor(const FViewInfo& View)
	{
		if (GLumenScreenProbeGatherReferenceMode)
		{
			return 16;
		}

		return FMath::Clamp(GLumenScreenProbeDownsampleFactor / (View.FinalPostProcessSettings.LumenFinalGatherQuality >= 6.0f ? 2 : 1), 4, 64);
	}

	bool UseScreenSpaceBentNormal(const FEngineShowFlags& ShowFlags)
	{
		return GLumenScreenProbeGatherReferenceMode ? false : (GLumenScreenSpaceBentNormal != 0 && ShowFlags.LumenScreenSpaceDirectionalOcclusion);
	}

	bool ApplyScreenBentNormalDuringIntegration()
	{
		return GLumenScreenBentNormalApplyDuringIntegration != 0;
	}

	bool UseProbeSpatialFilter()
	{
		return GLumenScreenProbeGatherReferenceMode ? false : GLumenScreenProbeSpatialFilter != 0;
	}

	bool UseProbeTemporalFilter()
	{
		return GLumenScreenProbeGatherReferenceMode ? false : GLumenScreenProbeTemporalFilterProbes != 0;
	}

	bool UseRadianceCache(const FViewInfo& View)
	{
		return GLumenScreenProbeGatherReferenceMode ? false : GLumenRadianceCache != 0;
	}

	int32 GetDiffuseIntegralMethod()
	{
		return GLumenScreenProbeGatherReferenceMode ? 2 : GLumenScreenProbeDiffuseIntegralMethod;
	}

	EScreenProbeIrradianceFormat GetScreenProbeIrradianceFormat(const FEngineShowFlags& ShowFlags)
	{
		const bool bApplyScreenBentNormal = UseScreenSpaceBentNormal(ShowFlags) && ApplyScreenBentNormalDuringIntegration();
		if (bApplyScreenBentNormal)
		{
			// At the moment only SH3 support bent normal path
			return EScreenProbeIrradianceFormat::SH3;
		}

		return (EScreenProbeIrradianceFormat)FMath::Clamp(GLumenScreenProbeIrradianceFormat, 0, 1);
	}

	float GetScreenProbeFullResolutionJitterWidth(const FViewInfo& View)
	{
		return GLumenScreenProbeFullResolutionJitterWidth * (View.FinalPostProcessSettings.LumenFinalGatherQuality >= 4.0f ? .5f : 1.0f);
	}
}

int32 GRadianceCacheNumClipmaps = 4;
FAutoConsoleVariableRef CVarRadianceCacheNumClipmaps(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.NumClipmaps"),
	GRadianceCacheNumClipmaps,
	TEXT("Number of radiance cache clipmaps."),
	ECVF_RenderThreadSafe
);

float GLumenRadianceCacheClipmapWorldExtent = 2500.0f;
FAutoConsoleVariableRef CVarLumenRadianceCacheClipmapWorldExtent(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.ClipmapWorldExtent"),
	GLumenRadianceCacheClipmapWorldExtent,
	TEXT("World space extent of the first clipmap"),
	ECVF_RenderThreadSafe
);

float GLumenRadianceCacheClipmapDistributionBase = 2.0f;
FAutoConsoleVariableRef CVarLumenRadianceCacheClipmapDistributionBase(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.ClipmapDistributionBase"),
	GLumenRadianceCacheClipmapDistributionBase,
	TEXT("Base of the Pow() that controls the size of each successive clipmap relative to the first."),
	ECVF_RenderThreadSafe
);

int32 GRadianceCacheNumProbesToTraceBudget = 300;
FAutoConsoleVariableRef CVarRadianceCacheNumProbesToTraceBudget(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.NumProbesToTraceBudget"),
	GRadianceCacheNumProbesToTraceBudget,
	TEXT(""),
	ECVF_RenderThreadSafe
);

int32 GRadianceCacheGridResolution = 48;
FAutoConsoleVariableRef CVarRadianceCacheResolution(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.GridResolution"),
	GRadianceCacheGridResolution,
	TEXT("Resolution of the probe placement grid within each clipmap"),
	ECVF_RenderThreadSafe
);

int32 GRadianceCacheProbeResolution = 32;
FAutoConsoleVariableRef CVarRadianceCacheProbeResolution(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.ProbeResolution"),
	GRadianceCacheProbeResolution,
	TEXT("Resolution of the probe's 2d radiance layout.  The number of rays traced for the probe will be ProbeResolution ^ 2"),
	ECVF_Scalability | ECVF_RenderThreadSafe
);

int32 GRadianceCacheNumMipmaps = 1;
FAutoConsoleVariableRef CVarRadianceCacheNumMipmaps(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.NumMipmaps"),
	GRadianceCacheNumMipmaps,
	TEXT("Number of radiance cache mipmaps."),
	ECVF_RenderThreadSafe
);

int32 GRadianceCacheProbeAtlasResolutionInProbes = 128;
FAutoConsoleVariableRef CVarRadianceCacheProbeAtlasResolutionInProbes(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.ProbeAtlasResolutionInProbes"),
	GRadianceCacheProbeAtlasResolutionInProbes,
	TEXT("Number of probes along one dimension of the probe atlas cache texture.  This controls the memory usage of the cache.  Overflow currently results in incorrect rendering."),
	ECVF_RenderThreadSafe
);

float GRadianceCacheReprojectionRadiusScale = 1.5f;
FAutoConsoleVariableRef CVarRadianceCacheProbeReprojectionRadiusScale(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.ReprojectionRadiusScale"),
	GRadianceCacheReprojectionRadiusScale,
	TEXT(""),
	ECVF_RenderThreadSafe
);

int32 GRadianceCacheStats = 0;
FAutoConsoleVariableRef CVarRadianceCacheStats(
	TEXT("r.Lumen.ScreenProbeGather.RadianceCache.Stats"),
	GRadianceCacheStats,
	TEXT("GPU print out Radiance Cache update stats. Requires r.ShaderPrintEnable 1."),
	ECVF_RenderThreadSafe
);

namespace LumenScreenProbeGatherRadianceCache
{
	int32 GetNumClipmaps()
	{
		return FMath::Clamp(GRadianceCacheNumClipmaps, 1, LumenRadianceCache::MaxClipmaps);
	}

	int32 GetClipmapGridResolution()
	{
		const int32 GridResolution = GRadianceCacheGridResolution / (GLumenFastCameraMode ? 2 : 1);
		return FMath::Clamp(GridResolution, 1, 256);
	}

	int32 GetProbeResolution()
	{
		return GRadianceCacheProbeResolution / (GLumenFastCameraMode ? 2 : 1);
	}

	int32 GetFinalProbeResolution()
	{
		return GetProbeResolution() + 2 * (1 << (GRadianceCacheNumMipmaps - 1));
	}

	FIntVector GetProbeIndirectionTextureSize()
	{
		return FIntVector(GetClipmapGridResolution() * GRadianceCacheNumClipmaps, GetClipmapGridResolution(), GetClipmapGridResolution());
	}

	FIntPoint GetProbeAtlasTextureSize()
	{
		return FIntPoint(GRadianceCacheProbeAtlasResolutionInProbes * GetProbeResolution());
	}

	FIntPoint GetFinalRadianceAtlasTextureSize()
	{
		return FIntPoint(GRadianceCacheProbeAtlasResolutionInProbes * GetFinalProbeResolution(), GRadianceCacheProbeAtlasResolutionInProbes * GetFinalProbeResolution());
	}

	int32 GetMaxNumProbes()
	{
		return GRadianceCacheProbeAtlasResolutionInProbes * GRadianceCacheProbeAtlasResolutionInProbes;
	}

	LumenRadianceCache::FRadianceCacheInputs SetupRadianceCacheInputs(const FViewInfo& View)
	{
		LumenRadianceCache::FRadianceCacheInputs Parameters = LumenRadianceCache::GetDefaultRadianceCacheInputs();
		Parameters.ReprojectionRadiusScale = GRadianceCacheReprojectionRadiusScale;
		Parameters.ClipmapWorldExtent = GLumenRadianceCacheClipmapWorldExtent;
		Parameters.ClipmapDistributionBase = GLumenRadianceCacheClipmapDistributionBase;
		Parameters.RadianceProbeClipmapResolution = GetClipmapGridResolution();
		Parameters.ProbeAtlasResolutionInProbes = FIntPoint(GRadianceCacheProbeAtlasResolutionInProbes, GRadianceCacheProbeAtlasResolutionInProbes);
		Parameters.NumRadianceProbeClipmaps = GetNumClipmaps();
		Parameters.RadianceProbeResolution = GetProbeResolution();
		Parameters.FinalProbeResolution = GetFinalProbeResolution();
		Parameters.FinalRadianceAtlasMaxMip = GRadianceCacheNumMipmaps - 1;
		const float LightingUpdateSpeed = FMath::Clamp(View.FinalPostProcessSettings.LumenFinalGatherLightingUpdateSpeed, .5f, 4.0f);
		Parameters.NumProbesToTraceBudget = FMath::RoundToInt(GRadianceCacheNumProbesToTraceBudget * LightingUpdateSpeed);
		Parameters.RadianceCacheStats = GRadianceCacheStats;
		return Parameters;
	}
};

class FOctahedralSolidAngleCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FOctahedralSolidAngleCS)
	SHADER_USE_PARAMETER_STRUCT(FOctahedralSolidAngleCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWOctahedralSolidAngleTexture)
		SHADER_PARAMETER(uint32, OctahedralSolidAngleTextureSize)
	END_SHADER_PARAMETER_STRUCT()

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

IMPLEMENT_GLOBAL_SHADER(FOctahedralSolidAngleCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "OctahedralSolidAngleCS", SF_Compute);

FRDGTextureRef InitializeOctahedralSolidAngleTexture(
	FRDGBuilder& GraphBuilder, 
	FGlobalShaderMap* ShaderMap,
	int32 OctahedralSolidAngleTextureSize,
	TRefCountPtr<IPooledRenderTarget>& OctahedralSolidAngleTextureRT)
{
	if (OctahedralSolidAngleTextureRT.IsValid()
		&& OctahedralSolidAngleTextureRT->GetDesc().Extent == OctahedralSolidAngleTextureSize)
	{
		return GraphBuilder.RegisterExternalTexture(OctahedralSolidAngleTextureRT, TEXT("OctahedralSolidAngleTexture"));
	}
	else
	{
		FRDGTextureDesc OctahedralSolidAngleTextureDesc(FRDGTextureDesc::Create2D(OctahedralSolidAngleTextureSize, PF_R16F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
		FRDGTextureRef OctahedralSolidAngleTexture = GraphBuilder.CreateTexture(OctahedralSolidAngleTextureDesc, TEXT("OctahedralSolidAngleTexture"));
	
		{
			RDG_GPU_MASK_SCOPE(GraphBuilder, FRHIGPUMask::All());

			FOctahedralSolidAngleCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FOctahedralSolidAngleCS::FParameters>();
			PassParameters->RWOctahedralSolidAngleTexture = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(OctahedralSolidAngleTexture));
			PassParameters->OctahedralSolidAngleTextureSize = OctahedralSolidAngleTextureSize;

			auto ComputeShader = ShaderMap->GetShader<FOctahedralSolidAngleCS>(0);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("OctahedralSolidAngleCS"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(FIntPoint(OctahedralSolidAngleTextureSize, OctahedralSolidAngleTextureSize), FOctahedralSolidAngleCS::GetGroupSize()));
		}

		OctahedralSolidAngleTextureRT = GraphBuilder.ConvertToExternalTexture(OctahedralSolidAngleTexture);
		return OctahedralSolidAngleTexture;
	}
}


class FCopyDepthCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FCopyDepthCS)
	SHADER_USE_PARAMETER_STRUCT(FCopyDepthCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWDepth)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, SceneDepthTexture)
	END_SHADER_PARAMETER_STRUCT()

	using FPermutationDomain = TShaderPermutationDomain<>;

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

IMPLEMENT_GLOBAL_SHADER(FCopyDepthCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "CopyDepthCS", SF_Compute);


class FScreenProbeDownsampleDepthUniformCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeDownsampleDepthUniformCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeDownsampleDepthUniformCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWScreenProbeSceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWScreenProbeWorldNormal)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWScreenProbeWorldSpeed)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWScreenProbeTranslatedWorldPosition)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
	END_SHADER_PARAMETER_STRUCT()

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
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeDownsampleDepthUniformCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "ScreenProbeDownsampleDepthUniformCS", SF_Compute);


class FScreenProbeAdaptivePlacementCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeAdaptivePlacementCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeAdaptivePlacementCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWScreenProbeSceneDepth)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWScreenProbeWorldNormal)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWScreenProbeWorldSpeed)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWScreenProbeTranslatedWorldPosition)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWNumAdaptiveScreenProbes)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWAdaptiveScreenProbeData)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWScreenTileAdaptiveProbeHeader)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWScreenTileAdaptiveProbeIndices)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
		SHADER_PARAMETER(uint32, PlacementDownsampleFactor)
	END_SHADER_PARAMETER_STRUCT()

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
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeAdaptivePlacementCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "ScreenProbeAdaptivePlacementCS", SF_Compute);

class FSetupAdaptiveProbeIndirectArgsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FSetupAdaptiveProbeIndirectArgsCS)
	SHADER_USE_PARAMETER_STRUCT(FSetupAdaptiveProbeIndirectArgsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWScreenProbeIndirectArgs)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}
};

IMPLEMENT_GLOBAL_SHADER(FSetupAdaptiveProbeIndirectArgsCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "SetupAdaptiveProbeIndirectArgsCS", SF_Compute);


class FMarkRadianceProbesUsedByScreenProbesCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMarkRadianceProbesUsedByScreenProbesCS)
	SHADER_USE_PARAMETER_STRUCT(FMarkRadianceProbesUsedByScreenProbesCS, FGlobalShader);

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheMarkParameters, RadianceCacheMarkParameters)
		END_SHADER_PARAMETER_STRUCT()

public:

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	static uint32 GetGroupSize()
	{
		return 8;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}
};

IMPLEMENT_GLOBAL_SHADER(FMarkRadianceProbesUsedByScreenProbesCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "MarkRadianceProbesUsedByScreenProbesCS", SF_Compute);

class FMarkRadianceProbesUsedByHairStrandsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FMarkRadianceProbesUsedByHairStrandsCS)
	SHADER_USE_PARAMETER_STRUCT(FMarkRadianceProbesUsedByHairStrandsCS, FGlobalShader);

	class FUseTile : SHADER_PERMUTATION_BOOL("PERMUTATION_USETILE");
	using FPermutationDomain = TShaderPermutationDomain<FUseTile>;

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER(FIntPoint, HairStrandsResolution)
		SHADER_PARAMETER(FVector2f, HairStrandsInvResolution)
		SHADER_PARAMETER(uint32, HairStrandsMip)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FHairStrandsViewUniformParameters, HairStrands)
		SHADER_PARAMETER_STRUCT_INCLUDE(LumenRadianceCache::FRadianceCacheMarkParameters, RadianceCacheMarkParameters)
		RDG_BUFFER_ACCESS(IndirectBufferArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

public:

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

IMPLEMENT_GLOBAL_SHADER(FMarkRadianceProbesUsedByHairStrandsCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "MarkRadianceProbesUsedByHairStrandsCS", SF_Compute);

// Must match usf INTEGRATE_TILE_SIZE
const int32 GScreenProbeIntegrateTileSize = 8;

class FScreenProbeTileClassificationMarkCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeTileClassificationMarkCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeTileClassificationMarkCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDiffuseIndirect)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWRoughSpecularIndirect)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIntegrateIndirectArgs)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWTileClassificationModes)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER(uint32, DefaultDiffuseIntegrationMethod)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	using FPermutationDomain = TShaderPermutationDomain<>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeTileClassificationMarkCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "ScreenProbeTileClassificationMarkCS", SF_Compute);


class FScreenProbeTileClassificationBuildListsCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeTileClassificationBuildListsCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeTileClassificationBuildListsCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWBuffer<uint>, RWIntegrateIndirectArgs)
		SHADER_PARAMETER_RDG_BUFFER_UAV(RWStructuredBuffer<uint2>, RWIntegrateTileData)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D<uint>, TileClassificationModes)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER(FIntPoint, ViewportTileDimensions)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	using FPermutationDomain = TShaderPermutationDomain<>;

	static int32 GetGroupSize()
	{
		return 64;
	}

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.SetDefine(TEXT("THREADGROUP_SIZE"), GetGroupSize());
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeTileClassificationBuildListsCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "ScreenProbeTileClassificationBuildListsCS", SF_Compute);


class FScreenProbeIntegrateCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeIntegrateCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeIntegrateCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float4>, RWDiffuseIndirect)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWRoughSpecularIndirect)
		SHADER_PARAMETER_RDG_BUFFER_SRV(StructuredBuffer<uint2>, IntegrateTileData)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeParameters, ScreenProbeParameters)
		SHADER_PARAMETER_STRUCT_INCLUDE(FScreenProbeGatherParameters, GatherParameters)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_STRUCT_INCLUDE(FLumenScreenSpaceBentNormalParameters, ScreenSpaceBentNormalParameters)
		SHADER_PARAMETER(float, FullResolutionJitterWidth)
		SHADER_PARAMETER(float, MaxRoughnessToTrace)
		SHADER_PARAMETER(float, RoughnessFadeLength)
		SHADER_PARAMETER(uint32, DefaultDiffuseIntegrationMethod)
		SHADER_PARAMETER(FIntPoint, ViewportTileDimensions)
		RDG_BUFFER_ACCESS(IndirectArgs, ERHIAccess::IndirectArgs)
	END_SHADER_PARAMETER_STRUCT()

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		return DoesPlatformSupportLumenGI(Parameters.Platform);
	}

	class FScreenSpaceBentNormal : SHADER_PERMUTATION_BOOL("SCREEN_SPACE_BENT_NORMAL");
	class FTileClassificationMode : SHADER_PERMUTATION_INT("INTEGRATE_TILE_CLASSIFICATION_MODE", 4);
	class FProbeIrradianceFormat : SHADER_PERMUTATION_ENUM_CLASS("PROBE_IRRADIANCE_FORMAT", EScreenProbeIrradianceFormat);
	class FStochasticProbeInterpolation : SHADER_PERMUTATION_BOOL("STOCHASTIC_PROBE_INTERPOLATION");
	using FPermutationDomain = TShaderPermutationDomain<FTileClassificationMode, FScreenSpaceBentNormal, FProbeIrradianceFormat, FStochasticProbeInterpolation>;

	static void ModifyCompilationEnvironment(const FGlobalShaderPermutationParameters& Parameters, FShaderCompilerEnvironment& OutEnvironment)
	{
		FGlobalShader::ModifyCompilationEnvironment(Parameters, OutEnvironment);
		OutEnvironment.CompilerFlags.Add(CFLAG_Wave32);
	}
};

IMPLEMENT_GLOBAL_SHADER(FScreenProbeIntegrateCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "ScreenProbeIntegrateCS", SF_Compute);


class FScreenProbeTemporalReprojectionCS : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FScreenProbeTemporalReprojectionCS)
	SHADER_USE_PARAMETER_STRUCT(FScreenProbeTemporalReprojectionCS, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWNewHistoryDiffuseIndirect)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float3>, RWNewHistoryRoughSpecularIndirect)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWNumHistoryFramesAccumulated)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWNewHistoryFastUpdateMode)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
		SHADER_PARAMETER_RDG_UNIFORM_BUFFER(FSceneTextureUniformParameters, SceneTexturesStruct)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DiffuseIndirectHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RoughSpecularIndirectHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DiffuseIndirectDepthHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, HistoryNumFramesAccumulated)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, FastUpdateModeHistory)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, NormalHistory)
		SHADER_PARAMETER(float,HistoryDistanceThreshold)
		SHADER_PARAMETER(float,PrevSceneColorPreExposureCorrection)
		SHADER_PARAMETER(float,InvFractionOfLightingMovingForFastUpdateMode)
		SHADER_PARAMETER(float,MaxFastUpdateModeAmount)
		SHADER_PARAMETER(float,MaxFramesAccumulated)
		SHADER_PARAMETER(float,HistoryNormalCosThreshold)
		SHADER_PARAMETER(FVector4f,HistoryScreenPositionScaleBias)
		SHADER_PARAMETER(FVector4f,HistoryUVToScreenPositionScaleBias)
		SHADER_PARAMETER(FVector4f,HistoryUVMinMax)
		SHADER_PARAMETER(FIntVector4,HistoryViewportMinMax)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, DiffuseIndirect)
		SHADER_PARAMETER_RDG_TEXTURE(Texture2D, RoughSpecularIndirect)
	END_SHADER_PARAMETER_STRUCT()

	class FHistoryRejectBasedOnNormal : SHADER_PERMUTATION_BOOL("HISTORY_REJECT_BASED_ON_NORMAL");
	class FFastUpdateModeNeighborhoodClamp : SHADER_PERMUTATION_BOOL("FAST_UPDATE_MODE_NEIGHBORHOOD_CLAMP");
	using FPermutationDomain = TShaderPermutationDomain<FFastUpdateModeNeighborhoodClamp, FHistoryRejectBasedOnNormal>;

	static bool ShouldCompilePermutation(const FGlobalShaderPermutationParameters& Parameters)
	{
		const bool bCompile = DoesPlatformSupportLumenGI(Parameters.Platform);

#if WITH_EDITOR
		if (bCompile)
		{
			ensureMsgf(VelocityEncodeDepth(Parameters.Platform), TEXT("Platform did not return true from VelocityEncodeDepth().  Lumen requires velocity depth."));
		}
#endif

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

IMPLEMENT_GLOBAL_SHADER(FScreenProbeTemporalReprojectionCS, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "ScreenProbeTemporalReprojectionCS", SF_Compute);


class FGenerateCompressedGBuffer : public FGlobalShader
{
	DECLARE_GLOBAL_SHADER(FGenerateCompressedGBuffer)
	SHADER_USE_PARAMETER_STRUCT(FGenerateCompressedGBuffer, FGlobalShader)

	BEGIN_SHADER_PARAMETER_STRUCT(FParameters, )
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<float>, RWCompressedDepthBufferOutput)
		SHADER_PARAMETER_RDG_TEXTURE_UAV(RWTexture2D<uint>, RWCompressedShadingModelOutput)
		SHADER_PARAMETER_STRUCT_REF(FViewUniformShaderParameters, View)
		SHADER_PARAMETER_STRUCT_INCLUDE(FSceneTextureParameters, SceneTextures)
	END_SHADER_PARAMETER_STRUCT()

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

IMPLEMENT_GLOBAL_SHADER(FGenerateCompressedGBuffer, "/Engine/Private/Lumen/LumenScreenProbeGather.usf", "GenerateCompressedGBuffer", SF_Compute);


const TCHAR* GetClassificationModeString(EScreenProbeIntegrateTileClassification Mode)
{
	if (Mode == EScreenProbeIntegrateTileClassification::SimpleDiffuse)
	{
		return TEXT("SimpleDiffuse");
	}
	else if (Mode == EScreenProbeIntegrateTileClassification::SupportImportanceSampleBRDF)
	{
		return TEXT("SupportImportanceSampleBRDF");
	}
	else if (Mode == EScreenProbeIntegrateTileClassification::SupportAll)
	{
		return TEXT("SupportAll");
	}

	return TEXT("");
}

void InterpolateAndIntegrate(
	FRDGBuilder& GraphBuilder,
	const FSceneTextures& SceneTextures,
	FViewInfo& View,
	FScreenProbeParameters ScreenProbeParameters,
	FScreenProbeGatherParameters GatherParameters,
	FLumenScreenSpaceBentNormalParameters ScreenSpaceBentNormalParameters,
	FRDGTextureRef DiffuseIndirect,
	FRDGTextureRef RoughSpecularIndirect)
{
	const bool bApplyScreenBentNormal = ScreenSpaceBentNormalParameters.UseScreenBentNormal != 0 && LumenScreenProbeGather::ApplyScreenBentNormalDuringIntegration();
	const bool bUseTileClassification = GLumenScreenProbeIntegrationTileClassification != 0 && LumenScreenProbeGather::GetDiffuseIntegralMethod() != 2;

	if (bUseTileClassification)
	{
		RDG_EVENT_SCOPE(GraphBuilder, "Integrate");

		FRDGBufferRef IntegrateIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>((uint32)EScreenProbeIntegrateTileClassification::Num), TEXT("Lumen.ScreenProbeGather.IntegrateIndirectArgs"));

		const FIntPoint ViewportIntegrateTileDimensions(
			FMath::DivideAndRoundUp(View.ViewRect.Size().X, GScreenProbeIntegrateTileSize), 
			FMath::DivideAndRoundUp(View.ViewRect.Size().Y, GScreenProbeIntegrateTileSize));

		checkf(ViewportIntegrateTileDimensions.X > 0 && ViewportIntegrateTileDimensions.Y > 0, TEXT("Compute shader needs non-zero dispatch to clear next pass's indirect args"));

		const FIntPoint TileClassificationBufferDimensions(
			FMath::DivideAndRoundUp(SceneTextures.Config.Extent.X, GScreenProbeIntegrateTileSize), 
			FMath::DivideAndRoundUp(SceneTextures.Config.Extent.Y, GScreenProbeIntegrateTileSize));

		FRDGTextureDesc TileClassificationModesDesc = FRDGTextureDesc::Create2D(TileClassificationBufferDimensions, PF_R8_UINT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef TileClassificationModes = GraphBuilder.CreateTexture(TileClassificationModesDesc, TEXT("Lumen.ScreenProbeGather.TileClassificationModes"));

		{
			FScreenProbeTileClassificationMarkCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeTileClassificationMarkCS::FParameters>();
			PassParameters->RWDiffuseIndirect = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DiffuseIndirect));
			PassParameters->RWRoughSpecularIndirect =  GraphBuilder.CreateUAV(FRDGTextureUAVDesc(RoughSpecularIndirect));
			PassParameters->RWIntegrateIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IntegrateIndirectArgs, PF_R32_UINT));
			PassParameters->RWTileClassificationModes = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(TileClassificationModes));
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
			PassParameters->DefaultDiffuseIntegrationMethod = (uint32)LumenScreenProbeGather::GetDiffuseIntegralMethod();

			FScreenProbeTileClassificationMarkCS::FPermutationDomain PermutationVector;
			auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeTileClassificationMarkCS>(PermutationVector);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("TileClassificationMark"),
				ComputeShader,
				PassParameters,
				FIntVector(ViewportIntegrateTileDimensions.X, ViewportIntegrateTileDimensions.Y, 1));
		}

		FRDGBufferRef IntegrateTileData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), TileClassificationBufferDimensions.X * TileClassificationBufferDimensions.Y * (uint32)EScreenProbeIntegrateTileClassification::Num), TEXT("Lumen.ScreenProbeGather.IntegrateTileData"));

		{
			FScreenProbeTileClassificationBuildListsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeTileClassificationBuildListsCS::FParameters>();
			PassParameters->RWIntegrateIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IntegrateIndirectArgs, PF_R32_UINT));
			PassParameters->RWIntegrateTileData = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(IntegrateTileData));
			PassParameters->TileClassificationModes = TileClassificationModes;
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->ViewportTileDimensions = ViewportIntegrateTileDimensions;

			FScreenProbeTileClassificationBuildListsCS::FPermutationDomain PermutationVector;
			auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeTileClassificationBuildListsCS>(PermutationVector);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("TileClassificationBuildLists"),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(ViewportIntegrateTileDimensions, 8));
		}

		// Allow integration passes to overlap
		FRDGTextureUAVRef DiffuseIndirectUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DiffuseIndirect), ERDGUnorderedAccessViewFlags::SkipBarrier);
		FRDGTextureUAVRef RoughSpecularIndirectUAV = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(RoughSpecularIndirect), ERDGUnorderedAccessViewFlags::SkipBarrier);

		for (uint32 ClassificationMode = 0; ClassificationMode < (uint32)EScreenProbeIntegrateTileClassification::Num; ClassificationMode++)
		{
			FScreenProbeIntegrateCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeIntegrateCS::FParameters>();
			PassParameters->RWDiffuseIndirect = DiffuseIndirectUAV;
			PassParameters->RWRoughSpecularIndirect = RoughSpecularIndirectUAV;
			PassParameters->IntegrateTileData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(IntegrateTileData));
			PassParameters->GatherParameters = GatherParameters;
			PassParameters->ScreenProbeParameters = ScreenProbeParameters;
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
			PassParameters->FullResolutionJitterWidth = LumenScreenProbeGather::GetScreenProbeFullResolutionJitterWidth(View);
			extern float GLumenReflectionMaxRoughnessToTrace;
			extern float GLumenReflectionRoughnessFadeLength;
			PassParameters->MaxRoughnessToTrace = GLumenReflectionMaxRoughnessToTrace;
			PassParameters->RoughnessFadeLength = GLumenReflectionRoughnessFadeLength;
			PassParameters->ScreenSpaceBentNormalParameters = ScreenSpaceBentNormalParameters;
			PassParameters->DefaultDiffuseIntegrationMethod = (uint32)LumenScreenProbeGather::GetDiffuseIntegralMethod();
			PassParameters->ViewportTileDimensions = ViewportIntegrateTileDimensions;
			PassParameters->IndirectArgs = IntegrateIndirectArgs;

			FScreenProbeIntegrateCS::FPermutationDomain PermutationVector;
			PermutationVector.Set< FScreenProbeIntegrateCS::FTileClassificationMode >(ClassificationMode);
			PermutationVector.Set< FScreenProbeIntegrateCS::FScreenSpaceBentNormal >(bApplyScreenBentNormal);
			PermutationVector.Set< FScreenProbeIntegrateCS::FProbeIrradianceFormat >(LumenScreenProbeGather::GetScreenProbeIrradianceFormat(View.Family->EngineShowFlags));
			PermutationVector.Set< FScreenProbeIntegrateCS::FStochasticProbeInterpolation >(GLumenScreenProbeStochasticInterpolation != 0);
			auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeIntegrateCS>(PermutationVector);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("%s", GetClassificationModeString((EScreenProbeIntegrateTileClassification)ClassificationMode)),
				ComputeShader,
				PassParameters,
				IntegrateIndirectArgs,
				ClassificationMode * sizeof(FRHIDispatchIndirectParameters));
		}
	}
	else
	{		
		FScreenProbeIntegrateCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeIntegrateCS::FParameters>();
		PassParameters->RWDiffuseIndirect = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(DiffuseIndirect));
		PassParameters->RWRoughSpecularIndirect =  GraphBuilder.CreateUAV(FRDGTextureUAVDesc(RoughSpecularIndirect));
		PassParameters->GatherParameters = GatherParameters;

		const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);
		if (!PassParameters->GatherParameters.ScreenProbeRadianceSHAmbient)
		{
			PassParameters->GatherParameters.ScreenProbeRadianceSHAmbient = SystemTextures.Black;
			PassParameters->GatherParameters.ScreenProbeRadianceSHDirectional = SystemTextures.Black;
		}

		PassParameters->ScreenProbeParameters = ScreenProbeParameters;
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
		PassParameters->FullResolutionJitterWidth = LumenScreenProbeGather::GetScreenProbeFullResolutionJitterWidth(View);
		extern float GLumenReflectionMaxRoughnessToTrace;
		extern float GLumenReflectionRoughnessFadeLength;
		PassParameters->MaxRoughnessToTrace = GLumenReflectionMaxRoughnessToTrace;
		PassParameters->RoughnessFadeLength = GLumenReflectionRoughnessFadeLength;
		PassParameters->ScreenSpaceBentNormalParameters = ScreenSpaceBentNormalParameters;
		PassParameters->DefaultDiffuseIntegrationMethod = (uint32)LumenScreenProbeGather::GetDiffuseIntegralMethod();
		PassParameters->ViewportTileDimensions = FIntPoint(0, 0);

		FScreenProbeIntegrateCS::FPermutationDomain PermutationVector;
		PermutationVector.Set< FScreenProbeIntegrateCS::FTileClassificationMode >((uint32)EScreenProbeIntegrateTileClassification::Num);
		PermutationVector.Set< FScreenProbeIntegrateCS::FScreenSpaceBentNormal >(bApplyScreenBentNormal);
		auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeIntegrateCS>(PermutationVector);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("Integrate"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(View.ViewRect.Size(), GScreenProbeIntegrateTileSize));
	}
}

void UpdateHistoryScreenProbeGather(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View, 
	const FSceneTextures& SceneTextures,
	FRDGTextureRef& DiffuseIndirect,
	FRDGTextureRef& RoughSpecularIndirect)
{
	LLM_SCOPE_BYTAG(Lumen);
	
	if (View.ViewState)
	{
		FScreenProbeGatherTemporalState& ScreenProbeGatherState = View.ViewState->Lumen.ScreenProbeGatherState;
		TRefCountPtr<IPooledRenderTarget>* DiffuseIndirectHistoryState0 = &ScreenProbeGatherState.DiffuseIndirectHistoryRT;
		TRefCountPtr<IPooledRenderTarget>* RoughSpecularIndirectHistoryState = &ScreenProbeGatherState.RoughSpecularIndirectHistoryRT;
		FIntRect* DiffuseIndirectHistoryViewRect = &ScreenProbeGatherState.DiffuseIndirectHistoryViewRect;
		FVector4f* DiffuseIndirectHistoryScreenPositionScaleBias = &ScreenProbeGatherState.DiffuseIndirectHistoryScreenPositionScaleBias;
		TRefCountPtr<IPooledRenderTarget>* HistoryNumFramesAccumulated = &ScreenProbeGatherState.NumFramesAccumulatedRT;
		TRefCountPtr<IPooledRenderTarget>* FastUpdateModeHistoryState = &ScreenProbeGatherState.FastUpdateModeHistoryRT;
		TRefCountPtr<IPooledRenderTarget>* NormalHistoryState = &ScreenProbeGatherState.NormalHistoryRT;
		const bool bRejectBasedOnNormal = GLumenScreenProbeTemporalRejectBasedOnNormal != 0 && NormalHistoryState
			&& !Strata::IsStrataEnabled(); // STRATA_TODO provide Lumen with a valid normal

		ensureMsgf(SceneTextures.Velocity->Desc.Format != PF_G16R16, TEXT("Lumen requires 3d velocity.  Update Velocity format code."));

		const FIntPoint BufferSize = SceneTextures.Config.Extent;
		const FIntRect NewHistoryViewRect = View.ViewRect;

		if (*DiffuseIndirectHistoryState0
			&& !View.bCameraCut 
			&& !View.bPrevTransformsReset
			&& !GLumenScreenProbeClearHistoryEveryFrame
			// If the scene render targets reallocate, toss the history so we don't read uninitialized data
			&& (*DiffuseIndirectHistoryState0)->GetDesc().Extent == SceneTextures.Config.Extent
			&& ScreenProbeGatherState.LumenGatherCvars == GLumenGatherCvars
			&& !View.bLumenPropagateGlobalLightingChange)
		{
			EPixelFormat HistoryFormat = PF_FloatRGBA;
			FRDGTextureDesc DiffuseIndirectDesc = FRDGTextureDesc::Create2D(BufferSize, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
			FRDGTextureRef NewDiffuseIndirect = GraphBuilder.CreateTexture(DiffuseIndirectDesc, TEXT("Lumen.ScreenProbeGather.DiffuseIndirect"));

			FRDGTextureRef OldDiffuseIndirectHistory = GraphBuilder.RegisterExternalTexture(ScreenProbeGatherState.DiffuseIndirectHistoryRT);

			FRDGTextureDesc RoughSpecularIndirectDesc = FRDGTextureDesc::Create2D(BufferSize, PF_FloatRGB, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
			FRDGTextureRef NewRoughSpecularIndirect = GraphBuilder.CreateTexture(RoughSpecularIndirectDesc, TEXT("Lumen.ScreenProbeGather.RoughSpecularIndirect"));

			FRDGTextureDesc NumHistoryFramesAccumulatedDesc(FRDGTextureDesc::Create2D(BufferSize, PF_R8, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
			FRDGTextureRef NewNumHistoryFramesAccumulated = GraphBuilder.CreateTexture(NumHistoryFramesAccumulatedDesc, TEXT("Lumen.ScreenProbeGather.NumHistoryFramesAccumulated"));
			FRDGTextureRef NewHistoryFastUpdateMode = GraphBuilder.CreateTexture(NumHistoryFramesAccumulatedDesc, TEXT("Lumen.ScreenProbeGather.FastUpdateMode"));

			{
				FRDGTextureRef OldRoughSpecularIndirectHistory = GraphBuilder.RegisterExternalTexture(*RoughSpecularIndirectHistoryState);
				FRDGTextureRef OldDepthHistory = View.ViewState->Lumen.DepthHistoryRT ? GraphBuilder.RegisterExternalTexture(View.ViewState->Lumen.DepthHistoryRT) : SceneTextures.Depth.Target;
				FRDGTextureRef OldHistoryNumFramesAccumulated = GraphBuilder.RegisterExternalTexture(*HistoryNumFramesAccumulated);
				FRDGTextureRef OldFastUpdateModeHistory = GraphBuilder.RegisterExternalTexture(*FastUpdateModeHistoryState);

				{
					FScreenProbeTemporalReprojectionCS::FPermutationDomain PermutationVector;
					PermutationVector.Set< FScreenProbeTemporalReprojectionCS::FFastUpdateModeNeighborhoodClamp>(GLumenScreenProbeTemporalFastUpdateModeUseNeighborhoodClamp != 0);
					PermutationVector.Set< FScreenProbeTemporalReprojectionCS::FHistoryRejectBasedOnNormal>(bRejectBasedOnNormal);
					auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeTemporalReprojectionCS>(PermutationVector);

					FScreenProbeTemporalReprojectionCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeTemporalReprojectionCS::FParameters>();
					PassParameters->RWNewHistoryDiffuseIndirect = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(NewDiffuseIndirect));
					PassParameters->RWNewHistoryRoughSpecularIndirect = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(NewRoughSpecularIndirect));
					PassParameters->RWNumHistoryFramesAccumulated = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(NewNumHistoryFramesAccumulated));
					PassParameters->RWNewHistoryFastUpdateMode = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(NewHistoryFastUpdateMode));

					PassParameters->View = View.ViewUniformBuffer;
					PassParameters->SceneTextures = GetSceneTextureParameters(GraphBuilder, SceneTextures.UniformBuffer);
					PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;

					PassParameters->DiffuseIndirectHistory = OldDiffuseIndirectHistory;
					PassParameters->RoughSpecularIndirectHistory = OldRoughSpecularIndirectHistory;
					PassParameters->DiffuseIndirectDepthHistory = OldDepthHistory;
					PassParameters->HistoryNumFramesAccumulated  = OldHistoryNumFramesAccumulated;
					PassParameters->FastUpdateModeHistory = OldFastUpdateModeHistory;
					PassParameters->NormalHistory = bRejectBasedOnNormal ? GraphBuilder.RegisterExternalTexture(*NormalHistoryState) : nullptr;

					PassParameters->HistoryDistanceThreshold = GLumenScreenProbeHistoryDistanceThreshold;
					PassParameters->PrevSceneColorPreExposureCorrection = View.PreExposure / View.PrevViewInfo.SceneColorPreExposure;
					PassParameters->InvFractionOfLightingMovingForFastUpdateMode = 1.0f / FMath::Max(GLumenScreenProbeFractionOfLightingMovingForFastUpdateMode, .001f);
					PassParameters->MaxFastUpdateModeAmount = GLumenScreenProbeTemporalMaxFastUpdateModeAmount;

					const float MaxFramesAccumulatedScale = 1.0f / FMath::Sqrt(FMath::Clamp(View.FinalPostProcessSettings.LumenFinalGatherLightingUpdateSpeed, .5f, 8.0f));
					PassParameters->MaxFramesAccumulated = FMath::RoundToInt(GLumenScreenProbeTemporalMaxFramesAccumulated * MaxFramesAccumulatedScale);
					PassParameters->HistoryNormalCosThreshold = FMath::Cos(GLumenScreenProbeTemporalHistoryNormalThreshold * (float)PI / 180.0f);
					PassParameters->HistoryScreenPositionScaleBias = *DiffuseIndirectHistoryScreenPositionScaleBias;

					const FVector2f HistoryUVToScreenPositionScale(1.0f / PassParameters->HistoryScreenPositionScaleBias.X, 1.0f / PassParameters->HistoryScreenPositionScaleBias.Y);
					const FVector2f HistoryUVToScreenPositionBias = -FVector2f(PassParameters->HistoryScreenPositionScaleBias.W, PassParameters->HistoryScreenPositionScaleBias.Z) * HistoryUVToScreenPositionScale;
					PassParameters->HistoryUVToScreenPositionScaleBias = FVector4f(HistoryUVToScreenPositionScale, HistoryUVToScreenPositionBias);

					const FVector2D InvBufferSize(1.0f / BufferSize.X, 1.0f / BufferSize.Y);
					// Pull in the max UV to exclude the region which will read outside the viewport due to bilinear filtering
					PassParameters->HistoryUVMinMax = FVector4f(
						(DiffuseIndirectHistoryViewRect->Min.X + 0.5f) * InvBufferSize.X,
						(DiffuseIndirectHistoryViewRect->Min.Y + 0.5f) * InvBufferSize.Y,
						(DiffuseIndirectHistoryViewRect->Max.X - 0.5f) * InvBufferSize.X,
						(DiffuseIndirectHistoryViewRect->Max.Y - 0.5f) * InvBufferSize.Y);

					PassParameters->HistoryViewportMinMax = FIntVector4(
						DiffuseIndirectHistoryViewRect->Min.X, 
						DiffuseIndirectHistoryViewRect->Min.Y, 
						DiffuseIndirectHistoryViewRect->Max.X,
						DiffuseIndirectHistoryViewRect->Max.Y);

					PassParameters->DiffuseIndirect = DiffuseIndirect;
					PassParameters->RoughSpecularIndirect = RoughSpecularIndirect;

					FComputeShaderUtils::AddPass(
						GraphBuilder,
						RDG_EVENT_NAME("TemporalReprojection %ux%u", View.ViewRect.Width(), View.ViewRect.Height()),
						ComputeShader,
						PassParameters,
						FComputeShaderUtils::GetGroupCount(View.ViewRect.Size(), FScreenProbeTemporalReprojectionCS::GetGroupSize()));
				}

				if (!View.bStatePrevViewInfoIsReadOnly)
				{
					// Queue updating the view state's render target reference with the new history
					GraphBuilder.QueueTextureExtraction(NewDiffuseIndirect, &ScreenProbeGatherState.DiffuseIndirectHistoryRT);
					GraphBuilder.QueueTextureExtraction(NewRoughSpecularIndirect, RoughSpecularIndirectHistoryState);
					GraphBuilder.QueueTextureExtraction(NewNumHistoryFramesAccumulated, HistoryNumFramesAccumulated);
					GraphBuilder.QueueTextureExtraction(NewHistoryFastUpdateMode, FastUpdateModeHistoryState);
				}
			}

			RoughSpecularIndirect = NewRoughSpecularIndirect;
			DiffuseIndirect = NewDiffuseIndirect;
		}
		else
		{
			if (!View.bStatePrevViewInfoIsReadOnly)
			{
				// Queue updating the view state's render target reference with the new values
				GraphBuilder.QueueTextureExtraction(DiffuseIndirect, &ScreenProbeGatherState.DiffuseIndirectHistoryRT);
				GraphBuilder.QueueTextureExtraction(RoughSpecularIndirect, RoughSpecularIndirectHistoryState);
				*HistoryNumFramesAccumulated = GSystemTextures.BlackDummy;
				*FastUpdateModeHistoryState = GSystemTextures.BlackDummy;
			}
		}

		if (!View.bStatePrevViewInfoIsReadOnly)
		{
			*DiffuseIndirectHistoryViewRect = NewHistoryViewRect;
			*DiffuseIndirectHistoryScreenPositionScaleBias = View.GetScreenPositionScaleBias(SceneTextures.Config.Extent, View.ViewRect);
			ScreenProbeGatherState.LumenGatherCvars = GLumenGatherCvars;

			if (bRejectBasedOnNormal)
			{
				GraphBuilder.QueueTextureExtraction(SceneTextures.GBufferA, NormalHistoryState);
			}
		}
	}
	else
	{
		// Temporal reprojection is disabled or there is no view state - pass through
	}
}

void FDeferredShadingSceneRenderer::StoreLumenDepthHistory(FRDGBuilder& GraphBuilder, const FSceneTextures& SceneTextures, FViewInfo& View)
{
	if (View.ViewState && !View.bStatePrevViewInfoIsReadOnly)
	{
		FRDGTextureDesc DepthDesc = SceneTextures.Depth.Resolve->Desc;

		FRDGTextureDesc NewDepthHistoryDesc = FRDGTextureDesc::Create2D(DepthDesc.Extent, PF_R32_FLOAT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
		FRDGTextureRef NewDepthHistory = GraphBuilder.CreateTexture(NewDepthHistoryDesc, TEXT("Lumen.DepthHistory"));

		FCopyDepthCS::FPermutationDomain PermutationVector;
		auto ComputeShader = View.ShaderMap->GetShader<FCopyDepthCS>(PermutationVector);

		FCopyDepthCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FCopyDepthCS::FParameters>();
		PassParameters->RWDepth = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(NewDepthHistory));
		PassParameters->SceneDepthTexture = SceneTextures.Depth.Resolve;

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("CopyDepth"),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(View.ViewRect.Size(), FCopyDepthCS::GetGroupSize()));

		GraphBuilder.QueueTextureExtraction(NewDepthHistory, &View.ViewState->Lumen.DepthHistoryRT);
	}
}

static void ScreenGatherMarkUsedProbes(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const FSceneTextures& SceneTextures,
	FScreenProbeParameters& ScreenProbeParameters,
	const LumenRadianceCache::FRadianceCacheMarkParameters& RadianceCacheMarkParameters)
{
	FMarkRadianceProbesUsedByScreenProbesCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FMarkRadianceProbesUsedByScreenProbesCS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
	PassParameters->ScreenProbeParameters = ScreenProbeParameters;
	PassParameters->RadianceCacheMarkParameters = RadianceCacheMarkParameters;

	auto ComputeShader = View.ShaderMap->GetShader<FMarkRadianceProbesUsedByScreenProbesCS>(0);

	FComputeShaderUtils::AddPass(
		GraphBuilder,
		RDG_EVENT_NAME("MarkRadianceProbes(ScreenProbes) %ux%u", PassParameters->ScreenProbeParameters.ScreenProbeAtlasViewSize.X, PassParameters->ScreenProbeParameters.ScreenProbeAtlasViewSize.Y),
		ComputeShader,
		PassParameters,
		PassParameters->ScreenProbeParameters.ProbeIndirectArgs,
		(uint32)EScreenProbeIndirectArgs::ThreadPerProbe * sizeof(FRHIDispatchIndirectParameters));
}

static void HairStrandsMarkUsedProbes(
	FRDGBuilder& GraphBuilder,
	const FViewInfo& View,
	const LumenRadianceCache::FRadianceCacheMarkParameters& RadianceCacheMarkParameters)
{
	const bool bUseTile = View.HairStrandsViewData.VisibilityData.TileData.IsValid();
	const uint32 TileMip = bUseTile ? 3u : 4u; // 8x8 tiles or 16x16 tiles
	const int32 TileSize = 1u<<TileMip;
	const FIntPoint Resolution(View.ViewRect.Width(), View.ViewRect.Height());
	const FIntPoint TileResolution = FIntPoint(
		FMath::DivideAndRoundUp(Resolution.X, TileSize), 
		FMath::DivideAndRoundUp(Resolution.Y, TileSize));

	FMarkRadianceProbesUsedByHairStrandsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FMarkRadianceProbesUsedByHairStrandsCS::FParameters>();
	PassParameters->View = View.ViewUniformBuffer;
	PassParameters->HairStrandsResolution = TileResolution;
	PassParameters->HairStrandsInvResolution = FVector2f(1.f / float(TileResolution.X), 1.f / float(TileResolution.Y));
	PassParameters->HairStrandsMip = TileMip;
	PassParameters->HairStrands = HairStrands::BindHairStrandsViewUniformParameters(View);
	PassParameters->RadianceCacheMarkParameters = RadianceCacheMarkParameters;
	PassParameters->IndirectBufferArgs = View.HairStrandsViewData.VisibilityData.TileData.TilePerThreadIndirectDispatchBuffer;

	FMarkRadianceProbesUsedByHairStrandsCS::FPermutationDomain PermutationVector;
	PermutationVector.Set< FMarkRadianceProbesUsedByHairStrandsCS::FUseTile>(bUseTile);
	auto ComputeShader = View.ShaderMap->GetShader<FMarkRadianceProbesUsedByHairStrandsCS>(PermutationVector);
	if (bUseTile)
	{
		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("MarkRadianceProbes(HairStrands,Tile)"),
			ComputeShader,
			PassParameters,
			View.HairStrandsViewData.VisibilityData.TileData.TilePerThreadIndirectDispatchBuffer,
			0);
	}
	else
	{
		const int32 GroupSize = 8;
		const FIntVector GroupCount = FIntVector(
			FMath::DivideAndRoundUp(TileResolution.X, FMarkRadianceProbesUsedByHairStrandsCS::GetGroupSize()),
			FMath::DivideAndRoundUp(TileResolution.Y, FMarkRadianceProbesUsedByHairStrandsCS::GetGroupSize()),
			1);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("MarkRadianceProbes(HairStrands,Screen) %ux%u", TileResolution.X, TileResolution.Y),
			ComputeShader,
			PassParameters,
			GroupCount);
	}
}

DECLARE_GPU_STAT(LumenScreenProbeGather);

FSSDSignalTextures FDeferredShadingSceneRenderer::RenderLumenScreenProbeGather(
	FRDGBuilder& GraphBuilder,
	const FSceneTextures& SceneTextures,
	FLumenSceneFrameTemporaries& FrameTemporaries,
	FRDGTextureRef LightingChannelsTexture,
	FViewInfo& View,
	FPreviousViewInfo* PreviousViewInfos,
	bool& bLumenUseDenoiserComposite,
	FLumenMeshSDFGridParameters& MeshSDFGridParameters,
	LumenRadianceCache::FRadianceCacheInterpolationParameters& RadianceCacheParameters,
	FLumenScreenSpaceBentNormalParameters& ScreenSpaceBentNormalParameters)
{
	LLM_SCOPE_BYTAG(Lumen);

	const FRDGSystemTextures& SystemTextures = FRDGSystemTextures::Get(GraphBuilder);
	bLumenUseDenoiserComposite = false;
	ScreenSpaceBentNormalParameters.UseScreenBentNormal = 0;
	ScreenSpaceBentNormalParameters.ScreenBentNormal = SystemTextures.Black;
	RadianceCacheParameters.RadianceProbeIndirectionTexture = nullptr;

	if (GLumenIrradianceFieldGather != 0)
	{
		return RenderLumenIrradianceFieldGather(GraphBuilder, SceneTextures, FrameTemporaries, View);
	}

	RDG_EVENT_SCOPE(GraphBuilder, "LumenScreenProbeGather");
	RDG_GPU_STAT_SCOPE(GraphBuilder, LumenScreenProbeGather);

	check(ShouldRenderLumenDiffuseGI(Scene, View));

	if (!LightingChannelsTexture)
	{
		LightingChannelsTexture = SystemTextures.Black;
	}

	if (!GLumenScreenProbeGather)
	{
		FSSDSignalTextures ScreenSpaceDenoiserInputs;
		ScreenSpaceDenoiserInputs.Textures[0] = SystemTextures.Black;
		FRDGTextureDesc RoughSpecularIndirectDesc = FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_FloatRGB, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
		ScreenSpaceDenoiserInputs.Textures[1] = GraphBuilder.CreateTexture(RoughSpecularIndirectDesc, TEXT("Lumen.ScreenProbeGather.RoughSpecularIndirect"));
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenSpaceDenoiserInputs.Textures[1])), FLinearColor::Black);
		return ScreenSpaceDenoiserInputs;
	}

	// Pull from uniform buffer to get fallback textures.
	const FSceneTextureParameters SceneTextureParameters = GetSceneTextureParameters(GraphBuilder, SceneTextures.UniformBuffer);

	FScreenProbeParameters ScreenProbeParameters;

	ScreenProbeParameters.ScreenProbeTracingOctahedronResolution = LumenScreenProbeGather::GetTracingOctahedronResolution(View);
	ensureMsgf(ScreenProbeParameters.ScreenProbeTracingOctahedronResolution < (1 << 6) - 1, TEXT("Tracing resolution %u was larger than supported by PackRayInfo()"), ScreenProbeParameters.ScreenProbeTracingOctahedronResolution);
	ScreenProbeParameters.ScreenProbeGatherOctahedronResolution = LumenScreenProbeGather::GetGatherOctahedronResolution(ScreenProbeParameters.ScreenProbeTracingOctahedronResolution);
	ScreenProbeParameters.ScreenProbeGatherOctahedronResolutionWithBorder = ScreenProbeParameters.ScreenProbeGatherOctahedronResolution + 2 * (1 << (GLumenScreenProbeGatherNumMips - 1));
	ScreenProbeParameters.ScreenProbeDownsampleFactor = LumenScreenProbeGather::GetScreenDownsampleFactor(View);

	ScreenProbeParameters.ScreenProbeViewSize = FIntPoint::DivideAndRoundUp(View.ViewRect.Size(), (int32)ScreenProbeParameters.ScreenProbeDownsampleFactor);
	ScreenProbeParameters.ScreenProbeAtlasViewSize = ScreenProbeParameters.ScreenProbeViewSize;
	ScreenProbeParameters.ScreenProbeAtlasViewSize.Y += FMath::TruncToInt(ScreenProbeParameters.ScreenProbeViewSize.Y * GLumenScreenProbeGatherAdaptiveProbeAllocationFraction);

	ScreenProbeParameters.ScreenProbeAtlasBufferSize = FIntPoint::DivideAndRoundUp(SceneTextures.Config.Extent, (int32)ScreenProbeParameters.ScreenProbeDownsampleFactor);
	ScreenProbeParameters.ScreenProbeAtlasBufferSize.Y += FMath::TruncToInt(ScreenProbeParameters.ScreenProbeAtlasBufferSize.Y * GLumenScreenProbeGatherAdaptiveProbeAllocationFraction);

	ScreenProbeParameters.ScreenProbeGatherMaxMip = GLumenScreenProbeGatherNumMips - 1;
	ScreenProbeParameters.RelativeSpeedDifferenceToConsiderLightingMoving = GLumenScreenProbeRelativeSpeedDifferenceToConsiderLightingMoving;
	ScreenProbeParameters.ScreenTraceNoFallbackThicknessScale = Lumen::UseHardwareRayTracedScreenProbeGather() ? 1.0f : GLumenScreenProbeScreenTracesThicknessScaleWhenNoFallback;
	ScreenProbeParameters.NumUniformScreenProbes = ScreenProbeParameters.ScreenProbeViewSize.X * ScreenProbeParameters.ScreenProbeViewSize.Y;
	ScreenProbeParameters.MaxNumAdaptiveProbes = FMath::TruncToInt(ScreenProbeParameters.NumUniformScreenProbes * GLumenScreenProbeGatherAdaptiveProbeAllocationFraction);
	
	ScreenProbeParameters.FixedJitterIndex = GLumenScreenProbeFixedJitterIndex;

	{
		FVector2f InvAtlasWithBorderBufferSize = FVector2f(1.0f) / (FVector2f(ScreenProbeParameters.ScreenProbeGatherOctahedronResolutionWithBorder) * FVector2f(ScreenProbeParameters.ScreenProbeAtlasBufferSize));
		ScreenProbeParameters.SampleRadianceProbeUVMul = FVector2f(ScreenProbeParameters.ScreenProbeGatherOctahedronResolution) * InvAtlasWithBorderBufferSize;
		ScreenProbeParameters.SampleRadianceProbeUVAdd = FMath::Exp2(ScreenProbeParameters.ScreenProbeGatherMaxMip) * InvAtlasWithBorderBufferSize;
		ScreenProbeParameters.SampleRadianceAtlasUVMul = FVector2f(ScreenProbeParameters.ScreenProbeGatherOctahedronResolutionWithBorder) * InvAtlasWithBorderBufferSize;
	}

	extern int32 GLumenScreenProbeGatherVisualizeTraces;
	// Automatically set a fixed jitter if we are visualizing, but don't override existing fixed jitter
	if (GLumenScreenProbeGatherVisualizeTraces != 0 && ScreenProbeParameters.FixedJitterIndex < 0)
	{
		ScreenProbeParameters.FixedJitterIndex = 6;
	}

	FRDGTextureDesc DownsampledDepthDesc(FRDGTextureDesc::Create2D(ScreenProbeParameters.ScreenProbeAtlasBufferSize, PF_R32_UINT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ScreenProbeParameters.ScreenProbeSceneDepth = GraphBuilder.CreateTexture(DownsampledDepthDesc, TEXT("Lumen.ScreenProbeGather.ScreenProbeSceneDepth"));

	FRDGTextureDesc DownsampledNormalDesc(FRDGTextureDesc::Create2D(ScreenProbeParameters.ScreenProbeAtlasBufferSize, PF_R8G8, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ScreenProbeParameters.ScreenProbeWorldNormal = GraphBuilder.CreateTexture(DownsampledNormalDesc, TEXT("Lumen.ScreenProbeGather.ScreenProbeWorldNormal"));

	FRDGTextureDesc DownsampledSpeedDesc(FRDGTextureDesc::Create2D(ScreenProbeParameters.ScreenProbeAtlasBufferSize, PF_R16_UINT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ScreenProbeParameters.ScreenProbeWorldSpeed = GraphBuilder.CreateTexture(DownsampledSpeedDesc, TEXT("Lumen.ScreenProbeGather.ScreenProbeWorldSpeed"));

	FRDGTextureDesc DownsampledWorldPositionDesc(FRDGTextureDesc::Create2D(ScreenProbeParameters.ScreenProbeAtlasBufferSize, PF_A32B32G32R32F, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ScreenProbeParameters.ScreenProbeTranslatedWorldPosition = GraphBuilder.CreateTexture(DownsampledWorldPositionDesc, TEXT("Lumen.ScreenProbeGather.ScreenProbeTranslatedWorldPosition"));


	FBlueNoise BlueNoise;
	InitializeBlueNoise(BlueNoise);
	ScreenProbeParameters.BlueNoise = CreateUniformBufferImmediate(BlueNoise, EUniformBufferUsage::UniformBuffer_SingleDraw);

	ScreenProbeParameters.OctahedralSolidAngleParameters.OctahedralSolidAngleTextureResolutionSq = GLumenOctahedralSolidAngleTextureSize * GLumenOctahedralSolidAngleTextureSize;
	ScreenProbeParameters.OctahedralSolidAngleParameters.OctahedralSolidAngleTexture = InitializeOctahedralSolidAngleTexture(GraphBuilder, View.ShaderMap, GLumenOctahedralSolidAngleTextureSize, View.ViewState->Lumen.ScreenProbeGatherState.OctahedralSolidAngleTextureRT);

	{
		FScreenProbeDownsampleDepthUniformCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeDownsampleDepthUniformCS::FParameters>();
		PassParameters->RWScreenProbeSceneDepth = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenProbeSceneDepth));
		PassParameters->RWScreenProbeWorldNormal = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenProbeWorldNormal));
		PassParameters->RWScreenProbeWorldSpeed = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenProbeWorldSpeed));
		PassParameters->RWScreenProbeTranslatedWorldPosition = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenProbeTranslatedWorldPosition));
		PassParameters->View = View.ViewUniformBuffer;
		PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
		PassParameters->SceneTextures = SceneTextureParameters;
		PassParameters->ScreenProbeParameters = ScreenProbeParameters;

		auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeDownsampleDepthUniformCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("UniformPlacement DownsampleFactor=%u", ScreenProbeParameters.ScreenProbeDownsampleFactor),
			ComputeShader,
			PassParameters,
			FComputeShaderUtils::GetGroupCount(ScreenProbeParameters.ScreenProbeViewSize, FScreenProbeDownsampleDepthUniformCS::GetGroupSize()));
	}

	FRDGBufferRef NumAdaptiveScreenProbes = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), 1), TEXT("Lumen.ScreenProbeGather.NumAdaptiveScreenProbes"));
	FRDGBufferRef AdaptiveScreenProbeData = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateStructuredDesc(sizeof(uint32), FMath::Max<uint32>(ScreenProbeParameters.MaxNumAdaptiveProbes, 1)), TEXT("Lumen.ScreenProbeGather.daptiveScreenProbeData"));

	ScreenProbeParameters.NumAdaptiveScreenProbes = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(NumAdaptiveScreenProbes, PF_R32_UINT));
	ScreenProbeParameters.AdaptiveScreenProbeData = GraphBuilder.CreateSRV(FRDGBufferSRVDesc(AdaptiveScreenProbeData, PF_R32_UINT));

	const FIntPoint ScreenProbeViewportBufferSize = FIntPoint::DivideAndRoundUp(SceneTextures.Config.Extent, (int32)ScreenProbeParameters.ScreenProbeDownsampleFactor);
	FRDGTextureDesc ScreenTileAdaptiveProbeHeaderDesc(FRDGTextureDesc::Create2D(ScreenProbeViewportBufferSize, PF_R32_UINT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV | TexCreate_AtomicCompatible));
	FIntPoint ScreenTileAdaptiveProbeIndicesBufferSize = FIntPoint(ScreenProbeViewportBufferSize.X * ScreenProbeParameters.ScreenProbeDownsampleFactor, ScreenProbeViewportBufferSize.Y * ScreenProbeParameters.ScreenProbeDownsampleFactor);
	FRDGTextureDesc ScreenTileAdaptiveProbeIndicesDesc(FRDGTextureDesc::Create2D(ScreenTileAdaptiveProbeIndicesBufferSize, PF_R16_UINT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ScreenProbeParameters.ScreenTileAdaptiveProbeHeader = GraphBuilder.CreateTexture(ScreenTileAdaptiveProbeHeaderDesc, TEXT("Lumen.ScreenProbeGather.ScreenTileAdaptiveProbeHeader"));
	ScreenProbeParameters.ScreenTileAdaptiveProbeIndices = GraphBuilder.CreateTexture(ScreenTileAdaptiveProbeIndicesDesc, TEXT("Lumen.ScreenProbeGather.ScreenTileAdaptiveProbeIndices"));

	uint32 ClearValues[4] = {0, 0, 0, 0};
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenTileAdaptiveProbeHeader)), ClearValues);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(NumAdaptiveScreenProbes), 0);

	const uint32 AdaptiveProbeMinDownsampleFactor = FMath::Clamp(GLumenScreenProbeGatherAdaptiveProbeMinDownsampleFactor, 1, 64);

	if (ScreenProbeParameters.MaxNumAdaptiveProbes > 0 && AdaptiveProbeMinDownsampleFactor < ScreenProbeParameters.ScreenProbeDownsampleFactor)
	{ 
		uint32 PlacementDownsampleFactor = ScreenProbeParameters.ScreenProbeDownsampleFactor;
		do
		{
			PlacementDownsampleFactor /= 2;
			FScreenProbeAdaptivePlacementCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FScreenProbeAdaptivePlacementCS::FParameters>();
			PassParameters->RWScreenProbeSceneDepth = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenProbeSceneDepth));
			PassParameters->RWScreenProbeWorldNormal = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenProbeWorldNormal));
			PassParameters->RWScreenProbeWorldSpeed = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenProbeWorldSpeed));
			PassParameters->RWScreenProbeTranslatedWorldPosition = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenProbeTranslatedWorldPosition));
			PassParameters->RWNumAdaptiveScreenProbes = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(NumAdaptiveScreenProbes, PF_R32_UINT));
			PassParameters->RWAdaptiveScreenProbeData = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(AdaptiveScreenProbeData, PF_R32_UINT));
			PassParameters->RWScreenTileAdaptiveProbeHeader = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenTileAdaptiveProbeHeader));
			PassParameters->RWScreenTileAdaptiveProbeIndices = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenTileAdaptiveProbeIndices));
			PassParameters->View = View.ViewUniformBuffer;
			PassParameters->SceneTexturesStruct = SceneTextures.UniformBuffer;
			PassParameters->SceneTextures = SceneTextureParameters;
			PassParameters->ScreenProbeParameters = ScreenProbeParameters;
			PassParameters->PlacementDownsampleFactor = PlacementDownsampleFactor;

			auto ComputeShader = View.ShaderMap->GetShader<FScreenProbeAdaptivePlacementCS>(0);

			FComputeShaderUtils::AddPass(
				GraphBuilder,
				RDG_EVENT_NAME("AdaptivePlacement DownsampleFactor=%u", PlacementDownsampleFactor),
				ComputeShader,
				PassParameters,
				FComputeShaderUtils::GetGroupCount(FIntPoint::DivideAndRoundDown(View.ViewRect.Size(), (int32)PlacementDownsampleFactor), FScreenProbeAdaptivePlacementCS::GetGroupSize()));
		}
		while (PlacementDownsampleFactor > AdaptiveProbeMinDownsampleFactor);
	}
	else
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(AdaptiveScreenProbeData), 0);
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.ScreenTileAdaptiveProbeIndices)), ClearValues);
	}

	FRDGBufferRef ScreenProbeIndirectArgs = GraphBuilder.CreateBuffer(FRDGBufferDesc::CreateIndirectDesc<FRHIDispatchIndirectParameters>((uint32)EScreenProbeIndirectArgs::Max), TEXT("Lumen.ScreenProbeGather.ScreenProbeIndirectArgs"));

	{
		FSetupAdaptiveProbeIndirectArgsCS::FParameters* PassParameters = GraphBuilder.AllocParameters<FSetupAdaptiveProbeIndirectArgsCS::FParameters>();
		PassParameters->RWScreenProbeIndirectArgs = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(ScreenProbeIndirectArgs, PF_R32_UINT));
		PassParameters->ScreenProbeParameters = ScreenProbeParameters;

		auto ComputeShader = View.ShaderMap->GetShader<FSetupAdaptiveProbeIndirectArgsCS>(0);

		FComputeShaderUtils::AddPass(
			GraphBuilder,
			RDG_EVENT_NAME("SetupAdaptiveProbeIndirectArgs"),
			ComputeShader,
			PassParameters,
			FIntVector(1, 1, 1));
	}

	ScreenProbeParameters.ProbeIndirectArgs = ScreenProbeIndirectArgs;

	FLumenCardTracingInputs TracingInputs(GraphBuilder, Scene, View, FrameTemporaries);

	FRDGTextureRef BRDFProbabilityDensityFunction = nullptr;
	FRDGBufferSRVRef BRDFProbabilityDensityFunctionSH = nullptr;
	GenerateBRDF_PDF(GraphBuilder, View, SceneTextures, BRDFProbabilityDensityFunction, BRDFProbabilityDensityFunctionSH, ScreenProbeParameters);

	const LumenRadianceCache::FRadianceCacheInputs RadianceCacheInputs = LumenScreenProbeGatherRadianceCache::SetupRadianceCacheInputs(View);

	if (LumenScreenProbeGather::UseRadianceCache(View))
	{
		FMarkUsedRadianceCacheProbes MarkUsedRadianceCacheProbesCallbacks;

		MarkUsedRadianceCacheProbesCallbacks.AddLambda([](
			FRDGBuilder& GraphBuilder, 
			const FViewInfo& View, 
			const LumenRadianceCache::FRadianceCacheMarkParameters& RadianceCacheMarkParameters)
			{
				MarkUsedProbesForVisualize(GraphBuilder, View, RadianceCacheMarkParameters);
			});

		// Mark radiance caches for screen probes
		MarkUsedRadianceCacheProbesCallbacks.AddLambda([&SceneTextures, &ScreenProbeParameters](
			FRDGBuilder& GraphBuilder, 
			const FViewInfo& View, 
			const LumenRadianceCache::FRadianceCacheMarkParameters& RadianceCacheMarkParameters)
			{
				ScreenGatherMarkUsedProbes(
					GraphBuilder,
					View,
					SceneTextures,
					ScreenProbeParameters,
					RadianceCacheMarkParameters);
			});

		// Mark radiance caches for hair strands
		if (HairStrands::HasViewHairStrandsData(View))
		{
			MarkUsedRadianceCacheProbesCallbacks.AddLambda([](
				FRDGBuilder& GraphBuilder,
				const FViewInfo& View,
				const LumenRadianceCache::FRadianceCacheMarkParameters& RadianceCacheMarkParameters)
				{
					HairStrandsMarkUsedProbes(
						GraphBuilder,
						View,
						RadianceCacheMarkParameters);
				});
		}

		if (Lumen::UseLumenTranslucencyReflections(View))
		{
			const FSceneRenderer& SceneRenderer = *this;
			FViewInfo& ViewNonConst = View;

			MarkUsedRadianceCacheProbesCallbacks.AddLambda([&SceneTextures, &SceneRenderer, &ViewNonConst](
				FRDGBuilder& GraphBuilder,
				const FViewInfo& View,
				const LumenRadianceCache::FRadianceCacheMarkParameters& RadianceCacheMarkParameters)
				{
					LumenTranslucencyReflectionsMarkUsedProbes(
						GraphBuilder,
						SceneRenderer,
						ViewNonConst,
						SceneTextures,
						RadianceCacheMarkParameters);
				});
		}

		RenderRadianceCache(
			GraphBuilder, 
			TracingInputs, 
			RadianceCacheInputs, 
			FRadianceCacheConfiguration(),
			Scene,
			View, 
			&ScreenProbeParameters, 
			BRDFProbabilityDensityFunctionSH, 
			MarkUsedRadianceCacheProbesCallbacks,
			View.ViewState->RadianceCacheState, 
			RadianceCacheParameters);

		if (Lumen::UseLumenTranslucencyReflections(View))
		{
			View.LumenTranslucencyGIVolume.RadianceCacheInterpolationParameters = RadianceCacheParameters;

			extern float GLumenTranslucencyReflectionsRadianceCacheReprojectionRadiusScale;
			extern float GLumenTranslucencyVolumeRadianceCacheClipmapFadeSize;
			View.LumenTranslucencyGIVolume.RadianceCacheInterpolationParameters.RadianceCacheInputs.ReprojectionRadiusScale = GLumenTranslucencyReflectionsRadianceCacheReprojectionRadiusScale;
			View.LumenTranslucencyGIVolume.RadianceCacheInterpolationParameters.RadianceCacheInputs.InvClipmapFadeSize = 1.0f / FMath::Clamp(GLumenTranslucencyVolumeRadianceCacheClipmapFadeSize, .001f, 16.0f);
		}
	}

	if (LumenScreenProbeGather::UseImportanceSampling(View))
	{
		GenerateImportanceSamplingRays(
			GraphBuilder,
			View,
			SceneTextures,
			RadianceCacheParameters,
			BRDFProbabilityDensityFunction,
			BRDFProbabilityDensityFunctionSH,
			ScreenProbeParameters);
	}

	const FIntPoint ScreenProbeTraceBufferSize = ScreenProbeParameters.ScreenProbeAtlasBufferSize * ScreenProbeParameters.ScreenProbeTracingOctahedronResolution;
	FRDGTextureDesc TraceRadianceDesc(FRDGTextureDesc::Create2D(ScreenProbeTraceBufferSize, PF_FloatRGB, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ScreenProbeParameters.TraceRadiance = GraphBuilder.CreateTexture(TraceRadianceDesc, TEXT("Lumen.ScreenProbeGather.TraceRadiance"));
	ScreenProbeParameters.RWTraceRadiance = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.TraceRadiance));

	FRDGTextureDesc TraceHitDesc(FRDGTextureDesc::Create2D(ScreenProbeTraceBufferSize, PF_R32_UINT, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV));
	ScreenProbeParameters.TraceHit = GraphBuilder.CreateTexture(TraceHitDesc, TEXT("Lumen.ScreenProbeGather.TraceHit"));
	ScreenProbeParameters.RWTraceHit = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(ScreenProbeParameters.TraceHit));

	TraceScreenProbes(
		GraphBuilder, 
		Scene,
		View, 
		GLumenGatherCvars.TraceMeshSDFs != 0 && Lumen::UseMeshSDFTracing(ViewFamily),
		SceneTextures,
		LightingChannelsTexture,
		TracingInputs,
		RadianceCacheParameters,
		ScreenProbeParameters,
		MeshSDFGridParameters);
	
	FScreenProbeGatherParameters GatherParameters;
	FilterScreenProbes(GraphBuilder, View, SceneTextures, ScreenProbeParameters, GatherParameters);

	if (LumenScreenProbeGather::UseScreenSpaceBentNormal(ViewFamily.EngineShowFlags))
	{
		ScreenSpaceBentNormalParameters = ComputeScreenSpaceBentNormal(GraphBuilder, Scene, View, SceneTextures, LightingChannelsTexture, ScreenProbeParameters);
	}

	FRDGTextureDesc DiffuseIndirectDesc = FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_FloatRGBA, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef DiffuseIndirect = GraphBuilder.CreateTexture(DiffuseIndirectDesc, TEXT("Lumen.ScreenProbeGather.DiffuseIndirect"));

	FRDGTextureDesc RoughSpecularIndirectDesc = FRDGTextureDesc::Create2D(SceneTextures.Config.Extent, PF_FloatRGB, FClearValueBinding::Black, TexCreate_ShaderResource | TexCreate_UAV);
	FRDGTextureRef RoughSpecularIndirect = GraphBuilder.CreateTexture(RoughSpecularIndirectDesc, TEXT("Lumen.ScreenProbeGather.RoughSpecularIndirect"));

	InterpolateAndIntegrate(
		GraphBuilder,
		SceneTextures,
		View,
		ScreenProbeParameters,
		GatherParameters,
		ScreenSpaceBentNormalParameters,
		DiffuseIndirect,
		RoughSpecularIndirect);

	// Set for DiffuseIndirectComposite
	ScreenSpaceBentNormalParameters.UseScreenBentNormal = ScreenSpaceBentNormalParameters.UseScreenBentNormal != 0 && !LumenScreenProbeGather::ApplyScreenBentNormalDuringIntegration();

	FSSDSignalTextures DenoiserOutputs;
	DenoiserOutputs.Textures[0] = DiffuseIndirect;
	DenoiserOutputs.Textures[1] = RoughSpecularIndirect;
	bLumenUseDenoiserComposite = false;

	if (GLumenScreenProbeTemporalFilter)
	{
		if (GLumenScreenProbeUseHistoryNeighborhoodClamp)
		{
			FRDGTextureRef CompressedDepthTexture;
			FRDGTextureRef CompressedShadingModelTexture;
			{
				FRDGTextureDesc Desc = FRDGTextureDesc::Create2D(
					SceneTextures.Depth.Resolve->Desc.Extent,
					PF_R16F,
					FClearValueBinding::None,					
					/* InTargetableFlags = */ TexCreate_ShaderResource | TexCreate_UAV);

				CompressedDepthTexture = GraphBuilder.CreateTexture(Desc, TEXT("Lumen.ScreenProbeGather.CompressedDepth"));

				Desc.Format = PF_R8_UINT;
				CompressedShadingModelTexture = GraphBuilder.CreateTexture(Desc, TEXT("Lumen.ScreenProbeGather.CompressedShadingModelID"));
			}

			{
				FGenerateCompressedGBuffer::FParameters* PassParameters = GraphBuilder.AllocParameters<FGenerateCompressedGBuffer::FParameters>();
				PassParameters->RWCompressedDepthBufferOutput = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(CompressedDepthTexture));
				PassParameters->RWCompressedShadingModelOutput = GraphBuilder.CreateUAV(FRDGTextureUAVDesc(CompressedShadingModelTexture));
				PassParameters->View = View.ViewUniformBuffer;
				PassParameters->SceneTextures = SceneTextureParameters;

				auto ComputeShader = View.ShaderMap->GetShader<FGenerateCompressedGBuffer>(0);

				FComputeShaderUtils::AddPass(
					GraphBuilder,
					RDG_EVENT_NAME("GenerateCompressedGBuffer"),
					ComputeShader,
					PassParameters,
					FComputeShaderUtils::GetGroupCount(View.ViewRect.Size(), FGenerateCompressedGBuffer::GetGroupSize()));
			}

			FSSDSignalTextures ScreenSpaceDenoiserInputs;
			ScreenSpaceDenoiserInputs.Textures[0] = DiffuseIndirect;
			ScreenSpaceDenoiserInputs.Textures[1] = RoughSpecularIndirect;

			DenoiserOutputs = IScreenSpaceDenoiser::DenoiseIndirectProbeHierarchy(
				GraphBuilder,
				View, 
				PreviousViewInfos,
				SceneTextureParameters,
				ScreenSpaceDenoiserInputs,
				CompressedDepthTexture,
				CompressedShadingModelTexture);

			bLumenUseDenoiserComposite = true;
		}
		else
		{
			UpdateHistoryScreenProbeGather(
				GraphBuilder,
				View,
				SceneTextures,
				DiffuseIndirect,
				RoughSpecularIndirect);

			DenoiserOutputs.Textures[0] = DiffuseIndirect;
			DenoiserOutputs.Textures[1] = RoughSpecularIndirect;
		}
	}

	// Sample radiance caches for hair strands lighting. Only used wht radiance cache is enabled
	if (LumenScreenProbeGather::UseRadianceCache(View) && HairStrands::HasViewHairStrandsData(View))
	{
		RenderHairStrandsLumenLighting(GraphBuilder, Scene, View);
	}

	return DenoiserOutputs;
}

