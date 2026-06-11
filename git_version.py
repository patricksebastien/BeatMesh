import subprocess
Import("env")

try:
    rev = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"]
    ).decode().strip()
except Exception:
    rev = "unknown"

env.Append(CPPDEFINES=[("GIT_VERSION", env.StringifyMacro(rev))])
