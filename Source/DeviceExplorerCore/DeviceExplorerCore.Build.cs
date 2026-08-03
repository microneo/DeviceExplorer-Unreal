using System.Linq;
using EpicGames.Core;
using UnrealBuildBase;
using UnrealBuildTool;

public class DeviceExplorerCore : ModuleRules
{
    public DeviceExplorerCore(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        bWarningsAsErrors = true;

        PublicDefinitions.Add(DeviceExplorerPlugin.IsEnabled(Target) ? "WITH_DEVICEEXPLORER=1" : "WITH_DEVICEEXPLORER=0");

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

/// <summary>
/// Build-time entry point for modules that register DeviceExplorer content.
/// Depending on a disabled plugin is a hard UnrealBuildTool error, so the
/// reference and the WITH_DEVICEEXPLORER guard have to be resolved here.
/// </summary>
public static class DeviceExplorerPlugin
{
    private const string PluginName = "DeviceExplorer";
    private const string ClientModuleName = "DeviceExplorer";

    /// <summary>
    /// Adds the DeviceExplorerCore dependency when the plugin is enabled for
    /// this target, otherwise defines WITH_DEVICEEXPLORER=0 so that guarded
    /// code compiles out instead of failing on a missing module.
    /// </summary>
    public static void AddDependency(ModuleRules Module, bool bPublicDependency = false)
    {
        if (!IsEnabled(Module.Target))
        {
            Module.PublicDefinitions.Add("WITH_DEVICEEXPLORER=0");
            return;
        }

        if (bPublicDependency)
        {
            Module.PublicDependencyModuleNames.Add("DeviceExplorerCore");
        }
        else
        {
            Module.PrivateDependencyModuleNames.Add("DeviceExplorerCore");
        }
    }

    public static bool IsEnabled(ReadOnlyTargetRules Target)
    {
        PluginInfo Plugin = Plugins
            .ReadAvailablePlugins(Unreal.EngineDirectory, DirectoryReference.FromFile(Target.ProjectFile), null)
            .FirstOrDefault(Candidate => Candidate.Name == PluginName);

        if (Plugin == null || Plugin.Descriptor.Modules == null)
        {
            return false;
        }

        ProjectDescriptor Project = Target.ProjectFile != null ? ProjectDescriptor.FromFile(Target.ProjectFile) : null;

        if (!Plugins.IsPluginEnabledForTarget(Plugin, Project, Target.Platform, Target.Configuration, Target.Type))
        {
            return false;
        }

        // Registrations are only ever read through the client module, so its own
        // deny lists gate the guard as well.
        ModuleDescriptor Client = Plugin.Descriptor.Modules.FirstOrDefault(Candidate => Candidate.Name == ClientModuleName);

        return Client != null && Client.IsCompiledInConfiguration(Target.Platform, Target.Configuration, Target.Name, Target.Type, Target.bBuildDeveloperTools, Target.bBuildRequiresCookedData, Target.Architectures);
    }
}
