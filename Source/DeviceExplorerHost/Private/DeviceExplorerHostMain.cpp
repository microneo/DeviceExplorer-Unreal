#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "DeviceExplorerHostApplication.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CoreDelegates.h"
#include "RequiredProgramMainCPPInclude.h"

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
