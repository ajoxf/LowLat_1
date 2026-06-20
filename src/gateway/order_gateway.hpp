// NNF order gateway: a persistent TCP connection to the NSE NNF gateway server.
//
// TCP_NODELAY is set so every order is its own segment (Nagle disabled), the
// send buffer is kept small for immediate egress, and a separate spin-read path
// drains acks/fills/rejects. When OpenOnload is preloaded these same calls run
// on its kernel-bypass stack.
#ifndef HFT_GATEWAY_ORDER_GATEWAY_HPP_
#define HFT_GATEWAY_ORDER_GATEWAY_HPP_

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common/compiler.hpp"
#include "common/time_utils.hpp"

namespace hft {

class TcpSocket {
 public:
  TcpSocket() = default;
  ~TcpSocket() { close_socket(); }
  TcpSocket(const TcpSocket&) = delete;
  TcpSocket& operator=(const TcpSocket&) = delete;

  bool connect_to(const char* ip, uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
      return false;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr(ip);
    addr.sin_port = htons(port);
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close_socket();
      return false;
    }
    apply_lowlatency_opts();
    return true;
  }

  // Adopt an already-connected fd (used by tests with socketpair/accept).
  void adopt(int fd) {
    close_socket();
    fd_ = fd;
    apply_lowlatency_opts();
  }

  void apply_lowlatency_opts() {
    if (fd_ < 0) {
      return;
    }
    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // Disable Nagle.
    int sndbuf = 64 * 1024;
    ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
#ifdef SO_BUSY_POLL
    int busy_us = 50;
    ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
#endif
  }

  void set_nonblocking(bool on) {
    if (fd_ < 0) {
      return;
    }
    int flags = ::fcntl(fd_, F_GETFL, 0);
    ::fcntl(fd_, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
  }

  // Send the whole buffer (one logical NNF message). Returns true on success.
  bool send_all(const uint8_t* buf, size_t len) {
    size_t off = 0;
    while (off < len) {
      ssize_t n = ::send(fd_, buf + off, len - off, MSG_NOSIGNAL);
      if (n <= 0) {
        return false;
      }
      off += static_cast<size_t>(n);
    }
    return true;
  }

  ssize_t recv(uint8_t* buf, size_t cap) { return ::recv(fd_, buf, cap, 0); }

  bool valid() const { return fd_ >= 0; }
  int fd() const { return fd_; }
  void close_socket() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

class OrderGateway {
 public:
  bool connect(const char* ip, uint16_t port) {
    if (!sock_.connect_to(ip, port)) {
      return false;
    }
    connected_.store(true, std::memory_order_release);
    return true;
  }

  void adopt(int fd) {
    sock_.adopt(fd);
    connected_.store(true, std::memory_order_release);
  }

  // Send one encoded NNF message. Records an RDTSC send timestamp and counts.
  bool send_order(const uint8_t* nnf_msg, size_t len) {
    last_send_tsc_ = rdtsc();
    if (!sock_.send_all(nnf_msg, len)) {
      connected_.store(false, std::memory_order_release);
      return false;
    }
    ++sent_;
    return true;
  }

  // Spin-read responses into buf. Returns bytes read (0 if none / -1 on close).
  ssize_t poll_response(uint8_t* buf, size_t cap) {
    ssize_t n = sock_.recv(buf, cap);
    if (n > 0) {
      ++received_;
    } else if (n == 0) {
      connected_.store(false, std::memory_order_release);
    }
    return n;
  }

  bool connected() const { return connected_.load(std::memory_order_acquire); }
  uint64_t sent() const { return sent_; }
  uint64_t received() const { return received_; }
  uint64_t last_send_tsc() const { return last_send_tsc_; }
  TcpSocket& socket() { return sock_; }

 private:
  TcpSocket sock_;
  std::atomic<bool> connected_{false};
  uint64_t sent_ = 0;
  uint64_t received_ = 0;
  uint64_t last_send_tsc_ = 0;
};

}  // namespace hft

#endif  // HFT_GATEWAY_ORDER_GATEWAY_HPP_
