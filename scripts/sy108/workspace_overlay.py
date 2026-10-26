# SPDX-License-Identifier: GPL-2.0-only
#
# Appended to tools/lib/workspace.py to replace the docker wrapper with
# direct local execution. Later definitions win, so the original file is
# left untouched.
#
# Requires OUT / DOWNLOADS / OPENWRT / subprocess from workspace.py.
# The three container-internal paths (/project, /output, /downloads) must be
# symlinked on the host before bootstrap runs.


def container(arguments, offline=False, root=False):
    OUT.mkdir(parents=True, exist_ok=True)
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    (OUT / "ccache").mkdir(exist_ok=True)
    return [str(a) for a in arguments]


def run(arguments, **kwargs):
    # docker run supplied --workdir; local execution needs it explicitly
    kwargs.setdefault("cwd", OPENWRT)
    return subprocess.run(container(arguments), check=True, **kwargs)
