#include "DeviceExplorerModule.h"

#include "Async/Async.h"
#include "Containers/StringConv.h"
#include "Containers/Ticker.h"
#include "DeviceExplorerAuth.h"
#include "DeviceExplorerCommandCapture.h"
#include "DeviceExplorerConnectionCoordinator.h"
#include "DeviceExplorerConsoleCatalog.h"
#include "DeviceExplorerCoreModule.h"
#include "DeviceExplorerDiscovery.h"
#include "DeviceExplorerDeviceIdentity.h"
#include "DeviceExplorerLogService.h"
#include "DeviceExplorerSettings.h"
#include "DeviceExplorerTrace.h"
#include "DeviceExplorerTransport.h"
#include "DeviceExplorerTypes.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineGlobals.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformMemory.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/App.h"
#include "Misc/Build.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Crc.h"
#include "Misc/EngineVersion.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorer, Log, All);

FDeviceExplorerModule::~FDeviceExplorerModule() = default;

namespace
{
constexpr int64 ArchiveStreamChunkSize = 1 << 20;    // keeps memory flat regardless of source file size
const FName RuntimeOwner(TEXT("DeviceExplorerRuntime"));

double GetAppUptimeSeconds()
{
	return FPlatformTime::Seconds() - GStartTime;
}

// Lower ranks sort first. LowerQuery is compared case-sensitively against the entry's cached lowercase name: an
// IgnoreCase scan over the whole catalog on every keystroke is what makes the search feel slow.
// CVar help is long and prose-like, so matching it by default would bury the name matches.
int32 RankConsoleMatch(const FDeviceExplorerConsoleEntry& Entry, const FString& LowerQuery, const bool bSearchHelp)
{
	if (LowerQuery.IsEmpty())
	{
		return 3;
	}
	if (Entry.LowerName.Equals(LowerQuery, ESearchCase::CaseSensitive))
	{
		return 0;
	}
	if (Entry.LowerName.StartsWith(LowerQuery, ESearchCase::CaseSensitive))
	{
		return 1;
	}
	if (Entry.LowerName.Contains(LowerQuery, ESearchCase::CaseSensitive))
	{
		return 2;
	}
	if (bSearchHelp && Entry.Help.Contains(LowerQuery, ESearchCase::IgnoreCase))
	{
		return 3;
	}
	return INDEX_NONE;
}

FString NormalizeSavedRelativePath(const FString& RawPath)
{
	FString Path = RawPath;
	Path.ReplaceInline(TEXT("\\"), TEXT("/"));
	while (Path.StartsWith(TEXT("/")))
	{
		Path.RightChopInline(1, EAllowShrinking::No);
	}

	TArray<FString> Segments;
	Path.ParseIntoArray(Segments, TEXT("/"), true);
	TArray<FString> SafeSegments;
	for (const FString& Segment : Segments)
	{
		if (Segment == TEXT("."))
		{
			continue;
		}
		if (Segment == TEXT("..") || Segment.Contains(TEXT(":")))
		{
			return FString();
		}
		SafeSegments.Add(Segment);
	}
	return FString::Join(SafeSegments, TEXT("/"));
}

bool ResolveRegisteredPath(const FName RootName, const FString& RelativePath, const bool bForDownload, FString& OutFullPath)
{
	const FString NormalizedRelative = NormalizeSavedRelativePath(RelativePath);
	if (!RelativePath.IsEmpty() && NormalizedRelative.IsEmpty())
	{
		return false;
	}

	FDeviceExplorerFileRootDescriptor Root;
	if (!FDeviceExplorerCoreModule::Get().FindFileRoot(RootName, Root) || (bForDownload && !Root.bAllowDownload))
	{
		return false;
	}

	FString RootPath = FPaths::ConvertRelativePathToFull(Root.AbsolutePath);
	FPaths::NormalizeDirectoryName(RootPath);
	OutFullPath = FPaths::ConvertRelativePathToFull(FPaths::Combine(RootPath, NormalizedRelative));
	FPaths::NormalizeFilename(OutFullPath);
	return OutFullPath == RootPath || OutFullPath.StartsWith(RootPath + TEXT("/"), ESearchCase::CaseSensitive);
}

// Hand-rolled store-only ZIP64 writer: FileUtilities/ZipArchiveWriter.h only links libzip for editor targets
// (see FileUtilities.Build.cs), so device builds (iOS, Android) can't depend on that module. Binary layout mirrors
// FZipArchiveWriter (always-ZIP64 extra fields, no compression, no data descriptor).
class FDeviceExplorerZipWriter
{
public:
	explicit FDeviceExplorerZipWriter(IFileHandle* InFile)
		: File(InFile)
	{
	}

	~FDeviceExplorerZipWriter()
	{
		const uint64 DirStartOffset = File->Tell();
		for (const FEntry& Entry : Entries)
		{
			WriteCentralDirectoryHeader(Entry);
		}
		const uint64 EndOffsetCD = File->Tell();

		FZip64EndOfCDRecord Zip64EndOfCDRecord;
		Zip64EndOfCDRecord.CDRecords = Entries.Num();
		Zip64EndOfCDRecord.CDTotalRecords = Entries.Num();
		Zip64EndOfCDRecord.CDSize = EndOffsetCD - DirStartOffset;
		Zip64EndOfCDRecord.CDStartOffset = DirStartOffset;
		WriteRaw(&Zip64EndOfCDRecord, sizeof(Zip64EndOfCDRecord));

		FZip64EndOfCDLocator Zip64EndOfCDLocator;
		Zip64EndOfCDLocator.EndOffsetCD = EndOffsetCD;
		WriteRaw(&Zip64EndOfCDLocator, sizeof(Zip64EndOfCDLocator));

		FEndOfCDRecord EndOfCDRecord;
		WriteRaw(&EndOfCDRecord, sizeof(EndOfCDRecord));

		delete File;
	}

	// Reads SourceFilePath in fixed-size chunks (CRC pass, then a rewound copy pass) instead of loading it
	// whole into memory, so a single multi-gigabyte capture file can't spike memory or stall the caller.
	bool AddFile(const FString& Filename, const FString& SourceFilePath, const FDateTime& Timestamp)
	{
		TUniquePtr<IFileHandle> SourceHandle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*SourceFilePath));
		if (!SourceHandle)
		{
			return false;
		}
		const int64 FileSize = SourceHandle->Size();

		TArray<uint8> Buffer;
		Buffer.SetNumUninitialized(ArchiveStreamChunkSize);
		uint32 Crc = 0;
		for (int64 Remaining = FileSize; Remaining > 0;)
		{
			const int64 ReadSize = FMath::Min<int64>(Remaining, ArchiveStreamChunkSize);
			if (!SourceHandle->Read(Buffer.GetData(), ReadSize))
			{
				return false;
			}
			Crc = FCrc::MemCrc32(Buffer.GetData(), ReadSize, Crc);
			Remaining -= ReadSize;
		}
		if (!SourceHandle->Seek(0))
		{
			return false;
		}

		const FTCHARToUTF8 Utf8Filename(*Filename);
		const uint32 ZipTime =
			(Timestamp.GetSecond() / 2) |
			(Timestamp.GetMinute() << 5) |
			(Timestamp.GetHour() << 11) |
			(Timestamp.GetDay() << 16) |
			(Timestamp.GetMonth() << 21) |
			((Timestamp.GetYear() - 1980) << 25);

		FEntry& Entry = Entries.Emplace_GetRef();
		Entry.Filename = Filename;
		Entry.Crc32 = Crc;
		Entry.Offset = File->Tell();
		Entry.Time = ZipTime;
		Entry.UnCompressedSize = FileSize;

		FZipLocalHeader LocalHeader;
		LocalHeader.TimeDate = ZipTime;
		LocalHeader.Crc = Crc;
		LocalHeader.FileNameLength = (uint16) Utf8Filename.Length();
		WriteRaw(&LocalHeader, sizeof(LocalHeader));
		WriteRaw(Utf8Filename.Get(), Utf8Filename.Length());

		FZip64Extra Zip64Extra;
		Zip64Extra.UnCompressedSize = FileSize;
		Zip64Extra.CompressedSize = FileSize;
		WriteRaw(&Zip64Extra, sizeof(Zip64Extra));

		for (int64 Remaining = FileSize; Remaining > 0;)
		{
			const int64 ReadSize = FMath::Min<int64>(Remaining, ArchiveStreamChunkSize);
			if (!SourceHandle->Read(Buffer.GetData(), ReadSize))
			{
				return false;
			}
			WriteRaw(Buffer.GetData(), ReadSize);
			Remaining -= ReadSize;
		}
		return true;
	}

private:
#pragma pack(push, 1)
	struct FZipLocalHeader
	{
		uint8 Sig[4] = { 0x50, 0x4b, 0x03, 0x04 };
		uint16 Version = 45;
		uint16 GenPurposeBit = 1 << 11;
		uint16 CompMode = 0;
		uint32 TimeDate;
		uint32 Crc;
		uint32 CompressedSize = 0xFFFFFFFF;
		uint32 UnCompressedSize = 0xFFFFFFFF;
		uint16 FileNameLength;
		uint16 ExtraFieldLength = 20;
	};

	struct FZip64Extra
	{
		uint16 Id = 0x0001;
		uint16 Length = 16;
		uint64 UnCompressedSize;
		uint64 CompressedSize;
	};

	struct FZipCDHeader
	{
		uint8 Sig[4] = { 0x50, 0x4b, 0x01, 0x02 };
		uint16 VersionMade = 63;
		uint16 VersionNeeded = 45;
		uint16 GenPurposeBit = 1 << 11;
		uint16 CompMode = 0;
		uint32 TimeDate;
		uint32 Crc;
		uint32 CompressedSize = 0xFFFFFFFF;
		uint32 UnCompressedSize = 0xFFFFFFFF;
		uint16 FilenameLength;
		uint16 ExtraFieldLength = 28;
		uint16 FileCommentLength = 0;
		uint16 DiskNumberStart = 0;
		uint16 InternalFileAttr = 0;
		uint32 ExternalFileAttr = 1 << 5;
		uint32 RelativeLocHeaderOffset = 0xFFFFFFFF;
	};

	struct FZipCDZip64Extra : public FZip64Extra
	{
		FZipCDZip64Extra()
		{
			Length = sizeof(FZipCDZip64Extra) - sizeof(Id) - sizeof(Length);
		}

		uint64 Offset;
	};

	struct FZip64EndOfCDRecord
	{
		uint8 Sig[4] = { 0x50, 0x4b, 0x06, 0x06 };
		uint64 SizeOfEndOfCDR = 0x2c;
		uint16 VersionMade = 63;
		uint16 VersionNeeded = 45;
		uint32 DiskNumber = 0;
		uint32 CDDiskNumber = 0;
		uint64 CDRecords;
		uint64 CDTotalRecords;
		uint64 CDSize;
		uint64 CDStartOffset;
	};

	struct FZip64EndOfCDLocator
	{
		uint8 Sig[4] = { 0x50, 0x4b, 0x06, 0x07 };
		uint32 DiskNumber = 0;
		uint64 EndOffsetCD;
		uint32 TotalDiskNumber = 1;
	};

	struct FEndOfCDRecord
	{
		uint8 Sig[4] = { 0x50, 0x4b, 0x05, 0x06 };
		uint16 DiskNumber = 0xFFFF;
		uint16 CDDiskNumber = 0xFFFF;
		uint16 CDRecords = 0xFFFF;
		uint16 TotalCDRecords = 0xFFFF;
		uint32 CDSize = 0xFFFFFFFF;
		uint32 CDOffset = 0xFFFFFFFF;
		uint16 CommentLength = 0;
	};
#pragma pack(pop)

	struct FEntry
	{
		FString Filename;
		uint32 Crc32 = 0;
		uint64 Offset = 0;
		uint32 Time = 0;
		uint64 UnCompressedSize = 0;
	};

	void WriteRaw(const void* Data, int64 Size)
	{
		if (Size > 0)
		{
			File->Write((const uint8*) Data, Size);
		}
	}

	void WriteCentralDirectoryHeader(const FEntry& Entry)
	{
		const FTCHARToUTF8 Utf8Filename(*Entry.Filename);

		FZipCDHeader CDHeader;
		CDHeader.TimeDate = Entry.Time;
		CDHeader.Crc = Entry.Crc32;
		CDHeader.FilenameLength = (uint16) Utf8Filename.Length();
		WriteRaw(&CDHeader, sizeof(CDHeader));
		WriteRaw(Utf8Filename.Get(), Utf8Filename.Length());

		FZipCDZip64Extra CDZip64Extra;
		CDZip64Extra.UnCompressedSize = Entry.UnCompressedSize;
		CDZip64Extra.CompressedSize = Entry.UnCompressedSize;
		CDZip64Extra.Offset = Entry.Offset;
		WriteRaw(&CDZip64Extra, sizeof(CDZip64Extra));
	}

	TArray<FEntry> Entries;
	IFileHandle* File;
};

// Entry names keep the archived folder as the top level so Apple bundles such as .gputrace stay openable after unzipping.
bool WriteDirectoryArchive(const FString& DirectoryPath, const FString& ArchivePath)
{
	FString NormalizedDirectory = DirectoryPath;
	FPaths::NormalizeDirectoryName(NormalizedDirectory);

	TArray<FString> SourceFiles;
	IFileManager::Get().FindFilesRecursive(SourceFiles, *NormalizedDirectory, TEXT("*"), true, false);
	if (SourceFiles.IsEmpty())
	{
		return false;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ArchivePath), true);
	IFileHandle* ArchiveHandle = FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*ArchivePath);
	if (ArchiveHandle == nullptr)
	{
		return false;
	}

	const FString BasePath = FPaths::GetPath(NormalizedDirectory) + TEXT("/");
	{
		FDeviceExplorerZipWriter Writer(ArchiveHandle);
		for (const FString& SourceFile : SourceFiles)
		{
			FString EntryName = SourceFile;
			FPaths::MakePathRelativeTo(EntryName, *BasePath);
			Writer.AddFile(EntryName, SourceFile, IFileManager::Get().GetTimeStamp(*SourceFile));
		}
	}
	return IFileManager::Get().FileSize(*ArchivePath) > 0;
}

TSharedRef<FJsonObject> MakeResponse(const FString& Type, const FString& RequestId)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), Type);
	Result->SetStringField(TEXT("request_id"), RequestId);
	return Result;
}

// FName display case comes from whoever registered the name first, which differs per build.
FString ToWireId(const FName Name)
{
	return Name.ToString().ToLower();
}

FString ToWireString(EDeviceExplorerWidget Widget)
{
	switch (Widget)
	{
		case EDeviceExplorerWidget::Text: return TEXT("text");
		case EDeviceExplorerWidget::Bool: return TEXT("bool");
		case EDeviceExplorerWidget::Number: return TEXT("number");
		case EDeviceExplorerWidget::Enum: return TEXT("enum");
		case EDeviceExplorerWidget::String: return TEXT("string");
		case EDeviceExplorerWidget::Badge: return TEXT("badge");
		case EDeviceExplorerWidget::Meter: return TEXT("meter");
		case EDeviceExplorerWidget::Button: return TEXT("button");
		case EDeviceExplorerWidget::Json: return TEXT("json");
		case EDeviceExplorerWidget::Series: return TEXT("series");
		case EDeviceExplorerWidget::Status: return TEXT("status");
		case EDeviceExplorerWidget::Table: return TEXT("table");
		case EDeviceExplorerWidget::Textarea: return TEXT("textarea");
		case EDeviceExplorerWidget::Vector: return TEXT("vector");
		case EDeviceExplorerWidget::Color: return TEXT("color");
		case EDeviceExplorerWidget::Path: return TEXT("path");
		case EDeviceExplorerWidget::Artifact: return TEXT("artifact");
		case EDeviceExplorerWidget::Flags: return TEXT("flags");
		case EDeviceExplorerWidget::ActionForm: return TEXT("action_form");
		default: return TEXT("text");
	}
}

FString ToWireString(EDeviceExplorerApply Apply)
{
	return Apply == EDeviceExplorerApply::Manual ? TEXT("manual") : TEXT("instant");
}

FString ToWireString(EDeviceExplorerSectionStyle Style)
{
	switch (Style)
	{
		case EDeviceExplorerSectionStyle::Stats: return TEXT("stats");
		case EDeviceExplorerSectionStyle::Toolbar: return TEXT("toolbar");
		case EDeviceExplorerSectionStyle::Settings: return TEXT("settings");
		case EDeviceExplorerSectionStyle::Hero: return TEXT("hero");
		default: return TEXT("default");
	}
}

FString ToWireString(EDeviceExplorerNumberDisplay Display)
{
	switch (Display)
	{
		case EDeviceExplorerNumberDisplay::Input: return TEXT("input");
		case EDeviceExplorerNumberDisplay::Slider: return TEXT("slider");
		case EDeviceExplorerNumberDisplay::SliderAndInput: return TEXT("slider_input");
		default: return TEXT("auto");
	}
}

FString ToWireString(EDeviceExplorerEnumDisplay Display)
{
	return Display == EDeviceExplorerEnumDisplay::Segmented ? TEXT("segmented") : TEXT("select");
}

FString ToWireString(EDeviceExplorerActionStyle Style)
{
	switch (Style)
	{
		case EDeviceExplorerActionStyle::Primary: return TEXT("primary");
		case EDeviceExplorerActionStyle::Danger: return TEXT("danger");
		default: return TEXT("default");
	}
}

TArray<TSharedPtr<FJsonValue>> BuildActionInputsJson(const TArray<FDeviceExplorerModuleActionInput>& Inputs)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Inputs.Num());
	for (const FDeviceExplorerModuleActionInput& Input : Inputs)
	{
		TSharedRef<FJsonObject> InputJson = MakeShared<FJsonObject>();
		InputJson->SetStringField(TEXT("id"), ToWireId(Input.Name));
		InputJson->SetStringField(TEXT("label"), Input.DisplayName.ToString());
		InputJson->SetStringField(TEXT("type"), Input.Type);
		if (!Input.DefaultValue.IsEmpty())
		{
			if (Input.Type == TEXT("number"))
			{
				InputJson->SetNumberField(TEXT("default"), FCString::Atod(*Input.DefaultValue));
			}
			else
			{
				InputJson->SetStringField(TEXT("default"), Input.DefaultValue);
			}
		}
		Result.Add(MakeShared<FJsonValueObject>(InputJson));
	}
	return Result;
}

TArray<TSharedPtr<FJsonValue>> BuildSectionsJson(const TArray<FDeviceExplorerSectionDescriptor>& Layout)
{
	TArray<TSharedPtr<FJsonValue>> Sections;
	Sections.Reserve(Layout.Num());
	for (const FDeviceExplorerSectionDescriptor& Section : Layout)
	{
		TSharedRef<FJsonObject> SectionJson = MakeShared<FJsonObject>();
		SectionJson->SetStringField(TEXT("id"), ToWireId(Section.Name));
		SectionJson->SetStringField(TEXT("label"), Section.DisplayName.ToString());
		SectionJson->SetNumberField(TEXT("columns"), Section.Columns);
		SectionJson->SetStringField(TEXT("apply"), ToWireString(Section.Apply));
		SectionJson->SetStringField(TEXT("style"), ToWireString(Section.Style));
		SectionJson->SetBoolField(TEXT("collapsible"), Section.bCollapsible);
		SectionJson->SetBoolField(TEXT("collapsed"), Section.bCollapsed);
		if (!Section.Description.IsEmpty())
		{
			SectionJson->SetStringField(TEXT("description"), Section.Description);
		}

		TArray<TSharedPtr<FJsonValue>> Fields;
		Fields.Reserve(Section.Fields.Num());
		for (const FDeviceExplorerFieldDescriptor& Field : Section.Fields)
		{
			TSharedRef<FJsonObject> FieldJson = MakeShared<FJsonObject>();
			FieldJson->SetStringField(TEXT("id"), ToWireId(Field.Name));
			FieldJson->SetStringField(TEXT("label"), Field.DisplayName.ToString());
			FieldJson->SetStringField(TEXT("widget"), ToWireString(Field.Widget));
			FieldJson->SetBoolField(TEXT("readonly"), Field.bReadOnly);
			FieldJson->SetNumberField(TEXT("span"), Field.Span);
			FieldJson->SetStringField(TEXT("number_display"), ToWireString(Field.NumberDisplay));
			if (!Field.Description.IsEmpty())
			{
				FieldJson->SetStringField(TEXT("description"), Field.Description);
			}
			if (!Field.Unit.IsEmpty())
			{
				FieldJson->SetStringField(TEXT("unit"), Field.Unit);
			}
			if (Field.Min.IsSet())
			{
				FieldJson->SetNumberField(TEXT("min"), Field.Min.GetValue());
			}
			if (Field.Max.IsSet())
			{
				FieldJson->SetNumberField(TEXT("max"), Field.Max.GetValue());
			}
			if (Field.Step.IsSet())
			{
				FieldJson->SetNumberField(TEXT("step"), Field.Step.GetValue());
			}
			if (Field.WarnAbove.IsSet())
			{
				FieldJson->SetNumberField(TEXT("warn_above"), Field.WarnAbove.GetValue());
			}
			if (Field.ErrorAbove.IsSet())
			{
				FieldJson->SetNumberField(TEXT("error_above"), Field.ErrorAbove.GetValue());
			}
			if (!Field.Options.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Options;
				Options.Reserve(Field.Options.Num());
				for (const FString& Option : Field.Options)
				{
					Options.Add(MakeShared<FJsonValueString>(Option));
				}
				FieldJson->SetArrayField(TEXT("options"), MoveTemp(Options));
			}
			if (!Field.Columns.IsEmpty())
			{
				TArray<TSharedPtr<FJsonValue>> Columns;
				Columns.Reserve(Field.Columns.Num());
				for (const FString& Column : Field.Columns)
				{
					Columns.Add(MakeShared<FJsonValueString>(Column));
				}
				FieldJson->SetArrayField(TEXT("columns_spec"), MoveTemp(Columns));
			}
			if (Field.Widget == EDeviceExplorerWidget::Enum)
			{
				FieldJson->SetStringField(TEXT("enum_display"), ToWireString(Field.EnumDisplay));
			}
			if (Field.Widget == EDeviceExplorerWidget::Textarea)
			{
				FieldJson->SetNumberField(TEXT("rows"), Field.Rows);
			}
			if (Field.bSeries)
			{
				FieldJson->SetBoolField(TEXT("series"), true);
			}
			if (Field.Widget == EDeviceExplorerWidget::Button || Field.Widget == EDeviceExplorerWidget::ActionForm)
			{
				FieldJson->SetStringField(TEXT("action"), ToWireId(Field.Action));
				FieldJson->SetBoolField(TEXT("requires_confirmation"), Field.bRequiresConfirmation);
				FieldJson->SetStringField(TEXT("action_style"), ToWireString(Field.ActionStyle));
			}
			if (Field.Widget == EDeviceExplorerWidget::ActionForm)
			{
				FieldJson->SetStringField(TEXT("action_label"), Field.ActionLabel);
				FieldJson->SetArrayField(TEXT("inputs"), BuildActionInputsJson(Field.Inputs));
			}
			Fields.Add(MakeShared<FJsonValueObject>(FieldJson));
		}
		SectionJson->SetArrayField(TEXT("fields"), MoveTemp(Fields));
		Sections.Add(MakeShared<FJsonValueObject>(SectionJson));
	}
	return Sections;
}

TArray<TSharedPtr<FJsonValue>> BuildPagesJson(const TArray<FDeviceExplorerPageDescriptor>& Pages)
{
	TArray<TSharedPtr<FJsonValue>> Result;
	Result.Reserve(Pages.Num());
	for (const FDeviceExplorerPageDescriptor& Page : Pages)
	{
		TSharedRef<FJsonObject> PageJson = MakeShared<FJsonObject>();
		PageJson->SetStringField(TEXT("id"), ToWireId(Page.Name));
		PageJson->SetStringField(TEXT("label"), Page.DisplayName.ToString());
		PageJson->SetStringField(TEXT("description"), Page.Description);
		PageJson->SetStringField(TEXT("icon"), Page.Icon);
		PageJson->SetArrayField(TEXT("sections"), BuildSectionsJson(Page.Sections));
		Result.Add(MakeShared<FJsonValueObject>(PageJson));
	}
	return Result;
}
}    // namespace

void FDeviceExplorerModule::StartupModule()
{
#if UE_BUILD_SHIPPING
	return;
#else
	RegisterDefaultFeatures();
	bStarted = true;

	FString ProvisionedToken;
	if (!FParse::Value(FCommandLine::Get(), TEXT("DeviceExplorerToken="), ProvisionedToken))
	{
		// A build launched from a device's home screen has no command line.
		ProvisionedToken = GetDefault<UDeviceExplorerSettings>()->SessionToken.TrimStartAndEnd();
	}
	if (!ProvisionedToken.IsEmpty())
	{
		if (DeviceExplorer::Auth::IsWeakToken(ProvisionedToken))
		{
			UE_LOG(LogDeviceExplorer, Warning,
			       TEXT("The DeviceExplorer session token is short enough to be guessed offline from the "
			            "advertised fingerprint. Clear it in project settings to have one generated."));
		}
		DeviceExplorer::Auth::SetProvisionedToken(ProvisionedToken);
	}

	DeviceIdentityStore = MakeUnique<FDeviceExplorerDeviceIdentityStore>(
		FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DeviceExplorer"), TEXT("device-identity.txt")));
	FString IdentityError;
	if (!DeviceIdentityStore->Load(IdentityError))
	{
		UE_LOG(LogDeviceExplorer, Error, TEXT("%s; network connections are disabled"), *IdentityError);
	}
	DeviceId = DeviceIdentityStore->GetDeviceId();
	LogService = MakeUnique<FDeviceExplorerLogService>([this](const TSharedRef<FJsonObject>& Message) { SendJson(Message); });
	LogService->Start();
	ConsoleCatalog = MakeUnique<FDeviceExplorerConsoleCatalog>();

	ConnectionCoordinator = MakeUnique<FDeviceExplorerConnectionCoordinator>();
	Transport = CreateDeviceExplorerTransport();
	EndpointCallbackGate = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(true);
	EndpointSources = CreateDeviceExplorerEndpointSources();
	TArray<TUniquePtr<IDeviceExplorerEndpointSource>> RegisteredSources = FDeviceExplorerCoreModule::Get().CreateEndpointSources();
	EndpointSources.Append(MoveTemp(RegisteredSources));

	TSet<FName> ProviderIds;
	for (int32 Index = 0; Index < EndpointSources.Num();)
	{
		const FName ProviderId = EndpointSources[Index] ? EndpointSources[Index]->GetProviderId() : NAME_None;
		if (ProviderId.IsNone() || ProviderIds.Contains(ProviderId))
		{
			UE_LOG(LogDeviceExplorer, Warning, TEXT("Ignoring duplicate or invalid endpoint provider: %s"), *ProviderId.ToString());
			EndpointSources.RemoveAt(Index);
			continue;
		}
		ProviderIds.Add(ProviderId);
		++Index;
	}

	ConnectConsoleCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("DeviceExplorer.Connect"),
		TEXT("Pins DeviceExplorer to an IPv4 endpoint. Usage: DeviceExplorer.Connect <address>:<port> <token>"),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FDeviceExplorerModule::ConnectManually),
		ECVF_Default);
	UnpinConsoleCommand = IConsoleManager::Get().RegisterConsoleCommand(
		TEXT("DeviceExplorer.Unpin"),
		TEXT("Removes the manual DeviceExplorer endpoint pin and resumes automatic selection."),
		FConsoleCommandWithArgsDelegate::CreateRaw(this, &FDeviceExplorerModule::UnpinManualEndpoint),
		ECVF_Default);

	for (TUniquePtr<IDeviceExplorerEndpointSource>& Source : EndpointSources)
	{
		const TWeakPtr<std::atomic<bool>, ESPMode::ThreadSafe> WeakGate = EndpointCallbackGate;
		Source->Start(
			[this, WeakGate](const EDeviceExplorerEndpointEvent Event, FDeviceExplorerEndpointCandidate Candidate) mutable
			{
				const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Gate = WeakGate.Pin();
				if (!Gate || !Gate->load(std::memory_order_acquire))
				{
					return;
				}

				auto Dispatch = [this, WeakGate, Event, Candidate = MoveTemp(Candidate)]() mutable
				{
					const TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> DispatchGate = WeakGate.Pin();
					if (DispatchGate && DispatchGate->load(std::memory_order_acquire))
					{
						OnEndpointEvent(Event, MoveTemp(Candidate));
					}
				};
				if (IsInGameThread())
				{
					Dispatch();
				}
				else
				{
					AsyncTask(ENamedThreads::GameThread, MoveTemp(Dispatch));
				}
			});
	}

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateRaw(this, &FDeviceExplorerModule::Tick), 0.1f);
	UE_LOG(LogDeviceExplorer, Display, TEXT("DeviceExplorer client started with %s transport"),
	       Transport ? Transport->GetName() : TEXT("no"));
#endif
}

void FDeviceExplorerModule::ShutdownModule()
{
	if (!bStarted)
	{
		return;
	}

	bStarted = false;
	if (EndpointCallbackGate)
	{
		EndpointCallbackGate->store(false, std::memory_order_release);
	}
	if (ConnectConsoleCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(ConnectConsoleCommand, false);
		ConnectConsoleCommand = nullptr;
	}
	if (UnpinConsoleCommand != nullptr)
	{
		IConsoleManager::Get().UnregisterConsoleObject(UnpinConsoleCommand, false);
		UnpinConsoleCommand = nullptr;
	}
	if (FDeviceExplorerCoreModule::IsAvailable())
	{
		FDeviceExplorerCoreModule::Get().UnregisterOwner(RuntimeOwner);
	}
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	for (TUniquePtr<IDeviceExplorerEndpointSource>& Source : EndpointSources)
	{
		Source->Stop();
	}
	EndpointSources.Empty();
	EndpointCallbackGate.Reset();
	Disconnect();
	Transport.Reset();
	ConnectionCoordinator.Reset();
	DeviceIdentityStore.Reset();
	LogService.Reset();
	ConsoleCatalog.Reset();
}

void FDeviceExplorerModule::RegisterDefaultFeatures()
{
	FDeviceExplorerCoreModule& Registry = FDeviceExplorerCoreModule::Get();
	Registry.RegisterCapability(RuntimeOwner, DeviceExplorer::LogsCapability);
	Registry.RegisterCapability(RuntimeOwner, DeviceExplorer::ConsoleCapability);
	Registry.RegisterCapability(RuntimeOwner, DeviceExplorer::FilesCapability);
	Registry.RegisterCapability(RuntimeOwner, DeviceExplorer::ModulesCapability);
	Registry.RegisterCapability(RuntimeOwner, DeviceExplorer::TraceCapability);

	FDeviceExplorerFileRootDescriptor SavedRoot;
	SavedRoot.Owner = RuntimeOwner;
	SavedRoot.Name = TEXT("saved");
	SavedRoot.DisplayName = NSLOCTEXT("DeviceExplorer", "SavedRoot", "Saved");
	SavedRoot.AbsolutePath = FPaths::ProjectSavedDir();
	SavedRoot.bAllowDownload = true;
	SavedRoot.bAllowDirectoryTransfer = false;
	Registry.RegisterFileRoot(MoveTemp(SavedRoot));

	FDeviceExplorerDataModuleDescriptor RuntimeModule;
	RuntimeModule.Owner = RuntimeOwner;
	RuntimeModule.Name = TEXT("runtime");
	RuntimeModule.DisplayName = NSLOCTEXT("DeviceExplorer", "RuntimeModule", "Runtime");
	RuntimeModule.Description = TEXT("Generic process, build, and memory information.");
	RuntimeModule.Icon = TEXT("pulse");
	RuntimeModule.RefreshIntervalMs = 2000;
	RuntimeModule.DataProvider = []()
	{
		const FPlatformMemoryStats Memory = FPlatformMemory::GetStats();
		constexpr double Megabyte = 1024.0 * 1024.0;

		TSharedRef<FJsonObject> Session = MakeShared<FJsonObject>();
		Session->SetStringField(TEXT("Project"), FApp::GetProjectName());
		Session->SetStringField(TEXT("Engine"), FEngineVersion::Current().ToString());
		Session->SetStringField(TEXT("Platform"), FPlatformProperties::PlatformName());
		Session->SetStringField(TEXT("Configuration"), LexToString(FApp::GetBuildConfiguration()));
		Session->SetStringField(TEXT("Build"), FApp::GetBuildVersion());
		Session->SetNumberField(TEXT("Uptime seconds"), GetAppUptimeSeconds());

		TSharedRef<FJsonObject> MemoryJson = MakeShared<FJsonObject>();
		MemoryJson->SetNumberField(TEXT("Used physical MB"), Memory.UsedPhysical / Megabyte);
		MemoryJson->SetNumberField(TEXT("Peak physical MB"), Memory.PeakUsedPhysical / Megabyte);
		MemoryJson->SetNumberField(TEXT("Available physical MB"), Memory.AvailablePhysical / Megabyte);

		TSharedRef<FJsonObject> Data = MakeShared<FJsonObject>();
		Data->SetObjectField(TEXT("Session"), Session);
		Data->SetObjectField(TEXT("Memory"), MemoryJson);

		FDeviceExplorerModuleResult Result;
		Result.Data = Data;
		return Result;
	};
	Registry.RegisterDataModule(MoveTemp(RuntimeModule));
}

bool FDeviceExplorerModule::Tick(float DeltaTime)
{
	(void) DeltaTime;
	if (!bStarted)
	{
		return false;
	}

	const double Now = FPlatformTime::Seconds();
	if (Transport)
	{
		Transport->Tick(Now);
	}
	if (!bConnecting && (!Transport || !Transport->IsConnected()) && ConnectionCoordinator)
	{
		FDeviceExplorerEndpointCandidate Candidate;
		if (ConnectionCoordinator->SelectNext(Now, Candidate))
		{
			Connect(MoveTemp(Candidate));
		}
	}

	if (bAuthenticated && Transport && Transport->IsConnected())
	{
		LogService->Flush();
		if (Now - LastHeartbeatSeconds >= 5.0)
		{
			SendHeartbeat();
			LastHeartbeatSeconds = Now;
		}
	}
	return true;
}

bool FDeviceExplorerModule::ResolveCandidateToken(FDeviceExplorerEndpointCandidate& Candidate)
{
	// A source that carries no token was told where a host is, not which token it holds.
	if (Candidate.Token.IsEmpty())
	{
		Candidate.Token = DeviceExplorer::Auth::GetProvisionedToken();
	}
	if (Candidate.Token.IsEmpty())
	{
		UE_LOG(LogDeviceExplorer, Verbose, TEXT("Ignoring %s: no session token was provisioned"),
		       *Candidate.Endpoint.Serialized.ToString());
		return false;
	}

	// Selects our host among several; a copied fingerprint still fails the challenge.
	if (!Candidate.HostFingerprint.IsEmpty() &&
	    !DeviceExplorer::Auth::ConstantTimeEquals(Candidate.HostFingerprint,
	                                              DeviceExplorer::Auth::ComputeTokenFingerprint(Candidate.Token)))
	{
		// Reported once: foreign hosts are normal on a shared network, but without any
		// report a host started without a matching -Token looks like no host at all.
		if (!bLoggedFingerprintMismatch)
		{
			bLoggedFingerprintMismatch = true;
			UE_LOG(LogDeviceExplorer, Warning, TEXT("Ignoring %s and any other host whose token differs from this build's"),
			       *Candidate.Endpoint.Serialized.ToString());
		}
		return false;
	}
	return true;
}

void FDeviceExplorerModule::OnEndpointEvent(const EDeviceExplorerEndpointEvent Event, FDeviceExplorerEndpointCandidate Candidate)
{
	// Source callbacks can arrive after ShutdownModule() clears bStarted.
	if (!bStarted || !ConnectionCoordinator)
	{
		return;
	}

	if (Event != EDeviceExplorerEndpointEvent::Removed && !ResolveCandidateToken(Candidate))
	{
		return;
	}

	if (Candidate.bManual && Event != EDeviceExplorerEndpointEvent::Removed)
	{
		if (ConnectionCoordinator->Pin(MoveTemp(Candidate)))
		{
			Disconnect();
			ConnectionCoordinator->AbandonActive();
		}
		return;
	}
	ConnectionCoordinator->ApplyEvent(Event, MoveTemp(Candidate));
}

void FDeviceExplorerModule::Connect(FDeviceExplorerEndpointCandidate Candidate)
{
	if (bConnecting || !Candidate.IsValid() || !ConnectionCoordinator || !ConnectionCoordinator->IsActive(Candidate))
	{
		return;
	}

	Disconnect();
	bConnecting = true;
	const uint64 AttemptGeneration = ConnectionGeneration;
	if (!Transport)
	{
		bConnecting = false;
		ConnectionCoordinator->MarkFailed(Candidate, FPlatformTime::Seconds());
		return;
	}
	if (!DeviceIdentityStore)
	{
		bConnecting = false;
		ConnectionCoordinator->MarkFailed(Candidate, FPlatformTime::Seconds());
		return;
	}
	FDeviceExplorerConnectionIdentity ConnectionIdentity;
	FString IdentityError;
	if (!DeviceIdentityStore->AllocateConnection(ConnectionIdentity, IdentityError))
	{
		bConnecting = false;
		ConnectionCoordinator->MarkFailed(Candidate, FPlatformTime::Seconds());
		UE_LOG(LogDeviceExplorer, Error, TEXT("%s; refusing to open a socket"), *IdentityError);
		return;
	}
	DeviceId = MoveTemp(ConnectionIdentity.DeviceId);
	DeviceSession = ConnectionIdentity.DeviceSession;
	ConnectionId = MoveTemp(ConnectionIdentity.ConnectionId);
	AuthenticatingCandidate = Candidate;

	FDeviceExplorerTransportCallbacks Callbacks;
	Callbacks.OnConnected = [this, AttemptGeneration, Candidate]()
		{
			if (!bStarted || AttemptGeneration != ConnectionGeneration || !ConnectionCoordinator || !ConnectionCoordinator->IsActive(Candidate))
			{
				return;
			}
			bConnecting = false;
			LastHeartbeatSeconds = 0.0;
			SendAuthRequest();
			UE_LOG(LogDeviceExplorer, Display, TEXT("Authenticating with %s via %s"),
			       *Candidate.Endpoint.Serialized.ToString(), *Candidate.ProviderId.ToString());
		};
	Callbacks.OnConnectionError = [this, AttemptGeneration, Candidate](const FString& Error)
		{
			if (!bStarted || AttemptGeneration != ConnectionGeneration || !ConnectionCoordinator || !ConnectionCoordinator->IsActive(Candidate))
			{
				return;
			}
			bConnecting = false;
			ConnectionCoordinator->MarkFailed(Candidate, FPlatformTime::Seconds());
			UE_LOG(LogDeviceExplorer, Warning, TEXT("Connection to %s failed: %s"), *Candidate.Endpoint.Serialized.ToString(), *Error);
		};
	Callbacks.OnClosed = [this, AttemptGeneration, Candidate](int32 StatusCode, const FString& Reason, bool bWasClean)
		{
			if (!bStarted || AttemptGeneration != ConnectionGeneration || !ConnectionCoordinator || !ConnectionCoordinator->IsActive(Candidate))
			{
				return;
			}
			(void) StatusCode;
			(void) bWasClean;
			bConnecting = false;
			ConnectionCoordinator->MarkFailed(Candidate, FPlatformTime::Seconds());
			UE_LOG(LogDeviceExplorer, Display, TEXT("Connection to %s closed: %s"), *Candidate.Endpoint.Serialized.ToString(), *Reason);
			if (LogService)
			{
				LogService->RevertOverrides();
			}
		};
	Callbacks.OnMessage = [this, AttemptGeneration](const FString& Message)
		{
			if (bStarted && AttemptGeneration == ConnectionGeneration)
			{
				HandleMessage(Message);
			}
		};
	Transport->Connect(Candidate.Endpoint, MoveTemp(Callbacks));
}

void FDeviceExplorerModule::Disconnect()
{
	++ConnectionGeneration;
	bConnecting = false;
	bAuthenticated = false;
	AuthenticatingCandidate.Reset();
	ClientNonce.Reset();
	if (!Transport)
	{
		return;
	}
	Transport->Close();
}

void FDeviceExplorerModule::ConnectManually(const TArray<FString>& Arguments)
{
	if (!bStarted || !ConnectionCoordinator)
	{
		return;
	}
	if (Arguments.Num() != 2)
	{
		UE_LOG(LogDeviceExplorer, Warning, TEXT("Usage: DeviceExplorer.Connect <IPv4-address>:<port> <token>"));
		return;
	}

	FDeviceExplorerSerializedEndpoint Endpoint;
	FString Error;
	if (!ParseDeviceExplorerEndpoint(Arguments[0], Endpoint, &Error))
	{
		UE_LOG(LogDeviceExplorer, Warning, TEXT("Invalid DeviceExplorer endpoint: %s"), *Error);
		return;
	}
	if (Arguments[1].IsEmpty())
	{
		UE_LOG(LogDeviceExplorer, Warning, TEXT("DeviceExplorer token cannot be empty"));
		return;
	}

	FDeviceExplorerEndpointCandidate Candidate;
	Candidate.ProviderId = TEXT("Console");
	Candidate.CandidateId = TEXT("ManualPin");
	Candidate.Endpoint.Serialized = MoveTemp(Endpoint);
	Candidate.Token = Arguments[1];
	Candidate.Instance = TEXT("Manual");
	Candidate.bManual = true;
	if (ConnectionCoordinator->Pin(MoveTemp(Candidate)))
	{
		Disconnect();
		ConnectionCoordinator->AbandonActive();
	}
}

void FDeviceExplorerModule::UnpinManualEndpoint(const TArray<FString>& Arguments)
{
	(void) Arguments;
	if (!bStarted || !ConnectionCoordinator)
	{
		return;
	}
	if (ConnectionCoordinator->Unpin())
	{
		Disconnect();
		ConnectionCoordinator->AbandonActive();
	}
}

void FDeviceExplorerModule::SendHello()
{
	const FDeviceExplorerRegistrySnapshot Registry = FDeviceExplorerCoreModule::Get().Snapshot();
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("hello"));
	Message->SetStringField(TEXT("device_id"), DeviceId);
	Message->SetStringField(TEXT("device_session"), LexToString(DeviceSession));
	Message->SetStringField(TEXT("connection_id"), ConnectionId);
	Message->SetStringField(TEXT("name"), FPlatformProcess::ComputerName());
	Message->SetStringField(TEXT("project_name"), FApp::GetProjectName());
	Message->SetStringField(TEXT("engine_version"), FEngineVersion::Current().ToString());
	Message->SetStringField(TEXT("platform"), FPlatformProperties::PlatformName());
	Message->SetStringField(TEXT("configuration"), LexToString(FApp::GetBuildConfiguration()));
	Message->SetStringField(TEXT("build_version"), FApp::GetBuildVersion());
	Message->SetNumberField(TEXT("protocol_version"), DeviceExplorer::ProtocolVersion);
	Message->SetNumberField(TEXT("uptime_seconds"), static_cast<int64>(GetAppUptimeSeconds()));
	TArray<TSharedPtr<FJsonValue>> Capabilities;
	Capabilities.Reserve(Registry.Capabilities.Num());
	for (const FName Capability : Registry.Capabilities)
	{
		Capabilities.Add(MakeShared<FJsonValueString>(ToWireId(Capability)));
	}
	Message->SetArrayField(TEXT("capabilities"), MoveTemp(Capabilities));

	TArray<TSharedPtr<FJsonValue>> Commands;
	Commands.Reserve(Registry.Commands.Num());
	for (const FDeviceExplorerCommandDescriptor& Descriptor : Registry.Commands)
	{
		TSharedRef<FJsonObject> Command = MakeShared<FJsonObject>();
		Command->SetStringField(TEXT("id"), ToWireId(Descriptor.Name));
		Command->SetStringField(TEXT("label"), Descriptor.DisplayName.ToString());
		Command->SetStringField(TEXT("category"), Descriptor.Category.ToString());
		Command->SetStringField(TEXT("command"), Descriptor.Command);
		Command->SetStringField(TEXT("description"), Descriptor.Description);
		Command->SetBoolField(TEXT("requires_confirmation"), Descriptor.bRequiresConfirmation);
		Commands.Add(MakeShared<FJsonValueObject>(Command));
	}
	Message->SetArrayField(TEXT("commands"), MoveTemp(Commands));

	TArray<TSharedPtr<FJsonValue>> FileRoots;
	FileRoots.Reserve(Registry.FileRoots.Num());
	for (const FDeviceExplorerFileRootDescriptor& Descriptor : Registry.FileRoots)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("id"), ToWireId(Descriptor.Name));
		Root->SetStringField(TEXT("label"), Descriptor.DisplayName.ToString());
		Root->SetBoolField(TEXT("allow_download"), Descriptor.bAllowDownload);
		Root->SetBoolField(TEXT("allow_directory_transfer"), Descriptor.bAllowDirectoryTransfer);
		FileRoots.Add(MakeShared<FJsonValueObject>(Root));
	}
	Message->SetArrayField(TEXT("file_roots"), MoveTemp(FileRoots));

	TArray<TSharedPtr<FJsonValue>> DataModules;
	DataModules.Reserve(Registry.DataModules.Num());
	for (const FDeviceExplorerDataModuleDescriptor& Descriptor : Registry.DataModules)
	{
		TSharedRef<FJsonObject> DataModule = MakeShared<FJsonObject>();
		DataModule->SetStringField(TEXT("id"), ToWireId(Descriptor.Name));
		DataModule->SetStringField(TEXT("label"), Descriptor.DisplayName.ToString());
		DataModule->SetStringField(TEXT("description"), Descriptor.Description);
		DataModule->SetStringField(TEXT("icon"), Descriptor.Icon);
		DataModule->SetNumberField(TEXT("refresh_interval_ms"), Descriptor.RefreshIntervalMs);

		TArray<TSharedPtr<FJsonValue>> Actions;
		Actions.Reserve(Descriptor.Actions.Num());
		for (const FDeviceExplorerModuleActionDescriptor& ActionDescriptor : Descriptor.Actions)
		{
			TSharedRef<FJsonObject> Action = MakeShared<FJsonObject>();
			Action->SetStringField(TEXT("id"), ToWireId(ActionDescriptor.Name));
			Action->SetStringField(TEXT("label"), ActionDescriptor.DisplayName.ToString());
			Action->SetStringField(TEXT("description"), ActionDescriptor.Description);
			Action->SetBoolField(TEXT("requires_confirmation"), ActionDescriptor.bRequiresConfirmation);
			Action->SetBoolField(TEXT("hidden"), ActionDescriptor.Name.ToString().StartsWith(TEXT("__")));

			Action->SetArrayField(TEXT("inputs"), BuildActionInputsJson(ActionDescriptor.Inputs));

			Actions.Add(MakeShared<FJsonValueObject>(Action));
		}
		DataModule->SetArrayField(TEXT("actions"), MoveTemp(Actions));
		DataModule->SetArrayField(TEXT("pages"), BuildPagesJson(Descriptor.Pages));
		if (!Descriptor.Pages.IsEmpty())
		{
			DataModule->SetArrayField(TEXT("layout"), BuildSectionsJson(Descriptor.Pages[0].Sections));
		}
		DataModules.Add(MakeShared<FJsonValueObject>(DataModule));
	}
	Message->SetArrayField(TEXT("data_modules"), MoveTemp(DataModules));
	SendJson(Message);
}

void FDeviceExplorerModule::SendAuthRequest()
{
	bAuthenticated = false;
	ClientNonce = DeviceExplorer::Auth::MakeNonce();

	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("auth_request"));
	Message->SetNumberField(TEXT("protocol_version"), DeviceExplorer::ProtocolVersion);
	Message->SetStringField(TEXT("client_nonce"), ClientNonce);
	SendUnauthenticatedJson(Message);
}

bool FDeviceExplorerModule::HandleAuthMessage(const FString& Type, const TSharedPtr<FJsonObject>& Message)
{
	if (!AuthenticatingCandidate.IsSet() || !ConnectionCoordinator)
	{
		return false;
	}

	if (Type == TEXT("auth_challenge"))
	{
		FString HostNonce;
		FString HostProof;
		Message->TryGetStringField(TEXT("host_nonce"), HostNonce);
		Message->TryGetStringField(TEXT("host_proof"), HostProof);

		const FString Endpoint = AuthenticatingCandidate->Endpoint.Serialized.ToString();
		const FString Expected = DeviceExplorer::Auth::ComputeProof(
			AuthenticatingCandidate->Token, DeviceExplorer::Auth::HostProofLabel, ClientNonce, HostNonce);
		if (!DeviceExplorer::Auth::IsValidNonce(HostNonce) ||
		    !DeviceExplorer::Auth::ConstantTimeEquals(HostProof, Expected))
		{
			UE_LOG(LogDeviceExplorer, Warning, TEXT("%s could not prove it holds our session token; disconnecting"), *Endpoint);
			ConnectionCoordinator->MarkFailed(*AuthenticatingCandidate, FPlatformTime::Seconds());
			if (LogService)
			{
				LogService->RevertOverrides();
			}
			// This runs inside the transport's own message callback, so the transport
			// defers releasing its socket; the reconnect is driven from the next Tick.
			Disconnect();
			return true;
		}

		TSharedRef<FJsonObject> Response = MakeShared<FJsonObject>();
		Response->SetStringField(TEXT("type"), TEXT("auth_response"));
		Response->SetStringField(TEXT("client_proof"),
		                         DeviceExplorer::Auth::ComputeProof(AuthenticatingCandidate->Token,
		                                                           DeviceExplorer::Auth::DeviceProofLabel,
		                                                           ClientNonce, HostNonce));
		SendUnauthenticatedJson(Response);
		return true;
	}

	if (Type == TEXT("auth_error"))
	{
		FString Error;
		Message->TryGetStringField(TEXT("error"), Error);
		UE_LOG(LogDeviceExplorer, Warning, TEXT("%s refused authentication: %s"),
		       *AuthenticatingCandidate->Endpoint.Serialized.ToString(), *Error);
		return true;
	}

	if (Type == TEXT("auth_ok"))
	{
		bAuthenticated = true;
		ConnectionCoordinator->MarkConnected(*AuthenticatingCandidate);
		UE_LOG(LogDeviceExplorer, Display, TEXT("Connected to %s via %s"),
		       *AuthenticatingCandidate->Endpoint.Serialized.ToString(), *AuthenticatingCandidate->ProviderId.ToString());
		SendHello();
		return true;
	}

	return false;
}

void FDeviceExplorerModule::SendHeartbeat()
{
	TSharedRef<FJsonObject> Message = MakeShared<FJsonObject>();
	Message->SetStringField(TEXT("type"), TEXT("heartbeat"));
	Message->SetNumberField(TEXT("uptime_seconds"), static_cast<int64>(GetAppUptimeSeconds()));
	SendJson(Message);
}

void FDeviceExplorerModule::HandleMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
	{
		return;
	}

	const FString Type = Json->GetStringField(TEXT("type"));
	if (HandleAuthMessage(Type, Json))
	{
		return;
	}
	if (!bAuthenticated)
	{
		return;
	}
	if ((Type == TEXT("attach_ack") || Type == TEXT("reconnect_required")) && HandleAttachAck(Json))
	{
		return;
	}
	if (LogService && LogService->HandleMessage(Type, Json))
	{
		return;
	}
	if (Type == TEXT("execute_command"))
	{
		ExecuteCommand(Json);
	}
	else if (Type == TEXT("list_console_objects"))
	{
		ListConsoleObjects(Json);
	}
	else if (Type == TEXT("list_files"))
	{
		ListFiles(Json);
	}
	else if (Type == TEXT("get_module_data"))
	{
		GetModuleData(Json);
	}
	else if (Type == TEXT("invoke_module_action"))
	{
		InvokeModuleAction(Json);
	}
	else if (Type == TEXT("upload_file"))
	{
		UploadFile(Json);
	}
}

bool FDeviceExplorerModule::HandleAttachAck(const TSharedPtr<FJsonObject>& Message)
{
	bool bAccepted = false;
	if (Message->TryGetBoolField(TEXT("accepted"), bAccepted) && bAccepted) return true;
	FString KnownText;
	uint64 KnownSession = 0;
	if (!Message->TryGetStringField(TEXT("last_known_device_session"), KnownText) ||
	    !LexTryParseString(KnownSession, *KnownText) || KnownSession < DeviceSession || !DeviceIdentityStore)
	{
		UE_LOG(LogDeviceExplorer, Error, TEXT("Host rejected the device attach without a usable last-known session"));
		Disconnect();
		if (ConnectionCoordinator) ConnectionCoordinator->AbandonActive();
		return true;
	}
	FString Error;
	if (!DeviceIdentityStore->AdvancePast(KnownSession, Error))
	{
		UE_LOG(LogDeviceExplorer, Error, TEXT("%s; refusing rollback recovery"), *Error);
		Disconnect();
		if (ConnectionCoordinator) ConnectionCoordinator->AbandonActive();
		return true;
	}
	UE_LOG(LogDeviceExplorer, Warning,
	       TEXT("Host remembered device session %llu; advanced the persistent counter and reconnecting"),
	       static_cast<unsigned long long>(KnownSession));
	Disconnect();
	if (ConnectionCoordinator) ConnectionCoordinator->AbandonActive();
	return true;
}

void FDeviceExplorerModule::ExecuteCommand(const TSharedPtr<FJsonObject>& Message)
{
	FString RequestId;
	FString CommandId;
	FString Arguments;
	FString Command;
	if (!Message->TryGetStringField(TEXT("request_id"), RequestId))
	{
		return;
	}
	Message->TryGetStringField(TEXT("command_id"), CommandId);
	Message->TryGetStringField(TEXT("arguments"), Arguments);
	Message->TryGetStringField(TEXT("command"), Command);

	FDeviceExplorerCommandDescriptor Descriptor;
	const bool bRegistered = !CommandId.IsEmpty() && FDeviceExplorerCoreModule::Get().FindCommand(FName(*CommandId), Descriptor);
	if (bRegistered && Command.IsEmpty())
	{
		Command = Descriptor.Command;
		if (!Arguments.IsEmpty())
		{
			Command += TEXT(" ");
			Command += Arguments;
		}
	}
	if (Command.IsEmpty() && !bRegistered)
	{
		return;
	}

	AsyncTask(ENamedThreads::GameThread,
	          [this, RequestId = MoveTemp(RequestId), Command = MoveTemp(Command), Arguments = MoveTemp(Arguments), Descriptor = MoveTemp(Descriptor), bRegistered]()
	          {
				  TRACE_CPUPROFILER_EVENT_SCOPE(DeviceExplorer::ExecuteCommand);
				  SCOPE_CYCLE_COUNTER(STAT_DeviceExplorerCommand);
				  TRACE_COUNTER_INCREMENT(DeviceExplorerCommandsExecuted);

				  FDeviceExplorerCommandResult CommandResult;
				  FString LogOutput;
				  {
					  FDeviceExplorerCommandCapture Capture;
					  if (bRegistered && Descriptor.Handler)
					  {
						  CommandResult = Descriptor.Handler(Arguments);
					  }
					  else
					  {
						  FStringOutputDevice Output;
						  CommandResult.bSuccess = IConsoleManager::Get().ProcessUserConsoleInput(*Command, Output, GWorld);
						  if (!CommandResult.bSuccess && GEngine != nullptr)
						  {
							  CommandResult.bSuccess = GEngine->Exec(GWorld, *Command, Output);
						  }
						  CommandResult.Output = Output;
					  }
					  LogOutput = Capture.BuildOutput(CommandResult.Output);
				  }

				  TSharedRef<FJsonObject> Result = MakeResponse(TEXT("command_result"), RequestId);
				  Result->SetBoolField(TEXT("success"), CommandResult.bSuccess);
				  Result->SetStringField(TEXT("output"), CommandResult.Output);
				  Result->SetStringField(TEXT("log_output"), LogOutput);
				  SendJson(Result);
			  });
}

void FDeviceExplorerModule::SendConsoleIndex(const FString& RequestId, const bool bRefresh)
{
	AsyncTask(ENamedThreads::GameThread,
	          [this, RequestId, bRefresh]()
	          {
				  if (!ConsoleCatalog)
				  {
					  return;
				  }

				  TRACE_CPUPROFILER_EVENT_SCOPE(DeviceExplorer::BuildConsoleIndex);
				  SCOPE_CYCLE_COUNTER(STAT_DeviceExplorerCatalogIndex);

				  // Help text is the bulk of the catalog and is only ever read one entry at a time, so the index
				  // leaves it out and the dashboard fetches it when an entry is selected.
				  const TArray<FDeviceExplorerConsoleEntry>& Catalog = ConsoleCatalog->Get(bRefresh);
				  TArray<TSharedPtr<FJsonValue>> Entries;
				  Entries.Reserve(Catalog.Num());
				  for (const FDeviceExplorerConsoleEntry& Entry : Catalog)
				  {
					  TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
					  Json->SetStringField(TEXT("name"), Entry.Name);
					  Json->SetStringField(TEXT("type"), Entry.bIsVariable ? TEXT("variable") : TEXT("command"));
					  Json->SetStringField(TEXT("source"), FDeviceExplorerConsoleCatalog::SourceToString(Entry.Source));
					  if (!Entry.Arguments.IsEmpty())
					  {
						  Json->SetStringField(TEXT("arguments"), Entry.Arguments);
					  }
					  if (!Entry.Companion.IsEmpty())
					  {
						  Json->SetStringField(TEXT("companion"), Entry.Companion);
					  }
					  if (Entry.bReadOnly)
					  {
						  Json->SetBoolField(TEXT("read_only"), true);
					  }
					  if (Entry.bCheat)
					  {
						  Json->SetBoolField(TEXT("cheat"), true);
					  }

					  const FString& ValueSource = Entry.bIsVariable ? Entry.Name : Entry.Companion;
					  if (!ValueSource.IsEmpty())
					  {
						  if (const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*ValueSource, false))
						  {
							  FString Value = Variable->GetString();
							  Value.LeftInline(128);
							  Json->SetStringField(TEXT("value"), Value);
						  }
					  }
					  Entries.Add(MakeShared<FJsonValueObject>(Json));
				  }

				  TSharedRef<FJsonObject> Result = MakeResponse(TEXT("console_objects_result"), RequestId);
				  Result->SetBoolField(TEXT("index"), true);
				  Result->SetNumberField(TEXT("total"), Entries.Num());
				  Result->SetNumberField(TEXT("catalog_total"), Entries.Num());
				  Result->SetBoolField(TEXT("truncated"), false);
				  Result->SetArrayField(TEXT("entries"), MoveTemp(Entries));
				  SendJson(Result);
			  });
}

void FDeviceExplorerModule::ListConsoleObjects(const TSharedPtr<FJsonObject>& Message)
{
	FString RequestId;
	FString Query;
	FString Scope;
	FString SourceFilter;
	FString KindFilter;
	bool bIndex = false;
	bool bRefresh = false;
	double RequestedLimit = 400.0;
	if (!Message->TryGetStringField(TEXT("request_id"), RequestId))
	{
		return;
	}
	Message->TryGetStringField(TEXT("query"), Query);
	Message->TryGetStringField(TEXT("scope"), Scope);
	Message->TryGetStringField(TEXT("source"), SourceFilter);
	Message->TryGetStringField(TEXT("kind"), KindFilter);
	Message->TryGetBoolField(TEXT("index"), bIndex);
	Message->TryGetBoolField(TEXT("refresh"), bRefresh);
	Message->TryGetNumberField(TEXT("limit"), RequestedLimit);
	const int32 Limit = FMath::Clamp(FMath::RoundToInt(RequestedLimit), 1, 2000);
	const bool bSearchHelp = Scope == TEXT("all");

	if (bIndex)
	{
		SendConsoleIndex(RequestId, bRefresh);
		return;
	}

	AsyncTask(ENamedThreads::GameThread,
	          [this, RequestId = MoveTemp(RequestId), Query = MoveTemp(Query), SourceFilter = MoveTemp(SourceFilter),
	           KindFilter = MoveTemp(KindFilter), Limit, bSearchHelp, bRefresh]()
	          {
				  if (!ConsoleCatalog)
				  {
					  return;
				  }

				  TRACE_CPUPROFILER_EVENT_SCOPE(DeviceExplorer::QueryConsoleCatalog);
				  SCOPE_CYCLE_COUNTER(STAT_DeviceExplorerCatalogQuery);
				  TRACE_COUNTER_INCREMENT(DeviceExplorerConsoleQueries);

				  // Source and kind are filtered here rather than in the dashboard: the limit truncates a
				  // name-sorted catalog, so a client-side filter would only ever see its first page.
				  const TArray<FDeviceExplorerConsoleEntry>& Catalog = ConsoleCatalog->Get(bRefresh);
				  const FString LowerQuery = Query.ToLower();
				  TArray<TPair<int32, const FDeviceExplorerConsoleEntry*>> Matches;
				  Matches.Reserve(Catalog.Num());
				  for (const FDeviceExplorerConsoleEntry& Entry : Catalog)
				  {
					  if (!SourceFilter.IsEmpty() && SourceFilter != FDeviceExplorerConsoleCatalog::SourceToString(Entry.Source))
					  {
						  continue;
					  }
					  if (!KindFilter.IsEmpty() && KindFilter != (Entry.bIsVariable ? TEXT("variable") : TEXT("command")))
					  {
						  continue;
					  }
					  const int32 Rank = RankConsoleMatch(Entry, LowerQuery, bSearchHelp);
					  if (Rank != INDEX_NONE)
					  {
						  Matches.Emplace(Rank, &Entry);
					  }
				  }

				  // The catalog is already name-sorted, so a stable sort keeps names ordered inside each rank.
				  Matches.StableSort(
					  [](const TPair<int32, const FDeviceExplorerConsoleEntry*>& Left, const TPair<int32, const FDeviceExplorerConsoleEntry*>& Right)
					  {
						  return Left.Key < Right.Key;
					  });

				  const int32 EmitCount = FMath::Min(Matches.Num(), Limit);
				  TArray<TSharedPtr<FJsonValue>> Entries;
				  Entries.Reserve(EmitCount);
				  for (int32 Index = 0; Index < EmitCount; ++Index)
				  {
					  const FDeviceExplorerConsoleEntry& Entry = *Matches[Index].Value;
					  TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
					  Json->SetStringField(TEXT("name"), Entry.Name);
					  Json->SetStringField(TEXT("type"), Entry.bIsVariable ? TEXT("variable") : TEXT("command"));
					  Json->SetStringField(TEXT("source"), FDeviceExplorerConsoleCatalog::SourceToString(Entry.Source));
					  Json->SetStringField(TEXT("help"), Entry.Help);
					  Json->SetStringField(TEXT("arguments"), Entry.Arguments);
					  Json->SetStringField(TEXT("companion"), Entry.Companion);
					  Json->SetBoolField(TEXT("read_only"), Entry.bReadOnly);
					  Json->SetBoolField(TEXT("cheat"), Entry.bCheat);

					  // A show flag reports its ShowFlag.X override, so the dashboard can show state, not just a toggle.
					  const FString& ValueSource = Entry.bIsVariable ? Entry.Name : Entry.Companion;
					  FString Value;
					  if (!ValueSource.IsEmpty())
					  {
						  if (const IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(*ValueSource, false))
						  {
							  Value = Variable->GetString();
							  Value.LeftInline(4096);
						  }
					  }
					  Json->SetStringField(TEXT("value"), Value);
					  Entries.Add(MakeShared<FJsonValueObject>(Json));
				  }

				  TSharedRef<FJsonObject> Result = MakeResponse(TEXT("console_objects_result"), RequestId);
				  Result->SetStringField(TEXT("query"), Query);
				  Result->SetNumberField(TEXT("total"), Matches.Num());
				  Result->SetNumberField(TEXT("catalog_total"), Catalog.Num());
				  Result->SetBoolField(TEXT("truncated"), Matches.Num() > Entries.Num());
				  Result->SetArrayField(TEXT("entries"), MoveTemp(Entries));
				  SendJson(Result);
			  });
}

void FDeviceExplorerModule::ListFiles(const TSharedPtr<FJsonObject>& Message)
{
	FString RequestId;
	FString RootName = TEXT("saved");
	FString RelativePath;
	if (!Message->TryGetStringField(TEXT("request_id"), RequestId))
	{
		return;
	}
	Message->TryGetStringField(TEXT("root"), RootName);
	Message->TryGetStringField(TEXT("path"), RelativePath);

	FString FullPath;
	TSharedRef<FJsonObject> Result = MakeResponse(TEXT("file_list_result"), RequestId);
	Result->SetStringField(TEXT("root"), RootName);
	Result->SetStringField(TEXT("path"), NormalizeSavedRelativePath(RelativePath));
	TArray<TSharedPtr<FJsonValue>> Entries;

	if (!ResolveRegisteredPath(FName(*RootName), RelativePath, false, FullPath))
	{
		Result->SetStringField(TEXT("error"), TEXT("Unknown file root or path is outside its root"));
		Result->SetArrayField(TEXT("entries"), MoveTemp(Entries));
		SendJson(Result);
		return;
	}

	IFileManager& FileManager = IFileManager::Get();
	const FString NormalizedParent = NormalizeSavedRelativePath(RelativePath);
	const bool bEnumerated = FileManager.IterateDirectoryStat(*FullPath,
	                                                          [&Entries, &NormalizedParent](const TCHAR* Filename, const FFileStatData& Stat)
	                                                          {
																  const FString Name = FPaths::GetCleanFilename(Filename);
																  const FString ChildPath = NormalizedParent.IsEmpty() ? Name : FPaths::Combine(NormalizedParent, Name);
																  TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
																  Entry->SetStringField(TEXT("name"), Name);
																  Entry->SetStringField(TEXT("path"), ChildPath);
																  Entry->SetBoolField(TEXT("is_directory"), Stat.bIsDirectory);
																  Entry->SetNumberField(TEXT("size"), static_cast<double>(Stat.FileSize));
																  Entry->SetStringField(TEXT("modified"), Stat.ModificationTime.ToIso8601());
																  Entries.Add(MakeShared<FJsonValueObject>(Entry));
																  return true;
															  });
	Result->SetStringField(TEXT("error"), bEnumerated ? FString() : TEXT("Directory does not exist or is unreadable"));
	Result->SetArrayField(TEXT("entries"), MoveTemp(Entries));
	SendJson(Result);
}

void FDeviceExplorerModule::GetModuleData(const TSharedPtr<FJsonObject>& Message)
{
	FString RequestId;
	FString ModuleName;
	if (!Message->TryGetStringField(TEXT("request_id"), RequestId) || !Message->TryGetStringField(TEXT("module"), ModuleName))
	{
		return;
	}

	AsyncTask(ENamedThreads::GameThread,
	          [this, RequestId = MoveTemp(RequestId), ModuleName = MoveTemp(ModuleName)]()
	          {
				  TSharedRef<FJsonObject> Response = MakeResponse(TEXT("module_result"), RequestId);
				  Response->SetStringField(TEXT("module"), ModuleName);

				  FDeviceExplorerDataModuleDescriptor Descriptor;
				  if (!FDeviceExplorerCoreModule::Get().FindDataModule(FName(*ModuleName), Descriptor))
				  {
					  Response->SetBoolField(TEXT("success"), false);
					  Response->SetStringField(TEXT("error"), TEXT("Unknown data module"));
					  SendJson(Response);
					  return;
				  }

				  const FDeviceExplorerModuleResult ModuleResult = Descriptor.DataProvider();
				  Response->SetBoolField(TEXT("success"), ModuleResult.bSuccess);
				  Response->SetStringField(TEXT("error"), ModuleResult.Error);
				  if (ModuleResult.Data.IsValid())
				  {
					  Response->SetObjectField(TEXT("data"), ModuleResult.Data);
				  }
				  if (ModuleResult.Values.IsValid())
				  {
					  Response->SetObjectField(TEXT("values"), ModuleResult.Values);
				  }
				  SendJson(Response);
			  });
}

void FDeviceExplorerModule::InvokeModuleAction(const TSharedPtr<FJsonObject>& Message)
{
	FString RequestId;
	FString ModuleName;
	FString ActionName;
	if (!Message->TryGetStringField(TEXT("request_id"), RequestId) || !Message->TryGetStringField(TEXT("module"), ModuleName) ||
	    !Message->TryGetStringField(TEXT("action"), ActionName))
	{
		return;
	}
	const TSharedPtr<FJsonObject>* ParametersField = nullptr;
	const TSharedPtr<FJsonObject> Parameters =
		Message->TryGetObjectField(TEXT("parameters"), ParametersField) && ParametersField != nullptr ? *ParametersField : MakeShared<FJsonObject>();

	AsyncTask(ENamedThreads::GameThread,
	          [this,
	           RequestId = MoveTemp(RequestId),
	           ModuleName = MoveTemp(ModuleName),
	           ActionName = MoveTemp(ActionName),
	           Parameters]()
	          {
				  TSharedRef<FJsonObject> Response = MakeResponse(TEXT("module_result"), RequestId);
				  Response->SetStringField(TEXT("module"), ModuleName);
				  Response->SetStringField(TEXT("action"), ActionName);

				  FDeviceExplorerDataModuleDescriptor Descriptor;
				  if (!FDeviceExplorerCoreModule::Get().FindDataModule(FName(*ModuleName), Descriptor))
				  {
					  Response->SetBoolField(TEXT("success"), false);
					  Response->SetStringField(TEXT("error"), TEXT("Unknown data module"));
					  SendJson(Response);
					  return;
				  }

				  const FDeviceExplorerModuleActionDescriptor* Action = Descriptor.Actions.FindByPredicate(
					  [&ActionName](const FDeviceExplorerModuleActionDescriptor& Candidate) { return Candidate.Name == FName(*ActionName); });
				  if (Action == nullptr)
				  {
					  Response->SetBoolField(TEXT("success"), false);
					  Response->SetStringField(TEXT("error"), TEXT("Unknown module action"));
					  SendJson(Response);
					  return;
				  }

				  const FDeviceExplorerModuleResult ModuleResult = Action->Handler(Parameters);
				  Response->SetBoolField(TEXT("success"), ModuleResult.bSuccess);
				  Response->SetStringField(TEXT("error"), ModuleResult.Error);
				  if (ModuleResult.Data.IsValid())
				  {
					  Response->SetObjectField(TEXT("data"), ModuleResult.Data);
				  }
				  if (ModuleResult.Values.IsValid())
				  {
					  Response->SetObjectField(TEXT("values"), ModuleResult.Values);
				  }
				  SendJson(Response);
			  });
}

void FDeviceExplorerModule::UploadFile(const TSharedPtr<FJsonObject>& Message)
{
	FString TransferId;
	FString RootName = TEXT("saved");
	FString RelativePath;
	FString UploadURL;
	if (!Message->TryGetStringField(TEXT("transfer_id"), TransferId) || !Message->TryGetStringField(TEXT("path"), RelativePath) ||
	    !Message->TryGetStringField(TEXT("upload_url"), UploadURL))
	{
		return;
	}
	Message->TryGetStringField(TEXT("root"), RootName);
	bool bArchive = false;
	Message->TryGetBoolField(TEXT("archive"), bArchive);

	FString FullPath;
	if (!ResolveRegisteredPath(FName(*RootName), RelativePath, true, FullPath))
	{
		SendTransferFailure(TransferId, TEXT("Unknown file root or path is outside its root"));
		return;
	}

	if (bArchive ? !IFileManager::Get().DirectoryExists(*FullPath) : !IFileManager::Get().FileExists(*FullPath))
	{
		SendTransferFailure(TransferId, bArchive ? TEXT("Directory does not exist") : TEXT("File does not exist"));
		return;
	}

	// Archiving a multi-gigabyte folder can take a long time; keep it off the game thread and the socket's
	// own message-handling thread so heartbeats and other commands keep flowing while it runs.
	Async(EAsyncExecution::ThreadPool,
	      [this, TransferId, FullPath, UploadURL, bArchive]()
	      {
		      FString UploadPath = FullPath;
		      FString ArchivePath;
		      if (bArchive)
		      {
			      ArchivePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DeviceExplorer"), TransferId + TEXT(".zip"));
			      if (!WriteDirectoryArchive(FullPath, ArchivePath))
			      {
				      IFileManager::Get().Delete(*ArchivePath, false, false, true);
				      AsyncTask(ENamedThreads::GameThread, [this, TransferId]() { SendTransferFailure(TransferId, TEXT("Cannot archive this directory")); });
				      return;
			      }
			      UploadPath = ArchivePath;
		      }

		      AsyncTask(ENamedThreads::GameThread,
		                [this, TransferId, UploadURL, UploadPath, ArchivePath]() { StartUpload(TransferId, UploadURL, UploadPath, ArchivePath); });
	      });
}

void FDeviceExplorerModule::StartUpload(const FString& TransferId, const FString& UploadURL, const FString& UploadPath, const FString& ArchivePath)
{
	const TSharedRef<IHttpRequest> Request = FHttpModule::Get().CreateRequest();
	Request->SetURL(UploadURL);
	Request->SetVerb(TEXT("PUT"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/octet-stream"));
	Request->SetContentAsStreamedFile(UploadPath);
	Request->SetTimeout(300.0f);
	Request->OnProcessRequestComplete().BindLambda(
		[this, TransferId, ArchivePath](FHttpRequestPtr CompletedRequest, FHttpResponsePtr Response, bool bConnectedSuccessfully)
		{
			(void) CompletedRequest;
			if (!ArchivePath.IsEmpty())
			{
				IFileManager::Get().Delete(*ArchivePath, false, false, true);
			}

			const bool bSuccess = bConnectedSuccessfully && Response.IsValid() && EHttpResponseCodes::IsOk(Response->GetResponseCode());
			if (bSuccess)
			{
				return;
			}
			SendTransferFailure(TransferId, Response.IsValid() ? FString::Printf(TEXT("HTTP %d"), Response->GetResponseCode()) : TEXT("HTTP upload failed"));
		});
	if (!Request->ProcessRequest())
	{
		if (!ArchivePath.IsEmpty())
		{
			IFileManager::Get().Delete(*ArchivePath, false, false, true);
		}
		SendTransferFailure(TransferId, TEXT("HTTP request did not start"));
	}
}

void FDeviceExplorerModule::SendTransferFailure(const FString& TransferId, const FString& Error)
{
	TSharedRef<FJsonObject> Result = MakeShared<FJsonObject>();
	Result->SetStringField(TEXT("type"), TEXT("transfer_result"));
	Result->SetStringField(TEXT("transfer_id"), TransferId);
	Result->SetBoolField(TEXT("success"), false);
	Result->SetStringField(TEXT("error"), Error);
	SendJson(Result);
}

void FDeviceExplorerModule::SendJson(const TSharedRef<FJsonObject>& Message)
{
	if (!bAuthenticated)
	{
		return;
	}
	SendUnauthenticatedJson(Message);
}

void FDeviceExplorerModule::SendUnauthenticatedJson(const TSharedRef<FJsonObject>& Message)
{
	if (!Transport || !Transport->IsConnected())
	{
		return;
	}

	FString Serialized;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Serialized);
	if (FJsonSerializer::Serialize(Message, Writer))
	{
		Transport->SendText(Serialized);
	}
}

IMPLEMENT_MODULE(FDeviceExplorerModule, DeviceExplorer)
