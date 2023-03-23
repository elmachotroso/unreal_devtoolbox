// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class TakeRecorderSources : ModuleRules
{
	public TakeRecorderSources(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateDependencyModuleNames.AddRange(
			new string[] {
                "CinematicCamera",
                "Core",
				"CoreUObject",
				"EditorFramework",
				"Engine",
				"EditorStyle",
				"InputCore",
				"LevelEditor",
				"LevelSequence",
                "LevelSequenceEditor",
                "MovieScene",
				"MovieSceneTracks",
                "SceneOutliner",
				"SequenceRecorder", // For ISequenceAudioRecorder
				"SerializedRecorderInterface",
                "Slate",
				"SlateCore",
				"TakesCore",
				"TakeRecorder",
				"TakeMovieScene",
				"UnrealEd",
			}
		);

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "TakeTrackRecorders",
            }
        );

		PrivateIncludePaths.AddRange(
			new string[] {
				"TakeRecorderSources/Private",
			}
		);
	}
}
