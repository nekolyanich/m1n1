# SPDX-License-Identifier: GPL-2.0-or-later
#
# AP writes into a display pipe's register ranges, ASYNC so the guest does not
# stall: to compare what macOS's kernel writes into the display hardware before
# hotplug against Linux. The ranges are the ADT dispextN node's reg entries plus
# the dcpextN node's from index 2 on; windows trace_dcp.py traces itself
# (dcpextN reg 1) are cut out, two tracers cannot share a page.
#   TRACE_DISP=1 (pipe/DCP number, default 1), TRACE_DISP_READS=1 to log reads too.

import os

from m1n1.hv import TraceMode
from m1n1.utils import irange

n = os.environ.get("TRACE_DISP", "1")
reads = os.environ.get("TRACE_DISP_READS") == "1"

def regs(name):
    path = "/arm-io/" + name
    if name not in hv.adt["/arm-io"] or not hasattr(hv.adt[path], "reg"):
        print(f"trace_dispmmio: no regs in {path}")
        return []
    node = hv.adt[path]
    r = [node.get_reg(i) for i in range(len(node.reg))]
    print(f"trace_dispmmio: {name} regs=" + ", ".join(f"[{i}] {a:#x}+{s:#x}" for i, (a, s) in enumerate(r)))
    return r

dcp = regs(f"dcpext{n}")
pipe = regs(f"dispext{n}")
skip = dcp[:2]  # ASC mailbox and the window trace_dcp traces
want = pipe + dcp[2:]

def cut(start, end):
    pieces = [(start, end)]
    for s, size in skip:
        e = s + size
        pieces = [p for a, b in pieces for p in ((a, min(b, s)), (max(a, e), b)) if p[0] < p[1]]
    return pieces

for addr, size in want:
    for a, b in cut(addr, addr + size):
        hv.trace_range(irange(a, b - a), TraceMode.ASYNC, read=reads, write=True,
                       name=f"disp{n}")
        print(f"trace_dispmmio: tracing {a:#x}+{b - a:#x} writes{' and reads' if reads else ''}")
