#include "event2/event-config.h"
#include "evconfig-private.h"

// #if defined EVENT__HAVE_IO_URING

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>

#include <sys/types.h>
#include <sys/resource.h>
#ifdef EVENT__HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <sys/queue.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#ifdef EVENT__HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef EVENT__HAVE_SYS_TIMERFD_H
#include <sys/timerfd.h>
#endif
#endif
#include <liburing.h>

#include "event-internal.h"
#include "evsignal-internal.h"
#include "event2/thread.h"
#include "evthread-internal.h"
#include "log-internal.h"
#include "evmap-internal.h"
#include "changelist-internal.h"
#include "time-internal.h"

#define URING_INIT_NEVENTS 32
#define URING_MAX_NEVENTS 4096

static void *io_uring_init(struct event_base *base);
static int io_uring_dispatch(struct event_base *, struct timeval *);
static void io_uring_dealloc(struct event_base *);

struct io_uringop {
	struct io_uring ring;
	int nevents;
	// epoll_handle epfd;
};

const struct eventop uringops = {
	"io_uring",
	io_uring_init,
	epoll_nochangelist_add,
	epoll_nochangelist_del,
	io_uring_dispatch,
	io_uring_dealloc,
	1, /* need reinit */
	EV_FEATURE_ET|EV_FEATURE_O1|EV_FEATURE_EARLY_CLOSE,
	0
};

static void *
io_uring_init(struct event_base *base)
{
    int ret;
	struct io_uringop *op;

	if (!(op = mm_calloc(1, sizeof(struct io_uringop)))) {
		return NULL;
	}

	/* Initialize fields */
    if ((ret = io_uring_queue_init(URING_INIT_NEVENTS, &op->ring, 0)) ret < 0) {
        mm_free(epollop);
		return NULL;
	}
	op->nevents = INITIAL_NEVENT;

	evsig_init_(base);

	return op;
}


static int
io_uring_dispatch(struct event_base *base, struct timeval *tv)
{
	struct io_uring *op = (struct io_uring*)base->evbase;
	// struct epoll_event *events = epollop->events;
	int i, res;
	long timeout = -1;

	if (tv != NULL) {
		timeout = evutil_tv_to_msec_(tv);
		if (timeout < 0 || timeout > MAX_EPOLL_TIMEOUT_MSEC) {
			/* Linux kernels can wait forever if the timeout is
			 * too big; see comment on MAX_EPOLL_TIMEOUT_MSEC. */
			timeout = MAX_EPOLL_TIMEOUT_MSEC;
		}
	}

	epoll_apply_changes(base);
	event_changelist_remove_all_(&base->changelist, base);

	EVBASE_RELEASE_LOCK(base, th_base_lock);

	res = epoll_wait(epollop->epfd, events, epollop->nevents, timeout);

	EVBASE_ACQUIRE_LOCK(base, th_base_lock);

	if (res == -1) {
		if (errno != EINTR) {
			event_warn("epoll_wait");
			return (-1);
		}

		return (0);
	}

	event_debug(("%s: epoll_wait reports %d", __func__, res));
	EVUTIL_ASSERT(res <= epollop->nevents);

	for (i = 0; i < res; i++) {
		int what = events[i].events;
		short ev = 0;
#ifdef USING_TIMERFD
		if (events[i].data.fd == epollop->timerfd)
			continue;
#endif

		if (what & EPOLLERR) {
			ev = EV_READ | EV_WRITE;
		} else if ((what & EPOLLHUP) && !(what & EPOLLRDHUP)) {
			ev = EV_READ | EV_WRITE;
		} else {
			if (what & EPOLLIN)
				ev |= EV_READ;
			if (what & EPOLLOUT)
				ev |= EV_WRITE;
			if (what & EPOLLRDHUP)
				ev |= EV_CLOSED;
		}

		if (!ev)
			continue;

#ifdef EVENT__HAVE_WEPOLL
		evmap_io_active_(base, events[i].data.sock, ev);
#else
		evmap_io_active_(base, events[i].data.fd, ev | EV_ET);
#endif
	}

	if (res == epollop->nevents && epollop->nevents < MAX_NEVENT) {
		/* We used all of the event space this time.  We should
		   be ready for more events next time. */
		int new_nevents = epollop->nevents * 2;
		struct epoll_event *new_events;

		new_events = mm_realloc(epollop->events,
		    new_nevents * sizeof(struct epoll_event));
		if (new_events) {
			epollop->events = new_events;
			epollop->nevents = new_nevents;
		}
	}

	return (0);
}

static void
io_uring_dealloc(struct event_base *base)
{
	struct io_uring *op = (struct io_uring*)base->evbase;

	evsig_dealloc_(base);

    io_uring_queue_exit(&op->ring);

	memset(op, 0, sizeof(struct io_uring));
	mm_free(op);
}

// #endif /* defined EVENT__HAVE_IO_URING */
