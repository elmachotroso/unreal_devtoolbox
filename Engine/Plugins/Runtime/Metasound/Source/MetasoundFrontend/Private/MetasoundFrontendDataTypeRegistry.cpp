// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetasoundFrontendDataTypeRegistry.h"


namespace Metasound
{
	namespace Frontend
	{
		namespace MetasoundFrontendDataTypeRegistryPrivate
		{
			// Return the compatible literal with the most descriptive type.
			// TODO: Currently TIsParsable<> allows for implicit conversion of
			// constructor arguments of integral types which can cause some confusion
			// here when trying to match a literal type to a constructor. For example:
			//
			// struct FBoolConstructibleType
			// {
			// 	FBoolConstructibleType(bool InValue);
			// };
			//
			// static_assert(TIsParsable<FBoolConstructible, double>::Value); 
			//
			// Implicit conversions are currently allowed in TIsParsable because this
			// is perfectly legal syntax.
			//
			// double Value = 10.0;
			// FBoolConstructibleType BoolConstructible = Value;
			//
			// There are some tricks to possibly disable implicit conversions when
			// checking for specific constructors, but they are yet to be implemented 
			// and are untested. Here's the basic idea.
			//
			// template<DataType, DesiredIntegralArgType>
			// struct TOnlyConvertIfIsSame
			// {
			// 		// Implicit conversion only defined if types match.
			// 		template<typename SuppliedIntegralArgType, std::enable_if<std::is_same<std::decay<SuppliedIntegralArgType>::type, DesiredIntegralArgType>::value, int> = 0>
			// 		operator DesiredIntegralArgType()
			// 		{
			// 			return DesiredIntegralArgType{};
			// 		}
			// };
			//
			// static_assert(false == std::is_constructible<FBoolConstructibleType, TOnlyConvertIfSame<double>>::value);
			// static_assert(true == std::is_constructible<FBoolConstructibleType, TOnlyConvertIfSame<bool>>::value);
			ELiteralType GetMostDescriptiveLiteralForDataType(const FDataTypeRegistryInfo& InDataTypeInfo)
			{
				if (InDataTypeInfo.bIsProxyArrayParsable)
				{
					return ELiteralType::UObjectProxyArray;
				}
				else if (InDataTypeInfo.bIsProxyParsable)
				{
					return ELiteralType::UObjectProxy;
				}
				else if (InDataTypeInfo.bIsEnum && InDataTypeInfo.bIsIntParsable)
				{
					return ELiteralType::Integer;
				}
				else if (InDataTypeInfo.bIsStringArrayParsable)
				{
					return ELiteralType::StringArray;
				}
				else if (InDataTypeInfo.bIsFloatArrayParsable)
				{
					return ELiteralType::FloatArray;
				}
				else if (InDataTypeInfo.bIsIntArrayParsable)
				{
					return ELiteralType::IntegerArray;
				}
				else if (InDataTypeInfo.bIsBoolArrayParsable)
				{
					return ELiteralType::BooleanArray;
				}
				else if (InDataTypeInfo.bIsStringParsable)
				{
					return ELiteralType::String;
				}
				else if (InDataTypeInfo.bIsFloatParsable)
				{
					return ELiteralType::Float;
				}
				else if (InDataTypeInfo.bIsIntParsable)
				{
					return ELiteralType::Integer;
				}
				else if (InDataTypeInfo.bIsBoolParsable)
				{
					return ELiteralType::Boolean;
				}
				else if (InDataTypeInfo.bIsDefaultArrayParsable)
				{
					return ELiteralType::NoneArray; 
				}
				else if (InDataTypeInfo.bIsDefaultParsable)
				{
					return ELiteralType::None;
				}
				else
				{
					// if we ever hit this, something has gone wrong with the REGISTER_METASOUND_DATATYPE macro.
					// we should have failed to compile if any of these are false.
					checkNoEntry();
					return ELiteralType::Invalid;
				}
			}

			// Base class for INodeRegistryEntrys that come from an IDataTypeRegistryEntry
			class FDataTypeNodeRegistryEntry : public INodeRegistryEntry
			{
			public:
				FDataTypeNodeRegistryEntry() = default;

				virtual ~FDataTypeNodeRegistryEntry() = default;

				virtual const FNodeClassInfo& GetClassInfo() const override
				{
					return ClassInfo;
				}

				virtual TUniquePtr<INode> CreateNode(const FNodeInitData&) const override
				{
					return nullptr;
				}

				virtual TUniquePtr<INode> CreateNode(FDefaultLiteralNodeConstructorParams&& InParams) const override
				{
					return nullptr;
				}

				virtual TUniquePtr<INode> CreateNode(FDefaultNamedVertexNodeConstructorParams&&) const override
				{
					return nullptr;
				}

				virtual TUniquePtr<INode> CreateNode(FDefaultNamedVertexWithLiteralNodeConstructorParams&& InParams) const override
				{
					return nullptr;
				}

				virtual const FMetasoundFrontendClass& GetFrontendClass() const override
				{
					return FrontendClass;
				}

				virtual bool IsNative() const override
				{
					return true;
				}

			protected:

				void SetFrontendClass(const FMetasoundFrontendClass& InClass)
				{
					FrontendClass = InClass;
					ClassInfo = FNodeClassInfo(FrontendClass.Metadata);
				}

			private:
				
				FNodeClassInfo ClassInfo;
				FMetasoundFrontendClass FrontendClass;
			};

			// Node registry entry for input nodes created from a data type registry entry.
			class FInputNodeRegistryEntry : public FDataTypeNodeRegistryEntry
			{
			public:
				FInputNodeRegistryEntry() = delete;

				FInputNodeRegistryEntry(TUniquePtr<IDataTypeRegistryEntry>&& InDataTypeEntry)
				: DataTypeEntry(MoveTemp(InDataTypeEntry))
				{
					if (DataTypeEntry.IsValid())
					{
						SetFrontendClass(DataTypeEntry->GetFrontendInputClass());
					}
				}

				virtual ~FInputNodeRegistryEntry() = default;

				virtual TUniquePtr<INode> CreateNode(FDefaultNamedVertexWithLiteralNodeConstructorParams&& InParams) const override
				{
					if (DataTypeEntry.IsValid())
					{
						return DataTypeEntry->CreateInputNode(MoveTemp(InParams));
					}
					return nullptr;
				}

				virtual TUniquePtr<INodeRegistryEntry> Clone() const override
				{
					if (DataTypeEntry.IsValid())
					{
						return MakeUnique<FInputNodeRegistryEntry>(DataTypeEntry->Clone());
					}
					return MakeUnique<FInputNodeRegistryEntry>(TUniquePtr<IDataTypeRegistryEntry>());
				}

			private:
				
				TUniquePtr<IDataTypeRegistryEntry> DataTypeEntry;
			};

			// Node registry entry for output nodes created from a data type registry entry.
			class FOutputNodeRegistryEntry : public FDataTypeNodeRegistryEntry
			{
			public:
				FOutputNodeRegistryEntry() = delete;

				FOutputNodeRegistryEntry(TUniquePtr<IDataTypeRegistryEntry>&& InDataTypeEntry)
				: DataTypeEntry(MoveTemp(InDataTypeEntry))
				{
					if (DataTypeEntry.IsValid())
					{
						SetFrontendClass(DataTypeEntry->GetFrontendOutputClass());
					}
				}

				virtual ~FOutputNodeRegistryEntry() = default;

				virtual TUniquePtr<INode> CreateNode(FDefaultNamedVertexNodeConstructorParams&& InParams) const override
				{
					if (DataTypeEntry.IsValid())
					{
						return DataTypeEntry->CreateOutputNode(MoveTemp(InParams));
					}
					return nullptr;
				}

				virtual TUniquePtr<INodeRegistryEntry> Clone() const override
				{
					if (DataTypeEntry.IsValid())
					{
						return MakeUnique<FOutputNodeRegistryEntry>(DataTypeEntry->Clone());
					}
					return MakeUnique<FOutputNodeRegistryEntry>(TUniquePtr<IDataTypeRegistryEntry>());
				}

			private:
				
				TUniquePtr<IDataTypeRegistryEntry> DataTypeEntry;
			};

			// Node registry entry for literal nodes created from a data type registry entry.
			class FLiteralNodeRegistryEntry : public FDataTypeNodeRegistryEntry
			{
			public:
				FLiteralNodeRegistryEntry() = delete;

				FLiteralNodeRegistryEntry(TUniquePtr<IDataTypeRegistryEntry>&& InDataTypeEntry)
				: DataTypeEntry(MoveTemp(InDataTypeEntry))
				{
					if (DataTypeEntry.IsValid())
					{
						SetFrontendClass(DataTypeEntry->GetFrontendLiteralClass());
					}
				}

				virtual ~FLiteralNodeRegistryEntry() = default;

				virtual TUniquePtr<INode> CreateNode(FDefaultLiteralNodeConstructorParams&& InParams) const override
				{
					if (DataTypeEntry.IsValid())
					{
						return DataTypeEntry->CreateLiteralNode(MoveTemp(InParams));
					}
					return nullptr;
				}

				virtual TUniquePtr<INodeRegistryEntry> Clone() const override
				{
					if (DataTypeEntry.IsValid())
					{
						return MakeUnique<FLiteralNodeRegistryEntry>(DataTypeEntry->Clone());
					}
					return MakeUnique<FLiteralNodeRegistryEntry>(TUniquePtr<IDataTypeRegistryEntry>());
				}

			private:
				
				TUniquePtr<IDataTypeRegistryEntry> DataTypeEntry;
			};


			// Node registry entry for init variable nodes created from a data type registry entry.
			class FVariableNodeRegistryEntry : public FDataTypeNodeRegistryEntry
			{
			public:
				FVariableNodeRegistryEntry() = delete;

				FVariableNodeRegistryEntry(TUniquePtr<IDataTypeRegistryEntry>&& InDataTypeEntry)
				: DataTypeEntry(MoveTemp(InDataTypeEntry))
				{
					if (DataTypeEntry.IsValid())
					{
						SetFrontendClass(DataTypeEntry->GetFrontendVariableClass());
					}
				}

				virtual ~FVariableNodeRegistryEntry() = default;

				virtual TUniquePtr<INode> CreateNode(FDefaultLiteralNodeConstructorParams&& InParams) const override
				{
					if (DataTypeEntry.IsValid())
					{
						return DataTypeEntry->CreateVariableNode(MoveTemp(InParams));
					}
					return nullptr;
				}

				virtual TUniquePtr<INodeRegistryEntry> Clone() const override
				{
					if (DataTypeEntry.IsValid())
					{
						return MakeUnique<FVariableNodeRegistryEntry>(DataTypeEntry->Clone());
					}
					return MakeUnique<FVariableNodeRegistryEntry>(TUniquePtr<IDataTypeRegistryEntry>());
				}

			private:
				
				TUniquePtr<IDataTypeRegistryEntry> DataTypeEntry;
			};

			// Node registry entry for set variable nodes created from a data type registry entry.
			class FVariableMutatorNodeRegistryEntry : public FDataTypeNodeRegistryEntry
			{
			public:
				FVariableMutatorNodeRegistryEntry() = delete;

				FVariableMutatorNodeRegistryEntry(TUniquePtr<IDataTypeRegistryEntry>&& InDataTypeEntry)
				: DataTypeEntry(MoveTemp(InDataTypeEntry))
				{
					if (DataTypeEntry.IsValid())
					{
						SetFrontendClass(DataTypeEntry->GetFrontendVariableMutatorClass());
					}
				}

				virtual ~FVariableMutatorNodeRegistryEntry() = default;

				virtual TUniquePtr<INode> CreateNode(const FNodeInitData& InParams) const override
				{
					if (DataTypeEntry.IsValid())
					{
						return DataTypeEntry->CreateVariableMutatorNode(InParams);
					}
					return nullptr;
				}

				virtual TUniquePtr<INodeRegistryEntry> Clone() const override
				{
					if (DataTypeEntry.IsValid())
					{
						return MakeUnique<FVariableMutatorNodeRegistryEntry>(DataTypeEntry->Clone());
					}
					return MakeUnique<FVariableMutatorNodeRegistryEntry>(TUniquePtr<IDataTypeRegistryEntry>());
				}

			private:
				
				TUniquePtr<IDataTypeRegistryEntry> DataTypeEntry;
			};

			// Node registry entry for get variable nodes created from a data type registry entry.
			class FVariableAccessorNodeRegistryEntry : public FDataTypeNodeRegistryEntry
			{
			public:
				FVariableAccessorNodeRegistryEntry() = delete;

				FVariableAccessorNodeRegistryEntry(TUniquePtr<IDataTypeRegistryEntry>&& InDataTypeEntry)
				: DataTypeEntry(MoveTemp(InDataTypeEntry))
				{
					if (DataTypeEntry.IsValid())
					{
						SetFrontendClass(DataTypeEntry->GetFrontendVariableAccessorClass());
					}
				}

				virtual ~FVariableAccessorNodeRegistryEntry() = default;

				virtual TUniquePtr<INode> CreateNode(const FNodeInitData& InParams) const override
				{
					if (DataTypeEntry.IsValid())
					{
						return DataTypeEntry->CreateVariableAccessorNode(InParams);
					}
					return nullptr;
				}

				virtual TUniquePtr<INodeRegistryEntry> Clone() const override
				{
					if (DataTypeEntry.IsValid())
					{
						return MakeUnique<FVariableAccessorNodeRegistryEntry>(DataTypeEntry->Clone());
					}
					return MakeUnique<FVariableAccessorNodeRegistryEntry>(TUniquePtr<IDataTypeRegistryEntry>());
				}

			private:
				
				TUniquePtr<IDataTypeRegistryEntry> DataTypeEntry;
			};

			// Node registry entry for get delayed variable nodes created from a data type registry entry.
			class FVariableDeferredAccessorNodeRegistryEntry : public FDataTypeNodeRegistryEntry
			{
			public:
				FVariableDeferredAccessorNodeRegistryEntry() = delete;

				FVariableDeferredAccessorNodeRegistryEntry(TUniquePtr<IDataTypeRegistryEntry>&& InDataTypeEntry)
				: DataTypeEntry(MoveTemp(InDataTypeEntry))
				{
					if (DataTypeEntry.IsValid())
					{
						SetFrontendClass(DataTypeEntry->GetFrontendVariableDeferredAccessorClass());
					}
				}

				virtual ~FVariableDeferredAccessorNodeRegistryEntry() = default;

				virtual TUniquePtr<INode> CreateNode(const FNodeInitData& InParams) const override
				{
					if (DataTypeEntry.IsValid())
					{
						return DataTypeEntry->CreateVariableDeferredAccessorNode(InParams);
					}
					return nullptr;
				}

				virtual TUniquePtr<INodeRegistryEntry> Clone() const override
				{
					if (DataTypeEntry.IsValid())
					{
						return MakeUnique<FVariableDeferredAccessorNodeRegistryEntry>(DataTypeEntry->Clone());
					}
					return MakeUnique<FVariableDeferredAccessorNodeRegistryEntry>(TUniquePtr<IDataTypeRegistryEntry>());
				}

			private:
				
				TUniquePtr<IDataTypeRegistryEntry> DataTypeEntry;
			};

			class FDataTypeRegistry : public IDataTypeRegistry
			{
			public:
				virtual ~FDataTypeRegistry() = default;

				/** Register a data type
				 * @param InName - Name of data type.
				 * @param InEntry - TUniquePtr to data type registry entry.
				 *
				 * @return True on success, false on failure.
				 */
				virtual bool RegisterDataType(TUniquePtr<IDataTypeRegistryEntry>&& InEntry) override;

				virtual void GetRegisteredDataTypeNames(TArray<FName>& OutNames) const override;

				virtual bool GetDataTypeInfo(const UObject* InObject, FDataTypeRegistryInfo& OutInfo) const override;
				virtual bool GetDataTypeInfo(const FName& InDataType, FDataTypeRegistryInfo& OutInfo) const override;

				virtual void IterateDataTypeInfo(TFunctionRef<void(const FDataTypeRegistryInfo&)> InFunction) const override;

				// Return the enum interface for a data type. If the data type does not have 
				// an enum interface, returns a nullptr.
				virtual TSharedPtr<const IEnumDataTypeInterface> GetEnumInterfaceForDataType(const FName& InDataType) const override;

				virtual ELiteralType GetDesiredLiteralType(const FName& InDataType) const override;

				virtual bool IsRegistered(const FName& InDataType) const override;

				virtual bool IsLiteralTypeSupported(const FName& InDataType, ELiteralType InLiteralType) const override;
				virtual bool IsLiteralTypeSupported(const FName& InDataType, EMetasoundFrontendLiteralType InLiteralType) const override;

				virtual UClass* GetUClassForDataType(const FName& InDataType) const override;

				bool IsUObjectProxyFactory(UObject* InObject) const override;
				Audio::IProxyDataPtr CreateProxyFromUObject(const FName& InDataType, UObject* InObject) const override;

				virtual FLiteral CreateDefaultLiteral(const FName& InDataType) const override;
				virtual FLiteral CreateLiteralFromUObject(const FName& InDataType, UObject* InObject) const override;
				virtual FLiteral CreateLiteralFromUObjectArray(const FName& InDataType, const TArray<UObject*>& InObjectArray) const override;

				virtual TSharedPtr<IDataChannel, ESPMode::ThreadSafe> CreateDataChannel(const FName& InDataType, const FOperatorSettings& InOperatorSettings) const override;

				virtual bool GetFrontendInputClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const override;
				virtual bool GetFrontendLiteralClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const override;
				virtual bool GetFrontendOutputClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const override;
				virtual bool GetFrontendVariableClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const override;
				virtual bool GetFrontendVariableMutatorClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const override;
				virtual bool GetFrontendVariableAccessorClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const override;
				virtual bool GetFrontendVariableDeferredAccessorClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const override;

				// Create a new instance of a C++ implemented node from the registry.
				virtual TUniquePtr<INode> CreateInputNode(const FName& InInputType, FInputNodeConstructorParams&& InParams) const override;
				virtual TUniquePtr<INode> CreateLiteralNode(const FName& InLiteralType, FLiteralNodeConstructorParams&& InParams) const override;
				virtual TUniquePtr<INode> CreateOutputNode(const FName& InOutputType, FOutputNodeConstructorParams&& InParams) const override;
				virtual TUniquePtr<INode> CreateReceiveNode(const FName& InOutputType, const FNodeInitData& InParams) const override;
				virtual TUniquePtr<INode> CreateVariableNode(const FName& InDataType, FVariableNodeConstructorParams&&) const override;
				virtual TUniquePtr<INode> CreateVariableMutatorNode(const FName& InDataType, const FNodeInitData&) const override;
				virtual TUniquePtr<INode> CreateVariableAccessorNode(const FName& InDataType, const FNodeInitData&) const override;
				virtual TUniquePtr<INode> CreateVariableDeferredAccessorNode(const FName& InDataType, const FNodeInitData&) const override;

			private:

				const IDataTypeRegistryEntry* FindDataTypeEntry(const FName& InDataTypeName) const;

				TMap<FName, TUniquePtr<IDataTypeRegistryEntry>> RegisteredDataTypes;

				// UObject type names to DataTypeNames
				TMap<const UClass*, FName> RegisteredObjectClasses;
			};

			bool FDataTypeRegistry::RegisterDataType(TUniquePtr<IDataTypeRegistryEntry>&& InEntry)
			{
				if (InEntry.IsValid())
				{
					const FName Name = InEntry->GetDataTypeInfo().DataTypeName;

					if (!ensureAlwaysMsgf(!RegisteredDataTypes.Contains(Name),
						TEXT("Name collision when trying to register Metasound Data Type [Name:%s]. DataType must have "
							"unique name and REGISTER_METASOUND_DATATYPE cannot be called in a public header."),
							*Name.ToString()))
					{
						return false;
					}

					// Register nodes associated with data type.
					FMetasoundFrontendRegistryContainer* NodeRegistry = FMetasoundFrontendRegistryContainer::Get();
					if (ensure(nullptr != NodeRegistry))
					{
						if (InEntry->GetDataTypeInfo().bIsParsable)
						{
							NodeRegistry->RegisterNode(MakeUnique<FInputNodeRegistryEntry>(InEntry->Clone()));
							NodeRegistry->RegisterNode(MakeUnique<FOutputNodeRegistryEntry>(InEntry->Clone()));
							NodeRegistry->RegisterNode(MakeUnique<FLiteralNodeRegistryEntry>(InEntry->Clone()));
							NodeRegistry->RegisterNode(MakeUnique<FVariableNodeRegistryEntry>(InEntry->Clone()));
							NodeRegistry->RegisterNode(MakeUnique<FVariableMutatorNodeRegistryEntry>(InEntry->Clone()));
							NodeRegistry->RegisterNode(MakeUnique<FVariableAccessorNodeRegistryEntry>(InEntry->Clone()));
							NodeRegistry->RegisterNode(MakeUnique<FVariableDeferredAccessorNodeRegistryEntry>(InEntry->Clone()));
						}
					}

					const FDataTypeRegistryInfo& RegistryInfo = InEntry->GetDataTypeInfo();
					if (!RegistryInfo.IsArrayType())
					{
						if (const UClass* Class = RegistryInfo.ProxyGeneratorClass)
						{
							RegisteredObjectClasses.Add(Class, Name);
						}
					}

					RegisteredDataTypes.Add(Name, MoveTemp(InEntry));

					UE_LOG(LogMetaSound, Verbose, TEXT("Registered Metasound Datatype [Name:%s]."), *Name.ToString());
					return true;
				}

				return false;
			}

			void FDataTypeRegistry::GetRegisteredDataTypeNames(TArray<FName>& OutNames) const
			{
				RegisteredDataTypes.GetKeys(OutNames);
			}

			bool FDataTypeRegistry::GetDataTypeInfo(const UObject* InObject, FDataTypeRegistryInfo& OutInfo) const
			{
				if (InObject)
				{
					if (const FName* DataTypeName = RegisteredObjectClasses.Find(InObject->GetClass()))
					{
						if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(*DataTypeName))
						{
							OutInfo = Entry->GetDataTypeInfo();
							return true;
						}
					}
				}

				return false;
			}

			bool FDataTypeRegistry::GetDataTypeInfo(const FName& InDataType, FDataTypeRegistryInfo& OutInfo) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					OutInfo = Entry->GetDataTypeInfo();
					return true;
				}
				return false;
			}

			void FDataTypeRegistry::IterateDataTypeInfo(TFunctionRef<void(const FDataTypeRegistryInfo&)> InFunction) const
			{
				for (const TPair<FName, TUniquePtr<IDataTypeRegistryEntry>>& Entry : RegisteredDataTypes)
				{
					InFunction(Entry.Value->GetDataTypeInfo());
				}
			}

			bool FDataTypeRegistry::IsRegistered(const FName& InDataType) const
			{
				return RegisteredDataTypes.Contains(InDataType);
			}

			// Return the enum interface for a data type. If the data type does not have 
			// an enum interface, returns a nullptr.
			TSharedPtr<const IEnumDataTypeInterface> FDataTypeRegistry::GetEnumInterfaceForDataType(const FName& InDataType) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->GetEnumInterface();
				}
				return nullptr;
			}

			ELiteralType FDataTypeRegistry::GetDesiredLiteralType(const FName& InDataType) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					const FDataTypeRegistryInfo& Info = Entry->GetDataTypeInfo();

					// If there's a designated preferred literal type for this datatype, use that.
					if (Info.PreferredLiteralType != Metasound::ELiteralType::Invalid)
					{
						return Info.PreferredLiteralType;
					}

					// Otherwise, we opt for the highest precision construction option available.
					return MetasoundFrontendDataTypeRegistryPrivate::GetMostDescriptiveLiteralForDataType(Info);
				}
				return Metasound::ELiteralType::Invalid;
			}

			bool FDataTypeRegistry::IsLiteralTypeSupported(const FName& InDataType, ELiteralType InLiteralType) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					const FDataTypeRegistryInfo& Info = Entry->GetDataTypeInfo();

					switch (InLiteralType)
					{
						case Metasound::ELiteralType::Boolean:
						{
							return Info.bIsBoolParsable;
						}
						case Metasound::ELiteralType::BooleanArray:
						{
							return Info.bIsBoolArrayParsable;
						}

						case Metasound::ELiteralType::Integer:
						{
							return Info.bIsIntParsable;
						}
						case Metasound::ELiteralType::IntegerArray:
						{
							return Info.bIsIntArrayParsable;
						}

						case Metasound::ELiteralType::Float:
						{
							return Info.bIsFloatParsable;
						}
						case Metasound::ELiteralType::FloatArray:
						{
							return Info.bIsFloatArrayParsable;
						}

						case Metasound::ELiteralType::String:
						{
							return Info.bIsStringParsable;
						}
						case Metasound::ELiteralType::StringArray:
						{
							return Info.bIsStringArrayParsable;
						}

						case Metasound::ELiteralType::UObjectProxy:
						{
							return Info.bIsProxyParsable;
						}
						case Metasound::ELiteralType::UObjectProxyArray:
						{
							return Info.bIsProxyArrayParsable;
						}

						case Metasound::ELiteralType::None:
						{
							return Info.bIsDefaultParsable;
						}
						case Metasound::ELiteralType::NoneArray:
						{
							return Info.bIsDefaultArrayParsable;
						}

						case Metasound::ELiteralType::Invalid:
						default:
						{
							static_assert(static_cast<int32>(Metasound::ELiteralType::COUNT) == 13, "Possible missing case coverage for ELiteralType");
							return false;
						}
					}
				}

				return false;
			}

			bool FDataTypeRegistry::IsLiteralTypeSupported(const FName& InDataType, EMetasoundFrontendLiteralType InLiteralType) const
			{
				return IsLiteralTypeSupported(InDataType, GetMetasoundLiteralType(InLiteralType));
			}

			UClass* FDataTypeRegistry::GetUClassForDataType(const FName& InDataType) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->GetDataTypeInfo().ProxyGeneratorClass;
				}

				return nullptr;
			}

			FLiteral FDataTypeRegistry::CreateDefaultLiteral(const FName& InDataType) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					const FDataTypeRegistryInfo& Info = Entry->GetDataTypeInfo();
					if (Info.bIsEnum)
					{
						if (TSharedPtr<const IEnumDataTypeInterface> EnumInterface = Entry->GetEnumInterface())
						{
							return FLiteral(EnumInterface->GetDefaultValue());
						}
					}
					return FLiteral::GetDefaultForType(Info.PreferredLiteralType);
				}
				return FLiteral::CreateInvalid();
			}

			bool FDataTypeRegistry::IsUObjectProxyFactory(UObject* InObject) const
			{
				if (!InObject)
				{
					return false;
				}

				UClass* ObjectClass = InObject->GetClass();
				while (ObjectClass != UObject::StaticClass())
				{
					if (RegisteredObjectClasses.Contains(ObjectClass))
					{
						return true;
					}

					ObjectClass = ObjectClass->GetSuperClass();
				}

				return false;
			}

			Audio::IProxyDataPtr FDataTypeRegistry::CreateProxyFromUObject(const FName& InDataType, UObject* InObject) const
			{
				Audio::IProxyDataPtr ProxyPtr;

				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					ProxyPtr = Entry->CreateProxy(InObject);
					if (!ProxyPtr && InObject)
					{
						UE_LOG(LogMetaSound, Error, TEXT("Failed to create a valid proxy from UObject '%s'."), *InObject->GetName());
					}
				}

				return ProxyPtr;
			}

			FLiteral FDataTypeRegistry::CreateLiteralFromUObject(const FName& InDataType, UObject* InObject) const
			{
				Audio::IProxyDataPtr ProxyPtr = CreateProxyFromUObject(InDataType, InObject);
				return Metasound::FLiteral(MoveTemp(ProxyPtr));
			}

			FLiteral FDataTypeRegistry::CreateLiteralFromUObjectArray(const FName& InDataType, const TArray<UObject*>& InObjectArray) const
			{
				TArray<Audio::IProxyDataPtr> ProxyArray;
				for (UObject* InObject : InObjectArray)
				{
					Audio::IProxyDataPtr ProxyPtr = CreateProxyFromUObject(InDataType, InObject);
					ProxyArray.Emplace(MoveTemp(ProxyPtr));
				}

				return Metasound::FLiteral(MoveTemp(ProxyArray));
			}

			TSharedPtr<IDataChannel, ESPMode::ThreadSafe> FDataTypeRegistry::CreateDataChannel(const FName& InDataType, const FOperatorSettings& InOperatorSettings) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->CreateDataChannel(InOperatorSettings);
				}
				return nullptr;
			}

			bool FDataTypeRegistry::GetFrontendInputClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					OutClass = Entry->GetFrontendInputClass();
					return true;
				}
				return false;
			}

			bool FDataTypeRegistry::GetFrontendLiteralClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					OutClass = Entry->GetFrontendLiteralClass();
					return true;
				}
				return false;
			}

			bool FDataTypeRegistry::GetFrontendOutputClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					OutClass = Entry->GetFrontendOutputClass();
					return true;
				}
				return false;
			}

			bool FDataTypeRegistry::GetFrontendVariableClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					OutClass = Entry->GetFrontendVariableClass();
					return true;
				}
				return false;
			}

			bool FDataTypeRegistry::GetFrontendVariableMutatorClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const

			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					OutClass = Entry->GetFrontendVariableMutatorClass();
					return true;
				}
				return false;
			}

			bool FDataTypeRegistry::GetFrontendVariableAccessorClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const

			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					OutClass = Entry->GetFrontendVariableAccessorClass();
					return true;
				}
				return false;
			}

			bool FDataTypeRegistry::GetFrontendVariableDeferredAccessorClass(const FName& InDataType, FMetasoundFrontendClass& OutClass) const

			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					OutClass = Entry->GetFrontendVariableDeferredAccessorClass();
					return true;
				}
				return false;
			}

			TUniquePtr<INode> FDataTypeRegistry::CreateInputNode(const FName& InDataType, FInputNodeConstructorParams&& InParams) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->CreateInputNode(MoveTemp(InParams));
				}
				return nullptr;
			}

			TUniquePtr<INode> FDataTypeRegistry::CreateLiteralNode(const FName& InDataType, FLiteralNodeConstructorParams&& InParams) const 
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->CreateLiteralNode(MoveTemp(InParams));
				}
				return nullptr;
			}

			TUniquePtr<INode> FDataTypeRegistry::CreateOutputNode(const FName& InDataType, FOutputNodeConstructorParams&& InParams) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->CreateOutputNode(MoveTemp(InParams));
				}
				return nullptr;
			}

			TUniquePtr<INode> FDataTypeRegistry::CreateReceiveNode(const FName& InDataType, const FNodeInitData& InParams) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->CreateReceiveNode(InParams);
				}
				return nullptr;
			}

			TUniquePtr<INode> FDataTypeRegistry::CreateVariableNode(const FName& InDataType, FVariableNodeConstructorParams&& InParams) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->CreateVariableNode(MoveTemp(InParams));
				}
				return nullptr;
			}

			TUniquePtr<INode> FDataTypeRegistry::CreateVariableMutatorNode(const FName& InDataType, const FNodeInitData& InParams) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->CreateVariableMutatorNode(InParams);
				}
				return nullptr;
			}

			TUniquePtr<INode> FDataTypeRegistry::CreateVariableAccessorNode(const FName& InDataType, const FNodeInitData& InParams) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->CreateVariableAccessorNode(InParams);
				}
				return nullptr;
			}

			TUniquePtr<INode> FDataTypeRegistry::CreateVariableDeferredAccessorNode(const FName& InDataType, const FNodeInitData& InParams) const
			{
				if (const IDataTypeRegistryEntry* Entry = FindDataTypeEntry(InDataType))
				{
					return Entry->CreateVariableDeferredAccessorNode(InParams);
				}
				return nullptr;
			}

			const IDataTypeRegistryEntry* FDataTypeRegistry::FindDataTypeEntry(const FName& InDataTypeName) const
			{
				const TUniquePtr<IDataTypeRegistryEntry>* Entry = RegisteredDataTypes.Find(InDataTypeName);

				if (ensureMsgf(nullptr != Entry, TEXT("Data type not registered [Name:%s]"), *InDataTypeName.ToString()))
				{
					if (Entry->IsValid())
					{
						return Entry->Get();
					}
				}

				return nullptr;
			}
		}

		IDataTypeRegistry& IDataTypeRegistry::Get()
		{
			static MetasoundFrontendDataTypeRegistryPrivate::FDataTypeRegistry Registry;
			return Registry;
		}

	}
}
