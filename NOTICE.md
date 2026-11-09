# Source provenance and distribution boundary

## Original device integration

TB375FC diagnostics, glue, UFS root integration, DTS/configuration, CI and host
tools are published as source under BSD-2-Clause-Patent unless a file says
otherwise. The recorder and storage transport were ported from this device's
earlier UEFI/16.5 implementation; addresses, DMA layout and storage windows are
TB375FC-specific.

The public snapshot originates from the project's bootstrap device branch
and its staged read-only-root changes. Private Git history, workflow runs,
artifacts, caches, machine credentials and hardware identity seeds are **not**
part of this publication.

## Upstream work

- **UniXNU/UniXNU**, pinned by `ci/framework-revision`, provides the framework,
  public OS profiles, source preparation, component builders and prelink flow.
  Upstream copyright: Copyright (c) 2026, UniXNU contributors.
  License: BSD-2-Clause-Patent.
- **UniXNU/UniXNU-sheng** supplies the 27.x bring-up patches
  (`zzzz-sheng-ipados27-bringup.patch`, CPU-idle, AMP and audio-addon patches),
  and the source-storage publication/late-class-registration design.
  Its copyright and BSD-2-Clause-Patent notice are retained in `LICENSE`.
- **Allenwdk/UniXNU-uke** supplies the bootstrap organization and physical/video
  console patch reference, and its `package/UkeLastLog.c` informed the
  independent console/msgbuf tails. Original upstream notices still apply.
- **UniXNU-tb365fc** is the layout/MTK LK-to-UEFI reference, not a source of
  TB375FC peripheral addresses.
- **UniXNU/RamBlock** provides the IOBlockStorageDevice interface reference.
- **UniXNU/VerifiedRoot**, catalog-pinned at
  `974f043d0c71ba2e60331483a41c6c6d35274ee3`, supplies RSA3072/SHA256
  policy parsing, block verification and the source-XNU crypto adapter.
- Root-policy handoff and mounted-device queries are adapted from
  **UniXNU/UniXNU-sheng** (`689400177f93219a38bd7cf5ae2e9f5782adc594`),
  `package/SourceRootPolicies.cpp` and `package/SourceRootBinding.cpp`.
  The full envelope is copied read-only before validation. Header-only policies
  are not supported by this adapter.
- **none24smu/UniXNU**, `ios26-ufs-owner-root` at
  `a8b90c0c645fd62ae850cd43e80c87bab4cc157d`, informed the mounted/rootdev
  equality, read-only media and integrity-failure-latch requirements. Its
  transport, non-PAC profile and userdata rewriting are not imported.
- **mouseos/UniXNU-a04br3** (`a8905ba`) is the independent owner-public-key
  build integration reference. All these UniXNU components retain their
  BSD-2-Clause-Patent licenses.
- Apple open-source **XNU**, **IOStorageFamily**, **IOHIDFamily**, libpthread,
  libdispatch and other dependencies are fetched at recorded commits, not
  vendored as binaries. The IOStorageFamily pin is
  `7edb88fbae296fb7c8ce2f64e115e116e566d51c`.

Patches do **not** relicense the Apple or other third-party files they target.
Retain the original copyright/license headers when applying them. Apple
open-source licenses (including APSL where applicable) and any other
per-component licenses continue to apply.

## Proprietary material

IPSW members, Apple's proprietary kexts, extracted kernelcaches, SDKs and
assembled mixed kernels are not released as open-source source or plaintext
artifacts. The public CI may transiently process the profile's inputs, subject
to their terms, and only returns its mixed result encrypted for the operator.
The recipient's private key is not part of this repository or any workflow.
This project's source license grants no rights to Apple's proprietary software.

The public repository is for reproducible, reviewable development. It is not an
Apple software mirror or a claim that all material needed for a bootable image
is open source.
