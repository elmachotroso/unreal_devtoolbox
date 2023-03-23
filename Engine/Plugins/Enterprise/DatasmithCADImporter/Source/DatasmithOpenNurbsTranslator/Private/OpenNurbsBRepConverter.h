// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

class ON_Brep;
class ON_3dVector;

class IOpenNurbsBRepConverter
{
public:
	virtual bool AddBRep(ON_Brep& Brep, const ON_3dVector& Offset) = 0;
	
	void SetScaleFactor(double NewScaleFactor)
	{
		if (!FMath::IsNearlyEqual(NewScaleFactor, 1.) && !FMath::IsNearlyZero(NewScaleFactor))
		{
			ScaleFactor = NewScaleFactor;
		}
	}

protected:

	/**
	 * Scale factor between OpenNurbs and the external modeler (CADKernel, TechSoft, ...)
	 * By default OpenNurbs unit is mm
	 */
	double ScaleFactor = 1;
};

