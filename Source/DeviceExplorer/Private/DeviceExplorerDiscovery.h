#pragma once

#include "CoreMinimal.h"
#include "DeviceExplorerEndpoint.h"

bool ParseDeviceExplorerEndpoint(const FString& Text, FDeviceExplorerSerializedEndpoint& OutEndpoint, FString* OutError = nullptr);
TArray<TUniquePtr<IDeviceExplorerEndpointSource>> CreateDeviceExplorerEndpointSources();
