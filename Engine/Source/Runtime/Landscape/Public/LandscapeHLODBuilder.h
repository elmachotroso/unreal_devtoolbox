// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "WorldPartition/HLOD/HLODBuilder.h"
#include "LandscapeHLODBuilder.generated.h"

UCLASS()
class LANDSCAPE_API ULandscapeHLODBuilder : public UHLODBuilder
{
	GENERATED_UCLASS_BODY()

public:
#if WITH_EDITOR
	virtual uint32 ComputeHLODHash(const UActorComponent* InSourceComponent) const;

	/**
	 * Components created with this method needs to be properly outered & assigned to your target actor.
	 */
	virtual TArray<UActorComponent*> Build(const FHLODBuildContext& InHLODBuildContext, const TArray<UActorComponent*>& InSourceComponents) const override;
#endif
};
