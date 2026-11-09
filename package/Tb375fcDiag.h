/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef TB375FC_DIAG_H
#define TB375FC_DIAG_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* DEVELOPMENT-only early-boot recorder; not a general-purpose UFS driver.
 * Call early_init once on the boot CPU after PE_init_platform(FALSE, args).
 * putc/stage are safe before VM setup (RAM only). Call vm_ready exactly once
 * AFTER arm_vm_init returns, on that same CPU, before secondary CPUs start.
 * vm_ready maps MMIO and persists the buffered early log. No heap allocation,
 * printf, scheduler, thread, boot-arg parser or watchdog service is used.
 *
 * This transport must be the ONLY kernel UFS owner. The TB375FC read-only
 * IOKit root uses read_block below and shares its io_lock with the recorder.
 * Stop it successfully before any unrelated UFS driver takes ownership.
 * No screenshot capture. Existing UEFI screenshots/logs are left untouched.
 */
void tb375fc_diag_early_init(void);
int tb375fc_diag_vm_ready(void);
void tb375fc_diag_putc(char c);
/* msgbuf producer tap: RAM only, safe while bsd_log_lock is held. */
void tb375fc_diag_msgbuf_putc(char c);
void tb375fc_diag_stage(const char *text);
void tb375fc_diag_panic(const char *text);
/* Call poll from a periodic callback (e.g. once a second); it starts no timer.
 * All I/O calls use try-locks, including panic: they never wait for another CPU.
 * A panic interrupting an in-flight flush may therefore not reach the disk.
 */
void tb375fc_diag_poll(void);
int tb375fc_diag_flush(void);
int tb375fc_diag_stop(void);
uint32_t tb375fc_diag_last_error(void);
/* LUN2 READ(10), exactly 4096 bytes. Caller bounds its root window; reads are
 * not storage writes. Shares the recorder's io_lock and DMA descriptors.
 * Returns 0 or negative DIAG error; BUSY never waits and may be retried. */
int tb375fc_ufs_read_block(uint32_t lba, void *out);
int tb375fc_ufs_link_hs(int32_t out[6]);

#define TB375FC_DIAG_OK          0u
#define TB375FC_DIAG_NOT_READY   1u
#define TB375FC_DIAG_BUSY        2u
#define TB375FC_DIAG_DMA         3u
#define TB375FC_DIAG_POLICY      4u
#define TB375FC_DIAG_STATE       5u
#define TB375FC_DIAG_TIMEOUT     6u
#define TB375FC_DIAG_RESPONSE    7u
#define TB375FC_DIAG_RESTORE     8u
#define TB375FC_DIAG_STOPPED     9u

/* mrdump-relative layout; all words big-endian. Two slots of 9 x 4096 B,
 * at blocks 64000 and 64009, between screenshot end 63492 and UEFI log 64512.
 * Slot = header block followed by up to 32768 UTF-8/raw bytes, with labeled
 * console and msgbuf sections (16320-byte chronological tail each). This is
 * not a merged timeline; a message emitted to both appears in both sections.
 * Header words (32-bit):
 *  0 magic "TBKL", 1 version=1, 2 committed sequence (starts at 1),
 *  3 milliseconds since early_init (wraps at 2^32), 4 text bytes,
 *  5 CRC32/IEEE of text, 6 flags (bit 0 panic, bit 1 either ring wrapped),
 *  7 successful WRITE(10) count before header commit, 8 I/O errors,
 *  9 log append attempts dropped on contention (not overwritten ring bytes),
 * 10 sum of both rings' total bytes low, 11 high, 12 boot counter low, 13 high,
 * 14 last UFS error, 15 header CRC32/IEEE over bytes 0..59.
 * Remaining header bytes are zero. Zero header -> payload -> valid header;
 * alternate ONLY after a successful commit. Validate BOTH CRCs on the host.
 * Clear both headers before each experiment or match the boot counter: a
 * failure before vm_ready cannot invalidate an older boot's on-disk records.
 */
#define TB375FC_DIAG_MAGIC       0x54424b4cu
#define TB375FC_DIAG_BLOCK       4096u
#define TB375FC_DIAG_LOG_BYTES   32768u
#define TB375FC_DIAG_SLOT_BLOCK  64000u
#define TB375FC_DIAG_SLOT_BLOCKS 9u

#ifdef __cplusplus
}
#endif
#endif
