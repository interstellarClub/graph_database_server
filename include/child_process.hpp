#ifndef PROCBRIDGE_CHILD_PROCESS_HPP
#define PROCBRIDGE_CHILD_PROCESS_HPP

// ---------------------------------------------------------------------------
// procbridge::runProcess —— 启动本地控制台程序，喂 stdin，收 stdout/stderr
//
//   * fork + exec("/bin/sh -c <command>")，因此支持管道、重定向等 shell 语法
//   * 三路管道全部非阻塞 + poll()，避免「写满 stdin」或「读 stdout」造成的死锁
//   * 支持超时（毫秒），超时后 SIGKILL 子进程
// ---------------------------------------------------------------------------

#include <string>

namespace procbridge {

struct ProcessResult {
    int exitCode = -1;          // 正常退出为 exit code；被信号杀死为 128+signo
    std::string stdoutData;     // 子进程标准输出原文
    std::string stderrData;     // 子进程标准错误原文
    bool timedOut = false;      // 是否超时
    bool failedToStart = false; // 是否根本没启动起来
    bool killedBySignal = false;
    bool stdinClosedEarly = false;  // 子进程未读完 stdin 就退出/关闭
    std::string error;          // failedToStart 时的原因
    long long durationMs = 0;
};

// command  : 要执行的命令（通过 /bin/sh -c 执行）
// input    : 写入子进程 stdin 的数据（可为空）
// timeoutMs: 超时毫秒数（<=0 表示不限时）
// cwd      : 子进程工作目录（为空则继承当前目录）
ProcessResult runProcess(const std::string& command, const std::string& input,
                         int timeoutMs = 3000, const std::string& cwd = std::string());

}  // namespace procbridge

#endif  // PROCBRIDGE_CHILD_PROCESS_HPP
