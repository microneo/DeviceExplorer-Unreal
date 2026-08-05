#include "DeviceExplorerTransport.h"

#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerTransport, Log, All);

namespace
{
const TCHAR* DeviceExplorerSettingsSection = TEXT("/Script/DeviceExplorer.Settings");

FString GetRequestedDeviceExplorerTransport()
{
	FString Requested = TEXT("Auto");
	if (GConfig != nullptr)
	{
		GConfig->GetString(DeviceExplorerSettingsSection, TEXT("Transport"), Requested, GGameUserSettingsIni);
	}
	FParse::Value(FCommandLine::Get(), TEXT("DeviceExplorerTransport="), Requested);
	Requested.TrimStartAndEndInline();
	return Requested.IsEmpty() ? TEXT("Auto") : Requested;
}
}    // namespace

TUniquePtr<IDeviceExplorerTransport> CreateDeviceExplorerTransport()
{
	FString Requested = GetRequestedDeviceExplorerTransport();
	if (!Requested.Equals(TEXT("Auto"), ESearchCase::IgnoreCase) &&
	    !Requested.Equals(TEXT("Engine"), ESearchCase::IgnoreCase) &&
	    !Requested.Equals(TEXT("Builtin"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogDeviceExplorerTransport, Warning, TEXT("Unknown transport '%s'; using Auto"), *Requested);
		Requested = TEXT("Auto");
	}

	if (Requested.Equals(TEXT("Builtin"), ESearchCase::IgnoreCase))
	{
		return CreateDeviceExplorerBuiltinTransport();
	}

#if WITH_WEBSOCKETS
	return CreateDeviceExplorerEngineTransport();
#else
	if (Requested.Equals(TEXT("Engine"), ESearchCase::IgnoreCase))
	{
		UE_LOG(LogDeviceExplorerTransport, Warning,
		       TEXT("Engine WebSockets are unavailable for this target; using the Builtin transport"));
	}
	return CreateDeviceExplorerBuiltinTransport();
#endif
}
