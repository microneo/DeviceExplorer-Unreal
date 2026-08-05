#include "DeviceExplorerHttpUpgrade.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>

namespace DeviceExplorer::Wire
{
namespace
{
constexpr std::string_view WebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr char Base64Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::uint32_t RotateLeft(const std::uint32_t Value, const unsigned Bits)
{
	return (Value << Bits) | (Value >> (32U - Bits));
}

std::array<std::uint8_t, 20> Sha1(const std::string& Input)
{
	std::vector<std::uint8_t> Message(Input.begin(), Input.end());
	const std::uint64_t BitLength = static_cast<std::uint64_t>(Message.size()) * 8ULL;
	Message.push_back(0x80);
	while ((Message.size() % 64) != 56)
	{
		Message.push_back(0);
	}
	for (int Shift = 56; Shift >= 0; Shift -= 8)
	{
		Message.push_back(static_cast<std::uint8_t>((BitLength >> Shift) & 0xFF));
	}

	std::uint32_t H0 = 0x67452301;
	std::uint32_t H1 = 0xEFCDAB89;
	std::uint32_t H2 = 0x98BADCFE;
	std::uint32_t H3 = 0x10325476;
	std::uint32_t H4 = 0xC3D2E1F0;
	for (std::size_t Chunk = 0; Chunk < Message.size(); Chunk += 64)
	{
		std::array<std::uint32_t, 80> Words{};
		for (std::size_t Index = 0; Index < 16; ++Index)
		{
			const std::size_t Offset = Chunk + Index * 4;
			Words[Index] = (static_cast<std::uint32_t>(Message[Offset]) << 24) |
			               (static_cast<std::uint32_t>(Message[Offset + 1]) << 16) |
			               (static_cast<std::uint32_t>(Message[Offset + 2]) << 8) |
			               Message[Offset + 3];
		}
		for (std::size_t Index = 16; Index < Words.size(); ++Index)
		{
			Words[Index] = RotateLeft(Words[Index - 3] ^ Words[Index - 8] ^ Words[Index - 14] ^ Words[Index - 16], 1);
		}

		std::uint32_t A = H0;
		std::uint32_t B = H1;
		std::uint32_t C = H2;
		std::uint32_t D = H3;
		std::uint32_t E = H4;
		for (std::size_t Index = 0; Index < Words.size(); ++Index)
		{
			std::uint32_t Function = 0;
			std::uint32_t Constant = 0;
			if (Index < 20)
			{
				Function = (B & C) | ((~B) & D);
				Constant = 0x5A827999;
			}
			else if (Index < 40)
			{
				Function = B ^ C ^ D;
				Constant = 0x6ED9EBA1;
			}
			else if (Index < 60)
			{
				Function = (B & C) | (B & D) | (C & D);
				Constant = 0x8F1BBCDC;
			}
			else
			{
				Function = B ^ C ^ D;
				Constant = 0xCA62C1D6;
			}
			const std::uint32_t Temporary = RotateLeft(A, 5) + Function + E + Constant + Words[Index];
			E = D;
			D = C;
			C = RotateLeft(B, 30);
			B = A;
			A = Temporary;
		}
		H0 += A;
		H1 += B;
		H2 += C;
		H3 += D;
		H4 += E;
	}

	const std::array<std::uint32_t, 5> HashWords = { H0, H1, H2, H3, H4 };
	std::array<std::uint8_t, 20> Digest{};
	for (std::size_t Index = 0; Index < HashWords.size(); ++Index)
	{
		Digest[Index * 4] = static_cast<std::uint8_t>((HashWords[Index] >> 24) & 0xFF);
		Digest[Index * 4 + 1] = static_cast<std::uint8_t>((HashWords[Index] >> 16) & 0xFF);
		Digest[Index * 4 + 2] = static_cast<std::uint8_t>((HashWords[Index] >> 8) & 0xFF);
		Digest[Index * 4 + 3] = static_cast<std::uint8_t>(HashWords[Index] & 0xFF);
	}
	return Digest;
}

std::string Base64Encode(const ByteView Bytes)
{
	std::string Result;
	Result.reserve(((Bytes.Size + 2) / 3) * 4);
	for (std::size_t Offset = 0; Offset < Bytes.Size; Offset += 3)
	{
		const std::uint32_t A = Bytes.Data[Offset];
		const std::uint32_t B = Offset + 1 < Bytes.Size ? Bytes.Data[Offset + 1] : 0;
		const std::uint32_t C = Offset + 2 < Bytes.Size ? Bytes.Data[Offset + 2] : 0;
		const std::uint32_t Triple = (A << 16) | (B << 8) | C;
		Result.push_back(Base64Alphabet[(Triple >> 18) & 0x3F]);
		Result.push_back(Base64Alphabet[(Triple >> 12) & 0x3F]);
		Result.push_back(Offset + 1 < Bytes.Size ? Base64Alphabet[(Triple >> 6) & 0x3F] : '=');
		Result.push_back(Offset + 2 < Bytes.Size ? Base64Alphabet[Triple & 0x3F] : '=');
	}
	return Result;
}

int Base64Value(const char Character)
{
	if (Character >= 'A' && Character <= 'Z') return Character - 'A';
	if (Character >= 'a' && Character <= 'z') return Character - 'a' + 26;
	if (Character >= '0' && Character <= '9') return Character - '0' + 52;
	if (Character == '+') return 62;
	if (Character == '/') return 63;
	return -1;
}

bool DecodeWebSocketKey(const std::string& Input, std::array<std::uint8_t, 16>& OutBytes)
{
	if (Input.size() != 24 || Input[22] != '=' || Input[23] != '=')
	{
		return false;
	}
	std::size_t OutputOffset = 0;
	for (std::size_t Offset = 0; Offset < Input.size(); Offset += 4)
	{
		const int A = Base64Value(Input[Offset]);
		const int B = Base64Value(Input[Offset + 1]);
		const int C = Input[Offset + 2] == '=' ? 0 : Base64Value(Input[Offset + 2]);
		const int D = Input[Offset + 3] == '=' ? 0 : Base64Value(Input[Offset + 3]);
		if (A < 0 || B < 0 || C < 0 || D < 0)
		{
			return false;
		}
		if (Offset + 4 == Input.size() && (B & 0x0F) != 0)
		{
			return false;
		}
		const std::uint32_t Triple = (static_cast<std::uint32_t>(A) << 18) |
		                             (static_cast<std::uint32_t>(B) << 12) |
		                             (static_cast<std::uint32_t>(C) << 6) |
		                             static_cast<std::uint32_t>(D);
		if (OutputOffset < OutBytes.size()) OutBytes[OutputOffset++] = static_cast<std::uint8_t>((Triple >> 16) & 0xFF);
		if (Input[Offset + 2] != '=' && OutputOffset < OutBytes.size()) OutBytes[OutputOffset++] = static_cast<std::uint8_t>((Triple >> 8) & 0xFF);
		if (Input[Offset + 3] != '=' && OutputOffset < OutBytes.size()) OutBytes[OutputOffset++] = static_cast<std::uint8_t>(Triple & 0xFF);
	}
	return OutputOffset == OutBytes.size();
}

char LowerAscii(const char Character)
{
	return Character >= 'A' && Character <= 'Z' ? static_cast<char>(Character - 'A' + 'a') : Character;
}

bool EqualsIgnoreCase(const std::string_view A, const std::string_view B)
{
	if (A.size() != B.size())
	{
		return false;
	}
	for (std::size_t Index = 0; Index < A.size(); ++Index)
	{
		if (LowerAscii(A[Index]) != LowerAscii(B[Index]))
		{
			return false;
		}
	}
	return true;
}

std::string_view Trim(const std::string_view Value)
{
	std::size_t Begin = 0;
	std::size_t End = Value.size();
	while (Begin < End && (Value[Begin] == ' ' || Value[Begin] == '\t')) ++Begin;
	while (End > Begin && (Value[End - 1] == ' ' || Value[End - 1] == '\t')) --End;
	return Value.substr(Begin, End - Begin);
}

bool IsValidHeaderName(const std::string_view Name)
{
	if (Name.empty()) return false;
	for (const char Character : Name)
	{
		const unsigned char Value = static_cast<unsigned char>(Character);
		if (Value <= 32 || Value >= 127 || Character == ':' || Character == '(' || Character == ')' ||
		    Character == '<' || Character == '>' || Character == '@' || Character == ',' || Character == ';' ||
		    Character == '\\' || Character == '"' || Character == '/' || Character == '[' || Character == ']' ||
		    Character == '?' || Character == '=' || Character == '{' || Character == '}')
		{
			return false;
		}
	}
	return true;
}

bool IsValidHeaderValue(const std::string_view Value)
{
	for (const char Character : Value)
	{
		if (Character == '\r' || Character == '\n' || Character == '\0')
		{
			return false;
		}
	}
	return true;
}

const std::string* FindHeader(const std::vector<HttpHeader>& Headers, const std::string_view Name)
{
	for (const HttpHeader& Header : Headers)
	{
		if (EqualsIgnoreCase(Header.first, Name))
		{
			return &Header.second;
		}
	}
	return nullptr;
}

std::size_t CountHeaders(const std::vector<HttpHeader>& Headers, const std::string_view Name)
{
	return static_cast<std::size_t>(std::count_if(
		Headers.begin(),
		Headers.end(),
		[Name](const HttpHeader& Header) { return EqualsIgnoreCase(Header.first, Name); }));
}

bool ContainsHeaderToken(const std::string_view Value, const std::string_view Expected)
{
	std::size_t Offset = 0;
	while (Offset <= Value.size())
	{
		const std::size_t Comma = Value.find(',', Offset);
		const std::size_t End = Comma == std::string::npos ? Value.size() : Comma;
		if (EqualsIgnoreCase(Trim(std::string_view(Value).substr(Offset, End - Offset)), Expected))
		{
			return true;
		}
		if (Comma == std::string::npos) break;
		Offset = Comma + 1;
	}
	return false;
}

bool HeadersContainToken(const std::vector<HttpHeader>& Headers,
	                     const std::string_view Name,
	                     const std::string_view Expected)
{
	for (const HttpHeader& Header : Headers)
	{
		if (EqualsIgnoreCase(Header.first, Name) && ContainsHeaderToken(Header.second, Expected))
		{
			return true;
		}
	}
	return false;
}

std::size_t FindHeaderEnd(const ByteView Bytes)
{
	if (Bytes.Data == nullptr) return std::string::npos;
	for (std::size_t Index = 3; Index < Bytes.Size; ++Index)
	{
		if (Bytes.Data[Index - 3] == '\r' && Bytes.Data[Index - 2] == '\n' &&
		    Bytes.Data[Index - 1] == '\r' && Bytes.Data[Index] == '\n')
		{
			return Index + 1;
		}
	}
	return std::string::npos;
}

bool SplitHeaderLines(const ByteView Bytes,
	                  const std::size_t HeaderEnd,
	                  std::vector<std::string_view>& OutLines)
{
	const std::string_view Text(reinterpret_cast<const char*>(Bytes.Data), HeaderEnd);
	std::size_t Offset = 0;
	while (Offset < Text.size())
	{
		const std::size_t End = Text.find("\r\n", Offset);
		if (End == std::string_view::npos)
		{
			return false;
		}
		if (End == Offset)
		{
			return End + 2 == Text.size();
		}
		OutLines.push_back(Text.substr(Offset, End - Offset));
		Offset = End + 2;
	}
	return false;
}

bool ParseHeaders(const std::vector<std::string_view>& Lines, std::vector<HttpHeader>& OutHeaders)
{
	for (std::size_t Index = 1; Index < Lines.size(); ++Index)
	{
		const std::size_t Colon = Lines[Index].find(':');
		if (Colon == std::string_view::npos)
		{
			return false;
		}
		const std::string_view Name = Lines[Index].substr(0, Colon);
		const std::string_view Value = Trim(Lines[Index].substr(Colon + 1));
		if (!IsValidHeaderName(Name) || !IsValidHeaderValue(Value))
		{
			return false;
		}
		OutHeaders.emplace_back(std::string(Name), std::string(Value));
	}
	return true;
}

HttpUpgradeParseResult ErrorResult(const HttpUpgradeError Error)
{
	return { HttpUpgradeStatus::Error, Error, 0 };
}

HttpUpgradeParseResult BeginParse(const ByteView Bytes,
	                              const std::size_t MaximumHeaderBytes,
	                              std::size_t& OutHeaderEnd,
	                              std::vector<std::string_view>& OutLines)
{
	OutHeaderEnd = FindHeaderEnd(Bytes);
	if (OutHeaderEnd == std::string::npos)
	{
		if (Bytes.Size >= MaximumHeaderBytes)
		{
			return ErrorResult(HttpUpgradeError::HeaderTooLarge);
		}
		return { HttpUpgradeStatus::NeedMoreData, HttpUpgradeError::None, 0 };
	}
	if (OutHeaderEnd > MaximumHeaderBytes)
	{
		return ErrorResult(HttpUpgradeError::HeaderTooLarge);
	}
	if (!SplitHeaderLines(Bytes, OutHeaderEnd, OutLines) || OutLines.empty())
	{
		return ErrorResult(HttpUpgradeError::InvalidHeader);
	}
	return { HttpUpgradeStatus::Complete, HttpUpgradeError::None, OutHeaderEnd };
}

bool AppendHeader(std::string& Out, const HttpHeader& Header)
{
	if (!IsValidHeaderName(Header.first) || !IsValidHeaderValue(Header.second))
	{
		return false;
	}
	Out += Header.first;
	Out += ": ";
	Out += Header.second;
	Out += "\r\n";
	return true;
}
}    // namespace

HttpUpgradeParseResult ParseWebSocketUpgradeRequest(const ByteView Bytes,
	                                                WebSocketUpgradeRequest& OutRequest,
	                                                const std::size_t MaximumHeaderBytes)
{
	std::size_t HeaderEnd = 0;
	std::vector<std::string_view> Lines;
	HttpUpgradeParseResult Result = BeginParse(Bytes, MaximumHeaderBytes, HeaderEnd, Lines);
	if (Result.Status != HttpUpgradeStatus::Complete)
	{
		return Result;
	}

	const std::string_view Start = Lines[0];
	const std::size_t FirstSpace = Start.find(' ');
	const std::size_t LastSpace = Start.rfind(' ');
	if (FirstSpace == std::string_view::npos || LastSpace == FirstSpace ||
	    Start.substr(0, FirstSpace) != "GET" || Start.substr(LastSpace + 1) != "HTTP/1.1")
	{
		return ErrorResult(HttpUpgradeError::InvalidStartLine);
	}

	WebSocketUpgradeRequest Parsed;
	Parsed.Target = std::string(Start.substr(FirstSpace + 1, LastSpace - FirstSpace - 1));
	if (Parsed.Target.empty() || !ParseHeaders(Lines, Parsed.Headers))
	{
		return ErrorResult(HttpUpgradeError::InvalidHeader);
	}
	const std::string* Key = FindHeader(Parsed.Headers, "sec-websocket-key");
	const std::string* Version = FindHeader(Parsed.Headers, "sec-websocket-version");
	const std::string* Host = FindHeader(Parsed.Headers, "host");
	if (!HeadersContainToken(Parsed.Headers, "upgrade", "websocket"))
	{
		return ErrorResult(HttpUpgradeError::InvalidUpgrade);
	}
	if (!HeadersContainToken(Parsed.Headers, "connection", "upgrade"))
	{
		return ErrorResult(HttpUpgradeError::InvalidConnection);
	}
	if (Host == nullptr || CountHeaders(Parsed.Headers, "host") != 1 || Trim(*Host).empty())
	{
		return ErrorResult(HttpUpgradeError::InvalidHost);
	}
	if (Key == nullptr)
	{
		return ErrorResult(HttpUpgradeError::MissingWebSocketKey);
	}
	if (CountHeaders(Parsed.Headers, "sec-websocket-key") != 1)
	{
		return ErrorResult(HttpUpgradeError::InvalidWebSocketKey);
	}
	std::array<std::uint8_t, 16> DecodedKey{};
	if (!DecodeWebSocketKey(*Key, DecodedKey))
	{
		return ErrorResult(HttpUpgradeError::InvalidWebSocketKey);
	}
	if (Version == nullptr || CountHeaders(Parsed.Headers, "sec-websocket-version") != 1 || Trim(*Version) != "13")
	{
		return ErrorResult(HttpUpgradeError::UnsupportedWebSocketVersion);
	}
	Parsed.Key = *Key;
	Parsed.Host = *Host;
	OutRequest = std::move(Parsed);
	return { HttpUpgradeStatus::Complete, HttpUpgradeError::None, HeaderEnd };
}

HttpUpgradeParseResult ParseWebSocketUpgradeResponse(const ByteView Bytes,
	                                                 const std::string& ExpectedAccept,
	                                                 WebSocketUpgradeResponse& OutResponse,
	                                                 const std::size_t MaximumHeaderBytes)
{
	std::size_t HeaderEnd = 0;
	std::vector<std::string_view> Lines;
	HttpUpgradeParseResult Result = BeginParse(Bytes, MaximumHeaderBytes, HeaderEnd, Lines);
	if (Result.Status != HttpUpgradeStatus::Complete)
	{
		return Result;
	}
	const std::string_view Start = Lines[0];
	if (Start.size() < 12 || Start.substr(0, 9) != "HTTP/1.1 " || Start.substr(9, 3) != "101" ||
	    (Start.size() > 12 && Start[12] != ' '))
	{
		return ErrorResult(HttpUpgradeError::InvalidStartLine);
	}

	WebSocketUpgradeResponse Parsed;
	if (!ParseHeaders(Lines, Parsed.Headers))
	{
		return ErrorResult(HttpUpgradeError::InvalidHeader);
	}
	const std::string* Accept = FindHeader(Parsed.Headers, "sec-websocket-accept");
	const std::string* Upgrade = FindHeader(Parsed.Headers, "upgrade");
	// RFC 6455 section 4.1 requires the server's selected protocol to be exactly
	// websocket. Unlike a request, a 101 response cannot advertise alternatives.
	if (Upgrade == nullptr || CountHeaders(Parsed.Headers, "upgrade") != 1 ||
	    !EqualsIgnoreCase(Trim(*Upgrade), "websocket"))
	{
		return ErrorResult(HttpUpgradeError::InvalidUpgrade);
	}
	if (!HeadersContainToken(Parsed.Headers, "connection", "upgrade"))
	{
		return ErrorResult(HttpUpgradeError::InvalidConnection);
	}
	if (Accept == nullptr || CountHeaders(Parsed.Headers, "sec-websocket-accept") != 1 || *Accept != ExpectedAccept)
	{
		return ErrorResult(HttpUpgradeError::InvalidWebSocketAccept);
	}
	Parsed.Accept = *Accept;
	OutResponse = std::move(Parsed);
	return { HttpUpgradeStatus::Complete, HttpUpgradeError::None, HeaderEnd };
}

bool MakeWebSocketAccept(const std::string& ClientKey, std::string& OutAccept)
{
	std::array<std::uint8_t, 16> DecodedKey{};
	if (!DecodeWebSocketKey(ClientKey, DecodedKey))
	{
		OutAccept.clear();
		return false;
	}
	const std::array<std::uint8_t, 20> Digest = Sha1(ClientKey + std::string(WebSocketGuid));
	OutAccept = Base64Encode({ Digest.data(), Digest.size() });
	return true;
}

bool MakeWebSocketClientKey(const ByteView Nonce, std::string& OutKey)
{
	if (Nonce.Data == nullptr || Nonce.Size != 16)
	{
		OutKey.clear();
		return false;
	}
	OutKey = Base64Encode(Nonce);
	return true;
}

std::string SerializeWebSocketUpgradeRequest(const std::string& Target,
	                                         const std::string& Host,
	                                         const std::string& ClientKey,
	                                         const std::vector<HttpHeader>& ExtraHeaders)
{
	std::array<std::uint8_t, 16> DecodedKey{};
	if (Target.empty() || Target.find_first_of("\r\n ") != std::string::npos ||
	    !IsValidHeaderValue(Host) || !DecodeWebSocketKey(ClientKey, DecodedKey))
	{
		return {};
	}
	std::string Result = "GET " + Target + " HTTP/1.1\r\nHost: " + Host +
	                     "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " +
	                     ClientKey + "\r\nSec-WebSocket-Version: 13\r\n";
	for (const HttpHeader& Header : ExtraHeaders)
	{
		if (!AppendHeader(Result, Header)) return {};
	}
	Result += "\r\n";
	return Result;
}

std::string SerializeWebSocketUpgradeResponse(const std::string& Accept,
	                                          const std::vector<HttpHeader>& ExtraHeaders)
{
	if (!IsValidHeaderValue(Accept) || Accept.empty()) return {};
	std::string Result = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + Accept + "\r\n";
	for (const HttpHeader& Header : ExtraHeaders)
	{
		if (!AppendHeader(Result, Header)) return {};
	}
	Result += "\r\n";
	return Result;
}

const char* HttpUpgradeErrorText(const HttpUpgradeError Error)
{
	switch (Error)
	{
		case HttpUpgradeError::None: return "none";
		case HttpUpgradeError::HeaderTooLarge: return "HTTP header exceeds the limit";
		case HttpUpgradeError::InvalidStartLine: return "invalid HTTP start line";
		case HttpUpgradeError::InvalidHeader: return "invalid HTTP header";
		case HttpUpgradeError::InvalidHost: return "missing, empty, or repeated Host header";
		case HttpUpgradeError::InvalidUpgrade: return "missing or invalid Upgrade header";
		case HttpUpgradeError::InvalidConnection: return "Connection does not contain Upgrade";
		case HttpUpgradeError::MissingWebSocketKey: return "missing Sec-WebSocket-Key";
		case HttpUpgradeError::InvalidWebSocketKey: return "invalid Sec-WebSocket-Key";
		case HttpUpgradeError::UnsupportedWebSocketVersion: return "unsupported WebSocket version";
		case HttpUpgradeError::InvalidWebSocketAccept: return "invalid Sec-WebSocket-Accept";
	}
	return "unknown HTTP upgrade error";
}
}    // namespace DeviceExplorer::Wire
