# bnio v0.2 ABI Compatibility Report

- **Release:** bnio v0.2 (package version 0.2.0)
- **Library:** `libbnio.so.0.2.0` (SONAME `libbnio.so.0`, development symlink `libbnio.so`)
- **Scope:** Linux x86_64, glibc systems, io_uring backend, shared-library build
  (`BUILD_SHARED_LIBS=ON`, hidden symbol visibility)
- **Supersedes:** the [bnio v0.1 report](https://github.com/haomingbai/bnio/blob/v0.1.1/docs/abi-compatibility.md), which stays in git history

## Overview

bnio is a C++20 asynchronous I/O library. Every operation is a lazy sender that
composes with the standard receiver pattern; the Linux backend runs on io_uring.
The v0.2 binary artifacts (DEB/RPM/TGZ) were built from bnio git commit
`24677a2d9e2824a72141122dac11690acc7d8653` in an `ubuntu:24.04` container with
GCC 13.3.0, CMake 3.28.3, Ninja 1.11.1, OpenSSL 3.0.13 and liburing 2.5; the
version string `0.2.0` was passed to the build as `-DBNIO_VERSION=0.2.0`.

Commits landed after `24677a2` are not part of this verification run. This report
documents the binary that was actually packaged and tested, and records the
distribution matrix on which that binary was verified.

## ABI stability

**The bnio ABI is not stable across releases.** v0.2 replaces the v0.1 baseline:

- Exported symbols, class layouts, and inline/template behavior may change in
  any release without SONAME preservation.
- The v0.2 shared object still carries SONAME **`libbnio.so.0`**, because the
  SONAME is derived from the major version and that is still 0. **An unchanged
  SONAME does not mean compatibility with v0.1** — a consumer linked against
  v0.1's `libbnio.so.0` resolves to the v0.2 object if both are present on the
  same system. Never install v0.1 and v0.2 side by side; rebuild all dependent
  code when upgrading.
- This release is a 0.1 → 0.2 version change, so no cross-version compatibility
  testing was done; the v0.1 report is reference material only.
- The only configuration covered here is the exact unit **bexec v0.1.0 +
  bnio v0.2.0**.

**Recommendation: build from source.** The most reliable way to consume bnio at
this stage is to compile bnio v0.2.0 together with bexec v0.1.0 as part of your
own build — via CMake `FetchContent` / `add_subdirectory`, or a source install of
both — with your own toolchain. The prebuilt DEB/RPM/TGZ artifacts are provided
for evaluation on the distributions listed below: they are usable across the
tested glibc/toolchain range, but they carry no ABI stability guarantee across
bnio releases. Whenever bnio or bexec is upgraded, rebuild all dependent code.

## Coupling with bexec v0.1 (read first)

**bnio v0.2 does not have a self-contained ABI.** Its public headers expose types
from the header-only library **bexec v0.1** directly in the public API:

- execution concepts — `bexec::scheduler`, `bexec::sender` — constrain the
  public customization-point interfaces (e.g. `include/bnio/io_context_cpo/concepts.h`);
- receiver CPOs — `bexec::set_value`, `bexec::set_stopped`, `bexec::get_env`,
  `bexec::get_stop_token` — appear in the signatures of the async operation
  states that consumers instantiate;
- consumers drive every bnio operation through `bexec::connect` / `bexec::start`.

Because bexec is header-only, all of its code is compiled into the consumer's own
translation units. Layout, ODR, and template-instantiation compatibility of any
bnio consumer therefore depends on the **exact bexec version** in use, not only
on the bnio binary.

The bnio v0.2 ABI is therefore defined as a single unit:

> **bexec v0.1.0 (commit `ddd9e9c5ec586ac8d6f221d6f3bce0e8fefd8ccd`) + bnio v0.2.0**
> (built at `24677a2`).

Compatibility rules for consumers:

- Consumers **must** build against bexec **v0.1.x**. The released packages were
  built and verified exclusively against bexec v0.1.0 (tag `v0.1.0`, installed
  from source to `/usr/local`, consumed via `BNIO_BEXEC_PROVIDER=FIND_PACKAGE`).
- Any **major or minor version change of bexec is an ABI change of bnio**, even
  if the bnio binary is untouched. Do not move bexec past v0.1.x without
  rebuilding and re-verifying bnio.
- bexec ships no binary package in the Debian/Ubuntu, Fedora, openSUSE or Arch
  repositories. The development packages declare the dependency
  (`libbnio-dev` → `bexec`, `bnio-devel` → `bexec`), so bexec has to be installed
  from source first.

## Binary identity

Identical in the DEB, RPM and TGZ artifacts:

| Property | Value |
|---|---|
| File name | `libbnio.so.0.2.0` |
| **SONAME** | **`libbnio.so.0`** |
| BuildID | `602bdfb9e10a75059059d24c706f94ab936dea5f` |
| Exported (dynamic, defined) symbols | 364 |
| NEEDED | `libssl.so.3`, `libcrypto.so.3`, `liburing.so.2`, `libstdc++.so.6`, `libc.so.6`, `ld-linux-x86-64.so.2` |

## Test methodology

The released packages were verified on an eight-distribution container matrix
(rootless `podman`, `--network=host --security-opt seccomp=unconfined` so that
io_uring is available). Each distribution ran in its own container, on its own
port range, and the containers were removed afterwards.

Per distribution, the procedure was:

1. Start a clean container from the official image.
2. Install the distribution's **native toolchain** (g++, cmake, make,
   pkg-config, the liburing development package, the OpenSSL development package).
3. Install bexec v0.1.0 from source to `/usr/local`.
4. Install the bnio package with the native package manager (DEB/RPM), or unpack
   the TGZ onto `/usr` where no native format applies.
5. Compile the example programs from the repository `examples/` tree against the
   **installed** bnio headers and shared library. The compiler and system headers
   come from the target distribution; bnio itself comes from the released binary.
6. Smoke-run six functional checks:
   - `timer_chain` — chained steady-timer waits;
   - `dns_lookup` — localhost name resolution;
   - `poll_fd` — descriptor polling;
   - `echo_server` — TCP echo round-trip;
   - `echo_server_sigint` — clean exit within 5 s of SIGINT;
   - `mini_curl` — HTTP fetch against a local server.

The same six checks as in the v0.1 matrix are used, so the results stay
comparable; `tcp_client`, `udp_echo`, `timer_cancel` and `udp_connected` are not
part of the matrix. Ubuntu 26.04 is covered for the first time and is the newest
environment in the set.

## Compatibility matrix

| Distribution | glibc | g++ | CMake | liburing | Package installation | Result |
|---|---|---|---|---|---|---|
| Ubuntu 24.04.4 LTS | 2.39 | 13.3.0 | 3.28.3 | 2.5 | `apt` runtime.deb + `dpkg --force-depends` dev.deb | **6/6 PASS** |
| Ubuntu 26.04 | 2.43 | 15.2.0 | 4.2.3 | 2.14 | same as Ubuntu 24.04 | **6/6 PASS** |
| Debian 13 (trixie) | 2.41 | 14.2.0 | 3.31.6 | 2.9 | same as Ubuntu | **6/6 PASS** |
| Fedora 43 | 2.42 | 15.3.1 | 3.31.11 | 2.9 | `dnf` runtime.rpm + `rpm --nodeps` dev.rpm | **6/6 PASS** |
| Rocky Linux 10.2 | 2.39 | 14.3.1 | 3.31.8 | 2.12 (CRB) | same as Fedora | **6/6 PASS** |
| openSUSE Leap 16.0 | 2.40 | 15.2.0 | 3.31.7 | 2.8 | `zypper` runtime.rpm + `rpm --nodeps --replacefiles` dev.rpm | **6/6 PASS** |
| Arch Linux (rolling) | 2.44 | 16.2.1 | 4.4.3 | 2.15 | TGZ unpacked onto `/usr` | **6/6 PASS** |
| Alpine 3.23 (musl) | musl 1.2.5 | 15.2.0 | 4.1.3 | 2.12 | TGZ onto `/usr` | **Expected failure** (see below) |

Every glibc distribution passes all six checks. The matrix spans GCC 13.3 →
16.2.1, glibc 2.39 → 2.44, CMake 3.28 → 4.4.3 and liburing 2.5 → 2.15 against the
single bnio v0.2 binary built with GCC 13.3 on glibc 2.39, so the released
artifacts are usable across that range. openSUSE Leap 16.0 and Arch Linux were
each verified a second time in an independent container with the same result.

## musl / Alpine (expected failure)

Alpine 3.23 uses musl libc and is **not** a supported target for the released
glibc-linked binaries. The Alpine run exists to confirm that conclusion and does
not count toward the pass rate:

1. **Library loading fails.** `ldd /usr/lib/libbnio.so.0.2.0` reports
   `Error loading shared library ld-linux-x86-64.so.2: No such file or directory
   (needed by /usr/lib/libbnio.so.0.2.0)` and exits 127 — the musl loader cannot
   resolve a glibc-linked shared object.
2. **Example compilation fails.** The link step produces 240 `undefined
   reference` errors against versioned glibc/libstdc++ symbols
   (`connect@GLIBC_2.2.5`, `__errno_location@GLIBC_2.2.5`,
   `__cxa_guard_acquire@CXXABI_1.3`, `std::__throw_logic_error@GLIBCXX_3.4`, …).
   musl ships none of these versioned definitions, so the link cannot be
   satisfied.
3. **Conclusion.** The DEB/RPM/TGZ artifacts of bnio v0.2 apply to glibc-based
   distributions only. musl consumers need to build bnio from source with their
   own toolchain; that configuration is outside the ABI baseline defined here.

One unsupported side experiment, recorded for information only: with the
third-party `gcompat` shim installed and `-Wl,--allow-shlib-undefined` passed to
the linker, all five examples link and the same six checks pass on Alpine 3.23.
This shows only that the bnio code itself is portable — it needs a best-effort
glibc emulation layer and the linker check switched off, neither of which is
within the support contract.
