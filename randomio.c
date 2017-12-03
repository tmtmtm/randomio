//
// multithreaded random i/o microbenchmark
//
// supports reads and writes...
//

// Copyright (c) 2006 dean gaudet <dean@arctic.org>
// 
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense,
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
// 
// The above copyright notice and this permission notice shall be included
// in all copies or substantial portions of the Software.
// 
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
// THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
// OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
// ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
// OTHER DEALINGS IN THE SOFTWARE.



#define _GNU_SOURCE             // for O_DIRECT
#define _FILE_OFFSET_BITS 64
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <inttypes.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <sys/time.h>
#include <math.h>

#ifdef __APPLE__
#define OPEN_FLAGS 0
#else
#ifdef O_DIRECT
#define OPEN_FLAGS O_DIRECT
#else
#error "Needs O_DIRECT"
#endif
#endif

#define DIRECT_ALIGN (512)

typedef struct {
        int fd;
	double write_fraction;
	double fsync_fraction;
	size_t io_size;
	off_t file_size;
} globals_t;
static globals_t globals;

typedef struct {
        double min_latency;
        double sum_latency;
        double sum_square_latency;
        double max_latency;
	unsigned long count;
} stats_t;

static pthread_mutex_t stats_lock = PTHREAD_MUTEX_INITIALIZER;
enum {
        S_RD,
        S_WR
};
static stats_t stats[2];

// the threads wait until they're all done initializing before
// they start the i/o.
static pthread_mutex_t wait_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wait_cond = PTHREAD_COND_INITIALIZER;
static size_t nr_to_startup;

static inline double now(void)
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec + 1e-6*tv.tv_usec;
}

void *thrasher(void *_params)
{
        // posix_memalign would be useful here, but it's not sufficiently portable.
        size_t page_mask = sysconf(_SC_PAGESIZE) - 1;
        void *io_buffer = malloc(globals.io_size + page_mask);
        if (io_buffer == NULL) {
                perror("malloc");
                exit(1);
        }
        io_buffer = (void *)(((uintptr_t)io_buffer + page_mask) & ~page_mask);

	int fd = globals.fd;
	size_t io_size = globals.io_size;
	off_t nr_valid_offsets = (globals.file_size - globals.io_size) / io_size;
	double write_fraction = globals.write_fraction;
	double fsync_fraction = write_fraction * globals.fsync_fraction;

        // now wait for everyone to be ready
        pthread_mutex_lock(&wait_mutex);
        --nr_to_startup;
        if (nr_to_startup) {
                pthread_cond_wait(&wait_cond, &wait_mutex);
        }
        else {
                pthread_cond_broadcast(&wait_cond);
        }
        // we only access drand48() while under a mutex... so grab
        // our first two random values here.  note that sharing the
        // same RNG across all threads is easier than trying to
        // arrange the threads to avoid 
        double d_offs = drand48();
        double d_rdwr = drand48();
        pthread_mutex_unlock(&wait_mutex);

	for (;;) {
                double start, latency;
                off_t offs;
                size_t rdwr = (d_rdwr < write_fraction) ? S_WR : S_RD;

		offs = io_size * (off_t)(d_offs * nr_valid_offsets);
                start = now();
		// XXX: too lazy to handle partial reads/writes
                int rc;
		do {
			if (rdwr == S_WR) {
				rc = pwrite(fd, io_buffer, io_size, offs);
			}
			else {
				rc = pread(fd, io_buffer, io_size, offs);
			}
		} while (rc < 0 && rc == EINTR);
		if (rc < 0) {
			perror("thrasher read or write");
			exit(1);
		}
		if (d_rdwr < fsync_fraction) {
			rc = fsync(fd);
			if (rc < 0) {
				perror("thrasher fsync");
				exit(1);
			}
		}
                latency = now() - start;

                // update the statistics
                pthread_mutex_lock(&stats_lock);
		++stats[rdwr].count;
                stats[rdwr].sum_latency += latency;
                stats[rdwr].sum_square_latency += latency * latency;
                if (stats[rdwr].min_latency > latency) {
                        stats[rdwr].min_latency = latency;
                }
                if (latency > stats[rdwr].max_latency) {
                        stats[rdwr].max_latency = latency;
                }
                d_offs = drand48();
                d_rdwr = drand48();
                pthread_mutex_unlock(&stats_lock);
	}
	return NULL;
}


int main(int argc, char **argv)
{
	if (argc != 7 && argc != 8) {
usage:
		fprintf(stderr, "usage: %s filename nr_threads write_fraction_of_io fsync_fraction_of_writes io_size nr_seconds_between_samples [nr_samples]\n"
		        "io_size must be a positive multiple of %u bytes\n",
			argv[0], DIRECT_ALIGN);
		exit(1);
	}

	char *p;

	optind = 1;
	const char *filename = argv[optind++];
	size_t nr_threads = strtoul(argv[optind++], &p, 0);
	if (*p || nr_threads < 1) goto usage;
	globals.write_fraction = strtod(argv[optind++], &p);
	if (*p || globals.write_fraction < 0. || globals.write_fraction > 1.) goto usage;
	globals.fsync_fraction = strtod(argv[optind++], &p);
	if (*p || globals.fsync_fraction < 0. || globals.fsync_fraction > 1.) goto usage;
	globals.io_size = strtoul(argv[optind++], &p, 0);
	if (*p || globals.io_size % DIRECT_ALIGN) goto usage;
	unsigned nr_seconds = strtoul(argv[optind++], &p, 0);
	if (*p || nr_seconds < 1) goto usage;
        unsigned nr_samples = 0;
        if (argc > 7) {
                nr_samples = strtoul(argv[optind++], &p, 0);
                if (*p) goto usage;
        }
        

	// open the file and save its size
	globals.fd = open(filename, OPEN_FLAGS | (globals.write_fraction > 0.f ? O_RDWR : O_RDONLY));
	if (globals.fd == -1) {
		perror("open");
		exit(1);
	}

#ifdef __APPLE__
	fcntl(globals.fd, F_NOCACHE);
#endif

	globals.file_size = lseek(globals.fd, 0, SEEK_END);
	if (globals.file_size == (off_t)-1) {
		perror("lseek");
		exit(1);
	}

	if (globals.file_size < globals.io_size) {
		fprintf(stderr, "file is smaller than io_size\n");
		exit(1);
	}

        const stats_t clear_stats = {
                .min_latency = 1./0.,   // infinity
                .max_latency = 0.,
                .sum_square_latency = 0.,
                .sum_latency = 0.,
                .count = 0ul,
        };
        stats[S_RD] = clear_stats;
        stats[S_WR] = clear_stats;
        nr_to_startup = nr_threads;

	for (size_t i = 0; i < nr_threads; ++i) {
		pthread_t thread;
		if (pthread_create(&thread, NULL, thrasher, NULL)) {
			perror("pthread_create");
			exit(1);
		}
	}

        size_t n_rows = 24;

#ifdef TIOCGWINSZ
        struct winsize win;
        if (isatty(0) && ioctl (0, TIOCGWINSZ, &win) == 0) {
                n_rows = win.ws_row;
        }
        if (n_rows < 10) {
                n_rows = 10;
        }
#endif

	double last_tstamp = now();
        size_t cur_row = 0;
        unsigned sample_num = 0;
	for (;;) {
                if (cur_row == 0) {
                        printf("  total |  read:         latency (ms)       |  write:        latency (ms)\n");
                        printf("   iops |   iops   min    avg    max   sdev |   iops   min    avg    max   sdev\n");
                        printf("--------+-----------------------------------+----------------------------------\n");
                }
                ++cur_row;
                if (cur_row == n_rows - 3) {
                        cur_row = 0;
                }

		sleep(nr_seconds);

                pthread_mutex_lock(&stats_lock);
                stats_t sample[2];
                sample[S_RD] = stats[S_RD];
                sample[S_WR] = stats[S_WR];
                stats[S_RD] = clear_stats;
                stats[S_WR] = clear_stats;
                pthread_mutex_unlock(&stats_lock);
		double tstamp = now();

                // total iops
                printf("%7.1f", (sample[S_RD].count + sample[S_WR].count) / (tstamp - last_tstamp));

                size_t i = S_RD;
                for (;;) {
                        double mean = sample[i].sum_latency / sample[i].count;
                        double sdev = sqrt((sample[i].sum_square_latency
                                - sample[i].sum_latency * sample[i].sum_latency / sample[i].count) / (sample[i].count - 1));
                        printf(" |%7.1f %5.1f %6.1f %6.1f %6.1f",
                                sample[i].count / (tstamp - last_tstamp),
                                sample[i].min_latency * 1e3,
                                mean * 1e3,
                                sample[i].max_latency * 1e3,
                                sdev * 1e3);
                        if (i == S_WR) break;
                        i = S_WR;
                }
                printf("\n");
		last_tstamp = tstamp;

                ++sample_num;
                if (nr_samples != 0 && sample_num == nr_samples) break;
	}
	exit(0);
}
