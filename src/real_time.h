#ifndef SRC_REAL_TIME_H
#define SRC_REAL_TIME_H

#include <bits/types/timer_t.h>
#include <bits/types/time_t.h>

typedef struct usr1_sigval{
    char *id;
    timer_t timer;
    char do_not_honour;
    struct send_node *timed_out_send_node;
} usr1_sigval_t;

int register_usr1_custom_sig(usr1_sigval_t *info);
int set_time_usr1_timer(usr1_sigval_t *info, time_t timeout);
void unregister_usr_1_custom_sig(usr1_sigval_t *info);

#endif //SRC_REAL_TIME_H
