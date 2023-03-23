// Copyright Epic Games, Inc. All Rights Reserved.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;
using System.Xml;
using System.Xml.Serialization;
using EpicGames.Core;
using UnrealBuildBase;

namespace UnrealBuildTool
{
	/// <summary>
	/// Flags for the PVS analyzer mode
	/// </summary>
	public enum PVSAnalysisModeFlags : uint
	{
		/// <summary>
		/// Check for 64-bit portability issues
		/// </summary>
		Check64BitPortability = 1,

		/// <summary>
		/// Enable general analysis
		/// </summary>
		GeneralAnalysis = 4,

		/// <summary>
		/// Check for optimizations
		/// </summary>
		Optimizations = 8,

		/// <summary>
		/// Enable customer-specific rules
		/// </summary>
		CustomerSpecific = 16,

		/// <summary>
		/// Enable MISRA analysis
		/// </summary>
		MISRA = 32,
	}

	/// <summary>
	/// Partial representation of PVS-Studio main settings file
	/// </summary>
	[XmlRoot("ApplicationSettings")]
	public class PVSApplicationSettings
	{
		/// <summary>
		/// Masks for paths excluded for analysis
		/// </summary>
		public string[]? PathMasks;

		/// <summary>
		/// Registered username
		/// </summary>
		public string? UserName;

		/// <summary>
		/// Registered serial number
		/// </summary>
		public string? SerialNumber;

		/// <summary>
		/// Disable the 64-bit Analysis
		/// </summary>
		public bool Disable64BitAnalysis;

		/// <summary>
		/// Disable the General Analysis
		/// </summary>
		public bool DisableGAAnalysis;

		/// <summary>
		/// Disable the Optimization Analysis
		/// </summary>
		public bool DisableOPAnalysis;

		/// <summary>
		/// Disable the Customer's Specific diagnostic rules
		/// </summary>
		public bool DisableCSAnalysis;

		/// <summary>
		/// Disable the MISRA Analysis
		/// </summary>
		public bool DisableMISRAAnalysis;

		/// <summary>
		/// Gets the analysis mode flags from the settings
		/// </summary>
		/// <returns>Mode flags</returns>
		public PVSAnalysisModeFlags GetModeFlags()
		{
			PVSAnalysisModeFlags Flags = 0;
			if (!Disable64BitAnalysis)
			{
				Flags |= PVSAnalysisModeFlags.Check64BitPortability;
			}
			if (!DisableGAAnalysis)
			{
				Flags |= PVSAnalysisModeFlags.GeneralAnalysis;
			}
			if (!DisableOPAnalysis)
			{
				Flags |= PVSAnalysisModeFlags.Optimizations;
			}
			if (!DisableCSAnalysis)
			{
				Flags |= PVSAnalysisModeFlags.CustomerSpecific;
			}
			if (!DisableMISRAAnalysis)
			{
				Flags |= PVSAnalysisModeFlags.MISRA;
			}
			return Flags;
		}

		/// <summary>
		/// Attempts to read the application settings from the default location
		/// </summary>
		/// <returns>Application settings instance, or null if no file was present</returns>
		internal static PVSApplicationSettings? Read()
		{
			FileReference SettingsPath = FileReference.Combine(new DirectoryReference(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData)), "PVS-Studio", "Settings.xml");
			if (FileReference.Exists(SettingsPath))
			{
				try
				{
					XmlSerializer Serializer = new XmlSerializer(typeof(PVSApplicationSettings));
					using (FileStream Stream = new FileStream(SettingsPath.FullName, FileMode.Open, FileAccess.Read, FileShare.Read))
					{
						return (PVSApplicationSettings)Serializer.Deserialize(Stream);
					}
				}
				catch (Exception Ex)
				{
					throw new BuildException(Ex, "Unable to read PVS-Studio settings file from {0}", SettingsPath);
				}
			}
			return null;
		}
	}

	/// <summary>
	/// Settings for the PVS Studio analyzer
	/// </summary>
	public class PVSTargetSettings
	{
		/// <summary>
		/// Returns the application settings
		/// </summary>
		internal Lazy<PVSApplicationSettings?> ApplicationSettings { get; } = new Lazy<PVSApplicationSettings?>(() => PVSApplicationSettings.Read());

		/// <summary>
		/// Whether to use application settings to determine the analysis mode
		/// </summary>
		public bool UseApplicationSettings { get; set; }

		/// <summary>
		/// Override for the analysis mode to use
		/// </summary>
		public PVSAnalysisModeFlags ModeFlags
		{
			get
 			{
				if (ModePrivate.HasValue)
				{
					return ModePrivate.Value;
				}
				else if (UseApplicationSettings && ApplicationSettings.Value != null)
				{
					return ApplicationSettings.Value.GetModeFlags();
				}
				else
				{
					return PVSAnalysisModeFlags.GeneralAnalysis;
				}
			}
			set
			{
				ModePrivate = value;
			}
		}

		/// <summary>
		/// Private storage for the mode flags
		/// </summary>
		PVSAnalysisModeFlags? ModePrivate;
	}

	/// <summary>
	/// Read-only version of the PVS toolchain settings
	/// </summary>
	public class ReadOnlyPVSTargetSettings
	{
		/// <summary>
		/// Inner settings
		/// </summary>
		PVSTargetSettings Inner;

		/// <summary>
		/// Constructor
		/// </summary>
		/// <param name="Inner">The inner object</param>
		public ReadOnlyPVSTargetSettings(PVSTargetSettings Inner)
		{
			this.Inner = Inner;
		}

		/// <summary>
		/// Accessor for the Application settings
		/// </summary>
		internal PVSApplicationSettings? ApplicationSettings
		{
			get { return Inner.ApplicationSettings.Value; }
		}

		/// <summary>
		/// Whether to use the application settings for the mode
		/// </summary>
		public bool UseApplicationSettings
		{
			get { return Inner.UseApplicationSettings; }
		}

		/// <summary>
		/// Override for the analysis mode to use
		/// </summary>
		public PVSAnalysisModeFlags ModeFlags
		{
			get { return Inner.ModeFlags; }
		}
	}

	/// <summary>
	/// Special mode for gathering all the messages into a single output file
	/// </summary>
	[ToolMode("PVSGather", ToolModeOptions.None)]
	class PVSGatherMode : ToolMode
	{
		/// <summary>
		/// Path to the input file list
		/// </summary>
		[CommandLine("-Input", Required = true)]
		FileReference? InputFileList = null;

		/// <summary>
		/// Output file to generate
		/// </summary>
		[CommandLine("-Output", Required = true)]
		FileReference? OutputFile = null;

		/// <summary>
		/// Execute the command
		/// </summary>
		/// <param name="Arguments">List of command line arguments</param>
		/// <returns>Always zero, or throws an exception</returns>
		public override int Execute(CommandLineArguments Arguments)
		{
			Arguments.ApplyTo(this);
			Arguments.CheckAllArgumentsUsed();

			Log.TraceInformation("{0}", OutputFile!.GetFileName());

			// Read the input files
			string[] InputFileLines = FileReference.ReadAllLines(InputFileList!);
			FileReference[] InputFiles = InputFileLines.Select(x => x.Trim()).Where(x => x.Length > 0).Select(x => new FileReference(x)).ToArray();

			// Create the combined output file, and print the diagnostics to the log
			HashSet<string> UniqueItems = new HashSet<string>();
			List<string> OutputLines = new List<string>();

			using (StreamWriter RawWriter = new StreamWriter(OutputFile.FullName))
			{
				foreach (FileReference InputFile in InputFiles)
				{
					string[] Lines = File.ReadAllLines(InputFile.FullName);
					for(int LineIdx = 0; LineIdx < Lines.Length; LineIdx++)
					{
						string Line = Lines[LineIdx];
						if (!String.IsNullOrWhiteSpace(Line) && UniqueItems.Add(Line))
						{
							bool bCanParse = false;

							string[] Tokens = Line.Split(new string[] { "<#~>" }, StringSplitOptions.None);
							if(Tokens.Length >= 9)
							{
								//string Trial = Tokens[1];
								string LineNumberStr = Tokens[2];
								string FileName = Tokens[3];
								string WarningCode = Tokens[5];
								string WarningMessage = Tokens[6];
								string FalseAlarmStr = Tokens[7];
								string LevelStr = Tokens[8];

								int LineNumber;
								bool bFalseAlarm;
								int Level;
								if(int.TryParse(LineNumberStr, out LineNumber) && bool.TryParse(FalseAlarmStr, out bFalseAlarm) && int.TryParse(LevelStr, out Level))
								{
									bCanParse = true;

									// Ignore anything in ThirdParty folders
									if(FileName.Replace('/', '\\').IndexOf("\\ThirdParty\\", StringComparison.InvariantCultureIgnoreCase) == -1)
									{
										// Output the line to the raw output file
										RawWriter.WriteLine(Line);

										// Output the line to the log
										if (!bFalseAlarm && Level == 1)
										{
											Log.WriteLine(LogEventType.Warning, LogFormatOptions.NoSeverityPrefix, "{0}({1}): warning {2}: {3}", FileName, LineNumber, WarningCode, WarningMessage);
										}
									}
								}
							}

							if(!bCanParse)
							{
								Log.WriteLine(LogEventType.Warning, LogFormatOptions.NoSeverityPrefix, "{0}({1}): warning: Unable to parse PVS output line '{2}' (tokens=|{3}|)", InputFile, LineIdx + 1, Line, String.Join("|", Tokens));
							}
						}
					}
				}
			}
			Log.TraceInformation("Written {0} {1} to {2}.", UniqueItems.Count, (UniqueItems.Count == 1)? "diagnostic" : "diagnostics", OutputFile.FullName);
			return 0;
		}
	}

	class PVSToolChain : UEToolChain
	{
		ReadOnlyTargetRules Target;
		ReadOnlyPVSTargetSettings Settings;
		PVSApplicationSettings? ApplicationSettings;
		VCToolChain InnerToolChain;
		FileReference AnalyzerFile;
		FileReference? LicenseFile;
		UnrealTargetPlatform Platform;
		Version AnalyzerVersion;

		public PVSToolChain(ReadOnlyTargetRules Target)
		{
			this.Target = Target;
			Platform = Target.Platform;
			InnerToolChain = new VCToolChain(Target);

			AnalyzerFile = FileReference.Combine(Unreal.RootDirectory, "Engine", "Restricted", "NoRedist", "Extras", "ThirdPartyNotUE", "PVS-Studio", "PVS-Studio.exe");
			if (!FileReference.Exists(AnalyzerFile))
			{
				FileReference InstalledAnalyzerFile = FileReference.Combine(new DirectoryReference(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86)), "PVS-Studio", "x64", "PVS-Studio.exe");
				if (FileReference.Exists(InstalledAnalyzerFile))
				{
					AnalyzerFile = InstalledAnalyzerFile;
				}
				else
				{
					throw new BuildException("Unable to find PVS-Studio at {0} or {1}", AnalyzerFile, InstalledAnalyzerFile);
				}
			}

			AnalyzerVersion = GetAnalyzerVersion(AnalyzerFile);
			Settings = Target.WindowsPlatform.PVS;
			ApplicationSettings = Settings.ApplicationSettings;

			if(ApplicationSettings != null)
			{
				if (Settings.ModeFlags == 0)
				{
					throw new BuildException("All PVS-Studio analysis modes are disabled.");
				}

				if (!String.IsNullOrEmpty(ApplicationSettings.UserName) && !String.IsNullOrEmpty(ApplicationSettings.SerialNumber))
				{
					LicenseFile = FileReference.Combine(Unreal.EngineDirectory, "Intermediate", "PVS", "PVS-Studio.lic");
					Utils.WriteFileIfChanged(LicenseFile, String.Format("{0}\n{1}\n", ApplicationSettings.UserName, ApplicationSettings.SerialNumber));
				}
			}
			else
			{
				FileReference DefaultLicenseFile = AnalyzerFile.ChangeExtension(".lic");
				if(FileReference.Exists(DefaultLicenseFile))
				{
					LicenseFile = DefaultLicenseFile;
				}
			}
		}

		public override void GetVersionInfo(List<string> Lines)
		{
			base.GetVersionInfo(Lines);

			ReadOnlyPVSTargetSettings Settings = Target.WindowsPlatform.PVS;
			Lines.Add(String.Format("Using PVS-Studio installation at {0} with analysis mode {1} ({2})", AnalyzerFile, (uint)Settings.ModeFlags, Settings.ModeFlags.ToString()));
		}

		static Version GetAnalyzerVersion(FileReference AnalyzerPath)
		{
			String Output = String.Empty;
			Version? AnalyzerVersion = new Version(0, 0);

			try
			{
				using (Process PvsProc = new Process())
				{
					PvsProc.StartInfo.FileName = AnalyzerPath.FullName;
					PvsProc.StartInfo.Arguments = "--version";
					PvsProc.StartInfo.UseShellExecute = false;
					PvsProc.StartInfo.CreateNoWindow = true;
					PvsProc.StartInfo.RedirectStandardOutput = true;

					PvsProc.Start();
					Output = PvsProc.StandardOutput.ReadToEnd();
					PvsProc.WaitForExit();
				}

				const String VersionPattern = @"\d+(?:\.\d+)+";
				Match Match = Regex.Match(Output, VersionPattern);

				if (Match.Success)
				{
					string VersionStr = Match.Value;
					if (!Version.TryParse(VersionStr, out AnalyzerVersion))
					{
						throw new BuildException(String.Format("Failed to parse PVS-Studio version: {0}", VersionStr));
					}
				}
			}
			catch (Exception Ex)
			{
				if (Ex is BuildException)
					throw;

				throw new BuildException(Ex, "Failed to obtain PVS-Studio version.");
			}

			return AnalyzerVersion;
		}

		class ActionGraphCapture : ForwardingActionGraphBuilder
		{
			List<IExternalAction> Actions;

			public ActionGraphCapture(IActionGraphBuilder Inner, List<IExternalAction> Actions)
				: base(Inner)
			{
				this.Actions = Actions;
			}

			public override void AddAction(IExternalAction Action)
			{
				base.AddAction(Action);

				Actions.Add(Action);
			}
		}

		public static readonly VersionNumber CLVerWithCPP20Support = new VersionNumber(14, 23);

		public static string GetLangStandForCfgFile(CppStandardVersion cppStandard, VersionNumber compilerVersion)
		{
			string cppCfgStandard;

			switch (cppStandard)
			{
				case CppStandardVersion.Cpp17:
					cppCfgStandard = "c++17";
					break;
				case CppStandardVersion.Latest:
					cppCfgStandard = VersionNumber.Compare(compilerVersion, CLVerWithCPP20Support) >= 0 ? "c++20" : "c++17";
					break;
				default:
					cppCfgStandard = "c++14";
					break;
			}

			return cppCfgStandard;
		}

		public override CPPOutput CompileCPPFiles(CppCompileEnvironment CompileEnvironment, List<FileItem> InputFiles, DirectoryReference OutputDir, string ModuleName, IActionGraphBuilder Graph)
		{
			// Ignore generated files
			if (InputFiles.All(x => x.Location.GetFileName().EndsWith(".gen.cpp")))
			{
				return new CPPOutput();
			}

			// Use a subdirectory for PVS output, to avoid clobbering regular build artifacts
			OutputDir = DirectoryReference.Combine(OutputDir, "PVS");

			// Preprocess the source files with the regular toolchain
			CppCompileEnvironment PreprocessCompileEnvironment = new CppCompileEnvironment(CompileEnvironment);
			PreprocessCompileEnvironment.bPreprocessOnly = true;
			PreprocessCompileEnvironment.bEnableUndefinedIdentifierWarnings = false; // Not sure why THIRD_PARTY_INCLUDES_START doesn't pick this up; the _Pragma appears in the preprocessed output. Perhaps in preprocess-only mode the compiler doesn't respect these?
			PreprocessCompileEnvironment.AdditionalArguments += " /wd4005 /wd4828";
			PreprocessCompileEnvironment.Definitions.Add("PVS_STUDIO");

			List<IExternalAction> PreprocessActions = new List<IExternalAction>();
			CPPOutput Result = InnerToolChain.CompileCPPFiles(PreprocessCompileEnvironment, InputFiles, OutputDir, ModuleName, new ActionGraphCapture(Graph, PreprocessActions));

			// Run the source files through PVS-Studio
			for(int Idx = 0; Idx < PreprocessActions.Count; Idx++)
			{
				VCCompileAction? PreprocessAction = PreprocessActions[Idx] as VCCompileAction;
				if (PreprocessAction == null)
				{
					continue;
				}

				FileItem? SourceFileItem = PreprocessAction.SourceFile;
				if (SourceFileItem == null)
				{
					Log.TraceWarning("Unable to find source file from command producing: {0}", String.Join(", ", PreprocessActions[Idx].ProducedItems.Select(x => x.Location.GetFileName())));
					continue;
				}

				FileItem? PreprocessedFileItem = PreprocessAction.PreprocessedFile;
				if (PreprocessedFileItem == null)
				{
					Log.TraceWarning("Unable to find preprocessed output file from {0}", SourceFileItem.Location.GetFileName());
					continue;
				}

				// Write the PVS studio config file
				StringBuilder ConfigFileContents = new StringBuilder();
				foreach(DirectoryReference IncludePath in Target.WindowsPlatform.Environment!.IncludePaths)
				{
					ConfigFileContents.AppendFormat("exclude-path={0}\n", IncludePath.FullName);
				}
				if(ApplicationSettings != null && ApplicationSettings.PathMasks != null)
				{
					foreach(string PathMask in ApplicationSettings.PathMasks)
					{
						if (PathMask.Contains(":") || PathMask.Contains("\\") || PathMask.Contains("/"))
						{
							if(Path.IsPathRooted(PathMask) && !PathMask.Contains(":"))
							{
								ConfigFileContents.AppendFormat("exclude-path=*{0}*\n", PathMask);
							}
							else
							{
								ConfigFileContents.AppendFormat("exclude-path={0}\n", PathMask);
							}
						}
					}
				}
				if (Platform == UnrealTargetPlatform.Win64)
				{
					ConfigFileContents.Append("platform=x64\n");
				}
				else
				{
					throw new BuildException("PVS-Studio does not support this platform");
				}
				ConfigFileContents.Append("preprocessor=visualcpp\n");
				ConfigFileContents.Append("language=C++\n");
				ConfigFileContents.Append("skip-cl-exe=yes\n");

				if(AnalyzerVersion.CompareTo(new Version("7.07")) >= 0)
				{
					VersionNumber compilerVersion = Target.WindowsPlatform.Environment.CompilerVersion;
					string languageStandardForCfg = GetLangStandForCfgFile(PreprocessCompileEnvironment.CppStandard, compilerVersion);

					ConfigFileContents.AppendFormat("std={0}\n", languageStandardForCfg);
				}

				string BaseFileName = PreprocessedFileItem.Location.GetFileName();

				FileReference ConfigFileLocation = FileReference.Combine(OutputDir, BaseFileName + ".cfg");
				FileItem ConfigFileItem = Graph.CreateIntermediateTextFile(ConfigFileLocation, ConfigFileContents.ToString());

				// Run the analzyer on the preprocessed source file
				FileReference OutputFileLocation = FileReference.Combine(OutputDir, BaseFileName + ".pvslog");
				FileItem OutputFileItem = FileItem.GetItemByFileReference(OutputFileLocation);

				Action AnalyzeAction = Graph.CreateAction(ActionType.Compile);
				AnalyzeAction.CommandDescription = "Analyzing";
				AnalyzeAction.StatusDescription = BaseFileName;
				AnalyzeAction.WorkingDirectory = UnrealBuildTool.EngineSourceDirectory;
				AnalyzeAction.CommandPath = BuildHostPlatform.Current.Shell;

				StringBuilder Arguments = new StringBuilder();
				Arguments.Append($"/C \"\"{AnalyzerFile}\" --source-file \"{SourceFileItem.AbsolutePath}\" --output-file \"{OutputFileLocation}\" --cfg \"{ConfigFileItem.AbsolutePath}\" --i-file=\"{PreprocessedFileItem.AbsolutePath}\" --analysis-mode {(uint)Settings.ModeFlags}");
				if (LicenseFile != null)
				{
					Arguments.Append($" --lic-file \"{LicenseFile}\"");
					AnalyzeAction.PrerequisiteItems.Add(FileItem.GetItemByFileReference(LicenseFile));
				}
				Arguments.Append($" && echo. >>\"{OutputFileLocation}\"\"");
				AnalyzeAction.CommandArguments = Arguments.ToString();

				AnalyzeAction.PrerequisiteItems.Add(ConfigFileItem);
				AnalyzeAction.PrerequisiteItems.Add(PreprocessedFileItem);
				AnalyzeAction.PrerequisiteItems.AddRange(InputFiles); // Add the InputFiles as PrerequisiteItems so that in SingleFileCompile mode the PVSAnalyze step is not filtered out
				AnalyzeAction.ProducedItems.Add(OutputFileItem);
				AnalyzeAction.DeleteItems.Add(OutputFileItem); // PVS Studio will append by default, so need to delete produced items
				AnalyzeAction.bCanExecuteRemotely = false;

				Result.ObjectFiles.AddRange(AnalyzeAction.ProducedItems);
			}
			return Result;
		}

		public override FileItem LinkFiles(LinkEnvironment LinkEnvironment, bool bBuildImportLibraryOnly, IActionGraphBuilder Graph)
		{
			throw new BuildException("Unable to link with PVS toolchain.");
		}

		public override void FinalizeOutput(ReadOnlyTargetRules Target, TargetMakefile Makefile)
		{
			FileReference OutputFile;
			if (Target.ProjectFile == null)
			{
				OutputFile = FileReference.Combine(Unreal.EngineDirectory, "Saved", "PVS-Studio", String.Format("{0}.pvslog", Target.Name));
			}
			else
			{
				OutputFile = FileReference.Combine(Target.ProjectFile.Directory, "Saved", "PVS-Studio", String.Format("{0}.pvslog", Target.Name));
			}

			List<FileReference> InputFiles = Makefile.OutputItems.Select(x => x.Location).Where(x => x.HasExtension(".pvslog")).ToList();

			// Collect the sourcefile items off of the Compile action added in CompileCPPFiles so that in SingleFileCompile mode the PVSGather step is also not filtered out
			List<FileItem> CompileSourceFiles = Makefile.Actions.OfType<VCCompileAction>().Select(x => x.SourceFile!).ToList();

			FileItem InputFileListItem = Makefile.CreateIntermediateTextFile(OutputFile.ChangeExtension(".input"), InputFiles.Select(x => x.FullName));

			Action AnalyzeAction = Makefile.CreateAction(ActionType.Compile);
			AnalyzeAction.CommandPath = UnrealBuildTool.GetUBTPath();
			AnalyzeAction.CommandArguments = String.Format("-Mode=PVSGather -Input=\"{0}\" -Output=\"{1}\" ", InputFileListItem.Location, OutputFile);
			AnalyzeAction.WorkingDirectory = UnrealBuildTool.EngineSourceDirectory;
			AnalyzeAction.PrerequisiteItems.Add(InputFileListItem);
			AnalyzeAction.PrerequisiteItems.AddRange(Makefile.OutputItems);
			AnalyzeAction.PrerequisiteItems.AddRange(CompileSourceFiles);
			AnalyzeAction.ProducedItems.Add(FileItem.GetItemByFileReference(OutputFile));
			AnalyzeAction.ProducedItems.Add(FileItem.GetItemByPath(OutputFile.FullName + "_does_not_exist")); // Force the gather step to always execute
			AnalyzeAction.DeleteItems.AddRange(AnalyzeAction.ProducedItems);

			Makefile.OutputItems.AddRange(AnalyzeAction.ProducedItems);
		}
	}
}
