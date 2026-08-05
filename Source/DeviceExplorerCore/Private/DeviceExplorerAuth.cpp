#include "DeviceExplorerAuth.h"

#include "DeviceExplorerAuthPrimitives.h"
#include "HAL/CriticalSection.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

namespace DeviceExplorer::Auth
{
namespace
{
FCriticalSection ProvisionedTokenMutex;
FString ProvisionedToken;

std::string ToUtf8(const FString& Value)
{
	const FTCHARToUTF8 Converted(*Value);
	return { Converted.Get(), static_cast<std::size_t>(Converted.Length()) };
}

FString FromUtf8(const std::string& Value)
{
	const FUTF8ToTCHAR Converted(Value.data(), static_cast<int32>(Value.size()));
	return { Converted.Length(), Converted.Get() };
}
}    // namespace

FString MakeNonce()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
}

bool IsValidNonce(const FString& Value)
{
	return Wire::Auth::IsValidNonce(ToUtf8(Value));
}

FString ComputeProof(const FString& Token,
	                 const FString& Label,
	                 const FString& ClientNonce,
	                 const FString& HostNonce)
{
	return FromUtf8(Wire::Auth::ComputeProof(ToUtf8(Token), ToUtf8(Label), ToUtf8(ClientNonce), ToUtf8(HostNonce)));
}

FString ComputeTokenFingerprint(const FString& Token)
{
	return FromUtf8(Wire::Auth::ComputeTokenFingerprint(ToUtf8(Token)));
}

void SetProvisionedToken(const FString& Token)
{
	FScopeLock Lock(&ProvisionedTokenMutex);
	ProvisionedToken = Token;
}

FString GetProvisionedToken()
{
	FScopeLock Lock(&ProvisionedTokenMutex);
	return ProvisionedToken;
}

bool IsWeakToken(const FString& Token)
{
	return Token.Len() < 24;
}

bool ConstantTimeEquals(const FString& A, const FString& B)
{
	return Wire::Auth::ConstantTimeEquals(ToUtf8(A), ToUtf8(B));
}
}    // namespace DeviceExplorer::Auth
