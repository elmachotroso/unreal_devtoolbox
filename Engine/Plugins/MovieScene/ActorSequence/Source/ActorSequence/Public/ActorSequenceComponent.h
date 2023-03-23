// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Components/ActorComponent.h"
#include "MovieSceneSequencePlayer.h"
#include "MovieSceneSequenceTickManager.h"
#include "ActorSequenceComponent.generated.h"


class UActorSequence;
class UActorSequencePlayer;


/**
 * Movie scene animation embedded within an actor.
 */
UCLASS(Blueprintable, Experimental, ClassGroup=Sequence, hidecategories=(Collision, Cooking, Activation), meta=(BlueprintSpawnableComponent))
class ACTORSEQUENCE_API UActorSequenceComponent
	: public UActorComponent
	, public IMovieSceneSequenceActor
{
public:
	GENERATED_BODY()

	UActorSequenceComponent(const FObjectInitializer& Init);

	UActorSequence* GetSequence() const
	{
		return Sequence;
	}

	UActorSequencePlayer* GetSequencePlayer() const 
	{
		return SequencePlayer;
	}
	
	// UActorComponent interface
	virtual void PostInitProperties() override;
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, enum ELevelTick TickType, FActorComponentTickFunction *ThisTickFunction) override;

	// IMovieSceneSequenceActor interface
	virtual void TickFromSequenceTickManager(float DeltaSeconds) override;

protected:

	UPROPERTY(EditAnywhere, Category="Playback", meta=(ShowOnlyInnerProperties))
	FMovieSceneSequencePlaybackSettings PlaybackSettings;

	/** Embedded actor sequence data */
	UPROPERTY(EditAnywhere, Instanced, Category=Animation)
	TObjectPtr<UActorSequence> Sequence;

	UPROPERTY(transient, BlueprintReadOnly, Category=Animation)
	TObjectPtr<UActorSequencePlayer> SequencePlayer;
};
