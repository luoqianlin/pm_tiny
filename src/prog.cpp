//
// Created by qianlinluo@foxmail.com on 2022/6/27.
//

#include "prog.h"
#include "log.h"

namespace pm_tiny {
     auto f_close (int &fd) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    };

     auto f_log_open(std::string &path, int oflag = O_CREAT | O_RDWR) {
        const char *file = path.c_str();
        int fd = open(file, oflag, S_IRUSR | S_IWUSR);
        if (fd == -1) {
            pm_tiny::logger->syscall_errorlog("open");
        }
        return fd;
    };

     auto get_file_size(int fd) {
        struct stat st;
        int rc = fstat(fd, &st);
        if (rc == -1) {
            logger->syscall_errorlog("fstat");
        }
        return st.st_size;
    };



    std::ostream &operator<<(std::ostream &os, struct prog_info_t const &prog) {
        os << "name:'" << prog.name + "'" << " pid:" << prog.pid << " ";
        os << "work dir:" << prog.work_dir << " ";
        os << "args: ";
        std::for_each(std::begin(prog.args), std::end(prog.args), [&os](const std::string &s) {
            os << s << " ";
        });
        return os;
    }

    void prog_info_t::close_pipefds() {
        std::for_each(std::begin(this->rpipefd),
                      std::end(this->rpipefd),
                      pm_tiny::f_close);
    }

    void prog_info_t::close_logfds() {
        std::for_each(std::begin(this->logfile_fd),
                      std::end(this->logfile_fd),
                      pm_tiny::f_close);
    }

    void prog_info_t::set_state(int s) {
        this->state = s;
    }

    /**
     * 监管的程序运行结束后会关闭pipefd,
     * select会监听到pipefd关闭进而关闭pipfd和对应的日志文件fd
     * */
    void prog_info_t::close_fds() {
        for (int i = 0; i < 2; i++) {
            if (this->rpipefd[i] != -1
                && this->logfile_fd[i] != -1) {
//                    logger->debug("start safe close fds");
                read_pipe(i);
//                    logger->debug("end safe close fds");
            }
        }
        this->close_pipefds();
        this->close_logfds();
        this->pid = -1;
    }

    std::string prog_info_t::get_dsc_name() const {
        return this->name + "(" + std::to_string(this->backup_pid) + ")";
    }

    void prog_info_t::init_prog_log() {
        for (int i = 0; i < 2; i++) {
            int oflag = O_CREAT | O_RDWR;
            this->logfile_fd[i] = f_log_open(this->logfile[i], oflag);
            lseek64(this->logfile_fd[i], 0, SEEK_END);
            this->logfile_size[i] = get_file_size(this->logfile_fd[i]);
            logger->info("log file %s  %ld bytes\n",
                         this->logfile[i].c_str(), this->logfile_size[i]);
        }
    }

    void prog_info_t::read_pipe(int i) {
        int nread;
        int rc;
        char buffer[4096];
        int &fd = this->rpipefd[i];
        ioctl(fd, FIONREAD, &nread);
//            printf("FIONREAD:%d\n",nread);
        if (nread == 0) {
            close(fd);
            close(this->logfile_fd[i]);
            logger->debug("pid:%d pipe fd %d closed\n",
                          this->backup_pid, fd);
            fd = -1;
            this->logfile_fd[i] = -1;
        } else {
            int remaining_bytes = nread;
            do {
                int max_nread;
#if PM_TINY_PIPE_SPLICE
                max_nread = remaining_bytes;
#else
                max_nread = std::min(remaining_bytes, (int) sizeof(buffer));
#endif

//                    off64_t off_out;
//                    off_out = lseek64(this->logfile_fd[i], 0, SEEK_END);
//                    if (off_out == -1) {
//                        logger->syscall_errorlog("name:%s pid:%d lseek64", this->name.c_str(), this->pid);
//                        break;
//                    }
                auto rotate_log_file = [this](int i) {
                    close(this->logfile_fd[i]);
                    pm_tiny::logger_t::logfile_cycle_write(this->logfile[i], this->logfile_count);
                    int oflag = O_CREAT | O_RDWR | O_TRUNC;
                    this->logfile_fd[i] = pm_tiny::f_log_open(this->logfile[i], oflag);
                    this->logfile_size[i] = 0;
                };
                int off_out = this->logfile_size[i];
                if (off_out >= this->logfile_maxsize) {
//                        logger->info("exceeds the maximum file size of %ld bytes,truncate\n",
//                                     this->logfile_maxsize);
                    rotate_log_file(i);
                    off_out = 0;
                }
                int exceed_n = off_out + remaining_bytes - this->logfile_maxsize;

                if (exceed_n > 0) {
                    max_nread = this->logfile_maxsize - off_out;
                }
#if PM_TINY_PIPE_SPLICE
                do {
                    rc = splice(fd, nullptr, this->logfile_fd[i],
                                nullptr, (size_t) max_nread, 0);
                } while (rc == -1 && errno == EAGAIN);
#else
                rc = (int) pm_tiny::safe_read(fd, buffer, max_nread);
#endif
                if (rc > 0) {
#if !PM_TINY_PIPE_SPLICE
                    pm_tiny::safe_write(this->logfile_fd[i], buffer, rc);
#endif
                    this->logfile_size[i] += rc;
                    remaining_bytes -= rc;
                } else if ((rc == -1 && errno != EINTR)) {
                    logger->syscall_errorlog("name:%s pid:%d fdin:%d fdout:%d off_out:%d splice",
                                             this->name.c_str(), this->pid, fd, this->logfile_fd[i], (int) off_out);
                    break;
                }
            } while (remaining_bytes > 0);
        }
    }
}