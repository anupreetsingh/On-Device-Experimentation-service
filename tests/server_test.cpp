#include <gtest/gtest.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "frame.h"
#include "snapshot_writer.h"
#include "test_daemon.h"
#include "test_util.h"

namespace expd {
namespace {

// kTestDevice lands in bucket 1673 for new_search_ranking, so it gets control.
constexpr char kConfig[] =
    "new_search_ranking k3f9 control:5000 treatment:5000\n"
    "photo_grid a1 off:0 on:10000\n";

TEST(Frame, RoundTripAndPartials) {
  std::string bytes;
  frame::append(&bytes, "GET exp");
  frame::append(&bytes, "");
  std::string_view payload;
  size_t len = 0;

  EXPECT_EQ(frame::parse(std::string_view(bytes).substr(0, 3), &payload, &len), 0);
  EXPECT_EQ(frame::parse(std::string_view(bytes).substr(0, 8), &payload, &len), 0);
  ASSERT_EQ(frame::parse(bytes, &payload, &len), 1);
  EXPECT_EQ(payload, "GET exp");
  EXPECT_EQ(len, 11u);
  ASSERT_EQ(frame::parse(std::string_view(bytes).substr(len), &payload, &len), 1);
  EXPECT_EQ(payload, "");

  const char huge[4] = {0, 0x10, 0, 0};  // 1 MB
  EXPECT_EQ(frame::parse(std::string_view(huge, 4), &payload, &len), -1);
}

TEST(Server, AnswersGet) {
  TempDir dir;
  TestDaemon daemon(dir.path(), kConfig);
  ASSERT_TRUE(daemon.loaded());
  EXPECT_EQ(daemon.call("GET new_search_ranking"), "OK control");
  EXPECT_EQ(daemon.call("GET photo_grid"), "OK on");
  EXPECT_EQ(daemon.call("GET missing"), "NONE");
  EXPECT_EQ(daemon.call("BOGUS"), "ERR unknown command");
}

TEST(Server, PublishesASnapshotOnLoad) {
  TempDir dir;
  TestDaemon daemon(dir.path(), kConfig);
  EXPECT_EQ(current_generation(dir.path()), 1u);
}

TEST(Server, ServesManyRequestsOnOneConnection) {
  TempDir dir;
  TestDaemon daemon(dir.path(), kConfig);
  int fd = uds::connect_to(daemon.socket_path());
  ASSERT_GE(fd, 0);
  for (int i = 0; i < 1000; ++i) {
    std::string reply;
    ASSERT_TRUE(uds::request(fd, "GET new_search_ranking", &reply));
    ASSERT_EQ(reply, "OK control");
  }
  close(fd);
  EXPECT_NE(daemon.call("STATS").find("requests=1001"), std::string::npos);
}

TEST(Server, HalfAFrameDoesNotBlockOtherClients) {
  TempDir dir;
  TestDaemon daemon(dir.path(), kConfig);
  int slow = uds::connect_to(daemon.socket_path());
  ASSERT_GE(slow, 0);
  std::string bytes;
  frame::append(&bytes, "GET photo_grid");
  ASSERT_EQ(send(slow, bytes.data(), 6, 0), 6);  // the length and two bytes of payload

  EXPECT_EQ(daemon.call("GET new_search_ranking"), "OK control");

  ASSERT_EQ(send(slow, bytes.data() + 6, bytes.size() - 6, 0), ssize_t(bytes.size() - 6));
  std::string reply;
  ASSERT_TRUE(uds::read_frame(slow, &reply));
  EXPECT_EQ(reply, "OK on");
  close(slow);
}

TEST(Server, DisconnectsAnOversizedFrame) {
  TempDir dir;
  TestDaemon daemon(dir.path(), kConfig);
  int fd = uds::connect_to(daemon.socket_path());
  ASSERT_GE(fd, 0);
  const char huge[4] = {0, 0x10, 0, 0};
  ASSERT_EQ(send(fd, huge, 4, 0), 4);
  char c;
  EXPECT_EQ(recv(fd, &c, 1, 0), 0);  // expd closed the connection
  close(fd);
  EXPECT_EQ(daemon.call("GET photo_grid"), "OK on");
}

TEST(Server, ManyClientsAtOnce) {
  TempDir dir;
  TestDaemon daemon(dir.path(), kConfig);
  std::vector<int> fds;
  for (int i = 0; i < 50; ++i) {
    int fd = uds::connect_to(daemon.socket_path());
    ASSERT_GE(fd, 0);
    ASSERT_TRUE(uds::send_frame(fd, "GET new_search_ranking"));
    fds.push_back(fd);
  }
  for (int fd : fds) {
    std::string reply;
    ASSERT_TRUE(uds::read_frame(fd, &reply));
    EXPECT_EQ(reply, "OK control");
    close(fd);
  }
}

TEST(Server, SecondListenerOnTheSamePathFails) {
  TempDir dir;
  TestDaemon daemon(dir.path(), kConfig);
  std::string error;
  EXPECT_EQ(listen_unix(daemon.socket_path(), &error), -1);
  EXPECT_NE(error.find("already listening"), std::string::npos) << error;
  EXPECT_EQ(daemon.call("GET photo_grid"), "OK on");
}

}  // namespace
}  // namespace expd
