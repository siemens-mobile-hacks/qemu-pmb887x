/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/error-report.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qom/object_interfaces.h"
#include "hw/gsm/gsm-air.h"
#include "hw/gsm/gsm-network.h"

#define TYPE_GSM_OSMOTRX "gsm-osmotrx"
#define GSM_OSMOTRX_DEFAULT_ADDRESS "127.0.0.1"
#define GSM_OSMOTRX_DEFAULT_BASE_PORT 5700
#define GSM_OSMOTRX_BTS_PORT_OFFSET 100
#define GSM_OSMOTRX_MAX_BASE_PORT (UINT16_MAX - GSM_OSMOTRX_BTS_PORT_OFFSET - GSM_OSMOTRX_SOCKET_COUNT + 1)
#define GSM_OSMOTRX_CLOCK_INDICATION_FRAMES 216
#define GSM_OSMOTRX_DOWNLINK_FRAMES 64
#define GSM_OSMOTRX_TRXD_V0_DOWNLINK_HEADER 6
#define GSM_OSMOTRX_TRXD_V0_UPLINK_HEADER 8
#define GSM_OSMOTRX_TRXD_V0_UPLINK_PADDING 2
#define GSM_OSMOTRX_UPLINK_RSSI_MINUS_DBM 60
#define GSM_OSMOTRX_SOFT_BIT_THRESHOLD 128
#define GSM_OSMOTRX_TOA_UNITS_PER_SYMBOL 256
#define GSM_ACCESS_BURST_BITS 88
#define GSM_ACCESS_SYNC_OFFSET 8
#define GSM_ACCESS_SYNC_MIN_MATCHES 37
#define GSM_NORMAL_TSC_OFFSET 61
#define GSM_NORMAL_TSC_MIN_MATCHES 23
#define GSM_BSIC_BCC_MASK 0x07
#define GSM_OSMOTRX_NOMINAL_TX_POWER_DBM "23"
#define GSM_OSMOTRX_TRXD_VERSION "0"
#define GSM_OSMOTRX_COMMAND_MAX 256
#define GSM_OSMOTRX_MUX_HELLO 'H'
#define GSM_OSMOTRX_MUX_CONFIG 'C'
#define GSM_OSMOTRX_MUX_DOWNLINK 'D'
#define GSM_OSMOTRX_MUX_UPLINK 'U'
#define GSM_OSMOTRX_MUX_RESYNC_FRAMES (2 * GSM_OSMOTRX_CLOCK_INDICATION_FRAMES)

OBJECT_DECLARE_SIMPLE_TYPE(GSMOsmoTrx, GSM_OSMOTRX)

/* GSM 05.02 5.2.5 and 5.2.7 synchronisation and training sequences. */
static const char GSM_ACCESS_SYNC[] = "01001011011111111001100110101010001111000";
static const char *const GSM_NORMAL_TSC[] = {
    "00100101110000100010010111",
    "00101101110111100010110111",
    "01000011101110100100001110",
    "01000111101101000100011110",
    "00011010111001000001101011",
    "01001110101100000100111010",
    "10100111110110001010011111",
    "11101111000100101110111100",
};

typedef enum GSMOsmoTrxSocket GSMOsmoTrxSocket;
typedef struct GSMOsmoTrxBurst GSMOsmoTrxBurst;
typedef struct GSMOsmoTrxMuxConfig GSMOsmoTrxMuxConfig;

/* Big-endian body of a multiplexer configuration message. */
struct QEMU_PACKED GSMOsmoTrxMuxConfig {
    uint32_t frame_number;
    uint32_t downlink_khz;
    uint32_t uplink_khz;
    uint8_t tsc;
    uint8_t powered;
};

_Static_assert(sizeof(GSMOsmoTrxMuxConfig) == 14, "multiplexer configuration layout");

/* Port offsets from base-port, as used by osmo-trx. */
enum GSMOsmoTrxSocket {
    GSM_OSMOTRX_CLOCK,
    GSM_OSMOTRX_CONTROL,
    GSM_OSMOTRX_DATA,
    GSM_OSMOTRX_SOCKET_COUNT,
};

struct GSMOsmoTrxBurst {
    uint32_t frame_number;
    bool present;
    GSMBurst burst;
};

struct GSMOsmoTrx {
    Object parent_obj;
    char *address;
    uint16_t base_port;
    uint16_t mux_port;
    uint32_t mux_frame_offset;
    bool mux_synced;
    int fds[GSM_OSMOTRX_SOCKET_COUNT];
    QEMUTimer *clock_timer;
    uint16_t downlink_arfcn;
    uint16_t uplink_arfcn;
    uint8_t tsc;
    bool powered;
    GSMOsmoTrxBurst downlink[GSM_OSMOTRX_DOWNLINK_FRAMES][GSM_TIMESLOTS];
};

static void gsm_osmotrx_send_text(GSMOsmoTrx *trx, GSMOsmoTrxSocket socket, const char *text) {
    if (send(trx->fds[socket], text, strlen(text) + 1, 0) < 0)
        warn_report("gsm-osmotrx: send failed: %s", strerror(errno));
}

static void gsm_osmotrx_send_data(GSMOsmoTrx *trx, const void *data, size_t size) {
    if (send(trx->fds[GSM_OSMOTRX_DATA], data, size, 0) < 0)
        warn_report("gsm-osmotrx: send failed: %s", strerror(errno));
}

static void gsm_osmotrx_tick(void *opaque) {
    GSMOsmoTrx *trx = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (trx->mux_port != 0) {
        uint8_t hello = GSM_OSMOTRX_MUX_HELLO;
        gsm_osmotrx_send_data(trx, &hello, sizeof(hello));
    } else {
        g_autofree char *indication = g_strdup_printf("IND CLOCK %u", gsm_air_frame_number(now));
        gsm_osmotrx_send_text(trx, GSM_OSMOTRX_CLOCK, indication);
    }
    timer_mod(trx->clock_timer, now + gsm_air_frames_to_ns(GSM_OSMOTRX_CLOCK_INDICATION_FRAMES));
}

static void gsm_osmotrx_power(GSMOsmoTrx *trx, bool powered) {
    trx->powered = powered;
    memset(trx->downlink, 0, sizeof(trx->downlink));
    if (powered) {
        gsm_osmotrx_tick(trx);
    } else {
        timer_del(trx->clock_timer);
    }
}

static int gsm_osmotrx_tune(
    const char *frequency_khz,
    bool (*frequency_to_arfcn)(uint32_t frequency_hz, uint16_t *arfcn),
    uint16_t *arfcn
) {
    uint64_t frequency_hz = g_ascii_strtoull(frequency_khz, NULL, 10) * 1000;

    if (frequency_hz > UINT32_MAX)
        return 1;
    if (!frequency_to_arfcn(frequency_hz, arfcn))
        return 1;
    return 0;
}

static void gsm_osmotrx_handle_command(GSMOsmoTrx *trx, char *message) {
    if (!g_str_has_prefix(message, "CMD "))
        return;

    char *name = message + strlen("CMD ");
    char *separator = strchr(name, ' ');
    const char *params = "";
    if (separator != NULL) {
        *separator = '\0';
        params = separator + 1;
    }

    int status = 0;
    const char *reply_params = params;
    if (strcmp(name, "POWERON") == 0) {
        gsm_osmotrx_power(trx, true);
    } else if (strcmp(name, "POWEROFF") == 0) {
        gsm_osmotrx_power(trx, false);
    } else if (strcmp(name, "TXTUNE") == 0) {
        status = gsm_osmotrx_tune(params, gsm_downlink_frequency_to_arfcn, &trx->downlink_arfcn);
    } else if (strcmp(name, "SETTSC") == 0 || strcmp(name, "SETBSIC") == 0) {
        trx->tsc = g_ascii_strtoull(params, NULL, 10) & GSM_BSIC_BCC_MASK;
    } else if (strcmp(name, "RXTUNE") == 0) {
        status = gsm_osmotrx_tune(params, gsm_uplink_frequency_to_arfcn, &trx->uplink_arfcn);
    } else if (strcmp(name, "SETFORMAT") == 0) {
        reply_params = GSM_OSMOTRX_TRXD_VERSION;
    } else if (strcmp(name, "NOMTXPOWER") == 0) {
        reply_params = GSM_OSMOTRX_NOMINAL_TX_POWER_DBM;
    }

    g_autofree char *reply = reply_params[0] != '\0' ?
        g_strdup_printf("RSP %s %d %s", name, status, reply_params) :
        g_strdup_printf("RSP %s %d", name, status);
    gsm_osmotrx_send_text(trx, GSM_OSMOTRX_CONTROL, reply);
}

static void gsm_osmotrx_control_read(void *opaque) {
    GSMOsmoTrx *trx = opaque;
    char message[GSM_OSMOTRX_COMMAND_MAX + 1];
    ssize_t length = recv(trx->fds[GSM_OSMOTRX_CONTROL], message, GSM_OSMOTRX_COMMAND_MAX, 0);

    if (length <= 0)
        return;

    message[length] = '\0';
    gsm_osmotrx_handle_command(trx, message);
}

static void gsm_osmotrx_store_downlink(GSMOsmoTrx *trx, const uint8_t *pdu, size_t length) {
    if (length != GSM_OSMOTRX_TRXD_V0_DOWNLINK_HEADER + GSM_BURST_BITS)
        return;
    if ((pdu[0] >> 4) != 0)
        return;

    uint8_t timeslot = pdu[0] & 0x07;
    uint32_t network_frame_number = ldl_be_p(&pdu[1]);
    if (network_frame_number >= GSM_HYPERFRAME_FRAMES)
        return;

    uint32_t frame_number = (network_frame_number + GSM_HYPERFRAME_FRAMES - trx->mux_frame_offset) %
        GSM_HYPERFRAME_FRAMES;
    GSMOsmoTrxBurst *slot = &trx->downlink[frame_number % GSM_OSMOTRX_DOWNLINK_FRAMES][timeslot];
    for (size_t bit = 0; bit < GSM_BURST_BITS; bit++)
        slot->burst.bits[bit] = pdu[GSM_OSMOTRX_TRXD_V0_DOWNLINK_HEADER + bit] != 0;
    slot->frame_number = frame_number;
    slot->present = true;
}

static void gsm_osmotrx_data_read(void *opaque) {
    GSMOsmoTrx *trx = opaque;
    uint8_t pdu[GSM_OSMOTRX_TRXD_V0_DOWNLINK_HEADER + GSM_BURST_BITS];
    ssize_t length = recv(trx->fds[GSM_OSMOTRX_DATA], pdu, sizeof(pdu), 0);

    if (length > 0)
        gsm_osmotrx_store_downlink(trx, pdu, length);
}

static void gsm_osmotrx_sync_mux_frames(GSMOsmoTrx *trx, uint32_t network_frame_number) {
    uint32_t frame_number = gsm_air_frame_number(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    uint32_t offset = (network_frame_number + GSM_HYPERFRAME_FRAMES - frame_number) % GSM_HYPERFRAME_FRAMES;
    uint32_t drift = (offset + GSM_HYPERFRAME_FRAMES - trx->mux_frame_offset) % GSM_HYPERFRAME_FRAMES;

    if (trx->mux_synced && MIN(drift, GSM_HYPERFRAME_FRAMES - drift) <= GSM_OSMOTRX_MUX_RESYNC_FRAMES)
        return;

    trx->mux_frame_offset = offset;
    trx->mux_synced = true;
    memset(trx->downlink, 0, sizeof(trx->downlink));
}

static void gsm_osmotrx_handle_mux_config(GSMOsmoTrx *trx, const GSMOsmoTrxMuxConfig *config) {
    uint32_t downlink_hz = ldl_be_p(&config->downlink_khz) * 1000;
    uint32_t uplink_hz = ldl_be_p(&config->uplink_khz) * 1000;
    bool powered = config->powered != 0;

    gsm_osmotrx_sync_mux_frames(trx, ldl_be_p(&config->frame_number));
    gsm_downlink_frequency_to_arfcn(downlink_hz, &trx->downlink_arfcn);
    gsm_uplink_frequency_to_arfcn(uplink_hz, &trx->uplink_arfcn);
    trx->tsc = config->tsc & GSM_BSIC_BCC_MASK;
    if (trx->powered != powered) {
        trx->powered = powered;
        memset(trx->downlink, 0, sizeof(trx->downlink));
    }
}

static void gsm_osmotrx_mux_read(void *opaque) {
    GSMOsmoTrx *trx = opaque;
    uint8_t message[1 + GSM_OSMOTRX_TRXD_V0_DOWNLINK_HEADER + GSM_BURST_BITS];
    ssize_t length = recv(trx->fds[GSM_OSMOTRX_DATA], message, sizeof(message), 0);

    if (length <= 0)
        return;

    if (message[0] == GSM_OSMOTRX_MUX_CONFIG && length == 1 + sizeof(GSMOsmoTrxMuxConfig)) {
        gsm_osmotrx_handle_mux_config(trx, (const GSMOsmoTrxMuxConfig *) &message[1]);
    } else if (message[0] == GSM_OSMOTRX_MUX_DOWNLINK && trx->mux_synced) {
        gsm_osmotrx_store_downlink(trx, &message[1], length - 1);
    }
}

static bool gsm_osmotrx_downlink_burst(
    GSMNetwork *network,
    uint16_t arfcn,
    uint32_t frame_number,
    uint8_t timeslot,
    GSMBurst *burst
) {
    GSMOsmoTrx *trx = GSM_OSMOTRX(network);
    const GSMOsmoTrxBurst *slot = &trx->downlink[frame_number % GSM_OSMOTRX_DOWNLINK_FRAMES][timeslot];

    if (!trx->powered || arfcn != trx->downlink_arfcn)
        return false;
    if (!slot->present || slot->frame_number != frame_number)
        return false;

    *burst = slot->burst;
    return true;
}

static size_t gsm_osmotrx_count_matches(const uint8_t *soft_bits, const char *sequence) {
    size_t matches = 0;

    for (size_t i = 0; sequence[i] != '\0'; i++) {
        if (soft_bits[i] == GSM_SOFT_BIT_UNKNOWN)
            continue;
        if ((soft_bits[i] >= GSM_OSMOTRX_SOFT_BIT_THRESHOLD) == (sequence[i] == '1'))
            matches++;
    }
    return matches;
}

static bool gsm_osmotrx_find_burst_start(
    const GSMUplinkBurst *burst,
    const char *sequence,
    size_t sequence_offset,
    size_t min_matches,
    size_t *burst_start
) {
    size_t length = strlen(sequence);
    size_t best_matches = 0;

    for (size_t position = sequence_offset; position + length <= GSM_UPLINK_WINDOW_SYMBOLS; position++) {
        size_t matches = gsm_osmotrx_count_matches(&burst->soft_bits[position], sequence);

        if (matches > best_matches) {
            best_matches = matches;
            *burst_start = position - sequence_offset;
        }
    }
    return best_matches >= min_matches;
}

static void gsm_osmotrx_uplink_burst(
    GSMNetwork *network,
    uint16_t arfcn,
    uint32_t frame_number,
    uint8_t timeslot,
    const GSMUplinkBurst *burst
) {
    GSMOsmoTrx *trx = GSM_OSMOTRX(network);
    uint8_t message[1 + GSM_OSMOTRX_TRXD_V0_UPLINK_HEADER + GSM_BURST_BITS + GSM_OSMOTRX_TRXD_V0_UPLINK_PADDING] = {
        GSM_OSMOTRX_MUX_UPLINK
    };
    uint8_t *pdu = &message[1];
    uint8_t *soft_bits = &pdu[GSM_OSMOTRX_TRXD_V0_UPLINK_HEADER];
    uint32_t network_frame_number = (frame_number + trx->mux_frame_offset) % GSM_HYPERFRAME_FRAMES;
    size_t start;
    size_t length;

    if (!trx->powered || arfcn != trx->uplink_arfcn)
        return;

    if (gsm_osmotrx_find_burst_start(burst, GSM_ACCESS_SYNC, GSM_ACCESS_SYNC_OFFSET, GSM_ACCESS_SYNC_MIN_MATCHES,
            &start)) {
        length = GSM_ACCESS_BURST_BITS;
    } else if (gsm_osmotrx_find_burst_start(burst, GSM_NORMAL_TSC[trx->tsc], GSM_NORMAL_TSC_OFFSET,
            GSM_NORMAL_TSC_MIN_MATCHES, &start)) {
        length = GSM_BURST_BITS;
    } else {
        return;
    }

    int toa_256 = ((int) start - GSM_UPLINK_LEAD_SYMBOLS) * GSM_OSMOTRX_TOA_UNITS_PER_SYMBOL + burst->phase_256;
    size_t received = MIN(length, GSM_UPLINK_WINDOW_SYMBOLS - start);

    pdu[0] = timeslot;
    stl_be_p(&pdu[1], network_frame_number);
    pdu[5] = GSM_OSMOTRX_UPLINK_RSSI_MINUS_DBM;
    stw_be_p(&pdu[6], toa_256);
    memset(soft_bits, GSM_SOFT_BIT_UNKNOWN, GSM_BURST_BITS);
    memcpy(soft_bits, &burst->soft_bits[start], received);

    if (trx->mux_port != 0) {
        gsm_osmotrx_send_data(trx, message, sizeof(message));
    } else {
        gsm_osmotrx_send_data(trx, pdu, sizeof(message) - 1);
    }
}

static int gsm_osmotrx_open_socket(GSMOsmoTrx *trx, uint16_t local_port, uint16_t remote_port, Error **errp) {
    struct sockaddr_in local = {
        .sin_family = AF_INET,
        .sin_port = htons(local_port),
    };

    if (inet_pton(AF_INET, trx->address, &local.sin_addr) != 1) {
        error_setg(errp, "gsm-osmotrx: invalid IPv4 address '%s'", trx->address);
        return -1;
    }

    struct sockaddr_in remote = local;
    remote.sin_port = htons(remote_port);

    int fd = qemu_socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        error_setg_errno(errp, errno, "gsm-osmotrx: cannot create a socket");
        return -1;
    }
    if (bind(fd, (struct sockaddr *) &local, sizeof(local)) < 0) {
        error_setg_errno(errp, errno, "gsm-osmotrx: cannot bind %s:%u", trx->address, local_port);
        close(fd);
        return -1;
    }
    if (connect(fd, (struct sockaddr *) &remote, sizeof(remote)) < 0) {
        error_setg_errno(errp, errno, "gsm-osmotrx: cannot connect to %s:%u", trx->address, remote_port);
        close(fd);
        return -1;
    }
    if (!qemu_set_blocking(fd, false, errp)) {
        close(fd);
        return -1;
    }
    return fd;
}

static void gsm_osmotrx_close_sockets(GSMOsmoTrx *trx) {
    for (size_t i = 0; i < GSM_OSMOTRX_SOCKET_COUNT; i++) {
        if (trx->fds[i] < 0)
            continue;
        qemu_set_fd_handler(trx->fds[i], NULL, NULL, NULL);
        close(trx->fds[i]);
        trx->fds[i] = -1;
    }
}

static void gsm_osmotrx_complete(UserCreatable *creatable, Error **errp) {
    GSMOsmoTrx *trx = GSM_OSMOTRX(creatable);

    if (trx->base_port > GSM_OSMOTRX_MAX_BASE_PORT) {
        error_setg(errp, "gsm-osmotrx: base-port must not exceed %u", GSM_OSMOTRX_MAX_BASE_PORT);
        return;
    }

    if (trx->mux_port != 0) {
        trx->fds[GSM_OSMOTRX_DATA] = gsm_osmotrx_open_socket(trx, 0, trx->mux_port, errp);
        if (trx->fds[GSM_OSMOTRX_DATA] < 0)
            return;

        qemu_set_fd_handler(trx->fds[GSM_OSMOTRX_DATA], gsm_osmotrx_mux_read, NULL, trx);
        gsm_osmotrx_tick(trx);
        return;
    }

    for (size_t i = 0; i < GSM_OSMOTRX_SOCKET_COUNT; i++) {
        uint16_t port = trx->base_port + i;

        trx->fds[i] = gsm_osmotrx_open_socket(trx, port, port + GSM_OSMOTRX_BTS_PORT_OFFSET, errp);
        if (trx->fds[i] < 0) {
            gsm_osmotrx_close_sockets(trx);
            return;
        }
    }

    qemu_set_fd_handler(trx->fds[GSM_OSMOTRX_CONTROL], gsm_osmotrx_control_read, NULL, trx);
    qemu_set_fd_handler(trx->fds[GSM_OSMOTRX_DATA], gsm_osmotrx_data_read, NULL, trx);
}

static char *gsm_osmotrx_get_address(Object *obj, Error **_errp) {
    return g_strdup(GSM_OSMOTRX(obj)->address);
}

static void gsm_osmotrx_set_address(Object *obj, const char *value, Error **_errp) {
    GSMOsmoTrx *trx = GSM_OSMOTRX(obj);

    g_free(trx->address);
    trx->address = g_strdup(value);
}

static void gsm_osmotrx_init(Object *obj) {
    GSMOsmoTrx *trx = GSM_OSMOTRX(obj);

    trx->address = g_strdup(GSM_OSMOTRX_DEFAULT_ADDRESS);
    trx->base_port = GSM_OSMOTRX_DEFAULT_BASE_PORT;
    for (size_t i = 0; i < GSM_OSMOTRX_SOCKET_COUNT; i++)
        trx->fds[i] = -1;
    trx->clock_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, gsm_osmotrx_tick, trx);
    object_property_add_uint16_ptr(obj, "base-port", &trx->base_port, OBJ_PROP_FLAG_READWRITE);
    object_property_add_uint16_ptr(obj, "mux-port", &trx->mux_port, OBJ_PROP_FLAG_READWRITE);
}

static void gsm_osmotrx_finalize(Object *obj) {
    GSMOsmoTrx *trx = GSM_OSMOTRX(obj);

    gsm_osmotrx_close_sockets(trx);
    timer_free(trx->clock_timer);
    g_free(trx->address);
}

static void gsm_osmotrx_class_init(ObjectClass *klass, const void *_data) {
    GSMNetworkClass *network_class = GSM_NETWORK_CLASS(klass);
    UserCreatableClass *creatable_class = USER_CREATABLE_CLASS(klass);

    network_class->downlink_burst = gsm_osmotrx_downlink_burst;
    network_class->uplink_burst = gsm_osmotrx_uplink_burst;
    creatable_class->complete = gsm_osmotrx_complete;
    object_class_property_add_str(klass, "address", gsm_osmotrx_get_address, gsm_osmotrx_set_address);
}

static const TypeInfo gsm_osmotrx_info = {
    .name               = TYPE_GSM_OSMOTRX,
    .parent             = TYPE_OBJECT,
    .instance_size      = sizeof(GSMOsmoTrx),
    .instance_init      = gsm_osmotrx_init,
    .instance_finalize  = gsm_osmotrx_finalize,
    .class_init         = gsm_osmotrx_class_init,
    .interfaces         = (const InterfaceInfo[]) {
        { TYPE_GSM_NETWORK },
        { TYPE_USER_CREATABLE },
        { NULL },
    },
};

static void gsm_osmotrx_register_types(void) {
    type_register_static(&gsm_osmotrx_info);
}

type_init(gsm_osmotrx_register_types)
