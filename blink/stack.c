/*-*- mode:c;indent-tabs-mode:nil;c-basic-offset:2;tab-width:8;coding:utf-8 -*-│
│ vi: set et ft=c ts=2 sts=2 sw=2 fenc=utf-8                               :vi │
╞══════════════════════════════════════════════════════════════════════════════╡
│ Copyright 2022 Justine Alexandra Roberts Tunney                              │
│                                                                              │
│ Permission to use, copy, modify, and/or distribute this software for         │
│ any purpose with or without fee is hereby granted, provided that the         │
│ above copyright notice and this permission notice appear in all copies.      │
│                                                                              │
│ THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL                │
│ WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED                │
│ WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE             │
│ AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL         │
│ DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR        │
│ PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER               │
│ TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR             │
│ PERFORMANCE OF THIS SOFTWARE.                                                │
╚─────────────────────────────────────────────────────────────────────────────*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blink/builtin.h"
#include "blink/bus.h"
#include "blink/endian.h"
#include "blink/log.h"
#include "blink/machine.h"
#include "blink/macros.h"
#include "blink/modrm.h"
#include "blink/rde.h"
#include "blink/tsan.h"
#include "blink/x86.h"

static const u8 kStackOsz[2][3] = {{4, 4, 8}, {2, 2, 2}};
static const u8 kCallOsz[2][3] = {{4, 4, 8}, {2, 2, 8}};

static bool ShouldTraceBranchTarget(u64 target) {
  static bool initialized;
  static bool enabled;
  static u64 start;
  static u64 end;
  const char *begin_env;
  const char *end_env;
  if (!initialized) {
    initialized = true;
    begin_env = getenv("OMNIKIT_BLINK_BRANCH_TARGET_START");
    end_env = getenv("OMNIKIT_BLINK_BRANCH_TARGET_END");
    if (begin_env && *begin_env && end_env && *end_env) {
      start = strtoull(begin_env, 0, 0);
      end = strtoull(end_env, 0, 0);
      enabled = true;
    }
  }
  return enabled && target >= start && target < end;
}

static void WriteStackWord(u8 *p, u64 rde, u32 osz, u64 x) {
  IGNORE_RACES_START();
  if (osz == 8) {
    Write64(p, x);
  } else if (osz == 2) {
    Write16(p, x);
  } else {
    Write32(p, x);
  }
  IGNORE_RACES_END();
}

static u64 ReadStackWord(u8 *p, u32 osz) {
  u64 x;
  IGNORE_RACES_START();
  if (osz == 8) {
    x = Read64(p);
  } else if (osz == 2) {
    x = Read16(p);
  } else {
    x = Read32(p);
  }
  IGNORE_RACES_END();
  return x;
}

static void WriteMemWord(u8 *p, u64 rde, u32 osz, u64 x) {
  if (osz == 8) {
    Store64(p, x);
  } else if (osz == 2) {
    Store16(p, x);
  } else {
    Store32(p, x);
  }
}

static u64 ReadMemWord(u8 *p, u32 osz) {
  u64 x;
  if (osz == 8) {
    x = Load64(p);
  } else if (osz == 2) {
    x = Load16(p);
  } else {
    x = Load32(p);
  }
  return x;
}

static void PushN(P, u64 x, unsigned mode, unsigned osz) {
  u8 *w;
  u64 v;
  u64 entry;
  u8 b[8];
  void *p[2];
  switch (mode) {
    case XED_MODE_REAL:
      v = (Get32(m->sp) - osz) & 0xffff;
      Put16(m->sp, v);
      v += m->ss.base;
      break;
    case XED_MODE_LEGACY:
      v = (Get32(m->sp) - osz) & 0xffffffff;
      Put64(m->sp, v);
      v += m->ss.base;
      break;
    case XED_MODE_LONG:
      v = (Get64(m->sp) - osz) & 0xffffffffffffffff;
      Put64(m->sp, v);
      break;
    default:
      __builtin_unreachable();
  }
  if (getenv("OMNIKIT_BLINK_BRANCH_TRACE") != NULL && x && (x & 1) &&
      v == Get64(m->r15)) {
    entry = FindPageTableEntry(m, x & -4096);
    if (!(entry & PAGE_V) || (entry & PAGE_XD)) {
      fprintf(stderr,
              "[branch-trace:push] src=%#" PRIx64 " dst_slot=%#" PRIx64
              " value=%#" PRIx64 " pte=%#" PRIx64 "\n",
              m->ip - Oplength(rde), v, x, entry);
      fflush(stderr);
    }
  }
  w = AccessRam(m, v, osz, p, b, false);
  WriteStackWord(w, rde, osz, x);
  EndStore(m, v, osz, p, b);
}

void Push(P, u64 x) {
  PushN(A, x, Mode(rde), kStackOsz[Osz(rde)][Mode(rde)]);
}

void OpPushZvq(P) {
  int osz = kStackOsz[Osz(rde)][Mode(rde)];
  PushN(A, ReadStackWord(RegRexbSrm(m, rde), osz), Mode(rde), osz);
  if (IsMakingPath(m) && HasLinearMapping() && !Osz(rde)) {
    Jitter(A,
           "a1i"
           "m",
           RexbSrm(rde), FastPush);
  }
}

static u64 PopN(P, u16 extra, unsigned osz) {
  u64 v;
  u8 b[8];
  void *p[2];
  switch (Mode(rde)) {
    case XED_MODE_LONG:
      v = Get64(m->sp);
      Put64(m->sp, v + osz + extra);
      break;
    case XED_MODE_LEGACY:
      v = Get32(m->sp);
      Put64(m->sp, (v + osz + extra) & 0xffffffff);
      v += m->ss.base;
      break;
    case XED_MODE_REAL:
      v = Get16(m->sp);
      Put16(m->sp, v + osz + extra);
      v += m->ss.base;
      break;
    default:
      __builtin_unreachable();
  }
  return ReadStackWord(AccessRam(m, v, osz, p, b, true), osz);
}

u64 Pop(P, u16 extra) {
  return PopN(A, extra, kStackOsz[Osz(rde)][Mode(rde)]);
}

void OpPopZvq(P) {
  u64 x;
  int osz;
  u64 source;
  u64 old_sp;
  osz = kStackOsz[Osz(rde)][Mode(rde)];
  source = m->ip - Oplength(rde);
  old_sp = Get64(m->sp);
  x = PopN(A, 0, osz);
  if (getenv("OMNIKIT_BLINK_BRANCH_TRACE") != NULL && x && (x & 1)) {
    u64 entry = FindPageTableEntry(m, x & -4096);
    if (!(entry & PAGE_V) || (entry & PAGE_XD)) {
      u8 *src = SpyAddress(m, source - 8);
      u8 *stk = SpyAddress(m, old_sp);
      fprintf(stderr,
              "[branch-trace:pop] src=%#" PRIx64 " slot=%#" PRIx64
              " value=%#" PRIx64 " pte=%#" PRIx64 " reg=%d\n",
              source, old_sp, x, entry, RexbSrm(rde));
      if (src) {
        fprintf(stderr,
                "[branch-trace:pop] src_window=%02x %02x %02x %02x %02x %02x"
                " %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x\n",
                src[0], src[1], src[2], src[3], src[4], src[5], src[6],
                src[7], src[8], src[9], src[10], src[11], src[12], src[13],
                src[14], src[15]);
      }
      if (stk) {
        fprintf(stderr,
                "[branch-trace:pop] stack_qwords=%#" PRIx64 " %#" PRIx64
                " %#" PRIx64 "\n",
                Read64(stk + 0), Read64(stk + 8), Read64(stk + 16));
      }
      fflush(stderr);
    }
  }
  switch (osz) {
    case 8:
    case 4:
      Put64(RegRexbSrm(m, rde), x);
      break;
    case 2:
      Put16(RegRexbSrm(m, rde), x);
      break;
    default:
      __builtin_unreachable();
  }
  if (IsMakingPath(m) && HasLinearMapping() && !Osz(rde)) {
    Jitter(A,
           "a1i"
           "m",
           RexbSrm(rde), FastPop);
  }
}

static void OpCall(P, u64 func) {
  if (getenv("OMNIKIT_BLINK_BRANCH_TRACE") != NULL ||
      ShouldTraceBranchTarget(func)) {
    u64 entry = FindPageTableEntry(m, func & -4096);
    if (ShouldTraceBranchTarget(func) || !(entry & PAGE_V) || (entry & PAGE_XD)) {
      fprintf(stderr,
              "[branch-trace:call] src=%#" PRIx64 " dst=%#" PRIx64
              " pte=%#" PRIx64 "\n",
              m->ip - Oplength(rde), func, entry);
      fflush(stderr);
    }
  }
  PushN(A, m->ip, Mode(rde), kCallOsz[Osz(rde)][Mode(rde)]);
  m->ip = func;
}

void OpCallJvds(P) {
  OpCall(A, m->ip + disp);
  if (HasLinearMapping() && IsMakingPath(m)) {
    Terminate(A, FastCall);
  }
}

static u64 LoadAddressFromMemory(P) {
  unsigned osz = kCallOsz[Osz(rde)][Mode(rde)];
  return ReadMemWord(GetModrmRegisterWordPointerRead(A, osz), osz);
}

void OpCallEq(P) {
  if (IsMakingPath(m) && HasLinearMapping() && !Osz(rde)) {
    Jitter(A,
           "z3B"    // res0 = GetRegOrMem[force64bit](RexbRm)
           "s0a1="  // arg1 = machine
           "t"      // arg0 = res0
           "m",     // call micro-op (FastCallAbs)
           FastCallAbs);
  }
  OpCall(A, LoadAddressFromMemory(A));
}

void OpJmpEq(P) {
  if (IsMakingPath(m) && HasLinearMapping() && !Osz(rde)) {
    Jitter(A,
           "z3B"    // res0 = GetRegOrMem[force64bit](RexbRm)
           "s0a1="  // arg1 = machine
           "t"      // arg0 = res0
           "m",     // call micro-op (FastJmpAbs)
           FastJmpAbs);
  }
  {
    u64 func = LoadAddressFromMemory(A);
    if (getenv("OMNIKIT_BLINK_BRANCH_TRACE") != NULL ||
        ShouldTraceBranchTarget(func)) {
      u64 entry = FindPageTableEntry(m, func & -4096);
      if (ShouldTraceBranchTarget(func) || !(entry & PAGE_V) || (entry & PAGE_XD)) {
        fprintf(stderr,
                "[branch-trace:jmp] src=%#" PRIx64 " dst=%#" PRIx64
                " pte=%#" PRIx64 "\n",
                m->ip - Oplength(rde), func, entry);
        fflush(stderr);
      }
    }
    m->ip = func;
  }
}

void OpEnter(P) {
  u16 allocsz = (u16)uimm0;
  u8 nesting = (u8)(uimm0 >> 16);
  unsigned osz = kStackOsz[Osz(rde)][Mode(rde)];
  if (nesting != 0) OpUdImpl(m);
  Push(A, Get64(m->bp));
  switch (osz) {
    case 8:
      Put64(m->bp, Get64(m->sp));
      Put64(m->sp, Get64(m->sp) - allocsz);
      break;
    case 4:
      Put64(m->bp, Get32(m->sp));
      Put64(m->sp, Get32(m->sp) - allocsz);
      break;
    case 2:
      Put16(m->bp, Get16(m->sp));
      Put16(m->sp, Get16(m->sp) - allocsz);
      break;
    default:
      __builtin_unreachable();
  }
}

void OpLeave(P) {
  unsigned osz = kStackOsz[Osz(rde)][Mode(rde)];
  switch (osz) {
    case 8:
      Put64(m->sp, Get64(m->bp));
      Put64(m->bp, Pop(A, 0));
      if (HasLinearMapping() && IsMakingPath(m)) {
        Jitter(A, "m", FastLeave);
      }
      break;
    case 4:
      Put64(m->sp, Get32(m->bp));
      Put64(m->bp, PopN(A, osz, 0));
      break;
    case 2:
      Put16(m->sp, Get16(m->bp));
      Put16(m->bp, PopN(A, osz, 0));
      break;
    default:
      __builtin_unreachable();
  }
}

void OpRet(P) {
  u64 source = m->ip - Oplength(rde);
  u64 old_sp = Get64(m->sp);
  m->ip = Pop(A, 0);
  if (getenv("OMNIKIT_BLINK_BRANCH_TRACE") != NULL ||
      ShouldTraceBranchTarget(m->ip)) {
    u64 entry = FindPageTableEntry(m, m->ip & -4096);
    if (ShouldTraceBranchTarget(m->ip) || !(entry & PAGE_V) ||
        (entry & PAGE_XD)) {
      u8 *src = SpyAddress(m, source - 8);
      u8 *stk = SpyAddress(m, old_sp);
      fprintf(stderr,
              "[branch-trace:ret] src=%#" PRIx64 " dst=%#" PRIx64
              " pte=%#" PRIx64 " sp_before=%#" PRIx64 " sp_after=%#" PRIx64
              "\n",
              source, m->ip, entry, old_sp, Get64(m->sp));
      if (src) {
        fprintf(stderr,
                "[branch-trace:ret] src_window=%02x %02x %02x %02x %02x %02x"
                " %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x\n",
                src[0], src[1], src[2], src[3], src[4], src[5], src[6],
                src[7], src[8], src[9], src[10], src[11], src[12], src[13],
                src[14], src[15]);
      }
      if (stk) {
        fprintf(stderr,
                "[branch-trace:ret] stack_qwords=%#" PRIx64 " %#" PRIx64
                " %#" PRIx64 "\n",
                Read64(stk + 0), Read64(stk + 8), Read64(stk + 16));
      }
      fflush(stderr);
    }
  }
  if (IsMakingPath(m) && HasLinearMapping() && !Osz(rde)) {
#ifdef __x86_64__
    Jitter(A,
           "a1i"  // arg1 = prediction
           "m"    // call micro-op (PredictRet)
           "q",   // arg0 = machine
           m->ip, PredictRet);
    AlignJit(m->path.jb, 8, 3);
    u8 code[] = {
        0x48, 0x85, 0300 | kJitRes0 << 3 | kJitRes0,  // test %rax,%rax
        0x75, 0x05,                                   // jnz   +5
    };
#else
    Jitter(A,
           "a1i"    // arg1 = prediction
           "m"      // call micro-op (PredictRet)
           "r0a2="  // arg2 = res0
           "q",     // arg0 = machine
           m->ip, PredictRet);
    u32 code[] = {
        0xb5000000 | (8 / 4) << 5 | kJitArg2,  // cbnz x2,#8
    };
#endif
    AppendJit(m->path.jb, code, sizeof(code));
    Connect(A, m->ip, true);
    AppendJitJump(m->path.jb, (void *)m->system->ender);
    FinishPath(m);
  }
}

relegated void OpRetIw(P) {
  u64 source = m->ip - Oplength(rde);
  u64 old_sp = Get64(m->sp);
  m->ip = Pop(A, uimm0);
  if (getenv("OMNIKIT_BLINK_BRANCH_TRACE") != NULL ||
      ShouldTraceBranchTarget(m->ip)) {
    u64 entry = FindPageTableEntry(m, m->ip & -4096);
    if (ShouldTraceBranchTarget(m->ip) || !(entry & PAGE_V) ||
        (entry & PAGE_XD)) {
      u8 *src = SpyAddress(m, source - 8);
      u8 *stk = SpyAddress(m, old_sp);
      fprintf(stderr,
              "[branch-trace:ret-imm] src=%#" PRIx64 " dst=%#" PRIx64
              " pte=%#" PRIx64 " sp_before=%#" PRIx64 " sp_after=%#" PRIx64
              "\n",
              source, m->ip, entry, old_sp, Get64(m->sp));
      if (src) {
        fprintf(stderr,
                "[branch-trace:ret-imm] src_window=%02x %02x %02x %02x %02x"
                " %02x %02x %02x | %02x %02x %02x %02x %02x %02x %02x %02x\n",
                src[0], src[1], src[2], src[3], src[4], src[5], src[6],
                src[7], src[8], src[9], src[10], src[11], src[12], src[13],
                src[14], src[15]);
      }
      if (stk) {
        fprintf(stderr,
                "[branch-trace:ret-imm] stack_qwords=%#" PRIx64 " %#" PRIx64
                " %#" PRIx64 "\n",
                Read64(stk + 0), Read64(stk + 8), Read64(stk + 16));
      }
      fflush(stderr);
    }
  }
}

void OpPushEvq(P) {
  unsigned osz = kStackOsz[Osz(rde)][Mode(rde)];
  Push(A, ReadMemWord(GetModrmRegisterWordPointerRead(A, osz), osz));
}

void OpPopEvq(P) {
  unsigned osz = kStackOsz[Osz(rde)][Mode(rde)];
  u64 x = Pop(A, 0);
  /* POP computes its memory destination after incrementing SP. */
  WriteMemWord(GetModrmRegisterWordPointerWrite(A, osz), rde, osz, x);
}
