using UnrealBuildTool;

public class DeviceExplorerEditor : ModuleRules
{
    public DeviceExplorerEditor(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;

        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "DesktopPlatform",
            "DeveloperSettings",
            "DeviceExplorer",
            "DeviceExplorerCore",
            "DeviceExplorerWire",
            "Engine",
            "Projects",
            "Settings",
            "Slate",
            "SlateCore",
            "ToolMenus",
            "UnrealEd"
        });
    }
}
