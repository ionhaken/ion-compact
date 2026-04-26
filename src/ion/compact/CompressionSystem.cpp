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
#include <ion/CoreCommon.h>

#include <ion/compact/CompressionSystem.h>
#include <ion/compact/Compression.h>
#include <ion/compact/CompressionUtil.h>

// ZSTD
#include "zstd/lib/compress/zstd_compress_internal.h"
#ifdef XXH_FALLTHROUGH
	#undef XXH_FALLTHROUGH
#endif
#include "zstd/lib/zstd.h"
#define FSE_STATIC_LINKING_ONLY
#include "zstd/lib/common/huf.h"
#include "zstd/lib/compress/hist.h"
// /ZSTD

// Per-call codec-selection tracing. Off by default; flip to ION_LOG_INFO when
// debugging compressor choices. Defined locally so it doesn't leak from a public header.
#ifndef ION_LOG_COMPRESSION
	#define ION_LOG_COMPRESSION(x) ((void)0)
#endif

namespace ion
{

namespace compact
{
namespace
{

unsigned hist(unsigned* count, unsigned* maxSymbolValuePtr, unsigned& repeats, unsigned& unpredicted, const void* src, size_t srcSize)
{
	const BYTE* ip = (const BYTE*)src;
	const BYTE* const end = ip + srcSize;
	unsigned maxSymbolValue = *maxSymbolValuePtr;
	unsigned largestCount = 0;

	ZSTD_memset(count, 0, (maxSymbolValue + 1) * sizeof(*count));
	if (srcSize == 0)
	{
		*maxSymbolValuePtr = 0;
		return 0;
	}

	// Seed with the first byte: it is the initial occurrence, neither a repeat nor an
	// unpredicted transition. (Previously prev was initialised to 0 which mis-counted
	// src[0]==0 as a self-repeat and any other src[0] as an unpredicted transition.)
	assert(*ip <= maxSymbolValue);
	BYTE prev = *ip++;
	count[prev]++;
	unsigned localRepeats = 0;
	while (ip < end)
	{
		assert(*ip <= maxSymbolValue);
		BYTE value = *ip++;
		if (prev == value)
		{
			localRepeats++;
		}
		else
		{
			prev = value;
			if (localRepeats > 0)
			{
				repeats += localRepeats;
				localRepeats = 0;
			}
			else
			{
				unpredicted++;
			}
		}
		count[prev]++;
	}

	if (localRepeats > 0)
	{
		repeats += localRepeats;
	}

	while (!count[maxSymbolValue])
		maxSymbolValue--;
	*maxSymbolValuePtr = maxSymbolValue;

	{
		U32 s;
		for (s = 0; s <= maxSymbolValue; s++)
			if (count[s] > largestCount)
				largestCount = count[s];
	}

	return largestCount;
}

bool ShouldUseRLE(unsigned maxSymbol, unsigned repeats, unsigned unpredicted, size_t size, unsigned& numBitsUsed)
{
	ION_LOG_COMPRESSION("Symbols repeated:" << repeats << ";unpredicted=" << unpredicted
											<< ";avg repeats=" << (unpredicted > 0 ? repeats / unpredicted : repeats) << ";size=" << size
											<< ";MaxSymbol=" << maxSymbol);
	if (unpredicted == 0)
	{
		numBitsUsed = unsigned(8 - CountLeadingZeroes(uint8_t(maxSymbol)));
		return true;
	}
	if (maxSymbol < 128)
	{
		if (repeats < size / 4)
		{
			return false;
		}

		size_t avgRepeatLen = size_t((repeats / unpredicted) + 0.5);
		if (avgRepeatLen < 4)
		{
			return false;
		}

		numBitsUsed = unsigned(8 - CountLeadingZeroes(uint8_t(maxSymbol)));
		if (avgRepeatLen < (size_t(1) << numBitsUsed))
		{
			return true;
		}
		return false;
	}
	if (repeats < size / 2)
	{
		return false;
	}
	size_t avgRepeatLen = size_t((repeats / unpredicted) + 0.5);
	if (avgRepeatLen > 0)
	{
		numBitsUsed = 8;
		return true;
	}

	return false;
}

size_t Bitpack(BYTE* op, const ArrayView<byte, uint32_t>& src, unsigned numBitsUsed)
{
	ByteWriterUnsafe writer((byte*)op);
	uint64_t tmp = 0;
	unsigned index = 0;
	for (size_t i = 0; i < src.Size(); ++i)
	{
		tmp = tmp | (uint64_t(src.Data()[i]) << index);
		index += numBitsUsed;
		if (index >= 32)
		{
			writer.Write(uint32_t(tmp));
			index = index - 32;
			tmp = tmp >> 32;
		}
	}
	if (index > 0)
	{
		size_t i = 0;
		while (i < index)
		{
			writer.Write(uint8_t(tmp >> i));
			i += 8;
		}
	}
	return writer.NumBytesUsed();
}

uint32_t CompressRLE(BYTE* op, const ArrayView<byte, uint32_t>& src, unsigned numBitsUsed, size_t tailSize)
{
	ByteWriterUnsafe writer((byte*)op);

	if (numBitsUsed <= 8)
	{
		unsigned maxCount = numBitsUsed == 8 ? 255 : (1 << (8 - numBitsUsed)) - 1;
		uint8_t prev = src.Data()[0];
		uint8_t count = 0;
		uint8_t prevCount = 0;
		size_t srcSize = src.Size() - tailSize;
		for (size_t i = 1; i < srcSize; ++i)
		{
			if (prev != src.Data()[i] || count == maxCount)
			{
				if (numBitsUsed == 8)
				{
					writer.Write(prev);
					uint8_t diff = count - prevCount;
					writer.Write(diff);
				}
				else
				{
					uint8_t diff = (count - prevCount) % (1 << (8 - numBitsUsed));
					writer.Write(uint8_t(prev | (diff << numBitsUsed)));
				}
				prevCount = count;
				count = 0;
				prev = src.Data()[i];
			}
			else
			{
				count++;
			}
		}

		writer.Write(prev);
		return ion::SafeRangeCast<uint32_t>(writer.NumBytesUsed());
	}

	ION_ASSERT(src.Size() >= tailSize, "Invalid tail");
	size_t srcSize = src.Size() - tailSize;

	uint8_t prevSymbol = src.Data()[0];
	uint8_t count = 0;
	const uint8_t maxCount = (1 << 7) - 1;
	size_t i = 1;
	while (i < srcSize)
	{
		if (writer.NumBytesUsed() >= srcSize)
		{
			return 0;
		}
		if (prevSymbol != src.Data()[i] || count >= maxCount)
		{
			if (prevSymbol < 16)
			{
				// 1+5 symbols
				bool canFit = true;
				for (int j = 0; j < 5; ++j)
				{
					if (i + j >= srcSize || src.Data()[i + j] >= 16)
					{
						canFit = false;
						break;
					}
				}
				if (canFit)
				{
					writer.Write<uint8_t>(prevSymbol << 4 | src.Data()[i]);
					writer.Write<uint8_t>(count << 1);
					writer.Write<uint8_t>((src.Data()[i + 1] << 4) | src.Data()[i + 2]);
					writer.Write<uint8_t>((src.Data()[i + 3] << 4) | src.Data()[i + 4]);
					count = 0;
					++i;
					continue;
				}
			}
			writer.Write(prevSymbol);
			writer.Write<uint8_t>((count << 1u) | 1u);
			prevSymbol = src.Data()[i];
			for (int j = 0; j < 2; j++)
			{
				writer.Write(prevSymbol);
				++i;
				if (i < srcSize)
				{
					prevSymbol = src.Data()[i];
				}
			}
			count = 0;
		}
		else
		{
			++count;
		}
		++i;
	}

	writer.Write(prevSymbol);
	return ion::SafeRangeCast<uint32_t>(writer.NumBytesUsed());
}

bool CompressInternal(Compression& compression, CompressedInfo& info, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src,
					  bool isInner)
{
	unsigned maxSymbol = MaxSymbol;
	unsigned repeats = 0;
	unsigned unpredicted = 0;
	unsigned maxCount = hist(compression.count, &maxSymbol, repeats, unpredicted, (const void*)src.Data(), src.Size()); /* never fails */

	for (;;)
	{
		BYTE* op = dst.Data();
		size_t srcSize = src.Size();

		if (maxCount == 1 && maxSymbol > 0)
		{
			ION_LOG_COMPRESSION("Each symbol present maximum once => not compressible;MaxSymbol:" << maxSymbol << ";Size=" << src.Size());
		}
		else
		{
			unsigned bitsPerSymbol = 0;

			ION_ASSERT(dst.Size() >= info.mNumSizeBytes + 1u, "Out of dst space");
			memcpy(op, &srcSize, info.mNumSizeBytes);
			op += info.mNumSizeBytes;
			if (maxCount == src.Size())	 // only a single symbol in src
			{
				info.mCompressor = Compressor::Rle;
				info.mCompressedSize = info.mNumSizeBytes;
				if (maxSymbol > 0)
				{
					uint8_t symbol = uint8_t(maxSymbol);
					memcpy(op, &symbol, 1);
					op++;
					info.mCompressedSize++;
				}
				info.mOptions = 7;
				break;
			}
			else if (!isInner && (ShouldUseRLE(maxSymbol, repeats, unpredicted, src.Size(), bitsPerSymbol)))
			{
				ION_ASSERT(maxSymbol > 0, "Must be more symbols");
				info.mCompressor = Compressor::Rle;

				size_t tailSize = 0;
				{
					const byte* end = src.Data();
					const byte* tailOp = end + src.Size() - 1;
					byte prev = *tailOp;
					tailOp--;
					while (prev == *tailOp)
					{
						tailSize++;
						if (tailOp == end)
						{
							break;
						}
						tailOp--;
					}
				}

				// RLE / RLE+FSE wrap. The control flow here is subtle:
				//   - For non-trivial inputs (srcSize - tailSize > tempLimit) we encode RLE into
				//     `temp` so that the original `dst` is still available for an inner FSE pass.
				//   - We then call CompressInternal recursively with isInner=true on the RLE
				//     output, which writes its result directly to `dst`.
				//   - If the inner pass beats the size budget we keep dst (Compressor::RleFse).
				//   - Otherwise we copy the plain RLE bytes from `temp` over `dst` (Compressor::Rle).
				//   - For trivial inputs (srcSize - tailSize <= tempLimit) we encode RLE directly
				//     into `dst` (rleOp = op) and skip the inner FSE attempt; `temp` stays empty.
				Vector<byte> temp;
				size_t tempLimit = 8;
				BYTE* rleOp = op;
				if (srcSize - tailSize > tempLimit)
				{
					temp.ResizeFast(srcSize * 2);
					rleOp = temp.Data();
					memcpy(rleOp, &srcSize, info.mNumSizeBytes);
					rleOp += info.mNumSizeBytes;
				}

				size_t maxCompressedSize = (src.Size() * bitsPerSymbol + 7) / 8 + info.mNumSizeBytes;

				uint32_t numBytesUsed = CompressRLE(rleOp, src, bitsPerSymbol, tailSize);
				if (numBytesUsed > 0)
				{
					info.mCompressedSize = numBytesUsed + info.mNumSizeBytes;
					info.mOptions = uint8_t(bitsPerSymbol - 1);
					ION_LOG_COMPRESSION("RLE compressed size:" << info.mCompressedSize << ";maxSymbols=" << maxSymbol
															   << ";BitsUsed=" << bitsPerSymbol);
					if (!temp.IsEmpty())
					{
						ArrayView<byte, uint32_t> rleSrc(temp.Data(), info.mCompressedSize);
						CompressedInfo innerInfo;
						innerInfo.mNumSizeBytes = info.mNumSizeBytes;
						innerInfo.mCompressedSize = info.mCompressedSize;
						// Only Fse is usable as the inner compressor for the RLE wrap: RleHuf cannot
						// round-trip because the intermediate RLE-encoded size is not stored in the
						// frame, and the Huf decoder requires the exact decompressed size.
						if (CompressInternal(compression, innerInfo, dst, rleSrc, true) &&
							innerInfo.mCompressor == Compressor::Fse &&
							innerInfo.mCompressedSize <= maxCompressedSize)
						{
							info = innerInfo;
							ION_LOG_COMPRESSION("RLE+FSE compressed size:" << innerInfo.mCompressedSize);
							info.mCompressor = Compressor::RleFse;
							info.mOptions = uint8_t(bitsPerSymbol - 1);
							break;
						}
						else if (info.mCompressedSize <= maxCompressedSize)
						{
							ION_LOG_COMPRESSION("RLE compressed size:" << info.mCompressedSize);
							memcpy(dst.Data(), temp.Data(), info.mCompressedSize);
							break;
						}
					}
				}
			}
			if (maxCount >= (src.Size() >> 7))	// Heuristic : not compressible enough
			{
				size_t maxCompressedSize = src.Size();
				bitsPerSymbol = 8;
				if (maxSymbol < 128 && !isInner)
				{
					bitsPerSymbol = 8 - CountLeadingZeroes<uint8_t>(uint8_t(maxSymbol));
					maxCompressedSize = (src.Size() * bitsPerSymbol + 7) / 8;
				}
				ION_ASSERT(dst.Size() - info.mNumSizeBytes >= maxCompressedSize, "Not enough destination space");

				ArrayView<byte, uint32_t> compressDst(op, SafeRangeCast<uint32_t>(maxCompressedSize + 128));
				// Codec choice. HUF and FSE compress to ~equal ratios, but HUF encodes ~2× faster.
				// FSE wins on table overhead when:
				//   - src is tiny (header amortization fails),
				//   - alphabet is tiny (HUF weights heavier than FSE normalized counts),
				//   - distribution is near-uniform (FSE's fractional bits outpace HUF's integer bits).
				// "skew" = maxCount * (maxSymbol+1) / srcSize: 1 = uniform, larger = more skewed.
				const uint64_t skewMetric = uint64_t(maxCount) * (maxSymbol + 1);
				const bool preferHuf = !isInner && (maxSymbol < 128) && (src.Size() >= 256) &&
									   (src.Size() <= HUF_BLOCKSIZE_MAX) && (maxSymbol >= 8) &&
									   (skewMetric >= uint64_t(src.Size()) * 8);
				if (preferHuf)
				{
					info.mCompressor = Compressor::Huf;
					bool reused = false;
					info.mCompressedSize = SafeRangeCast<uint32_t>(CompressHuf(compression, compressDst, src, maxSymbol, &reused));
					// mOptions bit 0 distinguishes Huf-with-table from Huf-with-reused-table.
					info.mOptions = reused ? 1u : 0u;
				}
				else
				{
					info.mCompressor = Compressor::Fse;
					info.mOptions = 0;	// Fse doesn't use the options bits; clear so stale RLE-path
										// settings from earlier in the loop don't leak into the wire format.
					info.mCompressedSize = SafeRangeCast<uint32_t>(CompressFse(compression, compressDst, src, maxSymbol));
				}

				if (info.mCompressedSize && info.mCompressedSize <= maxCompressedSize)
				{
					info.mCompressedSize += info.mNumSizeBytes;
					break;
				}
				if (bitsPerSymbol < 8)
				{
					memcpy(dst.Data(), &srcSize, info.mNumSizeBytes);
					info.mCompressedSize =
					  SafeRangeCast<uint32_t>(Bitpack(dst.Data() + info.mNumSizeBytes, src, bitsPerSymbol) + info.mNumSizeBytes);
					info.mCompressor = Compressor::Bitpack;
					info.mOptions = uint8_t(bitsPerSymbol - 1);
					break;
				}
				else
				{
					ION_LOG_COMPRESSION("" << (info.mCompressor == Compressor::Huf ? "Huf" : "FSE")
										   << " could not compress;MaxSymbol=" << maxSymbol << ";SourceSize=" << src.Size()
										   << ";CompressedSize=" << info.mCompressedSize << ";ExpectedSize=" << maxCompressedSize);
				}
			}
			else
			{
				ION_ASSERT(maxSymbol >= 128, "Not compressible enough - use bitpack;MaxSymbol=" << maxSymbol << ";maxCount=" << maxCount);
			}
		}
		return false;
	}
	ION_LOG_COMPRESSION("Compressed Block from " << src.Size() << " to " << info.mCompressedSize << " bytes using " << info.mCompressor
												 << "(" << info.mOptions << ");MaxSymbol=" << maxSymbol << ";maxCount=" << maxCount);
	return true;
}
}  // namespace

void* zalloc(void*, size_t s) { return ion::Malloc(s); }

void zfree(void*, void* address) { ion::Free(address); }

Compression* CompressionInit()
{
	ION_MEMORY_SCOPE(ion::tag::External);
	return new Compression();
}

void CompressionDeinit(Compression* compression)
{
	ION_MEMORY_SCOPE(ion::tag::External);
	if (compression->mCctx)
	{
		ZSTD_freeCCtx(compression->mCctx);
		compression->mCctx = nullptr;
	}
	delete compression;
}

void ResetHufRepeat(Compression& compression) { compression.mHufRepeat = HUF_repeat_none; }

size_t DestCapacityZstd(size_t srcSize) { return ZSTD_compressBound(srcSize); }

size_t DestCapacityHuf(size_t srcSize) { return HUF_compressBound(srcSize); }

size_t DestCapacityFse(size_t srcSize) { return FSE_compressBound(srcSize); }

CompressedInfo Compress(Compression& compression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src)
{
	ION_ASSERT(src.Size() <= 0xFFFFFF, "Unsupported source size");
	ION_ASSERT(src.Size() != 0, "No data");

	CompressedInfo info;
	info.mNumSizeBytes = src.Size() <= 0xFF ? 1 : (src.Size() <= 0xFFFF ? 2 : 3);
	if (!CompressInternal(compression, info, dst, src, false))
	{
		memcpy(dst.Data(), src.Data(), src.Size());
		info.mCompressor = Compressor::Bitpack;
		info.mCompressedSize = src.Size();
		info.mOptions = 0x7;
	}
	return info;
}

size_t CompressFse(Compression& compression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src, unsigned maxSymbol)
{
	BYTE* op = dst.Data();
	size_t dstSize = dst.Size();

	// CompressInternal pre-populates compression.count via the local hist(), but standalone
	// callers don't — build the histogram here so this function is self-contained.
	{
		ION_ASSERT(maxSymbol <= MaxSymbol, "maxSymbol out of range");
		ZSTD_memset(compression.count, 0, (MaxSymbol + 1) * sizeof(*compression.count));
		for (size_t i = 0; i < src.Size(); ++i)
		{
			compression.count[src.Data()[i]]++;
		}
		while (maxSymbol > 0 && !compression.count[maxSymbol])
		{
			maxSymbol--;
		}
	}

	const unsigned int tableLog = FSE_optimalTableLog(TableLogMax, src.Size(), maxSymbol);

	size_t s = FSE_normalizeCount(compression.norm, tableLog, compression.count, src.Size(), maxSymbol, int(src.Size() > 2096));
	ION_ASSERT(!FSE_isError(s), "Compression failed" << FSE_getErrorName(s));
	if (s != tableLog)
	{
		ION_LOG_INFO("FSE_normalizeCount returned: " << s);
		// RLE;
		return 0;
	}

	s = FSE_writeNCount(op, dstSize, compression.norm, maxSymbol, tableLog);
	if (FSE_isError(s))
	{
		ION_LOG_COMPRESSION("Compression failed:" << FSE_getErrorName(s));
		return 0;
	}

	op += s;
	dstSize -= s;

	s = FSE_buildCTable_wksp(compression.mFseCTable, compression.norm, maxSymbol, tableLog, compression.scratchBuffer,
							 sizeof(compression.scratchBuffer));
	if (FSE_isError(s))
	{
		ION_LOG_COMPRESSION("Compression failed:" << FSE_getErrorName(s));
		return 0;
	}

	s = FSE_compress_usingCTable(op, dstSize, src.Data(), src.Size(), compression.mFseCTable);
	ION_ASSERT(!FSE_isError(s), "Compression failed" << FSE_getErrorName(s));
	if (s == 0)
	{
		return 0;  // not enough space for compressed data
	}

	op += s;

	size_t compressedSize = size_t(op - dst.Data());
	return compressedSize;
}

size_t CompressZstd(Compression& compression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src)
{
	if (compression.mCctx == nullptr)
	{
		ZSTD_customMem customMem;
		customMem.customAlloc = &zalloc;
		customMem.customFree = &zfree;
		customMem.opaque = nullptr;
		compression.mCctx = ZSTD_createCCtx_advanced(customMem);
		size_t ret = ZSTD_CCtx_setParameter(compression.mCctx, ZSTD_c_format, ZSTD_f_zstd1_magicless);
		ION_ASSERT(!ZSTD_isError(ret), "Failed");
	}

	size_t size = ZSTD_compressCCtx(compression.mCctx, dst.Data(), dst.Size(), src.Data(), src.Size(), 3);
	if (ZSTD_isError(size))
	{
		return 0;
	}
	return size;
}

size_t CompressHuf(Compression& compression, ArrayView<byte, uint32_t>& dst, const ArrayView<byte, uint32_t>& src, unsigned maxSymbol,
				   bool* outRepeated)
{
	// HUF_compress1X_repeat reuses compression.mHufCTable when *repeat != HUF_repeat_none and
	// the previous table is competitive — in that case the output omits the table header.
	// After the call, *repeat == HUF_repeat_none means a fresh table was written; any other
	// value means the previous table was reused.
	HUF_repeat repeatBefore = compression.mHufRepeat;
	HUF_repeat repeat = repeatBefore;
	size_t res = HUF_compress1X_repeat(dst.Data(), dst.Size(), src.Data(), src.Size(), maxSymbol, TableLogMax, compression.scratchBuffer,
									   sizeof(compression.scratchBuffer), compression.mHufCTable, &repeat, 0);
	// HUF returns errors as size_t values just below SIZE_MAX (e.g. inputs > HUF_BLOCKSIZE_MAX
	// hit srcSize_wrong). Catch them explicitly so we don't end up returning a garbage size to
	// the caller (which then falls through to bitpack fallback).
	if (HUF_isError(res) || res <= 2)
	{
		compression.mHufRepeat = HUF_repeat_none;
		if (outRepeated) *outRepeated = false;
		return 0;
	}

	const bool reused = (repeatBefore != HUF_repeat_none) && (repeat != HUF_repeat_none);
	if (outRepeated) *outRepeated = reused;
	// Table is now valid (whether reused or freshly built); allow next call to attempt reuse.
	compression.mHufRepeat = HUF_repeat_check;
	return res;
}

}  // namespace compact
}  // namespace ion
