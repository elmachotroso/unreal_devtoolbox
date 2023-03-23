// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class libcurl : ModuleRules
{
	public libcurl(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		PublicDefinitions.Add("WITH_LIBCURL=1");

		string LinuxLibCurlPath = Target.UEThirdPartySourceDirectory + "libcurl/7_65_3/";
		string WinLibCurlPath = Target.UEThirdPartySourceDirectory + "libcurl/curl-7.55.1/";
		string AndroidLibCurlPath = Target.UEThirdPartySourceDirectory + "libcurl/7_75_0/";

		if (Target.IsInPlatformGroup(UnrealPlatformGroup.Unix))
		{
			string platform = "/Unix/" + Target.Architecture;
			string IncludePath = LinuxLibCurlPath + "include" + platform;
			string LibraryPath = LinuxLibCurlPath + "lib" + platform;

			PublicIncludePaths.Add(IncludePath);
			PublicAdditionalLibraries.Add(LibraryPath + "/libcurl.a");

			PrivateDependencyModuleNames.Add("SSL");
		}
		else if (Target.IsInPlatformGroup(UnrealPlatformGroup.Android))
		{
			string[] Architectures = new string[] {
				"ARM64",
				"x64",
			};
 
			PublicIncludePaths.Add(AndroidLibCurlPath + "include/Android/");
			foreach(var Architecture in Architectures)
			{
				PublicAdditionalLibraries.Add(AndroidLibCurlPath + "lib/Android/" + Architecture + "/libcurl.a");
			}
		}
		else if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicIncludePaths.Add(WinLibCurlPath + "include/" + Target.Platform.ToString() +  "/VS" + Target.WindowsPlatform.GetVisualStudioCompilerVersionName());
			string LibDir = WinLibCurlPath + "lib/" + Target.Platform.ToString() +  "/VS" + Target.WindowsPlatform.GetVisualStudioCompilerVersionName() + "/";
			PublicAdditionalLibraries.Add(LibDir + "libcurl_a.lib");
			PublicDefinitions.Add("CURL_STATICLIB=1");

			// Our build requires OpenSSL and zlib, so ensure thye're linked in
			AddEngineThirdPartyPrivateStaticDependencies(Target, new string[]
			{
				"OpenSSL",
				"zlib"
			});
		}
		else if (Target.Platform == UnrealTargetPlatform.HoloLens)
		{
			// We do not currently have hololens OpenSSL binaries, lets not pretend we do.
			// We will remove WITH_LIBCURL=1 which should mean we *should* compile out dependencies on it, but I'm not very confident that 
			// there won't be some runtime failures in projects that do depend on it (like EngineTest).
			
			PublicDefinitions.Remove("WITH_LIBCURL=1");
		}
	}
}
