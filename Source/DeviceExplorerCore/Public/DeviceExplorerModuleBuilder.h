#pragma once

#include "CoreMinimal.h"
#include "DeviceExplorerTypes.h"
#include <type_traits>

#if WITH_COREUOBJECT
#include "UObject/WeakObjectPtr.h"
#endif

class FJsonObject;
class FJsonValue;
class FDeviceExplorerModulePageBuilder;
class FDeviceExplorerModuleSectionBuilder;

struct FDeviceExplorerPageOptions
{
	const TCHAR* Description = nullptr;
	const TCHAR* Icon = nullptr;
};

struct FDeviceExplorerSectionOptions
{
	int32 Columns = 0;
	EDeviceExplorerApply Apply = EDeviceExplorerApply::Instant;
	EDeviceExplorerSectionStyle Style = EDeviceExplorerSectionStyle::Default;
	const TCHAR* Description = nullptr;
	bool bCollapsible = false;
	bool bCollapsed = false;
};

struct FDeviceExplorerFieldOptions
{
	const TCHAR* Description = nullptr;
	int32 Span = 1;
};

struct FDeviceExplorerNumberOptions
{
	double Min = TNumericLimits<double>::Lowest();
	double Max = TNumericLimits<double>::Max();
	double Step = 0.0;
	const TCHAR* Unit = nullptr;
	double WarnAbove = TNumericLimits<double>::Max();
	double ErrorAbove = TNumericLimits<double>::Max();
	EDeviceExplorerNumberDisplay Display = EDeviceExplorerNumberDisplay::Auto;
	const TCHAR* Description = nullptr;
	int32 Span = 1;
	bool bSeries = false;
};

struct FDeviceExplorerEnumOptions
{
	const TCHAR* Description = nullptr;
	int32 Span = 1;
	EDeviceExplorerEnumDisplay Display = EDeviceExplorerEnumDisplay::Select;
};

struct FDeviceExplorerTextOptions
{
	const TCHAR* Description = nullptr;
	int32 Span = 1;
	int32 Rows = 3;
};

struct FDeviceExplorerVectorOptions
{
	const TCHAR* Description = nullptr;
	int32 Span = 1;
	double Step = 0.0;
};

struct FDeviceExplorerActionOptions
{
	const TCHAR* Description = nullptr;
	bool bRequiresConfirmation = false;
	EDeviceExplorerActionStyle Style = EDeviceExplorerActionStyle::Default;
	int32 Span = 1;
	const TCHAR* ActionLabel = nullptr;
};

struct FDeviceExplorerActionInput
{
	const TCHAR* Name = nullptr;
	const TCHAR* DisplayName = nullptr;
	EDeviceExplorerInputType Type = EDeviceExplorerInputType::String;
	const TCHAR* DefaultValue = nullptr;
};

struct DEVICEEXPLORERCORE_API FDeviceExplorerActionParameters
{
	FString GetString(const TCHAR* Name, const FString& DefaultValue = FString()) const;
	double GetNumber(const TCHAR* Name, double DefaultValue = 0.0) const;
	bool GetBool(const TCHAR* Name, bool bDefaultValue = false) const;

	TSharedPtr<FJsonObject> Values;
};

class DEVICEEXPLORERCORE_API FDeviceExplorerModuleBuilder
{
public:
	FDeviceExplorerModuleBuilder(FName Owner, const TCHAR* Name, const TCHAR* DisplayName);

	FDeviceExplorerModuleBuilder& Description(const TCHAR* Text);
	FDeviceExplorerModuleBuilder& Icon(const TCHAR* Icon);
	FDeviceExplorerModuleBuilder& RefreshMs(int32 Milliseconds);
	FDeviceExplorerModuleBuilder& FileRoot(const TCHAR* Name, const FString& AbsolutePath, const TCHAR* DisplayName = nullptr, bool bAllowDownload = true);

	FDeviceExplorerModulePageBuilder Page(const TCHAR* Name, const TCHAR* DisplayName = nullptr, const FDeviceExplorerPageOptions& Options = {});
	FDeviceExplorerModuleSectionBuilder Section(const TCHAR* Name, const TCHAR* DisplayName, const FDeviceExplorerSectionOptions& Options = {});
	FDeviceExplorerModuleSectionBuilder Section(const TCHAR* DisplayName, const FDeviceExplorerSectionOptions& Options = {});

	template <typename GetterType>
	FDeviceExplorerModuleBuilder& Readonly(const TCHAR* InName, const TCHAR* InDisplayName, GetterType Getter, const FDeviceExplorerNumberOptions& Options = {})
	{
		using FReturnType = std::decay_t<decltype(Getter())>;
		if constexpr (std::is_same_v<FReturnType, bool>)
		{
			return ReadonlyBool(InName, InDisplayName, TFunction<bool()>(MoveTemp(Getter)), Options);
		}
		else if constexpr (std::is_arithmetic_v<FReturnType>)
		{
			return ReadonlyNumber(InName, InDisplayName, TFunction<double()>([Getter = MoveTemp(Getter)]() mutable { return double(Getter()); }), Options);
		}
		else if constexpr (std::is_same_v<FReturnType, FText>)
		{
			return ReadonlyText(InName, InDisplayName, TFunction<FString()>([Getter = MoveTemp(Getter)]() mutable { return Getter().ToString(); }), Options);
		}
		else if constexpr (std::is_same_v<FReturnType, FName>)
		{
			return ReadonlyText(InName, InDisplayName, TFunction<FString()>([Getter = MoveTemp(Getter)]() mutable { return Getter().ToString(); }), Options);
		}
		else
		{
			return ReadonlyText(InName, InDisplayName, TFunction<FString()>(MoveTemp(Getter)), Options);
		}
	}

	FDeviceExplorerModuleBuilder& Badge(const TCHAR* Name, const TCHAR* DisplayName, TFunction<double()> Getter, const FDeviceExplorerNumberOptions& Options = {});

	template <typename GetterType, typename MaxGetterType>
	FDeviceExplorerModuleBuilder& Meter(const TCHAR* InName, const TCHAR* InDisplayName, GetterType Getter, MaxGetterType MaxGetter)
	{
		return MeterImpl(InName, InDisplayName,
			TFunction<double()>([Getter = MoveTemp(Getter)]() mutable { return double(Getter()); }),
			TFunction<double()>([MaxGetter = MoveTemp(MaxGetter)]() mutable { return double(MaxGetter()); }));
	}

	template <typename GetterType>
	FDeviceExplorerModuleBuilder& Series(const TCHAR* InName, const TCHAR* InDisplayName, GetterType Getter, const FDeviceExplorerNumberOptions& Options = {})
	{
		using FReturnType = std::decay_t<decltype(Getter())>;
		if constexpr (std::is_arithmetic_v<FReturnType>)
		{
			return SeriesSampleImpl(InName, InDisplayName, TFunction<double()>([Getter = MoveTemp(Getter)]() mutable { return double(Getter()); }), Options);
		}
		else
		{
			return SeriesWindowImpl(InName, InDisplayName,
				TFunction<TArray<double>()>([Getter = MoveTemp(Getter)]() mutable
				{
					TArray<double> Samples;
					for (const auto& Sample : Getter())
					{
						Samples.Add(double(Sample));
					}
					return Samples;
				}), Options);
		}
	}

	FDeviceExplorerModuleBuilder& Status(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FDeviceExplorerStatus()> Getter, const FDeviceExplorerFieldOptions& Options = {});
	FDeviceExplorerModuleBuilder& Json(const TCHAR* Name, const TCHAR* DisplayName, TFunction<TSharedPtr<FJsonObject>()> Getter, const FDeviceExplorerFieldOptions& Options = {});
	FDeviceExplorerModuleBuilder& Table(const TCHAR* Name, const TCHAR* DisplayName, TArray<FString> Columns, TFunction<TArray<TArray<FString>>()> Getter, const FDeviceExplorerFieldOptions& Options = {});
	FDeviceExplorerModuleBuilder& Artifact(const TCHAR* Name, const TCHAR* DisplayName, TFunction<TArray<FDeviceExplorerArtifact>()> Getter, const FDeviceExplorerFieldOptions& Options = {});
	FDeviceExplorerModuleBuilder& Path(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FString()> Getter, const FDeviceExplorerFieldOptions& Options = {});

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleBuilder& Toggle(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerFieldOptions& Options = {})
	{
		return ToggleImpl(Name, DisplayName, TFunction<bool()>(MoveTemp(Getter)),
			TFunction<FDeviceExplorerWriteResult(bool)>([Setter = MoveTemp(Setter)](bool Value) mutable { return InvokeSetter(Setter, Value); }), Options);
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleBuilder& Number(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerNumberOptions& Options = {})
	{
		return NumberImpl(Name, DisplayName,
			TFunction<double()>([Getter = MoveTemp(Getter)]() mutable { return double(Getter()); }),
			TFunction<FDeviceExplorerWriteResult(double)>([Setter = MoveTemp(Setter)](double Value) mutable { return InvokeSetter(Setter, Value); }), Options);
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleBuilder& String(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerFieldOptions& Options = {})
	{
		return StringImpl(Name, DisplayName, TFunction<FString()>(MoveTemp(Getter)),
			TFunction<FDeviceExplorerWriteResult(const FString&)>([Setter = MoveTemp(Setter)](const FString& Value) mutable { return InvokeSetter(Setter, Value); }), Options);
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleBuilder& Enum(const TCHAR* Name, const TCHAR* DisplayName, TArray<FString> Values, GetterType Getter, SetterType Setter, const FDeviceExplorerEnumOptions& Options = {})
	{
		return EnumImpl(Name, DisplayName, MoveTemp(Values), TFunction<FString()>(MoveTemp(Getter)),
			TFunction<FDeviceExplorerWriteResult(const FString&)>([Setter = MoveTemp(Setter)](const FString& Value) mutable { return InvokeSetter(Setter, Value); }), Options);
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleBuilder& Flags(const TCHAR* Name, const TCHAR* DisplayName, TArray<FString> Values, GetterType Getter, SetterType Setter, const FDeviceExplorerFieldOptions& Options = {})
	{
		return FlagsImpl(Name, DisplayName, MoveTemp(Values), TFunction<TArray<FString>()>(MoveTemp(Getter)),
			TFunction<FDeviceExplorerWriteResult(const TArray<FString>&)>([Setter = MoveTemp(Setter)](const TArray<FString>& Value) mutable { return InvokeSetter(Setter, Value); }), Options);
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleBuilder& Text(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerTextOptions& Options = {})
	{
		return TextImpl(Name, DisplayName, TFunction<FString()>(MoveTemp(Getter)),
			TFunction<FDeviceExplorerWriteResult(const FString&)>([Setter = MoveTemp(Setter)](const FString& Value) mutable { return InvokeSetter(Setter, Value); }), Options);
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleBuilder& Vector(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerVectorOptions& Options = {})
	{
		return VectorImpl(Name, DisplayName, TFunction<FVector()>(MoveTemp(Getter)),
			TFunction<FDeviceExplorerWriteResult(const FVector&)>([Setter = MoveTemp(Setter)](const FVector& Value) mutable { return InvokeSetter(Setter, Value); }), Options);
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleBuilder& Color(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerFieldOptions& Options = {})
	{
		using FReturnType = std::decay_t<decltype(Getter())>;
		if constexpr (std::is_same_v<FReturnType, FLinearColor>)
		{
			return ColorImpl(Name, DisplayName,
				TFunction<FColor()>([Getter = MoveTemp(Getter)]() mutable { return Getter().ToFColor(true); }),
				TFunction<FDeviceExplorerWriteResult(const FColor&)>([Setter = MoveTemp(Setter)](const FColor& Value) mutable { return InvokeSetter(Setter, FLinearColor(Value)); }), Options);
		}
		else
		{
			return ColorImpl(Name, DisplayName, TFunction<FColor()>(MoveTemp(Getter)),
				TFunction<FDeviceExplorerWriteResult(const FColor&)>([Setter = MoveTemp(Setter)](const FColor& Value) mutable { return InvokeSetter(Setter, Value); }), Options);
		}
	}

	template <typename HandlerType>
	FDeviceExplorerModuleBuilder& Action(const TCHAR* Name, const TCHAR* DisplayName, HandlerType Handler, const FDeviceExplorerActionOptions& Options = {})
	{
		return ActionImpl(Name, DisplayName,
			TFunction<FDeviceExplorerModuleResult()>([Handler = MoveTemp(Handler)]() mutable { return InvokeHandler(Handler); }), Options);
	}

	template <typename HandlerType>
	FDeviceExplorerModuleBuilder& Action(const TCHAR* Name, const TCHAR* DisplayName, TArray<FDeviceExplorerActionInput> Inputs, HandlerType Handler, const FDeviceExplorerActionOptions& Options = {})
	{
		return ActionFormImpl(Name, DisplayName, MoveTemp(Inputs),
			TFunction<FDeviceExplorerModuleResult(const FDeviceExplorerActionParameters&)>(
				[Handler = MoveTemp(Handler)](const FDeviceExplorerActionParameters& Parameters) mutable { return InvokeHandler(Handler, Parameters); }), Options);
	}

	template <typename HandlerType>
	FDeviceExplorerModuleBuilder& Button(const TCHAR* Name, const TCHAR* DisplayName, HandlerType Handler, const TCHAR* Description = nullptr, bool bRequiresConfirmation = false)
	{
		return Action(Name, DisplayName, MoveTemp(Handler), { .Description = Description, .bRequiresConfirmation = bRequiresConfirmation });
	}

	FDeviceExplorerModuleBuilder& Command(const TCHAR* ConsoleCommand, const TCHAR* DisplayName, const TCHAR* Description);

#if WITH_COREUOBJECT
	FDeviceExplorerModuleBuilder& Object(UObject* Object, TArray<FName> PropertyNames = {}, bool bPersist = false, const TCHAR* SectionPrefix = nullptr, const FDeviceExplorerSectionOptions& SectionOptions = {});
	FDeviceExplorerModuleBuilder& SettingsObject(UObject* Object, TArray<FName> PropertyNames = {}, bool bPersist = true, const TCHAR* PageDisplayName = TEXT("Settings"));
#endif

	bool Register();

private:
	friend class FDeviceExplorerModulePageBuilder;
	friend class FDeviceExplorerModuleSectionBuilder;

	struct FBindingEntry
	{
		TFunction<TSharedPtr<FJsonValue>()> Reader;
		TFunction<bool(const TSharedPtr<FJsonValue>&, FString& OutError)> Writer;
	};

	struct FBindings
	{
		TMap<FString, FBindingEntry> Entries;
	};

	template <typename SetterType, typename ValueType>
	static FDeviceExplorerWriteResult InvokeSetter(SetterType& Setter, ValueType&& Value)
	{
		using FResult = std::invoke_result_t<SetterType, ValueType>;
		if constexpr (std::is_same_v<FResult, void>)
		{
			Setter(Forward<ValueType>(Value));
			return FDeviceExplorerWriteResult::Success();
		}
		else if constexpr (std::is_same_v<FResult, bool>)
		{
			return Setter(Forward<ValueType>(Value))
				? FDeviceExplorerWriteResult::Success()
				: FDeviceExplorerWriteResult::Failure(TEXT("Setter rejected the value"));
		}
		else
		{
			static_assert(std::is_same_v<FResult, FDeviceExplorerWriteResult>);
			return Setter(Forward<ValueType>(Value));
		}
	}

	template <typename HandlerType, typename... ArgTypes>
	static FDeviceExplorerModuleResult InvokeHandler(HandlerType& Handler, ArgTypes&&... Args)
	{
		using FResult = std::invoke_result_t<HandlerType, ArgTypes...>;
		if constexpr (std::is_same_v<FResult, void>)
		{
			Handler(Forward<ArgTypes>(Args)...);
			return FDeviceExplorerModuleResult();
		}
		else if constexpr (std::is_same_v<FResult, bool>)
		{
			FDeviceExplorerModuleResult Result;
			Result.bSuccess = Handler(Forward<ArgTypes>(Args)...);
			if (!Result.bSuccess)
			{
				Result.Error = TEXT("Action failed");
			}
			return Result;
		}
		else
		{
			static_assert(std::is_same_v<FResult, FDeviceExplorerModuleResult>);
			return Handler(Forward<ArgTypes>(Args)...);
		}
	}

	void Activate(int32 PageIndex, int32 SectionIndex);
	FDeviceExplorerPageDescriptor& EnsurePage();
	FDeviceExplorerSectionDescriptor& EnsureSection();
	FDeviceExplorerFieldDescriptor& AddField(const TCHAR* Name, const TCHAR* DisplayName, EDeviceExplorerWidget Widget, bool bReadOnly);
	void AddBinding(FName Name, TFunction<TSharedPtr<FJsonValue>()> Reader, TFunction<bool(const TSharedPtr<FJsonValue>&, FString& OutError)> Writer);
	static void ApplyNumberOptions(FDeviceExplorerFieldDescriptor& Field, const FDeviceExplorerNumberOptions& Options);

	FDeviceExplorerModuleBuilder& ReadonlyBool(const TCHAR* Name, const TCHAR* DisplayName, TFunction<bool()> Getter, const FDeviceExplorerNumberOptions& Options);
	FDeviceExplorerModuleBuilder& ReadonlyNumber(const TCHAR* Name, const TCHAR* DisplayName, TFunction<double()> Getter, const FDeviceExplorerNumberOptions& Options);
	FDeviceExplorerModuleBuilder& ReadonlyText(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FString()> Getter, const FDeviceExplorerNumberOptions& Options);
	FDeviceExplorerModuleBuilder& MeterImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<double()> Getter, TFunction<double()> MaxGetter);
	FDeviceExplorerModuleBuilder& SeriesSampleImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<double()> Getter, const FDeviceExplorerNumberOptions& Options);
	FDeviceExplorerModuleBuilder& SeriesWindowImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<TArray<double>()> Getter, const FDeviceExplorerNumberOptions& Options);
	FDeviceExplorerModuleBuilder& ToggleImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<bool()> Getter, TFunction<FDeviceExplorerWriteResult(bool)> Setter, const FDeviceExplorerFieldOptions& Options);
	FDeviceExplorerModuleBuilder& NumberImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<double()> Getter, TFunction<FDeviceExplorerWriteResult(double)> Setter, const FDeviceExplorerNumberOptions& Options);
	FDeviceExplorerModuleBuilder& StringImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FString()> Getter, TFunction<FDeviceExplorerWriteResult(const FString&)> Setter, const FDeviceExplorerFieldOptions& Options);
	FDeviceExplorerModuleBuilder& TextImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FString()> Getter, TFunction<FDeviceExplorerWriteResult(const FString&)> Setter, const FDeviceExplorerTextOptions& Options);
	FDeviceExplorerModuleBuilder& EnumImpl(const TCHAR* Name, const TCHAR* DisplayName, TArray<FString> Values, TFunction<FString()> Getter, TFunction<FDeviceExplorerWriteResult(const FString&)> Setter, const FDeviceExplorerEnumOptions& Options);
	FDeviceExplorerModuleBuilder& FlagsImpl(const TCHAR* Name, const TCHAR* DisplayName, TArray<FString> Values, TFunction<TArray<FString>()> Getter, TFunction<FDeviceExplorerWriteResult(const TArray<FString>&)> Setter, const FDeviceExplorerFieldOptions& Options);
	FDeviceExplorerModuleBuilder& VectorImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FVector()> Getter, TFunction<FDeviceExplorerWriteResult(const FVector&)> Setter, const FDeviceExplorerVectorOptions& Options);
	FDeviceExplorerModuleBuilder& ColorImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FColor()> Getter, TFunction<FDeviceExplorerWriteResult(const FColor&)> Setter, const FDeviceExplorerFieldOptions& Options);
	FDeviceExplorerModuleBuilder& ActionImpl(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FDeviceExplorerModuleResult()> Handler, const FDeviceExplorerActionOptions& Options);
	FDeviceExplorerModuleBuilder& ActionFormImpl(const TCHAR* Name, const TCHAR* DisplayName, TArray<FDeviceExplorerActionInput> Inputs, TFunction<FDeviceExplorerModuleResult(const FDeviceExplorerActionParameters&)> Handler, const FDeviceExplorerActionOptions& Options);

#if WITH_COREUOBJECT
	static void NotifyPropertyChanged(UObject* Object, class FProperty* Property, bool bPersist);
#endif

	FName Owner;
	FName Name;
	FText DisplayName;
	FString DescriptionText;
	FString IconText;
	int32 RefreshIntervalMs = 0;
	int32 ActivePage = INDEX_NONE;
	int32 ActiveSection = INDEX_NONE;
	TArray<FDeviceExplorerFileRootDescriptor> FileRoots;
	TArray<FDeviceExplorerModuleActionDescriptor> Actions;
	TArray<FDeviceExplorerPageDescriptor> Pages;
	TSharedRef<FBindings> Bindings;
};

class DEVICEEXPLORERCORE_API FDeviceExplorerModulePageBuilder
{
public:
	FDeviceExplorerModuleSectionBuilder Section(const TCHAR* Name, const TCHAR* DisplayName, const FDeviceExplorerSectionOptions& Options = {});
	FDeviceExplorerModuleSectionBuilder Section(const TCHAR* DisplayName, const FDeviceExplorerSectionOptions& Options = {});

private:
	friend class FDeviceExplorerModuleBuilder;
	FDeviceExplorerModulePageBuilder(FDeviceExplorerModuleBuilder& InBuilder, int32 InPageIndex)
		: Builder(InBuilder), PageIndex(InPageIndex)
	{
	}

	FDeviceExplorerModuleBuilder& Builder;
	int32 PageIndex;
};

class DEVICEEXPLORERCORE_API FDeviceExplorerModuleSectionBuilder
{
public:
	template <typename GetterType>
	FDeviceExplorerModuleSectionBuilder& Readonly(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, const FDeviceExplorerNumberOptions& Options = {})
	{
		Activate();
		Builder.Readonly(Name, DisplayName, MoveTemp(Getter), Options);
		return *this;
	}

	FDeviceExplorerModuleSectionBuilder& Badge(const TCHAR* Name, const TCHAR* DisplayName, TFunction<double()> Getter, const FDeviceExplorerNumberOptions& Options = {});

	template <typename GetterType, typename MaxGetterType>
	FDeviceExplorerModuleSectionBuilder& Meter(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, MaxGetterType MaxGetter)
	{
		Activate();
		Builder.Meter(Name, DisplayName, MoveTemp(Getter), MoveTemp(MaxGetter));
		return *this;
	}

	template <typename GetterType>
	FDeviceExplorerModuleSectionBuilder& Series(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, const FDeviceExplorerNumberOptions& Options = {})
	{
		Activate();
		Builder.Series(Name, DisplayName, MoveTemp(Getter), Options);
		return *this;
	}

	FDeviceExplorerModuleSectionBuilder& Status(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FDeviceExplorerStatus()> Getter, const FDeviceExplorerFieldOptions& Options = {});
	FDeviceExplorerModuleSectionBuilder& Json(const TCHAR* Name, const TCHAR* DisplayName, TFunction<TSharedPtr<FJsonObject>()> Getter, const FDeviceExplorerFieldOptions& Options = {});
	FDeviceExplorerModuleSectionBuilder& Table(const TCHAR* Name, const TCHAR* DisplayName, TArray<FString> Columns, TFunction<TArray<TArray<FString>>()> Getter, const FDeviceExplorerFieldOptions& Options = {});
	FDeviceExplorerModuleSectionBuilder& Artifact(const TCHAR* Name, const TCHAR* DisplayName, TFunction<TArray<FDeviceExplorerArtifact>()> Getter, const FDeviceExplorerFieldOptions& Options = {});
	FDeviceExplorerModuleSectionBuilder& Path(const TCHAR* Name, const TCHAR* DisplayName, TFunction<FString()> Getter, const FDeviceExplorerFieldOptions& Options = {});

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleSectionBuilder& Toggle(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerFieldOptions& Options = {})
	{
		Activate();
		Builder.Toggle(Name, DisplayName, MoveTemp(Getter), MoveTemp(Setter), Options);
		return *this;
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleSectionBuilder& Number(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerNumberOptions& Options = {})
	{
		Activate();
		Builder.Number(Name, DisplayName, MoveTemp(Getter), MoveTemp(Setter), Options);
		return *this;
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleSectionBuilder& String(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerFieldOptions& Options = {})
	{
		Activate();
		Builder.String(Name, DisplayName, MoveTemp(Getter), MoveTemp(Setter), Options);
		return *this;
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleSectionBuilder& Enum(const TCHAR* Name, const TCHAR* DisplayName, TArray<FString> Values, GetterType Getter, SetterType Setter, const FDeviceExplorerEnumOptions& Options = {})
	{
		Activate();
		Builder.Enum(Name, DisplayName, MoveTemp(Values), MoveTemp(Getter), MoveTemp(Setter), Options);
		return *this;
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleSectionBuilder& Flags(const TCHAR* Name, const TCHAR* DisplayName, TArray<FString> Values, GetterType Getter, SetterType Setter, const FDeviceExplorerFieldOptions& Options = {})
	{
		Activate();
		Builder.Flags(Name, DisplayName, MoveTemp(Values), MoveTemp(Getter), MoveTemp(Setter), Options);
		return *this;
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleSectionBuilder& Text(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerTextOptions& Options = {})
	{
		Activate();
		Builder.Text(Name, DisplayName, MoveTemp(Getter), MoveTemp(Setter), Options);
		return *this;
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleSectionBuilder& Vector(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerVectorOptions& Options = {})
	{
		Activate();
		Builder.Vector(Name, DisplayName, MoveTemp(Getter), MoveTemp(Setter), Options);
		return *this;
	}

	template <typename GetterType, typename SetterType>
	FDeviceExplorerModuleSectionBuilder& Color(const TCHAR* Name, const TCHAR* DisplayName, GetterType Getter, SetterType Setter, const FDeviceExplorerFieldOptions& Options = {})
	{
		Activate();
		Builder.Color(Name, DisplayName, MoveTemp(Getter), MoveTemp(Setter), Options);
		return *this;
	}

	template <typename HandlerType>
	FDeviceExplorerModuleSectionBuilder& Action(const TCHAR* Name, const TCHAR* DisplayName, HandlerType Handler, const FDeviceExplorerActionOptions& Options = {})
	{
		Activate();
		Builder.Action(Name, DisplayName, MoveTemp(Handler), Options);
		return *this;
	}

	template <typename HandlerType>
	FDeviceExplorerModuleSectionBuilder& Action(const TCHAR* Name, const TCHAR* DisplayName, TArray<FDeviceExplorerActionInput> Inputs, HandlerType Handler, const FDeviceExplorerActionOptions& Options = {})
	{
		Activate();
		Builder.Action(Name, DisplayName, MoveTemp(Inputs), MoveTemp(Handler), Options);
		return *this;
	}

	FDeviceExplorerModuleSectionBuilder& Command(const TCHAR* ConsoleCommand, const TCHAR* DisplayName, const TCHAR* Description);

private:
	friend class FDeviceExplorerModuleBuilder;
	friend class FDeviceExplorerModulePageBuilder;
	FDeviceExplorerModuleSectionBuilder(FDeviceExplorerModuleBuilder& InBuilder, int32 InPageIndex, int32 InSectionIndex)
		: Builder(InBuilder), PageIndex(InPageIndex), SectionIndex(InSectionIndex)
	{
	}

	void Activate();
	FDeviceExplorerModuleBuilder& Builder;
	int32 PageIndex;
	int32 SectionIndex;
};
