// Copyright The Phorepoc Authors
// SPDX-License-Identifier: AGPL-3.0-only
// the one hook of proteusgl.cpp a host test has no use for (the native window of the guest's RWindow)
#include "../../os/src/proteus/proteusrt.h"
namespace proteus { bool GlWindowSurface(CArmCpu&, THle&, uint32_t) { return false; } }
