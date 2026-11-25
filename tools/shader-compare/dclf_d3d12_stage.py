"""dclf_d3d12_stage.py: one DCLF stage built offline into DXIL with the game's D3D12 chain, for pascal-fetch-bench replay --override.

  python tools/shader-compare/dclf_d3d12_stage.py --stage <line.json | stages.jsonl> [--line N] [--options <file>] -o <out.dxil>
         [--shaders <Data/Shaders dir>] [--define NAME[=v] ...] [--as-d3d12] [--debug-info] [--keep] [--print]
  python tools/shader-compare/dclf_d3d12_stage.py --manifest <dump dir> --bucket <i> --stage-kind vs|ps [--pass <name>]
         [--define NAME[=v] ...] [-o <out.dxil>]

--stage takes a CS_DCLF_STAGE_DUMP "stage" line: a JSON string, a file holding one, or a stages.jsonl with --line N (1-based line
number). The "options" line (material textures, shadow mask slot, samplers, lazy rows) comes from --options, else from the first
options line of the --stage file, else from the manifest's "stageOptions", else from the game's defaults.
--manifest reads the stage from a replay dump (pascal-fetch-bench replay): the bucket's pipeline vsStage or psStage. The default
output is <dump>/override/b<i>.<vs|ps>.dxil, ready for `replay <dump> --override <i>:<vs|ps>=<that file>`.
--as-d3d12 turns a Vulkan stage line into the D3D12 one: drops the -fvk arguments and adds DCLF_D3D12_FRAME (and
DCLF_D3D12_SHADOW_FRAME for Utility) to pulled stages, as ShaderPrograms.cpp MakeStageRequest does on D3D12.

The chain, as the game runs it on D3D12:
  1. pulled stages: dclf-shadertool rewrite (PulledSource);
  2. stages with DCLF_D3D12_FRAME: prepend #include "Common/DCLFFrameRecord.hlsli", then dxc -P with -E main -T <target> -HV 2018
     and the defines and -I (DxilStages::PrepareFrameSource), then dclf-shadertool d3d12 (FrameSource::Rewrite);
  3. dxc -E main -T <target> -HV 2018 [-Zi -Qembed_debug] -D... -I <Shaders> (ORGModuleServices ShaderCompiler BuildArguments).
DXC is the Vulkan SDK's dxc.exe beside the game's dxcompiler.dll (1.9, which signs DXIL itself, so no dxil.dll is needed).
"""
import argparse
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
SDK = os.environ.get('VULKAN_SDK', r'C:\VulkanSDK\1.4.357.0')
SHADERTOOL = os.path.join(ROOT, 'build', 'shader-tools', 'dclf-shadertool.exe')
DEFAULT_SHADERS = r'C:\Modding\MO2\mods\CS AB pascal\Shaders'
# The game's options line (CS_DCLF_STAGE_DUMP), used when no options line is given.
DEFAULT_OPTIONS = {'materialTextures': [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 15, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91],
                   'shadowMaskSlot': 14, 'samplerSlots': 16, 'lazyRows': True}
FRAME_DEFINE = 'DCLF_D3D12_FRAME'
SHADOW_FRAME_DEFINE = 'DCLF_D3D12_SHADOW_FRAME'
PRINT = False


def run(cmd, cwd=None):
    if PRINT:
        print(subprocess.list2cmdline(cmd), f'  (in {cwd})' if cwd else '')
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace')
    if r.returncode:
        sys.exit(f'failed ({r.returncode}): {subprocess.list2cmdline(cmd)}\n{r.stdout[-4000:]}\n{r.stderr[-4000:]}')
    return r.stdout


def dxc_exe(shaders, work):
    """dxc.exe beside the game's dxcompiler.dll, in <work>/dxc."""
    bindir = os.path.join(work, 'dxc')
    os.makedirs(bindir, exist_ok=True)
    game_dll = os.path.join(os.path.dirname(os.path.normpath(shaders)), 'SKSE', 'Plugins', 'CommunityShaders', 'bin', 'dxcompiler.dll')
    dll = game_dll if os.path.exists(game_dll) else os.path.join(SDK, 'Bin', 'dxcompiler.dll')
    for src in [os.path.join(SDK, 'Bin', 'dxc.exe'), dll]:
        dst = os.path.join(bindir, os.path.basename(src))
        if not os.path.exists(dst) or os.path.getmtime(dst) != os.path.getmtime(src) or os.path.getsize(dst) != os.path.getsize(src):
            shutil.copy2(src, dst)
    return os.path.join(bindir, 'dxc.exe')


def read_lines(arg):
    """The JSON objects of a JSON string or a .json/.jsonl file."""
    text = arg
    if not arg.lstrip().startswith('{'):
        with open(arg, encoding='utf-8') as f:
            text = f.read()
    return [json.loads(line) for line in text.splitlines() if line.strip()]


def as_d3d12(stage):
    stage = dict(stage)
    stage['arguments'] = []
    names = {n for n, _ in stage['dclf']}
    dclf = list(stage['dclf'])
    if stage['pulled'] and FRAME_DEFINE not in names:
        dclf.append([FRAME_DEFINE, '1'])
        if stage['source'].endswith('Utility.hlsl'):
            dclf.append([SHADOW_FRAME_DEFINE, '1'])
    stage['dclf'] = dclf
    return stage


def define_args(defines):
    return [f'-D{n}={v}' if v != '' else f'-D{n}' for n, v in defines]


def parse_define(text):
    n, _, v = text.partition('=')
    return [n, v]


def build(stage, options, shaders, out, extra_defines, work, debug_info=False, bound_blocks=True):
    shaders = os.path.abspath(shaders)
    os.makedirs(work, exist_ok=True)
    dxc = dxc_exe(shaders, work)
    rel = stage['source']
    if rel.startswith('Data/Shaders/'):
        rel = rel[len('Data/Shaders/'):]
    original = os.path.join(shaders, *rel.split('/'))
    name = os.path.splitext(os.path.basename(rel))[0]
    pixel = stage['pixel']
    target = stage['target']
    defines = [list(d) for d in stage['cs']] + [list(d) for d in stage['dclf']] + extra_defines
    names = {n for n, _ in defines}
    # The source's folder first, so its relative includes resolve as they would beside the original.
    includes = ['-I', os.path.dirname(original)]
    if os.path.normcase(os.path.dirname(original)) != os.path.normcase(shaders):
        includes += ['-I', shaders]
    kind = ('pulled-' if stage['pulled'] else '') + ('depth-' if stage['depth'] else '') + ('ps' if pixel else 'vs')
    tag = f"{name}-{kind}-{stage['descriptor']}"
    source = original
    if stage['pulled']:
        pulled = os.path.join(work, tag + '.1-pulled.hlsl')
        cmd = [SHADERTOOL, 'rewrite', original, pulled, '--shaders', shaders,
               '--material-textures', ','.join(map(str, options['materialTextures'])),
               '--shadow-mask', str(options['shadowMaskSlot']), '--samplers', str(options['samplerSlots'])]
        if options.get('lazyRows', True):
            cmd.append('--lazy')
        run(cmd)
        source = pulled
    if FRAME_DEFINE in names:
        with open(source, encoding='utf-8', errors='replace', newline='') as f:
            text = f.read()
        prefixed = os.path.join(work, tag + '.2-prefixed.hlsl')
        with open(prefixed, 'w', encoding='utf-8', newline='') as f:
            f.write('#include "Common/DCLFFrameRecord.hlsli"\n' + text)
        pre = os.path.join(work, tag + '.3-pre.hlsl')
        run([dxc, '-P', '-E', 'main', '-T', target, '-HV', '2018'] + define_args(defines) + includes + ['-Fi', pre, prefixed])
        frame = os.path.join(work, tag + '.4-frame.hlsl')
        cmd = [SHADERTOOL, 'd3d12', pre, frame, '--pixel' if pixel else '--vertex']
        if SHADOW_FRAME_DEFINE in names:
            cmd.append('--shadow')
        elif bound_blocks:
            # The game's default: pass-wide blocks as register(b<ordinal>, space9) (DrawPipelines.h BoundFrameBlocks).
            cmd.append('--bound-blocks')
        run(cmd)
        source = frame
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    cmd = [dxc, '-E', 'main', '-T', target, '-HV', '2018']
    if debug_info:
        cmd += ['-Zi', '-Qembed_debug']
    cmd += define_args(defines) + includes + stage.get('arguments', []) + ['-Fo', out, source]
    run(cmd)
    with open(out, 'rb') as f:
        head = f.read(20)
    if head[:4] != b'DXBC' or not any(head[4:20]):
        sys.exit(f'{out}: DXC wrote unsigned or non-DXIL output')
    return out


def main():
    global PRINT
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--stage', help='stage line: JSON, a file with one line, or stages.jsonl with --line')
    ap.add_argument('--line', type=int, help='1-based line of the --stage file')
    ap.add_argument('--options', help='options line: JSON or a file holding it')
    ap.add_argument('--manifest', help='replay dump directory (manifest.json)')
    ap.add_argument('--bucket', type=int, help='bucket index in the manifest')
    ap.add_argument('--pass', dest='pass_name', help='pass name in the manifest (default: the first pass holding the bucket)')
    ap.add_argument('--stage-kind', choices=['vs', 'ps'], help='which stage of the bucket pipeline')
    ap.add_argument('--shaders', default=DEFAULT_SHADERS, help='the Data/Shaders tree')
    ap.add_argument('--define', action='append', default=[], help='extra define NAME[=value], after the stage defines')
    ap.add_argument('--as-d3d12', action='store_true', help='convert a Vulkan stage line to the D3D12 one')
    ap.add_argument('--vulkan', action='store_true', help='build a Vulkan stage line as it is, to SPIR-V (for spv-lower and spv-pipeline-stats)')
    ap.add_argument('--debug-info', action='store_true', help='-Zi -Qembed_debug, as the game with shader debug on')
    ap.add_argument('--bound-blocks', action='store_true', help='pass-wide blocks bound (the game default) even for a manifest without boundFrameBlocks')
    ap.add_argument('--heap-blocks', action='store_true',
                    help='pass-wide blocks by heap index, as the game read them before the bound tables (default: bound, or the manifest boundFrameBlocks)')
    ap.add_argument('-o', '--out', help='output .dxil')
    ap.add_argument('--work', help='folder for intermediate files (default: <out>.work)')
    ap.add_argument('--keep', action='store_true', help='keep the intermediate files')
    ap.add_argument('--print', action='store_true', help='print each command')
    a = ap.parse_args()
    PRINT = a.print

    options = None
    bound = True
    if a.manifest:
        if a.bucket is None or not a.stage_kind:
            ap.error('--manifest needs --bucket and --stage-kind')
        with open(os.path.join(a.manifest, 'manifest.json'), encoding='utf-8') as f:
            manifest = json.load(f)
        stage = None
        for p in manifest['passes']:
            if a.pass_name and p['name'] != a.pass_name:
                continue
            for b in p['buckets']:
                if b['index'] == a.bucket:
                    stage = b['pipeline'].get(a.stage_kind + 'Stage')
                    break
            if stage is not None:
                break
        if not stage:
            sys.exit(f'bucket {a.bucket} has no {a.stage_kind}Stage in the manifest')
        options = manifest.get('stageOptions')
        # Older captures predate bound blocks and were made with heap-indexed ones.
        bound = manifest.get('boundFrameBlocks', False)
        out = a.out or os.path.join(a.manifest, 'override', f'b{a.bucket}.{a.stage_kind}.dxil')
    elif a.stage:
        lines = read_lines(a.stage)
        if a.line:
            stage = lines[a.line - 1]
        else:
            stages = [e for e in lines if e.get('kind') == 'stage']
            if len(stages) != 1:
                ap.error(f'--stage holds {len(stages)} stage lines; pick one with --line')
            stage = stages[0]
        if stage.get('kind') != 'stage':
            sys.exit('the chosen line is not a stage line')
        options = next((e for e in lines if e.get('kind') == 'options'), None)
        if not a.out:
            ap.error('--stage needs -o')
        out = a.out
    else:
        ap.error('give --stage or --manifest')
    if a.options:
        options = next(e for e in read_lines(a.options) if e.get('kind', 'options') == 'options')
    if options is None:
        print('no options line given: using the game defaults', file=sys.stderr)
        options = DEFAULT_OPTIONS
    if a.as_d3d12:
        stage = as_d3d12(stage)
    if not a.vulkan and any(arg.startswith('-fvk') or arg == '-spirv' for arg in stage.get('arguments', [])):
        sys.exit('the stage line has Vulkan arguments; pass --as-d3d12 to build its D3D12 version')

    work = a.work or (os.path.abspath(out) + '.work')
    build(stage, options, a.shaders, out, [parse_define(d) for d in a.define], work, a.debug_info, (bound or a.bound_blocks) and not a.heap_blocks)
    if not a.keep and not a.work:
        shutil.rmtree(work, ignore_errors=True)
    print(out)


if __name__ == '__main__':
    main()
