/* Test-only force include for the NativeMutex runner. Windows critical
 * sections are recursive; reject nesting so accidental lock-held posting
 * also exposes the deadlock risk on non-recursive mutex implementations.
 * This does not substitute for concurrent-owner or frontend validation. */
#ifndef RFU_TEST_NATIVE_MUTEX_CHECK_H
#define RFU_TEST_NATIVE_MUTEX_CHECK_H

#include <mgba-util/threading.h>

#ifdef DISABLE_THREADING
#error Native mutex checks require threading enabled
#endif

static _Thread_local unsigned rfuTestMutexDepth;

static inline int rfuTestMutexLock(Mutex* mutex) {
	assert(rfuTestMutexDepth == 0);
	int result = MutexLock(mutex);
	++rfuTestMutexDepth;
	return result;
}

static inline int rfuTestMutexUnlock(Mutex* mutex) {
	assert(rfuTestMutexDepth == 1);
	--rfuTestMutexDepth;
	return MutexUnlock(mutex);
}

#define MutexLock rfuTestMutexLock
#define MutexUnlock rfuTestMutexUnlock

#endif
