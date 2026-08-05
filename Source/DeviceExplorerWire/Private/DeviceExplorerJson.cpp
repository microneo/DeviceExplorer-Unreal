#include "DeviceExplorerJson.h"

#include <algorithm>
#include <utility>

namespace DeviceExplorer::Wire
{
namespace
{
void SetError(JsonError* OutError, const JsonError Error)
{
	if (OutError != nullptr)
	{
		*OutError = Error;
	}
}

bool IsValidUtf8(const std::string_view Value)
{
	return IsValidWebSocketUtf8(
		{ reinterpret_cast<const std::uint8_t*>(Value.data()), Value.size() });
}

int HexValue(const std::uint8_t Character)
{
	if (Character >= '0' && Character <= '9') return Character - '0';
	if (Character >= 'a' && Character <= 'f') return Character - 'a' + 10;
	if (Character >= 'A' && Character <= 'F') return Character - 'A' + 10;
	return -1;
}

bool AppendUtf8(const std::uint32_t CodePoint, std::string& Out)
{
	if (CodePoint <= 0x7F)
	{
		Out.push_back(static_cast<char>(CodePoint));
	}
	else if (CodePoint <= 0x7FF)
	{
		Out.push_back(static_cast<char>(0xC0 | (CodePoint >> 6)));
		Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
	}
	else if (CodePoint >= 0xD800 && CodePoint <= 0xDFFF)
	{
		return false;
	}
	else if (CodePoint <= 0xFFFF)
	{
		Out.push_back(static_cast<char>(0xE0 | (CodePoint >> 12)));
		Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F)));
		Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
	}
	else if (CodePoint <= 0x10FFFF)
	{
		Out.push_back(static_cast<char>(0xF0 | (CodePoint >> 18)));
		Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 12) & 0x3F)));
		Out.push_back(static_cast<char>(0x80 | ((CodePoint >> 6) & 0x3F)));
		Out.push_back(static_cast<char>(0x80 | (CodePoint & 0x3F)));
	}
	else
	{
		return false;
	}
	return true;
}

class JsonParser
{
public:
	JsonParser(const ByteView InBytes, const JsonLimits InLimits)
		: Bytes(InBytes)
		, Limits(InLimits)
	{
	}

	bool Parse(JsonValue& OutValue)
	{
		SkipWhitespace();
		if (!ParseValue(0, OutValue)) return false;
		SkipWhitespace();
		if (Offset != Bytes.Size) return Fail(JsonError::TrailingData);
		return true;
	}

	JsonError GetError() const { return Error; }

private:
	bool Fail(const JsonError InError)
	{
		if (Error == JsonError::None) Error = InError;
		return false;
	}

	void SkipWhitespace()
	{
		while (Offset < Bytes.Size)
		{
			const std::uint8_t Character = Bytes.Data[Offset];
			if (Character != ' ' && Character != '\t' && Character != '\r' && Character != '\n') break;
			++Offset;
		}
	}

	bool Match(const std::string_view Text)
	{
		if (Offset > Bytes.Size || Bytes.Size - Offset < Text.size()) return false;
		for (std::size_t Index = 0; Index < Text.size(); ++Index)
		{
			if (Bytes.Data[Offset + Index] != static_cast<std::uint8_t>(Text[Index])) return false;
		}
		Offset += Text.size();
		return true;
	}

	bool ParseValue(const std::size_t Depth, JsonValue& OutValue)
	{
		if (NodeCount >= Limits.MaximumNodeCount) return Fail(JsonError::MaximumNodeCountExceeded);
		++NodeCount;
		if (Offset >= Bytes.Size) return Fail(JsonError::UnexpectedEnd);
		const std::uint8_t Character = Bytes.Data[Offset];
		if (Character == 'n')
		{
			if (!Match("null")) return Fail(JsonError::UnexpectedToken);
			OutValue.SetNull();
			return true;
		}
		if (Character == 't')
		{
			if (!Match("true")) return Fail(JsonError::UnexpectedToken);
			OutValue.SetBoolean(true);
			return true;
		}
		if (Character == 'f')
		{
			if (!Match("false")) return Fail(JsonError::UnexpectedToken);
			OutValue.SetBoolean(false);
			return true;
		}
		if (Character == '"')
		{
			std::string Value;
			if (!ParseString(Value)) return false;
			if (!OutValue.SetString(std::move(Value))) return Fail(JsonError::InvalidUnicode);
			return true;
		}
		if (Character == '[') return ParseArray(Depth, OutValue);
		if (Character == '{') return ParseObject(Depth, OutValue);
		if (Character == '-' || (Character >= '0' && Character <= '9')) return ParseNumber(OutValue);
		return Fail(JsonError::UnexpectedToken);
	}

	bool ParseArray(const std::size_t Depth, JsonValue& OutValue)
	{
		if (Depth >= Limits.MaximumDepth) return Fail(JsonError::MaximumDepthExceeded);
		++Offset;
		OutValue.SetArray();
		SkipWhitespace();
		if (Offset < Bytes.Size && Bytes.Data[Offset] == ']')
		{
			++Offset;
			return true;
		}
		for (;;)
		{
			JsonValue Element;
			if (!ParseValue(Depth + 1, Element) || !OutValue.Append(std::move(Element))) return false;
			SkipWhitespace();
			if (Offset >= Bytes.Size) return Fail(JsonError::UnexpectedEnd);
			if (Bytes.Data[Offset] == ']')
			{
				++Offset;
				return true;
			}
			if (Bytes.Data[Offset] != ',') return Fail(JsonError::UnexpectedToken);
			++Offset;
			SkipWhitespace();
		}
	}

	bool ParseObject(const std::size_t Depth, JsonValue& OutValue)
	{
		if (Depth >= Limits.MaximumDepth) return Fail(JsonError::MaximumDepthExceeded);
		++Offset;
		OutValue.SetObject();
		SkipWhitespace();
		if (Offset < Bytes.Size && Bytes.Data[Offset] == '}')
		{
			++Offset;
			return true;
		}
		for (;;)
		{
			if (Offset >= Bytes.Size) return Fail(JsonError::UnexpectedEnd);
			if (Bytes.Data[Offset] != '"') return Fail(JsonError::UnexpectedToken);
			std::string Key;
			if (!ParseString(Key)) return false;
			SkipWhitespace();
			if (Offset >= Bytes.Size) return Fail(JsonError::UnexpectedEnd);
			if (Bytes.Data[Offset] != ':') return Fail(JsonError::UnexpectedToken);
			++Offset;
			SkipWhitespace();
			JsonValue Value;
			if (!ParseValue(Depth + 1, Value)) return false;
			if (!OutValue.InsertMember(std::move(Key), std::move(Value))) return Fail(JsonError::DuplicateKey);
			SkipWhitespace();
			if (Offset >= Bytes.Size) return Fail(JsonError::UnexpectedEnd);
			if (Bytes.Data[Offset] == '}')
			{
				++Offset;
				return true;
			}
			if (Bytes.Data[Offset] != ',') return Fail(JsonError::UnexpectedToken);
			++Offset;
			SkipWhitespace();
		}
	}

	bool ParseNumber(JsonValue& OutValue)
	{
		const std::size_t Begin = Offset;
		if (Bytes.Data[Offset] == '-') ++Offset;
		if (Offset >= Bytes.Size) return Fail(JsonError::InvalidNumber);
		if (Bytes.Data[Offset] == '0')
		{
			++Offset;
			if (Offset < Bytes.Size && Bytes.Data[Offset] >= '0' && Bytes.Data[Offset] <= '9')
			{
				return Fail(JsonError::InvalidNumber);
			}
		}
		else if (Bytes.Data[Offset] >= '1' && Bytes.Data[Offset] <= '9')
		{
			do
			{
				++Offset;
			} while (Offset < Bytes.Size && Bytes.Data[Offset] >= '0' && Bytes.Data[Offset] <= '9');
		}
		else
		{
			return Fail(JsonError::InvalidNumber);
		}
		if (Offset < Bytes.Size && Bytes.Data[Offset] == '.')
		{
			++Offset;
			const std::size_t FractionBegin = Offset;
			while (Offset < Bytes.Size && Bytes.Data[Offset] >= '0' && Bytes.Data[Offset] <= '9') ++Offset;
			if (Offset == FractionBegin) return Fail(JsonError::InvalidNumber);
		}
		if (Offset < Bytes.Size && (Bytes.Data[Offset] == 'e' || Bytes.Data[Offset] == 'E'))
		{
			++Offset;
			if (Offset < Bytes.Size && (Bytes.Data[Offset] == '+' || Bytes.Data[Offset] == '-')) ++Offset;
			const std::size_t ExponentBegin = Offset;
			while (Offset < Bytes.Size && Bytes.Data[Offset] >= '0' && Bytes.Data[Offset] <= '9') ++Offset;
			if (Offset == ExponentBegin) return Fail(JsonError::InvalidNumber);
		}
		const std::string Number(reinterpret_cast<const char*>(Bytes.Data + Begin), Offset - Begin);
		if (!OutValue.SetNumberText(Number)) return Fail(JsonError::InvalidNumber);
		return true;
	}

	bool ReadHexQuad(std::uint32_t& OutValue)
	{
		if (Offset > Bytes.Size || Bytes.Size - Offset < 4) return Fail(JsonError::UnexpectedEnd);
		OutValue = 0;
		for (std::size_t Index = 0; Index < 4; ++Index)
		{
			const int Value = HexValue(Bytes.Data[Offset + Index]);
			if (Value < 0) return Fail(JsonError::InvalidEscape);
			OutValue = (OutValue << 4) | static_cast<std::uint32_t>(Value);
		}
		Offset += 4;
		return true;
	}

	bool ParseString(std::string& OutValue)
	{
		++Offset;
		OutValue.clear();
		while (Offset < Bytes.Size)
		{
			const std::uint8_t Character = Bytes.Data[Offset++];
			if (Character == '"')
			{
				if (!IsValidUtf8(OutValue)) return Fail(JsonError::InvalidUnicode);
				return true;
			}
			if (Character < 0x20) return Fail(JsonError::InvalidString);
			if (Character != '\\')
			{
				OutValue.push_back(static_cast<char>(Character));
			}
			else
			{
				if (Offset >= Bytes.Size) return Fail(JsonError::UnexpectedEnd);
				const std::uint8_t Escape = Bytes.Data[Offset++];
				switch (Escape)
				{
					case '"': OutValue.push_back('"'); break;
					case '\\': OutValue.push_back('\\'); break;
					case '/': OutValue.push_back('/'); break;
					case 'b': OutValue.push_back('\b'); break;
					case 'f': OutValue.push_back('\f'); break;
					case 'n': OutValue.push_back('\n'); break;
					case 'r': OutValue.push_back('\r'); break;
					case 't': OutValue.push_back('\t'); break;
					case 'u':
					{
						std::uint32_t CodePoint = 0;
						if (!ReadHexQuad(CodePoint)) return false;
						if (CodePoint >= 0xD800 && CodePoint <= 0xDBFF)
						{
							if (Offset > Bytes.Size || Bytes.Size - Offset < 6 ||
							    Bytes.Data[Offset] != '\\' || Bytes.Data[Offset + 1] != 'u')
							{
								return Fail(JsonError::InvalidUnicode);
							}
							Offset += 2;
							std::uint32_t Low = 0;
							if (!ReadHexQuad(Low)) return false;
							if (Low < 0xDC00 || Low > 0xDFFF) return Fail(JsonError::InvalidUnicode);
							CodePoint = 0x10000 + ((CodePoint - 0xD800) << 10) + (Low - 0xDC00);
						}
						else if (CodePoint >= 0xDC00 && CodePoint <= 0xDFFF)
						{
							return Fail(JsonError::InvalidUnicode);
						}
						if (!AppendUtf8(CodePoint, OutValue)) return Fail(JsonError::InvalidUnicode);
						break;
					}
					default: return Fail(JsonError::InvalidEscape);
				}
			}
			if (OutValue.size() > Limits.MaximumStringBytes) return Fail(JsonError::DocumentTooLarge);
		}
		return Fail(JsonError::UnexpectedEnd);
	}

	ByteView Bytes;
	JsonLimits Limits;
	std::size_t Offset = 0;
	std::size_t NodeCount = 0;
	JsonError Error = JsonError::None;
};

class JsonSerializer
{
public:
	JsonSerializer(const JsonLimits InLimits)
		: Limits(InLimits)
	{
	}

	bool Serialize(const JsonValue& Value, std::string& OutText)
	{
		Text.clear();
		if (!WriteValue(Value, 0)) return false;
		OutText = std::move(Text);
		return true;
	}

	JsonError GetError() const { return Error; }

private:
	bool Fail(const JsonError InError)
	{
		if (Error == JsonError::None) Error = InError;
		return false;
	}

	bool Append(const std::string_view Value)
	{
		if (Value.size() > Limits.MaximumDocumentBytes - std::min(Text.size(), Limits.MaximumDocumentBytes))
		{
			return Fail(JsonError::DocumentTooLarge);
		}
		Text.append(Value.data(), Value.size());
		return true;
	}

	bool AppendChar(const char Value)
	{
		if (Text.size() >= Limits.MaximumDocumentBytes) return Fail(JsonError::DocumentTooLarge);
		Text.push_back(Value);
		return true;
	}

	bool WriteString(const std::string_view Value)
	{
		if (Value.size() > Limits.MaximumStringBytes) return Fail(JsonError::DocumentTooLarge);
		if (!IsValidUtf8(Value)) return Fail(JsonError::InvalidUnicode);
		if (!AppendChar('"')) return false;
		constexpr char Hex[] = "0123456789abcdef";
		for (const unsigned char Character : Value)
		{
			switch (Character)
			{
				case '"': if (!Append("\\\"")) return false; break;
				case '\\': if (!Append("\\\\")) return false; break;
				case '\b': if (!Append("\\b")) return false; break;
				case '\f': if (!Append("\\f")) return false; break;
				case '\n': if (!Append("\\n")) return false; break;
				case '\r': if (!Append("\\r")) return false; break;
				case '\t': if (!Append("\\t")) return false; break;
				default:
					if (Character < 0x20)
					{
						const char Escape[] = { '\\', 'u', '0', '0', Hex[Character >> 4], Hex[Character & 0x0F] };
						if (!Append({ Escape, sizeof(Escape) })) return false;
					}
					else if (!AppendChar(static_cast<char>(Character)))
					{
						return false;
					}
			}
		}
		return AppendChar('"');
	}

	bool WriteValue(const JsonValue& Value, const std::size_t Depth)
	{
		if (NodeCount >= Limits.MaximumNodeCount) return Fail(JsonError::MaximumNodeCountExceeded);
		++NodeCount;
		switch (Value.GetType())
		{
			case JsonType::Null: return Append("null");
			case JsonType::Boolean:
			{
				bool Boolean = false;
				if (!Value.TryGetBoolean(Boolean)) return Fail(JsonError::UnexpectedToken);
				return Append(Boolean ? "true" : "false");
			}
			case JsonType::Number:
			{
				const std::string* Number = Value.TryGetNumberText();
				if (Number == nullptr || !IsValidJsonNumber(*Number)) return Fail(JsonError::InvalidNumber);
				return Append(*Number);
			}
			case JsonType::String:
			{
				const std::string* String = Value.TryGetString();
				return String != nullptr ? WriteString(*String) : Fail(JsonError::InvalidString);
			}
			case JsonType::Array:
			{
				if (Depth >= Limits.MaximumDepth) return Fail(JsonError::MaximumDepthExceeded);
				const std::vector<JsonValue>* Values = Value.TryGetArray();
				if (Values == nullptr || !AppendChar('[')) return false;
				for (std::size_t Index = 0; Index < Values->size(); ++Index)
				{
					if ((Index > 0 && !AppendChar(',')) || !WriteValue((*Values)[Index], Depth + 1)) return false;
				}
				return AppendChar(']');
			}
			case JsonType::Object:
			{
				if (Depth >= Limits.MaximumDepth) return Fail(JsonError::MaximumDepthExceeded);
				const std::vector<std::string>* Keys = Value.TryGetObjectKeys();
				const std::vector<JsonValue>* Values = Value.TryGetObjectValues();
				if (Keys == nullptr || Values == nullptr || Keys->size() != Values->size())
				{
					return Fail(JsonError::UnexpectedToken);
				}
				if (!AppendChar('{')) return false;
				for (std::size_t Index = 0; Index < Keys->size(); ++Index)
				{
					if ((Index > 0 && !AppendChar(',')) || !WriteString((*Keys)[Index]) ||
					    !AppendChar(':') || !WriteValue((*Values)[Index], Depth + 1))
					{
						return false;
					}
				}
				return AppendChar('}');
			}
		}
		return Fail(JsonError::UnexpectedToken);
	}

	JsonLimits Limits;
	std::string Text;
	std::size_t NodeCount = 0;
	JsonError Error = JsonError::None;
};

bool ReadMessageFields(const JsonValue& Root,
	                   std::string& OutType,
	                   std::string& OutRequestId,
	                   bool& OutHasRequestId,
	                   JsonError& OutError)
{
	if (Root.GetType() != JsonType::Object)
	{
		OutError = JsonError::RootNotObject;
		return false;
	}
	const JsonValue* TypeValue = Root.FindMember("type");
	if (TypeValue == nullptr)
	{
		OutError = JsonError::MissingMessageType;
		return false;
	}
	const std::string* Type = TypeValue->TryGetString();
	if (Type == nullptr || Type->empty())
	{
		OutError = JsonError::InvalidMessageType;
		return false;
	}
	OutType = *Type;
	OutRequestId.clear();
	OutHasRequestId = false;
	if (const JsonValue* RequestValue = Root.FindMember("request_id"))
	{
		const std::string* RequestId = RequestValue->TryGetString();
		if (RequestId == nullptr)
		{
			OutError = JsonError::InvalidRequestId;
			return false;
		}
		OutRequestId = *RequestId;
		OutHasRequestId = true;
	}
	OutError = JsonError::None;
	return true;
}
}    // namespace

void JsonValue::Reset(const JsonType NewType)
{
	Type = NewType;
	BooleanValue = false;
	ScalarValue.clear();
	ArrayValues.clear();
	ObjectKeys.clear();
	ObjectValues.clear();
	ObjectIndex.clear();
}

void JsonValue::SetNull()
{
	Reset(JsonType::Null);
}

void JsonValue::SetBoolean(const bool Value)
{
	Reset(JsonType::Boolean);
	BooleanValue = Value;
}

bool JsonValue::SetNumberText(std::string Value)
{
	if (!IsValidJsonNumber(Value)) return false;
	Reset(JsonType::Number);
	ScalarValue = std::move(Value);
	return true;
}

void JsonValue::SetSignedInteger(const std::int64_t Value)
{
	Reset(JsonType::Number);
	ScalarValue = std::to_string(Value);
}

void JsonValue::SetUnsignedInteger(const std::uint64_t Value)
{
	Reset(JsonType::Number);
	ScalarValue = std::to_string(Value);
}

bool JsonValue::SetString(std::string Value)
{
	if (!IsValidUtf8(Value)) return false;
	Reset(JsonType::String);
	ScalarValue = std::move(Value);
	return true;
}

void JsonValue::SetArray()
{
	Reset(JsonType::Array);
}

void JsonValue::SetObject()
{
	Reset(JsonType::Object);
}

bool JsonValue::TryGetBoolean(bool& OutValue) const
{
	if (Type != JsonType::Boolean) return false;
	OutValue = BooleanValue;
	return true;
}

const std::string* JsonValue::TryGetNumberText() const
{
	return Type == JsonType::Number ? &ScalarValue : nullptr;
}

const std::string* JsonValue::TryGetString() const
{
	return Type == JsonType::String ? &ScalarValue : nullptr;
}

const std::vector<JsonValue>* JsonValue::TryGetArray() const
{
	return Type == JsonType::Array ? &ArrayValues : nullptr;
}

const std::vector<std::string>* JsonValue::TryGetObjectKeys() const
{
	return Type == JsonType::Object ? &ObjectKeys : nullptr;
}

const std::vector<JsonValue>* JsonValue::TryGetObjectValues() const
{
	return Type == JsonType::Object ? &ObjectValues : nullptr;
}

bool JsonValue::Append(JsonValue Value)
{
	if (Type != JsonType::Array) return false;
	ArrayValues.push_back(std::move(Value));
	return true;
}

bool JsonValue::InsertMember(std::string Key, JsonValue Value)
{
	if (Type != JsonType::Object || !IsValidUtf8(Key) || ObjectIndex.find(Key) != ObjectIndex.end())
	{
		return false;
	}
	const std::size_t Index = ObjectKeys.size();
	ObjectKeys.push_back(Key);
	ObjectValues.push_back(std::move(Value));
	ObjectIndex.emplace(std::move(Key), Index);
	return true;
}

bool JsonValue::SetMember(std::string Key, JsonValue Value)
{
	if (Type != JsonType::Object || !IsValidUtf8(Key)) return false;
	const auto Existing = ObjectIndex.find(Key);
	if (Existing != ObjectIndex.end())
	{
		ObjectValues[Existing->second] = std::move(Value);
		return true;
	}
	const std::size_t Index = ObjectKeys.size();
	ObjectKeys.push_back(Key);
	ObjectValues.push_back(std::move(Value));
	ObjectIndex.emplace(std::move(Key), Index);
	return true;
}

const JsonValue* JsonValue::FindMember(const std::string_view Key) const
{
	if (Type != JsonType::Object) return nullptr;
	const auto Existing = ObjectIndex.find(std::string(Key));
	return Existing == ObjectIndex.end() ? nullptr : &ObjectValues[Existing->second];
}

JsonValue* JsonValue::FindMember(const std::string_view Key)
{
	if (Type != JsonType::Object) return nullptr;
	const auto Existing = ObjectIndex.find(std::string(Key));
	return Existing == ObjectIndex.end() ? nullptr : &ObjectValues[Existing->second];
}

bool IsValidJsonNumber(const std::string_view Value)
{
	if (Value.empty()) return false;
	std::size_t Offset = 0;
	if (Value[Offset] == '-' && ++Offset == Value.size()) return false;
	if (Value[Offset] == '0')
	{
		++Offset;
	}
	else if (Value[Offset] >= '1' && Value[Offset] <= '9')
	{
		do
		{
			++Offset;
		} while (Offset < Value.size() && Value[Offset] >= '0' && Value[Offset] <= '9');
	}
	else
	{
		return false;
	}
	if (Offset < Value.size() && Value[Offset] == '.')
	{
		++Offset;
		const std::size_t FractionBegin = Offset;
		while (Offset < Value.size() && Value[Offset] >= '0' && Value[Offset] <= '9') ++Offset;
		if (Offset == FractionBegin) return false;
	}
	if (Offset < Value.size() && (Value[Offset] == 'e' || Value[Offset] == 'E'))
	{
		++Offset;
		if (Offset < Value.size() && (Value[Offset] == '+' || Value[Offset] == '-')) ++Offset;
		const std::size_t ExponentBegin = Offset;
		while (Offset < Value.size() && Value[Offset] >= '0' && Value[Offset] <= '9') ++Offset;
		if (Offset == ExponentBegin) return false;
	}
	return Offset == Value.size();
}

bool ParseJson(const ByteView Bytes,
	           JsonValue& OutValue,
	           const JsonLimits Limits,
	           JsonError* OutError)
{
	if ((Bytes.Size > 0 && Bytes.Data == nullptr) || Bytes.Size == 0)
	{
		SetError(OutError, JsonError::InvalidInput);
		return false;
	}
	if (Bytes.Size > Limits.MaximumDocumentBytes)
	{
		SetError(OutError, JsonError::DocumentTooLarge);
		return false;
	}
	JsonParser Parser(Bytes, Limits);
	JsonValue Parsed;
	if (!Parser.Parse(Parsed))
	{
		SetError(OutError, Parser.GetError());
		return false;
	}
	OutValue = std::move(Parsed);
	SetError(OutError, JsonError::None);
	return true;
}

bool SerializeJson(const JsonValue& Value,
	               std::string& OutText,
	               const JsonLimits Limits,
	               JsonError* OutError)
{
	JsonSerializer Serializer(Limits);
	std::string Serialized;
	if (!Serializer.Serialize(Value, Serialized))
	{
		SetError(OutError, Serializer.GetError());
		return false;
	}
	OutText = std::move(Serialized);
	SetError(OutError, JsonError::None);
	return true;
}

bool ParseDeviceExplorerMessage(const ByteView Bytes,
	                            DeviceExplorerMessage& OutMessage,
	                            const JsonLimits Limits,
	                            JsonError* OutError)
{
	JsonValue Root;
	JsonError Error = JsonError::None;
	if (!ParseJson(Bytes, Root, Limits, &Error))
	{
		SetError(OutError, Error);
		return false;
	}
	DeviceExplorerMessage Message;
	if (!ReadMessageFields(Root, Message.Type, Message.RequestId, Message.HasRequestId, Error))
	{
		SetError(OutError, Error);
		return false;
	}
	Message.Root = std::move(Root);
	OutMessage = std::move(Message);
	SetError(OutError, JsonError::None);
	return true;
}

bool MakeDeviceExplorerMessage(std::string Type,
	                           std::string RequestId,
	                           DeviceExplorerMessage& OutMessage,
	                           JsonError* OutError)
{
	if (Type.empty() || !IsValidUtf8(Type))
	{
		SetError(OutError, JsonError::InvalidMessageType);
		return false;
	}
	if (!IsValidUtf8(RequestId))
	{
		SetError(OutError, JsonError::InvalidRequestId);
		return false;
	}

	DeviceExplorerMessage Message;
	Message.Root.SetObject();
	JsonValue TypeValue;
	TypeValue.SetString(Type);
	Message.Root.SetMember("type", std::move(TypeValue));
	Message.Type = std::move(Type);
	if (!RequestId.empty())
	{
		JsonValue RequestValue;
		RequestValue.SetString(RequestId);
		Message.Root.SetMember("request_id", std::move(RequestValue));
		Message.RequestId = std::move(RequestId);
		Message.HasRequestId = true;
	}
	OutMessage = std::move(Message);
	SetError(OutError, JsonError::None);
	return true;
}

bool SerializeDeviceExplorerMessage(const DeviceExplorerMessage& Message,
	                                std::string& OutText,
	                                const JsonLimits Limits,
	                                JsonError* OutError)
{
	std::string Type;
	std::string RequestId;
	bool HasRequestId = false;
	JsonError Error = JsonError::None;
	if (!ReadMessageFields(Message.Root, Type, RequestId, HasRequestId, Error))
	{
		SetError(OutError, Error);
		return false;
	}
	if (Message.Type != Type)
	{
		SetError(OutError, JsonError::InvalidMessageType);
		return false;
	}
	if (Message.HasRequestId != HasRequestId || (HasRequestId && Message.RequestId != RequestId))
	{
		SetError(OutError, JsonError::InvalidRequestId);
		return false;
	}
	return SerializeJson(Message.Root, OutText, Limits, OutError);
}

const char* JsonErrorText(const JsonError Error)
{
	switch (Error)
	{
		case JsonError::None: return "none";
		case JsonError::InvalidInput: return "invalid JSON input";
		case JsonError::DocumentTooLarge: return "JSON document or string exceeds its limit";
		case JsonError::MaximumDepthExceeded: return "JSON nesting depth exceeds its limit";
		case JsonError::MaximumNodeCountExceeded: return "JSON node count exceeds its limit";
		case JsonError::UnexpectedEnd: return "unexpected end of JSON input";
		case JsonError::UnexpectedToken: return "unexpected JSON token";
		case JsonError::TrailingData: return "data follows the JSON value";
		case JsonError::InvalidString: return "invalid JSON string";
		case JsonError::InvalidEscape: return "invalid JSON escape";
		case JsonError::InvalidUnicode: return "invalid JSON Unicode";
		case JsonError::InvalidNumber: return "invalid JSON number";
		case JsonError::DuplicateKey: return "duplicate JSON object key";
		case JsonError::RootNotObject: return "protocol message root is not an object";
		case JsonError::MissingMessageType: return "protocol message type is missing";
		case JsonError::InvalidMessageType: return "protocol message type is invalid";
		case JsonError::InvalidRequestId: return "protocol request_id is invalid";
	}
	return "unknown JSON error";
}
}    // namespace DeviceExplorer::Wire
