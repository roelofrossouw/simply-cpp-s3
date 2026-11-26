# simply-cpp-s3

S3 for [simply-cpp](https://github.com/roelofrossouw/simply-cpp): AWS S3 and S3-compatible storage (MinIO, Ceph, ...)
from the program itself, without the AWS CLI. C++20 on libcurl and OpenSSL's libcrypto.

- **AWS Signature Version 4**, step by step (checked against AWS's published examples), usable for any service.
- **Uploads of any length from a pipe** (`zfs send`, `pg_basebackup -Ft -D -`): one PUT when it fits in a part, else a
  multipart upload that reads the next part while sending this one, tries each part again on its own, doubles the
  part size every 1,000 parts (5 TB objects from 5 MiB parts), and is aborted when anything fails, so no parts are
  left to be billed.
- **Checksums S3 checks and keeps**: CRC32 (default), CRC32C or SHA-256, besides SigV4's signed SHA-256 of every body.
- **Storage classes**: STANDARD, STANDARD_IA, ONEZONE_IA, INTELLIGENT_TIERING, GLACIER_IR, GLACIER, DEEP_ARCHIVE.
- **Reading**: streamed GET (going on from where it broke off, for the same ETag), ranges, HEAD with metadata,
  checksums and restore status, ListObjectsV2 with paging and folders.
- **Archives**: RestoreObject (days, Bulk/Standard/Expedited) and its progress from HEAD.
- **Checking a copy**: GetObjectAttributes gives each part's size and checksum (also for archived objects), so a
  restore test can read single parts by range and check them, and `composite_checksum()` rebuilds the object's checksum.
- **Credentials** from a settings file and its `.d/` drop-ins, the `AWS_*` variables, or **STS AssumeRole**
  (short-lived credentials narrowed by a session policy, for handing one job upload-only access).
- **Bucket settings** (`bucket_config`: versioning, object lock, lifecycle, encryption, ...) as S3's XML, and
  `sc::s3::query()` for other AWS query-protocol services (CloudWatch's GetMetricData for bucket sizes).
- **A rate limit** each way, so a backup leaves room on a shared link.
- **sc-s3**, a program for scripts: `put`, `get`, `ls`, `head`, `restore`, `rm`, `uploads`, `abort`.

## Example

```cpp
#include <sc/s3/s3.h>

int main() {
    using namespace sc::s3::data;
    auto settings = sc::s3::settings::load();            // /etc/sc-s3/sc-s3.conf, then AWS_* variables
    settings.rate_limit = MiB(20);                       // a second, each way
    sc::s3::client s3{settings};

    sc::s3::upload_options options;
    options.kind = sc::s3::storage_class::deep_archive;
    const auto stored = s3.upload("backups", "db/base.tar", std::cin, options);   // pg_basebackup -Ft -D - | ...
    std::cout << stored.size.in<GiB>() << " in " << stored.parts << " parts\n";

    for (const auto &object: s3.list("backups", {.prefix = "db/"}).objects) std::cout << object.key << '\n';
    if (const auto info = s3.head("backups", "db/base.tar"); info && sc::s3::archived(info->kind) && !info->restore.available()) {
        s3.restore("backups", "db/base.tar", std::chrono::days{7}, sc::s3::restore_tier::bulk);
    }
}
```

## The program

```sh
zfs send pool/data@today | sc-s3 put s3://backups/hv/data@today --storage-class DEEP_ARCHIVE --rate-limit 20M --progress
sc-s3 ls s3://backups/hv/
sc-s3 head s3://backups/hv/data@today
sc-s3 restore s3://backups/hv/data@today --days 7 --tier bulk
sc-s3 get s3://backups/hv/data@today | zfs receive pool/restored
```

Settings come from `--config` (default `/etc/sc-s3/sc-s3.conf` when it's there, plus `sc-s3.conf.d/`), then
`AWS_REGION`, `AWS_ENDPOINT_URL`, `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY` and `AWS_SESSION_TOKEN`, then the
options. `--role-arn` assumes a role first. It exits 1 when S3 refuses, 2 for wrong use.
`put --json` prints the result as one line of JSON (bucket, key, size, parts, etag, checksum, seconds) for scripts. On
SIGINT, SIGTERM, SIGHUP or SIGPIPE (its caller gone) an upload stops before its next part and is aborted, so no parts
are left to be billed. `put --commit-if FILE` stores the object only if FILE holds 0 once the input has ended, so a
producer that fails part way never leaves a truncated object: `{ pg_basebackup -Ft -D - | zstd; echo $? > status; } | sc-s3
put s3://... --commit-if status`.

```ini
# /etc/sc-s3/sc-s3.conf (chmod 600 when it holds a key)
region = af-south-1
rate_limit = 20M
# endpoint = http://127.0.0.1:9000
# path_style = true
```

## Install

```sh
sudo apt -y install simply-cpp-s3-dev     # the library; simply-cpp-s3-tools has the sc-s3 program
```

CMake: `find_package(sc-s3 CONFIG REQUIRED)`, target `sc::sc-s3` or `sc::sc-s3-shared`.

## Tests

`ctest` runs the unit tests (signing against AWS's examples, sizes, checksums, XML, settings). The client's tests
run against an S3-compatible server when `SC_S3_TEST_ENDPOINT` names one, with `AWS_ACCESS_KEY_ID` and
`AWS_SECRET_ACCESS_KEY` for it; every key goes under `test/<pid>/` and is deleted again.

## Licence

Apache-2.0 (see LICENSE and NOTICE). Uses libcurl (curl licence) and OpenSSL (Apache-2.0) from the system.
