#include <string.h>
#include <ctype.h>
char is_legal_nick(char *nick) {
  size_t nick_len = strlen(nick);
  if (20 < nick_len) return 0;
  else {
    for (size_t i = 0; i < nick_len; i++) {
      // Interpreting return as \r or \n
      if (!isascii(nick[i]) || isblank(nick[i]) || nick[i] == '\n' || nick[i] == '\r') {
        return 0;
      }
    }
  }
  return 1;
}