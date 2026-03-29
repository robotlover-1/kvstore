#include <stdio.h>
#include "kvstore.h"

int proactor_start(unsigned short port, msg_handler handler) {
    (void)port; (void)handler;
    fprintf(stderr, "proactor skeleton included, but this package is validated with reactor mode only.\n");
    return -1;
}
