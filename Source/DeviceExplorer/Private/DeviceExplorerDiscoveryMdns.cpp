#include "Async/Async.h"
#include "Common/UdpSocketBuilder.h"
#include "Containers/StringConv.h"
#include "DeviceExplorerDiscovery.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "HAL/UnrealMemory.h"
#include "IPAddress.h"
#include "Interfaces/IPv4/IPv4Address.h"
#include "Interfaces/IPv4/IPv4Endpoint.h"
#include "Misc/Timespan.h"
#include "Modules/ModuleManager.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

#include <atomic>

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerDiscoveryMdns, Log, All);

namespace
{
constexpr uint16 MdnsPort = 5353;
constexpr double FastQueryIntervalSeconds = 5.0;
constexpr double SlowQueryIntervalSeconds = 30.0;
constexpr int32 MaxNameCompressionJumps = 64;
constexpr uint16 DnsClassMask = 0x7FFF;
constexpr uint16 DnsClassIN = 1;
const FString ServiceName(TEXT("_ue-deviceexplorer._tcp.local"));

void AddU16(TArray<uint8>& Buffer, const uint16 Value)
{
	Buffer.Add(static_cast<uint8>((Value >> 8) & 0xff));
	Buffer.Add(static_cast<uint8>(Value & 0xff));
}

void AddName(TArray<uint8>& Buffer, const FString& Name)
{
	TArray<FString> Labels;
	Name.ParseIntoArray(Labels, TEXT("."), true);
	for (const FString& Label : Labels)
	{
		const FTCHARToUTF8 Utf8(*Label);
		const int32 Length = FMath::Min(Utf8.Length(), 63);
		Buffer.Add(static_cast<uint8>(Length));
		Buffer.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Length);
	}
	Buffer.Add(0);
}

TArray<uint8> BuildQuery()
{
	TArray<uint8> Packet;
	AddU16(Packet, 0);    // ID
	AddU16(Packet, 0);    // Flags: standard query
	AddU16(Packet, 1);    // QDCOUNT
	AddU16(Packet, 0);    // ANCOUNT
	AddU16(Packet, 0);    // NSCOUNT
	AddU16(Packet, 0);    // ARCOUNT
	AddName(Packet, ServiceName);
	AddU16(Packet, 12);    // QTYPE: PTR
	AddU16(Packet, DnsClassIN);
	return Packet;
}

bool ReadU16(const uint8* Data, const int32 PacketLen, const int32 Offset, uint16& OutValue)
{
	if (Offset < 0 || Offset + 2 > PacketLen)
	{
		return false;
	}
	OutValue = (static_cast<uint16>(Data[Offset]) << 8) | Data[Offset + 1];
	return true;
}

FString DecodeChars(const uint8* Data, const int32 Offset, const int32 Length)
{
	TArray<ANSICHAR> Buffer;
	Buffer.SetNumUninitialized(Length + 1);
	FMemory::Memcpy(Buffer.GetData(), Data + Offset, Length);
	Buffer[Length] = 0;
	const FUTF8ToTCHAR Conv(Buffer.GetData());
	return FString(Conv.Get());
}

// DNS name compression (RFC 1035 4.1.4): a length byte with both top bits set is a
// pointer to an earlier offset in the packet. Pointers must move strictly backwards
// so a malformed packet cannot make this loop forever.
bool ReadName(const uint8* Data, const int32 PacketLen, int32& InOutOffset, FString& OutName)
{
	OutName.Reset();
	int32 CurrentOffset = InOutOffset;
	int32 ResumeOffset = -1;
	int32 JumpCount = 0;
	for (;;)
	{
		if (CurrentOffset < 0 || CurrentOffset >= PacketLen)
		{
			return false;
		}

		const uint8 LengthByte = Data[CurrentOffset];
		if ((LengthByte & 0xC0) == 0xC0)
		{
			uint16 PointerWord = 0;
			if (!ReadU16(Data, PacketLen, CurrentOffset, PointerWord))
			{
				return false;
			}
			const int32 PointerOffset = PointerWord & 0x3FFF;
			if (PointerOffset >= CurrentOffset || ++JumpCount > MaxNameCompressionJumps)
			{
				return false;
			}
			if (ResumeOffset < 0)
			{
				ResumeOffset = CurrentOffset + 2;
			}
			CurrentOffset = PointerOffset;
			continue;
		}
		if ((LengthByte & 0xC0) != 0)
		{
			return false;
		}
		if (LengthByte == 0)
		{
			CurrentOffset += 1;
			break;
		}

		const int32 LabelLength = LengthByte;
		const int32 LabelStart = CurrentOffset + 1;
		if (LabelStart + LabelLength > PacketLen)
		{
			return false;
		}
		if (!OutName.IsEmpty())
		{
			OutName += TEXT(".");
		}
		OutName += DecodeChars(Data, LabelStart, LabelLength);
		CurrentOffset = LabelStart + LabelLength;
	}

	InOutOffset = ResumeOffset >= 0 ? ResumeOffset : CurrentOffset;
	return true;
}

bool SkipName(const uint8* Data, const int32 PacketLen, int32& InOutOffset)
{
	FString Discard;
	return ReadName(Data, PacketLen, InOutOffset, Discard);
}

struct FParsedRecord
{
	FString Name;
	uint16 Type = 0;
	uint16 Class = 0;
	int32 RDataOffset = 0;
	int32 RDataLength = 0;
};

bool ReadResourceRecord(const uint8* Data, const int32 PacketLen, int32& InOutOffset, FParsedRecord& OutRecord)
{
	if (!ReadName(Data, PacketLen, InOutOffset, OutRecord.Name))
	{
		return false;
	}
	if (InOutOffset + 10 > PacketLen)
	{
		return false;
	}

	ReadU16(Data, PacketLen, InOutOffset, OutRecord.Type);
	InOutOffset += 2;
	ReadU16(Data, PacketLen, InOutOffset, OutRecord.Class);
	InOutOffset += 2;
	InOutOffset += 4;    // TTL, unused by the client

	uint16 RDLength = 0;
	ReadU16(Data, PacketLen, InOutOffset, RDLength);
	InOutOffset += 2;
	if (InOutOffset + RDLength > PacketLen)
	{
		return false;
	}
	OutRecord.RDataOffset = InOutOffset;
	OutRecord.RDataLength = RDLength;
	InOutOffset += RDLength;
	return true;
}

bool NamesMatch(const FString& A, const FString& B)
{
	return A.Equals(B, ESearchCase::IgnoreCase);
}

bool IsInClass(const FParsedRecord& Record)
{
	return (Record.Class & DnsClassMask) == DnsClassIN;
}

FString ParseTxtToken(const uint8* Data, const FParsedRecord& Record)
{
	int32 Offset = Record.RDataOffset;
	const int32 End = Record.RDataOffset + Record.RDataLength;
	while (Offset < End)
	{
		const uint8 StringLength = Data[Offset];
		const int32 StringStart = Offset + 1;
		if (StringStart + StringLength > End)
		{
			break;
		}

		const FString Entry = DecodeChars(Data, StringStart, StringLength);
		FString Key;
		FString Value;
		if (Entry.Split(TEXT("="), &Key, &Value) && Key.Equals(TEXT("token"), ESearchCase::IgnoreCase))
		{
			return Value;
		}
		Offset = StringStart + StringLength;
	}
	return FString();
}

bool ParseResponse(const uint8* Data, const int32 PacketLen, const FString& SenderIp, FDeviceExplorerDiscoveredServer& OutServer)
{
	if (PacketLen < 12)
	{
		return false;
	}

	uint16 QdCount = 0;
	uint16 AnCount = 0;
	uint16 NsCount = 0;
	uint16 ArCount = 0;
	if (!ReadU16(Data, PacketLen, 4, QdCount) || !ReadU16(Data, PacketLen, 6, AnCount) || !ReadU16(Data, PacketLen, 8, NsCount) || !ReadU16(Data, PacketLen, 10, ArCount))
	{
		return false;
	}

	int32 Offset = 12;
	for (uint16 Index = 0; Index < QdCount; ++Index)
	{
		if (!SkipName(Data, PacketLen, Offset) || Offset + 4 > PacketLen)
		{
			return false;
		}
		Offset += 4;    // QTYPE + QCLASS
	}

	const int32 RecordCount = static_cast<int32>(AnCount) + NsCount + ArCount;
	TArray<FParsedRecord> Records;
	Records.Reserve(RecordCount);
	for (int32 Index = 0; Index < RecordCount; ++Index)
	{
		FParsedRecord Record;
		if (!ReadResourceRecord(Data, PacketLen, Offset, Record))
		{
			return false;
		}
		Records.Add(MoveTemp(Record));
	}

	FString InstanceName;
	for (const FParsedRecord& Record : Records)
	{
		if (Record.Type == 12 && IsInClass(Record) && NamesMatch(Record.Name, ServiceName))
		{
			int32 RDataOffset = Record.RDataOffset;
			FString Target;
			if (ReadName(Data, PacketLen, RDataOffset, Target))
			{
				InstanceName = MoveTemp(Target);
				break;
			}
		}
	}
	if (InstanceName.IsEmpty())
	{
		return false;
	}

	FString TargetHost;
	int32 Port = 0;
	for (const FParsedRecord& Record : Records)
	{
		if (Record.Type == 33 && IsInClass(Record) && NamesMatch(Record.Name, InstanceName) && Record.RDataLength >= 6)
		{
			uint16 PortValue = 0;
			ReadU16(Data, PacketLen, Record.RDataOffset + 4, PortValue);
			int32 NameOffset = Record.RDataOffset + 6;
			FString Host;
			if (ReadName(Data, PacketLen, NameOffset, Host))
			{
				TargetHost = MoveTemp(Host);
				Port = PortValue;
			}
			break;
		}
	}
	if (Port <= 0 || Port > 65535)
	{
		return false;
	}

	FString Token;
	for (const FParsedRecord& Record : Records)
	{
		if (Record.Type == 16 && IsInClass(Record) && NamesMatch(Record.Name, InstanceName))
		{
			Token = ParseTxtToken(Data, Record);
			break;
		}
	}
	if (Token.IsEmpty())
	{
		return false;
	}

	FString HostIp;
	if (!TargetHost.IsEmpty())
	{
		for (const FParsedRecord& Record : Records)
		{
			if (Record.Type == 1 && IsInClass(Record) && Record.RDataLength == 4 && NamesMatch(Record.Name, TargetHost))
			{
				HostIp = FString::Printf(TEXT("%d.%d.%d.%d"),
				                         Data[Record.RDataOffset],
				                         Data[Record.RDataOffset + 1],
				                         Data[Record.RDataOffset + 2],
				                         Data[Record.RDataOffset + 3]);
				break;
			}
		}
	}
	if (HostIp.IsEmpty())
	{
		HostIp = SenderIp;
	}
	if (HostIp.IsEmpty())
	{
		return false;
	}

	OutServer.Host = MoveTemp(HostIp);
	OutServer.Port = Port;
	OutServer.Token = MoveTemp(Token);
	OutServer.Instance = MoveTemp(InstanceName);
	return true;
}

struct FDeviceExplorerMdnsSocketDeleter
{
	void operator()(FSocket* InSocket) const
	{
		if (InSocket != nullptr)
		{
			ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(InSocket);
		}
	}
};

class FDeviceExplorerMdnsDiscovery final : public IDeviceExplorerDiscovery, private FRunnable
{
public:
	virtual ~FDeviceExplorerMdnsDiscovery() override
	{
		bStopRequested.store(true, std::memory_order_relaxed);
		if (Thread != nullptr)
		{
			Thread->Kill(true);
			delete Thread;
			Thread = nullptr;
		}
	}

	virtual void Start(FDeviceExplorerDiscoveryCallback InCallback) override
	{
		Callback = MoveTemp(InCallback);
		// FUdpSocketBuilder resolves endpoints through FIPv4Endpoint, which the Networking
		// module only initializes in its own StartupModule.
		FModuleManager::Get().LoadModuleChecked(TEXT("Networking"));
		Thread = FRunnableThread::Create(this, TEXT("DeviceExplorerMdnsDiscovery"), 0, TPri_BelowNormal);
	}

	virtual void Stop() override { bStopRequested.store(true, std::memory_order_relaxed); }

private:
	virtual uint32 Run() override
	{
		Socket = TUniquePtr<FSocket, FDeviceExplorerMdnsSocketDeleter>(OpenSocket());
		if (!Socket)
		{
			UE_LOG(LogDeviceExplorerDiscoveryMdns, Warning, TEXT("Failed to open mDNS discovery socket"));
			return 0;
		}

		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		const TSharedRef<FInternetAddr> QueryAddr = FIPv4Endpoint(FIPv4Address(224, 0, 0, 251), MdnsPort).ToInternetAddr();
		const TArray<uint8> Query = BuildQuery();

		double NextQueryTime = 0.0;
		bool bServerKnown = false;
		while (!bStopRequested.load(std::memory_order_relaxed))
		{
			const double Now = FPlatformTime::Seconds();
			if (Now >= NextQueryTime)
			{
				int32 BytesSent = 0;
				Socket->SendTo(Query.GetData(), Query.Num(), BytesSent, *QueryAddr);
				NextQueryTime = Now + (bServerKnown ? SlowQueryIntervalSeconds : FastQueryIntervalSeconds);
			}

			if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(1.0)))
			{
				continue;
			}

			uint32 PendingSize = 0;
			while (Socket->HasPendingData(PendingSize))
			{
				TArray<uint8> Buffer;
				Buffer.SetNumUninitialized(static_cast<int32>(FMath::Clamp<uint32>(PendingSize, 512u, 9000u)));
				int32 BytesRead = 0;
				const TSharedRef<FInternetAddr> Sender = SocketSubsystem->CreateInternetAddr();
				if (!Socket->RecvFrom(Buffer.GetData(), Buffer.Num(), BytesRead, *Sender) || BytesRead <= 0)
				{
					break;
				}

				FDeviceExplorerDiscoveredServer Discovered;
				if (ParseResponse(Buffer.GetData(), BytesRead, Sender->ToString(false), Discovered))
				{
					bServerKnown = true;
					NextQueryTime = FPlatformTime::Seconds() + SlowQueryIntervalSeconds;
					const FDeviceExplorerDiscoveryCallback CallbackCopy = Callback;
					AsyncTask(ENamedThreads::GameThread,
					          [CallbackCopy, Discovered]()
					          {
								  if (CallbackCopy)
								  {
									  CallbackCopy(Discovered);
								  }
							  });
				}
			}
		}
		return 0;
	}

	// RFC 6762 5.4: a query sent from a port other than 5353 gets answered by unicast
	// back to that port, so this is a plain ephemeral-port querier, not a responder.
	// Binding 5353 here would fight the OS mDNS service (Bonjour/mDNSResponder) for it.
	static FSocket* OpenSocket()
	{
		return FUdpSocketBuilder(TEXT("DeviceExplorerMdnsDiscovery")).AsNonBlocking().WithMulticastTtl(255).Build();
	}

	FDeviceExplorerDiscoveryCallback Callback;
	FRunnableThread* Thread = nullptr;
	TUniquePtr<FSocket, FDeviceExplorerMdnsSocketDeleter> Socket;
	std::atomic<bool> bStopRequested{ false };
};
}    // namespace

TUniquePtr<IDeviceExplorerDiscovery> CreateMdnsDeviceExplorerDiscovery()
{
	return MakeUnique<FDeviceExplorerMdnsDiscovery>();
}
