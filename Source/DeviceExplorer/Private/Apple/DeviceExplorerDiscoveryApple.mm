#include "Async/Async.h"
#include "DeviceExplorerDiscovery.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

#import <Foundation/Foundation.h>
#import <Network/Network.h>
#include <netdb.h>
#include <sys/socket.h>

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerDiscoveryApple, Log, All);

namespace
{
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

@interface FDeviceExplorerNetworkBrowserDelegate : NSObject <NSNetServiceDelegate>
{
@public
	nw_browser_t Browser;
	NSMutableDictionary<NSString*, NSNetService*>* Services;
	NSMutableSet<NSString*>* PublishedServiceNames;
	FDeviceExplorerEndpointEventCallback Callback;
	NSTimeInterval RetryDelay;
	uint64 SearchGeneration;
}
- (void)start:(const FDeviceExplorerEndpointEventCallback&)InCallback;
- (void)stop;
@end

@implementation FDeviceExplorerNetworkBrowserDelegate

- (instancetype)init
{
	self = [super init];
	if (self != nil)
	{
		Browser = nullptr;
		Services = [[NSMutableDictionary alloc] init];
		PublishedServiceNames = [[NSMutableSet alloc] init];
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
	[super dealloc];
#endif
}

- (void)cancelBrowser
{
	if (Browser == nullptr)
	{
		return;
	}
	nw_browser_set_state_changed_handler(Browser, nullptr);
	nw_browser_set_browse_results_changed_handler(Browser, nullptr);
	nw_browser_cancel(Browser);
	nw_release(Browser);
	Browser = nullptr;
}

- (void)emitRemoval:(NSString*)Name
{
	if (![PublishedServiceNames containsObject:Name])
	{
		return;
	}
	[PublishedServiceNames removeObject:Name];
	FDeviceExplorerEndpointCandidate Candidate;
	Candidate.ProviderId = AppleBonjourProviderId;
	Candidate.CandidateId = UTF8_TO_TCHAR(Name.UTF8String);
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

- (void)removeService:(NSString*)Name
{
	NSNetService* Service = Services[Name];
	if (Service != nil)
	{
		Service.delegate = nil;
		[Service stop];
		[Services removeObjectForKey:Name];
	}
	[self emitRemoval:Name];
}

- (void)resolveService:(NSString*)Name type:(NSString*)Type domain:(NSString*)Domain
{
	NSNetService* Previous = Services[Name];
	if (Previous != nil)
	{
		Previous.delegate = nil;
		[Previous stop];
	}

	NSNetService* Service = [[NSNetService alloc] initWithDomain:Domain type:Type name:Name];
	Service.delegate = self;
	Services[Name] = Service;
	[Service resolveWithTimeout:5.0];
#if !__has_feature(objc_arc)
	[Service release];
#endif
}

- (void)scheduleRetry:(uint64)Generation
{
	const NSTimeInterval Delay = RetryDelay;
	RetryDelay = FMath::Min(RetryDelay * 2.0, MaxRetryDelay);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(Delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
	  if (self->SearchGeneration == Generation && self->Callback)
	  {
		  [self beginSearch];
	  }
	});
}

- (void)beginSearch
{
	[self cancelBrowser];
	if (@available(macOS 10.14, iOS 12.0, tvOS 12.0, *))
	{
		nw_browse_descriptor_t Descriptor = nw_browse_descriptor_create_bonjour_service("_deviceexplorer._tcp", "local.");
		nw_parameters_t Parameters = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
		Browser = nw_browser_create(Descriptor, Parameters);
		nw_release(Parameters);
		nw_release(Descriptor);
		if (Browser == nullptr)
		{
			UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Cannot create the Network.framework Bonjour browser"));
			[self scheduleRetry:SearchGeneration];
			return;
		}

		const uint64 Generation = SearchGeneration;
		nw_browser_set_state_changed_handler(Browser, ^(nw_browser_state_t State, nw_error_t Error) {
		  if (self->SearchGeneration != Generation)
		  {
			  return;
		  }
		  if (State == nw_browser_state_ready)
		  {
			  self->RetryDelay = MinRetryDelay;
			  UE_LOG(LogDeviceExplorerDiscoveryApple, Display, TEXT("Network.framework Bonjour browse is ready"));
		  }
		  else if (State == nw_browser_state_waiting)
		  {
			  UE_LOG(LogDeviceExplorerDiscoveryApple, Display,
			         TEXT("Bonjour browse is waiting for local-network access or a usable interface (%d)"),
			         Error == nullptr ? 0 : nw_error_get_error_code(Error));
		  }
		  else if (State == nw_browser_state_failed)
		  {
			  UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Bonjour browse failed (%d); retrying in %.0fs"),
			         Error == nullptr ? 0 : nw_error_get_error_code(Error), self->RetryDelay);
			  [self scheduleRetry:Generation];
		  }
		});

		nw_browser_set_browse_results_changed_handler(
			Browser,
			^(nw_browse_result_t OldResult, nw_browse_result_t NewResult, bool BatchComplete) {
			  (void) BatchComplete;
			  if (self->SearchGeneration != Generation)
			  {
				  return;
			  }
			  nw_browse_result_t Result = NewResult != nullptr ? NewResult : OldResult;
			  if (Result == nullptr)
			  {
				  return;
			  }
			  nw_endpoint_t Endpoint = nw_browse_result_copy_endpoint(Result);
			  if (Endpoint == nullptr || nw_endpoint_get_type(Endpoint) != nw_endpoint_type_bonjour_service)
			  {
				  if (Endpoint != nullptr) nw_release(Endpoint);
				  return;
			  }
			  const char* RawName = nw_endpoint_get_bonjour_service_name(Endpoint);
			  const char* RawType = nw_endpoint_get_bonjour_service_type(Endpoint);
			  const char* RawDomain = nw_endpoint_get_bonjour_service_domain(Endpoint);
			  NSString* Name = RawName == nullptr ? nil : [NSString stringWithUTF8String:RawName];
			  NSString* Type = RawType == nullptr ? @"_deviceexplorer._tcp." : [NSString stringWithUTF8String:RawType];
			  NSString* Domain = RawDomain == nullptr ? @"local." : [NSString stringWithUTF8String:RawDomain];
			  nw_release(Endpoint);
			  if (Name == nil)
			  {
				  return;
			  }
			  if (NewResult == nullptr)
			  {
				  [self removeService:Name];
			  }
			  else
			  {
				  [self resolveService:Name type:Type domain:Domain];
			  }
			});
		nw_browser_set_queue(Browser, dispatch_get_main_queue());
		nw_browser_start(Browser);
		return;
	}

	UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Network.framework Bonjour browsing is unavailable on this OS version"));
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
	[self cancelBrowser];
	for (NSNetService* Service in Services.allValues)
	{
		Service.delegate = nil;
		[Service stop];
	}
	[Services removeAllObjects];
	[PublishedServiceNames removeAllObjects];
	Callback = nullptr;
}

- (void)netService:(NSNetService*)Sender didNotResolve:(NSDictionary*)ErrorDict
{
	UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Bonjour resolve failed for %s: %s"),
	       *FString(UTF8_TO_TCHAR(Sender.name.UTF8String)),
	       *FString(UTF8_TO_TCHAR(ErrorDict.description.UTF8String)));
}

- (void)netServiceDidResolveAddress:(NSNetService*)Sender
{
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
		UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Resolved %s but found no usable IPv4 address/port"),
		       *FString(UTF8_TO_TCHAR(Sender.name.UTF8String)));
		return;
	}

	FString Fingerprint;
	if (Sender.TXTRecordData != nil)
	{
		NSDictionary<NSString*, NSData*>* Values = [NSNetService dictionaryFromTXTRecordData:Sender.TXTRecordData];
		Fingerprint = DataToString(Values[@"fp"]);
	}

	const bool bWasPublished = [PublishedServiceNames containsObject:Sender.name];
	[PublishedServiceNames addObject:Sender.name];
	const FString Instance(UTF8_TO_TCHAR(Sender.name.UTF8String));
	const int32 Port = static_cast<int32>(Sender.port);
	const FDeviceExplorerEndpointEventCallback CallbackCopy = Callback;
	AsyncTask(ENamedThreads::GameThread,
	          [CallbackCopy, Event = bWasPublished ? EDeviceExplorerEndpointEvent::Updated : EDeviceExplorerEndpointEvent::Added,
	           Host = MoveTemp(Host), Fingerprint = MoveTemp(Fingerprint), Instance, Port]() mutable
	          {
			  if (CallbackCopy)
			  {
				  FDeviceExplorerEndpointCandidate Candidate;
				  Candidate.ProviderId = AppleBonjourProviderId;
				  Candidate.CandidateId = Instance;
				  Candidate.Endpoint.Serialized.Address = MoveTemp(Host);
				  Candidate.Endpoint.Serialized.Port = Port;
				  Candidate.Endpoint.Serialized.Family = EDeviceExplorerAddressFamily::IPv4;
				  Candidate.HostFingerprint = MoveTemp(Fingerprint);
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
			  Delegate = [[FDeviceExplorerNetworkBrowserDelegate alloc] init];
		  }
		  [Delegate start:CallbackCopy];
		});
	}

	virtual void Stop() override
	{
		FDeviceExplorerNetworkBrowserDelegate* DelegateToStop = Delegate;
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
	FDeviceExplorerNetworkBrowserDelegate* Delegate = nil;
};

TUniquePtr<IDeviceExplorerEndpointSource> CreateAppleDeviceExplorerEndpointSource()
{
	return MakeUnique<FAppleDeviceExplorerEndpointSource>();
}

#pragma clang diagnostic pop
