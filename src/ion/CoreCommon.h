#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <random>
#include <sstream>
#include <type_traits>
#include <utility>
#include <vector>

// ----------------------------------------------------------------------------
// Macro shims
// ----------------------------------------------------------------------------
#ifndef ION_FORCE_INLINE
	#if defined(_MSC_VER)
		#define ION_FORCE_INLINE __forceinline
	#elif defined(__GNUC__) || defined(__clang__)
		#define ION_FORCE_INLINE inline __attribute__((always_inline))
	#else
		#define ION_FORCE_INLINE inline
	#endif
#endif

#ifndef ION_ALIGN
	#define ION_ALIGN(N) alignas(N)
#endif

#ifndef ION_CLASS_NON_COPYABLE
	#define ION_CLASS_NON_COPYABLE(T)   \
		T(const T&) = delete;           \
		T& operator=(const T&) = delete
#endif

// Logging / errors. The standalone build keeps these as cheap streamable macros
// that go to stderr; replace with your own if you need a real logger.
#define ION_LOG_INFO(__expr)                                            \
	do                                                                  \
	{                                                                   \
		std::ostringstream _ion_log_oss;                                \
		_ion_log_oss << __expr;                                         \
		std::fputs(_ion_log_oss.str().c_str(), stderr);                 \
		std::fputc('\n', stderr);                                       \
	} while (0)

#define ION_DBG(__expr) ION_LOG_INFO(__expr)
#define ION_ABNORMAL(__expr) ION_LOG_INFO("[Abnormal] " << __expr)

#define ION_ASSERT(__cond, __msg)                                       \
	do                                                                  \
	{                                                                   \
		if (!(__cond))                                                  \
		{                                                               \
			std::ostringstream _ion_assert_oss;                         \
			_ion_assert_oss << "Assertion failed: " #__cond " - " << __msg; \
			std::fputs(_ion_assert_oss.str().c_str(), stderr);          \
			std::fputc('\n', stderr);                                   \
			std::abort();                                               \
		}                                                               \
	} while (0)

#define ION_MEMORY_SCOPE(__tag) ((void)0)

// ----------------------------------------------------------------------------
// Type aliases
// ----------------------------------------------------------------------------
namespace ion
{
using i8 = int8_t;
using u8 = uint8_t;
using i16 = int16_t;
using u16 = uint16_t;
using i32 = int32_t;
using u32 = uint32_t;
using i64 = int64_t;
using u64 = uint64_t;
using byte = uint8_t;
using ByteSizeType = uint32_t;

namespace tag
{
struct External_t
{
};
inline constexpr External_t External{};
}  // namespace tag

template <typename T>
struct GlobalAllocator
{
	using value_type = T;
	GlobalAllocator() noexcept = default;
	template <typename U>
	GlobalAllocator(const GlobalAllocator<U>&) noexcept
	{
	}
	T* allocate(size_t n) { return static_cast<T*>(std::malloc(n * sizeof(T))); }
	void deallocate(T* p, size_t /*n*/) { std::free(p); }
};

inline void* Malloc(size_t n) { return std::malloc(n); }
inline void Free(void* p) { std::free(p); }

// ----------------------------------------------------------------------------
// Math + bit utilities
// ----------------------------------------------------------------------------
template <typename T>
constexpr const T& Min(const T& a, const T& b)
{
	return a < b ? a : b;
}
template <typename T, typename U>
constexpr auto Min(const T& a, const U& b) -> typename std::common_type<T, U>::type
{
	using R = typename std::common_type<T, U>::type;
	return R(a) < R(b) ? R(a) : R(b);
}
template <typename T>
constexpr const T& Max(const T& a, const T& b)
{
	return a < b ? b : a;
}
inline float Absf(float x) { return x < 0.0f ? -x : x; }
template <typename T>
constexpr T Mod2(T value, T modulus)
{
	return value % modulus;
}
constexpr size_t BitCountToByteCount(size_t bits) { return (bits + 7) / 8; }

template <typename T>
constexpr unsigned CountLeadingZeroes(T value)
{
	if (value == 0) return sizeof(T) * 8;
	return unsigned(std::countl_zero(static_cast<std::make_unsigned_t<T>>(value)));
}

template <typename TDst, typename TSrc>
constexpr TDst SafeRangeCast(TSrc src)
{
	TDst out = static_cast<TDst>(src);
	ION_ASSERT(static_cast<TSrc>(out) == src, "SafeRangeCast value out of range");
	return out;
}

// ----------------------------------------------------------------------------
// ArrayView
// ----------------------------------------------------------------------------
template <typename T, typename CountType = size_t>
class ArrayView
{
public:
	ArrayView() = default;
	ArrayView(T* data, CountType size) : mData(data), mSize(size) {}
	template <typename Container>
	ArrayView(Container& c) : mData(c.Data()), mSize(CountType(c.Size()))
	{
	}

	[[nodiscard]] T* Data() const { return mData; }
	[[nodiscard]] CountType Size() const { return mSize; }

private:
	T* mData = nullptr;
	CountType mSize = 0;
};

// ----------------------------------------------------------------------------
// Vector - thin wrapper over std::vector exposing the ion::Vector API used
// by this library.
// ----------------------------------------------------------------------------
template <typename T, typename Allocator = GlobalAllocator<T>>
class Vector
{
public:
	Vector() = default;
	Vector(const Vector&) = default;
	Vector(Vector&&) noexcept = default;
	Vector& operator=(const Vector&) = default;
	Vector& operator=(Vector&&) noexcept = default;

	void Reserve(size_t n) { mImpl.reserve(n); }
	void Resize(size_t n) { mImpl.resize(n); }
	// std::vector has no skip-default-construction resize, so this is just resize().
	// The ion::Vector original skips T construction for trivially-constructible T.
	void ResizeFast(size_t n) { mImpl.resize(n); }
	[[nodiscard]] size_t Size() const { return mImpl.size(); }
	[[nodiscard]] size_t Capacity() const { return mImpl.capacity(); }
	[[nodiscard]] T* Data() { return mImpl.data(); }
	[[nodiscard]] const T* Data() const { return mImpl.data(); }
	void Add(const T& v) { mImpl.push_back(v); }
	void Add(T&& v) { mImpl.push_back(std::move(v)); }
	void AddKeepCapacity(const T& v) { mImpl.push_back(v); }
	void AddKeepCapacity(T&& v) { mImpl.push_back(std::move(v)); }
	[[nodiscard]] T& Back() { return mImpl.back(); }
	[[nodiscard]] const T& Back() const { return mImpl.back(); }
	void Clear() { mImpl.clear(); }
	void ShrinkToFit() { mImpl.shrink_to_fit(); }
	[[nodiscard]] bool IsEmpty() const { return mImpl.empty(); }
	T& operator[](size_t i) { return mImpl[i]; }
	const T& operator[](size_t i) const { return mImpl[i]; }

private:
	std::vector<T> mImpl;
};

// SmallVector with inline buffer optimisation - we just delegate to Vector.
template <typename T, size_t /*N*/>
using SmallVector = Vector<T>;

// ----------------------------------------------------------------------------
// ByteBuffer / ByteReader / ByteWriter
//
// The library treats these as: an owning byte buffer, a forward-only byte
// reader over a region, and a forward-only byte writer that can grow the
// underlying buffer.
// ----------------------------------------------------------------------------
class ByteBufferBase
{
public:
	virtual ~ByteBufferBase() = default;
	virtual byte* Begin() = 0;
	virtual const byte* Begin() const = 0;
	virtual size_t Size() const = 0;
	virtual size_t Capacity() const = 0;
	virtual void Resize(size_t n) = 0;
	virtual void Reserve(size_t n) = 0;
	virtual void Rewind() = 0;
};

template <size_t /*Inline*/ = 0, typename Allocator = GlobalAllocator<u8>>
class ByteBuffer : public ByteBufferBase
{
public:
	ByteBuffer() = default;
	explicit ByteBuffer(ByteSizeType cap) { mImpl.reserve(cap); }
	ByteBuffer(const ByteBuffer&) = default;
	ByteBuffer(ByteBuffer&&) noexcept = default;
	ByteBuffer& operator=(const ByteBuffer&) = default;
	ByteBuffer& operator=(ByteBuffer&&) noexcept = default;

	byte* Begin() override { return mImpl.data(); }
	const byte* Begin() const override { return mImpl.data(); }
	size_t Size() const override { return mImpl.size(); }
	size_t Capacity() const override { return mImpl.capacity(); }
	void Resize(size_t n) override { mImpl.resize(n); }
	void Reserve(size_t n) override { mImpl.reserve(n); }
	void Rewind() override { mImpl.clear(); }

private:
	std::vector<byte> mImpl;
};

class ByteReader
{
public:
	ByteReader() = default;
	ByteReader(const byte* data, size_t size) : mStart(data), mPos(data), mEnd(data + size) {}
	template <typename Buffer>
	ByteReader(const Buffer& buf) : mStart(buf.Begin()), mPos(buf.Begin()), mEnd(buf.Begin() + buf.Size())
	{
	}

	[[nodiscard]] size_t Available() const { return size_t(mEnd - mPos); }
	[[nodiscard]] size_t Size() const { return size_t(mEnd - mStart); }
	[[nodiscard]] const byte* Data() const { return mStart; }
	void Rewind() { mPos = mStart; }
	void SkipBytes(size_t n) { mPos += n; }

	template <typename T>
	bool Read(T& out)
	{
		if (Available() < sizeof(T)) return false;
		std::memcpy(&out, mPos, sizeof(T));
		mPos += sizeof(T);
		return true;
	}
	template <typename T>
	void ReadAssumeAvailable(T& out)
	{
		std::memcpy(&out, mPos, sizeof(T));
		mPos += sizeof(T);
	}
	void ReadAssumeAvailable(u8* dst, size_t n)
	{
		std::memcpy(dst, mPos, n);
		mPos += n;
	}
	template <typename T>
	bool Process(T& out)
	{
		return Read(out);
	}

	// Pass a callable [&](const byte* start, const byte* end) -> size_t (consumed bytes).
	template <typename Callback>
	void ReadDirectly(Callback&& cb)
	{
		size_t consumed = cb(const_cast<byte*>(mPos), const_cast<byte*>(mEnd));
		mPos += consumed;
	}

private:
	const byte* mStart = nullptr;
	const byte* mPos = nullptr;
	const byte* mEnd = nullptr;
};

class ByteWriter
{
public:
	ByteWriter() = default;
	explicit ByteWriter(ByteBufferBase& buffer) : mBuffer(&buffer) {}
	ByteWriter(ByteWriter&&) noexcept = default;
	ByteWriter& operator=(ByteWriter&&) noexcept = default;
	ByteWriter(const ByteWriter&) = delete;
	ByteWriter& operator=(const ByteWriter&) = delete;

	void EnsureCapacity(size_t cap)
	{
		if (!mBuffer) return;
		if (mBuffer->Capacity() < mBuffer->Size() + cap)
		{
			mBuffer->Reserve(mBuffer->Size() + cap);
		}
	}
	void Flush() {}
	[[nodiscard]] size_t NumBytesUsed() const { return mBuffer ? mBuffer->Size() : 0; }

	template <typename T>
	bool Write(const T& v)
	{
		ION_ASSERT(mBuffer, "ByteWriter without buffer");
		const size_t off = mBuffer->Size();
		mBuffer->Resize(off + sizeof(T));
		std::memcpy(mBuffer->Begin() + off, &v, sizeof(T));
		return true;
	}
	template <typename T>
	bool Process(const T& v)
	{
		return Write(v);
	}

	void WriteArray(const u8* src, size_t n)
	{
		ION_ASSERT(mBuffer, "ByteWriter without buffer");
		const size_t off = mBuffer->Size();
		mBuffer->Resize(off + n);
		std::memcpy(mBuffer->Begin() + off, src, n);
	}
	void WriteArrayKeepCapacity(const u8* src, size_t n) { WriteArray(src, n); }

	// Callback signature: [&](byte* start, byte* end) -> size_t (bytes written).
	template <typename Callback>
	size_t WriteDirectly(Callback&& cb)
	{
		ION_ASSERT(mBuffer, "ByteWriter without buffer");
		const size_t before = mBuffer->Size();
		const size_t avail = mBuffer->Capacity() - before;
		mBuffer->Resize(mBuffer->Capacity());
		size_t produced = cb(mBuffer->Begin() + before, mBuffer->Begin() + mBuffer->Size());
		mBuffer->Resize(before + produced);
		return produced;
	}

private:
	ByteBufferBase* mBuffer = nullptr;
};

class ByteWriterUnsafe
{
public:
	explicit ByteWriterUnsafe(byte* dst) : mStart(dst), mPos(dst) {}

	template <typename T>
	void Write(const T& v)
	{
		std::memcpy(mPos, &v, sizeof(T));
		mPos += sizeof(T);
	}
	[[nodiscard]] size_t NumBytesUsed() const { return size_t(mPos - mStart); }

private:
	byte* mStart = nullptr;
	byte* mPos = nullptr;
};

// ----------------------------------------------------------------------------
// Random - simple wrapper over std::mt19937.
// ----------------------------------------------------------------------------
class Random
{
public:
	Random() : mRng(std::random_device{}()) {}
	explicit Random(uint32_t seed) : mRng(seed) {}

	uint32_t UInt32() { return mRng(); }
	float GetFastFloat()
	{
		// Uniform float in [0, 1).
		return float(mRng() >> 8) * (1.0f / 16777216.0f);
	}

private:
	std::mt19937 mRng;
};

}  // namespace ion
