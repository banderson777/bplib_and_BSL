# Local integration modifications

This BPLib tree contains modifications maintained by banderson777 for
https://github.com/banderson777/bpsec-bplib-integration .
Original upstream: https://github.com/nasa/bplib
Upstream base commit: `c5fb8358dd99149aa7ba463ed416d76049521a3a`

Original copyright/license notices are retained. Modified file notices and this
manifest identify local work; commit history records the exact changes. Detailed
fix reviews, regression tests and validation evidence live in the main integration
repository's docs/ directory. This branch supports the native integration lab.

## Modified and added source/configuration

- `CMakeLists.txt`
- `bpa/ct/inc/bplib_ct.h`
- `bpa/ct/src/bplib_ct.c`
- `bpa/ct/src/bplib_ct_ccs.c`
- `bpa/ct/src/bplib_ct_ccs.h`
- `bpa/ct/src/bplib_ct_db.c`
- `bpa/ct/src/bplib_ct_db.h`
- `bpa/ct/unit-test/bplib_ct_test.c`
- `bpa/stor/inc/bplib_stor.h`
- `bpa/stor/inc/bplib_stor_sql.h`
- `bpa/stor/src/bplib_stor.c`
- `bpa/stor/src/bplib_stor_sql.c`
- `bpa/stor/src/bplib_stor_sql_cust.c`
- `bpa/stor/src/bplib_stor_sql_cust.h`
- `bpa/stor/src/bplib_stor_sql_load.c`
- `bpa/stor/src/bplib_stor_sql_load.h`
- `bpa/stor/src/bplib_stor_sql_store.c`
- `bpa/stor/src/bplib_stor_sql_store.h`
- `bpa/stor/unit-test/stubs/bplib_stor_stubs.c`
- `ci/cbor/src/bplib_cbor_decode_canonical.c`
- `ci/cbor/src/bplib_cbor_decode_types.c`
- `ci/cbor/src/bplib_cbor_encode_bundle.c`
- `ci/cbor/src/bplib_cbor_encode_payload.c`
- `ci/crc/src/bplib_crc.c`
- `ci/mem/inc/bplib_mem.h`
- `ci/mem/src/bplib_mem.c`
- `cla/src/bplib_cla.c`
- `inc/bplib_cfg.h`
- `.gitignore`

`LOCAL_CHANGES.md` is also added for attribution. Source changes were reviewed and
tested in the integration project; recorded historical validations refer to their
pinned source snapshots. Publication adds attribution notices and Git packaging
without changing the tested program logic.
