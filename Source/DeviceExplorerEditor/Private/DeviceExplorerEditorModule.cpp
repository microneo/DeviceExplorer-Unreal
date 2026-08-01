#include "DeviceExplorerEditorModule.h"

#include "DesktopPlatformModule.h"
#include "DeviceExplorerEditorSettings.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "IDesktopPlatform.h"
#include "ISettingsModule.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Guid.h"
#include "Misc/MessageDialog.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/StyleColors.h"
#include "ToolMenus.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "DeviceExplorerEditor"

void FDeviceExplorerEditorModule::StartupModule()
{
	UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FDeviceExplorerEditorModule::RegisterMenus));

	const UDeviceExplorerEditorSettings* Settings = GetDefault<UDeviceExplorerEditorSettings>();
	bStopWithEditor = Settings->bStopWithEditor;
	if (Settings->bAutoStart && FPaths::FileExists(FindHostExecutable()))
	{
		StartHost();
	}
}

void FDeviceExplorerEditorModule::ShutdownModule()
{
	UToolMenus::UnRegisterStartupCallback(this);
	UToolMenus::UnregisterOwner(this);

	// GetDefault<>() here would crash (SIGBUS on Mac): UObjects are already torn down by the time
	// ShutdownModule() runs at engine exit. Use the value cached in StartupModule instead.
	if (bStopWithEditor)
	{
		StopHost();
	}
}

bool FDeviceExplorerEditorModule::IsHostRunning()
{
	if (!HostProcess.IsValid())
	{
		return false;
	}
	if (FPlatformProcess::IsProcRunning(HostProcess))
	{
		return true;
	}

	FPlatformProcess::CloseProc(HostProcess);
	HostProcess.Reset();
	HostProcessId = 0;
	return false;
}

void FDeviceExplorerEditorModule::StartHost()
{
	if (IsHostRunning())
	{
		OpenDashboard();
		return;
	}

	const FString Executable = FindHostExecutable();
	if (!FPaths::FileExists(Executable) && !BuildHost())
	{
		return;
	}

	const UDeviceExplorerEditorSettings* Settings = GetDefault<UDeviceExplorerEditorSettings>();
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DeviceExplorer"));
	if (!Plugin.IsValid())
	{
		Notify(LOCTEXT("PluginMissing", "Cannot locate the DeviceExplorer plugin."), true);
		return;
	}

	const FString WebRoot = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Resources"), TEXT("Web"));
	const FString TransferDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("DeviceExplorer"), TEXT("Transfers"));
	// Generated here so it can be shown below for the manual -DeviceExplorerServer/-DeviceExplorerToken fallback.
	CurrentHostToken = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	// -Project must not be passed: project loading aborts in the host's program target.
	const FString Arguments = FString::Printf(TEXT("-ParentPID=%u -DashboardPort=%d -DevicePort=%d -WebRoot=\"%s\" -TransferDir=\"%s\" -Token=%s"),
	                                          FPlatformProcess::GetCurrentProcessId(),
	                                          Settings->DashboardPort,
	                                          Settings->DevicePort,
	                                          *WebRoot,
	                                          *FPaths::ConvertRelativePathToFull(TransferDir),
	                                          *CurrentHostToken);

	HostProcess = FPlatformProcess::CreateProc(*Executable, *Arguments, true, false, false, &HostProcessId, 0, *FPaths::ProjectDir(), nullptr);
	if (!HostProcess.IsValid())
	{
		HostProcessId = 0;
		CurrentHostToken.Reset();
		Notify(LOCTEXT("StartFailed", "Failed to start DeviceExplorerHost."), true);
		return;
	}

	Notify(FText::Format(LOCTEXT("Started", "DeviceExplorerHost started.\nManual connect token: {0}"), FText::FromString(CurrentHostToken)));
	if (Settings->bOpenBrowserOnStart)
	{
		OpenDashboard();
	}
}

void FDeviceExplorerEditorModule::StopHost()
{
	if (!IsHostRunning())
	{
		return;
	}

	FPlatformProcess::TerminateProc(HostProcess, true);
	FPlatformProcess::CloseProc(HostProcess);
	HostProcess.Reset();
	HostProcessId = 0;
	CurrentHostToken.Reset();
	Notify(LOCTEXT("Stopped", "DeviceExplorerHost stopped."));
}

void FDeviceExplorerEditorModule::RestartHost()
{
	StopHost();
	StartHost();
}

bool FDeviceExplorerEditorModule::InstallHostTarget(FText& OutError) const
{
	const FString TargetPath = FPaths::Combine(FPaths::ProjectDir(), TEXT("Source"), TEXT("DeviceExplorerHost.Target.cs"));
	if (FPaths::FileExists(TargetPath))
	{
		return true;
	}

	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("DeviceExplorer"));
	if (!Plugin.IsValid())
	{
		OutError = LOCTEXT("PluginMissing", "Cannot locate the DeviceExplorer plugin.");
		return false;
	}

	IFileManager& FileManager = IFileManager::Get();
	const FString TemplatePath = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Templates"), TEXT("DeviceExplorerHost.Target.cs"));
	FileManager.MakeDirectory(*FPaths::GetPath(TargetPath), true);
	if (FileManager.Copy(*TargetPath, *TemplatePath) != COPY_OK)
	{
		OutError = FText::Format(LOCTEXT("TargetInstallFailed", "Failed to write {0}."), FText::FromString(TargetPath));
		return false;
	}

	return true;
}

bool FDeviceExplorerEditorModule::BuildHost()
{
	IDesktopPlatform* DesktopPlatform = FDesktopPlatformModule::Get();
	if (DesktopPlatform == nullptr || !DesktopPlatform->IsUnrealBuildToolAvailable())
	{
		Notify(LOCTEXT("BuildToolMissing", "UnrealBuildTool is not available.\nBuild the host with Scripts/BuildDeviceExplorer.ps1."), true);
		return false;
	}

	const EAppReturnType::Type Answer = FMessageDialog::Open(
		EAppMsgType::YesNo,
		LOCTEXT("BuildHostPrompt", "DeviceExplorerHost is not built yet.\n\nBuild it now? The editor is blocked until the build finishes, which takes a few minutes the first time."),
		LOCTEXT("BuildHostTitle", "DeviceExplorer"));
	if (Answer != EAppReturnType::Yes)
	{
		return false;
	}

	FText InstallError;
	if (!InstallHostTarget(InstallError))
	{
		Notify(InstallError, true);
		return false;
	}

	// Only Development produces the unsuffixed executable name FindHostExecutable() expects.
	const FString Arguments = FString::Printf(
		TEXT("DeviceExplorerHost Development %s -Project=\"%s\" -Progress -WaitMutex -NoHotReloadFromIDE"),
		FPlatformMisc::GetUBTPlatform(),
		*IFileManager::Get().ConvertToAbsolutePathForExternalAppForRead(*FPaths::GetProjectFilePath()));

	const bool bBuilt = DesktopPlatform->RunUnrealBuildTool(
		LOCTEXT("BuildingHost", "Building DeviceExplorerHost..."),
		FPaths::RootDir(),
		Arguments,
		GWarn);

	if (!bBuilt || !FPaths::FileExists(FindHostExecutable()))
	{
		Notify(LOCTEXT("BuildFailed", "DeviceExplorerHost build failed.\nSee the Output Log for details."), true);
		return false;
	}

	return true;
}

void FDeviceExplorerEditorModule::OpenDashboard()
{
	if (!IsHostRunning())
	{
		Notify(LOCTEXT("DashboardNoHost", "DeviceExplorerHost is not running.\nStart it from the DeviceExplorer status bar item."), true);
		return;
	}

	const FString URL = GetDashboardURL();
	FPlatformProcess::LaunchURL(*URL, nullptr, nullptr);
}

void FDeviceExplorerEditorModule::RegisterMenus()
{
	FToolMenuOwnerScoped OwnerScoped(this);

	UToolMenu* StatusBar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.StatusBar.ToolBar"));
	FToolMenuSection& StatusBarSection = StatusBar->AddSection(
		TEXT("DeviceExplorer"),
		FText::GetEmpty(),
		FToolMenuInsert(TEXT("SourceControl"), EToolMenuInsertType::Before));
	StatusBarSection.AddEntry(FToolMenuEntry::InitWidget(
		TEXT("DeviceExplorerStatus"),
		CreateStatusBarWidget(),
		FText::GetEmpty(),
		true,
		false));

	UToolMenu* ToolsMenu = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
	FToolMenuSection& MenuSection = ToolsMenu->FindOrAddSection(TEXT("DeviceExplorer"));
	MenuSection.AddMenuEntry(TEXT("DeviceExplorer.Start"),
	                         LOCTEXT("StartLabel", "Start DeviceExplorer"),
	                         LOCTEXT("StartTooltip", "Start the standalone DeviceExplorer host."),
	                         FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Play")),
	                         FUIAction(FExecuteAction::CreateRaw(this, &FDeviceExplorerEditorModule::StartHost)));
	MenuSection.AddMenuEntry(TEXT("DeviceExplorer.Open"),
	                         LOCTEXT("OpenLabel", "Open DeviceExplorer Dashboard"),
	                         LOCTEXT("OpenTooltip", "Open the local dashboard in a browser."),
	                         FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.BrowseContent")),
	                         FUIAction(FExecuteAction::CreateRaw(this, &FDeviceExplorerEditorModule::OpenDashboard)));
	MenuSection.AddMenuEntry(TEXT("DeviceExplorer.Restart"),
	                         LOCTEXT("RestartLabel", "Restart DeviceExplorer"),
	                         LOCTEXT("RestartTooltip", "Restart the standalone host process."),
	                         FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.ViewportScalabilityReset")),
	                         FUIAction(FExecuteAction::CreateRaw(this, &FDeviceExplorerEditorModule::RestartHost)));
	MenuSection.AddMenuEntry(TEXT("DeviceExplorer.Stop"),
	                         LOCTEXT("StopLabel", "Stop DeviceExplorer"),
	                         LOCTEXT("StopTooltip", "Stop the host process started by this editor."),
	                         FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Toolbar.Stop")),
	                         FUIAction(FExecuteAction::CreateRaw(this, &FDeviceExplorerEditorModule::StopHost)));
}

TSharedRef<SWidget> FDeviceExplorerEditorModule::CreateStatusBarWidget()
{
	return SNew(SComboButton)
		.ContentPadding(FMargin(6.0f, 0.0f))
		.MenuPlacement(MenuPlacement_AboveAnchor)
		.ComboButtonStyle(&FAppStyle::Get().GetWidgetStyle<FComboButtonStyle>(TEXT("SimpleComboButton")))
		.OnGetMenuContent(FOnGetContent::CreateRaw(this, &FDeviceExplorerEditorModule::BuildStatusBarMenu))
		.ToolTipText_Lambda([this]
		{
			return IsHostRunning()
				? FText::Format(LOCTEXT("StatusRunning", "DeviceExplorerHost is running.\nDashboard: {0}"), FText::FromString(GetDashboardURL()))
				: LOCTEXT("StatusStopped", "DeviceExplorerHost is stopped.");
		})
		.ButtonContent()
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 5.0f, 0.0f)
			[
				SNew(SImage)
				.Image(FAppStyle::Get().GetBrush(TEXT("Icons.Server")))
				.ColorAndOpacity_Lambda([this] { return IsHostRunning() ? FStyleColors::AccentGreen : FStyleColors::Foreground; })
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(LOCTEXT("StatusBarLabel", "DeviceExplorer"))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(5.0f, 0.0f, 0.0f, 0.0f)
			[
				SNew(STextBlock)
				.TextStyle(&FAppStyle::Get().GetWidgetStyle<FTextBlockStyle>(TEXT("SmallText")))
				.ColorAndOpacity(FStyleColors::AccentGreen)
				.Visibility_Lambda([this] { return IsHostRunning() ? EVisibility::Visible : EVisibility::Collapsed; })
				.Text_Lambda([this] { return FText::FromString(FString::FromInt(GetDefault<UDeviceExplorerEditorSettings>()->DashboardPort)); })
			]
		];
}

TSharedRef<SWidget> FDeviceExplorerEditorModule::BuildStatusBarMenu()
{
	FMenuBuilder MenuBuilder(true, nullptr);
	const bool bRunning = IsHostRunning();

	MenuBuilder.BeginSection(NAME_None, LOCTEXT("HostSection", "DeviceExplorer Host"));
	if (bRunning)
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("StatusBarOpenLabel", "Open Dashboard"),
			LOCTEXT("StatusBarOpenTooltip", "Open the local dashboard in a browser."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.BrowseContent")),
			FUIAction(FExecuteAction::CreateRaw(this, &FDeviceExplorerEditorModule::OpenDashboard)));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("StatusBarRestartLabel", "Restart Host"),
			LOCTEXT("StatusBarRestartTooltip", "Restart the standalone host process."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.ViewportScalabilityReset")),
			FUIAction(FExecuteAction::CreateRaw(this, &FDeviceExplorerEditorModule::RestartHost)));
		MenuBuilder.AddMenuEntry(
			LOCTEXT("StatusBarStopLabel", "Stop Host"),
			LOCTEXT("StatusBarStopTooltip", "Stop the host process started by this editor."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Toolbar.Stop")),
			FUIAction(FExecuteAction::CreateRaw(this, &FDeviceExplorerEditorModule::StopHost)));
	}
	else
	{
		MenuBuilder.AddMenuEntry(
			LOCTEXT("StatusBarStartLabel", "Start Host"),
			LOCTEXT("StatusBarStartTooltip", "Start the standalone DeviceExplorer host."),
			FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Play")),
			FUIAction(FExecuteAction::CreateRaw(this, &FDeviceExplorerEditorModule::StartHost)));
	}
	MenuBuilder.EndSection();

	MenuBuilder.BeginSection(NAME_None);
	MenuBuilder.AddMenuEntry(
		LOCTEXT("SettingsLabel", "Settings..."),
		LOCTEXT("SettingsTooltip", "Open the DeviceExplorer editor preferences."),
		FSlateIcon(FAppStyle::GetAppStyleSetName(), TEXT("Icons.Settings")),
		FUIAction(FExecuteAction::CreateLambda([]
		{
			const UDeviceExplorerEditorSettings* Settings = GetDefault<UDeviceExplorerEditorSettings>();
			if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>(TEXT("Settings")))
			{
				SettingsModule->ShowViewer(Settings->GetContainerName(), Settings->GetCategoryName(), Settings->GetSectionName());
			}
		})));
	MenuBuilder.EndSection();

	return MenuBuilder.MakeWidget();
}

FString FDeviceExplorerEditorModule::FindHostExecutable() const
{
#if PLATFORM_WINDOWS
	const TCHAR* ExecutableName = TEXT("DeviceExplorerHost.exe");
#else
	const TCHAR* ExecutableName = TEXT("DeviceExplorerHost");
#endif

	return FPaths::ConvertRelativePathToFull(FPaths::Combine(FPaths::ProjectDir(), TEXT("Binaries"), FPlatformProcess::GetBinariesSubdirectory(), ExecutableName));
}

FString FDeviceExplorerEditorModule::GetDashboardURL() const
{
	return FString::Printf(TEXT("http://127.0.0.1:%d"), GetDefault<UDeviceExplorerEditorSettings>()->DashboardPort);
}

void FDeviceExplorerEditorModule::Notify(const FText& Text, const bool bFailure) const
{
	// Slate can already be torn down when this runs from ShutdownModule() during engine exit.
	if (!FSlateApplication::IsInitialized())
	{
		return;
	}

	FNotificationInfo Info(Text);
	Info.ExpireDuration = bFailure ? 8.0f : 3.0f;
	Info.bUseSuccessFailIcons = true;
	const TSharedPtr<SNotificationItem> Item = FSlateNotificationManager::Get().AddNotification(Info);
	if (Item.IsValid())
	{
		Item->SetCompletionState(bFailure ? SNotificationItem::CS_Fail : SNotificationItem::CS_Success);
	}
}

IMPLEMENT_MODULE(FDeviceExplorerEditorModule, DeviceExplorerEditor)

#undef LOCTEXT_NAMESPACE
