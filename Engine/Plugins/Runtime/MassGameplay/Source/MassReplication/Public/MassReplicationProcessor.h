// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Engine/World.h"
#include "MassProcessor.h"
#include "MassCommonTypes.h"
#include "MassSimulationLOD.h"
#include "MassReplicationTypes.h"
#include "MassReplicationFragments.h"
#include "MassSpawnerTypes.h"
#include "MassLODSubsystem.h"
#include "MassReplicationSubsystem.h"
#include "MassReplicationProcessor.generated.h"

class UMassReplicationSubsystem;
class AMassClientBubbleInfoBase;
class UWorld;

/** 
 *  Base processor that handles replication and only runs on the server. You should derive from this per entity type (that require different replication processing). It and its derived classes 
 *  query Mass entity fragments and set those values for replication when appropriate, using the MassClientBubbleHandler.
 */
UCLASS()
class MASSREPLICATION_API UMassReplicationProcessor : public UMassLODProcessorBase
{
	GENERATED_BODY()

public:
	UMassReplicationProcessor();

protected:
	virtual void ConfigureQueries() override;
	virtual void Initialize(UObject& Owner) override;
	virtual void Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context) override;

	void PrepareExecution(UMassEntitySubsystem& EntitySubsystem);

protected:

	UPROPERTY(Transient)
	UMassReplicationSubsystem* ReplicationSubsystem = nullptr;

	FMassEntityQuery CollectViewerInfoQuery;
	FMassEntityQuery CalculateLODQuery;
	FMassEntityQuery AdjustLODDistancesQuery;
	FMassEntityQuery EntityQuery;
};


struct FMassReplicationContext
{
	FMassReplicationContext(UWorld& InWorld, UMassLODSubsystem& InLODSubsystem, UMassReplicationSubsystem& InReplicationSubsystem)
		: World(InWorld)
		, LODSubsystem(InLODSubsystem)
		, ReplicationSubsystem(InReplicationSubsystem)
	{}

	UWorld& World;
	UMassLODSubsystem& LODSubsystem;
	UMassReplicationSubsystem& ReplicationSubsystem;
};

UCLASS(Abstract)
class MASSREPLICATION_API UMassReplicatorBase : public UObject
{
	GENERATED_BODY()

public:
	/**
	 * Must override to add specific entity query requirements for replication
	 * Usually we add replication processor handler requirements
	 */
	virtual void AddRequirements(FMassEntityQuery& EntityQuery) PURE_VIRTUAL(UMassReplicatorBase::ConfigureQueries, );

	/**
	 * Must override to process the client replication
	 * This methods should call CalculateClientReplication with the appropriate callback implementation
	 */
	virtual void ProcessClientReplication(FMassExecutionContext& Context, FMassReplicationContext& ReplicationContext) PURE_VIRTUAL(UMassReplicatorBase::ProcessClientReplication, );

protected:
	/**
	 *  Implemented as straight template callbacks as when profiled this was faster than TFunctionRef. Its probably easier to pass Lamdas in to these
	 *  but Functors can also be used as well as TFunctionRefs etc. Its also fairly straight forward to call member functions via some Lamda glue code
	 */
	template<typename AgentArrayItem, typename CacheViewsCallback, typename AddEntityCallback, typename ModifyEntityCallback, typename RemoveEntityCallback>
	static void CalculateClientReplication(FMassExecutionContext& Context, FMassReplicationContext& ReplicationContext, CacheViewsCallback&& CacheViews, AddEntityCallback&& AddEntity, ModifyEntityCallback&& ModifyEntity, RemoveEntityCallback&& RemoveEntity);
};

template<typename AgentArrayItem, typename CacheViewsCallback, typename AddEntityCallback, typename ModifyEntityCallback, typename RemoveEntityCallback>
void UMassReplicatorBase::CalculateClientReplication(FMassExecutionContext& Context, FMassReplicationContext& ReplicationContext, CacheViewsCallback&& CacheViews, AddEntityCallback&& AddEntity, ModifyEntityCallback&& ModifyEntity, RemoveEntityCallback&& RemoveEntity)
{
#if UE_REPLICATION_COMPILE_SERVER_CODE

	const int32 NumEntities = Context.GetNumEntities();

	TConstArrayView<FMassNetworkIDFragment> NetworkIDList = Context.GetFragmentView<FMassNetworkIDFragment>();
	TArrayView<FMassReplicationLODFragment> ViewerLODList = Context.GetMutableFragmentView<FMassReplicationLODFragment>();
	TArrayView<FMassReplicatedAgentFragment> ReplicatedAgentList = Context.GetMutableFragmentView<FMassReplicatedAgentFragment>();
	TConstArrayView<FReplicationTemplateIDFragment> TemplateIDList = Context.GetFragmentView<FReplicationTemplateIDFragment>();
	FMassReplicationSharedFragment& RepSharedFragment = Context.GetMutableSharedFragment<FMassReplicationSharedFragment>();

	CacheViews(Context);

	const float Time = ReplicationContext.World.GetRealTimeSeconds();

	for (int EntityIdx = 0; EntityIdx < NumEntities; EntityIdx++)
	{
		FMassReplicatedAgentFragment& AgentFragment = ReplicatedAgentList[EntityIdx];

		if (AgentFragment.AgentsData.Num() < RepSharedFragment.CachedClientHandles.Num())
		{
			AgentFragment.AgentsData.AddDefaulted(RepSharedFragment.CachedClientHandles.Num() - AgentFragment.AgentsData.Num());
		}
		else if (AgentFragment.AgentsData.Num() > RepSharedFragment.CachedClientHandles.Num())
		{
			AgentFragment.AgentsData.RemoveAt(RepSharedFragment.CachedClientHandles.Num(), AgentFragment.AgentsData.Num() - RepSharedFragment.CachedClientHandles.Num(), /* bAllowShrinking */ false);
		}

		for (int32 ClientIdx = 0; ClientIdx < RepSharedFragment.CachedClientHandles.Num(); ++ClientIdx)
		{
			const FMassClientHandle& ClientHandle = RepSharedFragment.CachedClientHandles[ClientIdx];

			if (ClientHandle.IsValid())
			{
				checkSlow(RepSharedFragment.BubbleInfos[ClientHandle.GetIndex()] != nullptr);

				FMassReplicatedAgentArrayData& AgentData = AgentFragment.AgentsData[ClientIdx];

				//if the bubble has changed, set the handle Invalid. We will set this to something valid if the agent is going to replicate to the bubble
				//When a bubble has changed the existing AMassCrowdClientBubbleInfo will reset all the data associated with it.
				if (RepSharedFragment.bBubbleChanged[ClientIdx])
				{
					AgentData.Invalidate();
				}

				//now we need to see what our highest viewer LOD is on this client (split screen etc), we can use the unsafe version as we have already checked all the
				//CachedClientHandles for validity
				const FClientViewerHandles& ClientViewers = ReplicationContext.ReplicationSubsystem.GetClientViewersChecked(ClientHandle);

				EMassLOD::Type HighestLOD = EMassLOD::Off;

				for (const FMassViewerHandle& ViewerHandle : ClientViewers.Handles)
				{
					//this should always we valid as we synchronized the viewers just previously
					check(ReplicationContext.LODSubsystem.IsValidViewer(ViewerHandle));

					const EMassLOD::Type MassLOD = ViewerLODList[EntityIdx].LODPerViewer[ViewerHandle.GetIndex()];
					check(MassLOD <= EMassLOD::Off);

					if (HighestLOD > MassLOD)
					{
						HighestLOD = MassLOD;
					}
				}

				if (HighestLOD < EMassLOD::Off)
				{
#if UE_ALLOW_DEBUG_REPLICATION
					AgentData.LOD = HighestLOD;
#endif // UE_ALLOW_DEBUG_REPLICATION

					//if the handle isn't valid we need to add the agent
					if (!AgentData.Handle.IsValid())
					{
						typename AgentArrayItem::FReplicatedAgentType ReplicatedAgent;

						const FMassNetworkIDFragment& NetIDFragment = NetworkIDList[EntityIdx];
						const FReplicationTemplateIDFragment& TemplateIDFragment = TemplateIDList[EntityIdx];

						ReplicatedAgent.SetNetID(NetIDFragment.NetID);
						ReplicatedAgent.SetTemplateID(TemplateIDFragment.ID);

						AgentData.Handle = AddEntity(Context, EntityIdx, ReplicatedAgent, ClientHandle);

						AgentData.LastUpdateTime = Time;
					}
					else
					{
						ModifyEntity(Context, EntityIdx, HighestLOD, Time, AgentData.Handle, ClientHandle);
					}
				}
				else
				{
					// as this is a fresh handle, if its valid then we can use the unsafe remove function
					if (AgentData.Handle.IsValid())
					{
						RemoveEntity(Context, AgentData.Handle, ClientHandle);
						AgentData.Invalidate();
					}
				}
			}
		}
	}
#endif //UE_REPLICATION_COMPILE_SERVER_CODE
}
