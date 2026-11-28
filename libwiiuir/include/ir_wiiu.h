/*
 * libwiiuir - send infrared remote codes from the Wii U GamePad.
 *
 * Based on WiiIR by Dakota Thorpe, used with permission.
 * Copyright 2025 Dakota Thorpe
 * Copyright 2026 quatric
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef IR_WIIU_H
#define IR_WIIU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The protocols of Flipper Zero's infrared app, so Flipper-IRDB files load as
 * they are. Address and command mean what they mean in a Flipper .ir file. */
typedef enum {
    IR_PROTO_NEC = 0,  /* 8-bit address, 8-bit command */
    IR_PROTO_NECEXT,   /* 16-bit address, 16-bit command */
    IR_PROTO_NEC42,    /* 13-bit address, 8-bit command */
    IR_PROTO_NEC42EXT, /* 26-bit address, 16-bit command */
    IR_PROTO_SAMSUNG32,
    IR_PROTO_RC5,      /* 5-bit address, 6-bit command */
    IR_PROTO_RC5X,     /* RC5 commands 64-127, stored as Flipper does (minus 64) */
    IR_PROTO_RC6,      /* mode 0: 8-bit address, 8-bit command */
    IR_PROTO_SIRC,     /* Sony 12-bit: 5-bit address, 7-bit command */
    IR_PROTO_SIRC15,   /* 8-bit address */
    IR_PROTO_SIRC20,   /* 13-bit address */
    IR_PROTO_KASEIKYO, /* Panasonic and others: 26-bit address, 10-bit command */
    IR_PROTO_RCA,      /* 4-bit address, 8-bit command */
    IR_PROTO_PIONEER,  /* 8-bit address, 8-bit command */
    IR_PROTO_JVC,      /* 8-bit address, 8-bit command (not in Flipper) */
    IR_PROTO_RAW,      /* timings, see ir_signal */
    IR_PROTO_COUNT
} ir_protocol;

typedef struct {
    char name[32];          /* button name, e.g. "Power" */
    ir_protocol protocol;
    uint32_t address;       /* parsed protocols */
    uint32_t command;
    uint32_t frequency;     /* IR_PROTO_RAW: carrier in Hz (0 = 38000) */
    float duty_cycle;       /* IR_PROTO_RAW: 0..1 (0 = 0.33) */
    uint32_t *timings;      /* IR_PROTO_RAW: mark, space, mark... in microseconds */
    size_t count;
} ir_signal;

/* Every button of one .ir file. */
typedef struct {
    ir_signal *signals;
    size_t count;
} ir_remote;

/* Longest timing list IR_Encode() produces for a parsed protocol. */
#define IR_MAX_TIMINGS 256

/* Protocol names as Flipper writes them ("NECext", "Samsung32"...). */
const char *IR_ProtocolName(ir_protocol protocol);
/* -1 when unknown. Case-insensitive. */
int IR_ProtocolFromName(const char *name);

/* Turns a signal into mark/space timings in microseconds, starting with a mark
 * and ending with the gap a remote leaves before the next press.
 * Returns how many timings were written, 0 when the signal is invalid or does
 * not fit. RC5/RC6 flip their toggle bit on every call, like a real remote. */
size_t IR_Encode(const ir_signal *signal, uint32_t *out, size_t max,
                 uint32_t *frequency, float *duty_cycle);

/* Pronto hex (learned 0000 codes) to timings. Returns the count, 0 on error. */
size_t IR_ProntoToTimings(const uint16_t *pronto, size_t length,
                          uint32_t *out, size_t max, uint32_t *frequency);

/* Flipper .ir files ("Filetype: IR signals file"), parsed or raw signals.
 * Returns 0, or -1 when the file cannot be read or holds no signal. */
int IR_LoadRemote(const char *path, ir_remote *remote);
int IR_ParseRemote(const char *text, ir_remote *remote);
void IR_FreeRemote(ir_remote *remote);
/* Case-insensitive name lookup; NULL when absent. */
const ir_signal *IR_FindSignal(const ir_remote *remote, const char *name);

/* ---- irdb.xml: remotes by manufacturer and category ----
 * WiiIR's schema, with Flipper's full address and command and raw captures
 * kept in microseconds. tools/irdb_xml.c writes it from Flipper-IRDB; more
 * devices and buttons can be added by hand:
 *
 * <Manufacturers>
 *   <Manufacturer name="Samsung"><DeviceList>
 *     <DeviceEntry name="UE40" category="TVs">
 *       <ButtonEntry name="Power"><Data>NECext:0x7F00,0xEA15</Data></ButtonEntry>
 *       <ButtonEntry name="Vol_up"><Data frequency="38000" duty="0.33">9000 4500 560 ...</Data></ButtonEntry>
 *       <ButtonEntry name="Mute"><Data>RAW:0000 006C 0022 0002 ...</Data></ButtonEntry>  (Pronto)
 *
 * Opening a file only indexes its devices; IR_DbLoad reads one at a time. */
typedef struct {
    const char *manufacturer, *category, *name;
    uint32_t buttons;
    int file;               /* which IR_DbAdd file */
    long start, end;        /* where its <DeviceEntry> is in that file */
} ir_db_device;

#define IR_DB_MAX_FILES 4
typedef struct {
    char *files[IR_DB_MAX_FILES];
    int file_count;
    ir_db_device *devices;
    size_t count;
    char **owned;           /* every string above, freed by IR_DbFree */
    size_t owned_count;
} ir_db;

/* Adds a file's devices to `db` (zero it first). Call again for, say, a
 * custom file next to the shipped one. Returns 0, or -1 when it cannot be
 * read or holds no device. */
int IR_DbAdd(ir_db *db, const char *path);
void IR_DbFree(ir_db *db);
/* Reads device `index`'s buttons. Returns 0 or -1. */
int IR_DbLoad(const ir_db *db, size_t index, ir_remote *remote);
/* The first device with these names (case-insensitive), or -1. */
int IR_DbFind(const ir_db *db, const char *category, const char *manufacturer, const char *name);

/* Transmit. All return 0 on success, negative on error, and block until the
 * code (and its trailing gap) has been sent. */
int IR_Send(const ir_signal *signal);
/* The same code again while its button is held: RC5 and RC6 keep the toggle
 * bit of the last send, so the device sees one long press. */
int IR_SendRepeat(const ir_signal *signal);
int IR_SendCode(ir_protocol protocol, uint32_t address, uint32_t command);
int IR_SendPronto(const uint16_t *pronto, size_t length);
int IR_SendTimings(uint32_t frequency, float duty_cycle,
                   const uint32_t *timings, size_t count);

#ifdef __cplusplus
}
#endif

#endif
