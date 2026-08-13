#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"

#include "DeviceExplorerEditorSettings.generated.h"

UCLASS(Config = EditorPerProjectUserSettings, meta = (DisplayName = "DeviceExplorer"))
class DEVICEEXPLOREREDITOR_API UDeviceExplorerEditorSettings final : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }

	UPROPERTY(Config, EditAnywhere, Category = "Startup", meta = (DisplayName = "Start with Unreal Editor"))
	bool bAutoStart = false;

	UPROPERTY(Config, EditAnywhere, Category = "Server", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 DashboardPort = 18080;

	UPROPERTY(Config, EditAnywhere, Category = "Server", meta = (ClampMin = "1024", ClampMax = "65535"))
	int32 DevicePort = 18081;

	UPROPERTY(Config, EditAnywhere, Category = "Distributed", meta = (DisplayName = "Enable distributed mode"))
	bool bEnableDistributedMode = false;

	UPROPERTY(Config, EditAnywhere, Category = "Distributed", meta = (EditCondition = "bEnableDistributedMode"))
	FString ClusterId;

	UPROPERTY(Config, EditAnywhere, Category = "Distributed", meta = (EditCondition = "bEnableDistributedMode", PasswordField = "true"))
	FString PeerSecret;

	UPROPERTY(Config, EditAnywhere, Category = "Distributed", meta = (EditCondition = "bEnableDistributedMode", ClampMin = "0", ClampMax = "65535"))
	int32 PeerPort = 0;

	UPROPERTY(Config, EditAnywhere, Category = "Lifecycle", meta = (DisplayName = "Stop server with Unreal Editor"))
	bool bStopWithEditor = true;
};
