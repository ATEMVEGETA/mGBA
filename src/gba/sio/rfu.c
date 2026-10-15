/* Copyright (c) 2026 mGBA contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

/* Independent mGBA implementation of the RFU protocol. Command framing,
 * SI/SO sequencing and asynchronous replies were cross-checked against
 * David Guillen Fandos' gpSP RFU implementation and gba-link-connection.
 * This driver uses local per-core mailboxes, not gpSP/libretro globals. */
#include <mgba/internal/gba/sio/rfu.h>

#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/arm/arm.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define RFU_DRIVER_ID 0x52465500
#define RFU_FRAME_CYCLES 280896U
#define RFU_POLL_CYCLES 128U
#define RFU_INACTIVE_FRAMES 240U
#define RFU_CLOCK_INTERNAL 0x0001
#define RFU_CLOCK_FAST 0x0002
#define RFU_SI 0x0004
#define RFU_SO 0x0008
#define RFU_START 0x0080
#define RFU_COOPERATIVE_BUDGET_US 50000U

enum RFUMessageType {
	RFU_BROADCAST,
	RFU_CONNECT,
	RFU_CONNECT_ACK,
	RFU_CONNECT_NACK,
	RFU_DISCONNECT,
	RFU_HOST_DATA,
	RFU_CLIENT_DATA,
	RFU_KEEPALIVE,
	RFU_RADIO_OFFER,
	RFU_RADIO_READY,
	RFU_RADIO_RELEASE,
	RFU_RADIO_CANCEL,
	RFU_RADIO_CLAIM
};

static void _pollEvent(struct mTiming*, void*, uint32_t);
static void _maintenanceEvent(struct mTiming*, void*, uint32_t);
static void _clockEvent(struct mTiming*, void*, uint32_t);
static void _adapterEvent(struct mTiming*, void*, uint32_t);
static uint32_t _transfer(struct GBASIORFUDriver*, uint32_t);
static void _drain(struct GBASIORFUDriver*);
static void _radioService(struct GBASIORFUDriver*);
static void _radioCancel(struct GBASIORFUDriver*, bool);
static bool _radioOffer(struct GBASIORFUDriver*, struct GBASIORFUMessage*);
static void _radioAcceptOffer(struct GBASIORFUDriver*, const struct GBASIORFUMessage*);
static struct mTiming* _timing(struct GBASIORFUDriver*);
static bool _prearmedReconcile(struct GBASIORFUDriver*);
static bool _prearmedWithdraw(struct GBASIORFUDriver*);
static void _prearmedRetire(struct GBASIORFUDriver*);
static void _prearmedRetirePeer(struct GBASIORFUDriver*, unsigned);
static void _prearmedPublish(struct GBASIORFUDriver*);
static bool _prearmedPostOffer(struct GBASIORFUDriver*, struct GBASIORFUMessage*);
static bool _prearmedAcceptReady(struct GBASIORFUDriver*, const struct GBASIORFUMessage*);
static void _prearmedCancelTicket(struct GBASIORFUDriver*);
static int _prearmedPostRelease(struct GBASIORFUDriver*, struct GBASIORFUMessage*);
static void _prearmedStageTx(struct GBASIORFUDriver*);
static void _cooperativeWakeLocked(struct GBASIORFUDriver*);
static void _cooperativeRetire(struct GBASIORFUDriver*, bool);
static void _cooperativeTryPark(struct GBASIORFUDriver*);

static uint64_t _logWallMicros(void) {
#ifdef _WIN32
	LARGE_INTEGER counter, frequency;
	if (QueryPerformanceCounter(&counter) && QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0) {
		return (uint64_t) (counter.QuadPart / frequency.QuadPart) * 1000000 +
			(uint64_t) (counter.QuadPart % frequency.QuadPart) * 1000000 / frequency.QuadPart;
	}
#elif defined(CLOCK_MONOTONIC)
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
		return (uint64_t) now.tv_sec * 1000000 + now.tv_nsec / 1000;
	}
#endif
	return 0; /* Explicitly unavailable on platforms without this clock. */
}

static bool _logCommandUseful(uint8_t command) {
	switch (command) {
	case 0x11: case 0x12: case 0x13: case 0x14: case 0x1A:
	case 0x1D: case 0x1E: case 0x20: case 0x21: case 0x26:
		return false; /* Repeated discovery/status polling, not a transition. */
	default:
		return true;
	}
}

/* Diagnostic FNV-1a over the exact bounded packet snapshot, not mutable TX.
 * Zero-byte packets have the standard empty-input basis. Invalid sizes are
 * explicitly unhashable; callers retain the original bytes field in the log. */
static uint32_t _packetFingerprint(const uint8_t* data, unsigned bytes) {
	if (bytes > GBA_RFU_MAX_BYTES) return 0;
	uint32_t fingerprint = 2166136261U;
	for (unsigned i = 0; i < bytes; ++i) fingerprint = (fingerprint ^ data[i]) * 16777619U;
	return fingerprint;
}

static bool _packetLogEnabled(const struct GBASIORFUDriver* rfu) {
	return rfu->logBuffer && rfu->logPath[0];
}

static void _log(struct GBASIORFUDriver* rfu, const char* format, ...) {
	if (!rfu->logPath[0] || !rfu->logBuffer || strncmp(format, "event ", 6) != 0 ||
	    strncmp(format, "event external", 14) == 0 ||
	    ((!strncmp(format, "event command ", 14) || !strncmp(format, "event response ", 15)) &&
	     !_logCommandUseful(rfu->command))) {
		return;
	}
	char line[640];
	struct mTiming* timing = _timing(rfu);
	const uint32_t emu = timing ? mTimingCurrentTime(timing) : 0;
	const int64_t relative = rfu->radioClock + (timing ? (uint32_t) (emu - rfu->radioLastTime) : 0);
	int prefix = snprintf(line, sizeof(line),
		"P%d seq=%llu wallUs=%llu emu=%u session=%u role=%u state=%u cmd=%02X ticket=%llu relative=%lld pending=%d retry=%u wait=%u ",
		rfu->playerId, (unsigned long long) ++rfu->logSequence, (unsigned long long) _logWallMicros(),
		emu, rfu->radioSession, rfu->role, rfu->state, rfu->command, (unsigned long long) rfu->radioTicket,
		(long long) relative, rfu->radioPending, rfu->retryCycles, rfu->waitCycles);
	if (prefix < 0 || (size_t) prefix >= sizeof(line) - 1) {
		++rfu->logDropped;
		return;
	}
	va_list args;
	va_start(args, format);
	int body = vsnprintf(line + prefix, sizeof(line) - prefix, format, args);
	va_end(args);
	if (body < 0) {
		++rfu->logDropped;
		return;
	}
	size_t length = (size_t) prefix + body;
	if (length >= sizeof(line) - 1) {
		length = sizeof(line) - 2;
	}
	line[length++] = '\n';
	while (GBA_RFU_LOG_CAPACITY - rfu->logSize < length) {
		char discarded;
		do {
			discarded = rfu->logBuffer[rfu->logHead];
			rfu->logHead = (rfu->logHead + 1) % GBA_RFU_LOG_CAPACITY;
			--rfu->logSize;
		} while (rfu->logSize && discarded != '\n');
		++rfu->logDropped;
	}
	const size_t tail = (rfu->logHead + rfu->logSize) % GBA_RFU_LOG_CAPACITY;
	const size_t first = length < GBA_RFU_LOG_CAPACITY - tail ? length : GBA_RFU_LOG_CAPACITY - tail;
	memcpy(rfu->logBuffer + tail, line, first);
	memcpy(rfu->logBuffer, line + first, length - first);
	rfu->logSize += length;
}

static void _logSendSnapshot(struct GBASIORFUDriver* rfu, const struct GBASIORFUMessage* message) {
	if (!_packetLogEnabled(rfu)) return;
	_log(rfu, "event send role=%u bytes=%u epoch=%u revision=%llu fingerprint=%08X targets=%02X pending=%d",
		rfu->role, message->bytes, message->txEpoch, (unsigned long long) rfu->clientTxRevision,
		_packetFingerprint(message->data, message->bytes), rfu->txTargetMask, rfu->clientTxPending);
}

static struct mTiming* _timing(struct GBASIORFUDriver* rfu) {
	return rfu->d.p ? &rfu->d.p->p->timing : NULL;
}

/* All scheduler hooks linearize under the mailbox mutex. There is no delayed
 * callback holding a driver/user pointer after detach or a generation change. */
static void _cooperativeWakeLocked(struct GBASIORFUDriver* rfu) {
	if (!rfu->cooperativeAsleep) return;
	rfu->cooperativeAsleep = false;
	if (rfu->cooperativeUser && rfu->cooperativeUser->wake) {
		rfu->cooperativeUser->wake(rfu->cooperativeUser);
	}
}

static void _cooperativeRetireLocked(struct GBASIORFUDriver* rfu) {
	_cooperativeWakeLocked(rfu);
	if (rfu->cooperativeGeneration != UINT64_MAX) ++rfu->cooperativeGeneration;
	rfu->cooperativeWaitGeneration = 0;
	rfu->cooperativeWaitTicket = 0;
	rfu->cooperativeDeadlineUs = 0;
	rfu->cooperativeBypass = rfu->cooperativeGeneration == UINT64_MAX;
	rfu->cooperativePeerRetired = false;
}

static void _cooperativeRetirePeersLocked(struct GBASIORFUDriver* rfu) {
	struct GBASIORFUCoordinator* coordinator = rfu->coordinator;
	for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
		struct GBASIORFUDriver* target = coordinator->players[peer];
		if (!target || target == rfu) continue;
		/* An old peer-dependent WAIT cannot re-park after a lifecycle
		 * change. Its owner must drain the terminal event or time out. */
		target->cooperativeBypass = true;
		target->cooperativePeerRetired = true;
		target->cooperativeDeadlineUs = 0;
		_cooperativeWakeLocked(target);
	}
}

static void _cooperativeRetire(struct GBASIORFUDriver* rfu, bool wakePeers) {
	struct GBASIORFUCoordinator* coordinator = rfu->coordinator;
	if (coordinator) MutexLock(&coordinator->mutex);
	_cooperativeRetireLocked(rfu);
	if (coordinator && wakePeers) _cooperativeRetirePeersLocked(rfu);
	if (coordinator) MutexUnlock(&coordinator->mutex);
}

static void _cancelAdapter(struct GBASIORFUDriver* rfu) {
	struct mTiming* timing = _timing(rfu);
	if (timing) {
		mTimingDeschedule(timing, &rfu->adapterEvent);
	}
	rfu->adapterPending = false;
}

static void _schedulePoll(struct GBASIORFUDriver* rfu) {
	struct mTiming* timing = _timing(rfu);
	if (timing && !mTimingIsScheduled(timing, &rfu->pollEvent)) {
		mTimingSchedule(timing, &rfu->pollEvent, RFU_POLL_CYCLES);
	}
}

static uint16_t _newDevice(struct GBASIORFUDriver* rfu) {
	++rfu->generation;
	return 0x6100 | ((rfu->playerId < 0 ? 0 : rfu->playerId) << 6) |
		((rfu->generation % 63) + 1);
}

static bool _replacePacket(struct GBASIORFUFifo* fifo, const uint8_t* data, uint8_t bytes) {
	if (!bytes || bytes > GBA_RFU_MAX_BYTES) {
		return false;
	}
	/* The hardware holds one unread packet per peer, not a history of
	 * packets. A newer RF exchange replaces data the console has not read. */
	fifo->head = 0;
	fifo->packets[0].bytes = bytes;
	memcpy(fifo->packets[0].data, data, bytes);
	fifo->count = 1;
	return true;
}

static bool _popPacket(struct GBASIORFUFifo* fifo, struct GBASIORFUPacket* packet) {
	if (!fifo->count) {
		return false;
	}
	*packet = fifo->packets[fifo->head];
	fifo->head = (fifo->head + 1) % GBA_RFU_PACKET_DEPTH;
	--fifo->count;
	return true;
}

static bool _post(struct GBASIORFUDriver* rfu, unsigned peer, struct GBASIORFUMessage* message) {
	struct GBASIORFUCoordinator* coordinator = rfu->coordinator;
	if (!coordinator || peer >= MAX_GBAS) {
		return false;
	}
	message->source = rfu->playerId;
	message->sourceDevice = rfu->deviceId;
	message->sourceSession = rfu->radioSession;
	bool posted = false;
	MutexLock(&coordinator->mutex);
	struct GBASIORFUDriver* target = coordinator->players[peer];
	if (target && target != rfu && target->incomingCount < GBA_RFU_MESSAGE_DEPTH) {
		unsigned tail = (target->incomingHead + target->incomingCount) % GBA_RFU_MESSAGE_DEPTH;
		target->incoming[tail] = *message;
		++target->incomingCount;
		_cooperativeWakeLocked(target);
		posted = true;
	}
	MutexUnlock(&coordinator->mutex);
	if (!posted && message->type != RFU_BROADCAST && message->type != RFU_KEEPALIVE) {
		_log(rfu, "event messageDrop type=%u peer=%u", message->type, peer);
	}
	return posted;
}

static int64_t _radioClock(struct GBASIORFUDriver* rfu) {
	struct mTiming* timing = _timing(rfu);
	if (timing) {
		uint32_t now = mTimingCurrentTime(timing);
		rfu->radioClock += (uint32_t) (now - rfu->radioLastTime);
		rfu->radioLastTime = now;
	}
	return rfu->radioClock;
}

static void _cooperativeTryPark(struct GBASIORFUDriver* rfu) {
	/* The hook pointer is configured only with this owner stopped. */
	if (!rfu->cooperativeUser) return;
	/* This predicate is owner-only. In particular, alignment and physical
	 * serial edges still consume their original emulated cycles. */
	struct GBASIORFUCoordinator* coordinator = rfu->coordinator;
	struct mTiming* timing = _timing(rfu);
	if (!coordinator || !timing || !rfu->synchronize || rfu->state != GBA_RFU_WAIT ||
	    rfu->mode != GBA_SIO_NORMAL_32 || rfu->internalPending || rfu->adapterPending ||
	    (rfu->d.p->siocnt & (RFU_CLOCK_INTERNAL | RFU_SI | RFU_SO)) ||
	    !(rfu->d.p->siocnt & RFU_START) || rfu->radioNotice || rfu->radioTimedOut ||
	    mTimingIsScheduled(timing, &rfu->clockEvent) || mTimingIsScheduled(timing, &rfu->adapterEvent) ||
	    mTimingIsScheduled(timing, &rfu->d.p->completeEvent)) return;
	unsigned peer = MAX_GBAS;
	uint32_t peerSession = 0;
	if (rfu->role == GBA_RFU_HOST && rfu->radioPending && rfu->radioOfferPosted && !rfu->radioMatched &&
	    rfu->radioOffer.slot < 4 && rfu->clients[rfu->radioOffer.slot].active) {
		peer = rfu->clients[rfu->radioOffer.slot].peer;
		peerSession = rfu->clients[rfu->radioOffer.slot].radioSession;
	} else if (rfu->role == GBA_RFU_CLIENT && rfu->radioPending && rfu->radioMatched &&
	           !rfu->radioReleased && _radioClock(rfu) >= rfu->radioDue) {
		/* An unoffered client WAIT is an adapter subscription, not proof
		 * that the peer owes this CPU a turn. Keep its independent game
		 * timers running until an actual exchange is matched. Otherwise
		 * each ordinary receive wait can freeze a client frame while the
		 * host continues at normal speed. */
		peer = rfu->hostPeer;
		peerSession = rfu->hostRadioSession;
	}
	if (peer >= MAX_GBAS) return;
	MutexLock(&coordinator->mutex);
	struct GBASIORFUDriver* target = coordinator->players[peer];
	struct mLockstepUser* user = rfu->cooperativeUser;
	if (!coordinator->cooperativeNowUs || rfu->cooperativePeerRetired ||
	    rfu->playerId < 0 || rfu->playerId >= MAX_GBAS ||
	    coordinator->players[rfu->playerId] != rfu || !user || !user->sleep || !user->wake ||
	    !target || target == rfu || !peerSession || coordinator->cooperativeSessions[peer] != peerSession ||
	    !target->cooperativeUser ||
	    !target->cooperativeUser->sleep || !target->cooperativeUser->wake || rfu->incomingCount) {
		MutexUnlock(&coordinator->mutex);
		return;
	}
	const uint64_t ticket = rfu->radioPending ? rfu->radioTicket : 0;
	if (rfu->cooperativeWaitGeneration != rfu->cooperativeGeneration || rfu->cooperativeWaitTicket != ticket) {
		rfu->cooperativeWaitGeneration = rfu->cooperativeGeneration;
		rfu->cooperativeWaitTicket = ticket;
		rfu->cooperativeDeadlineUs = 0;
		rfu->cooperativeBypass = rfu->cooperativeGeneration == UINT64_MAX;
	}
	if (rfu->cooperativeDeadlineUs && coordinator->cooperativeNowUs >= rfu->cooperativeDeadlineUs) {
		rfu->cooperativeBypass = true;
		_cooperativeWakeLocked(rfu);
	}
	if (!rfu->cooperativeBypass && !rfu->cooperativeAsleep) {
		if (target->cooperativeAsleep) {
			/* At least one core must run to produce the missing mailbox
			 * transition. Never put both ends of an empty rendezvous to sleep. */
			_cooperativeWakeLocked(target);
		} else {
			if (!rfu->cooperativeDeadlineUs) {
				const uint64_t now = coordinator->cooperativeNowUs;
				rfu->cooperativeDeadlineUs = now > UINT64_MAX - RFU_COOPERATIVE_BUDGET_US ? UINT64_MAX :
					now + RFU_COOPERATIVE_BUDGET_US;
			}
			rfu->cooperativeAsleep = true;
			user->sleep(user);
			/* Only this owner touches its CPU. Wake paths touch thread hooks
			 * alone, just as the cable lockstep user does. */
			if (rfu->d.p->p->cpu) rfu->d.p->p->cpu->nextEvent = 0;
			GBAInterrupt(rfu->d.p->p);
		}
	}
	MutexUnlock(&coordinator->mutex);
}

static void _radioNewSession(struct GBASIORFUDriver* rfu) {
	if (rfu->coordinator) {
		MutexLock(&rfu->coordinator->mutex);
		if (!++rfu->coordinator->nextRadioSession) {
			++rfu->coordinator->nextRadioSession;
		}
		rfu->radioSession = rfu->coordinator->nextRadioSession;
		if (rfu->playerId >= 0 && rfu->playerId < MAX_GBAS &&
		    rfu->coordinator->players[rfu->playerId] == rfu) {
			rfu->coordinator->cooperativeSessions[rfu->playerId] = rfu->radioSession;
		}
		MutexUnlock(&rfu->coordinator->mutex);
	} else if (!++rfu->radioSession) {
		++rfu->radioSession;
	}
	/* Tickets from a different host session have their own sequence. */
	rfu->radioLastTicket = 0;
}

static uint32_t _radioLag(int64_t peer, int64_t own) {
	const int64_t limit = 240 * RFU_FRAME_CYCLES;
	if (peer <= own) {
		return 0;
	}
	if (own <= INT64_MAX - limit && peer > own + limit) {
		return limit;
	}
	return peer - own;
}

static bool _prearmedIdentity(const struct GBASIORFUDriver* rfu, const struct GBASIORFUMessage* ready, unsigned slot) {
	return slot < 4 && ready->source == rfu->clients[slot].peer && ready->slot == slot && ready->bytes <= 16 &&
		ready->sourceDevice == rfu->clients[slot].device && ready->sourceSession == rfu->clients[slot].radioSession &&
		ready->destinationDevice == rfu->deviceId && ready->targetSession == rfu->radioSession;
}

/* The claim record remains immutable until both owners have reconciled and
 * RELEASE/CANCEL is decided. No peer adapter/CPU/register fields are read. */
static bool _prearmedReconcile(struct GBASIORFUDriver* rfu) {
	if (!rfu->prearmed || !rfu->synchronize || rfu->role != GBA_RFU_CLIENT ||
	    !rfu->coordinator || rfu->playerId < 0) {
		return false;
	}
	const uint64_t generation = rfu->prearmedGeneration ? rfu->prearmedGeneration :
		(rfu->radioPending ? rfu->radioOffer.readinessGeneration : 0);
	if (!generation) return false;
	struct GBASIORFUMessage ready, offer;
	bool found = false, cancelled = false, alreadyAdopted = false;
	MutexLock(&rfu->coordinator->mutex);
	struct GBASIORFUPrearmed* capture = &rfu->coordinator->prearmed[rfu->playerId];
	if (capture->claimed && capture->generation == generation && (!capture->adopted || capture->cancelled)) {
		ready = capture->ready;
		offer = capture->offer;
		cancelled = capture->cancelled;
		alreadyAdopted = capture->adopted;
		capture->adopted = true;
		if ((capture->accepted && capture->released) || cancelled) capture->claimed = false;
		found = true;
	}
	MutexUnlock(&rfu->coordinator->mutex);
	if (!found) return false;
	if (alreadyAdopted) {
		if (cancelled && rfu->radioPending && rfu->radioTicket == offer.radioTicket &&
		    rfu->radioOffer.readinessGeneration == generation) {
			/* The peer mailbox may be full, so a failed CANCEL post must
			 * still terminate this owner's matching active window. */
			_radioCancel(rfu, false);
			rfu->radioTimedOut = true;
			_log(rfu, "event prearmedWithdraw generation=%llu cancelled=1 adopted=1", (unsigned long long) generation);
		}
		return false;
	}
	rfu->prearmedGeneration = 0;
	const bool fresh = generation > rfu->prearmedLastGeneration;
	if (fresh) rfu->prearmedLastGeneration = generation;
	const bool valid = fresh && ready.source == rfu->playerId && ready.sourceDevice == rfu->deviceId &&
		ready.sourceSession == rfu->radioSession && ready.destinationDevice == rfu->hostDevice &&
		ready.targetSession == rfu->hostRadioSession && ready.slot == rfu->clientNumber && ready.bytes <= 16 &&
		offer.source == rfu->hostPeer && offer.sourceDevice == rfu->hostDevice &&
		offer.sourceSession == rfu->hostRadioSession && offer.targetSession == rfu->radioSession &&
		offer.destinationDevice == rfu->deviceId && offer.slot == rfu->clientNumber && offer.bytes <= 90 &&
		offer.radioTicket > rfu->radioLastTicket && ready.txRevision == rfu->clientTxRevision && !rfu->radioPending;
	if (!valid || cancelled) {
		if (!valid) {
			MutexLock(&rfu->coordinator->mutex);
			capture = &rfu->coordinator->prearmed[rfu->playerId];
			if (capture->generation == generation) {
				if (!capture->released) capture->cancelled = true;
				capture->claimed = false;
			}
			MutexUnlock(&rfu->coordinator->mutex);
		}
		if (valid && cancelled) {
			rfu->radioLastTicket = offer.radioTicket;
			rfu->radioReady = false;
			if (rfu->state == GBA_RFU_WAIT) rfu->radioTimedOut = true;
		}
		_log(rfu, "event prearmedWithdraw generation=%llu cancelled=%d valid=%d", (unsigned long long) generation, cancelled, valid);
		return false;
	}
	/* Adoption is owner-local. Publication time is not treated as a match
	 * origin, and a newer TX is never consumed by an older generation. */
	const int64_t now = _radioClock(rfu);
	rfu->radioLastTicket = offer.radioTicket;
	rfu->radioTicket = offer.radioTicket;
	rfu->radioOffer = offer;
	rfu->radioOffer.type = RFU_RADIO_OFFER;
	rfu->radioReply = ready;
	rfu->radioReplyRevision = ready.txRevision;
	rfu->radioPending = true;
	rfu->radioMatched = true;
	rfu->radioReleased = false;
	rfu->radioReady = false;
	rfu->radioNotice = false;
	rfu->clientTxPending = false;
	rfu->radioMatchedAt = now;
	rfu->radioDue = now + _radioLag(offer.relativeCycles, now);
	rfu->radioDeadline = rfu->timeoutFrames ? now + rfu->timeoutFrames * RFU_FRAME_CYCLES : 0;
	_schedulePoll(rfu);
	if (_packetLogEnabled(rfu)) {
		_log(rfu, "event prearmedAdopt generation=%llu ticket=%llu epoch=%u bytes=%u revision=%llu fingerprint=%08X hostBytes=%u hostFingerprint=%08X",
			(unsigned long long) generation, (unsigned long long) offer.radioTicket, offer.txEpoch, ready.bytes,
			(unsigned long long) ready.txRevision, _packetFingerprint(ready.data, ready.bytes), offer.bytes,
			_packetFingerprint(offer.data, offer.bytes));
	}
	return true;
}

/* Return true only when this call adopted a fresh committed CLAIM. Callers
 * ending a console WAIT can distinguish it from an already pending ticket. */
static bool _prearmedWithdraw(struct GBASIORFUDriver* rfu) {
	if (!rfu->coordinator || rfu->playerId < 0 || !rfu->prearmedGeneration) return false;
	if (!rfu->prearmed || !rfu->synchronize || rfu->role != GBA_RFU_CLIENT) {
		_prearmedRetire(rfu);
		return false;
	}
	/* If a claim won the mutex first, reconcile it before TX mutation or
	 * ending the wait. Otherwise withdrawal wins and prevents that claim. */
	for (unsigned attempt = 0; attempt < 2; ++attempt) {
		MutexLock(&rfu->coordinator->mutex);
		struct GBASIORFUPrearmed* capture = &rfu->coordinator->prearmed[rfu->playerId];
		const bool committed = capture->claimed && !capture->adopted && capture->generation == rfu->prearmedGeneration;
		if (!committed) {
			const uint64_t generation = rfu->prearmedGeneration;
			if (capture->generation == generation) capture->published = false;
			rfu->prearmedGeneration = 0;
			if (generation > rfu->prearmedLastGeneration) rfu->prearmedLastGeneration = generation;
			MutexUnlock(&rfu->coordinator->mutex);
			_log(rfu, "event prearmedWithdraw generation=%llu cancelled=0 valid=1", (unsigned long long) generation);
			return false;
		}
		MutexUnlock(&rfu->coordinator->mutex);
		const bool adopted = _prearmedReconcile(rfu);
		if (!rfu->prearmedGeneration) return adopted;
	}
	_prearmedRetire(rfu); /* Bound this owner turn even if registration changed. */
	return false;
}

static void _prearmedRetire(struct GBASIORFUDriver* rfu) {
	if (!rfu->coordinator || rfu->playerId < 0) return;
	const uint64_t generation = rfu->prearmedGeneration;
	MutexLock(&rfu->coordinator->mutex);
	for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
		struct GBASIORFUPrearmed* capture = &rfu->coordinator->prearmed[peer];
		if (!capture->generation) continue;
		if (capture->ready.source == rfu->playerId && capture->ready.sourceSession == rfu->radioSession) {
			capture->published = false;
			capture->cancelled = true;
			capture->claimed = false;
		} else if (capture->ready.destinationDevice == rfu->deviceId && capture->ready.targetSession == rfu->radioSession) {
			capture->published = false;
			if (capture->claimed && !capture->released) capture->cancelled = true;
		}
		if (capture->cancelled && rfu->coordinator->players[peer]) {
			_cooperativeWakeLocked(rfu->coordinator->players[peer]);
		}
	}
	rfu->prearmedGeneration = 0;
	if (generation > rfu->prearmedLastGeneration) rfu->prearmedLastGeneration = generation;
	MutexUnlock(&rfu->coordinator->mutex);
	if (generation) _log(rfu, "event prearmedWithdraw generation=%llu terminal=1", (unsigned long long) generation);
}

static void _prearmedPublish(struct GBASIORFUDriver* rfu) {
	if (!rfu->prearmed || !rfu->synchronize || rfu->role != GBA_RFU_CLIENT || !rfu->radioReady ||
	    rfu->radioPending || rfu->radioQueued || rfu->prearmedGeneration || !rfu->coordinator || rfu->playerId < 0) return;
	struct GBASIORFUMessage ready = {0};
	ready.type = RFU_RADIO_READY;
	ready.source = rfu->playerId;
	ready.sourceDevice = rfu->deviceId;
	ready.sourceSession = rfu->radioSession;
	ready.destinationDevice = rfu->hostDevice;
	ready.targetSession = rfu->hostRadioSession;
	ready.slot = rfu->clientNumber;
	ready.relativeCycles = rfu->radioArrival;
	ready.txRevision = rfu->clientTxRevision;
	if (rfu->clientTxPending) {
		ready.bytes = rfu->tx.bytes;
		memcpy(ready.data, rfu->tx.data, ready.bytes);
	}
	MutexLock(&rfu->coordinator->mutex);
	struct GBASIORFUPrearmed* capture = &rfu->coordinator->prearmed[rfu->playerId];
	if (!capture->claimed && rfu->coordinator->nextReadinessGeneration != UINT64_MAX) {
		if (rfu->coordinator->nextReadinessGeneration < rfu->prearmedLastGeneration) {
			rfu->coordinator->nextReadinessGeneration = rfu->prearmedLastGeneration;
		}
		if (rfu->coordinator->nextReadinessGeneration == UINT64_MAX) {
			MutexUnlock(&rfu->coordinator->mutex);
			return; /* Never wrap and reuse a readiness generation. */
		}
		++rfu->coordinator->nextReadinessGeneration;
		memset(capture, 0, sizeof(*capture));
		capture->generation = rfu->coordinator->nextReadinessGeneration;
		ready.readinessGeneration = capture->generation;
		capture->ready = ready;
		capture->published = true;
		rfu->prearmedGeneration = capture->generation;
	}
	MutexUnlock(&rfu->coordinator->mutex);
	if (rfu->prearmedGeneration && _packetLogEnabled(rfu)) {
		_log(rfu, "event prearmedPublish generation=%llu bytes=%u revision=%llu fingerprint=%08X",
			(unsigned long long) rfu->prearmedGeneration, ready.bytes, (unsigned long long) ready.txRevision,
			_packetFingerprint(ready.data, ready.bytes));
	}
}

static void _prearmedRetirePeer(struct GBASIORFUDriver* rfu, unsigned slot) {
	if (!rfu->prearmed || !rfu->coordinator || slot >= 4 || rfu->clients[slot].peer >= MAX_GBAS) return;
	MutexLock(&rfu->coordinator->mutex);
	struct GBASIORFUPrearmed* capture = &rfu->coordinator->prearmed[rfu->clients[slot].peer];
	if (capture->generation && _prearmedIdentity(rfu, &capture->ready, slot)) {
		capture->published = false;
		if (capture->claimed && !capture->released) capture->cancelled = true;
		struct GBASIORFUDriver* target = rfu->coordinator->players[rfu->clients[slot].peer];
		if (target) _cooperativeWakeLocked(target);
	}
	MutexUnlock(&rfu->coordinator->mutex);
}

static void _enqueuePrearmedLocked(struct GBASIORFUDriver* target, const struct GBASIORFUMessage* message) {
	unsigned tail = (target->incomingHead + target->incomingCount) % GBA_RFU_MESSAGE_DEPTH;
	target->incoming[tail] = *message;
	++target->incomingCount;
	_cooperativeWakeLocked(target);
}

static bool _prearmedPostOffer(struct GBASIORFUDriver* rfu, struct GBASIORFUMessage* offer) {
	const unsigned peer = rfu->clients[offer->slot].peer;
	if (!rfu->coordinator || peer >= MAX_GBAS || rfu->playerId < 0) return false;
	offer->source = rfu->playerId;
	offer->sourceDevice = rfu->deviceId;
	offer->sourceSession = rfu->radioSession;
	bool posted = false, claimed = false, retired = false;
	uint64_t generation = 0;
	MutexLock(&rfu->coordinator->mutex);
	struct GBASIORFUDriver* target = rfu->coordinator->players[peer];
	struct GBASIORFUPrearmed* capture = &rfu->coordinator->prearmed[peer];
	if (target && target != rfu && target->incomingCount < GBA_RFU_MESSAGE_DEPTH) {
		const bool available = capture->published && !capture->claimed &&
			_prearmedIdentity(rfu, &capture->ready, offer->slot);
		if (available && rfu->incomingCount < GBA_RFU_MESSAGE_DEPTH) {
			generation = capture->generation;
			struct GBASIORFUMessage ready = capture->ready;
			ready.radioTicket = offer->radioTicket;
			ready.txEpoch = offer->txEpoch;
			offer->readinessGeneration = generation;
			struct GBASIORFUMessage claim = *offer;
			claim.type = RFU_RADIO_CLAIM;
			capture->ready = ready;
			capture->offer = claim;
			capture->published = false;
			capture->claimed = true;
			capture->adopted = false;
			capture->accepted = false;
			capture->released = false;
			capture->cancelled = false;
			_enqueuePrearmedLocked(rfu, &ready);
			_enqueuePrearmedLocked(target, &claim);
			posted = claimed = true;
		} else {
			/* A normal OFFER consumes this publication's eligibility for the
			 * fast path atomically with enqueue. It can only use legacy READY;
			 * a later ticket cannot steal the same old WAIT generation. */
			if (available) {
				generation = capture->generation;
				capture->published = false;
				retired = true;
			}
			_enqueuePrearmedLocked(target, offer);
			posted = true;
		}
	}
	MutexUnlock(&rfu->coordinator->mutex);
	if (claimed) _log(rfu, "event prearmedClaim generation=%llu ticket=%llu", (unsigned long long) generation,
		(unsigned long long) offer->radioTicket);
	if (retired) _log(rfu, "event prearmedWithdraw generation=%llu fallback=1", (unsigned long long) generation);
	if (!posted) _log(rfu, "event messageDrop type=%u peer=%u", offer->type, peer);
	return posted;
}

static bool _prearmedAcceptReady(struct GBASIORFUDriver* rfu, const struct GBASIORFUMessage* ready) {
	if (!ready->readinessGeneration) return true;
	if (!rfu->prearmed || !rfu->coordinator || ready->source >= MAX_GBAS) return false;
	bool valid = false;
	MutexLock(&rfu->coordinator->mutex);
	struct GBASIORFUPrearmed* capture = &rfu->coordinator->prearmed[ready->source];
	if (capture->claimed && !capture->cancelled && !capture->accepted &&
	    capture->generation == ready->readinessGeneration && capture->offer.radioTicket == ready->radioTicket &&
	    capture->offer.txEpoch == ready->txEpoch && _prearmedIdentity(rfu, &capture->ready, ready->slot) &&
	    ready->readinessGeneration == rfu->radioOffer.readinessGeneration &&
	    ready->bytes == capture->ready.bytes && ready->txRevision == capture->ready.txRevision &&
	    ready->relativeCycles == capture->ready.relativeCycles &&
	    memcmp(ready->data, capture->ready.data, ready->bytes) == 0) {
		capture->accepted = true;
		if (capture->adopted && capture->released) capture->claimed = false;
		valid = true;
	}
	MutexUnlock(&rfu->coordinator->mutex);
	return valid;
}

static void _prearmedCancelTicket(struct GBASIORFUDriver* rfu) {
	if (!rfu->prearmed || !rfu->radioPending || !rfu->coordinator ||
	    rfu->radioOffer.slot >= 4) return;
	const unsigned peer = rfu->role == GBA_RFU_CLIENT ? (unsigned) rfu->playerId : rfu->clients[rfu->radioOffer.slot].peer;
	if (peer >= MAX_GBAS) return;
	MutexLock(&rfu->coordinator->mutex);
	struct GBASIORFUPrearmed* capture = &rfu->coordinator->prearmed[peer];
	if (capture->generation && capture->generation == rfu->radioOffer.readinessGeneration &&
	    capture->offer.radioTicket == rfu->radioTicket &&
	    capture->offer.sourceSession == (rfu->role == GBA_RFU_CLIENT ? rfu->hostRadioSession : rfu->radioSession)) {
		if (!capture->released) capture->cancelled = true;
		if (rfu->role == GBA_RFU_CLIENT) capture->claimed = false;
		struct GBASIORFUDriver* target = rfu->coordinator->players[peer];
		if (target) _cooperativeWakeLocked(target);
	}
	MutexUnlock(&rfu->coordinator->mutex);
}

/* -1 means the readiness was withdrawn, 0 means mailbox backpressure, and
 * 1 commits RELEASE. Validate the escrow and enqueue under the same lock so
 * client retirement cannot race a successful receipt acknowledgement. */
static int _prearmedPostRelease(struct GBASIORFUDriver* rfu, struct GBASIORFUMessage* release) {
	if (!release->readinessGeneration) return _post(rfu, rfu->clients[release->slot].peer, release) ? 1 : 0;
	if (!rfu->prearmed || !rfu->coordinator || release->slot >= 4) return -1;
	const unsigned peer = rfu->clients[rfu->radioOffer.slot].peer;
	if (peer >= MAX_GBAS) return -1;
	release->source = rfu->playerId;
	release->sourceDevice = rfu->deviceId;
	release->sourceSession = rfu->radioSession;
	int result = -1;
	MutexLock(&rfu->coordinator->mutex);
	struct GBASIORFUPrearmed* capture = &rfu->coordinator->prearmed[peer];
	if (capture->claimed && !capture->cancelled && capture->accepted &&
	    capture->generation == release->readinessGeneration && capture->offer.radioTicket == release->radioTicket &&
	    capture->offer.sourceSession == rfu->radioSession) {
		result = 0;
		struct GBASIORFUDriver* target = rfu->coordinator->players[peer];
		if (target && target != rfu && target->incomingCount < GBA_RFU_MESSAGE_DEPTH) {
			_enqueuePrearmedLocked(target, release);
			capture->released = true;
			if (capture->adopted) capture->claimed = false;
			result = 1;
		}
	}
	MutexUnlock(&rfu->coordinator->mutex);
	return result;
}

static void _prearmedStageTx(struct GBASIORFUDriver* rfu) {
	if (!rfu->prearmed || !rfu->synchronize || rfu->role != GBA_RFU_CLIENT) return;
	_prearmedWithdraw(rfu);
	rfu->radioReady = false;
	if (!++rfu->clientTxRevision) ++rfu->clientTxRevision;
}

static void _radioCancel(struct GBASIORFUDriver* rfu, bool notify) {
	_prearmedWithdraw(rfu);
	_prearmedCancelTicket(rfu);
	if ((rfu->radioPending || rfu->radioReady) && _packetLogEnabled(rfu)) {
		const unsigned hostBytes = rfu->radioPending ? rfu->radioOffer.bytes : 0;
		const unsigned clientBytes = rfu->radioPending && rfu->radioMatched ? rfu->radioReply.bytes : 0;
		_log(rfu, "event radioCancel notify=%d matched=%d released=%d due=%lld deadline=%lld epoch=%u hostBytes=%u hostFingerprint=%08X clientBytes=%u clientFingerprint=%08X",
			notify, rfu->radioMatched, rfu->radioReleased, (long long) rfu->radioDue, (long long) rfu->radioDeadline,
			rfu->radioPending ? rfu->radioOffer.txEpoch : 0, hostBytes, _packetFingerprint(rfu->radioOffer.data, hostBytes),
			clientBytes, _packetFingerprint(rfu->radioReply.data, clientBytes));
	}
	if (rfu->radioPending && notify) {
		struct GBASIORFUMessage message = rfu->radioOffer;
		message.type = RFU_RADIO_CANCEL;
		message.bytes = 0;
		if (rfu->role == GBA_RFU_CLIENT) {
			message.destinationDevice = rfu->hostDevice;
			message.targetSession = rfu->hostRadioSession;
			_post(rfu, rfu->hostPeer, &message);
		} else if (message.slot < 4) {
			_post(rfu, rfu->clients[message.slot].peer, &message);
		}
	}
	if (rfu->role == GBA_RFU_CLIENT && rfu->radioPending && rfu->radioMatched &&
	    !rfu->clientTxPending && !rfu->radioReady && rfu->radioReply.bytes &&
	    (!rfu->prearmed || rfu->radioReplyRevision == rfu->clientTxRevision)) {
		/* A cancelled rendezvous did not exchange this snapshot. Retain it
		 * unless the console has already staged a newer (possibly zero) TX. */
		rfu->tx.bytes = rfu->radioReply.bytes;
		memcpy(rfu->tx.data, rfu->radioReply.data, rfu->tx.bytes);
		rfu->clientTxPending = true;
	}
	rfu->radioPending = false;
	rfu->radioOfferPosted = false;
	rfu->radioMatched = false;
	rfu->radioReleased = false;
	rfu->radioReady = false;
	rfu->radioNotice = false;
	rfu->radioTicket = 0;
	rfu->radioMatchedAt = 0;
	rfu->radioDeadline = 0;
	rfu->radioTimedOut = false;
}

static void _radioTryClient(struct GBASIORFUDriver* rfu) {
	if (!rfu->synchronize || rfu->role != GBA_RFU_CLIENT || !rfu->radioPending ||
	    !rfu->radioReady || rfu->radioMatched) {
		return;
	}
	_prearmedWithdraw(rfu);
	if (rfu->radioMatched || !rfu->radioReady) return;
	struct GBASIORFUMessage reply = {0};
	reply.type = RFU_RADIO_READY;
	reply.slot = rfu->clientNumber;
	reply.destinationDevice = rfu->hostDevice;
	reply.targetSession = rfu->hostRadioSession;
	reply.radioTicket = rfu->radioTicket;
	reply.txEpoch = rfu->radioOffer.txEpoch;
	reply.relativeCycles = rfu->radioArrival;
	reply.txRevision = rfu->clientTxRevision;
	if (rfu->clientTxPending) {
		reply.bytes = rfu->tx.bytes;
		memcpy(reply.data, rfu->tx.data, reply.bytes);
	}
	if (_post(rfu, rfu->hostPeer, &reply)) {
		rfu->radioReply = reply;
		rfu->radioReplyRevision = reply.txRevision;
		rfu->radioMatched = true;
		rfu->radioReady = false;
		rfu->clientTxPending = false;
		const int64_t now = _radioClock(rfu);
		rfu->radioMatchedAt = now;
		/* Owner time keeps advancing while awaiting a mailbox rendezvous.
		 * Align only the remaining peer lead, not the lead at WAIT entry
		 * plus time that has already elapsed while waiting for the offer. */
		const uint32_t lag = _radioLag(rfu->radioOffer.relativeCycles, now);
		rfu->radioDue = now + lag;
		if (_packetLogEnabled(rfu)) {
			_log(rfu, "event radioReady ticket=%llu epoch=%u bytes=%u fingerprint=%08X hostBytes=%u hostFingerprint=%08X lag=%u ownNow=%lld offerRelative=%lld due=%lld",
				(unsigned long long) reply.radioTicket, reply.txEpoch, reply.bytes, _packetFingerprint(reply.data, reply.bytes),
				rfu->radioOffer.bytes, _packetFingerprint(rfu->radioOffer.data, rfu->radioOffer.bytes), lag, (long long) now,
				(long long) rfu->radioOffer.relativeCycles, (long long) rfu->radioDue);
		}
	}
}

static void _radioReady(struct GBASIORFUDriver* rfu) {
	if (rfu->synchronize && rfu->role == GBA_RFU_CLIENT) {
		_prearmedWithdraw(rfu);
		rfu->radioArrival = _radioClock(rfu);
		rfu->radioReady = true;
		rfu->radioNotice = false;
		_radioTryClient(rfu);
		_prearmedPublish(rfu);
	}
}

/* This proof belongs to the console WAIT that actually completed. A later
 * unready radio ticket may already have been promoted, but cancelling that
 * ticket must not replace the completed WAIT's still-unclocked success.
 * Fresh readiness/TX keeps the existing wait/revision gates authoritative. */
static bool _radioCompletedClientWait(const struct GBASIORFUDriver* rfu) {
	return rfu->prearmed && rfu->synchronize && rfu->role == GBA_RFU_CLIENT &&
		rfu->state == GBA_RFU_WAIT && rfu->radioNotice && !rfu->radioReady &&
		!rfu->clientWaitNeedsExchange && !rfu->clientTxPending &&
		rfu->radioReplyRevision == rfu->clientTxRevision;
}

static void _radioService(struct GBASIORFUDriver* rfu) {
	if (!rfu->synchronize || !rfu->radioPending) {
		return;
	}
	int64_t now = _radioClock(rfu);
	if (rfu->radioDeadline && now >= rfu->radioDeadline) {
		_log(rfu, "event radioTimeout ticket=%llu", (unsigned long long) rfu->radioTicket);
		const bool completedWait = _radioCompletedClientWait(rfu);
		_radioCancel(rfu, true);
		rfu->radioQueued = false;
		rfu->radioNotice = completedWait;
		rfu->radioTimedOut = !completedWait;
		return;
	}
	if (rfu->role == GBA_RFU_CLIENT) {
		_radioTryClient(rfu);
		if (!rfu->radioReleased || !rfu->radioMatched || now < rfu->radioDue) {
			return;
		}
		if (rfu->radioOffer.bytes) {
			_replacePacket(&rfu->rx, rfu->radioOffer.data, rfu->radioOffer.bytes);
		}
		if (!rfu->clientTxPending && !rfu->radioReady &&
		    (!rfu->prearmed || rfu->radioReplyRevision == rfu->clientTxRevision)) {
			rfu->clientWaitNeedsExchange = false;
		}
	} else if (rfu->role == GBA_RFU_HOST) {
		if (!rfu->radioOfferPosted) {
			rfu->radioOfferPosted = rfu->prearmed ? _prearmedPostOffer(rfu, &rfu->radioOffer) :
				_post(rfu, rfu->clients[rfu->radioOffer.slot].peer, &rfu->radioOffer);
			if (!rfu->radioOfferPosted) {
				return;
			}
		}
		if (!rfu->radioMatched || now < rfu->radioDue) {
			return;
		}
		unsigned slot = rfu->radioOffer.slot;
		struct GBASIORFUMessage release = rfu->radioOffer;
		release.type = RFU_RADIO_RELEASE;
		const int released = _prearmedPostRelease(rfu, &release);
		if (released < 0) {
			_radioCancel(rfu, false);
			rfu->radioQueued = false;
			rfu->radioTimedOut = true;
			return;
		}
		if (!released) {
			return;
		}
		if (rfu->radioReply.bytes) {
			_replacePacket(&rfu->clients[slot].rx, rfu->radioReply.data, rfu->radioReply.bytes);
		}
		if (rfu->radioOffer.txEpoch == rfu->txEpoch) {
			rfu->txAckMask |= 1U << slot;
		}
		rfu->clients[slot].frames = 0;
	} else {
		return;
	}
	if (_packetLogEnabled(rfu)) {
		_log(rfu, "event radioComplete ticket=%llu epoch=%u generation=%llu relative=%lld hostBytes=%u hostFingerprint=%08X clientBytes=%u clientFingerprint=%08X currentEpoch=%u ack=%02X targets=%02X",
			(unsigned long long) rfu->radioTicket, rfu->radioOffer.txEpoch, (unsigned long long) rfu->radioOffer.readinessGeneration,
			(long long) now, rfu->radioOffer.bytes, _packetFingerprint(rfu->radioOffer.data, rfu->radioOffer.bytes),
			rfu->radioReply.bytes, _packetFingerprint(rfu->radioReply.data, rfu->radioReply.bytes), rfu->txEpoch,
			rfu->txAckMask, rfu->txTargetMask);
	}
	rfu->radioPending = false;
	rfu->radioNotice = true;
	/* Start the next epoch at this owner's READY match, not at each
	 * owner's differently delayed completion. Both CPUs keep running while
	 * aligning; erasing only the slower owner's elapsed interval would make
	 * the same peer lead recur in every following exchange. The separate
	 * physical serial word still takes 256 cycles. */
	const int64_t origin = rfu->radioMatchedAt + 256;
	rfu->radioClock = now - origin;
	if (rfu->radioReady) {
		/* The console may stage its next reply before the previous RELEASE.
		 * Translate that readiness timestamp into the newly rebased origin. */
		rfu->radioArrival -= origin;
	}
	if (rfu->radioQueued) {
		struct GBASIORFUMessage offer = rfu->radioQueuedOffer;
		rfu->radioQueued = false;
		if (rfu->role == GBA_RFU_HOST) {
			_radioOffer(rfu, &offer);
		} else {
			_radioAcceptOffer(rfu, &offer);
		}
	}
	_prearmedPublish(rfu);
}

static void _radioAcceptOffer(struct GBASIORFUDriver* rfu, const struct GBASIORFUMessage* message) {
	_prearmedWithdraw(rfu);
	if (rfu->radioPending) {
		if (message->radioTicket > rfu->radioTicket &&
		    (!rfu->radioQueued || message->radioTicket > rfu->radioQueuedOffer.radioTicket)) {
			rfu->radioQueuedOffer = *message;
			rfu->radioQueued = true;
		}
		return;
	}
	rfu->radioLastTicket = message->radioTicket;
	rfu->radioTicket = message->radioTicket;
	rfu->radioOffer = *message;
	rfu->radioPending = true;
	rfu->radioMatched = false;
	rfu->radioMatchedAt = 0;
	rfu->radioReleased = false;
	rfu->radioDeadline = rfu->timeoutFrames ? _radioClock(rfu) + rfu->timeoutFrames * RFU_FRAME_CYCLES : 0;
	_radioTryClient(rfu);
	_schedulePoll(rfu);
}

static bool _radioOffer(struct GBASIORFUDriver* rfu, struct GBASIORFUMessage* message) {
	unsigned count = 0;
	unsigned slot = 0;
	for (unsigned i = 0; i < 4; ++i) {
		if (rfu->clients[i].active) {
			++count;
			slot = i;
		}
	}
	if (!rfu->synchronize || count != 1) {
		return false;
	}
	if (rfu->radioPending) {
		/* Do not mutate an in-flight ticket. Plain SendData can continue
		 * clocking SPI; retain only its latest subsequent TX snapshot. */
		rfu->radioQueuedOffer = *message;
		rfu->radioQueued = true;
		return true;
	}
	_radioCancel(rfu, true);
	message->type = RFU_RADIO_OFFER;
	message->slot = slot;
	message->destinationDevice = rfu->clients[slot].device;
	message->targetSession = rfu->clients[slot].radioSession;
	message->radioTicket = ++rfu->radioNextTicket;
	if (!message->radioTicket) {
		message->radioTicket = ++rfu->radioNextTicket;
	}
	message->relativeCycles = _radioClock(rfu);
	rfu->radioArrival = message->relativeCycles;
	rfu->radioTicket = message->radioTicket;
	rfu->radioPending = true;
	rfu->radioDeadline = rfu->timeoutFrames ? rfu->radioClock + rfu->timeoutFrames * RFU_FRAME_CYCLES : 0;
	rfu->txTargetMask = 1U << slot;
	rfu->txAckMask &= rfu->txTargetMask;
	rfu->radioOfferPosted = rfu->prearmed ? _prearmedPostOffer(rfu, message) : _post(rfu, rfu->clients[slot].peer, message);
	rfu->radioOffer = *message;
	_schedulePoll(rfu);
	if (_packetLogEnabled(rfu)) {
		_log(rfu, "event radioOffer ticket=%llu epoch=%u bytes=%u fingerprint=%08X relative=%lld", (unsigned long long) message->radioTicket,
			message->txEpoch, message->bytes, _packetFingerprint(message->data, message->bytes), (long long) message->relativeCycles);
	}
	return true;
}

static void _broadcast(struct GBASIORFUDriver* rfu) {
	if (rfu->role != GBA_RFU_HOST || !rfu->accepting) {
		return;
	}
	struct GBASIORFUMessage message = {0};
	message.type = RFU_BROADCAST;
	message.bytes = sizeof(rfu->broadcast);
	memcpy(message.data, rfu->broadcast, sizeof(rfu->broadcast));
	for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
		if ((int) peer != rfu->playerId) {
			_post(rfu, peer, &message);
		}
	}
}

static void _disconnect(struct GBASIORFUDriver* rfu) {
	_cooperativeRetire(rfu, true);
	if (rfu->role != GBA_RFU_IDLE) {
		_log(rfu, "event disconnectLocal host=%04X hostSession=%u", rfu->hostDevice, rfu->hostRadioSession);
	}
	_prearmedRetire(rfu);
	_radioCancel(rfu, true);
	rfu->radioQueued = false;
	struct GBASIORFUMessage message = {0};
	message.type = RFU_DISCONNECT;
	if (rfu->role == GBA_RFU_HOST) {
		for (unsigned slot = 0; slot < 4; ++slot) {
			if (rfu->clients[slot].active) {
				message.slot = slot;
				message.destinationDevice = rfu->clients[slot].device;
				message.targetSession = rfu->clients[slot].radioSession;
				_post(rfu, rfu->clients[slot].peer, &message);
			}
		}
	} else if (rfu->role == GBA_RFU_CLIENT || rfu->role == GBA_RFU_CONNECTING) {
		message.slot = rfu->clientNumber;
		message.destinationDevice = rfu->hostDevice;
		message.targetSession = rfu->hostRadioSession;
		_post(rfu, rfu->hostPeer, &message);
	}
	rfu->role = GBA_RFU_IDLE;
	rfu->accepting = false;
	rfu->connectCycles = 0;
	rfu->hostDevice = 0;
	memset(rfu->clients, 0, sizeof(rfu->clients));
	memset(&rfu->rx, 0, sizeof(rfu->rx));
	memset(&rfu->tx, 0, sizeof(rfu->tx));
	rfu->clientTxPending = false;
	rfu->clientWaitNeedsExchange = false;
	rfu->txTargetMask = 0;
	rfu->txAckMask = 0;
	_radioNewSession(rfu);
}

static void _handleMessage(struct GBASIORFUDriver* rfu, const struct GBASIORFUMessage* message) {
	if (message->source >= MAX_GBAS || message->bytes > GBA_RFU_MAX_BYTES) {
		return;
	}
	struct GBASIORFUMessage reply = {0};
	switch (message->type) {
	case RFU_BROADCAST: {
		if (message->bytes != sizeof(rfu->broadcast)) {
			break;
		}
		struct GBASIORFUBroadcast* entry = &rfu->peerBroadcast[message->source];
		entry->valid = true;
		entry->ttl = 255;
		entry->device = message->sourceDevice;
		entry->radioSession = message->sourceSession;
		memcpy(entry->data, message->data, sizeof(entry->data));
		break;
	}
	case RFU_CONNECT: {
		reply.type = RFU_CONNECT_NACK;
		reply.targetSession = message->sourceSession;
		if (rfu->role == GBA_RFU_HOST && rfu->accepting && message->destinationDevice == rfu->deviceId &&
		    (!rfu->synchronize || message->targetSession == rfu->radioSession)) {
			unsigned slot;
			for (slot = 0; slot < 4; ++slot) {
				if (rfu->clients[slot].active && rfu->clients[slot].peer == message->source) {
					break;
				}
			}
			if (slot == 4) {
				for (slot = 0; slot < 4 && rfu->clients[slot].active; ++slot) {}
			}
			if (slot < 4) {
				struct GBASIORFUClient* client = &rfu->clients[slot];
				if (rfu->synchronize && client->active && client->radioSession != message->sourceSession &&
				    (int32_t) (message->sourceSession - client->radioSession) <= 0) {
					break;
				}
				if (!client->active || (rfu->synchronize && client->radioSession != message->sourceSession)) {
					memset(client, 0, sizeof(*client));
					client->active = true;
					client->peer = message->source;
					client->radioSession = message->sourceSession;
					if (rfu->synchronize) {
						/* Join establishes a radio-clock origin, independent of
						 * how long this emulator was running before its peer. */
						_radioCancel(rfu, false);
						rfu->radioQueued = false;
						rfu->radioClock = 0;
						rfu->radioLastTime = mTimingCurrentTime(_timing(rfu));
					}
					client->device = 0x6500 | ((rfu->playerId < 0 ? 0 : rfu->playerId) << 6) |
						((rfu->generation * 4 + slot + 1) & 0x3F);
				}
				reply.type = RFU_CONNECT_ACK;
				client->radioSession = message->sourceSession;
				reply.slot = slot;
				reply.destinationDevice = client->device;
				_log(rfu, "event accepted peer=%u slot=%u device=%04X", client->peer, slot, client->device);
			}
		}
		_log(rfu, "event connectReply type=%u accepting=%d requestedDevice=%04X targetSession=%u sourceSession=%u",
			reply.type, rfu->accepting, message->destinationDevice, message->targetSession, message->sourceSession);
		_post(rfu, message->source, &reply);
		break;
	}
	case RFU_CONNECT_ACK:
		if (rfu->role == GBA_RFU_CONNECTING && message->source == rfu->hostPeer &&
		    message->sourceDevice == rfu->hostDevice && message->slot < 4 &&
		    (!rfu->synchronize || (message->targetSession == rfu->radioSession &&
		                          message->sourceSession == rfu->hostRadioSession))) {
			_radioCancel(rfu, false);
			rfu->role = GBA_RFU_CLIENT;
			rfu->deviceId = message->destinationDevice;
			rfu->clientNumber = message->slot;
			rfu->connectCycles = 0;
			memset(&rfu->rx, 0, sizeof(rfu->rx));
			memset(&rfu->tx, 0, sizeof(rfu->tx));
			rfu->clientTxPending = false;
			rfu->clientWaitNeedsExchange = false;
			rfu->hostRadioSession = message->sourceSession;
			rfu->radioLastTicket = 0;
			rfu->radioQueued = false;
			rfu->radioClock = 0;
			rfu->radioLastTime = mTimingCurrentTime(_timing(rfu));
			_log(rfu, "event connected host=%04X slot=%u device=%04X", rfu->hostDevice, rfu->clientNumber, rfu->deviceId);
		}
		break;
	case RFU_CONNECT_NACK:
		if (rfu->role == GBA_RFU_CONNECTING && message->source == rfu->hostPeer &&
		    (!rfu->synchronize || (message->targetSession == rfu->radioSession &&
		                          message->sourceSession == rfu->hostRadioSession))) {
			_log(rfu, "event connectNack peer=%u peerSession=%u", message->source, message->sourceSession);
			rfu->role = GBA_RFU_IDLE;
			rfu->connectCycles = 0;
		}
		break;
	case RFU_DISCONNECT:
		_log(rfu, "event disconnectReceived source=%u sourceSession=%u targetSession=%u",
			message->source, message->sourceSession, message->targetSession);
		if (rfu->role == GBA_RFU_CLIENT && message->source == rfu->hostPeer &&
		    message->sourceDevice == rfu->hostDevice && message->destinationDevice == rfu->deviceId &&
		    (!rfu->synchronize || (message->sourceSession == rfu->hostRadioSession &&
		                          message->targetSession == rfu->radioSession))) {
			_prearmedRetire(rfu);
			_radioCancel(rfu, false);
			rfu->role = GBA_RFU_IDLE;
			memset(&rfu->rx, 0, sizeof(rfu->rx));
			memset(&rfu->tx, 0, sizeof(rfu->tx));
			rfu->clientTxPending = false;
			rfu->clientWaitNeedsExchange = false;
			rfu->radioQueued = false;
			_radioNewSession(rfu);
		} else if (rfu->role == GBA_RFU_HOST && message->slot < 4) {
			struct GBASIORFUClient* client = &rfu->clients[message->slot];
			if (client->active && client->peer == message->source && client->device == message->sourceDevice &&
			    (!rfu->synchronize || (message->sourceSession == client->radioSession &&
			                          message->targetSession == rfu->radioSession))) {
				_prearmedRetirePeer(rfu, message->slot);
				if (rfu->radioPending && rfu->radioOffer.slot == message->slot) {
					_radioCancel(rfu, false);
					rfu->radioQueued = false;
				}
				memset(client, 0, sizeof(*client));
				rfu->txTargetMask &= ~(1U << message->slot);
				rfu->txAckMask &= ~(1U << message->slot);
			}
		}
		break;
	case RFU_RADIO_CLAIM:
		if (rfu->prearmed && rfu->synchronize && rfu->role == GBA_RFU_CLIENT &&
		    message->source == rfu->hostPeer && message->sourceDevice == rfu->hostDevice &&
		    message->destinationDevice == rfu->deviceId && message->slot == rfu->clientNumber &&
		    message->sourceSession == rfu->hostRadioSession && message->targetSession == rfu->radioSession &&
		    message->readinessGeneration == rfu->prearmedGeneration) {
			_prearmedReconcile(rfu);
		}
		break;
	case RFU_RADIO_OFFER:
		if (rfu->synchronize && rfu->role == GBA_RFU_CLIENT &&
		    message->source == rfu->hostPeer && message->sourceDevice == rfu->hostDevice &&
		    message->destinationDevice == rfu->deviceId && message->slot == rfu->clientNumber &&
		    message->sourceSession == rfu->hostRadioSession && message->targetSession == rfu->radioSession &&
		    message->radioTicket > rfu->radioLastTicket && message->bytes <= 90) {
			if (rfu->radioPending) {
				/* RELEASE and the following OFFER can be in the same drain;
				 * retain the next ticket even if relative lag delays release. */
				if (!rfu->radioQueued || message->radioTicket > rfu->radioQueuedOffer.radioTicket) {
					rfu->radioQueuedOffer = *message;
					rfu->radioQueued = true;
				}
				break;
			}
			_radioAcceptOffer(rfu, message);
		}
		break;
	case RFU_RADIO_READY: {
		bool accepted = false;
		if (rfu->synchronize && rfu->role == GBA_RFU_HOST && rfu->radioPending && !rfu->radioMatched &&
		    message->slot == rfu->radioOffer.slot && message->slot < 4 && message->bytes <= 16 &&
		    message->destinationDevice == rfu->deviceId && message->targetSession == rfu->radioSession &&
		    message->radioTicket == rfu->radioTicket && message->txEpoch == rfu->radioOffer.txEpoch) {
			struct GBASIORFUClient* client = &rfu->clients[message->slot];
			if (client->active && client->peer == message->source && client->device == message->sourceDevice &&
			    client->radioSession == message->sourceSession && _prearmedAcceptReady(rfu, message)) {
				accepted = true;
				rfu->radioReply = *message;
				rfu->radioMatched = true;
				const int64_t now = _radioClock(rfu);
				rfu->radioMatchedAt = now;
				const uint32_t lag = _radioLag(message->relativeCycles, now);
				rfu->radioDue = now + lag;
				if (_packetLogEnabled(rfu)) {
					_log(rfu, "event radioMatched ticket=%llu epoch=%u clientBytes=%u clientFingerprint=%08X hostBytes=%u hostFingerprint=%08X peerRelative=%lld ownArrival=%lld ownNow=%lld lag=%u retryCycles=%u",
						(unsigned long long) message->radioTicket, message->txEpoch, message->bytes,
						_packetFingerprint(message->data, message->bytes), rfu->radioOffer.bytes,
						_packetFingerprint(rfu->radioOffer.data, rfu->radioOffer.bytes), (long long) message->relativeCycles,
						(long long) rfu->radioArrival, (long long) now, lag, rfu->retryCycles);
				}
			}
		}
		if (!accepted && rfu->synchronize) {
			unsigned reasons = 0;
			if (rfu->role != GBA_RFU_HOST) reasons |= 1;
			if (!rfu->radioPending) reasons |= 2;
			if (rfu->radioMatched) reasons |= 4;
			if (message->slot >= 4 || message->slot != rfu->radioOffer.slot) reasons |= 8;
			if (message->bytes > 16) reasons |= 16;
			if (message->destinationDevice != rfu->deviceId || message->targetSession != rfu->radioSession) reasons |= 32;
			if (message->radioTicket != rfu->radioTicket) reasons |= 64;
			if (message->txEpoch != rfu->radioOffer.txEpoch) reasons |= 128;
			if (message->slot < 4) {
				const struct GBASIORFUClient* client = &rfu->clients[message->slot];
				if (!client->active || client->peer != message->source || client->device != message->sourceDevice ||
				    client->radioSession != message->sourceSession) reasons |= 256;
			}
			/* Base identity passed, but the one-use claimed escrow did not. */
			if (!reasons && message->readinessGeneration) reasons |= 512;
			_log(rfu, "event radioReadyRejected receivedTicket=%llu receivedEpoch=%u expectedEpoch=%u reasons=%03X generation=%llu source=%u sourceSession=%u targetSession=%u",
				(unsigned long long) message->radioTicket, message->txEpoch, rfu->radioOffer.txEpoch, reasons,
				(unsigned long long) message->readinessGeneration,
				message->source, message->sourceSession, message->targetSession);
		}
		break;
	}
	case RFU_RADIO_RELEASE:
	case RFU_RADIO_CANCEL: {
		bool peerValid = rfu->synchronize &&
			message->targetSession == rfu->radioSession && message->destinationDevice == rfu->deviceId;
		if (rfu->role == GBA_RFU_CLIENT) {
			peerValid = peerValid && message->source == rfu->hostPeer && message->sourceDevice == rfu->hostDevice &&
				message->sourceSession == rfu->hostRadioSession && message->slot == rfu->clientNumber;
		} else if (rfu->role == GBA_RFU_HOST && message->slot < 4) {
			const struct GBASIORFUClient* client = &rfu->clients[message->slot];
			peerValid = peerValid && client->active && client->peer == message->source &&
				client->device == message->sourceDevice && client->radioSession == message->sourceSession;
		} else {
			peerValid = false;
		}
		if (peerValid && message->type == RFU_RADIO_CANCEL && rfu->role == GBA_RFU_CLIENT &&
		    rfu->radioQueued && message->radioTicket == rfu->radioQueuedOffer.radioTicket &&
		    message->txEpoch == rfu->radioQueuedOffer.txEpoch) {
			/* The host may cancel the next round while we are still aligning
			 * the previous RELEASE. Do not promote that cancelled offer later
			 * or allow a duplicate OFFER to revive it. */
			if (message->radioTicket > rfu->radioLastTicket) {
				rfu->radioLastTicket = message->radioTicket;
			}
			rfu->radioQueued = false;
			_log(rfu, "event radioCancelQueued ticket=%llu", (unsigned long long) message->radioTicket);
			break;
		}
		bool valid = peerValid && rfu->radioPending && message->radioTicket == rfu->radioTicket;
		if (valid && message->type == RFU_RADIO_RELEASE && rfu->role == GBA_RFU_CLIENT && rfu->radioMatched &&
		    message->txEpoch == rfu->radioOffer.txEpoch) {
			rfu->radioReleased = true;
		} else if (valid && message->type == RFU_RADIO_CANCEL) {
			_log(rfu, "event radioCancelReceived receivedTicket=%llu epoch=%u", (unsigned long long) message->radioTicket, message->txEpoch);
			const bool completedWait = _radioCompletedClientWait(rfu);
			_radioCancel(rfu, false);
			rfu->radioQueued = false;
			/* A completed WAIT owns its success even if this later ticket
			 * is cancelled before the console hands over the clock. Otherwise
			 * retained TX still requires fresh readiness after the timeout. */
			rfu->radioNotice = completedWait;
			rfu->radioTimedOut = !completedWait;
		} else if (!valid) {
			_log(rfu, "event radioControlIgnored type=%u receivedTicket=%llu peerValid=%d sourceSession=%u targetSession=%u",
				message->type, (unsigned long long) message->radioTicket, peerValid, message->sourceSession, message->targetSession);
		}
		break;
	}
	case RFU_HOST_DATA:
		if (rfu->role == GBA_RFU_CLIENT && message->source == rfu->hostPeer &&
		    message->sourceDevice == rfu->hostDevice && message->destinationDevice == rfu->deviceId &&
		    message->slot == rfu->clientNumber &&
		    (!rfu->synchronize || (message->sourceSession == rfu->hostRadioSession &&
		                          message->targetSession == rfu->radioSession))) {
			if (message->bytes && !_replacePacket(&rfu->rx, message->data, message->bytes)) {
				_log(rfu, "event packetDrop host bytes=%u", message->bytes);
			}
			if (rfu->clientTxPending) {
				/* Only the host can initiate RF communication. A client
				 * SendData arms one response, replaced by newer sends and
				 * consumed by this exchange, not by every later host frame. */
				struct GBASIORFUMessage data = {0};
				data.type = RFU_CLIENT_DATA;
				data.slot = rfu->clientNumber;
				data.destinationDevice = rfu->hostDevice;
				data.targetSession = rfu->hostRadioSession;
				data.bytes = rfu->tx.bytes;
				memcpy(data.data, rfu->tx.data, data.bytes);
				if (_post(rfu, rfu->hostPeer, &data)) {
					rfu->clientTxPending = false;
					_log(rfu, "event clientExchange bytes=%u epoch=%u", data.bytes, message->txEpoch);
				}
			}
			if (!rfu->clientTxPending) {
				/* Unread RX from an older exchange must not finish a new
				 * SendDataWait before its staged response has gone out. */
				rfu->clientWaitNeedsExchange = false;
			}
			reply.type = RFU_KEEPALIVE;
			reply.slot = rfu->clientNumber;
			reply.destinationDevice = rfu->hostDevice;
			reply.targetSession = rfu->hostRadioSession;
			reply.txEpoch = message->txEpoch;
			_post(rfu, rfu->hostPeer, &reply);
		}
		break;
	case RFU_CLIENT_DATA:
	case RFU_KEEPALIVE:
		if (rfu->role == GBA_RFU_HOST && message->slot < 4 && message->destinationDevice == rfu->deviceId) {
			struct GBASIORFUClient* client = &rfu->clients[message->slot];
			if (client->active && client->peer == message->source && client->device == message->sourceDevice &&
			    (!rfu->synchronize || (message->sourceSession == client->radioSession &&
			                          message->targetSession == rfu->radioSession))) {
				if (message->type == RFU_KEEPALIVE) {
					if (message->bytes) {
						break;
					}
					/* A delayed ACK still proves this registered peer is alive;
					 * only its receipt status is scoped to a TX epoch. */
					client->frames = 0;
					/* An ACK for an earlier packet may still be in a peer's
					 * mailbox. It must not finish the current transmission. */
					if (message->txEpoch != rfu->txEpoch ||
					    !(rfu->txTargetMask & (1U << message->slot))) {
						break;
					}
					rfu->txAckMask |= 1U << message->slot;
					_log(rfu, "event txAck epoch=%u slot=%u received=%02X targets=%02X", rfu->txEpoch,
						message->slot, rfu->txAckMask, rfu->txTargetMask);
				} else if (message->bytes > 16) {
					break;
				}
				client->frames = 0;
				if (message->type == RFU_CLIENT_DATA && message->bytes &&
				    !_replacePacket(&client->rx, message->data, message->bytes)) {
					_log(rfu, "event packetDrop client slot=%u bytes=%u", message->slot, message->bytes);
				}
			}
		}
		break;
	default:
		break;
	}
}

static void _drain(struct GBASIORFUDriver* rfu) {
	struct GBASIORFUCoordinator* coordinator = rfu->coordinator;
	if (!coordinator) {
		return;
	}
	_prearmedReconcile(rfu);
	/* Bound one service turn even if a peer keeps producing messages. */
	for (unsigned count = 0; count < GBA_RFU_MESSAGE_DEPTH; ++count) {
		struct GBASIORFUMessage message;
		bool available;
		MutexLock(&coordinator->mutex);
		available = rfu->incomingCount != 0;
		if (available) {
			message = rfu->incoming[rfu->incomingHead];
			rfu->incomingHead = (rfu->incomingHead + 1) % GBA_RFU_MESSAGE_DEPTH;
			--rfu->incomingCount;
		}
		MutexUnlock(&coordinator->mutex);
		if (!available) {
			break;
		}
		_handleMessage(rfu, &message);
	}
	_radioService(rfu);
}

static void _sendData(struct GBASIORFUDriver* rfu) {
	struct GBASIORFUMessage message = {0};
	message.bytes = rfu->tx.bytes;
	memcpy(message.data, rfu->tx.data, message.bytes);
	if (rfu->role == GBA_RFU_HOST) {
		message.type = RFU_HOST_DATA;
		if (rfu->command != 0x37 || !rfu->txEpoch) {
			if (!++rfu->txEpoch) {
				++rfu->txEpoch;
			}
			rfu->txAckMask = 0;
		}
		message.txEpoch = rfu->txEpoch;
		if (_radioOffer(rfu, &message)) {
			_logSendSnapshot(rfu, &message);
			return;
		}
		rfu->txTargetMask = 0;
		/* ResumeRetransmit resumes the same logical packet. A delayed
		 * receipt ACK remains valid, while explicit SendData starts anew.
		 * We still transmit to every currently registered peer here. */
		for (unsigned slot = 0; slot < 4; ++slot) {
			if (rfu->clients[slot].active) {
				rfu->txTargetMask |= 1U << slot;
				message.slot = slot;
				message.destinationDevice = rfu->clients[slot].device;
				message.targetSession = rfu->clients[slot].radioSession;
				_post(rfu, rfu->clients[slot].peer, &message);
			}
		}
		rfu->txAckMask &= rfu->txTargetMask;
	} else if (rfu->role == GBA_RFU_CLIENT) {
		/* TX buffer contents are retained for explicit retransmit/ghost
		 * commands. Zero bytes cancel an older staged response. */
		rfu->clientTxPending = rfu->tx.bytes != 0;
		if (rfu->command == 0x24) {
			_radioReady(rfu);
		}
	}
	_logSendSnapshot(rfu, &message);
}

static int _command(struct GBASIORFUDriver* rfu) {
	memset(rfu->response, 0, sizeof(rfu->response));
	rfu->responseIndex = 0;
	switch (rfu->command) {
	case 0x10:
	case 0x3D:
		return 0;
	case 0x17:
		if (rfu->sendWordsReceived != 1) {
			return -2;
		}
		rfu->timeoutFrames = rfu->payload[0];
		rfu->retryCount = rfu->payload[0] >> 8;
		_log(rfu, "event waitLimits timeoutFrames=%u retryCount=%u", rfu->timeoutFrames, rfu->retryCount);
		return 0;
	case 0x11:
		if (rfu->role == GBA_RFU_HOST) {
			for (unsigned slot = 0; slot < 4; ++slot) {
				if (rfu->clients[slot].active && rfu->clients[slot].frames < RFU_INACTIVE_FRAMES) {
					rfu->response[0] |= 0xFFU << (slot * 8);
				}
			}
		} else if (rfu->role == GBA_RFU_CLIENT) {
			rfu->response[0] = 0xFFFFFFFF;
		}
		return 1;
	case 0x12:
		rfu->response[0] = 0x00830117;
		return 1;
	case 0x13:
		if (rfu->role == GBA_RFU_HOST) {
			rfu->response[0] = 0x01000000 | rfu->deviceId;
		} else if (rfu->role == GBA_RFU_CLIENT) {
			rfu->response[0] = 0x05000000 | (1U << (16 + rfu->clientNumber)) | rfu->deviceId;
		}
		return 1;
	case 0x14: {
		unsigned words = 1;
		if (rfu->role != GBA_RFU_HOST) {
			return 0;
		}
		for (unsigned slot = 0; slot < 4; ++slot) {
			if (rfu->clients[slot].active) {
				rfu->response[words++] = rfu->clients[slot].device | (slot << 16);
			}
		}
		rfu->response[0] = words - 1;
		return words;
	}
	case 0x16:
		if (rfu->sendWordsReceived != 6) {
			return -2;
		}
		memcpy(rfu->broadcast, rfu->payload, sizeof(rfu->broadcast));
		rfu->hasBroadcast = true;
		_broadcast(rfu);
		return 0;
	case 0x19:
		if (rfu->role == GBA_RFU_CLIENT || rfu->role == GBA_RFU_CONNECTING) {
			return -1;
		}
		if (rfu->role != GBA_RFU_HOST) {
			rfu->role = GBA_RFU_HOST;
			rfu->deviceId = _newDevice(rfu);
			memset(rfu->clients, 0, sizeof(rfu->clients));
		}
		rfu->accepting = true;
		rfu->broadcastFrames = 0;
		_broadcast(rfu);
		return 0;
	case 0x1A: {
		unsigned words = 0;
		if (rfu->role != GBA_RFU_HOST) {
			return -1;
		}
		for (unsigned slot = 0; slot < 4; ++slot) {
			if (rfu->clients[slot].active) {
				rfu->response[words++] = rfu->clients[slot].device | (slot << 16);
			}
		}
		return words;
	}
	case 0x1B:
		if (rfu->role != GBA_RFU_HOST) {
			return -1;
		}
		rfu->accepting = false;
		for (unsigned slot = 0; slot < 4; ++slot) {
			if (rfu->clients[slot].active) {
				return 0;
			}
		}
		rfu->role = GBA_RFU_IDLE;
		return 0;
	case 0x1C:
		return 0;
	case 0x1D:
	case 0x1E: {
		unsigned words = 0;
		for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
			struct GBASIORFUBroadcast* entry = &rfu->peerBroadcast[peer];
			if (entry->valid) {
				rfu->response[words++] = entry->device;
				memcpy(&rfu->response[words], entry->data, sizeof(entry->data));
				words += 6;
			}
		}
		return words;
	}
	case 0x1F:
		if (rfu->role == GBA_RFU_HOST || rfu->sendWordsReceived != 1) {
			return -1;
		}
		for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
			if (rfu->peerBroadcast[peer].valid && rfu->peerBroadcast[peer].device == (rfu->payload[0] & 0xFFFF)) {
				if (rfu->synchronize) {
					/* A fresh connection attempt cannot accept a held ACK,
					 * OFFER or disconnect from an earlier use of this slot. */
					_disconnect(rfu);
				}
				struct GBASIORFUMessage message = {0};
				message.type = RFU_CONNECT;
				message.destinationDevice = rfu->peerBroadcast[peer].device;
				message.targetSession = rfu->peerBroadcast[peer].radioSession;
				rfu->role = GBA_RFU_CONNECTING;
				rfu->hostPeer = peer;
				rfu->hostDevice = message.destinationDevice;
				rfu->hostRadioSession = message.targetSession;
				rfu->connectCycles = 32 * RFU_FRAME_CYCLES;
				_log(rfu, "event connectRequest peer=%u device=%04X peerSession=%u", peer, rfu->hostDevice, rfu->hostRadioSession);
				_post(rfu, peer, &message);
				return 0;
			}
		}
		_log(rfu, "event connectMissingBroadcast requested=%04X", rfu->payload[0] & 0xFFFF);
		rfu->role = GBA_RFU_IDLE;
		return 0;
	case 0x20:
		if (rfu->role == GBA_RFU_HOST) {
			return -1;
		}
		rfu->response[0] = rfu->role == GBA_RFU_CONNECTING ? 0x01000000 :
			(rfu->role == GBA_RFU_CLIENT ? rfu->deviceId | ((uint32_t) rfu->clientNumber << 16) : 0x02000000);
		return 1;
	case 0x21:
		if (rfu->role == GBA_RFU_HOST) {
			return -1;
		}
		rfu->response[0] = rfu->role == GBA_RFU_CLIENT ?
			rfu->deviceId | ((uint32_t) rfu->clientNumber << 16) : 0x01000000;
		if (rfu->role != GBA_RFU_CLIENT) {
			rfu->role = GBA_RFU_IDLE;
			rfu->connectCycles = 0;
		}
		return 1;
	case 0x24:
	case 0x25: {
		if (rfu->role != GBA_RFU_HOST && rfu->role != GBA_RFU_CLIENT) {
			return -1;
		}
		if (rfu->role == GBA_RFU_CLIENT && rfu->command == 0x25) {
			rfu->clientWaitNeedsExchange = true;
		}
		if (!rfu->sendWordsReceived) {
			if (rfu->synchronize) {
				/* Even a zero-data command participates in the optional
				 * exchange. The disabled protocol retains its old behavior. */
				_prearmedStageTx(rfu);
				rfu->tx.bytes = 0;
				_sendData(rfu);
			}
			return 0;
		}
		unsigned bytes = rfu->role == GBA_RFU_HOST ? rfu->payload[0] & 0x7F :
			(rfu->payload[0] >> (8 + rfu->clientNumber * 5)) & 0x1F;
		unsigned maxBytes = rfu->role == GBA_RFU_HOST ? 90 : 16;
		bool ghost = rfu->sendWordsReceived == 1;
		_log(rfu, "event sendHeader cmd=%02X role=%u header=%08X bytes=%u supplied=%u", rfu->command,
			rfu->role, rfu->payload[0], bytes, (rfu->sendWordsReceived - 1U) * 4U);
		if (bytes > maxBytes || (ghost ? bytes > 4 : bytes > (rfu->sendWordsReceived - 1U) * 4U)) {
			return -2;
		}
		_prearmedStageTx(rfu);
		rfu->tx.bytes = bytes;
		/* A header-only send clocks up to four bytes left in the RFU TX
		 * buffer. Games use a one-byte ghost send to let clients reply. */
		if (!ghost) {
			for (unsigned i = 0; i < bytes; ++i) {
				rfu->tx.data[i] = rfu->payload[1 + i / 4] >> ((i % 4) * 8);
			}
		}
		_sendData(rfu);
		return 0;
	}
	case 0x37:
		if (rfu->role != GBA_RFU_HOST && rfu->role != GBA_RFU_CLIENT) {
			return -1;
		}
		if (rfu->role == GBA_RFU_CLIENT) {
			_prearmedStageTx(rfu);
			rfu->clientWaitNeedsExchange = true;
		}
		_sendData(rfu);
		return 0;
	case 0x26: {
		unsigned bytes = 0;
		uint8_t data[92] = {0};
		struct GBASIORFUPacket packet;
		if (rfu->role == GBA_RFU_HOST) {
			for (unsigned slot = 0; slot < 4; ++slot) {
				if (rfu->clients[slot].active && _popPacket(&rfu->clients[slot].rx, &packet)) {
					rfu->response[0] |= (uint32_t) packet.bytes << (8 + slot * 5);
					memcpy(&data[bytes], packet.data, packet.bytes);
					bytes += packet.bytes;
				}
			}
		} else if (rfu->role == GBA_RFU_CLIENT && _popPacket(&rfu->rx, &packet)) {
			bytes = packet.bytes;
			rfu->response[0] = bytes;
			memcpy(data, packet.data, bytes);
		}
		for (unsigned i = 0; i < bytes; ++i) {
			rfu->response[1 + i / 4] |= (uint32_t) data[i] << ((i % 4) * 8);
		}
		if (bytes && _packetLogEnabled(rfu)) {
			_log(rfu, "event readPacket bytes=%u header=%08X fingerprint=%08X", bytes, rfu->response[0], _packetFingerprint(data, bytes));
		}
		return 1 + (bytes + 3) / 4;
	}
	case 0x27:
		return 0;
	case 0x30:
		if (rfu->role == GBA_RFU_HOST) {
			unsigned mask = rfu->sendWordsReceived ? rfu->payload[0] : 15;
			for (unsigned slot = 0; slot < 4; ++slot) {
				if ((mask & (1U << slot)) && rfu->clients[slot].active) {
					_prearmedRetirePeer(rfu, slot);
					if (rfu->radioPending && rfu->radioOffer.slot == slot) {
						_radioCancel(rfu, true);
						rfu->radioQueued = false;
					}
					struct GBASIORFUMessage message = {0};
					message.type = RFU_DISCONNECT;
					message.slot = slot;
					message.destinationDevice = rfu->clients[slot].device;
					message.targetSession = rfu->clients[slot].radioSession;
					_post(rfu, rfu->clients[slot].peer, &message);
					memset(&rfu->clients[slot], 0, sizeof(rfu->clients[slot]));
					rfu->txTargetMask &= ~(1U << slot);
					rfu->txAckMask &= ~(1U << slot);
				}
			}
		} else {
			_disconnect(rfu);
		}
		return 0;
	default:
		_log(rfu, "event unsupported cmd=%02X words=%u", rfu->command, rfu->sendWordsReceived);
		return -2;
	}
}

static void _processCommand(struct GBASIORFUDriver* rfu) {
	if (rfu->sendWordsExpected > sizeof(rfu->payload) / sizeof(rfu->payload[0])) {
		rfu->errorCode = 2;
		rfu->state = GBA_RFU_ERROR;
		return;
	}
	int result = _command(rfu);
	if (result < 0) {
		_log(rfu, "event commandRejected cmd=%02X words=%u header=%08X error=%d", rfu->command,
			rfu->sendWordsReceived, rfu->payload[0], -result);
		rfu->errorCode = -result;
		rfu->state = GBA_RFU_ERROR;
	} else {
		rfu->responseWords = result;
		rfu->state = GBA_RFU_RESPONSE;
	}
}

static uint32_t _transfer(struct GBASIORFUDriver* rfu, uint32_t value) {
	uint32_t result = 0x80000000;
	switch (rfu->state) {
	case GBA_RFU_RESET:
		result = 0;
		if ((value & 0xFFFF) == 0x494E) {
			rfu->state = GBA_RFU_HANDSHAKE;
		}
		break;
	case GBA_RFU_HANDSHAKE:
		result = (value << 16) | ((~rfu->previousWord) & 0xFFFF);
		if (value == 0xB0BB8001) {
			rfu->state = GBA_RFU_COMMAND;
			_log(rfu, "event initialized");
		}
		break;
	case GBA_RFU_COMMAND:
		if ((value >> 16) == 0x9966) {
			_cooperativeRetire(rfu, false);
			rfu->command = value;
			if (rfu->synchronize && (rfu->command == 0x24 || rfu->command == 0x25 ||
			                        rfu->command == 0x27 || rfu->command == 0x37)) {
				/* Cancellation belongs to the earlier command. Clear it at
				 * framing, not WAIT entry, so a cancellation arriving during
				 * this command's payload/response still reaches this wait. */
				rfu->radioTimedOut = false;
			}
			rfu->sendWordsExpected = value >> 8;
			rfu->sendWordsReceived = 0;
			rfu->responseWords = 0;
			rfu->responseIndex = 0;
			memset(rfu->payload, 0, sizeof(rfu->payload));
			_log(rfu, "event command cmd=%02X words=%u", rfu->command, rfu->sendWordsExpected);
			if (rfu->sendWordsExpected) {
				rfu->state = GBA_RFU_PAYLOAD;
			} else {
				_processCommand(rfu);
			}
		}
		break;
	case GBA_RFU_PAYLOAD:
		if (rfu->sendWordsReceived < sizeof(rfu->payload) / sizeof(rfu->payload[0])) {
			rfu->payload[rfu->sendWordsReceived] = value;
		}
		++rfu->sendWordsReceived;
		if (rfu->sendWordsReceived == rfu->sendWordsExpected) {
			_processCommand(rfu);
		}
		break;
	case GBA_RFU_RESPONSE:
		result = 0x99660080 | rfu->command | ((uint32_t) rfu->responseWords << 8);
		_log(rfu, "event response cmd=%02X words=%u rx=%08X", rfu->command, rfu->responseWords, result);
		if (rfu->command == 0x25 || rfu->command == 0x27 || rfu->command == 0x37) {
			rfu->state = GBA_RFU_WAIT;
			rfu->waitCycles = rfu->timeoutFrames * RFU_FRAME_CYCLES;
			rfu->retryCycles = rfu->retryCount * (RFU_FRAME_CYCLES / 6);
			_radioReady(rfu);
			if (rfu->synchronize && rfu->role == GBA_RFU_HOST && rfu->command == 0x27 && !rfu->radioPending) {
				struct GBASIORFUMessage offer = {0};
				offer.txEpoch = rfu->txEpoch;
				offer.bytes = rfu->tx.bytes;
				memcpy(offer.data, rfu->tx.data, offer.bytes);
				_radioOffer(rfu, &offer);
			}
			_schedulePoll(rfu);
		} else {
			rfu->state = rfu->responseWords ? GBA_RFU_PAYLOAD_RESPONSE : GBA_RFU_COMMAND;
		}
		break;
	case GBA_RFU_PAYLOAD_RESPONSE:
		result = rfu->response[rfu->responseIndex++];
		_log(rfu, "recvWord index=%u/%u rx=%08X", rfu->responseIndex, rfu->responseWords, result);
		if (rfu->responseIndex == rfu->responseWords) {
			rfu->state = GBA_RFU_COMMAND;
		}
		break;
	case GBA_RFU_ERROR:
		result = 0x996601EE;
		rfu->state = GBA_RFU_ERROR_CODE;
		break;
	case GBA_RFU_ERROR_CODE:
		result = rfu->errorCode;
		rfu->state = GBA_RFU_COMMAND;
		break;
	case GBA_RFU_WAIT:
	case GBA_RFU_ASYNC_RESPONSE:
		/* Only adapter-driven clock edges may finish this phase. */
		break;
	}
	rfu->previousWord = value;
	return result;
}

static bool _dataAvailable(const struct GBASIORFUDriver* rfu) {
	if (rfu->role == GBA_RFU_CLIENT) {
		return rfu->rx.count != 0;
	}
	if (rfu->role == GBA_RFU_HOST) {
		for (unsigned slot = 0; slot < 4; ++slot) {
			if (rfu->clients[slot].active && rfu->clients[slot].rx.count) {
				return true;
			}
		}
	}
	return false;
}

static uint32_t _txStatus(const struct GBASIORFUDriver* rfu) {
	uint32_t status = rfu->txAckMask & rfu->txTargetMask;
	for (unsigned slot = 0; slot < 4; ++slot) {
		if ((rfu->txTargetMask & (1U << slot)) && rfu->clients[slot].active &&
		    rfu->clients[slot].frames >= RFU_INACTIVE_FRAMES) {
			status |= 1U << (8 + slot);
		}
	}
	return status;
}

static bool _asyncReply(struct GBASIORFUDriver* rfu, uint8_t command, bool payload, uint32_t data) {
	if (rfu->prearmed && rfu->synchronize && rfu->role == GBA_RFU_CLIENT && rfu->state == GBA_RFU_WAIT) {
		const bool newlyClaimed = _prearmedWithdraw(rfu);
		/* A claim that won just before an old unread-RX success notice must
		 * finish its actual exchange first, not end the newly claimed WAIT.
		 * An already-promoted normal OFFER is a later radio ticket, however;
		 * it must not suppress the completed exchange's console notification. */
		if (command == 0x28 && newlyClaimed && rfu->radioPending) return false;
		rfu->radioReady = false;
	}
	rfu->asyncWords[0] = 0x99660000 | command | (payload ? 0x100 : 0);
	rfu->asyncWords[1] = payload ? data : 0x80000000;
	rfu->asyncWords[2] = 0x80000000;
	rfu->asyncCount = payload ? 3 : 2;
	rfu->asyncIndex = 0;
	rfu->state = GBA_RFU_ASYNC_RESPONSE;
	_log(rfu, "event asyncReply cmd=%02X payload=%d value=%08X", command, payload, data);
	return true;
}

static void _pollEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBASIORFUDriver* rfu = context;
	_drain(rfu);
	if (rfu->state == GBA_RFU_WAIT) {
		unsigned elapsed = RFU_POLL_CYCLES + cyclesLate;
		rfu->waitCycles -= elapsed < rfu->waitCycles ? elapsed : rfu->waitCycles;
		rfu->retryCycles -= elapsed < rfu->retryCycles ? elapsed : rfu->retryCycles;
		/* Bit 0 chooses the clock owner. Bit 1 only chooses its frequency. */
		if (rfu->mode == GBA_SIO_NORMAL_32 && !(rfu->d.p->siocnt & RFU_CLOCK_INTERNAL)) {
			if (rfu->role == GBA_RFU_IDLE) {
				_asyncReply(rfu, 0x29, true, 15);
			} else {
				bool txWait = rfu->role == GBA_RFU_HOST && (rfu->command == 0x25 || rfu->command == 0x37);
				bool clientTxWait = rfu->role == GBA_RFU_CLIENT && (rfu->command == 0x25 || rfu->command == 0x37);
				if (rfu->synchronize && rfu->radioTimedOut) {
					rfu->radioTimedOut = false;
					_asyncReply(rfu, 0x27, false, 0);
				} else if (rfu->synchronize && rfu->radioNotice &&
				           (!clientTxWait || (!rfu->clientWaitNeedsExchange && !rfu->clientTxPending))) {
					if (_asyncReply(rfu, 0x28, false, 0)) {
						rfu->radioNotice = false;
					}
				} else if (rfu->synchronize && rfu->radioPending) {
					if (txWait && rfu->retryCount && !rfu->retryCycles) {
						_log(rfu, "event retryExpired matched=%d released=%d due=%lld ack=%02X targets=%02X", rfu->radioMatched,
							rfu->radioReleased, (long long) rfu->radioDue, rfu->txAckMask, rfu->txTargetMask);
						_radioCancel(rfu, true);
						rfu->radioQueued = false;
						_asyncReply(rfu, 0x28, true, _txStatus(rfu));
					} else if (rfu->timeoutFrames && !rfu->waitCycles) {
						_log(rfu, "event waitExpired matched=%d due=%lld", rfu->radioMatched, (long long) rfu->radioDue);
						_radioCancel(rfu, true);
						rfu->radioQueued = false;
						_asyncReply(rfu, 0x27, false, 0);
					}
				} else if (txWait && (rfu->txAckMask & rfu->txTargetMask) == rfu->txTargetMask) {
					_asyncReply(rfu, 0x28, false, 0);
				} else if (txWait && rfu->retryCount && !rfu->retryCycles) {
					_log(rfu, "event retryExpired matched=%d ack=%02X targets=%02X", rfu->radioMatched, rfu->txAckMask, rfu->txTargetMask);
					/* Exhausting a finite retry budget reports real RF ACKs.
					 * Failure to ACK one packet is not a four-second outage. */
					_asyncReply(rfu, 0x28, true, _txStatus(rfu));
				} else if (!txWait && _dataAvailable(rfu) &&
				           (!clientTxWait || (!rfu->clientWaitNeedsExchange && !rfu->clientTxPending))) {
					_asyncReply(rfu, 0x28, false, 0);
				} else if (rfu->timeoutFrames && !rfu->waitCycles) {
					_log(rfu, "event waitExpired awaitingOffer=%d", !rfu->radioPending);
					if (rfu->synchronize) {
						/* A wait may expire before any host offer arrives. Its
						 * readiness must not accept an unrelated later ticket. */
						_radioCancel(rfu, false);
						rfu->radioQueued = false;
					}
					_asyncReply(rfu, 0x27, false, 0);
				}
			}
		}
	}
	if (rfu->state == GBA_RFU_ASYNC_RESPONSE && rfu->mode == GBA_SIO_NORMAL_32 && !rfu->adapterPending) {
		uint16_t siocnt = rfu->d.p->siocnt;
		if (!(siocnt & (RFU_CLOCK_INTERNAL | RFU_SI | RFU_SO)) && (siocnt & RFU_START)) {
			rfu->adapterPending = true;
			rfu->adapterIndex = rfu->asyncIndex;
			rfu->adapterWord = rfu->asyncWords[rfu->asyncIndex];
			/* The adapter owns the 2 MHz clock. Polling readiness does not
			 * finish a 32-bit wire transfer or clear Start immediately. */
			mTimingSchedule(timing, &rfu->adapterEvent, 256);
			_log(rfu, "event externalStart index=%u/%u rx=%08X siocnt=%04X time=%u", rfu->adapterIndex,
				rfu->asyncCount, rfu->adapterWord, siocnt, (unsigned) mTimingCurrentTime(timing));
		}
	}
	if (rfu->state == GBA_RFU_WAIT || rfu->state == GBA_RFU_ASYNC_RESPONSE || rfu->radioPending) {
		/* Draining a new/queued offer can already schedule this event.
		 * Never insert the same timing node twice in the scheduler. */
		_schedulePoll(rfu);
	}
	/* Yield only after all mailbox work and local completion checks. The poll
	 * remains scheduled so a wake resumes the same finite emulated deadline. */
	_cooperativeTryPark(rfu);
}

static void _adapterEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	struct GBASIORFUDriver* rfu = context;
	if (!rfu->adapterPending) {
		return;
	}
	rfu->adapterPending = false;
	if (!rfu->d.p || rfu->mode != GBA_SIO_NORMAL_32 || rfu->state != GBA_RFU_ASYNC_RESPONSE ||
	    rfu->asyncIndex != rfu->adapterIndex || (rfu->d.p->siocnt & RFU_CLOCK_INTERNAL) ||
	    !(rfu->d.p->siocnt & RFU_START)) {
		return;
	}
	uint32_t word = rfu->adapterWord;
	++rfu->asyncIndex;
	if (rfu->asyncIndex == rfu->asyncCount) {
		rfu->state = GBA_RFU_COMMAND;
	}
	GBASIONormal32FinishTransfer(rfu->d.p, word, cyclesLate);
	rfu->previousSIOCNT = rfu->d.p->siocnt;
	_log(rfu, "event externalComplete index=%u/%u rx=%08X siocnt=%04X time=%u", rfu->asyncIndex,
		rfu->asyncCount, word, rfu->d.p->siocnt, (unsigned) mTimingCurrentTime(timing));
}

static void _clockEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(timing);
	struct GBASIORFUDriver* rfu = context;
	if (!rfu->internalPending) {
		return;
	}
	rfu->internalPending = false;
	if (rfu->mode != GBA_SIO_NORMAL_32 || !(rfu->d.p->siocnt & RFU_CLOCK_INTERNAL) ||
	    !(rfu->d.p->siocnt & RFU_START)) {
		return;
	}
	_drain(rfu);
	uint32_t result = _transfer(rfu, rfu->transferWord);
	/* Adapter is busy until the GBA's SO rising edge acknowledges completion. */
	rfu->d.p->siocnt |= RFU_SI;
	GBASIONormal32FinishTransfer(rfu->d.p, result, cyclesLate);
	rfu->previousSIOCNT = rfu->d.p->siocnt;
}

static void _maintenanceEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	UNUSED(cyclesLate);
	struct GBASIORFUDriver* rfu = context;
	_drain(rfu);
	for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
		struct GBASIORFUBroadcast* entry = &rfu->peerBroadcast[peer];
		if (entry->valid && !--entry->ttl) {
			entry->valid = false;
		}
	}
	if (rfu->role == GBA_RFU_HOST) {
		if (++rfu->broadcastFrames >= 30) {
			rfu->broadcastFrames = 0;
			_broadcast(rfu);
		}
		for (unsigned slot = 0; slot < 4; ++slot) {
			if (rfu->clients[slot].active && rfu->clients[slot].frames < RFU_INACTIVE_FRAMES &&
			    ++rfu->clients[slot].frames == RFU_INACTIVE_FRAMES) {
				_log(rfu, "event clientTimeout slot=%u", slot);
			}
		}
	} else if (rfu->role == GBA_RFU_CONNECTING) {
		rfu->connectCycles -= rfu->connectCycles > RFU_FRAME_CYCLES ? RFU_FRAME_CYCLES : rfu->connectCycles;
		if (!rfu->connectCycles) {
			_log(rfu, "event connectTimeout host=%04X hostSession=%u", rfu->hostDevice, rfu->hostRadioSession);
			rfu->role = GBA_RFU_IDLE;
		}
	}
	mTimingSchedule(timing, &rfu->maintenanceEvent, RFU_FRAME_CYCLES);
}

void GBASIORFUCoordinatorInit(struct GBASIORFUCoordinator* coordinator) {
	memset(coordinator, 0, sizeof(*coordinator));
	MutexInit(&coordinator->mutex);
}

void GBASIORFUCoordinatorDeinit(struct GBASIORFUCoordinator* coordinator) {
	/* Usually empty after detach. Retire hooks before destroying protection. */
	MutexLock(&coordinator->mutex);
	for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
		struct GBASIORFUDriver* target = coordinator->players[peer];
		if (!target) continue;
		_cooperativeRetireLocked(target);
		target->coordinator = NULL;
		target->playerId = -1;
	}
	MutexUnlock(&coordinator->mutex);
	MutexDeinit(&coordinator->mutex);
	memset(coordinator, 0, sizeof(*coordinator));
}

void GBASIORFUCoordinatorService(struct GBASIORFUCoordinator* coordinator, uint64_t nowUs) {
	if (!coordinator) return;
	MutexLock(&coordinator->mutex);
	/* A reset/backwards service clock cannot strand an old deadline. */
	const bool invalidClock = !nowUs || nowUs < coordinator->cooperativeNowUs;
	coordinator->cooperativeNowUs = invalidClock ? 0 : nowUs;
	for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
		struct GBASIORFUDriver* target = coordinator->players[peer];
		if (!target || !target->cooperativeDeadlineUs) continue;
		if (invalidClock || nowUs >= target->cooperativeDeadlineUs) {
			target->cooperativeBypass = true;
			target->cooperativeDeadlineUs = 0;
			_cooperativeWakeLocked(target);
		}
	}
	MutexUnlock(&coordinator->mutex);
}

void GBASIORFUCoordinatorAttach(struct GBASIORFUCoordinator* coordinator, struct GBASIORFUDriver* rfu) {
	MutexLock(&coordinator->mutex);
	rfu->playerId = -1;
	for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
		if (coordinator->players[peer] == rfu) {
			rfu->playerId = peer;
			break;
		}
	}
	if (rfu->playerId < 0) {
		for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
			if (!coordinator->players[peer]) {
				coordinator->players[peer] = rfu;
				rfu->playerId = peer;
				++coordinator->nAttached;
				break;
			}
		}
	}
	rfu->coordinator = rfu->playerId >= 0 ? coordinator : NULL;
	if (rfu->playerId >= 0) coordinator->cooperativeSessions[rfu->playerId] = rfu->radioSession;
	_cooperativeRetireLocked(rfu);
	MutexUnlock(&coordinator->mutex);
}

void GBASIORFUCoordinatorDetach(struct GBASIORFUCoordinator* coordinator, struct GBASIORFUDriver* rfu) {
	_disconnect(rfu);
	MutexLock(&coordinator->mutex);
	for (unsigned peer = 0; peer < MAX_GBAS; ++peer) {
		if (coordinator->players[peer] == rfu) {
			_cooperativeRetireLocked(rfu);
			coordinator->players[peer] = NULL;
			coordinator->cooperativeSessions[peer] = 0;
			--coordinator->nAttached;
		}
	}
	rfu->incomingCount = 0;
	rfu->incomingHead = 0;
	rfu->coordinator = NULL;
	rfu->playerId = -1;
	MutexUnlock(&coordinator->mutex);
}

size_t GBASIORFUCoordinatorAttached(struct GBASIORFUCoordinator* coordinator) {
	MutexLock(&coordinator->mutex);
	size_t attached = coordinator->nAttached;
	MutexUnlock(&coordinator->mutex);
	return attached;
}

static void _deinit(struct GBASIODriver* driver) {
	struct GBASIORFUDriver* rfu = (struct GBASIORFUDriver*) driver;
	struct mTiming* timing = _timing(rfu);
	if (timing) {
		mTimingDeschedule(timing, &rfu->clockEvent);
		mTimingDeschedule(timing, &rfu->adapterEvent);
		mTimingDeschedule(timing, &rfu->pollEvent);
		mTimingDeschedule(timing, &rfu->maintenanceEvent);
	}
	rfu->internalPending = false;
	rfu->adapterPending = false;
	_disconnect(rfu);
}

static void _reset(struct GBASIODriver* driver) {
	struct GBASIORFUDriver* rfu = (struct GBASIORFUDriver*) driver;
	_deinit(driver);
	_radioNewSession(rfu);
	rfu->radioClock = 0;
	rfu->radioLastTime = _timing(rfu) ? mTimingCurrentTime(_timing(rfu)) : 0;
	rfu->radioLastTicket = 0;
	rfu->hostRadioSession = 0;
	rfu->state = GBA_RFU_RESET;
	rfu->previousWord = 0;
	rfu->previousSIOCNT = 0;
	rfu->previousRCNT = 0;
	rfu->command = 0;
	rfu->sendWordsExpected = 0;
	rfu->sendWordsReceived = 0;
	rfu->responseWords = 0;
	rfu->responseIndex = 0;
	rfu->asyncCount = 0;
	rfu->asyncIndex = 0;
	rfu->timeoutFrames = 0;
	rfu->retryCount = 0;
	rfu->waitCycles = 0;
	rfu->retryCycles = 0;
	rfu->deviceId = 0;
	rfu->hasBroadcast = false;
	memset(rfu->broadcast, 0, sizeof(rfu->broadcast));
	memset(rfu->peerBroadcast, 0, sizeof(rfu->peerBroadcast));
	if (rfu->coordinator) {
		MutexLock(&rfu->coordinator->mutex);
		rfu->incomingCount = 0;
		rfu->incomingHead = 0;
		MutexUnlock(&rfu->coordinator->mutex);
	}
	struct mTiming* timing = _timing(rfu);
	if (timing) {
		mTimingSchedule(timing, &rfu->maintenanceEvent, RFU_FRAME_CYCLES);
	}
	_log(rfu, "event reset");
}

static bool _init(struct GBASIODriver* driver) {
	_reset(driver);
	return true;
}

static uint32_t _driverId(const struct GBASIODriver* driver) {
	UNUSED(driver);
	return RFU_DRIVER_ID;
}

static void _setMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	struct GBASIORFUDriver* rfu = (struct GBASIORFUDriver*) driver;
	if (rfu->mode != mode) {
		_cooperativeRetire(rfu, true);
		/* A fallback transfer belongs to the mode that started it. The
		 * core completion callback otherwise interprets it using the new
		 * mode and can prematurely complete an RFU-owned transaction. */
		struct mTiming* timing = _timing(rfu);
		if (timing) {
			mTimingDeschedule(timing, &rfu->d.p->completeEvent);
		}
		_log(rfu, "trace mode old=%u new=%u state=%u", rfu->mode, mode, rfu->state);
	}
	rfu->mode = mode;
	if (mode != GBA_SIO_NORMAL_32 && rfu->internalPending) {
		mTimingDeschedule(_timing(rfu), &rfu->clockEvent);
		rfu->internalPending = false;
	}
	if (mode != GBA_SIO_NORMAL_32) {
		_cancelAdapter(rfu);
	}
}

static bool _handlesMode(struct GBASIODriver* driver, enum GBASIOMode mode) {
	UNUSED(driver);
	return mode == GBA_SIO_NORMAL_32 || mode == GBA_SIO_GPIO;
}

static int _connectedDevices(struct GBASIODriver* driver) {
	struct GBASIORFUDriver* rfu = (struct GBASIORFUDriver*) driver;
	/* The GBA serial peer is its own adapter, independently of RF peers. */
	return rfu->mode == GBA_SIO_NORMAL_32 ? 1 : 0;
}

static int _deviceId(struct GBASIODriver* driver) {
	struct GBASIORFUDriver* rfu = (struct GBASIORFUDriver*) driver;
	/* An RFU must not masquerade as a second GBA on the cable bus. */
	return rfu->mode == GBA_SIO_MULTI ? 0 : rfu->playerId;
}

static uint16_t _writeSIOCNT(struct GBASIODriver* driver, uint16_t value) {
	struct GBASIORFUDriver* rfu = (struct GBASIORFUDriver*) driver;
	if (rfu->mode != GBA_SIO_NORMAL_32) {
		return value;
	}
	uint16_t old = rfu->previousSIOCNT;
	if (!(value & RFU_START) || (value & RFU_CLOCK_INTERNAL)) {
		_cancelAdapter(rfu);
	}
	value = (value & ~RFU_SI) | (old & RFU_SI);
	bool risingSO = (value & RFU_SO) && !(old & RFU_SO);
	bool fallingSO = !(value & RFU_SO) && (old & RFU_SO);
	if (value & RFU_CLOCK_INTERNAL) {
		if (risingSO) {
			value &= ~RFU_SI;
		}
	} else {
		if (risingSO) {
			value |= RFU_SI;
		} else if (fallingSO) {
			value &= ~RFU_SI;
		}
	}
	if ((value & RFU_START) && (value & RFU_CLOCK_INTERNAL) && !rfu->internalPending) {
		rfu->internalPending = true;
		rfu->transferWord = rfu->d.p->p->memory.io[GBA_REG(SIODATA32_LO)] |
			((uint32_t) rfu->d.p->p->memory.io[GBA_REG(SIODATA32_HI)] << 16);
		mTimingSchedule(_timing(rfu), &rfu->clockEvent, value & RFU_CLOCK_FAST ? 256 : 2048);
	} else if (rfu->internalPending && (!(value & RFU_START) || !(value & RFU_CLOCK_INTERNAL))) {
		mTimingDeschedule(_timing(rfu), &rfu->clockEvent);
		rfu->internalPending = false;
	}
	if (rfu->state == GBA_RFU_WAIT || rfu->state == GBA_RFU_ASYNC_RESPONSE) {
		_schedulePoll(rfu);
		if (value != old) {
			_log(rfu, "trace waitClock old=%04X new=%04X state=%u async=%u/%u time=%u", old, value,
				rfu->state, rfu->asyncIndex, rfu->asyncCount, (unsigned) mTimingCurrentTime(_timing(rfu)));
		}
	}
	rfu->previousSIOCNT = value;
	return value;
}

static uint16_t _writeRCNT(struct GBASIODriver* driver, uint16_t value) {
	struct GBASIORFUDriver* rfu = (struct GBASIORFUDriver*) driver;
	/* Reset pin is SD (bit 1). Only a driven rising edge resets the adapter;
 * entering GPIO alone is also used during ordinary clock handoff. */
	if ((value & 0xC000) == 0x8000 && (value & 0x0022) == 0x0022 &&
	    (rfu->previousRCNT & 0x0022) != 0x0022) {
		_reset(driver);
	}
	if (rfu->previousRCNT != value) {
		_log(rfu, "trace rcnt old=%04X new=%04X state=%u", rfu->previousRCNT, value, rfu->state);
	}
	rfu->previousRCNT = value;
	return value;
}

static bool _start(struct GBASIODriver* driver) {
	/* SIO invokes start before committing SIOCNT. Scheduling belongs to
	 * writeSIOCNT, which receives the actual clock owner and new Start bit.
	 * Unsupported modes retain the core's ordinary disconnected completion. */
	return ((struct GBASIORFUDriver*) driver)->mode != GBA_SIO_NORMAL_32;
}

static uint8_t _finishNormal8(struct GBASIODriver* driver) {
	UNUSED(driver);
	return 0xFF;
}

static void _finishMultiplayer(struct GBASIODriver* driver, uint16_t data[MAX_GBAS]) {
	for (unsigned player = 0; player < MAX_GBAS; ++player) {
		data[player] = 0xFFFF;
	}
	data[0] = driver->p->p->memory.io[GBA_REG(SIOMLT_SEND)];
}

void GBASIORFUDriverCreate(struct GBASIORFUDriver* rfu) {
	memset(rfu, 0, sizeof(*rfu));
	rfu->playerId = -1;
	rfu->d.init = _init;
	rfu->d.deinit = _deinit;
	rfu->d.reset = _reset;
	rfu->d.driverId = _driverId;
	rfu->d.setMode = _setMode;
	rfu->d.handlesMode = _handlesMode;
	rfu->d.connectedDevices = _connectedDevices;
	rfu->d.deviceId = _deviceId;
	rfu->d.writeSIOCNT = _writeSIOCNT;
	rfu->d.writeRCNT = _writeRCNT;
	rfu->d.start = _start;
	rfu->d.finishNormal8 = _finishNormal8;
	rfu->d.finishMultiplayer = _finishMultiplayer;
	rfu->clockEvent.context = rfu;
	rfu->clockEvent.name = "GBA RFU Master Transfer";
	rfu->clockEvent.callback = _clockEvent;
	rfu->clockEvent.priority = 0x70;
	rfu->adapterEvent.context = rfu;
	rfu->adapterEvent.name = "GBA RFU Adapter Transfer";
	rfu->adapterEvent.callback = _adapterEvent;
	rfu->adapterEvent.priority = 0x70;
	rfu->pollEvent.context = rfu;
	rfu->pollEvent.name = "GBA RFU Adapter Clock";
	rfu->pollEvent.callback = _pollEvent;
	rfu->pollEvent.priority = 0x71;
	rfu->maintenanceEvent.context = rfu;
	rfu->maintenanceEvent.name = "GBA RFU Local Transport";
	rfu->maintenanceEvent.callback = _maintenanceEvent;
	rfu->maintenanceEvent.priority = 0x72;
}

void GBASIORFUDriverSetLogPath(struct GBASIORFUDriver* rfu, const char* path) {
	if (!rfu) {
		return;
	}
	if (path && strcmp(path, rfu->logPath) == 0) {
		return;
	}
	rfu->logHead = 0;
	rfu->logSize = 0;
	rfu->logSequence = 0;
	rfu->logDropped = 0;
	if (!path || !path[0]) {
		free(rfu->logBuffer);
		rfu->logBuffer = NULL;
		rfu->logPath[0] = '\0';
		return;
	}
	if (!rfu->logBuffer) {
		rfu->logBuffer = malloc(GBA_RFU_LOG_CAPACITY);
		if (!rfu->logBuffer) {
			rfu->logPath[0] = '\0';
			return;
		}
	}
	strncpy(rfu->logPath, path ? path : "", sizeof(rfu->logPath) - 1);
	rfu->logPath[sizeof(rfu->logPath) - 1] = '\0';
	_log(rfu, "event logEnabled version=2 capacity=%u fileLimit=%u fingerprint=fnv1a32 cooperative=%d cooperativeBudgetUs=%u",
		GBA_RFU_LOG_CAPACITY, GBA_RFU_LOG_FILE_LIMIT, rfu->cooperativeUser != NULL, RFU_COOPERATIVE_BUDGET_US);
}

bool GBASIORFUDriverFlushLog(struct GBASIORFUDriver* rfu) {
	if (!rfu) {
		return false;
	}
	if (!rfu->logPath[0] || (!rfu->logSize && !rfu->logDropped)) {
		return true;
	}
	FILE* file = fopen(rfu->logPath, "ab");
	if (!file) {
		return false;
	}
	/* Bound persistent diagnostics too. Preserve capture on failure so a
	 * stopped caller can retry; never seek/write from a radio callback. */
	if (fseek(file, 0, SEEK_END) != 0 || ftell(file) < 0 ||
	    ftell(file) + (long) rfu->logSize + 256 > (long) GBA_RFU_LOG_FILE_LIMIT) {
		fclose(file);
		return false;
	}
	bool ok = fprintf(file, "# RFU events v2 flushWallUs=%llu sequence=%llu dropped=%llu bytes=%zu capacity=%u fileLimit=%u fingerprint=fnv1a32\n",
		(unsigned long long) _logWallMicros(), (unsigned long long) rfu->logSequence,
		(unsigned long long) rfu->logDropped, rfu->logSize, GBA_RFU_LOG_CAPACITY, GBA_RFU_LOG_FILE_LIMIT) >= 0;
	const size_t first = rfu->logSize < GBA_RFU_LOG_CAPACITY - rfu->logHead ?
		rfu->logSize : GBA_RFU_LOG_CAPACITY - rfu->logHead;
	if (ok) {
		ok = fwrite(rfu->logBuffer + rfu->logHead, 1, first, file) == first;
	}
	if (ok) {
		ok = fwrite(rfu->logBuffer, 1, rfu->logSize - first, file) == rfu->logSize - first;
	}
	if (fclose(file) != 0) {
		ok = false;
	}
	if (ok) {
		rfu->logHead = 0;
		rfu->logSize = 0;
		rfu->logDropped = 0;
	}
	return ok;
}

void GBASIORFUDriverSetSynchronize(struct GBASIORFUDriver* rfu, bool synchronize) {
	if (rfu->synchronize == synchronize) {
		return;
	}
	_cooperativeRetire(rfu, true);
	_prearmedRetire(rfu);
	_radioCancel(rfu, true);
	rfu->radioQueued = false;
	rfu->synchronize = synchronize;
	rfu->radioClock = 0;
	rfu->radioLastTime = _timing(rfu) ? mTimingCurrentTime(_timing(rfu)) : 0;
}

void GBASIORFUDriverSetPrearmed(struct GBASIORFUDriver* rfu, bool prearmed) {
	if (!rfu || rfu->prearmed == prearmed) return;
	_cooperativeRetire(rfu, true);
	/* Configure with the owner stopped. Retire before changing the flag so
	 * no queued old claim can revive an earlier wait after toggling. */
	_prearmedRetire(rfu);
	_radioCancel(rfu, true);
	rfu->radioQueued = false;
	rfu->prearmed = prearmed;
}

void GBASIORFUDriverSetCooperative(struct GBASIORFUDriver* rfu, struct mLockstepUser* user) {
	if (!rfu) return;
	struct GBASIORFUCoordinator* coordinator = rfu->coordinator;
	if (coordinator) MutexLock(&coordinator->mutex);
	_cooperativeRetireLocked(rfu);
	if (coordinator) _cooperativeRetirePeersLocked(rfu);
	rfu->cooperativeUser = user;
	if (coordinator) MutexUnlock(&coordinator->mutex);
}
