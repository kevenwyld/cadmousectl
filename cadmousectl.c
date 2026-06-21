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
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/hidraw.h>
#include <signal.h>
#include <sys/signalfd.h>

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

static int find_hidraw_for_3dconnexion(char *name_buf, int buf_len)
{
    DIR *dir = opendir("/sys/class/hidraw");
    if (!dir)
        return -1;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (ent->d_type != DT_LNK)
            continue;

        const char *name = ent->d_name;
        if (strncmp(name, "hidraw", 6) != 0)
            continue;

        char path[512];
        snprintf(path, sizeof(path), "/sys/class/hidraw/%s/device/uevent", name);

        FILE *f = fopen(path, "r");
        if (!f)
            continue;

        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "256F") &&
                (strstr(line, "C654") || strstr(line, "C656") || strstr(line, "C650"))) {
                if (name_buf && buf_len > 0) {
                    strncpy(name_buf, name, buf_len - 1);
                    name_buf[buf_len - 1] = '\0';
                }
                fclose(f);
                closedir(dir);
                return 0;
            }
        }
        fclose(f);
    }

    closedir(dir);
    return -1;
}

static int bt_send_config_report(int fd, unsigned char *report, int len)
{
    return ioctl(fd, HIDIOCSFEATURE(len), report);
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
 * for all settings. This function builds the default report, modifies the
 * relevant byte(s), and sends it. */
static int cadmouse_pro_set_config(hid_device *mouse, int type, int val1, int val2)
{
    unsigned char report[32];
    memset(report, 0, sizeof(report));
    report[0] = 0x10;      /* ReportID */
    report[2] = 0x1c;      /* Pointer speed default */
    report[17] = 0x03;     /* Button mapping count */
    report[18] = 0x00;
    report[19] = 0x0a;     /* left */
    report[20] = 0x0b;     /* right */
    report[21] = 0x0c;     /* middle */
    report[22] = 0x0c;     /* wheel */
    /* Forward/backward defaults differ per model (verified against USB captures
     * of the official driver). The scroll button (byte 25) is left at 0x00,
     * which the firmware treats as "keep this button's default function";
     * writing any specific code here is what remapped the scroll button. */
    if (cadmouse_pid == 0xc654) {   /* CadMouse Pro Wireless */
        report[23] = 0x2d;     /* forward */
        report[24] = 0x2e;     /* backward */
    } else {                        /* CadMouse Pro (0xc656) */
        report[23] = 0x0e;     /* forward */
        report[24] = 0x0d;     /* backward */
    }
    report[25] = 0x00;     /* scroll button: keep firmware default */
    report[26] = 0x00;
    report[27] = 0x1e;     /* rm map */
    report[28] = 0x00;
    report[29] = 0x00;
    report[30] = 0x00;
    report[31] = 0x01;     /* Polling rate 1000Hz default */

    /* Map hw button ID to byte position in config report */
    static const int btn_positions[] = {
        19,   /* 0x0a left */
        20,   /* 0x0b right */
        21,   /* 0x0c middle */
        22,   /* 0x0d wheel */
        23,   /* 0x0e forward */
        24,   /* 0x0f backward */
        27    /* 0x10 rm */
    };

    switch (type) {
    case 1: /* speed (0x01 -> byte[2]) */
        report[2] = val2;
        break;
    case 2: /* smartscroll (0x03/0x04/0x05 -> bytes[4-7]) */
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
    case 3: /* polling rate (0x06 -> byte[31]) */
        report[31] = val1;
        break;
    case 4: /* button mapping */
        {
            int idx = val1 - 0x0a;
            if (idx >= 0 && idx < 7)
                report[btn_positions[idx]] = val2;
        }
        break;
    case 5: /* liftoff detection (0x07) — uses 8-byte report format instead */
        break;
    }

    if (bt_fd >= 0)
        return bt_send_config_report(bt_fd, report, sizeof(report));
    return hid_send_feature_report(mouse, report, sizeof(report));
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

    hid_device *mouse = hid_open(0x256f, 0xc650, NULL);
    int wireless = 0;
    if (mouse != NULL)
        cadmouse_pid = 0xc650;

    if (mouse == NULL) {
        mouse = hid_open(0x256f, 0xc656, NULL);
        if (mouse != NULL)
            cadmouse_pid = 0xc656;
        if (mouse == NULL) {
            mouse = hid_open(0x256f, 0xc654, NULL);
            if (mouse != NULL)
                cadmouse_pid = 0xc654;
        }
        if (mouse != NULL)
            wireless = 1;
    }

    if (mouse == NULL) {
        fputs("Could not find/open a CadMouse (wired or wireless)\n", stderr);
        goto error;
    }

    fprintf(stderr, "Opened CadMouse 256f:%04x%s\n",
            cadmouse_pid, wireless ? " (Pro/Wireless config protocol)" : "");

    /* Check if connected via Bluetooth */
    char hidraw_name[64] = {0};

    if (find_hidraw_for_3dconnexion(hidraw_name, sizeof(hidraw_name)) == 0) {
        if (is_hidraw_bluetooth(hidraw_name)) {
            char dev_path[128];
            snprintf(dev_path, sizeof(dev_path), "/dev/%s", hidraw_name);
            bt_fd = open(dev_path, O_RDWR);
            if (bt_fd < 0) {
                fprintf(stderr, "Warning: Could not open %s for Bluetooth communication: %s\n",
                        dev_path, strerror(errno));
                bt_fd = -1;
            } else {
                fprintf(stderr, "Connected via Bluetooth (%s)\n", dev_path);
            }
        }
    }

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
                        res = cadmouse_pro_set_config(mouse, 3, pro_rate, 0);
                        if (res < 0) {
                            fputs("-p: failed to set polling rate\n", stderr);
                            goto error;
                        }
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
                        res = cadmouse_pro_set_config(mouse, 4, hw->id, sw->id);
                        if (res < 0) {
                            fputs("-r: failed to set button mapping\n", stderr);
                            goto error;
                        }
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
                            res = cadmouse_pro_set_config(mouse, 1, 0, (int)speed);
                            if (res < 0) {
                                fputs("-s: failed to set speed\n", stderr);
                                goto error;
                            }
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
                            res = cadmouse_pro_set_config(mouse, 1, 0, (int)speed);
                            if (res < 0) {
                                fputs("-d: failed to set speed\n", stderr);
                                goto error;
                            }
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
                            res = cadmouse_pro_set_config(mouse, 2, (int)smartscroll, 0);
                            if (res < 0) {
                                fputs("-S: failed to set smartscroll\n", stderr);
                                goto error;
                            }
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
