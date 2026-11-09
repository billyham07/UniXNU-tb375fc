/*
 * TB375FC: the boot_progress hook points (patches/xnu-12377/tb375fc-diag-hooks.patch, DEVELOPMENT
 * kernel only) feed the mrdump recorder Tb375fcDiag.c. This file holds no UFS code; it only decides
 * when the recorder is called:
 *   boot_progress_init (after PE_init_platform)  -> tb375fc_diag_early_init
 *   stage "arm_vm_init returned"                 -> tb375fc_diag_vm_ready (once)
 *   every stage                                  -> tb375fc_diag_stage (RAM + flush)
 *   _cnputc tap                                  -> tb375fc_diag_putc (console unchanged)
 *   log_putc_locked(msgbufp) tap                  -> tb375fc_diag_msgbuf_putc (separate RAM tail)
 *   exec stages / process exits                  -> formatted lines through putc (flushed by poll)
 *   boot_progress_start (after clock_service_create) -> a thread calling tb375fc_diag_poll each second
 *   panic: after SavePanicInfo (1), panic_spin_forever (2) -> paniclog tail, then tb375fc_diag_panic
 */
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <mach/mach_types.h>
#include <kern/thread.h>
#include "Tb375fcDiag.h"

extern void IOSleep(unsigned milliseconds);
extern char *debug_buf_base, *debug_buf_ptr;
/* osfmk/kern/debug.c: the panic being handled; bsd/kern/subr_prf.c: vsnprintf. With no panic region
 * (gPanicBase unset on this board) the paniclog stays empty, so the string is formatted here. */
extern const char *debugger_panic_str, *debugger_message;
extern va_list *debugger_panic_args;
extern unsigned long debugger_panic_caller;
extern int vsnprintf(char *, size_t, const char *, va_list);
extern uintptr_t ml_io_map(uintptr_t pa, uintptr_t size);

/*
 * Watchdog lifetime. stage0 armed TOPRGU (~30 s, LK's mode bits); UEFI's Tb375fcDiagDxe pets it until
 * ExitBootServices. Without a kernel pet the run ends ~30 s later, inside IOKitInitializeTime's fixed
 * 30 s wait for IORTC (no RTC driver yet; devloop 20261108T163643Z). The poll thread writes only the
 * RESTART key (as Tb375fcDiagDxe; never the mode register) for WDT_PET_SECONDS, then stops, so a hung
 * kernel still resets to LOS.
 */
#define WDT_PA          0x1C00A000u
#define WDT_RESTART     0x08u
#define WDT_RESTART_KEY 0x1971u
#define WDT_PET_SECONDS 120u
static uintptr_t wdt;

void tb375fc_glue_init(void);
void tb375fc_glue_start(void);
void tb375fc_glue_stage(const char *);
void tb375fc_glue_putc(char);
void tb375fc_glue_msgbuf(char);
void tb375fc_glue_process(int, const char *, const char *);
void tb375fc_glue_exit(int, const char *, int, unsigned, uint64_t);
void tb375fc_glue_panic(int);

#define PANIC_TAIL 16384u

static uint32_t vm_done, thread_started, panic_calls;

static bool
same(const char *a, const char *b)
{
	while (*a && *a == *b) {
		a++;
		b++;
	}
	return *a == *b;
}

static void
put_str(const char *s)
{
	for (unsigned n = 0; s && *s && n < 256; n++) {
		tb375fc_diag_putc(*s++);
	}
}

static void
put_dec(int64_t v)
{
	char buf[24];
	unsigned n = 0;
	uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
	if (v < 0) {
		tb375fc_diag_putc('-');
	}
	do {
		buf[n++] = (char)('0' + u % 10);
		u /= 10;
	} while (u && n < sizeof(buf));
	while (n) {
		tb375fc_diag_putc(buf[--n]);
	}
}

void
tb375fc_glue_init(void)
{
	tb375fc_diag_early_init();
}

void
tb375fc_glue_stage(const char *s)
{
	/*
	 * Breadcrumb that survives the reset without UFS: pet TOPRGU at every stage from arm_vm_init on.
	 * The return to LOS comes ~30 s after the last pet, so its time says how far the kernel got
	 * (147.5 s with no kernel pet at all; devloop 20261108T165010Z and the other no-log runs).
	 */
	if (!vm_done && same(s, "arm_vm_init returned")) {
		wdt = ml_io_map(WDT_PA, 0x1000);
	}
	if (wdt && !thread_started) {
		*(volatile uint32_t *)(wdt + WDT_RESTART) = WDT_RESTART_KEY;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	tb375fc_diag_stage(s);
	if (!vm_done && same(s, "arm_vm_init returned")) {
		vm_done = 1;
		int e = tb375fc_diag_vm_ready();
		if (e) {
			put_str("[tbdiag] vm_ready error ");
			put_dec(e);
			tb375fc_diag_putc('\n');
		}
	}
}

void
tb375fc_glue_putc(char c)
{
	tb375fc_diag_putc(c);
}

void
tb375fc_glue_msgbuf(char c)
{
	tb375fc_diag_msgbuf_putc(c);
}

void
tb375fc_glue_process(int pid, const char *name, const char *stage)
{
	put_str("[exec] ");
	put_str(name);
	tb375fc_diag_putc('[');
	put_dec(pid);
	put_str("] ");
	put_str(stage);
	tb375fc_diag_putc('\n');
}

void
tb375fc_glue_exit(int pid, const char *name, int rv, unsigned ns, uint64_t code)
{
	put_str("[exit] ");
	put_str(name);
	tb375fc_diag_putc('[');
	put_dec(pid);
	put_str("] rv=");
	put_dec(rv);
	put_str(" reason=");
	put_dec(ns);
	tb375fc_diag_putc('/');
	put_dec((int64_t)code);
	tb375fc_diag_putc('\n');
}

static void
poll_thread(__unused void *param, __unused wait_result_t wr)
{
	for (unsigned seconds = 0;; seconds++) {
		if (wdt && seconds < WDT_PET_SECONDS) {
			*(volatile uint32_t *)(wdt + WDT_RESTART) = WDT_RESTART_KEY;
			__asm__ volatile ("dsb sy" ::: "memory");
		} else if (wdt && seconds == WDT_PET_SECONDS) {
			tb375fc_diag_stage("tbdiag watchdog no longer petted");
		}
		IOSleep(1000);
		tb375fc_diag_poll();
	}
}

void
tb375fc_glue_start(void)
{
	thread_t th;
	if (thread_started++) {
		return;
	}
	if (!wdt) {
		wdt = ml_io_map(WDT_PA, 0x1000);
	}
	tb375fc_diag_stage(wdt ? "tbdiag watchdog petted for 120 s" : "tbdiag watchdog map FAILED");
	if (kernel_thread_start_priority(poll_thread, NULL, 95, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
		tb375fc_diag_stage("tbdiag poll thread started");
	} else {
		tb375fc_diag_stage("tbdiag poll thread FAILED");
	}
}

void
tb375fc_glue_panic(int phase)
{
	if (panic_calls++ >= 3) {
		return;
	}
	if (panic_calls == 1) {
		static char text[1024];
		put_str("\n--- panic ---\ncaller 0x");
		for (int shift = 60; shift >= 0; shift -= 4) {
			tb375fc_diag_putc("0123456789abcdef"[(debugger_panic_caller >> shift) & 15]);
		}
		tb375fc_diag_putc('\n');
		if (debugger_message) {
			put_str("message: ");
			put_str(debugger_message);
			tb375fc_diag_putc('\n');
		}
		if (debugger_panic_str) {
			text[0] = 0;
			if (debugger_panic_args) {
				va_list ap;
				va_copy(ap, *debugger_panic_args);
				vsnprintf(text, sizeof(text), debugger_panic_str, ap);
				va_end(ap);
			}
			const char *s = text[0] ? text : debugger_panic_str;
			for (unsigned n = 0; *s && n < sizeof(text); n++) {
				tb375fc_diag_putc(*s++);
			}
			tb375fc_diag_putc('\n');
		}
	}
	if (phase == 1 || panic_calls == 1) {
		const char *b = debug_buf_base, *e = debug_buf_ptr;
		if (b && e > b && (uintptr_t)(e - b) < (16u << 20)) {
			if ((uintptr_t)(e - b) > PANIC_TAIL) {
				b = e - PANIC_TAIL;
			}
			put_str("\n--- paniclog ---\n");
			while (b < e) {
				tb375fc_diag_putc(*b ? *b : ' ');
				b++;
			}
			tb375fc_diag_putc('\n');
		}
	}
	tb375fc_diag_panic(phase == 1 ? "paniclog written" : "panic_spin_forever");
}
