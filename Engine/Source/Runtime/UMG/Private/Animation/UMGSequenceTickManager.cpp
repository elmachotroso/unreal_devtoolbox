// Copyright Epic Games, Inc. All Rights Reserved.

#include "Animation/UMGSequenceTickManager.h"
#include "Animation/UMGSequencePlayer.h"
#include "Blueprint/UserWidget.h"
#include "Engine/World.h"
#include "EntitySystem/MovieSceneEntitySystemLinker.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "Framework/Application/SlateApplication.h"
#include "Engine/Engine.h"

DECLARE_CYCLE_STAT(TEXT("Flush End of Frame Animations"), MovieSceneEval_FlushEndOfFrameAnimations, STATGROUP_MovieSceneEval);

static TAutoConsoleVariable<int32> CVarUMGMaxAnimationLatentActions(
	TEXT("Widget.MaxAnimationLatentActions"),
	100,
	TEXT("Defines the maximum number of latent actions that can be run in one frame."),
	ECVF_Default
);
int32 GFlushUMGAnimationsAtEndOfFrame = 1;
static FAutoConsoleVariableRef CVarUMGAnimationsAtEndOfFrame(
	TEXT("UMG.FlushAnimationsAtEndOfFrame"),
	GFlushUMGAnimationsAtEndOfFrame,
	TEXT("Whether to automatically flush any outstanding animations at the end of the frame, or just wait until next frame."),
	ECVF_Default
);

extern TAutoConsoleVariable<bool> CVarUserWidgetUseParallelAnimation;

UUMGSequenceTickManager::UUMGSequenceTickManager(const FObjectInitializer& Init)
	: Super(Init)
	, bIsTicking(false)
{
}

void UUMGSequenceTickManager::AddWidget(UUserWidget* InWidget)
{
	TWeakObjectPtr<UUserWidget> WeakWidget = InWidget;
	WeakUserWidgetData.Add(WeakWidget, FSequenceTickManagerWidgetData());
}

void UUMGSequenceTickManager::RemoveWidget(UUserWidget* InWidget)
{
	TWeakObjectPtr<UUserWidget> WeakWidget = InWidget;
	WeakUserWidgetData.Remove(WeakWidget);
}

void UUMGSequenceTickManager::OnWidgetTicked(UUserWidget* InWidget)
{
	if (FSequenceTickManagerWidgetData* WidgetData = WeakUserWidgetData.Find(InWidget))
	{
		WidgetData->bIsTicking = true;
	}
}

void UUMGSequenceTickManager::BeginDestroy()
{
	if (SlateApplicationPreTickHandle.IsValid())
	{
		if (FSlateApplication::IsInitialized())
		{
			FSlateApplication& SlateApp = FSlateApplication::Get();
			SlateApp.OnPreTick().Remove(SlateApplicationPreTickHandle);
			SlateApplicationPreTickHandle.Reset();

			SlateApp.OnPostTick().Remove(SlateApplicationPostTickHandle);
			SlateApplicationPostTickHandle.Reset();
		}
	}

	Super::BeginDestroy();
}

void UUMGSequenceTickManager::TickWidgetAnimations(float DeltaSeconds)
{
	if (!CVarUserWidgetUseParallelAnimation.GetValueOnGameThread())
	{
		return;
	}

	if (bIsTicking)
	{
		return;
	}

	// Don't tick the animation if inside of a PostLoad
	if (FUObjectThreadContext::Get().IsRoutingPostLoad)
	{
		return;
	}

	TGuardValue<bool> IsTickingGuard(bIsTicking, true);

	// Tick all animations in all active widgets.
	//
	// In the main code path (the one where animations are just chugging along), the UMG sequence players
	// will queue evaluations on the global sequencer ECS linker. In some specific cases, though (pausing,
	// stopping, etc.), we might see some blocking (immediate) evaluations running here.
	//
	// The WidgetData have one frame delay (they are updated at the end of the frame).
	// This may delay the animation update by one frame.

	{
	#if STATS || ENABLE_STATNAMEDEVENTS
		const bool bShouldTrackObject = Stats::IsThreadCollectingData();
		FScopeCycleCounterUObject ContextScope(bShouldTrackObject ? this : nullptr);
	#endif


		// Process animations for visible widgets
		for (auto WidgetIter = WeakUserWidgetData.CreateIterator(); WidgetIter; ++WidgetIter)
		{
			UUserWidget* UserWidget = WidgetIter.Key().Get();
			FSequenceTickManagerWidgetData& WidgetData = WidgetIter.Value();
			WidgetData.bActionsAndAnimationTicked = false;

			if (!UserWidget)
			{
				WidgetIter.RemoveCurrent();
			}
			else if (!UserWidget->IsConstructed())
			{
				UserWidget->TearDownAnimations();
				UserWidget->AnimationTickManager = nullptr;

				WidgetIter.RemoveCurrent();
			}
			else if (!WidgetData.bIsTicking)
			{
				// If this widget has not told us it is ticking, and its last known state was
				// ticking, we disable animations for that widget. Once it ticks again, the animation
				// will be updated naturally, and doesn't need anything re-enabling.
				// 
				// @todo: There is a chance that relative animations hitting this code path will resume with
				// different relative bases due to the way the ecs data is destroyed and re-created.
				// In order to fix this we would have to annex that data instead of destroying it.
				if (WidgetData.bLastKnownTickState)
				{
					UserWidget->DisableAnimations();
				}

				WidgetData.bLastKnownTickState = false;
			}
			else
			{
	#if STATS || ENABLE_STATNAMEDEVENTS
				FScopeCycleCounterUObject WidgetContextScope(bShouldTrackObject ? UserWidget : nullptr);
	#endif

	#if WITH_EDITOR
				const bool bTickAnimations = !UserWidget->IsDesignTime();
	#else
				const bool bTickAnimations = true;
	#endif
				if (bTickAnimations && UserWidget->IsVisible())
				{
					UserWidget->TickActionsAndAnimation(DeltaSeconds);
					WidgetData.bActionsAndAnimationTicked = true;
				}

				// Assume this widget will no longer tick, until we're told otherwise by way of OnWidgetTicked
				WidgetData.bIsTicking = false;
				WidgetData.bLastKnownTickState = true;
			}
		}
	}

	ForceFlush();

	for (auto WidgetIter = WeakUserWidgetData.CreateIterator(); WidgetIter; ++WidgetIter)
	{
		UUserWidget* UserWidget = WidgetIter.Key().Get();
		ensureMsgf(UserWidget, TEXT("Widget became null during animation tick!"));

		if (UserWidget)
		{
			if (WidgetIter.Value().bActionsAndAnimationTicked)
			{
				UserWidget->PostTickActionsAndAnimation(DeltaSeconds);
			}

			// If this widget no longer has any animations playing, it doesn't need to be ticked any more
			if (UserWidget->ActiveSequencePlayers.Num() == 0)
			{
				UserWidget->UpdateCanTick();
				UserWidget->AnimationTickManager = nullptr;
				WidgetIter.RemoveCurrent();
			}
		}
		else
		{
			WidgetIter.RemoveCurrent();
		}
	}
}

void UUMGSequenceTickManager::ForceFlush()
{
	if (Runner.IsAttachedToLinker())
	{
		Runner.Flush();
		LatentActionManager.RunLatentActions(Runner);
	}
}

void UUMGSequenceTickManager::HandleSlatePostTick(float DeltaSeconds)
{
	// Early out if inside a PostLoad
	if (FUObjectThreadContext::Get().IsRoutingPostLoad)
	{
		return;
	}

	if (GFlushUMGAnimationsAtEndOfFrame && Runner.IsAttachedToLinker() && Runner.HasQueuedUpdates())
	{
		SCOPE_CYCLE_COUNTER(MovieSceneEval_FlushEndOfFrameAnimations);

		Runner.Flush();
		LatentActionManager.RunLatentActions(Runner);
	}
}

void UUMGSequenceTickManager::AddLatentAction(FMovieSceneSequenceLatentActionDelegate Delegate)
{
	LatentActionManager.AddLatentAction(Delegate);
}

void UUMGSequenceTickManager::ClearLatentActions(UObject* Object)
{
	LatentActionManager.ClearLatentActions(Object);
}

void UUMGSequenceTickManager::RunLatentActions()
{
	LatentActionManager.RunLatentActions(Runner);
}

UUMGSequenceTickManager* UUMGSequenceTickManager::Get(UObject* PlaybackContext)
{
	const TCHAR* TickManagerName = TEXT("GlobalUMGSequenceTickManager");

	// The tick manager is owned by GEngine to ensure that it is kept alive for widgets that do not belong to
	// a world, but still require animations to be ticked. Ultimately this class could become an engine subsystem
	// but that would mean it is still around and active even if there are no animations playing, which is less
	// than ideal
	UObject* Owner = GEngine;
	if (!ensure(Owner))
	{
		// If (in the hopefully impossible event) there is no engine, use the previous method of a World as a fallback.
		// This will at least ensure we do not crash at the callsite due to a null tick manager
		check(PlaybackContext != nullptr && PlaybackContext->GetWorld() != nullptr);
		Owner = PlaybackContext->GetWorld();
	}

	UUMGSequenceTickManager* TickManager = FindObject<UUMGSequenceTickManager>(Owner, TickManagerName);
	if (!TickManager)
	{
		TickManager = NewObject<UUMGSequenceTickManager>(Owner, TickManagerName);

		TickManager->Linker = UMovieSceneEntitySystemLinker::FindOrCreateLinker(Owner, TEXT("UMGAnimationEntitySystemLinker"));
		check(TickManager->Linker);
		TickManager->Runner.AttachToLinker(TickManager->Linker);

		FSlateApplication& SlateApp = FSlateApplication::Get();
		FDelegateHandle PreTickHandle = SlateApp.OnPreTick().AddUObject(TickManager, &UUMGSequenceTickManager::TickWidgetAnimations);
		check(PreTickHandle.IsValid());
		TickManager->SlateApplicationPreTickHandle = PreTickHandle;

		FDelegateHandle PostTickHandle = SlateApp.OnPostTick().AddUObject(TickManager, &UUMGSequenceTickManager::HandleSlatePostTick);
		check(PostTickHandle.IsValid());
		TickManager->SlateApplicationPostTickHandle = PostTickHandle;
	}
	return TickManager;
}

