#include "DeviceExplorerHostRoster.h"

#include <algorithm>
#include <utility>

namespace DeviceExplorer::Host
{
namespace
{
bool ValidDevice(const RosterDevice& Device)
{
	return !Device.DeviceId.empty() && Device.DeviceId.size() <= 128 && Device.DeviceSession != 0 &&
	       !Device.ConnectionId.empty() && Device.ConnectionId.size() <= 128 && Device.Name.size() <= 256 &&
	       Device.ProjectName.size() <= 256 && Device.Platform.size() <= 128 && Device.Configuration.size() <= 128 &&
	       Device.EngineVersion.size() <= 128 && Device.BuildVersion.size() <= 128 && Device.Capabilities.size() <= 32 &&
	       std::all_of(Device.Capabilities.begin(), Device.Capabilities.end(), [](const std::string& Capability)
	       {
		       return !Capability.empty() && Capability.size() <= 64;
	       });
}
}    // namespace

DistributedRoster::DistributedRoster(std::string InLocalNodeId,
	                                 const std::uint64_t LocalHostSession,
	                                 const std::size_t InMaximumDevices)
	: LocalNodeId(std::move(InLocalNodeId)), MaximumDevices(InMaximumDevices)
{
	RosterOwnerState& LocalState = OwnerTables[LocalNodeId];
	LocalState.HostSession = LocalHostSession;
	LocalState.Revision = 1;
	LocalState.Reachable = true;
}

const RosterOwnerState& DistributedRoster::Local() const
{
	return OwnerTables.at(LocalNodeId);
}

std::uint64_t DistributedRoster::AttachLocal(RosterDevice Device)
{
	RosterOwnerState& Owner = OwnerTables.at(LocalNodeId);
	if (!ValidDevice(Device)) return Owner.Revision;
	const bool NewDevice = Owner.Devices.find(Device.DeviceId) == Owner.Devices.end();
	if (NewDevice && DeviceCount() >= MaximumDevices) return Owner.Revision;
	Owner.Devices[Device.DeviceId] = std::move(Device);
	return ++Owner.Revision;
}

std::uint64_t DistributedRoster::DetachLocal(const std::string& DeviceId, const std::uint64_t DeviceSession)
{
	RosterOwnerState& Owner = OwnerTables.at(LocalNodeId);
	const auto Found = Owner.Devices.find(DeviceId);
	if (Found == Owner.Devices.end() || Found->second.DeviceSession != DeviceSession) return Owner.Revision;
	Owner.Devices.erase(Found);
	return ++Owner.Revision;
}

void DistributedRoster::AdvanceLocalHostSession(const std::uint64_t HostSession)
{
	RosterOwnerState& Owner = OwnerTables.at(LocalNodeId);
	Owner.HostSession = HostSession;
	Owner.Revision = 1;
}

bool DistributedRoster::FitsReplacing(const std::string& NodeId, const std::size_t ReplacementSize) const
{
	std::size_t Existing = 0;
	const auto Found = OwnerTables.find(NodeId);
	if (Found != OwnerTables.end()) Existing = Found->second.Devices.size();
	return ReplacementSize <= MaximumDevices && DeviceCount() - Existing <= MaximumDevices - ReplacementSize;
}

RosterOwnerState* DistributedRoster::FindBoundOwner(const std::string& NodeId, const std::uint64_t HostSession)
{
	const auto Found = OwnerTables.find(NodeId);
	if (Found == OwnerTables.end() || Found->second.HostSession != HostSession) return nullptr;
	return &Found->second;
}

RosterApplyResult DistributedRoster::ApplyFull(const std::string& BoundNodeId,
	                                            const std::uint64_t BoundHostSession,
	                                            const std::uint64_t Revision,
	                                            std::vector<RosterDevice> Devices)
{
	if (BoundNodeId.empty() || BoundNodeId == LocalNodeId || BoundHostSession == 0 || Revision == 0)
		return RosterApplyResult::WrongOwner;
	std::map<std::string, RosterDevice> Replacement;
	for (RosterDevice& Device : Devices)
	{
		if (!ValidDevice(Device) || !Replacement.emplace(Device.DeviceId, std::move(Device)).second)
			return RosterApplyResult::Invalid;
	}
	const auto Existing = OwnerTables.find(BoundNodeId);
	if (Existing != OwnerTables.end())
	{
		if (BoundHostSession < Existing->second.HostSession) return RosterApplyResult::Ignored;
		if (BoundHostSession == Existing->second.HostSession && Revision < Existing->second.Revision)
			return RosterApplyResult::Ignored;
	}
	if (!FitsReplacing(BoundNodeId, Replacement.size())) return RosterApplyResult::CapacityExceeded;
	RosterOwnerState& Owner = OwnerTables[BoundNodeId];
	Owner.HostSession = BoundHostSession;
	Owner.Revision = Revision;
	Owner.Reachable = true;
	Owner.Devices = std::move(Replacement);
	return RosterApplyResult::Applied;
}

RosterApplyResult DistributedRoster::ApplyAttached(const std::string& BoundNodeId,
	                                                const std::uint64_t BoundHostSession,
	                                                const std::uint64_t Revision,
	                                                RosterDevice Device)
{
	if (!ValidDevice(Device)) return RosterApplyResult::Invalid;
	RosterOwnerState* Owner = FindBoundOwner(BoundNodeId, BoundHostSession);
	if (Owner == nullptr)
	{
		const auto Existing = OwnerTables.find(BoundNodeId);
		return Existing != OwnerTables.end() && BoundHostSession < Existing->second.HostSession
			? RosterApplyResult::Ignored : RosterApplyResult::NeedFull;
	}
	if (Revision <= Owner->Revision) return RosterApplyResult::Ignored;
	if (Revision != Owner->Revision + 1) return RosterApplyResult::NeedFull;
	if (Owner->Devices.find(Device.DeviceId) == Owner->Devices.end() && DeviceCount() >= MaximumDevices)
		return RosterApplyResult::CapacityExceeded;
	Owner->Devices[Device.DeviceId] = std::move(Device);
	Owner->Revision = Revision;
	Owner->Reachable = true;
	return RosterApplyResult::Applied;
}

RosterApplyResult DistributedRoster::ApplyDetached(const std::string& BoundNodeId,
	                                                const std::uint64_t BoundHostSession,
	                                                const std::uint64_t Revision,
	                                                const std::string& DeviceId,
	                                                const std::uint64_t DeviceSession)
{
	RosterOwnerState* Owner = FindBoundOwner(BoundNodeId, BoundHostSession);
	if (Owner == nullptr)
	{
		const auto Existing = OwnerTables.find(BoundNodeId);
		return Existing != OwnerTables.end() && BoundHostSession < Existing->second.HostSession
			? RosterApplyResult::Ignored : RosterApplyResult::NeedFull;
	}
	if (Revision <= Owner->Revision) return RosterApplyResult::Ignored;
	if (Revision != Owner->Revision + 1) return RosterApplyResult::NeedFull;
	const auto Found = Owner->Devices.find(DeviceId);
	if (Found != Owner->Devices.end() && Found->second.DeviceSession == DeviceSession) Owner->Devices.erase(Found);
	Owner->Revision = Revision;
	return RosterApplyResult::Applied;
}

void DistributedRoster::SetReachable(const std::string& NodeId,
	                                 const std::uint64_t HostSession,
	                                 const bool Reachable)
{
	if (RosterOwnerState* Owner = FindBoundOwner(NodeId, HostSession)) Owner->Reachable = Reachable;
}

bool DistributedRoster::RemoveUnreachableOwner(const std::string& NodeId, const std::uint64_t HostSession)
{
	if (NodeId == LocalNodeId) return false;
	const auto Found = OwnerTables.find(NodeId);
	if (Found == OwnerTables.end() || Found->second.HostSession != HostSession || Found->second.Reachable) return false;
	OwnerTables.erase(Found);
	return true;
}

std::uint64_t DistributedRoster::KnownDeviceSession(const std::string& DeviceId) const
{
	std::uint64_t Result = 0;
	for (const auto& OwnerPair : OwnerTables)
	{
		const auto Found = OwnerPair.second.Devices.find(DeviceId);
		if (Found != OwnerPair.second.Devices.end()) Result = std::max(Result, Found->second.DeviceSession);
	}
	return Result;
}

std::vector<RosterViewEntry> DistributedRoster::View() const
{
	std::map<std::string, std::vector<RosterViewEntry>> Candidates;
	for (const auto& OwnerPair : OwnerTables)
	{
		for (const auto& DevicePair : OwnerPair.second.Devices)
		{
			Candidates[DevicePair.first].push_back({ DevicePair.second, OwnerPair.first,
				OwnerPair.second.HostSession, OwnerPair.second.Reachable ? "connected" : "unreachable" });
		}
	}
	std::vector<RosterViewEntry> Result;
	for (auto& CandidatePair : Candidates)
	{
		auto& Values = CandidatePair.second;
		const std::uint64_t MaximumSession = std::max_element(Values.begin(), Values.end(), [](const auto& Left, const auto& Right)
		{
			return Left.Device.DeviceSession < Right.Device.DeviceSession;
		})->Device.DeviceSession;
		std::vector<RosterViewEntry*> Winners;
		for (RosterViewEntry& Value : Values)
		{
			if (Value.Device.DeviceSession == MaximumSession) Winners.push_back(&Value);
		}
		if (Winners.size() == 1)
		{
			Result.push_back(std::move(*Winners.front()));
		}
		else
		{
			RosterViewEntry Ambiguous = *Winners.front();
			Ambiguous.State = "ambiguous_owner";
			Ambiguous.OwnerNodeId.clear();
			Ambiguous.OwnerHostSession = 0;
			Result.push_back(std::move(Ambiguous));
		}
	}
	return Result;
}

std::size_t DistributedRoster::DeviceCount() const
{
	std::size_t Result = 0;
	for (const auto& Pair : OwnerTables) Result += Pair.second.Devices.size();
	return Result;
}
}    // namespace DeviceExplorer::Host
