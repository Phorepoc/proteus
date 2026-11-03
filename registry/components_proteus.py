# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""components_proteus -- Proteus (private until finished: docs/public/README.md "Proteus" in the private repo).

Proteus runs ARM32 Symbian binaries on x32 Phorepoc OS: proteus.exe is an x32 user-side ARM loader + interpreter.
This repository is a layer (PHOREPOC_ADAPTATIONS=<board adaptations>:<path to proteus>).

  x32 (the PC):   phe component build proteus
  ARM test EXEs:  phe component build -arch arm proteushello proteusdll proteusdllexe proteusfs proteustrap proteusheap
                  then recipes/stage_arm.sh copies them (and the ARM euser/efsrv) into $PHOREPOC_BUILD/proteus/arm/ of the
                  x32 build, where the ROM data below picks them up.
  Board test:     SSM_TEST=proteus phe image rom -b pc   (P0..P2d run at boot; "PH: proteus ... PASS" lines)
"""
import os

from paths import OS_ROOT  # noqa: E402

_ARM = ".proteus/arm/"
COMPONENTS = {
    # proteus.exe: the loader + interpreter (x32). ROM data: the ARM test EXEs in the per-arch directory \sys\bin\arm\
    "proteus": dict(mmp=os.path.join(OS_ROOT, "src/proteus/proteus.mmp"), test=True, cdriver=False, also_own_name=True,
                    rom_data=[r"Sys\Bin\arm\proteushello.exe=" + _ARM + "proteushello.exe",
                              r"Sys\Bin\armhello.exe=" + _ARM + "proteushello.exe",           # P1: launched by plain name (the loader's foreign-CPU redirect looks in \sys\bin\ only)
                              r"Sys\Bin\arm\proteusdll.dll=" + _ARM + "proteusdll.bp.dll",    # P2: an ARM EXE + the ARM DLL it imports (byte-pair)
                              r"Sys\Bin\proteusdllexe.exe=" + _ARM + "proteusdllexe.exe",    # EXEs in \sys\bin\ (where the loader hook looks)
                              r"Sys\Bin\proteusdllbp.exe=" + _ARM + "proteusdllexe.bp.exe",   # P2b: compressed EXE
                              r"Sys\Bin\arm\euser.dll=" + _ARM + "euser.dll",                 # P2c: the real ARM euser under proteus.exe -r
                              r"Sys\Bin\arm\efsrv.dll=" + _ARM + "efsrv.dll",                 # P2d: the ARM efsrv over the IPC bridge
                              r"Sys\Bin\arm\proteusfs.exe=" + _ARM + "proteusfs.exe"]),
    # the same program under the name core's loader hands an ARM EXE to (sf_lepoc.cpp ForeignCpuRedirect): in every ROM built with this layer
    "cpuhandler_arm": dict(mmp=os.path.join(OS_ROOT, "src/proteus/proteus.mmp"), cdriver=False, rom="cpuhandler_arm.exe"),
    # the image broker: ARM images under \sys\bin\ for a handler holding the program's capabilities (no AllFiles)
    "proteusimg": dict(mmp=os.path.join(OS_ROOT, "src/proteusimg/proteusimg.mmp"), cdriver=False),
    # the ARM test programs (ARM builds only)
    "proteushello": dict(mmp=os.path.join(OS_ROOT, "src/proteushello/proteushello.mmp"), test=True),
    "proteusdll": dict(mmp=os.path.join(OS_ROOT, "src/proteusdll/proteusdll.mmp"), test=True,
                       **{"def": os.path.join(OS_ROOT, "src/proteusdll/proteusdll.def")}),
    "proteusdllexe": dict(mmp=os.path.join(OS_ROOT, "src/proteusdllexe/proteusdllexe.mmp"), test=True),
    "proteusfs": dict(mmp=os.path.join(OS_ROOT, "src/proteusfs/proteusfs.mmp"), test=True),
    "proteustrap": dict(mmp=os.path.join(OS_ROOT, "src/proteustrap/proteustrap.mmp"), test=True),
    "proteusheap": dict(mmp=os.path.join(OS_ROOT, "src/proteusheap/proteusheap.mmp"), test=True),
}
