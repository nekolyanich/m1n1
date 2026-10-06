# SPDX-License-Identifier: GPL-2.0-or-later
#
# AP writes into a DCP's display register ranges (the dcpextN node's reg entries
# from index 2 on; trace_dcp.py owns index 1 and the ASC mailbox), ASYNC so the
# guest does not stall. To compare what macOS's kernel writes into the display
# hardware before hotplug against Linux. TRACE_DISP=dcpext1 (default), and
# TRACE_DISP_READS=1 to log reads too.

import os

from m1n1.hv import TraceMode
from m1n1.utils import irange

name = os.environ.get("TRACE_DISP", "dcpext1")
reads = os.environ.get("TRACE_DISP_READS") == "1"
path = "/arm-io/" + name

node = hv.adt[path]
regs = [node.get_reg(i) for i in range(len(node.reg))]
print(f"trace_dispmmio: {name} regs=" + ", ".join(f"[{i}] {a:#x}+{s:#x}" for i, (a, s) in enumerate(regs)))
for i, (addr, size) in enumerate(regs):
    if i < 2:
        continue
    hv.trace_range(irange(addr, size), TraceMode.ASYNC, read=reads, write=True,
                   name=f"{name}[{i}]")
    print(f"trace_dispmmio: tracing {name}[{i}] {addr:#x}+{size:#x} writes{' and reads' if reads else ''}")
