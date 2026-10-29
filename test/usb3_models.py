#
# This file is part of SLogic16U3.
#
# Copyright (c) 2026 Florent Kermarrec <florent@enjoy-digital.fr>
# SPDX-License-Identifier: BSD-2-Clause

"""USB3-PIPE test models (usb3_pipe repository test/model package): host model (PyHost) and PHY
stand-in adapters, registered as test.model (the usb3_pipe test package shares our test package name)."""

import os
import sys
import importlib.util

import usb3_pipe

_model_dir = os.path.join(os.path.dirname(os.path.dirname(usb3_pipe.__file__)), "test", "model")

def _load(name, filename):
    spec   = importlib.util.spec_from_file_location(name, os.path.join(_model_dir, filename),
        submodule_search_locations=[_model_dir] if filename == "__init__.py" else None)
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module

if "test.model" not in sys.modules:
    _load("test.model", "__init__.py") if os.path.exists(os.path.join(_model_dir, "__init__.py")) else None
    host   = _load("test.model.host",   "host.py")
    device = _load("test.model.device", "device.py")
else:
    host   = sys.modules["test.model.host"]
    device = sys.modules["test.model.device"]
