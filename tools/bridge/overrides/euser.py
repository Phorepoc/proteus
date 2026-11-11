# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""Hand-written declarations for euser exports the 2010 headers do not (or no longer) declare, and bridge overrides.

MANUAL: mangled name -> record fields (kind, cls, ret, params as AAPCS classes; `alias` = the descriptor of another
mangled name). OVERRIDES: mangled name -> name of a hand-written C++ handler (none yet: R1 adds them for the varargs)."""
W, P, V, C = {"c": "w"}, {"c": "p"}, {"c": "v"}, {"c": "c"}
THIS = {"c": "p", "t": "this"}
MANUAL = {
    # explicit instantiations in the .def (the headers only have the template)
    **{f"_ZN13CArrayFixFlatI{t}EC{n}Ei": {"kind": "ctor", "cls": f"CArrayFixFlat<{r}>", "ret": C, "params": [THIS, W]}
       for t, r in (("4TUid", "TUid"), ("i", "TInt")) for n in (1, 2)},
    **{f"_ZN13CArrayFixFlatI{t}ED{n}Ev": {"kind": "dtor", "cls": f"CArrayFixFlat<{r}>", "ret": V if n == 0 else C, "params": [THIS]}
       for t, r in (("4TUid", "TUid"), ("i", "TInt")) for n in (0, 1, 2)},
    **{f"_ZT{k}13CArrayFixFlatI{t}E": {"kind": {"V": "vtable", "I": "typeinfo"}[k], "cls": f"CArrayFixFlat<{r}>"}
       for t, r in (("4TUid", "TUid"), ("i", "TInt")) for k in "VI"},
    # legacy spelling of Mem::Copy/Fill/Move (a class "Mem1" the headers never had): the same functions
    "_ZN4Mem14CopyEPvPKvi": {"alias": "_ZN3Mem4CopyEPvPKvi"},
    "_ZN4Mem14FillEPvi5TChar": {"alias": "_ZN3Mem4FillEPvi5TChar"},
    "_ZN4Mem14MoveEPvPKvi": {"alias": "_ZN3Mem4MoveEPvPKvi"},
    # declared only under __LEAVE_EQUALS_THROW__ / the emulator build
    "_ZNK15XLeaveException9GetReasonEv": {"kind": "method", "cls": "XLeaveException", "ret": W, "params": [THIS], "const": True},
    "_ZN4Exec19SetWin32RuntimeHookEPv": {"kind": "function", "cls": None, "ret": V, "params": [P]},
    # data defined in a .cpp, never declared in a header
    "KHeapMinCellSize": {"kind": "data", "size": 4},
    "KHeapShrinkHysRatio": {"kind": "data", "size": 4},
}
# the varargs of descriptor formatting (R1 gate 2): bridge.cpp reads the ARM argument layout (r2.. then the stack, 64-bit values in even pairs;
# or an ARM va_list = a pointer to that layout), walks the format string for the argument classes and builds the native x32 va_list the
# native TDes::FormatList / AppendFormatList read. (The plan's "AppendFormatListArm in euser" is this translation, done in Proteus: no core change.)
OVERRIDES: dict[str, str] = {
    "_ZN6TDes166FormatE11TRefByValueIK7TDesC16Ez": "BrOvr_Format16",
    "_ZN6TDes1612AppendFormatE11TRefByValueIK7TDesC16Ez": "BrOvr_AppendFormat16",
    "_ZN6TDes1612AppendFormatE11TRefByValueIK7TDesC16EP14TDes16Overflowz": "BrOvr_AppendFormatOvf16",
    "_ZN6TDes1610FormatListERK7TDesC16St9__va_list": "BrOvr_FormatList16",
    "_ZN6TDes1616AppendFormatListERK7TDesC16St9__va_listP14TDes16Overflow": "BrOvr_AppendFormatList16",
    "_ZN5TDes86FormatE11TRefByValueIK6TDesC8Ez": "BrOvr_Format8",
    "_ZN5TDes812AppendFormatE11TRefByValueIK6TDesC8Ez": "BrOvr_AppendFormat8",
    "_ZN5TDes812AppendFormatE11TRefByValueIK6TDesC8EP13TDes8Overflowz": "BrOvr_AppendFormatOvf8",
    "_ZN5TDes810FormatListERK6TDesC8St9__va_list": "BrOvr_FormatList8",
    "_ZN5TDes816AppendFormatListERK6TDesC8St9__va_listP13TDes8Overflow": "BrOvr_AppendFormatList8",
    # the process asking about itself: the guest program, not the handler (bridge.cpp; apparc names the app's resource file after FileName)
    "_ZNK8RProcess8FileNameEv": "BrOvr_ProcessFileName",
    "_ZNK8RProcess4TypeEv": "BrOvr_ProcessType",
    "_ZN4User11CommandLineER6TDes16": "BrOvr_CommandLine",
    "_ZN4User17CommandLineLengthEv": "BrOvr_CommandLineLength",
    "_ZN8RProcess13ExeExportDataEv": "BrOvr_ExeExportData",
}

# The process/thread asking about itself, audited (mixed mode: the ARM program runs in the handler's process on the native euser).
# Every self-query export of RProcess/RThread/RHandleBase/User is listed with who answers it; gen_bridge.py fails on an unlisted one
# (SELF_QUERY_CLASSES / SELF_QUERY_PREFIXES) or on an "override" without a handler. Native answers are right because the process IS the
# program's: the loader gives it the program's SecureId/VendorId/capabilities (sf_lepoc ForeignCpuRedirect), proteus renames it after the
# program (User::RenameProcess: Name/FullName of the process and of its threads, and what other processes see), Id/priority/exit/handles
# are the process's own. Only what the kernel takes from the handler's IMAGE is wrong natively: those have handlers.
NAT_LOADER, NAT_RENAME, NAT_SAME = "native: loader (program identity)", "native: renamed process", "native: the same process"
NAT_OTHER = "native: acts on a handle / another object"
SELF_QUERY_PREFIXES = ("RProcess::", "RThread::", "RHandleBase::Name", "RHandleBase::FullName", "User::CommandLine",
                       "User::ParameterLength", "User::GetDesParameter", "User::GetTIntParameter", "User::Creator", "User::Rename")
SELF: dict[str, str] = {
    # from the handler's image: answered from the guest image (bridge.cpp, the current-process handle only)
    "_ZNK8RProcess8FileNameEv": "override", "_ZNK8RProcess4TypeEv": "override", "_ZN8RProcess13ExeExportDataEv": "override",
    "_ZN4User11CommandLineER6TDes16": "override", "_ZN4User17CommandLineLengthEv": "override",
    # OPEN: the handler's code/data layout (TProcessMemoryInfo); no app is known to read it
    "_ZNK8RProcess13GetMemoryInfoER18TProcessMemoryInfo": "native: handler image (OPEN)",
    **{m: NAT_RENAME for m in ("_ZNK11RHandleBase4NameEv", "_ZNK11RHandleBase8FullNameEv", "_ZNK11RHandleBase8FullNameER6TDes16",
                               "_ZN4User13RenameProcessERK7TDesC16", "_ZN4User12RenameThreadERK7TDesC16")},
    **{m: NAT_LOADER for m in ("_ZNK8RProcess8SecureIdEv", "_ZNK8RProcess8VendorIdEv", "_ZNK7RThread8SecureIdEv", "_ZNK7RThread8VendorIdEv",
                               "_ZNK7RThread15DoHasCapabilityE11TCapabilityPKc", "_ZNK7RThread15DoHasCapabilityE11TCapabilityS0_PKc",
                               "_ZNK8RProcess15DoHasCapabilityE11TCapabilityPKc", "_ZNK8RProcess15DoHasCapabilityE11TCapabilityS0_PKc",
                               "_ZNK7RThread15DoHasCapabilityE11TCapability", "_ZNK7RThread15DoHasCapabilityE11TCapabilityS0_",
                               "_ZNK8RProcess15DoHasCapabilityE11TCapability", "_ZNK8RProcess15DoHasCapabilityE11TCapabilityS0_",
                               "_ZN4User15CreatorSecureIdEv", "_ZN4User15CreatorVendorIdEv", "_ZNK8RProcess16DefaultDataPagedEv")},
    **{m: NAT_SAME for m in (
        "_ZNK8RProcess2IdEv", "_ZNK7RThread2IdEv", "_ZNK8RProcess8PriorityEv", "_ZNK7RThread8PriorityEv", "_ZNK7RThread15ProcessPriorityEv",
        "_ZNK8RProcess11SetPriorityE16TProcessPriority", "_ZNK7RThread11SetPriorityE15TThreadPriority",
        "_ZNK7RThread18SetProcessPriorityE16TProcessPriority", "_ZNK8RProcess10ExitReasonEv", "_ZNK8RProcess12ExitCategoryEv",
        "_ZNK8RProcess8ExitTypeEv", "_ZNK7RThread10ExitReasonEv", "_ZNK7RThread12ExitCategoryEv", "_ZNK7RThread8ExitTypeEv",
        "_ZNK8RProcess10JustInTimeEv", "_ZNK8RProcess13SetJustInTimeEi", "_ZN8RProcess10DataCagingEi", "_ZN8RProcess9SecureApiEi",
        "_ZNK7RThread10GetCpuTimeER25TTimeIntervalMicroSeconds", "_ZNK7RThread11HandleCountERiS0_", "_ZNK7RThread12RequestCountEv",
        "_ZNK7RThread9StackInfoER16TThreadStackInfo", "_ZNK7RThread7ContextER5TDes8", "_ZNK7RThread7ProcessER8RProcess",
        "_ZN4User15ParameterLengthEi", "_ZN4User15GetDesParameterEiR5TDes8", "_ZN4User15GetDesParameterEiR6TDes16",
        "_ZN4User16GetTIntParameterEiRi")},
    **{m: NAT_OTHER for m in (
        "_ZN7RThread10RendezvousEi", "_ZN7RThread4KillEi", "_ZN7RThread4OpenE9TThreadId10TOwnerType", "_ZN7RThread4OpenERK7TDesC1610TOwnerType",
        "_ZN7RThread5PanicERK7TDesC16i", "_ZN7RThread6CreateERK7TDesC16PFiPvEiP10RAllocatorS3_10TOwnerType",
        "_ZN7RThread6CreateERK7TDesC16PFiPvEiiiS3_10TOwnerType", "_ZN7RThread9TerminateEi", "_ZN7RThread6CreateERK17TThreadCreateInfo",
        "_ZN8RProcess10RendezvousEi", "_ZN8RProcess12SetParameterEi11RHandleBase", "_ZN8RProcess12SetParameterEiRK15RSubSessionBase",
        "_ZN8RProcess12SetParameterEiRK6TDesC8", "_ZN8RProcess12SetParameterEiRK7TDesC16", "_ZN8RProcess12SetParameterEii",
        "_ZN8RProcess4KillEi", "_ZN8RProcess4OpenE10TProcessId10TOwnerType", "_ZN8RProcess4OpenERK7TDesC1610TOwnerType",
        "_ZN8RProcess5PanicERK7TDesC16i", "_ZN8RProcess6CreateERK7TDesC16S2_10TOwnerType",
        "_ZN8RProcess6CreateERK7TDesC16S2_RK8TUidType10TOwnerType", "_ZN8RProcess6ResumeEv", "_ZN8RProcess9TerminateEi",
        "_ZN8RProcess23CreateWithStackOverrideERK7TDesC16S2_RK8TUidTypei10TOwnerType",
        "_ZNK7RThread10RendezvousER14TRequestStatus", "_ZNK7RThread11LogonCancelER14TRequestStatus",
        "_ZNK7RThread15RequestCompleteERP14TRequestStatusi", "_ZNK7RThread16RendezvousCancelER14TRequestStatus", "_ZNK7RThread5LogonER14TRequestStatus",
        "_ZNK7RThread6ResumeEv", "_ZNK7RThread7SuspendEv", "_ZNK7RThread13RequestSignalEv",
        "_ZNK8RProcess10RendezvousER14TRequestStatus", "_ZNK8RProcess11LogonCancelER14TRequestStatus",
        "_ZNK8RProcess16RendezvousCancelER14TRequestStatus", "_ZNK8RProcess5LogonER14TRequestStatus")},
}
