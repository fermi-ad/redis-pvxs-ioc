#!/usr/bin/env python3
"""Apply the project-owned extension to the existing pinned PVXS source tree.

Only files matching the recorded before/after hashes are accepted. Patch work
is validated in a private scratch tree before any source files are replaced.
Re-running after a build or a Docker source overlay is safe and deterministic.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def run(args, cwd):
    env = dict(os.environ)
    for key in ('GIT_DIR', 'GIT_WORK_TREE', 'GIT_INDEX_FILE', 'GIT_COMMON_DIR'):
        env.pop(key, None)
    env['GIT_CEILING_DIRECTORIES'] = str(cwd)
    p = subprocess.run(args, cwd=cwd, env=env, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if p.returncode:
        raise RuntimeError(p.stderr.strip() or ('command failed: ' + ' '.join(args)))
    return p.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('--check', action='store_true', help='require the source to be fully patched without modifying it')
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    patches_dir = Path(__file__).resolve().parents[1] / 'patches/pvxs'
    manifest = json.loads((patches_dir / 'manifest.json').read_text())
    if manifest.get('base_revision') != '749a8933b7982cb4dd9c16660a53f93bcdd3d67e':
        raise RuntimeError('patch manifest does not describe the existing pinned PVXS revision')
    if manifest.get('api_version') != 1:
        raise RuntimeError('unsupported project PVXS proxy API version')
    files = manifest['files']
    patches = [patches_dir / manifest.get('patch', '0001-xrm-proxy-api.patch')]
    if not files or len(patches) != 1:
        raise RuntimeError('expected one integrated, nonempty PVXS patch')
    if patches[0].resolve().parent != patches_dir.resolve() or sha(patches[0]) != manifest['patch_sha256']:
        raise RuntimeError('project PVXS patch checksum/path mismatch')
    for relative in files:
        path = Path(relative)
        if path.is_absolute() or '..' in path.parts or any(c in relative for c in '\t\r\n'):
            raise RuntimeError('unsafe patch path: ' + relative)
        target = source / relative
        if target.is_symlink() or target.resolve() != target:
            raise RuntimeError('symlinked PVXS patch target: ' + relative)
        if target.exists() and not target.is_file():
            raise RuntimeError('PVXS patch target is not a regular file: ' + relative)
    states = {}
    for relative, record in files.items():
        actual = sha(source / relative)
        before, after = record.get('before_sha256'), record['after_sha256']
        if actual == after:
            states[relative] = 'after'
        elif actual == before:
            states[relative] = 'before'
        else:
            raise RuntimeError('PVXS source differs from pinned/patched input: ' + relative)
    if all(s == 'after' for s in states.values()):
        print('PVXS project proxy extension already applied')
        return
    if args.check:
        raise RuntimeError('PVXS source is not fully patched')
    with tempfile.TemporaryDirectory(prefix='pvxs-project-patch-') as temp:
        scratch = Path(temp).resolve()
        for relative in files:
            original = source / relative
            if original.exists():
                target = scratch / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(original, target)
        touched = set()
        for patch in patches:
            for line in run(['git', 'apply', '--numstat', str(patch)], scratch).splitlines():
                fields = line.split('\t')
                if len(fields) != 3 or fields[2] not in files or '-' in fields[:2]:
                    raise RuntimeError('patch has an unexpected/binary target')
                touched.add(fields[2])
        if touched != set(files):
            raise RuntimeError('patch target list differs from the hash manifest')
        already = [p for p, state in states.items() if state == 'after']
        if already:
            include = ['--include=' + p for p in already]
            run(['git', 'apply', '--reverse', '--check', *include, str(patches[0])], scratch)
            run(['git', 'apply', '--reverse', *include, str(patches[0])], scratch)
        for relative, record in files.items():
            if sha(scratch / relative) != record.get('before_sha256'):
                raise RuntimeError('could not reconstruct pristine PVXS input: ' + relative)
        run(['git', 'apply', '--check', str(patches[0])], scratch)
        run(['git', 'apply', str(patches[0])], scratch)
        for relative, record in files.items():
            if sha(scratch / relative) != record['after_sha256']:
                raise RuntimeError('patched output hash mismatch: ' + relative)
        for relative, state in states.items():
            if state == 'after':
                continue
            target, ready = source / relative, scratch / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            fd, temporary = tempfile.mkstemp(prefix='.pvxs-patch-', dir=target.parent)
            try:
                with os.fdopen(fd, 'wb') as output:
                    output.write(ready.read_bytes())
                    output.flush()
                    os.fchmod(output.fileno(), ready.stat().st_mode & 0o777)
                    os.fsync(output.fileno())
                os.replace(temporary, target)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
    print('Applied project-owned PVXS proxy extension')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, RuntimeError) as error:
        raise SystemExit(str(error))
