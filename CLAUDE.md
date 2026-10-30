# SLogic16U3

Open LiteX gateware + C/Python host stack for the Sipeed SLogic16U3 USB3 logic analyzer (Gowin GW5AT-15,
USB3 in the FPGA transceivers with USB3-PIPE).

- Plan and status: `doc/PLAN.md`. Hardware knowledge: `doc/HARDWARE.md`.
- Gateware: `slogic_16u3.py` (SoC/target, platform from LiteX-Boards `sipeed_slogic16u3`), `gateware/*.py`
  (one core per file). LiteX style: SPDX headers, 100-col `# Section ---` banners, aligned assignments,
  `# # #` separator, full sentence comments. Cores: control/status as plain Signals before `# # #`, CSRs
  created in `add_csr()`; the CSR map is fixed (`slogic_16u3.py` csr_map) and must stay stable (host).
- Host: `software/user` (libslogic: USB3 + simulated device transports, tools, C tests: `make -C software/user
  check`), `software/sigrok` (libsigrok driver, `build.sh`), `software/scopehal` (ngscopeclient bridge/driver).
  The simulated device (`sim`, libslogic/slogic_sim.c) must follow the gateware behavior (registers, packing).
- Tests: `python3 -m pytest test` (Migen sim, USB3 host model, sigrok/bridge end-to-end
  on the simulated device), `make -C software/user check`.
- Docs: `doc/GATEWARE.md`, `doc/SOFTWARE.md`, `doc/UPDATE.md`; promo video/stills: `doc/illustration`.
- Confidential: do not mention this project in upstream (LiteX-Boards/LiteICLink/USB3-PIPE) commits/PRs.
- Toolchain: Gowin IDE (gw_sh); dependencies from upstream master (LiteX, LiteX-Boards, LiteICLink, USB3-PIPE),
  `source scripts/env.sh` only adds optional development paths (SLOGIC_DEPS).
- Never commit flash dumps (Sipeed DFU loader/stock firmware): keep them outside the repository. Never
  write the flash at or above 0x800000 (DFU loader, recovery).
- Commits: `Area: summary`, bullet body, `Hardware: ... OK` line when validated on the board.
