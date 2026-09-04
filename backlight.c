#include "backlight.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <stdio.h>

/* cubevol's persistentmem backlight slot (reverse-engineered from cubevol's
 * sysdata_get/set_backlight_value). Struct + ioctl cmds must match exactly. */
struct pmem_req { unsigned short flag, id, len, pad; void *buf; };
#define PMEM_GET_BACKLIGHT 0x400c2602u
#define PMEM_SET_BACKLIGHT 0x800c2603u
#define I2SO_SET_VOLUME    0x8001080bu

/* Level (0..100) → raw backlight byte (23..255). Matches cubevol's own curve so
 * the value we store in persistentmem is what cubevol would store itself. */
static int backlight_raw(int level) {
    if (level < 0)   level = 0;
    if (level > 100) level = 100;
    int out = (level < 43) ? (level + 23) : (level * level / 43 + 23);
    if (out > 255) out = 255;
    return out;
}

/* Read cubevol's stored raw backlight from persistentmem (slot 30), or -1. */
int cube_pmem_backlight_read(void) {
    int fd = open("/dev/persistentmem", O_RDWR);
    if (fd < 0) return -1;
    int value = 0;
    struct pmem_req req = { 1, 30, 1, 0, &value };
    int rv = ioctl(fd, PMEM_GET_BACKLIGHT, &req);
    close(fd);
    if (rv < 0) return -1;
    return value & 0xFF;
}

/* Read cubevol's stored snd volume from persistentmem, or -1. RE'd from cubevol
 * avparam_get_volume: same GET ioctl, req{flag=3,id=0,len=260}, volume = buf[0]. */
int cube_pmem_volume_read(void) {
    int fd = open("/dev/persistentmem", O_RDWR);
    if (fd < 0) return -1;
    unsigned char buf[260] = {0};
    struct pmem_req req = { 3, 0, 260, 0, buf };
    int rv = ioctl(fd, PMEM_GET_BACKLIGHT, &req);   /* 0x400c2602 GET */
    close(fd);
    if (rv < 0) return -1;
    return buf[0];
}

/* Sync cubevol's stored raw backlight so its DELAYED startup apply already shows
 * the right brightness (no flash to our re-assert). persistentmem is EEPROM-like:
 * write ONLY on real change, never per frame. No-op if already correct. */
void cube_pmem_backlight_sync(int level) {
    int raw = backlight_raw(level);
    if (cube_pmem_backlight_read() == raw) return;   /* already in sync */
    int fd = open("/dev/persistentmem", O_RDWR);
    if (fd < 0) return;
    int value = raw;
    struct pmem_req req = { 1, 30, 1, 0, &value };
    (void)!ioctl(fd, PMEM_SET_BACKLIGHT, &req);
    close(fd);
}

void cube_set_backlight(int level) {
    int out = backlight_raw(level);
    int fd = open("/dev/backlight", O_RDWR);
    if (fd < 0) return;
    (void)!write(fd, &out, sizeof(int));
    close(fd);
}

static void cube_set_i2so_volume(int level) {
    unsigned char value;
    if (level < 0) level = 0;
    if (level > 100) level = 100;
    value = (unsigned char)level;
    /* Match cubevol's api_set_volume(): write-only open + this exact ioctl. */
    int fd = open("/dev/sndC0i2so", O_WRONLY);
    if (fd < 0) return;
    (void)ioctl(fd, I2SO_SET_VOLUME, &value);
    close(fd);
}

void cube_set_i2so_output_muted(int muted) {
    /* R36SX cubevol's api_set_i2so_gpio_mute exists but has no callers. Its
       real set_audio_mute path uses api_set_volume(0), so use that proven
       control and restore the stock daemon's saved hardware volume on launch. */
    if (muted) {
        cube_set_i2so_volume(0);
    } else {
        int volume = cube_pmem_volume_read();
        if (volume >= 0) cube_set_i2so_volume(volume);
    }
}

/* Write the SHARED system volume: cubevol's persistentmem slot (what the
 * physical volume buttons use) + the I2SO hardware volume (same ioctl a
 * cubevol button press applies, via the existing helper above) + legacy
 * cubegm/sndgain.txt for standalone frontends (pcsx4all, lgpt).  After
 * this, Settings' Volume slider and the physical buttons are one and the
 * same value, applied in real time.  persistentmem is EEPROM-like: write
 * ONLY on real change, never per frame. */
void cube_pmem_volume_write(int level) {
    if (level < 0)   level = 0;
    if (level > 100) level = 100;
    if (cube_pmem_volume_read() == level) return;   /* already in sync */
    int fd = open("/dev/persistentmem", O_RDWR);
    if (fd >= 0) {
        unsigned char buf[260] = {0};
        buf[0] = (unsigned char)level;
        struct pmem_req req = { 3, 0, 260, 0, buf };
        (void)!ioctl(fd, PMEM_SET_BACKLIGHT, &req);  /* 0x800c2603 SET */
        close(fd);
    }
    /* Mirror to the I2SO hardware path exactly like a cubevol button press,
     * so the new level is audible immediately (and Volume 0 truly silences:
     * the DAC/amp path is muted, not just the samples). */
    cube_set_i2so_volume(level);
    FILE *f = fopen("/mnt/sdcard/cubegm/sndgain.txt", "w");
    if (f) { fprintf(f, "%d\n", level); fclose(f); }
}

#include <stdlib.h>

void fb1_set_visible(int visible) {
    if (!visible) return;
    /* cubevol owns the physical volume GPIOs. Restarting it here can leave its
     * button state/debounce machinery in a bad state (most visibly volume-down
     * needing repeated presses). The battery mask handles the stale fb1 corner,
     * so leave the stock daemon untouched. Only recover it if it actually died. */
    if (system("pidof cubevol >/dev/null 2>&1") != 0) {
        system("/usr/bin/cubevol &");
        usleep(50000);
    }
}
