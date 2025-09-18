#include "nvim_client.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <sched.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace godot {

namespace {
constexpr pid_t INVALID_PID = -1;
constexpr int INVALID_FD = -1;
}

NvimClient::~NvimClient() {
	stop();
}

bool NvimClient::start(const std::string &p_command, const std::vector<std::string> &p_arguments, const std::string &p_working_directory) {
	stop();

	int stdin_pipe[2] = { INVALID_FD, INVALID_FD };
	int stdout_pipe[2] = { INVALID_FD, INVALID_FD };
	int stderr_pipe[2] = { INVALID_FD, INVALID_FD };

	if (pipe(stdin_pipe) == -1) {
		return false;
	}

	if (pipe(stdout_pipe) == -1) {
		_close_fd(stdin_pipe[0]);
		_close_fd(stdin_pipe[1]);
		return false;
	}

	if (pipe(stderr_pipe) == -1) {
		_close_fd(stdin_pipe[0]);
		_close_fd(stdin_pipe[1]);
		_close_fd(stdout_pipe[0]);
		_close_fd(stdout_pipe[1]);
		return false;
	}

	pid_t pid = fork();
	if (pid == -1) {
		_close_fd(stdin_pipe[0]);
		_close_fd(stdin_pipe[1]);
		_close_fd(stdout_pipe[0]);
		_close_fd(stdout_pipe[1]);
		_close_fd(stderr_pipe[0]);
		_close_fd(stderr_pipe[1]);
		return false;
	}

	if (pid == 0) {
		// Child process
		if (!p_working_directory.empty()) {
			if (chdir(p_working_directory.c_str()) == -1) {
				std::_Exit(EXIT_FAILURE);
			}
		}

		dup2(stdin_pipe[0], STDIN_FILENO);
		dup2(stdout_pipe[1], STDOUT_FILENO);
		dup2(stderr_pipe[1], STDERR_FILENO);

		_close_fd(stdin_pipe[0]);
		_close_fd(stdin_pipe[1]);
		_close_fd(stdout_pipe[0]);
		_close_fd(stdout_pipe[1]);
		_close_fd(stderr_pipe[0]);
		_close_fd(stderr_pipe[1]);

		std::vector<char *> argv;
		argv.reserve(p_arguments.size() + 2);
		argv.push_back(const_cast<char *>(p_command.c_str()));
		for (const std::string &arg : p_arguments) {
			argv.push_back(const_cast<char *>(arg.c_str()));
		}
		argv.push_back(nullptr);

		execvp(p_command.c_str(), argv.data());

		// If execvp returns, there was an error.
		std::_Exit(EXIT_FAILURE);
	}

	// Parent process
	_close_fd(stdin_pipe[0]);
	_close_fd(stdout_pipe[1]);
	_close_fd(stderr_pipe[1]);

	stdin_fd = stdin_pipe[1];
	stdout_fd = stdout_pipe[0];
	stderr_fd = stderr_pipe[0];
	child_pid = pid;

	_make_non_blocking(stdout_fd);
	_make_non_blocking(stderr_fd);

	return true;
}

void NvimClient::stop() {
	if (child_pid == INVALID_PID) {
		return;
	}

	if (kill(child_pid, SIGTERM) == -1 && errno != ESRCH) {
		// Best effort: continue to wait even if kill failed for reasons other than "no such process".
	}

	if (waitpid(child_pid, nullptr, 0) == -1) {
		// Ignore waitpid failure; process might have already been reaped elsewhere.
	}

	child_pid = INVALID_PID;
	_close_fd(stdin_fd);
	_close_fd(stdout_fd);
	_close_fd(stderr_fd);
}

bool NvimClient::is_running() {
	if (child_pid == INVALID_PID) {
		return false;
	}

	int status = 0;
	pid_t result = waitpid(child_pid, &status, WNOHANG);
	if (result == 0) {
		return true;
	}

	if (result == child_pid) {
		child_pid = INVALID_PID;
		_close_fd(stdin_fd);
		_close_fd(stdout_fd);
		_close_fd(stderr_fd);
		return false;
	}

	return false;
}

size_t NvimClient::write(const uint8_t *p_data, size_t p_length) {
	if (stdin_fd == INVALID_FD || p_data == nullptr || p_length == 0) {
		return 0;
	}

	size_t total_written = 0;
	while (total_written < p_length) {
		size_t remaining = p_length - total_written;
		ssize_t result = ::write(stdin_fd, p_data + total_written, remaining);
		if (result > 0) {
			total_written += static_cast<size_t>(result);
			continue;
		}

		if (result == -1 && (errno == EINTR)) {
			continue;
		}

		if (result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			// Yield to give the child process time to drain stdin.
			sched_yield();
			continue;
		}

		break;
	}

	return total_written;
}

std::vector<uint8_t> NvimClient::read_available() {
	std::vector<uint8_t> data;
	if (stdout_fd == INVALID_FD) {
		return data;
	}

	data.reserve(4096);
	uint8_t buffer[4096];
	while (true) {
		ssize_t read_bytes = ::read(stdout_fd, buffer, sizeof(buffer));
		if (read_bytes > 0) {
			data.insert(data.end(), buffer, buffer + read_bytes);
			continue;
		}

		if (read_bytes == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			break;
		}

		break;
	}

	return data;
}

void NvimClient::_close_fd(int &p_fd) {
	if (p_fd != INVALID_FD) {
		close(p_fd);
		p_fd = INVALID_FD;
	}
}

void NvimClient::_make_non_blocking(int p_fd) const {
	if (p_fd == INVALID_FD) {
		return;
	}

	int flags = fcntl(p_fd, F_GETFL, 0);
	if (flags == -1) {
		return;
	}

	fcntl(p_fd, F_SETFL, flags | O_NONBLOCK);
}

} // namespace godot
