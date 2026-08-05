#include "DeviceExplorerHostCore.h"

#include "DeviceExplorerHostManifest.h"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

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

void PrintUsage()
{
	std::cout
		<< "Usage: dexp-host [--dashboard-address ADDRESS] [--dashboard-port PORT]\n"
		   "                 [--device-address ADDRESS] [--device-port PORT]\n"
		   "                 [--build-id ID] [--version-json]\n";
}
}    // namespace

int main(const int ArgCount, char** ArgValues)
{
	DeviceExplorer::Host::HostConfig Config;
	Config.BuildId = DEVICEEXPLORER_BUILD_ID;
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
		std::cerr << "Invalid argument: " << Argument << '\n';
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

	Config.Log = [](const DeviceExplorer::Host::LogLevel Level, const std::string& Message)
	{
		std::ostream& Stream = Level == DeviceExplorer::Host::LogLevel::Error ? std::cerr : std::cout;
		Stream << Message << '\n';
	};
	DeviceExplorer::Host::HostCore Host(std::move(Config));
	std::string Error;
	if (!Host.Start(Error))
	{
		std::cerr << Error << '\n';
		return 1;
	}

	std::signal(SIGINT, HandleSignal);
	std::signal(SIGTERM, HandleSignal);
	while (!StopRequested)
	{
		Host.RunFor(std::chrono::milliseconds(250));
	}
	Host.Stop();
	return 0;
}
