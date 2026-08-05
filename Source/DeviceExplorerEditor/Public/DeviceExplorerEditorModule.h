#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"

class SWidget;

class DEVICEEXPLOREREDITOR_API FDeviceExplorerEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	bool IsHostRunning();
	void StartHost();
	void StopHost();
	void RestartHost();
	void OpenDashboard();

private:
	void RegisterMenus();
	void LaunchHost(bool bOpenDashboard);
	TSharedRef<SWidget> CreateStatusBarWidget();
	TSharedRef<SWidget> BuildStatusBarMenu();
	bool BuildHost();
	bool InstallHostTarget(FText& OutError) const;
	bool IsHostCompatible(const FString& Executable, FText& OutError) const;
	FString FindHostExecutable() const;
	FString EnsureProjectSessionToken() const;
	FString GetDashboardURL() const;
	void Notify(const FText& Text, bool bFailure = false) const;

	FProcHandle HostProcess;
	uint32 HostProcessId = 0;
	bool bStopWithEditor = true;
	bool bClientTokenFromLaunchArgument = false;
	FString CurrentHostToken;
};
