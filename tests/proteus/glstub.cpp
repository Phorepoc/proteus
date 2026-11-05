// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
// the one hook of proteusgl.cpp a host test has no use for (the native window of the guest's RWindow)
#include "../../os/src/proteus/proteusrt.h"
namespace proteus { bool GlWindowSurface(CArmCpu&, THle&, uint32_t) { return false; } }
