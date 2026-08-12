#include "DeviceExplorerDeviceIdentity.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"

namespace
{
constexpr int32 IdentitySchema = 1;

FString CurrentMachineMarker()
{
	const FString LoginId = FPlatformMisc::GetLoginId();
	return LoginId.IsEmpty() ? FString() : FMD5::HashAnsiString(*LoginId);
}

bool ParseUnsigned(const FString& Text, uint64& OutValue)
{
	if (Text.IsEmpty()) return false;
	TCHAR* End = nullptr;
	const uint64 Parsed = FCString::Strtoui64(*Text, &End, 10);
	if (End == nullptr || *End != TEXT('\0') || Parsed == 0) return false;
	OutValue = Parsed;
	return true;
}

TMap<FString, FString> ParseFields(const FString& Text)
{
	TMap<FString, FString> Fields;
	TArray<FString> Lines;
	Text.ParseIntoArrayLines(Lines, false);
	for (const FString& Line : Lines)
	{
		FString Name;
		FString Value;
		if (Line.Split(TEXT("="), &Name, &Value) && !Name.IsEmpty() && !Fields.Contains(Name))
		{
			Fields.Add(MoveTemp(Name), MoveTemp(Value));
		}
	}
	return Fields;
}
}    // namespace

FDeviceExplorerDeviceIdentityStore::FDeviceExplorerDeviceIdentityStore(FString InPath)
	: Path(MoveTemp(InPath))
{
}

void FDeviceExplorerDeviceIdentityStore::ResetIdentity()
{
	DeviceId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	MachineMarker = CurrentMachineMarker();
	NextDeviceSession = 1;
}

bool FDeviceExplorerDeviceIdentityStore::Load(FString& OutError)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path))
	{
		ResetIdentity();
		return Persist(OutError);
	}

	const TMap<FString, FString> Fields = ParseFields(Text);
	FGuid ParsedId;
	uint64 ParsedSession = 0;
	int32 ParsedSchema = 0;
	const FString* Schema = Fields.Find(TEXT("schema"));
	const FString* Id = Fields.Find(TEXT("device_id"));
	const FString* Session = Fields.Find(TEXT("next_device_session"));
	const FString* StoredMarker = Fields.Find(TEXT("machine_marker"));
	const FString CurrentMarker = CurrentMachineMarker();
	const bool MarkerMatches = CurrentMarker.IsEmpty() || (StoredMarker != nullptr && *StoredMarker == CurrentMarker);
	if (Schema == nullptr || !LexTryParseString(ParsedSchema, **Schema) || ParsedSchema != IdentitySchema ||
	    Id == nullptr || !FGuid::Parse(*Id, ParsedId) || Session == nullptr || !ParseUnsigned(*Session, ParsedSession) ||
	    !MarkerMatches)
	{
		ResetIdentity();
		return Persist(OutError);
	}

	DeviceId = ParsedId.ToString(EGuidFormats::DigitsWithHyphensLower);
	MachineMarker = StoredMarker == nullptr ? FString() : *StoredMarker;
	NextDeviceSession = ParsedSession;
	OutError.Reset();
	return true;
}

bool FDeviceExplorerDeviceIdentityStore::AllocateConnection(FDeviceExplorerConnectionIdentity& OutIdentity,
	                                                        FString& OutError)
{
	if (DeviceId.IsEmpty() || NextDeviceSession == 0 || NextDeviceSession == MAX_uint64)
	{
		OutError = TEXT("DeviceExplorer device session counter is exhausted or identity is not loaded");
		return false;
	}
	const uint64 AllocatedSession = NextDeviceSession;
	++NextDeviceSession;
	if (!Persist(OutError))
	{
		NextDeviceSession = AllocatedSession;
		return false;
	}
	OutIdentity.DeviceId = DeviceId;
	OutIdentity.DeviceSession = AllocatedSession;
	OutIdentity.ConnectionId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	return true;
}

bool FDeviceExplorerDeviceIdentityStore::AdvancePast(const uint64 LastKnownSession, FString& OutError)
{
	if (LastKnownSession == MAX_uint64)
	{
		OutError = TEXT("DeviceExplorer device session counter is exhausted");
		return false;
	}
	const uint64 Required = LastKnownSession + 1;
	if (NextDeviceSession >= Required)
	{
		OutError.Reset();
		return true;
	}
	const uint64 Previous = NextDeviceSession;
	NextDeviceSession = Required;
	if (!Persist(OutError))
	{
		NextDeviceSession = Previous;
		return false;
	}
	return true;
}

bool FDeviceExplorerDeviceIdentityStore::Persist(FString& OutError) const
{
	const FString Directory = FPaths::GetPath(Path);
	if (!IFileManager::Get().MakeDirectory(*Directory, true))
	{
		OutError = FString::Printf(TEXT("Cannot create DeviceExplorer identity directory: %s"), *Directory);
		return false;
	}
	const FString Text = FString::Printf(TEXT("schema=%d\ndevice_id=%s\nnext_device_session=%llu\nmachine_marker=%s\n"),
	                                     IdentitySchema, *DeviceId,
	                                     static_cast<unsigned long long>(NextDeviceSession), *MachineMarker);
	const FString Temporary = Path + TEXT(".") + FGuid::NewGuid().ToString(EGuidFormats::Digits) + TEXT(".tmp");
	if (!FFileHelper::SaveStringToFile(Text, *Temporary, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		OutError = TEXT("Cannot write the temporary DeviceExplorer identity file");
		return false;
	}
	if (!IFileManager::Get().Move(*Path, *Temporary, true, true, false, false))
	{
		IFileManager::Get().Delete(*Temporary, false, true);
		OutError = TEXT("Cannot atomically replace the DeviceExplorer identity file");
		return false;
	}
	OutError.Reset();
	return true;
}
