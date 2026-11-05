// h831-fe-test.c — deterministic direct-frontend tune + status probe
// for the AVerMedia H831 bring-up. Bypasses w_scan2's adapter
// auto-detection entirely: every open/ioctl failure prints its errno.
//
// usage:
//   ./h831-fe-test                  # defaults: 674000000, 8 MHz, DVB-T2
//   ./h831-fe-test 522000000 8 T    # DVB-T, if the client wants T1
// Multiple frequencies are welcome: 474 482 ... 858 (UHF grid). The
// per-frequency verdict plus the dmesg mxl601 PLL lines is the
// diagnostic.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <time.h>
#include <stdint.h>
#include <linux/dvb/frontend.h>

static void msleep(long ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000 };
	nanosleep(&ts, NULL);
}

int main(int argc, char **argv)
{
	struct dtv_property p[10] = {};
	struct dtv_properties cmdq;
	int fd, n = 0, i, locked = 0, ret;
	char *node = "/dev/dvb/adapter0/frontend0";
	uint32_t freq = argc > 1 ? strtoul(argv[1], NULL, 10) : 674000000;
	uint32_t bwmhz = argc > 2 ? strtoul(argv[2], NULL, 10) : 8;
	int use_t2 = argc > 3 ? (argv[3][0] == '2') : 1;

#define ADD(cmd__, val__) do { p[n].cmd = (cmd__); p[n].u.data = (val__); n++; } while (0)
	ADD(DTV_DELIVERY_SYSTEM, use_t2 ? SYS_DVBT2 : SYS_DVBT);
	ADD(DTV_FREQUENCY, freq);
	ADD(DTV_BANDWIDTH_HZ, bwmhz * 1000000);
	ADD(DTV_INVERSION, INVERSION_AUTO);
	ADD(DTV_MODULATION, QAM_AUTO);
	ADD(DTV_TRANSMISSION_MODE, TRANSMISSION_MODE_AUTO);
	ADD(DTV_GUARD_INTERVAL, GUARD_INTERVAL_AUTO);
	ADD(DTV_STREAM_ID, 0);		/* DVB-T2 PLP id; ignored for DVB-T */
	/* DTV_TUNE is what actually calls dtv_set_frontend(); without it
	 * FE_SET_PROPERTY only fills the property cache and never tunes. */
	ADD(DTV_TUNE, 0);
#undef ADD

	fd = open(node, O_RDWR | O_NONBLOCK);
	if (fd < 0) {
		fprintf(stderr, "open(%s): %s (errno %d)\n",
			node, strerror(errno), errno);
		return 1;
	}

	cmdq.num = n;
	cmdq.props = p;
	if (ioctl(fd, FE_SET_PROPERTY, &cmdq) < 0) {
		fprintf(stderr, "FE_SET_PROPERTY: %s (errno %d)\n",
			strerror(errno), errno);
		close(fd);
		return 1;
	}

	/* Drain the FE event queue: a completed retune cycle by the DVB
	 * core thread leaves exactly one event here. Empty queue plus
	 * silence means the thread never attempted set_frontend. */
	{
		struct dvb_frontend_event ev;
		int nev = 0;

		do {
			ret = ioctl(fd, FE_GET_EVENT, &ev);
			if (ret == 0) {
				nev++;
				printf("event %d: status=0x%02x parameters.frequency=%u\n",
				       nev, ev.status, ev.parameters.frequency);
			}
		} while (ret == 0);
		if (errno == EWOULDBLOCK)
			printf("FE_GET_EVENT: queue %s (errno %d EWOULDBLOCK)\n",
			       nev ? "drained" : "EMPTY — no retune event", errno);
		else
			printf("FE_GET_EVENT: %s (errno %d)\n",
			       strerror(errno), errno);
	}

	printf("tuned %s: freq=%u bw=%uMHz sys=%s — polling status 20 s\n",
	       node, freq, bwmhz, use_t2 ? "DVB-T2" : "DVB-T");

	for (i = 0; i < 40; i++) {
		fe_status_t st = 0;

		if (ioctl(fd, FE_READ_STATUS, &st) < 0) {
			fprintf(stderr, "FE_READ_STATUS: %s (errno %d)\n",
				strerror(errno), errno);
			close(fd);
			return 1;
		}
		printf("t+%2.1fs status=0x%02x%s%s%s%s%s\n",
		       i * 0.5,
		       st,
		       (st & FE_HAS_SIGNAL)  ? " SIGNAL"  : "",
		       (st & FE_HAS_CARRIER) ? " CARRIER" : "",
		       (st & FE_HAS_VITERBI) ? " VITERBI" : "",
		       (st & FE_HAS_SYNC)    ? " SYNC"    : "",
		       (st & FE_HAS_LOCK)    ? " LOCK"    : "");
		if (st & FE_HAS_LOCK) {
			locked = 1;
			break;
		}
		msleep(500);
	}

	/* Second drain: catches retune events that completed during the
	 * 20 s status polling. */
	{
		struct dvb_frontend_event ev;
		int nev = 0;

		do {
			ret = ioctl(fd, FE_GET_EVENT, &ev);
			if (ret == 0) {
				nev++;
				printf("late event %d: status=0x%02x parameters.frequency=%u\n",
				       nev, ev.status, ev.parameters.frequency);
			}
		} while (ret == 0);
		if (errno == EWOULDBLOCK && !nev)
			printf("FE_GET_EVENT: still EMPTY after 20 s — thread never retuned\n");
	}

	printf("RESULT: %s freq=%u\n",
	       locked ? "LOCKED" : "NO LOCK", freq);
	close(fd);
	return locked ? 0 : 2;
}
