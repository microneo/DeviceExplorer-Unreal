#include "DeviceExplorerDiscovery.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDeviceExplorerEndpointParserTest,
                                 "DeviceExplorer.Endpoint.ParseIPv4",
                                 EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDeviceExplorerEndpointParserTest::RunTest(const FString& Parameters)
{
	(void) Parameters;
	FDeviceExplorerSerializedEndpoint Endpoint;
	FString Error;
	TestTrue(TEXT("valid IPv4 endpoint parses"), ParseDeviceExplorerEndpoint(TEXT("192.0.2.15:42111"), Endpoint, &Error));
	TestEqual(TEXT("address is preserved"), Endpoint.Address, FString(TEXT("192.0.2.15")));
	TestEqual(TEXT("port is parsed"), Endpoint.Port, 42111);
	TestTrue(TEXT("parsed endpoint is supported"), Endpoint.IsSupported());

	TestFalse(TEXT("missing port is rejected"), ParseDeviceExplorerEndpoint(TEXT("192.0.2.15"), Endpoint, &Error));
	TestFalse(TEXT("out-of-range port is rejected"), ParseDeviceExplorerEndpoint(TEXT("192.0.2.15:70000"), Endpoint, &Error));
	TestFalse(TEXT("IPv6 remains explicitly unsupported in v1"), ParseDeviceExplorerEndpoint(TEXT("[2001:db8::1]:42111"), Endpoint, &Error));
	return true;
}

#endif
