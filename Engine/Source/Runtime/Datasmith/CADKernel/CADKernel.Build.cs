// Copyright Epic Games, Inc. All Rights Reserved.
using System.IO;

namespace UnrealBuildTool.Rules
{
	public class CADKernel : ModuleRules
	{
		public CADKernel(ReadOnlyTargetRules Target)
			: base(Target)
		{
			bUseUnity = false;
			PublicDependencyModuleNames.AddRange(
				new string[]
				{
					"Core",
				}
			);
		}
	}
}