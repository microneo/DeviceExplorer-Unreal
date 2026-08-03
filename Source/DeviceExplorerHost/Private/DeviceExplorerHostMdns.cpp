#include "DeviceExplorerHostMdns.h"

#include "DeviceExplorerTypes.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IPAddress.h"
#include "SocketSubsystem.h"
#include "Sockets.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerMdns, Log, All);

namespace
{
constexpr int32 MdnsPort = 5353;
constexpr int32 MaxMdnsDatagramSize = 9000;

void AddU16(TArray<uint8>& Buffer, const uint16 Value)
{
	Buffer.Add(static_cast<uint8>((Value >> 8) & 0xff));
	Buffer.Add(static_cast<uint8>(Value & 0xff));
}

void AddU32(TArray<uint8>& Buffer, const uint32 Value)
{
	Buffer.Add(static_cast<uint8>((Value >> 24) & 0xff));
	Buffer.Add(static_cast<uint8>((Value >> 16) & 0xff));
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

void AddRecord(TArray<uint8>& Packet, const FString& Name, const uint16 Type, const uint16 Class, const uint32 Ttl, const TArray<uint8>& Data)
{
	AddName(Packet, Name);
	AddU16(Packet, Type);
	AddU16(Packet, Class);
	AddU32(Packet, Ttl);
	AddU16(Packet, static_cast<uint16>(Data.Num()));
	Packet.Append(Data);
}

void AddTxtString(TArray<uint8>& Data, const FString& Text)
{
	const FTCHARToUTF8 Utf8(*Text);
	const int32 Length = FMath::Min(Utf8.Length(), 255);
	Data.Add(static_cast<uint8>(Length));
	Data.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Length);
}

bool ReadDnsName(const TArrayView<const uint8> Packet, int32& InOutOffset, FString& OutName)
{
	OutName.Reset();
	int32 Offset = InOutOffset;
	bool bJumped = false;
	int32 JumpCount = 0;
	for (;;)
	{
		if (Offset < 0 || Offset >= Packet.Num())
		{
			return false;
		}
		const uint8 LengthByte = Packet[Offset];
		if ((LengthByte & 0xC0) == 0xC0)
		{
			if (Offset + 1 >= Packet.Num())
			{
				return false;
			}
			const int32 PointerTarget = ((LengthByte & 0x3f) << 8) | Packet[Offset + 1];
			if (!bJumped)
			{
				InOutOffset = Offset + 2;
			}
			if (PointerTarget >= Offset || ++JumpCount > 32)
			{
				return false;
			}
			Offset = PointerTarget;
			bJumped = true;
			continue;
		}
		if ((LengthByte & 0xC0) != 0)
		{
			return false;
		}
		if (LengthByte == 0)
		{
			if (!bJumped)
			{
				InOutOffset = Offset + 1;
			}
			return true;
		}

		const int32 LabelStart = Offset + 1;
		const int32 LabelEnd = LabelStart + LengthByte;
		if (LabelEnd > Packet.Num())
		{
			return false;
		}
		if (!OutName.IsEmpty())
		{
			OutName.AppendChar(TEXT('.'));
		}
		for (int32 Index = LabelStart; Index < LabelEnd; ++Index)
		{
			OutName.AppendChar(static_cast<TCHAR>(Packet[Index]));
		}
		Offset = LabelEnd;
	}
}

bool PacketRequestsOurRecords(const TArrayView<const uint8> Packet, const FString& ServiceName, const FString& InstanceName, const FString& HostName, FString& OutMatchedName, uint16& OutMatchedType)
{
	if (Packet.Num() < 12)
	{
		return false;
	}
	const uint16 Flags = (static_cast<uint16>(Packet[2]) << 8) | Packet[3];
	if ((Flags & 0x8000) != 0)
	{
		return false;
	}
	const uint16 QuestionCount = (static_cast<uint16>(Packet[4]) << 8) | Packet[5];

	int32 Offset = 12;
	for (uint16 Index = 0; Index < QuestionCount; ++Index)
	{
		FString QName;
		if (!ReadDnsName(Packet, Offset, QName))
		{
			return false;
		}
		if (Offset + 4 > Packet.Num())
		{
			return false;
		}
		const uint16 QType = (static_cast<uint16>(Packet[Offset]) << 8) | Packet[Offset + 1];
		Offset += 4;    // QTYPE + QCLASS

		if (QName.Equals(ServiceName, ESearchCase::IgnoreCase) || QName.Equals(InstanceName, ESearchCase::IgnoreCase) || QName.Equals(HostName, ESearchCase::IgnoreCase))
		{
			OutMatchedName = QName;
			OutMatchedType = QType;
			return true;
		}
	}
	return false;
}

FString SafeDnsLabel(FString Value)
{
	for (TCHAR& Character : Value)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('-'))
		{
			Character = TEXT('-');
		}
	}
	Value.LeftInline(50);
	while (Value.StartsWith(TEXT("-")))
	{
		Value.RightChopInline(1, EAllowShrinking::No);
	}
	while (Value.EndsWith(TEXT("-")))
	{
		Value.LeftChopInline(1, EAllowShrinking::No);
	}
	return Value.IsEmpty() ? TEXT("deviceexplorer") : Value;
}
}    // namespace

FDeviceExplorerHostMdns::FDeviceExplorerHostMdns(const int32 InDevicePort, const int32 InDashboardPort, FString InToken)
	: DevicePort(InDevicePort)
	, DashboardPort(InDashboardPort)
	, Token(MoveTemp(InToken))
	, ServiceName(TEXT("_deviceexplorer._tcp.local"))
{
	const FString Machine = SafeDnsLabel(FPlatformProcess::ComputerName());
	HostName = FString::Printf(TEXT("%s-deviceexplorer.local"), *Machine);
	InstanceName = FString::Printf(TEXT("DeviceExplorer-%s-%u.%s"), *Machine, FPlatformProcess::GetCurrentProcessId(), *ServiceName);
}

FDeviceExplorerHostMdns::~FDeviceExplorerHostMdns()
{
	Stop();
}

bool FDeviceExplorerHostMdns::Start()
{
	if (bStarted)
	{
		return true;
	}

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("DeviceExplorerMdns"), false);
	if (Socket == nullptr)
	{
		return false;
	}

	bool bValid = false;
	TSharedRef<FInternetAddr> BindAddress = SocketSubsystem->CreateInternetAddr();
	BindAddress->SetIp(TEXT("0.0.0.0"), bValid);
	BindAddress->SetPort(MdnsPort);
	if (!bValid)
	{
		Stop();
		return false;
	}

	Socket->SetReuseAddr(true);
	Socket->SetNonBlocking(true);
	Socket->SetMulticastLoopback(true);
	Socket->SetMulticastTtl(255);
	if (!Socket->Bind(*BindAddress))
	{
		Stop();
		return false;
	}

	MulticastAddress = SocketSubsystem->CreateInternetAddr();
	MulticastAddress->SetIp(TEXT("224.0.0.251"), bValid);
	MulticastAddress->SetPort(MdnsPort);
	if (!bValid || !Socket->JoinMulticastGroup(*MulticastAddress))
	{
		Stop();
		return false;
	}

	TArray<TSharedPtr<FInternetAddr>> AdapterAddresses;
	SocketSubsystem->GetLocalAdapterAddresses(AdapterAddresses);
	HostAddresses.Reset();
	for (const TSharedPtr<FInternetAddr>& Adapter : AdapterAddresses)
	{
		if (!Adapter.IsValid())
		{
			continue;
		}
		const TArray<uint8> RawIp = Adapter->GetRawIp();
		if (RawIp.Num() != 4 || RawIp[0] == 127 || (RawIp[0] == 169 && RawIp[1] == 254))
		{
			continue;
		}
		HostAddresses.Add(RawIp);
		if (!Socket->JoinMulticastGroup(*MulticastAddress, *Adapter))
		{
			Socket->JoinMulticastGroup(*MulticastAddress);
		}
	}

	if (HostAddresses.IsEmpty())
	{
		UE_LOG(LogDeviceExplorerMdns, Warning, TEXT("No non-loopback IPv4 adapter address was found; advertising loopback only"));
		HostAddresses.Add({ 127, 0, 0, 1 });
	}

	bStarted = true;
	Announce(120);
	return true;
}

void FDeviceExplorerHostMdns::Tick()
{
	if (!bStarted)
	{
		return;
	}
	DrainQueries();
	const double Now = FPlatformTime::Seconds();
	if (Now - LastAnnouncementSeconds >= 30.0)
	{
		Announce(120);
	}
}

void FDeviceExplorerHostMdns::Stop()
{
	if (Socket == nullptr)
	{
		bStarted = false;
		return;
	}

	if (bStarted)
	{
		Announce(0);
		if (MulticastAddress)
		{
			Socket->LeaveMulticastGroup(*MulticastAddress);
		}
	}
	Socket->Close();
	ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->DestroySocket(Socket);
	Socket = nullptr;
	MulticastAddress.Reset();
	bStarted = false;
}

void FDeviceExplorerHostMdns::Announce(const uint32 Ttl)
{
	if (Socket == nullptr || !MulticastAddress)
	{
		return;
	}

	// With multiple live NICs, routing picks one interface for the multicast send; resend explicitly on each.
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	for (const TArray<uint8>& AdapterIp : HostAddresses)
	{
		TSharedRef<FInternetAddr> InterfaceAddress = SocketSubsystem->CreateInternetAddr();
		InterfaceAddress->SetRawIp(AdapterIp);
		Socket->SetMulticastInterface(*InterfaceAddress);
		SendAnnouncement(*MulticastAddress, Ttl);
	}
	LastAnnouncementSeconds = FPlatformTime::Seconds();
}

void FDeviceExplorerHostMdns::SendAnnouncement(const FInternetAddr& Destination, const uint32 Ttl)
{
	const TArray<uint8> Packet = BuildAnnouncement(Ttl);
	int32 Sent = 0;
	Socket->SendTo(Packet.GetData(), Packet.Num(), Sent, Destination);
}

TArray<uint8> FDeviceExplorerHostMdns::BuildAnnouncement(const uint32 Ttl) const
{
	TArray<uint8> Records;

	TArray<uint8> PtrData;
	AddName(PtrData, InstanceName);
	AddRecord(Records, ServiceName, 12, 1, Ttl, PtrData);

	TArray<uint8> SrvData;
	AddU16(SrvData, 0);
	AddU16(SrvData, 0);
	AddU16(SrvData, static_cast<uint16>(DevicePort));
	AddName(SrvData, HostName);
	AddRecord(Records, InstanceName, 33, 0x8001, Ttl, SrvData);

	TArray<uint8> TxtData;
	AddTxtString(TxtData, FString::Printf(TEXT("version=%d"), DeviceExplorer::ProtocolVersion));
	AddTxtString(TxtData, TEXT("token=") + Token);
	AddTxtString(TxtData, FString::Printf(TEXT("ui_port=%d"), DashboardPort));
	AddRecord(Records, InstanceName, 16, 0x8001, Ttl, TxtData);

	const TArray<TArray<uint8>>& Addresses = HostAddresses.IsEmpty() ? TArray<TArray<uint8>>{ TArray<uint8>{ 127, 0, 0, 1 } } : HostAddresses;
	for (const TArray<uint8>& Address : Addresses)
	{
		AddRecord(Records, HostName, 1, 0x8001, Ttl, Address);
	}

	TArray<uint8> Packet;
	AddU16(Packet, 0);
	AddU16(Packet, 0x8400);
	AddU16(Packet, 0);
	AddU16(Packet, static_cast<uint16>(3 + Addresses.Num()));
	AddU16(Packet, 0);
	AddU16(Packet, 0);
	Packet.Append(Records);
	return Packet;
}

void FDeviceExplorerHostMdns::DrainQueries()
{
	if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::Zero()))
	{
		return;
	}

	// Not every socket subsystem can report the size of a pending datagram.
	// The socket is non-blocking, so RecvFrom terminates the drain loop with
	// EWOULDBLOCK after the last packet.
	for (;;)
	{
		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(MaxMdnsDatagramSize);
		int32 Read = 0;
		TSharedRef<FInternetAddr> Sender = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
		if (!Socket->RecvFrom(Buffer.GetData(), Buffer.Num(), Read, *Sender, ESocketReceiveFlags::None) || Read <= 0)
		{
			break;
		}

		FString MatchedName;
		uint16 MatchedType = 0;
		if (!PacketRequestsOurRecords(TArrayView<const uint8>(Buffer.GetData(), Read), ServiceName, InstanceName, HostName, MatchedName, MatchedType))
		{
			UE_LOG(LogDeviceExplorerMdns, Log, TEXT("Ignoring mDNS query from %s (not for our records)"), *Sender->ToString(true));
			continue;
		}

		UE_LOG(LogDeviceExplorerMdns, Display, TEXT("Answering mDNS query from %s for %s (qtype=%d)"), *Sender->ToString(true), *MatchedName, MatchedType);

		// RFC 6762 6.7: a query from port 5353 (iOS's NSNetServiceBrowser goes through the
		// OS resolver, which always queries from 5353) expects a multicast reply so every
		// listener refreshes its cache; any other source port is a one-shot querier that
		// wants a direct unicast reply instead.
		if (Sender->GetPort() == MdnsPort)
		{
			Announce(120);
		}
		else
		{
			SendAnnouncement(*Sender, 120);
		}
	}
}
