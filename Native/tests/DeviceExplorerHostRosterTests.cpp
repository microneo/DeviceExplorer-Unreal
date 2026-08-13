#include "DeviceExplorerHostRoster.h"

#include <cstdlib>
#include <iostream>

namespace
{
#define CHECK(Expression) do { if (!(Expression)) { std::cerr << "check failed: " #Expression " at line " << __LINE__ << '\n'; return EXIT_FAILURE; } } while (false)

DeviceExplorer::Host::RosterDevice Device(const char* Id, const std::uint64_t Session, const char* Connection)
{
	DeviceExplorer::Host::RosterDevice Result;
	Result.DeviceId = Id;
	Result.DeviceSession = Session;
	Result.ConnectionId = Connection;
	Result.Name = Id;
	return Result;
}
}    // namespace

int main()
{
	using namespace DeviceExplorer::Host;
	DistributedRoster Roster("local", 7, 8);
	CHECK(Roster.Local().Revision == 1);
	CHECK(Roster.AttachLocal(Device("device", 10, "local-10")) == 2);
	CHECK(Roster.KnownDeviceSession("device") == 10);
	CHECK(Roster.ApplyFull("peer-a", 4, 1, { Device("device", 11, "a-11") }) == RosterApplyResult::Applied);
	CHECK(Roster.View().size() == 1);
	CHECK(Roster.View().front().OwnerNodeId == "peer-a");
	CHECK(Roster.View().front().Device.DeviceSession == 11);

	CHECK(Roster.ApplyAttached("peer-a", 4, 3, Device("other", 1, "gap")) == RosterApplyResult::NeedFull);
	CHECK(Roster.ApplyAttached("peer-a", 4, 2, Device("other", 1, "ok")) == RosterApplyResult::Applied);
	CHECK(Roster.ApplyDetached("peer-a", 4, 3, "device", 10) == RosterApplyResult::Applied);
	CHECK(Roster.KnownDeviceSession("device") == 11);
	CHECK(Roster.ApplyDetached("peer-a", 4, 4, "device", 11) == RosterApplyResult::Applied);
	CHECK(Roster.KnownDeviceSession("device") == 10);

	CHECK(Roster.ApplyFull("peer-a", 5, 1, { Device("device", 12, "a-12") }) == RosterApplyResult::Applied);
	CHECK(Roster.ApplyFull("peer-b", 2, 1, { Device("device", 12, "b-12") }) == RosterApplyResult::Applied);
	CHECK(Roster.View().front().State == "ambiguous_owner");
	CHECK(Roster.ApplyAttached("peer-a", 4, 99, Device("device", 99, "stale-owner")) == RosterApplyResult::Ignored);
	Roster.SetReachable("peer-b", 2, false);
	CHECK(!Roster.RemoveUnreachableOwner("peer-b", 1));
	CHECK(Roster.RemoveUnreachableOwner("peer-b", 2));
	CHECK(Roster.View().front().OwnerNodeId == "peer-a");

	CHECK(Roster.ApplyFull("peer-c", 1, 1,
	                       { Device("1", 1, "1"), Device("2", 1, "2"), Device("3", 1, "3"),
	                         Device("4", 1, "4"), Device("5", 1, "5"), Device("6", 1, "6"),
	                         Device("7", 1, "7") }) ==
	      RosterApplyResult::CapacityExceeded);
	std::cout << "host roster tests passed\n";
	return EXIT_SUCCESS;
}
