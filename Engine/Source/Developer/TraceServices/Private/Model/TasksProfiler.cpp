// Copyright Epic Games, Inc. All Rights Reserved.

#include "TraceServices/Model/TasksProfiler.h"
#include "Model/TasksProfilerPrivate.h"
#include "AnalysisServicePrivate.h"
#include "Misc/ScopeLock.h"
#include "Misc/Timespan.h"
#include "Algo/BinarySearch.h"
#include "Common/Utils.h"
#include "Async/TaskGraphInterfaces.h"

namespace TraceServices
{
	FTasksProvider::FTasksProvider(IAnalysisSession& InSession)
		: Session(InSession)
		, CounterProvider(EditCounterProvider(Session))
	{
#if 0
		////////////////////////////////
		// tests

		FAnalysisSessionEditScope _(Session);

		const int32 ValidThreadId = 42;
		const int32 InvalidThreadId = 0;
		const double InvalidTimestamp = 0;

		TArray64<TaskTrace::FId>& Thread = ExecutionThreads.FindOrAdd(ValidThreadId);

		auto MockTaskExecution = [this, Thread = &Thread](TaskTrace::FId TaskId, double StartedTimestamp, double FinishedTimestamp) -> FTaskInfo&
		{
			FTaskInfo& Task = GetOrCreateTask(TaskId);
			Task.Id = TaskId;
			Task.StartedTimestamp = StartedTimestamp;
			Task.FinishedTimestamp = FinishedTimestamp;
			Thread->Add(Task.Id);
			return Task;
		};

		FTaskInfo& Task1 = MockTaskExecution(0, 5, 10);
		FTaskInfo& Task2 = MockTaskExecution(1, 15, 20);
		
		check(GetTask(InvalidThreadId, InvalidTimestamp) == nullptr);
		check(GetTask(ValidThreadId, InvalidTimestamp) == nullptr);
		check(GetTask(ValidThreadId, 5) == &Task1);
		check(GetTask(ValidThreadId, 7) == &Task1);
		check(GetTask(ValidThreadId, 10) == nullptr);
		check(GetTask(ValidThreadId, 12) == nullptr);
		check(GetTask(ValidThreadId, 17) == &Task2);
		check(GetTask(ValidThreadId, 22) == nullptr);

		// reset
		FirstTaskId = TaskTrace::InvalidId;
		ExecutionThreads.Empty();
		Tasks.Empty();
#endif
	}

	void FTasksProvider::CreateCounters()
	{
		check(bCountersCreated == false);
		FAnalysisSessionEditScope _(Session);

		WaitingForPrerequisitesTasksCounter = CounterProvider.CreateCounter();
		WaitingForPrerequisitesTasksCounter->SetName(TEXT("Tasks::WaitingForPrerequisitesTasks"));
		WaitingForPrerequisitesTasksCounter->SetDescription(TEXT("Tasks: the number of tasks waiting for prerequisites (blocked by dependency)"));
		WaitingForPrerequisitesTasksCounter->SetIsFloatingPoint(false);

		TaskLatencyCounter = CounterProvider.CreateCounter();
		TaskLatencyCounter->SetName(TEXT("Tasks::TaskLatency"));
		TaskLatencyCounter->SetDescription(TEXT("Tasks: tasks latency - the time from scheduling to execution start"));
		TaskLatencyCounter->SetIsFloatingPoint(true);

		ScheduledTasksCounter = CounterProvider.CreateCounter();
		ScheduledTasksCounter->SetName(TEXT("Tasks::ScheduledTasks"));
		ScheduledTasksCounter->SetDescription(TEXT("Tasks: number of scheduled tasks excluding named threads (the size of the queue)"));
		ScheduledTasksCounter->SetIsFloatingPoint(false);

		NamedThreadsScheduledTasksCounter = CounterProvider.CreateCounter();
		NamedThreadsScheduledTasksCounter->SetName(TEXT("Tasks::NamedThreadsScheduledTasks"));
		NamedThreadsScheduledTasksCounter->SetDescription(TEXT("Tasks: number of scheduled tasks for named threads"));
		NamedThreadsScheduledTasksCounter->SetIsFloatingPoint(false);

		RunningTasksCounter = CounterProvider.CreateCounter();
		RunningTasksCounter->SetName(TEXT("Tasks::RunningTasks"));
		RunningTasksCounter->SetDescription(TEXT("Tasks: level of parallelism - the number of tasks being executed"));
		RunningTasksCounter->SetIsFloatingPoint(false);

		ExecutionTimeCounter = CounterProvider.CreateCounter();
		ExecutionTimeCounter->SetName(TEXT("Tasks::ExecutionTime"));
		ExecutionTimeCounter->SetDescription(TEXT("Tasks: execution time"));
		ExecutionTimeCounter->SetIsFloatingPoint(true);

		bCountersCreated = true;
	}

	void FTasksProvider::Init(uint32 InVersion)
	{
		Version = InVersion;

		if (!bCountersCreated)
		{
			CreateCounters();
		}
	}

	void FTasksProvider::TaskCreated(TaskTrace::FId TaskId, double Timestamp, uint32 ThreadId)
	{
		UE_LOG(LogTraceServices, Verbose, TEXT("TaskCreated(TaskId: %d, Timestamp %.6f)"), TaskId, Timestamp);

		InitTaskIdToIndexConversion(TaskId);

		FTaskInfo* Task = TryGetOrCreateTask(TaskId);
		if (Task == nullptr)
		{
			UE_LOG(LogTraceServices, Log, TEXT("TaskCreated(TaskId %d, Timestamp %.6f) skipped"), TaskId, Timestamp);
			return;
		}

		checkf(Task->CreatedTimestamp == FTaskInfo::InvalidTimestamp, TEXT("%d"), TaskId);

		Task->Id = TaskId;
		Task->CreatedTimestamp = Timestamp;
		Task->CreatedThreadId = ThreadId;
	}

	void FTasksProvider::TaskLaunched(TaskTrace::FId TaskId, const TCHAR* DebugName, bool bTracked, int32 ThreadToExecuteOn, double Timestamp, uint32 ThreadId)
	{
		UE_LOG(LogTraceServices, Verbose, TEXT("TaskLaunched(TaskId: %d, DebugName: %s, bTracked: %d, Timestamp %.6f)"), TaskId, DebugName, bTracked, Timestamp);

		InitTaskIdToIndexConversion(TaskId);

		FTaskInfo* Task = TryGetOrCreateTask(TaskId);
		if (Task == nullptr)
		{
			UE_LOG(LogTraceServices, Log, TEXT("TaskLaunched(TaskId %d, DebugName %s, bTracked %d, Timestamp %.6f) skipped"), TaskId, DebugName, bTracked, Timestamp);
			return;
		}

		checkf(Task->LaunchedTimestamp == FTaskInfo::InvalidTimestamp, TEXT("%d"), TaskId);
			
		if (Task->Id == TaskTrace::InvalidId) // created and launched in one go
		{
			Task->Id = TaskId;
			Task->CreatedTimestamp = Timestamp;
			Task->CreatedThreadId = ThreadId;
		}

		Task->DebugName = DebugName;
		Task->bTracked = bTracked;
		Task->ThreadToExecuteOn = ThreadToExecuteOn;
		Task->LaunchedTimestamp = Timestamp;
		Task->LaunchedThreadId = ThreadId;

		if (!bCountersCreated)
		{
			CreateCounters();
		}

		WaitingForPrerequisitesTasksCounter->SetValue(Timestamp, ++WaitingForPrerequisitesTasksNum);
	}

	void FTasksProvider::TaskScheduled(TaskTrace::FId TaskId, double Timestamp, uint32 ThreadId)
	{
		InitTaskIdToIndexConversion(TaskId);

		if (!TryRegisterEvent(TEXT("TaskScheduled"), TaskId, &FTaskInfo::ScheduledTimestamp, Timestamp, &FTaskInfo::ScheduledThreadId, ThreadId))
		{
			return;
		}

		if (!bCountersCreated)
		{
			CreateCounters();
		}

		WaitingForPrerequisitesTasksCounter->SetValue(Timestamp, --WaitingForPrerequisitesTasksNum);
		if (IsNamedThread(TryGetTask(TaskId)->ThreadToExecuteOn))
		{
			NamedThreadsScheduledTasksCounter->SetValue(Timestamp, ++ScheduledTasksNum);
		}
		else
		{
			ScheduledTasksCounter->SetValue(Timestamp, ++ScheduledTasksNum);
		}
	}

	void FTasksProvider::SubsequentAdded(TaskTrace::FId TaskId, TaskTrace::FId SubsequentId, double Timestamp, uint32 ThreadId)
	{
		InitTaskIdToIndexConversion(TaskId);

		// when FGraphEvent is used to wait for a notification, it doesn't have an associated task and so is not created or launched. 
		// In this case we need to create it and initialise it before registering the completion event
		FTaskInfo* Task = TryGetOrCreateTask(TaskId);
		if (Task == nullptr)
		{
			UE_LOG(LogTraceServices, Log, TEXT("SubsequentAdded(TaskId %d, SubsequentId %d, Timestamp %.6f) skipped"), TaskId, SubsequentId, Timestamp);
			return;
		}

		Task->Id = TaskId;

		AddRelative(TEXT("Subsequent"), TaskId, &FTaskInfo::Subsequents, SubsequentId, Timestamp, ThreadId);

		// make a backward link from the subsequent task to this task (prerequisite)
		AddRelative(TEXT("Prerequisite"), SubsequentId, &FTaskInfo::Prerequisites, TaskId, Timestamp, ThreadId);
	}

	void FTasksProvider::TaskStarted(TaskTrace::FId TaskId, double Timestamp, uint32 ThreadId)
	{
		InitTaskIdToIndexConversion(TaskId);

		if (!TryRegisterEvent(TEXT("TaskStarted"), TaskId, &FTaskInfo::StartedTimestamp, Timestamp, &FTaskInfo::StartedThreadId, ThreadId))
		{
			return;
		}

		ExecutionThreads.FindOrAdd(ThreadId).Add(TaskId);

		FTaskInfo* Task = TryGetTask(TaskId);
		check(Task != nullptr);

		if (!bCountersCreated)
		{
			CreateCounters();
		}

		if (IsNamedThread(Task->ThreadToExecuteOn))
		{
			NamedThreadsScheduledTasksCounter->SetValue(Timestamp, --ScheduledTasksNum);
		}
		else
		{
			ScheduledTasksCounter->SetValue(Timestamp, --ScheduledTasksNum);
		}
		RunningTasksCounter->SetValue(Timestamp, ++RunningTasksNum);

		TaskLatencyCounter->SetValue(Timestamp, Task->StartedTimestamp - Task->ScheduledTimestamp);
	}

	void FTasksProvider::NestedAdded(TaskTrace::FId TaskId, TaskTrace::FId NestedId, double Timestamp, uint32 ThreadId)
	{
		InitTaskIdToIndexConversion(TaskId);

		AddRelative(TEXT("Nested"), TaskId, &FTaskInfo::NestedTasks, NestedId, Timestamp, ThreadId);

		FTaskInfo* Task = TryGetTask(NestedId);
		if (Task != nullptr)
		{
			Task->ParentOfNestedTask = MakeUnique<FTaskInfo::FRelationInfo>(TaskId, Timestamp, ThreadId);
		}
	}

	void FTasksProvider::TaskFinished(TaskTrace::FId TaskId, double Timestamp)
	{
		InitTaskIdToIndexConversion(TaskId);

		if (!TryRegisterEvent(TEXT("TaskFinished"), TaskId, &FTaskInfo::FinishedTimestamp, Timestamp))
		{
			return;
		}

		if (!bCountersCreated)
		{
			CreateCounters();
		}

		RunningTasksCounter->SetValue(Timestamp, --RunningTasksNum);
	
		FTaskInfo* Task = TryGetTask(TaskId);
		check(Task != nullptr);
		ExecutionTimeCounter->SetValue(Timestamp, Task->FinishedTimestamp - Task->StartedTimestamp);
	}

	void FTasksProvider::TaskCompleted(TaskTrace::FId TaskId, double Timestamp, uint32 ThreadId)
	{
		InitTaskIdToIndexConversion(TaskId);

		// when FGraphEvent is used to wait for a notification, it doesn't have an associated task and so is not created or launched. 
		// In this case we need to create it and initialise it before registering the completion event
		FTaskInfo* Task = TryGetOrCreateTask(TaskId);
		if (Task == nullptr)
		{
			UE_LOG(LogTraceServices, Log, TEXT("TaskCompleted(TaskId %d, Timestamp %.6f) skipped"), TaskId, Timestamp);
			return;
		}

		Task->Id = TaskId;

		TryRegisterEvent(TEXT("TaskCompleted"), TaskId, &FTaskInfo::CompletedTimestamp, Timestamp, &FTaskInfo::CompletedThreadId, ThreadId);
	}

	void FTasksProvider::WaitingStarted(TArray<TaskTrace::FId> InTasks, double Timestamp, uint32 ThreadId)
	{
		FWaitingForTasks Waiting;
		Waiting.Tasks = MoveTemp(InTasks);
		Waiting.StartedTimestamp = Timestamp;

		WaitingThreads.FindOrAdd(ThreadId).Add(MoveTemp(Waiting));
	}

	void FTasksProvider::WaitingFinished(double Timestamp, uint32 ThreadId)
	{
		TArray64<FWaitingForTasks>* Thread = WaitingThreads.Find(ThreadId);
		if (Thread == nullptr)
		{
			UE_LOG(LogTraceServices, Log, TEXT("WaitingFinished task event (Thread %d, Timestamp %.6f) skipped."), ThreadId, Timestamp);
			return;
		}

		Thread->Last().FinishedTimestamp = Timestamp;
	}

	void FTasksProvider::InitTaskIdToIndexConversion(TaskTrace::FId InFirstTaskId)
	{
		check(InFirstTaskId != TaskTrace::InvalidId);
		if (FirstTaskId == TaskTrace::InvalidId)
		{
			FirstTaskId = InFirstTaskId;
		}
	}

	int64 FTasksProvider::GetTaskIndex(TaskTrace::FId TaskId) const
	{
		return (int64)TaskId - FirstTaskId;
	}

	const FTaskInfo* FTasksProvider::TryGetTask(TaskTrace::FId TaskId) const
	{
		check(TaskId != TaskTrace::InvalidId);
		int64 TaskIndex = GetTaskIndex(TaskId);
		return Tasks.IsValidIndex(TaskIndex) ? &Tasks[TaskIndex] : nullptr;
	}

	FTaskInfo* FTasksProvider::TryGetTask(TaskTrace::FId TaskId)
	{
		return const_cast<FTaskInfo*>(const_cast<const FTasksProvider*>(this)->TryGetTask(TaskId)); // reuse the const version
	}

	FTaskInfo* FTasksProvider::TryGetOrCreateTask(TaskTrace::FId TaskId)
	{
		int64 TaskIndex = GetTaskIndex(TaskId);
		// traces can race, it's possible a trace with `TaskId = X` can come first, initialize `FirstTaskId` and only then a trace with 
		// `TaskId = X - 1` arrives. This will produce `TaskIndex < 0`. Such traces can happen only at the very beginning of the capture 
		// and are ignored
		if (TaskIndex < 0)
		{
			return nullptr;
		}

		if (TaskIndex >= Tasks.Num())
		{
			Tasks.AddDefaulted(TaskIndex - Tasks.Num() + 1);
		}

		return &Tasks[TaskIndex];
	}

	bool FTasksProvider::IsNamedThread(int32 Thread)
	{
		return ENamedThreads::GetThreadIndex((ENamedThreads::Type)Thread) != ENamedThreads::AnyThread;
	}

	bool FTasksProvider::TryRegisterEvent(const TCHAR* EventName, TaskTrace::FId TaskId, double FTaskInfo::* TimestampPtr, double TimestampValue, uint32 FTaskInfo::* ThreadIdPtr/* = nullptr*/, uint32 ThreadIdValue/* = 0*/)
	{
		UE_LOG(LogTraceServices, Verbose, TEXT("%s(TaskId: %d, Timestamp %.6f)"), EventName, TaskId, TimestampValue);

		FTaskInfo* Task = TryGetTask(TaskId);
		if (Task == nullptr)
		{
			UE_LOG(LogTraceServices, Log, TEXT("%s(TaskId %d, Timestamp %.6f) skipped"), EventName, TaskId, TimestampValue);
			return false;
		}

		checkf(Task->*TimestampPtr == FTaskInfo::InvalidTimestamp, TEXT("%s: TaskId %d, old TS %.6f, new TS %.6f"), EventName, TaskId, Task->*TimestampPtr, TimestampValue);
		Task->*TimestampPtr = TimestampValue;
		if (ThreadIdPtr != nullptr)
		{
			Task->*ThreadIdPtr = ThreadIdValue;
		}

		return true;
	}

	void FTasksProvider::AddRelative(const TCHAR* RelationType, TaskTrace::FId TaskId, TArray<FTaskInfo::FRelationInfo> FTaskInfo::* RelationsPtr, TaskTrace::FId RelativeId, double Timestamp, uint32 ThreadId)
	{
		UE_LOG(LogTraceServices, Verbose, TEXT("%s (%d) added to TaskId: %d, Timestamp %.6f)"), RelationType, RelativeId, TaskId, Timestamp);

		InitTaskIdToIndexConversion(TaskId);

		FTaskInfo* Task = TryGetTask(TaskId);
		if (Task == nullptr)
		{
			UE_LOG(LogTraceServices, Log, TEXT("Add%s(TaskId %d, OtherId: %d, Timestamp %.6f) skipped"), RelationType, TaskId, RelativeId, Timestamp);
			return;
		}

		(Task->*RelationsPtr).Emplace(RelativeId, Timestamp, ThreadId);
	}

	/////////////////////////////////////////////////////////////////////////////////
	// ITasksProvider impl

	const FTaskInfo* FTasksProvider::TryGetTask(uint32 ThreadId, double Timestamp) const
	{
		const TArray64<TaskTrace::FId>* Thread = ExecutionThreads.Find(ThreadId);
		if (Thread == nullptr)
		{
			return nullptr;
		}

		int64 NextTaskIndex = Algo::LowerBound(*Thread, Timestamp, 
			[this](TaskTrace::FId TaskId, double Timestamp)
			{
				const FTaskInfo* Task = TryGetTask(TaskId);
				return Task != nullptr && Task->StartedTimestamp <= Timestamp;
			}
		);

		if (NextTaskIndex == 0)
		{
			return nullptr;
		}

		TaskTrace::FId TaskId = (*Thread)[NextTaskIndex - 1];
		const FTaskInfo* Task = TryGetTask(TaskId);
		return Task != nullptr && Task->FinishedTimestamp > Timestamp ? Task : nullptr;
	}

	const FWaitingForTasks* FTasksProvider::TryGetWaiting(const TCHAR* TimerName, uint32 ThreadId, double Timestamp) const
	{
		if (FCString::Strcmp(TimerName, TEXT("WaitUntilTasksComplete")) != 0 && 
			FCString::Strcmp(TimerName, TEXT("GameThreadWaitForTask")) != 0 &&
			FCString::Strcmp(TimerName, TEXT("Tasks::Wait")) != 0 &&
			FCString::Strcmp(TimerName, TEXT("Tasks::BusyWait")) != 0)
		{
			return nullptr;
		}

		const TArray64<FWaitingForTasks>* Thread = WaitingThreads.Find(ThreadId);
		if (Thread == nullptr)
		{
			return nullptr;
		}

		int64 NextWaitingIndex = Algo::LowerBound(*Thread, Timestamp,
			[this](const FWaitingForTasks& Waiting, double Timestamp)
			{
				return Waiting.StartedTimestamp <= Timestamp;
			}
		);

		if (NextWaitingIndex == 0)
		{
			return nullptr;
		}

		const FWaitingForTasks& Waiting = (*Thread)[NextWaitingIndex - 1];
		return Waiting.FinishedTimestamp > Timestamp || Waiting.FinishedTimestamp == FTaskInfo::InvalidTimestamp ? &Waiting : nullptr;
	}

	int64 FTasksProvider::GetNumTasks() const
	{
		return Tasks.Num();
	}

	void FTasksProvider::EnumerateTasks(double StartTime, double EndTime, TaskCallback Callback) const
	{
		for (const TPair<uint32, TArray64<TaskTrace::FId>>& KeyValue : ExecutionThreads)
		{
			const TArray64<TaskTrace::FId>& Thread = KeyValue.Value;
			
			// find the first task with `StartedTimestamp <= StartTime`
			int64 TaskIndex = Algo::LowerBound(Thread, StartTime, 
				[this](TaskTrace::FId TaskId, double Timestamp)
				{
					const FTaskInfo* Task = TryGetTask(TaskId);
					return Task != nullptr && Task->StartedTimestamp <= Timestamp;
				}
			);

			// check if there's a previous task whose execution overlaps `StartTime`
			if (TaskIndex != 0)
			{
				const FTaskInfo* Task = TryGetTask(Thread[TaskIndex - 1]);
				if (Task != nullptr && Task->FinishedTimestamp > StartTime)
				{
					--TaskIndex;
				}
			}

			if (TaskIndex == Thread.Num())
			{
				continue; // all tasks on this thread are before StartTime so nothing to do here, go to the next thread
			}

			// report all tasks whose execution overlaps [StartTime, EndTime]
			const FTaskInfo* Task = TryGetTask(Thread[TaskIndex]);
			while (Task != nullptr && Task->StartedTimestamp <= EndTime && Callback(*Task) != ETaskEnumerationResult::Stop && TaskIndex < Thread.Num() - 1)
			{
				++TaskIndex;
				Task = TryGetTask(Thread[TaskIndex]);
			}
		}
	}
}
