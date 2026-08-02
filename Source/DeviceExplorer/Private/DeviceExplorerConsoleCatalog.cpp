#include "DeviceExplorerConsoleCatalog.h"

#include "ConsoleSettings.h"
#include "Engine/Console.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "Misc/Build.h"
#include "Misc/OutputDevice.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include "ShowFlags.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

#if STATS
#include "Stats/StatsData.h"
#endif

namespace
{
constexpr int32 MaxHelpLength = 4096;

// UEngine::EngineStats is private and NewStatDelegate has already fired by the time a PostEngineInit module loads,
// so the built-in simple stats are probed by name against UEngine::IsEngineStat(). Names mirror the AddEngineStat
// block in UEngine::Init(); anything a build omits is filtered out by the probe, and stats registered later through
// UEngine::AddEngineStat() arrive on NewStatDelegate instead.
const TCHAR* const BuiltinEngineStatNames[] = {
	TEXT("AI"),
	TEXT("ColorList"),
	TEXT("Detailed"),
	TEXT("DrawCount"),
	TEXT("FPS"),
	TEXT("FrameCounter"),
	TEXT("Hitches"),
	TEXT("Levels"),
	TEXT("NamedEvents"),
	TEXT("ParticlePerf"),
	TEXT("Raw"),
	TEXT("Summary"),
	TEXT("TSR"),
	TEXT("Timecode"),
	TEXT("Unit"),
	TEXT("UnitCriticalPath"),
	TEXT("UnitGraph"),
	TEXT("UnitMax"),
	TEXT("UnitTime"),
	TEXT("VerboseNamedEvents"),
	TEXT("Version")
};

int32 CurrentStatGroupCount()
{
#if STATS
	return FStatGroupGameThreadNotifier::Get().StatGroupNames.Num();
#else
	return 0;
#endif
}

class FConsoleNameCollector final : public FOutputDevice
{
public:
	virtual void Serialize(const TCHAR* Message, ELogVerbosity::Type, const FName&) override
	{
		Lines.Emplace(Message);
	}

	TArray<FString> Lines;
};

class FShowFlagCollector
{
public:
	using FSink = TFunctionRef<void(const FString& FlagName, const FString& Command, const FString& Help)>;

	explicit FShowFlagCollector(FSink InSink)
		: Sink(InSink)
	{
	}

	bool OnEngineShowFlag(uint32, const FString& Name)
	{
		Add(Name);
		return true;
	}

	bool OnCustomShowFlag(uint32, const FString& Name)
	{
		Add(Name);
		return true;
	}

private:
	void Add(const FString& Name)
	{
		FText DisplayName;
		const bool bLocalized = FEngineShowFlags::FindShowFlagDisplayName(Name, DisplayName);
		Sink(Name,
		     FString::Printf(TEXT("show %s"), *Name),
		     FString::Printf(TEXT("Toggles the %s show flag in the game viewport."), bLocalized ? *DisplayName.ToString() : *Name));
	}

	FSink Sink;
};
}    // namespace

FDeviceExplorerConsoleCatalog::FDeviceExplorerConsoleCatalog()
{
	ModulesChangedHandle = FModuleManager::Get().OnModulesChanged().AddLambda(
		[this](FName, EModuleChangeReason)
		{
			Invalidate();
		});
	ConsoleObjectUnregisteredHandle = IConsoleManager::Get().OnConsoleObjectUnregistered().AddLambda(
		[this](const TCHAR*, IConsoleObject*)
		{
			Invalidate();
		});
	CustomShowFlagHandle = FEngineShowFlags::OnCustomShowFlagRegistered.AddLambda(
		[this]()
		{
			Invalidate();
		});
	NewEngineStatHandle = UEngine::NewStatDelegate.AddLambda(
		[this](const FName& CommandName, const FName&, const FText& Description)
		{
			FString Name = CommandName.ToString();
			Name.RemoveFromStart(TEXT("STAT_"));
			LateEngineStatDescriptions.Add(MoveTemp(Name), Description.ToString());
			Invalidate();
		});
}

FDeviceExplorerConsoleCatalog::~FDeviceExplorerConsoleCatalog()
{
	UEngine::NewStatDelegate.Remove(NewEngineStatHandle);
	FEngineShowFlags::OnCustomShowFlagRegistered.Remove(CustomShowFlagHandle);
	IConsoleManager::Get().OnConsoleObjectUnregistered().Remove(ConsoleObjectUnregisteredHandle);
	FModuleManager::Get().OnModulesChanged().Remove(ModulesChangedHandle);
}

const TCHAR* FDeviceExplorerConsoleCatalog::SourceToString(const EDeviceExplorerConsoleSource Source)
{
	switch (Source)
	{
	case EDeviceExplorerConsoleSource::Exec:
		return TEXT("exec");
	case EDeviceExplorerConsoleSource::Stat:
		return TEXT("stat");
	case EDeviceExplorerConsoleSource::Show:
		return TEXT("show");
	case EDeviceExplorerConsoleSource::Manual:
		return TEXT("manual");
	default:
		return TEXT("cvar");
	}
}

void FDeviceExplorerConsoleCatalog::Invalidate()
{
	bValid = false;
}

const TArray<FDeviceExplorerConsoleEntry>& FDeviceExplorerConsoleCatalog::Get(const bool bForceRebuild)
{
	if (bForceRebuild || !bValid)
	{
		Rebuild();
	}
	else
	{
		// Stat groups keep registering as each stat emits its first sample, which would otherwise force a full
		// rebuild - UFunction iteration plus an FExec walk - for minutes after launch.
		SyncStatGroups();
	}
	return Entries;
}

FDeviceExplorerConsoleEntry& FDeviceExplorerConsoleCatalog::AddEntry(const FString& Name, const FString& Help, const EDeviceExplorerConsoleSource Source)
{
	const FString LowerName = Name.ToLower();
	if (const int32* ExistingIndex = IndexByLowerName.Find(LowerName))
	{
		// Sources are collected best-documented first, so an existing entry only gains help it was missing.
		FDeviceExplorerConsoleEntry& Existing = Entries[*ExistingIndex];
		if (Existing.Help.IsEmpty() && !Help.IsEmpty())
		{
			Existing.Help = Help;
			Existing.Help.LeftInline(MaxHelpLength);
		}
		return Existing;
	}

	IndexByLowerName.Add(LowerName, Entries.Num());
	FDeviceExplorerConsoleEntry& Entry = Entries.AddDefaulted_GetRef();
	Entry.Name = Name;
	Entry.LowerName = LowerName;
	Entry.Help = Help;
	Entry.Help.LeftInline(MaxHelpLength);
	Entry.Source = Source;
	return Entry;
}

void FDeviceExplorerConsoleCatalog::SortAndReindex()
{
	Entries.Sort(
		[](const FDeviceExplorerConsoleEntry& Left, const FDeviceExplorerConsoleEntry& Right)
		{
			return Left.LowerName < Right.LowerName;
		});

	IndexByLowerName.Reset();
	IndexByLowerName.Reserve(Entries.Num());
	for (int32 Index = 0; Index < Entries.Num(); ++Index)
	{
		IndexByLowerName.Add(Entries[Index].LowerName, Index);
	}
}

void FDeviceExplorerConsoleCatalog::SyncStatGroups()
{
#if STATS
	const int32 GroupCount = CurrentStatGroupCount();
	if (GroupCount == CachedStatGroupCount)
	{
		return;
	}
	CachedStatGroupCount = GroupCount;

	const int32 PreviousNum = Entries.Num();
	for (const FName& StatGroupName : FStatGroupGameThreadNotifier::Get().StatGroupNames)
	{
		FString GroupName = StatGroupName.ToString();
		GroupName.RemoveFromStart(TEXT("STATGROUP_"));
		AddEntry(FString::Printf(TEXT("stat %s"), *GroupName),
		         FString::Printf(TEXT("Toggles the %s stat group overlay."), *GroupName),
		         EDeviceExplorerConsoleSource::Stat);
	}
	if (Entries.Num() != PreviousNum)
	{
		SortAndReindex();
	}
#endif
}

void FDeviceExplorerConsoleCatalog::Rebuild()
{
	Entries.Reset();
	IndexByLowerName.Reset();
	IndexByLowerName.Reserve(16384);
	CachedStatGroupCount = CurrentStatGroupCount();
	bValid = true;

	IConsoleManager::Get().ForEachConsoleObjectThatStartsWith(
		FConsoleObjectVisitor::CreateLambda(
			[this](const TCHAR* Name, IConsoleObject* Object)
			{
				if (Object == nullptr || Object->TestFlags(ECVF_Unregistered))
				{
					return;
				}
				FDeviceExplorerConsoleEntry& Entry = AddEntry(Name, Object->GetHelp(), EDeviceExplorerConsoleSource::CVar);
				Entry.bIsVariable = Object->AsVariable() != nullptr;
				Entry.bReadOnly = Object->TestFlags(ECVF_ReadOnly);
				Entry.bCheat = Object->TestFlags(ECVF_Cheat);
			}),
		TEXT(""));

#if STATS
	for (const FName& StatGroupName : FStatGroupGameThreadNotifier::Get().StatGroupNames)
	{
		FString GroupName = StatGroupName.ToString();
		GroupName.RemoveFromStart(TEXT("STATGROUP_"));
		AddEntry(FString::Printf(TEXT("stat %s"), *GroupName),
		         FString::Printf(TEXT("Toggles the %s stat group overlay."), *GroupName),
		         EDeviceExplorerConsoleSource::Stat);
	}
#endif

	if (GEngine != nullptr)
	{
		for (const TCHAR* StatName : BuiltinEngineStatNames)
		{
			if (GEngine->IsEngineStat(StatName))
			{
				AddEntry(FString::Printf(TEXT("stat %s"), StatName), FString(), EDeviceExplorerConsoleSource::Stat);
			}
		}
	}
	for (const TPair<FString, FString>& LateStat : LateEngineStatDescriptions)
	{
		AddEntry(FString::Printf(TEXT("stat %s"), *LateStat.Key), LateStat.Value, EDeviceExplorerConsoleSource::Stat);
	}

	{
		// The sink has to outlive the collector: FShowFlagCollector only holds a TFunctionRef to it.
		auto ShowFlagSink = [this](const FString& FlagName, const FString& Command, const FString& Help)
		{
			FDeviceExplorerConsoleEntry& Entry = AddEntry(Command, Help, EDeviceExplorerConsoleSource::Show);
			// Every show flag also has a ShowFlag.X override variable (FSystemSettings::RegisterShowFlagConsoleVariables),
			// which is what lets the dashboard read and force a state instead of only toggling.
			const FString CompanionName = FString::Printf(TEXT("ShowFlag.%s"), *FlagName);
			if (IConsoleManager::Get().FindConsoleVariable(*CompanionName, false) != nullptr)
			{
				Entry.Companion = CompanionName;
			}
		};
		FShowFlagCollector Collector(ShowFlagSink);
		FEngineShowFlags::IterateAllFlags(Collector);
	}

	// Exec functions are UFunctions, not console objects. Filter mirrors UConsole::BuildRuntimeAutoCompleteList.
	for (TObjectIterator<UFunction> FunctionIt; FunctionIt; ++FunctionIt)
	{
		UFunction* Function = *FunctionIt;
		const UClass* OwnerClass = Cast<UClass>(Function->GetOuter());
		if (!Function->HasAnyFunctionFlags(FUNC_Exec) || (Function->GetSuperFunction() != nullptr && OwnerClass == nullptr))
		{
			continue;
		}

		TArray<FString> Parameters;
		for (TFieldIterator<FProperty> ParamIt(Function); ParamIt && (ParamIt->PropertyFlags & CPF_Parm); ++ParamIt)
		{
			if ((ParamIt->PropertyFlags & CPF_ReturnParm) == 0)
			{
				Parameters.Add(FString::Printf(TEXT("<%s: %s>"), *ParamIt->GetName(), *ParamIt->GetCPPType()));
			}
		}

		FString Help = OwnerClass != nullptr
			? FString::Printf(TEXT("Exec function on %s."), *OwnerClass->GetName())
			: FString(TEXT("Exec function."));
#if WITH_EDITOR
		const FString ToolTip = Function->GetToolTipText().ToString();
		if (!ToolTip.IsEmpty())
		{
			Help = ToolTip + TEXT(" ") + Help;
		}
#endif
		FDeviceExplorerConsoleEntry& Entry = AddEntry(Function->GetName(), Help, EDeviceExplorerConsoleSource::Exec);
		if (Entry.Arguments.IsEmpty())
		{
			Entry.Arguments = FString::Join(Parameters, TEXT(" "));
		}
	}

	{
		TArray<FAutoCompleteCommand> ManualEntries = GetDefault<UConsoleSettings>()->ManualAutoCompleteList;
		// Plugins publish their own console entries here; UConsole broadcasts the same delegate to build autocomplete.
		UConsole::RegisterConsoleAutoCompleteEntries.Broadcast(ManualEntries);
		for (const FAutoCompleteCommand& Command : ManualEntries)
		{
			if (!Command.Command.IsEmpty() && !Command.IsHistory())
			{
				AddEntry(Command.Command.TrimStartAndEnd(), Command.Desc, EDeviceExplorerConsoleSource::Manual);
			}
		}
	}

#if !UE_BUILD_SHIPPING
	// Commands owned by the FExec chain (stat, show, open, travel, ...) exist only as literals inside FParse::Command
	// calls. This is how DumpConsoleCommands finds them: a global FParse hook plus a chain walk with a pattern that
	// matches nothing, so names are reported but no handler runs.
	if (GEngine != nullptr)
	{
		FConsoleNameCollector Collector;
		ConsoleCommandLibrary_DumpLibrary(GWorld, *GEngine, TEXT(""), Collector);
		for (const FString& Line : Collector.Lines)
		{
			const FString Name = Line.TrimStartAndEnd();
			if (Name.IsEmpty() || Name.Contains(TEXT(" ")) || Name.StartsWith(TEXT("ERROR:")) || Name.StartsWith(TEXT("However")))
			{
				continue;
			}
			AddEntry(Name, FString(), EDeviceExplorerConsoleSource::Exec);
		}
	}
#endif

	SortAndReindex();
}
