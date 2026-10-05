/*
 * sentinel_read - minimal C client for the sentinel kernel driver.
 *
 * Reads samples straight from /dev/sentinel0 (no daemon involved), which makes it the
 * first thing to run after `insmod` to prove the driver works.
 *
 *   sentinel_read [-d /dev/sentinel0] [-n count] [-p period_ms] [-s]
 *     -n  number of samples to print (default 10)
 *     -p  set sampling period through the ioctl first
 *     -s  print driver statistics (ioctl) when done
 */
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <sentinel/uapi.h>

static void usage(const char *argv0)
{
	fprintf(stderr, "usage: %s [-d device] [-n count] [-p period_ms] [-s]\n", argv0);
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/sentinel0";
	long count = 10;
	long period = 0;
	int want_stats = 0;
	int opt;

	while ((opt = getopt(argc, argv, "d:n:p:sh")) != -1) {
		switch (opt) {
		case 'd': dev = optarg; break;
		case 'n': count = strtol(optarg, NULL, 10); break;
		case 'p': period = strtol(optarg, NULL, 10); break;
		case 's': want_stats = 1; break;
		default: usage(argv[0]); return opt == 'h' ? 0 : 2;
		}
	}
	if (count < 1) {
		usage(argv[0]);
		return 2;
	}

	int fd = open(dev, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		fprintf(stderr, "open %s: %s\n", dev, strerror(errno));
		return 1;
	}

	if (period > 0) {
		__u32 p = (__u32)period;
		if (ioctl(fd, SENTINEL_IOC_SET_PERIOD, &p) != 0) {
			fprintf(stderr, "SET_PERIOD(%ld): %s\n", period, strerror(errno));
			close(fd);
			return 1;
		}
	}

	for (long i = 0; i < count; ) {
		struct sentinel_sample s[8];
		ssize_t n = read(fd, s, sizeof(s)); /* blocks until at least one sample exists */

		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "read: %s\n", strerror(errno));
			close(fd);
			return 1;
		}
		for (size_t k = 0; k < (size_t)n / sizeof(s[0]) && i < count; ++k, ++i) {
			printf("seq=%u ts=%llu ns temp=%.3f C hum=%.3f %% flags=0x%x\n", s[k].seq,
			       (unsigned long long)s[k].ts_ns, s[k].temp_mc / 1000.0, s[k].hum_mpct / 1000.0,
			       s[k].flags);
		}
	}

	if (want_stats) {
		struct sentinel_dev_stats st;

		if (ioctl(fd, SENTINEL_IOC_GET_STATS, &st) == 0)
			printf("driver: produced=%llu dropped=%llu period=%u ms fifo=%u/%u\n",
			       (unsigned long long)st.produced, (unsigned long long)st.dropped, st.period_ms,
			       st.fifo_used, st.fifo_depth);
		else
			fprintf(stderr, "GET_STATS: %s\n", strerror(errno));
	}
	close(fd);
	return 0;
}
