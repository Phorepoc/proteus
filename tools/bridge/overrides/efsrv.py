# Copyright The Phorepoc Authors
# SPDX-License-Identifier: Apache-2.0
"""Hand-written declarations for efsrv exports the 2010 headers do not declare (see euser.py)."""
W, P, V, C = {"c": "w"}, {"c": "p"}, {"c": "v"}, {"c": "c"}
THIS = {"c": "p", "t": "this"}
MANUAL = {
    "_ZN3RFs14SetDefaultPathERK7TDesC16": {"kind": "method", "cls": "RFs", "ret": W, "params": [THIS, P]},
    "_ZNK3RFs11DefaultPathER6TDes16": {"kind": "method", "cls": "RFs", "ret": W, "params": [THIS, P], "const": True},
    "_ZN9_RRawDisk4ReadEiR5TDes8": {"kind": "method", "cls": "_RRawDisk", "ret": W, "params": [THIS, W, P]},
    "_ZN9_RRawDisk5WriteEiR6TDesC8": {"kind": "method", "cls": "_RRawDisk", "ret": W, "params": [THIS, W, P]},
    "_ZTV13CFsNotifyBody": {"kind": "vtable", "cls": "CFsNotifyBody"},
    "_ZTI13CFsNotifyBody": {"kind": "typeinfo", "cls": "CFsNotifyBody"},
    "_ZTV19CFsNotificationList": {"kind": "vtable", "cls": "CFsNotificationList"},
    "_ZTI19CFsNotificationList": {"kind": "typeinfo", "cls": "CFsNotificationList"},
}
OVERRIDES: dict[str, str] = {}
