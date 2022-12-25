## lookbusy 1.4 -- a synthetic load generator for Linux systems

This is lookbusy, a tool for making systems busy.  It uses relatively simple
techniques to generate CPU activity, memory and disk utilization and traffic.

lookbusy is not a benchmarking tool, or a realistic load simulator.  While it
attempts to produce load factors which are exhibited by real applications, the
exact operations used are not modelled on real applications, and at the low
level, the exact hardware operations are not identical.

* Process Structure

One lookbusy process is forked for each load-generation task -- that is, one
process per CPU, one for memory usage, and one for each file on disk being
used, plus a toplevel parent process.  Errors in or termination of any process
will trigger a shutdown in all others.  It's safe to use ^C from a terminal,
or to kill processes remotely.

* CPU Usage Modes

Two CPU usage modes are provided.  The first, 'fixed', attempts to keep total
CPU utilization at a particular level, using up any balance in idle time
between the other processes on the host and the preferred level (if other
processes are themselves able to exceed the chosen level, obviously, lookbusy
can't fix that situation but will drop its own usage to near zero and wait for
load to drop.)

The second mode, 'curve', produces utilization levels which vary over a chosen
range, over a given interval.  The simplest (and the default) usage is to
modulate usage smoothly over the course of a 24-hour period, peaking at local
midnight and bottoming out at local noon.  Options are provided to adjust all
these settings -- see lookbusy(1).

* CPU Concurrency

lookbusy has basic awareness of multiprocessor and multi-logical-CPU systems;
it will attempt to keep cumulative system usage at the chosen level by forking
multiple instances of itself, one per CPU.  CPUs with a nonzero physical-id,
such as are found on hyperthreaded i386 CPUs, are ignored when counting.  The
CPU utilization algorithm uses a tight arithmetic loop, which should be
entirely register-based on most CPUs, incurring no memory traffic.

* Portability

As of 1.0, lookbusy claims support only for Linux systems.  Most of its
implementation is entirely portable to other UNIX systems; memory and disk
usage should work as-is.  CPU utilization will almost certainly need porting
work, as concerns use of the /proc filesystem and handling of SMP.



$Id$

## General options

  -h, --help           Commandline help (you're reading it)

  -v, --verbose        Verbose output (may be repeated)

  -q, --quiet          Be quiet, produce output on errors only

CPU usage options:

  -c, --cpu-util=PCT,  Desired utilization of each CPU, in percent (default --cpu-util=RANGE   50%).  If 'curve' CPU usage mode is chosen, a range of the form MIN-MAX should be given.

  -n, --ncpus=NUM      Number of CPUs to keep busy (default: autodetected)

  -r, --cpu-mode=MODE  Utilization mode ('fixed' or 'curve', see lookbusy(1))

  -p, --cpu-curve-peak=TIME Offset of peak utilization within curve period, in seconds (append 'm', 'h', 'd' for other units)

  -P, --cpu-curve-period=TIME Duration of utilization curve period, in seconds (append 'm', 'h', 'd' for other units)

Memory usage options:

  -m, --mem-util=SIZE   Amount of memory to use (in bytes, followed by KB, MB, or GB for other units; see lookbusy(1))

  -M, --mem-sleep=TIME Time to sleep between iterations, in usec (default 1000)

Disk usage options:

  -d, --disk-util=SIZE Size of files to use for disk churn (in bytes,  followed by KB, MB, GB or TB for other units)

  -b, --disk-block-size=SIZE Size of blocks to use for I/O (in bytes, followed by KB, MB or GB)

  -D, --disk-sleep=TIME  Time to sleep between iterations, in msec (default 100)

  -f, --disk-path=PATH Path to a file/directory to use as a buffer (default /tmp); specify multiple times for additional paths

