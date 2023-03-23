// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;

public class UnrealEd : ModuleRules
{
	public UnrealEd(ReadOnlyTargetRules Target) : base(Target)
	{
		if(Target.Type != TargetType.Editor)
		{
			throw new BuildException("Unable to instantiate UnrealEd module for non-editor targets.");
		}

		PrivatePCHHeaderFile = "Private/UnrealEdPrivatePCH.h";

		SharedPCHHeaderFile = "Public/UnrealEdSharedPCH.h";

		PrivateIncludePaths.AddRange(
			new string[]
			{
				"Editor/UnrealEd/Private",
				"Editor/UnrealEd/Private/Settings",
				"Editor/PackagesDialog/Public",
				"Developer/TargetPlatform/Public",
			}
		);

		PrivateIncludePathModuleNames.AddRange(
			new string[]
			{
				"BSPUtils",
				"BehaviorTreeEditor",
				"ClassViewer",
				"StructViewer",
				"ContentBrowser",
				"DerivedDataCache",
				"DesktopPlatform",
				"LauncherPlatform",
				"GameProjectGeneration",
				"MainFrame",
				"TurnkeySupport",
				"MaterialEditor",
				"MergeActors",
				"MeshUtilities",
				"MessagingCommon",
				"MovieSceneCapture",
				"NaniteTools",
				"PlacementMode",
				"Settings",
				"SettingsEditor",
				"AudioEditor",
				"ViewportSnapping",
				"SourceCodeAccess",
				"IntroTutorials",
				"OutputLog",
				"Landscape",
				"LocalizationService",
				"HierarchicalLODUtilities",
				"MessagingRpc",
				"PortalRpc",
				"PortalServices",
				"ViewportInteraction",
				"VREditor",
				"Persona",
				"PhysicsAssetEditor",
				"ClothingSystemEditorInterface",
				"NavigationSystem",
				"Media",
				"VirtualTexturingEditor",
				"TextureBuild",
				"ToolWidgets",
				"CSVtoSVG"
			}
		);

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"ApplicationCore",
				"DirectoryWatcher",
				"Documentation",
				"Engine",
				"Json",
				"Projects",
				"SandboxFile",
				"Slate",
				"SlateCore",
				"EditorFramework",
				"EditorStyle",
				"SourceControl",
				"UncontrolledChangelists",
				"UnrealEdMessages",
				"GameplayDebugger",
				"BlueprintGraph",
				"HTTP",
				"FunctionalTesting",
				"AutomationController",
				"Localization",
				"AudioEditor",
				"NetworkFileSystem",
				"UMG",
				"NavigationSystem",
				"MeshDescription",
				"StaticMeshDescription",
				"MeshBuilder",
				"MaterialShaderQualitySettings",
				"EditorSubsystem",
				"InteractiveToolsFramework",
				"TypedElementFramework",
				"TypedElementRuntime",
				"ToolMenusEditor",
				"StatusBar",
				"InterchangeCore",
				"InterchangeEngine",
				"DeveloperToolSettings",
				"SubobjectDataInterface",
				"SubobjectEditor",
				"PhysicsUtilities",
				"ToolWidgets",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"AssetRegistry",
				"AssetTagsEditor",
				"BSPUtils",
				"LevelSequence",
				"AnimGraph",
				"AppFramework",
				"BlueprintGraph",
				"CinematicCamera",
				"CurveEditor",
				"ContentBrowserData",
				"DerivedDataCache",
				"DesktopPlatform",
				"LauncherPlatform",
				"EditorStyle",
				"EngineSettings",
				"IESFile",
				"ImageWrapper",
				"ImageWriteQueue",
				"InputCore",
				"InputBindingEditor",
				"LauncherServices",
				"MaterialEditor",
				"MessageLog",
				"PakFile",
				"PropertyEditor",
				"Projects",
				"RawMesh",
				"MeshUtilitiesCommon",
				"SkeletalMeshUtilitiesCommon",
				"TextureUtilitiesCommon",
				"RenderCore",
				"RHI",
				"Sockets",
				"SourceControlWindows",
				"StatsViewer",
				"SwarmInterface",
				"TargetPlatform",
				"TargetDeviceServices",
				"EditorWidgets",
				"GraphEditor",
				"Kismet",
				"InternationalizationSettings",
				"JsonUtilities",
				"Landscape",
				"MeshPaint",
				"Foliage",
				"FoliageEdit",
				"VectorVM",
				"MaterialUtilities",
				"Localization",
				"LocalizationService",
				"LevelEditor",
				"AddContentDialog",
				"GameProjectGeneration",
				"HierarchicalLODUtilities",
				"Analytics",
				"AnalyticsET",
				"PluginWarden",
				"PixelInspectorModule",
				"MovieScene",
				"MovieSceneTracks",
				"Sequencer",
				"ViewportInteraction",
				"VREditor",
				"ClothingSystemEditor",
                "ClothingSystemRuntimeInterface",
                "ClothingSystemRuntimeCommon",
                "ClothingSystemRuntimeNv",
				"PIEPreviewDeviceProfileSelector",
				"PakFileUtilities",
				"TimeManagement",
                "LandscapeEditorUtilities",
				"ScriptDisassembler",
				"ToolMenus",
				"FreeImage",
				"UATHelper",
				"IoStoreUtilities",
				"EditorInteractiveToolsFramework",
				"TraceLog",
				"TraceAnalysis",
				"TraceServices",
				"DeveloperSettings",
				"AnimationModifiers",
				"AnimationBlueprintLibrary",
				"MaterialBaking",
				"CookOnTheFly",
				"Zen",
			}
		);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				"FontEditor",
				"StaticMeshEditor",
				"TextureEditor",
				"Cascade",
				"UMGEditor",
				"AssetTools",
				"ClassViewer",
				"StructViewer",
				"CollectionManager",
				"ContentBrowser",
				"CurveTableEditor",
				"DataTableEditor",
				"EditorSettingsViewer",
				"LandscapeEditor",
				"KismetCompiler",
				"DetailCustomizations",
				"ComponentVisualizers",
				"MainFrame",
				"TurnkeySupport",
				"PackagesDialog",
				"Persona",
				"PhysicsAssetEditor",
				"SettingsEditor",
				"StringTableEditor",
				"Blutility",
				"IntroTutorials",
				"WorkspaceMenuStructure",
				"PlacementMode",
				"MeshUtilities",
				"MergeActors",
				"NaniteTools",
				"ProjectSettingsViewer",
				"BehaviorTreeEditor",
				"ViewportSnapping",
				"GameplayTasksEditor",
				"UndoHistory",
				"SourceCodeAccess",
				"HotReload",
				"PortalProxies",
				"PortalServices",
				"OverlayEditor",
				"ClothPainter",
				"Media",
				"VirtualTexturingEditor",
				"WorldPartitionEditor",
				"CSVtoSVG",
			}
		);

		if (Target.bBuildTargetDeveloperTools)
		{
			PrivateIncludePathModuleNames.AddRange(
				new string[] {
					"ProjectTargetPlatformEditor",
				}
			);

			DynamicallyLoadedModuleNames.AddRange(
				new string[] {
					"SessionFrontend",
					"ProjectLauncher",
					"DeviceManager",
					"ProjectTargetPlatformEditor",
					"PListEditor",
					"TraceInsights",
				}
			);
		}

		CircularlyReferencedDependentModules.AddRange(
			new string[] {
				"Documentation",
				"GraphEditor",
				"Kismet",
				"AudioEditor",
				"ViewportInteraction",
				"VREditor",
				"MeshPaint",
				"PropertyEditor",
				"ToolMenusEditor",
				"InputBindingEditor",
				"ClothingSystemEditor",
				"PluginWarden",
				//"PIEPreviewDeviceProfileSelector",
				"EditorInteractiveToolsFramework"
			}
		);


		// Add include directory for Lightmass
		PublicIncludePaths.Add("Programs/UnrealLightmass/Public");

		PublicIncludePathModuleNames.AddRange(
			new string[] {
				"AssetRegistry",
				"AssetTagsEditor",
				"CollectionManager",
				"BlueprintGraph",
				"AddContentDialog",
				"MeshUtilities",
				"AssetTools",
				"KismetCompiler",
				"NavigationSystem",
				"GameplayTasks",
				"AIModule",
				"Engine",
				"SourceControl",
				"UncontrolledChangelists",
			}
		);


		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			PublicDependencyModuleNames.Add("AudioMixerXAudio2");

			PrivateDependencyModuleNames.Add("WindowsPlatformFeatures");
			PrivateDependencyModuleNames.Add("GameplayMediaEncoder");

			AddEngineThirdPartyPrivateStaticDependencies(Target,
				"UEOgg",
				"Vorbis",
				"VorbisFile",
				"DX11Audio"
			);
		}

		AddEngineThirdPartyPrivateStaticDependencies(Target,
			"FBX",
			"FreeType2"
		);

		SetupModulePhysicsSupport(Target);


		if (Target.bCompileRecast)
		{
			PrivateDependencyModuleNames.Add("Navmesh");
			PublicDefinitions.Add("WITH_RECAST=1");
			if (Target.bCompileNavmeshSegmentLinks)
			{
				PublicDefinitions.Add("WITH_NAVMESH_SEGMENT_LINKS=1");
			}
			else
			{
				PublicDefinitions.Add("WITH_NAVMESH_SEGMENT_LINKS=0");
			}

			if (Target.bCompileNavmeshClusterLinks)
			{
				PublicDefinitions.Add("WITH_NAVMESH_CLUSTER_LINKS=1");
			}
			else
			{
				PublicDefinitions.Add("WITH_NAVMESH_CLUSTER_LINKS=0");
			}
		}
		else
		{
			PublicDefinitions.Add("WITH_RECAST=0");
			PublicDefinitions.Add("WITH_NAVMESH_CLUSTER_LINKS=0");
			PublicDefinitions.Add("WITH_NAVMESH_SEGMENT_LINKS=0");
		}

		if (Target.bWithLiveCoding)
		{
			PrivateIncludePathModuleNames.Add("LiveCoding");
		}
    }
}
