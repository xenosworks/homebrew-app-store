# Homebrew App Store

An Xbox 360 title for finding, installing and updating homebrew, built on
[xsdk](https://github.com/xenosworks/xsdk) for consoles that run unsigned code. Its catalogue is
[homebrew-app-catalogue](https://github.com/xenosworks/homebrew-app-catalogue), signed with xenosworks's key; anyone's
homebrew can be listed there by pull request, and the store marks what isn't by xenosworks. The plan is xsdk's
docs/scope/63-homebrew-store.md.

The store shows the catalogue, its categories, each category's apps with their screenshots and each app's page;
installs, updates, removes and launches apps in `hdd:\apps\<id>`; and updates itself. Before an app not by xenosworks
first installs, it says so and waits for A.

## How it trusts what it shows

- The catalogue comes over HTTPS with a signature beside it, ECDSA on P-256, and the store takes it only if the key
  built into the store signed it. Every download's SHA-256 is in it, screenshots included, so one signature covers
  everything the store shows or installs.
- It keeps the last catalogue it took on the hard disk, and shows it when the network is down or what comes is refused.
  It refuses a catalogue older than the one it kept, so an old one can't roll apps back, and follows a signed new
  address if the catalogue ever moves.

## How it installs

- A package is a zip. It downloads into memory, a thread of its own putting each piece on the disk as it comes, so a
  cut download is taken up where it stopped. Its SHA-256 must be the catalogue's before it's opened.
- Each file is unpacked beside its place as `<name>.part`, its own SHA-256 checked against the catalogue, then renamed
  into place. `app.json`, the store's record of what it installed, is written first as `app.json.pending` and renamed
  `app.json.part` once every file checks: a cut install is taken back on the next start if it hadn't got that far, and
  finished if it had.
- An update replaces the files `app.json` lists and deletes the ones the new version drops; a removal deletes them, and
  then offers to delete what the app made itself, its saves and settings, which otherwise stay.
- The store finds its own entry in the catalogue by its title ID. That entry installs in the store's own folder, over
  the `.xex` it's running from, which the console allows, and then the store starts itself again.

## Coming back to the store

The store starts an app with xsdk's `xsdk_launch`, its launch data the store's own path, ended by a 0. An app that
offers a way back reads it and launches it:

```c
char store[XSDK_LAUNCH_DATA_MAX_BYTES];
const size_t size = xsdk_launch_data(store, sizeof store);
if (size && memchr(store, '\0', size))
	xsdk_launch(store, NULL, 0);
```

## Building

Build xsdk (`make` in its checkout), and check out [imgui-xenon](https://github.com/xenosworks/imgui-xenon),
[curl-xenon](https://github.com/xenosworks/curl-xenon) and [mbedtls-xenon](https://github.com/xenosworks/mbedtls-xenon)
beside this repository. Then, in WSL or Linux:

```bash
cmake -S . -B build -GNinja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

That makes `build/homebrew_app_store.xex`; `cacert.pem` goes beside it on the console. It trusts the catalogue's key,
`keys/catalogue.pub.pem`, alone; `STORE_PUBLIC_KEY` names another for a build signed some other way, as the checks'.

## Checking

`store_check.xex` runs the store's checks on the console against a test site on the PC, signed with a test key:

```bash
python3 tests/make_site.py build/test
```

Build with `-DSTORE_PUBLIC_KEY=build/test/keys/test.pub.pem`, then write the site again with the builds it serves:
xsdk's `launch_target.xex`, which launches back what started it, and the newer store_check and store:

```bash
python3 tests/make_site.py build/test --launch-target <xsdk>/build/launch_target.xex \
    --self-update build/store_check_v2.xex --store-update build/homebrew_app_store_v2.xex
python3 <xsdk>/tools/net_http.py --site build/test/site
```

Put `store_check.xex`, `test-ca2.crt` and
`net_http.txt`, holding the PC's address and port as `192.168.0.125 7380`, in a folder on the console and start it.
It runs its checks, installs and launches launch_target, which launches it back, then updates itself and starts again;
its log, `store_check.log`, ends with a count of its checks passed and failed. `tests/breaks.py` builds it broken on
purpose, a case at a time, each of which fails the checks it names.

The store itself takes a `store.txt` beside it for the same site: `url=https://localhost:7381/site/good/catalogue.json`,
`ca=test-ca2.crt` and `resolve=localhost:7381:192.168.0.125`, `apps=apps` to install in a folder beside it rather than
`hdd:\apps`, and `shots=1` to go through the screens by itself, writing each, and end.

## Licence

MIT (LICENSE). What the `.xex` carries from others is in THIRD_PARTY_NOTICES.md.
