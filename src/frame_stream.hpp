//
// Created by sansi on 2021/10/9.
//

#ifndef PM_TINY_FRAME_STREAM_HPP
#define PM_TINY_FRAME_STREAM_HPP

#include <utility>
#include <vector>
#include <map>
#include <memory.h>
#include <stdexcept>
#include <string>
#include <memory>

namespace pm_tiny {

    constexpr uint8_t frame_delimiter = '\n';
    constexpr size_t frame_max_length = 4 * (1 << 20);//4M
    using byte_t = uint8_t;
    using frame_t = std::vector<byte_t>;


    template<typename Iterator>
    using is_frame_iterator_t = std::is_same<typename
    std::decay_t<typename std::iterator_traits<Iterator>::value_type>, byte_t>;

    std::ostream &operator<<(std::ostream &os, frame_t const &f);

    template<typename IIter, typename OIter>
    inline std::enable_if_t<is_frame_iterator_t<IIter>::value, OIter>
    frame_escape(IIter first, IIter last, OIter result,
                 const std::map<byte_t, std::array<byte_t, 2>> &escapes) {
        for (; first != last; ++first) {
            auto iter = escapes.find(*first);
            if (iter != escapes.end()) {
                result = std::copy(iter->second.begin(), iter->second.end(), result);
            } else {
                *(result++) = *first;
            }
        }
        return result;

    }

    template<typename IIter, typename OIter>
    inline std::enable_if_t<is_frame_iterator_t<IIter>::value, OIter>
    frame_unescape(IIter first, IIter last, OIter result,
                   const std::map<byte_t, std::map<byte_t, byte_t>> &unescapes) {
        for (; first != last; ++first) {
            auto mp_iter = unescapes.find(*first);
            if (mp_iter != unescapes.end()) {
                auto iter = mp_iter->second.find(*(first + 1));
                if (iter != mp_iter->second.end()) {
                    *result++ = iter->second;
                }
                first += 1;
            } else {
                *result++ = *first;
            }
        }
        return result;
    }

    template<typename IIter, typename OIter>
    inline std::enable_if_t<is_frame_iterator_t<IIter>::value, OIter>
    frame_escape(IIter first, IIter last, OIter result) {
        return frame_escape(first, last, result, {{'\n', {'\\', 'n'}},
                                                  {'\\', {'\\', '\\'}}});

    }

    template<typename IIter, typename OIter>
    inline std::enable_if_t<is_frame_iterator_t<IIter>::value, OIter>
    frame_unescape(IIter first, IIter last, OIter result) {
        const std::map<byte_t, std::map<byte_t, byte_t>>
                unescapes{{'\\', {{'\\', '\\'}, {'n', '\n'}}}};
        return frame_unescape(first, last, result, unescapes);
    }


    template<typename T>
    inline std::enable_if_t<std::is_arithmetic<T>::value, void>
    fappend_value(frame_t &f, const T &n) {
        auto *addr = (uint8_t *) &n;
        std::copy(addr, addr + sizeof(T), std::back_inserter(f));
    }


    inline void fappend_value(frame_t &f, const std::string &str) {
        fappend_value<int>(f, (int) str.length());
        std::copy(str.begin(), str.end(), std::back_inserter(f));
    }

    using frame_ptr_t = std::unique_ptr<frame_t>;

    inline std::vector<frame_ptr_t> str_to_frames(int msg_type, const std::string &msg_content) {
        std::vector<frame_ptr_t> frames;
        auto frame_size = frame_max_length >> 1;
        if (msg_content.size() <= frame_size) {
            auto wf = std::make_unique<pm_tiny::frame_t>();
            pm_tiny::fappend_value<int>(*wf, msg_type);
            pm_tiny::fappend_value(*wf, msg_content);
            frames.push_back(std::move(wf));
        } else {
            std::vector<std::string> contents;
            auto n_frame = msg_content.size() / frame_size;
            for (size_t i = 0; i < n_frame; i++) {
                contents.push_back(msg_content.substr(frame_size * i, frame_size));
            }
            if ((msg_content.size() % frame_size) != 0) {
                contents.push_back(msg_content.substr(n_frame * frame_size));
            }
            for (size_t i = 0; i < contents.size(); i++) {
                auto wf = std::make_unique<pm_tiny::frame_t>();
                if (i < contents.size() - 1) {
                    pm_tiny::fappend_value<int>(*wf, 2);
                } else {
                    pm_tiny::fappend_value<int>(*wf, msg_type);
                }
                pm_tiny::fappend_value(*wf, contents[i]);
                frames.push_back(std::move(wf));
            }
        }
        return frames;
    }

    template<typename B1, typename B2>
    struct my_or : public std::conditional<B1::value, B1, B2>::type {
    };

    class oframe_stream {
    public:

        explicit oframe_stream(std::unique_ptr<frame_t> frame) : frame_(std::move(frame)) {
        }

        oframe_stream(const oframe_stream &) = delete;

        oframe_stream(oframe_stream &&) = delete;

        template<typename T>
        inline std::enable_if_t<std::is_arithmetic<T>::value, oframe_stream &>
        operator<<(const T &v) {
            fappend_value(*this->frame_, v);
            return *this;
        }

        oframe_stream &operator<<(const std::string &v) {
            fappend_value(*this->frame_, v);
            return *this;
        }

        frame_ptr_t &get_frame() {
            return this->frame_;
        }

    private:
        frame_ptr_t frame_;
    };

    class BufferInsufficientException : public std::runtime_error {
    public:
        BufferInsufficientException()
                : std::runtime_error("Insufficient buffer data to complete the read operation.") {}

        explicit BufferInsufficientException(const std::string &message)
                : std::runtime_error(message) {}
    };

    class iframe_stream {
    public:
        explicit iframe_stream(frame_t frame) : frame_(std::move(frame)) {
            this->addr_ = frame_.data();
        }

        iframe_stream(const iframe_stream &) = delete;

        iframe_stream(iframe_stream &&) = delete;

        template<typename T, typename R=void>
        using TypeConstraint = std::enable_if_t<std::is_arithmetic<T>::value, R>;

        void ensure_buffer_capacity(size_t type_size) {
            auto end = this->frame_.data() + this->frame_.size();
            if (static_cast<size_t>(end - addr_) < type_size) {
                throw BufferInsufficientException();
            }
        }

        template<typename T>
        inline TypeConstraint<T, iframe_stream &>
        fget_value(T &n) {
            ensure_buffer_capacity(sizeof(T));
            memcpy(&n, addr_, sizeof(T));
            addr_ += sizeof(T);
            return *this;
        }

        class StreamPositionGuard {
        public:
            StreamPositionGuard(const byte_t *&addr, size_t type_size)
                    : addr_(addr), type_size_(type_size), release_(false) {}

            StreamPositionGuard(const StreamPositionGuard &) = delete;

            StreamPositionGuard(StreamPositionGuard &&) = delete;

            StreamPositionGuard &operator=(const StreamPositionGuard &) = delete;

            ~StreamPositionGuard() {
                if (addr_ && !release_) {
                    addr_ -= type_size_;
                }
            }

            void release() {
                release_ = true;
            }

        private:
            const byte_t *&addr_;
            size_t type_size_;
            bool release_;
        };

        //strong guarantee.
        iframe_stream &fget_value(std::string &str) {
            int len;
            fget_value<int>(len);
            StreamPositionGuard guard(addr_, sizeof(int));
            ensure_buffer_capacity(len);
            str = std::string((char *) addr_, len);
            addr_ += len;
            guard.release();
            return *this;
        }


        template<typename T>
        inline std::enable_if_t<my_or<std::is_arithmetic<T>,
                std::is_same<T, std::string>>::value, iframe_stream &>
        operator>>(T &n) {
            return fget_value(n);
        }

        const frame_t &get_frame() const {
            return this->frame_;
        }

        const byte_t *get_addr() const {
            return addr_;
        }

    private:
        const frame_t frame_;
        const byte_t *addr_;
    };
}


#endif //PM_TINY_FRAME_STREAM_HPP
