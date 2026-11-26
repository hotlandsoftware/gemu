#!/usr/bin/env python3
"""Check that vendored third-party code is what upstream released.

    tools/check-third-party.py                    # the check CI runs (offline)
    tools/check-third-party.py --tag CLONE        # the manifest against the tag
    tools/check-third-party.py --update CLONE     # rewrite the manifest from the tag

Each vendored tree (third_party/86box/v6.0) has a README.md whose table lists
every file with its local changes ("none" or what and why) and a MANIFEST of
the SHA-256 of each file as upstream's tag has it.  The check fails when:

  - a file differs from the manifest and its README row says "none";
  - a file is in the tree but not in the README (an unlisted file);
  - a file in the README or the manifest isn't in the tree, or the README
    and the manifest list different files.

--tag CLONE compares the manifest with `git show TAG:path` in a clone of the
upstream repository (the tag and commit from the README), so a manifest
can't drift from the release; --update writes it from there (adding a file:
copy it, list it in the README, then --update).
"""
import hashlib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TREES = [ROOT / 'third_party' / '86box' / 'v6.0']
OWN = {'README.md', 'MANIFEST'}            # the tree's own bookkeeping, not upstream's


def readme_rows(tree):
    """{path: local changes} from the README's file table."""
    rows = {}
    for line in (tree / 'README.md').read_text(encoding='utf-8').splitlines():
        m = re.match(r'\|\s*`([^`]+)`\s*\|.*\|\s*([^|]+?)\s*\|\s*$', line)
        if m:
            rows[m.group(1)] = m.group(2)
    return rows


def readme_tag(tree):
    text = (tree / 'README.md').read_text(encoding='utf-8')
    tag = re.search(r'release tag \*\*([^*]+)\*\*', text)
    commit = re.search(r'commit `([0-9a-f]{40})`', text)
    if not tag or not commit:
        sys.exit(f'{tree}/README.md: no "release tag **TAG**" and "commit `SHA`"')
    return tag.group(1), commit.group(1)


def manifest(tree):
    m = {}
    path = tree / 'MANIFEST'
    if path.exists():
        for line in path.read_text(encoding='utf-8').splitlines():
            if line.strip():
                digest, name = line.split(None, 1)
                m[name.strip()] = digest
    return m


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def files_in(tree):
    return sorted(str(p.relative_to(tree)).replace('\\', '/') for p in tree.rglob('*')
                  if p.is_file() and p.name not in OWN)


def upstream(clone, tree, name):
    tag, commit = readme_tag(tree)
    got = subprocess.run(['git', '-C', str(clone), 'rev-parse', f'{tag}^{{commit}}'],
                         capture_output=True, text=True)
    if got.returncode or got.stdout.strip() != commit:
        sys.exit(f'{clone}: tag {tag} is {got.stdout.strip() or "missing"}, '
                 f'the README says {commit}')
    blob = subprocess.run(['git', '-C', str(clone), 'show', f'{commit}:{name}'], capture_output=True)
    if blob.returncode:
        sys.exit(f'{clone}: {name} is not in {tag}')
    return blob.stdout


def check(tree):
    errors = []
    rows, man, have = readme_rows(tree), manifest(tree), files_in(tree)
    rel = tree.relative_to(ROOT)
    for name in have:
        if name not in rows:
            errors.append(f'{rel}/{name}: not listed in README.md (an unlisted file)')
    for name in rows:
        if name not in have:
            errors.append(f'{rel}/{name}: listed in README.md but not there')
        if name not in man:
            errors.append(f'{rel}/{name}: not in MANIFEST (tools/check-third-party.py --update)')
    for name in man:
        if name not in rows:
            errors.append(f'{rel}/{name}: in MANIFEST but not in README.md')
    for name in have:
        if name in rows and name in man:
            same = sha256((tree / name).read_bytes()) == man[name]
            if not same and rows[name].lower() == 'none':
                errors.append(f'{rel}/{name}: differs from upstream, and README.md lists no '
                              f'local change')
    return errors


def main():
    args = sys.argv[1:]
    if args and args[0] in ('--tag', '--update'):
        if len(args) != 2:
            sys.exit(__doc__)
        clone = Path(args[1])
        for tree in TREES:
            rows = readme_rows(tree)
            if args[0] == '--update':
                lines = [f'{sha256(upstream(clone, tree, n))}  {n}\n' for n in sorted(rows)]
                (tree / 'MANIFEST').write_text(''.join(lines), encoding='utf-8')
                print(f'{tree.relative_to(ROOT)}/MANIFEST: {len(lines)} files from the tag')
            else:
                man, bad = manifest(tree), 0
                for name in sorted(rows):
                    if man.get(name) != sha256(upstream(clone, tree, name)):
                        print(f'{tree.relative_to(ROOT)}/MANIFEST: {name} is not the tag\'s')
                        bad += 1
                if bad:
                    sys.exit(1)
                print(f'{tree.relative_to(ROOT)}/MANIFEST: all {len(rows)} files match the tag')
        return
    errors = [e for tree in TREES for e in check(tree)]
    for e in errors:
        print(e)
    if errors:
        sys.exit(1)
    n = sum(len(readme_rows(t)) for t in TREES)
    print(f'third_party: {n} files as upstream released them (or listed as changed)')


if __name__ == '__main__':
    main()
