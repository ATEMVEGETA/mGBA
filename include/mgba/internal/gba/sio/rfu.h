/* Copyright (c) 2026 mGBA contributors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#ifndef GBA_SIO_RFU_H
#define GBA_SIO_RFU_H

#include <mgba-util/common.h>

CXX_GUARD_START

#include <mgba/internal/gba/sio.h>
#include <mgba/core/lockstep.h>
#include <mgba-util/threading.h>

struct GBASIORFUDriver;

#define GBA_RFU_PACKET_DEPTH 1
#define GBA_RFU_MESSAGE_DEPTH 64
#define GBA_RFU_MAX_BYTES 92
#define GBA_RFU_LOG_CAPACITY (16U * 1024U * 1024U)
#define GBA_RFU_LOG_FILE_LIMIT (128U * 1024U * 1024U)

enum GBASIORFUState {
	GBA_RFU_RESET,
	GBA_RFU_HANDSHAKE,
	GBA_RFU_COMMAND,
	GBA_RFU_PAYLOAD,
	GBA_RFU_RESPONSE,
	GBA_RFU_PAYLOAD_RESPONSE,
	GBA_RFU_ERROR,
	GBA_RFU_ERROR_CODE,
	GBA_RFU_WAIT,
	GBA_RFU_ASYNC_RESPONSE
};

enum GBASIORFURole {
	GBA_RFU_IDLE,
	GBA_RFU_HOST,
	GBA_RFU_CONNECTING,
	GBA_RFU_CLIENT
};

struct GBASIORFUPacket {
	uint8_t bytes;
	uint8_t data[GBA_RFU_MAX_BYTES];
};

struct GBASIORFUFifo {
	struct GBASIORFUPacket packets[GBA_RFU_PACKET_DEPTH];
	uint8_t head;
	uint8_t count;
};

/* Only these mailboxes cross emulator threads. They are protected by the
 * coordinator mutex; all adapter and serial state belongs to its own core. */
struct GBASIORFUMessage {
	uint8_t type;
	uint8_t source;
	uint8_t slot;
	uint8_t bytes;
	uint16_t sourceDevice;
	uint16_t destinationDevice;
	uint32_t txEpoch;
	uint32_t sourceSession;
	uint32_t targetSession;
	uint64_t radioTicket;
	uint64_t readinessGeneration;
	uint64_t txRevision;
	int64_t relativeCycles;
	uint8_t data[GBA_RFU_MAX_BYTES];
};

struct GBASIORFUClient {
	/* Registration survives loss of signal until an explicit disconnect. */
	bool active;
	uint8_t peer;
	uint16_t device;
	uint16_t frames;
	uint32_t radioSession;
	struct GBASIORFUFifo rx;
};

struct GBASIORFUBroadcast {
	bool valid;
	uint8_t ttl;
	uint16_t device;
	uint32_t radioSession;
	uint32_t data[6];
};

/* Published/claimed adapter snapshots only; never peer CPU or timing state.
 * Publication, claiming and withdrawal linearize under the coordinator mutex. */
struct GBASIORFUPrearmed {
	bool published;
	bool claimed;
	bool adopted;
	bool accepted;
	bool released;
	bool cancelled;
	uint64_t generation;
	struct GBASIORFUMessage ready;
	struct GBASIORFUMessage offer;
};

struct GBASIORFUCoordinator {
	struct GBASIORFUDriver* players[MAX_GBAS];
	int nAttached;
	uint32_t nextRadioSession;
	uint64_t nextReadinessGeneration;
	struct GBASIORFUPrearmed prearmed[MAX_GBAS];
	/* External monotonic service clock; zero leaves cooperative yielding off. */
	uint64_t cooperativeNowUs;
	/* Owner-published registration identity, not peer adapter state reads. */
	uint32_t cooperativeSessions[MAX_GBAS];
	Mutex mutex;
};

struct GBASIORFUDriver {
	struct GBASIODriver d;
	struct GBASIORFUCoordinator* coordinator;
	int playerId;
	enum GBASIOMode mode;

	enum GBASIORFUState state;
	enum GBASIORFURole role;
	struct mTimingEvent clockEvent;
	struct mTimingEvent adapterEvent;
	struct mTimingEvent pollEvent;
	struct mTimingEvent maintenanceEvent;
	bool internalPending;
	bool adapterPending;
	uint32_t transferWord;
	uint32_t adapterWord;
	uint8_t adapterIndex;
	uint16_t previousSIOCNT;
	uint16_t previousRCNT;
	uint32_t previousWord;
	uint8_t command;
	uint8_t sendWordsExpected;
	uint8_t sendWordsReceived;
	uint32_t payload[32];

	uint8_t responseWords;
	uint8_t responseIndex;
	uint32_t response[32];
	uint8_t errorCode;
	uint32_t asyncWords[3];
	uint8_t asyncCount;
	uint8_t asyncIndex;
	uint8_t timeoutFrames;
	uint8_t retryCount;
	uint32_t waitCycles;
	uint32_t retryCycles;
	uint32_t connectCycles;
	uint16_t generation;
	uint16_t deviceId;
	uint16_t hostDevice;
	uint8_t hostPeer;
	uint8_t clientNumber;
	bool accepting;
	uint8_t broadcastFrames;
	bool hasBroadcast;
	uint32_t broadcast[6];
	struct GBASIORFUBroadcast peerBroadcast[MAX_GBAS];
	struct GBASIORFUClient clients[4];
	struct GBASIORFUFifo rx;
	struct GBASIORFUPacket tx;
	/* Client TX is staged until the host initiates the next RF exchange. */
	bool clientTxPending;
	/* Send-and-wait requires an exchange after staging, even for zero TX. */
	bool clientWaitNeedsExchange;
	/* RF receipt is distinct from unread application data in the RX buffers.
	 * The epoch is retained across resets so delayed ACKs cannot match a
	 * subsequent transmission from the same adapter instance. */
	uint32_t txEpoch;
	uint8_t txTargetMask;
	uint8_t txAckMask;
	/* Optional, owner-thread-only two-player radio rendezvous. Mailbox
	 * messages contain snapshots; no peer CPU/timing state is inspected. */
	bool synchronize;
	bool prearmed;
	uint64_t prearmedGeneration;
	uint64_t prearmedLastGeneration;
	uint64_t clientTxRevision;
	uint64_t radioReplyRevision;
	uint32_t radioSession;
	uint32_t hostRadioSession;
	uint32_t radioLastTime;
	int64_t radioClock;
	int64_t radioArrival;
	/* Owner-local clock when this ticket's paired READY is posted/accepted. */
	int64_t radioMatchedAt;
	int64_t radioDue;
	int64_t radioDeadline;
	uint64_t radioNextTicket;
	uint64_t radioTicket;
	uint64_t radioLastTicket;
	bool radioPending;
	bool radioOfferPosted;
	bool radioReady;
	bool radioMatched;
	bool radioReleased;
	bool radioNotice;
	bool radioQueued;
	bool radioTimedOut;
	struct GBASIORFUMessage radioOffer;
	struct GBASIORFUMessage radioReply;
	struct GBASIORFUMessage radioQueuedOffer;
	struct GBASIORFUMessage incoming[GBA_RFU_MESSAGE_DEPTH];
	uint8_t incomingHead;
	uint8_t incomingCount;
	uint16_t multiSend;
	/* Scheduler metadata only: mutable fields use coordinator.mutex. The hook
	 * pointer is configured with the owner stopped and immutable while running.
	 * The service thread never reads owner protocol, CPU or timing state. */
	struct mLockstepUser* cooperativeUser;
	bool cooperativeAsleep;
	bool cooperativeBypass;
	/* A peer lifecycle invalidates this command even before its first park. */
	bool cooperativePeerRetired;
	uint64_t cooperativeGeneration;
	uint64_t cooperativeWaitGeneration;
	uint64_t cooperativeWaitTicket;
	uint64_t cooperativeDeadlineUs;

	char logPath[1024];
	/* Bounded owner-local event capture; disk I/O is explicit, never in SIO. */
	char* logBuffer;
	size_t logHead;
	size_t logSize;
	uint64_t logSequence;
	uint64_t logDropped;
};

void GBASIORFUCoordinatorInit(struct GBASIORFUCoordinator*);
void GBASIORFUCoordinatorDeinit(struct GBASIORFUCoordinator*);
/* Call externally about every 5 ms, with monotonically increasing microseconds.
 * Only wakes thread hooks, never accesses a peer core. A parked WAIT has a
 * fixed 50 ms wall-time budget; expiry resumes ordinary emulated timeouts. */
void GBASIORFUCoordinatorService(struct GBASIORFUCoordinator*, uint64_t nowUs);

void GBASIORFUCoordinatorAttach(struct GBASIORFUCoordinator*, struct GBASIORFUDriver*);
void GBASIORFUCoordinatorDetach(struct GBASIORFUCoordinator*, struct GBASIORFUDriver*);
size_t GBASIORFUCoordinatorAttached(struct GBASIORFUCoordinator*);

void GBASIORFUDriverCreate(struct GBASIORFUDriver*);
/* Call on the owner thread or with the core stopped. Path changes discard
 * unflushed capture without I/O; flush explicitly first to preserve it.
 * NULL disables capture and frees its storage. Enable allocates once here.
 * Reset/deinit do not flush: they can also run inside serial callbacks. */
void GBASIORFUDriverSetLogPath(struct GBASIORFUDriver*, const char*);
bool GBASIORFUDriverFlushLog(struct GBASIORFUDriver*);
/* Configure before attaching/initializing the peripheral. Default: false. */
void GBASIORFUDriverSetSynchronize(struct GBASIORFUDriver*, bool);
/* Optional published-readiness handoff. Configure before attach; default false.
 * Only explicit client readiness with no pending/queued radio ticket is published. */
void GBASIORFUDriverSetPrearmed(struct GBASIORFUDriver*, bool);
/* Optional owner-thread cooperative WAIT yielding. Configure before attach or
 * with the owner stopped. NULL (default) disables it. Hooks must be nonblocking;
 * their storage must outlive attachment and must not reenter the coordinator. */
void GBASIORFUDriverSetCooperative(struct GBASIORFUDriver*, struct mLockstepUser*);

CXX_GUARD_END

#endif
