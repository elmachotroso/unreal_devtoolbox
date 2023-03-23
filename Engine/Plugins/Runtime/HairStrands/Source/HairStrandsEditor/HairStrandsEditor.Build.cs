// Copyright Epic Games, Inc. All Rights Reserved.

namespace UnrealBuildTool.Rules
{
	public class HairStrandsEditor : ModuleRules
	{
		public HairStrandsEditor(ReadOnlyTargetRules Target) : base(Target)
		{
			PrivateIncludePaths.Add(ModuleDirectory + "/Private");
			PublicIncludePaths.Add(ModuleDirectory + "/Public");

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"EditorFramework",
					"EditorStyle",
					"Engine",
					"GeometryCache",
					"HairStrandsCore",
					"InputCore",
					"MainFrame",
					"Slate",
					"SlateCore",
					"Projects",
					"ToolMenus",
					"UnrealEd",
					"AssetTools",
					"EditorInteractiveToolsFramework",
					"AdvancedPreviewScene",
					"InputCore",
					"Renderer",
					"PropertyEditor",
					"RHI",
					"LevelSequence",
					"MovieScene",
					"MovieSceneTools",
					"Sequencer"
				});
			AddEngineThirdPartyPrivateStaticDependencies(Target,
			 "FBX"
			);
		}
	}
}
