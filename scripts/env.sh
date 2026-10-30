# Source me (kept for the scripts/docs): the dependencies (LiteX, LiteX-Boards, LiteICLink, USB3-PIPE) are
# used from their upstream master branches (installed: pip install -e / litex_setup.py). SLOGIC_DEPS: optional
# extra Python paths (ex: development branches/worktrees) prepended to PYTHONPATH.
if [ -n "$SLOGIC_DEPS" ]; then
    export PYTHONPATH=$SLOGIC_DEPS${PYTHONPATH:+:$PYTHONPATH}
fi
