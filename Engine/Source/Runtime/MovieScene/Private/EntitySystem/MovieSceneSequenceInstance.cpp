// Copyright Epic Games, Inc. All Rights Reserved.

#include "EntitySystem/MovieSceneSequenceInstance.h"
#include "EntitySystem/MovieSceneEntitySystemLinker.h"
#include "EntitySystem/MovieSceneEntitySystem.h"
#include "EntitySystem/MovieSceneSequenceUpdaters.h"
#include "Evaluation/PreAnimatedState/MovieScenePreAnimatedStateExtension.h"

#include "Compilation/MovieSceneCompiledVolatilityManager.h"
#include "Compilation/MovieSceneCompiledDataManager.h"

#include "Evaluation/MovieSceneEvaluationTemplateInstance.h"
#include "Evaluation/Instances/MovieSceneTrackEvaluator.h"
#include "Evaluation/MovieSceneRootOverridePath.h"

#include "IMovieScenePlayer.h"
#include "MovieSceneSequencePlayer.h"
#include "MovieSceneTimeHelpers.h"

#include "Algo/IndexOf.h"

namespace UE
{
namespace MovieScene
{


DECLARE_CYCLE_STAT(TEXT("Sequence Instance Update"), MovieSceneEval_SequenceInstanceUpdate, STATGROUP_MovieSceneEval);
DECLARE_CYCLE_STAT(TEXT("[External] Sequence Instance Post-Update"), MovieSceneEval_SequenceInstancePostUpdate, STATGROUP_MovieSceneEval);


void PurgeStaleTrackTemplates(UMovieSceneCompiledDataManager* CompiledDataManager, FMovieSceneCompiledDataID CompiledDataID)
{
	FMovieSceneEvaluationTemplate* EvalTemplate = const_cast<FMovieSceneEvaluationTemplate*>(CompiledDataManager->FindTrackTemplate(CompiledDataID));
	if (EvalTemplate)
	{
		EvalTemplate->PurgeStaleTracks();
	}

	// Do the same for all subsequences
	const FMovieSceneSequenceHierarchy* Hierarchy = CompiledDataManager->FindHierarchy(CompiledDataID);
	if (Hierarchy)
	{
		for (const TTuple<FMovieSceneSequenceID, FMovieSceneSubSequenceData>& Pair : Hierarchy->AllSubSequenceData())
		{
			UMovieSceneSequence* SubSequence = Pair.Value.GetLoadedSequence();
			if (!SubSequence)
			{
				continue;
			}
			FMovieSceneCompiledDataID SubCompiledDataID = CompiledDataManager->FindDataID(SubSequence);
			if (!SubCompiledDataID.IsValid())
			{
				continue;
			}

			FMovieSceneEvaluationTemplate* SubEvalTemplate = const_cast<FMovieSceneEvaluationTemplate*>(CompiledDataManager->FindTrackTemplate(SubCompiledDataID));
			if (SubEvalTemplate)
			{
				SubEvalTemplate->PurgeStaleTracks();
			}
		}
	}
}




FSequenceInstance::FSequenceInstance(UMovieSceneEntitySystemLinker* Linker, IMovieScenePlayer* Player, FInstanceHandle InInstanceHandle)
	: SequenceID(MovieSceneSequenceID::Root)
	, RootOverrideSequenceID(MovieSceneSequenceID::Root)
	, PlayerIndex(Player->GetUniqueIndex())
	, InstanceHandle(InInstanceHandle)
	, RootInstanceHandle(InstanceHandle)
{
	// Root instances always start in a finished state in order to ensure that 'Start'
	// is called correctly for the top level instance. This is subtly different from
	// bHasEverUpdated since a sequence instance can be Finished and restarted multiple times
	bFinished = true;
	bHasEverUpdated = false;

	CompiledDataID = Player->GetEvaluationTemplate().GetCompiledDataID();

	FMovieSceneObjectCache& ObjectCache = Player->State.GetObjectCache(SequenceID);
	OnInvalidateObjectBindingHandle = ObjectCache.OnBindingInvalidated.AddUObject(Linker, &UMovieSceneEntitySystemLinker::InvalidateObjectBinding, InstanceHandle);

	InvalidateCachedData(Linker);
}

FSequenceInstance::FSequenceInstance(UMovieSceneEntitySystemLinker* Linker, IMovieScenePlayer* Player, FInstanceHandle InInstanceHandle, FInstanceHandle InRootInstanceHandle, FMovieSceneSequenceID InSequenceID, FMovieSceneCompiledDataID InCompiledDataID)
	: CompiledDataID(InCompiledDataID)
	, SequenceID(InSequenceID)
	, RootOverrideSequenceID(MovieSceneSequenceID::Invalid)
	, PlayerIndex(Player->GetUniqueIndex())
	, InstanceHandle(InInstanceHandle)
	, RootInstanceHandle(InRootInstanceHandle)
{
	// Sub Sequence instances always start in a non-finished state because they will only ever
	// be created if they are active, and the Start/Update/Finish loop does not apply to sub-instances
	bFinished = false;
	bHasEverUpdated = false;

	FMovieSceneObjectCache& ObjectCache = Player->State.GetObjectCache(SequenceID);
	OnInvalidateObjectBindingHandle = ObjectCache.OnBindingInvalidated.AddUObject(Linker, &UMovieSceneEntitySystemLinker::InvalidateObjectBinding, InstanceHandle);

	InvalidateCachedData(Linker);
}

FSequenceInstance::~FSequenceInstance()
{}

FSequenceInstance::FSequenceInstance(FSequenceInstance&&) = default;

FSequenceInstance& FSequenceInstance::operator=(FSequenceInstance&&) = default;

IMovieScenePlayer* FSequenceInstance::GetPlayer() const
{
	return IMovieScenePlayer::Get(PlayerIndex);
}

void FSequenceInstance::InitializeLegacyEvaluator(UMovieSceneEntitySystemLinker* Linker)
{
	IMovieScenePlayer* Player = GetPlayer();
	check(Player);

	UMovieSceneCompiledDataManager*     CompiledDataManager = Player->GetEvaluationTemplate().GetCompiledDataManager();
	const FMovieSceneCompiledDataEntry& CompiledEntry       = CompiledDataManager->GetEntryRef(CompiledDataID);

	if (EnumHasAnyFlags(CompiledEntry.AccumulatedMask, EMovieSceneSequenceCompilerMask::EvaluationTemplate))
	{
		if (!LegacyEvaluator)
		{
			LegacyEvaluator = MakeUnique<FMovieSceneTrackEvaluator>(CompiledEntry.GetSequence(), CompiledDataID, CompiledDataManager);
		}
	}
	else if (LegacyEvaluator)
	{
		LegacyEvaluator->Finish(*Player);
		LegacyEvaluator = nullptr;
	}
}

void FSequenceInstance::InvalidateCachedData(UMovieSceneEntitySystemLinker* Linker)
{
	Ledger.Invalidate();

	IMovieScenePlayer* Player = GetPlayer();
	check(Player);

	UMovieSceneCompiledDataManager* CompiledDataManager = Player->GetEvaluationTemplate().GetCompiledDataManager();

	UMovieSceneSequence* Sequence = CompiledDataManager->GetEntryRef(CompiledDataID).GetSequence();
	Player->State.AssignSequence(SequenceID, *Sequence, *Player);

	if (SequenceID == MovieSceneSequenceID::Root)
	{
		// Try and recreate the volatility manager if this sequence is now volatile
		if (!VolatilityManager)
		{
			VolatilityManager = FCompiledDataVolatilityManager::Construct(*Player, CompiledDataID, CompiledDataManager);
			if (VolatilityManager)
			{
				VolatilityManager->ConditionalRecompile(*Player, CompiledDataID, CompiledDataManager);
			}
		}

		ISequenceUpdater::FactoryInstance(SequenceUpdater, CompiledDataManager, CompiledDataID);

		SequenceUpdater->InvalidateCachedData(Linker);

		if (LegacyEvaluator)
		{
			LegacyEvaluator->InvalidateCachedData();
		}

		InitializeLegacyEvaluator(Linker);
	}
}

void FSequenceInstance::DissectContext(UMovieSceneEntitySystemLinker* Linker, const FMovieSceneContext& InContext, TArray<TRange<FFrameTime>>& OutDissections)
{
	check(SequenceID == MovieSceneSequenceID::Root);


	IMovieScenePlayer* Player = GetPlayer();

	if (VolatilityManager)
	{
		UMovieSceneCompiledDataManager* CompiledDataManager = Player->GetEvaluationTemplate().GetCompiledDataManager();
		if (VolatilityManager->ConditionalRecompile(*Player, CompiledDataID, CompiledDataManager))
		{
			InvalidateCachedData(Linker);
		}
	}

	SequenceUpdater->DissectContext(Linker, Player, InContext, OutDissections);
}

void FSequenceInstance::Start(UMovieSceneEntitySystemLinker* Linker, const FMovieSceneContext& InContext)
{
	check(SequenceID == MovieSceneSequenceID::Root);

	bFinished = false;
	bHasEverUpdated = true;

	IMovieScenePlayer* Player = GetPlayer();
	SequenceUpdater->Start(Linker, InstanceHandle, Player, InContext);
}

void FSequenceInstance::Update(UMovieSceneEntitySystemLinker* Linker, const FMovieSceneContext& InContext)
{
	SCOPE_CYCLE_COUNTER(MovieSceneEval_SequenceInstanceUpdate);

#if STATS || ENABLE_STATNAMEDEVENTS
	const bool bShouldTrackObject = Stats::IsThreadCollectingData();
	FScopeCycleCounterUObject ContextScope(bShouldTrackObject ? GetPlayer()->AsUObject() : nullptr);
#endif

	bHasEverUpdated = true;

	if (bFinished)
	{
		Start(Linker, InContext);
	}

	Context = InContext;
	SequenceUpdater->Update(Linker, InstanceHandle, GetPlayer(), InContext);
}

void FSequenceInstance::Finish(UMovieSceneEntitySystemLinker* Linker)
{
	if (IsRootSequence() && !bHasEverUpdated)
	{
		return;
	}

	Linker->EntityManager.IncrementSystemSerial();
	bFinished = true;
	Ledger.UnlinkEverything(Linker);

	Ledger = FEntityLedger();

	IMovieScenePlayer* Player = IMovieScenePlayer::Get(PlayerIndex);
	if (!ensure(Player))
	{
		return;
	}

	if (SequenceUpdater)
	{
		SequenceUpdater->Finish(Linker, InstanceHandle, Player);
	}

	if (LegacyEvaluator)
	{
		LegacyEvaluator->Finish(*Player);
	}

	if (IsRootSequence())
	{
		FMovieSceneSpawnRegister& SpawnRegister = Player->GetSpawnRegister();
		SpawnRegister.ForgetExternallyOwnedSpawnedObjects(Player->State, *Player);
		SpawnRegister.CleanUp(*Player);

		if (Player->PreAnimatedState.IsCapturingGlobalPreAnimatedState())
		{
			Linker->PreAnimatedState.RestoreGlobalState(FRestoreStateParams{ Linker, RootInstanceHandle });
		}
	}
}

void FSequenceInstance::PreEvaluation(UMovieSceneEntitySystemLinker* Linker)
{
	if (IsRootSequence())
	{
		IMovieScenePlayer* Player = GetPlayer();
		if (ensure(Player))
		{
			Player->PreEvaluation(Context);
		}
	}
}

void FSequenceInstance::RunLegacyTrackTemplates()
{
	if (LegacyEvaluator)
	{
		IMovieScenePlayer* Player = IMovieScenePlayer::Get(PlayerIndex);
		if (ensure(Player))
		{
			if (bFinished)
			{
				LegacyEvaluator->Finish(*Player);
			}
			else
			{
				LegacyEvaluator->Evaluate(Context, *Player, RootOverrideSequenceID);
			}
		}
	}
}

void FSequenceInstance::PostEvaluation(UMovieSceneEntitySystemLinker* Linker)
{
	Ledger.UnlinkOneShots(Linker);

	if (IsRootSequence())
	{
		IMovieScenePlayer* Player = GetPlayer();
		if (ensure(Player))
		{
			SCOPE_CYCLE_COUNTER(MovieSceneEval_SequenceInstancePostUpdate);


			// DANGER: This function is highly fragile due to the nature of IMovieScenePlayer::PostEvaluation
			//         being able to re-evaluate sequences. Ultimately this can lead to FSequenceInstances being
			//         created, destroyed, or reallocated. As such
			//
			//                  ***** the current this ptr can become invalid at any point ***** 
			//
			//         Any code which needs to run after PostEvaluate must cache any member variables it needs on
			//         the stack _before_ Player->PostEvaluation is called.


			// If this sequence is volatile and has legacy track templates, purge any stale track templates from the compiled data after evaluation
			const bool bShouldPurgeTemplates = VolatilityManager && LegacyEvaluator;

			UMovieSceneCompiledDataManager* LocalCompiledDataManager = bShouldPurgeTemplates ? Player->GetEvaluationTemplate().GetCompiledDataManager() : nullptr;
			FMovieSceneCompiledDataID       LocalCompiledDataID      = CompiledDataID;

			Player->PostEvaluation(Context);

			if (LocalCompiledDataManager)
			{
				PurgeStaleTrackTemplates(LocalCompiledDataManager, LocalCompiledDataID);
			}
		}
	}
}

void FSequenceInstance::DestroyImmediately(UMovieSceneEntitySystemLinker* Linker)
{
	if (!Ledger.IsEmpty())
	{
		UE_LOG(LogMovieSceneECS, Verbose, TEXT("Instance being destroyed without first having been finished by calling Finish()"));
		Ledger.UnlinkEverything(Linker);
	}

	if (SequenceUpdater)
	{
		SequenceUpdater->Destroy(Linker);
	}
}

void FSequenceInstance::OverrideRootSequence(UMovieSceneEntitySystemLinker* Linker, FMovieSceneSequenceID NewRootSequenceID)
{
	if (SequenceUpdater)
	{
		SequenceUpdater->OverrideRootSequence(Linker, InstanceHandle, NewRootSequenceID);
	}

	RootOverrideSequenceID = NewRootSequenceID;
}

FInstanceHandle FSequenceInstance::FindSubInstance(FMovieSceneSequenceID SubSequenceID) const
{
	return SequenceUpdater ? SequenceUpdater->FindSubInstance(SubSequenceID) : FInstanceHandle();
}

FMovieSceneEntityID FSequenceInstance::FindEntity(UObject* Owner, uint32 EntityID) const
{
	return Ledger.FindImportedEntity(FMovieSceneEvaluationFieldEntityKey{ decltype(FMovieSceneEvaluationFieldEntityKey::EntityOwner)(Owner), EntityID });
}

FSubSequencePath FSequenceInstance::GetSubSequencePath() const
{
	return FSubSequencePath(SequenceID, *GetPlayer());
}

} // namespace MovieScene
} // namespace UE
