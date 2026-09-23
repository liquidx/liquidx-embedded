"""Inject FW_VERSION (git short SHA, "-dirty" if the tree has changes) into the build."""
import subprocess

Import("env")  # noqa: F821 -- provided by PlatformIO


def git(*args):
    try:
        return subprocess.check_output(["git", *args], text=True, cwd=env["PROJECT_DIR"]).strip()  # noqa: F821
    except Exception:
        return ""


sha = git("rev-parse", "--short", "HEAD") or "unknown"
if git("status", "--porcelain", "--", "."):
    sha += "-dirty"
env.Append(CPPDEFINES=[("FW_VERSION", '\\"0.1.0+%s\\"' % sha)])  # noqa: F821
