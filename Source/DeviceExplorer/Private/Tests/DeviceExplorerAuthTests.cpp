#include "DeviceExplorerAuth.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeviceExplorerAuthVectorsTest,
                                 "DeviceExplorer.Auth.KnownVectors",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeviceExplorerAuthVectorsTest::RunTest(const FString& Parameters)
{
	(void) Parameters;
	const FString Token = TEXT("token");
	const FString ClientNonce = TEXT("0123456789abcdef0123456789abcdef");
	const FString HostNonce = TEXT("00000000000000000000000000000000");

	TestEqual(TEXT("host proof matches HMAC-SHA256 vector"),
	          DeviceExplorer::Auth::ComputeProof(Token, DeviceExplorer::Auth::HostProofLabel, ClientNonce, HostNonce),
	          FString(TEXT("e5ad3480f24e7d81df61e1ce266ec2bea161e0c6abfe2ca702dedb12e4d6725f")));
	TestEqual(TEXT("device proof uses a distinct label"),
	          DeviceExplorer::Auth::ComputeProof(Token, DeviceExplorer::Auth::DeviceProofLabel, ClientNonce, HostNonce),
	          FString(TEXT("4cd37f68e1756ece49d208b6f35b2c6f83708d6a5036da8a4195e16a5d4ef887")));
	TestEqual(TEXT("fingerprint matches SHA-256 vector"),
	          DeviceExplorer::Auth::ComputeTokenFingerprint(Token),
	          FString(TEXT("1ec0cfe231196ff9")));

	TestTrue(TEXT("canonical lowercase nonce is accepted"), DeviceExplorer::Auth::IsValidNonce(ClientNonce));
	TestFalse(TEXT("uppercase nonce is rejected"),
	          DeviceExplorer::Auth::IsValidNonce(TEXT("0123456789ABCDEF0123456789ABCDEF")));
	TestTrue(TEXT("equal non-empty proofs compare equal"),
	         DeviceExplorer::Auth::ConstantTimeEquals(TEXT("proof"), TEXT("proof")));
	TestFalse(TEXT("empty values never authenticate"), DeviceExplorer::Auth::ConstantTimeEquals(FString(), FString()));
	return true;
}

#endif
