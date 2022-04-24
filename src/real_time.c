#include <bits/types/sigevent_t.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <signal.h>
#include <malloc.h>
#include "real_time.h"

usr1_sigval_t *register_usr1_custom_sig(enum USR1_TYPE type, usr1_sigval_t *info, time_t timeout, time_t interval) {
  sigevent_t event;
  memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGUSR1;
  event.sigev_value.sival_ptr = info;

  info->type = type;

  if (timer_create(CLOCK_REALTIME, &event, &info->timer) == -1) {
    perror("timer_create");
    return 0;
  }

  struct itimerspec timespec;
  memset(&timespec, 0, sizeof(timespec));
  timespec.it_value.tv_sec = timeout;
  timespec.it_interval.tv_sec = interval;
  if (timer_settime(info->timer, 0, &timespec, NULL) == -1) {
    perror("timer_settime()");
    return 0;
  }
  return info;
}

void unregister_usr_1_custom_sig(usr1_sigval_t *info) {
  timer_delete(info->timer);
  free(info);
}