/*
 * Wii U transmitter: the GamePad's IR LED, through its IrDA port.
 *
 * The console's own sensor bar is powered by the SMC on the Wii U, not by a
 * GPIO the PowerPC can toggle, so it cannot make a carrier. The GamePad's IR
 * LED (the one its TV remote uses) is reachable through CCRCDCPerIrdaControl,
 * whose bit rates include 36000 and 38400: ir_sir.c turns the timings into
 * bytes that make the carrier at those rates.
 *
 * Copyright 2026 quatric
 * SPDX-License-Identifier: Apache-2.0
 */
#include <string.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <nsysccr/irda.h>

#include "ir_platform.h"

static const struct {
    uint32_t rate;
    CCRCDCIrdaBitrate code;
} rates[] = {
    {36000, CCR_IRDA_BITRATE_36000},
    {38400, CCR_IRDA_BITRATE_38400},
    {48000, CCR_IRDA_BITRATE_48000},
    {57600, CCR_IRDA_BITRATE_57600},
};

/* What one send carries after the request, packet header, size and crc8. */
#define CHUNK (CCR_CDC_IRDA_DATA_TRANSFER_SIZE - sizeof(CCRCDCIrdaSendRequest) - \
               sizeof(CCRCDCIrdaLargePacketHeader) - 2 - 1)

/* shortcut: one set of buffers, so IR_SendTimings is not thread-safe; call it from one thread. */
static uint8_t request[CCR_CDC_IRDA_DATA_TRANSFER_SIZE] __attribute__((aligned(0x40)));
static uint8_t reply[0x40] __attribute__((aligned(0x40)));
static uint8_t sir[4096];

static uint32_t distance(uint32_t a, uint32_t b)
{
    return a > b ? a - b : b - a;
}

static int control(uint32_t size)
{
    reply[0] = 0xFF;
    if (CCRCDCPerIrdaControl(CCR_CDC_DESTINATION_DRC0, request, size, reply, 1) != 0)
        return -1;
    return reply[0] == 0 ? 0 : -1;
}

static int send_chunk(const uint8_t *data, uint32_t size)
{
    memset(request, 0, sizeof request);
    CCRCDCIrdaSendRequest *req = (CCRCDCIrdaSendRequest *)request;
    CCRCDCIrdaLargePacketHeader *hdr = (CCRCDCIrdaLargePacketHeader *)req->data;
    hdr->magic = 0xA5;
    hdr->isLarge = 1;
    hdr->dataSize = size + 2;
    hdr->receiveSize = sizeof(CCRCDCIrdaSmallPacketHeader) + 1; /* no reply wanted */
    memcpy(hdr + 1, data, size);
    req->command = CCR_IRDA_COMMAND_SEND;
    req->size = sizeof *hdr + size + 1;
    return control(CCR_CDC_IRDA_DATA_TRANSFER_SIZE);
}

int IR_SendTimings(uint32_t frequency, float duty_cycle,
                   const uint32_t *timings, size_t count)
{
    (void)duty_cycle; /* SIR pulses are 3/16 of a slot, whatever is asked */
    if (!timings || !count)
        return -1;
    uint32_t tail = count % 2 == 0 ? timings[--count] : 0;
    if (!count)
        return -1;

    size_t best = 0;
    for (size_t i = 1; i < sizeof rates / sizeof rates[0]; i++)
        if (distance(rates[i].rate, frequency) < distance(rates[best].rate, frequency))
            best = i;
    uint32_t rate = rates[best].rate;
    if (distance(rate, frequency) > frequency / 8)
        return -1; /* no bit rate near this carrier */

    size_t n = ir_sir_encode(timings, count, rate, sir, sizeof sir);
    if (!n)
        return -1;

    memset(request, 0, sizeof request);
    CCRCDCIrdaConnectRequest *conn = (CCRCDCIrdaConnectRequest *)request;
    conn->command = CCR_IRDA_COMMAND_CONNECT;
    conn->timeout = 100;
    conn->bitrate = rates[best].code;
    conn->receiveSize = sizeof(CCRCDCIrdaSmallPacketHeader) + 1;
    conn->targetId = 0;
    conn->type = CCR_IRDA_CONNECTION_ANY;
    if (control(sizeof *conn))
        return -2;

    int ret = 0;
    for (size_t at = 0; at < n && !ret; at += CHUNK)
        ret = send_chunk(sir + at, n - at < CHUNK ? n - at : CHUNK);

    memset(request, 0, sizeof request);
    ((CCRCDCIrdaDisconnectRequest *)request)->command = CCR_IRDA_COMMAND_DISCONNECT;
    control(sizeof(CCRCDCIrdaDisconnectRequest));

    /* the GamePad sends on its own time: wait for the air time and the gap */
    OSSleepTicks(OSMicrosecondsToTicks((uint64_t)n * 10 * 1000000 / rate + tail));
    return ret ? -3 : 0;
}
