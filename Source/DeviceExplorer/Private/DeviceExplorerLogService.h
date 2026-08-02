#pragma once

#include "CoreMinimal.h"

class FDeviceExplorerOutputDevice;
class FJsonObject;

/**
 * Device side of the dashboard's Logs module: captures engine output, batches it into
 * protocol messages, and serves the category/verbosity table behind the Verbosity tab.
 */
class FDeviceExplorerLogService final
{
public:
	using FSendJson = TFunction<void(const TSharedRef<FJsonObject>&)>;

	explicit FDeviceExplorerLogService(FSendJson InSendJson);
	~FDeviceExplorerLogService();

	void Start();
	void Stop();

	/** Drains captured output into a single log_batch message. */
	void Flush();

	/** Returns false when Type is not one of the messages this service owns. */
	bool HandleMessage(const FString& Type, const TSharedPtr<FJsonObject>& Message);

	/** Restores every level this session changed while auto-revert was armed. */
	void RevertOverrides();

private:
	TArray<TSharedPtr<class FJsonValue>> BuildCategoryList(const TMap<FString, FString>& Categories);
	void SendCategories(const FString& RequestId);
	void ApplyVerbosity(const FString& RequestId, const TSharedPtr<FJsonObject>& Message);

	FSendJson SendJson;
	TUniquePtr<FDeviceExplorerOutputDevice> OutputDevice;
	/**
	 * Level a category had the first time this session saw it. Categories register as their
	 * owning module loads, so the baseline is filled in lazily rather than snapshotted once.
	 */
	TMap<FString, FString> Baseline;
	/** Categories this session changed, so the dashboard can separate them from boot levels. */
	TSet<FString> Overrides;
	bool bAutoRevert = true;
	bool bStarted = false;
};
