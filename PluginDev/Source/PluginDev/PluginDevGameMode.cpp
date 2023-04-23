// Copyright Epic Games, Inc. All Rights Reserved.

#include "PluginDevGameMode.h"
#include "PluginDevCharacter.h"
#include "UObject/ConstructorHelpers.h"

APluginDevGameMode::APluginDevGameMode()
{
	// set default pawn class to our Blueprinted character
	static ConstructorHelpers::FClassFinder<APawn> PlayerPawnBPClass(TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (PlayerPawnBPClass.Class != NULL)
	{
		DefaultPawnClass = PlayerPawnBPClass.Class;
	}
}
