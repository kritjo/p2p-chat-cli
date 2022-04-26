#ifndef SRC_UPUSH_SERVER_H
#define SRC_UPUSH_SERVER_H

#define MAX_MSG 1460 // Longest msg can be 20 char + 2*nicklen + message
// Max message length is 1400.
// Assume that pkt num is 0 or 1.
// Nicklen is max 20 char

void handle_exit(void);

void handle_sig_terminate(int sig);

void print_illegal_dram(struct sockaddr_storage addr);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
void handle_sig_ignore(int sig) {}
#pragma GCC diagnostic pop

#endif //SRC_UPUSH_SERVER_H
