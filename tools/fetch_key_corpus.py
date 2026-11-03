"""Fetch the GiantSteps Key dataset: 604 two-minute EDM previews with keys corrected by
listeners (Knees et al., ISMIR 2015). Phase 10's key detector is measured on it
(phase_10.md §11, STATE.md P10-1): it is the labelled, recorded corpus §6 asks for.

The audio is Beatport's, so it is a local test fixture: cached outside the repository,
never committed, never shipped. Each file is checked against the dataset's own MD5 list
before it is kept; a mismatch is refused, not retried.

    py -3.12 tools/fetch_key_corpus.py            # all 604 (about 850 MB)
    py -3.12 tools/fetch_key_corpus.py --limit 100
"""

from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import sys
import urllib.request

#: The dataset's annotations and MD5 list, pinned to one commit of its repository.
DATASET_COMMIT = "master"
RAW = f"https://raw.githubusercontent.com/GiantSteps/giantsteps-key-dataset/{DATASET_COMMIT}"
LISTING = "https://api.github.com/repos/GiantSteps/giantsteps-key-dataset/contents/md5"
#: The dataset's audio host since 2025 (audio_dl.sh), then Beatport's own.
MIRRORS = (
    "https://www.cp.jku.at/datasets/giantsteps/backup/",
    "https://geo-samples.beatport.com/lofi/",
)


def default_cache() -> pathlib.Path:
    base = os.environ.get("ADX_CORPUS_DIR") or os.path.join(
        os.environ.get("LOCALAPPDATA", str(pathlib.Path.home())), "adx-corpus"
    )
    return pathlib.Path(base) / "giantsteps-key"


def _get(url: str, timeout: float = 60.0) -> bytes:
    request = urllib.request.Request(url, headers={"User-Agent": "adx-fetch-key-corpus"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        data: bytes = response.read()
        return data


def track_ids() -> list[str]:
    listing = json.loads(_get(LISTING))
    return sorted(str(entry["name"]).removesuffix(".md5") for entry in listing)


def fetch_one(name: str, cache: pathlib.Path) -> tuple[str, str]:
    """(name, status): ok, cached, or why it failed."""
    audio = cache / "audio" / f"{name}.mp3"
    key_file = cache / "key" / f"{name}.key"
    if not key_file.exists():
        key_file.write_bytes(_get(f"{RAW}/annotations/key/{name}.key"))
    want = _get(f"{RAW}/md5/{name}.md5").decode().split()[0].strip().lower()
    if audio.exists() and hashlib.md5(audio.read_bytes()).hexdigest() == want:
        return name, "cached"
    for mirror in MIRRORS:
        try:
            data = _get(f"{mirror}{name}.mp3", timeout=120.0)
        except OSError:
            continue
        if hashlib.md5(data).hexdigest() == want:
            audio.write_bytes(data)
            return name, "ok"
    return name, "failed: no mirror served the file with its published MD5"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cache", type=pathlib.Path, default=default_cache())
    parser.add_argument("--limit", type=int, default=0, help="only the first N tracks (by id)")
    parser.add_argument("--jobs", type=int, default=6)
    args = parser.parse_args(argv)
    cache: pathlib.Path = args.cache
    (cache / "audio").mkdir(parents=True, exist_ok=True)
    (cache / "key").mkdir(parents=True, exist_ok=True)
    names = track_ids()
    if args.limit:
        names = names[: args.limit]
    failed = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
        for name, status in pool.map(lambda n: fetch_one(n, cache), names):
            if status.startswith("failed"):
                failed += 1
                print(f"{name}: {status}", file=sys.stderr)
    print(f"fetch_key_corpus: {len(names) - failed} of {len(names)} tracks in {cache}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
