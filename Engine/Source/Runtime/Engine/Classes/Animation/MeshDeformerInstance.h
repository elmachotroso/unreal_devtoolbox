// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MeshDeformerInstance.generated.h"

class FSceneInterface;

/** 
 * Base class for mesh deformers instances.
 * This contains the per instance state for a UMeshDeformer.
 */
UCLASS(Abstract)
class ENGINE_API UMeshDeformerInstance : public UObject
{
	GENERATED_BODY()

public:
	/** Enumeration for workloads to enqueue. */
	enum EWorkLoad
	{
		WorkLoad_Setup,
		WorkLoad_Trigger,
		WorkLoad_Update,
	};

	/** Get if mesh deformer is active (compiled and valid). */
	virtual bool IsActive() const PURE_VIRTUAL(, return false;);
	/** Enqueue the mesh deformer workload on a scene. */
	virtual void EnqueueWork(FSceneInterface* InScene, EWorkLoad WorkLoadType) PURE_VIRTUAL(, );
};
