// Copyright Epic Games, Inc. All Rights Reserved.

#include "Tests/AutomationCommon.h"
#include "Misc/Paths.h"
#include "Misc/EngineVersion.h"
#include "EngineGlobals.h"
#include "Widgets/SWidget.h"
#include "Engine/GameViewportClient.h"
#include "Engine/Engine.h"
#include "HardwareInfo.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Package.h"
#include "Kismet/GameplayStatics.h"
#include "ContentStreaming.h"
#include "Widgets/SWindow.h"
#include "Framework/Application/SlateApplication.h"
#include "ShaderCompiler.h"
#include "GameFramework/GameStateBase.h"
#include "Scalability.h"
#include "Matinee/MatineeActor.h"
#include "StereoRendering.h"
#include "Misc/PackageName.h"
#include "TextureCompiler.h"
#include "Tests/AutomationTestSettings.h"
#include "GameMapsSettings.h"
#include "IRenderCaptureProvider.h"

#if WITH_AUTOMATION_TESTS

DEFINE_LOG_CATEGORY_STATIC(LogEngineAutomationLatentCommand, Log, All);
DEFINE_LOG_CATEGORY(LogEditorAutomationTests);
DEFINE_LOG_CATEGORY(LogEngineAutomationTests);

static TAutoConsoleVariable<int32> CVarAutomationAllowFrameTraceCapture(
	TEXT("AutomationAllowFrameTraceCapture"),
	1,
	TEXT("Allow automation to capture frame traces."),
	ECVF_Default
	);

//declare static variable
FOnEditorAutomationMapLoad AutomationCommon::OnEditorAutomationMapLoad;

///////////////////////////////////////////////////////////////////////
// Common Latent commands

namespace AutomationCommon
{
	FString GetRenderDetailsString()
	{
		FString HardwareDetailsString;

		// Create the folder name based on the hardware specs we have been provided
		FString HardwareDetails = FHardwareInfo::GetHardwareDetailsString();

		FString RHIString;
		FString RHILookup = NAME_RHI.ToString() + TEXT("=");
		if ( FParse::Value(*HardwareDetails, *RHILookup, RHIString) )
		{
			HardwareDetailsString = ( HardwareDetailsString + TEXT("_") ) + RHIString;
		}

		FString TextureFormatString;
		FString TextureFormatLookup = NAME_TextureFormat.ToString() + TEXT("=");
		if ( FParse::Value(*HardwareDetails, *TextureFormatLookup, TextureFormatString) )
		{
			HardwareDetailsString = ( HardwareDetailsString + TEXT("_") ) + TextureFormatString;
		}

		FString DeviceTypeString;
		FString DeviceTypeLookup = NAME_DeviceType.ToString() + TEXT("=");
		if ( FParse::Value(*HardwareDetails, *DeviceTypeLookup, DeviceTypeString) )
		{
			HardwareDetailsString = ( HardwareDetailsString + TEXT("_") ) + DeviceTypeString;
		}

		FString FeatureLevelString;
		GetFeatureLevelName(GMaxRHIFeatureLevel, FeatureLevelString);
		{
			HardwareDetailsString = ( HardwareDetailsString + TEXT("_") ) + FeatureLevelString;
		}

		if ( HardwareDetailsString.Len() > 0 )
		{
			//Get rid of the leading "_"
			HardwareDetailsString.RightChopInline(1, false);
		}

		return HardwareDetailsString;
	}

#if WITH_AUTOMATION_TESTS

	/** Gets a path used for automation testing (PNG sent to the AutomationTest folder) */
	FString GetScreenshotName(const FString& TestName)
	{
		FString PathName = TestName / FPlatformProperties::IniPlatformName();
		PathName = PathName + TEXT("/") + GetRenderDetailsString();

		// We need a unique ID for filenames from this run. We used to use GetDeviceId() but that is not guaranteed to return
		// a valid string on some platforms.
		static FString UUID = FGuid::NewGuid().ToString(EGuidFormats::Short).ToLower();

		return FString::Printf(TEXT("%s/%s.png"), *PathName, *UUID);
	}

	FString GetLocalPathForScreenshot(const FString& InScreenshotName)
	{
		return FPaths::AutomationDir() + InScreenshotName;
	}
	
	FAutomationScreenshotData BuildScreenshotData(const FString& MapOrContext, const FString& TestName, const FString& ScreenShotName, int32 Width, int32 Height)
	{
		FAutomationScreenshotData Data;

		Data.ScreenShotName = FPaths::MakeValidFileName(ScreenShotName, TEXT('_'));
		Data.Context = MapOrContext;
		Data.TestName = TestName;
		Data.Id = FGuid::NewGuid();
		Data.Commit = FEngineVersion::Current().HasChangelist() ? FString::FromInt(FEngineVersion::Current().GetChangelist()) : FString(TEXT(""));

		Data.Width = Width;
		Data.Height = Height;
		Data.Platform = FPlatformProperties::IniPlatformName();
		Data.Rhi = FHardwareInfo::GetHardwareInfo(NAME_RHI);
		GetFeatureLevelName(GMaxRHIFeatureLevel, Data.FeatureLevel);
		Data.bIsStereo = GEngine->StereoRenderingDevice.IsValid() ? GEngine->StereoRenderingDevice->IsStereoEnabled() : false;
		Data.Vendor = RHIVendorIdToString();
		Data.AdapterName = GRHIAdapterName;
		Data.AdapterInternalDriverVersion = GRHIAdapterInternalDriverVersion;
		Data.AdapterUserDriverVersion = GRHIAdapterUserDriverVersion;
		Data.UniqueDeviceId = FPlatformMisc::GetDeviceId();

		Scalability::FQualityLevels QualityLevels = Scalability::GetQualityLevels();

		Data.ResolutionQuality = QualityLevels.ResolutionQuality;
		Data.ViewDistanceQuality = QualityLevels.ViewDistanceQuality;
		Data.AntiAliasingQuality = QualityLevels.AntiAliasingQuality;
		Data.ShadowQuality = QualityLevels.ShadowQuality;
		Data.GlobalIlluminationQuality = QualityLevels.GlobalIlluminationQuality;
		Data.ReflectionQuality = QualityLevels.ReflectionQuality;
		Data.PostProcessQuality = QualityLevels.PostProcessQuality;
		Data.TextureQuality = QualityLevels.TextureQuality;
		Data.EffectsQuality = QualityLevels.EffectsQuality;
		Data.FoliageQuality = QualityLevels.FoliageQuality;
		Data.ShadingQuality = QualityLevels.ShadingQuality;
		
		//GRHIDeviceId

		// TBD - 
		// Device's native resolution (we want to use a hardware dump of the frontbuffer at the native resolution so we compare what we actually output rather than what we think we rendered)

		const FString MapAndTest = MapOrContext + TEXT("/") + Data.ScreenShotName;
		Data.ScreenshotName = GetScreenshotName(MapAndTest);

		return Data;
	}

	TArray<uint8> CaptureFrameTrace(const FString& MapOrContext, const FString& TestName)
	{
		TArray<uint8> FrameTrace;

		bool bDisableFrameTraceCapture = FParse::Param(FCommandLine::Get(), TEXT("DisableFrameTraceCapture"));
		if (!bDisableFrameTraceCapture && CVarAutomationAllowFrameTraceCapture.GetValueOnGameThread() != 0 && IRenderCaptureProvider::IsAvailable())
		{
			const FString MapAndTest = MapOrContext / FPaths::MakeValidFileName(TestName, TEXT('_'));
			FString ScreenshotName = GetScreenshotName(MapAndTest);
			FString TempCaptureFilePath = FPaths::ChangeExtension(FPaths::ConvertRelativePathToFull(FPaths::AutomationDir() / TEXT("Incoming/") / ScreenshotName), TEXT(".rdc"));

			UE_LOG(LogEngineAutomationTests, Log, TEXT("Taking Frame Trace: %s"), *TempCaptureFilePath);

			IRenderCaptureProvider::Get().CaptureFrame(GEngine->GameViewport->Viewport, 0, TempCaptureFilePath);
			FlushRenderingCommands();

			IPlatformFile& PlatformFileSystem = IPlatformFile::GetPlatformPhysical();
			if (PlatformFileSystem.FileExists(*TempCaptureFilePath))
			{
				{
					TUniquePtr<IFileHandle> FileHandle(PlatformFileSystem.OpenRead(*TempCaptureFilePath));

					int64 FileSize = FileHandle->Size();
					FrameTrace.SetNumUninitialized(FileSize);
					FileHandle->Read(FrameTrace.GetData(), FileSize);
				}

				PlatformFileSystem.DeleteFile(*TempCaptureFilePath);
			}
			else
			{
				UE_LOG(LogEngineAutomationTests, Warning, TEXT("Failed taking frame trace: %s"), *TempCaptureFilePath);
			}
		}

		return FrameTrace;
	}

	SWidget* FindWidgetByTag(const FName Tag)
	{
		const FTagMetaData UniqueMetaData(Tag);
		// Get a list of all the current slate windows
		TArray<TSharedRef<SWindow>> Windows;
		FSlateApplication::Get().GetAllVisibleWindowsOrdered(/*OUT*/Windows);

		TArray<SWidget*> Stack;
		for (const TSharedRef<SWindow>& Window : Windows)
		{
			Stack.Push(&Window.Get());
		}

		while (Stack.Num() > 0)
		{
			SWidget* Widget = Stack.Pop();
			const int32 NumChildren = Widget->GetChildren()->Num();
			for (int32 ChildIndex = 0; ChildIndex < NumChildren; ChildIndex++)
			{
				SWidget& ChildWidget = Widget->GetChildren()->GetChildAt(ChildIndex).Get();
				const TArray<TSharedRef<FTagMetaData>> AllMetaData = ChildWidget.GetAllMetaData<FTagMetaData>();
				for (int32 MetaDataIndex = 0; MetaDataIndex < AllMetaData.Num(); ++MetaDataIndex)
				{
					TSharedRef<FTagMetaData> MetaData = AllMetaData[MetaDataIndex];
					if (MetaData->Tag == UniqueMetaData.Tag)
					{
						// Done! found the widget
						return &ChildWidget;
					}
				}

				// If we got here we didn't match the widget so push this child on the stack.
				Stack.Push(&ChildWidget);
			}

		}

		return nullptr;
	}

	class FAutomationImageComparisonRequest : public IAutomationLatentCommand
	{
	public:
		FAutomationImageComparisonRequest(const FString& InImageName, const FString& InContext, int32 InWidth, int32 InHeight, const TArray<FColor>& InImageData, const FAutomationComparisonToleranceAmount& InTolerance, const FString& InNotes)
			: ImageName(InImageName), ImageData(InImageData), Initiate(false), TaskCompleted(false)
		{
			FString Context = InContext;
			if (Context.IsEmpty())
			{
				if (FAutomationTestBase* CurrentTest = FAutomationTestFramework::Get().GetCurrentTest())
				{
					Context = CurrentTest->GetTestContext();
					if (Context.IsEmpty())
					{
						Context = CurrentTest->GetTestFullName();
					}
				}
			}

			ComparisonParameters = BuildScreenshotData(Context, TEXT(""), ImageName, InWidth, InHeight);

			// Copy the relevant data into the metadata for the screenshot.
			ComparisonParameters.bHasComparisonRules = true;
			ComparisonParameters.ToleranceRed = InTolerance.Red;
			ComparisonParameters.ToleranceGreen = InTolerance.Green;
			ComparisonParameters.ToleranceBlue = InTolerance.Blue;
			ComparisonParameters.ToleranceAlpha = InTolerance.Alpha;
			ComparisonParameters.ToleranceMinBrightness = InTolerance.MinBrightness;
			ComparisonParameters.ToleranceMaxBrightness = InTolerance.MaxBrightness;
			ComparisonParameters.bIgnoreAntiAliasing = true;
			ComparisonParameters.bIgnoreColors = false;
			ComparisonParameters.MaximumLocalError = 0.10f;
			ComparisonParameters.MaximumGlobalError = 0.02f;

			// Record any user notes that were made to accompany this shot.
			ComparisonParameters.Notes = InNotes;
		}

		virtual ~FAutomationImageComparisonRequest()
		{
			FAutomationTestFramework::Get().OnScreenshotCompared.RemoveAll(this);
		}

		void OnComparisonComplete(const FAutomationScreenshotCompareResults& CompareResults)
		{
			FAutomationTestFramework::Get().OnScreenshotCompared.RemoveAll(this);

			if (FAutomationTestBase* CurrentTest = FAutomationTestFramework::Get().GetCurrentTest())
			{
				CurrentTest->AddEvent(CompareResults.ToAutomationEvent(ImageName));
			}

			TaskCompleted = true;
		}

		bool IsTaskCompleted()
		{
			return TaskCompleted;
		}

		bool Update() override
		{
			if (!Initiate)
			{
				FAutomationTestFramework::Get().OnScreenshotCaptured().ExecuteIfBound(ImageData, ComparisonParameters);

				UE_LOG(LogEditorAutomationTests, Log, TEXT("Requesting image %s to be compared."), *ComparisonParameters.ScreenshotName);

				FAutomationTestFramework::Get().OnScreenshotCompared.AddRaw(this, &FAutomationImageComparisonRequest::OnComparisonComplete);
				Initiate = true;
			}
			return IsTaskCompleted();
		}

	private:
		FString	ImageName;
		FAutomationScreenshotData ComparisonParameters;
		const TArray<FColor> ImageData;
		bool Initiate;
		bool TaskCompleted;
	};

#endif

	/** These save a PNG and get sent over the network */
	static void SaveWindowAsScreenshot(TSharedRef<SWindow> Window, const FString& ScreenshotName)
	{
		TSharedRef<SWidget> WindowRef = Window;

		TArray<FColor> OutImageData;
		FIntVector OutImageSize;
		if (FSlateApplication::Get().TakeScreenshot(WindowRef, OutImageData, OutImageSize))
		{
			FAutomationScreenshotData Data;
			Data.Width = OutImageSize.X;
			Data.Height = OutImageSize.Y;
			Data.ScreenshotName = ScreenshotName;
			FAutomationTestFramework::Get().OnScreenshotCaptured().ExecuteIfBound(OutImageData, Data);
		}
	}

	// @todo this is a temporary solution. Once we know how to get test's hands on a proper world
	// this function should be redone/removed
	UWorld* GetAnyGameWorld()
	{
		UWorld* TestWorld = nullptr;
		const TIndirectArray<FWorldContext>& WorldContexts = GEngine->GetWorldContexts();
		for ( const FWorldContext& Context : WorldContexts )
		{
			if ( ( ( Context.WorldType == EWorldType::PIE ) || ( Context.WorldType == EWorldType::Game ) ) && ( Context.World() != NULL ) )
			{
				TestWorld = Context.World();
				break;
			}
		}

		return TestWorld;
	}
}

bool AutomationOpenMap(const FString& MapName, bool bForceReload)
{
	bool bCanProceed = true;
	FString OutString = TEXT("");
#if WITH_EDITOR
	if (GIsEditor && AutomationCommon::OnEditorAutomationMapLoad.IsBound())
	{
		AutomationCommon::OnEditorAutomationMapLoad.Broadcast(MapName, bForceReload, &OutString);
	}
	else
#endif
	{
		UWorld* TestWorld = AutomationCommon::GetAnyGameWorld();

		// Convert both to short names and strip PIE prefix
		FString ShortMapName = FPackageName::GetShortName(MapName);
		FString ShortWorldMapName = FPackageName::GetShortName(TestWorld->GetMapName());

		if (TestWorld->GetOutermost()->GetPIEInstanceID() != INDEX_NONE)
		{
			FString PIEPrefix = FString::Printf(PLAYWORLD_PACKAGE_PREFIX TEXT("_%d_"), TestWorld->GetOutermost()->GetPIEInstanceID());
			ShortWorldMapName.ReplaceInline(*PIEPrefix, TEXT(""));
		}
		if (ShortMapName != ShortWorldMapName || bForceReload)
		{
			FString OpenCommand = FString::Printf(TEXT("Open %s"), *MapName);
			GEngine->Exec(TestWorld, *OpenCommand);
		}

		ADD_LATENT_AUTOMATION_COMMAND(FWaitForMapToLoadCommand());
	}

	return (OutString.IsEmpty());
}


bool FWaitLatentCommand::Update()
{
	const double NewTime = FPlatformTime::Seconds();
	if (NewTime - StartTime >= Duration)
	{
		return true;
	}
	return false;
}

bool FEditorAutomationLogCommand::Update()
{
	UE_LOG(LogEditorAutomationTests, Log, TEXT("%s"), *LogText);
	return true;
}

bool FTakeActiveEditorScreenshotCommand::Update()
{
	AutomationCommon::SaveWindowAsScreenshot(FSlateApplication::Get().GetActiveTopLevelWindow().ToSharedRef(),ScreenshotName);
	return true;
}

bool FTakeEditorScreenshotCommand::Update()
{
	AutomationCommon::SaveWindowAsScreenshot(ScreenshotParameters.CurrentWindow.ToSharedRef(), ScreenshotParameters.ScreenshotName);
	return true;
}

bool FLoadGameMapCommand::Update()
{
	check(GEngine->GetWorldContexts().Num() == 1);
	check(GEngine->GetWorldContexts()[0].WorldType == EWorldType::Game);

	UE_LOG(LogEngineAutomationTests, Log, TEXT("Loading Map Now. '%s'"), *MapName);
	GEngine->Exec(GEngine->GetWorldContexts()[0].World(), *FString::Printf(TEXT("Open %s"), *MapName));
	return true;
}

bool FExitGameCommand::Update()
{
	UWorld* TestWorld = AutomationCommon::GetAnyGameWorld();

	if ( APlayerController* TargetPC = UGameplayStatics::GetPlayerController(TestWorld, 0) )
	{
		TargetPC->ConsoleCommand(TEXT("Exit"), true);
	}

	return true;
}

bool FRequestExitCommand::Update()
{
	FPlatformMisc::RequestExit(true);
	return true;
}

bool FWaitForMapToLoadCommand::Update()
{
	UWorld* TestWorld = AutomationCommon::GetAnyGameWorld();

	if ( TestWorld && TestWorld->AreActorsInitialized() )
	{
		AGameStateBase* GameState = TestWorld->GetGameState();
		if (GameState && GameState->HasMatchStarted() )
		{
			return true;
		}
	}

	return false;
}

bool FWaitForSpecifiedMapToLoadCommand::Update()
{
	UWorld* TestWorld = AutomationCommon::GetAnyGameWorld();

	if ( TestWorld && TestWorld->AreActorsInitialized() )
	{
		AGameStateBase* GameState = TestWorld->GetGameState();
		if (GameState && GameState->HasMatchStarted())
		{
			// remove any paths or extensions to match the name of the world
			FString ShortMapName = FPackageName::GetShortName(MapName);
			ShortMapName = FPaths::GetBaseFilename(ShortMapName);

			// Handle both ways the user may have specified this
			if (TestWorld->GetName() == ShortMapName)
			{
				return true;
			}
		}
	}

	return false;
}

bool FWaitForAverageFrameRate::Update()
{
	if (StartTime == 0)
	{
		StartTime = FPlatformTime::Seconds();
	}
	else
	{
		const double ElapsedTime = FPlatformTime::Seconds() - StartTime;

		if (ElapsedTime > Delay)
		{
			extern ENGINE_API float GAverageFPS;
			if (GAverageFPS >= DesiredFrameRate)
			{
				return true;
			}

			if (ElapsedTime >= MaxWaitTime)
			{
				UE_LOG(LogEngineAutomationLatentCommand, Error, TEXT("FWaitForAverageFrameRate: Game did not reach %.02f FPS within %.02f seconds. Giving up."), DesiredFrameRate, MaxWaitTime);
				
				return true;
			}
		}
	}

	return false;
}



///////////////////////////////////////////////////////////////////////
// Common Latent commands which are used across test type. I.e. Engine, Network, etc...


bool FPlayMatineeLatentCommand::Update()
{
	if (MatineeActor)
	{
		UE_LOG(LogEngineAutomationLatentCommand, Log, TEXT("Triggering the matinee named: '%s'"), *MatineeActor->GetName())

		//force this matinee to not be looping so it doesn't infinitely loop
		MatineeActor->bLooping = false;
		MatineeActor->Play();
	}
	return true;
}


bool FWaitForMatineeToCompleteLatentCommand::Update()
{
	bool bTestComplete = true;
	if (MatineeActor)
	{
		bTestComplete = !MatineeActor->bIsPlaying;
	}
	return bTestComplete;
}


bool FExecStringLatentCommand::Update()
{
	UE_LOG(LogEngineAutomationLatentCommand, Log, TEXT("Executing the console command: '%s'"), *ExecCommand);

	if (GEngine->GameViewport)
	{
		GEngine->GameViewport->Exec(NULL, *ExecCommand, *GLog);
	}
	else
	{
		GEngine->Exec(NULL, *ExecCommand);
	}
	return true;
}


bool FEngineWaitLatentCommand::Update()
{
	const double NewTime = FPlatformTime::Seconds();
	if (NewTime - StartTime >= Duration)
	{
		return true;
	}
	return false;
}

ENGINE_API uint32 GStreamAllResourcesStillInFlight = -1;
bool FStreamAllResourcesLatentCommand::Update()
{
	const double LocalStartTime = FPlatformTime::Seconds();

	GStreamAllResourcesStillInFlight = IStreamingManager::Get().StreamAllResources(Duration);

	const double Time = FPlatformTime::Seconds();

	if(GStreamAllResourcesStillInFlight)
	{
		UE_LOG(LogEngineAutomationLatentCommand, Warning, TEXT("StreamAllResources() waited for %.2fs but %d resources are still in flight."), Time - LocalStartTime, GStreamAllResourcesStillInFlight);
	}
	else
	{
		UE_LOG(LogEngineAutomationLatentCommand, Log, TEXT("StreamAllResources() waited for %.2fs (max duration: %.2f)."), Time - LocalStartTime, Duration);
	}

	return true;
}

bool FEnqueuePerformanceCaptureCommands::Update()
{
	//for every matinee actor in the level
	for (TObjectIterator<AMatineeActor> It; It; ++It)
	{
		AMatineeActor* MatineeActor = *It;
		if (MatineeActor && MatineeActor->GetName().Contains(TEXT("Automation")))
		{
			//add latent action to execute this matinee
			ADD_LATENT_AUTOMATION_COMMAND(FPlayMatineeLatentCommand(MatineeActor));

			//add action to wait until matinee is complete
			ADD_LATENT_AUTOMATION_COMMAND(FWaitForMatineeToCompleteLatentCommand(MatineeActor));
		}
	}

	return true;
}


bool FMatineePerformanceCaptureCommand::Update()
{
	//for every matinee actor in the level
	for (TObjectIterator<AMatineeActor> It; It; ++It)
	{
		AMatineeActor* MatineeActor = *It;
		FString MatineeFOOName = MatineeActor->GetName();
		if (MatineeActor->GetName().Equals(MatineeName, ESearchCase::IgnoreCase))
		{


			//add latent action to execute this matinee
			ADD_LATENT_AUTOMATION_COMMAND(FPlayMatineeLatentCommand(MatineeActor));

			//Run the Stat FPS Chart command
			ADD_LATENT_AUTOMATION_COMMAND(FExecWorldStringLatentCommand(TEXT("StartFPSChart")));

			//add action to wait until matinee is complete
			ADD_LATENT_AUTOMATION_COMMAND(FWaitForMatineeToCompleteLatentCommand(MatineeActor));

			//Stop the Stat FPS Chart command
			ADD_LATENT_AUTOMATION_COMMAND(FExecWorldStringLatentCommand(TEXT("StopFPSChart")));
		}
		else
		{
			UE_LOG(LogEngineAutomationLatentCommand, Log, TEXT("'%s' is not the matinee name that is being searched for."), *MatineeActor->GetName())
		}
	}

	return true;

}


bool FExecWorldStringLatentCommand::Update()
{
	check(GEngine->GetWorldContexts().Num() == 1);
	check(GEngine->GetWorldContexts()[0].WorldType == EWorldType::Game);

	UE_LOG(LogEngineAutomationLatentCommand, Log, TEXT("Running Exec Command. '%s'"), *ExecCommand);
	GEngine->Exec(GEngine->GetWorldContexts()[0].World(), *ExecCommand);
	return true;
}


/**
* This will cause the test to wait for the shaders to finish compiling before moving on.
*/
bool FWaitForShadersToFinishCompilingInGame::Update()
{
#if WITH_EDITOR
	static double TimeShadersFinishedCompiling = 0;
	static double LastReportTime = FPlatformTime::Seconds();
	const double TimeToWaitForJobs = 2.0;

	bool ShadersCompiling = GShaderCompilingManager && GShaderCompilingManager->IsCompiling();
	bool TexturesCompiling = FTextureCompilingManager::Get().GetNumRemainingTextures() > 0;

	
	double TimeNow = FPlatformTime::Seconds();

	if (ShadersCompiling || TexturesCompiling)
	{
		if (TimeNow - LastReportTime > 5.0)
		{
			LastReportTime = TimeNow;

			if (ShadersCompiling)
			{
				UE_LOG(LogEditorAutomationTests, Log, TEXT("Waiting for %i shaders to finish."), GShaderCompilingManager->GetNumRemainingJobs() + GShaderCompilingManager->GetNumPendingJobs());
			}

			if (TexturesCompiling)
			{
				UE_LOG(LogEditorAutomationTests, Log, TEXT("Waiting for %i texures to finish."), FTextureCompilingManager::Get().GetNumRemainingTextures());
			}
		}

		TimeShadersFinishedCompiling = 0;

		return false;
	}

	// Current jobs are done, but things may still come in on subsequent frames..
	if (TimeShadersFinishedCompiling == 0)
	{
		TimeShadersFinishedCompiling = FPlatformTime::Seconds();
	}

	if (FPlatformTime::Seconds() - TimeShadersFinishedCompiling < TimeToWaitForJobs)
	{
		return false;
	}

	// may not be necessary, but just double-check everything is finished and ready
	GShaderCompilingManager->FinishAllCompilation();
	UE_LOG(LogEditorAutomationTests, Log, TEXT("Done waiting for shaders to finish."));
#endif

	return true;
}

void RequestImageComparison(const FString& InImageName, int32 InWidth, int32 InHeight, const TArray<FColor>& InImageData, EAutomationComparisonToleranceLevel InTolerance, const FString& InContext, const FString& InNotes)
{
#if WITH_AUTOMATION_TESTS
	FAutomationComparisonToleranceAmount ToleranceAmount = FAutomationComparisonToleranceAmount::FromToleranceLevel(InTolerance);
	ADD_LATENT_AUTOMATION_COMMAND(AutomationCommon::FAutomationImageComparisonRequest(InImageName, InContext, InWidth, InHeight, InImageData, ToleranceAmount, InNotes));
#endif
}


/**
 * Write a string to editor automation tests log
 */
DEFINE_ENGINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FEngineAutomationLogCommand, FString, LogText);

bool FEngineAutomationLogCommand::Update()
{
	UE_LOG(LogEngineAutomationTests, Log, TEXT("%s"), *LogText);
	return true;
}

/**
 * Generic Pie Test for projects.
 * By default this test will PIE the lit of MapsToPIETest from automation settings. if that is empty it will PIE the default editor and game (if they're different)
 * maps.
 *
 * If the editor session was started with a map on the command line then that's the only map that will be PIE'd. This allows project to set up tests that PIE
 * a list of maps from an external source.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FProjectMapsCycleTest, "Project.Maps.Cycle", EAutomationTestFlags::ClientContext | EAutomationTestFlags::ProductFilter)

/**
 * Execute the loading of one map to verify PIE works
 *
 * @param Parameters - Unused for this test
 * @return	TRUE if the test was successful, FALSE otherwise
 */
bool FProjectMapsCycleTest::RunTest(const FString& Parameters)
{
	UAutomationTestSettings const* AutomationTestSettings = GetDefault<UAutomationTestSettings>();
	check(AutomationTestSettings);

	// todo , move to automation settings
	int CycleCount = 2;
	TArray<FString> CycleMaps;

	FString ParsedMapName;

	if (FParse::Value(FCommandLine::Get(), TEXT("map="), ParsedMapName))
	{
		TArray<FString> MapList;

		ParsedMapName.ParseIntoArray(MapList, TEXT("+"), true);

		for (const FString& Map : MapList)
		{
			FString ActualName = Map;
			// If the specified package exists
			if (FPackageName::SearchForPackageOnDisk(Map, NULL, &ActualName) &&
				// and it's a valid map file
				FPaths::GetExtension(ActualName, /*bIncludeDot=*/true) == FPackageName::GetMapPackageExtension())
			{
				CycleMaps.Add(ActualName);
				UE_LOG(LogEngineAutomationTests, Display, TEXT("Found Map %s on command line. Cycle Test will be use this map"), *ActualName);
			}
			else
			{
				UE_LOG(LogEngineAutomationTests, Fatal, TEXT("Cound not find package for Map '%s' specified on command line."), *ActualName);
			}
		}
	}

	FParse::Value(FCommandLine::Get(), TEXT("map.cycles="), CycleCount);

	// If there was no command line map then default to the project settings
	if (CycleMaps.Num() == 0)
	{
		// If the project has maps configured for PIE then use those
	#if 0
		if (AutomationTestSettings->MapsToPIETest.Num())
		{
			for (const FString& Map : AutomationTestSettings->MapsToPIETest)
			{
				CycleMaps.Add(Map);
			}
		}
		else
	#endif
		{
			
			UGameMapsSettings const* MapSettings = GetDefault<UGameMapsSettings>();

			if (MapSettings->GetGameDefaultMap().Len())
			{
				FString StartupMap = MapSettings->GetGameDefaultMap();
				// Else pick the editor startup and game startup maps (if they are different).
				UE_LOG(LogEngineAutomationTests, Display, TEXT("No MapsToCycle specified in DefaultEngine.ini [/Script/Engine.AutomationTestSettings]. Using GameStartup Map %s"), *StartupMap);
				CycleMaps.Add(StartupMap);
			}
		}
	}

	// Uh-oh
	if (CycleMaps.Num() == 0)
	{
		UE_LOG(LogEngineAutomationTests, Fatal, TEXT("No automation or default maps are configured for cycling!"));
	}

	for (int i = 1; i <= CycleCount; i++)
	{
		AddCommand(new FEngineAutomationLogCommand(FString::Printf(TEXT("Starting Project.Maps Cycle (%d/%d)"), i, CycleCount)));
		for (const FString& Map : CycleMaps)
		{
			FString MapPackageName = FPackageName::ObjectPathToPackageName(Map);

			if (!FPackageName::IsValidObjectPath(MapPackageName))
			{
				if (!FPackageName::SearchForPackageOnDisk(MapPackageName, NULL, &MapPackageName))
				{
					UE_LOG(LogEditorAutomationTests, Error, TEXT("Couldn't resolve map for PIE test from %s to valid package name!"), *MapPackageName);
					continue;
				}
			}

			AddCommand(new FEngineAutomationLogCommand(FString::Printf(TEXT("LoadMap-Begin: %s"), *MapPackageName)));
			AddCommand(new FLoadGameMapCommand(Map));
			AddCommand(new FEngineAutomationLogCommand(FString::Printf(TEXT("LoadMap-End: %s"), *MapPackageName)));
			AddCommand(new FEngineAutomationLogCommand(FString::Printf(TEXT("MapWait-Begin: %s"), *MapPackageName)));
			AddCommand(new FWaitForShadersToFinishCompilingInGame());
			AddCommand(new FWaitForSpecifiedMapToLoadCommand(MapPackageName)); 
			AddCommand(new FWaitLatentCommand(AutomationTestSettings->PIETestDuration));
			AddCommand(new FEngineAutomationLogCommand(FString::Printf(TEXT("MapWait-End: %s"), *Map)));

		}
		AddCommand(new FEngineAutomationLogCommand(FString::Printf(TEXT("Ended Project.Maps Cycle (%d/%d)"), i, CycleCount)));
	}

	return true;
}

#endif //WITH_DEV_AUTOMATION_TESTS
