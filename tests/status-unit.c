#include "../third_party/captdriver/src/capt-command.h"
#include "../third_party/captdriver/src/capt-status.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned calls;
static int stuck;

void capt_sendrecv(uint16_t cmd, const void *data, size_t length, void *reply, size_t *reply_size)
{
    (void)cmd; (void)data; (void)length;
    if (reply && reply_size) {
        memset(reply, 0, *reply_size);
        ((unsigned char *)reply)[0] = stuck || ++calls < 3 ? CAPT_FL_CMD_BUSY_BIT : 0;
        *reply_size = 44;
    }
}

int main(int argc, char **argv)
{
    stuck = argc > 1 && !strcmp(argv[1], "stuck");
    capt_init_status();
    capt_wait_xready_only();
    if (calls != 3) return 1;
    puts("Status polling: ready state reached after transient busy");
    return 0;
}
