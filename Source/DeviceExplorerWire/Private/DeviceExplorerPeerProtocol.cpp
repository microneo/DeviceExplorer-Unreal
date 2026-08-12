#include "DeviceExplorerPeerProtocol.h"

#include "DeviceExplorerAuthPrimitives.h"

#include <charconv>
#include <limits>
#include <utility>

namespace DeviceExplorer::Wire
{
namespace
{
void SetError(PeerProtocolError* OutError, const PeerProtocolError Error)
{
	if (OutError != nullptr) *OutError = Error;
}

bool IsBoundedIdentity(const std::string& Value)
{
	return !Value.empty() && Value.size() <= 128;
}

bool IsNonce(const std::string& Value)
{
	if (Value.size() != 32) return false;
	for (const char Character : Value)
	{
		if (!((Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f'))) return false;
	}
	return true;
}

bool IsProof(const std::string& Value)
{
	if (Value.size() != 64) return false;
	for (const char Character : Value)
	{
		if (!((Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f'))) return false;
	}
	return true;
}

const JsonValue* Member(const JsonValue& Object, const std::string_view Name)
{
	return Object.FindMember(Name);
}

bool StringMember(const JsonValue& Object, const std::string_view Name, std::string& OutValue)
{
	const JsonValue* Value = Member(Object, Name);
	const std::string* Text = Value == nullptr ? nullptr : Value->TryGetString();
	if (Text == nullptr) return false;
	OutValue = *Text;
	return true;
}

template <typename Integer>
bool IntegerMember(const JsonValue& Object, const std::string_view Name, Integer& OutValue)
{
	const JsonValue* Value = Member(Object, Name);
	const std::string* Text = Value == nullptr ? nullptr : Value->TryGetNumberText();
	if (Text == nullptr || Text->empty()) return false;
	Integer Parsed{};
	const std::from_chars_result Result = std::from_chars(Text->data(), Text->data() + Text->size(), Parsed);
	if (Result.ec != std::errc{} || Result.ptr != Text->data() + Text->size()) return false;
	OutValue = Parsed;
	return true;
}

void WriteString(JsonValue& Object, std::string Name, std::string Value)
{
	JsonValue Field;
	(void) Field.SetString(std::move(Value));
	(void) Object.InsertMember(std::move(Name), std::move(Field));
}

void WriteSigned(JsonValue& Object, std::string Name, const std::int64_t Value)
{
	JsonValue Field;
	Field.SetSignedInteger(Value);
	(void) Object.InsertMember(std::move(Name), std::move(Field));
}

void WriteUnsigned(JsonValue& Object, std::string Name, const std::uint64_t Value)
{
	JsonValue Field;
	Field.SetUnsignedInteger(Value);
	(void) Object.InsertMember(std::move(Name), std::move(Field));
}

const char* ResultText(const PeerHelloResult Result)
{
	switch (Result)
	{
	case PeerHelloResult::Accepted: return "accepted";
	case PeerHelloResult::StaleSession: return "stale_session";
	case PeerHelloResult::IdentityCollision: return "identity_collision";
	case PeerHelloResult::Rejected: return "rejected";
	}
	return "rejected";
}

bool ParseResult(const std::string_view Text, PeerHelloResult& OutResult)
{
	if (Text == "accepted") OutResult = PeerHelloResult::Accepted;
	else if (Text == "stale_session") OutResult = PeerHelloResult::StaleSession;
	else if (Text == "identity_collision") OutResult = PeerHelloResult::IdentityCollision;
	else if (Text == "rejected") OutResult = PeerHelloResult::Rejected;
	else return false;
	return true;
}

bool ParseObject(const ByteView Bytes, JsonValue& OutRoot, PeerProtocolError* OutError)
{
	JsonLimits Limits;
	Limits.MaximumDocumentBytes = MaximumPeerMessageBytes;
	Limits.MaximumStringBytes = MaximumPeerMessageBytes;
	Limits.MaximumDepth = 8;
	Limits.MaximumNodeCount = 64;
	if (!ParseJson(Bytes, OutRoot, Limits) || OutRoot.GetType() != JsonType::Object)
	{
		SetError(OutError, PeerProtocolError::MalformedJson);
		return false;
	}
	return true;
}

bool ParseHelloObject(const JsonValue& Root, PeerHello& OutHello, PeerProtocolError* OutError)
{
	std::string Type;
	PeerHello Parsed;
	if (!StringMember(Root, "type", Type) || Type != "peer_hello")
	{
		SetError(OutError, PeerProtocolError::WrongMessageType);
		return false;
	}
	if (!StringMember(Root, "cluster_id", Parsed.ClusterId) || !StringMember(Root, "node_id", Parsed.NodeId) ||
	    !IntegerMember(Root, "host_session", Parsed.HostSession) || !StringMember(Root, "instance_id", Parsed.InstanceId) ||
	    !IntegerMember(Root, "protocol_min", Parsed.ProtocolMin) || !IntegerMember(Root, "protocol_max", Parsed.ProtocolMax) ||
	    !StringMember(Root, "connection_nonce", Parsed.ConnectionNonce))
	{
		SetError(OutError, PeerProtocolError::MissingField);
		return false;
	}
	if (!IsBoundedIdentity(Parsed.ClusterId) || !IsBoundedIdentity(Parsed.NodeId) || Parsed.HostSession == 0 ||
	    !IsBoundedIdentity(Parsed.InstanceId) || Parsed.ProtocolMin <= 0 || Parsed.ProtocolMax < Parsed.ProtocolMin ||
	    !IsNonce(Parsed.ConnectionNonce))
	{
		SetError(OutError, PeerProtocolError::InvalidField);
		return false;
	}
	OutHello = std::move(Parsed);
	SetError(OutError, PeerProtocolError::None);
	return true;
}

bool ParseAckObject(const JsonValue& Root, PeerHelloAck& OutAck, PeerProtocolError* OutError)
{
	std::string Type;
	std::string Result;
	PeerHelloAck Parsed;
	if (!StringMember(Root, "type", Type) || Type != "peer_hello_ack")
	{
		SetError(OutError, PeerProtocolError::WrongMessageType);
		return false;
	}
	if (!IntegerMember(Root, "negotiated_version", Parsed.NegotiatedVersion) ||
	    !IntegerMember(Root, "known_host_session", Parsed.KnownHostSession) || !StringMember(Root, "result", Result) ||
	    !StringMember(Root, "proof", Parsed.Proof))
	{
		SetError(OutError, PeerProtocolError::MissingField);
		return false;
	}
	if (!ParseResult(Result, Parsed.Result))
	{
		SetError(OutError, PeerProtocolError::UnsupportedResult);
		return false;
	}
	const JsonValue* ReasonValue = Member(Root, "reason");
	if (ReasonValue != nullptr)
	{
		const std::string* Reason = ReasonValue->TryGetString();
		if (Reason == nullptr)
		{
			SetError(OutError, PeerProtocolError::InvalidField);
			return false;
		}
		Parsed.Reason = *Reason;
	}
	if ((Parsed.Result == PeerHelloResult::Accepted && Parsed.NegotiatedVersion <= 0) ||
	    Parsed.Reason.size() > 512 || !IsProof(Parsed.Proof))
	{
		SetError(OutError, PeerProtocolError::InvalidField);
		return false;
	}
	OutAck = std::move(Parsed);
	SetError(OutError, PeerProtocolError::None);
	return true;
}

void AppendTranscriptField(std::string& Transcript, const std::string_view Value)
{
	Transcript.append(std::to_string(Value.size()));
	Transcript.push_back(':');
	Transcript.append(Value.data(), Value.size());
	Transcript.push_back('\n');
}
}    // namespace

bool EncodePeerFrame(const std::string_view Payload,
	                 std::vector<std::uint8_t>& OutFrame,
	                 PeerProtocolError* OutError)
{
	if (Payload.empty())
	{
		SetError(OutError, PeerProtocolError::InvalidInput);
		return false;
	}
	if (Payload.size() > MaximumPeerMessageBytes || Payload.size() > std::numeric_limits<std::uint32_t>::max())
	{
		SetError(OutError, PeerProtocolError::FrameTooLarge);
		return false;
	}
	const std::uint32_t Size = static_cast<std::uint32_t>(Payload.size());
	OutFrame.clear();
	OutFrame.reserve(PeerFrameHeaderBytes + Payload.size());
	OutFrame.push_back(static_cast<std::uint8_t>((Size >> 24) & 0xFF));
	OutFrame.push_back(static_cast<std::uint8_t>((Size >> 16) & 0xFF));
	OutFrame.push_back(static_cast<std::uint8_t>((Size >> 8) & 0xFF));
	OutFrame.push_back(static_cast<std::uint8_t>(Size & 0xFF));
	OutFrame.insert(OutFrame.end(), Payload.begin(), Payload.end());
	SetError(OutError, PeerProtocolError::None);
	return true;
}

PeerFrameDecoder::PeerFrameDecoder(const std::size_t InMaximumMessageBytes)
	: MaximumMessageBytes(InMaximumMessageBytes)
{
}

bool PeerFrameDecoder::Feed(const ByteView Bytes,
	                       std::vector<std::string>& OutMessages,
	                       PeerProtocolError* OutError)
{
	if ((Bytes.Size != 0 && Bytes.Data == nullptr) || MaximumMessageBytes == 0)
	{
		SetError(OutError, PeerProtocolError::InvalidInput);
		return false;
	}
	if (Bytes.Size > MaximumMessageBytes + PeerFrameHeaderBytes)
	{
		Reset();
		SetError(OutError, PeerProtocolError::FrameTooLarge);
		return false;
	}
	if (Bytes.Size != 0) Buffer.insert(Buffer.end(), Bytes.Data, Bytes.Data + Bytes.Size);
	std::size_t Consumed = 0;
	while (Buffer.size() - Consumed >= PeerFrameHeaderBytes)
	{
		const std::uint32_t Size = (static_cast<std::uint32_t>(Buffer[Consumed]) << 24) |
		                           (static_cast<std::uint32_t>(Buffer[Consumed + 1]) << 16) |
		                           (static_cast<std::uint32_t>(Buffer[Consumed + 2]) << 8) |
		                           Buffer[Consumed + 3];
		if (Size == 0 || Size > MaximumMessageBytes)
		{
			Reset();
			SetError(OutError, Size == 0 ? PeerProtocolError::InvalidInput : PeerProtocolError::FrameTooLarge);
			return false;
		}
		if (Buffer.size() - Consumed - PeerFrameHeaderBytes < Size) break;
		const char* Begin = reinterpret_cast<const char*>(Buffer.data() + Consumed + PeerFrameHeaderBytes);
		OutMessages.emplace_back(Begin, Size);
		Consumed += PeerFrameHeaderBytes + Size;
	}
	if (Consumed != 0) Buffer.erase(Buffer.begin(), Buffer.begin() + static_cast<std::ptrdiff_t>(Consumed));
	if (Buffer.size() > MaximumMessageBytes + PeerFrameHeaderBytes)
	{
		Reset();
		SetError(OutError, PeerProtocolError::FrameTooLarge);
		return false;
	}
	SetError(OutError, PeerProtocolError::None);
	return true;
}

void PeerFrameDecoder::Reset()
{
	Buffer.clear();
}

bool SerializePeerHello(const PeerHello& Hello, std::string& OutJson, PeerProtocolError* OutError)
{
	if (!IsBoundedIdentity(Hello.ClusterId) || !IsBoundedIdentity(Hello.NodeId) || Hello.HostSession == 0 ||
	    !IsBoundedIdentity(Hello.InstanceId) || Hello.ProtocolMin <= 0 || Hello.ProtocolMax < Hello.ProtocolMin ||
	    !IsNonce(Hello.ConnectionNonce))
	{
		SetError(OutError, PeerProtocolError::InvalidField);
		return false;
	}
	JsonValue Root;
	Root.SetObject();
	WriteString(Root, "type", "peer_hello");
	WriteString(Root, "cluster_id", Hello.ClusterId);
	WriteString(Root, "node_id", Hello.NodeId);
	WriteUnsigned(Root, "host_session", Hello.HostSession);
	WriteString(Root, "instance_id", Hello.InstanceId);
	WriteSigned(Root, "protocol_min", Hello.ProtocolMin);
	WriteSigned(Root, "protocol_max", Hello.ProtocolMax);
	WriteString(Root, "connection_nonce", Hello.ConnectionNonce);
	if (!SerializeJson(Root, OutJson))
	{
		SetError(OutError, PeerProtocolError::MalformedJson);
		return false;
	}
	SetError(OutError, PeerProtocolError::None);
	return true;
}

bool ParsePeerHello(const ByteView Json, PeerHello& OutHello, PeerProtocolError* OutError)
{
	JsonValue Root;
	if (!ParseObject(Json, Root, OutError)) return false;
	return ParseHelloObject(Root, OutHello, OutError);
}

bool SerializePeerHelloAck(const PeerHelloAck& Ack, std::string& OutJson, PeerProtocolError* OutError)
{
	if ((Ack.Result == PeerHelloResult::Accepted && Ack.NegotiatedVersion <= 0) ||
	    Ack.Reason.size() > 512 || !IsProof(Ack.Proof))
	{
		SetError(OutError, PeerProtocolError::InvalidField);
		return false;
	}
	JsonValue Root;
	Root.SetObject();
	WriteString(Root, "type", "peer_hello_ack");
	WriteSigned(Root, "negotiated_version", Ack.NegotiatedVersion);
	WriteUnsigned(Root, "known_host_session", Ack.KnownHostSession);
	WriteString(Root, "result", ResultText(Ack.Result));
	if (!Ack.Reason.empty()) WriteString(Root, "reason", Ack.Reason);
	WriteString(Root, "proof", Ack.Proof);
	if (!SerializeJson(Root, OutJson))
	{
		SetError(OutError, PeerProtocolError::MalformedJson);
		return false;
	}
	SetError(OutError, PeerProtocolError::None);
	return true;
}

bool ParsePeerHelloAck(const ByteView Json, PeerHelloAck& OutAck, PeerProtocolError* OutError)
{
	JsonValue Root;
	if (!ParseObject(Json, Root, OutError)) return false;
	return ParseAckObject(Root, OutAck, OutError);
}

bool ParsePeerMessage(const ByteView Json, PeerMessage& OutMessage, PeerProtocolError* OutError)
{
	JsonValue Root;
	if (!ParseObject(Json, Root, OutError)) return false;
	std::string Type;
	if (!StringMember(Root, "type", Type))
	{
		SetError(OutError, PeerProtocolError::MissingField);
		return false;
	}
	PeerMessage Parsed;
	if (Type == "peer_hello")
	{
		Parsed.Type = PeerMessageType::Hello;
		if (!ParseHelloObject(Root, Parsed.Hello, OutError)) return false;
	}
	else if (Type == "peer_hello_ack")
	{
		Parsed.Type = PeerMessageType::HelloAck;
		if (!ParseAckObject(Root, Parsed.HelloAck, OutError)) return false;
	}
	else if (Type == "peer_ping") Parsed.Type = PeerMessageType::Ping;
	else if (Type == "peer_pong") Parsed.Type = PeerMessageType::Pong;
	else
	{
		SetError(OutError, PeerProtocolError::WrongMessageType);
		return false;
	}
	OutMessage = std::move(Parsed);
	SetError(OutError, PeerProtocolError::None);
	return true;
}

std::string ComputePeerHelloAckProof(const std::string_view Secret,
	                                  const PeerHello& Prover,
	                                  const PeerHello& Verifier,
	                                  const PeerHelloAck& Ack)
{
	std::string Transcript = "deviceexplorer-peer-ack-v2\n";
	AppendTranscriptField(Transcript, Prover.ClusterId);
	AppendTranscriptField(Transcript, Prover.NodeId);
	AppendTranscriptField(Transcript, std::to_string(Prover.HostSession));
	AppendTranscriptField(Transcript, Prover.InstanceId);
	AppendTranscriptField(Transcript, std::to_string(Prover.ProtocolMin));
	AppendTranscriptField(Transcript, std::to_string(Prover.ProtocolMax));
	AppendTranscriptField(Transcript, Verifier.ClusterId);
	AppendTranscriptField(Transcript, Verifier.NodeId);
	AppendTranscriptField(Transcript, std::to_string(Verifier.HostSession));
	AppendTranscriptField(Transcript, Verifier.InstanceId);
	AppendTranscriptField(Transcript, std::to_string(Verifier.ProtocolMin));
	AppendTranscriptField(Transcript, std::to_string(Verifier.ProtocolMax));
	AppendTranscriptField(Transcript, std::to_string(Ack.NegotiatedVersion));
	AppendTranscriptField(Transcript, std::to_string(Ack.KnownHostSession));
	AppendTranscriptField(Transcript, ResultText(Ack.Result));
	AppendTranscriptField(Transcript, Ack.Reason);
	return Auth::ComputeProof(Secret, Transcript, Prover.ConnectionNonce, Verifier.ConnectionNonce);
}

const char* PeerProtocolErrorText(const PeerProtocolError Error)
{
	switch (Error)
	{
	case PeerProtocolError::None: return "none";
	case PeerProtocolError::InvalidInput: return "invalid input";
	case PeerProtocolError::FrameTooLarge: return "frame too large";
	case PeerProtocolError::MalformedJson: return "malformed JSON";
	case PeerProtocolError::WrongMessageType: return "wrong message type";
	case PeerProtocolError::MissingField: return "missing field";
	case PeerProtocolError::InvalidField: return "invalid field";
	case PeerProtocolError::UnsupportedResult: return "unsupported result";
	}
	return "unknown";
}
}    // namespace DeviceExplorer::Wire
