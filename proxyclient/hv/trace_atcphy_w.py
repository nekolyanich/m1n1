# SPDX-License-Identifier: GPL-2.0-or-later
#
# ASYNC writes into an ATC PHY (default atc-phy1): to see how macOS starts the pixel
# clock for a DP tunnel (AUSPLL, DPTX PCLK1 in TX_DP_CTRL0 +0x7000) when DCP sets the
# link rate. Writes only, so the guest never stalls; the ADT reg 0 (USB2 PHY) and
# the display crossbar / dpin windows (trace_tbtdp_dpin.py) are left out.
#   TRACE_ATCPHY=1 (PHY number), TRACE_ATCPHY_READS=1 to log reads too.

import os

from m1n1.hv import TraceMode
from m1n1.utils import irange

n = os.environ.get("TRACE_ATCPHY", "1")
reads = os.environ.get("TRACE_ATCPHY_READS") == "1"
node = hv.adt[f"/arm-io/atc-phy{n}"]
regs = [node.get_reg(i) for i in range(len(node.reg))]
print(f"trace_atcphy_w: atc-phy{n} regs=" + ", ".join(f"[{i}] {a:#x}+{s:#x}" for i, (a, s) in enumerate(regs)))

skip = []
for name in (f"atc{n}-dpxbar", f"atc{n}-dpin0", f"atc{n}-dpin1"):
    if name in hv.adt["/arm-io"]:
        skip.append(hv.adt["/arm-io/" + name].get_reg(0))

def cut(start, end):
    pieces = [(start, end)]
    for s, size in skip:
        e = s + size
        pieces = [p for a, b in pieces for p in ((a, min(b, s)), (max(a, e), b)) if p[0] < p[1]]
    return pieces

for i, (addr, size) in enumerate(regs):
    if i == 0:
        continue
    for a, b in cut(addr, addr + size):
        hv.trace_range(irange(a, b - a), TraceMode.ASYNC, read=reads, write=True,
                       name=f"atcphy{n}-{i}")
        print(f"trace_atcphy_w: tracing [{i}] {a:#x}+{b - a:#x}")
