#include "DeviceExplorerConnectionCoordinator.h"

namespace
{
constexpr double InitialRetryDelaySeconds = 1.0;
constexpr double MaxRetryDelaySeconds = 30.0;
}

void FDeviceExplorerConnectionCoordinator::ApplyEvent(const EDeviceExplorerEndpointEvent Event, FDeviceExplorerEndpointCandidate Candidate)
{
	const FString Key = Candidate.Key();
	if (Candidate.ProviderId.IsNone() || Candidate.CandidateId.IsEmpty())
	{
		return;
	}

	if (Event == EDeviceExplorerEndpointEvent::Removed)
	{
		Candidates.Remove(Key);
		RetryStates.Remove(Key);
		return;
	}

	if (Candidate.IsValid())
	{
		Candidates.Add(Key, MoveTemp(Candidate));
	}
}

bool FDeviceExplorerConnectionCoordinator::Pin(FDeviceExplorerEndpointCandidate Candidate)
{
	if (!Candidate.IsValid())
	{
		return false;
	}

	Candidate.bManual = true;
	const bool bReplaceActive = ActiveCandidate.IsSet()
	                         && (ActiveCandidate->Key() != Candidate.Key() || !(ActiveCandidate->Endpoint == Candidate.Endpoint)
	                             || ActiveCandidate->Token != Candidate.Token);
	RetryStates.Remove(Candidate.Key());
	ManualPin = MoveTemp(Candidate);
	return bReplaceActive;
}

bool FDeviceExplorerConnectionCoordinator::Unpin()
{
	if (!ManualPin.IsSet())
	{
		return false;
	}

	const bool bReplaceActive = ActiveCandidate.IsSet() && ActiveCandidate->Key() == ManualPin->Key();
	if (LastGoodCandidate.IsSet() && LastGoodCandidate->Key() == ManualPin->Key())
	{
		LastGoodCandidate.Reset();
	}
	RetryStates.Remove(ManualPin->Key());
	ManualPin.Reset();
	return bReplaceActive;
}

bool FDeviceExplorerConnectionCoordinator::SelectNext(const double NowSeconds, FDeviceExplorerEndpointCandidate& OutCandidate)
{
	if (ActiveCandidate.IsSet())
	{
		return false;
	}

	if (ManualPin.IsSet())
	{
		if (IsReady(*ManualPin, NowSeconds))
		{
			Select(*ManualPin, OutCandidate);
			return true;
		}
		return false;
	}

	if (LastGoodCandidate.IsSet())
	{
		const FDeviceExplorerEndpointCandidate Candidate = ResolveLatest(*LastGoodCandidate);
		if (IsReady(Candidate, NowSeconds))
		{
			Select(Candidate, OutCandidate);
			return true;
		}
	}

	TArray<FDeviceExplorerEndpointCandidate> OrderedCandidates;
	Candidates.GenerateValueArray(OrderedCandidates);
	OrderedCandidates.Sort(
		[](const FDeviceExplorerEndpointCandidate& Left, const FDeviceExplorerEndpointCandidate& Right)
		{
			if (Left.ProviderId != Right.ProviderId)
			{
				return Left.ProviderId.LexicalLess(Right.ProviderId);
			}
			return Left.CandidateId.Compare(Right.CandidateId, ESearchCase::CaseSensitive) < 0;
		});

	for (const FDeviceExplorerEndpointCandidate& Candidate : OrderedCandidates)
	{
		if (ManualPin.IsSet() && Candidate.Key() == ManualPin->Key())
		{
			continue;
		}
		if (LastGoodCandidate.IsSet() && Candidate.Key() == LastGoodCandidate->Key())
		{
			continue;
		}
		if (IsReady(Candidate, NowSeconds))
		{
			Select(Candidate, OutCandidate);
			return true;
		}
	}
	return false;
}

void FDeviceExplorerConnectionCoordinator::MarkConnected(const FDeviceExplorerEndpointCandidate& Candidate)
{
	if (!IsActive(Candidate))
	{
		return;
	}
	RetryStates.Remove(Candidate.Key());
	LastGoodCandidate = Candidate;
}

void FDeviceExplorerConnectionCoordinator::MarkFailed(const FDeviceExplorerEndpointCandidate& Candidate, const double NowSeconds)
{
	if (!IsActive(Candidate))
	{
		return;
	}

	FRetryState& Retry = RetryStates.FindOrAdd(Candidate.Key());
	const double Delay = FMath::Min(InitialRetryDelaySeconds * FMath::Pow(2.0, Retry.FailureCount), MaxRetryDelaySeconds);
	Retry.FailureCount = FMath::Min(Retry.FailureCount + 1, 30);
	Retry.RetryAtSeconds = NowSeconds + Delay;
	ActiveCandidate.Reset();
}

void FDeviceExplorerConnectionCoordinator::AbandonActive()
{
	ActiveCandidate.Reset();
}

bool FDeviceExplorerConnectionCoordinator::IsActive(const FDeviceExplorerEndpointCandidate& Candidate) const
{
	return ActiveCandidate.IsSet() && ActiveCandidate->Key() == Candidate.Key();
}

FDeviceExplorerEndpointCandidate FDeviceExplorerConnectionCoordinator::ResolveLatest(const FDeviceExplorerEndpointCandidate& Candidate) const
{
	if (const FDeviceExplorerEndpointCandidate* Latest = Candidates.Find(Candidate.Key()))
	{
		return *Latest;
	}
	return Candidate;
}

bool FDeviceExplorerConnectionCoordinator::IsReady(const FDeviceExplorerEndpointCandidate& Candidate, const double NowSeconds) const
{
	const FRetryState* Retry = RetryStates.Find(Candidate.Key());
	return Candidate.IsValid() && (Retry == nullptr || NowSeconds >= Retry->RetryAtSeconds);
}

void FDeviceExplorerConnectionCoordinator::Select(FDeviceExplorerEndpointCandidate Candidate, FDeviceExplorerEndpointCandidate& OutCandidate)
{
	ActiveCandidate = Candidate;
	OutCandidate = MoveTemp(Candidate);
}
