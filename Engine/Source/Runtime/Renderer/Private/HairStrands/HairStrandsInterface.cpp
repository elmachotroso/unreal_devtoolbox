// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	HairStrandsInterface.h: Hair manager implementation.
=============================================================================*/

#include "HairStrandsInterface.h"
#include "HairStrandsRendering.h"
#include "HairStrandsMeshProjection.h"
#include "HairStrandsData.h"

#include "GPUSkinCache.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkinWeightVertexBuffer.h"
#include "CommonRenderResources.h"
#include "Components/SkeletalMeshComponent.h"
#include "SkeletalRenderPublic.h"
#include "SceneRendering.h"
#include "SystemTextures.h"
#include "ShaderPrint.h"
#include "ScenePrivate.h"

DEFINE_LOG_CATEGORY_STATIC(LogHairRendering, Log, All);

static TAutoConsoleVariable<int32> CVarHairStrandsRaytracingEnable(
	TEXT("r.HairStrands.Raytracing"), 1,
	TEXT("Enable/Disable hair strands raytracing geometry. This is anopt-in option per groom asset/groom instance."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static int32 GHairStrandsPluginEnable = 0;

static TAutoConsoleVariable<int32> CVarHairStrandsGlobalEnable(
	TEXT("r.HairStrands.Enable"), 1,
	TEXT("Enable/Disable the entire hair strands system. This affects all geometric representations (i.e., strands, cards, and meshes)."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarHairStrandsEnable(
	TEXT("r.HairStrands.Strands"), 1,
	TEXT("Enable/Disable hair strands rendering"),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarHairCardsEnable(
	TEXT("r.HairStrands.Cards"), 1,
	TEXT("Enable/Disable hair cards rendering. This variable needs to be turned on when the engine starts."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarHairMeshesEnable(
	TEXT("r.HairStrands.Meshes"), 1,
	TEXT("Enable/Disable hair meshes rendering. This variable needs to be turned on when the engine starts."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarHairStrandsBinding(
	TEXT("r.HairStrands.Binding"), 1,
	TEXT("Enable/Disable hair binding, i.e., hair attached to skeletal meshes."),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarHairStrandsSimulation(
	TEXT("r.HairStrands.Simulation"), 1,
	TEXT("Enable/disable hair simulation"),
	ECVF_RenderThreadSafe | ECVF_Scalability);

static TAutoConsoleVariable<int32> CVarHairStrandsNonVisibleShadowCasting(
	TEXT("r.HairStrands.Shadow.CastShadowWhenNonVisible"), 0,
	TEXT("Enable shadow casting for hair strands even when culled out from the primary view"),
	ECVF_RenderThreadSafe);

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Hair strands instance ref. counting for debug purpose only
uint32 FHairStrandsInstance::GetRefCount() const
{
	return RefCount;
}

uint32 FHairStrandsInstance::AddRef() const
{
	return ++RefCount;
}

uint32 FHairStrandsInstance::Release() const
{
	check(RefCount > 0);
	uint32 LocalRefCount = --RefCount;
	return LocalRefCount;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// Import/export utils function for hair resources
void FRDGExternalBuffer::Release()
{
	Buffer = nullptr;
	SRV = nullptr;
	UAV = nullptr;
}

FRDGImportedBuffer Register(FRDGBuilder& GraphBuilder, const FRDGExternalBuffer& In, ERDGImportedBufferFlags Flags, ERDGUnorderedAccessViewFlags UAVFlags)
{
	FRDGImportedBuffer Out;
	if (!In.Buffer)
	{
		return Out;
	}
	const uint32 uFlags = uint32(Flags);
	Out.Buffer = GraphBuilder.RegisterExternalBuffer(In.Buffer);
	if (In.Format != PF_Unknown)
	{
		if (uFlags & uint32(ERDGImportedBufferFlags::CreateSRV)) { Out.SRV = GraphBuilder.CreateSRV(Out.Buffer, In.Format); }
		if (uFlags & uint32(ERDGImportedBufferFlags::CreateUAV)) { Out.UAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Out.Buffer, In.Format), UAVFlags); }
	}
	else
	{
		if (uFlags & uint32(ERDGImportedBufferFlags::CreateSRV)) { Out.SRV = GraphBuilder.CreateSRV(Out.Buffer); }
		if (uFlags & uint32(ERDGImportedBufferFlags::CreateUAV)) { Out.UAV = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Out.Buffer),  UAVFlags); }
	}
	return Out;
}

FRDGBufferSRVRef RegisterAsSRV(FRDGBuilder& GraphBuilder, const FRDGExternalBuffer& In)
{
	if (!In.Buffer)
	{
		return nullptr;
	}

	FRDGBufferSRVRef Out = nullptr;
	FRDGBufferRef Buffer = GraphBuilder.RegisterExternalBuffer(In.Buffer);
	if (In.Format != PF_Unknown)
	{
		Out = GraphBuilder.CreateSRV(Buffer, In.Format);
	}
	else
	{
		Out = GraphBuilder.CreateSRV(Buffer);
	}
	return Out;
}

FRDGBufferUAVRef RegisterAsUAV(FRDGBuilder& GraphBuilder, const FRDGExternalBuffer& In, ERDGUnorderedAccessViewFlags Flags)
{
	if (!In.Buffer)
	{
		return nullptr;
	}

	FRDGBufferUAVRef Out = nullptr;
	FRDGBufferRef Buffer = GraphBuilder.RegisterExternalBuffer(In.Buffer);
	if (In.Format != PF_Unknown)
	{
		Out = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buffer, In.Format), Flags);
	}
	else
	{
		Out = GraphBuilder.CreateUAV(FRDGBufferUAVDesc(Buffer), Flags);
	}
	return Out;
}

bool IsHairRayTracingEnabled()
{
	if (GIsRHIInitialized && !IsRunningCookCommandlet())
	{
		return IsRayTracingEnabled() && CVarHairStrandsRaytracingEnable.GetValueOnAnyThread();
	}
	else
	{
		return false;
	}
}

bool IsHairStrandsSupported(EHairStrandsShaderType Type, EShaderPlatform Platform)
{
	if (GHairStrandsPluginEnable <= 0 || CVarHairStrandsGlobalEnable.GetValueOnAnyThread() <= 0) return false;

	// Important:
	// EHairStrandsShaderType::All: Mobile is excluded as we don't need any interpolation/simulation code for this. It only do rigid transformation. 
	//                              The runtime setting in these case are r.HairStrands.Binding=0 & r.HairStrands.Simulation=0
	const bool Cards_Meshes_All = true;
	const bool bIsMobile = IsMobilePlatform(Platform);

	switch (Type)
	{
	case EHairStrandsShaderType::Strands: return IsHairStrandsGeometrySupported(Platform);
	case EHairStrandsShaderType::Cards:	  return Cards_Meshes_All;
	case EHairStrandsShaderType::Meshes:  return Cards_Meshes_All;
	case EHairStrandsShaderType::Tool:	  return (IsD3DPlatform(Platform) || IsVulkanSM5Platform(Platform)) && IsPCPlatform(Platform) && IsFeatureLevelSupported(Platform, ERHIFeatureLevel::SM5);
	case EHairStrandsShaderType::All:	  return Cards_Meshes_All && !bIsMobile;
	}
	return false;
}

bool IsHairStrandsEnabled(EHairStrandsShaderType Type, EShaderPlatform Platform)
{
	const bool HairStrandsGlobalEnable = CVarHairStrandsGlobalEnable.GetValueOnAnyThread() > 0 && GHairStrandsPluginEnable > 0;
	if (!HairStrandsGlobalEnable) return false;

	// Important:
	// EHairStrandsShaderType::All: Mobile is excluded as we don't need any interpolation/simulation code for this. It only do rigid transformation. 
	//                              The runtime setting in these case are r.HairStrands.Binding=0 & r.HairStrands.Simulation=0
	const bool bIsMobile = Platform != EShaderPlatform::SP_NumPlatforms ? IsMobilePlatform(Platform) : false;
	const int32 HairStrandsEnable = CVarHairStrandsEnable.GetValueOnAnyThread();
	const int32 HairCardsEnable   = CVarHairCardsEnable.GetValueOnAnyThread();
	const int32 HairMeshesEnable  = CVarHairMeshesEnable.GetValueOnAnyThread();
	switch (Type)
	{
	case EHairStrandsShaderType::Strands:	return HairStrandsEnable > 0 && (Platform != EShaderPlatform::SP_NumPlatforms ? IsHairStrandsGeometrySupported(Platform) : true);
	case EHairStrandsShaderType::Cards:		return HairCardsEnable > 0;
	case EHairStrandsShaderType::Meshes:	return HairMeshesEnable > 0;
#if PLATFORM_DESKTOP && PLATFORM_WINDOWS
	case EHairStrandsShaderType::Tool:		return (HairCardsEnable > 0 || HairMeshesEnable > 0 || HairStrandsEnable > 0);
#else
	case EHairStrandsShaderType::Tool:		return false;
#endif
	case EHairStrandsShaderType::All :		return HairStrandsGlobalEnable && (HairCardsEnable > 0 || HairMeshesEnable > 0 || HairStrandsEnable > 0) && !bIsMobile;
	}
	return false;
}

void SetHairStrandsEnabled(bool In)
{
	GHairStrandsPluginEnable = In ? 1 : 0;
}

bool IsHairStrandsBindingEnable()
{
	return CVarHairStrandsBinding.GetValueOnAnyThread() > 0;
}

bool IsHairStrandsSimulationEnable()
{
	return CVarHairStrandsSimulation.GetValueOnAnyThread() > 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void ConvertToExternalBufferWithViews(FRDGBuilder& GraphBuilder, FRDGBufferRef& InBuffer, FRDGExternalBuffer& OutBuffer, EPixelFormat Format)
{
	OutBuffer.Buffer = GraphBuilder.ConvertToExternalBuffer(InBuffer);
	if (EnumHasAnyFlags(InBuffer->Desc.Usage, BUF_ShaderResource))
	{
		OutBuffer.SRV = OutBuffer.Buffer->GetOrCreateSRV(FRDGBufferSRVDesc(InBuffer, Format));
	}
	if (EnumHasAnyFlags(InBuffer->Desc.Usage, BUF_UnorderedAccess))
	{
		OutBuffer.UAV = OutBuffer.Buffer->GetOrCreateUAV(FRDGBufferUAVDesc(InBuffer, Format));
	}
	OutBuffer.Format = Format;
}

void InternalCreateIndirectBufferRDG(FRDGBuilder& GraphBuilder, FRDGExternalBuffer& Out, const TCHAR* DebugName)
{
	FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(4, 4);
	Desc.Usage |= BUF_DrawIndirect;
	FRDGBufferRef Buffer = GraphBuilder.CreateBuffer(Desc, DebugName);
	AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer, PF_R32_UINT), 0u);
	ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, PF_R32_UINT);
}

void InternalCreateVertexBufferRDG(FRDGBuilder& GraphBuilder, uint32 ElementSizeInBytes, uint32 ElementCount, EPixelFormat Format, FRDGExternalBuffer& Out, const TCHAR* DebugName, bool bClearFloat=false)
{
	FRDGBufferRef Buffer = nullptr;

	const uint32 DataCount = ElementCount;
	const uint32 DataSizeInBytes = ElementSizeInBytes * DataCount;
	if (DataSizeInBytes == 0)
	{
		Out.Buffer = nullptr;
		return;
	}

	// #hair_todo: Create this with a create+clear pass instead?
	const FRDGBufferDesc Desc = FRDGBufferDesc::CreateBufferDesc(ElementSizeInBytes, ElementCount);
	Buffer = GraphBuilder.CreateBuffer(Desc, DebugName, ERDGBufferFlags::MultiFrame);
	if (bClearFloat)
	{
		AddClearUAVFloatPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer, Format), 0.f);
	}
	else
	{
		AddClearUAVPass(GraphBuilder, GraphBuilder.CreateUAV(Buffer, Format), 0);
	}
	ConvertToExternalBufferWithViews(GraphBuilder, Buffer, Out, Format);
}

FHairGroupPublicData::FHairGroupPublicData(uint32 InGroupIndex)
{
	GroupIndex = InGroupIndex;
	GroupControlTriangleStripVertexCount = 0;
	ClusterCount = 0;
	VertexCount = 0;
}

void FHairGroupPublicData::SetClusters(uint32 InClusterCount, uint32 InVertexCount)
{
	GroupControlTriangleStripVertexCount = InVertexCount * 6; // 6 vertex per point for a quad
	ClusterCount = InClusterCount;
	VertexCount = InVertexCount; // Control points
}

void FHairGroupPublicData::InitRHI()
{
	if (bIsInitialized || GUsingNullRHI) { return; }

	// Resource are allocated on-demand
	#if 0
	FMemMark Mark(FMemStack::Get());
	FRHICommandListImmediate& RHICmdList = FRHICommandListExecutor::GetImmediateCommandList();
	FRDGBuilder GraphBuilder(RHICmdList);
	Allocate(GraphBuilder);
	GraphBuilder.Execute();
	#endif
}

void FHairGroupPublicData::Allocate(FRDGBuilder& GraphBuilder)
{
	if (bIsInitialized)
		return;

	if (ClusterCount == 0)
		return;

	bool bHasStrands = false;
	for (const EHairGeometryType& Type : LODGeometryTypes)
	{
		if (Type == EHairGeometryType::Strands)
		{
			bHasStrands = true;
			break;
		}
	}
	
	if (GUsingNullRHI || !bHasStrands) { return; }

	InternalCreateIndirectBufferRDG(GraphBuilder, DrawIndirectBuffer, TEXT("Hair.Cluster_DrawIndirectBuffer"));
	InternalCreateIndirectBufferRDG(GraphBuilder, DrawIndirectRasterComputeBuffer, TEXT("Hair.Cluster_DrawIndirectRasterComputeBuffer"));

	InternalCreateVertexBufferRDG(GraphBuilder, sizeof(int32), ClusterCount * 6, EPixelFormat::PF_R32_SINT, ClusterAABBBuffer, TEXT("Hair.Cluster_ClusterAABBBuffer"));
	InternalCreateVertexBufferRDG(GraphBuilder, sizeof(int32), 6, EPixelFormat::PF_R32_SINT, GroupAABBBuffer, TEXT("Hair.Cluster_GroupAABBBuffer"));

	InternalCreateVertexBufferRDG(GraphBuilder, sizeof(int32), VertexCount, EPixelFormat::PF_R32_UINT, CulledVertexIdBuffer, TEXT("Hair.Cluster_CulledVertexIdBuffer"));
	InternalCreateVertexBufferRDG(GraphBuilder, sizeof(float), VertexCount, EPixelFormat::PF_R32_FLOAT, CulledVertexRadiusScaleBuffer, TEXT("Hair.Cluster_CulledVertexRadiusScaleBuffer"), true);

	GraphBuilder.SetBufferAccessFinal(Register(GraphBuilder, DrawIndirectBuffer, ERDGImportedBufferFlags::None).Buffer, ERHIAccess::IndirectArgs);

	bIsInitialized = true;
}

void FHairGroupPublicData::ReleaseRHI()
{
	//Release();
}

void FHairGroupPublicData::Release()
{
	DrawIndirectBuffer.Release();
	DrawIndirectRasterComputeBuffer.Release();
	ClusterAABBBuffer.Release();
	GroupAABBBuffer.Release();
	CulledVertexIdBuffer.Release();
	CulledVertexRadiusScaleBuffer.Release();
	bIsInitialized = false;
}

uint32 FHairGroupPublicData::GetResourcesSize() const
{
	auto ExtractSize = [](const TRefCountPtr<FRDGPooledBuffer>& InBuffer)
	{
		return InBuffer ? InBuffer->Desc.BytesPerElement * InBuffer->Desc.NumElements : 0; 
	};

	uint32 Total = 0;
	Total += ExtractSize(DrawIndirectBuffer.Buffer);
	Total += ExtractSize(DrawIndirectRasterComputeBuffer.Buffer);
	Total += ExtractSize(ClusterAABBBuffer.Buffer);
	Total += ExtractSize(GroupAABBBuffer.Buffer);
	Total += ExtractSize(CulledVertexIdBuffer.Buffer);
	Total += ExtractSize(CulledVertexRadiusScaleBuffer.Buffer);
	return Total;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
void TransitBufferToReadable(FRDGBuilder& GraphBuilder, FBufferTransitionQueue& BuffersToTransit)
{
	if (BuffersToTransit.Num())
	{
		AddPass(GraphBuilder, RDG_EVENT_NAME("TransitionToSRV"), [LocalBuffersToTransit = MoveTemp(BuffersToTransit)](FRHICommandList& RHICmdList)
		{
			FMemMark Mark(FMemStack::Get());
			TArray<FRHITransitionInfo, TMemStackAllocator<>> Transitions;
			Transitions.Reserve(LocalBuffersToTransit.Num());
			for (FRHIUnorderedAccessView* UAV : LocalBuffersToTransit)
			{
				Transitions.Add(FRHITransitionInfo(UAV, ERHIAccess::Unknown, ERHIAccess::SRVMask));
			}
			RHICmdList.Transition(Transitions);
		});
	}
}

///////////////////////////////////////////////////////////////////////////////////////////////////

bool IsHairStrandsNonVisibleShadowCastingEnable()
{
	return CVarHairStrandsNonVisibleShadowCasting.GetValueOnAnyThread() > 0;
}

bool IsHairStrandsVisibleInShadows(const FViewInfo& View, const FHairStrandsInstance& Instance)
{
	bool bIsVisibleInShadow = false;
	if (const FHairGroupPublicData* HairData = Instance.GetHairData())
	{
		const int32 LODIndex = FMath::CeilToInt(HairData->LODIndex);
		const bool bIsStrands = LODIndex >= 0 && HairData->IsVisible(LODIndex) && HairData->GetGeometryType(LODIndex) == EHairGeometryType::Strands;
		if (bIsStrands)
		{
			if (const FBoxSphereBounds* Bounds = Instance.GetBounds())
			{
				{
					for (const FLightSceneInfo* LightInfo : View.HairStrandsViewData.VisibleShadowCastingLights)
					{
						// Influence radius check
						if (LightInfo->Proxy->AffectsBounds(*Bounds))
						{
							bIsVisibleInShadow = true;
							break;
						}
					}
				}

				if (!bIsVisibleInShadow)
				{
					for (const FSphere& LightBound : View.HairStrandsViewData.VisibleShadowCastingBounds)
					{
						// Influence radius check
						if (Bounds->GetSphere().Intersects(LightBound))
						{
							bIsVisibleInShadow = true;
							break;
						}
					}
				}
			}
		}
	}
	return bIsVisibleInShadow;
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Bookmark API
THairStrandsBookmarkFunction  GHairStrandsBookmarkFunction = nullptr;
void RegisterBookmarkFunction(THairStrandsBookmarkFunction Bookmark)
{
	if (Bookmark)
	{
		GHairStrandsBookmarkFunction = Bookmark;
	}
}

void RunHairStrandsBookmark(FRDGBuilder& GraphBuilder, EHairStrandsBookmark Bookmark, FHairStrandsBookmarkParameters& Parameters)
{
	if (GHairStrandsBookmarkFunction)
	{
		GHairStrandsBookmarkFunction(&GraphBuilder, Bookmark, Parameters);
	}
}

void RunHairStrandsBookmark(EHairStrandsBookmark Bookmark, FHairStrandsBookmarkParameters& Parameters)
{
	if (GHairStrandsBookmarkFunction)
	{
		GHairStrandsBookmarkFunction(nullptr, Bookmark, Parameters);
	}
}

FHairStrandsBookmarkParameters CreateHairStrandsBookmarkParameters(FScene* Scene, FViewInfo& View)
{
	const int32 ActiveInstanceCount = Scene->HairStrandsSceneData.RegisteredProxies.Num();
	TBitArray InstancesVisibility(false, ActiveInstanceCount);

	FHairStrandsBookmarkParameters Out;
	Out.VisibleInstances.Reserve(View.HairStrandsMeshElements.Num());

	// 1. Add all visible strands instances
	for (const FMeshBatchAndRelevance& MeshBatch : View.HairStrandsMeshElements)
	{
		check(MeshBatch.PrimitiveSceneProxy && MeshBatch.PrimitiveSceneProxy->ShouldRenderInMainPass());
		if (MeshBatch.Mesh && MeshBatch.Mesh->Elements.Num() > 0)
		{
			FHairGroupPublicData* HairData = HairStrands::GetHairData(MeshBatch.Mesh);
			if (HairData && HairData->Instance)
			{
				Out.VisibleInstances.Add(HairData->Instance);
				InstancesVisibility[HairData->Instance->RegisteredIndex] = true;
			}
		}
	}

	// 2. Add all visible cards instances
	for (const FMeshBatchAndRelevance& MeshBatch : View.HairCardsMeshElements)
	{
		check(MeshBatch.PrimitiveSceneProxy && MeshBatch.PrimitiveSceneProxy->ShouldRenderInMainPass());
		if (MeshBatch.Mesh && MeshBatch.Mesh->Elements.Num() > 0)
		{
			FHairGroupPublicData* HairData = HairStrands::GetHairData(MeshBatch.Mesh);
			if (HairData && HairData->Instance)
			{
				Out.VisibleInstances.Add(HairData->Instance);
				InstancesVisibility[HairData->Instance->RegisteredIndex] = true;
			}
		}
	}

	// 3. Add all instances non-visible primary view(s) but visible in shadow view(s)
	if (IsHairStrandsNonVisibleShadowCastingEnable())
	{
		for (FHairStrandsInstance* Instance : Scene->HairStrandsSceneData.RegisteredProxies)
		{
			if (Instance->RegisteredIndex >= 0 && Instance->RegisteredIndex < ActiveInstanceCount && !InstancesVisibility[Instance->RegisteredIndex])
			{
				if (IsHairStrandsVisibleInShadows(View, *Instance))
				{
					Out.VisibleInstances.Add(Instance);
				}
			}
		}
	}

	Out.ShaderDebugData			= ShaderDrawDebug::IsEnabled(View) ? &View.ShaderDrawData : nullptr;
	Out.ShaderPrintData			= ShaderPrint::IsEnabled(View) ? &View.ShaderPrintData : nullptr;
	Out.SkinCache				= View.Family->Scene->GetGPUSkinCache();
	Out.ShaderMap				= View.ShaderMap;
	Out.Instances				= &Scene->HairStrandsSceneData.RegisteredProxies;
	Out.View					= &View;
	Out.ViewRect				= View.ViewRect;
	Out.ViewUniqueID			= View.ViewState ? View.ViewState->UniqueID : ~0;
	Out.SceneColorTexture		= nullptr;
	Out.bHzbRequest				= false; // Out.HasInstances() && IsHairStrandsEnabled(EHairStrandsShaderType::Strands, View.GetShaderPlatform());

	// Sanity check
	check(Out.Instances->Num() >= Out.VisibleInstances.Num());

	return Out;
}

FHairStrandsBookmarkParameters CreateHairStrandsBookmarkParameters(FScene* Scene, TArray<FViewInfo>& Views)
{
	FHairStrandsBookmarkParameters Out;
	Out = CreateHairStrandsBookmarkParameters(Scene, Views[0]);
	Out.AllViews.Reserve(Views.Num());
	for (const FViewInfo& View : Views)
	{
		Out.AllViews.Add(&View);
	}

	return Out;
}

namespace HairStrands
{

bool IsHairStrandsVF(const FMeshBatch* Mesh)
{
	if (Mesh)
	{
		static const FHashedName& VFTypeRef = FVertexFactoryType::GetVFByName(TEXT("FHairStrandsVertexFactory"))->GetHashedName();
		const FHashedName& VFType = Mesh->VertexFactory->GetType()->GetHashedName();
		return VFType == VFTypeRef;
	}
	return false;
}

bool IsHairCardsVF(const FMeshBatch* Mesh)
{
	if (Mesh)
	{
		static const FHashedName& VFTypeRef = FVertexFactoryType::GetVFByName(TEXT("FHairCardsVertexFactory"))->GetHashedName();
		const FHashedName& VFType = Mesh->VertexFactory->GetType()->GetHashedName();
		return VFType == VFTypeRef;
	}
	return false;
}

bool IsHairCompatible(const FMeshBatch* Mesh)
{
	return IsHairStrandsVF(Mesh) || IsHairCardsVF(Mesh);
}

bool IsHairVisible(const FMeshBatchAndRelevance& MeshBatch)
{
	if (MeshBatch.Mesh && MeshBatch.PrimitiveSceneProxy && MeshBatch.PrimitiveSceneProxy->ShouldRenderInMainPass())
	{
		const FHairGroupPublicData* Data = HairStrands::GetHairData(MeshBatch.Mesh);
		switch (Data->VFInput.GeometryType)
		{
		case EHairGeometryType::Strands: return Data->VFInput.Strands.HairLengthScale > 0;
		case EHairGeometryType::Cards: return true;
		case EHairGeometryType::Meshes: return true;
		}
	}
	return false;
}

FHairGroupPublicData* GetHairData(const FMeshBatch* Mesh)
{
	return reinterpret_cast<FHairGroupPublicData*>(Mesh->Elements[0].VertexFactoryUserData);
}

void AddVisibleShadowCastingLight(const FScene& Scene, TArray<FViewInfo>& Views, const FLightSceneInfo* LightSceneInfo)
{
	for (FViewInfo& View : Views)
	{
		// If any hair data are registered, track which lights are visible so that hair strands can cast shadow even if not visibible in primary view
		if (Scene.HairStrandsSceneData.RegisteredProxies.Num() > 0)
		{
			View.HairStrandsViewData.VisibleShadowCastingLights.Add(LightSceneInfo);
			break;
		}
	}
}

void AddVisibleShadowCastingLight(const FScene& Scene, TArray<FViewInfo>& Views, const FSphere& Bounds)
{
	for (FViewInfo& View : Views)
	{
		// If any hair data are registered, track which lights are visible so that hair strands can cast shadow even if not visibible in primary view
		if (Scene.HairStrandsSceneData.RegisteredProxies.Num() > 0)
		{
			View.HairStrandsViewData.VisibleShadowCastingBounds.Add(Bounds);
			break;
		}
	}
}

} // namespace HairStrands
