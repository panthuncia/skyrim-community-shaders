#!/usr/bin/env python3
"""Writes src/RenderGraph/Engine/Tripwire/TripwireInterfaces.inl from the Windows SDK's d3d11.h and dxgi.h.

For each interface the tripwire handles implement, the output lists its own methods in vtable order as
X(Ret, Name, (params), (forward args), (failure statements)) entries, plus the chain of IIDs the handle answers.
TripwireHandle.h expands them into the handle classes. The compiler checks the result: every entry is an
`override`, and a handle class that misses a slot stays abstract and cannot be created.

The SAL annotations decide two things:
- Failure: _Out_ / _Outptr_ / _COM_Outptr_ / _Inout_ pointees are zeroed, _Out_writes_*(N) arrays are zeroed
  for N elements, _Out_writes_bytes_*(N) buffers for N bytes when N is a parameter.
- Forwarding: interface pointers (_In_, or none) and _In_reads_*(N) interface arrays are unwrapped, so a
  tripwire handle passed to a wrapped D3D11 object reaches it as the object it wraps.

Usage: python tools/generate-tripwire-interfaces.py [--sdk <Include/10.0.x.y dir>]
"""

import argparse
import pathlib
import re
import sys

# Interfaces with a handle, and the ones they derive from. Order here is the order in the output.
D3D11 = [
    "ID3D11DeviceChild",
    "ID3D11DepthStencilState",
    "ID3D11BlendState",
    "ID3D11RasterizerState",
    "ID3D11Resource",
    "ID3D11Buffer",
    "ID3D11Texture1D",
    "ID3D11Texture2D",
    "ID3D11Texture3D",
    "ID3D11View",
    "ID3D11ShaderResourceView",
    "ID3D11RenderTargetView",
    "ID3D11DepthStencilView",
    "ID3D11UnorderedAccessView",
    "ID3D11VertexShader",
    "ID3D11PixelShader",
    "ID3D11ComputeShader",
    "ID3D11InputLayout",
    "ID3D11SamplerState",
    "ID3D11Asynchronous",
    "ID3D11Query",
    "ID3D11DeviceContext",
    "ID3D11Device",
]
DXGI = ["IDXGIObject", "IDXGIDeviceSubObject", "IDXGISwapChain"]
# The interfaces a handle class is made for (the rest are only bases).
HANDLES = [
    "ID3D11Texture1D",
    "ID3D11Texture2D",
    "ID3D11Texture3D",
    "ID3D11Buffer",
    "ID3D11ShaderResourceView",
    "ID3D11RenderTargetView",
    "ID3D11DepthStencilView",
    "ID3D11UnorderedAccessView",
    "ID3D11VertexShader",
    "ID3D11PixelShader",
    "ID3D11ComputeShader",
    "ID3D11InputLayout",
    "ID3D11BlendState",
    "ID3D11DepthStencilState",
    "ID3D11RasterizerState",
    "ID3D11SamplerState",
    "ID3D11Query",
    "ID3D11DeviceContext",
    "ID3D11Device",
    "IDXGISwapChain",
]

INTERFACE_RE = re.compile(r'MIDL_INTERFACE\("[^"]+"\)\s*(\w+)\s*:\s*public\s+(\w+)\s*\{\s*public:(.*?)\n    \};', re.S)
METHOD_RE = re.compile(r"virtual\s+(.+?)\s+STDMETHODCALLTYPE\s+(\w+)\(\s*(.*?)\)\s*=\s*0;", re.S)
COMMENT_RE = re.compile(r"/\*.*?\*/", re.S)


def split_top(text):
    parts, depth, cur = [], 0, ""
    for ch in text:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            parts.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        parts.append(cur)
    return [p.strip() for p in parts]


def take_sal(text):
    """Leading SAL annotations (identifiers like _Out_writes_opt_ with an optional balanced argument list)."""
    sal = []
    while True:
        m = re.match(r"(_\w+_)\s*", text)
        if not m:
            return sal, text
        name, rest = m.group(1), text[m.end():]
        arg = None
        if rest.startswith("("):
            depth = 0
            for i, ch in enumerate(rest):
                depth += ch == "("
                depth -= ch == ")"
                if depth == 0:
                    arg = rest[1:i].strip()
                    rest = rest[i + 1:].lstrip()
                    break
        sal.append((name, arg))
        text = rest


def is_interface(type_name):
    return type_name.startswith("ID3D11") or type_name.startswith("IDXGI") or type_name == "IUnknown"


class Param:
    def __init__(self, raw):
        raw = " ".join(COMMENT_RE.sub(" ", raw).split())
        self.sal, rest = take_sal(raw)
        m = re.match(r"(.*?)\b(\w+)\s*(\[\s*(\w+)\s*\])?$", rest)
        if not m:
            raise ValueError(f"cannot parse parameter: {raw}")
        self.type = " ".join(m.group(1).replace("*", " * ").split()).replace("* *", "**").replace(" *", "*")
        self.name = m.group(2)
        self.array = m.group(4)

    def decl(self):
        return f"{self.type} {self.name}" + (f"[{self.array}]" if self.array else "")

    def sal_names(self):
        return [s[0] for s in self.sal]

    def forward(self):
        m = re.match(r"^(\w+)\*$", self.type)
        if m and is_interface(m.group(1)) and not any(s.startswith(("_Out", "_COM_Out", "_Inout")) for s in self.sal_names()):
            return f"TW_UNWRAP({self.name})"
        m = re.match(r"^(\w+)\*\s*const\*$", self.type)
        if m and is_interface(m.group(1)):
            for name, arg in self.sal:
                if name.startswith("_In_reads"):
                    return f"TW_UNWRAP_ARRAY({self.name}, {arg})"
            raise ValueError(f"interface array without a count: {self.decl()}")
        return self.name

    def fail(self):
        is_void_ptr = self.type in ("void*", "const void*")
        for name, arg in self.sal:
            if name.startswith("_Out_writes_bytes") or name.startswith("_Out_writes_to_bytes"):
                count = arg.split(",")[0].strip() if arg else None
                if is_void_ptr and count and re.fullmatch(r"\w+", count):
                    return f"TW_ZERO_BYTES({self.name}, {count})"
                return None
            if name.startswith("_Out_writes"):
                count = arg.split(",")[0].strip()
                if count.startswith("*"):
                    return f"TW_ZERO_N_AT({self.name}, {count[1:].strip()})"
                return f"TW_ZERO_N({self.name}, {count})"
            if name.startswith(("_Out", "_COM_Out", "_Inout")):
                if is_void_ptr or self.type.startswith("const "):
                    return None
                if self.array:
                    return f"TW_ZERO_N({self.name}, {self.array})"
                if "*" in self.type:
                    return f"TW_ZERO({self.name})"
        return None


def parse(path, wanted):
    text = path.read_text(encoding="utf-8", errors="replace")
    found = {}
    for m in INTERFACE_RE.finditer(text):
        name, base, body = m.group(1), m.group(2), m.group(3)
        if name not in wanted or name in found:
            continue
        methods = []
        for mm in METHOD_RE.finditer(body):
            ret, method, params = " ".join(mm.group(1).split()), mm.group(2), mm.group(3)
            params = COMMENT_RE.sub(" ", params).strip()
            plist = [] if params in ("", "void") else [Param(p) for p in split_top(params)]
            methods.append((ret, method, plist))
        found[name] = (base, methods)
    missing = [w for w in wanted if w not in found]
    if missing:
        raise SystemExit(f"{path}: interfaces not found: {missing}")
    return found


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", default=None, help="Windows Kits Include/<version> directory (default: the newest)")
    parser.add_argument("--out", default="src/RenderGraph/Engine/Tripwire/TripwireInterfaces.inl")
    args = parser.parse_args()
    if args.sdk:
        sdk = pathlib.Path(args.sdk)
    else:
        root = pathlib.Path("C:/Program Files (x86)/Windows Kits/10/Include")
        sdk = sorted(p for p in root.iterdir() if p.name.startswith("10."))[-1]
    interfaces = parse(sdk / "um" / "d3d11.h", D3D11)
    interfaces.update(parse(sdk / "shared" / "dxgi.h", DXGI))

    out = [
        "// Generated by tools/generate-tripwire-interfaces.py from the Windows SDK " + sdk.name + ". Do not edit by hand.",
        "// Each interface's own methods in vtable order: X(Ret, Name, (params), (forward args), (failure statements)).",
        "// TripwireHandle.h expands these into the tripwire handle classes.",
        "",
        "// clang-format off",
        "#pragma once",
        "",
    ]
    for name in D3D11 + DXGI:
        base, methods = interfaces[name]
        out.append(f"// {name} : {base}, {len(methods)} own slots")
        out.append(f"#define TW_OWN_{name}(X) \\")
        for ret, method, plist in methods:
            decl = ", ".join(p.decl() for p in plist)
            fwd = ", ".join(p.forward() for p in plist)
            # Arrays first: their count can be an in/out parameter that is zeroed after them.
            fails = [f for f in (p.fail() for p in plist) if f]
            fails = " ".join(sorted(fails, key=lambda f: not f.startswith("TW_ZERO_N")))
            out.append(f"\tX({ret}, {method}, ({decl}), ({fwd}), ({fails})) \\")
        out.append("")
    for name in HANDLES:
        chain = [name]
        while chain[-1] != "IUnknown":
            chain.append(interfaces[chain[-1]][0])
        chain.reverse()
        own = [c for c in chain if c != "IUnknown"]
        slots = 3 + sum(len(interfaces[c][1]) for c in own)
        out.append(f"// {name}: {slots} vtable slots ({' > '.join(chain)})")
        out.append(f"#define TW_SLOTS_{name}(X) " + " ".join(f"TW_OWN_{c}(X)" for c in own))
        out.append(f"#define TW_IIDS_{name} " + ", ".join(f"__uuidof({c})" for c in chain))
        out.append(f"#define TW_SLOT_COUNT_{name} {slots}")
        out.append("")
    out.append("// clang-format on")
    pathlib.Path(args.out).write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {args.out} from {sdk}")


if __name__ == "__main__":
    sys.exit(main())
