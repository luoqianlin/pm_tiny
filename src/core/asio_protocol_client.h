#ifndef PM_TINY_ASIO_PROTOCOL_CLIENT_H
#define PM_TINY_ASIO_PROTOCOL_CLIENT_H

#include "protocol_v2.h"

#include <asio.hpp>

#include <cstdint>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace pm_tiny {

class asio_protocol_client {
public:
    asio_protocol_client(const std::string &address, bool abstract_namespace = false)
        : address_(address), abstract_namespace_(abstract_namespace) {
#if defined(_WIN32)
        for (int attempt = 0; attempt < 5; ++attempt) {
            handle_ = CreateFileA(address_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_PIPE_BUSY || !WaitNamedPipeA(address_.c_str(), 1000)) break;
        }
        if (handle_ == INVALID_HANDLE_VALUE) throw std::runtime_error("named pipe connect failed");
        DWORD mode = PIPE_READMODE_BYTE;
        if (!SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr)) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("named pipe mode setup failed");
        }
        stream_ = std::unique_ptr<asio::windows::stream_handle>(
                new asio::windows::stream_handle(io_, handle_));
#else
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
        sockaddr_un endpoint{};
        endpoint.sun_family = AF_UNIX;
        socklen_t length = 0;
        if (abstract_namespace) {
            endpoint.sun_path[0] = '\0';
            if (address_.size() >= sizeof(endpoint.sun_path)) {
                ::close(fd);
                throw std::runtime_error("abstract UDS name too long");
            }
            std::memcpy(endpoint.sun_path + 1, address_.data(), address_.size());
            length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + address_.size() + 1);
        } else {
            if (address_.size() >= sizeof(endpoint.sun_path)) {
                ::close(fd);
                throw std::runtime_error("UDS path too long");
            }
            std::strncpy(endpoint.sun_path, address_.c_str(), sizeof(endpoint.sun_path) - 1);
            length = sizeof(endpoint);
        }
        if (::connect(fd, reinterpret_cast<sockaddr *>(&endpoint), length) < 0) {
            const auto message = std::string("connect: ") + std::strerror(errno);
            ::close(fd);
            throw std::runtime_error(message);
        }
        stream_ = std::unique_ptr<asio::posix::stream_descriptor>(
                new asio::posix::stream_descriptor(io_, fd));
#endif
    }

    asio_protocol_client(const asio_protocol_client &) = delete;
    asio_protocol_client &operator=(const asio_protocol_client &) = delete;

    ~asio_protocol_client() {
        if (stream_) {
            asio::error_code ignored;
            stream_->close(ignored);
        }
    }

    protocol_message request(const protocol_message &message) {
        send(message);
        asio::error_code error;
        protocol_decoder decoder;
        std::uint8_t buffer[4096];
        while (decoder.empty()) {
            const auto count = stream_->read_some(asio::buffer(buffer), error);
            if (count != 0) decoder.feed(buffer, count);
            if (error) throw std::runtime_error(std::string("read: ") + error.message());
        }
        return decoder.pop();
    }

    void send(const protocol_message &message) {
        const auto wire = protocol_encode(message);
        asio::error_code error;
        asio::write(*stream_, asio::buffer(wire), error);
        if (error) throw std::runtime_error(std::string("write: ") + error.message());
    }

private:
    asio::io_context io_;
    std::string address_;
    bool abstract_namespace_ = false;
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::unique_ptr<asio::windows::stream_handle> stream_;
#else
    std::unique_ptr<asio::posix::stream_descriptor> stream_;
#endif
};

} // namespace pm_tiny

#endif
