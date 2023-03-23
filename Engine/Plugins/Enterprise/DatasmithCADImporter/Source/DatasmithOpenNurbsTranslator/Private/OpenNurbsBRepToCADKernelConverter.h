// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "CoreMinimal.h"

#ifdef USE_OPENNURBS

#include "CADModelToCADKernelConverterBase.h"
#include "OpenNurbsBRepConverter.h"

#include "CADKernel/Core/Session.h"
#include "CADKernel/Core/Types.h"

class ON_BoundingBox;
class ON_Brep;
class ON_BrepFace;
class ON_BrepLoop;
class ON_BrepTrim;
class ON_NurbsSurface;
class ON_3dVector;

namespace CADKernel
{
class FShell;
class FSurface;
class FTopologicalEdge;
class FTopologicalFace;
class FTopologicalLoop;
}

class FOpenNurbsBRepToCADKernelConverter : public FCADModelToCADKernelConverterBase, public IOpenNurbsBRepConverter
{
public:

	FOpenNurbsBRepToCADKernelConverter(const CADLibrary::FImportParameters& InImportParameters)
		: FCADModelToCADKernelConverterBase(InImportParameters)
	{
	}

	virtual ~FOpenNurbsBRepToCADKernelConverter() = default;

	/**
	 * Set BRep to tessellate, offsetting it prior to tessellation(used to set mesh pivot at the center of the surface bounding box)
	 *
	 * @param  Brep	a brep to tessellate
	 * @param  Offset translate brep by this value before tessellating
	 */
	bool AddBRep(ON_Brep& Brep, const ON_3dVector& Offset);

	static TSharedPtr<FOpenNurbsBRepToCADKernelConverter> GetSharedSession();

private:
	TSharedPtr<CADKernel::FTopologicalFace> AddFace(const ON_BrepFace& OpenNurbsFace);
	TSharedRef<CADKernel::FSurface> AddSurface(ON_NurbsSurface& Surface);

	TSharedPtr<CADKernel::FTopologicalLoop> AddLoop(const ON_BrepLoop& OpenNurbsLoop, TSharedRef<CADKernel::FSurface>& CarrierSurface, const bool bIsExternal);

	/**
	 * Build face's links with its neighbor have to be done after the loop is finalize.
	 * This is to avoid to link an edge with another and then to delete it...
	 */
	void LinkEdgesLoop(const ON_BrepLoop& OpenNurbsLoop, CADKernel::FTopologicalLoop& Loop);

	TSharedPtr<CADKernel::FTopologicalEdge> AddEdge(const ON_BrepTrim& OpenNurbsTrim, TSharedRef<CADKernel::FSurface>& CarrierSurface);

protected:

	TMap<int32, TSharedPtr<CADKernel::FTopologicalEdge>>  OpenNurbsTrimId2CADKernelEdge;

};

#endif // USE_OPENNURBS