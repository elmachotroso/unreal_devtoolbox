// Copyright Epic Games, Inc. All Rights Reserved.

#include "NiagaraSimulationStageBase.h"
#include "NiagaraEmitter.h"
#include "NiagaraSystem.h"
#include "NiagaraScriptSourceBase.h"

const FName UNiagaraSimulationStageBase::ParticleSpawnUpdateName("ParticleSpawnUpdate");

bool UNiagaraSimulationStageBase::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const
{
#if WITH_EDITORONLY_DATA
	const int32 Index = InVisitor->Values.AddDefaulted();
	InVisitor->Values[Index].Object = FString::Printf(TEXT("Class: \"%s\"  Name: \"%s\""), *GetClass()->GetName(), *GetName());
#endif
	InVisitor->UpdatePOD(TEXT("Enabled"), bEnabled ? 1 : 0);
	return true;
}

#if WITH_EDITOR
void UNiagaraSimulationStageBase::SetEnabled(bool bInEnabled)
{
	if (bEnabled != bInEnabled)
	{
		bEnabled = bInEnabled;
		RequestRecompile();
	}
}

void UNiagaraSimulationStageBase::RequestRecompile()
{
	UNiagaraEmitter* Emitter = Cast< UNiagaraEmitter>(GetOuter());
	if (Emitter)
	{
		UNiagaraScriptSourceBase* GraphSource = Emitter->UpdateScriptProps.Script->GetLatestSource();
		if (GraphSource != nullptr)
		{
			GraphSource->MarkNotSynchronized(TEXT("SimulationStage changed."));
		}

		UNiagaraSystem::RequestCompileForEmitter(Emitter);
	}
}

void UNiagaraSimulationStageBase::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FName PropertyName;
	if (PropertyChangedEvent.Property)
	{
		PropertyName = PropertyChangedEvent.Property->GetFName();
	}

	if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageBase, bEnabled))
	{
		RequestRecompile();
	}
}
#endif

bool UNiagaraSimulationStageGeneric::AppendCompileHash(FNiagaraCompileHashVisitor* InVisitor) const 
{
	Super::AppendCompileHash(InVisitor);

	InVisitor->UpdateString(TEXT("EnabledBinding"), EnabledBinding.GetDataSetBindableVariable().GetName().ToString());
	InVisitor->UpdatePOD(TEXT("Iterations"), Iterations);
	InVisitor->UpdateString(TEXT("NumIterationsBinding"), NumIterationsBinding.GetDataSetBindableVariable().GetName().ToString());
	InVisitor->UpdatePOD(TEXT("IterationSource"), (int32)IterationSource);
	InVisitor->UpdatePOD(TEXT("ExecuteBehavior"), (int32)ExecuteBehavior);
	InVisitor->UpdatePOD(TEXT("bDisablePartialParticleUpdate"), bDisablePartialParticleUpdate ? 1 : 0);
	InVisitor->UpdateString(TEXT("DataInterface"), DataInterface.BoundVariable.GetName().ToString());
	InVisitor->UpdateString(TEXT("SimulationStageName"), SimulationStageName.ToString());
	InVisitor->UpdatePOD(TEXT("bParticleIterationStateEnabled"), bParticleIterationStateEnabled ? 1 : 0);
	InVisitor->UpdateString(TEXT("ParticleIterationStateBinding"), ParticleIterationStateBinding.GetDataSetBindableVariable().GetName().ToString());
	InVisitor->UpdateString(TEXT("ParticleIterationStateRange"), FString::Printf(TEXT("%d,%d"), ParticleIterationStateRange.X, ParticleIterationStateRange.Y));
	InVisitor->UpdatePOD(TEXT("bGpuDispatchForceLinear"), bGpuDispatchForceLinear ? 1 : 0);
	InVisitor->UpdatePOD(TEXT("bOverrideGpuDispatchNumThreads"), bOverrideGpuDispatchNumThreads ? 1 : 0);
	InVisitor->UpdateString(TEXT("OverrideGpuDispatchNumThreads"), FString::Printf(TEXT("%d,%d,%d"), OverrideGpuDispatchNumThreads.X, OverrideGpuDispatchNumThreads.Y, OverrideGpuDispatchNumThreads.Z));

	return true;
}

void UNiagaraSimulationStageGeneric::PostInitProperties()
{
	Super::PostInitProperties();

	if (HasAnyFlags(RF_ClassDefaultObject) == false)
	{
		EnabledBinding.Setup(
			FNiagaraVariableBase(FNiagaraTypeDefinition::GetBoolDef(), NAME_None),
			FNiagaraVariableBase(FNiagaraTypeDefinition::GetBoolDef(), NAME_None),
			ENiagaraRendererSourceDataMode::Emitter
		);

		NumIterationsBinding.Setup(
			FNiagaraVariableBase(FNiagaraTypeDefinition::GetIntDef(), NAME_None),
			FNiagaraVariableBase(FNiagaraTypeDefinition::GetIntDef(), NAME_None),
			ENiagaraRendererSourceDataMode::Emitter
		);

		static const FName ParticleStateIndex("Particles.StateIndex");
		ParticleIterationStateBinding.Setup(
			FNiagaraVariableBase(FNiagaraTypeDefinition::GetIntDef(), ParticleStateIndex),
			FNiagaraVariableBase(FNiagaraTypeDefinition::GetIntDef(), ParticleStateIndex),
			ENiagaraRendererSourceDataMode::Particles
		);
	}
}

#if WITH_EDITOR
void UNiagaraSimulationStageGeneric::PostEditChangeProperty(struct FPropertyChangedEvent& PropertyChangedEvent) 
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	FName PropertyName;
	FName MemberPropertyName;
	if (PropertyChangedEvent.Property)
	{
		PropertyName = PropertyChangedEvent.Property->GetFName();
	}
	if (PropertyChangedEvent.MemberProperty)
	{
		MemberPropertyName = PropertyChangedEvent.MemberProperty->GetFName();
	}

	bool bNeedsRecompile = false;
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, EnabledBinding))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, Iterations))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, NumIterationsBinding))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, IterationSource))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, ExecuteBehavior))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, bDisablePartialParticleUpdate))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, DataInterface))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, SimulationStageName))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, bParticleIterationStateEnabled))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, ParticleIterationStateBinding))
	{
		bNeedsRecompile = true;
	}
	else if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, ParticleIterationStateRange))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, bGpuDispatchForceLinear))
	{
		bNeedsRecompile = true;
	}
	else if (PropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, bOverrideGpuDispatchNumThreads))
	{
		bNeedsRecompile = true;
	}
	else if (MemberPropertyName == GET_MEMBER_NAME_CHECKED(UNiagaraSimulationStageGeneric, OverrideGpuDispatchNumThreads))
	{
		OverrideGpuDispatchNumThreads.X = FMath::Max(OverrideGpuDispatchNumThreads.X, 1);
		OverrideGpuDispatchNumThreads.Y = FMath::Max(OverrideGpuDispatchNumThreads.Y, 1);
		OverrideGpuDispatchNumThreads.Z = FMath::Max(OverrideGpuDispatchNumThreads.Z, 1);
		bNeedsRecompile = true;
	}

	if (bNeedsRecompile)
	{
		RequestRecompile();
	}
}

FName UNiagaraSimulationStageGeneric::GetStackContextReplacementName() const 
{
	if (IterationSource == ENiagaraIterationSource::Particles)
		return NAME_None;
	else if (IterationSource == ENiagaraIterationSource::DataInterface)
		return DataInterface.BoundVariable.GetName();
	ensureMsgf(false, TEXT("Should not get here! Need to handle unknown case!"));
	return NAME_None;
}
#endif
