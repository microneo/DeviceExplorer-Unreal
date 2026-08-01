#pragma once

#include "CoreMinimal.h"
#include "DeviceExplorerTypes.h"
#include "Modules/ModuleManager.h"

class DEVICEEXPLORERCORE_API FDeviceExplorerCoreModule final : public IModuleInterface
{
public:
	static FDeviceExplorerCoreModule& Get();
	static bool IsAvailable();

	void RegisterCapability(FName Owner, FName Capability);
	bool RegisterCommand(FDeviceExplorerCommandDescriptor Descriptor);
	bool RegisterFileRoot(FDeviceExplorerFileRootDescriptor Descriptor);
	bool RegisterDataModule(FDeviceExplorerDataModuleDescriptor Descriptor);
	void UnregisterOwner(FName Owner);

	FDeviceExplorerRegistrySnapshot Snapshot() const;
	bool FindCommand(FName Name, FDeviceExplorerCommandDescriptor& OutDescriptor) const;
	bool FindFileRoot(FName Name, FDeviceExplorerFileRootDescriptor& OutDescriptor) const;
	bool FindDataModule(FName Name, FDeviceExplorerDataModuleDescriptor& OutDescriptor) const;

private:
	struct FCapabilityRegistration
	{
		FName Owner;
		FName Capability;
	};

	mutable FRWLock RegistryLock;
	TArray<FCapabilityRegistration> Capabilities;
	TMap<FName, FDeviceExplorerCommandDescriptor> Commands;
	TMap<FName, FDeviceExplorerFileRootDescriptor> FileRoots;
	TMap<FName, FDeviceExplorerDataModuleDescriptor> DataModules;
};
