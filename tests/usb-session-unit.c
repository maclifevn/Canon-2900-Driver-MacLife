#include "../backend/usb-session.c"
#include "capt-command.h"
#include "capt-status.h"
#include "word.h"

#include <assert.h>

static int listener;
static unsigned jobs;
static bool reserved, offline, heartbeat, ended;
static uint16_t page, printing, previous, before_previous;
static uint8_t reply[64];
static size_t reply_size;
static uint8_t command[65536 + 8192];
static size_t command_size;

/* Exercise the real per-job fd handoff and relay with a persistent fake printer. */
int launch_activate_socket(const char *name, int **sockets, size_t *count)
{
	assert(!strcmp(name, "CAPT"));
	*sockets = malloc(sizeof(**sockets));
	assert(*sockets);
	**sockets = listener;
	*count = 1;
	return 0;
}

static int session_usb_prepare(const char *uri, char *device_id, int size)
{
	assert(!strcmp(uri, "canon2900usb://Canon/LBP2900"));
	assert(!reserved && !reply_size && !command_size);
	strlcpy(device_id, "MFG:Canon;MDL:LBP2900;", size);
	fprintf(stderr, "Mock USB job %u\n", ++jobs);
	return CUPS_BACKEND_OK;
}

static ssize_t session_usb_read(void *buffer, size_t size)
{
	if (!reply_size) {
		usleep(1000);
		return 0;
	}
	/* Deliberately split even the six-byte CAPT reply header. */
	assert(size >= 3);
	size_t part = reply_size < 3 ? reply_size : 3;
	memcpy(buffer, reply, part);
	reply_size -= part;
	memmove(reply, reply + part, reply_size);
	return (ssize_t)part;
}

static void mock_command(uint16_t cmd, const uint8_t *body)
{
	assert(!reply_size);
	memset(reply, 0, sizeof(reply));
	size_t prefix = cmd == CAPT_GET_PRINTER_INFO && jobs == 2 ? 3 : 0;
	uint8_t *frame = reply + prefix;
	uint8_t *data = frame + 4;
	size_t length = 6;
	switch (cmd) {
	case CAPT_GET_PRINTER_INFO:
		assert(!reserved);
		length = 16;
		data[6] = 0xf0;
		data[7] = 0xff;
		data[8] = 64;
		break;
	case CAPT_GET_BASIC_STATUS:
	case CAPT_GET_EXTENDED_STATUS:
		length = 44;
		data[0] = offline ? CAPT_FL_OFFLINE_BIT : CAPT_FL_PRINTER_FREE_BIT;
		data[CAPT_XSTAT_BUF_LO] = 15;
		data[CAPT_XSTAT_START_LO] = LO(page);
		data[CAPT_XSTAT_PRINTING_LO] = LO(printing);
		data[CAPT_XSTAT_SHIPPED_LO] = LO(printing);
		data[CAPT_XSTAT_PRINTED_LO] = LO(printing);
		break;
	case CAPT_RESERVE_UNIT:
		assert(!reserved && previous == CAPT_GET_EXTENDED_STATUS &&
		       before_previous == CAPT_GET_EXTENDED_STATUS);
		reserved = true;
		heartbeat = ended = false;
		length = 8;
		data[2] = LO(jobs);
		break;
	case CAPT_SET_JOB_INFO2:
		assert(reserved && WORD(body[18], body[19]) == jobs);
		assert(body[16] == CAPT_JOBFLAG_START || body[16] == CAPT_JOBFLAG_CONT ||
		       body[16] == CAPT_JOBFLAG_END_LIN);
		if (body[16] == CAPT_JOBFLAG_CONT)
			heartbeat = true;
		if (body[16] == CAPT_JOBFLAG_END_LIN)
			ended = true;
		break;
	case CAPT_GO_OFFLINE:
		offline = true;
		break;
	case CAPT_GO_ONLINE:
		offline = false;
		page = printing = 0;
		break;
	case CAPT_MULTI_COMMAND:
		assert(reserved);
		++page;
		return;
	case CAPT_IC_VIDEO_DATA:
	case CAPT_IC_BLACK_END:
		assert(reserved);
		return;
	case CAPT_START_PRINT:
		assert(WORD(body[0], body[1]) == page);
		printing = page;
		break;
	case CAPT_RELEASE_UNIT:
		assert(reserved && heartbeat && ended && printing == 2 && WORD(body[0], body[1]) == jobs);
		reserved = false;
		break;
	}
	frame[0] = LO(cmd);
	frame[1] = HI(cmd);
	frame[2] = cmd == CAPT_GET_PRINTER_INFO && jobs == 1 ? 0x16 : LO(length);
	reply_size = prefix + length;
	before_previous = previous;
	previous = cmd;
}

static int session_usb_write(const void *buffer, size_t size)
{
	assert(command_size + size <= sizeof(command));
	memcpy(command + command_size, buffer, size);
	command_size += size;
	while (command_size >= 4) {
		size_t length = WORD(command[2], command[3]);
		assert(length >= 4);
		if (command_size < length)
			break;
		mock_command(WORD(command[0], command[1]), command + 4);
		command_size -= length;
		memmove(command, command + length, command_size);
	}
	return 0;
}

int main(int argc, char **argv)
{
	signal(SIGPIPE, SIG_IGN);
	assert(argc == 3);
	if (!strcmp(argv[1], "serve")) {
		listener = atoi(argv[2]);
		return session_serve();
	}
	return session_submit(argv[2], "canon2900usb://Canon/LBP2900", STDIN_FILENO, 1);
}
