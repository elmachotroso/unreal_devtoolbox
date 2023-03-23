// Copyright Epic Games, Inc. All Rights Reserved.

#include "Rendering/SkeletalMeshRenderData.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshLODModel.h"
#include "Engine/SkeletalMesh.h"
#include "UObject/Package.h"
#include "Algo/AnyOf.h"

#if WITH_EDITOR
#include "ProfilingDebugging/CookStats.h"
#include "DerivedDataCacheInterface.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Interfaces/ITargetPlatformManagerModule.h"
#include "Interfaces/ITargetPlatform.h"
#include "Misc/CoreMisc.h"
#include "PlatformInfo.h"
#include "IMeshBuilderModule.h"
#include "EngineUtils.h"
#include "Serialization/LargeMemoryReader.h"
#include "Serialization/LargeMemoryWriter.h"
#include "Async/Async.h"
#include "UObject/GarbageCollection.h"

#if ENABLE_COOK_STATS
namespace SkeletalMeshCookStats
{
	static FCookStats::FDDCResourceUsageStats UsageStats;
	static FCookStatsManager::FAutoRegisterCallback RegisterCookStats([](FCookStatsManager::AddStatFuncRef AddStat)
	{
		UsageStats.LogStats(AddStat, TEXT("SkeletalMesh.Usage"), TEXT(""));
	});
}
#endif

extern int32 GStripSkeletalMeshLodsDuringCooking;

#endif // WITH_EDITOR

static TAutoConsoleVariable<int32> CVarSkeletalMeshKeepMobileMinLODSettingOnDesktop(
	TEXT("r.SkeletalMesh.KeepMobileMinLODSettingOnDesktop"),
	0,
	TEXT("If non-zero, mobile setting for MinLOD will be stored in the cooked data for desktop platforms"));

#if WITH_EDITOR

/** 
 * Utility functions for storing and accessing data that exceeds the usual signed 32bit limits 
 * for data length.
 * We achieve this by splitting the data into multiple chunks that the DDC can handle along with 
 * a header chunk. Then when the data is requested we can load each chunk and reconstruct the 
 * original data.
 */
namespace DDCUtils64Bit
{
	struct FDDCChunkingHeader
	{
		/** Overall size of the data when reconstructed. */
		int64 TotalSize;
		/** The number of chunks that the data was split into. */
		int32 NumChunks;
	};

	/** The same as calling GetDerivedDataCacheRef().GetSynchronous(...) but with a TArray64 as the output parameter. */
	bool GetSynchronous(const FString& DerivedDataKey, USkeletalMesh* Owner, TArray64<uint8>& OutDerivedData)
	{
		TStringBuilder<512> OwnerPathName;
		Owner->GetPathName(nullptr, OwnerPathName);

		TArray<uint8> Data32Bit;
		if (GetDerivedDataCacheRef().GetSynchronous(*DerivedDataKey, Data32Bit, OwnerPathName))
		{
			// Note that currently the MoveTemp does nothing and the data is being copied although 
			// at some point this might be optimized and the TArray64 will just assume ownership of
			// the TArrays allocation instead.
			OutDerivedData = MoveTemp(Data32Bit);
			return true;
		}
		else
		{
			TStringBuilder<512> HeaderKey;
			HeaderKey << DerivedDataKey << TEXT("Header");

			TArray<uint8> HeaderData;
			HeaderData.Reserve(sizeof(FDDCChunkingHeader));

			// Early out if we cannot find the header or that it is the wrong size (in which case we cannot cast it)
			if (!GetDerivedDataCacheRef().GetSynchronous(HeaderKey.ToString(), HeaderData, Owner->GetPathName()) || HeaderData.Num() != sizeof(FDDCChunkingHeader))
			{
				return false;
			}

			FDDCChunkingHeader* Header = (FDDCChunkingHeader*)HeaderData.GetData();
			OutDerivedData.Reserve(Header->TotalSize);

			for (int32 ChunkIndex = 0; ChunkIndex < Header->NumChunks; ChunkIndex++)
			{
				TStringBuilder<512> ChunkKey;
				ChunkKey << DerivedDataKey << TEXT("Chunk") << ChunkIndex;

				TArray<uint8> ChunkData;
				if (!GetDerivedDataCacheRef().GetSynchronous(ChunkKey.ToString(), ChunkData, OwnerPathName))
				{
					OutDerivedData.Empty(); // Get rid of any partial results we might have
					return false;
				}

				OutDerivedData.Append(ChunkData);
			}

			return true;
		}
	}

	/** The same as calling GetDerivedDataCacheRef().Put(...) but with a TArrayView64 as the input data. */
	void Put(const FString& DerivedDataKey, USkeletalMesh* Owner, TArrayView64<const uint8> DerivedData)
	{
		TStringBuilder<512> OwnerPathName;
		Owner->GetPathName(nullptr, OwnerPathName);

		// We don't use the full 32 bit range as internally the DDC might append info to the end of 
		// the chunk, so we reserve 4kb for this, which is more than enough space to be safe.

		const int64 ChunkSize = MAX_int32 - (4 * 1024);
		if (DerivedData.Num() <= ChunkSize)
		{
			TArrayView<const uint8> DerivedData32Bit(DerivedData.GetData(), (int32)DerivedData.Num());
			GetDerivedDataCacheRef().Put(*DerivedDataKey, DerivedData32Bit, OwnerPathName);
		}
		else
		{
			const int32 NumChunks = (int32)FMath::DivideAndRoundUp(DerivedData.Num(), ChunkSize);

			FDDCChunkingHeader Header{ DerivedData.Num(), NumChunks };

			{
				TStringBuilder<512> HeaderKey;
				HeaderKey << DerivedDataKey << TEXT("Header");

				TArrayView<const uint8> HeaderView((uint8*)&Header, sizeof(FDDCChunkingHeader));

				GetDerivedDataCacheRef().Put(HeaderKey.ToString(), HeaderView, OwnerPathName);
			}

			for (int32 ChunkIndex = 0; ChunkIndex < NumChunks; ++ChunkIndex)
			{
				const int64 ChunkStart = ChunkIndex * ChunkSize;
				const uint64 BytesInChunk = FMath::Min(ChunkSize, DerivedData.Num() - ChunkStart);

				TArrayView<const uint8> ChunkData(DerivedData.GetData() + ChunkStart, (int32)BytesInChunk);

				TStringBuilder<512> ChunkKey;
				ChunkKey << DerivedDataKey << TEXT("Chunk") << ChunkIndex;
				GetDerivedDataCacheRef().Put(ChunkKey.ToString(), ChunkData, OwnerPathName);
			}
		}
	}
} //namespace DDCUtils64Bit

namespace MorphTargetUtils
{
	void ApplyMorphTargetsEditorData(USkeletalMesh* SkeletalMesh, TMap<FName, TArray<FMorphTargetLODModel> >& MorphLODModelsPerTargetName, bool bIsSerializeSaving)
	{
		//Make sure we do not create new morph target during a gc
		FGCScopeGuard GCScopeGuard;

		FSkeletalMeshModel* SkelMeshModel = SkeletalMesh->GetImportedModel();
		check(SkelMeshModel);

		TMap<FName, UMorphTarget*> ExistingMorphTargets;
		ExistingMorphTargets.Reserve(SkeletalMesh->GetMorphTargets().Num());
		for (UMorphTarget* MorphTarget : SkeletalMesh->GetMorphTargets())
		{
			ExistingMorphTargets.Add(MorphTarget->GetFName(), MorphTarget);
		}
		int32 MorphTargetNumber = MorphLODModelsPerTargetName.Num();
		TArray<UMorphTarget*> ToDeleteMorphTargets;
		ToDeleteMorphTargets.Append(SkeletalMesh->GetMorphTargets());
		SkeletalMesh->GetMorphTargets().Empty();
		//Rebuild the MorphTarget object
		for (TPair<FName, TArray<FMorphTargetLODModel>> TargetNameAndMorphLODModels : MorphLODModelsPerTargetName)
		{
			FName MorphTargetName = TargetNameAndMorphLODModels.Key;
			const TArray<FMorphTargetLODModel>& MorphTargetLODModels = TargetNameAndMorphLODModels.Value;
			int32 MorphLODModelNumber = MorphTargetLODModels.Num();

			UMorphTarget* MorphTarget = ExistingMorphTargets.FindRef(MorphTargetName);
			if (!MorphTarget)
			{
				if (!Algo::AnyOf(MorphTargetLODModels, [](const FMorphTargetLODModel& Model) { return Model.Vertices.Num() > 0;}))
				{
					//Skip this empty morphtarget
					continue;
				}
				//When we save the cook result we should never have to build a new morph target
				//When saving cook build, we call GetPlatformSkeletalMeshRenderData in USkeletalMesh::BeginCacheForCookedPlatformData
				//which happen before the serialization of that cook skeletalmesh
				if (!bIsSerializeSaving)
				{
					MorphTarget = NewObject<UMorphTarget>(SkeletalMesh, MorphTargetName);
					check(MorphTarget);
				}
				else
				{
					UE_ASSET_LOG(LogSkeletalMesh, Error, SkeletalMesh, TEXT("Cannot cache a skeletalmesh during a serialize if some morph targets need to be created. The solution is to Pre cache the skeletalmesh before the serialization so no morph target get created."));
					continue;
				}
			}
			else
			{
				ToDeleteMorphTargets.Remove(MorphTarget);
			}
			MorphTarget->EmptyMorphLODModels();
			SkeletalMesh->GetMorphTargets().Add(MorphTarget);
			
			MorphTarget->GetMorphLODModels().AddDefaulted(MorphLODModelNumber);
			for (int32 MorphDataIndex = 0; MorphDataIndex < MorphLODModelNumber; ++MorphDataIndex)
			{
				MorphTarget->GetMorphLODModels()[MorphDataIndex] = MorphTargetLODModels[MorphDataIndex];
			}
		}
		//Rebuild the mapping and rehook the curve data
		SkeletalMesh->InitMorphTargets();

		//Clear any async flags after the morphtargets have been set to the skeletalmesh
		for (UMorphTarget* MorphTarget : SkeletalMesh->GetMorphTargets())
		{
			const EInternalObjectFlags AsyncFlags = EInternalObjectFlags::Async | EInternalObjectFlags::AsyncLoading;
			MorphTarget->ClearInternalFlags(AsyncFlags);
		}

		//Make sure the old unused morph targets are clean up properly
		for (UMorphTarget* ToDeleteMorphTarget : ToDeleteMorphTargets)
		{
			ToDeleteMorphTarget->BaseSkelMesh = nullptr;
			ToDeleteMorphTarget->EmptyMorphLODModels();
			ToDeleteMorphTarget->MarkAsGarbage();
		}
	}
}
//Serialize the LODInfo and append the result to the KeySuffix to build the LODInfo part of the DDC KEY
//Note: this serializer is only used to build the mesh DDC key, no versioning is required
static void SerializeLODInfoForDDC(USkeletalMesh* SkeletalMesh, FString& KeySuffix)
{
	TArray<FSkeletalMeshLODInfo>& LODInfos = SkeletalMesh->GetLODInfoArray();
	const bool bIs16BitfloatBufferSupported = GVertexElementTypeSupport.IsSupported(VET_Half2);
	for (int32 LODIndex = 0; LODIndex < SkeletalMesh->GetLODNum(); ++LODIndex)
	{
		check(LODInfos.IsValidIndex(LODIndex));
		FSkeletalMeshLODInfo& LODInfo = LODInfos[LODIndex];
		bool bValidLODSettings = false;
		if (SkeletalMesh->GetLODSettings() != nullptr)
		{
			const int32 NumSettings = FMath::Min(SkeletalMesh->GetLODSettings()->GetNumberOfSettings(), SkeletalMesh->GetLODNum());
			if (LODIndex < NumSettings)
			{
				bValidLODSettings = true;
			}
		}
		const FSkeletalMeshLODGroupSettings* SkeletalMeshLODGroupSettings = bValidLODSettings ? &SkeletalMesh->GetLODSettings()->GetSettingsForLODLevel(LODIndex) : nullptr;
		LODInfo.BuildGUID = LODInfo.ComputeDeriveDataCacheKey(SkeletalMeshLODGroupSettings);
		KeySuffix += LODInfo.BuildGUID.ToString(EGuidFormats::Digits);
	}
}

// If skeletal mesh derived data needs to be rebuilt (new format, serialization
// differences, etc.) replace the version GUID below with a new one.
// In case of merge conflicts with DDC versions, you MUST generate a new GUID
// and set this new GUID as the version.
#define SKELETALMESH_DERIVEDDATA_VER TEXT("25C49E579B3142DDA2A8C14037267679")

const FString& GetSkeletalMeshDerivedDataVersion()
{
	static FString CachedVersionString = SKELETALMESH_DERIVEDDATA_VER;
	return CachedVersionString;
}

FString BuildSkeletalMeshDerivedDataKey(const ITargetPlatform* TargetPlatform, USkeletalMesh* SkelMesh)
{
	FString KeySuffix(TEXT(""));

	if (SkelMesh->GetUseLegacyMeshDerivedDataKey() )
	{
		//Old asset will have the same LOD settings for bUseFullPrecisionUVs. We can use the LOD 0
		const FSkeletalMeshLODInfo* BaseLODInfo = SkelMesh->GetLODInfo(0);
		bool bUseFullPrecisionUVs = BaseLODInfo ? BaseLODInfo->BuildSettings.bUseFullPrecisionUVs : false;
		KeySuffix += SkelMesh->GetImportedModel()->GetIdString();
		KeySuffix += (bUseFullPrecisionUVs || !GVertexElementTypeSupport.IsSupported(VET_Half2)) ? "1" : "0";
	}
	else
	{
		FString tmpDebugString;
		//Synchronize the user data that are part of the key
		SkelMesh->GetImportedModel()->SyncronizeLODUserSectionsData();
		tmpDebugString = SkelMesh->GetImportedModel()->GetIdString();
		KeySuffix += tmpDebugString;
		tmpDebugString = SkelMesh->GetImportedModel()->GetLODModelIdString();
		KeySuffix += tmpDebugString;
		
		//Add the max gpu bone per section
		const int32 MaxGPUSkinBones = FGPUBaseSkinVertexFactory::GetMaxGPUSkinBones(TargetPlatform);
		KeySuffix += FString::FromInt(MaxGPUSkinBones);

		tmpDebugString = TEXT("");
		SerializeLODInfoForDDC(SkelMesh, tmpDebugString);
		KeySuffix += tmpDebugString;
	}

	KeySuffix += SkelMesh->GetHasVertexColors() ? "1" : "0";
	KeySuffix += SkelMesh->GetVertexColorGuid().ToString(EGuidFormats::Digits);
	
	if (SkelMesh->GetEnableLODStreaming(TargetPlatform))
	{
		const int32 MaxNumStreamedLODs = SkelMesh->GetMaxNumStreamedLODs(TargetPlatform);
		const int32 MaxNumOptionalLODs = SkelMesh->GetMaxNumOptionalLODs(TargetPlatform);
		KeySuffix += *FString::Printf(TEXT("1%08x%08x"), MaxNumStreamedLODs, MaxNumOptionalLODs);
	}
	else
	{
		KeySuffix += TEXT("0zzzzzzzzzzzzzzzz");
	}

	if (TargetPlatform->GetPlatformInfo().PlatformGroupName == TEXT("Desktop")
		&& GStripSkeletalMeshLodsDuringCooking != 0
		&& CVarSkeletalMeshKeepMobileMinLODSettingOnDesktop.GetValueOnAnyThread() != 0)
	{
		KeySuffix += TEXT("_MinMLOD");
	}

	IMeshBuilderModule::GetForPlatform(TargetPlatform).AppendToDDCKey(KeySuffix, true);
	const bool bUnlimitedBoneInfluences = FGPUBaseSkinVertexFactory::GetUnlimitedBoneInfluences();
	KeySuffix += bUnlimitedBoneInfluences ? "1" : "0";

	return FDerivedDataCacheInterface::BuildCacheKey(
		TEXT("SKELETALMESH"),
		*GetSkeletalMeshDerivedDataVersion(),
		*KeySuffix
	);
}

/** This code verify that the data is all in sync index buffer versus sections data. It is active only in debug build*/
void VerifyAllLodSkeletalMeshModelIntegrity(USkeletalMesh* Owner)
{
	if (!Owner || !Owner->GetImportedModel())
	{
		return;
	}

	FSkeletalMeshModel* SkelMeshModel = Owner->GetImportedModel();
	for (int32 LODIndex = 0; LODIndex < SkelMeshModel->LODModels.Num(); LODIndex++)
	{
		FSkeletalMeshLODModel* LODModel = &(SkelMeshModel->LODModels[LODIndex]);
		int32 SectionsVerticeNum = 0;
		int32 SectionsTriangleNum = 0;
		for (const FSkelMeshSection& Section : LODModel->Sections)
		{
			SectionsVerticeNum += Section.GetNumVertices();
			SectionsTriangleNum += Section.NumTriangles;
			int32 LastSectionIndexBuffer = Section.BaseIndex + (Section.NumTriangles * 3);
			if (Section.NumTriangles > 0)
			{
				//Remove 1 if we have at least one triangle
				LastSectionIndexBuffer--;
			}

			if (LODModel->IndexBuffer.IsValidIndex(LastSectionIndexBuffer))
			{
				uint32 FirstSectionIndexBufferValue = LODModel->IndexBuffer[Section.BaseIndex];
				uint32 LastSectionIndexBufferValue = LODModel->IndexBuffer[LastSectionIndexBuffer];
				if (FirstSectionIndexBufferValue < Section.BaseVertexIndex || LastSectionIndexBufferValue >= Section.BaseVertexIndex + Section.GetNumVertices())
				{
					UE_ASSET_LOG(LogSkeletalMesh, Error, Owner, TEXT("The source model is corrupted! Section triangle refer to a vertex not in the section. LOD %d"), LODIndex);
				}
			}
			else
			{
				UE_ASSET_LOG(LogSkeletalMesh, Error, Owner, TEXT("The source model is corrupted! Section index buffer is invalid. LOD %d"), LODIndex);
			}
		}

		if (LODModel->NumVertices != SectionsVerticeNum)
		{
			UE_ASSET_LOG(LogSkeletalMesh, Error, Owner, TEXT("The source model is corrupted! Total sections vertice count is different from source model vertice count. LOD %d"), LODIndex);
		}
		if ((LODModel->IndexBuffer.Num() / 3) != SectionsTriangleNum)
		{
			UE_ASSET_LOG(LogSkeletalMesh, Error, Owner, TEXT("The source model is corrupted! Total sections triangle count is different from source model triangle count (index count divide by 3). LOD %d"), LODIndex);
		}
	}
}

FString FSkeletalMeshRenderData::GetDerivedDataKey(const ITargetPlatform* TargetPlatform, USkeletalMesh* Owner)
{
	return BuildSkeletalMeshDerivedDataKey(TargetPlatform, Owner);
}

void FSkeletalMeshRenderData::Cache(const ITargetPlatform* TargetPlatform, USkeletalMesh* Owner, FSkeletalMeshCompilationContext* ContextPtr)
{
	check(Owner);
	check(ContextPtr);

	check(LODRenderData.Num() == 0); // Should only be called on new, empty RenderData
	check(TargetPlatform);

	auto SerializeLodModelDdcData = [&Owner](FSkeletalMeshLODModel* LODModel, FArchive& Ar)
	{
		//Make sure we add everything FSkeletalMeshLODModel got modified by the skeletalmesh builder
		Ar << LODModel->Sections;
		Ar << LODModel->NumVertices;
		Ar << LODModel->NumTexCoords;
		Ar << LODModel->IndexBuffer;
		Ar << LODModel->ActiveBoneIndices;
		Ar << LODModel->RequiredBones;
		Ar << LODModel->MeshToImportVertexMap;
		Ar << LODModel->MaxImportVertex;
		TArray<uint32>& RawPointIndices = LODModel->GetRawPointIndices();
		Ar << RawPointIndices;
	};

	{
		COOK_STAT(auto Timer = SkeletalMeshCookStats::UsageStats.TimeSyncWork());
		int32 T0 = FPlatformTime::Cycles();

		//When we import a skeletalmesh, in some cases the asset is not yet built, and the usersectiondata and the inline cache are not set
		//until the initial build. This is due to the section count which is establish by the initial build of the import data. The section count
		//is part of the key because users can change section settings(see UserSectionData). So when we do a initial build we do not compute yet
		//the key and force the build code path, the key will be compute after the build and the DDC data will be store with the computed key.
		const bool bAllowDdcFetch = Owner->IsInitialBuildDone();
		if (bAllowDdcFetch)
		{
			DerivedDataKey = BuildSkeletalMeshDerivedDataKey(TargetPlatform, Owner);
		}
		
		//If we have an initial build, the ddc key will be computed only after the build. Some structure are missing until we first build the asset to get the drived data key
		
		TArray64<uint8> DerivedData;
		if(bAllowDdcFetch && DDCUtils64Bit::GetSynchronous(DerivedDataKey, Owner, DerivedData))
		{
			COOK_STAT(Timer.AddHit(DerivedData.Num()));

			FLargeMemoryReader Ar(DerivedData.GetData(), DerivedData.Num(), ELargeMemoryReaderFlags::Persistent);

			//Helper structure to change the morph targets
			TMap<FName, TArray<FMorphTargetLODModel> > MorphLODModelsPerTargetName;

			//With skeletal mesh build refactor we serialize the LODModel data into the DDC
			//We need to store those so we do not have to rerun the reduction to make them up to date
			//with the serialize renderdata. This allow to use DDC when changing the reduction settings.
			//The old workflow has to reduce the LODModel before getting the render data DDC.
			if (!Owner->GetUseLegacyMeshDerivedDataKey())
			{
				FSkeletalMeshModel* SkelMeshModel = Owner->GetImportedModel();
				check(SkelMeshModel);

				//Get the morph target data, we put it in the compilation context to apply them in the game thread before the InitResources
				{
					int32 MorphTargetNumber = 0;
					Ar << MorphTargetNumber;
					MorphLODModelsPerTargetName.Reserve(MorphTargetNumber);
					//We cannot serialize directly the UMorphTarget with a FMemoryArchive. This is not supported.
					for (int32 MorphTargetIndex = 0; MorphTargetIndex < MorphTargetNumber; ++MorphTargetIndex)
					{
						FName MorphTargetName = NAME_None;
						Ar << MorphTargetName;
						TArray<FMorphTargetLODModel>& MorphTargetLODModels = MorphLODModelsPerTargetName.FindOrAdd(MorphTargetName);
						int32 MorphLODModelNumber = 0;
						Ar << MorphLODModelNumber;
						MorphTargetLODModels.Empty(MorphLODModelNumber);
						MorphTargetLODModels.AddDefaulted(MorphLODModelNumber);
						for (int32 MorphDataIndex = 0; MorphDataIndex < MorphLODModelNumber; ++MorphDataIndex)
						{
							Ar << MorphTargetLODModels[MorphDataIndex];
						}
					}
				}

				//Serialize the LODModel sections since they are dependent on the reduction
				for (int32 LODIndex = 0; LODIndex < SkelMeshModel->LODModels.Num(); LODIndex++)
				{
					FSkeletalMeshLODModel* LODModel = &(SkelMeshModel->LODModels[LODIndex]);
					SerializeLodModelDdcData(LODModel, Ar);
					LODModel->SyncronizeUserSectionsDataArray();
				}
			}

			Serialize(Ar, Owner);
			for (int32 LODIndex = 0; LODIndex < LODRenderData.Num(); ++LODIndex)
			{
				FSkeletalMeshLODRenderData& LODData = LODRenderData[LODIndex];
				if (LODData.bStreamedDataInlined)
				{
					break;
				}
				constexpr uint8 DummyStripFlags = 0;
				const bool bForceKeepCPUResources = FSkeletalMeshLODRenderData::ShouldForceKeepCPUResources();
				const bool bNeedsCPUAccess = FSkeletalMeshLODRenderData::ShouldKeepCPUResources(Owner, LODIndex, bForceKeepCPUResources);
				LODData.SerializeStreamedData(Ar, Owner, LODIndex, DummyStripFlags, bNeedsCPUAccess, bForceKeepCPUResources);
			}

			//Apply the morphtargets change if any
			if (MorphLODModelsPerTargetName.Num() > 0)
			{
				MorphTargetUtils::ApplyMorphTargetsEditorData(Owner, MorphLODModelsPerTargetName, ContextPtr->bIsSerializeSaving);
			}

			int32 T1 = FPlatformTime::Cycles();
			UE_LOG(LogSkeletalMesh, Verbose, TEXT("Skeletal Mesh found in DDC [%fms] %s"), FPlatformTime::ToMilliseconds(T1 - T0), *Owner->GetPathName());
		}
		else
		{
			UE_LOG(LogSkeletalMesh, Log, TEXT("Building Skeletal Mesh %s..."),*Owner->GetName());

			// Allocate empty entries for each LOD level in source mesh
			FSkeletalMeshModel* SkelMeshModel = Owner->GetImportedModel();
			check(SkelMeshModel);

			uint32 VertexBufferBuildFlags = Owner->GetVertexBufferFlags();

			for (int32 LODIndex = 0; LODIndex < SkelMeshModel->LODModels.Num(); LODIndex++)
			{
				FSkeletalMeshLODModel* LODModel = &(SkelMeshModel->LODModels[LODIndex]);
				FSkeletalMeshLODInfo* LODInfo = Owner->GetLODInfo(LODIndex);
				check(LODInfo);
				//We want to avoid building a LOD if the LOD was generated from a previous LODIndex.
				const bool bIsGeneratedLodNotInline = (LODInfo->bHasBeenSimplified && Owner->IsReductionActive(LODIndex) && Owner->GetReductionSettings(LODIndex).BaseLOD < LODIndex);
				
				//Make sure the LOD have all the data needed to be build
				const bool bRawDataEmpty = Owner->IsLODImportedDataEmpty(LODIndex);
				const bool bRawBuildDataAvailable = Owner->IsLODImportedDataBuildAvailable(LODIndex);
				
				//Build the source model before the render data, if we are a purely generated LOD we do not need to be build
				IMeshBuilderModule& MeshBuilderModule = IMeshBuilderModule::GetForPlatform(TargetPlatform);
				if (!bIsGeneratedLodNotInline && !bRawDataEmpty && bRawBuildDataAvailable)
				{
					LODInfo->bHasBeenSimplified = false;
					const bool bRegenDepLODs = true;
					FSkeletalMeshBuildParameters BuildParameters(Owner, TargetPlatform, LODIndex, bRegenDepLODs);
					MeshBuilderModule.BuildSkeletalMesh(BuildParameters);
					LODModel = &(SkelMeshModel->LODModels[LODIndex]);
				}
				else
				{
					//We need to synchronize when we are generated mesh or if we have load an old asset that was not re-imported
					LODModel->SyncronizeUserSectionsDataArray();
				}

				FSkeletalMeshLODRenderData* LODData = new FSkeletalMeshLODRenderData();
				LODRenderData.Add(LODData);
				
				//Get the UVs and tangents precision build settings flag specific for this LOD index
				{
					bool bUseFullPrecisionUVs = LODInfo->BuildSettings.bUseFullPrecisionUVs;
					bool bUseHighPrecisionTangentBasis = LODInfo->BuildSettings.bUseHighPrecisionTangentBasis;
					bool bUseBackwardsCompatibleF16TruncUVs = LODInfo->BuildSettings.bUseBackwardsCompatibleF16TruncUVs;
					if (bUseFullPrecisionUVs || !GVertexElementTypeSupport.IsSupported(VET_Half2))
					{
						VertexBufferBuildFlags |= ESkeletalMeshVertexFlags::UseFullPrecisionUVs;
					}
					if (bUseHighPrecisionTangentBasis)
					{
						VertexBufferBuildFlags |= ESkeletalMeshVertexFlags::UseHighPrecisionTangentBasis;
					}
					if (bUseBackwardsCompatibleF16TruncUVs)
					{
						VertexBufferBuildFlags |= ESkeletalMeshVertexFlags::UseBackwardsCompatibleF16TruncUVs;
					}
				}
				LODData->BuildFromLODModel(LODModel, VertexBufferBuildFlags);
			}

			FLargeMemoryWriter Ar(0, /*bIsPersistent=*/ true);

			//If we load an old asset we want to be sure the serialize ddc will be the same has before the skeletalmesh build refactor
			//So we do not serialize the LODModel sections.
			if (!Owner->GetUseLegacyMeshDerivedDataKey())
			{
				int32 MorphTargetNumber = Owner->GetMorphTargets().Num();
				Ar << MorphTargetNumber;
				for (int32 MorphTargetIndex = 0; MorphTargetIndex < MorphTargetNumber; ++MorphTargetIndex)
				{
					FName MorphTargetName = Owner->GetMorphTargets()[MorphTargetIndex]->GetFName();
					Ar << MorphTargetName;
					Owner->GetMorphTargets()[MorphTargetIndex]->SerializeMemoryArchive(Ar);
				}
				//No need to serialize the morph target mapping since we will rebuild the mapping when loading a ddc

				//Serialize the LODModel sections since they are dependent on the reduction
				for (int32 LODIndex = 0; LODIndex < SkelMeshModel->LODModels.Num(); LODIndex++)
				{
					FSkeletalMeshLODModel* LODModel = &(SkelMeshModel->LODModels[LODIndex]);
					SerializeLodModelDdcData(LODModel, Ar);
				}
			}

			IMeshBuilderModule& MeshBuilderModule = IMeshBuilderModule::GetForPlatform(TargetPlatform);
			MeshBuilderModule.PostBuildSkeletalMesh(this, Owner);

			//Serialize the render data
			Serialize(Ar, Owner);
			for (int32 LODIndex = 0; LODIndex < LODRenderData.Num(); ++LODIndex)
			{
				FSkeletalMeshLODRenderData& LODData = LODRenderData[LODIndex];
				if (LODData.bStreamedDataInlined)
				{
					break;
				}
				const uint8 LODStripFlags = FSkeletalMeshLODRenderData::GenerateClassStripFlags(Ar, Owner, LODIndex);
				const bool bForceKeepCPUResources = FSkeletalMeshLODRenderData::ShouldForceKeepCPUResources();
				const bool bNeedsCPUAccess = FSkeletalMeshLODRenderData::ShouldKeepCPUResources(Owner, LODIndex, bForceKeepCPUResources);
				LODData.SerializeStreamedData(Ar, Owner, LODIndex, LODStripFlags, bNeedsCPUAccess, bForceKeepCPUResources);
			}

			//Recompute the derived data key in case there was some data correction during the build process, this make sure the DDC key is always representing the correct build result.
			//There should never be correction of the data during the build, the data has to be corrected in the post load before calling this function.
			FString BuiltDerivedDataKey = BuildSkeletalMeshDerivedDataKey(TargetPlatform, Owner);
			//Only compare keys if the ddc fetch was allowed
			if (bAllowDdcFetch)
			{
				if (BuiltDerivedDataKey != DerivedDataKey)
				{
					//If we are in this case we should resave the asset so the source data will be the same and we can use this DDC. Reduction can change the number of sections and the user section data is in the DDC key.
					//So if we change the reduction algorithm, its possible we fall in this situation.
					//We save the real data key which force the asset to always rebuild when the editor is loading it until the user save it
					UE_LOG(LogSkeletalMesh, Log, TEXT("Skeletal mesh [%s]: The derived data key is different after the build. Save the asset to avoid rebuilding it everytime the editor load it."), *Owner->GetPathName());
				}
			}
			else
			{
				//After the initial build we set the key to the built one
				DerivedDataKey = BuiltDerivedDataKey;
			}

			//Store the data using the built key to avoid DDC corruption
			TArrayView64<const uint8> ArView(Ar.GetData(), Ar.TotalSize());
			DDCUtils64Bit::Put(BuiltDerivedDataKey, Owner, ArView);

			int32 T1 = FPlatformTime::Cycles();
			UE_LOG(LogSkeletalMesh, Log, TEXT("Built Skeletal Mesh [%.2fs] %s"), FPlatformTime::ToMilliseconds(T1 - T0) / 1000.0f, *Owner->GetPathName());
			COOK_STAT(Timer.AddMiss(DerivedData.Num()));
		}
	}
	VerifyAllLodSkeletalMeshModelIntegrity(Owner);
}

void FSkeletalMeshRenderData::SyncUVChannelData(const TArray<FSkeletalMaterial>& ObjectData)
{
	TUniquePtr< TArray<FMeshUVChannelInfo> > UpdateData = MakeUnique< TArray<FMeshUVChannelInfo> >();
	UpdateData->Empty(ObjectData.Num());

	for (const FSkeletalMaterial& SkeletalMaterial : ObjectData)
	{
		UpdateData->Add(SkeletalMaterial.UVChannelData);
	}

	// SyncUVChannelData can be called from any thread during async skeletal mesh compilation. 
	// There is currently multiple race conditions in ENQUEUE_RENDER_COMMAND making it unsafe to be called from
	// any other thread than rendering or game because of the render thread suspension mecanism.
	// We sidestep the issue here by avoiding a call to ENQUEUE_RENDER_COMMAND if the resource has not been initialized and is still unknown
	// to the render thread.
	if (bInitialized)
	{
		ENQUEUE_RENDER_COMMAND(SyncUVChannelData)([this, UpdateData = MoveTemp(UpdateData)](FRHICommandListImmediate& RHICmdList)
		{
			FMemory::Memswap(&UVChannelDataPerMaterial, UpdateData.Get(), sizeof(UVChannelDataPerMaterial));
		});
	}
	else
	{
		FMemory::Memswap(&UVChannelDataPerMaterial, UpdateData.Get(), sizeof(UVChannelDataPerMaterial));
	}
}

#endif // WITH_EDITOR

FSkeletalMeshRenderData::FSkeletalMeshRenderData()
	: bReadyForStreaming(false)
	, NumInlinedLODs(0)
	, NumNonOptionalLODs(0)
	, CurrentFirstLODIdx(0)
	, PendingFirstLODIdx(0)
	, LODBiasModifier(0)
	, bSupportRayTracing(true)
	, bInitialized(false)
{}

FSkeletalMeshRenderData::~FSkeletalMeshRenderData()
{
	FSkeletalMeshLODRenderData** LODRenderDataArray = LODRenderData.GetData();
	for (int32 LODIndex = 0; LODIndex < LODRenderData.Num(); ++LODIndex)
	{
		LODRenderDataArray[LODIndex]->Release();
		// Prevent the array from calling the destructor to handle correctly the refcount.
		// For compatibility reason, LODRenderDataArray is using ptr directly instead of TRefCountPtr.
		LODRenderDataArray[LODIndex] = nullptr;
	}
	LODRenderData.Empty();
}

int32 FSkeletalMeshRenderData::GetNumNonStreamingLODs() const
{
	int LODCount = 0;
	for (int32 Idx = LODRenderData.Num() - 1; Idx >= 0; --Idx)
	{
		if (LODRenderData[Idx].bStreamedDataInlined)
		{
			++LODCount;
		}
		else
		{
			break;
		}
	}

	if (LODCount == 0 && LODRenderData.Num())
	{
		return 1;
	}
	else
	{
		return LODCount;
	}
}

int32 FSkeletalMeshRenderData::GetNumNonOptionalLODs() const
{
	int LODCount = 0;
	for (int32 Idx = LODRenderData.Num() - 1; Idx >= 0; --Idx)
	{
		// Make sure GetNumNonOptionalLODs() is bigger than GetNumNonStreamingLODs().
		if (LODRenderData[Idx].bStreamedDataInlined || !LODRenderData[Idx].bIsLODOptional)
		{
			++LODCount;
		}
		else
		{
			break;
		}
	}

	if (LODCount == 0 && LODRenderData.Num())
	{
		return 1;
	}
	else
	{
		return LODCount;
	}
}

void FSkeletalMeshRenderData::Serialize(FArchive& Ar, USkeletalMesh* Owner)
{
	DECLARE_SCOPE_CYCLE_COUNTER(TEXT("FSkeletalMeshRenderData::Serialize"), STAT_SkeletalMeshRenderData_Serialize, STATGROUP_LoadTime);

#if PLATFORM_DESKTOP

	if (Ar.IsFilterEditorOnly())
	{
		int32 MinMobileLODIdx = 0;
		bool bShouldSerialize = CVarSkeletalMeshKeepMobileMinLODSettingOnDesktop.GetValueOnAnyThread() != 0;
#if WITH_EDITOR
		if (Ar.IsSaving())
		{
			if (Ar.CookingTarget()->GetPlatformInfo().PlatformGroupName == TEXT("Desktop")
				&& GStripSkeletalMeshLodsDuringCooking != 0
				&& CVarSkeletalMeshKeepMobileMinLODSettingOnDesktop.GetValueOnAnyThread() != 0)
			{
				MinMobileLODIdx = Owner->GetMinLod().GetValueForPlatform(TEXT("Mobile")) - Owner->GetMinLod().GetValueForPlatform(TEXT("Desktop"));
				MinMobileLODIdx = FMath::Clamp(MinMobileLODIdx, 0, 255); // Will be cast to uint8 when applying LOD bias. Also, make sure it's not < 0,
																		 // which can happen if the desktop min LOD is higher than the mobile setting
			}
			else
			{
				bShouldSerialize = false;
			}
		}
#endif

		if (bShouldSerialize)
		{
			Ar << MinMobileLODIdx;

			if (Ar.IsLoading() && GMaxRHIFeatureLevel == ERHIFeatureLevel::ES3_1)
			{
				LODBiasModifier = MinMobileLODIdx;
			}
		}
	}
#endif // PLATFORM_DESKTOP

	LODRenderData.Serialize(Ar, Owner);

#if WITH_EDITOR
	if (Ar.IsSaving())
	{
		NumInlinedLODs = GetNumNonStreamingLODs();
		NumNonOptionalLODs = GetNumNonOptionalLODs();
	}
#endif
	Ar << NumInlinedLODs << NumNonOptionalLODs;
#if WITH_EDITOR
	//Recompute on load because previously we were storing NumOptionalLODs, which is less convenient because it includes first LODs (and can be stripped by MinMip).
	if (Ar.IsLoading())
	{
		NumInlinedLODs = GetNumNonStreamingLODs();
		NumNonOptionalLODs = GetNumNonOptionalLODs();
	}
#endif
	
	CurrentFirstLODIdx = LODRenderData.Num() - NumInlinedLODs;
	PendingFirstLODIdx = CurrentFirstLODIdx;

PRAGMA_DISABLE_DEPRECATION_WARNINGS
	bSupportRayTracing = Owner->bSupportRayTracing;
PRAGMA_ENABLE_DEPRECATION_WARNINGS
}

void FSkeletalMeshRenderData::InitResources(bool bNeedsVertexColors, TArray<UMorphTarget*>& InMorphTargets, USkeletalMesh* Owner)
{
	if (!bInitialized)
	{
		// initialize resources for each lod
		for (int32 LODIndex = 0; LODIndex < LODRenderData.Num(); LODIndex++)
		{
			FSkeletalMeshLODRenderData& RenderData = LODRenderData[LODIndex];

			if(RenderData.GetNumVertices() > 0)
			{
				RenderData.InitResources(bNeedsVertexColors, LODIndex, InMorphTargets, Owner);
			}
		}

		ENQUEUE_RENDER_COMMAND(CmdSetSkeletalMeshReadyForStreaming)(
			[this, Owner](FRHICommandListImmediate&)
		{
			bReadyForStreaming = true;
		});

		bInitialized = true;
	}
}

void FSkeletalMeshRenderData::ReleaseResources()
{
	if (bInitialized)
	{
		// release resources for each lod
		for (int32 LODIndex = 0; LODIndex < LODRenderData.Num(); LODIndex++)
		{
			LODRenderData[LODIndex].ReleaseResources();
		}
		bInitialized = false;
	}
}

uint32 FSkeletalMeshRenderData::GetNumBoneInfluences(int32 MinLODIndex) const
{
	uint32 NumBoneInfluences = 0;
	for (int32 LODIndex = MinLODIndex; LODIndex < LODRenderData.Num(); ++LODIndex)
	{
		const FSkeletalMeshLODRenderData& Data = LODRenderData[LODIndex];
		NumBoneInfluences = FMath::Max(NumBoneInfluences, Data.GetVertexBufferMaxBoneInfluences());
	}

	return NumBoneInfluences;
}

uint32 FSkeletalMeshRenderData::GetNumBoneInfluences() const
{
	return GetNumBoneInfluences(0);
}

bool FSkeletalMeshRenderData::RequiresCPUSkinning(ERHIFeatureLevel::Type FeatureLevel, int32 MinLODIndex) const
{
	const int32 MaxGPUSkinBones = FGPUBaseSkinVertexFactory::GetMaxGPUSkinBones();
	const int32 MaxBonesPerChunk = GetMaxBonesPerSection(MinLODIndex);
	// Do CPU skinning if we need too many bones per chunk
	return (MaxBonesPerChunk > MaxGPUSkinBones);
}

bool FSkeletalMeshRenderData::RequiresCPUSkinning(ERHIFeatureLevel::Type FeatureLevel) const
{
	return RequiresCPUSkinning(FeatureLevel, 0);
}

void FSkeletalMeshRenderData::GetResourceSizeEx(FResourceSizeEx& CumulativeResourceSize)
{
	for (int32 LODIndex = 0; LODIndex < LODRenderData.Num(); ++LODIndex)
	{
		const FSkeletalMeshLODRenderData& RenderData = LODRenderData[LODIndex];
		RenderData.GetResourceSizeEx(CumulativeResourceSize);
	}
}

SIZE_T FSkeletalMeshRenderData::GetCPUAccessMemoryOverhead() const
{
	SIZE_T Result = 0;
	for (int32 LODIndex = 0; LODIndex < LODRenderData.Num(); ++LODIndex)
	{
		const FSkeletalMeshLODRenderData& RenderData = LODRenderData[LODIndex];
		Result += RenderData.GetCPUAccessMemoryOverhead();
	}
	return Result;
}

int32 FSkeletalMeshRenderData::GetMaxBonesPerSection(int32 MinLODIdx) const
{
	int32 MaxBonesPerSection = 0;
	for (int32 LODIndex = MinLODIdx; LODIndex < LODRenderData.Num(); ++LODIndex)
	{
		const FSkeletalMeshLODRenderData& RenderData = LODRenderData[LODIndex];
		for (int32 SectionIndex = 0; SectionIndex < RenderData.RenderSections.Num(); ++SectionIndex)
		{
			MaxBonesPerSection = FMath::Max<int32>(MaxBonesPerSection, RenderData.RenderSections[SectionIndex].BoneMap.Num());
		}
	}
	return MaxBonesPerSection;
}

int32 FSkeletalMeshRenderData::GetMaxBonesPerSection() const
{
	return GetMaxBonesPerSection(0);
}

int32 FSkeletalMeshRenderData::GetFirstValidLODIdx(int32 MinIdx) const
{
	const int32 LODCount = LODRenderData.Num();
	if (LODCount == 0)
	{
		return INDEX_NONE;
	}

	int32 LODIndex = FMath::Clamp<int32>(MinIdx, 0, LODCount - 1);
	while (LODIndex < LODCount && !LODRenderData[LODIndex].GetNumVertices())
	{
		++LODIndex;
	}
	return (LODIndex < LODCount) ? LODIndex : INDEX_NONE;
}
