# Provenance of `pyrtklib5/rtksrc`

This directory is a vendored copy of the `src/` tree of
<https://github.com/rtklibexplorer/RTKLIB> ("demo5"), plus local deviations.
Every bump replaces the directory wholesale and re-applies the local patch series
(`git format-patch` of the commits marked `[rtksrc]` on this branch), so the
list below has to stay exact.

| Milestone | Upstream commit | Upstream date | Upstream `VER_RTKLIB` / `PATCH_LEVEL` |
|---|---|---|---|
| 1 (this branch, from IPNL-POLYU `05845e3`) | `e247fe1d` | 2025-07-20 | `"demo5"` / `"b34L"` (stale strings upstream, fixed later) |
| 2 (planned) | `06e86442` | 2026-08-31 | `"EX"` / `"2.5.1"` |

Verified with `git archive e247fe1d src | diff -rq` against this tree: every
computational `.c` (`rtkpos.c`, `rtkcmn.c` apart from the line below, `ephemeris.c`,
`preceph.c`, `rinex.c`, `rtcm3.c`, `lambda.c`, `pntpos.c`, `ppp.c`, ...) is
byte-identical to upstream.

## Local deviations inherited from IPNL-POLYU/pyrtklib_demo5 (portability only)

- `rtklib.h`: `VER_RTKLIB "EX"`, `PATCH_LEVEL "2.5.0"` (upstream at `e247fe1d` still
  said `"demo5"`/`"b34L"`); `THREADLOCAL __declspec(thread)` for MSVC (upstream has
  the misspelt `__declspec(__thread)`). `gen_rtk.py` hard-codes the same two
  strings as `m.attr(...)`, so the Python-visible version does **not** come from
  the header. Do not trust these strings for provenance: use the git SHA of this
  repository (exported as `GIT_SHA` by the module).
- `#define _POSIX_C_SOURCE 200809L` instead of `199506` in `rtkcmn.c`, `download.c`,
  `options.c`, `streamsvr.c`, `rcv/nvs.c`, `rcv/skytraq.c`, `rcv/ublox.c`; the line is
  commented out in `stream.c`.
- `CMakeLists.txt` and `src.pro` replaced by the bindings' own build files;
  `rcv/CMakeLists.txt` added. `rcv/comnav.c` and `rcv/tersus.c` are not vendored.
- `../CMakeLists.txt` defines `-DWIN_DLL` on every platform (harmless with GCC:
  `rtkcmn.c` tests `defined(WIN_DLL) || defined(DLL)`).
- The `note` file of the upstream bindings asks for `gcc-12` and two hand edits
  (`rtkcmn.c` `WIN_DLL -> DLL`, `rtklib.h:521` win32 define). Neither edit is applied
  to this tree and neither was needed: the tree builds unmodified with
  gcc 11.3 (Ubuntu 22.04), 8 warnings, all upstream (`download.c`, `gis.c`,
  `rtkcmn.c` format truncation, `stream.c` `__USE_MISC` redefinition).

## Local deviations added by the vrsgen port

Recorded commit by commit on this branch; each `[rtksrc]` commit touches the
smallest possible set of upstream lines so the series re-applies with `git am -3`
on the next bump. See `docs/migration_demo5_journal.md` in vrsgen for the gates.
