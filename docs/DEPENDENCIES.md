# Dependency audit

Audit date **2026-11-20**. Scope is the C application and tests, the Windows and Ubuntu link dependencies, and the OpenSSH plus llama.cpp deployment actually used here. The link inventories are observations of these builds. Other operating-system images, package variants, dynamically loaded OS modules, and remote model providers can differ.

## Source and build

| Component | Audit result |
| --- | --- |
| Neutrino and its tests | All application, gateway, terminal, world, and test sources build as C23. CMake declares only `LANGUAGES C`. Release builds use `-O3`. There is no package-manager lockfile, build-time download, script interpreter, module loader, or subprocess launcher in the application. |
| cJSON | The only vendored library. MIT, v1.7.19, upstream commit `c859b25`. The local changes make parse error state thread-local and cap parsing and deep copying at 128 levels. The vendor license is retained. [Release and fixes](https://github.com/DaveGamble/cJSON/releases/tag/v1.7.19) · [License](../third_party/cjson/LICENSE) |
| cJSON security review | The shipped version includes the `cJSON_SetValuestring` fix for CVE-2024-31755. CVE-2026-67215 and CVE-2026-67216 concern the JSON Patch utilities. Those source files and APIs are absent from the build. Parsed workspace, gateway and provider JSON are still bounded by application size and nesting limits. [Release history](https://github.com/DaveGamble/cJSON/releases) · [CVE-2026-67215](https://github.com/advisories/GHSA-5q3m-r3x7-8phg) · [CVE-2026-67216](https://nvd.nist.gov/vuln/detail/CVE-2026-67216) |
| Build tooling | CMake 3.25 or later and a final C23 compiler. Verified with CMake 4.2.3 and GCC 15.2 on Ubuntu 26.04.1 and the Clang C driver in Zig 0.15.2 on Windows build 26200. CTest runs the C regression test. Zig is used only as a compiler toolchain. |
| Upstream inspirations | Neko Agent `0c5bc71`, rp-core, and Cherenkov/Xenon were read to identify workflows. The implementation in Neutrino is C. There is no Neko Agent, Node, pnpm, browser client, SQLite module, JS plugin, TTS service, or paid provider SDK in its build or runtime. [Neko Agent source and license](https://github.com/GameDevFox/neko-agent/tree/0c5bc71) |

Current vendored source SHA-256 values:

- `cJSON.c` — `7fd89de5322c2e434ab66a402f20c33d5ace9852b1b8c93434da2af73d472e6d`
- `cJSON.h` — `c4146e9c6794a7d6610d967ff58dca8293a0ad41eccc52642aacb82dac225bce`

## Neutrino runtime

- **Windows.** The C23 executable imports `WINHTTP.dll`, `WS2_32.dll`, `KERNEL32.dll`, and the OS Universal CRT contracts for conversion, environment, heap, private, runtime, stdio, strings, time, utility, and math. The 0.6.0 imports were checked with `objdump -p`. Worker threads use the native CRT and Windows synchronization APIs. TLS and system DLL servicing follow Windows Update. [WinHTTP](https://learn.microsoft.com/en-us/windows/win32/winhttp/about-winhttp) · [Winsock](https://learn.microsoft.com/en-us/windows/win32/winsock/windows-sockets-start-page-2)
- **Ubuntu/WSL.** The application's direct ELF dependencies were `libc.so.6` and `libcurl.so.4`. POSIX threads and the C math library are declared in the build. They do not appear as separate ELF dependencies on the measured glibc build. The measured curl package was Ubuntu `8.18.0-1ubuntu2.8`, which includes the vendor fix for CVE-2026-92392. Neutrino now uses HTTP/1.1 on the libcurl path, limits requests to HTTP(S), disables automatic HTTP and proxy authentication, rejects redirects, and keeps certificate verification enabled. [Ubuntu curl security notice](https://ubuntu.com/security/notices/USN-8927-1) · [CVE-2026-92392 package status](https://ubuntu.com/security/CVE-2026-92392)
- **Other POSIX hosts.** The documented build uses system libcurl, threads and C library. macOS was not available for a binary inventory or SSH acceptance test. Re-run the platform loader inspection using `otool -L` or `ldd` before deploying another build.

## SSH and inference deployment

| Component | Observed deployment |
| --- | --- |
| OpenSSH | Ubuntu client and daemon `1:10.2p1-2ubuntu3.8`, the vendor's security release for USN-8928-1. The local verification listener ran as the workspace owner on loopback, with public-key authentication, PTY access and a fixed C command. It denied shell commands, SFTP and forwarding. No SSH library is linked into Neutrino. [Ubuntu security notice](https://ubuntu.com/security/notices/USN-8928-1) · [OpenSSH configuration](https://man.openbsd.org/sshd_config.5) |
| TCP wrappers | The Ubuntu daemon's `sshd-session` helper loads `libwrap.so.0`. The isolated deployment used Ubuntu package `libwrap0` `7.6.q-36build2`. Installing distro-managed OpenSSH is the regular deployment path. [Ubuntu package](https://packages.ubuntu.com/resolute/libwrap0) |
| Model provider | External llama.cpp b9789 (`b3ce5cedf`), CPU build, on loopback with two separate slots. The provider has its own C++/OpenMP runtime. It is a separate service and is not linked into Neutrino. This deployment uses one pre-existing local model and leaves the RPC backend disabled. The model's local SHA-256 identifies the file used and does not establish publisher provenance. [Release](https://github.com/ggml-org/llama.cpp/releases/tag/b9789) · [Provider security advisories and policy](https://github.com/ggml-org/llama.cpp/security) |
| Local model | `Qwen3.5-0.8B-UD-Q4_K_XL.gguf`, SHA-256 `3177ebd67afe4438374da19e690bc1b98756f7e0fea9240e1be404336156a7b5`. It is outside the project tree. |
| TLS | Ubuntu OpenSSL libraries `3.5.5-1ubuntu3.7` were observed across curl and the deployment. [Ubuntu package security changelog](https://lists.ubuntu.com/archives/resolute-changes/2026-October/030555.html) |

For the isolated test instance, the Ubuntu package archive hashes were:

- `openssh-server_1:10.2p1-2ubuntu3.8_amd64.deb` — `d14910ed045bd4a2a4c9a4eeb6b3edf08d527fb4cdee5cf527fe66b8bb364c31`
- `libwrap0_7.6.q-36build2_amd64.deb` — `a3784ca083c626c3ae41cdd32d4c5898554650dc77d2b54dbb1197a38198adf8`

## Observed Ubuntu transitive package inventory

Union of packages owning ELF libraries resolved for Neutrino, `sshd`, `sshd-session`, `sshd-auth`, `ssh`, `ssh-keygen` and the model server. The glibc entry includes the loader, resolver, and system C/math support. The external llama.cpp libraries resolved from the b9789 distribution were `libllama-server-impl`, `libllama-common`, `libllama`, `libmtmd`, `libggml` and `libggml-base`, with its CPU backend loaded by the provider. `libwrap0` is listed separately above. Libraries in the union are not all invoked by Neutrino or by the enabled SSH features.

```text
libaudit1          1:4.1.2-1build1
libbrotli1         1.2.0-3ubuntu0.1
libc6              2.43-2ubuntu2.7
libcap-ng0         0.8.5-4build5
libcom-err2        1.47.2-3ubuntu4
libcrypt1          1:4.5.1-1
libcurl4t64        8.18.0-1ubuntu2.8
libffi8            3.5.2-4
libgcc-s1          16-20260322-1ubuntu1
libgmp10           2:6.3.0+dfsg-5ubuntu2
libgnutls30t64     3.8.12-2ubuntu1.1
libgomp1           16-20260322-1ubuntu1
libgssapi-krb5-2   1.22.1-2ubuntu4.1
libhogweed6t64     3.10.2-1
libidn2-0          2.3.8-4build1
libk5crypto3       1.22.1-2ubuntu4.1
libkeyutils1       1.6.3-6ubuntu3
libkrb5-3          1.22.1-2ubuntu4.1
libkrb5support0    1.22.1-2ubuntu4.1
libldap2           2.6.10+dfsg-1ubuntu5
libnettle8t64      3.10.2-1
libnghttp2-14      1.68.0-2ubuntu0.2
libp11-kit0        0.26.2-2
libpam0g           1.7.0-5ubuntu3.2
libpcre2-8-0       10.46-1ubuntu0.1
libpsl5t64         0.21.2-1.1build2
librtmp1          2.4+20151223.gitfa8646d.1-3
libsasl2-2         2.1.28+dfsg1-9ubuntu3
libselinux1        3.9-4build1
libssh2-1t64       1.11.1-1ubuntu0.26.04.4
libssl3t64         3.5.5-1ubuntu3.7
libstdc++6         16-20260322-1ubuntu1
libtasn1-6         4.21.0-2
libunistring5      1.3-2build1
libzstd1           1.5.7+dfsg-3
zlib1g             1:1.3.dfsg+really1.3.1-1ubuntu3.2
```

`libaudit1` had a newer candidate (`1:4.1.2-1ubuntu0.1`) in `resolute-updates` when checked. The audit did not perform a system upgrade or certify that all platform libraries are free of vulnerabilities. Re-check package updates and advisories before expanding the loopback listener to another interface. [Ubuntu Security Notices](https://ubuntu.com/security/notices)
