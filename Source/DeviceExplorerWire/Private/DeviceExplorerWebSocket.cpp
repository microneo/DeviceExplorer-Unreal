#include "DeviceExplorerWebSocket.h"

#include <algorithm>
#include <array>
#include <limits>

namespace DeviceExplorer::Wire
{
namespace
{
bool IsKnownOpcode(const std::uint8_t Value)
{
	switch (static_cast<WebSocketOpcode>(Value))
	{
		case WebSocketOpcode::Continuation:
		case WebSocketOpcode::Text:
		case WebSocketOpcode::Binary:
		case WebSocketOpcode::Close:
		case WebSocketOpcode::Ping:
		case WebSocketOpcode::Pong:
			return true;
	}
	return false;
}

bool IsControlOpcode(const WebSocketOpcode Opcode)
{
	return static_cast<std::uint8_t>(Opcode) >= 0x8;
}

void SetError(WebSocketError* OutError, const WebSocketError Error)
{
	if (OutError != nullptr)
	{
		*OutError = Error;
	}
}

bool ValidateCompleteFrame(const WebSocketFrame& Frame, WebSocketError* OutError)
{
	const std::uint8_t OpcodeValue = static_cast<std::uint8_t>(Frame.Opcode);
	if (!IsKnownOpcode(OpcodeValue))
	{
		SetError(OutError, WebSocketError::UnknownOpcode);
		return false;
	}
	if (IsControlOpcode(Frame.Opcode) && (!Frame.Final || Frame.Payload.size() > 125))
	{
		SetError(OutError, WebSocketError::InvalidControlFrame);
		return false;
	}
	if (Frame.Opcode == WebSocketOpcode::Text && Frame.Final &&
	    !IsValidWebSocketUtf8({ Frame.Payload.data(), Frame.Payload.size() }))
	{
		SetError(OutError, WebSocketError::InvalidUtf8);
		return false;
	}
	if (Frame.Opcode == WebSocketOpcode::Close)
	{
		if (Frame.Payload.size() == 1)
		{
			SetError(OutError, WebSocketError::InvalidClosePayload);
			return false;
		}
		if (Frame.Payload.size() >= 2)
		{
			const std::uint16_t Code = (static_cast<std::uint16_t>(Frame.Payload[0]) << 8) | Frame.Payload[1];
			if (!IsValidWebSocketCloseCode(Code) ||
			    !IsValidWebSocketUtf8({ Frame.Payload.data() + 2, Frame.Payload.size() - 2 }))
			{
				SetError(OutError, WebSocketError::InvalidClosePayload);
				return false;
			}
		}
	}
	return true;
}
}    // namespace

WebSocketDecoder::WebSocketDecoder(const WebSocketRole LocalRole, const WebSocketLimits InLimits)
	: Role(LocalRole)
	, Limits(InLimits)
{
}

bool WebSocketDecoder::Consume(const ByteView Bytes)
{
	if (Error != WebSocketError::None)
	{
		return false;
	}
	if (Bytes.Size > 0 && Bytes.Data == nullptr)
	{
		return Fail(WebSocketError::InvalidInput);
	}

	constexpr std::size_t MaximumHeaderBytes = 14;
	const std::size_t MaximumBufferedBytes =
		Limits.MaximumFramePayloadBytes > std::numeric_limits<std::size_t>::max() - MaximumHeaderBytes
			? std::numeric_limits<std::size_t>::max()
			: static_cast<std::size_t>(Limits.MaximumFramePayloadBytes) + MaximumHeaderBytes;
	std::size_t Offset = 0;
	while (Offset < Bytes.Size)
	{
		if (!ParseAvailable())
		{
			return false;
		}
		if (Input.size() >= MaximumBufferedBytes)
		{
			return Fail(WebSocketError::FrameTooLarge);
		}

		const std::size_t Available = MaximumBufferedBytes - Input.size();
		const std::size_t ChunkSize = std::min(Available, Bytes.Size - Offset);
		Input.insert(Input.end(), Bytes.Data + Offset, Bytes.Data + Offset + ChunkSize);
		Offset += ChunkSize;
	}
	return ParseAvailable();
}

bool WebSocketDecoder::Drain(WebSocketFrame& OutFrame)
{
	if (Frames.empty())
	{
		return false;
	}
	OutFrame = std::move(Frames.front());
	Frames.pop_front();
	return true;
}

bool WebSocketDecoder::ParseAvailable()
{
	std::size_t Offset = 0;
	while (Input.size() - Offset >= 2)
	{
		const std::uint8_t First = Input[Offset];
		const std::uint8_t Second = Input[Offset + 1];
		if ((First & 0x70) != 0)
		{
			return Fail(WebSocketError::InvalidReservedBits);
		}
		const std::uint8_t OpcodeValue = First & 0x0F;
		if (!IsKnownOpcode(OpcodeValue))
		{
			return Fail(WebSocketError::UnknownOpcode);
		}

		const bool Masked = (Second & 0x80) != 0;
		const bool ExpectedMasked = Role == WebSocketRole::Server;
		if (Masked != ExpectedMasked)
		{
			return Fail(WebSocketError::InvalidMask);
		}

		std::uint64_t PayloadSize = Second & 0x7F;
		std::size_t HeaderSize = 2;
		if (PayloadSize == 126)
		{
			if (Input.size() - Offset < HeaderSize + 2)
			{
				break;
			}
			PayloadSize = (static_cast<std::uint64_t>(Input[Offset + 2]) << 8) | Input[Offset + 3];
			HeaderSize += 2;
			if (PayloadSize < 126)
			{
				return Fail(WebSocketError::NonMinimalLength);
			}
		}
		else if (PayloadSize == 127)
		{
			if (Input.size() - Offset < HeaderSize + 8)
			{
				break;
			}
			if ((Input[Offset + 2] & 0x80) != 0)
			{
				return Fail(WebSocketError::FrameTooLarge);
			}
			PayloadSize = 0;
			for (std::size_t Index = 0; Index < 8; ++Index)
			{
				PayloadSize = (PayloadSize << 8) | Input[Offset + 2 + Index];
			}
			HeaderSize += 8;
			if (PayloadSize <= std::numeric_limits<std::uint16_t>::max())
			{
				return Fail(WebSocketError::NonMinimalLength);
			}
		}

		if (PayloadSize > Limits.MaximumFramePayloadBytes || PayloadSize > std::numeric_limits<std::size_t>::max())
		{
			return Fail(WebSocketError::FrameTooLarge);
		}
		const WebSocketOpcode Opcode = static_cast<WebSocketOpcode>(OpcodeValue);
		const bool Final = (First & 0x80) != 0;
		if (IsControlOpcode(Opcode) && (!Final || PayloadSize > 125))
		{
			return Fail(WebSocketError::InvalidControlFrame);
		}

		std::array<std::uint8_t, 4> Mask{};
		if (Masked)
		{
			if (Input.size() - Offset < HeaderSize + Mask.size())
			{
				break;
			}
			std::copy_n(Input.data() + Offset + HeaderSize, Mask.size(), Mask.data());
			HeaderSize += Mask.size();
		}
		const std::size_t PayloadBytes = static_cast<std::size_t>(PayloadSize);
		if (PayloadBytes > Input.size() - Offset - HeaderSize)
		{
			break;
		}

		WebSocketFrame Frame;
		Frame.Opcode = Opcode;
		Frame.Final = Final;
		const std::uint8_t* PayloadStart = Input.data() + Offset + HeaderSize;
		Frame.Payload.assign(PayloadStart, PayloadStart + PayloadBytes);
		if (Masked)
		{
			for (std::size_t Index = 0; Index < Frame.Payload.size(); ++Index)
			{
				Frame.Payload[Index] ^= Mask[Index % Mask.size()];
			}
		}
		if (!ValidateFrame(Frame))
		{
			return false;
		}
		Frames.push_back(std::move(Frame));
		Offset += HeaderSize + PayloadBytes;
	}

	if (Offset > 0)
	{
		Input.erase(Input.begin(), Input.begin() + static_cast<std::ptrdiff_t>(Offset));
	}
	return true;
}

bool WebSocketDecoder::ValidateFrame(const WebSocketFrame& Frame)
{
	WebSocketError FrameError = WebSocketError::None;
	if (!ValidateCompleteFrame(Frame, &FrameError))
	{
		return Fail(FrameError);
	}

	if (Frame.Opcode == WebSocketOpcode::Continuation)
	{
		if (FragmentedOpcode == WebSocketOpcode::Continuation)
		{
			return Fail(WebSocketError::InvalidFragmentSequence);
		}
		if (Frame.Payload.size() > Limits.MaximumMessagePayloadBytes - FragmentedMessageBytes)
		{
			return Fail(WebSocketError::MessageTooLarge);
		}
		FragmentedMessageBytes += Frame.Payload.size();
		if (FragmentedOpcode == WebSocketOpcode::Text)
		{
			FragmentedText.insert(FragmentedText.end(), Frame.Payload.begin(), Frame.Payload.end());
		}
		if (Frame.Final)
		{
			if (FragmentedOpcode == WebSocketOpcode::Text &&
			    !IsValidWebSocketUtf8({ FragmentedText.data(), FragmentedText.size() }))
			{
				return Fail(WebSocketError::InvalidUtf8);
			}
			FragmentedOpcode = WebSocketOpcode::Continuation;
			FragmentedMessageBytes = 0;
			FragmentedText.clear();
		}
		return true;
	}

	if (Frame.Opcode == WebSocketOpcode::Text || Frame.Opcode == WebSocketOpcode::Binary)
	{
		if (FragmentedOpcode != WebSocketOpcode::Continuation)
		{
			return Fail(WebSocketError::InvalidFragmentSequence);
		}
		if (Frame.Payload.size() > Limits.MaximumMessagePayloadBytes)
		{
			return Fail(WebSocketError::MessageTooLarge);
		}
		if (!Frame.Final)
		{
			FragmentedOpcode = Frame.Opcode;
			FragmentedMessageBytes = Frame.Payload.size();
			if (Frame.Opcode == WebSocketOpcode::Text)
			{
				FragmentedText = Frame.Payload;
			}
		}
	}
	return true;
}

bool WebSocketDecoder::Fail(const WebSocketError InError)
{
	Error = InError;
	return false;
}

const char* WebSocketDecoder::GetErrorText() const
{
	switch (Error)
	{
		case WebSocketError::None: return "none";
		case WebSocketError::InvalidInput: return "input data is null";
		case WebSocketError::InvalidReservedBits: return "reserved bits are set";
		case WebSocketError::UnknownOpcode: return "unknown opcode";
		case WebSocketError::InvalidMask: return "invalid masking for peer role";
		case WebSocketError::NonMinimalLength: return "payload length is not minimally encoded";
		case WebSocketError::FrameTooLarge: return "frame payload exceeds the limit";
		case WebSocketError::MessageTooLarge: return "message payload exceeds the limit";
		case WebSocketError::InvalidControlFrame: return "invalid control frame";
		case WebSocketError::InvalidFragmentSequence: return "invalid fragmentation sequence";
		case WebSocketError::InvalidUtf8: return "invalid UTF-8 text";
		case WebSocketError::InvalidClosePayload: return "invalid close payload";
	}
	return "unknown WebSocket error";
}

bool EncodeWebSocketFrame(const WebSocketFrame& Frame,
	                      const WebSocketRole SenderRole,
	                      const std::uint32_t MaskKey,
	                      std::vector<std::uint8_t>& OutBytes,
	                      WebSocketError* OutError)
{
	SetError(OutError, WebSocketError::None);
	WebSocketError ValidationError = WebSocketError::None;
	if (!ValidateCompleteFrame(Frame, &ValidationError))
	{
		SetError(OutError, ValidationError);
		return false;
	}

	OutBytes.clear();
	const bool Masked = SenderRole == WebSocketRole::Client;
	OutBytes.push_back((Frame.Final ? 0x80 : 0x00) | static_cast<std::uint8_t>(Frame.Opcode));
	const std::uint64_t PayloadSize = Frame.Payload.size();
	const std::uint8_t MaskFlag = Masked ? 0x80 : 0x00;
	if (PayloadSize < 126)
	{
		OutBytes.push_back(MaskFlag | static_cast<std::uint8_t>(PayloadSize));
	}
	else if (PayloadSize <= std::numeric_limits<std::uint16_t>::max())
	{
		OutBytes.push_back(MaskFlag | 126);
		OutBytes.push_back(static_cast<std::uint8_t>((PayloadSize >> 8) & 0xFF));
		OutBytes.push_back(static_cast<std::uint8_t>(PayloadSize & 0xFF));
	}
	else
	{
		OutBytes.push_back(MaskFlag | 127);
		for (int Shift = 56; Shift >= 0; Shift -= 8)
		{
			OutBytes.push_back(static_cast<std::uint8_t>((PayloadSize >> Shift) & 0xFF));
		}
	}

	const std::array<std::uint8_t, 4> Mask = {
		static_cast<std::uint8_t>((MaskKey >> 24) & 0xFF),
		static_cast<std::uint8_t>((MaskKey >> 16) & 0xFF),
		static_cast<std::uint8_t>((MaskKey >> 8) & 0xFF),
		static_cast<std::uint8_t>(MaskKey & 0xFF)
	};
	if (Masked)
	{
		OutBytes.insert(OutBytes.end(), Mask.begin(), Mask.end());
	}
	const std::size_t PayloadOffset = OutBytes.size();
	OutBytes.insert(OutBytes.end(), Frame.Payload.begin(), Frame.Payload.end());
	if (Masked)
	{
		for (std::size_t Index = 0; Index < Frame.Payload.size(); ++Index)
		{
			OutBytes[PayloadOffset + Index] ^= Mask[Index % Mask.size()];
		}
	}
	return true;
}

bool IsValidWebSocketUtf8(const ByteView Bytes)
{
	if (Bytes.Size > 0 && Bytes.Data == nullptr)
	{
		return false;
	}
	std::size_t Index = 0;
	while (Index < Bytes.Size)
	{
		const std::uint8_t First = Bytes.Data[Index++];
		if (First <= 0x7F)
		{
			continue;
		}

		std::uint32_t CodePoint = 0;
		std::size_t Continuations = 0;
		std::uint32_t Minimum = 0;
		if (First >= 0xC2 && First <= 0xDF)
		{
			CodePoint = First & 0x1F;
			Continuations = 1;
			Minimum = 0x80;
		}
		else if (First >= 0xE0 && First <= 0xEF)
		{
			CodePoint = First & 0x0F;
			Continuations = 2;
			Minimum = 0x800;
		}
		else if (First >= 0xF0 && First <= 0xF4)
		{
			CodePoint = First & 0x07;
			Continuations = 3;
			Minimum = 0x10000;
		}
		else
		{
			return false;
		}
		if (Continuations > Bytes.Size - Index)
		{
			return false;
		}
		for (std::size_t Continuation = 0; Continuation < Continuations; ++Continuation)
		{
			const std::uint8_t Value = Bytes.Data[Index++];
			if ((Value & 0xC0) != 0x80)
			{
				return false;
			}
			CodePoint = (CodePoint << 6) | (Value & 0x3F);
		}
		if (CodePoint < Minimum || CodePoint > 0x10FFFF || (CodePoint >= 0xD800 && CodePoint <= 0xDFFF))
		{
			return false;
		}
	}
	return true;
}

bool IsValidWebSocketCloseCode(const std::uint16_t Code)
{
	if (Code >= 3000 && Code <= 4999)
	{
		return true;
	}
	switch (Code)
	{
		case 1000:
		case 1001:
		case 1002:
		case 1003:
		case 1007:
		case 1008:
		case 1009:
		case 1010:
		case 1011:
		// Registered close codes which some older Autobahn cases still treat as reserved.
		case 1012:
		case 1013:
		case 1014:
			return true;
		default:
			return false;
	}
}
}    // namespace DeviceExplorer::Wire
