/*
 * cbqri_experiment.c — one-file bandwidth-regulation experiment for the
 * CBQRI quad-BOOM FireSim SoC (crsullivan13/chipyard@cbqri, CBQRIQuadBoom16Config).
 *
 * Everything the experiment does is in this file: resctrl setup, the
 * RCID/MCID-to-core pinning, the victim and attacker kernels, the sweeps, the
 * measurements and the CSV output. No debugfs writes; the hardware is driven
 * purely through /sys/fs/resctrl, i.e. the same interface x86 uses.
 *
 * ---------------------------------------------------------------------------
 * 1. THE HARDWARE (what the two resctrl "MB" domains are)
 * ---------------------------------------------------------------------------
 *   MB:0  core->LLC regulator  (memory-traffic-controllers, MMIO 0x20000000)
 *         Sits at the LLC's input. Counts every 64-byte line request a core
 *         sends to the LLC (hits and misses). When a group is over budget its
 *         requests are held at the LLC door, in the core's own miss buffers.
 *   MB:1  LLC->DRAM regulator  (inclusive-cache, MMIO 0x21000000)
 *         Sits at the LLC's output to memory. Counts lines the LLC fetches
 *         from DRAM. When a group is over budget its refills are held at the
 *         exit, AFTER they have taken one of the LLC's shared miss-handling
 *         entries (24 per bank). A capped group can therefore pin all entries
 *         and stall everyone else: head-of-line blocking. Measured 2026-09-16:
 *         capping attackers to 1% on MB:1 made an uncapped victim 10-30x slower.
 *
 *   Budget semantics: a schemata value P (percent) becomes
 *         rbwb = P * 65535 / 100 line fills per BANK per regulation PERIOD.
 *   The kernel programs the period to 1,000,000 regulator cycles (~1 ms at the
 *   regulator's 1 GHz), so on MB:0 with 2 LLC banks:
 *         cap(MB/s) ~= P/100 * 65535 * 2 banks * 64 B / 1 ms = P * 83.9 MB/s
 *   e.g. 5% -> ~419 MB/s, 1% -> ~84 MB/s (measured 413 and 79).
 *   MB:1 has 4x more (DRAM) banks so the same P binds ~4x later.
 *   The hardware accepts at most 80% (mrbwb). 100% is rejected by the kernel.
 *
 * ---------------------------------------------------------------------------
 * 2. RCID / MCID PINNING (which core carries which id)
 * ---------------------------------------------------------------------------
 *   resctrl's "control group" = one RCID (closid) + one MCID (rmid).
 *   The kernel hands out ids in creation order: root = 0, then 1, 2, 3, ...
 *   We create four groups and assign ONE CORE to each through its "cpus" file,
 *   so any task running on that core carries the group's ids in srmcfg:
 *
 *       core 0 -> group "vic"  -> RCID 1 / MCID 1   (the VICTIM core)
 *       core 1 -> group "att1" -> RCID 2 / MCID 2   (attacker 1)
 *       core 2 -> group "att2" -> RCID 3 / MCID 3   (attacker 2)
 *       core 3 -> group "att3" -> RCID 4 / MCID 4   (attacker 3)
 *
 *   Every process in this experiment is pinned to its core with
 *   sched_setaffinity(), so core == group == RCID/MCID. (With the 16-RCID
 *   bitstream there are ids to spare; the 2-RCID dual build could not do this.)
 *   Verification: if the read-only debugfs module is present, the program
 *   prints the per-CPU srmcfg register so the mapping can be checked by eye.
 *
 * ---------------------------------------------------------------------------
 * 3. THE PROGRAMS (how bandwidth and latency are measured)
 * ---------------------------------------------------------------------------
 *   Victim kinds (run on core 0, timed):
 *     "stream"  Streaming reads: one 8-byte load per 64-byte line over a
 *               16 MiB buffer, 4 passes = 64 MiB of line fills. Reports MB/s.
 *               Bandwidth-bound: the out-of-order core keeps many misses in
 *               flight, so this measures achievable throughput.
 *     "pchase"  Pointer chase: one dependent load at a time around a random
 *               cycle of cache lines in a 64 MiB buffer (>> 512 KiB LLC),
 *               400,000 loads. Reports ns/load. Latency-bound: every load
 *               waits for the previous one, so this measures memory latency
 *               as seen by a latency-sensitive program.
 *   Attacker kinds (run continuously on cores 1-3 until killed):
 *     "pchase8" Pointer chase with 8 INDEPENDENT chains over 32 MiB. The 8
 *               chains give the core 8 outstanding misses at once (MLP), which
 *               is what makes a pointer chaser hurt a neighbour; a single
 *               chain barely loads the memory system. ~1500 MB/s each alone.
 *     "stream"  Streaming reads over 16 MiB, endless passes. ~3300 MB/s alone.
 *   Buffers are allocated and initialised ONCE before the sweeps, so timed
 *   regions contain only the memory traffic of interest.
 *
 *   Three independent bandwidth measurements are recorded per run:
 *     (a) program-side: bytes touched / wall time of the victim's timed loop
 *         (vic_MBps) and, for pchase, time per dependent load (vic_ns_per_load);
 *     (b) hardware-side: resctrl mbm_total_bytes per group on BOTH domains
 *         (64 B x fills counted by the regulator for that group's MCID), read
 *         immediately before and after the victim's timed loop -> MB/s
 *         "during the victim run" for the victim and for the attackers;
 *     (c) attacker-side: each attacker counts its own accesses in shared
 *         memory; sampled around the victim run -> attackers' achieved MB/s
 *         and ns per access under the current cap.
 *
 * ---------------------------------------------------------------------------
 * 4. THE EXPERIMENTS (the sweeps)
 * ---------------------------------------------------------------------------
 *   Caps are "MB:0=L;1=D" (L = core->LLC percent, D = LLC->DRAM percent);
 *   80 means "not binding" (the hardware maximum).
 *   E0  victim alone,      victim cap L in {80,40,20,10,5,2,1}, D=80   (own-cap curve)
 *   E0d victim alone,      victim cap L=80, D in {80,20,5,1}            (DRAM-side scale)
 *   E1  3 pchase8 attackers, attackers' L in {80..1}, victim 80/80    (isolation by MB:0)
 *   E2  3 stream  attackers, attackers' L in {80..1}, victim 80/80
 *   E3  3 pchase8 attackers, attackers' D in {80,20,5,1}, victim 80/80  (MB:1: expect harm)
 *   E4  3 pchase8 attackers uncapped, victim's own L in {80..1}         (victim cap under attack)
 *   E5  1 and 2 pchase8 attackers uncapped, victim 80/80               (more/fewer attackers)
 *   Each with both victim kinds.
 *
 * ---------------------------------------------------------------------------
 * 5. OUTPUT
 * ---------------------------------------------------------------------------
 *   Every result is one line on stdout starting with "CSV," (so it can be
 *   pulled out of the FireSim uartlog with grep) and is also appended to
 *   /root/cbqri_results.csv. Columns:
 *     exp, victim, attacker, natt, vic_llc, vic_dram, att_llc, att_dram,
 *     vic_time_s, vic_MBps, vic_ns_per_load,
 *     vic_mon_llc_MBps, vic_mon_dram_MBps,     (hardware counters, victim group)
 *     att_mon_llc_MBps, att_mon_dram_MBps,     (hardware counters, all attackers)
 *     att_self_MBps, att_ns_per_access         (attackers' own counters)
 *
 * Build:  riscv64-unknown-linux-gnu-gcc -O2 -static -o cbqri_experiment cbqri_experiment.c
 * Run:    as root on the target. "--dry" runs the kernels without resctrl (host test).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>

/* ------------------------------------------------------------------ config */
#define RESCTRL      "/sys/fs/resctrl"
#define CSV_PATH     "/root/cbqri_results.csv"
#define NCPU         4
#define LINE_BYTES   64
#define VICTIM_CPU   0
#define MAX_ATT      3
static const int   ATT_CPU[MAX_ATT]   = {1, 2, 3};
static const char *GROUP[NCPU]        = {"vic", "att1", "att2", "att3"};   /* index = cpu */

static const int LLC_SWEEP[]  = {80, 40, 20, 10, 5, 2, 1};
static const int DRAM_SWEEP[] = {80, 20, 5, 1};
#define NL (int)(sizeof LLC_SWEEP / sizeof *LLC_SWEEP)
#define ND (int)(sizeof DRAM_SWEEP / sizeof *DRAM_SWEEP)

#define VIC_STREAM_MB     16
#define VIC_STREAM_PASSES 4
#define VIC_PCHASE_MB     64
#define VIC_PCHASE_LOADS  400000
#define ATT_PCHASE_MB     32
#define ATT_PCHASE_CHAINS 8
#define ATT_STREAM_MB     16

/* ---------------------------------------------------------------- helpers */
static int dry;                                   /* --dry: no resctrl/monitors */
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint64_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }
static void die(const char *m) { perror(m); exit(1); }

static int write_str(const char *path, const char *s) {
    int fd = open(path, O_WRONLY); if (fd < 0) return -1;
    ssize_t n = write(fd, s, strlen(s)); int e = errno; close(fd);
    if (n < 0) { errno = e; return -1; } return 0;
}
static uint64_t read_u64(const char *path) {
    char b[64] = {0}; int fd = open(path, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, b, sizeof b - 1); close(fd); return n > 0 ? strtoull(b, 0, 10) : 0;
}
static void pin(int cpu) {
    cpu_set_t s; CPU_ZERO(&s); CPU_SET(cpu, &s);
    if (sched_setaffinity(0, sizeof s, &s)) die("sched_setaffinity");
}

/* ---------------------------------------------------- resctrl plumbing */
static void group_path(char *out, size_t n, const char *g, const char *file) { snprintf(out, n, RESCTRL "/%s/%s", g, file); }

/* Set a group's caps: L on MB:0 (core->LLC), D on MB:1 (LLC->DRAM). */
static void set_cap(const char *g, int L, int D) {
    if (dry) return;
    char p[256], v[64]; group_path(p, sizeof p, g, "schemata");
    snprintf(v, sizeof v, "MB:0=%d;1=%d\n", L, D);
    if (write_str(p, v)) { fprintf(stderr, "schemata %s <- %s failed: %s\n", g, v, strerror(errno)); exit(1); }
    usleep(3000);   /* > 1 regulation period, so the new budget is in force */
}
/* mbm_total_bytes of group g on domain dom (0 = core->LLC, 1 = LLC->DRAM) */
static uint64_t mon(const char *g, int dom) {
    if (dry) return 0;
    char p[256]; snprintf(p, sizeof p, RESCTRL "/%s/mon_data/mon_L3_%02d/mbm_total_bytes", g, dom);
    return read_u64(p);
}

static void resctrl_setup(void) {
    if (dry) return;
    if (mount("resctrl", RESCTRL, "resctrl", 0, NULL) && errno != EBUSY) die("mount resctrl");
    printf("resctrl: num_closids=%llu num_rmids=%llu\n",
           (unsigned long long)read_u64(RESCTRL "/info/MB/num_closids"),
           (unsigned long long)read_u64(RESCTRL "/info/L3_MON/num_rmids"));
    for (int cpu = 0; cpu < NCPU; cpu++) {
        char p[256], v[16];
        snprintf(p, sizeof p, RESCTRL "/%s", GROUP[cpu]);
        if (mkdir(p, 0755) && errno != EEXIST) { fprintf(stderr, "mkdir %s: %s\n", p, strerror(errno)); exit(1); }
        set_cap(GROUP[cpu], 80, 80);
        /* pin the core to the group: everything on this core gets the group's RCID/MCID */
        group_path(p, sizeof p, GROUP[cpu], "cpus");
        snprintf(v, sizeof v, "%x\n", 1u << cpu);
        if (write_str(p, v)) { fprintf(stderr, "cpus %s: %s\n", p, strerror(errno)); exit(1); }
        group_path(p, sizeof p, GROUP[cpu], "cpus_list");
        char list[32] = {0}; int fd = open(p, O_RDONLY); if (fd >= 0) { ssize_t n = read(fd, list, 31); if (n > 0) list[n-1] = 0; close(fd); }
        printf("pin: core %d -> group %-4s -> RCID/MCID %d (cpus_list=%s)\n", cpu, GROUP[cpu], cpu + 1, list);
    }
    /* optional read-only verification of the CSR each core carries */
    mount("none", "/sys/kernel/debug", "debugfs", 0, NULL);
    if (system("modprobe cbqri_bandwidth_driver 2>/dev/null") == 0) {
        FILE *f = fopen("/sys/kernel/debug/cbqri/per_cpu_srmcfg", "r");
        if (f) { char l[64]; while (fgets(l, sizeof l, f)) printf("verify srmcfg %s", l); fclose(f); }
    }
}
static void resctrl_teardown(void) {
    if (dry) return;
    for (int cpu = 0; cpu < NCPU; cpu++) { char p[256]; group_path(p, sizeof p, GROUP[cpu], "cpus"); write_str(p, "0\n"); }
    umount(RESCTRL);
}

/* ------------------------------------------------ memory kernels (shared) */
struct line { uint64_t next; uint64_t pad[7]; };          /* exactly one 64 B cache line */

/* Random single cycle through all lines (Sattolo), so a chase visits every
 * line once per lap in an unpredictable order: no prefetcher help, every
 * load is a miss when the buffer exceeds the caches. */
static struct line *make_cycle(size_t mb) {
    size_t n = (mb << 20) / sizeof(struct line);
    struct line *a = malloc(n * sizeof *a); if (!a) die("malloc");
    for (size_t i = 0; i < n; i++) a[i].next = i;
    for (size_t i = n - 1; i > 0; i--) { size_t j = rnd() % i; uint64_t t = a[i].next; a[i].next = a[j].next; a[j].next = t; }
    return a;
}
static uint64_t *make_stream(size_t mb) {
    uint64_t *a = malloc(mb << 20); if (!a) die("malloc"); memset(a, 1, mb << 20); return a;
}

/* ------------------------------------------------------------- attackers */
struct att_stat { volatile uint64_t accesses; volatile uint64_t pad[7]; };   /* own cache line */
static struct att_stat *att_stat;                     /* shared (mmap) */
static pid_t att_pid[MAX_ATT];
static int   att_n;
static volatile sig_atomic_t stop;
static void on_term(int s) { (void)s; stop = 1; }

/* Attacker body: runs on its core until SIGTERM, counting its accesses. */
static void attacker(int idx, int cpu, const char *kind) {
    pin(cpu);
    signal(SIGTERM, on_term);
    if (!strcmp(kind, "pchase8")) {
        struct line *a = make_cycle(ATT_PCHASE_MB);
        size_t n = (ATT_PCHASE_MB << 20) / sizeof *a;
        uint64_t p[ATT_PCHASE_CHAINS];
        for (int c = 0; c < ATT_PCHASE_CHAINS; c++) p[c] = (n / ATT_PCHASE_CHAINS) * c;
        while (!stop) {
            for (int k = 0; k < 256; k++)
                for (int c = 0; c < ATT_PCHASE_CHAINS; c++) p[c] = a[p[c]].next;   /* 8 independent misses in flight */
            att_stat[idx].accesses += 256 * ATT_PCHASE_CHAINS;
        }
        if (p[0] == 0xdeadbeef) puts("x");        /* keep p live */
    } else {                                      /* stream */
        uint64_t *a = make_stream(ATT_STREAM_MB), s = 0;
        size_t words = (ATT_STREAM_MB << 20) / 8;
        while (!stop) {
            for (size_t i = 0; i < words; i += 8) s += a[i];
            att_stat[idx].accesses += words / 8;
        }
        if (s == 0xdeadbeef) puts("x");
    }
    _exit(0);
}
static void start_attackers(int n, const char *kind) {
    att_n = n;
    for (int i = 0; i < n; i++) {
        att_stat[i].accesses = 0;
        pid_t p = fork(); if (p < 0) die("fork");
        if (p == 0) attacker(i, ATT_CPU[i], kind);
        att_pid[i] = p;
    }
    /* let them finish building their tables and reach steady state */
    for (int i = 0; i < n; i++) while (att_stat[i].accesses == 0) usleep(1000);
    usleep(20000);
}
static void stop_attackers(void) {
    for (int i = 0; i < att_n; i++) kill(att_pid[i], SIGTERM);
    for (int i = 0; i < att_n; i++) waitpid(att_pid[i], NULL, 0);
    att_n = 0;
}

/* --------------------------------------------------------------- victims */
static uint64_t     *vic_stream;
static struct line  *vic_chain;

struct result { double t, MBps, ns_per_load; };

static struct result victim_stream(void) {
    size_t words = (VIC_STREAM_MB << 20) / 8; uint64_t s = 0;
    double t0 = now();
    for (int p = 0; p < VIC_STREAM_PASSES; p++)
        for (size_t i = 0; i < words; i += 8) s += vic_stream[i];     /* one load per line */
    double dt = now() - t0;
    if (s == 0xdeadbeef) puts("x");
    return (struct result){ dt, (double)(VIC_STREAM_MB << 20) * VIC_STREAM_PASSES / dt / 1e6, 0 };
}
static struct result victim_pchase(void) {
    uint64_t p = 0;
    double t0 = now();
    for (int i = 0; i < VIC_PCHASE_LOADS; i++) p = vic_chain[p].next;  /* strictly dependent */
    double dt = now() - t0;
    if (p == 0xdeadbeef) puts("x");
    return (struct result){ dt, (double)VIC_PCHASE_LOADS * LINE_BYTES / dt / 1e6, dt / VIC_PCHASE_LOADS * 1e9 };
}

/* ------------------------------------------------------- one measurement */
static FILE *csv;
static void run(const char *exp, const char *vkind, const char *akind, int natt,
                int vic_L, int vic_D, int att_L, int att_D)
{
    set_cap(GROUP[VICTIM_CPU], vic_L, vic_D);
    for (int i = 0; i < MAX_ATT; i++) set_cap(GROUP[ATT_CPU[i]], att_L, att_D);

    uint64_t v0[2], a0[2] = {0, 0}, s0 = 0;
    for (int d = 0; d < 2; d++) { v0[d] = mon(GROUP[VICTIM_CPU], d); for (int i = 0; i < natt; i++) a0[d] += mon(GROUP[ATT_CPU[i]], d); }
    for (int i = 0; i < natt; i++) s0 += att_stat[i].accesses;
    double t0 = now();

    struct result r = !strcmp(vkind, "stream") ? victim_stream() : victim_pchase();

    double dt = now() - t0;
    uint64_t v1[2], a1[2] = {0, 0}, s1 = 0;
    for (int d = 0; d < 2; d++) { v1[d] = mon(GROUP[VICTIM_CPU], d); for (int i = 0; i < natt; i++) a1[d] += mon(GROUP[ATT_CPU[i]], d); }
    for (int i = 0; i < natt; i++) s1 += att_stat[i].accesses;

    double vm0 = (v1[0] - v0[0]) / dt / 1e6, vm1 = (v1[1] - v0[1]) / dt / 1e6;
    double am0 = (a1[0] - a0[0]) / dt / 1e6, am1 = (a1[1] - a0[1]) / dt / 1e6;
    double aself = (s1 - s0) * (double)LINE_BYTES / dt / 1e6;
    double ans   = natt && s1 > s0 ? dt / ((s1 - s0) / (double)natt) * 1e9 : 0;   /* per attacker */

    char line[512];
    snprintf(line, sizeof line, "%s,%s,%s,%d,%d,%d,%d,%d,%.4f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f",
             exp, vkind, natt ? akind : "none", natt, vic_L, vic_D, natt ? att_L : 0, natt ? att_D : 0,
             r.t, r.MBps, r.ns_per_load, vm0, vm1, am0, am1, aself, ans);
    printf("CSV,%s\n", line); fflush(stdout);
    if (csv) { fprintf(csv, "%s\n", line); fflush(csv); }
}

/* ------------------------------------------------------------------ main */
static const char *VK[2] = {"stream", "pchase"};

int main(int argc, char **argv) {
    dry = argc > 1 && !strcmp(argv[1], "--dry");
    setvbuf(stdout, NULL, _IOLBF, 0);
    pin(VICTIM_CPU);
    att_stat = mmap(NULL, sizeof *att_stat * MAX_ATT, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (att_stat == MAP_FAILED) die("mmap");

    printf("==================== CBQRI BANDWIDTH-REGULATION EXPERIMENT ====================\n");
    printf("victim core %d; attacker cores 1-3; caps written via resctrl schemata only\n", VICTIM_CPU);
    resctrl_setup();
    if (!dry) csv = fopen(CSV_PATH, "w");
    const char *hdr = "exp,victim,attacker,natt,vic_llc,vic_dram,att_llc,att_dram,vic_time_s,vic_MBps,vic_ns_per_load,"
                      "vic_mon_llc_MBps,vic_mon_dram_MBps,att_mon_llc_MBps,att_mon_dram_MBps,att_self_MBps,att_ns_per_access";
    printf("CSV,%s\n", hdr); if (csv) fprintf(csv, "%s\n", hdr);

    /* victim buffers, built once */
    vic_stream = make_stream(VIC_STREAM_MB);
    vic_chain  = make_cycle(VIC_PCHASE_MB);

    /* E0: victim alone, its own cap swept on the core->LLC side */
    for (int v = 0; v < 2; v++) for (int i = 0; i < NL; i++) run("E0_alone_vicLLC", VK[v], "none", 0, LLC_SWEEP[i], 80, 80, 80);
    /* E0d: victim alone, its own cap swept on the LLC->DRAM side */
    for (int v = 0; v < 2; v++) for (int i = 0; i < ND; i++) run("E0d_alone_vicDRAM", VK[v], "none", 0, 80, DRAM_SWEEP[i], 80, 80);

    /* E1: 3 pointer-chase attackers, their core->LLC cap swept */
    start_attackers(3, "pchase8");
    for (int v = 0; v < 2; v++) for (int i = 0; i < NL; i++) run("E1_3pchase_attLLC", VK[v], "pchase8", 3, 80, 80, LLC_SWEEP[i], 80);
    /* E3: same attackers, their LLC->DRAM cap swept (expect the victim to get WORSE) */
    for (int v = 0; v < 2; v++) for (int i = 0; i < ND; i++) run("E3_3pchase_attDRAM", VK[v], "pchase8", 3, 80, 80, 80, DRAM_SWEEP[i]);
    /* E4: attackers uncapped, the victim's own core->LLC cap swept */
    for (int v = 0; v < 2; v++) for (int i = 0; i < NL; i++) run("E4_3pchase_vicLLC", VK[v], "pchase8", 3, LLC_SWEEP[i], 80, 80, 80);
    stop_attackers();

    /* E5: fewer attackers, uncapped */
    for (int n = 1; n <= 2; n++) { start_attackers(n, "pchase8"); for (int v = 0; v < 2; v++) run("E5_npchase_uncapped", VK[v], "pchase8", n, 80, 80, 80, 80); stop_attackers(); }

    /* E2: 3 streaming attackers, their core->LLC cap swept */
    start_attackers(3, "stream");
    for (int v = 0; v < 2; v++) for (int i = 0; i < NL; i++) run("E2_3stream_attLLC", VK[v], "stream", 3, 80, 80, LLC_SWEEP[i], 80);
    stop_attackers();

    if (csv) fclose(csv);
    resctrl_teardown();
    printf("==================== experiment done ====================\n");
    return 0;
}
