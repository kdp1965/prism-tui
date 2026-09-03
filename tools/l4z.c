// l4z - host side compressor for PRISM DSM streams.
//
// Reads a generated song .c file (as produced by mp3_to_dsm.py: a
// `const uint8_t NAME[] = { 0x.., ... };` array plus a NAME_rate
// constant), takes the first SECONDS of samples (ignoring any
// preprocessor lines inside the array), compresses them and emits a new
// .c file with the compressed blob.
//
// Format (matches the decoder in play.c):
//   u32  total uncompressed bytes
//   u32  block size B (uncompressed bytes per block, a multiple of 3)
//   then per block:
//     u16 clen        0xFFFF = stored: min(B, remaining) raw bytes follow
//                     else   = LZ4 block data of clen bytes
//
// Blocks are compressed INDEPENDENTLY (matches never reach back into the
// previous block) so the target can stream-decompress into small
// ping-pong buffers with no history window.  The compressor is plain
// greedy LZ4 (single probe hash table); the decoder is the standard LZ4
// block sequence walk and is verified here against every block before
// the output is written.
//
// Build:  cc -O2 -o l4z l4z.c
// Usage:  l4z <song.c> <seconds> <out.c> <name>
//         seconds 0 = the whole song

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

#define BLK        3072u        // uncompressed bytes per block (mult of 3)
#define HASH_LOG   13
#define HASH_SIZE  (1u << HASH_LOG)
#define STORED     0xFFFFu

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t hash4(uint32_t v)
{
    return (v * 2654435761u) >> (32 - HASH_LOG);
}

// --------------------------------------------------------------------------
// Greedy LZ4 block compression, independent block, standard block format.
// Returns compressed length, or >= n if the block did not compress.
// --------------------------------------------------------------------------
static uint32_t l4z_compress_block(const uint8_t *src, uint32_t n,
                                   uint8_t *dst, uint32_t dmax)
{
    uint32_t table[HASH_SIZE] = { 0 };      // pos + 1, 0 = empty
    uint32_t pos = 0, anchor = 0, o = 0;

    while (pos + 12 <= n) {                 // LZ4 MFLIMIT: no matches near end
        uint32_t v = rd32(src + pos);
        uint32_t h = hash4(v);
        uint32_t cand = table[h];
        table[h] = pos + 1;

        if (cand && rd32(src + cand - 1) == v && pos - (cand - 1) <= 65535) {
            uint32_t mstart = cand - 1;
            uint32_t mlen = 4;
            while (pos + mlen < n - 5 && src[mstart + mlen] == src[pos + mlen])
                ++mlen;

            // token + literal run + offset + match run
            uint32_t lit = pos - anchor;
            uint32_t l = lit, m = mlen - 4;
            if (o + 15 + lit > dmax)
                return n;                   // never going to win, bail
            dst[o++] = (uint8_t)((l >= 15 ? 15 : l) << 4 |
                                 (m >= 15 ? 15 : m));
            if (l >= 15) {
                l -= 15;
                while (l >= 255) { dst[o++] = 255; l -= 255; }
                dst[o++] = (uint8_t)l;
            }
            memcpy(dst + o, src + anchor, lit);
            o += lit;
            uint32_t off = pos - mstart;
            dst[o++] = (uint8_t)off;
            dst[o++] = (uint8_t)(off >> 8);
            if (m >= 15) {
                m -= 15;
                while (m >= 255) { dst[o++] = 255; m -= 255; }
                dst[o++] = (uint8_t)m;
            }
            pos += mlen;
            anchor = pos;
        } else
            ++pos;
    }

    // final literal-only sequence
    uint32_t lit = n - anchor, l = lit;
    if (o + 15 + lit > dmax)
        return n;
    dst[o++] = (uint8_t)((l >= 15 ? 15 : l) << 4);
    if (l >= 15) {
        l -= 15;
        while (l >= 255) { dst[o++] = 255; l -= 255; }
        dst[o++] = (uint8_t)l;
    }
    memcpy(dst + o, src + anchor, lit);
    o += lit;
    return o;
}

// --------------------------------------------------------------------------
// Reference decoder - MUST stay logic-identical to l4z_decode() in play.c
// --------------------------------------------------------------------------
static uint32_t l4z_decode(const uint8_t *src, uint32_t slen, uint8_t *dst)
{
    const uint8_t *s_end = src + slen;
    uint8_t *d = dst;

    for (;;) {
        uint8_t  tok = *src++;
        uint32_t len = tok >> 4;

        if (len == 15) {
            uint8_t b;
            do { b = *src++; len += b; } while (b == 255);
        }
        while (len--)
            *d++ = *src++;
        if (src >= s_end)
            break;

        uint32_t off = src[0] | (src[1] << 8);
        src += 2;
        len = (tok & 15) + 4;
        if ((tok & 15) == 15) {
            uint8_t b;
            do { b = *src++; len += b; } while (b == 255);
        }
        const uint8_t *m = d - off;
        while (len--)
            *d++ = *m++;
    }
    return (uint32_t)(d - dst);
}

// --------------------------------------------------------------------------
// Parse the sample array and the _rate constant out of the song .c file.
// Preprocessor lines (#ifdef FULL_SONG etc) inside the array are ignored:
// anything that is not a 0xNN token is skipped.
// --------------------------------------------------------------------------
static uint8_t *parse_song(const char *path, uint32_t *n_out, uint32_t *rate)
{
    FILE *f = fopen(path, "r");
    if (!f) { perror(path); exit(1); }
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *txt = malloc(fsz + 1);
    if (fread(txt, 1, fsz, f) != (size_t)fsz) { perror("read"); exit(1); }
    txt[fsz] = 0;
    fclose(f);

    *rate = 0;
    char *r = strstr(txt, "_rate = ");
    if (r)
        *rate = (uint32_t)strtoul(r + 8, NULL, 0);

    char *a = strstr(txt, "uint8_t");
    while (a && !strchr(a, '{'))
        a = strstr(a + 1, "uint8_t");
    if (!a) { fprintf(stderr, "no uint8_t array found\n"); exit(1); }
    a = strchr(a, '{') + 1;

    uint8_t *data = malloc(fsz / 5 + 16);   // every byte is >= 5 chars of text
    uint32_t n = 0;
    while (*a && *a != '}') {
        if (a[0] == '0' && (a[1] == 'x' || a[1] == 'X') && isxdigit(a[2])) {
            data[n++] = (uint8_t)strtoul(a, &a, 16);
        } else
            ++a;
    }
    free(txt);
    *n_out = n;
    return data;
}

int main(int argc, char **argv)
{
    if (argc != 5) {
        fprintf(stderr, "usage: %s <song.c> <seconds> <out.c> <name>\n"
                        "       seconds 0 = whole song\n", argv[0]);
        return 1;
    }
    const char *inpath = argv[1];
    double seconds = atof(argv[2]);
    const char *outpath = argv[3];
    const char *name = argv[4];

    uint32_t total, rate;
    uint8_t *song = parse_song(inpath, &total, &rate);
    printf("parsed %s: %u bytes, rate %u bits/s\n", inpath, total, rate);
    if (!rate) { fprintf(stderr, "no _rate found\n"); return 1; }

    // Guard: an ADPCM blob (u32 samples, u32 rate, u32 block=1024) is
    // already high-entropy - L4Z gains nothing, and the result would be
    // misidentified as an L4Z DSM stream and play as noise.
    if (total >= 12 &&
            (song[8] | (song[9] << 8) | ((uint32_t)song[10] << 16) |
             ((uint32_t)song[11] << 24)) == 1024) {
        fprintf(stderr,
            "error: %s looks like an ADPCM blob - it is already playable\n"
            "       as-is ('-o file.bin' from mp3_to_dsm.py --format adpcm,\n"
            "       or tqv.py load/send it directly); L4Z-compressing ADPCM\n"
            "       gains nothing and the result plays as DSM noise\n",
            inpath);
        return 1;
    }

    uint32_t want = seconds > 0 ? (uint32_t)(rate / 8.0 * seconds) : total;
    if (want > total)
        want = total;
    want -= want % 3;                       // whole 24-bit samples
    printf("taking %u bytes (%.2f s, %u samples)\n",
           want, want * 8.0 / rate, want / 3);

    uint8_t  *blob = malloc(want + want / 128 + (want / BLK + 2) * 8 + 64);
    uint32_t o = 0;
    blob[o++] = (uint8_t)want;  blob[o++] = (uint8_t)(want >> 8);
    blob[o++] = (uint8_t)(want >> 16); blob[o++] = (uint8_t)(want >> 24);
    blob[o++] = (uint8_t)BLK;   blob[o++] = (uint8_t)(BLK >> 8);
    blob[o++] = (uint8_t)(BLK >> 16); blob[o++] = (uint8_t)(BLK >> 24);

    static uint8_t comp[BLK + BLK / 2], back[BLK];
    uint32_t stored_blocks = 0, blocks = 0;
    for (uint32_t off = 0; off < want; off += BLK) {
        uint32_t n = want - off < BLK ? want - off : BLK;
        uint32_t clen = l4z_compress_block(song + off, n, comp, n - 1);
        ++blocks;
        if (clen >= n / 2) {                // store unless the win is >= 2x:
            // marginally compressed blocks decode slowly (short match
            // byte copies), while big winners are RLE-like and fast.  The
            // stored payload is padded to 4-byte blob alignment so the
            // target can unpack it straight from flash with word loads.
            blob[o++] = 0xFF; blob[o++] = 0xFF;
            while (o & 3)
                blob[o++] = 0;
            memcpy(blob + o, song + off, n);
            o += n;
            ++stored_blocks;
        } else {
            // verify before committing
            uint32_t dn = l4z_decode(comp, clen, back);
            if (dn != n || memcmp(back, song + off, n)) {
                fprintf(stderr, "VERIFY FAILED at block offset %u\n", off);
                return 1;
            }
            blob[o++] = (uint8_t)clen; blob[o++] = (uint8_t)(clen >> 8);
            memcpy(blob + o, comp, clen);
            o += clen;
        }
    }

    printf("compressed: %u -> %u bytes (%.1f%%), %u blocks (%u stored)\n",
           want, o, 100.0 * o / want, blocks, stored_blocks);
    printf("all compressed blocks verified against the reference decoder\n");

    if (strlen(outpath) > 4 &&
            !strcmp(outpath + strlen(outpath) - 4, ".bin")) {
        FILE *fb = fopen(outpath, "wb");    // raw blob for tqv.py send
        if (!fb) { perror(outpath); return 1; }
        fwrite(blob, 1, o, fb);
        fclose(fb);
        printf("wrote %s (raw blob)\n", outpath);
        return 0;
    }

    FILE *f = fopen(outpath, "w");
    if (!f) { perror(outpath); return 1; }
    fprintf(f,
        "// Generated by tools/l4z from %s\n"
        "// First %.2f s (%u samples) as independently compressed blocks:\n"
        "// %u -> %u bytes (%.1f%%), block size %u, %u blocks (%u stored)\n"
        "// Layout: u32 total, u32 block size, then {u16 clen, data} per\n"
        "// block; clen 0xFFFF = stored.  Decoder: play.c l4z_decode().\n\n"
        "#include <stdint.h>\n\n"
        "const uint32_t %s_l4z_rate = %uu;\n"
        "const uint32_t %s_l4z_size = %uu;\n"
        "const uint8_t %s_l4z[] __attribute__((aligned(4))) = {\n",
        inpath, want * 8.0 / rate, want / 3, want, o, 100.0 * o / want,
        BLK, blocks, stored_blocks, name, rate, name, o, name);
    for (uint32_t i = 0; i < o; ++i)
        fprintf(f, "%s0x%02x,%s", i % 16 == 0 ? "    " : " ",
                blob[i], i % 16 == 15 ? "\n" : "");
    if (o % 16)
        fprintf(f, "\n");
    fprintf(f, "};\n");
    fclose(f);
    printf("wrote %s\n", outpath);
    return 0;
}
