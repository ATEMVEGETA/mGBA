/* Windows-only native-owner stress diagnostic. The real RFU/SIO/mTiming code
 * runs on two native owner threads, while a third thread services the bounded
 * wall clock. No Qt, CPU, ROM, save file, or production protocol mock is used.
 * Sleep/wake callbacks only set durable event state; actual waits occur in the
 * outer owner loop, never inside an RFU callback or coordinator lock. */
#define RFU_STRESS_THREADS
#define main regressionFixtureMain
#include "rfu-regression.c"
#undef main

#ifndef _WIN32
#error This stress fixture uses Windows native events and threads
#endif
#ifdef DISABLE_THREADING
#error This stress fixture requires actual native coordinator mutexes
#endif

struct NativeCooperativeOwner {
	struct mLockstepUser d;
	HANDLE start;
	HANDLE wake;
	HANDLE thread;
	volatile LONG asleep;
	volatile LONG stop;
	volatile LONG control;
	volatile LONG invalid;
	volatile LONG sleeps;
	volatile LONG wakes;
	unsigned id;
	bool protocol;
	struct CooperativeRoundOwner round;
};

static void nativeSleepRequest(struct mLockstepUser* user) {
	struct NativeCooperativeOwner* owner = (struct NativeCooperativeOwner*) user;
	CHECK(!InterlockedCompareExchange(&owner->invalid, 0, 0));
	ResetEvent(owner->wake);
	InterlockedExchange(&owner->asleep, 1);
	InterlockedIncrement(&owner->sleeps);
}

static void nativeWakeRequest(struct mLockstepUser* user) {
	struct NativeCooperativeOwner* owner = (struct NativeCooperativeOwner*) user;
	CHECK(!InterlockedCompareExchange(&owner->invalid, 0, 0));
	InterlockedExchange(&owner->asleep, 0);
	SetEvent(owner->wake);
	InterlockedIncrement(&owner->wakes);
}

static uint64_t nativeNowUs(void) {
	return GetTickCount64() * UINT64_C(1000) + 1;
}

static DWORD WINAPI nativeOwnerMain(void* context) {
	struct NativeCooperativeOwner* owner = context;
	CHECK(WaitForSingleObject(owner->start, 5000) == WAIT_OBJECT_0);
	bool started = false;
	for (unsigned turns = 0; !InterlockedCompareExchange(&owner->stop, 0, 0); ++turns) {
		const LONG control = InterlockedExchange(&owner->control, 0);
		if (control) {
			/* These operations execute on their own core, including while the
			 * peer is concurrently posting/claiming through the coordinator. */
			switch (control) {
			case 1: machines[owner->id]->rfu.d.reset(&machines[owner->id]->rfu.d); break;
			case 2: GBASIOWriteRCNT(&machines[owner->id]->gba.sio, 0x8000); break;
			case 3: GBASIORFUDriverSetCooperative(&machines[owner->id]->rfu, NULL); break;
			case 4: GBASIORFUCoordinatorDetach(&coordinator, &machines[owner->id]->rfu); break;
			default: CHECK(false);
			}
			GBASIORFUDriverSetCooperative(&machines[owner->id]->rfu, NULL);
			InterlockedExchange(&owner->invalid, 1);
			return 0;
		}
		if (InterlockedCompareExchange(&owner->asleep, 0, 0)) {
			/* Durable flag handles a peer wake between requesting sleep and
			 * reaching this wait. The event handles the opposite interleaving. */
			CHECK(WaitForSingleObject(owner->wake, 10) != WAIT_FAILED);
			continue;
		}
		if (owner->protocol) {
			cooperativeRoundService(owner->id, &owner->round);
			if (owner->round.done) return 0;
		} else if (!started) {
			uint32_t payload[20] = {0};
			payload[0] = owner->id ? 16 << 8 : 73;
			memcpy(payload + 1, owner->id ? cooperativeClientBytes : cooperativeHostBytes, owner->id ? 16 : 73);
			const unsigned words = owner->id ? 5 : 20;
			EQ(transferOne(owner->id, 0x99660025 | (words << 8)), 0x80000000);
			for (unsigned word = 0; word < words; ++word) EQ(transferOne(owner->id, payload[word]), 0x80000000);
			const uint32_t header = transferOne(owner->id, 0x80000000);
			if (header == 0x996601EE) {
				/* In this close race the real host can retire while the client
				 * is still shifting command payload. A disconnected-client
				 * rejection is valid; it must not become a fake successful TX. */
				EQ(owner->id, 1);
				EQ(transferOne(owner->id, 0x80000000), 1);
				EQ(machines[owner->id]->rfu.state, GBA_RFU_COMMAND);
				EQ(machines[owner->id]->rfu.txAckMask, 0);
			} else {
				EQ(header, 0x996600A5);
				armAsyncWithoutAdvancing(owner->id);
			}
			started = true;
		}
		advanceOne(owner->id, 128);
		if (!(turns & 63)) SwitchToThread();
	}
	return 0;
}

static struct NativeCooperativeOwner* createNativeOwner(unsigned id, bool protocol) {
	struct NativeCooperativeOwner* owner = calloc(1, sizeof(*owner));
	CHECK(owner != NULL);
	owner->id = id;
	owner->protocol = protocol;
	owner->d.sleep = nativeSleepRequest;
	owner->d.wake = nativeWakeRequest;
	owner->start = CreateEventW(NULL, TRUE, FALSE, NULL);
	owner->wake = CreateEventW(NULL, TRUE, FALSE, NULL);
	CHECK(owner->start != NULL && owner->wake != NULL);
	GBASIORFUDriverSetCooperative(&machines[id]->rfu, &owner->d);
	owner->thread = CreateThread(NULL, 0, nativeOwnerMain, owner, 0, NULL);
	CHECK(owner->thread != NULL);
	return owner;
}

static void nativeInterruptOwner(struct NativeCooperativeOwner* owner) {
	/* Model the frontend interrupt/stop path. Do not enter RFU or wait with
	 * any frontend lock held. The owner retires its actual RFU state itself. */
	InterlockedExchange(&owner->asleep, 0);
	SetEvent(owner->wake);
}

static void destroyNativeOwner(struct NativeCooperativeOwner* owner) {
	CHECK(WaitForSingleObject(owner->thread, 2000) == WAIT_OBJECT_0);
	GBASIORFUDriverSetCooperative(&machines[owner->id]->rfu, NULL);
	InterlockedExchange(&owner->invalid, 1);
	CHECK(CloseHandle(owner->thread));
	CHECK(CloseHandle(owner->start));
	CHECK(CloseHandle(owner->wake));
	free(owner);
}

static void nativeServiceUntilJoined(struct NativeCooperativeOwner* owners[2], unsigned timeoutMs) {
	const ULONGLONG deadline = GetTickCount64() + timeoutMs;
	while (WaitForSingleObject(owners[0]->thread, 0) == WAIT_TIMEOUT ||
		WaitForSingleObject(owners[1]->thread, 0) == WAIT_TIMEOUT) {
		CHECK(GetTickCount64() < deadline);
		GBASIORFUCoordinatorService(&coordinator, nativeNowUs());
		Sleep(1);
	}
}

static void testNativeProtocol(unsigned iteration) {
	beginPrearmed("Native concurrent RFU owners exchange exact packets through cooperative requests");
	synchronizePair();
	const uint32_t settings = 0x0420;
	EQ(commandOne(0, 0x17, &settings, 1, NULL), 0);
	EQ(commandOne(1, 0x17, &settings, 1, NULL), 0);
	struct NativeCooperativeOwner* owners[2] = {createNativeOwner(0, true), createNativeOwner(1, true)};
	GBASIORFUCoordinatorService(&coordinator, nativeNowUs());
	SetEvent(owners[iteration & 1]->start);
	/* Exercise an actual waiting owner, not only equal-time event pumping. */
	Sleep(iteration % 3);
	SetEvent(owners[1 - (iteration & 1)]->start);
	nativeServiceUntilJoined(owners, 5000);
	for (unsigned id = 0; id < 2; ++id) {
		CHECK(owners[id]->round.done);
		EQ(owners[id]->round.receipts, 1);
	}
	destroyNativeOwner(owners[0]);
	destroyNativeOwner(owners[1]);
	end();
}

static void testNativeRetirement(unsigned control, unsigned iteration) {
	beginPrearmed("Native reset/mode/hook-removal/detach race peer posting without a stale callback");
	synchronizePair();
	struct NativeCooperativeOwner* owners[2] = {createNativeOwner(0, false), createNativeOwner(1, false)};
	GBASIORFUCoordinatorService(&coordinator, nativeNowUs());
	/* Only a real host OFFER may park before its peer has entered WAIT.
	 * Retire that waiting host while the client concurrently adopts/posts. */
	SetEvent(owners[0]->start);
	const ULONGLONG deadline = GetTickCount64() + 3000;
	while (!InterlockedCompareExchange(&owners[0]->asleep, 0, 0)) {
		CHECK(GetTickCount64() < deadline);
		GBASIORFUCoordinatorService(&coordinator, nativeNowUs());
		Sleep(1);
	}
	/* Start the client and request host retirement in alternating orders.
	 * The actual reset/detach runs only on the host owner, never the main. */
	if (iteration & 1) SetEvent(owners[1]->start);
	InterlockedExchange(&owners[0]->control, (LONG) control);
	nativeInterruptOwner(owners[0]);
	if (!(iteration & 1)) SetEvent(owners[1]->start);
	while (WaitForSingleObject(owners[0]->thread, 0) == WAIT_TIMEOUT) {
		CHECK(GetTickCount64() < deadline);
		GBASIORFUCoordinatorService(&coordinator, nativeNowUs());
		Sleep(1);
	}
	/* The retired callback user can be freed while the peer owner and
	 * coordinator service still run. Any stale callback fails before free;
	 * native heap diagnostics can additionally catch access after free. */
	destroyNativeOwner(owners[0]);
	owners[0] = NULL;
	for (unsigned i = 0; i < 5; ++i) {
		GBASIORFUCoordinatorService(&coordinator, nativeNowUs());
		Sleep(1);
	}
	InterlockedExchange(&owners[1]->stop, 1);
	nativeInterruptOwner(owners[1]);
	destroyNativeOwner(owners[1]);
	end();
}

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	for (unsigned iteration = 0; iteration < 32; ++iteration) testNativeProtocol(iteration);
	for (unsigned iteration = 0; iteration < 8; ++iteration) {
		for (unsigned control = 1; control <= 4; ++control) testNativeRetirement(control, iteration);
	}
	printf("Native cooperative stress passed: %u cases (%u atomic assertions). No Qt/CPU/ROM run.\n", testCases, assertions);
	return 0;
}
