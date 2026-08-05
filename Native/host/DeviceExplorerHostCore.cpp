#include "DeviceExplorerHostCore.h"

#include "DeviceExplorerHostManifest.h"
#include "DeviceExplorerJson.h"

#include <asio.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <memory>
#include <string_view>
#include <utility>

namespace DeviceExplorer::Host
{
namespace
{
using Tcp = asio::ip::tcp;

constexpr std::size_t MaximumRequestHeaderBytes = 64 * 1024;

enum class ListenerKind : std::uint8_t
{
	Dashboard,
	Device
};

struct HttpRequest
{
	std::string Method;
	std::string Target;
};

struct HttpResponse
{
	int Status = 500;
	std::string ContentType = "application/json; charset=utf-8";
	std::string Body = "{\"error\":\"Internal server error\"}";
};

std::string_view StatusText(const int Status)
{
	switch (Status)
	{
		case 200: return "OK";
		case 400: return "Bad Request";
		case 404: return "Not Found";
		case 405: return "Method Not Allowed";
		case 503: return "Service Unavailable";
		default: return "Internal Server Error";
	}
}

bool ParseHttpRequest(const std::string_view Header, HttpRequest& OutRequest)
{
	const std::size_t LineEnd = Header.find("\r\n");
	if (LineEnd == std::string_view::npos) return false;
	const std::string_view Line = Header.substr(0, LineEnd);
	const std::size_t FirstSpace = Line.find(' ');
	const std::size_t SecondSpace = FirstSpace == std::string_view::npos
		? std::string_view::npos
		: Line.find(' ', FirstSpace + 1);
	if (FirstSpace == std::string_view::npos || SecondSpace == std::string_view::npos ||
	    Line.substr(SecondSpace + 1) != "HTTP/1.1")
	{
		return false;
	}
	const std::string_view Method = Line.substr(0, FirstSpace);
	const std::string_view Target = Line.substr(FirstSpace + 1, SecondSpace - FirstSpace - 1);
	if (Method.empty() || Target.empty() || Target.front() != '/' || Target.find_first_of("\r\n") != std::string_view::npos)
	{
		return false;
	}
	OutRequest.Method.assign(Method.data(), Method.size());
	OutRequest.Target.assign(Target.data(), Target.size());
	return true;
}

std::string MakeHttpResponse(const HttpResponse& Response)
{
	std::string Result = "HTTP/1.1 " + std::to_string(Response.Status) + " ";
	Result.append(StatusText(Response.Status));
	Result += "\r\nConnection: close\r\nContent-Type: ";
	Result += Response.ContentType;
	Result += "\r\nContent-Length: ";
	Result += std::to_string(Response.Body.size());
	Result += "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";
	Result += Response.Body;
	return Result;
}

bool AddNumber(Wire::JsonValue& Object, std::string Name, const std::int64_t Value)
{
	Wire::JsonValue Json;
	Json.SetSignedInteger(Value);
	return Object.InsertMember(std::move(Name), std::move(Json));
}

bool AddString(Wire::JsonValue& Object, std::string Name, std::string Value)
{
	Wire::JsonValue Json;
	if (!Json.SetString(std::move(Value))) return false;
	return Object.InsertMember(std::move(Name), std::move(Json));
}

std::string MakeConfigJson(const BoundEndpoints& Endpoints)
{
	Wire::JsonValue Root;
	Root.SetObject();
	if (!AddNumber(Root, "protocol_version", DeviceProtocolVersion) ||
	    !AddNumber(Root, "device_port", Endpoints.DevicePort) ||
	    !AddString(Root, "service_type", "_deviceexplorer._tcp.local."))
	{
		return {};
	}
	std::string Result;
	return Wire::SerializeJson(Root, Result) ? Result : std::string{};
}

class HttpSession final : public std::enable_shared_from_this<HttpSession>
{
public:
	HttpSession(Tcp::socket Socket,
	            const ListenerKind Kind,
	            std::string ManifestJson,
	            const BoundEndpoints Endpoints)
		: Socket(std::move(Socket))
		, Kind(Kind)
		, ManifestJson(std::move(ManifestJson))
		, Endpoints(Endpoints)
	{
	}

	void Start()
	{
		auto Self = shared_from_this();
		asio::async_read_until(
			Socket,
			asio::dynamic_buffer(RequestBuffer, MaximumRequestHeaderBytes),
			"\r\n\r\n",
			[Self](const asio::error_code& Error, const std::size_t Bytes)
			{
				Self->OnRead(Error, Bytes);
			});
	}

private:
	void OnRead(const asio::error_code& Error, const std::size_t Bytes)
	{
		if (Error)
		{
			Close();
			return;
		}
		HttpRequest Request;
		HttpResponse Response;
		if (!ParseHttpRequest(std::string_view(RequestBuffer).substr(0, Bytes), Request))
		{
			Response.Status = 400;
			Response.Body = "{\"error\":\"Invalid HTTP request\"}";
		}
		else
		{
			Response = Route(Request);
		}
		ResponseBytes = MakeHttpResponse(Response);
		auto Self = shared_from_this();
		asio::async_write(
			Socket,
			asio::buffer(ResponseBytes),
			[Self](const asio::error_code&, const std::size_t)
			{
				Self->Close();
			});
	}

	HttpResponse Route(const HttpRequest& Request) const
	{
		if (Request.Method != "GET")
		{
			return { 405, "application/json; charset=utf-8", "{\"error\":\"Method not allowed\"}" };
		}
		if (Request.Target == "/health")
		{
			return { 200, "application/json; charset=utf-8", "{\"status\":\"ok\"}" };
		}
		if (Request.Target == "/host-manifest")
		{
			return { 200, "application/json; charset=utf-8", ManifestJson };
		}
		if (Kind == ListenerKind::Dashboard && Request.Target == "/api/config")
		{
			const std::string Json = MakeConfigJson(Endpoints);
			return Json.empty()
				? HttpResponse{}
				: HttpResponse{ 200, "application/json; charset=utf-8", Json };
		}
		if (Kind == ListenerKind::Device && Request.Target == "/device/connect")
		{
			return { 503,
			         "application/json; charset=utf-8",
			         "{\"error\":\"Native device sessions are not enabled in this migration build\"}" };
		}
		return { 404, "application/json; charset=utf-8", "{\"error\":\"Route not found\"}" };
	}

	void Close()
	{
		asio::error_code Ignored;
		Socket.shutdown(Tcp::socket::shutdown_both, Ignored);
		Socket.close(Ignored);
	}

	Tcp::socket Socket;
	ListenerKind Kind;
	std::string ManifestJson;
	BoundEndpoints Endpoints;
	std::string RequestBuffer;
	std::string ResponseBytes;
};
}    // namespace

struct HostCore::Implementation
{
	explicit Implementation(HostConfig InConfig)
		: Config(std::move(InConfig))
		, DashboardAcceptor(Io)
		, DeviceAcceptor(Io)
	{
	}

	void Log(const LogLevel Level, const std::string& Message) const
	{
		if (Config.Log) Config.Log(Level, Message);
	}

	bool Bind(Tcp::acceptor& Acceptor,
	          const std::string& AddressText,
	          const std::uint16_t Port,
	          const bool RequireLoopback,
	          std::string& OutError)
	{
		asio::error_code Error;
		const asio::ip::address Address = asio::ip::make_address(AddressText, Error);
		if (Error)
		{
			OutError = "invalid listen address '" + AddressText + "': " + Error.message();
			return false;
		}
		if (!Address.is_v4() || (RequireLoopback && !Address.is_loopback()))
		{
			OutError = RequireLoopback
				? "dashboard address must be an IPv4 loopback address"
				: "device address must be IPv4";
			return false;
		}
		const Tcp::endpoint Endpoint(Address, Port);
		Acceptor.open(Endpoint.protocol(), Error);
		if (!Error) Acceptor.set_option(Tcp::acceptor::reuse_address(true), Error);
		if (!Error) Acceptor.bind(Endpoint, Error);
		if (!Error) Acceptor.listen(asio::socket_base::max_listen_connections, Error);
		if (Error)
		{
			OutError = "cannot listen on " + AddressText + ':' + std::to_string(Port) + ": " + Error.message();
			asio::error_code Ignored;
			Acceptor.close(Ignored);
			return false;
		}
		return true;
	}

	void Accept(Tcp::acceptor& Acceptor, const ListenerKind Kind)
	{
		Tcp::acceptor* const AcceptorPtr = &Acceptor;
		Acceptor.async_accept(
			[this, AcceptorPtr, Kind](const asio::error_code& Error, Tcp::socket Socket)
			{
				if (!Error)
				{
					std::make_shared<HttpSession>(std::move(Socket), Kind, ManifestJson, Endpoints)->Start();
				}
				else if (Running && Error != asio::error::operation_aborted)
				{
					Log(LogLevel::Warning, "accept failed: " + Error.message());
				}
				if (Running && AcceptorPtr->is_open()) Accept(*AcceptorPtr, Kind);
			});
	}

	HostConfig Config;
	asio::io_context Io;
	Tcp::acceptor DashboardAcceptor;
	Tcp::acceptor DeviceAcceptor;
	BoundEndpoints Endpoints;
	std::string ManifestJson;
	bool Running = false;
};

HostCore::HostCore(HostConfig Config)
	: Impl(std::make_unique<Implementation>(std::move(Config)))
{
}

HostCore::~HostCore()
{
	Stop();
}

bool HostCore::Start(std::string& OutError)
{
	if (Impl->Running)
	{
		OutError = "host is already running";
		return false;
	}
	Wire::HostManifest Manifest;
	Manifest.BuildId = Impl->Config.BuildId.empty() ? "unknown" : Impl->Config.BuildId;
	if (!Wire::SerializeHostManifest(Manifest, Impl->ManifestJson))
	{
		OutError = "cannot serialize host manifest";
		return false;
	}
	if (!Impl->Bind(Impl->DashboardAcceptor,
	                Impl->Config.DashboardAddress,
	                Impl->Config.DashboardPort,
	                true,
	                OutError) ||
	    !Impl->Bind(Impl->DeviceAcceptor,
	                Impl->Config.DeviceAddress,
	                Impl->Config.DevicePort,
	                false,
	                OutError))
	{
		Stop();
		return false;
	}

	asio::error_code Error;
	const Tcp::endpoint Dashboard = Impl->DashboardAcceptor.local_endpoint(Error);
	if (Error)
	{
		OutError = "cannot read the dashboard endpoint: " + Error.message();
		Stop();
		return false;
	}
	const Tcp::endpoint Device = Impl->DeviceAcceptor.local_endpoint(Error);
	if (Error)
	{
		OutError = "cannot read the device endpoint: " + Error.message();
		Stop();
		return false;
	}
	Impl->Endpoints = {
		Dashboard.address().to_string(), Dashboard.port(), Device.address().to_string(), Device.port()
	};
	Impl->Running = true;
	Impl->Accept(Impl->DashboardAcceptor, ListenerKind::Dashboard);
	Impl->Accept(Impl->DeviceAcceptor, ListenerKind::Device);
	Impl->Log(LogLevel::Information,
	          "dashboard listening on " + Impl->Endpoints.DashboardAddress + ':' +
	          std::to_string(Impl->Endpoints.DashboardPort));
	Impl->Log(LogLevel::Information,
	          "device listener on " + Impl->Endpoints.DeviceAddress + ':' +
	          std::to_string(Impl->Endpoints.DevicePort));
	OutError.clear();
	return true;
}

void HostCore::RunFor(const std::chrono::milliseconds Duration)
{
	if (!Impl->Running) return;
	Impl->Io.restart();
	Impl->Io.run_for(Duration);
}

void HostCore::Stop()
{
	if (!Impl) return;
	Impl->Running = false;
	asio::error_code Ignored;
	Impl->DashboardAcceptor.cancel(Ignored);
	Impl->DashboardAcceptor.close(Ignored);
	Impl->DeviceAcceptor.cancel(Ignored);
	Impl->DeviceAcceptor.close(Ignored);
	Impl->Io.stop();
}

bool HostCore::IsRunning() const
{
	return Impl->Running;
}

BoundEndpoints HostCore::GetBoundEndpoints() const
{
	return Impl->Endpoints;
}
}    // namespace DeviceExplorer::Host
