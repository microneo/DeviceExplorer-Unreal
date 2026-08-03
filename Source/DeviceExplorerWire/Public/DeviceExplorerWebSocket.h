#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace DeviceExplorer::Wire
{
struct ByteView
{
	const std::uint8_t* Data = nullptr;
	std::size_t Size = 0;
};

enum class WebSocketRole : std::uint8_t
{
	Client,
	Server
};

enum class WebSocketOpcode : std::uint8_t
{
	Continuation = 0x0,
	Text = 0x1,
	Binary = 0x2,
	Close = 0x8,
	Ping = 0x9,
	Pong = 0xA
};

enum class WebSocketError : std::uint8_t
{
	None,
	InvalidInput,
	InvalidReservedBits,
	UnknownOpcode,
	InvalidMask,
	NonMinimalLength,
	FrameTooLarge,
	MessageTooLarge,
	InvalidControlFrame,
	InvalidFragmentSequence,
	InvalidUtf8,
	InvalidClosePayload
};

struct WebSocketLimits
{
	std::uint64_t MaximumFramePayloadBytes = 8ULL * 1024ULL * 1024ULL;
	std::uint64_t MaximumMessagePayloadBytes = 8ULL * 1024ULL * 1024ULL;
};

struct WebSocketFrame
{
	WebSocketOpcode Opcode = WebSocketOpcode::Continuation;
	bool Final = true;
	std::vector<std::uint8_t> Payload;
};

class WebSocketDecoder
{
public:
	explicit WebSocketDecoder(WebSocketRole LocalRole, WebSocketLimits Limits = {});

	bool Consume(ByteView Bytes);
	bool Drain(WebSocketFrame& OutFrame);
	WebSocketError GetError() const { return Error; }
	const char* GetErrorText() const;

private:
	bool ParseAvailable();
	bool ValidateFrame(const WebSocketFrame& Frame);
	bool Fail(WebSocketError InError);

	WebSocketRole Role;
	WebSocketLimits Limits;
	WebSocketError Error = WebSocketError::None;
	std::vector<std::uint8_t> Input;
	std::deque<WebSocketFrame> Frames;
	WebSocketOpcode FragmentedOpcode = WebSocketOpcode::Continuation;
	std::uint64_t FragmentedMessageBytes = 0;
	std::vector<std::uint8_t> FragmentedText;
};

bool EncodeWebSocketFrame(const WebSocketFrame& Frame,
	                      WebSocketRole SenderRole,
	                      std::uint32_t MaskKey,
	                      std::vector<std::uint8_t>& OutBytes,
	                      WebSocketError* OutError = nullptr);

bool IsValidWebSocketUtf8(ByteView Bytes);
bool IsValidWebSocketCloseCode(std::uint16_t Code);
}    // namespace DeviceExplorer::Wire
