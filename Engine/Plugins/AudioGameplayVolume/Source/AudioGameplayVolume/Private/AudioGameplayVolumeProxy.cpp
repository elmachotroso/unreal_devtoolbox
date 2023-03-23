// Copyright Epic Games, Inc. All Rights Reserved.

#include "AudioGameplayVolumeProxy.h"
#include "AudioGameplayVolumeProxyMutator.h"
#include "AudioGameplayVolumeLogs.h"
#include "AudioGameplayVolumeComponent.h"
#include "Interfaces/IAudioGameplayCondition.h"
#include "Components/BrushComponent.h"
#include "Components/PrimitiveComponent.h"

UAudioGameplayVolumeProxy::UAudioGameplayVolumeProxy(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UAudioGameplayVolumeProxy::ContainsPosition(const FVector& Position) const
{ 
	return false;
}

void UAudioGameplayVolumeProxy::InitFromComponent(const UAudioGameplayVolumeProxyComponent* Component)
{
	if (!Component || !Component->GetWorld())
	{
		UE_LOG(AudioGameplayVolumeLog, Verbose, TEXT("AudioGameplayVolumeProxy - Attempted Init from invalid volume component!"));
		return;
	}

	VolumeID = Component->GetUniqueID();
	WorldID = Component->GetWorld()->GetUniqueID();

	PayloadType = PayloadFlags::AGCP_None;
	ProxyVolumeMutators.Reset();

	TInlineComponentArray<UAudioGameplayVolumeComponentBase*> Components(Component->GetOwner());
	for (UAudioGameplayVolumeComponentBase* Comp : Components)
	{
		if (!Comp || !Comp->IsActive())
		{
			continue;
		}

		TSharedPtr<FProxyVolumeMutator> NewMutator = Comp->CreateMutator();
		if (NewMutator.IsValid())
		{
			NewMutator->VolumeID = VolumeID;
			NewMutator->WorldID = WorldID;

			AddPayloadType(NewMutator->PayloadType);
			ProxyVolumeMutators.Emplace(NewMutator);
		}
	}
}

void UAudioGameplayVolumeProxy::FindMutatorPriority(FAudioProxyMutatorPriorities& Priorities) const
{
	check(IsInAudioThread());
	for (const TSharedPtr<FProxyVolumeMutator>& ProxyVolumeMutator : ProxyVolumeMutators)
	{
		if (!ProxyVolumeMutator.IsValid())
		{
			continue;
		}

		ProxyVolumeMutator->UpdatePriority(Priorities);
	}
}

void UAudioGameplayVolumeProxy::GatherMutators(const FAudioProxyMutatorPriorities& Priorities, FAudioProxyMutatorSearchResult& OutResult) const
{
	check(IsInAudioThread());
	for (const TSharedPtr<FProxyVolumeMutator>& ProxyVolumeMutator : ProxyVolumeMutators)
	{
		if (!ProxyVolumeMutator.IsValid())
		{
			continue;
		}

		if (ProxyVolumeMutator->CheckPriority(Priorities))
		{
			ProxyVolumeMutator->Apply(OutResult.InteriorSettings);
			OutResult.MatchingMutators.Add(ProxyVolumeMutator);
		}
	}
}

void UAudioGameplayVolumeProxy::AddPayloadType(PayloadFlags InType)
{
	PayloadType |= InType;
}

bool UAudioGameplayVolumeProxy::HasPayloadType(PayloadFlags InType) const
{
	return (PayloadType & InType) != PayloadFlags::AGCP_None;
}

uint32 UAudioGameplayVolumeProxy::GetVolumeID() const
{ 
	return VolumeID;
}

uint32 UAudioGameplayVolumeProxy::GetWorldID() const
{
	return WorldID;
}

UAGVPrimitiveComponentProxy::UAGVPrimitiveComponentProxy(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UAGVPrimitiveComponentProxy::ContainsPosition(const FVector& Position) const
{
	FBodyInstance* BodyInstancePointer = nullptr;
	if (UPrimitiveComponent* PrimitiveComponent = WeakPrimative.Get())
	{
		if (PrimitiveComponent->IsPhysicsStateCreated() && PrimitiveComponent->HasValidPhysicsState())
		{
			BodyInstancePointer = PrimitiveComponent->GetBodyInstance();
		}
	}
	
	if (!BodyInstancePointer)
	{
		return false;
	}

	float DistanceSquared = 0.f;
	FVector PointOnBody = FVector::ZeroVector;
	return BodyInstancePointer->GetSquaredDistanceToBody(Position, DistanceSquared, PointOnBody) && FMath::IsNearlyZero(DistanceSquared);
}

void UAGVPrimitiveComponentProxy::InitFromComponent(const UAudioGameplayVolumeProxyComponent* Component)
{
	Super::InitFromComponent(Component);

	if (Component)
	{
		TInlineComponentArray<UPrimitiveComponent*> PrimitiveComponents(Component->GetOwner());
		if (ensureMsgf(PrimitiveComponents.Num() == 1, TEXT("An Audio Gameplay Volume Shape Proxy requires exactly one Primitive Component on the owning actor")))
		{
			WeakPrimative = PrimitiveComponents[0];
		}
	}
}

UAGVConditionProxy::UAGVConditionProxy(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
}

bool UAGVConditionProxy::ContainsPosition(const FVector& Position) const
{
	const UObject* ObjectWithInterface = WeakObject.Get();
	if (ObjectWithInterface && ObjectWithInterface->Implements<UAudioGameplayCondition>())
	{
		return IAudioGameplayCondition::Execute_ConditionMet(ObjectWithInterface)
			|| IAudioGameplayCondition::Execute_ConditionMet_Position(ObjectWithInterface, Position);
	}

	return false;
}

void UAGVConditionProxy::InitFromComponent(const UAudioGameplayVolumeProxyComponent* Component)
{
	Super::InitFromComponent(Component);

	AActor* OwnerActor = Component ? Component->GetOwner() : nullptr;
	if (OwnerActor)
	{
		if (OwnerActor->Implements<UAudioGameplayCondition>())
		{
			WeakObject = MakeWeakObjectPtr(OwnerActor);
		}
		else
		{
			TInlineComponentArray<UActorComponent*> AllComponents(OwnerActor);

			for (UActorComponent* ActorComponent : AllComponents)
			{
				if (ActorComponent && ActorComponent->Implements<UAudioGameplayCondition>())
				{
					WeakObject = MakeWeakObjectPtr(ActorComponent);
					break;
				}
			}
		}
	}
}
