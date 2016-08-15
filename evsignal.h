/*
 * Copyright 2000-2002 Niels Provos <provos@citi.umich.edu>
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
#ifndef _EVSIGNAL_H_
#define _EVSIGNAL_H_

typedef void (*ev_sighandler_t)(int);

struct evsignal_info {
	struct event ev_signal;
    // ev_signal_pair[0]放入event_loop中监听，当有信号发生的时候，
    // 通过写入ev_signal_pair[1]，可以唤醒并回调与ev_signal_pair[0]关联
    // 的事件回调，这样的作用是可以让所有回调在event_loop中
    // 执行，不用担心中断问题，同时有多个信号发生的时候，
    // 处理更快，因为只需要往ev_signal_pair[1]写入一个字节进行
    // 通知即可，memcache源码中master线程唤醒worker线程处理客户
    // 客户连接也是这么做
	int ev_signal_pair[2];
	int ev_signal_added;
	volatile sig_atomic_t evsignal_caught;
	struct event_list evsigevents[NSIG];
	sig_atomic_t evsigcaught[NSIG];
#ifdef HAVE_SIGACTION
	struct sigaction **sh_old;
#else
	ev_sighandler_t **sh_old;
#endif
	int sh_old_max;
};
int evsignal_init(struct event_base *);
void evsignal_process(struct event_base *);
int evsignal_add(struct event *);
int evsignal_del(struct event *);
void evsignal_dealloc(struct event_base *);

#endif /* _EVSIGNAL_H_ */
