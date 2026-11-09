# UniXNU on Lenovo TB375FC

Experimental **MT6897 / MT8792 (Dimensity 8300 family)** device support for
[UniXNU](https://github.com/UniXNU/UniXNU), targeting the iPadOS 27.0.1 / 24A446
profile (xnu-12377, arm64e). This is **not a usable iPadOS distribution**.

## Status

- UEFI hands off to XNU on the physical TB375FC.
- The kernel publishes its read-only UFS window and mounts a genuine 24A446
  RestoreRamDisk APFS volume on the physical device (not the full OS volume).
- Independent console/msgbuf tails and panic text have been recovered from UFS;
  a watchdog restores the development device after a bounded experiment.
- Owner-root authentication has passed physical positive and negative tests:
  a valid signed root is accepted; a wrong signed UUID, a corrupted physical
  block and a damaged RSA signature are rejected. Apple root-auth results are
  not forged. See the source/kernel baseline below.
- With unchanged same-build v2 Static/System trust-cache modules, the kernel
  reports `launchd[1] exec process signature accepted`. This is **not proof of
  EL0 execution, a running launchd, or a usable/full iPadOS system**.
- Display, touch, keys and the other drivers still need porting on this route.
  Earlier 16.5 experiments are a separate fallback, not this repository's status.

This repository contains device sources, patches, build automation and the
embedded MT6897 SoC profile. It does not contain an IPSW, SDK, root filesystem,
extracted Apple kexts, private key, device backup, or a ready-to-flash image.

## Build

Use the real Apple clang, not merely an LLVM build accepting the arm64e target.
The XNU headers require the fourth `__ptrauth` qualifier argument and its
`authenticates-null-values` semantics. No qualifier-stripping fallback is applied.

The workflow pins UniXNU to `5dc00d201496a1e7a72d1f5db23f041eb94ace1c` and
uses **Xcode 26.6 on the standard macos-26 GitHub-hosted runner**. It is manually
dispatched; it does not run untrusted pull requests with credentials.

For a local macOS build (after installing the host tools listed in the workflow):

```sh
git clone https://github.com/billyham07/UniXNU-tb375fc.git device
git clone https://github.com/UniXNU/UniXNU.git framework
git -C framework checkout "$(cat device/ci/framework-revision)"
cd framework
git apply ../device/patches/framework/prepare-xnu-source.patch
export UNIXNU_DEVICE="$(cd ../device && pwd)"
./unixnu defconfig bootstrap_defconfig
./unixnu build kernel
```

Upstream source and firmware downloads are resolved by UniXNU's public source
definitions and the device lock file. Users must independently obtain the
necessary rights for proprietary inputs. See [NOTICE](NOTICE.md).

## Public CI and operator-only artifacts

Start **TB375FC macOS build** from the Actions tab or:

```sh
gh workflow run macos.yml -R billyham07/UniXNU-tb375fc
```

CI publishes only text logs and **recipient-encrypted** kernel output.
It does not publish plaintext mixed kernels, firmware, SDKs, IPSW members or
build caches. The checked-in certificate is public; its private key stays on
the operator's build host and is never sent to GitHub. Encryption uses
OpenSSL CMS AuthEnvelopedData, AES-256-GCM and RSA-OAEP-SHA256.

After reviewing the source commit and successful run, the operator checks out
that exact commit and receives the build into a **new directory**:

```sh
python3 ci/receive-build.py RUN_ID FULL_SOURCE_SHA NEW_OUTPUT_DIRECTORY \
  --key /private/location/recipient.key
```

The receiver checks the run, commit, ciphertext, authenticated decryption,
archive paths, encrypted build identity and file digests. It **never flashes
a device**. Fork operators must replace `ci/recipient.pem` with their own public
certificate and keep the matching private key off GitHub.

The existing private build repository and its historical artifacts are not
made public by this project.

## Owner-verified read-only root

The tested kernel is source `59f47a8b346ed8d62d030accb08d53dee1f16193`,
[encrypted build 42588243189](https://github.com/billyham07/UniXNU-tb375fc/actions/runs/42588243189).
Device records on 2026-11-09:

| Record | Result |
| --- | --- |
| `090705Z` | RSA/table verified, APFS mounted; Apple auth error 80 retained, mounted/selected device 1:2, read-only, owner accepted |
| `091331Z` | Valid signature and image hashes but wrong signed UUID: owner rejected |
| `091729Z` | One byte changed in physical block 0: integrity failure, no root publication; original fixture restored |
| `092948Z` | Original v2 Static/System trust modules: process signature accepted, no panic recorded; EL0 not yet observed |
| `094112Z` | One RSA signature byte changed: policy handoff rejected before publication |

All five bounded rounds returned to Android and restored boot_a/misc. They use
an isolated 232 MiB recovery fixture, not the production OS/SystemOS layout.
Host checks additionally verified all 59392 original blocks and ten actual
C/RSA positive/negative cases. Neither compilation nor these checks establish
full-system or UI support.

`tbroot=1` requires a complete `RootPolicies` firmware handoff. The immutable
copy is authenticated with the compiled owner public key before publication;
every 4 KiB read is verified before returning data. One integrity failure
latches the adapter closed. Root acceptance requires the actual mounted BSD
device to equal the selected root device, exactly one read-only IOMedia,
its real APFS UUID and a provider chain reaching this verified adapter.
Missing policies and header-only policies cannot authorize storage.

The owner signing key is **separate from the CI artifact encryption key**.
Operators using another key must replace `package/root-key/owner-public.pem`
and rebuild; keep the private key and firmware-derived policy on the operator
host. Never put either in CI. Using the pinned framework after fetching its
VerifiedRoot component:

```sh
python3 kits/VerifiedRoot/tools/manifest.py \
  --image "$ROOT_APFS" --private-key "$OWNER_PRIVATE_KEY" \
  --public-key "$UNIXNU_DEVICE/package/root-key/owner-public.pem" \
  --volume-uuid "$ACTUAL_APFS_UUID" --generation 1 \
  --output "$PRIVATE_POLICY_DIR"
python3 - "$PRIVATE_POLICY_DIR" <<'PY'
from pathlib import Path
import struct, sys
p = Path(sys.argv[1])
b = (p / 'manifest.bin').read_bytes()
(p / 'root-policies.bin').write_bytes(
    struct.pack('<8sQQQQQ', b'VROOTB01', 1, 1, 48 + len(b), 48, len(b)) + b)
PY
```

Set `root_policies` in the isolated board's existing `[payload]` table to that
private bundle path, and its root-match UUID to the real APFS volume UUID.
`tbroot_lba`/`tbroot_blocks` must describe the provisioned read-only window and
match the signed image size. The compiled minimum generation is 1; no RPMB or
persistent anti-rollback mechanism is claimed. This is an independent owner
chain, not Apple personalization or a hardware secure-boot claim.

Use original, pinned trust-cache inputs for execution policy; do not disable
AMFI or re-sign binaries to conceal a failure. The original recovery trust
cache is v1 and the current DarwinBoot loader accepts v2 only. The reported
signature-progress test used the two **unchanged original v2** Static/System
modules after checking their same-build BuildManifest digests, not a converted
recovery cache or owner-added hashes.

## Hardware safety

This is a board-specific research port. Its UFS constants refer to one device's
verified partition layout; they are not a generic MT6897 storage map. The
recorder writes only its two reserved diagnostic slots, and the experimental
root adapter is read-only and bounds its window. Do not flash a generic or
unreviewed build on another device. Back up and verify your own recovery path.

## License and attribution

TB375FC additions use **BSD-2-Clause-Patent** unless otherwise marked. Preserve
upstream notices and the licenses of the source files targeted by patches.
See [LICENSE](LICENSE), [NOTICE](NOTICE.md) and the pinned source definitions.
