/*
 * Copyright (c) 2000-2004 Niels Provos <provos@citi.umich.edu>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#ifdef WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#undef WIN32_LEAN_AND_MEAN
#endif
#include <sys/types.h>
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#else
#include <sys/_libevent_time.h>
#endif
#include <sys/queue.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef WIN32
#include <unistd.h>
#endif
#include <errno.h>
#include <signal.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#include "event.h"
#include "event-internal.h"
#include "evutil.h"
#include "log.h"

#ifdef HAVE_EVENT_PORTS
extern const struct eventop evportops;
#endif
#ifdef HAVE_SELECT
extern const struct eventop selectops;
#endif
#ifdef HAVE_POLL
extern const struct eventop pollops;
#endif
#ifdef HAVE_EPOLL
extern const struct eventop epollops;
#endif
#ifdef HAVE_WORKING_KQUEUE
extern const struct eventop kqops;
#endif
#ifdef HAVE_DEVPOLL
extern const struct eventop devpollops;
#endif
#ifdef WIN32
extern const struct eventop win32ops;
#endif

/* In order of preference */
static const struct eventop *eventops[] = {
#ifdef HAVE_EVENT_PORTS
	&evportops,
#endif
#ifdef HAVE_WORKING_KQUEUE
	&kqops,
#endif
#ifdef HAVE_EPOLL
	&epollops,
#endif
#ifdef HAVE_DEVPOLL
	&devpollops,
#endif
#ifdef HAVE_POLL
	&pollops,
#endif
#ifdef HAVE_SELECT
	&selectops,
#endif
#ifdef WIN32
	&win32ops,
#endif
	NULL
};

/* Global state */
struct event_base *current_base = NULL;
extern struct event_base *evsignal_base;
static int use_monotonic;

/* Handle signals - This is a deprecated interface */
int (*event_sigcb)(void);		/* Signal callback when gotsig is set */
volatile sig_atomic_t event_gotsig;	/* Set in signal handler */

/* Prototypes */
static void	event_queue_insert(struct event_base *, struct event *, int);
static void	event_queue_remove(struct event_base *, struct event *, int);
static int	event_haveevents(struct event_base *);

static void	event_process_active(struct event_base *);

static int	timeout_next(struct event_base *, struct timeval **);
static void	timeout_process(struct event_base *);
static void	timeout_correct(struct event_base *, struct timeval *);

static void
detect_monotonic(void)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	struct timespec	ts;

	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		use_monotonic = 1;
#endif
}


// 得到当前系统时间
static int
gettime(struct event_base *base, struct timeval *tp)
{
	if (base->tv_cache.tv_sec) {
		*tp = base->tv_cache;
		return (0);
	}

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
	if (use_monotonic) {
		struct timespec	ts;

		if (clock_gettime(CLOCK_MONOTONIC, &ts) == -1)
			return (-1);

		tp->tv_sec = ts.tv_sec;
		tp->tv_usec = ts.tv_nsec / 1000;
		return (0);
	}
#endif

	return (evutil_gettimeofday(tp, NULL));
}

struct event_base *
event_init(void)
{
	struct event_base *base = event_base_new();

	if (base != NULL)
		current_base = base;

	return (base);
}

struct event_base *
event_base_new(void)
{
	int i;
	struct event_base *base;

	if ((base = calloc(1, sizeof(struct event_base))) == NULL)
		event_err(1, "%s: calloc", __func__);

	event_sigcb = NULL;
	event_gotsig = 0;

	detect_monotonic();
	gettime(base, &base->event_tv);

	min_heap_ctor(&base->timeheap);
	TAILQ_INIT(&base->eventqueue);
	base->sig.ev_signal_pair[0] = -1;
	base->sig.ev_signal_pair[1] = -1;

    // 选一个最合适的多路复用系统api
	base->evbase = NULL;
	for (i = 0; eventops[i] && !base->evbase; i++) {
		base->evsel = eventops[i];

		base->evbase = base->evsel->init(base);
	}

	if (base->evbase == NULL)
		event_errx(1, "%s: no event mechanism available", __func__);

	if (evutil_getenv("EVENT_SHOW_METHOD"))
		event_msgx("libevent using: %s\n",
			   base->evsel->name);

	/* allocate a single active event queue */
	event_base_priority_init(base, 1);

	return (base);
}

void
event_base_free(struct event_base *base)
{
	int i, n_deleted=0;
	struct event *ev;

	if (base == NULL && current_base)
		base = current_base;
	if (base == current_base)
		current_base = NULL;

	/* XXX(niels) - check for internal events first */
	assert(base);
	/* Delete all non-internal events. */
	for (ev = TAILQ_FIRST(&base->eventqueue); ev; ) {
		struct event *next = TAILQ_NEXT(ev, ev_next);
		if (!(ev->ev_flags & EVLIST_INTERNAL)) {
			event_del(ev);
			++n_deleted;
		}
		ev = next;
	}
	while ((ev = min_heap_top(&base->timeheap)) != NULL) {
		event_del(ev);
		++n_deleted;
	}

	for (i = 0; i < base->nactivequeues; ++i) {
		for (ev = TAILQ_FIRST(base->activequeues[i]); ev; ) {
			struct event *next = TAILQ_NEXT(ev, ev_active_next);
			if (!(ev->ev_flags & EVLIST_INTERNAL)) {
				event_del(ev);
				++n_deleted;
			}
			ev = next;
		}
	}

	if (n_deleted)
		event_debug(("%s: %d events were still set in base",
			__func__, n_deleted));

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);

	for (i = 0; i < base->nactivequeues; ++i)
		assert(TAILQ_EMPTY(base->activequeues[i]));

	assert(min_heap_empty(&base->timeheap));
	min_heap_dtor(&base->timeheap);

	for (i = 0; i < base->nactivequeues; ++i)
		free(base->activequeues[i]);
	free(base->activequeues);

	assert(TAILQ_EMPTY(&base->eventqueue));

	free(base);
}

/* reinitialized the event base after a fork */
int
event_reinit(struct event_base *base)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	int res = 0;
	struct event *ev;

#if 0
	/* Right now, reinit always takes effect, since even if the
	   backend doesn't require it, the signal socketpair code does.
	 */
	/* check if this event mechanism requires reinit */
	if (!evsel->need_reinit)
		return (0);
#endif

	/* prevent internal delete */
	if (base->sig.ev_signal_added) {
		/* we cannot call event_del here because the base has
		 * not been reinitialized yet. */
		event_queue_remove(base, &base->sig.ev_signal,
		    EVLIST_INSERTED);
		if (base->sig.ev_signal.ev_flags & EVLIST_ACTIVE)
			event_queue_remove(base, &base->sig.ev_signal,
			    EVLIST_ACTIVE);
		base->sig.ev_signal_added = 0;
	}

	if (base->evsel->dealloc != NULL)
		base->evsel->dealloc(base, base->evbase);
	evbase = base->evbase = evsel->init(base);
	if (base->evbase == NULL)
		event_errx(1, "%s: could not reinitialize event mechanism",
		    __func__);

	TAILQ_FOREACH(ev, &base->eventqueue, ev_next) {
		if (evsel->add(evbase, ev) == -1)
			res = -1;
	}

	return (res);
}

int
event_priority_init(int npriorities)
{
  return event_base_priority_init(current_base, npriorities);
}

// 优先级队列
int
event_base_priority_init(struct event_base *base, int npriorities)
{
	int i;
    // 优先级队列中有活动事件不进行处理
	if (base->event_count_active)
		return (-1);
    // 未更改优先级队列数
	if (npriorities == base->nactivequeues)
		return (0);

    // 释放所有优先级队列
	if (base->nactivequeues) {
		for (i = 0; i < base->nactivequeues; ++i) {
			free(base->activequeues[i]); // 单个队列
		}
        // 队列数组
		free(base->activequeues);
	}

	/* Allocate our priority queues */
	base->nactivequeues = npriorities;
    // 分配链表数组
	base->activequeues = (struct event_list **)
	    calloc(base->nactivequeues, sizeof(struct event_list *));
	if (base->activequeues == NULL)
		event_err(1, "%s: calloc", __func__);

	for (i = 0; i < base->nactivequeues; ++i) {
		base->activequeues[i] = malloc(sizeof(struct event_list));// 分配一个链表头
		if (base->activequeues[i] == NULL)
			event_err(1, "%s: malloc", __func__);
		TAILQ_INIT(base->activequeues[i]);
	}

	return (0);
}

int
event_haveevents(struct event_base *base)
{
	return (base->event_count > 0);
}

/*
 * Active events are stored in priority queues.  Lower priorities are always
 * process before higher priorities.  Low priority events can starve high
 * priority ones.
 */
// 处理所有活动队列里的事件，对每个事件调用回调函数
static void
event_process_active(struct event_base *base)
{
	struct event *ev;
	struct event_list *activeq = NULL;
	int i;
	short ncalls;
	//取得数组nactivequeues最靠前的一个非NULL元素
	//即优先级最大的一个活动队列
	for (i = 0; i < base->nactivequeues; ++i) {
		if (TAILQ_FIRST(base->activequeues[i]) != NULL) {
			activeq = base->activequeues[i];
			break;
		}
	}

	assert(activeq != NULL);
	//一次对该最大优先级队列的event进行回调
	//优先级小的等到下次被调用时处理
	for (ev = TAILQ_FIRST(activeq); ev; ev = TAILQ_FIRST(activeq)) {
        // 这里处理EV_PERSIST的方式是仅从活动队列删除，没有从event_loop从删除
		if (ev->ev_events & EV_PERSIST)
			event_queue_remove(base, ev, EVLIST_ACTIVE);
		else
			event_del(ev);

		/* Allows deletes to work */
        // 回调过程中，允许event的回调删除删除自己
		ncalls = ev->ev_ncalls;
		ev->ev_pncalls = &ncalls;
		while (ncalls) {
			ncalls--;
			ev->ev_ncalls = ncalls;
			(*ev->ev_callback)((int)ev->ev_fd, ev->ev_res, ev->ev_arg);
            // 收到中断事件或者退出
			if (event_gotsig || base->event_break) {
			  	ev->ev_pncalls = NULL;
				return;
			}
		}
		ev->ev_pncalls = NULL;
	}
}

/*
 * Wait continously for events.  We exit only if no events are left.
 */

int
event_dispatch(void)
{
	return (event_loop(0));
}

int
event_base_dispatch(struct event_base *event_base)
{
  return (event_base_loop(event_base, 0));
}

const char *
event_base_get_method(struct event_base *base)
{
	assert(base);
	return (base->evsel->name);
}

static void
event_loopexit_cb(int fd, short what, void *arg)
{
	struct event_base *base = arg;
	base->event_gotterm = 1;
}

/* not thread safe */
int
event_loopexit(const struct timeval *tv)
{
	return (event_once(-1, EV_TIMEOUT, event_loopexit_cb,
		    current_base, tv));
}

int
event_base_loopexit(struct event_base *event_base, const struct timeval *tv)
{
	return (event_base_once(event_base, -1, EV_TIMEOUT, event_loopexit_cb,
		    event_base, tv));
}

/* not thread safe */
int
event_loopbreak(void)
{
	return (event_base_loopbreak(current_base));
}

int
event_base_loopbreak(struct event_base *event_base)
{
	if (event_base == NULL)
		return (-1);

	event_base->event_break = 1;
	return (0);
}



/* not thread safe */

int
event_loop(int flags)
{
	return event_base_loop(current_base, flags);
}

int
event_base_loop(struct event_base *base, int flags)
{
	const struct eventop *evsel = base->evsel;
	void *evbase = base->evbase;
	struct timeval tv;
	struct timeval *tv_p;
	int res, done;

	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	if (base->sig.ev_signal_added)
		evsignal_base = base;
	done = 0;
	while (!done) {
		/* Terminate the loop if we have been asked to */
		if (base->event_gotterm) {
			base->event_gotterm = 0;
			break;
		}

		if (base->event_break) {
			base->event_break = 0;
			break;
		}

		/* You cannot use this interface for multi-threaded apps */
		while (event_gotsig) {
			event_gotsig = 0;
			if (event_sigcb) {
				res = (*event_sigcb)();
				if (res == -1) {
					errno = EINTR;
					return (-1);
				}
			}
		}

		timeout_correct(base, &tv);

		tv_p = &tv;
        // 有活动事件或者设置event_loop为非阻塞的话，不会计算计时器触发时间
        // 并进入睡眠，这么做可能导致cpu占用率高
		if (!base->event_count_active && !(flags & EVLOOP_NONBLOCK)) {
			 //获取到base->timeheap中最先超时的计时器
			 //如果没有计时器tv_p被赋值NULL，注意参数是timeval**
			 //否则
			timeout_next(base, &tv_p);
		} else {
			/*
			 * if we have active events, we just poll new events
			 * without waiting.
			 */
			evutil_timerclear(&tv);
		}

		/* If we have no events, we just exit */
		if (!event_haveevents(base)) {//内核中没有要监听的事件退出
			event_debug(("%s: no events registered.", __func__));
			return (1);
		}

		/* update last old time */
        // 更新event_loop时间
		gettime(base, &base->event_tv);

		/* clear time cache */
		base->tv_cache.tv_sec = 0;
		//调用OS的I/O分发，tv_p表示超时的时间(如果不为NULL)
		//比如如果OS的I/O分发采用select，那么tv_p相当告诉select超时的时间，即正好是我们
		//添加到base->timeheap最先超时的event的时间(最小堆堆顶时间最靠前)
		res = evsel->dispatch(base, evbase, tv_p);

		if (res == -1)
			return (-1);
        // base->tv_cache - base->event_tv就是dispatch使用的时间
		gettime(base, &base->tv_cache);
		//处理超时的event，通过获取最小堆堆顶与当前时间比较是否超时
		//如果超时则将event插入到base->activequeues并将base->event_count_active加1
		timeout_process(base);

        // 活动队列有事件的话，统一进行处理
		if (base->event_count_active) {
			event_process_active(base);
			if (!base->event_count_active && (flags & EVLOOP_ONCE))
				done = 1;
		} else if (flags & EVLOOP_NONBLOCK)
			done = 1;
	}

	/* clear time cache */
	base->tv_cache.tv_sec = 0;

	event_debug(("%s: asked to terminate loop.", __func__));
	return (0);
}

/* Sets up an event for processing once */

struct event_once {
	struct event ev;

	void (*cb)(int, short, void *);
	void *arg;
};

/* One-time callback, it deletes itself */

static void
event_once_cb(int fd, short events, void *arg)
{
	struct event_once *eonce = arg;

	(*eonce->cb)(fd, events, eonce->arg);
	free(eonce);
}

/* not threadsafe, event scheduled once. */
int
event_once(int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	return event_base_once(current_base, fd, events, callback, arg, tv);
}

/* Schedules an event once */
int
event_base_once(struct event_base *base, int fd, short events,
    void (*callback)(int, short, void *), void *arg, const struct timeval *tv)
{
	struct event_once *eonce;
	struct timeval etv;
	int res;

	/* We cannot support signals that just fire once */
	if (events & EV_SIGNAL)
		return (-1);

	if ((eonce = calloc(1, sizeof(struct event_once))) == NULL)
		return (-1);

	eonce->cb = callback;
	eonce->arg = arg;
    // 设置event_once_cb作为回调，然后event_once_cb再调用eonce->callback(arg)
	if (events == EV_TIMEOUT) {
		if (tv == NULL) {
			evutil_timerclear(&etv);
			tv = &etv;
		}

		evtimer_set(&eonce->ev, event_once_cb, eonce);
	} else if (events & (EV_READ|EV_WRITE)) {
		events &= EV_READ|EV_WRITE;

		event_set(&eonce->ev, fd, events, event_once_cb, eonce);
	} else {
		/* Bad event combination */
		free(eonce);
		return (-1);
	}

	res = event_base_set(base, &eonce->ev);
	if (res == 0)
		res = event_add(&eonce->ev, tv);
	if (res != 0) {
		free(eonce);
		return (res);
	}

	return (0);
}
//设置并初始化event
//ev  注册的event
// fd  文件描述符，如果是timeout event则忽略该参数
//events  关心的事件 EV_TIMEOUT, EV_SIGNAL,  EV_READ, or EV_WRITE可以通过或运算符同时关心多个事件，发生
//            对应事件callback将会被调用
//callback 回调函数，被回调时fd和arg会传给它callback(fd, arg)
//arg  传给callback的自定义参数
void
event_set(struct event *ev, int fd, short events,
	  void (*callback)(int, short, void *), void *arg)
{
	/* Take the current base - caller needs to set the real base later */
	ev->ev_base = current_base; //默认对给全局reactor实例进行io分发

	ev->ev_callback = callback;
	ev->ev_arg = arg;
	ev->ev_fd = fd;
	ev->ev_events = events;
	ev->ev_res = 0;
	ev->ev_flags = EVLIST_INIT;
	ev->ev_ncalls = 0;
	ev->ev_pncalls = NULL;

	min_heap_elem_init(ev);

	/* by default, we put new events into the middle priority */
	if(current_base)
		ev->ev_pri = current_base->nactivequeues/2;
}


// 将event归属与某个event_loop
int
event_base_set(struct event_base *base /*分发该ev的特定reactor*/, struct event *ev)
{
	/* Only innocent events may be assigned to a different base */
	if (ev->ev_flags != EVLIST_INIT)
		return (-1);

	ev->ev_base = base;
	ev->ev_pri = base->nactivequeues/2;

	return (0);
}

/*
 * Set's the priority of an event - if an event is already scheduled
 * changing the priority is going to fail.
 */

int
event_priority_set(struct event *ev, int pri)
{
	if (ev->ev_flags & EVLIST_ACTIVE)
		return (-1);
	if (pri < 0 || pri >= ev->ev_base->nactivequeues)
		return (-1);

	ev->ev_pri = pri;

	return (0);
}

/*
 * Checks if a specific event is pending or scheduled.
 */
// 检查某个event当前状态，是否已经在event_loop中，
// 以及是什么类型的事件，如果tv非NULL且event中指定
// 有定时器事件，那么tv返回定时器发生的时间点
int
event_pending(struct event *ev, short event, struct timeval *tv)
{
	struct timeval	now, res;
	int flags = 0;

	if (ev->ev_flags & EVLIST_INSERTED)
		flags |= (ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL));
	if (ev->ev_flags & EVLIST_ACTIVE)
		flags |= ev->ev_res;
	if (ev->ev_flags & EVLIST_TIMEOUT)
		flags |= EV_TIMEOUT;

	event &= (EV_TIMEOUT|EV_READ|EV_WRITE|EV_SIGNAL);

	/* See if there is a timeout that we should report */
	if (tv != NULL && (flags & event & EV_TIMEOUT)) {
		gettime(ev->ev_base, &now);
		evutil_timersub(&ev->ev_timeout, &now, &res);
		/* correctly remap to real time */
		evutil_gettimeofday(&now, NULL);
		evutil_timeradd(&now, &res, tv);
	}

	return (flags & event);
}

int
event_add(struct event *ev, const struct timeval *tv)
{
	struct event_base *base = ev->ev_base;//获得与该事件关联的event_base
	const struct eventop *evsel = base->evsel; //底层与OS相关的I/O分发模式
	void *evbase = base->evbase;//底层与OS相关的I/O分发模式参数
	int res = 0;

	event_debug((
		 "event_add: event: %p, %s%s%scall %p",
		 ev,
		 ev->ev_events & EV_READ ? "EV_READ " : " ",
		 ev->ev_events & EV_WRITE ? "EV_WRITE " : " ",
		 tv ? "EV_TIMEOUT " : " ",
		 ev->ev_callback));

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/*
	 * prepare for timeout insertion further below, if we get a
	 * failure on any step, we should not change any state.
	 */
	 //如果该事件有计时器选项(tv不为NULL)
	 //预先在二叉堆中预留一个空位给新添加的event
	if (tv != NULL && !(ev->ev_flags & EVLIST_TIMEOUT)) {
		if (min_heap_reserve(&base->timeheap,
			1 + min_heap_size(&base->timeheap)) == -1)
			return (-1);  /* ENOMEM == errno */
	}


	if ((ev->ev_events & (EV_READ|EV_WRITE|EV_SIGNAL)) && //如果监听有非超时event
	    !(ev->ev_flags & (EVLIST_INSERTED|EVLIST_ACTIVE))) { //且未插入到libevent
		res = evsel->add(evbase, ev);//添加到OS的IO分发reactor
		if (res != -1)
			event_queue_insert(base, ev, EVLIST_INSERTED);// 插入event loop总的队列中
	}

	/*
	 * we should change the timout state only if the previous event
	 * addition succeeded.
	 */
	// 比较麻烦的是定时器的处理
	if (res != -1 && tv != NULL) {
		struct timeval now;

		/*
		 * we already reserved memory above for the case where we
		 * are not replacing an exisiting timeout.
		 */
		 // 如果定时器已经在定时队列中，先移除，主要发生在定时器已经插入
		 // event loop，想要修改定时器的发生时间
		if (ev->ev_flags & EVLIST_TIMEOUT)
			event_queue_remove(base, ev, EVLIST_TIMEOUT);

		/* Check if it is active due to a timeout.  Rescheduling
		 * this timeout before the callback can be executed
		 * removes it from the active list. */
		 // 如果定时器在活动队列中，立即将其中活动队列中移除
		if ((ev->ev_flags & EVLIST_ACTIVE) &&
		    (ev->ev_res & EV_TIMEOUT)) {
			/* See if we are just active executing this
			 * event in a loop
			 */
			if (ev->ev_ncalls && ev->ev_pncalls) {
				/* Abort loop */
				*ev->ev_pncalls = 0;
			}

			event_queue_remove(base, ev, EVLIST_ACTIVE);
		}

        // 得到定时器发生的时间点
		gettime(base, &now);
		evutil_timeradd(&now, tv, &ev->ev_timeout);

		event_debug((
			 "event_add: timeout in %ld seconds, call %p",
			 tv->tv_sec, ev->ev_callback));
        // 插入定时器
		event_queue_insert(base, ev, EVLIST_TIMEOUT);
	}

	return (res);
}

// 从event loop 中删除event
int
event_del(struct event *ev)
{
	struct event_base *base;
	const struct eventop *evsel;
	void *evbase;

	event_debug(("event_del: %p, callback %p",
		 ev, ev->ev_callback));

	/* An event without a base has not been added */
	if (ev->ev_base == NULL)
		return (-1);

	base = ev->ev_base;
	evsel = base->evsel;
	evbase = base->evbase;

	assert(!(ev->ev_flags & ~EVLIST_ALL));

	/* See if we are just active executing this event in a loop */
    // 如果正在回调，中止继续回调，因为如果
    // ev事件发生在回调函数中可能会删除自己，
    // 使用ev_pncalls就是这个原因
	if (ev->ev_ncalls && ev->ev_pncalls) {
		/* Abort loop */
		*ev->ev_pncalls = 0;
	}

	if (ev->ev_flags & EVLIST_TIMEOUT)
		event_queue_remove(base, ev, EVLIST_TIMEOUT);

	if (ev->ev_flags & EVLIST_ACTIVE)
		event_queue_remove(base, ev, EVLIST_ACTIVE);

	if (ev->ev_flags & EVLIST_INSERTED) {
		event_queue_remove(base, ev, EVLIST_INSERTED);
		return (evsel->del(evbase, ev));
	}

	return (0);
}

// 将event发到活动队列中
void
event_active(struct event *ev, int res, short ncalls)
{
	/* We get different kinds of events, add them together */
    // 如果已经在活动队列里面，只要
    // 更新一下事件结果标志位
	if (ev->ev_flags & EVLIST_ACTIVE) {
        // 用或运算，因为如果在活动队列中，
        // 就说明之前已经发生过某事件，ev->ev_res非0
		ev->ev_res |= res;
		return;
	}

    // 直接赋值assert(ev->ev_res == 0)
	ev->ev_res = res;
	ev->ev_ncalls = ncalls;
	ev->ev_pncalls = NULL;
	event_queue_insert(ev->ev_base, ev, EVLIST_ACTIVE);
}

// 得到计时器距离触发时的最小时间间隔
// 用于事件循环应该睡眠等待多长时间
static int
timeout_next(struct event_base *base, struct timeval **tv_p/*指针 的指针*/)
{
	struct timeval now;
	struct event *ev;
	struct timeval *tv = *tv_p; // 注意指向tv_p

    // 没有计时器
	if ((ev = min_heap_top(&base->timeheap)) == NULL) {
		/* if no time-based events are active wait for I/O */
		*tv_p = NULL; // 变为NULL说明没有计时器
		return (0);
	}

	if (gettime(base, &now) == -1)
		return (-1);

    // 已经有计时器达到触发时间
	if (evutil_timercmp(&ev->ev_timeout, &now, <=)) {
		evutil_timerclear(tv);
		return (0);
	}

    // 得到距离触发的最小时间，返回到tv_p变量中
	evutil_timersub(&ev->ev_timeout, &now, tv);

	assert(tv->tv_sec >= 0);
	assert(tv->tv_usec >= 0);

	event_debug(("timeout_next: in %ld seconds", tv->tv_sec));
	return (0);
}

/*
 * Determines if the time is running backwards by comparing the current
 * time against the last time we checked.  Not needed when using clock
 * monotonic.
 */

static void
timeout_correct(struct event_base *base, struct timeval *tv)
{
	struct event **pev;
	unsigned int size;
	struct timeval off;

	if (use_monotonic)
		return;

	/* Check if time is running backwards */
	gettime(base, tv);
    // 比event_loop时间大，时间已经往后走
	if (evutil_timercmp(tv, &base->event_tv, >=)) {
		base->event_tv = *tv;
		return;
	}

	event_debug(("%s: time is running backwards, corrected",
		    __func__));
    // 发生在手动修改系统时间或者时间错误，
    // 导致当前时间小于之前设置的event_tv
	evutil_timersub(&base->event_tv, tv, &off);

	/*
	 * We can modify the key element of the node without destroying
	 * the key, beause we apply it to all in the right order.
	 */
	pev = base->timeheap.p;
	size = base->timeheap.n;
    // 修正所有计时器的发生时间点，添加的时候指定了
    // 触发时间，现在系统时间错误如原来是12:00，变为了
    // 11:50，将所有计时器的触发时间点减去10分钟，off即为
    // 应该修正的时间长度
	for (; size-- > 0; ++pev) {
		struct timeval *ev_tv = &(**pev).ev_timeout;
		evutil_timersub(ev_tv, &off, ev_tv);
	}
	/* Now remember what the new time turned out to be. */
	base->event_tv = *tv;
}

void
timeout_process(struct event_base *base)
{
	struct timeval now;
	struct event *ev;
    // 有无计时器
	if (min_heap_empty(&base->timeheap))
		return;

	gettime(base, &now);

	//比较计时器是否超时
	while ((ev = min_heap_top(&base->timeheap))) {
		//与当前时间比较，如果大于当前时间
		//则说明没有超时，因为是最小堆，发现
		// 堆顶未超时，说明后面的计时器都是
		// 没有超时的
		if (evutil_timercmp(&ev->ev_timeout, &now, >))
			break;

		/* delete this event from the I/O queues */
		event_del(ev);

		event_debug(("timeout_process: call %p",
			 ev->ev_callback));
		//插入到活动队列中
		event_active(ev, EV_TIMEOUT, 1);
	}
}

// 将event从queue指定的链表中删除
void
event_queue_remove(struct event_base *base, struct event *ev, int queue)
{
	if (!(ev->ev_flags & queue))
		event_errx(1, "%s: %p(fd %d) not on queue %x", __func__,
			   ev, ev->ev_fd, queue);

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count--; // 总的event数递减
    // 更改该标志位，说明已经从queue指定的链表删除
	ev->ev_flags &= ~queue;
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_REMOVE(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		base->event_count_active--; // 活动事件队列要递减
		TAILQ_REMOVE(base->activequeues[ev->ev_pri],
		    ev, ev_active_next);
		break;
	case EVLIST_TIMEOUT:
        // 暂时将它抽象为链表
		min_heap_erase(&base->timeheap, ev);
		break;
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

void
event_queue_insert(struct event_base *base, struct event *ev, int queue)
{
	if (ev->ev_flags & queue) {
		/* Double insertion is possible for active events */
		if (queue & EVLIST_ACTIVE)
			return;

		event_errx(1, "%s: %p(fd %d) already on queue %x", __func__,
			   ev, ev->ev_fd, queue);
	}

	if (~ev->ev_flags & EVLIST_INTERNAL)
		base->event_count++;//总的event数递增

    // 更改标志位，说明已经被插入queue指定的链表当中
	ev->ev_flags |= queue;
	switch (queue) {
	case EVLIST_INSERTED:
		TAILQ_INSERT_TAIL(&base->eventqueue, ev, ev_next);
		break;
	case EVLIST_ACTIVE:
		base->event_count_active++;//活动队列递增
		TAILQ_INSERT_TAIL(base->activequeues[ev->ev_pri],
		    ev,ev_active_next);
		break;
	case EVLIST_TIMEOUT: {
		min_heap_push(&base->timeheap, ev);
		break;
	}
	default:
		event_errx(1, "%s: unknown queue %x", __func__, queue);
	}
}

/* Functions for debugging */

const char *
event_get_version(void)
{
	return (VERSION);
}

/*
 * No thread-safe interface needed - the information should be the same
 * for all threads.
 */

const char *
event_get_method(void)
{
	return (current_base->evsel->name);
}
