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
# trace_dcp.py owns the ASC mailbox, dcpext reg 1 and both DARTs
skip = dcp[:2] + regs(f"dart-dispext{n}") + regs(f"dart-dcpext{n}")

# the dispext ranges nest (disp-0 spans the others): merge them first
merged = []
for a, size in sorted(pipe + dcp[2:]):
    b = a + size
    if merged and a <= merged[-1][1]:
        merged[-1][1] = max(merged[-1][1], b)
    else:
        merged.append([a, b])

def cut(start, end):
    pieces = [(start, end)]
    for s, size in skip:
        e = s + size
        pieces = [p for a, b in pieces for p in ((a, min(b, s)), (max(a, e), b)) if p[0] < p[1]]
    return pieces

for start, end in merged:
    for a, b in cut(start, end):
        hv.trace_range(irange(a, b - a), TraceMode.ASYNC, read=reads, write=True,
                       name=f"disp{n}")
        print(f"trace_dispmmio: tracing {a:#x}+{b - a:#x} writes{' and reads' if reads else ''}")
