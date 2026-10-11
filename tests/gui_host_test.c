/* Host-only behavioral checks; no MCU, HAL, SPI or display is involved. */
#include "../gui/GUI_Paint.h"
#include "../app/epd_clock_view.h"

#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GUARD_BYTES 32U
#define MAX_IMAGE_BYTES 20000U
#define NATIVE_WIDTH 128U
#define NATIVE_HEIGHT 296U
#define FRAME_BYTES ((NATIVE_WIDTH / 8U) * NATIVE_HEIGHT)

typedef struct {
    uint8_t memory[GUARD_BYTES + MAX_IMAGE_BYTES + GUARD_BYTES];
    uint8_t *image;
    size_t length;
    uint16_t width;
    uint16_t height;
} guarded_image_t;

static guarded_image_t canvas;
static uint8_t expected[MAX_IMAGE_BYTES];
static uint8_t previous_frame[FRAME_BYTES];
static unsigned long checks;
static unsigned int failures;

#define CHECK(condition) do { \
    checks++; \
    if (!(condition)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
        failures++; \
    } \
} while (0)

static void begin_canvas(uint16_t width, uint16_t height, uint16_t rotate)
{
    size_t length = ((size_t)width + 7U) / 8U * height;
    if (length > MAX_IMAGE_BYTES) {
        fprintf(stderr, "Test canvas exceeds fixture capacity\n");
        exit(2);
    }
    memset(canvas.memory, 0xA5, sizeof(canvas.memory));
    canvas.image = &canvas.memory[GUARD_BYTES];
    canvas.length = length;
    canvas.width = width;
    canvas.height = height;
    memset(canvas.image, WHITE, length);
    memset(expected, WHITE, length);
    Paint_NewImage(canvas.image, width, height, rotate, WHITE);
}

static void check_guards(void)
{
    size_t index;
    for (index = 0U; index < GUARD_BYTES; index++) {
        CHECK(canvas.memory[index] == 0xA5U);
    }
    for (index = GUARD_BYTES + canvas.length;
         index < sizeof(canvas.memory); index++) {
        if (canvas.memory[index] != 0xA5U) {
            fprintf(stderr, "FAIL trailing guard at offset %lu\n",
                    (unsigned long)(index - GUARD_BYTES));
            failures++;
            break;
        }
    }
    checks++;
}

static void check_image(const char *label)
{
    size_t index;
    for (index = 0U; index < canvas.length; index++) {
        if (canvas.image[index] != expected[index]) {
            fprintf(stderr, "FAIL %s byte %lu: actual %02X, expected %02X\n",
                    label, (unsigned long)index,
                    canvas.image[index], expected[index]);
            failures++;
            break;
        }
    }
    checks++;
    check_guards();
}

static void oracle_pixel(uint8_t *image, uint16_t width, uint16_t height,
                         int32_t x, int32_t y, uint8_t color)
{
    size_t offset;
    uint8_t mask;
    if ((x < 0) || (y < 0) || (x >= width) || (y >= height)) {
        return;
    }
    offset = (size_t)y * (((size_t)width + 7U) / 8U) + (uint32_t)x / 8U;
    mask = (uint8_t)(0x80U >> ((uint32_t)x & 7U));
    if (color == BLACK) {
        image[offset] &= (uint8_t)~mask;
    } else {
        image[offset] |= mask;
    }
}

static void oracle_transform(uint16_t x, uint16_t y, uint16_t rotate,
                             uint8_t mirror, int32_t *px, int32_t *py)
{
    switch (rotate) {
    case ROTATE_0: *px = x; *py = y; break;
    case ROTATE_90: *px = canvas.width - 1 - y; *py = x; break;
    case ROTATE_180:
        *px = canvas.width - 1 - x;
        *py = canvas.height - 1 - y;
        break;
    default: *px = y; *py = canvas.height - 1 - x; break;
    }
    if ((mirror & MIRROR_HORIZONTAL) != 0U) {
        *px = canvas.width - 1 - *px;
    }
    if ((mirror & MIRROR_VERTICAL) != 0U) {
        *py = canvas.height - 1 - *py;
    }
}

static void test_coordinates(void)
{
    static const uint16_t rotations[] = {
        ROTATE_0, ROTATE_90, ROTATE_180, ROTATE_270
    };
    unsigned int r;
    uint8_t mirror;
    uint16_t x;
    uint16_t y;
    int32_t px;
    int32_t py;
    char label[80];

    for (r = 0U; r < 4U; r++) {
        for (mirror = MIRROR_NONE; mirror <= MIRROR_ORIGIN; mirror++) {
            begin_canvas(13U, 9U, ROTATE_0);
            Paint_SetRotate(rotations[r]);
            Paint_SetMirroring(mirror);
            CHECK(Paint.Width == ((r & 1U) ? 9U : 13U));
            CHECK(Paint.Height == ((r & 1U) ? 13U : 9U));
            for (y = 0U; y < Paint.Height; y++) {
                for (x = 0U; x < Paint.Width; x++) {
                    if (((x == 0U || x == Paint.Width - 1U) &&
                         (y == 0U || y == Paint.Height - 1U)) ||
                        ((x * 3U + y * 5U) % 7U == 0U)) {
                        Paint_SetPixel(x, y, BLACK);
                        oracle_transform(x, y, rotations[r], mirror, &px, &py);
                        oracle_pixel(expected, canvas.width, canvas.height,
                                     px, py, BLACK);
                    }
                }
            }
            Paint_SetPixel(Paint.Width, 0U, BLACK);
            Paint_SetPixel(0U, Paint.Height, BLACK);
            Paint_SetPixel(UINT16_MAX, UINT16_MAX, BLACK);
            (void)snprintf(label, sizeof(label), "rotate=%u mirror=%u",
                           rotations[r], mirror);
            check_image(label);
            Paint_SetRotate(123U);
            Paint_SetMirroring(7U);
            CHECK(Paint.Rotate == rotations[r]);
            CHECK(Paint.Mirror == mirror);
        }
    }

    begin_canvas(16U, 2U, ROTATE_0);
    Paint_SetPixel(7U, 0U, BLACK);
    Paint_SetPixel(8U, 0U, BLACK);
    expected[0] = 0xFEU;
    expected[1] = 0x7FU;
    check_image("MSB-first cross-byte pair");

    begin_canvas(3U, 5U, ROTATE_0);
    Paint_SetRotate(ROTATE_90);
    CHECK(Paint.Width == 5U && Paint.Height == 3U);
    Paint_SetRotate(ROTATE_180);
    CHECK(Paint.Width == 3U && Paint.Height == 5U);
    Paint_SetRotate(ROTATE_270);
    CHECK(Paint.Width == 5U && Paint.Height == 3U);
    check_guards();

    Paint_NewImage(canvas.image, 0U, 5U, ROTATE_0, WHITE);
    Paint_Clear(BLACK);
    Paint_SetPixel(0U, 0U, BLACK);
    check_image("zero-width inactive canvas");
    Paint_NewImage(canvas.image, 3U, 5U, 123U, WHITE);
    Paint_Clear(BLACK);
    Paint_SetPixel(0U, 0U, BLACK);
    check_image("invalid-rotation inactive canvas");
    Paint_NewImage(NULL, 3U, 5U, ROTATE_0, WHITE);
    Paint_Clear(BLACK);
    Paint_SetPixel(0U, 0U, BLACK);
    check_image("null inactive canvas");
}

static void test_primitives(void)
{
    int32_t x;
    int32_t y;
    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawPoint(5U, 7U, BLACK, DOT_PIXEL_1X1, DOT_FILL_AROUND);
    oracle_pixel(expected, 16U, 16U, 5, 7, BLACK);
    check_image("1x1 point has no minus-one displacement");

    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawPoint(0U, 0U, BLACK, DOT_PIXEL_3X3, DOT_FILL_AROUND);
    for (y = 0; y <= 2; y++) {
        for (x = 0; x <= 2; x++) {
            oracle_pixel(expected, 16U, 16U, x, y, BLACK);
        }
    }
    check_image("signed point clips at top and left");

    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawLine(2U, 3U, 6U, 3U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    for (x = 2; x <= 6; x++) {
        oracle_pixel(expected, 16U, 16U, x, 3, BLACK);
    }
    check_image("inclusive horizontal line");

    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawLine(6U, 6U, 2U, 2U, BLACK, LINE_STYLE_SOLID, DOT_PIXEL_1X1);
    for (x = 2; x <= 6; x++) {
        oracle_pixel(expected, 16U, 16U, x, x, BLACK);
    }
    check_image("reversed diagonal line");

    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawRectangle(7U, 6U, 3U, 2U, BLACK,
                        DRAW_FILL_EMPTY, DOT_PIXEL_1X1);
    for (y = 2; y <= 6; y++) {
        for (x = 3; x <= 7; x++) {
            if (x == 3 || x == 7 || y == 2 || y == 6) {
                oracle_pixel(expected, 16U, 16U, x, y, BLACK);
            }
        }
    }
    check_image("normalized inclusive rectangle outline");

    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawRectangle(3U, 2U, 7U, 6U, BLACK,
                        DRAW_FILL_FULL, DOT_PIXEL_1X1);
    for (y = 2; y <= 6; y++) {
        for (x = 3; x <= 7; x++) {
            oracle_pixel(expected, 16U, 16U, x, y, BLACK);
        }
    }
    check_image("inclusive filled rectangle");

    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawCircle(8U, 8U, 1U, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_1X1);
    oracle_pixel(expected, 16U, 16U, 8, 7, BLACK);
    oracle_pixel(expected, 16U, 16U, 7, 8, BLACK);
    oracle_pixel(expected, 16U, 16U, 9, 8, BLACK);
    oracle_pixel(expected, 16U, 16U, 8, 9, BLACK);
    check_image("radius-one outline has four axial pixels");
    Paint_DrawCircle(8U, 8U, 1U, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
    oracle_pixel(expected, 16U, 16U, 8, 8, BLACK);
    check_image("radius-one fill includes center");
    Paint_DrawCircle(0U, 0U, 0U, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_1X1);
    oracle_pixel(expected, 16U, 16U, 0, 0, BLACK);
    check_image("zero-radius circle is one pixel");

    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawCircle(0U, 0U, 1U, BLACK, DRAW_FILL_EMPTY, DOT_PIXEL_1X1);
    oracle_pixel(expected, 16U, 16U, 1, 0, BLACK);
    oracle_pixel(expected, 16U, 16U, 0, 1, BLACK);
    check_image("circle signed coordinates clip without wrap");

    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawCircle(8U, 8U, 3U, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
    /* Explicit midpoint raster: row widths 3, 5, 7, 7, 7, 5, 3. */
    for (y = 5; y <= 11; y++) {
        int32_t distance = (y >= 8) ? y - 8 : 8 - y;
        int32_t half_width = (distance <= 1) ? 3 : (distance == 2 ? 2 : 1);
        for (x = 8 - half_width; x <= 8 + half_width; x++) {
            oracle_pixel(expected, 16U, 16U, x, y, BLACK);
        }
    }
    check_image("filled circle covers both half-planes and center row");

    begin_canvas(16U, 16U, ROTATE_0);
    Paint_DrawCircle(0U, 0U, 3U, BLACK, DRAW_FILL_FULL, DOT_PIXEL_1X1);
    for (y = 0; y <= 3; y++) {
        int32_t half_width = (y <= 1) ? 3 : (y == 2 ? 2 : 1);
        for (x = 0; x <= half_width; x++) {
            oracle_pixel(expected, 16U, 16U, x, y, BLACK);
        }
    }
    check_image("filled circle clips negative half-planes");

    begin_canvas(UINT16_MAX, 1U, ROTATE_0);
    Paint_DrawRectangle(65534U, 0U, UINT16_MAX, UINT16_MAX, BLACK,
                        DRAW_FILL_FULL, DOT_PIXEL_1X1);
    oracle_pixel(expected, UINT16_MAX, 1U, 65534, 0, BLACK);
    check_image("filled rectangle clips uint16 maximum without wrap");
    Paint_DrawRectangle(65534U, 0U, UINT16_MAX, 0U, BLACK,
                        DRAW_FILL_EMPTY, DOT_PIXEL_3X3);
    for (x = 65532; x <= 65534; x++) {
        oracle_pixel(expected, UINT16_MAX, 1U, x, 0, BLACK);
    }
    check_image("thick outline clips uint16 maximum without wrap");

    begin_canvas(13U, 9U, ROTATE_0);
    Paint_Clear(BLACK);
    memset(expected, BLACK, canvas.length);
    Paint_ClearWindows(7U, 2U, 13U, 9U, WHITE);
    for (y = 2; y < 9; y++) {
        for (x = 7; x < 13; x++) {
            oracle_pixel(expected, 13U, 9U, x, y, WHITE);
        }
    }
    check_image("clear window uses exclusive end and preserves padding");
}

/* Decode row-strided fonts directly; do not call any GUI rendering helper. */
static void oracle_char(uint8_t *image, uint16_t width, uint16_t height,
                        uint32_t x, uint32_t y, unsigned int code,
                        const sFONT *font, uint8_t scale,
                        uint8_t background, uint8_t foreground,
                        uint8_t clock_mapping)
{
    uint32_t row;
    uint32_t col;
    uint32_t dx;
    uint32_t dy;
    size_t stride = ((size_t)font->Width + 7U) / 8U;
    size_t start;
    uint8_t bit;
    uint8_t color;
    int32_t px;
    int32_t py;
    if (code < 32U || code > 126U) {
        code = '?';
    }
    start = (code - 32U) * stride * font->Height;
    for (row = 0U; row < font->Height; row++) {
        for (col = 0U; col < font->Width; col++) {
            bit = (uint8_t)(font->table[start + row * stride + col / 8U] &
                            (0x80U >> (col % 8U)));
            if (bit == 0U && background == WHITE) {
                continue;
            }
            color = bit ? foreground : background;
            for (dy = 0U; dy < scale; dy++) {
                for (dx = 0U; dx < scale; dx++) {
                    px = (int32_t)(x + col * scale + dx);
                    py = (int32_t)(y + row * scale + dy);
                    if (clock_mapping != 0U) {
                        int32_t logical_x = px;
                        px = 127 - py;
                        py = 295 - logical_x;
                    }
                    oracle_pixel(image, width, height, px, py, color);
                }
            }
        }
    }
}

static void oracle_string(uint8_t *image, uint16_t width, uint16_t height,
                          uint32_t x, uint32_t y, const char *text,
                          const sFONT *font, uint8_t scale)
{
    while (*text != '\0') {
        oracle_char(image, width, height, x, y, (unsigned char)*text,
                    font, scale, WHITE, BLACK, 0U);
        x += (uint32_t)font->Width * scale;
        text++;
    }
}

static void test_fonts(void)
{
    static const sFONT *const fonts[] = {
        &Font8, &Font12, &Font16, &Font20, &Font24
    };
    static const unsigned int invalid[] = {0U, 1U, 31U, 127U, 128U, 255U};
    unsigned int f;
    unsigned int code;
    unsigned int i;
    char label[80];
    for (f = 0U; f < 5U; f++) {
        for (code = 32U; code <= 126U; code++) {
            begin_canvas(64U, 48U, ROTATE_0);
            Paint_DrawChar(7U, 5U, (char)code, fonts[f], WHITE, BLACK);
            oracle_char(expected, 64U, 48U, 7U, 5U, code, fonts[f],
                        1U, WHITE, BLACK, 0U);
            (void)snprintf(label, sizeof(label), "Font%u printable ASCII %u",
                           fonts[f]->Height, code);
            check_image(label);
        }
        for (i = 0U; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
            begin_canvas(64U, 48U, ROTATE_0);
            Paint_DrawChar(7U, 5U, (char)invalid[i], fonts[f], WHITE, BLACK);
            oracle_char(expected, 64U, 48U, 7U, 5U, '?', fonts[f],
                        1U, WHITE, BLACK, 0U);
            check_image("invalid ASCII becomes question mark");
        }
    }

    begin_canvas(64U, 48U, ROTATE_0);
    memset(canvas.image, 0x5AU, canvas.length);
    memset(expected, 0x5AU, canvas.length);
    Paint_DrawChar(7U, 5U, 'A', &Font24, WHITE, BLACK);
    oracle_char(expected, 64U, 48U, 7U, 5U, 'A', &Font24,
                1U, WHITE, BLACK, 0U);
    check_image("WHITE text background preserves existing pixels");

    begin_canvas(64U, 48U, ROTATE_0);
    Paint_DrawChar(7U, 5U, 'A', &Font24, BLACK, WHITE);
    oracle_char(expected, 64U, 48U, 7U, 5U, 'A', &Font24,
                1U, BLACK, WHITE, 0U);
    check_image("opaque black background and white foreground");

    begin_canvas(64U, 64U, ROTATE_0);
    Paint_DrawCharScaled(7U, 5U, '4', &Font24, WHITE, BLACK, 2U);
    oracle_char(expected, 64U, 64U, 7U, 5U, '4', &Font24,
                2U, WHITE, BLACK, 0U);
    check_image("2x glyph replicates every source pixel");
    Paint_DrawCharScaled(0U, 0U, 'X', &Font24, BLACK, WHITE, 0U);
    Paint_DrawString_ENScaled(0U, 0U, "ZERO", &Font24, BLACK, WHITE, 0U);
    check_image("zero scale is a no-op");

    begin_canvas(65535U, 1U, ROTATE_0);
    Paint_DrawCharScaled(65534U, 0U, 'A', &Font8, WHITE, BLACK, 255U);
    oracle_char(expected, 65535U, 1U, 65534U, 0U, 'A', &Font8,
                255U, WHITE, BLACK, 0U);
    check_image("scaled uint16 edge clips without coordinate wrap");

    begin_canvas(64U, 48U, ROTATE_0);
    Paint_DrawChar(0U, 0U, 'A', NULL, WHITE, BLACK);
    Paint_DrawString_EN(0U, 0U, NULL, &Font8, WHITE, BLACK);
    Paint_DrawString_EN(0U, 0U, "A", NULL, WHITE, BLACK);
    check_image("null font and string inputs are no-ops");
}

static void test_strings_and_numbers(void)
{
    static const struct {
        int32_t number;
        const char *text;
    } numbers[] = {
        {0, "0"}, {-1, "-1"}, {INT32_MIN, "-2147483648"},
        {INT32_MAX, "2147483647"}
    };
    unsigned int i;
    PAINT_TIME time;
    uint32_t w;

    begin_canvas((uint16_t)(Font8.Width * 2U),
                 (uint16_t)(Font8.Height * 2U), ROTATE_0);
    Paint_DrawString_EN(0U, 0U, "ABCDE", &Font8, WHITE, BLACK);
    oracle_char(expected, canvas.width, canvas.height, 0U, 0U,
                'A', &Font8, 1U, WHITE, BLACK, 0U);
    oracle_char(expected, canvas.width, canvas.height, Font8.Width, 0U,
                'B', &Font8, 1U, WHITE, BLACK, 0U);
    oracle_char(expected, canvas.width, canvas.height, 0U, Font8.Height,
                'C', &Font8, 1U, WHITE, BLACK, 0U);
    oracle_char(expected, canvas.width, canvas.height,
                Font8.Width, Font8.Height, 'D', &Font8, 1U, WHITE, BLACK, 0U);
    check_image("string stops at last row instead of overwriting first row");

    begin_canvas(96U, 32U, ROTATE_0);
    Paint_DrawString_ENScaled(2U, 1U, "12:34", &Font8, WHITE, BLACK, 2U);
    oracle_string(expected, 96U, 32U, 2U, 1U, "12:34", &Font8, 2U);
    check_image("scaled string keeps fixed character cell advance");

    for (i = 0U; i < sizeof(numbers) / sizeof(numbers[0]); i++) {
        begin_canvas(128U, 16U, ROTATE_0);
        Paint_DrawNum(1U, 1U, numbers[i].number, &Font8, WHITE, BLACK);
        oracle_string(expected, 128U, 16U, 1U, 1U,
                      numbers[i].text, &Font8, 1U);
        check_image("signed decimal number including int32 extrema");
    }

    begin_canvas(128U, 16U, ROTATE_0);
    memset(&time, 0, sizeof(time));
    time.Hour = 23U;
    time.Min = 59U;
    time.Sec = 58U;
    Paint_DrawTime(1U, 1U, &time, &Font8, WHITE, BLACK);
    w = Font8.Width;
    oracle_char(expected, 128U, 16U, 1U, 1U, '2', &Font8,
                1U, WHITE, BLACK, 0U);
    oracle_char(expected, 128U, 16U, 1U + w, 1U, '3', &Font8,
                1U, WHITE, BLACK, 0U);
    oracle_char(expected, 128U, 16U, 1U + w + w / 4U + w / 2U,
                1U, ':', &Font8, 1U, WHITE, BLACK, 0U);
    oracle_char(expected, 128U, 16U, 1U + w * 2U + w / 2U,
                1U, '5', &Font8, 1U, WHITE, BLACK, 0U);
    oracle_char(expected, 128U, 16U, 1U + w * 3U + w / 2U,
                1U, '9', &Font8, 1U, WHITE, BLACK, 0U);
    oracle_char(expected, 128U, 16U, 1U + w * 4U + w / 2U - w / 4U,
                1U, ':', &Font8, 1U, WHITE, BLACK, 0U);
    oracle_char(expected, 128U, 16U, 1U + w * 5U,
                1U, '5', &Font8, 1U, WHITE, BLACK, 0U);
    oracle_char(expected, 128U, 16U, 1U + w * 6U,
                1U, '8', &Font8, 1U, WHITE, BLACK, 0U);
    check_image("valid time preserves original helper positions");

    begin_canvas(128U, 16U, ROTATE_0);
    time.Hour = 24U;
    Paint_DrawTime(1U, 1U, &time, &Font8, WHITE, BLACK);
    time.Hour = 23U;
    time.Min = 60U;
    Paint_DrawTime(1U, 1U, &time, &Font8, WHITE, BLACK);
    time.Min = 59U;
    time.Sec = 60U;
    Paint_DrawTime(1U, 1U, &time, &Font8, WHITE, BLACK);
    Paint_DrawTime(1U, 1U, NULL, &Font8, WHITE, BLACK);
    check_image("out-of-range and null time rejected");
}

static void test_bitmap_and_selection(void)
{
    static const uint8_t bitmap[4] = {0x81U, 0x42U, 0x24U, 0x18U};
    uint8_t alternate[4] = {WHITE, WHITE, WHITE, WHITE};
    begin_canvas(16U, 2U, ROTATE_0);
    Paint_DrawBitMap(bitmap);
    memcpy(expected, bitmap, sizeof(bitmap));
    check_image("bitmap copies exact native frame bytes");
    Paint_DrawBitMap(NULL);
    check_image("null bitmap is a no-op");
    Paint_SelectImage(alternate);
    Paint_SetPixel(0U, 0U, BLACK);
    CHECK(alternate[0] == 0x7FU);
    check_image("image selection leaves original buffer untouched");
    Paint_SelectImage(NULL);
    Paint_SetPixel(0U, 0U, BLACK);
    Paint_Clear(BLACK);
    check_image("null selected image is inactive");
}

static void oracle_clock(uint8_t *image, uint8_t hours,
                         uint8_t minutes, uint8_t seconds)
{
    char text[9];
    unsigned int i;
    (void)snprintf(text, sizeof(text), "%02u:%02u:%02u",
                   hours, minutes, seconds);
    memset(image, WHITE, FRAME_BYTES);
    for (i = 0U; i < 8U; i++) {
        oracle_char(image, NATIVE_WIDTH, NATIVE_HEIGHT,
                    12U + 34U * i, 45U, (unsigned char)text[i], &Font24,
                    2U, WHITE, BLACK, 1U);
    }
}

static int in_seconds_window(size_t byte)
{
    size_t y = byte / (NATIVE_WIDTH / 8U);
    size_t x_byte = byte % (NATIVE_WIDTH / 8U);
    return y >= 12U && y < 80U && x_byte >= 4U && x_byte < 11U;
}

static void test_clock_view(void)
{
    static const uint8_t transitions[][2] = {
        {3U, 4U}, {4U, 5U}, {4U, 2U}, {9U, 10U}, {59U, 0U}
    };
    unsigned int i;
    uint8_t second;
    size_t byte;
    size_t window_bytes = 0U;
    unsigned int changed_outside;

    CHECK(EPD_CLOCK_VIEW_X == 12U && EPD_CLOCK_VIEW_Y == 45U);
    CHECK(EPD_CLOCK_VIEW_SCALE == 2U);
    CHECK(EPD_CLOCK_VIEW_FONT_WIDTH == 17U);
    CHECK(EPD_CLOCK_VIEW_FONT_HEIGHT == 24U);
    CHECK(EPD_CLOCK_VIEW_SECONDS_NATIVE_X == 32U);
    CHECK(EPD_CLOCK_VIEW_SECONDS_NATIVE_Y == 12U);
    CHECK(EPD_CLOCK_VIEW_SECONDS_NATIVE_WIDTH == 56U);
    CHECK(EPD_CLOCK_VIEW_SECONDS_NATIVE_HEIGHT == 68U);
    for (byte = 0U; byte < FRAME_BYTES; byte++) {
        if (in_seconds_window(byte)) {
            window_bytes++;
        }
    }
    CHECK(window_bytes == 476U);

    for (second = 0U; second < 60U; second++) {
        begin_canvas(NATIVE_WIDTH, NATIVE_HEIGHT, ROTATE_0);
        epd_clock_view_draw(canvas.image, 12U, 34U, second);
        oracle_clock(expected, 12U, 34U, second);
        check_image("clock matches independent full Font24 pixel oracle");
        if (second > 0U) {
            for (byte = 0U; byte < FRAME_BYTES; byte++) {
                if (!in_seconds_window(byte)) {
                    CHECK(canvas.image[byte] == previous_frame[byte]);
                }
            }
        }
        memcpy(previous_frame, canvas.image, FRAME_BYTES);
    }

    for (i = 0U; i < sizeof(transitions) / sizeof(transitions[0]); i++) {
        begin_canvas(NATIVE_WIDTH, NATIVE_HEIGHT, ROTATE_0);
        epd_clock_view_draw(canvas.image, 12U, 34U, transitions[i][0]);
        memcpy(previous_frame, canvas.image, FRAME_BYTES);
        epd_clock_view_draw(canvas.image, 12U, 34U, transitions[i][1]);
        oracle_clock(expected, 12U, 34U, transitions[i][1]);
        check_image("selected second transition clears previous glyph");
        for (byte = 0U; byte < FRAME_BYTES; byte++) {
            if (!in_seconds_window(byte)) {
                CHECK(canvas.image[byte] == previous_frame[byte]);
            }
        }
    }

    begin_canvas(NATIVE_WIDTH, NATIVE_HEIGHT, ROTATE_0);
    epd_clock_view_draw(canvas.image, 12U, 34U, 59U);
    memcpy(previous_frame, canvas.image, FRAME_BYTES);
    epd_clock_view_draw(canvas.image, 12U, 35U, 0U);
    oracle_clock(expected, 12U, 35U, 0U);
    check_image("minute rollover redraws all changed characters");
    changed_outside = 0U;
    for (byte = 0U; byte < FRAME_BYTES; byte++) {
        if (!in_seconds_window(byte) &&
            canvas.image[byte] != previous_frame[byte]) {
            changed_outside++;
        }
    }
    CHECK(changed_outside > 0U);

    begin_canvas(NATIVE_WIDTH, NATIVE_HEIGHT, ROTATE_0);
    epd_clock_view_draw(canvas.image, 23U, 59U, 59U);
    epd_clock_view_draw(canvas.image, 0U, 0U, 0U);
    oracle_clock(expected, 0U, 0U, 0U);
    check_image("midnight rollover pixel output");
    epd_clock_view_draw(NULL, 0U, 0U, 0U);
    check_image("null clock frame is a no-op");
}

static void write_le16(FILE *file, uint16_t value)
{
    (void)fputc((int)(value & 0xFFU), file);
    (void)fputc((int)(value >> 8U), file);
}

static void write_le32(FILE *file, uint32_t value)
{
    write_le16(file, (uint16_t)(value & 0xFFFFU));
    write_le16(file, (uint16_t)(value >> 16U));
}

/* Export the native framebuffer in the accepted photo-1 viewing direction. */
static void write_preview(const char *directory, const char *name)
{
    const uint32_t width = 296U * 3U;
    const uint32_t height = 128U * 3U;
    const uint32_t row_bytes = (width * 3U + 3U) & ~3U;
    char path[4096];
    FILE *file;
    int io_error;
    uint32_t x;
    uint32_t y;
    uint32_t padding;
    if (snprintf(path, sizeof(path), "%s/%s.bmp", directory, name) < 0) {
        failures++;
        return;
    }
    file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Cannot write preview %s\n", path);
        failures++;
        return;
    }
    (void)fputc('B', file);
    (void)fputc('M', file);
    write_le32(file, 54U + row_bytes * height);
    write_le32(file, 0U);
    write_le32(file, 54U);
    write_le32(file, 40U);
    write_le32(file, width);
    write_le32(file, height);
    write_le16(file, 1U);
    write_le16(file, 24U);
    write_le32(file, 0U);
    write_le32(file, row_bytes * height);
    write_le32(file, 2835U);
    write_le32(file, 2835U);
    write_le32(file, 0U);
    write_le32(file, 0U);
    for (y = height; y > 0U; y--) {
        for (x = 0U; x < width; x++) {
            uint32_t px = 127U - (y - 1U) / 3U;
            uint32_t py = 295U - x / 3U;
            uint8_t bit = (uint8_t)(canvas.image[py * 16U + px / 8U] &
                                    (0x80U >> (px % 8U)));
            uint8_t color = (bit != 0U) ? 255U : 0U;
            (void)fputc(color, file);
            (void)fputc(color, file);
            (void)fputc(color, file);
        }
        for (padding = width * 3U; padding < row_bytes; padding++) {
            (void)fputc(0, file);
        }
    }
    io_error = ferror(file);
    if (fclose(file) != 0) {
        io_error = 1;
    }
    if (io_error != 0) {
        fprintf(stderr, "Cannot finish preview %s\n", path);
        failures++;
        return;
    }
    printf("Preview BMP: %s\n", path);
}

static void export_previews(const char *directory)
{
    begin_canvas(NATIVE_WIDTH, NATIVE_HEIGHT, ROTATE_0);
    epd_clock_view_draw(canvas.image, 12U, 34U, 42U);
    check_guards();
    write_preview(directory, "clock-12-34-42");
    epd_clock_view_draw_demo(canvas.image);
    check_guards();
    write_preview(directory, "gui-font-demo");
    epd_clock_view_draw_orientation(canvas.image);
    check_guards();
    write_preview(directory, "orientation");
}

static int cell_matches(unsigned int cell)
{
    uint32_t x;
    uint32_t y;
    for (y = 45U; y < 93U; y++) {
        for (x = 12U + cell * 34U; x < 46U + cell * 34U; x++) {
            uint32_t px = 127U - y;
            uint32_t py = 295U - x;
            size_t byte = py * 16U + px / 8U;
            uint8_t mask = (uint8_t)(0x80U >> (px % 8U));
            if ((canvas.image[byte] & mask) != (expected[byte] & mask)) {
                return 0;
            }
        }
    }
    return 1;
}

/* A coherent target RAM snapshot must decode uniquely and match every pixel. */
static int verify_target_frame(const char *filename, const char *directory)
{
    FILE *file;
    size_t length;
    int extra;
    int io_error;
    unsigned int cell;
    unsigned int code;
    unsigned int matches;
    unsigned int first;
    unsigned int last;
    unsigned int hours;
    unsigned int minutes;
    unsigned int seconds;
    char text[9] = {0};

    file = fopen(filename, "rb");
    if (file == NULL) {
        fprintf(stderr, "UNAVAILABLE: target framebuffer cannot be opened: %s\n", filename);
        return 1;
    }
    begin_canvas(NATIVE_WIDTH, NATIVE_HEIGHT, ROTATE_0);
    length = fread(canvas.image, 1U, FRAME_BYTES, file);
    extra = fgetc(file);
    io_error = ferror(file);
    if (fclose(file) != 0) {
        io_error = 1;
    }
    if (length != FRAME_BYTES || extra != EOF || io_error != 0) {
        fprintf(stderr, "UNAVAILABLE: target snapshot must contain exactly %u bytes\n",
                (unsigned int)FRAME_BYTES);
        return 1;
    }
    write_preview(directory, "target-frame");
    if (failures != 0U) {
        return 1;
    }
    for (cell = 0U; cell < 8U; cell++) {
        first = ((cell == 2U) || (cell == 5U)) ? ':' : '0';
        last = ((cell == 2U) || (cell == 5U)) ? ':' : '9';
        matches = 0U;
        for (code = first; code <= last; code++) {
            memset(expected, WHITE, FRAME_BYTES);
            oracle_char(expected, NATIVE_WIDTH, NATIVE_HEIGHT,
                        12U + cell * 34U, 45U, code, &Font24,
                        2U, WHITE, BLACK, 1U);
            if (cell_matches(cell) != 0) {
                text[cell] = (char)code;
                matches++;
            }
        }
        if (matches != 1U) {
            fprintf(stderr, "UNAVAILABLE: target cell %u has %u glyph matches; "
                    "snapshot may be torn or use a different layout\n", cell, matches);
            return 1;
        }
    }
    hours = (unsigned int)(text[0] - '0') * 10U + (unsigned int)(text[1] - '0');
    minutes = (unsigned int)(text[3] - '0') * 10U + (unsigned int)(text[4] - '0');
    seconds = (unsigned int)(text[6] - '0') * 10U + (unsigned int)(text[7] - '0');
    if (hours >= 24U || minutes >= 60U || seconds >= 60U) {
        fprintf(stderr, "UNAVAILABLE: decoded target time is invalid: %s\n", text);
        return 1;
    }
    oracle_clock(expected, (uint8_t)hours, (uint8_t)minutes, (uint8_t)seconds);
    check_image("target snapshot equals independent Font24 full-frame oracle");
    if (failures != 0U) {
        fprintf(stderr, "UNAVAILABLE: glyphs decode to %s but full frame differs; "
                "snapshot may be torn or have unexpected background pixels\n", text);
        return 1;
    }
    printf("PASS: target RAM framebuffer uniquely decodes to %s and all %u bytes "
           "match the independent Font24 oracle\n", text, (unsigned int)FRAME_BYTES);
    printf("Evidence covers the captured RAM image; panel appearance and physical "
           "refresh timing require separate observation\n");
    return 0;
}

int main(int argc, char **argv)
{
    if ((argc == 4) && (strcmp(argv[1], "--target-frame") == 0)) {
        return verify_target_frame(argv[2], argv[3]);
    }
    test_coordinates();
    test_primitives();
    test_fonts();
    test_strings_and_numbers();
    test_bitmap_and_selection();
    test_clock_view();
    if (argc > 1) {
        export_previews(argv[1]);
    }
    if (failures != 0U) {
        fprintf(stderr, "%u failures across %lu checks\n", failures, checks);
        return 1;
    }
    printf("PASS: GUI and clock host tests (%lu checks)\n", checks);
    printf("Coverage: guards, transforms, primitives, 475 ASCII glyphs, "
           "scaling, strings, signed numbers, time and 476-byte seconds window\n");
    return 0;
}
