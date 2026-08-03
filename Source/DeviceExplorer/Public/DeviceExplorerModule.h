#pragma once

#include "Containers/Ticker.h"
#include "CoreMinimal.h"
#include "DeviceExplorerEndpoint.h"
#include "Modules/ModuleManager.h"

#include <atomic>

class FDeviceExplorerConnectionCoordinator;
class FDeviceExplorerConsoleCatalog;
class FDeviceExplorerLogService;
class IConsoleObject;
class IWebSocket;

class FDeviceExplorerModule final : public IModuleInterface
{
public:
	virtual ~FDeviceExplorerModule() override;
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

private:
	void RegisterDefaultFeatures();
	bool Tick(float DeltaTime);
	void OnEndpointEvent(EDeviceExplorerEndpointEvent Event, FDeviceExplorerEndpointCandidate Candidate);
	void Connect(FDeviceExplorerEndpointCandidate Candidate);
	void Disconnect();
	void ConnectManually(const TArray<FString>& Arguments);
	void UnpinManualEndpoint(const TArray<FString>& Arguments);
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

	TArray<TUniquePtr<IDeviceExplorerEndpointSource>> EndpointSources;
	TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> EndpointCallbackGate;
	TUniquePtr<FDeviceExplorerConnectionCoordinator> ConnectionCoordinator;
	TUniquePtr<FDeviceExplorerLogService> LogService;
	TUniquePtr<FDeviceExplorerConsoleCatalog> ConsoleCatalog;
	TSharedPtr<IWebSocket> Socket;
	FTSTicker::FDelegateHandle TickerHandle;
	IConsoleObject* ConnectConsoleCommand = nullptr;
	IConsoleObject* UnpinConsoleCommand = nullptr;

	FString DeviceId;
	double LastHeartbeatSeconds = 0.0;
	uint64 ConnectionGeneration = 0;
	bool bConnecting = false;
	bool bStarted = false;
};
