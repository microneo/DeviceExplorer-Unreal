#pragma once

#include "CoreMinimal.h"
#include "HAL/PlatformProcess.h"
#include "Modules/ModuleManager.h"

class DEVICEEXPLOREREDITOR_API FDeviceExplorerEditorModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	bool IsHostRunning();
	void StartHost();
	void StopHost();
	void RestartHost();
	void OpenDashboard() const;

private:
	void RegisterMenus();
	void ToggleHost();
	FString FindHostExecutable() const;
	FString GetDashboardURL() const;
	void Notify(const FText& Text, bool bFailure = false) const;

	FProcHandle HostProcess;
	uint32 HostProcessId = 0;
	bool bStopWithEditor = true;
	FString CurrentHostToken;
};
