// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "ICurveEditorDragOperation.h"
#include "CurveDataAbstraction.h"

class FCurveEditor;

class FCurveEditorDragOperation_Tangent : public ICurveEditorKeyDragOperation
{
public:

	virtual void OnInitialize(FCurveEditor* InCurveEditor, const TOptional<FCurvePointHandle>& InCardinalPoint) override;
	virtual void OnBeginDrag(FVector2D InitialPosition, FVector2D CurrentPosition, const FPointerEvent& MouseEvent) override;
	virtual void OnDrag(FVector2D InitialPosition, FVector2D CurrentPosition, const FPointerEvent& MouseEvent) override;
	virtual void OnEndDrag(FVector2D InitialPosition, FVector2D CurrentPosition, const FPointerEvent& MouseEvent) override;
	virtual void OnCancelDrag() override;

private:

	/** Round a trajectory to the nearest 45 degrees */
	static FVector2D RoundTrajectory(FVector2D Delta);

private:

	/** Ptr back to the curve editor */
	FCurveEditor* CurveEditor;

private:

	struct FKeyData
	{
		FKeyData(FCurveModelID InCurveID)
			: CurveID(InCurveID)
		{}

		/** The curve that contains the keys we're dragging */
		FCurveModelID CurveID;
		/** All the handles within a given curve that we are dragging */
		TArray<FKeyHandle> Handles;
		/** All the point types within a given curve that we are dragging */
		TArray<ECurvePointType> PointTypes;
		/** The key attributes for each of the above handles */
		TArray<FKeyAttributes> Attributes;
		/** Used in OnEndDrag to send final key updates */
		TArray<FKeyAttributes> LastDraggedAttributes;
	};

	/** Key dragging data stored per-curve */
	TArray<FKeyData> KeysByCurve;
};