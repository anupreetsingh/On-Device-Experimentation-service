#pragma once

#include <sys/types.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "assigner.h"
#include "event_loop.h"

namespace expd {

// Serves clients on the event-loop thread. Each request is one frame (frame.h) holding a
// text command:
//
//   GET <experiment>   ->  OK <variant>  |  NONE
//   STATS              ->  OK key=value ...
class Server {
 public:
  Server(EventLoop& loop, Assigner& assigner);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Accepts clients on a listening socket. The server closes it when destroyed.
  void listen_on(int listen_fd);

 private:
  struct Client {
    std::string in;   // bytes read that don't make a whole frame yet
    std::string out;  // replies not written yet
    uid_t uid = uid_t(-1);
    bool watching_write = false;
  };

  void accept_from(int listen_fd);
  void read_from(int fd);
  void write_to(int fd);
  void handle(Client& client, std::string_view request);
  void disconnect(int fd);

  EventLoop& loop_;
  Assigner& assigner_;
  std::unordered_map<int, Client> clients_;
  std::vector<int> listeners_;
  uint64_t requests_ = 0;
};

}  // namespace expd
