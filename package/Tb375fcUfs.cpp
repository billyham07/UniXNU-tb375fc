/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * TB375FC read-only LUN2 window. Hardware transport: Tb375fcDiag.c, shared with
 * the mrdump recorder (one controller owner/lock, no storage writes here).
 * Publication/later class registration: UniXNU-sheng package/SourceStorage.cpp;
 * IOBlockStorageDevice interface: UniXNU RamBlock/NativeRamBlock.cpp.
 */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include "Tb375fcDiag.h"
#include "verified_root.h"
#include "VerifiedBlockPolicy.h"
#include <libkern/crypto/sha2.h>

extern "C" {
extern const uint8_t verified_root_key[VR_KEY_BYTES];
extern const struct vr_crypto vr_xnu_crypto;
bool native_source_root_policies_load(void);
int native_ufs_block_authenticates(IOService *, const char *, uint8_t *);
volatile uint32_t native_ufs_integrity_failures = 0;
}

extern kmod_info_t g_kernel_kmod_info;
extern "C" void tb375fc_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void tb375fc_storage_disk_init(void) __asm__("__GLOBAL__sub_I_Tb375fcUfs.cpp");
extern "C" const OSMetaClass *tb375fc_storage_parent(void);
extern "C" void native_source_storage_start(void);

static const OSMetaClass *storageParent;

// xnu: dyld reads its flags from the comm page (osfmk/arm/commpage/commpage.c).
extern "C" uint64_t dyld_flags;
extern "C" void commpage_update_dyld_flags(uint64_t);

/*
 * tb_skip_ignition=1 sets the existing dyld skip-Ignition flag (0x8) before launchd runs, as the
 * UniXNU offline-owner-systemos-mount.patch does after its SystemOS mount (UniXNU-zeta: dyld_flags=0x8).
 * Without it launchd blocks in security.mac.img4.ignition_blob: the AppleImage4 handler gate-waits
 * for a service start this board cannot complete yet (devloop 20261109T114334Z; UniXNU-uke
 * docs/PROGRESS.md, 2026-11-03).
 */
static void skip_ignition_if_requested(void)
{
    unsigned skip = 0;
    if (!PE_parse_boot_argn("tb_skip_ignition", &skip, sizeof(skip)) || skip != 1) return;
    dyld_flags |= UINT64_C(8);
    commpage_update_dyld_flags(dyld_flags);
    printf("TB375FC: dyld skip-Ignition set in comm page, dyld_flags=0x%llx\n", (unsigned long long)dyld_flags);
}

extern "C" const OSMetaClass *tb375fc_storage_parent(void)
{
    if (!storageParent) panic("TB375FC storage: original family not ready");
    return storageParent;
}

class Tb375fcUfs : public IOBlockStorageDevice {
    OSDeclareDefaultStructors(Tb375fcUfs)
    IOLock *gate = nullptr;
    uint32_t first = 0, blocks = 0;
    bool online = false;
    uint64_t reads = 0, failures = 0, blocksRead = 0;
    uint8_t page[4096] = {};
    vr_view verified = {};
    const uint8_t *envelope = nullptr;
    uint64_t envelopeBytes = 0;
    // tb_unverified=1: no per-block policy (the full OS + SystemOS hash tables, ~82 MB, do not fit the
    // 64 MiB boot image). The window is then identified only by its boot-arg volume UUID.
    bool unverified = false;
    char volumeUUID[37] = {};

    bool configureVerification() {
        unsigned matches = 0;
        for (uint64_t i = 0; i < verified_root_policy_count; i++) {
            const auto &policy = verified_root_policies[i];
            vr_view candidate = {};
            int status = vr_open(policy.manifest, policy.manifestBytes, verified_root_key,
                1, &vr_xnu_crypto, &candidate);
            if (status != VR_OK) return false;
            if (candidate.image_bytes != (uint64_t)blocks * 4096) continue;
            if (++matches != 1) return false;
            verified = candidate;
            envelope = policy.manifest;
            envelopeBytes = policy.manifestBytes;
        }
        return matches == 1 && verified.hashes;
    }

    int readOne(uint32_t lba) {
        if ((!verified.hashes && !unverified) || lba < first || lba - first >= blocks ||
            __atomic_load_n(&native_ufs_integrity_failures, __ATOMIC_ACQUIRE)) return -1;
        int error;
        for (unsigned retry = 0;; retry++) {
            error = tb375fc_ufs_read_block(lba, page);
            if (error != -(int)TB375FC_DIAG_BUSY || retry >= 1000) break;
            IOSleep(1);  // recorder owns the same transfer list; never take it over
        }
        if (!error && !unverified) {
            int status = vr_verify_block(&verified, lba - first, page, sizeof(page), &vr_xnu_crypto);
            if (status != VR_OK) {
                __atomic_fetch_add(&native_ufs_integrity_failures, 1u, __ATOMIC_ACQ_REL);
                online = false;
                IOLog("Tb375fcRoot: block integrity rejected index=%u status=%d\n", lba - first, status);
                return -100 - status;
            }
        }
        return error;
    }
public:
    void setWindow(uint32_t lba, uint32_t count, const char *uuid) {
        first = lba; blocks = count;
        if (uuid) {
            unsigned i = 0;
            for (; i < 36 && uuid[i]; i++)
                volumeUUID[i] = (uuid[i] >= 'a' && uuid[i] <= 'f') ? (char)(uuid[i] - 32) : uuid[i];
            volumeUUID[i] = 0;
        }
    }
    bool authenticatesVolume(const char *uuid, uint8_t *digest) {
        if (!uuid || !gate) return false;
        IOLockLock(gate);
        char expected[37];
        static const char hex[] = "0123456789ABCDEF";
        unsigned at = 0;
        for (unsigned i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) expected[at++] = '-';
            expected[at++] = hex[verified.volume_uuid[i] >> 4];
            expected[at++] = hex[verified.volume_uuid[i] & 15];
        }
        expected[at] = 0;
        bool valid = online && verified.hashes && envelope && !strcmp(uuid, expected) &&
            !__atomic_load_n(&native_ufs_integrity_failures, __ATOMIC_ACQUIRE);
        if (unverified) {
            valid = online && volumeUUID[0] && !strcmp(uuid, volumeUUID);
            if (valid && digest) memset(digest, 0, 48);
        }
        if (valid && digest) {
            SHA384_CTX context;
            SHA384_Init(&context);
            SHA384_Update(&context, envelope, envelopeBytes);
            SHA384_Final(digest, &context);
        }
        IOLockUnlock(gate);
        return valid;
    }
    bool start(IOService *) override;
    void stop(IOService *) override;
    void free() override;
    IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
    IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
    UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
    char *getVendorString() override { return const_cast<char *>("TB375FC"); }
    char *getProductString() override { return const_cast<char *>("Read-only UFS root window"); }
    char *getRevisionString() override { return const_cast<char *>("1.0"); }
    char *getAdditionalDeviceInfoString() override { return const_cast<char *>("LUN2; shared diagnostic transport"); }
    IOReturn reportBlockSize(UInt64 *v) override { *v = 4096; return kIOReturnSuccess; }
    IOReturn reportMaxValidBlock(UInt64 *v) override { *v = blocks - 1; return kIOReturnSuccess; }
    IOReturn reportEjectability(bool *v) override { *v = false; return kIOReturnSuccess; }
    IOReturn reportRemovability(bool *v) override { *v = false; return kIOReturnSuccess; }
    IOReturn reportWriteProtection(bool *v) override { *v = true; return kIOReturnSuccess; }
    IOReturn reportMediaState(bool *present, bool *changed) override {
        *present = online; if (changed) *changed = false; return kIOReturnSuccess;
    }
    IOReturn doSynchronizeCache() override { return online ? kIOReturnSuccess : kIOReturnNotReady; }
    IOReturn doSynchronize(UInt64, UInt64, IOOptionBits) override { return doSynchronizeCache(); }
    IOReturn getWriteCacheState(bool *v) override { *v = false; return kIOReturnSuccess; }
    IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
    IOReturn doAsyncReadWrite(IOMemoryDescriptor *, UInt64, UInt64,
        IOStorageAttributes *, IOStorageCompletion *) override;
};
OSDefineMetaClassAndStructors(Tb375fcUfs, IOBlockStorageDevice)

bool Tb375fcUfs::start(IOService *provider)
{
    if (!IOBlockStorageDevice::start(provider) || !blocks) return false;
    uint64_t end = (uint64_t)first + blocks;
    // super unallocated tail, mrdump, tb_ipados (sdc83, split from userdata 2026-11-09)
    if (!((first >= 1492992u && end <= 2803712u) ||
          (first >= 31170552u && end <= 31236088u) ||
          (first >= 14393088u && end <= 31170552u))) {
        IOLog("Tb375fcUfs: window refused lba=%u blocks=%u\n", first, blocks);
        return false;
    }
    gate = IOLockAlloc();
    if (!gate) return false;
    unsigned skip = 0;
    if (PE_parse_boot_argn("tb_unverified", &skip, sizeof(skip)) && skip == 1) {
        if (strlen(volumeUUID) != 36) {
            IOLog("Tb375fcUfs: unverified window lba=%u needs its volume UUID boot-arg\n", first);
            return false;
        }
        unverified = true;
        IOLog("Tb375fcRoot: UNVERIFIED window lba=%u blocks=%u volume=%s (tb_unverified=1)\n",
            first, blocks, volumeUUID);
    } else if (!configureVerification()) {
        IOLog("Tb375fcRoot: no unique authenticated policy for %u blocks\n", blocks);
        return false;
    } else {
        IOLog("Tb375fcRoot: RSA signature/hash table accepted; verifying every 4KiB read\n");
    }
    int error = readOne(first);
    if (error) {
        IOLog("Tb375fcUfs: first READ(10) failed lba=%u error=%d\n", first, error);
        return false;
    }
    IOLog("Tb375fcUfs: block0 signature=%c%c%c%c lun=2 lba=%u blocks=%u readonly=1\n",
        page[32], page[33], page[34], page[35], first, blocks);
    online = true;
    registerService();
    return true;
}

IOReturn Tb375fcUfs::doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
    IOStorageAttributes *, IOStorageCompletion *completion)
{
    if (!buffer || !completion || !completion->action || !count || block >= blocks ||
        count > blocks - block || buffer->getLength() < count * 4096) return kIOReturnBadArgument;
    if (buffer->getDirection() != kIODirectionIn) return kIOReturnNotWritable;
    if (!gate) return kIOReturnNotReady;
    IOLockLock(gate);
    if (!online) { IOLockUnlock(gate); return kIOReturnNotReady; }
    uint64_t done = 0;
    int error = 0;
    for (uint64_t i = 0; i < count; i++) {
        error = readOne(first + (uint32_t)(block + i));
        if (error) break;
        if (buffer->writeBytes(done, page, 4096) != 4096) { error = -1; break; }
        done += 4096;
    }
    reads++;
    blocksRead += done / 4096;
    if (error) failures++;
    if (!(reads & 4095))
        IOLog("Tb375fcUfs: lba=%u requests=%llu blocks=%llu failures=%llu\n", first, reads, blocksRead, failures);
    if (reads <= 8 || (error && failures <= 16))
        IOLog("Tb375fcUfs: read=%llu block=%llu count=%llu done=%llu error=%d\n",
            reads, block, count, done, error);
    IOLockUnlock(gate);
    completion->action(completion->target, completion->parameter,
        error ? kIOReturnIOError : kIOReturnSuccess, done);
    return kIOReturnSuccess;
}

void Tb375fcUfs::stop(IOService *provider)
{
    if (gate) { IOLockLock(gate); online = false; IOLockUnlock(gate); }
    IOBlockStorageDevice::stop(provider);
}

void Tb375fcUfs::free()
{
    if (gate) { IOLockFree(gate); gate = nullptr; }
    IOBlockStorageDevice::free();
}

extern "C" int native_ufs_block_authenticates(IOService *provider, const char *uuid, uint8_t *digest)
{
    Tb375fcUfs *block = OSDynamicCast(Tb375fcUfs, provider);
    if (!block) return -1;
    return block->authenticatesVolume(uuid, digest) ? 1 : 0;
}

extern "C" void native_source_storage_start(void)
{
    static bool attempted;
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("tbroot", &enabled, sizeof(enabled)) || enabled != 1 || attempted) return;
    attempted = true;
    skip_ignition_if_requested();
    unsigned keepPwm = 0;
    if (!PE_parse_boot_argn("tbufs_pwm", &keepPwm, sizeof(keepPwm)) || !keepPwm) {
        int32_t link[6] = {};
        int status = tb375fc_ufs_link_hs(link);
        IOLog("Tb375fcUfs: link switch status=%d result=0x%x pwrmode=0x%x gear=%d/%d lanes=%d/%d\n",
            status, link[0], link[1], link[2], link[3], link[4], link[5]);
    }
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess) panic("TB375FC storage: IOStorageFamily load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("TB375FC storage: base size original=%u source=%lu",
            storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));
    if (!native_source_root_policies_load())
        panic("TB375FC root: signed policy handoff rejected");
    status = OSKextLoadKextWithIdentifier("com.apple.filesystems.apfs");
    if (status != kOSReturnSuccess) panic("TB375FC storage: APFS load failed %x", status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) panic("TB375FC storage: cannot start class registration");
    tb375fc_storage_base_init();
    tb375fc_storage_disk_init();
    status = OSMetaClass::postModLoad(handle);
    if (status != kOSReturnSuccess) panic("TB375FC storage: class registration failed %x", status);
    OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
    IOService *provider = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
    if (matching) matching->release();
    if (!provider) panic("TB375FC storage: platform root unavailable");
    // One verified read-only window per volume: the root (tbroot_*) and, for the full system,
    // the SystemOS cryptex (tbsys_*) that offline-owner-systemos-mount finds by volume UUID.
    static const struct { const char *lba, *count, *uuid, *role; bool required; } windows[] = {
        {"tbroot_lba", "tbroot_blocks", "tbroot_uuid", "root", true},
        {"tbsys_lba", "tbsys_blocks", "tbsys_uuid", "SystemOS", false},
    };
    for (const auto &w : windows) {
        uint32_t lba = 0, count = 0;
        if (!PE_parse_boot_argn(w.lba, &lba, sizeof(lba)) || !PE_parse_boot_argn(w.count, &count, sizeof(count))) {
            if (w.required) panic("TB375FC storage: %s window not configured", w.role);
            continue;
        }
        Tb375fcUfs *disk = new Tb375fcUfs;
        if (!disk) panic("TB375FC storage: allocation failed");
        char uuid[40] = {};
        if (!PE_parse_boot_argn(w.uuid, uuid, sizeof(uuid))) uuid[0] = 0;
        disk->setWindow(lba, count, uuid);
        const char *step = !disk->init(nullptr) ? "init" : !disk->attach(provider) ? "attach" :
            !disk->start(provider) ? "start" : nullptr;
        if (step) panic("TB375FC storage: %s disk %s failed", w.role, step);
        IOLog("Tb375fcUfs: %s window published through original IOStorageFamily\n", w.role);
    }
}
