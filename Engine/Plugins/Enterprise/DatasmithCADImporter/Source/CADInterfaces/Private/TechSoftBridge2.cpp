// Copyright Epic Games, Inc. All Rights Reserved.

#include "TechSoftBridge.h"

#ifdef USE_TECHSOFT_SDK



#include "TechSoftInterface.h"
#include "TUniqueTechSoftObj.h"

#define private public
#define protected public
#include "CADKernel/Core/Entity.h"
#include "CADKernel/Geo/Surfaces/Surface.h"
#include "CADKernel/Math/Boundary.h"

namespace CADLibrary
{
namespace TechSoftUtilsTmp
{

CADKernel::FMatrixH CreateCoordinateSystem(const A3DMiscCartesianTransformationData& Transformation, double UnitScale = 1.0)
{
	CADKernel::FPoint Origin(&Transformation.m_sOrigin.m_dX);
	CADKernel::FPoint Ox(&Transformation.m_sXVector.m_dX);
	CADKernel::FPoint Oy(&Transformation.m_sYVector.m_dX);

	Ox.Normalize();
	Oy.Normalize();

	if (!FMath::IsNearlyEqual(UnitScale, 1.))
	{
		Origin *= UnitScale;
	}
	CADKernel::FPoint Oz = Ox ^ Oy;

	CADKernel::FMatrixH Matrix = CADKernel::FMatrixH(Origin, Ox, Oy, Oz);

	if (!FMath::IsNearlyEqual(Transformation.m_sScale.m_dX, 1.) || !FMath::IsNearlyEqual(Transformation.m_sScale.m_dY, 1.) || !FMath::IsNearlyEqual(Transformation.m_sScale.m_dZ, 1.))
	{
		CADKernel::FMatrixH Scale = CADKernel::FMatrixH::MakeScaleMatrix(Transformation.m_sScale.m_dX, Transformation.m_sScale.m_dY, Transformation.m_sScale.m_dZ);
		Matrix *= Scale;
	}
	return Matrix;
}

CADKernel::FSurfacicBoundary GetSurfacicBoundary(A3DDomainData& Domain, const TechSoftUtils::FUVReparameterization& UVReparameterization)
{

	CADKernel::FPoint2D Min(Domain.m_sMin.m_dX, Domain.m_sMin.m_dY);
	CADKernel::FPoint2D Max(Domain.m_sMax.m_dX, Domain.m_sMax.m_dY);

	if (UVReparameterization.GetNeedApply())
	{
		UVReparameterization.Apply(Min);
		UVReparameterization.Apply(Max);
	}

	CADKernel::EIso UIndex = UVReparameterization.GetSwapUV() ? CADKernel::EIso::IsoV : CADKernel::EIso::IsoU;
	CADKernel::EIso VIndex = UVReparameterization.GetSwapUV() ? CADKernel::EIso::IsoU : CADKernel::EIso::IsoV;

	CADKernel::FSurfacicBoundary Boundary;
	Boundary[UIndex].Min = Min.U;
	Boundary[VIndex].Min = Min.V;
	Boundary[UIndex].Max = Max.U;
	Boundary[VIndex].Max = Max.V;

	return Boundary;
}

}

void TrimSurface(TSharedRef<CADKernel::FSurface>& Surface, const CADKernel::FSurfacicBoundary& SurfaceBoundary)
{
	TFunction<void(CADKernel::EIso)> TrimAt = [&](CADKernel::EIso Iso)
	{
		CADKernel::FLinearBoundary& Boundary = Surface->Boundary[Iso];
		const CADKernel::FLinearBoundary& MaxBound = SurfaceBoundary.Get(Iso);

		if (Boundary.GetMax() < MaxBound.Min || Boundary.GetMin() > MaxBound.Max)
		{
			Boundary = MaxBound;
			return;
		}
		Boundary.TrimAt(MaxBound);
	};

	TrimAt(CADKernel::EIso::IsoU);
	TrimAt(CADKernel::EIso::IsoV);
}

}


#endif // USE_TECHSOFT_SDK
