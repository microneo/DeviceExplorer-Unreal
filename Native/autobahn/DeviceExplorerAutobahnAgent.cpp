#include "DeviceExplorerHttpUpgrade.h"
#include "DeviceExplorerWebSocket.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
using NativeSocket = SOCKET;
constexpr NativeSocket InvalidSocket = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using NativeSocket = int;
constexpr NativeSocket InvalidSocket = -1;
#endif

namespace
{
using namespace DeviceExplorer::Wire;

constexpr std::size_t MaximumHandshakeBytes = 64 * 1024;
constexpr std::size_t ReceiveBufferBytes = 64 * 1024;

class SocketRuntime
{
public:
	SocketRuntime()
	{
#if defined(_WIN32)
		WSADATA Data{};
		Valid = WSAStartup(MAKEWORD(2, 2), &Data) == 0;
#else
		Valid = true;
#endif
	}

	~SocketRuntime()
	{
#if defined(_WIN32)
		if (Valid) WSACleanup();
#endif
	}

	bool IsValid() const { return Valid; }

private:
	bool Valid = false;
};

void CloseSocket(const NativeSocket Socket)
{
	if (Socket == InvalidSocket) return;
#if defined(_WIN32)
	closesocket(Socket);
#else
	close(Socket);
#endif
}

class OwnedSocket
{
public:
	OwnedSocket() = default;
	explicit OwnedSocket(const NativeSocket InSocket)
		: Socket(InSocket)
	{
	}
	~OwnedSocket() { CloseSocket(Socket); }
	OwnedSocket(const OwnedSocket&) = delete;
	OwnedSocket& operator=(const OwnedSocket&) = delete;
	OwnedSocket(OwnedSocket&& Other) noexcept
		: Socket(Other.Release())
	{
	}
	OwnedSocket& operator=(OwnedSocket&& Other) noexcept
	{
		if (this != &Other)
		{
			CloseSocket(Socket);
			Socket = Other.Release();
		}
		return *this;
	}

	NativeSocket Get() const { return Socket; }
	bool IsValid() const { return Socket != InvalidSocket; }
	NativeSocket Release()
	{
		const NativeSocket Result = Socket;
		Socket = InvalidSocket;
		return Result;
	}

private:
	NativeSocket Socket = InvalidSocket;
};

void SetSocketTimeout(const NativeSocket Socket, const int Milliseconds)
{
#if defined(_WIN32)
	const DWORD Timeout = static_cast<DWORD>(Milliseconds);
	setsockopt(Socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&Timeout), sizeof(Timeout));
	setsockopt(Socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&Timeout), sizeof(Timeout));
#else
	timeval Timeout{};
	Timeout.tv_sec = Milliseconds / 1000;
	Timeout.tv_usec = (Milliseconds % 1000) * 1000;
	setsockopt(Socket, SOL_SOCKET, SO_RCVTIMEO, &Timeout, sizeof(Timeout));
	setsockopt(Socket, SOL_SOCKET, SO_SNDTIMEO, &Timeout, sizeof(Timeout));
#endif
}

bool SendAll(const NativeSocket Socket, const std::uint8_t* Data, const std::size_t Size)
{
	std::size_t Offset = 0;
	while (Offset < Size)
	{
		const std::size_t Remaining = Size - Offset;
		const int Chunk = static_cast<int>(std::min<std::size_t>(Remaining, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#if defined(_WIN32)
		const int Sent = send(Socket, reinterpret_cast<const char*>(Data + Offset), Chunk, 0);
#else
		const int Sent = static_cast<int>(send(Socket, Data + Offset, static_cast<std::size_t>(Chunk), MSG_NOSIGNAL));
#endif
		if (Sent <= 0) return false;
		Offset += static_cast<std::size_t>(Sent);
	}
	return true;
}

int ReceiveSome(const NativeSocket Socket, std::uint8_t* Data, const std::size_t Capacity)
{
	const int Chunk = static_cast<int>(std::min<std::size_t>(Capacity, static_cast<std::size_t>(std::numeric_limits<int>::max())));
#if defined(_WIN32)
	return recv(Socket, reinterpret_cast<char*>(Data), Chunk, 0);
#else
	return static_cast<int>(recv(Socket, Data, static_cast<std::size_t>(Chunk), 0));
#endif
}

OwnedSocket ConnectTcp(const std::string& Host, const std::uint16_t Port)
{
	addrinfo Hints{};
	Hints.ai_family = AF_INET;
	Hints.ai_socktype = SOCK_STREAM;
	Hints.ai_protocol = IPPROTO_TCP;
	addrinfo* Addresses = nullptr;
	const std::string PortText = std::to_string(Port);
	if (getaddrinfo(Host.c_str(), PortText.c_str(), &Hints, &Addresses) != 0) return {};

	OwnedSocket Result;
	for (const addrinfo* Address = Addresses; Address != nullptr; Address = Address->ai_next)
	{
		OwnedSocket Candidate(socket(Address->ai_family, Address->ai_socktype, Address->ai_protocol));
		if (Candidate.IsValid() && connect(Candidate.Get(), Address->ai_addr, static_cast<int>(Address->ai_addrlen)) == 0)
		{
			Result = std::move(Candidate);
			break;
		}
	}
	freeaddrinfo(Addresses);
	if (Result.IsValid()) SetSocketTimeout(Result.Get(), 30000);
	return Result;
}

OwnedSocket ListenTcp(const std::string& Host, const std::uint16_t Port)
{
	addrinfo Hints{};
	Hints.ai_family = AF_INET;
	Hints.ai_socktype = SOCK_STREAM;
	Hints.ai_protocol = IPPROTO_TCP;
	Hints.ai_flags = AI_PASSIVE;
	addrinfo* Addresses = nullptr;
	const std::string PortText = std::to_string(Port);
	const char* HostName = Host.empty() || Host == "0.0.0.0" ? nullptr : Host.c_str();
	if (getaddrinfo(HostName, PortText.c_str(), &Hints, &Addresses) != 0) return {};

	OwnedSocket Result;
	for (const addrinfo* Address = Addresses; Address != nullptr; Address = Address->ai_next)
	{
		OwnedSocket Candidate(socket(Address->ai_family, Address->ai_socktype, Address->ai_protocol));
		if (!Candidate.IsValid()) continue;
		int Reuse = 1;
#if defined(_WIN32)
		setsockopt(Candidate.Get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&Reuse), sizeof(Reuse));
#else
		setsockopt(Candidate.Get(), SOL_SOCKET, SO_REUSEADDR, &Reuse, sizeof(Reuse));
#endif
		if (bind(Candidate.Get(), Address->ai_addr, static_cast<int>(Address->ai_addrlen)) == 0 &&
		    listen(Candidate.Get(), 16) == 0)
		{
			Result = std::move(Candidate);
			break;
		}
	}
	freeaddrinfo(Addresses);
	return Result;
}

std::array<std::uint8_t, 16> RandomNonce()
{
	std::random_device Random;
	std::array<std::uint8_t, 16> Result{};
	for (std::uint8_t& Byte : Result) Byte = static_cast<std::uint8_t>(Random());
	return Result;
}

std::uint32_t RandomMask()
{
	static std::atomic<std::uint64_t> Counter{ 0 };
	std::uint64_t Value = static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
	Value ^= ++Counter * 0x9E3779B97F4A7C15ULL;
	Value ^= Value >> 30;
	Value *= 0xBF58476D1CE4E5B9ULL;
	Value ^= Value >> 27;
	Value *= 0x94D049BB133111EBULL;
	Value ^= Value >> 31;
	return static_cast<std::uint32_t>(Value ^ (Value >> 32));
}

bool SendFrame(const NativeSocket Socket, const WebSocketFrame& Frame, const WebSocketRole Role)
{
	std::vector<std::uint8_t> Bytes;
	if (!EncodeWebSocketFrame(Frame, Role, RandomMask(), Bytes)) return false;
	return SendAll(Socket, Bytes.data(), Bytes.size());
}

std::uint16_t ProtocolCloseCode(const WebSocketError Error)
{
	switch (Error)
	{
		case WebSocketError::InvalidUtf8:
		case WebSocketError::InvalidClosePayload:
			return 1007;
		case WebSocketError::FrameTooLarge:
		case WebSocketError::MessageTooLarge:
			return 1009;
		default:
			return 1002;
	}
}

bool SendClose(const NativeSocket Socket, const WebSocketRole Role, const std::uint16_t Code)
{
	WebSocketFrame Close;
	Close.Opcode = WebSocketOpcode::Close;
	Close.Payload = {
		static_cast<std::uint8_t>((Code >> 8) & 0xFF),
		static_cast<std::uint8_t>(Code & 0xFF)
	};
	return SendFrame(Socket, Close, Role);
}

bool ReadClientHandshake(const NativeSocket Socket,
	                     const std::string& ExpectedAccept,
	                     std::vector<std::uint8_t>& OutTail)
{
	std::vector<std::uint8_t> Buffer;
	std::array<std::uint8_t, ReceiveBufferBytes> Chunk{};
	while (Buffer.size() < MaximumHandshakeBytes)
	{
		const int Read = ReceiveSome(Socket, Chunk.data(), Chunk.size());
		if (Read <= 0) return false;
		Buffer.insert(Buffer.end(), Chunk.begin(), Chunk.begin() + Read);
		WebSocketUpgradeResponse Response;
		const HttpUpgradeParseResult Parsed = ParseWebSocketUpgradeResponse(
			{ Buffer.data(), Buffer.size() }, ExpectedAccept, Response, MaximumHandshakeBytes);
		if (Parsed.Status == HttpUpgradeStatus::Error) return false;
		if (Parsed.Status == HttpUpgradeStatus::Complete)
		{
			OutTail.assign(Buffer.begin() + static_cast<std::ptrdiff_t>(Parsed.ConsumedBytes), Buffer.end());
			return true;
		}
	}
	return false;
}

bool OpenWebSocketClient(const NativeSocket Socket,
	                     const std::string& Host,
	                     const std::uint16_t Port,
	                     const std::string& Target,
	                     std::vector<std::uint8_t>& OutTail)
{
	const std::array<std::uint8_t, 16> Nonce = RandomNonce();
	std::string Key;
	std::string Accept;
	if (!MakeWebSocketClientKey({ Nonce.data(), Nonce.size() }, Key) || !MakeWebSocketAccept(Key, Accept)) return false;
	const std::string Request = SerializeWebSocketUpgradeRequest(
		Target, Host + ":" + std::to_string(Port), Key);
	return !Request.empty() &&
	       SendAll(Socket, reinterpret_cast<const std::uint8_t*>(Request.data()), Request.size()) &&
	       ReadClientHandshake(Socket, Accept, OutTail);
}

bool ReadServerHandshake(const NativeSocket Socket, std::vector<std::uint8_t>& OutTail)
{
	std::vector<std::uint8_t> Buffer;
	std::array<std::uint8_t, ReceiveBufferBytes> Chunk{};
	while (Buffer.size() < MaximumHandshakeBytes)
	{
		const int Read = ReceiveSome(Socket, Chunk.data(), Chunk.size());
		if (Read <= 0) return false;
		Buffer.insert(Buffer.end(), Chunk.begin(), Chunk.begin() + Read);
		WebSocketUpgradeRequest Request;
		const HttpUpgradeParseResult Parsed = ParseWebSocketUpgradeRequest(
			{ Buffer.data(), Buffer.size() }, Request, MaximumHandshakeBytes);
		if (Parsed.Status == HttpUpgradeStatus::Error) return false;
		if (Parsed.Status == HttpUpgradeStatus::Complete)
		{
			std::string Accept;
			if (!MakeWebSocketAccept(Request.Key, Accept)) return false;
			const std::string Response = SerializeWebSocketUpgradeResponse(Accept);
			if (Response.empty() || !SendAll(Socket, reinterpret_cast<const std::uint8_t*>(Response.data()), Response.size()))
			{
				return false;
			}
			OutTail.assign(Buffer.begin() + static_cast<std::ptrdiff_t>(Parsed.ConsumedBytes), Buffer.end());
			return true;
		}
	}
	return false;
}

enum class EchoResult
{
	Continue,
	Finished,
	Failed
};

EchoResult ProcessEchoBytes(const NativeSocket Socket,
	                        WebSocketDecoder& Decoder,
	                        const WebSocketRole Role,
	                        const ByteView Bytes)
{
	if (!Decoder.Consume(Bytes))
	{
		SendClose(Socket, Role, ProtocolCloseCode(Decoder.GetError()));
		return EchoResult::Finished;
	}
	WebSocketFrame Frame;
	while (Decoder.Drain(Frame))
	{
		if (Frame.Opcode == WebSocketOpcode::Ping)
		{
			Frame.Opcode = WebSocketOpcode::Pong;
			if (!SendFrame(Socket, Frame, Role)) return EchoResult::Failed;
		}
		else if (Frame.Opcode == WebSocketOpcode::Close)
		{
			return SendFrame(Socket, Frame, Role) ? EchoResult::Finished : EchoResult::Failed;
		}
		else if (Frame.Opcode == WebSocketOpcode::Text || Frame.Opcode == WebSocketOpcode::Binary ||
		         Frame.Opcode == WebSocketOpcode::Continuation)
		{
			if (!SendFrame(Socket, Frame, Role)) return EchoResult::Failed;
		}
	}
	return EchoResult::Continue;
}

bool RunEchoConnection(const NativeSocket Socket,
	                   const WebSocketRole Role,
	                   const std::vector<std::uint8_t>& InitialBytes)
{
	WebSocketDecoder Decoder(Role);
	if (!InitialBytes.empty())
	{
		const EchoResult Initial = ProcessEchoBytes(Socket, Decoder, Role, { InitialBytes.data(), InitialBytes.size() });
		if (Initial != EchoResult::Continue) return Initial == EchoResult::Finished;
	}
	std::array<std::uint8_t, ReceiveBufferBytes> Buffer{};
	for (;;)
	{
		const int Read = ReceiveSome(Socket, Buffer.data(), Buffer.size());
		if (Read <= 0) return false;
		const EchoResult Result = ProcessEchoBytes(Socket, Decoder, Role, { Buffer.data(), static_cast<std::size_t>(Read) });
		if (Result != EchoResult::Continue) return Result == EchoResult::Finished;
	}
}

bool ReadSingleText(const NativeSocket Socket,
	                const WebSocketRole Role,
	                const std::vector<std::uint8_t>& InitialBytes,
	                std::string& OutText)
{
	WebSocketDecoder Decoder(Role);
	std::vector<std::uint8_t> Fragmented;
	bool bFragmentedText = false;
	auto Process = [&](const ByteView Bytes) -> int
	{
		if (!Decoder.Consume(Bytes)) return -1;
		WebSocketFrame Frame;
		while (Decoder.Drain(Frame))
		{
			if (Frame.Opcode == WebSocketOpcode::Ping)
			{
				Frame.Opcode = WebSocketOpcode::Pong;
				if (!SendFrame(Socket, Frame, Role)) return -1;
			}
			else if (Frame.Opcode == WebSocketOpcode::Text)
			{
				if (Frame.Final)
				{
					OutText.assign(Frame.Payload.begin(), Frame.Payload.end());
					return 1;
				}
				bFragmentedText = true;
				Fragmented = Frame.Payload;
			}
			else if (Frame.Opcode == WebSocketOpcode::Continuation && bFragmentedText)
			{
				Fragmented.insert(Fragmented.end(), Frame.Payload.begin(), Frame.Payload.end());
				if (Frame.Final)
				{
					OutText.assign(Fragmented.begin(), Fragmented.end());
					return 1;
				}
			}
			else if (Frame.Opcode == WebSocketOpcode::Close)
			{
				SendFrame(Socket, Frame, Role);
				return -1;
			}
		}
		return 0;
	};

	if (!InitialBytes.empty())
	{
		const int Result = Process({ InitialBytes.data(), InitialBytes.size() });
		if (Result != 0) return Result > 0;
	}
	std::array<std::uint8_t, ReceiveBufferBytes> Buffer{};
	for (;;)
	{
		const int Read = ReceiveSome(Socket, Buffer.data(), Buffer.size());
		if (Read <= 0) return false;
		const int Result = Process({ Buffer.data(), static_cast<std::size_t>(Read) });
		if (Result != 0) return Result > 0;
	}
}

bool FetchAutobahnCaseCount(const std::string& Host, const std::uint16_t Port, std::size_t& OutCount)
{
	OwnedSocket Socket = ConnectTcp(Host, Port);
	std::vector<std::uint8_t> Tail;
	std::string Text;
	if (!Socket.IsValid() || !OpenWebSocketClient(Socket.Get(), Host, Port, "/getCaseCount", Tail) ||
	    !ReadSingleText(Socket.Get(), WebSocketRole::Client, Tail, Text))
	{
		return false;
	}
	char* End = nullptr;
	errno = 0;
	const unsigned long Parsed = std::strtoul(Text.c_str(), &End, 10);
	if (errno != 0 || End == Text.c_str() || *End != '\0' || Parsed == 0 || Parsed > 10000) return false;
	OutCount = static_cast<std::size_t>(Parsed);
	SendClose(Socket.Get(), WebSocketRole::Client, 1000);
	return true;
}

bool RunAutobahnClientCase(const std::string& Host,
	                       const std::uint16_t Port,
	                       const std::string& Target)
{
	OwnedSocket Socket = ConnectTcp(Host, Port);
	std::vector<std::uint8_t> Tail;
	return Socket.IsValid() && OpenWebSocketClient(Socket.Get(), Host, Port, Target, Tail) &&
	       RunEchoConnection(Socket.Get(), WebSocketRole::Client, Tail);
}

int RunAutobahnClient(const std::string& Host, const std::uint16_t Port, const std::string& Agent)
{
	std::size_t CaseCount = 0;
	if (!FetchAutobahnCaseCount(Host, Port, CaseCount))
	{
		std::cerr << "cannot read Autobahn case count\n";
		return 1;
	}
	std::cout << "running " << CaseCount << " Autobahn client cases\n";
	for (std::size_t Case = 1; Case <= CaseCount; ++Case)
	{
		const std::string Target = "/runCase?case=" + std::to_string(Case) + "&agent=" + Agent;
		if (!RunAutobahnClientCase(Host, Port, Target))
		{
			std::cerr << "case " << Case << " transport failed\n";
		}
	}
	if (!RunAutobahnClientCase(Host, Port, "/updateReports?agent=" + Agent))
	{
		std::cerr << "cannot update Autobahn reports\n";
		return 1;
	}
	return 0;
}

int RunEchoServer(const std::string& Host, const std::uint16_t Port, const bool bOnce)
{
	OwnedSocket Listener = ListenTcp(Host, Port);
	if (!Listener.IsValid())
	{
		std::cerr << "cannot listen on " << Host << ':' << Port << '\n';
		return 1;
	}
	std::cout << "Autobahn echo server listening on " << Host << ':' << Port << '\n';
	do
	{
		OwnedSocket Client(accept(Listener.Get(), nullptr, nullptr));
		if (!Client.IsValid()) return 1;
		SetSocketTimeout(Client.Get(), 30000);
		std::vector<std::uint8_t> Tail;
		if (!ReadServerHandshake(Client.Get(), Tail) ||
		    !RunEchoConnection(Client.Get(), WebSocketRole::Server, Tail))
		{
			std::cerr << "echo connection ended with an I/O error\n";
		}
	} while (!bOnce);
	return 0;
}

int RunEchoClient(const std::string& Host,
	              const std::uint16_t Port,
	              const std::string& Message)
{
	OwnedSocket Socket = ConnectTcp(Host, Port);
	std::vector<std::uint8_t> Tail;
	if (!Socket.IsValid())
	{
		std::cerr << "echo client cannot connect\n";
		return 1;
	}
	if (!OpenWebSocketClient(Socket.Get(), Host, Port, "/echo", Tail))
	{
		std::cerr << "echo client handshake failed\n";
		return 1;
	}
	WebSocketFrame Frame;
	Frame.Opcode = WebSocketOpcode::Text;
	Frame.Payload.assign(Message.begin(), Message.end());
	if (!SendFrame(Socket.Get(), Frame, WebSocketRole::Client))
	{
		std::cerr << "echo client send failed\n";
		return 1;
	}
	std::string Echo;
	if (!ReadSingleText(Socket.Get(), WebSocketRole::Client, Tail, Echo))
	{
		std::cerr << "echo client receive failed\n";
		return 1;
	}
	if (Echo != Message)
	{
		std::cerr << "echo payload mismatch\n";
		return 1;
	}
	SendClose(Socket.Get(), WebSocketRole::Client, 1000);
	std::cout << "echo self-test passed\n";
	return 0;
}

int RunSelfTest(const std::uint16_t Port)
{
	int ServerResult = 1;
	std::thread Server(
		[Port, &ServerResult]() { ServerResult = RunEchoServer("127.0.0.1", Port, true); });
	std::this_thread::sleep_for(std::chrono::milliseconds(100));
	const int ClientResult = RunEchoClient("127.0.0.1", Port, "DeviceExplorer-D1");
	Server.join();
	return ClientResult == 0 && ServerResult == 0 ? 0 : 1;
}

std::string ArgumentValue(const int ArgC,
	                      const char* const* ArgV,
	                      const std::string_view Name,
	                      const std::string& Default)
{
	for (int Index = 2; Index + 1 < ArgC; ++Index)
	{
		if (ArgV[Index] == Name) return ArgV[Index + 1];
	}
	return Default;
}

bool HasArgument(const int ArgC, const char* const* ArgV, const std::string_view Name)
{
	for (int Index = 2; Index < ArgC; ++Index)
	{
		if (ArgV[Index] == Name) return true;
	}
	return false;
}

bool ParsePort(const std::string& Text, std::uint16_t& OutPort)
{
	char* End = nullptr;
	errno = 0;
	const unsigned long Parsed = std::strtoul(Text.c_str(), &End, 10);
	if (errno != 0 || End == Text.c_str() || *End != '\0' || Parsed == 0 || Parsed > 65535) return false;
	OutPort = static_cast<std::uint16_t>(Parsed);
	return true;
}
}    // namespace

int main(const int ArgC, const char* const* ArgV)
{
	SocketRuntime Runtime;
	if (!Runtime.IsValid())
	{
		std::cerr << "cannot initialize sockets\n";
		return 1;
	}
	if (ArgC < 2)
	{
		std::cerr << "usage: dexp-autobahn-agent <server|client|echo-client|self-test> [options]\n";
		return 2;
	}

	const std::string Mode = ArgV[1];
	const std::string DefaultPort = Mode == "server" || Mode == "echo-client" ? "9002" :
	                                Mode == "self-test" ? "19002" : "9001";
	std::uint16_t Port = 0;
	if (!ParsePort(ArgumentValue(ArgC, ArgV, "--port", DefaultPort), Port))
	{
		std::cerr << "invalid port\n";
		return 2;
	}
	if (Mode == "server")
	{
		return RunEchoServer(ArgumentValue(ArgC, ArgV, "--host", "0.0.0.0"), Port,
		                     HasArgument(ArgC, ArgV, "--once"));
	}
	if (Mode == "client")
	{
		return RunAutobahnClient(ArgumentValue(ArgC, ArgV, "--host", "127.0.0.1"), Port,
		                         ArgumentValue(ArgC, ArgV, "--agent", "DeviceExplorerWire"));
	}
	if (Mode == "echo-client")
	{
		return RunEchoClient(ArgumentValue(ArgC, ArgV, "--host", "127.0.0.1"), Port,
		                     ArgumentValue(ArgC, ArgV, "--message", "DeviceExplorerWire"));
	}
	if (Mode == "self-test") return RunSelfTest(Port);
	std::cerr << "unknown mode: " << Mode << '\n';
	return 2;
}
