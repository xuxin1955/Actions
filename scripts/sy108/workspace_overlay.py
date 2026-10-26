# SPDX-License-Identifier: GPL-2.0-only


def container(arguments, offline=False, root=False):
    OUT.mkdir(parents=True, exist_ok=True)
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    (OUT / "ccache").mkdir(exist_ok=True)
    return [str(a) for a in arguments]


def run(arguments, **kwargs):
    kwargs.setdefault("cwd", OPENWRT)
    return subprocess.run(container(arguments), check=True, **kwargs)
