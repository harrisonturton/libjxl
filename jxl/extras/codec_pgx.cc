// Copyright (c) the JPEG XL Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "jxl/extras/codec_pgx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

#include "jxl/base/bits.h"
#include "jxl/base/byte_order.h"
#include "jxl/base/compiler_specific.h"
#include "jxl/base/file_io.h"
#include "jxl/color_management.h"
#include "jxl/external_image.h"
#include "jxl/fields.h"  // AllDefault
#include "jxl/image.h"
#include "jxl/image_bundle.h"
#include "jxl/luminance.h"

namespace jxl {
namespace {

struct HeaderPGX {
  // NOTE: PGX is always grayscale
  size_t xsize;
  size_t ysize;
  size_t bits_per_sample;
  bool big_endian;
  bool is_signed;
};

class Parser {
 public:
  explicit Parser(const Span<const uint8_t> bytes)
      : pos_(bytes.data()), end_(pos_ + bytes.size()) {}

  // Sets "pos" to the first non-header byte/pixel on success.
  Status ParseHeader(HeaderPGX* header, const uint8_t** pos) {
    // codec.cc ensures we have at least two bytes => no range check here.
    if (pos_[0] != 'P' || pos_[1] != 'G') return false;
    pos_ += 2;
    return ParseHeaderPGX(header, pos);
  }

  // Exposed for testing
  Status ParseUnsigned(size_t* number) {
    if (pos_ == end_) return JXL_FAILURE("PGX: reached end before number");
    if (!IsDigit(*pos_)) return JXL_FAILURE("PGX: expected unsigned number");

    *number = 0;
    while (pos_ < end_ && *pos_ >= '0' && *pos_ <= '9') {
      *number *= 10;
      *number += *pos_ - '0';
      ++pos_;
    }

    return true;
  }

 private:
  static bool IsDigit(const uint8_t c) { return '0' <= c && c <= '9'; }
  static bool IsLineBreak(const uint8_t c) { return c == '\r' || c == '\n'; }
  static bool IsWhitespace(const uint8_t c) {
    return IsLineBreak(c) || c == '\t' || c == ' ';
  }

  Status SkipSpace() {
    if (pos_ == end_) return JXL_FAILURE("PGX: reached end before space");
    const uint8_t c = *pos_;
    if (c != ' ') return JXL_FAILURE("PGX: expected space");
    ++pos_;
    return true;
  }

  Status SkipLineBreak() {
    if (pos_ == end_) return JXL_FAILURE("PGX: reached end before line break");
    if (!IsLineBreak(*pos_)) return JXL_FAILURE("PGX: expected line break");
    ++pos_;
    // 0xd 0xa linebreak is possible
    if (pos_ != end_ && IsLineBreak(*pos_)) ++pos_;
    return true;
  }

  Status SkipSingleWhitespace() {
    if (pos_ == end_) return JXL_FAILURE("PGX: reached end before whitespace");
    if (!IsWhitespace(*pos_)) return JXL_FAILURE("PGX: expected whitespace");
    ++pos_;
    return true;
  }

  Status ParseHeaderPGX(HeaderPGX* header, const uint8_t** pos) {
    JXL_RETURN_IF_ERROR(SkipSpace());
    if (pos_ + 2 > end_) return JXL_FAILURE("PGX: header too small");
    if (*pos_ == 'M' && *(pos_ + 1) == 'L') {
      header->big_endian = true;
    } else if (*pos_ == 'L' && *(pos_ + 1) == 'M') {
      header->big_endian = false;
    } else {
      return JXL_FAILURE("PGX: invalid endianness");
    }
    pos_ += 2;
    JXL_RETURN_IF_ERROR(SkipSpace());
    if (pos_ == end_) return JXL_FAILURE("PGX: header too small");
    if (*pos_ == '+') {
      header->is_signed = false;
    } else if (*pos_ == '-') {
      header->is_signed = true;
    } else {
      return JXL_FAILURE("PGX: invalid signedness");
    }
    pos_++;
    // Skip optional space
    if (pos_ < end_ && *pos_ == ' ') pos_++;
    JXL_RETURN_IF_ERROR(ParseUnsigned(&header->bits_per_sample));
    JXL_RETURN_IF_ERROR(SkipSingleWhitespace());
    JXL_RETURN_IF_ERROR(ParseUnsigned(&header->xsize));
    JXL_RETURN_IF_ERROR(SkipSingleWhitespace());
    JXL_RETURN_IF_ERROR(ParseUnsigned(&header->ysize));
    // 0xa, or 0xd 0xa.
    JXL_RETURN_IF_ERROR(SkipLineBreak());

    if (header->bits_per_sample > 16) {
      return JXL_FAILURE("PGX: >16 bits not yet supported");
    }
    // TODO(lode): support signed integers. This may require changing the way
    // external_image works.
    if (header->is_signed) {
      return JXL_FAILURE("PGX: signed not yet supported");
    }

    size_t numpixels = header->xsize * header->ysize;
    size_t bytes_per_pixel = header->bits_per_sample <= 8
                                 ? 1
                                 : header->bits_per_sample <= 16 ? 2 : 4;
    if (pos_ + numpixels * bytes_per_pixel > end_) {
      return JXL_FAILURE("PGX: data too small");
    }

    *pos = pos_;
    return true;
  }

  const uint8_t* pos_;
  const uint8_t* const end_;
};

constexpr size_t kMaxHeaderSize = 200;

Status EncodeHeader(const ExternalImage& external, char* header,
                    int* JXL_RESTRICT chars_written) {
  if (external.HasAlpha()) return JXL_FAILURE("PGX: can't store alpha");
  if (!external.IsGray()) return JXL_FAILURE("PGX: must be grayscale");
  // TODO(lode): verify other bit depths: for other bit depths such as 1 or 4
  // bits, have a test case to verify it works correctly. For bits > 16, we may
  // need to change the way external_image works.
  if (external.BitsPerSample() != 8 && external.BitsPerSample() != 16) {
    return JXL_FAILURE("PGX: bits other than 8 or 16 not yet supported");
  }

  // Use ML (Big Endian), LM may not be well supported by all decoders.
  snprintf(header, kMaxHeaderSize, "PG ML + %zu %zu %zu\n%n",
           external.BitsPerSample(), external.xsize(), external.ysize(),
           chars_written);
  return true;
}

Status ApplyHints(CodecInOut* io) {
  bool got_color_space = false;

  JXL_RETURN_IF_ERROR(io->dec_hints.Foreach(
      [io, &got_color_space](const std::string& key, const std::string& value) {
        ColorEncoding* c_original = &io->metadata.color_encoding;
        if (key == "color_space") {
          if (!ParseDescription(value, c_original) ||
              !c_original->CreateICC()) {
            return JXL_FAILURE("PGX: Failed to apply color_space");
          }

          if (!io->metadata.color_encoding.IsGray()) {
            return JXL_FAILURE("PGX: color_space hint must be grayscale");
          }

          got_color_space = true;
        } else if (key == "icc_pathname") {
          PaddedBytes icc;
          JXL_RETURN_IF_ERROR(ReadFile(value, &icc));
          JXL_RETURN_IF_ERROR(c_original->SetICC(std::move(icc)));
          got_color_space = true;
        } else {
          JXL_WARNING("PGX decoder ignoring %s hint", key.c_str());
        }
        return true;
      }));

  if (!got_color_space) {
    JXL_WARNING("PGX: no color_space/icc_pathname given, assuming sRGB");
    JXL_RETURN_IF_ERROR(io->metadata.color_encoding.SetSRGB(ColorSpace::kGray));
  }

  return true;
}

template <typename T>
void ExpectNear(T a, T b, T precision) {
  JXL_CHECK(std::abs(a - b) <= precision);
}

Span<const uint8_t> MakeSpan(const char* str) {
  return Span<const uint8_t>(reinterpret_cast<const uint8_t*>(str),
                             strlen(str));
}

}  // namespace

Status DecodeImagePGX(const Span<const uint8_t> bytes, ThreadPool* pool,
                      CodecInOut* io) {
  io->enc_size = bytes.size();

  Parser parser(bytes);
  HeaderPGX header = {};
  const uint8_t* pos;
  if (!parser.ParseHeader(&header, &pos)) return false;
  JXL_RETURN_IF_ERROR(io->VerifyDimensions(header.xsize, header.ysize));
  if (header.bits_per_sample == 0 || header.bits_per_sample > 32) {
    return JXL_FAILURE("PGX: bits_per_sample invalid");
  }

  JXL_RETURN_IF_ERROR(ApplyHints(io));
  io->metadata.bits_per_sample = header.bits_per_sample;
  io->metadata.floating_point_sample = false;
  io->metadata.alpha_bits = 0;
  io->dec_pixels = header.xsize * header.ysize;
  io->frames.clear();
  io->frames.reserve(1);
  ImageBundle ib(&io->metadata);

  const bool has_alpha = false;
  const bool flipped_y = false;
  const PackedImage desc(
      header.xsize, header.ysize, io->metadata.color_encoding, has_alpha,
      /*alpha_is_premultiplied=*/false, io->metadata.alpha_bits,
      io->metadata.bits_per_sample, header.big_endian, flipped_y);
  const Span<const uint8_t> span(pos, bytes.data() + bytes.size() - pos);
  if (!CopyTo(desc, span, pool, &ib)) return false;
  io->frames.push_back(std::move(ib));
  return Map255ToTargetNits(io, pool);
}

Status EncodeImagePGX(const CodecInOut* io, const ColorEncoding& c_desired,
                      size_t bits_per_sample, ThreadPool* pool,
                      PaddedBytes* bytes) {
  io->enc_bits_per_sample = bits_per_sample;
  const bool big_endian = true;

  if (!Bundle::AllDefault(io->metadata)) {
    JXL_WARNING("PGX encoder ignoring metadata - use a different codec");
  }
  if (!c_desired.IsSRGB()) {
    JXL_WARNING(
        "PGX encoder cannot store custom ICC profile; decoder\n"
        "will need hint key=color_space to get the same values");
  }

  ImageBundle ib = io->Main().Copy();
  JXL_RETURN_IF_ERROR(MapTargetNitsTo255(&ib, pool));
  const ImageU* alpha = ib.HasAlpha() ? &ib.alpha() : nullptr;
  const size_t alpha_bits = ib.HasAlpha() ? io->metadata.alpha_bits : 0;
  CodecIntervals* temp_intervals = nullptr;  // Can't store min/max.

  const Image3F* to_external_image = &ib.color();
  ExternalImage external(pool, *to_external_image, Rect(ib), ib.c_current(),
                         c_desired, ib.HasAlpha(), ib.AlphaIsPremultiplied(),
                         alpha, alpha_bits, io->enc_bits_per_sample, big_endian,
                         temp_intervals);
  JXL_RETURN_IF_ERROR(external.IsHealthy());

  char header[kMaxHeaderSize];
  int header_size = 0;
  JXL_RETURN_IF_ERROR(EncodeHeader(external, header, &header_size));

  const PaddedBytes& pixels = external.Bytes();
  io->enc_size = static_cast<size_t>(header_size) + pixels.size();
  bytes->resize(io->enc_size);
  memcpy(bytes->data(), header, static_cast<size_t>(header_size));
  memcpy(bytes->data() + header_size, pixels.data(), pixels.size());

  return true;
}

void TestCodecPGX() {
  {
    std::string pgx = "PG ML + 8 2 3\npixels";

    CodecInOut io;
    ThreadPool* pool = nullptr;

    Status ok = DecodeImagePGX(MakeSpan(pgx.c_str()), pool, &io);
    JXL_CHECK(ok == true);

    JXL_CHECK(io.metadata.bits_per_sample == 8);
    JXL_CHECK(io.metadata.floating_point_sample == false);
    JXL_CHECK(io.metadata.color_encoding.IsGray());
    JXL_CHECK(io.xsize() == 2);
    JXL_CHECK(io.ysize() == 3);
    JXL_CHECK(io.Main().color().Plane(0).Row(0)[0] == 'p');
    JXL_CHECK(io.Main().color().Plane(0).Row(0)[1] == 'i');
    JXL_CHECK(io.Main().color().Plane(0).Row(1)[0] == 'x');
    JXL_CHECK(io.Main().color().Plane(0).Row(1)[1] == 'e');
    JXL_CHECK(io.Main().color().Plane(0).Row(2)[0] == 'l');
    JXL_CHECK(io.Main().color().Plane(0).Row(2)[1] == 's');
  }

  {
    std::string pgx = "PG ML + 16 2 3\np_i_x_e_l_s_";

    CodecInOut io;
    ThreadPool* pool = nullptr;

    Status ok = DecodeImagePGX(MakeSpan(pgx.c_str()), pool, &io);
    JXL_CHECK(ok == true);

    JXL_CHECK(io.metadata.bits_per_sample == 16);
    JXL_CHECK(io.metadata.floating_point_sample == false);
    JXL_CHECK(io.metadata.color_encoding.IsGray());
    JXL_CHECK(io.xsize() == 2);
    JXL_CHECK(io.ysize() == 3);
    float eps = 1e-7;
    const auto& plane = io.Main().color().Plane(0);
    ExpectNear(256.0f * 'p' + '_', plane.Row(0)[0] * 257, eps);
    ExpectNear(256.0f * 'i' + '_', plane.Row(0)[1] * 257, eps);
    ExpectNear(256.0f * 'x' + '_', plane.Row(1)[0] * 257, eps);
    ExpectNear(256.0f * 'e' + '_', plane.Row(1)[1] * 257, eps);
    ExpectNear(256.0f * 'l' + '_', plane.Row(2)[0] * 257, eps);
    ExpectNear(256.0f * 's' + '_', plane.Row(2)[1] * 257, eps);
  }
}

}  // namespace jxl
