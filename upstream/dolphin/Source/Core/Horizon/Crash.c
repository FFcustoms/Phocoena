// SPDX-License-Identifier: GPL-2.0-or-later
#include <switch.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

__attribute__((aligned(16))) u8 __nx_exception_stack[0x10000];
u64 __nx_exception_stack_size = sizeof(__nx_exception_stack);
static int crash_fd = -1;
bool HorizonTryHandlePpcFastmemException(ThreadExceptionDump* context);

bool HorizonOpenCrashLog(void)
{
  // Preserve a fault across the next launch so returning to the menu does not
  // erase the evidence before the tester can retrieve it.
  struct stat status;
  const char* current = "sdmc:/switch/Phocoena/logs/crash.log";
  const char* previous = "sdmc:/switch/Phocoena/logs/crash.previous.log";
  if (stat(current, &status) == 0 && status.st_size > 0)
  {
    unlink(previous);
    if (rename(current, previous) != 0)
      return false;  // Do not truncate evidence if rotation failed.
  }
  crash_fd = open("sdmc:/switch/Phocoena/logs/crash.log", O_WRONLY | O_CREAT | O_TRUNC, 0666);
  return crash_fd >= 0;
}
void HorizonCloseCrashLog(void)
{
  if (crash_fd >= 0) close(crash_fd);
  crash_fd = -1;
}
static void DumpRegister(const char* label, u64 value)
{
  char line[64];
  unsigned n = 0;
  while (*label && n < 40) line[n++] = *label++;
  line[n++] = '='; line[n++] = '0'; line[n++] = 'x';
  for (int shift = 60; shift >= 0; shift -= 4) line[n++] = "0123456789abcdef"[(value >> shift) & 15];
  line[n++] = '\n';
  if (crash_fd >= 0) (void)write(crash_fd, line, n);
}

// Mesa calls fatalThrow on native buffer queue errors, bypassing the ordinary
// CPU exception handler. Keep the real fatal behavior but save its Result first.
void __attribute__((noreturn)) __real_fatalThrow(Result result);
void __attribute__((noreturn)) __real_diagAbortWithResult(Result result);
void __attribute__((noreturn)) __wrap_fatalThrow(Result result);
void __attribute__((noreturn)) __wrap_diagAbortWithResult(Result result);

void __wrap_fatalThrow(Result result)
{
  const char message[] = "ERROR: libnx fatalThrow (driver/service failure)\n";
  if (crash_fd >= 0) (void)write(crash_fd, message, sizeof(message) - 1);
  DumpRegister("Result", result);
  DumpRegister("caller LR", (u64)__builtin_return_address(0));
  DumpRegister("fatal wrapper (ASLR ref)", (u64)__wrap_fatalThrow);
  __real_fatalThrow(result);
}

void __wrap_diagAbortWithResult(Result result)
{
  const char message[] = "ERROR: libnx diagAbortWithResult\n";
  if (crash_fd >= 0) (void)write(crash_fd, message, sizeof(message) - 1);
  DumpRegister("Result", result);
  DumpRegister("caller LR", (u64)__builtin_return_address(0));
  DumpRegister("abort wrapper (ASLR ref)", (u64)__wrap_diagAbortWithResult);
  __real_diagAbortWithResult(result);
}

void __libnx_exception_handler(ThreadExceptionDump* context)
{
  // The upstream fastmem backpatcher resumes a recognized JIT load/store fault
  // directly. All other exceptions continue through the existing reporter.
  if (HorizonTryHandlePpcFastmemException(context))
    __builtin_unreachable();

  // Best effort only: SD IPC can fail in exception context. No heap, stdio,
  // formatting libraries, or application mutexes are used here. Atmosphere's
  // own fatal report remains the fallback if the process cannot write.
  const char message[] = "ERROR: Exception: unrecoverable Horizon fault\n";
  if (crash_fd >= 0) (void)write(crash_fd, message, sizeof(message) - 1);
  DumpRegister("description", context->error_desc);
  DumpRegister("PC", context->pc.x); DumpRegister("LR", context->lr.x);
  DumpRegister("SP", context->sp.x); DumpRegister("FAR", context->far.x);
  DumpRegister("ESR", context->esr);
  DumpRegister("handler (ASLR reference)", (u64)__libnx_exception_handler);
  svcReturnFromException(MAKERESULT(Module_Libnx, LibnxError_BadInput));
  __builtin_unreachable();
}
