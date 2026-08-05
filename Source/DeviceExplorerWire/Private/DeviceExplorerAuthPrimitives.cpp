#include "DeviceExplorerAuthPrimitives.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace DeviceExplorer::Wire::Auth
{
namespace
{
constexpr std::size_t Sha256BlockBytes = 64;
constexpr std::size_t Sha256DigestBytes = 32;
constexpr std::size_t FingerprintBytes = 8;

constexpr std::array<std::uint32_t, 64> RoundConstants = {
	0x428A2F98, 0x71374491, 0xB5C0FBCF, 0xE9B5DBA5, 0x3956C25B, 0x59F111F1, 0x923F82A4, 0xAB1C5ED5,
	0xD807AA98, 0x12835B01, 0x243185BE, 0x550C7DC3, 0x72BE5D74, 0x80DEB1FE, 0x9BDC06A7, 0xC19BF174,
	0xE49B69C1, 0xEFBE4786, 0x0FC19DC6, 0x240CA1CC, 0x2DE92C6F, 0x4A7484AA, 0x5CB0A9DC, 0x76F988DA,
	0x983E5152, 0xA831C66D, 0xB00327C8, 0xBF597FC7, 0xC6E00BF3, 0xD5A79147, 0x06CA6351, 0x14292967,
	0x27B70A85, 0x2E1B2138, 0x4D2C6DFC, 0x53380D13, 0x650A7354, 0x766A0ABB, 0x81C2C92E, 0x92722C85,
	0xA2BFE8A1, 0xA81A664B, 0xC24B8B70, 0xC76C51A3, 0xD192E819, 0xD6990624, 0xF40E3585, 0x106AA070,
	0x19A4C116, 0x1E376C08, 0x2748774C, 0x34B0BCB5, 0x391C0CB3, 0x4ED8AA4A, 0x5B9CCA4F, 0x682E6FF3,
	0x748F82EE, 0x78A5636F, 0x84C87814, 0x8CC70208, 0x90BEFFFA, 0xA4506CEB, 0xBEF9A3F7, 0xC67178F2
};

std::uint32_t RotateRight(const std::uint32_t Value, const std::uint32_t Bits)
{
	return (Value >> Bits) | (Value << (32U - Bits));
}

void CompressBlock(const std::uint8_t* Block, std::array<std::uint32_t, 8>& State)
{
	std::array<std::uint32_t, 64> Words{};
	for (std::size_t Index = 0; Index < 16; ++Index)
	{
		const std::size_t Offset = Index * 4;
		Words[Index] = (static_cast<std::uint32_t>(Block[Offset]) << 24) |
		               (static_cast<std::uint32_t>(Block[Offset + 1]) << 16) |
		               (static_cast<std::uint32_t>(Block[Offset + 2]) << 8) |
		               static_cast<std::uint32_t>(Block[Offset + 3]);
	}
	for (std::size_t Index = 16; Index < Words.size(); ++Index)
	{
		const std::uint32_t S0 = RotateRight(Words[Index - 15], 7) ^ RotateRight(Words[Index - 15], 18) ^ (Words[Index - 15] >> 3);
		const std::uint32_t S1 = RotateRight(Words[Index - 2], 17) ^ RotateRight(Words[Index - 2], 19) ^ (Words[Index - 2] >> 10);
		Words[Index] = Words[Index - 16] + S0 + Words[Index - 7] + S1;
	}

	std::uint32_t A = State[0];
	std::uint32_t B = State[1];
	std::uint32_t C = State[2];
	std::uint32_t D = State[3];
	std::uint32_t E = State[4];
	std::uint32_t F = State[5];
	std::uint32_t G = State[6];
	std::uint32_t H = State[7];
	for (std::size_t Index = 0; Index < Words.size(); ++Index)
	{
		const std::uint32_t Sigma1 = RotateRight(E, 6) ^ RotateRight(E, 11) ^ RotateRight(E, 25);
		const std::uint32_t Choice = (E & F) ^ ((~E) & G);
		const std::uint32_t Temporary1 = H + Sigma1 + Choice + RoundConstants[Index] + Words[Index];
		const std::uint32_t Sigma0 = RotateRight(A, 2) ^ RotateRight(A, 13) ^ RotateRight(A, 22);
		const std::uint32_t Majority = (A & B) ^ (A & C) ^ (B & C);
		const std::uint32_t Temporary2 = Sigma0 + Majority;
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

std::array<std::uint8_t, Sha256DigestBytes> Sha256(const std::string_view Message)
{
	std::array<std::uint32_t, 8> State = {
		0x6A09E667, 0xBB67AE85, 0x3C6EF372, 0xA54FF53A,
		0x510E527F, 0x9B05688C, 0x1F83D9AB, 0x5BE0CD19
	};
	const std::size_t FullBlocks = Message.size() / Sha256BlockBytes;
	for (std::size_t Block = 0; Block < FullBlocks; ++Block)
	{
		CompressBlock(reinterpret_cast<const std::uint8_t*>(Message.data()) + Block * Sha256BlockBytes, State);
	}

	std::array<std::uint8_t, Sha256BlockBytes * 2> Tail{};
	const std::size_t Remainder = Message.size() - FullBlocks * Sha256BlockBytes;
	std::copy_n(reinterpret_cast<const std::uint8_t*>(Message.data()) + FullBlocks * Sha256BlockBytes,
	            Remainder, Tail.data());
	Tail[Remainder] = 0x80;
	const std::size_t TailBlocks = Remainder + 9 > Sha256BlockBytes ? 2 : 1;
	const std::uint64_t BitLength = static_cast<std::uint64_t>(Message.size()) * 8ULL;
	for (std::size_t Index = 0; Index < 8; ++Index)
	{
		Tail[TailBlocks * Sha256BlockBytes - 1 - Index] = static_cast<std::uint8_t>((BitLength >> (Index * 8)) & 0xFF);
	}
	for (std::size_t Block = 0; Block < TailBlocks; ++Block)
	{
		CompressBlock(Tail.data() + Block * Sha256BlockBytes, State);
	}

	std::array<std::uint8_t, Sha256DigestBytes> Digest{};
	for (std::size_t Index = 0; Index < State.size(); ++Index)
	{
		Digest[Index * 4] = static_cast<std::uint8_t>((State[Index] >> 24) & 0xFF);
		Digest[Index * 4 + 1] = static_cast<std::uint8_t>((State[Index] >> 16) & 0xFF);
		Digest[Index * 4 + 2] = static_cast<std::uint8_t>((State[Index] >> 8) & 0xFF);
		Digest[Index * 4 + 3] = static_cast<std::uint8_t>(State[Index] & 0xFF);
	}
	return Digest;
}

std::array<std::uint8_t, Sha256DigestBytes> HmacSha256(const std::string_view Key,
	                                                   const std::string_view Message)
{
	std::array<std::uint8_t, Sha256BlockBytes> PaddedKey{};
	if (Key.size() > Sha256BlockBytes)
	{
		const auto Digest = Sha256(Key);
		std::copy(Digest.begin(), Digest.end(), PaddedKey.begin());
	}
	else
	{
		std::copy(Key.begin(), Key.end(), PaddedKey.begin());
	}

	std::string Inner(Sha256BlockBytes, '\0');
	std::string Outer(Sha256BlockBytes, '\0');
	for (std::size_t Index = 0; Index < Sha256BlockBytes; ++Index)
	{
		Inner[Index] = static_cast<char>(PaddedKey[Index] ^ 0x36);
		Outer[Index] = static_cast<char>(PaddedKey[Index] ^ 0x5C);
	}
	Inner.append(Message.data(), Message.size());
	const auto InnerDigest = Sha256(Inner);
	Outer.append(reinterpret_cast<const char*>(InnerDigest.data()), InnerDigest.size());
	return Sha256(Outer);
}

std::string ToHex(const std::uint8_t* Bytes, const std::size_t Count)
{
	static constexpr char HexDigits[] = "0123456789abcdef";
	std::string Result(Count * 2, '\0');
	for (std::size_t Index = 0; Index < Count; ++Index)
	{
		Result[Index * 2] = HexDigits[(Bytes[Index] >> 4) & 0x0F];
		Result[Index * 2 + 1] = HexDigits[Bytes[Index] & 0x0F];
	}
	return Result;
}
}    // namespace

bool IsValidNonce(const std::string_view Value)
{
	if (Value.size() != NonceHexLength) return false;
	return std::all_of(Value.begin(), Value.end(), [](const char Character)
	{
		return (Character >= '0' && Character <= '9') || (Character >= 'a' && Character <= 'f');
	});
}

std::string ComputeProof(const std::string_view Token,
	                     const std::string_view Label,
	                     const std::string_view ClientNonce,
	                     const std::string_view HostNonce)
{
	std::string Transcript;
	Transcript.reserve(Label.size() + ClientNonce.size() + HostNonce.size() + 2);
	Transcript.append(Label.data(), Label.size());
	Transcript.push_back('\n');
	Transcript.append(ClientNonce.data(), ClientNonce.size());
	Transcript.push_back('\n');
	Transcript.append(HostNonce.data(), HostNonce.size());
	const auto Digest = HmacSha256(Token, Transcript);
	return ToHex(Digest.data(), Digest.size());
}

std::string ComputeTokenFingerprint(const std::string_view Token)
{
	std::string Input = "deviceexplorer-fp-v1\n";
	Input.append(Token.data(), Token.size());
	const auto Digest = Sha256(Input);
	return ToHex(Digest.data(), FingerprintBytes);
}

bool ConstantTimeEquals(const std::string_view Left, const std::string_view Right)
{
	if (Left.empty() || Left.size() != Right.size()) return false;
	unsigned Difference = 0;
	for (std::size_t Index = 0; Index < Left.size(); ++Index)
	{
		Difference |= static_cast<unsigned>(static_cast<unsigned char>(Left[Index]) ^
		                                    static_cast<unsigned char>(Right[Index]));
	}
	return Difference == 0;
}
}    // namespace DeviceExplorer::Wire::Auth
