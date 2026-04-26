/*
 * Copyright 2026 Markus Haikonen, Ionhaken
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

namespace ion
{
namespace compact
{
// 3 bits compressor + 5 bits options, packed into one byte by CompactWriter.
// CompactWriter asserts mCompressor < 8 so adding a 9th value fails loudly rather
// than silently aliasing to Bitpack on the wire. Append-only: existing values are
// part of the on-disk format.
enum class Compressor : uint8_t
{
	Bitpack,  // 0
	Fse,	  // 1
	Huf,	  // 2
	Rle,	  // 3 - Run-length: symbol + length of pattern.
	RleFse,	  // 4
	Zstd,	  // 5 - Produced only via direct CompressZstd; not selected by the high-level Compress().
};

struct CompressedInfo
{
	uint32_t mCompressedSize = 0;
	Compressor mCompressor = Compressor::Bitpack;
	uint8_t mOptions = 0;
	uint8_t mNumSizeBytes = 0;
};
}  // namespace compact
}  // namespace ion
