// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetasoundEditorGraphBuilder.h"

#include "Algo/Sort.h"
#include "Algo/Transform.h"
#include "Algo/AnyOf.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GraphEditor.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Metasound.h"
#include "MetasoundAssetBase.h"
#include "MetasoundEditor.h"
#include "MetasoundEditorGraph.h"
#include "MetasoundEditorGraphBuilder.h"
#include "MetasoundEditorGraphInputNode.h"
#include "MetasoundEditorGraphMemberDefaults.h"
#include "MetasoundEditorGraphNode.h"
#include "MetasoundEditorGraphSchema.h"
#include "MetasoundEditorGraphValidation.h"
#include "MetasoundEditorModule.h"
#include "MetasoundEditorSettings.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendDocumentAccessPtr.h"
#include "MetasoundFrontendQuery.h"
#include "MetasoundFrontendQuerySteps.h"
#include "MetasoundFrontendRegistries.h"
#include "MetasoundFrontendSearchEngine.h"
#include "MetasoundFrontendTransform.h"
#include "MetasoundLiteral.h"
#include "MetasoundUObjectRegistry.h"
#include "MetasoundVariableNodes.h"
#include "MetasoundVertex.h"
#include "Modules/ModuleManager.h"
#include "Templates/Tuple.h"
#include "Toolkits/ToolkitManager.h"
#include "Widgets/Notifications/SNotificationList.h"

#define LOCTEXT_NAMESPACE "MetaSoundEditor"


namespace Metasound
{
	namespace Editor
	{
		const FName FGraphBuilder::PinCategoryAudio = "audio";
		const FName FGraphBuilder::PinCategoryBoolean = "bool";
		//const FName FGraphBuilder::PinCategoryDouble = "double";
		const FName FGraphBuilder::PinCategoryFloat = "float";
		const FName FGraphBuilder::PinCategoryInt32 = "int";
		//const FName FGraphBuilder::PinCategoryInt64 = "int64";
		const FName FGraphBuilder::PinCategoryObject = "object";
		const FName FGraphBuilder::PinCategoryString = "string";
		const FName FGraphBuilder::PinCategoryTrigger = "trigger";

		const FName FGraphBuilder::PinSubCategoryTime = "time";

		namespace GraphBuilderPrivate
		{
			void DeleteNode(UObject& InMetaSound, Frontend::FNodeHandle InNodeHandle)
			{
				if (InNodeHandle->IsValid())
				{
					Frontend::FGraphHandle GraphHandle = InNodeHandle->GetOwningGraph();
					if (GraphHandle->IsValid())
					{
						GraphHandle->RemoveNode(*InNodeHandle);
					}
				}
			}

			FName GenerateUniqueName(const TArray<FName>& InExistingNames, const FString& InBaseName)
			{
				int32 PostFixInt = 0;
				FString NewName = InBaseName;

				while (InExistingNames.Contains(*NewName))
				{
					PostFixInt++;
					NewName = FString::Format(TEXT("{0} {1}"), { InBaseName, PostFixInt });
				}

				return FName(*NewName);
			}
		} // namespace GraphBuilderPrivate

		FText FGraphBuilder::GetDisplayName(const FMetasoundFrontendClassMetadata& InClassMetadata, FName InNodeName, bool bInIncludeNamespace)
		{
			using namespace Frontend;

			FName Namespace;
			FName ParameterName;
			Audio::FParameterPath::SplitName(InNodeName, Namespace, ParameterName);

			FText DisplayName;
			auto GetAssetDisplayNameFromMetadata = [&DisplayName](const FMetasoundFrontendClassMetadata& Metadata)
			{
				DisplayName = Metadata.GetDisplayName();
				if (DisplayName.IsEmptyOrWhitespace())
				{
					const FNodeRegistryKey RegistryKey = NodeRegistryKey::CreateKey(Metadata);
					bool bIsClassNative = FMetasoundFrontendRegistryContainer::Get()->IsNodeNative(RegistryKey);
					if (!bIsClassNative)
					{
						if (IMetaSoundAssetManager* AssetManager = IMetaSoundAssetManager::Get())
						{
							if (const FSoftObjectPath* Path = AssetManager->FindObjectPathFromKey(RegistryKey))
							{
								DisplayName = FText::FromString(Path->GetAssetName());
							}
						}
					}
				}
			};

			// 1. Try to get display name from metadata or asset if one can be found from the asset manager
			GetAssetDisplayNameFromMetadata(InClassMetadata);

			// 2. If version is missing from the registry or from asset system, then this node
			// will not provide a useful DisplayName.  In that case, attempt to find the next highest
			// class & associated DisplayName.
			if (DisplayName.IsEmptyOrWhitespace())
			{
				FMetasoundFrontendClass ClassWithHighestVersion;
				if (ISearchEngine::Get().FindClassWithHighestVersion(InClassMetadata.GetClassName(), ClassWithHighestVersion))
				{
					GetAssetDisplayNameFromMetadata(ClassWithHighestVersion.Metadata);
				}
			}

			// 3. If that cannot be found, build a title from the cached node registry FName.
			if (DisplayName.IsEmptyOrWhitespace())
			{
				DisplayName = FText::FromString(ParameterName.ToString());
			}

			// 4. Tack on the namespace if requested
			if (bInIncludeNamespace)
			{
				if (!Namespace.IsNone())
				{
					return FText::Format(LOCTEXT("ClassMetadataDisplayNameWithNamespaceFormat", "{0} ({1})"), DisplayName, FText::FromName(Namespace));
				}
			}

			return DisplayName;
		}

		FText FGraphBuilder::GetDisplayName(const Frontend::INodeController& InFrontendNode, bool bInIncludeNamespace)
		{
			using namespace Frontend;

			FText DisplayName = InFrontendNode.GetDisplayName();
			if (!DisplayName.IsEmptyOrWhitespace())
			{
				return DisplayName;
			}

			return GetDisplayName(InFrontendNode.GetClassMetadata(), InFrontendNode.GetNodeName(), bInIncludeNamespace);
		}

		FText FGraphBuilder::GetDisplayName(const Frontend::IInputController& InFrontendInput)
		{
			FText DisplayName = InFrontendInput.GetDisplayName();
			if (DisplayName.IsEmptyOrWhitespace())
			{
				DisplayName = FText::FromName(InFrontendInput.GetName());
			}
			return DisplayName;
		}

		FText FGraphBuilder::GetDisplayName(const Frontend::IOutputController& InFrontendOutput)
		{
			FText DisplayName = InFrontendOutput.GetDisplayName();
			if (DisplayName.IsEmptyOrWhitespace())
			{
				DisplayName = FText::FromName(InFrontendOutput.GetName());
			}
			return DisplayName;
		}

		FText FGraphBuilder::GetDisplayName(const Frontend::IVariableController& InFrontendVariable, bool bInIncludeNamespace)
		{
			FText DisplayName = InFrontendVariable.GetDisplayName();
			if (DisplayName.IsEmptyOrWhitespace())
			{
				FName Namespace;
				FName ParameterName;
				Audio::FParameterPath::SplitName(InFrontendVariable.GetName(), Namespace, ParameterName);

				DisplayName = FText::FromName(ParameterName);
				if (bInIncludeNamespace && !Namespace.IsNone())
				{
					return FText::Format(LOCTEXT("ClassMetadataDisplayNameWithNamespaceFormat", "{0} ({1})"), DisplayName, FText::FromName(Namespace));
				}
			}

			return DisplayName;
		}

		FName FGraphBuilder::GetPinName(const Frontend::IOutputController& InFrontendOutput)
		{
			Frontend::FConstNodeHandle OwningNode = InFrontendOutput.GetOwningNode();
			EMetasoundFrontendClassType OwningNodeClassType = OwningNode->GetClassMetadata().GetType();

			switch (OwningNodeClassType)
			{
				case EMetasoundFrontendClassType::Variable:
				case EMetasoundFrontendClassType::VariableAccessor:
				case EMetasoundFrontendClassType::VariableDeferredAccessor:
				case EMetasoundFrontendClassType::VariableMutator:
				{
					// All variables nodes use the same pin name for user-modifiable node
					// inputs and outputs and the editor does not display the pin's name. The
					// editor instead displays the variable's name in place of the pin name to
					// maintain a consistent look and behavior to input and output nodes.
					return VariableNames::GetOutputDataName();
				}
				case EMetasoundFrontendClassType::Input:
				case EMetasoundFrontendClassType::Output:
				{
					return OwningNode->GetNodeName();
				}

				default:
				{
					return InFrontendOutput.GetName();
				}
			}
		}

		FName FGraphBuilder::GetPinName(const Frontend::IInputController& InFrontendInput)
		{
			Frontend::FConstNodeHandle OwningNode = InFrontendInput.GetOwningNode();
			EMetasoundFrontendClassType OwningNodeClassType = OwningNode->GetClassMetadata().GetType();

			switch (OwningNodeClassType)
			{
				case EMetasoundFrontendClassType::Variable:
				case EMetasoundFrontendClassType::VariableAccessor:
				case EMetasoundFrontendClassType::VariableDeferredAccessor:
				case EMetasoundFrontendClassType::VariableMutator:
				{
					// All variables nodes use the same pin name for user-modifiable node
					// inputs and outputs and the editor does not display the pin's name. The
					// editor instead displays the variable's name in place of the pin name to
					// maintain a consistent look and behavior to input and output nodes.
					return VariableNames::GetInputDataName();
				}

				case EMetasoundFrontendClassType::Input:
				case EMetasoundFrontendClassType::Output:
				{
					return OwningNode->GetNodeName();
				}

				default:
				{
					return InFrontendInput.GetName();
				}
			}
		}

		UMetasoundEditorGraphExternalNode* FGraphBuilder::AddExternalNode(UObject& InMetaSound, Frontend::FNodeHandle& InNodeHandle, FVector2D InLocation, bool bInSelectNewNode)
		{
			using namespace Frontend;

			UMetasoundEditorGraphExternalNode* NewGraphNode = nullptr;
			if (!ensure(InNodeHandle->GetClassMetadata().GetType() == EMetasoundFrontendClassType::External))
			{
				return nullptr;
			}

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);
			UEdGraph& Graph = MetaSoundAsset->GetGraphChecked();
			FGraphNodeCreator<UMetasoundEditorGraphExternalNode> NodeCreator(Graph);

			NewGraphNode = NodeCreator.CreateNode(bInSelectNewNode);
			if (ensure(NewGraphNode))
			{
				const FNodeRegistryKey RegistryKey = NodeRegistryKey::CreateKey(InNodeHandle->GetClassMetadata());
				NewGraphNode->bIsClassNative = FMetasoundFrontendRegistryContainer::Get()->IsNodeNative(RegistryKey);
				NewGraphNode->ClassName = InNodeHandle->GetClassMetadata().GetClassName();
				NewGraphNode->CacheTitle();

				NodeCreator.Finalize();
				InitGraphNode(InNodeHandle, NewGraphNode, InMetaSound);
				NewGraphNode->SetNodeLocation(InLocation);

				// Adding external node may introduce referenced asset so rebuild referenced keys.
				MetaSoundAsset->RebuildReferencedAssetClassKeys();
			}

			return NewGraphNode;
		}

		UMetasoundEditorGraphExternalNode* FGraphBuilder::AddExternalNode(UObject& InMetaSound, const FMetasoundFrontendClassMetadata& InMetadata, FVector2D InLocation, bool bInSelectNewNode)
		{
			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);

			Frontend::FNodeHandle NodeHandle = MetaSoundAsset->GetRootGraphHandle()->AddNode(InMetadata);
			return AddExternalNode(InMetaSound, NodeHandle, InLocation, bInSelectNewNode);
		}

		UMetasoundEditorGraphVariableNode* FGraphBuilder::AddVariableNode(UObject& InMetaSound, Frontend::FNodeHandle& InNodeHandle, FVector2D InLocation, bool bInSelectNewNode)
		{
			using namespace Frontend;

			EMetasoundFrontendClassType ClassType = InNodeHandle->GetClassMetadata().GetType();
			const bool bIsSupportedClassType = (ClassType == EMetasoundFrontendClassType::VariableAccessor) 
				|| (ClassType == EMetasoundFrontendClassType::VariableDeferredAccessor)
				|| (ClassType == EMetasoundFrontendClassType::VariableMutator);

			if (!ensure(bIsSupportedClassType))
			{
				return nullptr;
			}

			FConstVariableHandle FrontendVariable = InNodeHandle->GetOwningGraph()->FindVariableContainingNode(InNodeHandle->GetID());
			if (!ensure(FrontendVariable->IsValid()))
			{
				return nullptr;
			}

			UMetasoundEditorGraphVariableNode* NewGraphNode = nullptr;
			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			if (ensure(nullptr != MetaSoundAsset))
			{
				if (UMetasoundEditorGraph* MetasoundGraph = CastChecked<UMetasoundEditorGraph>(MetaSoundAsset->GetGraph()))
				{
					FGraphNodeCreator<UMetasoundEditorGraphVariableNode> NodeCreator(*MetasoundGraph);

					NewGraphNode = NodeCreator.CreateNode(bInSelectNewNode);
					if (ensure(NewGraphNode))
					{
						NewGraphNode->ClassName = InNodeHandle->GetClassMetadata().GetClassName();
						NewGraphNode->ClassType = ClassType;
						NodeCreator.Finalize();
						InitGraphNode(InNodeHandle, NewGraphNode, InMetaSound);

						UMetasoundEditorGraphVariable* Variable = MetasoundGraph->FindOrAddVariable(FrontendVariable);
						if (ensure(Variable))
						{
							NewGraphNode->Variable = Variable;

							// Ensures the variable node value is synced with the editor literal value should it be set
							constexpr bool bPostTransaction = false;
							Variable->UpdateFrontendDefaultLiteral(bPostTransaction);
						}

						MetasoundGraph->SetSynchronizationRequired();
						NewGraphNode->SetNodeLocation(InLocation);
					}
				}
			}

			return NewGraphNode;
		}

		UMetasoundEditorGraphOutputNode* FGraphBuilder::AddOutputNode(UObject& InMetaSound, Frontend::FNodeHandle& InNodeHandle, FVector2D InLocation, bool bInSelectNewNode)
		{
			using namespace Frontend;

			UMetasoundEditorGraphOutputNode* NewGraphNode = nullptr;
			if (!ensure(InNodeHandle->GetClassMetadata().GetType() == EMetasoundFrontendClassType::Output))
			{
				return nullptr;
			}

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);
			UEdGraph& Graph = MetaSoundAsset->GetGraphChecked();
			FGraphNodeCreator<UMetasoundEditorGraphOutputNode> NodeCreator(Graph);

			NewGraphNode = NodeCreator.CreateNode(bInSelectNewNode);
			if (ensure(NewGraphNode))
			{
				UMetasoundEditorGraph* MetasoundGraph = CastChecked<UMetasoundEditorGraph>(&Graph);

				UMetasoundEditorGraphOutput* Output = MetasoundGraph->FindOrAddOutput(InNodeHandle);
				if (ensure(Output))
				{
					NewGraphNode->Output = Output;
					NodeCreator.Finalize();
					InitGraphNode(InNodeHandle, NewGraphNode, InMetaSound);

					// Ensures the output node value is synced with the editor literal value should it be set
					constexpr bool bPostTransaction = false;
					Output->UpdateFrontendDefaultLiteral(bPostTransaction);

					MetasoundGraph->SetSynchronizationRequired();
				}

				NewGraphNode->CacheTitle();
				NewGraphNode->SetNodeLocation(InLocation);
			}

			return NewGraphNode;
		}

		void FGraphBuilder::InitGraphNode(Frontend::FNodeHandle& InNodeHandle, UMetasoundEditorGraphNode* NewGraphNode, UObject& InMetaSound)
		{
			NewGraphNode->SetNodeID(InNodeHandle->GetID());
			RebuildNodePins(*NewGraphNode);
		}

		bool FGraphBuilder::ValidateGraph(UObject& InMetaSound, bool bForceRefreshNodes)
		{
			using namespace Frontend;

			TSharedPtr<SGraphEditor> GraphEditor;
			TSharedPtr<FEditor> MetaSoundEditor = GetEditorForMetasound(InMetaSound);
			if (MetaSoundEditor.IsValid())
			{
				GraphEditor = MetaSoundEditor->GetGraphEditor();
			}

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);
			UMetasoundEditorGraph& Graph = *CastChecked<UMetasoundEditorGraph>(&MetaSoundAsset->GetGraphChecked());

			bForceRefreshNodes |= Graph.RequiresForceRefreshNodes();
			Graph.ClearForceRefreshNodes();

			FGraphValidationResults Results;

			bool bMarkDirty = false;

			Graph.ValidateInternal(Results);
			for (const FGraphNodeValidationResult& Result : Results.GetResults())
			{
				bMarkDirty |= Result.bIsDirty;
				check(Result.Node);
				const bool bInterfaceChange = Result.Node->ContainsInterfaceChange();
				const bool bMetadataChange = Result.Node->ContainsMetadataChange();
				const bool bStyleChange = Result.Node->ContainsStyleChange();

				const FText Title = Result.Node->GetCachedTitle();
				Result.Node->CacheTitle();
				const bool bTitleUpdated = !Title.IdenticalTo(Result.Node->GetCachedTitle());

				if (Result.bIsDirty || bTitleUpdated || bMetadataChange || bInterfaceChange || bStyleChange || bForceRefreshNodes)
				{
					Result.Node->SyncChangeIDs();

					if (GraphEditor.IsValid())
					{
						GraphEditor->RefreshNode(*Result.Node);
					}
				}
			}

			if (MetaSoundEditor.IsValid())
			{
				MetaSoundEditor->RefreshGraphMemberMenu();
			}

			if (bMarkDirty)
			{
				InMetaSound.MarkPackageDirty();
			}

			return Results.IsValid();
		}

		TArray<FString> FGraphBuilder::GetDataTypeNameCategories(const FName& InDataTypeName)
		{
			FString CategoryString = InDataTypeName.ToString();

			TArray<FString> Categories;
			CategoryString.ParseIntoArray(Categories, TEXT(":"));

			if (Categories.Num() > 0)
			{
				// Remove name
				Categories.RemoveAt(Categories.Num() - 1);
			}

			return Categories;
		}

		FName FGraphBuilder::GenerateUniqueNameByClassType(const UObject& InMetaSound, EMetasoundFrontendClassType InClassType, const FString& InBaseName)
		{
			const FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);

			// Get existing names.
			TArray<FName> ExistingNames;
			MetaSoundAsset->GetRootGraphHandle()->IterateConstNodes([&](const Frontend::FConstNodeHandle& Node)
			{
				ExistingNames.Add(Node->GetNodeName());
			}, InClassType);

			return GraphBuilderPrivate::GenerateUniqueName(ExistingNames, InBaseName);
		}

		TSharedPtr<FEditor> FGraphBuilder::GetEditorForMetasound(const UObject& Metasound)
		{
			// TODO: FToolkitManager is deprecated. Replace with UAssetEditorSubsystem.
			if (TSharedPtr<IToolkit> FoundAssetEditor = FToolkitManager::Get().FindEditorForAsset(&Metasound))
			{
				if (FEditor::EditorName == FoundAssetEditor->GetToolkitFName())
				{
					return StaticCastSharedPtr<FEditor, IToolkit>(FoundAssetEditor);
				}
			}

			return TSharedPtr<FEditor>(nullptr);
		}

		TSharedPtr<FEditor> FGraphBuilder::GetEditorForGraph(const UEdGraph& EdGraph)
		{
			const UMetasoundEditorGraph* MetasoundGraph = CastChecked<const UMetasoundEditorGraph>(&EdGraph);
			return GetEditorForMetasound(MetasoundGraph->GetMetasoundChecked());
		}

		FLinearColor FGraphBuilder::GetPinCategoryColor(const FEdGraphPinType& PinType)
		{
			const UMetasoundEditorSettings* Settings = GetDefault<UMetasoundEditorSettings>();
			check(Settings);

			if (PinType.PinCategory == PinCategoryAudio)
			{
				return Settings->AudioPinTypeColor;
			}

			if (PinType.PinCategory == PinCategoryTrigger)
			{
				return Settings->TriggerPinTypeColor;
			}

			if (PinType.PinCategory == PinCategoryBoolean)
			{
				return Settings->BooleanPinTypeColor;
			}

			if (PinType.PinCategory == PinCategoryFloat)
			{
				if (PinType.PinSubCategory == PinSubCategoryTime)
				{
					return Settings->TimePinTypeColor;
				}
				return Settings->FloatPinTypeColor;
			}

			if (PinType.PinCategory == PinCategoryInt32)
			{
				return Settings->IntPinTypeColor;
			}

			//if (PinType.PinCategory == PinCategoryInt64)
			//{
			//	return Settings->Int64PinTypeColor;
			//}

			if (PinType.PinCategory == PinCategoryString)
			{
				return Settings->StringPinTypeColor;
			}

			//if (PinType.PinCategory == PinCategoryDouble)
			//{
			//	return Settings->DoublePinTypeColor;
			//}

			if (PinType.PinCategory == PinCategoryObject)
			{
				return Settings->ObjectPinTypeColor;
			}

			return Settings->DefaultPinTypeColor;
		}

		Frontend::FInputHandle FGraphBuilder::GetInputHandleFromPin(const UEdGraphPin* InPin)
		{
			using namespace Frontend;

			if (InPin && ensure(InPin->Direction == EGPD_Input))
			{
				if (UMetasoundEditorGraphVariableNode* EdVariableNode = Cast<UMetasoundEditorGraphVariableNode>(InPin->GetOwningNode()))
				{
					// UEdGraphPins on variable nodes use the variable's name for display
					// purposes instead of the underlying vertex's name. The frontend vertices
					// of a variable node have consistent names no matter what the 
					// variable is named.
					return EdVariableNode->GetNodeHandle()->GetInputWithVertexName(VariableNames::GetInputDataName());
				}
				else if (UMetasoundEditorGraphNode* EdNode = CastChecked<UMetasoundEditorGraphNode>(InPin->GetOwningNode()))
				{
					return EdNode->GetNodeHandle()->GetInputWithVertexName(InPin->GetFName());
				}
			}

			return IInputController::GetInvalidHandle();
		}

		Frontend::FConstInputHandle FGraphBuilder::GetConstInputHandleFromPin(const UEdGraphPin* InPin)
		{
			return GetInputHandleFromPin(InPin);
		}

		Frontend::FOutputHandle FGraphBuilder::GetOutputHandleFromPin(const UEdGraphPin* InPin)
		{
			using namespace Frontend;

			if (InPin && ensure(InPin->Direction == EGPD_Output))
			{
				if (UMetasoundEditorGraphVariableNode* EdVariableNode = Cast<UMetasoundEditorGraphVariableNode>(InPin->GetOwningNode()))
				{
					// UEdGraphPins on variable nodes use the variable's name for display
					// purposes instead of the underlying vertex's name. The frontend vertices
					// of a variable node have consistent names no matter what the 
					// variable is named.
					return EdVariableNode->GetNodeHandle()->GetOutputWithVertexName(VariableNames::GetOutputDataName());
				}
				else if (UMetasoundEditorGraphNode* EdNode = CastChecked<UMetasoundEditorGraphNode>(InPin->GetOwningNode()))
				{
					return EdNode->GetNodeHandle()->GetOutputWithVertexName(InPin->GetFName());
				}
			}

			return IOutputController::GetInvalidHandle();
		}

		Frontend::FConstOutputHandle FGraphBuilder::GetConstOutputHandleFromPin(const UEdGraphPin* InPin)
		{
			return GetOutputHandleFromPin(InPin);
		}

		bool FGraphBuilder::GraphContainsErrors(const UObject& InMetaSound)
		{
			const FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);
			const UMetasoundEditorGraph* EditorGraph = CastChecked<UMetasoundEditorGraph>(MetaSoundAsset->GetGraph());

			// Get all editor nodes from editor graph (some nodes on graph may *NOT* be metasound ed nodes,
			// such as comment boxes, etc, so just get nodes of class UMetasoundEditorGraph).
			TArray<const UMetasoundEditorGraphNode*> EditorNodes;
			EditorGraph->GetNodesOfClass(EditorNodes);

			// Do not synchronize with errors present as the graph is expected to be malformed.
			for (const UMetasoundEditorGraphNode* Node : EditorNodes)
			{
				if (Node->ErrorType == EMessageSeverity::Error)
				{
					return true;
				}
			}

			return false;
		}

		bool FGraphBuilder::SynchronizeNodeLocation(UMetasoundEditorGraphNode& InNode)
		{
			bool bModified = false;

			const FMetasoundFrontendNodeStyle& Style = InNode.GetConstNodeHandle()->GetNodeStyle();

			const FVector2D* Location = Style.Display.Locations.Find(InNode.NodeGuid);
			if (!Location)
			{
				// If no specific location found, use default location if provided (zero guid
				// for example, provided by preset defaults.)
				Location = Style.Display.Locations.Find({ });
			}

			if (Location)
			{
				const int32 LocX = FMath::TruncToInt(Location->X);
				const int32 LocY = FMath::TruncToInt(Location->Y);
				const bool bXChanged = static_cast<bool>(LocX - InNode.NodePosX);
				const bool bYChanged = static_cast<bool>(LocY - InNode.NodePosY);
				if (bXChanged || bYChanged)
				{
					InNode.NodePosX = LocX;
					InNode.NodePosY = LocY;
					bModified = true;
				}
			}

			return bModified;
		}

		UMetasoundEditorGraphInputNode* FGraphBuilder::AddInputNode(UObject& InMetaSound, Frontend::FNodeHandle InNodeHandle, FVector2D InLocation, bool bInSelectNewNode)
		{
			using namespace Frontend;

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);

			UMetasoundEditorGraph* MetasoundGraph = Cast<UMetasoundEditorGraph>(MetaSoundAsset->GetGraph());
			if (!ensure(MetasoundGraph))
			{
				return nullptr;
			}

			UMetasoundEditorGraphInputNode* NewGraphNode = MetasoundGraph->CreateInputNode(InNodeHandle, bInSelectNewNode);
			if (ensure(NewGraphNode))
			{
				NewGraphNode->SetNodeLocation(InLocation);
				RebuildNodePins(*NewGraphNode);
				MetasoundGraph->SetSynchronizationRequired();
				return NewGraphNode;
			}

			return nullptr;
		}

		bool FGraphBuilder::GetPinLiteral(UEdGraphPin& InInputPin, FMetasoundFrontendLiteral& OutDefaultLiteral)
		{
			using namespace Frontend;

			IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetaSoundEditor");

			FInputHandle InputHandle = GetInputHandleFromPin(&InInputPin);
			if (!ensure(InputHandle->IsValid()))
			{
				return false;
			}

			const FString& InStringValue = InInputPin.DefaultValue;
			const FName TypeName = InputHandle->GetDataType();
			const FEditorDataType DataType = EditorModule.FindDataTypeChecked(TypeName);
			switch (DataType.RegistryInfo.PreferredLiteralType)
			{
				case ELiteralType::Boolean:
				{
					// Currently don't support triggers being initialized to boolean in-graph
					if (GetMetasoundDataTypeName<FTrigger>() != TypeName)
					{
						OutDefaultLiteral.Set(FCString::ToBool(*InStringValue));
					}
				}
				break;

				case ELiteralType::Float:
				{
					OutDefaultLiteral.Set(FCString::Atof(*InStringValue));
				}
				break;

				case ELiteralType::Integer:
				{
					OutDefaultLiteral.Set(FCString::Atoi(*InStringValue));
				}
				break;

				case ELiteralType::String:
				{
					OutDefaultLiteral.Set(InStringValue);
				}
				break;

				case ELiteralType::UObjectProxy:
				{
					bool bObjectFound = false;
					if (!InInputPin.DefaultValue.IsEmpty())
					{
						if (UClass* Class = IDataTypeRegistry::Get().GetUClassForDataType(TypeName))
						{
							FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");

							// Remove class prefix if included in default value path
							FString ObjectPath = InInputPin.DefaultValue;
							ObjectPath.RemoveFromStart(Class->GetName() + TEXT(" "));

							FARFilter Filter;
							Filter.bRecursiveClasses = false;
							Filter.ObjectPaths.Add(*ObjectPath);

							TArray<FAssetData> AssetData;
							AssetRegistryModule.Get().GetAssets(Filter, AssetData);
							if (!AssetData.IsEmpty())
							{
								if (UObject* AssetObject = AssetData.GetData()->GetAsset())
								{
									const UClass* AssetClass = AssetObject->GetClass();
									if (ensureAlways(AssetClass))
									{
										if (AssetClass->IsChildOf(Class))
										{
											Filter.ClassNames.Add(Class->GetFName());
											OutDefaultLiteral.Set(AssetObject);
											bObjectFound = true;
										}
									}
								}
							}
						}
					}
					
					if (!bObjectFound)
					{
						OutDefaultLiteral.Set(static_cast<UObject*>(nullptr));
					}
				}
				break;

				case ELiteralType::BooleanArray:
				{
					OutDefaultLiteral.Set(TArray<bool>());
				}
				break;

				case ELiteralType::FloatArray:
				{
					OutDefaultLiteral.Set(TArray<float>());
				}
				break;

				case ELiteralType::IntegerArray:
				{
					OutDefaultLiteral.Set(TArray<int32>());
				}
				break;

				case ELiteralType::NoneArray:
				{
					OutDefaultLiteral.Set(FMetasoundFrontendLiteral::FDefaultArray());
				}
				break;

				case ELiteralType::StringArray:
				{
					OutDefaultLiteral.Set(TArray<FString>());
				}
				break;

				case ELiteralType::UObjectProxyArray:
				{
					OutDefaultLiteral.Set(TArray<UObject*>());
				}
				break;

				case ELiteralType::None:
				{
					OutDefaultLiteral.Set(FMetasoundFrontendLiteral::FDefault());
				}
				break;

				case ELiteralType::Invalid:
				default:
				{
					static_assert(static_cast<int32>(ELiteralType::COUNT) == 13, "Possible missing ELiteralType case coverage.");
					ensureMsgf(false, TEXT("Failed to set input node default: Literal type not supported"));
					return false;
				}
				break;
			}

			return true;
		}

		Frontend::FNodeHandle FGraphBuilder::AddNodeHandle(UObject& InMetaSound, UMetasoundEditorGraphNode& InGraphNode)
		{
			using namespace Frontend;

			FNodeHandle NodeHandle = INodeController::GetInvalidHandle();
			if (UMetasoundEditorGraphInputNode* InputNode = Cast<UMetasoundEditorGraphInputNode>(&InGraphNode))
			{
				const TArray<UEdGraphPin*>& Pins = InGraphNode.GetAllPins();
				const UEdGraphPin* Pin = Pins.IsEmpty() ? nullptr : Pins[0];
				if (ensure(Pin) && ensure(Pin->Direction == EGPD_Output))
				{
					UMetasoundEditorGraphInput* Input = InputNode->Input;
					if (ensure(Input))
					{
						const FName PinName = Pin->GetFName();
						NodeHandle = AddInputNodeHandle(InMetaSound, Input->GetDataType(), nullptr, &PinName);
						NodeHandle->SetDescription(InGraphNode.GetTooltipText());
					}
				}
			}

			else if (UMetasoundEditorGraphOutputNode* OutputNode = Cast<UMetasoundEditorGraphOutputNode>(&InGraphNode))
			{
				const TArray<UEdGraphPin*>& Pins = InGraphNode.GetAllPins();
				const UEdGraphPin* Pin = Pins.IsEmpty() ? nullptr : Pins[0];
				if (ensure(Pin) && ensure(Pin->Direction == EGPD_Input))
				{
					UMetasoundEditorGraphOutput* Output = OutputNode->Output;
					if (ensure(Output))
					{
						const FName PinName = Pin->GetFName();
						NodeHandle = FGraphBuilder::AddOutputNodeHandle(InMetaSound, Output->GetDataType(), &PinName);
						NodeHandle->SetDescription(InGraphNode.GetTooltipText());
					}
				}
			}
			else if (UMetasoundEditorGraphVariableNode* VariableNode = Cast<UMetasoundEditorGraphVariableNode>(&InGraphNode))
			{
				NodeHandle = FGraphBuilder::AddVariableNodeHandle(InMetaSound, VariableNode->Variable->GetVariableID(), VariableNode->GetClassName().ToNodeClassName());
			}
			else if (UMetasoundEditorGraphExternalNode* ExternalNode = Cast<UMetasoundEditorGraphExternalNode>(&InGraphNode))
			{
				FMetasoundFrontendClass FrontendClass;
				bool bDidFindClassWithName = ISearchEngine::Get().FindClassWithHighestVersion(ExternalNode->ClassName.ToNodeClassName(), FrontendClass);
				if (ensure(bDidFindClassWithName))
				{
					FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
					check(MetaSoundAsset);

					Frontend::FNodeHandle NewNode = MetaSoundAsset->GetRootGraphHandle()->AddNode(FrontendClass.Metadata);
					ExternalNode->SetNodeID(NewNode->GetID());

					NodeHandle = NewNode;
				}
			}

			if (NodeHandle->IsValid())
			{
				FMetasoundFrontendNodeStyle Style = NodeHandle->GetNodeStyle();
				Style.Display.Locations.Add(InGraphNode.NodeGuid, FVector2D(InGraphNode.NodePosX, InGraphNode.NodePosY));
				NodeHandle->SetNodeStyle(Style);
			}

			return NodeHandle;
		}

		Frontend::FNodeHandle FGraphBuilder::AddInputNodeHandle(UObject& InMetaSound, const FName InTypeName, const FMetasoundFrontendLiteral* InDefaultValue, const FName* InNameBase)
		{
			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);

			const FName NewName = GenerateUniqueNameByClassType(InMetaSound, EMetasoundFrontendClassType::Input, InNameBase ? InNameBase->ToString() : TEXT("Input"));
			return MetaSoundAsset->GetRootGraphHandle()->AddInputVertex(NewName, InTypeName, InDefaultValue);
		}

		Frontend::FNodeHandle FGraphBuilder::AddOutputNodeHandle(UObject& InMetaSound, const FName InTypeName, const FName* InNameBase)
		{
			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);

			const FName NewName = GenerateUniqueNameByClassType(InMetaSound, EMetasoundFrontendClassType::Output, InNameBase ? InNameBase->ToString() : TEXT("Output"));
			return MetaSoundAsset->GetRootGraphHandle()->AddOutputVertex(NewName, InTypeName);
		}

		FName FGraphBuilder::GenerateUniqueVariableName(const Frontend::FConstGraphHandle& InFrontendGraph, const FString& InBaseName)
		{
			using namespace Frontend;

			TArray<FName> ExistingVariableNames;

			// Get all the names from the existing variables on the graph
			// and place into the ExistingVariableNames array.
			Algo::Transform(InFrontendGraph->GetVariables(), ExistingVariableNames, [](const FConstVariableHandle& Var) { return Var->GetName(); });

			return GraphBuilderPrivate::GenerateUniqueName(ExistingVariableNames, InBaseName);
		}

		Frontend::FVariableHandle FGraphBuilder::AddVariableHandle(UObject& InMetaSound, const FName& InTypeName)
		{
			using namespace Frontend;

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);

			FGraphHandle FrontendGraph = MetaSoundAsset->GetRootGraphHandle();

			FText BaseDisplayName = LOCTEXT("VariableDefaultDisplayName", "Variable");

			FString BaseName = BaseDisplayName.ToString();
			FName VariableName = GenerateUniqueVariableName(FrontendGraph, BaseName);
			FVariableHandle Variable = FrontendGraph->AddVariable(InTypeName);

			Variable->SetDisplayName(FText::GetEmpty());
			Variable->SetName(VariableName);

			return Variable;
		}

		Frontend::FNodeHandle FGraphBuilder::AddVariableNodeHandle(UObject& InMetaSound, const FGuid& InVariableID, const Metasound::FNodeClassName& InVariableNodeClassName, UMetasoundEditorGraphVariableNode* InVariableNode)
		{
			using namespace Frontend;

			FNodeHandle FrontendNode = INodeController::GetInvalidHandle();

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			
			if (ensure(MetaSoundAsset))
			{
				FMetasoundFrontendClass FrontendClass;
				bool bDidFindClassWithName = ISearchEngine::Get().FindClassWithHighestVersion(InVariableNodeClassName, FrontendClass);
				if (ensure(bDidFindClassWithName))
				{
					FGraphHandle Graph = MetaSoundAsset->GetRootGraphHandle();

					switch (FrontendClass.Metadata.GetType())
					{
						case EMetasoundFrontendClassType::VariableDeferredAccessor:
							FrontendNode = Graph->AddVariableDeferredAccessorNode(InVariableID);
							break;

						case EMetasoundFrontendClassType::VariableAccessor:
							FrontendNode = Graph->AddVariableAccessorNode(InVariableID);
							break;

						case EMetasoundFrontendClassType::VariableMutator:
							{
								FConstVariableHandle Variable = Graph->FindVariable(InVariableID);
								FConstNodeHandle ExistingMutator = Variable->FindMutatorNode();
								if (!ExistingMutator->IsValid())
								{
									FrontendNode = Graph->FindOrAddVariableMutatorNode(InVariableID);
								}
								else
								{
									UE_LOG(LogMetaSound, Error, TEXT("Cannot add node because \"%s\" already exists for variable \"%s\""), *ExistingMutator->GetDisplayName().ToString(), *Variable->GetDisplayName().ToString());
								}
							}
							break;

						default:
							{
								checkNoEntry();
							}
					}
				}
			}

			if (InVariableNode)
			{
				InVariableNode->ClassName = FrontendNode->GetClassMetadata().GetClassName();
				InVariableNode->ClassType = FrontendNode->GetClassMetadata().GetType();
				InVariableNode->SetNodeID(FrontendNode->GetID());
			}

			return FrontendNode;
		}

		UMetasoundEditorGraphNode* FGraphBuilder::AddNode(UObject& InMetaSound, Frontend::FNodeHandle InNodeHandle, FVector2D InLocation, bool bInSelectNewNode)
		{
			switch (InNodeHandle->GetClassMetadata().GetType())
			{
				case EMetasoundFrontendClassType::Input:
				{
					return CastChecked<UMetasoundEditorGraphNode>(AddInputNode(InMetaSound, InNodeHandle, InLocation, bInSelectNewNode));
				}
				break;

				case EMetasoundFrontendClassType::External:
				{
					return CastChecked<UMetasoundEditorGraphNode>(AddExternalNode(InMetaSound, InNodeHandle, InLocation, bInSelectNewNode));
				}
				break;

				case EMetasoundFrontendClassType::Output:
				{
					return CastChecked<UMetasoundEditorGraphNode>(AddOutputNode(InMetaSound, InNodeHandle, InLocation, bInSelectNewNode));
				}
				break;

				case EMetasoundFrontendClassType::VariableMutator:
				case EMetasoundFrontendClassType::VariableAccessor:
				case EMetasoundFrontendClassType::VariableDeferredAccessor:
				case EMetasoundFrontendClassType::Variable:
				{
					return CastChecked<UMetasoundEditorGraphNode>(AddVariableNode(InMetaSound, InNodeHandle, InLocation, bInSelectNewNode));
				}
				break;

				case EMetasoundFrontendClassType::Invalid:
				case EMetasoundFrontendClassType::Graph:
				
				case EMetasoundFrontendClassType::Literal: // Not yet supported in editor
				
				default:
				{
					checkNoEntry();
					static_assert(static_cast<int32>(EMetasoundFrontendClassType::Invalid) == 9, "Possible missing FMetasoundFrontendClassType case coverage");
				}
				break;
			}

			return nullptr;
		}

		bool FGraphBuilder::ConnectNodes(UEdGraphPin& InInputPin, UEdGraphPin& InOutputPin, bool bInConnectEdPins)
		{
			using namespace Frontend;

			// When true, will recursively call back into this function
			// from the schema if the editor pins are successfully connected
			if (bInConnectEdPins)
			{
				const UEdGraphSchema* Schema = InInputPin.GetSchema();
				if (ensure(Schema))
				{
					return Schema->TryCreateConnection(&InInputPin, &InOutputPin);
				}
				else
				{
					return false;
				}
			}

			FInputHandle InputHandle = GetInputHandleFromPin(&InInputPin);
			FOutputHandle OutputHandle = GetOutputHandleFromPin(&InOutputPin);
			if (!InputHandle->IsValid() || !OutputHandle->IsValid())
			{
				return false;
			}

			if (!ensure(InputHandle->Connect(*OutputHandle)))
			{
				InInputPin.BreakLinkTo(&InOutputPin);
				return false;
			}

			return true;
		}

		void FGraphBuilder::DisconnectPinVertex(UEdGraphPin& InPin, bool bAddLiteralInputs)
		{
			using namespace Editor;
			using namespace Frontend;

			TArray<FInputHandle> InputHandles;
			TArray<UEdGraphPin*> InputPins;

			UMetasoundEditorGraphNode* Node = CastChecked<UMetasoundEditorGraphNode>(InPin.GetOwningNode());

			if (InPin.Direction == EGPD_Input)
			{
				const FName PinName = InPin.GetFName();

				FNodeHandle NodeHandle = Node->GetNodeHandle();
				FInputHandle InputHandle = NodeHandle->GetInputWithVertexName(PinName);

				// Input can be invalid if renaming a vertex member
				if (InputHandle->IsValid())
				{
					InputHandles.Add(InputHandle);
					InputPins.Add(&InPin);
				}
			}
			else
			{
				check(InPin.Direction == EGPD_Output);
				for (UEdGraphPin* Pin : InPin.LinkedTo)
				{
					check(Pin);
					FNodeHandle NodeHandle = CastChecked<UMetasoundEditorGraphNode>(Pin->GetOwningNode())->GetNodeHandle();
					FInputHandle InputHandle = NodeHandle->GetInputWithVertexName(Pin->GetFName());

					// Input can be invalid if renaming a vertex member
					if (InputHandle->IsValid())
					{
						InputHandles.Add(InputHandle);
						InputPins.Add(Pin);
					}
				}
			}

			for (int32 i = 0; i < InputHandles.Num(); ++i)
			{
				FInputHandle InputHandle = InputHandles[i];
				FConstOutputHandle OutputHandle = InputHandle->GetConnectedOutput();

				InputHandle->Disconnect();

				if (bAddLiteralInputs)
				{
					FNodeHandle NodeHandle = InputHandle->GetOwningNode();
					SynchronizePinLiteral(*InputPins[i]);
				}
			}

			UObject& MetaSound = Node->GetMetasoundChecked();
			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&MetaSound);
			MetaSoundAsset->SetSynchronizationRequired();
		}

		void FGraphBuilder::InitMetaSound(UObject& InMetaSound, const FString& InAuthor)
		{
			using namespace Frontend;
			using namespace GraphBuilderPrivate;

			FMetasoundFrontendClassMetadata Metadata;

			// 1. Set default class Metadata
			Metadata.SetClassName(FMetasoundFrontendClassName(FName(), *FGuid::NewGuid().ToString(), FName()));
			Metadata.SetVersion({ 1, 0 });
			Metadata.SetType(EMetasoundFrontendClassType::Graph);
			Metadata.SetAuthor(InAuthor);

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);
			MetaSoundAsset->SetMetadata(Metadata);

			// 2. Set default doc version Metadata
			FDocumentHandle DocumentHandle = MetaSoundAsset->GetDocumentHandle();
			FMetasoundFrontendDocumentMetadata DocMetadata = DocumentHandle->GetMetadata();
			DocMetadata.Version.Number = FVersionDocument::GetMaxVersion();
			DocumentHandle->SetMetadata(DocMetadata);

			MetaSoundAsset->AddDefaultInterfaces();

			FGraphHandle GraphHandle = MetaSoundAsset->GetRootGraphHandle();
			FVector2D InputNodeLocation = FVector2D::ZeroVector;
			FVector2D ExternalNodeLocation = InputNodeLocation + DisplayStyle::NodeLayout::DefaultOffsetX;
			FVector2D OutputNodeLocation = ExternalNodeLocation + DisplayStyle::NodeLayout::DefaultOffsetX;

			TArray<FNodeHandle> NodeHandles = GraphHandle->GetNodes();
			for (FNodeHandle& NodeHandle : NodeHandles)
			{
				const EMetasoundFrontendClassType NodeType = NodeHandle->GetClassMetadata().GetType();
				FVector2D NewLocation;
				if (NodeType == EMetasoundFrontendClassType::Input)
				{
					NewLocation = InputNodeLocation;
					InputNodeLocation += DisplayStyle::NodeLayout::DefaultOffsetY;
				}
				else if (NodeType == EMetasoundFrontendClassType::Output)
				{
					NewLocation = OutputNodeLocation;
					OutputNodeLocation += DisplayStyle::NodeLayout::DefaultOffsetY;
				}
				else
				{
					NewLocation = ExternalNodeLocation;
					ExternalNodeLocation += DisplayStyle::NodeLayout::DefaultOffsetY;
				}
				FMetasoundFrontendNodeStyle Style = NodeHandle->GetNodeStyle();
				// TODO: Find consistent location for controlling node locations.
				// Currently it is split between MetasoundEditor and MetasoundFrontend modules.
				Style.Display.Locations = {{FGuid::NewGuid(), NewLocation}};
				NodeHandle->SetNodeStyle(Style);
			}
		}

		void FGraphBuilder::InitMetaSoundPreset(UObject& InMetaSoundReferenced, UObject& InMetaSoundPreset)
		{
			using namespace Frontend;
			using namespace GraphBuilderPrivate;

			FMetasoundAssetBase* PresetAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSoundPreset);
			check(PresetAsset);

			// Mark preset as auto-update and non-editable
			FGraphHandle PresetGraphHandle = PresetAsset->GetRootGraphHandle();
			FMetasoundFrontendGraphStyle Style = PresetGraphHandle->GetGraphStyle();
			Style.bIsGraphEditable = false;
			PresetGraphHandle->SetGraphStyle(Style);

			// Mark all inputs as inherited by default
			TSet<FName> InputsInheritingDefault;
			Algo::Transform(PresetGraphHandle->GetInputNodes(), InputsInheritingDefault, [](FConstNodeHandle NodeHandle)
			{
				return NodeHandle->GetNodeName();
			});
			PresetGraphHandle->SetInputsInheritingDefault(MoveTemp(InputsInheritingDefault));

			FGraphBuilder::RegisterGraphWithFrontend(InMetaSoundReferenced);

			const FMetasoundAssetBase* ReferencedAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSoundReferenced);
			check(ReferencedAsset);

			FRebuildPresetRootGraph(ReferencedAsset->GetDocumentHandle()).Transform(PresetAsset->GetDocumentHandle());
			PresetAsset->ConformObjectDataToInterfaces();
		}

		bool FGraphBuilder::DeleteNode(UEdGraphNode& InNode)
		{
			using namespace Frontend;

			if (!InNode.CanUserDeleteNode())
			{
				return false;
			}

			// If node isn't a MetasoundEditorGraphNode, just remove and return (ex. comment nodes)
			UMetasoundEditorGraphNode* Node = Cast<UMetasoundEditorGraphNode>(&InNode);
			UMetasoundEditorGraph* Graph = CastChecked<UMetasoundEditorGraph>(InNode.GetGraph());
			if (!Node)
			{
				Graph->RemoveNode(&InNode);
				return true;
			}


			// Remove connects only to pins associated with this EdGraph node
			// only (Iterate pins and not Frontend representation to preserve
			// other input/output EditorGraph reference node associations)
			Node->IteratePins([](UEdGraphPin& Pin, int32 Index)
			{
				// Only add literal inputs for output pins as adding when disconnecting
				// inputs would immediately orphan them on EditorGraph node removal below.
				const bool bAddLiteralInputs = Pin.Direction == EGPD_Output;
				FGraphBuilder::DisconnectPinVertex(Pin, bAddLiteralInputs);
			});

			FNodeHandle NodeHandle = Node->GetNodeHandle();
			Frontend::FGraphHandle GraphHandle = NodeHandle->GetOwningGraph();

			auto RemoveNodeLocation = [](FNodeHandle InNodeHandle, const FGuid& InNodeGuid)
			{
				FMetasoundFrontendNodeStyle Style = InNodeHandle->GetNodeStyle();
				Style.Display.Locations.Remove(InNodeGuid);
				InNodeHandle->SetNodeStyle(Style);
			};	

			auto RemoveNodeHandle = [] (FGraphHandle InGraphHandle, FNodeHandle InNodeHandle)
			{
				if (ensure(InGraphHandle->RemoveNode(*InNodeHandle)))
				{
					InGraphHandle->GetOwningDocument()->RemoveUnreferencedDependencies();
				}
			};

			if (GraphHandle->IsValid())
			{
				const EMetasoundFrontendClassType ClassType = NodeHandle->GetClassMetadata().GetType();
				switch (ClassType)
				{
					// NodeHandle does not get removed in these cases as EdGraph Inputs/Outputs
					// Frontend node is represented by the editor graph as a respective member
					// (not a node) on the MetasoundGraph. Therefore, just the editor position
					// data is removed.
					case EMetasoundFrontendClassType::Output:
					case EMetasoundFrontendClassType::Input:
					{
						RemoveNodeLocation(NodeHandle, InNode.NodeGuid);
					}
					break;
					
					// NodeHandle is only removed for variable accessors if the editor graph
					// no longer contains nodes representing the given accessor on the MetasoundGraph.
					// Therefore, just the editor position data is removed unless no location remains
					// on the Frontend node.
					case EMetasoundFrontendClassType::VariableAccessor:
					case EMetasoundFrontendClassType::VariableDeferredAccessor:
					{
						RemoveNodeLocation(NodeHandle, InNode.NodeGuid);
						if (NodeHandle->GetNodeStyle().Display.Locations.IsEmpty())
						{
							RemoveNodeHandle(GraphHandle, NodeHandle);
						}
					}
					break;

					case EMetasoundFrontendClassType::Graph:
					case EMetasoundFrontendClassType::Literal:
					case EMetasoundFrontendClassType::VariableMutator:
					case EMetasoundFrontendClassType::Variable:
					case EMetasoundFrontendClassType::External:
					default:
					{
						static_assert(static_cast<int32>(EMetasoundFrontendClassType::Invalid) == 9, "Possible missing MetasoundFrontendClassType switch case coverage.");
						RemoveNodeHandle(GraphHandle, NodeHandle);
					}
					break;
				}
			}

			return ensure(Graph->RemoveNode(&InNode));
		}

		void FGraphBuilder::RebuildNodePins(UMetasoundEditorGraphNode& InGraphNode)
		{
			using namespace Frontend;
		
			for (int32 i = InGraphNode.Pins.Num() - 1; i >= 0; i--)
			{
				InGraphNode.RemovePin(InGraphNode.Pins[i]);
			}

			// TODO: Make this a utility in Frontend (ClearInputLiterals())
			FNodeHandle NodeHandle = InGraphNode.GetNodeHandle();
			TArray<FInputHandle> Inputs = NodeHandle->GetInputs();
			for (FInputHandle& Input : Inputs)
			{
				NodeHandle->ClearInputLiteral(Input->GetID());
			}

			TArray<FInputHandle> InputHandles = NodeHandle->GetInputs();
			NodeHandle->GetInputStyle().SortDefaults(InputHandles);
			for (const FInputHandle& InputHandle : InputHandles)
			{
				// Only add pins of the node if the connection is user modifiable. 
				// Connections which the user cannot modify are controlled elsewhere.
				if (InputHandle->IsConnectionUserModifiable())
				{
					AddPinToNode(InGraphNode, InputHandle);
				}
			}

			TArray<FOutputHandle> OutputHandles = NodeHandle->GetOutputs();
			NodeHandle->GetOutputStyle().SortDefaults(OutputHandles);
			for (const FOutputHandle& OutputHandle : OutputHandles)
			{
				// Only add pins of the node if the connection is user modifiable. 
				// Connections which the user cannot modify are controlled elsewhere.
				if (OutputHandle->IsConnectionUserModifiable())
				{
					AddPinToNode(InGraphNode, OutputHandle);
				}
			}
		}

		void FGraphBuilder::RefreshPinMetadata(UEdGraphPin& InPin, const FMetasoundFrontendVertexMetadata& InMetadata)
		{
			// Pin ToolTips are no longer cached on pins, and are instead dynamically generated via UMetasoundEditorGraphNode::GetPinHoverText
			InPin.PinToolTip = { };
			InPin.bAdvancedView = InMetadata.bIsAdvancedDisplay;
			if (InPin.bAdvancedView)
			{
				UEdGraphNode* OwningNode = InPin.GetOwningNode();
				check(OwningNode);
				if (OwningNode->AdvancedPinDisplay == ENodeAdvancedPins::NoPins)
				{
					OwningNode->AdvancedPinDisplay = ENodeAdvancedPins::Hidden;
				}
			}
		}

		void FGraphBuilder::RegisterGraphWithFrontend(UObject& InMetaSound, bool bInForceViewSynchronization)
		{
			using namespace Frontend;

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);

			TArray<FMetasoundAssetBase*> EditedReferencingMetaSounds;
			if (GEditor)
			{
				TArray<UObject*> EditedAssets = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->GetAllEditedAssets();
				for (UObject* Asset : EditedAssets)
				{
					if (Asset != &InMetaSound)
					{
						if (FMetasoundAssetBase* EditedMetaSound = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(Asset))
						{
							EditedMetaSound->RebuildReferencedAssetClassKeys();
							if (EditedMetaSound->IsReferencedAsset(*MetaSoundAsset))
							{
								EditedReferencingMetaSounds.Add(EditedMetaSound);
							}
						}
					}
				}
			}

			FMetaSoundAssetRegistrationOptions RegOptions;
			RegOptions.bForceReregister = true;
			RegOptions.bForceViewSynchronization = bInForceViewSynchronization;
			// if EditedReferencingMetaSounds is empty, then no MetaSounds are open
			// that reference this MetaSound, so just register this asset. Otherwise,
			// this graph will recursively get updated when the open referencing graphs
			// are registered recursively via bRegisterDependencies flag.
			if (EditedReferencingMetaSounds.IsEmpty())
			{
				MetaSoundAsset->RegisterGraphWithFrontend(RegOptions);
			}
			else
			{
				for (FMetasoundAssetBase* MetaSound : EditedReferencingMetaSounds)
				{
					MetaSound->RegisterGraphWithFrontend(RegOptions);
				}
			}
		}

		void FGraphBuilder::UnregisterGraphWithFrontend(UObject& InMetaSound)
		{
			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			if (!ensure(MetaSoundAsset))
			{
				return;
			}

			if (GEditor)
			{
				TArray<UObject*> EditedAssets = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->GetAllEditedAssets();
				for (UObject* Asset : EditedAssets)
				{
					if (Asset != &InMetaSound)
					{
						if (FMetasoundAssetBase* EditedMetaSound = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(Asset))
						{
							EditedMetaSound->RebuildReferencedAssetClassKeys();
							if (EditedMetaSound->IsReferencedAsset(*MetaSoundAsset))
							{
								EditedMetaSound->SetSynchronizationRequired();
							}
						}
					}
				}
			}

			MetaSoundAsset->UnregisterGraphWithFrontend();
		}

		bool FGraphBuilder::IsMatchingInputHandleAndPin(const Frontend::FConstInputHandle& InInputHandle, const UEdGraphPin& InEditorPin)
		{
			if (InEditorPin.Direction != EGPD_Input)
			{
				return false;
			}

			Frontend::FInputHandle PinInputHandle = GetInputHandleFromPin(&InEditorPin);
			if (PinInputHandle->GetID() == InInputHandle->GetID())
			{
				return true;
			}

			return false;
		}

		bool FGraphBuilder::IsMatchingOutputHandleAndPin(const Frontend::FConstOutputHandle& InOutputHandle, const UEdGraphPin& InEditorPin)
		{
			if (InEditorPin.Direction != EGPD_Output)
			{
				return false;
			}

			Frontend::FOutputHandle PinOutputHandle = GetOutputHandleFromPin(&InEditorPin);
			if (PinOutputHandle->GetID() == InOutputHandle->GetID())
			{
				return true;
			}

			return false;
		}

		void FGraphBuilder::DepthFirstTraversal(UEdGraphNode* InInitialNode, FDepthFirstVisitFunction InVisitFunction)
		{
			// Non recursive depth first traversal.
			TArray<UEdGraphNode*> Stack({InInitialNode});
			TSet<UEdGraphNode*> Visited;

			while (Stack.Num() > 0)
			{
				UEdGraphNode* CurrentNode = Stack.Pop();
				if (Visited.Contains(CurrentNode))
				{
					// Do not revisit a node that has already been visited. 
					continue;
				}

				TArray<UEdGraphNode*> Children = InVisitFunction(CurrentNode).Array();
				Stack.Append(Children);

				Visited.Add(CurrentNode);
			}
		}

		UEdGraphPin* FGraphBuilder::AddPinToNode(UMetasoundEditorGraphNode& InEditorNode, Frontend::FConstInputHandle InInputHandle)
		{
			using namespace Frontend;

			FEdGraphPinType PinType;
			FName DataTypeName = InInputHandle->GetDataType();

			IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetaSoundEditor");
			if (const FEditorDataType* EditorDataType = EditorModule.FindDataType(DataTypeName))
			{
				PinType = EditorDataType->PinType;
			}

			FName PinName = FGraphBuilder::GetPinName(*InInputHandle);
			UEdGraphPin* NewPin = InEditorNode.CreatePin(EGPD_Input, PinType, PinName);
			if (ensure(NewPin))
			{
				RefreshPinMetadata(*NewPin, InInputHandle->GetMetadata());
				SynchronizePinLiteral(*NewPin);
			}

			return NewPin;
		}

		UEdGraphPin* FGraphBuilder::AddPinToNode(UMetasoundEditorGraphNode& InEditorNode, Frontend::FConstOutputHandle InOutputHandle)
		{
			FEdGraphPinType PinType;
			FName DataTypeName = InOutputHandle->GetDataType();

			IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetaSoundEditor");
			if (const FEditorDataType* EditorDataType = EditorModule.FindDataType(DataTypeName))
			{
				PinType = EditorDataType->PinType;
			}

			FName PinName = FGraphBuilder::GetPinName(*InOutputHandle);
			UEdGraphPin* NewPin = InEditorNode.CreatePin(EGPD_Output, PinType, PinName);
			if (ensure(NewPin))
			{
				RefreshPinMetadata(*NewPin, InOutputHandle->GetMetadata());
			}

			return NewPin;
		}

		bool FGraphBuilder::SynchronizePinType(const IMetasoundEditorModule& InEditorModule, UEdGraphPin& InPin, const FName InDataType)
		{
			FEdGraphPinType PinType;
			if (const FEditorDataType* EditorDataType = InEditorModule.FindDataType(InDataType))
			{
				PinType = EditorDataType->PinType;
			}

			if (InPin.PinType != PinType)
			{
				if (const UMetasoundEditorGraphNode* Node = Cast<UMetasoundEditorGraphNode>(InPin.GetOwningNodeUnchecked()))
				{
					const FString NodeName = Node->GetDisplayName().ToString();
					UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Pin '%s' on Node '%s': Type converted to '%s'"), *NodeName, *InPin.GetName(), *InDataType.ToString());
				}
				InPin.PinType = PinType;
				return true;
			}

			return false;
		}

		bool FGraphBuilder::SynchronizeConnections(UObject& InMetaSound)
		{
			using namespace Frontend;

			bool bIsGraphDirty = false;

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);

			FGraphHandle GraphHandle = MetaSoundAsset->GetRootGraphHandle();

			UMetasoundEditorGraph* EditorGraph = CastChecked<UMetasoundEditorGraph>(MetaSoundAsset->GetGraph());

			TArray<UMetasoundEditorGraphNode*> EditorNodes;
			EditorGraph->GetNodesOfClass(EditorNodes);

			TMap<FGuid, TArray<UMetasoundEditorGraphNode*>> EditorNodesByFrontendID;
			for (UMetasoundEditorGraphNode* EditorNode : EditorNodes)
			{
				EditorNodesByFrontendID.FindOrAdd(EditorNode->GetNodeID()).Add(EditorNode);
			}

			// Iterate through all nodes in metasound editor graph and synchronize connections.
			for (UMetasoundEditorGraphNode* EditorNode : EditorNodes)
			{
				bool bIsNodeDirty = false;

				FConstNodeHandle Node = EditorNode->GetNodeHandle();

				TArray<UEdGraphPin*> Pins = EditorNode->GetAllPins();
				TArray<FConstInputHandle> NodeInputs = Node->GetConstInputs();

				// Ignore connections which are not handled by the editor.
				NodeInputs.RemoveAll([](const FConstInputHandle& FrontendInput) { return !FrontendInput->IsConnectionUserModifiable(); });

				for (FConstInputHandle& NodeInput : NodeInputs)
				{
					auto IsMatchingInputPin = [&](const UEdGraphPin* Pin) -> bool
					{
						return IsMatchingInputHandleAndPin(NodeInput, *Pin);
					};

					UEdGraphPin* MatchingPin = nullptr;
					if (UEdGraphPin** DoublePointer = Pins.FindByPredicate(IsMatchingInputPin))
					{
						MatchingPin = *DoublePointer;
					}

					if (!ensure(MatchingPin))
					{
						continue;
					}

					// Remove pin so it isn't used twice.
					Pins.Remove(MatchingPin);

					FConstOutputHandle OutputHandle = NodeInput->GetConnectedOutput();
					if (OutputHandle->IsValid())
					{
						// Both input and output handles be user modifiable for a
						// connection to be controlled by the editor.
						check(OutputHandle->IsConnectionUserModifiable());

						bool bAddLink = false;

						if (MatchingPin->LinkedTo.IsEmpty())
						{
							// No link currently exists. Add the appropriate link.
							bAddLink = true;
						}
						else if (!IsMatchingOutputHandleAndPin(OutputHandle, *MatchingPin->LinkedTo[0]))
						{
							// The wrong link exists.
							MatchingPin->BreakAllPinLinks();
							bAddLink = true;
						}

						if (bAddLink)
						{
							const FGuid NodeID = OutputHandle->GetOwningNodeID();
							TArray<UMetasoundEditorGraphNode*>* OutputEditorNode = EditorNodesByFrontendID.Find(NodeID);
							if (ensure(OutputEditorNode))
							{
								if (ensure(!OutputEditorNode->IsEmpty()))
								{
									UEdGraphPin* OutputPin = (*OutputEditorNode)[0]->FindPinChecked(OutputHandle->GetName(), EEdGraphPinDirection::EGPD_Output);
									const FText& OwningNodeName = EditorNode->GetDisplayName();

									UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Node '%s' Connection: Linking Pin '%s' to '%s'"), *OwningNodeName.ToString(), *MatchingPin->GetName(), *OutputPin->GetName());
									MatchingPin->MakeLinkTo(OutputPin);
									bIsNodeDirty = true;
								}
							}
						}
					}
					else
					{
						// No link should exist.
						if (!MatchingPin->LinkedTo.IsEmpty())
						{
							MatchingPin->BreakAllPinLinks();
							const FText OwningNodeName = EditorNode->GetDisplayName();
							const FText InputName = FGraphBuilder::GetDisplayName(*NodeInput);
							UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Node '%s' Connection: Breaking all pin links to '%s'"), *OwningNodeName.ToString(), *InputName.ToString());
							bIsNodeDirty = true;
						}
					}

					SynchronizePinLiteral(*MatchingPin);
				}

				bIsGraphDirty |= bIsNodeDirty;
			}

			return bIsGraphDirty;
		}

		bool FGraphBuilder::SynchronizeGraph(UObject& InMetaSound, bool bForceRefreshNodes)
		{
			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);

			// If no graph is set, MetaSound has been created outside of asset factory, so initialize it here.
			// TODO: Move factory initialization and this code to single builder function (in header so cannot move
			// until 5.1+).
			if(!MetaSoundAsset->GetGraph())
			{
				FString Author = UKismetSystemLibrary::GetPlatformUserName();
				if (const UMetasoundEditorSettings* EditorSettings = GetDefault<UMetasoundEditorSettings>())
				{
					if (!EditorSettings->DefaultAuthor.IsEmpty())
					{
						Author = EditorSettings->DefaultAuthor;
					}
				}

				FGraphBuilder::InitMetaSound(InMetaSound, Author);

				// Initial graph generation is not something to be managed by the transaction
				// stack, so don't track dirty state until after initial setup if necessary.
				UMetasoundEditorGraph* Graph = NewObject<UMetasoundEditorGraph>(&InMetaSound, FName(), RF_Transactional);
				Graph->Schema = UMetasoundEditorGraphSchema::StaticClass();
				MetaSoundAsset->SetGraph(Graph);
			}

			bool bEditorGraphModified = SynchronizeGraphMembers(InMetaSound);
			bEditorGraphModified |= SynchronizeNodeMembers(InMetaSound);
			bEditorGraphModified |= SynchronizeNodes(InMetaSound);
			bEditorGraphModified |= SynchronizeConnections(InMetaSound);

			if (bEditorGraphModified)
			{
				InMetaSound.MarkPackageDirty();
			}

			bForceRefreshNodes |= bEditorGraphModified;
			const bool bIsValid = FGraphBuilder::ValidateGraph(InMetaSound, bForceRefreshNodes);

			MetaSoundAsset->ResetSynchronizationState();

			return bIsValid;
		}

		bool FGraphBuilder::SynchronizeNodeMembers(UObject& InMetaSound)
		{
			using namespace Frontend;

			bool bEditorGraphModified = false;

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);
			FGraphHandle GraphHandle = MetaSoundAsset->GetRootGraphHandle();
			UMetasoundEditorGraph* EditorGraph = CastChecked<UMetasoundEditorGraph>(MetaSoundAsset->GetGraph());

			TArray<UMetasoundEditorGraphInputNode*> InputNodes;
			EditorGraph->GetNodesOfClassEx<UMetasoundEditorGraphInputNode>(InputNodes);
			for (UMetasoundEditorGraphInputNode* Node : InputNodes)
			{
				check(Node);
				FConstNodeHandle NodeHandle = Node->GetConstNodeHandle();
				if (!NodeHandle->IsValid())
				{
					for (UEdGraphPin* Pin : Node->Pins)
					{
						check(Pin);

						FConstClassInputAccessPtr ClassInputPtr = GraphHandle->FindClassInputWithName(Pin->PinName);
						if (const FMetasoundFrontendClassInput* Input = ClassInputPtr.Get())
						{
							const FGuid& InitialID = Node->GetNodeID();
							if (Node->GetNodeHandle()->GetID() != Input->NodeID)
							{
								Node->SetNodeID(Input->NodeID);

								// Requery handle as the id has been fixed up
								NodeHandle = Node->GetConstNodeHandle();
								FText InputDisplayName = Node->GetDisplayName();
								UE_LOG(LogMetasoundEditor, Verbose, TEXT("Editor Input Node '%s' interface versioned"), *InputDisplayName.ToString());

								bEditorGraphModified = true;
							}
						}
					}
				}
			}

			TArray<UMetasoundEditorGraphOutputNode*> OutputNodes;
			EditorGraph->GetNodesOfClassEx<UMetasoundEditorGraphOutputNode>(OutputNodes);
			for (UMetasoundEditorGraphOutputNode* Node : OutputNodes)
			{
				FConstNodeHandle NodeHandle = Node->GetConstNodeHandle();
				if (!NodeHandle->IsValid())
				{
					for (UEdGraphPin* Pin : Node->Pins)
					{
						check(Pin);
						FConstClassOutputAccessPtr ClassOutputPtr = GraphHandle->FindClassOutputWithName(Pin->PinName);
						if (const FMetasoundFrontendClassOutput* Output = ClassOutputPtr.Get())
						{
							const FGuid& InitialID = Node->GetNodeID();
							if (Node->GetNodeHandle()->GetID() != Output->NodeID)
							{
								Node->SetNodeID(Output->NodeID);

								// Requery handle as the id has been fixed up
								NodeHandle = Node->GetConstNodeHandle();
								FText OutputDisplayName = Node->GetDisplayName();
								UE_LOG(LogMetasoundEditor, Verbose, TEXT("Editor Output Node '%s' interface versioned"), *OutputDisplayName.ToString());

								bEditorGraphModified = true;
							}
						}
					}
				}
			}

			return bEditorGraphModified;
		}

		bool FGraphBuilder::SynchronizeNodes(UObject& InMetaSound)
		{
			using namespace Frontend;

			bool bEditorGraphModified = false;

			// Get all external nodes from Frontend graph.  Input and output references will only be added/synchronized
			// if required when synchronizing connections (as they are not required to inhabit editor graph).
			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);
			FGraphHandle GraphHandle = MetaSoundAsset->GetRootGraphHandle();
			TArray<FNodeHandle> FrontendNodes = GraphHandle->GetNodes();
			UMetasoundEditorGraph* EditorGraph = CastChecked<UMetasoundEditorGraph>(MetaSoundAsset->GetGraph());
			TArray<UMetasoundEditorGraphNode*> EditorNodes;
			EditorGraph->GetNodesOfClass(EditorNodes);

			TMap<FGuid, UMetasoundEditorGraphNode*> EditorNodesByEdNodeGuid;
			for (UMetasoundEditorGraphNode* Node : EditorNodes)
			{
				EditorNodesByEdNodeGuid.Add(Node->NodeGuid, Node);
			}

			// Find existing array of editor nodes associated with Frontend node
			struct FAssociatedNodes
			{
				TArray<UMetasoundEditorGraphNode*> EditorNodes;
				FNodeHandle Node = Metasound::Frontend::INodeController::GetInvalidHandle();
			};
			TMap<FGuid, FAssociatedNodes> AssociatedNodes;

			// Reverse iterate so paired nodes can safely be removed from the array.
			for (int32 i = FrontendNodes.Num() - 1; i >= 0; i--)
			{
				FNodeHandle Node = FrontendNodes[i];
				bool bFoundEditorNode = false;
				for (int32 j = EditorNodes.Num() - 1; j >= 0; --j)
				{
					UMetasoundEditorGraphNode* EditorNode = EditorNodes[j];
					if (EditorNode->GetNodeID() == Node->GetID())
					{
						bFoundEditorNode = true;
						FAssociatedNodes& AssociatedNodeData = AssociatedNodes.FindOrAdd(Node->GetID());
						if (AssociatedNodeData.Node->IsValid())
						{
							ensure(AssociatedNodeData.Node == Node);
						}
						else
						{
							AssociatedNodeData.Node = Node;
						}

						bEditorGraphModified |= SynchronizeNodeLocation(*EditorNode);
						AssociatedNodeData.EditorNodes.Add(EditorNode);
						EditorNodes.RemoveAtSwap(j, 1, false /* bAllowShrinking */);
					}
				}

				if (bFoundEditorNode)
				{
					FrontendNodes.RemoveAtSwap(i, 1, false /* bAllowShrinking */);
				}
			}

			// FrontendNodes now contains nodes which need to be added to the editor graph.
			// EditorNodes now contains nodes that need to be removed from the editor graph.
			// AssociatedNodes contains pairs which we have to check have synchronized pins

			// Add and remove nodes first in order to make sure correct editor nodes
			// exist before attempting to synchronize connections.
			for (UMetasoundEditorGraphNode* EditorNode : EditorNodes)
			{
				bEditorGraphModified |= EditorGraph->RemoveNode(EditorNode);
			}

			// Add missing editor nodes marked as visible.
			for (FNodeHandle Node : FrontendNodes)
			{
				const FMetasoundFrontendNodeStyle& CurrentStyle = Node->GetNodeStyle();
				if (CurrentStyle.Display.Locations.IsEmpty())
				{
					continue;
				}

				FMetasoundFrontendNodeStyle NewStyle = CurrentStyle;
				bEditorGraphModified = true;

				TArray<UMetasoundEditorGraphNode*> AddedNodes;
				for (const TPair<FGuid, FVector2D>& Location : NewStyle.Display.Locations)
				{
					UMetasoundEditorGraphNode* NewNode = AddNode(InMetaSound, Node, Location.Value, false /* bInSelectNewNode */);
					if (ensure(NewNode))
					{
						FAssociatedNodes& AssociatedNodeData = AssociatedNodes.FindOrAdd(Node->GetID());
						if (AssociatedNodeData.Node->IsValid())
						{
							ensure(AssociatedNodeData.Node == Node);
						}
						else
						{
							AssociatedNodeData.Node = Node;
						}

						AddedNodes.Add(NewNode);
						AssociatedNodeData.EditorNodes.Add(NewNode);
					}
				}

				NewStyle.Display.Locations.Reset();
				for (UMetasoundEditorGraphNode* EditorNode : AddedNodes)
				{
					NewStyle.Display.Locations.Add(EditorNode->NodeGuid, FVector2D(EditorNode->NodePosX, EditorNode->NodePosY));
				}
				Node->SetNodeStyle(NewStyle);
			}

			// Synchronize pins on node associations.
			for (const TPair<FGuid, FAssociatedNodes>& IdNodePair : AssociatedNodes)
			{
				for (UMetasoundEditorGraphNode* EditorNode : IdNodePair.Value.EditorNodes)
				{
					bEditorGraphModified |= SynchronizeNodePins(*EditorNode, IdNodePair.Value.Node);
				}
			}

			return bEditorGraphModified;
		}

		bool FGraphBuilder::SynchronizeNodePins(UMetasoundEditorGraphNode& InEditorNode, Frontend::FConstNodeHandle InNode, bool bRemoveUnusedPins, bool bLogChanges)
		{
			bool bIsNodeDirty = false;

			IMetasoundEditorModule& EditorModule = FModuleManager::GetModuleChecked<IMetasoundEditorModule>("MetaSoundEditor");

			TArray<Frontend::FConstInputHandle> InputHandles;
			TArray<Frontend::FConstOutputHandle> OutputHandles;
			auto GetUserModifiableHandles = [InNode](TArray<Frontend::FConstInputHandle>& InHandles, TArray<Frontend::FConstOutputHandle>& OutHandles)
			{
				InHandles = InNode->GetConstInputs();
				OutHandles = InNode->GetConstOutputs();

				// Remove input and output handles which are not user modifiable
				InHandles.RemoveAll([](const Frontend::FConstInputHandle& FrontendInput) { return !FrontendInput->IsConnectionUserModifiable(); });
				OutHandles.RemoveAll([](const Frontend::FConstOutputHandle& FrontendOutput) { return !FrontendOutput->IsConnectionUserModifiable(); });
			};
			GetUserModifiableHandles(InputHandles, OutputHandles);

			// Filter out pins which are not paired.
			TArray<UEdGraphPin*> EditorPins = InEditorNode.Pins;
			for (int32 i = EditorPins.Num() - 1; i >= 0; i--)
			{
				UEdGraphPin* Pin = EditorPins[i];

				auto IsMatchingInputHandle = [&](const Frontend::FConstInputHandle& InputHandle) -> bool
				{
					return IsMatchingInputHandleAndPin(InputHandle, *Pin);
				};

				auto IsMatchingOutputHandle = [&](const Frontend::FConstOutputHandle& OutputHandle) -> bool
				{
					return IsMatchingOutputHandleAndPin(OutputHandle, *Pin);
				};

				switch (Pin->Direction)
				{
					case EEdGraphPinDirection::EGPD_Input:
					{
						int32 MatchingInputIndex = InputHandles.FindLastByPredicate(IsMatchingInputHandle);
						if (INDEX_NONE != MatchingInputIndex)
						{
							bIsNodeDirty |= SynchronizePinType(EditorModule, *EditorPins[i], InputHandles[MatchingInputIndex]->GetDataType());
							InputHandles.RemoveAtSwap(MatchingInputIndex);
							EditorPins.RemoveAtSwap(i);
						}
					}
					break;

					case EEdGraphPinDirection::EGPD_Output:
					{
						int32 MatchingOutputIndex = OutputHandles.FindLastByPredicate(IsMatchingOutputHandle);
						if (INDEX_NONE != MatchingOutputIndex)
						{
							bIsNodeDirty |= SynchronizePinType(EditorModule, *EditorPins[i], OutputHandles[MatchingOutputIndex]->GetDataType());
							OutputHandles.RemoveAtSwap(MatchingOutputIndex);
							EditorPins.RemoveAtSwap(i);
						}
					}
					break;
				}
			}

			// Remove any unused editor pins.
			if (bRemoveUnusedPins)
			{
				bIsNodeDirty |= !EditorPins.IsEmpty();
				for (UEdGraphPin* Pin : EditorPins)
				{
					if (bLogChanges)
					{
						constexpr bool bIncludeNamespace = true;
						const FText NodeDisplayName = FGraphBuilder::GetDisplayName(*InNode, bIncludeNamespace);
						UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Node '%s' Pins: Removing Excess Editor Pin '%s'"), *NodeDisplayName.ToString(), *Pin->GetName());
					}
					InEditorNode.RemovePin(Pin);
				}
			}


			if (!InputHandles.IsEmpty())
			{
				bIsNodeDirty = true;
				for (Frontend::FConstInputHandle& InputHandle : InputHandles)
				{
					if (bLogChanges)
					{
						constexpr bool bIncludeNamespace = true;
						const FText NodeDisplayName = FGraphBuilder::GetDisplayName(*InNode, bIncludeNamespace);
						const FText InputDisplayName = FGraphBuilder::GetDisplayName(*InputHandle);
						UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Node '%s' Pins: Adding missing Editor Input Pin '%s'"), *NodeDisplayName.ToString(), *InputDisplayName.ToString());
					}
					AddPinToNode(InEditorNode, InputHandle);
				}
			}

			if (!OutputHandles.IsEmpty())
			{
				bIsNodeDirty = true;
				for (Frontend::FConstOutputHandle& OutputHandle : OutputHandles)
				{
					if (bLogChanges)
					{
						constexpr bool bIncludeNamespace = true;
						const FText NodeDisplayName = FGraphBuilder::GetDisplayName(*InNode, bIncludeNamespace);
						const FText OutputDisplayName = FGraphBuilder::GetDisplayName(*OutputHandle);
						UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Node '%s' Pins: Adding missing Editor Output Pin '%s'"), *NodeDisplayName.ToString(), *OutputDisplayName.ToString());
					}
					AddPinToNode(InEditorNode, OutputHandle);
				}
			}

			// Order pins
			GetUserModifiableHandles(InputHandles, OutputHandles);
			InNode->GetInputStyle().SortDefaults(InputHandles);
			InNode->GetOutputStyle().SortDefaults(OutputHandles);

			auto SwapAndDirty = [&](int32 IndexA, int32 IndexB)
			{
				const bool bRequiresSwap = IndexA != IndexB;
				if (bRequiresSwap)
				{
					InEditorNode.Pins.Swap(IndexA, IndexB);
					bIsNodeDirty |= bRequiresSwap;
				}
			};

			for (int32 i = InEditorNode.Pins.Num() - 1; i >= 0; --i)
			{
				UEdGraphPin* Pin = InEditorNode.Pins[i];
				if (Pin->Direction == EGPD_Input)
				{
					if (!InputHandles.IsEmpty())
					{
						constexpr bool bAllowShrinking = false;
						Frontend::FConstInputHandle InputHandle = InputHandles.Pop(bAllowShrinking);
						for (int32 j = i; j >= 0; --j)
						{
							if (IsMatchingInputHandleAndPin(InputHandle, *InEditorNode.Pins[j]))
							{
								SwapAndDirty(i, j);
								break;
							}
						}
					}
				}
				else /* Pin->Direction == EGPD_Output */
				{
					if (!OutputHandles.IsEmpty())
					{
						constexpr bool bAllowShrinking = false;
						Frontend::FConstOutputHandle OutputHandle = OutputHandles.Pop(bAllowShrinking);
						for (int32 j = i; j >= 0; --j)
						{
							if (IsMatchingOutputHandleAndPin(OutputHandle, *InEditorNode.Pins[j]))
							{
								SwapAndDirty(i, j);
								break;
							}
						}
					}
				}
			}

			return bIsNodeDirty;
		}

		bool FGraphBuilder::SynchronizePinLiteral(UEdGraphPin& InPin)
		{
			using namespace Frontend;

			if (!ensure(InPin.Direction == EGPD_Input))
			{
				return false;
			}

			const FString OldValue = InPin.DefaultValue;

			FInputHandle InputHandle = GetInputHandleFromPin(&InPin);
			if (const FMetasoundFrontendLiteral* NodeDefaultLiteral = InputHandle->GetLiteral())
			{
				InPin.DefaultValue = NodeDefaultLiteral->ToString();
				return OldValue != InPin.DefaultValue;
			}

			if (const FMetasoundFrontendLiteral* ClassDefaultLiteral = InputHandle->GetClassDefaultLiteral())
			{
				InPin.DefaultValue = ClassDefaultLiteral->ToString();
				return OldValue != InPin.DefaultValue;
			}

			FMetasoundFrontendLiteral DefaultLiteral;
			DefaultLiteral.SetFromLiteral(IDataTypeRegistry::Get().CreateDefaultLiteral(InputHandle->GetDataType()));

			InPin.DefaultValue = DefaultLiteral.ToString();
			return OldValue != InPin.DefaultValue;
		}

		bool FGraphBuilder::SynchronizeGraphMembers(UObject& InMetaSound)
		{
			using namespace Frontend;

			bool bEditorGraphModified = false;

			FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(&InMetaSound);
			check(MetaSoundAsset);
			UMetasoundEditorGraph* Graph = CastChecked<UMetasoundEditorGraph>(MetaSoundAsset->GetGraph());
			FGraphHandle GraphHandle = MetaSoundAsset->GetRootGraphHandle();

			TSet<UMetasoundEditorGraphInput*> Inputs;
			TSet<UMetasoundEditorGraphOutput*> Outputs;

			// Collect all editor graph inputs with corresponding frontend inputs. 
			GraphHandle->IterateNodes([&](FNodeHandle NodeHandle)
			{
				if (UMetasoundEditorGraphInput* Input = Graph->FindInput(NodeHandle->GetID()))
				{
					Inputs.Add(Input);
					return;
				}

				// Add an editor input if none exist for a frontend input.
				Inputs.Add(Graph->FindOrAddInput(NodeHandle));
				constexpr bool bIncludeNamespace = true;
				FText NodeDisplayName = FGraphBuilder::GetDisplayName(*NodeHandle, bIncludeNamespace);
				UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Inputs: Added missing input '%s'."), *NodeDisplayName.ToString());
				bEditorGraphModified = true;
			}, EMetasoundFrontendClassType::Input);

			// Collect all editor graph outputs with corresponding frontend outputs. 
			GraphHandle->IterateNodes([&](FNodeHandle NodeHandle)
			{
				if (UMetasoundEditorGraphOutput* Output = Graph->FindOutput(NodeHandle->GetID()))
				{
					Outputs.Add(Output);
					return;
				}

				// Add an editor output if none exist for a frontend output.
				Outputs.Add(Graph->FindOrAddOutput(NodeHandle));
				constexpr bool bIncludeNamespace = true;
				FText NodeDisplayName = FGraphBuilder::GetDisplayName(*NodeHandle, bIncludeNamespace);
				UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Outputs: Added missing output '%s'."), *NodeDisplayName.ToString());
				bEditorGraphModified = true;
			}, EMetasoundFrontendClassType::Output);

			// Collect editor inputs and outputs to remove which have no corresponding frontend input or output.
			TArray<UMetasoundEditorGraphMember*> ToRemove;
			Graph->IterateInputs([&](UMetasoundEditorGraphInput& Input)
			{
				if (!Inputs.Contains(&Input))
				{
					UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Inputs: Removing stale input '%s'."), *Input.GetName());
					ToRemove.Add(&Input);
				}
			});
			Graph->IterateOutputs([&](UMetasoundEditorGraphOutput& Output)
			{
				if (!Outputs.Contains(&Output))
				{
					UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Outputs: Removing stale output '%s'."), *Output.GetName());
					ToRemove.Add(&Output);
				}
			});

			// Remove stale inputs and outputs.
			bEditorGraphModified |= !ToRemove.IsEmpty();
			for (UMetasoundEditorGraphMember* GraphMember: ToRemove)
			{
				Graph->RemoveMember(*GraphMember);
			}

			UMetasoundEditorGraphMember* Member = nullptr;

			auto SynchronizeMemberDataType = [&](UMetasoundEditorGraphVertex& InVertex)
			{
				FConstNodeHandle NodeHandle = InVertex.GetConstNodeHandle();
				TArray<FConstInputHandle> InputHandles = NodeHandle->GetConstInputs();
				if (ensure(InputHandles.Num() == 1))
				{
					FConstInputHandle InputHandle = InputHandles.Last();
					const FName NewDataType = InputHandle->GetDataType();
					if (InVertex.GetDataType() != NewDataType)
					{
						constexpr bool bIncludeNamespace = true;
						FText NodeDisplayName = FGraphBuilder::GetDisplayName(*NodeHandle, bIncludeNamespace);
						UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing Member '%s': Updating DataType to '%s'."), *NodeDisplayName.ToString(), *NewDataType.ToString());

						FMetasoundFrontendLiteral DefaultLiteral;
						DefaultLiteral.SetFromLiteral(IDataTypeRegistry::Get().CreateDefaultLiteral(NewDataType));
						if (const FMetasoundFrontendLiteral* InputLiteral = InputHandle->GetLiteral())
						{
							DefaultLiteral = *InputLiteral;
						}

						InVertex.ClassName = NodeHandle->GetClassMetadata().GetClassName();

						constexpr bool bPostTransaction = false;
						InVertex.SetDataType(NewDataType, bPostTransaction);

						if (DefaultLiteral.IsValid())
						{
							InVertex.GetLiteral()->SetFromLiteral(DefaultLiteral);
						}
					}
				}
			};

			// Synchronize data types & default values for input nodes.
			GraphHandle->IterateNodes([&](FNodeHandle NodeHandle)
			{
				if (UMetasoundEditorGraphInput* Input = Graph->FindInput(NodeHandle->GetID()))
				{
					SynchronizeMemberDataType(*Input);

					if (UMetasoundEditorGraphMemberDefaultLiteral* Literal = Input->GetLiteral())
					{
						const FName NodeName = NodeHandle->GetNodeName();
						const FGuid VertexID = GraphHandle->GetVertexIDForInputVertex(NodeName);
						FMetasoundFrontendLiteral DefaultLiteral = GraphHandle->GetDefaultInput(VertexID);
						if (!DefaultLiteral.IsEqual(Literal->GetDefault()))
						{
							if (DefaultLiteral.GetType() != EMetasoundFrontendLiteralType::None)
							{
								UE_LOG(LogMetasoundEditor, Verbose, TEXT("Synchronizing default value to '%s' for input '%s'"), *DefaultLiteral.ToString(), *NodeName.ToString());
								Literal->SetFromLiteral(DefaultLiteral);
								bEditorGraphModified = true;
							}
						}
					}
				}
			}, EMetasoundFrontendClassType::Input);

			// Synchronize data types of output nodes.
			GraphHandle->IterateNodes([&](FNodeHandle NodeHandle)
			{
				if (UMetasoundEditorGraphOutput* Output = Graph->FindOutput(NodeHandle->GetID()))
				{
					SynchronizeMemberDataType(*Output);
				}
			}, EMetasoundFrontendClassType::Output);

			// Remove empty entries
			bEditorGraphModified |= Graph->Inputs.RemoveAllSwap([](const TObjectPtr<UMetasoundEditorGraphInput>& Input) { return !Input; }) > 0;
			bEditorGraphModified |= Graph->Outputs.RemoveAllSwap([](const TObjectPtr<UMetasoundEditorGraphOutput>& Output) { return !Output; }) > 0;
			bEditorGraphModified |= Graph->Variables.RemoveAllSwap([](const TObjectPtr<UMetasoundEditorGraphVariable>& Variable) { return !Variable; }) > 0;

			return bEditorGraphModified;
		}
	} // namespace Editor
} // namespace Metasound
#undef LOCTEXT_NAMESPACE
