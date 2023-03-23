// Copyright Epic Games, Inc. All Rights Reserved.

using System.IO;
using System.Linq;
using EpicGames.Core;

namespace UnrealBuildTool.Rules
{
	public class OpenXRHMD : ModuleRules
	{
		public OpenXRHMD(ReadOnlyTargetRules Target) : base(Target)
        {
            var EngineDir = Path.GetFullPath(Target.RelativeEnginePath);
            PrivateIncludePaths.AddRange(
				new string[] {
					"OpenXRHMD/Private",
                    EngineDir + "/Source/ThirdParty/OpenXR/include",
                    EngineDir + "/Source/Runtime/Renderer/Private",
                    EngineDir + "/Source/Runtime/OpenGLDrv/Private",
                    EngineDir + "/Source/Runtime/VulkanRHI/Private",
					// ... add other private include paths required here ...
				}
				);

            if (Target.Platform == UnrealTargetPlatform.Win64)
            {
                PrivateIncludePaths.Add(EngineDir + "/Source/Runtime/VulkanRHI/Private/Windows");
            }
			else if (Target.Platform == UnrealTargetPlatform.Android  || Target.Platform == UnrealTargetPlatform.Linux)
            {
                PrivateIncludePaths.Add(EngineDir + "/Source/Runtime/VulkanRHI/Private/" + Target.Platform);
            }

			PublicIncludePathModuleNames.Add("OpenXR");
			

            PublicDependencyModuleNames.Add("HeadMountedDisplay");

			PrivateDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
					"CoreUObject",
					"Engine",
                    "BuildSettings",
                    "InputCore",
					"RHI",
					"RHICore",
					"RenderCore",
					"Renderer",
					"RenderCore",
                    "Slate",
                    "SlateCore",
					"AugmentedReality",
					"EngineSettings",
				}
				);

			if (Target.bBuildEditor == true)
            {
				PrivateDependencyModuleNames.Add("EditorFramework");
                PrivateDependencyModuleNames.Add("UnrealEd");
			}

            if (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.HoloLens)
            {
                PrivateDependencyModuleNames.AddRange(new string[] {
					"D3D11RHI",
					"D3D12RHI"
				});

                // Required for some private headers needed for the rendering support.
                PrivateIncludePaths.AddRange(
                    new string[] {
                            Path.Combine(EngineDir, @"Source\Runtime\Windows\D3D11RHI\Private"),
                            Path.Combine(EngineDir, @"Source\Runtime\Windows\D3D11RHI\Private\Windows"),
							Path.Combine(EngineDir, @"Source\Runtime\D3D12RHI\Private"),
							Path.Combine(EngineDir, @"Source\Runtime\D3D12RHI\Private\Windows")
								});

                AddEngineThirdPartyPrivateStaticDependencies(Target, "DX11");
				AddEngineThirdPartyPrivateStaticDependencies(Target, "DX12");
				AddEngineThirdPartyPrivateStaticDependencies(Target, "NVAPI");
                AddEngineThirdPartyPrivateStaticDependencies(Target, "AMD_AGS");
				AddEngineThirdPartyPrivateStaticDependencies(Target, "IntelMetricsDiscovery");
				AddEngineThirdPartyPrivateStaticDependencies(Target, "IntelExtensionsFramework");
            }

			if (Target.IsInPlatformGroup(UnrealPlatformGroup.Windows) || Target.IsInPlatformGroup(UnrealPlatformGroup.Linux))
			{
				AddEngineThirdPartyPrivateStaticDependencies(Target, "NVAftermath");
			}

            if (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Android)
            {
                PrivateDependencyModuleNames.AddRange(new string[] {
                    "OpenGLDrv",
                });

                AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenGL");
			}

			if (Target.Platform == UnrealTargetPlatform.Win64 || Target.Platform == UnrealTargetPlatform.Android  
			    || Target.IsInPlatformGroup(UnrealPlatformGroup.Linux))
            {
                PrivateDependencyModuleNames.AddRange(new string[] {
                    "VulkanRHI"
                });

                AddEngineThirdPartyPrivateStaticDependencies(Target, "Vulkan");
			}

			if (Target.Platform == UnrealTargetPlatform.Android)
			{
				bool bAndroidOculusEnabled = false;
				if (Target.ProjectFile != null)
				{
					ProjectDescriptor Project = ProjectDescriptor.FromFile(Target.ProjectFile);
					if (Project.Plugins != null)
					{
						foreach (PluginReferenceDescriptor PluginDescriptor in Project.Plugins)
						{
							if ((PluginDescriptor.Name == "OculusVR") && PluginDescriptor.IsEnabledForPlatform(Target.Platform))
							{
								Log.TraceInformation("OpenXRHMD: Android Oculus plugin enabled, will use OculusMobile_APL.xml");
								bAndroidOculusEnabled = true;
								break;
							}
						}
					}
				}

				if (!bAndroidOculusEnabled)
				{
					Log.TraceInformation("OpenXRHMD: Using OculusOpenXRLoader_APL.xml");

					// If the Oculus plugin is not enabled we need to include our own APL
					string PluginPath = Utils.MakePathRelativeTo(ModuleDirectory, Target.RelativeEnginePath);
					AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(PluginPath, "..", "..", "OculusOpenXRLoader_APL.xml"));
				}
			}
		}
	}
}
