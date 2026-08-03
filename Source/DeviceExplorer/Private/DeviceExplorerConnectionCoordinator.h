#pragma once

#include "CoreMinimal.h"
#include "DeviceExplorerEndpoint.h"

/**
 * Chooses connection attempts from concurrently active endpoint sources.
 * The coordinator owns policy only; the module owns the actual transport.
 */
class FDeviceExplorerConnectionCoordinator
{
public:
	void ApplyEvent(EDeviceExplorerEndpointEvent Event, FDeviceExplorerEndpointCandidate Candidate);

	/** Returns true when a live automatic connection should be replaced. */
	bool Pin(FDeviceExplorerEndpointCandidate Candidate);

	/** Returns true when the currently active connection is the removed pin. */
	bool Unpin();

	bool SelectNext(double NowSeconds, FDeviceExplorerEndpointCandidate& OutCandidate);
	void MarkConnected(const FDeviceExplorerEndpointCandidate& Candidate);
	void MarkFailed(const FDeviceExplorerEndpointCandidate& Candidate, double NowSeconds);
	void AbandonActive();

	bool HasActive() const { return ActiveCandidate.IsSet(); }
	bool IsActive(const FDeviceExplorerEndpointCandidate& Candidate) const;

private:
	struct FRetryState
	{
		int32 FailureCount = 0;
		double RetryAtSeconds = 0.0;
	};

	FDeviceExplorerEndpointCandidate ResolveLatest(const FDeviceExplorerEndpointCandidate& Candidate) const;
	bool IsReady(const FDeviceExplorerEndpointCandidate& Candidate, double NowSeconds) const;
	void Select(FDeviceExplorerEndpointCandidate Candidate, FDeviceExplorerEndpointCandidate& OutCandidate);

	TMap<FString, FDeviceExplorerEndpointCandidate> Candidates;
	TMap<FString, FRetryState> RetryStates;
	TOptional<FDeviceExplorerEndpointCandidate> ManualPin;
	TOptional<FDeviceExplorerEndpointCandidate> LastGoodCandidate;
	TOptional<FDeviceExplorerEndpointCandidate> ActiveCandidate;
};
