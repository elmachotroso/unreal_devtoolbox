// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "EntitySystem/MovieSceneBlenderSystem.h"
#include "EntitySystem/MovieSceneCachedEntityFilterResult.h"
#include "Systems/MovieSceneBlenderSystemHelper.h"
#include "MovieScenePiecewiseEnumBlenderSystem.generated.h"


UCLASS()
class MOVIESCENETRACKS_API UMovieScenePiecewiseEnumBlenderSystem : public UMovieSceneBlenderSystem
{
public:

	GENERATED_BODY()

	UMovieScenePiecewiseEnumBlenderSystem(const FObjectInitializer& ObjInit);

	virtual void OnRun(FSystemTaskPrerequisites& InPrerequisites, FSystemSubsequentTasks& Subsequents) override;

private:

	UE::MovieScene::TSimpleBlenderSystemImpl<uint8> Impl;
};

