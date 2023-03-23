// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
	SimpleMeshDrawCommandPass.cpp: 
=============================================================================*/

#include "SimpleMeshDrawCommandPass.h"
#include "ScenePrivate.h"

// 5.0.2 deadend workaround to avoid changing public headers: InstanceFactor encodes the flag bWasDrawCommandsSetup in the high bit
constexpr uint32 kWasDrawCommandsSetUpFlag = 1U << 31U;

FSimpleMeshDrawCommandPass::FSimpleMeshDrawCommandPass(const FSceneView& View, FInstanceCullingManager* InstanceCullingManager, bool bEnableStereo) :
	DynamicPassMeshDrawListContext(DynamicMeshDrawCommandStorage, VisibleMeshDrawCommands, GraphicsMinimalPipelineStateSet, bNeedsInitialization)
{
	check(View.bIsViewInfo);
	const FViewInfo* ViewInfo = static_cast<const FViewInfo*>(&View);
	
	TArray<int32, TFixedAllocator<2> > ViewIds;
	ViewIds.Add(ViewInfo->GPUSceneViewId);
	bUsingStereo = bEnableStereo && ViewInfo->bIsInstancedStereoEnabled && !View.bIsMultiViewEnabled && IStereoRendering::IsStereoEyeView(View);
	if (bUsingStereo)
	{
		check(ViewInfo->GetInstancedView() != nullptr);
		ViewIds.Add(ViewInfo->GetInstancedView()->GPUSceneViewId);
	}

	ERHIFeatureLevel::Type FeatureLevel = ViewInfo->GetFeatureLevel();
	InstanceCullingContext = FInstanceCullingContext(FeatureLevel, InstanceCullingManager, ViewIds, nullptr, bUsingStereo ? EInstanceCullingMode::Stereo : EInstanceCullingMode::Normal);

	// 5.0.2 deadend workaround to avoid changing public headers: InstanceFactor encodes the flag bWasDrawCommandsSetup in the high bit, which is cleared here
	InstanceFactor = static_cast<uint32>(ViewIds.Num());
}

void FSimpleMeshDrawCommandPass::BuildRenderingCommands(FRDGBuilder& GraphBuilder, const FSceneView& View, const FGPUScene& GPUScene, FInstanceCullingDrawParams& OutInstanceCullingDrawParams)
{
	// NOTE: Everything up to InstanceCullingContext.BuildRenderingCommands could be peeled off into an async task.
	ApplyViewOverridesToMeshDrawCommands(View, VisibleMeshDrawCommands, DynamicMeshDrawCommandStorage, GraphicsMinimalPipelineStateSet, bNeedsInitialization);

	// 5.0.2 deadend workaround to avoid changing public headers: InstanceFactor encodes the flag bWasDrawCommandsSetup in the high bit, which is cleared here
	const bool bWasDrawCommandsSetup = (InstanceFactor & kWasDrawCommandsSetUpFlag) != 0U;

	FInstanceCullingResult InstanceCullingResult;
	VisibleMeshDrawCommands.Sort(FCompareFMeshDrawCommands());
	if (GPUScene.IsEnabled())
	{
		int32 MaxInstances = 0;
		int32 VisibleMeshDrawCommandsNum = 0;
		int32 NewPassVisibleMeshDrawCommandsNum = 0;

		// 1. Run draw command setup, but only the first time.
		if (!bWasDrawCommandsSetup)
		{
			InstanceCullingContext.SetupDrawCommands(VisibleMeshDrawCommands, true, MaxInstances, VisibleMeshDrawCommandsNum, NewPassVisibleMeshDrawCommandsNum);

			// 5.0.2 deadend workaround to avoid changing public headers: InstanceFactor encodes the flag bWasDrawCommandsSetup in the high bit, which is cleared here
			InstanceFactor |= kWasDrawCommandsSetUpFlag;
		}

		// 2. Run finalize culling commands pass
		check(View.bIsViewInfo);
		const FViewInfo* ViewInfo = static_cast<const FViewInfo*>(&View);
		InstanceCullingContext.BuildRenderingCommands(GraphBuilder, GPUScene, ViewInfo->DynamicPrimitiveCollector.GetInstanceSceneDataOffset(), ViewInfo->DynamicPrimitiveCollector.NumInstances(), InstanceCullingResult, &OutInstanceCullingDrawParams);
		
		// Signal that scene primitives are supported, used for validation, the existence of a valid InstanceCullingResult is the required signal
		bSupportsScenePrimitives = true;
	}

	InstanceCullingResult.GetDrawParameters(OutInstanceCullingDrawParams);
}

void FSimpleMeshDrawCommandPass::BuildRenderingCommands(FRDGBuilder& GraphBuilder, const FSceneView& View, const FScene& Scene, FInstanceCullingDrawParams& OutInstanceCullingDrawParams)
{
	BuildRenderingCommands(GraphBuilder, View, Scene.GPUScene, OutInstanceCullingDrawParams);
}

void FSimpleMeshDrawCommandPass::SubmitDraw(FRHICommandList& RHICmdList, const FInstanceCullingDrawParams& InstanceCullingDrawParams) const
{
	if (VisibleMeshDrawCommands.Num() > 0)
	{
		// 5.0.2 deadend workaround to avoid changing public headers: InstanceFactor encodes the flag bWasDrawCommandsSetup in the high bit, which is cleared here
		uint32 ActualInstanceFactor = InstanceFactor & (~kWasDrawCommandsSetUpFlag);

		if (bSupportsScenePrimitives)
		{
			InstanceCullingContext.SubmitDrawCommands(
				VisibleMeshDrawCommands,
				GraphicsMinimalPipelineStateSet,
				GetMeshDrawCommandOverrideArgs(InstanceCullingDrawParams),
				0,
				VisibleMeshDrawCommands.Num(),
				ActualInstanceFactor,
				RHICmdList);
		}
		else
		{
			const uint32 PrimitiveIdBufferStride = FInstanceCullingContext::GetInstanceIdBufferStride(InstanceCullingContext.FeatureLevel);
			SubmitMeshDrawCommandsRange(VisibleMeshDrawCommands, GraphicsMinimalPipelineStateSet, PrimitiveIdVertexBuffer, PrimitiveIdBufferStride, 0, false, 0, VisibleMeshDrawCommands.Num(), ActualInstanceFactor, RHICmdList);
		}
	}
}
