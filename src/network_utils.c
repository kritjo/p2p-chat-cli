#include "network_utils.h"

char *get_addr(struct sockaddr_storage addr, char *buf, size_t buflen) {
  if (addr.ss_family == AF_INET) {
    inet_ntop(addr.ss_family, &((struct sockaddr_in*) &addr)->sin_addr, buf, buflen);
  } else {
    inet_ntop(addr.ss_family, &((struct sockaddr_in6*) &addr)->sin6_addr, buf, buflen);
  }
  return buf;
}

socklen_t get_addr_len(struct sockaddr_storage addr) {
  if (addr.ss_family == AF_INET) {
    return INET_ADDRSTRLEN;
  } else {
    return INET6_ADDRSTRLEN;
  }
}

char *get_port(struct sockaddr_storage addr, char *buf) {
  unsigned short port;
  if (addr.ss_family == AF_INET) {
    port = ntohs(((struct sockaddr_in*) &addr)->sin_port);
  } else {
    port = ntohs(((struct sockaddr_in6*) &addr)->sin6_port);
  }
  sprintf(buf, "%hu", port);
  return buf;
}
