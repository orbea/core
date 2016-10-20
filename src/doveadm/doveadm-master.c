/* Copyright (c) 2010-2016 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "str.h"
#include "istream.h"
#include "write-full.h"
#include "doveadm.h"

#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

#define MASTER_PID_FILE_NAME "master.pid"

static bool pid_file_read(const char *path, pid_t *pid_r)
{
	char buf[32];
	int fd;
	ssize_t ret;
	bool found;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		if (errno == ENOENT)
			return FALSE;
		i_fatal("open(%s) failed: %m", path);
	}

	ret = read(fd, buf, sizeof(buf)-1);
	if (ret <= 0) {
		if (ret == 0)
			i_error("Empty PID file in %s", path);
		else
			i_fatal("read(%s) failed: %m", path);
		found = FALSE;
	} else {
		if (buf[ret-1] == '\n')
			ret--;
		buf[ret] = '\0';
		if (str_to_pid(buf, pid_r) < 0)
			found = FALSE;
		else {
			found = !(*pid_r == getpid() ||
				  (kill(*pid_r, 0) < 0 && errno == ESRCH));
		}
	}
	i_close_fd(&fd);
	return found;
}

void doveadm_master_send_signal(int signo)
{
	const char *pidfile_path;
	unsigned int i;
	pid_t pid;

	pidfile_path = t_strconcat(doveadm_settings->base_dir,
				   "/"MASTER_PID_FILE_NAME, NULL);

	if (!pid_file_read(pidfile_path, &pid))
		i_fatal("Dovecot is not running (read from %s)", pidfile_path);

	if (kill(pid, signo) < 0)
		i_fatal("kill(%s, %d) failed: %m", dec2str(pid), signo);

	if (signo == SIGTERM) {
		/* wait for a while for the process to die */
		usleep(1000);
		for (i = 0; i < 30; i++) {
			if (kill(pid, 0) < 0) {
				if (errno != ESRCH)
					i_error("kill() failed: %m");
				break;
			}
			usleep(100000);
		}
	}
}

static void cmd_stop(int argc ATTR_UNUSED, char *argv[] ATTR_UNUSED)
{
	doveadm_master_send_signal(SIGTERM);
}

static void cmd_reload(int argc ATTR_UNUSED, char *argv[] ATTR_UNUSED)
{
	doveadm_master_send_signal(SIGHUP);
}

static void cmd_service_stop(int argc, char *argv[])
{
	const char *path, *line;
	int fd;

	if (argc == 1)
		help_ver2(&doveadm_cmd_service_stop_ver2);

	path = t_strconcat(doveadm_settings->base_dir, "/master", NULL);
	fd = net_connect_unix(path);
	if (fd == -1)
		i_fatal("net_connect_unix(%s) failed: %m", path);
	net_set_nonblock(fd, FALSE);

	string_t *cmd = t_str_new(128);
	str_append(cmd, "VERSION\tmaster-client\t1\t0\nSTOP");
	for (int i = 1; i < argc; i++) {
		str_append_c(cmd, '\t');
		str_append(cmd, argv[i]);
	}
	str_append_c(cmd, '\n');
	if (write_full(fd, str_data(cmd), str_len(cmd)) < 0)
		i_error("write(%s) failed: %m", path);

	alarm(5);
	struct istream *input = i_stream_create_fd(fd, IO_BLOCK_SIZE, FALSE);
	if (i_stream_read_next_line(input) == NULL ||
	    (line = i_stream_read_next_line(input)) == NULL) {
		i_error("read(%s) failed: %s", path, i_stream_get_error(input));
		doveadm_exit_code = EX_TEMPFAIL;
	} else if (line[0] == '-') {
		doveadm_exit_code = DOVEADM_EX_NOTFOUND;
		i_error("%s", line+1);
	} else if (line[0] != '+') {
		i_error("Unexpected input from %s: %s", path, line);
		doveadm_exit_code = EX_TEMPFAIL;
	}
	alarm(0);
	i_stream_destroy(&input);
	i_close_fd(&fd);
}

struct doveadm_cmd_ver2 doveadm_cmd_stop_ver2 = {
	.old_cmd = cmd_stop,
	.name = "stop",
	.usage = "",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAMS_END
};

struct doveadm_cmd_ver2 doveadm_cmd_reload_ver2 = {
        .old_cmd = cmd_reload,
        .name = "reload",
        .usage = "",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAMS_END
};

struct doveadm_cmd_ver2 doveadm_cmd_service_stop_ver2 = {
	.old_cmd = cmd_service_stop,
	.name = "service stop",
	.usage = "<service> [<service> [...]]",
DOVEADM_CMD_PARAMS_START
DOVEADM_CMD_PARAM('\0', "service", CMD_PARAM_ARRAY, CMD_PARAM_FLAG_POSITIONAL)
DOVEADM_CMD_PARAMS_END
};
