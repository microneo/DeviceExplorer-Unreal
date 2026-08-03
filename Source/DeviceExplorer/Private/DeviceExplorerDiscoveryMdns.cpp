#include "Async/Async.h"
#include "Common/UdpSocketBuilder.h"
#include "Containers/StringConv.h"
#include "DeviceExplorerDiscovery.h"
#include "DeviceExplorerMdns.h"
#include "HAL/PlatformTime.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
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
constexpr int32 MaxMdnsDatagramSize = 9000;
const FName MdnsProviderId(TEXT("Mdns"));

FString FromUtf8(const std::string& Value)
{
	const FUTF8ToTCHAR Converted(Value.data(), static_cast<int32>(Value.size()));
	return FString(Converted.Length(), Converted.Get());
}

TArray<uint8> BuildQuery()
{
	std::vector<std::uint8_t> Encoded;
	if (!DeviceExplorer::Wire::EncodeMdnsQuery(DeviceExplorer::Wire::DeviceExplorerMdnsServiceName, Encoded))
	{
		return {};
	}
	TArray<uint8> Packet;
	Packet.Append(Encoded.data(), static_cast<int32>(Encoded.size()));
	return Packet;
}

bool ParseResponse(const uint8* Data,
	               const int32 PacketLen,
	               const FString& SenderIp,
	               FDeviceExplorerEndpointCandidate& OutCandidate,
	               uint32& OutTimeToLive)
{
	const DeviceExplorer::Wire::MdnsAnnouncementParseResult Result = DeviceExplorer::Wire::ParseMdnsAnnouncement(
		{ Data, static_cast<std::size_t>(PacketLen) });
	if (Result.Status != DeviceExplorer::Wire::MdnsStatus::Complete)
	{
		return false;
	}

	FString HostIp;
	if (!Result.Announcement.IPv4Addresses.empty())
	{
		const std::array<std::uint8_t, 4>& Address = Result.Announcement.IPv4Addresses.front();
		HostIp = FString::Printf(TEXT("%d.%d.%d.%d"), Address[0], Address[1], Address[2], Address[3]);
	}
	if (HostIp.IsEmpty())
	{
		HostIp = SenderIp;
	}
	if (HostIp.IsEmpty())
	{
		return false;
	}

	OutCandidate.ProviderId = MdnsProviderId;
	OutCandidate.CandidateId = FromUtf8(Result.Announcement.InstanceName);
	OutCandidate.Endpoint.Serialized.Address = MoveTemp(HostIp);
	OutCandidate.Endpoint.Serialized.Port = Result.Announcement.DevicePort;
	OutCandidate.Endpoint.Serialized.Family = EDeviceExplorerAddressFamily::IPv4;
	OutCandidate.Token = FromUtf8(Result.Announcement.Token);
	OutCandidate.Instance = OutCandidate.CandidateId;
	OutTimeToLive = Result.Announcement.TimeToLive;
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

class FDeviceExplorerMdnsEndpointSource final : public IDeviceExplorerEndpointSource, private FRunnable
{
public:
	virtual ~FDeviceExplorerMdnsEndpointSource() override
	{
		bStopRequested.store(true, std::memory_order_relaxed);
		if (Thread != nullptr)
		{
			Thread->Kill(true);
			delete Thread;
			Thread = nullptr;
		}
	}

	virtual FName GetProviderId() const override { return MdnsProviderId; }

	virtual void Start(FDeviceExplorerEndpointEventCallback InCallback) override
	{
		Callback = MoveTemp(InCallback);
		// FUdpSocketBuilder resolves endpoints through FIPv4Endpoint, which the Networking
		// module only initializes in its own StartupModule.
		FModuleManager::Get().LoadModuleChecked(TEXT("Networking"));
		Thread = FRunnableThread::Create(this, TEXT("DeviceExplorerMdnsDiscovery"), 0, TPri_BelowNormal);
	}

	virtual void Stop() override { bStopRequested.store(true, std::memory_order_relaxed); }

private:
	struct FKnownCandidate
	{
		FDeviceExplorerEndpointCandidate Candidate;
		double ExpiresAtSeconds = 0.0;
	};

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
		if (Query.IsEmpty())
		{
			UE_LOG(LogDeviceExplorerDiscoveryMdns, Warning, TEXT("Failed to encode mDNS discovery query"));
			return 0;
		}

		double NextQueryTime = 0.0;
		while (!bStopRequested.load(std::memory_order_relaxed))
		{
			const double Now = FPlatformTime::Seconds();
			ExpireCandidates(Now);
			if (Now >= NextQueryTime)
			{
				int32 BytesSent = 0;
				Socket->SendTo(Query.GetData(), Query.Num(), BytesSent, *QueryAddr);
				NextQueryTime = Now + (KnownCandidates.IsEmpty() ? FastQueryIntervalSeconds : SlowQueryIntervalSeconds);
			}

			if (!Socket->Wait(ESocketWaitConditions::WaitForRead, FTimespan::FromSeconds(1.0)))
			{
				continue;
			}

			// Not every socket subsystem can report the size of a pending datagram.
			// Drain a non-blocking socket into a buffer large enough for the mDNS
			// packets accepted by this protocol until the backend reports EWOULDBLOCK.
			for (;;)
			{
				TArray<uint8> Buffer;
				Buffer.SetNumUninitialized(MaxMdnsDatagramSize);
				int32 BytesRead = 0;
				const TSharedRef<FInternetAddr> Sender = SocketSubsystem->CreateInternetAddr();
				if (!Socket->RecvFrom(Buffer.GetData(), Buffer.Num(), BytesRead, *Sender) || BytesRead <= 0)
				{
					break;
				}

				FDeviceExplorerEndpointCandidate Candidate;
				uint32 TimeToLive = 0;
				if (!ParseResponse(Buffer.GetData(), BytesRead, Sender->ToString(false), Candidate, TimeToLive))
				{
					continue;
				}

				NextQueryTime = FPlatformTime::Seconds() + SlowQueryIntervalSeconds;
				if (TimeToLive == 0)
				{
					if (FKnownCandidate* Known = KnownCandidates.Find(Candidate.CandidateId))
					{
						FDeviceExplorerEndpointCandidate Removed = MoveTemp(Known->Candidate);
						KnownCandidates.Remove(Candidate.CandidateId);
						Emit(EDeviceExplorerEndpointEvent::Removed, MoveTemp(Removed));
					}
					continue;
				}

				const EDeviceExplorerEndpointEvent Event = KnownCandidates.Contains(Candidate.CandidateId)
				                                                  ? EDeviceExplorerEndpointEvent::Updated
				                                                  : EDeviceExplorerEndpointEvent::Added;
				FKnownCandidate Known;
				Known.Candidate = Candidate;
				Known.ExpiresAtSeconds = FPlatformTime::Seconds() + TimeToLive;
				KnownCandidates.Add(Candidate.CandidateId, MoveTemp(Known));
				Emit(Event, MoveTemp(Candidate));
			}
		}
		return 0;
	}

	void ExpireCandidates(const double Now)
	{
		for (auto It = KnownCandidates.CreateIterator(); It; ++It)
		{
			if (It.Value().ExpiresAtSeconds <= Now)
			{
				FDeviceExplorerEndpointCandidate Removed = MoveTemp(It.Value().Candidate);
				It.RemoveCurrent();
				Emit(EDeviceExplorerEndpointEvent::Removed, MoveTemp(Removed));
			}
		}
	}

	void Emit(const EDeviceExplorerEndpointEvent Event, FDeviceExplorerEndpointCandidate Candidate) const
	{
		const FDeviceExplorerEndpointEventCallback CallbackCopy = Callback;
		AsyncTask(ENamedThreads::GameThread,
		          [CallbackCopy, Event, Candidate = MoveTemp(Candidate)]() mutable
		          {
				  if (CallbackCopy)
				  {
					  CallbackCopy(Event, MoveTemp(Candidate));
				  }
			  });
	}

	// RFC 6762 5.4: a query sent from a port other than 5353 gets answered by unicast
	// back to that port, so this is a plain ephemeral-port querier, not a responder.
	// Binding 5353 here would fight the OS mDNS service (Bonjour/mDNSResponder) for it.
	static FSocket* OpenSocket()
	{
		return FUdpSocketBuilder(TEXT("DeviceExplorerMdnsDiscovery")).AsNonBlocking().WithMulticastTtl(255).Build();
	}

	FDeviceExplorerEndpointEventCallback Callback;
	TMap<FString, FKnownCandidate> KnownCandidates;
	FRunnableThread* Thread = nullptr;
	TUniquePtr<FSocket, FDeviceExplorerMdnsSocketDeleter> Socket;
	std::atomic<bool> bStopRequested{ false };
};
}    // namespace

TUniquePtr<IDeviceExplorerEndpointSource> CreateMdnsDeviceExplorerEndpointSource()
{
	return MakeUnique<FDeviceExplorerMdnsEndpointSource>();
}
