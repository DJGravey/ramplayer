/* mkexr.c - writes small uncompressed EXR files for testing.
 *
 * ffmpeg can produce ordinary EXRs, but not the awkward ones: a data window
 * that differs from the display window, odd channel sets, or a single
 * luminance channel. This writes those directly so reader_exr.c can be tested
 * against them.
 *
 * The pattern encodes each pixel's position in the *display* window, so a
 * decoder that mislays the data window offset shows up immediately:
 *     R = x / (W-1)      G = y / (H-1)      B = 0.25
 *
 * Build: cc -O2 -o mkexr tools/mkexr.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *g_out;

static void w_bytes(const void *p, size_t n) { fwrite(p, 1, n, g_out); }
static void w_u8(uint8_t v)   { w_bytes(&v, 1); }
static void w_i32(int32_t v)  { w_bytes(&v, 4); }  /* host is little endian */
static void w_u64(uint64_t v) { w_bytes(&v, 8); }
static void w_f32(float v)    { w_bytes(&v, 4); }
static void w_str0(const char *s) { w_bytes(s, strlen(s) + 1); }

static void attr_head(const char *name, const char *type, int32_t size)
{
    w_str0(name);
    w_str0(type);
    w_i32(size);
}

static uint16_t float_to_half(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t  exp  = (int32_t)((x >> 23) & 0xff) - 127 + 15;
    uint32_t man  = x & 0x7fffffu;
    if (exp <= 0)  return (uint16_t)sign;
    if (exp >= 31) return (uint16_t)(sign | 0x7c00u);
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (man >> 13));
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        fprintf(stderr,
            "usage: mkexr OUT.exr W H [dx0 dy0 dx1 dy1] [rgb|rgba|y]\n"
            "  W H              display window size (0,0)-(W-1,H-1)\n"
            "  dx0..dy1         data window, defaults to the display window\n");
        return 2;
    }

    const char *path = argv[1];
    int W = atoi(argv[2]);
    int H = atoi(argv[3]);
    int dx0 = 0, dy0 = 0, dx1 = W - 1, dy1 = H - 1;
    const char *chans = "rgb";

    int argi = 4;
    if (argc >= 8 && argv[4][0] != 'r' && argv[4][0] != 'y') {
        dx0 = atoi(argv[4]); dy0 = atoi(argv[5]);
        dx1 = atoi(argv[6]); dy1 = atoi(argv[7]);
        argi = 8;
    }
    if (argc > argi) chans = argv[argi];

    int dw = dx1 - dx0 + 1;
    int dh = dy1 - dy0 + 1;
    if (W <= 0 || H <= 0 || dw <= 0 || dh <= 0) {
        fprintf(stderr, "mkexr: bad window\n");
        return 2;
    }

    /* Channels must be written in alphabetical order, which is also the order
     * the pixel data for each scanline follows. */
    const char *names[4];
    int nch = 0;
    if (strcmp(chans, "y") == 0) {
        names[nch++] = "Y";
    } else {
        if (strcmp(chans, "rgba") == 0) names[nch++] = "A";
        names[nch++] = "B";
        names[nch++] = "G";
        names[nch++] = "R";
    }

    g_out = fopen(path, "wb");
    if (!g_out) { perror("mkexr"); return 1; }

    w_u8(0x76); w_u8(0x2f); w_u8(0x31); w_u8(0x01); /* magic */
    w_i32(2);                                       /* version 2, no flags */

    int32_t chlist_size = 1; /* trailing null */
    for (int i = 0; i < nch; i++) chlist_size += (int32_t)strlen(names[i]) + 1 + 16;
    attr_head("channels", "chlist", chlist_size);
    for (int i = 0; i < nch; i++) {
        w_str0(names[i]);
        w_i32(1);          /* HALF */
        w_u8(0);           /* pLinear */
        w_u8(0); w_u8(0); w_u8(0);
        w_i32(1); w_i32(1); /* sampling */
    }
    w_u8(0);

    attr_head("compression", "compression", 1);
    w_u8(0); /* NO_COMPRESSION: one scanline per chunk */

    attr_head("dataWindow", "box2i", 16);
    w_i32(dx0); w_i32(dy0); w_i32(dx1); w_i32(dy1);

    attr_head("displayWindow", "box2i", 16);
    w_i32(0); w_i32(0); w_i32(W - 1); w_i32(H - 1);

    attr_head("lineOrder", "lineOrder", 1);
    w_u8(0); /* INCREASING_Y */

    attr_head("pixelAspectRatio", "float", 4);
    w_f32(1.0f);

    attr_head("screenWindowCenter", "v2f", 8);
    w_f32(0.0f); w_f32(0.0f);

    attr_head("screenWindowWidth", "float", 4);
    w_f32(1.0f);

    w_u8(0); /* end of header */

    /* Offset table, then the chunks it points at. */
    long table_pos = ftell(g_out);
    for (int y = 0; y < dh; y++) w_u64(0);

    size_t row_bytes = (size_t)dw * 2;
    uint16_t *row = malloc(row_bytes);
    uint64_t *offsets = malloc((size_t)dh * sizeof *offsets);

    for (int j = 0; j < dh; j++) {
        offsets[j] = (uint64_t)ftell(g_out);
        w_i32(dy0 + j);                          /* scanline y */
        w_i32((int32_t)(row_bytes * nch));       /* pixel data size */

        for (int c = 0; c < nch; c++) {
            for (int i = 0; i < dw; i++) {
                int dispx = dx0 + i;
                int dispy = dy0 + j;
                float v;
                /* Position encoded against the display window, clamped so
                 * overscan pixels stay in a sane range. */
                float fx = (W > 1) ? (float)dispx / (float)(W - 1) : 0.0f;
                float fy = (H > 1) ? (float)dispy / (float)(H - 1) : 0.0f;
                if (fx < 0.0f) fx = 0.0f; else if (fx > 1.0f) fx = 1.0f;
                if (fy < 0.0f) fy = 0.0f; else if (fy > 1.0f) fy = 1.0f;

                const char *nm = names[c];
                if      (strcmp(nm, "R") == 0) v = fx;
                else if (strcmp(nm, "G") == 0) v = fy;
                else if (strcmp(nm, "B") == 0) v = 0.25f;
                else if (strcmp(nm, "A") == 0) v = 1.0f;
                else                           v = fx; /* Y */
                row[i] = float_to_half(v);
            }
            w_bytes(row, row_bytes);
        }
    }

    fseek(g_out, table_pos, SEEK_SET);
    for (int j = 0; j < dh; j++) w_u64(offsets[j]);

    fclose(g_out);
    free(row);
    free(offsets);
    printf("wrote %s: display %dx%d, data (%d,%d)-(%d,%d), %d channels\n",
           path, W, H, dx0, dy0, dx1, dy1, nch);
    return 0;
}
