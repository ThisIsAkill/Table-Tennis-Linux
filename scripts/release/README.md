# Linux releases

Linux releases are tagged `linux-vX.Y.Z` (the plain tags `v1.0.x` are upstream's Windows
releases). A release holds only game-free files built by `make-release.sh`; players' machines
build the game from their own disc.

| File | Purpose |
|---|---|
| `install-table-tennis-linux-x86_64.tar.gz` | what players download: `TableTennis-Installer/` with the source snapshot, `VERSION`, the graphical installer and `README.txt` |
| `tt-toolchain-<id>.tar.zst` | slim Ubuntu 24.04 build toolchain for the installer's fast path (`toolchain-slim-recipe.sh`) |
| `tt-toolchain-sources-<id>.tar` | source packages for that toolchain (see [Sources](#sources)); never downloaded by the installer |
| `tt-sdk-<patchset>-x86-64-v3.tar.zst` | ReXGlue SDK v0.10.0 built with `project/sdk-patches/` |
| `manifest.json` | version, title, notes, date and every file's size and SHA256 |
| `manifest.json.sig` | detached Ed25519 signature of `manifest.json` |

Installs and updates fetch `manifest.json` and `manifest.json.sig` from
`https://github.com/ThisIsAkill/Table-Tennis-Rockstar-PC/releases/latest/download/`, refuse a
manifest whose signature does not verify against `scripts/install/release-public-key`, and then
check each downloaded file against the manifest's SHA256. The newest `linux-v*` release must
therefore be marked as the repository's latest release.

## Making a release

```sh
# once per machine: the signing key (see Key handling)
scripts/release/make-release.sh --gen-key

# release notes, one per line
cat > /tmp/notes.txt <<'EOF'
- Faster first install: prebuilt build tools
- Updates from the game's PC SETTINGS
EOF

# everything (about 15 minutes from nothing; the toolchain, sources and SDK are reused later)
scripts/release/make-release.sh --out /tmp/tt-release --version 1.0.0 \
  --title "Table Tennis for Linux 1.0.0" --notes-file /tmp/notes.txt

git tag -a linux-v1.0.0 -m "Table Tennis for Linux 1.0.0" && git push origin linux-v1.0.0
gh release create linux-v1.0.0 --repo ThisIsAkill/Table-Tennis-Rockstar-PC --latest \
  --title "Table Tennis for Linux 1.0.0" --notes-file /tmp/notes.txt \
  /tmp/tt-release/install-table-tennis-linux-x86_64.tar.gz \
  /tmp/tt-release/tt-toolchain-*.tar.zst /tmp/tt-release/tt-toolchain-sources-*.tar \
  /tmp/tt-release/tt-sdk-*.tar.zst \
  /tmp/tt-release/manifest.json /tmp/tt-release/manifest.json.sig
```

Run it from a clean checkout of the commit being released: the player package is `git archive
HEAD` (`--from-worktree` packs the working tree instead, for local tests only). The version is
the plain number (`1.0.0`); it goes into the package's `VERSION`, the manifest and, after
installing, `<install dir>/.tt-version`, which the game and the launcher compare against the
newest release.

Partial runs: `--only toolchain|sources|sdk|package|manifest` rebuilds one part; every run
rewrites and re-signs the manifest from whatever files are in `--out`. The toolchain and SDK do
not depend on the commit, so a release that only changes game-side code can reuse them
(copy the previous release's files into `--out` and run `--only package`). Upload a new SDK file
whenever `project/sdk-patches/` changes; a new toolchain (and its sources) whenever
`toolchain-slim-recipe.sh` changes.

## Key handling

- `--gen-key` creates an Ed25519 key with `openssl` at `~/.config/tt-recomp-release/key` (mode
  600, directory 700) and writes its public half to `scripts/install/release-public-key`, which
  is committed. It refuses to overwrite an existing key.
- The private key never enters the repository or a release. Keep an offline backup (for example
  an encrypted USB stick or a password manager's file attachment): without it no further update
  can be signed, and every existing install keeps trusting only the old public key.
- `make-release.sh` signs `manifest.json` with `openssl pkeyutl -sign -rawin` and checks that the
  key matches the committed public key before signing, then verifies the result.
- Changing keys: generate a new key on a new path (`--key`), commit the new public key, and
  publish a release signed with the **old** key whose package contains the new public key.
  Installs that update to it trust the new key from then on. If the old key is lost or leaked,
  existing installs cannot be moved to a new key automatically; players must download the new
  installer by hand.
- Verification: the installer binary checks signatures with Monocypher
  (`install-table-tennis --verify-manifest MANIFEST SIG PUBLIC_KEY`, exit 0 valid, 1 invalid);
  `scripts/install/tt_install.py verify-sig` is the same check in pure Python, used when the
  binary is not available. Both reject a manifest changed after signing.

## Sources

The toolchain file redistributes Ubuntu packages, some under the GPL and other licenses that
require offering source code. `make-release.sh` therefore builds `tt-toolchain-sources-<id>.tar`:
for every installed package whose `/usr/share/doc/<package>/copyright` names a copyleft
license (GPL, LGPL, MPL, EPL, CDDL), `apt-get source --download-only` fetches the source package
at the exact installed version (the recipe upgrades the base system first so every version is
still in the archive). `sources/INDEX.txt` lists them; any package the archive no longer has is
marked `MISSING` there and the build prints a warning. The toolchain's `/SOURCES.txt` points to
this file, to the Ubuntu archive, and to LLVM's source tarball (URL and SHA256) for the clang
build in `/opt/llvm`. Publish the sources file in the same release as the toolchain it belongs to,
and keep it as long as that toolchain is offered.

The installer binary and the SDK are built from this repository and the pinned ReXGlue SDK
source, which are public.
