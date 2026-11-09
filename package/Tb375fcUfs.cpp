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

extern kmod_info_t g_kernel_kmod_info;
extern "C" void tb375fc_storage_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void tb375fc_storage_disk_init(void) __asm__("__GLOBAL__sub_I_Tb375fcUfs.cpp");
extern "C" const OSMetaClass *tb375fc_storage_parent(void);
extern "C" void native_source_storage_start(void);

static const OSMetaClass *storageParent;

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
    uint64_t reads = 0, failures = 0;
    uint8_t page[4096] = {};

    int readOne(uint32_t lba) {
        int error;
        for (unsigned retry = 0;; retry++) {
            error = tb375fc_ufs_read_block(lba, page);
            if (error != -(int)TB375FC_DIAG_BUSY || retry >= 1000) break;
            IOSleep(1);  // recorder owns the same transfer list; never take it over
        }
        return error;
    }
public:
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
    if (!IOBlockStorageDevice::start(provider)) return false;
    if (!PE_parse_boot_argn("tbroot_lba", &first, sizeof(first)) ||
        !PE_parse_boot_argn("tbroot_blocks", &blocks, sizeof(blocks)) || !blocks) return false;
    uint64_t end = (uint64_t)first + blocks;
    if (!((first >= 1492992u && end <= 2803712u) ||
          (first >= 31170552u && end <= 31236088u))) {
        IOLog("Tb375fcUfs: window refused lba=%u blocks=%u\n", first, blocks);
        return false;
    }
    gate = IOLockAlloc();
    if (!gate) return false;
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
    if (error) failures++;
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

extern "C" void native_source_storage_start(void)
{
    static bool attempted;
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("tbroot", &enabled, sizeof(enabled)) || enabled != 1 || attempted) return;
    attempted = true;
    OSReturn status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
    if (status != kOSReturnSuccess) panic("TB375FC storage: IOStorageFamily load failed %x", status);
    const OSSymbol *name = OSSymbol::withCString("IOBlockStorageDevice");
    storageParent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (name) name->release();
    if (!storageParent || storageParent->getClassSize() != sizeof(IOBlockStorageDevice))
        panic("TB375FC storage: base size original=%u source=%lu",
            storageParent ? storageParent->getClassSize() : 0, sizeof(IOBlockStorageDevice));
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
    Tb375fcUfs *disk = new Tb375fcUfs;
    if (!disk) panic("TB375FC storage: allocation failed");
    const char *step = !disk->init(nullptr) ? "init" : !disk->attach(provider) ? "attach" :
        !disk->start(provider) ? "start" : nullptr;
    if (step) panic("TB375FC storage: disk %s failed", step);
    IOLog("Tb375fcUfs: source window published through original IOStorageFamily\n");
}
