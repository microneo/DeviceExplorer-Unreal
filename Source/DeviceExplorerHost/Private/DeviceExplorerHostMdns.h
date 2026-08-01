#pragma once

#include "CoreMinimal.h"

class FInternetAddr;
class FSocket;

class FDeviceExplorerHostMdns
{
public:
	FDeviceExplorerHostMdns(int32 InDevicePort, int32 InDashboardPort, FString InToken);
	~FDeviceExplorerHostMdns();

	bool Start();
	void Tick();
	void Stop();

private:
	void Announce(uint32 Ttl);
	void SendAnnouncement(const FInternetAddr& Destination, uint32 Ttl);
	TArray<uint8> BuildAnnouncement(uint32 Ttl) const;
	void DrainQueries();

	int32 DevicePort = 0;
	int32 DashboardPort = 0;
	FString Token;
	FString ServiceName;
	FString InstanceName;
	FString HostName;
	TArray<TArray<uint8>> HostAddresses;

	FSocket* Socket = nullptr;
	TSharedPtr<FInternetAddr> MulticastAddress;
	double LastAnnouncementSeconds = 0.0;
	bool bStarted = false;
};
