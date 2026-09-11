// Copyright 2008 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <string>

namespace Common
{
void* AllocateExecutableMemory(size_t size);

// Code addresses remain execution addresses, including mutable emitter cursors.
// Translate only the destination of actual stores on dual-mapped platforms.
#ifdef __SWITCH__
void* GetWritableJitAddress(void* executable_address, size_t size);
void FlushJitCode(void* executable_address, size_t size);
#else
inline void* GetWritableJitAddress(void* address, size_t) { return address; }
#endif

// These two functions control the executable/writable state of the W^X memory
// allocations. More detailed documentation about them is in the .cpp file.
// In general where applicable the ScopedJITPageWriteAndNoExecute wrapper
// should be used to prevent bugs from not pairing up the calls properly.

// Allows a thread to write to executable memory, but not execute the data.
void JITPageWriteEnableExecuteDisable();
// Allows a thread to execute memory allocated for execution, but not write to it.
void JITPageWriteDisableExecuteEnable();
// RAII Wrapper around JITPageWrite*Execute*(). When this is in scope the thread can
// write to executable memory but not execute it.
struct ScopedJITPageWriteAndNoExecute
{
  ScopedJITPageWriteAndNoExecute() { JITPageWriteEnableExecuteDisable(); }
  ~ScopedJITPageWriteAndNoExecute() { JITPageWriteDisableExecuteEnable(); }
};
void* AllocateMemoryPages(size_t size);
bool FreeMemoryPages(void* ptr, size_t size);
void* AllocateAlignedMemory(size_t size, size_t alignment);
void FreeAlignedMemory(void* ptr);
bool ReadProtectMemory(void* ptr, size_t size);
bool WriteProtectMemory(void* ptr, size_t size, bool executable = false);
bool UnWriteProtectMemory(void* ptr, size_t size, bool allowExecute = false);
size_t MemPhysical();

}  // namespace Common
