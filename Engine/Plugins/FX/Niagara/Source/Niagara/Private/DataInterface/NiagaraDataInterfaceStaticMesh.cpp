// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraDataInterfaceStaticMesh.h"
#include "NiagaraEmitterInstance.h"
#include "NiagaraComponent.h"
#include "NiagaraStats.h"
#include "NiagaraSystemInstance.h"
#include "NiagaraRenderer.h"
#include "NiagaraSettings.h"
#include "NiagaraScript.h"
#include "NiagaraDataInterfaceUtilities.h"
#include "ShaderParameterUtils.h"
#include "NiagaraStats.h"

#if WITH_EDITOR
#include "Editor.h"
#include "Subsystems/ImportSubsystem.h"
#endif

#include "Engine/StaticMeshActor.h"
#include "Engine/StaticMeshSocket.h"
#include "Internationalization/Internationalization.h"
#include "GPUSkinCache.h"
#include "ShaderCompilerCore.h"

#include "Renderer/Private/ScenePrivate.h"
#include "DistanceFieldAtlas.h"
#include "Renderer/Private/DistanceFieldLightingShared.h"
#include "NiagaraGpuComputeDispatchInterface.h"
#include "NiagaraGpuComputeDispatch.h"
#include "NiagaraDistanceFieldParameters.h"

#define LOCTEXT_NAMESPACE "NiagaraDataInterfaceStaticMesh"

//-OPT: We should be able to share data between data interfaces where possible

namespace NDIStaticMeshLocal
{
	static const TCHAR* TemplateShaderFile = TEXT("/Plugin/FX/Niagara/Private/NiagaraDataInterfaceStaticMeshTemplate.ush");

	static int32 GNiagaraFailStaticMeshDataInterface = 0;
	static FAutoConsoleVariableRef CVarNiagaraFailStaticMeshDataInterface(
		TEXT("fx.Niagara.FailStaticMeshDataInterface"),
		GNiagaraFailStaticMeshDataInterface,
		TEXT("When enabled we will fail out using static mesh data interfaces."),
		ECVF_Default
	);

	static int32 GNDIStaticMesh_UseInlineLODsOnly = 1;
	static FAutoConsoleVariableRef CVarNDIStaticMesh_UseInlineLODsOnly(
		TEXT("fx.Niagara.NDIStaticMesh.UseInlineLODsOnly"),
		GNDIStaticMesh_UseInlineLODsOnly,
		TEXT("When enabled Niagara will never use streaming LOD levels, only inline LODs."),
		ECVF_Default
	);

	//////////////////////////////////////////////////////////////////////////
	struct EDIFunctionVersion
	{
		enum Type
		{
			InitialVersion = 0,
			RefactoredV1 = 1,
			LargeWorldCoordinates = 2,
			LargeWorldCoordinates2 = 3,

			VersionPlusOne,
			LatestVersion = VersionPlusOne - 1
		};
	};

	//////////////////////////////////////////////////////////////////////////
	// Vertex Sampling
	static const FName	IsValidVertexName("IsValidVertex");
	static const FName	RandomVertexName("RandomVertex");
	static const FName	GetVertexCountName("GetVertexCount");

	static const FName	GetVertexName("GetVertex");
	static const FName	GetVertexWSName("GetVertexWS");
	static const FName	GetVertexColorName("GetVertexColor");
	static const FName	GetVertexUVName("GetVertexUV");

	//////////////////////////////////////////////////////////////////////////
	// VM Triangle Sampling
	static const FName	IsValidTriangleName("IsValidTriangle");
	static const FName	RandomTriangleName("RandomTriangle");
	static const FName	GetTriangleCountName("GetTriangleCount");

	static const FName	IsValidFilteredTriangleName("IsValidFilteredTriangle");
	static const FName	RandomFilteredTriangleName("RandomFilteredTriangle");
	static const FName	GetFilteredTriangleCountName("GetFilteredTriangleCount");
	static const FName	GetFilteredTriangleAtName("GetFilteredTriangleAt");

	static const FName	IsValidUnfilteredTriangleName("IsValidUnfilteredTriangle");
	static const FName	RandomUnfilteredTriangleName("RandomUnfilteredTriangle");
	static const FName	GetUnfilteredTriangleCountName("GetUnfilteredTriangleCount");
	static const FName	GetUnfilteredTriangleAtName("GetUnfilteredTriangleAt");

	static const FName	GetTriangleName("GetTriangle");
	static const FName	GetTriangleWSName("GetTriangleWS");
	static const FName	GetTriangleColorName("GetTriangleColor");
	static const FName	GetTriangleUVName("GetTriangleUV");

	static const FName	GetTriangleIndicesName("GetTriangleIndices");

	//////////////////////////////////////////////////////////////////////////
	// Socket Functions
	static const FName	GetSocketCountName("GetSocketCount");
	static const FName	GetFilteredSocketCountName("GetFilteredSocketCount");
	static const FName	GetUnfilteredSocketCountName("GetUnfilteredSocketCount");
	static const FName	GetSocketTransformName("GetSocketTransform");
	static const FName	GetSocketTransformWSName("GetSocketTransformWS");
	static const FName	GetFilteredSocketTransformName("GetFilteredSocketTransform");
	static const FName	GetFilteredSocketTransformWSName("GetFilteredSocketTransformWS");
	static const FName	GetUnfilteredSocketTransformName("GetUnfilteredSocketTransform");
	static const FName	GetUnfilteredSocketTransformWSName("GetUnfilteredSocketTransformWS");

	//////////////////////////////////////////////////////////////////////////
	// Section functions
	static const FName	IsValidSectionName("IsValidSection");
	static const FName	GetSectionTriangleCountName("GetSectionTriangleCount");
	static const FName	RandomSectionTriangleName("RandomSectionTriangle");
	static const FName	GetSectionTriangleAtName("GetSectionTriangleAt");

	static const FName	GetFilteredSectionAtName("GetFilteredSectionAt");
	static const FName	GetUnfilteredSectionAtName("GetUnfilteredSectionAt");

	static const FName	GetSectionCountName("GetSectionCount");
	static const FName	GetFilteredSectionCountName("GetFilteredSectionCount");
	static const FName	GetUnfilteredSectionCountName("GetUnfilteredSectionCount");

	static const FName	RandomSectionName("RandomSection");
	static const FName	RandomFilteredSectionName("RandomFilteredSection");
	static const FName	RandomUnfilteredSectionName("RandomUnfilteredSection");

	//////////////////////////////////////////////////////////////////////////
	// VM Misc Functions
	static const FName	IsValidName("IsValid");

	static const FName	GetLocalToWorldName("GetLocalToWorld");
	static const FName	GetLocalToWorldInverseTransposedName("GetLocalToWorldInverseTransposed");
	static const FName	GetWorldVelocityName("GetWorldVelocity");

	//////////////////////////////////////////////////////////////////////////
	// Distance Field Functions
	static const FName	QueryDistanceFieldName("QueryDistanceField");

	//////////////////////////////////////////////////////////////////////////
	// Deprecated Functions
	static const FName Deprecated_RandomSectionName("DeprecatedRandomSection");
	static const FName Deprecated_RandomTriCoordName("DeprecatedRandomTriCoord");
	static const FName Deprecated_RandomTriCoordOnSectionName("DeprecatedRandomTriCoordOnSection");
	static const FName Deprecated_RandomTriCoordVCFilteredName("DeprecatedRandomTriCoordUsingVertexColorFilter");
	static const FName Deprecated_GetVertexPositionName("DeprecatedGetVertexPosition");
	static const FName Deprecated_GetVertexPositionWSName("DeprecatedGetVertexPositionWS");
	static const FName Deprecated_GetTriPositionName("DeprecatedGetTriPosition");
	static const FName Deprecated_GetTriNormalName("DeprecatedGetTriNormal");
	static const FName Deprecated_GetTriTangentsName("DeprecatedGetTriTangents");
	static const FName Deprecated_GetTriPositionWSName("DeprecatedGetTriPositionWS");
	static const FName Deprecated_GetTriNormalWSName("DeprecatedGetTriNormalWS");
	static const FName Deprecated_GetTriTangentsWSName("DeprecatedGetTriTangentsWS");
	static const FName Deprecated_GetTriColorName("DeprecatedGetTriColor");
	static const FName Deprecated_GetTriUVName("DeprecatedGetTriUV");
	static const FName Deprecated_GetTriPositionAndVelocityWSName("DeprecatedGetTriPositionAndVelocityWS");
	//static const FName Deprecated_GetMeshLocalToWorldInverseTransposedName("DeprecatedGetMeshLocalToWorldInverseTransposed");

	//////////////////////////////////////////////////////////////////////////
	// Shader Parameters
	static FString	NumTriangles_String("NumTriangles_");
	static FString	NumVertices_String("NumVertices_");
	static FString	NumUVs_String("NumUVs_");
	static FString	HasColors_String("HasColors_");
	static FString	IndexBuffer_String("IndexBuffer_");
	static FString	PositionBuffer_String("PositionBuffer_");
	static FString	TangentBuffer_String("TangentBuffer_");
	static FString	UVBuffer_String("UVBuffer_");
	static FString	ColorBuffer_String("ColorBuffer_");

	static FString	HasUniformSampling_String("HasUniformSampling_");
	static FString	UniformSamplingTriangles_String("UniformSamplingTriangles_");

	static FString	SectionCounts_String("SectionCounts_");
	static FString	SectionInfos_String("SectionInfos_");
	static FString	FilteredAndUnfilteredSections_String("FilteredAndUnfilteredSections_");

	static FString	SocketCounts_String("SocketCounts_");
	static FString	SocketTransforms_String("SocketTransforms_");
	static FString	FilteredAndUnfilteredSockets_String("FilteredAndUnfilteredSockets_");

	static FString	InvDeltaSeconds_String("InvDeltaSeconds_");
	static FString	InstanceTransform_String("InstanceTransform_");
	static FString	InstanceTransformInverseTransposed_String("InstanceTransformInverseTransposed_");
	static FString	InstanceRotation_String("InstanceRotation_");
	static FString	InstancePreviousTransform_String("InstancePreviousTransform_");
	static FString	InstancePreviousTransformInverseTransposed_String("InstancePreviousTransformInverseTransposed_");
	static FString	InstancePreviousRotation_String("InstancePreviousRotation_");
	static FString	InstanceWorldVelocity_String("InstanceWorldVelocity_");

	static FString	InstanceDistanceFieldIndex_String("InstanceDistanceFieldIndex_");

	//////////////////////////////////////////////////////////////////////////
	struct FNDISectionInfo
	{
		uint32 FirstTriangle = 0;
		uint32 NumTriangles = 0;
		float  Prob = 0.0f;
		uint32 Alias = 0;
	};

	class FNDISectionAreaWeightedSampler : public FWeightedRandomSampler
	{
	public:
		static void Init(const FStaticMeshLODResources* LODResource, TConstArrayView<int32> Sections, const FStaticMeshSectionAreaWeightedTriangleSamplerArray* MeshSectionSamplers, TArray<FNDISectionInfo>& OutSectionInfos)
		{
			if (MeshSectionSamplers)
			{
				FNDISectionAreaWeightedSampler Sampler;
				Sampler.Init(Sections, *MeshSectionSamplers);
				for (int32 i=0; i < Sections.Num(); ++i)
				{
					const int32 SectionIndex	= Sections[i];
					FNDISectionInfo& SectionInfo= OutSectionInfos.AddDefaulted_GetRef();
					SectionInfo.FirstTriangle	= LODResource->Sections[SectionIndex].FirstIndex / 3;
					SectionInfo.NumTriangles	= LODResource->Sections[SectionIndex].NumTriangles;
					SectionInfo.Prob			= Sampler.GetProb()[i];
					SectionInfo.Alias			= Sampler.GetAlias()[i];
				}
			}
			else
			{
				for (int32 i=0; i < Sections.Num(); ++i)
				{
					const int32 SectionIndex	= Sections[i];
					FNDISectionInfo& SectionInfo= OutSectionInfos.AddDefaulted_GetRef();
					SectionInfo.FirstTriangle	= LODResource->Sections[SectionIndex].FirstIndex / 3;
					SectionInfo.NumTriangles	= LODResource->Sections[SectionIndex].NumTriangles;
					SectionInfo.Prob			= 1.0f;
					SectionInfo.Alias			= i;
				}
			}
		}

		void Init(TConstArrayView<int32> Sections, const FStaticMeshSectionAreaWeightedTriangleSamplerArray& MeshSectionSamplers)
		{
			TempTotalWeight = 0.0f;
			TempWeights.Empty();

			for (const int32 SectionIndex : Sections)
			{
				const FStaticMeshSectionAreaWeightedTriangleSampler& SectionSampler = MeshSectionSamplers[SectionIndex];
				const float SectionWeight = SectionSampler.GetTotalWeight();
				TempTotalWeight += SectionWeight;
				TempWeights.Add(SectionWeight);
			}

			FWeightedRandomSampler::Initialize();
		}

	protected:
		virtual float GetWeights(TArray<float>& OutWeights) override
		{
			OutWeights = MoveTemp(TempWeights);
			return TempTotalWeight;
		}

		float			TempTotalWeight = 0.0f;
		TArray<float>	TempWeights;
	};

	//////////////////////////////////////////////////////////////////////////
	struct FGpuInitializeData
	{
		struct FRenderProxy*				RenderProxy = nullptr;
		FNiagaraSystemInstanceID			SystemInstanceID;
		const FStaticMeshLODResources*		LODResource = nullptr;

		int32								NumFilteredTriangles = 0;
		int32								NumUnfilteredTriangles = 0;

		bool								bGpuUniformDistribution = false;

		int32								NumSections = 0;
		int32								NumFilteredSections = 0;
		int32								NumUnfilteredSections = 0;
		TResourceArray<FIntVector4>			SectionInfos;
		TResourceArray<uint16>				FilteredAndUnfilteredSections;

		int32								NumSockets = 0;
		int32								NumFilteredSockets = 0;
		int32								NumUnfilteredSockets = 0;
		TResourceArray<FVector4f>			SocketTransforms;
		TResourceArray<uint16>				FilteredAndUnfilteredSockets;
	};

	//////////////////////////////////////////////////////////////////////////
	struct FInstanceData_RenderThread
	{
		bool		bIsValid = false;
		bool		bGpuUniformDistribution = false;
		FMatrix44f	Transform = FMatrix44f::Identity;
		FMatrix44f	PrevTransform = FMatrix44f::Identity;
		FQuat4f		Rotation = FQuat4f::Identity;
		FQuat4f		PrevRotation = FQuat4f::Identity;
		float		DeltaSeconds = 0.0f;

		FPrimitiveComponentId	DistanceFieldPrimitiveId;

		FIntVector NumTriangles = FIntVector::ZeroValue;
		int32 NumVertices = 0;
		int32 NumUVs = 0;
		FShaderResourceViewRHIRef MeshIndexBufferSRV;
		FShaderResourceViewRHIRef MeshPositionBufferSRV;
		FShaderResourceViewRHIRef MeshTangentBufferSRV;
		FShaderResourceViewRHIRef MeshUVBufferSRV;
		FShaderResourceViewRHIRef MeshColorBufferSRV;

		FShaderResourceViewRHIRef MeshUniformSamplingTriangleSRV;

		FIntVector	SectionCounts = FIntVector::ZeroValue;	// X=NumSections, y = Num Filtered, Z=NumUnfiltered
		FReadBuffer SectionInfos;
		FReadBuffer FilteredAndUnfilteredSections;

		FIntVector	SocketCounts = FIntVector::ZeroValue;	// X=NumSockets, Y=NumFiltered, Z=NumUnfiltered
		FReadBuffer	SocketTransforms;
		FReadBuffer	FilteredAndUnfilteredSockets;

	#if STATS
		int64 GPUMemoryUsage = 0;
	#endif

		~FInstanceData_RenderThread()
		{
			Release();
		}

		void Init(FGpuInitializeData& GpuInitializeData)
		{
			// Gather mesh buffers
			if (GpuInitializeData.LODResource)
			{
				MeshIndexBufferSRV = RHICreateShaderResourceView(GpuInitializeData.LODResource->IndexBuffer.IndexBufferRHI);
				MeshPositionBufferSRV = GpuInitializeData.LODResource->VertexBuffers.PositionVertexBuffer.GetSRV();
				MeshTangentBufferSRV = GpuInitializeData.LODResource->VertexBuffers.StaticMeshVertexBuffer.GetTangentsSRV();
				MeshUVBufferSRV = GpuInitializeData.LODResource->VertexBuffers.StaticMeshVertexBuffer.GetTexCoordsSRV();
				MeshColorBufferSRV = GpuInitializeData.LODResource->VertexBuffers.ColorVertexBuffer.GetColorComponentsSRV();

				NumTriangles.X = GpuInitializeData.LODResource->IndexBuffer.GetNumIndices() / 3;
				NumTriangles.Y = GpuInitializeData.NumFilteredTriangles;
				NumTriangles.Z = GpuInitializeData.NumUnfilteredTriangles;
				NumVertices = GpuInitializeData.LODResource->VertexBuffers.PositionVertexBuffer.GetNumVertices();
				NumUVs = MeshUVBufferSRV.IsValid() ? GpuInitializeData.LODResource->VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() : 0;

				bIsValid = MeshIndexBufferSRV.IsValid() && MeshPositionBufferSRV.IsValid() && MeshTangentBufferSRV.IsValid();

				// Build uniform sampling buffer
				MeshUniformSamplingTriangleSRV = GpuInitializeData.bGpuUniformDistribution ? GpuInitializeData.LODResource->AreaWeightedSectionSamplersBuffer.GetBufferSRV() : nullptr;
				bGpuUniformDistribution = GpuInitializeData.bGpuUniformDistribution && MeshUniformSamplingTriangleSRV.IsValid();

				// Build section buffers
				SectionCounts.X = GpuInitializeData.NumSections;
				SectionCounts.Y = GpuInitializeData.NumFilteredSections;
				SectionCounts.Z = GpuInitializeData.NumUnfilteredSections;
				if (GpuInitializeData.SectionInfos.Num() > 0)
				{
					SectionInfos.Initialize(TEXT("NDISkelMesh_SectionInfos"), sizeof(FIntVector4), GpuInitializeData.SectionInfos.Num(), EPixelFormat::PF_R32G32B32A32_UINT, BUF_Static, &GpuInitializeData.SectionInfos);
				}
				if (GpuInitializeData.FilteredAndUnfilteredSections.Num() > 0)
				{
					FilteredAndUnfilteredSections.Initialize(TEXT("NDISkelMesh_FilteredAndUnfilteredSections"), sizeof(uint16), GpuInitializeData.FilteredAndUnfilteredSections.Num(), EPixelFormat::PF_R16_UINT, BUF_Static, &GpuInitializeData.FilteredAndUnfilteredSections);
				}
			}
			else
			{
				NumTriangles = FIntVector::ZeroValue;
				NumVertices = 0;
				NumUVs = 0;

				bIsValid = false;

				bGpuUniformDistribution = false;

				SectionCounts = FIntVector::ZeroValue;
			}

			// Build socket buffers
			SocketCounts.X = GpuInitializeData.NumSockets;
			SocketCounts.Y = GpuInitializeData.NumFilteredSockets;
			SocketCounts.Z = GpuInitializeData.NumUnfilteredSockets;
			if ( GpuInitializeData.SocketTransforms.Num() > 0 )
			{
				SocketTransforms.Initialize(TEXT("NDISkelMesh_SocketTransforms"), sizeof(FVector4f), GpuInitializeData.SocketTransforms.Num(), EPixelFormat::PF_A32B32G32R32F, BUF_Static, &GpuInitializeData.SocketTransforms);
			}
			if ( GpuInitializeData.FilteredAndUnfilteredSockets.Num() > 0 )
			{
				FilteredAndUnfilteredSockets.Initialize(TEXT("NDISkelMesh_FilteredAndUnfilteredSockets"), sizeof(uint16), GpuInitializeData.FilteredAndUnfilteredSockets.Num(), EPixelFormat::PF_R16_UINT, BUF_Static, &GpuInitializeData.FilteredAndUnfilteredSockets);
			}
		#if STATS
			GPUMemoryUsage = SectionInfos.NumBytes + FilteredAndUnfilteredSections.NumBytes + SocketTransforms.NumBytes + FilteredAndUnfilteredSockets.NumBytes;
			INC_MEMORY_STAT_BY(STAT_NiagaraGPUDataInterfaceMemory, GPUMemoryUsage);
		#endif
		}

		void Release()
		{
			bIsValid = false;

		#if STATS
			INC_MEMORY_STAT_BY(STAT_NiagaraGPUDataInterfaceMemory, GPUMemoryUsage);
			GPUMemoryUsage = 0;
		#endif

			MeshIndexBufferSRV.SafeRelease();
			MeshPositionBufferSRV.SafeRelease();
			MeshTangentBufferSRV.SafeRelease();
			MeshUVBufferSRV.SafeRelease();
			MeshColorBufferSRV.SafeRelease();

			SectionInfos.Release();
			FilteredAndUnfilteredSections.Release();

			SocketTransforms.Release();
			FilteredAndUnfilteredSockets.Release();
		}
	};

	struct FInstanceData_FromGameThread
	{
		FMatrix44f				Transform = FMatrix44f::Identity;
		FMatrix44f				PrevTransform = FMatrix44f::Identity;
		FQuat4f					Rotation = FQuat4f::Identity;
		FQuat4f					PrevRotation = FQuat4f::Identity;
		float					DeltaSeconds = 0.0f;
		FPrimitiveComponentId	DistanceFieldPrimitiveId;
	};

	struct FInstanceData_GameThread
	{
		/** Cached ptr to StaticMeshComponent we sample from, when found. Otherwise, the SceneComponent to use to transform the Default or Preview mesh. */
		TWeakObjectPtr<USceneComponent> SceneComponentWeakPtr;

		/** Cached ptr to the mesh so that we can make sure that we haven't been deleted. */
		TWeakObjectPtr<UStaticMesh> StaticMeshWeakPtr;

		/** Cached ComponentToWorld. (Falls back to WorldTransform of the system instance) */
		FMatrix Transform;
		/** InverseTranspose of above for transforming normals/tangents. */
		FMatrix TransformInverseTransposed;

		/** Cached ComponentToWorld from previous tick. */
		FMatrix PrevTransform;
		/** Cached Previous InverseTranspose of above for transforming normals/tangents. */
		FMatrix PrevTransformInverseTransposed;

		/** Cached Rotation. */
		FQuat4f Rotation;
		/** Cached Previous Rotation. */
		FQuat4f PrevRotation;

		/** Time separating Transform and PrevTransform. */
		float DeltaSeconds;

		/** Velocity set by the physics body of the mesh component */
		FVector PhysicsVelocity;
		/** True if velocity should not be calculated via the transforms, but rather read the physics data from the mesh component */
		uint32 bUsePhysicsVelocity : 1;

		/** True if SceneComponent was valid on initialization (used to track invalidation of the component on tick) */
		uint32 bComponentValid : 1;

		/** True if StaticMesh was valid on initialization (used to track invalidation of the mesh on tick) */
		uint32 bMeshValid : 1;

		/** True if the mesh allows CPU access. Use to reset the instance in the editor*/
		uint32 bMeshAllowsCpuAccess : 1;
		/** True if the mesh we're using allows area weighted sampling on CPU. */
		uint32 bIsCpuUniformlyDistributedSampling : 1;
		/** True if the mesh we're using allows area weighted sampling on GPU. */
		uint32 bIsGpuUniformlyDistributedSampling : 1;

		/** Filtered sections followed by unfiltered sections. */
		int32 NumFilteredSections = 0;
		int32 NumUnfilteredSections = 0;
		int32 NumFilteredTriangles = 0;
		int32 NumUnfilteredTriangles = 0;
		TArray<int32> FilteredAndUnfilteredSections;
		TArray<FNDISectionInfo> FilteredAndUnfilteredSectionInfos;

		///** Allows sampling of the mesh's tris based on a dynamic color range. */
		//TSharedPtr<struct FDynamicVertexColorFilterData> DynamicVertexColorSampler;

		/** Cached change id off of the data interface.*/
		uint32 ChangeId = 0;

		/** The MinLOD, see UStaticMesh::MinLOD which is platform specific.*/
		int32 MinLOD = 0;
		/** The cached LODIdx used to initialize the FNDIStaticMesh_InstanceData.*/
		int32 CachedLODIdx = 0;

		/** Cached socket information, if available */
		TArray<FTransform3f> CachedSockets;

		/** Number of filtered sockets. */
		int32 NumFilteredSockets = 0;

		/** Filter sockets followed by unfiltered sockets */
		TArray<uint16> FilteredAndUnfilteredSockets;

#if WITH_EDITOR
		FDelegateHandle OnMeshChanged;
		FDelegateHandle OnPostMeshBuild;
		FDelegateHandle OnMeshReimported;

		void OnEditorMeshChanged(UObject* OptionalMesh)
		{
			UStaticMesh* StaticMesh = StaticMeshWeakPtr.Get();
			if (!OptionalMesh || (OptionalMesh == StaticMesh))
			{
				StaticMeshWeakPtr.Reset();
				if (StaticMesh)
				{
					StaticMesh->GetOnMeshChanged().Remove(OnMeshChanged);
					StaticMesh->OnPostMeshBuild().Remove(OnPostMeshBuild);
				}
				if (UImportSubsystem* ImportSubsystem = GEditor->GetEditorSubsystem<UImportSubsystem>())
				{
					ImportSubsystem->OnAssetReimport.Remove(OnMeshReimported);
				}
			}
			OnMeshChanged.Reset();
			OnPostMeshBuild.Reset();
			OnMeshReimported.Reset();
		}
#endif

		bool Init(UNiagaraDataInterfaceStaticMesh* Interface, FNiagaraSystemInstance* SystemInstance)
		{
			check(SystemInstance);

			// Initialize members
			SceneComponentWeakPtr = nullptr;
			StaticMeshWeakPtr = nullptr;
			Transform = FMatrix::Identity;
			TransformInverseTransposed = FMatrix::Identity;
			PrevTransform = FMatrix::Identity;
			PrevTransformInverseTransposed = FMatrix::Identity;
			Rotation = FQuat4f::Identity;
			PrevRotation = FQuat4f::Identity;
			DeltaSeconds = 0.0f;
			PhysicsVelocity = FVector::ZeroVector;
			bUsePhysicsVelocity = Interface->bUsePhysicsBodyVelocity;
			bComponentValid = false;
			bMeshValid = false;
			bMeshAllowsCpuAccess = false;
			bIsCpuUniformlyDistributedSampling = false;
			bIsGpuUniformlyDistributedSampling = false;
			NumFilteredSections = 0;
			NumUnfilteredSections = 0;
			FilteredAndUnfilteredSections.Empty();
			FilteredAndUnfilteredSectionInfos.Empty();
			NumFilteredTriangles = 0;
			NumUnfilteredTriangles = 0;
			ChangeId = Interface->ChangeId;
			MinLOD = 0;
			CachedLODIdx = 0;
			CachedSockets.Empty();
			NumFilteredSockets = 0;
			FilteredAndUnfilteredSockets.Empty();

			// Get component / mesh we are using
			USceneComponent* SceneComponent = nullptr;
			UStaticMesh* StaticMesh = Interface->GetStaticMesh(SceneComponent, SystemInstance);
			SceneComponentWeakPtr = SceneComponent;

			// Gather attached information
			bComponentValid = SceneComponent != nullptr;
			FTransform ComponentTransform = bComponentValid ? SceneComponent->GetComponentToWorld() : SystemInstance->GetWorldTransform();
			ComponentTransform.AddToTranslation(FVector(SystemInstance->GetLWCTile()) * -FLargeWorldRenderScalar::GetTileSize());

			Transform = ComponentTransform.ToMatrixWithScale();
			TransformInverseTransposed = ComponentTransform.Inverse().ToMatrixWithScale().GetTransposed();
			PrevTransform = Transform;
			PrevTransformInverseTransposed = ComponentTransform.Inverse().ToMatrixWithScale().GetTransposed();

			Rotation = FQuat4f(ComponentTransform.GetRotation());
			PrevRotation = Rotation;

			if (bUsePhysicsVelocity)
			{
				if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(SceneComponent))
				{
					PhysicsVelocity = MeshComponent->GetPhysicsLinearVelocity();
				}
			}

			if (GNiagaraFailStaticMeshDataInterface != 0)
			{
				UE_LOG(LogNiagara, Log, TEXT("StaticMesh data interface aborting because \"fx.Niagara.FailStaticMeshDataInterface 1\". Failed InitPerInstanceData - %s"), *Interface->GetFullName());
				return false;
			}

			// Report missing or inaccessible meshes to the log
			if (StaticMesh == nullptr)
			{
				UE_LOG(LogNiagara, Log, TEXT("StaticMesh data interface has no valid mesh - %s"), *Interface->GetFullName());
			}
			else if (!StaticMesh->bAllowCPUAccess)
			{
				//-TODO: This will be modified with the refactor to allow GPU to be used when CPU is also used
				if (Interface->IsUsedWithGPUEmitter() && !Interface->IsUsedWithCPUEmitter())
				{
					if (!FNiagaraUtilities::AreBufferSRVsAlwaysCreated(GMaxRHIShaderPlatform))
					{
						UE_LOG(LogNiagara, Log, TEXT("NiagaraStaticMeshDataInterface used by GPU emitter but does not have SRV access on this platform.  Enable CPU access to fix this issue. Interface: %s, Mesh: %s"), *GetFullNameSafe(Interface), *GetFullNameSafe(StaticMesh));
						StaticMesh = nullptr;
					}
				}
				else
				{
					UE_LOG(LogNiagara, Log, TEXT("NiagaraStaticMeshDataInterface used by CPU emitter and does not allow CPU access. Interface: %s, Mesh: %s"), *GetFullNameSafe(Interface), *GetFullNameSafe(StaticMesh));
					StaticMesh = nullptr;
				}
			}

			TRefCountPtr<const FStaticMeshLODResources> LODData;
			if (StaticMesh != nullptr)
			{
				// Check if any valid LODs are found. If not, we won't use this mesh
				MinLOD = StaticMesh->GetMinLOD().GetValue();
				if (GNDIStaticMesh_UseInlineLODsOnly)
				{
					MinLOD = StaticMesh->GetNumLODs() - StaticMesh->GetRenderData()->NumInlinedLODs;
				}

				FStaticMeshRenderData* RenderData = StaticMesh->GetRenderData();
				if (RenderData)
				{
					CachedLODIdx = RenderData->GetCurrentFirstLODIdx(MinLOD);
					if (RenderData->LODResources.IsValidIndex(CachedLODIdx))
					{
						LODData = &RenderData->LODResources[CachedLODIdx];
					}
				}

				if (!LODData.IsValid())
				{
					StaticMesh = nullptr;
				}
			}

			StaticMeshWeakPtr = StaticMesh;
			bMeshValid = StaticMesh != nullptr;

			if (StaticMesh != nullptr)
			{
#if WITH_EDITOR
				if ( GIsEditor )
				{
					OnMeshChanged = StaticMesh->GetOnMeshChanged().AddLambda([this]() { OnEditorMeshChanged(nullptr); });
					OnPostMeshBuild = StaticMesh->OnPostMeshBuild().AddLambda([this](UStaticMesh* StaticMesh) { OnEditorMeshChanged(StaticMesh); });
					if (UImportSubsystem* ImportSubsystem = GEditor->GetEditorSubsystem<UImportSubsystem>())
					{
						OnMeshReimported = ImportSubsystem->OnAssetReimport.AddLambda([this](UObject* Object) { OnEditorMeshChanged(Object); });
					}
				}
#endif

				bMeshAllowsCpuAccess = StaticMesh->bAllowCPUAccess;
				bIsCpuUniformlyDistributedSampling = StaticMesh->bSupportUniformlyDistributedSampling;
				if (bIsCpuUniformlyDistributedSampling)
				{
					bIsCpuUniformlyDistributedSampling &= LODData->AreaWeightedSampler.GetNumEntries() == LODData->Sections.Num();
					bIsCpuUniformlyDistributedSampling &= LODData->AreaWeightedSectionSamplers.Num() == LODData->Sections.Num();
				}
				bIsGpuUniformlyDistributedSampling = bIsCpuUniformlyDistributedSampling && StaticMesh->bSupportGpuUniformlyDistributedSampling;

				// Init section filter
				//-OPT: I don't foresee a lot of entries here, could potentially optimize this later to avoid insert
				FilteredAndUnfilteredSections.Reserve(LODData->Sections.Num());
				FilteredAndUnfilteredSectionInfos.Reserve(LODData->Sections.Num());
				for (int32 i = 0; i < LODData->Sections.Num(); ++i)
				{
					// Skip garbage data
					if ( LODData->Sections[i].NumTriangles == 0 )
					{
						continue;
					}

					if (Interface->SectionFilter.AllowedMaterialSlots.Num() == 0 || Interface->SectionFilter.AllowedMaterialSlots.Contains(LODData->Sections[i].MaterialIndex))
					{
						FilteredAndUnfilteredSections.Insert(i, NumFilteredSections);
						++NumFilteredSections;
						NumFilteredTriangles += LODData->Sections[i].NumTriangles;
					}
					else
					{
						FilteredAndUnfilteredSections.Add(i);
						++NumUnfilteredSections;
						NumUnfilteredTriangles += LODData->Sections[i].NumTriangles;
					}
				}

				// Initialize section information
				{
					const FStaticMeshSectionAreaWeightedTriangleSamplerArray* SectionSamplerArray = bIsCpuUniformlyDistributedSampling ? &LODData->AreaWeightedSectionSamplers : nullptr;
					FNDISectionAreaWeightedSampler::Init(LODData, GetFilteredSections(), SectionSamplerArray, FilteredAndUnfilteredSectionInfos);
					FNDISectionAreaWeightedSampler::Init(LODData, GetUnfilteredSections(), SectionSamplerArray, FilteredAndUnfilteredSectionInfos);
				}

				// Init socket information
				const int32 NumMeshSockets = StaticMesh->Sockets.Num();
				if (NumMeshSockets > 0)
				{
					if (NumMeshSockets > TNumericLimits<uint16>::Max())
					{
						UE_LOG(LogNiagara, Warning, TEXT("Static Mesh Data Interface '%s' requires more sockets '%d' than we currently support '%d' Mesh '%s' Component '%s'"), *GetFullNameSafe(Interface), NumMeshSockets, TNumericLimits<uint16>::Max(), *GetFullNameSafe(StaticMesh), *GetFullNameSafe(SceneComponent));
						return false;
					}

					CachedSockets.AddDefaulted(NumMeshSockets);
					for (int32 i = 0; i < NumMeshSockets; ++i)
					{
						CachedSockets[i].SetTranslation(FVector3f(StaticMesh->Sockets[i]->RelativeLocation));
						CachedSockets[i].SetRotation(FQuat4f(FRotator3f(StaticMesh->Sockets[i]->RelativeRotation)));
						CachedSockets[i].SetScale3D(FVector3f(StaticMesh->Sockets[i]->RelativeScale));
					}

					NumFilteredSockets = 0;
					if (Interface->FilteredSockets.Num() > 0)
					{
						FilteredAndUnfilteredSockets.Reserve(NumMeshSockets);
						for (int32 i = 0; i < Interface->FilteredSockets.Num(); ++i)
						{
							const FName FilteredSocketName = Interface->FilteredSockets[i];
							const int32 SocketIndex = StaticMesh->Sockets.IndexOfByPredicate([&](const UStaticMeshSocket* Socket) { return Socket->SocketName == FilteredSocketName; });
							if (SocketIndex == INDEX_NONE)
							{
								UE_LOG(LogNiagara, Warning, TEXT("Static Mesh Data Interface '%s' could not find socket '%s' Mesh '%s' Component '%s'"), *GetFullNameSafe(Interface), *FilteredSocketName.ToString(), *GetFullNameSafe(StaticMesh), *GetFullNameSafe(SceneComponent));
								continue;
							}
							++NumFilteredSockets;
							FilteredAndUnfilteredSockets.Add(uint16(SocketIndex));
						}

						for (int32 i = 0; i < NumMeshSockets; ++i)
						{
							FilteredAndUnfilteredSockets.AddUnique(uint16(i));
						}
					}
				}
			}

			return true;
		}

		bool Tick(UNiagaraDataInterfaceStaticMesh* Interface, FNiagaraSystemInstance* SystemInstance, float InDeltaSeconds)
		{
			if (ResetRequired(Interface))
			{
				return true;
			}

			DeltaSeconds = InDeltaSeconds;

			USceneComponent* SceneComponent = SceneComponentWeakPtr.Get();
			FTransform ComponentTransform = SceneComponent != nullptr ? SceneComponent->GetComponentToWorld() : SystemInstance->GetWorldTransform();
			ComponentTransform.AddToTranslation(FVector(SystemInstance->GetLWCTile()) * -FLargeWorldRenderScalar::GetTileSize());

			PrevTransform = Transform;
			PrevTransformInverseTransposed = TransformInverseTransposed;
			Transform = ComponentTransform.ToMatrixWithScale();
			TransformInverseTransposed = ComponentTransform.Inverse().ToMatrixWithScale().GetTransposed();

			PrevRotation = Rotation;
			Rotation = FQuat4f(ComponentTransform.GetRotation());

			if (bUsePhysicsVelocity)
			{
				if (UStaticMeshComponent* MeshComponent = Cast<UStaticMeshComponent>(SceneComponent))
				{
					PhysicsVelocity = MeshComponent->GetPhysicsLinearVelocity();
				}
				else
				{
					PhysicsVelocity = FVector::ZeroVector;
				}
			}

			return false;
		}

		void Release()
		{
#if WITH_EDITOR
			if (GIsEditor)
			{
				// We reuse the function to clear the delegates out
				OnEditorMeshChanged(nullptr);
			}
#endif
		}

		bool ResetRequired(UNiagaraDataInterfaceStaticMesh* Interface) const
		{
			USceneComponent* Component = SceneComponentWeakPtr.Get();
			if (bComponentValid && !Component)
			{
				// The component we were bound to is no longer valid so we have to trigger a reset.
				return true;
			}

			UStaticMesh* Mesh = StaticMeshWeakPtr.Get();
			if (bMeshValid)
			{
				if (!Mesh)
				{
					// The static mesh we were bound to is no longer valid so we have to trigger a reset.
					return true;
				}
				else if (UStaticMeshComponent* StaticMeshComp = Cast<UStaticMeshComponent>(Component))
				{
					if (Mesh != StaticMeshComp->GetStaticMesh())
					{
						// The mesh changed on the component we're attached to so we have to reset
						return true;
					}
				}
			}

			if (Interface != nullptr && ChangeId != Interface->ChangeId)
			{
				return true;
			}

			if (Mesh != nullptr)
			{
				// Currently we only reset if the cached LOD was streamed out, to avoid performance hits. To revisit.
				// We could probably just recache the data derived from the LOD instead of resetting everything.
				if (Mesh->GetRenderData()->GetCurrentFirstLODIdx(MinLOD) > CachedLODIdx)
				{
					return true;
				}
			}

			return false;
		}

		const FStaticMeshLODResources* GetCurrentFirstLOD()
		{
			//-OPT: Perhaps we could cache this during the tick function?
			UStaticMesh* StaticMesh = StaticMeshWeakPtr.Get();
			if ( bMeshValid && StaticMesh )
			{
				if ( const FStaticMeshLODResources* LODResource = StaticMesh->GetRenderData()->GetCurrentFirstLOD(MinLOD) )
				{
					return LODResource;
				}
			}
			return nullptr;
		}

		TConstArrayView<int32> GetFilteredSections() const { return MakeArrayView(FilteredAndUnfilteredSections.GetData(), NumFilteredSections); }
		TConstArrayView<int32> GetUnfilteredSections() const { return MakeArrayView(FilteredAndUnfilteredSections.GetData() + NumFilteredSections, NumUnfilteredSections); }

		TConstArrayView<FNDISectionInfo> GetFilteredSectionInfos() const { return MakeArrayView(FilteredAndUnfilteredSectionInfos.GetData(), NumFilteredSections); }
		TConstArrayView<FNDISectionInfo> GetUnfilteredSectionInfos() const { return MakeArrayView(FilteredAndUnfilteredSectionInfos.GetData() + NumFilteredSections, NumUnfilteredSections); }
	};

	struct FRenderProxy : public FNiagaraDataInterfaceProxy
	{
		FRenderProxy() {}

		virtual void ConsumePerInstanceDataFromGameThread(void* FromGameThreadData, const FNiagaraSystemInstanceID& InstanceID) override
		{
			FInstanceData_FromGameThread* FromGameThread = reinterpret_cast<FInstanceData_FromGameThread*>(FromGameThreadData);
			FInstanceData_RenderThread* InstanceData = &PerInstanceData_RT.FindChecked(InstanceID);

			InstanceData->Transform					= FromGameThread->Transform;
			InstanceData->PrevTransform				= FromGameThread->PrevTransform;
			InstanceData->Rotation					= FromGameThread->Rotation;
			InstanceData->PrevRotation				= FromGameThread->PrevRotation;
			InstanceData->DeltaSeconds				= FromGameThread->DeltaSeconds;
			InstanceData->DistanceFieldPrimitiveId	= FromGameThread->DistanceFieldPrimitiveId;

			FromGameThread->~FInstanceData_FromGameThread();
		}

		virtual int32 PerInstanceDataPassedToRenderThreadSize() const override
		{
			return sizeof(FInstanceData_FromGameThread);
		}

		TMap<FNiagaraSystemInstanceID, FInstanceData_RenderThread> PerInstanceData_RT;
	};

	//////////////////////////////////////////////////////////////////////////

	template<typename TTransformHandler = FNDITransformHandlerNoop>
	struct FStaticMeshCpuHelper
	{
		FORCEINLINE FStaticMeshCpuHelper(FVectorVMExternalFunctionContext& Context)
			: InstanceData(Context)
		{
			LODResource = InstanceData->GetCurrentFirstLOD();
		}


		FORCEINLINE float GetInvDeltaSeconds() const
		{
			return InstanceData->DeltaSeconds > 0.0f ? 1.0f / InstanceData->DeltaSeconds : 0.0f;
		}

		FORCEINLINE FVector3f TransformPosition(FVector3f Position) const
		{
			TransformHandler.TransformPosition(Position, FMatrix44f(InstanceData->Transform));					// LWC_TODO: Precision loss
			return Position;
		}

		FORCEINLINE FVector3f TransformVector(FVector3f Vector) const
		{
			TransformHandler.TransformVector(Vector, FMatrix44f(InstanceData->TransformInverseTransposed));		// LWC_TODO: Precision loss
			return Vector;
		}

		FORCEINLINE FQuat4f TransformRotation(FQuat4f Rotation) const
		{
			TransformHandler.TransformRotation(Rotation, InstanceData->Rotation);
			return Rotation;
		}

		FORCEINLINE FVector3f PreviousTransformPosition(FVector3f Position) const
		{
			TransformHandler.TransformPosition(Position, FMatrix44f(InstanceData->PrevTransform));				// LWC_TODO: Precision loss
			return Position;
		}

		FORCEINLINE FVector3f PreviousTransformVector(FVector3f Vector) const
		{
			TransformHandler.TransformVector(Vector, FMatrix44f(InstanceData->PrevTransformInverseTransposed));	// LWC_TODO: Precision loss
			return Vector;
		}

		FORCEINLINE FQuat4f PreviousTransformRotation(FQuat4f Rotation) const
		{
			TransformHandler.TransformRotation(Rotation, InstanceData->PrevRotation);
			return Rotation;
		}

		FORCEINLINE FIndexArrayView GetIndexArrayView() const
		{
			return LODResource ? LODResource->IndexBuffer.GetArrayView() : FIndexArrayView();
		}

		FORCEINLINE int32 GetNumPositionVertices() const
		{
			return LODResource && LODResource->VertexBuffers.PositionVertexBuffer.GetVertexData() && LODResource->VertexBuffers.PositionVertexBuffer.GetAllowCPUAccess() ? LODResource->VertexBuffers.PositionVertexBuffer.GetNumVertices() : 0;
		}

		FORCEINLINE int32 GetNumTangentVertices() const
		{
			return LODResource && LODResource->VertexBuffers.StaticMeshVertexBuffer.GetTangentData() && LODResource->VertexBuffers.StaticMeshVertexBuffer.GetAllowCPUAccess() ? LODResource->VertexBuffers.StaticMeshVertexBuffer.GetNumVertices() : 0;
		}

		uint32 GetNumUVs() const
		{
			return LODResource && LODResource->VertexBuffers.StaticMeshVertexBuffer.GetTexCoordData() && LODResource->VertexBuffers.StaticMeshVertexBuffer.GetAllowCPUAccess() ? LODResource->VertexBuffers.StaticMeshVertexBuffer.GetNumTexCoords() : 0;
		}

		uint32 GetNumUVVertices() const
		{
			return LODResource && LODResource->VertexBuffers.StaticMeshVertexBuffer.GetTexCoordData() && LODResource->VertexBuffers.StaticMeshVertexBuffer.GetAllowCPUAccess() ? LODResource->VertexBuffers.StaticMeshVertexBuffer.GetNumVertices() : 0;
		}

		FORCEINLINE int32 GetNumColorVertices() const
		{
			return LODResource && LODResource->VertexBuffers.ColorVertexBuffer.GetVertexData() && LODResource->VertexBuffers.ColorVertexBuffer.GetAllowCPUAccess() ? LODResource->VertexBuffers.ColorVertexBuffer.GetNumVertices() : 0;
		}

		FORCEINLINE FVector3f GetLocalTrianglePosition(const FVector3f& BaryCoord, int32 Index0, int32 Index1, int32 Index2) const
		{
			FVector3f Position;
			Position = LODResource->VertexBuffers.PositionVertexBuffer.VertexPosition(Index0) * BaryCoord.X;
			Position += LODResource->VertexBuffers.PositionVertexBuffer.VertexPosition(Index1) * BaryCoord.Y;
			Position += LODResource->VertexBuffers.PositionVertexBuffer.VertexPosition(Index2) * BaryCoord.Z;
			return Position;
		}

		FORCEINLINE FVector3f GetTrianglePosition(const FVector3f& BaryCoord, int32 Index0, int32 Index1, int32 Index2) const
		{
			FVector3f Position = GetLocalTrianglePosition(BaryCoord, Index0, Index1, Index2);
			TransformHandler.TransformPosition(Position, FMatrix44f(InstanceData->Transform));					// LWC_TODO: Precision loss?
			return Position;
		}

		FORCEINLINE FVector3f GetTriangleTangentX(const FVector3f& BaryCoord, int32 Index0, int32 Index1, int32 Index2) const
		{
			FVector3f Tangent;
			Tangent  = LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(Index0) * BaryCoord.X;
			Tangent += LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(Index1) * BaryCoord.Y;
			Tangent += LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(Index2) * BaryCoord.Z;
			TransformHandler.TransformVector(Tangent, FMatrix44f(InstanceData->Transform));						// LWC_TODO: Precision loss?
			return Tangent;
		}

		FORCEINLINE FVector3f GetTriangleTangentY(const FVector3f& BaryCoord, int32 Index0, int32 Index1, int32 Index2) const
		{
			FVector3f Tangent;
			Tangent  = LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentY(Index0) * BaryCoord.X;
			Tangent += LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentY(Index1) * BaryCoord.Y;
			Tangent += LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentY(Index2) * BaryCoord.Z;
			TransformHandler.TransformVector(Tangent, FMatrix44f(InstanceData->Transform));						// LWC_TODO: Precision loss?
			return Tangent;
		}

		FORCEINLINE FVector3f GetTriangleTangentZ(const FVector3f& BaryCoord, int32 Index0, int32 Index1, int32 Index2) const
		{
			FVector3f Tangent;
			Tangent  = LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Index0) * BaryCoord.X;
			Tangent += LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Index1) * BaryCoord.Y;
			Tangent += LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Index2) * BaryCoord.Z;
			TransformHandler.TransformVector(Tangent, FMatrix44f(InstanceData->Transform));						// LWC_TODO: Precision loss?
			return Tangent;
		}

		FORCEINLINE FLinearColor GetTriangleColor(const FVector3f& BaryCoord, int32 Index0, int32 Index1, int32 Index2) const
		{
			FLinearColor Color;
			Color  = FLinearColor(LODResource->VertexBuffers.ColorVertexBuffer.VertexColor(Index0)) * BaryCoord.X;
			Color += FLinearColor(LODResource->VertexBuffers.ColorVertexBuffer.VertexColor(Index1)) * BaryCoord.Y;
			Color += FLinearColor(LODResource->VertexBuffers.ColorVertexBuffer.VertexColor(Index2)) * BaryCoord.Z;
			return Color;
		}

		FORCEINLINE FVector2D GetTriangleUV(const FVector3f& BaryCoord, int32 Index0, int32 Index1, int32 Index2, int32 UVSet) const
		{
			FVector2f UV;
			UV = LODResource->VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Index0, UVSet) * BaryCoord.X;
			UV += LODResource->VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Index1, UVSet) * BaryCoord.Y;
			UV += LODResource->VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Index2, UVSet) * BaryCoord.Z;
			return FVector2D(UV);
		}

		FORCEINLINE FVector3f GetPosition(int32 Vertex) const
		{
			FVector3f Position = LODResource->VertexBuffers.PositionVertexBuffer.VertexPosition(Vertex);
			TransformHandler.TransformPosition(Position, FMatrix44f(InstanceData->Transform));					// LWC_TODO: Precision loss?
			return Position;
		}

		FORCEINLINE FVector3f GetLocalPosition(int32 Vertex) const
		{
			return LODResource->VertexBuffers.PositionVertexBuffer.VertexPosition(Vertex);
		}

		FORCEINLINE FVector3f GetTangentX(int32 Vertex) const
		{
			FVector3f TangentX = LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentX(Vertex);
			TransformHandler.TransformVector(TangentX, FMatrix44f(InstanceData->TransformInverseTransposed));		// LWC_TODO: Precision loss?
			return TangentX;
		}

		FORCEINLINE FVector3f GetTangentY(int32 Vertex) const
		{
			FVector3f TangentY = LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentY(Vertex);
			TransformHandler.TransformVector(TangentY, FMatrix44f(InstanceData->TransformInverseTransposed));		// LWC_TODO: Precision loss?
			return TangentY;
		}

		FORCEINLINE FVector3f GetTangentZ(int32 Vertex) const
		{
			FVector3f TangentZ = LODResource->VertexBuffers.StaticMeshVertexBuffer.VertexTangentZ(Vertex);
			TransformHandler.TransformVector(TangentZ, FMatrix44f(InstanceData->TransformInverseTransposed));		// LWC_TODO: Precision loss?
			return TangentZ;
		}

		FORCEINLINE FLinearColor GetColor(int32 Vertex) const
		{
			return LODResource->VertexBuffers.ColorVertexBuffer.VertexColor(Vertex);
		}

		FORCEINLINE FVector2D GetUV(int32 Vertex, int32 UVSet) const
		{
			return FVector2D(LODResource->VertexBuffers.StaticMeshVertexBuffer.GetVertexUV(Vertex, UVSet));
		}

		FORCEINLINE bool IsCpuUniformlyDistributedSampling() const
		{
			return LODResource && InstanceData->bIsCpuUniformlyDistributedSampling;
		}

		template<typename TRandomHelper>
		FORCEINLINE int32 RandomUniformTriangle(const TRandomHelper& RandHelper, int32 InstanceIndex) const
		{
			int32 Triangle = 0;
			const int32 Section = LODResource->AreaWeightedSampler.GetNumEntries() ? LODResource->AreaWeightedSampler.GetEntryIndex(RandHelper.Rand(InstanceIndex), RandHelper.Rand(InstanceIndex)) : 0;
			if ( LODResource->AreaWeightedSectionSamplers.IsValidIndex(Section) && LODResource->AreaWeightedSectionSamplers[Section].GetNumEntries() )
			{
				const int32 SectionTriangle = LODResource->AreaWeightedSectionSamplers[Section].GetEntryIndex(RandHelper.Rand(InstanceIndex), RandHelper.Rand(InstanceIndex));
				Triangle = (LODResource->Sections[Section].FirstIndex / 3) + SectionTriangle;
			}
			return Triangle;
		}

		template<typename TRandomHelper>
		FORCEINLINE int32 RandomUniformTriangle(const TRandomHelper& RandHelper, int32 InstanceIndex, const TConstArrayView<FNDISectionInfo>& SectionInfos, TConstArrayView<int32>& SectionRemap) const
		{
			int32 Triangle = 0;
			int32 SectionIndex = RandHelper.Rand(InstanceIndex) * SectionInfos.Num();
			SectionIndex = RandHelper.Rand(InstanceIndex) < SectionInfos[SectionIndex].Prob ? SectionIndex : SectionInfos[SectionIndex].Alias;
			const int32 Section = SectionRemap[SectionIndex];
			if ( LODResource->AreaWeightedSectionSamplers.IsValidIndex(Section) && LODResource->AreaWeightedSectionSamplers[Section].GetNumEntries() )
			{
				const int32 SectionTriangle = LODResource->AreaWeightedSectionSamplers[Section].GetEntryIndex(RandHelper.Rand(InstanceIndex), RandHelper.Rand(InstanceIndex));
				Triangle = SectionInfos[SectionIndex].FirstTriangle + SectionTriangle;
			}
			return Triangle;
		}

		template<typename TRandomHelper>
		FORCEINLINE int32 RandomTriangle(const TRandomHelper& RandHelper, int32 InstanceIndex, const TConstArrayView<FNDISectionInfo>& SectionInfos) const
		{
			const int32 Section = RandHelper.Rand(InstanceIndex) * SectionInfos.Num();
			const int32 Triangle = SectionInfos[Section].FirstTriangle + (RandHelper.Rand(InstanceIndex) * SectionInfos[Section].NumTriangles);
			return Triangle;
		}

		template<typename TRandomHelper>
		FORCEINLINE int32 RandomUniformSection(const TRandomHelper& RandHelper, int32 InstanceIndex) const
		{
			return LODResource->AreaWeightedSampler.GetEntryIndex(RandHelper.Rand(InstanceIndex), RandHelper.Rand(InstanceIndex));
		}

		template<typename TRandomHelper>
		FORCEINLINE int32 RandomUniformSection(const TRandomHelper& RandHelper, int32 InstanceIndex, const TConstArrayView<FNDISectionInfo>& SectionInfos, TConstArrayView<int32>& SectionRemap) const
		{
			int32 SectionIndex = RandHelper.Rand(InstanceIndex) * SectionInfos.Num();
			SectionIndex = RandHelper.Rand(InstanceIndex) < SectionInfos[SectionIndex].Prob ? SectionIndex : SectionInfos[SectionIndex].Alias;
			return SectionRemap[SectionIndex];
		}

		template<typename TRandomHelper>
		FORCEINLINE int32 RandomSectionUniformTriangle(const TRandomHelper& RandHelper, int32 InstanceIndex, int32 Section) const
		{
			int32 Triangle = 0;
			if (LODResource->AreaWeightedSectionSamplers.IsValidIndex(Section) && LODResource->AreaWeightedSectionSamplers[Section].GetNumEntries())
			{
				const int32 SectionTriangle = LODResource->AreaWeightedSectionSamplers[Section].GetEntryIndex(RandHelper.Rand(InstanceIndex), RandHelper.Rand(InstanceIndex));
				Triangle = (LODResource->Sections[Section].FirstIndex / 3) + SectionTriangle;
			}
			return Triangle;
		}

		template<typename TRandomHelper>
		FORCEINLINE int32 RandomSectionTriangle(const TRandomHelper& RandHelper, int32 InstanceIndex, int32 Section) const
		{
			const int32 Triangle = (LODResource->Sections[Section].FirstIndex / 3) + (RandHelper.Rand(InstanceIndex) * LODResource->Sections[Section].NumTriangles);
			return Triangle;
		}

		FORCEINLINE int32 GetNumTriangles() const
		{
			return LODResource ? LODResource->IndexBuffer.GetArrayView().Num() / 3 : 0;
		}

		FORCEINLINE int32 GetNumFilteredTriangles() const
		{
			return LODResource ? InstanceData->NumFilteredTriangles : 0;
		}

		FORCEINLINE int32 GetNumUnfilteredTriangles() const
		{
			return LODResource ? InstanceData->NumUnfilteredTriangles : 0;
		}

		FORCEINLINE int32 SectionTriangleToTriangle(const TConstArrayView<FNDISectionInfo>& SectionInfos, int32 Index)
		{
			int32 SectionFirstTriangle = 0;
			for ( const FNDISectionInfo& SectionInfo : SectionInfos )
			{
				const int32 SectionLastTriangle = SectionFirstTriangle + SectionInfo.NumTriangles;
				if ( (Index >= SectionFirstTriangle) && (Index < SectionLastTriangle) )
				{
					return (Index - SectionFirstTriangle) + SectionInfo.FirstTriangle;
				}
				SectionFirstTriangle = SectionLastTriangle;
			}
			return 0;
		}

		VectorVM::FUserPtrHandler<FInstanceData_GameThread> InstanceData;
		TRefCountPtr<const FStaticMeshLODResources> LODResource;
		TTransformHandler TransformHandler;
	};
}

//////////////////////////////////////////////////////////////////////////

struct FNiagaraDataInterfaceParametersCS_StaticMesh : public FNiagaraDataInterfaceParametersCS
{
	DECLARE_TYPE_LAYOUT(FNiagaraDataInterfaceParametersCS_StaticMesh, NonVirtual);

public:
	void Bind(const FNiagaraDataInterfaceGPUParamInfo& ParameterInfo, const class FShaderParameterMap& ParameterMap)
	{
		using namespace NDIStaticMeshLocal;

		NumTrianglesParam.Bind(ParameterMap, *(NumTriangles_String + ParameterInfo.DataInterfaceHLSLSymbol));
		NumVerticesParam.Bind(ParameterMap, *(NumVertices_String + ParameterInfo.DataInterfaceHLSLSymbol));
		NumUVsParam.Bind(ParameterMap, *(NumUVs_String + ParameterInfo.DataInterfaceHLSLSymbol));
		HasColorsParam.Bind(ParameterMap, *(HasColors_String + ParameterInfo.DataInterfaceHLSLSymbol));
		IndexBufferParam.Bind(ParameterMap, *(IndexBuffer_String + ParameterInfo.DataInterfaceHLSLSymbol));
		PositionBufferParam.Bind(ParameterMap, *(PositionBuffer_String + ParameterInfo.DataInterfaceHLSLSymbol));
		TangentBufferParam.Bind(ParameterMap, *(TangentBuffer_String + ParameterInfo.DataInterfaceHLSLSymbol));
		UVBufferParam.Bind(ParameterMap, *(UVBuffer_String + ParameterInfo.DataInterfaceHLSLSymbol));
		ColorBufferParam.Bind(ParameterMap, *(ColorBuffer_String + ParameterInfo.DataInterfaceHLSLSymbol));

		HasUniformSamplingParam.Bind(ParameterMap, *(HasUniformSampling_String + ParameterInfo.DataInterfaceHLSLSymbol));
		UniformSamplingTrianglesParam.Bind(ParameterMap, *(UniformSamplingTriangles_String + ParameterInfo.DataInterfaceHLSLSymbol));

		SectionCountsParam.Bind(ParameterMap, *(SectionCounts_String + ParameterInfo.DataInterfaceHLSLSymbol));
		SectionInfosParam.Bind(ParameterMap, *(SectionInfos_String + ParameterInfo.DataInterfaceHLSLSymbol));
		FilteredAndUnfilteredSectionsParam.Bind(ParameterMap, *(FilteredAndUnfilteredSections_String + ParameterInfo.DataInterfaceHLSLSymbol));

		SocketCountsParam.Bind(ParameterMap, *(SocketCounts_String + ParameterInfo.DataInterfaceHLSLSymbol));
		SocketTransformsParam.Bind(ParameterMap, *(SocketTransforms_String + ParameterInfo.DataInterfaceHLSLSymbol));
		FilteredAndUnfilteredSocketsParam.Bind(ParameterMap, *(FilteredAndUnfilteredSockets_String + ParameterInfo.DataInterfaceHLSLSymbol));

		InvDeltaSecondsParam.Bind(ParameterMap, *(InvDeltaSeconds_String + ParameterInfo.DataInterfaceHLSLSymbol));
		InstanceTransformParam.Bind(ParameterMap, *(InstanceTransform_String + ParameterInfo.DataInterfaceHLSLSymbol));
		InstanceTransformInverseTransposedParam.Bind(ParameterMap, *(InstanceTransformInverseTransposed_String + ParameterInfo.DataInterfaceHLSLSymbol));
		InstanceRotationParam.Bind(ParameterMap, *(InstanceRotation_String + ParameterInfo.DataInterfaceHLSLSymbol));
		InstancePreviousTransformParam.Bind(ParameterMap, *(InstancePreviousTransform_String + ParameterInfo.DataInterfaceHLSLSymbol));
		InstancePreviousTransformInverseTransposedParam.Bind(ParameterMap, *(InstancePreviousTransformInverseTransposed_String + ParameterInfo.DataInterfaceHLSLSymbol));
		InstancePreviousRotationParam.Bind(ParameterMap, *(InstancePreviousRotation_String + ParameterInfo.DataInterfaceHLSLSymbol));
		InstanceWorldVelocityParam.Bind(ParameterMap, *(InstanceWorldVelocity_String + ParameterInfo.DataInterfaceHLSLSymbol));

		InstanceDistanceFieldIndexParam.Bind(ParameterMap, *(InstanceDistanceFieldIndex_String + ParameterInfo.DataInterfaceHLSLSymbol));
		DistanceFieldParameters.Bind(ParameterMap);
	}

	void Set(FRHICommandList& RHICmdList, const FNiagaraDataInterfaceSetArgs& Context) const
	{
		check(IsInRenderingThread());

		FRHIComputeShader* ComputeShaderRHI = Context.Shader.GetComputeShader();

		NDIStaticMeshLocal::FRenderProxy* InterfaceProxy = static_cast<NDIStaticMeshLocal::FRenderProxy*>(Context.DataInterface);
		const NDIStaticMeshLocal::FInstanceData_RenderThread& InstanceData = InterfaceProxy->PerInstanceData_RT.FindChecked(Context.SystemInstanceID);

		// Set mesh sampling data
		if ( InstanceData.bIsValid )
		{
			const bool bHasColors = InstanceData.MeshColorBufferSRV.IsValid();
			SetShaderValue(RHICmdList, ComputeShaderRHI, NumTrianglesParam, InstanceData.NumTriangles);
			SetShaderValue(RHICmdList, ComputeShaderRHI, NumVerticesParam, InstanceData.NumVertices);
			SetShaderValue(RHICmdList, ComputeShaderRHI, NumUVsParam, InstanceData.NumUVs);
			SetShaderValue(RHICmdList, ComputeShaderRHI, HasColorsParam, bHasColors ? 1 : 0);
			SetSRVParameter(RHICmdList, ComputeShaderRHI, IndexBufferParam, InstanceData.MeshIndexBufferSRV);
			SetSRVParameter(RHICmdList, ComputeShaderRHI, PositionBufferParam, InstanceData.MeshPositionBufferSRV);
			SetSRVParameter(RHICmdList, ComputeShaderRHI, TangentBufferParam, InstanceData.MeshTangentBufferSRV);
			SetSRVParameter(RHICmdList, ComputeShaderRHI, UVBufferParam, InstanceData.MeshUVBufferSRV.IsValid() ? InstanceData.MeshUVBufferSRV.GetReference() : FNiagaraRenderer::GetDummyFloat2Buffer());
			SetSRVParameter(RHICmdList, ComputeShaderRHI, ColorBufferParam, bHasColors ? InstanceData.MeshColorBufferSRV.GetReference() : FNiagaraRenderer::GetDummyWhiteColorBuffer());

			SetShaderValue(RHICmdList, ComputeShaderRHI, HasUniformSamplingParam, InstanceData.bGpuUniformDistribution ? 1 : 0);
			SetSRVParameter(RHICmdList, ComputeShaderRHI, UniformSamplingTrianglesParam, InstanceData.bGpuUniformDistribution ? InstanceData.MeshUniformSamplingTriangleSRV.GetReference() : FNiagaraRenderer::GetDummyUInt2Buffer());
		}
		else
		{
			SetShaderValue(RHICmdList, ComputeShaderRHI, NumTrianglesParam, 0);
			SetShaderValue(RHICmdList, ComputeShaderRHI, NumVerticesParam, 0);
			SetShaderValue(RHICmdList, ComputeShaderRHI, NumUVsParam, 0);
			SetShaderValue(RHICmdList, ComputeShaderRHI, HasColorsParam, 0);
			SetSRVParameter(RHICmdList, ComputeShaderRHI, IndexBufferParam, FNiagaraRenderer::GetDummyUIntBuffer());
			SetSRVParameter(RHICmdList, ComputeShaderRHI, PositionBufferParam, FNiagaraRenderer::GetDummyFloatBuffer());
			SetSRVParameter(RHICmdList, ComputeShaderRHI, TangentBufferParam, FNiagaraRenderer::GetDummyFloat4Buffer());
			SetSRVParameter(RHICmdList, ComputeShaderRHI, UVBufferParam, FNiagaraRenderer::GetDummyFloat2Buffer());
			SetSRVParameter(RHICmdList, ComputeShaderRHI, ColorBufferParam, FNiagaraRenderer::GetDummyWhiteColorBuffer());

			SetShaderValue(RHICmdList, ComputeShaderRHI, HasUniformSamplingParam, 0);
			SetSRVParameter(RHICmdList, ComputeShaderRHI, UniformSamplingTrianglesParam, FNiagaraRenderer::GetDummyUInt2Buffer());
		}

		// Section information
		SetShaderValue(RHICmdList, ComputeShaderRHI, SectionCountsParam, InstanceData.SectionCounts);
		SetSRVParameter(RHICmdList, ComputeShaderRHI, SectionInfosParam, InstanceData.SectionInfos.SRV.IsValid() ? InstanceData.SectionInfos.SRV.GetReference() : FNiagaraRenderer::GetDummyUInt4Buffer());
		SetSRVParameter(RHICmdList, ComputeShaderRHI, FilteredAndUnfilteredSectionsParam, InstanceData.FilteredAndUnfilteredSections.SRV.IsValid() ? InstanceData.FilteredAndUnfilteredSections.SRV.GetReference() : FNiagaraRenderer::GetDummyUIntBuffer());

		// Set socket sampling information
		SetShaderValue(RHICmdList, ComputeShaderRHI, SocketCountsParam, InstanceData.SocketCounts);
		SetSRVParameter(RHICmdList, ComputeShaderRHI, SocketTransformsParam, InstanceData.SocketTransforms.SRV.IsValid() ? InstanceData.SocketTransforms.SRV.GetReference() : FNiagaraRenderer::GetDummyFloat4Buffer());
		SetSRVParameter(RHICmdList, ComputeShaderRHI, FilteredAndUnfilteredSocketsParam, InstanceData.FilteredAndUnfilteredSockets.SRV.IsValid() ? InstanceData.FilteredAndUnfilteredSockets.SRV.GetReference() : FNiagaraRenderer::GetDummyUIntBuffer());

		// Set misc data
		const float InvDeltaTime = InstanceData.DeltaSeconds > 0.0f ? 1.0f / InstanceData.DeltaSeconds : 0.0f;
		const FVector3f DeltaPosition = InstanceData.Transform.GetOrigin() - InstanceData.PrevTransform.GetOrigin();

		SetShaderValue(RHICmdList, ComputeShaderRHI, InvDeltaSecondsParam, InvDeltaTime);
		SetShaderValue(RHICmdList, ComputeShaderRHI, InstanceTransformParam, InstanceData.Transform);
		SetShaderValue(RHICmdList, ComputeShaderRHI, InstanceTransformInverseTransposedParam, InstanceData.Transform.Inverse().GetTransposed());
		SetShaderValue(RHICmdList, ComputeShaderRHI, InstanceRotationParam, InstanceData.Rotation);
		SetShaderValue(RHICmdList, ComputeShaderRHI, InstancePreviousTransformParam, InstanceData.PrevTransform);
		SetShaderValue(RHICmdList, ComputeShaderRHI, InstancePreviousTransformInverseTransposedParam, InstanceData.PrevTransform.Inverse().GetTransposed());
		SetShaderValue(RHICmdList, ComputeShaderRHI, InstancePreviousRotationParam, InstanceData.PrevRotation);
		SetShaderValue(RHICmdList, ComputeShaderRHI, InstanceWorldVelocityParam, DeltaPosition);

		const FDistanceFieldSceneData* DistanceFieldSceneData = static_cast<const FNiagaraGpuComputeDispatch*>(Context.ComputeDispatchInterface)->GetMeshDistanceFieldParameters();	//-BATCHERTODO:

		if ( InstanceDistanceFieldIndexParam.IsBound() )
		{
			int32 DistanceFieldIndex = -1;
			if ( DistanceFieldSceneData != nullptr && InstanceData.DistanceFieldPrimitiveId.IsValid() )
			{
				if ( FScene* Scene = Context.ComputeDispatchInterface->GetScene() )
				{
					// Kind of gross, but currently no way to reference other primitive scene infos
					const int32 PrimitiveSceneIndex = Scene->PrimitiveComponentIds.Find(InstanceData.DistanceFieldPrimitiveId);
					if (PrimitiveSceneIndex != INDEX_NONE)
					{
						const TArray<int32, TInlineAllocator<1>>& DFIndices = Scene->Primitives[PrimitiveSceneIndex]->DistanceFieldInstanceIndices;
						DistanceFieldIndex = DFIndices.Num() > 0 ? DFIndices[0] : -1;
					}
				}
			}
			SetShaderValue(RHICmdList, ComputeShaderRHI, InstanceDistanceFieldIndexParam, DistanceFieldIndex);
		}

		if (DistanceFieldParameters.IsBound())
		{
			if (ensure(DistanceFieldSceneData != nullptr))
			{
				DistanceFieldParameters.Set(RHICmdList, ComputeShaderRHI, DistanceFieldSceneData);
			}
			else 
			{
				//-TODO: We can't create dummy buffers here due to dll boundaries, we will skip the distance field calcs since DistanceFieldIndex will be set to -1
				//       However some platforms may still complain due to lack of dummy data bound
				//FDistanceFieldSceneData DummyDistanceFieldSceneData(Context.ComputeDispatchInterface->GetShaderPlatform());
				//DistanceFieldParameters.Set(RHICmdList, ComputeShaderRHI, &DummyDistanceFieldSceneData);
			}
		}
	}

private:
	LAYOUT_FIELD(FShaderParameter, NumTrianglesParam);
	LAYOUT_FIELD(FShaderParameter, NumVerticesParam);
	LAYOUT_FIELD(FShaderParameter, NumUVsParam);
	LAYOUT_FIELD(FShaderParameter, HasColorsParam);
	LAYOUT_FIELD(FShaderResourceParameter, IndexBufferParam);
	LAYOUT_FIELD(FShaderResourceParameter, PositionBufferParam);
	LAYOUT_FIELD(FShaderResourceParameter, TangentBufferParam);
	LAYOUT_FIELD(FShaderResourceParameter, UVBufferParam);
	LAYOUT_FIELD(FShaderResourceParameter, ColorBufferParam);

	LAYOUT_FIELD(FShaderParameter, HasUniformSamplingParam);
	LAYOUT_FIELD(FShaderResourceParameter, UniformSamplingTrianglesParam);

	LAYOUT_FIELD(FShaderParameter,  SectionCountsParam);
	LAYOUT_FIELD(FShaderResourceParameter, SectionInfosParam);
	LAYOUT_FIELD(FShaderResourceParameter, FilteredAndUnfilteredSectionsParam);

	LAYOUT_FIELD(FShaderParameter, SocketCountsParam);
	LAYOUT_FIELD(FShaderResourceParameter, SocketTransformsParam);
	LAYOUT_FIELD(FShaderResourceParameter, FilteredAndUnfilteredSocketsParam);

	LAYOUT_FIELD(FShaderParameter, InvDeltaSecondsParam);
	LAYOUT_FIELD(FShaderParameter, InstanceTransformParam);
	LAYOUT_FIELD(FShaderParameter, InstanceTransformInverseTransposedParam);
	LAYOUT_FIELD(FShaderParameter, InstanceRotationParam);
	LAYOUT_FIELD(FShaderParameter, InstancePreviousTransformParam);
	LAYOUT_FIELD(FShaderParameter, InstancePreviousTransformInverseTransposedParam);
	LAYOUT_FIELD(FShaderParameter, InstancePreviousRotationParam);
	LAYOUT_FIELD(FShaderParameter, InstanceWorldVelocityParam);

	LAYOUT_FIELD(FShaderParameter, InstanceDistanceFieldIndexParam);
	LAYOUT_FIELD(FDistanceFieldParameters, DistanceFieldParameters);	
};

IMPLEMENT_TYPE_LAYOUT(FNiagaraDataInterfaceParametersCS_StaticMesh);
IMPLEMENT_NIAGARA_DI_PARAMETER(UNiagaraDataInterfaceStaticMesh, FNiagaraDataInterfaceParametersCS_StaticMesh);

//////////////////////////////////////////////////////////////////////////

UNiagaraDataInterfaceStaticMesh::UNiagaraDataInterfaceStaticMesh(FObjectInitializer const& ObjectInitializer)
	: Super(ObjectInitializer)
{
	Proxy.Reset(new NDIStaticMeshLocal::FRenderProxy());
}

void UNiagaraDataInterfaceStaticMesh::PostInitProperties()
{
	Super::PostInitProperties();

	// Can we register data interfaces as regular types and fold them into the FNiagaraVariable framework for UI and function calls etc?
	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		ENiagaraTypeRegistryFlags DIFlags =
			ENiagaraTypeRegistryFlags::AllowAnyVariable |
			ENiagaraTypeRegistryFlags::AllowParameter;
		FNiagaraTypeRegistry::Register(FNiagaraTypeDefinition(GetClass()), DIFlags);

		// Still some issues with using custom structs. Convert node for example throws a wobbler. TODO after GDC.
		ENiagaraTypeRegistryFlags CoordFlags =
			ENiagaraTypeRegistryFlags::AllowAnyVariable |
			ENiagaraTypeRegistryFlags::AllowParameter;
		FNiagaraTypeRegistry::Register(FMeshTriCoordinate::StaticStruct(), CoordFlags);
	}
}

#if WITH_EDITOR
void UNiagaraDataInterfaceStaticMesh::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	++ChangeId;

	if (PropertyChangedEvent.Property &&
		PropertyChangedEvent.Property->GetFName() == GET_MEMBER_NAME_CHECKED(UNiagaraDataInterfaceStaticMesh, SourceMode))
	{
		if (SourceMode != ENDIStaticMesh_SourceMode::Default && SourceMode != ENDIStaticMesh_SourceMode::Source)
		{
			// Ensure we don't have any reference to a source actor that we'll never use
			Source = nullptr;
			SourceComponent = nullptr;
		}
	
		if (SourceMode != ENDIStaticMesh_SourceMode::Default && SourceMode != ENDIStaticMesh_SourceMode::DefaultMeshOnly)
		{
			// Ensure we don't cook in a default mesh we'll never use
			DefaultMesh = nullptr;
		}
	}
}

bool UNiagaraDataInterfaceStaticMesh::CanEditChange(const FProperty* InProperty) const
{
	if (!Super::CanEditChange(InProperty))
	{
		return false;
	}

	if (InProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UNiagaraDataInterfaceStaticMesh, Source) &&
		SourceMode != ENDIStaticMesh_SourceMode::Default &&
		SourceMode != ENDIStaticMesh_SourceMode::Source)
	{
		// Disable Source if we'll never use it
		return false;
	}

	if (InProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UNiagaraDataInterfaceStaticMesh, DefaultMesh) &&
		SourceMode != ENDIStaticMesh_SourceMode::Default &&
		SourceMode != ENDIStaticMesh_SourceMode::DefaultMeshOnly)
	{
		// Disable Default Mesh if we'll never use it
		return false;
	}

	return true;
}
#endif

bool UNiagaraDataInterfaceStaticMesh::InitPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	NDIStaticMeshLocal::FInstanceData_GameThread* InstanceData = new(PerInstanceData) NDIStaticMeshLocal::FInstanceData_GameThread();
	const bool bIsValid = InstanceData->Init(this, SystemInstance);

	// Create render thread data?
	if (bIsValid && IsUsedWithGPUEmitter() )
	{
		TUniquePtr<NDIStaticMeshLocal::FGpuInitializeData> GpuInitializeData = MakeUnique<NDIStaticMeshLocal::FGpuInitializeData>();
		GpuInitializeData->RenderProxy = GetProxyAs<NDIStaticMeshLocal::FRenderProxy>();
		GpuInitializeData->SystemInstanceID = SystemInstance->GetId();
		GpuInitializeData->LODResource = InstanceData->GetCurrentFirstLOD();
		if ( GpuInitializeData->LODResource )
		{
			GpuInitializeData->NumFilteredTriangles = InstanceData->NumFilteredTriangles;
			GpuInitializeData->NumUnfilteredTriangles = InstanceData->NumUnfilteredTriangles;

			GpuInitializeData->bGpuUniformDistribution = InstanceData->bIsGpuUniformlyDistributedSampling;

			// Build section data to pass
			GpuInitializeData->NumSections = GpuInitializeData->LODResource->Sections.Num();
			if (GpuInitializeData->NumSections > 0)
			{
				const FStaticMeshAreaWeightedSectionSampler* SectionSamplers = GpuInitializeData->bGpuUniformDistribution ? &GpuInitializeData->LODResource->AreaWeightedSampler : nullptr;
				for (int i = 0; i < GpuInitializeData->NumSections; ++i)
				{
					const FStaticMeshSection& MeshSection = GpuInitializeData->LODResource->Sections[i];
					const float Prob = SectionSamplers ? SectionSamplers->GetProb()[i] : 1.0f;
					const int32 Alias = SectionSamplers ? SectionSamplers->GetAlias()[i] : i;
					GpuInitializeData->SectionInfos.Emplace(MeshSection.FirstIndex / 3, MeshSection.NumTriangles, reinterpret_cast<const int32&>(Prob), Alias);
				}

				GpuInitializeData->NumFilteredSections = InstanceData->NumFilteredSections;
				for (const NDIStaticMeshLocal::FNDISectionInfo& SectionInfo : InstanceData->GetFilteredSectionInfos())
				{
					GpuInitializeData->SectionInfos.Emplace(SectionInfo.FirstTriangle, SectionInfo.NumTriangles, reinterpret_cast<const int32&>(SectionInfo.Prob), SectionInfo.Alias);
				}
				for (int Section : InstanceData->GetFilteredSections())
				{
					GpuInitializeData->FilteredAndUnfilteredSections.Add(Section);
				}

				GpuInitializeData->NumUnfilteredSections = InstanceData->NumUnfilteredSections;
				for (const NDIStaticMeshLocal::FNDISectionInfo& SectionInfo : InstanceData->GetUnfilteredSectionInfos())
				{
					GpuInitializeData->SectionInfos.Emplace(SectionInfo.FirstTriangle, SectionInfo.NumTriangles, reinterpret_cast<const int32&>(SectionInfo.Prob), SectionInfo.Alias);
				}
				for (int Section : InstanceData->GetUnfilteredSections())
				{
					GpuInitializeData->FilteredAndUnfilteredSections.Add(Section);
				}
			}
		}

		// Build socket data to pass
		GpuInitializeData->NumSockets = InstanceData->CachedSockets.Num();
		if (GpuInitializeData->NumSockets > 0)
		{
			GpuInitializeData->NumFilteredSockets = InstanceData->NumFilteredSockets;
			GpuInitializeData->NumUnfilteredSockets = GpuInitializeData->NumSockets - InstanceData->NumFilteredSockets;

			GpuInitializeData->SocketTransforms.Reserve(GpuInitializeData->NumFilteredSockets * 3);
			for (const FTransform3f& SocketTransform : InstanceData->CachedSockets)
			{
				const FQuat4f SocketRotation = SocketTransform.GetRotation();
				GpuInitializeData->SocketTransforms.Emplace(SocketTransform.GetTranslation(), 0.0f);
				GpuInitializeData->SocketTransforms.Emplace(SocketRotation.X, SocketRotation.Y, SocketRotation.Z, SocketRotation.W);
				GpuInitializeData->SocketTransforms.Emplace(SocketTransform.GetScale3D(), 0.0f);
			}

			if (InstanceData->NumFilteredSockets > 0)
			{
				GpuInitializeData->FilteredAndUnfilteredSockets.Reserve(InstanceData->FilteredAndUnfilteredSockets.Num());
				for (uint16 v : InstanceData->FilteredAndUnfilteredSockets)
				{
					GpuInitializeData->FilteredAndUnfilteredSockets.Add(v);
				}
			}
		}

		// Initialize the render thread side data
		ENQUEUE_RENDER_COMMAND(InitInstanceData)(
			[GpuInitializeData_RT=MoveTemp(GpuInitializeData)](FRHICommandListImmediate& CmdList)
			{
				NDIStaticMeshLocal::FInstanceData_RenderThread& InstanceData_RT = GpuInitializeData_RT->RenderProxy->PerInstanceData_RT.Add(GpuInitializeData_RT->SystemInstanceID);
				InstanceData_RT.Init(*(GpuInitializeData_RT.Get()));
			}
		);
	}

	return bIsValid;
}

void UNiagaraDataInterfaceStaticMesh::DestroyPerInstanceData(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance)
{
	NDIStaticMeshLocal::FInstanceData_GameThread* InstanceData = reinterpret_cast<NDIStaticMeshLocal::FInstanceData_GameThread*>(PerInstanceData);
	InstanceData->Release();
	InstanceData->~FInstanceData_GameThread();

	if ( IsUsedWithGPUEmitter() )
	{
		NDIStaticMeshLocal::FRenderProxy* Proxy_RT = GetProxyAs<NDIStaticMeshLocal::FRenderProxy>();
		FNiagaraSystemInstanceID InstanceID_RT = SystemInstance->GetId();
		ENQUEUE_RENDER_COMMAND(RemoveInstanceData)(
			[Proxy_RT, InstanceID_RT](FRHICommandListImmediate& CmdList)
			{
				Proxy_RT->PerInstanceData_RT.Remove(InstanceID_RT);
			}
		);
	}
}

bool UNiagaraDataInterfaceStaticMesh::PerInstanceTick(void* PerInstanceData, FNiagaraSystemInstance* SystemInstance, float DeltaSeconds)
{
	using namespace NDIStaticMeshLocal;

	FInstanceData_GameThread* InstanceData = reinterpret_cast<FInstanceData_GameThread*>(PerInstanceData);
	return InstanceData->Tick(this, SystemInstance, DeltaSeconds);
}

int32 UNiagaraDataInterfaceStaticMesh::PerInstanceDataSize() const
{
	return sizeof(NDIStaticMeshLocal::FInstanceData_GameThread);
}

void UNiagaraDataInterfaceStaticMesh::ProvidePerInstanceDataForRenderThread(void* InDataFromGT, void* InInstanceData, const FNiagaraSystemInstanceID& SystemInstance)
{
	NDIStaticMeshLocal::FInstanceData_GameThread* InstanceData = static_cast<NDIStaticMeshLocal::FInstanceData_GameThread*>(InInstanceData);
	NDIStaticMeshLocal::FInstanceData_FromGameThread* DataFromGT = static_cast<NDIStaticMeshLocal::FInstanceData_FromGameThread*>(InDataFromGT);

	DataFromGT->Transform					= FMatrix44f(InstanceData->Transform);			// LWC_TODO: Precision loss
	DataFromGT->PrevTransform				= FMatrix44f(InstanceData->PrevTransform);
	DataFromGT->Rotation					= InstanceData->Rotation;
	DataFromGT->PrevRotation				= InstanceData->PrevRotation;
	DataFromGT->DeltaSeconds				= InstanceData->DeltaSeconds;
	DataFromGT->DistanceFieldPrimitiveId	= FPrimitiveComponentId();
	if ( UPrimitiveComponent* PrimitiveComponent = Cast<UPrimitiveComponent>(InstanceData->SceneComponentWeakPtr) )
	{
		DataFromGT->DistanceFieldPrimitiveId = PrimitiveComponent->ComponentId;
	}
}

void UNiagaraDataInterfaceStaticMesh::GetFunctions(TArray<FNiagaraFunctionSignature>& OutFunctions)
{
	using namespace NDIStaticMeshLocal;

	//OutFunctions.Reserve(OutFunctions.Num() + NumFunctions);

	// Setup base signature
	FNiagaraFunctionSignature BaseSignature;
	BaseSignature.Inputs.Emplace(FNiagaraTypeDefinition(GetClass()), TEXT("StaticMesh"));
	BaseSignature.bMemberFunction = true;
	BaseSignature.bRequiresContext = false;
#if WITH_EDITORONLY_DATA
	BaseSignature.FunctionVersion = EDIFunctionVersion::LatestVersion;
#endif

	//////////////////////////////////////////////////////////////////////////
	// VM Vertex Sampling
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = IsValidVertexName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Vertex"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Valid"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = RandomVertexName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetRandInfoDef(), TEXT("RandomInfo"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Vertex"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetVertexCountName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Count"));
	}

	//{
	//	FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
	//	Sig.Name = IsValidFilteredVertexName;
	//	Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Vertex"));
	//	Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Valid"));
	//}
	//{
	//	FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
	//	Sig.Name = RandomFilteredVertexName;
	//	Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Vertex"));
	//}
	//{
	//	FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
	//	Sig.Name = GetFilteredVertexCountName;
	//	Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Count"));
	//}
	//{
	//	FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
	//	Sig.Name = GetFilteredVertexAtName;
	//	Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
	//	Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Vertex"));
	//}

	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Vertex"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetPositionDef(), TEXT("Position"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Velocity"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Normal"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Bitangent"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Tangent"));
		OutFunctions.Add_GetRef(Sig).Name = GetVertexName;
		
		FNiagaraFunctionSignature& WsSig = OutFunctions.Add_GetRef(Sig);
		WsSig.Name = GetVertexWSName;
		WsSig.Outputs[0].SetType(FNiagaraTypeDefinition::GetPositionDef());
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetVertexColorName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Vertex"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetColorDef(), TEXT("Color"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetVertexUVName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Vertex"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("UV Set"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec2Def(), TEXT("UV"));
	}

	//////////////////////////////////////////////////////////////////////////
	// VM Triangle Sampling
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = IsValidTriangleName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Valid"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = RandomTriangleName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetRandInfoDef(), TEXT("RandomInfo"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("BaryCoord"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetTriangleCountName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Count"));
	}

	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = IsValidFilteredTriangleName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Valid"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = RandomFilteredTriangleName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetRandInfoDef(), TEXT("RandomInfo"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("BaryCoord"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetFilteredTriangleCountName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Count"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetFilteredTriangleAtName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
	}

	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = IsValidUnfilteredTriangleName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Valid"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = RandomUnfilteredTriangleName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetRandInfoDef(), TEXT("RandomInfo"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("BaryCoord"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetUnfilteredTriangleCountName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Count"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetUnfilteredTriangleAtName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
	}

	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
		Sig.Inputs.Emplace_GetRef(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("BaryCoord"))).SetValue(FVector3f(1.0f / 3.0f));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetPositionDef(), TEXT("Position"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Velocity"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Normal"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Bitangent"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Tangent"));
		OutFunctions.Add_GetRef(Sig).Name = GetTriangleName;

		FNiagaraFunctionSignature& WsSig = OutFunctions.Add_GetRef(Sig);
		WsSig.Name = GetTriangleWSName;
		WsSig.Outputs[0].SetType(FNiagaraTypeDefinition::GetPositionDef());
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetTriangleColorName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
		Sig.Inputs.Emplace_GetRef(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("BaryCoord"))).SetValue(FVector3f(1.0f / 3.0f));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetColorDef(), TEXT("Color"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetTriangleUVName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
		Sig.Inputs.Emplace_GetRef(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("BaryCoord"))).SetValue(FVector3f(1.0f / 3.0f));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("UV Set"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec2Def(), TEXT("UV"));
	}

	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetTriangleIndicesName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index0"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index1"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index2"));
	}

	//////////////////////////////////////////////////////////////////////////
	// Socket Functions
	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Num")));

		OutFunctions.Add_GetRef(Sig).Name = GetSocketCountName;
		OutFunctions.Add_GetRef(Sig).Name = GetFilteredSocketCountName;
		OutFunctions.Add_GetRef(Sig).Name = GetUnfilteredSocketCountName;
	}
	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.Inputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetPositionDef(), TEXT("Position")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetQuatDef(), TEXT("Rotation")));
		Sig.Outputs.Add(FNiagaraVariable(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Scale")));

		FNiagaraFunctionSignature WsSig = Sig;
		WsSig.Outputs[0].SetType(FNiagaraTypeDefinition::GetPositionDef());

		OutFunctions.Add_GetRef(Sig).Name = GetSocketTransformName;
		OutFunctions.Add_GetRef(WsSig).Name = GetSocketTransformWSName;
		OutFunctions.Add_GetRef(Sig).Name = GetFilteredSocketTransformName;
		OutFunctions.Add_GetRef(WsSig).Name = GetFilteredSocketTransformWSName;
		OutFunctions.Add_GetRef(Sig).Name = GetUnfilteredSocketTransformName;
		OutFunctions.Add_GetRef(WsSig).Name = GetUnfilteredSocketTransformWSName;
	}

	////////////////////////////////////////////////////////////////////////////
	// Section functions
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = IsValidSectionName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Valid"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetSectionTriangleCountName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Section"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Count"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = RandomSectionTriangleName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetRandInfoDef(), TEXT("RandomInfo"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Section"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("BaryCoord"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetSectionTriangleAtName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Section"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Index"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Triangle"));
	}

	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Section"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Count"));
		OutFunctions.Add_GetRef(Sig).Name = GetFilteredSectionAtName;
		OutFunctions.Add_GetRef(Sig).Name = GetUnfilteredSectionAtName;
	}

	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Count"));
		OutFunctions.Add_GetRef(Sig).Name = GetSectionCountName;
		OutFunctions.Add_GetRef(Sig).Name = GetFilteredSectionCountName;
		OutFunctions.Add_GetRef(Sig).Name = GetUnfilteredSectionCountName;
	}

	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetRandInfoDef(), TEXT("RandomInfo"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Section"));
		OutFunctions.Add_GetRef(Sig).Name = RandomSectionName;
		OutFunctions.Add_GetRef(Sig).Name = RandomFilteredSectionName;
		OutFunctions.Add_GetRef(Sig).Name = RandomUnfilteredSectionName;
	}

	//////////////////////////////////////////////////////////////////////////
	// VM Misc Functions
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = IsValidName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Valid"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetLocalToWorldName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetMatrix4Def(), TEXT("Matrix"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetLocalToWorldInverseTransposedName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetMatrix4Def(), TEXT("Matrix"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = GetWorldVelocityName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Velocity"));
	}

	//////////////////////////////////////////////////////////////////////////
	// Distance Field Functions
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.Name = QueryDistanceFieldName;
		Sig.bSupportsCPU = false;
		Sig.bExperimental = true;
		Sig.Inputs.Emplace_GetRef(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Execute")).SetValue(true);
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetPositionDef(), TEXT("World Position"));
		const FNiagaraVariable& UseMaxDistanceVar = Sig.Inputs.Emplace_GetRef(FNiagaraTypeDefinition::GetBoolDef(), TEXT("Use Max Distance"));
		const FNiagaraVariable& MaxDistanceVar = Sig.Inputs.Emplace_GetRef(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Max Distance"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetBoolDef(), TEXT("IsValid"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetFloatDef(), TEXT("Distance"));
		
#if WITH_EDITORONLY_DATA
		Sig.Description = LOCTEXT("QueryDistanceFieldDescription", "Given a world position, this returns the value of the parented static mesh`s signed distance field");
		Sig.InputDescriptions.Add(
			UseMaxDistanceVar,
			LOCTEXT("UseMaxDistanceOptimizationDescription", 
			"This enables an optimization that will skip reading the SDF texture if the world position exceeds a provided max distance value.\n"
			"This is a useful optimization if you only need accurate distance information within a certain distance threshold. \n"
			"One example of a good usecase is if you only want to spawn particles inside the mesh (i.e. distance < 0.0), you should enable this with a MaxDistance of 0.0.")
		);
		Sig.InputDescriptions.Add(
			MaxDistanceVar,
			LOCTEXT("MaxDistanceDescription",
			"Only used if UseMaxDistanceOptimization is enabled. This is the max distance from the static meshes` BOUNDING BOX at which we should query it's SDF.\n"
			"This defaults to 0.0, which means it will only read the SDF if the world position is inside the meshes' bounding box")
		);
#endif
	}

	//////////////////////////////////////////////////////////////////////////
	// Deprecated Functions
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.bSoftDeprecatedFunction = true;
		Sig.Name = Deprecated_GetTriColorName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition(FMeshTriCoordinate::StaticStruct()), TEXT("Coord"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetColorDef(), TEXT("Color"));
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.bSoftDeprecatedFunction = true;
		Sig.Name = Deprecated_GetTriUVName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition(FMeshTriCoordinate::StaticStruct()), TEXT("Coord"));
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("UV Set"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec2Def(), TEXT("UV"));
	}
	{
		FNiagaraFunctionSignature & Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.bSoftDeprecatedFunction = true;
		Sig.Name = Deprecated_RandomSectionName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Section"));
	}
	{
		FNiagaraFunctionSignature & Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.bSoftDeprecatedFunction = true;
		Sig.Name = Deprecated_RandomTriCoordName;
		Sig.Outputs.Emplace(FNiagaraTypeDefinition(FMeshTriCoordinate::StaticStruct()), TEXT("Coord"));
	}
	{
		FNiagaraFunctionSignature & Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.bSoftDeprecatedFunction = true;
		Sig.Name = Deprecated_RandomTriCoordOnSectionName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Section"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition(FMeshTriCoordinate::StaticStruct()), TEXT("Coord"));
	}
	// Need custom code
	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.bSoftDeprecatedFunction = true;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition::GetIntDef(), TEXT("Vertex"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Position"));
		OutFunctions.Add_GetRef(Sig).Name = Deprecated_GetVertexPositionName;
		OutFunctions.Add_GetRef(Sig).Name = Deprecated_GetVertexPositionWSName;
	}

	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.bSoftDeprecatedFunction = true;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition(FMeshTriCoordinate::StaticStruct()), TEXT("Coord"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Position"));
		OutFunctions.Add_GetRef(Sig).Name = Deprecated_GetTriPositionName;
		OutFunctions.Add_GetRef(Sig).Name = Deprecated_GetTriPositionWSName;
	}
	{
		FNiagaraFunctionSignature& Sig = OutFunctions.Add_GetRef(BaseSignature);
		Sig.bSoftDeprecatedFunction = true;
		Sig.Name = Deprecated_GetTriPositionAndVelocityWSName;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition(FMeshTriCoordinate::StaticStruct()), TEXT("Coord"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Position"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Velocity"));
	}
	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.bSoftDeprecatedFunction = true;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition(FMeshTriCoordinate::StaticStruct()), TEXT("Coord"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Tangent"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Binormal"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Normal"));
		OutFunctions.Add_GetRef(Sig).Name = Deprecated_GetTriTangentsName;
		OutFunctions.Add_GetRef(Sig).Name = Deprecated_GetTriTangentsWSName;
	}
	{
		FNiagaraFunctionSignature Sig = BaseSignature;
		Sig.bSoftDeprecatedFunction = true;
		Sig.Inputs.Emplace(FNiagaraTypeDefinition(FMeshTriCoordinate::StaticStruct()), TEXT("Coord"));
		Sig.Outputs.Emplace(FNiagaraTypeDefinition::GetVec3Def(), TEXT("Normal"));
		OutFunctions.Add_GetRef(Sig).Name = Deprecated_GetTriNormalName;
		OutFunctions.Add_GetRef(Sig).Name = Deprecated_GetTriNormalWSName;
	}
}

#if WITH_EDITORONLY_DATA
bool UNiagaraDataInterfaceStaticMesh::UpgradeFunctionCall(FNiagaraFunctionSignature& FunctionSignature)
{
	using namespace NDIStaticMeshLocal;

	// Early out for version matching
	if (FunctionSignature.FunctionVersion == EDIFunctionVersion::LatestVersion)
	{
		return false;
	}

	// Major refactor for consistency across the data interface
	if (FunctionSignature.FunctionVersion < EDIFunctionVersion::RefactoredV1)
	{
		static const TPair<FName, FName> NameTranslations[] =
		{
			MakeTuple("RandomSection", Deprecated_RandomSectionName),
			MakeTuple("RandomTriCoord", Deprecated_RandomTriCoordName),
			MakeTuple("RandomTriCoordOnSection", Deprecated_RandomTriCoordOnSectionName),
			MakeTuple("RandomTriCoordUsingVertexColorFilter", Deprecated_RandomTriCoordVCFilteredName),
			MakeTuple("GetVertexPosition", Deprecated_GetVertexPositionName),
			MakeTuple("GetVertexPositionWS", Deprecated_GetVertexPositionWSName),
			MakeTuple("GetTriPosition", Deprecated_GetTriPositionName),
			MakeTuple("GetTriNormal", Deprecated_GetTriNormalName),
			MakeTuple("GetTriTangents", Deprecated_GetTriTangentsName),
			MakeTuple("GetTriPositionWS", Deprecated_GetTriPositionWSName),
			MakeTuple("GetTriNormalWS", Deprecated_GetTriNormalWSName),
			MakeTuple("GetTriTangentsWS", Deprecated_GetTriTangentsWSName),
			MakeTuple("GetTriColor", Deprecated_GetTriColorName),
			MakeTuple("GetTriUV", Deprecated_GetTriUVName),
			MakeTuple("GetTriPositionAndVelocityWS", Deprecated_GetTriPositionAndVelocityWSName),
			MakeTuple("GetMeshLocalToWorldInverseTransposed", GetLocalToWorldInverseTransposedName),
		};

		for ( const auto& NameTrans : NameTranslations )
		{
			if ( FunctionSignature.Name == NameTrans.Key )
			{
				FunctionSignature.Name = NameTrans.Value;
				break;
			}
		}
	}

	if (FunctionSignature.FunctionVersion < EDIFunctionVersion::LargeWorldCoordinates2)
	{
		if (
			(FunctionSignature.Name == GetVertexName) ||
			(FunctionSignature.Name == GetVertexWSName) ||
			(FunctionSignature.Name == GetTriangleName) ||
			(FunctionSignature.Name == GetTriangleWSName) ||
			(FunctionSignature.Name == GetSocketTransformName) ||
			(FunctionSignature.Name == GetSocketTransformWSName) ||
			(FunctionSignature.Name == GetFilteredSocketTransformName) ||
			(FunctionSignature.Name == GetFilteredSocketTransformWSName) ||
			(FunctionSignature.Name == GetUnfilteredSocketTransformName) ||
			(FunctionSignature.Name == GetUnfilteredSocketTransformWSName) )
		{
			check(FunctionSignature.Outputs[0].GetName() == TEXT("Position"));
			check(FunctionSignature.Outputs[0].GetType() == FNiagaraTypeDefinition::GetVec3Def() || FunctionSignature.Outputs[0].GetType() == FNiagaraTypeDefinition::GetPositionDef());
			FunctionSignature.Outputs[0].SetType(FNiagaraTypeDefinition::GetPositionDef());
		}
	}

	FunctionSignature.FunctionVersion = EDIFunctionVersion::LatestVersion;

	return true;
}

void UNiagaraDataInterfaceStaticMesh::GetCommonHLSL(FString& OutHLSL)
{
	if (GetDefault<UNiagaraSettings>()->NDIStaticMesh_AllowDistanceFields)
	{
		OutHLSL += TEXT("#include \"/Engine/Private/DistanceFieldLightingShared.ush\"\n");
		OutHLSL += TEXT("#include \"/Engine/Private/MeshDistanceFieldCommon.ush\"\n");
	}
}
#endif //WITH_EDITORONLY_DATA

void UNiagaraDataInterfaceStaticMesh::GetVMExternalFunction(const FVMExternalFunctionBindingInfo& BindingInfo, void* InstanceData, FVMExternalFunction& OutFunc)
{
	using namespace NDIStaticMeshLocal;

	//////////////////////////////////////////////////////////////////////////
	// VM Vertex Sampling
	if (BindingInfo.Name == IsValidVertexName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMIsValidVertex(Context); });
	}
	else if (BindingInfo.Name == RandomVertexName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomVertex(Context); });
	}
	else if (BindingInfo.Name == GetVertexCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetVertexCount(Context); });
	}
	//else if (BindingInfo.Name == IsValidFilteredVertexName)
	//{
	//	OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMIsValidFilteredVertex(Context); });
	//}
	//else if (BindingInfo.Name == RandomFilteredVertexName)
	//{
	//	OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomFilteredVertexAt(Context); });
	//}
	//else if (BindingInfo.Name == GetFilteredVertexCountName)
	//{
	//	OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetFilteredVertexCount(Context); });
	//}
	//else if (BindingInfo.Name == GetFilteredVertexAtName)
	//{
	//	OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetFilteredVertexAt(Context); });
	//}
	else if (BindingInfo.Name == GetVertexName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetVertex<FNDITransformHandlerNoop>(Context); });
	}
	else if (BindingInfo.Name == GetVertexWSName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetVertex<FNDITransformHandler>(Context); });
	}
	else if (BindingInfo.Name == GetVertexColorName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetVertexColor(Context); });
	}
	else if (BindingInfo.Name == GetVertexUVName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetVertexUV(Context); });
	}

	//////////////////////////////////////////////////////////////////////////
	// VM Triangle Sampling
	else if (BindingInfo.Name == IsValidTriangleName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMIsValidTriangle(Context); });
	}
	else if (BindingInfo.Name == RandomTriangleName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomTriangle<FNDIRandomHelper>(Context); });
	}
	else if (BindingInfo.Name == GetTriangleCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleCount(Context); });
	}
	else if (BindingInfo.Name == IsValidFilteredTriangleName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMIsValidFilteredTriangle(Context); });
	}
	else if (BindingInfo.Name == RandomFilteredTriangleName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomFilteredTriangle(Context); });
	}
	else if (BindingInfo.Name == GetFilteredTriangleCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetFilteredTriangleCount(Context); });
	}
	else if (BindingInfo.Name == GetFilteredTriangleAtName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetFilteredTriangleAt(Context); });
	}
	else if (BindingInfo.Name == IsValidUnfilteredTriangleName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMIsValidUnfilteredTriangle(Context); });
	}
	else if (BindingInfo.Name == RandomUnfilteredTriangleName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomUnfilteredTriangle(Context); });
	}
	else if (BindingInfo.Name == GetUnfilteredTriangleCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetUnfilteredTriangleCount(Context); });
	}
	else if (BindingInfo.Name == GetUnfilteredTriangleAtName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetUnfilteredTriangleAt(Context); });
	}
	else if (BindingInfo.Name == GetTriangleName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangle<FNDITransformHandlerNoop>(Context); });
	}
	else if (BindingInfo.Name == GetTriangleWSName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangle<FNDITransformHandler>(Context); });
	}
	else if (BindingInfo.Name == GetTriangleColorName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleColor(Context); });
	}
	else if (BindingInfo.Name == GetTriangleUVName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleUV(Context); });
	}
	else if (BindingInfo.Name == GetTriangleIndicesName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleIndices(Context); });
	}

	//////////////////////////////////////////////////////////////////////////
	// Socket Functions
	else if (BindingInfo.Name == GetSocketCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetSocketCount(Context); });
	}
	else if (BindingInfo.Name == GetFilteredSocketCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetFilteredSocketCount(Context); });
	}
	else if (BindingInfo.Name == GetUnfilteredSocketCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetUnfilteredSocketCount(Context); });
	}
	else if (BindingInfo.Name == GetSocketTransformName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetSocketTransform<FNDITransformHandlerNoop>(Context); });
	}
	else if (BindingInfo.Name == GetSocketTransformWSName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetSocketTransform<FNDITransformHandler>(Context); });
	}
	else if (BindingInfo.Name == GetFilteredSocketTransformName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetFilteredSocketTransform<FNDITransformHandlerNoop>(Context); });
	}
	else if (BindingInfo.Name == GetFilteredSocketTransformWSName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetFilteredSocketTransform<FNDITransformHandler>(Context); });
	}
	else if (BindingInfo.Name == GetUnfilteredSocketTransformName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetUnfilteredSocketTransform<FNDITransformHandlerNoop>(Context); });
	}
	else if (BindingInfo.Name == GetUnfilteredSocketTransformWSName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetUnfilteredSocketTransform<FNDITransformHandler>(Context); });
	}

	//////////////////////////////////////////////////////////////////////////
	// Section functions
	else if (BindingInfo.Name == IsValidSectionName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMIsValidSection(Context); });
	}
	else if (BindingInfo.Name == GetSectionTriangleCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetSectionTriangleCount(Context); });
	}
	else if (BindingInfo.Name == RandomSectionTriangleName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomSectionTriangle<FNDIRandomHelper>(Context); });
	}
	else if (BindingInfo.Name == GetSectionTriangleAtName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetSectionTriangleAt(Context); });
	}
	else if (BindingInfo.Name == GetFilteredSectionAtName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetFilteredSectionAt(Context); });
	}
	else if (BindingInfo.Name == GetUnfilteredSectionAtName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetUnfilteredSectionAt(Context); });
	}
	else if (BindingInfo.Name == GetSectionCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetSectionCount(Context); });
	}
	else if (BindingInfo.Name == GetFilteredSectionCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetFilteredSectionCount(Context); });
	}
	else if (BindingInfo.Name == GetUnfilteredSectionCountName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetUnfilteredSectionCount(Context); });
	}
	else if (BindingInfo.Name == RandomSectionName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomSection<FNDIRandomHelper>(Context); });
	}
	else if (BindingInfo.Name == RandomFilteredSectionName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomFilteredSection<FNDIRandomHelper>(Context); });
	}
	else if (BindingInfo.Name == RandomUnfilteredSectionName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomUnfilteredSection(Context); });
	}

	//////////////////////////////////////////////////////////////////////////
	// VM Misc Functions
	else if (BindingInfo.Name == IsValidName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMIsValid(Context); });
	}
	else if (BindingInfo.Name == GetLocalToWorldName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetLocalToWorld(Context); });
	}
	else if (BindingInfo.Name == GetLocalToWorldInverseTransposedName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetLocalToWorldInverseTransposed(Context); });
	}
	else if (BindingInfo.Name == GetWorldVelocityName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetWorldVelocity(Context); });
	}

	//////////////////////////////////////////////////////////////////////////
	// Deprecated Functions
	else if (BindingInfo.Name == Deprecated_GetTriColorName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleColor(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetTriUVName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleUV(Context); });
	}
	// Custom VM bindings
	else if (BindingInfo.Name == Deprecated_RandomSectionName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomFilteredSection<FNDIRandomHelperFromStream>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_RandomTriCoordName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomTriangle<FNDIRandomHelperFromStream>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_RandomTriCoordOnSectionName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMRandomSectionTriangle<FNDIRandomHelperFromStream>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetVertexPositionName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetVertexPosition_Deprecated<FNDITransformHandlerNoop>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetVertexPositionWSName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetVertexPosition_Deprecated<FNDITransformHandler>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetTriPositionName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriPosition_Deprecated<FNDITransformHandlerNoop>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetTriPositionWSName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriPosition_Deprecated<FNDITransformHandler>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetTriTangentsName)
	{	
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleTangentBasis_Deprecated<FNDITransformHandlerNoop>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetTriTangentsWSName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleTangentBasis_Deprecated<FNDITransformHandler>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetTriNormalName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleNormal_Deprecated<FNDITransformHandlerNoop>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetTriNormalWSName)
	{
		OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriangleNormal_Deprecated<FNDITransformHandler>(Context); });
	}
	else if (BindingInfo.Name == Deprecated_GetTriPositionAndVelocityWSName)
	{
	OutFunc = FVMExternalFunction::CreateLambda([this](FVectorVMExternalFunctionContext& Context) { VMGetTriPositionAndVelocity_Deprecated<FNDITransformHandler>(Context); });
	}
}

bool UNiagaraDataInterfaceStaticMesh::RequiresDistanceFieldData() const
{
	return GetDefault<UNiagaraSettings>()->NDIStaticMesh_AllowDistanceFields;
}

#if WITH_EDITORONLY_DATA
bool UNiagaraDataInterfaceStaticMesh::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
	bool bSuccess = Super::AppendCompileHash(InVisitor);
	FSHAHash Hash = GetShaderFileHash(NDIStaticMeshLocal::TemplateShaderFile, EShaderPlatform::SP_PCD3D_SM5);
	InVisitor->UpdateString(TEXT("NiagaraDataInterfaceStaticMeshTemplateHLSLSource"), Hash.ToString());
	InVisitor->UpdatePOD(TEXT("NDIStaticMesh_AllowDistanceField"), GetDefault<UNiagaraSettings>()->NDIStaticMesh_AllowDistanceFields ? 1 : 0);
	return bSuccess;
}

void UNiagaraDataInterfaceStaticMesh::ModifyCompilationEnvironment(EShaderPlatform ShaderPlatform, struct FShaderCompilerEnvironment& OutEnvironment) const
{
	Super::ModifyCompilationEnvironment(ShaderPlatform, OutEnvironment);

	OutEnvironment.SetDefine(TEXT("DISTATICMESH_ALLOWDISTANCEFIELD"), GetDefault<UNiagaraSettings>()->NDIStaticMesh_AllowDistanceFields ? 1 : 0);
}

void UNiagaraDataInterfaceStaticMesh::GetParameterDefinitionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, FString& OutHLSL)
{
	TMap<FString, FStringFormatArg> TemplateArgs =
	{
		{TEXT("ParameterName"),	ParamInfo.DataInterfaceHLSLSymbol},
	};

	FString TemplateFile;
	LoadShaderSourceFile(NDIStaticMeshLocal::TemplateShaderFile, EShaderPlatform::SP_PCD3D_SM5, &TemplateFile, nullptr);
	OutHLSL += FString::Format(*TemplateFile, TemplateArgs);
}

bool UNiagaraDataInterfaceStaticMesh::GetFunctionHLSL(const FNiagaraDataInterfaceGPUParamInfo& ParamInfo, const FNiagaraDataInterfaceGeneratedFunction& FunctionInfo, int FunctionInstanceIndex, FString& OutHLSL)
{
	using namespace NDIStaticMeshLocal;

	// Currently we support all functions on the GPU
	// Add an exclusion if the function is not supported by GPU

	TMap<FString, FStringFormatArg> ArgsSample =
	{
		{TEXT("ParameterName"),	ParamInfo.DataInterfaceHLSLSymbol},
		{TEXT("InstanceFunctionName"), FunctionInfo.InstanceName},
	};

	// We still need to go through some hoops for the old deprecated functions
	if (FunctionInfo.DefinitionName == Deprecated_RandomSectionName)
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(out int OutSection) { RandomFilteredSection_{ParameterName}(MakeRandInfo(), OutSection); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if (FunctionInfo.DefinitionName == Deprecated_RandomTriCoordName )
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(out MeshTriCoordinate OutTriCoord) { RandomTriangle_{ParameterName}(MakeRandInfo(), OutTriCoord.Tri, OutTriCoord.BaryCoord); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if (FunctionInfo.DefinitionName == Deprecated_RandomTriCoordOnSectionName )
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(int Section, out MeshTriCoordinate OutTriCoord) { RandomSectionTriangle_{ParameterName}(MakeRandInfo(), Section, OutTriCoord.Tri, OutTriCoord.BaryCoord); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if (FunctionInfo.DefinitionName == Deprecated_RandomTriCoordVCFilteredName )
	{
		// Not supported anymore
		return false;
	}
	else if (FunctionInfo.DefinitionName == Deprecated_GetVertexPositionName)
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(int Vertex, out float3 Position) { float3 Dummy; GetVertex_{ParameterName}(Vertex, Position, Dummy, Dummy, Dummy, Dummy); }");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if (FunctionInfo.DefinitionName == Deprecated_GetVertexPositionWSName)
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(int Vertex, out float3 Position) { float3 Dummy; GetVertexWS_{ParameterName}(Vertex, Position, Dummy, Dummy, Dummy, Dummy); }");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if (FunctionInfo.DefinitionName == Deprecated_GetTriPositionName )
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(MeshTriCoordinate TriCoord, out float3 Position) { float3 Dummy; GetTriangle_{ParameterName}(TriCoord.Tri, TriCoord.BaryCoord, Position, Dummy, Dummy, Dummy, Dummy); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if ( FunctionInfo.DefinitionName == Deprecated_GetTriNormalName )
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(MeshTriCoordinate TriCoord, out float3 Normal) { float3 Dummy; GetTriangle_{ParameterName}(TriCoord.Tri, TriCoord.BaryCoord, Dummy, Dummy, Normal, Dummy, Dummy); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if ( FunctionInfo.DefinitionName == Deprecated_GetTriTangentsName )
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(MeshTriCoordinate TriCoord, out float3 Tangent, out float3 Bitangent, out float3 Normal) { float3 Dummy; GetTriangle_{ParameterName}(TriCoord.Tri, TriCoord.BaryCoord, Dummy, Dummy, Normal, Bitangent, Tangent); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if ( FunctionInfo.DefinitionName == Deprecated_GetTriPositionWSName )
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(MeshTriCoordinate TriCoord, out float3 Position) { float3 Dummy; GetTriangleWS_{ParameterName}(TriCoord.Tri, TriCoord.BaryCoord, Position, Dummy, Dummy, Dummy, Dummy); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if ( FunctionInfo.DefinitionName == Deprecated_GetTriNormalWSName )
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(MeshTriCoordinate TriCoord, out float3 Normal) { float3 Dummy; GetTriangleWS_{ParameterName}(TriCoord.Tri, TriCoord.BaryCoord, Dummy, Dummy, Normal, Dummy, Dummy); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if ( FunctionInfo.DefinitionName == Deprecated_GetTriTangentsWSName )
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(MeshTriCoordinate TriCoord, out float3 Tangent, out float3 Bitangent, out float3 Normal) { float3 Dummy; GetTriangleWS_{ParameterName}(TriCoord.Tri, TriCoord.BaryCoord, Dummy, Dummy, Normal, Bitangent, Tangent); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if (FunctionInfo.DefinitionName == Deprecated_GetTriColorName)
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(MeshTriCoordinate TriCoord, out float4 Color) { GetTriangleColor_{ParameterName}(TriCoord.Tri, TriCoord.BaryCoord, Color); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if (FunctionInfo.DefinitionName == Deprecated_GetTriUVName)
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(MeshTriCoordinate TriCoord, int UVSet, out float2 UV) { GetTriangleUV_{ParameterName}(TriCoord.Tri, TriCoord.BaryCoord, UVSet, UV); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}
	else if ( FunctionInfo.DefinitionName == Deprecated_GetTriPositionAndVelocityWSName )
	{
		static const TCHAR* FormatSample = TEXT("void {InstanceFunctionName}(MeshTriCoordinate TriCoord, out float3 Position, out float3 Velocity) { float3 Dummy; GetTriangleWS_{ParameterName}(TriCoord.Tri, TriCoord.BaryCoord, Position, Velocity, Dummy, Dummy, Dummy); }\r\n");
		OutHLSL += FString::Format(FormatSample, ArgsSample);
	}

	return true;
}
#endif

bool UNiagaraDataInterfaceStaticMesh::Equals(const UNiagaraDataInterface* Other) const
{
	if (!Super::Equals(Other))
	{
		return false;
	}
	const UNiagaraDataInterfaceStaticMesh* OtherTyped = CastChecked<const UNiagaraDataInterfaceStaticMesh>(Other);
	return OtherTyped->SourceMode == SourceMode &&
		OtherTyped->Source == Source &&
		OtherTyped->SourceComponent == SourceComponent &&
		OtherTyped->DefaultMesh == DefaultMesh &&
		OtherTyped->SectionFilter.AllowedMaterialSlots == SectionFilter.AllowedMaterialSlots &&
		OtherTyped->bUsePhysicsBodyVelocity == bUsePhysicsBodyVelocity &&
		OtherTyped->FilteredSockets == FilteredSockets;
}

bool UNiagaraDataInterfaceStaticMesh::CopyToInternal(UNiagaraDataInterface* Destination) const
{
	if (!Super::CopyToInternal(Destination))
	{
		return false;
	}

	UNiagaraDataInterfaceStaticMesh* OtherTyped = CastChecked<UNiagaraDataInterfaceStaticMesh>(Destination);
	OtherTyped->SourceMode = SourceMode;
	OtherTyped->Source = Source;
	OtherTyped->SourceComponent = SourceComponent;
	OtherTyped->DefaultMesh = DefaultMesh;
#if WITH_EDITORONLY_DATA
	OtherTyped->PreviewMesh = PreviewMesh;
#endif
	OtherTyped->SectionFilter = SectionFilter;
	OtherTyped->bUsePhysicsBodyVelocity = bUsePhysicsBodyVelocity;
	OtherTyped->FilteredSockets = FilteredSockets;
	return true;
}

#if WITH_EDITOR
void UNiagaraDataInterfaceStaticMesh::GetFeedback(UNiagaraSystem* Asset, UNiagaraComponent* Component, TArray<FNiagaraDataInterfaceError>& OutErrors, TArray<FNiagaraDataInterfaceFeedback>& OutWarnings, TArray<FNiagaraDataInterfaceFeedback>& OutInfo)
{
	if (Source == nullptr && DefaultMesh != nullptr && !DefaultMesh->bAllowCPUAccess)
	{
		FNiagaraDataInterfaceError CPUAccessNotAllowedError(FText::Format(LOCTEXT("CPUAccessNotAllowedError", "This mesh needs CPU access in order to be used properly.({0})"), FText::FromString(DefaultMesh->GetName())),
			LOCTEXT("CPUAccessNotAllowedErrorSummary", "CPU access error"),
			FNiagaraDataInterfaceFix::CreateLambda([=]()
		{
			DefaultMesh->Modify();
			DefaultMesh->bAllowCPUAccess = true;
			return true;
		}));

		OutErrors.Add(CPUAccessNotAllowedError);
	}

	bool bHasNoMeshAssignedWarning = (Source == nullptr && DefaultMesh == nullptr);
#if WITH_EDITORONLY_DATA
	if (bHasNoMeshAssignedWarning)
	{
		if (UStaticMesh* LocalPreviewMesh = PreviewMesh.LoadSynchronous())
		{
			bHasNoMeshAssignedWarning = false;

			if (!LocalPreviewMesh->bAllowCPUAccess)
			{
				FNiagaraDataInterfaceError CPUAccessNotAllowedError(FText::Format(LOCTEXT("CPUAccessNotAllowedError", "This mesh needs CPU access in order to be used properly.({0})"), FText::FromString(LocalPreviewMesh->GetName())),
					LOCTEXT("CPUAccessNotAllowedErrorSummary", "CPU access error"),
					FNiagaraDataInterfaceFix::CreateLambda([=]()
					{
						LocalPreviewMesh->Modify();
						LocalPreviewMesh->bAllowCPUAccess = true;
						return true;
					}));

				OutErrors.Add(CPUAccessNotAllowedError);
			}
		}
	}
#endif

	if (bHasNoMeshAssignedWarning)
	{
		FNiagaraDataInterfaceFeedback NoMeshAssignedError(LOCTEXT("NoMeshAssignedError", "This Data Interface should be assigned a static mesh to operate correctly."),
			LOCTEXT("NoMeshAssignedErrorSummary", "No mesh assigned warning"),
			FNiagaraDataInterfaceFix());

		OutWarnings.Add(NoMeshAssignedError);
	}

	if (GetDefault<UNiagaraSettings>()->NDIStaticMesh_AllowDistanceFields == false)
	{
		FNiagaraDataInterfaceUtilities::ForEachGpuFunctionEquals(
			this, Asset, Component,
			[&](const FNiagaraDataInterfaceGeneratedFunction& FunctionBinding)
			{
				if (FunctionBinding.DefinitionName == NDIStaticMeshLocal::QueryDistanceFieldName )
				{
					OutWarnings.Emplace(
						LOCTEXT("DistanceFieldsNotEnabled", "Distance fields functionality is disabled, this can be enabled in Niagara project settings."),
						LOCTEXT("DistanceFieldsNotEnabledSummary", "Distance fields functionality is disabled."),
						FNiagaraDataInterfaceFix()
					);

					return false;
				}
				return true;
			}
		);
	}
}
#endif //WITH_EDITOR

UStaticMesh* UNiagaraDataInterfaceStaticMesh::GetStaticMesh(USceneComponent*& OutComponent, class FNiagaraSystemInstance* SystemInstance)
{
	// Helper to scour an actor (or its parents) for a valid Static mesh component
	auto FindActorMeshComponent = [](AActor* Actor, bool bRecurseParents = false) -> UStaticMeshComponent*
	{
		if (AStaticMeshActor* MeshActor = Cast<AStaticMeshActor>(Actor))
		{
			UStaticMeshComponent* Comp = MeshActor->GetStaticMeshComponent();
			if (::IsValid(Comp))
			{
				return Comp;
			}
		}

		// Fall back on any valid component on the actor
		while (Actor)
		{
			for (UActorComponent* ActorComp : Actor->GetComponents())
			{
				UStaticMeshComponent* Comp = Cast<UStaticMeshComponent>(ActorComp);
				if (::IsValid(Comp) && Comp->GetStaticMesh() != nullptr)
				{
					return Comp;
				}
			}

			if (bRecurseParents)
			{
				Actor = Actor->GetParentActor();
			}
			else
			{
				break;
			}
		}

		return nullptr;
	};

	UStaticMeshComponent* FoundMeshComponent = nullptr;	

	const bool bTrySource = SourceMode == ENDIStaticMesh_SourceMode::Default || SourceMode == ENDIStaticMesh_SourceMode::Source;
	const bool bTryAttachParent = SourceMode == ENDIStaticMesh_SourceMode::Default || SourceMode == ENDIStaticMesh_SourceMode::AttachParent;
	const bool bTryDefaultMesh = SourceMode == ENDIStaticMesh_SourceMode::Default || SourceMode == ENDIStaticMesh_SourceMode::DefaultMeshOnly;

	if (bTrySource && ::IsValid(SourceComponent))
	{
		FoundMeshComponent = SourceComponent;
	}
	else if (bTrySource && Source)
	{
		FoundMeshComponent = FindActorMeshComponent(Source);
	}
	else if (bTryAttachParent && SystemInstance)
	{
		if (USceneComponent* AttachComponent = SystemInstance->GetAttachComponent())
		{
			// First, try to find the mesh component up the attachment hierarchy
			for (USceneComponent* Curr = AttachComponent; Curr; Curr = Curr->GetAttachParent())
			{
				UStaticMeshComponent* ParentComp = Cast<UStaticMeshComponent>(Curr);
				if (::IsValid(ParentComp))
				{
					FoundMeshComponent = ParentComp;
					break;
				}
			}
			
			if (!FoundMeshComponent)
			{
				// Next, try to find one in our outer chain
				UStaticMeshComponent* OuterComp = AttachComponent->GetTypedOuter<UStaticMeshComponent>();
				if (::IsValid(OuterComp))
				{
					FoundMeshComponent = OuterComp;
				}
				else if (AActor* Actor = AttachComponent->GetAttachmentRootActor())
				{
					// Final fall-back, look for any mesh component on our root actor or any of its parents
					FoundMeshComponent = FindActorMeshComponent(Actor, true);
				}
			}
		}
	}

	UStaticMesh* Mesh = nullptr;
	OutComponent = nullptr;
	if (FoundMeshComponent)
	{
		Mesh = FoundMeshComponent->GetStaticMesh();
		OutComponent = FoundMeshComponent;
	}
	else if (bTryDefaultMesh)
	{
		Mesh = DefaultMesh;
	}

#if WITH_EDITORONLY_DATA
	if (!Mesh && !FoundMeshComponent && (!SystemInstance || !SystemInstance->GetWorld()->IsGameWorld()))
	{
		// NOTE: We don't fall back on the preview mesh if we have a valid static mesh component referenced
		Mesh = PreviewMesh.LoadSynchronous();		
	}
#endif

	return Mesh;
}

void UNiagaraDataInterfaceStaticMesh::SetSourceComponentFromBlueprints(UStaticMeshComponent* ComponentToUse)
{
	// NOTE: When ChangeId changes the next tick will be skipped and a reset of the per-instance data will be initiated. 
	++ChangeId;
	SourceComponent = ComponentToUse;
	Source = ComponentToUse->GetOwner();
}

void UNiagaraDataInterfaceStaticMesh::SetDefaultMeshFromBlueprints(UStaticMesh* MeshToUse)
{
	// NOTE: When ChangeId changes the next tick will be skipped and a reset of the per-instance data will be initiated. 
	++ChangeId;
	SourceComponent = nullptr;
	Source = nullptr;
	DefaultMesh = MeshToUse;
}

//////////////////////////////////////////////////////////////////////////
// VM Vertex Sampling
void UNiagaraDataInterfaceStaticMesh::VMIsValidVertex(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> VertexParam(Context);
	FNDIOutputParam<bool> OutValid(Context);

	const int32 NumVertices = StaticMeshHelper.GetNumPositionVertices();
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		const int32 Vertex = VertexParam.GetAndAdvance();
		OutValid.SetAndAdvance(Vertex >= 0 && Vertex < NumVertices);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMRandomVertex(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIRandomHelper RandHelper(Context);
	FNDIOutputParam<int32> OutVertex(Context);

	const int32 VertexMax = FMath::Max(StaticMeshHelper.GetNumPositionVertices() - 1, 0);
	for (int32 i=0; i < Context.GetNumInstances(); ++i)
	{
		RandHelper.GetAndAdvance();
		const int32 Vertex = RandHelper.RandRange(i, 0, VertexMax);
		OutVertex.SetAndAdvance(Vertex);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetVertexCount(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIOutputParam<int32> OutNumVertices(Context);

	const int32 NumVertices = StaticMeshHelper.GetNumPositionVertices();
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutNumVertices.SetAndAdvance(NumVertices);
	}
}

//void UNiagaraDataInterfaceStaticMesh::VMIsValidFilteredVertex(FVectorVMExternalFunctionContext& Context)
//{
//}

//void UNiagaraDataInterfaceStaticMesh::VMRandomFilteredVertex(FVectorVMExternalFunctionContext& Context)
//{
//}

//void UNiagaraDataInterfaceStaticMesh::VMGetFilteredVertexCount(FVectorVMExternalFunctionContext& Context)
//{
//}
//
//void UNiagaraDataInterfaceStaticMesh::VMGetFilteredVertexAt(FVectorVMExternalFunctionContext& Context)
//{
//}

template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetVertex(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> VertexParam(Context);
	FNDIOutputParam<FVector3f> OutPosition(Context);
	FNDIOutputParam<FVector3f> OutVelocity(Context);
	FNDIOutputParam<FVector3f> OutNormal(Context);
	FNDIOutputParam<FVector3f> OutBitangent(Context);
	FNDIOutputParam<FVector3f> OutTangent(Context);

	const int32 PositionsMax = StaticMeshHelper.GetNumPositionVertices() - 1;
	const int32 TangentsMax = StaticMeshHelper.GetNumTangentVertices() - 1;
	if ( (PositionsMax >= 0) && (PositionsMax == TangentsMax) )
	{
		const float InvDt = StaticMeshHelper.GetInvDeltaSeconds();
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Vertex = FMath::Clamp(VertexParam.GetAndAdvance(), 0, PositionsMax);
			const FVector3f LocalPosition = StaticMeshHelper.GetLocalPosition(Vertex);
			const FVector3f Position = StaticMeshHelper.TransformPosition(LocalPosition);
			const FVector3f PreviousPosition = StaticMeshHelper.PreviousTransformPosition(LocalPosition);
			const FVector3f Velocity = (Position - PreviousPosition) * InvDt;
			OutPosition.SetAndAdvance(Position);
			OutVelocity.SetAndAdvance(Velocity);
			OutNormal.SetAndAdvance(StaticMeshHelper.GetTangentZ(Vertex));
			OutBitangent.SetAndAdvance(StaticMeshHelper.GetTangentY(Vertex));
			OutTangent.SetAndAdvance(StaticMeshHelper.GetTangentX(Vertex));
		}
	}
	else
	{
		const FVector3f Position = StaticMeshHelper.TransformPosition(FVector3f::ZeroVector);
		const FVector3f Velocity = FVector3f::ZeroVector;
		const FVector3f TangentX = StaticMeshHelper.TransformVector(FVector3f(1.0f, 0.0f, 0.0f));
		const FVector3f TangentY = StaticMeshHelper.TransformVector(FVector3f(0.0f, 1.0f, 0.0f));
		const FVector3f TangentZ = StaticMeshHelper.TransformVector(FVector3f(0.0f, 0.0f, 1.0f));
		for ( int32 i=0; i < Context.GetNumInstances(); ++i )
		{
			OutPosition.SetAndAdvance(Position);
			OutVelocity.SetAndAdvance(Velocity);
			OutNormal.SetAndAdvance(TangentZ);
			OutBitangent.SetAndAdvance(TangentY);
			OutTangent.SetAndAdvance(TangentX);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetVertexColor(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper StaticMeshHelper(Context);
	FNDIInputParam<int32> VertexParam(Context);
	FNDIOutputParam<FLinearColor> OutColor(Context);

	const int32 ColorsMax = StaticMeshHelper.GetNumColorVertices() - 1;
	if ( ColorsMax >= 0 )
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Vertex = VertexParam.GetAndAdvance();
			OutColor.SetAndAdvance(StaticMeshHelper.GetColor(FMath::Clamp(Vertex, 0, ColorsMax)));
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutColor.SetAndAdvance(FLinearColor::White);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetVertexUV(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper StaticMeshHelper(Context);
	FNDIInputParam<int32> VertexParam(Context);
	FNDIInputParam<int32> UVSetParam(Context);
	FNDIOutputParam<FVector2f> OutUV(Context);

	const int32 VertexMax = StaticMeshHelper.GetNumUVVertices() - 1;
	const int32 UVSetMax = StaticMeshHelper.GetNumUVs() - 1;
	if (VertexMax >= 0 && UVSetMax >= 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Vertex = VertexParam.GetAndAdvance();
			const int32 UVSet = UVSetParam.GetAndAdvance();
			const FVector2f UV = FVector2f(StaticMeshHelper.GetUV(FMath::Clamp(Vertex, 0, VertexMax), FMath::Clamp(UVSet, 0, UVSetMax)));	// LWC_TODO: Precision loss
			OutUV.SetAndAdvance(UV);
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutUV.SetAndAdvance(FVector2f::ZeroVector);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// VM Triangle Sampling
void UNiagaraDataInterfaceStaticMesh::VMIsValidTriangle(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIOutputParam<bool> OutValid(Context);

	const int32 NumTriangles = StaticMeshHelper.GetNumTriangles();
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		const int32 Triangle = TriangleParam.GetAndAdvance();
		OutValid.SetAndAdvance(Triangle >= 0 && Triangle < NumTriangles);
	}
}

template<typename TRandomHelper>
void UNiagaraDataInterfaceStaticMesh::VMRandomTriangle(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	TRandomHelper RandHelper(Context);
	FNDIOutputParam<int32> OutTriangleParam(Context);
	FNDIOutputParam<FVector3f> OutBaryCoordParam(Context);

	if ( StaticMeshHelper.IsCpuUniformlyDistributedSampling() )
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			RandHelper.GetAndAdvance();
			OutTriangleParam.SetAndAdvance(StaticMeshHelper.RandomUniformTriangle(RandHelper, i));
			OutBaryCoordParam.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
		}
	}
	else
	{
		const int32 NumTriangles = StaticMeshHelper.GetNumTriangles();
		const int32 TriangleMax = FMath::Max(NumTriangles - 1, 0);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			RandHelper.GetAndAdvance();
			OutTriangleParam.SetAndAdvance(RandHelper.RandRange(i, 0, TriangleMax));
			OutBaryCoordParam.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetTriangleCount(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIOutputParam<int32> OutCountParam(Context);

	const int32 NumTriangles = StaticMeshHelper.GetNumTriangles();
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutCountParam.SetAndAdvance(NumTriangles);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMIsValidFilteredTriangle(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIOutputParam<bool> OutValid(Context);

	const int32 NumTriangles = StaticMeshHelper.GetNumFilteredTriangles();
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		const int32 Triangle = TriangleParam.GetAndAdvance();
		OutValid.SetAndAdvance(Triangle >= 0 && Triangle < NumTriangles);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMRandomFilteredTriangle(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIRandomHelper RandHelper(Context);
	FNDIOutputParam<int32> OutTriangleParam(Context);
	FNDIOutputParam<FVector3f> OutBaryCoordParam(Context);

	TConstArrayView<NDIStaticMeshLocal::FNDISectionInfo> SectionInfos = StaticMeshHelper.InstanceData->GetFilteredSectionInfos();
	if (SectionInfos.Num() )
	{
		if ( StaticMeshHelper.IsCpuUniformlyDistributedSampling() )
		{
			TConstArrayView<int32> SectionRemap = StaticMeshHelper.InstanceData->GetFilteredSections();
			for (int32 i=0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				OutTriangleParam.SetAndAdvance(StaticMeshHelper.RandomUniformTriangle(RandHelper, i, SectionInfos, SectionRemap));
				OutBaryCoordParam.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
			}
		}
		else
		{
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				OutTriangleParam.SetAndAdvance(StaticMeshHelper.RandomTriangle(RandHelper, i, SectionInfos));
				OutBaryCoordParam.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
			}
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			RandHelper.GetAndAdvance();
			OutTriangleParam.SetAndAdvance(0);
			OutBaryCoordParam.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetFilteredTriangleCount(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIOutputParam<int32> OutCount(Context);

	const int32 NumTriangles = StaticMeshHelper.GetNumFilteredTriangles();
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutCount.SetAndAdvance(NumTriangles);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetFilteredTriangleAt(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> IndexParam(Context);
	FNDIOutputParam<int32> OutTriangle(Context);

	const int32 TriangleMax = StaticMeshHelper.GetNumFilteredTriangles() - 1;
	if ( TriangleMax > 0 )
	{
		TConstArrayView<NDIStaticMeshLocal::FNDISectionInfo> FilteredSections = StaticMeshHelper.InstanceData->GetFilteredSectionInfos();
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Index = FMath::Clamp(IndexParam.GetAndAdvance(), 0, TriangleMax);
			const int32 Triangle = StaticMeshHelper.SectionTriangleToTriangle(FilteredSections, Index);
			OutTriangle.SetAndAdvance(Triangle);
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutTriangle.SetAndAdvance(0);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMIsValidUnfilteredTriangle(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIOutputParam<bool> OutValid(Context);

	const int32 NumTriangles = StaticMeshHelper.GetNumUnfilteredTriangles();
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		const int32 Triangle = TriangleParam.GetAndAdvance();
		OutValid.SetAndAdvance(Triangle >= 0 && Triangle < NumTriangles);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMRandomUnfilteredTriangle(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIRandomHelper RandHelper(Context);
	FNDIOutputParam<int32> OutTriangleParam(Context);
	FNDIOutputParam<FVector3f> OutBaryCoordParam(Context);

	TConstArrayView<NDIStaticMeshLocal::FNDISectionInfo> SectionInfos = StaticMeshHelper.InstanceData->GetUnfilteredSectionInfos();
	if (SectionInfos.Num() )
	{
		if ( StaticMeshHelper.IsCpuUniformlyDistributedSampling() )
		{
			TConstArrayView<int32> SectionRemap = StaticMeshHelper.InstanceData->GetUnfilteredSections();
			for (int32 i=0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				OutTriangleParam.SetAndAdvance(StaticMeshHelper.RandomUniformTriangle(RandHelper, i, SectionInfos, SectionRemap));
				OutBaryCoordParam.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
			}
		}
		else
		{
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				OutTriangleParam.SetAndAdvance(StaticMeshHelper.RandomTriangle(RandHelper, i, SectionInfos));
				OutBaryCoordParam.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
			}
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			RandHelper.GetAndAdvance();
			OutTriangleParam.SetAndAdvance(0);
			OutBaryCoordParam.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetUnfilteredTriangleCount(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIOutputParam<int32> OutCount(Context);

	const int32 NumTriangles = StaticMeshHelper.GetNumUnfilteredTriangles();
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutCount.SetAndAdvance(NumTriangles);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetUnfilteredTriangleAt(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> IndexParam(Context);
	FNDIOutputParam<int32> OutTriangle(Context);

	const int32 TriangleMax = StaticMeshHelper.GetNumUnfilteredTriangles() - 1;
	if ( TriangleMax > 0 )
	{
		TConstArrayView<NDIStaticMeshLocal::FNDISectionInfo> UnfilteredSections = StaticMeshHelper.InstanceData->GetUnfilteredSectionInfos();
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Index = FMath::Clamp(IndexParam.GetAndAdvance(), 0, TriangleMax);
			const int32 Triangle = StaticMeshHelper.SectionTriangleToTriangle(UnfilteredSections, Index);
			OutTriangle.SetAndAdvance(Triangle);
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutTriangle.SetAndAdvance(0);
		}
	}
}

template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetTriangle(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIInputParam<FVector3f> BaryCoordParam(Context);
	FNDIOutputParam<FVector3f> OutPositionParam(Context);
	FNDIOutputParam<FVector3f> OutVelocityParam(Context);
	FNDIOutputParam<FVector3f> OutNormalParam(Context);
	FNDIOutputParam<FVector3f> OutBitangentParam(Context);
	FNDIOutputParam<FVector3f> OutTangentParam(Context);

	const FIndexArrayView IndexArray = StaticMeshHelper.GetIndexArrayView();
	const int32 TriangleMax = StaticMeshHelper.GetNumTriangles() - 1;
	if (TriangleMax >= 0 && StaticMeshHelper.GetNumPositionVertices() > 0 && StaticMeshHelper.GetNumTangentVertices() > 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Triangle = FMath::Clamp(TriangleParam.GetAndAdvance(), 0, TriangleMax);
			const FVector3f BaryCoord = BaryCoordParam.GetAndAdvance();
			const FVector3f LocalPosition = StaticMeshHelper.GetLocalTrianglePosition(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			const FVector3f Position = StaticMeshHelper.TransformPosition(LocalPosition);
			const FVector3f PreviousPosition = StaticMeshHelper.PreviousTransformPosition(LocalPosition);
			const FVector3f Velocity = (Position - PreviousPosition) * StaticMeshHelper.GetInvDeltaSeconds();
			const FVector3f TangentX = StaticMeshHelper.GetTriangleTangentX(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			const FVector3f TangentY = StaticMeshHelper.GetTriangleTangentY(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			const FVector3f TangentZ = StaticMeshHelper.GetTriangleTangentZ(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			OutPositionParam.SetAndAdvance(Position);
			OutVelocityParam.SetAndAdvance(Velocity);
			OutNormalParam.SetAndAdvance(TangentZ);
			OutBitangentParam.SetAndAdvance(TangentY);
			OutTangentParam.SetAndAdvance(TangentX);
		}
	}
	else
	{
		const FVector3f Position = StaticMeshHelper.TransformPosition(FVector3f::ZeroVector);
		const FVector3f PreviousPosition = StaticMeshHelper.PreviousTransformPosition(FVector3f::ZeroVector);
		const FVector3f Velocity = FVector3f::ZeroVector;
		const FVector3f TangentX = StaticMeshHelper.TransformVector(FVector3f(1.0f, 0.0f, 0.0f));
		const FVector3f TangentY = StaticMeshHelper.TransformVector(FVector3f(0.0f, 1.0f, 0.0f));
		const FVector3f TangentZ = StaticMeshHelper.TransformVector(FVector3f(0.0f, 0.0f, 1.0f));
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutPositionParam.SetAndAdvance(Position);
			OutVelocityParam.SetAndAdvance(Velocity);
			OutNormalParam.SetAndAdvance(TangentZ);
			OutBitangentParam.SetAndAdvance(TangentY);
			OutTangentParam.SetAndAdvance(TangentX);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetTriangleColor(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIInputParam<FVector3f> BaryCoordParam(Context);
	FNDIOutputParam<FLinearColor> OutColorParam(Context);

	const FIndexArrayView IndexArray = StaticMeshHelper.GetIndexArrayView();
	const int32 TriangleMax = StaticMeshHelper.GetNumTriangles() - 1;
	if (TriangleMax >= 0 && StaticMeshHelper.GetNumColorVertices() > 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Triangle = FMath::Clamp(TriangleParam.GetAndAdvance(), 0, TriangleMax);
			const FVector3f BaryCoord = BaryCoordParam.GetAndAdvance();
			const FLinearColor Color = StaticMeshHelper.GetTriangleColor(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			OutColorParam.SetAndAdvance(Color);
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutColorParam.SetAndAdvance(FLinearColor::White);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetTriangleUV(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIInputParam<FVector3f> BaryCoordParam(Context);
	FNDIInputParam<int32> UVSetParam(Context);
	FNDIOutputParam<FVector2f> OutUVParam(Context);

	const FIndexArrayView IndexArray = StaticMeshHelper.GetIndexArrayView();
	const int32 TriangleMax = StaticMeshHelper.GetNumTriangles() - 1;
	const int32 UVSetMax = StaticMeshHelper.GetNumUVs() - 1;
	if (TriangleMax >= 0 && UVSetMax >= 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Triangle = FMath::Clamp(TriangleParam.GetAndAdvance(), 0, TriangleMax);
			const FVector3f BaryCoord = BaryCoordParam.GetAndAdvance();
			const int32 UVSet = FMath::Clamp(UVSetParam.GetAndAdvance(), 0, UVSetMax);
			const FVector2f UV = FVector2f(StaticMeshHelper.GetTriangleUV(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2], UVSet));	// LWC_TODO: Precision loss
			OutUVParam.SetAndAdvance(UV);
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutUVParam.SetAndAdvance(FVector2f::ZeroVector);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetTriangleIndices(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIOutputParam<int32> OutIndex0Param(Context);
	FNDIOutputParam<int32> OutIndex1Param(Context);
	FNDIOutputParam<int32> OutIndex2Param(Context);

	const FIndexArrayView IndexArray = StaticMeshHelper.GetIndexArrayView();
	const int32 TriangleMax = StaticMeshHelper.GetNumTriangles() - 1;
	if (TriangleMax >= 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Triangle = FMath::Clamp(TriangleParam.GetAndAdvance(), 0, TriangleMax);
			OutIndex0Param.SetAndAdvance(IndexArray[Triangle * 3 + 0]);
			OutIndex1Param.SetAndAdvance(IndexArray[Triangle * 3 + 1]);
			OutIndex2Param.SetAndAdvance(IndexArray[Triangle * 3 + 2]);
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutIndex0Param.SetAndAdvance(0);
			OutIndex1Param.SetAndAdvance(0);
			OutIndex2Param.SetAndAdvance(0);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Socket Functions
void UNiagaraDataInterfaceStaticMesh::VMGetSocketCount(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<NDIStaticMeshLocal::FInstanceData_GameThread> InstanceData(Context);
	FNDIOutputParam<int32> OutCountParam(Context);

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutCountParam.SetAndAdvance(InstanceData->CachedSockets.Num());
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetFilteredSocketCount(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<NDIStaticMeshLocal::FInstanceData_GameThread> InstanceData(Context);
	FNDIOutputParam<int32> OutCountParam(Context);

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutCountParam.SetAndAdvance(InstanceData->NumFilteredSockets);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetUnfilteredSocketCount(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<NDIStaticMeshLocal::FInstanceData_GameThread> InstanceData(Context);
	FNDIOutputParam<int32> OutCountParam(Context);

	const int32 NumUnfilteredSockets = InstanceData->CachedSockets.Num() - InstanceData->NumFilteredSockets;
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutCountParam.SetAndAdvance(NumUnfilteredSockets);
	}
}

template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetSocketTransform(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> SocketIndexParam(Context);
	FNDIOutputParam<FVector3f> OutTranslateParam(Context);
	FNDIOutputParam<FQuat4f> OutRotateParam(Context);
	FNDIOutputParam<FVector3f> OutScaleParam(Context);

	const int32 SocketMax = StaticMeshHelper.InstanceData->CachedSockets.Num() - 1;
	if (SocketMax >= 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 SocketIndex = FMath::Clamp(SocketIndexParam.GetAndAdvance(), 0, SocketMax);
			const FTransform3f& SocketTransform = StaticMeshHelper.InstanceData->CachedSockets[SocketIndex];
			OutTranslateParam.SetAndAdvance(StaticMeshHelper.TransformPosition(SocketTransform.GetTranslation()));
			OutRotateParam.SetAndAdvance(StaticMeshHelper.TransformRotation(SocketTransform.GetRotation()));
			OutScaleParam.SetAndAdvance(StaticMeshHelper.TransformVector(SocketTransform.GetScale3D()));
		}
	}
	else
	{
		const FVector3f DefaultTranslate = StaticMeshHelper.TransformPosition(FVector3f::ZeroVector);
		const FQuat4f DefaultRotation = StaticMeshHelper.TransformRotation(FQuat4f::Identity);
		const FVector3f DefaultScale = StaticMeshHelper.TransformVector(FVector3f::OneVector);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutTranslateParam.SetAndAdvance(DefaultTranslate);
			OutRotateParam.SetAndAdvance(DefaultRotation);
			OutScaleParam.SetAndAdvance(DefaultScale);
		}
	}
}

template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetFilteredSocketTransform(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> SocketIndexParam(Context);
	FNDIOutputParam<FVector3f> OutTranslateParam(Context);
	FNDIOutputParam<FQuat4f> OutRotateParam(Context);
	FNDIOutputParam<FVector3f> OutScaleParam(Context);

	const int32 SocketMax = StaticMeshHelper.InstanceData->NumFilteredSockets - 1;
	if (SocketMax >= 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 FilteredSocketIndex = FMath::Clamp(SocketIndexParam.GetAndAdvance(), 0, SocketMax);
			const int32 SocketIndex = StaticMeshHelper.InstanceData->FilteredAndUnfilteredSockets[FilteredSocketIndex];
			const FTransform3f& SocketTransform = StaticMeshHelper.InstanceData->CachedSockets[SocketIndex];
			OutTranslateParam.SetAndAdvance(StaticMeshHelper.TransformPosition(SocketTransform.GetTranslation()));
			OutRotateParam.SetAndAdvance(StaticMeshHelper.TransformRotation(SocketTransform.GetRotation()));
			OutScaleParam.SetAndAdvance(StaticMeshHelper.TransformVector(SocketTransform.GetScale3D()));
		}
	}
	else
	{
		const FVector3f DefaultTranslate = StaticMeshHelper.TransformPosition(FVector3f::ZeroVector);
		const FQuat4f DefaultRotation = StaticMeshHelper.TransformRotation(FQuat4f::Identity);
		const FVector3f DefaultScale = StaticMeshHelper.TransformVector(FVector3f::OneVector);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutTranslateParam.SetAndAdvance(DefaultTranslate);
			OutRotateParam.SetAndAdvance(DefaultRotation);
			OutScaleParam.SetAndAdvance(DefaultScale);
		}
	}
}

template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetUnfilteredSocketTransform(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> SocketIndexParam(Context);
	FNDIOutputParam<FVector3f> OutTranslateParam(Context);
	FNDIOutputParam<FQuat4f> OutRotateParam(Context);
	FNDIOutputParam<FVector3f> OutScaleParam(Context);

	const int32 UnfilteredOffset = StaticMeshHelper.InstanceData->NumFilteredSockets;
	const int32 SocketMax = StaticMeshHelper.InstanceData->FilteredAndUnfilteredSockets.Num() - UnfilteredOffset - 1;
	if (SocketMax >= 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 UnfilteredSocketIndex = FMath::Clamp(SocketIndexParam.GetAndAdvance(), 0, SocketMax) + UnfilteredOffset;
			const int32 SocketIndex = StaticMeshHelper.InstanceData->FilteredAndUnfilteredSockets[UnfilteredSocketIndex];
			const FTransform3f& SocketTransform = StaticMeshHelper.InstanceData->CachedSockets[SocketIndex];
			OutTranslateParam.SetAndAdvance(StaticMeshHelper.TransformPosition(SocketTransform.GetTranslation()));
			OutRotateParam.SetAndAdvance(StaticMeshHelper.TransformRotation(SocketTransform.GetRotation()));
			OutScaleParam.SetAndAdvance(StaticMeshHelper.TransformVector(SocketTransform.GetScale3D()));
		}
	}
	else
	{
		const FVector3f DefaultTranslate = StaticMeshHelper.TransformPosition(FVector3f::ZeroVector);
		const FQuat4f DefaultRotation = StaticMeshHelper.TransformRotation(FQuat4f::Identity);
		const FVector3f DefaultScale = StaticMeshHelper.TransformVector(FVector3f::OneVector);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutTranslateParam.SetAndAdvance(DefaultTranslate);
			OutRotateParam.SetAndAdvance(DefaultRotation);
			OutScaleParam.SetAndAdvance(DefaultScale);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// Section functions
void UNiagaraDataInterfaceStaticMesh::VMIsValidSection(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> SectionIndexParam(Context);
	FNDIOutputParam<bool> OutIsValid(Context);

	const int32 NumSections = StaticMeshHelper.LODResource ? StaticMeshHelper.LODResource->Sections.Num() : 0;
	for ( int32 i=0; i < Context.GetNumInstances(); ++i )
	{
		const int32 Section = SectionIndexParam.GetAndAdvance();
		OutIsValid.SetAndAdvance(Section >= 0 && Section < NumSections);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetSectionTriangleCount(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> SectionIndexParam(Context);
	FNDIOutputParam<int32> OutTriangleCount(Context);

	const int32 SectionMax = StaticMeshHelper.LODResource ? StaticMeshHelper.LODResource->Sections.Num() : 0;
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		const int32 Section = SectionIndexParam.GetAndAdvance();
		const bool ValidSection = Section >= 0 && Section < SectionMax;
		OutTriangleCount.SetAndAdvance(ValidSection ? StaticMeshHelper.LODResource->Sections[Section].NumTriangles : 0);
	}
}

template<typename TRandomHandler>
void UNiagaraDataInterfaceStaticMesh::VMRandomSectionTriangle(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	TRandomHandler RandHelper(Context);
	FNDIInputParam<int32> SectionIndexParam(Context);
	FNDIOutputParam<int32> OutTriangle(Context);
	FNDIOutputParam<FVector3f> OutBaryCoord(Context);

	const int32 SectionMax = StaticMeshHelper.LODResource ? StaticMeshHelper.LODResource->Sections.Num() - 1 : 0;
	if (SectionMax >= 0)
	{
		if ( StaticMeshHelper.IsCpuUniformlyDistributedSampling() )
		{
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				const int32 Section = FMath::Clamp(SectionIndexParam.GetAndAdvance(), 0, SectionMax);
				OutTriangle.SetAndAdvance(StaticMeshHelper.RandomSectionUniformTriangle(RandHelper, i, Section));
				OutBaryCoord.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
			}
		}
		else
		{
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				const int32 Section = FMath::Clamp(SectionIndexParam.GetAndAdvance(), 0, SectionMax);
				OutTriangle.SetAndAdvance(StaticMeshHelper.RandomSectionTriangle(RandHelper, i, Section));
				OutBaryCoord.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
			}
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			RandHelper.GetAndAdvance();
			OutTriangle.SetAndAdvance(0);
			OutBaryCoord.SetAndAdvance(RandHelper.RandomBarycentricCoord(i));
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetSectionTriangleAt(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> SectionIndexParam(Context);
	FNDIInputParam<int32> TriangleIndexParam(Context);
	FNDIOutputParam<int32> OutTriangle(Context);

	const int32 NumSections = StaticMeshHelper.LODResource ? StaticMeshHelper.LODResource->Sections.Num() : 0;
	if (NumSections >= 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Section = SectionIndexParam.GetAndAdvance();
			const int32 SectionTriangleIndex = TriangleIndexParam.GetAndAdvance();
			const bool SectionValid = Section >= 0 && Section < NumSections;
			const int32 SectionTriangleMax = SectionValid ? StaticMeshHelper.LODResource->Sections[Section].NumTriangles - 1 : 0;
			const int32 SectionTriangleOffset = SectionValid ? StaticMeshHelper.LODResource->Sections[Section].FirstIndex / 3 : 0;
			OutTriangle.SetAndAdvance(FMath::Clamp(SectionTriangleIndex, 0, SectionTriangleMax) + SectionTriangleOffset);
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutTriangle.SetAndAdvance(0);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetFilteredSectionAt(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> SectionIndexParam(Context);
	FNDIOutputParam<int32> OutSection(Context);

	TConstArrayView<int32> FilteredSections = StaticMeshHelper.InstanceData->GetFilteredSections();
	const int32 SectionMax = FilteredSections.Num() - 1;
	if ( SectionMax >= 0 )
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 SectionIndex = FMath::Clamp(SectionIndexParam.GetAndAdvance(), 0, SectionMax);
			OutSection.SetAndAdvance(FilteredSections[SectionIndex]);
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutSection.SetAndAdvance(0);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetUnfilteredSectionAt(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIInputParam<int32> SectionIndexParam(Context);
	FNDIOutputParam<int32> OutSection(Context);

	TConstArrayView<int32> UnfilteredSections = StaticMeshHelper.InstanceData->GetUnfilteredSections();
	const int32 SectionMax = UnfilteredSections.Num() - 1;
	if (SectionMax >= 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 SectionIndex = FMath::Clamp(SectionIndexParam.GetAndAdvance(), 0, SectionMax);
			OutSection.SetAndAdvance(UnfilteredSections[SectionIndex]);
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutSection.SetAndAdvance(0);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetSectionCount(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIOutputParam<int32> OutSectionCount(Context);

	const int32 NumSections = StaticMeshHelper.LODResource ? StaticMeshHelper.LODResource->Sections.Num() : 0;
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutSectionCount.SetAndAdvance(NumSections);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetFilteredSectionCount(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIOutputParam<int32> OutSectionCount(Context);

	const int32 NumSections = StaticMeshHelper.InstanceData->NumFilteredSections;
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutSectionCount.SetAndAdvance(NumSections);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetUnfilteredSectionCount(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIOutputParam<int32> OutSectionCount(Context);

	const int32 NumSections = StaticMeshHelper.InstanceData->NumUnfilteredSections;
	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutSectionCount.SetAndAdvance(NumSections);
	}
}

template<typename TRandomHandler>
void UNiagaraDataInterfaceStaticMesh::VMRandomSection(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	TRandomHandler RandHelper(Context);
	FNDIOutputParam<int32> OutSection(Context);

	if ( StaticMeshHelper.IsCpuUniformlyDistributedSampling() )
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			RandHelper.GetAndAdvance();
			OutSection.SetAndAdvance(StaticMeshHelper.RandomUniformSection(RandHelper, i));
		}
	}
	else
	{
		const int32 SectionMax = StaticMeshHelper.LODResource ? StaticMeshHelper.LODResource->Sections.Num() - 1 : 0;
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			RandHelper.GetAndAdvance();
			OutSection.SetAndAdvance(RandHelper.RandRange(i, 0, SectionMax));
		}
	}
}

template<typename TRandomHandler>
void UNiagaraDataInterfaceStaticMesh::VMRandomFilteredSection(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	TRandomHandler RandHelper(Context);
	FNDIOutputParam<int32> OutSection(Context);

	TConstArrayView<int32> FilteredSections = StaticMeshHelper.InstanceData->GetFilteredSections();
	const int32 SectionMax = FilteredSections.Num() - 1;
	if (SectionMax >= 0)
	{
		if ( StaticMeshHelper.IsCpuUniformlyDistributedSampling() )
		{
			TConstArrayView<NDIStaticMeshLocal::FNDISectionInfo> FilteredSectionInfos = StaticMeshHelper.InstanceData->GetFilteredSectionInfos();
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				const int32 Section = StaticMeshHelper.RandomUniformSection(RandHelper, i, FilteredSectionInfos, FilteredSections);
				OutSection.SetAndAdvance(FilteredSections[Section]);
			}
		}
		else
		{
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				OutSection.SetAndAdvance(FilteredSections[RandHelper.RandRange(i, 0, SectionMax)]);
			}
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			RandHelper.GetAndAdvance();
			OutSection.SetAndAdvance(0);
		}
	}
}

void UNiagaraDataInterfaceStaticMesh::VMRandomUnfilteredSection(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<FNDITransformHandlerNoop> StaticMeshHelper(Context);
	FNDIRandomHelper RandHelper(Context);
	FNDIOutputParam<int32> OutSection(Context);

	TConstArrayView<int32> UnfilteredSections = StaticMeshHelper.InstanceData->GetUnfilteredSections();
	const int32 SectionMax = UnfilteredSections.Num() - 1;
	if (SectionMax >= 0)
	{
		if (StaticMeshHelper.IsCpuUniformlyDistributedSampling())
		{
			TConstArrayView<NDIStaticMeshLocal::FNDISectionInfo> UnfilteredSectionInfos = StaticMeshHelper.InstanceData->GetUnfilteredSectionInfos();
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				const int32 Section = StaticMeshHelper.RandomUniformSection(RandHelper, i, UnfilteredSectionInfos, UnfilteredSections);
				OutSection.SetAndAdvance(UnfilteredSections[Section]);
			}
		}
		else
		{
			for (int32 i = 0; i < Context.GetNumInstances(); ++i)
			{
				RandHelper.GetAndAdvance();
				OutSection.SetAndAdvance(UnfilteredSections[RandHelper.RandRange(i, 0, SectionMax)]);
			}
		}
	}
	else
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			RandHelper.GetAndAdvance();
			OutSection.SetAndAdvance(0);
		}
	}
}

//////////////////////////////////////////////////////////////////////////
// VM Misc Functions
void UNiagaraDataInterfaceStaticMesh::VMIsValid(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<NDIStaticMeshLocal::FInstanceData_GameThread> InstanceData(Context);
	FNDIOutputParam<bool> OutValid(Context);

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutValid.SetAndAdvance(InstanceData->bMeshValid);
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetLocalToWorld(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<NDIStaticMeshLocal::FInstanceData_GameThread> InstanceData(Context);
	FNDIOutputParam<FMatrix44f> OutMatrix(Context);

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutMatrix.SetAndAdvance(FMatrix44f(InstanceData->Transform));						// LWC_TODO: Precision loss
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetLocalToWorldInverseTransposed(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<NDIStaticMeshLocal::FInstanceData_GameThread> InstanceData(Context);
	FNDIOutputParam<FMatrix44f> OutMatrix(Context);

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutMatrix.SetAndAdvance(FMatrix44f(InstanceData->TransformInverseTransposed));		// LWC_TODO: Precision loss
	}
}

void UNiagaraDataInterfaceStaticMesh::VMGetWorldVelocity(FVectorVMExternalFunctionContext& Context)
{
	VectorVM::FUserPtrHandler<NDIStaticMeshLocal::FInstanceData_GameThread> InstanceData(Context);
	FNDIOutputParam<FVector3f> OutVelocity(Context);

	for (int32 i = 0; i < Context.GetNumInstances(); ++i)
	{
		OutVelocity.SetAndAdvance((FVector3f)InstanceData->PhysicsVelocity);
	}
}

//////////////////////////////////////////////////////////////////////////
// Deprecated VM Functions
template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetVertexPosition_Deprecated(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> VertexParam(Context);
	FNDIOutputParam<FVector3f> OutPosition(Context);

	const int32 PositionsMax = StaticMeshHelper.GetNumPositionVertices() - 1;
	if ( PositionsMax >= 0 )
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Vertex = FMath::Clamp(VertexParam.GetAndAdvance(), 0, PositionsMax);
			const FVector3f Position = StaticMeshHelper.GetPosition(Vertex);
			OutPosition.SetAndAdvance(Position);
		}
	}
	else
	{
		const FVector3f Position = StaticMeshHelper.TransformPosition(FVector3f::ZeroVector);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutPosition.SetAndAdvance(Position);
		}
	}
}

template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetTriPosition_Deprecated(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIInputParam<FVector3f> BaryCoordParam(Context);
	FNDIOutputParam<FVector3f> OutPositionParam(Context);

	const FIndexArrayView IndexArray = StaticMeshHelper.GetIndexArrayView();
	const int32 TriangleMax = StaticMeshHelper.GetNumTriangles() - 1;
	if (TriangleMax >= 0 && StaticMeshHelper.GetNumPositionVertices() > 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Triangle = FMath::Clamp(TriangleParam.GetAndAdvance(), 0, TriangleMax);
			const FVector3f BaryCoord = BaryCoordParam.GetAndAdvance();
			const FVector3f Position = StaticMeshHelper.GetTrianglePosition(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			OutPositionParam.SetAndAdvance(Position);
		}
	}
	else
	{
		const FVector3f Position = StaticMeshHelper.TransformPosition(FVector3f::ZeroVector);
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutPositionParam.SetAndAdvance(Position);
		}
	}
}

template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetTriPositionAndVelocity_Deprecated(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIInputParam<FVector3f> BaryCoordParam(Context);
	FNDIOutputParam<FVector3f> OutPositionParam(Context);
	FNDIOutputParam<FVector3f> OutVelocityParam(Context);

	const FIndexArrayView IndexArray = StaticMeshHelper.GetIndexArrayView();
	const int32 TriangleMax = StaticMeshHelper.GetNumTriangles() - 1;
	if (TriangleMax >= 0 && StaticMeshHelper.GetNumPositionVertices() > 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Triangle = FMath::Clamp(TriangleParam.GetAndAdvance(), 0, TriangleMax);
			const FVector3f BaryCoord = BaryCoordParam.GetAndAdvance();
			const FVector3f LocalPosition = StaticMeshHelper.GetLocalTrianglePosition(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			const FVector3f Position = StaticMeshHelper.TransformPosition(LocalPosition);
			const FVector3f PreviousPosition = StaticMeshHelper.PreviousTransformPosition(LocalPosition);
			const FVector3f Velocity = (Position - PreviousPosition) * StaticMeshHelper.GetInvDeltaSeconds();
			OutPositionParam.SetAndAdvance(Position);
			OutVelocityParam.SetAndAdvance(Velocity);
		}
	}
	else
	{
		const FVector3f Position = StaticMeshHelper.TransformPosition(FVector3f::ZeroVector);
		const FVector3f PreviousPosition = StaticMeshHelper.PreviousTransformPosition(FVector3f::ZeroVector);
		const FVector3f Velocity = FVector3f::ZeroVector;
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutPositionParam.SetAndAdvance(Position);
			OutVelocityParam.SetAndAdvance(Velocity);
		}
	}
}

template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetTriangleTangentBasis_Deprecated(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIInputParam<FVector3f> BaryCoordParam(Context);
	FNDIOutputParam<FVector3f> OutTangent(Context);
	FNDIOutputParam<FVector3f> OutBitangent(Context);
	FNDIOutputParam<FVector3f> OutNormal(Context);

	const FIndexArrayView IndexArray = StaticMeshHelper.GetIndexArrayView();
	const int32 TriangleMax = StaticMeshHelper.GetNumTriangles() - 1;
	if (TriangleMax >= 0 && StaticMeshHelper.GetNumTangentVertices() > 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Triangle = FMath::Clamp(TriangleParam.GetAndAdvance(), 0, TriangleMax);
			const FVector3f BaryCoord = BaryCoordParam.GetAndAdvance();
			const FVector3f TangentX = StaticMeshHelper.GetTriangleTangentX(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			const FVector3f TangentY = StaticMeshHelper.GetTriangleTangentY(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			const FVector3f TangentZ = StaticMeshHelper.GetTriangleTangentZ(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			OutNormal.SetAndAdvance(TangentZ);
			OutBitangent.SetAndAdvance(TangentY);
			OutTangent.SetAndAdvance(TangentX);
		}
	}
	else
	{
		const FVector3f TangentX = StaticMeshHelper.TransformVector(FVector3f(1.0f, 0.0f, 0.0f));
		const FVector3f TangentY = StaticMeshHelper.TransformVector(FVector3f(0.0f, 1.0f, 0.0f));
		const FVector3f TangentZ = StaticMeshHelper.TransformVector(FVector3f(0.0f, 0.0f, 1.0f));
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutNormal.SetAndAdvance(TangentZ);
			OutBitangent.SetAndAdvance(TangentY);
			OutTangent.SetAndAdvance(TangentX);
		}
	}
}

template<typename TTransformHandler>
void UNiagaraDataInterfaceStaticMesh::VMGetTriangleNormal_Deprecated(FVectorVMExternalFunctionContext& Context)
{
	NDIStaticMeshLocal::FStaticMeshCpuHelper<TTransformHandler> StaticMeshHelper(Context);
	FNDIInputParam<int32> TriangleParam(Context);
	FNDIInputParam<FVector3f> BaryCoordParam(Context);
	FNDIOutputParam<FVector3f> OutNormal(Context);

	const FIndexArrayView IndexArray = StaticMeshHelper.GetIndexArrayView();
	const int32 TriangleMax = StaticMeshHelper.GetNumTriangles() - 1;
	if (TriangleMax >= 0 && StaticMeshHelper.GetNumTangentVertices() > 0)
	{
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			const int32 Triangle = FMath::Clamp(TriangleParam.GetAndAdvance(), 0, TriangleMax);
			const FVector3f BaryCoord = BaryCoordParam.GetAndAdvance();
			const FVector3f TangentZ = StaticMeshHelper.GetTriangleTangentZ(BaryCoord, IndexArray[Triangle * 3 + 0], IndexArray[Triangle * 3 + 1], IndexArray[Triangle * 3 + 2]);
			OutNormal.SetAndAdvance(TangentZ);
		}
	}
	else
	{
		const FVector3f TangentZ = StaticMeshHelper.TransformVector(FVector3f(0.0f, 0.0f, 1.0f));
		for (int32 i = 0; i < Context.GetNumInstances(); ++i)
		{
			OutNormal.SetAndAdvance(TangentZ);
		}
	}
}

#undef LOCTEXT_NAMESPACE
