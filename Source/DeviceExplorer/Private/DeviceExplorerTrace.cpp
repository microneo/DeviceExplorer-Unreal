#include "DeviceExplorerTrace.h"

DEFINE_STAT(STAT_DeviceExplorerCatalogRebuild);
DEFINE_STAT(STAT_DeviceExplorerCatalogQuery);
DEFINE_STAT(STAT_DeviceExplorerCatalogIndex);
DEFINE_STAT(STAT_DeviceExplorerCommand);

TRACE_DECLARE_INT_COUNTER(DeviceExplorerCatalogEntries, TEXT("DeviceExplorer/CatalogEntries"));
TRACE_DECLARE_INT_COUNTER(DeviceExplorerCatalogRebuilds, TEXT("DeviceExplorer/CatalogRebuilds"));
TRACE_DECLARE_INT_COUNTER(DeviceExplorerConsoleQueries, TEXT("DeviceExplorer/ConsoleQueries"));
TRACE_DECLARE_INT_COUNTER(DeviceExplorerCommandsExecuted, TEXT("DeviceExplorer/CommandsExecuted"));
