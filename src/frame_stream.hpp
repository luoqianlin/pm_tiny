//
// Created by sansi on 2021/10/9.
//

#ifndef PM_TINY_FRAME_STREAM_HPP
#define PM_TINY_FRAME_STREAM_HPP

#include <utility>

#include "session.h"

namespace pm_tiny {

    constexpr uint8_t frame_delimiter = '\n';
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

    using frame_ptr_t = std::shared_ptr<frame_t>;

    template<typename B1, typename B2>
    struct my_or : public std::conditional<B1::value, B1, B2>::type {
    };

    class oframe_stream {
    public:

        explicit oframe_stream(std::shared_ptr<frame_t> frame) : frame_(std::move(frame)) {
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

    class iframe_stream {
    public:
        explicit iframe_stream(frame_t frame) : frame_(std::move(frame)) {
            this->addr_ = frame_.data();
        }

        iframe_stream(const iframe_stream &) = delete;

        iframe_stream(iframe_stream &&) = delete;

        template<typename T>
        inline std::enable_if_t<std::is_arithmetic<T>::value, iframe_stream &>
        fget_value(T &n) {
            n = *((T *) addr_);
            addr_ += sizeof(T);
            return *this;
        }

        iframe_stream &fget_value(std::string &str) {
            int len;
            fget_value<int>(len);
            str = std::string((char *) addr_, len);
            addr_ += len;
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

    private:
        const frame_t frame_;
        const byte_t *addr_;
    };
}


#endif //PM_TINY_FRAME_STREAM_HPP
