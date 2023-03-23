// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "MassSignals/Public/MassSignalProcessorBase.h"
#include "MassStateTreeFragments.h"
#include "MassObserverProcessor.h"
#include "MassLODTypes.h"
#include "MassStateTreeProcessors.generated.h"

struct FMassStateTreeExecutionContext;

/** 
 * Processor to stop and uninitialize StateTrees on entities.
 */
UCLASS()
class MASSAIBEHAVIOR_API UMassStateTreeFragmentDestructor : public UMassObserverProcessor
{
	GENERATED_BODY()

public:
	UMassStateTreeFragmentDestructor();

protected:
	virtual void Initialize(UObject& Owner) override;
	virtual void ConfigureQueries() override;
	virtual void Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context) override;

	FMassEntityQuery EntityQuery;

	UPROPERTY(Transient)
	UMassSignalSubsystem* SignalSubsystem = nullptr;
};

/**
 * Special tag to know if the state tree has been activated
 */
USTRUCT()
struct MASSAIBEHAVIOR_API FMassStateTreeActivatedTag : public FMassTag
{
	GENERATED_BODY()
};
/**
 * Processor to send the activation signal to the state tree which will execute the first tick */
UCLASS()
class MASSAIBEHAVIOR_API UMassStateTreeActivationProcessor : public UMassProcessor
{
	GENERATED_BODY()
public:
	UMassStateTreeActivationProcessor();
protected:
	virtual void Initialize(UObject& Owner) override;
	virtual void ConfigureQueries() override;
	virtual void Execute(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context) override;

	UPROPERTY(Transient)
	UMassSignalSubsystem* SignalSubsystem = nullptr;
	
	FMassEntityQuery EntityQuery;

	/** The maximum number of entities to signal activation per LOD for each update, -1 means no limit */
	UPROPERTY(EditAnywhere, Category = "Mass|LOD", config)
	int32 MaxActivationsPerLOD[EMassLOD::Max];
};

/** 
 * Processor for executing a StateTree
 */
UCLASS()
class MASSAIBEHAVIOR_API UMassStateTreeProcessor : public UMassSignalProcessorBase
{
	GENERATED_BODY()

public:
	UMassStateTreeProcessor(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

protected:
	virtual void Initialize(UObject& Owner) override;
	virtual void ConfigureQueries() override;
	virtual void SignalEntities(UMassEntitySubsystem& EntitySubsystem, FMassExecutionContext& Context, FMassSignalNameLookup& EntitySignals) override;

	UPROPERTY(Transient)
	UMassStateTreeSubsystem* MassStateTreeSubsystem;
};
