// Copyright Epic Games, Inc. All Rights Reserved.
#include "MetasoundGenerator.h"

#include "AudioParameter.h"
#include "DSP/Dsp.h"
#include "MetasoundGraph.h"
#include "MetasoundInputNode.h"
#include "MetasoundOperatorBuilder.h"
#include "MetasoundOperatorInterface.h"
#include "MetasoundOutputNode.h"
#include "MetasoundSourceInterface.h"
#include "MetasoundTrace.h"
#include "MetasoundTrigger.h"


namespace Metasound
{
	namespace ConsoleVariables
	{
		static bool bEnableAsyncMetaSoundGeneratorBuilder = true;
	}
}

FAutoConsoleVariableRef CVarMetaSoundEnableAsyncGeneratorBuilder(
	TEXT("au.MetaSound.EnableAsyncGeneratorBuilder"),
	Metasound::ConsoleVariables::bEnableAsyncMetaSoundGeneratorBuilder,
	TEXT("Enables async building of FMetaSoundGenerators\n")
	TEXT("Default: true"),
	ECVF_Default);


namespace Metasound
{
	void FMetasoundGeneratorInitParams::Release()
	{
		Graph.Reset();
		Environment = {};
		MetaSoundName = {};
		AudioOutputNames = {};
	}

	FAsyncMetaSoundBuilder::FAsyncMetaSoundBuilder(FMetasoundGenerator* InGenerator, FMetasoundGeneratorInitParams&& InInitParams, bool bInTriggerGenerator)
		: Generator(InGenerator)
		, InitParams(MoveTemp(InInitParams))
		, bTriggerGenerator(bInTriggerGenerator)
	{
	}

	void FAsyncMetaSoundBuilder::DoWork()
	{
		using namespace Audio;
		using namespace Frontend;

		METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(*FString::Printf(TEXT("AsyncMetaSoundBuilder::DoWork %s"), *InitParams.MetaSoundName));

		// Create an instance of the new graph
		FOperatorBuilder OperatorBuilder(FOperatorBuilderSettings::GetDefaultSettings());
		FDataReferenceCollection DataReferenceCollection{};
		FBuildGraphParams BuildParams{*InitParams.Graph, InitParams.OperatorSettings, DataReferenceCollection, InitParams.Environment};
		TArray<IOperatorBuilder::FBuildErrorPtr> BuildErrors;

		TUniquePtr<IOperator> GraphOperator = OperatorBuilder.BuildGraphOperator(BuildParams, BuildErrors);

		// Log build errors
		for (const IOperatorBuilder::FBuildErrorPtr& Error : BuildErrors)
		{
			if (Error.IsValid())
			{
				UE_LOG(LogMetaSound, Warning, TEXT("MetasoundSource [%s] build error [%s] \"%s\""), *InitParams.MetaSoundName, *(Error->GetErrorType().ToString()), *(Error->GetErrorDescription().ToString()));
			}
		}

		if (GraphOperator.IsValid())
		{
			TArray<FAudioBufferReadRef> OutputBuffers;
			FDataReferenceCollection Outputs = GraphOperator->GetOutputs();

			// Get output audio buffers.
			for (const FVertexName& AudioOutputName : InitParams.AudioOutputNames)
			{
				if (!Outputs.ContainsDataReadReference<FAudioBuffer>(AudioOutputName))
				{
					UE_LOG(LogMetaSound, Warning, TEXT("MetasoundSource [%s] does not contain audio output [%s] in output"), *InitParams.MetaSoundName, *AudioOutputName.ToString());
				}
				OutputBuffers.Add(Outputs.GetDataReadReferenceOrConstruct<FAudioBuffer>(AudioOutputName, InitParams.OperatorSettings));
			}

			// References must be cached before moving the operator to the InitParams
			FDataReferenceCollection Inputs = GraphOperator->GetInputs();
			FTriggerWriteRef PlayTrigger = Inputs.GetDataWriteReferenceOrConstruct<FTrigger>(SourceInterface::Inputs::OnPlay, InitParams.OperatorSettings, false);
			FTriggerReadRef FinishTrigger = Outputs.GetDataReadReferenceOrConstruct<FTrigger>(SourceOneShotInterface::Outputs::OnFinished, InitParams.OperatorSettings, false);

			FMetasoundGeneratorData GeneratorData
			{
				InitParams.OperatorSettings,
				MoveTemp(GraphOperator),
				MoveTemp(OutputBuffers),
				MoveTemp(PlayTrigger),
				MoveTemp(FinishTrigger)
			};

			Generator->SetPendingGraph(MoveTemp(GeneratorData), bTriggerGenerator);
		}
		else 
		{
			UE_LOG(LogMetaSound, Error, TEXT("Failed to build Metasound operator from graph in MetasoundSource [%s]"), *InitParams.MetaSoundName);
			// Set null generator data to inform that generator failed to build. 
			// Otherwise, generator will continually wait for a new generator.
			Generator->SetPendingGraphBuildFailed();
		}

		InitParams.Release();
	}

	FMetasoundGenerator::FMetasoundGenerator(FMetasoundGeneratorInitParams&& InParams)
		: MetasoundName(InParams.MetaSoundName)
		, bIsFinishTriggered(false)
		, bIsFinished(false)
		, NumChannels(0)
		, NumFramesPerExecute(0)
		, NumSamplesPerExecute(0)
		, OnPlayTriggerRef(FTriggerWriteRef::CreateNew(InParams.OperatorSettings))
		, OnFinishedTriggerRef(FTriggerWriteRef::CreateNew(InParams.OperatorSettings))
		, bPendingGraphTrigger(true)
		, bIsNewGraphPending(false)
		, bIsWaitingForFirstGraph(true)
	{
		NumChannels = InParams.AudioOutputNames.Num();
		NumFramesPerExecute = InParams.OperatorSettings.GetNumFramesPerBlock();
		NumSamplesPerExecute = NumChannels * NumFramesPerExecute;

		BuilderTask = MakeUnique<FBuilderTask>(this, MoveTemp(InParams), true /* bTriggerGenerator */);
		
		if (Metasound::ConsoleVariables::bEnableAsyncMetaSoundGeneratorBuilder)
		{
			// Build operator asynchronously
			BuilderTask->StartBackgroundTask(GBackgroundPriorityThreadPool);
		}
		else
		{
			// Build operator synchronously
			BuilderTask->StartSynchronousTask();
			BuilderTask = nullptr;
			UpdateGraphIfPending();
			bIsWaitingForFirstGraph = false;
		}
	}

	FMetasoundGenerator::~FMetasoundGenerator()
	{
		if (BuilderTask.IsValid())
		{
			BuilderTask->EnsureCompletion();
			BuilderTask = nullptr;
		}
	}

	void FMetasoundGenerator::SetPendingGraph(FMetasoundGeneratorData&& InData, bool bTriggerGraph)
	{
		FScopeLock SetPendingGraphLock(&PendingGraphMutex);
		{
			PendingGraphData = MakeUnique<FMetasoundGeneratorData>(MoveTemp(InData));
			bPendingGraphTrigger = bTriggerGraph;
			bIsNewGraphPending = true;
		}
	}

	void FMetasoundGenerator::SetPendingGraphBuildFailed()
	{
		FScopeLock SetPendingGraphLock(&PendingGraphMutex);
		{
			PendingGraphData = TUniquePtr<FMetasoundGeneratorData>(nullptr);
			bPendingGraphTrigger = false;
			bIsNewGraphPending = true;
		}
	}

	bool FMetasoundGenerator::UpdateGraphIfPending()
	{
		FScopeLock GraphLock(&PendingGraphMutex);
		if (bIsNewGraphPending)
		{
			SetGraph(MoveTemp(PendingGraphData), bPendingGraphTrigger);
			bIsNewGraphPending = false;
			return true;
		}

		return false;
	}

	void FMetasoundGenerator::SetGraph(TUniquePtr<FMetasoundGeneratorData>&& InData, bool bTriggerGraph)
	{
		if (!InData.IsValid())
		{
			return;
		}

		InterleavedAudioBuffer.Reset();

		GraphOutputAudio.Reset();
		if (InData->OutputBuffers.Num() == NumChannels)
		{
			if (InData->OutputBuffers.Num() > 0)
			{
				GraphOutputAudio.Append(InData->OutputBuffers.GetData(), InData->OutputBuffers.Num());
			}
		}
		else
		{
			int32 FoundNumChannels = InData->OutputBuffers.Num();

			UE_LOG(LogMetaSound, Warning, TEXT("Metasound generator expected %d number of channels, found %d"), NumChannels, FoundNumChannels);

			int32 NumChannelsToCopy = FMath::Min(FoundNumChannels, NumChannels);
			int32 NumChannelsToCreate = NumChannels - NumChannelsToCopy;

			if (NumChannelsToCopy > 0)
			{
				GraphOutputAudio.Append(InData->OutputBuffers.GetData(), NumChannelsToCopy);
			}
			for (int32 i = 0; i < NumChannelsToCreate; i++)
			{
				GraphOutputAudio.Add(TDataReadReference<FAudioBuffer>::CreateNew(InData->OperatorSettings));
			}
		}

		OnPlayTriggerRef = InData->TriggerOnPlayRef;
		OnFinishedTriggerRef = InData->TriggerOnFinishRef;

		// The graph operator and graph audio output contain all the values needed
		// by the sound generator.
		RootExecuter.SetOperator(MoveTemp(InData->GraphOperator));


		// Query the graph output to get the number of output audio channels.
		// Multichannel version:
		check(NumChannels == GraphOutputAudio.Num());

		if (NumSamplesPerExecute > 0)
		{
			// Preallocate interleaved buffer as it is necessary for any audio generation calls.
			InterleavedAudioBuffer.AddUninitialized(NumSamplesPerExecute);
		}

		if (bTriggerGraph)
		{
			OnPlayTriggerRef->TriggerFrame(0);
		}
	}

	int32 FMetasoundGenerator::GetNumChannels() const
	{
		return GraphOutputAudio.Num();
	}

	int32 FMetasoundGenerator::OnGenerateAudio(float* OutAudio, int32 NumSamplesRemaining)
	{
		METASOUND_TRACE_CPUPROFILER_EVENT_SCOPE_TEXT(*FString::Printf(TEXT("MetasoundGenerator::OnGenerateAudio %s"), *MetasoundName));

		// Defer finishing the metasound generator one block
		if (bIsFinishTriggered)
		{
			bIsFinished = true;
		}

		if (NumSamplesRemaining <= 0)
		{
			return 0;
		}

		const bool bDidUpdateGraph = UpdateGraphIfPending();
		bIsWaitingForFirstGraph = bIsWaitingForFirstGraph && !bDidUpdateGraph;

		// Output silent audio if we're still building a graph
		if (bIsWaitingForFirstGraph)
		{
			FMemory::Memset(OutAudio, 0, sizeof(float)* NumSamplesRemaining);
			return NumSamplesRemaining;
		}
		// If no longer pending and executer is no-op, kill the MetaSound.
		// Covers case where there was an error when building, resulting in
		// Executer operator being assigned to NoOp.
		else if (RootExecuter.IsNoOp() || NumSamplesPerExecute < 1)
		{
			bIsFinished = true;
			FMemory::Memset(OutAudio, 0, sizeof(float) * NumSamplesRemaining);
			return NumSamplesRemaining;
		}

		// If we have any audio left in the internal overflow buffer from 
		// previous calls, write that to the output before generating more audio.
		int32 NumSamplesWritten = FillWithBuffer(OverflowBuffer, OutAudio, NumSamplesRemaining);

		if (NumSamplesWritten > 0)
		{
			NumSamplesRemaining -= NumSamplesWritten;
			OverflowBuffer.RemoveAtSwap(0 /* Index */, NumSamplesWritten /* Count */, false /* bAllowShrinking */);
		}

		while (NumSamplesRemaining > 0)
		{
			// Call metasound graph operator.
			RootExecuter.Execute();

			// Interleave audio because ISoundGenerator interface expects interleaved audio.
			InterleaveGeneratedAudio();

			// Add audio generated during graph execution to the output buffer.
			int32 ThisLoopNumSamplesWritten = FillWithBuffer(InterleavedAudioBuffer, &OutAudio[NumSamplesWritten], NumSamplesRemaining);

			NumSamplesRemaining -= ThisLoopNumSamplesWritten;
			NumSamplesWritten += ThisLoopNumSamplesWritten;

			// If not all the samples were written, then we have to save the 
			// additional samples to the overflow buffer.
			if (ThisLoopNumSamplesWritten < InterleavedAudioBuffer.Num())
			{
				int32 OverflowCount = InterleavedAudioBuffer.Num() - ThisLoopNumSamplesWritten;

				OverflowBuffer.Reset();
				OverflowBuffer.AddUninitialized(OverflowCount);

				FMemory::Memcpy(OverflowBuffer.GetData(), &InterleavedAudioBuffer.GetData()[ThisLoopNumSamplesWritten], OverflowCount * sizeof(float));
			}
		}

		if (*OnFinishedTriggerRef)
		{
			bIsFinishTriggered = true;
		}

		return NumSamplesWritten;
	}

	int32 FMetasoundGenerator::GetDesiredNumSamplesToRenderPerCallback() const
	{
		// TODO: may improve performance if this number is increased. 
		return NumFramesPerExecute;
	}

	bool FMetasoundGenerator::IsFinished() const
	{
		return bIsFinished;
	}

	int32 FMetasoundGenerator::FillWithBuffer(const Audio::FAlignedFloatBuffer& InBuffer, float* OutAudio, int32 MaxNumOutputSamples)
	{
		int32 InNum = InBuffer.Num();

		if (InNum > 0)
		{
			if (InNum < MaxNumOutputSamples)
			{
				FMemory::Memcpy(OutAudio, InBuffer.GetData(), InNum * sizeof(float));
				return InNum;
			}
			else
			{
				FMemory::Memcpy(OutAudio, InBuffer.GetData(), MaxNumOutputSamples * sizeof(float));
				return MaxNumOutputSamples;
			}
		}

		return 0;
	}

	void FMetasoundGenerator::InterleaveGeneratedAudio()
	{
		// Prepare output buffer
		InterleavedAudioBuffer.Reset();

		if (NumSamplesPerExecute > 0)
		{
			InterleavedAudioBuffer.AddUninitialized(NumSamplesPerExecute);
		}

		// Iterate over channels
		for (int32 ChannelIndex = 0; ChannelIndex < NumChannels; ChannelIndex++)
		{
			const FAudioBuffer& InputBuffer = *GraphOutputAudio[ChannelIndex];

			const float* InputPtr = InputBuffer.GetData();
			float* OutputPtr = &InterleavedAudioBuffer.GetData()[ChannelIndex];

			// Assign values to output for single channel.
			for (int32 FrameIndex = 0; FrameIndex < NumFramesPerExecute; FrameIndex++)
			{
				*OutputPtr = InputPtr[FrameIndex];
				OutputPtr += NumChannels;
			}
		}
		// TODO: memcpy for single channel. 
	}
} 
