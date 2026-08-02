#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

class FDeviceExplorerConsoleCatalog;
class FDeviceExplorerLogService;
class IDeviceExplorerDiscovery;
class IWebSocket;

class FDeviceExplorerModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterDefaultFeatures();
	bool Tick(float DeltaTime);
	void OnServerDiscovered(const FString& Host, int32 Port, const FString& Token);
	void Connect();
	void Disconnect();
	void SendHello();
	void SendHeartbeat();
	void HandleMessage(const FString& Message);
	void ExecuteCommand(const TSharedPtr<class FJsonObject>& Message);
	void ListConsoleObjects(const TSharedPtr<class FJsonObject>& Message);
	void SendConsoleIndex(const FString& RequestId, bool bRefresh);
	void ListFiles(const TSharedPtr<class FJsonObject>& Message);
	void GetModuleData(const TSharedPtr<class FJsonObject>& Message);
	void InvokeModuleAction(const TSharedPtr<class FJsonObject>& Message);
	void UploadFile(const TSharedPtr<class FJsonObject>& Message);
	void StartUpload(const FString& TransferId, const FString& UploadURL, const FString& UploadPath, const FString& ArchivePath);
	void SendTransferFailure(const FString& TransferId, const FString& Error);
	void SendJson(const TSharedRef<class FJsonObject>& Message);
	FString GetOrCreateDeviceId() const;

	TUniquePtr<IDeviceExplorerDiscovery> Discovery;
	TUniquePtr<FDeviceExplorerLogService> LogService;
	TUniquePtr<FDeviceExplorerConsoleCatalog> ConsoleCatalog;
	TSharedPtr<IWebSocket> Socket;
	FTSTicker::FDelegateHandle TickerHandle;

	FString ServerHost;
	FString ServerToken;
	FString DeviceId;
	int32 ServerPort = 0;
	double LastHeartbeatSeconds = 0.0;
	double NextReconnectSeconds = 0.0;
	double ReconnectDelaySeconds = 1.0;
	bool bConnecting = false;
	bool bStarted = false;
};
