// Copyright Epic Games, Inc. All Rights Reserved.

#include "RigVMCore/RigVM.h"
#include "UObject/Package.h"
#include "UObject/AnimObjectVersion.h"
#include "UObject/UE5MainStreamObjectVersion.h"
#include "HAL/PlatformTLS.h"
#include "UObject/UObjectHash.h"


void FRigVMParameter::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FAnimObjectVersion::GUID);

	if (Ar.CustomVer(FAnimObjectVersion::GUID) < FAnimObjectVersion::StoreMarkerNamesOnSkeleton)
	{
		return;
	}

	if (Ar.IsSaving() || Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		Save(Ar);
	}
	else if (Ar.IsLoading())
	{
		Load(Ar);
	}
	else
	{
		// remove due to FPIEFixupSerializer hitting this checkNoEntry();
	}
}

void FRigVMParameter::Save(FArchive& Ar)
{
	Ar << Type;
	Ar << Name;
	Ar << RegisterIndex;
	Ar << CPPType;
	Ar << ScriptStructPath;
}

void FRigVMParameter::Load(FArchive& Ar)
{
	Ar << Type;
	Ar << Name;
	Ar << RegisterIndex;
	Ar << CPPType;
	Ar << ScriptStructPath;

	ScriptStruct = nullptr;
}

UScriptStruct* FRigVMParameter::GetScriptStruct() const
{
	if (ScriptStruct == nullptr)
	{
		if (ScriptStructPath != NAME_None)
		{
			FRigVMParameter* MutableThis = (FRigVMParameter*)this;
			MutableThis->ScriptStruct = FindObject<UScriptStruct>(ANY_PACKAGE, *ScriptStructPath.ToString());
		}
	}
	return ScriptStruct;
}

URigVM::URigVM()
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
    : WorkMemoryPtr(&WorkMemoryStorage)
    , LiteralMemoryPtr(&LiteralMemoryStorage)
    , DebugMemoryPtr(&DebugMemoryStorage)
	, WorkMemoryStorageObject(nullptr)
#else
	: WorkMemoryStorageObject(nullptr)
#endif
	, LiteralMemoryStorageObject(nullptr)
	, DebugMemoryStorageObject(nullptr)
	, ByteCodePtr(&ByteCodeStorage)
	, NumExecutions(0)
#if WITH_EDITOR
	, DebugInfo(nullptr)
	, HaltedAtBreakpoint(nullptr)
	, HaltedAtBreakpointHit(INDEX_NONE)
#endif
    , FunctionNamesPtr(&FunctionNamesStorage)
    , FunctionsPtr(&FunctionsStorage)
#if WITH_EDITOR
	, FirstEntryEventInQueue(NAME_None)
#endif
	, ExecutingThreadId(INDEX_NONE)
	, DeferredVMToCopy(nullptr)
{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	GetWorkMemory().SetMemoryType(ERigVMMemoryType::Work);
	GetLiteralMemory().SetMemoryType(ERigVMMemoryType::Literal);
	GetDebugMemory().SetMemoryType(ERigVMMemoryType::Debug);
#endif
}

URigVM::~URigVM()
{
	Reset();

	ExecutionReachedExit().Clear();
#if WITH_EDITOR
	ExecutionHalted().Clear();
#endif
}

void URigVM::Serialize(FArchive& Ar)
{
	Ar.UsingCustomVersion(FAnimObjectVersion::GUID);
	Ar.UsingCustomVersion(FUE5MainStreamObjectVersion::GUID);

	if (Ar.CustomVer(FAnimObjectVersion::GUID) < FAnimObjectVersion::StoreMarkerNamesOnSkeleton)
	{
		return;
	}

#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

	// call into the super class to serialize any uproperty
	if(Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		Super::Serialize(Ar);
	}
	
#endif

	ensure(ExecutingThreadId == INDEX_NONE);

	if (Ar.IsSaving() || Ar.IsObjectReferenceCollector() || Ar.IsCountingMemory())
	{
		Save(Ar);
	}
	else if (Ar.IsLoading())
	{
		Load(Ar);
	}
	else
	{
		// remove due to FPIEFixupSerializer hitting this checkNoEntry();
	}
}

void URigVM::Save(FArchive& Ar)
{
	CopyDeferredVMIfRequired();

	int32 RigVMUClassBasedStorageDefine = UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED;
	Ar << RigVMUClassBasedStorageDefine;

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	
	Ar << WorkMemoryStorage;
	Ar << LiteralMemoryStorage;

#else

	if(!Ar.IsIgnoringArchetypeRef())
	{
		Ar << ExternalPropertyPathDescriptions;
	}
	
#endif

	// we rely on Ar.IsIgnoringArchetypeRef for determining if we are currently performing
	// CPFUO (Copy Properties for unrelated objects). During a reinstance pass we don't
	// want to overwrite the bytecode and some other properties - since that's handled already
	// by the RigVMCompiler.
	if(!Ar.IsIgnoringArchetypeRef())
	{
		Ar << FunctionNamesStorage;
		Ar << ByteCodeStorage;
		Ar << Parameters;
	}
}

void URigVM::Load(FArchive& Ar)
{
	// we rely on Ar.IsIgnoringArchetypeRef for determining if we are currently performing
	// CPFUO (Copy Properties for unrelated objects). During a reinstance pass we don't
	// want to overwrite the bytecode and some other properties - since that's handled already
	// by the RigVMCompiler.
	Reset(Ar.IsIgnoringArchetypeRef());

	int32 RigVMUClassBasedStorageDefine = 1;
	if (Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) >= FUE5MainStreamObjectVersion::RigVMMemoryStorageObject)
	{
		Ar << RigVMUClassBasedStorageDefine;
	}

	if(RigVMUClassBasedStorageDefine == 1)
	{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
		FRigVMMemoryContainer WorkMemoryStorage;
		FRigVMMemoryContainer LiteralMemoryStorage;
#endif
		
		Ar << WorkMemoryStorage;
		Ar << LiteralMemoryStorage;
		Ar << FunctionNamesStorage;
		Ar << ByteCodeStorage;
		Ar << Parameters;
		
		if (Ar.CustomVer(FUE5MainStreamObjectVersion::GUID) < FUE5MainStreamObjectVersion::RigVMCopyOpStoreNumBytes)
		{
			Reset();
			return;
		}
	}

	if(RigVMUClassBasedStorageDefine != UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED)
	{
		Reset();
		return;
	}

#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

	// requesting the memory types will create them
	// Cooked platforms will just load the objects and do no need to clear the referenes
	if (!FPlatformProperties::RequiresCookedData())
	{
		ClearMemory();
	}

	if(!Ar.IsIgnoringArchetypeRef())
	{
		Ar << ExternalPropertyPathDescriptions;
		Ar << FunctionNamesStorage;
		Ar << ByteCodeStorage;
		Ar << Parameters;
	}

#endif

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	
	if (WorkMemoryStorage.bEncounteredErrorDuringLoad ||
		LiteralMemoryStorage.bEncounteredErrorDuringLoad ||
		!ValidateAllOperandsDuringLoad())
	{
		Reset();
	}
	else
	{
		Instructions.Reset();
		FunctionsStorage.Reset();
		ParametersNameMap.Reset();

		for (int32 Index = 0; Index < Parameters.Num(); Index++)
		{
			ParametersNameMap.Add(Parameters[Index].Name, Index);
		}

		// rebuild the bytecode to adjust for byte shifts in shipping
		RebuildByteCodeOnLoad();

		InvalidateCachedMemory();
	}

#endif
}

void URigVM::PostLoad()
{
	Super::PostLoad();
	
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

	ClearMemory();

	TArray<ERigVMMemoryType> MemoryTypes;
	MemoryTypes.Add(ERigVMMemoryType::Literal);
	MemoryTypes.Add(ERigVMMemoryType::Work);
	MemoryTypes.Add(ERigVMMemoryType::Debug);

	for(ERigVMMemoryType MemoryType : MemoryTypes)
	{
		if(URigVMMemoryStorageGeneratorClass* Class = URigVMMemoryStorageGeneratorClass::GetStorageClass(this, MemoryType))
		{
			if(Class->LinkedProperties.Num() == 0)
			{
				Class->RefreshLinkedProperties();
			}
			if(Class->PropertyPathDescriptions.Num() != Class->PropertyPaths.Num())
			{
				Class->RefreshPropertyPaths();
			}
		}
	}
	
	RefreshExternalPropertyPaths();

	if (!ValidateAllOperandsDuringLoad())
	{
		Reset();
	}
	else
	{
		Instructions.Reset();
		FunctionsStorage.Reset();
		ParametersNameMap.Reset();

		for (int32 Index = 0; Index < Parameters.Num(); Index++)
		{
			ParametersNameMap.Add(Parameters[Index].Name, Index);
		}

		// rebuild the bytecode to adjust for byte shifts in shipping
		RebuildByteCodeOnLoad();

		InvalidateCachedMemory();
	}

#endif
}

bool URigVM::ValidateAllOperandsDuringLoad()
{
	// check all operands on all ops for validity
	bool bAllOperandsValid = true;

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

	FRigVMMemoryContainer* LocalMemory[] = { &WorkMemoryStorage, &LiteralMemoryStorage, &DebugMemoryStorage };
	
#else

	TArray<URigVMMemoryStorage*> LocalMemory = { GetWorkMemory(), GetLiteralMemory(), GetDebugMemory() };
	
#endif
	
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	auto CheckOperandValidity = [LocalMemory, &bAllOperandsValid](const FRigVMOperand& InOperand) -> bool
#else
	auto CheckOperandValidity = [LocalMemory, &bAllOperandsValid](const FRigVMOperand& InOperand) -> bool
#endif
	{
		if(InOperand.GetContainerIndex() < 0 || InOperand.GetContainerIndex() >= (int32)ERigVMMemoryType::Invalid)
		{
			bAllOperandsValid = false;
			return false;
		}


#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
		const FRigVMMemoryContainer* MemoryForOperand = LocalMemory[InOperand.GetContainerIndex()];
#else
		const URigVMMemoryStorage* MemoryForOperand = LocalMemory[InOperand.GetContainerIndex()];
#endif

		if(InOperand.GetMemoryType() != ERigVMMemoryType::External)
		{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
			if(!MemoryForOperand->Registers.IsValidIndex(InOperand.GetRegisterIndex()))
#else
			if(!MemoryForOperand->IsValidIndex(InOperand.GetRegisterIndex()))
#endif
			{
				bAllOperandsValid = false;
				return false;
			}
		}

		if(InOperand.GetRegisterOffset() != INDEX_NONE)
		{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
			if(!MemoryForOperand->RegisterOffsets.IsValidIndex(InOperand.GetRegisterOffset()))
#else
			if(!MemoryForOperand->GetPropertyPaths().IsValidIndex(InOperand.GetRegisterOffset()))
#endif
			{
				bAllOperandsValid = false;
				return false;
			}
		}

		return true;
	};
	
	const FRigVMInstructionArray ByteCodeInstructions = ByteCodeStorage.GetInstructions();
	for(const FRigVMInstruction& ByteCodeInstruction : ByteCodeInstructions)
	{
		switch (ByteCodeInstruction.OpCode)
		{
			case ERigVMOpCode::Execute_0_Operands:
			case ERigVMOpCode::Execute_1_Operands:
			case ERigVMOpCode::Execute_2_Operands:
			case ERigVMOpCode::Execute_3_Operands:
			case ERigVMOpCode::Execute_4_Operands:
			case ERigVMOpCode::Execute_5_Operands:
			case ERigVMOpCode::Execute_6_Operands:
			case ERigVMOpCode::Execute_7_Operands:
			case ERigVMOpCode::Execute_8_Operands:
			case ERigVMOpCode::Execute_9_Operands:
			case ERigVMOpCode::Execute_10_Operands:
			case ERigVMOpCode::Execute_11_Operands:
			case ERigVMOpCode::Execute_12_Operands:
			case ERigVMOpCode::Execute_13_Operands:
			case ERigVMOpCode::Execute_14_Operands:
			case ERigVMOpCode::Execute_15_Operands:
			case ERigVMOpCode::Execute_16_Operands:
			case ERigVMOpCode::Execute_17_Operands:
			case ERigVMOpCode::Execute_18_Operands:
			case ERigVMOpCode::Execute_19_Operands:
			case ERigVMOpCode::Execute_20_Operands:
			case ERigVMOpCode::Execute_21_Operands:
			case ERigVMOpCode::Execute_22_Operands:
			case ERigVMOpCode::Execute_23_Operands:
			case ERigVMOpCode::Execute_24_Operands:
			case ERigVMOpCode::Execute_25_Operands:
			case ERigVMOpCode::Execute_26_Operands:
			case ERigVMOpCode::Execute_27_Operands:
			case ERigVMOpCode::Execute_28_Operands:
			case ERigVMOpCode::Execute_29_Operands:
			case ERigVMOpCode::Execute_30_Operands:
			case ERigVMOpCode::Execute_31_Operands:
			case ERigVMOpCode::Execute_32_Operands:
			case ERigVMOpCode::Execute_33_Operands:
			case ERigVMOpCode::Execute_34_Operands:
			case ERigVMOpCode::Execute_35_Operands:
			case ERigVMOpCode::Execute_36_Operands:
			case ERigVMOpCode::Execute_37_Operands:
			case ERigVMOpCode::Execute_38_Operands:
			case ERigVMOpCode::Execute_39_Operands:
			case ERigVMOpCode::Execute_40_Operands:
			case ERigVMOpCode::Execute_41_Operands:
			case ERigVMOpCode::Execute_42_Operands:
			case ERigVMOpCode::Execute_43_Operands:
			case ERigVMOpCode::Execute_44_Operands:
			case ERigVMOpCode::Execute_45_Operands:
			case ERigVMOpCode::Execute_46_Operands:
			case ERigVMOpCode::Execute_47_Operands:
			case ERigVMOpCode::Execute_48_Operands:
			case ERigVMOpCode::Execute_49_Operands:
			case ERigVMOpCode::Execute_50_Operands:
			case ERigVMOpCode::Execute_51_Operands:
			case ERigVMOpCode::Execute_52_Operands:
			case ERigVMOpCode::Execute_53_Operands:
			case ERigVMOpCode::Execute_54_Operands:
			case ERigVMOpCode::Execute_55_Operands:
			case ERigVMOpCode::Execute_56_Operands:
			case ERigVMOpCode::Execute_57_Operands:
			case ERigVMOpCode::Execute_58_Operands:
			case ERigVMOpCode::Execute_59_Operands:
			case ERigVMOpCode::Execute_60_Operands:
			case ERigVMOpCode::Execute_61_Operands:
			case ERigVMOpCode::Execute_62_Operands:
			case ERigVMOpCode::Execute_63_Operands:
			case ERigVMOpCode::Execute_64_Operands:
			{
				const FRigVMExecuteOp& Op = ByteCodeStorage.GetOpAt<FRigVMExecuteOp>(ByteCodeInstruction);
				FRigVMOperandArray Operands = ByteCodeStorage.GetOperandsForExecuteOp(ByteCodeInstruction);
				for (const FRigVMOperand& Arg : Operands)
				{
					CheckOperandValidity(Arg);
				}
				break;
			}
			case ERigVMOpCode::Zero:
			case ERigVMOpCode::BoolFalse:
			case ERigVMOpCode::BoolTrue:
			case ERigVMOpCode::Increment:
			case ERigVMOpCode::Decrement:
			case ERigVMOpCode::ArrayReset:
			case ERigVMOpCode::ArrayReverse:
			{
				const FRigVMUnaryOp& Op = ByteCodeStorage.GetOpAt<FRigVMUnaryOp>(ByteCodeInstruction);
				CheckOperandValidity(Op.Arg);
				break;
			}
			case ERigVMOpCode::Copy:
			{
				const FRigVMCopyOp& Op = ByteCodeStorage.GetOpAt<FRigVMCopyOp>(ByteCodeInstruction);
				CheckOperandValidity(Op.Source);
				CheckOperandValidity(Op.Target);
				break;
			}
			case ERigVMOpCode::Equals:
			case ERigVMOpCode::NotEquals:
			{
				const FRigVMComparisonOp& Op = ByteCodeStorage.GetOpAt<FRigVMComparisonOp>(ByteCodeInstruction);
				CheckOperandValidity(Op.A);
				CheckOperandValidity(Op.B);
				CheckOperandValidity(Op.Result);
				break;
			}
			case ERigVMOpCode::JumpAbsoluteIf:
			case ERigVMOpCode::JumpForwardIf:
			case ERigVMOpCode::JumpBackwardIf:
			{
				const FRigVMJumpIfOp& Op = ByteCodeStorage.GetOpAt<FRigVMJumpIfOp>(ByteCodeInstruction);
				CheckOperandValidity(Op.Arg);
				break;
			}
			case ERigVMOpCode::BeginBlock:
			case ERigVMOpCode::ArrayGetNum:
			case ERigVMOpCode::ArraySetNum:
			case ERigVMOpCode::ArrayAppend:
			case ERigVMOpCode::ArrayClone:
			case ERigVMOpCode::ArrayRemove:
			case ERigVMOpCode::ArrayUnion:
			{
				const FRigVMBinaryOp& Op = ByteCodeStorage.GetOpAt<FRigVMBinaryOp>(ByteCodeInstruction);
				CheckOperandValidity(Op.ArgA);
				CheckOperandValidity(Op.ArgB);
				break;
			}
			case ERigVMOpCode::ArrayAdd:
			case ERigVMOpCode::ArrayGetAtIndex:
			case ERigVMOpCode::ArraySetAtIndex:
			case ERigVMOpCode::ArrayInsert:
			case ERigVMOpCode::ArrayDifference:
			case ERigVMOpCode::ArrayIntersection:
			{
				const FRigVMTernaryOp& Op = ByteCodeStorage.GetOpAt<FRigVMTernaryOp>(ByteCodeInstruction);
				CheckOperandValidity(Op.ArgA);
				CheckOperandValidity(Op.ArgB);
				CheckOperandValidity(Op.ArgC);
				break;
			}
			case ERigVMOpCode::ArrayFind:
			{
				const FRigVMQuaternaryOp& Op = ByteCodeStorage.GetOpAt<FRigVMQuaternaryOp>(ByteCodeInstruction);
				CheckOperandValidity(Op.ArgA);
				CheckOperandValidity(Op.ArgB);
				CheckOperandValidity(Op.ArgC);
				CheckOperandValidity(Op.ArgD);
				break;
			}
			case ERigVMOpCode::ArrayIterator:
			{
				const FRigVMSenaryOp& Op = ByteCodeStorage.GetOpAt<FRigVMSenaryOp>(ByteCodeInstruction);
				CheckOperandValidity(Op.ArgA);
				CheckOperandValidity(Op.ArgB);
				CheckOperandValidity(Op.ArgC);
				CheckOperandValidity(Op.ArgD);
				CheckOperandValidity(Op.ArgE);
				CheckOperandValidity(Op.ArgF);
				break;
			}
			case ERigVMOpCode::Invalid:
			{
				ensure(false);
				break;
			}
			default:
			{
				break;
			}
		}
	}

	return bAllOperandsValid;
}

void URigVM::Reset(bool IsIgnoringArchetypeRef)
{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	WorkMemoryStorage.Reset();
	LiteralMemoryStorage.Reset();
	DebugMemoryStorage.Reset();
#endif
	if(!IsIgnoringArchetypeRef)
	{
		FunctionNamesStorage.Reset();
		FunctionsStorage.Reset();
		ExternalPropertyPathDescriptions.Reset();
		ExternalPropertyPaths.Reset();
		ByteCodeStorage.Reset();
		Instructions.Reset();
		Parameters.Reset();
		ParametersNameMap.Reset();
	}
	DeferredVMToCopy = nullptr;

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	WorkMemoryPtr = &WorkMemoryStorage;
	LiteralMemoryPtr = &LiteralMemoryStorage;
	DebugMemoryPtr = &DebugMemoryStorage;
#endif
	if(!IsIgnoringArchetypeRef)
	{
		FunctionNamesPtr = &FunctionNamesStorage;
		FunctionsPtr = &FunctionsStorage;
		ByteCodePtr = &ByteCodeStorage;
	}

	InvalidateCachedMemory();
	
	OperandToDebugRegisters.Reset();
	NumExecutions = 0;
}

void URigVM::Empty()
{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	WorkMemoryStorage.Empty();
	LiteralMemoryStorage.Empty();
	DebugMemoryStorage.Empty();
#endif
	FunctionNamesStorage.Empty();
	FunctionsStorage.Empty();
	ExternalPropertyPathDescriptions.Empty();
	ExternalPropertyPaths.Empty();
	ByteCodeStorage.Empty();
	Instructions.Empty();
	Parameters.Empty();
	ParametersNameMap.Empty();
	DeferredVMToCopy = nullptr;
	ExternalVariables.Empty();

	InvalidateCachedMemory();

	CachedMemory.Empty();
	FirstHandleForInstruction.Empty();
	CachedMemoryHandles.Empty();

	OperandToDebugRegisters.Empty();
}

void URigVM::CopyFrom(URigVM* InVM, bool bDeferCopy, bool bReferenceLiteralMemory, bool bReferenceByteCode, bool bCopyExternalVariables, bool bCopyDynamicRegisters)
{
	check(InVM);

	// if this vm is currently executing on a worker thread
	// we defer the copy until the next execute
	if (ExecutingThreadId != INDEX_NONE || bDeferCopy)
	{
		DeferredVMToCopy = InVM;
		return;
	}
	
	Reset();

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	
	if(InVM->WorkMemoryPtr == &InVM->WorkMemoryStorage)
	{
		WorkMemoryStorage = InVM->WorkMemoryStorage;
		if (bCopyDynamicRegisters)
		{
			WorkMemoryStorage.CopyRegisters(InVM->WorkMemoryStorage);
		}
		WorkMemoryPtr = &WorkMemoryStorage;
	}
	else
	{
		WorkMemoryPtr = InVM->WorkMemoryPtr;
	}

	if(InVM->LiteralMemoryPtr == &InVM->LiteralMemoryStorage && !bReferenceLiteralMemory)
	{
		LiteralMemoryStorage = InVM->LiteralMemoryStorage;
		LiteralMemoryPtr = &LiteralMemoryStorage;
	}
	else
	{
		LiteralMemoryPtr = InVM->LiteralMemoryPtr;
	}

	if(InVM->DebugMemoryPtr == &InVM->DebugMemoryStorage)
	{
		DebugMemoryStorage = InVM->DebugMemoryStorage;
		DebugMemoryPtr = &DebugMemoryStorage;
	}
	else
	{
		DebugMemoryPtr = InVM->DebugMemoryPtr;
	}
	
#else

	auto CopyMemoryStorage = [](TObjectPtr<URigVMMemoryStorage>& TargetMemory, URigVMMemoryStorage* SourceMemory, UObject* Outer)
	{
		if(SourceMemory != nullptr)
		{
			if(TargetMemory == nullptr)
			{
				TargetMemory = NewObject<URigVMMemoryStorage>(Outer, SourceMemory->GetClass());
			}
			else if(TargetMemory->GetClass() != SourceMemory->GetClass())
			{
				TargetMemory->Rename(nullptr, GetTransientPackage(), REN_ForceNoResetLoaders | REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
				TargetMemory->MarkAsGarbage();
				TargetMemory = NewObject<URigVMMemoryStorage>(Outer, SourceMemory->GetClass());
			}

			for(int32 PropertyIndex = 0; PropertyIndex < TargetMemory->Num(); PropertyIndex++)
			{
				URigVMMemoryStorage::CopyProperty(
					TargetMemory,
					PropertyIndex,
					FRigVMPropertyPath::Empty,
					SourceMemory,
					PropertyIndex,
					FRigVMPropertyPath::Empty
				);
			}
		}
		else if(TargetMemory != nullptr)
		{
			TargetMemory->Rename(nullptr, GetTransientPackage(), REN_ForceNoResetLoaders | REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
			TargetMemory->MarkAsGarbage();
			TargetMemory = nullptr;
		}
	};

	// we don't need to copy the literals since they are shared
	// between all instances of the VM
	LiteralMemoryStorageObject = Cast<URigVMMemoryStorage>(InVM->GetLiteralMemory()->GetClass()->GetDefaultObject());
	CopyMemoryStorage(WorkMemoryStorageObject, InVM->GetWorkMemory(), this);
	CopyMemoryStorage(DebugMemoryStorageObject, InVM->GetDebugMemory(), this);

	ExternalPropertyPathDescriptions = InVM->ExternalPropertyPathDescriptions;
	ExternalPropertyPaths.Reset();
	
#endif

	if(InVM->FunctionNamesPtr == &InVM->FunctionNamesStorage && !bReferenceByteCode)
	{
		FunctionNamesStorage = InVM->FunctionNamesStorage;
		FunctionNamesPtr = &FunctionNamesStorage;
	}
	else
	{
		FunctionNamesPtr = InVM->FunctionNamesPtr;
	}
	
	if(InVM->FunctionsPtr == &InVM->FunctionsStorage && !bReferenceByteCode)
	{
		FunctionsStorage = InVM->FunctionsStorage;
		FunctionsPtr = &FunctionsStorage;
	}
	else
	{
		FunctionsPtr = InVM->FunctionsPtr;
	}
	
	if(InVM->ByteCodePtr == &InVM->ByteCodeStorage && !bReferenceByteCode)
	{
		ByteCodeStorage = InVM->ByteCodeStorage;
		ByteCodePtr = &ByteCodeStorage;
		ByteCodePtr->bByteCodeIsAligned = InVM->ByteCodeStorage.bByteCodeIsAligned;
	}
	else
	{
		ByteCodePtr = InVM->ByteCodePtr;
	}
	
	Instructions = InVM->Instructions;
	Parameters = InVM->Parameters;
	ParametersNameMap = InVM->ParametersNameMap;
	OperandToDebugRegisters = InVM->OperandToDebugRegisters;

	if (bCopyExternalVariables)
	{
		ExternalVariables = InVM->ExternalVariables;
	}
}

int32 URigVM::AddRigVMFunction(UScriptStruct* InRigVMStruct, const FName& InMethodName)
{
	check(InRigVMStruct);
	FString FunctionKey = FString::Printf(TEXT("F%s::%s"), *InRigVMStruct->GetName(), *InMethodName.ToString());
	int32 FunctionIndex = GetFunctionNames().Find(*FunctionKey);
	if (FunctionIndex != INDEX_NONE)
	{
		return FunctionIndex;
	}

	FRigVMFunctionPtr Function = FRigVMRegistry::Get().FindFunction(*FunctionKey);
	if (Function == nullptr)
	{
		return INDEX_NONE;
	}

	GetFunctionNames().Add(*FunctionKey);
	return GetFunctions().Add(Function);
}

FString URigVM::GetRigVMFunctionName(int32 InFunctionIndex) const
{
	return GetFunctionNames()[InFunctionIndex].ToString();
}

#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

URigVMMemoryStorage* URigVM::GetMemoryByType(ERigVMMemoryType InMemoryType, bool bCreateIfNeeded)
{
	switch(InMemoryType)
	{
		case ERigVMMemoryType::Literal:
		{
			if(bCreateIfNeeded)
			{
				if(LiteralMemoryStorageObject == nullptr)
				{
					if(UClass* Class = URigVMMemoryStorageGeneratorClass::GetStorageClass(this, InMemoryType))
					{
						// for literals we share the CDO between all VMs
						LiteralMemoryStorageObject = Cast<URigVMMemoryStorage>(Class->GetDefaultObject(true));
					}
					else
					{
						// since literal memory object can be shared across packages, it needs to have the RF_Public flag
						// for example, a control rig instance in a level sequence pacakge can references
						// the literal memory object in the control rig package
						LiteralMemoryStorageObject = NewObject<URigVMMemoryStorage>(this, FName(), RF_Public);
					}
				}
			}
			return LiteralMemoryStorageObject;
		}
		case ERigVMMemoryType::Work:
		{
			if(bCreateIfNeeded)
			{
				if(WorkMemoryStorageObject)
				{
					if(WorkMemoryStorageObject->GetOuter() != this)
					{
						WorkMemoryStorageObject = nullptr;
					}
				}
				if(WorkMemoryStorageObject == nullptr)
				{
					if(UClass* Class = URigVMMemoryStorageGeneratorClass::GetStorageClass(this, InMemoryType))
					{
						WorkMemoryStorageObject = NewObject<URigVMMemoryStorage>(this, Class);
					}
					else
					{
						WorkMemoryStorageObject = NewObject<URigVMMemoryStorage>(this);
					}
				}
			}
			check(WorkMemoryStorageObject->GetOuter() == this);
			return WorkMemoryStorageObject;
		}
		case ERigVMMemoryType::Debug:
		{
			if(bCreateIfNeeded)
			{
				if(DebugMemoryStorageObject)
				{
					if(DebugMemoryStorageObject->GetOuter() != this)
					{
						DebugMemoryStorageObject = nullptr;
					}
				}
				if(DebugMemoryStorageObject == nullptr)
				{
#if WITH_EDITOR
					if(UClass* Class = URigVMMemoryStorageGeneratorClass::GetStorageClass(this, InMemoryType))
					{
						DebugMemoryStorageObject = NewObject<URigVMMemoryStorage>(this, Class);
					}
					else
#endif
					{
						DebugMemoryStorageObject = NewObject<URigVMMemoryStorage>(this);
					}
				}
			}
			check(DebugMemoryStorageObject->GetOuter() == this);
			return DebugMemoryStorageObject;
		}
		default:
		{
			break;
		}
	}
	return nullptr;
}

void URigVM::ClearMemory()
{
	// At one point our memory objects were saved with RF_Public,
	// so to truly clear them, we have to also clear the flags
	// RF_Public will make them stay around as zombie unreferenced objects, and get included in SavePackage and cooking.
	// Clear their flags so they are not included by editor or cook SavePackage calls.

	// we now make sure that only the literal memory object on the CDO is marked as RF_Public
	// and work memory objects are no longer marked as RF_Public
	
	TArray<UObject*> SubObjects;
	GetObjectsWithOuter(this, SubObjects);
	for (UObject* SubObject : SubObjects)
	{
		if (URigVMMemoryStorage* MemoryObject = Cast<URigVMMemoryStorage>(SubObject))
		{
			// we don't care about memory type here because
			// 
			// if "this" is not CDO, its subobjects will not include the literal memory and
			// thus only clears the flag for work mem
			// 
			// if "this" is CDO, its subobjects will include the literal memory and this allows
			// us to actually clear the literal memory
			MemoryObject->ClearFlags(RF_Public);
		}
	}

	LiteralMemoryStorageObject = nullptr;

	if(WorkMemoryStorageObject)
	{
		WorkMemoryStorageObject->Rename(nullptr, GetTransientPackage(), REN_ForceNoResetLoaders | REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
		WorkMemoryStorageObject->MarkAsGarbage();
		WorkMemoryStorageObject = nullptr;
	}

	if(DebugMemoryStorageObject)
	{
		DebugMemoryStorageObject->Rename(nullptr, GetTransientPackage(), REN_ForceNoResetLoaders | REN_DoNotDirty | REN_DontCreateRedirectors | REN_NonTransactional);
		DebugMemoryStorageObject->MarkAsGarbage();
		DebugMemoryStorageObject = nullptr;
	}

	InvalidateCachedMemory();
}

#endif

const FRigVMInstructionArray& URigVM::GetInstructions()
{
	RefreshInstructionsIfRequired();
	return Instructions;
}

bool URigVM::ContainsEntry(const FName& InEntryName) const
{
	const FRigVMByteCode& ByteCode = GetByteCode();
	return ByteCode.FindEntryIndex(InEntryName) != INDEX_NONE;
}

TArray<FName> URigVM::GetEntryNames() const
{
	TArray<FName> EntryNames;

	const FRigVMByteCode& ByteCode = GetByteCode();
	for (int32 EntryIndex = 0; EntryIndex < ByteCode.NumEntries(); EntryIndex++)
	{
		EntryNames.Add(ByteCode.GetEntry(EntryIndex).Name);
	}

	return EntryNames;
}

#if WITH_EDITOR

bool URigVM::ResumeExecution()
{
	HaltedAtBreakpoint = nullptr;
	HaltedAtBreakpointHit = INDEX_NONE;
	if (DebugInfo)
	{
		if (const TSharedPtr<FRigVMBreakpoint> CurrentBreakpoint = DebugInfo->GetCurrentActiveBreakpoint())
		{
			DebugInfo->IncrementBreakpointActivationOnHit(CurrentBreakpoint);
			DebugInfo->SetCurrentActiveBreakpoint(nullptr);
			return true;
		}
	}

	return false;
}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
bool URigVM::ResumeExecution(FRigVMMemoryContainerPtrArray Memory, FRigVMFixedArray<void*> AdditionalArguments, const FName& InEntryName)
#else
bool URigVM::ResumeExecution(TArrayView<URigVMMemoryStorage*> Memory, TArrayView<void*> AdditionalArguments, const FName& InEntryName)
#endif
{
	ResumeExecution();
	return Execute(Memory, AdditionalArguments, InEntryName);
}

#endif

const TArray<FRigVMParameter>& URigVM::GetParameters() const
{
	return Parameters;
}

FRigVMParameter URigVM::GetParameterByName(const FName& InParameterName)
{
	if (ParametersNameMap.Num() == Parameters.Num())
	{
		const int32* ParameterIndex = ParametersNameMap.Find(InParameterName);
		if (ParameterIndex)
		{
			Parameters[*ParameterIndex].GetScriptStruct();
			return Parameters[*ParameterIndex];
		}
		return FRigVMParameter();
	}

	for (FRigVMParameter& Parameter : Parameters)
	{
		if (Parameter.GetName() == InParameterName)
		{
			Parameter.GetScriptStruct();
			return Parameter;
		}
	}

	return FRigVMParameter();
}

void URigVM::ResolveFunctionsIfRequired()
{
	if (GetFunctions().Num() != GetFunctionNames().Num())
	{
		GetFunctions().Reset();
		GetFunctions().SetNumZeroed(GetFunctionNames().Num());

		for (int32 FunctionIndex = 0; FunctionIndex < GetFunctionNames().Num(); FunctionIndex++)
		{
			GetFunctions()[FunctionIndex] = FRigVMRegistry::Get().FindFunction(*GetFunctionNames()[FunctionIndex].ToString());
			ensureMsgf(GetFunctions()[FunctionIndex], TEXT("Function %s is not valid"), *GetFunctionNames()[FunctionIndex].ToString());
		}
	}
}

void URigVM::RefreshInstructionsIfRequired()
{
	if (GetByteCode().Num() == 0 && Instructions.Num() > 0)
	{
		Instructions.Reset();
	}
	else if (Instructions.Num() == 0)
	{
		Instructions = GetByteCode().GetInstructions();
	}
}

void URigVM::InvalidateCachedMemory()
{
	CachedMemory.Reset();
	FirstHandleForInstruction.Reset();
	CachedMemoryHandles.Reset();

#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	ExternalPropertyPaths.Reset();
#endif
}

void URigVM::CopyDeferredVMIfRequired()
{
	ensure(ExecutingThreadId == INDEX_NONE);

	URigVM* VMToCopy = nullptr;
	Swap(VMToCopy, DeferredVMToCopy);

	if (VMToCopy)
	{
		CopyFrom(VMToCopy);
	}
}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
void URigVM::CacheMemoryHandlesIfRequired(FRigVMMemoryContainerPtrArray InMemory)
#else
void URigVM::CacheMemoryHandlesIfRequired(TArrayView<URigVMMemoryStorage*> InMemory)
#endif
{
	ensureMsgf(ExecutingThreadId == FPlatformTLS::GetCurrentThreadId(), TEXT("RigVM::CacheMemoryHandlesIfRequired from multiple threads (%d and %d)"), ExecutingThreadId, (int32)FPlatformTLS::GetCurrentThreadId());

	RefreshInstructionsIfRequired();

	if (Instructions.Num() == 0 || InMemory.Num() == 0)
	{
		InvalidateCachedMemory();
		return;
	}

	if (Instructions.Num() != FirstHandleForInstruction.Num())
	{
		InvalidateCachedMemory();
	}
	else if (InMemory.Num() != CachedMemory.Num())
	{
		InvalidateCachedMemory();
	}
	else
	{
		for (int32 Index = 0; Index < InMemory.Num(); Index++)
		{
			if (InMemory[Index] != CachedMemory[Index])
			{
				InvalidateCachedMemory();
				break;
			}
		}
	}

	if (Instructions.Num() == FirstHandleForInstruction.Num())
	{
		return;
	}

	for (int32 Index = 0; Index < InMemory.Num(); Index++)
	{
		CachedMemory.Add(InMemory[Index]);
	}

#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

	RefreshExternalPropertyPaths();
	
#endif		

	FRigVMByteCode& ByteCode = GetByteCode();

	uint16 InstructionIndex = 0;
	while (Instructions.IsValidIndex(InstructionIndex))
	{
		FirstHandleForInstruction.Add(CachedMemoryHandles.Num());

		switch (Instructions[InstructionIndex].OpCode)
		{
			case ERigVMOpCode::Execute_0_Operands:
			case ERigVMOpCode::Execute_1_Operands:
			case ERigVMOpCode::Execute_2_Operands:
			case ERigVMOpCode::Execute_3_Operands:
			case ERigVMOpCode::Execute_4_Operands:
			case ERigVMOpCode::Execute_5_Operands:
			case ERigVMOpCode::Execute_6_Operands:
			case ERigVMOpCode::Execute_7_Operands:
			case ERigVMOpCode::Execute_8_Operands:
			case ERigVMOpCode::Execute_9_Operands:
			case ERigVMOpCode::Execute_10_Operands:
			case ERigVMOpCode::Execute_11_Operands:
			case ERigVMOpCode::Execute_12_Operands:
			case ERigVMOpCode::Execute_13_Operands:
			case ERigVMOpCode::Execute_14_Operands:
			case ERigVMOpCode::Execute_15_Operands:
			case ERigVMOpCode::Execute_16_Operands:
			case ERigVMOpCode::Execute_17_Operands:
			case ERigVMOpCode::Execute_18_Operands:
			case ERigVMOpCode::Execute_19_Operands:
			case ERigVMOpCode::Execute_20_Operands:
			case ERigVMOpCode::Execute_21_Operands:
			case ERigVMOpCode::Execute_22_Operands:
			case ERigVMOpCode::Execute_23_Operands:
			case ERigVMOpCode::Execute_24_Operands:
			case ERigVMOpCode::Execute_25_Operands:
			case ERigVMOpCode::Execute_26_Operands:
			case ERigVMOpCode::Execute_27_Operands:
			case ERigVMOpCode::Execute_28_Operands:
			case ERigVMOpCode::Execute_29_Operands:
			case ERigVMOpCode::Execute_30_Operands:
			case ERigVMOpCode::Execute_31_Operands:
			case ERigVMOpCode::Execute_32_Operands:
			case ERigVMOpCode::Execute_33_Operands:
			case ERigVMOpCode::Execute_34_Operands:
			case ERigVMOpCode::Execute_35_Operands:
			case ERigVMOpCode::Execute_36_Operands:
			case ERigVMOpCode::Execute_37_Operands:
			case ERigVMOpCode::Execute_38_Operands:
			case ERigVMOpCode::Execute_39_Operands:
			case ERigVMOpCode::Execute_40_Operands:
			case ERigVMOpCode::Execute_41_Operands:
			case ERigVMOpCode::Execute_42_Operands:
			case ERigVMOpCode::Execute_43_Operands:
			case ERigVMOpCode::Execute_44_Operands:
			case ERigVMOpCode::Execute_45_Operands:
			case ERigVMOpCode::Execute_46_Operands:
			case ERigVMOpCode::Execute_47_Operands:
			case ERigVMOpCode::Execute_48_Operands:
			case ERigVMOpCode::Execute_49_Operands:
			case ERigVMOpCode::Execute_50_Operands:
			case ERigVMOpCode::Execute_51_Operands:
			case ERigVMOpCode::Execute_52_Operands:
			case ERigVMOpCode::Execute_53_Operands:
			case ERigVMOpCode::Execute_54_Operands:
			case ERigVMOpCode::Execute_55_Operands:
			case ERigVMOpCode::Execute_56_Operands:
			case ERigVMOpCode::Execute_57_Operands:
			case ERigVMOpCode::Execute_58_Operands:
			case ERigVMOpCode::Execute_59_Operands:
			case ERigVMOpCode::Execute_60_Operands:
			case ERigVMOpCode::Execute_61_Operands:
			case ERigVMOpCode::Execute_62_Operands:
			case ERigVMOpCode::Execute_63_Operands:
			case ERigVMOpCode::Execute_64_Operands:
			{
				const FRigVMExecuteOp& Op = ByteCode.GetOpAt<FRigVMExecuteOp>(Instructions[InstructionIndex]);
				FRigVMOperandArray Operands = ByteCode.GetOperandsForExecuteOp(Instructions[InstructionIndex]);

				for (const FRigVMOperand& Arg : Operands)
				{
					CacheSingleMemoryHandle(Arg, true);
				}

				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Zero:
			case ERigVMOpCode::BoolFalse:
			case ERigVMOpCode::BoolTrue:
			case ERigVMOpCode::Increment:
			case ERigVMOpCode::Decrement:
			case ERigVMOpCode::ArrayReset:
			case ERigVMOpCode::ArrayReverse:
			{
				const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instructions[InstructionIndex]);
				CacheSingleMemoryHandle(Op.Arg);
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Copy:
			{
				const FRigVMCopyOp& Op = ByteCode.GetOpAt<FRigVMCopyOp>(Instructions[InstructionIndex]);
				CacheSingleMemoryHandle(Op.Source);
				CacheSingleMemoryHandle(Op.Target);

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				if (UScriptStruct* ScriptStruct = GetScriptStructForCopyOp(Op))
				{
					CachedMemoryHandles.Add(FRigVMMemoryHandle((uint8*)ScriptStruct));
				}
#endif

				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Equals:
			case ERigVMOpCode::NotEquals:
			{
				const FRigVMComparisonOp& Op = ByteCode.GetOpAt<FRigVMComparisonOp>(Instructions[InstructionIndex]);
				FRigVMOperand Arg = Op.A;
				CacheSingleMemoryHandle(Arg);
				Arg = Op.B;
				CacheSingleMemoryHandle(Arg);
				Arg = Op.Result;
				CacheSingleMemoryHandle(Arg);
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::JumpAbsolute:
			case ERigVMOpCode::JumpForward:
			case ERigVMOpCode::JumpBackward:
			{
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::JumpAbsoluteIf:
			case ERigVMOpCode::JumpForwardIf:
			case ERigVMOpCode::JumpBackwardIf:
			{
				const FRigVMJumpIfOp& Op = ByteCode.GetOpAt<FRigVMJumpIfOp>(Instructions[InstructionIndex]);
				const FRigVMOperand& Arg = Op.Arg;
				CacheSingleMemoryHandle(Arg);
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ChangeType:
			{
				const FRigVMChangeTypeOp& Op = ByteCode.GetOpAt<FRigVMChangeTypeOp>(Instructions[InstructionIndex]);
				const FRigVMOperand& Arg = Op.Arg;
				CacheSingleMemoryHandle(Arg);
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Exit:
			{
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::BeginBlock:
			case ERigVMOpCode::ArrayGetNum:
			case ERigVMOpCode::ArraySetNum:
			case ERigVMOpCode::ArrayAppend:
			case ERigVMOpCode::ArrayClone:
			case ERigVMOpCode::ArrayRemove:
			case ERigVMOpCode::ArrayUnion:
			{
				const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instructions[InstructionIndex]);
				CacheSingleMemoryHandle(Op.ArgA);
				CacheSingleMemoryHandle(Op.ArgB);
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayAdd:
			case ERigVMOpCode::ArrayGetAtIndex:
			case ERigVMOpCode::ArraySetAtIndex:
			case ERigVMOpCode::ArrayInsert:
			case ERigVMOpCode::ArrayDifference:
			case ERigVMOpCode::ArrayIntersection:
			{
				const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instructions[InstructionIndex]);
				CacheSingleMemoryHandle(Op.ArgA);
				CacheSingleMemoryHandle(Op.ArgB);
				CacheSingleMemoryHandle(Op.ArgC);
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayFind:
			{
				const FRigVMQuaternaryOp& Op = ByteCode.GetOpAt<FRigVMQuaternaryOp>(Instructions[InstructionIndex]);
				CacheSingleMemoryHandle(Op.ArgA);
				CacheSingleMemoryHandle(Op.ArgB);
				CacheSingleMemoryHandle(Op.ArgC);
				CacheSingleMemoryHandle(Op.ArgD);
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayIterator:
			{
				const FRigVMSenaryOp& Op = ByteCode.GetOpAt<FRigVMSenaryOp>(Instructions[InstructionIndex]);
				CacheSingleMemoryHandle(Op.ArgA);
				CacheSingleMemoryHandle(Op.ArgB);
				CacheSingleMemoryHandle(Op.ArgC);
				CacheSingleMemoryHandle(Op.ArgD);
				CacheSingleMemoryHandle(Op.ArgE);
				CacheSingleMemoryHandle(Op.ArgF);
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::EndBlock:
			{
				InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Invalid:
			{
				ensure(false);
				break;
			}
		}
	}

	if (FirstHandleForInstruction.Num() < Instructions.Num())
	{
		FirstHandleForInstruction.Add(CachedMemoryHandles.Num());
	}
}

void URigVM::RebuildByteCodeOnLoad()
{
	Instructions = GetByteCode().GetInstructions();
	for(int32 InstructionIndex = 0; InstructionIndex < Instructions.Num(); InstructionIndex++)
	{
		const FRigVMInstruction& Instruction = Instructions[InstructionIndex];
		switch(Instruction.OpCode)
		{
			case ERigVMOpCode::Copy:
			{
				FRigVMCopyOp OldCopyOp = GetByteCode().GetOpAt<FRigVMCopyOp>(Instruction);
				if((OldCopyOp.Source.GetMemoryType() == ERigVMMemoryType::External) ||
					(OldCopyOp.Target.GetMemoryType() == ERigVMMemoryType::External))
				{
					if(ExternalVariables.IsEmpty())
					{
						break;
					}
				}
					
				// create a local copy of the original op
				FRigVMCopyOp& NewCopyOp = GetByteCode().GetOpAt<FRigVMCopyOp>(Instruction);
				NewCopyOp = GetCopyOpForOperands(OldCopyOp.Source, OldCopyOp.Target);
				check(OldCopyOp.Source == NewCopyOp.Source);
				check(OldCopyOp.Target == NewCopyOp.Target);
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				//check(OldCopyOp.NumBytes == NewCopyOp.NumBytes);
				check(OldCopyOp.RegisterType == NewCopyOp.RegisterType);
#endif
				break;
			}
			default:
			{
				break;
			}
		}
	}
}

#if WITH_EDITOR
bool URigVM::ShouldHaltAtInstruction(const FName& InEventName, const uint16 InstructionIndex)
{
	FRigVMByteCode& ByteCode = GetByteCode();

	TArray<TSharedPtr<FRigVMBreakpoint>> BreakpointsAtInstruction = DebugInfo->FindBreakpointsAtInstruction(InstructionIndex);
	for (TSharedPtr<FRigVMBreakpoint> Breakpoint : BreakpointsAtInstruction)
	{
		if (DebugInfo->IsActive(Breakpoint))
		{
			switch (CurrentBreakpointAction)
			{
				case ERigVMBreakpointAction::None:
				{
					// Halted at breakpoint. Check if this is a new breakpoint different from the previous halt.
					if (HaltedAtBreakpoint != Breakpoint ||
						HaltedAtBreakpointHit != DebugInfo->GetBreakpointHits(Breakpoint))
					{
						HaltedAtBreakpoint = Breakpoint;
						HaltedAtBreakpointHit = DebugInfo->GetBreakpointHits(Breakpoint);
						DebugInfo->SetCurrentActiveBreakpoint(Breakpoint);
						
						// We want to keep the callstack up to the node that produced the halt
						const TArray<UObject*>* FullCallstack = ByteCode.GetCallstackForInstruction(Context.InstructionIndex);
						if (FullCallstack)
						{
							DebugInfo->SetCurrentActiveBreakpointCallstack(TArray<UObject*>(FullCallstack->GetData(), FullCallstack->Find((UObject*)Breakpoint->Subject)+1));
						}
						ExecutionHalted().Broadcast(Context.InstructionIndex, Breakpoint->Subject, InEventName);
					}
					return true;
				}
				case ERigVMBreakpointAction::Resume:
				{
					CurrentBreakpointAction = ERigVMBreakpointAction::None;

					if (DebugInfo->IsTemporaryBreakpoint(Breakpoint))
					{
						DebugInfo->RemoveBreakpoint(Breakpoint);
					}
					else
					{
						DebugInfo->IncrementBreakpointActivationOnHit(Breakpoint);
						DebugInfo->HitBreakpoint(Breakpoint);
					}
					break;
				}
				case ERigVMBreakpointAction::StepOver:
				case ERigVMBreakpointAction::StepInto:
				case ERigVMBreakpointAction::StepOut:
				{
					// If we are stepping, check if we were halted at the current instruction, and remember it 
					if (!DebugInfo->GetCurrentActiveBreakpoint())
					{
						DebugInfo->SetCurrentActiveBreakpoint(Breakpoint);
						const TArray<UObject*>* FullCallstack = ByteCode.GetCallstackForInstruction(Context.InstructionIndex);
						
						// We want to keep the callstack up to the node that produced the halt
						if (FullCallstack)
						{
							DebugInfo->SetCurrentActiveBreakpointCallstack(TArray<UObject*>(FullCallstack->GetData(), FullCallstack->Find((UObject*)DebugInfo->GetCurrentActiveBreakpoint()->Subject)+1));
						}
					}							
					
					break;	
				}
				default:
				{
					ensure(false);
					break;
				}
			}
		}
		else
		{
			DebugInfo->HitBreakpoint(Breakpoint);
		}
	}

	// If we are stepping, and the last active breakpoint was set, check if this is the new temporary breakpoint
	if (CurrentBreakpointAction != ERigVMBreakpointAction::None && DebugInfo->GetCurrentActiveBreakpoint())
	{
		const TArray<UObject*>* CurrentCallstack = ByteCode.GetCallstackForInstruction(Context.InstructionIndex);
		if (CurrentCallstack && !CurrentCallstack->IsEmpty())
		{
			UObject* NewBreakpointNode = nullptr;

			// Find the first difference in the callstack
			int32 DifferenceIndex = INDEX_NONE;
			TArray<UObject*>& PreviousCallstack = DebugInfo->GetCurrentActiveBreakpointCallstack();
			for (int32 i=0; i<PreviousCallstack.Num(); ++i)
			{
				if (CurrentCallstack->Num() == i)
				{
					DifferenceIndex = i-1;
					break;
				}
				if (PreviousCallstack[i] != CurrentCallstack->operator[](i))
				{
					DifferenceIndex = i;
					break;
				}
			}

			if (CurrentBreakpointAction == ERigVMBreakpointAction::StepOver)
			{
				if (DifferenceIndex != INDEX_NONE)
				{
					NewBreakpointNode = CurrentCallstack->operator[](DifferenceIndex);
				}
			}
			else if (CurrentBreakpointAction == ERigVMBreakpointAction::StepInto)
			{
				if (DifferenceIndex == INDEX_NONE)
				{
					if (!CurrentCallstack->IsEmpty() && !PreviousCallstack.IsEmpty() && CurrentCallstack->Last() != PreviousCallstack.Last())
					{
						NewBreakpointNode = CurrentCallstack->operator[](FMath::Min(PreviousCallstack.Num(), CurrentCallstack->Num()-1));
					}
				}
				else
				{
					NewBreakpointNode = CurrentCallstack->operator[](DifferenceIndex);
				}
			}
			else if (CurrentBreakpointAction == ERigVMBreakpointAction::StepOut)
			{
				if (DifferenceIndex != INDEX_NONE && DifferenceIndex <= PreviousCallstack.Num() - 2)
                {
                	NewBreakpointNode = CurrentCallstack->operator[](DifferenceIndex);
                }
			}
			
			if (NewBreakpointNode)
			{
				// Remove or hit previous breakpoint
				if (DebugInfo->IsTemporaryBreakpoint(DebugInfo->GetCurrentActiveBreakpoint()))
				{
					DebugInfo->RemoveBreakpoint(DebugInfo->GetCurrentActiveBreakpoint());
				}
				else
				{
					DebugInfo->IncrementBreakpointActivationOnHit(DebugInfo->GetCurrentActiveBreakpoint());
					DebugInfo->HitBreakpoint(DebugInfo->GetCurrentActiveBreakpoint());
				}

				// Create new temporary breakpoint
				TSharedPtr<FRigVMBreakpoint> NewBreakpoint = DebugInfo->AddBreakpoint(Context.InstructionIndex, NewBreakpointNode, 0, true);
				DebugInfo->SetBreakpointHits(NewBreakpoint, GetInstructionVisitedCount(Context.InstructionIndex));
				DebugInfo->SetBreakpointActivationOnHit(NewBreakpoint, GetInstructionVisitedCount(Context.InstructionIndex));
				CurrentBreakpointAction = ERigVMBreakpointAction::None;					

				HaltedAtBreakpoint = NewBreakpoint;
				HaltedAtBreakpointHit = DebugInfo->GetBreakpointHits(HaltedAtBreakpoint);
				ExecutionHalted().Broadcast(Context.InstructionIndex, NewBreakpointNode, InEventName);
		
				return true;
			}
		}
	}

	return false;
}
#endif

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
bool URigVM::Initialize(FRigVMMemoryContainerPtrArray Memory, FRigVMFixedArray<void*> AdditionalArguments)
#else
bool URigVM::Initialize(TArrayView<URigVMMemoryStorage*> Memory, TArrayView<void*> AdditionalArguments)
#endif
{
	if (ExecutingThreadId != INDEX_NONE)
	{
		ensureMsgf(ExecutingThreadId == FPlatformTLS::GetCurrentThreadId(), TEXT("RigVM::Initialize from multiple threads (%d and %d)"), ExecutingThreadId, (int32)FPlatformTLS::GetCurrentThreadId());
	}
	CopyDeferredVMIfRequired();
	TGuardValue<int32> GuardThreadId(ExecutingThreadId, FPlatformTLS::GetCurrentThreadId());

	ResolveFunctionsIfRequired();
	RefreshInstructionsIfRequired();

	if (Instructions.Num() == 0)
	{
		return true;
	}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	FRigVMMemoryContainer* LocalMemory[] = { WorkMemoryPtr, LiteralMemoryPtr, DebugMemoryPtr };
	if (Memory.Num() == 0)
	{
		Memory = FRigVMMemoryContainerPtrArray(LocalMemory, 3);
	}
#else
	TArray<URigVMMemoryStorage*> LocalMemory;
	if (Memory.Num() == 0)
	{
		LocalMemory = GetLocalMemoryArray();
		Memory = LocalMemory;
	}
#endif

	CacheMemoryHandlesIfRequired(Memory);
	FRigVMByteCode& ByteCode = GetByteCode();
	TArray<FRigVMFunctionPtr>& Functions = GetFunctions();

#if WITH_EDITOR
	TArray<FName>& FunctionNames = GetFunctionNames();
#endif

	Context.Reset();
	Context.SliceOffsets.AddZeroed(Instructions.Num());
	Context.OpaqueArguments = AdditionalArguments;
	Context.ExternalVariables = ExternalVariables;

	TGuardValue<URigVM*> VMInContext(Context.VM, this);
	
	while (Instructions.IsValidIndex(Context.InstructionIndex))
	{
		const FRigVMInstruction& Instruction = Instructions[Context.InstructionIndex];

		switch (Instruction.OpCode)
		{
			case ERigVMOpCode::Execute_0_Operands:
			case ERigVMOpCode::Execute_1_Operands:
			case ERigVMOpCode::Execute_2_Operands:
			case ERigVMOpCode::Execute_3_Operands:
			case ERigVMOpCode::Execute_4_Operands:
			case ERigVMOpCode::Execute_5_Operands:
			case ERigVMOpCode::Execute_6_Operands:
			case ERigVMOpCode::Execute_7_Operands:
			case ERigVMOpCode::Execute_8_Operands:
			case ERigVMOpCode::Execute_9_Operands:
			case ERigVMOpCode::Execute_10_Operands:
			case ERigVMOpCode::Execute_11_Operands:
			case ERigVMOpCode::Execute_12_Operands:
			case ERigVMOpCode::Execute_13_Operands:
			case ERigVMOpCode::Execute_14_Operands:
			case ERigVMOpCode::Execute_15_Operands:
			case ERigVMOpCode::Execute_16_Operands:
			case ERigVMOpCode::Execute_17_Operands:
			case ERigVMOpCode::Execute_18_Operands:
			case ERigVMOpCode::Execute_19_Operands:
			case ERigVMOpCode::Execute_20_Operands:
			case ERigVMOpCode::Execute_21_Operands:
			case ERigVMOpCode::Execute_22_Operands:
			case ERigVMOpCode::Execute_23_Operands:
			case ERigVMOpCode::Execute_24_Operands:
			case ERigVMOpCode::Execute_25_Operands:
			case ERigVMOpCode::Execute_26_Operands:
			case ERigVMOpCode::Execute_27_Operands:
			case ERigVMOpCode::Execute_28_Operands:
			case ERigVMOpCode::Execute_29_Operands:
			case ERigVMOpCode::Execute_30_Operands:
			case ERigVMOpCode::Execute_31_Operands:
			case ERigVMOpCode::Execute_32_Operands:
			case ERigVMOpCode::Execute_33_Operands:
			case ERigVMOpCode::Execute_34_Operands:
			case ERigVMOpCode::Execute_35_Operands:
			case ERigVMOpCode::Execute_36_Operands:
			case ERigVMOpCode::Execute_37_Operands:
			case ERigVMOpCode::Execute_38_Operands:
			case ERigVMOpCode::Execute_39_Operands:
			case ERigVMOpCode::Execute_40_Operands:
			case ERigVMOpCode::Execute_41_Operands:
			case ERigVMOpCode::Execute_42_Operands:
			case ERigVMOpCode::Execute_43_Operands:
			case ERigVMOpCode::Execute_44_Operands:
			case ERigVMOpCode::Execute_45_Operands:
			case ERigVMOpCode::Execute_46_Operands:
			case ERigVMOpCode::Execute_47_Operands:
			case ERigVMOpCode::Execute_48_Operands:
			case ERigVMOpCode::Execute_49_Operands:
			case ERigVMOpCode::Execute_50_Operands:
			case ERigVMOpCode::Execute_51_Operands:
			case ERigVMOpCode::Execute_52_Operands:
			case ERigVMOpCode::Execute_53_Operands:
			case ERigVMOpCode::Execute_54_Operands:
			case ERigVMOpCode::Execute_55_Operands:
			case ERigVMOpCode::Execute_56_Operands:
			case ERigVMOpCode::Execute_57_Operands:
			case ERigVMOpCode::Execute_58_Operands:
			case ERigVMOpCode::Execute_59_Operands:
			case ERigVMOpCode::Execute_60_Operands:
			case ERigVMOpCode::Execute_61_Operands:
			case ERigVMOpCode::Execute_62_Operands:
			case ERigVMOpCode::Execute_63_Operands:
			case ERigVMOpCode::Execute_64_Operands:
			{
				const FRigVMExecuteOp& Op = ByteCode.GetOpAt<FRigVMExecuteOp>(Instruction);
				int32 OperandCount = FirstHandleForInstruction[Context.InstructionIndex + 1] - FirstHandleForInstruction[Context.InstructionIndex];
				FRigVMMemoryHandleArray OpHandles(&CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]], OperandCount);
#if WITH_EDITOR
				Context.FunctionName = FunctionNames[Op.FunctionIndex];
#endif

				// find out the largest slice count
				int32 MaxSliceCount = 1;

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				for (const FRigVMMemoryHandle& OpHandle : OpHandles)
				{
					if (OpHandle.Type == FRigVMMemoryHandle::Dynamic)
					{
						if (const FRigVMByteArray* Storage = (const FRigVMByteArray*)OpHandle.Ptr)
						{
							MaxSliceCount = FMath::Max<int32>(MaxSliceCount, Storage->Num() / OpHandle.Size);
						}
					}
					else if (OpHandle.Type == FRigVMMemoryHandle::NestedDynamic)
					{
						if (const FRigVMNestedByteArray* Storage = (const FRigVMNestedByteArray*)OpHandle.Ptr)
						{
							MaxSliceCount = FMath::Max<int32>(MaxSliceCount, Storage->Num());
						}
					}
				}
#else
				// todo Deal with slice counts
#endif

				Context.BeginSlice(MaxSliceCount);
				for (int32 SliceIndex = 0; SliceIndex < MaxSliceCount; SliceIndex++)
				{
					(*Functions[Op.FunctionIndex])(Context, OpHandles);
					Context.IncrementSlice();
				}
				Context.EndSlice();

				break;
			}
			case ERigVMOpCode::Zero:
			case ERigVMOpCode::BoolFalse:
			case ERigVMOpCode::BoolTrue:
			{
				break;
			}
			case ERigVMOpCode::Copy:
			{
				const FRigVMCopyOp& Op = ByteCode.GetOpAt<FRigVMCopyOp>(Instruction);

				FRigVMMemoryHandle& SourceHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				FRigVMMemoryHandle& TargetHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1];

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				void* SourcePtr = SourceHandle;
				void* TargetPtr = TargetHandle;

				const uint64 NumBytes = (uint64)Op.NumBytes;

				if (TargetHandle.Type == FRigVMMemoryHandle::Dynamic)
				{
					FRigVMByteArray* Storage = (FRigVMByteArray*)TargetHandle.Ptr;
					if (Context.GetSlice().GetIndex() == 0)
					{
						Storage->Reset();
					}
					int32 ByteIndex = Storage->AddZeroed(NumBytes);
					TargetPtr = Storage->GetData() + ByteIndex;
				}
				else if (TargetHandle.Type == FRigVMMemoryHandle::NestedDynamic)
				{
					FRigVMNestedByteArray* Storage = (FRigVMNestedByteArray*)TargetHandle.Ptr;
					if (Context.GetSlice().GetIndex() == 0)
					{
						Storage->Reset();
					}
					int32 ArrayIndex = Storage->Add(FRigVMByteArray());
					(*Storage)[ArrayIndex].AddZeroed(NumBytes);
					TargetPtr = (*Storage)[ArrayIndex].GetData();
				}

				switch (Op.RegisterType)
				{
					case ERigVMRegisterType::Plain:
					{
						switch(Op.CopyType)
						{
							case ERigVMCopyType::FloatToDouble:
							{
								const float* Floats = (float*)SourcePtr;
								double* Doubles = (double*)TargetPtr;
								const int32 NumElementsToCopy = NumBytes / sizeof(double);

								for(int32 ElementIndex = 0; ElementIndex < NumElementsToCopy; ElementIndex++)
								{
									Doubles[ElementIndex] = (double)Floats[ElementIndex];
								}
								break;
							}
							case ERigVMCopyType::DoubleToFloat:
							{
								const double* Doubles = (double*)SourcePtr;
								float* Floats = (float*)TargetPtr;
								const int32 NumElementsToCopy = NumBytes / sizeof(float); 

								for(int32 ElementIndex = 0; ElementIndex < NumElementsToCopy; ElementIndex++)
								{
									Floats[ElementIndex] = (float)Doubles[ElementIndex];
								}
								break;
							}
							case ERigVMCopyType::Default:
							default:
							{
								FMemory::Memcpy(TargetPtr, SourcePtr, NumBytes);
								break;
							}
						}
						break;
					}
					case ERigVMRegisterType::Name:
					{
						int32 NumNames = NumBytes / sizeof(FName);
						FRigVMFixedArray<FName> TargetNames((FName*)TargetPtr, NumNames);
						FRigVMFixedArray<FName> SourceNames((FName*)SourcePtr, NumNames);
						for (int32 Index = 0; Index < NumNames; Index++)
						{
							TargetNames[Index] = SourceNames[Index];
						}
						break;
					}
					case ERigVMRegisterType::String:
					{
						int32 NumStrings = NumBytes / sizeof(FString);
						FRigVMFixedArray<FString> TargetStrings((FString*)TargetPtr, NumStrings);
						FRigVMFixedArray<FString> SourceStrings((FString*)SourcePtr, NumStrings);
						for (int32 Index = 0; Index < NumStrings; Index++)
						{
							TargetStrings[Index] = SourceStrings[Index];
						}
						break;
					}
					case ERigVMRegisterType::Struct:
					{
						UScriptStruct* ScriptStruct = (UScriptStruct*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2].GetData();
						int32 NumStructs = NumBytes / ScriptStruct->GetStructureSize();
						if (NumStructs > 0 && TargetPtr)
						{
							ScriptStruct->CopyScriptStruct(TargetPtr, SourcePtr, NumStructs);
						}
						break;
					}
					default:
					{
						// the default pass for any complex memory
						Memory[Op.Target.GetContainerIndex()]->Copy(Op.Source, Op.Target, Memory[Op.Source.GetContainerIndex()]);
						break;
					}
				}
#else

				URigVMMemoryStorage::CopyProperty(TargetHandle, SourceHandle);

#endif
				break;
			}
			case ERigVMOpCode::Increment:
			case ERigVMOpCode::Decrement:
			case ERigVMOpCode::Equals:
			case ERigVMOpCode::NotEquals:
			case ERigVMOpCode::JumpAbsolute:
			case ERigVMOpCode::JumpForward:
			case ERigVMOpCode::JumpBackward:
			case ERigVMOpCode::JumpAbsoluteIf:
			case ERigVMOpCode::JumpForwardIf:
			case ERigVMOpCode::JumpBackwardIf:
			case ERigVMOpCode::ChangeType:
			case ERigVMOpCode::BeginBlock:
			case ERigVMOpCode::EndBlock:
			case ERigVMOpCode::Exit:
			case ERigVMOpCode::ArrayGetNum:
			case ERigVMOpCode::ArraySetNum:
			case ERigVMOpCode::ArrayAppend:
			case ERigVMOpCode::ArrayClone:
			case ERigVMOpCode::ArrayGetAtIndex:
			case ERigVMOpCode::ArraySetAtIndex:
			case ERigVMOpCode::ArrayInsert:
			case ERigVMOpCode::ArrayRemove:
			case ERigVMOpCode::ArrayAdd:
			case ERigVMOpCode::ArrayFind:
			case ERigVMOpCode::ArrayIterator:
			case ERigVMOpCode::ArrayUnion:
			case ERigVMOpCode::ArrayDifference:
			case ERigVMOpCode::ArrayIntersection:
			case ERigVMOpCode::ArrayReverse:

			{
				break;
			}
			case ERigVMOpCode::Invalid:
			{
				ensure(false);
				return false;
			}
		}
		Context.InstructionIndex++;
	}
	
	return true;
}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
bool URigVM::Execute(FRigVMMemoryContainerPtrArray Memory, FRigVMFixedArray<void*> AdditionalArguments, const FName& InEntryName)
#else
bool URigVM::Execute(TArrayView<URigVMMemoryStorage*> Memory, TArrayView<void*> AdditionalArguments, const FName& InEntryName)
#endif
{
	if (ExecutingThreadId != INDEX_NONE)
	{
		ensureMsgf(ExecutingThreadId == FPlatformTLS::GetCurrentThreadId(), TEXT("RigVM::Execute from multiple threads (%d and %d)"), ExecutingThreadId, (int32)FPlatformTLS::GetCurrentThreadId());
	}
	CopyDeferredVMIfRequired();
	TGuardValue<int32> GuardThreadId(ExecutingThreadId, FPlatformTLS::GetCurrentThreadId());

	ResolveFunctionsIfRequired();
	RefreshInstructionsIfRequired();

	if (Instructions.Num() == 0)
	{
		return true;
	}

	// changes to the layout of memory array should be reflected in GetContainerIndex()
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	FRigVMMemoryContainer* LocalMemory[] = { WorkMemoryPtr, LiteralMemoryPtr, DebugMemoryPtr };
	if (Memory.Num() == 0)
	{
		Memory = FRigVMMemoryContainerPtrArray(LocalMemory, 3);
	}
#else
	TArray<URigVMMemoryStorage*> LocalMemory;
	if (Memory.Num() == 0)
	{
		LocalMemory = GetLocalMemoryArray();
		Memory = LocalMemory;
	}
#endif
	
	CacheMemoryHandlesIfRequired(Memory);
	FRigVMByteCode& ByteCode = GetByteCode();
	TArray<FRigVMFunctionPtr>& Functions = GetFunctions();

#if WITH_EDITOR
	TArray<FName>& FunctionNames = GetFunctionNames();

	if (FirstEntryEventInQueue == NAME_None || FirstEntryEventInQueue == InEntryName)
	{
		InstructionVisitedDuringLastRun.Reset();
		InstructionVisitOrder.Reset();
		InstructionVisitedDuringLastRun.SetNumZeroed(Instructions.Num());
		InstructionCyclesDuringLastRun.Reset();
		if(Context.RuntimeSettings.bEnableProfiling)
		{
			InstructionCyclesDuringLastRun.SetNumUninitialized(Instructions.Num());
			for(int32 DurationIndex=0;DurationIndex<InstructionCyclesDuringLastRun.Num();DurationIndex++)
			{
				InstructionCyclesDuringLastRun[DurationIndex] = UINT64_MAX;
			}
		}
	}
#endif

	Context.Reset();
	Context.SliceOffsets.AddZeroed(Instructions.Num());
	Context.OpaqueArguments = AdditionalArguments;
	Context.ExternalVariables = ExternalVariables;

	TGuardValue<URigVM*> VMInContext(Context.VM, this);

	ClearDebugMemory();

	if (!InEntryName.IsNone())
	{
		int32 EntryIndex = ByteCode.FindEntryIndex(InEntryName);
		if (EntryIndex == INDEX_NONE)
		{
			return false;
		}
		Context.InstructionIndex = (uint16)ByteCode.GetEntry(EntryIndex).InstructionIndex;
	}

#if WITH_EDITOR
	if (DebugInfo)
	{
		DebugInfo->StartExecution();
	}
#endif

	NumExecutions++;

#if WITH_EDITOR
	uint64 StartCycles = 0;
	uint64 OverallCycles = 0;
	if(Context.RuntimeSettings.bEnableProfiling)
	{
		StartCycles = FPlatformTime::Cycles64();
	}
#endif

	while (Instructions.IsValidIndex(Context.InstructionIndex))
	{
#if WITH_EDITOR
		if (DebugInfo && ShouldHaltAtInstruction(InEntryName, Context.InstructionIndex))
		{
			return true;
		}

		const int32 CurrentInstructionIndex = Context.InstructionIndex;
		InstructionVisitedDuringLastRun[Context.InstructionIndex]++;
		InstructionVisitOrder.Add(Context.InstructionIndex);
	
#endif

		const FRigVMInstruction& Instruction = Instructions[Context.InstructionIndex];

		switch (Instruction.OpCode)
		{
			case ERigVMOpCode::Execute_0_Operands:
			case ERigVMOpCode::Execute_1_Operands:
			case ERigVMOpCode::Execute_2_Operands:
			case ERigVMOpCode::Execute_3_Operands:
			case ERigVMOpCode::Execute_4_Operands:
			case ERigVMOpCode::Execute_5_Operands:
			case ERigVMOpCode::Execute_6_Operands:
			case ERigVMOpCode::Execute_7_Operands:
			case ERigVMOpCode::Execute_8_Operands:
			case ERigVMOpCode::Execute_9_Operands:
			case ERigVMOpCode::Execute_10_Operands:
			case ERigVMOpCode::Execute_11_Operands:
			case ERigVMOpCode::Execute_12_Operands:
			case ERigVMOpCode::Execute_13_Operands:
			case ERigVMOpCode::Execute_14_Operands:
			case ERigVMOpCode::Execute_15_Operands:
			case ERigVMOpCode::Execute_16_Operands:
			case ERigVMOpCode::Execute_17_Operands:
			case ERigVMOpCode::Execute_18_Operands:
			case ERigVMOpCode::Execute_19_Operands:
			case ERigVMOpCode::Execute_20_Operands:
			case ERigVMOpCode::Execute_21_Operands:
			case ERigVMOpCode::Execute_22_Operands:
			case ERigVMOpCode::Execute_23_Operands:
			case ERigVMOpCode::Execute_24_Operands:
			case ERigVMOpCode::Execute_25_Operands:
			case ERigVMOpCode::Execute_26_Operands:
			case ERigVMOpCode::Execute_27_Operands:
			case ERigVMOpCode::Execute_28_Operands:
			case ERigVMOpCode::Execute_29_Operands:
			case ERigVMOpCode::Execute_30_Operands:
			case ERigVMOpCode::Execute_31_Operands:
			case ERigVMOpCode::Execute_32_Operands:
			case ERigVMOpCode::Execute_33_Operands:
			case ERigVMOpCode::Execute_34_Operands:
			case ERigVMOpCode::Execute_35_Operands:
			case ERigVMOpCode::Execute_36_Operands:
			case ERigVMOpCode::Execute_37_Operands:
			case ERigVMOpCode::Execute_38_Operands:
			case ERigVMOpCode::Execute_39_Operands:
			case ERigVMOpCode::Execute_40_Operands:
			case ERigVMOpCode::Execute_41_Operands:
			case ERigVMOpCode::Execute_42_Operands:
			case ERigVMOpCode::Execute_43_Operands:
			case ERigVMOpCode::Execute_44_Operands:
			case ERigVMOpCode::Execute_45_Operands:
			case ERigVMOpCode::Execute_46_Operands:
			case ERigVMOpCode::Execute_47_Operands:
			case ERigVMOpCode::Execute_48_Operands:
			case ERigVMOpCode::Execute_49_Operands:
			case ERigVMOpCode::Execute_50_Operands:
			case ERigVMOpCode::Execute_51_Operands:
			case ERigVMOpCode::Execute_52_Operands:
			case ERigVMOpCode::Execute_53_Operands:
			case ERigVMOpCode::Execute_54_Operands:
			case ERigVMOpCode::Execute_55_Operands:
			case ERigVMOpCode::Execute_56_Operands:
			case ERigVMOpCode::Execute_57_Operands:
			case ERigVMOpCode::Execute_58_Operands:
			case ERigVMOpCode::Execute_59_Operands:
			case ERigVMOpCode::Execute_60_Operands:
			case ERigVMOpCode::Execute_61_Operands:
			case ERigVMOpCode::Execute_62_Operands:
			case ERigVMOpCode::Execute_63_Operands:
			case ERigVMOpCode::Execute_64_Operands:
			{
				const FRigVMExecuteOp& Op = ByteCode.GetOpAt<FRigVMExecuteOp>(Instruction);
				const int32 OperandCount = FirstHandleForInstruction[Context.InstructionIndex + 1] - FirstHandleForInstruction[Context.InstructionIndex];
				FRigVMMemoryHandleArray Handles(&CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]], OperandCount);
#if WITH_EDITOR
				Context.FunctionName = FunctionNames[Op.FunctionIndex];
#endif
				(*Functions[Op.FunctionIndex])(Context, Handles);

#if WITH_EDITOR

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				if(DebugMemoryPtr->Num() > 0)
				{
					const FRigVMOperandArray Operands = ByteCode.GetOperandsForExecuteOp(Instruction);
					for(int32 OperandIndex = 0, HandleIndex = 0; OperandIndex < Operands.Num() && HandleIndex < Handles.Num(); HandleIndex++)
					{
						// skip array sizes
						if(Handles[HandleIndex].GetType() == FRigVMMemoryHandle::FType::ArraySize)
						{
							continue;
						}
						CopyOperandForDebuggingIfNeeded(Operands[OperandIndex++], Handles[HandleIndex]);
					}
				}
#else
				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMOperandArray Operands = ByteCode.GetOperandsForExecuteOp(Instruction);
					for(int32 OperandIndex = 0, HandleIndex = 0; OperandIndex < Operands.Num() && HandleIndex < Handles.Num(); HandleIndex++)
					{
						CopyOperandForDebuggingIfNeeded(Operands[OperandIndex++], Handles[HandleIndex]);
					}
				}
#endif

#endif

				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Zero:
			{
				*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData()) = 0;
#if WITH_EDITOR
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				if(DebugMemoryPtr->Num() > 0)
#else
				if(DebugMemoryStorageObject->Num() > 0)
#endif
				{
					const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.Arg, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]);
				}
#endif

				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::BoolFalse:
			{
				*((bool*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData()) = false;
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::BoolTrue:
			{
				*((bool*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData()) = true;
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Copy:
			{
				const FRigVMCopyOp& Op = ByteCode.GetOpAt<FRigVMCopyOp>(Instruction);

				FRigVMMemoryHandle& SourceHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				FRigVMMemoryHandle& TargetHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1];

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				void* SourcePtr = SourceHandle;
				void* TargetPtr = TargetHandle;

				const uint64 NumBytes = (uint64)Op.NumBytes;

				if (TargetHandle.Type == FRigVMMemoryHandle::Dynamic)
				{
					FRigVMByteArray* Storage = (FRigVMByteArray*)TargetHandle.Ptr;
					if (Context.GetSlice().GetIndex() == 0)
					{
						Storage->Reset();
					}
					const int32 ByteIndex = Storage->AddZeroed(NumBytes);
					TargetPtr = Storage->GetData() + ByteIndex;
				}
				else if (TargetHandle.Type == FRigVMMemoryHandle::NestedDynamic)
				{
					FRigVMNestedByteArray* Storage = (FRigVMNestedByteArray*)TargetHandle.Ptr;
					if (Context.GetSlice().GetIndex() == 0)
					{
						Storage->Reset();
					}
					const int32 ArrayIndex = Storage->Add(FRigVMByteArray());
					(*Storage)[ArrayIndex].AddZeroed(NumBytes);
					TargetPtr = (*Storage)[ArrayIndex].GetData();
				}

				switch (Op.RegisterType)
				{
					case ERigVMRegisterType::Plain:
					{
						switch(Op.CopyType)
						{
							case ERigVMCopyType::FloatToDouble:
							{
								const float* Floats = (float*)SourcePtr;
								double* Doubles = (double*)TargetPtr;
								const int32 NumElementsToCopy = NumBytes / sizeof(double);

								for(int32 ElementIndex = 0; ElementIndex < NumElementsToCopy; ElementIndex++)
								{
									Doubles[ElementIndex] = (double)Floats[ElementIndex];
								}
								break;
							}
							case ERigVMCopyType::DoubleToFloat:
							{
								const double* Doubles = (double*)SourcePtr;
								float* Floats = (float*)TargetPtr;
								const int32 NumElementsToCopy = NumBytes / sizeof(float); 

								for(int32 ElementIndex = 0; ElementIndex < NumElementsToCopy; ElementIndex++)
								{
									Floats[ElementIndex] = (float)Doubles[ElementIndex];
								}
								break;
							}
							case ERigVMCopyType::Default:
							default:
							{
								FMemory::Memcpy(TargetPtr, SourcePtr, NumBytes);
								break;
							}
						}
						break;
					}
					case ERigVMRegisterType::Name:
					{
						const int32 NumNames = NumBytes / sizeof(FName);
						FRigVMFixedArray<FName> TargetNames((FName*)TargetPtr, NumNames);
						FRigVMFixedArray<FName> SourceNames((FName*)SourcePtr, NumNames);
						for (int32 Index = 0; Index < NumNames; Index++)
						{
							TargetNames[Index] = SourceNames[Index];
						}
						break;
					}
					case ERigVMRegisterType::String:
					{
						const int32 NumStrings = NumBytes / sizeof(FString);
						FRigVMFixedArray<FString> TargetStrings((FString*)TargetPtr, NumStrings);
						FRigVMFixedArray<FString> SourceStrings((FString*)SourcePtr, NumStrings);
						for (int32 Index = 0; Index < NumStrings; Index++)
						{
							TargetStrings[Index] = SourceStrings[Index];
						}
						break;
					}
					case ERigVMRegisterType::Struct:
					{
						UScriptStruct* ScriptStruct = (UScriptStruct*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2].GetData();
						const int32 NumStructs = NumBytes / ScriptStruct->GetStructureSize();
						if (NumStructs > 0 && TargetPtr)
						{
							ScriptStruct->CopyScriptStruct(TargetPtr, SourcePtr, NumStructs);
						}
						break;
					}
					default:
					{
						// the default pass for any complex memory
						Memory[Op.Target.GetContainerIndex()]->Copy(Op.Source, Op.Target, Memory[Op.Source.GetContainerIndex()]);
						break;
					}
				}
					
#else
					
				URigVMMemoryStorage::CopyProperty(TargetHandle, SourceHandle);
					
#endif
					
#if WITH_EDITOR
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				if(DebugMemoryPtr->Num() > 0)
#else
				if(DebugMemoryStorageObject->Num() > 0)
#endif
				{
					CopyOperandForDebuggingIfNeeded(Op.Source, SourceHandle);
				}
#endif
					
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Increment:
			{
				(*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData()))++;
#if WITH_EDITOR
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				if(DebugMemoryPtr->Num() > 0)
#else
				if(DebugMemoryStorageObject->Num() > 0)
#endif
				{
					const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.Arg, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Decrement:
			{
				(*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData()))--;
#if WITH_EDITOR
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				if(DebugMemoryPtr->Num() > 0)
#else
				if(DebugMemoryStorageObject->Num() > 0)
#endif
				{
					const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.Arg, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Equals:
			case ERigVMOpCode::NotEquals:
			{
				const FRigVMComparisonOp& Op = ByteCode.GetOpAt<FRigVMComparisonOp>(Instruction);

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

				const FRigVMRegister& RegisterA = (*Memory[Op.A.GetContainerIndex()])[Op.A.GetRegisterIndex()];
				const FRigVMRegister& RegisterB = (*Memory[Op.B.GetContainerIndex()])[Op.B.GetRegisterIndex()];
				const uint16 BytesA = RegisterA.GetNumBytesPerSlice();
				const uint16 BytesB = RegisterB.GetNumBytesPerSlice();
				
				bool Result = false;
				if (BytesA == BytesB && RegisterA.Type == RegisterB.Type && RegisterA.ScriptStructIndex == RegisterB.ScriptStructIndex)
				{
					switch (RegisterA.Type)
					{
						case ERigVMRegisterType::Plain:
						case ERigVMRegisterType::Name:
						{
							void* DataA = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData();
							void* DataB = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]+1].GetData();
							Result = FMemory::Memcmp(DataA, DataB, BytesA) == 0;
							break;
						}
						case ERigVMRegisterType::String:
						{
							FRigVMFixedArray<FString> StringsA = Memory[Op.A.GetContainerIndex()]->GetFixedArray<FString>(Op.A.GetRegisterIndex());
							FRigVMFixedArray<FString> StringsB = Memory[Op.B.GetContainerIndex()]->GetFixedArray<FString>(Op.B.GetRegisterIndex());

							Result = true;
							for (int32 StringIndex = 0; StringIndex < StringsA.Num(); StringIndex++)
							{
								if (StringsA[StringIndex] != StringsB[StringIndex])
								{
									Result = false;
									break;
								}
							}
							break;
						}
						case ERigVMRegisterType::Struct:
						{
							UScriptStruct* ScriptStruct = Memory[Op.A.GetContainerIndex()]->GetScriptStruct(RegisterA.ScriptStructIndex);

							uint8* DataA = (uint8*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData();
							uint8* DataB = (uint8*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]+1].GetData();
							
							Result = true;
							for (int32 ElementIndex = 0; ElementIndex < RegisterA.ElementCount; ElementIndex++)
							{
								if (!ScriptStruct->CompareScriptStruct(DataA, DataB, 0))
								{
									Result = false;
									break;
								}
								DataA += RegisterA.ElementSize;
								DataB += RegisterB.ElementSize;
							}

							break;
						}
						case ERigVMRegisterType::Invalid:
						{
							break;
						}
					}
				}
				if (Op.OpCode == ERigVMOpCode::NotEquals)
				{
					Result = !Result;
				}

#else

				FRigVMMemoryHandle& HandleA = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				FRigVMMemoryHandle& HandleB = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1];
				const bool Result = HandleA.GetProperty()->Identical(HandleA.GetData(true), HandleB.GetData(true));
					
#endif				

				*((bool*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]+2].GetData()) = Result;
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::JumpAbsolute:
			{
				const FRigVMJumpOp& Op = ByteCode.GetOpAt<FRigVMJumpOp>(Instruction);
				Context.InstructionIndex = Op.InstructionIndex;
				break;
			}
			case ERigVMOpCode::JumpForward:
			{
				const FRigVMJumpOp& Op = ByteCode.GetOpAt<FRigVMJumpOp>(Instruction);
				Context.InstructionIndex += Op.InstructionIndex;
				break;
			}
			case ERigVMOpCode::JumpBackward:
			{
				const FRigVMJumpOp& Op = ByteCode.GetOpAt<FRigVMJumpOp>(Instruction);
				Context.InstructionIndex -= Op.InstructionIndex;
				break;
			}
			case ERigVMOpCode::JumpAbsoluteIf:
			{
				const FRigVMJumpIfOp& Op = ByteCode.GetOpAt<FRigVMJumpIfOp>(Instruction);
				const bool Condition = *(bool*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData();
				if (Condition == Op.Condition)
				{
					Context.InstructionIndex = Op.InstructionIndex;
				}
				else
				{
					Context.InstructionIndex++;
				}
				break;
			}
			case ERigVMOpCode::JumpForwardIf:
			{
				const FRigVMJumpIfOp& Op = ByteCode.GetOpAt<FRigVMJumpIfOp>(Instruction);
				const bool Condition = *(bool*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData();
				if (Condition == Op.Condition)
				{
					Context.InstructionIndex += Op.InstructionIndex;
				}
				else
				{
					Context.InstructionIndex++;
				}
				break;
			}
			case ERigVMOpCode::JumpBackwardIf:
			{
				const FRigVMJumpIfOp& Op = ByteCode.GetOpAt<FRigVMJumpIfOp>(Instruction);
				const bool Condition = *(bool*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData();
				if (Condition == Op.Condition)
				{
					Context.InstructionIndex -= Op.InstructionIndex;
				}
				else
				{
					Context.InstructionIndex++;
				}
				break;
			}
			case ERigVMOpCode::ChangeType:
			{
				ensureMsgf(false, TEXT("not implemented."));
				break;
			}
			case ERigVMOpCode::Exit:
			{
#if WITH_EDITOR
				Context.LastExecutionMicroSeconds = OverallCycles * FPlatformTime::GetSecondsPerCycle() * 1000.0 * 1000.0;
#endif
				ExecutionReachedExit().Broadcast(InEntryName);
#if WITH_EDITOR					
				if (HaltedAtBreakpoint != nullptr)
				{
					HaltedAtBreakpoint = nullptr;
					DebugInfo->SetCurrentActiveBreakpoint(nullptr);
					ExecutionHalted().Broadcast(INDEX_NONE, nullptr, InEntryName);
				}
#endif
				return true;
			}
			case ERigVMOpCode::BeginBlock:
			{
				const int32 Count = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]].GetData()));
				const int32 Index = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1].GetData()));
				Context.BeginSlice(Count, Index);
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::EndBlock:
			{
				Context.EndSlice();
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayReset:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]; 					
				FScriptArrayHelper ArrayHelper(CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty()), ArrayHandle.GetData());
				ArrayHelper.Resize(0);

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.Arg, ArrayHandle);
				}
#endif

				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayGetNum:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]; 					
				FScriptArrayHelper ArrayHelper(CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty()), ArrayHandle.GetData());
				int32& Count = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1].GetData()));
				Count = ArrayHelper.Num();

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]);
				}
#endif	
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArraySetNum:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]; 					
				FScriptArrayHelper ArrayHelper(CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty()), ArrayHandle.GetData());
				const int32 Count = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1].GetData()));
				if(Context.IsValidArraySize(Count))
				{
					ArrayHelper.Resize(Count);
				}

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayAppend:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]; 					
				FRigVMMemoryHandle& OtherArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1];
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty()); 
				const FArrayProperty* OtherArrayProperty = CastFieldChecked<FArrayProperty>(OtherArrayHandle.GetProperty()); 

				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayHandle.GetData());
				FScriptArrayHelper OtherArrayHelper(OtherArrayProperty, OtherArrayHandle.GetData());

				if(OtherArrayHelper.Num() > 0)
				{
					if(Context.IsValidArraySize(ArrayHelper.Num() + OtherArrayHelper.Num()))
					{
						const FProperty* TargetProperty = ArrayProperty->Inner;
						const FProperty* SourceProperty = OtherArrayProperty->Inner;

						int32 TargetIndex = ArrayHelper.AddValues(OtherArrayHelper.Num());
						for(int32 SourceIndex = 0; SourceIndex < OtherArrayHelper.Num(); SourceIndex++, TargetIndex++)
						{
							uint8* TargetMemory = ArrayHelper.GetRawPtr(TargetIndex);
							const uint8* SourceMemory = OtherArrayHelper.GetRawPtr(SourceIndex);
							URigVMMemoryStorage::CopyProperty(TargetProperty, TargetMemory, SourceProperty, SourceMemory);
						}
					}
				}

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayClone:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]; 					
				FRigVMMemoryHandle& ClonedArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1];
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty()); 
				const FArrayProperty* ClonedArrayProperty = CastFieldChecked<FArrayProperty>(ClonedArrayHandle.GetProperty()); 
				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayHandle.GetData());
				FScriptArrayHelper ClonedArrayHelper(ClonedArrayProperty, ClonedArrayHandle.GetData());

				CopyArray(ClonedArrayHelper, ClonedArrayHandle, ArrayHelper, ArrayHandle);
					
				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, ClonedArrayHandle);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayGetAtIndex:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty());
				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayHandle.GetData());
				const int32 Index = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1].GetData()));
				if(Context.IsValidArrayIndex(Index, ArrayHelper.Num()))
				{
					FRigVMMemoryHandle& ElementHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2];
					uint8* TargetMemory = ElementHandle.GetData();
					const uint8* SourceMemory = ArrayHelper.GetRawPtr(Index);
					URigVMMemoryStorage::CopyProperty(ElementHandle.GetProperty(), TargetMemory, ArrayProperty->Inner, SourceMemory);
				}

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]);
					CopyOperandForDebuggingIfNeeded(Op.ArgC, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArraySetAtIndex:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty());
				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayHandle.GetData());
				const int32 Index = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1].GetData()));
				if(Context.IsValidArrayIndex(Index, ArrayHelper.Num()))
				{
					FRigVMMemoryHandle& ElementHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2];
					uint8* TargetMemory = ArrayHelper.GetRawPtr(Index);
					const uint8* SourceMemory = ElementHandle.GetData();
					URigVMMemoryStorage::CopyProperty(ArrayProperty->Inner, TargetMemory, ElementHandle.GetProperty(), SourceMemory);
				}

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]);
					CopyOperandForDebuggingIfNeeded(Op.ArgC, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayInsert:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty());
				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayHandle.GetData());
				if(Context.IsValidArraySize(ArrayHelper.Num() + 1))
				{
					int32 Index = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1].GetData()));
					Index = FMath::Clamp<int32>(Index, 0, ArrayHelper.Num());
					ArrayHelper.InsertValues(Index, 1);

					FRigVMMemoryHandle& ElementHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2];
					uint8* TargetMemory = ArrayHelper.GetRawPtr(Index);
					const uint8* SourceMemory = ElementHandle.GetData();
					URigVMMemoryStorage::CopyProperty(ArrayProperty->Inner, TargetMemory, ElementHandle.GetProperty(), SourceMemory);
				}

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]);
					CopyOperandForDebuggingIfNeeded(Op.ArgC, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayRemove:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty());
				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayHandle.GetData());
				const int32 Index = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1].GetData()));
				if(Context.IsValidArrayIndex(Index, ArrayHelper.Num()))
				{
					ArrayHelper.RemoveValues(Index, 1);
				}

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayAdd:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty());
				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayHandle.GetData());
				int32& Index = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2].GetData()));
				if(Context.IsValidArraySize(ArrayHelper.Num() + 1))
				{
					FRigVMMemoryHandle& ElementHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1];
					Index = ArrayHelper.AddValue();

					uint8* TargetMemory = ArrayHelper.GetRawPtr(Index);
					const uint8* SourceMemory = ElementHandle.GetData();
					URigVMMemoryStorage::CopyProperty(ArrayProperty->Inner, TargetMemory, ElementHandle.GetProperty(), SourceMemory);
				}
				else
				{
					Index = INDEX_NONE;
				}

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]);
					CopyOperandForDebuggingIfNeeded(Op.ArgC, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayFind:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty());
				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayHandle.GetData());

				FRigVMMemoryHandle& ElementHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1];
				int32& FoundIndex = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2].GetData()));
				bool& bFound = (*((bool*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 3].GetData()));

				FoundIndex = INDEX_NONE;
				bFound = false;

				const FProperty* PropertyA = ElementHandle.GetProperty();
				const FProperty* PropertyB = ArrayProperty->Inner;

				if(PropertyA->SameType(PropertyB))
				{
					const uint8* MemoryA = ElementHandle.GetData();

					for(int32 Index = 0; Index < ArrayHelper.Num(); Index++)
					{
						const uint8* MemoryB = ArrayHelper.GetRawPtr(Index);
						if(PropertyA->Identical(MemoryA, MemoryB))
						{
							FoundIndex = Index;
							bFound = true;
							break;
						}
					}
				}
				else
				{
					static const TCHAR IncompatibleTypes[] = TEXT("Array('%s') doesn't support searching for element('%$s').");
					Context.Logf(EMessageSeverity::Error, IncompatibleTypes, *PropertyB->GetCPPType(), *PropertyA->GetCPPType());
				}

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMQuaternaryOp& Op = ByteCode.GetOpAt<FRigVMQuaternaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle);
					CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]);
					CopyOperandForDebuggingIfNeeded(Op.ArgC, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2]);
					CopyOperandForDebuggingIfNeeded(Op.ArgD, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 3]);
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayIterator:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]];
				const FArrayProperty* ArrayProperty = CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty());
				FScriptArrayHelper ArrayHelper(ArrayProperty, ArrayHandle.GetData());

				FRigVMMemoryHandle& ElementHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1];
				const int32& Index = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2].GetData()));
				int32& Count = (*((int32*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 3].GetData()));
				float& Ratio = (*((float*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 4].GetData()));
				bool& bContinue = (*((bool*)CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 5].GetData()));

				Count = ArrayHelper.Num();
				bContinue = Index >=0 && Index < Count;

				if((Count <= 0) || !bContinue)
				{
					Ratio = 0.f;
				}
				else
				{
					Ratio = float(Index) / float(Count - 1);

					uint8* TargetMemory = ElementHandle.GetData();
					const uint8* SourceMemory = ArrayHelper.GetRawPtr(Index);
					URigVMMemoryStorage::CopyProperty(ElementHandle.GetProperty(), TargetMemory, ArrayProperty->Inner, SourceMemory);

					if(DebugMemoryStorageObject->Num() > 0)
					{
						const FRigVMSenaryOp& Op = ByteCode.GetOpAt<FRigVMSenaryOp>(Instruction);
						CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandle); // array
						CopyOperandForDebuggingIfNeeded(Op.ArgD, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 3]); // count

						Context.BeginSlice(Count, Index);
						CopyOperandForDebuggingIfNeeded(Op.ArgB, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]); // element
						CopyOperandForDebuggingIfNeeded(Op.ArgC, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2]); // index
						CopyOperandForDebuggingIfNeeded(Op.ArgE, CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 4]); // ratio
						Context.EndSlice();
					}
				}

#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayUnion:
			case ERigVMOpCode::ArrayDifference:
			case ERigVMOpCode::ArrayIntersection:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandleA = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]; 					
				FRigVMMemoryHandle& ArrayHandleB = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 1]; 					
				FScriptArrayHelper ArrayHelperA(CastFieldChecked<FArrayProperty>(ArrayHandleA.GetProperty()), ArrayHandleA.GetData());
				FScriptArrayHelper ArrayHelperB(CastFieldChecked<FArrayProperty>(ArrayHandleB.GetProperty()), ArrayHandleB.GetData());
				const FArrayProperty* ArrayPropertyA = CastFieldChecked<FArrayProperty>(ArrayHandleA.GetProperty());
				const FArrayProperty* ArrayPropertyB = CastFieldChecked<FArrayProperty>(ArrayHandleB.GetProperty());
				const FProperty* ElementPropertyA = ArrayPropertyA->Inner;
				const FProperty* ElementPropertyB = ArrayPropertyB->Inner;

				TMap<uint32, int32> HashA, HashB;
				HashA.Reserve(ArrayHelperA.Num());
				HashB.Reserve(ArrayHelperB.Num());

				for(int32 Index = 0; Index < ArrayHelperA.Num(); Index++)
				{
					uint32 HashValue;
					if (ElementPropertyA->PropertyFlags & CPF_HasGetValueTypeHash)
					{
						HashValue = ElementPropertyA->GetValueTypeHash(ArrayHelperA.GetRawPtr(Index));
					}
					else
					{
						FString Value;
						ElementPropertyA->ExportTextItem(Value, ArrayHelperA.GetRawPtr(Index), nullptr, nullptr, PPF_None);
						HashValue = TextKeyUtil::HashString(Value);
					}
					
					if(!HashA.Contains(HashValue))
					{
						HashA.Add(HashValue, Index);
					}
				}
				for(int32 Index = 0; Index < ArrayHelperB.Num(); Index++)
				{
					uint32 HashValue;
					if (ElementPropertyB->PropertyFlags & CPF_HasGetValueTypeHash)
					{
						HashValue = ElementPropertyB->GetValueTypeHash(ArrayHelperB.GetRawPtr(Index));
					}
					else
					{
						FString Value;
						ElementPropertyB->ExportTextItem(Value, ArrayHelperB.GetRawPtr(Index), nullptr, nullptr, PPF_None);
						HashValue = TextKeyUtil::HashString(Value);
					}
					if(!HashB.Contains(HashValue))
					{
						HashB.Add(HashValue, Index);
					}
				}

				if(Instruction.OpCode == ERigVMOpCode::ArrayUnion)
				{
					// copy the complete array to a temp storage
					TArray<uint8, TAlignedHeapAllocator<16>> TempStorage;
					const int32 NumElementsA = ArrayHelperA.Num();
					TempStorage.AddZeroed(NumElementsA * ElementPropertyA->GetSize());
					uint8* TempMemory = TempStorage.GetData();
					for(int32 Index = 0; Index < NumElementsA; Index++)
					{
						ElementPropertyA->InitializeValue(TempMemory);
						ElementPropertyA->CopyCompleteValue(TempMemory, ArrayHelperA.GetRawPtr(Index));
						TempMemory += ElementPropertyA->GetSize();
					}

					ArrayHelperA.Resize(0);

					for(const TPair<uint32, int32>& Pair : HashA)
					{
						int32 AddedIndex = ArrayHelperA.AddValue();
						TempMemory = TempStorage.GetData() + Pair.Value * ElementPropertyA->GetSize();
						
						URigVMMemoryStorage::CopyProperty(
							ElementPropertyA,
							ArrayHelperA.GetRawPtr(AddedIndex),
							ElementPropertyA,
							TempMemory
						);
					}

					TempMemory = TempStorage.GetData();
					for(int32 Index = 0; Index < NumElementsA; Index++)
					{
						ElementPropertyA->DestroyValue(TempMemory);
						TempMemory += ElementPropertyA->GetSize();
					}

					for(const TPair<uint32, int32>& Pair : HashB)
					{
						if(!HashA.Contains(Pair.Key))
						{
							int32 AddedIndex = ArrayHelperA.AddValue();
							
							URigVMMemoryStorage::CopyProperty(
								ElementPropertyA,
								ArrayHelperA.GetRawPtr(AddedIndex),
								ElementPropertyB,
								ArrayHelperB.GetRawPtr(Pair.Value)
							);
						}
					}
					
					if(DebugMemoryStorageObject->Num() > 0)
					{
						const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instruction);
						CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandleA);
						CopyOperandForDebuggingIfNeeded(Op.ArgB, ArrayHandleB);
					}
				}
				else
				{
					FRigVMMemoryHandle& ResultArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex] + 2]; 					
					FScriptArrayHelper ResultArrayHelper(CastFieldChecked<FArrayProperty>(ResultArrayHandle.GetProperty()), ResultArrayHandle.GetData());
					const FArrayProperty* ResultArrayProperty = CastFieldChecked<FArrayProperty>(ResultArrayHandle.GetProperty());
					const FProperty* ResultElementProperty = ResultArrayProperty->Inner;

					ResultArrayHelper.Resize(0);
					
					if(Instruction.OpCode == ERigVMOpCode::ArrayDifference)
					{
						for(const TPair<uint32, int32>& Pair : HashA)
						{
							if(!HashB.Contains(Pair.Key))
							{
								int32 AddedIndex = ResultArrayHelper.AddValue();
								URigVMMemoryStorage::CopyProperty(
									ResultElementProperty,
									ResultArrayHelper.GetRawPtr(AddedIndex),
									ElementPropertyA,
									ArrayHelperA.GetRawPtr(Pair.Value)
								);
							}
						}
						for(const TPair<uint32, int32>& Pair : HashB)
						{
							if(!HashA.Contains(Pair.Key))
							{
								int32 AddedIndex = ResultArrayHelper.AddValue();
								URigVMMemoryStorage::CopyProperty(
									ResultElementProperty,
									ResultArrayHelper.GetRawPtr(AddedIndex),
									ElementPropertyB,
									ArrayHelperB.GetRawPtr(Pair.Value)
								);
							}
						}
					}
					else // intersection
					{
						for(const TPair<uint32, int32>& Pair : HashA)
						{
							if(HashB.Contains(Pair.Key))
							{
								int32 AddedIndex = ResultArrayHelper.AddValue();
								URigVMMemoryStorage::CopyProperty(
									ResultElementProperty,
									ResultArrayHelper.GetRawPtr(AddedIndex),
									ElementPropertyA,
									ArrayHelperA.GetRawPtr(Pair.Value)
								);
							}
						}
					}

					if(DebugMemoryStorageObject->Num() > 0)
					{
						const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instruction);
						CopyOperandForDebuggingIfNeeded(Op.ArgA, ArrayHandleA);
						CopyOperandForDebuggingIfNeeded(Op.ArgB, ArrayHandleB);
						CopyOperandForDebuggingIfNeeded(Op.ArgC, ResultArrayHandle);
					}
				}
#endif
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::ArrayReverse:
			{
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
				FRigVMMemoryHandle& ArrayHandle = CachedMemoryHandles[FirstHandleForInstruction[Context.InstructionIndex]]; 					
				FScriptArrayHelper ArrayHelper(CastFieldChecked<FArrayProperty>(ArrayHandle.GetProperty()), ArrayHandle.GetData());
				for(int32 A=0, B=ArrayHelper.Num()-1; A<B; A++, B--)
				{
					ArrayHelper.SwapValues(A, B);
				}

				if(DebugMemoryStorageObject->Num() > 0)
				{
					const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instruction);
					CopyOperandForDebuggingIfNeeded(Op.Arg, ArrayHandle);
				}
#endif	
				Context.InstructionIndex++;
				break;
			}
			case ERigVMOpCode::Invalid:
			{
				ensure(false);
				return false;
			}
		}

#if WITH_EDITOR
		if(Context.RuntimeSettings.bEnableProfiling && !InstructionVisitOrder.IsEmpty())
		{
			const uint64 EndCycles = FPlatformTime::Cycles64();
			const uint64 Cycles = EndCycles - StartCycles;
			if(InstructionCyclesDuringLastRun[CurrentInstructionIndex] == UINT64_MAX)
			{
				InstructionCyclesDuringLastRun[CurrentInstructionIndex] = Cycles;
			}
			else
			{
				InstructionCyclesDuringLastRun[CurrentInstructionIndex] += Cycles;
			}

			StartCycles = EndCycles;
			OverallCycles += Cycles;
		}
#endif
	}

#if WITH_EDITOR
	if (HaltedAtBreakpoint != nullptr)
	{
		DebugInfo->SetCurrentActiveBreakpoint(nullptr);
		HaltedAtBreakpoint = nullptr;
		ExecutionHalted().Broadcast(INDEX_NONE, nullptr, InEntryName);
	}
#endif

	return true;
}

bool URigVM::Execute(const FName& InEntryName)
{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	return Execute(FRigVMMemoryContainerPtrArray(), FRigVMFixedArray<void*>(), InEntryName);
#else
	return Execute(TArray<URigVMMemoryStorage*>(), TArrayView<void*>(), InEntryName);
#endif
}

FRigVMExternalVariable URigVM::GetExternalVariableByName(const FName& InExternalVariableName)
{
	for (const FRigVMExternalVariable& ExternalVariable : ExternalVariables)
	{
		if (ExternalVariable.Name == InExternalVariableName)
		{
			return ExternalVariable;
		}
	}
	return FRigVMExternalVariable();
}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

void URigVM::SetRegisterValueFromString(const FRigVMOperand& InOperand, const FString& InCPPType, const UObject* InCPPTypeObject, const TArray<FString>& InDefaultValues)
{
	if (InOperand.GetMemoryType() == ERigVMMemoryType::Literal)
	{
		GetLiteralMemory().SetRegisterValueFromString(InOperand, InCPPType, InCPPTypeObject, InDefaultValues);
	}
	else if (InOperand.GetMemoryType() == ERigVMMemoryType::Work)
	{
		GetWorkMemory().SetRegisterValueFromString(InOperand, InCPPType, InCPPTypeObject, InDefaultValues);
	}
	else if (InOperand.GetMemoryType() == ERigVMMemoryType::Debug)
	{
		GetDebugMemory().SetRegisterValueFromString(InOperand, InCPPType, InCPPTypeObject, InDefaultValues);
	}
}

#else

void URigVM::SetPropertyValueFromString(const FRigVMOperand& InOperand, const FString& InDefaultValue)
{
	URigVMMemoryStorage* Memory = GetMemoryByType(InOperand.GetMemoryType());
	if(Memory == nullptr)
	{
		return;
	}

	Memory->SetDataFromString(InOperand.GetRegisterIndex(), InDefaultValue);
}

#endif

#if WITH_EDITOR

TArray<FString> URigVM::DumpByteCodeAsTextArray(const TArray<int32>& InInstructionOrder, bool bIncludeLineNumbers, TFunction<FString(const FString& RegisterName, const FString& RegisterOffsetName)> OperandFormatFunction)
{
	RefreshInstructionsIfRequired();
	const FRigVMByteCode& ByteCode = GetByteCode();
	const TArray<FName>& FunctionNames = GetFunctionNames();

	TArray<int32> InstructionOrder;
	InstructionOrder.Append(InInstructionOrder);
	if (InstructionOrder.Num() == 0)
	{
		for (int32 InstructionIndex = 0; InstructionIndex < Instructions.Num(); InstructionIndex++)
		{
			InstructionOrder.Add(InstructionIndex);
		}
	}

	TArray<FString> Result;

	for (int32 InstructionIndex : InstructionOrder)
	{
		FString ResultLine;

		switch (Instructions[InstructionIndex].OpCode)
		{
			case ERigVMOpCode::Execute_0_Operands:
			case ERigVMOpCode::Execute_1_Operands:
			case ERigVMOpCode::Execute_2_Operands:
			case ERigVMOpCode::Execute_3_Operands:
			case ERigVMOpCode::Execute_4_Operands:
			case ERigVMOpCode::Execute_5_Operands:
			case ERigVMOpCode::Execute_6_Operands:
			case ERigVMOpCode::Execute_7_Operands:
			case ERigVMOpCode::Execute_8_Operands:
			case ERigVMOpCode::Execute_9_Operands:
			case ERigVMOpCode::Execute_10_Operands:
			case ERigVMOpCode::Execute_11_Operands:
			case ERigVMOpCode::Execute_12_Operands:
			case ERigVMOpCode::Execute_13_Operands:
			case ERigVMOpCode::Execute_14_Operands:
			case ERigVMOpCode::Execute_15_Operands:
			case ERigVMOpCode::Execute_16_Operands:
			case ERigVMOpCode::Execute_17_Operands:
			case ERigVMOpCode::Execute_18_Operands:
			case ERigVMOpCode::Execute_19_Operands:
			case ERigVMOpCode::Execute_20_Operands:
			case ERigVMOpCode::Execute_21_Operands:
			case ERigVMOpCode::Execute_22_Operands:
			case ERigVMOpCode::Execute_23_Operands:
			case ERigVMOpCode::Execute_24_Operands:
			case ERigVMOpCode::Execute_25_Operands:
			case ERigVMOpCode::Execute_26_Operands:
			case ERigVMOpCode::Execute_27_Operands:
			case ERigVMOpCode::Execute_28_Operands:
			case ERigVMOpCode::Execute_29_Operands:
			case ERigVMOpCode::Execute_30_Operands:
			case ERigVMOpCode::Execute_31_Operands:
			case ERigVMOpCode::Execute_32_Operands:
			case ERigVMOpCode::Execute_33_Operands:
			case ERigVMOpCode::Execute_34_Operands:
			case ERigVMOpCode::Execute_35_Operands:
			case ERigVMOpCode::Execute_36_Operands:
			case ERigVMOpCode::Execute_37_Operands:
			case ERigVMOpCode::Execute_38_Operands:
			case ERigVMOpCode::Execute_39_Operands:
			case ERigVMOpCode::Execute_40_Operands:
			case ERigVMOpCode::Execute_41_Operands:
			case ERigVMOpCode::Execute_42_Operands:
			case ERigVMOpCode::Execute_43_Operands:
			case ERigVMOpCode::Execute_44_Operands:
			case ERigVMOpCode::Execute_45_Operands:
			case ERigVMOpCode::Execute_46_Operands:
			case ERigVMOpCode::Execute_47_Operands:
			case ERigVMOpCode::Execute_48_Operands:
			case ERigVMOpCode::Execute_49_Operands:
			case ERigVMOpCode::Execute_50_Operands:
			case ERigVMOpCode::Execute_51_Operands:
			case ERigVMOpCode::Execute_52_Operands:
			case ERigVMOpCode::Execute_53_Operands:
			case ERigVMOpCode::Execute_54_Operands:
			case ERigVMOpCode::Execute_55_Operands:
			case ERigVMOpCode::Execute_56_Operands:
			case ERigVMOpCode::Execute_57_Operands:
			case ERigVMOpCode::Execute_58_Operands:
			case ERigVMOpCode::Execute_59_Operands:
			case ERigVMOpCode::Execute_60_Operands:
			case ERigVMOpCode::Execute_61_Operands:
			case ERigVMOpCode::Execute_62_Operands:
			case ERigVMOpCode::Execute_63_Operands:
			case ERigVMOpCode::Execute_64_Operands:
			{
				const FRigVMExecuteOp& Op = ByteCode.GetOpAt<FRigVMExecuteOp>(Instructions[InstructionIndex]);
				FString FunctionName = FunctionNames[Op.FunctionIndex].ToString();
				FRigVMOperandArray Operands = ByteCode.GetOperandsForExecuteOp(Instructions[InstructionIndex]);

				TArray<FString> Labels;
				for (const FRigVMOperand& Operand : Operands)
				{
					Labels.Add(GetOperandLabel(Operand, OperandFormatFunction));
				}

				ResultLine = FString::Printf(TEXT("%s(%s)"), *FunctionName, *FString::Join(Labels, TEXT(",")));
				break;
			}
			case ERigVMOpCode::Zero:
			{
				const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Set %s to 0"), *GetOperandLabel(Op.Arg, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::BoolFalse:
			{
				const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Set %s to False"), *GetOperandLabel(Op.Arg, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::BoolTrue:
			{
				const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Set %s to True"), *GetOperandLabel(Op.Arg, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::Increment:
			{
				const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Inc %s ++"), *GetOperandLabel(Op.Arg, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::Decrement:
			{
				const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Dec %s --"), *GetOperandLabel(Op.Arg, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::Copy:
			{
				const FRigVMCopyOp& Op = ByteCode.GetOpAt<FRigVMCopyOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Copy %s to %s"), *GetOperandLabel(Op.Source, OperandFormatFunction), *GetOperandLabel(Op.Target, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::Equals:
			{
				const FRigVMComparisonOp& Op = ByteCode.GetOpAt<FRigVMComparisonOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Set %s to %s == %s "), *GetOperandLabel(Op.Result, OperandFormatFunction), *GetOperandLabel(Op.A, OperandFormatFunction), *GetOperandLabel(Op.B, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::NotEquals:
			{
				const FRigVMComparisonOp& Op = ByteCode.GetOpAt<FRigVMComparisonOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Set %s to %s != %s"), *GetOperandLabel(Op.Result, OperandFormatFunction), *GetOperandLabel(Op.A, OperandFormatFunction), *GetOperandLabel(Op.B, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::JumpAbsolute:
			{
				const FRigVMJumpOp& Op = ByteCode.GetOpAt<FRigVMJumpOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Jump to instruction %d"), Op.InstructionIndex);
				break;
			}
			case ERigVMOpCode::JumpForward:
			{
				const FRigVMJumpOp& Op = ByteCode.GetOpAt<FRigVMJumpOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Jump %d instructions forwards"), Op.InstructionIndex);
				break;
			}
			case ERigVMOpCode::JumpBackward:
			{
				const FRigVMJumpOp& Op = ByteCode.GetOpAt<FRigVMJumpOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Jump %d instructions backwards"), Op.InstructionIndex);
				break;
			}
			case ERigVMOpCode::JumpAbsoluteIf:
			{
				const FRigVMJumpIfOp& Op = ByteCode.GetOpAt<FRigVMJumpIfOp>(Instructions[InstructionIndex]);
				if (Op.Condition)
				{
					ResultLine = FString::Printf(TEXT("Jump to instruction %d if %s"), Op.InstructionIndex, *GetOperandLabel(Op.Arg, OperandFormatFunction));
				}
				else
				{
					ResultLine = FString::Printf(TEXT("Jump to instruction %d if !%s"), Op.InstructionIndex, *GetOperandLabel(Op.Arg, OperandFormatFunction));
				}
				break;
			}
			case ERigVMOpCode::JumpForwardIf:
			{
				const FRigVMJumpIfOp& Op = ByteCode.GetOpAt<FRigVMJumpIfOp>(Instructions[InstructionIndex]);
				if (Op.Condition)
				{
					ResultLine = FString::Printf(TEXT("Jump %d instructions forwards if %s"), Op.InstructionIndex, *GetOperandLabel(Op.Arg, OperandFormatFunction));
				}
				else
				{
					ResultLine = FString::Printf(TEXT("Jump %d instructions forwards if !%s"), Op.InstructionIndex, *GetOperandLabel(Op.Arg, OperandFormatFunction));
				}
				break;
			}
			case ERigVMOpCode::JumpBackwardIf:
			{
				const FRigVMJumpIfOp& Op = ByteCode.GetOpAt<FRigVMJumpIfOp>(Instructions[InstructionIndex]);
				if (Op.Condition)
				{
					ResultLine = FString::Printf(TEXT("Jump %d instructions backwards if %s"), Op.InstructionIndex, *GetOperandLabel(Op.Arg, OperandFormatFunction));
				}
				else
				{
					ResultLine = FString::Printf(TEXT("Jump %d instructions backwards if !%s"), Op.InstructionIndex, *GetOperandLabel(Op.Arg, OperandFormatFunction));
				}
				break;
			}
			case ERigVMOpCode::ChangeType:
			{
				const FRigVMChangeTypeOp& Op = ByteCode.GetOpAt<FRigVMChangeTypeOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Change type of %s"), *GetOperandLabel(Op.Arg, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::Exit:
			{
				ResultLine = TEXT("Exit");
				break;
			}
			case ERigVMOpCode::BeginBlock:
			{
				ResultLine = TEXT("Begin Block");
				break;
			}
			case ERigVMOpCode::EndBlock:
			{
				ResultLine = TEXT("End Block");
				break;
			}
			case ERigVMOpCode::ArrayReset:
			{
				const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Reset array %s"), *GetOperandLabel(Op.Arg, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayGetNum:
			{
				const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Get size of array %s and assign to %s"), *GetOperandLabel(Op.ArgA, OperandFormatFunction), *GetOperandLabel(Op.ArgB, OperandFormatFunction));
				break;
			} 
			case ERigVMOpCode::ArraySetNum:
			{
				const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Set size of array %s to %s"), *GetOperandLabel(Op.ArgA, OperandFormatFunction), *GetOperandLabel(Op.ArgB, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayGetAtIndex:
			{
				const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Get item of array %s at index %s and assign to %s"), *GetOperandLabel(Op.ArgA, OperandFormatFunction), *GetOperandLabel(Op.ArgB, OperandFormatFunction), *GetOperandLabel(Op.ArgC, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArraySetAtIndex:
			{
				const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Set item of array %s at index %s to %s"), *GetOperandLabel(Op.ArgA, OperandFormatFunction), *GetOperandLabel(Op.ArgB, OperandFormatFunction), *GetOperandLabel(Op.ArgC, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayAdd:
			{
				const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Add element %s to array %s and return index %s"),
					*GetOperandLabel(Op.ArgB, OperandFormatFunction),
					*GetOperandLabel(Op.ArgA, OperandFormatFunction),
					*GetOperandLabel(Op.ArgC, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayInsert:
			{
				const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Insert element %s to array %s at index %s"), *GetOperandLabel(Op.ArgC, OperandFormatFunction), *GetOperandLabel(Op.ArgA, OperandFormatFunction), *GetOperandLabel(Op.ArgB, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayRemove:
			{
				const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Remove element at index %s from array %s"), *GetOperandLabel(Op.ArgB, OperandFormatFunction), *GetOperandLabel(Op.ArgA, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayFind:
			{
				const FRigVMQuaternaryOp& Op = ByteCode.GetOpAt<FRigVMQuaternaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Find element %s in array %s and returns index %s and if element was found %s"),
					*GetOperandLabel(Op.ArgB, OperandFormatFunction),
					*GetOperandLabel(Op.ArgA, OperandFormatFunction),
					*GetOperandLabel(Op.ArgC, OperandFormatFunction),
					*GetOperandLabel(Op.ArgD, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayAppend:
			{
				const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Append array %s to array %s"), *GetOperandLabel(Op.ArgB, OperandFormatFunction), *GetOperandLabel(Op.ArgA, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayClone:
			{
				const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Clone array %s to array %s"), *GetOperandLabel(Op.ArgA, OperandFormatFunction), *GetOperandLabel(Op.ArgB, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayIterator:
			{
				const FRigVMSenaryOp& Op = ByteCode.GetOpAt<FRigVMSenaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Iterate over array %s, with current element in %s, current index in %s, array count in %s and current ratio in %s"),
					*GetOperandLabel(Op.ArgA, OperandFormatFunction),
					*GetOperandLabel(Op.ArgB, OperandFormatFunction),
					*GetOperandLabel(Op.ArgC, OperandFormatFunction),
					*GetOperandLabel(Op.ArgD, OperandFormatFunction),
					*GetOperandLabel(Op.ArgE, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayUnion:
			{
				const FRigVMBinaryOp& Op = ByteCode.GetOpAt<FRigVMBinaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Merge array %s and array %s"), *GetOperandLabel(Op.ArgA, OperandFormatFunction), *GetOperandLabel(Op.ArgB, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayDifference:
			{
				const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Create array %s from differences of array %s and array %s"),
					*GetOperandLabel(Op.ArgC, OperandFormatFunction),
					*GetOperandLabel(Op.ArgA, OperandFormatFunction),
					*GetOperandLabel(Op.ArgB, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayIntersection:
			{
				const FRigVMTernaryOp& Op = ByteCode.GetOpAt<FRigVMTernaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Create array %s from intersection of array %s and array %s"),
					*GetOperandLabel(Op.ArgC, OperandFormatFunction), 
					*GetOperandLabel(Op.ArgA, OperandFormatFunction),
					*GetOperandLabel(Op.ArgB, OperandFormatFunction));
				break;
			}
			case ERigVMOpCode::ArrayReverse:
			{
				const FRigVMUnaryOp& Op = ByteCode.GetOpAt<FRigVMUnaryOp>(Instructions[InstructionIndex]);
				ResultLine = FString::Printf(TEXT("Reverse array %s"), *GetOperandLabel(Op.Arg, OperandFormatFunction));
				break;
			}
			default:
			{
				ensure(false);
				break;
			}
		}

		if (bIncludeLineNumbers)
		{
			FString ResultIndexStr = FString::FromInt(InstructionIndex);
			while (ResultIndexStr.Len() < 3)
			{
				ResultIndexStr = TEXT("0") + ResultIndexStr;
			}
			Result.Add(FString::Printf(TEXT("%s. %s"), *ResultIndexStr, *ResultLine));
		}
		else
		{
			Result.Add(ResultLine);
		}
	}

	return Result;
}

FString URigVM::DumpByteCodeAsText(const TArray<int32>& InInstructionOrder, bool bIncludeLineNumbers)
{
	return FString::Join(DumpByteCodeAsTextArray(InInstructionOrder, bIncludeLineNumbers), TEXT("\n"));
}

FString URigVM::GetOperandLabel(const FRigVMOperand& InOperand, TFunction<FString(const FString& RegisterName, const FString& RegisterOffsetName)> FormatFunction)
{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	
	const FRigVMMemoryContainer* MemoryPtr = nullptr;

	if (InOperand.GetMemoryType() == ERigVMMemoryType::Literal)
	{
		MemoryPtr = LiteralMemoryPtr;
	}
	else if (InOperand.GetMemoryType() == ERigVMMemoryType::Debug)
	{
		MemoryPtr = DebugMemoryPtr;
	}
	else
	{
		MemoryPtr = WorkMemoryPtr;
	}

	const FRigVMMemoryContainer& Memory = *MemoryPtr;

	FString RegisterName;
	if (InOperand.GetMemoryType() == ERigVMMemoryType::External)
	{
		const FRigVMExternalVariable& ExternalVariable = ExternalVariables[InOperand.GetRegisterIndex()];
		RegisterName = FString::Printf(TEXT("Variable::%s"), *ExternalVariable.Name.ToString());
	}
	else
	{
		FRigVMRegister Register = Memory[InOperand];
		RegisterName = Register.Name.ToString();
	}

	FString OperandLabel;
	OperandLabel = RegisterName;
	
	// append an offset name if it exists
	FString RegisterOffsetName;
	if (InOperand.GetRegisterOffset() != INDEX_NONE)
	{
		RegisterOffsetName = Memory.RegisterOffsets[InOperand.GetRegisterOffset()].CachedSegmentPath;
		OperandLabel = FString::Printf(TEXT("%s.%s"), *OperandLabel, *RegisterOffsetName);
	}

#else

	FString RegisterName;
	FString RegisterOffsetName;
	if (InOperand.GetMemoryType() == ERigVMMemoryType::External)
	{
		const FRigVMExternalVariable& ExternalVariable = ExternalVariables[InOperand.GetRegisterIndex()];
		RegisterName = FString::Printf(TEXT("Variable::%s"), *ExternalVariable.Name.ToString());
		if (InOperand.GetRegisterOffset() != INDEX_NONE)
		{
			if(ensure(ExternalPropertyPaths.IsValidIndex(InOperand.GetRegisterOffset())))
			{
				RegisterOffsetName = ExternalPropertyPaths[InOperand.GetRegisterOffset()].ToString();
			}
		}
	}
	else
	{
		URigVMMemoryStorage* Memory = GetMemoryByType(InOperand.GetMemoryType());
		if(Memory == nullptr)
		{
			return FString();
		}

		check(Memory->IsValidIndex(InOperand.GetRegisterIndex()));
		
		RegisterName = Memory->GetProperties()[InOperand.GetRegisterIndex()]->GetName();
		RegisterOffsetName =
			InOperand.GetRegisterOffset() != INDEX_NONE ?
			Memory->GetPropertyPaths()[InOperand.GetRegisterOffset()].ToString() :
			FString();
	}
	
	FString OperandLabel = RegisterName;
	
#endif
	
	// caller can provide an alternative format to override the default format(optional)
	if (FormatFunction)
	{
		OperandLabel = FormatFunction(RegisterName, RegisterOffsetName);
	}

	return OperandLabel;
}

#endif

void URigVM::ClearDebugMemory()
{
#if WITH_EDITOR
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	FRigVMMemoryContainer* DebugMemory = CachedMemory[GetContainerIndex(ERigVMMemoryType::Debug)];
	if (DebugMemory)
	{ 
		for (int32 RegisterIndex = 0; RegisterIndex < DebugMemory->Num(); RegisterIndex++)
		{
			ensure(DebugMemory->GetRegister(RegisterIndex).IsDynamic());
			DebugMemory->Destroy(RegisterIndex);
		}	
	}
#else

	for(int32 PropertyIndex = 0; PropertyIndex < GetDebugMemory()->Num(); PropertyIndex++)
	{
		if(const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(GetDebugMemory()->GetProperties()[PropertyIndex]))
		{
			FScriptArrayHelper ArrayHelper(ArrayProperty, GetDebugMemory()->GetData<uint8>(PropertyIndex));
			ArrayHelper.EmptyValues();
		}
	}
	
#endif
#endif
}

void URigVM::CacheSingleMemoryHandle(const FRigVMOperand& InArg, bool bForExecute)
{
#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	
	if (InArg.GetMemoryType() == ERigVMMemoryType::External)
	{
		ensure(ExternalVariables.IsValidIndex(InArg.GetRegisterIndex()));

		FRigVMExternalVariable& ExternalVariable = ExternalVariables[InArg.GetRegisterIndex()];
		const FRigVMRegisterOffset& RegisterOffset = GetWorkMemory().GetRegisterOffsetForOperand(InArg);
		check(ExternalVariable.Memory);

		FRigVMMemoryHandle Handle = ExternalVariable.GetHandle();
		if (RegisterOffset.IsValid())
		{
			Handle.RegisterOffset = &RegisterOffset;
		}
		CachedMemoryHandles.Add(Handle);
		return;
	}

	const FRigVMRegister& Register = CachedMemory[InArg.GetContainerIndex()]->GetRegister(InArg);
	
	CachedMemoryHandles.Add(CachedMemory[InArg.GetContainerIndex()]->GetHandle(Register, InArg.GetRegisterOffset()));

	if (bForExecute)
	{
		if (Register.IsArray() && !Register.IsDynamic())
	{
			void* ElementsForArray = reinterpret_cast<void*>(Register.ElementCount);
			CachedMemoryHandles.Add(FRigVMMemoryHandle((uint8*)ElementsForArray, sizeof(uint16), FRigVMMemoryHandle::FType::ArraySize));
		}
	}
	
#else

	URigVMMemoryStorage* Memory = GetMemoryByType(InArg.GetMemoryType());

	if (InArg.GetMemoryType() == ERigVMMemoryType::External)
	{
		const FRigVMPropertyPath* PropertyPath = nullptr;
		if(InArg.GetRegisterOffset() != INDEX_NONE)
		{
			check(ExternalPropertyPaths.IsValidIndex(InArg.GetRegisterOffset()));
			PropertyPath = &ExternalPropertyPaths[InArg.GetRegisterOffset()];
		}

		check(ExternalVariables.IsValidIndex(InArg.GetRegisterIndex()));

		FRigVMExternalVariable& ExternalVariable = ExternalVariables[InArg.GetRegisterIndex()];
		check(ExternalVariable.IsValid(false));

		const FRigVMMemoryHandle Handle(ExternalVariable.Memory, ExternalVariable.Property, PropertyPath);
		CachedMemoryHandles.Add(Handle);
		return;
	}

	const FRigVMPropertyPath* PropertyPath = nullptr;
	if(InArg.GetRegisterOffset() != INDEX_NONE)
	{
		check(Memory->GetPropertyPaths().IsValidIndex(InArg.GetRegisterOffset()));
		PropertyPath = &Memory->GetPropertyPaths()[InArg.GetRegisterOffset()];
	}

	// if you are hitting this it's likely that the VM was created outside of a valid
	// package. the compiler bases the memory class construction on the package the VM
	// is in - so a VM under GetTransientPackage() can be created - but not run.
	uint8* Data = Memory->GetData<uint8>(InArg.GetRegisterIndex());
	const FProperty* Property = Memory->GetProperties()[InArg.GetRegisterIndex()];
	const FRigVMMemoryHandle Handle(Data, Property, PropertyPath);
	CachedMemoryHandles.Add(Handle);

#endif
}

void URigVM::CopyOperandForDebuggingImpl(const FRigVMOperand& InArg, const FRigVMMemoryHandle& InHandle, const FRigVMOperand& InDebugOperand)
{
#if WITH_EDITOR

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

	check(InArg.IsValid());
	check(InArg.GetRegisterOffset() == INDEX_NONE);
	check(InDebugOperand.IsValid());
	check(InDebugOperand.GetRegisterOffset() == INDEX_NONE);

	const FRigVMRegister& DebugRegister = DebugMemoryPtr->GetRegister(InDebugOperand.GetRegisterIndex());
	check(DebugRegister.IsDynamic());

	if (Context.GetSlice().GetIndex() == 0)
	{
		DebugMemoryPtr->Destroy(InDebugOperand.GetRegisterIndex());
	}

	// the source pointer is not going to be sliced since we only allow
	// watches on things exposed from a node (so no hidden pins)
	const uint8* SourcePtr = InHandle.GetData(0, true);
	uint8* TargetPtr = nullptr;

	int32 NumBytes = (int32)DebugRegister.ElementSize;
	if(InHandle.GetType() == FRigVMMemoryHandle::FType::Dynamic)
	{
		FRigVMByteArray* Storage = (FRigVMByteArray*)InHandle.Ptr;
		NumBytes = Storage->Num();
		TargetPtr = Storage->GetData();
	}
	else if(InHandle.GetType() == FRigVMMemoryHandle::FType::NestedDynamic)
	{
		FRigVMNestedByteArray* Storage = (FRigVMNestedByteArray*)InHandle.Ptr;
		NumBytes = (*Storage)[Context.GetSlice().GetIndex()].Num(); 
		TargetPtr = (*Storage)[Context.GetSlice().GetIndex()].GetData();
	}

	const FRigVMMemoryHandle DebugHandle = DebugMemoryPtr->GetHandle(InDebugOperand.GetRegisterIndex());
	if (DebugRegister.IsNestedDynamic())
	{
		FRigVMNestedByteArray* Storage = (FRigVMNestedByteArray*)DebugHandle.Ptr;
		while(Storage->Num() < Context.GetSlice().TotalNum())
		{
			Storage->Add(FRigVMByteArray());
		}
		(*Storage)[Context.GetSlice().GetIndex()].AddZeroed(NumBytes);
		TargetPtr = (*Storage)[Context.GetSlice().GetIndex()].GetData();
	}
	else
	{
		const int32 TotalBytes = Context.GetSlice().TotalNum() * NumBytes;
		FRigVMByteArray* Storage = (FRigVMByteArray*)DebugHandle.Ptr;
		while(Storage->Num() < TotalBytes)
		{
			Storage->AddZeroed(NumBytes);
		}
		TargetPtr = &(*Storage)[Context.GetSlice().GetIndex() * NumBytes];
	}

	if((SourcePtr == nullptr) || (TargetPtr == nullptr))
	{
		return;
	}

	switch (DebugRegister.Type)
	{
		case ERigVMRegisterType::Plain:
		{
			FMemory::Memcpy(TargetPtr, SourcePtr, NumBytes);
			break;
		}
		case ERigVMRegisterType::Name:
		{
			const int32 NumNames = NumBytes / sizeof(FName);
			FRigVMFixedArray<FName> TargetNames((FName*)TargetPtr, NumNames);
			FRigVMFixedArray<FName> SourceNames((FName*)SourcePtr, NumNames);
			for (int32 Index = 0; Index < NumNames; Index++)
			{
				TargetNames[Index] = SourceNames[Index];
			}
			break;
		}
		case ERigVMRegisterType::String:
		{
			const int32 NumStrings = NumBytes / sizeof(FString);
			FRigVMFixedArray<FString> TargetStrings((FString*)TargetPtr, NumStrings);
			FRigVMFixedArray<FString> SourceStrings((FString*)SourcePtr, NumStrings);
			for (int32 Index = 0; Index < NumStrings; Index++)
			{
				TargetStrings[Index] = SourceStrings[Index];
			}
			break;
		}
		case ERigVMRegisterType::Struct:
		{
			UScriptStruct* ScriptStruct = DebugMemoryPtr->GetScriptStruct(DebugRegister);
			const int32 NumStructs = NumBytes / ScriptStruct->GetStructureSize();
			if (NumStructs > 0 && TargetPtr)
			{
				ScriptStruct->CopyScriptStruct(TargetPtr, SourcePtr, NumStructs);
			}
			break;
		}
		default:
		{
			// the default pass for any complex memory
			// changes to the layout of memory array should be reflected in GetContainerIndex()
			FRigVMMemoryContainer* LocalMemory[] = { WorkMemoryPtr, LiteralMemoryPtr, DebugMemoryPtr };
			DebugMemoryPtr->Copy(InArg, InDebugOperand, LocalMemory[InArg.GetContainerIndex()]);
			break;
		}
	}

#else

	URigVMMemoryStorage* TargetMemory = GetDebugMemory();
	if(TargetMemory == nullptr)
	{
		return;
	}
	const FProperty* TargetProperty = TargetMemory->GetProperties()[InDebugOperand.GetRegisterIndex()];
	uint8* TargetPtr = TargetMemory->GetData<uint8>(InDebugOperand.GetRegisterIndex());

	// since debug properties are always arrays, we need to divert to the last array element's memory
	const FArrayProperty* TargetArrayProperty = CastField<FArrayProperty>(TargetProperty);
	if(TargetArrayProperty == nullptr)
	{
		return;
	}

	// add an element to the end for debug watching
	FScriptArrayHelper ArrayHelper(TargetArrayProperty, TargetPtr);

	if (Context.GetSlice().GetIndex() == 0)
	{
		ArrayHelper.Resize(0);
	}
	else if(Context.GetSlice().GetIndex() == ArrayHelper.Num() - 1)
	{
		return;
	}

	const int32 AddedIndex = ArrayHelper.AddValue();
	TargetPtr = ArrayHelper.GetRawPtr(AddedIndex);
	TargetProperty = TargetArrayProperty->Inner;

	if(InArg.GetMemoryType() == ERigVMMemoryType::External)
	{
		if(ExternalVariables.IsValidIndex(InArg.GetRegisterIndex()))
		{
			FRigVMExternalVariable& ExternalVariable = ExternalVariables[InArg.GetRegisterIndex()];
			const FProperty* SourceProperty = ExternalVariable.Property;
			const uint8* SourcePtr = ExternalVariable.Memory;
			URigVMMemoryStorage::CopyProperty(TargetProperty, TargetPtr, SourceProperty, SourcePtr);
		}
		return;
	}

	URigVMMemoryStorage* SourceMemory = GetMemoryByType(InArg.GetMemoryType());
	if(SourceMemory == nullptr)
	{
		return;
	}
	const FProperty* SourceProperty = SourceMemory->GetProperties()[InArg.GetRegisterIndex()];
	const uint8* SourcePtr = SourceMemory->GetData<uint8>(InArg.GetRegisterIndex());

	URigVMMemoryStorage::CopyProperty(TargetProperty, TargetPtr, SourceProperty, SourcePtr);
	
#endif
	
#endif
}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

FRigVMCopyOp URigVM::GetCopyOpForOperands(const FRigVMOperand& InSource, const FRigVMOperand& InTarget)
{
	FCopyInfoForOperand SourceCopyInfo = GetCopyInfoForOperand(InSource);
	FCopyInfoForOperand TargetCopyInfo = GetCopyInfoForOperand(InTarget);

#if !WITH_EDITOR
	check(SourceCopyInfo.RegisterType != ERigVMRegisterType::Invalid);
	check(SourceCopyInfo.NumBytesToCopy > 0);
	check(TargetCopyInfo.RegisterType != ERigVMRegisterType::Invalid);
	check(TargetCopyInfo.NumBytesToCopy > 0);
#endif

	ERigVMCopyType CopyType = ERigVMCopyType::Default;

	if(SourceCopyInfo.RegisterType == ERigVMRegisterType::Plain && TargetCopyInfo.RegisterType == ERigVMRegisterType::Plain)
	{
		if((SourceCopyInfo.ElementSize == sizeof(float)) && (TargetCopyInfo.ElementSize == sizeof(double)))
		{
			CopyType = ERigVMCopyType::FloatToDouble;
		}
		else if((SourceCopyInfo.ElementSize == sizeof(double)) && (TargetCopyInfo.ElementSize == sizeof(float)))
		{
			CopyType = ERigVMCopyType::DoubleToFloat;
		}
	}

	return FRigVMCopyOp(InSource, InTarget, TargetCopyInfo.NumBytesToCopy, TargetCopyInfo.RegisterType, CopyType);
}

#else

FRigVMCopyOp URigVM::GetCopyOpForOperands(const FRigVMOperand& InSource, const FRigVMOperand& InTarget)
{
	return FRigVMCopyOp(InSource, InTarget);
}

void URigVM::RefreshExternalPropertyPaths()
{
	ExternalPropertyPaths.Reset();

	ExternalPropertyPaths.SetNumZeroed(ExternalPropertyPathDescriptions.Num());
	for(int32 PropertyPathIndex = 0; PropertyPathIndex < ExternalPropertyPaths.Num(); PropertyPathIndex++)
	{
		ExternalPropertyPaths[PropertyPathIndex] = FRigVMPropertyPath();

		const int32 PropertyIndex = ExternalPropertyPathDescriptions[PropertyPathIndex].PropertyIndex;
		if(ExternalVariables.IsValidIndex(PropertyIndex))
		{
			check(ExternalVariables[PropertyIndex].Property);
			
			ExternalPropertyPaths[PropertyPathIndex] = FRigVMPropertyPath(
				ExternalVariables[PropertyIndex].Property,
				ExternalPropertyPathDescriptions[PropertyPathIndex].SegmentPath);
		}
	}
}

void URigVM::CopyArray(FScriptArrayHelper& TargetHelper, FRigVMMemoryHandle& TargetHandle,
	FScriptArrayHelper& SourceHelper, FRigVMMemoryHandle& SourceHandle)
{
	const FArrayProperty* TargetArrayProperty = CastFieldChecked<FArrayProperty>(TargetHandle.GetProperty());
	const FArrayProperty* SourceArrayProperty = CastFieldChecked<FArrayProperty>(SourceHandle.GetProperty());

	TargetHelper.Resize(SourceHelper.Num());
	if(SourceHelper.Num() > 0)
	{
		const FProperty* TargetProperty = TargetArrayProperty->Inner;
		const FProperty* SourceProperty = SourceArrayProperty->Inner;
		for(int32 ElementIndex = 0; ElementIndex < SourceHelper.Num(); ElementIndex++)
		{
			uint8* TargetMemory = TargetHelper.GetRawPtr(ElementIndex);
			const uint8* SourceMemory = SourceHelper.GetRawPtr(ElementIndex);
			URigVMMemoryStorage::CopyProperty(TargetProperty, TargetMemory, SourceProperty, SourceMemory);
		}
	}
}

#endif

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

URigVM::FCopyInfoForOperand URigVM::GetCopyInfoForOperand(const FRigVMOperand& InOperand)
{
	if(CachedMemory.IsEmpty())
	{
		// changes to the layout of memory array should be reflected in GetContainerIndex()
		CachedMemory.Add(WorkMemoryPtr);
		CachedMemory.Add(LiteralMemoryPtr);
		CachedMemory.Add(DebugMemoryPtr);
	}
	
	ERigVMRegisterType RegisterType = ERigVMRegisterType::Invalid;
	uint16 NumBytesToCopy = 0;
	uint16 ElementSize = 0;

	if (InOperand.GetRegisterOffset() != INDEX_NONE)
	{
		const FRigVMRegisterOffset& RegisterOffset = CachedMemory[InOperand.GetContainerIndex()]->RegisterOffsets[InOperand.GetRegisterOffset()];
		RegisterType = RegisterOffset.GetType();
		NumBytesToCopy = RegisterOffset.GetElementSize();
		ElementSize = RegisterOffset.GetElementSize();
	}
	else if (InOperand.GetMemoryType() == ERigVMMemoryType::External)
	{
		if(ExternalVariables.IsValidIndex(InOperand.GetRegisterIndex()))
		{
			const FRigVMExternalVariable& ExternalVariable = ExternalVariables[InOperand.GetRegisterIndex()];

			NumBytesToCopy = ExternalVariable.Size;
			ElementSize = ExternalVariable.Size;
			RegisterType = ERigVMRegisterType::Plain;
			
			if (UScriptStruct* ExternalScriptStruct = Cast<UScriptStruct>(ExternalVariable.TypeObject))	
			{
				RegisterType = ERigVMRegisterType::Struct;
			}
			else if (ExternalVariable.TypeName == TEXT("FString"))
			{
				RegisterType = ERigVMRegisterType::String;
			}
			else if (ExternalVariable.TypeName == TEXT("FName"))
			{
				RegisterType = ERigVMRegisterType::Name;
			}
		}
	}
	else
	{
		const FRigVMRegister& Register = CachedMemory[InOperand.GetContainerIndex()]->Registers[InOperand.GetRegisterIndex()];

		RegisterType = Register.Type;
		NumBytesToCopy = Register.GetNumBytesPerSlice();
		ElementSize = Register.ElementSize;
	}

	return FCopyInfoForOperand(RegisterType, NumBytesToCopy, ElementSize); 
}

UScriptStruct* URigVM::GetScriptStructForCopyOp(const FRigVMCopyOp& InCopyOp) const
{
	UScriptStruct* SourceScriptStruct = GetScripStructForOperand(InCopyOp.Source);
#if WITH_EDITOR
	UScriptStruct* TargetScriptStruct = GetScripStructForOperand(InCopyOp.Target);
	check(SourceScriptStruct == TargetScriptStruct);
#endif
	return SourceScriptStruct;
}

UScriptStruct* URigVM::GetScripStructForOperand(const FRigVMOperand& InOperand) const
{
	if(InOperand.GetRegisterOffset() != INDEX_NONE)
	{
		const FRigVMRegisterOffset& RegisterOffset = CachedMemory[InOperand.GetContainerIndex()]->RegisterOffsets[InOperand.GetRegisterOffset()];
		return RegisterOffset.GetScriptStruct();
	}

	if(InOperand.GetMemoryType() == ERigVMMemoryType::External)
	{
		const FRigVMExternalVariable& ExternalVariable = ExternalVariables[InOperand.GetRegisterIndex()];
		return Cast<UScriptStruct>(ExternalVariable.TypeObject);
	}

	return CachedMemory[InOperand.GetContainerIndex()]->GetScriptStruct(InOperand.GetRegisterIndex());
}

#endif
