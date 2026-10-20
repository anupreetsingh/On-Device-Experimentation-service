// expctl: talks to expd over its socket.
//
//   expctl get <experiment>   this device's variant
//   expctl push <file>        send a new config (- for stdin)
//   expctl expose <experiment> <variant>
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
               "       expctl push <config-file | ->\n"
               "       expctl expose <experiment> <variant>\n"
               "       expctl stats\n");
  return 2;
}

bool read_all(const char* path, std::string* out) {
  FILE* f = std::strcmp(path, "-") == 0 ? stdin : std::fopen(path, "r");
  if (f == nullptr) return false;
  char buf[4096];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out->append(buf, n);
  bool ok = !std::ferror(f);
  if (f != stdin) std::fclose(f);
  return ok;
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
  } else if (command == "push" && argc == 3) {
    std::string config;
    if (!read_all(argv[2], &config)) {
      std::fprintf(stderr, "expctl: can't read %s: %s\n", argv[2], std::strerror(errno));
      return 1;
    }
    request = "PUSH\n" + config;
  } else if (command == "expose" && argc == 4) {
    request = std::string("EXPOSE ") + argv[2] + " " + argv[3];
  } else {
    return usage();
  }

  std::string path = locations::socket_path();
  int fd = uds::connect_to(path);
  if (fd < 0) {
    std::fprintf(stderr, "expctl: can't connect to %s: %s\n", path.c_str(), std::strerror(errno));
    return 1;
  }
  if (command == "expose") {  // expd doesn't reply to exposures
    bool sent = uds::send_frame(fd, request);
    close(fd);
    return sent ? 0 : 1;
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
