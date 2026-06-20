// Thin UDP socket wrapper for NSE MTBT multicast reception.
//
// Supports joining a multicast group on a specific colocation NIC interface
// (NSE requires a specific local interface, not INADDR_ANY), batched receive
// via recvmmsg(), and SO_BUSY_POLL for low-latency busy polling. When
// OpenOnload is preloaded its user-space stack transparently accelerates these
// same socket calls; onload_active() reports whether that is the case.
#ifndef HFT_GATEWAY_UDP_SOCKET_HPP_
#define HFT_GATEWAY_UDP_SOCKET_HPP_

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace hft {

// True when OpenOnload's accelerated stack is active for this process.
inline bool onload_active() {
  return std::getenv("EF_NAME") != nullptr || std::getenv("ONLOAD_KERNEL") != nullptr ||
         std::getenv("LD_PRELOAD") != nullptr;
}

class UdpSocket {
 public:
  UdpSocket() = default;
  ~UdpSocket() { close_socket(); }

  UdpSocket(const UdpSocket&) = delete;
  UdpSocket& operator=(const UdpSocket&) = delete;
  UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }

  bool valid() const { return fd_ >= 0; }
  int fd() const { return fd_; }

  // Create and bind a UDP socket on `port` (INADDR_ANY). Used for multicast
  // reception and for loopback unicast testing.
  bool bind_rx(uint16_t port) {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd_ < 0) {
      return false;
    }
    int one = 1;
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    ::setsockopt(fd_, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
      close_socket();
      return false;
    }
    return true;
  }

  // Join a multicast group, receiving on the given local interface IP (the colo
  // NIC). Best-effort SO_BUSY_POLL and large receive buffer are also applied.
  bool join_multicast(const char* group_ip, const char* iface_ip) {
    if (fd_ < 0) {
      return false;
    }
    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = ::inet_addr(group_ip);
    mreq.imr_interface.s_addr =
        iface_ip != nullptr ? ::inet_addr(iface_ip) : htonl(INADDR_ANY);
    if (::setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) != 0) {
      return false;
    }
#ifdef SO_BUSY_POLL
    int busy_us = 50;
    ::setsockopt(fd_, SOL_SOCKET, SO_BUSY_POLL, &busy_us, sizeof(busy_us));
#endif
    int rcvbuf = 16 * 1024 * 1024;
    ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    return true;
  }

  // The actual local port the socket is bound to (useful when binding port 0).
  uint16_t local_port() const {
    sockaddr_in addr{};
    socklen_t len = sizeof(addr);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
      return 0;
    }
    return ntohs(addr.sin_port);
  }

  void set_nonblocking(bool on) {
    if (fd_ < 0) {
      return;
    }
    int flags = ::fcntl(fd_, F_GETFL, 0);
    if (on) {
      ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    } else {
      ::fcntl(fd_, F_SETFL, flags & ~O_NONBLOCK);
    }
  }

  // Single datagram receive. Returns bytes received, 0/-1 on would-block/error.
  ssize_t recv(uint8_t* buf, size_t cap) {
    return ::recv(fd_, buf, cap, 0);
  }

  // Batched receive (recvmmsg). `bufs` is `count` buffers each of `cap` bytes;
  // `out_lens` receives per-message lengths. Returns the number of messages.
  int recv_batch(uint8_t* bufs, size_t cap, size_t count, uint32_t* out_lens) {
    static constexpr size_t kMax = 32;
    if (count > kMax) {
      count = kMax;
    }
    mmsghdr msgs[kMax];
    iovec iovs[kMax];
    std::memset(msgs, 0, sizeof(msgs));
    for (size_t i = 0; i < count; ++i) {
      iovs[i].iov_base = bufs + i * cap;
      iovs[i].iov_len = cap;
      msgs[i].msg_hdr.msg_iov = &iovs[i];
      msgs[i].msg_hdr.msg_iovlen = 1;
    }
    int n = ::recvmmsg(fd_, msgs, static_cast<unsigned>(count), 0, nullptr);
    if (n <= 0) {
      return n;
    }
    for (int i = 0; i < n; ++i) {
      out_lens[i] = msgs[i].msg_len;
    }
    return n;
  }

  // Unicast send (for loopback tests / tooling).
  bool send_to(const char* ip, uint16_t port, const uint8_t* buf, size_t len) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = ::inet_addr(ip);
    addr.sin_port = htons(port);
    return ::sendto(fd_, buf, len, 0, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ==
           static_cast<ssize_t>(len);
  }

  bool open_tx() {
    fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    return fd_ >= 0;
  }

  void close_socket() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

 private:
  int fd_ = -1;
};

}  // namespace hft

#endif  // HFT_GATEWAY_UDP_SOCKET_HPP_
