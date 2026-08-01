#include "DeviceExplorerCoreModule.h"

#include "Misc/Paths.h"
#include "Misc/ScopeRWLock.h"

FDeviceExplorerCoreModule& FDeviceExplorerCoreModule::Get()
{
	return FModuleManager::LoadModuleChecked<FDeviceExplorerCoreModule>(TEXT("DeviceExplorerCore"));
}

bool FDeviceExplorerCoreModule::IsAvailable()
{
	return FModuleManager::Get().IsModuleLoaded(TEXT("DeviceExplorerCore"));
}

void FDeviceExplorerCoreModule::RegisterCapability(const FName Owner, const FName Capability)
{
	if (Owner.IsNone() || Capability.IsNone())
	{
		return;
	}

	FWriteScopeLock Lock(RegistryLock);
	const bool bAlreadyRegistered = Capabilities.ContainsByPredicate(
		[Owner, Capability](const FCapabilityRegistration& Registration)
		{
			return Registration.Owner == Owner && Registration.Capability == Capability;
		});
	if (!bAlreadyRegistered)
	{
		Capabilities.Add({ Owner, Capability });
	}
}

bool FDeviceExplorerCoreModule::RegisterCommand(FDeviceExplorerCommandDescriptor Descriptor)
{
	if (Descriptor.Owner.IsNone() || Descriptor.Name.IsNone() || Descriptor.DisplayName.IsEmpty())
	{
		return false;
	}

	FWriteScopeLock Lock(RegistryLock);
	Commands.Add(Descriptor.Name, MoveTemp(Descriptor));
	return true;
}

bool FDeviceExplorerCoreModule::RegisterFileRoot(FDeviceExplorerFileRootDescriptor Descriptor)
{
	if (Descriptor.Owner.IsNone() || Descriptor.Name.IsNone() || Descriptor.DisplayName.IsEmpty() || Descriptor.AbsolutePath.IsEmpty())
	{
		return false;
	}

	Descriptor.AbsolutePath = FPaths::ConvertRelativePathToFull(Descriptor.AbsolutePath);
	FPaths::NormalizeDirectoryName(Descriptor.AbsolutePath);

	FWriteScopeLock Lock(RegistryLock);
	FileRoots.Add(Descriptor.Name, MoveTemp(Descriptor));
	return true;
}

bool FDeviceExplorerCoreModule::RegisterDataModule(FDeviceExplorerDataModuleDescriptor Descriptor)
{
	if (Descriptor.Owner.IsNone() || Descriptor.Name.IsNone() || Descriptor.DisplayName.IsEmpty() || !Descriptor.DataProvider)
	{
		return false;
	}

	TSet<FName> ActionNames;
	for (const FDeviceExplorerModuleActionDescriptor& Action : Descriptor.Actions)
	{
		if (Action.Name.IsNone() || Action.DisplayName.IsEmpty() || !Action.Handler || ActionNames.Contains(Action.Name))
		{
			return false;
		}
		ActionNames.Add(Action.Name);
	}

	TSet<FName> PageNames;
	TSet<FName> FieldNames;
	for (const FDeviceExplorerPageDescriptor& Page : Descriptor.Pages)
	{
		if (Page.Name.IsNone() || Page.DisplayName.IsEmpty() || PageNames.Contains(Page.Name))
		{
			return false;
		}
		PageNames.Add(Page.Name);

		TSet<FName> SectionNames;
		for (const FDeviceExplorerSectionDescriptor& Section : Page.Sections)
		{
			if (Section.Name.IsNone() || Section.DisplayName.IsEmpty() || SectionNames.Contains(Section.Name))
			{
				return false;
			}
			SectionNames.Add(Section.Name);

			for (const FDeviceExplorerFieldDescriptor& Field : Section.Fields)
			{
				if (Field.Name.IsNone() || Field.DisplayName.IsEmpty() || FieldNames.Contains(Field.Name))
				{
					return false;
				}
				FieldNames.Add(Field.Name);
			}
		}
	}

	Descriptor.RefreshIntervalMs = FMath::Clamp(Descriptor.RefreshIntervalMs, 0, 60 * 60 * 1000);
	FWriteScopeLock Lock(RegistryLock);
	DataModules.Add(Descriptor.Name, MoveTemp(Descriptor));
	return true;
}

void FDeviceExplorerCoreModule::UnregisterOwner(const FName Owner)
{
	FWriteScopeLock Lock(RegistryLock);
	Capabilities.RemoveAll(
		[Owner](const FCapabilityRegistration& Registration)
		{
			return Registration.Owner == Owner;
		});

	for (auto Iterator = Commands.CreateIterator(); Iterator; ++Iterator)
	{
		if (Iterator.Value().Owner == Owner)
		{
			Iterator.RemoveCurrent();
		}
	}
	for (auto Iterator = FileRoots.CreateIterator(); Iterator; ++Iterator)
	{
		if (Iterator.Value().Owner == Owner)
		{
			Iterator.RemoveCurrent();
		}
	}
	for (auto Iterator = DataModules.CreateIterator(); Iterator; ++Iterator)
	{
		if (Iterator.Value().Owner == Owner)
		{
			Iterator.RemoveCurrent();
		}
	}
}

FDeviceExplorerRegistrySnapshot FDeviceExplorerCoreModule::Snapshot() const
{
	FReadScopeLock Lock(RegistryLock);
	FDeviceExplorerRegistrySnapshot Result;

	for (const FCapabilityRegistration& Registration : Capabilities)
	{
		Result.Capabilities.AddUnique(Registration.Capability);
	}
	Commands.GenerateValueArray(Result.Commands);
	FileRoots.GenerateValueArray(Result.FileRoots);
	DataModules.GenerateValueArray(Result.DataModules);

	Result.Capabilities.Sort(FNameLexicalLess());
	Result.Commands.Sort(
		[](const FDeviceExplorerCommandDescriptor& Left, const FDeviceExplorerCommandDescriptor& Right)
		{
			if (Left.Category != Right.Category)
			{
				return Left.Category.LexicalLess(Right.Category);
			}
			return Left.Name.LexicalLess(Right.Name);
		});
	Result.FileRoots.Sort(
		[](const FDeviceExplorerFileRootDescriptor& Left, const FDeviceExplorerFileRootDescriptor& Right)
		{
			return Left.Name.LexicalLess(Right.Name);
		});
	Result.DataModules.Sort(
		[](const FDeviceExplorerDataModuleDescriptor& Left, const FDeviceExplorerDataModuleDescriptor& Right)
		{
			return Left.Name.LexicalLess(Right.Name);
		});
	return Result;
}

bool FDeviceExplorerCoreModule::FindCommand(const FName Name, FDeviceExplorerCommandDescriptor& OutDescriptor) const
{
	FReadScopeLock Lock(RegistryLock);
	const FDeviceExplorerCommandDescriptor* Descriptor = Commands.Find(Name);
	if (Descriptor == nullptr)
	{
		return false;
	}
	OutDescriptor = *Descriptor;
	return true;
}

bool FDeviceExplorerCoreModule::FindFileRoot(const FName Name, FDeviceExplorerFileRootDescriptor& OutDescriptor) const
{
	FReadScopeLock Lock(RegistryLock);
	const FDeviceExplorerFileRootDescriptor* Descriptor = FileRoots.Find(Name);
	if (Descriptor == nullptr)
	{
		return false;
	}
	OutDescriptor = *Descriptor;
	return true;
}

bool FDeviceExplorerCoreModule::FindDataModule(const FName Name, FDeviceExplorerDataModuleDescriptor& OutDescriptor) const
{
	FReadScopeLock Lock(RegistryLock);
	const FDeviceExplorerDataModuleDescriptor* Descriptor = DataModules.Find(Name);
	if (Descriptor == nullptr)
	{
		return false;
	}
	OutDescriptor = *Descriptor;
	return true;
}

IMPLEMENT_MODULE(FDeviceExplorerCoreModule, DeviceExplorerCore)
