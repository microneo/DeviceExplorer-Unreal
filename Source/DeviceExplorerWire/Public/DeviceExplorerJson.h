#pragma once

#include "DeviceExplorerWebSocket.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace DeviceExplorer::Wire
{
enum class JsonType : std::uint8_t
{
	Null,
	Boolean,
	Number,
	String,
	Array,
	Object
};

enum class JsonError : std::uint8_t
{
	None,
	InvalidInput,
	DocumentTooLarge,
	MaximumDepthExceeded,
	MaximumNodeCountExceeded,
	UnexpectedEnd,
	UnexpectedToken,
	TrailingData,
	InvalidString,
	InvalidEscape,
	InvalidUnicode,
	InvalidNumber,
	DuplicateKey,
	RootNotObject,
	MissingMessageType,
	InvalidMessageType,
	InvalidRequestId
};

struct JsonLimits
{
	std::size_t MaximumDocumentBytes = 8 * 1024 * 1024;
	std::size_t MaximumStringBytes = 8 * 1024 * 1024;
	std::size_t MaximumDepth = 64;
	std::size_t MaximumNodeCount = 100000;
};

class JsonValue
{
public:
	JsonValue() = default;

	JsonType GetType() const { return Type; }
	void SetNull();
	void SetBoolean(bool Value);
	// Numbers retain their original JSON spelling so large integers and protocol
	// counters round-trip without an intermediate floating-point conversion.
	bool SetNumberText(std::string Value);
	void SetSignedInteger(std::int64_t Value);
	void SetUnsignedInteger(std::uint64_t Value);
	bool SetString(std::string Value);
	void SetArray();
	void SetObject();

	bool TryGetBoolean(bool& OutValue) const;
	const std::string* TryGetNumberText() const;
	const std::string* TryGetString() const;
	const std::vector<JsonValue>* TryGetArray() const;
	const std::vector<std::string>* TryGetObjectKeys() const;
	const std::vector<JsonValue>* TryGetObjectValues() const;

	bool Append(JsonValue Value);
	// Replaces an existing member with the same case-sensitive key.
	bool SetMember(std::string Key, JsonValue Value);
	const JsonValue* FindMember(std::string_view Key) const;
	JsonValue* FindMember(std::string_view Key);

private:
	void Reset(JsonType NewType);

	JsonType Type = JsonType::Null;
	bool BooleanValue = false;
	std::string ScalarValue;
	std::vector<JsonValue> ArrayValues;
	std::vector<std::string> ObjectKeys;
	std::vector<JsonValue> ObjectValues;
};

struct DeviceExplorerMessage
{
	JsonValue Root;
	// Cached envelope fields. SerializeDeviceExplorerMessage verifies that they
	// still match Root so callers cannot accidentally send a stale envelope.
	std::string Type;
	std::string RequestId;
	bool HasRequestId = false;
};

bool IsValidJsonNumber(std::string_view Value);

bool ParseJson(ByteView Bytes,
	           JsonValue& OutValue,
	           JsonLimits Limits = {},
	           JsonError* OutError = nullptr);

bool SerializeJson(const JsonValue& Value,
	               std::string& OutText,
	               JsonLimits Limits = {},
	               JsonError* OutError = nullptr);

bool ParseDeviceExplorerMessage(ByteView Bytes,
	                            DeviceExplorerMessage& OutMessage,
	                            JsonLimits Limits = {},
	                            JsonError* OutError = nullptr);

bool MakeDeviceExplorerMessage(std::string Type,
	                           std::string RequestId,
	                           DeviceExplorerMessage& OutMessage,
	                           JsonError* OutError = nullptr);

bool SerializeDeviceExplorerMessage(const DeviceExplorerMessage& Message,
	                                std::string& OutText,
	                                JsonLimits Limits = {},
	                                JsonError* OutError = nullptr);

const char* JsonErrorText(JsonError Error);
}    // namespace DeviceExplorer::Wire
