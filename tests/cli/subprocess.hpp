// Run the built analyzer-cli and capture what it did.
//
// The harness tests drive the real executable rather than calling run(), so
// they exercise what a user gets: argument handling in main(), the allocation
// trap in debug builds, exit status, and what lands on each stream. POSIX only;
// the tests that include this compile to nothing elsewhere.

#pragma once

#if !defined(_WIN32)

#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace analyzer::cli::harness {

// What a finished child left behind.
struct Output {
    // Exit status, or 128 plus the signal number if it was killed (a failed
    // allocation trap shows up here as 134, SIGABRT).
    int status = -1;
    std::string out;
    std::string err;

    bool success() const noexcept { return status == 0; }
};

// Run `executable` with `args`, in `directory` if given, and wait for it. Both
// streams are drained together so a child that fills one pipe cannot deadlock
// against us reading the other.
inline Output run_process(const std::string& executable, const std::vector<std::string>& args,
                          const std::filesystem::path& directory = {}) {
    // Built before the fork: the child may only call async-signal-safe functions.
    std::vector<std::string> storage;
    storage.push_back(executable);
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& item : storage) {
        argv.push_back(item.data());
    }
    argv.push_back(nullptr);
    const std::string cwd = directory.string();

    std::array<int, 2> out_pipe{};
    std::array<int, 2> err_pipe{};
    if (pipe(out_pipe.data()) != 0 || pipe(err_pipe.data()) != 0) {
        throw std::runtime_error("pipe failed");
    }

    const pid_t child = fork();
    if (child < 0) {
        throw std::runtime_error("fork failed");
    }
    if (child == 0) {
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) {
            _exit(126);
        }
        execv(argv[0], argv.data());
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);

    Output result;
    std::array<pollfd, 2> fds{{{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}}};
    std::array<std::string*, 2> sinks{&result.out, &result.err};
    int open_streams = 2;
    while (open_streams > 0) {
        if (poll(fds.data(), fds.size(), -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        for (std::size_t i = 0; i < fds.size(); ++i) {
            if (fds[i].fd < 0 || (fds[i].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
                continue;
            }
            std::array<char, 65536> buffer;
            const ssize_t got = read(fds[i].fd, buffer.data(), buffer.size());
            if (got > 0) {
                sinks[i]->append(buffer.data(), static_cast<std::size_t>(got));
            } else if (got == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open_streams;
            }
        }
    }

    int wait_status = 0;
    while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(wait_status)) {
        result.status = WEXITSTATUS(wait_status);
    } else if (WIFSIGNALED(wait_status)) {
        result.status = 128 + WTERMSIG(wait_status);
    }
    return result;
}

}  // namespace analyzer::cli::harness

#endif  // !defined(_WIN32)
