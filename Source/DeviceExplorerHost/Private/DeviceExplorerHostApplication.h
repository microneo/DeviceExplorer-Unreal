#pragma once

#include "CoreMinimal.h"

class FDeviceExplorerHostServer;

class FDeviceExplorerHostApplication
{
public:
	bool Initialize();
	bool Tick();
	void Shutdown();

private:
	TUniquePtr<FDeviceExplorerHostServer> Server;
	uint32 ParentProcessId = 0;
};
