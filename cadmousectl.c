/* Copyright © 2015, Martin Herkt <lachs0r@srsfckn.biz>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or  without fee is hereby granted,  provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH  REGARD  TO  THIS  SOFTWARE  INCLUDING  ALL  IMPLIED  WARRANTIES OF
 * MERCHANTABILITY AND FITNESS.  IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL,  DIRECT,  INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING  FROM LOSS OF USE,  DATA OR PROFITS,  WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/hidraw.h>

#include <hidapi.h>

static int bt_fd = -1;
static unsigned short cadmouse_pid = 0;   /* product id of the opened device */

static int is_hidraw_bluetooth(const char *hidraw_name)
{
    char path[512];
    char resolved[512];

    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device", hidraw_name);
    if (realpath(path, resolved) == NULL)
        return 0;

    return strstr(resolved, "uhid/") != NULL;
}

/* Return 1 if the hidraw node `name` (e.g. "hidraw1") belongs to a 3Dconnexion
 * CadMouse (vendor 256f, one of the known product ids). Shared by the directory
 * walks below so the uevent matching lives in one place. */
static int hidraw_is_cadmouse(const char *name)
{
    char path[512];
    snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", name);

    FILE *f = fopen(path, "r");
    if (!f)
        return 0;

    int match = 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (strstr(line, "256F") &&
            (strstr(line, "C654") || strstr(line, "C656") || strstr(line, "C650"))) {
            match = 1;
            break;
        }
    }
    fclose(f);
    return match;
}

static int find_hidraw_for_3dconnexion(char *name_buf, int buf_len)
{
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir)
        return -1;

    int found = -1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0)
            continue;
        if (!hidraw_is_cadmouse(ent->d_name))
            continue;

        if (name_buf && buf_len > 0) {
            strncpy(name_buf, ent->d_name, buf_len - 1);
            name_buf[buf_len - 1] = '\0';
        }
        found = 0;
        break;
    }

    closedir(dir);
    return found;
}

static int bt_send_config_report(int fd, unsigned char *report, int len)
{
    return ioctl(fd, HIDIOCSFEATURE(len), report);
}

/* Return 1 if the open hidraw fd's report descriptor declares report id
 * want_id. Used to pick the right interface: a device may expose several
 * hidraw nodes and only one carries the Pro config report (id 0x10). */
static int hidraw_has_report_id(int fd, unsigned char want_id)
{
    int size = 0;
    struct hidraw_report_descriptor desc;

    if (ioctl(fd, HIDIOCGRDESCSIZE, &size) < 0)
        return 0;
    memset(&desc, 0, sizeof(desc));
    desc.size = size;
    if (ioctl(fd, HIDIOCGRDESC, &desc) < 0)
        return 0;

    /* Walk the HID short-item stream looking for a Report ID item (0x85). */
    for (unsigned int i = 0; i < desc.size; ) {
        unsigned char b = desc.value[i];
        if (b == 0xfe) {            /* long item: 0xfe, dataSize, tag, data */
            if (i + 1 >= desc.size)
                break;
            i += 3 + desc.value[i + 1];
            continue;
        }
        unsigned char bsize = b & 0x03;
        if (bsize == 3)
            bsize = 4;
        if ((b & 0xfc) == 0x84 && bsize >= 1 &&   /* 0x85 = Report ID (global) */
            i + 1 < desc.size && desc.value[i + 1] == want_id)
            return 1;
        i += 1 + bsize;
    }
    return 0;
}

/* Open the Bluetooth CadMouse hidraw interface that accepts the 32-byte config
 * report (id 0x10). The mouse exposes several hidraw nodes over Bluetooth and
 * only one handles the feature report; the order is not reliable, so the
 * interfaces are probed via their report descriptors. Falls back to the first
 * Bluetooth node if none advertises the config report. Returns an O_RDWR fd,
 * or -1 (e.g. for USB, where the hidapi handle is used instead). */
static int open_bt_config_hidraw(void)
{
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir)
        return -1;

    int chosen = -1, fallback = -1;
    char chosen_path[128] = {0}, fallback_path[128] = {0};
    struct dirent *ent;

    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0)
            continue;
        if (!hidraw_is_cadmouse(ent->d_name) || !is_hidraw_bluetooth(ent->d_name))
            continue;

        char dev_path[128];
        snprintf(dev_path, sizeof(dev_path), "/dev/%s", ent->d_name);
        int fd = open(dev_path, O_RDWR);
        if (fd < 0)
            continue;

        if (hidraw_has_report_id(fd, 0x10)) {
            chosen = fd;
            snprintf(chosen_path, sizeof(chosen_path), "%s", dev_path);
            break;
        }
        if (fallback < 0) {
            fallback = fd;
            snprintf(fallback_path, sizeof(fallback_path), "%s", dev_path);
        } else {
            close(fd);
        }
    }
    closedir(dir);

    if (chosen >= 0) {
        if (fallback >= 0)
            close(fallback);
        fprintf(stderr, "Connected via Bluetooth (%s)\n", chosen_path);
        return chosen;
    }
    if (fallback >= 0)
        fprintf(stderr, "Connected via Bluetooth (%s; no config report advertised)\n",
                fallback_path);
    return fallback;
}

int cadmouse_send_command(hid_device *mouse, int opt, int val1, int val2)
{
    unsigned char cmd[8] = { 0x0c, opt, val1, val2, 0x00, 0x00, 0x00, 0x00 };
    return hid_send_feature_report(mouse, cmd, 8);
}

int cadmouse_set_smartscroll(hid_device *mouse, int state)
{
    int result;

    if (state == 1 || state == 3)
        result = cadmouse_send_command(mouse, 0x03, 0x00, 0x00);
    else
        result = cadmouse_send_command(mouse, 0x03, 0x00, 0x01);

    if (result < 0)
        return result;

    if (state == 3)
        result = cadmouse_send_command(mouse, 0x04, 0xff, 0x00);
    else
        result = cadmouse_send_command(mouse, 0x04, state ? 0x00 : 0xff, 0x00);

    if (result < 0)
        return result;

    result = cadmouse_send_command(mouse, 0x05, 0x00, state ? 0x01 : 0x00);

    return result;
}

/* Pro mice (0xc656, 0xc654) use a single 32-byte config report (ReportID 0x10)
 * for ALL settings, and the device offers no readable "current config", so every
 * write appears to have to carry the complete configuration. To stop one option from
 * resetting the others, the report is built once with defaults
 * (cadmouse_pro_default_config), each requested option is applied to it in place
 * (cadmouse_pro_apply), and it is written to the device a single time
 * (cadmouse_pro_send). */

/* Map a hw button id (0x0a-0x10) to its byte offset in the config report. */
static const int cadmouse_pro_btn_pos[] = {
    19,   /* 0x0a left */
    20,   /* 0x0b right */
    21,   /* 0x0c middle */
    22,   /* 0x0d wheel */
    23,   /* 0x0e forward */
    24,   /* 0x0f backward */
    27,   /* 0x10 rm */
};

/* Fill report[] (32 bytes) with the device's factory-default configuration. */
static void cadmouse_pro_default_config(unsigned char *report)
{
    memset(report, 0, 32);
    report[0]  = 0x10;     /* ReportID */
    report[2]  = 0x1c;     /* pointer speed default */
    report[17] = 0x03;     /* button mapping count */
    report[19] = 0x0a;     /* left */
    report[20] = 0x0b;     /* right */
    report[21] = 0x0c;     /* middle */
    report[22] = 0x0c;     /* wheel */
    /* Forward/backward defaults differ per model (verified against USB captures
     * of the official driver). Buttons left at 0x00 remain at firmware default */
    report[23] = 0x00;     /* extra (side forward): firmware default */
    report[24] = 0x00;     /* side (side back):     firmware default */
    report[25] = 0x00;     /* scroll button:        firmware default */
    report[27] = 0x00;     /* radial menu:          firmware default */
    report[31] = 0x01;     /* polling rate 1000Hz default */
}

/* Apply a single setting to an already-initialised report[] (no I/O). */
static void cadmouse_pro_apply(unsigned char *report, int type, int val1, int val2)
{
    switch (type) {
    case 1: /* speed -> byte[2] */
        report[2] = val2;
        break;
    case 2: /* smartscroll -> bytes[4-7] */
        if (val1 == 0 || val1 == 2) {
            report[4] = 0x01;
            report[5] = 0xff;
            report[7] = 0x00;
        } else {
            report[4] = 0x00;
            report[5] = 0x00;
            report[7] = 0x01;
        }
        break;
    case 3: /* polling rate -> byte[31] */
        report[31] = val1;
        break;
    case 4: /* button mapping */
        {
            int idx = val1 - 0x0a;
            if (idx >= 0 && idx < (int)(sizeof(cadmouse_pro_btn_pos) /
                                        sizeof(cadmouse_pro_btn_pos[0])))
                report[cadmouse_pro_btn_pos[idx]] = val2;
        }
        break;
    }
}

/* Write the assembled report[] to the device */
static int cadmouse_pro_send(hid_device *mouse, unsigned char *report)
{
    if (bt_fd >= 0)
        return bt_send_config_report(bt_fd, report, 32);
    return hid_send_feature_report(mouse, report, 32);
}

static void keepalive_mode(void);

int cadmouse_set_speed(hid_device *mouse, int speed);
int cadmouse_set_smartscroll(hid_device *mouse, int state);

enum cadmouse_pollrate {
    POLL_125 = 0x08,
    POLL_250 = 0x04,
    POLL_500 = 0x02,
    POLL_1000 = 0x01
};

int cadmouse_set_pollrate(hid_device *mouse, enum cadmouse_pollrate rate)
{
    return cadmouse_send_command(mouse, 0x06, 0x00, rate);
}

int cadmouse_set_liftoff_detection(hid_device *mouse, int state)
{
    if (bt_fd >= 0) {
        unsigned char report[8] = { 0x0c, 0x07, state ? 0x00 : 0x1f, 0x00, 0x00, 0x00, 0x00, 0x00 };
        return ioctl(bt_fd, HIDIOCSFEATURE(8), report);
    }
    return cadmouse_send_command(mouse, 0x07, 0x00, state ? 0x00 : 0x1f);
}

static void keepalive_mode(void)
{
    /* Open the hidraw device to keep it alive */
    char hidraw_name[64] = {0};
    if (find_hidraw_for_3dconnexion(hidraw_name, sizeof(hidraw_name)) == 0) {
        if (is_hidraw_bluetooth(hidraw_name)) {
            char dev_path[128];
            snprintf(dev_path, sizeof(dev_path), "/dev/%s", hidraw_name);
            bt_fd = open(dev_path, O_RDWR);
            if (bt_fd >= 0) {
                fprintf(stderr, "BT keepalive: %s\n", dev_path);
            }
        } else {
            char dev_path[128];
            snprintf(dev_path, sizeof(dev_path), "/dev/%s", hidraw_name);
            int usb_fd = open(dev_path, O_RDWR);
            if (usb_fd >= 0) {
                bt_fd = usb_fd;
                fprintf(stderr, "USB keepalive: %s\n", dev_path);
            }
        }
    }

    if (bt_fd < 0) {
        fprintf(stderr, "Warning: Could not open hidraw for keepalive\n");
        return;
    }

    fprintf(stderr, "Keepalive running (Ctrl+C to exit)...\n");
    pause();
}

typedef struct Button {
    char *name;
    char id;
} Button;

Button HWButtons[] = {
    { "left", 0x0a },
    { "right", 0x0b },
    { "middle", 0x0c },
    { "wheel", 0x0d },
    { "forward", 0x0e },
    { "backward", 0x0f },
    { "rm", 0x10 },
    { NULL, 0 }
};

Button SWButtons[] = {
    { "left", 0x0a },
    { "right", 0x0b },
    { "middle", 0x0c },
    { "backward", 0x0d },
    { "forward", 0x0e },
    { "rm", 0x2e },
    { "extra", 0x2f },
    { NULL, 0 }
};

Button *get_button(const char *name, Button *type)
{
    for (; type->name != NULL; type++) {
        if (strcasecmp(type->name, name) == 0)
            break;
    }

    return type->name != NULL ? type : NULL;
}

int cadmouse_set_hwbutton(hid_device *mouse, Button *hw, Button *sw)
{
    return cadmouse_send_command(mouse, hw->id, 0x00, sw->id);
}

int cadmouse_set_speed(hid_device *mouse, int speed)
{
    return cadmouse_send_command(mouse, 0x01, 0x00, speed);
}

#define COMMAND(cmd, ...)                                          \
    do {                                                           \
        res = cmd(mouse, __VA_ARGS__);                             \
        if (res == -1) {                                           \
            fwprintf(stderr, L"%s: %s\n", #cmd, hid_error(mouse)); \
            goto error;                                            \
        }                                                          \
    } while (0)

static const struct option long_opts[] = {
    {"keepalive", no_argument, 0, 'k'},
    {0, 0, 0, 0}
};

int main(int argc, char **argv)
{
    int opt, res;
    int keepalive = 0;

    /* Known CadMice, in open-preference order. "wireless" means the device
     * speaks the Pro 32-byte config-report protocol rather than the original
     * incremental command protocol.  TODO: change wireless to something else */
    static const struct { unsigned short pid; int wireless; } known_devices[] = {
        { 0xc650, 0 },   /* CadMouse (original) */
        { 0xc656, 1 },   /* CadMouse Pro */
        { 0xc654, 1 },   /* CadMouse Pro Wireless */
    };
    hid_device *mouse = NULL;
    int wireless = 0;
    for (size_t i = 0; i < sizeof(known_devices) / sizeof(known_devices[0]); i++) {
        mouse = hid_open(0x256f, known_devices[i].pid, NULL);
        if (mouse != NULL) {
            cadmouse_pid = known_devices[i].pid;
            wireless = known_devices[i].wireless;
            break;
        }
    }

    if (mouse == NULL) {
        fputs("Could not find/open a CadMouse (wired or wireless)\n", stderr);
        goto error;
    }

    fprintf(stderr, "Opened CadMouse 256f:%04x%s\n",
            cadmouse_pid, wireless ? " (Pro/Wireless config protocol)" : "");

    /* If connected via Bluetooth, open the hidraw interface that carries the
     * config report. (USB devices return -1) */
    bt_fd = open_bt_config_hidraw();

    /* Collect all settings into a single report */
    unsigned char pro_report[32];
    int pro_dirty = 0;
    if (wireless)
        cadmouse_pro_default_config(pro_report);

    extern char *optarg;
    extern int optind, opterr, optopt;
    int speedconflict = 0;
    while ((opt = getopt_long(argc, argv, "l:p:r:s:d:S:k", long_opts, NULL)) != -1) {
        switch(opt) {
            case 'k':
                keepalive = 1;
                break;
            case 'l':
                {
                    long int liftdetect = strtol(optarg, NULL, 10);

                    if (liftdetect == 0)
                        COMMAND(cadmouse_set_liftoff_detection, 0);
                    else
                        COMMAND(cadmouse_set_liftoff_detection, 1);
                }
                break;
            case 'p':
                {
                    long int rate = strtol(optarg, NULL, 10);
                    int pro_rate;

                    if (rate == 125)
                        pro_rate = POLL_125;
                    else if (rate == 250)
                        pro_rate = POLL_250;
                    else if (rate == 500)
                        pro_rate = POLL_500;
                    else if (rate == 1000)
                        pro_rate = POLL_1000;
                    else {
                        fputs("-p: Unsupported polling rate\n", stderr);
                        break;
                    }

                    if (wireless) {
                        cadmouse_pro_apply(pro_report, 3, pro_rate, 0);
                        pro_dirty = 1;
                    } else {
                        COMMAND(cadmouse_set_pollrate, (enum cadmouse_pollrate)pro_rate);
                    }
                }
                break;
            case 'r':
                {
                    Button *hw = NULL, *sw = NULL;
                    char *sep = strchr(optarg, ':');

                    if (sep != NULL) {
                        *sep = '\0';
                        sep++;
                        hw = get_button(optarg, HWButtons);
                        sw = get_button(sep, SWButtons);
                    }

                    if (hw == NULL || sw == NULL)
                        fputs("-r: invalid button mapping\n", stderr);
                    else if (wireless) {
                        cadmouse_pro_apply(pro_report, 4, hw->id, sw->id);
                        pro_dirty = 1;
                    } else {
                        COMMAND(cadmouse_set_hwbutton, hw, sw);
                    }
                }
                break;
            case 's':
                {
                    long int speed = strtol(optarg, NULL, 10);
                    if (speedconflict != 0)
                        fputs("-s: -s cannot be used with -d or used more than once\n", stderr);
                    else
                    if (speed < 1 || speed > 164)
                        fputs("-s: Option value out of range\n", stderr);
                    else {
                        speedconflict++;
                        if (wireless) {
                            cadmouse_pro_apply(pro_report, 1, 0, (int)speed);
                            pro_dirty = 1;
                        } else {
                            COMMAND(cadmouse_set_speed, speed);
                        }
                    }
                }
                break;
              case 'd':
                {
                    long int dpi = strtol(optarg, NULL, 10);
                    long int speed = (dpi * 164)/8200;
                    if (speedconflict != 0)
                        fputs("-d: -d cannot be used with -s or used more than once\n", stderr);
                    if (dpi < 50 || dpi > 8200)
                        fputs("-d: Option value out of range\n", stderr);
                    else {
                        speedconflict++;
                        if (wireless) {
                            cadmouse_pro_apply(pro_report, 1, 0, (int)speed);
                            pro_dirty = 1;
                        } else {
                            COMMAND(cadmouse_set_speed, speed);
                        }
                    }
                }
                break;
            case 'S':
                {
                    long int smartscroll = strtol(optarg, NULL, 10);

                    if (smartscroll >= 0 && smartscroll < 4) {
                        if (wireless) {
                            cadmouse_pro_apply(pro_report, 2, (int)smartscroll, 0);
                            pro_dirty = 1;
                        } else {
                            COMMAND(cadmouse_set_smartscroll, smartscroll);
                        }
                    }
                    else
                        fputs("-S: Option value out of range\n", stderr);
                }
                break;
        }
    }

    /* Send the accumulated Pro/Wireless configuration */
    if (wireless && pro_dirty) {
        res = cadmouse_pro_send(mouse, pro_report);
        if (res < 0) {
            fprintf(stderr, "failed to write configuration to device: %s\n",
                    strerror(errno));
            goto error;
        }
    }

error:

    if (keepalive) {
        if (mouse != NULL) {
            hid_close(mouse);
            hid_exit();
        }
        keepalive_mode();
    }

    if (bt_fd >= 0)
        close(bt_fd);

    if (mouse != NULL) {
        hid_close(mouse);
        hid_exit();
    }

    return 0;
}
