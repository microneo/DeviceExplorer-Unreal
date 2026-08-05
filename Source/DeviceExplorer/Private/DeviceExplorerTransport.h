#pragma once

#include "CoreMinimal.h"
#include "DeviceExplorerEndpoint.h"

struct FDeviceExplorerTransportCallbacks
{
	TFunction<void()> OnConnected;
	TFunction<void(const FString&)> OnConnectionError;
	TFunction<void(int32, const FString&, bool)> OnClosed;
	TFunction<void(const FString&)> OnMessage;
};

/**
 * The small transport surface used by the runtime module. Implementations own
 * sockets and timing; protocol bytes are encoded and decoded by DeviceExplorerWire.
 * Every callback is delivered on the game thread from Connect() or Tick().
 */
class IDeviceExplorerTransport
{
public:
	virtual ~IDeviceExplorerTransport() = default;

	virtual void Connect(const FDeviceExplorerResolvedEndpoint& Endpoint,
	                     const FString& Token,
	                     FDeviceExplorerTransportCallbacks Callbacks) = 0;
	virtual void Tick(double NowSeconds) = 0;
	virtual bool SendText(const FString& Message) = 0;
	virtual void Close() = 0;
	virtual bool IsConnected() const = 0;
	virtual const TCHAR* GetName() const = 0;
};

TUniquePtr<IDeviceExplorerTransport> CreateDeviceExplorerTransport();
TUniquePtr<IDeviceExplorerTransport> CreateDeviceExplorerBuiltinTransport();

#if WITH_WEBSOCKETS
TUniquePtr<IDeviceExplorerTransport> CreateDeviceExplorerEngineTransport();
#endif
