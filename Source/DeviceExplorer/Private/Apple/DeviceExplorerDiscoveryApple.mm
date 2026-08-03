#include "Async/Async.h"
#include "DeviceExplorerDiscovery.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#import <Foundation/Foundation.h>
#include <netdb.h>
#include <sys/socket.h>

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerDiscoveryApple, Log, All);

namespace
{
// A browse is refused until the user grants local-network access, so the first failure is not
// terminal. Back off and keep trying rather than leaving discovery dead for the whole session.
constexpr NSTimeInterval MinRetryDelay = 2.0;
constexpr NSTimeInterval MaxRetryDelay = 30.0;
const FName AppleBonjourProviderId(TEXT("AppleBonjour"));

FString DataToString(NSData* Data)
{
	if (Data == nil)
	{
		return FString();
	}
	NSString* String = [[NSString alloc] initWithData:Data encoding:NSUTF8StringEncoding];
	FString Result = String == nil ? FString() : FString(UTF8_TO_TCHAR(String.UTF8String));
#if !__has_feature(objc_arc)
	[String release];
#endif
	return Result;
}
}    // namespace

@interface FDeviceExplorerNetServiceDelegate : NSObject <NSNetServiceBrowserDelegate, NSNetServiceDelegate>
{
@public
	NSNetServiceBrowser* Browser;
	NSMutableSet<NSNetService*>* Services;
	NSMutableSet<NSString*>* PublishedServiceNames;
	FDeviceExplorerEndpointEventCallback Callback;
	NSTimeInterval RetryDelay;
	uint64 SearchGeneration;
}
- (void)start:(const FDeviceExplorerEndpointEventCallback&)InCallback;
- (void)stop;
@end

@implementation FDeviceExplorerNetServiceDelegate

- (instancetype)init
{
	self = [super init];
	if (self != nil)
	{
		Browser = [[NSNetServiceBrowser alloc] init];
		Services = [[NSMutableSet alloc] init];
		PublishedServiceNames = [[NSMutableSet alloc] init];
		Browser.delegate = self;
		RetryDelay = MinRetryDelay;
		SearchGeneration = 0;
	}
	return self;
}

- (void)dealloc
{
	[self stop];
#if !__has_feature(objc_arc)
	[Services release];
	[PublishedServiceNames release];
	[Browser release];
	[super dealloc];
#endif
}

- (void)beginSearch
{
	UE_LOG(LogDeviceExplorerDiscoveryApple, Display, TEXT("Starting Bonjour browse for _deviceexplorer._tcp.local."));
	[Browser searchForServicesOfType:@"_deviceexplorer._tcp." inDomain:@"local."];
}

- (void)start:(const FDeviceExplorerEndpointEventCallback&)InCallback
{
	Callback = InCallback;
	RetryDelay = MinRetryDelay;
	++SearchGeneration;
	[self beginSearch];
}

- (void)stop
{
	++SearchGeneration;
	[Browser stop];
	for (NSNetService* Service in Services)
	{
		Service.delegate = nil;
		[Service stop];
	}
	[Services removeAllObjects];
	[PublishedServiceNames removeAllObjects];
	Callback = nullptr;
}

- (void)netServiceBrowser:(NSNetServiceBrowser*)NetServiceBrowser didNotSearch:(NSDictionary*)ErrorDict
{
	(void) NetServiceBrowser;
	UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Bonjour browse failed to start, retrying in %.0fs: %s"), RetryDelay,
	       *FString(UTF8_TO_TCHAR(ErrorDict.description.UTF8String)));

	const uint64 Generation = SearchGeneration;
	const NSTimeInterval Delay = RetryDelay;
	RetryDelay = FMath::Min(RetryDelay * 2.0, MaxRetryDelay);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(Delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
	  if (self->SearchGeneration == Generation && self->Callback)
	  {
		  [self beginSearch];
	  }
	});
}

- (void)netServiceBrowser:(NSNetServiceBrowser*)NetServiceBrowser didFindService:(NSNetService*)NetService moreComing:(BOOL)MoreComing
{
	(void) NetServiceBrowser;
	(void) MoreComing;
	UE_LOG(LogDeviceExplorerDiscoveryApple, Display, TEXT("Bonjour browse found service: %s"), *FString(UTF8_TO_TCHAR(NetService.name.UTF8String)));
	RetryDelay = MinRetryDelay;
	NetService.delegate = self;
	[Services addObject:NetService];
	[NetService resolveWithTimeout:5.0];
}

- (void)netServiceBrowser:(NSNetServiceBrowser*)NetServiceBrowser didRemoveService:(NSNetService*)NetService moreComing:(BOOL)MoreComing
{
	(void) NetServiceBrowser;
	(void) MoreComing;
	NetService.delegate = nil;
	[Services removeObject:NetService];
	if ([PublishedServiceNames containsObject:NetService.name])
	{
		[PublishedServiceNames removeObject:NetService.name];
		FDeviceExplorerEndpointCandidate Candidate;
		Candidate.ProviderId = AppleBonjourProviderId;
		Candidate.CandidateId = UTF8_TO_TCHAR(NetService.name.UTF8String);
		const FDeviceExplorerEndpointEventCallback CallbackCopy = Callback;
		AsyncTask(ENamedThreads::GameThread,
		          [CallbackCopy, Candidate = MoveTemp(Candidate)]() mutable
		          {
					  if (CallbackCopy)
					  {
						  CallbackCopy(EDeviceExplorerEndpointEvent::Removed, MoveTemp(Candidate));
					  }
				  });
	}
}

- (void)netService:(NSNetService*)Sender didNotResolve:(NSDictionary*)ErrorDict
{
	UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Bonjour resolve failed for %s: %s"), *FString(UTF8_TO_TCHAR(Sender.name.UTF8String)),
	       *FString(UTF8_TO_TCHAR(ErrorDict.description.UTF8String)));
}

- (void)netServiceDidResolveAddress:(NSNetService*)Sender
{
	UE_LOG(LogDeviceExplorerDiscoveryApple, Display, TEXT("Bonjour resolve succeeded for %s (%d address(es))"), *FString(UTF8_TO_TCHAR(Sender.name.UTF8String)),
	       static_cast<int32>(Sender.addresses.count));
	FString Host;
	for (NSData* AddressData in Sender.addresses)
	{
		const sockaddr* Address = static_cast<const sockaddr*>(AddressData.bytes);
		if (Address == nullptr || Address->sa_family != AF_INET)
		{
			continue;
		}

		char HostBuffer[NI_MAXHOST] = {};
		if (getnameinfo(Address, static_cast<socklen_t>(AddressData.length), HostBuffer, sizeof(HostBuffer), nullptr, 0, NI_NUMERICHOST) == 0)
		{
			Host = UTF8_TO_TCHAR(HostBuffer);
			break;
		}
	}
	if (Host.IsEmpty() || Sender.port <= 0)
	{
		UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Resolved %s but found no usable IPv4 address/port"), *FString(UTF8_TO_TCHAR(Sender.name.UTF8String)));
		return;
	}

	FString Token;
	NSData* TXTRecord = Sender.TXTRecordData;
	if (TXTRecord != nil)
	{
		NSDictionary<NSString*, NSData*>* Values = [NSNetService dictionaryFromTXTRecordData:TXTRecord];
		Token = DataToString(Values[@"token"]);
	}

	const bool bWasPublished = [PublishedServiceNames containsObject:Sender.name];
	[PublishedServiceNames addObject:Sender.name];
	const FString Instance(UTF8_TO_TCHAR(Sender.name.UTF8String));
	const int32 Port = static_cast<int32>(Sender.port);
	const FDeviceExplorerEndpointEventCallback CallbackCopy = Callback;
	AsyncTask(ENamedThreads::GameThread,
	          [CallbackCopy, Event = bWasPublished ? EDeviceExplorerEndpointEvent::Updated : EDeviceExplorerEndpointEvent::Added,
	           Host = MoveTemp(Host), Token = MoveTemp(Token), Instance, Port]() mutable
	          {
				  if (CallbackCopy)
				  {
					  FDeviceExplorerEndpointCandidate Candidate;
					  Candidate.ProviderId = AppleBonjourProviderId;
					  Candidate.CandidateId = Instance;
					  Candidate.Endpoint.Serialized.Address = MoveTemp(Host);
					  Candidate.Endpoint.Serialized.Port = Port;
					  Candidate.Endpoint.Serialized.Family = EDeviceExplorerAddressFamily::IPv4;
					  Candidate.Token = MoveTemp(Token);
					  Candidate.Instance = Instance;
					  CallbackCopy(Event, MoveTemp(Candidate));
				  }
			  });
}

@end

class FAppleDeviceExplorerEndpointSource final : public IDeviceExplorerEndpointSource
{
public:
	virtual ~FAppleDeviceExplorerEndpointSource() override { Stop(); }

	virtual FName GetProviderId() const override { return AppleBonjourProviderId; }

	virtual void Start(FDeviceExplorerEndpointEventCallback Callback) override
	{
		const FDeviceExplorerEndpointEventCallback CallbackCopy = MoveTemp(Callback);
		dispatch_async(dispatch_get_main_queue(), ^{
		  if (Delegate == nil)
		  {
			  Delegate = [[FDeviceExplorerNetServiceDelegate alloc] init];
		  }
		  [Delegate start:CallbackCopy];
		});
	}

	virtual void Stop() override
	{
		FDeviceExplorerNetServiceDelegate* DelegateToStop = Delegate;
		Delegate = nil;
		if (DelegateToStop == nil)
		{
			return;
		}
		dispatch_async(dispatch_get_main_queue(), ^{
		  [DelegateToStop stop];
#if !__has_feature(objc_arc)
		  [DelegateToStop release];
#endif
		});
	}

private:
	FDeviceExplorerNetServiceDelegate* Delegate = nil;
};

TUniquePtr<IDeviceExplorerEndpointSource> CreateAppleDeviceExplorerEndpointSource()
{
	return MakeUnique<FAppleDeviceExplorerEndpointSource>();
}

#pragma clang diagnostic pop
