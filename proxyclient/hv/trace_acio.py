# SPDX-License-Identifier: GPL-2.0-or-later
#
# ACIO (USB4/Thunderbolt host router co-processor) mailbox: every message the AP
# exchanges with the ACIO firmware, raw, plus its syslog. Used to see what macOS
# tells the firmware around DP tunnel setup (atcN-dpinM MODE is already 1 before
# macOS writes it). TRACE_ACIO selects the instance (default 1 = left-near port).

import os

from m1n1.trace.asc import ASCTracer
from m1n1.trace.dart import DARTTracer

ASCTracer = ASCTracer._reloadcls()
DARTTracer = DARTTracer._reloadcls()

acio_n = int(os.environ.get("TRACE_ACIO", "1"))
# the ASC (co-processor + mailbox) is acio-cpuN; acioN itself is the NHI/router
# window, and trapping it SYNC drags macOS into its watchdog
acio_path = f"/arm-io/acio-cpu{acio_n}"
dart_path = f"/arm-io/dart-acio{acio_n}"

acio_dart = None
acio_sid = 0
# dart-acioN lives inside the ACIO window, which is only mapped while the
# co-processor runs; at HV start it is off and reading the DART faults. Opt in
# with TRACE_ACIO_DART=1 when attaching later.
if dart_path in hv.adt and os.environ.get("TRACE_ACIO_DART"):
    acio_dart_tracer = DARTTracer(hv, dart_path)
    acio_dart_tracer.start()
    acio_dart = acio_dart_tracer.dart
    try:
        acio_sid = hv.adt[dart_path][0].reg
    except Exception as e:
        print(f"trace_acio: no SID under {dart_path}: {e}")
else:
    print(f"trace_acio: {dart_path} not traced (TRACE_ACIO_DART=1), syslog buffers will not decode")

if os.environ.get("TRACE_ACIO_ASYNC"):
    # Raw ASYNC trace of the ASC mailbox page only: no SYNC stall per message
    # (the SYNC ASCTracer wedged the HV once macOS 15 started parking CPUs).
    # Messages: W.8 +0x800/+0x808 AP->ACIO, R.8 +0x830/+0x838 ACIO->AP.
    from m1n1.hv import TraceMode
    from m1n1.utils import irange
    node = hv.adt[acio_path]
    for i in range(len(node.reg)):
        base, size = node.get_reg(i)
        mbox = base + 0x8000 if size > 0x8000 else base
        hv.trace_range(irange(mbox, 0x4000), TraceMode.ASYNC, name=f"acio{acio_n}-mbox")
        print(f"trace_acio: ASYNC mailbox {mbox:#x}+0x4000 ({acio_path} reg {i})")
        break
else:
    # verbose=2 logs every SEND/RECV, so endpoints without a decoder still show up
    acio_tracer = ASCTracer(hv, acio_path, verbose=2)
    acio_tracer.start(acio_dart, stream=acio_sid)
    print(f"trace_acio: tracing {acio_path} (dart {dart_path}, sid {acio_sid})")
