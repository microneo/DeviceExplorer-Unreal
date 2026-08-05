using UnrealBuildTool;

public class DeviceExplorerWire : ModuleRules
{
    public DeviceExplorerWire(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.NoPCHs;
        bRequiresImplementModule = false;
        bWarningsAsErrors = true;
        bEnableExceptions = false;
        bUseRTTI = false;
        CppStandard = CppStandardVersion.Cpp20;

        // Only Unreal/ uses Core, to route this module's allocation through FMemory.
        PrivateDependencyModuleNames.Add("Core");
    }
}
