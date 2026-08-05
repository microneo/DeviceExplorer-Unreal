#include "DeviceExplorerHostCore.h"

#include "DeviceExplorerHostManifest.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>

#if __has_include("DeviceExplorerBuildId.h")
#include "DeviceExplorerBuildId.h"
#endif

#if defined(_WIN32)
#include <Windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifndef DEVICEEXPLORER_BUILD_ID
#define DEVICEEXPLORER_BUILD_ID "unknown"
#endif

namespace
{
volatile std::sig_atomic_t StopRequested = 0;

void HandleSignal(int)
{
	StopRequested = 1;
}

bool ParsePort(const std::string_view Text, std::uint16_t& OutPort)
{
	unsigned Parsed = 0;
	const std::from_chars_result Result = std::from_chars(Text.data(), Text.data() + Text.size(), Parsed);
	if (Result.ec != std::errc{} || Result.ptr != Text.data() + Text.size() || Parsed > 65535) return false;
	OutPort = static_cast<std::uint16_t>(Parsed);
	return true;
}

bool ReadValue(int& Index, const int Count, char** Values, std::string_view& OutValue)
{
	if (Index + 1 >= Count) return false;
	OutValue = Values[++Index];
	return true;
}

std::string RandomToken()
{
	std::random_device Random;
	static constexpr char Hex[] = "0123456789abcdef";
	std::string Result(64, '\0');
	for (char& Character : Result) Character = Hex[Random() & 0x0F];
	return Result;
}

bool ParseProcessId(const std::string_view Text, std::uint64_t& OutProcessId)
{
	const std::from_chars_result Result = std::from_chars(Text.data(), Text.data() + Text.size(), OutProcessId);
	return Result.ec == std::errc{} && Result.ptr == Text.data() + Text.size();
}

bool ParentIsRunning(const std::uint64_t ProcessId)
{
	if (ProcessId == 0) return true;
#if defined(_WIN32)
	const HANDLE Process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(ProcessId));
	if (Process == nullptr) return false;
	DWORD ExitCode = 0;
	const bool Running = GetExitCodeProcess(Process, &ExitCode) && ExitCode == STILL_ACTIVE;
	CloseHandle(Process);
	return Running;
#else
	if (ProcessId > static_cast<std::uint64_t>(std::numeric_limits<pid_t>::max())) return false;
	return kill(static_cast<pid_t>(ProcessId), 0) == 0 || errno == EPERM;
#endif
}

bool SplitUnrealArgument(const std::string_view Argument, const std::string_view Name, std::string_view& OutValue)
{
	if (Argument.size() <= Name.size() + 2 || Argument.front() != '-' || Argument.substr(1, Name.size()) != Name ||
	    Argument[Name.size() + 1] != '=') return false;
	OutValue = Argument.substr(Name.size() + 2);
	return true;
}

std::filesystem::path DefaultStateDirectory()
{
#if defined(_WIN32)
	const char* Base = std::getenv("LOCALAPPDATA");
	return Base && *Base ? std::filesystem::path(Base) / "DeviceExplorer" / "Host" : std::filesystem::path{};
#elif defined(__APPLE__)
	const char* Home = std::getenv("HOME");
	return Home && *Home ? std::filesystem::path(Home) / "Library" / "Application Support" / "DeviceExplorer" / "Host" : std::filesystem::path{};
#else
	if (const char* StateHome = std::getenv("XDG_STATE_HOME"); StateHome && *StateHome)
	{
		return std::filesystem::path(StateHome) / "DeviceExplorer" / "Host";
	}
	const char* Home = std::getenv("HOME");
	return Home && *Home ? std::filesystem::path(Home) / ".local" / "state" / "DeviceExplorer" / "Host" : std::filesystem::path{};
#endif
}

class HostLock
{
public:
	~HostLock()
	{
#if defined(_WIN32)
		if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
#else
		if (Descriptor >= 0) close(Descriptor);
#endif
	}

	bool Acquire(const std::filesystem::path& StateDirectory, std::string& OutError)
	{
		if (StateDirectory.empty())
		{
			OutError = "cannot determine the per-user state directory";
			return false;
		}
		std::error_code Error;
		std::filesystem::create_directories(StateDirectory, Error);
		if (Error)
		{
			OutError = "cannot create the per-user state directory: " + Error.message();
			return false;
		}
		const std::filesystem::path LockPath = StateDirectory / "host.lock";
#if defined(_WIN32)
		Handle = CreateFileW(LockPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
		                     FILE_ATTRIBUTE_NORMAL, nullptr);
		if (Handle == INVALID_HANDLE_VALUE)
#else
		Descriptor = open(LockPath.c_str(), O_RDWR | O_CREAT, 0600);
		if (Descriptor < 0 || flock(Descriptor, LOCK_EX | LOCK_NB) != 0)
#endif
		{
			OutError = "another DeviceExplorer host is already running for this user";
			return false;
		}
		return true;
	}

private:
#if defined(_WIN32)
	HANDLE Handle = INVALID_HANDLE_VALUE;
#else
	int Descriptor = -1;
#endif
};

void PrintUsage()
{
	std::cout
		<< "Usage: dexp-host [--dashboard-address ADDRESS] [--dashboard-port PORT]\n"
		   "                 [--device-address ADDRESS] [--device-port PORT]\n"
		   "                 [--trace-port PORT] [--token TOKEN] [--web-root PATH]\n"
		   "                 [--transfer-dir PATH]\n"
		   "                 [--parent-pid PID] [--state-dir PATH] [--build-id ID]\n"
		   "                 [--version-json]\n";
}
}    // namespace

int main(const int ArgCount, char** ArgValues)
{
	DeviceExplorer::Host::HostConfig Config;
	Config.BuildId = DEVICEEXPLORER_BUILD_ID;
	Config.Token = RandomToken();
	std::uint64_t ParentProcessId = 0;
	std::filesystem::path StateDirectory = DefaultStateDirectory();
	bool VersionJson = false;
	for (int Index = 1; Index < ArgCount; ++Index)
	{
		const std::string_view Argument = ArgValues[Index];
		std::string_view Value;
		if (Argument == "--help")
		{
			PrintUsage();
			return 0;
		}
		if (Argument == "--version-json" || Argument == "-VersionJson")
		{
			VersionJson = true;
			continue;
		}
		if (Argument == "--dashboard-address" && ReadValue(Index, ArgCount, ArgValues, Value))
		{
			Config.DashboardAddress = Value;
			continue;
		}
		if (Argument == "--device-address" && ReadValue(Index, ArgCount, ArgValues, Value))
		{
			Config.DeviceAddress = Value;
			continue;
		}
		if (Argument == "--build-id" && ReadValue(Index, ArgCount, ArgValues, Value))
		{
			Config.BuildId = Value;
			continue;
		}
		if (Argument == "--token" && ReadValue(Index, ArgCount, ArgValues, Value))
		{
			Config.Token = Value;
			continue;
		}
		if (Argument == "--web-root" && ReadValue(Index, ArgCount, ArgValues, Value))
		{
			Config.WebRoot = Value;
			continue;
		}
		if (Argument == "--transfer-dir" && ReadValue(Index, ArgCount, ArgValues, Value))
		{
			Config.TransferDirectory = Value;
			continue;
		}
		if (Argument == "--parent-pid" && ReadValue(Index, ArgCount, ArgValues, Value) && ParseProcessId(Value, ParentProcessId))
		{
			continue;
		}
		if (Argument == "--state-dir" && ReadValue(Index, ArgCount, ArgValues, Value))
		{
			StateDirectory = Value;
			continue;
		}
		if (Argument == "--dashboard-port" && ReadValue(Index, ArgCount, ArgValues, Value) &&
		    ParsePort(Value, Config.DashboardPort))
		{
			continue;
		}
		if (Argument == "--device-port" && ReadValue(Index, ArgCount, ArgValues, Value) &&
		    ParsePort(Value, Config.DevicePort))
		{
			continue;
		}
		if (Argument == "--trace-port" && ReadValue(Index, ArgCount, ArgValues, Value) &&
		    ParsePort(Value, Config.TracePort))
		{
			continue;
		}

		if (SplitUnrealArgument(Argument, "DashboardPort", Value) && ParsePort(Value, Config.DashboardPort)) continue;
		if (SplitUnrealArgument(Argument, "DevicePort", Value) && ParsePort(Value, Config.DevicePort)) continue;
		if (SplitUnrealArgument(Argument, "TracePort", Value) && ParsePort(Value, Config.TracePort)) continue;
		if (SplitUnrealArgument(Argument, "ParentPID", Value) && ParseProcessId(Value, ParentProcessId)) continue;
		if (SplitUnrealArgument(Argument, "Token", Value)) { Config.Token = Value; continue; }
		if (SplitUnrealArgument(Argument, "WebRoot", Value)) { Config.WebRoot = Value; continue; }
		if (SplitUnrealArgument(Argument, "TransferDir", Value)) { Config.TransferDirectory = Value; continue; }
		if (SplitUnrealArgument(Argument, "StateDir", Value)) { StateDirectory = Value; continue; }

		std::cerr << "Invalid argument or value: " << Argument;
		if (!Value.empty()) std::cerr << " " << Value;
		std::cerr << '\n';
		PrintUsage();
		return 2;
	}

	if (VersionJson)
	{
		DeviceExplorer::Wire::HostManifest Manifest;
		Manifest.BuildId = Config.BuildId.empty() ? "unknown" : Config.BuildId;
		std::string Json;
		if (!DeviceExplorer::Wire::SerializeHostManifest(Manifest, Json)) return 1;
		std::cout << Json << '\n';
		return 0;
	}

	std::cout << std::unitbuf;
	std::cerr << std::unitbuf;
	HostLock Lock;
	std::string Error;
	if (!Lock.Acquire(StateDirectory, Error))
	{
		std::cerr << Error << '\n';
		return 1;
	}
	Config.Log = [](const DeviceExplorer::Host::LogLevel Level, const std::string& Message)
	{
		std::ostream& Stream = Level == DeviceExplorer::Host::LogLevel::Error ? std::cerr : std::cout;
		Stream << Message << '\n';
	};
	DeviceExplorer::Host::HostCore Host(std::move(Config));
	if (!Host.Start(Error))
	{
		std::cerr << Error << '\n';
		return 1;
	}

	std::signal(SIGINT, HandleSignal);
	std::signal(SIGTERM, HandleSignal);
	while (!StopRequested && ParentIsRunning(ParentProcessId))
	{
		Host.RunFor(std::chrono::milliseconds(250));
	}
	Host.Stop();
	return 0;
}
