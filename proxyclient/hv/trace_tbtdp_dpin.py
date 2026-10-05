# SPDX-License-Identifier: GPL-2.0-or-later
#
# DP-over-USB4 stream source only: each port's DP crossbar and DP IN glue plus
# the display crossbar, without a PhyTracer on the ATC PHYs (trapping those
# breaks the USB4 sideband). Pair with trace_dcp.py (TRACE_DCP=dcpextN) to see
# which dptx target DCP connects to. Standalone: does not need trace_atc.py.

from m1n1.hv import TraceMode

PORTS = [1, 2]

def _props(node):
    return {k: v for k, v in node._properties.items() if k not in ("name", "reg")}

nodes = [f"atc{n}-{s}" for n in PORTS for s in ("dpxbar", "dpin0", "dpin1")]
nodes += [n.name for n in hv.adt["/arm-io"] if n.name.startswith("display-crossbar")]

for name in nodes:
    if name not in hv.adt["/arm-io"]:
        print(f"trace_tbtdp_dpin: no /arm-io/{name}")
        continue
    node = hv.adt["/arm-io/" + name]
    print(f"trace_tbtdp_dpin: {name} {_props(node)}")
    if not hasattr(node, "reg"):
        continue
    regs = [node.get_reg(i) for i in range(len(node.reg))]
    print(f"trace_tbtdp_dpin: {name} regs=" +
          ", ".join(f"{a:#x}+{s:#x}" for a, s in regs))
    hv.trace_device("/arm-io/" + name, TraceMode.ASYNC)

# the dcpext instances and their dptx wiring, for mapping dispextN -> dcpextN
for node in hv.adt["/arm-io"]:
    if node.name.startswith("dcpext") or node.name.startswith("dispext"):
        keep = {k: v for k, v in _props(node).items()
                if any(w in k for w in ("dp", "phy", "port", "atc", "xbar", "index",
                                        "transport", "die", "AAPL,phandle"))}
        print(f"trace_tbtdp_dpin: {node.name} {keep}")
