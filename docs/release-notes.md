# Shipping a working Windows package from `.github/workflows/release.yml`

Audit of the release workflow against what `cfspeed.exe` actually needs at
runtime. Everything below is an ordered checklist: do them in sequence, because
later steps depend on earlier ones (the CA bundle must exist before the archive
is built, the archive must exist before it is checksummed, and so on).

Line references are to `release.yml` as it stands today.

---

## 0. The current state, in one paragraph

The Windows leg cannot produce an artifact at all. It does not merely ship the
wrong DLLs — it ships nothing, and the packaging step fails outright, which
takes the entire release down with it (see §7). The `cfspeed-ca.pem` CA bundle
that `src/net.c` requires is never fetched by either workflow.

---

## 1. TRIGGER — already correct, verify only

```yaml
on:
  push:
    tags: ["v*"]
```

`v*` is a glob filter, and `v0.1.0` matches it. A plain annotated or lightweight
`git push origin v0.1.0` **does** trigger the Release workflow. There is no
silent failure here, and no change is required.

Do **not** "fix" this by adding `branches:` alongside `tags:` while a `push` has
both keys — a `push` carrying both filters requires the ref to satisfy *both*.
A tag push carries no branch, so adding `branches:` under the same `push:`
would break the trigger. If CI is ever merged into this file, use two separate
`push` entries or rely on `ci.yml`'s `branches: [main]`.

Note that `ci.yml` is `push: branches: [main]` + `pull_request`, so a tag push
runs **no** CI. The release workflow is the only validation a tag gets.

---

## 2. BUILD — delete the dead DLL loop

Lines 56-60 copy DLLs into the workspace root:

```bash
for dll in $(ldd cfspeed.exe | grep -o '[A-Za-z0-9_-]*\.dll'); do
  src=$(ldd cfspeed.exe | grep -o "[^ ]*$dll" | head -1)
  [ -f "$src" ] && cp "$src" .
done
```

This copies **nothing**, and it is not worth repairing — step 4 replaces it.
The bug, verified against real `ldd` output:

```
        libcurl-4.dll => /mingw64/bin/libcurl-4.dll
```

`grep -o` returns the **leftmost** match. `[^ ]*libcurl-4.dll` first matches at
the DLL name on the left of `=>`, yielding the bare string `libcurl-4.dll`;
only the *second* `-o` match is the path `/mingw64/bin/libcurl-4.dll`. `head -1`
keeps the first, so `src` is a bare relative name, and `[ -f "libcurl-4.dll" ]`
is false in the workspace root. (The outer `grep -o` also yields each name twice,
once per side of `=>`.)

Reduce the build step to:

```yaml
      - name: Build (Windows)
        if: matrix.os == 'windows-latest'
        shell: msys2 {0}
        run: |
          export PATH="/mingw64/bin:$PATH"
          make
          strip cfspeed.exe || true
```

Keep `strip ... || true`. `strip` failing is cosmetic; masking it is correct.

---

## 3. FETCH THE CA BUNDLE — new step, must run before packaging

`src/net.c` `use_ca_bundle()` builds the path from `GetModuleFileNameA()` and
probes for exactly `cfspeed-ca.pem` **in the same directory as the executable**.
It is not optional: MSYS2's libcurl is GnuTLS-backed, GnuTLS reads trust
anchors from a file, and Windows has none on disk. Without this file every
request fails `CURLE_PEER_FAILED_VERIFICATION` (curl 60) and `cfspeed` measures
nothing. `CURLSSLOPT_NATIVE_CA` does not rescue this — it is honoured only by
schannel builds.

Neither workflow mentions the file today.

Insert immediately after the Build step, in the Windows leg:

```yaml
      - name: Fetch CA bundle
        if: matrix.os == 'windows-latest'
        shell: msys2 {0}
        run: bash scripts/fetch-ca.sh cfspeed-ca.pem
```

`scripts/fetch-ca.sh` defaults its output to `./cfspeed-ca.pem`, which is the
exact name `net.c` probes for, so the argument is optional but explicit is
better. It validates before installing (non-empty, has `BEGIN CERTIFICATE` and
`END CERTIFICATE`) and exits non-zero on failure, so a truncated download fails
the job loudly instead of producing a silently untrusting binary. `curl` is
already on `PATH` — `mingw-w64-x86_64-curl` is in the MSYS2 install list and the
step exports `/mingw64/bin`.

Equivalent, if you prefer PowerShell over the MSYS2 shell:
`pwsh -File scripts/fetch-ca.ps1 -OutPath cfspeed-ca.pem`. Both scripts take
the same contract; use one, not both.

Do not `|| true` this step. A missing CA bundle must fail the release.

---

## 4. COLLECT THE TRANSITIVE DLL CLOSURE — replace lines 66-76

`ldd` reports a PE binary's **direct** imports only. It does not recurse.
`cfspeed.exe` imports roughly `libcurl-4.dll`, `libwinpthread-1.dll`,
`libws2_32.dll` and the CRT; everything below `libcurl-4.dll` — GnuTLS, nettle,
hogweed, gcrypt, gmp, gpg-error, iconv, intl, idn, idn2, psl, shishi, ssh2,
tasn1, unistring, zlib, zstd — is invisible to it. That is the ~19-DLL, ~31.6 MB
package. A single `ldd` pass can never produce it; **the tree must be walked to
a fixed point.**

The current Windows branch has three independent defects, all fatal:

1. `mv cfspeed.exe win/` runs **before** the collection loop, so
   `ldd cfspeed.exe` fails with `No such file or directory` and yields nothing.
   (Verified by replaying the step.)
2. The `src` regex resolves to the bare DLL name, not the path (§2).
3. Nothing is ever placed in `dist/`. `win/` receives only the exe, so
   `cp win/*.dll dist/` matches nothing (error swallowed by `|| true`), and then
   `zip cfspeed-windows-x86_64.zip cfspeed.exe *.dll` runs in an **empty**
   `dist/` with the exe stranded in `win/`. `zip` exits 12 ("nothing to do"),
   or 127 if `zip` is not on the image. Either way the step fails.

Replace the whole `if [ "${{ matrix.os }}" = "windows-latest" ]` block:

```bash
      - name: Package (Windows)
        if: matrix.os == 'windows-latest'
        shell: msys2 {0}
        run: |
          export PATH="/mingw64/bin:$PATH"
          stage="stage"
          rm -rf "$stage" dist
          mkdir -p "$stage" dist

          # Direct imports of $1, as absolute paths, minus Windows system
          # libraries (which must never be vendored) and API-set stubs.
          deps_of() {
            ldd "$1" 2>/dev/null \
              | sed -n 's/^[[:space:]]*\([^[:space:]]*\.dll\)[[:space:]]*=>[[:space:]]*\(\/[^[:space:]]*\)$/\2/p' \
              | grep -v -i '^/c/windows/' \
              | grep -v -i -E '^/(system32|syswow64)/' \
              | grep -v -i -E '^/(api-ms-win|ext-ms)-'
          }

          cp cfspeed.exe "$stage/"
          cp cfspeed-ca.pem "$stage/"

          # Breadth-first closure: copy each newly seen DLL, then ldd *that*
          # DLL too. Terminates when a pass finds nothing new.
          frontier="$stage/cfspeed.exe"
          while [ -n "$frontier" ]; do
            next=""
            for bin in $frontier; do
              for d in $(deps_of "$bin"); do
                base="$(basename "$d")"
                [ -f "$stage/$base" ] && continue
                cp -f "$d" "$stage/$base" || { echo "MISSING: $d" >&2; exit 1; }
                echo "$base  <-  $(basename "$bin")" >> "$stage/dlls.manifest"
                next="$next $stage/$base"
              done
            done
            frontier="$next"
          done

          ls -1 "$stage"
          ( cd "$stage" && zip -r "../dist/cfspeed-windows-x86_64.zip" . -x '.*' )
```

Three details that matter and are easy to get wrong:

- **The `sed` captures group 2, the path side of `=>`.** Do not "simplify" this
  back into a `grep -o` + `head -1`; that is the §2 bug.
- **System DLLs are filtered out.** `ldd` reports `KERNEL32.dll =>
  /c/WINDOWS/SYSTEM32/KERNEL32.DLL` and friends. Copying those into the package
  is wrong — it bloats the zip and risks shipping a mismatched system library.
- **`-x '.*'` keeps `dlls.manifest` out of the zip** only if you name it with a
  leading dot or exclude it explicitly. Prefer writing the manifest *outside*
  `$stage`, or exclude it by name, so the zip contains exactly `cfspeed.exe`,
  `cfspeed-ca.pem` and the DLLs — nothing else.

### 4a. Make `zip` exist

`zip` is not guaranteed on the Windows runner image. Add an install step before
packaging (default `msystem` in a non-`msys2` shell is MSYS, so this resolves
from the `msys` repository into `/usr/bin`):

```yaml
      - name: Ensure zip is available
        if: matrix.os == 'windows-latest'
        shell: msys2 {0}
        run: pacman -S --noconfirm --needed zip
```

Fallback if you would rather not add a package: build the archive with
`Compress-Archive -Path "$stage/*" -DestinationPath ...` from a `pwsh` step.
Do not let the step depend on `zip` being preinstalled.

### 4b. Guard the closure — turn a silent shallow glob into a loud failure

Add this after the walk, before zipping:

```bash
          n=$(ls -1 "$stage"/*.dll | wc -l)
          echo "collected $n DLLs"
          for req in libcurl-4.dll libgnutl-30.dll libnettle-8.dll \
                     libhogweed-6.dll libgcrypt-20.dll libgmp-10.dll \
                     libzstd.dll; do
            [ -f "$stage/$req" ] || { echo "MISSING REQUIRED: $req" >&2; exit 1; }
          done
          [ "$n" -ge 15 ] || { echo "only $n DLLs - closure looks shallow" >&2; exit 1; }
```

Exact `libgnutl-*` / `libnettle-*` version suffixes track the MSYS2 release, so
check the major set rather than pinning a suffix that will drift. The point is
that a regression to a shallow glob fails the release instead of shipping a zip
that dies on the user's machine.

---

## 5. CHECKSUM AFTER THE ARCHIVE — current order is already correct

```bash
(cd dist && sha256sum cfspeed-*.* > checksums.txt) || \
  (cd dist && shasum -a 256 cfspeed-*.* > checksums.txt)
```

This runs after the archive is built and `cfspeed-*.*` does not match
`checksums.txt`. **No ordering bug here** — leave the sequencing as is.

Fix the name collision instead. Every matrix leg writes a file called
`checksums.txt`, so the publish job globs three identically named assets and
GitHub ends up with `checksums.txt`, `checksums (1).txt`, `checksums (2).txt`.
Make each one unique:

```bash
sha256sum cfspeed-*.* > "dist/checksums-${{ matrix.asset }}.txt"
```

and update the publish glob to `staged/**/checksums-*.txt`.

---

## 6. UPLOAD AND PUBLISH — mostly fine

`actions/upload-artifact@v4` with `path: dist/` and `if-no-files-found: error`
is correct once `dist/` is actually populated. Keep it.

In `publish`, change `staged/**/checksums.txt` to `staged/**/checksums-*.txt`
to match §5. The `staged/**/` prefix is fine — `download-artifact@v4` nests each
artifact under `staged/<artifact-name>/`, and `**` spans that level.

Keep the single `publish` job behind `needs: build`. One publisher is correct.

---

## 7. Failure isolation — do not let Windows kill the Linux release

`publish` has `needs: build`, and `build` is one job covering all three matrix
legs. Any Windows leg failure skips `publish` entirely, so a Windows packaging
bug silently costs users the Linux and macOS artifacts too. After §2-§4 the
Windows leg will pass; if you want that decoupling anyway, split the matrix into
separate jobs and give `publish` `needs: [linux, macos, windows]`.

---

## 8. Do NOT wire `scripts/smoke.sh` into this workflow yet

`scripts/smoke.sh` gates on `"$bin" --json` and parses the result with a real
JSON parser. **`--json` is not implemented.** `main.c` only recognises
`--ping`/`-p`, `--download`/`-d` and `--upload`/`-u`; `--json` falls through to
the TUI, stdout will not be JSON, and the script fails the build. The MSYS2 leg
also has no `python3`, which the script needs. Leave it unwired until `--json`
exists.

---

## 9. Out of scope but worth knowing

The Linux and macOS tarballs ship a bare binary linked against the system
`libcurl` (`libcurl4-openssl-dev` on Ubuntu; `brew install curl` on macOS,
which is keg-only). Neither is vendored, so both are at the mercy of whatever
the user's machine has. Windows is the only leg that is *guaranteed* broken
without packaging, so it is the only one this checklist fixes.

---

## Ordered summary

| # | Action | Lines touched |
|---|---|---|
| 1 | Leave `on: push: tags: ["v*"]` alone | 3-5 (no change) |
| 2 | Strip the DLL loop from Build (Windows) | 56-60 (delete) |
| 3 | Add a Fetch CA bundle step | new, after 60 |
| 4 | Replace the packaging block with a BFS DLL closure | 66-76 (replace) |
| 5 | Add a `pacman -S zip` step | new, before packaging |
| 6 | Add the required-DLL / count guard | new, after the walk |
| 7 | Rename checksums per platform | 81-82 |
| 8 | Update the publish glob | 105 |