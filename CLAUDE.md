# SLogic16U3

Open LiteX gateware + C/Python host stack for the Sipeed SLogic16U3 USB3 logic analyzer (Gowin GW5AT-15,
USB3 in the FPGA transceivers with USB3-PIPE).

- Plan and status: `doc/PLAN.md`. Hardware knowledge: `doc/HARDWARE.md`.
- Gateware: `slogic_16u3.py` (SoC/target, platform from LiteX-Boards `sipeed_slogic16u3`), `gateware/*.py`
  (one core per file). LiteX style: SPDX headers, 100-col `# Section ---` banners, aligned assignments,
  `# # #` separator, full sentence comments. Cores: control/status as plain Signals before `# # #`, CSRs
  created in `add_csr()`; the CSR map is fixed (`slogic_16u3.py` csr_map) and must stay stable (host).
- Host: `software/user` (libliteusb, libslogic, tools, C tests: `make -C software/user check`),
  `software/sigrok` (libsigrok driver), `software/scopehal` (ngscopeclient bridge/driver), `software/slogic.py`.
- Tests: `source scripts/env.sh && python3 -m pytest test` (Migen sim, USB3 host model).
- Toolchain: Gowin IDE (gw_sh), dependencies on branches until merged: `source scripts/env.sh`.
- Never commit flash dumps (Sipeed DFU loader/stock firmware): keep them outside the repository. Never
  write the flash at or above 0x800000 (DFU loader, recovery).
- Commits: `Area: summary`, bullet body, `Hardware: ... OK` line when validated on the board.
