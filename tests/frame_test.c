#include "check.h"
#include "voice/wr_frame.h"

#include <string.h>

enum { CONTINUATION = 0x0, BINARY = 0x2, PING = 0x9 };

static void test_chunked_and_fragmented(void)
{
    wr_frame_t frame = {0};
    const uint8_t payload[] = {0xf8, 0x11, 0x22, 0x33, 0x44, 0x55};
    const uint8_t ping[] = {0x99};

    CHECK(wr_frame_feed(&frame, BINARY, false, 3, 0, payload, 1) == WR_FRAME_MORE);
    CHECK(wr_frame_feed(&frame, BINARY, false, 3, 1, payload + 1, 2) == WR_FRAME_MORE);
    CHECK(wr_frame_feed(&frame, PING, true, 1, 0, ping, 1) == WR_FRAME_IGNORED);
    CHECK(wr_frame_feed(&frame, CONTINUATION, true, 3, 0, payload + 3, 2) == WR_FRAME_MORE);
    CHECK(wr_frame_feed(&frame, CONTINUATION, true, 3, 2, payload + 5, 1) == WR_FRAME_READY);
    CHECK(frame.size == sizeof(payload));
    CHECK(memcmp(frame.data, payload, sizeof(payload)) == 0);

    CHECK(wr_frame_feed(&frame, BINARY, true, 6, 0, payload, 2) == WR_FRAME_MORE);
    CHECK(wr_frame_feed(&frame, BINARY, true, 6, 2, payload + 2, 4) == WR_FRAME_READY);
    CHECK(frame.size == sizeof(payload));
    CHECK(memcmp(frame.data, payload, sizeof(payload)) == 0);
}

static void test_bounds_and_recovery(void)
{
    wr_frame_t frame = {0};
    uint8_t payload[WR_FRAME_MAX + 1];
    for (size_t i = 0; i < sizeof(payload); ++i) {
        payload[i] = (uint8_t)i;
    }

    CHECK(wr_frame_feed(&frame, CONTINUATION, true, 1, 0, payload, 1) == WR_FRAME_INVALID);
    CHECK(wr_frame_feed(&frame, BINARY, true, 3, 0, payload, 1) == WR_FRAME_MORE);
    CHECK(wr_frame_feed(&frame, BINARY, true, 3, 2, payload + 2, 1) == WR_FRAME_INVALID);
    CHECK(wr_frame_feed(&frame, BINARY, true, 3, 0, payload, 1) == WR_FRAME_MORE);
    CHECK(wr_frame_feed(&frame, BINARY, false, 3, 1, payload + 1, 2) == WR_FRAME_INVALID);
    CHECK(wr_frame_feed(&frame, BINARY, false, WR_FRAME_MAX, 0, payload, WR_FRAME_MAX) == WR_FRAME_MORE);
    CHECK(wr_frame_feed(&frame, CONTINUATION, true, 1, 0, payload, 1) == WR_FRAME_INVALID);
    CHECK(wr_frame_feed(&frame, BINARY, true, sizeof(payload), 0, payload, sizeof(payload)) == WR_FRAME_INVALID);
    CHECK(wr_frame_feed(&frame, BINARY, true, WR_FRAME_MAX, 0, payload, WR_FRAME_MAX) == WR_FRAME_READY);
    CHECK(frame.size == WR_FRAME_MAX);
    CHECK(memcmp(frame.data, payload, WR_FRAME_MAX) == 0);

    CHECK(wr_frame_feed(&frame, BINARY, true, 1, 0, payload + 3, 1) == WR_FRAME_READY);
    CHECK(frame.size == 1);
    CHECK(frame.data[0] == payload[3]);
}

int main(void)
{
    test_chunked_and_fragmented();
    test_bounds_and_recovery();
    return 0;
}
