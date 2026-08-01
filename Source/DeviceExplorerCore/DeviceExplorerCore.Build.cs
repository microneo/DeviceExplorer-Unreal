using UnrealBuildTool;

public class DeviceExplorerCore : ModuleRules
{
    public DeviceExplorerCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "Json"
        });

        if (Target.bCompileAgainstCoreUObject)
        {
            PublicDependencyModuleNames.Add("CoreUObject");
        }
    }
}
