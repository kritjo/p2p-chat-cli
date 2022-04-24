#ifndef SRC_REAL_TIME_H
#define SRC_REAL_TIME_H

#include <bits/types/timer_t.h>
#include <bits/types/time_t.h>

enum USR1_TYPE {
    REG,
    HEARTBEAT,
};

typedef struct {
    enum USR1_TYPE type;
    timer_t timer;
} usr1_sigval_t;

usr1_sigval_t *register_usr1_custom_sig(enum USR1_TYPE type, usr1_sigval_t *info, time_t timeout, time_t interval);
void unregister_usr_1_custom_sig(usr1_sigval_t *info);

#endif //SRC_REAL_TIME_H
