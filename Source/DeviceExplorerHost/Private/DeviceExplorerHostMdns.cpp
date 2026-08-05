#include "DeviceExplorerHostMdns.h"

#include "Containers/StringConv.h"
#include "DeviceExplorerAuth.h"
#include "DeviceExplorerMdns.h"
#include "DeviceExplorerProtocol.h"
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

std::string ToUtf8(const FString& Value)
{
	const FTCHARToUTF8 Utf8(*Value);
	return std::string(Utf8.Get(), static_cast<std::size_t>(Utf8.Length()));
}

FString FromUtf8(const std::string& Value)
{
	const FUTF8ToTCHAR Converted(Value.data(), static_cast<int32>(Value.size()));
	return FString(Converted.Length(), Converted.Get());
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

FDeviceExplorerHostMdns::FDeviceExplorerHostMdns(const int32 InDevicePort, const int32 InDashboardPort, const FString& InToken)
	: DevicePort(InDevicePort)
	, DashboardPort(InDashboardPort)
	, TokenFingerprint(DeviceExplorer::Auth::ComputeTokenFingerprint(InToken))
	, ServiceName(UTF8_TO_TCHAR(DeviceExplorer::Wire::DeviceExplorerMdnsServiceName))
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
	if (Packet.IsEmpty())
	{
		return;
	}
	int32 Sent = 0;
	Socket->SendTo(Packet.GetData(), Packet.Num(), Sent, Destination);
}

TArray<uint8> FDeviceExplorerHostMdns::BuildAnnouncement(const uint32 Ttl) const
{
	if (DevicePort <= 0 || DevicePort > MAX_uint16 || DashboardPort < 0 || DashboardPort > MAX_uint16)
	{
		UE_LOG(LogDeviceExplorerMdns, Error, TEXT("Cannot encode mDNS announcement with invalid ports %d/%d"), DevicePort, DashboardPort);
		return {};
	}

	DeviceExplorer::Wire::MdnsServiceAnnouncement Announcement;
	Announcement.ServiceName = ToUtf8(ServiceName);
	Announcement.InstanceName = ToUtf8(InstanceName);
	Announcement.HostName = ToUtf8(HostName);
	Announcement.TokenFingerprint = ToUtf8(TokenFingerprint);
	Announcement.DevicePort = static_cast<std::uint16_t>(DevicePort);
	Announcement.DashboardPort = static_cast<std::uint16_t>(DashboardPort);
	Announcement.ProtocolVersion = DeviceExplorer::ProtocolVersion;
	Announcement.TimeToLive = Ttl;
	for (const TArray<uint8>& Address : HostAddresses)
	{
		if (Address.Num() == 4)
		{
			Announcement.IPv4Addresses.push_back({ Address[0], Address[1], Address[2], Address[3] });
		}
	}

	std::vector<std::uint8_t> Encoded;
	DeviceExplorer::Wire::MdnsError Error = DeviceExplorer::Wire::MdnsError::None;
	if (!DeviceExplorer::Wire::EncodeMdnsAnnouncement(Announcement, Encoded, &Error))
	{
		UE_LOG(LogDeviceExplorerMdns, Error, TEXT("Failed to encode mDNS announcement: %s"),
		       UTF8_TO_TCHAR(DeviceExplorer::Wire::MdnsErrorText(Error)));
		return {};
	}

	TArray<uint8> Packet;
	Packet.Append(Encoded.data(), static_cast<int32>(Encoded.size()));
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

		const DeviceExplorer::Wire::MdnsQueryParseResult Result = DeviceExplorer::Wire::ParseMdnsQuery(
			{ Buffer.GetData(), static_cast<std::size_t>(Read) },
			ToUtf8(ServiceName),
			ToUtf8(InstanceName),
			ToUtf8(HostName));
		if (Result.Status != DeviceExplorer::Wire::MdnsStatus::Complete)
		{
			UE_LOG(LogDeviceExplorerMdns, Log, TEXT("Ignoring mDNS query from %s (not for our records)"), *Sender->ToString(true));
			continue;
		}

		UE_LOG(LogDeviceExplorerMdns, Display, TEXT("Answering mDNS query from %s for %s (qtype=%d)"),
		       *Sender->ToString(true), *FromUtf8(Result.Match.Name), Result.Match.Type);

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
