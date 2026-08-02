#pragma once

#include "CoreMinimal.h"

enum class EDeviceExplorerConsoleSource : uint8
{
	CVar,
	Exec,
	Stat,
	Show,
	Manual
};

struct FDeviceExplorerConsoleEntry
{
	FString Name;
	/** Cached so a query can use case-sensitive comparisons against an already lowered needle. */
	FString LowerName;
	FString Help;
	/** Parameter signature for exec functions, empty when the source does not publish one. */
	FString Arguments;
	/** Console variable that carries this entry's state, currently the ShowFlag.X override behind a show flag. */
	FString Companion;
	EDeviceExplorerConsoleSource Source = EDeviceExplorerConsoleSource::CVar;
	bool bIsVariable = false;
	bool bReadOnly = false;
	bool bCheat = false;
};

/**
 * Snapshot of everything this build can execute from the console: IConsoleManager objects, exec functions, stat
 * commands and show flags all live in separate registries.
 *
 * Names are cached because only IConsoleManager is cheap to enumerate; exec functions need a UFunction pass over
 * every loaded class and the FExec chain has to be probed. Variable values are not cached - they are re-read per
 * query for the entries that survive the filter.
 */
class FDeviceExplorerConsoleCatalog
{
public:
	FDeviceExplorerConsoleCatalog();
	~FDeviceExplorerConsoleCatalog();

	/** Game thread only. Rebuilds on first use and after Invalidate(). */
	const TArray<FDeviceExplorerConsoleEntry>& Get(bool bForceRebuild);

	void Invalidate();

	static const TCHAR* SourceToString(EDeviceExplorerConsoleSource Source);

private:
	void Rebuild();
	/** Appends stat groups that registered since the last pass. Cheap enough to run on every query. */
	void SyncStatGroups();
	FDeviceExplorerConsoleEntry& AddEntry(const FString& Name, const FString& Help, EDeviceExplorerConsoleSource Source);
	void SortAndReindex();

	TArray<FDeviceExplorerConsoleEntry> Entries;
	TMap<FString, int32> IndexByLowerName;
	TMap<FString, FString> LateEngineStatDescriptions;
	FDelegateHandle ModulesChangedHandle;
	FDelegateHandle ConsoleObjectUnregisteredHandle;
	FDelegateHandle CustomShowFlagHandle;
	FDelegateHandle NewEngineStatHandle;
	int32 CachedStatGroupCount = 0;
	bool bValid = false;
};
