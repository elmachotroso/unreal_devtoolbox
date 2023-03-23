// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "Trace/Analyzer.h"
#include "Containers/UnrealString.h"
#include "Model/TimingProfilerPrivate.h"

namespace TraceServices
{

class IAnalysisSession;
class FThreadProvider;

class FCpuProfilerAnalyzer
	: public UE::Trace::IAnalyzer
{
public:
	FCpuProfilerAnalyzer(IAnalysisSession& Session, FTimingProfilerProvider& TimingProfilerProvider, FThreadProvider& InThreadProvider);
	~FCpuProfilerAnalyzer();
	virtual void OnAnalysisBegin(const FOnAnalysisContext& Context) override;
	virtual bool OnEvent(uint16 RouteId, EStyle Style, const FOnEventContext& Context) override;

private:
	struct FEventScopeState
	{
		uint64 StartCycle;
		uint32 EventTypeId;
	};

	struct FPendingEvent
	{
		uint64 Cycle;
		uint32 TimerId;
	};

	struct FThreadState
	{
		TArray<FEventScopeState> ScopeStack;
		TArray<FPendingEvent> PendingEvents;
		FTimingProfilerProvider::TimelineInternal* Timeline;
		uint64 LastCycle = 0;
	};

	void OnCpuScopeEnter(const FOnEventContext& Context);
	void OnCpuScopeLeave(const FOnEventContext& Context);
	uint32 DefineTimer(uint32 SpecId, const TCHAR* ScopeName, const TCHAR* File, uint32 Line, bool bMergeByName); // returns the TimerId
	FThreadState& GetThreadState(uint32 ThreadId);
	uint64 ProcessBuffer(const FEventTime& EventTime, FThreadState& ThreadState, const uint8* BufferPtr, uint32 BufferSize);

	enum : uint16
	{
		RouteId_EventSpec,
		RouteId_EventBatch,
		RouteId_EndThread,
		RouteId_EndCapture,
		RouteId_CpuScope,
		RouteId_ChannelAnnounce,
		RouteId_ChannelToggle,
	};

	IAnalysisSession& Session;
	FTimingProfilerProvider& TimingProfilerProvider;
	FThreadProvider& ThreadProvider;
	TMap<uint32, FThreadState*> ThreadStatesMap;
	TMap<uint32, uint32> SpecIdToTimerIdMap;
	TMap<const TCHAR*, uint32> ScopeNameToTimerIdMap;
	uint64 TotalEventSize = 0;
	uint64 TotalScopeCount = 0;
	double BytesPerScope = 0.0;
};

} // namespace TraceServices
