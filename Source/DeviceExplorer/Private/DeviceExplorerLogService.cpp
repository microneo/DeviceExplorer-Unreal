#include "DeviceExplorerLogService.h"

#include "Async/Async.h"
#include "DeviceExplorerOutputDevice.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/World.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreMisc.h"
#include "Misc/OutputDevice.h"
#include "Misc/StringOutputDevice.h"

namespace
{
constexpr int32 MaxLogsPerBatch = 256;

const TCHAR* const VerbosityNames[] = {
	TEXT("Fatal"), TEXT("Error"), TEXT("Warning"), TEXT("Display"), TEXT("Log"), TEXT("Verbose"), TEXT("VeryVerbose")
};

FString VerbosityToString(const ELogVerbosity::Type Verbosity)
{
	switch (Verbosity)
	{
		case ELogVerbosity::Fatal: return TEXT("Fatal");
		case ELogVerbosity::Error: return TEXT("Error");
		case ELogVerbosity::Warning: return TEXT("Warning");
		case ELogVerbosity::Display: return TEXT("Display");
		case ELogVerbosity::Log: return TEXT("Log");
		case ELogVerbosity::Verbose: return TEXT("Verbose");
		case ELogVerbosity::VeryVerbose: return TEXT("VeryVerbose");
		default: return TEXT("Log");
	}
}

/** Returns the canonical spelling, or an empty string when Value is not a verbosity the Log command accepts. */
FString NormalizeVerbosity(const FString& Value)
{
	for (const TCHAR* Name : VerbosityNames)
	{
		if (Value.Equals(Name, ESearchCase::IgnoreCase))
		{
			return Name;
		}
	}
	return FString();
}

/**
 * `Log` is an FSelfRegisteringExec handler rather than a console object, so it has to be
 * routed through the exec chain instead of IConsoleManager. Game thread only.
 */
FString RunLogCommand(const FString& Command)
{
	FStringOutputDevice Output;
	// FStringOutputDevice concatenates without separators by default, which would collapse the
	// one-category-per-line listing into a single unparseable line.
	Output.SetAutoEmitLineTerminator(true);
	FSelfRegisteringExec::StaticExec(GWorld, *Command, Output);
	return Output;
}

/** `Log list` prints `%-40s  %-12s  %s`, where the last column is an optional " - DebugBreak" flag. */
TMap<FString, FString> ReadCategories()
{
	TArray<FString> Lines;
	RunLogCommand(TEXT("Log list")).ParseIntoArrayLines(Lines);

	TMap<FString, FString> Categories;
	Categories.Reserve(Lines.Num());
	for (const FString& Line : Lines)
	{
		TArray<FString> Parts;
		Line.ParseIntoArrayWS(Parts);
		if (Parts.Num() < 2)
		{
			continue;
		}
		const FString Verbosity = NormalizeVerbosity(Parts[1]);
		if (!Verbosity.IsEmpty())
		{
			Categories.Add(Parts[0], Verbosity);
		}
	}
	return Categories;
}
}    // namespace

FDeviceExplorerLogService::FDeviceExplorerLogService(FSendJson InSendJson)
	: SendJson(MoveTemp(InSendJson))
{
}

FDeviceExplorerLogService::~FDeviceExplorerLogService()
{
	Stop();
}

void FDeviceExplorerLogService::Start()
{
	if (bStarted)
	{
		return;
	}

	OutputDevice = MakeUnique<FDeviceExplorerOutputDevice>();
	if (GLog != nullptr)
	{
		GLog->AddOutputDevice(OutputDevice.Get());
	}
	bStarted = true;
}

void FDeviceExplorerLogService::Stop()
{
	if (!bStarted)
	{
		return;
	}

	RevertOverrides();
	if (GLog != nullptr && OutputDevice)
	{
		GLog->RemoveOutputDevice(OutputDevice.Get());
	}
	OutputDevice.Reset();
	bStarted = false;
}

void FDeviceExplorerLogService::Flush()
{
	if (!OutputDevice || !SendJson)
	{
		return;
	}

	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Reserve(MaxLogsPerBatch);
	FDeviceExplorerQueuedLog Entry;
	while (Entries.Num() < MaxLogsPerBatch && OutputDevice->Dequeue(Entry))
	{
		TSharedRef<FJsonObject> JsonEntry = MakeShared<FJsonObject>();
		JsonEntry->SetStringField(TEXT("timestamp"), Entry.Timestamp.ToIso8601());
		JsonEntry->SetStringField(TEXT("category"), Entry.Category.ToString());
		JsonEntry->SetStringField(TEXT("verbosity"), VerbosityToString(Entry.Verbosity));
		JsonEntry->SetStringField(TEXT("message"), MoveTemp(Entry.Message));
		Entries.Add(MakeShared<FJsonValueObject>(JsonEntry));
	}

	const uint64 Dropped = OutputDevice->ConsumeDroppedCount();
	if (Entries.IsEmpty() && Dropped == 0)
	{
		return;
	}

	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("log_batch"));
	Message->SetArrayField(TEXT("entries"), MoveTemp(Entries));
	Message->SetNumberField(TEXT("dropped"), static_cast<double>(Dropped));
	SendJson(Message);
}

bool FDeviceExplorerLogService::HandleMessage(const FString& Type, const TSharedPtr<FJsonObject>& Message)
{
	FString RequestId;
	if (!Message.IsValid() || !Message->TryGetStringField(TEXT("request_id"), RequestId))
	{
		return Type == TEXT("list_log_categories") || Type == TEXT("set_log_verbosity");
	}

	if (Type == TEXT("list_log_categories"))
	{
		AsyncTask(ENamedThreads::GameThread, [this, RequestId = MoveTemp(RequestId)]() { SendCategories(RequestId); });
		return true;
	}
	if (Type == TEXT("set_log_verbosity"))
	{
		AsyncTask(ENamedThreads::GameThread, [this, RequestId = MoveTemp(RequestId), Message]() { ApplyVerbosity(RequestId, Message); });
		return true;
	}
	return false;
}

TArray<TSharedPtr<FJsonValue>> FDeviceExplorerLogService::BuildCategoryList(const TMap<FString, FString>& Categories)
{
	TArray<TSharedPtr<FJsonValue>> Entries;
	Entries.Reserve(Categories.Num());
	for (const TPair<FString, FString>& Category : Categories)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Category.Key);
		Json->SetStringField(TEXT("verbosity"), Category.Value);
		Json->SetStringField(TEXT("baseline"), Baseline.FindOrAdd(Category.Key, Category.Value));
		Json->SetStringField(TEXT("source"), Overrides.Contains(Category.Key) ? TEXT("runtime") : TEXT("boot"));
		Entries.Add(MakeShared<FJsonValueObject>(Json));
	}
	return Entries;
}

void FDeviceExplorerLogService::SendCategories(const FString& RequestId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), TEXT("log_categories"));
	Result->SetStringField(TEXT("request_id"), RequestId);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("auto_revert"), bAutoRevert);
	Result->SetArrayField(TEXT("categories"), BuildCategoryList(ReadCategories()));
	SendJson(Result);
}

void FDeviceExplorerLogService::ApplyVerbosity(const FString& RequestId, const TSharedPtr<FJsonObject>& Message)
{
	struct FRequestedLevel
	{
		FString Category;
		FString Verbosity;
		FString Error;
	};

	const TArray<TSharedPtr<FJsonValue>> Empty;
	const TArray<TSharedPtr<FJsonValue>>* Requested = &Empty;
	Message->TryGetArrayField(TEXT("entries"), Requested);
	Message->TryGetBoolField(TEXT("auto_revert"), bAutoRevert);
	bool bPersist = false;
	Message->TryGetBoolField(TEXT("persist"), bPersist);

	const TMap<FString, FString> Before = ReadCategories();
	TArray<FRequestedLevel> Levels;
	Levels.Reserve(Requested->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Requested)
	{
		const TSharedPtr<FJsonObject> Object = Value.IsValid() ? Value->AsObject() : nullptr;
		if (!Object.IsValid())
		{
			continue;
		}

		FRequestedLevel Level;
		Object->TryGetStringField(TEXT("category"), Level.Category);
		Object->TryGetStringField(TEXT("verbosity"), Level.Verbosity);
		Level.Verbosity = NormalizeVerbosity(Level.Verbosity);
		if (Level.Category.IsEmpty() || Level.Verbosity.IsEmpty())
		{
			Level.Error = TEXT("Unsupported verbosity level");
		}
		else if (!Before.Contains(Level.Category))
		{
			Level.Error = TEXT("This build has no such log category");
		}
		else
		{
			RunLogCommand(FString::Printf(TEXT("Log %s %s"), *Level.Category, *Level.Verbosity));
		}
		Levels.Add(MoveTemp(Level));
	}

	// SetVerbosity() silently clamps to the category's compile-time ceiling, so the level that
	// actually took effect can only be learned by reading the table back.
	const TMap<FString, FString> After = ReadCategories();
	TArray<TSharedPtr<FJsonValue>> Results;
	Results.Reserve(Levels.Num());
	for (const FRequestedLevel& Level : Levels)
	{
		TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
		Entry->SetStringField(TEXT("category"), Level.Category);
		Entry->SetStringField(TEXT("requested"), Level.Verbosity);
		Results.Add(MakeShared<FJsonValueObject>(Entry));
		if (!Level.Error.IsEmpty())
		{
			Entry->SetBoolField(TEXT("success"), false);
			Entry->SetStringField(TEXT("error"), Level.Error);
			continue;
		}

		const FString Applied = After.FindRef(Level.Category);
		Entry->SetStringField(TEXT("applied"), Applied);
		Entry->SetBoolField(TEXT("success"), Applied == Level.Verbosity);
		if (Applied != Level.Verbosity)
		{
			Entry->SetStringField(TEXT("error"), FString::Printf(TEXT("%s is compiled out in this build"), *Level.Verbosity));
		}

		if (Applied == Baseline.FindOrAdd(Level.Category, Before.FindRef(Level.Category)))
		{
			Overrides.Remove(Level.Category);
		}
		else
		{
			Overrides.Add(Level.Category);
		}
		if (bPersist)
		{
			GConfig->SetString(TEXT("Core.Log"), *Level.Category, *Applied, GEngineIni);
		}
	}
	if (bPersist)
	{
		GConfig->Flush(false, GEngineIni);
	}

	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), TEXT("log_verbosity_result"));
	Result->SetStringField(TEXT("request_id"), RequestId);
	Result->SetBoolField(TEXT("success"), true);
	Result->SetBoolField(TEXT("auto_revert"), bAutoRevert);
	Result->SetArrayField(TEXT("results"), MoveTemp(Results));
	Result->SetArrayField(TEXT("categories"), BuildCategoryList(After));
	SendJson(Result);
}

void FDeviceExplorerLogService::RevertOverrides()
{
	if (!bAutoRevert || Overrides.IsEmpty())
	{
		Overrides.Reset();
		return;
	}

	TArray<FString> Commands;
	Commands.Reserve(Overrides.Num());
	for (const FString& Category : Overrides)
	{
		if (const FString* Level = Baseline.Find(Category))
		{
			Commands.Add(FString::Printf(TEXT("%s %s"), *Category, **Level));
		}
	}
	Overrides.Reset();
	if (!Commands.IsEmpty())
	{
		RunLogCommand(TEXT("Log ") + FString::Join(Commands, TEXT(", ")));
	}
}
