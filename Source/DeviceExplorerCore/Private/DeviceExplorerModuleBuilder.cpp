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

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::EnumImpl(const TCHAR* InName, const TCHAR* InDisplayName, TArray<FString> Values, TFunction<FString()> Getter, TFunction<FDeviceExplorerWriteResult(const FString&)> Setter, const FDeviceExplorerFieldOptions& Options)
{
	FDeviceExplorerFieldDescriptor& Field = AddField(InName, InDisplayName, EDeviceExplorerWidget::Enum, false);
	Field.Options = MoveTemp(Values);
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

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::Object(UObject* InObject, TArray<FName> PropertyNames, bool bPersist, const TCHAR* SectionPrefix)
{
	if (InObject == nullptr)
	{
		return *this;
	}

	const TWeakObjectPtr<UObject> WeakObject(InObject);
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
		const FString Label = MetaDisplayName.IsEmpty() ? Property->GetName() : MetaDisplayName;
		if (!bHasSection || Category != LastCategory)
		{
			const FString SectionId = Category.IsEmpty() ? TEXT("General") : Category;
			FString SectionLabel = SectionId;
			if (SectionPrefix != nullptr)
			{
				SectionLabel = Category.IsEmpty() ? FString(SectionPrefix) : FString::Printf(TEXT("%s · %s"), SectionPrefix, *Category);
			}
			Section(*SectionId);
			EnsureSection().DisplayName = FText::FromString(SectionLabel);
			LastCategory = Category;
			bHasSection = true;
		}

		const FString PropertyId = Property->GetName();

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
			TArray<FString> EnumOptions;
			for (int32 Index = 0; Index < PropertyEnum->NumEnums() - 1; ++Index)
			{
				EnumOptions.Add(PropertyEnum->GetNameStringByIndex(Index));
			}
			Enum(*PropertyId, *Label, MoveTemp(EnumOptions),
				[WeakObject, Property, UnderlyingNumeric, PropertyEnum]() -> FString
				{
					UObject* Obj = WeakObject.Get();
					if (Obj == nullptr)
					{
						return FString();
					}
					const int64 Value = UnderlyingNumeric->GetSignedIntPropertyValue(Property->ContainerPtrToValuePtr<void>(Obj));
					return PropertyEnum->GetNameStringByValue(Value);
				},
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
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}

		if (FBoolProperty* BoolProperty = CastField<FBoolProperty>(Property))
		{
			Toggle(*PropertyId, *Label,
				[WeakObject, BoolProperty]() -> bool
				{
					UObject* Obj = WeakObject.Get();
					return Obj != nullptr && BoolProperty->GetPropertyValue_InContainer(Obj);
				},
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

			Number(*PropertyId, *Label,
				[WeakObject, Property, NumericProperty]() -> double
				{
					UObject* Obj = WeakObject.Get();
					if (Obj == nullptr)
					{
						return 0.0;
					}
					const void* ValuePtr = Property->ContainerPtrToValuePtr<void>(Obj);
					return NumericProperty->IsFloatingPoint() ? NumericProperty->GetFloatingPointPropertyValue(ValuePtr) : double(NumericProperty->GetSignedIntPropertyValue(ValuePtr));
				},
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
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}

		if (FStrProperty* StrProperty = CastField<FStrProperty>(Property))
		{
			String(*PropertyId, *Label,
				[WeakObject, StrProperty]() -> FString
				{
					UObject* Obj = WeakObject.Get();
					return Obj != nullptr ? StrProperty->GetPropertyValue_InContainer(Obj) : FString();
				},
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
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}

		if (FNameProperty* NameProperty = CastField<FNameProperty>(Property))
		{
			String(*PropertyId, *Label,
				[WeakObject, NameProperty]() -> FString
				{
					UObject* Obj = WeakObject.Get();
					return Obj != nullptr ? NameProperty->GetPropertyValue_InContainer(Obj).ToString() : FString();
				},
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
			EnsureSection().Fields.Last().Description = Tooltip;
			continue;
		}
	}
	return *this;
}

FDeviceExplorerModuleBuilder& FDeviceExplorerModuleBuilder::SettingsObject(UObject* InObject, TArray<FName> PropertyNames, const bool bPersist, const TCHAR* PageDisplayName)
{
	Page(TEXT("settings"), PageDisplayName, { .Icon = TEXT("settings") });
	const int32 FirstSection = EnsurePage().Sections.Num();
	Object(InObject, MoveTemp(PropertyNames), bPersist);
	for (int32 Index = FirstSection; Index < EnsurePage().Sections.Num(); ++Index)
	{
		FDeviceExplorerSectionDescriptor& SectionDescriptor = EnsurePage().Sections[Index];
		SectionDescriptor.Apply = EDeviceExplorerApply::Manual;
		SectionDescriptor.Style = EDeviceExplorerSectionStyle::Settings;
		SectionDescriptor.bCollapsible = true;
	}
	return *this;
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
