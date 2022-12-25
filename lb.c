/* $Id: lb.c 795 2013-04-22 05:12:27Z aqua $
 *
 * lookbusy -- a tool for producing synthetic CPU, memory consumption and
 *             disk loads on a host.
 *
 * Copyright (c) 2006, Devin Carraway <lookbusy@devin.com>.
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 *
 */

/* and, redundantly ... */
static const char *copyright =
"Copyright (c) 2006-2008, by Devin Carraway <lookbusy@devin.com>\n"
"$Id: lb.c 795 2013-04-22 05:12:27Z aqua $\n"
"\n"
"This program is free software; you can redistribute it and/or modify\n"
"it under the terms of the GNU General Public License as published by\n"
"the Free Software Foundation; either version 2 of the License, or\n"
"(at your option) any later version.\n"
"\n"
"This program is distributed in the hope that it will be useful,\n"
"but WITHOUT ANY WARRANTY; without even the implied warranty of\n"
"MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n"
"GNU General Public License for more details.\n"
"\n"
"You should have received a copy of the GNU General Public License\n"
"along with this program; if not, write to the Free Software\n"
"Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA\n"
"";

#ifdef HAVE_CONFIG_H
#include "config.h"
#else
#define VERSION "1.4"
#define PACKAGE "lookbusy"
#endif

#include <stdio.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <stdlib.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#define _GNU_SOURCE
#include <getopt.h>
#ifdef HAVE_STRING_H
#include <string.h>
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <inttypes.h>
#include <ctype.h>
#include <regex.h>
#include <stdarg.h>
#include <math.h>

#ifndef HAVE_STRTOL
#define strtol(x,e,b) atol(x)
#endif

#ifndef HAVE_STRTOLL
#define strtoll(x,e,b) atol(x)
#endif

#ifdef HAVE_SYSCONF
#define LB_PAGE_SIZE sysconf(_SC_PAGESIZE)
#else
#define LB_PAGE_SIZE 4096
#endif

#ifdef _FILE_OFFSET_BITS
#if _FILE_OFFSET_BITS == 64
#define HAVE_LFS 1
#endif
#else
#define HAVE_LFS (sizeof(off_t) >= 8)
#endif

#define PI 3.14159265358979323846

enum cpu_util_mode {
    UTIL_MODE_FIXED = 0,
    UTIL_MODE_CURVE
};
enum cpu_util_mode c_cpu_util_mode = UTIL_MODE_FIXED;

static int utc = 0;

static int c_cpu_curve_period = 86400; /* seconds */
static int c_cpu_curve_peak = 60 * 60 * 13; /* 1PM local time */

static int c_cpu_util_l = 50, c_cpu_util_h = 50; /* percent */
static size_t c_mem_util = 0; /* bytes */
static long c_mem_stir_sleep = 1000; /* 1000 usec / 1 ms */
static off_t c_disk_util = 0; /* MB */
static char **c_disk_churn_paths;
static size_t c_disk_churn_paths_n;
static size_t c_disk_churn_block_size = 32 * 1024; /* bytes */
static size_t c_disk_churn_step_size = 4 * 1024; /* bytes */
static long c_disk_churn_sleep = 100; /* ms */

static int ncpus = -1; /* autodetect */

static int verbosity = 1;

static char *mem_stir_buffer;

static pid_t *cpu_pids;
static size_t n_cpu_pids;
static pid_t *disk_pids;
static size_t n_disk_pids;
static pid_t mem_pid;

typedef void (*spinner_fn)(long long, long long, long long, void*, void *);

static int say(int level, const char *fmt, ...)
{
    va_list ap;
    int r;

    if (level > verbosity) return 0;
    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}

static int err(const char *fmt, ...)
{
    va_list ap;
    int r;

    va_start(ap, fmt);
    r = vfprintf(stderr, fmt, ap);
    va_end(ap);
    return r;
}

static int parse_timespan(const char *str, int *r)
{
    regex_t ex;
    int e;
    static const char *pattern =
        "^[[:space:]]*([0-9]+)([dhms]?)[[:space:]]*$";

    if ((e = regcomp(&ex,pattern, REG_EXTENDED)) != 0) {
        char errbuf[128];
        regerror(e, &ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        err("regcomp(%s): %s\n", pattern, errbuf);
        return -1;
    }
    regmatch_t matches[3];
    if ((e = regexec(&ex, str, 3, matches, 0)) != 0) {
        char errbuf[128];
        regerror(e, &ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        if (e != REG_NOMATCH)
            err("regexec: Couldn't match '%s' in '%s': %s\n",
                pattern, str, errbuf);
        return -1;
    }
    *r = (int)strtol(str + matches[1].rm_so, NULL, 10) *
           (matches[2].rm_so == -1 ? 1 :
            *(str + matches[2].rm_so) == 'd' ? 86400 : 
            *(str + matches[2].rm_so) == 'h' ? 3600 : 
            *(str + matches[2].rm_so) == 'm' ? 60 : 1);
    return 0;
}

static int parse_large_size(const char *str, off_t *r)
{
    regex_t ex;
    int e;
    static const char *pattern = HAVE_LFS ?
        "^[[:space:]]*([0-9]+)(b|kb?|mb?|gb?|tb?)?[[:space:]]*$" :
        "^[[:space:]]*([0-9]+)(b|kb?|mb?|gb?)?[[:space:]]*$";

    if ((e = regcomp(&ex,pattern, REG_EXTENDED | REG_ICASE)) != 0) {
        char errbuf[128];
        regerror(e, &ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        err("regcomp(%s): %s\n", pattern, errbuf);
        return -1;
    }
    regmatch_t matches[3];
    if ((e = regexec(&ex, str, 3, matches, 0)) != 0) {
        char errbuf[128];
        regerror(e, &ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        if (e != REG_NOMATCH)
            err("regexec: Couldn't match '%s' in '%s': %s\n",
                pattern, str, errbuf);
        return -1;
    }

    *r = (off_t)strtoll(str + matches[1].rm_so, NULL, 10) *
           (matches[2].rm_so == -1 ? 1 :
            tolower(*(str + matches[2].rm_so)) == 't' ? (off_t)1 << 40 : 
            tolower(*(str + matches[2].rm_so)) == 'g' ? (off_t)1 << 30 :
            tolower(*(str + matches[2].rm_so)) == 'm' ? (off_t)1 << 20 : 
            tolower(*(str + matches[2].rm_so)) == 'k' ? (off_t)1 << 10 : 1);
    return 0;
}

static int parse_size(const char *str, size_t *r)
{
    regex_t ex;
    int e;
    static const char *pattern =
        "^[[:space:]]*([0-9]+)(b|kb?|mb?|gb?)?[[:space:]]*$";

    if ((e = regcomp(&ex,pattern, REG_EXTENDED | REG_ICASE)) != 0) {
        char errbuf[128];
        regerror(e, &ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        err("regcomp(%s): %s\n", pattern, errbuf);
        return -1;
    }
    regmatch_t matches[3];
    if ((e = regexec(&ex, str, 3, matches, 0)) != 0) {
        char errbuf[128];
        regerror(e, &ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        if (e != REG_NOMATCH)
            err("regexec: Couldn't match '%s' in '%s': %s\n",
                pattern, str, errbuf);
        return -1;
    }
    *r = (size_t)strtoll(str + matches[1].rm_so, NULL, 10) *
           (matches[2].rm_so == -1 ? 1 :
            tolower(*(str + matches[2].rm_so)) == 'g' ? 1024 * 1024 * 1024 : 
            tolower(*(str + matches[2].rm_so)) == 'm' ? 1024 * 1024 : 
            tolower(*(str + matches[2].rm_so)) == 'k' ? 1024 : 1);
    return 0;
}

static int parse_int_range(const char *str, int *start, int *end)
{
    regex_t ex;
    int e;
    static const char *pattern =
        "^[[:space:]]*([0-9]+)[[:space:]]*(-([0-9]+))?[[:space:]]*$";

    if ((e = regcomp(&ex,pattern, REG_EXTENDED)) != 0) {
        char errbuf[128];
        regerror(e, &ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        err("regcomp(%s): %s\n", pattern, errbuf);
        return -1;
    }
    regmatch_t matches[4];
    if ((e = regexec(&ex, str, 4, matches, 0)) != 0) {
        char errbuf[128];
        regerror(e, &ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        if (e != REG_NOMATCH)
            err("regexec: Couldn't match '%s' in '%s': %s\n",
                pattern, str, errbuf);
        return -1;
    }
    *start = (int)strtol(str + matches[1].rm_so, NULL, 10);
    if (matches[3].rm_so != -1)
        *end = (int)strtol(str + matches[3].rm_so, NULL, 10);
    else
        *end = *start;

    return 0;
}

static void shutdown()
{
    if (cpu_pids != NULL) {
        int i;
        for (i = 0; i < n_cpu_pids; i++) {
            if (cpu_pids[i] != 0) {
                say(1, "killing CPU spinner %d (PID %d)\n", i, cpu_pids[i]);
                kill(cpu_pids[i], SIGTERM);
            }
        }
        free(cpu_pids);
        cpu_pids = NULL;
    }
    if (mem_pid != 0) {
        say(1, "killing mem spinner %d\n", mem_pid);
        kill(mem_pid, SIGTERM);
    }
    if (mem_stir_buffer != NULL) {
        free(mem_stir_buffer);
        mem_stir_buffer = NULL;
    }
    if (disk_pids != NULL) {
        int i;
        for (i = 0; i < n_disk_pids; i++) {
            if (disk_pids[i] != 0) {
                say(1,"killing disk churner %d (PID %d)\n", i, disk_pids[i]);
                kill(disk_pids[i], SIGTERM);
            }
        }
        free(disk_pids);
        disk_pids = NULL;
    }
    if (c_disk_churn_paths != NULL) {
        size_t i;
        for (i = 0; i < c_disk_churn_paths_n; i++) {
            if (c_disk_churn_paths[i] != NULL) {
                if (unlink(c_disk_churn_paths[i]) == -1 && errno != ENOENT) {
                    perror(c_disk_churn_paths[i]);
                }
            }
        }
    }
    exit(0);
}

static RETSIGTYPE sigchld_handler(int signum)
{
    int status;
    pid_t which = wait(&status);
    err("got SIGCHLD for dead child %d", which);
    if (WIFSIGNALED(status)) {
        err("; exited with signal %d\n", WTERMSIG(status));
    } else {
        err("; exited with status %d\n", WEXITSTATUS(status));
    }
    shutdown();
}

static void sigterm_handler(int signum)
{
    shutdown();
}

static uint64_t jiffies_to_usec(uint64_t jiffies)
{
    return jiffies * (1000 / sysconf(_SC_CLK_TCK)) * 1000;
}

static int get_cpu_count()
{
    /* FIXME: linux-specific */
    FILE *f;
    char s[128];
    int n = 0;
    static const char *pattern =
        "^physical *id +: *0*[1-9]";
    regex_t ex;
    int e;

    if ((e = regcomp(&ex, pattern, REG_EXTENDED | REG_ICASE)) != 0) {
        char errbuf[128];
        regerror(e, &ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        err("regcomp(%s): %s\n", pattern, errbuf);
        return -1;
    }

    f = fopen("/proc/cpuinfo", "r");
    if (f == NULL) {
        perror("/proc/cpuinfo");
        exit(1);
    }
    while (fgets(s, sizeof(s)-1, f) != NULL) {
        s[sizeof(s)-1] = '\0';
        if (!strncmp(s, "processor", strlen("processor"))) {
            n++;
        } else {
            e = regexec(&ex, s, 0, NULL, 0);
            if (e == 0) {
                /* matched a phys-id other than zero, don't count it */
                n--;
            } else if (e != REG_NOMATCH) {
                char errbuf[128];
                regerror(e, &ex, errbuf, sizeof(errbuf)-1);
                errbuf[sizeof(errbuf)-1] = '\0';
                err("regexec: Couldn't match '%s' in '%s': %s\n",
                    pattern, s, errbuf);
                return -1;
            }
        }
    }
    fclose(f);
    if (n == 0) {
        err("Couldn't get any CPU counts, assuming one CPU core\n");
        err("If this is wrong, override with --ncpus\n");
        ncpus = 1;
    }
    return n;
}

static uint64_t get_cpu_busy_time()
{
    FILE *f;
    char s[256];
    uint64_t utime, ntime, stime;
    static regex_t *ex = NULL;
    static const char *pattern =
        "^cpu[[:space:]]+([0-9]+)[[:space:]]+([0-9]+)[[:space:]]+([0-9]+)";
    int r;

    if (ex == NULL) {
        ex = (regex_t *)malloc(sizeof(*ex));
        if (ex == NULL) {
            perror("malloc");
            _exit(1);
        }
        if ((r = regcomp(ex,pattern, REG_EXTENDED)) != 0) {
            char errbuf[128];
            regerror(r, ex, errbuf, sizeof(errbuf)-1);
            errbuf[sizeof(errbuf)-1] = '\0';

            err("regcomp(%s): %s\n", pattern, errbuf);
            _exit(1);
        }
    }

    if ((f = fopen("/proc/stat", "r")) == NULL) {
        perror("/proc/stat");
        _exit(1);
    }
    if (fgets(s, sizeof(s)-1, f) == NULL) {
        perror("fgets");
        _exit(1);
    }

    regmatch_t matches[4];
    if ((r = regexec(ex, s, 4, matches, 0)) != 0) {
        char errbuf[128];
        regerror(r, ex, errbuf, sizeof(errbuf)-1);
        errbuf[sizeof(errbuf)-1] = '\0';

        err("regexec: Couldn't match '%s' in '%s': %s\n", pattern, s, errbuf);
        _exit(1);
    }

    utime = atol(s + matches[1].rm_so);
    ntime = atol(s + matches[2].rm_so);
    stime = atol(s + matches[3].rm_so);

    say(3, "cpu_spin(%d): current utime=%"PRIu64
           " ntime=%"PRIu64" stime=%"PRIu64" total=%"PRIu64"\n",
            getpid(), utime, ntime, stime, utime + ntime + stime);
    fclose(f);
    return utime + ntime + stime;
}

static char cpu_spin_accumulator;

static char squander_time(uint64_t iteration)
{
    return (cpu_spin_accumulator += (char)iteration);
}

static void cpu_spin_calibrate(int util, uint64_t *busycount, suseconds_t *sleeptime)
{
    uint64_t counter;
    struct timeval tv, tv2;
    const uint64_t iterations = 10000000;

    say(1, "cpu_spin (%d): measuring CPU\n", getpid());
    if (gettimeofday(&tv, NULL) == -1) shutdown();
    for (counter = 0; counter < iterations; counter++) {
        squander_time(counter);
    }
    if (gettimeofday(&tv2, NULL) == -1) shutdown();
    long long elapsed = (tv2.tv_sec - tv.tv_sec) * 1000000 +
                    (tv2.tv_usec - tv.tv_usec);
    long long countspeed = (counter * 100000)/elapsed;
    *busycount = (countspeed * util) / 100LL;
    /* busy_count_time = busycount/(counter/elapsed)
     * idle_time = 1sec - busy_count_time
     */

    *sleeptime = 100000 - *busycount / (counter / elapsed);
    say(3, "cpu_spin (%d): %"PRIu64" iterations in %lld usec (%lld/sec)\n",
           getpid(), counter, elapsed, countspeed);

    say(1, "cpu_spin (%d): est. %d%% util at %"PRIu64" cycles, %u usec sleep\n",
            getpid(), util, *busycount, *sleeptime);
}

static double cpu_spin_compute_util(enum cpu_util_mode mode, int l, int h,
                                    time_t curtime)
{
    static long time_offset = -1;

    if (time_offset == -1) {
        if (!utc) {
#ifdef HAVE_TZSET
            tzset();
#endif
            time_offset = -timezone;
        } else {
            time_offset = 0;
        }
        time_offset += c_cpu_curve_peak;
    }

    if (mode == UTIL_MODE_FIXED) return l;
    if (mode == UTIL_MODE_CURVE) {
        if (curtime == 0)
            curtime = time(NULL);
        long offset_in_interval = (curtime + time_offset) % c_cpu_curve_period;
        double fraction = (double)offset_in_interval / (double)c_cpu_curve_period;
        /* model the CPU usage as a cosine function over [0,2pi], shifted up
         * by min+1 to place the floor on min, and scaled so as to have its
         * entire range between min and max.  That is, the peak of the cosine
         * curve is placed at X=0, and it oscillates over the selected CPU
         * utilization range.
         *
         * This isn't a particularly realistic utilization curve and something
         * better ought to be found.  Or just bite the bullet and make it
         * data-driven.
         */
        double level = (double)l +
            (double)(h - l) * (cos(fraction * PI * 2) + 1)/2;
        return level;
    }
    /* shouldn't get here */
    return -1;
}

static void cpu_spin(long long ncpus, long long util_l, long long util_h, void *dummy, void *dummy2)
{
    uint64_t busycount;
    const uint64_t minimum_cycles = 10000;
    suseconds_t sleeptime;
    int first = 1;
    double util;
    int64_t adjust = 0;
        
    util = cpu_spin_compute_util(c_cpu_util_mode, util_l, util_h, 0);

    cpu_spin_calibrate(util, &busycount, &sleeptime);

    say(2, "cpu_spin (%d): spinning cpu\n", getpid());
    while (1) {
        struct timeval tv;
        long long counter;
        uint64_t busytime, busytime2;
        uint64_t walltime, walltime2;

        if (! first) {
            uint64_t busy = jiffies_to_usec(busytime2 - busytime) / ncpus;
            uint64_t wall = walltime2 - walltime;
            double actual = (100 * busy) / wall;
            int64_t oldadjust = adjust;
            /* On a multi-CPU system there will be more than one spinner
             * trying to hit the target utilization, so restrain the
             * adjustment range so as to keep from collectively
             * overcompensating
             */
            adjust = (int64_t)(((util - actual) * busycount) / 100. / ncpus);
            say(3, "cpu_spin (%d): last iter: count=%lld"
                   " (~%lld of %lld); adjust=%lld\n",
                   getpid(), busycount, busy, wall, 
                   adjust);
            if (adjust < 0 && busycount < -adjust) {
                say(2, "cpu_spin (%d): usage at lower limit\n", getpid());
                busycount = minimum_cycles;
            } else if (adjust > 0 && UINT64_MAX - adjust < busycount) {
                say(2, "cpu_spin (%d): usage at upper limit\n", getpid());
            } else {
                busycount += adjust;
                if (busycount < minimum_cycles)
                    busycount = minimum_cycles;
            }

            if ((adjust < 0 && oldadjust > 0) ||
                (adjust > 0 && oldadjust < 0)) {
                say(3, "cpu_spin (%d): adjusted approximately correctly\n",
                       getpid());
                say(3, "cpu_spin (%d): spin count %"PRIu64"\n", getpid(), busycount);
            }
        } else {
            first = 0;
        }
        gettimeofday(&tv, NULL);
        walltime = tv.tv_sec * 1000000 + tv.tv_usec;
        busytime = get_cpu_busy_time();

        say(3, "cpu_spin (%d): spinning (0 to %"PRIu64")...\n", getpid(), busycount);
        for (counter = 0; counter < busycount; counter++) {
            squander_time(counter);
        }

        say(3, "cpu_spin (%d): sleeping...\n", getpid());
        usleep(sleeptime);
        gettimeofday(&tv, NULL);
        walltime2 = tv.tv_sec * 1000000 + tv.tv_usec;
        busytime2 = get_cpu_busy_time();

        util = cpu_spin_compute_util(c_cpu_util_mode, util_l, util_h, tv.tv_sec);

        /* "elapsed" here doesn't necessarily refer only to our own usage */
        say(2, "cpu_spin (%d): %"PRIu64" iterations; %"PRIu64" CPU-jiffies elapsed\n",
               getpid(), counter, busytime2-busytime,
               ncpus);
    }
    _exit(1);
}

static void mem_stir(long long asz, long long dummy, long long dummy2, void *dummyp, void *dummyp2)
{
    char *mem_stir_buffer;
    char *p;
    const size_t pagesize = LB_PAGE_SIZE;
    const size_t sz = asz;

    say(1, "mem_stir (%d): stirring %llu bytes...\n", getpid(), sz);
    if ((mem_stir_buffer = (char *)malloc(sz)) == NULL) {
        perror("malloc");
        _exit(1);
    }

    say(2, "mem_stir (%d): dirtying buffer...", getpid());
    for (p = mem_stir_buffer; p < mem_stir_buffer + sz; p++) {
        *p = (char)((uintptr_t)p & 0xff);
    }
    say(2, "done\n");

    char *sp = mem_stir_buffer;
    char *dp = mem_stir_buffer;
    while (1) {
        const size_t copysz = pagesize;

#ifdef HAVE_MEMMOVE
        memmove(dp, sp, copysz);
#else
        memcpy(dp, sp, copysz);
#endif
        sp += pagesize * 1;
        dp += pagesize * 5;
        if (sp >= mem_stir_buffer + sz) {
            say(2, "mem_stir (%d): read position wrapped\n", getpid());
            sp = mem_stir_buffer;
        }
        if (dp >= mem_stir_buffer + sz) {
            say(2, "mem_stir (%d): write position wrapped\n", getpid());
            dp = mem_stir_buffer;
        }
        usleep(c_mem_stir_sleep);
    }
    _exit(1);
}

static void disk_churn(long long dummy0, long long dummy, long long dummy2, void *pathv, void *szv)
{
    char *path = (char *)pathv;
    off_t sz = *(off_t *)szv;
    int fd;
    int witer;
    off_t rpos, wpos;

    say(1, (sizeof(off_t) == 8 ?
            "disk_churn (%d): churning disk on %s (%ld bytes)\n" :
            "disk_churn (%d): churning disk on %s (%d bytes)\n"),
           getpid(), path, sz);
    if ((fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600)) == -1) {
        err("disk_churn (%d): Couldn't create %s: %s\n",
                getpid(), path, strerror(errno));
        exit(1);
    }
    if (0 && unlink(path) && errno != ENOENT) {
        perror(path);
        exit(1);
    }

    if (lseek(fd, sz - sizeof(fd), SEEK_SET) == (off_t)-1) {
        perror("lseek");
        exit(1);
    }
    if (write(fd, &fd, sizeof(fd)) == -1) {
        perror("write");
        exit(1);
    }
    
    char *block = malloc(c_disk_churn_block_size);
    if (block == NULL) {
        perror("malloc");
        _exit(1);
    }

    witer = 0;
    rpos = wpos = 0;
    while (1) {
        size_t p;
        ssize_t r;

        if (rpos >= sz) {
            say(2, "disk_churn (%d) reader reached EOF at %ld\n",
                   getpid(), (long)rpos);
            rpos = 0;
        }
        if (wpos >= sz) {
            say(2, "disk_churn (%d) writer reached EOF at %ld\n",
                   getpid(), (long)wpos);
            wpos = 0;
            witer++;
        }

        for (p = 0; p < c_disk_churn_block_size; p++)
            block[p] = (char)((witer | (int)p) & 0xff);

        if (lseek(fd, wpos, SEEK_SET) == (off_t)-1)  {
            perror("lseek");
            exit(1);
        }
        if (write(fd, block, c_disk_churn_block_size) == -1) {
            perror("write");
            exit(1);
        }
        wpos += c_disk_churn_step_size * 2;

        if (lseek(fd, rpos, SEEK_SET) == (off_t)-1)  {
            perror("lseek");
            exit(1);
        }
        r = read(fd, block, c_disk_churn_block_size);
        if (r == -1) {
            err("disk_churn (%d): error reading from %s at %ld: %s\n",
                    getpid(), path, (long)rpos, strerror(errno));
            exit(1);
        }
        if (r == 0) {
            say(1, "disk_churn (%d): reader reached EOF early (at %ld)\n",
                   getpid(), (long)rpos);
            rpos = 0;
        }
        rpos += c_disk_churn_step_size;
        usleep(c_disk_churn_sleep * 1000);
    }
    _exit(0);
}


static pid_t fork_and_call(char *desc, spinner_fn fn, long long arg1, long long arg2, long long arg3, void *argP, void *argP2)
{
    pid_t p = fork();
    if (p == -1) {
        perror("fork");
        return 0;
    }
    else if (p == 0) {
        if (cpu_pids != NULL) {
            free(cpu_pids);
            cpu_pids = NULL;
        }
        if (disk_pids != NULL) {
            free(disk_pids);
            disk_pids = NULL;
        }
        mem_pid = 0;
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        (*fn)(arg1, arg2, arg3, argP, argP2);
        exit(0);
    } else {
        say(1, "lookbusy (%d): %s started, PID %d\n", getpid(), desc, p);
        return p;
    }
}

static pid_t *start_cpu_spinners(int *ncpus, int util_l, int util_h)
{
    pid_t *pids;
    if (*ncpus <= 0)
        *ncpus = get_cpu_count();
    if ((pids = (pid_t *)malloc(sizeof(*pids) * *ncpus)) == NULL) {
        perror("malloc");
        return NULL;
    }
    int i;
    say(1, "cpu_spin (%d): starting %d spinner(s) for %d%%-%d%% usage\n",
           getpid(), *ncpus, util_l, util_h);
    for (i = 0; i < *ncpus; i++) {
        pids[i] = fork_and_call("CPU spinner", cpu_spin, *ncpus, util_l, util_h, NULL, NULL);
    }
    return pids;
}

static pid_t *start_disk_stirrer(off_t util, char **paths, size_t paths_n)
{
    size_t i;
    pid_t *p = malloc(sizeof(*p) * paths_n);

    for (i = 0; i < paths_n; i++) {
        struct stat st;
        if (stat(paths[i], &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                if (access(paths[i], W_OK) != 0) {
                    err("disk path %s is not writable\n", paths[i]);
                    shutdown();
                }
                char *tmpl = (char *)malloc(strlen(paths[i]) + 32);
                if (tmpl == NULL) {
                    perror("malloc");
                    shutdown();
                }
                snprintf(tmpl, strlen(paths[i]) + 31, "%s/lb.%d.XXXXXX",
                         paths[i], getpid());
                int fd = mkstemp(tmpl);
                if (fd == -1) {
                    perror("mkstemp");
                    free(tmpl);
                    shutdown();
                }

                free(paths[i]);
                paths[i] = tmpl;
            }
        }

        char *desc = (char *)malloc(32 + strlen(paths[i]));
        if (desc == NULL) {
            perror("malloc");
            shutdown();
        }
        snprintf(desc, 31 + strlen(paths[i]), "disk churn: %s", paths[i]); 
        p[i] = fork_and_call(desc, disk_churn, 0, 0, 0, paths[i], &util);
    }
    return p;
}

static pid_t start_mem_whisker(size_t sz)
{
    return fork_and_call("mem stirrer", mem_stir, sz, 0, 0, NULL, NULL);
}

static void usage()
{
    static const char *msg =
"lookbusy [ -h ] [ options ]\n"
"General options:\n"
"  -h, --help           Commandline help (you're reading it)\n"
"  -v, --verbose        Verbose output (may be repeated)\n"
"  -q, --quiet          Be quiet, produce output on errors only\n"
"CPU usage options:\n"
"  -c, --cpu-util=PCT,  Desired utilization of each CPU, in percent (default\n"
"      --cpu-util=RANGE   50%).  If 'curve' CPU usage mode is chosen, a range\n"
"                         of the form MIN-MAX should be given.\n"
"  -n, --ncpus=NUM      Number of CPUs to keep busy (default: autodetected)\n"
"  -r, --cpu-mode=MODE  Utilization mode ('fixed' or 'curve', see lookbusy(1))\n"
"  -p, --cpu-curve-peak=TIME\n"
"                       Offset of peak utilization within curve period, in\n"
"                         seconds (append 'm', 'h', 'd' for other units)\n"
"  -P, --cpu-curve-period=TIME\n"
"                       Duration of utilization curve period, in seconds (append\n"
"		       'm', 'h', 'd' for other units)\n"
"Memory usage options:\n"
"  -m, --mem-util=SIZE   Amount of memory to use (in bytes, followed by KB, MB,\n"
"                         or GB for other units; see lookbusy(1))\n"
"  -M, --mem-sleep=TIME Time to sleep between iterations, in usec (default 1000)\n"
"Disk usage options:\n"
"  -d, --disk-util=SIZE Size of files to use for disk churn (in bytes,\n"
"                         followed by KB, MB, GB or TB for other units)\n"
"  -b, --disk-block-size=SIZE\n"
"                       Size of blocks to use for I/O (in bytes, followed\n"
"                         by KB, MB or GB)\n"
"  -D, --disk-sleep=TIME\n"
"                       Time to sleep between iterations, in msec (default 100)\n"
"  -f, --disk-path=PATH Path to a file/directory to use as a buffer (default\n"
"                         /tmp); specify multiple times for additional paths\n"
"";
    printf("usage: %s", msg);
    exit(0);
}

int main(int argc, char **argv)
{
    int c;
    size_t disk_paths_cap = 0;

    static const struct option long_options[] = {
        { "help", 0, NULL, 'h' },
        { "verbose", 0, NULL, 'v' },
        { "quiet", 0, NULL, 'q' },
        { "version", 0, NULL, 'V' },

        { "cpu-util", 1, NULL, 'c' },
        { "cpu-mode", 1, NULL, 'r' },
        { "ncpus", 1, NULL, 'n' },
        { "cpu-curve-peak", 1, NULL, 'p' },
        { "cpu-curve-period", 1, NULL, 'P' },
        { "utc", 0, NULL, 'u' },

        { "disk-util", 1, NULL, 'd' },
        { "disk-sleep", 1, NULL, 'D' },
        { "disk-block-size", 1, NULL, 'b' },
        { "disk-path", 1, NULL, 'f' },

        { "mem-util", 1, NULL, 'm' },
        { "mem-sleep", 1, NULL, 'M' },
        { 0, 0, 0, 0 }
    };

    while ((c = getopt_long(argc, argv,
                            "n:b:c:d:D:f:m:M:p:P:r:qvVhu",
                            long_options, NULL)) != -1) {
        switch (c) {
            default:
            case 'h': usage(); break;
            case 'b':
                if (parse_size(optarg, &c_disk_churn_block_size) < 0) {
                    err("Couldn't parse disk block size '%s'\n", optarg);
                    return 1;
                }
                break;
            case 'c':
                if (parse_int_range(optarg, &c_cpu_util_l, &c_cpu_util_h) < 0) {
                    err("Couldn't parse CPU utilization setting '%s'\n",
                        optarg);
                    return 1;
                }
                break;
            case 'd':
                if (parse_large_size(optarg, &c_disk_util) < 0) {
                    err("Couldn't parse disk utilization filesize '%s'\n",
                            optarg);
                    return 1;
                }
                break;
            case 'D':
                c_disk_churn_sleep = atol(optarg);
                break;
            case 'f':
                if (!optarg || !*optarg)
                    break;
                if (c_disk_churn_paths_n + 1 > disk_paths_cap) {
                    disk_paths_cap = (c_disk_churn_paths_n + 1) * 2;
                    char **tmp;

                    tmp = (char **)realloc(c_disk_churn_paths,
                                           disk_paths_cap * sizeof(*tmp));
                    if (tmp == NULL) {
                        perror("realloc");
                        _exit(1);
                    }
                    c_disk_churn_paths = tmp;
                }
                c_disk_churn_paths[c_disk_churn_paths_n++] = strdup(optarg);
                break;
            case 'm':
                if (parse_size(optarg, &c_mem_util) < 0) {
                    err("Couldn't parse memory utilization size '%s'\n", optarg);
                    return 1;
                }
                break;
            case 'M':
                c_mem_stir_sleep = atol(optarg);
                break;
            case 'n':
                ncpus = atoi(optarg);
                break;
            case 'p':
                if (parse_timespan(optarg, &c_cpu_curve_peak) < 0) {
                    err("Couldn't parse CPU curve peak '%s'; format is"
                        " INTEGER[SUFFIX], where SUFFIX\n"
                        "is one of 's' (seconds), 'm' (minutes), 'h' (hours)"
                        ", or 'd' (days); e.g. \"2h\"\n", optarg);
                    return 1;
                }
                break;
            case 'P':
                if (parse_timespan(optarg, &c_cpu_curve_period) < 0) {
                    err("Couldn't parse CPU curve period '%s'; format is"
                        " INTEGER[SUFFIX], where SUFFIX\n"
                        "is one of 's' (seconds), 'm' (minutes), 'h' (hours)"
                        ", or 'd' (days); e.g. \"2h\"\n", optarg);
                    return 1;
                }
                break;
            case 'q':
                verbosity = 0;
                break;
            case 'r':
#ifdef HAVE_STRCASECMP
                if (strcasecmp(optarg, "fixed") == 0)
                    c_cpu_util_mode = UTIL_MODE_FIXED;
                else if (strcasecmp(optarg, "curve") == 0)
                    c_cpu_util_mode = UTIL_MODE_CURVE;
#else
                if (strcmp(optarg, "fixed") == 0)
                    c_cpu_util_mode = UTIL_MODE_FIXED;
                else if (strcmp(optarg, "curve") == 0)
                    c_cpu_util_mode = UTIL_MODE_CURVE;
#endif
                else {
                    err("Unrecognized CPU utilization mode '%s'; choose one"
                        " of 'fixed' or 'curve'\n", optarg);
                    return 1;
                }
                break;
            case 'u':
                utc = ! utc;
                break;
            case 'v':
                verbosity++;
                break;
            case 'V':
                printf("%s %s -- %s\n", PACKAGE, VERSION, copyright);
                return 0;
        }
    }

    if (c_cpu_util_l != c_cpu_util_h && c_cpu_util_mode == UTIL_MODE_FIXED) {
        err("Fixed CPU usage mode selected, but CPU usage range %d-%d%%"
            " given; %d%% will\nbe used.\n",
            c_cpu_util_l, c_cpu_util_h, c_cpu_util_l);
        c_cpu_util_h = c_cpu_util_l;
    }
    if (c_cpu_util_mode == UTIL_MODE_CURVE &&
        c_cpu_curve_peak > c_cpu_curve_period) {
        err("Peak of CPU usage curve is outside curve frequency"
            "(%ds > %ds)\n", c_cpu_curve_peak, c_cpu_curve_period);
        return 1;
    }

    if (c_disk_churn_paths == NULL && c_disk_util != 0) {
        c_disk_churn_paths = (char **)malloc(sizeof(*c_disk_churn_paths) * 1);
        *c_disk_churn_paths = strdup("/tmp");
        c_disk_churn_paths_n = 1;
    }

    if (c_disk_util != 0 && c_disk_churn_block_size < 4) {
        err("Disk utilization block size must be at least 4 bytes\n");
        return 1;
    }
    if (c_disk_util != 0 && c_disk_util < c_disk_churn_block_size) {
        err("Disk utilization size must be at least equal to the block size"
            " (%d < %d)\n"
            "You can adjust the block size with (-b or --disk-block-size)\n",
            c_disk_util, c_disk_churn_block_size);
        return 1;
    }

    signal(SIGCHLD, sigchld_handler);
    signal(SIGTERM, sigterm_handler);
    signal(SIGINT, sigterm_handler);

    if (ncpus != 0 && c_cpu_util_h != 0) {
        cpu_pids = start_cpu_spinners(&ncpus, c_cpu_util_l, c_cpu_util_h); // forks
        n_cpu_pids = ncpus;
    }
    if (c_disk_util != 0) {
        disk_pids = start_disk_stirrer(c_disk_util,
                                       c_disk_churn_paths,
                                       c_disk_churn_paths_n); // forks
        n_disk_pids = c_disk_churn_paths_n;
    }
    if (c_mem_util != 0) {
        mem_pid = start_mem_whisker(c_mem_util); // forks
    }
    while (sleep(1) == 0)
            say(2, "lookbusy (%d): waiting for spinners...\n", getpid());
    shutdown();
    return 0;
}

/* vi: set ts=4 sw=4 et: */
