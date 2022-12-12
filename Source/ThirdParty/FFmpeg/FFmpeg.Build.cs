// Fill out your copyright notice in the Description page of Project Settings.

using System.IO;
using UnrealBuildTool;

public class FFmpeg : ModuleRules
{
	public FFmpeg(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;
		bool bIsLibrarySupported = false;
		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
            bIsLibrarySupported = true;
			// Add the import library
			string[] libs = { "avcodec.lib", "avdevice.lib", "avfilter.lib", "avformat.lib", "avutil.lib", "postproc.lib", "swresample.lib", "swscale.lib" };
			foreach (string lib in libs)
			{
				string LibPath = Path.Combine(ModuleDirectory, "lib", "Win64", lib);

                PublicAdditionalLibraries.Add(LibPath);
			}

			// Ensure that the DLL is staged along with the executable
			string[] dlls = { "avcodec-59.dll", "avdevice-59.dll", "avfilter-8.dll", "avformat-59.dll", "avutil-57.dll", "swresample-4.dll", "swscale-6.dll", "postproc-56.dll" };
			foreach (string dll in dlls)
            {
                string DllPath = Path.Combine(ModuleDirectory, "lib", "Win64", dll);

                PublicDelayLoadDLLs.Add(dll);
				RuntimeDependencies.Add(DllPath);
			}

		}
        else if (Target.Platform == UnrealTargetPlatform.Mac)
        {
			bIsLibrarySupported = true;

			string[] dlls = { "libavcodec.59.dylib", "libavdevice.59.dylib", "libavfilter.8.dylib", "libavformat.59.dylib", "libavutil.57.dylib", "libpostproc.56.dylib", "libswresample.4.dylib", "libswscale.6.dylib" };
			foreach (string dll in dlls)
			{
                string DllPath = Path.Combine(ModuleDirectory, "lib", "Mac", dll);

				PublicDelayLoadDLLs.Add(DllPath);
				RuntimeDependencies.Add(DllPath);
			}
		}
        else if (Target.Platform == UnrealTargetPlatform.Linux)
		{
            bIsLibrarySupported = true;
			string[] dlls = { "libavcodec.so.59.37.100", "libavdevice.so.59.7.100", "libavfilter.so.8.44.100", "libavformat.so.59.27.100", "libavutil.so.57.28.100", "libpostproc.so.56.6.100", "libswresample.so.4.7.100", "libswscale.so.6.7.100" };
			foreach (string dll in dlls)
			{
				string DllPath = Path.Combine(ModuleDirectory, "lib", "Linux", dll);

				PublicAdditionalLibraries.Add(DllPath);
				PublicDelayLoadDLLs.Add(DllPath);
				RuntimeDependencies.Add(DllPath);
			}
		}
		if (bIsLibrarySupported)
		{
			// Include path
			PublicIncludePaths.AddRange(
				new string[] {
					// ... add public include paths required here ...
					Path.Combine(ModuleDirectory, "include")
				}
			);
		}
	}
}
