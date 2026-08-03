#include "DeviceExplorerTransport.h"

#include "Containers/StringConv.h"
#include "DeviceExplorerHttpUpgrade.h"
#include "DeviceExplorerWebSocket.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/Guid.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace
{
constexpr double DeviceExplorerConnectTimeoutSeconds = 10.0;
constexpr double DeviceExplorerCloseTimeoutSeconds = 2.0;
constexpr int32 DeviceExplorerReceiveChunkBytes = 64 * 1024;
constexpr int32 DeviceExplorerMaximumIoIterations = 64;
constexpr int32 DeviceExplorerMaximumHandshakeBytes = 64 * 1024;
constexpr int32 DeviceExplorerMaximumSendQueueBytes = 16 * 1024 * 1024;

enum class EDeviceExplorerBuiltinState : uint8
{
	Idle,
	Connecting,
	Handshaking,
	Connected,
	Closing
};

std::string DeviceExplorerStringToUtf8(const FString& Value)
{
	const FTCHARToUTF8 Converted(*Value);
	return std::string(Converted.Get(), static_cast<std::size_t>(Converted.Length()));
}

FString DeviceExplorerUtf8ToString(const std::uint8_t* Data, const std::size_t Size)
{
	if (Size == 0) return FString();
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data), static_cast<int32>(Size));
	return FString(Converted.Length(), Converted.Get());
}

std::array<std::uint8_t, 16> MakeDeviceExplorerWebSocketNonce()
{
	const FGuid Guid = FGuid::NewGuid();
	const std::array<std::uint32_t, 4> Words = { Guid.A, Guid.B, Guid.C, Guid.D };
	std::array<std::uint8_t, 16> Bytes{};
	for (std::size_t WordIndex = 0; WordIndex < Words.size(); ++WordIndex)
	{
		for (std::size_t ByteIndex = 0; ByteIndex < 4; ++ByteIndex)
		{
			Bytes[WordIndex * 4 + ByteIndex] =
				static_cast<std::uint8_t>((Words[WordIndex] >> (24U - static_cast<unsigned>(ByteIndex) * 8U)) & 0xFFU);
		}
	}
	return Bytes;
}

std::uint32_t MakeDeviceExplorerMaskKey()
{
	const FGuid Guid = FGuid::NewGuid();
	return Guid.A ^ Guid.B ^ Guid.C ^ Guid.D;
}

std::uint16_t DeviceExplorerProtocolCloseCode(const DeviceExplorer::Wire::WebSocketError Error)
{
	using DeviceExplorer::Wire::WebSocketError;
	switch (Error)
	{
		case WebSocketError::InvalidUtf8:
			return 1007;
		case WebSocketError::FrameTooLarge:
		case WebSocketError::MessageTooLarge:
		case WebSocketError::FrameQueueFull:
			return 1009;
		default:
			return 1002;
	}
}

class FDeviceExplorerBuiltinTransport final : public IDeviceExplorerTransport
{
public:
	virtual ~FDeviceExplorerBuiltinTransport() override { CloseSilently(); }

	virtual void Connect(const FDeviceExplorerResolvedEndpoint& Endpoint,
	                     const FString& Token,
	                     FDeviceExplorerTransportCallbacks InCallbacks) override
	{
		CloseSilently();
		Callbacks = MoveTemp(InCallbacks);
		if (!Endpoint.IsUsable() || Token.IsEmpty())
		{
			Fail(TEXT("invalid endpoint or empty token"));
			return;
		}

		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		Socket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("DeviceExplorerBuiltinWebSocket"), false);
		if (Socket == nullptr)
		{
			Fail(TEXT("cannot create a TCP socket"));
			return;
		}
		Socket->SetNonBlocking(true);
		Socket->SetNoDelay(true);

		bool bValidAddress = false;
		TSharedRef<FInternetAddr> RemoteAddress = SocketSubsystem->CreateInternetAddr();
		RemoteAddress->SetIp(*Endpoint.Serialized.Address, bValidAddress);
		RemoteAddress->SetPort(Endpoint.Serialized.Port);
		if (!bValidAddress)
		{
			Fail(TEXT("cannot parse the endpoint address"));
			return;
		}

		Target = FString::Printf(TEXT("/device/connect?token=%s"), *FGenericPlatformHttp::UrlEncode(Token));
		Host = Endpoint.Serialized.ToString();
		Decoder = DeviceExplorer::Wire::CreateWebSocketDecoder(DeviceExplorer::Wire::WebSocketRole::Client);
		if (Decoder == nullptr)
		{
			Fail(TEXT("cannot allocate the WebSocket decoder"));
			return;
		}
		State = EDeviceExplorerBuiltinState::Connecting;
		DeadlineSeconds = FPlatformTime::Seconds() + DeviceExplorerConnectTimeoutSeconds;
		const bool bConnectedImmediately = Socket->Connect(*RemoteAddress);
		if (bConnectedImmediately || Socket->GetConnectionState() == SCS_Connected)
		{
			BeginHandshake();
		}
		else if (Socket->GetConnectionState() == SCS_ConnectionError)
		{
			Fail(TEXT("TCP connection failed"));
		}
	}

	virtual void Tick(const double NowSeconds) override
	{
		if (Socket == nullptr || State == EDeviceExplorerBuiltinState::Idle) return;

		if (State == EDeviceExplorerBuiltinState::Connecting)
		{
			const ESocketConnectionState ConnectionState = Socket->GetConnectionState();
			if (ConnectionState == SCS_Connected)
			{
				BeginHandshake();
			}
			else if (ConnectionState == SCS_ConnectionError)
			{
				Fail(TEXT("TCP connection failed"));
				return;
			}
			else if (NowSeconds >= DeadlineSeconds)
			{
				Fail(TEXT("TCP connection timed out"));
				return;
			}
		}

		if (State == EDeviceExplorerBuiltinState::Handshaking ||
		    State == EDeviceExplorerBuiltinState::Connected ||
		    State == EDeviceExplorerBuiltinState::Closing)
		{
			if (!FlushWrites() || !DrainReads()) return;
		}

		if (State == EDeviceExplorerBuiltinState::Handshaking && NowSeconds >= DeadlineSeconds)
		{
			Fail(TEXT("WebSocket upgrade timed out"));
			return;
		}
		if (State == EDeviceExplorerBuiltinState::Closing &&
		    (((bPeerCloseReceived || bCloseAfterFlush) && PendingSend.IsEmpty()) || NowSeconds >= DeadlineSeconds))
		{
			FinishClosed(PendingCloseCode, PendingCloseReason, bPeerCloseReceived && bPendingCloseClean);
		}
	}

	virtual bool SendText(const FString& Message) override
	{
		if (State != EDeviceExplorerBuiltinState::Connected) return false;
		DeviceExplorer::Wire::WebSocketFrame Frame;
		Frame.Opcode = DeviceExplorer::Wire::WebSocketOpcode::Text;
		const std::string Utf8 = DeviceExplorerStringToUtf8(Message);
		Frame.Payload.assign(Utf8.begin(), Utf8.end());
		if (!QueueFrame(Frame))
		{
			Fail(TEXT("WebSocket send queue limit exceeded"));
			return false;
		}
		return true;
	}

	virtual void Close() override
	{
		if (State == EDeviceExplorerBuiltinState::Connected)
		{
			QueueClose(1000, "");
			FlushWrites();
		}
		CloseSilently();
	}

	virtual bool IsConnected() const override { return State == EDeviceExplorerBuiltinState::Connected; }
	virtual const TCHAR* GetName() const override { return TEXT("Builtin"); }

private:
	void BeginHandshake()
	{
		const std::array<std::uint8_t, 16> Nonce = MakeDeviceExplorerWebSocketNonce();
		std::string Key;
		if (!DeviceExplorer::Wire::MakeWebSocketClientKey({ Nonce.data(), Nonce.size() }, Key) ||
		    !DeviceExplorer::Wire::MakeWebSocketAccept(Key, ExpectedAccept))
		{
			Fail(TEXT("cannot create a WebSocket client key"));
			return;
		}

		const std::string Request = DeviceExplorer::Wire::SerializeWebSocketUpgradeRequest(
			DeviceExplorerStringToUtf8(Target), DeviceExplorerStringToUtf8(Host), Key);
		if (Request.empty() || !QueueBytes(
			    reinterpret_cast<const std::uint8_t*>(Request.data()), Request.size()))
		{
			Fail(TEXT("cannot serialize the WebSocket upgrade request"));
			return;
		}
		State = EDeviceExplorerBuiltinState::Handshaking;
		DeadlineSeconds = FPlatformTime::Seconds() + DeviceExplorerConnectTimeoutSeconds;
	}

	bool FlushWrites()
	{
		for (int32 Iteration = 0; Iteration < DeviceExplorerMaximumIoIterations && PendingSendOffset < PendingSend.Num(); ++Iteration)
		{
			if (!Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::Zero())) return true;
			int32 Sent = 0;
			if (!Socket->Send(PendingSend.GetData() + PendingSendOffset, PendingSend.Num() - PendingSendOffset, Sent) || Sent <= 0)
			{
				Fail(TEXT("TCP send failed"));
				return false;
			}
			PendingSendOffset += Sent;
		}
		if (PendingSendOffset == PendingSend.Num())
		{
			PendingSend.Reset();
			PendingSendOffset = 0;
		}
		return true;
	}

	bool DrainReads()
	{
		if (bCloseAfterFlush) return true;
		std::array<std::uint8_t, DeviceExplorerReceiveChunkBytes> Buffer{};
		for (int32 Iteration = 0; Iteration < DeviceExplorerMaximumIoIterations; ++Iteration)
		{
			if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero())) return true;
			int32 Read = 0;
			if (!Socket->Recv(Buffer.data(), static_cast<int32>(Buffer.size()), Read, ESocketReceiveFlags::None) || Read <= 0)
			{
				Fail(TEXT("TCP connection closed"));
				return false;
			}

			if (State == EDeviceExplorerBuiltinState::Handshaking)
			{
				if (!ConsumeHandshake(Buffer.data(), static_cast<std::size_t>(Read))) return false;
			}
			else if (!ConsumeFrames(Buffer.data(), static_cast<std::size_t>(Read)))
			{
				return false;
			}
			if (Socket == nullptr) return false;    // a callback may close the transport
			if (bCloseAfterFlush) return true;
		}
		return true;
	}

	bool ConsumeHandshake(const std::uint8_t* Data, const std::size_t Size)
	{
		if (Size > static_cast<std::size_t>(DeviceExplorerMaximumHandshakeBytes - HandshakeBuffer.Num()))
		{
			Fail(TEXT("WebSocket upgrade header exceeds 64 KiB"));
			return false;
		}
		HandshakeBuffer.Append(Data, static_cast<int32>(Size));
		DeviceExplorer::Wire::WebSocketUpgradeResponse Response;
		const DeviceExplorer::Wire::HttpUpgradeParseResult Result = DeviceExplorer::Wire::ParseWebSocketUpgradeResponse(
			{ HandshakeBuffer.GetData(), static_cast<std::size_t>(HandshakeBuffer.Num()) }, ExpectedAccept, Response,
			DeviceExplorerMaximumHandshakeBytes);
		if (Result.Status == DeviceExplorer::Wire::HttpUpgradeStatus::NeedMoreData) return true;
		if (Result.Status == DeviceExplorer::Wire::HttpUpgradeStatus::Error)
		{
			Fail(FString::Printf(TEXT("WebSocket upgrade failed: %s"),
			                     UTF8_TO_TCHAR(DeviceExplorer::Wire::HttpUpgradeErrorText(Result.Error))));
			return false;
		}

		TArray<uint8> Tail;
		const int32 Consumed = static_cast<int32>(Result.ConsumedBytes);
		if (Consumed < HandshakeBuffer.Num())
		{
			Tail.Append(HandshakeBuffer.GetData() + Consumed, HandshakeBuffer.Num() - Consumed);
		}
		HandshakeBuffer.Reset();
		State = EDeviceExplorerBuiltinState::Connected;
		bEverConnected = true;
		if (Callbacks.OnConnected) Callbacks.OnConnected();
		if (Socket == nullptr || State != EDeviceExplorerBuiltinState::Connected) return false;
		return Tail.IsEmpty() || ConsumeFrames(Tail.GetData(), static_cast<std::size_t>(Tail.Num()));
	}

	bool ConsumeFrames(const std::uint8_t* Data, const std::size_t Size)
	{
		if (!DeviceExplorer::Wire::ConsumeWebSocketBytes(Decoder, { Data, Size }))
		{
			const DeviceExplorer::Wire::WebSocketError Error = DeviceExplorer::Wire::GetWebSocketDecoderError(Decoder);
			const FString Reason = UTF8_TO_TCHAR(DeviceExplorer::Wire::GetWebSocketDecoderErrorText(Decoder));
			if (!QueueClose(DeviceExplorerProtocolCloseCode(Error), DeviceExplorerStringToUtf8(Reason)))
			{
				Fail(Reason);
				return false;
			}
			State = EDeviceExplorerBuiltinState::Closing;
			DeadlineSeconds = FPlatformTime::Seconds() + DeviceExplorerCloseTimeoutSeconds;
			PendingCloseCode = DeviceExplorerProtocolCloseCode(Error);
			PendingCloseReason = Reason;
			bPendingCloseClean = false;
			bCloseAfterFlush = true;
			return true;
		}

		DeviceExplorer::Wire::WebSocketFrame Frame;
		while (DeviceExplorer::Wire::DrainWebSocketFrame(Decoder, Frame))
		{
			if (!HandleFrame(Frame)) return false;
		}
		return true;
	}

	bool HandleFrame(const DeviceExplorer::Wire::WebSocketFrame& Frame)
	{
		using DeviceExplorer::Wire::WebSocketOpcode;
		switch (Frame.Opcode)
		{
			case WebSocketOpcode::Ping:
			{
				DeviceExplorer::Wire::WebSocketFrame Pong = Frame;
				Pong.Opcode = WebSocketOpcode::Pong;
				if (!QueueFrame(Pong))
				{
					Fail(TEXT("cannot queue a WebSocket pong"));
					return false;
				}
				return true;
			}
			case WebSocketOpcode::Pong:
				return true;
			case WebSocketOpcode::Close:
			{
				std::uint16_t Code = 1000;
				FString Reason;
				if (Frame.Payload.size() >= 2)
				{
					Code = static_cast<std::uint16_t>((static_cast<std::uint16_t>(Frame.Payload[0]) << 8) | Frame.Payload[1]);
					Reason = DeviceExplorerUtf8ToString(Frame.Payload.data() + 2, Frame.Payload.size() - 2);
				}
				if (!bCloseFrameSent && !QueueFrame(Frame))
				{
					Fail(TEXT("cannot queue a WebSocket close reply"));
					return false;
				}
				bCloseFrameSent = true;
				bPeerCloseReceived = true;
				bCloseAfterFlush = true;
				PendingCloseCode = Code;
				PendingCloseReason = MoveTemp(Reason);
				bPendingCloseClean = true;
				State = EDeviceExplorerBuiltinState::Closing;
				DeadlineSeconds = FPlatformTime::Seconds() + DeviceExplorerCloseTimeoutSeconds;
				return true;
			}
			case WebSocketOpcode::Text:
			case WebSocketOpcode::Binary:
				if (Frame.Final)
				{
					if (Frame.Opcode == WebSocketOpcode::Text)
					{
						DispatchText(Frame.Payload);
						if (Socket == nullptr) return false;    // a callback may close the transport
					}
				}
				else
				{
					IncomingMessageOpcode = Frame.Opcode;
					IncomingMessagePayload = Frame.Payload;
				}
				return true;
			case WebSocketOpcode::Continuation:
				IncomingMessagePayload.insert(IncomingMessagePayload.end(), Frame.Payload.begin(), Frame.Payload.end());
				if (Frame.Final)
				{
					if (IncomingMessageOpcode == WebSocketOpcode::Text)
					{
						DispatchText(IncomingMessagePayload);
						if (Socket == nullptr) return false;    // a callback may close the transport
					}
					IncomingMessageOpcode = WebSocketOpcode::Continuation;
					IncomingMessagePayload.clear();
				}
				return true;
		}
		return true;
	}

	void DispatchText(const std::vector<std::uint8_t>& Payload)
	{
		if (Callbacks.OnMessage)
		{
			Callbacks.OnMessage(DeviceExplorerUtf8ToString(Payload.data(), Payload.size()));
		}
	}

	bool QueueClose(const std::uint16_t Code, const std::string& Reason)
	{
		DeviceExplorer::Wire::WebSocketFrame Frame;
		Frame.Opcode = DeviceExplorer::Wire::WebSocketOpcode::Close;
		Frame.Payload = {
			static_cast<std::uint8_t>((Code >> 8) & 0xFF),
			static_cast<std::uint8_t>(Code & 0xFF)
		};
		const std::size_t ReasonBytes = FMath::Min<std::size_t>(Reason.size(), 123);
		Frame.Payload.insert(Frame.Payload.end(), Reason.begin(), Reason.begin() + static_cast<std::ptrdiff_t>(ReasonBytes));
		if (!QueueFrame(Frame)) return false;
		bCloseFrameSent = true;
		return true;
	}

	bool QueueFrame(const DeviceExplorer::Wire::WebSocketFrame& Frame)
	{
		std::vector<std::uint8_t> Encoded;
		if (!DeviceExplorer::Wire::EncodeWebSocketFrame(
			    Frame, DeviceExplorer::Wire::WebSocketRole::Client, MakeDeviceExplorerMaskKey(), Encoded))
		{
			return false;
		}
		return QueueBytes(Encoded.data(), Encoded.size());
	}

	bool QueueBytes(const std::uint8_t* Data, const std::size_t Size)
	{
		if (Size > static_cast<std::size_t>(MAX_int32)) return false;
		if (PendingSendOffset > 0)
		{
			PendingSend.RemoveAt(0, PendingSendOffset, EAllowShrinking::No);
			PendingSendOffset = 0;
		}
		if (Size > static_cast<std::size_t>(DeviceExplorerMaximumSendQueueBytes - PendingSend.Num())) return false;
		PendingSend.Append(Data, static_cast<int32>(Size));
		return true;
	}

	void Fail(const FString& Reason)
	{
		const bool bWasConnected = bEverConnected;
		CloseSocket();
		State = EDeviceExplorerBuiltinState::Idle;
		if (bWasConnected)
		{
			if (Callbacks.OnClosed) Callbacks.OnClosed(1006, Reason, false);
		}
		else if (Callbacks.OnConnectionError)
		{
			Callbacks.OnConnectionError(Reason);
		}
	}

	void FinishClosed(const int32 Code, const FString& Reason, const bool bWasClean)
	{
		CloseSocket();
		State = EDeviceExplorerBuiltinState::Idle;
		if (Callbacks.OnClosed) Callbacks.OnClosed(Code, Reason, bWasClean);
	}

	void CloseSocket()
	{
		if (Socket != nullptr)
		{
			Socket->Close();
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
			Socket = nullptr;
		}
		DeviceExplorer::Wire::DestroyWebSocketDecoder(Decoder);
		Decoder = nullptr;
	}

	void CloseSilently()
	{
		CloseSocket();
		State = EDeviceExplorerBuiltinState::Idle;
		Callbacks = {};
		Target.Reset();
		Host.Reset();
		ExpectedAccept.clear();
		HandshakeBuffer.Reset();
		PendingSend.Reset();
		PendingSendOffset = 0;
		IncomingMessageOpcode = DeviceExplorer::Wire::WebSocketOpcode::Continuation;
		IncomingMessagePayload.clear();
		bCloseFrameSent = false;
		bPeerCloseReceived = false;
		bPendingCloseClean = false;
		bCloseAfterFlush = false;
		bEverConnected = false;
		PendingCloseCode = 1006;
		PendingCloseReason.Reset();
		DeadlineSeconds = 0.0;
	}

	FDeviceExplorerTransportCallbacks Callbacks;
	FSocket* Socket = nullptr;
	EDeviceExplorerBuiltinState State = EDeviceExplorerBuiltinState::Idle;
	FString Target;
	FString Host;
	std::string ExpectedAccept;
	TArray<uint8> HandshakeBuffer;
	TArray<uint8> PendingSend;
	int32 PendingSendOffset = 0;
	DeviceExplorer::Wire::WebSocketDecoderHandle* Decoder = nullptr;
	DeviceExplorer::Wire::WebSocketOpcode IncomingMessageOpcode = DeviceExplorer::Wire::WebSocketOpcode::Continuation;
	std::vector<std::uint8_t> IncomingMessagePayload;
	double DeadlineSeconds = 0.0;
	int32 PendingCloseCode = 1006;
	FString PendingCloseReason;
	bool bCloseFrameSent = false;
	bool bPeerCloseReceived = false;
	bool bPendingCloseClean = false;
	bool bCloseAfterFlush = false;
	bool bEverConnected = false;
};
}    // namespace

TUniquePtr<IDeviceExplorerTransport> CreateDeviceExplorerBuiltinTransport()
{
	return MakeUnique<FDeviceExplorerBuiltinTransport>();
}
