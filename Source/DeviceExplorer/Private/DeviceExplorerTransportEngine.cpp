#include "DeviceExplorerTransport.h"

#if WITH_WEBSOCKETS

#include "GenericPlatform/GenericPlatformHttp.h"
#include "IWebSocket.h"
#include "Modules/ModuleManager.h"
#include "WebSocketsModule.h"

namespace
{
FString BuildDeviceExplorerEngineWebSocketUrl(const FDeviceExplorerResolvedEndpoint& Endpoint, const FString& Token)
{
	return FString::Printf(TEXT("ws://%s/device/connect?token=%s"),
	                       *Endpoint.Serialized.ToString(),
	                       *FGenericPlatformHttp::UrlEncode(Token));
}

class FDeviceExplorerEngineTransport final : public IDeviceExplorerTransport
{
public:
	virtual ~FDeviceExplorerEngineTransport() override { Close(); }

	virtual void Connect(const FDeviceExplorerResolvedEndpoint& Endpoint,
	                     const FString& Token,
	                     FDeviceExplorerTransportCallbacks InCallbacks) override
	{
		Close();
		Callbacks = MoveTemp(InCallbacks);
		FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
		Socket = FWebSocketsModule::Get().CreateWebSocket(BuildDeviceExplorerEngineWebSocketUrl(Endpoint, Token));
		Socket->OnConnected().AddLambda(
			[this]()
			{
				if (Callbacks.OnConnected) Callbacks.OnConnected();
			});
		Socket->OnConnectionError().AddLambda(
			[this](const FString& Error)
			{
				if (Callbacks.OnConnectionError) Callbacks.OnConnectionError(Error);
			});
		Socket->OnClosed().AddLambda(
			[this](const int32 StatusCode, const FString& Reason, const bool bWasClean)
			{
				if (Callbacks.OnClosed) Callbacks.OnClosed(StatusCode, Reason, bWasClean);
			});
		Socket->OnMessage().AddLambda(
			[this](const FString& Message)
			{
				if (Callbacks.OnMessage) Callbacks.OnMessage(Message);
			});
		Socket->Connect();
	}

	virtual void Tick(double NowSeconds) override { (void) NowSeconds; }

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
		Socket.Reset();
		Callbacks = {};
	}

	virtual bool IsConnected() const override { return Socket.IsValid() && Socket->IsConnected(); }
	virtual const TCHAR* GetName() const override { return TEXT("Engine"); }

private:
	FDeviceExplorerTransportCallbacks Callbacks;
	TSharedPtr<IWebSocket> Socket;
};
}    // namespace

TUniquePtr<IDeviceExplorerTransport> CreateDeviceExplorerEngineTransport()
{
	return MakeUnique<FDeviceExplorerEngineTransport>();
}

#endif
