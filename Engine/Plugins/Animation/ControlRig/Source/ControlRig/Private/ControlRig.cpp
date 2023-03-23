// Copyright Epic Games, Inc. All Rights Reserved.

#include "ControlRig.h"
#include "Engine/SkeletalMesh.h"
#include "GameFramework/Actor.h"
#include "Misc/RuntimeErrors.h"
#include "IControlRigObjectBinding.h"
#include "HelperUtil.h"
#include "ObjectTrace.h"
#include "ControlRigBlueprintGeneratedClass.h"
#include "ControlRigObjectVersion.h"
#include "Rigs/RigHierarchyController.h"
#include "Units/Execution/RigUnit_BeginExecution.h"
#include "Units/Execution/RigUnit_PrepareForExecution.h"
#include "Units/Execution/RigUnit_InverseExecution.h"
#include "Components/SkeletalMeshComponent.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimationPoseData.h"
#if WITH_EDITOR
#include "ControlRigModule.h"
#include "Modules/ModuleManager.h"
#include "RigVMModel/RigVMNode.h"
#include "Engine/Blueprint.h"
#include "EdGraphSchema_K2.h"
#include "Kismet2/BlueprintEditorUtils.h"
#endif// WITH_EDITOR
#include "ControlRigComponent.h"

#define LOCTEXT_NAMESPACE "ControlRig"

DEFINE_LOG_CATEGORY(LogControlRig);

DECLARE_STATS_GROUP(TEXT("ControlRig"), STATGROUP_ControlRig, STATCAT_Advanced);
DECLARE_CYCLE_STAT_EXTERN(TEXT("Control Rig Execution"), STAT_RigExecution, STATGROUP_ControlRig, );
DEFINE_STAT(STAT_RigExecution);

const FName UControlRig::OwnerComponent("OwnerComponent");

//CVar to specify if we should create a float control for each curve in the curve container
//By default we don't but it may be useful to do so for debugging
static TAutoConsoleVariable<int32> CVarControlRigCreateFloatControlsForCurves(
	TEXT("ControlRig.CreateFloatControlsForCurves"),
	0,
	TEXT("If nonzero we create a float control for each curve in the curve container, useful for debugging low level controls."),
	ECVF_Default);

// CVar to disable all control rig execution 
static TAutoConsoleVariable<int32> CVarControlRigDisableExecutionAll(TEXT("ControlRig.DisableExecutionAll"), 0, TEXT("if nonzero we disable all execution of Control Rigs."));

UControlRig::UControlRig(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
	, DeltaTime(0.0f)
	, AbsoluteTime(0.0f)
	, FramesPerSecond(0.0f)
	, bAccumulateTime(true)
	, LatestExecutedState(EControlRigState::Invalid)
#if WITH_EDITOR
	, ControlRigLog(nullptr)
	, bEnableControlRigLogging(true)
#endif
	, DataSourceRegistry(nullptr)
	, EventQueue()
#if WITH_EDITOR
	, PreviewInstance(nullptr)
#endif
	, bRequiresInitExecution(false)
	, bRequiresSetupEvent(false)
	, bSetupModeEnabled(false)
	, bCopyHierarchyBeforeSetup(true)
	, bResetInitialTransformsBeforeSetup(true)
	, bManipulationEnabled(false)
	, InitBracket(0)
	, UpdateBracket(0)
	, PreSetupBracket(0)
	, PostSetupBracket(0)
	, InteractionBracket(0)
	, InterRigSyncBracket(0)
#if WITH_EDITORONLY_DATA
	, VMSnapshotBeforeExecution(nullptr)
#endif
	, DebugBoneRadiusMultiplier(1.f)
{
	EventQueue.Add(FRigUnit_BeginExecution::EventName);
}

void UControlRig::BeginDestroy()
{
	Super::BeginDestroy();
	InitializedEvent.Clear();
	PreSetupEvent.Clear();
	PostSetupEvent.Clear();
	PreForwardsSolveEvent.Clear();
	PostForwardsSolveEvent.Clear();
	ExecutedEvent.Clear();
	SetInteractionRig(nullptr);

	if (VM)
	{
		VM->ExecutionReachedExit().RemoveAll(this);
	}

#if WITH_EDITOR
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		if(UControlRig* CDO = GetClass()->GetDefaultObject<UControlRig>())
		{
			if (!CDO->HasAnyFlags(RF_BeginDestroyed))
			{
				if (CDO->GetHierarchy())
				{
					CDO->GetHierarchy()->UnregisterListeningHierarchy(GetHierarchy());
				}
			}
		}
	}
#endif

#if WITH_EDITORONLY_DATA
	if (VMSnapshotBeforeExecution)
	{
		VMSnapshotBeforeExecution = nullptr;
	}
#endif

	TRACE_OBJECT_LIFETIME_END(this);
}

UWorld* UControlRig::GetWorld() const
{
	if (ObjectBinding.IsValid())
	{
		AActor* HostingActor = ObjectBinding->GetHostingActor();
		if (HostingActor)
		{
			return HostingActor->GetWorld();
		}

		UObject* Owner = ObjectBinding->GetBoundObject();
		if (Owner)
		{
			return Owner->GetWorld();
		}
	}

	UObject* Outer = GetOuter();
	if (Outer)
	{
		return Outer->GetWorld();
	}

	return nullptr;
}

void UControlRig::Initialize(bool bInitRigUnits)
{
	TRACE_OBJECT_LIFETIME_BEGIN(this);

	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()
	QUICK_SCOPE_CYCLE_COUNTER(STAT_ControlRig_Initialize);

	if(IsInitializing())
	{
		UE_LOG(LogControlRig, Warning, TEXT("%s: Initialize is being called recursively."), *GetPathName());
		return;
	}

	if (IsTemplate())
	{
		// don't initialize template class 
		return;
	}

	InitializeFromCDO();
	InstantiateVMFromCDO();

	// Create the data source registry here to avoid UObject creation from Non-Game Threads
	GetDataSourceRegistry();

	// Create the Hierarchy Controller here to avoid UObject creation from Non-Game Threads
	GetHierarchy()->GetController(true);
	
	// should refresh mapping 
	RequestSetup();

	if (bInitRigUnits)
	{
		RequestInit();
	}
	
	GetHierarchy()->OnModified().RemoveAll(this);
	GetHierarchy()->OnModified().AddUObject(this, &UControlRig::HandleHierarchyModified);
	GetHierarchy()->OnEventReceived().RemoveAll(this);
	GetHierarchy()->OnEventReceived().AddUObject(this, &UControlRig::HandleHierarchyEvent);
}

void UControlRig::InitializeFromCDO()
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()

	// copy CDO property you need to here
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		// similar to FControlRigBlueprintCompilerContext::CopyTermDefaultsToDefaultObject,
		// where CDO is initialized from BP there,
		// we initialize all other instances of Control Rig from the CDO here
		UControlRig* CDO = GetClass()->GetDefaultObject<UControlRig>();

		// copy hierarchy
		{
			PostInitInstanceIfRequired();

			FRigHierarchyValidityBracket ValidityBracketA(GetHierarchy());
			FRigHierarchyValidityBracket ValidityBracketB(CDO->GetHierarchy());
			
			TGuardValue<bool> Guard(GetHierarchy()->GetSuspendNotificationsFlag(), true);
			GetHierarchy()->CopyHierarchy(CDO->GetHierarchy());
			GetHierarchy()->ResetPoseToInitial(ERigElementType::All);
		}

#if WITH_EDITOR
		// current hierarchy should always mirror CDO's hierarchy whenever a change of interest happens
		CDO->GetHierarchy()->RegisterListeningHierarchy(GetHierarchy());
#endif

		// notify clients that the hierarchy has changed
		GetHierarchy()->Notify(ERigHierarchyNotification::HierarchyReset, nullptr);

		// copy draw container
		DrawContainer = CDO->DrawContainer;

		// copy vm settings
		VMRuntimeSettings = CDO->VMRuntimeSettings;
	}
}

void UControlRig::Evaluate_AnyThread()
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()
	QUICK_SCOPE_CYCLE_COUNTER(STAT_ControlRig_Evaluate);

	for (const FName& EventName : EventQueue)
	{
		Execute(EControlRigState::Update, EventName);

#if WITH_EDITOR
		if (VM)
		{
			if (VM->GetHaltedAtBreakpoint().IsValid())
			{
				break;
			}
		}
#endif
			
	}
}


TArray<FRigVMExternalVariable> UControlRig::GetExternalVariables() const
{
	return GetExternalVariablesImpl(true);
}

TArray<FRigVMExternalVariable> UControlRig::GetExternalVariablesImpl(bool bFallbackToBlueprint) const
{
	TArray<FRigVMExternalVariable> ExternalVariables;

	for (TFieldIterator<FProperty> PropertyIt(GetClass()); PropertyIt; ++PropertyIt)
	{
		FProperty* Property = *PropertyIt;
		if(Property->IsNative())
		{
			continue;
		}

		FRigVMExternalVariable ExternalVariable = FRigVMExternalVariable::Make(Property, (UObject*)this);
		if(!ExternalVariable.IsValid())
		{
			UE_LOG(LogControlRig, Warning, TEXT("%s: Property '%s' of type '%s' is not supported."), *GetClass()->GetName(), *Property->GetName(), *Property->GetCPPType());
			continue;
		}

		ExternalVariables.Add(ExternalVariable);
	}

#if WITH_EDITOR

	if (bFallbackToBlueprint)
	{
		// if we have a difference in the blueprint variables compared to us - let's 
		// use those instead. the assumption here is that the blueprint is dirty and
		// hasn't been compiled yet.
		if (UBlueprint* Blueprint = Cast<UBlueprint>(GetClass()->ClassGeneratedBy))
		{
			TArray<FRigVMExternalVariable> BlueprintVariables;
			for (const FBPVariableDescription& VariableDescription : Blueprint->NewVariables)
			{
				FRigVMExternalVariable ExternalVariable = RigVMTypeUtils::ExternalVariableFromBPVariableDescription(VariableDescription);
				if (ExternalVariable.TypeName.IsNone())
				{
					continue;
				}

				ExternalVariable.Memory = nullptr;

				BlueprintVariables.Add(ExternalVariable);
			}

			if (ExternalVariables.Num() != BlueprintVariables.Num())
			{
				return BlueprintVariables;
			}

			TMap<FName, int32> NameMap;
			for (int32 Index = 0; Index < ExternalVariables.Num(); Index++)
			{
				NameMap.Add(ExternalVariables[Index].Name, Index);
			}

			for (FRigVMExternalVariable BlueprintVariable : BlueprintVariables)
			{
				const int32* Index = NameMap.Find(BlueprintVariable.Name);
				if (Index == nullptr)
				{
					return BlueprintVariables;
				}

				FRigVMExternalVariable ExternalVariable = ExternalVariables[*Index];
				if (ExternalVariable.bIsArray != BlueprintVariable.bIsArray ||
					ExternalVariable.bIsPublic != BlueprintVariable.bIsPublic ||
					ExternalVariable.TypeName != BlueprintVariable.TypeName ||
					ExternalVariable.TypeObject != BlueprintVariable.TypeObject)
				{
					return BlueprintVariables;
				}
			}
		}
	}
#endif

	return ExternalVariables;
}

TArray<FRigVMExternalVariable> UControlRig::GetPublicVariables() const
{
	TArray<FRigVMExternalVariable> PublicVariables;
	TArray<FRigVMExternalVariable> ExternalVariables = GetExternalVariables();
	for (const FRigVMExternalVariable& ExternalVariable : ExternalVariables)
	{
		if (ExternalVariable.bIsPublic)
		{
			PublicVariables.Add(ExternalVariable);
		}
	}
	return PublicVariables;
}

FRigVMExternalVariable UControlRig::GetPublicVariableByName(const FName& InVariableName) const
{
	if (FProperty* Property = GetPublicVariableProperty(InVariableName))
	{
		return FRigVMExternalVariable::Make(Property, (UObject*)this);
	}
	return FRigVMExternalVariable();
}

TArray<FName> UControlRig::GetScriptAccessibleVariables() const
{
	TArray<FRigVMExternalVariable> PublicVariables = GetPublicVariables();
	TArray<FName> Names;
	for (const FRigVMExternalVariable& PublicVariable : PublicVariables)
	{
		Names.Add(PublicVariable.Name);
	}
	return Names;
}

FName UControlRig::GetVariableType(const FName& InVariableName) const
{
	FRigVMExternalVariable PublicVariable = GetPublicVariableByName(InVariableName);
	if (PublicVariable.IsValid(true /* allow nullptr */))
	{
		return PublicVariable.TypeName;
	}
	return NAME_None;
}

FString UControlRig::GetVariableAsString(const FName& InVariableName) const
{
#if WITH_EDITOR
	if (const FProperty* Property = GetClass()->FindPropertyByName(InVariableName))
	{
		FString Result;
		const uint8* Container = (const uint8*)this;
		if (FBlueprintEditorUtils::PropertyValueToString(Property, Container, Result, nullptr))
		{
			return Result;
		}
	}
#endif
	return FString();
}

bool UControlRig::SetVariableFromString(const FName& InVariableName, const FString& InValue)
{
#if WITH_EDITOR
	if (const FProperty* Property = GetClass()->FindPropertyByName(InVariableName))
	{
		uint8* Container = (uint8*)this;
		return FBlueprintEditorUtils::PropertyValueFromString(Property, InValue, Container, nullptr);
	}
#endif
	return false;
}

bool UControlRig::SupportsEvent(const FName& InEventName) const
{
	if (VM)
	{
		return VM->ContainsEntry(InEventName);
	}
	return false;
}

TArray<FName> UControlRig::GetSupportedEvents() const
{
	if (VM)
	{
		return VM->GetEntryNames();
	}
	return TArray<FName>();
}

#if WITH_EDITOR
FText UControlRig::GetCategory() const
{
	return LOCTEXT("DefaultControlRigCategory", "Animation|ControlRigs");
}

FText UControlRig::GetToolTipText() const
{
	return LOCTEXT("DefaultControlRigTooltip", "ControlRig");
}
#endif

void UControlRig::SetDeltaTime(float InDeltaTime)
{
	DeltaTime = InDeltaTime;
}

void UControlRig::SetAbsoluteTime(float InAbsoluteTime, bool InSetDeltaTimeZero)
{
	if(InSetDeltaTimeZero)
	{
		DeltaTime = 0.f;
	}
	AbsoluteTime = InAbsoluteTime;
	bAccumulateTime = false;
}

void UControlRig::SetAbsoluteAndDeltaTime(float InAbsoluteTime, float InDeltaTime)
{
	AbsoluteTime = InAbsoluteTime;
	DeltaTime = InDeltaTime;
}

void UControlRig::SetFramesPerSecond(float InFramesPerSecond)
{
	FramesPerSecond = InFramesPerSecond;	
}

float UControlRig::GetCurrentFramesPerSecond() const
{
	if(FramesPerSecond > SMALL_NUMBER)
	{
		return FramesPerSecond;
	}
	if(DeltaTime > SMALL_NUMBER)
	{
		return 1.f / DeltaTime;
	}
	return 60.f;
}

void UControlRig::InstantiateVMFromCDO()
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()

	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		UControlRig* CDO = GetClass()->GetDefaultObject<UControlRig>();
		if (VM && CDO && CDO->VM)
		{
			// reference the literal memory + byte code
			// only defer if called from worker thread,
			// which should be unlikely
			VM->CopyFrom(CDO->VM, !IsInGameThread(), true);
		}
		else if (VM)
		{
			VM->Reset();
		}
		else
		{
			ensure(false);
		}
	}

	RequestInit();
}

void UControlRig::CopyExternalVariableDefaultValuesFromCDO()
{
	if (!HasAnyFlags(RF_ClassDefaultObject))
	{
		UControlRig* CDO = GetClass()->GetDefaultObject<UControlRig>();
		TArray<FRigVMExternalVariable> CurrentVariables = GetExternalVariablesImpl(false);
		TArray<FRigVMExternalVariable> CDOVariables = CDO->GetExternalVariablesImpl(false);
		if (ensure(CurrentVariables.Num() == CDOVariables.Num()))
		{
			for (int32 i=0; i<CurrentVariables.Num(); ++i)
			{
				FRigVMExternalVariable& Variable = CurrentVariables[i];
				FRigVMExternalVariable& CDOVariable = CDOVariables[i];
				Variable.Property->CopyCompleteValue(Variable.Memory, CDOVariable.Memory);
			}
		}
	}
}

void UControlRig::Execute(const EControlRigState InState, const FName& InEventName)
{
	if(!CanExecute())
	{
		return;
	}

	ensure(!HasAnyFlags(RF_ClassDefaultObject));
	
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()
	QUICK_SCOPE_CYCLE_COUNTER(STAT_ControlRig_Execute);
	
	LatestExecutedState = InState;

	if (VM)
	{
		if (VM->GetOuter() != this)
		{
			InstantiateVMFromCDO();
		}

		if (InState == EControlRigState::Init)
		{
			VM->ClearExternalVariables();

			TArray<FRigVMExternalVariable> ExternalVariables = GetExternalVariablesImpl(false);
			for (FRigVMExternalVariable ExternalVariable : ExternalVariables)
			{
				VM->AddExternalVariable(ExternalVariable);
			}
			
#if WITH_EDITOR
			// setup the hierarchy's controller log function
			if(URigHierarchyController* HierarchyController = GetHierarchy()->GetController(true))
			{
				HierarchyController->LogFunction = [this](EMessageSeverity::Type InSeverity, const FString& Message)
				{
					const FRigVMExecuteContext& Context = GetVM()->GetContext();
					if(ControlRigLog)
					{
						ControlRigLog->Report(InSeverity, Context.FunctionName, Context.InstructionIndex, Message);
					}
					else
					{
						LogOnce(InSeverity, Context.InstructionIndex, Message);
					}
				};
			}
#endif
		}
#if WITH_EDITOR
		// default to always clear data after each execution
		// only set a valid first entry event later when execution
		// has passed the initialization stage and there are multiple events present in one evaluation
		// first entry event is used to determined when to clear data during an evaluation
		VM->SetFirstEntryEventInEventQueue(NAME_None);
#endif
	}

#if WITH_EDITOR
	if (bIsInDebugMode)
	{
		if (UControlRig* CDO = GetClass()->GetDefaultObject<UControlRig>())
		{
			// Copy the breakpoints. This will not override the state of the breakpoints
			DebugInfo.SetBreakpoints(CDO->DebugInfo.GetBreakpoints());

			// If there are any breakpoints, create the Snapshot VM if it hasn't been created yet
			if (DebugInfo.GetBreakpoints().Num() > 0)
			{
				GetSnapshotVM();
			}
		}

		if(VM)
		{
			VM->SetDebugInfo(&DebugInfo);
		}
	}
	else if(VM)
	{
		VM->SetDebugInfo(nullptr);
	}
#endif

	bool bJustRanInit = false;
	if (bRequiresInitExecution)
	{
		bRequiresInitExecution = false;

		if (InState != EControlRigState::Init)
		{
			Execute(EControlRigState::Init, InEventName);
			bJustRanInit = true;
		}
	}

	FRigUnitContext Context;
	DrawInterface.Reset();
	Context.DrawInterface = &DrawInterface;

	// draw container contains persistent draw instructions, 
	// so we cannot call Reset(), which will clear them,
	// instead, we re-initialize them from the CDO
	if (UControlRig* CDO = GetClass()->GetDefaultObject<UControlRig>())
	{
		DrawContainer = CDO->DrawContainer;
	}

	Context.DrawContainer = &DrawContainer;
	Context.DataSourceRegistry = GetDataSourceRegistry();

	if (InState == EControlRigState::Init)
	{
		AbsoluteTime = DeltaTime = 0.f;
		NameCache.Reset();
	}

	Context.DeltaTime = DeltaTime;
	Context.AbsoluteTime = AbsoluteTime;
	Context.FramesPerSecond = GetCurrentFramesPerSecond();
	Context.bDuringInteraction = IsInteracting();
	Context.State = InState;
	Context.Hierarchy = GetHierarchy();

	Context.ToWorldSpaceTransform = FTransform::Identity;
	Context.OwningComponent = nullptr;
	Context.OwningActor = nullptr;
	Context.World = nullptr;
	Context.NameCache = &NameCache;

	if (!OuterSceneComponent.IsValid())
	{
		USceneComponent* SceneComponentFromRegistry = Context.DataSourceRegistry->RequestSource<USceneComponent>(UControlRig::OwnerComponent);
		if (SceneComponentFromRegistry)
		{
			OuterSceneComponent = SceneComponentFromRegistry;
		}
		else
		{
			UObject* Parent = this;
			while (Parent)
			{
				Parent = Parent->GetOuter();
				if (Parent)
				{
					if (USceneComponent* SceneComponent = Cast<USceneComponent>(Parent))
					{
						OuterSceneComponent = SceneComponent;
						break;
					}
				}
			}
		}
	}

	if (OuterSceneComponent.IsValid())
	{
		Context.ToWorldSpaceTransform = OuterSceneComponent->GetComponentToWorld();
		Context.OwningComponent = OuterSceneComponent.Get();
		Context.OwningActor = Context.OwningComponent->GetOwner();
		Context.World = Context.OwningComponent->GetWorld();
	}
	else
	{
		if (ObjectBinding.IsValid())
		{
			AActor* HostingActor = ObjectBinding->GetHostingActor();
			if (HostingActor)
			{
				Context.OwningActor = HostingActor;
				Context.World = HostingActor->GetWorld();
			}
			else if (UObject* Owner = ObjectBinding->GetBoundObject())
			{
				Context.World = Owner->GetWorld();
			}
		}

		if (Context.World == nullptr)
		{
			if (UObject* Outer = GetOuter())
			{
				Context.World = Outer->GetWorld();
			}
		}
	}

	if(GetHierarchy())
	{
		// if we have any referenced elements dirty them
		GetHierarchy()->UpdateReferences(&Context);
	}

#if WITH_EDITOR
	Context.Log = ControlRigLog;
	if (ControlRigLog != nullptr)
	{
		ControlRigLog->Reset();
		UpdateVMSettings();
	}
#endif

	// execute units
	if (bRequiresSetupEvent && InState != EControlRigState::Init)
	{
		if(!IsRunningPreSetup() && !IsRunningPostSetup())
		{
			bRequiresSetupEvent = bSetupModeEnabled;
			{
				// save the current state of all pose elements to preserve user intention, since setup event can
				// run in between forward events
				// the saved pose is reapplied to the rig after setup event as the pose scope goes out of scope
				TUniquePtr<UControlRig::FPoseScope> PoseScope;
				if (!bSetupModeEnabled)
				{
					// only do this in non-setup mode because 
					// when setup mode is enabled, the control values are cleared before reaching here (too late to save them)
					PoseScope = MakeUnique<UControlRig::FPoseScope>(this, ERigElementType::ToResetAfterSetupEvent);
				}
				
				if (UControlRig* CDO = Cast<UControlRig>(GetClass()->GetDefaultObject()))
				{
					if(bCopyHierarchyBeforeSetup && !bSetupModeEnabled)
					{
						if(CDO->GetHierarchy()->GetTopologyVersion()!= GetHierarchy()->GetTopologyVersion())
						{
#if WITH_EDITOR
							FTransientControlScope TransientControlScope(GetHierarchy());
#endif
							GetHierarchy()->CopyHierarchy(CDO->GetHierarchy());
						}
					}
					
					if (bResetInitialTransformsBeforeSetup && !bSetupModeEnabled)
					{
						GetHierarchy()->CopyPose(CDO->GetHierarchy(), false, true);
					}
				}

				{
#if WITH_EDITOR
					TUniquePtr<FTransientControlPoseScope> TransientControlPoseScope;
					if (bSetupModeEnabled)
					{
						// save the transient control value, it should not be constantly reset in setup mode
						TransientControlPoseScope = MakeUnique<FTransientControlPoseScope>(this);
					}
#endif
					// reset the pose to initial such that setup event can run from a deterministic initial state
					GetHierarchy()->ResetPoseToInitial(ERigElementType::All);
				}

				if (PreSetupEvent.IsBound())
				{
					FControlRigBracketScope BracketScope(PreSetupBracket);
					PreSetupEvent.Broadcast(this, EControlRigState::Update, FRigUnit_PrepareForExecution::EventName);
				}

				ExecuteUnits(Context, FRigUnit_PrepareForExecution::EventName);

				if (PostSetupEvent.IsBound())
				{
					FControlRigBracketScope BracketScope(PostSetupBracket);
					PostSetupEvent.Broadcast(this, EControlRigState::Update, FRigUnit_PrepareForExecution::EventName);
				}
			}

			if (bSetupModeEnabled)
			{
#if WITH_EDITOR
				TUniquePtr<FTransientControlPoseScope> TransientControlPoseScope;
				if (bSetupModeEnabled)
				{
					// save the transient control value, it should not be constantly reset in setup mode
					TransientControlPoseScope = MakeUnique<FTransientControlPoseScope>(this);
				}
#endif
				GetHierarchy()->ResetPoseToInitial(ERigElementType::Bone);
			}			
		}
		else
		{
			UE_LOG(LogControlRig, Warning, TEXT("%s: Setup is being called recursively."), *GetPathName());
		}
	}

	if (!bSetupModeEnabled)
	{
		if(!IsExecuting())
		{ 

#if WITH_EDITOR
			// only set a valid first entry event when execution
			// has passed the initialization stage and there are multiple events present
			if (EventQueue.Num() >= 2 && VM && InState != EControlRigState::Init)
			{
				VM->SetFirstEntryEventInEventQueue(EventQueue[0]);
			}

			// Transform Overrride is generated using a Transient Control 
			ApplyTransformOverrideForUserCreatedBones();
#endif

			if (InState == EControlRigState::Update && InEventName == FRigUnit_BeginExecution::EventName)
			{
				if (PreForwardsSolveEvent.IsBound())
				{
					FControlRigBracketScope BracketScope(PreForwardsSolveBracket);
					PreForwardsSolveEvent.Broadcast(this, EControlRigState::Update, FRigUnit_BeginExecution::EventName);
				}
			}

			ExecuteUnits(Context, InEventName);

			if (InState == EControlRigState::Update && InEventName == FRigUnit_BeginExecution::EventName)
			{
				if (PostForwardsSolveEvent.IsBound())
				{
					FControlRigBracketScope BracketScope(PostForwardsSolveBracket);
					PostForwardsSolveEvent.Broadcast(this, EControlRigState::Update, FRigUnit_BeginExecution::EventName);
				}
			}
			
			if (InState == EControlRigState::Init)
			{
				ExecuteUnits(Context, FRigUnit_BeginExecution::EventName);
			}
		}
		else
		{
			UE_LOG(LogControlRig, Warning, TEXT("%s: Update is being called recursively."), *GetPathName());
		}
	}

#if WITH_EDITOR
	if (ControlRigLog != nullptr && bEnableControlRigLogging && InState != EControlRigState::Init && !bJustRanInit)
	{
		for (const FControlRigLog::FLogEntry& Entry : ControlRigLog->Entries)
		{
			if (Entry.FunctionName == NAME_None || Entry.InstructionIndex == INDEX_NONE || Entry.Message.IsEmpty())
			{
				continue;
			}

			FString PerInstructionMessage = 
				FString::Printf(
					TEXT("Instruction[%d] '%s': '%s'"),
					Entry.InstructionIndex,
					*Entry.FunctionName.ToString(),
					*Entry.Message
				);

			LogOnce(Entry.Severity, Entry.InstructionIndex, PerInstructionMessage);
		}
	}

	if (bJustRanInit && ControlRigLog != nullptr)
	{
		ControlRigLog->KnownMessages.Reset();
		LoggedMessages.Reset();
	}
#endif


	if (InState == EControlRigState::Init)
	{
		if (InitializedEvent.IsBound())
		{
			FControlRigBracketScope BracketScope(InitBracket);
			InitializedEvent.Broadcast(this, EControlRigState::Init, InEventName);
		}
	}
	else if (InState == EControlRigState::Update)
	{
		DeltaTime = 0.f;

		if (ExecutedEvent.IsBound())
		{
			FControlRigBracketScope BracketScope(UpdateBracket);
			ExecutedEvent.Broadcast(this, EControlRigState::Update, InEventName);
		}
	}

	if (Context.DrawInterface && Context.DrawContainer)
	{
		Context.DrawInterface->Instructions.Append(Context.DrawContainer->Instructions);

		FRigHierarchyValidityBracket ValidityBracket(GetHierarchy());
		
		GetHierarchy()->ForEach<FRigControlElement>([this](FRigControlElement* ControlElement) -> bool
		{
			const FRigControlSettings& Settings = ControlElement->Settings;

			if (Settings.bShapeEnabled &&
				Settings.bShapeVisible &&
				!Settings.bIsTransientControl &&
				Settings.bDrawLimits &&
				Settings.LimitEnabled.Contains(FRigControlLimitEnabled(true, true)))
			{
				FTransform Transform = GetHierarchy()->GetGlobalControlOffsetTransformByIndex(ControlElement->GetIndex());
				FControlRigDrawInstruction Instruction(EControlRigDrawSettings::Lines, Settings.ShapeColor, 0.f, Transform);

				switch (Settings.ControlType)
				{
					case ERigControlType::Float:
					{
						if(Settings.LimitEnabled[0].IsOff())
						{
							break;
						}

						FVector MinPos = FVector::ZeroVector;
						FVector MaxPos = FVector::ZeroVector;

						switch (Settings.PrimaryAxis)
						{
							case ERigControlAxis::X:
							{
								MinPos.X = Settings.MinimumValue.Get<float>();
								MaxPos.X = Settings.MaximumValue.Get<float>();
								break;
							}
							case ERigControlAxis::Y:
							{
								MinPos.Y = Settings.MinimumValue.Get<float>();
								MaxPos.Y = Settings.MaximumValue.Get<float>();
								break;
							}
							case ERigControlAxis::Z:
							{
								MinPos.Z = Settings.MinimumValue.Get<float>();
								MaxPos.Z = Settings.MaximumValue.Get<float>();
								break;
							}
						}

						Instruction.Positions.Add(MinPos);
						Instruction.Positions.Add(MaxPos);
						break;
					}
					case ERigControlType::Integer:
					{
						if(Settings.LimitEnabled[0].IsOff())
						{
							break;
						}

						FVector MinPos = FVector::ZeroVector;
						FVector MaxPos = FVector::ZeroVector;

						switch (Settings.PrimaryAxis)
						{
							case ERigControlAxis::X:
							{
								MinPos.X = (float)Settings.MinimumValue.Get<int32>();
								MaxPos.X = (float)Settings.MaximumValue.Get<int32>();
								break;
							}
							case ERigControlAxis::Y:
							{
								MinPos.Y = (float)Settings.MinimumValue.Get<int32>();
								MaxPos.Y = (float)Settings.MaximumValue.Get<int32>();
								break;
							}
							case ERigControlAxis::Z:
							{
								MinPos.Z = (float)Settings.MinimumValue.Get<int32>();
								MaxPos.Z = (float)Settings.MaximumValue.Get<int32>();
								break;
							}
						}

						Instruction.Positions.Add(MinPos);
						Instruction.Positions.Add(MaxPos);
						break;
					}
					case ERigControlType::Vector2D:
					{
						if(Settings.LimitEnabled.Num() < 2)
						{
							break;
						}
						if(Settings.LimitEnabled[0].IsOff() && Settings.LimitEnabled[1].IsOff())
						{
							break;
						}

						Instruction.PrimitiveType = EControlRigDrawSettings::LineStrip;
						FVector3f MinPos = Settings.MinimumValue.Get<FVector3f>();
						FVector3f MaxPos = Settings.MaximumValue.Get<FVector3f>();

						switch (Settings.PrimaryAxis)
						{
							case ERigControlAxis::X:
							{
								Instruction.Positions.Add(FVector(0.f, MinPos.X, MinPos.Y));
								Instruction.Positions.Add(FVector(0.f, MaxPos.X, MinPos.Y));
								Instruction.Positions.Add(FVector(0.f, MaxPos.X, MaxPos.Y));
								Instruction.Positions.Add(FVector(0.f, MinPos.X, MaxPos.Y));
								Instruction.Positions.Add(FVector(0.f, MinPos.X, MinPos.Y));
								break;
							}
							case ERigControlAxis::Y:
							{
								Instruction.Positions.Add(FVector(MinPos.X, 0.f, MinPos.Y));
								Instruction.Positions.Add(FVector(MaxPos.X, 0.f, MinPos.Y));
								Instruction.Positions.Add(FVector(MaxPos.X, 0.f, MaxPos.Y));
								Instruction.Positions.Add(FVector(MinPos.X, 0.f, MaxPos.Y));
								Instruction.Positions.Add(FVector(MinPos.X, 0.f, MinPos.Y));
								break;
							}
							case ERigControlAxis::Z:
							{
								Instruction.Positions.Add(FVector(MinPos.X, MinPos.Y, 0.f));
								Instruction.Positions.Add(FVector(MaxPos.X, MinPos.Y, 0.f));
								Instruction.Positions.Add(FVector(MaxPos.X, MaxPos.Y, 0.f));
								Instruction.Positions.Add(FVector(MinPos.X, MaxPos.Y, 0.f));
								Instruction.Positions.Add(FVector(MinPos.X, MinPos.Y, 0.f));
								break;
							}
						}
						break;
					}
					case ERigControlType::Position:
					case ERigControlType::Scale:
					case ERigControlType::Transform:
					case ERigControlType::TransformNoScale:
					case ERigControlType::EulerTransform:
					{
						FVector3f MinPos = FVector3f::ZeroVector;
						FVector3f MaxPos = FVector3f::ZeroVector;

						// we only check the first three here
						// since we only consider translation anyway
						// for scale it's also the first three
						if(Settings.LimitEnabled.Num() < 3)
						{
							break;
						}
						if(!Settings.LimitEnabled[0].IsOn() && !Settings.LimitEnabled[1].IsOn() && !Settings.LimitEnabled[2].IsOn())
						{
							break;
						}

						switch (Settings.ControlType)
						{
							case ERigControlType::Position:
							case ERigControlType::Scale:
							{
								MinPos = Settings.MinimumValue.Get<FVector3f>();
								MaxPos = Settings.MaximumValue.Get<FVector3f>();
								break;
							}
							case ERigControlType::Transform:
							{
								MinPos = Settings.MinimumValue.Get<FRigControlValue::FTransform_Float>().GetTranslation();
								MaxPos = Settings.MaximumValue.Get<FRigControlValue::FTransform_Float>().GetTranslation();
								break;
							}
							case ERigControlType::TransformNoScale:
							{
								MinPos = Settings.MinimumValue.Get<FRigControlValue::FTransformNoScale_Float>().GetTranslation();
								MaxPos = Settings.MaximumValue.Get<FRigControlValue::FTransformNoScale_Float>().GetTranslation();
								break;
							}
							case ERigControlType::EulerTransform:
							{
								MinPos = Settings.MinimumValue.Get<FRigControlValue::FEulerTransform_Float>().GetTranslation();
								MaxPos = Settings.MaximumValue.Get<FRigControlValue::FEulerTransform_Float>().GetTranslation();
								break;
							}
						}

						Instruction.Positions.Add(FVector(MinPos.X, MinPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MinPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MinPos.X, MaxPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MaxPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MinPos.X, MinPos.Y, MaxPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MinPos.Y, MaxPos.Z));
						Instruction.Positions.Add(FVector(MinPos.X, MaxPos.Y, MaxPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MaxPos.Y, MaxPos.Z));

						Instruction.Positions.Add(FVector(MinPos.X, MinPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MinPos.X, MaxPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MinPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MaxPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MinPos.X, MinPos.Y, MaxPos.Z));
						Instruction.Positions.Add(FVector(MinPos.X, MaxPos.Y, MaxPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MinPos.Y, MaxPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MaxPos.Y, MaxPos.Z));

						Instruction.Positions.Add(FVector(MinPos.X, MinPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MinPos.X, MinPos.Y, MaxPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MinPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MinPos.Y, MaxPos.Z));
						Instruction.Positions.Add(FVector(MinPos.X, MaxPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MinPos.X, MaxPos.Y, MaxPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MaxPos.Y, MinPos.Z));
						Instruction.Positions.Add(FVector(MaxPos.X, MaxPos.Y, MaxPos.Z));
						break;
					}
				}

				if (Instruction.Positions.Num() > 0)
				{
					DrawInterface.Instructions.Add(Instruction);
				}
			}

			return true;
		});
	}
}

void UControlRig::ExecuteUnits(FRigUnitContext& InOutContext, const FName& InEventName)
{
	if (VM)
	{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
		FRigVMMemoryContainer* LocalMemory[] = { VM->WorkMemoryPtr, VM->LiteralMemoryPtr, VM->DebugMemoryPtr };
#else
		TArray<URigVMMemoryStorage*> LocalMemory = VM->GetLocalMemoryArray();
#endif
		TArray<void*> AdditionalArguments;
		AdditionalArguments.Add(&InOutContext);

		if (InOutContext.State == EControlRigState::Init)
		{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
			VM->Initialize(FRigVMMemoryContainerPtrArray(LocalMemory, 3), AdditionalArguments);
#else
			VM->Initialize(LocalMemory, AdditionalArguments);
#endif
		}
		else
		{
#if WITH_EDITOR
			if(URigVM* SnapShotVM = GetSnapshotVM(false)) // don't create it for normal runs
			{
				if (VM->GetHaltedAtBreakpoint() != nullptr)
				{
					VM->CopyFrom(SnapShotVM, false, false, false, true, true);	
				}
				else
				{
					SnapShotVM->CopyFrom(VM, false, false, false, true, true);
				}
			}
#endif

#if WITH_EDITOR
			URigHierarchy* Hierarchy = GetHierarchy();

			bool bRecordTransformsPerInstruction = true;
			if(const UObject* Outer = GetOuter())
			{
				if(Outer->IsA<UControlRigComponent>())
				{
					bRecordTransformsPerInstruction = false;
				}
			}

			TGuardValue<bool> RecordTransformsPerInstructionGuard(Hierarchy->bRecordTransformsPerInstruction, bRecordTransformsPerInstruction);
			if(Hierarchy->bRecordTransformsPerInstruction)
			{
				Hierarchy->ReadTransformsPerInstructionPerSlice.Reset();
				Hierarchy->WrittenTransformsPerInstructionPerSlice.Reset();
				Hierarchy->ReadTransformsPerInstructionPerSlice.AddZeroed(VM->GetByteCode().GetNumInstructions());
				Hierarchy->WrittenTransformsPerInstructionPerSlice.AddZeroed(VM->GetByteCode().GetNumInstructions());
			}
			
			TGuardValue<const FRigVMExecuteContext*> HierarchyContextGuard(Hierarchy->ExecuteContext, &VM->GetContext());
#endif

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
			VM->Execute(FRigVMMemoryContainerPtrArray(LocalMemory, 3), AdditionalArguments, InEventName);
#else
			VM->Execute(LocalMemory, AdditionalArguments, InEventName);
#endif
		}
	}
}

void UControlRig::RequestInit()
{
	bRequiresInitExecution = true;
	RequestSetup();
}

void UControlRig::RequestSetup()
{
	bRequiresSetupEvent = true;
}

void UControlRig::SetEventQueue(const TArray<FName>& InEventNames)
{
	EventQueue = InEventNames;
}

void UControlRig::UpdateVMSettings()
{
	if(VM)
	{
#if WITH_EDITOR

		// setup array handling and error reporting on the VM
		VMRuntimeSettings.LogFunction = [this](EMessageSeverity::Type InSeverity, const FRigVMExecuteContext* InContext, const FString& Message)
		{
			check(InContext);

			if(ControlRigLog)
			{
				ControlRigLog->Report(InSeverity, InContext->FunctionName, InContext->InstructionIndex, Message);
			}
			else
			{
				LogOnce(InSeverity, InContext->InstructionIndex, Message);
			}
		};
		
#endif
		
		VM->SetRuntimeSettings(VMRuntimeSettings);
	}
}

URigVM* UControlRig::GetVM()
{
	if (VM == nullptr)
	{
		Initialize(true);
		check(VM);
	}
	return VM;
}

void UControlRig::GetMappableNodeData(TArray<FName>& OutNames, TArray<FNodeItem>& OutNodeItems) const
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()

	OutNames.Reset();
	OutNodeItems.Reset();

	check(DynamicHierarchy);

	// now add all nodes
	DynamicHierarchy->ForEach<FRigBoneElement>([&OutNames, &OutNodeItems, this](FRigBoneElement* BoneElement) -> bool
    {
		OutNames.Add(BoneElement->GetName());
		FRigElementKey ParentKey = DynamicHierarchy->GetFirstParent(BoneElement->GetKey());
		if(ParentKey.Type != ERigElementType::Bone)
		{
			ParentKey.Name = NAME_None;
		}

		const FTransform GlobalInitial = DynamicHierarchy->GetGlobalTransformByIndex(BoneElement->GetIndex(), true);
		OutNodeItems.Add(FNodeItem(ParentKey.Name, GlobalInitial));
		return true;
	});
}

UAnimationDataSourceRegistry* UControlRig::GetDataSourceRegistry()
{
	if (DataSourceRegistry)
	{
		if (DataSourceRegistry->GetOuter() != this)
		{
			DataSourceRegistry = nullptr;
		}
	}
	if (DataSourceRegistry == nullptr)
	{
		DataSourceRegistry = NewObject<UAnimationDataSourceRegistry>(this);
	}
	return DataSourceRegistry;
}

#if WITH_EDITORONLY_DATA

void UControlRig::PostReinstanceCallback(const UControlRig* Old)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()

	ObjectBinding = Old->ObjectBinding;
	Initialize();
}

#endif // WITH_EDITORONLY_DATA

void UControlRig::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()

	Super::AddReferencedObjects(InThis, Collector);
}

#if WITH_EDITOR
//undo will clear out the transient Operators, need to recreate them
void UControlRig::PostEditUndo()
{
	Super::PostEditUndo();
}

#endif // WITH_EDITOR

bool UControlRig::CanExecute() const
{
	return CVarControlRigDisableExecutionAll->GetInt() == 0;
}

TArray<UControlRig*> UControlRig::FindControlRigs(UObject* Outer, TSubclassOf<UControlRig> OptionalClass)
{
	TArray<UControlRig*> Result;
	
	if(Outer == nullptr)
	{
		return Result; 
	}
	
	AActor* OuterActor = Cast<AActor>(Outer);
	if(OuterActor == nullptr)
	{
		OuterActor = Outer->GetTypedOuter<AActor>();
	}
	
	for (TObjectIterator<UControlRig> Itr; Itr; ++Itr)
	{
		UControlRig* RigInstance = *Itr;
		if (OptionalClass == nullptr || RigInstance->GetClass()->IsChildOf(OptionalClass))
		{
			if(RigInstance->IsInOuter(Outer))
			{
				Result.Add(RigInstance);
				continue;
			}

			if(OuterActor)
			{
				if(RigInstance->IsInOuter(OuterActor))
				{
					Result.Add(RigInstance);
					continue;
				}

				if (TSharedPtr<IControlRigObjectBinding> Binding = RigInstance->GetObjectBinding())
				{
					if (AActor* Actor = Binding->GetHostingActor())
					{
						if (Actor == OuterActor)
						{
							Result.Add(RigInstance);
							continue;
						}
					}
				}
			}
		}
	}

	return Result;
}

void UControlRig::Serialize(FArchive& Ar)
{
	DECLARE_SCOPE_HIERARCHICAL_COUNTER_FUNC()

	Super::Serialize(Ar);

	Ar.UsingCustomVersion(FControlRigObjectVersion::GUID);
}

void UControlRig::PostLoad()
{
	Super::PostLoad();

	if(HasAnyFlags(RF_ClassDefaultObject))
	{
		if(DynamicHierarchy)
		{
			// Some dynamic hierarchy objects have been created using NewObject<> instead of CreateDefaultSubObjects.
			// Assets from that version require the dynamic hierarchy to be flagged as below.
			DynamicHierarchy->SetFlags(DynamicHierarchy->GetFlags() | RF_Public | RF_DefaultSubObject);
		}
	}

#if WITH_EDITORONLY_DATA
	if (VMSnapshotBeforeExecution)
	{
		// Some VMSnapshots might have been created without the Transient flag.
		// Assets from that version require the snapshot to be flagged as below.
		VMSnapshotBeforeExecution->SetFlags(VMSnapshotBeforeExecution->GetFlags() | RF_Transient);
	}
#endif
}

TArray<FRigControlElement*> UControlRig::AvailableControls() const
{
	if(DynamicHierarchy)
	{
		return DynamicHierarchy->GetElementsOfType<FRigControlElement>();
	}
	return TArray<FRigControlElement*>();
}

FRigControlElement* UControlRig::FindControl(const FName& InControlName) const
{
	if(DynamicHierarchy == nullptr)
	{
		return nullptr;
	}
	return DynamicHierarchy->Find<FRigControlElement>(FRigElementKey(InControlName, ERigElementType::Control));
}

FTransform UControlRig::SetupControlFromGlobalTransform(const FName& InControlName, const FTransform& InGlobalTransform)
{
	if (IsSetupModeEnabled())
	{
		FRigControlElement* ControlElement = FindControl(InControlName);
		if (ControlElement && !ControlElement->Settings.bIsTransientControl)
		{
			const FTransform ParentTransform = GetHierarchy()->GetParentTransform(ControlElement, ERigTransformType::CurrentGlobal);
			const FTransform OffsetTransform = InGlobalTransform.GetRelativeTransform(ParentTransform);
			GetHierarchy()->SetControlOffsetTransform(ControlElement, OffsetTransform, ERigTransformType::InitialLocal, true, true);
			GetHierarchy()->SetControlOffsetTransform(ControlElement, OffsetTransform, ERigTransformType::CurrentLocal, true, true);
		}
	}
	return InGlobalTransform;
}

void UControlRig::CreateRigControlsForCurveContainer()
{
	const bool bCreateFloatControls = CVarControlRigCreateFloatControlsForCurves->GetInt() == 0 ? false : true;
	if(bCreateFloatControls && DynamicHierarchy)
	{
		URigHierarchyController* Controller = DynamicHierarchy->GetController(true);
		if(Controller == nullptr)
		{
			return;
		}
		static const FString CtrlPrefix(TEXT("CTRL_"));

		DynamicHierarchy->ForEach<FRigCurveElement>([this, Controller](FRigCurveElement* CurveElement) -> bool
        {
			const FString Name = CurveElement->GetName().ToString();
			
			if (Name.Contains(CtrlPrefix) && !DynamicHierarchy->Contains(FRigElementKey(*Name, ERigElementType::Curve))) //-V1051
			{
				FRigControlSettings Settings;
				Settings.ControlType = ERigControlType::Float;
				Settings.bIsCurve = true;
				Settings.bAnimatable = true;
				Settings.bDrawLimits = false;
				Settings.bShapeEnabled = false;
				Settings.bShapeVisible = false;
				Settings.ShapeColor = FLinearColor::Red;

				FRigControlValue Value;
				Value.Set<float>(CurveElement->Value);

				Controller->AddControl(CurveElement->GetName(), FRigElementKey(), Settings, Value, FTransform::Identity, FTransform::Identity); 
			}

			return true;
		});

		ControlModified().AddUObject(this, &UControlRig::HandleOnControlModified);
	}
}

void UControlRig::HandleOnControlModified(UControlRig* Subject, FRigControlElement* Control, const FRigControlModifiedContext& Context)
{
	if (Control->Settings.bIsCurve && DynamicHierarchy)
	{
		const FRigControlValue Value = DynamicHierarchy->GetControlValue(Control, IsSetupModeEnabled() ? ERigControlValueType::Initial : ERigControlValueType::Current);
		DynamicHierarchy->SetCurveValue(FRigElementKey(Control->GetName(), ERigElementType::Curve), Value.Get<float>());
	}	
}

void UControlRig::HandleExecutionReachedExit(const FName& InEventName)
{
#if WITH_EDITOR
	if (EventQueue.Last() == InEventName)
	{
		if(URigVM* SnapShotVM = GetSnapshotVM(false))
		{
			SnapShotVM->CopyFrom(VM, false, false, false, true, true);
		}
		DebugInfo.ResetState();
		VM->SetBreakpointAction(ERigVMBreakpointAction::None);
	}
#endif
	
	if (LatestExecutedState != EControlRigState::Init && bAccumulateTime)
	{
		AbsoluteTime += DeltaTime;
	}
}

bool UControlRig::IsCurveControl(const FRigControlElement* InControlElement) const
{
	return InControlElement->Settings.bIsCurve;
}

FTransform UControlRig::GetControlGlobalTransform(const FName& InControlName) const
{
	if(DynamicHierarchy == nullptr)
	{
		return FTransform::Identity;
	}
	return DynamicHierarchy->GetGlobalTransform(FRigElementKey(InControlName, ERigElementType::Control), false);
}

bool UControlRig::SetControlGlobalTransform(const FName& InControlName, const FTransform& InGlobalTransform, bool bNotify, const FRigControlModifiedContext& Context, bool bSetupUndo, bool bPrintPythonCommands)
{
	FTransform GlobalTransform = InGlobalTransform;
	if (IsSetupModeEnabled())
	{
		GlobalTransform = SetupControlFromGlobalTransform(InControlName, GlobalTransform);
	}

	FRigControlValue Value = GetControlValueFromGlobalTransform(InControlName, GlobalTransform);
	if (OnFilterControl.IsBound())
	{
		FRigControlElement* Control = FindControl(InControlName);
		if (Control)
		{
			OnFilterControl.Broadcast(this, Control, Value);
		}
	}

	SetControlValue(InControlName, Value, bNotify, Context, bSetupUndo, bPrintPythonCommands);
	return true;
}

FRigControlValue UControlRig::GetControlValueFromGlobalTransform(const FName& InControlName, const FTransform& InGlobalTransform)
{
	FRigControlValue Value;

	if (FRigControlElement* ControlElement = FindControl(InControlName))
	{
		if(DynamicHierarchy)
		{
			FTransform Transform = DynamicHierarchy->ComputeLocalControlValue(ControlElement, InGlobalTransform, ERigTransformType::CurrentGlobal);
			Value.SetFromTransform(Transform, ControlElement->Settings.ControlType, ControlElement->Settings.PrimaryAxis);

			if (ShouldApplyLimits())
			{
				ControlElement->Settings.ApplyLimits(Value);
			}
		}
	}

	return Value;
}

void UControlRig::SetControlLocalTransform(const FName& InControlName, const FTransform& InLocalTransform, bool bNotify, const FRigControlModifiedContext& Context, bool bSetupUndo)
{
	if (FRigControlElement* ControlElement = FindControl(InControlName))
	{
		FRigControlValue Value;
		Value.SetFromTransform(InLocalTransform, ControlElement->Settings.ControlType, ControlElement->Settings.PrimaryAxis);

		if (OnFilterControl.IsBound())
		{
			OnFilterControl.Broadcast(this, ControlElement, Value);
			
		}
		SetControlValue(InControlName, Value, bNotify, Context, bSetupUndo);
	}
}

FTransform UControlRig::GetControlLocalTransform(const FName& InControlName)
{
	if(DynamicHierarchy == nullptr)
	{
		return FTransform::Identity;
	}
	return DynamicHierarchy->GetLocalTransform(FRigElementKey(InControlName, ERigElementType::Control));
}

const TArray<TSoftObjectPtr<UControlRigShapeLibrary>>& UControlRig::GetShapeLibraries() const
{
	if (UControlRig* CDO = Cast<UControlRig>(GetClass()->GetDefaultObject()))
	{
		for(TSoftObjectPtr<UControlRigShapeLibrary>& ShapeLibrary : CDO->ShapeLibraries)
		{
			if (!ShapeLibrary.IsValid())
			{
				ShapeLibrary.LoadSynchronous();
			}
		}
		return CDO->ShapeLibraries;
	}

	static TArray<TSoftObjectPtr<UControlRigShapeLibrary>> EmptyShapeLibraries;
	return EmptyShapeLibraries;
}

void UControlRig::SelectControl(const FName& InControlName, bool bSelect)
{
	if(DynamicHierarchy)
	{
		if(URigHierarchyController* Controller = DynamicHierarchy->GetController(true))
		{
			Controller->SelectElement(FRigElementKey(InControlName, ERigElementType::Control), bSelect);
		}
	}
}

bool UControlRig::ClearControlSelection()
{
	if(DynamicHierarchy)
	{
		if(URigHierarchyController* Controller = DynamicHierarchy->GetController(true))
		{
			return Controller->ClearSelection();
		}
	}
	return false;
}

TArray<FName> UControlRig::CurrentControlSelection() const
{
	TArray<FName> SelectedControlNames;

	if(DynamicHierarchy)
	{
		TArray<FRigBaseElement*> SelectedControls = DynamicHierarchy->GetSelectedElements(ERigElementType::Control);
		for (FRigBaseElement* SelectedControl : SelectedControls)
		{
			SelectedControlNames.Add(SelectedControl->GetName());
		}
	}
	return SelectedControlNames;
}

bool UControlRig::IsControlSelected(const FName& InControlName)const
{
	if(DynamicHierarchy)
	{
		if(FRigControlElement* ControlElement = FindControl(InControlName))
		{
			return DynamicHierarchy->IsSelected(ControlElement);
		}
	}
	return false;
}

void UControlRig::HandleHierarchyModified(ERigHierarchyNotification InNotification, URigHierarchy* InHierarchy,
    const FRigBaseElement* InElement)
{
	switch(InNotification)
	{
		case ERigHierarchyNotification::ElementSelected:
		case ERigHierarchyNotification::ElementDeselected:
		{
			if(FRigControlElement* ControlElement = Cast<FRigControlElement>((FRigBaseElement*)InElement))
			{
				const bool bSelected = InNotification == ERigHierarchyNotification::ElementSelected;
				ControlSelected().Broadcast(this, ControlElement, bSelected);
			}
			break;
		}
		case ERigHierarchyNotification::ControlSettingChanged:
		case ERigHierarchyNotification::ControlShapeTransformChanged:
		{
			if(FRigControlElement* ControlElement = Cast<FRigControlElement>((FRigBaseElement*)InElement))
			{
				ControlModified().Broadcast(this, ControlElement, FRigControlModifiedContext(EControlRigSetKey::Never));
			}
			break;
		}
		default:
		{
			break;
		}
	}
}

#if WITH_EDITOR

FName UControlRig::AddTransientControl(URigVMPin* InPin, FRigElementKey SpaceKey, FTransform OffsetTransform)
{
	if ((InPin == nullptr) || (DynamicHierarchy == nullptr))
	{
		return NAME_None;
	}

	if (InPin->GetCPPType() != TEXT("FVector") &&
		InPin->GetCPPType() != TEXT("FQuat") &&
		InPin->GetCPPType() != TEXT("FTransform"))
	{
		return NAME_None;
	}

	RemoveTransientControl(InPin);

	URigHierarchyController* Controller = DynamicHierarchy->GetController(true);
	if(Controller == nullptr)
	{
		return NAME_None;
	}

	URigVMPin* PinForLink = InPin->GetPinForLink();

	const FName ControlName = GetNameForTransientControl(InPin);
	FTransform ShapeTransform = FTransform::Identity;
	ShapeTransform.SetScale3D(FVector::ZeroVector);

	FRigControlSettings Settings;
	Settings.ControlType = ERigControlType::Transform;
	if (URigVMPin* ColorPin = PinForLink->GetNode()->FindPin(TEXT("Color")))
	{
		if (ColorPin->GetCPPType() == TEXT("FLinearColor"))
		{
			FRigControlValue Value;
			Settings.ShapeColor = Value.SetFromString<FLinearColor>(ColorPin->GetDefaultValue());
		}
	}
	Settings.bIsTransientControl = true;
	Settings.DisplayName = TEXT("Temporary Control");

	Controller->ClearSelection();

    const FRigElementKey ControlKey = Controller->AddControl(
    	ControlName,
    	SpaceKey,
    	Settings,
    	FRigControlValue::Make(FTransform::Identity),
    	OffsetTransform,
    	ShapeTransform, false);

	SetTransientControlValue(InPin);

	if(const FRigBaseElement* Element = DynamicHierarchy->Find(ControlKey))
	{
		DynamicHierarchy->Notify(ERigHierarchyNotification::ElementSelected, Element);
	}

	return ControlName;
}

bool UControlRig::SetTransientControlValue(URigVMPin* InPin)
{
	const FName ControlName = GetNameForTransientControl(InPin);
	if (FRigControlElement* ControlElement = FindControl(ControlName))
	{
		FString DefaultValue = InPin->GetPinForLink()->GetDefaultValue();
		if (!DefaultValue.IsEmpty())
		{
			if (InPin->GetCPPType() == TEXT("FVector"))
			{
				ControlElement->Settings.ControlType = ERigControlType::Position;
				FRigControlValue Value;
				Value.SetFromString<FVector>(DefaultValue);
				DynamicHierarchy->SetControlValue(ControlElement, Value, ERigControlValueType::Current, false);
			}
			else if (InPin->GetCPPType() == TEXT("FQuat"))
			{
				ControlElement->Settings.ControlType = ERigControlType::Rotator;
				FRigControlValue Value;
				Value.SetFromString<FRotator>(DefaultValue);
				DynamicHierarchy->SetControlValue(ControlElement, Value, ERigControlValueType::Current, false);
			}
			else
			{
				ControlElement->Settings.ControlType = ERigControlType::Transform;
				FRigControlValue Value;
				Value.SetFromString<FTransform>(DefaultValue);
				DynamicHierarchy->SetControlValue(ControlElement, Value, ERigControlValueType::Current, false);
			}
		}
		return true;
	}
	return false;
}

FName UControlRig::RemoveTransientControl(URigVMPin* InPin)
{
	if ((InPin == nullptr) || (DynamicHierarchy == nullptr))
	{
		return NAME_None;
	}

	URigHierarchyController* Controller = DynamicHierarchy->GetController(true);
	if(Controller == nullptr)
	{
		return NAME_None;
	}

	const FName ControlName = GetNameForTransientControl(InPin);
	if(FRigControlElement* ControlElement = FindControl(ControlName))
	{
		DynamicHierarchy->Notify(ERigHierarchyNotification::ElementDeselected, ControlElement);
		if(Controller->RemoveElement(ControlElement))
		{
			return ControlName;
		}
	}

	return NAME_None;
}

FName UControlRig::AddTransientControl(const FRigElementKey& InElement)
{
	if (!InElement.IsValid())
	{
		return NAME_None;
	}

	if(DynamicHierarchy == nullptr)
	{
		return NAME_None;
	}

	URigHierarchyController* Controller = DynamicHierarchy->GetController(true);
	if(Controller == nullptr)
	{
		return NAME_None;
	}

	const FName ControlName = GetNameForTransientControl(InElement);
	if(DynamicHierarchy->Contains(FRigElementKey(ControlName, ERigElementType::Control)))
	{
		SetTransientControlValue(InElement);
		return ControlName;
	}

	const int32 ElementIndex = DynamicHierarchy->GetIndex(InElement);
	if (ElementIndex == INDEX_NONE)
	{
		return NAME_None;
	}

	FTransform ShapeTransform = FTransform::Identity;
	ShapeTransform.SetScale3D(FVector::ZeroVector);

	FRigControlSettings Settings;
	Settings.ControlType = ERigControlType::Transform;
	Settings.bIsTransientControl = true;
	Settings.DisplayName = TEXT("Temporary Control");

	FRigElementKey Parent;
	switch (InElement.Type)
	{
		case ERigElementType::Bone:
		{
			Parent = DynamicHierarchy->GetFirstParent(InElement);
			break;
		}
		case ERigElementType::Null:
		{
			Parent = InElement;
			break;
		}
		default:
		{
			break;
		}
	}

	TArray<FRigElementKey> SelectedControls = DynamicHierarchy->GetSelectedKeys(ERigElementType::Control);
	for(const FRigElementKey& SelectedControl : SelectedControls)
	{
		Controller->DeselectElement(SelectedControl);
	}

	const FRigElementKey ControlKey = Controller->AddControl(
        ControlName,
        Parent,
        Settings,
        FRigControlValue::Make(FTransform::Identity),
        FTransform::Identity,
        ShapeTransform, false);

	if (InElement.Type == ERigElementType::Bone)
	{
		// don't allow transient control to modify forward mode poses when we
		// already switched to the setup mode
		if (!IsSetupModeEnabled())
		{
			if(FRigBoneElement* BoneElement = DynamicHierarchy->Find<FRigBoneElement>(InElement))
			{
				// add a modify bone AnimNode internally that the transient control controls for imported bones only
				// for user created bones, refer to UControlRig::TransformOverrideForUserCreatedBones 
				if (BoneElement->BoneType == ERigBoneType::Imported)
				{ 
					if (PreviewInstance)
					{
						PreviewInstance->ModifyBone(InElement.Name);
					}
				}
				else if (BoneElement->BoneType == ERigBoneType::User)
				{
					// add an empty entry, which will be given the correct value in
					// SetTransientControlValue(InElement);
					TransformOverrideForUserCreatedBones.FindOrAdd(InElement.Name);
				}
			}
		}
	}

	SetTransientControlValue(InElement);

	if(const FRigBaseElement* Element = DynamicHierarchy->Find(ControlKey))
	{
		DynamicHierarchy->Notify(ERigHierarchyNotification::ElementSelected, Element);
	}

	return ControlName;
}

bool UControlRig::SetTransientControlValue(const FRigElementKey& InElement)
{
	if (!InElement.IsValid())
	{
		return false;
	}

	if(DynamicHierarchy == nullptr)
	{
		return false;
	}

	const FName ControlName = GetNameForTransientControl(InElement);
	if(FRigControlElement* ControlElement = FindControl(ControlName))
	{
		if (InElement.Type == ERigElementType::Bone)
		{
			if (IsSetupModeEnabled())
			{
				// need to get initial because that is what setup mode uses
				// specifically, when user change the initial from the details panel
				// this will allow the transient control to react to that change
				const FTransform InitialLocalTransform = DynamicHierarchy->GetInitialLocalTransform(InElement);
				DynamicHierarchy->SetTransform(ControlElement, InitialLocalTransform, ERigTransformType::InitialLocal, true, false);
				DynamicHierarchy->SetTransform(ControlElement, InitialLocalTransform, ERigTransformType::CurrentLocal, true, false);
			}
			else
			{
				const FTransform LocalTransform = DynamicHierarchy->GetLocalTransform(InElement);
				DynamicHierarchy->SetTransform(ControlElement, LocalTransform, ERigTransformType::InitialLocal, true, false);
				DynamicHierarchy->SetTransform(ControlElement, LocalTransform, ERigTransformType::CurrentLocal, true, false);

				if (FRigBoneElement* BoneElement = DynamicHierarchy->Find<FRigBoneElement>(InElement))
				{
					if (BoneElement->BoneType == ERigBoneType::Imported)
					{
						if (PreviewInstance)
						{
							if (FAnimNode_ModifyBone* Modify = PreviewInstance->FindModifiedBone(InElement.Name))
							{
								Modify->Translation = LocalTransform.GetTranslation();
								Modify->Rotation = LocalTransform.GetRotation().Rotator();
								Modify->TranslationSpace = EBoneControlSpace::BCS_ParentBoneSpace;
								Modify->RotationSpace = EBoneControlSpace::BCS_ParentBoneSpace;
							}
						}	
					}
					else if (BoneElement->BoneType == ERigBoneType::User)
					{
						if (FTransform* TransformOverride = TransformOverrideForUserCreatedBones.Find(InElement.Name))
						{
							*TransformOverride = LocalTransform;
						}	
					}
				}
			}
		}
		else if (InElement.Type == ERigElementType::Null)
		{
			const FTransform GlobalTransform = DynamicHierarchy->GetGlobalTransform(InElement);
			DynamicHierarchy->SetTransform(ControlElement, GlobalTransform, ERigTransformType::InitialGlobal, true, false);
			DynamicHierarchy->SetTransform(ControlElement, GlobalTransform, ERigTransformType::CurrentGlobal, true, false);
		}

		return true;
	}
	return false;
}

FName UControlRig::RemoveTransientControl(const FRigElementKey& InElement)
{
	if (!InElement.IsValid())
	{
		return NAME_None;
	}

	if(DynamicHierarchy == nullptr)
	{
		return NAME_None;
	}

	URigHierarchyController* Controller = DynamicHierarchy->GetController(true);
	if(Controller == nullptr)
	{
		return NAME_None;
	}

	const FName ControlName = GetNameForTransientControl(InElement);
	if(FRigControlElement* ControlElement = FindControl(ControlName))
	{
		DynamicHierarchy->Notify(ERigHierarchyNotification::ElementDeselected, ControlElement);
		if(Controller->RemoveElement(ControlElement))
		{
			return ControlName;
		}
	}

	return NAME_None;
}

FName UControlRig::GetNameForTransientControl(URigVMPin* InPin) const
{
	check(InPin);
	check(DynamicHierarchy);
	
	const FString OriginalPinPath = InPin->GetOriginalPinFromInjectedNode()->GetPinPath();
	return DynamicHierarchy->GetSanitizedName(FString::Printf(TEXT("ControlForPin_%s"), *OriginalPinPath));
}

FString UControlRig::GetPinNameFromTransientControl(const FRigElementKey& InKey)
{
	FString Name = InKey.Name.ToString();
	if(Name.StartsWith(TEXT("ControlForPin_")))
	{
		Name.RightChopInline(14);
	}
	return Name;
}

FName UControlRig::GetNameForTransientControl(const FRigElementKey& InElement)
{
	if (InElement.Type == ERigElementType::Control)
	{
		return InElement.Name;
	}

	const FName EnumName = *StaticEnum<ERigElementType>()->GetDisplayNameTextByValue((int64)InElement.Type).ToString();
	return *FString::Printf(TEXT("ControlForRigElement_%s_%s"), *EnumName.ToString(), *InElement.Name.ToString());
}

FRigElementKey UControlRig::GetElementKeyFromTransientControl(const FRigElementKey& InKey)
{
	if(InKey.Type != ERigElementType::Control)
	{
		return FRigElementKey();
	}
	
	static FString ControlRigForElementBoneName;
	static FString ControlRigForElementNullName;

	if (ControlRigForElementBoneName.IsEmpty())
	{
		ControlRigForElementBoneName = FString::Printf(TEXT("ControlForRigElement_%s_"),
            *StaticEnum<ERigElementType>()->GetDisplayNameTextByValue((int64)ERigElementType::Bone).ToString());
		ControlRigForElementNullName = FString::Printf(TEXT("ControlForRigElement_%s_"),
            *StaticEnum<ERigElementType>()->GetDisplayNameTextByValue((int64)ERigElementType::Null).ToString());
	}
	
	FString Name = InKey.Name.ToString();
	if(Name.StartsWith(ControlRigForElementBoneName))
	{
		Name.RightChopInline(ControlRigForElementBoneName.Len());
		return FRigElementKey(*Name, ERigElementType::Bone);
	}
	if(Name.StartsWith(ControlRigForElementNullName))
	{
		Name.RightChopInline(ControlRigForElementNullName.Len());
		return FRigElementKey(*Name, ERigElementType::Null);
	}
	
	return FRigElementKey();;
}

void UControlRig::ClearTransientControls()
{
	if(DynamicHierarchy == nullptr)
	{
		return;
	}

	URigHierarchyController* Controller = DynamicHierarchy->GetController(true);
	if(Controller == nullptr)
	{
		return;
	}

	const TArray<FRigControlElement*> ControlsToRemove = DynamicHierarchy->GetTransientControls();
	for (FRigControlElement* ControlToRemove : ControlsToRemove)
	{
		DynamicHierarchy->Notify(ERigHierarchyNotification::ElementDeselected, ControlToRemove);
		Controller->RemoveElement(ControlToRemove);
	}
}

void UControlRig::ApplyTransformOverrideForUserCreatedBones()
{
	if(DynamicHierarchy == nullptr)
	{
		return;
	}
	
	for (const auto& Entry : TransformOverrideForUserCreatedBones)
	{
		DynamicHierarchy->SetLocalTransform(FRigElementKey(Entry.Key, ERigElementType::Bone), Entry.Value, false);
	}
}

#endif

void UControlRig::HandleHierarchyEvent(URigHierarchy* InHierarchy, const FRigEventContext& InEvent)
{
	if (RigEventDelegate.IsBound())
	{
		RigEventDelegate.Broadcast(InHierarchy, InEvent);
	}

	switch (InEvent.Event)
	{
		case ERigEvent::RequestAutoKey:
		{
			int32 Index = InHierarchy->GetIndex(InEvent.Key);
			if (Index != INDEX_NONE && InEvent.Key.Type == ERigElementType::Control)
			{
				if(FRigControlElement* ControlElement = InHierarchy->GetChecked<FRigControlElement>(Index))
				{
					FRigControlModifiedContext Context;
					Context.SetKey = EControlRigSetKey::Always;
					Context.LocalTime = InEvent.LocalTime;
					Context.EventName = InEvent.SourceEventName;
					ControlModified().Broadcast(this, ControlElement, Context);
				}
			}
		}
		default:
		{
			break;
		}
	}
}

void UControlRig::GetControlsInOrder(TArray<FRigControlElement*>& SortedControls) const
{
	SortedControls.Reset();

	if(DynamicHierarchy == nullptr)
	{
		return;
	}

	SortedControls = DynamicHierarchy->GetControls(true);
}

const FRigInfluenceMap* UControlRig::FindInfluenceMap(const FName& InEventName)
{
	if (UControlRig* CDO = GetClass()->GetDefaultObject<UControlRig>())
	{
		return CDO->Influences.Find(InEventName);
	}
	return nullptr;
}

void UControlRig::SetInteractionRig(UControlRig* InInteractionRig)
{
	if (InteractionRig == InInteractionRig)
	{
		return;
	}

	if (InteractionRig != nullptr)
	{
		InteractionRig->ControlModified().RemoveAll(this);
		InteractionRig->OnInitialized_AnyThread().RemoveAll(this);
		InteractionRig->OnExecuted_AnyThread().RemoveAll(this);
		InteractionRig->ControlSelected().RemoveAll(this);
		OnInitialized_AnyThread().RemoveAll(InteractionRig);
		OnExecuted_AnyThread().RemoveAll(InteractionRig);
		ControlSelected().RemoveAll(InteractionRig);
	}

	InteractionRig = InInteractionRig;

	if (InteractionRig != nullptr)
	{
		SetInteractionRigClass(InteractionRig->GetClass());

		InteractionRig->Initialize(true);
		InteractionRig->CopyPoseFromOtherRig(this);
		InteractionRig->RequestSetup();
		InteractionRig->Execute(EControlRigState::Update, FRigUnit_BeginExecution::EventName);

		InteractionRig->ControlModified().AddUObject(this, &UControlRig::HandleInteractionRigControlModified);
		InteractionRig->OnInitialized_AnyThread().AddUObject(this, &UControlRig::HandleInteractionRigInitialized);
		InteractionRig->OnExecuted_AnyThread().AddUObject(this, &UControlRig::HandleInteractionRigExecuted);
		InteractionRig->ControlSelected().AddUObject(this, &UControlRig::HandleInteractionRigControlSelected, false);
		OnInitialized_AnyThread().AddUObject(ToRawPtr(InteractionRig), &UControlRig::HandleInteractionRigInitialized);
		OnExecuted_AnyThread().AddUObject(ToRawPtr(InteractionRig), &UControlRig::HandleInteractionRigExecuted);
		ControlSelected().AddUObject(ToRawPtr(InteractionRig), &UControlRig::HandleInteractionRigControlSelected, true);

		FControlRigBracketScope BracketScope(InterRigSyncBracket);
		InteractionRig->HandleInteractionRigExecuted(this, EControlRigState::Update, FRigUnit_BeginExecution::EventName);
	}
}

void UControlRig::SetInteractionRigClass(TSubclassOf<UControlRig> InInteractionRigClass)
{
	if (InteractionRigClass == InInteractionRigClass)
	{
		return;
	}

	InteractionRigClass = InInteractionRigClass;

	if(InteractionRigClass)
	{
		if(InteractionRig != nullptr)
		{
			if(InteractionRig->GetClass() != InInteractionRigClass)
			{
				SetInteractionRig(nullptr);
			}
		}

		if(InteractionRig == nullptr)
		{
			UControlRig* NewInteractionRig = NewObject<UControlRig>(this, InteractionRigClass);
			SetInteractionRig(NewInteractionRig);
		}
	}
}

#if WITH_EDITOR

void UControlRig::PreEditChange(FProperty* PropertyAboutToChange)
{
	Super::PreEditChange(PropertyAboutToChange);

	if (PropertyAboutToChange && PropertyAboutToChange->GetFName() == GET_MEMBER_NAME_CHECKED(UControlRig, InteractionRig))
	{
		SetInteractionRig(nullptr);
	}
	else if (PropertyAboutToChange && PropertyAboutToChange->GetFName() == GET_MEMBER_NAME_CHECKED(UControlRig, InteractionRigClass))
	{
		SetInteractionRigClass(nullptr);
	}
}

void UControlRig::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.MemberProperty && PropertyChangedEvent.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UControlRig, InteractionRig))
	{
		UControlRig* NewInteractionRig = InteractionRig;
		SetInteractionRig(nullptr);
		SetInteractionRig(NewInteractionRig);
	}
	else if (PropertyChangedEvent.MemberProperty && PropertyChangedEvent.MemberProperty->GetFName() == GET_MEMBER_NAME_CHECKED(UControlRig, InteractionRigClass))
	{
		TSubclassOf<UControlRig> NewInteractionRigClass = InteractionRigClass;
		SetInteractionRigClass(nullptr);
		SetInteractionRigClass(NewInteractionRigClass);
		if (NewInteractionRigClass == nullptr)
		{
			SetInteractionRig(nullptr);
		}
	}
}
#endif

void UControlRig::AddAssetUserData(UAssetUserData* InUserData)
{
	if (InUserData != NULL)
	{
		UAssetUserData* ExistingData = GetAssetUserDataOfClass(InUserData->GetClass());
		if (ExistingData != NULL)
		{
			AssetUserData.Remove(ExistingData);
		}
		AssetUserData.Add(InUserData);
	}
}

UAssetUserData* UControlRig::GetAssetUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIdx = 0; DataIdx < AssetUserData.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserData[DataIdx];
		if (Datum != NULL && Datum->IsA(InUserDataClass))
		{
			return Datum;
		}
	}
	return NULL;
}

void UControlRig::RemoveUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass)
{
	for (int32 DataIdx = 0; DataIdx < AssetUserData.Num(); DataIdx++)
	{
		UAssetUserData* Datum = AssetUserData[DataIdx];
		if (Datum != NULL && Datum->IsA(InUserDataClass))
		{
			AssetUserData.RemoveAt(DataIdx);
			return;
		}
	}
}

const TArray<UAssetUserData*>* UControlRig::GetAssetUserDataArray() const
{
	return &ToRawPtrTArrayUnsafe(AssetUserData);
}

void UControlRig::CopyPoseFromOtherRig(UControlRig* Subject)
{
	check(DynamicHierarchy);
	check(Subject);
	URigHierarchy* OtherHierarchy = Subject->GetHierarchy();
	check(OtherHierarchy);

	for (FRigBaseElement* Element : *DynamicHierarchy)
	{
		FRigBaseElement* OtherElement = OtherHierarchy->Find(Element->GetKey());
		if(OtherElement == nullptr)
		{
			continue;
		}

		if(OtherElement->GetType() != Element->GetType())
		{
			continue;
		}

		if(FRigBoneElement* BoneElement = Cast<FRigBoneElement>(Element))
		{
			FRigBoneElement* OtherBoneElement = CastChecked<FRigBoneElement>(OtherElement);
			const FTransform Transform = OtherHierarchy->GetTransform(OtherBoneElement, ERigTransformType::CurrentLocal);
			DynamicHierarchy->SetTransform(BoneElement, Transform, ERigTransformType::CurrentLocal, true, false);
		}
		else if(FRigCurveElement* CurveElement = Cast<FRigCurveElement>(Element))
		{
			FRigCurveElement* OtherCurveElement = CastChecked<FRigCurveElement>(OtherElement);
			const float Value = OtherHierarchy->GetCurveValue(OtherCurveElement);
			DynamicHierarchy->SetCurveValue(CurveElement, Value, false);
		}
	}
}

void UControlRig::HandleInteractionRigControlModified(UControlRig* Subject, FRigControlElement* Control, const FRigControlModifiedContext& Context)
{
	check(Subject);

	if (IsSyncingWithOtherRig() || IsExecuting())
	{
		return;
	}
	FControlRigBracketScope BracketScope(InterRigSyncBracket);

	if (Subject != InteractionRig)
	{
		return;
	}

	if (const FRigInfluenceMap* InfluenceMap = Subject->FindInfluenceMap(Context.EventName))
	{
		if (const FRigInfluenceEntry* InfluenceEntry = InfluenceMap->Find(Control->GetKey()))
		{
			for (const FRigElementKey& AffectedKey : *InfluenceEntry)
			{
				if (AffectedKey.Type == ERigElementType::Control)
				{
					if (FRigControlElement* AffectedControl = FindControl(AffectedKey.Name))
					{
						QueuedModifiedControls.Add(AffectedControl->GetKey());
					}
				}
				else if (
					AffectedKey.Type == ERigElementType::Bone ||
					AffectedKey.Type == ERigElementType::Curve)
				{
					// special case controls with a CONTROL suffix
					FName BoneControlName = *FString::Printf(TEXT("%s_CONTROL"), *AffectedKey.Name.ToString());
					if (FRigControlElement* AffectedControl = FindControl(BoneControlName))
					{
						QueuedModifiedControls.Add(AffectedControl->GetKey());
					}
				}
			}
		}
	}

}

void UControlRig::HandleInteractionRigInitialized(UControlRig* Subject, EControlRigState State, const FName& EventName)
{
	check(Subject);

	if (IsSyncingWithOtherRig())
	{
		return;
	}
	FControlRigBracketScope BracketScope(InterRigSyncBracket);
	RequestInit();
}

void UControlRig::HandleInteractionRigExecuted(UControlRig* Subject, EControlRigState State, const FName& EventName)
{
	check(Subject);

	if (IsSyncingWithOtherRig() || IsExecuting())
	{
		return;
	}
	FControlRigBracketScope BracketScope(InterRigSyncBracket);

	CopyPoseFromOtherRig(Subject);
	Execute(EControlRigState::Update, FRigUnit_InverseExecution::EventName);

	FRigControlModifiedContext Context;
	Context.EventName = FRigUnit_InverseExecution::EventName;
	Context.SetKey = EControlRigSetKey::DoNotCare;

	for (const FRigElementKey& QueuedModifiedControl : QueuedModifiedControls)
	{
		if(FRigControlElement* ControlElement = FindControl(QueuedModifiedControl.Name))
		{
			ControlModified().Broadcast(this, ControlElement, Context);
		}
	}
}

void UControlRig::HandleInteractionRigControlSelected(UControlRig* Subject, FRigControlElement* Control, bool bSelected, bool bInverted)
{
	check(Subject);

	if (IsSyncingWithOtherRig() || IsExecuting())
	{
		return;
	}
	if (Subject->IsSyncingWithOtherRig() || Subject->IsExecuting())
	{
		return;
	}
	FControlRigBracketScope BracketScope(InterRigSyncBracket);

	const FRigInfluenceMap* InfluenceMap = nullptr;
	if (bInverted)
	{
		InfluenceMap = FindInfluenceMap(FRigUnit_BeginExecution::EventName);
	}
	else
	{
		InfluenceMap = Subject->FindInfluenceMap(FRigUnit_BeginExecution::EventName);
	}

	if (InfluenceMap)
	{
		FRigInfluenceMap InvertedMap;
		if (bInverted)
		{
			InvertedMap = InfluenceMap->Inverse();
			InfluenceMap = &InvertedMap;
		}

		struct Local
		{
			static void SelectAffectedElements(UControlRig* ThisRig, const FRigInfluenceMap* InfluenceMap, const FRigElementKey& InKey, bool bSelected, bool bInverted)
			{
				if (const FRigInfluenceEntry* InfluenceEntry = InfluenceMap->Find(InKey))
				{
					for (const FRigElementKey& AffectedKey : *InfluenceEntry)
					{
						if (AffectedKey.Type == ERigElementType::Control)
						{
							ThisRig->SelectControl(AffectedKey.Name, bSelected);
						}

						if (bInverted)
						{
							if (AffectedKey.Type == ERigElementType::Control)
							{
								ThisRig->SelectControl(AffectedKey.Name, bSelected);
							}
						}
						else
						{
							if (AffectedKey.Type == ERigElementType::Control)
							{
								ThisRig->SelectControl(AffectedKey.Name, bSelected);
							}
							else if (AffectedKey.Type == ERigElementType::Bone ||
								AffectedKey.Type == ERigElementType::Curve)
							{
								FName ControlName = *FString::Printf(TEXT("%s_CONTROL"), *AffectedKey.Name.ToString());
								ThisRig->SelectControl(ControlName, bSelected);
							}
						}
					}
				}
			}
		};

		Local::SelectAffectedElements(this, InfluenceMap, Control->GetKey(), bSelected, bInverted);

		if (bInverted)
		{
			const FString ControlName = Control->GetName().ToString();
			if (ControlName.EndsWith(TEXT("_CONTROL")))
			{
				const FString BaseName = ControlName.Left(ControlName.Len() - 8);
				Local::SelectAffectedElements(this, InfluenceMap, FRigElementKey(*BaseName, ERigElementType::Bone), bSelected, bInverted);
				Local::SelectAffectedElements(this, InfluenceMap, FRigElementKey(*BaseName, ERigElementType::Curve), bSelected, bInverted);
			}
		}
	}
}

#if WITH_EDITOR

URigVM* UControlRig::GetSnapshotVM(bool bCreateIfNeeded)
{
#if WITH_EDITOR
	if ((VMSnapshotBeforeExecution == nullptr) && bCreateIfNeeded)
	{
		VMSnapshotBeforeExecution = NewObject<URigVM>(GetTransientPackage(), NAME_None, RF_Transient);
	}
	return VMSnapshotBeforeExecution;
#else
	return nullptr;
#endif
}

void UControlRig::LogOnce(EMessageSeverity::Type InSeverity, int32 InInstructionIndex, const FString& InMessage)
{
	if(LoggedMessages.Contains(InMessage))
	{
		return;
	}

	switch (InSeverity)
	{
		case EMessageSeverity::CriticalError:
		case EMessageSeverity::Error:
		{
			UE_LOG(LogControlRig, Error, TEXT("%s"), *InMessage);
			break;
		}
		case EMessageSeverity::PerformanceWarning:
		case EMessageSeverity::Warning:
		{
			UE_LOG(LogControlRig, Warning, TEXT("%s"), *InMessage);
			break;
		}
		case EMessageSeverity::Info:
		{
			UE_LOG(LogControlRig, Display, TEXT("%s"), *InMessage);
			break;
		}
		default:
		{
			break;
		}
	}

	LoggedMessages.Add(InMessage, true);
}

void UControlRig::AddBreakpoint(int32 InstructionIndex, URigVMNode* InNode, uint16 InDepth)
{
	DebugInfo.AddBreakpoint(InstructionIndex, InNode, InDepth);
}

bool UControlRig::ExecuteBreakpointAction(const ERigVMBreakpointAction BreakpointAction)
{
	if (VM->GetHaltedAtBreakpoint() != nullptr)
	{
		VM->SetBreakpointAction(BreakpointAction);
		return true;
	}
	return false;
}

#endif // WITH_EDITOR

void UControlRig::SetBoneInitialTransformsFromAnimInstance(UAnimInstance* InAnimInstance)
{
	FMemMark Mark(FMemStack::Get());
	FCompactPose OutPose;
	OutPose.ResetToRefPose(InAnimInstance->GetRequiredBones());
	SetBoneInitialTransformsFromCompactPose(&OutPose);
}

void UControlRig::SetBoneInitialTransformsFromAnimInstanceProxy(const FAnimInstanceProxy* InAnimInstanceProxy)
{
	FMemMark Mark(FMemStack::Get());
	FCompactPose OutPose;
	OutPose.ResetToRefPose(InAnimInstanceProxy->GetRequiredBones());
	SetBoneInitialTransformsFromCompactPose(&OutPose);
}

void UControlRig::SetBoneInitialTransformsFromSkeletalMeshComponent(USkeletalMeshComponent* InSkelMeshComp, bool bUseAnimInstance)
{
	check(InSkelMeshComp);
	check(DynamicHierarchy);
	
	if (!bUseAnimInstance && (InSkelMeshComp->GetAnimInstance() != nullptr))
	{
		SetBoneInitialTransformsFromAnimInstance(InSkelMeshComp->GetAnimInstance());
	}
	else
	{
		SetBoneInitialTransformsFromSkeletalMesh(InSkelMeshComp->SkeletalMesh);
		}
}


void UControlRig::SetBoneInitialTransformsFromSkeletalMesh(USkeletalMesh* InSkeletalMesh)
{
	if (ensure(InSkeletalMesh))
	{ 
		SetBoneInitialTransformsFromRefSkeleton(InSkeletalMesh->GetRefSkeleton());
	}
}

void UControlRig::SetBoneInitialTransformsFromRefSkeleton(const FReferenceSkeleton& InReferenceSkeleton)
{
	check(DynamicHierarchy);

	DynamicHierarchy->ForEach<FRigBoneElement>([this, InReferenceSkeleton](FRigBoneElement* BoneElement) -> bool
	{
		if(BoneElement->BoneType == ERigBoneType::Imported)
		{
			const int32 BoneIndex = InReferenceSkeleton.FindBoneIndex(BoneElement->GetName());
			if (BoneIndex != INDEX_NONE)
			{
				const FTransform LocalInitialTransform = InReferenceSkeleton.GetRefBonePose()[BoneIndex];
				DynamicHierarchy->SetTransform(BoneElement, LocalInitialTransform, ERigTransformType::InitialLocal, true, false);
			}
		}
		return true;
	});
	bResetInitialTransformsBeforeSetup = false;
	RequestSetup();
}

void UControlRig::SetBoneInitialTransformsFromCompactPose(FCompactPose* InCompactPose)
{
	check(InCompactPose);

	if(!InCompactPose->IsValid())
	{
		return;
	}
	if(!InCompactPose->GetBoneContainer().IsValid())
	{
		return;
	}
	
	FMemMark Mark(FMemStack::Get());

	DynamicHierarchy->ForEach<FRigBoneElement>([this, InCompactPose](FRigBoneElement* BoneElement) -> bool
		{
			if (BoneElement->BoneType == ERigBoneType::Imported)
			{
				int32 MeshIndex = InCompactPose->GetBoneContainer().GetPoseBoneIndexForBoneName(BoneElement->GetName());
				if (MeshIndex != INDEX_NONE)
				{
					FCompactPoseBoneIndex CPIndex = InCompactPose->GetBoneContainer().MakeCompactPoseIndex(FMeshPoseBoneIndex(MeshIndex));
					if (CPIndex != INDEX_NONE)
					{
						FTransform LocalInitialTransform = InCompactPose->GetRefPose(CPIndex);
						DynamicHierarchy->SetTransform(BoneElement, LocalInitialTransform, ERigTransformType::InitialLocal, true, false);
					}
				}
			}
			return true;
		});

	bResetInitialTransformsBeforeSetup = false;
	RequestSetup();
}

const FRigControlElementCustomization* UControlRig::GetControlCustomization(const FRigElementKey& InControl) const
{
	check(InControl.Type == ERigElementType::Control);

	if(const FRigControlElementCustomization* Customization = ControlCustomizations.Find(InControl))
	{
		return Customization;
	}

	if(DynamicHierarchy)
	{
		if(const FRigControlElement* ControlElement = DynamicHierarchy->Find<FRigControlElement>(InControl))
		{
			return &ControlElement->Settings.Customization;
		}
	}

	return nullptr;
}

void UControlRig::SetControlCustomization(const FRigElementKey& InControl, const FRigControlElementCustomization& InCustomization)
{
	check(InControl.Type == ERigElementType::Control);
	
	ControlCustomizations.FindOrAdd(InControl) = InCustomization;
}

void UControlRig::PostInitInstanceIfRequired()
{
	if(GetHierarchy() == nullptr || VM == nullptr)
	{
		if(HasAnyFlags(RF_ClassDefaultObject))
		{
			PostInitInstance(nullptr);
		}
		else
		{
			UControlRig* CDO = GetClass()->GetDefaultObject<UControlRig>();
			PostInitInstance(CDO);
		}
	}
}

void UControlRig::PostInitInstance(UControlRig* InCDO)
{
	const EObjectFlags SubObjectFlags =
	HasAnyFlags(RF_ClassDefaultObject) ?
		RF_Public | RF_DefaultSubObject :
		RF_Transient | RF_Transactional;

	// set up the VM
	VM = NewObject<URigVM>(this, TEXT("VM"), SubObjectFlags);

	// Cooked platforms will load these pointers from disk
	if (!FPlatformProperties::RequiresCookedData())
	{
		VM->GetMemoryByType(ERigVMMemoryType::Work, true);
		VM->GetMemoryByType(ERigVMMemoryType::Literal, true);
		VM->GetMemoryByType(ERigVMMemoryType::Debug, true);
	}

	VM->ExecutionReachedExit().AddUObject(this, &UControlRig::HandleExecutionReachedExit);
	UpdateVMSettings();

	// set up the hierarchy
	DynamicHierarchy = NewObject<URigHierarchy>(this, TEXT("DynamicHierarchy"), SubObjectFlags);

#if WITH_EDITOR
	const TWeakObjectPtr<UControlRig> WeakThis = this;
	DynamicHierarchy->OnUndoRedo().AddStatic(&UControlRig::OnHierarchyTransformUndoRedoWeak, WeakThis);
#endif

	if(!HasAnyFlags(RF_ClassDefaultObject) && InCDO)
	{
		InCDO->PostInitInstanceIfRequired();
		VM->CopyFrom(InCDO->GetVM());
		DynamicHierarchy->CopyHierarchy(InCDO->GetHierarchy());
	}
	else // we are the CDO
	{
		// for default objects we need to check if the CDO is rooted. specialized Control Rigs
		// such as the FK control rig may not have a root since they are part of a C++ package.
		if(!IsRooted() && GetClass()->IsNative())
		{
			VM->AddToRoot();
			DynamicHierarchy->AddToRoot();
		}
	}
}

void UControlRig::OnHierarchyTransformUndoRedo(URigHierarchy* InHierarchy, const FRigElementKey& InKey, ERigTransformType::Type InTransformType, const FTransform& InTransform, bool bIsUndo)
{
	if(InKey.Type == ERigElementType::Control)
	{
		if(FRigControlElement* ControlElement = InHierarchy->Find<FRigControlElement>(InKey))
		{
			ControlModified().Broadcast(this, ControlElement, FRigControlModifiedContext(EControlRigSetKey::Never));
		}
	}
}

UControlRig::FPoseScope::FPoseScope(UControlRig* InControlRig, ERigElementType InFilter)
: ControlRig(InControlRig)
, Filter(InFilter)
{
	check(InControlRig);
	CachedPose = InControlRig->GetHierarchy()->GetPose(false, InFilter, FRigElementKeyCollection());
}

UControlRig::FPoseScope::~FPoseScope()
{
	check(ControlRig);

	ControlRig->GetHierarchy()->SetPose(CachedPose);
}

#if WITH_EDITOR

UControlRig::FTransientControlScope::FTransientControlScope(TObjectPtr<URigHierarchy> InHierarchy)
	:Hierarchy(InHierarchy)
{
	for (FRigControlElement* Control : Hierarchy->GetTransientControls())
	{
		FTransientControlInfo Info;
		Info.Name = Control->GetName();
		Info.Parent = Hierarchy->GetFirstParent(Control->GetKey());
		Info.Settings = Control->Settings;
		// preserve whatever value that was produced by this transient control at the moment
		Info.Value = Hierarchy->GetControlValue(Control->GetKey(),ERigControlValueType::Current);
		Info.OffsetTransform = Hierarchy->GetControlOffsetTransform(Control, ERigTransformType::CurrentLocal);
		Info.ShapeTransform = Hierarchy->GetControlShapeTransform(Control, ERigTransformType::CurrentLocal);
				
		SavedTransientControls.Add(Info);
	}
}

UControlRig::FTransientControlScope::~FTransientControlScope()
{
	URigHierarchyController* Controller = Hierarchy->GetController();
	for (const FTransientControlInfo& Info : SavedTransientControls)
	{
		Controller->AddControl(
            Info.Name,
            Info.Parent,
            Info.Settings,
            Info.Value,
            Info.OffsetTransform,
            Info.ShapeTransform,
            false,
            false
        );
	}
}

#endif
 
#undef LOCTEXT_NAMESPACE


