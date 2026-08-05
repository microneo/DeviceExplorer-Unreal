#include "DeviceExplorerHostBuildId.h"

#include "Interfaces/IPluginManager.h"
#include "Misc/App.h"
#include "Misc/EngineVersion.h"

FString GetDeviceExplorerLegacyHostBuildId()
{
	FString PluginVersion = TEXT("unknown");
	if (const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DeviceExplorer")))
	{
		PluginVersion = Plugin->GetDescriptor().VersionName;
	}
	return FString::Printf(TEXT("UE-%s-Plugin-%s-Compiled-%s-%s"),
	                       *FEngineVersion::Current().ToString(),
	                       *PluginVersion,
	                       ANSI_TO_TCHAR(__DATE__),
	                       ANSI_TO_TCHAR(__TIME__));
}
