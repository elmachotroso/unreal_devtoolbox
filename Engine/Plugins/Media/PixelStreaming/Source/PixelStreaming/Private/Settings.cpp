// Copyright Epic Games, Inc. All Rights Reserved.

#include "Settings.h"
#include "PixelStreamingPrivate.h"
#include "Misc/DefaultValueHelper.h"
#include "Async/Async.h"
#include "IPixelStreamingStatsConsumer.h"
#include "IPixelStreamingModule.h"
#include "PixelStreamingDelegates.h"

template <typename T>
void CommandLineParseValue(const TCHAR* Match, TAutoConsoleVariable<T>& CVar)
{
	T Value;
	if (FParse::Value(FCommandLine::Get(), Match, Value))
		CVar->Set(Value, ECVF_SetByCommandline);
};

void CommandLineParseValue(const TCHAR* Match, TAutoConsoleVariable<FString>& CVar, bool bStopOnSeparator = false)
{
	FString Value;
	if (FParse::Value(FCommandLine::Get(), Match, Value, bStopOnSeparator))
		CVar->Set(*Value, ECVF_SetByCommandline);
};

void CommandLineParseOption(const TCHAR* Match, TAutoConsoleVariable<bool>& CVar)
{
	FString ValueMatch(Match);
	ValueMatch.Append(TEXT("="));
	FString Value;
	if (FParse::Value(FCommandLine::Get(), *ValueMatch, Value))
	{
		if (Value.Equals(FString(TEXT("true")), ESearchCase::IgnoreCase))
		{
			CVar->Set(true, ECVF_SetByCommandline);
		}
		else if (Value.Equals(FString(TEXT("false")), ESearchCase::IgnoreCase))
		{
			CVar->Set(false, ECVF_SetByCommandline);
		}
	}
	else if (FParse::Param(FCommandLine::Get(), Match))
	{
		CVar->Set(true, ECVF_SetByCommandline);
	}
}

namespace UE::PixelStreaming::Settings
{
	// Begin Encoder CVars
	TAutoConsoleVariable<int32> CVarPixelStreamingEncoderTargetBitrate(
		TEXT("PixelStreaming.Encoder.TargetBitrate"),
		-1,
		TEXT("Target bitrate (bps). Ignore the bitrate WebRTC wants (not recommended). Set to -1 to disable. Default -1."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarPixelStreamingEncoderMaxBitrate(
		TEXT("PixelStreaming.Encoder.MaxBitrateVBR"),
		20000000,
		TEXT("Max bitrate (bps). Does not work in CBR rate control mode with NVENC."),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<bool> CVarPixelStreamingDebugDumpFrame(
		TEXT("PixelStreaming.Encoder.DumpDebugFrames"),
		false,
		TEXT("Dumps frames from the encoder to a file on disk for debugging purposes."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarPixelStreamingEncoderMinQP(
		TEXT("PixelStreaming.Encoder.MinQP"),
		0,
		TEXT("0-51, lower values result in better quality but higher bitrate. Default 0 - i.e. no limit on a minimum QP."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarPixelStreamingEncoderMaxQP(
		TEXT("PixelStreaming.Encoder.MaxQP"),
		51,
		TEXT("0-51, lower values result in better quality but higher bitrate. Default 51 - i.e. no limit on a maximum QP."),
		ECVF_Default);

	TAutoConsoleVariable<FString> CVarPixelStreamingEncoderRateControl(
		TEXT("PixelStreaming.Encoder.RateControl"),
		TEXT("CBR"),
		TEXT("PixelStreaming video encoder RateControl mode. Supported modes are `ConstQP`, `VBR`, `CBR`. Default: CBR, which we recommend."),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarPixelStreamingEnableFillerData(
		TEXT("PixelStreaming.Encoder.EnableFillerData"),
		false,
		TEXT("Maintains constant bitrate by filling with junk data. Note: Should not be required with CBR and MinQP = -1. Default: false."),
		ECVF_Default);

	TAutoConsoleVariable<FString> CVarPixelStreamingEncoderMultipass(
		TEXT("PixelStreaming.Encoder.Multipass"),
		TEXT("FULL"),
		TEXT("PixelStreaming encoder multipass. Supported modes are `DISABLED`, `QUARTER`, `FULL`"),
		ECVF_Default);

	TAutoConsoleVariable<FString> CVarPixelStreamingH264Profile(
		TEXT("PixelStreaming.Encoder.H264Profile"),
		TEXT("BASELINE"),
		TEXT("PixelStreaming encoder profile. Supported modes are `AUTO`, `BASELINE`, `MAIN`, `HIGH`, `HIGH444`, `STEREO`, `SVC_TEMPORAL_SCALABILITY`, `PROGRESSIVE_HIGH`, `CONSTRAINED_HIGH`"),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarPixelStreamingEncoderKeyframeInterval(
		TEXT("PixelStreaming.Encoder.KeyframeInterval"),
		300,
		TEXT("How many frames before a key frame is sent. Default: 300. Values <=0 will disable sending of periodic key frames. Note: NVENC does not support changing this after encoding has started."),
		ECVF_Default);

	TAutoConsoleVariable<FString> CVarPixelStreamingEncoderCodec(
		TEXT("PixelStreaming.Encoder.Codec"),
		TEXT("H264"),
		TEXT("PixelStreaming encoder codec. Supported values are `H264`, `VP8`, `VP9`"),
		ECVF_Default);
	// End Encoder CVars

	// Begin WebRTC CVars
	TAutoConsoleVariable<FString> CVarPixelStreamingDegradationPreference(
		TEXT("PixelStreaming.WebRTC.DegradationPreference"),
		TEXT("MAINTAIN_FRAMERATE"),
		TEXT("PixelStreaming degradation preference. Supported modes are `BALANCED`, `MAINTAIN_FRAMERATE`, `MAINTAIN_RESOLUTION`"),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarPixelStreamingWebRTCFps(
		TEXT("PixelStreaming.WebRTC.Fps"),
		60,
		TEXT("Framerate for WebRTC encoding. Default: 60"),
		ECVF_Default);

	//Note: 1 megabit is the maximum allowed in WebRTC for a start bitrate.
	TAutoConsoleVariable<int32> CVarPixelStreamingWebRTCStartBitrate(
		TEXT("PixelStreaming.WebRTC.StartBitrate"),
		1000000,
		TEXT("Start bitrate (bps) that WebRTC will try begin the stream with. Must be between Min/Max bitrates. Default: 1000000"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarPixelStreamingWebRTCMinBitrate(
		TEXT("PixelStreaming.WebRTC.MinBitrate"),
		100000,
		TEXT("Min bitrate (bps) that WebRTC will not request below. Careful not to set too high otherwise WebRTC will just drop frames. Default: 100000"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int32> CVarPixelStreamingWebRTCMaxBitrate(
		TEXT("PixelStreaming.WebRTC.MaxBitrate"),
		100000000,
		TEXT("Max bitrate (bps) that WebRTC will not request above. Careful not to set too high otherwise because a local (ideal network) will actually reach this. Default: 20000000"),
		ECVF_RenderThreadSafe);

	TAutoConsoleVariable<int> CVarPixelStreamingWebRTCLowQpThreshold(
		TEXT("PixelStreaming.WebRTC.LowQpThreshold"),
		25,
		TEXT("Only useful when MinQP=-1. Value between 1-51 (default: 25). If WebRTC is getting frames below this QP it will try to increase resolution when not in MAINTAIN_RESOLUTION mode."),
		ECVF_Default);

	TAutoConsoleVariable<int> CVarPixelStreamingWebRTCHighQpThreshold(
		TEXT("PixelStreaming.WebRTC.HighQpThreshold"),
		37,
		TEXT("Only useful when MinQP=-1. Value between 1-51 (default: 37). If WebRTC is getting frames above this QP it will decrease resolution when not in MAINTAIN_RESOLUTION mode."),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarPixelStreamingWebRTCDisableReceiveAudio(
		TEXT("PixelStreaming.WebRTC.DisableReceiveAudio"),
		false,
		TEXT("Disables receiving audio from the browser into UE."),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarPixelStreamingWebRTCDisableTransmitAudio(
		TEXT("PixelStreaming.WebRTC.DisableTransmitAudio"),
		false,
		TEXT("Disables transmission of UE audio to the browser."),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarPixelStreamingWebRTCDisableAudioSync(
		TEXT("PixelStreaming.WebRTC.DisableAudioSync"),
		true,
		TEXT("Disables the synchronization of audio and video tracks in WebRTC. This can be useful in low latency usecases where synchronization is not required."),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarPixelStreamingWebRTCUseLegacyAudioDevice(
		TEXT("PixelStreaming.WebRTC.UseLegacyAudioDevice"),
		false,
		TEXT("Whether put audio and video in the same stream (which will make WebRTC try to sync them)."),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarPixelStreamingWebRTCDisableStats(
		TEXT("PixelStreaming.WebRTC.DisableStats"),
		false,
		TEXT("Disables the collection of WebRTC stats."),
		ECVF_Default);

	// End WebRTC CVars

	// Begin Pixel Streaming Plugin CVars

	TAutoConsoleVariable<bool> CVarPixelStreamingOnScreenStats(
		TEXT("PixelStreaming.HUDStats"),
		false,
		TEXT("Whether to show PixelStreaming stats on the in-game HUD (default: true)."),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarPixelStreamingLogStats(
		TEXT("PixelStreaming.LogStats"),
		false,
		TEXT("Whether to show PixelStreaming stats in the log (default: false)."),
		ECVF_Default);

	TAutoConsoleVariable<int32> CVarPixelStreamingFreezeFrameQuality(
		TEXT("PixelStreaming.FreezeFrameQuality"),
		100,
		TEXT("Compression quality of the freeze frame"),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarSendPlayerIdAsInteger(
		TEXT("PixelStreaming.SendPlayerIdAsInteger"),
		true,
		TEXT("If true transmit the player id as an integer (for backward compatibility) or as a string."),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarPixelStreamingDisableLatencyTester(
		TEXT("PixelStreaming.DisableLatencyTester"),
		false,
		TEXT("If true disables latency tester being triggerable."),
		ECVF_Default);

	TAutoConsoleVariable<FString> CVarPixelStreamingKeyFilter(
		TEXT("PixelStreaming.KeyFilter"),
		"",
		TEXT("Comma separated list of keys to ignore from streaming clients."),
		ECVF_Default);

	TAutoConsoleVariable<bool> CVarPixelStreamingAllowConsoleCommands(
		TEXT("PixelStreaming.AllowPixelStreamingCommands"),
		false,
		TEXT("If true browser can send consoleCommand payloads that execute in UE's console."),
		ECVF_Default);

	TArray<FKey> FilteredKeys;

	void OnFilteredKeysChanged(IConsoleVariable* Var)
	{
		FString CommaList = Var->GetString();
		TArray<FString> KeyStringArray;
		CommaList.ParseIntoArray(KeyStringArray, TEXT(","), true);
		FilteredKeys.Empty();
		for (auto&& KeyString : KeyStringArray)
		{
			FilteredKeys.Add(FKey(*KeyString));
		}
	}

	void OnKeyframeIntervalChanged(IConsoleVariable* Var)
	{
		AsyncTask(ENamedThreads::GameThread, [Var]() {
			IConsoleVariable* CVarNVENCKeyframeInterval = IConsoleManager::Get().FindConsoleVariable(TEXT("NVENC.KeyframeInterval"));
			if (CVarNVENCKeyframeInterval)
			{
				CVarNVENCKeyframeInterval->Set(Var->GetInt(), ECVF_SetByCommandline);
			}

			IConsoleVariable* CVarAMFKeyframeInterval = IConsoleManager::Get().FindConsoleVariable(TEXT("AMF.KeyframeInterval"));
			if (CVarNVENCKeyframeInterval)
			{
				CVarAMFKeyframeInterval->Set(Var->GetInt(), ECVF_SetByCommandline);
			}
		});
	}
	// Ends Pixel Streaming Plugin CVars

	// Begin utility functions etc.
	std::map<FString, AVEncoder::FVideoEncoder::RateControlMode> const RateControlCVarMap{
		{ "ConstQP", AVEncoder::FVideoEncoder::RateControlMode::CONSTQP },
		{ "VBR", AVEncoder::FVideoEncoder::RateControlMode::VBR },
		{ "CBR", AVEncoder::FVideoEncoder::RateControlMode::CBR },
	};

	std::map<FString, AVEncoder::FVideoEncoder::MultipassMode> const MultipassCVarMap{
		{ "DISABLED", AVEncoder::FVideoEncoder::MultipassMode::DISABLED },
		{ "QUARTER", AVEncoder::FVideoEncoder::MultipassMode::QUARTER },
		{ "FULL", AVEncoder::FVideoEncoder::MultipassMode::FULL },
	};

	std::map<FString, AVEncoder::FVideoEncoder::H264Profile> const H264ProfileMap{
		{ "AUTO", AVEncoder::FVideoEncoder::H264Profile::AUTO },
		{ "BASELINE", AVEncoder::FVideoEncoder::H264Profile::BASELINE },
		{ "MAIN", AVEncoder::FVideoEncoder::H264Profile::MAIN },
		{ "HIGH", AVEncoder::FVideoEncoder::H264Profile::HIGH },
		{ "HIGH444", AVEncoder::FVideoEncoder::H264Profile::HIGH444 },
		{ "STEREO", AVEncoder::FVideoEncoder::H264Profile::STEREO },
		{ "SVC_TEMPORAL_SCALABILITY", AVEncoder::FVideoEncoder::H264Profile::SVC_TEMPORAL_SCALABILITY },
		{ "PROGRESSIVE_HIGH", AVEncoder::FVideoEncoder::H264Profile::PROGRESSIVE_HIGH },
		{ "CONSTRAINED_HIGH", AVEncoder::FVideoEncoder::H264Profile::CONSTRAINED_HIGH },
	};

	AVEncoder::FVideoEncoder::RateControlMode GetRateControlCVar()
	{
		const FString EncoderRateControl = CVarPixelStreamingEncoderRateControl.GetValueOnAnyThread();
		auto const Iter = RateControlCVarMap.find(EncoderRateControl);
		if (Iter == std::end(RateControlCVarMap))
			return AVEncoder::FVideoEncoder::RateControlMode::CBR;
		return Iter->second;
	}

	AVEncoder::FVideoEncoder::MultipassMode GetMultipassCVar()
	{
		const FString EncoderMultipass = CVarPixelStreamingEncoderMultipass.GetValueOnAnyThread();
		auto const Iter = MultipassCVarMap.find(EncoderMultipass);
		if (Iter == std::end(MultipassCVarMap))
			return AVEncoder::FVideoEncoder::MultipassMode::FULL;
		return Iter->second;
	}

	webrtc::DegradationPreference GetDegradationPreference()
	{
		FString DegradationPreference = CVarPixelStreamingDegradationPreference.GetValueOnAnyThread();
		if (DegradationPreference == "MAINTAIN_FRAMERATE")
		{
			return webrtc::DegradationPreference::MAINTAIN_FRAMERATE;
		}
		else if (DegradationPreference == "MAINTAIN_RESOLUTION")
		{
			return webrtc::DegradationPreference::MAINTAIN_RESOLUTION;
		}
		// Everything else, return balanced.
		return webrtc::DegradationPreference::BALANCED;
	}

	AVEncoder::FVideoEncoder::H264Profile GetH264Profile()
	{
		const FString H264Profile = CVarPixelStreamingH264Profile.GetValueOnAnyThread();
		auto const Iter = H264ProfileMap.find(H264Profile);
		if (Iter == std::end(H264ProfileMap))
			return AVEncoder::FVideoEncoder::H264Profile::BASELINE;
		return Iter->second;
	}
	// End utility functions etc.

	FSimulcastParameters SimulcastParameters;

	void ReadSimulcastParameters()
	{
		SimulcastParameters.Layers.Empty();

		FString StringOptions;
		bool bPassedSimulcastParams = FParse::Value(FCommandLine::Get(), TEXT("SimulcastParameters="), StringOptions, false);

		// If no simulcast parameters are passed use some default values
		if (!bPassedSimulcastParams)
		{
			//StringOptions = FString(TEXT("1.0,5000000,20000000,2.0,1000000,5000000,4.0,50000,1000000"));
			StringOptions = FString(TEXT("1.0,5000000,20000000,2.0,1000000,5000000"));
		}

		TArray<FString> ParameterArray;
		StringOptions.ParseIntoArray(ParameterArray, TEXT(","), true);
		const int OptionCount = ParameterArray.Num();
		bool bSuccess = OptionCount % 3 == 0;
		int NextOption = 0;
		while (bSuccess && ((OptionCount - NextOption) >= 3))
		{
			FSimulcastParameters::FLayer Layer;
			bSuccess = FDefaultValueHelper::ParseFloat(ParameterArray[NextOption++], Layer.Scaling);
			bSuccess = FDefaultValueHelper::ParseInt(ParameterArray[NextOption++], Layer.MinBitrate);
			bSuccess = FDefaultValueHelper::ParseInt(ParameterArray[NextOption++], Layer.MaxBitrate);
			SimulcastParameters.Layers.Add(Layer);
		}

		if (!bSuccess)
		{
			// failed parsing the parameters. just ignore the parameters.
			UE_LOG(LogPixelStreaming, Error, TEXT("Simulcast parameters malformed. Expected [Scaling_0, MinBitrate_0, MaxBitrate_0, ... , Scaling_N, MinBitrate_N, MaxBitrate_N] as [float, int, int, ... , float, int, int] etc.]"));
			SimulcastParameters.Layers.Empty();
		}
	}

	/*
	* Selected Codec.
	*/
	ECodec GetSelectedCodec()
	{
		const FString CodecStr = CVarPixelStreamingEncoderCodec.GetValueOnAnyThread();
		if (CodecStr == TEXT("H264"))
		{
			return ECodec::H264;
		}
		else if (CodecStr == TEXT("VP8"))
		{
			return ECodec::VP8;
		}
		else if (CodecStr == TEXT("VP9"))
		{
			return ECodec::VP9;
		}
		else
		{
			return ECodec::H264;
		}
	}

	bool IsCodecVPX()
	{
		ECodec SelectedCodec = GetSelectedCodec();
		return SelectedCodec == ECodec::VP8 || SelectedCodec == ECodec::VP9;
	}

	/*
	* Stats logger - as turned on/off by CVarPixelStreamingLogStats
	*/
	void ConsumeStat(FPixelStreamingPlayerId PlayerId, FName StatName, float StatValue)
	{
		UE_LOG(LogPixelStreaming, Log, TEXT("[%s](%s) = %f"), *PlayerId, *StatName.ToString(), StatValue);
	}

	void OnLogStatsChanged(IConsoleVariable* Var)
	{
		bool bLogStats = Var->GetBool();
		UPixelStreamingDelegates* Delegates = UPixelStreamingDelegates::GetPixelStreamingDelegates();
		if (bLogStats && Delegates)
		{
			Delegates->OnStatChangedNative.AddStatic(&UE::PixelStreaming::Settings::ConsumeStat);
		}
	}

	/*
	* Settings parsing and initialization.
	*/

	// Some settings need to be set after streamer is initialized
	void OnStreamerReady(IPixelStreamingModule& Module)
	{
		UE::PixelStreaming::Settings::CVarPixelStreamingLogStats.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&UE::PixelStreaming::Settings::OnLogStatsChanged));
		CommandLineParseOption(TEXT("PixelStreamingLogStats"), UE::PixelStreaming::Settings::CVarPixelStreamingLogStats);
	}

	void InitialiseSettings()
	{
		UE_LOG(LogPixelStreaming, Log, TEXT("Initialising Pixel Streaming settings."));

		UE::PixelStreaming::Settings::CVarPixelStreamingKeyFilter.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&UE::PixelStreaming::Settings::OnFilteredKeysChanged));
		UE::PixelStreaming::Settings::CVarPixelStreamingEncoderKeyframeInterval.AsVariable()->SetOnChangedCallback(FConsoleVariableDelegate::CreateStatic(&UE::PixelStreaming::Settings::OnKeyframeIntervalChanged));

		// Values parse from commands line
		CommandLineParseValue(TEXT("PixelStreamingEncoderKeyframeInterval="), UE::PixelStreaming::Settings::CVarPixelStreamingEncoderKeyframeInterval);
		CommandLineParseValue(TEXT("PixelStreamingEncoderTargetBitrate="), UE::PixelStreaming::Settings::CVarPixelStreamingEncoderTargetBitrate);
		CommandLineParseValue(TEXT("PixelStreamingEncoderMaxBitrate="), UE::PixelStreaming::Settings::CVarPixelStreamingEncoderMaxBitrate);
		CommandLineParseValue(TEXT("PixelStreamingEncoderMinQP="), UE::PixelStreaming::Settings::CVarPixelStreamingEncoderMinQP);
		CommandLineParseValue(TEXT("PixelStreamingEncoderMaxQP="), UE::PixelStreaming::Settings::CVarPixelStreamingEncoderMaxQP);
		CommandLineParseValue(TEXT("PixelStreamingEncoderRateControl="), UE::PixelStreaming::Settings::CVarPixelStreamingEncoderRateControl);
		CommandLineParseValue(TEXT("PixelStreamingEncoderMultipass="), UE::PixelStreaming::Settings::CVarPixelStreamingEncoderMultipass);
		CommandLineParseValue(TEXT("PixelStreamingEncoderCodec="), UE::PixelStreaming::Settings::CVarPixelStreamingEncoderCodec);
		CommandLineParseValue(TEXT("PixelStreamingH264Profile="), UE::PixelStreaming::Settings::CVarPixelStreamingH264Profile);
		CommandLineParseValue(TEXT("PixelStreamingDegradationPreference="), UE::PixelStreaming::Settings::CVarPixelStreamingDegradationPreference);
		CommandLineParseValue(TEXT("PixelStreamingWebRTCDegradationPreference="), UE::PixelStreaming::Settings::CVarPixelStreamingDegradationPreference);
		CommandLineParseValue(TEXT("PixelStreamingWebRTCFps="), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCFps);
		CommandLineParseValue(TEXT("PixelStreamingWebRTCStartBitrate="), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCStartBitrate);
		CommandLineParseValue(TEXT("PixelStreamingWebRTCMinBitrate="), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCMinBitrate);
		CommandLineParseValue(TEXT("PixelStreamingWebRTCMaxBitrate="), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCMaxBitrate);
		CommandLineParseValue(TEXT("PixelStreamingWebRTCLowQpThreshold="), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCLowQpThreshold);
		CommandLineParseValue(TEXT("PixelStreamingWebRTCHighQpThreshold="), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCHighQpThreshold);
		CommandLineParseValue(TEXT("PixelStreamingFreezeFrameQuality"), UE::PixelStreaming::Settings::CVarPixelStreamingFreezeFrameQuality);
		CommandLineParseValue(TEXT("PixelStreamingKeyFilter="), UE::PixelStreaming::Settings::CVarPixelStreamingKeyFilter);

		// Options parse (if these exist they are set to true)
		CommandLineParseOption(TEXT("AllowPixelStreamingCommands"), UE::PixelStreaming::Settings::CVarPixelStreamingAllowConsoleCommands);
		CommandLineParseOption(TEXT("PixelStreamingOnScreenStats"), UE::PixelStreaming::Settings::CVarPixelStreamingOnScreenStats);
		CommandLineParseOption(TEXT("PixelStreamingHudStats"), UE::PixelStreaming::Settings::CVarPixelStreamingOnScreenStats);

		CommandLineParseOption(TEXT("PixelStreamingDebugDumpFrame"), UE::PixelStreaming::Settings::CVarPixelStreamingDebugDumpFrame);
		CommandLineParseOption(TEXT("PixelStreamingEnableFillerData"), UE::PixelStreaming::Settings::CVarPixelStreamingEnableFillerData);
		CommandLineParseOption(TEXT("PixelStreamingWebRTCDisableStats"), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCDisableStats);
		CommandLineParseOption(TEXT("PixelStreamingWebRTCDisableReceiveAudio"), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCDisableReceiveAudio);
		CommandLineParseOption(TEXT("PixelStreamingWebRTCDisableTransmitAudio"), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCDisableTransmitAudio);
		CommandLineParseOption(TEXT("PixelStreamingWebRTCDisableAudioSync"), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCDisableAudioSync);
		CommandLineParseOption(TEXT("PixelStreamingSendPlayerIdAsInteger"), UE::PixelStreaming::Settings::CVarSendPlayerIdAsInteger);
		CommandLineParseOption(TEXT("PixelStreamingWebRTCUseLegacyAudioDevice"), UE::PixelStreaming::Settings::CVarPixelStreamingWebRTCUseLegacyAudioDevice);
		CommandLineParseOption(TEXT("PixelStreamingDisableLatencyTester"), UE::PixelStreaming::Settings::CVarPixelStreamingDisableLatencyTester);

		ReadSimulcastParameters();

		IPixelStreamingModule& Module = IPixelStreamingModule::Get();
		Module.OnReady().AddStatic(&UE::PixelStreaming::Settings::OnStreamerReady);
	}

} // namespace UE::PixelStreaming::Settings
