/* Included after the real RFU/SIO/mTiming protocol fixture. No RFU protocol
 * state machine is mocked: only the frontend's durable sleep/wake requests are
 * recorded, and the explicit scheduler below honors those requests. */
struct CooperativeTestUser {
	struct mLockstepUser d;
	bool asleep;
	unsigned sleeps;
	unsigned wakes;
};

static struct CooperativeTestUser cooperativeUsers[2];

static void cooperativeTestSleep(struct mLockstepUser* user) {
	struct CooperativeTestUser* fixture = (struct CooperativeTestUser*) user;
	CHECK(!fixture->asleep);
	fixture->asleep = true;
	++fixture->sleeps;
}

static void cooperativeTestWake(struct mLockstepUser* user) {
	struct CooperativeTestUser* fixture = (struct CooperativeTestUser*) user;
	fixture->asleep = false;
	++fixture->wakes;
}

static void enableCooperativeTestUsers(bool serviceClock) {
	memset(cooperativeUsers, 0, sizeof(cooperativeUsers));
	for (unsigned id = 0; id < 2; ++id) {
		cooperativeUsers[id].d.sleep = cooperativeTestSleep;
		cooperativeUsers[id].d.wake = cooperativeTestWake;
		GBASIORFUDriverSetCooperative(&machines[id]->rfu, &cooperativeUsers[id].d);
	}
	if (serviceClock) GBASIORFUCoordinatorService(&coordinator, 1000000);
}

static void beginCooperative(const char* name) {
	beginPrearmed(name);
	synchronizePair();
	enableCooperativeTestUsers(true);
}

static unsigned cooperativeHostWait(void) {
	const uint32_t settings = 0x0420;
	const uint32_t payload[] = {4, 0xCAFEBABE};
	EQ(commandOne(0, 0x17, &settings, 1, NULL), 0);
	EQ(commandOne(0, 0x25, payload, 2, NULL), 0);
	return armAsyncWithoutAdvancing(0);
}

static void reachCooperativeSleep(unsigned id) {
	for (unsigned i = 0; !cooperativeUsers[id].asleep && i < 64; ++i) advanceOne(id, 128);
	CHECK(cooperativeUsers[id].asleep);
	CHECK(machines[id]->rfu.cooperativeAsleep);
	CHECK(machines[id]->gba.earlyExit);
	CHECK(machines[id]->rfu.state == GBA_RFU_WAIT);
	CHECK(!machines[id]->rfu.internalPending && !machines[id]->rfu.adapterPending);
}

static void cooperativeAdvance(unsigned cycles) {
	CHECK(cycles % 128 == 0);
	for (unsigned spent = 0; spent < cycles; spent += 128) {
		for (unsigned id = 0; id < 2; ++id) {
			if (machines[id]->rfu.internalPending || machines[id]->rfu.adapterPending) {
				CHECK(!cooperativeUsers[id].asleep);
			}
			if (!cooperativeUsers[id].asleep) advanceOne(id, 128);
		}
		CHECK(!(cooperativeUsers[0].asleep && cooperativeUsers[1].asleep));
	}
}

static void testCooperativeOptions(void) {
	for (unsigned variant = 0; variant < 3; ++variant) {
		static const char* const names[] = {
			"Default NULL cooperative user retains finite legacy retry behavior",
			"Unserviced cooperative clock cannot park a core indefinitely",
			"Cooperative user without RFU synchronization leaves transport unchanged"
		};
		beginPrearmed(names[variant]);
		synchronizePair();
		CHECK(machines[0]->rfu.cooperativeUser == NULL);
		if (variant) enableCooperativeTestUsers(variant == 2);
		if (variant == 2) {
			GBASIORFUDriverSetSynchronize(&machines[0]->rfu, false);
			GBASIORFUDriverSetSynchronize(&machines[1]->rfu, false);
		}
		const unsigned irqs = cooperativeHostWait();
		advanceOne(0, FRAME_CYCLES);
		EQ(machines[0]->irqs, irqs + 1);
		EQ(rx(0), 0x99660128);
		CHECK(!machines[0]->rfu.cooperativeAsleep);
		if (variant) EQ(cooperativeUsers[0].sleeps, 0);
		end();
	}
}

static void testCooperativeTimeoutBudget(void) {
	beginCooperative("One wall-time budget bounds a paused-peer WAIT; repeated service cannot renew it");
	const unsigned irqs = cooperativeHostWait();
	reachCooperativeSleep(0);
	const uint32_t frozen = mTimingCurrentTime(&machines[0]->gba.timing);
	const uint32_t retry = machines[0]->rfu.retryCycles;
	const uint64_t deadline = machines[0]->rfu.cooperativeDeadlineUs;
	CHECK(deadline == 1050000);
	GBASIORFUCoordinatorService(&coordinator, deadline - 1);
	GBASIORFUCoordinatorService(&coordinator, deadline - 1);
	CHECK(cooperativeUsers[0].asleep);
	EQ(mTimingCurrentTime(&machines[0]->gba.timing), frozen);
	EQ(machines[0]->rfu.retryCycles, retry);
	GBASIORFUCoordinatorService(&coordinator, deadline);
	CHECK(!cooperativeUsers[0].asleep);
	CHECK(machines[0]->rfu.cooperativeBypass);
	GBASIORFUCoordinatorService(&coordinator, deadline + 1000000);
	advanceOne(0, FRAME_CYCLES);
	EQ(machines[0]->irqs, irqs + 1);
	EQ(rx(0), 0x99660128);
	EQ(machines[0]->rfu.txAckMask, 0);
	EQ(machines[1]->rfu.rx.count, 0);
	EQ(cooperativeUsers[0].sleeps, 1);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	/* A genuine new command/ticket gets its own bounded opportunity. */
	EQ(commandOne(0, 0x37, NULL, 0, NULL), 0);
	armAsyncWithoutAdvancing(0);
	reachCooperativeSleep(0);
	CHECK(machines[0]->rfu.cooperativeDeadlineUs > deadline);
	end();
}

static void testCooperativeSpuriousWakeAndFullMailbox(void) {
	beginConfigured("Mailbox wakes are durable and unrelated traffic never renews the WAIT deadline", 3, true);
	synchronizePair();
	enableCooperativeTestUsers(true);
	/* An unrelated, genuine broadcasting host supplies mailbox traffic. The
	 * connected client cannot broadcast while retaining its client role. */
	const uint32_t broadcast[] = {1, 2, 3, 4, 5, 6};
	handshake(2);
	EQ(commandOne(2, 0x16, broadcast, 6, NULL), 0);
	EQ(commandOne(2, 0x19, NULL, 0, NULL), 0);
	advance(128);
	const unsigned irqs = cooperativeHostWait();
	reachCooperativeSleep(0);
	const uint64_t deadline = machines[0]->rfu.cooperativeDeadlineUs;
	/* The owner has requested sleep but has not performed its external wait.
	 * Actual peer posting must clear that durable request before it returns. */
	EQ(commandOne(2, 0x16, broadcast, 6, NULL), 0);
	CHECK(!cooperativeUsers[0].asleep);
	CHECK(!machines[0]->rfu.cooperativeAsleep);
	CHECK(machines[0]->rfu.incomingCount > 0);
	GBASIORFUCoordinatorService(&coordinator, 1010000);
	reachCooperativeSleep(0);
	CHECK(machines[0]->rfu.cooperativeDeadlineUs == deadline);
	/* Repeated real peer commands coalesce advertisements rather than fill
	 * the mailbox. Such posts still cannot create a stale sleeping flag. */
	for (unsigned i = 0; i < GBA_RFU_MESSAGE_DEPTH + 1; ++i) {
		EQ(commandOne(2, 0x16, broadcast, 6, NULL), 0);
	}
	/* Repeated advertisements from one peer coalesce to its newest snapshot;
	 * they must still wake durably without extending the exchange budget. */
	EQ(machines[0]->rfu.incomingCount, 1);
	CHECK(!cooperativeUsers[0].asleep);
	advanceOne(0, 128);
	EQ(machines[0]->rfu.incomingCount, 0);
	CHECK(machines[0]->rfu.cooperativeDeadlineUs == deadline);
	CHECK(cooperativeUsers[0].asleep);
	EQ(machines[0]->irqs, irqs);
	EQ(machines[0]->rfu.txAckMask, 0);
	GBASIORFUCoordinatorService(&coordinator, deadline);
	CHECK(!cooperativeUsers[0].asleep);
	advanceOne(0, FRAME_CYCLES);
	EQ(rx(0), 0x99660128);
	EQ(machines[0]->irqs, irqs + 1);
	end();
}

static void testCooperativeLifecycle(void) {
	static const char* const names[] = {
		"Cooperative callback removal wakes before its frontend user can be destroyed",
		"Cooperative synchronization disable retires a blocked WAIT",
		"Cooperative owner reset retires a blocked WAIT",
		"Cooperative serial mode change retires a blocked WAIT",
		"Cooperative peripheral deinit retires a blocked WAIT",
		"Cooperative peer detach releases the blocked survivor",
		"Cooperative owner detach releases its frontend sleep request",
		"Cooperative peer explicit disconnect wakes a blocked WAIT"
	};
	for (unsigned variant = 0; variant < COUNT_OF(names); ++variant) {
		beginCooperative(names[variant]);
		cooperativeHostWait();
		reachCooperativeSleep(0);
		switch (variant) {
		case 0: GBASIORFUDriverSetCooperative(&machines[0]->rfu, NULL); break;
		case 1: GBASIORFUDriverSetSynchronize(&machines[0]->rfu, false); break;
		case 2: machines[0]->rfu.d.reset(&machines[0]->rfu.d); break;
		case 3: GBASIOWriteRCNT(&machines[0]->gba.sio, 0x8000); break;
		case 4: GBASIODeinit(&machines[0]->gba.sio); break;
		case 5: GBASIORFUCoordinatorDetach(&coordinator, &machines[1]->rfu); break;
		case 6: GBASIORFUCoordinatorDetach(&coordinator, &machines[0]->rfu); break;
		case 7: EQ(commandOne(1, 0x30, NULL, 0, NULL), 0); break;
		}
		CHECK(!cooperativeUsers[0].asleep);
		CHECK(!machines[0]->rfu.cooperativeAsleep);
		CHECK(cooperativeUsers[0].wakes > 0);
		/* Service after invalidation must not touch an obsolete callback user. */
		const unsigned sleeps = cooperativeUsers[0].sleeps;
		GBASIORFUCoordinatorService(&coordinator, UINT64_C(5000000));
		EQ(cooperativeUsers[0].sleeps, sleeps);
		end();
	}
}

static void testCooperativePeerRetirementBarrier(void) {
	for (unsigned beforeFirstPark = 0; beforeFirstPark < 2; ++beforeFirstPark) {
		for (unsigned variant = 0; variant < 4; ++variant) {
			beginCooperative(beforeFirstPark ?
				"Peer lifecycle before first park remains a sticky barrier for this explicit WAIT" :
				"Peer lifecycle immediately wakes and bypasses an already parked explicit WAIT");
			const unsigned irqs = cooperativeHostWait();
			if (!beforeFirstPark) reachCooperativeSleep(0);
			switch (variant) {
			case 0: GBASIORFUDriverSetCooperative(&machines[1]->rfu, NULL); break;
			case 1: GBASIOWriteRCNT(&machines[1]->gba.sio, 0x8000); break;
			case 2: machines[1]->rfu.d.reset(&machines[1]->rfu.d); break;
			case 3: GBASIORFUCoordinatorDetach(&coordinator, &machines[1]->rfu); break;
			}
			CHECK(!cooperativeUsers[0].asleep);
			CHECK(machines[0]->rfu.cooperativePeerRetired);
			advanceOne(0, 1024);
			CHECK(!cooperativeUsers[0].asleep);
			CHECK(machines[0]->rfu.cooperativePeerRetired);
			advanceOne(0, FRAME_CYCLES);
			EQ(machines[0]->irqs, irqs + 1);
			/* A peer reset/detach may generate disconnect rather than retry
			 * status; either must be a genuine bounded adapter notification. */
			const uint32_t header = rx(0);
			CHECK(header == 0x99660128 || header == 0x99660129 || header == 0x99660027 || header == 0x99660028);
			EQ(machines[0]->rfu.txAckMask, 0); /* A removed target is not a fabricated receipt. */
			if (header & 0x100) asynchronousReceive(0, CLOCK_FAST);
			EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
			const uint32_t settings = 0x0420;
			EQ(commandOne(0, 0x17, &settings, 1, NULL), 0);
			CHECK(!machines[0]->rfu.cooperativePeerRetired);
			end();
		}
	}
}

struct CooperativeRoundOwner {
	unsigned phase, irqs, words, retries, receipts;
	uint32_t header, status;
	bool done;
};

static const uint32_t cooperativeHostBytes[19] = {
	0x03020100, 0x07060504, 0x0B0A0908, 0x0F0E0D0C, 0x13121110,
	0x17161514, 0x1B1A1918, 0x1F1E1D1C, 0x23222120, 0x27262524,
	0x2B2A2928, 0x2F2E2D2C, 0x33323130, 0x37363534, 0x3B3A3938,
	0x3F3E3D3C, 0x43424140, 0x47464544, 0x00000048
};
static const uint32_t cooperativeClientBytes[4] = {0x83828180, 0x87868584, 0x8B8A8988, 0x8F8E8D8C};

static void cooperativeRoundService(unsigned id, struct CooperativeRoundOwner* owner) {
	if (owner->done) return;
	if (!owner->phase) {
		if (owner->retries) EQ(commandOne(id, 0x37, NULL, 0, NULL), 0);
		else {
			uint32_t payload[20] = {0};
			payload[0] = id ? 16 << 8 : 73;
			memcpy(payload + 1, id ? cooperativeClientBytes : cooperativeHostBytes, id ? 16 : 73);
			EQ(commandOne(id, 0x25, payload, id ? 5 : 20, NULL), 0);
		}
		owner->irqs = armAsyncWithoutAdvancing(id);
		owner->phase = 1;
		return;
	}
	if (machines[id]->irqs == owner->irqs) return;
	EQ(machines[id]->irqs, owner->irqs + 1);
	CHECK(!(machines[id]->gba.sio.siocnt & TRANSFER_START));
	EQ(rx(id), machines[id]->irqWord);
	if (owner->phase == 1) {
		owner->header = rx(id);
		CHECK(owner->header == 0x99660028 || owner->header == 0x99660128 || owner->header == 0x99660027);
		owner->words = ((owner->header >> 8) & 0xFF) + 1;
		owner->status = 0;
		owner->phase = 2;
	} else {
		if ((owner->header & 0x100) && owner->words == 2) owner->status = rx(id);
		else EQ(rx(id), 0x80000000);
		CHECK(owner->words > 0);
		if (!--owner->words) {
			EQ(machines[id]->rfu.state, GBA_RFU_COMMAND);
			const bool success = owner->header == 0x99660028 ||
				(owner->header == 0x99660128 && (owner->status & 1));
			if (success) {
				if (!id) EQ(machines[id]->rfu.txAckMask & 1, 1);
				uint32_t response[32] = {0};
				const unsigned bytes = id ? 73 : 16;
				EQ(commandOne(id, 0x26, NULL, 0, response), 1 + (bytes + 3) / 4);
				EQ(response[0], id ? bytes : bytes << 8);
				CHECK(memcmp(response + 1, id ? cooperativeHostBytes : cooperativeClientBytes, bytes) == 0);
				++owner->receipts;
				owner->done = true;
			} else {
				++owner->retries;
				CHECK(owner->retries < 1000);
				owner->phase = 0;
			}
			return;
		}
	}
	owner->irqs = armAsyncWithoutAdvancing(id);
}

static void testCooperativeCoarseOwnerTurns(void) {
	for (unsigned enabled = 0; enabled < 2; ++enabled) {
		for (unsigned first = 0; first < 2; ++first) {
			beginPrearmed(enabled ? "Cooperative coarse owner turns avoid frontend-induced retry exhaustion" :
				"Disabled cooperative coarse owner turns retain the scheduling-gap baseline");
			synchronizePair();
			const uint32_t settings = 0x0420;
			EQ(commandOne(0, 0x17, &settings, 1, NULL), 0);
			EQ(commandOne(1, 0x17, &settings, 1, NULL), 0);
			if (enabled) enableCooperativeTestUsers(true);
			struct CooperativeRoundOwner owners[2] = {{0}};
			for (unsigned turn = 0; turn < 10000 && !(owners[0].done && owners[1].done); ++turn) {
				const unsigned id = (turn + first) & 1;
				const uint32_t start = mTimingCurrentTime(&machines[id]->gba.timing);
				while (mTimingCurrentTime(&machines[id]->gba.timing) - start < 584349U) {
					if (enabled && cooperativeUsers[id].asleep) break;
					cooperativeRoundService(id, &owners[id]);
					if (owners[id].done) break;
					advanceOne(id, 128);
				}
				if (enabled) CHECK(!(cooperativeUsers[0].asleep && cooperativeUsers[1].asleep));
			}
			CHECK(owners[0].done && owners[1].done);
			EQ(owners[0].receipts, 1);
			EQ(owners[1].receipts, 1);
			if (enabled) {
				EQ(owners[0].retries, 0);
				EQ(owners[1].retries, 0);
				/* Host-first must yield its genuine unresolved OFFER. Client-
				 * first may do only genuine alignment/serial work once offered;
				 * its earlier unoffered application WAIT must not be parked. */
				if (!first) CHECK(cooperativeUsers[0].sleeps > 0);
			} else if (!first) CHECK(owners[0].retries >= 3);
			end();
		}
	}
}

static void testCooperativeArmingAndOwnWork(void) {
	beginCooperative("Only armed external WAIT can park; genuine matched alignment and 256-cycle SPI keep running");
	const uint32_t host[] = {4, 0x12345678};
	EQ(commandOne(0, 0x25, host, 2, NULL), 0);
	advanceOne(0, 4096);
	EQ(cooperativeUsers[0].sleeps, 0); /* WAIT without START is not a blocked receive. */
	GBASIOWriteSIOCNT(&machines[0]->gba.sio, NORMAL_IRQ | SO_HIGH | CLOCK_FAST | TRANSFER_START);
	advanceOne(0, 1024);
	EQ(cooperativeUsers[0].sleeps, 0); /* SO high: console not ready. */
	GBASIOWriteSIOCNT(&machines[0]->gba.sio, NORMAL_IRQ | CLOCK_FAST);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	reachCooperativeSleep(0);
	const uint32_t client[] = {4 << 8, 0x90ABCDEF};
	EQ(commandOne(1, 0x25, client, 2, NULL), 0);
	CHECK(!cooperativeUsers[0].asleep);
	const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
	for (unsigned polls = 0; !machines[0]->rfu.adapterPending && polls < 40000; ++polls) cooperativeAdvance(128);
	CHECK(machines[0]->rfu.adapterPending);
	CHECK(!cooperativeUsers[0].asleep);
	finishPendingRadioHeader(0, hostIrqs, 0x99660028);
	for (unsigned polls = 0; polls < 40000 &&
		(machines[0]->irqs == hostIrqs || machines[1]->irqs == clientIrqs); ++polls) cooperativeAdvance(128);
	EQ(machines[0]->irqs, hostIrqs + 1);
	EQ(machines[1]->irqs, clientIrqs + 1);
	EQ(rx(0), 0x99660028);
	EQ(rx(1), 0x99660028);
	CHECK(!cooperativeUsers[0].asleep && !cooperativeUsers[1].asleep);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	expectShortPacket(0, 4, client[1]);
	expectShortPacket(1, 4, host[1]);
	end();
}

static void testCooperativeMatchedClientWaitsForRelease(void) {
	begin("Cooperative matched client finishes own alignment, parks for real RELEASE, then clocks exactly 256 cycles", 2);
	synchronizePair();
	enableCooperativeTestUsers(true);
	/* Start the host ahead so the client has genuine own-clock alignment
	 * work before its missing-RELEASE sleep is legitimate. */
	advanceOne(0, 4096);
	const uint32_t host[] = {4, 0x12345678};
	const uint32_t client[] = {4 << 8, 0x90ABCDEF};
	EQ(commandOne(0, 0x25, host, 2, NULL), 0);
	const uint64_t ticket = machines[0]->rfu.radioTicket;
	EQ(commandOne(1, 0x25, client, 2, NULL), 0);
	CHECK(machines[1]->rfu.radioPending && machines[1]->rfu.radioMatched && !machines[1]->rfu.radioReleased);
	const struct GBASIORFUMessage ready = holdRadioMessage(0, ticket, 1);
	const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
	CHECK(effectiveRadioClock(1) < machines[1]->rfu.radioDue);
	while (effectiveRadioClock(1) + 128 < machines[1]->rfu.radioDue) {
		advanceOne(1, 128);
		CHECK(!cooperativeUsers[1].asleep);
	}
	reachCooperativeSleep(1);
	CHECK(effectiveRadioClock(1) >= machines[1]->rfu.radioDue);
	CHECK(machines[1]->rfu.radioPending && machines[1]->rfu.radioMatched && !machines[1]->rfu.radioReleased);
	EQ(machines[1]->rfu.rx.count, 0);
	EQ(machines[1]->irqs, clientIrqs);
	/* Replay the genuine held READY only; production host completion must
	 * enqueue the actual RELEASE and atomically wake its recipient. */
	deliverRadioMessage(0, &ready);
	for (unsigned polls = 0; cooperativeUsers[1].asleep && polls < 64; ++polls) advanceOne(0, 128);
	CHECK(!cooperativeUsers[1].asleep);
	EQ(machines[0]->rfu.txAckMask, 1);
	finishPendingRadioHeader(1, clientIrqs, 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	awaitAsyncHeader(0, hostIrqs, 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	expectShortPacket(0, 4, client[1]);
	expectShortPacket(1, 4, host[1]);
	end();
}

struct CooperativeIdleTimer {
	struct mTimingEvent event;
	unsigned firings;
};

static void cooperativeIdleTimerEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct CooperativeIdleTimer* timer = context;
	++timer->firings;
	mTimingSchedule(timing, &timer->event, FRAME_CYCLES - cyclesLate);
}

struct CooperativeIdleClient {
	unsigned phase;
	unsigned irqs;
	unsigned waits;
	unsigned timeouts;
	uint8_t command;
};

static void serviceCooperativeIdleClient(struct CooperativeIdleClient* client) {
	if (!client->phase) {
		const uint32_t payload[] = {4 << 8, 0xDEADBEEF};
		const bool send = client->command == 0x25;
		EQ(commandOne(1, client->command, send ? payload : NULL, send ? 2 : 0, NULL), 0);
		client->irqs = armAsyncWithoutAdvancing(1);
		++client->waits;
		client->phase = 1;
	} else if (machines[1]->irqs != client->irqs) {
		EQ(machines[1]->irqs, client->irqs + 1);
		if (client->phase == 1) {
			EQ(rx(1), 0x99660027);
			client->irqs = armAsyncWithoutAdvancing(1);
			client->phase = 2;
		} else {
			EQ(rx(1), 0x80000000);
			EQ(machines[1]->rfu.state, GBA_RFU_COMMAND);
			++client->timeouts;
			client->phase = 0;
		}
	}
}

/* A client application WAIT with no actual host OFFER is not a physical
 * exchange rendezvous. Suppressing unrelated owner timers in that state
 * makes its game time fall behind even though serial IRQs remain correct. */
static void testCooperativeIdleWaitProgress(bool enabled, bool sendAndWait) {
	beginPrearmed(enabled ? "Cooperative idle client WAIT preserves unrelated frame/timer progress" :
		"Legacy idle client WAIT establishes unrelated frame/timer progress baseline");
	synchronizePair();
	const uint32_t settings = 0x0001;
	EQ(commandOne(1, 0x17, &settings, 1, NULL), 0);
	if (enabled) enableCooperativeTestUsers(true);
	struct CooperativeIdleTimer timers[2] = {0};
	for (unsigned id = 0; id < 2; ++id) {
		timers[id].event.context = &timers[id];
		timers[id].event.callback = cooperativeIdleTimerEvent;
		timers[id].event.name = "Independent frame-rate timer";
		mTimingSchedule(&machines[id]->gba.timing, &timers[id].event, FRAME_CYCLES);
	}
	const uint32_t before[2] = {mTimingCurrentTime(&machines[0]->gba.timing), mTimingCurrentTime(&machines[1]->gba.timing)};
	struct CooperativeIdleClient client = {0};
	client.command = sendAndWait ? 0x25 : 0x27;
	const unsigned totalCycles = CPU_HZ * 3 / 10;
	for (unsigned elapsed = 0; elapsed < totalCycles; elapsed += 128) {
		if (enabled) GBASIORFUCoordinatorService(&coordinator, UINT64_C(1000000) + (uint64_t) elapsed * 1000000 / CPU_HZ);
		advanceOne(0, 128);
		if (!enabled || !cooperativeUsers[1].asleep) {
			serviceCooperativeIdleClient(&client);
			advanceOne(1, 128);
		}
	}
	const uint32_t elapsed[2] = {mTimingCurrentTime(&machines[0]->gba.timing) - before[0], mTimingCurrentTime(&machines[1]->gba.timing) - before[1]};
	printf("IDLE_WAIT enabled=%d command=%02X hostCycles=%u clientCycles=%u hostTimers=%u clientTimers=%u waits=%u timeouts=%u sleeps=%u wakes=%u\n",
		enabled, client.command, elapsed[0], elapsed[1], timers[0].firings, timers[1].firings, client.waits, client.timeouts,
		enabled ? cooperativeUsers[1].sleeps : 0, enabled ? cooperativeUsers[1].wakes : 0);
	if (enabled) {
		EQ(cooperativeUsers[1].sleeps, 0);
		CHECK(!machines[1]->rfu.cooperativeAsleep);
	}
	CHECK(elapsed[1] >= elapsed[0]);
	CHECK(elapsed[1] - elapsed[0] < FRAME_CYCLES / 10);
	EQ(timers[0].firings, 18);
	EQ(timers[1].firings, timers[0].firings);
	EQ(client.waits, 18);
	EQ(client.timeouts, 17);
	EQ(machines[0]->rfu.radioNextTicket, 0);
	EQ(machines[0]->rfu.txAckMask, 0);
	EQ(machines[1]->rfu.rx.count, 0);
	EQ(machines[1]->rfu.clientTxPending, sendAndWait);
	for (unsigned id = 0; id < 2; ++id) mTimingDeschedule(&machines[id]->gba.timing, &timers[id].event);
	end();
}

static void testCooperativeScheduling(void) {
	testCooperativeOptions();
	testCooperativeTimeoutBudget();
	testCooperativeSpuriousWakeAndFullMailbox();
	testCooperativeLifecycle();
	testCooperativePeerRetirementBarrier();
	testCooperativeCoarseOwnerTurns();
	testCooperativeArmingAndOwnWork();
	testCooperativeMatchedClientWaitsForRelease();
	testCooperativeIdleWaitProgress(false, false);
	testCooperativeIdleWaitProgress(true, false);
	testCooperativeIdleWaitProgress(false, true);
	testCooperativeIdleWaitProgress(true, true);
}
