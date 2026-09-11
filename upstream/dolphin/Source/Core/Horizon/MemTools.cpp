// SPDX-License-Identifier: GPL-2.0-or-later
#include "Core/MemTools.h"

#include <cstddef>

#include <switch.h>

#include "Core/MachineContext.h"
#include "Core/PowerPC/JitInterface.h"
#include "Core/System.h"
#include "Horizon/Fastmem.h"

namespace EMM
{
extern "C" bool HorizonTryHandlePpcFastmemException(ThreadExceptionDump* ctx);

namespace
{
static_assert(offsetof(ThreadExceptionDump, cpu_gprs) == 16);
static_assert(offsetof(ThreadExceptionDump, fp) == 248);
static_assert(offsetof(ThreadExceptionDump, lr) == 256);
static_assert(offsetof(ThreadExceptionDump, sp) == 264);
static_assert(offsetof(ThreadExceptionDump, pc) == 272);
static_assert(offsetof(ThreadExceptionDump, fpu_gprs) == 288);
static_assert(offsetof(ThreadExceptionDump, pstate) == 800);
static_assert(offsetof(ThreadExceptionDump, far) == 816);

[[noreturn]] void RestoreContextAndJump(ThreadExceptionDump* ctx)
{
  // libnx enters this handler instead of returning through a kernel-managed
  // signal frame. Restore the complete AArch64/NEON state and branch to the PC
  // that Dolphin's existing backpatch handler selected. x18 is the reserved
  // Horizon platform register and is used only as the final branch scratch.
  __asm__ volatile(
      "mov x21, %0\n"
      "ldp q0,  q1,  [x21, #288]\n"
      "ldp q2,  q3,  [x21, #320]\n"
      "ldp q4,  q5,  [x21, #352]\n"
      "ldp q6,  q7,  [x21, #384]\n"
      "ldp q8,  q9,  [x21, #416]\n"
      "ldp q10, q11, [x21, #448]\n"
      "ldp q12, q13, [x21, #480]\n"
      "ldp q14, q15, [x21, #512]\n"
      "ldp q16, q17, [x21, #544]\n"
      "ldp q18, q19, [x21, #576]\n"
      "ldp q20, q21, [x21, #608]\n"
      "ldp q22, q23, [x21, #640]\n"
      "ldp q24, q25, [x21, #672]\n"
      "ldp q26, q27, [x21, #704]\n"
      "ldp q28, q29, [x21, #736]\n"
      "ldp q30, q31, [x21, #768]\n"
      "ldr w16, [x21, #800]\n"
      "msr nzcv, x16\n"
      "ldr x16, [x21, #264]\n"
      "ldr x18, [x21, #272]\n"
      "str x18, [x16, #-16]!\n"
      "mov x18, x16\n"
      "ldr x30, [x21, #256]\n"
      "ldr x29, [x21, #248]\n"
      "ldp x0,  x1,  [x21, #16]\n"
      "ldp x2,  x3,  [x21, #32]\n"
      "ldp x4,  x5,  [x21, #48]\n"
      "ldp x6,  x7,  [x21, #64]\n"
      "ldp x8,  x9,  [x21, #80]\n"
      "ldp x10, x11, [x21, #96]\n"
      "ldp x12, x13, [x21, #112]\n"
      "ldp x14, x15, [x21, #128]\n"
      "ldr x16, [x21, #144]\n"
      "ldr x17, [x21, #152]\n"
      "ldr x19, [x21, #168]\n"
      "ldr x20, [x21, #176]\n"
      "ldp x22, x23, [x21, #192]\n"
      "ldp x24, x25, [x21, #208]\n"
      "ldp x26, x27, [x21, #224]\n"
      "ldr x28, [x21, #240]\n"
      "mov sp, x18\n"
      "ldr x21, [x21, #184]\n"
      "ldr x18, [sp], #16\n"
      "br x18\n"
      :
      : "r"(ctx)
      : "memory");
  __builtin_unreachable();
}
}  // namespace

// Crash.c remains the sole libnx exception entry point so ordinary faults keep
// using the established allocation-free crash reporter. A handled JIT fault
// resumes directly and therefore never returns to Crash.c.
extern "C" bool HorizonTryHandlePpcFastmemException(ThreadExceptionDump* ctx)
{
  if (ctx && Horizon::IsPpcFastmemExceptionRecoveryEnabled())
  {
    const uintptr_t fault_address = ctx->far.x;
    SContext sctx{};
    for (int i = 0; i < 29; ++i)
      sctx.regs[i] = ctx->cpu_gprs[i].x;
    sctx.fp = ctx->fp.x;
    sctx.lr = ctx->lr.x;
    sctx.sp = ctx->sp.x;
    sctx.pc = ctx->pc.x;
    sctx.pstate = ctx->pstate;
    sctx.far = fault_address;

    const bool handled =
        Core::System::GetInstance().GetJitInterface().HandleFault(fault_address, &sctx);
    Horizon::RecordPpcFastmemFault(handled);
    if (handled)
    {
      ctx->pc.x = sctx.pc;
      for (int i = 0; i < 29; ++i)
        ctx->cpu_gprs[i].x = sctx.regs[i];
      ctx->fp.x = sctx.fp;
      ctx->lr.x = sctx.lr;
      ctx->sp.x = sctx.sp;
      RestoreContextAndJump(ctx);
    }
  }
  return false;
}

void InstallExceptionHandler()
{
}

void UninstallExceptionHandler()
{
}

bool IsExceptionHandlerSupported()
{
  return Horizon::IsPpcFastmemExceptionRecoveryEnabled();
}
}  // namespace EMM
