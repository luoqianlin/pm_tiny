//
// Created by qianlinluo@foxmail.com on 2022/7/25.
//
#include "string_utils.h"

namespace mgr {

    namespace utils {
        void parse_numeric(int text_length, const std::string &text, int &j) {
            int num = 0;
            while (text[j] >= '0' && text[j] <= '9' && num <= 108) {
                num = num * 10 + (text[j] - '0');
                j++;
                if (j >= text_length) {
                    break;
                }
            }
        }

        void get_leftovers(const std::string &text, std::string &truncate_text, int i, int j) {
            for (int k = i; k < j; k++) {
                truncate_text += text[k];
            }
        }

/*
        *  if (n > 108) break;
        *  printf("\033[%dm %3d\033[m", n, n);
         *  ^[[97;42m
        * */
        std::pair<std::string, std::string>
        remove_ANSI_escape_code(const std::string &text) {
            std::string pure_text;
            std::string truncate_text;
            int text_length = (int) text.size();
            for (int i = 0; i < text_length;) {
                if (i + 1 < text_length) {
                    if (text[i] == '\033' && text[i + 1] == '[') {
                        int j = i + 2;
                        if (j >= text_length) {
                            get_leftovers(text, truncate_text, i, j);
                            break;
                        }
                        parse_numeric(text_length, text, j);

                        if (j >= text_length) {
                            get_leftovers(text, truncate_text, i, j);
                            break;
                        }

                        if (text[j] == 'm') {
                            i = j + 1;
                            continue;
                        } else {
                            if (text[j] == ';') {
                                j++;
                                if (j >= text_length) {
                                    get_leftovers(text, truncate_text, i, j);
                                    break;
                                }
                                parse_numeric(text_length, text, j);
                                if (j >= text_length) {
                                    get_leftovers(text, truncate_text, i, j);
                                    break;
                                }
                                if (text[j] == 'm') {
                                    i = j + 1;
                                    continue;
                                }
                            }

                            for (; i <= j; i++) {
                                pure_text += text[i];
                            }
                        }
                    } else {
                        pure_text += text[i];
                        i++;
                    }
                } else {
                    if (text[i] == '\033') {
                        truncate_text += text[i];
                    } else {
                        pure_text += text[i];
                    }
                    break;
                }
            }
            return {pure_text, truncate_text};
        }

        std::tuple<std::string, std::string>
        splitext(const std::string &file) {
            std::string name, ext;
            auto idx = file.rfind('.');
            if (idx != std::string::npos && idx != 0) {
                int i = static_cast<int>(idx - 1);
                for (; i >= 0; i--) {
                    if (file[i] != '.') {
                        break;
                    }
                }
                if (i < 0) {
                    name = file;
                } else {
                    name = file.substr(0, idx);
                    ext = file.substr(idx, file.length() - idx);
                }
            } else {
                name = file;
            }
            return {name, ext};
        }
    }
}