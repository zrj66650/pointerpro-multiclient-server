#include "animate.h"

#include <stdio.h>

#include <stdlib.h>


struct sprite {
    size_t width;
    size_t height;
    color_t* pixels;

    int ref_count;     // how many placements are using (key point)

};

struct sprite_placement {
    struct sprite* sprite;

    ssize_t x;
    ssize_t y;

    ssize_t vx;
    ssize_t vy;
    ssize_t ax;
    ssize_t ay;

    struct sprite_placement* prev;
    struct sprite_placement* next;

    struct canvas* canvas;
};

struct canvas {
    size_t width;
    size_t height;
    color_t background;

    struct sprite_placement* head; // bottom
    struct sprite_placement* tail; // top
};


/* This is the header structure that we can expect to find at position 0 in a bitmap file. */
struct bitmap_header {
    uint8_t  magic[2];          // Expect {'B', 'M'}
    uint32_t size_bytes;        // Size of the file in bytes
    uint16_t reserved[2];
    uint32_t pixel_offset;      // Starting address of pixel data
// Don't pad this struct for alignment
} __attribute__((packed));

/*
 * This header immediately follows bitmap_header in the file.
 * You may ignore all fields except bV5Width and bV5Height, unless you'd like to validate
 * the image format
 */
struct bitmapv5_header {
                               // Offset     Size Description
    uint32_t bV5Size         ; // 0x00          4 Size of this header (124 bytes)
    uint32_t bV5Width        ; // 0x04          4 Width of the bitmap in pixels
    uint32_t bV5Height       ; // 0x08          4 Height of the bitmap in pixels
    uint16_t bV5Planes       ; // 0x0C          2 Number of planes (must be 1)
    uint16_t bV5BitCount     ; // 0x0E          2 Bits per pixel (e.g., 32)
    uint32_t bV5Compression  ; // 0x10          4 BI_RGB (0), BI_BITFIELDS (3)
    uint32_t bV5SizeImage    ; // 0x14          4 Size of image data (0 if uncompressed)
    uint32_t bV5XPelsPerMeter; // 0x18          4 Horizontal pixels per meter
    uint32_t bV5YPelsPerMeter; // 0x1C          4 Vertical pixels per meter
    uint32_t bV5ClrUsed      ; // 0x20          4 Number of color indices used
    uint32_t bV5ClrImportant ; // 0x24          4 Number of important colors
    uint32_t bV5RedMask      ; // 0x28          4 Color mask for red component
    uint32_t bV5GreenMask    ; // 0x2C          4 Color mask for green component
    uint32_t bV5BlueMask     ; // 0x30          4 Color mask for blue component
    uint32_t bV5AlphaMask    ; // 0x34          4 Color mask for alpha channel
    uint32_t bV5CSType       ; // 0x38          4 Color space type (e.g., LCS_CALIBRATED_RGB)
    uint8_t  bV5Endpoints[36]; // 0x3C-0x5B    36 CIE XYZ color space endpoints
    uint32_t bV5GammaRed     ; // 0x5C          4 Gamma red component
    uint32_t bV5GammaGreen   ; // 0x60          4 Gamma green component
    uint32_t bV5GammaBlue    ; // 0x64          4 Gamma blue component
    uint32_t bV5Intent       ; // 0x68          4 Rendering intent
    uint32_t bV5ProfileData  ; // 0x6C          4 Offset to ICC profile data
    uint32_t bV5ProfileSize  ; // 0x70          4 Size of embedded profile data
    uint32_t bV5Reserved     ; // 0x74          4 Reserved (must be 0)
};


struct canvas* animate_create_canvas(size_t height, size_t width,
                                     color_t background_color){
    struct canvas* c = malloc(sizeof(struct canvas));
    if (c == NULL) return NULL;

    c->width = width;
    c->height = height;
    c->background = background_color;

    c->head = NULL;
    c->tail = NULL;

    return c;
}

struct sprite* animate_create_sprite(const char* file) {

    if (file == NULL) return NULL;

    FILE* fp = fopen(file, "rb");
    if (fp == NULL) return NULL;

    // read bitmap header
    struct bitmap_header bh;
    if (fread(&bh, sizeof(bh), 1, fp) != 1) {
        fclose(fp);
        return NULL;
    }

    // check BM
    if (bh.magic[0] != 'B' || bh.magic[1] != 'M') {
        fclose(fp);
        return NULL;
    }

    // read bitmap v5 header
    struct bitmapv5_header b5;
    if (fread(&b5, sizeof(b5), 1, fp) != 1) {
        fclose(fp);
        return NULL;
    }

    size_t width = b5.bV5Width;
    size_t height = b5.bV5Height;

    // create sprite
    struct sprite* s = malloc(sizeof(struct sprite));
    if (s == NULL) {
        fclose(fp);
        return NULL;
    }

    s->width = width;
    s->height = height;
    s->ref_count = 0;

    s->pixels = malloc(width * height * sizeof(color_t));
    if (s->pixels == NULL) {
        free(s);
        fclose(fp);
        return NULL;
    }

    // jump to pixel data location
    fseek(fp, bh.pixel_offset, SEEK_SET);

    // read from bottom to top
    for (size_t row = 0; row < height; row++) {

        color_t* temp_row = malloc(width * sizeof(color_t));
        if (temp_row == NULL) {
            free(s->pixels);
            free(s);
            fclose(fp);
            return NULL;
        }

        if (fread(temp_row, sizeof(color_t), width, fp) != width) {
            free(temp_row);
            free(s->pixels);
            free(s);
            fclose(fp);
            return NULL;
        }

        // write into sprite
        size_t target_row = height - 1 - row;

        for (size_t col = 0; col < width; col++) {
            s->pixels[target_row * width + col] = temp_row[col];
        }

        free(temp_row);
    }

    fclose(fp);
    return s;
}

struct sprite* animate_create_circle(size_t radius, color_t c, bool filled) {
    size_t size = 2 * radius + 1;

    struct sprite* s = malloc(sizeof(struct sprite));
    if (s == NULL) return NULL;

    s->width = size;
    s->height = size;
    s->ref_count = 0;

    s->pixels = malloc(size * size * sizeof(color_t));
    if (s->pixels == NULL) {
        free(s);
        return NULL;
    }

    for (int y = -radius; y <= (int)radius; y++) {
        for (int x = -radius; x <= (int)radius; x++) {
            int idx = (y + radius) * size + (x + radius);

            if (x*x + y*y <= radius*radius) {
                s->pixels[idx] = c;
            } else {
                s->pixels[idx] = 0; // alpha = 0 means transparent
            }
        }
    }

    return s;
}

struct sprite* animate_create_rectangle(size_t width, size_t height,
                                        color_t c, bool filled) {

    struct sprite* s = malloc(sizeof(struct sprite));
    if (s == NULL) return NULL;

    s->width = width;
    s->height = height;
    s->ref_count = 0;

    s->pixels = malloc(width * height * sizeof(color_t));
    if (s->pixels == NULL) {
        free(s);
        return NULL;
    }

    // determine pixels based on filled
    for (size_t y = 0; y < height; y++) {
        for (size_t x = 0; x < width; x++) {
            if (filled) {
                s->pixels[y * width + x] = c;
            } else {
                if (y == 0 || y == height - 1 || x == 0 || x == width - 1) {
                    s->pixels[y * width + x] = c;
                } else {
                    s->pixels[y * width + x] = 0;
                }
            }
        }
    }

    return s;
}

bool animate_destroy_sprite(struct sprite* sprite) {
    if (sprite == NULL) return 1;
    if (sprite->ref_count > 0) return 1;

    free(sprite->pixels);
    free(sprite);
    return 0;
}

struct sprite_placement* animate_place_sprite(struct canvas* canvas,
                                              struct sprite* sprite,
                                              ssize_t x, ssize_t y) {

    struct sprite_placement* p = malloc(sizeof(struct sprite_placement));
    if (p == NULL) return NULL;

    p->sprite = sprite;
    p->x = x;
    p->y = y;

    p->vx = 0;
    p->vy = 0;
    p->ax = 0;
    p->ay = 0;

    p->prev = canvas->tail;
    p->next = NULL;
    p->canvas = canvas;

    if (canvas->tail != NULL) {
        canvas->tail->next = p;
    } else {
        canvas->head = p;
    }

    canvas->tail = p;

    sprite->ref_count++; // record using

    return p;
}

void animate_placement_up(struct sprite_placement* sprite_placement){
// TODO COMP9017
}

void animate_placement_down(struct sprite_placement* sprite_placement){
// TODO COMP9017
}

void animate_placement_top(struct sprite_placement* p){

    if (p == NULL || p->canvas == NULL) return;

    struct canvas* c = p->canvas;

    // already at the top
    if (c->tail == p) return;

    // remove from current location
    if (p->prev) p->prev->next = p->next;
    else c->head = p->next;

    if (p->next) p->next->prev = p->prev;

    // insert to the tail
    p->prev = c->tail;
    p->next = NULL;

    if (c->tail) c->tail->next = p;
    c->tail = p;

    if (c->head == NULL) c->head = p;
}

void animate_placement_bottom(struct sprite_placement* p){

    if (p == NULL || p->canvas == NULL) return;

    struct canvas* c = p->canvas;

    // already at the bottom
    if (c->head == p) return;

    // remove from current location
    if (p->prev) p->prev->next = p->next;
    if (p->next) p->next->prev = p->prev;
    else c->tail = p->prev;

    // insert it into the head
    p->prev = NULL;
    p->next = c->head;

    if (c->head) c->head->prev = p;
    c->head = p;

    if (c->tail == NULL) c->tail = p;
}

void animate_destroy_placement(struct sprite_placement* p){

    if (p == NULL || p->canvas == NULL) return;

    struct canvas* c = p->canvas;

    // remove from linked list
    if (p->prev) p->prev->next = p->next;
    else c->head = p->next;

    if (p->next) p->next->prev = p->prev;
    else c->tail = p->prev;

    p->sprite->ref_count--; // key point

    free(p);
}

void animate_set_animation_params(struct sprite_placement* p,
                                  ssize_t vx, ssize_t vy,
                                  ssize_t ax, ssize_t ay){
    if (p == NULL) return;

    p->vx = vx;
    p->vy = vy;
    p->ax = ax;
    p->ay = ay;
}

void animate_destroy_canvas(struct canvas* canvas){

    if (canvas == NULL) return;

    struct sprite_placement* p = canvas->head;

    while (p != NULL) {
        struct sprite_placement* next = p->next;
        animate_destroy_placement(p);
        p = next;
    }

    free(canvas);
}

size_t animate_frame_size_bytes(struct canvas* canvas){
    if (canvas == NULL) return 0;

    return canvas->width * canvas->height * sizeof(color_t);
}

void animate_generate_frame(const struct canvas* canvas, size_t frame,
                            size_t frame_rate, void* buf) {

    color_t* framebuf = (color_t*)buf;

    size_t total = canvas->width * canvas->height;

    // fill background
    color_t bg_full_alpha = (canvas->background & 0x00FFFFFF) | 0xFF000000;
    for (size_t i = 0; i < total; i++) {
        framebuf[i] = bg_full_alpha;
    }

    double t = (double)frame / frame_rate;

    // traverse all placements
    struct sprite_placement* p = canvas->head;

    while (p != NULL) {

        // calculate position
        ssize_t x = (ssize_t)(p->x + p->vx * t + 0.5 * p->ax * t * t);
        ssize_t y = (ssize_t)(p->y + p->vy * t + 0.5 * p->ay * t * t);

        struct sprite* s = p->sprite;

        // copy sprite
        for (size_t sy = 0; sy < s->height; sy++) {
            for (size_t sx = 0; sx < s->width; sx++) {

                ssize_t fx = x + sx;
                ssize_t fy = y + sy;

                // boundary check
                if (fx < 0 || fy < 0 ||
                    fx >= (ssize_t)canvas->width ||
                    fy >= (ssize_t)canvas->height) {
                    continue;
                }

                color_t pixel = s->pixels[sy * s->width + sx];

                // alpha != 0
                if ((pixel >> 24) != 0) {
                    framebuf[fy * canvas->width + fx] = pixel | 0xFF000000;
                }
            }
        }

        p = p->next;
    }
}

size_t animate_get_width(struct canvas *canvas) {
    if (canvas == NULL) return 0;
    return canvas->width;
}

size_t animate_get_height(struct canvas *canvas) {
    if (canvas == NULL) return 0;
    return canvas->height;
}

// Optional extension
void animate_set_animation_function(struct sprite_placement* sprite_placement,
                                   animate_fn, void* priv) {
}