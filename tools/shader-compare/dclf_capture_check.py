"""Checks a DCLF pass capture dump (CS_DCLF_CAPTURE, dclf:capture) against docs/development/plans/dclf-replay.md.

Usage: python tools/shader-compare/dclf_capture_check.py <capture dir>
Prints counts and every problem found. Exit 0 when the dump is complete.
"""

import json
import os
import sys


def number(value):
    """A 64-bit field: a number or a hex/decimal string."""
    if isinstance(value, str):
        return int(value, 0)
    return int(value)


def main():
    if len(sys.argv) != 2:
        print(__doc__)
        return 2
    root = sys.argv[1]
    with open(os.path.join(root, "manifest.json"), encoding="utf-8") as f:
        manifest = json.load(f)
    problems = []

    def problem(text):
        problems.append(text)

    def file_size(path):
        full = os.path.join(root, path)
        if not os.path.isfile(full):
            problem(f"missing file {path}")
            return None
        return os.path.getsize(full)

    if manifest.get("version") != 1:
        problem(f"version {manifest.get('version')} is not 1")
    for key in ("resources", "views", "samplers", "pointers", "rootSignature", "commandSignature", "indexBuffer", "viewport", "targets", "passes"):
        if key not in manifest:
            problem(f"no {key}")

    resources = {r["id"]: r for r in manifest.get("resources", [])}
    if len(resources) != len(manifest.get("resources", [])):
        problem("resource ids repeat")
    total = 0
    spans = []
    for r in resources.values():
        if r["kind"] == "buffer":
            spans.append((number(r["gpuVA"]), r["bytes"], r["id"]))
            size = file_size(r["file"]) if r.get("file") else None
            if size is not None and size != r["bytes"]:
                problem(f"buffer {r['id']} file holds {size} bytes, not {r['bytes']}")
        elif r["kind"] == "texture":
            for key in ("dimension", "format", "width", "height", "depthOrArraySize", "mips", "flags"):
                if key not in r:
                    problem(f"texture {r['id']} has no {key}")
            if not r.get("file"):
                problem(f"texture {r['id']} has no file")
            else:
                file_size(r["file"])
        else:
            problem(f"resource {r['id']} has kind {r['kind']}")
        if r.get("file") and os.path.isfile(os.path.join(root, r["file"])):
            total += os.path.getsize(os.path.join(root, r["file"]))

    def resolves(address):
        return any(base <= address < base + size for base, size, _ in spans)

    slots = set()
    for view in manifest.get("views", []):
        if view["slot"] in slots:
            problem(f"slot {view['slot']} listed twice")
        slots.add(view["slot"])
        if not 0 <= view["slot"] < manifest["resourceHeapSlots"]:
            problem(f"slot {view['slot']} is outside the heap")
        if view["resource"] is not None and view["resource"] not in resources:
            problem(f"slot {view['slot']} names resource {view['resource']}, which is not listed")
        if view["type"] not in ("srv", "uav", "cbv"):
            problem(f"slot {view['slot']} has type {view['type']}")
    for sampler in manifest.get("samplers", []):
        if not 0 <= sampler["slot"] < manifest["samplerHeapSlots"]:
            problem(f"sampler slot {sampler['slot']} is outside the heap")

    index = manifest.get("indexBuffer")
    if index and index["resource"] not in resources:
        problem("the index buffer's resource is not listed")

    pointers = 0
    buckets = 0
    draws = 0
    indices = set()
    pass_names = []
    for p in manifest.get("passes", []):
        pass_names.append(p["name"])
        if len(p["frameWords"]) != manifest["rootSignature"]["frameWords"]:
            problem(f"{p['name']}: {len(p['frameWords'])} frame words")
        for w in p["framePointerWords"]:
            address = p["frameWords"][w] | (p["frameWords"][w + 1] << 32)
            if not resolves(address):
                problem(f"{p['name']}: frame word {w} holds 0x{address:X}, in no listed buffer")
        if p.get("depthBefore"):
            file_size(p["depthBefore"])
        for f in p.get("colourBefore") or []:
            if f:
                file_size(f)
        for upload in p.get("uploads", []):
            r = resources.get(upload["resource"])
            size = file_size(upload["file"])
            if r is None:
                problem(f"{p['name']}: upload names resource {upload['resource']}, not listed")
                continue
            if size is not None and upload["offset"] + size > r["bytes"]:
                problem(f"{p['name']}: upload {upload['file']} runs past its resource")
            if size is not None:
                with open(os.path.join(root, upload["file"]), "rb") as f:
                    data = f.read()
                for offset in upload["pointers"]:
                    address = int.from_bytes(data[offset:offset + 8], "little")
                    if not resolves(address):
                        problem(f"{p['name']}: {upload['file']}+{offset} holds 0x{address:X}, in no listed buffer")
            pointers += len(upload["pointers"])
        for b in p["buckets"]:
            buckets += 1
            draws += b["count"]
            if b["index"] in indices:
                problem(f"bucket index {b['index']} repeats")
            indices.add(b["index"])
            if len(b["drawWords"]) != manifest["rootSignature"]["drawWords"]:
                problem(f"bucket {b['index']}: {len(b['drawWords'])} draw words")
            for key in ("argsResource", "countResource"):
                if b[key] not in resources:
                    problem(f"bucket {b['index']}: {key} {b[key]} is not listed")
            if b["count"] > b["capacity"]:
                problem(f"bucket {b['index']}: count {b['count']} over capacity {b['capacity']}")
            pipeline = b.get("pipeline")
            if not pipeline:
                problem(f"bucket {b['index']}: no pipeline")
                continue
            file_size(pipeline["vs"])
            if pipeline.get("ps"):
                file_size(pipeline["ps"])
            if not pipeline.get("vsStage"):
                problem(f"bucket {b['index']}: no vertex stage line")
            if pipeline.get("ps") and not pipeline.get("psStage"):
                problem(f"bucket {b['index']}: no pixel stage line")
            if "state" not in pipeline:
                problem(f"bucket {b['index']}: no state")
            # Draw word 0 names the sequences' view ref slot.
            if b["drawWords"][0] not in slots:
                problem(f"bucket {b['index']}: draw word 0 slot {b['drawWords'][0]} has no view")

    for wanted in ("Z-prepass", "main opaque"):
        if wanted not in pass_names:
            problem(f"no {wanted} pass")

    print(f"passes {pass_names}, buckets {buckets} ({draws} draws), resources {len(resources)}, views {len(manifest.get('views', []))}, "
          f"samplers {len(manifest.get('samplers', []))}, pointers {pointers}, resource files {total / 1048576:.1f} MiB")
    for text in problems[:60]:
        print("PROBLEM:", text)
    if len(problems) > 60:
        print(f"... {len(problems) - 60} more")
    return 0 if not problems else 1


if __name__ == "__main__":
    sys.exit(main())
