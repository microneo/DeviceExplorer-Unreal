#include "DeviceExplorerModuleBuilder.h"

#include "DeviceExplorerCoreModule.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "HAL/IConsoleManager.h"
#include "Misc/StringOutputDevice.h"

#if WITH_COREUOBJECT
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Object.h"
#include "UObject/UnrealType.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogDeviceExplorerModuleBuilder, Log, All);

namespace
{
TSharedPtr<FJsonValue> MakeStringArrayValue(const TArray<FString>& Values)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	Items.Reserve(Values.Num());
	for (const FString& Value : Values)
	{
		Items.Add(MakeShared<FJsonValueString>(Value));
	}
	return MakeShared<FJsonValueArray>(MoveTemp(Items));
}

bool ReadStringArray(const TSharedPtr<FJsonValue>& Value, TArray<FString>& OutValues, FString& OutError)
{
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Value.IsValid() || !Value->TryGetArray(Items))
	{
		OutError = TEXT("Expected an array of strings");
		return false;
	}
	for (const TSharedPtr<FJsonValue>& Item : *Items)
	{
		FString Text;
		if (!Item.IsValid() || !Item->TryGetString(Text))
		{
			OutError = TEXT("Expected an array of strings");
			return false;
		}
		OutValues.Add(MoveTemp(Text));
	}
	return true;
}

FString ToHexColor(const FColor& Color)
{
	return FString::Printf(TEXT("#%02X%02X%02X"), Color.R, Color.G, Color.B);
}

const TCHAR* ToWireString(EDeviceExplorerStatusTone Tone)
{
	switch (Tone)
	{
		case EDeviceExplorerStatusTone::Active: return TEXT("active");
		case EDeviceExplorerStatusTone::Warn: return TEXT("warn");
		case EDeviceExplorerStatusTone::Error: return TEXT("error");
		default: return TEXT("idle");
	}
}
}

FString FDeviceExplorerActionParameters::GetString(const TCHAR* Name, const FString& DefaultValue) const
{
	FString Value;
	return Values.IsValid() && Values->TryGetStringField(Name, Value) ? Value : DefaultValue;
}

double FDeviceExplorerActionParameters::GetNumber(const TCHAR* Name, double DefaultValue) const
{
	double Value = 0.0;
	return Values.IsValid() && Values->TryGetNumberField(Name, Value) ? Value : DefaultValue;
}

bool FDeviceExplorerActionParameters::GetBool(const TCHAR* Name, bool bDefaultValue) const
{
	bool bValue = false;
	return Values.IsValid() && Values->TryGetBoolField(Name, bValue) ? bValue : bDefaultValue;
}

FDeviceExplorerModuleBuilder::FDeviceExplorerModuleBuilder(FName InOwner, const TCHAR* InName, const TCHAR* InDisplayName)
	: Owner(InOwner)
	, Name(InName)
	, DisplayName(FText::FromString(InDisplayName))
	, Bindings(MakeShared<FBindings>())
{
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Description(const TCHAR* Text)
{
	DescriptionText = Text;
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Icon(const TCHAR* InIcon)
{
	IconText = InIcon;
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::RefreshMs(int32 Milliseconds)
{
	RefreshIntervalMs = Milliseconds;
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::FileRoot(const TCHAR* InName, const FString& AbsolutePath, const TCHAR* InDisplayName, bool bAllowDownload)
{
	FDeviceExplorerFileRootDescriptor Root;
	Root.Owner = Owner;
	Root.Name = FName(InName);
	Root.DisplayName = FText::FromString(InDisplayName != nullptr ? InDisplayName : InName);
	Root.AbsolutePath = AbsolutePath;
	Root.bAllowDownload = bAllowDownload;
	FileRoots.Add(MoveTemp(Root));
	return *this;
}

FDeviceExplorerModulePageBuilder FDeviceExplorerModuleBuilder::Page(const TCHAR* InName, const TCHAR* InDisplayName, const FDeviceExplorerPageOptions& Options)
{
	FDeviceExplorerPageDescriptor& NewPage = Pages.AddDefaulted_GetRef();
	NewPage.Name = FName(InName);
	NewPage.DisplayName = FText::FromString(InDisplayName != nullptr ? InDisplayName : InName);
	NewPage.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	NewPage.Icon = Options.Icon != nullptr ? Options.Icon : TEXT("");
	ActivePage = Pages.Num() - 1;
	ActiveSection = INDEX_NONE;
	return FDeviceExplorerModulePageBuilder(*this, ActivePage);
}

FDeviceExplorerModuleSectionBuilder FDeviceExplorerModuleBuilder::Section(const TCHAR* InName, const TCHAR* InDisplayName, const FDeviceExplorerSectionOptions& Options)
{
	FDeviceExplorerPageDescriptor& CurrentPage = EnsurePage();
	FDeviceExplorerSectionDescriptor& NewSection = CurrentPage.Sections.AddDefaulted_GetRef();
	NewSection.Name = FName(InName);
	NewSection.DisplayName = FText::FromString(InDisplayName != nullptr ? InDisplayName : InName);
	NewSection.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	NewSection.Columns = FMath::Max(0, Options.Columns);
	NewSection.Apply = Options.Apply;
	NewSection.Style = Options.Style;
	NewSection.bCollapsible = Options.bCollapsible;
	NewSection.bCollapsed = Options.bCollapsed;
	ActiveSection = CurrentPage.Sections.Num() - 1;
	return FDeviceExplorerModuleSectionBuilder(*this, ActivePage, ActiveSection);
}

FDeviceExplorerModuleSectionBuilder FDeviceExplorerModuleBuilder::Section(const TCHAR* InDisplayName, const FDeviceExplorerSectionOptions& Options)
{
	return Section(InDisplayName, InDisplayName, Options);
}

void FDeviceExplorerModuleBuilder::Activate(const int32 PageIndex, const int32 SectionIndex)
{
	check(Pages.IsValidIndex(PageIndex));
	check(Pages[PageIndex].Sections.IsValidIndex(SectionIndex));
	ActivePage = PageIndex;
	ActiveSection = SectionIndex;
}

FDeviceExplorerPageDescriptor& FDeviceExplorerModuleBuilder::EnsurePage()
{
	if (!Pages.IsValidIndex(ActivePage))
	{
		FDeviceExplorerPageDescriptor& DefaultPage = Pages.AddDefaulted_GetRef();
		DefaultPage.Name = TEXT("overview");
		DefaultPage.DisplayName = FText::FromString(TEXT("Overview"));
		ActivePage = Pages.Num() - 1;
		ActiveSection = INDEX_NONE;
	}
	return Pages[ActivePage];
}

FDeviceExplorerSectionDescriptor& FDeviceExplorerModuleBuilder::EnsureSection()
{
	FDeviceExplorerPageDescriptor& CurrentPage = EnsurePage();
	if (!CurrentPage.Sections.IsValidIndex(ActiveSection))
	{
		FDeviceExplorerSectionDescriptor& DefaultSection = CurrentPage.Sections.AddDefaulted_GetRef();
		DefaultSection.Name = TEXT("general");
		DefaultSection.DisplayName = FText::FromString(TEXT("General"));
		ActiveSection = CurrentPage.Sections.Num() - 1;
	}
	return CurrentPage.Sections[ActiveSection];
}

FDeviceExplorerFieldDescriptor& FDeviceExplorerModuleBuilder::AddField(const TCHAR* InName, const TCHAR* InDisplayName, EDeviceExplorerWidget Widget, bool bReadOnly)
{
	FDeviceExplorerFieldDescriptor& Field = EnsureSection().Fields.AddDefaulted_GetRef();
	Field.Name = FName(InName);
	Field.DisplayName = FText::FromString(InDisplayName);
	Field.Widget = Widget;
	Field.bReadOnly = bReadOnly;
	return Field;
}

void FDeviceExplorerModuleBuilder::AddBinding(FName BindingName, TFunction<TSharedPtr<FJsonValue>()> Reader, TFunction<bool(const TSharedPtr<FJsonValue>&, FString&)> Writer)
{
	Bindings->Entries.Add(BindingName.ToString().ToLower(), { MoveTemp(Reader), MoveTemp(Writer) });
}

void FDeviceExplorerModuleBuilder::ApplyNumberOptions(FDeviceExplorerFieldDescriptor& Field, const FDeviceExplorerNumberOptions& Options)
{
	constexpr double Lowest = TNumericLimits<double>::Lowest();
	constexpr double Highest = TNumericLimits<double>::Max();
	if (Options.Min > Lowest)
	{
		Field.Min = Options.Min;
	}
	if (Options.Max < Highest)
	{
		Field.Max = Options.Max;
	}
	if (Options.Step > 0.0)
	{
		Field.Step = Options.Step;
	}
	if (Options.Unit != nullptr)
	{
		Field.Unit = Options.Unit;
	}
	if (Options.Description != nullptr)
	{
		Field.Description = Options.Description;
	}
	if (Options.WarnAbove < Highest)
	{
		Field.WarnAbove = Options.WarnAbove;
	}
	if (Options.ErrorAbove < Highest)
	{
		Field.ErrorAbove = Options.ErrorAbove;
	}
	Field.NumberDisplay = Options.Display;
	Field.Span = Options.Span;
	Field.bSeries = Options.bSeries;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::ReadonlyBool(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<bool()> Getter, const FDeviceExplorerNumberOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Bool, true);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName), [Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Getter())); }, nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::ReadonlyNumber(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<double()> Getter, const FDeviceExplorerNumberOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Number, true);
	ApplyNumberOptions(Field, Options);
	AddBinding(FName(InName), [Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Getter())); }, nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::ReadonlyText(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FString()> Getter, const FDeviceExplorerNumberOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Text, true);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName), [Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Getter())); }, nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Badge(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<double()> Getter, const FDeviceExplorerNumberOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Badge, true);
	ApplyNumberOptions(Field, Options);
	AddBinding(FName(InName), [Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Getter())); }, nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::MeterImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<double()> Getter, TFunction<double()> MaxGetter)
{
	AddField(InName, InDisplayName, EDeviceExplorerWidget::Meter, true);
	AddBinding(FName(InName),
		[Getter, MaxGetter]()
		{
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetNumberField(TEXT("value"), Getter());
			Value->SetNumberField(TEXT("max"), MaxGetter());
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Value));
		},
		nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::SeriesSampleImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<double()> Getter, const FDeviceExplorerNumberOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Series, true);
	ApplyNumberOptions(Field, Options);
	AddBinding(FName(InName), [Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Getter())); }, nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::SeriesWindowImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<TArray<double>()> Getter, const FDeviceExplorerNumberOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Series, true);
	ApplyNumberOptions(Field, Options);
	AddBinding(FName(InName),
		[Getter]()
		{
			TArray<TSharedPtr<FJsonValue>> Samples;
			for (const double Sample : Getter())
			{
				Samples.Add(MakeShared<FJsonValueNumber>(Sample));
			}
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueArray>(MoveTemp(Samples)));
		},
		nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Status(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FDeviceExplorerStatus()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Status, true);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName),
		[Getter]()
		{
			const FDeviceExplorerStatus Status = Getter();
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetStringField(TEXT("label"), Status.Label);
			Value->SetStringField(TEXT("tone"), ToWireString(Status.Tone));
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Value));
		},
		nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Json(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<TSharedPtr<FJsonObject>()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Json, true);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName),
		[Getter]()
		{
			const TSharedPtr<FJsonObject> Value = Getter();
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Value.IsValid() ? Value.ToSharedRef() : MakeShared<FJsonObject>()));
		},
		nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Table(const TCHAR* InName, const TCHAR* InDisplayName, TArray<FString> Columns, TFunction<TArray<TArray<FString>>()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Table, true);
	Field.Columns = MoveTemp(Columns);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName),
		[Getter, ColumnNames = Field.Columns]()
		{
			TArray<TSharedPtr<FJsonValue>> Rows;
			for (const TArray<FString>& Cells : Getter())
			{
				TSharedRef<FJsonObject> Row = MakeShared<FJsonObject>();
				for (int32 Index = 0; Index < ColumnNames.Num(); ++Index)
				{
					Row->SetStringField(ColumnNames[Index], Cells.IsValidIndex(Index) ? Cells[Index] : FString());
				}
				Rows.Add(MakeShared<FJsonValueObject>(Row));
			}
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueArray>(MoveTemp(Rows)));
		},
		nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Artifact(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<TArray<FDeviceExplorerArtifact>()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Artifact, true);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName),
		[Getter]()
		{
			TArray<TSharedPtr<FJsonValue>> Entries;
			for (const FDeviceExplorerArtifact& Artifact : Getter())
			{
				TSharedRef<FJsonObject> Entry = MakeShared<FJsonObject>();
				Entry->SetStringField(TEXT("name"), Artifact.Name);
				if (!Artifact.Size.IsEmpty())
				{
					Entry->SetStringField(TEXT("size"), Artifact.Size);
				}
				if (!Artifact.Age.IsEmpty())
				{
					Entry->SetStringField(TEXT("age"), Artifact.Age);
				}
				Entries.Add(MakeShared<FJsonValueObject>(Entry));
			}
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueArray>(MoveTemp(Entries)));
		},
		nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Path(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FString()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Path, true);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName), [Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Getter())); }, nullptr);
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::ToggleImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<bool()> Getter, TFunction<FDeviceExplorerWriteResult(bool)> Setter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Bool, false);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName),
		[Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueBoolean>(Getter())); },
		[Setter](const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			bool BoolValue = false;
			if (!Value.IsValid() || !Value->TryGetBool(BoolValue))
			{
				OutError = TEXT("Expected a boolean value");
				return false;
			}
			const FDeviceExplorerWriteResult Result = Setter(BoolValue);
			if (!Result.bSuccess)
			{
				OutError = Result.Error;
				return false;
			}
			return true;
		});
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::NumberImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<double()> Getter, TFunction<FDeviceExplorerWriteResult(double)> Setter, const FDeviceExplorerNumberOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Number, false);
	ApplyNumberOptions(Field, Options);
	AddBinding(FName(InName),
		[Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueNumber>(Getter())); },
		[Setter](const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			double NumberValue = 0.0;
			if (!Value.IsValid() || !Value->TryGetNumber(NumberValue))
			{
				OutError = TEXT("Expected a number");
				return false;
			}
			const FDeviceExplorerWriteResult Result = Setter(NumberValue);
			OutError = Result.Error;
			return Result.bSuccess;
		});
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::StringImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FString()> Getter, TFunction<FDeviceExplorerWriteResult(const FString&)> Setter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::String, false);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName),
		[Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Getter())); },
		[Setter](const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = TEXT("Expected a string");
				return false;
			}
			const FDeviceExplorerWriteResult Result = Setter(StringValue);
			OutError = Result.Error;
			return Result.bSuccess;
		});
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::TextImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FString()> Getter, TFunction<FDeviceExplorerWriteResult(const FString&)> Setter, const FDeviceExplorerTextOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Textarea, false);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	Field.Rows = Options.Rows;
	AddBinding(FName(InName),
		[Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Getter())); },
		[Setter](const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = TEXT("Expected a string");
				return false;
			}
			const FDeviceExplorerWriteResult Result = Setter(StringValue);
			OutError = Result.Error;
			return Result.bSuccess;
		});
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::VectorImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FVector()> Getter, TFunction<FDeviceExplorerWriteResult(const FVector&)> Setter, const FDeviceExplorerVectorOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Vector, false);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	if (Options.Step > 0.0)
	{
		Field.Step = Options.Step;
	}
	AddBinding(FName(InName),
		[Getter]()
		{
			const FVector Vector = Getter();
			TSharedRef<FJsonObject> Value = MakeShared<FJsonObject>();
			Value->SetNumberField(TEXT("x"), Vector.X);
			Value->SetNumberField(TEXT("y"), Vector.Y);
			Value->SetNumberField(TEXT("z"), Vector.Z);
			return TSharedPtr<FJsonValue>(MakeShared<FJsonValueObject>(Value));
		},
		[Setter](const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			const TSharedPtr<FJsonObject>* Object = nullptr;
			if (!Value.IsValid() || !Value->TryGetObject(Object))
			{
				OutError = TEXT("Expected x, y and z components");
				return false;
			}
			FVector Vector = FVector::ZeroVector;
			if (!(*Object)->TryGetNumberField(TEXT("x"), Vector.X) || !(*Object)->TryGetNumberField(TEXT("y"), Vector.Y) || !(*Object)->TryGetNumberField(TEXT("z"), Vector.Z))
			{
				OutError = TEXT("Expected x, y and z components");
				return false;
			}
			const FDeviceExplorerWriteResult Result = Setter(Vector);
			OutError = Result.Error;
			return Result.bSuccess;
		});
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::ColorImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FColor()> Getter, TFunction<FDeviceExplorerWriteResult(const FColor&)> Setter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Color, false);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName),
		[Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(ToHexColor(Getter()))); },
		[Setter](const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			FString StringValue;
			const FColor Color = Value.IsValid() && Value->TryGetString(StringValue) ? FColor::FromHex(StringValue) : FColor(ForceInitToZero);
			// FromHex zeroes the whole color on a malformed string; #RRGGBB always parses opaque.
			if (Color.A == 0)
			{
				OutError = TEXT("Expected an #RRGGBB color");
				return false;
			}
			const FDeviceExplorerWriteResult Result = Setter(Color);
			OutError = Result.Error;
			return Result.bSuccess;
		});
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::EnumImpl(const TCHAR* InName, const TCHAR* InDisplayName, TArray<FString> Values, TFunction<FString()> Getter, TFunction<FDeviceExplorerWriteResult(const FString&)> Setter, const FDeviceExplorerEnumOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Enum, false);
	Field.Options = MoveTemp(Values);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	Field.EnumDisplay = Options.Display;
	AddBinding(FName(InName),
		[Getter]() { return TSharedPtr<FJsonValue>(MakeShared<FJsonValueString>(Getter())); },
		[Setter](const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			FString StringValue;
			if (!Value.IsValid() || !Value->TryGetString(StringValue))
			{
				OutError = TEXT("Expected a string");
				return false;
			}
			const FDeviceExplorerWriteResult Result = Setter(StringValue);
			OutError = Result.Error;
			return Result.bSuccess;
		});
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::FlagsImpl(const TCHAR* InName, const TCHAR* InDisplayName, TArray<FString> Values, TFunction<TArray<FString>()> Getter, TFunction<FDeviceExplorerWriteResult(const TArray<FString>&)> Setter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Flags, false);
	Field.Options = MoveTemp(Values);
	Field.Description = Options.Description != nullptr ? Options.Description : TEXT("");
	Field.Span = Options.Span;
	AddBinding(FName(InName),
		[Getter]() { return MakeStringArrayValue(Getter()); },
		[Setter](const TSharedPtr<FJsonValue>& Value, FString& OutError)
		{
			TArray<FString> Selected;
			if (!ReadStringArray(Value, Selected, OutError))
			{
				return false;
			}
			const FDeviceExplorerWriteResult Result = Setter(Selected);
			OutError = Result.Error;
			return Result.bSuccess;
		});
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::ActionImpl(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FDeviceExplorerModuleResult()> Handler, const FDeviceExplorerActionOptions& Options)
{
	const FName ActionName(*(FString(TEXT("__btn_")) + InName));

	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Button, true);
	Field.bRequiresConfirmation = Options.bRequiresConfirmation;
	Field.Action = ActionName;
	Field.ActionStyle = Options.Style;
	Field.Span = Options.Span;
	if (Options.Description != nullptr)
	{
		Field.Description = Options.Description;
	}

	FDeviceExplorerModuleActionDescriptor ActionDescriptor;
	ActionDescriptor.Name = ActionName;
	ActionDescriptor.DisplayName = FText::FromString(InDisplayName);
	if (Options.Description != nullptr)
	{
		ActionDescriptor.Description = Options.Description;
	}
	ActionDescriptor.bRequiresConfirmation = Options.bRequiresConfirmation;
	ActionDescriptor.Handler = [Handler](const TSharedPtr<FJsonObject>&)
	{
		return Handler();
	};
	Actions.Add(MoveTemp(ActionDescriptor));
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::ActionFormImpl(const TCHAR* InName, const TCHAR* InDisplayName, TArray<FDeviceExplorerActionInput> Inputs, TFunction<FDeviceExplorerModuleResult(const FDeviceExplorerActionParameters&)> Handler, const FDeviceExplorerActionOptions& Options)
{
	const FName ActionName(*(FString(TEXT("__btn_")) + InName));

	TArray<FDeviceExplorerModuleActionInput> InputDescriptors;
	InputDescriptors.Reserve(Inputs.Num());
	for (const FDeviceExplorerActionInput& Input : Inputs)
	{
		FDeviceExplorerModuleActionInput& InputDescriptor = InputDescriptors.AddDefaulted_GetRef();
		InputDescriptor.Name = FName(Input.Name);
		InputDescriptor.DisplayName = FText::FromString(Input.DisplayName != nullptr ? Input.DisplayName : Input.Name);
		InputDescriptor.Type = Input.Type == EDeviceExplorerInputType::Number ? TEXT("number") : TEXT("string");
		InputDescriptor.DefaultValue = Input.DefaultValue != nullptr ? Input.DefaultValue : TEXT("");
	}

	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::ActionForm, true);
	Field.bRequiresConfirmation = Options.bRequiresConfirmation;
	Field.Action = ActionName;
	Field.ActionStyle = Options.Style;
	Field.ActionLabel = Options.ActionLabel != nullptr ? Options.ActionLabel : InDisplayName;
	Field.Span = Options.Span;
	Field.Inputs = InputDescriptors;
	if (Options.Description != nullptr)
	{
		Field.Description = Options.Description;
	}

	FDeviceExplorerModuleActionDescriptor ActionDescriptor;
	ActionDescriptor.Name = ActionName;
	ActionDescriptor.DisplayName = FText::FromString(InDisplayName);
	if (Options.Description != nullptr)
	{
		ActionDescriptor.Description = Options.Description;
	}
	ActionDescriptor.bRequiresConfirmation = Options.bRequiresConfirmation;
	ActionDescriptor.Inputs = MoveTemp(InputDescriptors);
	ActionDescriptor.Handler = [Handler](const TSharedPtr<FJsonObject>& Parameters)
	{
		return Handler(FDeviceExplorerActionParameters{ Parameters });
	};
	Actions.Add(MoveTemp(ActionDescriptor));
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Command(const TCHAR* ConsoleCommand, const TCHAR* InDisplayName, const TCHAR* Description)
{
	const FString FieldId = FString(ConsoleCommand).Replace(TEXT("."), TEXT("_"));
	const FName ActionName(*(TEXT("__btn_") + FieldId));

	FDeviceExplorerFieldDescriptor& Field = AddField(*FieldId, InDisplayName, EDeviceExplorerWidget::Button, true);
	Field.Action = ActionName;
	Field.Description = Description;

	FDeviceExplorerModuleActionDescriptor ActionDescriptor;
	ActionDescriptor.Name = ActionName;
	ActionDescriptor.DisplayName = FText::FromString(InDisplayName);
	ActionDescriptor.Description = Description;
	const FString ConsoleCommandCopy(ConsoleCommand);
	ActionDescriptor.Handler = [ConsoleCommandCopy](const TSharedPtr<FJsonObject>&)
	{
		FStringOutputDevice Output;
		FDeviceExplorerModuleResult Result;
		Result.bSuccess = IConsoleManager::Get().ProcessUserConsoleInput(*ConsoleCommandCopy, Output, nullptr);
		if (!Result.bSuccess)
		{
			Result.Error = Output.IsEmpty() ? FString::Printf(TEXT("Command failed: %s"), *ConsoleCommandCopy) : FString(Output);
		}
		return Result;
	};
	Actions.Add(MoveTemp(ActionDescriptor));
	return *this;
}

#if WITH_COREUOBJECT
void FDeviceExplorerModuleBuilder::NotifyPropertyChanged(UObject* Object, FProperty* Property, bool bPersist)
{
#if WITH_EDITOR
	FPropertyChangedEvent ChangeEvent(Property);
	Object->PostEditChangeProperty(ChangeEvent);
#endif
	if (bPersist)
	{
		Object->SaveConfig();
	}
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Object(UObject* InObject, TArray<FName> PropertyNames, bool bPersist, const TCHAR* SectionPrefix, const FDeviceExplorerSectionOptions& SectionOptions)
{
	if (InObject == nullptr)
	{
		return *this;
	}

	const TWeakObjectPtr<UObject> WeakObject(InObject);
	FDeviceExplorerSectionOptions CategoryOptions = SectionOptions;
	FString LastCategory;
	bool bHasSection = false;

	for (TFieldIterator<FProperty> It(InObject->GetClass()); It; ++It)
	{
		FProperty* Property = *It;
		if (!Property->HasAnyPropertyFlags(CPF_Edit) || (!PropertyNames.IsEmpty() && !PropertyNames.Contains(Property->GetFName())))
		{
			continue;
		}

		// FField metadata is WITH_METADATA-only (editor builds); packaged builds fall back to raw names.
#if WITH_METADATA
		const FString Category = Property->GetMetaData(TEXT("Category"));
		const FString Tooltip = Property->GetMetaData(TEXT("ToolTip"));
		const FString MetaDisplayName = Property->GetMetaData(TEXT("DisplayName"));
#else
		const FString Category;
		const FString Tooltip;
		const FString MetaDisplayName;
#endif
		const FString Label = MetaDisplayName.IsEmpty() ? FName::NameToDisplayString(Property->GetName(), Property->IsA<FBoolProperty>()) : MetaDisplayName;
		if (!bHasSection || Category != LastCategory)
		{
			const FString SectionId = Category.IsEmpty() ? TEXT("General") : Category;
			FString SectionLabel = SectionId;
			if (SectionPrefix != nullptr)
			{
				SectionLabel = Category.IsEmpty() ? FString(SectionPrefix) : FString::Printf(TEXT("%s · %s"), SectionPrefix, *Category);
			}
			Section(*SectionId, CategoryOptions);
			EnsureSection().DisplayName = FText::FromString(SectionLabel);
			// A description documents the object as a whole, not each category it splits into.
			CategoryOptions.Description = nullptr;
			LastCategory = Category;
			bHasSection = true;
		}

		const FString PropertyId = Property->GetName();
		// VisibleAnywhere carries CPF_Edit too, so it reaches here; it must not get a writer.
		const bool bEditConst = Property->HasAnyPropertyFlags(CPF_EditConst);

		FNumericProperty* UnderlyingNumeric = nullptr;
		UEnum* PropertyEnum = nullptr;
		if (FEnumProperty* EnumProperty = CastField<FEnumProperty>(Property))
		{
			UnderlyingNumeric = EnumProperty->GetUnderlyingProperty();
			PropertyEnum = EnumProperty->GetEnum();
		}
		else if (FByteProperty* ByteProperty = CastField<FByteProperty>(Property); ByteProperty != nullptr && ByteProperty->Enum != nullptr)
		{
			UnderlyingNumeric = ByteProperty;
			PropertyEnum = ByteProperty->Enum;
		}

		if (PropertyEnum != nullptr)
		{
			auto GetEnumValue = [WeakObject, Property, UnderlyingNumeric, PropertyEnum]() -> FString
			{
				UObject* Obj = WeakObject.Get();
				if (Obj == nullptr)
				{
					return FString();
				}
				const int64 Value = UnderlyingNumeric->GetSignedIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Obj));
				return PropertyEnum->GetNameStringByValue(Value);
			};

			if (bEditConst)
			{
				Readonly(*PropertyId, *Label, MoveTemp(GetEnumValue));
			}
			else
			{
				TArray<FString> EnumOptions;
				for (int32 Index = 0; Index < PropertyEnum->NumEnums() - 1; ++Index)
				{
					EnumOptions.Add(PropertyEnum->GetNameStringByIndex(Index));
				}
				Enum(*PropertyId, *Label, MoveTemp(EnumOptions), MoveTemp(GetEnumValue),
					[WeakObject, Property, UnderlyingNumeric, PropertyEnum, bPersist](const FString& NewValue) -> bool
					{
						UObject* Obj = WeakObject.Get();
						if (Obj == nullptr)
						{
							return false;
						}
						const int64 Value = PropertyEnum->GetValueByNameString(NewValue);
						if (Value == INDEX_NONE)
						{
							return false;
						}
						UnderlyingNumeric->SetIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Obj), Value);
						NotifyPropertyChanged(Obj, Property, bPersist);
						return true;
					});
			}
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}

		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			auto GetBoolValue = [WeakObject, BoolProperty]() -> bool
			{
				UObject* Obj = WeakObject.Get();
				return Obj != nullptr && BoolProperty->GetPropertyValue_InContainer(Obj);
			};

			if (bEditConst)
			{
				Readonly(*PropertyId, *Label, MoveTemp(GetBoolValue));
			}
			else
			{
				Toggle(*PropertyId, *Label, MoveTemp(GetBoolValue),
					[WeakObject, BoolProperty, Property, bPersist](bool NewValue) -> bool
					{
						UObject* Obj = WeakObject.Get();
						if (Obj == nullptr)
						{
							return false;
						}
						BoolProperty->SetPropertyValue_InContainer(Obj, NewValue);
						NotifyPropertyChanged(Obj, Property, bPersist);
						return true;
					});
			}
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}

		if (FNumericProperty* NumericProperty = CastField<FNumericProperty>(Property))
		{
			FDeviceExplorerNumberOptions NumberOptions;
#if WITH_METADATA
			const FString MinMeta = Property->HasMetaData(TEXT("ClampMin")) ? Property->GetMetaData(TEXT("ClampMin")) : Property->GetMetaData(TEXT("UIMin"));
			const FString MaxMeta = Property->HasMetaData(TEXT("ClampMax")) ? Property->GetMetaData(TEXT("ClampMax")) : Property->GetMetaData(TEXT("UIMax"));
			const FString UnitsMeta = Property->GetMetaData(TEXT("Units"));
#else
			const FString MinMeta;
			const FString MaxMeta;
			const FString UnitsMeta;
#endif
			if (!MinMeta.IsEmpty())
			{
				NumberOptions.Min = FCString::Atod(*MinMeta);
			}
			if (!MaxMeta.IsEmpty())
			{
				NumberOptions.Max = FCString::Atod(*MaxMeta);
			}
			if (!UnitsMeta.IsEmpty())
			{
				NumberOptions.Unit = *UnitsMeta;
			}

			auto GetNumericValue = [WeakObject, Property, NumericProperty]() -> double
			{
				UObject* Obj = WeakObject.Get();
				if (Obj == nullptr)
				{
					return 0.0;
				}
				const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Obj);
				return NumericProperty->IsFloatingPoint() ? NumericProperty->GetFloatingPointPropertyValue(ValuePtr) : double(NumericProperty->GetSignedIntPropertyValue(ValuePtr));
			};

			if (bEditConst)
			{
				Readonly(*PropertyId, *Label, MoveTemp(GetNumericValue), NumberOptions);
			}
			else
			{
				Number(*PropertyId, *Label, MoveTemp(GetNumericValue),
					[WeakObject, Property, NumericProperty, bPersist](double NewValue) -> bool
					{
						UObject* Obj = WeakObject.Get();
						if (Obj == nullptr)
						{
							return false;
						}
						void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Obj);
						if (NumericProperty->IsFloatingPoint())
						{
							NumericProperty->SetFloatingPointPropertyValue(ValuePtr, NewValue);
						}
						else
						{
							NumericProperty->SetIntPropertyValue(ValuePtr, (int64) NewValue);
						}
						NotifyPropertyChanged(Obj, Property, bPersist);
						return true;
					},
					NumberOptions);
			}
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}

		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			auto GetStringValue = [WeakObject, StrProperty]() -> FString
			{
				UObject* Obj = WeakObject.Get();
				return Obj != nullptr ? StrProperty->GetPropertyValue_InContainer(Obj) : FString();
			};

			if (bEditConst)
			{
				Readonly(*PropertyId, *Label, MoveTemp(GetStringValue));
			}
			else
			{
				String(*PropertyId, *Label, MoveTemp(GetStringValue),
					[WeakObject, StrProperty, Property, bPersist](const FString& NewValue) -> bool
					{
						UObject* Obj = WeakObject.Get();
						if (Obj == nullptr)
						{
							return false;
						}
						StrProperty->SetPropertyValue_InContainer(Obj, NewValue);
						NotifyPropertyChanged(Obj, Property, bPersist);
						return true;
					});
			}
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}

		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			auto GetNameValue = [WeakObject, NameProperty]() -> FString
			{
				UObject* Obj = WeakObject.Get();
				return Obj != nullptr ? NameProperty->GetPropertyValue_InContainer(Obj).ToString() : FString();
			};

			if (bEditConst)
			{
				Readonly(*PropertyId, *Label, MoveTemp(GetNameValue));
			}
			else
			{
				String(*PropertyId, *Label, MoveTemp(GetNameValue),
					[WeakObject, NameProperty, Property, bPersist](const FString& NewValue) -> bool
					{
						UObject* Obj = WeakObject.Get();
						if (Obj == nullptr)
						{
							return false;
						}
						NameProperty->SetPropertyValue_InContainer(Obj, FName(*NewValue));
						NotifyPropertyChanged(Obj, Property, bPersist);
						return true;
					});
			}
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}

		if (FTextProperty* TextProperty = CastField<FTextProperty>(Property))
		{
			auto GetTextValue = [WeakObject, TextProperty]() -> FString
			{
				UObject* Obj = WeakObject.Get();
				return Obj != nullptr ? TextProperty->GetPropertyValue_InContainer(Obj).ToString() : FString();
			};

			if (bEditConst)
			{
				Readonly(*PropertyId, *Label, MoveTemp(GetTextValue));
			}
			else
			{
				String(*PropertyId, *Label, MoveTemp(GetTextValue),
					[WeakObject, TextProperty, Property, bPersist](const FString& NewValue) -> bool
					{
						UObject* Obj = WeakObject.Get();
						if (Obj == nullptr)
						{
							return false;
						}
						TextProperty->SetPropertyValue_InContainer(Obj, FText::FromString(NewValue));
						NotifyPropertyChanged(Obj, Property, bPersist);
						return true;
					});
			}
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}

		if (FStructProperty* StructProperty = CastField<FStructProperty>(Property))
		{
			const UScriptStruct* Struct = StructProperty->Struct;
			if (Struct == TBaseStructure<FVector>::Get())
			{
				auto GetVectorValue = [WeakObject, StructProperty]() -> FVector
				{
					UObject* Obj = WeakObject.Get();
					return Obj != nullptr ? *StructProperty->ContainerPtrToValuePtr<FVector>(Obj) : FVector::ZeroVector;
				};

				if (bEditConst)
				{
					Readonly(*PropertyId, *Label, [GetVectorValue]() -> FString { return GetVectorValue().ToString(); });
				}
				else
				{
					Vector(*PropertyId, *Label, MoveTemp(GetVectorValue),
						[WeakObject, StructProperty, Property, bPersist](const FVector& NewValue) -> bool
						{
							UObject* Obj = WeakObject.Get();
							if (Obj == nullptr)
							{
								return false;
							}
							*StructProperty->ContainerPtrToValuePtr<FVector>(Obj) = NewValue;
							NotifyPropertyChanged(Obj, Property, bPersist);
							return true;
						});
				}
				EnsureSection().Fields.Last().Description = Tooltip;
				continue;
			}

			if (Struct == TBaseStructure<FLinearColor>::Get())
			{
				auto GetColorValue = [WeakObject, StructProperty]() -> FLinearColor
				{
					UObject* Obj = WeakObject.Get();
					return Obj != nullptr ? *StructProperty->ContainerPtrToValuePtr<FLinearColor>(Obj) : FLinearColor::Black;
				};

				if (bEditConst)
				{
					Readonly(*PropertyId, *Label, [GetColorValue]() -> FString { return GetColorValue().ToFColor(true).ToHex(); });
				}
				else
				{
					Color(*PropertyId, *Label, MoveTemp(GetColorValue),
						[WeakObject, StructProperty, Property, bPersist](const FLinearColor& NewValue) -> bool
						{
							UObject* Obj = WeakObject.Get();
							if (Obj == nullptr)
							{
								return false;
							}
							*StructProperty->ContainerPtrToValuePtr<FLinearColor>(Obj) = NewValue;
							NotifyPropertyChanged(Obj, Property, bPersist);
							return true;
						});
				}
				EnsureSection().Fields.Last().Description = Tooltip;
				continue;
			}

			if (Struct == TBaseStructure<FColor>::Get())
			{
				auto GetColorValue = [WeakObject, StructProperty]() -> FColor
				{
					UObject* Obj = WeakObject.Get();
					return Obj != nullptr ? *StructProperty->ContainerPtrToValuePtr<FColor>(Obj) : FColor::Black;
				};

				if (bEditConst)
				{
					Readonly(*PropertyId, *Label, [GetColorValue]() -> FString { return GetColorValue().ToHex(); });
				}
				else
				{
					Color(*PropertyId, *Label, MoveTemp(GetColorValue),
						[WeakObject, StructProperty, Property, bPersist](const FColor& NewValue) -> bool
						{
							UObject* Obj = WeakObject.Get();
							if (Obj == nullptr)
							{
								return false;
							}
							*StructProperty->ContainerPtrToValuePtr<FColor>(Obj) = NewValue;
							NotifyPropertyChanged(Obj, Property, bPersist);
							return true;
						});
				}
				EnsureSection().Fields.Last().Description = Tooltip;
			}
			continue;
		}
	}
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::SettingsObject(UObject* InObject, TArray<FName> PropertyNames, const bool bPersist, const TCHAR* PageDisplayName)
{
	Page(TEXT("settings"), PageDisplayName, { .Icon = TEXT("settings") });
	return Object(InObject, MoveTemp(PropertyNames), bPersist, nullptr,
		{ .Apply = EDeviceExplorerApply::Manual, .Style = EDeviceExplorerSectionStyle::Settings, .bCollapsible = true });
}
#endif

FDeviceExplorerModuleSectionBuilder FDeviceExplorerModulePageBuilder::Section(const TCHAR* InName, const TCHAR* InDisplayName, const FDeviceExplorerSectionOptions& Options)
{
	Builder.ActivePage = PageIndex;
	Builder.ActiveSection = INDEX_NONE;
	return Builder.Section(InName, InDisplayName, Options);
}

FDeviceExplorerModuleSectionBuilder FDeviceExplorerModulePageBuilder::Section(const TCHAR* InDisplayName, const FDeviceExplorerSectionOptions& Options)
{
	return Section(InDisplayName, InDisplayName, Options);
}

#if WITH_COREUOBJECT
FDeviceExplorerModuleBuilder& FDeviceExplorerModulePageBuilder::Object(UObject* InObject, TArray<FName> PropertyNames, bool bPersist, const TCHAR* SectionPrefix, const FDeviceExplorerSectionOptions& SectionOptions)
{
	Builder.ActivePage = PageIndex;
	Builder.ActiveSection = INDEX_NONE;
	return Builder.Object(InObject, MoveTemp(PropertyNames), bPersist, SectionPrefix, SectionOptions);
}
#endif

void FDeviceExplorerModuleSectionBuilder::Activate()
{
	Builder.Activate(PageIndex, SectionIndex);
}

FDeviceExplorerModuleSectionBuilder& FDeviceExplorerModuleSectionBuilder::Badge(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<double()> Getter, const FDeviceExplorerNumberOptions& Options)
{
	Activate();
	Builder.Badge(InName, InDisplayName, MoveTemp(Getter), Options);
	return *this;
}

FDeviceExplorerModuleSectionBuilder& FDeviceExplorerModuleSectionBuilder::Status(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FDeviceExplorerStatus()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	Activate();
	Builder.Status(InName, InDisplayName, MoveTemp(Getter), Options);
	return *this;
}

FDeviceExplorerModuleSectionBuilder& FDeviceExplorerModuleSectionBuilder::Json(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<TSharedPtr<FJsonObject>()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	Activate();
	Builder.Json(InName, InDisplayName, MoveTemp(Getter), Options);
	return *this;
}

FDeviceExplorerModuleSectionBuilder& FDeviceExplorerModuleSectionBuilder::Table(const TCHAR* InName, const TCHAR* InDisplayName, TArray<FString> Columns, TFunction<TArray<TArray<FString>>()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	Activate();
	Builder.Table(InName, InDisplayName, MoveTemp(Columns), MoveTemp(Getter), Options);
	return *this;
}

FDeviceExplorerModuleSectionBuilder& FDeviceExplorerModuleSectionBuilder::Artifact(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<TArray<FDeviceExplorerArtifact>()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	Activate();
	Builder.Artifact(InName, InDisplayName, MoveTemp(Getter), Options);
	return *this;
}

FDeviceExplorerModuleSectionBuilder& FDeviceExplorerModuleSectionBuilder::Path(const TCHAR* InName, const TCHAR* InDisplayName, TFunction<FString()> Getter, const FDeviceExplorerFieldOptions& Options)
{
	Activate();
	Builder.Path(InName, InDisplayName, MoveTemp(Getter), Options);
	return *this;
}

FDeviceExplorerModuleSectionBuilder& FDeviceExplorerModuleSectionBuilder::Command(const TCHAR* ConsoleCommand, const TCHAR* InDisplayName, const TCHAR* Description)
{
	Activate();
	Builder.Command(ConsoleCommand, InDisplayName, Description);
	return *this;
}

bool FDeviceExplorerModuleBuilder::Register()
{
	FDeviceExplorerCoreModule& Registry = FDeviceExplorerCoreModule::Get();
	Registry.RegisterCapability(Owner, DeviceExplorer::ModulesCapability);

	for (FDeviceExplorerFileRootDescriptor& Root : FileRoots)
	{
		Registry.RegisterFileRoot(Root);
	}
	const TSharedRef<FBindings> BindingsRef = Bindings;
	const auto ReadValues = [BindingsRef]()
	{
		TSharedRef<FJsonObject> Values = MakeShared<FJsonObject>();
		for (const auto& Pair : BindingsRef->Entries)
		{
			Values->SetField(Pair.Key, Pair.Value.Reader());
		}
		return Values;
	};

	for (FDeviceExplorerModuleActionDescriptor& ActionDescriptor : Actions)
	{
		ActionDescriptor.Handler = [ReadValues, Inner = MoveTemp(ActionDescriptor.Handler)](const TSharedPtr<FJsonObject>& Parameters)
		{
			FDeviceExplorerModuleResult Result = Inner(Parameters);
			if (!Result.Values.IsValid())
			{
				Result.Values = ReadValues();
			}
			return Result;
		};
	}

	FDeviceExplorerDataModuleDescriptor Descriptor;
	Descriptor.Owner = Owner;
	Descriptor.Name = Name;
	Descriptor.DisplayName = DisplayName;
	Descriptor.Description = DescriptionText;
	Descriptor.Icon = IconText;
	Descriptor.RefreshIntervalMs = RefreshIntervalMs;
	Descriptor.Pages = Pages;
	Descriptor.Actions = Actions;
	Descriptor.DataProvider = [ReadValues]()
	{
		FDeviceExplorerModuleResult Result;
		Result.Values = ReadValues();
		return Result;
	};

	FDeviceExplorerModuleActionDescriptor SetAction;
	SetAction.Name = DeviceExplorer::SetFieldsAction;
	SetAction.DisplayName = FText::FromString(TEXT("Set fields"));
	SetAction.Handler = [BindingsRef, ReadValues](const TSharedPtr<FJsonObject>& Parameters)
	{
		TArray<FString> Errors;
		if (Parameters.IsValid())
		{
			for (const auto& Pair : Parameters->Values)
			{
				const FBindingEntry* Binding = BindingsRef->Entries.Find(Pair.Key);
				if (Binding == nullptr || !Binding->Writer)
				{
					Errors.Add(FString::Printf(TEXT("Unknown or read-only field: %s"), *Pair.Key));
					continue;
				}
				FString FieldError;
				if (!Binding->Writer(Pair.Value, FieldError))
				{
					Errors.Add(FString::Printf(TEXT("%s: %s"), *Pair.Key, *FieldError));
				}
			}
		}

		FDeviceExplorerModuleResult Result;
		Result.bSuccess = Errors.IsEmpty();
		Result.Error = FString::Join(Errors, TEXT("; "));
		Result.Values = ReadValues();
		return Result;
	};
	Descriptor.Actions.Add(MoveTemp(SetAction));

	const bool bRegistered = Registry.RegisterDataModule(MoveTemp(Descriptor));
	if (!bRegistered)
	{
		UE_LOG(LogDeviceExplorerModuleBuilder, Warning, TEXT("DeviceExplorer module '%s' failed to register: check for duplicate field or action ids"), *Name.ToString());
	}
	return bRegistered;
}
