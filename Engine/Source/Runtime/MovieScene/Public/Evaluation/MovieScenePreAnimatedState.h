// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "Misc/TVariant.h"
#include "Evaluation/MovieSceneAnimTypeID.h"
#include "Evaluation/MovieSceneEvaluationKey.h"
#include "Evaluation/MovieSceneCompletionMode.h"
#include "Evaluation/IMovieSceneEvaluationHook.h"
#include "EntitySystem/TrackInstance/MovieSceneTrackInstance.h"
#include "EntitySystem/MovieSceneSequenceInstanceHandle.h"
#include "EntitySystem/MovieScenePropertySystemTypes.h"


class UObject;
class IMovieScenePlayer;
class FMovieScenePreAnimatedState;
class UMovieSceneEntitySystemLinker;

struct IMovieScenePreAnimatedTokenProducer;
struct IMovieScenePreAnimatedGlobalTokenProducer;

namespace UE
{
namespace MovieScene
{

struct FPreAnimatedStateEntry;
struct FPreAnimatedStateExtension;
struct FPreAnimatedTemplateCaptureSources;
struct FAnimTypePreAnimatedStateObjectStorage;
struct FAnimTypePreAnimatedStateMasterStorage;
struct FPreAnimatedTrackInstanceCaptureSources;
struct FPreAnimatedEvaluationHookCaptureSources;

}
}

/**
 * Class that caches pre-animated state for objects that were manipulated by sequencer
 */
class FMovieScenePreAnimatedState
{
public:

	FMovieScenePreAnimatedState() = default;

	FMovieScenePreAnimatedState(const FMovieScenePreAnimatedState&) = delete;
	FMovieScenePreAnimatedState& operator=(const FMovieScenePreAnimatedState&) = delete;

	MOVIESCENE_API ~FMovieScenePreAnimatedState();

	MOVIESCENE_API void Initialize(UMovieSceneEntitySystemLinker* Linker, UE::MovieScene::FInstanceHandle InstanceHandle);

	/**
	 * Check whether this sequence instance is capturing any and all changes of state so they can be restored later
	 */
	MOVIESCENE_API bool IsCapturingGlobalPreAnimatedState() const;

	/**
	 * Check whether this sequence instance is capturing any and all changes of state so they can be restored later
	 */
	MOVIESCENE_API void EnableGlobalPreAnimatedStateCapture();

	/**
	 * Retrieve the linker this container is bound to
	 */
	MOVIESCENE_API UMovieSceneEntitySystemLinker* GetLinker() const;

public:

	/**
	 * Save the current state of an object as defined by the specified token producer, identified by a specific anim type ID
	 * This will use the currently evaluating track template, evaluation hook or track instance (and its 'When Finished' property) as the capture source
	 */
	MOVIESCENE_API void SavePreAnimatedState(UObject& InObject, FMovieSceneAnimTypeID InTokenType, const IMovieScenePreAnimatedTokenProducer& Producer);

	/**
	 * Save the current state of the environment as defined by the specified token producer, identified by a specific anim type ID
	 * This will use the currently evaluating track template, evaluation hook or track instance (and its 'When Finished' property) as the capture source
	 */
	MOVIESCENE_API void SavePreAnimatedState(FMovieSceneAnimTypeID InTokenType, const IMovieScenePreAnimatedGlobalTokenProducer& Producer);

public:

	MOVIESCENE_API void OnFinishedEvaluating(const FMovieSceneEvaluationKey& Key);

	MOVIESCENE_API void OnFinishedEvaluating(const UObject* EvaluationHook, FMovieSceneSequenceID SequenceID);

public:

	MOVIESCENE_API void RestorePreAnimatedState();

	MOVIESCENE_API void RestorePreAnimatedState(UObject& Object);

	MOVIESCENE_API void RestorePreAnimatedState(UClass* GeneratedClass);

	MOVIESCENE_API void RestorePreAnimatedState(UObject& Object, TFunctionRef<bool(FMovieSceneAnimTypeID)> InFilter);

	/**
	 * Discard any tokens that relate to entity animation (ie sections or tracks) without restoring the values.
	 * Any global pre-animated state tokens (that reset the animation when saving a map, for instance) will remain.
	 */
	MOVIESCENE_API void DiscardEntityTokens();

	/**
	 * Discard any tokens that relate to the requested object (ie sections or tracks) without restoring the values.
	 * Any global pre-animated state tokens for this object will be removed.
	 */
	MOVIESCENE_API void DiscardAndRemoveEntityTokensForObject(UObject& Object);

	/**
	 * Called when objects have been replaced so that pre animated state can swap out to the new objects
	 */
	MOVIESCENE_API void OnObjectsReplaced(const TMap<UObject*, UObject*>& ReplacementMap);

public:

	/**
	 * Search the global pre-animated state extension for any captured state that originated from this sequence
	 * WARNING: This is a linear search across all state, and so is potentially very slow
	 */
	MOVIESCENE_API bool ContainsAnyStateForSequence() const;

private:

	void ConditionalInitializeEntityStorage(bool bOverrideWantsRestoreState);

	void InitializeStorage(TSharedPtr<UE::MovieScene::FPreAnimatedStateExtension> Extension);

	void AddSourceMetaData(const UE::MovieScene::FPreAnimatedStateEntry& Entry);

private:

	friend struct FScopedPreAnimatedCaptureSource;

	/** Weak pointer to the linker that we're associated with */
	TWeakObjectPtr<UMovieSceneEntitySystemLinker> WeakLinker;

	/** Pointers to the storage for state bound to objects, organized by FMovieSceneAnimTypeID */
	TWeakPtr<UE::MovieScene::FAnimTypePreAnimatedStateObjectStorage> WeakObjectStorage;
	/** Pointers to the storage for state created from master tracks, or otherwise not bound to objects */
	TWeakPtr<UE::MovieScene::FAnimTypePreAnimatedStateMasterStorage> WeakMasterStorage;

	/** Meta-data ledger for any pre-animated state that originates from track templates */
	TSharedPtr<UE::MovieScene::FPreAnimatedTemplateCaptureSources> TemplateMetaData;
	/** Meta-data ledger for any pre-animated state that originates from evaluation hooks */
	TSharedPtr<UE::MovieScene::FPreAnimatedEvaluationHookCaptureSources> EvaluationHookMetaData;

	/** The instance handle for the root sequence instance */
	UE::MovieScene::FInstanceHandle InstanceHandle;

	bool bCapturingGlobalPreAnimatedState;
};
