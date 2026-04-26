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

#include <ion/compact/CompactReader.h>
#include <ion/compact/CompactWriter.h>
#include <ion/CoreCommon.h>

namespace ion
{



namespace compact
{
static constexpr serialization::Tag TagNoCompression = {0};
static constexpr serialization::Tag TagSignificantByte = {1};
static constexpr serialization::Tag TagByteCount = {2};

// ---------------------------------------------------------------------------
// Caller-provided context modeling for variable-byte ints and floats.
//
// The library treats bytes as i.i.d. by design (see README). The caller can
// recover conditional information by routing values to per-bucket substreams
// chosen from the previous value via a templated context function.
//
// Each Context type must provide:
//   static constexpr size_t NumContexts;        // bucket count
//   static constexpr size_t Compute(uint64_t);  // bucket id 0..NumContexts-1
//
// Compose with CompactWriter/CompactReader by sizing them with
// ContextState<MyCtx>::NumStreams. The same Context type must be used on both
// sides; the wire format is determined by NumContexts and the bucket function.
//
// Stock contexts in `context::` mirror Brotli's literal context modes (LSB6,
// MSB6, Signed) plus a handful of magnitude-bucketed strategies useful for
// game-state varbyte streams. Add your own as needed.
// ---------------------------------------------------------------------------

namespace context
{
struct None
{
	static constexpr size_t NumContexts = 1;
	static constexpr size_t Compute(uint64_t /*prev*/) { return 0; }
};

// Two buckets: previous was zero vs nonzero. Useful for sparse value streams.
struct ZeroVsNonzero
{
	static constexpr size_t NumContexts = 2;
	static constexpr size_t Compute(uint64_t prev) { return prev != 0u ? 1u : 0u; }
};

// Four buckets by previous magnitude: 0 / 1..15 / 16..255 / >=256.
struct PrevMagnitude4
{
	static constexpr size_t NumContexts = 4;
	static constexpr size_t Compute(uint64_t prev)
	{
		if (prev == 0u) return 0u;
		if (prev <= 0xFu) return 1u;
		if (prev <= 0xFFu) return 2u;
		return 3u;
	}
};

// Eight buckets by ceil(log2(prev+1)) / 8 -> covers 0..63-bit ranges.
struct PrevMagnitude8
{
	static constexpr size_t NumContexts = 8;
	static constexpr size_t Compute(uint64_t prev)
	{
		if (prev == 0u) return 0u;
		const unsigned msb = 64u - CountLeadingZeroes(prev);
		return ion::Min((msb - 1u) / 8u, 7u);
	}
};

// Brotli LSB6: low 6 bits of the previous byte. 64 buckets.
struct LSB6
{
	static constexpr size_t NumContexts = 64;
	static constexpr size_t Compute(uint64_t prev) { return size_t(prev) & 0x3Fu; }
};

// Brotli MSB6: top 6 bits of the previous byte. 64 buckets.
struct MSB6
{
	static constexpr size_t NumContexts = 64;
	static constexpr size_t Compute(uint64_t prev) { return (size_t(prev) & 0xFFu) >> 2; }
};

// Sign of previous value interpreted as signed int64. Useful for delta streams.
struct Signed
{
	static constexpr size_t NumContexts = 2;
	static constexpr size_t Compute(uint64_t prev) { return int64_t(prev) < 0 ? 1u : 0u; }
};
}  // namespace context

template <typename ContextFn>
struct ContextState
{
	using Context = ContextFn;
	// Per context, reserve two compressed substreams: significant-byte and byte-count.
	// The "no-compression" stream (raw middle bytes) is the entry stream and is shared.
	static constexpr size_t SubstreamsPerContext = 2;
	static constexpr size_t NumStreams = 1 + ContextFn::NumContexts * SubstreamsPerContext;
	uint64_t prev = 0;
};

namespace detail
{
template <typename CtxState>
constexpr serialization::Tag CtxTagSignificant(size_t ctxId)
{
	return serialization::Tag{1u + ctxId * CtxState::SubstreamsPerContext + 0u};
}
template <typename CtxState>
constexpr serialization::Tag CtxTagByteCount(size_t ctxId)
{
	return serialization::Tag{1u + ctxId * CtxState::SubstreamsPerContext + 1u};
}
}  // namespace detail


// Delta-encoding strategies. Pick one explicitly per call (default = ZigZag, the
// historical project-wide choice). Wire format depends on the strategy: encode and
// decode must use the same one.
namespace delta
{
// No delta: pass value through unchanged. Useful when prior values give no signal.
struct None
{
	template <typename T>
	static T Forward(T newValue, T& /*stored*/)
	{
		return newValue;
	}
	template <typename T>
	static T Backward(T delta, T& /*stored*/)
	{
		return delta;
	}
};

// Signed difference with low-bit sign flag (alt to ZigZag). Encodes |delta| << 1, with
// the low bit indicating sign.
struct SignedDelta
{
	template <typename T>
	static T Forward(T newValue, T& stored)
	{
		T delta = newValue - stored;
		if (delta >> (sizeof(T) * 8 - 1))
		{
			delta = ((stored - newValue) << 1) | 1;
		}
		else
		{
			delta = delta << 1;
		}
		stored = newValue;
		return delta;
	}
	template <typename T>
	static T Backward(T delta, T& stored)
	{
		if (delta & 1)
		{
			stored = stored - (delta >> 1);
		}
		else
		{
			stored = stored + (delta >> 1);
		}
		return stored;
	}
};

// XOR delta. Good for floating-point bit patterns and bitfield streams; preserves
// position of changing bits which entropy-codes well.
struct Xor
{
	template <typename T>
	static T Forward(T newValue, T& stored)
	{
		T delta = newValue ^ stored;
		stored = newValue;
		return delta;
	}
	template <typename T>
	static T Backward(T delta, T& stored)
	{
		stored = delta ^ stored;
		return stored;
	}
};

// ZigZag-style signed difference: small absolute deltas pack into low bits regardless
// of sign. Default historical choice; works well for delta-encoded quantized values.
struct ZigZag
{
	template <typename T>
	static T Forward(T newValue, T& stored)
	{
		T delta = newValue - stored;
		if (delta >> (sizeof(T) * 8 - 1))
		{
			delta = ~(delta << 1);
		}
		else
		{
			delta = delta << 1;
		}
		stored = newValue;
		return delta;
	}
	template <typename T>
	static T Backward(T delta, T& stored)
	{
		if (delta & 1)
		{
			delta = ((~delta) >> 1) | (1u << (sizeof(T) * 8 - 1));
		}
		else
		{
			delta = delta >> 1;
		}
		stored = delta + stored;
		return stored;
	}
};
}  // namespace delta

template <typename Strategy = delta::ZigZag, typename T>
T UpdateDeltaForward(T newValue, T& stored)
{
	return Strategy::template Forward<T>(newValue, stored);
}

template <typename Strategy = delta::ZigZag, typename T>
T UpdateDeltaBackward(T delta, T& stored)
{
	return Strategy::template Backward<T>(delta, stored);
}

template <typename TVal, typename Reader>
bool ReadVariableBytes(Reader& reader, TVal& qval)
{
	if constexpr (std::is_signed_v<TVal>)
	{
		using UnsignedType = typename std::make_unsigned<TVal>::type;
		UnsignedType unsignedVal = 0;
		bool result = ReadVariableBytes(reader, unsignedVal);
		qval = TVal(unsignedVal);
		return result;
	}
	else if constexpr (sizeof(TVal) == 1)
	{
		return reader.Read(qval, TagSignificantByte);
	}
	else
	{
		qval = 0;
		uint8_t numBytes = 0;
		if (!reader.Read(numBytes, TagByteCount))
		{
			return false;
		}
		{
			byte sp;
			size_t j = 0;
			for (; j < numBytes; ++j)
			{
				if (!reader.Read(sp, TagNoCompression))
				{
					return false;
				}
				qval |= (TVal(sp) << (j * 8));
			}

			if (!reader.Read(sp, TagSignificantByte))
			{
				return false;
			}
			qval |= (TVal(sp) << (j * 8));
		}
		return true;
	}
}

template <typename TVal, typename Writer>
void WriteVariableBytes(Writer& writer, const TVal qval)
{
	if constexpr (std::is_signed_v<TVal>)
	{
		using UnsignedType = typename std::make_unsigned<TVal>::type;
		UnsignedType unsignedVal = UnsignedType(qval);
		WriteVariableBytes(writer, unsignedVal);
		return;
	}
	else if constexpr (sizeof(TVal) == 1)
	{
		writer.Write(qval, TagSignificantByte);
		return;
	}
	else
	{
		size_t numBytes = (((sizeof(qval) * 8 + 7) - CountLeadingZeroes(qval))) / 8;
		ION_ASSERT(numBytes <= 8, "Invalid value;Leading zeroes:" << CountLeadingZeroes(qval));
		size_t j = 0;
		numBytes = ion::Max(size_t(1), numBytes);
		writer.Write(byte(numBytes - 1), TagByteCount);

		for (; j < numBytes - 1; ++j)
		{
			byte sp = byte(qval >> (j * 8));
			writer.Write(sp, TagNoCompression);
		}
		byte sp = byte(qval >> (j * 8));
		writer.Write(sp, TagSignificantByte);
	}
}

// Context-aware variants: route the byte-count and significant-byte to per-context
// substreams chosen from the previous value. The middle bytes still go uncompressed
// to the entry stream (Tag 0), shared across all contexts.

template <typename Ctx, typename TVal, typename Writer>
void WriteVariableBytes(Writer& writer, const TVal qval, ContextState<Ctx>& state)
{
	using State = ContextState<Ctx>;
	using UnsignedType = std::make_unsigned_t<TVal>;
	const UnsignedType uval = UnsignedType(qval);
	const size_t ctxId = Ctx::Compute(state.prev);
	state.prev = uint64_t(uval);

	if constexpr (sizeof(TVal) == 1)
	{
		writer.Write(uval, detail::CtxTagSignificant<State>(ctxId));
	}
	else
	{
		size_t numBytes = (((sizeof(uval) * 8 + 7) - CountLeadingZeroes(uval))) / 8;
		ION_ASSERT(numBytes <= 8, "Invalid value;Leading zeroes:" << CountLeadingZeroes(uval));
		numBytes = ion::Max(size_t(1), numBytes);
		writer.Write(byte(numBytes - 1), detail::CtxTagByteCount<State>(ctxId));
		size_t j = 0;
		for (; j < numBytes - 1; ++j)
		{
			byte sp = byte(uval >> (j * 8));
			writer.Write(sp, TagNoCompression);
		}
		byte sp = byte(uval >> (j * 8));
		writer.Write(sp, detail::CtxTagSignificant<State>(ctxId));
	}
}

template <typename Ctx, typename TVal, typename Reader>
bool ReadVariableBytes(Reader& reader, TVal& qval, ContextState<Ctx>& state)
{
	using State = ContextState<Ctx>;
	using UnsignedType = std::make_unsigned_t<TVal>;
	const size_t ctxId = Ctx::Compute(state.prev);
	UnsignedType uval = 0;

	if constexpr (sizeof(TVal) == 1)
	{
		if (!reader.Read(uval, detail::CtxTagSignificant<State>(ctxId)))
		{
			return false;
		}
	}
	else
	{
		uint8_t numBytes = 0;
		if (!reader.Read(numBytes, detail::CtxTagByteCount<State>(ctxId)))
		{
			return false;
		}
		byte sp = 0;
		size_t j = 0;
		for (; j < numBytes; ++j)
		{
			if (!reader.Read(sp, TagNoCompression))
			{
				return false;
			}
			uval |= (UnsignedType(sp) << (j * 8));
		}
		if (!reader.Read(sp, detail::CtxTagSignificant<State>(ctxId)))
		{
			return false;
		}
		uval |= (UnsignedType(sp) << (j * 8));
	}

	qval = TVal(uval);
	state.prev = uint64_t(uval);
	return true;
}

template <typename Writer>
void WriteFloat(Writer& writer, float in, uint32_t& delta)
{
	uint32_t fraction = 0;
	memcpy(&fraction, &in, sizeof(float));
	uint32_t sign = fraction >> 31;
	fraction = (fraction << 1) | sign;

	auto out = UpdateDeltaForward(fraction, delta);
	WriteVariableBytes(writer, out);
}

template <typename Reader>
bool ReadFloat(Reader& reader, float& out, uint32_t& delta)
{
	uint32_t fraction = 0;
	bool isOk = ReadVariableBytes(reader, fraction);

	uint32_t res = UpdateDeltaBackward(fraction, delta);
	uint32_t sign = res & 1;
	res = (res >> 1) | (sign << 31);

	memcpy(&out, &res, sizeof(float));
	return isOk;
}

template <typename Ctx, typename Writer>
void WriteFloat(Writer& writer, float in, uint32_t& delta, ContextState<Ctx>& state)
{
	uint32_t fraction = 0;
	memcpy(&fraction, &in, sizeof(float));
	uint32_t sign = fraction >> 31;
	fraction = (fraction << 1) | sign;

	auto out = UpdateDeltaForward(fraction, delta);
	WriteVariableBytes(writer, out, state);
}

template <typename Ctx, typename Reader>
bool ReadFloat(Reader& reader, float& out, uint32_t& delta, ContextState<Ctx>& state)
{
	uint32_t fraction = 0;
	bool isOk = ReadVariableBytes(reader, fraction, state);

	uint32_t res = UpdateDeltaBackward(fraction, delta);
	uint32_t sign = res & 1;
	res = (res >> 1) | (sign << 31);

	memcpy(&out, &res, sizeof(float));
	return isOk;
}

template <typename Writer>
void WriteVariableArray(Writer& writer, const ion::ArrayView<const byte>& data)
{
	WriteVariableBytes(writer, data.Size());
	for (size_t i = 0; i < data.Size(); ++i)
	{
		writer.Write(data.Data()[i], ion::compact::TagSignificantByte);
	}
}

template <typename Reader>
bool ReadVariableArray(Reader& reader, ion::Vector<byte>& data)
{
	uint32_t len = 0;
	if (!ReadVariableBytes(reader, len)) 
	{
		return false;
	}

	data.Resize(len);
	for (size_t i = 0; i < len; ++i)
	{
		if (!reader.Read(data[i], ion::compact::TagSignificantByte))
		{
			return false;
		}
	}
	return true;	
}





}  // namespace compact
}  // namespace ion
