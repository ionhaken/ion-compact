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

#include <ion/compact/CompressedInfo.h>

namespace ion
{

namespace compact
{
struct Compression;

Compression* CompressionInit();

void CompressionDeinit(Compression* compression);

size_t DestCapacityZstd(size_t srcSize);
size_t DestCapacityFse(size_t srcSize);
size_t DestCapacityHuf(size_t srcSize);

// Invalidates the cached HUF table so the next CompressHuf call rebuilds it from scratch
// instead of attempting reuse. Useful for cold-path benchmarking and for callers that
// know the upcoming distribution will diverge from prior calls.
void ResetHufRepeat(Compression& compression);

size_t CompressZstd(Compression& compression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src);
size_t CompressFse(Compression& compression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src, unsigned maxSymbol);
// outRepeated, if non-null, is set to true when the previous HUF table was reused (no
// table header in output). Reuse only fires when CompressHuf has been called previously
// on the same Compression and the new distribution is similar enough to the cached table.
size_t CompressHuf(Compression& compression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src, unsigned maxSymbol,
				   bool* outRepeated = nullptr);

CompressedInfo Compress(Compression& compression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src);

};	// namespace compact

}  // namespace ion
