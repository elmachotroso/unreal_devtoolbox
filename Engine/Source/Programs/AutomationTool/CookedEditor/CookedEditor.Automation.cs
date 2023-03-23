// Copyright Epic Games, Inc. All Rights Reserved.
using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using AutomationTool;
using UnrealBuildTool;
using AutomationScripts;
using EpicGames.Core;
using UnrealBuildBase;
using System.Text.Json;

public class ConfigHelper
{
	private string SpecificConfigSection;
	private string SharedConfigSection;
	private ConfigHierarchy GameConfig;


	public ConfigHelper(UnrealTargetPlatform Platform, FileReference ProjectFile, bool bIsCookedCooker)
	{
		SharedConfigSection = "CookedEditorSettings";
		SpecificConfigSection = SharedConfigSection + (bIsCookedCooker ? "_CookedCooker" : "_CookedEditor");

		GameConfig = ConfigCache.ReadHierarchy(ConfigHierarchyType.Game, ProjectFile.Directory, Platform);
	}

	public bool GetBool(string Key)
	{
		bool Value;
		// GetBool will set Value to false if it's not found, which is what we want
		if (!GameConfig.GetBool(SpecificConfigSection, Key, out Value))
		{
			GameConfig.GetBool(SharedConfigSection, Key, out Value);
		}
		return Value;
	}

	public string GetString(string Key)
	{
		string Value;
		// GetBool will set Value to "" if it's not found, so, set it to null if not found
		if (!GameConfig.GetString(SpecificConfigSection, Key, out Value))
		{
			if (!GameConfig.GetString(SharedConfigSection, Key, out Value))
			{
				Value = null;
			}
		}
		return Value;
	}

	public List<string> GetArray(string Key)
	{
		List<string> Value = new List<string>();
		List<string> Temp;

		// merge both sections into one array (probably don't depend on order)
		if (GameConfig.GetArray(SpecificConfigSection, Key, out Temp))
		{
			Value.AddRange(Temp);
		}
		if (GameConfig.GetArray(SharedConfigSection, Key, out Temp))
		{
			Value.AddRange(Temp);
		}

		return Value;
	}
}

public class ModifyStageContext
{
	// any assets that end up in this list that are already in the DeploymentContext will be removed during Apply
	public List<FileReference> UFSFilesToStage = new List<FileReference>();
	// files in this list will remove the matching cooked package from the DeploymentContext and these uncooked assets will replace them
	public List<FileReference> FilesToUncook = new List<FileReference>();
	// these files will just be staged
	public List<FileReference> NonUFSFilesToStage = new List<FileReference>();

	public bool bStageShaderDirs = true;
	public bool bStagePlatformBuildDirs = true;
	public bool bStageExtrasDirs = false;
	public bool bStagePlatformDirs = true;
	public bool bStageUAT = false;
	public bool bIsForExternalDistribution = false;

	public ConfigHelper ConfigHelper;

	public DirectoryReference EngineDirectory;
	public DirectoryReference ProjectDirectory;
	public string ProjectName;
	public string IniPlatformName;
	public bool bIsDLC;

	// when creating a cooked editor against a premade client, this is the sub-directory in the Releases directory to compare against
	public DirectoryReference ReleaseMetadataLocation = null;

	// where to find files like CachedEditorThumbnails.bin or EditorClientAssetRegistry.bin
	public DirectoryReference CachedEditorDataLocation = null;

	// commandline etc helper
	private BuildCommand Command;

	public ModifyStageContext(ConfigHelper ConfigHelper, DirectoryReference EngineDirectory, ProjectParams Params, DeploymentContext SC, BuildCommand Command)
	{
		this.EngineDirectory = EngineDirectory;
		this.Command = Command;
		this.ConfigHelper = ConfigHelper;

		bStageShaderDirs = ConfigHelper.GetBool("bStageShaderDirs");
		bStagePlatformBuildDirs = ConfigHelper.GetBool("bStagePlatformBuildDirs");
		bStageExtrasDirs = ConfigHelper.GetBool("bStageExtrasDirs");
		bStagePlatformDirs = ConfigHelper.GetBool("bStagePlatformDirs");
		bStageUAT = ConfigHelper.GetBool("bStageUAT");
		bIsForExternalDistribution = ConfigHelper.GetBool("bIsForExternalDistribution");

		// cache some useful properties
		ProjectDirectory = Params.RawProjectPath.Directory;
		ProjectName = Params.RawProjectPath.GetFileNameWithoutAnyExtensions();
		IniPlatformName = ConfigHierarchy.GetIniPlatformName(SC.StageTargetPlatform.PlatformType);
		bIsDLC = Params.DLCFile != null;

		// cache info for DLC against a release
		if (Params.BasedOnReleaseVersionPathOverride != null)
		{
			ReleaseMetadataLocation = DirectoryReference.Combine(new DirectoryReference(Params.BasedOnReleaseVersionPathOverride), "Metadata");
		}
		else
		{
			ReleaseMetadataLocation = SC.MetadataDir;
		}

		// by default, the files are in the cooked data location (which is SC.Metadatadir)
		CachedEditorDataLocation = SC.MetadataDir;
	}

	public void Apply(DeploymentContext SC)
	{
		if (bIsDLC)
		{
			// remove files that we are about to stage that were already in the shipped client
			RemoveReleasedFiles(SC);
		}

		// maps can't be cooked and loaded by the editor, so make sure no cooked ones exist
		UncookMaps(SC);
		UnUFSFiles(SC);

		// anything we want to be NonUFS make sure is not alreay UFS
		UFSFilesToStage.RemoveAll(x => NonUFSFilesToStage.Contains(x));

		Dictionary<StagedFileReference, FileReference> StagedUFSFiles = MimicStageFiles(SC, UFSFilesToStage);
		Dictionary<StagedFileReference, FileReference> StagedNonUFSFiles = MimicStageFiles(SC, NonUFSFilesToStage);
		Dictionary<StagedFileReference, FileReference> StagedUncookFiles = MimicStageFiles(SC, FilesToUncook);

		// filter out already-cooked assets
		foreach (var CookedFile in SC.FilesToStage.UFSFiles)
		{
			// remove any of the entries in the "staged" UFSFilesToStage that match already staged files
			// we don't check extension here because the UFSFilesToStage should only contain .uasset/.umap files, and not .uexp, etc, 
			// and .uasset/.umap files are going to be in SC.FilesToStage
			StagedUFSFiles.Remove(CookedFile.Key);
		}

		// remove already-cooked assets to be replaced with 
		string[] CookedExtensions = { ".uasset", ".umap", ".ubulk", ".uexp" };
		foreach (var UncookedFile in StagedUncookFiles)
		{
			string PathWithNoExtension = Path.ChangeExtension(UncookedFile.Key.Name, null);
			// we need to remove cooked files that match the files to Uncook, and there can be several extensions
			// for each source asset, so remove them all
			foreach (string CookedExtension in CookedExtensions)
			{
				StagedFileReference PathWithExtension = new StagedFileReference(PathWithNoExtension + CookedExtension);
				SC.FilesToStage.UFSFiles.Remove(PathWithExtension);
				StagedUFSFiles.Remove(PathWithExtension);
			}
		}

		// stage the filtered UFSFiles
		SC.StageFiles(StagedFileType.UFS, StagedUFSFiles.Values);

		// stage the Uncooked files now that any cooked ones are removed from SC
		SC.StageFiles(StagedFileType.UFS, StagedUncookFiles.Values);

		// stage the processed NonUFSFiles
		SC.StageFiles(StagedFileType.NonUFS, StagedNonUFSFiles.Values);

		// now remove or allow restricted files
		HandleRestrictedFiles(SC, ref SC.FilesToStage.UFSFiles);
		HandleRestrictedFiles(SC, ref SC.FilesToStage.NonUFSFiles);

		// remove UFS files if they are also in NonUFS - no need to duplicate
		SC.FilesToStage.UFSFiles = SC.FilesToStage.UFSFiles.Where(x => !SC.FilesToStage.NonUFSFiles.ContainsKey(x.Key)).ToDictionary(x => x.Key, x => x.Value);
	}
	#region Private implementation

	private StagedFileReference MakeRelativeStagedReference(DeploymentContext SC, FileSystemReference Ref)
	{
		return MakeRelativeStagedReference(SC, Ref, out _);
	}

	private StagedFileReference MakeRelativeStagedReference(DeploymentContext SC, FileSystemReference Ref, out DirectoryReference RootDir)
	{
		if (Ref.IsUnderDirectory(ProjectDirectory))
		{
			RootDir = ProjectDirectory;
			return Project.ApplyDirectoryRemap(SC, new StagedFileReference(ProjectName + "/" + Ref.MakeRelativeTo(ProjectDirectory).Replace('\\', '/')));
		}
		else if (Ref.IsUnderDirectory(EngineDirectory))
		{
			RootDir = EngineDirectory;
			return Project.ApplyDirectoryRemap(SC, new StagedFileReference( "Engine/" + Ref.MakeRelativeTo(EngineDirectory).Replace('\\', '/')));
		}
		throw new Exception();
	}

	private FileReference UnmakeRelativeStagedReference(DeploymentContext SC, StagedFileReference Ref)
	{
		// paths will be in the form "Engine/Foo" or "{ProjectName}/Foo" (or something that we don't handle, so assert)
		// So, replace the Engine/ with {EngineDir} and {ProjectName}/ with {ProjectDir}, and then append Foo
		if (Ref.Name.StartsWith("Engine/"))
		{
			// skip over "Engine/" which is 7 chars long
			return FileReference.Combine(EngineDirectory, Ref.Name.Substring(7));
		}
		else if (Ref.Name.StartsWith(ProjectName + "/"))
		{
			return FileReference.Combine(ProjectDirectory, Ref.Name.Substring(ProjectName.Length + 1));
		}
		throw new Exception();
	}

	private void RemoveReleasedFiles(DeploymentContext SC)
	{
		HashSet<StagedFileReference> ShippedFiles = new HashSet<StagedFileReference>();
		Action<string, string> FindShippedFiles = (string ParamName, string FileNamePortion) =>
		{
			FileReference UFSManifestFile = Command.ParseOptionalFileReferenceParam(ParamName);
			if (UFSManifestFile == null)
			{
				UFSManifestFile = FileReference.Combine(ReleaseMetadataLocation, $"Manifest_{FileNamePortion}_{SC.StageTargetPlatform.PlatformType}.txt");
			}
			if (FileReference.Exists(UFSManifestFile))
			{
				foreach (string Line in File.ReadAllLines(UFSManifestFile.FullName))
				{
					string[] Tokens = Line.Split("\t".ToCharArray());
					if (Tokens?.Length > 1)
					{
						ShippedFiles.Add(new StagedFileReference(Tokens[0]));
					}
				}
			}
		};

		FindShippedFiles("ClientUFSManifest", "UFSFiles");
		FindShippedFiles("ClientNonUFSManifest", "NonUFSFiles");
		FindShippedFiles("ClientDebugManifest", "DebugFiles");

		ShippedFiles.RemoveWhere(x => x.HasExtension(".ttf") && !x.Name.Contains("LastResort"));

		var RemappedNonUFS = NonUFSFilesToStage.Select(x => MakeRelativeStagedReference(SC, x));

		UFSFilesToStage.RemoveAll(x => ShippedFiles.Contains(MakeRelativeStagedReference(SC, x)));
		NonUFSFilesToStage.RemoveAll(x => ShippedFiles.Contains(MakeRelativeStagedReference(SC, x)));
	}
	private Dictionary<StagedFileReference, FileReference> MimicStageFiles(DeploymentContext SC, List<FileReference> SourceFiles)
	{
		Dictionary<StagedFileReference, FileReference> Mapping = new Dictionary<StagedFileReference, FileReference>();

		foreach (FileReference FileRef in new HashSet<FileReference>(SourceFiles))
		{
			DirectoryReference RootDir;
			StagedFileReference StagedFile = MakeRelativeStagedReference(SC, FileRef, out RootDir);

			// add the mapping
			Mapping.Add(StagedFile, FileRef);
		}

		return Mapping;
	}

	private void HandleRestrictedFiles(DeploymentContext SC, ref Dictionary<StagedFileReference, FileReference> Files)
	{
		if (bIsForExternalDistribution)
		{
			// remove entries where any restricted folder names are in the name remapped path (if we remap from NFL to non-NFL, then we don't remove it)
			Files = Files.Where(x => !SC.RestrictedFolderNames.Any(y => Project.ApplyDirectoryRemap(SC, x.Key).ContainsName(y))).ToDictionary(x => x.Key, x => x.Value);
		}
		else
		{
			Log.TraceInformationOnce("Allowing restricted directories to be staged...");
		}
	}

	private void AddUFSFilesToList(List<FileReference> FileList, string Extension, DeploymentContext SC)
	{
		// look in SC and UFSFiles
		FileList.AddRange(SC.FilesToStage.UFSFiles.Keys.Where(x => x.HasExtension(Extension)).Select(y => UnmakeRelativeStagedReference(SC, y)));
		FileList.AddRange(UFSFilesToStage.Where(x => x.GetExtension().Equals(Extension, StringComparison.InvariantCultureIgnoreCase)));
	}

	private void UncookMaps(DeploymentContext SC)
	{
		string CookedMapMode = ConfigHelper.GetString("MapMode").ToLower();
		if (CookedMapMode == "cooked")
		{
			// nothing to do, they are already staged as cooked as normal
		}
		else if (CookedMapMode == "uncooked")
		{
			// remove maps from SC and Context (SC has path to the cooked map, so we have to come back from Staged refernece that doesn't have the Cooked dir in it)
			AddUFSFilesToList(FilesToUncook, ".umap", SC);
		}
		else if (CookedMapMode == "none")
		{
			// remove umaps and their sidecar files so they won't be staged (remove extension so that we remove Foo.umap and Foo.uexp)
			// also remove Foo_BuiltData.*
			HashSet<string> Maps = UFSFilesToStage.Where(x => x.HasExtension("umap")).Select(x => Path.ChangeExtension(x.FullName, null)).ToHashSet();
			UFSFilesToStage = UFSFilesToStage.Where(x => !Maps.Contains(Path.ChangeExtension(x.FullName.Replace("_BuiltData", ""), null))).ToList();

			Maps = SC.FilesToStage.UFSFiles.Keys.Where(x => x.HasExtension("umap")).Select(x => Path.ChangeExtension(x.Name, null)).ToHashSet();
			Console.WriteLine($"Found {Maps.Count()} maps");
			SC.FilesToStage.UFSFiles = SC.FilesToStage.UFSFiles.Where(x => !Maps.Contains(Path.ChangeExtension(x.Key.Name.Replace("_BuiltData", ""), null))).ToDictionary(x => x.Key, x => x.Value);

		}
	}

	private void UnUFSFiles(DeploymentContext SC)
	{
		if (bStageUAT)
		{
			// UAT needs uplugin and ini files, so make sure they are not in the .pak
			AddUFSFilesToList(NonUFSFilesToStage, ".uplugin", SC);
			AddUFSFilesToList(NonUFSFilesToStage, ".ini", SC);
			NonUFSFilesToStage = NonUFSFilesToStage.Where(x => x.GetFileName() != "BinaryConfig.ini").ToList();
		}
	}

	#endregion
}


public class MakeCookedEditor : BuildCommand
{
	protected bool bIsCookedCooker;
	protected FileReference ProjectFile;

	protected ConfigHelper ConfigHelper;

	public override void ExecuteBuild()
	{
		LogInformation("************************* MakeCookedEditor");

		bIsCookedCooker = ParseParam("cookedcooker");
		ProjectFile = ParseProjectParam();

		// set up config sections and the like
		ConfigHelper = new ConfigHelper(BuildHostPlatform.Current.Platform, ProjectFile, bIsCookedCooker);


		ProjectParams BuildParams = GetParams();

		LogInformation("Build? {0}", BuildParams.Build);

		Project.Build(this, BuildParams);
		Project.Cook(BuildParams);
		Project.CopyBuildToStagingDirectory(BuildParams);

		//this will do packaging if requested, and also symbol upload if requested.
		Project.Package(BuildParams);

		Project.Archive(BuildParams);
		PrintRunTime();
		Project.Deploy(BuildParams);


	}

	protected virtual void StageEngineEditorFiles(ProjectParams Params, DeploymentContext SC, ModifyStageContext Context)
	{
		StagePlatformExtensionFiles(Params, SC, Context, Unreal.EngineDirectory);
		StagePluginFiles(Params, SC, Context, true);

		// engine shaders
		if (Context.bStageShaderDirs)
		{
			Context.NonUFSFilesToStage.AddRange(DirectoryReference.EnumerateFiles(DirectoryReference.Combine(Unreal.EngineDirectory, "Shaders"), "*", SearchOption.AllDirectories));
			GatherTargetDependencies(Params, SC, Context, "ShaderCompileWorker");
		}
		if (bIsCookedCooker)
		{
			GatherTargetDependencies(Params, SC, Context, "UnrealPak");
		}

		StageIniPathArray(Params, SC, "EngineExtraStageFiles", Unreal.EngineDirectory, Context);

		Context.FilesToUncook.Add(FileReference.Combine(Context.EngineDirectory, "Content", "EngineMaterials", "DefaultMaterial.uasset"));
	}

	protected virtual void StageProjectEditorFiles(ProjectParams Params, DeploymentContext SC, ModifyStageContext Context)
	{
		// always stage the main exe, in case DLC mode is on, then it won't by default
		if (SC.StageExecutables.Count > 0)
		{
			GatherTargetDependencies(Params, SC, Context, SC.StageExecutables[0]);
		}


		StagePlatformExtensionFiles(Params, SC, Context, Context.ProjectDirectory);
		StagePluginFiles(Params, SC, Context, false);

		// add stripped out editor .ini files back in
		Context.UFSFilesToStage.AddRange(DirectoryReference.EnumerateFiles(DirectoryReference.Combine(Context.ProjectDirectory, "Config"), "*Editor*", SearchOption.AllDirectories));

		StageIniPathArray(Params, SC, "ProjectExtraStageFiles", Context.ProjectDirectory, Context);

		if (!bIsCookedCooker)
		{
			// the editor AR may be named EditorClientAssetRegistry.bin already, but probably is DevelopmentAssetRegistry.bin, so look for both, and name it EditorClientAssetRegistry
			FileReference EditorAR = FileReference.Combine(Context.CachedEditorDataLocation, "EditorClientAssetRegistry.bin");
			if (!FileReference.Exists(EditorAR))
			{
				EditorAR = FileReference.Combine(Context.CachedEditorDataLocation, "DevelopmentAssetRegistry.bin");
			}
			SC.StageFile(StagedFileType.UFS, EditorAR, new StagedFileReference($"{Context.ProjectName}/EditorClientAssetRegistry.bin"));

			// this file is optional
			FileReference EditorThumbnails = FileReference.Combine(Context.CachedEditorDataLocation, "CachedEditorThumbnails.bin");
			if (FileReference.Exists(EditorThumbnails))
			{
				SC.StageFile(StagedFileType.UFS, EditorThumbnails, new StagedFileReference($"{Context.ProjectName}/CachedEditorThumbnails.bin"));
			}
		}
	}

	static void ReadProjectsRecursively(FileReference File, Dictionary<string, string> InitialProperties, Dictionary<FileReference, CsProjectInfo> FileToProjectInfo)
	{
		// Early out if we've already read this project
		if (!FileToProjectInfo.ContainsKey(File))
		{
			// Try to read this project
			CsProjectInfo ProjectInfo;
			if (!CsProjectInfo.TryRead(File, InitialProperties, out ProjectInfo))
			{
				throw new AutomationException("Couldn't read project '{0}'", File.FullName);
			}

			// Add it to the project lookup, and try to read all the projects it references
			FileToProjectInfo.Add(File, ProjectInfo);
			foreach (FileReference ProjectReference in ProjectInfo.ProjectReferences.Keys)
			{
				if (!FileReference.Exists(ProjectReference))
				{
					throw new AutomationException("Unable to find project '{0}' referenced by '{1}'", ProjectReference, File);
				}
				ReadProjectsRecursively(ProjectReference, InitialProperties, FileToProjectInfo);
			}
		}
	}

	protected virtual void StageUAT(ProjectParams Params, DeploymentContext SC, ModifyStageContext Context)
	{
		// now look in the .json file that UAT made that lists its script files
		DirectoryReference AutomationToolBinaryDir = DirectoryReference.Combine(Context.EngineDirectory, "Binaries", "DotNET", "AutomationTool");
		DirectoryReference UnrealBuildToolBinaryDir = DirectoryReference.Combine(Context.EngineDirectory, "Binaries", "DotNET", "UnrealBuildTool");
		DirectoryReference ProjectAutomationToolBinaryDir = DirectoryReference.Combine(Context.ProjectDirectory, "Binaries", "DotNET", "AutomationTool");

		// some netcore dependencies in the tool directories can't be discovered with CsProjectInfo, so just stage the enture UBT and UAT directories
		Context.NonUFSFilesToStage.AddRange(DirectoryReference.EnumerateFiles(UnrealBuildToolBinaryDir, "*", SearchOption.AllDirectories));
		Context.NonUFSFilesToStage.AddRange(DirectoryReference.EnumerateFiles(AutomationToolBinaryDir, "*", SearchOption.AllDirectories));

		StagedDirectoryReference StagedBinariesDir = new StagedDirectoryReference("Engine/Binaries/DotNET/AutomationTool");

		// look in Engine/Intermediate/ScriptModules and Project/Intermediate/ScriptModules
		DirectoryReference EngineScriptModulesDir = DirectoryReference.Combine(Context.EngineDirectory, "Intermediate", "ScriptModules");
		DirectoryReference ProjectScriptModulesDir = DirectoryReference.Combine(Context.ProjectDirectory, "Intermediate", "ScriptModules");
		IEnumerable<FileReference> JsonFiles = DirectoryReference.EnumerateFiles(EngineScriptModulesDir);
		if (DirectoryReference.Exists(ProjectScriptModulesDir))
		{
			JsonFiles = JsonFiles.Concat(DirectoryReference.EnumerateFiles(ProjectScriptModulesDir));
		}
		foreach (FileReference JsonFile in JsonFiles)
		{
			try
			{
				// load build info 
				CsProjBuildRecord BuildRecord = JsonSerializer.Deserialize<CsProjBuildRecord>(FileReference.ReadAllText(JsonFile));

				Context.NonUFSFilesToStage.Add(JsonFile);

				// get location of the project where the other paths are relative to
				DirectoryReference RecordRoot = FileReference.Combine(JsonFile.Directory, BuildRecord.ProjectPath).Directory;
				string FullPath = RecordRoot.FullName;

				// stage the output and everything it pulled in next to it
				foreach (FileReference TargetDirFile in DirectoryReference.EnumerateFiles(FileReference.Combine(RecordRoot, BuildRecord.TargetPath).Directory, "*", SearchOption.AllDirectories))
				{
					Context.NonUFSFilesToStage.Add(TargetDirFile);
				}

				// now pull in any dependencies in case something loads it by path
				foreach (string Dep in BuildRecord.Dependencies)
				{
					if (Path.GetExtension(Dep).ToLower() == ".dll")
					{
						FileReference DepFile = FileReference.Combine(RecordRoot, Dep);
						// if ht's not in the engine or the project, we will just stage it next to the .exe, in a last ditch effort
						if (!DepFile.IsUnderDirectory(Context.EngineDirectory) && !DepFile.IsUnderDirectory(Context.ProjectDirectory))
						{
							SC.StageFile(StagedFileType.NonUFS, DepFile, StagedFileReference.Combine(StagedBinariesDir, DepFile.GetFileName()));
						}
						else
						{
							Context.NonUFSFilesToStage.Add(DepFile);
						}
					}
				}
			}
			catch(Exception)
			{
				// skip json files that fail
			}
		}

		if (Context.IniPlatformName == "Linux")
		{
			// linux needs dotnet runtime
			SC.StageFiles(StagedFileType.NonUFS, DirectoryReference.Combine(Context.EngineDirectory, "Binaries", "ThirdParty", "DotNet", Context.IniPlatformName), StageFilesSearch.AllDirectories);
		}

		// not sure if we need this or not now

		// ask each platform if they need extra files
		//foreach (UnrealTargetPlatform Platform in UnrealTargetPlatform.GetValidPlatforms())
		//{
		//	List<FileReference> Files = new List<FileReference>();
		//	AutomationTool.Platform.GetPlatform(Platform).GetPlatformUATDependencies(Context.ProjectDirectory, Files);
		//	Context.NonUFSFilesToStage.AddRange(Files.Where(x => FileReference.Exists(x)));
		//}
	}

	protected virtual void StagePluginDirectory(DirectoryReference PluginDir, ModifyStageContext Context, bool bStageUncookedContent)
	{
		foreach (DirectoryReference Subdir in DirectoryReference.EnumerateDirectories(PluginDir))
		{
			StagePluginSubdirectory(Subdir, Context, bStageUncookedContent);
		}
	}

	protected virtual void StagePluginSubdirectory(DirectoryReference PluginSubdir, ModifyStageContext Context, bool bStageUncookedContent)
	{
		string DirNameLower = PluginSubdir.GetDirectoryName().ToLower();

		if (DirNameLower == "content")
		{
			if (bStageUncookedContent)
			{
				Context.FilesToUncook.AddRange(DirectoryReference.EnumerateFiles(PluginSubdir, "*", SearchOption.AllDirectories));
			}
			else
			{
				Context.UFSFilesToStage.AddRange(DirectoryReference.EnumerateFiles(PluginSubdir, "*", SearchOption.AllDirectories));
			}
		}
		else if (DirNameLower == "resources" || DirNameLower == "config" || DirNameLower == "scripttemplates")
		{
			Context.UFSFilesToStage.AddRange(DirectoryReference.EnumerateFiles(PluginSubdir, "*", SearchOption.AllDirectories));
		}
		else if (DirNameLower == "shaders" && Context.bStageShaderDirs)
		{
			Context.NonUFSFilesToStage.AddRange(DirectoryReference.EnumerateFiles(PluginSubdir, "*", SearchOption.AllDirectories));
		}
	}

	protected virtual ModifyStageContext CreateContext(ProjectParams Params, DeploymentContext SC)
	{
		return new ModifyStageContext(ConfigHelper, Unreal.EngineDirectory, Params, SC, this);
	}

	protected virtual void ModifyParams(ProjectParams BuildParams)
	{
	}

	protected virtual void PreModifyDeploymentContext(ProjectParams Params, DeploymentContext SC)
	{
		ModifyStageContext Context = CreateContext(Params, SC);

		DefaultPreModifyDeploymentContext(Params, SC, Context);

		Context.Apply(SC);
	}

	protected virtual void ModifyDeploymentContext(ProjectParams Params, DeploymentContext SC)
	{
		ModifyStageContext Context = CreateContext(Params, SC);

		DefaultModifyDeploymentContext(Params, SC, Context);

		Context.Apply(SC);

		// we do this after the apply to make sure we get any SC and Context based staging
		if (bIsCookedCooker)
		{
			// cooker can run with just the -Cmd, so we reduce the size byt removing the non-Cmd executable and debug info (this is sizeable for monolithic editors)
			string MainCookedTarget = Params.ServerCookedTargets[0];
			// @todo mac
			SC.FilesToStage.NonUFSFiles.Remove(new StagedFileReference(Path.Combine(Context.ProjectName, "Binaries", "Win64", MainCookedTarget + ".exe")));
			SC.FilesToStage.NonUFSFiles.Remove(new StagedFileReference(Path.Combine(Context.ProjectName, "Binaries", "Linux", MainCookedTarget)));
			SC.FilesToStage.NonUFSDebugFiles.Remove(new StagedFileReference(Path.Combine(Context.ProjectName, "Binaries", "Win64", MainCookedTarget + ".pdb")));
			SC.FilesToStage.NonUFSDebugFiles.Remove(new StagedFileReference(Path.Combine(Context.ProjectName, "Binaries", "Linux", MainCookedTarget + ".sym")));
		}

	}

	protected virtual void SetupDLCMode(FileReference ProjectFile, out string DLCName, out string ReleaseVersion, out TargetType Type)
	{
		bool bBuildAgainstRelease = ConfigHelper.GetBool("bBuildAgainstRelease");
		if (bBuildAgainstRelease)
		{
			DLCName = ConfigHelper.GetString("DLCPluginName");
			ReleaseVersion = ConfigHelper.GetString("ReleaseName");

			// if not set, default to gamename
			if (string.IsNullOrEmpty(ReleaseVersion))
			{
				ReleaseVersion = ProjectFile.GetFileNameWithoutAnyExtensions();
			}

			string TargetTypeString;
			TargetTypeString = ConfigHelper.GetString("ReleaseTargetType");
			Type = (TargetType)Enum.Parse(typeof(TargetType), TargetTypeString);
		}
		else
		{
			DLCName = null;
			ReleaseVersion = null;
			Type = TargetType.Game;
		}
	}







	protected void StagePlatformExtensionFiles(ProjectParams Params, DeploymentContext SC, ModifyStageContext Context, DirectoryReference RootDir)
	{
		if (!Context.bStagePlatformDirs)
		{
			return;
		}


		// plugins are already handled in the Plugins staging code
		List<string> RootFoldersToStrip = new List<string> { "source", "plugins" };//, "binaries" };
		List<string> SubFoldersToStrip = new List<string> { "source", "intermediate", "tests", "binaries" + Path.DirectorySeparatorChar + HostPlatform.Current.HostEditorPlatform.ToString().ToLower() };
		List<string> RootNonUFSFolders = new List<string> { "shaders", "binaries", "build", "extras" };


		if (!Context.bStageShaderDirs)
		{
			RootFoldersToStrip.Add("shaders");
		}
		if (!Context.bStagePlatformBuildDirs)
		{
			RootFoldersToStrip.Add("build");
		}
		if (!Context.bStageExtrasDirs)
		{
			RootFoldersToStrip.Add("extras");
		}

		foreach (DirectoryReference PlatformDir in Unreal.GetExtensionDirs(RootDir, true, false, false))
		{
			foreach (DirectoryReference Subdir in DirectoryReference.EnumerateDirectories(PlatformDir, "*", SearchOption.TopDirectoryOnly))
			{
				string SubdirName = Subdir.GetDirectoryName().ToLower();

				// Remvoe some unnecessary folders that can be large
				List<FileReference> ContextFileList = Context.UFSFilesToStage;

				// some files need to be NonUFS for C# etc to access
				if (RootNonUFSFolders.Contains(SubdirName))
				{
					ContextFileList = Context.NonUFSFilesToStage;
				}

				List<FileReference> FilesToStage = new List<FileReference>();
				// if we aren't in a bad subdir, add files
				if (!RootFoldersToStrip.Contains(SubdirName))
				{
					FilesToStage.AddRange(DirectoryReference.EnumerateFiles(Subdir, "*", SearchOption.AllDirectories));

					// now remove files in subdirs we want to skip
					FilesToStage.RemoveAll(x => x.ContainsAnyNames(SubFoldersToStrip, Subdir));
					ContextFileList.AddRange(FilesToStage);
				}
			}
		}
	}

	protected void StagePluginFiles(ProjectParams Params, DeploymentContext SC, ModifyStageContext Context, bool bEnginePlugins)
	{
		List<FileReference> ActivePlugins = new List<FileReference>();
		foreach (StageTarget Target in SC.StageTargets)
		{
			if (Target.Receipt.TargetType == TargetType.Editor)
			{
				IEnumerable<RuntimeDependency> TargetPlugins = Target.Receipt.RuntimeDependencies.Where(x => x.Path.GetExtension().ToLower() == ".uplugin");
				// grab just engine plugins, or non-engine plugins depending
				TargetPlugins = TargetPlugins.Where(x => (bEnginePlugins ? x.Path.IsUnderDirectory(Unreal.EngineDirectory) : !x.Path.IsUnderDirectory(Unreal.EngineDirectory)));

				// convert to paths
				ActivePlugins.AddRange(TargetPlugins.Select(x => x.Path));
			}
		}

		foreach (FileReference ActivePlugin in ActivePlugins)
		{
			PluginInfo Plugin = new PluginInfo(ActivePlugin, bEnginePlugins ? PluginType.Engine : PluginType.Project);
			// we don't cook for unsupported target platforms, but the plugin may still need to be used in the editor, so
			// stage uncooked assets for these plugins
			bool bStageUncookedContent = (!Plugin.Descriptor.SupportsTargetPlatform(SC.StageTargetPlatform.PlatformType));

			StagePluginDirectory(ActivePlugin.Directory, Context, bStageUncookedContent);
		}

	}

	protected void StageIniPathArray(ProjectParams Params, DeploymentContext SC, string IniKey, DirectoryReference BaseDirectory, ModifyStageContext Context)
	{
		HashSet<string> Entries = new HashSet<string>();

		// read the ini for all platforms, and merge together to remove duplicates
		foreach (UnrealTargetPlatform Platform in UnrealTargetPlatform.GetValidPlatforms())
		{
			ConfigHelper PlatformHelper = new ConfigHelper(Platform, ProjectFile, bIsCookedCooker);
			Entries.UnionWith(ConfigHelper.GetArray(IniKey));
		}

		foreach (string Entry in Entries)
		{
			Dictionary<string, string> Props = ParseStructProperties(Entry);

			string SubPath = Props["Path"];
			string FileWildcard = "*";
			List<FileReference> FileList = Context.UFSFilesToStage;
			SearchOption SearchMode = SearchOption.AllDirectories;
			if (Props.ContainsKey("Files"))
			{
				FileWildcard = Props["Files"];
			}
			if (Props.ContainsKey("NonUFS") && bool.Parse(Props["NonUFS"]) == true)
			{
				FileList = Context.NonUFSFilesToStage;
			}
			if (Props.ContainsKey("Recursive") && bool.Parse(Props["Recursive"]) == false)
			{
				SearchMode = SearchOption.TopDirectoryOnly;
			}

			// now enumerate files based on the settings
			DirectoryReference Dir = DirectoryReference.Combine(BaseDirectory, SubPath);
			if (DirectoryReference.Exists(Dir))
			{
				FileList.AddRange(DirectoryReference.EnumerateFiles(Dir, FileWildcard, SearchMode));
			}
		}
	}


	protected void DefaultPreModifyDeploymentContext(ProjectParams Params, DeploymentContext SC, ModifyStageContext Context)
	{

	}
	protected void DefaultModifyDeploymentContext(ProjectParams Params, DeploymentContext SC, ModifyStageContext Context)
	{
		// this will make sure that uncooked packages (maps, etc) go into the .pak, NOT the IOStore, which will fail to package them from a different location
		SC.OnlyAllowPackagesFromStdCookPathInIoStore = true;

		// if this is for internal use, then we allow all restricted  directories and ini settings
		if (!Context.bIsForExternalDistribution)
		{
			SC.RestrictedFolderNames.Clear();

			if (SC.IniKeyDenyList != null)
			{
				SC.IniKeyDenyList.Clear();
			}
			if (SC.IniSectionDenyList != null)
			{
				SC.IniSectionDenyList.Clear();
			}
		}

		StageEngineEditorFiles(Params, SC, Context);
		StageProjectEditorFiles(Params, SC, Context);

		// we need a better decision for this
		if (Context.bStageUAT)
		{
			StageUAT(Params, SC, Context);
		}


		// final filtering

		// we already cooked assets, so remove assets we may have found, except for the Uncook ones
		Context.UFSFilesToStage.RemoveAll(x => x.GetExtension() == ".uasset");

		// don't need the .target files
		Context.NonUFSFilesToStage.RemoveAll(x => x.GetExtension() == ".target");

		if (!Context.bStageShaderDirs)
		{
			// don't need standalone shaders
			Context.UFSFilesToStage.RemoveAll(x => x.GetExtension() == ".glsl");
			Context.UFSFilesToStage.RemoveAll(x => x.GetExtension() == ".hlsl");
		}

		// move some files from UFS to NonUFS if they ended up there
		List<string> UFSIncompatibleExtensions = new List<string> { ".py", ".pyc" };
		Context.NonUFSFilesToStage.AddRange(Context.UFSFilesToStage.Where(x => UFSIncompatibleExtensions.Contains(x.GetExtension())));
		Context.UFSFilesToStage.RemoveAll(x => UFSIncompatibleExtensions.Contains(x.GetExtension()));
	}

	private ProjectParams GetParams()
	{
		// setup DLC defaults, then ask project if it should 
		string DLCName;
		string BasedOnReleaseVersion;
		TargetType ReleaseType;
		SetupDLCMode(ProjectFile, out DLCName, out BasedOnReleaseVersion, out ReleaseType);

		var Params = new ProjectParams
		(
			Command: this,
			RawProjectPath: ProjectFile

			// standard cookededitor settings
			//			, Client:false
			//			, EditorTargets: new ParamList<string>()
			// , SkipBuildClient: true
			, NoBootstrapExe: true
			// , Client: true
			, DLCName: DLCName
			, BasedOnReleaseVersion: BasedOnReleaseVersion
			, DedicatedServer: bIsCookedCooker
			, NoClient: bIsCookedCooker
		);

		string TargetPlatformType = bIsCookedCooker ? "CookedCooker" : "CookedEditor";
		string TargetName = ConfigHelper.GetString(bIsCookedCooker ? "CookedCookerTargetName" : "CookedEditorTargetName");
		UnrealTargetPlatform Platform;

		// look to see if ini didn't override target name
		if (string.IsNullOrEmpty(TargetName))
		{
			// if not, then use ProjectCookedEditor
			TargetName = ProjectFile.GetFileNameWithoutAnyExtensions() + TargetPlatformType;
		}

		// cook the cooked editor targetplatorm as the "client"
		//Params.ClientCookedTargets.Add("CrashReportClientEditor");

		// control the server/client taregts
		Params.ServerCookedTargets.Clear();
		Params.ClientCookedTargets.Clear();
		if (bIsCookedCooker)
		{
			Platform = Params.ServerTargetPlatforms[0].Type;
			Params.EditorTargets.Add(TargetName);
			Params.ServerCookedTargets.Add(TargetName);
			Params.ServerTargetPlatforms = new List<TargetPlatformDescriptor>() { new TargetPlatformDescriptor(Platform, TargetPlatformType) };
		}
		else
		{
			Platform = Params.ClientTargetPlatforms[0].Type;
			Params.ClientCookedTargets.Add(TargetName);
			Params.ClientTargetPlatforms = new List<TargetPlatformDescriptor>() { new TargetPlatformDescriptor(Platform, TargetPlatformType) };
		}


		// when making cooked editors, we some special commandline options to override some assumptions about editor data
		Params.AdditionalCookerOptions += " -ini:Engine:[Core.System]:CanStripEditorOnlyExportsAndImports=False";
		// We tend to "over-cook" packages to get everything we might need, so some non-editor BPs that are referencing editor BPs may
		// get cooked. This is okay, because the editor stuff should exist. We may want to revist this, and not cook anything that would
		// cause the issues
		Params.AdditionalCookerOptions += " -AllowUnsafeBlueprintCalls";

		// Params.AdditionalCookerOptions += " -NoFilterAssetRegistry";

		// set up cooking against a client, as DLC
		if (BasedOnReleaseVersion != null)
		{
			// make the platform name, like "WindowsClient", or "LinuxGame", of the premade build we are cooking/staging against
			string IniPlatformName = ConfigHierarchy.GetIniPlatformName(Platform);
			string ReleaseTargetName = IniPlatformName + (ReleaseType == TargetType.Game ? "NoEditor" : ReleaseType.ToString());

			Params.AdditionalCookerOptions += " -CookAgainstFixedBase";
			Params.AdditionalCookerOptions += $" -DevelopmentAssetRegistryPlatformOverride={ReleaseTargetName}";
			Params.AdditionalIoStoreOptions += $" -DevelopmentAssetRegistryPlatformOverride={ReleaseTargetName}";

			// point to where the premade asset registry can be found
			Params.BasedOnReleaseVersionPathOverride = CommandUtils.CombinePaths(ProjectFile.Directory.FullName, "Releases", BasedOnReleaseVersion, ReleaseTargetName);

			Params.DLCOverrideStagedSubDir = "";
			Params.DLCIncludeEngineContent = true;

		}



		// set up override functions
		Params.PreModifyDeploymentContextCallback = new Action<ProjectParams, DeploymentContext>((ProjectParams P, DeploymentContext SC) => { PreModifyDeploymentContext(P, SC); });
		Params.ModifyDeploymentContextCallback = new Action<ProjectParams, DeploymentContext>((ProjectParams P, DeploymentContext SC) => { ModifyDeploymentContext(P, SC); });

		ModifyParams(Params);

		return Params;
	}



	protected static void GatherTargetDependencies(ProjectParams Params, DeploymentContext SC, ModifyStageContext Context, string ReceiptName)
	{
		GatherTargetDependencies(Params, SC, Context, ReceiptName, UnrealTargetConfiguration.Development);
	}

	protected static void GatherTargetDependencies(ProjectParams Params, DeploymentContext SC, ModifyStageContext Context, string ReceiptName, UnrealTargetConfiguration Configuration)
	{
		string Architecture = Params.SpecifiedArchitecture;
		if (string.IsNullOrEmpty(Architecture))
		{
			Architecture = "";
			if (PlatformExports.IsPlatformAvailable(SC.StageTargetPlatform.IniPlatformType))
			{
				Architecture = PlatformExports.GetDefaultArchitecture(SC.StageTargetPlatform.IniPlatformType, Params.RawProjectPath);
			}
		}

		FileReference ReceiptFilename = TargetReceipt.GetDefaultPath(Params.RawProjectPath.Directory, ReceiptName, SC.StageTargetPlatform.IniPlatformType, Configuration, Architecture);
		if (!FileReference.Exists(ReceiptFilename))
		{
			ReceiptFilename = TargetReceipt.GetDefaultPath(Unreal.EngineDirectory, ReceiptName, SC.StageTargetPlatform.IniPlatformType, Configuration, Architecture);
		}

		TargetReceipt Receipt;
		if (!TargetReceipt.TryRead(ReceiptFilename, out Receipt))
		{
			throw new AutomationException("Missing or invalid target receipt ({0})", ReceiptFilename);
		}

		foreach (BuildProduct BuildProduct in Receipt.BuildProducts)
		{
			Context.NonUFSFilesToStage.Add(BuildProduct.Path);
		}

		foreach (RuntimeDependency RuntimeDependency in Receipt.RuntimeDependencies)
		{
			if (RuntimeDependency.Type == StagedFileType.UFS)
			{
				Context.UFSFilesToStage.Add(RuntimeDependency.Path);
			}
			else// if (RuntimeDependency.Type == StagedFileType.NonUFS)
			{
				Context.NonUFSFilesToStage.Add(RuntimeDependency.Path);
			}
			//else
			//{
			//	// otherwise, just stage it directly
			//	// @todo: add a FilesToStage type to context like SC has?
			//	SC.StageFile(RuntimeDependency.Type, RuntimeDependency.Path);
			//}
		}

		Context.NonUFSFilesToStage.Add(ReceiptFilename);
	}




	// @todo: Move this into UBT or something
	private static Dictionary<string, string> ParseStructProperties(string PropsString)
	{
		// we expect parens around a properly encoded struct
		if (!PropsString.StartsWith("(") || !PropsString.EndsWith(")"))
		{
			return null;
		}
		// strip ()
		PropsString = PropsString.Substring(1, PropsString.Length - 2);

		List<string> Props = new List<string>();

		int TokenStart = 0;
		int StrLen = PropsString.Length;
		while (TokenStart < StrLen)
		{
			// get the next location of each special character
			int NextComma = PropsString.IndexOf(',', TokenStart);
			int NextQuote = PropsString.IndexOf('\"', TokenStart);
			// comma first? easy
			if (NextComma != -1 && NextComma < NextQuote)
			{
				Props.Add(PropsString.Substring(TokenStart, NextComma - TokenStart));
				TokenStart = NextComma + 1;
			}
			// comma but no quotes
			else if (NextComma != -1 && NextQuote == -1)
			{
				Props.Add(PropsString.Substring(TokenStart, NextComma - TokenStart));
				TokenStart = NextComma + 1;
			}
			// neither found, use the rest
			else if (NextComma == -1 && NextQuote == -1)
			{
				Props.Add(PropsString.Substring(TokenStart));
				break;
			}
			// quote first? look for quote after
			else
			{
				NextQuote = PropsString.IndexOf('\"', NextQuote + 1);
				// are we at the end?
				if (NextQuote + 1 == StrLen)
				{
					// use the rest of the string
					Props.Add(PropsString.Substring(TokenStart));
					break;
				}
				// it's expected that the following character is a comma, if not, give up
				if (PropsString[NextQuote + 1] != ',')
				{
					break;
				}
				// if next is comma, we are done this token
				Props.Add(PropsString.Substring(TokenStart, (NextQuote - TokenStart) + 1));
				// skip over the quote and following commma
				TokenStart = NextQuote + 2;
			}
		}

		// now make a dictionary from the properties
		Dictionary<string, string> KeyValues = new Dictionary<string, string>();
		foreach (string AProp in Props)
		{
			string Prop = AProp.Trim(" \t".ToCharArray());
			// find the first = (UE properties can't have an equal sign, so it's valid to do)
			int Equals = Prop.IndexOf('=');
			// we must have one
			if (Equals == -1)
			{
				continue;
			}

			string Key = Prop.Substring(0, Equals);
			string Value = Prop.Substring(Equals + 1);
			// trim off any quotes around the entire value
			Value = Value.Trim(" \"".ToCharArray());
			Key = Key.Trim(" ".ToCharArray());
			KeyValues.Add(Key, Value);
		}

		// convert to array type
		return KeyValues;
	}
}