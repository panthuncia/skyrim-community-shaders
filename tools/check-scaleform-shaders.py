"""Check CS's transcriptions of Scaleform's shaders (package/Shaders/Scaleform/) against the game's.

SkyrimSE.exe (GFx 3.3's GRendererD3D1x) carries 55 SM 4.0 bytecode blobs and the HLSL source they were compiled from. This
script reads both from the unpacked SE executable in a Ghidra server (read_memory), then for every variant:

1. compiles the embedded source with FXC and compares its instructions with the shipped blob's (the pairing);
2. compiles CS's transcription with the variant's defines and compares its instructions with the embedded source's.

A transcription passes when its instructions (declarations included) are those of the embedded source. A pairing is "same"
when the blob's instructions match too, "registers" when only register allocation (and so the swizzles) differs (the game's compiler, 9.29.952,
is older than the SDK's), and "differs" otherwise. Exit code 0 when every transcription passes.

    python tools/check-scaleform-shaders.py [--ghidra http://127.0.0.1:8089] [--fxc path]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SHADERS = ROOT / "package/Shaders"
PROGRAM = "/skyrim/se/SkyrimSE.exe.unpacked.exe"
SOURCE_RANGE = (0x1417D44B0, 0x1417DC000)  # the embedded HLSL strings

# Each variant: (transcription, stage, defines, embedded source address, shipped blob address and size or None).
FILL_VS = [(1, 0x1417D8F00, 0x1417C4A70, 0x330), (2, 0x1417D9040, 0x1417C3450, 0x348), (3, 0x1417D91D0, 0x1417C4DA0, 0x394),
           (4, 0x1417D9380, 0x1417C5140, 0x3F0), (5, 0x1417D9590, 0x1417C5530, 0x454)]
FILL_VS_3D = [(1, 0x1417D9800, 0x1417C3AB0, 0x35C), (2, 0x1417D9920, 0x1417C30D0, 0x374), (3, 0x1417D9A80, 0x1417C3E10, 0x3C0),
              (4, 0x1417D9C00, 0x1417C41D0, 0x41C), (5, 0x1417D9DE0, 0x1417C45F0, 0x480)]
FILL_PS = [(1, 0x1417DA030, 0x1417D2490, 0x208), (2, 0x1417DA0A0, 0x1417C74C0, 0x2FC), (3, 0x1417DA1A0, 0x1417C77C0, 0x354),
           (4, 0x1417DA2C0, 0x1417D26A0, 0x354), (5, 0x1417DA420, 0x1417D2A00, 0x318), (6, 0x1417DA560, 0x1417D2D20, 0x370),
           (7, 0x1417DA6B0, 0x1417D3090, 0x574), (8, 0x1417DA9B0, 0x1417C1670, 0x5CC), (9, 0x1417DACE0, 0x1417D3610, 0x5EC),
           (10, 0x1417DB040, 0x1417D3C00, 0x644), (11, 0x1417DB3D0, 0x1417C5D90, 0x2C0), (12, 0x1417DB600, 0x1417C6A30, 0x264),
           (13, 0x1417DB790, 0x1417C6CA0, 0x3BC), (14, 0x1417DBAC0, 0x1417C5990, 0x3FC), (15, 0x1417DB4E0, 0x1417C6050, 0x304),
           (16, 0x1417DB6B0, 0x1417C6360, 0x2C4), (17, 0x1417DB920, 0x1417C6630, 0x400), (18, 0x1417DBC70, 0x1417C7060, 0x454)]
# FILTER n: the n-th filter source; the PS-B slot whose blob it is (one of them where several share it), or None.
FILTER_PS = [(1, 0x1417D44B0, 0x1417C8EF0, 0x6CC), (2, 0x1417D47F0, 0x1417C95C0, 0x7F8), (3, 0x1417D4C00, 0x1417CA5C0, 0x6AC),
             (4, 0x1417D4F40, 0x1417CAC70, 0x724), (5, 0x1417D52A0, 0x1417CB3A0, 0x850), (6, 0x1417D56E0, 0x1417CC440, 0x704),
             (7, 0x1417D5A40, 0x1417CCB50, 0x6C0), (8, 0x1417D5D70, 0x1417CD210, 0x768), (9, 0x1417D6110, 0x1417CD980, 0x784),
             (10, 0x1417D64D0, 0x1417CE110, 0x6DC), (11, 0x1417D6820, 0x1417CE7F0, 0x718), (12, 0x1417D6B70, 0x1417CEF10, 0x7C0),
             (13, 0x1417D6F30, 0x1417CF6D0, 0x7DC), (14, 0x1417D7310, 0x1417CFEB0, 0x734), (15, 0x1417D7680, 0x1417D05F0, 0x588),
             (16, 0x1417D78E0, None, None), (17, 0x1417D7BA0, 0x1417D1110, 0x5E0), (18, 0x1417D7E30, None, None),
             (19, 0x1417D8200, 0x1417C7B20, 0x464), (20, 0x1417D83D0, 0x1417C7F90, 0x4BC), (21, 0x1417D85C0, 0x1417C8450, 0x520),
             (22, 0x1417D87C0, 0x1417C8970, 0x578), (23, 0x1417D89F0, 0x1417D1CD0, 0x3AC), (24, 0x1417D8B20, 0x1417D2080, 0x404)]
FILTER_VS = (0x1417D8110, 0x1417C37A0, 0x310)


def variants():
    for kind, source, blob, size in FILL_VS:
        yield f"FillVS {kind}", "Scaleform/FillVS.hlsl", "vs", [f"VS_KIND={kind}"], source, blob, size
    for kind, source, blob, size in FILL_VS_3D:
        yield f"FillVS {kind} 3D", "Scaleform/FillVS.hlsl", "vs", [f"VS_KIND={kind}", "VS_3D"], source, blob, size
    for kind, source, blob, size in FILL_PS:
        yield f"FillPS {kind}", "Scaleform/FillPS.hlsl", "ps", [f"PS_KIND={kind}"], source, blob, size
    yield "FilterVS", "Scaleform/FilterVS.hlsl", "vs", [], *FILTER_VS
    for kind, source, blob, size in FILTER_PS:
        yield f"FilterPS {kind}", "Scaleform/FilterPS.hlsl", "ps", [f"FILTER={kind}"], source, blob, size


def find_fxc(explicit):
    if explicit:
        return Path(explicit)
    for base in [os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")]:
        if base:
            found = sorted(Path(base, "Windows Kits/10/bin").glob("*/x64/fxc.exe"), reverse=True)
            if found:
                return found[0]
    raise RuntimeError("fxc.exe not found: pass --fxc or install the Windows SDK")


def read_memory(ghidra, address, length):
    data = bytearray()
    while length > 0:
        chunk = min(length, 0x4000)
        url = f"{ghidra}/read_memory?program={PROGRAM}&address={address:x}&length={chunk}"
        data += bytes.fromhex(json.load(urllib.request.urlopen(url, timeout=60))["hex"])
        address += chunk
        length -= chunk
    return bytes(data)


def instructions(fxc, path):
    out = subprocess.run([str(fxc), "/nologo", "/dumpbin", str(path)], capture_output=True, text=True).stdout
    return [line.strip() for line in out.splitlines() if line.strip() and not line.strip().startswith("//")]


def compile_shader(fxc, path, stage, defines, out):
    command = [str(fxc), "/nologo", "/T", stage + "_4_0", "/E", "main", "/I", str(SHADERS), "/Fo", str(out)]
    for define in defines:
        command += ["/D", define]
    result = subprocess.run(command + [str(path)], capture_output=True, text=True)
    if result.returncode:
        print(result.stdout + result.stderr)
        return None
    return instructions(fxc, out)


def registers_only(a, b):
    def strip(lines):
        # Register numbers, the swizzles and masks that follow from them, and plain register-to-register moves.
        lines = [re.sub(r"\.[xyzw]+\b", "", re.sub(r"\b[rv]\d+", "R", line)) for line in lines if not line.startswith("dcl_temps")]
        return [line for line in lines if line != "mov R, R"]

    return strip(a) == strip(b)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--ghidra", default="http://127.0.0.1:8089")
    parser.add_argument("--fxc")
    args = parser.parse_args()
    fxc = find_fxc(args.fxc)
    sources = read_memory(args.ghidra, SOURCE_RANGE[0], SOURCE_RANGE[1] - SOURCE_RANGE[0])
    failed = 0
    with tempfile.TemporaryDirectory() as work:
        work = Path(work)
        for index, (name, file, stage, defines, source, blob, size) in enumerate(variants()):
            start = source - SOURCE_RANGE[0]
            text = sources[start:sources.index(b"\0", start)].decode("latin-1")
            (work / f"{index}.hlsl").write_text(text)
            embedded = compile_shader(fxc, work / f"{index}.hlsl", stage, [], work / f"{index}.e.cso")
            mine = compile_shader(fxc, SHADERS / file, stage, defines, work / f"{index}.m.cso")
            pairing = "no blob"
            if blob:
                (work / f"{index}.dxbc").write_bytes(read_memory(args.ghidra, blob, size))
                shipped = instructions(fxc, work / f"{index}.dxbc")
                pairing = "same" if shipped == embedded else "registers" if registers_only(shipped, embedded) else "differs"
            ok = embedded is not None and mine == embedded
            failed += 0 if ok else 1
            print(f"{name:16} transcription {'ok' if ok else 'DIFFERS'}, blob {pairing}")
    print("all transcriptions match" if not failed else f"{failed} transcription(s) differ")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
