// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once
#include "UObject/Object.h"
#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "UObject/ScriptMacros.h"
#include "Engine/EngineBaseTypes.h"
#include "Templates/SubclassOf.h"
#include "ControlRigDefines.h"
#include "ControlRigGizmoLibrary.h"
#include "Rigs/RigHierarchy.h"
#include "Units/RigUnitContext.h"
#include "Animation/NodeMappingProviderInterface.h"
#include "Units/RigUnit.h"
#include "Units/Control/RigUnit_Control.h"
#include "RigVMCore/RigVM.h"
#include "Components/SceneComponent.h"
#include "Engine/AssetUserData.h"
#include "Interfaces/Interface_AssetUserData.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"

#if WITH_EDITOR
#include "RigVMModel/RigVMPin.h"
#include "RigVMTypeUtils.h"
#endif

#if WITH_EDITOR
#include "AnimPreviewInstance.h"
#endif 

#include "ControlRig.generated.h"

class IControlRigObjectBinding;
class UScriptStruct;
class USkeletalMesh;
class USkeletalMeshComponent;

struct FReferenceSkeleton;
struct FRigUnit;
struct FRigControl;

CONTROLRIG_API DECLARE_LOG_CATEGORY_EXTERN(LogControlRig, Log, All);

/** Runs logic for mapping input data to transforms (the "Rig") */
UCLASS(Blueprintable, Abstract, editinlinenew)
class CONTROLRIG_API UControlRig : public UObject, public INodeMappingProviderInterface, public IInterface_AssetUserData
{
	GENERATED_UCLASS_BODY()

	friend class UControlRigComponent;
	friend class SControlRigStackView;

public:

	/** Bindable event for external objects to contribute to / filter a control value */
	DECLARE_EVENT_ThreeParams(UControlRig, FFilterControlEvent, UControlRig*, FRigControlElement*, FRigControlValue&);

	/** Bindable event for external objects to be notified of Control changes */
	DECLARE_EVENT_ThreeParams(UControlRig, FControlModifiedEvent, UControlRig*, FRigControlElement*, const FRigControlModifiedContext&);

	/** Bindable event for external objects to be notified that a Control is Selected */
	DECLARE_EVENT_ThreeParams(UControlRig, FControlSelectedEvent, UControlRig*, FRigControlElement*, bool);

#if WITH_EDITOR
	/** Bindable event for external objects to be notified that a control rig is fully end-loaded*/
	DECLARE_EVENT_OneParam(UControlRig, FOnEndLoadPackage, UControlRig*);
#endif

	static const FName OwnerComponent;

	UFUNCTION(BlueprintCallable, Category = ControlRig)
	static TArray<UControlRig*> FindControlRigs(UObject* Outer, TSubclassOf<UControlRig> OptionalClass);

private:
	/** Current delta time */
	float DeltaTime;

	/** Current delta time */
	float AbsoluteTime;

	/** Current delta time */
	float FramesPerSecond;

	/** true if the rig itself should increase the AbsoluteTime */
	bool bAccumulateTime;

	/** Latest state being processed */
	EControlRigState LatestExecutedState;

	
public:
	virtual void Serialize(FArchive& Ar) override;
	virtual void PostLoad() override;
#if WITH_EDITOR

private:
	FOnEndLoadPackage EndLoadPackageEvent;

public:
	// these are needed so that sequencer can have a chance to update its 
	// ControlRig instances after the package is fully end-loaded
	void BroadCastEndLoadPackage() { EndLoadPackageEvent.Broadcast(this); }
	FOnEndLoadPackage& OnEndLoadPackage() { return EndLoadPackageEvent; }

	virtual void PostEditUndo() override;
#endif

	/** Is valid for execution */
	UFUNCTION(BlueprintPure, Category="Control Rig")
	virtual bool CanExecute() const;

	/** Gets the current absolute time */
	UFUNCTION(BlueprintPure, Category = "Control Rig")
	float GetAbsoluteTime() const { return AbsoluteTime; }

	/** Set the current delta time */
	UFUNCTION(BlueprintCallable, Category="Control Rig")
	void SetDeltaTime(float InDeltaTime);

	/** Set the current absolute time */
	UFUNCTION(BlueprintCallable, Category = "Control Rig")
	void SetAbsoluteTime(float InAbsoluteTime, bool InSetDeltaTimeZero = false);

	/** Set the current absolute and delta times */
	UFUNCTION(BlueprintCallable, Category = "Control Rig")
	void SetAbsoluteAndDeltaTime(float InAbsoluteTime, float InDeltaTime);

	/** Set the current fps */
	UFUNCTION(BlueprintCallable, Category = "Control Rig")
	void SetFramesPerSecond(float InFramesPerSecond);

	/** Returns the current frames per second (this may change over time) */
	UFUNCTION(BlueprintPure, Category = "Control Rig")
	float GetCurrentFramesPerSecond() const;

#if WITH_EDITOR
	/** Get the category of this ControlRig (for display in menus) */
	virtual FText GetCategory() const;

	/** Get the tooltip text to display for this node (displayed in graphs and from context menus) */
	virtual FText GetToolTipText() const;
#endif

	/** UObject interface */
	virtual UWorld* GetWorld() const override;

	/** Initialize things for the ControlRig */
	virtual void Initialize(bool bInitRigUnits = true);

	/** Evaluate at Any Thread */
	virtual void Evaluate_AnyThread();

	/** Returns the member properties as an external variable array */
	TArray<FRigVMExternalVariable> GetExternalVariables() const;

	/** Returns the public member properties as an external variable array */
	TArray<FRigVMExternalVariable> GetPublicVariables() const;

	/** Returns a public variable given its name */
	FRigVMExternalVariable GetPublicVariableByName(const FName& InVariableName) const;

	/** Returns the names of variables accessible in scripting */
	UFUNCTION(BlueprintPure, Category = "Control Rig", meta=(DisplayName="Get Variables"))
	TArray<FName> GetScriptAccessibleVariables() const;

	/** Returns the type of a given variable */
	UFUNCTION(BlueprintPure, Category = "Control Rig")
	FName GetVariableType(const FName& InVariableName) const;

	/** Returns the value of a given variable as a string */
	UFUNCTION(BlueprintPure, Category = "Control Rig")
	FString GetVariableAsString(const FName& InVariableName) const;

	/** Returns the value of a given variable as a string */
	UFUNCTION(BlueprintCallable, Category = "Control Rig")
	bool SetVariableFromString(const FName& InVariableName, const FString& InValue);

	template<class T>
	FORCEINLINE T GetPublicVariableValue(const FName& InVariableName)
	{
		FRigVMExternalVariable Variable = GetPublicVariableByName(InVariableName);
		if (Variable.IsValid())
		{
			return Variable.GetValue<T>();
		}
		return T();
	}

	template<class T>
	FORCEINLINE void SetPublicVariableValue(const FName& InVariableName, const T& InValue)
	{
		FRigVMExternalVariable Variable = GetPublicVariableByName(InVariableName);
		if (Variable.IsValid())
		{
			Variable.SetValue<T>(InValue);
		}
	}

	template<class T>
	FORCEINLINE bool SupportsEvent() const
	{
		return SupportsEvent(T::EventName);
	}

	UFUNCTION(BlueprintPure, Category = "Control Rig")
	bool SupportsEvent(const FName& InEventName) const;

	UFUNCTION(BlueprintPure, Category = "Control Rig")
	TArray<FName> GetSupportedEvents() const;

	/** Setup bindings to a runtime object (or clear by passing in nullptr). */
	FORCEINLINE void SetObjectBinding(TSharedPtr<IControlRigObjectBinding> InObjectBinding)
	{
		ObjectBinding = InObjectBinding;
	}

	FORCEINLINE TSharedPtr<IControlRigObjectBinding> GetObjectBinding() const
	{
		return ObjectBinding;
	}

	virtual FString GetName() const
	{
		FString ObjectName = (GetClass()->GetName());
		ObjectName.RemoveFromEnd(TEXT("_C"));
		return ObjectName;
	}

	UFUNCTION(BlueprintPure, Category = "Control Rig")
	FORCEINLINE_DEBUGGABLE URigHierarchy* GetHierarchy()
	{
		return DynamicHierarchy;
	}

#if WITH_EDITOR

	// called after post reinstance when compilng blueprint by Sequencer
	void PostReinstanceCallback(const UControlRig* Old);

#endif // WITH_EDITOR
	
	// BEGIN UObject interface
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);
	virtual void BeginDestroy() override;
	// END UObject interface

	UPROPERTY(transient)
	ERigExecutionType ExecutionType;

	UPROPERTY()
	FRigVMRuntimeSettings VMRuntimeSettings;

	/** Execute */
	UFUNCTION(BlueprintCallable, Category = "Control Rig")
	void Execute(const EControlRigState State, const FName& InEventName);

	/** ExecuteUnits */
	virtual void ExecuteUnits(FRigUnitContext& InOutContext, const FName& InEventName);

	/** Requests to perform an init during the next execution */
	UFUNCTION(BlueprintCallable, Category = "Control Rig")
	void RequestInit();

	/** Requests to perform a setup during the next execution */
	UFUNCTION(BlueprintCallable, Category = "Control Rig")
	void RequestSetup();

	/** Returns the queue of events to run */
	const TArray<FName>& GetEventQueue() const { return EventQueue; }

	/** Sets the queue of events to run */
	void SetEventQueue(const TArray<FName>& InEventNames);

	/** Update the settings such as array bound and log facilities */
	void UpdateVMSettings();

	UFUNCTION(BlueprintPure, Category = "Control Rig")
	URigVM* GetVM();

	/** INodeMappingInterface implementation */
	virtual void GetMappableNodeData(TArray<FName>& OutNames, TArray<FNodeItem>& OutNodeItems) const override;

	/** Data Source Registry Getter */
	UAnimationDataSourceRegistry* GetDataSourceRegistry();

	virtual TArray<FRigControlElement*> AvailableControls() const;
	virtual FRigControlElement* FindControl(const FName& InControlName) const;
	virtual bool ShouldApplyLimits() const { return !bSetupModeEnabled; }
	virtual bool IsSetupModeEnabled() const { return bSetupModeEnabled; }
	virtual FTransform SetupControlFromGlobalTransform(const FName& InControlName, const FTransform& InGlobalTransform);
	virtual FTransform GetControlGlobalTransform(const FName& InControlName) const;

	// Sets the relative value of a Control
	template<class T>
	FORCEINLINE_DEBUGGABLE void SetControlValue(const FName& InControlName, T InValue, bool bNotify = true,
		const FRigControlModifiedContext& Context = FRigControlModifiedContext(), bool bSetupUndo = true, bool bPrintPythonCommnds = false)
	{
		SetControlValueImpl(InControlName, FRigControlValue::Make<T>(InValue), bNotify, Context, bSetupUndo, bPrintPythonCommnds);
	}

	// Returns the value of a Control
	FORCEINLINE_DEBUGGABLE FRigControlValue GetControlValue(const FName& InControlName)
	{
		const FRigElementKey Key(InControlName, ERigElementType::Control);
		return DynamicHierarchy->GetControlValue(Key);
	}

	// Sets the relative value of a Control
	FORCEINLINE_DEBUGGABLE virtual void SetControlValueImpl(const FName& InControlName, const FRigControlValue& InValue, bool bNotify = true,
		const FRigControlModifiedContext& Context = FRigControlModifiedContext(), bool bSetupUndo = true, bool bPrintPythonCommnds = false)
	{
		const FRigElementKey Key(InControlName, ERigElementType::Control);

		FRigControlElement* ControlElement = DynamicHierarchy->Find<FRigControlElement>(Key);
		if(ControlElement == nullptr)
		{
			return;
		}

		DynamicHierarchy->SetControlValue(ControlElement, InValue, ERigControlValueType::Current, bSetupUndo, false, bPrintPythonCommnds);

		if (bNotify && OnControlModified.IsBound())
		{
			OnControlModified.Broadcast(this, ControlElement, Context);
		}
	}

	bool SetControlGlobalTransform(const FName& InControlName, const FTransform& InGlobalTransform, bool bNotify = true, const FRigControlModifiedContext& Context = FRigControlModifiedContext(), bool bSetupUndo = true, bool bPrintPythonCommands = false);

	virtual FRigControlValue GetControlValueFromGlobalTransform(const FName& InControlName, const FTransform& InGlobalTransform);

	virtual void SetControlLocalTransform(const FName& InControlName, const FTransform& InLocalTransform, bool bNotify = true, const FRigControlModifiedContext& Context = FRigControlModifiedContext(), bool bSetupUndo = true);
	virtual FTransform GetControlLocalTransform(const FName& InControlName) ;

	virtual const TArray<TSoftObjectPtr<UControlRigShapeLibrary>>& GetShapeLibraries() const;
	virtual void CreateRigControlsForCurveContainer();
	virtual void GetControlsInOrder(TArray<FRigControlElement*>& SortedControls) const;

	UFUNCTION(BlueprintCallable, Category = "Control Rig")
	virtual void SelectControl(const FName& InControlName, bool bSelect = true);
	UFUNCTION(BlueprintCallable, Category = "Control Rig")
	virtual bool ClearControlSelection();
	UFUNCTION(BlueprintPure, Category = "Control Rig")
	virtual TArray<FName> CurrentControlSelection() const;
	UFUNCTION(BlueprintPure, Category = "Control Rig")
	virtual bool IsControlSelected(const FName& InControlName)const;

	// Returns true if this manipulatable subject is currently
	// available for manipulation / is enabled.
	virtual bool ManipulationEnabled() const
	{
		return bManipulationEnabled;
	}

	// Sets the manipulatable subject to enabled or disabled
	virtual bool SetManipulationEnabled(bool Enabled = true)
	{
		if (bManipulationEnabled == Enabled)
		{
			return false;
		}
		bManipulationEnabled = Enabled;
		return true;
	}

	// Returns a event that can be used to subscribe to
	// filtering control data when needed
	FFilterControlEvent& ControlFilter() { return OnFilterControl; }

	// Returns a event that can be used to subscribe to
	// change notifications coming from the manipulated subject.
	FControlModifiedEvent& ControlModified() { return OnControlModified; }

	// Returns a event that can be used to subscribe to
	// selection changes coming from the manipulated subject.
	FControlSelectedEvent& ControlSelected() { return OnControlSelected; }

	bool IsCurveControl(const FRigControlElement* InControlElement) const;

	DECLARE_EVENT_ThreeParams(UControlRig, FControlRigExecuteEvent, class UControlRig*, const EControlRigState, const FName&);
	FControlRigExecuteEvent& OnInitialized_AnyThread() { return InitializedEvent; }
	FControlRigExecuteEvent& OnPreSetup_AnyThread() { return PreSetupEvent; }
	FControlRigExecuteEvent& OnPostSetup_AnyThread() { return PostSetupEvent; }
	FControlRigExecuteEvent& OnPreForwardsSolve_AnyThread() { return PreForwardsSolveEvent; }
	FControlRigExecuteEvent& OnPostForwardsSolve_AnyThread() { return PostForwardsSolveEvent; }
	FControlRigExecuteEvent& OnExecuted_AnyThread() { return ExecutedEvent; }
	FRigEventDelegate& OnRigEvent_AnyThread() { return RigEventDelegate; }

	// Setup the initial transform / ref pose of the bones based upon an anim instance
	// This uses the current refpose instead of the RefSkeleton pose.
	virtual void SetBoneInitialTransformsFromAnimInstance(UAnimInstance* InAnimInstance);

	// Setup the initial transform / ref pose of the bones based upon an anim instance proxy
	// This uses the current refpose instead of the RefSkeleton pose.
	virtual void SetBoneInitialTransformsFromAnimInstanceProxy(const FAnimInstanceProxy* InAnimInstanceProxy);

	// Setup the initial transform / ref pose of the bones based upon skeletal mesh component (ref skeleton)
	// This uses the RefSkeleton pose instead of the current refpose (or vice versae is bUseAnimInstance == true)
	virtual void SetBoneInitialTransformsFromSkeletalMeshComponent(USkeletalMeshComponent* InSkelMeshComp, bool bUseAnimInstance = false);

	// Setup the initial transforms / ref pose of the bones based on a skeletal mesh
	// This uses the RefSkeleton pose instead of the current refpose.
	virtual void SetBoneInitialTransformsFromSkeletalMesh(USkeletalMesh* InSkeletalMesh);

	// Setup the initial transforms / ref pose of the bones based on a reference skeleton
	// This uses the RefSkeleton pose instead of the current refpose.
	virtual void SetBoneInitialTransformsFromRefSkeleton(const FReferenceSkeleton& InReferenceSkeleton);

private:

	void SetBoneInitialTransformsFromCompactPose(FCompactPose* InCompactPose);

public:
	
	const FControlRigDrawInterface& GetDrawInterface() const { return DrawInterface; };
	FControlRigDrawInterface& GetDrawInterface() { return DrawInterface; };

	const FControlRigDrawContainer& GetDrawContainer() const { return DrawContainer; };
	FControlRigDrawContainer& GetDrawContainer() { return DrawContainer; };

	const FRigControlElementCustomization* GetControlCustomization(const FRigElementKey& InControl) const;
	void SetControlCustomization(const FRigElementKey& InControl, const FRigControlElementCustomization& InCustomization);

	void PostInitInstanceIfRequired();

private:

	void PostInitInstance(UControlRig* InCDO);

	UPROPERTY()
	TMap<FRigElementKey, FRigControlElementCustomization> ControlCustomizations;

	UPROPERTY()
	TObjectPtr<URigVM> VM;

	UPROPERTY()
	TObjectPtr<URigHierarchy> DynamicHierarchy;

	UPROPERTY()
	TSoftObjectPtr<UControlRigShapeLibrary> GizmoLibrary_DEPRECATED;

	UPROPERTY()
	TArray<TSoftObjectPtr<UControlRigShapeLibrary>> ShapeLibraries;

	/** Runtime object binding */
	TSharedPtr<IControlRigObjectBinding> ObjectBinding;

#if WITH_EDITOR
	FControlRigLog* ControlRigLog;
	bool bEnableControlRigLogging;
#endif

	// you either go Input or Output, currently if you put it in both place, Output will override
	UPROPERTY()
	TMap<FName, FCachedPropertyPath> InputProperties_DEPRECATED;

	UPROPERTY()
	TMap<FName, FCachedPropertyPath> OutputProperties_DEPRECATED;

	FRigNameCache NameCache;

private:
	
	void HandleOnControlModified(UControlRig* Subject, FRigControlElement* Control, const FRigControlModifiedContext& Context);

	void HandleExecutionReachedExit(const FName& InEventName);
	
	TArray<FRigVMExternalVariable> GetExternalVariablesImpl(bool bFallbackToBlueprint) const;

	FORCEINLINE FProperty* GetPublicVariableProperty(const FName& InVariableName) const
	{
		if (FProperty* Property = GetClass()->FindPropertyByName(InVariableName))
		{
			if (!Property->IsNative())
			{
				if (!Property->HasAllPropertyFlags(CPF_DisableEditOnInstance))
				{
					return Property;
				}
			}
		}
		return nullptr;
	}

private:

	UPROPERTY()
	FControlRigDrawContainer DrawContainer;

	/** The draw interface for the units to use */
	FControlRigDrawInterface DrawInterface;

	/** The registry to access data source */
	UPROPERTY(transient)
	TObjectPtr<UAnimationDataSourceRegistry> DataSourceRegistry;

	/** The event name used during an update */
	UPROPERTY(transient)
	TArray<FName> EventQueue;

	/** Copy the VM from the default object */
	void InstantiateVMFromCDO();
	
	/** Copy the default values of external variables from the default object */
	void CopyExternalVariableDefaultValuesFromCDO();
	
	/** Broadcasts a notification whenever the controlrig's memory is initialized. */
	FControlRigExecuteEvent InitializedEvent;

	/** Broadcasts a notification just before the controlrig is setup. */
	FControlRigExecuteEvent PreSetupEvent;

	/** Broadcasts a notification whenever the controlrig has been setup. */
	FControlRigExecuteEvent PostSetupEvent;

	/** Broadcasts a notification before a forward solve has been initiated */
	FControlRigExecuteEvent PreForwardsSolveEvent;
	
	/** Broadcasts a notification after a forward solve has been initiated */
	FControlRigExecuteEvent PostForwardsSolveEvent;
	
	/** Broadcasts a notification whenever the controlrig is executed / updated. */
	FControlRigExecuteEvent ExecutedEvent;

	/** Handle changes within the hierarchy */
	void HandleHierarchyModified(ERigHierarchyNotification InNotification, URigHierarchy* InHierarchy, const FRigBaseElement* InElement);

#if WITH_EDITOR
	/** Remove a transient / temporary control used to interact with a pin */
	FName AddTransientControl(URigVMPin* InPin, FRigElementKey SpaceKey = FRigElementKey(), FTransform OffsetTransform = FTransform::Identity);

	/** Sets the value of a transient control based on a pin */
	bool SetTransientControlValue(URigVMPin* InPin);

	/** Remove a transient / temporary control used to interact with a pin */
	FName RemoveTransientControl(URigVMPin* InPin);

	FName AddTransientControl(const FRigElementKey& InElement);

	/** Sets the value of a transient control based on a bone */
	bool SetTransientControlValue(const FRigElementKey& InElement);

	/** Remove a transient / temporary control used to interact with a bone */
	FName RemoveTransientControl(const FRigElementKey& InElement);

	static FName GetNameForTransientControl(const FRigElementKey& InElement);
	FName GetNameForTransientControl(URigVMPin* InPin) const;
	static FString GetPinNameFromTransientControl(const FRigElementKey& InKey);
	static FRigElementKey GetElementKeyFromTransientControl(const FRigElementKey& InKey);

	/** Removes all  transient / temporary control used to interact with pins */
	void ClearTransientControls();

	UAnimPreviewInstance* PreviewInstance;

	// this is needed because PreviewInstance->ModifyBone(...) cannot modify user created bones,
	TMap<FName, FTransform> TransformOverrideForUserCreatedBones;
	
public:
	
	void ApplyTransformOverrideForUserCreatedBones();
	
#endif

private: 

	void HandleHierarchyEvent(URigHierarchy* InHierarchy, const FRigEventContext& InEvent);
	FRigEventDelegate RigEventDelegate;

	void InitializeFromCDO();

	UPROPERTY()
	FRigInfluenceMapPerEvent Influences;

	const FRigInfluenceMap* FindInfluenceMap(const FName& InEventName);

	UPROPERTY(transient, BlueprintGetter = GetInteractionRig, BlueprintSetter = SetInteractionRig, Category = "Interaction")
	TObjectPtr<UControlRig> InteractionRig;

	UPROPERTY(EditInstanceOnly, transient, BlueprintGetter = GetInteractionRigClass, BlueprintSetter = SetInteractionRigClass, Category = "Interaction", Meta=(DisplayName="Interaction Rig"))
	TSubclassOf<UControlRig> InteractionRigClass;

public:

	UFUNCTION(BlueprintGetter)
	UControlRig* GetInteractionRig() const { return InteractionRig; }

	UFUNCTION(BlueprintSetter)
	void SetInteractionRig(UControlRig* InInteractionRig);

	UFUNCTION(BlueprintGetter)
	TSubclassOf<UControlRig> GetInteractionRigClass() const { return InteractionRigClass; }

	UFUNCTION(BlueprintSetter)
	void SetInteractionRigClass(TSubclassOf<UControlRig> InInteractionRigClass);

	// UObject interface
#if WITH_EDITOR
	virtual void PreEditChange(FProperty* PropertyAboutToChange) override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent) override;
#endif

	float GetDebugBoneRadiusMultiplier() const { return DebugBoneRadiusMultiplier; }

public:
	//~ Begin IInterface_AssetUserData Interface
	virtual void AddAssetUserData(UAssetUserData* InUserData) override;
	virtual void RemoveUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass) override;
	virtual UAssetUserData* GetAssetUserDataOfClass(TSubclassOf<UAssetUserData> InUserDataClass) override;
	virtual const TArray<UAssetUserData*>* GetAssetUserDataArray() const override;
	//~ End IInterface_AssetUserData Interface
protected
	:
	/** Array of user data stored with the asset */
	UPROPERTY(EditAnywhere, AdvancedDisplay, Instanced, Category = "Default")
	TArray<TObjectPtr<UAssetUserData>> AssetUserData;

private:

	void CopyPoseFromOtherRig(UControlRig* Subject);
	void HandleInteractionRigControlModified(UControlRig* Subject, FRigControlElement* Control, const FRigControlModifiedContext& Context);
	void HandleInteractionRigInitialized(UControlRig* Subject, EControlRigState State, const FName& EventName);
	void HandleInteractionRigExecuted(UControlRig* Subject, EControlRigState State, const FName& EventName);
	void HandleInteractionRigControlSelected(UControlRig* Subject, FRigControlElement* InControl, bool bSelected, bool bInverted);


protected:
	bool bRequiresInitExecution;
	bool bRequiresSetupEvent;
	bool bSetupModeEnabled;
	bool bCopyHierarchyBeforeSetup;
	bool bResetInitialTransformsBeforeSetup;
	bool bManipulationEnabled;

	int32 InitBracket;
	int32 UpdateBracket;
	int32 PreSetupBracket;
	int32 PostSetupBracket;
	int32 PreForwardsSolveBracket;
	int32 PostForwardsSolveBracket;
	int32 InteractionBracket;
	int32 InterRigSyncBracket;

	TWeakObjectPtr<USceneComponent> OuterSceneComponent;

	FORCEINLINE bool IsInitializing() const
	{
		return InitBracket > 0;
	}

	FORCEINLINE bool IsExecuting() const
	{
		return UpdateBracket > 0;
	}

	FORCEINLINE bool IsRunningPreSetup() const
	{
		return PreSetupBracket > 0;
	}

	FORCEINLINE bool IsRunningPostSetup() const
	{
		return PostSetupBracket > 0;
	}

	FORCEINLINE bool IsInteracting() const
	{
		return InteractionBracket > 0;
	}

	FORCEINLINE bool IsSyncingWithOtherRig() const
	{
		return InterRigSyncBracket > 0;
	}

#if WITH_EDITOR
	FORCEINLINE static void OnHierarchyTransformUndoRedoWeak(URigHierarchy* InHierarchy, const FRigElementKey& InKey, ERigTransformType::Type InTransformType, const FTransform& InTransform, bool bIsUndo, TWeakObjectPtr<UControlRig> WeakThis)
	{
		if(WeakThis.IsValid() && InHierarchy != nullptr)
		{
			WeakThis->OnHierarchyTransformUndoRedo(InHierarchy, InKey, InTransformType, InTransform, bIsUndo);
		}
	}
#endif
	
	void OnHierarchyTransformUndoRedo(URigHierarchy* InHierarchy, const FRigElementKey& InKey, ERigTransformType::Type InTransformType, const FTransform& InTransform, bool bIsUndo);

	FFilterControlEvent OnFilterControl;
	FControlModifiedEvent OnControlModified;
	FControlSelectedEvent OnControlSelected;

	TArray<FRigElementKey> QueuedModifiedControls;

private:

#if WITH_EDITORONLY_DATA

	UPROPERTY(transient)
	TObjectPtr<URigVM> VMSnapshotBeforeExecution;

	/** The current execution mode */
	UPROPERTY(transient)
	bool bIsInDebugMode;

#endif

	float DebugBoneRadiusMultiplier;
	
#if WITH_EDITOR	

	FRigVMDebugInfo DebugInfo;
	TMap<FString, bool> LoggedMessages;
	void LogOnce(EMessageSeverity::Type InSeverity, int32 InInstructionIndex, const FString& InMessage);
	
public:

	void SetIsInDebugMode(const bool bValue) { bIsInDebugMode = bValue; }
	bool IsInDebugMode() const { return bIsInDebugMode; }
	
	/** Adds a breakpoint in the VM at the InstructionIndex for the Node */
	void AddBreakpoint(int32 InstructionIndex, URigVMNode* InNode, uint16 InDepth);

	/** If the VM is halted at a breakpoint, it sets a breakpoint action so that
	 *  it is applied on the next VM execution */
	bool ExecuteBreakpointAction(const ERigVMBreakpointAction BreakpointAction);
	
	FRigVMDebugInfo& GetDebugInfo() { return DebugInfo; }

	/** Creates the snapshot VM if required and returns it */
	URigVM* GetSnapshotVM(bool bCreateIfNeeded = true);
#endif	

private:

	// Class used to temporarily cache current pose of the hierarchy
	// restore it on destruction, similar to UControlRigBlueprint::FControlValueScope
	class FPoseScope
	{
	public:
		FPoseScope(UControlRig* InControlRig, ERigElementType InFilter = ERigElementType::All);
		~FPoseScope();

	private:

		UControlRig* ControlRig;
		ERigElementType Filter;
		FRigPose CachedPose;
	};

#if WITH_EDITOR

	// Class used to temporarily cache current transient controls
	// restore them after a CopyHierarchy call
	class FTransientControlScope
	{
	public:
		FTransientControlScope(TObjectPtr<URigHierarchy> InHierarchy);
		~FTransientControlScope();
	
	private:
		// used to match URigHierarchyController::AddControl(...)
		struct FTransientControlInfo
		{
			FName Name;
			// transient control should only have 1 parent, with weight = 1.0
			FRigElementKey Parent;
			FRigControlSettings Settings;
			FRigControlValue Value;
			FTransform OffsetTransform;
			FTransform ShapeTransform;
		};
		
		TArray<FTransientControlInfo> SavedTransientControls;
		TObjectPtr<URigHierarchy> Hierarchy;
	};

public:
	// Class used to temporarily cache current pose of transient controls
	// restore them after a ResetPoseToInitial call,
	// which allows user to move bones in setup mode
	class FTransientControlPoseScope
	{
	public:
		FTransientControlPoseScope(TObjectPtr<UControlRig> InControlRig)
		{
			ControlRig = InControlRig;

			TArray<FRigControlElement*> TransientControls = ControlRig->GetHierarchy()->GetTransientControls();
			TArray<FRigElementKey> Keys;
			for(FRigControlElement* TransientControl : TransientControls)
			{
				Keys.Add(TransientControl->GetKey());
			}
	
			CachedPose = ControlRig->GetHierarchy()->GetPose(false, ERigElementType::Control, TArrayView<FRigElementKey>(Keys));
		}
		~FTransientControlPoseScope()
		{
			check(ControlRig);

			ControlRig->GetHierarchy()->SetPose(CachedPose);
		}
	
	private:
		
		UControlRig* ControlRig;
		FRigPose CachedPose;	
	};	

private:
	
#endif

	friend class FControlRigBlueprintCompilerContext;
	friend struct FRigHierarchyRef;
	friend class FControlRigEditor;
	friend class SRigCurveContainer;
	friend class SRigHierarchy;
	friend class UEngineTestControlRig;
 	friend class FControlRigEditMode;
	friend class UControlRigBlueprint;
	friend class UControlRigComponent;
	friend class UControlRigBlueprintGeneratedClass;
	friend class FControlRigInteractionScope;
	friend class UControlRigValidator;
	friend struct FAnimNode_ControlRig;
	friend class URigHierarchy;
};

class CONTROLRIG_API FControlRigBracketScope
{
public:

	FORCEINLINE FControlRigBracketScope(int32& InBracket)
		: Bracket(InBracket)
	{
		Bracket++;
	}

	FORCEINLINE ~FControlRigBracketScope()
	{
		Bracket--;
	}

private:

	int32& Bracket;
};

class CONTROLRIG_API FControlRigInteractionScope
{
public:

	FORCEINLINE_DEBUGGABLE FControlRigInteractionScope(UControlRig* InControlRig)
		: ControlRig(InControlRig)
		, InteractionBracketScope(InControlRig->InteractionBracket)
		, SyncBracketScope(InControlRig->InterRigSyncBracket)
	{
		InControlRig->GetHierarchy()->StartInteraction();
	}

	FORCEINLINE_DEBUGGABLE ~FControlRigInteractionScope()
	{
		if(ensure(ControlRig.IsValid()))
		{
			ControlRig->GetHierarchy()->EndInteraction();
		}
	}

private:

	TWeakObjectPtr<UControlRig> ControlRig;
	FControlRigBracketScope InteractionBracketScope;
	FControlRigBracketScope SyncBracketScope;
};
