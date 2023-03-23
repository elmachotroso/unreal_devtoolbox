// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class MovieSceneTracks : ModuleRules
{
	public MovieSceneTracks(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateIncludePaths.AddRange(new string[]
			{ 
				"Runtime/MovieSceneTracks/Private",
				"Runtime/MovieSceneTracks/Private/Sections",
				"Runtime/MovieSceneTracks/Private/Tracks",
			});

		PublicDependencyModuleNames.AddRange(new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"MovieScene",
				"TimeManagement",
				"AnimationCore",
				"AudioExtensions",
			});

		PrivateDependencyModuleNames.AddRange(new string[]
			{
				"SlateCore",
				"AnimGraphRuntime",
				"PropertyPath"
			});

		if (Target.bBuildWithEditorOnlyData && Target.bBuildEditor)
		{
			PublicDependencyModuleNames.AddRange(new string[]
				{
					"BlueprintGraph"
				});
			PrivateDependencyModuleNames.AddRange(new string[]
				{
					"AnimationBlueprintLibrary",
					"DataLayerEditor",
					"EditorFramework",
					"UnrealEd"
				});
		}
	}
}
