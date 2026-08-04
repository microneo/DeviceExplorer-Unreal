using UnrealBuildTool;

public class DeviceExplorer : ModuleRules
{
    public DeviceExplorer(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "DeviceExplorerCore"
        });

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "CoreUObject",
            "DeveloperSettings",
            "Engine",
            "EngineSettings",
            "HTTP",
            "Json",
            "Networking",
            "Projects",
            "Sockets",
            "WebSockets"
        });

        if (Target.Platform == UnrealTargetPlatform.IOS)
        {
            PublicFrameworks.AddRange(new[]
            {
                "CFNetwork",
                "Foundation"
            });
        }
    }
}
