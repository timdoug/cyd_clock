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

`upstream/` holds a fix found in chrony's own code during the port, written
up for the chrony-dev list and not yet sent.

Source: https://gitlab.com/chrony/chrony.git, commit
c97d8083cf9a9edc64d53236565916d326d8a109 on main (`src/version.txt`:
4.9-10-gc97d808): the 4.9 release plus the removal of leap second support.
Commits are not signed; the 4.9 tag that the commit builds on is, by Miroslav
Lichvar (8F375C7E8D0EE125A3D3BD51537E2B76F7680DAC).
License: GNU General Public License version 2 only (`src/COPYING`)

Update with:

```sh
make update-chrony CHRONY_REF=<commit or tag>
```

The commit is exported with `git archive` into `src/` unmodified, after the
signature on the nearest release tag behind it is checked, and `version.txt`
is written from `git describe` (it sets the version chronyd reports). A git
export lacks the generated files of a release tarball (man pages,
`getdate.c`), none of which the firmware builds. Review
`components/chrony/CMakeLists.txt` (object list) and `port/include/config.h`
against the new source's `configure` and `Makefile.in`, then update the
commit above and `CHRONY_REF` in the Makefile.
