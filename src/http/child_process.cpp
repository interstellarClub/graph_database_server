#include "child_process.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace procbridge {

namespace {

void setNonBlocking(int fd) {
    int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void closeFd(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

long long nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

ProcessResult runProcess(const std::string& command, const std::string& input,
                         int timeoutMs, const std::string& cwd) {
    ProcessResult res;
    const long long start = nowMs();

    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};

    if (::pipe(inPipe) != 0 || ::pipe(outPipe) != 0 || ::pipe(errPipe) != 0) {
        res.failedToStart = true;
        res.error = std::string("pipe() failed: ") + std::strerror(errno);
        closeFd(inPipe[0]); closeFd(inPipe[1]);
        closeFd(outPipe[0]); closeFd(outPipe[1]);
        closeFd(errPipe[0]); closeFd(errPipe[1]);
        return res;
    }

    pid_t pid = ::fork();
    if (pid < 0) {
        res.failedToStart = true;
        res.error = std::string("fork() failed: ") + std::strerror(errno);
        closeFd(inPipe[0]); closeFd(inPipe[1]);
        closeFd(outPipe[0]); closeFd(outPipe[1]);
        closeFd(errPipe[0]); closeFd(errPipe[1]);
        return res;
    }

    if (pid == 0) {
        // ---- 子进程 ----
        ::dup2(inPipe[0], STDIN_FILENO);
        ::dup2(outPipe[1], STDOUT_FILENO);
        ::dup2(errPipe[1], STDERR_FILENO);
        closeFd(inPipe[0]); closeFd(inPipe[1]);
        closeFd(outPipe[0]); closeFd(outPipe[1]);
        closeFd(errPipe[0]); closeFd(errPipe[1]);
        if (!cwd.empty()) ::chdir(cwd.c_str());
        ::execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        ::_exit(127);  // exec 失败
    }

    // ---- 父进程 ----
    closeFd(inPipe[0]);
    closeFd(outPipe[1]);
    closeFd(errPipe[1]);
    setNonBlocking(inPipe[1]);
    setNonBlocking(outPipe[0]);
    setNonBlocking(errPipe[0]);

    std::size_t written = 0;
    bool inClosed = false;   // stdin 侧已关闭 / 出错
    if (input.empty()) {
        closeFd(inPipe[1]);  // 立即给子进程 EOF，避免它一直阻塞在读取
        inClosed = true;
    }
    bool outEof = false;
    bool errEof = false;
    bool timedOut = false;

    for (;;) {
        if ((inClosed || written >= input.size()) && outEof && errEof) break;

        if (timeoutMs > 0 && nowMs() - start >= timeoutMs) {
            timedOut = true;
            break;
        }

        struct pollfd fds[3];
        int nfds = 0;
        int inIdx = -1, outIdx = -1, errIdx = -1;

        if (!inClosed && written < input.size()) {
            inIdx = nfds;
            fds[nfds].fd = inPipe[1];
            fds[nfds].events = POLLOUT;
            fds[nfds++].revents = 0;
        }
        if (!outEof) {
            outIdx = nfds;
            fds[nfds].fd = outPipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds++].revents = 0;
        }
        if (!errEof) {
            errIdx = nfds;
            fds[nfds].fd = errPipe[0];
            fds[nfds].events = POLLIN;
            fds[nfds++].revents = 0;
        }
        if (nfds == 0) break;

        int waitMs = 200;
        if (timeoutMs > 0) {
            long long left = timeoutMs - (nowMs() - start);
            if (left <= 0) {
                timedOut = true;
                break;
            }
            waitMs = static_cast<int>(left > 200 ? 200 : left);
        }

        int r = ::poll(fds, static_cast<nfds_t>(nfds), waitMs);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue;

        // 写 stdin
        if (inIdx >= 0) {
            short rev = fds[inIdx].revents;
            if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
                inClosed = true;
                if (written < input.size()) res.stdinClosedEarly = true;
            } else if (rev & POLLOUT) {
                ssize_t n = ::write(inPipe[1], input.data() + written, input.size() - written);
                if (n > 0) {
                    written += static_cast<std::size_t>(n);
                    if (written >= input.size()) {
                        closeFd(inPipe[1]);  // 关闭写端，让子进程看到 EOF
                        inClosed = true;
                    }
                } else if (n < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                    inClosed = true;
                    res.stdinClosedEarly = written < input.size();
                }
            }
        }

        // 读 stdout
        if (outIdx >= 0 && (fds[outIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            for (;;) {
                ssize_t n = ::read(outPipe[0], buf, sizeof(buf));
                if (n > 0) {
                    res.stdoutData.append(buf, static_cast<std::size_t>(n));
                    continue;
                }
                if (n == 0) {
                    outEof = true;
                } else {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    outEof = true;
                }
                break;
            }
        }

        // 读 stderr
        if (errIdx >= 0 && (fds[errIdx].revents & (POLLIN | POLLHUP | POLLERR))) {
            char buf[4096];
            for (;;) {
                ssize_t n = ::read(errPipe[0], buf, sizeof(buf));
                if (n > 0) {
                    res.stderrData.append(buf, static_cast<std::size_t>(n));
                    continue;
                }
                if (n == 0) {
                    errEof = true;
                } else {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    errEof = true;
                }
                break;
            }
        }
    }

    closeFd(inPipe[1]);
    closeFd(outPipe[0]);
    closeFd(errPipe[0]);

    if (timedOut) {
        res.timedOut = true;
        ::kill(pid, SIGKILL);
    }

    int status = 0;
    for (;;) {
        pid_t w = ::waitpid(pid, &status, 0);
        if (w == pid) break;
        if (w < 0 && errno == EINTR) continue;
        break;
    }

    if (WIFEXITED(status)) {
        res.exitCode = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        res.exitCode = 128 + WTERMSIG(status);
        res.killedBySignal = true;
    }
    res.durationMs = nowMs() - start;
    return res;
}

}  // namespace procbridge
