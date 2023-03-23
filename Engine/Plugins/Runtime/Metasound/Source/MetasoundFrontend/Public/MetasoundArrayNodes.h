// Copyright Epic Games, Inc. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Internationalization/Text.h"
#include "MetasoundBuilderInterface.h"
#include "MetasoundDataFactory.h"
#include "MetasoundExecutableOperator.h"
#include "MetasoundFacade.h"
#include "MetasoundLog.h"
#include "MetasoundNodeInterface.h"
#include "MetasoundNodeRegistrationMacro.h"
#include "MetasoundOperatorInterface.h"
#include "MetasoundPrimitives.h"
#include "MetasoundTrigger.h"
#include "MetasoundVertex.h"

#include <type_traits>

#define LOCTEXT_NAMESPACE "MetasoundFrontend"


namespace Metasound
{
	namespace MetasoundArrayNodesPrivate
	{
		// Convenience function for make FNodeClassMetadata of array nodes.
		METASOUNDFRONTEND_API FNodeClassMetadata CreateArrayNodeClassMetadata(const FName& InDataTypeName, const FName& InOperatorName, const FText& InDisplayName, const FText& InDescription, const FVertexInterface& InDefaultInterface);

		// Retrieve the ElementType from an ArrayType
		template<typename ArrayType>
		struct TArrayElementType
		{
			// Default implementation has Type. 
		};

		// ElementType specialization for TArray types.
		template<typename ElementType>
		struct TArrayElementType<TArray<ElementType>>
		{
			using Type = ElementType;
		};
	}

	namespace ArrayNodeVertexNames
	{
		/* Input Vertex Names */
		METASOUNDFRONTEND_API const FVertexName& GetInputArrayName();
		METASOUNDFRONTEND_API const FVertexName& GetInputLeftArrayName();
		METASOUNDFRONTEND_API const FVertexName& GetInputRightArrayName();
		METASOUNDFRONTEND_API const FVertexName& GetInputTriggerName();
		METASOUNDFRONTEND_API const FVertexName& GetInputStartIndexName();
		METASOUNDFRONTEND_API const FVertexName& GetInputEndIndexName();
		METASOUNDFRONTEND_API const FVertexName& GetInputIndexName();
		METASOUNDFRONTEND_API const FVertexName& GetInputValueName();

		/* Output Vertex Names */
		METASOUNDFRONTEND_API const FVertexName& GetOutputNumName();
		METASOUNDFRONTEND_API const FVertexName& GetOutputValueName();
		METASOUNDFRONTEND_API const FVertexName& GetOutputArrayName();
	};

	/** TArrayNumOperator gets the number of elements in an Array. The operator
	 * uses the FNodeFacade and defines the vertex, metadata and vertex interface
	 * statically on the operator class. */
	template<typename ArrayType>
	class TArrayNumOperator : public TExecutableOperator<TArrayNumOperator<ArrayType>>
	{
	public:
		using FArrayDataReadReference = TDataReadReference<ArrayType>;

		// Declare the vertex interface
		static const FVertexInterface& GetDefaultInterface()
		{
			using namespace ArrayNodeVertexNames;

			static const FVertexInterface DefaultInterface(
				FInputVertexInterface(
					TInputDataVertexModel<ArrayType>(GetInputArrayName(), METASOUND_LOCTEXT("ArrayOpArrayNumInput", "Array to inspect."))
				),
				FOutputVertexInterface(
					TOutputDataVertexModel<int32>(GetOutputNumName(), METASOUND_LOCTEXT("ArrayOpArrayNumOutput", "Number of elements in the array."))
				)
			);

			return DefaultInterface;
		}

		static const FNodeClassMetadata& GetNodeInfo()
		{
			auto CreateNodeClassMetadata = []() -> FNodeClassMetadata
			{
				const FName DataTypeName = GetMetasoundDataTypeName<ArrayType>();
				const FName OperatorName = TEXT("Num");
				const FText NodeDisplayName = METASOUND_LOCTEXT_FORMAT("ArrayOpArrayNumDisplayNamePattern", "Num ({0})", GetMetasoundDataTypeDisplayText<ArrayType>());
				const FText NodeDescription = METASOUND_LOCTEXT("ArrayOpArrayNumDescription", "Number of elements in the array");
				const FVertexInterface NodeInterface = GetDefaultInterface();
			
				return MetasoundArrayNodesPrivate::CreateArrayNodeClassMetadata(DataTypeName, OperatorName, NodeDisplayName, NodeDescription, NodeInterface);
			};
			
			static const FNodeClassMetadata Metadata = CreateNodeClassMetadata();

			return Metadata;
		}

		static TUniquePtr<IOperator> CreateOperator(const FCreateOperatorParams& InParams, TArray<TUniquePtr<IOperatorBuildError>>& OutErrors)
		{
			using namespace ArrayNodeVertexNames;
			using namespace MetasoundArrayNodesPrivate;

			const FInputVertexInterface& Inputs = InParams.Node.GetVertexInterface().GetInputInterface();

			// Get the input array or construct an empty one. 
			FArrayDataReadReference Array = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<ArrayType>(Inputs, GetInputArrayName(), InParams.OperatorSettings);

			return MakeUnique<TArrayNumOperator>(Array);
		}

		TArrayNumOperator(FArrayDataReadReference InArray)
		: Array(InArray)
		, Num(TDataWriteReference<int32>::CreateNew())
		{
			// Initialize value for downstream nodes.
			*Num = Array->Num();
		}

		virtual ~TArrayNumOperator() = default;

		virtual FDataReferenceCollection GetInputs() const override
		{
			using namespace ArrayNodeVertexNames;

			FDataReferenceCollection Inputs;

			Inputs.AddDataReadReference(GetInputArrayName(), Array);

			return Inputs;
		}

		virtual FDataReferenceCollection GetOutputs() const override
		{
			using namespace ArrayNodeVertexNames;

			FDataReferenceCollection Outputs;

			Outputs.AddDataReadReference(GetOutputNumName(), Num);

			return Outputs;
		}

		void Execute()
		{
			*Num = Array->Num();
		}

	private:

		FArrayDataReadReference Array;
		TDataWriteReference<int32> Num;
	};

	template<typename ArrayType>
	class TArrayNumNode : public FNodeFacade
	{
	public:
		TArrayNumNode(const FNodeInitData& InInitData)
		: FNodeFacade(InInitData.InstanceName, InInitData.InstanceID, TFacadeOperatorClass<TArrayNumOperator<ArrayType>>())
		{
		}

		virtual ~TArrayNumNode() = default;
	};

	/** TArrayGetOperator copies a value from the array to the output when
	 * a trigger occurs. Initially, the output value is default constructed and
	 * will remain that way until until a trigger is encountered.
	 */
	template<typename ArrayType>
	class TArrayGetOperator : public TExecutableOperator<TArrayGetOperator<ArrayType>>
	{
	public:
		using FArrayDataReadReference = TDataReadReference<ArrayType>;
		using ElementType = typename MetasoundArrayNodesPrivate::TArrayElementType<ArrayType>::Type;

		static const FVertexInterface& GetDefaultInterface()
		{
			using namespace ArrayNodeVertexNames;
			static const FVertexInterface DefaultInterface(
				FInputVertexInterface(
					TInputDataVertexModel<FTrigger>(GetInputTriggerName(), METASOUND_LOCTEXT("ArrayOpArrayGetTrigger", "Trigger to get value.")),
					TInputDataVertexModel<ArrayType>(GetInputArrayName(), METASOUND_LOCTEXT("ArrayOpArrayGetInput", "Input Array.")),
					TInputDataVertexModel<int32>(GetInputIndexName(), METASOUND_LOCTEXT("ArrayOpArrayGetIndex", "Index in Array."))
				),
				FOutputVertexInterface(
					TOutputDataVertexModel<ElementType>(GetOutputValueName(), METASOUND_LOCTEXT("ArrayOpArrayGetOutput", "Value of element at array index."))
				)
			);

			return DefaultInterface;
		}

		static const FNodeClassMetadata& GetNodeInfo()
		{
			auto CreateNodeClassMetadata = []() -> FNodeClassMetadata
			{
				const FName DataTypeName = GetMetasoundDataTypeName<ArrayType>();
				const FName OperatorName = TEXT("Get"); 
				const FText NodeDisplayName = METASOUND_LOCTEXT_FORMAT("ArrayOpArrayGetDisplayNamePattern", "Get ({0})", GetMetasoundDataTypeDisplayText<ArrayType>());
				const FText NodeDescription = METASOUND_LOCTEXT("ArrayOpArrayGetDescription", "Get element at index in array.");
				const FVertexInterface NodeInterface = GetDefaultInterface();
			
				return MetasoundArrayNodesPrivate::CreateArrayNodeClassMetadata(DataTypeName, OperatorName, NodeDisplayName, NodeDescription, NodeInterface);
			};
			
			static const FNodeClassMetadata Metadata = CreateNodeClassMetadata();

			return Metadata;
		}

		static TUniquePtr<IOperator> CreateOperator(const FCreateOperatorParams& InParams, TArray<TUniquePtr<IOperatorBuildError>>& OutErrors)
		{
			using namespace ArrayNodeVertexNames;
			using namespace MetasoundArrayNodesPrivate;

			const FInputVertexInterface& Inputs = InParams.Node.GetVertexInterface().GetInputInterface();

			// Input Trigger
			TDataReadReference<FTrigger> Trigger = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<FTrigger>(Inputs, GetInputTriggerName(), InParams.OperatorSettings);
			
			// Input Array
			FArrayDataReadReference Array = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<ArrayType>(Inputs, GetInputArrayName(), InParams.OperatorSettings);

			// Input Index
			TDataReadReference<int32> Index = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<int32>(Inputs, GetInputIndexName(), InParams.OperatorSettings);

			return MakeUnique<TArrayGetOperator>(InParams.OperatorSettings, Trigger, Array, Index);
		}


		TArrayGetOperator(const FOperatorSettings& InSettings, TDataReadReference<FTrigger> InTrigger, FArrayDataReadReference InArray, TDataReadReference<int32> InIndex)
		: Trigger(InTrigger)
		, Array(InArray)
		, Index(InIndex)
		, Value(TDataWriteReferenceFactory<ElementType>::CreateAny(InSettings))
		{
		}

		virtual ~TArrayGetOperator() = default;

		virtual FDataReferenceCollection GetInputs() const override
		{
			using namespace ArrayNodeVertexNames;

			FDataReferenceCollection Inputs;

			Inputs.AddDataReadReference(GetInputTriggerName(), Trigger);
			Inputs.AddDataReadReference(GetInputArrayName(), Array);
			Inputs.AddDataReadReference(GetInputIndexName(), Index);

			return Inputs;
		}

		virtual FDataReferenceCollection GetOutputs() const override
		{
			using namespace ArrayNodeVertexNames;

			FDataReferenceCollection Outputs;

			Outputs.AddDataReadReference(GetOutputValueName(), Value);

			return Outputs;
		}

		void Execute()
		{
			// Only perform get on trigger.
			if (*Trigger)
			{
				const int32 IndexValue = *Index;
				const ArrayType& ArrayRef = *Array;

				if ((IndexValue >= 0) && (IndexValue < ArrayRef.Num()))
				{
					*Value = ArrayRef[IndexValue];
				}
				else
				{
					UE_LOG(LogMetaSound, Error, TEXT("Attempt to get value at invalid index [ArraySize:%d, Index:%d]"), ArrayRef.Num(), IndexValue);
				}
			}
		}

	private:

		TDataReadReference<FTrigger> Trigger;
		FArrayDataReadReference Array;
		TDataReadReference<int32> Index;
		TDataWriteReference<ElementType> Value;
	};

	template<typename ArrayType>
	class TArrayGetNode : public FNodeFacade
	{
	public:
		TArrayGetNode(const FNodeInitData& InInitData)
		: FNodeFacade(InInitData.InstanceName, InInitData.InstanceID, TFacadeOperatorClass<TArrayGetOperator<ArrayType>>())
		{
		}

		virtual ~TArrayGetNode() = default;
	};

	/** TArraySetOperator sets an element in an array to a specific value. */
	template<typename ArrayType>
	class TArraySetOperator : public TExecutableOperator<TArraySetOperator<ArrayType>>
	{
	public:
		using FArrayDataReadReference = TDataReadReference<ArrayType>;
		using FArrayDataWriteReference = TDataWriteReference<ArrayType>;
		using ElementType = typename MetasoundArrayNodesPrivate::TArrayElementType<ArrayType>::Type;

		static const FVertexInterface& GetDefaultInterface()
		{
			using namespace ArrayNodeVertexNames;
			static const FVertexInterface DefaultInterface(
				FInputVertexInterface(
					TInputDataVertexModel<FTrigger>(GetInputTriggerName(), METASOUND_LOCTEXT("ArrayOpArraySetTrigger", "Trigger to set value.")),
					TInputDataVertexModel<ArrayType>(GetInputArrayName(), METASOUND_LOCTEXT("ArrayOpArraySetInput", "Input Array.")),
					TInputDataVertexModel<int32>(GetInputIndexName(), METASOUND_LOCTEXT("ArrayOpArraySetIndex", "Index in Array.")),
					TInputDataVertexModel<ElementType>(GetInputValueName(), METASOUND_LOCTEXT("ArrayOpArraySetElement", "Value to set"))
				),
				FOutputVertexInterface(
					TOutputDataVertexModel<ArrayType>(GetOutputArrayName(), METASOUND_LOCTEXT("ArrayOpArraySetOutput", "Array after setting."))
				)
			);

			return DefaultInterface;
		}

		static const FNodeClassMetadata& GetNodeInfo()
		{
			auto CreateNodeClassMetadata = []() -> FNodeClassMetadata
			{
				const FName DataTypeName = GetMetasoundDataTypeName<ArrayType>();
				const FName OperatorName = TEXT("Set"); 
				const FText NodeDisplayName = METASOUND_LOCTEXT_FORMAT("ArrayOpArraySetDisplayNamePattern", "Set ({0})", GetMetasoundDataTypeDisplayText<ArrayType>());
				const FText NodeDescription = METASOUND_LOCTEXT("ArrayOpArraySetDescription", "Set element at index in array.");
				const FVertexInterface NodeInterface = GetDefaultInterface();
			
				return MetasoundArrayNodesPrivate::CreateArrayNodeClassMetadata(DataTypeName, OperatorName, NodeDisplayName, NodeDescription, NodeInterface);
			};
			
			static const FNodeClassMetadata Metadata = CreateNodeClassMetadata();

			return Metadata;
		}

		static TUniquePtr<IOperator> CreateOperator(const FCreateOperatorParams& InParams, TArray<TUniquePtr<IOperatorBuildError>>& OutErrors)
		{
			using namespace ArrayNodeVertexNames;
			using namespace MetasoundArrayNodesPrivate;

			const FInputVertexInterface& Inputs = InParams.Node.GetVertexInterface().GetInputInterface();
			
			TDataReadReference<FTrigger> Trigger = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<FTrigger>(Inputs, GetInputTriggerName(), InParams.OperatorSettings);

			FArrayDataReadReference InitArray = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<ArrayType>(Inputs, GetInputArrayName(), InParams.OperatorSettings);
			FArrayDataWriteReference Array = TDataWriteReferenceFactory<ArrayType>::CreateExplicitArgs(InParams.OperatorSettings, *InitArray);

			TDataReadReference<int32> Index = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<int32>(Inputs, GetInputIndexName(), InParams.OperatorSettings);

			TDataReadReference<ElementType> Value = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<ElementType>(Inputs, GetInputValueName(), InParams.OperatorSettings);

			return MakeUnique<TArraySetOperator>(InParams.OperatorSettings, Trigger, InitArray, Array, Index, Value);
		}


		TArraySetOperator(const FOperatorSettings& InSettings, TDataReadReference<FTrigger> InTrigger, FArrayDataReadReference InInitArray, FArrayDataWriteReference InArray, TDataReadReference<int32> InIndex, TDataReadReference<ElementType> InValue)
		: OperatorSettings(InSettings)
		, Trigger(InTrigger)
		, InitArray(InInitArray)
		, Array(InArray)
		, Index(InIndex)
		, Value(InValue)
		{
		}

		virtual ~TArraySetOperator() = default;

		virtual FDataReferenceCollection GetInputs() const override
		{
			using namespace ArrayNodeVertexNames;
			FDataReferenceCollection Inputs;

			Inputs.AddDataReadReference(GetInputTriggerName(), Trigger);
			Inputs.AddDataReadReference(GetInputArrayName(), InitArray);
			Inputs.AddDataReadReference(GetInputIndexName(), Index);
			Inputs.AddDataReadReference(GetInputValueName(), Value);

			return Inputs;
		}

		virtual FDataReferenceCollection GetOutputs() const override
		{
			using namespace ArrayNodeVertexNames;
			FDataReferenceCollection Outputs;

			Outputs.AddDataReadReference(GetOutputArrayName(), Array);

			return Outputs;
		}

		void Execute()
		{
			if (*Trigger)
			{
				const int32 IndexValue = *Index;
				ArrayType& ArrayRef = *Array;

				if ((IndexValue >= 0) && (IndexValue < ArrayRef.Num()))
				{
					ArrayRef[IndexValue] = *Value;
				}
				else
				{
					UE_LOG(LogMetaSound, Error, TEXT("Attempt to set value at invalid index [ArraySize:%d, Index:%d]"), ArrayRef.Num(), IndexValue);
				}
			}
		}

	private:
		FOperatorSettings OperatorSettings;

		TDataReadReference<FTrigger> Trigger;
		FArrayDataReadReference InitArray;
		FArrayDataWriteReference Array;
		TDataReadReference<int32> Index;
		TDataReadReference<ElementType> Value;
	};

	template<typename ArrayType>
	class TArraySetNode : public FNodeFacade
	{
	public:
		TArraySetNode(const FNodeInitData& InInitData)
		: FNodeFacade(InInitData.InstanceName, InInitData.InstanceID, TFacadeOperatorClass<TArraySetOperator<ArrayType>>())
		{
		}

		virtual ~TArraySetNode() = default;
	};

	/** TArrayConcatOperator concatenates two arrays on trigger. */
	template<typename ArrayType>
	class TArrayConcatOperator : public TExecutableOperator<TArrayConcatOperator<ArrayType>>
	{
	public:
		using FArrayDataReadReference = TDataReadReference<ArrayType>;
		using FArrayDataWriteReference = TDataWriteReference<ArrayType>;
		using ElementType = typename MetasoundArrayNodesPrivate::TArrayElementType<ArrayType>::Type;

		static const FVertexInterface& GetDefaultInterface()
		{
			using namespace ArrayNodeVertexNames;

			static const FVertexInterface DefaultInterface(
				FInputVertexInterface(
					TInputDataVertexModel<FTrigger>(GetInputTriggerName(), METASOUND_LOCTEXT("ArrayOpArrayConcatTrigger", "Trigger to set value.")),
					TInputDataVertexModel<ArrayType>(GetInputLeftArrayName(), METASOUND_LOCTEXT("ArrayOpArrayConcatInputLeft", "Input Left Array.")),
					TInputDataVertexModel<ArrayType>(GetInputRightArrayName(), METASOUND_LOCTEXT("ArrayOpArrayConcatInputRight", "Input Right Array."))
				),
				FOutputVertexInterface(
					TOutputDataVertexModel<ArrayType>(GetOutputArrayName(), METASOUND_LOCTEXT("ArrayOpArrayConcatOutput", "Array after concatenation."))
				)
			);

			return DefaultInterface;
		}

		static const FNodeClassMetadata& GetNodeInfo()
		{
			auto CreateNodeClassMetadata = []() -> FNodeClassMetadata
			{
				const FName DataTypeName = GetMetasoundDataTypeName<ArrayType>();
				const FName OperatorName = TEXT("Concat"); 
				const FText NodeDisplayName = METASOUND_LOCTEXT_FORMAT("ArrayOpArrayConcatDisplayNamePattern", "Concatenate ({0})", GetMetasoundDataTypeDisplayText<ArrayType>());
				const FText NodeDescription = METASOUND_LOCTEXT("ArrayOpArrayConcatDescription", "Concatenates two arrays on trigger.");
				const FVertexInterface NodeInterface = GetDefaultInterface();
			
				return MetasoundArrayNodesPrivate::CreateArrayNodeClassMetadata(DataTypeName, OperatorName, NodeDisplayName, NodeDescription, NodeInterface);
			};
			
			static const FNodeClassMetadata Metadata = CreateNodeClassMetadata();

			return Metadata;
		}

		static TUniquePtr<IOperator> CreateOperator(const FCreateOperatorParams& InParams, TArray<TUniquePtr<IOperatorBuildError>>& OutErrors)
		{
			using namespace ArrayNodeVertexNames;
			using namespace MetasoundArrayNodesPrivate;

			const FInputVertexInterface& Inputs = InParams.Node.GetVertexInterface().GetInputInterface();
			
			TDataReadReference<FTrigger> Trigger = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<FTrigger>(Inputs, GetInputTriggerName(), InParams.OperatorSettings);

			FArrayDataReadReference LeftArray = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<ArrayType>(Inputs, GetInputLeftArrayName(), InParams.OperatorSettings);
			FArrayDataReadReference RightArray = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<ArrayType>(Inputs, GetInputRightArrayName(), InParams.OperatorSettings);

			FArrayDataWriteReference OutArray = TDataWriteReferenceFactory<ArrayType>::CreateExplicitArgs(InParams.OperatorSettings);

			return MakeUnique<TArrayConcatOperator>(Trigger, LeftArray, RightArray, OutArray);
		}


		TArrayConcatOperator(TDataReadReference<FTrigger> InTrigger, FArrayDataReadReference InLeftArray, FArrayDataReadReference InRightArray, FArrayDataWriteReference InOutArray)
		: Trigger(InTrigger)
		, LeftArray(InLeftArray)
		, RightArray(InRightArray)
		, OutArray(InOutArray)
		{
		}

		virtual ~TArrayConcatOperator() = default;

		virtual FDataReferenceCollection GetInputs() const override
		{
			using namespace ArrayNodeVertexNames;
			FDataReferenceCollection Inputs;

			Inputs.AddDataReadReference(GetInputTriggerName(), Trigger);
			Inputs.AddDataReadReference(GetInputLeftArrayName(), LeftArray);
			Inputs.AddDataReadReference(GetInputRightArrayName(), RightArray);

			return Inputs;
		}

		virtual FDataReferenceCollection GetOutputs() const override
		{
			using namespace ArrayNodeVertexNames;
			FDataReferenceCollection Outputs;

			Outputs.AddDataReadReference(GetOutputArrayName(), OutArray);

			return Outputs;
		}

		void Execute()
		{
			if (*Trigger)
			{
				*OutArray = *LeftArray;
				OutArray->Append(*RightArray);
			}
		}

	private:

		TDataReadReference<FTrigger> Trigger;
		FArrayDataReadReference LeftArray;
		FArrayDataReadReference RightArray;
		FArrayDataWriteReference OutArray;
	};

	template<typename ArrayType>
	class TArrayConcatNode : public FNodeFacade
	{
	public:
		TArrayConcatNode(const FNodeInitData& InInitData)
		: FNodeFacade(InInitData.InstanceName, InInitData.InstanceID, TFacadeOperatorClass<TArrayConcatOperator<ArrayType>>())
		{
		}

		virtual ~TArrayConcatNode() = default;
	};

	/** TArraySubsetOperator slices an array on trigger. */
	template<typename ArrayType>
	class TArraySubsetOperator : public TExecutableOperator<TArraySubsetOperator<ArrayType>>
	{
	public:
		using FArrayDataReadReference = TDataReadReference<ArrayType>;
		using FArrayDataWriteReference = TDataWriteReference<ArrayType>;
		using ElementType = typename MetasoundArrayNodesPrivate::TArrayElementType<ArrayType>::Type;

		static const FVertexInterface& GetDefaultInterface()
		{
			using namespace ArrayNodeVertexNames;

			static const FVertexInterface DefaultInterface(
				FInputVertexInterface(
					TInputDataVertexModel<FTrigger>(GetInputTriggerName(), METASOUND_LOCTEXT("ArrayOpArraySubsetTrigger", "Trigger to set value.")),
					TInputDataVertexModel<ArrayType>(GetInputArrayName(), METASOUND_LOCTEXT("ArrayOpArraySubsetInputLeft", "Input Array.")),
					TInputDataVertexModel<int32>(GetInputStartIndexName(), METASOUND_LOCTEXT("ArrayOpArraySubsetStartIndex", "First index to include.")),
					TInputDataVertexModel<int32>(GetInputEndIndexName(), METASOUND_LOCTEXT("ArrayOpArraySubsetEndIndex", "Last index to include."))

				),
				FOutputVertexInterface(
					TOutputDataVertexModel<ArrayType>(GetOutputArrayName(), METASOUND_LOCTEXT("ArrayOpArraySubsetOutput", "Subset of input array."))
				)
			);

			return DefaultInterface;
		}

		static const FNodeClassMetadata& GetNodeInfo()
		{
			auto CreateNodeClassMetadata = []() -> FNodeClassMetadata
			{
				const FName DataTypeName = GetMetasoundDataTypeName<ArrayType>();
				const FName OperatorName = TEXT("Subset"); 
				const FText NodeDisplayName = METASOUND_LOCTEXT_FORMAT("ArrayOpArraySubsetDisplayNamePattern", "Subset ({0})", GetMetasoundDataTypeDisplayText<ArrayType>());
				const FText NodeDescription = METASOUND_LOCTEXT("ArrayOpArraySubsetDescription", "Subset array on trigger.");
				const FVertexInterface NodeInterface = GetDefaultInterface();
			
				return MetasoundArrayNodesPrivate::CreateArrayNodeClassMetadata(DataTypeName, OperatorName, NodeDisplayName, NodeDescription, NodeInterface);
			};
			
			static const FNodeClassMetadata Metadata = CreateNodeClassMetadata();

			return Metadata;
		}

		static TUniquePtr<IOperator> CreateOperator(const FCreateOperatorParams& InParams, TArray<TUniquePtr<IOperatorBuildError>>& OutErrors)
		{
			using namespace ArrayNodeVertexNames;
			using namespace MetasoundArrayNodesPrivate;

			const FInputVertexInterface& Inputs = InParams.Node.GetVertexInterface().GetInputInterface();
			
			TDataReadReference<FTrigger> Trigger = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<FTrigger>(Inputs, GetInputTriggerName(), InParams.OperatorSettings);

			FArrayDataReadReference InArray = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<ArrayType>(Inputs, GetInputArrayName(), InParams.OperatorSettings);

			TDataReadReference<int32> StartIndex = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<int32>(Inputs, GetInputStartIndexName(), InParams.OperatorSettings);
			TDataReadReference<int32> EndIndex = InParams.InputDataReferences.GetDataReadReferenceOrConstructWithVertexDefault<int32>(Inputs, GetInputEndIndexName(), InParams.OperatorSettings);

			FArrayDataWriteReference OutArray = TDataWriteReferenceFactory<ArrayType>::CreateExplicitArgs(InParams.OperatorSettings);

			return MakeUnique<TArraySubsetOperator>(Trigger, InArray, StartIndex, EndIndex, OutArray);
		}


		TArraySubsetOperator(TDataReadReference<FTrigger> InTrigger, FArrayDataReadReference InInputArray, TDataReadReference<int32> InStartIndex, TDataReadReference<int32> InEndIndex, FArrayDataWriteReference InOutputArray)
		: Trigger(InTrigger)
		, InputArray(InInputArray)
		, StartIndex(InStartIndex)
		, EndIndex(InEndIndex)
		, OutputArray(InOutputArray)
		{
		}

		virtual ~TArraySubsetOperator() = default;

		virtual FDataReferenceCollection GetInputs() const override
		{
			using namespace ArrayNodeVertexNames;

			FDataReferenceCollection Inputs;

			Inputs.AddDataReadReference(GetInputTriggerName(), Trigger);
			Inputs.AddDataReadReference(GetInputArrayName(), InputArray);
			Inputs.AddDataReadReference(GetInputStartIndexName(), StartIndex);
			Inputs.AddDataReadReference(GetInputEndIndexName(), EndIndex);

			return Inputs;
		}

		virtual FDataReferenceCollection GetOutputs() const override
		{
			using namespace ArrayNodeVertexNames;

			FDataReferenceCollection Outputs;

			Outputs.AddDataReadReference(GetOutputArrayName(), OutputArray);

			return Outputs;
		}

		void Execute()
		{
			if (*Trigger)
			{
				OutputArray->Reset();

				const ArrayType& InputArrayRef = *InputArray;
				const int32 StartIndexValue = FMath::Max(0, *StartIndex);
				const int32 EndIndexValue = FMath::Min(InputArrayRef.Num(), *EndIndex + 1);

				if (StartIndexValue < EndIndexValue)
				{
					const int32 Num = EndIndexValue - StartIndexValue;
					OutputArray->Append(&InputArrayRef[StartIndexValue], Num);
				}
			}
		}

	private:

		TDataReadReference<FTrigger> Trigger;
		FArrayDataReadReference InputArray;
		TDataReadReference<int32> StartIndex;
		TDataReadReference<int32> EndIndex;
		FArrayDataWriteReference OutputArray;
	};

	template<typename ArrayType>
	class TArraySubsetNode : public FNodeFacade
	{
	public:
		TArraySubsetNode(const FNodeInitData& InInitData)
		: FNodeFacade(InInitData.InstanceName, InInitData.InstanceID, TFacadeOperatorClass<TArraySubsetOperator<ArrayType>>())
		{
		}

		virtual ~TArraySubsetNode() = default;
	};
} // namespace Metasound
#undef LOCTEXT_NAMESPACE
