#include <bits/types/sigevent_t.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <signal.h>
#include "real_time.h"

int register_usr1_custom_sig(usr1_sigval_t *info) {
  sigevent_t event;
  memset(&event, 0, sizeof(event));
  event.sigev_notify = SIGEV_SIGNAL;
  event.sigev_signo = SIGUSR1;
  event.sigev_value.sival_ptr = info;

  if (timer_create(CLOCK_REALTIME, &event, &info->timer) == -1) {
    perror("timer_create");
    return -1;
  }
  return 1;

}

int set_time_usr1_timer(usr1_sigval_t *info, time_t timeout) {
  struct itimerspec timespec;
  memset(&timespec, 0, sizeof(timespec));
  timespec.it_value.tv_sec = timeout;
  if (timer_settime(info->timer, 0, &timespec, NULL) == -1) {
    perror("timer_settime()");
    return -1;
  }
  return 1;
}

void unregister_usr_1_custom_sig(usr1_sigval_t *info) {
  timer_delete(info->timer);
  free(info);
}