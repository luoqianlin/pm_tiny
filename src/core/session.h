
#ifndef PM_TINY_SESSION_H
#define PM_TINY_SESSION_H

#include <cstdint>
#include <vector>
#include <map>
#include <algorithm>
#include <ostream>
#include <sstream>
#include <iomanip>
#include <list>
#include <queue>
#include <unistd.h>
#include <memory>
#include <string>
#include <type_traits>
#include "frame_stream.hpp"
#include "protocol_v2.h"

namespace pm_tiny {

    struct prog_info_t;

    class session_t {

    public:

        session_t(int fd, int fd_type);

        session_t(const session_t &) = delete;

        session_t(session_t &&) = delete;

        session_t &operator=(const session_t &) = delete;

        int read();

        int write();

        void close();

        bool is_close() const;

        int get_fd() const;

        int get_fd_type() const;

        int sbuf_size() const;

        int rbuf_size() const;

        bool sbuf_empty() const;

        bool rbuf_empty() const;

        frame_ptr_t read_frame(int block = 0);

        protocol_message read_message(int block = 0);

        frame_ptr_t get_frame_from_buf();

        int write_frame(const pm_tiny::frame_ptr_t &f, int block = 0);

        int write_command_frame(const pm_tiny::frame_ptr_t &f, int block = 0,
                                std::uint32_t request_id = 0,
                                std::uint8_t flags = 0);

        int write_stream_frame(const pm_tiny::frame_ptr_t &f, int block = 0,
                               bool more = false);

        void mark_close();

        bool is_marked_close() const;

        int shutdown_read();

#if PM_TINY_SERVER

        void set_prog(prog_info_t *prog);

        prog_info_t *get_prog();

#endif
    private:
        int fd_ = -1;
        int fd_type_ = 0;
        int fsbuf_size_ = 0;
        int frbuf_size_ = 0;
        int fd_nonblock_ = 0;
        std::deque<frame_ptr_t> recv_buf = {};//last item as tmp buffer
        std::deque<protocol_message> recv_messages_ = {};
        std::deque<frame_ptr_t> send_buf = {};
        frame_t recv_frame_buf_;
        protocol_decoder decoder_;
        std::uint16_t current_message_type_ = 0;
        std::uint32_t current_request_id_ = 0;
        std::uint32_t next_request_id_ = 1;
        bool mark_closed_ = false;
#if PM_TINY_SERVER
        prog_info_t *prog_ = nullptr;

#endif
    };

    using session_ptr_t = std::shared_ptr<pm_tiny::session_t>;
}


#endif //PM_TINY_SESSION_H
