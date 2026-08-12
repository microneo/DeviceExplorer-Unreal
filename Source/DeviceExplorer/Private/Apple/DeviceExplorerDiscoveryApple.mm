#include "Async/Async.h"
#include "DeviceExplorerDiscovery.h"

#import <Foundation/Foundation.h>
#import <Network/Network.h>

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerDiscoveryApple, Log, All);

namespace
{
constexpr NSTimeInterval MinRetryDelay = 2.0;
constexpr NSTimeInterval MaxRetryDelay = 30.0;
constexpr NSTimeInterval ResolveTimeout = 5.0;
const FName AppleBonjourProviderId(TEXT("AppleBonjour"));

NSString* CopyFingerprint(nw_browse_result_t Result)
{
	if (@available(macOS 10.15, iOS 13.0, tvOS 13.0, *))
	{
		nw_txt_record_t Record = nw_browse_result_copy_txt_record_object(Result);
		if (Record == nullptr) return nil;
		__block NSString* Fingerprint = nil;
		nw_txt_record_apply(Record, ^bool(const char* Key, nw_txt_record_find_key_t Found, const uint8_t* Value, size_t Length) {
		  if (Key == nullptr || FCStringAnsi::Stricmp(Key, "fp") != 0 ||
		      Found != nw_txt_record_find_key_non_empty_value || Value == nullptr)
		  {
			  return true;
		  }
		  Fingerprint = [[NSString alloc] initWithBytes:Value length:Length encoding:NSUTF8StringEncoding];
		  return false;
		});
		nw_release(Record);
		return Fingerprint;
	}
	return nil;
}
}    // namespace

@interface FDeviceExplorerNetworkBrowserDelegate : NSObject
{
@public
	nw_browser_t Browser;
	NSMutableDictionary<NSString*, NSValue*>* Connections;
	NSMutableDictionary<NSString*, NSString*>* Fingerprints;
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
		Connections = [[NSMutableDictionary alloc] init];
		Fingerprints = [[NSMutableDictionary alloc] init];
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
	[Connections release];
	[Fingerprints release];
	[PublishedServiceNames release];
	[super dealloc];
#endif
}

- (void)cancelBrowser
{
	if (Browser == nullptr) return;
	nw_browser_set_state_changed_handler(Browser, nullptr);
	nw_browser_set_browse_results_changed_handler(Browser, nullptr);
	nw_browser_cancel(Browser);
	nw_release(Browser);
	Browser = nullptr;
}

- (void)cancelConnection:(NSString*)Name
{
	NSValue* Wrapped = Connections[Name];
	if (Wrapped == nil) return;
	nw_connection_t Connection = static_cast<nw_connection_t>(Wrapped.pointerValue);
	nw_connection_set_state_changed_handler(Connection, nullptr);
	nw_connection_cancel(Connection);
	nw_release(Connection);
	[Connections removeObjectForKey:Name];
}

- (void)emitRemoval:(NSString*)Name
{
	if (![PublishedServiceNames containsObject:Name]) return;
	[PublishedServiceNames removeObject:Name];
	FDeviceExplorerEndpointCandidate Candidate;
	Candidate.ProviderId = AppleBonjourProviderId;
	Candidate.CandidateId = UTF8_TO_TCHAR(Name.UTF8String);
	const FDeviceExplorerEndpointEventCallback CallbackCopy = Callback;
	AsyncTask(ENamedThreads::GameThread,
	          [CallbackCopy, Candidate = MoveTemp(Candidate)]() mutable
	          {
		          if (CallbackCopy) CallbackCopy(EDeviceExplorerEndpointEvent::Removed, MoveTemp(Candidate));
	});
}

- (void)removeResult:(NSString*)Name
{
	[self cancelConnection:Name];
	[Fingerprints removeObjectForKey:Name];
	[self emitRemoval:Name];
}

- (bool)publishResult:(NSString*)Name connection:(nw_connection_t)Connection logFailure:(bool)bLogFailure
{
	nw_path_t Path = nw_connection_copy_current_path(Connection);
	nw_endpoint_t Endpoint = Path == nullptr ? nullptr : nw_path_copy_effective_remote_endpoint(Path);
	if (Path != nullptr) nw_release(Path);
	if (Endpoint == nullptr || nw_endpoint_get_type(Endpoint) != nw_endpoint_type_host)
	{
		if (Endpoint != nullptr) nw_release(Endpoint);
		if (bLogFailure)
		{
			UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Resolved %s but found no usable host endpoint"),
			       *FString(UTF8_TO_TCHAR(Name.UTF8String)));
		}
		return false;
	}
	const char* RawHost = nw_endpoint_get_hostname(Endpoint);
	const uint16 Port = nw_endpoint_get_port(Endpoint);
	if (RawHost == nullptr || *RawHost == '\0' || Port == 0)
	{
		nw_release(Endpoint);
		if (bLogFailure)
		{
			UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Resolved %s but found no usable address/port"),
			       *FString(UTF8_TO_TCHAR(Name.UTF8String)));
		}
		return false;
	}
	const FString Host(UTF8_TO_TCHAR(RawHost));
	const EDeviceExplorerAddressFamily Family = FCStringAnsi::Strchr(RawHost, ':') == nullptr
		? EDeviceExplorerAddressFamily::IPv4
		: EDeviceExplorerAddressFamily::IPv6;
	nw_release(Endpoint);

	const bool bWasPublished = [PublishedServiceNames containsObject:Name];
	[PublishedServiceNames addObject:Name];
	NSString* FingerprintValue = Fingerprints[Name];
	const FString Fingerprint = FingerprintValue == nil ? FString() : FString(UTF8_TO_TCHAR(FingerprintValue.UTF8String));
	const FString Instance(UTF8_TO_TCHAR(Name.UTF8String));
	const FDeviceExplorerEndpointEventCallback CallbackCopy = Callback;
	AsyncTask(ENamedThreads::GameThread,
	          [CallbackCopy, Event = bWasPublished ? EDeviceExplorerEndpointEvent::Updated : EDeviceExplorerEndpointEvent::Added,
	           Host, Fingerprint, Instance, Port, Family]() mutable
	          {
		          if (!CallbackCopy) return;
		          FDeviceExplorerEndpointCandidate Candidate;
		          Candidate.ProviderId = AppleBonjourProviderId;
		          Candidate.CandidateId = Instance;
		          Candidate.Endpoint.Serialized.Address = MoveTemp(Host);
		          Candidate.Endpoint.Serialized.Port = Port;
		          Candidate.Endpoint.Serialized.Family = Family;
		          Candidate.HostFingerprint = MoveTemp(Fingerprint);
		          Candidate.Instance = Instance;
		          CallbackCopy(Event, MoveTemp(Candidate));
	          });
	return true;
}

- (void)resolveResult:(nw_browse_result_t)Result endpoint:(nw_endpoint_t)Endpoint name:(NSString*)Name
{
	[self cancelConnection:Name];
	NSString* Fingerprint = CopyFingerprint(Result);
	if (Fingerprint != nil)
	{
		Fingerprints[Name] = Fingerprint;
#if !__has_feature(objc_arc)
		[Fingerprint release];
#endif
	}
	else
	{
		[Fingerprints removeObjectForKey:Name];
	}

	nw_parameters_t Parameters = nw_parameters_create_secure_tcp(NW_PARAMETERS_DISABLE_PROTOCOL, NW_PARAMETERS_DEFAULT_CONFIGURATION);
	nw_connection_t Connection = nw_connection_create(Endpoint, Parameters);
	nw_release(Parameters);
	if (Connection == nullptr)
	{
		UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Cannot resolve Bonjour service %s through Network.framework"),
		       *FString(UTF8_TO_TCHAR(Name.UTF8String)));
		return;
	}
	Connections[Name] = [NSValue valueWithPointer:Connection];
	const uint64 Generation = SearchGeneration;
	const std::string NameUtf8(Name.UTF8String == nullptr ? "" : Name.UTF8String);
	nw_connection_set_state_changed_handler(Connection, ^(nw_connection_state_t State, nw_error_t Error) {
	  if (self->SearchGeneration != Generation) return;
	  NSString* CurrentName = [NSString stringWithUTF8String:NameUtf8.c_str()];
	  NSValue* Current = CurrentName == nil ? nil : self->Connections[CurrentName];
	  if (Current == nil || Current.pointerValue != Connection) return;
	  if (State == nw_connection_state_ready)
	  {
		  (void) [self publishResult:CurrentName connection:Connection logFailure:true];
		  [self cancelConnection:CurrentName];
	  }
	  else if (State == nw_connection_state_waiting)
	  {
		  // DNS-SD resolution can complete even when the service's TCP port is
		  // blocked. Publish the effective endpoint as soon as the path exposes it;
		  // otherwise the timeout below owns cleanup.
		  if ([self publishResult:CurrentName connection:Connection logFailure:false])
		  {
			  [self cancelConnection:CurrentName];
		  }
	  }
	  else if (State == nw_connection_state_failed)
	  {
		  UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Bonjour resolve failed for %s (%d)"),
		         *FString(UTF8_TO_TCHAR(NameUtf8.c_str())), Error == nullptr ? 0 : nw_error_get_error_code(Error));
		  [self cancelConnection:CurrentName];
	  }
	});
	nw_connection_set_queue(Connection, dispatch_get_main_queue());
	nw_connection_start(Connection);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(ResolveTimeout * NSEC_PER_SEC)),
	               dispatch_get_main_queue(), ^{
	  if (self->SearchGeneration != Generation) return;
	  NSString* CurrentName = [NSString stringWithUTF8String:NameUtf8.c_str()];
	  NSValue* Current = CurrentName == nil ? nil : self->Connections[CurrentName];
	  if (Current == nil || Current.pointerValue != Connection) return;
	  UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Bonjour resolve timed out for %s"),
	         *FString(UTF8_TO_TCHAR(NameUtf8.c_str())));
	  [self cancelConnection:CurrentName];
	});
}

- (void)scheduleRetry:(uint64)Generation
{
	const NSTimeInterval Delay = RetryDelay;
	RetryDelay = FMath::Min(RetryDelay * 2.0, MaxRetryDelay);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(Delay * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
	  if (self->SearchGeneration == Generation && self->Callback) [self beginSearch];
	});
}

- (void)beginSearch
{
	[self cancelBrowser];
	if (@available(macOS 10.15, iOS 13.0, tvOS 13.0, *))
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
		  if (self->SearchGeneration != Generation) return;
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
			  if (self->SearchGeneration != Generation) return;
			  nw_browse_result_t Result = NewResult != nullptr ? NewResult : OldResult;
			  if (Result == nullptr) return;
			  nw_endpoint_t Endpoint = nw_browse_result_copy_endpoint(Result);
			  if (Endpoint == nullptr || nw_endpoint_get_type(Endpoint) != nw_endpoint_type_bonjour_service)
			  {
				  if (Endpoint != nullptr) nw_release(Endpoint);
				  return;
			  }
			  const char* RawName = nw_endpoint_get_bonjour_service_name(Endpoint);
			  NSString* Name = RawName == nullptr ? nil : [NSString stringWithUTF8String:RawName];
			  if (Name == nil)
			  {
				  nw_release(Endpoint);
				  return;
			  }
			  if (NewResult == nullptr) [self removeResult:Name];
			  else [self resolveResult:Result endpoint:Endpoint name:Name];
			  nw_release(Endpoint);
			});
		nw_browser_set_queue(Browser, dispatch_get_main_queue());
		nw_browser_start(Browser);
		return;
	}
	UE_LOG(LogDeviceExplorerDiscoveryApple, Warning, TEXT("Network.framework Bonjour browsing requires macOS 10.15 or iOS/tvOS 13"));
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
	for (NSString* Name in Connections.allKeys) [self cancelConnection:Name];
	[Fingerprints removeAllObjects];
	[PublishedServiceNames removeAllObjects];
	Callback = nullptr;
}

@end

class FAppleDeviceExplorerEndpointSource final : public IDeviceExplorerEndpointSource
{
public:
	virtual ~FAppleDeviceExplorerEndpointSource() override { Stop(); }
	virtual FName GetProviderId() const override { return AppleBonjourProviderId; }

	virtual void Start(FDeviceExplorerEndpointEventCallback Callback) override
	{
		FDeviceExplorerNetworkBrowserDelegate* DelegateToStart = [[FDeviceExplorerNetworkBrowserDelegate alloc] init];
		FDeviceExplorerNetworkBrowserDelegate* Previous = Delegate;
		Delegate = DelegateToStart;
		const FDeviceExplorerEndpointEventCallback CallbackCopy = MoveTemp(Callback);
		dispatch_async(dispatch_get_main_queue(), ^{
		  if (Previous != nil)
		  {
			  [Previous stop];
#if !__has_feature(objc_arc)
			  [Previous release];
#endif
		  }
		  [DelegateToStart start:CallbackCopy];
		});
	}

	virtual void Stop() override
	{
		FDeviceExplorerNetworkBrowserDelegate* DelegateToStop = Delegate;
		Delegate = nil;
		if (DelegateToStop == nil) return;
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
