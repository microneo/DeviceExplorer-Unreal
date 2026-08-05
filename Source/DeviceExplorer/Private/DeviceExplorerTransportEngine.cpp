#include "DeviceExplorerTransport.h"

#if WITH_WEBSOCKETS

#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "WebSocketsModule.h"

namespace
{
class FDeviceExplorerEngineTransport final : public IDeviceExplorerTransport
{
public:
	virtual ~FDeviceExplorerEngineTransport() override { Close(); }

	virtual void Connect(const FDeviceExplorerResolvedEndpoint& Endpoint,
	                     FDeviceExplorerTransportCallbacks InCallbacks) override
	{
		Close();
		Callbacks = MoveTemp(InCallbacks);
		FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
		Socket = FWebSocketsModule::Get().CreateWebSocket(
			FString::Printf(TEXT("ws://%s/device/connect"), *Endpoint.Serialized.ToString()));
		// Every dispatch copies the handler first: a handler is allowed to call
		// Close(), which clears Callbacks while the handler is still running.
		Socket->OnConnected().AddLambda(
			[this]()
			{
				if (const TFunction<void()> Handler = Callbacks.OnConnected) Handler();
			});
		Socket->OnConnectionError().AddLambda(
			[this](const FString& Error)
			{
				if (const TFunction<void(const FString&)> Handler = Callbacks.OnConnectionError) Handler(Error);
			});
		Socket->OnClosed().AddLambda(
			[this](const int32 StatusCode, const FString& Reason, const bool bWasClean)
			{
				if (const TFunction<void(int32, const FString&, bool)> Handler = Callbacks.OnClosed)
				{
					Handler(StatusCode, Reason, bWasClean);
				}
			});
		Socket->OnMessage().AddLambda(
			[this](const FString& Message)
			{
				if (const TFunction<void(const FString&)> Handler = Callbacks.OnMessage) Handler(Message);
			});
		Socket->Connect();
	}

	virtual void Tick(double NowSeconds) override
	{
		(void) NowSeconds;
		ClosingSockets.Reset();
	}

	virtual bool SendText(const FString& Message) override
	{
		if (!Socket.IsValid() || !Socket->IsConnected()) return false;
		Socket->Send(Message);
		return true;
	}

	virtual void Close() override
	{
		if (!Socket.IsValid()) return;
		Socket->OnConnected().Clear();
		Socket->OnConnectionError().Clear();
		Socket->OnClosed().Clear();
		Socket->OnMessage().Clear();
		if (Socket->IsConnected()) Socket->Close();
		// Close() can be reached from inside one of the socket's own delegates, so
		// the last reference is handed to Tick instead of being dropped here.
		ClosingSockets.Add(MoveTemp(Socket));
		Socket.Reset();
		Callbacks = {};
	}

	virtual bool IsConnected() const override { return Socket.IsValid() && Socket->IsConnected(); }
	virtual const TCHAR* GetName() const override { return TEXT("Engine"); }

private:
	FDeviceExplorerTransportCallbacks Callbacks;
	TSharedPtr<IWebSocket> Socket;
	TArray<TSharedPtr<IWebSocket>> ClosingSockets;
};
}    // namespace

TUniquePtr<IDeviceExplorerTransport> CreateDeviceExplorerEngineTransport()
{
	return MakeUnique<FDeviceExplorerEngineTransport>();
}

#endif
