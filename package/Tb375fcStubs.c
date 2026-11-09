/*
 * TB375FC: definitions for the weak hooks the sheng iPadOS 27 bring-up patches reference
 * (zzzz-sheng-ipados27-bringup.patch, source-storage-runtime.patch). Sheng links its own BootLog
 * archive. TB375FC uses its own recorder and read-only source storage adapter.
 *   boot_log_rounds: only polled with boot-arg sheng-log=1, which this board never sets.
 */
#include <stdint.h>

volatile uint32_t boot_log_rounds;

/*
 * Data RAM disk (framework drivers/RamBlock/NativeRamBlock.cpp, XNU_OWNER_DATA_MOUNT):
 *   native_ufs_store_*: the optional persistent overlay (RamBlock PersistOverlay.h). mode 0 keeps it off, so
 *     Data stays in RAM and the others are never called. A tb_ipados store window can implement them later.
 *     The kernel link cannot leave the weak imports undefined.
 *   sheng_boot_watchdog_policy: UniXNU-sheng's launchd job policy after the Data mount; 0 = no policy.
 */
unsigned native_ufs_store_mode(void);
int native_ufs_store_open(uint64_t blocks, uint64_t *first, const char **why);
unsigned native_ufs_store_gpt_check(void);
int native_ufs_store_read(uint64_t block, uint32_t count, void *data);
int native_ufs_store_write(uint64_t block, uint32_t count, const void *data);
int native_ufs_store_sync(void);
int sheng_boot_watchdog_policy(void *process, uint64_t scratch);

int native_ufs_store_read(uint64_t block, uint32_t count, void *data)
{
    (void)block; (void)count; (void)data;
    return 0;
}

int native_ufs_store_write(uint64_t block, uint32_t count, const void *data)
{
    (void)block; (void)count; (void)data;
    return 0;
}

int native_ufs_store_sync(void)
{
    return 0;
}

int sheng_boot_watchdog_policy(void *process, uint64_t scratch)
{
    (void)process; (void)scratch;
    return 0;
}

unsigned native_ufs_store_mode(void)
{
    return 0;
}

int native_ufs_store_open(uint64_t blocks, uint64_t *first, const char **why)
{
    (void)blocks; (void)first;
    if (why) *why = "TB375FC: no persistent Data store yet";
    return 0;
}

unsigned native_ufs_store_gpt_check(void)
{
    return 0;
}
