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

#include <ion/CoreCommon.h>


#include <ion/compact/CompressedInfo.h>

namespace ion
{

namespace compact
{
struct Decompression;

Decompression* DecompressInit();
void DecompressDeinit(Decompression*);
size_t Decompress(Decompression& decompression, const CompressedInfo& info, ion::ByteReader& reader, ion::ByteWriter& writer);
size_t DecompressZstd(Decompression& decompression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src);
size_t DecompressFse(Decompression& decompression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src);
// useExistingTable=true means the input has no table header and must be decoded against
// the DTable populated by a previous Huf decompress on the same Decompression.
size_t DecompressHuf(Decompression& decompression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src,
					 bool useExistingTable = false);

}  // namespace compact

}  // namespace ion
