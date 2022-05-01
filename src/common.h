#ifndef SRC_COMMON_H
#define SRC_COMMON_H

#define QUIT_ON_NULL(msg, param) if (param == NULL) { perror(msg); exit(EXIT_FAILURE); }
#define QUIT_ON_MINUSONE(msg, param) if (param == -1) { perror(msg); exit(EXIT_FAILURE); }

char is_legal_nick(char *nick);
#endif //SRC_COMMON_H
