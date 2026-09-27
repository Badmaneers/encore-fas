#include "fas_client.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

/**
 * @brief Opens the FAS device node.
 *
 * @param flags Extra file open flags (e.g. O_NONBLOCK).
 * @return File descriptor on success, or -1 on error with errno set.
 */
int fas_client_open(int flags) {
    return open(FAS_DEV_PATH, O_RDWR | O_CLOEXEC | flags);
}

/**
 * @brief Reads kernel module version information.
 *
 * @param fd File descriptor of FAS device.
 * @param out Output structure for version data.
 * @return 0 on success, or -1 on error with errno set.
 */
int fas_client_get_version(int fd, struct fas_version *out) {
    return ioctl(fd, FAS_IOC_GET_VERSION, out);
}

/**
 * @brief Initializes configuration structure.
 *
 * @param cfg Configuration structure to fill.
 * @param fps Array of target frame rates.
 * @param count Element count in fps array. Clamped to FAS_MAX_TARGETS.
 * @param vsync_hz Display refresh rate in Hz, or 0 for default.
 * @param lock_down Set to non-zero to disable switching to lower rates.
 */
void fas_client_config_init(struct fas_config *cfg, const uint32_t *fps, uint32_t count, uint32_t vsync_hz, int lock_down) {
    memset(cfg, 0, sizeof(*cfg));

    if (count > FAS_MAX_TARGETS)
        count = FAS_MAX_TARGETS;

    cfg->count = count;
    for (uint32_t i = 0; i < count; i++)
        cfg->fps[i] = fps[i];

    if (vsync_hz)
        cfg->vsync_ns = (uint32_t)(1000000000u / vsync_hz);
    if (lock_down)
        cfg->flags |= FAS_CFG_LOCK_DOWN;
}

/**
 * @brief Attaches listener to target process.
 *
 * @param fd File descriptor of FAS device.
 * @param pid Process ID of target process.
 * @param path Binary file path containing target function (e.g. libgui.so).
 * @param offset File offset of target function.
 * @param cfg Initial target configuration.
 * @param out_ctx_id Optional output pointer for assigned listener ID.
 * @return 0 on success, or -1 on error with errno set.
 */
int fas_client_register(int fd, int pid, const char *path, uint64_t offset, const struct fas_config *cfg, int *out_ctx_id) {
    struct fas_register_args args;

    if (strlen(path) >= FAS_MAX_PATH_LEN) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memset(&args, 0, sizeof(args));
    args.pid = pid;
    args.offset = offset;
    args.cfg = *cfg;
    strcpy(args.path, path);

    if (ioctl(fd, FAS_IOC_REGISTER, &args) != 0)
        return -1;

    if (out_ctx_id)
        *out_ctx_id = args.ctx_id;
    return 0;
}

/**
 * @brief Detaches a listener.
 *
 * @param fd File descriptor of FAS device.
 * @param ctx_id Listener ID to detach.
 * @return 0 on success, or -1 on error with errno set.
 */
int fas_client_remove(int fd, int ctx_id) {
    struct fas_remove_args args = {.ctx_id = ctx_id};

    return ioctl(fd, FAS_IOC_REMOVE, &args);
}

/**
 * @brief Updates configuration targets for a listener.
 *
 * @param fd File descriptor of FAS device.
 * @param ctx_id Listener ID to update.
 * @param cfg New configuration targets.
 * @return 0 on success, or -1 on error with errno set.
 */
int fas_client_set_config(int fd, int ctx_id, const struct fas_config *cfg) {
    struct fas_config_args args;

    memset(&args, 0, sizeof(args));
    args.ctx_id = ctx_id;
    args.cfg = *cfg;

    return ioctl(fd, FAS_IOC_SET_CONFIG, &args);
}

/**
 * @brief Reads status information for a listener.
 *
 * Call this function to rebuild status after event sequence gaps.
 *
 * @param fd File descriptor of FAS device.
 * @param ctx_id Listener ID to query.
 * @param out Output structure for state data.
 * @return 0 on success, or -1 on error with errno set.
 */
int fas_client_get_state(int fd, int ctx_id, struct fas_state *out) {
    memset(out, 0, sizeof(*out));
    out->ctx_id = ctx_id;

    return ioctl(fd, FAS_IOC_GET_STATE, out);
}

/**
 * @brief Gets list of active listeners.
 *
 * Listeners remain attached until target process exits.
 *
 * @param fd File descriptor of FAS device.
 * @param out Output structure for listener list.
 * @return 0 on success, or -1 on error with errno set.
 */
int fas_client_list(int fd, struct fas_listener_list *out) {
    return ioctl(fd, FAS_IOC_LIST, out);
}

/**
 * @brief Reads generated events from FAS device.
 *
 * Blocks for incoming events if file descriptor is in blocking mode.
 *
 * @param fd File descriptor of FAS device.
 * @param out Output array for events.
 * @param max Maximum event count to read (up to 8 events per call).
 * @return Count of read events on success, or -1 on error with errno set.
 */
ssize_t fas_client_read_events(int fd, struct fas_event *out, size_t max) {
    ssize_t n = read(fd, out, max * sizeof(*out));

    if (n < 0)
        return -1;

    return n / (ssize_t)sizeof(*out);
}

/**
 * @brief Converts event type ID to string representation.
 *
 * @param type Event type ID from struct fas_event.
 * @return Constant string name, or "unknown" for invalid IDs.
 */
const char *fas_client_event_name(uint32_t type) {
    static const char *const names[] = {
        [FAS_EVENT_NONE] = "none",
        [FAS_EVENT_SMALL_JANK] = "small_jank",
        [FAS_EVENT_BIG_JANK] = "big_jank",
        [FAS_EVENT_BOOST_SOFT] = "boost_soft",
        [FAS_EVENT_BOOST_HARD] = "boost_hard",
        [FAS_EVENT_DEGRADED] = "degraded",
        [FAS_EVENT_RECOVERED] = "recovered",
        [FAS_EVENT_PAUSED] = "paused",
        [FAS_EVENT_RESUMED] = "resumed",
        [FAS_EVENT_RATE_SWITCH] = "rate_switch",
    };

    if (type < sizeof(names) / sizeof(names[0]) && names[type])
        return names[type];
    return "unknown";
}
