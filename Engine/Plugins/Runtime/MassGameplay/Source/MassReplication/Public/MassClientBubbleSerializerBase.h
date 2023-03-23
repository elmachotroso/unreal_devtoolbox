// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/NetSerialization.h"
#include "MassReplicationTypes.h"

#include "MassClientBubbleSerializerBase.generated.h"


class UWorld;
class UMassSpawnerSubsystem;
class UMassEntitySubsystem;
class IClientBubbleHandlerInterface;

/**
 * Classes derived from this will contain the IClientBubbleHandlerInterface derived class as well as the actual Fast Array.
 * This class mainly provides the base automation with the IClientBubbleHandlerInterface
 */
USTRUCT()
struct MASSREPLICATION_API FMassClientBubbleSerializerBase : public FFastArraySerializer
{
	GENERATED_BODY()

public:

#if UE_REPLICATION_COMPILE_CLIENT_CODE
	void PreReplicatedRemove(const TArrayView<int32> RemovedIndices, int32 FinalSize) const;
	void PostReplicatedAdd(const TArrayView<int32> AddedIndices, int32 FinalSize) const;
	void PostReplicatedChange(const TArrayView<int32> ChangedIndices, int32 FinalSize) const;
#endif //UE_REPLICATION_COMPILE_CLIENT_CODE

	void InitializeForWorld(UWorld& InWorld);

	UWorld* GetWorld() const { return World; }
	UMassSpawnerSubsystem* GetSpawnerSubsystem() const { return SpawnerSubsystem; }
	UMassReplicationSubsystem* GetReplicationSubsystem() const { return ReplicationSubsystem; }
	UMassEntitySubsystem* GetEntitySystem() const { return EntitySystem; }

	void SetClientHandler(IClientBubbleHandlerInterface& InClientHandler) { ClientHandler = &InClientHandler; }
	IClientBubbleHandlerInterface* GetClientHandler() const { return ClientHandler; }

private:
	UPROPERTY(Transient)
	UWorld* World = nullptr;

	UPROPERTY(Transient)
	UMassSpawnerSubsystem* SpawnerSubsystem = nullptr;

	UPROPERTY(Transient)
	UMassEntitySubsystem* EntitySystem = nullptr;

	UPROPERTY(Transient)
	UMassReplicationSubsystem* ReplicationSubsystem = nullptr;

	/** Pointer to the IClientBubbleHandlerInterface derived class in the class derived from this one */
	IClientBubbleHandlerInterface* ClientHandler = nullptr;
};
