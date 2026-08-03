#include "DeviceExplorerHttpUpgrade.h"
#include "DeviceExplorerProtocol.h"
#include "DeviceExplorerWebSocket.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using namespace DeviceExplorer::Wire;

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

std::vector<std::uint8_t> Bytes(const std::string& Text)
{
	return { Text.begin(), Text.end() };
}

void TestHttpUpgrade()
{
	CHECK(DeviceExplorer::DeviceProtocolVersion == 9);
	const std::string Key = "dGhlIHNhbXBsZSBub25jZQ==";
	std::string Accept;
	CHECK(MakeWebSocketAccept(Key, Accept));
	CHECK(Accept == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
	CHECK(!MakeWebSocketAccept("not-base64", Accept));

	const std::string RequestBytes = SerializeWebSocketUpgradeRequest(
		"/device/connect?token=golden", "127.0.0.1:18081", Key, { { "X-Test", "one" } });
	CHECK(!RequestBytes.empty());
	for (std::size_t Prefix = 0; Prefix + 1 < RequestBytes.size(); ++Prefix)
	{
		WebSocketUpgradeRequest Request;
		const HttpUpgradeParseResult Result = ParseWebSocketUpgradeRequest(
			{ reinterpret_cast<const std::uint8_t*>(RequestBytes.data()), Prefix }, Request);
		CHECK(Result.Status == HttpUpgradeStatus::NeedMoreData);
	}

	WebSocketUpgradeRequest Request;
	const HttpUpgradeParseResult RequestResult = ParseWebSocketUpgradeRequest(
		{ reinterpret_cast<const std::uint8_t*>(RequestBytes.data()), RequestBytes.size() }, Request);
	CHECK(RequestResult.Status == HttpUpgradeStatus::Complete);
	CHECK(RequestResult.ConsumedBytes == RequestBytes.size());
	CHECK(Request.Target == "/device/connect?token=golden");
	CHECK(Request.Host == "127.0.0.1:18081");
	CHECK(Request.Key == Key);

	CHECK(MakeWebSocketAccept(Key, Accept));
	const std::string ResponseBytes = SerializeWebSocketUpgradeResponse(Accept);
	WebSocketUpgradeResponse Response;
	const HttpUpgradeParseResult ResponseResult = ParseWebSocketUpgradeResponse(
		{ reinterpret_cast<const std::uint8_t*>(ResponseBytes.data()), ResponseBytes.size() }, Accept, Response);
	CHECK(ResponseResult.Status == HttpUpgradeStatus::Complete);
	CHECK(Response.Accept == Accept);

	std::string BadRequest = RequestBytes;
	const std::size_t Version = BadRequest.find("Sec-WebSocket-Version: 13");
	CHECK(Version != std::string::npos);
	BadRequest.replace(Version, std::string("Sec-WebSocket-Version: 13").size(), "Sec-WebSocket-Version: 12");
	const HttpUpgradeParseResult BadResult = ParseWebSocketUpgradeRequest(
		{ reinterpret_cast<const std::uint8_t*>(BadRequest.data()), BadRequest.size() }, Request);
	CHECK(BadResult.Status == HttpUpgradeStatus::Error);
	CHECK(BadResult.Error == HttpUpgradeError::UnsupportedWebSocketVersion);
}

void TestWebSocketRoundTrip()
{
	WebSocketFrame Source;
	Source.Opcode = WebSocketOpcode::Text;
	Source.Payload.assign(130, static_cast<std::uint8_t>('x'));
	std::vector<std::uint8_t> Encoded;
	CHECK(EncodeWebSocketFrame(Source, WebSocketRole::Client, 0x12345678, Encoded));
	CHECK(Encoded.size() == Source.Payload.size() + 8);
	CHECK(Encoded[1] == (0x80 | 126));

	WebSocketDecoder ServerDecoder(WebSocketRole::Server);
	for (const std::uint8_t Value : Encoded)
	{
		CHECK(ServerDecoder.Consume({ &Value, 1 }));
	}
	WebSocketFrame Decoded;
	CHECK(ServerDecoder.Drain(Decoded));
	CHECK(Decoded.Opcode == WebSocketOpcode::Text);
	CHECK(Decoded.Final);
	CHECK(Decoded.Payload == Source.Payload);
	CHECK(!ServerDecoder.Drain(Decoded));

	WebSocketFrame Pong;
	Pong.Opcode = WebSocketOpcode::Pong;
	Pong.Payload = Bytes("ping-data");
	CHECK(EncodeWebSocketFrame(Pong, WebSocketRole::Server, 0, Encoded));
	WebSocketDecoder ClientDecoder(WebSocketRole::Client);
	CHECK(ClientDecoder.Consume({ Encoded.data(), Encoded.size() }));
	CHECK(ClientDecoder.Drain(Decoded));
	CHECK(Decoded.Opcode == WebSocketOpcode::Pong);
	CHECK(Decoded.Payload == Pong.Payload);
}

void TestWebSocketLengthBoundaries()
{
	for (const std::size_t PayloadSize : { 0U, 1U, 125U, 126U, 65535U, 65536U })
	{
		WebSocketFrame Source;
		Source.Opcode = WebSocketOpcode::Binary;
		Source.Payload.resize(PayloadSize);
		for (std::size_t Index = 0; Index < Source.Payload.size(); ++Index)
		{
			Source.Payload[Index] = static_cast<std::uint8_t>(Index & 0xFF);
		}
		std::vector<std::uint8_t> Encoded;
		CHECK(EncodeWebSocketFrame(Source, WebSocketRole::Client, 0x89ABCDEF, Encoded));
		WebSocketDecoder Decoder(WebSocketRole::Server);
		std::size_t Offset = 0;
		while (Offset < Encoded.size())
		{
			const std::size_t Chunk = std::min<std::size_t>(17, Encoded.size() - Offset);
			CHECK(Decoder.Consume({ Encoded.data() + Offset, Chunk }));
			Offset += Chunk;
		}
		WebSocketFrame Decoded;
		CHECK(Decoder.Drain(Decoded));
		CHECK(Decoded.Payload == Source.Payload);
	}
}

void TestFragmentationAndUtf8()
{
	WebSocketFrame First;
	First.Opcode = WebSocketOpcode::Text;
	First.Final = false;
	First.Payload = { 'a', 0xE2 };
	WebSocketFrame Ping;
	Ping.Opcode = WebSocketOpcode::Ping;
	Ping.Payload = Bytes("p");
	WebSocketFrame Last;
	Last.Opcode = WebSocketOpcode::Continuation;
	Last.Payload = { 0x82, 0xAC };

	std::vector<std::uint8_t> Stream;
	for (const WebSocketFrame* Frame : { &First, &Ping, &Last })
	{
		std::vector<std::uint8_t> Encoded;
		CHECK(EncodeWebSocketFrame(*Frame, WebSocketRole::Client, 0x01020304, Encoded));
		Stream.insert(Stream.end(), Encoded.begin(), Encoded.end());
	}
	WebSocketDecoder Decoder(WebSocketRole::Server);
	CHECK(Decoder.Consume({ Stream.data(), Stream.size() }));
	WebSocketFrame Decoded;
	CHECK(Decoder.Drain(Decoded) && Decoded.Opcode == WebSocketOpcode::Text);
	CHECK(Decoder.Drain(Decoded) && Decoded.Opcode == WebSocketOpcode::Ping);
	CHECK(Decoder.Drain(Decoded) && Decoded.Opcode == WebSocketOpcode::Continuation);

	WebSocketFrame InvalidText;
	InvalidText.Opcode = WebSocketOpcode::Text;
	InvalidText.Payload = { 0xC0, 0x80 };
	std::vector<std::uint8_t> Ignored;
	WebSocketError Error = WebSocketError::None;
	CHECK(!EncodeWebSocketFrame(InvalidText, WebSocketRole::Client, 0, Ignored, &Error));
	CHECK(Error == WebSocketError::InvalidUtf8);
}

void TestInvalidFrames()
{
	WebSocketDecoder ServerDecoder(WebSocketRole::Server);
	const std::uint8_t Unmasked[] = { 0x81, 0x00 };
	CHECK(!ServerDecoder.Consume({ Unmasked, sizeof(Unmasked) }));
	CHECK(ServerDecoder.GetError() == WebSocketError::InvalidMask);

	const std::uint8_t Rsv[] = { 0xC1, 0x80, 0, 0, 0, 0 };
	WebSocketDecoder RsvDecoder(WebSocketRole::Server);
	CHECK(!RsvDecoder.Consume({ Rsv, sizeof(Rsv) }));
	CHECK(RsvDecoder.GetError() == WebSocketError::InvalidReservedBits);

	const std::uint8_t UnknownOpcode[] = { 0x83, 0x80, 0, 0, 0, 0 };
	WebSocketDecoder OpcodeDecoder(WebSocketRole::Server);
	CHECK(!OpcodeDecoder.Consume({ UnknownOpcode, sizeof(UnknownOpcode) }));
	CHECK(OpcodeDecoder.GetError() == WebSocketError::UnknownOpcode);

	const std::uint8_t NonMinimal[] = { 0x81, 0xFE, 0, 125 };
	WebSocketDecoder LengthDecoder(WebSocketRole::Server);
	CHECK(!LengthDecoder.Consume({ NonMinimal, sizeof(NonMinimal) }));
	CHECK(LengthDecoder.GetError() == WebSocketError::NonMinimalLength);

	const std::uint8_t Invalid64BitLength[] = { 0x82, 0xFF, 0x80, 0, 0, 0, 0, 0, 0, 0 };
	WebSocketDecoder HighBitDecoder(WebSocketRole::Server);
	CHECK(!HighBitDecoder.Consume({ Invalid64BitLength, sizeof(Invalid64BitLength) }));
	CHECK(HighBitDecoder.GetError() == WebSocketError::FrameTooLarge);

	const std::uint8_t FragmentedPing[] = { 0x09, 0x80, 0, 0, 0, 0 };
	WebSocketDecoder ControlDecoder(WebSocketRole::Server);
	CHECK(!ControlDecoder.Consume({ FragmentedPing, sizeof(FragmentedPing) }));
	CHECK(ControlDecoder.GetError() == WebSocketError::InvalidControlFrame);

	WebSocketFrame BadClose;
	BadClose.Opcode = WebSocketOpcode::Close;
	BadClose.Payload = { 0x03 };
	std::vector<std::uint8_t> Encoded;
	WebSocketError Error = WebSocketError::None;
	CHECK(!EncodeWebSocketFrame(BadClose, WebSocketRole::Server, 0, Encoded, &Error));
	CHECK(Error == WebSocketError::InvalidClosePayload);

	WebSocketLimits Limits;
	Limits.MaximumFramePayloadBytes = 16;
	Limits.MaximumMessagePayloadBytes = 3;
	WebSocketFrame TooLarge;
	TooLarge.Opcode = WebSocketOpcode::Binary;
	TooLarge.Payload = { 1, 2, 3, 4 };
	CHECK(EncodeWebSocketFrame(TooLarge, WebSocketRole::Client, 0, Encoded));
	WebSocketDecoder LimitedDecoder(WebSocketRole::Server, Limits);
	CHECK(!LimitedDecoder.Consume({ Encoded.data(), Encoded.size() }));
	CHECK(LimitedDecoder.GetError() == WebSocketError::MessageTooLarge);
}
}    // namespace

int main()
{
	TestHttpUpgrade();
	TestWebSocketRoundTrip();
	TestWebSocketLengthBoundaries();
	TestFragmentationAndUtf8();
	TestInvalidFrames();
	if (Failures != 0)
	{
		std::cerr << Failures << " test(s) failed\n";
		return 1;
	}
	std::cout << "DeviceExplorerWire tests passed\n";
	return 0;
}
