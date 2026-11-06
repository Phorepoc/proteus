// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// armjit.h -- OURS (Proteus P1, docs/phorepoc/research/proteus-x86.md section 4): CArmCpu's guest code run by dynarmic (0BSD, recipes/build_dynarmic.sh)
// instead of the interpreter. Built only with PROTEUS_JIT. The guest state stays in CArmCpu between runs and at every hook (SVC, trap, foreign pc,
// slice), so the embedder, the executive's thread switches and the interpreter all see one state; the JIT holds it only while guest code runs.
#ifndef PROTEUS_ARMJIT_H
#define PROTEUS_ARMJIT_H

#include <stdint.h>

namespace proteus
{
class CArmCpu;
struct TArmJit;

TArmJit* JitNew(CArmCpu& aCpu);									// null when the translator cannot start (the interpreter carries on)
void JitDelete(TArmJit* aJit);
void JitRun(TArmJit* aJit, uint64_t aLimit);					// runs until iSteps >= aLimit, a halt, or a hook the caller's loop must see
void JitMap(TArmJit* aJit, uint32_t aAddr, uint32_t aLen);		// the guest's pages in [aAddr, aAddr + aLen) changed (commit, decommit, a window)
void JitInvalidate(TArmJit* aJit, uint32_t aAddr, uint32_t aLen);	// code there changed (a module loaded or unloaded, an IMB)
}

#endif
