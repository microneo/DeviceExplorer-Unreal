#pragma once

#include "CoreMinimal.h"

enum class EDeviceExplorerAddressFamily : uint8
{
	Unspecified,
	IPv4,
	IPv6
};

/** Address data that is safe to persist or send over the wire. */
struct FDeviceExplorerSerializedEndpoint
{
	FString Address;
	int32 Port = 0;
	EDeviceExplorerAddressFamily Family = EDeviceExplorerAddressFamily::Unspecified;

	bool IsValid() const
	{
		return !Address.IsEmpty() && Port > 0 && Port <= 65535 && Family != EDeviceExplorerAddressFamily::Unspecified;
	}

	bool IsSupported() const
	{
		// The structure is intentionally ready for IPv6, but protocol v1 remains
		// explicitly IPv4-only until discovery and listeners support scope ids.
		return IsValid() && Family == EDeviceExplorerAddressFamily::IPv4;
	}

	FString ToString() const
	{
		return Family == EDeviceExplorerAddressFamily::IPv6
			       ? FString::Printf(TEXT("[%s]:%d"), *Address, Port)
			       : FString::Printf(TEXT("%s:%d"), *Address, Port);
	}

	bool operator==(const FDeviceExplorerSerializedEndpoint& Other) const
	{
		return Address == Other.Address && Port == Other.Port && Family == Other.Family;
	}
};

/** Process-local address metadata. ScopeId and interface names are never serialized. */
struct FDeviceExplorerResolvedEndpoint
{
	FDeviceExplorerSerializedEndpoint Serialized;
	uint32 ScopeId = 0;
	FString Interface;

	bool IsUsable() const
	{
		return Serialized.IsSupported();
	}

	bool operator==(const FDeviceExplorerResolvedEndpoint& Other) const
	{
		return Serialized == Other.Serialized && ScopeId == Other.ScopeId && Interface == Other.Interface;
	}
};

enum class EDeviceExplorerEndpointEvent : uint8
{
	Added,
	Updated,
	Removed
};

struct FDeviceExplorerEndpointCandidate
{
	FName ProviderId;
	FString CandidateId;
	FDeviceExplorerResolvedEndpoint Endpoint;
	FString Token;
	FString HostFingerprint;
	FString Instance;
	bool bManual = false;

	bool IsValid() const
	{
		// A discovery source reports where a host is and which token it claims, never the
		// token itself, so either one identifies a usable candidate.
		return !ProviderId.IsNone() && !CandidateId.IsEmpty() && Endpoint.IsUsable() &&
		       (!Token.IsEmpty() || !HostFingerprint.IsEmpty());
	}

	FString Key() const
	{
		return ProviderId.ToString() + TEXT(":") + CandidateId;
	}
};

using FDeviceExplorerEndpointEventCallback = TFunction<void(EDeviceExplorerEndpointEvent, FDeviceExplorerEndpointCandidate)>;

class DEVICEEXPLORERCORE_API IDeviceExplorerEndpointSource
{
public:
	virtual ~IDeviceExplorerEndpointSource() = default;
	virtual FName GetProviderId() const = 0;
	virtual void Start(FDeviceExplorerEndpointEventCallback Callback) = 0;
	virtual void Stop() = 0;
};

using FDeviceExplorerEndpointSourceFactory = TFunction<TUniquePtr<IDeviceExplorerEndpointSource>()>;
