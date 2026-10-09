#include "msg_sink.h"
#include <stddef.h>

static msg_sink_cb g_sink = NULL;

void msg_sink_register(msg_sink_cb cb) {
    g_sink = cb;
}

void msg_sink_notify(const AcarsMessage *m, const char *source) {
    if (g_sink)
        g_sink(m, source);
}
