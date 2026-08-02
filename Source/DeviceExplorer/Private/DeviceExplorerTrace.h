#pragma once

#include "ProfilingDebugging/CountersTrace.h"
#include "ProfilingDebugging/CpuProfilerTrace.h"
#include "Stats/Stats.h"

DECLARE_STATS_GROUP(TEXT("DeviceExplorer"), STATGROUP_DeviceExplorer, STATCAT_Advanced);

DECLARE_CYCLE_STAT_EXTERN(TEXT("Console catalog rebuild"), STAT_DeviceExplorerCatalogRebuild, STATGROUP_DeviceExplorer, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Console catalog query"), STAT_DeviceExplorerCatalogQuery, STATGROUP_DeviceExplorer, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Console catalog index"), STAT_DeviceExplorerCatalogIndex, STATGROUP_DeviceExplorer, );
DECLARE_CYCLE_STAT_EXTERN(TEXT("Console command"), STAT_DeviceExplorerCommand, STATGROUP_DeviceExplorer, );

TRACE_DECLARE_INT_COUNTER_EXTERN(DeviceExplorerCatalogEntries);
TRACE_DECLARE_INT_COUNTER_EXTERN(DeviceExplorerCatalogRebuilds);
TRACE_DECLARE_INT_COUNTER_EXTERN(DeviceExplorerConsoleQueries);
TRACE_DECLARE_INT_COUNTER_EXTERN(DeviceExplorerCommandsExecuted);
