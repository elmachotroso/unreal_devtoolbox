// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetasoundEditorModule.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistryModule.h"
#include "AssetTypeActions_Base.h"
#include "Brushes/SlateImageBrush.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphUtilities.h"
#include "EditorStyleSet.h"
#include "HAL/IConsoleManager.h"
#include "IDetailCustomization.h"
#include "ISettingsModule.h"
#include "Metasound.h"
#include "MetasoundAssetSubsystem.h"
#include "MetasoundAssetTypeActions.h"
#include "MetasoundAudioBuffer.h"
#include "MetasoundDetailCustomization.h"
#include "MetasoundEditorGraph.h"
#include "MetasoundEditorGraphBuilder.h"
#include "MetasoundEditorGraphConnectionDrawingPolicy.h"
#include "MetasoundEditorGraphMemberDefaults.h"
#include "MetasoundEditorGraphNodeFactory.h"
#include "MetasoundEditorSettings.h"
#include "MetasoundFrontendDataTypeRegistry.h"
#include "MetasoundFrontendDocument.h"
#include "MetasoundFrontendRegistries.h"
#include "MetasoundFrontendTransform.h"
#include "MetasoundNodeDetailCustomization.h"
#include "MetasoundSettings.h"
#include "MetasoundSource.h"
#include "MetasoundTime.h"
#include "MetasoundTrigger.h"
#include "MetasoundUObjectRegistry.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorDelegates.h"
#include "PropertyEditorModule.h"
#include "Styling/CoreStyle.h"
#include "Styling/StyleColors.h"
#include "Styling/SlateStyle.h"
#include "Styling/SlateStyleMacros.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/SlateTypes.h"
#include "Templates/SharedPointer.h"
#include "UObject/UObjectGlobals.h"


DEFINE_LOG_CATEGORY(LogMetasoundEditor);


static int32 MetaSoundEditorAsyncRegistrationEnabledCVar = 1;
FAutoConsoleVariableRef CVarMetaSoundEditorAsyncRegistrationEnabled(
	TEXT("au.MetaSounds.Editor.AsyncRegistrationEnabled"),
	MetaSoundEditorAsyncRegistrationEnabledCVar,
	TEXT("Enable registering all MetaSound asset classes asyncronously on editor load.\n")
	TEXT("0: Disabled, !0: Enabled (default)"),
	ECVF_Default);

// Forward declarations 
class UMetasoundInterfacesView;

namespace Metasound
{
	namespace Editor
	{
		static const FName AssetToolName { "AssetTools" };

		template <typename T>
		void AddAssetAction(IAssetTools& AssetTools, TArray<TSharedPtr<FAssetTypeActions_Base>>& AssetArray)
		{
			TSharedPtr<T> AssetAction = MakeShared<T>();
			TSharedPtr<FAssetTypeActions_Base> AssetActionBase = StaticCastSharedPtr<FAssetTypeActions_Base>(AssetAction);
			AssetTools.RegisterAssetTypeActions(AssetAction.ToSharedRef());
			AssetArray.Add(AssetActionBase);
		}

		class FSlateStyle final : public FSlateStyleSet
		{
		public:
			FSlateStyle()
				: FSlateStyleSet("MetaSoundStyle")
			{
				SetParentStyleName(FEditorStyle::GetStyleSetName());

				SetContentRoot(FPaths::EnginePluginsDir() / TEXT("Runtime/Metasound/Content/Editor/Slate"));
				SetCoreContentRoot(FPaths::EngineContentDir() / TEXT("Slate"));

				static const FVector2D Icon20x20(20.0f, 20.0f);
				static const FVector2D Icon40x40(40.0f, 40.0f);

				static const FVector2D Icon16 = FVector2D(16.0f, 16.0f);
				static const FVector2D Icon64 = FVector2D(64.0f, 64.0f);

				const FVector2D Icon15x11(15.0f, 11.0f);

				// Metasound Editor
				{
					// Actions
					Set("MetasoundEditor.Play", new FSlateImageBrush(RootToContentDir(TEXT("Icons/play_40x.png")), Icon40x40));
					Set("MetasoundEditor.Play.Small", new FSlateImageBrush(RootToContentDir(TEXT("Icons/play_40x.png")), Icon20x20));
					Set("MetasoundEditor.Stop", new FSlateImageBrush(RootToContentDir(TEXT("Icons/stop_40x.png")), Icon40x40));
					Set("MetasoundEditor.Stop.Small", new FSlateImageBrush(RootToContentDir(TEXT("Icons/stop_40x.png")), Icon20x20));
					Set("MetasoundEditor.Import", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_40x.png")), Icon40x40));
					Set("MetasoundEditor.Import.Small", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_40x.png")), Icon20x20));
					Set("MetasoundEditor.Export", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_40x.png")), Icon40x40));
					Set("MetasoundEditor.Export.Small", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_40x.png")), Icon20x20));
					Set("MetasoundEditor.ExportError", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_error_40x.png")), Icon40x40));
					Set("MetasoundEditor.ExportError.Small", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/build_error_40x.png")), Icon20x20));
					Set("MetasoundEditor.Settings", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/settings_40x.png")), Icon20x20));

					// Graph Editor
					Set("MetasoundEditor.Graph.Node.Body.Input", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_input_body_64x.png")), FVector2D(114.0f, 64.0f)));
					Set("MetasoundEditor.Graph.Node.Body.Default", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_default_body_64x.png")), FVector2D(64.0f, 64.0f)));

					Set("MetasoundEditor.Graph.TriggerPin.Connected", new IMAGE_BRUSH(TEXT("Graph/pin_trigger_connected"), Icon15x11));
					Set("MetasoundEditor.Graph.TriggerPin.Disconnected", new IMAGE_BRUSH(TEXT("Graph/pin_trigger_disconnected"), Icon15x11));

					Set("MetasoundEditor.Graph.Node.Class.Native", new IMAGE_BRUSH_SVG(TEXT("Icons/native_node"), FVector2D(8.0f, 16.0f)));
					Set("MetasoundEditor.Graph.Node.Class.Graph", new IMAGE_BRUSH_SVG(TEXT("Icons/graph_node"), Icon16));
					Set("MetasoundEditor.Graph.Node.Class.Input", new IMAGE_BRUSH_SVG(TEXT("Icons/input_node"), FVector2D(16.0f, 13.0f)));
					Set("MetasoundEditor.Graph.Node.Class.Output", new IMAGE_BRUSH_SVG(TEXT("Icons/output_node"), FVector2D(16.0f, 13.0f)));
					Set("MetasoundEditor.Graph.Node.Class.Variable", new IMAGE_BRUSH_SVG(TEXT("Icons/variable_node"), FVector2D(16.0f, 13.0f)));

					Set("MetasoundEditor.Graph.Node.Math.Add", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_add_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Divide", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_divide_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Modulo", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_modulo_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Multiply", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_multiply_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Subtract", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_subtract_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Power", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_power_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Math.Logarithm", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_math_logarithm_40x.png")), Icon40x40));
					Set("MetasoundEditor.Graph.Node.Conversion", new FSlateImageBrush(RootToContentDir(TEXT("/Graph/node_conversion_40x.png")), Icon40x40));

					// Analyzers
					Set("MetasoundEditor.Analyzers.BackgroundColor", FLinearColor(0.0075f, 0.0075f, 0.0075, 1.0f));

					// Misc
					Set("MetasoundEditor.Speaker", new FSlateImageBrush(RootToContentDir(TEXT("/Icons/speaker_144x.png")), FVector2D(144.0f, 144.0f)));
					Set("MetasoundEditor.Metasound.Icon", new IMAGE_BRUSH_SVG(TEXT("Icons/metasound_icon"), Icon16));

					// Class Icons
					auto SetClassIcon = [this, InIcon16 = Icon16, InIcon64 = Icon64](const FString& ClassName)
					{
						const FString IconFileName = FString::Printf(TEXT("Icons/%s"), *ClassName.ToLower());
						const FSlateColor DefaultForeground(FStyleColors::Foreground);

						Set(*FString::Printf(TEXT("ClassIcon.%s"), *ClassName), new IMAGE_BRUSH_SVG(IconFileName, InIcon16));
						Set(*FString::Printf(TEXT("ClassThumbnail.%s"), *ClassName), new IMAGE_BRUSH_SVG(IconFileName, InIcon64));
					};

					SetClassIcon(TEXT("Metasound"));
					SetClassIcon(TEXT("MetasoundSource"));
				}

				FSlateStyleRegistry::RegisterSlateStyle(*this);
			}
		};

		class FMetasoundGraphPanelPinFactory : public FGraphPanelPinFactory
		{
		};

		class FModule : public IMetasoundEditorModule
		{
			void LoadAndRegisterAsset(const FAssetData& InAssetData)
			{
				Frontend::FMetaSoundAssetRegistrationOptions RegOptions;
				RegOptions.bForceReregister = false;
				if (const UMetaSoundSettings* Settings = GetDefault<UMetaSoundSettings>())
				{
					RegOptions.bAutoUpdateLogWarningOnDroppedConnection = Settings->bAutoUpdateLogWarningOnDroppedConnection;
				}

				if (InAssetData.IsAssetLoaded())
				{
					if (UObject* AssetObject = InAssetData.GetAsset())
					{
						FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(AssetObject);
						check(MetaSoundAsset);
						MetaSoundAsset->RegisterGraphWithFrontend(RegOptions);
					}
				}
				else
				{
					if (!MetaSoundEditorAsyncRegistrationEnabledCVar)
					{
						return;
					}

					if (AssetPrimeStatus == EAssetPrimeStatus::NotRequested)
					{
						return;
					}

					ActiveAsyncAssetLoadRequests++;

					FSoftObjectPath AssetPath = InAssetData.ToSoftObjectPath();
					auto LoadAndRegister = [this, ObjectPath = AssetPath, RegOptions](const FName& PackageName, UPackage* Package, EAsyncLoadingResult::Type Result)
					{
						if (Result == EAsyncLoadingResult::Succeeded)
						{
							FMetasoundAssetBase* MetaSoundAsset = IMetasoundUObjectRegistry::Get().GetObjectAsAssetBase(ObjectPath.ResolveObject());
							check(MetaSoundAsset);
							if (!MetaSoundAsset->IsRegistered())
							{
								MetaSoundAsset->RegisterGraphWithFrontend(RegOptions);
							}
						}

						ActiveAsyncAssetLoadRequests--;
						if (AssetPrimeStatus == EAssetPrimeStatus::InProgress && ActiveAsyncAssetLoadRequests == 0)
						{
							AssetPrimeStatus = EAssetPrimeStatus::Complete;
						}
					};
					LoadPackageAsync(AssetPath.GetLongPackageName(), FLoadPackageAsyncDelegate::CreateLambda(LoadAndRegister));
				}
			}

			void AddClassRegistryAsset(const FAssetData& InAssetData)
			{
				using namespace Frontend;

				if (!IsMetaSoundAssetClass(InAssetData.AssetClass))
				{
					return;
				}

				check(GEngine);
				UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
				check(AssetSubsystem);

				const FNodeRegistryKey RegistryKey = AssetSubsystem->AddOrUpdateAsset(InAssetData);

				// Can be invalid if being called for the first time on an asset before FRenameRootGraphClass is called
				if (NodeRegistryKey::IsValid(RegistryKey))
				{
					const bool bPrimeRequested = AssetPrimeStatus > EAssetPrimeStatus::NotRequested;
					const bool bIsRegistered = FMetasoundFrontendRegistryContainer::Get()->IsNodeRegistered(RegistryKey);
					if (bPrimeRequested && !bIsRegistered)
					{
						LoadAndRegisterAsset(InAssetData);
					}
				}
			}

			void UpdateClassRegistryAsset(const FAssetData& InAssetData)
			{
				using namespace Frontend;

				if (!IsMetaSoundAssetClass(InAssetData.AssetClass))
				{
					return;
				}

				check(GEngine);
				UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
				check(AssetSubsystem);

				const FNodeRegistryKey RegistryKey = AssetSubsystem->AddOrUpdateAsset(InAssetData);
				const bool bPrimeRequested = AssetPrimeStatus > EAssetPrimeStatus::NotRequested;
				const bool bIsRegistered = FMetasoundFrontendRegistryContainer::Get()->IsNodeRegistered(RegistryKey);

				// Have to re-register even if prime was not requested to avoid registry desync.
				if (bPrimeRequested || bIsRegistered)
				{
					LoadAndRegisterAsset(InAssetData);
				}
			}

			void OnPackageReloaded(const EPackageReloadPhase InPackageReloadPhase, FPackageReloadedEvent* InPackageReloadedEvent)
			{
				using namespace Metasound;
				using namespace Metasound::Editor;
				using namespace Metasound::Frontend;

				if (!InPackageReloadedEvent)
				{
					return;
				}

				if (InPackageReloadPhase != EPackageReloadPhase::OnPackageFixup)
				{
					return;
				}

				for (const TPair<UObject*, UObject*>& Pair : InPackageReloadedEvent->GetRepointedObjects())
				{
					if (UObject* Obj = Pair.Key)
					{
						if (IsMetaSoundAssetClass(Obj->GetClass()->GetFName()))
						{
							check(GEngine);
							UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
							check(AssetSubsystem);

							// Use the editor version of UnregisterWithFrontend so it refreshes any open MetaSound editors
							AssetSubsystem->RemoveAsset(*Pair.Key);
							FGraphBuilder::UnregisterGraphWithFrontend(*Pair.Key);
						}
					}

					if (UObject* Obj = Pair.Value)
					{
						if (IsMetaSoundAssetClass(Obj->GetClass()->GetFName()))
						{
							check(GEngine);
							UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
							check(AssetSubsystem);
							// Use the editor version of RegisterWithFrontend so it refreshes any open MetaSound editors
							AssetSubsystem->AddOrUpdateAsset(*Pair.Value);
							FGraphBuilder::RegisterGraphWithFrontend(*Pair.Value);
						}
					}
				}
			}

			void OnAssetScanFinished()
			{
				AssetScanStatus = EAssetScanStatus::Complete;

				if (AssetPrimeStatus == EAssetPrimeStatus::Requested)
				{
					PrimeAssetRegistryAsync();
				}

				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				AssetRegistryModule.Get().OnAssetAdded().AddRaw(this, &FModule::AddClassRegistryAsset);
				AssetRegistryModule.Get().OnAssetUpdated().AddRaw(this, &FModule::UpdateClassRegistryAsset);
				AssetRegistryModule.Get().OnAssetRemoved().AddRaw(this, &FModule::RemoveAssetFromClassRegistry);
				AssetRegistryModule.Get().OnAssetRenamed().AddRaw(this, &FModule::RenameAssetInClassRegistry);

				AssetRegistryModule.Get().OnFilesLoaded().RemoveAll(this);

				FCoreUObjectDelegates::OnPackageReloaded.AddRaw(this, &FModule::OnPackageReloaded);
			}

			void RemoveAssetFromClassRegistry(const FAssetData& InAssetData)
			{
				if (IsMetaSoundAssetClass(InAssetData.AssetClass))
				{
					check(GEngine);
					UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
					check(AssetSubsystem);

					// Use the editor version of UnregisterWithFrontend so it refreshes any open MetaSound editors
					AssetSubsystem->RemoveAsset(InAssetData);
					if (UObject* AssetObject = InAssetData.GetAsset())
					{
						FGraphBuilder::UnregisterGraphWithFrontend(*AssetObject);
					}
				}
			}

			void RenameAssetInClassRegistry(const FAssetData& InAssetData, const FString& InOldObjectPath)
			{
				if (IsMetaSoundAssetClass(InAssetData.AssetClass))
				{
					check(GEngine);
					UMetaSoundAssetSubsystem* AssetSubsystem = GEngine->GetEngineSubsystem<UMetaSoundAssetSubsystem>();
					check(AssetSubsystem);

					// Use the FGraphBuilder Register call instead of registering via the
					// MetaSoundAssetSubsystem so as to properly refresh respective open editors.
					constexpr bool bReregisterWithFrontend = false;
					AssetSubsystem->RenameAsset(InAssetData, bReregisterWithFrontend);

					constexpr bool bForceViewSynchronization = true;
					UObject* AssetObject = InAssetData.GetAsset();
					FGraphBuilder::RegisterGraphWithFrontend(*AssetObject, bForceViewSynchronization);
				}
			}

			void RegisterInputDefaultClasses()
			{
				TSubclassOf<UMetasoundEditorGraphMemberDefaultLiteral> NodeClass;
				for (TObjectIterator<UClass> ClassIt; ClassIt; ++ClassIt)
				{
					UClass* Class = *ClassIt;
					if (!Class->IsNative())
					{
						continue;
					}

					if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
					{
						continue;
					}

					if (!ClassIt->IsChildOf(UMetasoundEditorGraphMemberDefaultLiteral::StaticClass()))
					{
						continue;
					}

					if (const UMetasoundEditorGraphMemberDefaultLiteral* DefaultLiteralCDO = Class->GetDefaultObject<UMetasoundEditorGraphMemberDefaultLiteral>())
					{
						InputDefaultLiteralClassRegistry.Add(DefaultLiteralCDO->GetLiteralType(), DefaultLiteralCDO->GetClass());
					}
				}
			}

			void RegisterCoreDataTypes()
			{
				using namespace Metasound::Frontend;

				const IDataTypeRegistry& DataTypeRegistry = IDataTypeRegistry::Get();

				TArray<FName> DataTypeNames;
				DataTypeRegistry.GetRegisteredDataTypeNames(DataTypeNames);

				for (FName DataTypeName : DataTypeNames)
				{
					FDataTypeRegistryInfo RegistryInfo;
					if (ensure(DataTypeRegistry.GetDataTypeInfo(DataTypeName, RegistryInfo)))
					{
						FName PinCategory = DataTypeName;
						FName PinSubCategory;

						// Execution path triggers are specialized
						if (DataTypeName == GetMetasoundDataTypeName<FTrigger>())
						{
							PinCategory = FGraphBuilder::PinCategoryTrigger;
						}

						// GraphEditor by default designates specialized connection
						// specification for Int64, so use it even though literal is
						// boiled down to int32
						//else if (DataTypeName == Frontend::GetDataTypeName<int64>())
						//{
						//	PinCategory = FGraphBuilder::PinCategoryInt64;
						//}

						// Primitives
						else
						{
							switch (RegistryInfo.PreferredLiteralType)
							{
								case ELiteralType::Boolean:
								case ELiteralType::BooleanArray:
								{
									PinCategory = FGraphBuilder::PinCategoryBoolean;
								}
								break;

								case ELiteralType::Float:
								case ELiteralType::FloatArray:
								{
									PinCategory = FGraphBuilder::PinCategoryFloat;

									// Doubles use the same preferred literal
									// but different colorization
									//if (DataTypeName == Frontend::GetDataTypeName<double>())
									//{
									//	PinCategory = FGraphBuilder::PinCategoryDouble;
									//}

									// Differentiate stronger numeric types associated with audio
									if (DataTypeName == GetMetasoundDataTypeName<FTime>())
									{
										PinSubCategory = FGraphBuilder::PinSubCategoryTime;
									}
								}
								break;

								case ELiteralType::Integer:
								case ELiteralType::IntegerArray:
								{
									PinCategory = FGraphBuilder::PinCategoryInt32;
								}
								break;

								case ELiteralType::String:
								case ELiteralType::StringArray:
								{
									PinCategory = FGraphBuilder::PinCategoryString;
								}
								break;

								case ELiteralType::UObjectProxy:
								case ELiteralType::UObjectProxyArray:
								{
									PinCategory = FGraphBuilder::PinCategoryObject;
								}
								break;

								case ELiteralType::None:
								case ELiteralType::Invalid:
								default:
								{
									// Audio types are ubiquitous, so added as subcategory
									// to be able to stylize connections (i.e. wire color & wire animation)
									if (DataTypeName == GetMetasoundDataTypeName<FAudioBuffer>())
									{
										PinCategory = FGraphBuilder::PinCategoryAudio;
									}
									static_assert(static_cast<int32>(ELiteralType::Invalid) == 12, "Possible missing binding of pin category to primitive type");
								}
								break;
							}
						}

						const bool bIsArray = RegistryInfo.IsArrayType();
						const EPinContainerType ContainerType = bIsArray ? EPinContainerType::Array : EPinContainerType::None;
						FEdGraphPinType PinType(PinCategory, PinSubCategory, nullptr, ContainerType, false, FEdGraphTerminalType());
						UClass* ClassToUse = DataTypeRegistry.GetUClassForDataType(DataTypeName);
						PinType.PinSubCategoryObject = Cast<UObject>(ClassToUse);

						DataTypeInfo.Emplace(DataTypeName, FEditorDataType(MoveTemp(PinType), MoveTemp(RegistryInfo)));
					}
				}
			}

			void ShutdownAssetClassRegistry()
			{
				if (FAssetRegistryModule* AssetRegistryModule = static_cast<FAssetRegistryModule*>(FModuleManager::Get().GetModule("AssetRegistry")))
				{
					AssetRegistryModule->Get().OnAssetAdded().RemoveAll(this);
					AssetRegistryModule->Get().OnAssetUpdated().RemoveAll(this);
					AssetRegistryModule->Get().OnAssetRemoved().RemoveAll(this);
					AssetRegistryModule->Get().OnAssetRenamed().RemoveAll(this);
					AssetRegistryModule->Get().OnFilesLoaded().RemoveAll(this);

					FCoreUObjectDelegates::OnPackageReloaded.RemoveAll(this);
				}
			}

			virtual void PrimeAssetRegistryAsync() override
			{
				// Ignore step if still loading assets from initial scan but set prime status as requested.
				if (AssetScanStatus <= EAssetScanStatus::InProgress)
				{
					AssetPrimeStatus = EAssetPrimeStatus::Requested;
					return;
				}

				if (AssetPrimeStatus != EAssetPrimeStatus::InProgress)
				{
					AssetPrimeStatus = EAssetPrimeStatus::InProgress;

					FARFilter Filter;
					Filter.ClassNames = MetaSoundClassNames;

					FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
					AssetRegistryModule.Get().EnumerateAssets(Filter, [this](const FAssetData& AssetData)
					{
						AddClassRegistryAsset(AssetData);
						return true;
					});
				}
			}

			virtual EAssetPrimeStatus GetAssetRegistryPrimeStatus() const override
			{
				return AssetPrimeStatus;
			}

			virtual void RegisterExplicitProxyClass(const UClass& InClass) override
			{
				using namespace Metasound::Frontend;

				const IDataTypeRegistry& DataTypeRegistry = IDataTypeRegistry::Get();
				FDataTypeRegistryInfo RegistryInfo;
				ensureAlways(DataTypeRegistry.IsUObjectProxyFactory(InClass.GetDefaultObject()));

				ExplicitProxyClasses.Add(&InClass);
			}

			virtual bool IsExplicitProxyClass(const UClass& InClass) const override
			{
				return ExplicitProxyClasses.Contains(&InClass);
			}

			virtual TUniquePtr<FMetasoundDefaultLiteralCustomizationBase> CreateMemberDefaultLiteralCustomization(UClass& InClass, IDetailCategoryBuilder& InDefaultCategoryBuilder) const override
			{
				const TUniquePtr<IMemberDefaultLiteralCustomizationFactory>* CustomizationFactory = LiteralCustomizationFactories.Find(&InClass);
				if (CustomizationFactory && CustomizationFactory->IsValid())
				{
					return (*CustomizationFactory)->CreateLiteralCustomization(InDefaultCategoryBuilder);
				}

				return nullptr;
			}

			virtual const TSubclassOf<UMetasoundEditorGraphMemberDefaultLiteral> FindDefaultLiteralClass(EMetasoundFrontendLiteralType InLiteralType) const override
			{
				return InputDefaultLiteralClassRegistry.FindRef(InLiteralType);
			}

			virtual const FEditorDataType* FindDataType(FName InDataTypeName) const override
			{
				return DataTypeInfo.Find(InDataTypeName);
			}

			virtual const FEditorDataType& FindDataTypeChecked(FName InDataTypeName) const override
			{
				return DataTypeInfo.FindChecked(InDataTypeName);
			}

			virtual bool IsRegisteredDataType(FName InDataTypeName) const override
			{
				return DataTypeInfo.Contains(InDataTypeName);
			}

			virtual void IterateDataTypes(TUniqueFunction<void(const FEditorDataType&)> InDataTypeFunction) const override
			{
				for (const TPair<FName, FEditorDataType>& Pair : DataTypeInfo)
				{
					InDataTypeFunction(Pair.Value);
				}
			}

			virtual bool IsMetaSoundAssetClass(const FName InClassName) const override
			{
				// TODO: Move to IMetasoundUObjectRegistry (overload IsRegisteredClass to take in class name?)
				return MetaSoundClassNames.Contains(InClassName);
			}

			virtual void StartupModule() override
			{
				// Register Metasound asset type actions
				IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(AssetToolName).Get();

				AddAssetAction<FAssetTypeActions_MetaSound>(AssetTools, AssetActions);
				AddAssetAction<FAssetTypeActions_MetaSoundSource>(AssetTools, AssetActions);

				FPropertyEditorModule& PropertyModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
				
				PropertyModule.RegisterCustomClassLayout(
					UMetaSound::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundDetailCustomization>(UMetaSound::GetDocumentPropertyName()); }));

				PropertyModule.RegisterCustomClassLayout(
					UMetaSoundSource::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundDetailCustomization>(UMetaSoundSource::GetDocumentPropertyName()); }));

				PropertyModule.RegisterCustomClassLayout(
					UMetasoundInterfacesView::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundInterfacesDetailCustomization>(); }));

				PropertyModule.RegisterCustomClassLayout(
					UMetasoundEditorGraphInput::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundInputDetailCustomization>(); }));

				PropertyModule.RegisterCustomClassLayout(
					UMetasoundEditorGraphOutput::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundOutputDetailCustomization>(); }));

				PropertyModule.RegisterCustomClassLayout(
					UMetasoundEditorGraphVariable::StaticClass()->GetFName(),
					FOnGetDetailCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundVariableDetailCustomization>(); }));

				PropertyModule.RegisterCustomPropertyTypeLayout(
					"MetaSoundEditorGraphMemberDefaultBoolRef",
					FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundMemberDefaultBoolDetailCustomization>(); }));

				PropertyModule.RegisterCustomPropertyTypeLayout(
					"MetaSoundEditorGraphMemberDefaultIntRef",
					FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundMemberDefaultIntDetailCustomization>(); }));

				PropertyModule.RegisterCustomPropertyTypeLayout(
					"MetaSoundEditorGraphMemberDefaultObjectRef",
					FOnGetPropertyTypeCustomizationInstance::CreateLambda([]() { return MakeShared<FMetasoundMemberDefaultObjectDetailCustomization>(); }));

				LiteralCustomizationFactories.Add(UMetasoundEditorGraphMemberDefaultLiteral::StaticClass(), MakeUnique<FMetasoundDefaultLiteralCustomizationFactory>());
				LiteralCustomizationFactories.Add(UMetasoundEditorGraphMemberDefaultFloat::StaticClass(), MakeUnique<FMetasoundFloatLiteralCustomizationFactory>());
				LiteralCustomizationFactories.Add(UMetasoundEditorGraphMemberDefaultObjectArray::StaticClass(), MakeUnique<FMetasoundObjectArrayLiteralCustomizationFactory>());

				StyleSet = MakeShared<FSlateStyle>();

				RegisterCoreDataTypes();
				RegisterInputDefaultClasses();

				GraphConnectionFactory = MakeShared<FGraphConnectionDrawingPolicyFactory>();
				FEdGraphUtilities::RegisterVisualPinConnectionFactory(GraphConnectionFactory);

				GraphNodeFactory = MakeShared<FMetasoundGraphNodeFactory>();
				FEdGraphUtilities::RegisterVisualNodeFactory(GraphNodeFactory);

				GraphPanelPinFactory = MakeShared<FMetasoundGraphPanelPinFactory>();
				FEdGraphUtilities::RegisterVisualPinFactory(GraphPanelPinFactory);

				ISettingsModule& SettingsModule = FModuleManager::LoadModuleChecked<ISettingsModule>("Settings");

				SettingsModule.RegisterSettings("Editor", "ContentEditors", "MetaSound Editor",
					NSLOCTEXT("MetaSoundsEditor", "MetaSoundEditorSettingsName", "MetaSound Editor"),
					NSLOCTEXT("MetaSoundsEditor", "MetaSoundEditorSettingsDescription", "Customize MetaSound Editor."),
					GetMutableDefault<UMetasoundEditorSettings>()
				);

				MetaSoundClassNames.Add(UMetaSound::StaticClass()->GetFName());
				MetaSoundClassNames.Add(UMetaSoundSource::StaticClass()->GetFName());

				FAssetTypeActions_MetaSound::RegisterMenuActions();
				FAssetTypeActions_MetaSoundSource::RegisterMenuActions();

				FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry");
				if (AssetRegistryModule.Get().IsLoadingAssets())
				{
					AssetScanStatus = EAssetScanStatus::InProgress;
					AssetRegistryModule.Get().OnFilesLoaded().AddRaw(this, &FModule::OnAssetScanFinished);
				}
				else
				{
					AssetScanStatus = EAssetScanStatus::Complete;
				}

				// Metasound Engine registers USoundWave as a proxy class in the
				// Metasound Frontend. The frontend registration must occur before
				// the Metasound Editor registration of a USoundWave.
				FModuleManager::LoadModuleChecked<IModuleInterface>("MetasoundEngine");

				RegisterExplicitProxyClass(*USoundWave::StaticClass());
			}

			virtual void ShutdownModule() override
			{
				if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
				{
					SettingsModule->UnregisterSettings("Editor", "Audio", "MetaSound Editor");
				}

				if (FModuleManager::Get().IsModuleLoaded(AssetToolName))
				{
					IAssetTools& AssetTools = FModuleManager::GetModuleChecked<FAssetToolsModule>(AssetToolName).Get();
					for (TSharedPtr<FAssetTypeActions_Base>& AssetAction : AssetActions)
					{
						AssetTools.UnregisterAssetTypeActions(AssetAction.ToSharedRef());
					}
				}

				if (GraphConnectionFactory.IsValid())
				{
					FEdGraphUtilities::UnregisterVisualPinConnectionFactory(GraphConnectionFactory);
				}

				if (GraphNodeFactory.IsValid())
				{
					FEdGraphUtilities::UnregisterVisualNodeFactory(GraphNodeFactory);
					GraphNodeFactory.Reset();
				}

				if (GraphPanelPinFactory.IsValid())
				{
					FEdGraphUtilities::UnregisterVisualPinFactory(GraphPanelPinFactory);
					GraphPanelPinFactory.Reset();
				}

				ShutdownAssetClassRegistry();

				AssetActions.Reset();
				DataTypeInfo.Reset();
				MetaSoundClassNames.Reset();
			}

			TArray<FName> MetaSoundClassNames;

			TArray<TSharedPtr<FAssetTypeActions_Base>> AssetActions;
			TMap<FName, FEditorDataType> DataTypeInfo;
			TMap<EMetasoundFrontendLiteralType, const TSubclassOf<UMetasoundEditorGraphMemberDefaultLiteral>> InputDefaultLiteralClassRegistry;

			TMap<UClass*, TUniquePtr<IMemberDefaultLiteralCustomizationFactory>> LiteralCustomizationFactories;

			TSharedPtr<FMetasoundGraphNodeFactory> GraphNodeFactory;
			TSharedPtr<FGraphPanelPinConnectionFactory> GraphConnectionFactory;
			TSharedPtr<FMetasoundGraphPanelPinFactory> GraphPanelPinFactory;
			TSharedPtr<FSlateStyleSet> StyleSet;

			TSet<const UClass*> ExplicitProxyClasses;

			EAssetPrimeStatus AssetPrimeStatus = EAssetPrimeStatus::NotRequested;
			EAssetScanStatus AssetScanStatus = EAssetScanStatus::NotRequested;
			int32 ActiveAsyncAssetLoadRequests = 0;
		};
	} // namespace Editor
} // namespace Metasound

IMPLEMENT_MODULE(Metasound::Editor::FModule, MetasoundEditor);
