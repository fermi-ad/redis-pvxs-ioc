#!/usr/bin/env python3
"""Build the independent PVA test peer from the project's pinned Base modules.

No dependency download or tracked source modification occurs. Example:
  python3 tests/build_pva_native_reference.py \
    --source-base /path/to/populated/third_party/epics-base \
    --built-base /path/to/built/third_party/epics-base \
    --build-dir /tmp/pva-native-reference
"""
import argparse
import os
from pathlib import Path
import platform
import shlex
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-base", required=True, type=Path)
    parser.add_argument("--built-base", required=True, type=Path)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--jobs", type=int, default=4)
    args = parser.parse_args()
    root = args.build_dir.resolve()
    base = args.built_base.resolve()
    source = args.source_base.resolve()
    env = dict(os.environ, LC_ALL="C", LANG="C")
    arch = subprocess.check_output(
        ["perl", str(base / "lib/perl/EpicsHostArch.pl")], text=True, env=env
    ).strip()
    root.mkdir(parents=True, exist_ok=True)
    for name in ("pvData", "pvAccess", "pvDatabase"):
        dest = root / name
        if not dest.exists():
            shutil.copytree(source / "modules" / name, dest,
                            ignore=shutil.ignore_patterns(".git", "O.*", "lib", "bin", "include", "cfg"))
        release = (f"EPICS_BASE = {base}\n"
                   f"EPICS_PVDATA = {root / 'pvData'}\n"
                   f"EPICS_PVACCESS = {root / 'pvAccess'}\n")
        (dest / "configure/RELEASE.local").write_text(release)
        command = ["make", "-C", str(dest), f"-j{args.jobs}", "configure", "src"]
        if name == "pvDatabase":
            # syncChannelFind.h is a header-only helper normally installed by
            # pvAccessIOC; including its pinned source avoids building an IOC.
            command.append(f"USR_INCLUDES=-I{root / 'pvAccess/src/ioc'}")
        print(shlex.join(command), flush=True)
        with (root / f"build-{name}.log").open("w") as log:
            subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT, check=True)
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    compiler = "clang" if platform.system() == "Darwin" else "gcc"
    osdir = "Darwin" if platform.system() == "Darwin" else "Linux"
    command = cxx + ["-std=c++17", "-O1", "-g"]
    command += [f"-I{root / name / 'include'}" for name in ("pvData", "pvAccess", "pvDatabase")]
    command += [f"-I{base / 'include'}", f"-I{base / 'include' / 'os' / osdir}",
                f"-I{base / 'include' / 'compiler' / compiler}"]
    command += [str(Path(__file__).with_name("pva_native_reference.cpp"))]
    libraries = [root / name / "lib" / arch for name in ("pvData", "pvAccess", "pvDatabase")]
    libraries.append(base / "lib" / arch)
    for directory in libraries:
        command += [f"-L{directory}", f"-Wl,-rpath,{directory}"]
    command += ["-lpvDatabase", "-lpvAccess", "-lpvData", "-lCom", "-pthread",
                "-o", str(root / "pva_native_reference")]
    print(shlex.join(command), flush=True)
    subprocess.run(command, env=env, check=True)
    print(root / "pva_native_reference", flush=True)


if __name__ == "__main__":
    main()
