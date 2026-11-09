"""Compile dual-target shaders (Common/OrgResources.hlsli) for every target they must build for.

Each shader is built three ways:
- FXC, *_5_0: the D3D11 path, with register bindings.
- DXC, *_6_6 SPIR-V with CS_ORG_BINDLESS and BasicRHI's Vulkan ABI (rhi_shader_abi.h): the ORG path today.
- DXC, *_6_6 DXIL with CS_ORG_BINDLESS: the D3D12 path (Phase 7), checked now so the convention stays ready.

It also reads the ORG push block's size from the DXIL reflection and fails above 128 bytes.
Exit code 0 when every build passes.
"""

import argparse
import os
from pathlib import Path
import re
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SHADERS = [
    "tests/Shaders/OrgResourcesSampleCS.hlsl",
    "features/IBL/Shaders/IBL/DiffuseIBLCS.hlsl",
]
MAX_PUSH_BYTES = 128


def find_dxc(explicit):
    if explicit:
        return Path(explicit)
    sdk = os.environ.get("VULKAN_SDK")
    candidates = [Path(sdk) / "Bin/dxc.exe"] if sdk else []
    candidates += sorted(Path("C:/VulkanSDK").glob("*/Bin/dxc.exe"), reverse=True)
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise RuntimeError("dxc.exe not found: pass --dxc or set VULKAN_SDK")


def find_fxc(explicit):
    if explicit:
        return Path(explicit)
    for base in [os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")]:
        if not base:
            continue
        found = sorted(Path(base, "Windows Kits/10/bin").glob("*/x64/fxc.exe"), reverse=True)
        if found:
            return found[0]
    raise RuntimeError("fxc.exe not found: pass --fxc or install the Windows SDK")


def spirv_flags():
    abi = (ROOT / "extern/BasicRHI/rhi_shader_abi.h").read_text()

    def constant(name):
        match = re.search(rf"\b{name}\s*=\s*(\d+)\s*;", abi)
        if not match:
            raise RuntimeError(f"Missing shader ABI constant: {name}")
        return match.group(1)

    flags = ["-spirv", "-fvk-use-dx-layout", "-fvk-use-dx-position-w", "-fspv-target-env=vulkan1.3",
             "-D", "BASICRHI_SHADER_API_VULKAN=1"]
    for option, name in [("resource", "RESOURCE"), ("sampler", "SAMPLER"), ("counter", "COUNTER")]:
        flags += [f"-fvk-bind-{option}-heap", constant(f"VULKAN_{name}_DESCRIPTOR_HEAP_BINDING"),
                  constant("VULKAN_DESCRIPTOR_HEAP_SET")]
    return flags


def stage_of(path):
    stem = path.stem.lower()
    for suffix, stage in [("cs", "cs"), ("ps", "ps"), ("vs", "vs")]:
        if stem.endswith(suffix):
            return stage
    return "cs"


def include_dirs(path):
    dirs = [ROOT / "package/Shaders"]
    for parent in path.parents:
        if parent.name == "Shaders" and parent not in dirs:
            dirs.append(parent)
            break
    return [arg for d in dirs for arg in ("-I", str(d))]


def run(name, command):
    result = subprocess.run(command, capture_output=True, text=True)
    status = "ok" if result.returncode == 0 else "FAILED"
    print(f"  {name}: {status}")
    if result.returncode != 0:
        print(result.stdout + result.stderr)
    return result


def push_bytes(dxc, dxil):
    dump = subprocess.run([str(dxc), "-dumpbin", str(dxil)], capture_output=True, text=True).stdout
    match = re.search(r"\}\s*OrgBindings;\s*;\s*Offset:\s*\d+\s+Size:\s*(\d+)", dump)
    return int(match.group(1)) if match else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("shaders", nargs="*", help="Shader paths, repo-relative or absolute (default: the sample and DiffuseIBLCS)")
    parser.add_argument("--dxc", help="dxc.exe (default: the Vulkan SDK's)")
    parser.add_argument("--fxc", help="fxc.exe (default: the newest Windows SDK's)")
    parser.add_argument("--spirv-val", help="spirv-val.exe (default: beside dxc.exe, when present)")
    parser.add_argument("--entry", default="main")
    parser.add_argument("--output-dir", type=Path, default=ROOT / "build/dual-target-shaders")
    args = parser.parse_args()

    dxc = find_dxc(args.dxc)
    fxc = find_fxc(args.fxc)
    spirv_val = Path(args.spirv_val) if args.spirv_val else dxc.with_name("spirv-val.exe")
    args.output_dir.mkdir(parents=True, exist_ok=True)

    failed = 0
    for shader in args.shaders or DEFAULT_SHADERS:
        path = Path(shader)
        path = path if path.is_absolute() else ROOT / path
        stage = stage_of(path)
        includes = include_dirs(path)
        out = args.output_dir / path.stem
        print(path.relative_to(ROOT) if path.is_relative_to(ROOT) else path)

        common = ["-E", args.entry, *includes, str(path)]
        fxc_includes = ["/I" if arg == "-I" else arg for arg in includes]
        results = [
            run("FXC " + stage + "_5_0", [str(fxc), "/nologo", "/T", stage + "_5_0", "/E", args.entry, *fxc_includes, "/Fo", f"{out}.cso", str(path)]),
            run("DXC SPIR-V " + stage + "_6_6", [str(dxc), "-HV", "2021", "-T", stage + "_6_6", *spirv_flags(), "-D", "CS_ORG_BINDLESS=1", "-Fo", f"{out}.spv", *common]),
            run("DXC DXIL " + stage + "_6_6", [str(dxc), "-HV", "2021", "-T", stage + "_6_6", "-D", "CS_ORG_BINDLESS=1", "-Fo", f"{out}.dxil", *common]),
        ]
        ok = all(r.returncode == 0 for r in results)
        if ok and spirv_val.is_file():
            ok = run("spirv-val", [str(spirv_val), "--target-env", "vulkan1.3", f"{out}.spv"]).returncode == 0
        if ok:
            size = push_bytes(dxc, f"{out}.dxil")
            if not size:
                print("  push block: none (the shader does not use ORG_PUSH_BLOCK)")
            elif size > MAX_PUSH_BYTES:
                print(f"  push block: {size} bytes, over the {MAX_PUSH_BYTES}-byte limit")
            else:
                print(f"  push block: {size} bytes")
            ok = 0 < size <= MAX_PUSH_BYTES
        failed += 0 if ok else 1

    print("all targets built" if not failed else f"{failed} shader(s) failed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
