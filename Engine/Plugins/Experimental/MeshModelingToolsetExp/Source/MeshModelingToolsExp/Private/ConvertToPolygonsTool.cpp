// Copyright Epic Games, Inc. All Rights Reserved.

#include "ConvertToPolygonsTool.h"
#include "InteractiveToolManager.h"
#include "ToolBuilderUtil.h"
#include "ToolSetupUtil.h"
#include "ModelingToolTargetUtil.h"
#include "Drawing/PreviewGeometryActor.h"
#include "PreviewMesh.h"

#include "MeshOpPreviewHelpers.h"
#include "ModelingOperators.h"

#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "DynamicMeshEditor.h"
#include "MeshRegionBoundaryLoops.h"
#include "Util/ColorConstants.h"
#include "Polygroups/PolygroupUtil.h"
#include "Util/ColorConstants.h"

#include "Polygroups/PolygroupsGenerator.h"

using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "UConvertToPolygonsTool"

/*
 * ToolBuilder
 */

USingleSelectionMeshEditingTool* UConvertToPolygonsToolBuilder::CreateNewTool(const FToolBuilderState& SceneState) const
{
	return NewObject<UConvertToPolygonsTool>(SceneState.ToolManager);
}

class FConvertToPolygonsOp : public  FDynamicMeshOperator
{
public:
	// parameters set by the tool
	EConvertToPolygonsMode ConversionMode = EConvertToPolygonsMode::FaceNormalDeviation;
	double AngleTolerance = 0.1f;
	int32 NumPoints = 10;
	bool bSubdivideExisting = false;
	FPolygroupsGenerator::EWeightingType WeightingType = FPolygroupsGenerator::EWeightingType::None;
	FVector3d WeightingCoeffs = FVector3d::One();
	int32 MinGroupSize = 2;
	bool bCalculateNormals = false;

	// input mesh
	TSharedPtr<FDynamicMesh3, ESPMode::ThreadSafe> OriginalMesh;
	// result
	FPolygroupsGenerator Generator;

	virtual void CalculateResult(FProgressCancel* Progress) override
	{
		if ((Progress && Progress->Cancelled()) || !OriginalMesh)
		{
			return;
		}

		ResultMesh->Copy(*OriginalMesh, true, true, true, true);
	

		if (Progress && Progress->Cancelled())
		{
			return;
		}

		Generator = FPolygroupsGenerator(ResultMesh.Get());
		Generator.MinGroupSize = MinGroupSize;

		switch (ConversionMode)
		{
		case EConvertToPolygonsMode::FromUVIslands:
		{
			Generator.FindPolygroupsFromUVIslands();
			break;
		}
		case EConvertToPolygonsMode::FromNormalSeams:
		{
			Generator.FindPolygroupsFromHardNormalSeams();
			break;
		}
		case EConvertToPolygonsMode::FromConnectedTris:
		{
			Generator.FindPolygroupsFromConnectedTris();
			break;
		}
		case EConvertToPolygonsMode::FaceNormalDeviation:
		{
			double DotTolerance = 1.0 - FMathd::Cos(AngleTolerance * FMathd::DegToRad);
			Generator.FindPolygroupsFromFaceNormals(DotTolerance);
			break;
		}
		case EConvertToPolygonsMode::FromFurthestPointSampling:
		{
			if (bSubdivideExisting)
			{
				FPolygroupSet InputGroups(OriginalMesh.Get());
				Generator.FindPolygroupsFromFurthestPointSampling(NumPoints, WeightingType, WeightingCoeffs, &InputGroups);
			}
			else
			{
				Generator.FindPolygroupsFromFurthestPointSampling(NumPoints, WeightingType, WeightingCoeffs, nullptr);
			}
			break;
		}
		default:
			check(0);
		}

		Generator.FindPolygroupEdges();

		if (bCalculateNormals && ConversionMode == EConvertToPolygonsMode::FaceNormalDeviation)
		{
			if (ResultMesh->HasAttributes() == false)
			{
				ResultMesh->EnableAttributes();
			}

			FDynamicMeshNormalOverlay* NormalOverlay = ResultMesh->Attributes()->PrimaryNormals();
			NormalOverlay->ClearElements();

			FDynamicMeshEditor Editor(ResultMesh.Get());
			for (const TArray<int>& Polygon : Generator.FoundPolygroups)
			{
				FVector3f Normal = (FVector3f)ResultMesh->GetTriNormal(Polygon[0]);
				Editor.SetTriangleNormals(Polygon, Normal);
			}

			FMeshNormals Normals(ResultMesh.Get());
			Normals.RecomputeOverlayNormals(ResultMesh->Attributes()->PrimaryNormals());
			Normals.CopyToOverlay(NormalOverlay, false);
		}

	}

	void SetTransform(const FTransformSRT3d& Transform)
	{
		ResultTransform = Transform;
	}


};

TUniquePtr<FDynamicMeshOperator> UConvertToPolygonsOperatorFactory::MakeNewOperator()
{
	// backpointer used to populate parameters.
	check(ConvertToPolygonsTool);

	// Create the actual operator type based on the requested operation
	TUniquePtr<FConvertToPolygonsOp>  MeshOp = MakeUnique<FConvertToPolygonsOp>();

	// Operator runs on another thread - copy data over that it needs.
	ConvertToPolygonsTool->UpdateOpParameters(*MeshOp);

	// give the operator
	return MeshOp;
}

/*
 * Tool
 */
UConvertToPolygonsTool::UConvertToPolygonsTool()
{
	SetToolDisplayName(LOCTEXT("ConvertToPolygonsToolName", "Generate PolyGroups"));
}

bool UConvertToPolygonsTool::CanAccept() const
{
	return Super::CanAccept() && (PreviewCompute == nullptr || PreviewCompute->HaveValidResult());
}

void UConvertToPolygonsTool::Setup()
{
	UInteractiveTool::Setup();

	FComponentMaterialSet MaterialSet = UE::ToolTarget::GetMaterialSet(Target);

	OriginalDynamicMesh = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>(UE::ToolTarget::GetDynamicMeshCopy(Target));

	Settings = NewObject<UConvertToPolygonsToolProperties>(this);
	Settings->RestoreProperties(this);
	AddToolPropertySource(Settings);
	FTransform MeshTransform = (FTransform)UE::ToolTarget::GetLocalToWorldTransform(Target);
	UE::ToolTarget::HideSourceObject(Target);

	{
		// create the operator factory
		UConvertToPolygonsOperatorFactory* ConvertToPolygonsOperatorFactory = NewObject<UConvertToPolygonsOperatorFactory>(this);
		ConvertToPolygonsOperatorFactory->ConvertToPolygonsTool = this; // set the back pointer

		PreviewCompute = NewObject<UMeshOpPreviewWithBackgroundCompute>(ConvertToPolygonsOperatorFactory);
		PreviewCompute->Setup(GetTargetWorld(), ConvertToPolygonsOperatorFactory);
		ToolSetupUtil::ApplyRenderingConfigurationToPreview(PreviewCompute->PreviewMesh, Target);
		PreviewCompute->SetIsMeshTopologyConstant(true, EMeshRenderAttributeFlags::Positions | EMeshRenderAttributeFlags::VertexNormals);

		// Give the preview something to display
		PreviewCompute->PreviewMesh->SetTransform(MeshTransform);
		PreviewCompute->PreviewMesh->SetTangentsMode(EDynamicMeshComponentTangentsMode::AutoCalculated);
		PreviewCompute->PreviewMesh->UpdatePreview(OriginalDynamicMesh.Get());
		
		PreviewCompute->ConfigureMaterials(MaterialSet.Materials,
			ToolSetupUtil::GetDefaultWorkingMaterial(GetToolManager())
		);

		// show the preview mesh
		PreviewCompute->SetVisibility(true);

		// something to capture the polygons from the async task when it is done
		PreviewCompute->OnOpCompleted.AddLambda([this](const FDynamicMeshOperator* MeshOp) 
		{ 
			const FConvertToPolygonsOp*  ConvertToPolygonsOp = static_cast<const FConvertToPolygonsOp*>(MeshOp);
			this->PolygonEdges = ConvertToPolygonsOp->Generator.PolygroupEdges;
			UpdateVisualization();
		});

		// start the compute
		PreviewCompute->InvalidateResult();
	}
	
	PreviewGeometry = NewObject<UPreviewGeometry>(this);
	PreviewGeometry->CreateInWorld(GetTargetWorld(), MeshTransform);

	// updates the triangle color visualization
	UpdateVisualization();

	Settings->WatchProperty(Settings->ConversionMode, [this](EConvertToPolygonsMode) { OnSettingsModified(); });
	Settings->WatchProperty(Settings->bShowGroupColors, [this](bool) { UpdateVisualization(); });
	Settings->WatchProperty(Settings->AngleTolerance, [this](float) { OnSettingsModified(); });
	Settings->WatchProperty(Settings->NumPoints, [this](int32) { OnSettingsModified(); });
	Settings->WatchProperty(Settings->bSplitExisting, [this](bool) { OnSettingsModified(); });
	Settings->WatchProperty(Settings->bNormalWeighted, [this](bool) { OnSettingsModified(); });
	Settings->WatchProperty(Settings->NormalWeighting, [this](float) { OnSettingsModified(); });
	Settings->WatchProperty(Settings->MinGroupSize, [this](int32) { OnSettingsModified(); });
	

	GetToolManager()->DisplayMessage(
		LOCTEXT("OnStartTool", "Cluster triangles of the Mesh into PolyGroups using various strategies"),
		EToolMessageLevel::UserNotification);
}

void UConvertToPolygonsTool::UpdateOpParameters(FConvertToPolygonsOp& ConvertToPolygonsOp) const
{
	ConvertToPolygonsOp.bCalculateNormals = Settings->bCalculateNormals;
	ConvertToPolygonsOp.ConversionMode    = Settings->ConversionMode;
	ConvertToPolygonsOp.AngleTolerance    = Settings->AngleTolerance;
	ConvertToPolygonsOp.NumPoints = Settings->NumPoints;
	ConvertToPolygonsOp.bSubdivideExisting = Settings->bSplitExisting;
	ConvertToPolygonsOp.WeightingType = (Settings->bNormalWeighted) ? FPolygroupsGenerator::EWeightingType::NormalDeviation : FPolygroupsGenerator::EWeightingType::None;
	ConvertToPolygonsOp.WeightingCoeffs = FVector3d(Settings->NormalWeighting, 1.0, 1.0);
	ConvertToPolygonsOp.MinGroupSize = Settings->MinGroupSize;
	ConvertToPolygonsOp.OriginalMesh = OriginalDynamicMesh;
	
	FTransform LocalToWorld = (FTransform)UE::ToolTarget::GetLocalToWorldTransform(Target);
	ConvertToPolygonsOp.SetTransform(LocalToWorld);
}



void UConvertToPolygonsTool::OnShutdown(EToolShutdownType ShutdownType)
{
	Settings->SaveProperties(this);
	UE::ToolTarget::ShowSourceObject(Target);

	PreviewGeometry->Disconnect();
	PreviewGeometry = nullptr;

	if (PreviewCompute)
	{
		FDynamicMeshOpResult Result = PreviewCompute->Shutdown();
		if (ShutdownType == EToolShutdownType::Accept)
		{
			GetToolManager()->BeginUndoTransaction(LOCTEXT("ConvertToPolygonsToolTransactionName", "Find Polygroups"));
			FDynamicMesh3* DynamicMeshResult = Result.Mesh.Get();
			if (ensure(DynamicMeshResult != nullptr))
			{
				// todo: have not actually modified topology here, but groups-only update is not supported yet
				UE::ToolTarget::CommitDynamicMeshUpdate(Target, *DynamicMeshResult, true);
			}
			GetToolManager()->EndUndoTransaction();
		}
	}
}

void UConvertToPolygonsTool::OnSettingsModified()
{
	PreviewCompute->InvalidateResult();
}


void UConvertToPolygonsTool::OnTick(float DeltaTime)
{
	PreviewCompute->Tick(DeltaTime);
}




void UConvertToPolygonsTool::UpdateVisualization()
{
	if (!PreviewCompute)
	{
		return;
	}

	IMaterialProvider* MaterialTarget = Cast<IMaterialProvider>(Target);
	FComponentMaterialSet MaterialSet;
	if (Settings->bShowGroupColors)
	{
		int32 NumMaterials = MaterialTarget->GetNumMaterials();
		for (int32 i = 0; i < NumMaterials; ++i)
		{ 
			MaterialSet.Materials.Add(ToolSetupUtil::GetSelectionMaterial(GetToolManager()));
		}
		PreviewCompute->PreviewMesh->SetTriangleColorFunction([this](const FDynamicMesh3* Mesh, int TriangleID)
		{
			return LinearColors::SelectFColor(Mesh->GetTriangleGroup(TriangleID));
		}, 
		UPreviewMesh::ERenderUpdateMode::FastUpdate);
	}
	else
	{
		MaterialTarget->GetMaterialSet(MaterialSet);
		PreviewCompute->PreviewMesh->ClearTriangleColorFunction(UPreviewMesh::ERenderUpdateMode::FastUpdate);
	}
	PreviewCompute->ConfigureMaterials(MaterialSet.Materials,
		ToolSetupUtil::GetDefaultWorkingMaterial(GetToolManager()));

	FColor GroupLineColor = FColor::Red;
	float GroupLineThickness = 2.0f;

	PreviewGeometry->CreateOrUpdateLineSet(TEXT("GroupBorders"), PolygonEdges.Num(), 
		[&](int32 k, TArray<FRenderableLine>& LinesOut) {
			FVector3d A, B;
			OriginalDynamicMesh->GetEdgeV(PolygonEdges[k], A, B);
			LinesOut.Add(FRenderableLine(A, B, GroupLineColor, GroupLineThickness));
		}, 1);

}



#undef LOCTEXT_NAMESPACE
