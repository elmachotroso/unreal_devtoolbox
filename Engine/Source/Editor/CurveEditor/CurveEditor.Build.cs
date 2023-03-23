// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class CurveEditor : ModuleRules
{
	public CurveEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[] {
                "ApplicationCore",
                "AppFramework",
                "Core",
				"CoreUObject",
				"EditorFramework",
				"Engine",
				"EditorStyle",
                "InputCore",
				"Slate",
				"SlateCore",
				"TimeManagement",
				"UnrealEd",
				"SequencerWidgets",
			}
		);

        PublicDependencyModuleNames.Add("SequencerWidgets");
	}
}
