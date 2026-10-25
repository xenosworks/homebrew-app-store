#!/usr/bin/env python3
"""Writes the test site store_check fetches from, with xsdk's tools/store_catalogue.py: <out>/keys holds a test key,
and another, and <out>/site the catalogues xsdk's tools/net_http.py --site serves under /site/:
- good/, serial 5: two categories and two apps, alpha by xenosworks and beta by someone else, each a .xex and a data
  file, with screenshots; beta's screenshot is changed after signing, so it isn't the signed one;
- byte/, good's with one byte changed and good's signature; otherkey/, good's signed with the other key;
- old/, serial 4; moved/, serial 6, naming moved_to/, serial 7, as its new address;
- drop/, serial 5, good's but its packages under /site-drop/, which sends half and closes;
- badupdate/, serial 8, alpha 1.1 with a byte of its package changed after signing; update/, serial 9, alpha 1.1, its
  data file data/new.txt in place of data/readme.txt;
- badfile/, serial 5, good's with beta's data file signed with a SHA-256 its package's copy hasn't.
Build the store and store_check with -DSTORE_PUBLIC_KEY=<out>/keys/test.pub.pem. A test key, never the catalogue's."""

import argparse
import copy
import json
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CATEGORIES = [
    {"id": "tools", "name": "Tools", "sentence": "Things to run on the console: file managers, network tools, tests."},
    {"id": "games", "name": "Games", "sentence": "Homebrew games, and ports of games whose source is open."},
]
APPS = [
    {"id": "alpha", "name": "Alpha", "author": "xenosworks", "category": "tools",
     "sentence": "The first test app, by xenosworks.", "needs": "", "changes": "The first version.",
     "source": "https://github.com/xenosworks/homebrew-app-store", "license": "MIT", "screenshot": "shots/alpha.png",
     "launch": "alpha.xex", "title_id": 0x58534101, "version": (1, 0, 0, 0),
     "data": {"data/readme.txt": b"alpha's first data file\n"}},
    {"id": "beta", "name": "Beta", "author": "Someone Else", "category": "games",
     "sentence": "The second test app, by someone else.", "needs": "A controller", "changes": "Faster.",
     "source": "https://github.com/xenosworks/homebrew-app-store", "license": "MIT", "screenshot": "shots/beta.png",
     "launch": "beta.xex", "title_id": 0x58534102, "version": (2, 1, 0, 3),
     "data": {"data/readme.txt": b"beta's data file\n"}},
]
FIELDS = ("id", "name", "author", "category", "sentence", "needs", "changes", "source", "license", "screenshot",
          "launch")


def png(width, height, row):
    """A PNG of 8-bit RGB, every row the same."""
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    pixels = zlib.compress((b"\x00" + row) * height, 9)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", pixels) + chunk(b"IEND", b"")


def xex(title_id, version):
    """A .xex's start as far as its execution info, which is all store_catalogue.py and the store read."""
    data = bytearray(0x38)
    data[0:4] = b"XEX2"
    for offset, word in ((0x14, 1), (0x18, 0x00040006), (0x1C, 0x20), (0x24, version), (0x2C, title_id)):
        struct.pack_into(">I", data, offset, word)
    return bytes(data)


def tool(xsdk, *arguments):
    subprocess.run([sys.executable, str(xsdk / "tools" / "store_catalogue.py"), *map(str, arguments)], check=True,
                   stdout=subprocess.DEVNULL)


def write_variant(xsdk, folder, packages_base, serial, url, key, work, apps, edit=None):
    shots = folder / "shots"
    shots.mkdir(parents=True)
    (shots / "alpha.png").write_bytes(png(1280, 720, b"\xff\xff\xff" * 640 + b"\xff\x00\x00" * 640))
    (shots / "beta.png").write_bytes(png(640, 640, b"\x00\x00\xff" * 640))
    catalogue = folder / "catalogue.json"
    catalogue.write_text(json.dumps({"format": 1, "serial": serial - 1, "url": url, "categories": CATEGORIES,
                                     "apps": [{field: app[field] for field in FIELDS} for app in apps]}, indent=2))
    (folder / "packages").mkdir()
    for app in apps:
        app_folder = work / folder.name / app["id"]
        app_folder.mkdir(parents=True)
        major, minor, build, fix = app["version"]
        (app_folder / app["launch"]).write_bytes(xex(app["title_id"], major << 28 | minor << 24 | build << 8 | fix))
        for path, data in app["data"].items():
            (app_folder / path).parent.mkdir(parents=True, exist_ok=True)
            (app_folder / path).write_bytes(data)
        tool(xsdk, "pack", catalogue, app["id"], app_folder, folder / "packages" / f"{app['id']}.zip", "--url",
             f"{packages_base}{folder.name}/packages/{app['id']}.zip")
    if edit:
        entries = json.loads(catalogue.read_text())
        edit(entries)
        catalogue.write_text(json.dumps(entries, indent=2))
    tool(xsdk, "sign", catalogue, key)


def wrong_file_hash(entries):
    """beta's data file signed with a SHA-256 its package's copy hasn't."""
    data_file = next(file for file in entries["apps"][1]["files"] if file["path"] == "data/readme.txt")
    data_file["sha256"] = "0" * 64


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out", type=Path)
    parser.add_argument("--xsdk", type=Path, default=ROOT.parent / "Xbox-360-SDK", help="an xsdk checkout")
    parser.add_argument("--port", type=int, default=7380, help="net_http.py's HTTP port; HTTPS is the next")
    args = parser.parse_args()
    keys = args.out / "keys"
    keys.mkdir(parents=True, exist_ok=True)
    for name in ("test", "other"):
        if not (keys / f"{name}.pem").exists():
            tool(args.xsdk, "keygen", keys / f"{name}.pem")
    site = args.out / "site"
    if site.exists():
        shutil.rmtree(site)
    base = f"https://localhost:{args.port + 1}/site/"
    drop_base = f"https://localhost:{args.port + 1}/site-drop/"
    updated = copy.deepcopy(APPS)
    updated[0].update(version=(1, 1, 0, 0), changes="The second version.", data={"data/new.txt": b"alpha's new one\n"})
    with tempfile.TemporaryDirectory() as directory:
        work = Path(directory)
        for name, serial, url, key, packages_base, apps in (
                ("good", 5, "good", "test", base, APPS), ("otherkey", 5, "otherkey", "other", base, APPS),
                ("old", 4, "old", "test", base, APPS), ("moved", 6, "moved_to", "test", base, APPS),
                ("moved_to", 7, "moved_to", "test", base, APPS), ("drop", 5, "drop", "test", drop_base, APPS),
                ("badupdate", 8, "badupdate", "test", base, updated), ("update", 9, "update", "test", base, updated),
                ("badfile", 5, "badfile", "test", base, APPS)):
            write_variant(args.xsdk, site / name, packages_base, serial, f"{base}{url}/catalogue.json",
                          keys / f"{key}.pem", work, apps, wrong_file_hash if name == "badfile" else None)
    byte = site / "byte"
    shutil.copytree(site / "good", byte)
    data = (byte / "catalogue.json").read_bytes()
    (byte / "catalogue.json").write_bytes(data.replace(b"The first test app", b"The first test apq", 1))
    (site / "good" / "shots" / "beta.png").write_bytes(png(640, 640, b"\x00\x00\xfe" * 640))
    # A package that isn't the one signed, at its size, so its SHA-256 is what tells.
    bad = bytearray((site / "badupdate" / "packages" / "alpha.zip").read_bytes())
    bad[len(bad) // 2] ^= 0x01
    (site / "badupdate" / "packages" / "alpha.zip").write_bytes(bad)
    print(f"make_site: {site}, its key's public half {keys / 'test.pub.pem'}")


if __name__ == "__main__":
    main()
