using System.IO;
using UnrealBuildTool;

public class DeviceExplorerHost : ModuleRules
{
    public DeviceExplorerHost(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "ApplicationCore",
            "Core",
            "DeviceExplorerCore",
            "DeviceExplorerWire",
            "HTTP",
            "Json",
            "Networking",
            "Projects",
            "Sockets"
        });

        PrivateIncludePathModuleNames.Add("Launch");

        // The host serves the already-built dashboard from disk. Keep these
        // files as NonUFS so HTML/JS/CSS stay directly readable at runtime.
        string WebRoot = Path.Combine(PluginDirectory, "Resources", "Web");
        if (Directory.Exists(WebRoot))
        {
            foreach (string WebFile in Directory.GetFiles(WebRoot, "*", SearchOption.AllDirectories))
            {
                RuntimeDependencies.Add(WebFile, StagedFileType.NonUFS);
            }
        }
    }
}
