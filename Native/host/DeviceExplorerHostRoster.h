#pragma once

#include "DeviceExplorerHostCore.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace DeviceExplorer::Host
{
enum class RosterApplyResult : std::uint8_t
{
	Applied,
	Ignored,
	NeedFull,
	WrongOwner,
	CapacityExceeded,
	Invalid
};

struct RosterOwnerState
{
	std::uint64_t HostSession = 0;
	std::uint64_t Revision = 0;
	bool Reachable = false;
	std::map<std::string, RosterDevice> Devices;
};

struct RosterViewEntry
{
	RosterDevice Device;
	std::string OwnerNodeId;
	std::uint64_t OwnerHostSession = 0;
	std::string State;
};

class DistributedRoster final
{
public:
	DistributedRoster(std::string LocalNodeId, std::uint64_t LocalHostSession, std::size_t MaximumDevices);

	std::uint64_t AttachLocal(RosterDevice Device);
	std::uint64_t DetachLocal(const std::string& DeviceId, std::uint64_t DeviceSession);
	void AdvanceLocalHostSession(std::uint64_t HostSession);
	const RosterOwnerState& Local() const;

	RosterApplyResult ApplyFull(const std::string& BoundNodeId,
	                            std::uint64_t BoundHostSession,
	                            std::uint64_t Revision,
	                            std::vector<RosterDevice> Devices);
	RosterApplyResult ApplyAttached(const std::string& BoundNodeId,
	                                std::uint64_t BoundHostSession,
	                                std::uint64_t Revision,
	                                RosterDevice Device);
	RosterApplyResult ApplyDetached(const std::string& BoundNodeId,
	                                std::uint64_t BoundHostSession,
	                                std::uint64_t Revision,
	                                const std::string& DeviceId,
	                                std::uint64_t DeviceSession);
	void SetReachable(const std::string& NodeId, std::uint64_t HostSession, bool Reachable);
	bool RemoveUnreachableOwner(const std::string& NodeId, std::uint64_t HostSession);
	std::uint64_t KnownDeviceSession(const std::string& DeviceId) const;
	std::vector<RosterViewEntry> View() const;
	std::size_t DeviceCount() const;
	const std::map<std::string, RosterOwnerState>& Owners() const { return OwnerTables; }

private:
	bool FitsReplacing(const std::string& NodeId, std::size_t ReplacementSize) const;
	RosterOwnerState* FindBoundOwner(const std::string& NodeId, std::uint64_t HostSession);

	std::string LocalNodeId;
	std::size_t MaximumDevices;
	std::map<std::string, RosterOwnerState> OwnerTables;
};
}    // namespace DeviceExplorer::Host
