#pragma once

#include "DeviceExplorerWebSocket.h"

#include <cstddef>
#include <cstdint>
#include <memory>
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

struct JsonObjectStorage;

class JsonValue
{
public:
	JsonValue();
	~JsonValue();
	JsonValue(const JsonValue& Other);
	JsonValue& operator=(const JsonValue& Other);
	JsonValue(JsonValue&& Other) noexcept;
	JsonValue& operator=(JsonValue&& Other) noexcept;

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
	// Inserts a new case-sensitive key and returns false for a duplicate.
	bool InsertMember(std::string Key, JsonValue Value);
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
	// Object-only storage stays off scalar and array nodes. Protocol documents
	// contain many scalar values, so embedding three object containers in every
	// JsonValue made the bounded node count consume far more memory than needed.
	std::unique_ptr<JsonObjectStorage> ObjectValue;
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

DEVICEEXPLORERWIRE_API bool IsValidJsonNumber(std::string_view Value);

DEVICEEXPLORERWIRE_API bool ParseJson(ByteView Bytes,
	           JsonValue& OutValue,
	           JsonLimits Limits = {},
	           JsonError* OutError = nullptr);

DEVICEEXPLORERWIRE_API bool SerializeJson(const JsonValue& Value,
	               std::string& OutText,
	               JsonLimits Limits = {},
	               JsonError* OutError = nullptr);

DEVICEEXPLORERWIRE_API bool ParseDeviceExplorerMessage(ByteView Bytes,
	                            DeviceExplorerMessage& OutMessage,
	                            JsonLimits Limits = {},
	                            JsonError* OutError = nullptr);

DEVICEEXPLORERWIRE_API bool MakeDeviceExplorerMessage(std::string Type,
	                           std::string RequestId,
	                           DeviceExplorerMessage& OutMessage,
	                           JsonError* OutError = nullptr);

DEVICEEXPLORERWIRE_API bool SerializeDeviceExplorerMessage(const DeviceExplorerMessage& Message,
	                                std::string& OutText,
	                                JsonLimits Limits = {},
	                                JsonError* OutError = nullptr);

DEVICEEXPLORERWIRE_API const char* JsonErrorText(JsonError Error);
}    // namespace DeviceExplorer::Wire
