// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// armjitnone.cpp -- the translator's entry points for the interpreter-only build (proteus_interp.mmp): no translator, the interpreter runs the guest.
#include "armjit.h"
namespace proteus
{
TArmJit* JitNew(CArmCpu&) { return 0; }
void JitDelete(TArmJit*) {}
void JitRun(TArmJit*, uint64_t) {}
void JitMap(TArmJit*, uint32_t, uint32_t) {}
void JitInvalidate(TArmJit*, uint32_t, uint32_t) {}
}
