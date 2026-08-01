#include "DeviceExplorerHostApplication.h"

#include "DeviceExplorerHostServer.h"
#include "Misc/CommandLine.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerHostApplication, Log, All);

bool FDeviceExplorerHostApplication::Initialize()
{
	FDeviceExplorerHostConfig Config;
	FParse::Value(FCommandLine::Get(), TEXT("DashboardPort="), Config.DashboardPort);
	FParse::Value(FCommandLine::Get(), TEXT("DevicePort="), Config.DevicePort);
	FParse::Value(FCommandLine::Get(), TEXT("TracePort="), Config.TracePort);
	FParse::Value(FCommandLine::Get(), TEXT("ParentPID="), ParentProcessId);
	FParse::Value(FCommandLine::Get(), TEXT("WebRoot="), Config.WebRoot);
	FParse::Value(FCommandLine::Get(), TEXT("Token="), Config.Token);
	FParse::Value(FCommandLine::Get(), TEXT("TransferDir="), Config.TransferDirectory);

	// -Project can't be passed (aborts project loading); fall back to Plugins/DeviceExplorer/Resources/Web relative to the exe.
	if (Config.WebRoot.IsEmpty())
	{
		Config.WebRoot = FPaths::Combine(FPaths::LaunchDir(), TEXT(".."), TEXT(".."), TEXT("Plugins"), TEXT("DeviceExplorer"), TEXT("Resources"), TEXT("Web"));
	}
	Config.WebRoot = FPaths::ConvertRelativePathToFull(Config.WebRoot);

	if (Config.TransferDirectory.IsEmpty())
	{
		Config.TransferDirectory = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DeviceExplorer"), TEXT("Transfers"));
	}
	Config.TransferDirectory = FPaths::ConvertRelativePathToFull(Config.TransferDirectory);

	if (Config.Token.IsEmpty())
	{
		Config.Token = FGuid::NewGuid().ToString(EGuidFormats::Digits) + FGuid::NewGuid().ToString(EGuidFormats::Digits);
	}

	const FString Token = Config.Token;
	const FString TransferDirectory = Config.TransferDirectory;
	Server = MakeUnique<FDeviceExplorerHostServer>(MoveTemp(Config));
	if (!Server->Start())
	{
		return false;
	}

	UE_LOG(LogDeviceExplorerHostApplication, Display, TEXT("Session token: %s"), *Token);
	UE_LOG(LogDeviceExplorerHostApplication, Display, TEXT("Transfers: %s"), *TransferDirectory);
	return true;
}

bool FDeviceExplorerHostApplication::Tick()
{
	if (!Server || !Server->Tick())
	{
		return false;
	}
	return ParentProcessId == 0 || FPlatformProcess::IsApplicationRunning(ParentProcessId);
}

void FDeviceExplorerHostApplication::Shutdown()
{
	if (Server)
	{
		Server->Stop();
		Server.Reset();
	}
}
