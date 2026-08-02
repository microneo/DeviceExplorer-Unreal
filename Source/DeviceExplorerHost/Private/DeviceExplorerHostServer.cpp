#include "DeviceExplorerHostServer.h"

#include "Async/Async.h"
#include "Containers/RingBuffer.h"
#include "DeviceExplorerHostMdns.h"
#include "DeviceExplorerTypes.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "Misc/Base64.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerHost, Log, All);

namespace
{
constexpr int32 MaximumHeaderBytes = 64 * 1024;
constexpr int64 MaximumJsonBodyBytes = 1024 * 1024;
constexpr uint64 MaximumWebSocketPayloadBytes = 8 * 1024 * 1024;
constexpr int32 IoChunkBytes = 64 * 1024;
constexpr double MaximumSocketIdleSeconds = 30.0;
constexpr double MaximumSocketSendSeconds = 15.0;
constexpr double DisconnectedDeviceExpirySeconds = 3600.0;

bool SendAll(FSocket* Socket, const uint8* Data, int64 Size)
{
	int64 Offset = 0;
	while (Offset < Size)
	{
		int32 Sent = 0;
		const int32 Remaining = static_cast<int32>(FMath::Min<int64>(Size - Offset, MAX_int32));
		if (!Socket->Send(Data + Offset, Remaining, Sent) || Sent <= 0)
		{
			return false;
		}
		Offset += Sent;
	}
	return true;
}

bool SendAllBounded(FSocket* Socket, const uint8* Data, int64 Size, double DeadlineSeconds)
{
	const double Deadline = FPlatformTime::Seconds() + DeadlineSeconds;
	int64 Offset = 0;
	while (Offset < Size)
	{
		const double SecondsLeft = Deadline - FPlatformTime::Seconds();
		if (SecondsLeft <= 0.0)
		{
			return false;
		}
		if (!Socket->Wait(ESocketWaitConditions::WaitForWrite, FTimespan::FromSeconds(FMath::Min(SecondsLeft, 0.25))))
		{
			continue;
		}

		int32 Sent = 0;
		const int32 Remaining = static_cast<int32>(FMath::Min<int64>(Size - Offset, MAX_int32));
		if (!Socket->Send(Data + Offset, Remaining, Sent) || Sent <= 0)
		{
			return false;
		}
		Offset += Sent;
	}
	return true;
}

bool SendUtf8(FSocket* Socket, const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);
	return SendAll(Socket, reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

bool ReceiveExact(FSocket* Socket, uint8* Destination, int64 Size, const TAtomic<bool>& bStopping)
{
	int64 Offset = 0;
	double LastProgressSeconds = FPlatformTime::Seconds();
	while (Offset < Size && !bStopping.Load())
	{
		if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(250)))
		{
			if (FPlatformTime::Seconds() - LastProgressSeconds > MaximumSocketIdleSeconds)
			{
				return false;
			}
			continue;
		}

		int32 Read = 0;
		const int32 Remaining = static_cast<int32>(FMath::Min<int64>(Size - Offset, MAX_int32));
		if (!Socket->Recv(Destination + Offset, Remaining, Read, ESocketReceiveFlags::None) || Read <= 0)
		{
			return false;
		}
		Offset += Read;
		LastProgressSeconds = FPlatformTime::Seconds();
	}
	return Offset == Size;
}

int32 FindHeaderEnd(const TArray<uint8>& Data)
{
	for (int32 Index = 3; Index < Data.Num(); ++Index)
	{
		if (Data[Index - 3] == '\r' && Data[Index - 2] == '\n' && Data[Index - 1] == '\r' && Data[Index] == '\n')
		{
			return Index + 1;
		}
	}
	return INDEX_NONE;
}

FString BytesToString(const TArray<uint8>& Data)
{
	if (Data.IsEmpty())
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Data.GetData()), Data.Num());
	return FString(Converted.Length(), Converted.Get());
}

FString JsonString(const TSharedRef<FJsonObject>& Object)
{
	FString Result;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Result);
	FJsonSerializer::Serialize(Object, Writer);
	return Result;
}

TSharedPtr<FJsonObject> ParseJson(const TArray<uint8>& Body)
{
	const FString Text = BytesToString(Body);
	TSharedPtr<FJsonObject> Object;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Object))
	{
		return nullptr;
	}
	return Object;
}

FString StatusText(const int32 Status)
{
	switch (Status)
	{
		case 200: return TEXT("OK");
		case 101: return TEXT("Switching Protocols");
		case 202: return TEXT("Accepted");
		case 400: return TEXT("Bad Request");
		case 401: return TEXT("Unauthorized");
		case 403: return TEXT("Forbidden");
		case 404: return TEXT("Not Found");
		case 405: return TEXT("Method Not Allowed");
		case 408: return TEXT("Request Timeout");
		case 409: return TEXT("Conflict");
		case 411: return TEXT("Length Required");
		case 413: return TEXT("Content Too Large");
		case 500: return TEXT("Internal Server Error");
		case 502: return TEXT("Bad Gateway");
		case 504: return TEXT("Gateway Timeout");
		default: return TEXT("Error");
	}
}

bool SendHttpHeaders(FSocket* Socket, const int32 Status, const FString& ContentType, const int64 ContentLength, const TMap<FString, FString>& ExtraHeaders = {})
{
	FString Headers = FString::Printf(TEXT("HTTP/1.1 %d %s\r\n") TEXT("Connection: close\r\n") TEXT("Content-Type: %s\r\n") TEXT("Content-Length: %lld\r\n")
	                                      TEXT("X-Content-Type-Options: nosniff\r\n"),
	                                  Status,
	                                  *StatusText(Status),
	                                  *ContentType,
	                                  ContentLength);
	for (const TPair<FString, FString>& Header : ExtraHeaders)
	{
		Headers += Header.Key;
		Headers += TEXT(": ");
		Headers += Header.Value;
		Headers += TEXT("\r\n");
	}
	Headers += TEXT("\r\n");
	return SendUtf8(Socket, Headers);
}

void SendHttpBytes(FSocket* Socket, const int32 Status, const FString& ContentType, const TArray<uint8>& Body, const TMap<FString, FString>& ExtraHeaders = {})
{
	if (SendHttpHeaders(Socket, Status, ContentType, Body.Num(), ExtraHeaders) && !Body.IsEmpty())
	{
		SendAll(Socket, Body.GetData(), Body.Num());
	}
}

void SendHttpText(FSocket* Socket, const int32 Status, const FString& ContentType, const FString& Body, const TMap<FString, FString>& ExtraHeaders = {})
{
	const FTCHARToUTF8 Utf8(*Body);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	SendHttpBytes(Socket, Status, ContentType, Bytes, ExtraHeaders);
}

void SendJsonResponse(FSocket* Socket, const int32 Status, const TSharedRef<FJsonObject>& Object)
{
	SendHttpText(Socket, Status, TEXT("application/json; charset=utf-8"), JsonString(Object), { { TEXT("Cache-Control"), TEXT("no-store") } });
}

void SendJsonError(FSocket* Socket, const int32 Status, const FString& Error)
{
	TSharedRef<FJsonObject> Object = MakeShared<FJsonObject>();
	Object->SetStringField(TEXT("error"), Error);
	SendJsonResponse(Socket, Status, Object);
}

FString NewRequestId()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

FString NormalizeRelativePath(const FString& Raw)
{
	FString Path = Raw;
	Path.TrimStartAndEndInline();
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	while (Path.StartsWith(TEXT("/")))
	{
		Path.RightChopInline(1, EAllowShrinking::No);
	}

	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("/"), true);
	TArray<FString> Safe;
	for (const FString& Segment : Segments)
	{
		if (Segment == TEXT("."))
		{
			continue;
		}
		if (Segment == TEXT("..") || Segment.Contains(TEXT(":")) || Segment.Contains(TEXT("\r")) || Segment.Contains(TEXT("\n")))
		{
			return FString();
		}
		Safe.Add(Segment);
	}
	return FString::Join(Safe, TEXT("/"));
}

FString SafeFilename(const FString& Raw)
{
	FString Result = FPaths::GetCleanFilename(Raw);
	Result.ReplaceInline(TEXT("\""), TEXT("_"));
	Result.ReplaceInline(TEXT("\r"), TEXT("_"));
	Result.ReplaceInline(TEXT("\n"), TEXT("_"));
	return Result.IsEmpty() ? TEXT("download.bin") : Result;
}

bool ReadWebSocketFrame(FSocket* Socket, const TAtomic<bool>& bStopping, uint8& OutOpcode, bool& bOutFinal, TArray<uint8>& OutPayload)
{
	uint8 Header[2] = {};
	if (!ReceiveExact(Socket, Header, 2, bStopping))
	{
		return false;
	}

	bOutFinal = (Header[0] & 0x80) != 0;
	OutOpcode = Header[0] & 0x0f;
	const bool bMasked = (Header[1] & 0x80) != 0;
	uint64 PayloadSize = Header[1] & 0x7f;
	if (PayloadSize == 126)
	{
		uint8 Extended[2] = {};
		if (!ReceiveExact(Socket, Extended, 2, bStopping))
		{
			return false;
		}
		PayloadSize = (static_cast<uint64>(Extended[0]) << 8) | Extended[1];
	}
	else if (PayloadSize == 127)
	{
		uint8 Extended[8] = {};
		if (!ReceiveExact(Socket, Extended, 8, bStopping))
		{
			return false;
		}
		PayloadSize = 0;
		for (uint8 Byte : Extended)
		{
			PayloadSize = (PayloadSize << 8) | Byte;
		}
	}
	if (!bMasked || PayloadSize > MaximumWebSocketPayloadBytes)
	{
		return false;
	}

	uint8 Mask[4] = {};
	if (!ReceiveExact(Socket, Mask, 4, bStopping))
	{
		return false;
	}

	OutPayload.SetNumUninitialized(static_cast<int32>(PayloadSize));
	if (PayloadSize > 0 && !ReceiveExact(Socket, OutPayload.GetData(), PayloadSize, bStopping))
	{
		return false;
	}
	for (int32 Index = 0; Index < OutPayload.Num(); ++Index)
	{
		OutPayload[Index] ^= Mask[Index % 4];
	}
	return true;
}

bool SendWebSocketFrame(FSocket* Socket, const uint8 Opcode, const TArrayView<const uint8> Payload)
{
	TArray<uint8> Header;
	Header.Add(0x80 | (Opcode & 0x0f));
	const uint64 Size = Payload.Num();
	if (Size < 126)
	{
		Header.Add(static_cast<uint8>(Size));
	}
	else if (Size <= MAX_uint16)
	{
		Header.Add(126);
		Header.Add(static_cast<uint8>((Size >> 8) & 0xff));
		Header.Add(static_cast<uint8>(Size & 0xff));
	}
	else
	{
		Header.Add(127);
		for (int32 Shift = 56; Shift >= 0; Shift -= 8)
		{
			Header.Add(static_cast<uint8>((Size >> Shift) & 0xff));
		}
	}
	return SendAllBounded(Socket, Header.GetData(), Header.Num(), MaximumSocketSendSeconds) &&
	       (Payload.IsEmpty() || SendAllBounded(Socket, Payload.GetData(), Payload.Num(), MaximumSocketSendSeconds));
}

FString WebSocketAcceptKey(const FString& ClientKey)
{
	const FString Combined = ClientKey + TEXT("258EAFA5-E914-47DA-95CA-C5AB0DC85B11");
	const FTCHARToUTF8 Utf8(*Combined);
	uint8 Digest[FSHA1::DigestSize] = {};
	FSHA1::HashBuffer(Utf8.Get(), Utf8.Length(), Digest);
	return FBase64::Encode(Digest, UE_ARRAY_COUNT(Digest));
}

FString ContentTypeForPath(const FString& Path)
{
	if (Path.EndsWith(TEXT(".html")))
	{
		return TEXT("text/html; charset=utf-8");
	}
	if (Path.EndsWith(TEXT(".css")))
	{
		return TEXT("text/css; charset=utf-8");
	}
	if (Path.EndsWith(TEXT(".js")))
	{
		return TEXT("text/javascript; charset=utf-8");
	}
	if (Path.EndsWith(TEXT(".svg")))
	{
		return TEXT("image/svg+xml");
	}
	if (Path.EndsWith(TEXT(".png")))
	{
		return TEXT("image/png");
	}
	return TEXT("application/octet-stream");
}
}    // namespace

struct FDeviceExplorerHostServer::FHttpRequest
{
	FString Method;
	FString Target;
	FString Path;
	TMap<FString, FString> Query;
	TMap<FString, FString> Headers;
	TArray<uint8> Body;
	int64 ContentLength = 0;
};

struct FDeviceExplorerHostServer::FLogEntry
{
	uint64 Sequence = 0;
	FString Timestamp;
	FString Category;
	FString Verbosity;
	FString Message;
};

struct FDeviceExplorerHostServer::FDeviceConnection : public TSharedFromThis<FDeviceConnection>
{
	explicit FDeviceConnection(FSocket* InSocket)
		: Socket(InSocket)
	{
	}

	bool SendText(const FString& Text)
	{
		const FTCHARToUTF8 Utf8(*Text);
		return SendFrame(0x1, MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()));
	}

	bool SendPong(const TArray<uint8>& Payload) { return SendFrame(0xA, Payload); }

	FSocket* Socket = nullptr;
	FString DeviceId;
	FString LocalAddress;
	FCriticalSection SendMutex;
	TAtomic<bool> bClosed{ false };

private:
	bool SendFrame(const uint8 Opcode, const TArrayView<const uint8> Payload)
	{
		FScopeLock Lock(&SendMutex);
		if (bClosed.Load())
		{
			return false;
		}
		if (SendWebSocketFrame(Socket, Opcode, Payload))
		{
			return true;
		}
		bClosed.Store(true);
		Socket->Shutdown(ESocketShutdownMode::ReadWrite);
		return false;
	}
};

struct FDeviceExplorerHostServer::FDeviceState
{
	FString Id;
	FString Name;
	FString ProjectName;
	FString EngineVersion;
	FString Platform;
	FString Configuration;
	FString BuildVersion;
	FString RemoteAddress;
	int32 ProtocolVersion = 0;
	int64 UptimeSeconds = 0;
	bool bConnected = false;
	FDateTime ConnectedAt;
	FDateTime LastSeen;
	uint64 DroppedLogs = 0;
	uint64 NextSequence = 1;
	TArray<FString> Capabilities;
	TArray<TSharedPtr<FJsonValue>> Commands;
	TArray<TSharedPtr<FJsonValue>> FileRoots;
	TArray<TSharedPtr<FJsonValue>> DataModules;
	TRingBuffer<FLogEntry> Logs;
	TSharedPtr<FDeviceConnection> Connection;
};

struct FDeviceExplorerHostServer::FPendingRequest
{
	FPendingRequest() { Event = FPlatformProcess::GetSynchEventFromPool(true); }

	~FPendingRequest()
	{
		if (Event != nullptr)
		{
			FPlatformProcess::ReturnSynchEventToPool(Event);
		}
	}

	FEvent* Event = nullptr;
	TSharedPtr<FJsonObject> Result;
};

struct FDeviceExplorerHostServer::FTransfer
{
	FString Id;
	FString DeviceId;
	FString Root;
	FString RelativePath;
	FString Filename;
	FString State;
	FString Error;
	FString LocalPath;
	int64 Bytes = 0;
	FDateTime CreatedAt;
	FDateTime UpdatedAt;
};

FDeviceExplorerHostServer::FDeviceExplorerHostServer(FDeviceExplorerHostConfig InConfig)
	: Config(MoveTemp(InConfig))
{
}

FDeviceExplorerHostServer::~FDeviceExplorerHostServer()
{
	Stop();
}

bool FDeviceExplorerHostServer::Start()
{
	if (bStarted)
	{
		return true;
	}

	IFileManager::Get().MakeDirectory(*Config.TransferDirectory, true);
	if (!CreateListener(TEXT("127.0.0.1"), Config.DashboardPort, DashboardListener) || !CreateListener(TEXT("0.0.0.0"), Config.DevicePort, DeviceListener))
	{
		Stop();
		return false;
	}

	Mdns = MakeUnique<FDeviceExplorerHostMdns>(Config.DevicePort, Config.DashboardPort, Config.Token);
	if (!Mdns->Start())
	{
		UE_LOG(LogDeviceExplorerHost, Warning, TEXT("mDNS advertisement did not start; manual endpoint remains available"));
	}

	bStopping.Store(false);
	bStarted = true;
	UE_LOG(LogDeviceExplorerHost, Display, TEXT("Dashboard: http://127.0.0.1:%d"), Config.DashboardPort);
	UE_LOG(LogDeviceExplorerHost, Display, TEXT("Device endpoint: 0.0.0.0:%d"), Config.DevicePort);
	return true;
}

bool FDeviceExplorerHostServer::Tick()
{
	if (!bStarted || bStopping.Load())
	{
		return false;
	}

	AcceptConnections(DashboardListener, true);
	AcceptConnections(DeviceListener, false);
	if (Mdns)
	{
		Mdns->Tick();
	}
	ReapWorkers();

	const double Now = FPlatformTime::Seconds();
	if (Now - LastCleanupSeconds >= 60.0)
	{
		LastCleanupSeconds = Now;
		CleanupExpiredState();
	}
	return true;
}

void FDeviceExplorerHostServer::Stop()
{
	if (!bStarted && DashboardListener == nullptr && DeviceListener == nullptr)
	{
		return;
	}

	bStopping.Store(true);
	if (Mdns)
	{
		Mdns->Stop();
		Mdns.Reset();
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (DashboardListener != nullptr)
	{
		DashboardListener->Close();
		SocketSubsystem->DestroySocket(DashboardListener);
		DashboardListener = nullptr;
	}
	if (DeviceListener != nullptr)
	{
		DeviceListener->Close();
		SocketSubsystem->DestroySocket(DeviceListener);
		DeviceListener = nullptr;
	}

	{
		FScopeLock Lock(&WorkerMutex);
		for (FSocket* Socket : ClientSockets)
		{
			Socket->Shutdown(ESocketShutdownMode::ReadWrite);
		}
	}

	for (TFuture<void>& Worker : Workers)
	{
		Worker.Wait();
	}
	Workers.Reset();

	{
		FScopeLock Lock(&StateMutex);
		Devices.Reset();
		PendingRequests.Reset();
		Transfers.Reset();
	}
	bStarted = false;
}

bool FDeviceExplorerHostServer::CreateListener(const FString& Address, const int32 Port, FSocket*& OutSocket)
{
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	OutSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("DeviceExplorerHostListener"), false);
	if (OutSocket == nullptr)
	{
		return false;
	}

	TSharedRef<FInternetAddr> InternetAddress = SocketSubsystem->CreateInternetAddr();
	bool bValidAddress = false;
	InternetAddress->SetIp(*Address, bValidAddress);
	InternetAddress->SetPort(Port);

	OutSocket->SetReuseAddr(true);
	OutSocket->SetNonBlocking(true);
	if (!bValidAddress || !OutSocket->Bind(*InternetAddress) || !OutSocket->Listen(64))
	{
		UE_LOG(LogDeviceExplorerHost, Error, TEXT("Cannot listen on %s:%d"), *Address, Port);
		OutSocket->Close();
		SocketSubsystem->DestroySocket(OutSocket);
		OutSocket = nullptr;
		return false;
	}
	return true;
}

void FDeviceExplorerHostServer::AcceptConnections(FSocket* Listener, const bool bDashboard)
{
	if (Listener == nullptr)
	{
		return;
	}

	bool bPending = false;
	while (Listener->HasPendingConnection(bPending) && bPending)
	{
		TSharedRef<FInternetAddr> RemoteAddress = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		FSocket* Client = Listener->Accept(*RemoteAddress, TEXT("DeviceExplorerHostClient"));
		if (Client == nullptr)
		{
			break;
		}
		Client->SetNonBlocking(false);
		Client->SetNoDelay(true);
		DispatchConnection(Client, bDashboard);
	}
}

void FDeviceExplorerHostServer::DispatchConnection(FSocket* Socket, const bool bDashboard)
{
	{
		FScopeLock Lock(&WorkerMutex);
		ClientSockets.Add(Socket);
	}

	TFuture<void> Worker = Async(EAsyncExecution::Thread,
	                             [this, Socket, bDashboard]()
	                             {
									 if (bDashboard)
									 {
										 HandleDashboardConnection(Socket);
									 }
									 else
									 {
										 HandleDeviceConnection(Socket);
									 }
									 CloseClientSocket(Socket);
								 });

	FScopeLock Lock(&WorkerMutex);
	Workers.Add(MoveTemp(Worker));
}

void FDeviceExplorerHostServer::CloseClientSocket(FSocket* Socket)
{
	{
		FScopeLock Lock(&WorkerMutex);
		ClientSockets.Remove(Socket);
	}
	Socket->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
}

void FDeviceExplorerHostServer::ReapWorkers()
{
	FScopeLock Lock(&WorkerMutex);
	Workers.RemoveAll(
		[](TFuture<void>& Worker)
		{
			return Worker.IsReady();
		});
}

bool FDeviceExplorerHostServer::ReadHttpRequest(FSocket* Socket, FHttpRequest& OutRequest)
{
	TArray<uint8> Buffer;
	Buffer.Reserve(4096);
	int32 HeaderEnd = INDEX_NONE;
	double LastProgressSeconds = FPlatformTime::Seconds();

	while (!bStopping.Load() && Buffer.Num() < MaximumHeaderBytes && HeaderEnd == INDEX_NONE)
	{
		if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(250)))
		{
			if (FPlatformTime::Seconds() - LastProgressSeconds > MaximumSocketIdleSeconds)
			{
				return false;
			}
			continue;
		}

		uint8 Chunk[4096];
		int32 Read = 0;
		if (!Socket->Recv(Chunk, UE_ARRAY_COUNT(Chunk), Read, ESocketReceiveFlags::None) || Read <= 0)
		{
			return false;
		}
		Buffer.Append(Chunk, Read);
		LastProgressSeconds = FPlatformTime::Seconds();
		HeaderEnd = FindHeaderEnd(Buffer);
	}
	if (HeaderEnd == INDEX_NONE)
	{
		return false;
	}

	TArray<uint8> HeaderBytes;
	HeaderBytes.Append(Buffer.GetData(), HeaderEnd);
	FString HeaderText = BytesToString(HeaderBytes);
	TArray<FString> Lines;
	HeaderText.ParseIntoArrayLines(Lines, false);
	if (Lines.IsEmpty())
	{
		return false;
	}

	TArray<FString> RequestParts;
	Lines[0].ParseIntoArrayWS(RequestParts);
	if (RequestParts.Num() < 2)
	{
		return false;
	}
	OutRequest.Method = RequestParts[0].ToUpper();
	OutRequest.Target = RequestParts[1];

	FString QueryString;
	if (!OutRequest.Target.Split(TEXT("?"), &OutRequest.Path, &QueryString))
	{
		OutRequest.Path = OutRequest.Target;
	}
	OutRequest.Path = FGenericPlatformHttp::UrlDecode(OutRequest.Path);

	TArray<FString> QueryParts;
	QueryString.ParseIntoArray(QueryParts, TEXT("&"), true);
	for (const FString& Part : QueryParts)
	{
		FString Key;
		FString Value;
		if (Part.Split(TEXT("="), &Key, &Value))
		{
			// UrlDecode only expands %XX, but query strings are form-encoded, where '+' stands for a space.
			Key.ReplaceInline(TEXT("+"), TEXT(" "), ESearchCase::CaseSensitive);
			Value.ReplaceInline(TEXT("+"), TEXT(" "), ESearchCase::CaseSensitive);
			OutRequest.Query.Add(FGenericPlatformHttp::UrlDecode(Key), FGenericPlatformHttp::UrlDecode(Value));
		}
	}

	for (int32 Index = 1; Index < Lines.Num(); ++Index)
	{
		FString Key;
		FString Value;
		if (Lines[Index].Split(TEXT(":"), &Key, &Value))
		{
			Key.TrimStartAndEndInline();
			Key.ToLowerInline();
			Value.TrimStartAndEndInline();
			OutRequest.Headers.Add(MoveTemp(Key), MoveTemp(Value));
		}
	}

	const FString ContentLength = OutRequest.Headers.FindRef(TEXT("content-length"));
	OutRequest.ContentLength = ContentLength.IsEmpty() ? 0 : FCString::Atoi64(*ContentLength);
	const int32 Remaining = Buffer.Num() - HeaderEnd;
	if (Remaining > 0)
	{
		OutRequest.Body.Append(Buffer.GetData() + HeaderEnd, Remaining);
	}
	return true;
}

bool FDeviceExplorerHostServer::ReadRequestBody(FSocket* Socket, FHttpRequest& Request, const int64 MaximumBytes)
{
	if (Request.ContentLength < 0 || Request.ContentLength > MaximumBytes)
	{
		return false;
	}
	if (Request.Body.Num() > Request.ContentLength)
	{
		Request.Body.SetNum(static_cast<int32>(Request.ContentLength));
	}
	const int64 Remaining = Request.ContentLength - Request.Body.Num();
	if (Remaining == 0)
	{
		return true;
	}

	const int32 OldSize = Request.Body.Num();
	Request.Body.SetNumUninitialized(static_cast<int32>(Request.ContentLength));
	if (!ReceiveExact(Socket, Request.Body.GetData() + OldSize, Remaining, bStopping))
	{
		Request.Body.SetNum(OldSize);
		return false;
	}
	return true;
}

void FDeviceExplorerHostServer::HandleDashboardConnection(FSocket* Socket)
{
	FHttpRequest Request;
	if (!ReadHttpRequest(Socket, Request))
	{
		return;
	}
	RouteDashboardRequest(Socket, Request);
}

void FDeviceExplorerHostServer::HandleDeviceConnection(FSocket* Socket)
{
	FHttpRequest Request;
	if (!ReadHttpRequest(Socket, Request))
	{
		return;
	}
	RouteDeviceRequest(Socket, Request);
}

void FDeviceExplorerHostServer::RouteDashboardRequest(FSocket* Socket, FHttpRequest& Request)
{
	if (Request.Method == TEXT("GET") && Request.Path == TEXT("/health"))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("ok"));
		SendJsonResponse(Socket, 200, Result);
		return;
	}
	if (Request.Method == TEXT("GET") && Request.Path == TEXT("/api/config"))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetNumberField(TEXT("protocol_version"), DeviceExplorer::ProtocolVersion);
		Result->SetNumberField(TEXT("device_port"), Config.DevicePort);
		Result->SetStringField(TEXT("service_type"), TEXT("_ue-deviceexplorer._tcp.local."));
		SendJsonResponse(Socket, 200, Result);
		return;
	}
	if (Request.Method == TEXT("GET") && Request.Path == TEXT("/api/devices"))
	{
		HandleDevicesApi(Socket);
		return;
	}
	if (Request.Path.StartsWith(TEXT("/api/devices/")))
	{
		FString Remainder = Request.Path.RightChop(FCString::Strlen(TEXT("/api/devices/")));
		FString DeviceId;
		FString Action;
		if (Remainder.Split(TEXT("/"), &DeviceId, &Action))
		{
			HandleDeviceApi(Socket, Request, DeviceId, Action);
			return;
		}
	}
	if (Request.Path.StartsWith(TEXT("/api/transfers/")))
	{
		FString Remainder = Request.Path.RightChop(FCString::Strlen(TEXT("/api/transfers/")));
		FString TransferId = Remainder;
		FString Action;
		Remainder.Split(TEXT("/"), &TransferId, &Action);
		HandleTransferApi(Socket, Request, TransferId, Action);
		return;
	}
	if (Request.Method == TEXT("GET"))
	{
		const FString RelativePath = Request.Path == TEXT("/") ? TEXT("index.html") : Request.Path.RightChop(1);
		ServeStaticFile(Socket, RelativePath);
		return;
	}
	SendJsonError(Socket, 404, TEXT("Route not found"));
}

void FDeviceExplorerHostServer::RouteDeviceRequest(FSocket* Socket, FHttpRequest& Request)
{
	if (Request.Method == TEXT("GET") && Request.Path == TEXT("/health"))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("ok"));
		SendJsonResponse(Socket, 200, Result);
		return;
	}
	if (Request.Method == TEXT("GET") && Request.Path == TEXT("/device/connect"))
	{
		HandleWebSocket(Socket, Request);
		return;
	}
	if (Request.Method == TEXT("PUT") && Request.Path.StartsWith(TEXT("/device/transfers/")))
	{
		const FString TransferId = Request.Path.RightChop(FCString::Strlen(TEXT("/device/transfers/")));
		HandleTransferUpload(Socket, Request, TransferId);
		return;
	}
	SendJsonError(Socket, 404, TEXT("Route not found"));
}

void FDeviceExplorerHostServer::ServeStaticFile(FSocket* Socket, const FString& RelativePath)
{
	const FString Normalized = NormalizeRelativePath(RelativePath);
	if (Normalized.IsEmpty() || Normalized != RelativePath.Replace(TEXT("\\"), TEXT("/")))
	{
		SendJsonError(Socket, 403, TEXT("Invalid path"));
		return;
	}

	const FString FullPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(Config.WebRoot, Normalized));
	FString NormalizedRoot = FPaths::ConvertRelativePathToFull(Config.WebRoot);
	FPaths::NormalizeDirectoryName(NormalizedRoot);
	FString NormalizedFull = FullPath;
	FPaths::NormalizeFilename(NormalizedFull);
	if (!NormalizedFull.StartsWith(NormalizedRoot + TEXT("/"), ESearchCase::CaseSensitive))
	{
		SendJsonError(Socket, 403, TEXT("Invalid path"));
		return;
	}

	TArray<uint8> Body;
	if (!FFileHelper::LoadFileToArray(Body, *NormalizedFull))
	{
		SendJsonError(Socket, 404, TEXT("File not found"));
		return;
	}

	const bool bImmutableAsset = Normalized.StartsWith(TEXT("assets/"), ESearchCase::CaseSensitive);
	const FString CacheControl = bImmutableAsset
		                                 ? TEXT("public, max-age=31536000, immutable")
		                                 : TEXT("no-cache");
	SendHttpBytes(Socket,
	              200,
	              ContentTypeForPath(NormalizedFull),
	              Body,
	              { { TEXT("Cache-Control"), CacheControl },
	                { TEXT("Content-Security-Policy"),
	                  TEXT("default-src 'self'; connect-src 'self'; img-src 'self' data:; style-src 'self'; script-src 'self'; frame-ancestors 'none'") } });
}

void FDeviceExplorerHostServer::HandleWebSocket(FSocket* Socket, const FHttpRequest& Request)
{
	if (Request.Query.FindRef(TEXT("token")) != Config.Token)
	{
		SendJsonError(Socket, 401, TEXT("Invalid token"));
		return;
	}

	const FString ClientKey = Request.Headers.FindRef(TEXT("sec-websocket-key"));
	if (ClientKey.IsEmpty() || !Request.Headers.FindRef(TEXT("upgrade")).Equals(TEXT("websocket"), ESearchCase::IgnoreCase))
	{
		SendJsonError(Socket, 400, TEXT("Invalid WebSocket upgrade"));
		return;
	}

	const FString Upgrade = FString::Printf(TEXT("HTTP/1.1 101 Switching Protocols\r\n") TEXT("Upgrade: websocket\r\n") TEXT("Connection: Upgrade\r\n")
	                                            TEXT("Sec-WebSocket-Accept: %s\r\n\r\n"),
	                                        *WebSocketAcceptKey(ClientKey));
	if (!SendUtf8(Socket, Upgrade))
	{
		return;
	}

	const TSharedRef<FDeviceConnection> Connection = MakeShared<FDeviceConnection>(Socket);
	TSharedRef<FInternetAddr> LocalAddress = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	Socket->GetAddress(*LocalAddress);
	Connection->LocalAddress = LocalAddress->ToString(false);

	uint8 FragmentOpcode = 0;
	TArray<uint8> Fragment;
	while (!bStopping.Load())
	{
		uint8 Opcode = 0;
		bool bFinal = false;
		TArray<uint8> Payload;
		if (!ReadWebSocketFrame(Socket, bStopping, Opcode, bFinal, Payload))
		{
			break;
		}
		if (Opcode == 0x8)
		{
			break;
		}
		if (Opcode == 0x9)
		{
			Connection->SendPong(Payload);
			continue;
		}
		if (Opcode == 0x1)
		{
			FragmentOpcode = Opcode;
			Fragment = MoveTemp(Payload);
		}
		else if (Opcode == 0x0 && FragmentOpcode != 0)
		{
			Fragment.Append(Payload);
		}
		else
		{
			continue;
		}

		if (Fragment.Num() > static_cast<int32>(MaximumWebSocketPayloadBytes))
		{
			break;
		}
		if (bFinal && FragmentOpcode == 0x1)
		{
			HandleDeviceMessage(Connection, BytesToString(Fragment));
			Fragment.Reset();
			FragmentOpcode = 0;
		}
	}

	Connection->bClosed.Store(true);
	DetachDevice(Connection);
}

void FDeviceExplorerHostServer::HandleDeviceMessage(const TSharedRef<FDeviceConnection>& Connection, const FString& Message)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return;
	}

	FString Type;
	if (!Json->TryGetStringField(TEXT("type"), Type))
	{
		return;
	}
	if (Type == TEXT("hello"))
	{
		AttachDevice(Connection, Json);
		return;
	}

	if (Type == TEXT("transfer_result"))
	{
		bool bSuccess = false;
		FString TransferId;
		Json->TryGetBoolField(TEXT("success"), bSuccess);
		Json->TryGetStringField(TEXT("transfer_id"), TransferId);
		if (!bSuccess && !TransferId.IsEmpty())
		{
			FString Error;
			Json->TryGetStringField(TEXT("error"), Error);
			FScopeLock Lock(&StateMutex);
			if (const TSharedPtr<FTransfer>* Transfer = Transfers.Find(TransferId))
			{
				(*Transfer)->State = TEXT("failed");
				(*Transfer)->Error = Error;
				(*Transfer)->UpdatedAt = FDateTime::UtcNow();
			}
		}
		return;
	}

	// Replies are matched by request_id alone, so a new feature's response type does not have
	// to be listed here. Unmatched ids are ignored by CompletePendingRequest.
	FString RequestId;
	if (Json->TryGetStringField(TEXT("request_id"), RequestId) && !RequestId.IsEmpty())
	{
		CompletePendingRequest(RequestId, Json);
		return;
	}

	FScopeLock Lock(&StateMutex);
	TSharedPtr<FDeviceState>* Device = Devices.Find(Connection->DeviceId);
	if (Device == nullptr)
	{
		return;
	}
	(*Device)->LastSeen = FDateTime::UtcNow();

	if (Type == TEXT("heartbeat"))
	{
		double Uptime = 0.0;
		Json->TryGetNumberField(TEXT("uptime_seconds"), Uptime);
		(*Device)->UptimeSeconds = static_cast<int64>(Uptime);
		(*Device)->bConnected = true;
		return;
	}
	if (Type != TEXT("log_batch"))
	{
		return;
	}

	double Dropped = 0.0;
	Json->TryGetNumberField(TEXT("dropped"), Dropped);
	(*Device)->DroppedLogs += static_cast<uint64>(FMath::Max(0.0, Dropped));

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!Json->TryGetArrayField(TEXT("entries"), Entries) || Entries == nullptr)
	{
		return;
	}
	for (const TSharedPtr<FJsonValue>& Value : *Entries)
	{
		const TSharedPtr<FJsonObject> Entry = Value->AsObject();
		if (!Entry.IsValid())
		{
			continue;
		}
		FLogEntry Log;
		Log.Sequence = (*Device)->NextSequence++;
		Entry->TryGetStringField(TEXT("timestamp"), Log.Timestamp);
		Entry->TryGetStringField(TEXT("category"), Log.Category);
		Entry->TryGetStringField(TEXT("verbosity"), Log.Verbosity);
		Entry->TryGetStringField(TEXT("message"), Log.Message);
		if (Log.Timestamp.IsEmpty())
		{
			Log.Timestamp = FDateTime::UtcNow().ToIso8601();
		}
		Log.Category.LeftInline(256);
		Log.Verbosity.LeftInline(64);
		Log.Message.LeftInline(64 * 1024);
		(*Device)->Logs.Add(MoveTemp(Log));
	}
	const int32 Overflow = (*Device)->Logs.Num() - Config.LogCapacity;
	if (Overflow > 0)
	{
		(*Device)->Logs.PopFront(Overflow);
		(*Device)->DroppedLogs += Overflow;
	}
}

void FDeviceExplorerHostServer::AttachDevice(const TSharedRef<FDeviceConnection>& Connection, const TSharedPtr<FJsonObject>& Hello)
{
	FString DeviceId;
	if (!Hello->TryGetStringField(TEXT("device_id"), DeviceId) || DeviceId.IsEmpty())
	{
		return;
	}

	FScopeLock Lock(&StateMutex);
	TSharedPtr<FDeviceState>& Device = Devices.FindOrAdd(DeviceId);
	if (!Device)
	{
		Device = MakeShared<FDeviceState>();
		Device->Id = DeviceId;
		Device->Logs.Reserve(Config.LogCapacity);
	}
	if (Device->Connection && Device->Connection != Connection)
	{
		Device->Connection->bClosed.Store(true);
		Device->Connection->Socket->Shutdown(ESocketShutdownMode::ReadWrite);
	}

	Connection->DeviceId = DeviceId;
	Device->Connection = Connection;
	Device->bConnected = true;
	Device->ConnectedAt = FDateTime::UtcNow();
	Device->LastSeen = Device->ConnectedAt;
	Hello->TryGetStringField(TEXT("name"), Device->Name);
	Hello->TryGetStringField(TEXT("project_name"), Device->ProjectName);
	Hello->TryGetStringField(TEXT("engine_version"), Device->EngineVersion);
	Hello->TryGetStringField(TEXT("platform"), Device->Platform);
	Hello->TryGetStringField(TEXT("configuration"), Device->Configuration);
	Hello->TryGetStringField(TEXT("build_version"), Device->BuildVersion);

	double Number = 0.0;
	if (Hello->TryGetNumberField(TEXT("protocol_version"), Number))
	{
		Device->ProtocolVersion = static_cast<int32>(Number);
	}
	if (Hello->TryGetNumberField(TEXT("uptime_seconds"), Number))
	{
		Device->UptimeSeconds = static_cast<int64>(Number);
	}

	Device->Capabilities.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Capabilities = nullptr;
	if (Hello->TryGetArrayField(TEXT("capabilities"), Capabilities) && Capabilities != nullptr)
	{
		for (const TSharedPtr<FJsonValue>& Capability : *Capabilities)
		{
			Device->Capabilities.Add(Capability->AsString());
		}
	}

	Device->Commands.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Commands = nullptr;
	if (Hello->TryGetArrayField(TEXT("commands"), Commands) && Commands != nullptr)
	{
		Device->Commands = *Commands;
	}

	Device->FileRoots.Reset();
	const TArray<TSharedPtr<FJsonValue>>* FileRoots = nullptr;
	if (Hello->TryGetArrayField(TEXT("file_roots"), FileRoots) && FileRoots != nullptr)
	{
		Device->FileRoots = *FileRoots;
	}

	Device->DataModules.Reset();
	const TArray<TSharedPtr<FJsonValue>>* DataModules = nullptr;
	if (Hello->TryGetArrayField(TEXT("data_modules"), DataModules) && DataModules != nullptr)
	{
		Device->DataModules = *DataModules;
	}

	TSharedRef<FInternetAddr> RemoteAddress = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	if (Connection->Socket->GetPeerAddress(*RemoteAddress))
	{
		Device->RemoteAddress = RemoteAddress->ToString(true);
	}
	UE_LOG(LogDeviceExplorerHost, Display, TEXT("Device connected: %s (%s)"), *Device->Name, *DeviceId);
}

void FDeviceExplorerHostServer::DetachDevice(const TSharedRef<FDeviceConnection>& Connection)
{
	FScopeLock Lock(&StateMutex);
	if (TSharedPtr<FDeviceState>* Device = Devices.Find(Connection->DeviceId))
	{
		if ((*Device)->Connection == Connection)
		{
			(*Device)->Connection.Reset();
			(*Device)->bConnected = false;
			(*Device)->LastSeen = FDateTime::UtcNow();
		}
	}
}

void FDeviceExplorerHostServer::HandleDevicesApi(FSocket* Socket)
{
	TArray<TSharedPtr<FJsonValue>> ResultDevices;
	{
		FScopeLock Lock(&StateMutex);
		const FDateTime Now = FDateTime::UtcNow();
		for (TPair<FString, TSharedPtr<FDeviceState>>& Pair : Devices)
		{
			FDeviceState& Device = *Pair.Value;
			if (Device.bConnected && (Now - Device.LastSeen).GetTotalSeconds() > 15.0)
			{
				Device.bConnected = false;
			}

			TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
			Json->SetStringField(TEXT("id"), Device.Id);
			Json->SetStringField(TEXT("name"), Device.Name);
			Json->SetStringField(TEXT("project_name"), Device.ProjectName);
			Json->SetStringField(TEXT("engine_version"), Device.EngineVersion);
			Json->SetStringField(TEXT("platform"), Device.Platform);
			Json->SetStringField(TEXT("configuration"), Device.Configuration);
			Json->SetStringField(TEXT("build_version"), Device.BuildVersion);
			Json->SetNumberField(TEXT("protocol_version"), Device.ProtocolVersion);
			Json->SetNumberField(TEXT("uptime_seconds"), static_cast<double>(Device.UptimeSeconds));
			Json->SetBoolField(TEXT("connected"), Device.bConnected);
			Json->SetStringField(TEXT("connected_at"), Device.ConnectedAt.ToIso8601());
			Json->SetStringField(TEXT("last_seen"), Device.LastSeen.ToIso8601());
			Json->SetStringField(TEXT("remote_address"), Device.RemoteAddress);
			Json->SetNumberField(TEXT("dropped_logs"), static_cast<double>(Device.DroppedLogs));
			Json->SetNumberField(TEXT("log_count"), Device.Logs.Num());

			TArray<TSharedPtr<FJsonValue>> Capabilities;
			for (const FString& Capability : Device.Capabilities)
			{
				Capabilities.Add(MakeShared<FJsonValueString>(Capability));
			}
			Json->SetArrayField(TEXT("capabilities"), MoveTemp(Capabilities));
			Json->SetArrayField(TEXT("commands"), Device.Commands);
			Json->SetArrayField(TEXT("file_roots"), Device.FileRoots);
			Json->SetArrayField(TEXT("data_modules"), Device.DataModules);
			ResultDevices.Add(MakeShared<FJsonValueObject>(Json));
		}
	}

	ResultDevices.Sort(
		[](const TSharedPtr<FJsonValue>& Left, const TSharedPtr<FJsonValue>& Right)
		{
			const TSharedPtr<FJsonObject> LeftObject = Left->AsObject();
			const TSharedPtr<FJsonObject> RightObject = Right->AsObject();
			const bool bLeftConnected = LeftObject->GetBoolField(TEXT("connected"));
			const bool bRightConnected = RightObject->GetBoolField(TEXT("connected"));
			if (bLeftConnected != bRightConnected)
			{
				return bLeftConnected;
			}
			return LeftObject->GetStringField(TEXT("name")) < RightObject->GetStringField(TEXT("name"));
		});

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("devices"), MoveTemp(ResultDevices));
	SendJsonResponse(Socket, 200, Root);
}

void FDeviceExplorerHostServer::HandleDeviceApi(FSocket* Socket, FHttpRequest& Request, const FString& DeviceId, const FString& Action)
{
	if (Action == TEXT("logs") && Request.Method == TEXT("GET"))
	{
		const uint64 After = FCString::Strtoui64(*Request.Query.FindRef(TEXT("after")), nullptr, 10);
		const FString Category = Request.Query.FindRef(TEXT("category"));
		const FString Verbosity = Request.Query.FindRef(TEXT("verbosity"));

		TArray<TSharedPtr<FJsonValue>> Entries;
		uint64 Dropped = 0;
		int32 Buffered = 0;
		{
			FScopeLock Lock(&StateMutex);
			const TSharedPtr<FDeviceState>* Device = Devices.Find(DeviceId);
			if (Device == nullptr)
			{
				SendJsonError(Socket, 404, TEXT("Unknown device"));
				return;
			}
			Dropped = (*Device)->DroppedLogs;
			Buffered = (*Device)->Logs.Num();
			for (const FLogEntry& Log : (*Device)->Logs)
			{
				if (Log.Sequence <= After || (!Category.IsEmpty() && !Log.Category.Contains(Category, ESearchCase::IgnoreCase)) ||
				    (!Verbosity.IsEmpty() && !Log.Verbosity.Equals(Verbosity, ESearchCase::IgnoreCase)))
				{
					continue;
				}

				TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
				Json->SetNumberField(TEXT("sequence"), static_cast<double>(Log.Sequence));
				Json->SetStringField(TEXT("timestamp"), Log.Timestamp);
				Json->SetStringField(TEXT("category"), Log.Category);
				Json->SetStringField(TEXT("verbosity"), Log.Verbosity);
				Json->SetStringField(TEXT("message"), Log.Message);
				Entries.Add(MakeShared<FJsonValueObject>(Json));
				if (Entries.Num() >= 2000)
				{
					break;
				}
			}
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetArrayField(TEXT("entries"), MoveTemp(Entries));
		Result->SetNumberField(TEXT("dropped"), static_cast<double>(Dropped));
		Result->SetNumberField(TEXT("buffered"), Buffered);
		Result->SetNumberField(TEXT("capacity"), Config.LogCapacity);
		SendJsonResponse(Socket, 200, Result);
		return;
	}

	if (Action == TEXT("log-categories") && Request.Method == TEXT("GET"))
	{
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("list_log_categories"));
		Message->SetStringField(TEXT("request_id"), NewRequestId());
		const TSharedPtr<FJsonObject> Result = SendDeviceRequestAndWait(DeviceId, Message);
		if (!Result.IsValid())
		{
			SendJsonError(Socket, 504, TEXT("Device did not answer the log category request"));
			return;
		}
		SendJsonResponse(Socket, 200, Result.ToSharedRef());
		return;
	}

	if (Action == TEXT("log-verbosity") && Request.Method == TEXT("POST"))
	{
		if (Request.Headers.FindRef(TEXT("x-deviceexplorer-request")) != TEXT("1") || !ReadRequestBody(Socket, Request, MaximumJsonBodyBytes))
		{
			SendJsonError(Socket, 400, TEXT("Invalid request"));
			return;
		}
		const TSharedPtr<FJsonObject> Body = ParseJson(Request.Body);
		const TArray<TSharedPtr<FJsonValue>>* Levels = nullptr;
		if (!Body.IsValid() || !Body->TryGetArrayField(TEXT("entries"), Levels))
		{
			SendJsonError(Socket, 400, TEXT("Invalid JSON"));
			return;
		}

		bool bPersist = false;
		bool bAutoRevert = true;
		Body->TryGetBoolField(TEXT("persist"), bPersist);
		Body->TryGetBoolField(TEXT("auto_revert"), bAutoRevert);

		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("set_log_verbosity"));
		Message->SetStringField(TEXT("request_id"), NewRequestId());
		Message->SetArrayField(TEXT("entries"), *Levels);
		Message->SetBoolField(TEXT("persist"), bPersist);
		Message->SetBoolField(TEXT("auto_revert"), bAutoRevert);
		const TSharedPtr<FJsonObject> Result = SendDeviceRequestAndWait(DeviceId, Message);
		if (!Result.IsValid())
		{
			SendJsonError(Socket, 504, TEXT("Device did not apply the log levels"));
			return;
		}
		SendJsonResponse(Socket, 200, Result.ToSharedRef());
		return;
	}

	if (Action == TEXT("command") && Request.Method == TEXT("POST"))
	{
		if (Request.Headers.FindRef(TEXT("x-deviceexplorer-request")) != TEXT("1") || !ReadRequestBody(Socket, Request, MaximumJsonBodyBytes))
		{
			SendJsonError(Socket, 400, TEXT("Invalid request"));
			return;
		}
		const TSharedPtr<FJsonObject> Body = ParseJson(Request.Body);
		if (!Body.IsValid())
		{
			SendJsonError(Socket, 400, TEXT("Invalid JSON"));
			return;
		}

		FString Command;
		FString CommandId;
		FString Arguments;
		Body->TryGetStringField(TEXT("command"), Command);
		Body->TryGetStringField(TEXT("command_id"), CommandId);
		Body->TryGetStringField(TEXT("arguments"), Arguments);
		if (Command.IsEmpty() && CommandId.IsEmpty())
		{
			SendJsonError(Socket, 400, TEXT("Command is empty"));
			return;
		}

		const FString RequestId = NewRequestId();
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("execute_command"));
		Message->SetStringField(TEXT("request_id"), RequestId);
		Message->SetStringField(TEXT("command"), Command);
		Message->SetStringField(TEXT("command_id"), CommandId);
		Message->SetStringField(TEXT("arguments"), Arguments);
		const TSharedPtr<FJsonObject> Result = SendDeviceRequestAndWait(DeviceId, Message);
		if (!Result.IsValid())
		{
			SendJsonError(Socket, 504, TEXT("Device request timed out"));
			return;
		}
		SendJsonResponse(Socket, 200, Result.ToSharedRef());
		return;
	}

	if (Action == TEXT("console-objects") && Request.Method == TEXT("GET"))
	{
		const FString RequestId = NewRequestId();
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("list_console_objects"));
		Message->SetStringField(TEXT("request_id"), RequestId);
		Message->SetStringField(TEXT("query"), Request.Query.FindRef(TEXT("q")));
		const int32 RequestedLimit = FCString::Atoi(*Request.Query.FindRef(TEXT("limit")));
		Message->SetNumberField(TEXT("limit"), RequestedLimit > 0 ? RequestedLimit : 400);
		const TSharedPtr<FJsonObject> Result = SendDeviceRequestAndWait(DeviceId, Message);
		if (!Result.IsValid())
		{
			SendJsonError(Socket, 504, TEXT("Device did not answer the console catalog request"));
			return;
		}
		SendJsonResponse(Socket, 200, Result.ToSharedRef());
		return;
	}

	if (Action == TEXT("files") && Request.Method == TEXT("GET"))
	{
		const FString RequestId = NewRequestId();
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("list_files"));
		Message->SetStringField(TEXT("request_id"), RequestId);
		Message->SetStringField(TEXT("root"), Request.Query.FindRef(TEXT("root")));
		Message->SetStringField(TEXT("path"), Request.Query.FindRef(TEXT("path")));
		const TSharedPtr<FJsonObject> Result = SendDeviceRequestAndWait(DeviceId, Message);
		if (!Result.IsValid())
		{
			SendJsonError(Socket, 504, TEXT("Device request timed out"));
			return;
		}
		SendJsonResponse(Socket, 200, Result.ToSharedRef());
		return;
	}

	if (Action == TEXT("module-data") && Request.Method == TEXT("GET"))
	{
		const FString ModuleName = Request.Query.FindRef(TEXT("module"));
		if (ModuleName.IsEmpty())
		{
			SendJsonError(Socket, 400, TEXT("Module is required"));
			return;
		}

		const FString RequestId = NewRequestId();
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("get_module_data"));
		Message->SetStringField(TEXT("request_id"), RequestId);
		Message->SetStringField(TEXT("module"), ModuleName);
		const TSharedPtr<FJsonObject> Result = SendDeviceRequestAndWait(DeviceId, Message);
		if (!Result.IsValid())
		{
			SendJsonError(Socket, 504, TEXT("Device did not answer the module request"));
			return;
		}
		SendJsonResponse(Socket, 200, Result.ToSharedRef());
		return;
	}

	if (Action == TEXT("module-action") && Request.Method == TEXT("POST"))
	{
		if (Request.Headers.FindRef(TEXT("x-deviceexplorer-request")) != TEXT("1") || !ReadRequestBody(Socket, Request, MaximumJsonBodyBytes))
		{
			SendJsonError(Socket, 400, TEXT("Invalid request"));
			return;
		}
		const TSharedPtr<FJsonObject> Body = ParseJson(Request.Body);
		FString ModuleName;
		FString ActionName;
		if (!Body.IsValid() || !Body->TryGetStringField(TEXT("module"), ModuleName) || !Body->TryGetStringField(TEXT("action"), ActionName))
		{
			SendJsonError(Socket, 400, TEXT("Module and action are required"));
			return;
		}

		const TSharedPtr<FJsonObject>* Parameters = nullptr;
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("invoke_module_action"));
		Message->SetStringField(TEXT("request_id"), NewRequestId());
		Message->SetStringField(TEXT("module"), ModuleName);
		Message->SetStringField(TEXT("action"), ActionName);
		if (Body->TryGetObjectField(TEXT("parameters"), Parameters) && Parameters != nullptr && Parameters->IsValid())
		{
			Message->SetObjectField(TEXT("parameters"), *Parameters);
		}
		else
		{
			Message->SetObjectField(TEXT("parameters"), MakeShared<FJsonObject>());
		}

		const TSharedPtr<FJsonObject> Result = SendDeviceRequestAndWait(DeviceId, Message);
		if (!Result.IsValid())
		{
			SendJsonError(Socket, 504, TEXT("Device did not answer the module action"));
			return;
		}
		SendJsonResponse(Socket, 200, Result.ToSharedRef());
		return;
	}

	if (Action == TEXT("transfers") && Request.Method == TEXT("POST"))
	{
		if (Request.Headers.FindRef(TEXT("x-deviceexplorer-request")) != TEXT("1") || !ReadRequestBody(Socket, Request, MaximumJsonBodyBytes))
		{
			SendJsonError(Socket, 400, TEXT("Invalid request"));
			return;
		}
		const TSharedPtr<FJsonObject> Body = ParseJson(Request.Body);
		FString Root;
		FString RelativePath;
		if (!Body.IsValid() || !Body->TryGetStringField(TEXT("root"), Root) || !Body->TryGetStringField(TEXT("path"), RelativePath))
		{
			SendJsonError(Socket, 400, TEXT("Missing transfer path"));
			return;
		}
		bool bArchive = false;
		Body->TryGetBoolField(TEXT("archive"), bArchive);
		RelativePath = NormalizeRelativePath(RelativePath);
		if (Root.IsEmpty() || (RelativePath.IsEmpty() && !bArchive))
		{
			SendJsonError(Socket, 400, TEXT("Invalid transfer path"));
			return;
		}

		TSharedPtr<FDeviceConnection> Connection;
		{
			FScopeLock Lock(&StateMutex);
			const TSharedPtr<FDeviceState>* Device = Devices.Find(DeviceId);
			if (Device == nullptr || !(*Device)->bConnected || !(*Device)->Connection)
			{
				SendJsonError(Socket, 404, TEXT("Device is offline"));
				return;
			}
			Connection = (*Device)->Connection;
		}

		const FString TransferId = NewRequestId();
		const FDateTime Now = FDateTime::UtcNow();
		const TSharedRef<FTransfer> Transfer = MakeShared<FTransfer>();
		Transfer->Id = TransferId;
		Transfer->DeviceId = DeviceId;
		Transfer->Root = Root;
		Transfer->RelativePath = RelativePath;
		Transfer->Filename = bArchive ? SafeFilename(RelativePath.IsEmpty() ? Root : RelativePath) + TEXT(".zip") : SafeFilename(RelativePath);
		Transfer->State = TEXT("requested");
		Transfer->CreatedAt = Now;
		Transfer->UpdatedAt = Now;
		Transfer->LocalPath = FPaths::Combine(Config.TransferDirectory, TransferId + TEXT(".bin"));
		{
			FScopeLock Lock(&StateMutex);
			Transfers.Add(TransferId, Transfer);
		}

		const FString UploadHost = Connection->LocalAddress.IsEmpty() ? TEXT("127.0.0.1") : Connection->LocalAddress;
		TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
		Message->SetStringField(TEXT("type"), TEXT("upload_file"));
		Message->SetStringField(TEXT("request_id"), NewRequestId());
		Message->SetStringField(TEXT("transfer_id"), TransferId);
		Message->SetStringField(TEXT("root"), Root);
		Message->SetStringField(TEXT("path"), RelativePath);
		Message->SetBoolField(TEXT("archive"), bArchive);
		Message->SetStringField(TEXT("upload_url"),
		                        FString::Printf(TEXT("http://%s:%d/device/transfers/%s?token=%s"), *UploadHost, Config.DevicePort, *TransferId, *Config.Token));
		if (!Connection->SendText(JsonString(Message)))
		{
			FScopeLock Lock(&StateMutex);
			Transfer->State = TEXT("failed");
			Transfer->Error = TEXT("Cannot send transfer request");
			Transfer->UpdatedAt = FDateTime::UtcNow();
			SendJsonError(Socket, 502, Transfer->Error);
			return;
		}

		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("id"), Transfer->Id);
		Result->SetStringField(TEXT("device_id"), DeviceId);
		Result->SetStringField(TEXT("root"), Root);
		Result->SetStringField(TEXT("path"), RelativePath);
		Result->SetStringField(TEXT("filename"), Transfer->Filename);
		Result->SetStringField(TEXT("state"), Transfer->State);
		Result->SetNumberField(TEXT("bytes"), 0);
		SendJsonResponse(Socket, 202, Result);
		return;
	}

	SendJsonError(Socket, 404, TEXT("Route not found"));
}

void FDeviceExplorerHostServer::HandleTransferApi(FSocket* Socket, FHttpRequest& Request, const FString& TransferId, const FString& Action)
{
	TSharedPtr<FTransfer> Transfer;
	{
		FScopeLock Lock(&StateMutex);
		const TSharedPtr<FTransfer>* Found = Transfers.Find(TransferId);
		if (Found != nullptr)
		{
			Transfer = *Found;
		}
	}
	if (!Transfer)
	{
		SendJsonError(Socket, 404, TEXT("Unknown transfer"));
		return;
	}

	if (Action.IsEmpty() && Request.Method == TEXT("GET"))
	{
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		{
			FScopeLock Lock(&StateMutex);
			Result->SetStringField(TEXT("id"), Transfer->Id);
			Result->SetStringField(TEXT("device_id"), Transfer->DeviceId);
			Result->SetStringField(TEXT("root"), Transfer->Root);
			Result->SetStringField(TEXT("path"), Transfer->RelativePath);
			Result->SetStringField(TEXT("filename"), Transfer->Filename);
			Result->SetStringField(TEXT("state"), Transfer->State);
			Result->SetNumberField(TEXT("bytes"), static_cast<double>(Transfer->Bytes));
			Result->SetStringField(TEXT("error"), Transfer->Error);
			Result->SetStringField(TEXT("created_at"), Transfer->CreatedAt.ToIso8601());
			Result->SetStringField(TEXT("updated_at"), Transfer->UpdatedAt.ToIso8601());
		}
		SendJsonResponse(Socket, 200, Result);
		return;
	}

	if (Action == TEXT("download") && Request.Method == TEXT("GET"))
	{
		FString Path;
		FString Filename;
		int64 Size = 0;
		{
			FScopeLock Lock(&StateMutex);
			if (Transfer->State != TEXT("ready"))
			{
				SendJsonError(Socket, 409, TEXT("Transfer is not ready"));
				return;
			}
			Path = Transfer->LocalPath;
			Filename = Transfer->Filename;
			Size = IFileManager::Get().FileSize(*Path);
		}
		if (Size < 0)
		{
			SendJsonError(Socket, 404, TEXT("Transfer file is missing"));
			return;
		}

		TMap<FString, FString> Headers;
		Headers.Add(TEXT("Content-Disposition"), FString::Printf(TEXT("attachment; filename=\"%s\""), *SafeFilename(Filename)));
		if (!SendHttpHeaders(Socket, 200, TEXT("application/octet-stream"), Size, Headers))
		{
			return;
		}

		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		TUniquePtr<IFileHandle> File(PlatformFile.OpenRead(*Path));
		if (!File)
		{
			return;
		}
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(IoChunkBytes);
		int64 Remaining = Size;
		while (Remaining > 0 && !bStopping.Load())
		{
			const int64 Chunk = FMath::Min<int64>(Remaining, Buffer.Num());
			if (!File->Read(Buffer.GetData(), Chunk) || !SendAllBounded(Socket, Buffer.GetData(), Chunk, MaximumSocketSendSeconds))
			{
				break;
			}
			Remaining -= Chunk;
		}
		return;
	}

	if (Action == TEXT("trace") && Request.Method == TEXT("POST"))
	{
		if (Request.Headers.FindRef(TEXT("x-deviceexplorer-request")) != TEXT("1"))
		{
			SendJsonError(Socket, 400, TEXT("Invalid request"));
			return;
		}
		FString Error;
		if (!ForwardTrace(TransferId, Error))
		{
			SendJsonError(Socket, 502, Error);
			return;
		}
		TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
		Result->SetStringField(TEXT("status"), TEXT("sent"));
		SendJsonResponse(Socket, 200, Result);
		return;
	}

	SendJsonError(Socket, 404, TEXT("Route not found"));
}

void FDeviceExplorerHostServer::HandleTransferUpload(FSocket* Socket, FHttpRequest& Request, const FString& TransferId)
{
	if (Request.Query.FindRef(TEXT("token")) != Config.Token)
	{
		SendJsonError(Socket, 401, TEXT("Invalid token"));
		return;
	}
	if (Request.ContentLength <= 0)
	{
		SendJsonError(Socket, 411, TEXT("Content-Length is required"));
		return;
	}
	if (Request.ContentLength > Config.MaximumTransferBytes)
	{
		SendJsonError(Socket, 413, TEXT("Transfer is too large"));
		return;
	}

	TSharedPtr<FTransfer> Transfer;
	{
		FScopeLock Lock(&StateMutex);
		const TSharedPtr<FTransfer>* Found = Transfers.Find(TransferId);
		if (Found != nullptr)
		{
			Transfer = *Found;
		}
		if (!Transfer)
		{
			SendJsonError(Socket, 404, TEXT("Unknown transfer"));
			return;
		}
		if (Transfer->State != TEXT("requested") && Transfer->State != TEXT("failed"))
		{
			SendJsonError(Socket, 409, TEXT("Transfer is not uploadable"));
			return;
		}
		Transfer->State = TEXT("uploading");
		Transfer->Error.Reset();
		Transfer->Bytes = 0;
		Transfer->UpdatedAt = FDateTime::UtcNow();
	}

	const FString PartPath = Transfer->LocalPath + TEXT(".part");
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> File(PlatformFile.OpenWrite(*PartPath, false, false));
	if (!File)
	{
		{
			FScopeLock Lock(&StateMutex);
			Transfer->State = TEXT("failed");
			Transfer->Error = TEXT("Cannot create transfer file");
			Transfer->UpdatedAt = FDateTime::UtcNow();
		}
		SendJsonError(Socket, 500, TEXT("Cannot create transfer file"));
		return;
	}

	int64 Written = 0;
	if (!Request.Body.IsEmpty())
	{
		const int64 PrefixBytes = FMath::Min<int64>(Request.Body.Num(), Request.ContentLength);
		if (!File->Write(Request.Body.GetData(), PrefixBytes))
		{
			File.Reset();
			IFileManager::Get().Delete(*PartPath);
			{
				FScopeLock Lock(&StateMutex);
				Transfer->State = TEXT("failed");
				Transfer->Error = TEXT("Cannot write transfer file");
				Transfer->UpdatedAt = FDateTime::UtcNow();
			}
			SendJsonError(Socket, 500, TEXT("Cannot write transfer file"));
			return;
		}
		Written += PrefixBytes;
	}

	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(IoChunkBytes);
	double LastProgressSeconds = FPlatformTime::Seconds();
	while (Written < Request.ContentLength && !bStopping.Load())
	{
		if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromMilliseconds(250)))
		{
			if (FPlatformTime::Seconds() - LastProgressSeconds > MaximumSocketIdleSeconds)
			{
				break;
			}
			continue;
		}
		const int32 Wanted = static_cast<int32>(FMath::Min<int64>(Request.ContentLength - Written, Buffer.Num()));
		int32 Read = 0;
		if (!Socket->Recv(Buffer.GetData(), Wanted, Read, ESocketReceiveFlags::None) || Read <= 0 || !File->Write(Buffer.GetData(), Read))
		{
			break;
		}
		Written += Read;
		LastProgressSeconds = FPlatformTime::Seconds();
		{
			FScopeLock Lock(&StateMutex);
			Transfer->Bytes = Written;
			Transfer->UpdatedAt = FDateTime::UtcNow();
		}
	}
	File.Reset();

	const bool bComplete = Written == Request.ContentLength;
	const bool bMoved = bComplete && IFileManager::Get().Move(*Transfer->LocalPath, *PartPath, true, true, false, false);
	if (!bComplete || !bMoved)
	{
		IFileManager::Get().Delete(*PartPath);
		{
			FScopeLock Lock(&StateMutex);
			Transfer->State = TEXT("failed");
			Transfer->Error = bComplete ? TEXT("Cannot finalize transfer file") : TEXT("Upload ended before Content-Length");
			Transfer->UpdatedAt = FDateTime::UtcNow();
		}
		SendJsonError(Socket, 500, Transfer->Error);
		return;
	}

	{
		FScopeLock Lock(&StateMutex);
		Transfer->State = TEXT("ready");
		Transfer->Bytes = Written;
		Transfer->UpdatedAt = FDateTime::UtcNow();
	}
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("status"), TEXT("ready"));
	Result->SetNumberField(TEXT("bytes"), static_cast<double>(Written));
	SendJsonResponse(Socket, 200, Result);
}

TSharedPtr<FJsonObject> FDeviceExplorerHostServer::SendDeviceRequestAndWait(const FString& DeviceId, const TSharedRef<FJsonObject>& Message)
{
	const FString RequestId = Message->GetStringField(TEXT("request_id"));
	const TSharedRef<FPendingRequest> Pending = MakeShared<FPendingRequest>();
	{
		FScopeLock Lock(&StateMutex);
		PendingRequests.Add(RequestId, Pending);
	}

	if (!SendDeviceJson(DeviceId, Message))
	{
		FScopeLock Lock(&StateMutex);
		PendingRequests.Remove(RequestId);
		return nullptr;
	}

	Pending->Event->Wait(FTimespan::FromSeconds(Config.RequestTimeoutSeconds));
	TSharedPtr<FJsonObject> Result;
	{
		FScopeLock Lock(&StateMutex);
		Result = Pending->Result;
		PendingRequests.Remove(RequestId);
	}
	return Result;
}

bool FDeviceExplorerHostServer::SendDeviceJson(const FString& DeviceId, const TSharedRef<FJsonObject>& Message)
{
	TSharedPtr<FDeviceConnection> Connection;
	{
		FScopeLock Lock(&StateMutex);
		const TSharedPtr<FDeviceState>* Device = Devices.Find(DeviceId);
		if (Device == nullptr || !(*Device)->bConnected || !(*Device)->Connection)
		{
			return false;
		}
		Connection = (*Device)->Connection;
	}
	return Connection->SendText(JsonString(Message));
}

void FDeviceExplorerHostServer::CompletePendingRequest(const FString& RequestId, const TSharedPtr<FJsonObject>& Result)
{
	TSharedPtr<FPendingRequest> Pending;
	{
		FScopeLock Lock(&StateMutex);
		const TSharedPtr<FPendingRequest>* Found = PendingRequests.Find(RequestId);
		if (Found != nullptr)
		{
			Pending = *Found;
			Pending->Result = Result;
		}
	}
	if (Pending)
	{
		Pending->Event->Trigger();
	}
}

bool FDeviceExplorerHostServer::ForwardTrace(const FString& TransferId, FString& OutError)
{
	FString Path;
	{
		FScopeLock Lock(&StateMutex);
		const TSharedPtr<FTransfer>* Transfer = Transfers.Find(TransferId);
		if (Transfer == nullptr || (*Transfer)->State != TEXT("ready"))
		{
			OutError = TEXT("Transfer is not ready");
			return false;
		}
		Path = (*Transfer)->LocalPath;
	}

	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	TUniquePtr<IFileHandle> File(PlatformFile.OpenRead(*Path));
	if (!File)
	{
		OutError = TEXT("Trace file is missing");
		return false;
	}

	uint8 Magic[4] = {};
	if (!File->Read(Magic, UE_ARRAY_COUNT(Magic)) || Magic[0] != 'T' || Magic[1] != 'R' || Magic[2] != 'C' || Magic[3] != '2')
	{
		OutError = TEXT("File does not start with TRC2");
		return false;
	}
	File->Seek(0);

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	FSocket* TraceSocket = SocketSubsystem->CreateSocket(NAME_Stream, TEXT("DeviceExplorerTraceForward"), false);
	if (TraceSocket == nullptr)
	{
		OutError = TEXT("Cannot create trace socket");
		return false;
	}

	TSharedRef<FInternetAddr> Address = SocketSubsystem->CreateInternetAddr();
	bool bValid = false;
	Address->SetIp(TEXT("127.0.0.1"), bValid);
	Address->SetPort(Config.TracePort);
	if (!bValid || !TraceSocket->Connect(*Address))
	{
		TraceSocket->Close();
		SocketSubsystem->DestroySocket(TraceSocket);
		OutError = FString::Printf(TEXT("Cannot connect to Unreal Trace Server on port %d"), Config.TracePort);
		return false;
	}

	bool bSuccess = true;
	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(IoChunkBytes);
	int64 Remaining = File->Size();
	while (Remaining > 0)
	{
		const int64 Chunk = FMath::Min<int64>(Remaining, Buffer.Num());
		if (!File->Read(Buffer.GetData(), Chunk) || !SendAllBounded(TraceSocket, Buffer.GetData(), Chunk, MaximumSocketSendSeconds))
		{
			bSuccess = false;
			break;
		}
		Remaining -= Chunk;
	}
	TraceSocket->Close();
	SocketSubsystem->DestroySocket(TraceSocket);
	if (!bSuccess)
	{
		OutError = TEXT("Trace forwarding was interrupted");
	}
	return bSuccess;
}

void FDeviceExplorerHostServer::CleanupExpiredState()
{
	const FDateTime Now = FDateTime::UtcNow();
	TArray<FString> Paths;
	{
		FScopeLock Lock(&StateMutex);
		for (auto Iterator = Transfers.CreateIterator(); Iterator; ++Iterator)
		{
			if ((Now - Iterator.Value()->UpdatedAt).GetTotalSeconds() > Config.TransferTtlSeconds)
			{
				Paths.Add(Iterator.Value()->LocalPath);
				Paths.Add(Iterator.Value()->LocalPath + TEXT(".part"));
				Iterator.RemoveCurrent();
			}
		}

		for (auto Iterator = Devices.CreateIterator(); Iterator; ++Iterator)
		{
			const FDeviceState& Device = *Iterator.Value();
			if (!Device.Connection && (Now - Device.LastSeen).GetTotalSeconds() > DisconnectedDeviceExpirySeconds)
			{
				Iterator.RemoveCurrent();
			}
		}
	}
	for (const FString& Path : Paths)
	{
		IFileManager::Get().Delete(*Path, false, true);
	}
}
