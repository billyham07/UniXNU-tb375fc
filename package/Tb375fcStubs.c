/*
 * TB375FC: definitions for the weak hooks the sheng iPadOS 27 bring-up patches reference
 * (zzzz-sheng-ipados27-bringup.patch, source-storage-runtime.patch). Sheng links its own BootLog
 * archive. TB375FC uses its own recorder and read-only source storage adapter.
 *   boot_log_rounds: only polled with boot-arg sheng-log=1, which this board never sets.
 */
#include <stdint.h>

volatile uint32_t boot_log_rounds;
