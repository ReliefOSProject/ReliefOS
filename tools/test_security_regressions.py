#!/usr/bin/env python3
"""Audit current IPC peer boundaries and execute capability/PAM/OpenRC regressions.

Source lint and host behavior tests do not establish target-kernel compatibility.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

PEERCRED_PATHS = (
    "userland/apps/device-agent/main.c",
    "userland/apps/sessiond/main.c",
)


def read(path: str) -> str:
    return (ROOT / path).read_text(encoding="utf-8")



def missing_peercred() -> list[str]:
    missing = []
    for path in PEERCRED_PATHS:
        text = read(path)
        if "SO_PEERCRED" not in text or "reliefos_ipc_peer_credentials" not in text:
            missing.append(path)
    return missing


def kernel_report():
    import json
    result = subprocess.run([sys.executable, "kernel/reliefnt/tools/test_security_regressions.py", "--json"],
                            cwd=ROOT, capture_output=True, text=True, timeout=90)
    try:
        report = json.loads(result.stdout)
        fields = ("kernel_private_ioctl_violations", "credential_regression_failures")
        if all(isinstance(report.get(name), list) for name in fields):
            if result.returncode == 0 or any(report[name] for name in fields):
                return report
    except (json.JSONDecodeError, AttributeError):
        pass
    return {"kernel_private_ioctl_violations": [],
            "credential_regression_failures": ["kernel suite failed: " + (result.stdout + result.stderr)[-4000:]]}


def authorization_regressions() -> list[str]:
    """Execute the current PAM boundary and narrow OpenRC authorization adapter.

    authd was removed by the earlier PAM migration; historical handler-name
    checks cannot test the production sudo/PAM implementation.
    """
    failures = []
    for script in ("tools/test_pam_login_case.py", "tools/test_openrc_authorization.py"):
        result = subprocess.run([sys.executable, script], cwd=ROOT,
                                capture_output=True, text=True, timeout=90)
        if result.returncode:
            failures.append(script + ": " + (result.stdout + result.stderr)[-4000:])
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--strict", action="store_true")
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()

    kernel = kernel_report()
    violations = kernel["kernel_private_ioctl_violations"]
    peercred = missing_peercred()
    gates = kernel["credential_regression_failures"]
    from_peer = authorization_regressions()
    failures = violations + peercred + gates + from_peer

    if args.json:
        import json
        print(json.dumps({
            "tool": "test_security_regressions",
            "strict": args.strict,
            "kernel_private_ioctl_violations": violations,
            "missing_so_peercred": peercred,
            "credential_regression_failures": gates,
            "authorization_regression_failures": from_peer,
        }, indent=2))
    else:
        print("ReliefOS Unix-IPC 安全回归源码检测")
        print(f"私有 ioctl 残留: {len(violations)}")
        for item in violations:
            print(f"  FAIL {item}")
        print(f"SO_PEERCRED 缺失: {len(peercred)}")
        for item in peercred:
            print(f"  FAIL {item}")
        print(f"凭据/capability 行为回归失败: {len(gates)}")
        for item in gates:
            print(f"  FAIL {item}")
        print(f"PAM/OpenRC 授权行为回归失败: {len(from_peer)}")
        for item in from_peer:
            print(f"  FAIL {item}")
        print("source lint and focused credential check " + ("passed" if not failures else "failed"))

    return 1 if args.strict and failures else 0


if __name__ == "__main__":
    sys.exit(main())
