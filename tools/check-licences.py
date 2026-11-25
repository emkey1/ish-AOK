#!/usr/bin/env python3
"""app/Licences.json lists what the tree builds into the app.

The Licences screen shows app/Licences.json, which tools/gen-licences.py writes
from the submodules, the vendored texts in tools/licences/ and `cargo tree`.
A new dependency, a bumped crate or a new bundled binary leaves it stale, and a
stale list ships without the notice the new code asks for. This runs the
generator's --check. It runs cargo offline, so it belongs after a build has
fetched the crates (tools/check-all.py --build-only).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.exit(subprocess.call([sys.executable, os.path.join(HERE, 'gen-licences.py'), '--check']))
