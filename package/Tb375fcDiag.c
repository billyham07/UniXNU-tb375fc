/*
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 * TB375FC / MT6897 early XNU flight recorder, xnu-12377 arm64(e).
 *
 * Port of Tb375fcDiagDxe.c UfsWrite and the hardware-tested XNU 16.5 driver
 * hakios.tb375fc_ufs_ro.c read_block_or_fr/issue_fr_write. Addresses and LUN
 * come from this device, not sheng. See Tb375fcDiag.h for integration/layout.
 * Writes only the two log slots inside the authorized mrdump window.
 *
 * DMA uses small, page-aligned kernel BSS buffers, translated with kvtophys;
 * never mistake a kernel virtual pointer for a physical DMA address. Cache
 * maintenance is to PoC, as in stage0/ufs_ro.c. One 4 KiB WRITE(10) at a time
 * also works with the PWM-G1 link left by LK (no link/power-mode changes).
 */
#include "Tb375fcDiag.h"
#include <stddef.h>

/* Same ABI as machine_routines.h / arm/pmap.h on arm64 XNU. Keeping this
 * archive independent of generated XNU headers permits a freestanding build.
 */
extern uintptr_t ml_io_map(uintptr_t pa, uintptr_t size);
extern uint64_t kvtophys(uintptr_t va);

#define UFS_PA      0x112b0000u
#define MRD_LBA     31170552u
#define MRD_BLOCKS  65536u
#define BLOCK       TB375FC_DIAG_BLOCK
#define LOG_BYTES   TB375FC_DIAG_LOG_BYTES
#define LOG_BLOCK   TB375FC_DIAG_SLOT_BLOCK
#define SLOT_BLOCKS TB375FC_DIAG_SLOT_BLOCKS
#define CAP  0x00u
#define AHIT 0x18u
#define IS   0x20u
#define IE   0x24u
#define HCS  0x30u
#define HCE  0x34u
#define BA   0x50u
#define BAU  0x54u
#define DBR  0x58u
#define CLR  0x5cu
#define RSR  0x60u
#define CNR  0x64u
#define TMDBR 0x78u
#define UIC_UE (1u << 2)
#define FATAL ((1u << 11) | (1u << 12) | (1u << 16) | (1u << 17))
#define RESP 512u
#define PRDT 1024u
#define LUN 2u
#define ALIGN_DMA __attribute__((aligned(16384)))

_Static_assert(LOG_BLOCK >= 12u * 5291u, "overlaps screenshot slots");
_Static_assert(LOG_BLOCK + 2u * SLOT_BLOCKS <= 64512u, "overlaps UEFI log");
_Static_assert(LOG_BYTES == (SLOT_BLOCKS - 1u) * BLOCK, "log slot size");

static uint32_t list[256] ALIGN_DMA;
static uint8_t command[2048] ALIGN_DMA;
static uint8_t bounce[BLOCK] ALIGN_DMA;
/* Two independent tails, as UniXNU-uke UkeLastLog's msgbuf capture plus the
 * early console. Tap msgbuf at its producer instead of sampling msg_bufx:
 * this preserves exact wrap boundaries and never takes the BSD log lock.
 * Section labels fit in the 128 bytes left in the existing on-disk payload.
 */
#define STREAM_BYTES ((LOG_BYTES - 128u) / 2u)
static uint8_t ring[STREAM_BYTES], log_ring[STREAM_BYTES], snapshot[LOG_BYTES];
static uint32_t ring_lock, io_lock, enabled, stopped, vm_called;
static uint32_t dropped, panic_seen, error_code = TB375FC_DIAG_NOT_READY;
static uint32_t seq, writes, errors;
static uint64_t total, log_total, boot_counter, frequency, last_flush;
static uintptr_t hci;
static uint64_t list_pa, command_pa, bounce_pa;

static uint64_t ticks(void)
{
    uint64_t n;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(n));
    return n;
}

static uint64_t irq_save(void)
{
    uint64_t daif;
    __asm__ volatile("mrs %0, daif; msr daifset, #3" : "=r"(daif) :: "memory");
    return daif;
}

static void irq_restore(uint64_t daif)
{
    __asm__ volatile("msr daif, %0" :: "r"(daif) : "memory");
}

static int try_lock(uint32_t *lock)
{
    return !__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE);
}

static void unlock(uint32_t *lock)
{
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
}

static void zero(void *p, size_t n)
{
    uint8_t *b = p;
    while (n--) *b++ = 0;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static uint32_t crc32(const uint8_t *p, size_t n)
{
    uint32_t crc = ~0u;
    while (n--) {
        crc ^= *p++;
        for (unsigned i = 0; i < 8; i++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

static uint32_t elapsed_ms(uint64_t now)
{
    uint64_t d = now - boot_counter;
    return (uint32_t)((d / frequency) * 1000u + (d % frequency) * 1000u / frequency);
}

/* All DMA buffers are exclusively CPU-owned while this runs. CIVAC before
 * and after device writes prevents a dirty CPU line overwriting the response.
 */
static void cache_dma(void *p, size_t n)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uintptr_t step = (uintptr_t)4u << ((ctr >> 16) & 15u);
    uintptr_t end = (uintptr_t)p + n;
    for (uintptr_t a = (uintptr_t)p & ~(step - 1u); a < end; a += step)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint32_t rd(unsigned r)
{
    return *(volatile uint32_t *)(hci + r);
}

static void wr(unsigned r, uint32_t v)
{
    *(volatile uint32_t *)(hci + r) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}

static int wait_bits(unsigned r, uint32_t mask, uint32_t want, uint64_t deadline)
{
    do {
        if ((rd(r) & mask) == want) return 1;
    } while (ticks() < deadline);
    return 0;
}

static void absorb_uic_error(void)
{
    if (rd(IS) & UIC_UE) {
        for (unsigned i = 0; i < 5; i++) (void)rd(0x38u + 4u * i);
        wr(IS, UIC_UE);
    }
}

/* Single allow-list predicate, applied to both the request and encoded CDB.
 * No externally supplied LBA, LUN, length or DMA address is accepted by APIs.
 */
static int write_permitted(uint32_t lba, uint32_t blocks)
{
    return blocks == 1 && lba >= MRD_LBA + LOG_BLOCK &&
        (uint64_t)lba + blocks <= (uint64_t)MRD_LBA + LOG_BLOCK + 2u * SLOT_BLOCKS &&
        (uint64_t)lba + blocks <= (uint64_t)MRD_LBA + MRD_BLOCKS;
}

static int dma_extent(void *p, size_t n, uint64_t *pa)
{
    *pa = kvtophys((uintptr_t)p);
    return *pa && !(*pa & (BLOCK - 1u)) &&
        kvtophys((uintptr_t)p + n - 1u) == *pa + n - 1u &&
        ((rd(CAP) & (1u << 24)) || !((*pa + n - 1u) >> 32));
}

static int fail(uint32_t why)
{
    __atomic_store_n(&error_code, why, __ATOMIC_RELAXED);
    return -(int)why;
}

/* Caller holds io_lock. Overall snapshot budget is one second, plus at most
 * 100 ms cleanup for each failed command. Never service/disable the watchdog.
 * Like the verified UEFI path, save/restore the firmware's transfer-list base.
 */
static int transfer_block(uint32_t lba, uint64_t deadline, int read)
{
    uint32_t status = TB375FC_DIAG_TIMEOUT;
    if (!read && !write_permitted(lba, 1)) return fail(TB375FC_DIAG_POLICY);
    if (ticks() >= deadline) return fail(TB375FC_DIAG_TIMEOUT);
    absorb_uic_error();
    uint32_t lo = rd(BA), hi = rd(BAU), old_is = rd(IS), cnr = rd(CNR);
    if (rd(HCE) != 1 || (rd(HCS) & 3u) != 3u || rd(RSR) != 1 ||
        rd(DBR) || rd(TMDBR) || rd(AHIT) || rd(IE) || (old_is & FATAL)) {
        __atomic_store_n(&enabled, 0u, __ATOMIC_RELEASE);
        return fail(TB375FC_DIAG_STATE);
    }
    zero(list, sizeof(list)); zero(command, sizeof(command));
    command[0] = 1; command[1] = read ? 0x40 : 0x20; command[2] = LUN;
    put32(command + 12, BLOCK);
    command[16] = read ? 0x28 : 0x2a; put32(command + 18, lba); command[24] = 1;
    uint32_t *prdt = (uint32_t *)(command + PRDT);
    prdt[0] = (uint32_t)bounce_pa; prdt[1] = (uint32_t)(bounce_pa >> 32); prdt[3] = BLOCK - 1u;
    list[0] = (1u << 28) | (read ? 2u : 1u) << 25; list[2] = 15;
    list[4] = (uint32_t)command_pa; list[5] = (uint32_t)(command_pa >> 32);
    list[6] = (RESP / 4u) << 16 | RESP / 4u;
    list[7] = (PRDT / 4u) << 16 | 1u;
    if (command[16] != (read ? 0x28 : 0x2a) || command[2] != LUN || get32(command + 12) != BLOCK ||
        (!read && !write_permitted(get32(command + 18), (uint32_t)command[23] << 8 | command[24])))
        return fail(TB375FC_DIAG_POLICY);
    cache_dma(list, sizeof(list)); cache_dma(command, sizeof(command)); cache_dma(bounce, BLOCK);
    wr(RSR, 0);
    if (wait_bits(RSR, 1, 0, deadline) && !rd(DBR)) {
        wr(BA, (uint32_t)list_pa); wr(BAU, (uint32_t)(list_pa >> 32)); wr(RSR, 1);
        if (wait_bits(RSR, 1, 1, deadline)) {
            wr(DBR, 1);
            if (wait_bits(DBR, 1, 0, deadline)) {
                cache_dma(list, sizeof(list)); cache_dma(command, sizeof(command));
                const uint8_t *q = command + RESP;
                status = ((list[2] & 255u) || q[0] != 0x21 || q[2] != LUN || q[3] ||
                    q[6] || q[7] || (q[1] & 0x60u) || get32(q + 12)) ? TB375FC_DIAG_RESPONSE : 0u;
            }
        }
    }
    uint64_t cleanup = ticks() + frequency / 10u;
    if (rd(BA) == (uint32_t)list_pa && rd(BAU) == (uint32_t)(list_pa >> 32) && (rd(DBR) & 1u)) {
        wr(CLR, ~1u); (void)wait_bits(DBR, 1, 0, cleanup);
    }
    wr(RSR, 0);
    int restored = 0;
    if (wait_bits(RSR, 1, 0, cleanup) && !rd(DBR)) {
        wr(CNR, (rd(CNR) & ~cnr) & 1u);
        wr(IS, (rd(IS) & ~old_is) & 1u);
        wr(BA, lo); wr(BAU, hi); wr(RSR, 1);
        restored = wait_bits(RSR, 1, 1, cleanup) && rd(BA) == lo && rd(BAU) == hi &&
            !rd(DBR) && !rd(TMDBR) && !rd(IE) && !rd(AHIT) && rd(HCE) == 1 && (rd(HCS) & 3u) == 3u;
    }
    absorb_uic_error();
    if (!restored || (rd(IS) & FATAL)) {
        status = TB375FC_DIAG_RESTORE;
        __atomic_store_n(&enabled, 0u, __ATOMIC_RELEASE);
    }
    if (status) { errors++; return fail(status); }
    if (read) cache_dma(bounce, BLOCK);
    else writes++;
    return 0;
}

static int write_block(uint32_t lba, uint64_t deadline)
{
    return transfer_block(lba, deadline, 0);
}

int tb375fc_ufs_read_block(uint32_t lba, void *out)
{
    if (!out) return -(int)TB375FC_DIAG_POLICY;
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE)) return -(int)tb375fc_diag_last_error();
    if (!try_lock(&io_lock)) return -(int)TB375FC_DIAG_BUSY;
    int result = __atomic_load_n(&enabled, __ATOMIC_ACQUIRE) ?
        transfer_block(lba, ticks() + frequency, 1) : -(int)tb375fc_diag_last_error();
    if (!result) {
        uint8_t *p = out;
        for (unsigned i = 0; i < BLOCK; i++) p[i] = bounce[i];
    }
    unlock(&io_lock);
    return result;
}

static void append(const char *prefix, const char *text, int newline)
{
    uint64_t irq = irq_save();
    if (!try_lock(&ring_lock)) {
        __atomic_fetch_add(&dropped, 1u, __ATOMIC_RELAXED);
        irq_restore(irq);
        return;
    }
    const char *strings[2] = { prefix, text };
    for (unsigned s = 0; s < 2; s++) {
        const char *p = strings[s];
        /* Bounded for panic/stage callers; arguments must be valid C strings. */
        for (unsigned n = 0; p && *p && n < 512u; n++) ring[total++ % STREAM_BYTES] = (uint8_t)*p++;
    }
    if (newline) ring[total++ % STREAM_BYTES] = '\n';
    unlock(&ring_lock);
    irq_restore(irq);
}

void tb375fc_diag_early_init(void)
{
    /* Boot CPU only, once, before any console producers. BSS is already zero. */
    boot_counter = ticks();
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    append("[tbdiag] ", "early RAM recorder", 1);
}

static void record_char(uint8_t *buffer, uint64_t *count, char c)
{
    uint64_t irq = irq_save();
    if (try_lock(&ring_lock)) {
        buffer[(*count)++ % STREAM_BYTES] = (uint8_t)c;
        unlock(&ring_lock);
    } else __atomic_fetch_add(&dropped, 1u, __ATOMIC_RELAXED);
    irq_restore(irq);
}

void tb375fc_diag_putc(char c)
{
    record_char(ring, &total, c);
}

void tb375fc_diag_msgbuf_putc(char c)
{
    /* Called under bsd_log_lock: RAM only, no printf, flush or blocking lock. */
    record_char(log_ring, &log_total, c);
}

/* Caller holds ring_lock; each section is chronological, not a merged timeline. */
static uint32_t snapshot_stream(uint32_t off, const char *title,
    const uint8_t *buffer, uint64_t count)
{
    while (*title) snapshot[off++] = (uint8_t)*title++;
    uint32_t len = count < STREAM_BYTES ? (uint32_t)count : STREAM_BYTES;
    for (uint32_t i = 0; i < len; i++)
        snapshot[off++] = buffer[(count - len + i) % STREAM_BYTES];
    return off;
}

int tb375fc_diag_flush(void)
{
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE)) return -(int)tb375fc_diag_last_error();
    if (!try_lock(&io_lock)) return -(int)TB375FC_DIAG_BUSY;
    if (!__atomic_load_n(&enabled, __ATOMIC_ACQUIRE)) {
        unlock(&io_lock);
        return -(int)tb375fc_diag_last_error();
    }
    uint64_t irq = irq_save();
    if (!try_lock(&ring_lock)) {
        irq_restore(irq); unlock(&io_lock);
        return -(int)TB375FC_DIAG_BUSY;
    }
    uint64_t count = total + log_total;
    int wrapped = total > STREAM_BYTES || log_total > STREAM_BYTES;
    uint32_t len = snapshot_stream(0, "--- console ---\n", ring, total);
    len = snapshot_stream(len, "\n--- msgbuf ---\n", log_ring, log_total);
    unlock(&ring_lock); irq_restore(irq);

    uint64_t now = ticks(), deadline = now + frequency;
    uint32_t next = seq + 1u, base = MRD_LBA + LOG_BLOCK + (next & 1u) * SLOT_BLOCKS;
    uint32_t previous_error = __atomic_load_n(&error_code, __ATOMIC_RELAXED);
    zero(bounce, BLOCK);
    int result = write_block(base, deadline); /* invalidate target, keep other slot intact */
    for (uint32_t off = 0; !result && off < len; off += BLOCK) {
        zero(bounce, BLOCK);
        uint32_t n = len - off < BLOCK ? len - off : BLOCK;
        for (uint32_t i = 0; i < n; i++) bounce[i] = snapshot[off + i];
        result = write_block(base + 1u + off / BLOCK, deadline);
    }
    if (!result) {
        zero(bounce, BLOCK);
        put32(bounce, TB375FC_DIAG_MAGIC); put32(bounce + 4, 1); put32(bounce + 8, next);
        put32(bounce + 12, elapsed_ms(now)); put32(bounce + 16, len);
        put32(bounce + 20, crc32(snapshot, len));
        put32(bounce + 24, (__atomic_load_n(&panic_seen, __ATOMIC_RELAXED) ? 1u : 0u) | (wrapped ? 2u : 0u));
        put32(bounce + 28, writes); put32(bounce + 32, errors);
        put32(bounce + 36, __atomic_load_n(&dropped, __ATOMIC_RELAXED));
        put32(bounce + 40, (uint32_t)count); put32(bounce + 44, (uint32_t)(count >> 32));
        put32(bounce + 48, (uint32_t)boot_counter); put32(bounce + 52, (uint32_t)(boot_counter >> 32));
        put32(bounce + 56, previous_error); put32(bounce + 60, crc32(bounce, 60));
        result = write_block(base, deadline); /* only now is this slot a valid record */
    }
    if (!result) { seq = next; __atomic_store_n(&error_code, 0u, __ATOMIC_RELAXED); }
    last_flush = ticks();
    unlock(&io_lock);
    return result;
}

int tb375fc_diag_vm_ready(void)
{
    if (__atomic_exchange_n(&vm_called, 1u, __ATOMIC_ACQ_REL))
        return fail(TB375FC_DIAG_STATE);
    if (!frequency || __atomic_load_n(&stopped, __ATOMIC_ACQUIRE))
        return fail(TB375FC_DIAG_NOT_READY);
    /* arm_vm_init has established kernel translation; ml_io_map can steal
     * early virtual space before STARTUP_SUB_KMEM (osfmk/arm/io_map.c). */
    hci = ml_io_map(UFS_PA, 0x3000u);
    if (!hci) return fail(TB375FC_DIAG_NOT_READY);
    if (!dma_extent(list, sizeof(list), &list_pa) ||
        !dma_extent(command, sizeof(command), &command_pa) ||
        !dma_extent(bounce, sizeof(bounce), &bounce_pa)) return fail(TB375FC_DIAG_DMA);
    __atomic_store_n(&error_code, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&enabled, 1u, __ATOMIC_RELEASE);
    append("[tbdiag] ", "VM ready; mrdump recorder enabled", 1);
    return tb375fc_diag_flush();
}

void tb375fc_diag_stage(const char *text)
{
    append("[stage] ", text, 1);
    (void)tb375fc_diag_flush();
}

void tb375fc_diag_panic(const char *text)
{
    __atomic_store_n(&panic_seen, 1u, __ATOMIC_RELAXED);
    append("[panic] ", text, 1);
    (void)tb375fc_diag_flush();
}

void tb375fc_diag_poll(void)
{
    /* Serialize the rate-limit read with flush's last_flush update. Flush
     * itself re-acquires the lock non-blockingly, so nested callbacks drop. */
    if (!frequency || !try_lock(&io_lock)) return;
    int due = ticks() - last_flush >= frequency;
    unlock(&io_lock);
    if (due) (void)tb375fc_diag_flush();
}

int tb375fc_diag_stop(void)
{
    if (!try_lock(&io_lock)) return -(int)TB375FC_DIAG_BUSY;
    __atomic_store_n(&stopped, 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&enabled, 0u, __ATOMIC_RELEASE);
    (void)fail(TB375FC_DIAG_STOPPED);
    unlock(&io_lock);
    return 0;
}

uint32_t tb375fc_diag_last_error(void)
{
    return __atomic_load_n(&error_code, __ATOMIC_RELAXED);
}
