// SPDX-License-Identifier: GPL-2.0-only
/*
 * usif-loop: exercise /dev/ttyUSIF0 with the driver loaded as loopback=1
 * (TX looped back to RX inside the USIF, pads not driven).
 *
 * Uses the port the way a firmware loader would: open with O_RDWR |
 * O_NOCTTY | O_NONBLOCK, cfmakeraw(), 8N1, CLOCAL | CREAD, no CRTSCTS,
 * rate set with cfsetispeed()/cfsetospeed() and tcsetattr(TCSAFLUSH)
 * before each rate. For each rate it measures
 *   - the latency of one byte (write until read() returns it),
 *   - echo of 1..9 byte writes (partial FIFO words),
 *   - a bulk transfer of about one second, checking every byte and
 *     deriving the effective line rate from the receive timing,
 *   - overrun and framing error counts (TIOCGICOUNT),
 *   - that TCSAFLUSH leaves nothing stale to read.
 * The port is put into raw mode at the first rate before anything is
 * written: with the default termios (cooked, ECHO on) and the loopback, the
 * tty would echo what it receives and keep those bytes circulating.
 *
 * With -r it only opens the port and applies each rate as above, never
 * writing a byte (safe in normal mode, pins claimed): it prints the rate
 * read back by tcgetattr(), the driver's baud and divider lines from
 * debugfs, and how many bytes arrived during one second at that rate.
 *
 * Usage: usif-loop [-d device] [-r] [rate ...]
 * Default rates: 9600 19200 115200 230400 460800 9600.
 */
#include <errno.h>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static speed_t speed_of(unsigned int baud)
{
	switch (baud) {
	case 1200: return B1200;
	case 2400: return B2400;
	case 4800: return B4800;
	case 9600: return B9600;
	case 19200: return B19200;
	case 38400: return B38400;
	case 57600: return B57600;
	case 115200: return B115200;
	case 230400: return B230400;
	case 460800: return B460800;
	case 921600: return B921600;
	default: return 0;
	}
}

static int set_rate(int fd, unsigned int baud)
{
	struct termios t;
	speed_t sp = speed_of(baud);

	if (!sp) {
		fprintf(stderr, "unsupported rate %u\n", baud);
		return -1;
	}
	if (tcgetattr(fd, &t))
		return -1;
	cfmakeraw(&t);
	t.c_cflag |= CLOCAL | CREAD;
	t.c_cflag &= ~CRTSCTS;
	t.c_cc[VMIN] = 1;
	t.c_cc[VTIME] = 0;
	cfsetispeed(&t, sp);
	cfsetospeed(&t, sp);
	return tcsetattr(fd, TCSAFLUSH, &t);
}

/*
 * Send @len bytes while reading them back. Returns the number of bytes
 * received; t_first and n_first describe the first read, t_last the last.
 */
static size_t xfer(int fd, const uint8_t *tx, uint8_t *rx, size_t len,
		   double timeout, double *t_first, size_t *n_first,
		   double *t_last)
{
	double end = now() + timeout;
	size_t wr = 0, rd = 0;

	*t_first = *t_last = 0;
	*n_first = 0;
	while (rd < len && now() < end) {
		struct pollfd p = { .fd = fd, .events = POLLIN };
		ssize_t n;

		if (wr < len)
			p.events |= POLLOUT;
		if (poll(&p, 1, 20) < 0 && errno != EINTR)
			break;
		if (p.revents & POLLOUT) {
			n = write(fd, tx + wr, len - wr);
			if (n > 0)
				wr += n;
		}
		if (p.revents & POLLIN) {
			n = read(fd, rx + rd, len - rd);
			if (n > 0) {
				if (!rd) {
					*t_first = now();
					*n_first = n;
				}
				rd += n;
				*t_last = now();
			}
		}
	}
	return rd;
}

/* read whatever is pending within @ms milliseconds */
static size_t drain(int fd, int ms)
{
	uint8_t buf[256];
	size_t total = 0;
	struct pollfd p = { .fd = fd, .events = POLLIN };

	while (poll(&p, 1, ms) > 0) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n <= 0)
			break;
		total += n;
	}
	return total;
}

static unsigned int baud_of(speed_t sp)
{
	static const unsigned int rates[] = {
		1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200,
		230400, 460800, 921600,
	};
	unsigned int i;

	for (i = 0; i < sizeof(rates) / sizeof(rates[0]); i++)
		if (speed_of(rates[i]) == sp)
			return rates[i];
	return 0;
}

/* print the driver's own view of the rate, if debugfs is there */
static void show_driver_state(void)
{
	FILE *f = fopen("/sys/kernel/debug/1da00000.usif/state", "r");
	char line[160];

	if (!f) {
		printf("         (no debugfs state file)\n");
		return;
	}
	while (fgets(line, sizeof(line), f))
		if (!strncmp(line, "baud", 4) || !strncmp(line, "divider", 7))
			printf("         %s", line);
	fclose(f);
}

/* -r: apply a rate like a loader would, listen one second, never write */
static int set_only(int fd, unsigned int baud)
{
	struct termios t;
	size_t got;

	if (set_rate(fd, baud)) {
		perror("tcsetattr");
		return 0;
	}
	if (tcgetattr(fd, &t)) {
		perror("tcgetattr");
		return 0;
	}
	printf("%7u Bd: accepted, reads back %u/%u Bd\n", baud,
	       baud_of(cfgetispeed(&t)), baud_of(cfgetospeed(&t)));
	show_driver_state();
	got = drain(fd, 1000);
	printf("%7u Bd: %zu bytes received in about 1 s (nothing sent)\n",
	       baud, got);
	return baud_of(cfgetospeed(&t)) == baud;
}

static int run_rate(int fd, unsigned int baud)
{
	struct serial_icounter_struct ic0, ic1;
	size_t len = baud / 10, i, got, n_first, bad = 0;
	double char_s = 10.0 / baud, t0, t_first, t_last;
	uint8_t one = 0x5a, back, *tx, *rx;
	uint32_t lcg = 0x12345678 ^ baud;
	int ok = 1, have_ic;
	unsigned int k;

	if (len < 960)
		len = 960;
	tx = malloc(len);
	rx = malloc(len);
	if (!tx || !rx)
		return 0;
	for (i = 0; i < len; i++) {
		lcg = lcg * 1103515245u + 12345u;
		tx[i] = lcg >> 24;
	}

	/* queue something unread, then change the rate: it must be gone */
	if (write(fd, "stale", 5) != 5)
		perror("write");
	usleep(200000);
	if (set_rate(fd, baud)) {
		perror("tcsetattr");
		return 0;
	}
	printf("%7u Bd: TCSAFLUSH left %zu stale bytes\n", baud, drain(fd, 50));
	have_ic = !ioctl(fd, TIOCGICOUNT, &ic0);

	/* one byte */
	t0 = now();
	got = xfer(fd, &one, &back, 1, 1.0, &t_first, &n_first, &t_last);
	if (got == 1 && back == one) {
		printf("%7u Bd: 1 byte echoed after %.2f ms (%.1f character times)\n",
		       baud, (t_last - t0) * 1e3, (t_last - t0) / char_s);
	} else {
		printf("%7u Bd: 1 byte echo FAILED (got %zu, 0x%02x)\n",
		       baud, got, got ? back : 0);
		ok = 0;
	}

	/* partial words: 1..9 bytes */
	for (k = 1; k <= 9; k++) {
		got = xfer(fd, tx, rx, k, 1.0, &t_first, &n_first, &t_last);
		if (got != k || memcmp(tx, rx, k)) {
			printf("%7u Bd: %u-byte echo FAILED (got %zu)\n",
			       baud, k, got);
			ok = 0;
		}
	}
	if (ok)
		printf("%7u Bd: 1..9 byte echoes ok\n", baud);

	/* bulk */
	got = xfer(fd, tx, rx, len, 3.0 + 3.0 * len * char_s, &t_first,
		   &n_first, &t_last);
	for (i = 0; i < got; i++)
		bad += tx[i] != rx[i];
	printf("%7u Bd: bulk %zu/%zu bytes back, %zu differ", baud, got, len,
	       bad);
	if (got > n_first && t_last > t_first) {
		double eff = 10.0 * (got - n_first) / (t_last - t_first);

		printf(", effective %.0f Bd (%+.2f %%)", eff,
		       100.0 * (eff - baud) / baud);
	}
	printf("\n");
	if (got != len || bad)
		ok = 0;

	if (have_ic && !ioctl(fd, TIOCGICOUNT, &ic1)) {
		printf("%7u Bd: icount rx +%d tx +%d overrun +%d frame +%d\n",
		       baud, ic1.rx - ic0.rx, ic1.tx - ic0.tx,
		       ic1.overrun - ic0.overrun, ic1.frame - ic0.frame);
		if (ic1.overrun != ic0.overrun || ic1.frame != ic0.frame)
			ok = 0;
	}
	printf("%7u Bd: %s\n", baud, ok ? "PASS" : "FAIL");
	free(tx);
	free(rx);
	return ok;
}

int main(int argc, char **argv)
{
	static const unsigned int def[] = {
		9600, 19200, 115200, 230400, 460800, 9600,
	};
	unsigned int rates[32];
	const char *dev = "/dev/ttyUSIF0";
	int fd, i, n = 0, all = 1, rates_only = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-d") && i + 1 < argc)
			dev = argv[++i];
		else if (!strcmp(argv[i], "-r"))
			rates_only = 1;
		else if (n < (int)(sizeof(rates) / sizeof(rates[0])))
			rates[n++] = strtoul(argv[i], NULL, 0);
	}
	if (!n)
		for (; n < (int)(sizeof(def) / sizeof(def[0])); n++)
			rates[n] = def[n];

	fd = open(dev, O_RDWR | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		perror(dev);
		return 2;
	}
	/* raw before anything else: no echo of whatever may arrive */
	if (set_rate(fd, rates[0])) {
		perror("tcsetattr");
		return 2;
	}
	for (i = 0; i < n; i++)
		all &= rates_only ? set_only(fd, rates[i]) :
				    run_rate(fd, rates[i]);
	close(fd);
	printf("%s\n", all ? "ALL PASS" : "SOME FAILED");
	return all ? 0 : 1;
}
