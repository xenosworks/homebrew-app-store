# Homebrew App Store

An Xbox 360 title for finding, installing and updating homebrew, built on
[xsdk](https://github.com/xenosworks/xsdk) for consoles that run unsigned code. Its catalogue is
[homebrew-app-catalogue](https://github.com/xenosworks/homebrew-app-catalogue), signed with xenosworks's key; anyone's
homebrew can be listed there by pull request, and the store marks what isn't by xenosworks. The plan is xsdk's
docs/scope/63-homebrew-store.md.

So far the store shows the catalogue: its categories, each category's apps with their screenshots, and each app's page,
with whether it's installed in `hdd:\apps\<id>` and whether an update is ready. Installing comes next.

## How it trusts what it shows

- The catalogue comes over HTTPS with a signature beside it, ECDSA on P-256, and the store takes it only if the key
  built into the store signed it. Every download's SHA-256 is in it, screenshots included, so one signature covers
  everything the store shows or installs.
- It keeps the last catalogue it took on the hard disk, and shows it when the network is down or what comes is refused.
  It refuses a catalogue older than the one it kept, so an old one can't roll apps back, and follows a signed new
  address if the catalogue ever moves.

## Building

Build xsdk (`make` in its checkout), and check out [imgui-xenon](https://github.com/xenosworks/imgui-xenon),
[curl-xenon](https://github.com/xenosworks/curl-xenon) and [mbedtls-xenon](https://github.com/xenosworks/mbedtls-xenon)
beside this repository. Then, in WSL or Linux:

```bash
cmake -S . -B build -GNinja -DCMAKE_TOOLCHAIN_FILE=<xsdk>/cmake/xsdk.cmake -DCMAKE_BUILD_TYPE=Release \
    -DSTORE_PUBLIC_KEY=<the catalogue's key.pub.pem>
cmake --build build
```

That makes `build/homebrew_app_store.xex`; `cacert.pem` goes beside it on the console. `STORE_PUBLIC_KEY` has no
default: a store trusts only the key it's built with.

## Checking

`store_check.xex` runs the store's checks on the console against a test site on the PC, signed with a test key:

```bash
python3 tests/make_site.py build/test
python3 <xsdk>/tools/net_http.py --site build/test/site
```

Build with `-DSTORE_PUBLIC_KEY=build/test/keys/test.pub.pem`, then put `store_check.xex`, `test-ca2.crt` and
`net_http.txt`, holding the PC's address and port as `192.168.0.125 7380`, in a folder on the console and start it.
Its log, `store_check.log`, ends with a count of its checks passed and failed. `tests/breaks.py` builds it broken on
purpose, a case at a time, each of which fails the checks it names.

The store itself takes a `store.txt` beside it for the same site: `url=https://localhost:7381/site/good/catalogue.json`,
`ca=test-ca2.crt` and `resolve=localhost:7381:192.168.0.125`, and `shots=1` to write each screen and end.

## Licence

MIT (LICENSE). What the `.xex` carries from others is in THIRD_PARTY_NOTICES.md.
