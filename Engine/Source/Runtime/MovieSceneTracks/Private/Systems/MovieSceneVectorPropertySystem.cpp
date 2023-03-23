// Copyright Epic Games, Inc. All Rights Reserved.

#include "Systems/MovieSceneVectorPropertySystem.h"
#include "EntitySystem/MovieSceneBoundObjectInstantiator.h"
#include "EntitySystem/MovieSceneDecompositionQuery.h"
#include "EntitySystem/MovieSceneEntitySystemTypes.h"
#include "MovieSceneTracksComponentTypes.h"
#include "Systems/DoubleChannelEvaluatorSystem.h"
#include "Systems/FloatChannelEvaluatorSystem.h"
#include "Systems/MovieScenePiecewiseDoubleBlenderSystem.h"
#include "Systems/MovieScenePiecewiseFloatBlenderSystem.h"
#include "Systems/MovieScenePropertyInstantiator.h"

UMovieSceneFloatVectorPropertySystem::UMovieSceneFloatVectorPropertySystem(const FObjectInitializer& ObjInit)
	: Super(ObjInit)
{
	SystemExclusionContext |= UE::MovieScene::EEntitySystemContext::Interrogation;

	BindToProperty(UE::MovieScene::FMovieSceneTracksComponentTypes::Get()->FloatVector);

	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		// We need our floats correctly evaluated and blended, so we are downstream from those systems.
		DefineImplicitPrerequisite(UFloatChannelEvaluatorSystem::StaticClass(), GetClass());
		DefineImplicitPrerequisite(UMovieScenePiecewiseFloatBlenderSystem::StaticClass(), GetClass());
	}
}

void UMovieSceneFloatVectorPropertySystem::OnRun(FSystemTaskPrerequisites& InPrerequisites, FSystemSubsequentTasks& Subsequents)
{
	Super::OnRun(InPrerequisites, Subsequents);
}

UMovieSceneDoubleVectorPropertySystem::UMovieSceneDoubleVectorPropertySystem(const FObjectInitializer& ObjInit)
	: Super(ObjInit)
{
	SystemExclusionContext |= UE::MovieScene::EEntitySystemContext::Interrogation;

	BindToProperty(UE::MovieScene::FMovieSceneTracksComponentTypes::Get()->DoubleVector);

	if (HasAnyFlags(RF_ClassDefaultObject))
	{
		// We need our floats correctly evaluated and blended, so we are downstream from those systems.
		DefineImplicitPrerequisite(UDoubleChannelEvaluatorSystem::StaticClass(), GetClass());
		DefineImplicitPrerequisite(UMovieScenePiecewiseDoubleBlenderSystem::StaticClass(), GetClass());
	}
}

void UMovieSceneDoubleVectorPropertySystem::OnRun(FSystemTaskPrerequisites& InPrerequisites, FSystemSubsequentTasks& Subsequents)
{
	Super::OnRun(InPrerequisites, Subsequents);
}

