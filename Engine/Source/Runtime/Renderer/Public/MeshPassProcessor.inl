// Copyright Epic Games, Inc. All Rights Reserved.

/*=============================================================================
MeshPassProcessor.inl:
=============================================================================*/

#pragma once

static EVRSShadingRate GetShadingRateFromMaterial(EMaterialShadingRate MaterialShadingRate)
{
	if (GRHISupportsPipelineVariableRateShading && GRHIVariableRateShadingEnabled)
	{
		switch (MaterialShadingRate)
		{
		case MSR_1x2:
			return EVRSShadingRate::VRSSR_1x2;
		case MSR_2x1:
			return EVRSShadingRate::VRSSR_2x1;
		case MSR_2x2:
			return EVRSShadingRate::VRSSR_2x2;
		case MSR_4x2:
			return EVRSShadingRate::VRSSR_4x2;
		case MSR_2x4:
			return EVRSShadingRate::VRSSR_2x4;						
		case MSR_4x4:
			return EVRSShadingRate::VRSSR_4x4;
		}
	}
	return EVRSShadingRate::VRSSR_1x1;
}

template<typename PassShadersType, typename ShaderElementDataType>
void FMeshPassProcessor::BuildMeshDrawCommands(
	const FMeshBatch& RESTRICT MeshBatch,
	uint64 BatchElementMask,
	const FPrimitiveSceneProxy* RESTRICT PrimitiveSceneProxy,
	const FMaterialRenderProxy& RESTRICT MaterialRenderProxy,
	const FMaterial& RESTRICT MaterialResource,
	const FMeshPassProcessorRenderState& RESTRICT DrawRenderState,
	PassShadersType PassShaders,
	ERasterizerFillMode MeshFillMode,
	ERasterizerCullMode MeshCullMode,
	FMeshDrawCommandSortKey SortKey,
	EMeshPassFeatures MeshPassFeatures,
	const ShaderElementDataType& ShaderElementData)
{
	const FVertexFactory* RESTRICT VertexFactory = MeshBatch.VertexFactory;
	const FPrimitiveSceneInfo* RESTRICT PrimitiveSceneInfo = PrimitiveSceneProxy ? PrimitiveSceneProxy->GetPrimitiveSceneInfo() : nullptr;

	FMeshDrawCommand SharedMeshDrawCommand;
	EFVisibleMeshDrawCommandFlags SharedFlags = EFVisibleMeshDrawCommandFlags::Default;

	if (MaterialResource.MaterialModifiesMeshPosition_RenderThread())
	{
		SharedFlags |= EFVisibleMeshDrawCommandFlags::MaterialMayModifyPosition;
	}

	SharedMeshDrawCommand.SetStencilRef(DrawRenderState.GetStencilRef());
	SharedMeshDrawCommand.PrimitiveType = (EPrimitiveType)MeshBatch.Type;

	FGraphicsMinimalPipelineStateInitializer PipelineState;
	PipelineState.PrimitiveType = (EPrimitiveType)MeshBatch.Type;
	PipelineState.ImmutableSamplerState = MaterialRenderProxy.ImmutableSamplerState;

	EVertexInputStreamType InputStreamType = EVertexInputStreamType::Default;
	if ((MeshPassFeatures & EMeshPassFeatures::PositionOnly) != EMeshPassFeatures::Default)				InputStreamType = EVertexInputStreamType::PositionOnly;
	if ((MeshPassFeatures & EMeshPassFeatures::PositionAndNormalOnly) != EMeshPassFeatures::Default)	InputStreamType = EVertexInputStreamType::PositionAndNormalOnly;

	check(VertexFactory && VertexFactory->IsInitialized());
	FRHIVertexDeclaration* VertexDeclaration = VertexFactory->GetDeclaration(InputStreamType);

	check(!VertexFactory->NeedsDeclaration() || VertexDeclaration);

	SharedMeshDrawCommand.SetShaders(VertexDeclaration, PassShaders.GetUntypedShaders(), PipelineState);

	PipelineState.RasterizerState = GetStaticRasterizerState<true>(MeshFillMode, MeshCullMode);

	check(DrawRenderState.GetDepthStencilState());
	check(DrawRenderState.GetBlendState());

	PipelineState.BlendState = DrawRenderState.GetBlendState();
	PipelineState.DepthStencilState = DrawRenderState.GetDepthStencilState();
	PipelineState.DrawShadingRate = GetShadingRateFromMaterial(MaterialResource.GetShadingRate());

	check(VertexFactory && VertexFactory->IsInitialized());
	VertexFactory->GetStreams(FeatureLevel, InputStreamType, SharedMeshDrawCommand.VertexStreams);

	SharedMeshDrawCommand.PrimitiveIdStreamIndex = VertexFactory->GetPrimitiveIdStreamIndex(FeatureLevel, InputStreamType);

	if (SharedMeshDrawCommand.PrimitiveIdStreamIndex != INDEX_NONE)
	{
		SharedFlags |= EFVisibleMeshDrawCommandFlags::HasPrimitiveIdStreamIndex;
	}

	int32 DataOffset = 0;
	if (PassShaders.VertexShader.IsValid())
	{
		FMeshDrawSingleShaderBindings ShaderBindings = SharedMeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Vertex, DataOffset);
		PassShaders.VertexShader->GetShaderBindings(Scene, FeatureLevel, PrimitiveSceneProxy, MaterialRenderProxy, MaterialResource, DrawRenderState, ShaderElementData, ShaderBindings);
	}

	if (PassShaders.PixelShader.IsValid())
	{
		FMeshDrawSingleShaderBindings ShaderBindings = SharedMeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Pixel, DataOffset);
		PassShaders.PixelShader->GetShaderBindings(Scene, FeatureLevel, PrimitiveSceneProxy, MaterialRenderProxy, MaterialResource, DrawRenderState, ShaderElementData, ShaderBindings);
	}

	if (PassShaders.GeometryShader.IsValid())
	{
		FMeshDrawSingleShaderBindings ShaderBindings = SharedMeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Geometry, DataOffset);
		PassShaders.GeometryShader->GetShaderBindings(Scene, FeatureLevel, PrimitiveSceneProxy, MaterialRenderProxy, MaterialResource, DrawRenderState, ShaderElementData, ShaderBindings);
	}

	SharedMeshDrawCommand.SetDebugData(PrimitiveSceneProxy, &MaterialResource, &MaterialRenderProxy, PassShaders.GetUntypedShaders(), VertexFactory);

	const int32 NumElements = ShouldSkipMeshDrawCommand(MeshBatch, PrimitiveSceneProxy) ? 0 : MeshBatch.Elements.Num();

	for (int32 BatchElementIndex = 0; BatchElementIndex < NumElements; BatchElementIndex++)
	{
		if ((1ull << BatchElementIndex) & BatchElementMask)
		{
			const FMeshBatchElement& BatchElement = MeshBatch.Elements[BatchElementIndex];
			FMeshDrawCommand& MeshDrawCommand = DrawListContext->AddCommand(SharedMeshDrawCommand, NumElements);
			
			EFVisibleMeshDrawCommandFlags Flags = SharedFlags;
			if (BatchElement.bForceInstanceCulling)
			{
				Flags |= EFVisibleMeshDrawCommandFlags::ForceInstanceCulling;
			}
			if (BatchElement.bPreserveInstanceOrder)
			{
				// TODO: add support for bPreserveInstanceOrder on mobile
				if (ensureMsgf(FeatureLevel > ERHIFeatureLevel::ES3_1, TEXT("FMeshBatchElement::bPreserveInstanceOrder is currently only supported on non-mobile platforms.")))
				{					
					Flags |= EFVisibleMeshDrawCommandFlags::PreserveInstanceOrder;
				}
			}

			DataOffset = 0;
			if (PassShaders.VertexShader.IsValid())
			{
				FMeshDrawSingleShaderBindings VertexShaderBindings = MeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Vertex, DataOffset);
				FMeshMaterialShader::GetElementShaderBindings(PassShaders.VertexShader, Scene, ViewIfDynamicMeshCommand, VertexFactory, InputStreamType, FeatureLevel, PrimitiveSceneProxy, MeshBatch, BatchElement, ShaderElementData, VertexShaderBindings, MeshDrawCommand.VertexStreams);
			}

			if (PassShaders.PixelShader.IsValid())
			{
				FMeshDrawSingleShaderBindings PixelShaderBindings = MeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Pixel, DataOffset);
				FMeshMaterialShader::GetElementShaderBindings(PassShaders.PixelShader, Scene, ViewIfDynamicMeshCommand, VertexFactory, EVertexInputStreamType::Default, FeatureLevel, PrimitiveSceneProxy, MeshBatch, BatchElement, ShaderElementData, PixelShaderBindings, MeshDrawCommand.VertexStreams);
			}


			if (PassShaders.GeometryShader.IsValid())
			{
				FMeshDrawSingleShaderBindings GeometryShaderBindings = MeshDrawCommand.ShaderBindings.GetSingleShaderBindings(SF_Geometry, DataOffset);
				FMeshMaterialShader::GetElementShaderBindings(PassShaders.GeometryShader, Scene, ViewIfDynamicMeshCommand, VertexFactory, EVertexInputStreamType::Default, FeatureLevel, PrimitiveSceneProxy, MeshBatch, BatchElement, ShaderElementData, GeometryShaderBindings, MeshDrawCommand.VertexStreams);
			}

			FMeshDrawCommandPrimitiveIdInfo IdInfo = GetDrawCommandPrimitiveId(PrimitiveSceneInfo, BatchElement);

			FMeshProcessorShaders ShadersForDebugging = PassShaders.GetUntypedShaders();
			DrawListContext->FinalizeCommand(MeshBatch, BatchElementIndex, IdInfo, MeshFillMode, MeshCullMode, SortKey, Flags, PipelineState, &ShadersForDebugging, MeshDrawCommand);
		}
	}
}

/**
* Provides a callback to build FMeshDrawCommands and then submits them immediately.  Useful for legacy / editor code paths.
* Does many dynamic allocations - do not use for game rendering.
*/
template<typename LambdaType>
void DrawDynamicMeshPass(const FSceneView& View, FRHICommandList& RHICmdList, const LambdaType& BuildPassProcessorLambda, bool bForceStereoInstancingOff = false)
{
	FDynamicMeshDrawCommandStorage DynamicMeshDrawCommandStorage;
	FMeshCommandOneFrameArray VisibleMeshDrawCommands;
	FGraphicsMinimalPipelineStateSet GraphicsMinimalPipelineStateSet;
	bool NeedsShaderInitialisation;

	FDynamicPassMeshDrawListContext DynamicMeshPassContext(DynamicMeshDrawCommandStorage, VisibleMeshDrawCommands, GraphicsMinimalPipelineStateSet, NeedsShaderInitialisation);

	BuildPassProcessorLambda(&DynamicMeshPassContext);

	// We assume all dynamic passes are in stereo if it is enabled in the view, so we apply ISR to them
	const uint32 InstanceFactor = (!bForceStereoInstancingOff && View.IsInstancedStereoPass()) ? 2 : 1;
	DrawDynamicMeshPassPrivate(View, RHICmdList, VisibleMeshDrawCommands, DynamicMeshDrawCommandStorage, GraphicsMinimalPipelineStateSet, NeedsShaderInitialisation, InstanceFactor);
}
