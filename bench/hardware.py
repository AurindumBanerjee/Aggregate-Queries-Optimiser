"""Record the machine the benchmark ran on (bench/results/hardware.json)."""
import json
import platform
import subprocess
import sys
from pathlib import Path


def ps(cmd: str) -> str:
    try:
        return subprocess.run(["powershell", "-NoProfile", "-Command", cmd], capture_output=True, text=True, timeout=30).stdout.strip()
    except Exception:
        return ""


def main() -> None:
    info = {"os": platform.platform(), "python": platform.python_version(), "machine": platform.machine()}
    if sys.platform == "win32":
        info["cpu"] = ps("(Get-CimInstance Win32_Processor).Name")
        info["cores"] = ps("(Get-CimInstance Win32_Processor).NumberOfCores")
        info["logical_processors"] = ps("(Get-CimInstance Win32_Processor).NumberOfLogicalProcessors")
        info["ram_gib"] = ps("[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1GB,1)")
        info["disk"] = ps("(Get-PhysicalDisk | Select-Object -First 1).Model")
    else:
        info["cpu"] = platform.processor()
        try:
            info["ram_gib"] = round(int(Path("/proc/meminfo").read_text().split()[1]) / 1048576, 1)
            info["cpu"] = next(l.split(":")[1].strip() for l in Path("/proc/cpuinfo").read_text().splitlines() if l.startswith("model name"))
        except Exception:
            pass
    out = Path(__file__).parent / "results" / "hardware.json"
    out.parent.mkdir(exist_ok=True)
    out.write_text(json.dumps(info, indent=2))
    print(json.dumps(info, indent=2))


if __name__ == "__main__":
    main()
