#!/usr/bin/env python3
"""Builds store_check broken on purpose, each case from a copy of src/ with one thing changed, leaving the tree as it
is: <build>/breaks/<case>/store_check.xex, each of whose runs must fail the checks the case names. Run it in WSL, with
the main build's options: breaks.py <build> -DCMAKE_TOOLCHAIN_FILE=... -DSTORE_PUBLIC_KEY=..."""

from concurrent.futures import ThreadPoolExecutor
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# Each case: the file, the text it changes and what to, and the checks that must fail.
CASES = {
    "signature_unchecked": ("catalogue.c", "\treturn good;\n}", "\treturn good || true;\n}",
                            "byte changed; another key"),
    "serial_unchecked": ("catalogue.c", "have_kept && fresh.serial < kept.serial)",
                         "have_kept && fresh.serial < kept.serial && false)",
                         "older; nothing at its address, as the older one was kept"),
    "kept_cut_short": ("catalogue.c", "return store_write_file(path, start, 4 + signature_size, data, size);",
                       "return store_write_file(path, start, 4 + signature_size, data, size / 2);",
                       "byte changed; another key; older; nothing at its address; a signed new address, as none is "
                       "kept to follow"),
    "address_ignored": ("catalogue.c", "have_kept ? kept.url : STORE_DEFAULT_URL",
                        "have_kept ? STORE_DEFAULT_URL : STORE_DEFAULT_URL", "a signed new address"),
    "shot_hash_unchecked": ("shots.c", "memcmp(hash, app->screenshot_sha256, sizeof hash) != 0",
                            "memcmp(hash, app->screenshot_sha256, 0) != 0", "one that isn't the signed one"),
    "shot_not_kept": ("shots.c", "if (take_kept(path, app, pixels))", "if (take_kept(path, app, pixels) && false)",
                      "the kept one comes back"),
    "title_id_ignored": ("catalogue.c", "title_id == app->title_id)", "(title_id == app->title_id || true))",
                         "another title ID"),
}


def build(name, out, options):
    file, old, new, _ = CASES[name]
    directory = out / name
    if directory.exists():
        shutil.rmtree(directory)
    source = directory / "src"
    shutil.copytree(ROOT / "src", source)
    text = (source / file).read_text()
    if text.count(old) != 1:
        raise ValueError(f"{name}: {old!r} isn't in {file} once")
    (source / file).write_text(text.replace(old, new))
    log = directory / "build.log"
    with log.open("w") as output:
        for command in (["cmake", "-S", str(ROOT), "-B", str(directory / "build"), "-GNinja",
                         f"-DSTORE_CORE_DIR={source}", *options],
                        ["cmake", "--build", str(directory / "build"), "--target", "store_check", "-j8"]):
            subprocess.run(command, stdout=output, stderr=subprocess.STDOUT, check=True)
    shutil.copy(directory / "build" / "store_check.xex", directory / "store_check.xex")
    return name


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1]) / "breaks"
    out.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(4) as pool:
        for name in pool.map(lambda case: build(case, out, sys.argv[2:]), CASES):
            print(f"breaks: {name}: must fail {CASES[name][3]}")


if __name__ == "__main__":
    main()
