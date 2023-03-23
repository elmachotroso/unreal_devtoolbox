// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class EditorFramework : ModuleRules
{
	public EditorFramework(ReadOnlyTargetRules Target) : base(Target)
	{
		PrivateIncludePaths.AddRange(
			new string[] {
			}
		);

		PrivateIncludePathModuleNames.AddRange(
			new string[] {
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[] {
				"Core",
				"CoreUObject",
				"EditorStyle",
				"Engine",
				"InputCore",
				"RenderCore",
				"Slate",
				"SlateCore",
				"WorkspaceMenuStructure"
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[] {
				"EditorSubsystem",
				"InteractiveToolsFramework",
				"TypedElementFramework",
				"TypedElementRuntime",
			}
		);

		PublicIncludePathModuleNames.AddRange(
			new string[] {
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
            new string[] { 
            }
		);
	}
}
