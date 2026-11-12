"""The command line: python tools/cs-test/cs_test.py <command> ..."""

import argparse
import sys

from . import commands, journal as journal_mod, procs
from .paths import GAME_EXE, MO2_EXE, default_paths
from .schema import ConfigError


def _common(parser):
    parser.add_argument("--set", dest="sets", action="append", default=[], metavar="KEY=VALUE",
                        help="a config value that wins over the files (repeatable): dclf.shadows=false, run.timeout_s=120")
    parser.add_argument("--label", help="a label for the run folder and summary")
    parser.add_argument("--stage", help="the staged build to run (same as --set build.stage=NAME)")
    parser.add_argument("--dry-run", action="store_true", help="resolve and print the config and its difference from base; touch nothing")
    parser.add_argument("--wait", type=int, default=0, metavar="S", help="wait up to S seconds for another run's lock (default: refuse)")


def build_parser():
    parser = argparse.ArgumentParser(prog="cs-test", description="Automated Community Shaders game runs through MO2.")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("stage", help="stage a worktree's build (or an AIO folder) as MO2 mod 'CS AB <name>'")
    p.add_argument("source", help="a worktree (build/<preset>, package, features) or an AIO folder")
    p.add_argument("--name", required=True)
    p.add_argument("--preset", default="Dev-Fast", help="the build folder under build/ (default Dev-Fast)")
    p.add_argument("--base-aio", help="an AIO folder for the files a DLL build lacks, the first time a name is staged")
    p.add_argument("--pad", type=int, default=0, help="empty files that make usvfs grow its table before the game starts")
    p.add_argument("--pdb", action="store_true", help="AIO mode: copy PDBs too")

    p = sub.add_parser("run", help="one game run of a config ('a+b' composes)")
    p.add_argument("config")
    _common(p)

    p = sub.add_parser("sweep", help="a base run, then one run per varied key")
    p.add_argument("config")
    p.add_argument("--base", action="append", default=[], help="assignments for every run, comma-separated: \"dclf.*=false\"")
    p.add_argument("--vary", action="append", required=True, help="keys or globs, comma-separated: dclf.shadows,dclf.grass or features.*")
    p.add_argument("--mode", choices=["on-one", "off-one"], default="on-one", help="on-one: each run turns one key on; off-one: one key off")
    _common(p)

    p = sub.add_parser("compare", help="two runs: outcome, config, milestones, error kinds, shots, memory")
    p.add_argument("a")
    p.add_argument("b")
    p.add_argument("--no-shots", action="store_true")
    p.add_argument("--no-memory", action="store_true")

    sub.add_parser("status", help="the lock, the journal, the live variant, leftovers")

    p = sub.add_parser("restore", help="undo leftovers, then put back the CS mod from before the first swap")
    p.add_argument("--journal-only", action="store_true", help="undo leftovers only; keep the live variant")

    p = sub.add_parser("check-schema", help="compare schema.toml with the game's switch dump")
    p.add_argument("dump", nargs="?", help="the dump (default: <SKSE>\\CommunityShaders-Switches.json)")
    return parser


def main(argv=None):
    args = build_parser().parse_args(argv)
    paths = default_paths()
    try:
        if args.command == "stage":
            from .stage import stage

            lock = journal_mod.Lock(paths.lock, f"stage {args.name}")
            lock.acquire(0)
            try:
                if not journal_mod.recover(paths):
                    return 1
                if procs.find(GAME_EXE) or procs.find(MO2_EXE):
                    print("The game or MO2 is running: close them first")
                    return 1
                manifest = stage(paths, args.source, args.name, args.preset, args.base_aio, args.pad, args.pdb)
            finally:
                lock.release()
            cs = (manifest.get("git") or {}).get("cs") or {}
            print(f"Staged {args.name} from {manifest['source']} ({manifest['mode']}): DLL {manifest['dll']['mtime']} "
                  f"sha256 {manifest['dll']['sha256'][:12]}" + (f", git {cs.get('sha', '')[:10]}{' dirty' if cs.get('dirty') else ''}" if cs else "")
                  + (f", shaders +{manifest['shaders']['copied']} -{manifest['shaders']['deleted']}" if "shaders" in manifest else ""))
            return 0
        if args.command == "run":
            from .runner import run

            result = run(paths, args.config, args.sets, args.label, args.stage, args.dry_run, wait_lock=args.wait)
            return 0 if result.get("outcome") in ("passed", "dry-run") else 1
        if args.command == "sweep":
            rows = commands.sweep(paths, args.config, args.base, args.vary, args.mode, args.sets, args.label, args.stage, args.dry_run, args.wait)
            return 0 if all(r.get("outcome") in ("passed", "dry-run") for _, r in rows) else 1
        if args.command == "compare":
            commands.compare(paths, args.a, args.b, shots=not args.no_shots, memory=not args.no_memory)
            return 0
        if args.command == "status":
            commands.status(paths)
            return 0
        if args.command == "restore":
            return 0 if commands.restore(paths, original=not args.journal_only) else 1
        if args.command == "check-schema":
            return 0 if commands.check_schema(paths, args.dump) == 0 else 1
    except (ConfigError, journal_mod.LockError) as error:
        print(f"error: {error}")
        return 2
    except Exception as error:  # noqa: BLE001 - one readable line for the expected failures below
        from .stage import StageError
        from .mo2 import Mo2Error

        if isinstance(error, (StageError, Mo2Error)):
            print(f"error: {error}")
            return 2
        raise
    return 0


if __name__ == "__main__":
    sys.exit(main())
