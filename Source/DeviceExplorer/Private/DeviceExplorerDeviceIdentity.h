#pragma once

#include "CoreMinimal.h"

struct FDeviceExplorerConnectionIdentity
{
	FString DeviceId;
	uint64 DeviceSession = 0;
	FString ConnectionId;
};

class FDeviceExplorerDeviceIdentityStore final
{
public:
	explicit FDeviceExplorerDeviceIdentityStore(FString InPath);

	bool Load(FString& OutError);
	bool AllocateConnection(FDeviceExplorerConnectionIdentity& OutIdentity, FString& OutError);
	bool AdvancePast(uint64 LastKnownSession, FString& OutError);
	const FString& GetDeviceId() const { return DeviceId; }
	const FString& GetPath() const { return Path; }

private:
	bool Persist(FString& OutError) const;
	void ResetIdentity();

	FString Path;
	FString DeviceId;
	FString MachineMarker;
	uint64 NextDeviceSession = 1;
};
