/*
 * Copyright 2010 Damjan Jovanovic
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#include <stdarg.h>
#include <math.h>

#define COBJMACROS

#include "windef.h"
#include "objbase.h"
#include "wincodec.h"
#include "wine/test.h"

#pragma pack(push,1)

struct ICONHEADER
{
    WORD idReserved;
    WORD idType;
    WORD idCount;
};

struct ICONDIRENTRY
{
    BYTE bWidth;
    BYTE bHeight;
    BYTE bColorCount;
    BYTE bReserved;
    WORD wPlanes;
    WORD wBitCount;
    DWORD dwDIBSize;
    DWORD dwDIBOffset;
};

struct test_ico
{
    struct ICONHEADER header;
    struct ICONDIRENTRY direntry;
    BITMAPINFOHEADER bmi;
    unsigned char data[512];
};

static const struct test_ico ico_1 =
{
    /* ICONHEADER */
    {
      0, /* reserved */
      1, /* type */
      1, /* count */
    },
    /* ICONDIRENTRY */
    {
      16, /* width */
      16, /* height */
      2, /* color count */
      0, /* reserved */
      1, /* planes */
      8, /* bitcount*/
      40 + 2*4 + 16 * 16 + 16 * 4, /* data size */
      22 /* data offset */
    },
    /* BITMAPINFOHEADER */
    {
      sizeof(BITMAPINFOHEADER), /* header size */
      16, /* width */
      2*16, /* height (XOR+AND rows) */
      1, /* planes */
      8, /* bit count */
      0, /* compression */
      0, /* sizeImage */
      0, /* x pels per meter */
      0, /* y pels per meter */
      2, /* clrUsed */
      0, /* clrImportant */
    },
    {
      /* palette */
      0,0,0,0,
      0xFF,0xFF,0xFF,0,
      /* XOR mask */
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,
      0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,
      0,1,0,0,0,0,0,0,0,0,0,0,0,1,0,0,
      0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,
      0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,
      0,0,1,0,0,0,0,0,0,0,0,0,1,0,0,0,
      0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,
      0,0,0,1,0,0,0,0,0,0,0,1,0,0,0,0,
      0,0,0,1,0,0,0,1,0,0,0,1,0,0,0,0,
      0,0,0,0,1,0,1,0,1,0,1,0,0,0,0,0,
      0,0,0,0,0,1,0,0,0,1,0,0,0,0,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
      /* AND mask */
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0,
      0,0,0,0
    }
};

#pragma pack(pop)

#define test_ico_data(a, b, c) test_ico_data_(a, b, c,  0, __LINE__)
#define test_ico_data_todo(a, b, c) test_ico_data_(a, b, c, 1, __LINE__)
static void test_ico_data_(void *data, DWORD data_size, HRESULT init_hr, int todo, unsigned int line)
{
    IWICBitmapDecoder *decoder;
    IWICImagingFactory *factory;
    HRESULT hr;
    IWICStream *icostream;
    IWICBitmapFrameDecode *framedecode = NULL;
    UINT count;

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
        &IID_IWICImagingFactory, (void**)&factory);
    ok(hr == S_OK, "CoCreateInstance failed, hr=%lx\n", hr);
    if (FAILED(hr)) return;

    hr = IWICImagingFactory_CreateStream(factory, &icostream);
    ok(hr == S_OK, "CreateStream failed, hr=%lx\n", hr);
    if (SUCCEEDED(hr))
    {
        hr = IWICStream_InitializeFromMemory(icostream, data, data_size);
        ok(hr == S_OK, "InitializeFromMemory failed, hr=%lx\n", hr);

        if (SUCCEEDED(hr))
        {
            hr = CoCreateInstance(&CLSID_WICIcoDecoder, NULL, CLSCTX_INPROC_SERVER,
                &IID_IWICBitmapDecoder, (void**)&decoder);
            ok(hr == S_OK, "CoCreateInstance failed, hr=%lx\n", hr);
        }

        if (SUCCEEDED(hr))
        {
            hr = IWICBitmapDecoder_Initialize(decoder, (IStream*)icostream,
                WICDecodeMetadataCacheOnDemand);
            todo_wine_if(todo)
            ok_(__FILE__, line)(hr == init_hr, "Initialize failed, hr=%lx\n", hr);

            if (SUCCEEDED(hr))
            {
                hr = IWICBitmapDecoder_GetFrameCount(decoder, &count);
                ok(hr == S_OK, "GetFrameCount failed, hr=%lx\n", hr);
                ok(count == 1, "Expected 1 frame, got %u\n", count);
                hr = IWICBitmapDecoder_GetFrame(decoder, count, &framedecode);
                ok(hr == WINCODEC_ERR_FRAMEMISSING, "Expected WINCODEC_ERR_FRAMEMISSING from GetFrame, got %lx\n", hr);
                hr = IWICBitmapDecoder_GetFrame(decoder, 0, &framedecode);
                ok(hr == S_OK, "GetFrame failed, hr=%lx\n", hr);
            }

            if (SUCCEEDED(hr))
            {
                UINT width, height;
                IWICBitmapSource *thumbnail;

                width = height = 0;
                hr = IWICBitmapFrameDecode_GetSize(framedecode, &width, &height);
                ok(hr == S_OK, "GetFrameSize failed, hr=%lx\n", hr);
                ok(width == 16 && height == 16, "framesize=%ux%u\n", width, height);

                hr = IWICBitmapFrameDecode_GetThumbnail(framedecode, &thumbnail);
                ok(hr == S_OK, "GetThumbnail failed, hr=%lx\n", hr);
                if (hr == S_OK)
                {
                    width = height = 0;
                    hr = IWICBitmapSource_GetSize(thumbnail, &width, &height);
                    ok(hr == S_OK, "GetFrameSize failed, hr=%lx\n", hr);
                    ok(width == 16 && height == 16, "framesize=%ux%u\n", width, height);
                    IWICBitmapSource_Release(thumbnail);
                }
                IWICBitmapFrameDecode_Release(framedecode);
            }

            IWICBitmapDecoder_Release(decoder);
        }

        IWICStream_Release(icostream);
    }

    IWICImagingFactory_Release(factory);
}

static void test_decoder(void)
{
    struct test_ico ico;

    /* Icon size specified in ICONDIRENTRY does not match bitmap header. */
    ico = ico_1;
    ico.direntry.bWidth = 2;
    ico.direntry.bHeight = 2;
    test_ico_data(&ico, sizeof(ico), S_OK);

    /* Invalid DIRENTRY data size/offset. */
    ico = ico_1;
    ico.direntry.dwDIBOffset = sizeof(ico);
    test_ico_data(&ico, sizeof(ico), WINCODEC_ERR_BADIMAGE);

    ico = ico_1;
    ico.direntry.dwDIBSize = sizeof(ico);
    test_ico_data(&ico, sizeof(ico), WINCODEC_ERR_BADIMAGE);

    /* Header fields validation. */
    ico = ico_1;
    ico.header.idReserved = 1;
    test_ico_data_todo(&ico, sizeof(ico), S_OK);
    ico.header.idReserved = 0;
    ico.header.idType = 100;
    test_ico_data_todo(&ico, sizeof(ico), S_OK);

    /* Premature end of data. */
    ico = ico_1;
    test_ico_data(&ico, sizeof(ico.header) - 1, WINCODEC_ERR_STREAMREAD);
    test_ico_data(&ico, sizeof(ico.header) + sizeof(ico.direntry) - 1, WINCODEC_ERR_BADIMAGE);
}

struct frame_desc
{
    BOOL png;
    UINT size;
    UINT bpp;
};

static void frame_color(UINT index, BYTE *bgr)
{
    bgr[0] = 0x20 + index * 0x0d;
    bgr[1] = 0xf0 - index * 0x0b;
    bgr[2] = 0x30 + index * 0x17;
}

static DWORD crc32_update(DWORD crc, const BYTE *data, DWORD len)
{
    unsigned int i;

    crc = ~crc;
    while (len--)
    {
        crc ^= *data++;
        for (i = 0; i < 8; i++)
            crc = (crc >> 1) ^ (0xedb88320 & (0u - (crc & 1)));
    }
    return ~crc;
}

static BYTE *put_be32(BYTE *p, DWORD value)
{
    p[0] = value >> 24;
    p[1] = value >> 16;
    p[2] = value >> 8;
    p[3] = value;
    return p + 4;
}

static BYTE *put_png_chunk(BYTE *p, const char *type, const BYTE *data, DWORD len)
{
    BYTE *start = p + 4;

    p = put_be32(p, len);
    memcpy(p, type, 4);
    p += 4;
    if (len) memmove(p, data, len);
    p += len;
    return put_be32(p, crc32_update(0, start, len + 4));
}

/* Solid colour RGBA PNG, with the image data in stored (uncompressed) deflate blocks. */
static BYTE *build_png(UINT size, const BYTE *bgr, DWORD *png_size)
{
    static const BYTE signature[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    DWORD row_len = 1 + size * 4, raw_len = row_len * size, blocks = (raw_len + 0xfffe) / 0xffff;
    DWORD zlib_len = 2 + blocks * 5 + raw_len + 4, a = 1, b = 0, i, done;
    BYTE *raw, *zlib, *png, *p, ihdr[13];

    raw = malloc(raw_len);
    for (i = 0; i < raw_len; i++)
    {
        DWORD x = i % row_len;
        if (!x) raw[i] = 0;
        else if ((x - 1) % 4 == 3) raw[i] = 0xff;
        else raw[i] = bgr[2 - (x - 1) % 4];
    }

    zlib = p = malloc(zlib_len);
    *p++ = 0x78;
    *p++ = 0x01;
    for (done = 0; done < raw_len; done += 0xffff)
    {
        DWORD len = min(raw_len - done, 0xffff);
        *p++ = (done + len == raw_len);
        *p++ = len & 0xff;
        *p++ = len >> 8;
        *p++ = ~len & 0xff;
        *p++ = (~len >> 8) & 0xff;
        memcpy(p, raw + done, len);
        p += len;
    }
    for (i = 0; i < raw_len; i++)
    {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    put_be32(p, (b << 16) | a);

    put_be32(ihdr, size);
    put_be32(ihdr + 4, size);
    ihdr[8] = 8;  /* bit depth */
    ihdr[9] = 6;  /* RGBA */
    ihdr[10] = ihdr[11] = ihdr[12] = 0;

    *png_size = sizeof(signature) + (12 + sizeof(ihdr)) + (12 + zlib_len) + 12;
    png = p = malloc(*png_size);
    memcpy(p, signature, sizeof(signature));
    p += sizeof(signature);
    p = put_png_chunk(p, "IHDR", ihdr, sizeof(ihdr));
    p = put_png_chunk(p, "IDAT", zlib, zlib_len);
    put_png_chunk(p, "IEND", NULL, 0);

    free(zlib);
    free(raw);
    return png;
}

/* Solid colour DIB icon image (XOR bitmap + empty AND mask). */
static BYTE *build_dib(UINT size, UINT bpp, const BYTE *bgr, DWORD *dib_size)
{
    DWORD colors = bpp <= 8 ? 1u << bpp : 0, xor_stride = ((size * bpp + 31) / 32) * 4;
    DWORD and_stride = ((size + 31) / 32) * 4, i;
    BITMAPINFOHEADER *bih;
    BYTE *dib, *p;

    *dib_size = sizeof(*bih) + colors * 4 + xor_stride * size + and_stride * size;
    dib = calloc(1, *dib_size);
    bih = (BITMAPINFOHEADER *)dib;
    bih->biSize = sizeof(*bih);
    bih->biWidth = size;
    bih->biHeight = size * 2;
    bih->biPlanes = 1;
    bih->biBitCount = bpp;
    bih->biCompression = BI_RGB;
    bih->biClrUsed = colors;

    p = dib + sizeof(*bih);
    if (colors)
    {
        /* every pixel uses palette entry 1 */
        memcpy(p + 4, bgr, 3);
        p += colors * 4;
        for (i = 0; i < xor_stride * size; i++)
            p[i] = bpp == 4 ? 0x11 : (bpp == 8 ? 0x01 : 0x55);
    }
    else
    {
        BYTE pixel[4] = {bgr[0], bgr[1], bgr[2], 0xff};
        UINT bytes = bpp / 8, x, y;

        if (bpp == 16)
        {
            WORD rgb555 = ((bgr[2] >> 3) << 10) | ((bgr[1] >> 3) << 5) | (bgr[0] >> 3);
            memcpy(pixel, &rgb555, sizeof(rgb555));
        }
        for (y = 0; y < size; y++)
            for (x = 0; x < size; x++)
                memcpy(p + y * xor_stride + x * bytes, pixel, bytes);
    }
    return dib;
}

static UINT parse_layout(const char *layout, struct frame_desc *frames, UINT max)
{
    UINT count = 0;
    char *end;

    while (*layout && count < max)
    {
        while (*layout == ' ') layout++;
        if (!*layout) break;
        frames[count].png = (*layout++ == 'p');
        frames[count].size = strtoul(layout, &end, 10);
        frames[count].bpp = 32;
        if (*end == '/') frames[count].bpp = strtoul(end + 1, &end, 10);
        layout = end;
        count++;
    }
    return count;
}

static BYTE *build_ico(const struct frame_desc *frames, UINT count, DWORD *ico_size)
{
    struct ICONHEADER *header;
    struct ICONDIRENTRY *entry;
    BYTE *images[16], *ico;
    DWORD sizes[16], offset;
    UINT i;

    *ico_size = offset = sizeof(*header) + count * sizeof(*entry);
    for (i = 0; i < count; i++)
    {
        BYTE bgr[3];
        frame_color(i, bgr);
        if (frames[i].png) images[i] = build_png(frames[i].size, bgr, &sizes[i]);
        else images[i] = build_dib(frames[i].size, frames[i].bpp, bgr, &sizes[i]);
        *ico_size += sizes[i];
    }

    ico = malloc(*ico_size);
    header = (struct ICONHEADER *)ico;
    header->idReserved = 0;
    header->idType = 1;
    header->idCount = count;
    entry = (struct ICONDIRENTRY *)(header + 1);
    for (i = 0; i < count; i++)
    {
        entry[i].bWidth = frames[i].size;
        entry[i].bHeight = frames[i].size;
        entry[i].bColorCount = frames[i].bpp < 8 ? 1 << frames[i].bpp : 0;
        entry[i].bReserved = 0;
        entry[i].wPlanes = 1;
        entry[i].wBitCount = frames[i].bpp;
        entry[i].dwDIBSize = sizes[i];
        entry[i].dwDIBOffset = offset;
        memcpy(ico + offset, images[i], sizes[i]);
        offset += sizes[i];
        free(images[i]);
    }
    return ico;
}

/* Find which entry of the file a decoded frame is, by its size and, for
 * entries of the same size, by its colour. */
static int identify_frame(IWICBitmapFrameDecode *frame, const struct frame_desc *frames, UINT count)
{
    UINT width = 0, height = 0, i, matches = 0;
    int found = -1;
    WICRect rect;
    BYTE pixel[4];
    HRESULT hr;

    hr = IWICBitmapFrameDecode_GetSize(frame, &width, &height);
    ok(hr == S_OK, "GetSize error %#lx\n", hr);
    for (i = 0; i < count; i++)
        if (frames[i].size == width) { found = i; matches++; }
    if (matches <= 1) return found;

    rect.X = width / 2;
    rect.Y = height / 2;
    rect.Width = rect.Height = 1;
    hr = IWICBitmapFrameDecode_CopyPixels(frame, &rect, 4, sizeof(pixel), pixel);
    ok(hr == S_OK, "CopyPixels error %#lx\n", hr);
    for (i = 0; i < count; i++)
    {
        BYTE bgr[3];
        frame_color(i, bgr);
        if (frames[i].size == width && !memcmp(pixel, bgr, 3)) return i;
    }
    return -1;
}

#define test_frame_order(a, b) test_frame_order_(a, b, __LINE__)
static void test_frame_order_(const char *layout, const char *expected, unsigned int line)
{
    IWICImagingFactory *factory;
    IWICBitmapDecoder *decoder;
    IWICBitmapFrameDecode *frame;
    struct frame_desc frames[16];
    IWICStream *stream;
    UINT count, frame_count, i;
    char order[64], *p = order;
    DWORD ico_size;
    BYTE *ico;
    HRESULT hr;

    count = parse_layout(layout, frames, ARRAY_SIZE(frames));
    ico = build_ico(frames, count, &ico_size);

    hr = CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
            &IID_IWICImagingFactory, (void **)&factory);
    ok(hr == S_OK, "CoCreateInstance error %#lx\n", hr);
    hr = IWICImagingFactory_CreateStream(factory, &stream);
    ok(hr == S_OK, "CreateStream error %#lx\n", hr);
    hr = IWICStream_InitializeFromMemory(stream, ico, ico_size);
    ok(hr == S_OK, "InitializeFromMemory error %#lx\n", hr);
    hr = CoCreateInstance(&CLSID_WICIcoDecoder, NULL, CLSCTX_INPROC_SERVER,
            &IID_IWICBitmapDecoder, (void **)&decoder);
    ok(hr == S_OK, "CoCreateInstance error %#lx\n", hr);
    hr = IWICBitmapDecoder_Initialize(decoder, (IStream *)stream, WICDecodeMetadataCacheOnDemand);
    ok_(__FILE__, line)(hr == S_OK, "%s: Initialize error %#lx\n", layout, hr);

    hr = IWICBitmapDecoder_GetFrameCount(decoder, &frame_count);
    ok(hr == S_OK, "GetFrameCount error %#lx\n", hr);
    ok_(__FILE__, line)(frame_count == count, "%s: got %u frames\n", layout, frame_count);

    *p = 0;
    for (i = 0; i < frame_count; i++)
    {
        hr = IWICBitmapDecoder_GetFrame(decoder, i, &frame);
        ok(hr == S_OK, "GetFrame %u error %#lx\n", i, hr);
        if (FAILED(hr)) break;
        p += sprintf(p, "%s%d", i ? " " : "", identify_frame(frame, frames, count));
        IWICBitmapFrameDecode_Release(frame);
    }

    ok_(__FILE__, line)(!strcmp(order, expected), "%s: got frame order %s, expected %s\n",
            layout, order, expected);

    IWICBitmapDecoder_Release(decoder);
    IWICStream_Release(stream);
    IWICImagingFactory_Release(factory);
    free(ico);
}

static void test_frames(void)
{
    /* GetFrame() does not return the frames in file order: the decoder sorts
     * them, and the result depends on the order of the directory entries. The
     * expected orders are the ones Windows returns. */

    /* DIB, 32 bpp. */
    test_frame_order("d16 d48", "1 0");
    test_frame_order("d48 d16", "0 1");
    test_frame_order("d32 d48", "1 0");
    test_frame_order("d48 d32", "1 0");
    test_frame_order("d64 d60 d56", "2 1 0");
    test_frame_order("d64 d60 d56 d52", "1 0 3 2");
    test_frame_order("d64 d60 d56 d52 d48", "0 1 2 3 4");
    test_frame_order("d16 d24 d32 d48 d64 d96 d128", "5 4 6 3 2 1 0");
    test_frame_order("d128 d96 d72 d64 d48 d32 d24 d16", "2 1 0 3 4 5 6 7");
    test_frame_order("d46 d33 d112 d50 d29 d9", "2 3 0 1 4 5");
    test_frame_order("d21 d33 d76 d106 d12 d111 d89 d112 d75", "8 5 6 3 7 2 1 0 4");
    test_frame_order("d39 d80 d38 d78 d81 d96 d8 d72 d59 d22 d42", "5 1 7 8 3 4 10 0 2 9 6");
    test_frame_order("d25 d20 d31 d14 d23 d34 d28 d37 d38 d27 d19 d36 d33 d80",
            "13 8 7 11 5 12 2 6 9 0 4 1 10 3");

    /* With a 256x256 PNG image. */
    test_frame_order("p256 d128 d96 d72 d64 d48 d32 d24 d16", "1 3 2 4 5 0 6 7 8");
    test_frame_order("d128 d96 d72 d64 d48 d32 d24 d16 p256", "0 2 3 1 4 8 5 6 7");
    test_frame_order("p256 d16 d24 d32 d48 d64 d96 d128", "7 6 5 4 0 3 2 1");
    test_frame_order("p256 d48 d32 d16", "1 2 3 0");
    test_frame_order("d16 d32 d48 p256", "1 2 0 3");
    test_frame_order("p256 d16", "1 0");
    test_frame_order("d256 p256", "1 0");
    test_frame_order("d43 d128 p256", "1 0 2");
    test_frame_order("d43 p64 d128", "2 0 1");
    test_frame_order("p256 d256", "0 1");

    /* Same size, different bit depth. */
    test_frame_order("d32/4 d32/32 d32/8", "1 2 0");
    test_frame_order("d32/8 d32/4 d32/32", "2 0 1");
    test_frame_order("d48/4 d32/4 d16/4 d48/8 d32/8 d16/8 d48 d32 d16", "7 6 8 3 4 0 1 5 2");
    test_frame_order("d16 d32 d48 d16/8 d32/8 d48/8 d16/4 d32/4 d48/4", "2 1 0 4 5 8 7 3 6");
    test_frame_order("p256 d48 d32 d16 d48/8 d32/8 d16/8 d48/4 d32/4 d16/4", "1 2 3 4 5 8 7 6 9 0");

    /* PNG images with DIBs of less than 32 bpp only, and PNG images only. */
    test_frame_order("d128/8 d40/8 p48", "0 1 2");
    test_frame_order("d211/8 p168", "0 1");
    test_frame_order("p115 d71/1", "1 0");
    test_frame_order("p96 d128/4 d55/4 d32/1 d200/16 p20", "4 1 0 2 3 5");
    test_frame_order("p32 p48 p256", "0 1 2");
}

START_TEST(icoformat)
{
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    test_decoder();
    test_frames();

    CoUninitialize();
}
