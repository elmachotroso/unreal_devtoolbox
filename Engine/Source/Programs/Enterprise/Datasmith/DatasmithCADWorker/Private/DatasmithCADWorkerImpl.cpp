// Copyright Epic Games, Inc. All Rights Reserved.

#include "DatasmithCADWorkerImpl.h"

#include "CADFileReader.h"
#include "CADOptions.h"
#include "DatasmithCommands.h"
#include "DatasmithDispatcherConfig.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/Thread.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

using namespace DatasmithDispatcher;


FDatasmithCADWorkerImpl::FDatasmithCADWorkerImpl(int32 InServerPID, int32 InServerPort, const FString& InEnginePluginsPath, const FString& InCachePath)
	: ServerPID(InServerPID)
	, ServerPort(InServerPort)
	, EnginePluginsPath(InEnginePluginsPath)
	, CachePath(InCachePath)
	, PingStartCycle(0)
{
}

bool FDatasmithCADWorkerImpl::Run()
{
	UE_LOG(LogDatasmithCADWorker, Verbose, TEXT("connect to %d..."), ServerPort);
	bool bConnected = NetworkInterface.Connect(TEXT("Datasmith CAD Worker"), ServerPort, Config::ConnectTimeout_s);
	UE_LOG(LogDatasmithCADWorker, Verbose, TEXT("connected to %d %s"), ServerPort, bConnected ? TEXT("OK") : TEXT("FAIL"));
	if (bConnected)
	{
		CommandIO.SetNetworkInterface(&NetworkInterface);
	}
	else
	{
		UE_LOG(LogDatasmithCADWorker, Error, TEXT("Server connection failure. exit"));
		return false;
	}

	InitiatePing();

	bool bIsRunning = true;
	while (bIsRunning)
	{
		if (TSharedPtr<ICommand> Command = CommandIO.GetNextCommand(1.0))
		{
			switch(Command->GetType())
			{
				case ECommandId::Ping:
					ProcessCommand(*StaticCast<FPingCommand*>(Command.Get()));
					break;

				case ECommandId::BackPing:
					ProcessCommand(*StaticCast<FBackPingCommand*>(Command.Get()));
					break;

				case ECommandId::RunTask:
					ProcessCommand(*StaticCast<FRunTaskCommand*>(Command.Get()));
					break;

				case ECommandId::ImportParams:
					ProcessCommand(*StaticCast<FImportParametersCommand*>(Command.Get()));
					break;

				case ECommandId::Terminate:
					UE_LOG(LogDatasmithCADWorker, Verbose, TEXT("Terminate command received. Exiting."));
					bIsRunning = false;
					break;

				case ECommandId::NotifyEndTask:
				default:
					break;
			}
		}
		else
		{
			if (bIsRunning)
			{
				bIsRunning = ServerPID == 0 ? true : FPlatformProcess::IsApplicationRunning(ServerPID);
				UE_CLOG(!bIsRunning, LogDatasmithCADWorker, Error, TEXT("Worker failure: server lost"));
			}
		}
	}

	UE_CLOG(!bIsRunning, LogDatasmithCADWorker, Verbose, TEXT("Worker loop exit..."));
	CommandIO.Disconnect(0);
	return true;
}

void FDatasmithCADWorkerImpl::InitiatePing()
{
	PingStartCycle = FPlatformTime::Cycles64();
	FPingCommand Ping;
	CommandIO.SendCommand(Ping, Config::SendCommandTimeout_s);
}

void FDatasmithCADWorkerImpl::ProcessCommand(const FPingCommand& PingCommand)
{
	FBackPingCommand BackPing;
	CommandIO.SendCommand(BackPing, Config::SendCommandTimeout_s);
}

void FDatasmithCADWorkerImpl::ProcessCommand(const FBackPingCommand& BackPingCommand)
{
	if (PingStartCycle)
	{
		double ElapsedTime_s = FGenericPlatformTime::ToSeconds(FPlatformTime::Cycles64() - PingStartCycle);
		UE_LOG(LogDatasmithCADWorker, Verbose, TEXT("Ping %f s"), ElapsedTime_s);
	}
	PingStartCycle = 0;
}

void FDatasmithCADWorkerImpl::ProcessCommand(const FImportParametersCommand& ImportParametersCommand)
{
	ImportParameters = ImportParametersCommand.ImportParameters;
}

uint64 DefineMaximumAllowedDuration(const CADLibrary::FFileDescriptor& FileDescriptor)
{
	FFileStatData FileStatData = IFileManager::Get().GetStatData(*FileDescriptor.GetSourcePath());
	double MaxTimePerMb = 5e-6;
	double SafetyCoeficient = 5;

	CADLibrary::ECADFormat Format = FileDescriptor.GetFileFormat();
	switch (Format)
	{
	case CADLibrary::ECADFormat::SOLIDWORKS:
	case CADLibrary::ECADFormat::CATIA_3DXML:
		MaxTimePerMb = 1e-5;
		break;
	case CADLibrary::ECADFormat::CATIA_CGR:
		MaxTimePerMb = 5e-7;
		break;
	case CADLibrary::ECADFormat::IGES:
		MaxTimePerMb = 1e-6;
		break;
	default:
		break;
	}

	uint64 MaximumDuration = ((double)FileStatData.FileSize) * MaxTimePerMb * SafetyCoeficient;
	return FMath::Max(MaximumDuration, (uint64)30);
}

void FDatasmithCADWorkerImpl::ProcessCommand(const FRunTaskCommand& RunTaskCommand)
{
	CADLibrary::FFileDescriptor FileToProcess = RunTaskCommand.JobFileDescription;
	UE_LOG(LogDatasmithCADWorker, Verbose, TEXT("Process %s %s"), *FileToProcess.GetFileName(), *FileToProcess.GetConfiguration());

	FCompletedTaskCommand CompletedTask;

	bProcessIsRunning = true;
	int64 MaxDuration = DefineMaximumAllowedDuration(FileToProcess);
	FThread TimeCheckerThread = FThread(TEXT("TimeCheckerThread"), [&]() { CheckDuration(FileToProcess, MaxDuration); });

	CADLibrary::FCADFileReader FileReader(ImportParameters, FileToProcess, EnginePluginsPath, CachePath);
	ETaskState ProcessResult = FileReader.ProcessFile();

	bProcessIsRunning = false;
	TimeCheckerThread.Join();
	
	CompletedTask.ProcessResult = ProcessResult;

	if (CompletedTask.ProcessResult == ETaskState::ProcessOk)
	{
		const CADLibrary::FCADFileData& CADFileData = FileReader.GetCADFileData();
		CompletedTask.ExternalReferences = CADFileData.GetExternalRefSet();
		CompletedTask.SceneGraphFileName = CADFileData.GetSceneGraphFileName();
		CompletedTask.GeomFileName = CADFileData.GetMeshFileName();
		CompletedTask.WarningMessages = CADFileData.GetWarningMessages();
	}

	CommandIO.SendCommand(CompletedTask, Config::SendCommandTimeout_s);

	UE_LOG(LogDatasmithCADWorker, Verbose, TEXT("End of Process %s %s saved in %s"), *FileToProcess.GetFileName(), *FileToProcess.GetConfiguration(), *CompletedTask.GeomFileName);
}

void FDatasmithCADWorkerImpl::CheckDuration(const CADLibrary::FFileDescriptor& FileToProcess, const int64 MaxDuration)
{
	if (!ImportParameters.bGEnableTimeControl)
	{
		return;
	}

	const uint64 StartTime = FPlatformTime::Cycles64();
	const uint64 MaxCycles = MaxDuration / FPlatformTime::GetSecondsPerCycle64() + StartTime;

	while (bProcessIsRunning)
	{
		FPlatformProcess::Sleep(0.1f);
		if (FPlatformTime::Cycles64() > MaxCycles)
		{
			UE_LOG(LogDatasmithCADWorker, Verbose, TEXT("Time exceeded to process %s %s. The maximum allowed duration is %ld s"), *FileToProcess.GetFileName(), *FileToProcess.GetConfiguration(), MaxDuration);
			FPlatformMisc::RequestExit(true);
		}
	}
	double Duration = (FPlatformTime::Cycles64() - StartTime) * FPlatformTime::GetSecondsPerCycle64();
	UE_LOG(LogDatasmithCADWorker, Verbose, TEXT("    Processing Time: %f s"), Duration);
}
