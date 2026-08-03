#pragma once

#include "CoreMinimal.h"
#include "DeviceExplorerProtocol.h"

class FJsonObject;

namespace DeviceExplorer
{
inline const FName LogsCapability(TEXT("logs"));
inline const FName ConsoleCapability(TEXT("console"));
inline const FName FilesCapability(TEXT("files"));
inline const FName MetricsCapability(TEXT("metrics"));
inline const FName ModulesCapability(TEXT("modules"));
inline const FName TraceCapability(TEXT("trace"));
inline const FName SetFieldsAction(TEXT("__set"));
}

enum class EDeviceExplorerWidget : uint8
{
	Text,
	Bool,
	Number,
	Enum,
	String,
	Badge,
	Meter,
	Button,
	Json,
	Series,
	Status,
	Table,
	Textarea,
	Vector,
	Color,
	Path,
	Artifact,
	Flags,
	ActionForm
};

enum class EDeviceExplorerApply : uint8
{
	Instant,
	Manual
};

enum class EDeviceExplorerSectionStyle : uint8
{
	Default,
	Stats,
	Toolbar,
	Settings,
	Hero
};

enum class EDeviceExplorerNumberDisplay : uint8
{
	Auto,
	Input,
	Slider,
	SliderAndInput
};

enum class EDeviceExplorerEnumDisplay : uint8
{
	Select,
	Segmented
};

enum class EDeviceExplorerActionStyle : uint8
{
	Default,
	Primary,
	Danger
};

enum class EDeviceExplorerInputType : uint8
{
	String,
	Number
};

enum class EDeviceExplorerStatusTone : uint8
{
	Idle,
	Active,
	Warn,
	Error
};

struct FDeviceExplorerStatus
{
	FString Label;
	EDeviceExplorerStatusTone Tone = EDeviceExplorerStatusTone::Idle;
};

struct FDeviceExplorerArtifact
{
	FString Name;
	FString Size;
	FString Age;
};

struct FDeviceExplorerModuleActionInput
{
	FName Name;
	FText DisplayName;
	FString Type;
	FString DefaultValue;
};

struct FDeviceExplorerFieldDescriptor
{
	FName Name;
	FText DisplayName;
	FString Description;
	EDeviceExplorerWidget Widget = EDeviceExplorerWidget::Text;
	bool bReadOnly = true;
	bool bRequiresConfirmation = false;
	bool bSeries = false;
	FName Action;
	FString ActionLabel;
	EDeviceExplorerActionStyle ActionStyle = EDeviceExplorerActionStyle::Default;
	EDeviceExplorerNumberDisplay NumberDisplay = EDeviceExplorerNumberDisplay::Auto;
	EDeviceExplorerEnumDisplay EnumDisplay = EDeviceExplorerEnumDisplay::Select;
	FString Unit;
	TOptional<double> Min;
	TOptional<double> Max;
	TOptional<double> Step;
	TOptional<double> WarnAbove;
	TOptional<double> ErrorAbove;
	TArray<FString> Options;
	TArray<FString> Columns;
	TArray<FDeviceExplorerModuleActionInput> Inputs;
	int32 Rows = 0;
	int32 Span = 1;
};

struct FDeviceExplorerSectionDescriptor
{
	FName Name;
	FText DisplayName;
	FString Description;
	int32 Columns = 0;
	EDeviceExplorerApply Apply = EDeviceExplorerApply::Instant;
	EDeviceExplorerSectionStyle Style = EDeviceExplorerSectionStyle::Default;
	bool bCollapsible = false;
	bool bCollapsed = false;
	TArray<FDeviceExplorerFieldDescriptor> Fields;
};

struct FDeviceExplorerPageDescriptor
{
	FName Name;
	FText DisplayName;
	FString Description;
	FString Icon;
	TArray<FDeviceExplorerSectionDescriptor> Sections;
};

struct FDeviceExplorerCommandResult
{
	bool bSuccess = false;
	FString Output;
};

using FDeviceExplorerCommandHandler = TFunction<FDeviceExplorerCommandResult(const FString& Arguments)>;

struct FDeviceExplorerCommandDescriptor
{
	FName Owner;
	FName Name;
	FText DisplayName;
	FName Category;
	FString Command;
	FString Description;
	bool bRequiresConfirmation = false;
	FDeviceExplorerCommandHandler Handler;
};

struct FDeviceExplorerFileRootDescriptor
{
	FName Owner;
	FName Name;
	FText DisplayName;
	FString AbsolutePath;
	bool bAllowDownload = true;
	bool bAllowDirectoryTransfer = false;
};

struct FDeviceExplorerModuleResult
{
	bool bSuccess = true;
	FString Error;
	TSharedPtr<FJsonObject> Data;
	TSharedPtr<FJsonObject> Values;
};

using FDeviceExplorerModuleDataProvider = TFunction<FDeviceExplorerModuleResult()>;
using FDeviceExplorerModuleActionHandler = TFunction<FDeviceExplorerModuleResult(const TSharedPtr<FJsonObject>& Parameters)>;

struct FDeviceExplorerModuleActionDescriptor
{
	FName Name;
	FText DisplayName;
	FString Description;
	bool bRequiresConfirmation = false;
	TArray<FDeviceExplorerModuleActionInput> Inputs;
	FDeviceExplorerModuleActionHandler Handler;
};

struct FDeviceExplorerDataModuleDescriptor
{
	FName Owner;
	FName Name;
	FText DisplayName;
	FString Description;
	FString Icon;
	int32 RefreshIntervalMs = 0;
	FDeviceExplorerModuleDataProvider DataProvider;
	TArray<FDeviceExplorerModuleActionDescriptor> Actions;
	TArray<FDeviceExplorerPageDescriptor> Pages;
};

struct FDeviceExplorerWriteResult
{
	bool bSuccess = true;
	FString Error;

	static FDeviceExplorerWriteResult Success()
	{
		return {};
	}

	static FDeviceExplorerWriteResult Failure(FString InError)
	{
		FDeviceExplorerWriteResult Result;
		Result.bSuccess = false;
		Result.Error = MoveTemp(InError);
		return Result;
	}
};

struct FDeviceExplorerRegistrySnapshot
{
	TArray<FName> Capabilities;
	TArray<FDeviceExplorerCommandDescriptor> Commands;
	TArray<FDeviceExplorerFileRootDescriptor> FileRoots;
	TArray<FDeviceExplorerDataModuleDescriptor> DataModules;
};
