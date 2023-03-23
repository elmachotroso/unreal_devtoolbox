// Copyright Epic Games, Inc. All Rights Reserved.

#include "DatasmithCADTranslator.h"

#include "CADFileReader.h"
#include "CADInterfacesModule.h"
#include "CADKernelSurfaceExtension.h"

#include "DatasmithCADTranslatorModule.h"
#include "DatasmithDispatcher.h"
#include "DatasmithMeshBuilder.h"
#include "DatasmithSceneGraphBuilder.h"
#include "DatasmithTranslator.h"
#include "DatasmithUtils.h"
#include "IDatasmithSceneElements.h"


DEFINE_LOG_CATEGORY(LogCADTranslator);


void FDatasmithCADTranslator::Initialize(FDatasmithTranslatorCapabilities& OutCapabilities)
{
	if (ICADInterfacesModule::GetAvailability() == ECADInterfaceAvailability::Unavailable)
	{
		OutCapabilities.bIsEnabled = false;
		return;
	}

#ifndef CAD_TRANSLATOR_DEBUG
	OutCapabilities.bParallelLoadStaticMeshSupported = true;
#endif
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("CATPart"), TEXT("CATIA Part files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("CATProduct"), TEXT("CATIA Product files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("cgr"), TEXT("CATIA Graphical Representation V5 files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("3dxml"), TEXT("CATIA files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("3drep"), TEXT("CATIA files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("model"), TEXT("CATIA V4 files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("asm.*"), TEXT("Creo Assembly files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("creo.*"), TEXT("Creo Assembly files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("creo"), TEXT("Creo Assembly files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("neu.*"), TEXT("Creo Assembly files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("neu"), TEXT("Creo Assembly files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("prt.*"), TEXT("Creo Part files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("xas"), TEXT("Creo Assembly files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("xpr"), TEXT("Creo Part files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("iam"), TEXT("Inventor Assembly files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("ipt"), TEXT("Inventor Part files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("iges"), TEXT("IGES files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("igs"), TEXT("IGES files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("jt"), TEXT("JT Open files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("sat"), TEXT("3D ACIS model files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("SLDASM"), TEXT("SolidWorks Product files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("SLDPRT"), TEXT("SolidWorks Part files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("step"), TEXT("Step files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("stp"), TEXT("Step files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("xml"), TEXT("AP242 Xml Step files, XPDM files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("x_t"), TEXT("Parasolid files (Text format)") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("x_b"), TEXT("Parasolid files (Binary format)") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("asm"), TEXT("Unigraphics, NX, SolidEdge Assembly files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("prt"), TEXT("Unigraphics, NX Part files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("par"), TEXT("SolidEdge Part files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("psm"), TEXT("SolidEdge Part files") });

	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("dwg"), TEXT("AutoCAD, Model files") });
	OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("dgn"), TEXT("MicroStation files") });

	if (CADLibrary::FImportParameters::GCADLibrary.Equals(TEXT("TechSoft")))
	{
		OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("hsf"), TEXT("HOOPS stream files") });
		OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("prc"), TEXT("HOOPS stream files") });
	}
	else
	{
		OutCapabilities.SupportedFileFormats.Add(FFileFormatInfo{ TEXT("ct"), TEXT("Kernel_IO files") });
	}
}

bool FDatasmithCADTranslator::IsSourceSupported(const FDatasmithSceneSource& Source)
{
	if (Source.GetSourceFileExtension() != TEXT("xml"))
	{
		return true;
	}

	return Datasmith::CheckXMLFileSchema(Source.GetSourceFile(), TEXT("XPDMXML"), TEXT("ns3:Uos"));
}

bool FDatasmithCADTranslator::LoadScene(TSharedRef<IDatasmithScene> DatasmithScene)
{
	const FDatasmithTessellationOptions& TesselationOptions = GetCommonTessellationOptions();
	CADLibrary::FFileDescriptor FileDescriptor(*FPaths::ConvertRelativePathToFull(GetSource().GetSourceFile()));

	UE_LOG(LogCADTranslator, Display, TEXT("CAD translation [%s]."), *FileDescriptor.GetSourcePath());
	UE_LOG(LogCADTranslator, Display, TEXT(" - Parsing Library:     %s"), *CADLibrary::FImportParameters::GCADLibrary);
	UE_LOG(LogCADTranslator, Display, TEXT(" - Tesselation Library: %s")
		, CADLibrary::FImportParameters::bGDisableCADKernelTessellation ? *CADLibrary::FImportParameters::GCADLibrary : TEXT("CADKernel"));
	UE_LOG(LogCADTranslator, Display, TEXT(" - Cache mode:          %s")
		, CADLibrary::FImportParameters::bGEnableCADCache ? (CADLibrary::FImportParameters::bGOverwriteCache ? TEXT("Override") : TEXT("Enabled")) : TEXT("Disabled"));
	UE_LOG(LogCADTranslator, Display, TEXT(" - Processing:          %s")
		, CADLibrary::FImportParameters::bGEnableCADCache ? (CADLibrary::GMaxImportThreads == 1 ? TEXT("Sequencial") : TEXT("Parallel")) : TEXT("Sequencial"));

	ImportParameters.SetTesselationParameters(TesselationOptions.ChordTolerance, TesselationOptions.MaxEdgeLength, TesselationOptions.NormalTolerance, (CADLibrary::EStitchingTechnique)TesselationOptions.StitchingTechnique);
	ImportParameters.SetModelCoordinateSystem(FDatasmithUtils::EModelCoordSystem::ZUp_RightHanded);

	switch (FileDescriptor.GetFileFormat())
	{
	case CADLibrary::ECADFormat::NX:
	{
		ImportParameters.SetDisplayPreference(CADLibrary::EDisplayPreference::ColorOnly);
		ImportParameters.SetPropagationMode(CADLibrary::EDisplayDataPropagationMode::BodyOnly);
		break;
	}

	case CADLibrary::ECADFormat::SOLIDWORKS:
	{
		ImportParameters.SetModelCoordinateSystem(FDatasmithUtils::EModelCoordSystem::YUp_RightHanded);
		ImportParameters.SetDisplayPreference(CADLibrary::EDisplayPreference::ColorOnly);
		break;
	}

	case CADLibrary::ECADFormat::INVENTOR:
	case CADLibrary::ECADFormat::CREO:
	{
		ImportParameters.SetModelCoordinateSystem(FDatasmithUtils::EModelCoordSystem::YUp_RightHanded);
		ImportParameters.SetDisplayPreference(CADLibrary::EDisplayPreference::ColorOnly);
		ImportParameters.SetPropagationMode(CADLibrary::EDisplayDataPropagationMode::BodyOnly);
		break;
	}

	case CADLibrary::ECADFormat::DWG:
	{
		ImportParameters.SetDisplayPreference(CADLibrary::EDisplayPreference::ColorOnly);
		ImportParameters.SetPropagationMode(CADLibrary::EDisplayDataPropagationMode::BodyOnly);
		break;
	}

	default:
		break;
	}

	FString CachePath = FDatasmithCADTranslatorModule::Get().GetCacheDir();
	if (!CachePath.IsEmpty())
	{
		CachePath = FPaths::ConvertRelativePathToFull(CachePath);
	}

	// Use sequential translation (multi-processed or not)
	if (CADLibrary::FImportParameters::bGEnableCADCache)
	{
		TMap<uint32, FString> CADFileToUEFileMap;
		{
			int32 NumCores = FPlatformMisc::NumberOfCores();
			if (CADLibrary::GMaxImportThreads > 1)
			{
				NumCores = FMath::Min(CADLibrary::GMaxImportThreads, NumCores);
			}
			DatasmithDispatcher::FDatasmithDispatcher Dispatcher(ImportParameters, CachePath, NumCores, CADFileToUEFileMap, CADFileToUEGeomMap);
			Dispatcher.AddTask(FileDescriptor);

			Dispatcher.Process(CADLibrary::GMaxImportThreads != 1);
		}

		FDatasmithSceneGraphBuilder SceneGraphBuilder(CADFileToUEFileMap, CachePath, DatasmithScene, GetSource(), ImportParameters);
		SceneGraphBuilder.Build();

		MeshBuilderPtr = MakeUnique<FDatasmithMeshBuilder>(CADFileToUEGeomMap, CachePath, ImportParameters);

		return true;
	}

	CADLibrary::FCADFileReader FileReader(ImportParameters, FileDescriptor, *FPaths::EnginePluginsDir(), CachePath);
	if (FileReader.ProcessFile() != CADLibrary::ECADParsingResult::ProcessOk)
	{
		return false;
	}

	CADLibrary::FCADFileData& CADFileData = FileReader.GetCADFileData();
	FDatasmithSceneBaseGraphBuilder SceneGraphBuilder(&CADFileData.GetSceneGraphArchive(), CachePath, DatasmithScene, GetSource(), ImportParameters);
	SceneGraphBuilder.Build();

	MeshBuilderPtr = MakeUnique<FDatasmithMeshBuilder>(CADFileData.GetBodyMeshes(), ImportParameters);

	return true;
}

void FDatasmithCADTranslator::UnloadScene()
{
	MeshBuilderPtr.Reset();

	CADFileToUEGeomMap.Empty();
}

bool FDatasmithCADTranslator::LoadStaticMesh(const TSharedRef<IDatasmithMeshElement> MeshElement, FDatasmithMeshElementPayload& OutMeshPayload)
{
	if (!MeshBuilderPtr.IsValid())
	{
		return false;
	}

	CADLibrary::FMeshParameters MeshParameters;

	if (TOptional< FMeshDescription > Mesh = MeshBuilderPtr->GetMeshDescription(MeshElement, MeshParameters))
	{
		OutMeshPayload.LodMeshes.Add(MoveTemp(Mesh.GetValue()));

		if (CADLibrary::FImportParameters::bGDisableCADKernelTessellation)
		{
			ParametricSurfaceUtils::AddSurfaceData(MeshElement->GetFile(), ImportParameters, MeshParameters, GetCommonTessellationOptions(), OutMeshPayload);
		}
		else
		{
			CADKernelSurface::AddSurfaceDataForMesh(MeshElement->GetFile(), ImportParameters, MeshParameters, GetCommonTessellationOptions(), OutMeshPayload);
		}
	}

	return OutMeshPayload.LodMeshes.Num() > 0;
}

