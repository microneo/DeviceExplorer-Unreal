// The sans-I/O sources under Private/ and Public/ include no UE header, so this
// module would otherwise keep the CRT's operator new. Its codecs hand ownership
// of std::vector and std::string buffers to callers in other modules, and in a
// modular build those free through UE's allocator: allocating here and freeing
// there corrupts the heap. Routing this module's allocation through FMemory as
// every other module does removes the mismatch.
//
// This file sits outside Private/ so the standalone CMake build, which has no
// engine available, never sees it.
//
// A monolithic target links every module into one binary that already defines
// these operators once, so the shim applies to modular builds only.
#if !IS_MONOLITHIC
#include "HAL/PerModuleInline.inl"
#endif
