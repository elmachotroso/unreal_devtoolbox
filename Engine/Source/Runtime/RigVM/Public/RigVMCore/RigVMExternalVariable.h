// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "RigVMTraits.h"
#include "RigVMMemory.h"

/**
 * The external variable can be used to map external / unowned
 * memory into the VM and back out
 */
struct RIGVM_API FRigVMExternalVariable
{
	FORCEINLINE FRigVMExternalVariable()
		: Name(NAME_None)
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
		, Property(nullptr)
#endif
		, TypeName(NAME_None)
		, TypeObject(nullptr)
		, bIsArray(false)
		, bIsPublic(false)
		, bIsReadOnly(false)
		, Size(0)
		, Memory(nullptr)
	{
	}

	FORCEINLINE static void GetTypeFromProperty(const FProperty* InProperty, FName& OutTypeName, UObject*& OutTypeObject)
	{
		if (CastField<FBoolProperty>(InProperty))
		{
			OutTypeName = TEXT("bool");
			OutTypeObject = nullptr;
		}
		else if (CastField<FIntProperty>(InProperty))
		{
			OutTypeName = TEXT("int32");
			OutTypeObject = nullptr;
		}
		else if (CastField<FFloatProperty>(InProperty))
		{
			OutTypeName = TEXT("float");
			OutTypeObject = nullptr;
		}
		else if (CastField<FDoubleProperty>(InProperty))
		{
			OutTypeName = TEXT("double");
			OutTypeObject = nullptr;
		}
		else if (CastField<FStrProperty>(InProperty))
		{
			OutTypeName = TEXT("FString");
			OutTypeObject = nullptr;
		}
		else if (CastField<FNameProperty>(InProperty))
		{
			OutTypeName = TEXT("FName");
			OutTypeObject = nullptr;
		}
		else if (const FEnumProperty* EnumProperty = CastField<FEnumProperty>(InProperty))
		{
			OutTypeName = EnumProperty->GetEnum()->GetFName();
			OutTypeObject = EnumProperty->GetEnum();
		}
		else if (const FByteProperty* ByteProperty = CastField<FByteProperty>(InProperty))
		{
			if (UEnum* BytePropertyEnum = ByteProperty->Enum)
			{
				OutTypeName = BytePropertyEnum->GetFName();
				OutTypeObject = BytePropertyEnum;
			}
		}
		else if (const FStructProperty* StructProperty = CastField<FStructProperty>(InProperty))
		{
			OutTypeName = *StructProperty->Struct->GetStructCPPName();
			OutTypeObject = StructProperty->Struct;
		}
		else if (const FObjectProperty* ObjectProperty = CastField<FObjectProperty>(InProperty))
		{
			OutTypeName = *FString::Printf(TEXT("TObjectPtr<%s%s>"),
				ObjectProperty->PropertyClass->GetPrefixCPP(),
				*ObjectProperty->PropertyClass->GetName());
			OutTypeObject = ObjectProperty->PropertyClass;
		}
		else
		{
			checkNoEntry();
		}
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FProperty* InProperty, void* InContainer, const FName& InOptionalName = NAME_None)
	{
		check(InProperty);

		const FProperty* Property = InProperty;

		FRigVMExternalVariable ExternalVariable;
		ExternalVariable.Name = InOptionalName.IsNone() ? InProperty->GetFName() : InOptionalName;
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
		ExternalVariable.Property = Property;
#endif
		ExternalVariable.bIsPublic = !InProperty->HasAllPropertyFlags(CPF_DisableEditOnInstance);
		ExternalVariable.bIsReadOnly = InProperty->HasAllPropertyFlags(CPF_BlueprintReadOnly);

		if (InContainer)
		{
			ExternalVariable.Memory = (uint8*)Property->ContainerPtrToValuePtr<uint8>(InContainer);
		}

		FString TypePrefix, TypeSuffix;
		if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(Property))
		{
			ExternalVariable.bIsArray = true;
			TypePrefix = TEXT("TArray<");
			TypeSuffix = TEXT(">");
			Property = ArrayProperty->Inner;
		}

		ExternalVariable.Size = Property->GetSize();
		GetTypeFromProperty(Property, ExternalVariable.TypeName, ExternalVariable.TypeObject);

		return ExternalVariable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, bool& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("bool");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = false;
		Variable.Size = sizeof(bool);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<bool>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("bool");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = true;
		Variable.Size = sizeof(bool);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, int32& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("int32");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = false;
		Variable.Size = sizeof(int32);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<int32>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("int32");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = true;
		Variable.Size = sizeof(int32);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, uint8& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("uint8");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = false;
		Variable.Size = sizeof(uint8);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<uint8>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("uint8");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = true;
		Variable.Size = sizeof(uint8);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, float& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("float");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = false;
		Variable.Size = sizeof(float);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<float>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("float");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = true;
		Variable.Size = sizeof(float);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, double& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("double");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = false;
		Variable.Size = sizeof(double);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<double>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("double");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = true;
		Variable.Size = sizeof(double);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, FString& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("FString");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = false;
		Variable.Size = sizeof(FString);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<FString>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("FString");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = true;
		Variable.Size = sizeof(FString);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, FName& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("FName");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = false;
		Variable.Size = sizeof(FName);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<FName>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = TEXT("FName");
		Variable.TypeObject = nullptr;
		Variable.bIsArray = true;
		Variable.Size = sizeof(FName);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	template <
		typename T,
		typename TEnableIf<TIsEnum<T>::Value>::Type* = nullptr
	>
	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, T& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = StaticEnum<T>()->GetFName();
		Variable.TypeObject = StaticEnum<T>();
		Variable.bIsArray = false;
		Variable.Size = sizeof(T);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	template <
		typename T,
		typename TEnableIf<TIsEnum<T>::Value>::Type* = nullptr
	>
	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<T>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = StaticEnum<T>()->GetFName();
		Variable.TypeObject = StaticEnum<T>();
		Variable.bIsArray = true;
		Variable.Size = sizeof(T);
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	template <
		typename T,
		typename TEnableIf<TRigVMIsBaseStructure<T>::Value, T>::Type* = nullptr
	>
	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, T& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = *TBaseStructure<T>::Get()->GetStructCPPName();
		Variable.TypeObject = TBaseStructure<T>::Get();
		Variable.bIsArray = false;
		Variable.Size = TBaseStructure<T>::Get()->GetStructureSize();
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	template <
		typename T,
		typename TEnableIf<TRigVMIsBaseStructure<T>::Value, T>::Type* = nullptr
	>
	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<T>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = *TBaseStructure<T>::Get()->GetStructCPPName();
		Variable.TypeObject = TBaseStructure<T>::Get();
		Variable.bIsArray = true;
		Variable.Size = TBaseStructure<T>::Get()->GetStructureSize();
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	template <
		typename T,
		typename TEnableIf<TModels<CRigVMUStruct, T>::Value>::Type * = nullptr
	>
	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, T& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = *T::StaticStruct()->GetStructCPPName();
		Variable.TypeObject = T::StaticStruct();
		Variable.bIsArray = false;
		Variable.Size = T::StaticStruct()->GetStructureSize();
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	template <
		typename T,
		typename TEnableIf<TModels<CRigVMUStruct, T>::Value>::Type * = nullptr
	>
	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<T>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = *T::StaticStruct()->GetStructCPPName();
		Variable.TypeObject = T::StaticStruct();
		Variable.bIsArray = true;
		Variable.Size = T::StaticStruct()->GetStructureSize();
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	template <
		typename T,
		typename TEnableIf<TModels<CRigVMUClass, T>::Value>::Type * = nullptr
	>
	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, T& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = *T::StaticClass()->GetStructCPPName();
		Variable.TypeObject = T::StaticClass();
		Variable.bIsArray = false;
		Variable.Size = T::StaticClass()->GetStructureSize();
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	template <
		typename T,
		typename TEnableIf<TModels<CRigVMUClass, T>::Value>::Type * = nullptr
	>
	FORCEINLINE static FRigVMExternalVariable Make(const FName& InName, TArray<T>& InValue)
	{
		FRigVMExternalVariable Variable;
		Variable.Name = InName;
		Variable.TypeName = *T::StaticClass()->GetStructCPPName();
		Variable.TypeObject = T::StaticClass();
		Variable.bIsArray = true;
		Variable.Size = T::StaticClass()->GetStructureSize();
		Variable.Memory = (uint8*)&InValue;
		return Variable;
	}

	template<typename T>
	FORCEINLINE T GetValue() const
	{
		ensure(IsValid() && !bIsArray);
		return *(T*)Memory;
	}

	template<typename T>
	FORCEINLINE T& GetRef()
	{
		ensure(IsValid() && !bIsArray);
		return *(T*)Memory;
	}

	template<typename T>
	FORCEINLINE const T& GetRef() const
	{
		ensure(IsValid() && !bIsArray);
		return *(T*)Memory;
	}

	template<typename T>
	FORCEINLINE void SetValue(const T& InValue)
	{
		ensure(IsValid() && !bIsArray);
		(*(T*)Memory) = InValue;
	}

	template<typename T>
	FORCEINLINE TArray<T> GetArray()
	{
		ensure(IsValid() && bIsArray);
		return *(TArray<T>*)Memory;
	}

	template<typename T>
	FORCEINLINE void SetArray(const TArray<T>& InValue)
	{
		ensure(IsValid() && bIsArray);
		(*(TArray<T>*)Memory) = InValue;
	}

	FORCEINLINE bool IsValid(bool bAllowNullPtr = false) const
	{
		return Name.IsValid() && 
			!Name.IsNone() &&
			TypeName.IsValid() &&
			!TypeName.IsNone() &&
			(bAllowNullPtr || Memory != nullptr);
	}

#if UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED

	FORCEINLINE FRigVMMemoryHandle GetHandle() const
	{
		return FRigVMMemoryHandle(
			Memory,
			Size,
			bIsArray ? FRigVMMemoryHandle::FType::Dynamic : FRigVMMemoryHandle::FType::Plain
		);
	}

#endif

	FORCEINLINE static void MergeExternalVariable(TArray<FRigVMExternalVariable>& OutVariables, const FRigVMExternalVariable& InVariable)
	{
		if(!InVariable.IsValid(true))
		{
			return;
		}

		for(const FRigVMExternalVariable& ExistingVariable : OutVariables)
		{
			if(ExistingVariable.Name == InVariable.Name)
			{
				ensure(ExistingVariable.TypeName == InVariable.TypeName);
				ensure(ExistingVariable.TypeObject == InVariable.TypeObject);
				return;
			}
		}

		OutVariables.Add(InVariable);
	}

	FName Name;
#if !UE_RIGVM_UCLASS_BASED_STORAGE_DISABLED
	const FProperty* Property;
#endif
	FName TypeName;
	UObject* TypeObject;
	bool bIsArray;
	bool bIsPublic;
	bool bIsReadOnly;
	int32 Size;
	uint8* Memory;
};
