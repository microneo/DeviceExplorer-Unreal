#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "DeviceExplorerHostApplication.h"
#include "DeviceExplorerHostManifest.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "RequiredProgramMainCPPInclude.h"

#include <cstdio>
#include <string>

IMPLEMENT_APPLICATION(DeviceExplorerHost, "DeviceExplorerHost");

INT32_MAIN_INT32_ARGC_TCHAR_ARGV()
{
	// Rider passes a .uproject arg by default; LaunchSetGameName() reacts to that even for Program targets and crashes PreInit.
	TArray<TCHAR*> FilteredArgV;
	FilteredArgV.Reserve(ArgC);
	for (int32 Index = 0; Index < ArgC; ++Index)
	{
		if (!FString(ArgV[Index]).EndsWith(TEXT(".uproject"), ESearchCase::IgnoreCase))
		{
			FilteredArgV.Add(ArgV[Index]);
		}
	}
	GEngineLoop.PreInit(FilteredArgV.Num(), FilteredArgV.GetData());
	if (FParse::Param(FCommandLine::Get(), TEXT("VersionJson")))
	{
		DeviceExplorer::Wire::HostManifest Manifest;
		const FString BuildVersion = FApp::GetBuildVersion();
		if (!BuildVersion.IsEmpty())
		{
			const FTCHARToUTF8 BuildId(*BuildVersion);
			Manifest.BuildId.assign(BuildId.Get(), static_cast<std::size_t>(BuildId.Length()));
		}
		std::string Json;
		const bool bSerialized = DeviceExplorer::Wire::SerializeHostManifest(Manifest, Json);
		if (bSerialized)
		{
			std::fwrite(Json.data(), 1, Json.size(), stdout);
			std::fputc('\n', stdout);
			std::fflush(stdout);
		}
		FEngineLoop::AppExit();
		return bSerialized ? 0 : 1;
	}
	// Program targets do not install the Launch graceful-termination handler for
	// us. Without it, Ctrl+C on Windows terminates the process before mDNS Stop()
	// can publish the TTL=0 goodbye packet.
	FPlatformMisc::SetGracefulTerminationHandler();

	FDeviceExplorerHostApplication Application;
	if (!Application.Initialize())
	{
		FEngineLoop::AppExit();
		return 1;
	}

	while (!IsEngineExitRequested() && Application.Tick())
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		FTSTicker::GetCoreTicker().Tick(0.01f);
		FPlatformProcess::Sleep(0.01f);
	}

	Application.Shutdown();
	FEngineLoop::AppExit();
	return 0;
}
