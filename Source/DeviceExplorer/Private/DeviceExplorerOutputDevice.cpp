#include "DeviceExplorerOutputDevice.h"

void FDeviceExplorerOutputDevice::Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category)
{
	if (Category == TEXT("LogDeviceExplorer"))
	{
		return;
	}

	int32 Expected = PendingCount.load(std::memory_order_relaxed);
	while (Expected < MaxPendingLogs)
	{
		if (PendingCount.compare_exchange_weak(Expected, Expected + 1, std::memory_order_acquire, std::memory_order_relaxed))
		{
			FDeviceExplorerQueuedLog Entry;
			Entry.Timestamp = FDateTime::UtcNow();
			Entry.Category = Category;
			Entry.Verbosity = Verbosity;
			Entry.Message = Message;
			Queue.Enqueue(MoveTemp(Entry));
			return;
		}
	}

	DroppedCount.fetch_add(1, std::memory_order_relaxed);
}

bool FDeviceExplorerOutputDevice::Dequeue(FDeviceExplorerQueuedLog& Entry)
{
	if (!Queue.Dequeue(Entry))
	{
		return false;
	}
	PendingCount.fetch_sub(1, std::memory_order_release);
	return true;
}

uint64 FDeviceExplorerOutputDevice::ConsumeDroppedCount()
{
	return DroppedCount.exchange(0, std::memory_order_acq_rel);
}
