// Copyright Epic Games, Inc. All Rights Reserved.

#include "CADKernel/Core/Types.h"

#define private public
#define protected public
#include "CADKernel/Geo/Sampling/PolylineTools.h"
#include "CADKernel/Geo/Sampling/Polyline.h"
#include "CADKernel/Geo/Sampling/SurfacicPolyline.h"

namespace CADKernel
{
#ifdef WIN32
int FMessage::NumberOfIndentation = 0;


FString FProgressManager::GetCurrentStep() const
{
	return FString();
}

void FProgressManager::SetCurrentProgress(FProgress* InNewCurrent)
{
}

void FMessage::VPrintf(EVerboseLevel Level, const TCHAR* Text, ...)
{
}

void FMessage::VReportPrintF(FString Header, const TCHAR* Text, ...)
{
}
bool FEntity::SetId(FDatabase&)
{
	return true;
}

void FParameter::AddToParameterMap(FParameter&, FParameters&)
{
}

double FCurve::ComputeLength(FLinearBoundary const&, double) const
{
	return 0;
}

double FCurve::ComputeLength2D(FLinearBoundary const&, double) const
{
	return 0;
}
#endif 

void UpdateSubPolylineBBox(const FPolyline3D& Polyline, const FLinearBoundary& IntersectionBoundary, FPolylineBBox& IsoBBox)
{
	int32 BoundaryIndices[2];
	Polyline.Approximator.GetStartEndIndex(IntersectionBoundary, BoundaryIndices);
	if (BoundaryIndices[1] - BoundaryIndices[0] <= 0)
	{
		IsoBBox.Update(IntersectionBoundary.Min, Polyline.Approximator.ComputePoint(BoundaryIndices[0], IntersectionBoundary.Min));
		IsoBBox.Update(IntersectionBoundary.Max, Polyline.Approximator.ComputePoint(BoundaryIndices[1], IntersectionBoundary.Max));
	}
	else
	{
		Polyline.Approximator.UpdateSubPolylineBBox(IntersectionBoundary, IsoBBox);
	}
}

}
