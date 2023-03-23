// Copyright Epic Games, Inc. All Rights Reserved.

#include "Engine/CancellableAsyncAction.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "TimerManager.h"

void UCancellableAsyncAction::Cancel()
{
	// Child classes should override this
	SetReadyToDestroy();
}

bool UCancellableAsyncAction::IsActive() const
{
	return ShouldBroadcastDelegates();
}

bool UCancellableAsyncAction::ShouldBroadcastDelegates() const
{
	return IsRegistered();
}

bool UCancellableAsyncAction::IsRegistered() const
{
	return RegisteredWithGameInstance.IsValid();
}

class FTimerManager* UCancellableAsyncAction::GetTimerManager() const
{
	if (RegisteredWithGameInstance.IsValid())
	{
		return &RegisteredWithGameInstance->GetTimerManager();
	}

	return nullptr;
}
