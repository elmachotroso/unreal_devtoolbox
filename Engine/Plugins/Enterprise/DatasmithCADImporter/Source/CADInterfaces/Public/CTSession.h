// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "CoreTechTypes.h"
#include "CADOptions.h"

struct FMeshDescription;

// Fill data array with a debug value (eg -1) to help debugging
#define MARK_UNINITIALIZED_MEMORY 0

namespace CADLibrary
{
class CADINTERFACES_API FCTSession : public FCoreTechSessionBase
{
public:
	/**
	 * Make sure CT is initialized, and a main object is ready.
	 * Handle input file unit and an output unit
	 * @param InOwner:        text that describe the owner of the session (helps to fix initialization issues)
	 */
	FCTSession(const TCHAR* InOwner, const CADLibrary::FImportParameters& InImportParameters)
		: FCoreTechSessionBase(InOwner)
		, ImportParams(InImportParameters)
	{
	}

	void ClearData();

	bool SaveBrep(const FString& FilePath);

	/**
 	 * This function calls, according to the chosen EStitchingTechnique, Kernel_io CT_REPAIR_IO::Sew or CT_REPAIR_IO::Heal. In case of sew, the used tolerance is 100x the geometric tolerance (SewingToleranceFactor = 100). 
	 * With the case of UE-83379, Alias file, this value is too big (biggest than the geometric features. So Kernel_io hangs during the sew process... In the wait of more test, 100x is still the value used for CAD import except for Alias where the value of the SewingToleranceFactor is set to 1x
	 * @param SewingToleranceFactor Factor apply to the tolerance 3D to define the sewing tolerance. 
	 */
	bool TopoFixes(double SewingToleranceFactor = 100);

	double GetScaleFactor() const
	{
		return ImportParams.GetScaleFactor();
	}

	double GetSceneUnit() const
	{
		return ImportParams.GetMetricUnit();
	}

	/**
	 * Handle input file unit
	 * @param FileMetricUnit: number of meters per file unit.
	 * e.g. For a file in inches, arg should be 0.0254
	 */
	void SetSceneUnit(double InMetricUnit);

	/**
	 * Set Import parameters,
	 * Tack care to set scale factor before because import parameters will be scale according to scale factor
	 * @param ChordTolerance : SAG	
	 * @param MaxEdgeLength : max length of element's edge
	 * @param NormalTolerance : Angle between two adjacent triangles
	 * @param StitchingTechnique : CAD topology correction technique
	 */
	void SetImportParameters(double ChordTolerance, double MaxEdgeLength, double NormalTolerance, CADLibrary::EStitchingTechnique StitchingTechnique);
	
	void SetModelCoordinateSystem(FDatasmithUtils::EModelCoordSystem NewCoordinateSystem)
	{
		ImportParams.SetModelCoordinateSystem(NewCoordinateSystem);
	}

	const CADLibrary::FImportParameters& GetImportParameters() const
	{
		return ImportParams;
	}

protected:
	CADLibrary::FImportParameters ImportParams;
	static TWeakPtr<FCTSession> SharedSession;
};

}

