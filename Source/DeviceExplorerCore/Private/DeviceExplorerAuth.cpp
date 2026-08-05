#include "DeviceExplorerAuth.h"

#include "Containers/StringConv.h"
#include "HAL/CriticalSection.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"

namespace DeviceExplorer::Auth
{
namespace
{
constexpr int32 Sha256BlockBytes = 64;
constexpr int32 Sha256DigestBytes = 32;
constexpr int32 FingerprintBytes = 8;

constexpr uint32 RoundConstants[64] = {
	0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
	0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
	0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
	0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
	0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
	0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
	0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
	0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2
};

uint32 RotateRight(const uint32 Value, const uint32 Bits)
{
	return (Value >> Bits) | (Value << (32U - Bits));
}

void CompressBlock(const uint8* Block, uint32* State)
{
	uint32 Words[64] = {};
	for (int32 Index = 0; Index < 16; ++Index)
	{
		const int32 Offset = Index * 4;
		Words[Index] = (static_cast<uint32>(Block[Offset]) << 24) |
		               (static_cast<uint32>(Block[Offset + 1]) << 16) |
		               (static_cast<uint32>(Block[Offset + 2]) << 8) |
		               static_cast<uint32>(Block[Offset + 3]);
	}
	for (int32 Index = 16; Index < 64; ++Index)
	{
		const uint32 S0 = RotateRight(Words[Index - 15], 7) ^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
		const uint32 S1 = RotateRight(Words[Index - 2], 17) ^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
		Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
	}

	uint32 A = State[0];
	uint32 B = State[1];
	uint32 C = State[2];
	uint32 D = State[3];
	uint32 E = State[4];
	uint32 F = State[5];
	uint32 G = State[6];
	uint32 H = State[7];
	for (int32 Index = 0; Index < 64; ++Index)
	{
		const uint32 Sigma1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
		const uint32 Choice = (E & F) ^ ((~E) & G);
		const uint32 Temporary1 = H + Sigma1 + Choice + RoundConstants[Index] + Words[Index];
		const uint32 Sigma0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
		const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
		const uint32 Temporary2 = Sigma0 + Majority;
		H = G;
		G = F;
		F = E;
		E = D + Temporary1;
		D = C;
		C = B;
		B = A;
		A = Temporary1 + Temporary2;
	}
	State[0] += A;
	State[1] += B;
	State[2] += C;
	State[3] += D;
	State[4] += E;
	State[5] += F;
	State[6] += G;
	State[7] += H;
}

TArray<uint8> Sha256(const TArray<uint8>& Message)
{
	uint32 State[8] = { 0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
		                0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19 };

	const int32 FullBlocks = Message.Num() / Sha256BlockBytes;
	for (int32 Block = 0; Block < FullBlocks; ++Block)
	{
		CompressBlock(Message.GetData() + Block * Sha256BlockBytes, State);
	}

	// Tail plus padding never exceeds two blocks: up to 63 bytes of data, one 0x80 byte
	// and an 8-byte length.
	uint8 Tail[Sha256BlockBytes * 2] = {};
	const int32 Remainder = Message.Num() - FullBlocks * Sha256BlockBytes;
	for (int32 Index = 0; Index < Remainder; ++Index)
	{
		Tail[Index] = Message[FullBlocks * Sha256BlockBytes + Index];
	}
	Tail[Remainder] = 0x80;
	const int32 TailBlocks = Remainder + 1 + 8 > Sha256BlockBytes ? 2 : 1;
	const uint64 BitLength = static_cast<uint64>(Message.Num()) * 8ULL;
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Tail[TailBlocks * Sha256BlockBytes - 1 - Index] = static_cast<uint8>((BitLength >> (Index * 8)) & 0xFF);
	}
	for (int32 Block = 0; Block < TailBlocks; ++Block)
	{
		CompressBlock(Tail + Block * Sha256BlockBytes, State);
	}

	TArray<uint8> Digest;
	Digest.SetNumUninitialized(Sha256DigestBytes);
	for (int32 Index = 0; Index < 8; ++Index)
	{
		Digest[Index * 4] = static_cast<uint8>((State[Index] >> 24) & 0xFF);
		Digest[Index * 4 + 1] = static_cast<uint8>((State[Index] >> 16) & 0xFF);
		Digest[Index * 4 + 2] = static_cast<uint8>((State[Index] >> 8) & 0xFF);
		Digest[Index * 4 + 3] = static_cast<uint8>(State[Index] & 0xFF);
	}
	return Digest;
}

TArray<uint8> HmacSha256(const TArray<uint8>& Key, const TArray<uint8>& Message)
{
	TArray<uint8> PaddedKey;
	PaddedKey.AddZeroed(Sha256BlockBytes);
	const TArray<uint8> ShortKey = Key.Num() > Sha256BlockBytes ? Sha256(Key) : Key;
	for (int32 Index = 0; Index < ShortKey.Num(); ++Index)
	{
		PaddedKey[Index] = ShortKey[Index];
	}

	TArray<uint8> Inner;
	TArray<uint8> Outer;
	Inner.SetNumUninitialized(Sha256BlockBytes);
	Outer.SetNumUninitialized(Sha256BlockBytes);
	for (int32 Index = 0; Index < Sha256BlockBytes; ++Index)
	{
		Inner[Index] = static_cast<uint8>(PaddedKey[Index] ^ 0x36);
		Outer[Index] = static_cast<uint8>(PaddedKey[Index] ^ 0x5C);
	}
	Inner.Append(Message);
	Outer.Append(Sha256(Inner));
	return Sha256(Outer);
}

TArray<uint8> Utf8Bytes(const FString& Value)
{
	const FTCHARToUTF8 Converted(*Value);
	TArray<uint8> Bytes;
	Bytes.Append(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	return Bytes;
}

FCriticalSection ProvisionedTokenMutex;
FString ProvisionedToken;

FString ToHex(const TArray<uint8>& Bytes, const int32 ByteCount)
{
	static const TCHAR* HexDigits = TEXT("0123456789abcdef");
	const int32 Count = FMath::Min(ByteCount, Bytes.Num());
	FString Result;
	Result.Reserve(Count * 2);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Result.AppendChar(HexDigits[(Bytes[Index] >> 4) & 0x0F]);
		Result.AppendChar(HexDigits[Bytes[Index] & 0x0F]);
	}
	return Result;
}
}    // namespace

FString MakeNonce()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
}

bool IsValidNonce(const FString& Value)
{
	if (Value.Len() != NonceHexLength)
	{
		return false;
	}
	for (const TCHAR Character : Value)
	{
		const bool bHexDigit = (Character >= TEXT('0') && Character <= TEXT('9')) ||
		                       (Character >= TEXT('a') && Character <= TEXT('f'));
		if (!bHexDigit)
		{
			return false;
		}
	}
	return true;
}

FString ComputeProof(const FString& Token,
	                 const FString& Label,
	                 const FString& ClientNonce,
	                 const FString& HostNonce)
{
	// Nonces are hex and labels are ASCII without newlines, so '\n' separators keep the
	// transcript unambiguous without length prefixes.
	const FString Transcript = Label + TEXT("\n") + ClientNonce + TEXT("\n") + HostNonce;
	return ToHex(HmacSha256(Utf8Bytes(Token), Utf8Bytes(Transcript)), Sha256DigestBytes);
}

FString ComputeTokenFingerprint(const FString& Token)
{
	return ToHex(Sha256(Utf8Bytes(TEXT("deviceexplorer-fp-v1\n") + Token)), FingerprintBytes);
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
	if (A.Len() != B.Len())
	{
		return false;
	}
	uint32 Difference = 0;
	for (int32 Index = 0; Index < A.Len(); ++Index)
	{
		Difference |= static_cast<uint32>(A[Index] ^ B[Index]);
	}
	return Difference == 0;
}
}    // namespace DeviceExplorer::Auth
