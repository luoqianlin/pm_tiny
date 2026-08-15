#include "core/session.h"
#include "core/frame_stream.hpp"
#include "core/pm_sys.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

[[noreturn]] void fail(const char *message) {
    std::fprintf(stderr, "session_test failure: %s\n", message);
    std::abort();
}

void expect(bool condition, const char *message) {
    if (!condition) {
        fail(message);
    }
}

std::array<int, 2> make_socket_pair() {
    std::array<int, 2> sv{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sv.data()) != 0) {
        fail("socketpair failed");
    }
    return sv;
}

void test_write_frame_encodes_v2() {
    auto sv = make_socket_pair();
    pm_tiny::session_t session(sv[0], 0);
    auto frame = std::make_unique<pm_tiny::frame_t>();
    frame->push_back('A');
    frame->push_back('B');
    int written = session.write_frame(frame, 1);
    expect(written > 0, "write_frame should write bytes");

    std::vector<uint8_t> encoded(32);
    auto read_bytes = pm_tiny::safe_read(sv[1], encoded.data(), encoded.size());
    expect(read_bytes > 0, "peer should receive data");
    encoded.resize(static_cast<size_t>(read_bytes));
    pm_tiny::protocol_decoder decoder;
    decoder.feed(encoded.data(), encoded.size());
    auto decoded = decoder.pop();
    expect(decoded.flags == pm_tiny::protocol_flag_response, "response flag missing");
    expect(decoded.payload.size() == 2, "decoded content size mismatch");
    expect(decoded.payload[0] == 'A', "decoded content mismatch (A)");
    expect(decoded.payload[1] == 'B', "decoded content mismatch (B)");
    ::close(sv[1]);
}

void test_read_frame_unescapes_payload() {
    auto sv = make_socket_pair();
    pm_tiny::session_t session(sv[0], 0);
    pm_tiny::protocol_message message;
    message.type = 0x23;
    message.request_id = 7;
    message.payload = {'X', '\n', '\\'};
    auto encoded = pm_tiny::protocol_encode(message);
    pm_tiny::safe_write(sv[1], encoded.data(), encoded.size());

    auto frame = session.read_message(1);
    expect(frame.type == 0x23, "message type mismatch");
    expect(frame.request_id == 7, "request id mismatch");
    expect(frame.payload.size() == 3, "payload length mismatch");
    expect(frame.payload[1] == '\n' && frame.payload[2] == '\\', "payload content mismatch");
    ::close(sv[1]);
}

void test_peer_shutdown_marks_session_closed() {
    auto sv = make_socket_pair();
    pm_tiny::session_t session(sv[0], 0);
    ::close(sv[1]);
    session.read();
    expect(session.is_close(), "session should close when peer disconnects");
}

void test_nonblocking_empty_read_keeps_session_open() {
    auto sv = make_socket_pair();
    expect(pm_tiny::set_nonblock(sv[0]) == 0, "set_nonblock failed");
    pm_tiny::session_t session(sv[0], 0);
    expect(session.read() == 0, "empty nonblocking read should report no data");
    expect(!session.is_close(), "empty nonblocking read should keep session open");
    ::close(sv[1]);
    session.read();
    expect(session.is_close(), "nonblocking session should close after peer disconnects");
}

void test_destructor_closes_owned_fd() {
    auto sv = make_socket_pair();
    const int owned_fd = sv[0];
    {
        pm_tiny::session_t session(owned_fd, 0);
    }
    errno = 0;
    expect(::fcntl(owned_fd, F_GETFD) == -1 && errno == EBADF,
           "session destructor should close its owned fd");
    ::close(sv[1]);
}

} // namespace

int main() {
    test_write_frame_encodes_v2();
    test_read_frame_unescapes_payload();
    test_peer_shutdown_marks_session_closed();
    test_nonblocking_empty_read_keeps_session_open();
    test_destructor_closes_owned_fd();
    return 0;
}
