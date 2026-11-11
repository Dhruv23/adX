"""Fetch the sample libraries into the library store (phase_12.md 4.8).

    python tools/packs/fetch_libraries.py list
    python tools/packs/fetch_libraries.py fetch vcsl [vsco2ce salamander ...]
    python tools/packs/fetch_libraries.py verify [vcsl ...]

Libraries are data, not dependencies. Each is a git repository pinned to one commit,
fetched shallow into ``<store>/<id>@<commit12>`` and checked file by file against a
SHA-256 manifest committed under engine/preset/libraries/. A file that does not match
is restored from the clone's own objects and checked again; a file that still does not
match fails the run. The store is %LOCALAPPDATA%\\adX\\libraries, or ADX_LIBRARY_STORE.

``fetch --write-manifest`` writes the manifest from what was fetched: the first fetch of
a newly pinned commit, whose git object ids are already the upstream's word for it.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
MANIFESTS = REPO / "engine" / "preset" / "libraries"


@dataclass(frozen=True)
class Library:
    id: str
    title: str
    url: str
    commit: str
    licence: str
    redistributable: bool
    gigabytes: float
    credit: str = ""

    @property
    def folder(self) -> str:
        return f"{self.id}@{self.commit[:12]}"


LIBRARIES: dict[str, Library] = {
    lib.id: lib
    for lib in (
        Library(
            "vsco2ce",
            "VSCO 2 Community Edition (SFZ)",
            "https://github.com/sgossner/VSCO-2-CE",
            "6dd651d55dde97fd4028699be9d4481f26917891",
            "CC0-1.0",
            True,
            3.1,
            "Versilian Studios",
        ),
        Library(
            "vcsl",
            "Versilian Community Sample Library (SFZ)",
            "https://github.com/sgossner/VCSL",
            "dfcf4a4918771eee884b96ad4493de82ef84daf6",
            "CC0-1.0",
            True,
            5.9,
            "Versilian Studios",
        ),
        Library(
            "salamander",
            "Salamander Grand Piano V3",
            "https://github.com/sfzinstruments/SalamanderGrandPiano",
            "3382bf9496bba2486f5ab0de55a264d1dfc38404",
            "CC-BY-3.0",
            True,
            1.9,
            "Alexander Holm",
        ),
    )
}


def store_root() -> Path:
    override = os.environ.get("ADX_LIBRARY_STORE")
    if override:
        return Path(override)
    local = os.environ.get("LOCALAPPDATA")
    base = Path(local) if local else Path.home() / ".local" / "share"
    return base / "adX" / "libraries"


def manifest_path(lib: Library, manifests: Path = MANIFESTS) -> Path:
    return manifests / f"{lib.id}.sha256"


def git(args: list[str], cwd: Path) -> str:
    done = subprocess.run(
        ["git", "-c", "core.autocrlf=false", "-c", "core.longpaths=true", *args],
        cwd=cwd,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if done.returncode != 0:
        raise RuntimeError(f"git {' '.join(args)}: {done.stderr.strip()}")
    return done.stdout


def sha256_of(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        while chunk := handle.read(1 << 20):
            digest.update(chunk)
    return digest.hexdigest()


def library_files(folder: Path) -> list[str]:
    """Every file of the checkout, as forward-slash paths, sorted."""
    out = []
    for path in folder.rglob("*"):
        if path.is_file() and ".git" not in path.relative_to(folder).parts:
            out.append(path.relative_to(folder).as_posix())
    return sorted(out)


def hash_all(folder: Path, names: list[str]) -> dict[str, str]:
    with ThreadPoolExecutor(max_workers=8) as pool:
        return dict(zip(names, pool.map(lambda n: sha256_of(folder / n), names), strict=True))


def read_manifest(path: Path) -> dict[str, str]:
    entries: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        digest, _, name = line.partition("  ")
        entries[name] = digest
    return entries


def write_manifest(lib: Library, folder: Path, path: Path) -> int:
    hashes = hash_all(folder, library_files(folder))
    lines = [f"# {lib.id} {lib.commit} {lib.licence}"]
    lines += [f"{digest}  {name}" for name, digest in sorted(hashes.items())]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    return len(hashes)


def clone(lib: Library, folder: Path) -> None:
    folder.mkdir(parents=True, exist_ok=True)
    if not (folder / ".git").exists():
        git(["init", "-q"], folder)
        git(["remote", "add", "origin", lib.url], folder)
    git(["fetch", "-q", "--depth", "1", "origin", lib.commit], folder)
    git(["checkout", "-q", "--force", lib.commit], folder)


@dataclass
class VerifyResult:
    checked: int
    repaired: list[str]
    failed: list[str]

    @property
    def ok(self) -> bool:
        return not self.failed


def verify(lib: Library, folder: Path, manifest: Path, *, repair: bool = True) -> VerifyResult:
    """Checks every manifest entry; restores and re-checks the ones that differ."""
    wanted = read_manifest(manifest)
    present = [name for name in wanted if (folder / name).is_file()]
    actual = hash_all(folder, present)
    bad = sorted(name for name in wanted if actual.get(name) != wanted[name])
    repaired: list[str] = []
    failed: list[str] = []
    for name in bad:
        if repair and (folder / ".git").exists():
            try:
                git(["checkout", "--force", lib.commit, "--", name], folder)
            except RuntimeError:
                failed.append(name)
                continue
            if (folder / name).is_file() and sha256_of(folder / name) == wanted[name]:
                repaired.append(name)
                continue
        failed.append(name)
    return VerifyResult(checked=len(wanted), repaired=repaired, failed=failed)


def write_index(root: Path) -> None:
    """index.json: library id -> folder, which is what lib:// paths resolve through."""
    index = {}
    for lib in LIBRARIES.values():
        if (root / lib.folder).is_dir():
            index[lib.id] = {
                "folder": lib.folder,
                "commit": lib.commit,
                "licence": lib.licence,
                "redistributable": lib.redistributable,
                "credit": lib.credit,
            }
    root.mkdir(parents=True, exist_ok=True)
    (root / "index.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")


def fetch(lib: Library, root: Path, manifests: Path, *, write: bool) -> VerifyResult:
    folder = root / lib.folder
    clone(lib, folder)
    manifest = manifest_path(lib, manifests)
    if write or not manifest.exists():
        if not write:
            raise RuntimeError(f"{lib.id}: no manifest at {manifest}; run with --write-manifest")
        count = write_manifest(lib, folder, manifest)
        print(f"{lib.id}: wrote {manifest.name} ({count} files)")
    result = verify(lib, folder, manifest)
    write_index(root)
    return result


def report(lib: Library, result: VerifyResult) -> None:
    state = "ok" if result.ok else "FAILED"
    print(
        f"{lib.id}: {state} - {result.checked} files checked, "
        f"{len(result.repaired)} repaired, {len(result.failed)} failed"
    )
    for name in result.failed[:20]:
        print(f"  mismatch: {name}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("command", choices=["list", "fetch", "verify"])
    parser.add_argument("libraries", nargs="*")
    parser.add_argument("--store", type=Path, default=None)
    parser.add_argument("--write-manifest", action="store_true")
    args = parser.parse_args(argv)
    root = args.store or store_root()

    if args.command == "list":
        for lib in LIBRARIES.values():
            have = "fetched" if (root / lib.folder).is_dir() else "absent"
            print(
                f"{lib.id:<12} {lib.gigabytes:>4.1f} GB  {lib.licence:<10} {have:<8} {lib.title}"
            )
        return 0

    names = args.libraries or list(LIBRARIES)
    unknown = [n for n in names if n not in LIBRARIES]
    if unknown:
        print(f"unknown library: {', '.join(unknown)}", file=sys.stderr)
        return 2
    ok = True
    for name in names:
        lib = LIBRARIES[name]
        try:
            if args.command == "fetch":
                result = fetch(lib, root, MANIFESTS, write=args.write_manifest)
            else:
                result = verify(lib, root / lib.folder, manifest_path(lib))
        except RuntimeError as error:
            print(f"{lib.id}: {error}", file=sys.stderr)
            ok = False
            continue
        report(lib, result)
        ok = ok and result.ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
