// Copyright Epic Games, Inc. All Rights Reserved.

#include "ColorCorrectRegion.h"
#include "ColorCorrectRegionsSubsystem.h"
#include "Engine/Classes/Components/MeshComponent.h"
#include "CoreMinimal.h"
#include "UObject/ConstructorHelpers.h"

AColorCorrectRegion::AColorCorrectRegion(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
	, Type(EColorCorrectRegionsType::Sphere)
	, Priority(0)
	, Intensity(1.0)
	, Inner(0.5)
	, Outer(1.0)
	, Falloff(1.0)
	, Invert(false)
	, Temperature(6500)
	, Enabled(true)
	, ExcludeStencil(false)
	, ColorCorrectRegionsSubsystem(nullptr)
{
	PrimaryActorTick.bCanEverTick = true;
}

void AColorCorrectRegion::BeginPlay()
{	
	Super::BeginPlay();
	if (const UWorld* World = GetWorld())
	{
		ColorCorrectRegionsSubsystem = World->GetSubsystem<UColorCorrectRegionsSubsystem>();
	}

	if (ColorCorrectRegionsSubsystem)
	{
		ColorCorrectRegionsSubsystem->OnActorSpawned(this);
	}
}

void AColorCorrectRegion::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ColorCorrectRegionsSubsystem)
	{
		ColorCorrectRegionsSubsystem->OnActorDeleted(this);
		ColorCorrectRegionsSubsystem = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void AColorCorrectRegion::BeginDestroy()
{
	if (ColorCorrectRegionsSubsystem)
	{
		ColorCorrectRegionsSubsystem->OnActorDeleted(this);
		ColorCorrectRegionsSubsystem = nullptr;
	}
	
	Super::BeginDestroy();
}

bool AColorCorrectRegion::ShouldTickIfViewportsOnly() const
{
	return true;
}

void AColorCorrectRegion::TickActor(float DeltaTime, ELevelTick TickType, FActorTickFunction& ThisTickFunction)
{
	Super::Tick(DeltaTime);
	FTransform CurrentFrameTransform = GetTransform();
	if (!PreviousFrameTransform.Equals(CurrentFrameTransform))
	{
		PreviousFrameTransform = CurrentFrameTransform;
		GetActorBounds(true, BoxOrigin, BoxExtent);
	}
}

void AColorCorrectRegion::Cleanup()
{
	ColorCorrectRegionsSubsystem = nullptr;
}

#if WITH_EDITOR
void AColorCorrectRegion::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);
	const FName PropertyName = PropertyChangedEvent.GetPropertyName();

	// Reorder all CCRs after the Priority property has changed.
	// Also, in context of Multi-User: PropertyChangedEvent can be a stub without the actual property data. 
	// Therefore we need to refresh priority if PropertyChangedEvent.Property is nullptr. 
	if (PropertyName == GET_MEMBER_NAME_CHECKED(AColorCorrectRegion, Priority) || PropertyChangedEvent.Property == nullptr)
	{
		if (!ColorCorrectRegionsSubsystem)
		{
			if (const UWorld* World = GetWorld())
			{
				ColorCorrectRegionsSubsystem = World->GetSubsystem<UColorCorrectRegionsSubsystem>();
			}
		}
		if (ColorCorrectRegionsSubsystem)
		{
			ColorCorrectRegionsSubsystem->SortRegionsByPriority();
		}
	}
	GetActorBounds(true, BoxOrigin, BoxExtent);
}
#endif //WITH_EDITOR

