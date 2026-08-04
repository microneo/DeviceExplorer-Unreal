#pragma once

#include "CoreMinimal.h"

// The device link carries a proof of the session token, never the token itself, and both
// sides prove knowledge of it: a host that cannot answer the challenge is not the host
// that issued the token.
namespace DeviceExplorer::Auth
{
// Distinct labels keep one side's proof from being replayed as the other's.
inline const TCHAR* HostProofLabel = TEXT("deviceexplorer-host-v1");
inline const TCHAR* DeviceProofLabel = TEXT("deviceexplorer-device-v1");

inline constexpr int32 NonceHexLength = 32;

DEVICEEXPLORERCORE_API FString MakeNonce();

DEVICEEXPLORERCORE_API bool IsValidNonce(const FString& Value);

DEVICEEXPLORERCORE_API FString ComputeProof(const FString& Token,
	                                        const FString& Label,
	                                        const FString& ClientNonce,
	                                        const FString& HostNonce);

/** Public label for a session token, safe to advertise: it identifies a host without exposing the token. */
DEVICEEXPLORERCORE_API FString ComputeTokenFingerprint(const FString& Token);

DEVICEEXPLORERCORE_API bool ConstantTimeEquals(const FString& A, const FString& B);

/** A short token is recoverable offline: the advertised fingerprint is dictionary-checkable. */
DEVICEEXPLORERCORE_API bool IsWeakToken(const FString& Token);

/** Set from a launch argument, project settings, or by the Editor for an in-process client. Thread safe. */
DEVICEEXPLORERCORE_API void SetProvisionedToken(const FString& Token);
DEVICEEXPLORERCORE_API FString GetProvisionedToken();
}    // namespace DeviceExplorer::Auth
