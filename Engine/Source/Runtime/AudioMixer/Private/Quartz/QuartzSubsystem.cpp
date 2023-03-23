// Copyright Epic Games, Inc. All Rights Reserved.

#include "Quartz/QuartzSubsystem.h"

#include "Quartz/QuartzMetronome.h"
#include "Quartz/AudioMixerClockManager.h"
#include "Sound/QuartzQuantizationUtilities.h"
#include "ProfilingDebugging/CountersTrace.h"
#include "Stats/Stats.h"

#include "AudioDevice.h"
#include "AudioMixerDevice.h"

static int32 MaxQuartzSubscribersToUpdatePerTickCvar = -1;
FAutoConsoleVariableRef CVarMaxQuartzSubscribersToUpdatePerTick(
	TEXT("au.Quartz.MaxSubscribersToUpdatePerTick"),
	MaxQuartzSubscribersToUpdatePerTickCvar,
	TEXT("Limits the number of Quartz subscribers to update per Tick.\n")
	TEXT("<= 0: No Limit, >= 1: Limit"),
	ECVF_Default);

static int32 SimulateNoAudioDeviceCvar = 0;
FAutoConsoleVariableRef CVarSimulateNoAudioDevice(
	TEXT("au.Quartz.SimulateNoAudioDevice"),
	SimulateNoAudioDeviceCvar,
	TEXT("If enabled, the QuartzSubsystem will assume no audio device, and will run new clocks in headless mode.\n")
	TEXT("0: Not Enabled, 1: Enabled"),
	ECVF_Default);


static FAudioDevice* GetAudioDeviceUsingWorldContext(const UObject* WorldContextObject)
{
	if (nullptr == WorldContextObject)
	{
		return nullptr;
	}

	UWorld* ThisWorld = GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::LogAndReturnNull);
	if (!ThisWorld || !ThisWorld->bAllowAudioPlayback || ThisWorld->GetNetMode() == NM_DedicatedServer)
	{
		return nullptr;
	}

	return ThisWorld->GetAudioDeviceRaw();
}


static Audio::FMixerDevice* GetAudioMixerDeviceUsingWorldContext(const UObject* WorldContextObject)
{
	if (FAudioDevice* AudioDevice = GetAudioDeviceUsingWorldContext(WorldContextObject))
	{
		if (!AudioDevice->IsAudioMixerEnabled())
		{
			return nullptr;
		}
		else
		{
			return static_cast<Audio::FMixerDevice*>(AudioDevice);
		}
	}
	return nullptr;
}


UQuartzSubsystem::UQuartzSubsystem()
{
}

UQuartzSubsystem::~UQuartzSubsystem()
{
}

void UQuartzSubsystem::BeginDestroy()
{
	Super::BeginDestroy();

	// force un-subscribe all Quartz tickable objects
	TArray<FQuartzTickableObject*> SubscribersCopy = QuartzTickSubscribers;
	for (FQuartzTickableObject* Entry : SubscribersCopy)
	{
		Entry->Shutdown();
	}
	QuartzTickSubscribers.Reset();

	SubsystemClockManager.Shutdown();
	SubsystemClockManager.Flush();
}

bool UQuartzSubsystem::DoesSupportWorldType(EWorldType::Type WorldType) const
{
	return Super::DoesSupportWorldType(WorldType) || WorldType == EWorldType::EditorPreview;
}


void UQuartzSubsystem::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	TRACE_CPUPROFILER_EVENT_SCOPE(QuartzSubsystem::Tick);


	SubsystemClockManager.LowResoultionUpdate(DeltaTime);

	const int32 NumSubscribers = QuartzTickSubscribers.Num();

	if (MaxQuartzSubscribersToUpdatePerTickCvar <= 0 || NumSubscribers <= MaxQuartzSubscribersToUpdatePerTickCvar)
	{
		TArray<FQuartzTickableObject*> SubscribersCopy = QuartzTickSubscribers;

		// we can afford to update ALL subscribers
		for (FQuartzTickableObject* Entry : SubscribersCopy)
		{
			if (Entry && Entry->QuartzIsTickable())
			{
				Entry->QuartzTick(DeltaTime);
			}
		}

		UpdateIndex = 0;
	}
	else
	{
		// only update up to our limit
		for (int i = 0; i < MaxQuartzSubscribersToUpdatePerTickCvar; ++i)
		{
			FQuartzTickableObject* CurrentSubscriber = QuartzTickSubscribers[UpdateIndex];
			if (!ensure(CurrentSubscriber))
			{
				continue;
			}

			if (CurrentSubscriber->QuartzIsTickable())
			{
				CurrentSubscriber->QuartzTick(DeltaTime);
			}

			if (++UpdateIndex == NumSubscribers)
			{
				UpdateIndex = 0;
			}
		}
	}
}

bool UQuartzSubsystem::IsTickable() const
{
	const int32 NumSubscribers = QuartzTickSubscribers.Num();
	const int32 NumClocks = SubsystemClockManager.GetNumClocks();
	const bool bHasTickSubscribers = NumSubscribers > 0;
	const bool bIsManagingClocks = NumClocks > 0;

	TRACE_INT_VALUE(TEXT("QuartzSubsystem::NumClocks"), NumClocks);
	TRACE_INT_VALUE(TEXT("QuartzSubsystem::NumSubscribers"), NumSubscribers);

	// if our manager has clocks, we need to tick
	if (bIsManagingClocks)
	{
		return true;
	}
	// if our manager has no clocks, and we have no ClockHandle subscribers, we don't need to tick
	else if (!bHasTickSubscribers)
	{
		return false;
	}

	// if our manager has no clocks, and none of our subscribers are tickable, we don't need to tick
	for (const FQuartzTickableObject * Entry : QuartzTickSubscribers)
	{
		if (Entry && Entry->QuartzIsTickable())
		{
			return true;
		}
	}

	return false;
}

TStatId UQuartzSubsystem::GetStatId() const
{
	RETURN_QUICK_DECLARE_CYCLE_STAT(UQuartzSubsystem, STATGROUP_Tickables);
}


void UQuartzSubsystem::SubscribeToQuartzTick(FQuartzTickableObject * InObjectToTick)
{
	if (!InObjectToTick)
	{
		return;
	}

	QuartzTickSubscribers.AddUnique(InObjectToTick);
}


void UQuartzSubsystem::UnsubscribeFromQuartzTick(FQuartzTickableObject * InObjectToTick)
{
	if (!InObjectToTick)
	{
		return;
	}

	QuartzTickSubscribers.RemoveSingleSwap(InObjectToTick);
}


UQuartzSubsystem* UQuartzSubsystem::Get(UWorld* World)
{
	if (World)
	{
		return World->GetSubsystem<UQuartzSubsystem>();
	}

	return nullptr;
}


TSharedPtr<Audio::FShareableQuartzCommandQueue, ESPMode::ThreadSafe> UQuartzSubsystem::CreateQuartzCommandQueue()
{
	return MakeShared<Audio::FShareableQuartzCommandQueue, ESPMode::ThreadSafe>();
}



Audio::FQuartzQuantizedRequestData UQuartzSubsystem::CreateDataDataForSchedulePlaySound(UQuartzClockHandle* InClockHandle, const FOnQuartzCommandEventBP& InDelegate, const FQuartzQuantizationBoundary& InQuantizationBoundary)
{
	Audio::FQuartzQuantizedRequestData CommandInitInfo;

	if (!InClockHandle)
	{
		return {};
	}

	CommandInitInfo.ClockName = InClockHandle->GetClockName();
	CommandInitInfo.ClockHandleName = InClockHandle->GetHandleName();
	CommandInitInfo.QuantizationBoundary = InQuantizationBoundary;
	CommandInitInfo.QuantizedCommandPtr = MakeShared<Audio::FQuantizedPlayCommand>();
	CommandInitInfo.GameThreadSubscribers.Append(InQuantizationBoundary.GameThreadSubscribers);

	const bool bRequiresAudioDevice = CommandInitInfo.QuantizedCommandPtr->RequiresAudioDevice();
	const bool bClockManagedByAudioDevice = ClockManagerTypeMap.Contains(CommandInitInfo.ClockName)
		&& ClockManagerTypeMap[CommandInitInfo.ClockName] == EQuarztClockManagerType::AudioEngine;

	if (!(bRequiresAudioDevice && bClockManagedByAudioDevice))
	{
		return {};
	}

	if (InDelegate.IsBound())
	{
		CommandInitInfo.GameThreadDelegateID = InClockHandle->AddCommandDelegate(InDelegate, CommandInitInfo.GameThreadSubscribers);
	}

	return CommandInitInfo;
}


bool UQuartzSubsystem::IsQuartzEnabled()
{
	return true;
}


Audio::FQuartzQuantizedRequestData UQuartzSubsystem::CreateDataForTickRateChange(UQuartzClockHandle* InClockHandle, const FOnQuartzCommandEventBP& InDelegate, const Audio::FQuartzClockTickRate& InNewTickRate, const FQuartzQuantizationBoundary& InQuantizationBoundary)
{
	if (!ensure(InClockHandle))
	{
		return { };
	}

	TSharedPtr<Audio::FQuantizedTickRateChange> TickRateChangeCommandPtr = MakeShared<Audio::FQuantizedTickRateChange>();
	TickRateChangeCommandPtr->SetTickRate(InNewTickRate);

	Audio::FQuartzQuantizedRequestData CommandInitInfo;

	CommandInitInfo.ClockName = InClockHandle->GetClockName();
	CommandInitInfo.ClockHandleName = InClockHandle->GetHandleName();
	CommandInitInfo.QuantizationBoundary = InQuantizationBoundary;
	CommandInitInfo.QuantizedCommandPtr = TickRateChangeCommandPtr;
	CommandInitInfo.GameThreadSubscribers.Append(InQuantizationBoundary.GameThreadSubscribers);

	if (InDelegate.IsBound())
	{
		CommandInitInfo.GameThreadDelegateID = InClockHandle->AddCommandDelegate(InDelegate, CommandInitInfo.GameThreadSubscribers);
	}

	return CommandInitInfo;
}

Audio::FQuartzQuantizedRequestData UQuartzSubsystem::CreateDataForTransportReset(UQuartzClockHandle* InClockHandle, const FQuartzQuantizationBoundary& InQuantizationBoundary, const FOnQuartzCommandEventBP& InDelegate)
{
	if (!ensure(InClockHandle))
	{
		return { };
	}

	TSharedPtr<Audio::FQuantizedTransportReset> TransportResetCommandPtr = MakeShared<Audio::FQuantizedTransportReset>();

	Audio::FQuartzQuantizedRequestData CommandInitInfo;

	CommandInitInfo.ClockName = InClockHandle->GetClockName();
	CommandInitInfo.ClockHandleName = InClockHandle->GetHandleName();
	CommandInitInfo.QuantizationBoundary = InQuantizationBoundary;
	CommandInitInfo.QuantizedCommandPtr = TransportResetCommandPtr;
	CommandInitInfo.GameThreadSubscribers.Append(InQuantizationBoundary.GameThreadSubscribers);

	if (InDelegate.IsBound())
	{
		CommandInitInfo.GameThreadDelegateID = InClockHandle->AddCommandDelegate(InDelegate, CommandInitInfo.GameThreadSubscribers);
	}

	return CommandInitInfo;
}

Audio::FQuartzQuantizedRequestData UQuartzSubsystem::CreateDataForStartOtherClock(UQuartzClockHandle* InClockHandle, FName InClockToStart, const FQuartzQuantizationBoundary& InQuantizationBoundary, const FOnQuartzCommandEventBP& InDelegate)
{
	if (!ensure(InClockHandle))
	{
		return { };
	}

	TSharedPtr<Audio::FQuantizedOtherClockStart> TransportResetCommandPtr = MakeShared<Audio::FQuantizedOtherClockStart>();

	Audio::FQuartzQuantizedRequestData CommandInitInfo;

	CommandInitInfo.ClockName = InClockHandle->GetClockName();
	CommandInitInfo.ClockHandleName = InClockHandle->GetHandleName();
	CommandInitInfo.OtherClockName = InClockToStart;
	CommandInitInfo.QuantizationBoundary = InQuantizationBoundary;
	CommandInitInfo.QuantizedCommandPtr = TransportResetCommandPtr;
	CommandInitInfo.GameThreadSubscribers.Append(InQuantizationBoundary.GameThreadSubscribers);

	if (InDelegate.IsBound())
	{
		CommandInitInfo.GameThreadDelegateID = InClockHandle->AddCommandDelegate(InDelegate, CommandInitInfo.GameThreadSubscribers);
	}

	return CommandInitInfo;
}

UQuartzClockHandle* UQuartzSubsystem::CreateNewClock(const UObject* WorldContextObject, FName ClockName, FQuartzClockSettings InSettings, bool bOverrideSettingsIfClockExists, bool bUseAudioEngineClockManager)
{
	if (ClockName.IsNone())
	{
		return nullptr;
	}

	if (!bUseAudioEngineClockManager)
	{
		ClockManagerTypeMap.Add(ClockName, EQuarztClockManagerType::QuartzSubsystem);
	}

	// add or create clock
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject, ClockName);
	if (!ClockManager)
	{
		return nullptr;
	}

	// numerator of time signature must be >= 1
	if (InSettings.TimeSignature.NumBeats < 1)
	{
		UE_LOG(LogAudioQuartz, Warning, TEXT("Clock: (%s) is attempting to set a time signature with a Numerator < 1.  Clamping to 1 beat per bar"), *ClockName.ToString());
		InSettings.TimeSignature.NumBeats = 1;
	}

	ClockManager->GetOrCreateClock(ClockName, InSettings, bOverrideSettingsIfClockExists);

	UQuartzClockHandle* ClockHandlePtr = (UQuartzClockHandle *)(NewObject<UQuartzClockHandle>()->Init(WorldContextObject->GetWorld()));
	ClockHandlePtr->SubscribeToClock(WorldContextObject, ClockName);

	return ClockHandlePtr;
}


void UQuartzSubsystem::DeleteClockByName(const UObject* WorldContextObject, FName ClockName)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject, ClockName);
	if (!ClockManager)
	{
		return;
	}

	ClockManager->RemoveClock(ClockName);
	ClockManagerTypeMap.Remove(ClockName);
}

void UQuartzSubsystem::DeleteClockByHandle(const UObject* WorldContextObject, UQuartzClockHandle*& InClockHandle)
{
	if (InClockHandle)
	{
		DeleteClockByName(WorldContextObject, InClockHandle->GetClockName());
	}
}

UQuartzClockHandle* UQuartzSubsystem::GetHandleForClock(const UObject* WorldContextObject, FName ClockName)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject, ClockName);
	if (!ClockManager)
	{
		return nullptr;
	}

	if (ClockManager->DoesClockExist(ClockName))
	{
		UQuartzClockHandle* ClockHandlePtr = (UQuartzClockHandle *)(NewObject<UQuartzClockHandle>()->Init(WorldContextObject->GetWorld()));
		return ClockHandlePtr->SubscribeToClock(WorldContextObject, ClockName);
	}

	return nullptr;
}


bool UQuartzSubsystem::DoesClockExist(const UObject* WorldContextObject, FName ClockName)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject, ClockName);
	if (!ClockManager)
	{
		return false;
	}

	return ClockManager->DoesClockExist(ClockName);
}

bool UQuartzSubsystem::IsClockRunning(const UObject* WorldContextObject, FName ClockName)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject, ClockName);
	if (!ClockManager)
	{
		return false;
	}

	return ClockManager->IsClockRunning(ClockName);
}

float UQuartzSubsystem::GetDurationOfQuantizationTypeInSeconds(const UObject* WorldContextObject, FName ClockName, const EQuartzCommandQuantization& QuantizationType, float Multiplier)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject, ClockName);
	if (!ClockManager)
	{
		return INDEX_NONE;
	}

	return ClockManager->GetDurationOfQuantizationTypeInSeconds(ClockName, QuantizationType, Multiplier);
}

FQuartzTransportTimeStamp UQuartzSubsystem::GetCurrentClockTimestamp(const UObject* WorldContextObject, const FName& InClockName)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject, InClockName);
	if (!ClockManager)
	{
		return FQuartzTransportTimeStamp();
	}

	return ClockManager->GetCurrentTimestamp(InClockName);
}

float UQuartzSubsystem::GetEstimatedClockRunTime(const UObject* WorldContextObject, const FName& InClockName)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject, InClockName);
	if (!ClockManager)
	{
		return INDEX_NONE;
	}

	return ClockManager->GetEstimatedRunTime(InClockName);
}

float UQuartzSubsystem::GetGameThreadToAudioRenderThreadAverageLatency(const UObject* WorldContextObject)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject);
	if (!ClockManager)
	{
		return { };
	}
	return ClockManager->GetLifetimeAverageLatency();
}


float UQuartzSubsystem::GetGameThreadToAudioRenderThreadMinLatency(const UObject* WorldContextObject)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject);
	if (!ClockManager)
	{
		return { };
	}
	return ClockManager->GetMinLatency();
}


float UQuartzSubsystem::GetGameThreadToAudioRenderThreadMaxLatency(const UObject* WorldContextObject)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject);
	if (!ClockManager)
	{
		return { };
	}
	return ClockManager->GetMinLatency();
}


float UQuartzSubsystem::GetAudioRenderThreadToGameThreadAverageLatency()
{
	return GetLifetimeAverageLatency();
}


float UQuartzSubsystem::GetAudioRenderThreadToGameThreadMinLatency()
{
	return GetMinLatency();
}


float UQuartzSubsystem::GetAudioRenderThreadToGameThreadMaxLatency()
{
	return GetMaxLatency();
}


float UQuartzSubsystem::GetRoundTripAverageLatency(const UObject* WorldContextObject)
{
	// very much an estimate
	return GetAudioRenderThreadToGameThreadAverageLatency() + GetGameThreadToAudioRenderThreadAverageLatency(WorldContextObject);
}


float UQuartzSubsystem::GetRoundTripMinLatency(const UObject* WorldContextObject)
{
	return GetAudioRenderThreadToGameThreadMaxLatency() + GetGameThreadToAudioRenderThreadMaxLatency(WorldContextObject);
}


float UQuartzSubsystem::GetRoundTripMaxLatency(const UObject* WorldContextObject)
{
	return GetAudioRenderThreadToGameThreadMinLatency() + GetGameThreadToAudioRenderThreadMinLatency(WorldContextObject);
}


void UQuartzSubsystem::AddCommandToClock(const UObject* WorldContextObject, Audio::FQuartzQuantizedCommandInitInfo& InQuantizationCommandInitInfo, FName ClockName)
{
	Audio::FQuartzClockManager* ClockManager = GetManagerForClock(WorldContextObject, ClockName);
	if (!ClockManager)
	{
		return;
	}

	ClockManager->AddCommandToClock(InQuantizationCommandInitInfo);
}


Audio::FQuartzClockManager* UQuartzSubsystem::GetManagerForClock(const UObject* WorldContextObject, FName ExistingClockName)
{
	// if the enum has changed, the logic in this function needs to be updated
	ensure((int32)EQuarztClockManagerType::Count == 2);
	if (!WorldContextObject)
	{
		// Do not mutate the ClockManagerTypeMap if we do not have a world context yet
		return nullptr;
	}

	Audio::FMixerDevice* MixerDevice = nullptr;
	if (!SimulateNoAudioDeviceCvar)
	{
		MixerDevice = GetAudioMixerDeviceUsingWorldContext(WorldContextObject);
	}

	// see if this clock prefers the subsystem clock be used
	if (ClockManagerTypeMap.Contains(ExistingClockName) && (ClockManagerTypeMap[ExistingClockName] == EQuarztClockManagerType::QuartzSubsystem))
	{
		return &SubsystemClockManager;
	}
	else // otherwise 
	{
		// if the clock doesn't exist in our type map, try to use the mixer device's manager
		if (MixerDevice)
		{
			ClockManagerTypeMap.Add(ExistingClockName, EQuarztClockManagerType::AudioEngine);
			return &MixerDevice->QuantizedEventClockManager;
		}

		// no mixer device, use the subsystem's manager
		ClockManagerTypeMap.Add(ExistingClockName, EQuarztClockManagerType::QuartzSubsystem);
		return &SubsystemClockManager;
	}

	return nullptr;
}
