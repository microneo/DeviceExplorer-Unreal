#include "DeviceExplorerHostCore.h"
#include "DeviceExplorerHostPeerNetwork.h"

#include "DeviceExplorerHostManifest.h"
#include "DeviceExplorerAuthPrimitives.h"
#include "DeviceExplorerJson.h"
#include "DeviceExplorerProtocol.h"
#include "DeviceExplorerWebSocket.h"

#include <asio.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace
{
int Failures = 0;

void Check(const bool Condition, const char* Expression, const int Line)
{
	if (!Condition)
	{
		std::cerr << "line " << Line << ": check failed: " << Expression << '\n';
		++Failures;
	}
}

#define CHECK(Expression) Check((Expression), #Expression, __LINE__)

std::string Get(const std::string& Address, const std::uint16_t Port, const std::string& Path)
{
	asio::io_context Io;
	asio::ip::tcp::socket Socket(Io);
	asio::error_code Error;
	Socket.connect({ asio::ip::make_address(Address, Error), Port }, Error);
	if (Error) return {};
	const std::string Request =
		"GET " + Path + " HTTP/1.1\r\nHost: " + Address + ':' + std::to_string(Port) +
		"\r\nConnection: close\r\n\r\n";
	asio::write(Socket, asio::buffer(Request), Error);
	if (Error) return {};
	std::string Response;
	std::array<char, 4096> Buffer{};
	for (;;)
	{
		const std::size_t Read = Socket.read_some(asio::buffer(Buffer), Error);
		Response.append(Buffer.data(), Read);
		if (Error == asio::error::eof) break;
		if (Error) return {};
	}
	return Response;
}

std::string Body(const std::string& Response)
{
	const std::size_t Separator = Response.find("\r\n\r\n");
	return Separator == std::string::npos ? std::string{} : Response.substr(Separator + 4);
}

std::string JsonStringMember(const std::string& Text, const std::string_view Name)
{
	DeviceExplorer::Wire::JsonValue Root;
	if (!DeviceExplorer::Wire::ParseJson(
		    { reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }, Root)) return {};
	const DeviceExplorer::Wire::JsonValue* Value = Root.FindMember(Name);
	const std::string* String = Value == nullptr ? nullptr : Value->TryGetString();
	return String == nullptr ? std::string{} : *String;
}

class DeviceClient
{
public:
	explicit DeviceClient(asio::io_context& Io)
		: Socket(Io), Decoder(DeviceExplorer::Wire::WebSocketRole::Client)
	{
	}

	bool Connect(const std::string& Address, const std::uint16_t Port, const std::string& Token,
	             const std::uint64_t DeviceSession, std::string& OutAttachAck)
	{
		asio::error_code Error;
		Socket.connect({ asio::ip::make_address(Address, Error), Port }, Error);
		if (Error) return false;
		const std::string Request =
			"GET /device/connect HTTP/1.1\r\nHost: " + Address + ':' + std::to_string(Port) +
			"\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
			"Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
		asio::write(Socket, asio::buffer(Request), Error);
		if (Error) return false;
		std::string Header;
		asio::read_until(Socket, asio::dynamic_buffer(Header), "\r\n\r\n", Error);
		if (Error || Header.find("HTTP/1.1 101 Switching Protocols") != 0) return false;

		const std::string ClientNonce = "0123456789abcdef0123456789abcdef";
		if (!Send("{\"type\":\"auth_request\",\"protocol_version\":" +
		          std::to_string(DeviceExplorer::DeviceProtocolVersion) + ",\"client_nonce\":\"" + ClientNonce + "\"}")) return false;
		const std::string Challenge = Receive();
		const std::string HostNonce = JsonStringMember(Challenge, "host_nonce");
		if (!DeviceExplorer::Wire::Auth::IsValidNonce(HostNonce)) return false;
		const std::string Proof = DeviceExplorer::Wire::Auth::ComputeProof(
			Token, DeviceExplorer::Wire::Auth::DeviceProofLabel, ClientNonce, HostNonce);
		if (!Send("{\"type\":\"auth_response\",\"client_proof\":\"" + Proof + "\"}")) return false;
		if (Receive().find("\"type\":\"auth_ok\"") == std::string::npos) return false;
		const std::string Hello =
			"{\"type\":\"hello\",\"device_id\":\"aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee\","
			"\"device_session\":\"" + std::to_string(DeviceSession) +
			"\",\"connection_id\":\"connection-" + std::to_string(DeviceSession) +
			"\",\"name\":\"Roster device\",\"project_name\":\"Integration\","
			"\"engine_version\":\"5.test\",\"platform\":\"Test\",\"configuration\":\"Development\","
			"\"build_version\":\"test\",\"protocol_version\":" +
			std::to_string(DeviceExplorer::DeviceProtocolVersion) +
			",\"uptime_seconds\":1,\"capabilities\":[\"logs\"],\"commands\":[],\"file_roots\":[],\"data_modules\":[]}";
		if (!Send(Hello)) return false;
		OutAttachAck = Receive();
		return !OutAttachAck.empty();
	}

	void Close()
	{
		asio::error_code Error;
		Socket.close(Error);
	}

private:
	bool Send(const std::string& Text)
	{
		DeviceExplorer::Wire::WebSocketFrame Frame;
		Frame.Opcode = DeviceExplorer::Wire::WebSocketOpcode::Text;
		Frame.Payload.assign(Text.begin(), Text.end());
		std::vector<std::uint8_t> Bytes;
		if (!DeviceExplorer::Wire::EncodeWebSocketFrame(
			    Frame, DeviceExplorer::Wire::WebSocketRole::Client, NextMask++, Bytes)) return false;
		asio::error_code Error;
		asio::write(Socket, asio::buffer(Bytes), Error);
		return !Error;
	}

	std::string Receive()
	{
		for (;;)
		{
			DeviceExplorer::Wire::WebSocketFrame Frame;
			if (Decoder.Drain(Frame))
			{
				if (Frame.Opcode == DeviceExplorer::Wire::WebSocketOpcode::Text)
					return { reinterpret_cast<const char*>(Frame.Payload.data()), Frame.Payload.size() };
				if (Frame.Opcode == DeviceExplorer::Wire::WebSocketOpcode::Close) return {};
			}
			std::array<std::uint8_t, 4096> Buffer{};
			asio::error_code Error;
			const std::size_t Bytes = Socket.read_some(asio::buffer(Buffer), Error);
			if (Error || Bytes == 0 || !Decoder.Consume({ Buffer.data(), Bytes })) return {};
		}
	}

	asio::ip::tcp::socket Socket;
	DeviceExplorer::Wire::WebSocketDecoder Decoder;
	std::uint32_t NextMask = 0x12345678;
};
}    // namespace

int main()
{
	DeviceExplorer::Host::HostConfig Config;
	Config.DashboardPort = 0;
	Config.DeviceAddress = "127.0.0.1";
	Config.DevicePort = 0;
	Config.Token = "integration-test-token-that-is-long-enough";
	Config.BuildId = "integration-test";
	Config.NodeId = "11111111-1111-4111-8111-111111111111";
	Config.HostSession = 7;
	Config.InstanceId = "22222222-2222-4222-8222-222222222222";
	Config.EnableDistributedMode = true;
	Config.ClusterId = "integration-cluster";
	Config.PeerSecret = "integration-peer-secret-shared-by-all-hosts";
	Config.PeerAddress = "127.0.0.1";
	Config.PeerPort = 0;
	Config.PeerRosterRemovalTimeout = std::chrono::milliseconds(100);
	DeviceExplorer::Host::HostCore Host(std::move(Config));
	std::string Error;
	CHECK(Host.Start(Error));
	const DeviceExplorer::Host::BoundEndpoints Endpoints = Host.GetBoundEndpoints();
	CHECK(Endpoints.DashboardPort != 0);
	CHECK(Endpoints.DevicePort != 0);
	CHECK(Endpoints.DashboardPort != Endpoints.DevicePort);
	CHECK(Endpoints.PeerPort != 0);

	DeviceExplorer::Host::HostConfig PeerConfig;
	PeerConfig.DashboardPort = 0;
	PeerConfig.DeviceAddress = "127.0.0.1";
	PeerConfig.DevicePort = 0;
	PeerConfig.Token = "second-integration-test-token-that-is-long-enough";
	PeerConfig.BuildId = "integration-test-peer";
	PeerConfig.NodeId = "33333333-3333-4333-8333-333333333333";
	PeerConfig.HostSession = 3;
	PeerConfig.InstanceId = "44444444-4444-4444-8444-444444444444";
	PeerConfig.EnableDistributedMode = true;
	PeerConfig.ClusterId = "integration-cluster";
	PeerConfig.PeerSecret = "integration-peer-secret-shared-by-all-hosts";
	PeerConfig.PeerAddress = "127.0.0.1";
	PeerConfig.PeerPort = 0;
	PeerConfig.PeerPingInterval = std::chrono::seconds(1);
	PeerConfig.PeerSuspectTimeout = std::chrono::seconds(3);
	PeerConfig.PeerSeeds.push_back({ "127.0.0.1", Endpoints.PeerPort });
	DeviceExplorer::Host::HostCore PeerHost(std::move(PeerConfig));
	CHECK(PeerHost.Start(Error));

	std::atomic<bool> Done{ false };
	std::thread EventLoop(
		[&Host, &PeerHost, &Done]
		{
			while (!Done.load())
			{
				Host.RunFor(std::chrono::milliseconds(10));
				PeerHost.RunFor(std::chrono::milliseconds(10));
			}
		});

	bool PeersConnected = false;
	const DeviceExplorer::Host::BoundEndpoints PeerEndpoints = PeerHost.GetBoundEndpoints();
	for (int Attempt = 0; Attempt < 200; ++Attempt)
	{
		const std::string DirectDiagnostics = Host.GetPeerDiagnosticsJson();
		CHECK(!DirectDiagnostics.empty());
		if (Body(Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/api/peers")).find("\"peer_count\":1") != std::string::npos &&
		    Body(Get(PeerEndpoints.DashboardAddress, PeerEndpoints.DashboardPort, "/api/peers")).find("\"peer_count\":1") != std::string::npos)
		{
			PeersConnected = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(PeersConnected);
	CHECK(Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/api/roster").find("HTTP/1.1 200 OK") == 0);

	asio::io_context DeviceIo;
	DeviceClient FirstDevice(DeviceIo);
	std::string AttachAck;
	CHECK(FirstDevice.Connect(Endpoints.DeviceAddress, Endpoints.DevicePort,
	                          "integration-test-token-that-is-long-enough", 10, AttachAck));
	CHECK(AttachAck.find("\"accepted\":true") != std::string::npos);
	bool FirstRosterConverged = false;
	for (int Attempt = 0; Attempt < 200; ++Attempt)
	{
		const std::string Roster = Body(Get(PeerEndpoints.DashboardAddress, PeerEndpoints.DashboardPort, "/api/roster"));
		if (Roster.find("\"device_session\":10") != std::string::npos &&
		    Roster.find("\"owner_node_id\":\"11111111-1111-4111-8111-111111111111\"") != std::string::npos)
		{
			FirstRosterConverged = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(FirstRosterConverged);

	DeviceClient MigratedDevice(DeviceIo);
	CHECK(MigratedDevice.Connect(PeerEndpoints.DeviceAddress, PeerEndpoints.DevicePort,
	                             "second-integration-test-token-that-is-long-enough", 11, AttachAck));
	CHECK(AttachAck.find("\"accepted\":true") != std::string::npos);
	bool MigrationConverged = false;
	for (int Attempt = 0; Attempt < 200; ++Attempt)
	{
		const std::string Roster = Body(Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/api/roster"));
		if (Roster.find("\"device_session\":11") != std::string::npos &&
		    Roster.find("\"owner_node_id\":\"33333333-3333-4333-8333-333333333333\"") != std::string::npos &&
		    Roster.find("\"ambiguous_owners\":0") != std::string::npos)
		{
			MigrationConverged = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(MigrationConverged);

	DeviceClient RolledBackDevice(DeviceIo);
	CHECK(RolledBackDevice.Connect(Endpoints.DeviceAddress, Endpoints.DevicePort,
	                               "integration-test-token-that-is-long-enough", 10, AttachAck));
	CHECK(AttachAck.find("\"accepted\":false") != std::string::npos);
	CHECK(AttachAck.find("\"last_known_device_session\":\"11\"") != std::string::npos);
	RolledBackDevice.Close();
	FirstDevice.Close();

	const std::string DashboardHealth = Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/health");
	const std::string DeviceHealth = Get(Endpoints.DeviceAddress, Endpoints.DevicePort, "/health");
	CHECK(DashboardHealth.find("HTTP/1.1 200 OK") == 0);
	CHECK(Body(DashboardHealth).find("\"status\":\"ok\"") != std::string::npos);
	CHECK(Body(DashboardHealth).find("\"node_id\":\"11111111-1111-4111-8111-111111111111\"") != std::string::npos);
	CHECK(Body(DashboardHealth).find("\"host_session\":7") != std::string::npos);
	CHECK(DeviceHealth.find("HTTP/1.1 200 OK") == 0);

	const std::string ManifestResponse = Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/host-manifest");
	const std::string ManifestBody = Body(ManifestResponse);
	DeviceExplorer::Wire::HostManifest Manifest;
	CHECK(DeviceExplorer::Wire::ParseHostManifest(
		{ reinterpret_cast<const std::uint8_t*>(ManifestBody.data()), ManifestBody.size() }, Manifest));
	CHECK(Manifest.BuildId == "integration-test");

	const std::string ConfigResponse = Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/api/config");
	CHECK(ConfigResponse.find("HTTP/1.1 200 OK") == 0);
	CHECK(Body(ConfigResponse).find("\"device_port\":" + std::to_string(Endpoints.DevicePort)) != std::string::npos);
	CHECK(Body(ConfigResponse).find("\"instance_id\":\"22222222-2222-4222-8222-222222222222\"") != std::string::npos);
	CHECK(Body(ConfigResponse).find("\"peer_protocol_version\":" +
	                               std::to_string(DeviceExplorer::PeerProtocolVersion)) != std::string::npos);
	CHECK(Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/api/peers").find("HTTP/1.1 200 OK") == 0);
	CHECK(Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/missing").find("HTTP/1.1 404 Not Found") == 0);
	CHECK(Get(Endpoints.DeviceAddress, Endpoints.DevicePort, "/device/connect").find("HTTP/1.1 400 Bad Request") == 0);

	Done.store(true);
	EventLoop.join();
	PeerHost.Stop();
	Host.RunFor(std::chrono::milliseconds(250));
	CHECK(Host.GetPeerDiagnosticsJson().find("\"expired_roster_owners\":1") != std::string::npos);

	DeviceExplorer::Host::HostConfig RollbackConfig;
	RollbackConfig.DashboardPort = 0;
	RollbackConfig.DeviceAddress = "127.0.0.1";
	RollbackConfig.DevicePort = 0;
	RollbackConfig.Token = "rollback-integration-test-token-that-is-long-enough";
	RollbackConfig.NodeId = "33333333-3333-4333-8333-333333333333";
	RollbackConfig.HostSession = 1;
	RollbackConfig.InstanceId = "55555555-5555-4555-8555-555555555555";
	RollbackConfig.EnableDistributedMode = true;
	RollbackConfig.ClusterId = "integration-cluster";
	RollbackConfig.PeerSecret = "integration-peer-secret-shared-by-all-hosts";
	RollbackConfig.PeerAddress = "127.0.0.1";
	RollbackConfig.PeerPort = 0;
	RollbackConfig.PeerSeeds.push_back({ "127.0.0.1", Endpoints.PeerPort });
	std::atomic<std::uint64_t> CorrectedPast{ 0 };
	RollbackConfig.ApplyHostSessionCorrection = [&CorrectedPast](const std::uint64_t Known,
	                                                           std::uint64_t& OutSession,
	                                                           std::string& OutError)
	{
		CorrectedPast.store(Known);
		OutSession = Known + 1;
		OutError.clear();
		return true;
	};
	DeviceExplorer::Host::HostCore RollbackHost(std::move(RollbackConfig));
	CHECK(RollbackHost.Start(Error));
	const DeviceExplorer::Host::BoundEndpoints RollbackEndpoints = RollbackHost.GetBoundEndpoints();
	Done.store(false);
	std::thread CorrectionLoop(
		[&Host, &RollbackHost, &Done]
		{
			while (!Done.load())
			{
				Host.RunFor(std::chrono::milliseconds(10));
				RollbackHost.RunFor(std::chrono::milliseconds(10));
			}
		});
	bool CorrectedAndReconnected = false;
	for (int Attempt = 0; Attempt < 500; ++Attempt)
	{
		const std::string Peers = Body(Get(RollbackEndpoints.DashboardAddress, RollbackEndpoints.DashboardPort, "/api/peers"));
		const std::string Health = Body(Get(RollbackEndpoints.DashboardAddress, RollbackEndpoints.DashboardPort, "/health"));
		if (CorrectedPast.load() == 3 && Peers.find("\"peer_count\":1") != std::string::npos &&
		    Health.find("\"host_session\":4") != std::string::npos)
		{
			CorrectedAndReconnected = true;
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	CHECK(CorrectedAndReconnected);
	Done.store(true);
	CorrectionLoop.join();
	RollbackHost.Stop();

	DeviceExplorer::Host::HostConfig AttackerConfig;
	AttackerConfig.DashboardPort = 0;
	AttackerConfig.DeviceAddress = "127.0.0.1";
	AttackerConfig.DevicePort = 0;
	AttackerConfig.Token = "attacker-device-token-that-is-long-enough";
	AttackerConfig.NodeId = "77777777-7777-4777-8777-777777777777";
	AttackerConfig.HostSession = 1000000;
	AttackerConfig.InstanceId = "88888888-8888-4888-8888-888888888888";
	AttackerConfig.EnableDistributedMode = true;
	AttackerConfig.ClusterId = "integration-cluster";
	AttackerConfig.PeerSecret = "wrong-peer-secret-that-is-still-long-enough";
	AttackerConfig.PeerAddress = "127.0.0.1";
	AttackerConfig.PeerPort = 0;
	AttackerConfig.PeerSeeds.push_back({ "127.0.0.1", Endpoints.PeerPort });
	DeviceExplorer::Host::HostCore AttackerHost(std::move(AttackerConfig));
	CHECK(AttackerHost.Start(Error));
	const DeviceExplorer::Host::BoundEndpoints AttackerEndpoints = AttackerHost.GetBoundEndpoints();
	Done.store(false);
	std::thread AttackLoop(
		[&Host, &AttackerHost, &Done]
		{
			while (!Done.load())
			{
				Host.RunFor(std::chrono::milliseconds(10));
				AttackerHost.RunFor(std::chrono::milliseconds(10));
			}
		});
	std::this_thread::sleep_for(std::chrono::seconds(1));
	const std::string HostPeersAfterAttack = Body(Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/api/peers"));
	const std::string AttackerPeers = Body(Get(AttackerEndpoints.DashboardAddress, AttackerEndpoints.DashboardPort, "/api/peers"));
	CHECK(HostPeersAfterAttack.find("\"refused_authentications\":0") == std::string::npos);
	CHECK(AttackerPeers.find("\"peer_count\":0") != std::string::npos);
	CHECK(Body(Get(Endpoints.DashboardAddress, Endpoints.DashboardPort, "/health")).find("\"host_session\":7") != std::string::npos);
	Done.store(true);
	AttackLoop.join();
	AttackerHost.Stop();

	// Stop with an accepted but silent peer handshake, then reuse the same
	// io_context. Cancellation handlers from the old network must be drained
	// before the replacement network starts.
	asio::io_context SilentIo;
	asio::ip::tcp::socket SilentPeer(SilentIo);
	asio::error_code SilentError;
	SilentPeer.connect({ asio::ip::make_address(Endpoints.PeerAddress, SilentError), Endpoints.PeerPort }, SilentError);
	CHECK(!SilentError);
	Host.RunFor(std::chrono::milliseconds(10));
	Host.Stop();
	SilentPeer.close(SilentError);
	CHECK(Host.Start(Error));
	Host.RunFor(std::chrono::milliseconds(10));
	Host.Stop();

	DeviceExplorer::Host::HostConfig UnsafeConfig;
	UnsafeConfig.DashboardAddress = "0.0.0.0";
	UnsafeConfig.DashboardPort = 0;
	UnsafeConfig.DevicePort = 0;
	UnsafeConfig.Token = "integration-test-token-that-is-long-enough";
	UnsafeConfig.NodeId = "11111111-1111-4111-8111-111111111111";
	UnsafeConfig.HostSession = 7;
	UnsafeConfig.InstanceId = "22222222-2222-4222-8222-222222222222";
	DeviceExplorer::Host::HostCore UnsafeHost(std::move(UnsafeConfig));
	CHECK(!UnsafeHost.Start(Error));
	CHECK(Error.find("loopback") != std::string::npos);

	DeviceExplorer::Host::HostConfig UnauthenticatedPeerConfig;
	UnauthenticatedPeerConfig.DashboardPort = 0;
	UnauthenticatedPeerConfig.DeviceAddress = "127.0.0.1";
	UnauthenticatedPeerConfig.DevicePort = 0;
	UnauthenticatedPeerConfig.Token = "integration-test-token-that-is-long-enough";
	UnauthenticatedPeerConfig.NodeId = "99999999-9999-4999-8999-999999999999";
	UnauthenticatedPeerConfig.HostSession = 1;
	UnauthenticatedPeerConfig.InstanceId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
	UnauthenticatedPeerConfig.EnableDistributedMode = true;
	UnauthenticatedPeerConfig.ClusterId = "integration-cluster";
	UnauthenticatedPeerConfig.PeerAddress = "127.0.0.1";
	DeviceExplorer::Host::HostCore UnauthenticatedPeerHost(std::move(UnauthenticatedPeerConfig));
	CHECK(!UnauthenticatedPeerHost.Start(Error));
	CHECK(Error.find("peer secret") != std::string::npos);

	asio::io_context CandidateIo;
	DeviceExplorer::Host::HostConfig CandidateConfig;
	CandidateConfig.NodeId = "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa";
	CandidateConfig.HostSession = 1;
	CandidateConfig.InstanceId = "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb";
	CandidateConfig.EnableDistributedMode = true;
	CandidateConfig.ClusterId = "candidate-test";
	CandidateConfig.PeerSecret = "candidate-test-peer-secret-that-is-long-enough";
	CandidateConfig.PeerAddress = "127.0.0.1";
	CandidateConfig.PeerCandidateTtl = std::chrono::seconds(0);
	DeviceExplorer::Host::HostPeerNetwork CandidateNetwork(CandidateIo, CandidateConfig);
	CHECK(CandidateNetwork.Start(Error));
	DeviceExplorer::Host::PeerCandidate ExpiringCandidate;
	ExpiringCandidate.ClusterId = "candidate-test";
	ExpiringCandidate.NodeId = "cccccccc-cccc-4ccc-8ccc-cccccccccccc";
	ExpiringCandidate.HostSession = 1;
	ExpiringCandidate.InstanceId = "dddddddd-dddd-4ddd-8ddd-dddddddddddd";
	ExpiringCandidate.Address = "not-an-ip-address";
	ExpiringCandidate.Port = 12345;
	ExpiringCandidate.ProtocolMinimum = DeviceExplorer::PeerProtocolVersion;
	ExpiringCandidate.ProtocolMaximum = DeviceExplorer::PeerProtocolVersion;
	CandidateNetwork.Discover(std::move(ExpiringCandidate));
	CandidateIo.run_for(std::chrono::milliseconds(100));
	const std::string CandidateDiagnostics = CandidateNetwork.DiagnosticsJson();
	CHECK(CandidateDiagnostics.find("\"candidate_count\":0") != std::string::npos);
	CHECK(CandidateDiagnostics.find("\"expired_candidates\":1") != std::string::npos);
	CandidateNetwork.Stop();
	if (Failures != 0)
	{
		std::cerr << Failures << " test(s) failed\n";
		return 1;
	}
	std::cout << "DeviceExplorer native host integration tests passed\n";
	return 0;
}
