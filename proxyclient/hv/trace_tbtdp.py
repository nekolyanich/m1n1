# SPDX-License-Identifier: GPL-2.0-or-later
#
# DP-over-Thunderbolt path on the guest-owned Type-C ports: the ATC PHYs, each
# port's DP crossbar / DP PHY / DP IN adapters, and the display crossbar that
# feeds them. Run after trace_atc.py, which defines PhyTracer and covers atc-phy1.

from m1n1.hv import TraceMode

PORTS = [1, 2]

for n in PORTS:
    if globals().get("_tbtdp_phys_started"):
        break
    if n != 1 and f"atc-phy{n}" in hv.adt["/arm-io"]:
        t = PhyTracer(hv, f"/arm-io/atc-phy{n}", verbose=2)
        t.start()
        print(f"trace_tbtdp: PhyTracer on atc-phy{n}")

_tbtdp_phys_started = True

nodes = [f"atc{n}-{s}" for n in PORTS for s in ("dpxbar", "dpphy", "dpin0", "dpin1")]
nodes += [n.name for n in hv.adt["/arm-io"] if n.name.startswith("display-crossbar")]

for name in nodes:
    if name not in hv.adt["/arm-io"]:
        print(f"trace_tbtdp: no /arm-io/{name}")
        continue
    node = hv.adt["/arm-io/" + name]
    if not hasattr(node, "reg"):
        props = {k: v for k, v in node._properties.items() if k not in ("name",)}
        print(f"trace_tbtdp: {name} (no reg) {props}")
        continue
    regs = [node.get_reg(i) for i in range(len(node.reg))]
    print(f"trace_tbtdp: {name} compat={getattr(node, 'compatible', '?')} regs=" +
          ", ".join(f"{a:#x}+{s:#x}" for a, s in regs))
    hv.trace_device("/arm-io/" + name, TraceMode.ASYNC)
