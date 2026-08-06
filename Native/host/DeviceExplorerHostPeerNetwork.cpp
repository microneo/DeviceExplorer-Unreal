#include "DeviceExplorerHostPeerNetwork.h"

#include "DeviceExplorerHostPeerState.h"

#include "DeviceExplorerJson.h"
#include "DeviceExplorerPeerProtocol.h"
#include "DeviceExplorerProtocol.h"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace DeviceExplorer::Host
{
namespace
{
using Tcp = asio::ip::tcp;

std::string MakeNonce()
{
	std::random_device Random;
	static constexpr char Hex[] = "0123456789abcdef";
	std::string Result(32, '\0');
	for (char& Character : Result) Character = Hex[Random() & 0x0F];
	return Result;
}

std::string MessageType(const std::string& Text)
{
	Wire::DeviceExplorerMessage Message;
	if (!Wire::ParseDeviceExplorerMessage(
		    { reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }, Message)) return {};
	return Message.Type;
}

void AddString(Wire::JsonValue& Object, std::string Name, std::string Value)
{
	Wire::JsonValue Field;
	(void) Field.SetString(std::move(Value));
	(void) Object.InsertMember(std::move(Name), std::move(Field));
}

void AddUnsigned(Wire::JsonValue& Object, std::string Name, const std::uint64_t Value)
{
	Wire::JsonValue Field;
	Field.SetUnsignedInteger(Value);
	(void) Object.InsertMember(std::move(Name), std::move(Field));
}

void AddBoolean(Wire::JsonValue& Object, std::string Name, const bool Value)
{
	Wire::JsonValue Field;
	Field.SetBoolean(Value);
	(void) Object.InsertMember(std::move(Name), std::move(Field));
}
}    // namespace

struct HostPeerNetwork::Implementation
{
	struct Session;

	struct Candidate : public std::enable_shared_from_this<Candidate>
	{
		Candidate(Implementation& InOwner, PeerSeed InSeed)
			: Owner(InOwner), Seed(std::move(InSeed)), Timer(Owner.Io)
		{
		}

		void Schedule(const bool Immediate)
		{
			if (!Owner.Running || Dialing) return;
			const std::chrono::milliseconds RateDelay = Owner.ReserveDialDelay();
			const std::chrono::milliseconds RetryDelay = Immediate
				? std::chrono::milliseconds(0)
				: std::chrono::duration_cast<std::chrono::milliseconds>(Backoff);
			Timer.expires_after(std::max(RateDelay, RetryDelay));
			auto Self = shared_from_this();
			Timer.async_wait([Self](const asio::error_code& Error)
			{
				if (!Error) Self->Dial();
			});
		}

		void Dial();

		void OnClosed()
		{
			Dialing = false;
			Backoff = std::min(Backoff * 2, std::chrono::seconds(30));
			Schedule(false);
		}

		Implementation& Owner;
		PeerSeed Seed;
		asio::steady_timer Timer;
		std::chrono::seconds Backoff{ 1 };
		bool Dialing = false;
	};

	struct Session : public std::enable_shared_from_this<Session>
	{
		Session(Implementation& InOwner, Tcp::socket InSocket, std::shared_ptr<Candidate> InCandidate, const bool InCountsInboundHandshake)
			: Owner(InOwner), Socket(std::move(InSocket)), Timer(Owner.Io), SourceCandidate(std::move(InCandidate)),
		      LocalNonce(MakeNonce()), LastReceive(std::chrono::steady_clock::now()),
		      CountsInboundHandshake(InCountsInboundHandshake)
		{
		}

		void Start()
		{
			asio::error_code Ignored;
			Socket.set_option(Tcp::no_delay(true), Ignored);
			Socket.set_option(asio::socket_base::send_buffer_size(32 * 1024), Ignored);
			Socket.set_option(asio::socket_base::receive_buffer_size(32 * 1024), Ignored);
			SendHello();
			Read();
			Timer.expires_after(Owner.Config.PeerHandshakeTimeout);
			auto Self = shared_from_this();
			Timer.async_wait([Self](const asio::error_code& Error)
			{
				if (!Error && !Self->Established) Self->Close("handshake timeout");
			});
		}

		void SendHello()
		{
			Wire::PeerHello Hello;
			Hello.ClusterId = Owner.Config.ClusterId;
			Hello.NodeId = Owner.Config.NodeId;
			Hello.HostSession = Owner.Config.LiveHostSession
				? Owner.Config.LiveHostSession->load()
				: Owner.Config.HostSession;
			Hello.InstanceId = Owner.Config.InstanceId;
			Hello.ProtocolMin = PeerProtocolVersion;
			Hello.ProtocolMax = PeerProtocolVersion;
			Hello.ConnectionNonce = LocalNonce;
			std::string Json;
			if (!Wire::SerializePeerHello(Hello, Json))
			{
				Close("cannot serialize peer hello");
				return;
			}
			Send(std::move(Json));
		}

		void Read()
		{
			auto Self = shared_from_this();
			Socket.async_read_some(asio::buffer(ReadBuffer), [Self](const asio::error_code& Error, const std::size_t Bytes)
			{
				if (Error || Bytes == 0)
				{
					Self->Close(Error ? Error.message() : "peer closed");
					return;
				}
				Self->LastReceive = std::chrono::steady_clock::now();
				std::vector<std::string> Messages;
				Wire::PeerProtocolError DecodeError = Wire::PeerProtocolError::None;
				if (!Self->Decoder.Feed({ Self->ReadBuffer.data(), Bytes }, Messages, &DecodeError))
				{
					Self->Close(Wire::PeerProtocolErrorText(DecodeError));
					return;
				}
				for (const std::string& Message : Messages)
				{
					if (!Self->Handle(Message)) return;
				}
				if (!Self->Closed) Self->Read();
			});
		}

		bool Handle(const std::string& Text)
		{
			const std::string Type = MessageType(Text);
			if (Type == "peer_hello") return HandleHello(Text);
			if (Type == "peer_hello_ack") return HandleAck(Text);
			if (!Established)
			{
				Close("message before peer handshake");
				return false;
			}
			if (Type == "peer_ping")
			{
				Send("{\"type\":\"peer_pong\"}");
				return true;
			}
			if (Type == "peer_pong") return true;
			Close("unsupported peer message");
			return false;
		}

		bool HandleHello(const std::string& Text)
		{
			if (ReceivedHello)
			{
				Close("duplicate peer hello");
				return false;
			}
			Wire::PeerProtocolError Error = Wire::PeerProtocolError::None;
			if (!Wire::ParsePeerHello({ reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }, RemoteHello, &Error))
			{
				Close(Wire::PeerProtocolErrorText(Error));
				return false;
			}
			ReceivedHello = true;
			const auto Now = std::chrono::steady_clock::now();
			const std::uint64_t Known = Owner.KnownSessions.Get(RemoteHello.NodeId, Now);
			const std::uint64_t LocalSession = Owner.Config.LiveHostSession
				? Owner.Config.LiveHostSession->load()
				: Owner.Config.HostSession;
			const PeerIdentity Local{ Owner.Config.ClusterId, Owner.Config.NodeId,
			                          LocalSession, Owner.Config.InstanceId };
			const PeerHandshakeDecision Decision = EvaluatePeerHello(
				Local, RemoteHello, Known, PeerProtocolVersion, PeerProtocolVersion);
			std::string Ack;
			if (!Wire::SerializePeerHelloAck(Decision.Ack, Ack))
			{
				Close("cannot serialize peer hello ack");
				return false;
			}
			RemoteAccepted = Decision.Establish;
			if (Decision.Establish) Owner.KnownSessions.Remember(RemoteHello.NodeId, RemoteHello.HostSession, Now);
			else CloseAfterWrite = true;
			Send(std::move(Ack));
			TryEstablish();
			return true;
		}

		bool HandleAck(const std::string& Text)
		{
			if (ReceivedAck)
			{
				Close("duplicate peer hello ack");
				return false;
			}
			Wire::PeerHelloAck Ack;
			Wire::PeerProtocolError Error = Wire::PeerProtocolError::None;
			if (!Wire::ParsePeerHelloAck({ reinterpret_cast<const std::uint8_t*>(Text.data()), Text.size() }, Ack, &Error))
			{
				Close(Wire::PeerProtocolErrorText(Error));
				return false;
			}
			ReceivedAck = true;
			std::uint64_t Corrected = 0;
			const std::uint64_t LocalSession = Owner.Config.LiveHostSession
				? Owner.Config.LiveHostSession->load()
				: Owner.Config.HostSession;
			if (ShouldApplyKnownLocalSession(LocalSession, Ack.KnownHostSession,
			                                Owner.Config.MaximumHostSessionCorrection, Corrected))
			{
				Owner.RecordCorrection(Corrected);
				Close("peer knows a newer local host session");
				return false;
			}
			if (Ack.KnownHostSession > LocalSession && Corrected == 0)
			{
				Owner.RecordAnomalousSession(Ack.KnownHostSession);
			}
			LocalAccepted = Ack.Result == Wire::PeerHelloResult::Accepted && Ack.NegotiatedVersion == PeerProtocolVersion;
			if (!LocalAccepted)
			{
				Close(Ack.Reason.empty() ? "peer rejected handshake" : Ack.Reason);
				return false;
			}
			TryEstablish();
			return true;
		}

		void TryEstablish()
		{
			if (Established || !LocalAccepted || !RemoteAccepted || !ReceivedHello) return;
			if (!Owner.Register(shared_from_this()))
			{
				Close("peer limit or duplicate connection");
				return;
			}
			Established = true;
			Timer.cancel();
			ScheduleKeepalive();
		}

		std::string ConnectionKey() const
		{
			return LocalNonce < RemoteHello.ConnectionNonce
				? LocalNonce + RemoteHello.ConnectionNonce
				: RemoteHello.ConnectionNonce + LocalNonce;
		}

		void ScheduleKeepalive()
		{
			Timer.expires_after(Owner.Config.PeerPingInterval);
			auto Self = shared_from_this();
			Timer.async_wait([Self](const asio::error_code& Error)
			{
				if (Error || Self->Closed || !Self->Established) return;
				if (std::chrono::steady_clock::now() - Self->LastReceive > Self->Owner.Config.PeerSuspectTimeout)
				{
					Self->Close("peer receive timeout");
					return;
				}
				Self->Send("{\"type\":\"peer_ping\"}");
				Self->ScheduleKeepalive();
			});
		}

		void Send(std::string Text)
		{
			std::vector<std::uint8_t> Frame;
			if (!Wire::EncodePeerFrame(Text, Frame))
			{
				Close("cannot frame peer message");
				return;
			}
			if (QueuedBytes > Owner.Config.MaximumQueuedControlBytes ||
			    Frame.size() > Owner.Config.MaximumQueuedControlBytes - QueuedBytes)
			{
				Close("peer control queue is full");
				return;
			}
			QueuedBytes += Frame.size();
			Writes.push_back(std::move(Frame));
			if (Writes.size() == 1) Write();
		}

		void Write()
		{
			if (Writes.empty() || Closed) return;
			auto Self = shared_from_this();
			asio::async_write(Socket, asio::buffer(Writes.front()), [Self](const asio::error_code& Error, std::size_t)
			{
				if (Error)
				{
					Self->Close(Error.message());
					return;
				}
				Self->QueuedBytes -= Self->Writes.front().size();
				Self->Writes.pop_front();
				if (Self->CloseAfterWrite && Self->Writes.empty())
				{
					// The peer answers our hello while we are already rejecting theirs, so
					// closing now would reset a connection with unread bytes on it and the
					// reset would discard the rejection the peer still has to read. Half
					// close instead and let their disconnect, or the handshake timer, end it.
					asio::error_code Ignored;
					Self->Socket.shutdown(Tcp::socket::shutdown_send, Ignored);
					return;
				}
				Self->Write();
			});
		}

		void Close(const std::string& Reason)
		{
			if (Closed) return;
			Closed = true;
			Timer.cancel();
			asio::error_code Ignored;
			Socket.shutdown(Tcp::socket::shutdown_both, Ignored);
			Socket.close(Ignored);
			Owner.Closed(*this, Reason);
			if (SourceCandidate) SourceCandidate->OnClosed();
		}

		Implementation& Owner;
		Tcp::socket Socket;
		asio::steady_timer Timer;
		std::shared_ptr<Candidate> SourceCandidate;
		Wire::PeerFrameDecoder Decoder;
		std::array<std::uint8_t, 16 * 1024> ReadBuffer{};
		std::deque<std::vector<std::uint8_t>> Writes;
		std::size_t QueuedBytes = 0;
		std::string LocalNonce;
		Wire::PeerHello RemoteHello;
		std::chrono::steady_clock::time_point LastReceive;
		bool ReceivedHello = false;
		bool ReceivedAck = false;
		bool RemoteAccepted = false;
		bool LocalAccepted = false;
		bool Established = false;
		bool CloseAfterWrite = false;
		bool Closed = false;
		bool CountsInboundHandshake = false;
	};

	Implementation(asio::io_context& InIo, HostConfig InConfig)
		: Io(InIo), Config(std::move(InConfig)), Acceptor(Io),
	      KnownSessions(Config.MaximumKnownHostSessions, std::chrono::hours(24 * 7))
	{
	}

	void Log(const LogLevel Level, const std::string& Message) const
	{
		if (Config.Log) Config.Log(Level, Message);
	}

	bool Start(std::string& OutError)
	{
		if (Config.ClusterId.empty() || Config.ClusterId.size() > 128)
		{
			OutError = "distributed mode requires a cluster id of at most 128 bytes";
			return false;
		}
		if (Config.MaximumPeers == 0 || Config.MaximumInboundHandshakes == 0 ||
		    Config.MaximumQueuedControlBytes < Wire::MaximumPeerMessageBytes + Wire::PeerFrameHeaderBytes ||
		    Config.MaximumKnownHostSessions == 0 || Config.MaximumCandidateMembers == 0 ||
		    Config.MaximumDialAttemptsPerSecond == 0)
		{
			OutError = "peer limits must be non-zero and hold one maximum-sized control frame";
			return false;
		}
		asio::error_code Error;
		const asio::ip::address Address = asio::ip::make_address(Config.PeerAddress, Error);
		if (Error || !Address.is_v4())
		{
			OutError = "peer address must be IPv4";
			return false;
		}
		const Tcp::endpoint Endpoint(Address, Config.PeerPort);
		Acceptor.open(Endpoint.protocol(), Error);
		if (!Error) Acceptor.set_option(Tcp::acceptor::reuse_address(true), Error);
		if (!Error) Acceptor.bind(Endpoint, Error);
		if (!Error) Acceptor.listen(asio::socket_base::max_listen_connections, Error);
		if (Error)
		{
			OutError = "cannot listen for peers: " + Error.message();
			return false;
		}
		Bound = Acceptor.local_endpoint(Error);
		if (Error)
		{
			OutError = "cannot read peer endpoint: " + Error.message();
			return false;
		}
		Running = true;
		Accept();
		for (const PeerSeed& Seed : Config.PeerSeeds)
		{
			if (Seed.Address.empty() || Seed.Port == 0) continue;
			if (Candidates.size() >= Config.MaximumCandidateMembers)
			{
				++RefusedCandidates;
				break;
			}
			auto Item = std::make_shared<Candidate>(*this, Seed);
			Candidates.push_back(Item);
			Item->Schedule(true);
		}
		OutError.clear();
		return true;
	}

	void Accept()
	{
		Acceptor.async_accept([this](const asio::error_code& Error, Tcp::socket Socket)
		{
			if (!Error)
			{
				if (PendingHandshakes >= Config.MaximumInboundHandshakes)
				{
					asio::error_code Ignored;
					Socket.close(Ignored);
					++RefusedHandshakes;
				}
				else
				{
					++PendingHandshakes;
					StartSession(std::make_shared<Session>(*this, std::move(Socket), nullptr, true));
				}
			}
			if (Running && Acceptor.is_open()) Accept();
		});
	}

	void TrackSession(const std::shared_ptr<Session>& SessionValue)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		Sessions.emplace(SessionValue.get(), SessionValue);
	}

	void StartSession(const std::shared_ptr<Session>& SessionValue)
	{
		TrackSession(SessionValue);
		SessionValue->Start();
	}

	bool Register(const std::shared_ptr<Session>& SessionValue)
	{
		std::shared_ptr<Session> Duplicate;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			const auto Existing = Peers.find(SessionValue->RemoteHello.NodeId);
			if (Existing != Peers.end())
			{
				Duplicate = Existing->second.lock();
				if (Duplicate)
				{
					if (Duplicate->RemoteHello.HostSession > SessionValue->RemoteHello.HostSession) return false;
					if (Duplicate->RemoteHello.HostSession == SessionValue->RemoteHello.HostSession &&
					    Duplicate->ConnectionKey() <= SessionValue->ConnectionKey()) return false;
				}
			}
			else if (Peers.size() >= Config.MaximumPeers)
			{
				++RefusedPeers;
				return false;
			}
			Peers[SessionValue->RemoteHello.NodeId] = SessionValue;
		}
		if (Duplicate) Duplicate->Close("duplicate connection replaced");
		if (SessionValue->CountsInboundHandshake && PendingHandshakes != 0)
		{
			--PendingHandshakes;
			SessionValue->CountsInboundHandshake = false;
		}
		Log(LogLevel::Information, "peer connected: " + SessionValue->RemoteHello.NodeId +
		    " session " + std::to_string(SessionValue->RemoteHello.HostSession));
		return true;
	}

	void Closed(Session& SessionValue, const std::string& Reason)
	{
		if (SessionValue.CountsInboundHandshake && PendingHandshakes != 0)
		{
			--PendingHandshakes;
			SessionValue.CountsInboundHandshake = false;
		}
		if (SessionValue.Established)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			const auto Found = Peers.find(SessionValue.RemoteHello.NodeId);
			if (Found != Peers.end() && Found->second.lock().get() == &SessionValue) Peers.erase(Found);
		}
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Sessions.erase(&SessionValue);
		}
		if (!Reason.empty()) Log(LogLevel::Information, "peer disconnected: " + Reason);
	}

	void RecordCorrection(const std::uint64_t CorrectedSession)
	{
		std::uint64_t Applied = 0;
		std::string Error;
		const std::uint64_t KnownSession = CorrectedSession - 1;
		if (!Config.ApplyHostSessionCorrection ||
		    !Config.ApplyHostSessionCorrection(KnownSession, Applied, Error) || Applied < CorrectedSession)
		{
			{
				std::lock_guard<std::mutex> Lock(Mutex);
				RequiredHostSession = std::max(RequiredHostSession, CorrectedSession);
			}
			Log(LogLevel::Error, "cannot persist corrected host session" +
			    (Error.empty() ? std::string{} : ": " + Error));
			return;
		}
		Config.HostSession = Applied;
		if (Config.LiveHostSession) Config.LiveHostSession->store(Applied);
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			RequiredHostSession = 0;
		}
		Log(LogLevel::Warning, "advanced local host session to " + std::to_string(Applied) +
		    " after peer rollback detection");
		if (Config.RequestMdnsReannounce && *Config.RequestMdnsReannounce)
		{
			(*Config.RequestMdnsReannounce)();
		}
		std::vector<std::shared_ptr<Session>> ToClose;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			for (const auto& Pair : Peers)
			{
				if (const std::shared_ptr<Session> Value = Pair.second.lock()) ToClose.push_back(Value);
			}
		}
		for (const std::shared_ptr<Session>& Value : ToClose) Value->Close("host session corrected");
	}

	void RecordAnomalousSession(const std::uint64_t Session)
	{
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			AnomalousKnownSession = std::max(AnomalousKnownSession, Session);
		}
		Log(LogLevel::Warning, "peer reported an implausible known host session " + std::to_string(Session));
	}

	void Discover(PeerCandidate CandidateValue)
	{
		const bool ExactSelf = CandidateValue.NodeId == Config.NodeId &&
		                       CandidateValue.HostSession == Config.HostSession &&
		                       CandidateValue.InstanceId == Config.InstanceId;
		const bool ShouldInitiate = CandidateValue.NodeId == Config.NodeId
			? Config.InstanceId < CandidateValue.InstanceId
			: Config.NodeId < CandidateValue.NodeId;
		if (!Running || CandidateValue.ClusterId != Config.ClusterId || CandidateValue.NodeId.empty() || ExactSelf ||
		    CandidateValue.Address.empty() || CandidateValue.Port == 0 ||
		    CandidateValue.ProtocolMinimum > PeerProtocolVersion || CandidateValue.ProtocolMaximum < PeerProtocolVersion ||
		    !ShouldInitiate)
		{
			return;
		}
		const auto ExistingPeer = Peers.find(CandidateValue.NodeId);
		if (ExistingPeer != Peers.end() && !ExistingPeer->second.expired()) return;
		const auto Existing = DiscoveredCandidates.find(CandidateValue.NodeId);
		if (Existing != DiscoveredCandidates.end())
		{
			Existing->second->Seed = { std::move(CandidateValue.Address), CandidateValue.Port };
			Existing->second->Schedule(true);
			return;
		}
		if (DiscoveredCandidates.size() >= Config.MaximumCandidateMembers)
		{
			++RefusedCandidates;
			return;
		}
		auto Item = std::make_shared<Candidate>(*this,
			PeerSeed{ std::move(CandidateValue.Address), CandidateValue.Port });
		DiscoveredCandidates.emplace(std::move(CandidateValue.NodeId), Item);
		Candidates.push_back(Item);
		Item->Schedule(true);
	}

	std::chrono::milliseconds ReserveDialDelay()
	{
		const auto Now = std::chrono::steady_clock::now();
		const std::size_t Rate = std::max<std::size_t>(1, Config.MaximumDialAttemptsPerSecond);
		const auto Spacing = std::chrono::milliseconds(std::max<std::size_t>(1, 1000 / Rate));
		if (NextDial < Now) NextDial = Now;
		const auto Reserved = NextDial;
		NextDial += Spacing;
		return std::chrono::duration_cast<std::chrono::milliseconds>(Reserved - Now);
	}

	void Stop()
	{
		if (!Running) return;
		Running = false;
		asio::error_code Ignored;
		Acceptor.cancel(Ignored);
		Acceptor.close(Ignored);
		for (const std::shared_ptr<Candidate>& CandidateValue : Candidates) CandidateValue->Timer.cancel();
		std::vector<std::shared_ptr<Session>> ToClose;
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			for (const auto& Pair : Sessions)
			{
				ToClose.push_back(Pair.second);
			}
			Peers.clear();
		}
		for (const std::shared_ptr<Session>& Value : ToClose) Value->Close("host stopping");
	}

	std::string DiagnosticsJson() const
	{
		Wire::JsonValue Root;
		Root.SetObject();
		AddBoolean(Root, "enabled", Config.EnableDistributedMode);
		AddString(Root, "cluster_id", Config.ClusterId);
		AddUnsigned(Root, "peer_protocol_version", PeerProtocolVersion);
		AddUnsigned(Root, "peer_port", Bound.port());
		Wire::JsonValue Array;
		Array.SetArray();
		std::lock_guard<std::mutex> Lock(Mutex);
		for (const auto& Pair : Peers)
		{
			const std::shared_ptr<Session> Value = Pair.second.lock();
			if (!Value) continue;
			Wire::JsonValue Item;
			Item.SetObject();
			AddString(Item, "node_id", Value->RemoteHello.NodeId);
			AddUnsigned(Item, "host_session", Value->RemoteHello.HostSession);
			AddString(Item, "instance_id", Value->RemoteHello.InstanceId);
			AddString(Item, "state", "connected");
			(void) Array.Append(std::move(Item));
		}
		(void) Root.InsertMember("peers", std::move(Array));
		AddUnsigned(Root, "peer_count", Peers.size());
		AddUnsigned(Root, "maximum_peers", Config.MaximumPeers);
		AddUnsigned(Root, "pending_handshakes", PendingHandshakes);
		AddUnsigned(Root, "refused_handshakes", RefusedHandshakes);
		AddUnsigned(Root, "refused_peers", RefusedPeers);
		AddUnsigned(Root, "refused_candidates", RefusedCandidates);
		AddUnsigned(Root, "known_session_cache", KnownSessions.Size());
		AddUnsigned(Root, "required_host_session", RequiredHostSession);
		AddUnsigned(Root, "anomalous_known_session", AnomalousKnownSession);
		std::string Result;
		return Wire::SerializeJson(Root, Result) ? Result : "{}";
	}

	asio::io_context& Io;
	HostConfig Config;
	Tcp::acceptor Acceptor;
	Tcp::endpoint Bound;
	KnownHostSessions KnownSessions;
	std::vector<std::shared_ptr<Candidate>> Candidates;
	std::map<std::string, std::shared_ptr<Candidate>> DiscoveredCandidates;
	mutable std::mutex Mutex;
	std::map<Session*, std::shared_ptr<Session>> Sessions;
	std::map<std::string, std::weak_ptr<Session>> Peers;
	std::size_t PendingHandshakes = 0;
	std::uint64_t RefusedHandshakes = 0;
	std::uint64_t RefusedPeers = 0;
	std::uint64_t RefusedCandidates = 0;
	std::uint64_t RequiredHostSession = 0;
	std::uint64_t AnomalousKnownSession = 0;
	bool Running = false;
	std::chrono::steady_clock::time_point NextDial{};
};

void HostPeerNetwork::Implementation::Candidate::Dial()
{
	if (!Owner.Running || Dialing) return;
	Dialing = true;
	asio::error_code Error;
	const asio::ip::address Address = asio::ip::make_address(Seed.Address, Error);
	if (Error || !Address.is_v4())
	{
		Owner.Log(LogLevel::Warning, "invalid peer seed " + Seed.Address);
		OnClosed();
		return;
	}
	auto Self = shared_from_this();
	auto Peer = std::make_shared<Session>(Owner, Tcp::socket(Owner.Io), Self, false);
	Owner.TrackSession(Peer);
	Peer->Socket.async_connect(Tcp::endpoint(Address, Seed.Port), [Self, Peer](const asio::error_code& ConnectError)
	{
		if (ConnectError)
		{
			Self->Owner.Log(LogLevel::Information, "peer seed connect failed: " + ConnectError.message());
			Peer->Close({});
			return;
		}
		Self->Backoff = std::chrono::seconds(1);
		Peer->Start();
	});
}

HostPeerNetwork::HostPeerNetwork(asio::io_context& Io, HostConfig Config)
	: Impl(std::make_unique<Implementation>(Io, std::move(Config)))
{
}

HostPeerNetwork::~HostPeerNetwork()
{
	Stop();
}

bool HostPeerNetwork::Start(std::string& OutError)
{
	return Impl->Start(OutError);
}

void HostPeerNetwork::Stop()
{
	if (Impl) Impl->Stop();
}

std::string HostPeerNetwork::DiagnosticsJson() const
{
	return Impl->DiagnosticsJson();
}

std::string HostPeerNetwork::BoundAddress() const
{
	return Impl->Bound.address().to_string();
}

std::uint16_t HostPeerNetwork::BoundPort() const
{
	return Impl->Bound.port();
}

void HostPeerNetwork::Discover(PeerCandidate Candidate)
{
	Impl->Discover(std::move(Candidate));
}
}    // namespace DeviceExplorer::Host
