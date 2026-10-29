# Source me: dependencies not yet merged upstream (worktrees/branches), see doc/PLAN.md.
#   litex-boards : sipeed_slogic16u3 (platform/target completed).
#   liteiclink   : serdes/gtr12-gw5at (GTR12, GW5AT-15 GTR12_QUADB/GTR12_UPARA).
#   usb3_pipe    : gw5/gw5at-15 (GW5AT-15 Q0 USB3 SerDes configuration).
LITEX_DIR=${LITEX_DIR:-$HOME/dev/litex}
SLOGIC_DEPS=${SLOGIC_DEPS:-$LITEX_DIR/litex-boards/.claude/worktrees/slogic16u3:$LITEX_DIR/liteiclink/.claude/worktrees/gtr12-gw5at:$LITEX_DIR/usb3_pipe/.claude/worktrees/gw5at-15}
export PYTHONPATH=$SLOGIC_DEPS${PYTHONPATH:+:$PYTHONPATH}
