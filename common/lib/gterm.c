#include <stdint.h>
#include <stddef.h>
#include <lib/gterm.h>
#include <lib/misc.h>
#include <lib/libc.h>
#include <lib/config.h>
#include <lib/print.h>
#include <lib/uri.h>
#include <lib/fb.h>
#include <mm/pmm.h>
#include <term/term.h>
#include <term/backends/framebuffer.h>
#include <lib/term.h>

struct fb_info fbinfo;

extern symbol _binary_font_bin_start, _binary_font_bin_size;

static struct image *background;

static size_t margin = 64;
static size_t margin_gradient = 4;

static uint32_t default_bg, default_fg;

static size_t bg_canvas_size;
static uint32_t *bg_canvas;

#define A(rgb) (uint8_t)(rgb >> 24)
#define R(rgb) (uint8_t)(rgb >> 16)
#define G(rgb) (uint8_t)(rgb >> 8)
#define B(rgb) (uint8_t)(rgb)
#define ARGB(a, r, g, b) (a << 24) | ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF)

static inline uint32_t colour_blend(uint32_t fg, uint32_t bg) {
    unsigned alpha = 255 - A(fg);
    unsigned inv_alpha = A(fg) + 1;

    uint8_t r = (uint8_t)((alpha * R(fg) + inv_alpha * R(bg)) / 256);
    uint8_t g = (uint8_t)((alpha * G(fg) + inv_alpha * G(bg)) / 256);
    uint8_t b = (uint8_t)((alpha * B(fg) + inv_alpha * B(bg)) / 256);

    return ARGB(0, r, g, b);
}

static uint32_t blend_gradient_from_box(size_t x, size_t y, uint32_t bg_px, uint32_t hex) {
    size_t distance, x_distance, y_distance;
    size_t gradient_stop_x = fbinfo.framebuffer_width - margin;
    size_t gradient_stop_y = fbinfo.framebuffer_height - margin;

    if (x < margin)
        x_distance = margin - x;
    else
        x_distance = x - gradient_stop_x;

    if (y < margin)
        y_distance = margin - y;
    else
        y_distance = y - gradient_stop_y;

    if (x >= margin && x < gradient_stop_x) {
        distance = y_distance;
    } else if (y >= margin && y < gradient_stop_y) {
        distance = x_distance;
    } else {
        distance = sqrt((uint64_t)x_distance * (uint64_t)x_distance
                      + (uint64_t)y_distance * (uint64_t)y_distance);
    }

    if (distance > margin_gradient)
        return bg_px;

    uint8_t gradient_step = (0xff - A(hex)) / margin_gradient;
    uint8_t new_alpha     = A(hex) + gradient_step * distance;

    return colour_blend((hex & 0xffffff) | (new_alpha << 24), bg_px);
}

typedef size_t fixedp6; // the last 6 bits are the fixed point part
static size_t fixedp6_to_int(fixedp6 value) { return value / 64; }
static fixedp6 int_to_fixedp6(size_t value) { return value * 64; }

// Draw rect at coordinates, copying from the image to the fb and canvas, applying fn on every pixel
__attribute__((always_inline)) static inline void genloop(size_t xstart, size_t xend, size_t ystart, size_t yend, uint32_t (*blend)(size_t x, size_t y, uint32_t orig)) {
    uint8_t *img = background->img;
    const size_t img_width = background->img_width, img_height = background->img_height, img_pitch = background->pitch, colsize = background->bpp / 8;

    switch (background->type) {
    case IMAGE_TILED:
        for (size_t y = ystart; y < yend; y++) {
            size_t image_y = y % img_height, image_x = xstart % img_width;
            const size_t off = img_pitch * (img_height - 1 - image_y);
            size_t canvas_off = fbinfo.framebuffer_width * y;
            for (size_t x = xstart; x < xend; x++) {
                uint32_t img_pixel = *(uint32_t*)(img + image_x * colsize + off);
                uint32_t i = blend(x, y, img_pixel);
                bg_canvas[canvas_off + x] = i;
                if (image_x++ == img_width) image_x = 0; // image_x = x % img_width, but modulo is too expensive
            }
        }
        break;

    case IMAGE_CENTERED:
        for (size_t y = ystart; y < yend; y++) {
            size_t image_y = y - background->y_displacement;
            const size_t off = img_pitch * (img_height - 1 - image_y);
            size_t canvas_off = fbinfo.framebuffer_width * y;
            if (image_y >= background->y_size) { /* external part */
                for (size_t x = xstart; x < xend; x++) {
                    uint32_t i = blend(x, y, background->back_colour);
                    bg_canvas[canvas_off + x] = i;
                }
            }
            else { /* internal part */
                for (size_t x = xstart; x < xend; x++) {
                    size_t image_x = (x - background->x_displacement);
                    bool x_external = image_x >= background->x_size;
                    uint32_t img_pixel = *(uint32_t*)(img + image_x * colsize + off);
                    uint32_t i = blend(x, y, x_external ? background->back_colour : img_pixel);
                    bg_canvas[canvas_off + x] = i;
                }
            }
        }
        break;
    // For every pixel, ratio = img_width / gterm_width, img_x = x * ratio, x = (xstart + i)
    // hence x = xstart * ratio + i * ratio
    // so you can set x = xstart * ratio, and increment by ratio at each iteration
    case IMAGE_STRETCHED:
        for (size_t y = ystart; y < yend; y++) {
            size_t img_y = (y * img_height) / fbinfo.framebuffer_height; // calculate Y with full precision
            size_t off = img_pitch * (img_height - 1 - img_y);
            size_t canvas_off = fbinfo.framebuffer_width * y;

            size_t ratio = int_to_fixedp6(img_width) / fbinfo.framebuffer_width;
            fixedp6 img_x = ratio * xstart;
            for (size_t x = xstart; x < xend; x++) {
                uint32_t img_pixel = *(uint32_t*)(img + fixedp6_to_int(img_x) * colsize + off);
                uint32_t i = blend(x, y, img_pixel);
                bg_canvas[canvas_off + x] = i;
                img_x += ratio;
            }
        }
        break;
    }
}

static uint32_t blend_external(size_t x, size_t y, uint32_t orig) { (void)x; (void)y; return orig; }
static uint32_t blend_internal(size_t x, size_t y, uint32_t orig) { (void)x; (void)y; return colour_blend(default_bg, orig); }
static uint32_t blend_margin(size_t x, size_t y, uint32_t orig) { return blend_gradient_from_box(x, y, orig, default_bg); }

static void loop_external(size_t xstart, size_t xend, size_t ystart, size_t yend) { genloop(xstart, xend, ystart, yend, blend_external); }
static void loop_margin(size_t xstart, size_t xend, size_t ystart, size_t yend) { genloop(xstart, xend, ystart, yend, blend_margin); }
static void loop_internal(size_t xstart, size_t xend, size_t ystart, size_t yend) { genloop(xstart, xend, ystart, yend, blend_internal); }

static void *generate_canvas(void) {
    if (background) {
        bg_canvas_size = fbinfo.framebuffer_width * fbinfo.framebuffer_height * sizeof(uint32_t);
        bg_canvas = ext_mem_alloc(bg_canvas_size);

        int64_t margin_no_gradient = (int64_t)margin - margin_gradient;

        if (margin_no_gradient < 0) {
            margin_no_gradient = 0;
        }

        size_t scan_stop_x = fbinfo.framebuffer_width - margin_no_gradient;
        size_t scan_stop_y = fbinfo.framebuffer_height - margin_no_gradient;

        loop_external(0, fbinfo.framebuffer_width, 0, margin_no_gradient);
        loop_external(0, fbinfo.framebuffer_width, scan_stop_y, fbinfo.framebuffer_height);
        loop_external(0, margin_no_gradient, margin_no_gradient, scan_stop_y);
        loop_external(scan_stop_x, fbinfo.framebuffer_width, margin_no_gradient, scan_stop_y);

        size_t gradient_stop_x = fbinfo.framebuffer_width - margin;
        size_t gradient_stop_y = fbinfo.framebuffer_height - margin;

        if (margin_gradient) {
            loop_margin(margin_no_gradient, scan_stop_x, margin_no_gradient, margin);
            loop_margin(margin_no_gradient, scan_stop_x, gradient_stop_y, scan_stop_y);
            loop_margin(margin_no_gradient, margin, margin, gradient_stop_y);
            loop_margin(gradient_stop_x, scan_stop_x, margin, gradient_stop_y);
        }

        loop_internal(margin, gradient_stop_x, margin, gradient_stop_y);

        return bg_canvas;
    }

    return NULL;
}

static bool last_serial = false;
static char *last_config = NULL;

bool gterm_init(char *config, size_t width, size_t height) {
    if (term_backend != GTERM) {
        term->deinit(term, pmm_free);
    }

    if (quiet || allocations_disallowed) {
        return false;
    }

    if (current_video_mode >= 0
#if defined (BIOS)
     && current_video_mode != 0x03
#endif
     && fbinfo.default_res == true
     && width == 0
     && height == 0
     && fbinfo.framebuffer_bpp == 32
     && serial == last_serial
     && config == last_config) {
        term->clear(term, true);
        return true;
    }

    if (current_video_mode >= 0
#if defined (BIOS)
     && current_video_mode != 0x03
#endif
     && fbinfo.framebuffer_width == width
     && fbinfo.framebuffer_height == height
     && fbinfo.framebuffer_bpp == 32
     && serial == last_serial
     && config == last_config) {
        term->clear(term, true);
        return true;
    }

    // We force bpp to 32
    if (!fb_init(&fbinfo, width, height, 32))
        return false;

    // Ensure this is xRGB8888, we only support that for the menu
    if (fbinfo.red_mask_size    != 8
     || fbinfo.red_mask_shift   != 16
     || fbinfo.green_mask_size  != 8
     || fbinfo.green_mask_shift != 8
     || fbinfo.blue_mask_size   != 8
     || fbinfo.blue_mask_shift  != 0)
        return false;

    last_serial = serial;

    // default scheme
    margin = 64;
    margin_gradient = 4;

    uint32_t ansi_colours[8];

    ansi_colours[0] = 0x00000000; // black
    ansi_colours[1] = 0x00aa0000; // red
    ansi_colours[2] = 0x0000aa00; // green
    ansi_colours[3] = 0x00aa5500; // brown
    ansi_colours[4] = 0x000000aa; // blue
    ansi_colours[5] = 0x00aa00aa; // magenta
    ansi_colours[6] = 0x0000aaaa; // cyan
    ansi_colours[7] = 0x00aaaaaa; // grey

    char *colours = config_get_value(config, 0, "TERM_PALETTE");
    if (colours != NULL) {
        const char *first = colours;
        size_t i;
        for (i = 0; i < 8; i++) {
            const char *last;
            uint32_t col = strtoui(first, &last, 16);
            if (first == last)
                break;
            ansi_colours[i] = col & 0xffffff;
            if (*last == 0)
                break;
            first = last + 1;
        }
    }

    uint32_t ansi_bright_colours[8];

    ansi_bright_colours[0] = 0x00555555; // black
    ansi_bright_colours[1] = 0x00ff5555; // red
    ansi_bright_colours[2] = 0x0055ff55; // green
    ansi_bright_colours[3] = 0x00ffff55; // brown
    ansi_bright_colours[4] = 0x005555ff; // blue
    ansi_bright_colours[5] = 0x00ff55ff; // magenta
    ansi_bright_colours[6] = 0x0055ffff; // cyan
    ansi_bright_colours[7] = 0x00ffffff; // grey

    char *bright_colours = config_get_value(config, 0, "TERM_PALETTE_BRIGHT");
    if (bright_colours != NULL) {
        const char *first = bright_colours;
        size_t i;
        for (i = 0; i < 8; i++) {
            const char *last;
            uint32_t col = strtoui(first, &last, 16);
            if (first == last)
                break;
            ansi_bright_colours[i] = col & 0xffffff;
            if (*last == 0)
                break;
            first = last + 1;
        }
    }

    default_bg = 0x00000000; // background (black)
    default_fg = 0x00aaaaaa; // foreground (grey)

    char *theme_background = config_get_value(config, 0, "TERM_BACKGROUND");
    if (theme_background != NULL) {
        default_bg = strtoui(theme_background, NULL, 16);
    }

    char *theme_foreground = config_get_value(config, 0, "TERM_FOREGROUND");
    if (theme_foreground != NULL) {
        default_fg = strtoui(theme_foreground, NULL, 16) & 0xffffff;
    }

    background = NULL;
    char *background_path = config_get_value(config, 0, "TERM_WALLPAPER");
    if (background_path != NULL) {
        struct file_handle *bg_file;
        if ((bg_file = uri_open(background_path)) != NULL) {
            background = image_open(bg_file);
            fclose(bg_file);
        }
    }

    if (background == NULL) {
        margin = 0;
        margin_gradient = 0;
    } else {
        if (theme_background == NULL) {
            default_bg = 0x80000000;
        }
    }

    char *theme_margin = config_get_value(config, 0, "TERM_MARGIN");
    if (theme_margin != NULL) {
        margin = strtoui(theme_margin, NULL, 10);
    }

    char *theme_margin_gradient = config_get_value(config, 0, "TERM_MARGIN_GRADIENT");
    if (theme_margin_gradient != NULL) {
        margin_gradient = strtoui(theme_margin_gradient, NULL, 10);
    }

    if (background != NULL) {
        char *background_layout = config_get_value(config, 0, "TERM_WALLPAPER_STYLE");
        if (background_layout != NULL && strcmp(background_layout, "centered") == 0) {
            char *background_colour = config_get_value(config, 0, "TERM_BACKDROP");
            if (background_colour == NULL)
                background_colour = "0";
            uint32_t bg_col = strtoui(background_colour, NULL, 16);
            image_make_centered(background, fbinfo.framebuffer_width, fbinfo.framebuffer_height, bg_col);
        } else if (background_layout != NULL && strcmp(background_layout, "tiled") == 0) {
        } else {
            image_make_stretched(background, fbinfo.framebuffer_width, fbinfo.framebuffer_height);
        }
    }

    size_t font_width = 8;
    size_t font_height = 16;
    size_t font_size = (font_width * font_height * FBTERM_FONT_GLYPHS) / 8;

#define FONT_MAX 16384
    uint8_t *font = ext_mem_alloc(FONT_MAX);

    memcpy(font, (void *)_binary_font_bin_start, (uintptr_t)_binary_font_bin_size);

    size_t tmp_font_width, tmp_font_height;

    char *menu_font_size = config_get_value(config, 0, "TERM_FONT_SIZE");
    if (menu_font_size != NULL) {
        parse_resolution(&tmp_font_width, &tmp_font_height, NULL, menu_font_size);

        size_t tmp_font_size = (tmp_font_width * tmp_font_height * FBTERM_FONT_GLYPHS) / 8;

        if (tmp_font_size > FONT_MAX) {
            print("Font would be too large (%u bytes, %u bytes allowed). Not loading.\n", tmp_font_size, FONT_MAX);
            goto no_load_font;
        }

        font_size = tmp_font_size;
    }

    char *menu_font = config_get_value(config, 0, "TERM_FONT");
    if (menu_font != NULL) {
        struct file_handle *f;
        if ((f = uri_open(menu_font)) == NULL) {
            print("menu: Could not open font file.\n");
        } else {
            fread(f, font, 0, font_size);
            if (menu_font_size != NULL) {
                font_width = tmp_font_width;
                font_height = tmp_font_height;
            }
            fclose(f);
        }
    }

no_load_font:;
    size_t font_spacing = 1;
    char *font_spacing_str = config_get_value(config, 0, "TERM_FONT_SPACING");
    if (font_spacing_str != NULL) {
        font_spacing = strtoui(font_spacing_str, NULL, 10);
    }

    size_t font_scale_x = 1;
    size_t font_scale_y = 1;

    char *menu_font_scale = config_get_value(config, 0, "TERM_FONT_SCALE");
    if (menu_font_scale != NULL) {
        parse_resolution(&font_scale_x, &font_scale_y, NULL, menu_font_scale);
        if (font_scale_x > 8 || font_scale_y > 8) {
            font_scale_x = 1;
            font_scale_y = 1;
        }
    }

    uint32_t *canvas = generate_canvas();

    term->deinit(term, pmm_free);

    term = fbterm_init(ext_mem_alloc,
                (void *)(uintptr_t)fbinfo.framebuffer_addr,
                fbinfo.framebuffer_width, fbinfo.framebuffer_height, fbinfo.framebuffer_pitch,
                canvas,
                ansi_colours, ansi_bright_colours,
                &default_bg, &default_fg,
                font, font_width, font_height, font_spacing,
                font_scale_x, font_scale_y,
                margin);

    if (serial) {
        term->cols = term->cols > 80 ? 80 : term->cols;
        term->rows = term->rows > 24 ? 24 : term->rows;
    }

    term_context_reinit(term);

    term_backend = GTERM;

    term->in_bootloader = true;

    return true;
}
