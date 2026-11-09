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
#define WDT_PET_SECONDS 240u
static uintptr_t wdt;

void tb375fc_glue_init(void);
void tb375fc_glue_start(void);
void tb375fc_glue_stage(const char *);
void tb375fc_glue_putc(char);
void tb375fc_glue_msgbuf(char);
void tb375fc_glue_process(int, const char *, const char *);
void tb375fc_glue_exit(int, const char *, int, unsigned, uint64_t);
void tb375fc_glue_panic(int);
void tb375fc_glue_user_fault(int, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
void tb375fc_glue_critical_exit(int, const char *);
void tb375fc_glue_syscall(int, const char *, unsigned, int, int, const uint64_t *);
/* bsd/kern/copyio: user pointer -> kernel buffer, returns 0 or errno; safe on bad pointers. */
extern int copyin(uint64_t uaddr, void *kaddr, size_t len);
extern int copyinstr(uint64_t uaddr, void *kaddr, size_t len, size_t *done);

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

static char mline[192];
static unsigned mlen;
static uint32_t nag_lines;

static bool
line_has(const char *line, unsigned len, const char *needle)
{
	unsigned n = 0;
	while (needle[n]) {
		n++;
	}
	for (unsigned i = 0; i + n <= len; i++) {
		unsigned j = 0;
		while (j < n && line[i + j] == needle[j]) {
			j++;
		}
		if (j == n) {
			return true;
		}
	}
	return false;
}

void
tb375fc_glue_msgbuf(char c)
{
	mline[mlen++] = c;
	if (c != '\n' && mlen < sizeof(mline)) {
		return;
	}
	if (line_has(mline, mlen, "handleNagTimerExpiry") && nag_lines++ >= 2) {
		mlen = 0;
		return;
	}
	for (unsigned i = 0; i < mlen; i++) {
		tb375fc_diag_msgbuf_putc(mline[i]);
	}
	mlen = 0;
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
put_hex(uint64_t v)
{
	put_str("0x");
	for (int shift = 60; shift >= 0; shift -= 4) {
		tb375fc_diag_putc("0123456789abcdef"[(v >> shift) & 15]);
	}
}

/* User-mode breakpoints/aborts (sleh.c hooks): the first ones are the EL0 crash evidence. */
#define UFAULT_BUDGET 48u
static uint32_t ufault_lines;

void
tb375fc_glue_user_fault(int kind, uint64_t pc, uint64_t lr, uint64_t sp, uint64_t esr, uint64_t far)
{
	if (__atomic_fetch_add(&ufault_lines, 1, __ATOMIC_RELAXED) >= UFAULT_BUDGET) {
		return;
	}
	put_str(kind ? "[ufault] abort pc=" : "[ufault] brk pc=");
	put_hex(pc);
	put_str(" lr=");
	put_hex(lr);
	put_str(" sp=");
	put_hex(sp);
	put_str(" esr=");
	put_hex(esr);
	if (kind) {
		put_str(" far=");
		put_hex(far);
	}
	tb375fc_diag_putc('\n');
}

void
tb375fc_glue_critical_exit(int pid, const char *msg)
{
	put_str("[crit] pid ");
	put_dec(pid);
	put_str(": ");
	put_str(msg ? msg : "?");
	tb375fc_diag_putc('\n');
}

/*
 * BSD syscalls (bsd/dev/arm/systemcalls.c). Every pid prints its first SYS_PER_PID returns (the
 * first one proves EL0 ran), within a global budget so the console tail keeps earlier lines.
 * The syscall each pid is currently inside is kept and reported by the poll thread: that is the
 * wait point when a process stops making progress.
 */
#define SYS_PIDS    256
#define SYS_PER_PID 12u
#define SYS_BUDGET  420u
static uint16_t sys_inflight[SYS_PIDS];
static uint32_t sys_count[SYS_PIDS];
static char sys_name[SYS_PIDS][17];
static uint32_t sys_lines;
static uint64_t sys_total;

/*
 * __sysctl (202): name = int[namelen] MIB; sysctlbyname (274): name string. launchd sat in 202
 * forever in devloop 20261109T103639Z, so each call names what it asked for.
 */
#define SYSCTL_BUDGET 160u
static uint32_t sysctl_lines;

static void
trace_sysctl(int pid, const char *name, unsigned code, const uint64_t *args)
{
	if (__atomic_fetch_add(&sysctl_lines, 1, __ATOMIC_RELAXED) >= SYSCTL_BUDGET) {
		return;
	}
	put_str("[sysctl] ");
	put_str(name ? name : "?");
	tb375fc_diag_putc('[');
	put_dec(pid);
	put_str("] ");
	if (code == 202) {
		int mib[12];
		uint64_t n = args[1];
		if (n > 12) {
			n = 12;
		}
		if (n && copyin(args[0], mib, (size_t)n * sizeof(int)) == 0) {
			put_str("mib=");
			for (uint64_t i = 0; i < n; i++) {
				if (i) {
					tb375fc_diag_putc('.');
				}
				put_dec(mib[i]);
			}
		} else {
			put_str("mib=<copyin failed>");
		}
		put_str(" len=");
		put_dec((int64_t)args[1]);
		put_str(args[4] ? " set" : " get");
	} else {
		char str[64];
		size_t done = 0;
		if (copyinstr(args[0], str, sizeof(str), &done) == 0 || done) {
			str[sizeof(str) - 1] = 0;
			put_str("name=");
			put_str(str);
		} else {
			put_str("name=<copyin failed>");
		}
		put_str(args[4] ? " set" : " get");
	}
	tb375fc_diag_putc('\n');
}

void
tb375fc_glue_syscall(int pid, const char *name, unsigned code, int err, int phase, const uint64_t *args)
{
	if (pid < 0 || pid >= SYS_PIDS) {
		return;
	}
	if (phase == 0 && args && (code == 202 || code == 274)) {
		trace_sysctl(pid, name, code, args);
	}
	if (phase == 0) {
		sys_inflight[pid] = (uint16_t)(code + 1);
		__atomic_fetch_add(&sys_total, 1, __ATOMIC_RELAXED);
		if (name) {
			int i = 0;
			for (; i < 16 && name[i]; i++) {
				sys_name[pid][i] = name[i];
			}
			sys_name[pid][i] = 0;
		}
		return;
	}
	sys_inflight[pid] = 0;
	if (__atomic_fetch_add(&sys_count[pid], 1, __ATOMIC_RELAXED) >= SYS_PER_PID) {
		return;
	}
	if (__atomic_fetch_add(&sys_lines, 1, __ATOMIC_RELAXED) >= SYS_BUDGET) {
		return;
	}
	put_str("[sys] ");
	put_str(name ? name : "?");
	tb375fc_diag_putc('[');
	put_dec(pid);
	put_str("] ");
	put_dec(code);
	put_str(" err=");
	put_dec(err);
	tb375fc_diag_putc('\n');
}

static void
report_waits(unsigned seconds)
{
	unsigned shown = 0;
	put_str("[wait] t=");
	put_dec(seconds);
	put_str(" syscalls=");
	put_dec((int64_t)sys_total);
	tb375fc_diag_putc('\n');
	for (int pid = 0; pid < SYS_PIDS && shown < 24; pid++) {
		unsigned c = sys_inflight[pid];
		if (!c && !sys_count[pid]) {
			continue;
		}
		put_str("[wait]   ");
		put_str(sys_name[pid][0] ? sys_name[pid] : "?");
		tb375fc_diag_putc('[');
		put_dec(pid);
		put_str("] n=");
		put_dec(sys_count[pid]);
		if (c) {
			put_str(" in=");
			put_dec(c - 1);
		}
		tb375fc_diag_putc('\n');
		shown++;
	}
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
		if (seconds && seconds % 30 == 0) {
			report_waits(seconds);
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
