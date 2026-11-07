# chrony

chronyd, the NTP implementation of the chrony project, vendored unmodified
in `src/` and built for ESP-IDF by `components/chrony`. The platform code
that chrony's `./configure` would select on a Unix host lives in
`components/chrony/port/` instead of being patched into `src/`:

- `sys_esp32.c`: system clock driver (adjtime-based, after `sys_macosx.c`)
- `tls_mbedtls.c`, `siv_mbedtls.c`: NTS-KE TLS and AES-SIV-CMAC on mbedTLS
- `chronyd_esp.c`: entry point in place of `main.c`; the app interface is
  `components/chrony/include/chronyd_esp.h`
- `nts_server_stubs.c`: the device is an NTS client only
- `include/`: `config.h` and POSIX headers newlib/lwIP lack

Source: https://chrony-project.org/releases/chrony-4.9.tar.gz
Signature: `chrony-4.9-tar-gz-asc.txt`, Miroslav Lichvar
(8F375C7E8D0EE125A3D3BD51537E2B76F7680DAC)
SHA-256: 4924c6f530105bcd5b9e9e33c48a2ae1bfd889222c8480bc41601110efc864d0
License: GNU General Public License version 2 only (`src/COPYING`)

Update with:

```sh
make update-chrony CHRONY_VERSION=<version>
```

The tarball is checked against its detached signature and unpacked into
`src/` unmodified. Review `components/chrony/CMakeLists.txt` (object list)
and `port/include/config.h` against the new release's `configure` and
`Makefile.in`, then update the version, hash and `CHRONY_VERSION` above.
