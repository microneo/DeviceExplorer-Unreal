#pragma once

#include "Containers/Queue.h"
#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

#include <atomic>

struct FDeviceExplorerQueuedLog
{
	FDateTime Timestamp;
	FName Category;
	ELogVerbosity::Type Verbosity = ELogVerbosity::Log;
	FString Message;
};

class FDeviceExplorerOutputDevice final : public FOutputDevice
{
public:
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;

	bool Dequeue(FDeviceExplorerQueuedLog& Entry);
	uint64 ConsumeDroppedCount();

private:
	static constexpr int32 MaxPendingLogs = 10'000;

	TQueue<FDeviceExplorerQueuedLog, EQueueMode::Mpsc> Queue;
	std::atomic<int32> PendingCount{ 0 };
	std::atomic<uint64> DroppedCount{ 0 };
};
