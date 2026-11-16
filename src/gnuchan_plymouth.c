#define _POSIX_C_SOURCE 200809L

#include <dirent.h>
#include <drm.h>
#include <drm_mode.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <png.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#define MAX_CONFIG_LINE 1024
#define MAX_CONFIG_VALUE 512
#define MAX_FRAMEBUFFER_DIMENSION 16384
#define MAX_DURATION_MS 15000

typedef struct {
    uint32_t width;
    uint32_t height;
    uint8_t *pixels;
} Image;

typedef struct {
    char drm_device[MAX_CONFIG_VALUE];
    char background[MAX_CONFIG_VALUE];
    char logo[MAX_CONFIG_VALUE];
    unsigned wait_for_framebuffer_ms;
    unsigned duration_ms;
    unsigned fps;
} Config;

typedef struct {
    int fd;
    uint8_t *memory;
    size_t memory_length;
    struct fb_fix_screeninfo fixed;
    struct fb_var_screeninfo variable;
    uint32_t drm_handle;
    uint32_t drm_fb_id;
    uint32_t drm_crtc_id;
    uint32_t drm_connector_id;
    drmModeCrtcPtr saved_crtc;
    drmModeModeInfo mode;
    bool drm_master;
} Framebuffer;

static void report_error(const char *message)
{
    fprintf(stderr, "GnuChanPlymouth: %s\n", message);
}

static bool read_text_file(const char *path, char *buffer, size_t size)
{
    FILE *file = fopen(path, "r");
    if (file == NULL)
        return false;

    size_t count = fread(buffer, 1, size - 1, file);
    bool okay = !ferror(file);
    fclose(file);
    buffer[count] = '\0';
    return okay;
}

static bool gpu_from_vendor(const char *vendor, const char *class_code,
                            char *gpu, size_t gpu_size)
{
    char *end = NULL;
    unsigned long device_class = strtoul(class_code, &end, 0);
    if (end == class_code || device_class >> 16 != 0x03)
        return false;

    if (strcmp(vendor, "0x8086") == 0)
        snprintf(gpu, gpu_size, "intel");
    else if (strcmp(vendor, "0x1002") == 0)
        snprintf(gpu, gpu_size, "amd");
    else if (strcmp(vendor, "0x10de") == 0)
        snprintf(gpu, gpu_size, "nvidia");
    else
        return false;
    return true;
}

static bool selected_gpu_from_cmdline(const char *path, char *gpu,
                                      size_t gpu_size)
{
    char cmdline[8192];
    if (!read_text_file(path, cmdline, sizeof(cmdline)))
        return false;

    char *save = NULL;
    for (char *token = strtok_r(cmdline, " \t\r\n", &save);
         token != NULL; token = strtok_r(NULL, " \t\r\n", &save)) {
        if (strncmp(token, "gnuchan.gpu=", 12) != 0)
            continue;
        const char *selected = token + 12;
        if (strcmp(selected, "intel") == 0)
            snprintf(gpu, gpu_size, "intel");
        else if (strcmp(selected, "amd") == 0)
            snprintf(gpu, gpu_size, "amd");
        else if (strcmp(selected, "nvidia") == 0 ||
                 strcmp(selected, "nvidia-new") == 0 ||
                 strcmp(selected, "nvidia-legacy") == 0)
            snprintf(gpu, gpu_size, "nvidia");
        else {
            report_error("unknown gnuchan.gpu selection; using PCI detection");
            return false;
        }
        return true;
    }
    return false;
}

static bool detect_gpu(const char *sysfs_root, const char *cmdline_path,
                       char *gpu, size_t gpu_size)
{
    if (cmdline_path != NULL &&
        selected_gpu_from_cmdline(cmdline_path, gpu, gpu_size))
        return true;

    char devices_path[1024];
    if (snprintf(devices_path, sizeof(devices_path), "%s/bus/pci/devices",
                 sysfs_root) >= (int)sizeof(devices_path))
        return false;

    DIR *devices = opendir(devices_path);
    if (devices == NULL)
        return false;

    char first_gpu[32] = "";
    char boot_gpu[32] = "";
    struct dirent *entry;
    while ((entry = readdir(devices)) != NULL) {
        if (entry->d_name[0] == '.')
            continue;

        char device_path[1200];
        char vendor_path[1300];
        char class_path[1300];
        char boot_vga_path[1300];
        if (snprintf(device_path, sizeof(device_path), "%s/%s",
                     devices_path, entry->d_name) >= (int)sizeof(device_path) ||
            snprintf(vendor_path, sizeof(vendor_path), "%s/vendor",
                     device_path) >= (int)sizeof(vendor_path) ||
            snprintf(class_path, sizeof(class_path), "%s/class",
                     device_path) >= (int)sizeof(class_path) ||
            snprintf(boot_vga_path, sizeof(boot_vga_path), "%s/boot_vga",
                     device_path) >= (int)sizeof(boot_vga_path))
            continue;

        char vendor[64];
        char class_code[64];
        char detected[32];
        if (!read_text_file(vendor_path, vendor, sizeof(vendor)) ||
            !read_text_file(class_path, class_code, sizeof(class_code)))
            continue;
        vendor[strcspn(vendor, "\r\n")] = '\0';
        class_code[strcspn(class_code, "\r\n")] = '\0';
        if (!gpu_from_vendor(vendor, class_code, detected, sizeof(detected)))
            continue;

        if (first_gpu[0] == '\0')
            snprintf(first_gpu, sizeof(first_gpu), "%s", detected);

        char boot_vga[8];
        if (read_text_file(boot_vga_path, boot_vga, sizeof(boot_vga)) &&
            boot_vga[0] == '1')
            snprintf(boot_gpu, sizeof(boot_gpu), "%s", detected);
    }
    closedir(devices);

    const char *selected = boot_gpu[0] != '\0' ? boot_gpu : first_gpu;
    if (selected[0] == '\0')
        return false;
    snprintf(gpu, gpu_size, "%s", selected);
    return true;
}

static bool parse_unsigned(const char *value, unsigned maximum,
                           unsigned *result)
{
    char *end = NULL;
    errno = 0;
    unsigned long parsed = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed > maximum)
        return false;
    *result = (unsigned)parsed;
    return true;
}

static bool assign_config(Config *config, const char *key, const char *value)
{
    if (strcmp(key, "drm_device") == 0) {
        if (value[0] != '/')
            return false;
        snprintf(config->drm_device, sizeof(config->drm_device), "%s", value);
    } else if (strcmp(key, "background") == 0) {
        if (value[0] != '/')
            return false;
        snprintf(config->background, sizeof(config->background), "%s", value);
    } else if (strcmp(key, "logo") == 0) {
        if (value[0] != '/')
            return false;
        snprintf(config->logo, sizeof(config->logo), "%s", value);
    } else if (strcmp(key, "wait_for_framebuffer_ms") == 0) {
        return parse_unsigned(value, 10000, &config->wait_for_framebuffer_ms);
    } else if (strcmp(key, "duration_ms") == 0) {
        return parse_unsigned(value, MAX_DURATION_MS, &config->duration_ms);
    } else if (strcmp(key, "fps") == 0) {
        if (!parse_unsigned(value, 60, &config->fps))
            return false;
        return config->fps >= 1;
    } else {
        return false;
    }
    return true;
}

static bool load_config(const char *config_dir, const char *gpu, Config *config)
{
    memset(config, 0, sizeof(*config));
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s.conf", config_dir, gpu) >=
        (int)sizeof(path))
        return false;

    FILE *file = fopen(path, "r");
    if (file == NULL) {
        perror(path);
        return false;
    }

    char line[MAX_CONFIG_LINE];
    unsigned line_number = 0;
    bool okay = true;
    while (fgets(line, sizeof(line), file) != NULL) {
        ++line_number;
        line[strcspn(line, "\r\n")] = '\0';
        char *start = line;
        while (*start == ' ' || *start == '\t')
            ++start;
        if (*start == '\0' || *start == '#')
            continue;

        char *separator = strchr(start, '=');
        if (separator == NULL) {
            okay = false;
            break;
        }
        *separator++ = '\0';
        char *end = start + strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        while (*separator == ' ' || *separator == '\t')
            ++separator;
        end = separator + strlen(separator);
        while (end > separator && (end[-1] == ' ' || end[-1] == '\t'))
            *--end = '\0';
        if (!assign_config(config, start, separator)) {
            fprintf(stderr, "GnuChanPlymouth: invalid %s:%u\n", path,
                    line_number);
            okay = false;
            break;
        }
    }
    if (ferror(file))
        okay = false;
    fclose(file);

    if (okay && (config->drm_device[0] == '\0' ||
                 config->background[0] == '\0' || config->logo[0] == '\0' ||
                 config->duration_ms == 0 || config->fps == 0)) {
        fprintf(stderr, "GnuChanPlymouth: incomplete GPU profile %s\n", path);
        okay = false;
    }
    return okay;
}

static bool load_png(const char *path, Image *image)
{
    png_image png = {0};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&png, path)) {
        fprintf(stderr, "GnuChanPlymouth: cannot read %s: %s\n",
                path, png.message);
        return false;
    }

    png.format = PNG_FORMAT_RGBA;
    if (png.width == 0 || png.height == 0 ||
        png.width > MAX_FRAMEBUFFER_DIMENSION ||
        png.height > MAX_FRAMEBUFFER_DIMENSION) {
        png_image_free(&png);
        report_error("PNG dimensions are outside the supported range");
        return false;
    }

    size_t size = PNG_IMAGE_SIZE(png);
    image->pixels = malloc(size);
    if (image->pixels == NULL) {
        png_image_free(&png);
        report_error("could not allocate image memory");
        return false;
    }
    if (!png_image_finish_read(&png, NULL, image->pixels, 0, NULL)) {
        fprintf(stderr, "GnuChanPlymouth: cannot decode %s: %s\n",
                path, png.message);
        free(image->pixels);
        image->pixels = NULL;
        png_image_free(&png);
        return false;
    }
    image->width = png.width;
    image->height = png.height;
    png_image_free(&png);
    return true;
}

static void free_image(Image *image)
{
    free(image->pixels);
    image->pixels = NULL;
}

static void close_framebuffer(Framebuffer *framebuffer)
{
    if (framebuffer->fd < 0)
        return;
    if (framebuffer->drm_fb_id != 0) {
        if (framebuffer->saved_crtc != NULL) {
            uint32_t connector = framebuffer->drm_connector_id;
            bool had_mode = framebuffer->saved_crtc->mode_valid;
            if (drmModeSetCrtc(
                    framebuffer->fd, framebuffer->drm_crtc_id,
                    had_mode ? framebuffer->saved_crtc->buffer_id : 0,
                    framebuffer->saved_crtc->x, framebuffer->saved_crtc->y,
                    had_mode ? &connector : NULL, had_mode ? 1 : 0,
                    had_mode ? &framebuffer->saved_crtc->mode : NULL) != 0)
                fprintf(stderr, "GnuChanPlymouth: could not restore previous "
                                "DRM mode: %s\n", strerror(errno));
        }
        drmModeRmFB(framebuffer->fd, framebuffer->drm_fb_id);
    }
    if (framebuffer->memory != NULL && framebuffer->memory != MAP_FAILED)
        munmap(framebuffer->memory, framebuffer->memory_length);
    if (framebuffer->drm_handle != 0) {
        struct drm_mode_destroy_dumb destroy = {
            .handle = framebuffer->drm_handle
        };
        drmIoctl(framebuffer->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &destroy);
    }
    if (framebuffer->saved_crtc != NULL)
        drmModeFreeCrtc(framebuffer->saved_crtc);
    if (framebuffer->drm_master)
        drmDropMaster(framebuffer->fd);
    close(framebuffer->fd);
    framebuffer->fd = -1;
    framebuffer->memory = NULL;
    framebuffer->saved_crtc = NULL;
}

static bool open_framebuffer_at(const char *path, unsigned wait_ms,
                                Framebuffer *framebuffer)
{
    const unsigned interval_ms = 100;
    unsigned elapsed = 0;
    while (true) {
        framebuffer->fd = open(path, O_RDWR | O_CLOEXEC);
        if (framebuffer->fd >= 0)
            break;
        if (elapsed >= wait_ms) {
            fprintf(stderr, "GnuChanPlymouth: cannot open DRM device %s: %s\n",
                    path, strerror(errno));
            return false;
        }
        struct timespec delay = {.tv_sec = 0, .tv_nsec = interval_ms * 1000000L};
        nanosleep(&delay, NULL);
        elapsed += interval_ms;
    }

    if (drmSetMaster(framebuffer->fd) != 0) {
        fprintf(stderr, "GnuChanPlymouth: cannot acquire DRM display master: "
                        "%s\n", strerror(errno));
        close(framebuffer->fd);
        framebuffer->fd = -1;
        return false;
    }
    framebuffer->drm_master = true;

    drmModeResPtr resources = drmModeGetResources(framebuffer->fd);
    if (resources == NULL) {
        report_error("cannot query DRM display resources");
        close_framebuffer(framebuffer);
        return false;
    }

    drmModeConnectorPtr selected_connector = NULL;
    drmModeModeInfo selected_mode = {0};
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnectorPtr connector =
            drmModeGetConnector(framebuffer->fd, resources->connectors[i]);
        if (connector == NULL)
            continue;
        if (connector->connection != DRM_MODE_CONNECTED ||
            connector->count_modes == 0) {
            drmModeFreeConnector(connector);
            continue;
        }

        drmModeEncoderPtr encoder = NULL;
        if (connector->encoder_id != 0)
            encoder = drmModeGetEncoder(framebuffer->fd, connector->encoder_id);
        if (encoder == NULL) {
            for (int j = 0; j < connector->count_encoders; ++j) {
                encoder = drmModeGetEncoder(framebuffer->fd,
                                            connector->encoders[j]);
                if (encoder != NULL)
                    break;
            }
        }
        if (encoder == NULL) {
            drmModeFreeConnector(connector);
            continue;
        }

        uint32_t crtc_id = encoder->crtc_id;
        if (crtc_id == 0) {
            for (int j = 0; j < resources->count_crtcs; ++j) {
                if (encoder->possible_crtcs & (1U << j)) {
                    crtc_id = resources->crtcs[j];
                    break;
                }
            }
        }
        drmModeFreeEncoder(encoder);
        if (crtc_id == 0) {
            drmModeFreeConnector(connector);
            continue;
        }

        int mode_index = 0;
        for (int j = 0; j < connector->count_modes; ++j) {
            if (connector->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
                mode_index = j;
                break;
            }
        }
        framebuffer->drm_crtc_id = crtc_id;
        framebuffer->drm_connector_id = connector->connector_id;
        selected_mode = connector->modes[mode_index];
        selected_connector = connector;
        break;
    }
    drmModeFreeResources(resources);
    if (selected_connector == NULL) {
        report_error("no connected DRM display with a usable CRTC");
        close_framebuffer(framebuffer);
        return false;
    }
    framebuffer->saved_crtc =
        drmModeGetCrtc(framebuffer->fd, framebuffer->drm_crtc_id);
    drmModeFreeConnector(selected_connector);

    if (selected_mode.hdisplay == 0 || selected_mode.vdisplay == 0 ||
        selected_mode.hdisplay > MAX_FRAMEBUFFER_DIMENSION ||
        selected_mode.vdisplay > MAX_FRAMEBUFFER_DIMENSION) {
        report_error("connected DRM display mode is outside the supported range");
        close_framebuffer(framebuffer);
        return false;
    }

    struct drm_mode_create_dumb create = {
        .width = selected_mode.hdisplay,
        .height = selected_mode.vdisplay,
        .bpp = 32
    };
    if (drmIoctl(framebuffer->fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0) {
        fprintf(stderr, "GnuChanPlymouth: cannot allocate DRM scanout buffer: "
                        "%s\n", strerror(errno));
        close_framebuffer(framebuffer);
        return false;
    }
    framebuffer->drm_handle = create.handle;
    framebuffer->memory_length = (size_t)create.size;
    framebuffer->fixed.line_length = create.pitch;
    framebuffer->fixed.smem_len = (uint32_t)create.size;
    framebuffer->variable.xres = selected_mode.hdisplay;
    framebuffer->variable.yres = selected_mode.vdisplay;
    framebuffer->variable.bits_per_pixel = 32;
    framebuffer->variable.red = (struct fb_bitfield){.offset = 16, .length = 8};
    framebuffer->variable.green = (struct fb_bitfield){.offset = 8, .length = 8};
    framebuffer->variable.blue = (struct fb_bitfield){.offset = 0, .length = 8};

    struct drm_mode_map_dumb map = {.handle = framebuffer->drm_handle};
    if (drmIoctl(framebuffer->fd, DRM_IOCTL_MODE_MAP_DUMB, &map) != 0) {
        fprintf(stderr, "GnuChanPlymouth: cannot map DRM scanout buffer: %s\n",
                strerror(errno));
        close_framebuffer(framebuffer);
        return false;
    }
    framebuffer->memory = mmap(NULL, framebuffer->memory_length,
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               framebuffer->fd, (off_t)map.offset);
    if (framebuffer->memory == MAP_FAILED) {
        framebuffer->memory = NULL;
        fprintf(stderr, "GnuChanPlymouth: cannot map DRM scanout buffer: %s\n",
                strerror(errno));
        close_framebuffer(framebuffer);
        return false;
    }

    if (drmModeAddFB(framebuffer->fd, framebuffer->variable.xres,
                     framebuffer->variable.yres, 24, 32,
                     framebuffer->fixed.line_length, framebuffer->drm_handle,
                     &framebuffer->drm_fb_id) != 0) {
        fprintf(stderr, "GnuChanPlymouth: cannot register DRM buffer: %s\n",
                strerror(errno));
        close_framebuffer(framebuffer);
        return false;
    }
    framebuffer->mode = selected_mode;
    return true;
}

static bool is_drm_card_name(const char *name)
{
    if (strncmp(name, "card", 4) != 0 || name[4] == '\0')
        return false;
    for (const char *digit = name + 4; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9')
            return false;
    }
    return true;
}

static bool append_drm_candidate(char candidates[][MAX_CONFIG_VALUE],
                                size_t *count, const char *path)
{
    if (path[0] != '/' || *count >= 32)
        return false;
    for (size_t i = 0; i < *count; ++i) {
        if (strcmp(candidates[i], path) == 0)
            return true;
    }
    int length = snprintf(candidates[*count], MAX_CONFIG_VALUE, "%s", path);
    if (length < 0 || length >= MAX_CONFIG_VALUE)
        return false;
    ++*count;
    return true;
}

static bool open_framebuffer(const char *sysfs_root, const char *preferred_path,
                             unsigned wait_ms, Framebuffer *framebuffer)
{
    char candidates[32][MAX_CONFIG_VALUE];
    size_t candidate_count = 0;
    char drm_class[1024];
    if (snprintf(drm_class, sizeof(drm_class), "%s/class/drm", sysfs_root) <
        (int)sizeof(drm_class)) {
        DIR *devices = opendir(drm_class);
        if (devices != NULL) {
            struct dirent *entry;
            while ((entry = readdir(devices)) != NULL) {
                if (!is_drm_card_name(entry->d_name))
                    continue;
                char boot_vga_path[1400];
                if (snprintf(boot_vga_path, sizeof(boot_vga_path),
                             "%s/%s/device/boot_vga",
                             drm_class, entry->d_name) >=
                    (int)sizeof(boot_vga_path))
                    continue;
                char boot_vga[8];
                if (!read_text_file(boot_vga_path, boot_vga,
                                    sizeof(boot_vga)) ||
                    boot_vga[0] != '1')
                    continue;
                char card_path[MAX_CONFIG_VALUE];
                if (snprintf(card_path, sizeof(card_path), "/dev/dri/%s",
                             entry->d_name) < (int)sizeof(card_path))
                    append_drm_candidate(candidates, &candidate_count,
                                         card_path);
            }
            closedir(devices);
        }
    }

    append_drm_candidate(candidates, &candidate_count, preferred_path);
    if (snprintf(drm_class, sizeof(drm_class), "%s/class/drm", sysfs_root) <
        (int)sizeof(drm_class)) {
        DIR *devices = opendir(drm_class);
        if (devices != NULL) {
            struct dirent *entry;
            while ((entry = readdir(devices)) != NULL) {
                if (!is_drm_card_name(entry->d_name))
                    continue;
                char card_path[MAX_CONFIG_VALUE];
                if (snprintf(card_path, sizeof(card_path), "/dev/dri/%s",
                             entry->d_name) < (int)sizeof(card_path))
                    append_drm_candidate(candidates, &candidate_count,
                                         card_path);
            }
            closedir(devices);
        }
    }

    for (size_t i = 0; i < candidate_count; ++i) {
        unsigned candidate_wait_ms = i == 0 ? wait_ms : 0;
        if (open_framebuffer_at(candidates[i], candidate_wait_ms, framebuffer)) {
            fprintf(stderr, "GnuChanPlymouth: using DRM device %s\n",
                    candidates[i]);
            return true;
        }
    }
    report_error("no DRM card could present to a connected display");
    return false;
}

static uint32_t channel_value(uint8_t value, struct fb_bitfield field)
{
    if (field.length == 0)
        return 0;
    uint64_t maximum = (UINT64_C(1) << field.length) - 1;
    return (uint32_t)(((uint64_t)value * maximum + 127) / 255) << field.offset;
}

static bool put_pixel(Framebuffer *fb, uint32_t x, uint32_t y,
                      uint8_t red, uint8_t green, uint8_t blue)
{
    const struct fb_var_screeninfo *v = &fb->variable;
    uint32_t bytes_per_pixel = v->bits_per_pixel / 8;
    uint64_t offset = (uint64_t)(y + v->yoffset) * fb->fixed.line_length +
                      (uint64_t)(x + v->xoffset) * bytes_per_pixel;
    if (x >= v->xres || y >= v->yres ||
        offset + bytes_per_pixel > fb->memory_length)
        return false;

    uint32_t pixel = channel_value(red, v->red) |
                     channel_value(green, v->green) |
                     channel_value(blue, v->blue);
    memcpy(fb->memory + offset, &pixel, bytes_per_pixel);
    return true;
}

static bool present_framebuffer(Framebuffer *fb)
{
    uint32_t connector = fb->drm_connector_id;
    if (drmModeSetCrtc(fb->fd, fb->drm_crtc_id, fb->drm_fb_id,
                       0, 0, &connector, 1, &fb->mode) != 0) {
        fprintf(stderr, "GnuChanPlymouth: cannot activate DRM scanout buffer: "
                        "%s\n", strerror(errno));
        return false;
    }
    return true;
}

static void clear_framebuffer(Framebuffer *framebuffer)
{
    for (uint32_t y = 0; y < framebuffer->variable.yres; ++y) {
        for (uint32_t x = 0; x < framebuffer->variable.xres; ++x)
            put_pixel(framebuffer, x, y, 0, 0, 0);
    }
}

static void blend_pixel(Framebuffer *fb, uint32_t x, uint32_t y,
                        const uint8_t *rgba)
{
    if (rgba[3] == 255) {
        put_pixel(fb, x, y, rgba[0], rgba[1], rgba[2]);
        return;
    }
    if (rgba[3] == 0)
        return;

    uint32_t bytes_per_pixel = fb->variable.bits_per_pixel / 8;
    uint64_t offset =
        (uint64_t)(y + fb->variable.yoffset) * fb->fixed.line_length +
        (uint64_t)(x + fb->variable.xoffset) * bytes_per_pixel;
    if (x >= fb->variable.xres || y >= fb->variable.yres ||
        offset + bytes_per_pixel > fb->memory_length)
        return;

    uint32_t previous = 0;
    memcpy(&previous, fb->memory + offset, bytes_per_pixel);
    uint32_t red_max = (UINT32_C(1) << fb->variable.red.length) - 1;
    uint32_t green_max = (UINT32_C(1) << fb->variable.green.length) - 1;
    uint32_t blue_max = (UINT32_C(1) << fb->variable.blue.length) - 1;
    uint8_t previous_red = (uint8_t)(
        ((previous >> fb->variable.red.offset) & red_max) * 255 / red_max);
    uint8_t previous_green = (uint8_t)(
        ((previous >> fb->variable.green.offset) & green_max) * 255 / green_max);
    uint8_t previous_blue = (uint8_t)(
        ((previous >> fb->variable.blue.offset) & blue_max) * 255 / blue_max);
    uint32_t alpha = rgba[3];
    put_pixel(fb, x, y,
              (uint8_t)((rgba[0] * alpha + previous_red * (255 - alpha)) / 255),
              (uint8_t)((rgba[1] * alpha + previous_green * (255 - alpha)) / 255),
              (uint8_t)((rgba[2] * alpha + previous_blue * (255 - alpha)) / 255));
}

static void draw_image(Framebuffer *fb, const Image *image, bool contain,
                       float center_x, float center_y,
                       float maximum_width, float maximum_height)
{
    const uint32_t screen_width = fb->variable.xres;
    const uint32_t screen_height = fb->variable.yres;
    float scale_x = (float)screen_width / image->width;
    float scale_y = (float)screen_height / image->height;
    if (contain) {
        float max_width = screen_width * maximum_width;
        float max_height = screen_height * maximum_height;
        scale_x = max_width / image->width;
        scale_y = max_height / image->height;
        if (scale_x > scale_y)
            scale_x = scale_y;
        else
            scale_y = scale_x;
    }

    uint32_t drawn_width = (uint32_t)(image->width * scale_x);
    uint32_t drawn_height = (uint32_t)(image->height * scale_y);
    if (drawn_width == 0 || drawn_height == 0)
        return;
    int start_x = (int)(center_x * screen_width - drawn_width / 2.0f);
    int start_y = (int)(center_y * screen_height - drawn_height / 2.0f);

    for (uint32_t y = 0; y < drawn_height; ++y) {
        int screen_y = start_y + (int)y;
        if (screen_y < 0 || screen_y >= (int)screen_height)
            continue;
        uint32_t source_y = (uint32_t)((uint64_t)y * image->height / drawn_height);
        for (uint32_t x = 0; x < drawn_width; ++x) {
            int screen_x = start_x + (int)x;
            if (screen_x < 0 || screen_x >= (int)screen_width)
                continue;
            uint32_t source_x = (uint32_t)((uint64_t)x * image->width / drawn_width);
            const uint8_t *pixel =
                image->pixels + ((size_t)source_y * image->width + source_x) * 4;
            blend_pixel(fb, (uint32_t)screen_x, (uint32_t)screen_y, pixel);
        }
    }
}

static void draw_dot(Framebuffer *fb, int center_x, int center_y, int radius,
                     unsigned brightness)
{
    const unsigned red = 110;
    const unsigned green = 45;
    const unsigned blue = 210;
    uint8_t color[4] = {
        (uint8_t)(red * brightness / 255),
        (uint8_t)(green * brightness / 255),
        (uint8_t)(blue * brightness / 255),
        255
    };
    int radius_squared = radius * radius;
    for (int y = -radius; y <= radius; ++y) {
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y <= radius_squared &&
                center_x + x >= 0 && center_y + y >= 0)
                blend_pixel(fb, (uint32_t)(center_x + x),
                            (uint32_t)(center_y + y), color);
        }
    }
}

static void restore_background_patch(Framebuffer *fb, const Image *background,
                                     int center_x, int center_y, int radius)
{
    for (int y = -radius; y <= radius; ++y) {
        int screen_y = center_y + y;
        if (screen_y < 0 || screen_y >= (int)fb->variable.yres)
            continue;
        uint32_t source_y =
            (uint32_t)((uint64_t)screen_y * background->height /
                       fb->variable.yres);
        for (int x = -radius; x <= radius; ++x) {
            if (x * x + y * y > radius * radius)
                continue;
            int screen_x = center_x + x;
            if (screen_x < 0 || screen_x >= (int)fb->variable.xres)
                continue;
            uint32_t source_x =
                (uint32_t)((uint64_t)screen_x * background->width /
                           fb->variable.xres);
            const uint8_t *pixel = background->pixels +
                ((size_t)source_y * background->width + source_x) * 4;
            uint8_t red = (uint8_t)((pixel[0] * pixel[3] + 8) / 255);
            uint8_t green = (uint8_t)((pixel[1] * pixel[3] + 8) / 255);
            uint8_t blue = (uint8_t)((pixel[2] * pixel[3] + 8) / 255);
            put_pixel(fb, (uint32_t)screen_x, (uint32_t)screen_y,
                      red, green, blue);
        }
    }
}

static void draw_frame(Framebuffer *fb, const Image *background,
                       const Image *logo, unsigned frame)
{
    if (frame == 0) {
        draw_image(fb, background, false, 0.5f, 0.5f, 1.0f, 1.0f);
        draw_image(fb, logo, true, 0.5f, 0.39f, 0.42f, 0.36f);
    }

    int radius = (int)(fb->variable.yres / 130);
    if (radius < 4)
        radius = 4;
    if (radius > 12)
        radius = 12;
    int spacing = radius * 4;
    int center_x = (int)fb->variable.xres / 2;
    int center_y = (int)(fb->variable.yres * 0.72);
    for (unsigned index = 0; index < 3; ++index) {
        unsigned phase = (frame + index * 6) % 18;
        unsigned distance = phase <= 9 ? phase : 18 - phase;
        int dot_x = center_x + ((int)index - 1) * spacing;
        restore_background_patch(fb, background, dot_x, center_y, radius);
        draw_dot(fb, dot_x, center_y, radius, 80 + distance * 19);
    }
}

static bool capture_framebuffer(Framebuffer *fb, const char *path)
{
    png_image png = {0};
    png.version = PNG_IMAGE_VERSION;
    png.width = fb->variable.xres;
    png.height = fb->variable.yres;
    png.format = PNG_FORMAT_RGBA;
    size_t size = PNG_IMAGE_SIZE(png);
    uint8_t *pixels = malloc(size);
    if (pixels == NULL) {
        report_error("could not allocate framebuffer capture");
        return false;
    }

    for (uint32_t y = 0; y < png.height; ++y) {
        for (uint32_t x = 0; x < png.width; ++x) {
            uint32_t bytes_per_pixel = fb->variable.bits_per_pixel / 8;
            uint64_t offset =
                (uint64_t)(y + fb->variable.yoffset) * fb->fixed.line_length +
                (uint64_t)(x + fb->variable.xoffset) * bytes_per_pixel;
            uint32_t pixel = 0;
            memcpy(&pixel, fb->memory + offset, bytes_per_pixel);
            uint8_t *rgba = pixels + ((size_t)y * png.width + x) * 4;
            uint32_t red_max = (UINT32_C(1) << fb->variable.red.length) - 1;
            uint32_t green_max =
                (UINT32_C(1) << fb->variable.green.length) - 1;
            uint32_t blue_max = (UINT32_C(1) << fb->variable.blue.length) - 1;
            rgba[0] = (uint8_t)(
                ((pixel >> fb->variable.red.offset) & red_max) * 255 / red_max);
            rgba[1] = (uint8_t)(((pixel >> fb->variable.green.offset) &
                                 green_max) * 255 / green_max);
            rgba[2] = (uint8_t)(((pixel >> fb->variable.blue.offset) &
                                 blue_max) * 255 / blue_max);
            rgba[3] = 255;
        }
    }

    bool okay = png_image_write_to_file(&png, path, 0, pixels, 0, NULL);
    if (!okay)
        fprintf(stderr, "GnuChanPlymouth: could not write framebuffer capture "
                        "%s: %s\n", path, png.message);
    else
        fprintf(stderr, "GnuChanPlymouth: captured framebuffer to %s\n", path);
    free(pixels);
    png_image_free(&png);
    return okay;
}

static bool parse_arguments(int argc, char **argv, const char **config_dir,
                            const char **sysfs_root, const char **cmdline_path,
                            const char **capture_path, bool *detect_only,
                            bool *probe_drm, unsigned *duration_override)
{
    *config_dir = "/etc/gnuchan-plymouth";
    *sysfs_root = "/sys";
    *cmdline_path = "/proc/cmdline";
    *capture_path = NULL;
    *detect_only = false;
    *probe_drm = false;
    *duration_override = 0;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--detect-gpu") == 0) {
            *detect_only = true;
        } else if (strcmp(argv[i], "--probe-drm") == 0) {
            *probe_drm = true;
        } else if ((strcmp(argv[i], "--config-dir") == 0 ||
                    strcmp(argv[i], "--sysfs-root") == 0 ||
                    strcmp(argv[i], "--proc-cmdline") == 0 ||
                    strcmp(argv[i], "--capture") == 0 ||
                    strcmp(argv[i], "--duration-ms") == 0) &&
                   i + 1 < argc) {
            const char *option = argv[i++];
            const char *value = argv[i];
            if (strcmp(option, "--config-dir") == 0)
                *config_dir = value;
            else if (strcmp(option, "--sysfs-root") == 0)
                *sysfs_root = value;
            else if (strcmp(option, "--proc-cmdline") == 0)
                *cmdline_path = value;
            else if (strcmp(option, "--capture") == 0)
                *capture_path = value;
            else if (!parse_unsigned(value, MAX_DURATION_MS,
                                     duration_override) ||
                     *duration_override == 0)
                return false;
        } else {
            return false;
        }
    }
    return true;
}

static int probe_drm_device(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "GnuChanPlymouth: cannot open DRM device %s: %s\n",
                path, strerror(errno));
        return 1;
    }

    drmModeResPtr resources = drmModeGetResources(fd);
    if (resources == NULL) {
        report_error("DRM device did not return display resources");
        close(fd);
        return 1;
    }

    bool connected = false;
    for (int i = 0; i < resources->count_connectors; ++i) {
        drmModeConnectorPtr connector =
            drmModeGetConnector(fd, resources->connectors[i]);
        if (connector == NULL)
            continue;
        if (connector->connection == DRM_MODE_CONNECTED &&
            connector->count_modes > 0) {
            connected = true;
            printf("%s-%u: connected\n", drmModeGetConnectorTypeName(
                       connector->connector_type), connector->connector_type_id);
            for (int j = 0; j < connector->count_modes; ++j) {
                printf("  %ux%u%s\n", connector->modes[j].hdisplay,
                       connector->modes[j].vdisplay,
                       (connector->modes[j].type & DRM_MODE_TYPE_PREFERRED)
                           ? " preferred" : "");
            }
        }
        drmModeFreeConnector(connector);
    }
    drmModeFreeResources(resources);
    close(fd);
    if (!connected) {
        report_error("DRM device has no connected display modes");
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    const char *config_dir;
    const char *sysfs_root;
    const char *cmdline_path;
    const char *capture_path;
    bool detect_only;
    bool probe_drm;
    unsigned duration_override;
    if (!parse_arguments(argc, argv, &config_dir, &sysfs_root, &cmdline_path,
                         &capture_path, &detect_only, &probe_drm,
                         &duration_override)) {
        report_error("usage: GnuChanPlymouth [--detect-gpu] [--probe-drm] "
                     "[--config-dir DIR] "
                     "[--sysfs-root DIR] [--proc-cmdline FILE] "
                     "[--duration-ms MS] [--capture PNG]");
        return 2;
    }
    if (probe_drm) {
        char candidates[32][MAX_CONFIG_VALUE];
        size_t candidate_count = 0;
        char drm_class[1024];
        if (snprintf(drm_class, sizeof(drm_class), "%s/class/drm", sysfs_root) <
            (int)sizeof(drm_class)) {
            DIR *devices = opendir(drm_class);
            if (devices != NULL) {
                struct dirent *entry;
                while ((entry = readdir(devices)) != NULL) {
                    if (!is_drm_card_name(entry->d_name))
                        continue;
                    char boot_vga_path[1400];
                    if (snprintf(boot_vga_path, sizeof(boot_vga_path),
                                 "%s/%s/device/boot_vga",
                                 drm_class, entry->d_name) >=
                        (int)sizeof(boot_vga_path))
                        continue;
                    char boot_vga[8];
                    if (!read_text_file(boot_vga_path, boot_vga,
                                        sizeof(boot_vga)) ||
                        boot_vga[0] != '1')
                        continue;
                    char card_path[MAX_CONFIG_VALUE];
                    if (snprintf(card_path, sizeof(card_path), "/dev/dri/%s",
                                 entry->d_name) < (int)sizeof(card_path))
                        append_drm_candidate(candidates, &candidate_count,
                                             card_path);
                }
                closedir(devices);
            }
        }
        append_drm_candidate(candidates, &candidate_count, "/dev/dri/card0");
        if (snprintf(drm_class, sizeof(drm_class), "%s/class/drm", sysfs_root) <
            (int)sizeof(drm_class)) {
            DIR *devices = opendir(drm_class);
            if (devices != NULL) {
                struct dirent *entry;
                while ((entry = readdir(devices)) != NULL) {
                    if (!is_drm_card_name(entry->d_name))
                        continue;
                    char card_path[MAX_CONFIG_VALUE];
                    if (snprintf(card_path, sizeof(card_path), "/dev/dri/%s",
                                 entry->d_name) < (int)sizeof(card_path))
                        append_drm_candidate(candidates, &candidate_count,
                                             card_path);
                }
                closedir(devices);
            }
        }
        for (size_t i = 0; i < candidate_count; ++i) {
            if (probe_drm_device(candidates[i]) == 0)
                return 0;
        }
        report_error("no DRM card could be probed with a connected display mode");
        return 1;
    }

    char gpu[32];
    if (!detect_gpu(sysfs_root, cmdline_path,
                    gpu, sizeof(gpu))) {
        report_error("could not detect a supported Intel, AMD, or NVIDIA GPU");
        return 1;
    }
    if (detect_only) {
        puts(gpu);
        return 0;
    }

    Config config;
    if (!load_config(config_dir, gpu, &config))
        return 1;
    if (duration_override != 0)
        config.duration_ms = duration_override;

    Image background = {0};
    Image logo = {0};
    if (!load_png(config.background, &background))
        return 1;
    if (!load_png(config.logo, &logo)) {
        free_image(&background);
        return 1;
    }

    Framebuffer framebuffer = {.fd = -1};
    if (!open_framebuffer(sysfs_root, config.drm_device,
                          config.wait_for_framebuffer_ms,
                          &framebuffer)) {
        free_image(&background);
        free_image(&logo);
        return 1;
    }

    fprintf(stderr, "GnuChanPlymouth: %s profile, %ux%u framebuffer, %u ms\n",
            gpu, framebuffer.variable.xres, framebuffer.variable.yres,
            config.duration_ms);
    fprintf(stderr, "GnuChanPlymouth: background %ux%u, logo %ux%u; "
                    "RGB offsets %u/%u/%u, bits per pixel %u\n",
            background.width, background.height, logo.width, logo.height,
            framebuffer.variable.red.offset, framebuffer.variable.green.offset,
            framebuffer.variable.blue.offset,
            framebuffer.variable.bits_per_pixel);
    clear_framebuffer(&framebuffer);
    draw_frame(&framebuffer, &background, &logo, 0);
    if (capture_path != NULL && !capture_framebuffer(&framebuffer, capture_path)) {
        close_framebuffer(&framebuffer);
        free_image(&background);
        free_image(&logo);
        return 1;
    }
    if (!present_framebuffer(&framebuffer)) {
        close_framebuffer(&framebuffer);
        free_image(&background);
        free_image(&logo);
        return 1;
    }
    struct timespec animation_start;
    clock_gettime(CLOCK_MONOTONIC, &animation_start);
    unsigned frame = 1;
    const uint64_t frame_interval_ns = 1000000000ULL / config.fps;
    uint64_t elapsed_ms = 0;
    while (elapsed_ms < config.duration_ms) {
        struct timespec frame_start;
        clock_gettime(CLOCK_MONOTONIC, &frame_start);
        draw_frame(&framebuffer, &background, &logo, frame++);
        __sync_synchronize();
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t frame_elapsed_ns =
            (int64_t)(now.tv_sec - frame_start.tv_sec) * 1000000000 +
            now.tv_nsec - frame_start.tv_nsec;
        uint64_t sleep_ns = frame_elapsed_ns >= 0 &&
                                    (uint64_t)frame_elapsed_ns < frame_interval_ns
                                ? frame_interval_ns - (uint64_t)frame_elapsed_ns
                                : 0;
        if (sleep_ns != 0) {
            struct timespec delay = {
                .tv_sec = (time_t)(sleep_ns / 1000000000ULL),
                .tv_nsec = (long)(sleep_ns % 1000000000ULL)
            };
            nanosleep(&delay, NULL);
        }
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t elapsed_ns =
            (int64_t)(now.tv_sec - animation_start.tv_sec) * 1000000000 +
            now.tv_nsec - animation_start.tv_nsec;
        elapsed_ms = elapsed_ns > 0 ? (uint64_t)elapsed_ns / 1000000 : 0;
    }

    close_framebuffer(&framebuffer);
    free_image(&background);
    free_image(&logo);
    return 0;
}
