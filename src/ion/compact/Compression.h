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

#ifdef XXH_FALLTHROUGH
	#undef XXH_FALLTHROUGH
#endif
#include "zstd/lib/zstd.h"
#define FSE_STATIC_LINKING_ONLY
#include "zstd/lib/common/huf.h"

namespace ion
{

namespace compact
{

constexpr unsigned TableLogMax = 11;
constexpr unsigned MaxSymbol = 255;

struct Compression
{
	alignas(alignof(size_t)) FSE_CTable mFseCTable[FSE_CTABLE_SIZE_U32(TableLogMax, MaxSymbol)];
	// Separate HUF CTable so FSE encodes don't invalidate it; lets HUF reuse the table
	// across consecutive Huf calls on the same Compression (skipping the table header).
	alignas(alignof(size_t)) HUF_CElt mHufCTable[HUF_CTABLE_SIZE_ST(MaxSymbol)];
	HUF_repeat mHufRepeat = HUF_repeat_none;
	U32 scratchBuffer[Max(size_t(HUF_WORKSPACE_SIZE / sizeof(U32)), size_t(FSE_BUILD_CTABLE_WORKSPACE_SIZE(MaxSymbol, TableLogMax)))];
	unsigned count[MaxSymbol + 1];
	S16 norm[MaxSymbol + 1];
	ZSTD_CCtx* mCctx = nullptr;
};
}  // namespace compact
}  // namespace ion
