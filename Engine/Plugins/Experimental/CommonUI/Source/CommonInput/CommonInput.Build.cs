// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CommonInput : ModuleRules
{
	public CommonInput(ReadOnlyTargetRules Target) : base(Target)
	{
		PublicDependencyModuleNames.AddRange(
			new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "InputCore",
				"DeveloperSettings"
            }
		);

		PrivateDependencyModuleNames.AddRange(
		new string[]
			{
                "SlateCore",
				"Slate",
				"ApplicationCore",
                "EngineSettings"
            }
		);

        PrivateIncludePaths.AddRange(
			new string[]
			{
                "CommonInput/Private",
			}
		);

        if (Target.Type == TargetType.Editor)
        {
            PublicDependencyModuleNames.AddRange(
                new string[] {
					"EditorFramework",
                    "UnrealEd",
                }
            );
        }

		string CommonUIPlatform = ToCommonUIPlatform(Target.Platform);
		if (!string.IsNullOrEmpty(CommonUIPlatform))
		{
			PublicDefinitions.Add("UE_COMMONINPUT_PLATFORM_TYPE = " + CommonUIPlatform);
		}

		PrivateDependencyModuleNames.Add("GeForceNOWWrapper");
	}

	static public string ToCommonUIPlatform(UnrealTargetPlatform TargetPlatform)
	{
		if (TargetPlatform == UnrealTargetPlatform.Win64)
		{
			return "PC";
		}
		else if (TargetPlatform == UnrealTargetPlatform.Mac)
		{
			return "Mac";
		}
		else if (TargetPlatform == UnrealTargetPlatform.Linux)
		{
			return "PC";
		}
		else if (TargetPlatform == UnrealTargetPlatform.IOS)
		{
			return "IOS";
		}
		else if (TargetPlatform == UnrealTargetPlatform.Android)
		{
			return "Android";
		}

		return string.Empty;
	}
}
