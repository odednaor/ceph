// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 UnitedStack <haomai@unitedstack.com>
 *
 * Author: Haomai Wang <haomaiwang@gmail.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "common/errno.h"
#include "EventKqueue.h"

#define dout_subsys ceph_subsys_ms

#undef dout_prefix
#define dout_prefix *_dout << "KqueueDriver."

#define KEVENT_NOWAIT 0

int KqueueDriver::test_kqfd() {
  struct kevent ke[1];
  if (kevent(kqfd, ke, 0, NULL, 0, NULL) == -1) {
    ldout(cct,0) << __func__ << " invalid kqfd = " << kqfd 
                 << cpp_strerror(errno) << dendl;
    return -errno;
  }
  return kqfd;
}

int KqueueDriver::restore_events() {
  struct kevent ke[2];
  int i;
  int num = 0;

  ldout(cct,10) << __func__ << " on kqfd = " << kqfd << dendl;
  for(i=0;i<size;i++) {
    if (sav_events[i].mask == 0 )
      continue;
    ldout(cct,10) << __func__ << " restore kqfd = " << kqfd 
	<< " fd = " << i << " mask " << sav_events[i].mask
	<< dendl;
    if (sav_events[i].mask & EVENT_READABLE)
      EV_SET(&ke[num++], i, EVFILT_READ, EV_ADD, 0, 0, NULL);
    if (sav_events[i].mask & EVENT_WRITABLE)
      EV_SET(&ke[num++], i, EVFILT_WRITE, EV_ADD, 0, 0, NULL);
    if (num) {
      if (kevent(kqfd, ke, num, NULL, 0, NULL) == -1) {
        ldout(cct,0) << __func__ << " unable to add event: "
                     << cpp_strerror(errno) << dendl;
        return -errno;
      }
    }
  }
  return 0;
}

int KqueueDriver::init(int nevent)
{
  // keep track of possible changes of our thread
  // because change of thread kills the kqfd
  int oldkqfd = kqfd;	
  mythread = pthread_self();

  // Reserve the space to accept the kevent return events.
  res_events = (struct kevent*)malloc(sizeof(struct kevent)*nevent);
  if (!res_events) {
    lderr(cct) << __func__ << " unable to malloc memory: "
                           << cpp_strerror(errno) << dendl;
    return -ENOMEM;
  }
  memset(res_events, 0, sizeof(struct kevent)*nevent);

  // Reserve the space to keep all of the events set, so it can be redone
  // when we change trhread ID. 
  sav_events = (struct SaveEvent*)malloc(sizeof(struct SaveEvent)*nevent);
  if (!sav_events) {
    lderr(cct) << __func__ << " unable to malloc memory: "
                           << cpp_strerror(errno) << dendl;
    return -ENOMEM;
  }
  memset(sav_events, 0, sizeof(struct SaveEvent)*nevent);

  // Delay assigning a descriptor until it is really needed.
  // kqfd = kqueue();
  kqfd = -1;
  
  size = nevent;

  return 0;
}

int KqueueDriver::add_event(int fd, int cur_mask, int add_mask)
{
  struct kevent ke[2];
  int events = 0;
  int filter = 0;
  int num = 0;
  int oldkqfd = kqfd;

  if (!pthread_equal(mythread, pthread_self())) {
    ldout(cct,10) << __func__ << " We changed thread from " << mythread 
		  << " to " << pthread_self() << dendl;
    mythread = pthread_self();
    kqfd = -1;
  } else if ((kqfd != -1) && (test_kqfd() < 0)) {
    ldout(cct,10) << __func__ << " Recreating old kqfd."  << dendl;
    kqfd = -1;
  }
  if (kqfd == -1) {
    kqfd = kqueue();
    ldout(cct,10) << __func__ << " kqueue: new kqfd = " << kqfd 
	          << " (was: " << oldkqfd << ")" 
		  << dendl;
    if (kqfd < 0) {
      lderr(cct) << __func__ << " unable to do kqueue: "
                             << cpp_strerror(errno) << dendl;
      return -errno;
    }
    if (restore_events()< 0) {
      lderr(cct) << __func__ << " unable restore all events "
                             << cpp_strerror(errno) << dendl;
      return -errno;
    }
  }

  ldout(cct,20) << __func__ << " add event kqfd = " << kqfd << " fd = " << fd 
	<< " cur_mask = " << cur_mask << " add_mask = " << add_mask 
	<< dendl;

  if (add_mask & EVENT_READABLE)
    EV_SET(&ke[num++], fd, EVFILT_READ, EV_ADD|EV_CLEAR, 0, 0, NULL);
  if (add_mask & EVENT_WRITABLE)
    EV_SET(&ke[num++], fd, EVFILT_WRITE, EV_ADD|EV_CLEAR, 0, 0, NULL);

  if (num) {
    if (kevent(kqfd, ke, num, NULL, 0, NULL) == -1) {
      lderr(cct) << __func__ << " unable to add event: "
                             << cpp_strerror(errno) << dendl;
      return -errno;
    }
  }
  // keep what we set
  sav_events[fd].mask = cur_mask | add_mask;
  return 0;
}

int KqueueDriver::del_event(int fd, int cur_mask, int del_mask)
{
  struct kevent ke[2];
  int num = 0;
  int mask = cur_mask & del_mask;

  ldout(cct,20) << __func__ << " delete event kqfd = " << kqfd 
	<< " fd = " << fd << " cur_mask = " << cur_mask 
	<< " del_mask = " << del_mask << dendl;

  if (!pthread_equal(mythread, pthread_self())) {
    ldout(cct,10) << __func__ << " We changed thread from " << mythread 
		  << " to " << pthread_self() << dendl;
    mythread = pthread_self();
  } else if ((kqfd != -1) && (test_kqfd() < 0)) {
    ldout(cct,10) << __func__ << " Recreating old kqfd."  << dendl;
    kqfd = -1;
  } 
  if (kqfd == -1) {
    kqfd = kqueue();
    ldout(cct,10) << __func__ << " kqueue: new kqfd = " << kqfd << dendl;
    if (kqfd < 0) {
      lderr(cct) << __func__ << " unable to do kqueue: "
                             << cpp_strerror(errno) << dendl;
      return -errno;
    }
    if (restore_events()< 0) {
      lderr(cct) << __func__ << " unable restore all events "
                             << cpp_strerror(errno) << dendl;
      return -errno;
    }
  }

  if (mask & EVENT_READABLE)
    EV_SET(&ke[num++], fd, EVFILT_READ, EV_DELETE, 0, 0, NULL);
  if (mask & EVENT_WRITABLE)
    EV_SET(&ke[num++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, NULL);

  if (num) {
    int r = 0;
    if ((r = kevent(kqfd, ke, num, NULL, 0, NULL)) < 0) {
      lderr(cct) << __func__ << " kevent: delete fd=" << fd << " mask=" << mask
                 << " failed." << cpp_strerror(errno) << dendl;
      return -errno;
    }
  }
  // keep the administration
  sav_events[fd].mask = cur_mask & ~del_mask;
  return 0;
}

int KqueueDriver::resize_events(int newsize)
{
  return 0;
}

int KqueueDriver::event_wait(vector<FiredFileEvent> &fired_events, struct timeval *tvp)
{
  int retval, numevents = 0;
  struct timespec timeout;

  ldout(cct,10) << __func__ << " kqfd = " << kqfd << dendl;
  if (!pthread_equal(mythread, pthread_self())) {
    ldout(cct,10) << __func__ << " We changed thread from " << mythread 
		  << " to " << pthread_self() << dendl;
    mythread = pthread_self();
    kqfd = -1;
  } else if ((kqfd != -1) && (test_kqfd() < 0)) {
    ldout(cct,10) << __func__ << " Recreating old kqfd."  << dendl;
    kqfd = -1;
  } 
  if (kqfd == -1) {
    kqfd = kqueue();
    ldout(cct,10) << __func__ << " kqueue: new kqfd = " << kqfd << dendl;
    if (kqfd < 0) {
      lderr(cct) << __func__ << " unable to do kqueue: "
                             << cpp_strerror(errno) << dendl;
      return -errno;
    }
    if (restore_events()< 0) {
      lderr(cct) << __func__ << " unable restore all events "
                             << cpp_strerror(errno) << dendl;
      return -errno;
    }
  }

  if (tvp != NULL) {
      timeout.tv_sec = tvp->tv_sec;
      timeout.tv_nsec = tvp->tv_usec * 1000;
      ldout(cct,20) << __func__ << " "
		<< timeout.tv_sec << " sec "
		<< timeout.tv_nsec << " nsec"
		<< dendl;
      retval = kevent(kqfd, NULL, 0, res_events, size, &timeout);
  } else {
      ldout(cct,20) << __func__ << " event_wait: "
		<< " NULL"
		<< dendl;
      retval = kevent(kqfd, NULL, 0, res_events, size, NULL);
  }

  ldout(cct,25) << __func__ << " kevent retval: "
		<< retval
		<< dendl;
  if (retval < 0) {
    lderr(cct) << __func__ << " kqueue error: "
                           << cpp_strerror(errno) << dendl;
    return -errno;
  } else if (retval == 0) {
    ldout(cct,5) << __func__ << " Hit timeout("
                 << timeout.tv_sec << " sec "
                 << timeout.tv_nsec << " nsec"
		 << ")." << dendl;
  } else {
    int j;

    numevents = retval;
    fired_events.resize(numevents);
    for (j = 0; j < numevents; j++) {
      int mask = 0;
      struct kevent *e = res_events + j;
      ldout(cct,20) << __func__ << " j: " << j << dendl;

      if (e->filter == EVFILT_READ) mask |= EVENT_READABLE;
      if (e->filter == EVFILT_WRITE) mask |= EVENT_WRITABLE;
      if (e->flags & EV_ERROR) mask |= EVENT_READABLE|EVENT_WRITABLE;
      fired_events[j].fd = (int)e->ident;
      fired_events[j].mask = mask;

    }
  }
  return numevents;
}
