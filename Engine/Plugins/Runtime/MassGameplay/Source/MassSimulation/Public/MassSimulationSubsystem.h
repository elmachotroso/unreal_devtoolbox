// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Delegates/IDelegateInstance.h"
#include "MassProcessingTypes.h"
#include "MassProcessingPhase.h"
#include "Subsystems/WorldSubsystem.h"
#include "MassSimulationSubsystem.generated.h"


class AMassSimulationLocalCoordinator;
class UMassEntitySubsystem;
class UMassProcessingPhaseManager;

DECLARE_LOG_CATEGORY_EXTERN(LogMassSim, Log, All);

UCLASS(config = Game, defaultconfig)
class MASSSIMULATION_API UMassSimulationSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()
public:
	DECLARE_MULTICAST_DELEGATE_OneParam(FOnSimulationStarted, UWorld* /*World*/);
	
	UMassSimulationSubsystem(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	//UMassEntitySubsystem* GetEntitySubsystem() const { return CachedEntitySubsystem; }
	const UMassProcessingPhaseManager& GetPhaseManager() const { check(PhaseManager); return *PhaseManager; }

	FMassProcessingPhase::FOnPhaseEvent& GetOnProcessingPhaseStarted(const EMassProcessingPhase Phase) const;
	FMassProcessingPhase::FOnPhaseEvent& GetOnProcessingPhaseFinished(const EMassProcessingPhase Phase) const;
	static FOnSimulationStarted& GetOnSimulationStarted() { return OnSimulationStarted; }

	bool IsSimulationStarted() const { return bSimulationStarted; }

protected:
	virtual void PostInitProperties() override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void PostInitialize() override;
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;
	virtual void Deinitialize() override;
	virtual void BeginDestroy() override;

	void RebuildTickPipeline();

	void StartSimulation(UWorld& InWorld);
	void StopSimulation();

	void OnProcessingPhaseStarted(const float DeltaSeconds, const EMassProcessingPhase Phase) const;

#if WITH_EDITOR
	void OnPieBegin(const bool bIsSimulation);
	void OnPieEnded(const bool bIsSimulation);
#endif // WITH_EDITOR

protected:

	UPROPERTY()
	UMassEntitySubsystem* EntitySubsystem;

	UPROPERTY()
	UMassProcessingPhaseManager* PhaseManager;

	inline static FOnSimulationStarted OnSimulationStarted={};

	UPROPERTY()
	FMassRuntimePipeline RuntimePipeline;

	float CurrentDeltaSeconds = 0.f;
	bool bTickInProgress = false;
	bool bSimulationStarted = false;

#if WITH_EDITOR
	FDelegateHandle PieBeginEventHandle;
	FDelegateHandle PieEndedEventHandle;
#endif // WITH_EDITOR
};
