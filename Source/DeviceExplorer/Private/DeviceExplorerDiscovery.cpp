#include "DeviceExplorerDiscovery.h"

#include "Interfaces/IPv4/IPv4Address.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerEndpointConfig, Log, All);

#if PLATFORM_IOS
TUniquePtr<IDeviceExplorerEndpointSource> CreateAppleDeviceExplorerEndpointSource();
#else
TUniquePtr<IDeviceExplorerEndpointSource> CreateMdnsDeviceExplorerEndpointSource();
#endif

namespace
{
const FName ConfigProviderId(TEXT("Config"));
const TCHAR* SettingsSection = TEXT("/Script/DeviceExplorer.Settings");

class FConfigDeviceExplorerEndpointSource final : public IDeviceExplorerEndpointSource
{
public:
	virtual FName GetProviderId() const override { return ConfigProviderId; }

	virtual void Start(FDeviceExplorerEndpointEventCallback InCallback) override
	{
		FString Endpoint;
		FString Token;
		FString CandidateId;
		bool bManual = false;
		if (FParse::Value(FCommandLine::Get(), TEXT("DeviceExplorerServer="), Endpoint))
		{
			FParse::Value(FCommandLine::Get(), TEXT("DeviceExplorerToken="), Token);
			CandidateId = TEXT("CommandLine");
			bManual = true;
		}
		else if (GConfig != nullptr)
		{
			GConfig->GetString(SettingsSection, TEXT("Endpoint"), Endpoint, GGameUserSettingsIni);
			GConfig->GetString(SettingsSection, TEXT("Token"), Token, GGameUserSettingsIni);
			CandidateId = TEXT("UserIni");
		}

		if (Endpoint.IsEmpty())
		{
			return;
		}
		if (Token.IsEmpty())
		{
			UE_LOG(LogDeviceExplorerEndpointConfig, Warning, TEXT("Ignoring configured endpoint %s because its token is empty"), *Endpoint);
			return;
		}

		FDeviceExplorerSerializedEndpoint ParsedEndpoint;
		FString Error;
		if (!ParseDeviceExplorerEndpoint(Endpoint, ParsedEndpoint, &Error))
		{
			UE_LOG(LogDeviceExplorerEndpointConfig, Warning, TEXT("Ignoring configured endpoint %s: %s"), *Endpoint, *Error);
			return;
		}

		FDeviceExplorerEndpointCandidate Candidate;
		Candidate.ProviderId = ConfigProviderId;
		Candidate.CandidateId = MoveTemp(CandidateId);
		Candidate.Endpoint.Serialized = MoveTemp(ParsedEndpoint);
		Candidate.Token = MoveTemp(Token);
		Candidate.Instance = TEXT("Configured");
		Candidate.bManual = bManual;
		InCallback(EDeviceExplorerEndpointEvent::Added, MoveTemp(Candidate));
	}

	virtual void Stop() override {}
};
}    // namespace

bool ParseDeviceExplorerEndpoint(const FString& Text, FDeviceExplorerSerializedEndpoint& OutEndpoint, FString* OutError)
{
	auto Fail = [OutError](const TCHAR* Message)
	{
		if (OutError != nullptr)
		{
			*OutError = Message;
		}
		return false;
	};

	FString AddressText;
	FString PortText;
	if (!Text.Split(TEXT(":"), &AddressText, &PortText, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		return Fail(TEXT("expected an IPv4 endpoint in address:port form"));
	}
	AddressText.TrimStartAndEndInline();
	PortText.TrimStartAndEndInline();

	FIPv4Address Address;
	if (!FIPv4Address::Parse(AddressText, Address))
	{
		return Fail(TEXT("only IPv4 literals are supported in protocol v1"));
	}

	if (!PortText.IsNumeric())
	{
		return Fail(TEXT("port must be a decimal number"));
	}
	const int32 Port = FCString::Atoi(*PortText);
	if (Port <= 0 || Port > 65535)
	{
		return Fail(TEXT("port must be between 1 and 65535"));
	}

	OutEndpoint.Address = Address.ToString();
	OutEndpoint.Port = Port;
	OutEndpoint.Family = EDeviceExplorerAddressFamily::IPv4;
	return true;
}

TArray<TUniquePtr<IDeviceExplorerEndpointSource>> CreateDeviceExplorerEndpointSources()
{
	TArray<TUniquePtr<IDeviceExplorerEndpointSource>> Result;
	Result.Add(MakeUnique<FConfigDeviceExplorerEndpointSource>());
#if PLATFORM_IOS
	Result.Add(CreateAppleDeviceExplorerEndpointSource());
#else
	Result.Add(CreateMdnsDeviceExplorerEndpointSource());
#endif
	return Result;
}
