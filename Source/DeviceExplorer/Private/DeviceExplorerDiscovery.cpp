#include "DeviceExplorerDiscovery.h"

#include "Async/Async.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"

#if PLATFORM_IOS
TUniquePtr<IDeviceExplorerDiscovery> CreateIOSDeviceExplorerDiscovery();
#else
TUniquePtr<IDeviceExplorerDiscovery> CreateMdnsDeviceExplorerDiscovery();
#endif

namespace
{
class FManualDeviceExplorerDiscovery final : public IDeviceExplorerDiscovery
{
public:
	virtual void Start(FDeviceExplorerDiscoveryCallback InCallback) override
	{
		FString Endpoint;
		FString Token;
		if (!FParse::Value(FCommandLine::Get(), TEXT("DeviceExplorerServer="), Endpoint))
		{
			return;
		}
		FParse::Value(FCommandLine::Get(), TEXT("DeviceExplorerToken="), Token);

		FString Host;
		FString PortText;
		if (!Endpoint.Split(TEXT(":"), &Host, &PortText, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			return;
		}
		const int32 Port = FCString::Atoi(*PortText);
		if (Host.IsEmpty() || Port <= 0 || Port > 65535)
		{
			return;
		}

		AsyncTask(ENamedThreads::GameThread,
		          [Callback = MoveTemp(InCallback), Host = MoveTemp(Host), Token = MoveTemp(Token), Port]() mutable
		          {
					  Callback({ MoveTemp(Host), Port, MoveTemp(Token), TEXT("Manual") });
				  });
	}

	virtual void Stop() override {}
};
}    // namespace

TUniquePtr<IDeviceExplorerDiscovery> CreateDeviceExplorerDiscovery()
{
	FString Endpoint;
	if (FParse::Value(FCommandLine::Get(), TEXT("DeviceExplorerServer="), Endpoint))
	{
		return MakeUnique<FManualDeviceExplorerDiscovery>();
	}

#if PLATFORM_IOS
	return CreateIOSDeviceExplorerDiscovery();
#else
	return CreateMdnsDeviceExplorerDiscovery();
#endif
}
