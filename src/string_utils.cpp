//
// Created by qianlinluo@foxmail.com on 2022/7/25.
//
#include "string_utils.h"

namespace mgr {

    namespace utils {
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
                            for (int k = i; k < j; k++) {
                                truncate_text += text[k];
                            }
                            break;
                        }
                        int num = 0;
                        while (text[j] >= '0' && text[j] <= '9' && num <= 108) {
                            num = num * 10 + text[j] - '0';
                            j++;
                            if (j >= text_length) {
                                break;
                            }
                        }

                        if (j >= text_length) {
                            for (int k = i; k < j; k++) {
                                truncate_text += text[k];
                            }
                            break;
                        }

                        if (text[j] == 'm') {
                            i = j + 1;
                            continue;
                        } else {
                            if (text[j] == ';') {
                                j++;
                                if (j >= text_length) {
                                    for (int k = i; k < j; k++) {
                                        truncate_text += text[k];
                                    }
                                    break;
                                }
                                int num2 = 0;
                                while (text[j] >= '0' && text[j] <= '9' && num2 <= 108) {
                                    num2 = num2 * 10 + text[j] - '0';
                                    j++;
                                    if (j >= text_length) {
                                        break;
                                    }
                                }
                                if (j >= text_length) {
                                    for (int k = i; k < j; k++) {
                                        truncate_text += text[k];
                                    }
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
    }
}