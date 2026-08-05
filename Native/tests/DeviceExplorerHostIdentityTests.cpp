#include "DeviceExplorerHostIdentity.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace
{
int Failures = 0;

void Check(const bool Condition, const char* Message)
{
	if (Condition) return;
	std::cerr << "FAIL: " << Message << '\n';
	++Failures;
}

struct TemporaryDirectory
{
	TemporaryDirectory()
	{
		const auto Stamp = std::chrono::high_resolution_clock::now().time_since_epoch().count();
		Path = std::filesystem::temp_directory_path() / ("deviceexplorer-identity-test-" + std::to_string(Stamp));
		std::filesystem::create_directories(Path);
	}

	~TemporaryDirectory()
	{
		std::error_code Ignored;
		std::filesystem::remove_all(Path, Ignored);
	}

	std::filesystem::path Path;
};
}    // namespace

int main()
{
	TemporaryDirectory Directory;
	DeviceExplorer::Host::HostIdentityStore Store(Directory.Path);
	DeviceExplorer::Host::HostIdentity First;
	std::string Error;
	Check(Store.LoadAndAdvance(First, Error), "new identity is created");
	Check(Error.empty(), "new identity reports no error");
	Check(First.NodeId.size() == 36, "node id is a UUID");
	Check(First.HostSession == 1, "new identity starts at session one");
	Check(First.InstanceId.size() == 36, "instance id is a UUID");
	Check(std::filesystem::exists(Store.IdentityPath()), "identity is persisted before return");

	DeviceExplorer::Host::HostIdentity Second;
	Check(Store.LoadAndAdvance(Second, Error), "existing identity advances");
	Check(Second.NodeId == First.NodeId, "node id is stable");
	Check(Second.HostSession == 2, "host session increments");
	Check(Second.InstanceId != First.InstanceId, "instance id changes per process");

	Check(Store.AdvancePast(41, Second, Error), "peer-known session can advance the identity");
	Check(Second.HostSession == 42, "known session advances past the peer value");
	DeviceExplorer::Host::HostIdentity Third;
	Check(Store.LoadAndAdvance(Third, Error), "corrected identity remains readable");
	Check(Third.NodeId == First.NodeId && Third.HostSession == 43, "corrected session remains monotonic");

	{
		std::ofstream Corrupt(Store.IdentityPath(), std::ios::binary | std::ios::trunc);
		Corrupt << "{broken";
	}
	DeviceExplorer::Host::HostIdentity Recovered;
	Check(Store.LoadAndAdvance(Recovered, Error), "corrupt identity is replaced");
	Check(Recovered.NodeId != First.NodeId, "corrupt identity receives a new node id");
	Check(Recovered.HostSession == 1, "replacement identity restarts its own session namespace");

	for (const std::filesystem::directory_entry& Entry : std::filesystem::directory_iterator(Directory.Path))
	{
		Check(Entry.path().filename() == "identity.json", "atomic writer leaves no temporary files");
	}

	if (Failures == 0) std::cout << "DeviceExplorer host identity tests passed\n";
	return Failures == 0 ? 0 : 1;
}
