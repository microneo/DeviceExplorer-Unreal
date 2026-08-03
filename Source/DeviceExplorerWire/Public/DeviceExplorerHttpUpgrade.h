#pragma once

#include "DeviceExplorerWebSocket.h"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace DeviceExplorer::Wire
{
enum class HttpUpgradeStatus : std::uint8_t
{
	NeedMoreData,
	Complete,
	Error
};

enum class HttpUpgradeError : std::uint8_t
{
	None,
	HeaderTooLarge,
	InvalidStartLine,
	InvalidHeader,
	InvalidUpgrade,
	InvalidConnection,
	MissingWebSocketKey,
	InvalidWebSocketKey,
	UnsupportedWebSocketVersion,
	InvalidWebSocketAccept
};

using HttpHeader = std::pair<std::string, std::string>;

struct WebSocketUpgradeRequest
{
	std::string Target;
	std::string Host;
	std::string Key;
	std::vector<HttpHeader> Headers;
};

struct WebSocketUpgradeResponse
{
	std::string Accept;
	std::vector<HttpHeader> Headers;
};

struct HttpUpgradeParseResult
{
	HttpUpgradeStatus Status = HttpUpgradeStatus::NeedMoreData;
	HttpUpgradeError Error = HttpUpgradeError::None;
	std::size_t ConsumedBytes = 0;
};

HttpUpgradeParseResult ParseWebSocketUpgradeRequest(ByteView Bytes,
	                                                WebSocketUpgradeRequest& OutRequest,
	                                                std::size_t MaximumHeaderBytes = 64 * 1024);

HttpUpgradeParseResult ParseWebSocketUpgradeResponse(ByteView Bytes,
	                                                 const std::string& ExpectedAccept,
	                                                 WebSocketUpgradeResponse& OutResponse,
	                                                 std::size_t MaximumHeaderBytes = 64 * 1024);

bool MakeWebSocketAccept(const std::string& ClientKey, std::string& OutAccept);

std::string SerializeWebSocketUpgradeRequest(const std::string& Target,
	                                         const std::string& Host,
	                                         const std::string& ClientKey,
	                                         const std::vector<HttpHeader>& ExtraHeaders = {});

std::string SerializeWebSocketUpgradeResponse(const std::string& Accept,
	                                          const std::vector<HttpHeader>& ExtraHeaders = {});

const char* HttpUpgradeErrorText(HttpUpgradeError Error);
}    // namespace DeviceExplorer::Wire
