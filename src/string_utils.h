#ifndef PM_TINY_STRING_UTILS_H
#define PM_TINY_STRING_UTILS_H

#include <string>
#include <algorithm>
#include <iterator>
#include <vector>

namespace mgr {

    namespace utils {
// trim from start (in place)
        static inline void ltrim(std::string &s) {
            s.erase(s.begin(), std::find_if(s.begin(), s.end(), [](unsigned char ch) {
                return !std::isspace(ch);
            }));
        }

// trim from end (in place)
        static inline void rtrim(std::string &s) {
            s.erase(std::find_if(s.rbegin(), s.rend(), [](unsigned char ch) {
                return !std::isspace(ch);
            }).base(), s.end());
        }

// trim from both ends (in place)
        static inline void trim(std::string &s) {
            ltrim(s);
            rtrim(s);
        }

// trim from start (copying)
        static inline std::string ltrim_copy(std::string s) {
            ltrim(s);
            return s;
        }

// trim from end (copying)
        static inline std::string rtrim_copy(std::string s) {
            rtrim(s);
            return s;
        }

// trim from both ends (copying)
        static inline std::string trim_copy(std::string s) {
            trim(s);
            return s;
        }


        template<typename Out>
        void split(const std::string &s, const std::vector<char> &delims, Out result) {
            int i, prev_index = 0;
            for (i = 0; i < s.length(); i++) {
                if (std::find(delims.begin(), delims.end(), s[i]) != delims.end()) {
                    if (i - prev_index > 0) {
                        *result++ = s.substr(prev_index, i - prev_index);
                    } else {
                        *result++ = "";
                    }
                    prev_index = i + 1;
                }
            }

            if (i - prev_index > 0) {
                *result++ = s.substr(prev_index, i - prev_index);
            } else {
                *result++ = "";
            }

        }

        inline std::vector<std::string> split(const std::string &s, const std::vector<char> &delims) {
            std::vector<std::string> elems;
            split(s, delims, std::back_inserter(elems));
            return elems;
        }
    }


}
#endif //PM_TINY_STRING_UTILS_H
