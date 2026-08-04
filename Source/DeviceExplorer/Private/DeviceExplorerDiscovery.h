#pragma once

#include "CoreMinimal.h"

struct FDeviceExplorerDiscoveredServer
{
	FString Host;
	int32 Port = 0;
	FString Fingerprint;
	FString Instance;
};

using FDeviceExplorerDiscoveryCallback = TFunction<void(FDeviceExplorerDiscoveredServer)>;

class IDeviceExplorerDiscovery
{
public:
	virtual ~IDeviceExplorerDiscovery() = default;
	virtual void Start(FDeviceExplorerDiscoveryCallback Callback) = 0;
	virtual void Stop() = 0;
};

TUniquePtr<IDeviceExplorerDiscovery> CreateDeviceExplorerDiscovery();
