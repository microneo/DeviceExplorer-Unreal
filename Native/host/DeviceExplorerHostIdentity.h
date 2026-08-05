#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace DeviceExplorer::Host
{
struct HostIdentity
{
	std::string NodeId;
	std::uint64_t HostSession = 0;
	std::string InstanceId;
};

// The caller must hold the per-user host lock. A session is persisted before it
// is returned, so callers can safely publish it immediately after this call.
class HostIdentityStore
{
public:
	explicit HostIdentityStore(std::filesystem::path StateDirectory);

	bool LoadAndAdvance(HostIdentity& OutIdentity, std::string& OutError) const;
	bool AdvancePast(std::uint64_t KnownSession, HostIdentity& InOutIdentity, std::string& OutError) const;

	std::filesystem::path IdentityPath() const;

private:
	bool Persist(const HostIdentity& Identity, std::string& OutError) const;

	std::filesystem::path StateDirectory;
};
}    // namespace DeviceExplorer::Host
