using UnrealBuildTool;

public class DeviceExplorerHostTarget : TargetRules
{
    public DeviceExplorerHostTarget(TargetInfo Target) : base(Target)
    {
        Type = TargetType.Program;
        LinkType = TargetLinkType.Monolithic;
        CppStandard = CppStandardVersion.Cpp20;
        LaunchModuleName = "DeviceExplorerHost";
        bBuildDeveloperTools = false;
        bCompileAgainstEngine = false;
        bCompileAgainstCoreUObject = false;
        bCompileAgainstApplicationCore = true;
        bUsesSlate = false;
        bHasExports = false;
        bIsBuildingConsoleApplication = true;
    }
}
