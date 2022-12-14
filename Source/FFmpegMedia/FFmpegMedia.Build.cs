// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;
using System.IO;
public class FFmpegMedia : ModuleRules
{
	public FFmpegMedia(ReadOnlyTargetRules Target) : base(Target)
	{
		bEnableExceptions = true;
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;
        OptimizeCode = CodeOptimization.InShippingBuildsOnly;

        PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);

		PrivateIncludePathModuleNames.AddRange(
			new string[] {
					"Media",
			});

		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
				"FFmpegMedia/Private",
				"FFmpegMedia/Private/Player",
				"FFmpegMedia/Private/FFmpeg",
			}
			);


        PublicDependencyModuleNames.AddRange(
            new string[]
            {
				"FFmpeg",
				// ... add other public dependencies that you statically link with here ...
			}
            );


        PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"MediaUtils",
				"RenderCore",
				"Projects",
				"FFmpegMediaFactory"
				// ... add private dependencies that you statically link with here ...	
			}
			);
		
		
		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				"Media",
				// ... add any modules that your module loads dynamically here ...
			}
			);

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            // Ensure that the DLL is staged along with the executable
            string[] dlls = { "avcodec-59.dll", "avdevice-59.dll", "avfilter-8.dll", "avformat-59.dll", "avutil-57.dll", "swresample-4.dll", "swscale-6.dll", "postproc-56.dll" };
            foreach (string dll in dlls)
            {
                string DllPath = Path.Combine(PluginDirectory, "Source", "ThirdParty", "FFmpeg", "bin", "Win64", dll);

                System.Console.WriteLine("... Dll Path -> " + DllPath);

                PublicDelayLoadDLLs.Add(dll);
                RuntimeDependencies.Add(DllPath);
            }
        }
    }
}
