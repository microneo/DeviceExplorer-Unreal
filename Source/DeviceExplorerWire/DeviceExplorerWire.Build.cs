using UnrealBuildTool;

public class DeviceExplorerWire : ModuleRules
{
    public DeviceExplorerWire(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.NoSharedPCHs;
        bRequiresImplementModule = false;
        bWarningsAsErrors = true;
        bEnableExceptions = false;
        bUseRTTI = false;
        CppStandard = CppStandardVersion.Cpp17;
    }
}
