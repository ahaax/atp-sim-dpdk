"""Build/test without third-party Python packages; works with GCC on Linux/Windows."""
from pathlib import Path
import os
import subprocess
from datetime import datetime, timezone

root = Path(__file__).resolve().parents[1]
build = root.parent / "work" / "modification-002" / "verification" / datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
build.mkdir(parents=True, exist_ok=True)
suffix = ".exe" if os.name == "nt" else ""
flags = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow",
         "-Wstrict-prototypes", "-pedantic"]
logs = []


def run(args):
    result = subprocess.run(args, text=True, encoding="utf-8", errors="replace", capture_output=True)
    logs.append("COMMAND: " + " ".join(map(str, args)) + "\n" + result.stdout + result.stderr)
    (build / "verification.log").write_text("\n".join(logs), encoding="utf-8")
    if result.returncode:
        raise RuntimeError(logs[-1])
    if result.stdout:
        print(result.stdout.strip())


run(["gcc", "--version"])
for name, optimize in [("debug", ["-O0", "-g"]), ("release", ["-O2", "-DNDEBUG"])]:
    exe = build / ("test_" + name + suffix)
    run(["gcc", *flags, *optimize, "-DATP_NO_MAIN", str(root / "atp_sim.c"), str(root / "atp_wire.c"),
         str(root / "tests/test_atp.c"), "-lm", "-o", str(exe)])
    run([str(exe)])
demo = build / ("atp_demo" + suffix)
run(["gcc", *flags, "-O2", str(root / "atp_sim.c"), str(root / "atp_wire.c"), "-lm", "-o", str(demo)])
run([str(demo)])
for source in ["atp_sim.c", "atp_wire.c"]:
    run(["gcc", *flags, "-O0", "-fanalyzer", "-DATP_NO_MAIN", "-c",
         str(root / source), "-o", str(build / (source + ".o"))])
print("Verification log:", build / "verification.log")
