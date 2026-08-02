#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"

/**
 * Collects UE_LOG output produced while a console command runs. Only three of the seven IConsoleCommand delegate
 * flavours forward the FOutputDevice to their handler and exec functions never receive one, so most commands answer
 * through the log instead. Lines from other threads are dropped - the command runs synchronously on the constructing
 * thread, so anything arriving from elsewhere is unrelated work.
 */
class FDeviceExplorerCommandCapture final : public FOutputDevice
{
public:
	FDeviceExplorerCommandCapture();
	virtual ~FDeviceExplorerCommandCapture() override;

	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type Verbosity, const FName& Category) override;

	/** Joins the captured lines, dropping any that the command already wrote to its own output device. */
	FString BuildOutput(const FString& DirectOutput) const;

private:
	static constexpr int32 MaxCapturedLines = 512;
	static constexpr int32 MaxCapturedChars = 64 * 1024;

	TArray<FString> Lines;
	uint32 OwnerThreadId = 0;
	int32 CapturedChars = 0;
	bool bTruncated = false;
};
