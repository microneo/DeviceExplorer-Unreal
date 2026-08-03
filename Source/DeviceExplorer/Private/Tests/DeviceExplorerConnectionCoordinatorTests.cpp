#include "DeviceExplorerConnectionCoordinator.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

namespace
{
FDeviceExplorerEndpointCandidate MakeCandidate(const TCHAR* Provider, const TCHAR* CandidateId, const int32 LastOctet)
{
	FDeviceExplorerEndpointCandidate Candidate;
	Candidate.ProviderId = Provider;
	Candidate.CandidateId = CandidateId;
	Candidate.Endpoint.Serialized.Address = FString::Printf(TEXT("192.0.2.%d"), LastOctet);
	Candidate.Endpoint.Serialized.Port = 42111;
	Candidate.Endpoint.Serialized.Family = EDeviceExplorerAddressFamily::IPv4;
	Candidate.Token = TEXT("test-token");
	Candidate.Instance = CandidateId;
	return Candidate;
}
}    // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeviceExplorerCoordinatorStickyTest,
                                 "DeviceExplorer.ConnectionCoordinator.StickyAndLastGood",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeviceExplorerCoordinatorStickyTest::RunTest(const FString& Parameters)
{
	(void) Parameters;
	FDeviceExplorerConnectionCoordinator Coordinator;
	const FDeviceExplorerEndpointCandidate CandidateZ = MakeCandidate(TEXT("ZProvider"), TEXT("one"), 1);
	const FDeviceExplorerEndpointCandidate CandidateA = MakeCandidate(TEXT("AProvider"), TEXT("two"), 2);
	Coordinator.ApplyEvent(EDeviceExplorerEndpointEvent::Added, CandidateZ);
	Coordinator.ApplyEvent(EDeviceExplorerEndpointEvent::Added, CandidateA);

	FDeviceExplorerEndpointCandidate Selected;
	TestTrue(TEXT("selects an initial candidate"), Coordinator.SelectNext(0.0, Selected));
	TestEqual(TEXT("provider tie-break is stable"), Selected.ProviderId, CandidateA.ProviderId);
	Coordinator.MarkConnected(Selected);
	TestFalse(TEXT("a live connection remains sticky"), Coordinator.SelectNext(0.0, Selected));

	Coordinator.MarkFailed(CandidateA, 0.0);
	TestTrue(TEXT("another candidate is tried while last-good backs off"), Coordinator.SelectNext(0.0, Selected));
	TestEqual(TEXT("fallback candidate selected"), Selected.ProviderId, CandidateZ.ProviderId);
	Coordinator.MarkFailed(Selected, 0.0);
	TestFalse(TEXT("all failed candidates honor backoff"), Coordinator.SelectNext(0.5, Selected));
	TestTrue(TEXT("last-good is retried first after backoff"), Coordinator.SelectNext(1.0, Selected));
	TestEqual(TEXT("last-good candidate selected"), Selected.ProviderId, CandidateA.ProviderId);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeviceExplorerCoordinatorManualPinTest,
                                 "DeviceExplorer.ConnectionCoordinator.ManualPin",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeviceExplorerCoordinatorManualPinTest::RunTest(const FString& Parameters)
{
	(void) Parameters;
	FDeviceExplorerConnectionCoordinator Coordinator;
	const FDeviceExplorerEndpointCandidate Automatic = MakeCandidate(TEXT("Mdns"), TEXT("automatic"), 10);
	FDeviceExplorerEndpointCandidate Manual = MakeCandidate(TEXT("Console"), TEXT("ManualPin"), 20);
	Manual.bManual = true;
	Coordinator.ApplyEvent(EDeviceExplorerEndpointEvent::Added, Automatic);

	FDeviceExplorerEndpointCandidate Selected;
	TestTrue(TEXT("automatic candidate selected"), Coordinator.SelectNext(0.0, Selected));
	Coordinator.MarkConnected(Selected);
	TestTrue(TEXT("manual pin replaces an automatic connection"), Coordinator.Pin(Manual));
	Coordinator.AbandonActive();
	TestTrue(TEXT("manual candidate selected"), Coordinator.SelectNext(0.0, Selected));
	TestEqual(TEXT("manual provider selected"), Selected.ProviderId, Manual.ProviderId);
	Coordinator.MarkFailed(Selected, 0.0);
	TestFalse(TEXT("manual pin blocks automatic fallback during backoff"), Coordinator.SelectNext(0.5, Selected));
	TestTrue(TEXT("manual pin is retried after backoff"), Coordinator.SelectNext(1.0, Selected));
	Coordinator.MarkConnected(Selected);

	TestTrue(TEXT("unpin replaces the manual connection"), Coordinator.Unpin());
	Coordinator.AbandonActive();
	TestTrue(TEXT("automatic selection resumes"), Coordinator.SelectNext(0.0, Selected));
	TestEqual(TEXT("automatic provider restored"), Selected.ProviderId, Automatic.ProviderId);
	return true;
}

#endif
