// Copyright (c) Improbable Worlds Ltd, All Rights Reserved

using System;
using System.ComponentModel;
using System.Collections.Generic;
using System.Text;
using System.IO;
using System.Diagnostics;
using UnrealBuildTool;

public class SpatialGDK : ModuleRules
{
    public SpatialGDK(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        bFasterWithoutUnity = true;

        PrivateIncludePaths.Add("SpatialGDK/Private");

        PublicDependencyModuleNames.AddRange(
            new string[]
            {
                "Core",
                "CoreUObject",
                "Engine",
                "EngineSettings",
                "Projects",
                "OnlineSubsystemUtils",
                "InputCore",
                "Sockets",
            });

		// Check if we're building in the editor.
		if (Target.bBuildEditor)
		{
			// Required by USpatialGameInstance::StartPlayInEditorGameInstance.
			PublicDependencyModuleNames.Add("UnrealEd");
		}

   		var CoreSdkLibraryDir = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", "..", "Binaries", "ThirdParty", "Improbable", Target.Platform.ToString()));

        string LibPrefix = "";
        string ImportLibSuffix = "";
        string SharedLibSuffix = "";
        bool bAddDelayLoad = false;

        switch (Target.Platform)
        {
            case UnrealTargetPlatform.Win32:
            case UnrealTargetPlatform.Win64:
                ImportLibSuffix = ".lib";
                SharedLibSuffix = ".dll";
                bAddDelayLoad = true;
                break;
            case UnrealTargetPlatform.Mac:
                LibPrefix = "lib";
                ImportLibSuffix = SharedLibSuffix = ".dylib";
                break;
            case UnrealTargetPlatform.Linux:
                LibPrefix = "lib";
                ImportLibSuffix = SharedLibSuffix = ".so";
                break;
            case UnrealTargetPlatform.PS4:
                LibPrefix = "lib";
                ImportLibSuffix = "_stub.a";
                SharedLibSuffix = ".prx";
                bAddDelayLoad = true;
                break;
            case UnrealTargetPlatform.XboxOne:
                ImportLibSuffix = ".lib";
                SharedLibSuffix = ".dll";
                break;
            default:
                throw new System.Exception(System.String.Format("Unsupported platform {0}", Target.Platform.ToString()));
        }

        string CoreSdkImportLib = System.String.Format("{0}worker{1}", LibPrefix, ImportLibSuffix);
        string CoreSdkSharedLib = System.String.Format("{0}worker{1}", LibPrefix, SharedLibSuffix);

        PublicAdditionalLibraries.AddRange(new[] { Path.Combine(CoreSdkLibraryDir, CoreSdkImportLib) });
        RuntimeDependencies.Add(Path.Combine(CoreSdkLibraryDir, CoreSdkSharedLib), StagedFileType.NonUFS);
        PublicLibraryPaths.Add(CoreSdkLibraryDir);
        if (bAddDelayLoad)
        {
            PublicDelayLoadDLLs.Add(CoreSdkSharedLib);
        }

        // Point generated code to the correct API spec.
        PublicDefinitions.Add("IMPROBABLE_DLL_API=SPATIALGDK_API");
	}
}
