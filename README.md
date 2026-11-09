# UniXNU on Lenovo TB375FC

Experimental **MT6897 / MT8792 (Dimensity 8300 family)** device support for
[UniXNU](https://github.com/UniXNU/UniXNU), targeting the iPadOS 27.0.1 / 24A446
profile (xnu-12377, arm64e). This is **not a usable iPadOS distribution**.

## Status

- UEFI hands off to XNU on the physical TB375FC.
- The kernel reaches BSD initialization and root-device selection.
- Independent console/msgbuf tails and panic text have been recovered from UFS;
  a watchdog restores the development device after a bounded experiment.
- The read-only IOKit UFS root adapter in this repository is **experimental**.
  Compilation and CI results do not establish APFS mount or launchd success.
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
git apply ../device/patches/build/prepare-xnu-source.patch
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
