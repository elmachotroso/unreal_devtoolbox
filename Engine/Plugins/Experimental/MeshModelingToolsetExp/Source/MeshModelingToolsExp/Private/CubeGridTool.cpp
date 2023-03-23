// Copyright Epic Games, Inc. All Rights Reserved.

#include "CubeGridTool.h"

#include "BaseBehaviors/ClickDragBehavior.h"
#include "BaseBehaviors/SingleClickBehavior.h"
#include "BaseBehaviors/MouseHoverBehavior.h"
#include "BaseGizmos/GizmoMath.h"
#include "BaseGizmos/TransformGizmoUtil.h"
#include "BaseGizmos/TransformProxy.h"
#include "CompositionOps/CubeGridBooleanOp.h"
#include "Distance/DistLine3Ray3.h"
#include "Drawing/PreviewGeometryActor.h"
#include "Drawing/LineSetComponent.h"
#include "DynamicMesh/DynamicMeshChangeTracker.h"
#include "DynamicMesh/MeshTransforms.h"
#include "DynamicMeshToMeshDescription.h"
#include "InteractiveToolChange.h"
#include "InteractiveToolManager.h"
#include "InputState.h"
#include "Mechanics/DragAlignmentMechanic.h"
#include "MeshOpPreviewHelpers.h" //UMeshOpPreviewWithBackgroundCompute
#include "MeshDescriptionToDynamicMesh.h"
#include "ModelingObjectsCreationAPI.h"
#include "ModelingToolTargetUtil.h"
#include "Properties/MeshMaterialProperties.h"
#include "PropertySets/CreateMeshObjectTypeProperties.h"
#include "Selection/ToolSelectionUtil.h"
#include "ToolContextInterfaces.h"
#include "ToolTargetManager.h"
#include "ToolTargets/ToolTarget.h"
#include "TargetInterfaces/MeshDescriptionCommitter.h"
#include "TargetInterfaces/MeshDescriptionProvider.h"
#include "TargetInterfaces/PrimitiveComponentBackedTarget.h"
#include "ToolSceneQueriesUtil.h"
#include "ToolSetupUtil.h"

using namespace UE::Geometry;

#define LOCTEXT_NAMESPACE "UCubeGridTool"

// General note: This tool used to largely operate in cube grid space. However it turned out
// to be valuable to be able to keep a selection more or less constant while resizing the grid,
// and this was easier to do by generally operating in just the frame space of the grid (not
// scaled into the grid size).

namespace CubeGridToolLocals
{
	FText PushPullModeMessage = LOCTEXT("CubeGridPushPullModeDescription", "Select grid cells and push/pull them to create geometry. "
		"Refer to side panel for shortcuts.");

	FText CornerModeMessage = LOCTEXT("CubeGridCornerModeDescription", "Toggle corner selection for push/pulling by clicking or dragging. "
		"Press Enter or click \"Done\" in the side panel to accept the result.");

	const FText SelectionChangeTransactionName = LOCTEXT("SelectionChangeTransaction", "Cube Grid Selection Change");
	const FText ModeChangeTransactionName = LOCTEXT("ModeChangeTransaction", "Cube Grid Mode Change");

	const FString& HoverLineSetID(TEXT("HoverLines"));
	const FString& GridLineSetID(TEXT("GridLines"));
	const FString& SelectionLineSetID(TEXT("SelectionLines"));
	const FString& CornerModeLineSetID(TEXT("CornerModeLines"));

	const FColor HoverLineColor(255, 255, 128); // Pale yellow
	const double HoverLineThickness = 2;
	const double HoverLineDepthBias = 0.1;

	const FColor SelectionLineColor(255, 128, 0); // Orange
	const double SelectionLineDepthBias = 0.1;
	const double SelectionGridLineThickness = 1;
	const double SelectionMainLineThickness = 3;

	const FColor GridLineColor(200, 200, 200);
	const double GridLineDepthBias = 0.05;
	const double GridLineThickness = 0.5;

	const FColor UnselectedCornerLineColor(64, 0, 128); // dark purple
	const FColor SelectedCornerLineColor = FColor::Yellow;
	const double CornerLineThickness = 3;
	const int32 CornerCircleNumSteps = 12;
	const FColor CornerModeWireframeColor = FColor::Red;
	const double CornerModeWireframeThickness = SelectionGridLineThickness;
	const double CornerModeWireframeDepthBias = 0.1;


	/**
	 * Undoes the actual mesh change (i.e. after boolean operations)
	 */
	class FCubeGridToolMeshChange : public FToolCommandChange
	{
	public:
		FCubeGridToolMeshChange(TUniquePtr<UE::Geometry::FDynamicMeshChange> MeshChangeIn)
			: MeshChange(MoveTemp(MeshChangeIn))
		{};

		virtual void Apply(UObject* Object) override
		{
			UCubeGridTool* Tool = Cast<UCubeGridTool>(Object);
			Tool->UpdateUsingMeshChange(*MeshChange, false);
		}
		virtual void Revert(UObject* Object) override
		{
			UCubeGridTool* Tool = Cast<UCubeGridTool>(Object);
			Tool->UpdateUsingMeshChange(*MeshChange, true);
		}

		virtual FString ToString() const override
		{
			return TEXT("CubeGridToolLocals::FCubeGridToolMeshChange");
		}

	protected:
		TUniquePtr<UE::Geometry::FDynamicMeshChange> MeshChange;
	};

	/** Undoes selection changes */
	class FCubeGridToolSelectionChange : public FToolCommandChange
	{
	public:
		FCubeGridToolSelectionChange(
			bool bHaveStartSelectionBeforeIn, bool bHaveStartSelectionAfterIn,
			const UCubeGridTool::FSelection& SelectionBeforeIn,
			const UCubeGridTool::FSelection& SelectionAfterIn
			)
			: bHaveStartSelectionBefore(bHaveStartSelectionBeforeIn)
			, bHaveStartSelectionAfter(bHaveStartSelectionAfterIn)
			, SelectionBefore(SelectionBeforeIn)
			, SelectionAfter(SelectionAfterIn)
		{};

		virtual void Apply(UObject* Object) override
		{
			UCubeGridTool* Tool = Cast<UCubeGridTool>(Object);
			if (!bHaveStartSelectionAfter)
			{
				Tool->ClearSelection(false);
			}
			else
			{
				Tool->SetSelection(SelectionAfter, false);
			}
		}
		virtual void Revert(UObject* Object) override
		{

			UCubeGridTool* Tool = Cast<UCubeGridTool>(Object);
			if (!bHaveStartSelectionBefore)
			{
				Tool->ClearSelection(false);
			}
			else
			{
				Tool->SetSelection(SelectionBefore, false);
			}
		}

		virtual FString ToString() const override
		{
			return TEXT("CubeGridToolLocals::FCubeGridToolSelectionChange");
		}

	protected:
		bool bHaveStartSelectionBefore;
		bool bHaveStartSelectionAfter;
		UCubeGridTool::FSelection SelectionBefore;
		UCubeGridTool::FSelection SelectionAfter;
	};

	/** Undoes activating "corner" mode. Not redoable. */
	class FCubeGridToolModeChange : public FToolCommandChange
	{
	public:
		FCubeGridToolModeChange() {};

		virtual bool HasExpired(UObject* Object) const override
		{
			return Cast<UCubeGridTool>(Object)->IsInDefaultMode();
		}

		virtual void Apply(UObject* Object) override
		{
			return;
		}
		virtual void Revert(UObject* Object) override
		{
			Cast<UCubeGridTool>(Object)->RevertToDefaultMode();
		}

		virtual FString ToString() const override
		{
			return TEXT("CubeGridToolLocals::FCubeGridToolModeChange");
		}
	};
	
	// Attach a frame to the box such that Z points along the given direction.
	FOrientedBox3d ConvertToOrientedBox(const FAxisAlignedBox3d& Box, FCubeGrid::EFaceDirection Direction)
	{
		int FlatDim = FCubeGrid::DirToFlatDim(Direction);

		FVector3d GridSpaceZ = FCubeGrid::DirToNormal(Direction);
		FVector3d GridSpaceX = (FlatDim == 0) ? FVector3d::UnitY() : FVector3d::UnitX();
		FVector3d GridSpaceY = GridSpaceZ.Cross(GridSpaceX);

		FVector3d BoxExtents = Box.Extents();
		FVector3d Extents = BoxExtents; // The case if selection axis is Z

		if (FlatDim == 0)
		{
			// If selection dir was along x axis, then frame z is x, frame x is y, and frame y is x
			BoxExtents = FVector3d(BoxExtents[1], BoxExtents[2], BoxExtents[0]);
		}
		else if (FlatDim == 1)
		{
			// If selection dir was along y axis, then frame z is y, frame x is x, and frame y is z
			Swap(BoxExtents[2], BoxExtents[1]);
		}

		return FOrientedBox3d(FFrame3d(Box.Center(), GridSpaceX, GridSpaceY, GridSpaceZ), BoxExtents);
	}

	void GetNewSelectionFaceInBox(const FCubeGrid& Grid, const FAxisAlignedBox3d& Box, 
		const FCubeGrid::FCubeFace& FaceIn, FCubeGrid::FCubeFace& FaceOut)
	{
		// Start at the corner of the selection and move a little bit to make sure
		// you're in the first face in the corner.
		FVector3d TowardOtherCorner(Box.Max - Box.Min);
		TowardOtherCorner.Normalize();

		FVector3d PointOnDesiredFace = (Box.Min + Grid.GetCurrentGridCellSize() * TowardOtherCorner)
			/ Grid.GetCurrentGridCellSize();

		FaceOut = FCubeGrid::FCubeFace(
			PointOnDesiredFace,
			FaceIn.GetDirection(),
			Grid.GetGridPower());
	}

	/**
	 * Given grid, start point, extrude direction, and number of blocks to extrude, produces
	 * the frame-space extrusion distance. If the start point is not on the grid, the first
	 * "block" is counted as the distance to get back onto grid in the extrusion direction.
	 */
	double GetFrameSpaceExtrudeDist(const FCubeGrid& CubeGrid, const FVector3d& FrameSpaceStartPoint,
		int32 CurrentExtrudeAmount, FCubeGrid::EFaceDirection Direction)
	{
		double GridSpaceExtrudeDist = CurrentExtrudeAmount; // Will be adjusted

		int FlatDim = FCubeGrid::DirToFlatDim(Direction);
		double GridSpaceExtrudeCoord = FrameSpaceStartPoint[FlatDim] / CubeGrid.GetCurrentGridCellSize();
		double ClosestOnGridCoord = FMath::RoundToDouble(GridSpaceExtrudeCoord);

		// See if we're actually off-grid
		if (FMath::Abs(GridSpaceExtrudeCoord - ClosestOnGridCoord) > KINDA_SMALL_NUMBER)
		{
			double NextOnGrid = CurrentExtrudeAmount > 0 ? FMath::CeilToDouble(GridSpaceExtrudeCoord)
				: FMath::FloorToDouble(GridSpaceExtrudeCoord);
			GridSpaceExtrudeDist += (NextOnGrid - GridSpaceExtrudeCoord);
			GridSpaceExtrudeDist += (CurrentExtrudeAmount > 0 ? -1 : 1);
		}

		return GridSpaceExtrudeDist * CubeGrid.GetCurrentGridCellSize();
	}

	bool IsAnyCornerSelected(bool CornerSelectedFlags[])
	{
		for (int i = 0; i < 4; ++i)
		{
			if (CornerSelectedFlags[i])
			{
				return true;
			}
		}
		return false;
	}

	/** 
	 * @param CornerVector Lines to draw on the corners, for instance to show the depth direction
	 */
	void DrawGridRectangle(ULineSetComponent& LineSet, const FCubeGrid& Grid,
		const FVector3d& GridMin, const FVector3d& GridMax,
		const FColor& Color, double Thickness,
		double DepthBias, const FVector3d* CornerVector = nullptr)
	{
		// We'll step from max/min along one of the differing dimensions to get corners
		int32 DifferingDimension = (GridMin[0] == GridMax[0]) ? 1 : 0;

		FVector3d Corner1 = GridMin;
		Corner1[DifferingDimension] = GridMax[DifferingDimension];

		FVector3d Corner2 = GridMax;
		Corner2[DifferingDimension] = GridMin[DifferingDimension];

		LineSet.AddLine(GridMin, Corner1, Color, Thickness, DepthBias);
		LineSet.AddLine(GridMin, Corner2, Color, Thickness, DepthBias);
		LineSet.AddLine(GridMax, Corner1, Color, Thickness, DepthBias);
		LineSet.AddLine(GridMax, Corner2, Color, Thickness, DepthBias);

		if (CornerVector)
		{
			LineSet.AddLine(GridMin, GridMin + *CornerVector, Color, Thickness, DepthBias);
			LineSet.AddLine(GridMax, GridMax + *CornerVector, Color, Thickness, DepthBias);
			LineSet.AddLine(Corner1, Corner1 + *CornerVector, Color, Thickness, DepthBias);
			LineSet.AddLine(Corner2, Corner2 + *CornerVector, Color, Thickness, DepthBias);
		}
	}

	const int32 MAX_NUM_INTERIOR_GRID_LINES = 1000;

	void DrawGridSection(ULineSetComponent& LineSet, const FCubeGrid& Grid, 
		const FAxisAlignedBox3d& BBox,
		const FColor& Color, double Thickness, 
		double DepthBias, const FVector3d* CornerVector = nullptr)
	{
		// Draw the boundary
		DrawGridRectangle(LineSet, Grid, BBox.Min, BBox.Max, Color, Thickness, DepthBias, CornerVector);

		// Find the two nonzero dimensions of the box
		const FVector3d BoxDimensions = BBox.Max - BBox.Min;
		int32 Dim1 = BoxDimensions[0] != 0 ? 0 : 2;
		int32 Dim2 = BoxDimensions[1] != 0 ? 1 : 2;

		double StepSize = Grid.GetCurrentGridCellSize();

		// Draw the inside only if there aren't too many lines to draw (approximate)
		if (StepSize <= 0 || BoxDimensions[Dim1] / StepSize + BoxDimensions[Dim2] / StepSize > MAX_NUM_INTERIOR_GRID_LINES)
		{
			return;
		}

		// Draws lines that lie in the DimToDrawAlong dimension, along border in DimToStepAlong dimension
		auto DrawParallelInteriorLines = [&BBox, &LineSet, &Grid, &Color, &BoxDimensions, StepSize,
			Thickness, DepthBias](int32 DimToStepAlong, int32 DimToDrawAlong)
		{
			FVector3d BorderDirection = FVector3d::Zero();
			BorderDirection[DimToStepAlong] = StepSize;

			int NumSteps = BoxDimensions[DimToStepAlong] / StepSize;
			if (NumSteps * StepSize == BoxDimensions[DimToStepAlong])
			{
				--NumSteps;
			}

			for (int32 i = 0; i < NumSteps; ++i)
			{
				FVector3d SidePoint = BBox.Min + BorderDirection * (i + 1);
				FVector3d OtherSidePoint = SidePoint;
				OtherSidePoint[DimToDrawAlong] = BBox.Max[DimToDrawAlong];

				LineSet.AddLine(SidePoint, OtherSidePoint, Color, Thickness, DepthBias);
			}
		};
		DrawParallelInteriorLines(Dim1, Dim2);
		DrawParallelInteriorLines(Dim2, Dim1);
	}

	/** Given a world hit, get a hit face. */
	void ConvertToFaceHit(const FCubeGrid& CubeGrid, ECubeGridToolFaceSelectionMode SelectionMode, 
		 const FRay& WorldRay, double HitT, const FVector3d& Normal, FCubeGrid::FCubeFace& FaceOut,
		 double Tolerance)
	{
		FVector3d WorldPoint = WorldRay.PointAt(HitT);
		bool bSuccess = false;
		switch (SelectionMode)
		{
		case ECubeGridToolFaceSelectionMode::OutsideBasedOnNormal:
			bSuccess = CubeGrid.GetHitGridFaceBasedOnRay(WorldPoint, Normal, FaceOut, false, Tolerance);
			break;
		case ECubeGridToolFaceSelectionMode::InsideBasedOnNormal:
			bSuccess = CubeGrid.GetHitGridFaceBasedOnRay(WorldPoint, Normal, FaceOut, true, Tolerance);
			break;
		case ECubeGridToolFaceSelectionMode::OutsideBasedOnViewRay:
			bSuccess = CubeGrid.GetHitGridFaceBasedOnRay(WorldPoint, -WorldRay.Direction, FaceOut, false, Tolerance);
			break;
		case ECubeGridToolFaceSelectionMode::InsideBasedOnViewRay:
			bSuccess = CubeGrid.GetHitGridFaceBasedOnRay(WorldPoint, -WorldRay.Direction, FaceOut, true, Tolerance);
			break;
		}

		ensureMsgf(bSuccess, TEXT("CubeGridTool: Unable to convert hit location to proper grid face."));
	}

	/**
	 * Given a world ray and a flat box in grid frame space, intersect the ray with the plane containing the box, 
	 * find the selected cell in the cube grid, and project that cell onto the same plane to produce a new frame 
	 * space box.
	 * 
	 * @param bExpandBoxWithStartBox If true, the output is expanded to contain the original box.
	 * @return true If the plane was actually hit.
	 */
	bool GetCoplanarFrameSpaceSelectedBox(const FCubeGrid& CubeGrid, const FRay& WorldRay, 
		const FAxisAlignedBox3d& StartBox, bool bExpandOutputBoxWithStartBox, FAxisAlignedBox3d& BoxOut)
	{
		FVector3d BoxDims = StartBox.Max - StartBox.Min;
		int FlatDim = 0;
		for (int i = 0; i < 3; ++i)
		{
			if (BoxDims[i] == 0)
			{
				FlatDim = i;
				break;
			}
		}

		bool bIntersects = false;
		FVector IntersectionPoint;
		GizmoMath::RayPlaneIntersectionPoint(
			CubeGrid.GetFrame().FromFramePoint(StartBox.Min), 
			CubeGrid.GetFrame().GetAxis(FlatDim), 
			WorldRay.Origin, WorldRay.Direction, 
			bIntersects, IntersectionPoint);

		if (!bIntersects)
		{
			return false;
		}

		FVector3d GridSpaceIntersection = CubeGrid.ToGridPoint(IntersectionPoint);

		FVector3d FrameSpaceMin(
			FMath::Floor(GridSpaceIntersection.X) * CubeGrid.GetCurrentGridCellSize(),
			FMath::Floor(GridSpaceIntersection.Y) * CubeGrid.GetCurrentGridCellSize(),
			FMath::Floor(GridSpaceIntersection.Z) * CubeGrid.GetCurrentGridCellSize());
		FVector3d FrameSpaceMax(
			FMath::CeilToDouble(GridSpaceIntersection.X) * CubeGrid.GetCurrentGridCellSize(),
			FMath::CeilToDouble(GridSpaceIntersection.Y) * CubeGrid.GetCurrentGridCellSize(),
			FMath::CeilToDouble(GridSpaceIntersection.Z) * CubeGrid.GetCurrentGridCellSize());

		// Project the cell we got back onto the plane
		FrameSpaceMin[FlatDim] = StartBox.Min[FlatDim];
		FrameSpaceMax[FlatDim] = StartBox.Min[FlatDim];

		BoxOut = FAxisAlignedBox3d(FrameSpaceMin, FrameSpaceMax);
		if (bExpandOutputBoxWithStartBox)
		{
			BoxOut.Contain(StartBox);
		}

		return true;
	}
}

const FToolTargetTypeRequirements& UCubeGridToolBuilder::GetTargetRequirements() const
{
	static FToolTargetTypeRequirements TypeRequirements({
		UMaterialProvider::StaticClass(),
		UMeshDescriptionCommitter::StaticClass(),
		UMeshDescriptionProvider::StaticClass(),
		UPrimitiveComponentBackedTarget::StaticClass()
		});
	return TypeRequirements;
}

bool UCubeGridToolBuilder::CanBuildTool(const FToolBuilderState& SceneState) const
{
	return SceneState.TargetManager->CountSelectedAndTargetable(SceneState, GetTargetRequirements()) <= 1;
}

UInteractiveTool* UCubeGridToolBuilder::BuildTool(const FToolBuilderState& SceneState) const
{
	UCubeGridTool* NewTool = NewObject<UCubeGridTool>(SceneState.ToolManager);

	TObjectPtr<UToolTarget> Target = SceneState.TargetManager->BuildFirstSelectedTargetable(SceneState, GetTargetRequirements());
	NewTool->SetTarget(Target); // May be null
	NewTool->SetWorld(SceneState.World);

	return NewTool;
}



void UCubeGridTool::InvalidatePreview(bool bUpdateCornerLineSet)
{
	using namespace CubeGridToolLocals;

	// Do the line set first before we get to all the early returns
	if (bUpdateCornerLineSet && Mode == EMode::Corner)
	{
		UpdateCornerModeLineSet();
	}

	if (CurrentExtrudeAmount == 0)
	{
		// Reset the preview
		Preview->CancelCompute();
		if (bPreviewMayDiffer)
		{
			Preview->PreviewMesh->UpdatePreview(CurrentMesh.Get());
			bPreviewMayDiffer = false;
			bWaitingToApplyPreview = false;
		}
		return;
	}
	else if (CurrentMesh->TriangleCount() == 0 && CurrentExtrudeAmount < 0 
		&& Mode != EMode::Corner)
	{
		// We're subtracting from an empty mesh. Just slide the selection.
		// This will also reset the preview.
		SlideSelection(CurrentExtrudeAmount, true);
		bPreviewMayDiffer = false;
		bWaitingToApplyPreview = false;
		return;
	}

	// If we didn't start with an existing mesh, and we are adding to an empty
	// starting mesh, set the transform such that it is in the (grid) minimum of
	// the selection. This frequently (though not always, in the case of
	// a pyramid) places the pivot in a handy corner for snapping.
	if (!Target && CurrentMesh->TriangleCount() == 0 && CurrentExtrudeAmount > 0
		&& ensure(bHaveSelection))
	{
		FFrame3d GridFrame = CubeGrid->GetFrame();
		GridFrame.Origin = GridFrame.FromFramePoint(Selection.Box.Min);
		CurrentMeshTransform = GridFrame.ToTransform();

		GridGizmoAlignmentMechanic->InitializeDeformedMeshRayCast([this]() { return MeshSpatial.Get(); }, CurrentMeshTransform, nullptr);
	}

	// Finally invalidate the preview
	Preview->InvalidateResult();
	bPreviewMayDiffer = true;
}

TUniquePtr<FDynamicMeshOperator> UCubeGridTool::MakeNewOperator()
{
	using namespace CubeGridToolLocals;

	// Figure out how far to extrude in grid space. This only becomes tricky if the selection
	// is no longer on grid because we changed grid power.
	double FrameSpaceExtrudeAmount = GetFrameSpaceExtrudeDist(*CubeGrid, Selection.Box.Min, CurrentExtrudeAmount, Selection.Direction);

	FOrientedBox3d FrameSpaceBox = ConvertToOrientedBox(Selection.Box, Selection.Direction);

	// Give the selection box depth
	FrameSpaceBox.Frame.Origin += (FrameSpaceExtrudeAmount / 2.0f) * FCubeGrid::DirToNormal(Selection.Direction);
	FrameSpaceBox.Extents.Z = FMath::Abs(FrameSpaceExtrudeAmount) / 2.0f;

	// Translate the oriented box from grid space to world space
	const FFrame3d& GridFrame = CubeGrid->GetFrame();
	FOrientedBox3d WorldBox(FFrame3d(GridFrame.FromFrame(FrameSpaceBox.Frame)), FrameSpaceBox.Extents);

	// Make the op.
	TUniquePtr<FCubeGridBooleanOp> Op = MakeUnique<FCubeGridBooleanOp>();
	Op->InputMesh = ComputeStartMesh;
	Op->InputTransform = CurrentMeshTransform;
	Op->bKeepInputTransform = true;
	Op->WorldBox = WorldBox;
	Op->bSubtract = CurrentExtrudeAmount < 0;
	Op->bTrackChangedTids = true;

	if (Mode == EMode::Corner)
	{
		Op->CornerInfo = MakeShared<FCubeGridBooleanOp::FCornerInfo>();
		for (int i = 0; i < 4; ++i)
		{
			Op->CornerInfo->WeldedAtBase[i] = !CornerSelectedFlags[i];
		}

		Op->bCrosswiseDiagonal = Settings->bCrosswiseDiagonal;
	}

	return Op;
}

void UCubeGridTool::SlideSelection(int32 Amount, bool bEmitChange)
{
	using namespace CubeGridToolLocals;

	FVector3d FrameSpaceDisplacement = FCubeGrid::DirToNormal(Selection.Direction)
		* GetFrameSpaceExtrudeDist(*CubeGrid, Selection.StartBox.Min, Amount, Selection.Direction);

	FSelection NewSelection = Selection;
	NewSelection.StartBox = FAxisAlignedBox3d(
		Selection.StartBox.Min + FrameSpaceDisplacement,
		Selection.StartBox.Max + FrameSpaceDisplacement);
	NewSelection.Box = FAxisAlignedBox3d(
		Selection.Box.Min + FrameSpaceDisplacement,
		Selection.Box.Max + FrameSpaceDisplacement);

	SetSelection(NewSelection, true);
}

void UCubeGridTool::SetSelection(const FSelection& NewSelection, bool bEmitChange)
{
	using namespace CubeGridToolLocals;

	// Clear op/preview
	if (Mode != EMode::Corner)
	{
		CurrentExtrudeAmount = 0;
		InvalidatePreview();
	}

	if (bEmitChange && (!bHaveSelection || Selection != NewSelection))
	{
		GetToolManager()->EmitObjectChange(this,
			MakeUnique<FCubeGridToolSelectionChange>(bHaveSelection, true, Selection, NewSelection),
			SelectionChangeTransactionName);
	}
	Selection = NewSelection;
	bHaveSelection = true;

	UpdateSelectionLineSet();
	if (Mode == EMode::Corner)
	{
		InvalidatePreview();
	}
}

void UCubeGridTool::ClearSelection(bool bEmitChange)
{
	using namespace CubeGridToolLocals;

	if (bEmitChange && bHaveSelection)
	{
		GetToolManager()->EmitObjectChange(this,
			MakeUnique<FCubeGridToolSelectionChange>(bHaveSelection, false, Selection, Selection),
			SelectionChangeTransactionName);
	}
	bHaveSelection = false;

	UpdateSelectionLineSet();
}


void UCubeGridTool::Setup()
{
	using namespace CubeGridToolLocals;

	UInteractiveTool::Setup();

	GetToolManager()->DisplayMessage(PushPullModeMessage, EToolMessageLevel::UserNotification);

	DuringActivityActions = NewObject<UCubeGridDuringActivityActions>(this);
	DuringActivityActions->Initialize(this);
	AddToolPropertySource(DuringActivityActions);
	SetToolPropertySourceEnabled(DuringActivityActions, false);

	ToolActions = NewObject<UCubeGridToolActions>(this);
	ToolActions->Initialize(this);
	AddToolPropertySource(ToolActions);

	Settings = NewObject<UCubeGridToolProperties>(this);
	Settings->RestoreProperties(this);
	AddToolPropertySource(Settings);

	OutputTypeProperties = NewObject<UCreateMeshObjectTypeProperties>(this);
	OutputTypeProperties->RestoreProperties(this);
	OutputTypeProperties->InitializeDefault();
	OutputTypeProperties->WatchProperty(OutputTypeProperties->OutputType, [this](FString) { OutputTypeProperties->UpdatePropertyVisibility(); });
	AddToolPropertySource(OutputTypeProperties);

	MaterialProperties = NewObject<UNewMeshMaterialProperties>(this);
	MaterialProperties->RestoreProperties(this);
	MaterialProperties->bShowExtendedOptions = false;
	AddToolPropertySource(MaterialProperties);

	CurrentMesh = MakeShared<FDynamicMesh3, ESPMode::ThreadSafe>();
	CurrentMesh->EnableAttributes();
	if (Target)
	{
		*CurrentMesh = UE::ToolTarget::GetDynamicMeshCopy(Target);
		UE::ToolTarget::SetSourceObjectVisible(Target, false);
	}

	MeshSpatial = MakeShared<FDynamicMeshAABBTree3, ESPMode::ThreadSafe>();
	MeshSpatial->SetMesh(CurrentMesh.Get(), true);

	Preview = NewObject<UMeshOpPreviewWithBackgroundCompute>(this);
	Preview->Setup(TargetWorld, this);
	ToolSetupUtil::ApplyRenderingConfigurationToPreview(Preview->PreviewMesh, Target);
	Preview->PreviewMesh->SetTangentsMode(EDynamicMeshComponentTangentsMode::AutoCalculated);
	if (Target)
	{
		Preview->PreviewMesh->UpdatePreview(CurrentMesh.Get());
		CurrentMeshTransform = UE::ToolTarget::GetLocalToWorldTransform(Target);

		// Set materials
		FComponentMaterialSet MaterialSet = UE::ToolTarget::GetMaterialSet(Target);
		Preview->ConfigureMaterials(MaterialSet.Materials, ToolSetupUtil::GetDefaultWorkingMaterial(GetToolManager()));
	}
	Preview->PreviewMesh->SetTransform((FTransform)CurrentMeshTransform);
	Preview->OnOpCompleted.AddWeakLambda(this, [this](const FDynamicMeshOperator* UncastOp) {
		const FCubeGridBooleanOp* Op = static_cast<const FCubeGridBooleanOp*>(UncastOp);
		if (Op->InputMesh == ComputeStartMesh)
		{
			LastOpChangedTids = static_cast<const FCubeGridBooleanOp*>(Op)->ChangedTids;
		}
	});

	CubeGrid = MakeShared<FCubeGrid>();
	CubeGrid->SetGridFrame(FFrame3d(Settings->GridFrameOrigin, Settings->GridFrameOrientation.Quaternion()));
	CubeGrid->SetBaseGridCellSize(Settings->BlockBaseSize);
	CubeGrid->SetGridPower(Settings->PowerOfTwo);

	FTransform GridTransform = CubeGrid->GetFrame().ToFTransform();

	GridGizmoTransformProxy = NewObject<UTransformProxy>(this);
	GridGizmoTransformProxy->SetTransform(GridTransform);
	GridGizmoTransformProxy->OnBeginTransformEdit.AddWeakLambda(this, [this](UTransformProxy* Proxy) 
		{
			bInGizmoDrag = true;
		});
	GridGizmoTransformProxy->OnTransformChanged.AddUObject(this, &UCubeGridTool::GridGizmoMoved);
	GridGizmoTransformProxy->OnEndTransformEdit.AddWeakLambda(this,
		[this](UTransformProxy* Proxy) {
			NotifyOfPropertyChangeByTool(Settings);
			UpdateCornerGeometrySet();
			bInGizmoDrag = false;
		});

	GridGizmo = UE::TransformGizmoUtil::CreateCustomTransformGizmo(GetToolManager(),
		ETransformGizmoSubElements::StandardTranslateRotate, this);
	GridGizmo->bUseContextCoordinateSystem = false;
	GridGizmo->SetActiveTarget(GridGizmoTransformProxy, GetToolManager());
	GridGizmoAlignmentMechanic = NewObject<UDragAlignmentMechanic>(this);
	GridGizmoAlignmentMechanic->Setup(this);
	GridGizmoAlignmentMechanic->InitializeDeformedMeshRayCast([this]() { return MeshSpatial.Get(); }, CurrentMeshTransform, nullptr);
	GridGizmoAlignmentMechanic->AddToGizmo(GridGizmo);

	GridGizmo->SetVisibility(false);

	LineSets = NewObject<UPreviewGeometry>();
	LineSets->CreateInWorld(TargetWorld, GridTransform);

	LineSets->AddLineSet(HoverLineSetID);
	LineSets->AddLineSet(SelectionLineSetID);
	LineSets->AddLineSet(CornerModeLineSetID);
	LineSets->SetAllLineSetsMaterial(ToolSetupUtil::GetDefaultLineComponentMaterial(GetToolManager(), /*bDepthTested*/ false));

	LineSets->AddLineSet(GridLineSetID);
	LineSets->SetLineSetMaterial(GridLineSetID, 
		ToolSetupUtil::GetDefaultLineComponentMaterial(GetToolManager(), /*bDepthTested*/ true));

	UpdateGridLineSet();

	SelectedCornerRenderer.LineThickness = CornerLineThickness;

	ClickDragBehavior = NewObject<UClickDragInputBehavior>();
	ClickDragBehavior->Initialize(this);
	AddInputBehavior(ClickDragBehavior, this);

	HoverBehavior = NewObject<UMouseHoverBehavior>();
	HoverBehavior->Modifiers.RegisterModifier(ShiftModifierID, FInputDeviceState::IsShiftKeyDown);
	HoverBehavior->Modifiers.RegisterModifier(CtrlModifierID, FInputDeviceState::IsCtrlKeyDown);
	HoverBehavior->Initialize(this);
	AddInputBehavior(HoverBehavior, this);

	CtrlMiddleClickBehavior = NewObject<ULocalSingleClickInputBehavior>();
	CtrlMiddleClickBehavior->Initialize();
	CtrlMiddleClickBehavior->SetUseMiddleMouseButton();
	CtrlMiddleClickBehavior->ModifierCheckFunc = [this](const FInputDeviceState& InputState) {
		return FInputDeviceState::IsCtrlKeyDown(InputState);
	};
	CtrlMiddleClickBehavior->IsHitByClickFunc = [this](const FInputDeviceRay& InputRay) {
		FCubeGrid::FCubeFace Face;
		FInputRayHit OutResult;
		OutResult.bHit = GetHitGridFace(InputRay.WorldRay, Face);
		return OutResult;
	};
	CtrlMiddleClickBehavior->OnClickedFunc = [this](const FInputDeviceRay& ClickPos) {
		OnCtrlMiddleClick(ClickPos);
	};
	AddInputBehavior(CtrlMiddleClickBehavior);

	MiddleClickDragBehavior = NewObject<ULocalClickDragInputBehavior>();
	MiddleClickDragBehavior->Initialize();
	MiddleClickDragBehavior->SetUseMiddleMouseButton();
	MiddleClickDragBehavior->ModifierCheckFunc = [this](const FInputDeviceState& InputState) {
		return !FInputDeviceState::IsCtrlKeyDown(InputState);
	};
	MiddleClickDragBehavior->CanBeginClickDragFunc = [this](const FInputDeviceRay& ClickPos) {
		return CanBeginMiddleClickDrag(ClickPos);
	};
	MiddleClickDragBehavior->OnClickPressFunc = [this](const FInputDeviceRay& ClickPos) {
		PrepForSelectionChange();
		RayCastSelectionPlane((FRay3d)ClickPos.WorldRay, MiddleClickDragStart);
	};
	MiddleClickDragBehavior->OnClickDragFunc = [this](const FInputDeviceRay& ClickPos) {
		OnMiddleClickDrag(ClickPos);
	};
	MiddleClickDragBehavior->OnClickReleaseFunc = [this](const FInputDeviceRay&) {
		EndSelectionChange();
	};
	MiddleClickDragBehavior->OnTerminateFunc = [this]() {
		EndSelectionChange();
	};
	AddInputBehavior(MiddleClickDragBehavior);

	PowerOfTwoPrevious = Settings->PowerOfTwo;
	Settings->WatchProperty(Settings->PowerOfTwo,
		[this](uint8 NewPower) {
			SetPowerOfTwoClamped(Settings->PowerOfTwo);
		});
	Settings->WatchProperty(Settings->BlockBaseSize,
		[this](double NewBaseSize) {
			ClearHover();

			CubeGrid->SetBaseGridCellSize(NewBaseSize);

			UpdateSelectionLineSet();
			UpdateGridLineSet();
		});
	Settings->WatchProperty(Settings->bShowGizmo,
		[this](bool bOn) {
			UpdateGizmoVisibility(bOn);
		});
	Settings->WatchProperty(Settings->bCrosswiseDiagonal,
		[this](bool bOn) {
			InvalidatePreview();
		});
	Settings->WatchProperty(Settings->PlaneTolerance,
		[this](double Tolerance) {
			InvalidatePreview(false);
		});

	auto UpdateFromDetailsPanelTransformChange = [this]()
	{
		CubeGrid->SetGridFrame(FFrame3d(Settings->GridFrameOrigin, Settings->GridFrameOrientation.Quaternion()));

		FTransform GridTransform = CubeGrid->GetFrame().ToFTransform();
		LineSets->SetTransform(GridTransform);
		GridGizmo->ReinitializeGizmoTransform(GridTransform);

		InvalidatePreview(false);
	};

	GridFrameOriginWatcherIdx = Settings->WatchProperty(Settings->GridFrameOrigin,
		[UpdateFromDetailsPanelTransformChange](FVector) {
			UpdateFromDetailsPanelTransformChange();
		});
	GridFrameOrientationWatcherIdx = Settings->WatchProperty(Settings->GridFrameOrientation,
		[UpdateFromDetailsPanelTransformChange](FRotator) {
			UpdateFromDetailsPanelTransformChange();
		});

	Settings->SilentUpdateWatched();

	UpdateComputeInputs();

	if (Target)
	{
		GetToolManager()->DisplayMessage(LOCTEXT("EditingExistingAssetLabel", "Editing existing asset."), 
			EToolMessageLevel::UserWarning);
	}
	else
	{
		GetToolManager()->DisplayMessage(LOCTEXT("CreatingNewAssetLabel", "Creating new asset."),
			EToolMessageLevel::UserWarning);
	}
}

void UCubeGridTool::UpdateComputeInputs()
{
	ComputeStartMesh = MakeShared<FDynamicMesh3>(*CurrentMesh);
}

void UCubeGridTool::Shutdown(EToolShutdownType ShutdownType)
{
	Settings->SaveProperties(this);
	MaterialProperties->SaveProperties(this);

	if (Mode == EMode::Corner)
	{
		ApplyCornerMode(true);
	}

	if (Target)
	{
		Cast<IPrimitiveComponentBackedTarget>(Target)->SetOwnerVisibility(true);

		// We check the shutdown type because even though we are not an accept/cancel tool, we
		// get a cancel shutdown via Ctrl+Z. In this case we definitely don't want to update because
		// an update in an undo transaction results in a crash.
		if (bChangesMade && ShutdownType != EToolShutdownType::Cancel && Target->IsValid())
		{
			GetToolManager()->BeginUndoTransaction(LOCTEXT("CubeGridToolEditTransactionName", "Block Tool Edit"));
			UE::ToolTarget::CommitDynamicMeshUpdate(Target, *CurrentMesh, true);
			GetToolManager()->EndUndoTransaction();
		}
		else if (!Target->IsValid())
		{
			UE_LOG(LogGeometry, Error, TEXT("CubeGridTool:: Edited mesh could not be committed (it was likely forcibly deleted from under the tool)."));
		}
	}
	else if (CurrentMesh->TriangleCount() > 0 && ShutdownType != EToolShutdownType::Cancel)
	{
		GetToolManager()->BeginUndoTransaction(LOCTEXT("CubeGridToolCreateTransactionName", "Block Tool Create New"));

		FCreateMeshObjectParams NewMeshObjectParams;
		NewMeshObjectParams.TargetWorld = TargetWorld;
		NewMeshObjectParams.Transform = (FTransform)CurrentMeshTransform;
		NewMeshObjectParams.BaseName = TEXT("CubeGridToolOutput");
		NewMeshObjectParams.Materials.Add(MaterialProperties->Material.Get());
		NewMeshObjectParams.SetMesh(CurrentMesh.Get());
		OutputTypeProperties->ConfigureCreateMeshObjectParams(NewMeshObjectParams);
		FCreateMeshObjectResult Result = UE::Modeling::CreateMeshObject(GetToolManager(), MoveTemp(NewMeshObjectParams));
		if (Result.IsOK() && Result.NewActor != nullptr)
		{
			ToolSelectionUtil::SetNewActorSelection(GetToolManager(), Result.NewActor);
		}

		GetToolManager()->EndUndoTransaction();
	}

	Preview->OnOpCompleted.RemoveAll(this);
	Preview->Shutdown();

	if (LineSets)
	{
		LineSets->Disconnect();
		LineSets = nullptr;
	}

	GridGizmoAlignmentMechanic->Shutdown();

	GetToolManager()->GetPairedGizmoManager()->DestroyAllGizmosByOwner(this);
}

void UCubeGridTool::OnTick(float DeltaTime)
{
	if (Preview)
	{
		Preview->Tick(DeltaTime);
	}

	if (PendingAction != ECubeGridToolAction::NoAction)
	{
		ApplyAction(PendingAction);
		PendingAction = ECubeGridToolAction::NoAction;
	}

	if (bWaitingToApplyPreview && Preview->HaveValidResult())
	{
		ApplyPreview();
	}
}

void UCubeGridTool::ApplyPreview()
{
	using namespace CubeGridToolLocals;

	FDynamicMeshChangeTracker ChangeTracker(CurrentMesh.Get());
	ChangeTracker.BeginChange();
	ChangeTracker.SaveTriangles(*LastOpChangedTids, true /*bSaveVertices*/);

	// Update current mesh
	bChangesMade = true; // TODO: make this undoable

	CurrentMesh->Copy(*Preview->PreviewMesh->GetMesh());
	MeshSpatial->Build();

	UpdateComputeInputs();

	const FText TransactionText = LOCTEXT("CubeGridToolTransactionName", "Block Tool Change");
	GetToolManager()->BeginUndoTransaction(TransactionText);

	bWaitingToApplyPreview = false;
	bPreviewMayDiffer = false;
	bBlockUntilPreviewUpdate = false;

	if (bAdjustSelectionOnPreviewUpdate)
	{
		// Change the selection to the new location. Note that this should to happen
		// after resetting bPreviewMayDiffer to avoid an extra preview reset when selection
		// changes.
		SlideSelection(CurrentExtrudeAmount, true);
	}
	CurrentExtrudeAmount = 0;

	GetToolManager()->EmitObjectChange(this, MakeUnique<FCubeGridToolMeshChange>(ChangeTracker.EndChange()), TransactionText);
	GetToolManager()->EndUndoTransaction();
}

void UCubeGridTool::Render(IToolsContextRenderAPI* RenderAPI)
{
	using namespace CubeGridToolLocals;

	GetToolManager()->GetContextQueriesAPI()->GetCurrentViewState(CameraState);

	if (Mode == EMode::Corner)
	{
		FOrientedBox3d OrientedBox = ConvertToOrientedBox(Selection.Box, Selection.Direction);

		SelectedCornerRenderer.BeginFrame(RenderAPI, CameraState);
		for (int i = 0; i < 4; ++i)
		{
			FVector WorldPosition = CubeGrid->GetFrame().FromFramePoint(OrientedBox.GetCorner(i));

			// Depending on whether we're in an orthographic view or not, we set the radius based on visual angle or based on ortho 
			// viewport width (divided into 90 segments like the FOV is divided into 90 degrees).
			float Radius = (CameraState.bIsOrthographic) ? CameraState.OrthoWorldCoordinateWidth * 0.5 / 90.0
				: (float)ToolSceneQueriesUtil::CalculateDimensionFromVisualAngleD(CameraState, (FVector3d)WorldPosition, 0.5);
			bool bDepthTested = false;
			SelectedCornerRenderer.DrawViewFacingCircle(
				WorldPosition,
				Radius,
				CornerCircleNumSteps,
				CornerSelectedFlags[i] ? SelectedCornerLineColor : UnselectedCornerLineColor,
				CornerLineThickness, bDepthTested);
		}
		SelectedCornerRenderer.EndFrame();
	}

	GridGizmoAlignmentMechanic->Render(RenderAPI);
}

void UCubeGridTool::OnPropertyModified(UObject* PropertySet, FProperty* Property)
{
}

void UCubeGridTool::ClearHover() 
{
	if (bHaveHoveredSelection)
	{
		UpdateHoverLineSet(false, HoveredSelectionBox);
	}
}

void  UCubeGridTool::GridGizmoMoved(UTransformProxy* Proxy, FTransform Transform)
{
	Settings->GridFrameOrigin = Transform.GetTranslation();
	Settings->SilentUpdateWatcherAtIndex(GridFrameOriginWatcherIdx);

	Settings->GridFrameOrientation = Transform.GetRotation().Rotator();
	Settings->SilentUpdateWatcherAtIndex(GridFrameOrientationWatcherIdx);

	CubeGrid->SetGridFrame(FFrame3d(Settings->GridFrameOrigin, Settings->GridFrameOrientation.Quaternion()));
	LineSets->SetTransform(CubeGrid->GetFrame().ToFTransform());

	if (Mode == EMode::Corner)
	{
		InvalidatePreview(false);

		if (!bInGizmoDrag)
		{
			UpdateCornerGeometrySet();
		}
	}
}

void UCubeGridTool::OnCtrlMiddleClick(const FInputDeviceRay& ClickPos)
{
	// Get the selected face
	FCubeGrid::FCubeFace Face;
	if (!ensure(GetHitGridFace(ClickPos.WorldRay, Face)))
	{
		return;
	}

	if (!GridGizmo->IsVisible())
	{
		UpdateGizmoVisibility(true);
	}

	// Get the face's four corners in grid space
	FVector3d FaceMin = Face.GetMinCorner();
	FVector3d FaceMax = Face.GetMaxCorner();

	FVector3d Corners[4] = {
		FaceMin,
		FaceMin,
		FaceMax,
		FaceMax
	};

	int32 DifferingDimension = (FaceMin[0] == FaceMax[0]) ? 1 : 0;

	Corners[1][DifferingDimension] = FaceMax[DifferingDimension];
	Corners[3][DifferingDimension] = FaceMin[DifferingDimension];

	// Transform the ray to grid space and see which of the corners is closest.
	FVector3d GridSpaceRayOrigin = CubeGrid->ToGridPoint(ClickPos.WorldRay.Origin);
	FVector3d GridSpaceRayDirection = CubeGrid->GetFrame().ToFrameVector(FVector3d(ClickPos.WorldRay.Direction));
	GridSpaceRayDirection.Normalize();
	FRay3d GizmoSpaceRay(GridSpaceRayOrigin, GridSpaceRayDirection);

	double MinDistSquared = GizmoSpaceRay.DistSquared(Corners[0]);
	int32 ClosestCornerIndex = 0;
	for (int32 i = 1; i < 4; ++i)
	{
		double DistSquared = GizmoSpaceRay.DistSquared(Corners[i]);
		if (DistSquared < MinDistSquared)
		{
			MinDistSquared = DistSquared;
			ClosestCornerIndex = i;
		}
	}

	GetToolManager()->BeginUndoTransaction(LOCTEXT("QuickAdjustGizmo", "Transform Gizmo"));
	
	// Adjust the selection if needed
	if (bHaveSelection)
	{
		FVector3d GridSpaceDisplacement = Corners[ClosestCornerIndex];
		FVector3d FrameSpaceDisplacement = GridSpaceDisplacement * CubeGrid->GetCurrentGridCellSize();

		FSelection NewSelection = Selection;
		NewSelection.StartBox = FAxisAlignedBox3d(
			Selection.StartBox.Min - FrameSpaceDisplacement,
			Selection.StartBox.Max - FrameSpaceDisplacement);
		NewSelection.Box = FAxisAlignedBox3d(
			Selection.Box.Min - FrameSpaceDisplacement,
			Selection.Box.Max - FrameSpaceDisplacement);

		SetSelection(NewSelection, true);
	}

	// Move the gizmo to that corner.
	Settings->GridFrameOrigin = CubeGrid->ToWorldPoint(Corners[ClosestCornerIndex]);
	Settings->SilentUpdateWatcherAtIndex(GridFrameOriginWatcherIdx);
	CubeGrid->SetGridFrame(FFrame3d(Settings->GridFrameOrigin, Settings->GridFrameOrientation.Quaternion()));

	FTransform GridTransform = CubeGrid->GetFrame().ToFTransform();
	LineSets->SetTransform(GridTransform);
	GridGizmo->SetNewGizmoTransform(GridTransform);

	GetToolManager()->EndUndoTransaction();
}

// Tries to intersect the selected box. Used for middle mouse dragging the selection.
FInputRayHit UCubeGridTool::RayCastSelectionPlane(const FRay3d& WorldRay,
	FVector3d& HitPointOut)
{
	FVector3d Normal = CubeGrid->GetFrame().FromFrameVector(
		FCubeGrid::DirToNormal(Selection.Direction));

	FInputRayHit HitResult;
	FVector HitPoint;
	GizmoMath::RayPlaneIntersectionPoint(CubeGrid->GetFrame().FromFramePoint(Selection.Box.Min), Normal,
		WorldRay.Origin, WorldRay.Direction, HitResult.bHit, HitPoint);
	
	if (HitResult.bHit)
	{
		HitPointOut = HitPoint;
		HitResult = FInputRayHit(WorldRay.GetParameter(HitPointOut));
	}
	return HitResult;
}

FInputRayHit UCubeGridTool::CanBeginMiddleClickDrag(const FInputDeviceRay& ClickPos)
{
	FInputRayHit HitResult;
	if (!bHaveSelection)
	{
		return HitResult;
	}

	FVector3d WorldHitPoint;
	HitResult = RayCastSelectionPlane((FRay3d)ClickPos.WorldRay, WorldHitPoint);

	int FlatDim = FCubeGrid::DirToFlatDim(Selection.Direction);

	FVector3d FrameSpacePoint = CubeGrid->GetFrame().ToFramePoint(WorldHitPoint);
	FrameSpacePoint[FlatDim] = Selection.Box.Min[FlatDim];
	HitResult.bHit = Selection.Box.Contains(FrameSpacePoint);

	return HitResult;
}

void UCubeGridTool::OnMiddleClickDrag(const FInputDeviceRay& DragPos)
{
	if (!bHaveSelection)
	{
		return;
	}

	FVector3d MiddleClickDragEnd;
	RayCastSelectionPlane((FRay3d)DragPos.WorldRay, MiddleClickDragEnd);
	FVector3d DisplacementInGridFrame = CubeGrid->GetFrame().ToFrameVector(MiddleClickDragEnd - MiddleClickDragStart); 
	
	// Clamp the relevant dimension in the displacement vector
	DisplacementInGridFrame[FCubeGrid::DirToFlatDim(Selection.Direction)] = 0;

	// Make the displacement be a multiple of the current grid cell size
	DisplacementInGridFrame /= CubeGrid->GetCurrentGridCellSize();
	DisplacementInGridFrame = FVector3d(
		FMath::RoundToDouble(DisplacementInGridFrame.X), 
		FMath::RoundToDouble(DisplacementInGridFrame.Y),
		FMath::RoundToDouble(DisplacementInGridFrame.Z));
	DisplacementInGridFrame *= CubeGrid->GetCurrentGridCellSize();

	FAxisAlignedBox3d NewSelectionBox(
		PreviousSelection.Box.Min + DisplacementInGridFrame,
		PreviousSelection.Box.Max + DisplacementInGridFrame
	);

	// Adjust selection
	if (NewSelectionBox != Selection.Box)
	{
		Selection.StartBox = FAxisAlignedBox3d(
			PreviousSelection.StartBox.Min + DisplacementInGridFrame,
			PreviousSelection.StartBox.Max + DisplacementInGridFrame
		);

		Selection.Box = FAxisAlignedBox3d(
			PreviousSelection.Box.Min + DisplacementInGridFrame,
			PreviousSelection.Box.Max + DisplacementInGridFrame
		);

		UpdateSelectionLineSet();
		InvalidatePreview();
	}
}

void UCubeGridTool::PrepForSelectionChange()
{
	bPreviousHaveSelection = bHaveSelection;
	PreviousSelection = Selection;
}

void UCubeGridTool::EndSelectionChange()
{
	using namespace CubeGridToolLocals;

	if (bPreviousHaveSelection != bHaveSelection
		|| (bHaveSelection && PreviousSelection != Selection))
	{
		GetToolManager()->EmitObjectChange(this,
			MakeUnique<FCubeGridToolSelectionChange>(bPreviousHaveSelection, bHaveSelection,
				PreviousSelection, Selection),
			SelectionChangeTransactionName);
	}
}

void UCubeGridTool::UpdateGizmoVisibility(bool bVisible)
{
	GridGizmo->SetVisibility(bVisible);
	LineSets->SetLineSetMaterial(CubeGridToolLocals::GridLineSetID,
		ToolSetupUtil::GetDefaultLineComponentMaterial(GetToolManager(), /*bDepthTested*/ !bVisible));
	Settings->bShowGizmo = bVisible;
	Settings->SilentUpdateWatched();
}

bool UCubeGridTool::GetHitGridFace(const FRay& WorldRay, FCubeGrid::FCubeFace& FaceOut)
{
	using namespace CubeGridToolLocals;

	double BestHitT = TNumericLimits<double>::Max();

	// We always hit-test the ground plane...
	bool bHitPlane;
	FVector IntersectionPoint;
	GizmoMath::RayPlaneIntersectionPoint(CubeGrid->GetFrame().Origin, CubeGrid->GetFrame().Z(), 
		WorldRay.Origin, WorldRay.Direction, bHitPlane, IntersectionPoint);
	if (bHitPlane)
	{
		FVector3d ClampedGridPoint = CubeGrid->ToGridPoint(IntersectionPoint);
		ClampedGridPoint.Z = 0;
		FaceOut = FCubeGrid::FCubeFace(ClampedGridPoint,
			CubeGrid->ToGridPoint(WorldRay.Origin).Z >= 0 ? FCubeGrid::EFaceDirection::PositiveZ : FCubeGrid::EFaceDirection::NegativeZ,
			CubeGrid->GetGridPower());
		BestHitT = WorldRay.GetParameter(IntersectionPoint);
	}

	// ...However depending on the settings, we may give everything else priority, which we do by
	// keeping the plane hit distance maximal.
	if (!Settings->bHitGridGroundPlaneIfCloser)
	{
		BestHitT = TNumericLimits<double>::Max();
	}

	if (Settings->bHitUnrelatedGeometry)
	{
		FHitResult HitResult;
		if (ToolSceneQueriesUtil::FindNearestVisibleObjectHit(this, HitResult, WorldRay)
			&& HitResult.Distance < BestHitT)
		{
			BestHitT = HitResult.Distance;
			ConvertToFaceHit(*CubeGrid, Settings->FaceSelectionMode, 
				WorldRay, BestHitT, HitResult.ImpactNormal, FaceOut, Settings->PlaneTolerance);
		}
	}

	if (MeshSpatial)
	{
		FRay3d LocalRay(CurrentMeshTransform.InverseTransformPosition((FVector3d)WorldRay.Origin),
			CurrentMeshTransform.InverseTransformVectorNoScale((FVector3d)WorldRay.Direction));

		int32 Tid;
		double LocalHitT = TNumericLimits<double>::Max();
		if (MeshSpatial->FindNearestHitTriangle(LocalRay, LocalHitT, Tid))
		{
			double HitT = WorldRay.GetParameter(CurrentMeshTransform.TransformPosition(LocalRay.PointAt(LocalHitT)));
			if (HitT < BestHitT)
			{
				BestHitT = HitT;
				ConvertToFaceHit(*CubeGrid, Settings->FaceSelectionMode,
					WorldRay, BestHitT, CurrentMeshTransform.TransformNormal(CurrentMesh->GetTriNormal(Tid)), 
					FaceOut, Settings->PlaneTolerance);
			}
		}
	}

	// We can't go just off of BestHitT because we keep it maximal for plane hits when 
	// bHitGridGroundPlaneIfCloser is false.
	return bHitPlane || BestHitT != TNumericLimits<double>::Max();
}

FInputRayHit UCubeGridTool::CanBeginClickDragSequence(const FInputDeviceRay& PressPos)
{
	FInputRayHit HitResult;
	HitResult.bHit = (Mode != EMode::FitGrid && !(bBlockUntilPreviewUpdate && bWaitingToApplyPreview));
	return HitResult;
}


void UCubeGridTool::OnClickPress(const FInputDeviceRay& PressPos)
{
	using namespace CubeGridToolLocals;

	UpdateHoverLineSet(false, HoveredSelectionBox); // clear hover

	// Ctrl+drag extrude setting works in both corner mode and regular mode
	if (bMouseDragShouldPushPull)
	{
		MouseState = EMouseState::DraggingExtrudeDistance;
		if (bHaveSelection)
		{
			DragProjectionAxis = FRay3d(CubeGrid->GetFrame().FromFramePoint(Selection.Box.Center()),
				CubeGrid->GetFrame().FromFrameVector(FCubeGrid::DirToNormal(Selection.Direction)), true);

			FDistLine3Ray3d DistanceCalculator(UE::Geometry::FLine3d(DragProjectionAxis.Origin, DragProjectionAxis.Direction), (FRay3d)PressPos.WorldRay);
			DistanceCalculator.ComputeResult();
			DragProjectedStartParam = DistanceCalculator.LineParameter;
			DragStartExtrudeAmount = CurrentExtrudeAmount;
		}
		return;
	}

	// Deal with corner selection if in corner mode
	if (Mode == EMode::Corner)
	{
		MouseState = EMouseState::DraggingCornerSelection;
		for (int i = 0; i < 4; ++i)
		{
			PreDragCornerSelectedFlags[i] = CornerSelectedFlags[i];
		}
		AttemptToSelectCorner((FRay3d)PressPos.WorldRay);
		return;
	}

	// Otherwise, deal with selection
	PrepForSelectionChange();

	MouseState = EMouseState::DraggingRegularSelection;
	FCubeGrid::FCubeFace HitFace;
	if (bHaveSelection && bSelectionToggle)
	{
		// We're adding to existing selection
		if (GetCoplanarFrameSpaceSelectedBox(*CubeGrid, PressPos.WorldRay, Selection.StartBox, true, Selection.Box))
		{
			UpdateSelectionLineSet();
		}
	} 
	else if (GetHitGridFace(PressPos.WorldRay, HitFace))
	{
		// Reset start of the selection
		bHaveSelection = true;
		double GridScale = CubeGrid->GetCellSize(HitFace.GetSourceCubeGridPower());
		Selection.Box = FAxisAlignedBox3d(HitFace.GetMinCorner() * GridScale, HitFace.GetMaxCorner() * GridScale);
		Selection.StartBox = Selection.Box;
		Selection.Direction = HitFace.GetDirection();
		UpdateSelectionLineSet();
	}
	else
	{
		// Clear selection (the event emit, if needed, happens on click release) 
		bHaveSelection = false;
		Selection.Box = FAxisAlignedBox3d();
	}
}

void UCubeGridTool::OnClickDrag(const FInputDeviceRay& DragPos)
{
	using namespace CubeGridToolLocals;

	if (!bHaveSelection)
	{
		return;
	}

	if (MouseState == EMouseState::DraggingExtrudeDistance)
	{
		if (!bHaveSelection || (Mode == EMode::Corner && !IsAnyCornerSelected(CornerSelectedFlags)))
		{
			return;
		}

		FDistLine3Ray3d DistanceCalculator(
			UE::Geometry::FLine3d(DragProjectionAxis.Origin, DragProjectionAxis.Direction), (FRay3d)DragPos.WorldRay);
		DistanceCalculator.ComputeResult();

		double ParamDelta = DistanceCalculator.LineParameter - DragProjectedStartParam;
		double CubeSize = CubeGrid->GetCurrentGridCellSize();
		int32 NewExtrudeDelta = FMath::RoundToInt(ParamDelta / (CubeSize * Settings->BlocksPerStep)) * Settings->BlocksPerStep;
		int32 NewExtrudeAmount = DragStartExtrudeAmount + NewExtrudeDelta;
		if (NewExtrudeAmount != CurrentExtrudeAmount)
		{
			CurrentExtrudeAmount = NewExtrudeAmount;
			InvalidatePreview();
		}
		return;
	}
	else if (MouseState == EMouseState::DraggingCornerSelection)
	{
		AttemptToSelectCorner((FRay3d)DragPos.WorldRay);
		return;
	}
	else // Grid selection
	{
		FCubeGrid::FCubeFace HitFace;
		double HitT = TNumericLimits<double>::Max();
		bool bHit = GetCoplanarFrameSpaceSelectedBox(*CubeGrid, DragPos.WorldRay, Selection.StartBox,
			true, Selection.Box);

		UpdateSelectionLineSet();
	}
}

void UCubeGridTool::OnClickRelease(const FInputDeviceRay& ReleasePos)
{
	if (MouseState == EMouseState::DraggingExtrudeDistance)
	{
		// Only apply result if we're not in corner mode, because in corner mode
		// we apply when exiting corner mode (that behavior is particularly important 
		// when using E/Q to set extrude distance, to allow different slopes to be
		// set).
		if (Mode != EMode::Corner && CurrentExtrudeAmount != 0)
		{
			bWaitingToApplyPreview = true;
			bBlockUntilPreviewUpdate = false;
			bAdjustSelectionOnPreviewUpdate = true;
		}
	}
	else if (MouseState == EMouseState::DraggingRegularSelection)
	{
		EndSelectionChange();
	}

	MouseState = EMouseState::NotDragging;
}

void UCubeGridTool::OnTerminateDragSequence()
{
	if (MouseState == EMouseState::DraggingExtrudeDistance)
	{
		// Only apply result if we're not in corner mode
		if (Mode != EMode::Corner && CurrentExtrudeAmount != 0)
		{
			bWaitingToApplyPreview = true;
			bBlockUntilPreviewUpdate = false;
			bAdjustSelectionOnPreviewUpdate = true;
		}
	}

	MouseState = EMouseState::NotDragging;
}

void UCubeGridTool::AttemptToSelectCorner(const FRay3d& WorldRay)
{
	TArray<FGeometrySet3::FNearest> HitCorners;
	CornersGeometrySet.CollectPointsNearRay(WorldRay, HitCorners, [this](const FVector3d& Position1, const FVector3d& Position2) {
		double ToleranceScale = 3;
		if (CameraState.bIsOrthographic)
		{
			// We could just always use ToolSceneQueriesUtil::PointSnapQuery. But in ortho viewports, we happen to know
			// that the only points that we will ever give this function will be the closest points between a ray and
			// some geometry, meaning that the vector between them will be orthogonal to the view ray. With this knowledge,
			// we can do the tolerance computation more efficiently than PointSnapQuery can, since we don't need to project
			// down to the view plane.
			// As in PointSnapQuery, we convert our angle-based tolerance to one we can use in an ortho viewport (instead of
			// dividing our field of view into 90 visual angle degrees, we divide the plane into 90 units).
			float OrthoTolerance = ToolSceneQueriesUtil::GetDefaultVisualAngleSnapThreshD() * CameraState.OrthoWorldCoordinateWidth / 90.0;
			OrthoTolerance *= ToleranceScale;
			return FVector3d::DistSquared(Position1, Position2) < OrthoTolerance * OrthoTolerance;
		}
		else
		{
			return ToolSceneQueriesUtil::PointSnapQuery(CameraState,
				Position1, Position2,
				ToolSceneQueriesUtil::GetDefaultVisualAngleSnapThreshD() * ToleranceScale);
		}
		});

	for (FGeometrySet3::FNearest Hit : HitCorners)
	{
		CornerSelectedFlags[Hit.ID] = !PreDragCornerSelectedFlags[Hit.ID];
	}

	if (HitCorners.Num() > 0)
	{
		InvalidatePreview();
	}
}

void UCubeGridTool::UpdateSelectionLineSet()
{
	using namespace CubeGridToolLocals;

	ULineSetComponent* LineSet = LineSets->FindLineSet(SelectionLineSetID);
	LineSet->Clear();
	if (bHaveSelection)
	{
		FVector3d CornerVector = FCubeGrid::DirToNormal(Selection.Direction)
			* GetFrameSpaceExtrudeDist(*CubeGrid, Selection.Box.Min, -Settings->BlocksPerStep, Selection.Direction);
		DrawGridRectangle(*LineSet, *CubeGrid, Selection.StartBox.Min, Selection.StartBox.Max, SelectionLineColor,
			SelectionMainLineThickness, SelectionLineDepthBias);
		DrawGridRectangle(*LineSet, *CubeGrid, Selection.Box.Min, Selection.Box.Max, SelectionLineColor,
			SelectionMainLineThickness, SelectionLineDepthBias);
		DrawGridSection(*LineSet, *CubeGrid, Selection.Box, SelectionLineColor, 
			SelectionGridLineThickness, SelectionLineDepthBias, &CornerVector);

		if (Mode == EMode::Corner)
		{
			// This isn't quite relevant to updating the selection line set, but it's a convenient place
			// to put this because if the selection set is changing, the geometry set probably needs
			// to be doing the same.
			UpdateCornerGeometrySet();
		}
	}
}

FInputRayHit UCubeGridTool::BeginHoverSequenceHitTest(const FInputDeviceRay& PressPos)
{
	FInputRayHit HitResult;
	HitResult.bHit = UpdateHover(PressPos.WorldRay);
	return HitResult;
}

bool UCubeGridTool::UpdateHover(const FRay& WorldRay)
{
	using namespace CubeGridToolLocals;

	if (Mode != EMode::PushPull)
	{
		UpdateHoverLineSet(false, HoveredSelectionBox);
		return false;
	}

	bool bHit;
	UE::Geometry::FAxisAlignedBox3d Box;
	if (bHaveSelection && bSelectionToggle)
	{
		bHit = GetCoplanarFrameSpaceSelectedBox(*CubeGrid, WorldRay, Selection.StartBox, false, Box);
	}
	else
	{
		FCubeGrid::FCubeFace HitFace;
		bHit = GetHitGridFace(WorldRay, HitFace);
		if (bHit)
		{
			double HoverScale = CubeGrid->GetCurrentGridCellSize();
			Box = FAxisAlignedBox3d(HitFace.GetMinCorner() * HoverScale,
				HitFace.GetMaxCorner() * HoverScale);
		}
	}

	UpdateHoverLineSet(bHit, Box);

	return bHit;
}

void UCubeGridTool::OnBeginHover(const FInputDeviceRay& DevicePos)
{
}

bool UCubeGridTool::OnUpdateHover(const FInputDeviceRay& DevicePos)
{
	return UpdateHover(DevicePos.WorldRay);
}

// We could have not taken arguments here and done it the way we do selection, but we'd need
// to keep track of previous hover to avoid unnecessary updates
void UCubeGridTool::UpdateHoverLineSet(bool bNewHaveHover, const UE::Geometry::FAxisAlignedBox3d& NewHoveredBox)
{
	using namespace CubeGridToolLocals;

	ULineSetComponent* LineSet = LineSets->FindLineSet(HoverLineSetID);

	if (!bNewHaveHover)
	{
		if (bHaveHoveredSelection)
		{
			LineSet->Clear();
		}
	}
	else if (!bHaveHoveredSelection || NewHoveredBox != HoveredSelectionBox)
	{
		HoveredSelectionBox = NewHoveredBox;
		bHaveHoveredSelection = true;

		LineSet->Clear();
		DrawGridRectangle(*LineSet, *CubeGrid, HoveredSelectionBox.Min, HoveredSelectionBox.Max,
			HoverLineColor, HoverLineThickness, HoverLineDepthBias);
	}
}

void UCubeGridTool::OnEndHover()
{
}

void UCubeGridTool::OnUpdateModifierState(int ModifierID, bool bIsOn)
{
	if (ModifierID == ShiftModifierID)
	{
		bSelectionToggle = bIsOn;
	}
	else if (ModifierID == CtrlModifierID)
	{
		bMouseDragShouldPushPull = bIsOn;
	}
}

void UCubeGridTool::UpdateGridLineSet()
{
	using namespace CubeGridToolLocals;

	double CurrentGridScale = CubeGrid->GetCurrentGridCellSize();

	FAxisAlignedBox3d GridBox;
	GridBox.Contain(FVector3d(-50, -50, 0) * CurrentGridScale);
	GridBox.Contain(FVector3d(50, 50, 0) * CurrentGridScale);

	ULineSetComponent* LineSet = LineSets->FindLineSet(GridLineSetID);
	LineSet->Clear();
	DrawGridSection(*LineSet, *CubeGrid, GridBox,
		GridLineColor, GridLineThickness, GridLineDepthBias);
}

void UCubeGridTool::UpdateCornerModeLineSet()
{
	using namespace CubeGridToolLocals;
	ULineSetComponent* LineSet = LineSets->FindLineSet(CornerModeLineSetID);

	LineSet->Clear();
	if (Mode == EMode::Corner && CurrentExtrudeAmount < 0)
	{
		FOrientedBox3d FrameSpaceBox = ConvertToOrientedBox(Selection.Box, Selection.Direction);

		bool CornerWelded[4];
		for (int i = 0; i < 4; ++i) {
			CornerWelded[i] = !CornerSelectedFlags[i];
		}

		// The choice of diagonal here lines up with the generator in CubeGridBooleanOp. The
		// indices look a little different because we're accounting for mirroring that happens in
		// subtract mode, though we would have actually gotten the same results either way in
		// the cases we care about (the nonplanar ones)
		int DiagStartIdx = 1;
		if (CornerWelded[0] != CornerWelded[2] ||
			(!CornerWelded[0] && CornerWelded[1] && CornerWelded[3]))
		{
			DiagStartIdx = 0;
		}
		DiagStartIdx = Settings->bCrosswiseDiagonal ? 1 - DiagStartIdx : DiagStartIdx;
		
		bool bDiagonalWelded = CornerWelded[DiagStartIdx] && CornerWelded[DiagStartIdx + 2];
		int DeletedVert = -1;
		if (bDiagonalWelded)
		{
			if (CornerWelded[DiagStartIdx + 1])
			{
				DeletedVert = DiagStartIdx + 1;
			}
			else if (CornerWelded[(DiagStartIdx + 3) % 4])
			{
				DeletedVert = (DiagStartIdx + 3) % 4;
			}
		}

		FVector3d CornerExtrudeVector = CubeGrid->GetCurrentGridCellSize() * CurrentExtrudeAmount 
			* FCubeGrid::DirToNormal(Selection.Direction);

		for (int i = 0; i < 4; ++i)
		{
			FVector3d CurrentCorner = FrameSpaceBox.GetCorner(i);
			if (!CornerWelded[i])
			{
				FVector3d UpCorner = CurrentCorner + CornerExtrudeVector;
				LineSet->AddLine(CurrentCorner, UpCorner, CornerModeWireframeColor, 
					CornerModeWireframeThickness, CornerModeWireframeDepthBias);
				CurrentCorner = UpCorner;
			}

			int NextIdx = (i + 1) % 4;
			if (i == DeletedVert || NextIdx == DeletedVert)
			{
				continue;
			}

			FVector3d NextCorner = CornerWelded[NextIdx] ? FrameSpaceBox.GetCorner(NextIdx)
				: FrameSpaceBox.GetCorner(NextIdx) + CornerExtrudeVector;
			LineSet->AddLine(CurrentCorner, NextCorner, CornerModeWireframeColor,
				CornerModeWireframeThickness, CornerModeWireframeDepthBias);
		}

		FVector3d DiagCorner1 = CornerWelded[DiagStartIdx] ? FrameSpaceBox.GetCorner(DiagStartIdx)
			: FrameSpaceBox.GetCorner(DiagStartIdx) + CornerExtrudeVector;
		FVector3d DiagCorner2 = CornerWelded[DiagStartIdx + 2] ? FrameSpaceBox.GetCorner(DiagStartIdx + 2)
			: FrameSpaceBox.GetCorner(DiagStartIdx + 2) + CornerExtrudeVector;
		LineSet->AddLine(DiagCorner1, DiagCorner2, CornerModeWireframeColor,
			CornerModeWireframeThickness, CornerModeWireframeDepthBias);
	}
}

void UCubeGridTool::ApplyFlipSelection()
{
	if (!bHaveSelection)
	{
		return;
	}

	GetToolManager()->BeginUndoTransaction(LOCTEXT("FlipTransactionName", "Flip Selection"));

	FSelection NewSelection = Selection;
	NewSelection.Direction = FCubeGrid::FlipDir(Selection.Direction);
	SetSelection(NewSelection, true);

	GetToolManager()->EndUndoTransaction();

	// TODO: We actually probably want some special handling here in Corner mode. For one thing,
	// we're keeping the selected corners the same, which ends up rotating rather than mirroring
	// the currently pushed/pulled portion (should fix this, but would need another undo transaction,
	// at which point we probably want full undo support for corner mode, rather than our current
	// approach of keeping corner selection and extrude distance...). For another, might a user want
	// a flip in corner mode to equate to a reversal of push vs pull, instead of a mirror of the same
	// operation (i.e. you flip a pull and you get a mirrored push rather than mirrored pull)? Not certain.
}

void UCubeGridTool::ApplySlide(int32 NumBlocks)
{
	if (!bHaveSelection)
	{
		return;
	}

	GetToolManager()->BeginUndoTransaction(LOCTEXT("SlideTransactionName", "Slide Selection"));
	SlideSelection(NumBlocks, true);
	GetToolManager()->EndUndoTransaction();
}

void UCubeGridTool::ApplyPushPull(int32 NumBlocks)
{
	using namespace CubeGridToolLocals;

	if (!bHaveSelection || (Mode == EMode::Corner && !IsAnyCornerSelected(CornerSelectedFlags)))
	{
		return;
	}

	CurrentExtrudeAmount += NumBlocks;

	InvalidatePreview();

	if (Mode != EMode::Corner)
	{
		bWaitingToApplyPreview = true;
		bBlockUntilPreviewUpdate = false;
		bAdjustSelectionOnPreviewUpdate = true;
	}
}

void UCubeGridTool::SetPowerOfTwoClamped(int32 PowerOfTwo)
{
	Settings->PowerOfTwo = FMath::Clamp<int32>(PowerOfTwo, 0, Settings->MaxPowerOfTwo);
	CubeGrid->SetGridPower(Settings->PowerOfTwo);

	// Update CurrentExtrudeAmount to reflect the new step size (mainly important
	// for corner mode).
	int32 PowerOfTwoDifference = static_cast<int32>(Settings->PowerOfTwo) - PowerOfTwoPrevious;
	if (CurrentExtrudeAmount != 0 && PowerOfTwoDifference != 0)
	{
		int32 AbsExtrudeAmount = FMath::Abs(CurrentExtrudeAmount);

		int32 NewAbsExtrudeAmount = AbsExtrudeAmount >> PowerOfTwoDifference;
		if (NewAbsExtrudeAmount << PowerOfTwoDifference != AbsExtrudeAmount)
		{
			++NewAbsExtrudeAmount;
			InvalidatePreview();
		}

		CurrentExtrudeAmount = NewAbsExtrudeAmount * FMath::Sign(CurrentExtrudeAmount);
	}
	UpdateCornerModeLineSet();

	ClearHover();
	UpdateSelectionLineSet(); // Updates the grid drawn inside
	UpdateGridLineSet();

	PowerOfTwoPrevious = Settings->PowerOfTwo;
}

// Action support

void UCubeGridToolActions::PostAction(ECubeGridToolAction Action)
{
	if (ParentTool.IsValid())
	{
		ParentTool->RequestAction(Action);
	}
}

void UCubeGridDuringActivityActions::PostAction(ECubeGridToolAction Action)
{
	if (ParentTool.IsValid())
	{
		ParentTool->RequestAction(Action);
	}
}

void UCubeGridTool::RequestAction(ECubeGridToolAction ActionType)
{
	if (PendingAction == ECubeGridToolAction::NoAction)
	{
		PendingAction = ActionType;
	}
}

void UCubeGridTool::ApplyAction(ECubeGridToolAction ActionType)
{
	switch (ActionType)
	{
	case ECubeGridToolAction::Push:
		ApplyPushPull(-Settings->BlocksPerStep);
		break;
	case ECubeGridToolAction::Pull:
		ApplyPushPull(Settings->BlocksPerStep);
		break;
	case ECubeGridToolAction::Flip:
		ApplyFlipSelection();
		break;
	case ECubeGridToolAction::SlideForward:
		ApplySlide(-Settings->BlocksPerStep);
		break;
	case ECubeGridToolAction::SlideBack:
		ApplySlide(Settings->BlocksPerStep);
		break;
	case ECubeGridToolAction::DecreasePowerOfTwo:
		// cast is just to be explicit
		SetPowerOfTwoClamped(static_cast<int32>(Settings->PowerOfTwo) - 1); 
		break;
	case ECubeGridToolAction::IncreasePowerOfTwo:
		SetPowerOfTwoClamped(Settings->PowerOfTwo + 1);
		break;

	case ECubeGridToolAction::CornerMode:
		StartCornerMode();
		break;
	//case ECubeGridToolAction::FitGrid:
	//	StartFitGrid();
	//	break;
	case ECubeGridToolAction::Done:
		if (Mode == EMode::Corner)
		{
			ApplyCornerMode();
		}
		else if (Mode == EMode::FitGrid)
		{
			//CancelFitGrid();
		}
		break;
	case ECubeGridToolAction::Cancel:
		RevertToDefaultMode();
		break;
	}
}

void UCubeGridTool::RegisterActions(FInteractiveToolActionSet& ActionSet)
{
	int32 ActionID = (int32)EStandardToolActions::BaseClientDefinedActionID + 1;
	ActionSet.RegisterAction(this, ActionID++,
		TEXT("PullBlock"),
		LOCTEXT("PullAction", "Pull Out Blocks"),
		LOCTEXT("PullTooltip", ""),
		EModifierKey::None, EKeys::E,
		[this]() { RequestAction(ECubeGridToolAction::Pull); });
	ActionSet.RegisterAction(this, ActionID++,
		TEXT("PushBlock"),
		LOCTEXT("PushAction", "Push In Holes"),
		LOCTEXT("PushTooltip", ""),
		EModifierKey::None, EKeys::Q,
		[this]() { RequestAction(ECubeGridToolAction::Push); });
	ActionSet.RegisterAction(this, ActionID++,
		TEXT("SlideBack"),
		LOCTEXT("SlideBackAction", "Slide Selection Back"),
		LOCTEXT("SlideBackTooltip", ""),
		EModifierKey::Shift, EKeys::E,
		[this]() { RequestAction(ECubeGridToolAction::SlideBack); });
	ActionSet.RegisterAction(this, ActionID++,
		TEXT("SlideForward"),
		LOCTEXT("SlideForwardAction", "Slide Selection Forward"),
		LOCTEXT("SlideForwardTooltip", ""),
		EModifierKey::Shift, EKeys::Q,
		[this]() { RequestAction(ECubeGridToolAction::SlideForward); });

	ActionSet.RegisterAction(this, ActionID++,
		TEXT("DecreasePowerOfTwo"),
		LOCTEXT("DecreasePowerOfTwoAction", "Decrease Power Of Two"),
		LOCTEXT("DecreasePowerOfTwoTooltip", ""),
		EModifierKey::Control, EKeys::Q,
		[this]() { RequestAction(ECubeGridToolAction::DecreasePowerOfTwo); });
	ActionSet.RegisterAction(this, ActionID++,
		TEXT("IncreasePowerOfTwo"),
		LOCTEXT("IncreasePowerOfTwoAction", "Increase Power Of Two"),
		LOCTEXT("IncreasePowerOfTwoTooltip", ""),
		EModifierKey::Control, EKeys::E,
		[this]() { RequestAction(ECubeGridToolAction::IncreasePowerOfTwo); });

	ActionSet.RegisterAction(this, ActionID++,
		TEXT("ToggleGizmoVisibility"),
		LOCTEXT("ToggleGizmoVisibilityAction", "Toggle Gizmo Visibility"),
		LOCTEXT("ToggleGizmoVisibilityTooltip", ""),
		EModifierKey::None, EKeys::R,
		[this]() {
			if (Mode != EMode::FitGrid)
			{
				UpdateGizmoVisibility(!GridGizmo->IsVisible());
			}
		});

	ActionSet.RegisterAction(this, ActionID++,
		TEXT("ToggleCornerMode"),
		LOCTEXT("ToggleCornerModeAction", "Toggle Corner Mode"),
		LOCTEXT("ToggleCornerModeTooltip", ""),
		EModifierKey::None, EKeys::Z,
		[this]() {
			if (Mode != EMode::Corner)
			{
				StartCornerMode();
			}
			else
			{
				ApplyCornerMode();
			}
		});

	ActionSet.RegisterAction(this, ActionID++,
		TEXT("ToggleDiagonalMode"),
		LOCTEXT("ToggleDiagonalModeAction", "Toggle Diagonal Mode"),
		LOCTEXT("ToggleDiagonalModeTooltip", ""),
		EModifierKey::None, EKeys::X,
		[this]() {
			if (Mode == EMode::Corner)
			{
				Settings->bCrosswiseDiagonal = !Settings->bCrosswiseDiagonal;
			}
		});

	ActionSet.RegisterAction(this, ActionID++,
		TEXT("FlipSelection"),
		LOCTEXT("FlipSelectionAction", "Flip Selection"),
		LOCTEXT("FlipSelectionTooltip", ""),
		EModifierKey::None, EKeys::T,
		[this]() {
			ApplyFlipSelection();
		});
}

void UCubeGridTool::StartCornerMode()
{
	using namespace CubeGridToolLocals;

	if (!bHaveSelection)
	{
		// TODO: Write out a message here and clear it at some point
		return;
	}
	if (Mode == EMode::Corner)
	{
		return; // Already in mode
	}

	// Clear/cancel stuff
	//if (Mode == EMode::FitGrid)
	//{
	//	CancelFitGrid();
	//}
	CurrentExtrudeAmount = 0;
	InvalidatePreview();

	// Clear selected corner render
	for (int i = 0; i < 4; ++i)
	{
		CornerSelectedFlags[i] = false;
		PreDragCornerSelectedFlags[i] = false;
	}

	UpdateCornerGeometrySet();
	Mode = EMode::Corner;

	SetToolPropertySourceEnabled(ToolActions, false);
	SetToolPropertySourceEnabled(DuringActivityActions, true);

	Settings->bInCornerMode = true;
	NotifyOfPropertyChangeByTool(Settings);

	GetToolManager()->BeginUndoTransaction(ModeChangeTransactionName);
	GetToolManager()->EmitObjectChange(this, MakeUnique<FCubeGridToolModeChange>(), ModeChangeTransactionName);
	GetToolManager()->EndUndoTransaction();

	GetToolManager()->DisplayMessage(CornerModeMessage, EToolMessageLevel::UserNotification);
}

void UCubeGridTool::UpdateCornerGeometrySet()
{
	using namespace CubeGridToolLocals;

	FOrientedBox3d FrameSpaceBox = ConvertToOrientedBox(Selection.Box, Selection.Direction);
	CornersGeometrySet.Reset();
	for (int i = 0; i < 4; ++i)
	{
		CornersGeometrySet.AddPoint(i, CubeGrid->GetFrame().FromFramePoint(FrameSpaceBox.GetCorner(i)));
	}
}

void UCubeGridTool::ApplyCornerMode(bool bDontWaitForTick)
{
	using namespace CubeGridToolLocals;

	if (CurrentExtrudeAmount != 0 && IsAnyCornerSelected(CornerSelectedFlags))
	{
		bWaitingToApplyPreview = true;
		bBlockUntilPreviewUpdate = true;
		bAdjustSelectionOnPreviewUpdate = false;

		if (bDontWaitForTick)
		{
			ApplyPreview();
		}
	}

	CornersGeometrySet.Reset();

	Mode = EMode::PushPull;
	GetToolManager()->DisplayMessage(PushPullModeMessage, EToolMessageLevel::UserNotification);
	SetToolPropertySourceEnabled(ToolActions, true);
	SetToolPropertySourceEnabled(DuringActivityActions, false);

	Settings->bInCornerMode = false;
	NotifyOfPropertyChangeByTool(Settings);

	UpdateCornerModeLineSet();
}

void UCubeGridTool::CancelCornerMode()
{
	using namespace CubeGridToolLocals;

	CornersGeometrySet.Reset();

	Mode = EMode::PushPull;
	GetToolManager()->DisplayMessage(PushPullModeMessage, EToolMessageLevel::UserNotification);
	SetToolPropertySourceEnabled(ToolActions, true);
	SetToolPropertySourceEnabled(DuringActivityActions, false);

	CurrentExtrudeAmount = 0;
	InvalidatePreview();

	Settings->bInCornerMode = false;
	NotifyOfPropertyChangeByTool(Settings);

	UpdateCornerModeLineSet();
}

void UCubeGridTool::UpdateUsingMeshChange(const FDynamicMeshChange& MeshChange, bool bRevert)
{
	MeshChange.Apply(CurrentMesh.Get(), bRevert);
	MeshSpatial->Build();
	UpdateComputeInputs();
	CurrentExtrudeAmount = 0;
	bPreviewMayDiffer = true;
	InvalidatePreview();
}

bool UCubeGridTool::IsInDefaultMode() const
{
	return Mode == EMode::PushPull;
}

void UCubeGridTool::RevertToDefaultMode()
{
	if (Mode == EMode::Corner)
	{
		CancelCornerMode();
	}
	else if (Mode == EMode::FitGrid)
	{
		//CancelFitGrid();
	}
}

bool UCubeGridTool::CanCurrentlyNestedCancel()
{
	return Mode == EMode::Corner || bHaveSelection;
}

bool UCubeGridTool::ExecuteNestedCancelCommand()
{
	if (!IsInDefaultMode())
	{
		RevertToDefaultMode();
		return true;
	}
	else if (bHaveSelection)
	{
		ClearSelection(true);
		return true;
	}
	return false;
}

bool UCubeGridTool::CanCurrentlyNestedAccept()
{
	return Mode == EMode::Corner;
}

bool UCubeGridTool::ExecuteNestedAcceptCommand()
{
	if (Mode == EMode::Corner)
	{
		ApplyCornerMode();
		return true;
	}
	return false;
}

#undef LOCTEXT_NAMESPACE
