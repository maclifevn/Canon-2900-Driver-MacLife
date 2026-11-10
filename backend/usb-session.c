/* SPDX-License-Identifier: Apache-2.0 */

#include <cups/backend.h>
#include <cups/sidechannel.h>
#include <errno.h>
#include <fcntl.h>
#include <launch.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define SESSION_SOCKET "/private/var/run/canon2900usb.sock"
#define BACKCHANNEL_FD 3
#define JOB_FD_COUNT 4

/* The CUPS backend hands the job's pipes to a socket-activated process. Only
 * the pipes change between jobs. That process owns the open USB interface. */
struct session_job {
	char uri[2048];
	int copies;
	int file_input;
};

static int session_usb_prepare(const char *uri, char *device_id, int size);
static ssize_t session_usb_read(void *buffer, size_t size);
static int session_usb_write(const void *buffer, size_t size);

static int session_submit(const char *socket_path, const char *uri, int print_fd, int copies)
{
	struct session_job job = { .copies = copies, .file_input = print_fd != STDIN_FILENO };
	if (strlcpy(job.uri, uri, sizeof(job.uri)) >= sizeof(job.uri) || copies < 1)
		return CUPS_BACKEND_FAILED;
	if (fcntl(BACKCHANNEL_FD, F_GETFD) < 0 || fcntl(CUPS_SC_FD, F_GETFD) < 0) {
		fputs("ERROR: CAPT: CUPS job channels are unavailable\n", stderr);
		return CUPS_BACKEND_FAILED;
	}

	struct sockaddr_un address = { .sun_family = AF_UNIX };
	if (strlcpy(address.sun_path, socket_path, sizeof(address.sun_path)) >= sizeof(address.sun_path))
		return CUPS_BACKEND_FAILED;
	int client = socket(AF_UNIX, SOCK_STREAM, 0);
	if (client < 0 || connect(client, (struct sockaddr *)&address, sizeof(address)) < 0) {
		fprintf(stderr, "ERROR: CAPT: cannot connect to the USB session service: %s\n", strerror(errno));
		if (client >= 0)
			close(client);
		return CUPS_BACKEND_STOP;
	}

	int fds[JOB_FD_COUNT] = { print_fd, STDERR_FILENO, BACKCHANNEL_FD, CUPS_SC_FD };
	union { struct cmsghdr align; unsigned char data[CMSG_SPACE(sizeof(fds))]; } control = {0};
	struct iovec payload = { &job, sizeof(job) };
	struct msghdr message = {
		.msg_iov = &payload, .msg_iovlen = 1,
		.msg_control = control.data, .msg_controllen = sizeof(control.data),
	};
	struct cmsghdr *header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof(fds));
	memcpy(CMSG_DATA(header), fds, sizeof(fds));

	unsigned char result = CUPS_BACKEND_FAILED;
	ssize_t sent = sendmsg(client, &message, 0);
	if (sent == sizeof(job)) {
		/* Let the filter finish its cancellation commands before its pipe closes. */
		if (!job.file_input)
			signal(SIGTERM, SIG_IGN);
		if (read(client, &result, 1) != 1) {
			fprintf(stderr, "ERROR: CAPT: USB session service disconnected during the job\n");
			result = CUPS_BACKEND_FAILED;
		}
	} else {
		fprintf(stderr, "ERROR: CAPT: cannot hand the job to the USB session service: %s\n",
			sent < 0 ? strerror(errno) : "partial request");
	}
	close(client);
	return result;
}

static int session_receive(int client, struct session_job *job, int fds[JOB_FD_COUNT])
{
	/* A stalled client must not block all later jobs. */
	struct timeval timeout = { .tv_sec = 2 };
	if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)))
		return -1;
	union { struct cmsghdr align; unsigned char data[CMSG_SPACE(sizeof(int) * JOB_FD_COUNT)]; } control;
	struct iovec payload = { job, sizeof(*job) };
	struct msghdr message = {
		.msg_iov = &payload, .msg_iovlen = 1,
		.msg_control = control.data, .msg_controllen = sizeof(control.data),
	};
	ssize_t size = recvmsg(client, &message, MSG_WAITALL);
	if (size < 0)
		return -1;

	size_t count = 0;
	for (struct cmsghdr *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
		if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
			continue;
		size_t received = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
		const unsigned char *data = CMSG_DATA(header);
		for (size_t i = 0; i < received; ++i) {
			int fd;
			memcpy(&fd, data + i * sizeof(fd), sizeof(fd));
			if (count < JOB_FD_COUNT)
				fds[count] = fd;
			else
				close(fd);
			++count;
		}
	}
	if (size != sizeof(*job) || count != JOB_FD_COUNT || (message.msg_flags & MSG_CTRUNC) ||
	    !memchr(job->uri, '\0', sizeof(job->uri)) || job->copies < 1 ||
	    (job->file_input != 0 && job->file_input != 1))
		return -1;
	return 0;
}

static void session_sidechannel(const char *device_id)
{
	cups_sc_command_t command;
	cups_sc_status_t status;
	char data[2048];
	int size = sizeof(data);
	if (cupsSideChannelRead(&command, &status, data, &size, 1.0))
		return;

	status = CUPS_SC_STATUS_OK;
	size = 0;
	switch (command) {
	case CUPS_SC_CMD_GET_DEVICE_ID:
		size = (int)strlcpy(data, device_id, sizeof(data));
		break;
	case CUPS_SC_CMD_GET_BIDI:
		data[size++] = 1;
		break;
	case CUPS_SC_CMD_GET_STATE:
		data[size++] = CUPS_SC_STATE_ONLINE;
		break;
	case CUPS_SC_CMD_DRAIN_OUTPUT:
		/* The same loop writes USB output before handling side-channel commands. */
		break;
	default:
		status = CUPS_SC_STATUS_NOT_IMPLEMENTED;
		break;
	}
	cupsSideChannelWrite(command, status, data, size, 1.0);
}

static int session_print(int client, int input, const struct session_job *job)
{
	char device_id[2048];
	fputs("STATE: +connecting-to-device\n", stderr);
	struct pollfd disconnected[] = { { client, POLLIN, 0 }, { input, POLLHUP, 0 } };
	for (unsigned attempt = 0; ; ++attempt) {
		int status = session_usb_prepare(job->uri, device_id, sizeof(device_id));
		if (status == CUPS_BACKEND_OK)
			break;
		if (status != CUPS_BACKEND_RETRY || attempt == 7)
			return status;
		fputs("INFO: Waiting for the Canon LBP2900 to become available.\n", stderr);
		if (poll(disconnected, 2, 5000) != 0)
			return CUPS_BACKEND_CANCEL;
	}
	fputs("STATE: -connecting-to-device\nINFO: Sending data to printer.\n", stderr);

	struct pollfd events[] = { { client, POLLIN, 0 }, { input, POLLIN, 0 }, { CUPS_SC_FD, POLLIN, 0 } };
	int copies = job->copies;
	if (job->file_input)
		fputs("PAGE: 1 1\n", stderr);
	int wrote_data = 0;
	while (1) {
		int ready = poll(events, 3, wrote_data && !job->file_input ? 0 : -1);
		if (ready < 0) {
			if (errno == EINTR)
				continue;
			return CUPS_BACKEND_FAILED;
		}
		if (events[0].revents)
			return CUPS_BACKEND_CANCEL;

		char data[8192];
		if (events[1].revents) {
			ssize_t size = read(input, data, sizeof(data));
			if (size < 0) {
				if (errno == EINTR || errno == EAGAIN)
					continue;
				return CUPS_BACKEND_FAILED;
			}
			if (!size) {
				if (!job->file_input || --copies == 0)
					return CUPS_BACKEND_OK;
				if (lseek(input, 0, SEEK_SET) < 0)
					return CUPS_BACKEND_FAILED;
				fputs("PAGE: 1 1\n", stderr);
				continue;
			}
			if (session_usb_write(data, (size_t)size))
				return CUPS_BACKEND_FAILED;
			wrote_data = 1;
			continue;
		}
		if (events[2].revents & POLLIN) {
			session_sidechannel(device_id);
			continue;
		}
		if (events[2].revents)
			events[2].fd = -1;
		if (wrote_data && !job->file_input) {
			ssize_t size = session_usb_read(data, 512);
			if (size < 0 || (size && cupsBackChannelWrite(data, (size_t)size, 1.0) != size)) {
				fputs("ERROR: CAPT: USB backchannel failed during the job\n", stderr);
				return CUPS_BACKEND_FAILED;
			}
		}
	}
}

static void session_run_client(int client, int service_stderr)
{
	uid_t uid;
	gid_t gid;
	if (getpeereid(client, &uid, &gid) || (uid != geteuid() && uid != 0))
		return;

	struct session_job job;
	int fds[JOB_FD_COUNT] = { -1, -1, -1, -1 };
	unsigned char result = CUPS_BACKEND_FAILED;
	if (!session_receive(client, &job, fds) &&
	    dup2(fds[1], STDERR_FILENO) >= 0 && dup2(fds[2], BACKCHANNEL_FD) >= 0 &&
	    dup2(fds[3], CUPS_SC_FD) >= 0)
		result = (unsigned char)session_print(client, fds[0], &job);

	/* Restore the reserved descriptors before accepting any more fds. */
	dup2(service_stderr, STDERR_FILENO);
	dup2(service_stderr, BACKCHANNEL_FD);
	dup2(service_stderr, CUPS_SC_FD);
	for (size_t i = 0; i < JOB_FD_COUNT; ++i)
		if (fds[i] >= 0)
			close(fds[i]);
	(void)write(client, &result, 1);
}

static int session_serve(void)
{
	int *sockets = NULL;
	size_t count = 0;
	int error = launch_activate_socket("CAPT", &sockets, &count);
	if (error || count != 1) {
		fprintf(stderr, "ERROR: CAPT: cannot activate launchd socket: %s\n", strerror(error ? error : EINVAL));
		free(sockets);
		return 1;
	}
	int listener = fcntl(sockets[0], F_DUPFD_CLOEXEC, CUPS_SC_FD + 1);
	close(sockets[0]);
	free(sockets);
	int service_stderr = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, CUPS_SC_FD + 1);
	if (listener < 0 || service_stderr < 0 || dup2(service_stderr, BACKCHANNEL_FD) < 0 ||
	    dup2(service_stderr, CUPS_SC_FD) < 0)
		return 1;

	while (1) {
		int client = accept(listener, NULL, NULL);
		if (client < 0) {
			if (errno == EINTR)
				continue;
			return 1;
		}
		session_run_client(client, service_stderr);
		close(client);
	}
}
