/* -*- C++ -*-
 * Copyright 2019-2025 LibRaw LLC (info@libraw.org)
 *
 LibRaw uses code from dcraw.c -- Dave Coffin's raw photo decoder,
 dcraw.c is copyright 1997-2018 by Dave Coffin, dcoffin a cybercom o net.
 LibRaw do not use RESTRICTED code from dcraw.c

 LibRaw is free software; you can redistribute it and/or modify
 it under the terms of the one of two licenses as you choose:

1. GNU LESSER GENERAL PUBLIC LICENSE version 2.1
   (See file LICENSE.LGPL provided in LibRaw distribution archive for details).

2. COMMON DEVELOPMENT AND DISTRIBUTION LICENSE (CDDL) Version 1.0
   (See file LICENSE.CDDL provided in LibRaw distribution archive for details).

 */

#if defined(_WIN32) && defined(_OPENMP) && (defined(__GNUC__) || defined(__clang__))
// MinGW builds pass -fopenmp, but LibRaw's Windows gate only enables it for MSVC/Intel.
#define LIBRAW_FORCE_OPENMP
#endif

#include "../../internal/dcraw_defs.h"

#ifdef LIBRAW_USE_OPENMP
#include <atomic>
#include <vector>

namespace
{

struct LosslessDngTile
{
  INT64 offset;
  INT64 bytes;
  unsigned row;
  unsigned col;

  LosslessDngTile() : offset(0), bytes(0), row(0), col(0) {}
};

std::atomic<int> activeLosslessDngParallel(0);

bool read_u32_at(LibRaw_abstract_datastream *stream, short byte_order, INT64 pos, INT64 size, INT64 &value)
{
  if (!stream || pos < 0 || pos + 4 > size)
    return false;

  uchar data[4] = {0, 0, 0, 0};
  stream->seek(pos, SEEK_SET);
  if (stream->read(data, 1, 4) != 4)
    return false;

  value = libraw_sget4_static(byte_order, data);
  return true;
}

} // namespace
#endif

void LibRaw::vc5_dng_load_raw_placeholder()
{
    // placeholder only, real decoding implemented in GPR SDK
    if(!dng_version)
    	throw LIBRAW_EXCEPTION_IO_EOF; // never reached
    throw LIBRAW_EXCEPTION_UNSUPPORTED_FORMAT;
}
void LibRaw::jxl_dng_load_raw_placeholder()
{
  // placeholder only, real decoding implemented in DNG SDK
  throw LIBRAW_EXCEPTION_UNSUPPORTED_FORMAT;
}

void LibRaw::adobe_copy_pixel(unsigned row, unsigned col, ushort **rp)
{
  int c;

  if (tiff_samples == 2 && shot_select)
    (*rp)++;
  if (raw_image)
  {
    if (row < raw_height && col < raw_width)
      RAW(row, col) = curve[**rp];
    *rp += tiff_samples;
  }
  else
  {
    if (row < raw_height && col < raw_width)
      FORC(int(tiff_samples))
    image[row * raw_width + col][c] = curve[(*rp)[c]];
    *rp += tiff_samples;
  }
  if (tiff_samples == 2 && shot_select)
    (*rp)--;
}

void LibRaw::lossless_dng_decode_tile(const uchar *tile_data, size_t tile_size, unsigned trow, unsigned tcol)
{
  if (!tile_data || !tile_size)
    throw LIBRAW_EXCEPTION_IO_CORRUPT;

  unsigned jwide, jrow, jcol, row, col, i, j;
  struct jhead jh;
  ushort *rp;
  LibRaw_buffer_datastream tile_stream(tile_data, tile_size);

  libraw_internal_data.internal_data.input = &tile_stream;
  libraw_internal_data.internal_data.input_internal = 0;

  if (!ljpeg_start(&jh, 0))
  {
    libraw_internal_data.internal_data.input = NULL;
    throw LIBRAW_EXCEPTION_IO_CORRUPT;
  }

  jwide = jh.wide;
  if (filters || colors == 1)
    jwide *= jh.clrs;

  if (filters && (tiff_samples == 2)) // Fuji Super CCD
      jwide /= 2;

  try
  {
    switch (jh.algo)
    {
    case 0xc1:
      jh.vpred[0] = 16384;
      getbits(-1);
      for (jrow = 0; jrow + 7 < (unsigned)jh.high; jrow += 8)
      {
        checkCancel();
        for (jcol = 0; jcol + 7 < (unsigned)jh.wide; jcol += 8)
        {
          ljpeg_idct(&jh);
          rp = jh.idct;
          row = trow + jcol / tile_width + jrow * 2;
          col = tcol + jcol % tile_width;
          for (i = 0; i < 16; i += 2)
            for (j = 0; j < 8; j++)
              adobe_copy_pixel(row + i, col + j, &rp);
        }
      }
      break;
    case 0xc3:
      for (row = col = jrow = 0; jrow < (unsigned)jh.high; jrow++)
      {
        checkCancel();
        rp = ljpeg_row(jrow, &jh);
        if (tiff_samples == 1 && jh.clrs > 1 && jh.clrs * jwide == raw_width)
          for (jcol = 0; jcol < jwide * jh.clrs; jcol++)
          {
            adobe_copy_pixel(trow + row, tcol + col, &rp);
            if (++col >= tile_width || col >= raw_width)
              row += 1 + (col = 0);
          }
        else
          for (jcol = 0; jcol < jwide; jcol++)
          {
            adobe_copy_pixel(trow + row, tcol + col, &rp);
            if (++col >= tile_width || col >= raw_width)
              row += 1 + (col = 0);
          }
      }
      break;
    default:
      throw LIBRAW_EXCEPTION_IO_CORRUPT;
    }
  }
  catch (...)
  {
    ljpeg_end(&jh);
    libraw_internal_data.internal_data.input = NULL;
    throw;
  }

  ljpeg_end(&jh);
  libraw_internal_data.internal_data.input = NULL;
}

bool LibRaw::lossless_dng_load_raw_parallel()
{
#ifndef LIBRAW_USE_OPENMP
  return false;
#else
  if (!raw_image || tiff_samples != 1 || tile_length >= INT_MAX ||
      tile_width == 0 || tile_length == 0 || tile_width >= INT_MAX ||
      omp_in_parallel())
    return false;

  const int iifd = find_ifd_by_offset(data_offset);
  if (iifd < 0 || iifd >= (int)tiff_nifds)
    return false;

  const tiff_ifd_t *ifd = &tiff_ifd[iifd];
  if (ifd->samples != 1 || ifd->offset != data_offset || ifd->bytes <= 0)
    return false;

  const unsigned tiles_h = (raw_width + tile_width - 1) / tile_width;
  const unsigned tiles_v = (raw_height + tile_length - 1) / tile_length;
  const unsigned tile_count = tiles_h * tiles_v;
  if (tile_count < 8 || tile_count > 100000)
    return false;

  const INT64 stream_size = ifp ? ifp->size() : 0;
  if (stream_size <= 0)
    return false;

  std::vector<LosslessDngTile> tiles(tile_count);
  INT64 max_tile_bytes = 0;
  INT64 total_tile_bytes = 0;
  for (unsigned t = 0; t < tile_count; ++t)
  {
    INT64 offset = 0;
    INT64 bytes = 0;
    if (!read_u32_at(ifp, order, data_offset + INT64(t) * 4, stream_size, offset) ||
        !read_u32_at(ifp, order, ifd->bytes + INT64(t) * 4, stream_size, bytes))
      return false;

    if (offset <= 0 || bytes <= 0 || bytes > INT_MAX || offset + bytes > stream_size)
      return false;

    tiles[t].offset = offset;
    tiles[t].bytes = bytes;
    tiles[t].row = (t / tiles_h) * tile_length;
    tiles[t].col = (t % tiles_h) * tile_width;
    max_tile_bytes = MAX(max_tile_bytes, bytes);
    total_tile_bytes += bytes;
  }

  if (max_tile_bytes <= 0 || max_tile_bytes > INT64(imgdata.rawparams.max_raw_memory_mb) * INT64(1024 * 1024))
    return false;

  const int max_threads = omp_get_max_threads();
  const int threads = MIN(4, max_threads);
  if (threads < 2)
    return false;

  if (activeLosslessDngParallel.fetch_add(1) != 0)
  {
    activeLosslessDngParallel.fetch_sub(1);
    return false;
  }

  struct ParallelGuard
  {
    ~ParallelGuard() { activeLosslessDngParallel.fetch_sub(1); }
  } guard;

  std::atomic<int> decode_error(0);

#pragma omp parallel for schedule(dynamic, 1) num_threads(threads)
  for (int ti = 0; ti < (int)tile_count; ++ti)
  {
    if (decode_error.load())
      continue;

    try
    {
      const LosslessDngTile &tile = tiles[ti];
      std::vector<uchar> tile_buffer((size_t)tile.bytes);

#pragma omp critical(libraw_lossless_dng_tile_read)
      {
        if (!decode_error.load())
        {
          ifp->seek(tile.offset, SEEK_SET);
          if (ifp->read(tile_buffer.data(), 1, (size_t)tile.bytes) != (int)tile.bytes)
            decode_error.store(LIBRAW_EXCEPTION_IO_EOF);
        }
      }

      if (decode_error.load())
        continue;

      ushort *shared_raw_image = raw_image;
      LibRaw tile_decoder(LIBRAW_OPTIONS_NO_DATAERR_CALLBACK);
      tile_decoder.imgdata.idata = imgdata.idata;
#pragma push_macro("xmpdata")
#undef xmpdata
      tile_decoder.imgdata.idata.xmpdata = NULL;
#pragma pop_macro("xmpdata")
      tile_decoder.imgdata.sizes = imgdata.sizes;
      tile_decoder.imgdata.color = imgdata.color;
      tile_decoder.imgdata.color.profile = NULL;
      tile_decoder.imgdata.params = imgdata.params;
      tile_decoder.imgdata.params.output_profile = NULL;
      tile_decoder.imgdata.params.camera_profile = NULL;
      tile_decoder.imgdata.params.bad_pixels = NULL;
      tile_decoder.imgdata.params.dark_frame = NULL;
      tile_decoder.imgdata.rawparams = imgdata.rawparams;
      tile_decoder.imgdata.rawparams.custom_camera_strings = NULL;
#pragma push_macro("raw_image")
#undef raw_image
      tile_decoder.imgdata.rawdata.raw_image = shared_raw_image;
#pragma pop_macro("raw_image")
      tile_decoder.imgdata.rawdata.raw_alloc = NULL;
      tile_decoder.libraw_internal_data.internal_output_params =
          libraw_internal_data.internal_output_params;
      tile_decoder.libraw_internal_data.unpacker_data =
          libraw_internal_data.unpacker_data;

      tile_decoder.lossless_dng_decode_tile(tile_buffer.data(), tile_buffer.size(), tile.row, tile.col);
#pragma push_macro("raw_image")
#undef raw_image
      tile_decoder.imgdata.rawdata.raw_image = NULL;
#pragma pop_macro("raw_image")
    }
    catch (int err)
    {
      decode_error.store(err);
    }
    catch (const std::bad_alloc&)
    {
      decode_error.store(LIBRAW_EXCEPTION_ALLOC);
    }
    catch (...)
    {
      decode_error.store(LIBRAW_EXCEPTION_DECODE_RAW);
    }
  }

  if (decode_error.load() == LIBRAW_EXCEPTION_CANCELLED_BY_CALLBACK)
    throw LIBRAW_EXCEPTION_CANCELLED_BY_CALLBACK;

  if (decode_error.load())
    return false;

  if (total_tile_bytes <= 0)
    return false;

  ifp->seek(data_offset, SEEK_SET);
  return true;
#endif
}

void LibRaw::lossless_dng_load_raw()
{
  unsigned trow = 0, tcol = 0, jwide, jrow, jcol, row, col, i, j;
  INT64 save;
  struct jhead jh;
  ushort *rp;

  int ss = shot_select;
  shot_select = libraw_internal_data.unpacker_data.dng_frames[LIM(ss,0,(LIBRAW_IFD_MAXCOUNT*2-1))] & 0xff;

  if (lossless_dng_load_raw_parallel())
  {
    shot_select = ss;
    return;
  }

  while (trow < raw_height)
  {
    checkCancel();
    save = ftell(ifp);
    if (tile_length < INT_MAX)
      fseek(ifp, get4(), SEEK_SET);
    if (!ljpeg_start(&jh, 0))
      break;
    jwide = jh.wide;
    if (filters || colors == 1)
      jwide *= jh.clrs;

    if(filters && (tiff_samples == 2)) // Fuji Super CCD
        jwide /= 2;
    try
    {
      switch (jh.algo)
      {
      case 0xc1:
        jh.vpred[0] = 16384;
        getbits(-1);
        for (jrow = 0; jrow + 7 < (unsigned)jh.high; jrow += 8)
        {
          checkCancel();
          for (jcol = 0; jcol + 7 < (unsigned)jh.wide; jcol += 8)
          {
            ljpeg_idct(&jh);
            rp = jh.idct;
            row = trow + jcol / tile_width + jrow * 2;
            col = tcol + jcol % tile_width;
            for (i = 0; i < 16; i += 2)
              for (j = 0; j < 8; j++)
                adobe_copy_pixel(row + i, col + j, &rp);
          }
        }
        break;
      case 0xc3:
        for (row = col = jrow = 0; jrow < (unsigned)jh.high; jrow++)
        {
          checkCancel();
          rp = ljpeg_row(jrow, &jh);
          if (tiff_samples == 1 && jh.clrs > 1 && jh.clrs * jwide == raw_width)
            for (jcol = 0; jcol < jwide * jh.clrs; jcol++)
            {
              adobe_copy_pixel(trow + row, tcol + col, &rp);
              if (++col >= tile_width || col >= raw_width)
                row += 1 + (col = 0);
            }
          else
            for (jcol = 0; jcol < jwide; jcol++)
            {
              adobe_copy_pixel(trow + row, tcol + col, &rp);
              if (++col >= tile_width || col >= raw_width)
                row += 1 + (col = 0);
            }
        }
      }
    }
    catch (...)
    {
      ljpeg_end(&jh);
      shot_select = ss;
      throw;
    }
    fseek(ifp, save + 4, SEEK_SET);
    if ((tcol += tile_width) >= raw_width)
      trow += tile_length + (tcol = 0);
    ljpeg_end(&jh);
  }
  shot_select = ss;
}

void LibRaw::packed_dng_load_raw()
{
  ushort *pixel, *rp;
  unsigned row, col;

  if (tile_length < INT_MAX)
  {
      packed_tiled_dng_load_raw();
      return;
  }

  int ss = shot_select;
  shot_select = libraw_internal_data.unpacker_data.dng_frames[LIM(ss,0,(LIBRAW_IFD_MAXCOUNT*2-1))] & 0xff;

  pixel = (ushort *)calloc(raw_width, tiff_samples * sizeof *pixel);
  try
  {
    for (row = 0; row < raw_height; row++)
    {
      checkCancel();
      if (tiff_bps == 16)
        read_shorts(pixel, raw_width * tiff_samples);
      else
      {
        getbits(-1);
        for (col = 0; col < raw_width * tiff_samples; col++)
          pixel[col] = getbits(tiff_bps);
      }
      for (rp = pixel, col = 0; col < raw_width; col++)
        adobe_copy_pixel(row, col, &rp);
    }
  }
  catch (...)
  {
    free(pixel);
    shot_select = ss;
    throw;
  }
  free(pixel);
  shot_select = ss;
}
#ifdef NO_JPEG
void LibRaw::lossy_dng_load_raw() {}
#else

static void jpegErrorExit_d(j_common_ptr /*cinfo*/)
{
  throw LIBRAW_EXCEPTION_DECODE_JPEG;
}

void LibRaw::lossy_dng_load_raw()
{
  if (!image)
    throw LIBRAW_EXCEPTION_IO_CORRUPT;
  struct jpeg_decompress_struct cinfo;

  unsigned sorder = order, ntags, opcode, deg, i, j, c;
  unsigned trow = 0, tcol = 0, row, col;
  INT64 save = data_offset - 4;
  ushort cur[4][256];
  double coeff[9], tot;

  if (meta_offset)
  {
    fseek(ifp, meta_offset, SEEK_SET);
    order = 0x4d4d;
    ntags = get4();
    while (ntags--)
    {
      opcode = get4();
      get4();
      get4();
      if (opcode != 8)
      {
        fseek(ifp, get4(), SEEK_CUR);
        continue;
      }
      fseek(ifp, 20, SEEK_CUR);
      if ((c = get4()) > 3)
        break;
      fseek(ifp, 12, SEEK_CUR);
      if ((deg = get4()) > 8)
        break;
      for (i = 0; i <= deg && i < 9; i++)
        coeff[i] = getreal(LIBRAW_EXIFTAG_TYPE_DOUBLE);
      for (i = 0; i < 256; i++)
      {
        for (tot = j = 0; j <= deg; j++)
          tot += coeff[j] * pow(i / 255.0, (int)j);
        cur[c][i] = (ushort)(tot * 0xffff);
      }
    }
    order = sorder;
  }
  else
  {
    gamma_curve(1 / 2.4, 12.92, 1, 255);
    FORC4 memcpy(cur[c], curve, sizeof cur[0]);
  }

  struct jpeg_error_mgr pub;
  cinfo.err = jpeg_std_error(&pub);
  pub.error_exit = jpegErrorExit_d;

  std::vector<JSAMPLE> buf;

  jpeg_create_decompress(&cinfo);

  while (trow < raw_height)
  {
    fseek(ifp, save += 4, SEEK_SET);
    if (tile_length < INT_MAX)
      fseek(ifp, get4(), SEEK_SET);
    if (libraw_internal_data.internal_data.input->jpeg_src(&cinfo) == -1)
    {
      jpeg_destroy_decompress(&cinfo);
      throw LIBRAW_EXCEPTION_DECODE_JPEG;
    }
    jpeg_read_header(&cinfo, TRUE);
    jpeg_start_decompress(&cinfo);
	if (cinfo.output_components != colors)
		throw LIBRAW_EXCEPTION_DECODE_JPEG;

	if (buf.size() < cinfo.output_width * cinfo.output_components)
		buf = std::vector<JSAMPLE>(cinfo.output_width * cinfo.output_components,0);

    try
    {
      JSAMPLE *buffer_array[1];
      buffer_array[0] = buf.data();
      while (cinfo.output_scanline < cinfo.output_height &&
             (row = trow + cinfo.output_scanline) < height)
      {
        checkCancel();
        jpeg_read_scanlines(&cinfo, buffer_array, 1);
        for (col = 0; col < cinfo.output_width && tcol + col < width; col++)
        {
          FORC(colors) image[row * width + tcol + col][c] = cur[c][buf[col*colors+c]];
        }
      }
    }
    catch (...)
    {
      jpeg_destroy_decompress(&cinfo);
      throw;
    }
    jpeg_abort_decompress(&cinfo);
    if ((tcol += tile_width) >= raw_width)
      trow += tile_length + (tcol = 0);
  }
  jpeg_destroy_decompress(&cinfo);
  maximum = 0xffff;
}
#endif
