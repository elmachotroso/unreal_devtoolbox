// Copyright Epic Games, Inc. All Rights Reserved.

#include "InterchangeManager.h"

#include "AssetDataTagMap.h"
#include "AssetRegistryModule.h"
#include "Async/Async.h"
#include "CoreMinimal.h"
#include "Engine/Blueprint.h"
#include "Framework/Notifications/NotificationManager.h"
#include "InterchangeAssetImportData.h"
#include "InterchangeFactoryBase.h"
#include "InterchangeEngineLogPrivate.h"
#include "InterchangeProjectSettings.h"
#include "InterchangeSourceData.h"
#include "InterchangeTranslatorBase.h"
#include "InterchangeWriterBase.h"
#include "Internationalization/Internationalization.h"
#include "Misc/App.h"
#include "Misc/AsyncTaskNotification.h"
#include "Misc/MessageDialog.h"
#include "Misc/ScopedSlowTask.h"
#include "Nodes/InterchangeBaseNodeContainer.h"
#include "PackageUtils/PackageUtils.h"
#include "Tasks/InterchangeTaskParsing.h"
#include "Tasks/InterchangeTaskPipeline.h"
#include "Tasks/InterchangeTaskTranslator.h"
#include "UObject/Class.h"
#include "UObject/GarbageCollection.h"
#include "UObject/NameTypes.h"
#include "UObject/Object.h"
#include "UObject/ObjectMacros.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtrTemplates.h"
#include "Widgets/Notifications/SNotificationList.h"



namespace InternalInterchangePrivate
{
	const FLogCategoryBase* GetLogInterchangePtr()
	{
#if NO_LOGGING
		return nullptr;
#else
		return &LogInterchangeEngine;
#endif
	}
}

UE::Interchange::FScopedSourceData::FScopedSourceData(const FString& Filename)
{
	//Found the translator
	SourceDataPtr = TStrongObjectPtr<UInterchangeSourceData>(UInterchangeManager::GetInterchangeManager().CreateSourceData(Filename));
	check(SourceDataPtr.IsValid());
}

UInterchangeSourceData* UE::Interchange::FScopedSourceData::GetSourceData() const
{
	return SourceDataPtr.Get();
}

UE::Interchange::FScopedTranslator::FScopedTranslator(const UInterchangeSourceData* SourceData)
{
	//Found the translator
	ScopedTranslatorPtr = TStrongObjectPtr<UInterchangeTranslatorBase>(UInterchangeManager::GetInterchangeManager().GetTranslatorForSourceData(SourceData));
}

UInterchangeTranslatorBase* UE::Interchange::FScopedTranslator::GetTranslator()
{
	return ScopedTranslatorPtr.Get();
}

UE::Interchange::FImportAsyncHelper::FImportAsyncHelper()
	: AssetImportResult(MakeShared<FImportResult>())
	, SceneImportResult(MakeShared<FImportResult>())
{
	bCancel = false;
}

void UE::Interchange::FImportAsyncHelper::AddReferencedObjects(FReferenceCollector& Collector)
{
	Collector.AddReferencedObjects(SourceDatas);
	Collector.AddReferencedObjects(Translators);
	Collector.AddReferencedObjects(Pipelines);
	Collector.AddReferencedObjects(CreatedFactories);
}

void UE::Interchange::FImportAsyncHelper::ReleaseTranslatorsSource()
{
	for (UInterchangeTranslatorBase* BaseTranslator : Translators)
	{
		if (BaseTranslator)
		{
			BaseTranslator->ReleaseSource();
		}
	}
}

void UE::Interchange::FImportAsyncHelper::InitCancel()
{
	bCancel = true;
	ReleaseTranslatorsSource();
}

void UE::Interchange::FImportAsyncHelper::CancelAndWaitUntilDoneSynchronously()
{
	bCancel = true;

	FGraphEventArray TasksToComplete;

	TasksToComplete.Append(TranslatorTasks);
	TasksToComplete.Append(PipelinePreImportTasks);
	
	if (ParsingTask.GetReference())
	{
		TasksToComplete.Add(ParsingTask);
	}

	TasksToComplete.Append(CreatePackageTasks);
	TasksToComplete.Append(CreateAssetTasks);
	TasksToComplete.Append(SceneTasks);
	TasksToComplete.Append(PipelinePostImportTasks);

	if (PreAsyncCompletionTask.GetReference())
	{
		TasksToComplete.Add(PreAsyncCompletionTask);
	}
	if (PreCompletionTask.GetReference())
	{
		TasksToComplete.Add(PreCompletionTask);
	}
	if (CompletionTask.GetReference())
	{
		//Completion task will make sure any created asset before canceling will be mark for delete
		TasksToComplete.Add(CompletionTask);
	}

	//Block until all task are completed, it should be fast since bCancel is true
	if (TasksToComplete.Num())
	{
		FTaskGraphInterface::Get().WaitUntilTasksComplete(TasksToComplete, ENamedThreads::GameThread);
	}

	AssetImportResult->SetDone();
	SceneImportResult->SetDone();
}

void UE::Interchange::FImportAsyncHelper::CleanUp()
{
	//Release the graph
	BaseNodeContainers.Empty();

	for (UInterchangeSourceData* SourceData : SourceDatas)
	{
		if (SourceData)
		{
			SourceData->RemoveFromRoot();
			SourceData->MarkAsGarbage();
		}
	}
	SourceDatas.Empty();

	for (UInterchangeTranslatorBase* Translator : Translators)
	{
		if(Translator)
		{
			Translator->ImportFinish();
			Translator->RemoveFromRoot();
			Translator->MarkAsGarbage();
		}
	}
	Translators.Empty();

	for (UInterchangePipelineBase* Pipeline : Pipelines)
	{
		if(Pipeline)
		{
			Pipeline->RemoveFromRoot();
			Pipeline->MarkAsGarbage();
		}
	}
	Pipelines.Empty();

	for (const TPair<FString, UInterchangeFactoryBase*>& FactoryKeyAndValue : CreatedFactories)
	{
		if (FactoryKeyAndValue.Value)
		{
			FactoryKeyAndValue.Value->RemoveFromRoot();
			FactoryKeyAndValue.Value->MarkAsGarbage();
		}
	}
	CreatedFactories.Empty();
}

UE::Interchange::FImportResult::FImportResult()
	: ImportStatus(EStatus::Invalid)
{
	Results = NewObject<UInterchangeResultsContainer>(GetTransientPackage());
}

UE::Interchange::FImportResult::EStatus UE::Interchange::FImportResult::GetStatus() const
{
	return ImportStatus;
}

bool UE::Interchange::FImportResult::IsValid() const
{
	return GetStatus() != EStatus::Invalid;
}

void UE::Interchange::FImportResult::SetInProgress()
{
	EStatus ExpectedStatus = EStatus::Invalid;
	if (ImportStatus.compare_exchange_strong(ExpectedStatus, EStatus::InProgress))
	{
		GraphEvent = FGraphEvent::CreateGraphEvent();
	}
}

void UE::Interchange::FImportResult::SetDone()
{
	SetInProgress(); // Make sure we always pass through the InProgress state

	EStatus ExpectedStatus = EStatus::InProgress;
	if (ImportStatus.compare_exchange_strong(ExpectedStatus, EStatus::Done))
	{
		if (DoneCallback)
		{
			DoneCallback(*this);
		}

		TArray<UObject*> Objects = GetImportedObjects();

		if (IsInGameThread())
		{
			OnImportDoneNative.ExecuteIfBound(Objects);
			OnImportDone.ExecuteIfBound(Objects);
		}
		else
		{
			TArray<TWeakObjectPtr<UObject>> WeakObjects;
			WeakObjects.Reserve(Objects.Num());
			for (UObject* Object : Objects)
			{
				WeakObjects.Emplace(Object);
			}

			// call the callbacks on the game thread
			Async(EAsyncExecution::TaskGraphMainThread, [InWeakObjects = MoveTemp(WeakObjects), ImportDoneNative = OnImportDoneNative, ImportDone = OnImportDone]()
			{
				TArray<UObject*> ValidObjects;
				ValidObjects.Reserve(InWeakObjects.Num());

				for (const TWeakObjectPtr<UObject>& WeakObject : InWeakObjects)
				{
					if (UObject* ValidObject = WeakObject.Get())
					{
						ValidObjects.Add(ValidObject);
					}
				}

				ImportDoneNative.ExecuteIfBound(ValidObjects);
				ImportDone.ExecuteIfBound(ValidObjects);
			});
		}


		GraphEvent->DispatchSubsequents();
	}
}

void UE::Interchange::FImportResult::WaitUntilDone()
{
	if (ImportStatus == EStatus::InProgress)
	{
		FTaskGraphInterface::Get().WaitUntilTaskCompletes(GraphEvent);
	}
}

const TArray< UObject* >& UE::Interchange::FImportResult::GetImportedObjects() const
{
	FReadScopeLock ReadScopeLock(ImportedObjectsRWLock);
	return ImportedObjects;
}

UObject* UE::Interchange::FImportResult::GetFirstAssetOfClass(UClass* InClass) const
{
	UObject* Asset = nullptr;

	FReadScopeLock ReadScopeLock(ImportedObjectsRWLock);
	for (UObject* ImportedAsset : ImportedObjects)
	{
		if (ImportedAsset->IsA(InClass))
		{
			Asset = ImportedAsset;
			break;
		}
	}

	return Asset;
}

void UE::Interchange::FImportResult::AddImportedObject(UObject* ImportedObject)
{
	{
		FWriteScopeLock WriteScopeLock(ImportedObjectsRWLock);
		ImportedObjects.Add(ImportedObject);
	}

	if (IsInGameThread())
	{
		OnObjectDoneNative.ExecuteIfBound(ImportedObject);
		OnObjectDone.ExecuteIfBound(ImportedObject);
	}
	else
	{
		// call the callbacks on the game thread
		Async(EAsyncExecution::TaskGraphMainThread, [WeakImportedObject = TWeakObjectPtr<UObject>(ImportedObject), ObjectDoneNative = OnObjectDoneNative, ObjectDone = OnObjectDone] ()
			{
				if (UObject* ImportedObjectInGameThread = WeakImportedObject.Get())
				{
					ObjectDoneNative.ExecuteIfBound(ImportedObjectInGameThread);
					ObjectDone.ExecuteIfBound(ImportedObjectInGameThread);
				}
			});
	}
}

void UE::Interchange::FImportResult::OnDone(TFunction< void(FImportResult&) > Callback)
{
	DoneCallback = Callback;
}

void UE::Interchange::FImportResult::AddReferencedObjects(FReferenceCollector& Collector)
{
	FReadScopeLock ReadScopeLock(ImportedObjectsRWLock);
	Collector.AddReferencedObjects(ImportedObjects);
	Collector.AddReferencedObject(Results);
}

void UE::Interchange::SanitizeObjectPath(FString& ObjectPath)
{
	const TCHAR* InvalidChar = INVALID_OBJECTPATH_CHARACTERS;
	while (*InvalidChar)
	{
		ObjectPath.ReplaceCharInline(*InvalidChar, TCHAR('_'), ESearchCase::CaseSensitive);
		++InvalidChar;
	}
}

void UE::Interchange::SanitizeObjectName(FString& ObjectName)
{
	const TCHAR* InvalidChar = INVALID_OBJECTNAME_CHARACTERS;
	while (*InvalidChar)
	{
		ObjectName.ReplaceCharInline(*InvalidChar, TCHAR('_'), ESearchCase::CaseSensitive);
		++InvalidChar;
	}
}

UInterchangeManager& UInterchangeManager::GetInterchangeManager()
{
	static TStrongObjectPtr<UInterchangeManager> InterchangeManager = nullptr;
	
	//This boolean will be true after we delete the singleton
	static bool InterchangeManagerScopeOfLifeEnded = false;

	if (!InterchangeManager.IsValid())
	{
		//We cannot create a TStrongObjectPtr outside of the main thread, we also need a valid Transient package
		check(IsInGameThread() && GetTransientPackage());

		//Avoid hard crash if someone call the manager after we delete it, but send a callstack to the crash manager
		ensure(!InterchangeManagerScopeOfLifeEnded);

		InterchangeManager = TStrongObjectPtr<UInterchangeManager>(NewObject<UInterchangeManager>(GetTransientPackage(), NAME_None, EObjectFlags::RF_NoFlags));
		
		//We cancel any running task when we pre exit the engine
		FCoreDelegates::OnEnginePreExit.AddLambda([]()
		{
			//In editor the user cannot exit the editor if the interchange manager has active task.
			//But if we are not running the editor its possible to get here, so block the main thread until all
			//cancel tasks are done.
			if(GIsEditor)
			{
				ensure(InterchangeManager->ImportTasks.Num() == 0);
			}
			else
			{
				InterchangeManager->CancelAllTasksSynchronously();
			}
			//Task should have been cancel in the Engine pre exit callback
			ensure(InterchangeManager->ImportTasks.Num() == 0);
			InterchangeManager->OnPreDestroyInterchangeManager.Broadcast();
			//Release the InterchangeManager object
			InterchangeManager.Reset();
			InterchangeManagerScopeOfLifeEnded = true;
		});
	}

	//When we get here we should be valid
	check(InterchangeManager.IsValid());

	return *(InterchangeManager.Get());
}

bool UInterchangeManager::RegisterTranslator(const UClass* TranslatorClass)
{
	if (!TranslatorClass)
	{
		return false;
	}

	RegisteredTranslatorsClass.Add(TranslatorClass);
	return true;
}

bool UInterchangeManager::RegisterFactory(const UClass* FactoryClass)
{
	if (!FactoryClass || !FactoryClass->IsChildOf<UInterchangeFactoryBase>())
	{
		return false;
	}

	UClass* ClassToMake = FactoryClass->GetDefaultObject<UInterchangeFactoryBase>()->GetFactoryClass();
	if (ClassToMake)
	{
		if (!RegisteredFactoryClasses.Contains(ClassToMake))
		{
			RegisteredFactoryClasses.Add(ClassToMake, FactoryClass);
		}

		return true;
	}

	return false;
}

bool UInterchangeManager::RegisterWriter(const UClass* WriterClass)
{
	if (!WriterClass)
	{
		return false;
	}

	if (RegisteredWriters.Contains(WriterClass))
	{
		return true;
	}
	UInterchangeWriterBase* WriterToRegister = NewObject<UInterchangeWriterBase>(GetTransientPackage(), WriterClass, NAME_None);
	if (!WriterToRegister)
	{
		return false;
	}
	RegisteredWriters.Add(WriterClass, WriterToRegister);
	return true;
}

bool UInterchangeManager::CanTranslateSourceData(const UInterchangeSourceData* SourceData) const
{
	// Can we find a translator
	if (GetTranslatorForSourceData(SourceData))
	{
		return true;
	}
	return false;
}

void UInterchangeManager::StartQueuedTasks(bool bCancelAllTasks /*= false*/)
{
	if (!ensure(IsInGameThread()))
	{
		//Do not crash but we will not start any queued tasks if we are not in the game thread
		return;
	}

	auto UpdateNotification = [this]()
	{
		if (Notification.IsValid())
		{
			int32 ImportTaskNumber = ImportTasks.Num() + QueueTaskCount;
			FString ImportTaskNumberStr = TEXT(" (") + FString::FromInt(ImportTaskNumber) + TEXT(")");
			Notification->SetProgressText(FText::FromString(ImportTaskNumberStr));
		}
		else
		{
			FText TitleText = NSLOCTEXT("Interchange", "Asynchronous_import_start", "Importing");
			FAsyncTaskNotificationConfig NotificationConfig;
			NotificationConfig.bIsHeadless = false;
			NotificationConfig.bKeepOpenOnFailure = true;
			NotificationConfig.TitleText = TitleText;
			NotificationConfig.LogCategory = InternalInterchangePrivate::GetLogInterchangePtr();
			NotificationConfig.bCanCancel.Set(true);
			NotificationConfig.bKeepOpenOnFailure.Set(true);

			Notification = MakeShared<FAsyncTaskNotification>(NotificationConfig);
			Notification->SetNotificationState(FAsyncNotificationStateData(TitleText, FText::GetEmpty(), EAsyncTaskNotificationState::Pending));
		}
	};

	while (!QueuedTasks.IsEmpty() && (ImportTasks.Num() < FTaskGraphInterface::Get().GetNumWorkerThreads() || bCancelAllTasks))
	{
		FQueuedTaskData QueuedTaskData;
		if (QueuedTasks.Dequeue(QueuedTaskData))
		{
			QueueTaskCount = FMath::Clamp(QueueTaskCount-1, 0, MAX_int32);
			check(QueuedTaskData.AsyncHelper.IsValid());

			int32 AsyncHelperIndex = ImportTasks.Add(QueuedTaskData.AsyncHelper);
			SetActiveMode(true);
			//Update the asynchronous notification
			UpdateNotification();

			TWeakPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> WeakAsyncHelper = QueuedTaskData.AsyncHelper;
			
			if (bCancelAllTasks)
			{
				QueuedTaskData.AsyncHelper->InitCancel();
			}

			//Create/Start import tasks
			FGraphEventArray PipelinePrerequistes;
			check(QueuedTaskData.AsyncHelper->Translators.Num() == QueuedTaskData.AsyncHelper->SourceDatas.Num());
			for (int32 SourceDataIndex = 0; SourceDataIndex < QueuedTaskData.AsyncHelper->SourceDatas.Num(); ++SourceDataIndex)
			{
				int32 TranslatorTaskIndex = QueuedTaskData.AsyncHelper->TranslatorTasks.Add(TGraphTask<UE::Interchange::FTaskTranslator>::CreateTask().ConstructAndDispatchWhenReady(SourceDataIndex, WeakAsyncHelper));
				PipelinePrerequistes.Add(QueuedTaskData.AsyncHelper->TranslatorTasks[TranslatorTaskIndex]);
			}

			FGraphEventArray GraphParsingPrerequistes;
			for (int32 GraphPipelineIndex = 0; GraphPipelineIndex < QueuedTaskData.AsyncHelper->Pipelines.Num(); ++GraphPipelineIndex)
			{
				UInterchangePipelineBase* GraphPipeline = QueuedTaskData.AsyncHelper->Pipelines[GraphPipelineIndex];
				TWeakObjectPtr<UInterchangePipelineBase> WeakPipelinePtr = GraphPipeline;
				int32 GraphPipelineTaskIndex = INDEX_NONE;
				GraphPipelineTaskIndex = QueuedTaskData.AsyncHelper->PipelinePreImportTasks.Add(TGraphTask<UE::Interchange::FTaskPipelinePreImport>::CreateTask(&PipelinePrerequistes).ConstructAndDispatchWhenReady(WeakPipelinePtr, WeakAsyncHelper));
				//Ensure we run the pipeline in the same order we create the task, since pipeline modify the node container, its important that its not process in parallel, Adding the one we start to the prerequisites
				//is the way to go here
				PipelinePrerequistes.Add(QueuedTaskData.AsyncHelper->PipelinePreImportTasks[GraphPipelineTaskIndex]);

				//Add pipeline to the graph parsing prerequisites
				GraphParsingPrerequistes.Add(QueuedTaskData.AsyncHelper->PipelinePreImportTasks[GraphPipelineTaskIndex]);
			}

			if (GraphParsingPrerequistes.Num() > 0)
			{
				QueuedTaskData.AsyncHelper->ParsingTask = TGraphTask<UE::Interchange::FTaskParsing>::CreateTask(&GraphParsingPrerequistes).ConstructAndDispatchWhenReady(this, QueuedTaskData.PackageBasePath, WeakAsyncHelper);
			}
			else
			{
				//Fallback on the translator pipeline prerequisites (translator must be done if there is no pipeline)
				QueuedTaskData.AsyncHelper->ParsingTask = TGraphTask<UE::Interchange::FTaskParsing>::CreateTask(&PipelinePrerequistes).ConstructAndDispatchWhenReady(this, QueuedTaskData.PackageBasePath, WeakAsyncHelper);
			}

			//The graph parsing task will create the FCreateAssetTask that will run after them, the FAssetImportTask will call the appropriate Post asset import pipeline when the asset is completed
		}
	}

	if (!QueuedTasks.IsEmpty())
	{
		//Make sure any task we add is count in the task to do, even if we cannot start it
		UpdateNotification();
	}
}

bool UInterchangeManager::ImportAsset(const FString& ContentPath, const UInterchangeSourceData* SourceData, const FImportAssetParameters& ImportAssetParameters)
{
	return ImportAssetAsync( ContentPath, SourceData, ImportAssetParameters )->IsValid();
}

UE::Interchange::FAssetImportResultRef UInterchangeManager::ImportAssetAsync(const FString& ContentPath, const UInterchangeSourceData* SourceData, const FImportAssetParameters& ImportAssetParameters)
{
	return ImportInternal(ContentPath, SourceData, ImportAssetParameters, UE::Interchange::EImportType::ImportType_Asset).Get<0>();
}

bool UInterchangeManager::ImportScene(const FString& ContentPath, const UInterchangeSourceData* SourceData, const FImportAssetParameters& ImportAssetParameters)
{
	using namespace UE::Interchange;
	TTuple<FAssetImportResultRef, FSceneImportResultRef> ImportResults = ImportInternal(ContentPath, SourceData, ImportAssetParameters, UE::Interchange::EImportType::ImportType_Scene);

	return ImportResults.Get<0>()->IsValid() && ImportResults.Get<1>()->IsValid();
}

TTuple<UE::Interchange::FAssetImportResultRef, UE::Interchange::FSceneImportResultRef>
UInterchangeManager::ImportSceneAsync(const FString& ContentPath, const UInterchangeSourceData* SourceData, const FImportAssetParameters& ImportAssetParameters)
{
	return ImportInternal(ContentPath, SourceData, ImportAssetParameters, UE::Interchange::EImportType::ImportType_Scene);
}

TTuple<UE::Interchange::FAssetImportResultRef, UE::Interchange::FSceneImportResultRef>
UInterchangeManager::ImportInternal(const FString& ContentPath, const UInterchangeSourceData* SourceData, const FImportAssetParameters& ImportAssetParameters, const UE::Interchange::EImportType ImportType)
{
	if (!ensure(IsInGameThread()))
	{
		//Import process can be started only in the game thread
		return TTuple<UE::Interchange::FAssetImportResultRef, UE::Interchange::FSceneImportResultRef>{ MakeShared< UE::Interchange::FImportResult, ESPMode::ThreadSafe >(), MakeShared< UE::Interchange::FImportResult, ESPMode::ThreadSafe >() };
	}
	UInterchangeAssetImportData* OriginalAssetImportData = nullptr;
	FString PackageBasePath = ContentPath;
	if(!ImportAssetParameters.ReimportAsset)
	{
		UE::Interchange::SanitizeObjectPath(PackageBasePath);
	}
	else
	{
		PackageBasePath = FPaths::GetPath(ImportAssetParameters.ReimportAsset->GetPathName());
		TArray<UObject*> SubObjects;
		GetObjectsWithOuter(ImportAssetParameters.ReimportAsset, SubObjects);
		for (UObject* SubObject : SubObjects)
		{
			OriginalAssetImportData = Cast<UInterchangeAssetImportData>(SubObject);
			if(OriginalAssetImportData)
			{
				break;
			}
		}
	}
	bool bImportCancel = false;

	//Create a task for every source data
	UE::Interchange::FImportAsyncHelperData TaskData;
	TaskData.bIsAutomated = ImportAssetParameters.bIsAutomated;
	TaskData.ImportType = ImportType;
	TaskData.ReimportObject = ImportAssetParameters.ReimportAsset;
	TSharedRef<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = CreateAsyncHelper(TaskData, ImportAssetParameters);

	//Create a duplicate of the source data, we need to be multithread safe so we copy it to control the life cycle. The async helper will hold it and delete it when the import task will be completed.
	UInterchangeSourceData* DuplicateSourceData = Cast<UInterchangeSourceData>(StaticDuplicateObject(SourceData, GetTransientPackage()));
	//Array of source data to build one graph per source
	AsyncHelper->SourceDatas.Add(DuplicateSourceData);

	//Get all the translators for the source datas
	for (int32 SourceDataIndex = 0; SourceDataIndex < AsyncHelper->SourceDatas.Num(); ++SourceDataIndex)
	{
		ensure(AsyncHelper->Translators.Add(GetTranslatorForSourceData(AsyncHelper->SourceDatas[SourceDataIndex])) == SourceDataIndex);
	}

	//Create the node graphs for each source data (StrongObjectPtr has to be created on the main thread)
	for (int32 SourceDataIndex = 0; SourceDataIndex < AsyncHelper->SourceDatas.Num(); ++SourceDataIndex)
	{
		AsyncHelper->BaseNodeContainers.Add(TStrongObjectPtr<UInterchangeBaseNodeContainer>(NewObject<UInterchangeBaseNodeContainer>(GetTransientPackage(), NAME_None)));
		check(AsyncHelper->BaseNodeContainers[SourceDataIndex].IsValid());
	}

	const UInterchangeProjectSettings* InterchangeProjectSettings = GetDefault<UInterchangeProjectSettings>();
	UInterchangePipelineConfigurationBase* RegisteredPipelineConfiguration = nullptr;

	//In runtime we do not have any pipeline configurator
#if WITH_EDITORONLY_DATA
	TSoftClassPtr <UInterchangePipelineConfigurationBase> PipelineConfigurationDialogClass = InterchangeProjectSettings->PipelineConfigurationDialogClass;
	if (PipelineConfigurationDialogClass.IsValid())
	{
		UClass* PipelineConfigurationClass = PipelineConfigurationDialogClass.LoadSynchronous();
		if (PipelineConfigurationClass)
		{
			RegisteredPipelineConfiguration = NewObject<UInterchangePipelineConfigurationBase>(GetTransientPackage(), PipelineConfigurationClass, NAME_None, RF_NoFlags);
		}
	}
#endif


	if ( ImportAssetParameters.OverridePipelines.Num() == 0 )
	{
		const bool bIsUnattended = FApp::IsUnattended() || GIsAutomationTesting;

#if WITH_EDITORONLY_DATA
		const bool bShowPipelineStacksConfigurationDialog = !bIsUnattended
															&& InterchangeProjectSettings->bShowPipelineStacksConfigurationDialog
															&& !bImportAllWithDefault;
#else
		const bool bShowPipelineStacksConfigurationDialog = false;
#endif
															


		auto GetDefaultPipelineStackName = [ImportType, &InterchangeProjectSettings]()->FName
		{
			if (ImportType == UE::Interchange::EImportType::ImportType_Scene)
			{
				return InterchangeProjectSettings->DefaultScenePipelineStack;
			}
			else
			{
				return InterchangeProjectSettings->DefaultPipelineStack;
			}
		};
			
		const TMap<FName, FInterchangePipelineStack>& DefaultPipelineStacks = InterchangeProjectSettings->PipelineStacks;

		//If we reimport we want to load the original pipeline and the original pipeline settings
		if (OriginalAssetImportData && OriginalAssetImportData->Pipelines.Num() > 0)
		{
			TArray<UInterchangePipelineBase*> PipelineStack;
			for (int32 PipelineIndex = 0; PipelineIndex < OriginalAssetImportData->Pipelines.Num(); ++PipelineIndex)
			{
				UInterchangePipelineBase* SourcePipeline = OriginalAssetImportData->Pipelines[PipelineIndex];
				if (SourcePipeline) //Its possible a pipeline doesnt exist anymore so it wont load into memory when we loading the outer asset
				{
					//Duplicate the pipeline saved in the asset import data
					UInterchangePipelineBase* GeneratedPipeline = Cast<UInterchangePipelineBase>(StaticDuplicateObject(SourcePipeline, GetTransientPackage()));
					PipelineStack.Add(GeneratedPipeline);
				}
				else
				{
					//A pipeline was not loaded
					//Log something
				}
			}

			// Simply move the existing pipeline for now. To be revisited for the MVP release.
			AsyncHelper->Pipelines = MoveTemp(PipelineStack);

			//if (RegisteredPipelineConfiguration && (bShowPipelineStacksConfigurationDialog || (!DefaultPipelineStacks.Contains(DefaultPipelineStackName) && !bIsUnattended)))
			//{
				//Show the re-import dialog to let the user make change in the pipelines
				//PipelineStackName = RegisteredPipelineConfiguration->ScriptedShowReimportPipelineConfigurationDialog(PipelineStack);
			//}


			//If the Stack name is empty it mean we want to use the re-import stack (PipelineStack). If there is a name we will
			//extract the pipeline stack the user want to use.
		}
		else
		{
			FName PipelineStackName = GetDefaultPipelineStackName();
			if (RegisteredPipelineConfiguration && (bShowPipelineStacksConfigurationDialog || (!DefaultPipelineStacks.Contains(PipelineStackName) && !bIsUnattended)))
			{
				//Show the dialog, a plugin should have register this dialog. We use a plugin to be able to use editor code when doing UI
				EInterchangePipelineConfigurationDialogResult DialogResult = RegisteredPipelineConfiguration->ScriptedShowPipelineConfigurationDialog();
				if (DialogResult == EInterchangePipelineConfigurationDialogResult::Cancel)
				{
					bImportCancel = true;
				}
				if (DialogResult == EInterchangePipelineConfigurationDialogResult::ImportAll)
				{
					bImportAllWithDefault = true;
				}
				PipelineStackName = GetDefaultPipelineStackName();
			}
			if (!bImportCancel)
			{
				//Get the latest PipelineStacks (the Dialog can change the CDO)
				const TMap<FName, FInterchangePipelineStack>& PipelineStacks = InterchangeProjectSettings->PipelineStacks;
				if (!PipelineStacks.Contains(PipelineStackName))
				{
					if (PipelineStacks.Contains(GetDefaultPipelineStackName()))
					{
						PipelineStackName = GetDefaultPipelineStackName();
					}
					else
					{
						//Log an error, we cannot import asset without a valid pipeline, we will use the first available pipeline
						for (const TPair<FName, FInterchangePipelineStack>& PipelineStack : PipelineStacks)
						{
							PipelineStackName = PipelineStack.Key;
						}
					}
				}

				if (PipelineStacks.Contains(PipelineStackName))
				{
					//use the default pipeline
					const FInterchangePipelineStack& PipelineStack = PipelineStacks.FindChecked(PipelineStackName);
					TArray<TSoftClassPtr<UInterchangePipelineBase>> Pipelines = PipelineStack.Pipelines;
					for (int32 GraphPipelineIndex = 0; GraphPipelineIndex < Pipelines.Num(); ++GraphPipelineIndex)
					{
						if (Pipelines[GraphPipelineIndex].IsValid())
						{
							UClass* PipelineClass = Pipelines[GraphPipelineIndex].LoadSynchronous();
							if (PipelineClass)
							{
								UInterchangePipelineBase* GeneratedPipeline = NewObject<UInterchangePipelineBase>(GetTransientPackage(), PipelineClass, NAME_None, RF_NoFlags);
								//Load the settings for this pipeline
								GeneratedPipeline->LoadSettings(PipelineStackName);
								AsyncHelper->Pipelines.Add(GeneratedPipeline);
							}
						}
					}
				}
				else
				{
					//Log an error, we cannot import asset without a valid pipeline, there is no pipeline stack defined
					bImportCancel = true;
				}
			}
		}
	}
	else
	{
		for (int32 GraphPipelineIndex = 0; GraphPipelineIndex < ImportAssetParameters.OverridePipelines.Num(); ++GraphPipelineIndex)
		{
			// Duplicate the override pipelines to protect the scripted users form making race conditions
			AsyncHelper->Pipelines.Add(DuplicateObject<UInterchangePipelineBase>(ImportAssetParameters.OverridePipelines[GraphPipelineIndex], GetTransientPackage()));
		}
	}

	//Cancel the import do not queue task
	if (bImportCancel)
	{
		AsyncHelper->InitCancel();
		AsyncHelper->CleanUp();
	}

	//Queue the task cancel or not, we need to return a valid asset import result
	FQueuedTaskData QueuedTaskData;
	QueuedTaskData.AsyncHelper = AsyncHelper;
	QueuedTaskData.PackageBasePath = PackageBasePath;
	QueuedTasks.Enqueue(QueuedTaskData);
	QueueTaskCount = FMath::Clamp(QueueTaskCount + 1, 0, MAX_int32);

	StartQueuedTasks();

	return TTuple<UE::Interchange::FAssetImportResultRef, UE::Interchange::FSceneImportResultRef>{ AsyncHelper->AssetImportResult, AsyncHelper->SceneImportResult };
}

bool UInterchangeManager::ExportAsset(const UObject* Asset, bool bIsAutomated)
{
	return false;
}

bool UInterchangeManager::ExportScene(const UObject* World, bool bIsAutomated)
{
	return false;
}

UInterchangeSourceData* UInterchangeManager::CreateSourceData(const FString& InFileName)
{
	UInterchangeSourceData* SourceDataAsset = NewObject<UInterchangeSourceData>(GetTransientPackage(), NAME_None);
	if(!InFileName.IsEmpty())
	{
		SourceDataAsset->SetFilename(InFileName);
	}
	return SourceDataAsset;
}

const UClass* UInterchangeManager::GetRegisteredFactoryClass(const UClass* ClassToMake) const
{
	const UClass* BestClassToMake = nullptr;
	const UClass* Result = nullptr;

	for (const auto& Kvp : RegisteredFactoryClasses)
	{
		if (ClassToMake->IsChildOf(Kvp.Key))
		{
			// Find the factory which handles the most derived registered type 
			if (BestClassToMake == nullptr || Kvp.Key->IsChildOf(BestClassToMake))
			{
				BestClassToMake = Kvp.Key.Get();
				Result = Kvp.Value.Get();
			}
		}
	}
	return Result;
}

TSharedRef<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> UInterchangeManager::CreateAsyncHelper(const UE::Interchange::FImportAsyncHelperData& Data, const FImportAssetParameters& ImportAssetParameters)
{
	TSharedRef<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = MakeShared<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe>();
	//Copy the task data
	AsyncHelper->TaskData = Data;
	
	AsyncHelper->AssetImportResult->OnObjectDone = ImportAssetParameters.OnAssetDone;
	AsyncHelper->AssetImportResult->OnObjectDoneNative = ImportAssetParameters.OnAssetDoneNative;
	AsyncHelper->AssetImportResult->OnImportDone = ImportAssetParameters.OnAssetsImportDone;
	AsyncHelper->AssetImportResult->OnImportDoneNative = ImportAssetParameters.OnAssetsImportDoneNative;

	AsyncHelper->SceneImportResult->OnObjectDone = ImportAssetParameters.OnSceneObjectDone;
	AsyncHelper->SceneImportResult->OnObjectDoneNative = ImportAssetParameters.OnSceneObjectDoneNative;
	AsyncHelper->SceneImportResult->OnImportDone = ImportAssetParameters.OnSceneImportDone;
	AsyncHelper->SceneImportResult->OnImportDoneNative = ImportAssetParameters.OnSceneImportDoneNative;

	AsyncHelper->AssetImportResult->SetInProgress();

	return AsyncHelper;
}

void UInterchangeManager::ReleaseAsyncHelper(TWeakPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper)
{
	check(AsyncHelper.IsValid());
	ImportTasks.RemoveSingle(AsyncHelper.Pin());
	//Make sure the async helper is destroy, if not destroy its because we are canceling the import and we still have a shared ptr on it
	{
		TSharedPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelperSharedPtr = AsyncHelper.Pin();
		check(!AsyncHelperSharedPtr.IsValid() || AsyncHelperSharedPtr->bCancel);
	}

	int32 ImportTaskNumber = ImportTasks.Num();
	FString ImportTaskNumberStr = TEXT(" (") + FString::FromInt(ImportTaskNumber) + TEXT(")");
	if (ImportTaskNumber == 0)
	{
		bImportAllWithDefault = false;
		SetActiveMode(false);

		if (Notification.IsValid())
		{
			FText TitleText = NSLOCTEXT("Interchange", "Asynchronous_import_end", "Import Done");
			//TODO make sure any error are reported so we can control success or not
			const bool bSuccess = true;
			Notification->SetComplete(TitleText, FText::GetEmpty(), bSuccess);
			Notification = nullptr; //This should delete the notification
		}
	}
	else if(Notification.IsValid())
	{
		Notification->SetProgressText(FText::FromString(ImportTaskNumberStr));
	}

	//Start some task if there is some waiting
	StartQueuedTasks();
}

UInterchangeTranslatorBase* UInterchangeManager::GetTranslatorForSourceData(const UInterchangeSourceData* SourceData) const
{
	// Find the translator
	for (const UClass* TranslatorClass : RegisteredTranslatorsClass)
	{
		if (TranslatorClass->GetDefaultObject<UInterchangeTranslatorBase>()->CanImportSourceData(SourceData))
		{
			UInterchangeTranslatorBase* SourceDataTranslator = NewObject<UInterchangeTranslatorBase>(GetTransientPackage(), TranslatorClass, NAME_None);
			SourceDataTranslator->SourceData = SourceData;
			return SourceDataTranslator;
		}
	}
	return nullptr;
}

bool UInterchangeManager::WarnIfInterchangeIsActive()
{
	if (!bIsActive)
	{
		return false;
	}
	//Tell user he have to cancel the import before closing the editor
	FNotificationInfo Info(NSLOCTEXT("InterchangeManager", "WarnCannotProceed", "An import process is currently underway! Please cancel it to proceed!"));
	Info.ExpireDuration = 5.0f;
	TSharedPtr<SNotificationItem> WarnNotification = FSlateNotificationManager::Get().AddNotification(Info);
	if (WarnNotification.IsValid())
	{
		WarnNotification->SetCompletionState(SNotificationItem::CS_Fail);
	}
	return true;
}

bool UInterchangeManager::CanTranslateSourceDataWithPayloadInterface(const UInterchangeSourceData* SourceData, const UClass* PayloadInterfaceClass) const
{
	if (GetTranslatorSupportingPayloadInterfaceForSourceData(SourceData, PayloadInterfaceClass))
	{
		return true;
	}
	return false;
}

UInterchangeTranslatorBase* UInterchangeManager::GetTranslatorSupportingPayloadInterfaceForSourceData(const UInterchangeSourceData* SourceData, const UClass* PayloadInterfaceClass) const
{
	// Find the translator
	for (const UClass* TranslatorClass : RegisteredTranslatorsClass)
	{
		if (TranslatorClass->ImplementsInterface(PayloadInterfaceClass) &&
			TranslatorClass->GetDefaultObject<UInterchangeTranslatorBase>()->CanImportSourceData(SourceData))
		{
			UInterchangeTranslatorBase* SourceDataTranslator = NewObject<UInterchangeTranslatorBase>(GetTransientPackage(), TranslatorClass, NAME_None);
			SourceDataTranslator->SourceData = SourceData;
			return SourceDataTranslator;
		}
	}
	return nullptr;
}

void UInterchangeManager::RegisterTextureOnlyTranslatorClass(const UClass* TranslatorClass)
{
	if (TranslatorClass)
	{
		TextureOnlyTranslatorClass.Add(TranslatorClass);
	}
}

bool UInterchangeManager::IsTranslatorClassForTextureOnly(const UClass* TranslatorClass) const
{
	return TextureOnlyTranslatorClass.Contains(TranslatorClass);
}

bool UInterchangeManager::IsAttended()
{
	if (FApp::IsGame())
	{
		return false;
	}
	if (FApp::IsUnattended())
	{
		return false;
	}
	return true;
}

void UInterchangeManager::FindPipelineCandidate(TArray<UClass*>& PipelineCandidates)
{
	//Find in memory pipeline class
	for (TObjectIterator< UClass > ClassIt; ClassIt; ++ClassIt)
	{
		UClass* Class = *ClassIt;
		// Only interested in native C++ classes
// 		if (!Class->IsNative())
// 		{
// 			continue;
// 		}
		// Ignore deprecated
		if (Class->HasAnyClassFlags(CLASS_Deprecated | CLASS_NewerVersionExists))
		{
			continue;
		}

		// Check this class is a subclass of Base and not the base itself
		if (Class == UInterchangePipelineBase::StaticClass() || !Class->IsChildOf(UInterchangePipelineBase::StaticClass()))
		{
			continue;
		}

		//We found a candidate
		PipelineCandidates.AddUnique(Class);
	}

	//Blueprint and python script discoverability is available only if we compile with the engine
	// Load the asset registry module
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked< FAssetRegistryModule >(FName("AssetRegistry"));
	IAssetRegistry& AssetRegistry = AssetRegistryModule.Get();

	TArray< FString > ContentPaths;
	ContentPaths.Add(TEXT("/Game"));
	//TODO do we have an other alternative, this call is synchronous and will wait unitl the registry database have finish the initial scan. If there is a lot of asset it can take multiple second the first time we call it.
	AssetRegistry.ScanPathsSynchronous(ContentPaths);

	FName BaseClassName = UInterchangePipelineBase::StaticClass()->GetFName();

	// Use the asset registry to get the set of all class names deriving from Base
	TSet< FName > DerivedNames;
	{
		TArray< FName > BaseNames;
		BaseNames.Add(BaseClassName);

		TSet< FName > Excluded;
		AssetRegistry.GetDerivedClassNames(BaseNames, Excluded, DerivedNames);
	}

	FARFilter Filter;
	Filter.ClassNames.Add(UBlueprint::StaticClass()->GetFName());
	Filter.bRecursiveClasses = true;
	Filter.bRecursivePaths = true;

	TArray< FAssetData > AssetList;
	AssetRegistry.GetAssets(Filter, AssetList);

	// Iterate over retrieved blueprint assets
	for (const FAssetData& Asset : AssetList)
	{
		//Only get the asset with the native parent class using UInterchangePipelineBase
		FAssetDataTagMapSharedView::FFindTagResult GeneratedClassPath = Asset.TagsAndValues.FindTag(TEXT("GeneratedClass"));
		if (GeneratedClassPath.IsSet())
		{
			// Convert path to just the name part
			const FString ClassObjectPath = FPackageName::ExportTextPathToObjectPath(*GeneratedClassPath.GetValue());
			const FString ClassName = FPackageName::ObjectPathToObjectName(ClassObjectPath);

			// Check if this class is in the derived set
			if (!DerivedNames.Contains(*ClassName))
			{
				continue;
			}

			UBlueprint* Blueprint = Cast<UBlueprint>(Asset.GetAsset());
			check(Blueprint);
			check(Blueprint->ParentClass == UInterchangePipelineBase::StaticClass());
			PipelineCandidates.AddUnique(Blueprint->GeneratedClass);
		}
	}
}

void UInterchangeManager::CancelAllTasks()
{
	check(IsInGameThread());

	//Cancel the queued tasks, we cannot simply not do them since, there is some promise objects
	//to setup in the completion task
	const bool bCancelAllTasks = true;
	StartQueuedTasks(bCancelAllTasks);

	//Set the cancel state on all running tasks
	int32 ImportTaskCount = ImportTasks.Num();
	for (int32 TaskIndex = 0; TaskIndex < ImportTaskCount; ++TaskIndex)
	{
		TSharedPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = ImportTasks[TaskIndex];
		if (AsyncHelper.IsValid())
		{
			AsyncHelper->InitCancel();
		}
	}
	//Tasks should all finish quite fast now
};

void UInterchangeManager::CancelAllTasksSynchronously()
{
	//Start the cancel process by cancelling all current task
	CancelAllTasks();

	//Now wait for each task to be completed on the main thread
	while (ImportTasks.Num() > 0)
	{
		int32 ImportTaskCount = ImportTasks.Num();
		TSharedPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> AsyncHelper = ImportTasks[0];
		if (AsyncHelper.IsValid())
		{
			//Cancel any on going interchange activity this is blocking but necessary.
			AsyncHelper->CancelAndWaitUntilDoneSynchronously();
			ensure(ImportTaskCount > ImportTasks.Num());
			TWeakPtr<UE::Interchange::FImportAsyncHelper, ESPMode::ThreadSafe> WeakAsyncHelper = AsyncHelper;
			//free the async helper
			AsyncHelper = nullptr;
			//We verify that the weak pointer is invalid after releasing the async helper
			ensure(!WeakAsyncHelper.IsValid());
		}
	}
}

void UInterchangeManager::SetActiveMode(bool IsActive)
{
	if (bIsActive == IsActive)
	{
		return;
	}

	bIsActive = IsActive;
	if (bIsActive)
	{
		ensure(!NotificationTickHandle.IsValid());
		NotificationTickHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("InterchangeManagerTickHandle"), 0.1f, [this](float)
		{
			if (Notification.IsValid() && Notification->GetPromptAction() == EAsyncTaskNotificationPromptAction::Cancel)
			{
				CancelAllTasks();
			}
			return true;
		});
	}
	else
	{
		FTSTicker::GetCoreTicker().RemoveTicker(NotificationTickHandle);
		NotificationTickHandle.Reset();
	}
}