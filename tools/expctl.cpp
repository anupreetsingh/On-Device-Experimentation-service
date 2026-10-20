// expctl: talks to expd over its socket.
//
//   expctl get <experiment>   this device's variant
//   expctl stats              expd's counters
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

#include "locations.h"
#include "uds_client.h"

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: expctl get <experiment>\n"
               "       expctl stats\n");
  return 2;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return usage();
  std::string command = argv[1];
  std::string request;
  if (command == "get" && argc == 3) {
    request = std::string("GET ") + argv[2];
  } else if (command == "stats" && argc == 2) {
    request = "STATS";
  } else {
    return usage();
  }

  std::string path = locations::socket_path();
  int fd = uds::connect_to(path);
  if (fd < 0) {
    std::fprintf(stderr, "expctl: can't connect to %s: %s\n", path.c_str(), std::strerror(errno));
    return 1;
  }
  std::string reply;
  bool ok = uds::request(fd, request, &reply);
  close(fd);
  if (!ok) {
    std::fprintf(stderr, "expctl: no reply from expd\n");
    return 1;
  }
  std::printf("%s\n", reply.c_str());
  return reply.rfind("OK", 0) == 0 ? 0 : 1;
}
