#ifndef NVIM_CLIENT_H
#define NVIM_CLIENT_H

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

namespace godot {

class NvimClient {
public:
	NvimClient() = default;
	~NvimClient();

	bool start(const std::string &p_command, const std::vector<std::string> &p_arguments, const std::string &p_working_directory = std::string());
	void stop();
	bool is_running();
	pid_t get_pid() const { return child_pid; }

	size_t write(const uint8_t *p_data, size_t p_length);
	std::vector<uint8_t> read_available();

private:
	pid_t child_pid = -1;
	int stdin_fd = -1;
	int stdout_fd = -1;
	int stderr_fd = -1;

	void _close_fd(int &p_fd);
	void _make_non_blocking(int p_fd) const;
};

} // namespace godot

#endif // NVIM_CLIENT_H
