// Copyright Epic Games, Inc. All Rights Reserved.

#include "WorldPartition/HLOD/HLODSubsystem.h"
#include "WorldPartition/HLOD/HLODActor.h"
#include "WorldPartition/HLOD/HLODActorDesc.h"
#include "WorldPartition/HLOD/HLODLayer.h"
#include "WorldPartition/WorldPartition.h"
#include "WorldPartition/WorldPartitionSubsystem.h"
#include "WorldPartition/WorldPartitionRuntimeCell.h"
#include "WorldPartition/WorldPartitionRuntimeHash.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectGlobals.h"
#include "Engine/Engine.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineModule.h"
#include "EngineUtils.h"
#include "Components/StaticMeshComponent.h"

#define LOCTEXT_NAMESPACE "HLODSubsystem"

DEFINE_LOG_CATEGORY_STATIC(LogHLODSubsystem, Log, All);

static TAutoConsoleVariable<int32> CVarHLODWarmupNumFrames(
	TEXT("wp.Runtime.HLOD.WarmupNumFrames"),
	5,
	TEXT("Delay unloading of a cell for this amount of frames to ensure HLOD assets are ready to be shown at the proper resolution. Set to 0 to force disable warmup."),
	ECVF_Default
);

static TAutoConsoleVariable<int32> CVarHLODWarmupEnabled(
	TEXT("wp.Runtime.HLOD.WarmupEnabled"),
	1,
	TEXT("Enable HLOD assets warmup. Will delay unloading of cells & transition to HLODs for wp.Runtime.HLOD.WarmupNumFrames frames."),
	ECVF_Default
);

static TAutoConsoleVariable<int32> CVarHLODWarmupDebugDraw(
	TEXT("wp.Runtime.HLOD.WarmupDebugDraw"),
	0,
	TEXT("Draw debug display for the warmup requests"),
	ECVF_Default
);

static TAutoConsoleVariable<float> CVarHLODWarmupVTScaleFactor(
	TEXT("wp.Runtime.HLOD.WarmupVTScaleFactor"),
	2.0f,
	TEXT("Scale the VT size we ask to prefetch by this factor."),
	ECVF_Default
);

static TAutoConsoleVariable<int32> CVarHLODWarmupVTSizeClamp(
	TEXT("wp.Runtime.HLOD.WarmupVTSizeClamp"),
	2048,
	TEXT("Clamp VT warmup requests for safety."),
	ECVF_Default
);

namespace FHLODSubsystem
{
	// @todo_ow: remove cell prefix to avoid this mapping
	FName GetHLODCellName(UWorld* InWorld, const TSet<FString>& InGridNames, AWorldPartitionHLOD* InWorldPartitionHLOD)
	{
		FString CellName = InWorldPartitionHLOD->GetSourceCellName().ToString();
		for (const FString& GridName : InGridNames)
		{
			if (int32 Index = CellName.Find(GridName); Index != INDEX_NONE)
			{
				return *FString::Format(TEXT("{0}_{1}"), { *InWorld->GetName(), *CellName.Mid(Index) });
			}
		}

		return *CellName;
	}
};

UHLODSubsystem::UHLODSubsystem()
	: UWorldSubsystem()
{
}

UHLODSubsystem::~UHLODSubsystem()
{
}

bool UHLODSubsystem::WorldPartitionHLODEnabled = true;

FAutoConsoleCommand UHLODSubsystem::EnableHLODCommand(
	TEXT("wp.Runtime.HLOD"),
	TEXT("Turn on/off loading & rendering of world partition HLODs."),
	FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
	{
		UHLODSubsystem::WorldPartitionHLODEnabled = (Args.Num() != 1) || (Args[0] != TEXT("0"));
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World && World->IsGameWorld())
			{
				UHLODSubsystem* HLODSubSystem = World->GetSubsystem<UHLODSubsystem>();
				for (const auto& CellHLODMapping : HLODSubSystem->CellsData)
				{
					const FCellData& CellData = CellHLODMapping.Value;
					bool bIsHLODVisible = UHLODSubsystem::WorldPartitionHLODEnabled && !CellData.bIsCellVisible;
					for (AWorldPartitionHLOD* HLODActor : CellData.LoadedHLODs)
					{
						HLODActor->SetVisibility(bIsHLODVisible);
					}
				}
			}
		}
	})
);

bool UHLODSubsystem::IsHLODEnabled()
{
	return UHLODSubsystem::WorldPartitionHLODEnabled;
}

bool UHLODSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}

	if (UWorld* WorldOuter = Cast<UWorld>(Outer))
	{
		return WorldOuter->IsPartitionedWorld();
	}
	return false;
}

void UHLODSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	// Ensure the WorldPartitionSubsystem gets created before the HLODSubsystem
	Collection.InitializeDependency<UWorldPartitionSubsystem>();

	Super::Initialize(Collection);

	UWorld* World = GetWorld();

	if (World->IsGameWorld())
	{
		UWorldPartition* WorldPartition = GetWorld()->GetWorldPartition();
		check(WorldPartition);

		WorldPartition->OnWorldPartitionInitialized.AddUObject(this, &UHLODSubsystem::OnWorldPartitionInitialized);
		WorldPartition->OnWorldPartitionUninitialized.AddUObject(this, &UHLODSubsystem::OnWorldPartitionUninitialized);

		SceneViewExtension = FSceneViewExtensions::NewExtension<FHLODResourcesResidencySceneViewExtension>(World);
	}
}

void UHLODSubsystem::OnWorldPartitionInitialized(UWorldPartition* InWorldPartition)
{
	check(InWorldPartition == GetWorld()->GetWorldPartition());
	check(CellsData.IsEmpty());

	TSet<const UWorldPartitionRuntimeCell*> StreamingCells;
	InWorldPartition->RuntimeHash->GetAllStreamingCells(StreamingCells, /*bAllDataLayers*/ true);

	// Build cell to HLOD mapping
	for (const UWorldPartitionRuntimeCell* Cell : StreamingCells)
	{
		GridNames.Add(Cell->GetGridName().ToString());
		CellsData.Emplace(Cell->GetFName());
	}
}

void UHLODSubsystem::OnWorldPartitionUninitialized(UWorldPartition* InWorldPartition)
{
	check(InWorldPartition == GetWorld()->GetWorldPartition());
	CellsData.Reset();
}

void UHLODSubsystem::RegisterHLODActor(AWorldPartitionHLOD* InWorldPartitionHLOD)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UHLODSubsystem::RegisterHLODActor);

	FName CellName = FHLODSubsystem::GetHLODCellName(GetWorld(), GridNames, InWorldPartitionHLOD);
	FCellData* CellData = CellsData.Find(CellName);

#if WITH_EDITOR
	UE_LOG(LogHLODSubsystem, Verbose, TEXT("Registering HLOD %s (%s) for cell %s"), *InWorldPartitionHLOD->GetActorLabel(), *InWorldPartitionHLOD->GetActorGuid().ToString(), *CellName.ToString());
#endif

	if (CellData)
	{
		CellData->LoadedHLODs.Add(InWorldPartitionHLOD);
		InWorldPartitionHLOD->SetVisibility(UHLODSubsystem::WorldPartitionHLODEnabled && !CellData->bIsCellVisible);
	}
	else
	{
		UE_LOG(LogHLODSubsystem, Verbose, TEXT("Found HLOD referencing nonexistent cell '%s'"), *CellName.ToString());
		InWorldPartitionHLOD->SetVisibility(false);
	}
}

void UHLODSubsystem::UnregisterHLODActor(AWorldPartitionHLOD* InWorldPartitionHLOD)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UHLODSubsystem::UnregisterHLODActor);

	FName CellName = FHLODSubsystem::GetHLODCellName(GetWorld(), GridNames, InWorldPartitionHLOD);
	FCellData* CellData = CellsData.Find(CellName);

#if WITH_EDITOR
	UE_LOG(LogHLODSubsystem, Verbose, TEXT("Unregistering HLOD %s (%s) for cell %s"), *InWorldPartitionHLOD->GetActorLabel(), *InWorldPartitionHLOD->GetActorGuid().ToString(), *CellName.ToString());
#endif

	if (CellData)
	{
		int32 NumRemoved = CellData->LoadedHLODs.Remove(InWorldPartitionHLOD);
		check(NumRemoved == 1);
	}
}

void UHLODSubsystem::OnCellShown(const UWorldPartitionRuntimeCell* InCell)
{
	FCellData& CellData = CellsData.FindChecked(InCell->GetFName());
	CellData.bIsCellVisible = true;

#if WITH_EDITOR
	UE_LOG(LogHLODSubsystem, Verbose, TEXT("Cell shown - %s - hiding %d HLOD actors"), *InCell->GetName(), CellData.LoadedHLODs.Num());
#endif

	for (AWorldPartitionHLOD* HLODActor : CellData.LoadedHLODs)
	{
#if WITH_EDITOR
		UE_LOG(LogHLODSubsystem, Verbose, TEXT("\t\t%s - %s"), *HLODActor->GetActorLabel(), *HLODActor->GetActorGuid().ToString());
#endif
		HLODActor->SetVisibility(false);
	}
}

void UHLODSubsystem::OnCellHidden(const UWorldPartitionRuntimeCell* InCell)
{
	FCellData& CellData = CellsData.FindChecked(InCell->GetFName());
	CellData.bIsCellVisible = false;

#if WITH_EDITOR
	UE_LOG(LogHLODSubsystem, Verbose, TEXT("Cell hidden - %s - showing %d HLOD actors"), *InCell->GetName(), CellData.LoadedHLODs.Num());
#endif

	for (AWorldPartitionHLOD* HLODActor : CellData.LoadedHLODs)
	{
#if WITH_EDITOR
		UE_LOG(LogHLODSubsystem, Verbose, TEXT("\t\t%s - %s"), *HLODActor->GetActorLabel(), *HLODActor->GetActorGuid().ToString());
#endif
		HLODActor->SetVisibility(UHLODSubsystem::WorldPartitionHLODEnabled);
	}
}

static void PrepareVTRequests(TMap<UMaterialInterface*, float>& InOutVTRequests, UStaticMeshComponent* InStaticMeshComponent, float InPixelSize)
{
	float PixelSize = InPixelSize;

	// Assume the texture is wrapped around the object, so the screen size is actually less than the resolution we require.
	PixelSize *= CVarHLODWarmupVTScaleFactor.GetValueOnAnyThread();

	// Clamp for safety
	PixelSize = FMath::Min(PixelSize, CVarHLODWarmupVTSizeClamp.GetValueOnAnyThread());

	for (UMaterialInterface* MaterialInterface : InStaticMeshComponent->GetMaterials())
	{
		// We have a VT we'd like to prefetch, add or update a request in our request map.
		// If the texture was already requested by another component, fetch the highest required resolution only.
		float& CurrentMaxPixel = InOutVTRequests.FindOrAdd(MaterialInterface);
		CurrentMaxPixel = FMath::Max(CurrentMaxPixel, PixelSize);
	}
}

static void PrepareNaniteRequests(TSet<Nanite::FResources*>& InOutNaniteRequests, UStaticMeshComponent* InStaticMeshComponent)
{
	UStaticMesh* StaticMesh = InStaticMeshComponent->GetStaticMesh();
	if (StaticMesh && StaticMesh->HasValidNaniteData())
	{
		// UE_LOG(LogHLODSubsystem, Warning, TEXT("NanitePrefetch: %s, %d pages"), *StaticMesh->GetFullName(), StaticMesh->GetRenderData()->NaniteResources.PageStreamingStates.Num());
		InOutNaniteRequests.Add(&StaticMesh->GetRenderData()->NaniteResources);
	}
}

static float EstimateScreenSize(UStaticMeshComponent* InStaticMeshComponent, const FSceneViewFamily& InViewFamily)
{
	float MaxPixels = 0;

	// Estimate the highest screen pixel size of this component in the provided views
	for (const FSceneView* View : InViewFamily.Views)
	{
		// Make sure the HLOD actor we're about to show is actually in the frustum
		if (View->ViewFrustum.IntersectSphere(InStaticMeshComponent->Bounds.Origin, InStaticMeshComponent->Bounds.SphereRadius))
		{
			float ScreenDiameter = ComputeBoundsScreenSize(InStaticMeshComponent->Bounds.Origin, InStaticMeshComponent->Bounds.SphereRadius, *View);
			float PixelSize = ScreenDiameter * View->ViewMatrices.GetScreenScale() * 2.0f;

			MaxPixels = FMath::Max(MaxPixels, PixelSize);
		}
	}

	return MaxPixels;
}

void UHLODSubsystem::MakeRenderResourcesResident(const FCellData& CellData, const FSceneViewFamily& InViewFamily)
{
	TRACE_CPUPROFILER_EVENT_SCOPE(UHLODSubsystem::MakeRenderResourcesResident)

	TMap<UMaterialInterface*, float> VTRequests;
	TSet<Nanite::FResources*> NaniteRequests;

	// For each HLOD actor representing this cell
	for(AWorldPartitionHLOD* HLODActor : CellData.LoadedHLODs)
	{
		// Skip HLOD actors that doesn't require warmup.
		// For example, instanced HLODs, as they reuse the same meshes/textures as their source actors.
		// These resources should already be resident & at the proper resolution.
		if (!HLODActor->DoesRequireWarmup())
		{
			continue;
		}

		HLODActor->ForEachComponent<UStaticMeshComponent>(false, [&](UStaticMeshComponent* SMC)
		{
			float PixelSize = EstimateScreenSize(SMC, InViewFamily);

			if (PixelSize > 0)
			{
				PrepareVTRequests(VTRequests, SMC, PixelSize);

				// Only issue Nanite requests on the first warmup frame
				if (CellData.WarmupStartFrame == InViewFamily.FrameNumber)
				{
					PrepareNaniteRequests(NaniteRequests, SMC);
				}

#if ENABLE_DRAW_DEBUG
				if (CVarHLODWarmupDebugDraw.GetValueOnAnyThread())
				{
					const FBox& Box = SMC->CalcLocalBounds().GetBox();
					DrawDebugBox(HLODActor->GetWorld(), Box.GetCenter(), Box.GetExtent(), FColor::Yellow, /*bPersistentLine*/ false, /*Lifetime*/ 1.0f);
				}
#endif
			}
		});
	}

	if (!VTRequests.IsEmpty() || !NaniteRequests.IsEmpty())
	{
		ENQUEUE_RENDER_COMMAND(MakeHLODRenderResourcesResident)(
			[VTRequests = MoveTemp(VTRequests), NaniteRequests = MoveTemp(NaniteRequests), FeatureLevel = InViewFamily.GetFeatureLevel()](FRHICommandListImmediate& RHICmdList)
			{
				for (const TPair<UMaterialInterface*, float>& VTRequest : VTRequests)
				{
					UMaterialInterface* Material = VTRequest.Key;
					FMaterialRenderProxy* MaterialRenderProxy = Material->GetRenderProxy();

					GetRendererModule().RequestVirtualTextureTiles(MaterialRenderProxy, FVector2D(VTRequest.Value, VTRequest.Value), FeatureLevel);
				}

				const uint32 NumFramesBeforeRender = CVarHLODWarmupNumFrames.GetValueOnRenderThread();
				for (const Nanite::FResources* Resource : NaniteRequests)
				{
					GetRendererModule().PrefetchNaniteResource(Resource, NumFramesBeforeRender);
				}
			});
	}
}

bool UHLODSubsystem::RequestUnloading(const UWorldPartitionRuntimeCell* InCell)
{
	// Test if warmup is disabled globally.
	bool bPerformWarmpup = CVarHLODWarmupEnabled.GetValueOnGameThread() != 0 && CVarHLODWarmupNumFrames.GetValueOnGameThread() > 0;
	if (!bPerformWarmpup)
	{
		return true;
	}

	FCellData& CellData = CellsData.FindChecked(InCell->GetFName());

	// If cell wasn't even visible yet or doesn't have HLOD actors, skip warmup.
	bPerformWarmpup = !CellData.LoadedHLODs.IsEmpty() && CellData.bIsCellVisible;
	if (!bPerformWarmpup)
	{
		return true;
	}

	// Verify that at least one HLOD actor associated with this cell needs warmup.
	bPerformWarmpup = Algo::AnyOf(CellData.LoadedHLODs, [](const AWorldPartitionHLOD* HLODActor) { return HLODActor->DoesRequireWarmup(); });
	if (!bPerformWarmpup)
	{
		return true;
	}

	// In case a previous request to unload was aborted and the cell never unloaded... assume warmup requests are expired after a given amount of frames.
	const uint32 WarmupExpiredFrames = 30;

	uint32 CurrentFrameNumber = GetWorld()->Scene->GetFrameNumber();

	// Trigger warmup on the first request to unload, or if a warmup request expired
	if (CellData.WarmupEndFrame == INDEX_NONE || CurrentFrameNumber > (CellData.WarmupEndFrame + WarmupExpiredFrames))
	{
		// Warmup will be triggered in the next BeginRenderView() call, at which point the frame number will have been incremented.
		CellData.WarmupStartFrame = CurrentFrameNumber + 1; 
		CellData.WarmupEndFrame = CellData.WarmupStartFrame + CVarHLODWarmupNumFrames.GetValueOnGameThread();
		CellsToWarmup.Add(&CellData);
	}

	// Test if warmup is completed
	bool bCanUnload = CurrentFrameNumber >= CellData.WarmupEndFrame;
	if (bCanUnload)
	{
		CellData.WarmupStartFrame = INDEX_NONE;
		CellData.WarmupEndFrame = INDEX_NONE;
	}

	return bCanUnload;
}

void UHLODSubsystem::OnBeginRenderViews(const FSceneViewFamily& InViewFamily)
{
	for (TSet<FCellData*>::TIterator CellIt(CellsToWarmup); CellIt; ++CellIt)
	{
		FCellData* CellPendingUnload = *CellIt;
	
		MakeRenderResourcesResident(*CellPendingUnload, InViewFamily);

		// Stop processing this cell if warmup is done.
		if (InViewFamily.FrameNumber >= CellPendingUnload->WarmupEndFrame)
		{
			CellIt.RemoveCurrent();
		}
	}
}

void FHLODResourcesResidencySceneViewExtension::BeginRenderViewFamily(FSceneViewFamily& InViewFamily)
{
	GetWorld()->GetSubsystem<UHLODSubsystem>()->OnBeginRenderViews(InViewFamily);
}

#undef LOCTEXT_NAMESPACE
