#include "DeviceExplorerCommandCapture.h"

#include "HAL/PlatformTLS.h"
#include "Logging/LogVerbosity.h"
#include "Misc/OutputDeviceRedirector.h"

FDeviceExplorerCommandCapture::FDeviceExplorerCommandCapture()
	: OwnerThreadId(FPlatformTLS::GetCurrentThreadId())
{
	if (GLog != nullptr)
	{
		// Background threads buffer their lines until the primary thread drains them. Draining first keeps a backlog
		// that predates the command out of its output.
		GLog->Flush();
		GLog->AddOutputDevice(this);
	}
}

FDeviceExplorerCommandCapture::~FDeviceExplorerCommandCapture()
{
	if (GLog != nullptr)
	{
		GLog->RemoveOutputDevice(this);
	}
}

void FDeviceExplorerCommandCapture::Serialize(const TCHAR* Message, const ELogVerbosity::Type Verbosity, const FName& Category)
{
	if (FPlatformTLS::GetCurrentThreadId() != OwnerThreadId || Category == TEXT("LogDeviceExplorer"))
	{
		return;
	}
	if (Lines.Num() >= MaxCapturedLines || CapturedChars >= MaxCapturedChars)
	{
		bTruncated = true;
		return;
	}

	FString Line;
	if (Verbosity == ELogVerbosity::Error || Verbosity == ELogVerbosity::Warning)
	{
		Line = FString::Printf(TEXT("%s: %s"), ToString(Verbosity), Message);
	}
	else
	{
		Line = Message;
	}

	CapturedChars += Line.Len();
	Lines.Add(MoveTemp(Line));
}

FString FDeviceExplorerCommandCapture::BuildOutput(const FString& DirectOutput) const
{
	TSet<FString> AlreadyReported;
	TArray<FString> DirectLines;
	DirectOutput.ParseIntoArrayLines(DirectLines);
	for (const FString& DirectLine : DirectLines)
	{
		AlreadyReported.Add(DirectLine.TrimStartAndEnd());
	}

	TArray<FString> Result;
	Result.Reserve(Lines.Num());
	for (const FString& Line : Lines)
	{
		if (!AlreadyReported.Contains(Line.TrimStartAndEnd()))
		{
			Result.Add(Line);
		}
	}
	if (bTruncated)
	{
		Result.Add(TEXT("[output truncated]"));
	}
	return FString::Join(Result, TEXT("\n"));
}
