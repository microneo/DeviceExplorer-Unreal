#pragma once

#include "Async/Future.h"
#include "CoreMinimal.h"

class FDeviceExplorerHostMdns;
class FSocket;
class FJsonObject;

struct FDeviceExplorerHostConfig
{
	FString WebRoot;
	FString TransferDirectory;
	FString Token;
	int32 DashboardPort = 18080;
	int32 DevicePort = 18081;
	int32 TracePort = 1981;
	int32 LogCapacity = 100000;
	int64 LogCapacityBytes = 16ll * 1024 * 1024;
	int32 MaximumDevices = 1024;
	int64 MaximumTransferBytes = 16ll * 1024 * 1024 * 1024;
	double RequestTimeoutSeconds = 10.0;
	double TransferTtlSeconds = 1800.0;
};

class FDeviceExplorerHostServer
{
public:
	explicit FDeviceExplorerHostServer(FDeviceExplorerHostConfig InConfig);
	~FDeviceExplorerHostServer();

	bool Start();
	bool Tick();
	void Stop();

private:
	struct FDeviceConnection;
	struct FDeviceState;
	struct FLogEntry;
	struct FPendingRequest;
	struct FTransfer;
	struct FHttpRequest;
	struct FKnownDeviceSession
	{
		uint64 Session = 0;
		FDateTime LastSeen;
	};

	bool CreateListener(const FString& Address, int32 Port, FSocket*& OutSocket);
	void AcceptConnections(FSocket* Listener, bool bDashboard);
	void DispatchConnection(FSocket* Socket, bool bDashboard);
	void CloseClientSocket(FSocket* Socket);
	void ReapWorkers();

	void HandleDashboardConnection(FSocket* Socket);
	void HandleDeviceConnection(FSocket* Socket);
	bool ReadHttpRequest(FSocket* Socket, FHttpRequest& OutRequest);
	bool ReadRequestBody(FSocket* Socket, FHttpRequest& Request, int64 MaximumBytes);

	void RouteDashboardRequest(FSocket* Socket, FHttpRequest& Request);
	void RouteDeviceRequest(FSocket* Socket, FHttpRequest& Request);
	void ServeStaticFile(FSocket* Socket, const FString& RelativePath);

	void HandleWebSocket(FSocket* Socket, const FHttpRequest& Request);
	bool IsTrustedDashboardRequest(const FHttpRequest& Request) const;
	void HandleDeviceAuth(const TSharedRef<FDeviceConnection>& Connection, const FString& Type, const TSharedPtr<FJsonObject>& Message);
	void HandleDeviceMessage(const TSharedRef<FDeviceConnection>& Connection, const FString& Message);
	void AttachDevice(const TSharedRef<FDeviceConnection>& Connection, const TSharedPtr<FJsonObject>& Hello);
	void DetachDevice(const TSharedRef<FDeviceConnection>& Connection);

	void HandleDevicesApi(FSocket* Socket);
	void HandleDeviceApi(FSocket* Socket, FHttpRequest& Request, const FString& DeviceId, const FString& Action);
	void HandleTransferApi(FSocket* Socket, FHttpRequest& Request, const FString& TransferId, const FString& Action);
	void HandleTransferUpload(FSocket* Socket, FHttpRequest& Request, const FString& TransferId);

	TSharedPtr<FJsonObject> SendDeviceRequestAndWait(const FString& DeviceId, const TSharedRef<FJsonObject>& Message);
	bool SendDeviceJson(const FString& DeviceId, const TSharedRef<FJsonObject>& Message);
	void CompletePendingRequest(const TSharedRef<FDeviceConnection>& Connection, const FString& RequestId, const TSharedPtr<FJsonObject>& Result);

	bool ForwardTrace(const FString& TransferId, FString& OutError);
	void CleanupExpiredState();

	FDeviceExplorerHostConfig Config;
	FSocket* DashboardListener = nullptr;
	FSocket* DeviceListener = nullptr;
	TUniquePtr<FDeviceExplorerHostMdns> Mdns;

	FCriticalSection StateMutex;
	TMap<FString, TSharedPtr<FDeviceState>> Devices;
	TMap<FString, FKnownDeviceSession> KnownDeviceSessions;
	TMap<FString, TSharedPtr<FPendingRequest>> PendingRequests;
	TMap<FString, TSharedPtr<FTransfer>> Transfers;

	FCriticalSection WorkerMutex;
	TSet<FSocket*> ClientSockets;
	TArray<TFuture<void>> Workers;

	double LastCleanupSeconds = 0.0;
	TAtomic<bool> bStopping{ false };
	bool bStarted = false;
};
