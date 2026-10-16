/* Protocol-level regression tests for the actual mGBA RFU + SIO + timing code.
 * Only the CPU IRQ receiver, unrelated GBP peripheral, and logging are stubbed.
 * No ROM, UI, production-only test API, or test implementation of RFU is used.
 */
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/io.h>
#include <mgba/internal/gba/sio/rfu.h>
#include <mgba/core/lockstep.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define COUNT_OF(x) (sizeof(x) / sizeof((x)[0]))
#define NORMAL_IRQ 0x5000
#define CLOCK_INTERNAL 0x0001
#define CLOCK_FAST 0x0002
#define SO_HIGH 0x0008
#define TRANSFER_START 0x0080
#define CPU_HZ 16777216U
#define FRAME_CYCLES (CPU_HZ / 60)

struct TestGBA {
	struct GBA gba;
	struct GBASIORFUDriver rfu;
	int32_t relativeCycles;
	int32_t nextEvent;
	unsigned irqs;
	uint32_t irqWord;
	uint32_t irqTime;
};

static struct TestGBA* machines[MAX_GBAS];
static unsigned machineCount;
static struct GBASIORFUCoordinator coordinator;
static unsigned assertions;
static unsigned testCases;
static const char* currentTest;

/* The separate native-owner stress runner shares this fixture. Its owners
 * assert concurrently, so only that runner needs an atomic observation count. */
static void countAssertion(void) {
#ifdef RFU_STRESS_THREADS
	__atomic_fetch_add(&assertions, 1U, __ATOMIC_RELAXED);
#else
	++assertions;
#endif
}

static void fail(const char* expression, int line) {
	fprintf(stderr, "FAIL [%s] line %d: %s\n", currentTest, line, expression);
	exit(1);
}

#define CHECK(x) do { countAssertion(); if (!(x)) fail(#x, __LINE__); } while (0)
#define EQ(a, b) do { uint32_t got_ = (uint32_t) (a); uint32_t want_ = (uint32_t) (b); \
	countAssertion(); if (got_ != want_) { fprintf(stderr, "got=%08X expected=%08X\n", got_, want_); fail(#a " == " #b, __LINE__); } } while (0)

/* IRQ is the observation boundary: RX must be committed and busy cleared first. */
void GBARaiseIRQ(struct GBA* gba, enum GBAIRQ irq, uint32_t cyclesLate) {
	(void) cyclesLate;
	CHECK(irq == GBA_IRQ_SIO);
	CHECK(!(gba->sio.siocnt & TRANSFER_START));
	for (unsigned i = 0; i < machineCount; ++i) {
		if (&machines[i]->gba == gba) {
			struct TestGBA* machine = machines[i];
			++machine->irqs;
			machine->irqWord = (uint32_t) gba->memory.io[GBA_REG(SIODATA32_LO)] |
				((uint32_t) gba->memory.io[GBA_REG(SIODATA32_HI)] << 16);
			machine->irqTime = mTimingCurrentTime(&gba->timing);
			return;
		}
	}
	fail("IRQ belongs to an active test GBA", __LINE__);
}

void GBASIOPlayerInit(struct GBASIOPlayer* player) { (void) player; }
void GBASIOPlayerReset(struct GBASIOPlayer* player) { (void) player; }
void GBAInterrupt(struct GBA* gba) {
	/* Same owner-local timing-queue escape as the real core; no CPU/ROM runs. */
	gba->earlyExit = true;
	mTimingInterrupt(&gba->timing);
}
int mLogGenerateCategory(const char* name, const char* id) { (void) name; (void) id; return 1; }
void mLog(int category, enum mLogLevel level, const char* format, ...) {
	(void) category; (void) level; (void) format;
}
const char* const GBAIORegisterNames[512] = {0};

static void beginConfigured(const char* name, unsigned count, bool prearmed) {
	currentTest = name;
	++testCases;
	machineCount = count;
	GBASIORFUCoordinatorInit(&coordinator);
	for (unsigned i = 0; i < count; ++i) {
		machines[i] = calloc(1, sizeof(*machines[i]));
		CHECK(machines[i] != NULL);
		struct TestGBA* machine = machines[i];
		machine->nextEvent = INT_MAX;
		mTimingInit(&machine->gba.timing, &machine->relativeCycles, &machine->nextEvent);
		machine->gba.sio.p = &machine->gba;
		GBASIOInit(&machine->gba.sio);
		GBASIORFUDriverCreate(&machine->rfu);
		if (prearmed) GBASIORFUDriverSetPrearmed(&machine->rfu, true);
		GBASIORFUCoordinatorAttach(&coordinator, &machine->rfu);
		GBASIOSetDriver(&machine->gba.sio, &machine->rfu.d);
		GBASIOWriteRCNT(&machine->gba.sio, 0);
	}
}

static void begin(const char* name, unsigned count) {
	beginConfigured(name, count, false);
}

static void beginPrearmed(const char* name) {
	beginConfigured(name, 2, true);
}

static void end(void) {
	for (unsigned i = 0; i < machineCount; ++i) {
		GBASIORFUCoordinatorDetach(&coordinator, &machines[i]->rfu);
		GBASIODeinit(&machines[i]->gba.sio);
		GBASIORFUDriverSetLogPath(&machines[i]->rfu, NULL);
		mTimingDeinit(&machines[i]->gba.timing);
		free(machines[i]);
		machines[i] = NULL;
	}
	GBASIORFUCoordinatorDeinit(&coordinator);
	machineCount = 0;
	printf("PASS %s\n", currentTest);
}

/* Advance real mTiming queues for all GBAs, keeping their emulated clocks aligned. */
static void advance(unsigned cycles) {
	unsigned guard = 0;
	while (cycles) {
		CHECK(++guard < 1000000);
		unsigned step = cycles;
		for (unsigned i = 0; i < machineCount; ++i) {
			int32_t next = mTimingNextEvent(&machines[i]->gba.timing);
			if (next <= 0) {
				step = 0;
			} else if ((unsigned) next < step) {
				step = next;
			}
		}
		for (unsigned i = 0; i < machineCount; ++i) {
			mTimingTick(&machines[i]->gba.timing, step);
		}
		cycles -= step;
	}
}

/* Synchronization tests intentionally run one adapter clock at a time. */
static void advanceOne(unsigned id, unsigned cycles) {
	CHECK(id < machineCount);
	struct mTiming* timing = &machines[id]->gba.timing;
	unsigned guard = 0;
	while (cycles) {
		CHECK(++guard < 1000000);
		int32_t next = mTimingNextEvent(timing);
		unsigned step = cycles;
		if (next <= 0) {
			step = 0;
		} else if ((unsigned) next < step) {
			step = next;
		}
		mTimingTick(timing, step);
		cycles -= step;
	}
}

/* Move the stopped test core's epoch, preserving every queued local deadline. */
static void moveClock(unsigned id, uint32_t newTime) {
	struct mTiming* timing = &machines[id]->gba.timing;
	CHECK(timing->reroot == NULL);
	CHECK(machines[id]->relativeCycles == 0);
	const uint32_t delta = newTime - timing->masterCycles;
	for (struct mTimingEvent* event = timing->root; event; event = event->next) {
		event->when += delta;
	}
	timing->masterCycles = newTime;
	timing->globalCycles += delta;
}

static void countTimerEvent(struct mTiming* timing, void* context, uint32_t cyclesLate) {
	(void) timing;
	(void) cyclesLate;
	++*(unsigned*) context;
}

static uint32_t rx(unsigned id) {
	struct GBA* gba = &machines[id]->gba;
	return (uint32_t) gba->memory.io[GBA_REG(SIODATA32_LO)] |
		((uint32_t) gba->memory.io[GBA_REG(SIODATA32_HI)] << 16);
}

static void writeWord(unsigned id, uint32_t word) {
	machines[id]->gba.memory.io[GBA_REG(SIODATA32_LO)] = word;
	machines[id]->gba.memory.io[GBA_REG(SIODATA32_HI)] = word >> 16;
}

static uint32_t transfer(unsigned id, uint32_t word, bool fast) {
	struct GBASIO* sio = &machines[id]->gba.sio;
	uint16_t value = NORMAL_IRQ | CLOCK_INTERNAL | SO_HIGH | (fast ? CLOCK_FAST : 0);
	unsigned irqs = machines[id]->irqs;
	writeWord(id, word);
	GBASIOWriteSIOCNT(sio, value);
	GBASIOWriteSIOCNT(sio, value | TRANSFER_START);
	CHECK(!mTimingIsScheduled(&machines[id]->gba.timing, &sio->completeEvent));
	CHECK(sio->siocnt & TRANSFER_START);
	unsigned duration = fast ? 256 : 2048;
	advance(duration - 1);
	EQ(machines[id]->irqs, irqs);
	CHECK(sio->siocnt & TRANSFER_START);
	advance(1);
	EQ(machines[id]->irqs, irqs + 1);
	CHECK(!(sio->siocnt & TRANSFER_START));
	EQ(rx(id), machines[id]->irqWord);
	GBASIOWriteSIOCNT(sio, NORMAL_IRQ | CLOCK_INTERNAL | (fast ? CLOCK_FAST : 0));
	return rx(id);
}

static void handshake(unsigned id) {
	const uint32_t sequence[] = {0x0000494E, 0x494E544E, 0x544E4E45, 0x4E45444F, 0x444F8001, 0xB0BB8001};
	uint32_t previous = 0;
	for (unsigned i = 0; i < COUNT_OF(sequence); ++i) {
		uint32_t expected = i ? (sequence[i] << 16) | ((~previous) & 0xFFFF) : 0;
		EQ(transfer(id, sequence[i], true), expected);
		previous = sequence[i];
	}
}

static unsigned command(unsigned id, uint8_t cmd, const uint32_t* words, unsigned count, uint32_t* response) {
	EQ(transfer(id, 0x99660000 | (count << 8) | cmd, true), 0x80000000);
	for (unsigned i = 0; i < count; ++i) {
		EQ(transfer(id, words[i], true), 0x80000000);
	}
	uint32_t header = transfer(id, 0x80000000, true);
	EQ(header & 0xFFFF00FF, 0x99660080 | cmd);
	unsigned responseCount = (header >> 8) & 0xFF;
	CHECK(responseCount <= 32);
	for (unsigned i = 0; i < responseCount; ++i) {
		uint32_t word = transfer(id, 0x80000000, true);
		if (response) response[i] = word;
	}
	return responseCount;
}

/* Issue the same real SPI command while the other owner gets no CPU turn.
 * Ordinary command() intentionally keeps both clocks advancing; using it here
 * would accidentally let the recipient process the OFFER before reproducing
 * the frontend scheduling gap this fixture is intended to cover. */
static uint32_t transferOne(unsigned id, uint32_t word) {
	struct GBASIO* sio = &machines[id]->gba.sio;
	const uint16_t value = NORMAL_IRQ | CLOCK_INTERNAL | SO_HIGH | CLOCK_FAST;
	const unsigned irqs = machines[id]->irqs;
	writeWord(id, word);
	GBASIOWriteSIOCNT(sio, value);
	GBASIOWriteSIOCNT(sio, value | TRANSFER_START);
	CHECK(!mTimingIsScheduled(&machines[id]->gba.timing, &sio->completeEvent));
	CHECK(sio->siocnt & TRANSFER_START);
	advanceOne(id, 255);
	EQ(machines[id]->irqs, irqs);
	CHECK(sio->siocnt & TRANSFER_START);
	advanceOne(id, 1);
	EQ(machines[id]->irqs, irqs + 1);
	CHECK(!(sio->siocnt & TRANSFER_START));
	EQ(rx(id), machines[id]->irqWord);
	GBASIOWriteSIOCNT(sio, NORMAL_IRQ | CLOCK_INTERNAL | CLOCK_FAST);
	return rx(id);
}

static unsigned commandOne(unsigned id, uint8_t cmd, const uint32_t* words, unsigned count, uint32_t* response) {
	EQ(transferOne(id, 0x99660000 | (count << 8) | cmd), 0x80000000);
	for (unsigned i = 0; i < count; ++i) EQ(transferOne(id, words[i]), 0x80000000);
	const uint32_t header = transferOne(id, 0x80000000);
	EQ(header & 0xFFFF00FF, 0x99660080 | cmd);
	const unsigned responseCount = (header >> 8) & 0xFF;
	CHECK(responseCount <= 32);
	for (unsigned i = 0; i < responseCount; ++i) {
		const uint32_t word = transferOne(id, 0x80000000);
		if (response) response[i] = word;
	}
	return responseCount;
}

static void connectPair(void) {
	uint32_t response[32];
	const uint32_t settings = 0x0432; /* 50-frame timeout, four retries */
	const uint32_t broadcast[] = {0x10203040, 0x50607080, 0x90A0B0C0, 0xD0E0F000, 0x01020304, 0x05060708};
	for (unsigned i = 0; i < 2; ++i) {
		handshake(i);
		EQ(command(i, 0x17, &settings, 1, NULL), 0);
	}
	EQ(command(0, 0x16, broadcast, 6, NULL), 0);
	EQ(command(0, 0x19, NULL, 0, NULL), 0);
	advance(FRAME_CYCLES * 2);
	EQ(command(1, 0x1C, NULL, 0, NULL), 0);
	EQ(command(1, 0x1D, NULL, 0, response), 7);
	for (unsigned i = 0; i < 6; ++i) EQ(response[i + 1], broadcast[i]);
	uint32_t hostId = response[0] & 0xFFFF;
	CHECK(hostId != 0);
	EQ(command(1, 0x1F, &hostId, 1, NULL), 0);
	advance(FRAME_CYCLES * 2);
	EQ(command(1, 0x20, NULL, 0, response), 1);
	CHECK((response[0] & 0xFFFF) != 0);
	CHECK(!(response[0] & 0xFF000000));
	EQ(command(1, 0x21, NULL, 0, response), 1);
	EQ(command(0, 0x1A, NULL, 0, response), 1);
}

static uint32_t asynchronousReceive(unsigned id, uint16_t extra) {
	struct TestGBA* machine = machines[id];
	unsigned previousIrqs = machine->irqs;
	/* GBA signals busy then ready; SI is read-only and follows these SO edges. */
	GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | SO_HIGH | extra);
	GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | extra);
	GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | TRANSFER_START | extra);
	CHECK(!mTimingIsScheduled(&machine->gba.timing, &machine->gba.sio.completeEvent));
	for (unsigned count = 0; machine->irqs == previousIrqs && count < 20000; ++count) advance(256);
	EQ(machine->irqs, previousIrqs + 1);
	return rx(id);
}

static void testHandshakeAndTiming(void) {
	begin("Nintendo handshake, exact internal speed durations, IRQ commit order", 1);
	handshake(0);
	uint32_t response[32];
	EQ(command(0, 0x12, NULL, 0, response), 1);
	EQ(response[0], 0x00830117);
	EQ(transfer(0, 0x99660012, false), 0x80000000);
	EQ(transfer(0, 0x80000000, false), 0x99660192);
	EQ(transfer(0, 0x80000000, false), 0x00830117);
	unsigned irqs = machines[0]->irqs;
	advance(4096);
	EQ(machines[0]->irqs, irqs);
	end();
}

static void testExternalClockOwnership(void) {
	begin("External start is not generically completed", 1);
	handshake(0);
	struct TestGBA* machine = machines[0];
	unsigned irqs = machine->irqs;
	writeWord(0, 0xDEADBEEF);
	GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | CLOCK_FAST | TRANSFER_START);
	CHECK(!mTimingIsScheduled(&machine->gba.timing, &machine->gba.sio.completeEvent));
	advance(FRAME_CYCLES);
	EQ(machine->irqs, irqs);
	EQ(rx(0), 0xDEADBEEF);
	CHECK(machine->gba.sio.siocnt & TRANSFER_START);
	end();
}

static void testPackets(void) {
	begin("Latest unread RX wins and only host sends exchange staged client data", 2);
	connectPair();
	uint32_t response[32];
	EQ(command(0, 0x26, NULL, 0, response), 1);
	EQ(response[0], 0);
	EQ(command(1, 0x26, NULL, 0, response), 1);
	EQ(response[0], 0);
	for (unsigned i = 0; i < 3; ++i) {
		uint32_t clientPacket[] = {4 << 8, 0xB0000000 | i};
		EQ(command(1, 0x24, clientPacket, 2, NULL), 0);
	}
	advance(FRAME_CYCLES);
	EQ(command(0, 0x26, NULL, 0, response), 1);
	EQ(response[0], 0); /* Client sends stage data; time alone cannot deliver it. */
	for (unsigned i = 0; i < 3; ++i) {
		uint32_t hostPacket[] = {4, 0xA0000000 | i};
		EQ(command(0, 0x24, hostPacket, 2, NULL), 0);
	}
	advance(FRAME_CYCLES);
	EQ(command(1, 0x26, NULL, 0, response), 2);
	EQ(response[0], 4);
	EQ(response[1], 0xA0000002); /* Earlier unread host packets were overwritten. */
	EQ(command(0, 0x26, NULL, 0, response), 2);
	EQ(response[0], 4 << 8);
	EQ(response[1], 0xB0000002); /* Last scheduled client packet exchanged once. */
	EQ(command(0, 0x26, NULL, 0, response), 1);
	EQ(response[0], 0);
	EQ(command(1, 0x26, NULL, 0, response), 1);
	EQ(response[0], 0);
	/* Exercise the real RFU maximums: a 90-byte host payload and 16-byte client payload. */
	uint32_t maximumHost[24] = {90};
	uint32_t maximumClient[5] = {16 << 8, 0x01020304, 0x05060708, 0x090A0B0C, 0x0D0E0F10};
	for (unsigned i = 1; i < COUNT_OF(maximumHost); ++i) maximumHost[i] = 0x76540000 | i;
	EQ(command(1, 0x24, maximumClient, COUNT_OF(maximumClient), NULL), 0);
	EQ(command(0, 0x24, maximumHost, COUNT_OF(maximumHost), NULL), 0);
	advance(FRAME_CYCLES);
	EQ(command(1, 0x26, NULL, 0, response), 24);
	EQ(response[0], 90);
	for (unsigned i = 1; i < 23; ++i) EQ(response[i], maximumHost[i]);
	EQ(response[23] & 0xFFFF, maximumHost[23] & 0xFFFF);
	EQ(command(0, 0x26, NULL, 0, response), 5);
	EQ(response[0], 16 << 8);
	for (unsigned i = 1; i < 5; ++i) EQ(response[i], maximumClient[i]);
	end();
}

static void testAsyncReadiness(void) {
	begin("Adapter responses require armed external clock, SI/SO low; bit1 may be set", 2);
	connectPair();
	uint32_t hostPacket[] = {4, 0xCAFEBABE};
	EQ(command(0, 0x24, hostPacket, 2, NULL), 0);
	EQ(command(1, 0x27, NULL, 0, NULL), 0);
	struct TestGBA* client = machines[1];
	unsigned irqs = client->irqs;
	advance(FRAME_CYCLES);
	EQ(client->irqs, irqs); /* no armed receive */
	GBASIOWriteSIOCNT(&client->gba.sio, NORMAL_IRQ | SO_HIGH | TRANSFER_START | CLOCK_FAST);
	advance(FRAME_CYCLES);
	EQ(client->irqs, irqs); /* external but SO/SI high */
	GBASIOWriteSIOCNT(&client->gba.sio, NORMAL_IRQ | SO_HIGH | CLOCK_FAST);
	/* gpSP completes an internal-clock transaction with a dummy word here;
	 * it must not consume the queued adapter-master notification. */
	EQ(transfer(1, 0x80000000, true), 0x80000000);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	irqs = client->irqs;
	advance(4096);
	EQ(client->irqs, irqs); /* exactly one IRQ per async word */
	uint32_t response[32];
	EQ(command(1, 0x26, NULL, 0, response), 2);
	EQ(response[0], 4);
	EQ(response[1], 0xCAFEBABE);
	end();
}

static void testWaitTimeoutAndDisconnect(void) {
	begin("Configured wait timeout and peer disconnect produce adapter notifications", 2);
	connectPair();
	uint32_t settings = 0x0401; /* one-frame client wait timeout */
	EQ(command(1, 0x17, &settings, 1, NULL), 0);
	EQ(command(1, 0x27, NULL, 0, NULL), 0);
	advance(FRAME_CYCLES * 2);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660027);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(command(1, 0x27, NULL, 0, NULL), 0);
	GBASIORFUCoordinatorDetach(&coordinator, &machines[0]->rfu);
	advance(FRAME_CYCLES);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660129);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x0000000F);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	end();
}

static void testSendAndWait(void) {
	begin("0x25 send-and-wait reverses clocks and exchanges real data both ways", 2);
	connectPair();
	uint32_t hostPacket[] = {4, 0x10203040};
	uint32_t clientPacket[] = {4 << 8, 0x50607080};
	uint32_t replyPacket[] = {4, 0x90A0B0C0};
	uint32_t response[32];
	EQ(command(0, 0x25, hostPacket, 2, NULL), 0);
	EQ(command(1, 0x27, NULL, 0, NULL), 0);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(command(1, 0x26, NULL, 0, response), 2);
	EQ(response[0], 4);
	EQ(response[1], hostPacket[1]);
	EQ(command(1, 0x25, clientPacket, 2, NULL), 0);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	EQ(command(0, 0x24, replyPacket, 2, NULL), 0); /* Host frame clocks the pending client reply. */
	EQ(command(0, 0x26, NULL, 0, response), 2);
	EQ(response[0], 4 << 8);
	EQ(response[1], clientPacket[1]);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(command(1, 0x26, NULL, 0, response), 2);
	EQ(response[1], replyPacket[1]);
	end();
}

static void testHostRetryNotification(void) {
	begin("Missing MAC ACK waits for retry period without declaring live slot inactive", 2);
	connectPair();
	mTimingClear(&machines[1]->gba.timing); /* Pause adapter service, preserving registration. */
	const uint32_t settings = 0x0120; /* one retry, 32-frame outer timeout */
	const uint32_t zeroBytePayload = 0;
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	EQ(command(0, 0x25, &zeroBytePayload, 1, NULL), 0);
	struct TestGBA* host = machines[0];
	unsigned irqs = host->irqs;
	GBASIOWriteSIOCNT(&host->gba.sio, NORMAL_IRQ | SO_HIGH | CLOCK_FAST);
	GBASIOWriteSIOCNT(&host->gba.sio, NORMAL_IRQ | CLOCK_FAST);
	GBASIOWriteSIOCNT(&host->gba.sio, NORMAL_IRQ | CLOCK_FAST | TRANSFER_START);
	advance(FRAME_CYCLES / 12);
	EQ(host->irqs, irqs);
	advance(FRAME_CYCLES / 6);
	EQ(host->irqs, irqs + 1);
	EQ(rx(0), 0x99660128);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x00000000);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	end();
}

static void testDisconnectedFallbacks(void) {
	begin("NORMAL8 and MULTI complete through disconnected peripheral fallback", 2);
	/* Two configured RFUs must not pretend to be a cable-connected pair. */
	struct TestGBA* machine = machines[1];
	struct GBASIO* sio = &machine->gba.sio;
	GBASIOWriteSIOCNT(sio, 0x6000);
	EQ(sio->mode, GBA_SIO_MULTI);
	EQ(machine->rfu.d.connectedDevices(&machine->rfu.d), 0);
	EQ(machine->rfu.d.deviceId(&machine->rfu.d), 0);
	machine->gba.memory.io[GBA_REG(SIOMLT_SEND)] = 0xBEEF;
	unsigned irqs = machine->irqs;
	GBASIOWriteSIOCNT(sio, 0x6080);
	CHECK(mTimingIsScheduled(&machine->gba.timing, &sio->completeEvent));
	advance(31975);
	EQ(machine->irqs, irqs);
	CHECK(sio->siocnt & TRANSFER_START);
	advance(1);
	EQ(machine->irqs, irqs + 1);
	EQ(machine->gba.memory.io[GBA_REG(SIOMULTI0)], 0xBEEF);
	EQ(machine->gba.memory.io[GBA_REG(SIOMULTI1)], 0xFFFF);
	EQ(machine->gba.memory.io[GBA_REG(SIOMULTI2)], 0xFFFF);
	EQ(machine->gba.memory.io[GBA_REG(SIOMULTI3)], 0xFFFF);
	CHECK(!(sio->siocnt & TRANSFER_START));
	GBASIOWriteSIOCNT(sio, 0x4003); /* NORMAL8, 2 MHz internal clock, IRQ */
	EQ(sio->mode, GBA_SIO_NORMAL_8);
	EQ(machine->rfu.d.connectedDevices(&machine->rfu.d), 0);
	machine->gba.memory.io[GBA_REG(SIODATA8)] = 0xAB;
	irqs = machine->irqs;
	GBASIOWriteSIOCNT(sio, 0x4083);
	CHECK(mTimingIsScheduled(&machine->gba.timing, &sio->completeEvent));
	advance(63);
	EQ(machine->irqs, irqs);
	advance(1);
	EQ(machine->irqs, irqs + 1);
	EQ(machine->gba.memory.io[GBA_REG(SIODATA8)], 0xFF);
	CHECK(!(sio->siocnt & TRANSFER_START));
	end();
}

static void expectShortPacket(unsigned recipient, unsigned bytes, uint32_t data) {
	uint32_t response[32];
	EQ(command(recipient, 0x26, NULL, 0, response), bytes ? 2 : 1);
	EQ(response[0], recipient == 0 ? bytes << 8 : bytes);
	if (bytes) {
		uint32_t mask = bytes == 4 ? UINT32_MAX : (1U << (bytes * 8)) - 1;
		EQ(response[1], data & mask);
	}
}

static void testGhostSendBuffers(void) {
	begin("Header-only ghost sends preserve prior 1-4 bytes on host and client", 2);
	connectPair();
	/* A fresh adapter's retained TX data is deterministically initialized to zero. */
	uint32_t hostHeader = 3;
	uint32_t clientHeader = 3 << 8;
	EQ(command(0, 0x24, &hostHeader, 1, NULL), 0);
	EQ(command(1, 0x24, &clientHeader, 1, NULL), 0);
	EQ(command(0, 0x24, &hostHeader, 1, NULL), 0);
	expectShortPacket(1, 3, 0);
	expectShortPacket(0, 3, 0);
	const uint32_t hostSeed[] = {4, 0x44332211};
	const uint32_t clientSeed[] = {4 << 8, 0xA1B2C3D4};
	EQ(command(1, 0x24, clientSeed, 2, NULL), 0);
	EQ(command(0, 0x24, hostSeed, 2, NULL), 0);
	expectShortPacket(1, 4, hostSeed[1]);
	expectShortPacket(0, 4, clientSeed[1]);
	for (unsigned bytes = 0; bytes <= 4; ++bytes) {
		hostHeader = bytes;
		clientHeader = bytes << 8;
		EQ(command(1, 0x24, &clientHeader, 1, NULL), 0);
		EQ(command(0, 0x24, &hostHeader, 1, NULL), 0);
		expectShortPacket(1, bytes, hostSeed[1]);
		expectShortPacket(0, bytes, clientSeed[1]);
	}
	end();
}

static void testGhostSendAndWait(void) {
	begin("Observed one-parameter 0x25 ghost send ACKs and enters normal wait", 2);
	connectPair();
	const uint32_t hostSeed[] = {4, 0x44332211};
	const uint32_t clientSeed[] = {4 << 8, 0xA1B2C3D4};
	EQ(command(1, 0x24, clientSeed, 2, NULL), 0);
	EQ(command(0, 0x24, hostSeed, 2, NULL), 0);
	expectShortPacket(1, 4, hostSeed[1]);
	expectShortPacket(0, 4, clientSeed[1]);
	const uint32_t hostHeader = 1;
	EQ(command(0, 0x25, &hostHeader, 1, NULL), 0); /* Mystery Gift host pattern */
	EQ(command(1, 0x27, NULL, 0, NULL), 0);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	expectShortPacket(1, 1, hostSeed[1]);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	/* Client ghost reply is pending until the next host frame clocks it. */
	const uint32_t clientHeader = 1 << 8;
	EQ(command(1, 0x25, &clientHeader, 1, NULL), 0);
	EQ(command(0, 0x24, &hostHeader, 1, NULL), 0);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	expectShortPacket(0, 1, clientSeed[1]);
	expectShortPacket(1, 1, hostSeed[1]);
	end();
}

static void expectInvalidSend(unsigned sender, uint8_t cmd, const uint32_t* payload, unsigned count) {
	EQ(transfer(sender, 0x99660000 | (count << 8) | cmd, true), 0x80000000);
	for (unsigned i = 0; i < count; ++i) EQ(transfer(sender, payload[i], true), 0x80000000);
	EQ(transfer(sender, 0x80000000, true), 0x996601EE);
	EQ(transfer(sender, 0x80000000, true), 2);
	/* Error responses leave the adapter ready to accept a subsequent command. */
	uint32_t response[32];
	EQ(command(sender, 0x12, NULL, 0, response), 1);
	EQ(response[0], 0x00830117);
}

static void testGhostSendValidation(void) {
	begin("Ghost exception retains oversized and regular-underflow validation", 2);
	connectPair();
	const uint32_t hostSeed[] = {4, 0x44332211};
	const uint32_t clientSeed[] = {4 << 8, 0xA1B2C3D4};
	EQ(command(1, 0x24, clientSeed, 2, NULL), 0);
	EQ(command(0, 0x24, hostSeed, 2, NULL), 0);
	expectShortPacket(1, 4, hostSeed[1]);
	expectShortPacket(0, 4, clientSeed[1]);
	uint32_t hostOversize[24] = {91};
	uint32_t clientOversize[6] = {17 << 8};
	uint32_t hostUnderflow[] = {5, 0x76543210};
	uint32_t clientUnderflow[] = {5 << 8, 0x76543210};
	uint32_t hostGhostOversize = 5;
	uint32_t clientGhostOversize = 5 << 8;
	for (uint8_t cmd = 0x24; cmd <= 0x25; ++cmd) {
		expectInvalidSend(0, cmd, &hostGhostOversize, 1);
		expectInvalidSend(1, cmd, &clientGhostOversize, 1);
		expectInvalidSend(0, cmd, hostUnderflow, COUNT_OF(hostUnderflow));
		expectInvalidSend(1, cmd, clientUnderflow, COUNT_OF(clientUnderflow));
		expectInvalidSend(0, cmd, hostOversize, COUNT_OF(hostOversize));
		expectInvalidSend(1, cmd, clientOversize, COUNT_OF(clientOversize));
	}
	uint32_t hostHeader = 4;
	uint32_t clientHeader = 4 << 8;
	EQ(command(1, 0x24, &clientHeader, 1, NULL), 0);
	EQ(command(0, 0x24, &hostHeader, 1, NULL), 0);
	expectShortPacket(1, 4, hostSeed[1]);
	expectShortPacket(0, 4, clientSeed[1]);
	end();
}

static void prepareAdapterResponse(void) {
	connectPair();
	const uint32_t packet[] = {4, 0x76543210};
	EQ(command(0, 0x24, packet, 2, NULL), 0);
	EQ(command(1, 0x27, NULL, 0, NULL), 0);
}

static void armAdapterTransfer(unsigned id, uint16_t speed) {
	struct TestGBA* machine = machines[id];
	struct GBASIO* sio = &machine->gba.sio;
	const unsigned irqs = machine->irqs;
	const unsigned asyncIndex = machine->rfu.asyncIndex;
	writeWord(id, 0xCCDDEEFF);
	GBASIOWriteSIOCNT(sio, NORMAL_IRQ | SO_HIGH | speed);
	GBASIOWriteSIOCNT(sio, NORMAL_IRQ | speed);
	GBASIOWriteSIOCNT(sio, NORMAL_IRQ | TRANSFER_START | speed);
	for (unsigned cycles = 0; !machine->rfu.adapterPending && cycles < 1024; ++cycles) {
		advance(1);
		EQ(machine->irqs, irqs);
		EQ(machine->rfu.asyncIndex, asyncIndex);
		EQ(rx(id), 0xCCDDEEFF);
		CHECK(sio->siocnt & TRANSFER_START);
	}
	CHECK(machine->rfu.adapterPending);
	CHECK(mTimingIsScheduled(&machine->gba.timing, &machine->rfu.adapterEvent));
	CHECK(!mTimingIsScheduled(&machine->gba.timing, &sio->completeEvent));
	EQ(mTimingUntil(&machine->gba.timing, &machine->rfu.adapterEvent), 256);
}

static void exactAdapterWord(unsigned id, uint16_t speed, uint32_t expected) {
	struct TestGBA* machine = machines[id];
	const unsigned irqs = machine->irqs;
	const unsigned index = machine->rfu.asyncIndex;
	armAdapterTransfer(id, speed);
	const unsigned started = mTimingCurrentTime(&machine->gba.timing);
	advance(255);
	EQ(machine->irqs, irqs);
	EQ(machine->rfu.asyncIndex, index);
	EQ(rx(id), 0xCCDDEEFF);
	CHECK(machine->gba.sio.siocnt & TRANSFER_START);
	CHECK(machine->rfu.adapterPending);
	advance(1);
	EQ(machine->irqs, irqs + 1);
	EQ(machine->irqTime, started + 256);
	EQ(machine->rfu.asyncIndex, index + 1);
	EQ(rx(id), expected);
	EQ(machine->irqWord, expected);
	CHECK(!(machine->gba.sio.siocnt & TRANSFER_START));
	CHECK(!machine->rfu.adapterPending);
}

static void testExactAdapterTiming(void) {
	for (unsigned fast = 0; fast < 2; ++fast) {
		begin(fast ? "Adapter 256-cycle completion with console speed bit set" :
			"Adapter 256-cycle completion with console speed bit clear", 2);
		prepareAdapterResponse();
		const uint16_t speed = fast ? CLOCK_FAST : 0;
		exactAdapterWord(1, speed, 0x99660028);
		exactAdapterWord(1, speed, 0x80000000);
		const unsigned irqs = machines[1]->irqs;
		advance(1024);
		EQ(machines[1]->irqs, irqs);
		expectShortPacket(1, 4, 0x76543210);
		end();
	}
}

static void testAdapterCancellation(void) {
	static const char* const cases[] = {
		"External event cancellation on clearing Start retains queued word",
		"External event cancellation on clock ownership change retains queued word",
		"External event cancellation on mode change retains queued word",
		"External event cancellation on reset discards prior queued response",
		"External event cancellation on deinit prevents stale interrupt"
	};
	for (unsigned variant = 0; variant < COUNT_OF(cases); ++variant) {
		begin(cases[variant], 2);
		prepareAdapterResponse();
		struct TestGBA* client = machines[1];
		struct GBASIO* sio = &client->gba.sio;
		armAdapterTransfer(1, CLOCK_FAST);
		const unsigned irqs = client->irqs;
		advance(100); /* Cancel after the transfer really starts, before its wire deadline. */
		const unsigned remaining = mTimingUntil(&client->gba.timing, &client->rfu.adapterEvent);
		EQ(remaining, 156);
		switch (variant) {
		case 0:
			GBASIOWriteSIOCNT(sio, NORMAL_IRQ | CLOCK_FAST);
			break;
		case 1:
			/* Start remains set, but the GBA now owns the clock. Its dummy transaction is legitimate. */
			GBASIOWriteSIOCNT(sio, NORMAL_IRQ | CLOCK_INTERNAL | CLOCK_FAST | TRANSFER_START);
			break;
		case 2:
			GBASIOWriteRCNT(sio, 0x8000); /* GPIO mode, not the RFU reset pin. */
			break;
		case 3:
			GBASIOReset(sio);
			break;
		case 4:
			GBASIOSetDriver(sio, NULL);
			break;
		}
		CHECK(!client->rfu.adapterPending);
		CHECK(!mTimingIsScheduled(&client->gba.timing, &client->rfu.adapterEvent));
		advance(remaining + 1);
		EQ(client->irqs, irqs); /* Original adapter deadline must not fire. */
		EQ(rx(1), 0xCCDDEEFF);
		if (variant == 1) {
			advance(256 - remaining - 1);
			EQ(client->irqs, irqs + 1);
			EQ(rx(1), 0x80000000);
		} else {
			advance(512);
			EQ(client->irqs, irqs);
		}
		EQ(client->rfu.asyncIndex, 0);
		if (variant < 3) {
			EQ(client->rfu.asyncWords[0], 0x99660028);
			if (variant == 2) GBASIOWriteRCNT(sio, 0);
			exactAdapterWord(1, CLOCK_FAST, 0x99660028);
			exactAdapterWord(1, CLOCK_FAST, 0x80000000);
			expectShortPacket(1, 4, 0x76543210);
		} else if (variant == 3) {
			EQ(client->rfu.asyncCount, 0);
			EQ(client->rfu.state, GBA_RFU_RESET);
			GBASIOWriteRCNT(sio, 0);
			GBASIOWriteSIOCNT(sio, NORMAL_IRQ | CLOCK_FAST | TRANSFER_START);
			advance(1024);
			EQ(client->irqs, irqs);
		} else {
			CHECK(sio->driver == NULL);
			EQ(client->rfu.asyncWords[0], 0x99660028);
			CHECK(!mTimingIsScheduled(&client->gba.timing, &client->rfu.pollEvent));
			CHECK(!mTimingIsScheduled(&client->gba.timing, &client->rfu.clockEvent));
		}
		end();
	}
}

static void testGenericCompletionModeChange(void) {
	for (unsigned multi = 0; multi < 2; ++multi) {
		begin(multi ? "Stale MULTI completion cannot affect a new RFU transfer" :
			"Stale NORMAL8 completion cannot interrupt a new RFU transfer", 1);
		struct TestGBA* machine = machines[0];
		struct GBASIO* sio = &machine->gba.sio;
		const uint16_t fallback = multi ? 0x6000 : 0x4003;
		GBASIOWriteSIOCNT(sio, fallback);
		GBASIOWriteSIOCNT(sio, fallback | TRANSFER_START);
		CHECK(mTimingIsScheduled(&machine->gba.timing, &sio->completeEvent));
		const unsigned oldDeadline = multi ? 31976 : 64;
		EQ(mTimingUntil(&machine->gba.timing, &sio->completeEvent), oldDeadline);
		/* Switch while the cable/dummy transfer is still in flight, like adapter detection. */
		writeWord(0, 0x0000494E);
		GBASIOWriteSIOCNT(sio, NORMAL_IRQ | CLOCK_INTERNAL | CLOCK_FAST | SO_HIGH);
		GBASIOWriteSIOCNT(sio, NORMAL_IRQ | CLOCK_INTERNAL | CLOCK_FAST | SO_HIGH | TRANSFER_START);
		if (!multi) {
			advance(63);
			EQ(machine->irqs, 0);
			advance(1);
			EQ(machine->irqs, 0); /* The retired 8-bit timer must not run as NORMAL32. */
			EQ(rx(0), 0x0000494E);
			CHECK(sio->siocnt & TRANSFER_START);
			advance(191);
		} else {
			advance(255);
		}
		EQ(machine->irqs, 0);
		EQ(rx(0), 0x0000494E);
		CHECK(sio->siocnt & TRANSFER_START);
		advance(1);
		EQ(machine->irqs, 1);
		EQ(machine->irqTime, 256);
		EQ(rx(0), 0);
		CHECK(!(sio->siocnt & TRANSFER_START));
		CHECK(!mTimingIsScheduled(&machine->gba.timing, &sio->completeEvent));
		advance(oldDeadline + 512);
		EQ(machine->irqs, 1); /* No late callback from the retired MULTI transfer either. */
		end();
	}
}

static void connectThirdClient(void) {
	uint32_t response[32];
	const uint32_t settings = 0x0432;
	handshake(2);
	EQ(command(2, 0x17, &settings, 1, NULL), 0);
	advance(FRAME_CYCLES * 2);
	EQ(command(2, 0x1C, NULL, 0, NULL), 0);
	EQ(command(2, 0x1D, NULL, 0, response), 7);
	const uint32_t host = response[0];
	EQ(command(2, 0x1F, &host, 1, NULL), 0);
	advance(FRAME_CYCLES * 2);
	EQ(command(2, 0x20, NULL, 0, response), 1);
	EQ(response[0] >> 16, 1);
	EQ(command(2, 0x21, NULL, 0, response), 1);
	EQ(command(0, 0x1A, NULL, 0, response), 2);
}

static void expectHostStatus(uint32_t mask) {
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x99660128);
	EQ(asynchronousReceive(0, CLOCK_FAST), mask);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
}

static void testAutomaticMacAck(void) {
	begin("Host send-and-wait completes on receipt ACK without client application data", 2);
	connectPair();
	const uint32_t ghost = 1;
	EQ(command(0, 0x25, &ghost, 1, NULL), 0);
	advance(FRAME_CYCLES * 2); /* The client adapter receives while its console runs no commands. */
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	uint32_t response[32];
	EQ(command(0, 0x26, NULL, 0, response), 1);
	EQ(response[0], 0); /* Completion came from MAC receipt, not fabricated client payload. */
	expectShortPacket(1, 1, 0);
	end();
}

static void testPartialMacAck(void) {
	begin("Partial transmission status reports only actual receipt and no premature inactivity", 3);
	connectPair();
	connectThirdClient();
	mTimingClear(&machines[2]->gba.timing); /* Slot 1 is registered but temporarily unresponsive. */
	const uint32_t ghost = 1;
	EQ(command(0, 0x25, &ghost, 1, NULL), 0);
	advance(FRAME_CYCLES * 2);
	expectHostStatus(0x00000001); /* Slot 0 received; slot 1 is not yet inactive. */
	uint32_t response[32];
	EQ(command(0, 0x1A, NULL, 0, response), 2);
	EQ(command(0, 0x11, NULL, 0, response), 1);
	EQ(response[0], 0x0000FFFF);
	end();
}

/* Model transport delay using a real ACK produced by the client, without a test-only driver API. */
static struct GBASIORFUMessage holdActualAck(uint32_t epoch) {
	struct GBASIORFUDriver* host = &machines[0]->rfu;
	struct GBASIORFUMessage held = {0};
	bool found = false;
	MutexLock(&coordinator.mutex);
	for (unsigned i = 0; i < host->incomingCount; ++i) {
		const unsigned index = (host->incomingHead + i) % GBA_RFU_MESSAGE_DEPTH;
		if (host->incoming[index].txEpoch == epoch) {
			held = host->incoming[index];
			found = true;
			break;
		}
	}
	/* The host has no other incoming application traffic in this isolated case. */
	host->incomingCount = 0;
	host->incomingHead = 0;
	MutexUnlock(&coordinator.mutex);
	CHECK(found);
	return held;
}

static void deliverHeldAck(const struct GBASIORFUMessage* message) {
	struct GBASIORFUDriver* host = &machines[0]->rfu;
	MutexLock(&coordinator.mutex);
	CHECK(host->incomingCount < GBA_RFU_MESSAGE_DEPTH);
	const unsigned tail = (host->incomingHead + host->incomingCount) % GBA_RFU_MESSAGE_DEPTH;
	host->incoming[tail] = *message;
	++host->incomingCount;
	MutexUnlock(&coordinator.mutex);
}

static void testRetransmitAcceptsDelayedAck(void) {
	begin("Host 37 preserves the logical TX epoch and accepts its delayed genuine ACK", 2);
	connectPair();
	const uint32_t settings = 0;
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	mTimingDeschedule(&machines[0]->gba.timing, &machines[0]->rfu.maintenanceEvent);
	mTimingClear(&machines[1]->gba.timing);
	const uint32_t packet[] = {4, 0x11223344};
	EQ(command(0, 0x24, packet, 2, NULL), 0);
	const uint32_t epoch = machines[0]->rfu.txEpoch;
	uint32_t response[32];
	EQ(command(1, 0x12, NULL, 0, response), 1);
	const struct GBASIORFUMessage held = holdActualAck(epoch);
	mTimingClear(&machines[1]->gba.timing); /* Withhold the retry's fresh ACK entirely. */
	EQ(command(0, 0x37, NULL, 0, NULL), 0);
	EQ(machines[0]->rfu.txEpoch, epoch);
	EQ(machines[0]->rfu.txTargetMask, 1);
	EQ(machines[0]->rfu.txAckMask, 0);
	struct TestGBA* host = machines[0];
	const unsigned irqs = host->irqs;
	GBASIOWriteSIOCNT(&host->gba.sio, NORMAL_IRQ | SO_HIGH | CLOCK_FAST);
	GBASIOWriteSIOCNT(&host->gba.sio, NORMAL_IRQ | CLOCK_FAST);
	GBASIOWriteSIOCNT(&host->gba.sio, NORMAL_IRQ | CLOCK_FAST | TRANSFER_START);
	advance(1024);
	EQ(host->irqs, irqs);
	deliverHeldAck(&held); /* It acknowledged this same packet before 37 retransmitted it. */
	advance(1024);
	EQ(host->irqs, irqs + 1);
	EQ(rx(0), 0x99660028);
	EQ(host->rfu.txAckMask, 1);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	expectShortPacket(1, 4, packet[1]); /* The retained packet was still physically posted on retry. */
	end();
}

static void testRetransmitPreservesPartialAck(void) {
	begin("Host 37 preserves partial receipt masks but new 24/25 packets reset them", 3);
	connectPair();
	connectThirdClient();
	mTimingClear(&machines[2]->gba.timing);
	const uint32_t settings = 0x0132; /* A finite one-retry budget exposes partial receipt status. */
	const uint32_t packet[] = {4, 0x55667788};
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	EQ(command(0, 0x25, packet, 2, NULL), 0);
	advance(FRAME_CYCLES * 2); /* Let slot 0's normal adapter service receive the first packet. */
	expectHostStatus(1);
	const uint32_t epoch = machines[0]->rfu.txEpoch;
	EQ(machines[0]->rfu.txAckMask, 1);
	mTimingClear(&machines[1]->gba.timing);
	EQ(command(0, 0x37, NULL, 0, NULL), 0);
	EQ(machines[0]->rfu.txEpoch, epoch);
	EQ(machines[0]->rfu.txTargetMask, 3);
	EQ(machines[0]->rfu.txAckMask, 1);
	expectHostStatus(1); /* No peer served the retry; slot 0's earlier receipt must survive. */
	EQ(machines[0]->rfu.txAckMask, 1);
	EQ(command(0, 0x24, packet, 2, NULL), 0); /* Even identical bytes are a new SendData transaction. */
	CHECK(machines[0]->rfu.txEpoch != epoch);
	EQ(machines[0]->rfu.txAckMask, 0);
	const uint32_t epoch24 = machines[0]->rfu.txEpoch;
	EQ(command(0, 0x25, packet, 2, NULL), 0);
	CHECK(machines[0]->rfu.txEpoch != epoch24);
	EQ(machines[0]->rfu.txAckMask, 0);
	end();
}

static void testRetransmitMasksRemovedPeer(void) {
	for (unsigned remote = 0; remote < 2; ++remote) {
		begin(remote ? "Host 37 excludes remotely disconnected peer from preserved ACK/target masks" :
			"Host 37 excludes explicitly removed peer from preserved ACK/target masks", 3);
		connectPair();
		connectThirdClient();
		const uint32_t packet[] = {4, 0xABCDEF12};
		uint32_t response[32];
		EQ(command(0, 0x24, packet, 2, NULL), 0);
		advance(FRAME_CYCLES * 2);
		EQ(command(0, 0x12, NULL, 0, response), 1);
		EQ(machines[0]->rfu.txAckMask, 3);
		const uint32_t epoch = machines[0]->rfu.txEpoch;
		const uint32_t slot1 = 2;
		EQ(command(remote ? 2 : 0, 0x30, &slot1, 1, NULL), 0);
		advance(FRAME_CYCLES);
		EQ(command(0, 0x12, NULL, 0, response), 1);
		CHECK(!machines[0]->rfu.clients[1].active);
		EQ(machines[0]->rfu.txAckMask, 1);
		EQ(machines[0]->rfu.txTargetMask, 1);
		mTimingClear(&machines[1]->gba.timing); /* Completion must use the ACK already received. */
		EQ(command(0, 0x37, NULL, 0, NULL), 0);
		EQ(machines[0]->rfu.txEpoch, epoch);
		EQ(machines[0]->rfu.txAckMask, 1);
		EQ(machines[0]->rfu.txTargetMask, 1);
		EQ(asynchronousReceive(0, CLOCK_FAST), 0x99660028);
		EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
		end();
	}
}

static void testStaleMacAck(void) {
	begin("Delayed ACK of prior transmission cannot complete current send-and-wait", 2);
	connectPair();
	const uint32_t settings = 0xFF32; /* Keep the finite retry deadline beyond this observation window. */
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	mTimingDeschedule(&machines[0]->gba.timing, &machines[0]->rfu.maintenanceEvent);
	mTimingClear(&machines[1]->gba.timing);
	const uint32_t packetA[] = {4, 0x11223344};
	EQ(command(0, 0x24, packetA, 2, NULL), 0);
	const uint32_t epochA = machines[0]->rfu.txEpoch;
	uint32_t response[32];
	EQ(command(1, 0x12, NULL, 0, response), 1); /* Client handles A and produces its genuine MAC ACK. */
	const struct GBASIORFUMessage oldAck = holdActualAck(epochA);
	mTimingClear(&machines[1]->gba.timing);
	const uint32_t earlierClientData[] = {4 << 8, 0xDEADBEEF};
	EQ(command(1, 0x24, earlierClientData, 2, NULL), 0);
	const uint32_t exchangeGhost = 1;
	EQ(command(0, 0x24, &exchangeGhost, 1, NULL), 0);
	EQ(command(1, 0x12, NULL, 0, response), 1); /* This host frame exchanges staged older app data. */
	mTimingClear(&machines[1]->gba.timing);
	const uint32_t packetB[] = {4, 0x55667788};
	EQ(command(0, 0x25, packetB, 2, NULL), 0);
	CHECK(machines[0]->rfu.txEpoch != epochA);
	CHECK(machines[0]->rfu.clients[0].rx.count != 0); /* Old application RX must not complete this TX. */
	struct GBASIORFUMessage wrongDeviceAck = oldAck;
	wrongDeviceAck.sourceDevice ^= 1;
	struct TestGBA* host = machines[0];
	mTimingSchedule(&host->gba.timing, &host->rfu.maintenanceEvent, FRAME_CYCLES);
	const unsigned irqs = host->irqs;
	GBASIOWriteSIOCNT(&host->gba.sio, NORMAL_IRQ | SO_HIGH | CLOCK_FAST);
	GBASIOWriteSIOCNT(&host->gba.sio, NORMAL_IRQ | CLOCK_FAST);
	GBASIOWriteSIOCNT(&host->gba.sio, NORMAL_IRQ | CLOCK_FAST | TRANSFER_START);
	advance(FRAME_CYCLES * 2);
	EQ(host->irqs, irqs);
	EQ(host->rfu.txAckMask, 0);
	const unsigned previousFrames = host->rfu.clients[0].frames;
	CHECK(previousFrames > 0);
	deliverHeldAck(&wrongDeviceAck);
	advance(1024);
	EQ(host->irqs, irqs);
	EQ(host->rfu.txAckMask, 0);
	CHECK(host->rfu.clients[0].frames >= previousFrames); /* Wrong prior-session identity proves no liveness. */
	CHECK(host->gba.sio.siocnt & TRANSFER_START);
	deliverHeldAck(&oldAck);
	advance(1024);
	EQ(host->irqs, irqs);
	EQ(host->rfu.txAckMask, 0);
	CHECK(host->rfu.clients[0].frames <= 1); /* Valid old-epoch ACK proves liveness, not current receipt. */
	/* Now allow B to reach the client adapter. Only B's ACK may finish the host wait. */
	mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent, 0);
	advance(1024);
	EQ(host->irqs, irqs + 1);
	EQ(rx(0), 0x99660028);
	EQ(host->rfu.txAckMask, 1);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	expectShortPacket(0, 4, earlierClientData[1]);
	end();
}

static void testZeroWaitLimits(void) {
	for (unsigned clientWait = 0; clientWait < 2; ++clientWait) {
		begin(clientWait ? "Zero client wait timeout never expires" : "Zero host retry/timeout limits remain unlimited", 2);
		connectPair();
		const unsigned id = clientWait ? 1 : 0;
		const uint32_t settings = 0;
		EQ(command(id, 0x17, &settings, 1, NULL), 0);
		if (clientWait) {
			EQ(command(1, 0x27, NULL, 0, NULL), 0);
		} else {
			mTimingClear(&machines[1]->gba.timing);
			const uint32_t ghost = 1;
			EQ(command(0, 0x25, &ghost, 1, NULL), 0);
		}
		struct TestGBA* machine = machines[id];
		const unsigned irqs = machine->irqs;
		GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | SO_HIGH | CLOCK_FAST);
		GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | CLOCK_FAST);
		GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | CLOCK_FAST | TRANSFER_START);
		advance(FRAME_CYCLES * 12);
		EQ(machine->irqs, irqs);
		CHECK(machine->gba.sio.siocnt & TRANSFER_START);
		CHECK(!machine->rfu.adapterPending);
		end();
	}
}

static void testInactiveClientRecovery(void) {
	begin("Inactivity occurs at 240 frames while registration survives and current ACK restores signal", 2);
	connectPair();
	mTimingClear(&machines[1]->gba.timing);
	const uint32_t settings = 0;
	const uint32_t ghost = 1;
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	EQ(command(0, 0x24, &ghost, 1, NULL), 0);
	struct TestGBA* host = machines[0];
	const unsigned irqs = host->irqs;
	while (host->rfu.clients[0].frames < 239) {
		int32_t until = mTimingUntil(&host->gba.timing, &host->rfu.maintenanceEvent);
		CHECK(until > 0);
		advance(until);
		EQ(host->irqs, irqs);
		CHECK(host->rfu.clients[0].active);
	}
	EQ(host->rfu.clients[0].frames, 239);
	advance(mTimingUntil(&host->gba.timing, &host->rfu.maintenanceEvent));
	EQ(host->rfu.clients[0].frames, 240);
	CHECK(host->rfu.clients[0].active);
	uint32_t response[32];
	EQ(command(0, 0x1A, NULL, 0, response), 1);
	EQ(command(0, 0x11, NULL, 0, response), 1);
	EQ(response[0], 0);
	const uint32_t finiteRetry = 0x0132;
	EQ(command(0, 0x17, &finiteRetry, 1, NULL), 0);
	EQ(command(0, 0x25, &ghost, 1, NULL), 0);
	expectHostStatus(0x00000100); /* Only actual slot 0 is inactive, not all four slots. */
	/* The suspended peer now receives the current TX and returns its valid receipt ACK. */
	mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent, 0);
	advance(1024);
	EQ(command(0, 0x11, NULL, 0, response), 1);
	EQ(response[0], 0xFF);
	EQ(host->rfu.txAckMask, 1);
	CHECK(host->rfu.clients[0].frames < 240);
	EQ(command(0, 0x1A, NULL, 0, response), 1);
	end();
}

static void testAckWithFullFifoAndZeroData(void) {
	begin("Unread RX replacement and zero-byte host frames still receive MAC ACKs", 2);
	connectPair();
	for (unsigned packet = 0; packet < 4; ++packet) {
		const uint32_t data[] = {4, 0x87654300 | packet};
		EQ(command(0, 0x24, data, 2, NULL), 0);
		advance(FRAME_CYCLES * 2);
	}
	EQ(machines[1]->rfu.rx.count, 1);
	const uint32_t latestPacket[] = {4, 0xABCDEF12};
	EQ(command(0, 0x25, latestPacket, 2, NULL), 0);
	advance(FRAME_CYCLES * 2);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	EQ(machines[1]->rfu.rx.count, 1);
	const uint32_t zeroBytes = 0;
	EQ(command(0, 0x25, &zeroBytes, 1, NULL), 0);
	advance(FRAME_CYCLES * 2);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	EQ(command(0, 0x37, NULL, 0, NULL), 0); /* Retransmit the retained zero-byte frame. */
	advance(FRAME_CYCLES * 2);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	EQ(machines[1]->rfu.rx.count, 1);
	expectShortPacket(1, 4, latestPacket[1]); /* Zero-byte frames do not erase older nonzero RX. */
	end();
}

static void testDocumentedHostDrivenExchange(void) {
	begin("Documented host-driven exchange retains old RX while newer client TX is only staged", 2);
	connectPair();
	const uint32_t client10[] = {4 << 8, 10};
	const uint32_t client20[] = {4 << 8, 20};
	const uint32_t host1[] = {4, 1};
	const uint32_t host2[] = {4, 2};
	const uint32_t host3[] = {4, 3};
	const uint32_t host4[] = {4, 4};
	EQ(command(1, 0x24, client10, 2, NULL), 0);
	advance(FRAME_CYCLES * 2);
	expectShortPacket(0, 0, 0);
	EQ(command(0, 0x24, host1, 2, NULL), 0); /* This exchange receives the scheduled 10. */
	advance(FRAME_CYCLES);
	EQ(command(0, 0x24, host2, 2, NULL), 0);
	advance(FRAME_CYCLES);
	expectShortPacket(1, 4, 2); /* Unread 1 was overwritten. */
	EQ(command(1, 0x24, client20, 2, NULL), 0);
	advance(FRAME_CYCLES * 2);
	expectShortPacket(0, 4, 10); /* New scheduled 20 cannot replace already received 10 yet. */
	expectShortPacket(0, 0, 0);
	EQ(command(0, 0x24, host3, 2, NULL), 0);
	advance(FRAME_CYCLES);
	expectShortPacket(0, 4, 20);
	expectShortPacket(0, 0, 0);
	EQ(command(0, 0x24, host4, 2, NULL), 0);
	advance(FRAME_CYCLES);
	expectShortPacket(0, 0, 0); /* The pending client data was exchanged exactly once. */
	expectShortPacket(1, 4, 4);
	end();
}

static void testStagedRetransmitAndZero(void) {
	begin("Client latest TX wins, explicit retransmit rearms once, and zero send clears pending", 2);
	connectPair();
	const uint32_t clientA[] = {4 << 8, 0x12345678};
	const uint32_t clientB[] = {4 << 8, 0x90ABCDEF};
	const uint32_t hostGhost = 1;
	const uint32_t clientZero = 0;
	EQ(command(1, 0x24, clientA, 2, NULL), 0);
	EQ(command(1, 0x24, clientB, 2, NULL), 0);
	CHECK(machines[1]->rfu.clientTxPending);
	advance(FRAME_CYCLES * 2);
	expectShortPacket(0, 0, 0);
	EQ(command(0, 0x24, &hostGhost, 1, NULL), 0);
	advance(FRAME_CYCLES);
	CHECK(!machines[1]->rfu.clientTxPending);
	expectShortPacket(0, 4, clientB[1]);
	EQ(command(0, 0x24, &hostGhost, 1, NULL), 0);
	advance(FRAME_CYCLES);
	expectShortPacket(0, 0, 0);
	EQ(command(1, 0x37, NULL, 0, NULL), 0);
	CHECK(machines[1]->rfu.clientTxPending);
	EQ(command(0, 0x24, &hostGhost, 1, NULL), 0);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	expectShortPacket(0, 4, clientB[1]);
	expectShortPacket(0, 0, 0);
	EQ(command(1, 0x24, clientA, 2, NULL), 0);
	CHECK(machines[1]->rfu.clientTxPending);
	EQ(command(1, 0x24, &clientZero, 1, NULL), 0);
	CHECK(!machines[1]->rfu.clientTxPending);
	EQ(command(0, 0x24, &hostGhost, 1, NULL), 0);
	advance(FRAME_CYCLES);
	expectShortPacket(0, 0, 0);
	end();
}

static unsigned armClientExchangeWait(void) {
	struct TestGBA* client = machines[1];
	const unsigned irqs = client->irqs;
	writeWord(1, 0xCCDDEEFF);
	GBASIOWriteSIOCNT(&client->gba.sio, NORMAL_IRQ | SO_HIGH | CLOCK_FAST);
	GBASIOWriteSIOCNT(&client->gba.sio, NORMAL_IRQ | CLOCK_FAST);
	GBASIOWriteSIOCNT(&client->gba.sio, NORMAL_IRQ | CLOCK_FAST | TRANSFER_START);
	advance(FRAME_CYCLES);
	EQ(client->irqs, irqs);
	EQ(rx(1), 0xCCDDEEFF);
	CHECK(client->gba.sio.siocnt & TRANSFER_START);
	CHECK(!client->rfu.adapterPending);
	CHECK(client->rfu.clientWaitNeedsExchange);
	EQ(client->rfu.state, GBA_RFU_WAIT);
	return irqs;
}

static void finishClientExchangeWait(unsigned previousIrqs) {
	advance(FRAME_CYCLES);
	EQ(machines[1]->irqs, previousIrqs + 1);
	EQ(rx(1), 0x99660028);
	CHECK(!machines[1]->rfu.clientWaitNeedsExchange);
	CHECK(!machines[1]->rfu.clientTxPending);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
}

static void testUnreadRxCannotFinishNewClientSendWait(void) {
	static const char* const cases[] = {
		"Unread old host RX cannot finish client 25 before staged TX exchanges",
		"Zero-byte client 25 still needs a new host exchange, including a zero-byte host frame",
		"Headerless client 25 still needs a new host exchange",
		"Unread old host RX cannot finish client 37 before retained TX exchanges"
	};
	for (unsigned variant = 0; variant < COUNT_OF(cases); ++variant) {
		begin(cases[variant], 2);
		connectPair();
		const uint32_t oldHost[] = {4, 0x11223344};
		const uint32_t newHost[] = {4, 0x55667788};
		const uint32_t clientData[] = {4 << 8, 0xA1B2C3D4};
		const uint32_t zero = 0;
		EQ(command(0, 0x24, oldHost, 2, NULL), 0);
		advance(FRAME_CYCLES);
		EQ(machines[1]->rfu.rx.count, 1); /* Deliberately leave the earlier RX unread. */
		switch (variant) {
		case 0: EQ(command(1, 0x25, clientData, 2, NULL), 0); break;
		case 1: EQ(command(1, 0x25, &zero, 1, NULL), 0); break;
		case 2: EQ(command(1, 0x25, NULL, 0, NULL), 0); break;
		case 3:
			EQ(command(1, 0x24, clientData, 2, NULL), 0);
			EQ(command(1, 0x37, NULL, 0, NULL), 0);
			break;
		}
		EQ(machines[1]->rfu.clientTxPending, variant == 0 || variant == 3);
		CHECK(machines[1]->rfu.clientWaitNeedsExchange);
		const unsigned irqs = armClientExchangeWait();
		expectShortPacket(0, 0, 0); /* Waiting alone cannot transmit the staged response. */
		if (variant == 1) {
			EQ(command(0, 0x24, &zero, 1, NULL), 0);
		} else {
			EQ(command(0, 0x24, newHost, 2, NULL), 0);
		}
		finishClientExchangeWait(irqs);
		expectShortPacket(1, 4, variant == 1 ? oldHost[1] : newHost[1]);
		expectShortPacket(0, variant == 0 || variant == 3 ? 4 : 0, clientData[1]);
		expectShortPacket(0, 0, 0);
		end();
	}
}

static void testHostFrameBeforeClientCommandCompletion(void) {
	begin("Host frame received during client 25 payload cannot satisfy the ensuing new wait", 2);
	connectPair();
	const uint32_t earlierHost[] = {4, 0x0BADF00D};
	const uint32_t laterHost[] = {4, 0x87654321};
	const uint32_t clientData = 0xDEADBEEF;
	EQ(transfer(1, 0x99660225, true), 0x80000000);
	EQ(machines[1]->rfu.state, GBA_RFU_PAYLOAD);
	EQ(command(0, 0x24, earlierHost, 2, NULL), 0);
	EQ(transfer(1, 4 << 8, true), 0x80000000);
	EQ(machines[1]->rfu.rx.count, 1);
	CHECK(!machines[1]->rfu.clientWaitNeedsExchange);
	EQ(transfer(1, clientData, true), 0x80000000);
	CHECK(machines[1]->rfu.clientTxPending);
	CHECK(machines[1]->rfu.clientWaitNeedsExchange);
	EQ(transfer(1, 0x80000000, true), 0x996600A5);
	const unsigned irqs = armClientExchangeWait();
	expectShortPacket(0, 0, 0);
	EQ(command(0, 0x24, laterHost, 2, NULL), 0);
	finishClientExchangeWait(irqs);
	expectShortPacket(0, 4, clientData);
	expectShortPacket(1, 4, laterHost[1]);
	end();
}

static void testClientExchangeGuardCleared(void) {
	static const char* const cases[] = {
		"Remote disconnect clears the client send-wait exchange guard",
		"Reset clears the client send-wait exchange guard",
		"Deinit clears the client send-wait exchange guard"
	};
	for (unsigned variant = 0; variant < COUNT_OF(cases); ++variant) {
		begin(cases[variant], 2);
		connectPair();
		const uint32_t clientData[] = {4 << 8, 0x12345678};
		const uint32_t slot0 = 1;
		EQ(command(1, 0x25, clientData, 2, NULL), 0);
		CHECK(machines[1]->rfu.clientWaitNeedsExchange);
		CHECK(machines[1]->rfu.clientTxPending);
		switch (variant) {
		case 0: EQ(command(0, 0x30, &slot0, 1, NULL), 0); break;
		case 1: GBASIOReset(&machines[1]->gba.sio); break;
		case 2: GBASIOSetDriver(&machines[1]->gba.sio, NULL); break;
		}
		advance(FRAME_CYCLES);
		CHECK(!machines[1]->rfu.clientWaitNeedsExchange);
		CHECK(!machines[1]->rfu.clientTxPending);
		EQ(machines[1]->rfu.rx.count, 0);
		end();
	}
}

static void reconnectClearedClient(bool resetHandshake) {
	if (resetHandshake) {
		GBASIOWriteRCNT(&machines[1]->gba.sio, 0);
		handshake(1);
	}
	/* A reset forgets cached broadcasts; wait for a fresh normal host announcement. */
	advance(FRAME_CYCLES * 32);
	uint32_t response[32];
	EQ(command(1, 0x1C, NULL, 0, NULL), 0);
	EQ(command(1, 0x1D, NULL, 0, response), 7);
	const uint32_t host = response[0];
	EQ(command(1, 0x1F, &host, 1, NULL), 0);
	advance(FRAME_CYCLES * 2);
	EQ(command(1, 0x20, NULL, 0, response), 1);
	CHECK((response[0] & 0xFFFF) != 0 && !(response[0] & 0xFF000000));
	EQ(command(1, 0x21, NULL, 0, response), 1);
}

static void testPendingBuffersCleared(void) {
	static const char* const cases[] = {
		"Local disconnect clears staged TX, unread RX, and retained data",
		"Remote disconnect clears staged TX, unread RX, and retained data",
		"Reset clears staged TX, unread RX, and retained data",
		"Deinit clears staged TX, unread RX, and retained data"
	};
	for (unsigned variant = 0; variant < COUNT_OF(cases); ++variant) {
		begin(cases[variant], 2);
		connectPair();
		const uint32_t hostSeed[] = {4, 0x0BADF00D};
		const uint32_t clientSeed[] = {4 << 8, 0xDEADBEEF};
		const uint32_t slot0 = 1;
		EQ(command(0, 0x24, hostSeed, 2, NULL), 0);
		EQ(command(1, 0x24, clientSeed, 2, NULL), 0);
		CHECK(machines[1]->rfu.clientTxPending);
		EQ(machines[1]->rfu.rx.count, 1);
		switch (variant) {
		case 0: EQ(command(1, 0x30, &slot0, 1, NULL), 0); break;
		case 1: EQ(command(0, 0x30, &slot0, 1, NULL), 0); break;
		case 2: GBASIOReset(&machines[1]->gba.sio); break;
		case 3: GBASIOSetDriver(&machines[1]->gba.sio, NULL); break;
		}
		advance(FRAME_CYCLES * 2);
		CHECK(!machines[1]->rfu.clientTxPending);
		EQ(machines[1]->rfu.rx.count, 0);
		EQ(machines[1]->rfu.tx.bytes, 0);
		EQ(machines[1]->rfu.role, GBA_RFU_IDLE);
		if (variant == 3) GBASIOSetDriver(&machines[1]->gba.sio, &machines[1]->rfu.d);
		reconnectClearedClient(variant >= 2);
		const uint32_t hostGhost = 4;
		const uint32_t clientGhost = 4 << 8;
		EQ(command(1, 0x24, &clientGhost, 1, NULL), 0);
		EQ(command(0, 0x24, &hostGhost, 1, NULL), 0);
		advance(FRAME_CYCLES);
		expectShortPacket(0, 4, 0); /* Prior-session DEADBEEF must not survive as ghost data. */
		end();
	}
}

static void enableSynchronization(void) {
	for (unsigned id = 0; id < 2; ++id) {
		GBASIORFUDriverSetSynchronize(&machines[id]->rfu, true);
		const uint32_t unlimited = 0;
		EQ(command(id, 0x17, &unlimited, 1, NULL), 0);
	}
}

static void synchronizePair(void) {
	connectPair();
	enableSynchronization();
}

static unsigned armAsyncWithoutAdvancing(unsigned id) {
	struct TestGBA* machine = machines[id];
	const unsigned irqs = machine->irqs;
	writeWord(id, 0xCCDDEEFF);
	GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | SO_HIGH | CLOCK_FAST);
	GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | CLOCK_FAST);
	GBASIOWriteSIOCNT(&machine->gba.sio, NORMAL_IRQ | CLOCK_FAST | TRANSFER_START);
	CHECK(!mTimingIsScheduled(&machine->gba.timing, &machine->gba.sio.completeEvent));
	return irqs;
}

static void awaitAsyncHeader(unsigned id, unsigned previousIrqs, uint32_t expected) {
	for (unsigned polls = 0; machines[id]->irqs == previousIrqs && polls < 40000; ++polls) {
		advance(256);
	}
	EQ(machines[id]->irqs, previousIrqs + 1);
	EQ(rx(id), expected);
	EQ(machines[id]->irqWord, expected);
	CHECK(!(machines[id]->gba.sio.siocnt & TRANSFER_START));
}

static void finishSynchronizedRound(bool hostWait) {
	const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
	unsigned hostIrqs = 0;
	if (hostWait) hostIrqs = armAsyncWithoutAdvancing(0);
	awaitAsyncHeader(1, clientIrqs, 0x99660028);
	if (hostWait) awaitAsyncHeader(0, hostIrqs, 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	if (hostWait) EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	EQ(machines[1]->rfu.state, GBA_RFU_COMMAND);
	if (hostWait) EQ(machines[0]->rfu.state, GBA_RFU_COMMAND);
}

static void testSynchronizedRendezvous(void) {
	static const char* const cases[] = {
		"Synchronized host24/client25 rendezvous requires both ready adapters",
		"Synchronized host25/client25 rendezvous requires both ready adapters",
		"Synchronized rendezvous tolerates unequal local core clocks",
		"Synchronized rendezvous crosses local 32-bit timer wrap"
	};
	for (unsigned variant = 0; variant < COUNT_OF(cases); ++variant) {
		begin(cases[variant], 2);
		connectPair();
		if (variant == 3) {
			moveClock(0, UINT32_MAX - 2047);
			moveClock(1, UINT32_MAX - 1023);
		}
		enableSynchronization();
		if (variant == 2) advanceOne(0, FRAME_CYCLES * 3);
		const bool hostWait = variant != 0;
		const uint32_t hostPacket[] = {4, 0x10203040 | variant};
		const uint32_t clientPacket[] = {4 << 8, 0x50607080 | variant};
		EQ(command(0, hostWait ? 0x25 : 0x24, hostPacket, 2, NULL), 0);
		advance(4096);
		EQ(machines[1]->rfu.rx.count, 0);
		EQ(machines[0]->rfu.clients[0].rx.count, 0);
		EQ(machines[0]->rfu.txAckMask, 0);
		EQ(command(1, 0x25, clientPacket, 2, NULL), 0);
		finishSynchronizedRound(hostWait);
		expectShortPacket(1, 4, hostPacket[1]);
		expectShortPacket(0, 4, clientPacket[1]);
		expectShortPacket(1, 0, 0);
		expectShortPacket(0, 0, 0);
		if (variant == 3) {
			CHECK((uint32_t) mTimingCurrentTime(&machines[0]->gba.timing) < FRAME_CYCLES * 2);
			CHECK((uint32_t) mTimingCurrentTime(&machines[1]->gba.timing) < FRAME_CYCLES * 2);
		}
		end();
	}
}

static void testSynchronizedEmptyRounds(void) {
	for (unsigned headerless = 0; headerless < 2; ++headerless) {
		begin(headerless ? "Synchronized headerless send/wait produces one fresh radio notice" :
			"Synchronized zero-byte send/wait produces one fresh radio notice", 2);
		synchronizePair();
		const uint32_t zero = 0;
		for (unsigned round = 0; round < 3; ++round) {
			const bool hostWait = round != 0;
			EQ(command(0, hostWait ? 0x25 : 0x24, headerless ? NULL : &zero,
				headerless ? 0 : 1, NULL), 0);
			EQ(command(1, 0x25, headerless ? NULL : &zero, headerless ? 0 : 1, NULL), 0);
			finishSynchronizedRound(hostWait);
			EQ(machines[1]->rfu.rx.count, 0);
			EQ(machines[0]->rfu.clients[0].rx.count, 0);
			const unsigned hostIrqs = machines[0]->irqs;
			const unsigned clientIrqs = machines[1]->irqs;
			advance(4096);
			EQ(machines[0]->irqs, hostIrqs);
			EQ(machines[1]->irqs, clientIrqs);
		}
		end();
	}
}

static void testSynchronizedPausedPeerProgress(void) {
	begin("Synchronized paused-peer rendezvous does not freeze unrelated local timers", 2);
		synchronizePair();
		const uint32_t packet[] = {4, 0x12345678};
		EQ(command(0, 0x25, packet, 2, NULL), 0);
		const unsigned irqs = armAsyncWithoutAdvancing(0);
		unsigned timerFirings = 0;
		struct mTimingEvent timer = {0};
		timer.context = &timerFirings;
		timer.callback = countTimerEvent;
		timer.name = "Independent regression timer";
		mTimingSchedule(&machines[0]->gba.timing, &timer, 1024);
		const uint32_t previousTime = mTimingCurrentTime(&machines[0]->gba.timing);
		advanceOne(0, FRAME_CYCLES * 2);
		EQ(timerFirings, 1);
		EQ((uint32_t) mTimingCurrentTime(&machines[0]->gba.timing) - previousTime, FRAME_CYCLES * 2);
		EQ(machines[0]->irqs, irqs);
		CHECK(machines[0]->gba.sio.siocnt & TRANSFER_START);
		EQ(machines[1]->rfu.rx.count, 0);
		const uint32_t client[] = {4 << 8, 0x90ABCDEF};
		EQ(command(1, 0x25, client, 2, NULL), 0);
		/* The host was already armed; avoid arming its completed first word twice. */
		const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
		awaitAsyncHeader(1, clientIrqs, 0x99660028);
		awaitAsyncHeader(0, irqs, 0x99660028);
		EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
		EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
		expectShortPacket(1, 4, packet[1]);
		expectShortPacket(0, 4, client[1]);
		end();
}

static void testSynchronizedFreshWaitAndRetransmit(void) {
	begin("Synchronized new waits need fresh physical tickets while host37 retains logical epoch", 2);
		synchronizePair();
	const uint32_t host[] = {4, 0x0BADF00D};
	const uint32_t client[] = {4 << 8, 0xDEADBEEF};
	EQ(command(0, 0x24, host, 2, NULL), 0);
	EQ(command(1, 0x25, client, 2, NULL), 0);
	finishSynchronizedRound(false);
	const uint32_t epoch = machines[0]->rfu.txEpoch;
	const uint64_t firstTicket = machines[0]->rfu.radioNextTicket;
	CHECK(firstTicket != 0);
	expectShortPacket(0, 4, client[1]);
	EQ(machines[1]->rfu.rx.count, 1); /* Keep the old host data unread. */
	EQ(command(1, 0x27, NULL, 0, NULL), 0);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(machines[0]->rfu.txEpoch, epoch);
	const uint32_t zero = 0;
	EQ(command(1, 0x25, &zero, 1, NULL), 0);
	const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
	advance(4096);
	EQ(machines[1]->irqs, clientIrqs);
	CHECK(machines[1]->gba.sio.siocnt & TRANSFER_START);
	CHECK(machines[1]->rfu.clientWaitNeedsExchange);
	/* Retrying the retained TX is a new physical exchange, not a new logical TX. */
	EQ(command(0, 0x37, NULL, 0, NULL), 0);
	EQ(machines[0]->rfu.txEpoch, epoch);
	CHECK(machines[0]->rfu.radioNextTicket > firstTicket);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	awaitAsyncHeader(1, clientIrqs, 0x99660028);
	awaitAsyncHeader(0, hostIrqs, 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	expectShortPacket(1, 4, host[1]);
	expectShortPacket(0, 0, 0);
	const uint32_t identical[] = {4, host[1]};
	EQ(command(0, 0x24, identical, 2, NULL), 0);
	CHECK(machines[0]->rfu.txEpoch != epoch);
	EQ(command(1, 0x25, client, 2, NULL), 0);
	finishSynchronizedRound(false);
	expectShortPacket(0, 4, client[1]);
	expectShortPacket(1, 4, host[1]);
	end();
}

static void testSynchronizedQueuedOffer(void) {
	begin("Queued host24 ticket survives prior RELEASE and next OFFER in one client drain", 2);
		synchronizePair();
	const uint32_t firstHost[] = {4, 0x11223344};
	const uint32_t secondHost[] = {4, 0x55667788};
	const uint32_t firstClient[] = {4 << 8, 0x90ABCDEF};
	const uint32_t secondClient[] = {4 << 8, 0x10203040};
	EQ(command(0, 0x24, firstHost, 2, NULL), 0);
	const uint64_t firstTicket = machines[0]->rfu.radioTicket;
	CHECK(firstTicket != 0);
	EQ(command(0, 0x24, secondHost, 2, NULL), 0);
	CHECK(machines[0]->rfu.radioQueued);
	CHECK(machines[0]->rfu.radioTicket == firstTicket);
	EQ(command(1, 0x25, firstClient, 2, NULL), 0);
	finishSynchronizedRound(false);
	expectShortPacket(1, 4, firstHost[1]);
	expectShortPacket(0, 4, firstClient[1]);
	CHECK(machines[0]->rfu.radioNextTicket > firstTicket);
	CHECK(machines[1]->rfu.radioLastTicket > firstTicket);
	CHECK(machines[0]->rfu.radioPending);
	EQ(command(1, 0x25, secondClient, 2, NULL), 0);
	finishSynchronizedRound(false);
	expectShortPacket(1, 4, secondHost[1]);
	expectShortPacket(0, 4, secondClient[1]);
	CHECK(!machines[0]->rfu.radioPending);
	CHECK(!machines[1]->rfu.radioPending);
	end();
}

static void testSynchronizedRetryBudget(void) {
	begin("Unmatched synchronized host wait reports receipt mask at finite retry budget", 2);
		synchronizePair();
	const uint32_t settings = 0x0120; /* One retry, outer timeout much later. */
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	const uint32_t packet[] = {4, 0xCAFEBABE};
	EQ(command(0, 0x25, packet, 2, NULL), 0);
	const unsigned irqs = armAsyncWithoutAdvancing(0);
	advance(FRAME_CYCLES / 12);
	EQ(machines[0]->irqs, irqs);
	advance(FRAME_CYCLES / 6);
	EQ(machines[0]->irqs, irqs + 1);
	EQ(rx(0), 0x99660128);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	CHECK(!machines[0]->rfu.radioPending);
	EQ(machines[1]->rfu.rx.count, 0);
	end();
}

static struct GBASIORFUMessage holdRadioMessage(unsigned recipient, uint64_t ticket, unsigned source) {
	struct GBASIORFUDriver* rfu = &machines[recipient]->rfu;
	struct GBASIORFUMessage held = {0};
	struct GBASIORFUMessage retained[GBA_RFU_MESSAGE_DEPTH];
	unsigned count = 0;
	bool found = false;
	MutexLock(&coordinator.mutex);
	for (unsigned i = 0; i < rfu->incomingCount; ++i) {
		struct GBASIORFUMessage message = rfu->incoming[(rfu->incomingHead + i) % GBA_RFU_MESSAGE_DEPTH];
		if (!found && message.radioTicket == ticket && message.source == source) {
			held = message;
			found = true;
		} else {
			retained[count++] = message;
		}
	}
	memcpy(rfu->incoming, retained, count * sizeof(retained[0]));
	rfu->incomingHead = 0;
	rfu->incomingCount = count;
	MutexUnlock(&coordinator.mutex);
	CHECK(found);
	return held;
}

static void deliverRadioMessage(unsigned recipient, const struct GBASIORFUMessage* message) {
	struct GBASIORFUDriver* rfu = &machines[recipient]->rfu;
	MutexLock(&coordinator.mutex);
	CHECK(rfu->incomingCount < GBA_RFU_MESSAGE_DEPTH);
	rfu->incoming[(rfu->incomingHead + rfu->incomingCount) % GBA_RFU_MESSAGE_DEPTH] = *message;
	++rfu->incomingCount;
	MutexUnlock(&coordinator.mutex);
}

static void testSynchronizedSessionReplay(void) {
	begin("Old synchronized OFFER, READY receipt and RELEASE cannot enter a reset session", 2);
		synchronizePair();
	const uint32_t host[] = {4, 0x11223344};
	const uint32_t client[] = {4 << 8, 0x55667788};
	EQ(command(0, 0x24, host, 2, NULL), 0);
	const struct GBASIORFUMessage oldOffer = machines[0]->rfu.radioOffer;
	const uint64_t ticket = oldOffer.radioTicket;
	CHECK(ticket != 0);
	EQ(command(1, 0x25, client, 2, NULL), 0);
	const struct GBASIORFUMessage oldReady = machines[1]->rfu.radioReply;
	CHECK(oldReady.radioTicket == ticket);
	/* Keep the actual release in its mailbox while advancing only the host. */
	mTimingDeschedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent);
	mTimingDeschedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent);
	advanceOne(0, FRAME_CYCLES);
	CHECK(!machines[0]->rfu.radioPending);
	const struct GBASIORFUMessage oldRelease = holdRadioMessage(1, ticket, 0);
	for (unsigned id = 0; id < 2; ++id) {
		GBASIOReset(&machines[id]->gba.sio);
		GBASIOWriteRCNT(&machines[id]->gba.sio, 0);
	}
	connectPair();
	enableSynchronization();
	CHECK(oldOffer.targetSession != machines[1]->rfu.radioSession);
	CHECK(oldReady.targetSession != machines[0]->rfu.radioSession);
	deliverRadioMessage(1, &oldOffer);
	deliverRadioMessage(1, &oldRelease);
	deliverRadioMessage(0, &oldReady);
	uint32_t response[32];
	EQ(command(0, 0x12, NULL, 0, response), 1);
	EQ(command(1, 0x12, NULL, 0, response), 1);
	CHECK(!machines[0]->rfu.radioPending);
	CHECK(!machines[1]->rfu.radioPending);
	EQ(machines[0]->rfu.txAckMask, 0);
	EQ(machines[0]->rfu.clients[0].rx.count, 0);
	EQ(machines[1]->rfu.rx.count, 0);
	const uint32_t newHost[] = {4, 0x90ABCDEF};
	const uint32_t newClient[] = {4 << 8, 0x10203040};
	EQ(command(0, 0x25, newHost, 2, NULL), 0);
	CHECK(machines[0]->rfu.radioTicket > ticket);
	EQ(command(1, 0x25, newClient, 2, NULL), 0);
	finishSynchronizedRound(true);
	expectShortPacket(1, 4, newHost[1]);
	expectShortPacket(0, 4, newClient[1]);
	end();
}

static void testSynchronizedLifecycle(void) {
	static const char* const cases[] = {
		"Synchronized reset cancels unresolved tickets on both adapters",
		"Synchronized local disconnect cancels unresolved tickets",
		"Synchronized remote disconnect cancels unresolved tickets",
		"Synchronized peer detach cancels unresolved tickets without a stale IRQ",
		"Synchronized peer deinit cancels unresolved tickets without a stale IRQ"
	};
	for (unsigned variant = 0; variant < COUNT_OF(cases); ++variant) {
		begin(cases[variant], 2);
		synchronizePair();
		const uint32_t host[] = {4, 0x0BADF00D};
		EQ(command(0, 0x24, host, 2, NULL), 0);
		advance(FRAME_CYCLES); /* COMMAND-state peer drains through frame maintenance. */
		CHECK(machines[0]->rfu.radioPending);
		CHECK(machines[1]->rfu.radioPending);
		const unsigned clientIrqs = machines[1]->irqs;
		const uint32_t slot0 = 1;
		switch (variant) {
		case 0: GBASIOReset(&machines[1]->gba.sio); break;
		case 1: EQ(command(1, 0x30, &slot0, 1, NULL), 0); break;
		case 2: EQ(command(0, 0x30, &slot0, 1, NULL), 0); break;
		case 3: GBASIORFUCoordinatorDetach(&coordinator, &machines[1]->rfu); break;
		case 4: GBASIOSetDriver(&machines[1]->gba.sio, NULL); break;
		}
		const unsigned irqsAfterAction = machines[1]->irqs;
		advance(FRAME_CYCLES * 2);
		CHECK(!machines[0]->rfu.radioPending);
		CHECK(!machines[1]->rfu.radioPending);
		CHECK(!machines[0]->rfu.radioQueued);
		CHECK(!machines[1]->rfu.radioQueued);
		EQ(machines[1]->rfu.radioTicket, 0);
		EQ(machines[1]->rfu.rx.count, 0);
		EQ(machines[1]->rfu.tx.bytes, 0);
		EQ(machines[1]->rfu.role, GBA_RFU_IDLE);
		EQ(machines[1]->irqs, irqsAfterAction);
		if (variant != 1) EQ(machines[1]->irqs, clientIrqs);
		end();
	}
}

static void testSynchronizedSerialCancellation(void) {
	begin("Synchronized completed data survives canceled 256-cycle serial word", 2);
		synchronizePair();
	const uint32_t host[] = {4, 0x76543210};
	const uint32_t zero = 0;
	EQ(command(0, 0x24, host, 2, NULL), 0);
	EQ(command(1, 0x25, &zero, 1, NULL), 0);
	armAdapterTransfer(1, CLOCK_FAST);
	const unsigned irqs = machines[1]->irqs;
	advance(100);
	GBASIOWriteSIOCNT(&machines[1]->gba.sio, NORMAL_IRQ | CLOCK_FAST);
	CHECK(!machines[1]->rfu.adapterPending);
	advance(512);
	EQ(machines[1]->irqs, irqs);
	EQ(machines[1]->rfu.asyncIndex, 0);
	exactAdapterWord(1, CLOCK_FAST, 0x99660028);
	exactAdapterWord(1, CLOCK_FAST, 0x80000000);
	expectShortPacket(1, 4, host[1]);
	end();
}

static void testSynchronizationOptionAndMissingPeer(void) {
	begin("Synchronization defaults off; enabled lone host remains a responsive peripheral", 1);
	CHECK(!machines[0]->rfu.synchronize);
	GBASIORFUDriverSetSynchronize(&machines[0]->rfu, true);
	CHECK(machines[0]->rfu.synchronize);
	handshake(0);
	const uint32_t zero = 0;
	EQ(command(0, 0x17, &zero, 1, NULL), 0);
	EQ(command(0, 0x19, NULL, 0, NULL), 0);
	EQ(command(0, 0x25, &zero, 1, NULL), 0);
	CHECK(!machines[0]->rfu.radioPending);
	EQ(machines[0]->rfu.txTargetMask, 0);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	GBASIORFUDriverSetSynchronize(&machines[0]->rfu, false);
	CHECK(!machines[0]->rfu.synchronize);
	uint32_t response[32];
	EQ(command(0, 0x12, NULL, 0, response), 1);
	EQ(response[0], 0x00830117);
	end();
}

static void testSynchronizedFiniteWaitCancellation(void) {
	for (unsigned clientTimeout = 0; clientTimeout < 2; ++clientTimeout) {
		begin(clientTimeout ? "Finite synchronized client wait retires readiness before a later offer" :
			"Finite synchronized host wait cancels its unmatched ticket", 2);
		synchronizePair();
		const unsigned id = clientTimeout ? 1 : 0;
		const uint32_t settings = 0xFF01; /* Timeout first; retry limit is much later. */
		EQ(command(id, 0x17, &settings, 1, NULL), 0);
		const uint32_t pending[] = {clientTimeout ? 4 << 8 : 4, 0x11223344};
		EQ(command(id, 0x25, pending, 2, NULL), 0);
		const unsigned irqs = armAsyncWithoutAdvancing(id);
		advance(FRAME_CYCLES / 2);
		EQ(machines[id]->irqs, irqs);
		advance(FRAME_CYCLES * 2);
		EQ(machines[id]->irqs, irqs + 1);
		EQ(rx(id), 0x99660027);
		EQ(asynchronousReceive(id, CLOCK_FAST), 0x80000000);
		CHECK(!machines[id]->rfu.radioPending);
		CHECK(!machines[id]->rfu.radioReady);
		const uint32_t unlimited = 0;
		EQ(command(id, 0x17, &unlimited, 1, NULL), 0);
		const uint32_t host[] = {4, 0x55667788};
		const uint32_t client[] = {4 << 8, 0x90ABCDEF};
		EQ(command(0, 0x24, host, 2, NULL), 0);
		advance(4096);
		EQ(machines[1]->rfu.rx.count, 0);
		CHECK(!machines[0]->rfu.radioMatched);
		EQ(command(1, 0x25, client, 2, NULL), 0);
		finishSynchronizedRound(false);
		expectShortPacket(1, 4, host[1]);
		expectShortPacket(0, 4, client[1]);
		end();
	}
}

static void testSynchronizedPlainSendReadiness(void) {
	begin("Synchronized client24 readiness works before or after a host24 offer without serial IRQs", 2);
		synchronizePair();
	for (unsigned clientFirst = 0; clientFirst < 2; ++clientFirst) {
		const uint32_t host[] = {4, 0xA0000000 | clientFirst};
		const uint32_t client[] = {4 << 8, 0xB0000000 | clientFirst};
		if (clientFirst) EQ(command(1, 0x24, client, 2, NULL), 0);
		EQ(command(0, 0x24, host, 2, NULL), 0);
		if (!clientFirst) EQ(command(1, 0x24, client, 2, NULL), 0);
		const unsigned hostIrqs = machines[0]->irqs;
		const unsigned clientIrqs = machines[1]->irqs;
		advance(FRAME_CYCLES * 2);
		EQ(machines[0]->irqs, hostIrqs);
		EQ(machines[1]->irqs, clientIrqs);
		CHECK(!machines[0]->rfu.radioPending);
		CHECK(!machines[1]->rfu.radioPending);
		expectShortPacket(1, 4, host[1]);
		expectShortPacket(0, 4, client[1]);
	}
	end();
}

static void testSynchronizedSameSessionReplay(void) {
	begin("Same-session old OFFER, READY receipt and RELEASE cannot satisfy a newer retry ticket", 2);
		synchronizePair();
	const uint32_t host[] = {4, 0x0BADF00D};
	const uint32_t client[] = {4 << 8, 0x12345678};
	EQ(command(0, 0x24, host, 2, NULL), 0);
	const struct GBASIORFUMessage oldOffer = machines[0]->rfu.radioOffer;
	const uint64_t ticket = oldOffer.radioTicket;
	const uint32_t epoch = oldOffer.txEpoch;
	EQ(command(1, 0x25, client, 2, NULL), 0);
	const struct GBASIORFUMessage oldReady = machines[1]->rfu.radioReply;
	mTimingDeschedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent);
	mTimingDeschedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent);
	advanceOne(0, FRAME_CYCLES);
	const struct GBASIORFUMessage oldRelease = holdRadioMessage(1, ticket, 0);
	deliverRadioMessage(1, &oldRelease);
	mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent, 0);
	mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent, FRAME_CYCLES);
	const unsigned oldIrqs = armAsyncWithoutAdvancing(1);
	awaitAsyncHeader(1, oldIrqs, 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	expectShortPacket(1, 4, host[1]);
	expectShortPacket(0, 4, client[1]);
	EQ(command(0, 0x37, NULL, 0, NULL), 0);
	const uint64_t newTicket = machines[0]->rfu.radioTicket;
	CHECK(newTicket > ticket);
	EQ(machines[0]->rfu.txEpoch, epoch); /* The old receipt has a valid logical epoch. */
	EQ(oldReady.targetSession, machines[0]->rfu.radioSession);
	EQ(oldRelease.targetSession, machines[1]->rfu.radioSession);
	deliverRadioMessage(0, &oldReady);
	deliverRadioMessage(1, &oldRelease);
	deliverRadioMessage(1, &oldOffer);
	/* An idle COMMAND adapter drains unsolicited offers at maintenance,
	 * unlike a WAIT adapter with an active short poll. */
	advance(FRAME_CYCLES);
	CHECK(machines[0]->rfu.radioPending);
	CHECK(!machines[0]->rfu.radioMatched);
	CHECK(machines[0]->rfu.radioTicket == newTicket);
	CHECK(machines[1]->rfu.radioTicket == newTicket);
	CHECK(!machines[1]->rfu.radioReleased);
	EQ(machines[1]->rfu.rx.count, 0);
	const uint32_t nextClient[] = {4 << 8, 0x90ABCDEF};
	EQ(command(1, 0x25, nextClient, 2, NULL), 0);
	finishSynchronizedRound(true);
	expectShortPacket(1, 4, host[1]);
	expectShortPacket(0, 4, nextClient[1]);
	end();
}

static void testSynchronizedFullMailboxRecovery(void) {
	begin("Full client mailbox retries the same OFFER ticket once capacity returns", 2);
		synchronizePair();
	struct GBASIORFUDriver* client = &machines[1]->rfu;
	mTimingDeschedule(&machines[1]->gba.timing, &client->pollEvent);
	mTimingDeschedule(&machines[1]->gba.timing, &client->maintenanceEvent);
	MutexLock(&coordinator.mutex);
	memset(client->incoming, 0, sizeof(client->incoming));
	client->incomingHead = 0;
	client->incomingCount = GBA_RFU_MESSAGE_DEPTH;
	MutexUnlock(&coordinator.mutex);
	const uint32_t hostData[] = {4, 0x11223344};
	EQ(command(0, 0x25, hostData, 2, NULL), 0);
	const uint64_t ticket = machines[0]->rfu.radioTicket;
	const uint32_t epoch = machines[0]->rfu.txEpoch;
	CHECK(ticket != 0);
	CHECK(machines[0]->rfu.radioPending);
	CHECK(!machines[0]->rfu.radioOfferPosted);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	advanceOne(0, 4096);
	EQ(machines[0]->irqs, hostIrqs);
	CHECK(machines[0]->rfu.radioTicket == ticket);
	EQ(machines[0]->rfu.txEpoch, epoch);
	MutexLock(&coordinator.mutex);
	client->incomingCount = 0;
	client->incomingHead = 0;
	MutexUnlock(&coordinator.mutex);
	advanceOne(0, 1024);
	CHECK(machines[0]->rfu.radioOfferPosted);
	CHECK(machines[0]->rfu.radioTicket == ticket);
	EQ(machines[0]->rfu.txEpoch, epoch);
	mTimingSchedule(&machines[1]->gba.timing, &client->maintenanceEvent, 0);
	advanceOne(1, 1024);
	CHECK(client->radioPending);
	const uint32_t clientData[] = {4 << 8, 0x55667788};
	EQ(command(1, 0x25, clientData, 2, NULL), 0);
	const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
	awaitAsyncHeader(1, clientIrqs, 0x99660028);
	awaitAsyncHeader(0, hostIrqs, 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	expectShortPacket(1, 4, hostData[1]);
	expectShortPacket(0, 4, clientData[1]);
	end();
}

static void testSynchronizedMatchedClientTimeout(void) {
	begin("Local client timeout restores matched TX snapshot for a later explicit retry", 2);
		synchronizePair();
	const uint32_t settings = 0xFF01;
	EQ(command(1, 0x17, &settings, 1, NULL), 0);
	advanceOne(0, FRAME_CYCLES * 10);
	const uint32_t oldHost[] = {4, 0x11223344};
	const uint32_t clientData[] = {4 << 8, 0x55667788};
	EQ(command(0, 0x24, oldHost, 2, NULL), 0);
	EQ(command(1, 0x25, clientData, 2, NULL), 0);
	CHECK(machines[1]->rfu.radioMatched);
	CHECK(!machines[1]->rfu.clientTxPending);
	const unsigned irqs = armAsyncWithoutAdvancing(1);
	advanceOne(1, FRAME_CYCLES * 2);
	EQ(machines[1]->irqs, irqs + 1);
	EQ(rx(1), 0x99660027);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	CHECK(!machines[1]->rfu.radioPending);
	CHECK(machines[1]->rfu.clientTxPending);
	EQ(machines[1]->rfu.tx.bytes, 4);
	uint32_t retained = 0;
	memcpy(&retained, machines[1]->rfu.tx.data, sizeof(retained));
	EQ(retained, clientData[1]);
	const uint32_t unlimited = 0;
	EQ(command(1, 0x17, &unlimited, 1, NULL), 0);
	const uint32_t newHost[] = {4, 0x90ABCDEF};
	EQ(command(0, 0x24, newHost, 2, NULL), 0);
	EQ(command(1, 0x37, NULL, 0, NULL), 0);
	finishSynchronizedRound(false);
	expectShortPacket(1, 4, newHost[1]);
	expectShortPacket(0, 4, clientData[1]);
	end();
}

static void testSynchronizedCancelledQueuedOffer(void) {
	begin("Cancelled queued OFFER is not promoted after an older RELEASE finishes alignment", 2);
	synchronizePair();
	const uint32_t settings = 0x0001; /* One-frame ticket timeout, no retry budget. */
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	advanceOne(0, FRAME_CYCLES * 3);
	const uint32_t firstHost[] = {4, 0x11223344};
	const uint32_t secondHost[] = {4, 0x55667788};
	const uint32_t clientData[] = {4 << 8, 0x90ABCDEF};
	EQ(command(0, 0x24, firstHost, 2, NULL), 0);
	const uint64_t firstTicket = machines[0]->rfu.radioTicket;
	EQ(command(0, 0x24, secondHost, 2, NULL), 0);
	CHECK(machines[0]->rfu.radioQueued);
	EQ(command(1, 0x25, clientData, 2, NULL), 0);
	CHECK(machines[1]->rfu.radioMatched);
	CHECK(machines[1]->rfu.radioDue > machines[1]->rfu.radioClock + FRAME_CYCLES);
	/* Keep A locally unresolved while the host completes A and times out B. */
	mTimingDeschedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent);
	mTimingDeschedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent);
	advanceOne(0, FRAME_CYCLES * 2);
	const uint64_t secondTicket = machines[0]->rfu.radioNextTicket;
	CHECK(secondTicket > firstTicket);
	CHECK(!machines[0]->rfu.radioPending);
	const struct GBASIORFUMessage release = holdRadioMessage(1, firstTicket, 0);
	const struct GBASIORFUMessage offer = holdRadioMessage(1, secondTicket, 0);
	const struct GBASIORFUMessage cancel = holdRadioMessage(1, secondTicket, 0);
	EQ(offer.bytes, 4);
	EQ(cancel.bytes, 0);
	deliverRadioMessage(1, &release);
	deliverRadioMessage(1, &offer);
	deliverRadioMessage(1, &cancel);
	mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent, 0);
	mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent, FRAME_CYCLES);
	advanceOne(1, FRAME_CYCLES * 5);
	CHECK(!machines[1]->rfu.radioPending);
	CHECK(!machines[1]->rfu.radioQueued);
	CHECK(!machines[1]->rfu.radioTimedOut); /* Cancelling B must not cancel A's wait. */
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	expectShortPacket(1, 4, firstHost[1]);
	expectShortPacket(0, 4, clientData[1]);
	end();
}

static struct GBASIORFUMessage prepareSynchronizedRemoteCancellation(void) {
	synchronizePair();
	const uint32_t settings = 0x0120; /* One host retry; client limits stay unlimited. */
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	const uint32_t host[] = {4, 0x11223344};
	const uint32_t client[] = {4 << 8, 0x55667788};
	EQ(command(0, 0x25, host, 2, NULL), 0);
	const uint64_t ticket = machines[0]->rfu.radioTicket;
	EQ(command(1, 0x25, client, 2, NULL), 0);
	CHECK(machines[1]->rfu.radioMatched);
	/* Hold the genuine READY so only the host's finite retry cancels this wait. */
	const struct GBASIORFUMessage ready = holdRadioMessage(0, ticket, 1);
	EQ(ready.bytes, 4);
	mTimingDeschedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent);
	mTimingDeschedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	advanceOne(0, FRAME_CYCLES / 3);
	EQ(machines[0]->irqs, hostIrqs + 1);
	EQ(rx(0), 0x99660128);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	CHECK(!machines[0]->rfu.radioPending);
	return holdRadioMessage(1, ticket, 0);
}

static void testRemoteCancellationReadiness(bool prearmed) {
	static const char* const cases[] = {
		"Remote CANCEL and next OFFER in one drain cannot reuse cancelled WAIT readiness",
		"Remote CANCEL retires WAIT readiness before a later OFFER after timeout notification"
	};
	for (unsigned variant = 0; variant < COUNT_OF(cases); ++variant) {
		const bool sameDrain = variant == 0;
		if (prearmed) beginPrearmed(sameDrain ?
			"Prearmed policy rejects cancelled readiness when CANCEL and next OFFER share one drain" :
			"Prearmed policy requires explicit retry after remote WAIT cancellation");
		else begin(cases[variant], 2);
		const struct GBASIORFUMessage cancel = prepareSynchronizedRemoteCancellation();
		const uint32_t host[] = {4, 0x90ABCDEF};
		struct GBASIORFUMessage offer = {0};
		if (sameDrain) {
			EQ(command(0, 0x24, host, 2, NULL), 0);
			offer = holdRadioMessage(1, machines[0]->rfu.radioTicket, 0);
		}
		deliverRadioMessage(1, &cancel);
		if (sameDrain) deliverRadioMessage(1, &offer);
		mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent, 0);
		mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent, FRAME_CYCLES);
		const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
		advanceOne(1, 512);
		EQ(machines[1]->irqs, clientIrqs + 1);
		EQ(rx(1), 0x99660027);
		CHECK(!machines[1]->rfu.radioReady);
		if (sameDrain) CHECK(!machines[1]->rfu.radioMatched);
		EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
		CHECK(machines[1]->rfu.clientTxPending);
		if (!sameDrain) {
			EQ(command(0, 0x24, host, 2, NULL), 0);
			offer = holdRadioMessage(1, machines[0]->rfu.radioTicket, 0);
			deliverRadioMessage(1, &offer);
			mTimingDeschedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent);
			mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent, 0);
		}
		advance(FRAME_CYCLES / 12);
		CHECK(!machines[0]->rfu.radioMatched);
		CHECK(!machines[1]->rfu.radioMatched);
		EQ(machines[1]->rfu.rx.count, 0);
		/* A fresh explicit retry, not the old cancelled readiness, may exchange TX. */
		EQ(command(1, 0x37, NULL, 0, NULL), 0);
		finishSynchronizedRound(false);
		expectShortPacket(1, 4, host[1]);
		expectShortPacket(0, 4, 0x55667788);
		end();
	}
}

static void testSynchronizedRemoteCancellationReadiness(void) {
	testRemoteCancellationReadiness(false);
}

static void testSynchronizedElapsedMailboxAlignment(void) {
	static const char* const cases[] = {
		"Delayed READY credits host time already advanced past the peer timestamp",
		"Delayed OFFER credits client time already advanced past the peer timestamp"
	};
	for (unsigned delayedOffer = 0; delayedOffer < COUNT_OF(cases); ++delayedOffer) {
		begin(cases[delayedOffer], 2);
		synchronizePair();
		const uint32_t host[] = {4, 0x10203040};
		const uint32_t client[] = {4 << 8, 0x50607080};
		const unsigned owner = delayedOffer ? 1 : 0;
		advanceOne(delayedOffer ? 0 : 1, FRAME_CYCLES * 3);
		if (delayedOffer) {
			EQ(command(1, 0x25, client, 2, NULL), 0);
		} else {
			EQ(command(0, 0x25, host, 2, NULL), 0);
		}
		/* Hold a production message while its owner continues running locally. */
		mTimingDeschedule(&machines[owner]->gba.timing, &machines[owner]->rfu.pollEvent);
		mTimingDeschedule(&machines[owner]->gba.timing, &machines[owner]->rfu.maintenanceEvent);
		if (delayedOffer) {
			EQ(command(0, 0x24, host, 2, NULL), 0);
		} else {
			EQ(command(1, 0x25, client, 2, NULL), 0);
		}
		const uint64_t ticket = machines[0]->rfu.radioTicket;
		const struct GBASIORFUMessage message = holdRadioMessage(owner, ticket, 1 - owner);
		CHECK(message.relativeCycles > machines[owner]->rfu.radioArrival + FRAME_CYCLES * 2);
		advanceOne(owner, FRAME_CYCLES * 4);
		deliverRadioMessage(owner, &message);
		mTimingSchedule(&machines[owner]->gba.timing, &machines[owner]->rfu.pollEvent, 0);
		mTimingSchedule(&machines[owner]->gba.timing, &machines[owner]->rfu.maintenanceEvent, FRAME_CYCLES);
		const unsigned irqs = armAsyncWithoutAdvancing(owner);
		uint32_t started = mTimingCurrentTime(&machines[owner]->gba.timing);
		if (delayedOffer) {
			advanceOne(1, 128); /* Client posts its real READY. */
			advanceOne(0, 128); /* Host receives READY and posts RELEASE. */
			advanceOne(1, 128); /* Client drains RELEASE; no historical lag remains. */
			CHECK(!machines[1]->rfu.radioPending);
			EQ(machines[1]->rfu.rx.count, 1);
			CHECK(machines[1]->rfu.adapterPending);
			EQ(mTimingUntil(&machines[1]->gba.timing, &machines[1]->rfu.adapterEvent), 256);
			started = mTimingCurrentTime(&machines[1]->gba.timing);
		}
		advanceOne(owner, 255);
		CHECK(!machines[owner]->rfu.radioPending);
		EQ(machines[0]->rfu.clients[0].rx.count, 1);
		CHECK(machines[owner]->rfu.adapterPending);
		EQ(machines[owner]->irqs, irqs);
		EQ(mTimingUntil(&machines[owner]->gba.timing, &machines[owner]->rfu.adapterEvent), 1);
		advanceOne(owner, 1);
		EQ(machines[owner]->irqs, irqs + 1);
		EQ(machines[owner]->irqTime, started + 256);
		EQ(rx(owner), 0x99660028);
		EQ(asynchronousReceive(owner, CLOCK_FAST), 0x80000000);
		if (!delayedOffer) {
			EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
			EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
		}
		expectShortPacket(1, 4, host[1]);
		expectShortPacket(0, 4, client[1]);
		end();
	}
}

static int64_t effectiveRadioClock(unsigned id) {
	const struct GBASIORFUDriver* rfu = &machines[id]->rfu;
	return rfu->radioClock + (uint32_t) (mTimingCurrentTime(&machines[id]->gba.timing) - rfu->radioLastTime);
}

static void testSynchronizedMatchedClockOrigin(void) {
	static const char* const cases[] = {
		"Paired READY clock origin prevents a second alignment after equal subsequent progress",
		"Readiness staged during alignment is translated from the paired READY clock origin"
	};
	for (unsigned variant = 0; variant < COUNT_OF(cases); ++variant) {
		const unsigned stagedReady = 1 - variant;
		begin(cases[stagedReady], 2);
		synchronizePair();
		advanceOne(0, FRAME_CYCLES * 2);
		const uint32_t firstHost[] = {4, 0x11223344};
		const uint32_t firstClient[] = {4 << 8, 0x55667788};
		const uint32_t secondHost[] = {4, 0x90ABCDEF};
		const uint32_t secondClient[] = {4 << 8, 0x10203040};
		EQ(command(0, 0x24, firstHost, 2, NULL), 0);
		const uint64_t firstTicket = machines[0]->rfu.radioTicket;
		EQ(command(1, stagedReady ? 0x24 : 0x25, firstClient, 2, NULL), 0);
		CHECK(machines[1]->rfu.radioMatched);
		CHECK(machines[1]->rfu.radioDue - effectiveRadioClock(1) > FRAME_CYCLES);
		const int64_t firstMatchedClock = machines[1]->rfu.radioReply.relativeCycles;
		const uint32_t firstMatchedTime = mTimingCurrentTime(&machines[1]->gba.timing) -
			(uint32_t) (effectiveRadioClock(1) - firstMatchedClock);
		int64_t stagedArrival = 0;
		if (stagedReady) {
			EQ(command(1, 0x24, secondClient, 2, NULL), 0);
			CHECK(machines[1]->rfu.radioReady);
			stagedArrival = machines[1]->rfu.radioArrival;
			EQ(command(0, 0x24, secondHost, 2, NULL), 0);
			CHECK(machines[1]->rfu.radioPending);
		}
		/* Both owner clocks advance equally throughout the client's first alignment. */
		advance(FRAME_CYCLES * 3);
		CHECK(!machines[0]->rfu.radioPending);
		CHECK(!machines[1]->rfu.radioPending);
		if (stagedReady) {
			CHECK(machines[1]->rfu.radioReply.radioTicket > firstTicket);
			EQ(machines[1]->rfu.radioReply.relativeCycles, stagedArrival - firstMatchedClock - 256);
		} else {
			EQ(asynchronousReceive(1, CLOCK_FAST), 0x99660028);
			EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
			expectShortPacket(1, 4, firstHost[1]);
			expectShortPacket(0, 4, firstClient[1]);
			EQ(command(0, 0x24, secondHost, 2, NULL), 0);
			EQ(command(1, 0x25, secondClient, 2, NULL), 0);
			const int64_t secondLag = machines[1]->rfu.radioDue - effectiveRadioClock(1);
			const int64_t expectedClock = (uint32_t) (mTimingCurrentTime(&machines[1]->gba.timing) - firstMatchedTime) - 256;
			if (secondLag > 8192) {
				fprintf(stderr, "secondLag=%lld effectiveClock=%lld expectedClock=%lld\n",
					(long long) secondLag, (long long) effectiveRadioClock(1), (long long) expectedClock);
			}
			CHECK(secondLag <= 8192); /* Only bounded wire/poll jitter, not another two-frame skew. */
			EQ(effectiveRadioClock(1), expectedClock);
			finishSynchronizedRound(false);
		}
		expectShortPacket(1, 4, secondHost[1]);
		expectShortPacket(0, 4, secondClient[1]);
		end();
	}
}

static void finishPendingRadioHeader(unsigned id, unsigned previousIrqs, uint32_t expected) {
	for (unsigned polls = 0; !machines[id]->rfu.adapterPending && polls < 8; ++polls) {
		advanceOne(id, 128);
	}
	CHECK(machines[id]->rfu.adapterPending);
	const uint32_t remaining = mTimingUntil(&machines[id]->gba.timing, &machines[id]->rfu.adapterEvent);
	CHECK(remaining && remaining <= 256);
	const uint32_t started = mTimingCurrentTime(&machines[id]->gba.timing) + remaining - 256;
	EQ(machines[id]->irqs, previousIrqs);
	advanceOne(id, remaining - 1);
	EQ(machines[id]->irqs, previousIrqs);
	advanceOne(id, 1);
	EQ(machines[id]->irqs, previousIrqs + 1);
	EQ(machines[id]->irqTime, started + 256);
	EQ(rx(id), expected);
}

static void testSynchronizedPausedAlignmentProgress(void) {
	static const char* const cases[] = {
		"Paused host preserves alternating genuine alignment lead and finite client timeout",
		"Paused client preserves alternating genuine alignment lead and finite host timeout"
	};
	for (unsigned initiallyAhead = 0; initiallyAhead < COUNT_OF(cases); ++initiallyAhead) {
		begin(cases[initiallyAhead], 2);
		synchronizePair();
		advanceOne(initiallyAhead, FRAME_CYCLES * 2);
		for (unsigned round = 0; round < 3; ++round) {
			const unsigned laggingOwner = (1 - initiallyAhead) ^ (round & 1);
			const uint32_t host[] = {4, 0x11223340 | round};
			const uint32_t client[] = {4 << 8, 0x55667780 | round};
			if (round == 2) {
				const uint32_t finite = 0xFF01; /* One-frame timeout, much later retry limit. */
				EQ(command(laggingOwner, 0x17, &finite, 1, NULL), 0);
			}
			EQ(command(0, 0x25, host, 2, NULL), 0);
			EQ(command(1, 0x25, client, 2, NULL), 0);
			advanceOne(0, 128); /* Receive the genuine READY; at most one owner can complete yet. */
			CHECK(machines[laggingOwner]->rfu.radioPending);
			CHECK(machines[laggingOwner]->rfu.radioMatched);
			const int64_t lead = machines[laggingOwner]->rfu.radioDue - effectiveRadioClock(laggingOwner);
			CHECK(lead > FRAME_CYCLES && lead < FRAME_CYCLES * 3);
			const unsigned irqs[] = {armAsyncWithoutAdvancing(0), armAsyncWithoutAdvancing(1)};
			/* The opposite owner is paused: this is real independent progress, not equal pacing. */
			for (unsigned polls = 0; machines[laggingOwner]->rfu.radioPending && polls < 10000; ++polls) {
				advanceOne(laggingOwner, 128);
			}
			CHECK(!machines[laggingOwner]->rfu.radioPending);
			const uint32_t header = round == 2 ? 0x99660027 : 0x99660028;
			finishPendingRadioHeader(laggingOwner, irqs[laggingOwner], header);
			/* A host already released the packet before a client's local timeout.
			 * Its committed receipt cannot be rolled back by that later CANCEL. */
			const uint32_t peerHeader = round == 2 && laggingOwner == 1 ? 0x99660028 : header;
			finishPendingRadioHeader(1 - laggingOwner, irqs[1 - laggingOwner], peerHeader);
			EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
			EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
			if (round == 2) {
				CHECK(!machines[0]->rfu.radioPending && !machines[1]->rfu.radioPending);
				EQ(machines[laggingOwner]->rfu.radioMatchedAt, 0);
				EQ(machines[1]->rfu.radioMatchedAt, 0);
				CHECK(!machines[1]->rfu.radioReady);
				CHECK(machines[1]->rfu.clientTxPending);
				if (laggingOwner == 1) {
					expectShortPacket(0, 4, client[1]); /* Preserve the already committed host receipt. */
				}
				EQ(machines[0]->rfu.clients[0].rx.count, 0);
				EQ(machines[1]->rfu.rx.count, 0);
				const uint32_t unlimited = 0;
				EQ(command(laggingOwner, 0x17, &unlimited, 1, NULL), 0);
				EQ(command(0, 0x37, NULL, 0, NULL), 0);
				EQ(command(1, 0x37, NULL, 0, NULL), 0);
				finishSynchronizedRound(true);
			}
			expectShortPacket(1, 4, host[1]);
			expectShortPacket(0, 4, client[1]);
		}
		end();
	}
}

static bool logFileExists(const char* path) {
	struct stat status;
	return stat(path, &status) == 0;
}

static void unusedLogPath(char* path, size_t capacity, const char* tag) {
	/* run.ps1 compiles this absolute source path; all test files stay beside it. */
	for (unsigned index = 0; index < 1000; ++index) {
		int length = snprintf(path, capacity, "%s.buffered-%s-%u.log", __FILE__, tag, index);
		CHECK(length > 0 && (size_t) length < capacity);
		if (!logFileExists(path)) return;
	}
	fail("unused logging test path", __LINE__);
}

static char* readLogFile(const char* path) {
	FILE* file = fopen(path, "rb");
	CHECK(file != NULL);
	CHECK(fseek(file, 0, SEEK_END) == 0);
	long length = ftell(file);
	CHECK(length >= 0 && (size_t) length < GBA_RFU_LOG_CAPACITY + 4096);
	CHECK(fseek(file, 0, SEEK_SET) == 0);
	char* data = malloc((size_t) length + 1);
	CHECK(data != NULL);
	EQ(fread(data, 1, (size_t) length, file), length);
	data[length] = '\0';
	CHECK(fclose(file) == 0);
	return data;
}

static unsigned verifyEventLog(const char* data, unsigned expectedHeaders) {
	unsigned records = 0;
	unsigned headers = 0;
	unsigned long long previousSequence = 0;
	unsigned long long previousWall = 0;
	unsigned previousEmu = 0;
	for (const char* cursor = data; *cursor;) {
		const char* end = strchr(cursor, '\n');
		CHECK(end != NULL); /* Overflow must never retain a partial record. */
		size_t length = (size_t) (end - cursor);
		char line[1024];
		CHECK(length < sizeof(line));
		memcpy(line, cursor, length);
		line[length] = '\0';
		if (line[0] == '#') {
			CHECK(strncmp(line, "# RFU events v2 ", 16) == 0);
			++headers;
		} else {
			int player;
			unsigned emu, session, role, state, command;
			unsigned long long sequence, wall, ticket;
			EQ(sscanf(line, "P%d seq=%llu wallUs=%llu emu=%u session=%u role=%u state=%u cmd=%x ticket=%llu",
				&player, &sequence, &wall, &emu, &session, &role, &state, &command, &ticket), 9);
			EQ(player, 0);
			CHECK(sequence > previousSequence);
			CHECK(wall >= previousWall && emu >= previousEmu);
			CHECK(strstr(line, " event ") != NULL);
			CHECK(strstr(line, " trace ") == NULL && strstr(line, " recvWord ") == NULL);
			CHECK(strstr(line, "event external") == NULL);
			previousSequence = sequence;
			previousWall = wall;
			previousEmu = emu;
			++records;
		}
		cursor = end + 1;
	}
	EQ(headers, expectedHeaders);
	CHECK(records != 0);
	return records;
}

static void testBufferedLogExplicitFlush(void) {
	begin("RFU logging defaults disabled; events survive reset/deinit without hot-path disk writes", 1);
	handshake(0);
	struct GBASIORFUDriver* rfu = &machines[0]->rfu;
	CHECK(!rfu->logPath[0] && !rfu->logBuffer);
	EQ(rfu->logSize, 0);
	EQ(rfu->logSequence, 0);
	char path[1024];
	unusedLogPath(path, sizeof(path), "explicit");
	GBASIORFUDriverSetLogPath(rfu, path);
	CHECK(rfu->logBuffer != NULL);
	CHECK(!logFileExists(path));
	CHECK(GBASIORFUDriverFlushLog(rfu));
	char* beforeCommands = readLogFile(path);
	const size_t enabledSize = rfu->logSize;
	EQ(command(0, 0x11, NULL, 0, NULL), 1);
	EQ(command(0, 0x12, NULL, 0, NULL), 1);
	EQ(command(0, 0x13, NULL, 0, NULL), 1);
	EQ(rfu->logSize, enabledSize); /* Idle command/response and received-word noise is filtered. */
	const uint32_t settings = 0x0111;
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	const size_t beforeReset = rfu->logSize;
	CHECK(beforeReset > enabledSize);
	GBASIOWriteRCNT(&machines[0]->gba.sio, 0x8022);
	GBASIOWriteRCNT(&machines[0]->gba.sio, 0);
	handshake(0);
	CHECK(rfu->logSize > beforeReset);
	GBASIOSetDriver(&machines[0]->gba.sio, NULL);
	char* afterCommands = readLogFile(path);
	CHECK(strcmp(beforeCommands, afterCommands) == 0); /* Existing files are untouched too. */
	free(beforeCommands);
	free(afterCommands);
	CHECK(GBASIORFUDriverFlushLog(rfu));
	char* data = readLogFile(path);
	verifyEventLog(data, 2);
	CHECK(strstr(data, "event waitLimits timeoutFrames=17 retryCount=1") != NULL);
	CHECK(strstr(data, "event reset") != NULL);
	CHECK(strstr(data, "event command cmd=11") == NULL);
	CHECK(strstr(data, "event response cmd=11") == NULL);
	EQ(rfu->logSize, 0);
	CHECK(GBASIORFUDriverFlushLog(rfu));
	char* repeated = readLogFile(path);
	CHECK(strcmp(data, repeated) == 0); /* An empty repeated flush does not append a header. */
	free(repeated);
	free(data);
	GBASIORFUDriverSetLogPath(rfu, NULL);
	CHECK(remove(path) == 0);
	end();
}

static void testBufferedLogOverflow(void) {
	begin("Bounded RFU event capture evicts complete oldest records and reports retained/dropped counts", 1);
	handshake(0);
	struct GBASIORFUDriver* rfu = &machines[0]->rfu;
	char path[1024];
	unusedLogPath(path, sizeof(path), "overflow");
	GBASIORFUDriverSetLogPath(rfu, path);
	for (unsigned count = 0; rfu->logDropped < 32 && count < 100000; ++count) {
		const uint32_t settings = 0x0100 | (count & 0xFF);
		EQ(command(0, 0x17, &settings, 1, NULL), 0);
		CHECK(rfu->logSize <= GBA_RFU_LOG_CAPACITY);
	}
	CHECK(rfu->logDropped >= 32);
	CHECK(rfu->logHead < GBA_RFU_LOG_CAPACITY);
	CHECK(!logFileExists(path));
	const uint64_t sequence = rfu->logSequence;
	const uint64_t dropped = rfu->logDropped;
	CHECK(GBASIORFUDriverFlushLog(rfu));
	char* data = readLogFile(path);
	const unsigned retained = verifyEventLog(data, 1);
	EQ(retained + dropped, sequence);
	char marker[80];
	snprintf(marker, sizeof(marker), "dropped=%llu ", (unsigned long long) dropped);
	CHECK(strstr(data, marker) != NULL);
	EQ(rfu->logSize, 0);
	EQ(rfu->logDropped, 0);
	free(data);
	GBASIORFUDriverSetLogPath(rfu, NULL);
	CHECK(remove(path) == 0);
	end();
}

static void testBufferedLogPathChanges(void) {
	begin("Explicit RFU flush preserves old-path events; path changes/disable discard without disk I/O", 1);
	handshake(0);
	struct GBASIORFUDriver* rfu = &machines[0]->rfu;
	char first[1024], second[1024];
	unusedLogPath(first, sizeof(first), "path-first");
	unusedLogPath(second, sizeof(second), "path-second");
	GBASIORFUDriverSetLogPath(rfu, first);
	const uint32_t firstSettings = 0x0111;
	EQ(command(0, 0x17, &firstSettings, 1, NULL), 0);
	CHECK(!logFileExists(first));
	CHECK(GBASIORFUDriverFlushLog(rfu));
	char* firstData = readLogFile(first);
	GBASIORFUDriverSetLogPath(rfu, second);
	const uint32_t discarded = 0x0112;
	EQ(command(0, 0x17, &discarded, 1, NULL), 0);
	GBASIORFUDriverSetLogPath(rfu, first); /* Intentionally discard unflushed second-path capture. */
	CHECK(!logFileExists(second));
	GBASIORFUDriverSetLogPath(rfu, second);
	const uint32_t retained = 0x0113;
	EQ(command(0, 0x17, &retained, 1, NULL), 0);
	CHECK(GBASIORFUDriverFlushLog(rfu));
	char* secondData = readLogFile(second);
	verifyEventLog(secondData, 1);
	CHECK(strstr(secondData, "timeoutFrames=19 retryCount=1") != NULL);
	CHECK(strstr(secondData, "timeoutFrames=18 retryCount=1") == NULL);
	const uint32_t disabled = 0x0114;
	EQ(command(0, 0x17, &disabled, 1, NULL), 0);
	GBASIORFUDriverSetLogPath(rfu, NULL);
	CHECK(!rfu->logBuffer && !rfu->logPath[0]);
	EQ(command(0, 0x17, &disabled, 1, NULL), 0);
	EQ(rfu->logSize, 0);
	CHECK(GBASIORFUDriverFlushLog(rfu));
	char* firstAfter = readLogFile(first);
	char* secondAfter = readLogFile(second);
	CHECK(strcmp(firstData, firstAfter) == 0 && strcmp(secondData, secondAfter) == 0);
	free(firstData);
	free(secondData);
	free(firstAfter);
	free(secondAfter);
	CHECK(remove(first) == 0 && remove(second) == 0);
	end();
}

static void testBufferedLogFailedFlush(void) {
	begin("Failed RFU flush retains capture; stopped path replacement and explicit retry recover", 1);
	handshake(0);
	struct GBASIORFUDriver* rfu = &machines[0]->rfu;
	char parent[1024], invalid[1100], valid[1024];
	unusedLogPath(parent, sizeof(parent), "missing-parent");
	CHECK(snprintf(invalid, sizeof(invalid), "%s/events.log", parent) > 0);
	unusedLogPath(valid, sizeof(valid), "failure-recovery");
	GBASIORFUDriverSetLogPath(rfu, invalid);
	const uint32_t settings = 0x0111;
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	const size_t size = rfu->logSize;
	const size_t head = rfu->logHead;
	const uint64_t sequence = rfu->logSequence;
	const uint64_t dropped = rfu->logDropped;
	CHECK(!GBASIORFUDriverFlushLog(rfu));
	EQ(rfu->logSize, size);
	EQ(rfu->logHead, head);
	EQ(rfu->logSequence, sequence);
	EQ(rfu->logDropped, dropped);
	CHECK(!logFileExists(parent) && !logFileExists(invalid));
	GBASIORFUDriverSetLogPath(rfu, valid); /* The documented path change discards the failed capture. */
	const uint32_t recovered = 0x0112;
	EQ(command(0, 0x17, &recovered, 1, NULL), 0);
	CHECK(!logFileExists(valid));
	CHECK(GBASIORFUDriverFlushLog(rfu));
	char* data = readLogFile(valid);
	verifyEventLog(data, 1);
	CHECK(strstr(data, "timeoutFrames=18 retryCount=1") != NULL);
	CHECK(strstr(data, "timeoutFrames=17 retryCount=1") == NULL);
	free(data);
	GBASIORFUDriverSetLogPath(rfu, NULL);
	CHECK(remove(valid) == 0);
	end();
}

static void testAlreadyReadyHostOnlyBaseline(void) {
	begin("Disabled prearmed handoff retains the recorded host-only retry cancellation", 2);
	CHECK(!machines[0]->rfu.prearmed && !machines[1]->rfu.prearmed);
	synchronizePair();
	const uint32_t settings = 0x0120;
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	const uint32_t client[] = {4 << 8, 0x13579BDF};
	const uint32_t host[] = {4, 0x2468ACE0};
	EQ(command(1, 0x25, client, 2, NULL), 0);
	CHECK(machines[1]->rfu.radioReady);
	const uint32_t clientTime = mTimingCurrentTime(&machines[1]->gba.timing);
	const unsigned clientIrqs = machines[1]->irqs;
	EQ(commandOne(0, 0x25, host, 2, NULL), 0);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	advanceOne(0, FRAME_CYCLES / 6 + 1024);
	EQ(mTimingCurrentTime(&machines[1]->gba.timing), clientTime);
	EQ(machines[1]->irqs, clientIrqs);
	EQ(machines[0]->irqs, hostIrqs + 1);
	EQ(rx(0), 0x99660128);
	/* Baseline reproducer: the waiting owner cannot post a ticketed READY
	 * until its next CPU turn, so the finite host retry legitimately expires.
	 * The optional prearmed case below must eliminate this scheduling gap. */
	EQ(machines[0]->rfu.txAckMask, 0);
	EQ(machines[0]->rfu.clients[0].rx.count, 0);
	CHECK(!machines[0]->rfu.radioPending);
	end();
}

static struct GBASIORFUPrearmed prearmedSnapshot(unsigned id) {
	CHECK(id < MAX_GBAS);
	MutexLock(&coordinator.mutex);
	const struct GBASIORFUPrearmed snapshot = coordinator.prearmed[id];
	MutexUnlock(&coordinator.mutex);
	return snapshot;
}

static void testPrearmedHostOnlyReceipt(void) {
	for (unsigned readyCommand = 0; readyCommand < 3; ++readyCommand) {
		static const char* const names[] = {
			"Prearmed client25 snapshot completes finite host retry without a recipient CPU turn",
			"Prearmed client24 snapshot completes finite host retry without a recipient CPU turn",
			"Explicit client27 publishes a valid empty prearmed receipt without staged data"
		};
		beginPrearmed(names[readyCommand]);
		synchronizePair();
		const uint32_t settings = 0x0120;
		EQ(command(0, 0x17, &settings, 1, NULL), 0);
		const uint32_t client[] = {4 << 8, 0x13579BDF};
		const uint32_t host[] = {4, 0x2468ACE0};
		if (readyCommand == 2) EQ(command(1, 0x27, NULL, 0, NULL), 0);
		else EQ(command(1, readyCommand ? 0x24 : 0x25, client, 2, NULL), 0);
		CHECK(machines[1]->rfu.radioReady);
		const struct GBASIORFUPrearmed published = prearmedSnapshot(1);
		CHECK(published.published && !published.claimed);
		CHECK(published.generation != 0);
		EQ(published.ready.bytes, readyCommand == 2 ? 0 : 4);
		EQ(published.ready.sourceDevice, machines[1]->rfu.deviceId);
		EQ(published.ready.sourceSession, machines[1]->rfu.radioSession);
		EQ(published.ready.targetSession, machines[0]->rfu.radioSession);
		unsigned timerFirings = 0;
		struct mTimingEvent timer = {0};
		timer.context = &timerFirings;
		timer.callback = countTimerEvent;
		timer.name = "Untouched recipient regression timer";
		mTimingSchedule(&machines[1]->gba.timing, &timer, 512);
		const uint32_t clientTime = mTimingCurrentTime(&machines[1]->gba.timing);
		const unsigned clientIrqs = machines[1]->irqs;
		const uint32_t clientRX = rx(1);
		const uint16_t clientSIO = machines[1]->gba.sio.siocnt;
		const enum GBASIORFUState clientState = machines[1]->rfu.state;
		const struct mTimingEvent* clientRoot = machines[1]->gba.timing.root;
		EQ(commandOne(0, 0x25, host, 2, NULL), 0);
		const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
		finishPendingRadioHeader(0, hostIrqs, 0x99660028);
		EQ(machines[0]->rfu.txAckMask, 1);
		const struct GBASIORFUPrearmed claimed = prearmedSnapshot(1);
		CHECK(!claimed.published && claimed.claimed && !claimed.adopted && claimed.accepted);
		CHECK(claimed.generation == published.generation);
		CHECK(machines[0]->rfu.radioReply.readinessGeneration == published.generation);
		CHECK(!machines[0]->rfu.radioPending);
		EQ(machines[0]->rfu.clients[0].rx.count, readyCommand == 2 ? 0 : 1);
		if (readyCommand != 2) {
			EQ(machines[0]->rfu.clients[0].rx.packets[0].bytes, 4);
			CHECK(memcmp(machines[0]->rfu.clients[0].rx.packets[0].data, &client[1], 4) == 0);
		}
		/* A claim copies protected transport state only, not owner registers,
		 * CPU time, IRQ state, packet FIFO, or its independently queued timer. */
		advanceOne(0, FRAME_CYCLES / 6 + 1024);
		EQ(mTimingCurrentTime(&machines[1]->gba.timing), clientTime);
		EQ(machines[1]->irqs, clientIrqs);
		EQ(rx(1), clientRX);
		EQ(machines[1]->gba.sio.siocnt, clientSIO);
		EQ(machines[1]->rfu.state, clientState);
		CHECK(machines[1]->gba.timing.root == clientRoot);
		EQ(timerFirings, 0);
		EQ(machines[1]->rfu.rx.count, 0);
		EQ(machines[0]->irqs, hostIrqs + 1);
		/* Owner adoption must still clock the physical word at exactly 256.
		 * Plain SendData has no async word, but its later owner command drains
		 * the same actual CLAIM/RELEASE messages and commits its application RX. */
		if (readyCommand == 1) {
			uint32_t response[32];
			EQ(commandOne(1, 0x12, NULL, 0, response), 1);
			advanceOne(1, 4096);
		} else {
			const unsigned irqs = armAsyncWithoutAdvancing(1);
			for (unsigned polls = 0; !machines[1]->rfu.adapterPending && polls < 128; ++polls) {
				advanceOne(1, 128);
			}
			finishPendingRadioHeader(1, irqs, 0x99660028);
			EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
		}
		EQ(timerFirings, 1);
		CHECK(!machines[1]->rfu.radioPending);
		EQ(machines[1]->rfu.rx.count, 1);
		CHECK(memcmp(machines[1]->rfu.rx.packets[0].data, &host[1], 4) == 0);
		EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
		expectShortPacket(0, readyCommand == 2 ? 0 : 4, readyCommand == 2 ? 0 : client[1]);
		expectShortPacket(1, 4, host[1]);
		end();
	}
}

static void testPrearmedNoImplicitReadiness(void) {
	beginPrearmed("Prearmed handoff cannot manufacture readiness before an explicit client command");
	synchronizePair();
	const uint32_t settings = 0x0120;
	EQ(command(0, 0x17, &settings, 1, NULL), 0);
	const uint32_t host[] = {4, 0x2468ACE0};
	CHECK(!machines[1]->rfu.radioReady);
	CHECK(!prearmedSnapshot(1).published);
	EQ(commandOne(0, 0x25, host, 2, NULL), 0);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	advanceOne(0, FRAME_CYCLES / 6 + 1024);
	EQ(machines[0]->irqs, hostIrqs + 1);
	EQ(rx(0), 0x99660128);
	EQ(machines[0]->rfu.txAckMask, 0);
	EQ(machines[0]->rfu.clients[0].rx.count, 0);
	CHECK(!machines[0]->rfu.radioPending);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	end();
}

static void testPrearmedRequiresSynchronization(void) {
	beginPrearmed("Prearmed option without synchronization cannot bypass legacy transport readiness");
	connectPair();
	CHECK(machines[0]->rfu.prearmed && machines[1]->rfu.prearmed);
	CHECK(!machines[0]->rfu.synchronize && !machines[1]->rfu.synchronize);
	const uint32_t finite = 0x0120;
	const uint32_t client[] = {4 << 8, 0x11223344};
	const uint32_t host[] = {4, 0xAABBCCDD};
	EQ(command(0, 0x17, &finite, 1, NULL), 0);
	EQ(command(1, 0x25, client, 2, NULL), 0);
	CHECK(!prearmedSnapshot(1).published);
	EQ(commandOne(0, 0x25, host, 2, NULL), 0);
	const unsigned irqs = armAsyncWithoutAdvancing(0);
	advanceOne(0, FRAME_CYCLES / 6 + 1024);
	EQ(machines[0]->irqs, irqs + 1);
	EQ(rx(0), 0x99660128);
	EQ(machines[0]->rfu.txAckMask, 0);
	CHECK(!prearmedSnapshot(1).claimed);
	end();
}

static void testPrearmedLatestAndZeroSnapshot(void) {
	for (unsigned zero = 0; zero < 2; ++zero) {
		beginPrearmed(zero ? "Explicit zero client TX replaces the published prearmed data snapshot" :
			"Latest client TX replaces the published prearmed snapshot before it is claimed");
		synchronizePair();
		const uint32_t settings = 0x0120;
		EQ(command(0, 0x17, &settings, 1, NULL), 0);
		const uint32_t first[] = {4 << 8, 0x13579BDF};
		const uint32_t latest[] = {4 << 8, 0xAABBCCDD};
		const uint32_t empty = 0;
		const uint32_t host[] = {4, 0x2468ACE0};
		EQ(command(1, 0x24, first, 2, NULL), 0);
		const uint64_t previousGeneration = prearmedSnapshot(1).generation;
		EQ(command(1, 0x24, zero ? &empty : latest, zero ? 1 : 2, NULL), 0);
		const struct GBASIORFUPrearmed replacement = prearmedSnapshot(1);
		CHECK(replacement.published && !replacement.claimed);
		CHECK(replacement.generation > previousGeneration);
		EQ(replacement.ready.bytes, zero ? 0 : 4);
		EQ(commandOne(0, 0x25, host, 2, NULL), 0);
		CHECK(machines[0]->rfu.radioReply.readinessGeneration == replacement.generation);
		EQ(machines[0]->rfu.txAckMask, 1);
		EQ(machines[0]->rfu.clients[0].rx.count, zero ? 0 : 1);
		if (!zero) CHECK(memcmp(machines[0]->rfu.clients[0].rx.packets[0].data, &latest[1], 4) == 0);
		const unsigned irqs = armAsyncWithoutAdvancing(0);
		finishPendingRadioHeader(0, irqs, 0x99660028);
		EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
		advance(8192);
		expectShortPacket(0, zero ? 0 : 4, zero ? 0 : latest[1]);
		expectShortPacket(1, 4, host[1]);
		/* Merely retaining the latest TX bytes is not another readiness
		 * generation. A second host attempt gets no replayed receipt/data. */
		EQ(commandOne(0, 0x25, host, 2, NULL), 0);
		const unsigned nextIrqs = armAsyncWithoutAdvancing(0);
		advanceOne(0, FRAME_CYCLES / 6 + 1024);
		EQ(machines[0]->irqs, nextIrqs + 1);
		EQ(rx(0), 0x99660128);
		EQ(machines[0]->rfu.txAckMask, 0);
		EQ(machines[0]->rfu.clients[0].rx.count, 0);
		EQ(asynchronousReceive(0, CLOCK_FAST), 0);
		EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
		end();
	}
}

static void testPrearmedNewStagingDuringPriorAlignment(void) {
	beginPrearmed("Prior claimed escrow cannot clear newer TX or publish next readiness while aligning");
	synchronizePair();
	const uint32_t firstClient[] = {4 << 8, 0x11223344};
	const uint32_t nextClient[] = {4 << 8, 0xAABBCCDD};
	const uint32_t firstHost[] = {4, 0x55667788};
	const uint32_t nextHost[] = {4, 0x99AABBCC};
	EQ(command(1, 0x24, firstClient, 2, NULL), 0);
	advanceOne(0, FRAME_CYCLES * 2);
	EQ(commandOne(0, 0x24, firstHost, 2, NULL), 0);
	EQ(machines[0]->rfu.txAckMask, 1);
	CHECK(!machines[0]->rfu.radioPending);
	/* First owner entry adopts the claimed old snapshot. The new command
	 * stages a different payload while the old local alignment is unfinished. */
	EQ(commandOne(1, 0x24, nextClient, 2, NULL), 0);
	CHECK(machines[1]->rfu.radioPending && machines[1]->rfu.radioMatched);
	CHECK(machines[1]->rfu.radioReady && machines[1]->rfu.clientTxPending);
	CHECK(!prearmedSnapshot(1).published);
	CHECK(machines[1]->rfu.radioDue - effectiveRadioClock(1) > FRAME_CYCLES);
	CHECK(memcmp(machines[1]->rfu.tx.data, &nextClient[1], 4) == 0);
	EQ(commandOne(0, 0x25, nextHost, 2, NULL), 0);
	advanceOne(0, 4096);
	CHECK(machines[0]->rfu.radioPending);
	CHECK(!machines[0]->rfu.radioMatched);
	EQ(machines[0]->rfu.txAckMask, 0);
	advance(FRAME_CYCLES * 3);
	CHECK(!machines[0]->rfu.radioPending && !machines[1]->rfu.radioPending);
	EQ(machines[0]->rfu.txAckMask, 1);
	CHECK(memcmp(machines[0]->rfu.clients[0].rx.packets[0].data, &nextClient[1], 4) == 0);
	CHECK(memcmp(machines[1]->rfu.rx.packets[0].data, &nextHost[1], 4) == 0);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	finishPendingRadioHeader(0, hostIrqs, 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	end();
}

static void testPrearmedPublicationLifecycle(void) {
	static const char* const names[] = {
		"Reset retires prearmed publication before a host can claim it",
		"Local disconnect retires prearmed publication before a host can claim it",
		"Peer detach retires prearmed publication before a host can claim it",
		"Peripheral deinit retires prearmed publication before a host can claim it",
		"Finite client timeout retires prearmed publication before a host can claim it"
	};
	for (unsigned variant = 0; variant < COUNT_OF(names); ++variant) {
		beginPrearmed(names[variant]);
		synchronizePair();
		const uint32_t hostSettings = 0x0120;
		EQ(command(0, 0x17, &hostSettings, 1, NULL), 0);
		if (variant == 4) {
			const uint32_t clientSettings = 0x0001;
			EQ(command(1, 0x17, &clientSettings, 1, NULL), 0);
		}
		const uint32_t client[] = {4 << 8, 0x11223344};
		const uint32_t host[] = {4, 0xAABBCCDD};
		EQ(command(1, variant == 4 ? 0x25 : 0x24, client, 2, NULL), 0);
		CHECK(machines[1]->rfu.radioReady);
		const uint32_t slot0 = 1;
		switch (variant) {
		case 0: GBASIOReset(&machines[1]->gba.sio); break;
		case 1: EQ(commandOne(1, 0x30, &slot0, 1, NULL), 0); break;
		case 2: GBASIORFUCoordinatorDetach(&coordinator, &machines[1]->rfu); break;
		case 3: GBASIODeinit(&machines[1]->gba.sio); break;
		case 4: {
			const unsigned irqs = armAsyncWithoutAdvancing(1);
			advanceOne(1, FRAME_CYCLES + 4096);
			EQ(machines[1]->irqs, irqs + 1);
			CHECK(!machines[1]->rfu.radioReady);
			break;
		}
		}
		CHECK(!prearmedSnapshot(1).published);
		EQ(commandOne(0, 0x25, host, 2, NULL), 0);
		advanceOne(0, FRAME_CYCLES / 6 + 1024);
		EQ(machines[0]->rfu.txAckMask, 0);
		EQ(machines[0]->rfu.clients[0].rx.count, 0);
		CHECK(!machines[0]->rfu.radioMatched);
		end();
	}
}

static void testPrearmedFullMailboxRecovery(void) {
	for (unsigned available = 0; available < 2; ++available) {
		beginPrearmed(available ? "One available mailbox slot cannot manufacture a receipt before RELEASE fits" :
			"Full recipient mailbox cannot consume a prearmed snapshot or manufacture an ACK");
		synchronizePair();
		const uint32_t client[] = {4 << 8, 0x11223344};
		const uint32_t host[] = {4, 0xAABBCCDD};
		EQ(command(1, 0x25, client, 2, NULL), 0);
		const uint64_t generation = prearmedSnapshot(1).generation;
		struct GBASIORFUDriver* receiver = &machines[1]->rfu;
		MutexLock(&coordinator.mutex);
		memset(receiver->incoming, 0, sizeof(receiver->incoming));
		receiver->incomingHead = 0;
		receiver->incomingCount = GBA_RFU_MESSAGE_DEPTH - available;
		MutexUnlock(&coordinator.mutex);
		EQ(commandOne(0, 0x25, host, 2, NULL), 0);
		const uint64_t ticket = machines[0]->rfu.radioTicket;
		const uint32_t epoch = machines[0]->rfu.txEpoch;
		advanceOne(0, 4096);
		CHECK(machines[0]->rfu.radioPending);
		if (!available) CHECK(!machines[0]->rfu.radioMatched);
		EQ(machines[0]->rfu.txAckMask, 0);
		EQ(machines[0]->rfu.clients[0].rx.count, 0);
		CHECK(receiver->radioReady && receiver->clientTxPending);
		if (!available) {
			const struct GBASIORFUPrearmed blocked = prearmedSnapshot(1);
			CHECK(blocked.published && !blocked.claimed && blocked.generation == generation);
		}
		/* Remove only inert capacity placeholders, retaining any genuine CLAIM
		 * or OFFER the one spare slot permitted. No recipient core turn occurs. */
		MutexLock(&coordinator.mutex);
		struct GBASIORFUMessage retained[GBA_RFU_MESSAGE_DEPTH];
		unsigned count = 0;
		for (unsigned i = 0; i < receiver->incomingCount; ++i) {
			const struct GBASIORFUMessage message = receiver->incoming[(receiver->incomingHead + i) % GBA_RFU_MESSAGE_DEPTH];
			if (message.radioTicket || message.sourceSession) retained[count++] = message;
		}
		receiver->incomingHead = 0;
		receiver->incomingCount = count;
		memcpy(receiver->incoming, retained, count * sizeof(retained[0]));
		MutexUnlock(&coordinator.mutex);
		advanceOne(0, 1024);
		CHECK(machines[0]->rfu.radioTicket == ticket);
		EQ(machines[0]->rfu.txEpoch, epoch);
		EQ(machines[0]->rfu.txAckMask, 1);
		CHECK(!machines[0]->rfu.radioPending);
		CHECK(memcmp(machines[0]->rfu.clients[0].rx.packets[0].data, &client[1], 4) == 0);
		finishSynchronizedRound(true);
		expectShortPacket(0, 4, client[1]);
		expectShortPacket(1, 4, host[1]);
		end();
	}
}

static void testPrearmedClaimSessionReplay(void) {
	beginPrearmed("Captured prearmed CLAIM and RELEASE cannot enter a reset session");
	synchronizePair();
	const uint32_t oldClient[] = {4 << 8, 0x11223344};
	const uint32_t oldHost[] = {4, 0x55667788};
	EQ(command(1, 0x24, oldClient, 2, NULL), 0);
	EQ(commandOne(0, 0x24, oldHost, 2, NULL), 0);
	const uint64_t ticket = machines[0]->rfu.radioTicket;
	EQ(machines[0]->rfu.txAckMask, 1);
	const struct GBASIORFUMessage claim = holdRadioMessage(1, ticket, 0);
	const struct GBASIORFUMessage release = holdRadioMessage(1, ticket, 0);
	CHECK(claim.readinessGeneration != 0);
	for (unsigned id = 0; id < 2; ++id) {
		GBASIOReset(&machines[id]->gba.sio);
		GBASIOWriteRCNT(&machines[id]->gba.sio, 0);
	}
	connectPair();
	enableSynchronization();
	CHECK(claim.targetSession != machines[1]->rfu.radioSession);
	deliverRadioMessage(1, &claim);
	deliverRadioMessage(1, &release);
	uint32_t response[32];
	EQ(command(1, 0x12, NULL, 0, response), 1);
	CHECK(!machines[1]->rfu.radioPending && !machines[1]->rfu.radioMatched);
	EQ(machines[1]->rfu.rx.count, 0);
	EQ(machines[0]->rfu.txAckMask, 0);
	const uint32_t newClient[] = {4 << 8, 0x99AABBCC};
	const uint32_t newHost[] = {4, 0xDDEEFF00};
	EQ(command(1, 0x25, newClient, 2, NULL), 0);
	EQ(commandOne(0, 0x25, newHost, 2, NULL), 0);
	finishSynchronizedRound(true);
	expectShortPacket(0, 4, newClient[1]);
	expectShortPacket(1, 4, newHost[1]);
	end();
}

static void testPrearmedClaimedLocalTimeout(void) {
	beginPrearmed("Claimed prearmed receipt preserves finite client timeout and explicit retry recovery");
	synchronizePair();
	const uint32_t finite = 0x0001;
	const uint32_t unlimited = 0;
	const uint32_t client[] = {4 << 8, 0x12345678};
	const uint32_t oldHost[] = {4, 0x11223344};
	const uint32_t newHost[] = {4, 0xAABBCCDD};
	EQ(command(1, 0x17, &finite, 1, NULL), 0);
	EQ(command(1, 0x25, client, 2, NULL), 0);
	advanceOne(0, FRAME_CYCLES * 3);
	EQ(commandOne(0, 0x24, oldHost, 2, NULL), 0);
	EQ(machines[0]->rfu.txAckMask, 1);
	const unsigned irqs = armAsyncWithoutAdvancing(1);
	advanceOne(1, FRAME_CYCLES + 4096);
	EQ(machines[1]->irqs, irqs + 1);
	EQ(rx(1), 0x99660027);
	CHECK(!machines[1]->rfu.radioPending && !machines[1]->rfu.radioReady);
	CHECK(!prearmedSnapshot(1).published);
	CHECK(machines[1]->rfu.clientTxPending);
	EQ(machines[1]->rfu.tx.bytes, 4);
	CHECK(memcmp(machines[1]->rfu.tx.data, &client[1], 4) == 0);
	EQ(machines[1]->rfu.rx.count, 0);
	/* The receipt committed before the later owner-local timeout is real;
	 * cancellation cannot retroactively remove its host-side ACK or data. */
	EQ(machines[0]->rfu.txAckMask, 1);
	CHECK(memcmp(machines[0]->rfu.clients[0].rx.packets[0].data, &client[1], 4) == 0);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(command(1, 0x17, &unlimited, 1, NULL), 0);
	EQ(command(1, 0x37, NULL, 0, NULL), 0);
	CHECK(prearmedSnapshot(1).published);
	EQ(commandOne(0, 0x24, newHost, 2, NULL), 0);
	finishSynchronizedRound(false);
	expectShortPacket(0, 4, client[1]);
	expectShortPacket(1, 4, newHost[1]);
	end();
}

static void testPrearmedFullReleaseCancellation(void) {
	beginPrearmed("Full RELEASE mailbox cancellation tombstones a claim even when CANCEL cannot be queued");
	synchronizePair();
	const uint32_t finite = 0x0120;
	const uint32_t client[] = {4 << 8, 0x12345678};
	const uint32_t oldHost[] = {4, 0x11223344};
	const uint32_t newHost[] = {4, 0xAABBCCDD};
	EQ(command(0, 0x17, &finite, 1, NULL), 0);
	EQ(command(1, 0x25, client, 2, NULL), 0);
	struct GBASIORFUDriver* receiver = &machines[1]->rfu;
	MutexLock(&coordinator.mutex);
	memset(receiver->incoming, 0, sizeof(receiver->incoming));
	receiver->incomingHead = 0;
	receiver->incomingCount = GBA_RFU_MESSAGE_DEPTH - 1;
	MutexUnlock(&coordinator.mutex);
	EQ(commandOne(0, 0x25, oldHost, 2, NULL), 0);
	CHECK(machines[0]->rfu.radioMatched && machines[0]->rfu.radioPending);
	EQ(machines[0]->rfu.txAckMask, 0);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	advanceOne(0, FRAME_CYCLES / 6 + 1024);
	EQ(machines[0]->irqs, hostIrqs + 1);
	EQ(rx(0), 0x99660128);
	CHECK(!machines[0]->rfu.radioPending);
	EQ(machines[0]->rfu.txAckMask, 0);
	EQ(receiver->incomingCount, GBA_RFU_MESSAGE_DEPTH);
	const struct GBASIORFUPrearmed cancelled = prearmedSnapshot(1);
	CHECK(cancelled.cancelled && !cancelled.published);
	CHECK(receiver->radioReady && receiver->clientTxPending);
	const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
	finishPendingRadioHeader(1, clientIrqs, 0x99660027);
	CHECK(!receiver->radioReady && !receiver->radioPending);
	CHECK(receiver->clientTxPending);
	CHECK(memcmp(receiver->tx.data, &client[1], 4) == 0);
	EQ(receiver->rx.count, 0);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	EQ(command(1, 0x37, NULL, 0, NULL), 0);
	EQ(commandOne(0, 0x24, newHost, 2, NULL), 0);
	finishSynchronizedRound(false);
	expectShortPacket(0, 4, client[1]);
	expectShortPacket(1, 4, newHost[1]);
	end();
}

static void testPrearmedRetirementBeforeHostAcceptance(void) {
	for (unsigned detach = 0; detach < 2; ++detach) {
		beginPrearmed(detach ? "Peer detach between prearmed claim and READY acceptance prevents any receipt" :
			"Client reset between prearmed claim and READY acceptance prevents any receipt");
		synchronizePair();
		const uint32_t client[] = {4 << 8, 0x12345678};
		const uint32_t host[] = {4, 0xAABBCCDD};
		EQ(command(1, 0x24, client, 2, NULL), 0);
		/* The last payload SPI word emits CLAIM+READY. Stopping before the
		 * response SPI word leaves the actual READY queued and unaccepted. */
		EQ(transferOne(0, 0x99660224), 0x80000000);
		EQ(transferOne(0, host[0]), 0x80000000);
		EQ(transferOne(0, host[1]), 0x80000000);
		const struct GBASIORFUPrearmed claimed = prearmedSnapshot(1);
		CHECK(claimed.claimed && !claimed.accepted && !claimed.released);
		CHECK(machines[0]->rfu.radioPending && !machines[0]->rfu.radioMatched);
		if (detach) GBASIORFUCoordinatorDetach(&coordinator, &machines[1]->rfu);
		else GBASIOReset(&machines[1]->gba.sio);
		EQ(transferOne(0, 0x80000000), 0x996600A4);
		advanceOne(0, FRAME_CYCLES * 2);
		CHECK(!machines[0]->rfu.radioMatched);
		EQ(machines[0]->rfu.txAckMask, 0);
		EQ(machines[0]->rfu.clients[0].rx.count, 0);
		end();
	}
}

static void testPrearmedCommittedReceiptSurvivesHostRetirement(void) {
	for (unsigned detach = 0; detach < 2; ++detach) {
		beginPrearmed(detach ? "Committed prearmed RELEASE survives host detach before recipient adoption" :
			"Committed prearmed RELEASE survives host reset before recipient adoption");
		synchronizePair();
		const uint32_t client[] = {4 << 8, 0x12345678};
		const uint32_t host[] = {4, 0xAABBCCDD};
		EQ(command(1, 0x24, client, 2, NULL), 0);
		EQ(commandOne(0, 0x24, host, 2, NULL), 0);
		const struct GBASIORFUPrearmed committed = prearmedSnapshot(1);
		CHECK(committed.claimed && committed.accepted && committed.released && !committed.adopted);
		EQ(machines[0]->rfu.txAckMask, 1);
		if (detach) GBASIORFUCoordinatorDetach(&coordinator, &machines[0]->rfu);
		else GBASIOReset(&machines[0]->gba.sio);
		CHECK(!prearmedSnapshot(1).cancelled);
		/* Observe the earlier committed packet before a later disconnect
		 * deliberately clears session RX. Hold only the genuine subsequent
		 * disconnect record; do not fabricate or reorder CLAIM/RELEASE. */
		const struct GBASIORFUMessage disconnect = holdRadioMessage(1, 0, 0);
		uint32_t response[32];
		EQ(commandOne(1, 0x12, NULL, 0, response), 1);
		advanceOne(1, 4096);
		CHECK(!machines[1]->rfu.radioPending);
		EQ(machines[1]->rfu.rx.count, 1);
		CHECK(memcmp(machines[1]->rfu.rx.packets[0].data, &host[1], 4) == 0);
		deliverRadioMessage(1, &disconnect);
		EQ(commandOne(1, 0x12, NULL, 0, response), 1);
		EQ(machines[1]->rfu.role, GBA_RFU_IDLE);
		EQ(machines[1]->rfu.rx.count, 0);
		end();
	}
}

static void testPrearmedRetiredClaimWithFullHostMailbox(void) {
	beginPrearmed("Client reset cancels a matched prearmed host even when its CANCEL mailbox is full");
	synchronizePair();
	const uint32_t client[] = {4 << 8, 0x12345678};
	const uint32_t host[] = {4, 0xAABBCCDD};
	advanceOne(1, FRAME_CYCLES * 3);
	EQ(commandOne(1, 0x24, client, 2, NULL), 0);
	EQ(commandOne(0, 0x24, host, 2, NULL), 0);
	CHECK(machines[0]->rfu.radioMatched && machines[0]->rfu.radioPending);
	CHECK(machines[0]->rfu.radioDue - effectiveRadioClock(0) > FRAME_CYCLES);
	EQ(machines[0]->rfu.txAckMask, 0);
	MutexLock(&coordinator.mutex);
	memset(machines[0]->rfu.incoming, 0, sizeof(machines[0]->rfu.incoming));
	machines[0]->rfu.incomingHead = 0;
	machines[0]->rfu.incomingCount = GBA_RFU_MESSAGE_DEPTH;
	MutexUnlock(&coordinator.mutex);
	GBASIOReset(&machines[1]->gba.sio);
	EQ(machines[0]->rfu.incomingCount, GBA_RFU_MESSAGE_DEPTH);
	advanceOne(0, FRAME_CYCLES * 4);
	CHECK(!machines[0]->rfu.radioPending && !machines[0]->rfu.radioMatched);
	EQ(machines[0]->rfu.txAckMask, 0);
	EQ(machines[0]->rfu.clients[0].rx.count, 0);
	end();
}

static void testPrearmedStandaloneWaitWithUnreadRX(void) {
	beginPrearmed("Standalone client27 keeps immediate old RX semantics and withdraws completed WAIT publication");
	synchronizePair();
	const uint32_t firstClient[] = {4 << 8, 0x12345678};
	const uint32_t nextClient[] = {4 << 8, 0x99AABBCC};
	const uint32_t oldHost[] = {4, 0x11223344};
	const uint32_t nextHost[] = {4, 0xDDEEFF00};
	const uint32_t finite = 0x0120;
	EQ(command(1, 0x24, firstClient, 2, NULL), 0);
	EQ(commandOne(0, 0x24, oldHost, 2, NULL), 0);
	uint32_t response[32];
	EQ(commandOne(1, 0x12, NULL, 0, response), 1);
	advance(8192);
	expectShortPacket(0, 4, firstClient[1]);
	EQ(machines[1]->rfu.rx.count, 1); /* Leave the old host packet unread. */
	EQ(command(0, 0x17, &finite, 1, NULL), 0);
	EQ(commandOne(1, 0x27, NULL, 0, NULL), 0);
	const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
	finishPendingRadioHeader(1, clientIrqs, 0x99660028);
	CHECK(!prearmedSnapshot(1).published);
	CHECK(!machines[1]->rfu.radioReady);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(machines[1]->rfu.state, GBA_RFU_COMMAND);
	CHECK(!prearmedSnapshot(1).published);
	/* No new host OFFER ran concurrently with the old-RX response. A later
	 * offer cannot claim a generation from the already completed 27 WAIT. */
	EQ(commandOne(0, 0x25, nextHost, 2, NULL), 0);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	advanceOne(0, FRAME_CYCLES / 6 + 1024);
	EQ(machines[0]->irqs, hostIrqs + 1);
	EQ(rx(0), 0x99660128);
	EQ(machines[0]->rfu.txAckMask, 0);
	EQ(machines[0]->rfu.clients[0].rx.count, 0);
	EQ(machines[1]->rfu.rx.count, 1);
	CHECK(memcmp(machines[1]->rfu.rx.packets[0].data, &oldHost[1], 4) == 0);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	expectShortPacket(1, 4, oldHost[1]);
	EQ(command(1, 0x25, nextClient, 2, NULL), 0);
	EQ(commandOne(0, 0x37, NULL, 0, NULL), 0);
	finishSynchronizedRound(true);
	expectShortPacket(0, 4, nextClient[1]);
	expectShortPacket(1, 4, nextHost[1]);
	end();
}

static uint32_t expectedPacketFingerprint(const uint32_t* packet, unsigned bytes) {
	uint32_t fingerprint = 2166136261U;
	for (unsigned i = 0; i < bytes; ++i) {
		const uint8_t byte = packet[1 + i / 4] >> ((i % 4) * 8);
		fingerprint = (fingerprint ^ byte) * 16777619U;
	}
	return fingerprint;
}

static const char* requireLogEvent(const char* data, const char* event, const char* fields) {
	const char* found = strstr(data, event);
	CHECK(found != NULL);
	const char* end = strchr(found, '\n');
	CHECK(end != NULL);
	const char* match = strstr(found, fields);
	CHECK(match != NULL && match < end);
	return found;
}

static void requireSnapshotEvent(const char* data, const char* event, uint64_t ticket, uint32_t epoch,
	unsigned hostBytes, uint32_t hostFingerprint, unsigned clientBytes, uint32_t clientFingerprint) {
	char needle[128], fields[160];
	snprintf(needle, sizeof(needle), "event %s ticket=%llu epoch=%u ", event, (unsigned long long) ticket, epoch);
	snprintf(fields, sizeof(fields), "hostBytes=%u hostFingerprint=%08X clientBytes=%u clientFingerprint=%08X",
		hostBytes, hostFingerprint, clientBytes, clientFingerprint);
	requireLogEvent(data, needle, fields);
}

static void testPacketFingerprintRoundTrip(void) {
	beginPrearmed("Buffered fingerprints correlate whole immutable 90-byte host and 16-byte client packets");
	synchronizePair();
	char paths[2][1024];
	for (unsigned id = 0; id < 2; ++id) {
		unusedLogPath(paths[id], sizeof(paths[id]), id ? "fingerprint-client" : "fingerprint-host");
		GBASIORFUDriverSetLogPath(&machines[id]->rfu, paths[id]);
	}
	uint32_t host[24] = {90};
	uint32_t client[5] = {16 << 8};
	for (unsigned i = 1; i < COUNT_OF(host); ++i) host[i] = 0xA5010203U ^ (i * 0x10204081U);
	for (unsigned i = 1; i < COUNT_OF(client); ++i) client[i] = 0xB5020408U ^ (i * 0x08102041U);
	const uint32_t hostFingerprint = expectedPacketFingerprint(host, 90);
	const uint32_t clientFingerprint = expectedPacketFingerprint(client, 16);
	EQ(command(1, 0x25, client, COUNT_OF(client), NULL), 0);
	EQ(commandOne(0, 0x25, host, COUNT_OF(host), NULL), 0);
	const uint64_t ticket = machines[0]->rfu.radioTicket;
	const uint32_t epoch = machines[0]->rfu.radioOffer.txEpoch;
	finishSynchronizedRound(true);
	uint32_t response[32];
	EQ(command(1, 0x26, NULL, 0, response), 24);
	EQ(response[0], 90);
	for (unsigned i = 0; i < 90; ++i) EQ((response[1 + i / 4] >> ((i % 4) * 8)) & 0xFF,
		(host[1 + i / 4] >> ((i % 4) * 8)) & 0xFF);
	EQ(command(0, 0x26, NULL, 0, response), 5);
	EQ(response[0], 16 << 8);
	for (unsigned i = 0; i < 4; ++i) EQ(response[1 + i], client[1 + i]);
	char* logs[2];
	for (unsigned id = 0; id < 2; ++id) {
		CHECK(!logFileExists(paths[id])); /* Packet capture never writes in callbacks. */
		CHECK(GBASIORFUDriverFlushLog(&machines[id]->rfu));
		logs[id] = readLogFile(paths[id]);
		CHECK(strstr(logs[id], "# RFU events v2 ") != NULL);
		CHECK(strstr(logs[id], "capacity=16777216 fileLimit=134217728 fingerprint=fnv1a32") != NULL);
		requireSnapshotEvent(logs[id], "radioComplete", ticket, epoch, 90, hostFingerprint, 16, clientFingerprint);
	}
	char fields[128], event[128];
	snprintf(fields, sizeof(fields), "fingerprint=%08X", hostFingerprint);
	requireLogEvent(logs[0], "event send role=1 bytes=90 ", fields);
	snprintf(event, sizeof(event), "event radioOffer ticket=%llu epoch=%u bytes=90 ", (unsigned long long) ticket, epoch);
	requireLogEvent(logs[0], event, fields);
	requireLogEvent(logs[1], "event readPacket bytes=90 ", fields);
	snprintf(fields, sizeof(fields), "fingerprint=%08X", clientFingerprint);
	requireLogEvent(logs[1], "event send role=3 bytes=16 ", fields);
	requireLogEvent(logs[1], "event prearmedPublish ", fields);
	requireLogEvent(logs[1], "event prearmedAdopt ", fields);
	requireLogEvent(logs[0], "event readPacket bytes=16 ", fields);
	snprintf(event, sizeof(event), "event radioComplete ticket=%llu epoch=%u ", (unsigned long long) ticket, epoch);
	requireLogEvent(logs[0], event, "ack=01 targets=01");
	for (unsigned id = 0; id < 2; ++id) {
		free(logs[id]);
		GBASIORFUDriverSetLogPath(&machines[id]->rfu, NULL);
		CHECK(remove(paths[id]) == 0);
	}
	end();
}

static void testPacketFingerprintNewerStagedTX(void) {
	beginPrearmed("Packet diagnostics hash immutable claimed snapshots rather than newer staged TX");
	synchronizePair();
	char paths[2][1024];
	for (unsigned id = 0; id < 2; ++id) {
		unusedLogPath(paths[id], sizeof(paths[id]), id ? "fingerprint-new-client" : "fingerprint-new-host");
		GBASIORFUDriverSetLogPath(&machines[id]->rfu, paths[id]);
	}
	const uint32_t firstClient[] = {4 << 8, 0x12345678};
	const uint32_t nextClient[] = {4 << 8, 0xAABBCCDD};
	const uint32_t firstHost[] = {4, 0x11223344};
	const uint32_t nextHost[] = {4, 0x55667788};
	EQ(command(1, 0x24, firstClient, 2, NULL), 0);
	advanceOne(0, FRAME_CYCLES * 2);
	EQ(commandOne(0, 0x24, firstHost, 2, NULL), 0);
	const uint64_t firstTicket = machines[0]->rfu.radioTicket;
	const uint32_t firstEpoch = machines[0]->rfu.radioOffer.txEpoch;
	EQ(commandOne(1, 0x24, nextClient, 2, NULL), 0);
	CHECK(machines[1]->rfu.radioPending && machines[1]->rfu.clientTxPending);
	EQ(commandOne(0, 0x25, nextHost, 2, NULL), 0);
	const uint64_t nextTicket = machines[0]->rfu.radioTicket;
	const uint32_t nextEpoch = machines[0]->rfu.radioOffer.txEpoch;
	advance(FRAME_CYCLES * 3);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	finishPendingRadioHeader(0, hostIrqs, 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	expectShortPacket(0, 4, nextClient[1]);
	expectShortPacket(1, 4, nextHost[1]);
	for (unsigned id = 0; id < 2; ++id) {
		CHECK(!logFileExists(paths[id]));
		CHECK(GBASIORFUDriverFlushLog(&machines[id]->rfu));
		char* data = readLogFile(paths[id]);
		requireSnapshotEvent(data, "radioComplete", firstTicket, firstEpoch, 4, expectedPacketFingerprint(firstHost, 4),
			4, expectedPacketFingerprint(firstClient, 4));
		requireSnapshotEvent(data, "radioComplete", nextTicket, nextEpoch, 4, expectedPacketFingerprint(nextHost, 4),
			4, expectedPacketFingerprint(nextClient, 4));
		char fields[64];
		snprintf(fields, sizeof(fields), "fingerprint=%08X", expectedPacketFingerprint(id ? nextHost : nextClient, 4));
		requireLogEvent(data, "event readPacket bytes=4 ", fields);
		if (id == 1) {
			snprintf(fields, sizeof(fields), "fingerprint=%08X", expectedPacketFingerprint(firstClient, 4));
			requireLogEvent(data, "event prearmedAdopt ", fields);
		}
		free(data);
		GBASIORFUDriverSetLogPath(&machines[id]->rfu, NULL);
		CHECK(remove(paths[id]) == 0);
	}
	end();
}

static void testPacketFingerprintCancelledTicket(void) {
	begin("Cancelled ticket diagnostics retain failed snapshot without fabricating commit/read hashes", 2);
	char paths[2][1024];
	for (unsigned id = 0; id < 2; ++id) {
		unusedLogPath(paths[id], sizeof(paths[id]), id ? "fingerprint-cancel-client" : "fingerprint-cancel-host");
		GBASIORFUDriverSetLogPath(&machines[id]->rfu, paths[id]);
	}
	const struct GBASIORFUMessage cancel = prepareSynchronizedRemoteCancellation();
	const uint32_t failedHost[] = {4, 0x11223344};
	const uint32_t client[] = {4 << 8, 0x55667788};
	const uint32_t newHost[] = {4, 0x90ABCDEF};
	deliverRadioMessage(1, &cancel);
	mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.pollEvent, 0);
	mTimingSchedule(&machines[1]->gba.timing, &machines[1]->rfu.maintenanceEvent, FRAME_CYCLES);
	const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
	advanceOne(1, 512);
	EQ(machines[1]->irqs, clientIrqs + 1);
	EQ(rx(1), 0x99660027);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(command(0, 0x24, newHost, 2, NULL), 0);
	const uint64_t ticket = machines[0]->rfu.radioTicket;
	const uint32_t epoch = machines[0]->rfu.radioOffer.txEpoch;
	EQ(command(1, 0x37, NULL, 0, NULL), 0);
	finishSynchronizedRound(false);
	expectShortPacket(0, 4, client[1]);
	expectShortPacket(1, 4, newHost[1]);
	for (unsigned id = 0; id < 2; ++id) {
		CHECK(!logFileExists(paths[id]));
		CHECK(GBASIORFUDriverFlushLog(&machines[id]->rfu));
		char* data = readLogFile(paths[id]);
		char event[128], fields[128];
		snprintf(event, sizeof(event), "event radioComplete ticket=%llu ", (unsigned long long) cancel.radioTicket);
		CHECK(strstr(data, event) == NULL);
		requireSnapshotEvent(data, "radioComplete", ticket, epoch, 4, expectedPacketFingerprint(newHost, 4),
			4, expectedPacketFingerprint(client, 4));
		snprintf(fields, sizeof(fields), "fingerprint=%08X", expectedPacketFingerprint(id ? newHost : client, 4));
		requireLogEvent(data, "event readPacket bytes=4 ", fields);
		if (!id) {
			snprintf(event, sizeof(event), "event radioOffer ticket=%llu epoch=%u bytes=4 ",
				(unsigned long long) cancel.radioTicket, cancel.txEpoch);
			snprintf(fields, sizeof(fields), "fingerprint=%08X", expectedPacketFingerprint(failedHost, 4));
			requireLogEvent(data, event, fields);
			snprintf(fields, sizeof(fields), "hostBytes=4 hostFingerprint=%08X", expectedPacketFingerprint(failedHost, 4));
			requireLogEvent(data, "event radioCancel notify=1 ", fields);
		}
		free(data);
		GBASIORFUDriverSetLogPath(&machines[id]->rfu, NULL);
		CHECK(remove(paths[id]) == 0);
	}
	end();
}

static void testCompletedNoticeWithQueuedOffer(void) {
	for (unsigned fallback = 0; fallback < 2; ++fallback) {
		beginConfigured(fallback ? "Ordinary synchronized completed A survives normal queued OFFER B" :
			"Completed A emits one 256-cycle success word despite unready normal OFFER B", 2, !fallback);
		synchronizePair();
		const uint32_t finite = 0x0120;
		const uint32_t clientA[] = {4 << 8, 0x12345678};
		const uint32_t hostA[] = {4, 0x11223344};
		const uint32_t hostB[] = {4, 0x55667788};
		EQ(command(0, 0x17, &finite, 1, NULL), 0);
		EQ(command(1, 0x25, clientA, 2, NULL), 0);
		EQ(commandOne(0, 0x24, hostA, 2, NULL), 0);
		if (fallback) advanceOne(1, 128); /* Generate the real legacy READY owner-side. */
		advanceOne(0, 8192);
		CHECK(!machines[0]->rfu.radioPending);
		EQ(machines[0]->rfu.txAckMask, 1);
		const uint64_t ticketA = machines[0]->rfu.radioTicket;
		EQ(commandOne(0, 0x25, hostB, 2, NULL), 0);
		const uint64_t ticketB = machines[0]->rfu.radioTicket;
		CHECK(ticketB > ticketA && !machines[0]->rfu.radioMatched);
		const unsigned clientIrqs = armAsyncWithoutAdvancing(1);
		for (unsigned polls = 0; !machines[1]->rfu.adapterPending && polls < 32; ++polls) advanceOne(1, 128);
		/* Baseline drops radioNotice before _asyncReply rejects pending B.
		 * The completed A WAIT must instead queue its real success word. */
		CHECK(machines[1]->rfu.adapterPending);
		finishPendingRadioHeader(1, clientIrqs, 0x99660028);
		CHECK(machines[1]->rfu.radioPending && machines[1]->rfu.radioTicket == ticketB);
		CHECK(!machines[1]->rfu.radioMatched);
		EQ(machines[0]->rfu.txAckMask, 0);
		EQ(machines[1]->rfu.rx.count, 1);
		CHECK(memcmp(machines[1]->rfu.rx.packets[0].data, &hostA[1], 4) == 0);
		EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
		const unsigned afterA = machines[1]->irqs;
		const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
		advanceOne(0, FRAME_CYCLES / 6 + 1024);
		EQ(machines[0]->irqs, hostIrqs + 1);
		EQ(rx(0), 0x99660128);
		EQ(machines[0]->rfu.txAckMask, 0);
		advanceOne(1, 512);
		EQ(machines[1]->irqs, afterA);
		EQ(machines[1]->rfu.state, GBA_RFU_COMMAND);
		expectShortPacket(1, 4, hostA[1]);
		end();
	}
}

static void testCompletedNoticeCancellationTiming(void) {
	static const char* const names[] = {
		"B cancellation before A clock handoff cannot replace completed A success",
		"B cancellation during A's 256-cycle serial word cannot replace its latched success",
		"B cancellation after A header cannot replace its pending success terminator"
	};
	for (unsigned variant = 0; variant < COUNT_OF(names); ++variant) {
		beginPrearmed(names[variant]);
		synchronizePair();
		const uint32_t finite = 0x0120;
		const uint32_t clientA[] = {4 << 8, 0x12345678};
		const uint32_t clientRetry[] = {4 << 8, 0xAABBCCDD};
		const uint32_t hostA[] = {4, 0x11223344};
		const uint32_t hostB[] = {4, 0x55667788};
		EQ(command(0, 0x17, &finite, 1, NULL), 0);
		EQ(command(1, 0x25, clientA, 2, NULL), 0);
		EQ(commandOne(0, 0x24, hostA, 2, NULL), 0);
		advanceOne(0, 8192);
		EQ(machines[0]->rfu.txAckMask, 1);
		EQ(commandOne(0, 0x25, hostB, 2, NULL), 0);
		const uint64_t ticketB = machines[0]->rfu.radioTicket;
		unsigned clientIrqs;
		if (variant == 0) {
			clientIrqs = machines[1]->irqs;
			CHECK(machines[1]->gba.sio.siocnt & CLOCK_INTERNAL);
			advanceOne(1, 4096); /* Commit A, but keep the GBA owning the SPI clock. */
			CHECK(machines[1]->rfu.radioNotice);
			CHECK(!machines[1]->rfu.adapterPending);
		} else {
			clientIrqs = armAsyncWithoutAdvancing(1);
			for (unsigned polls = 0; !machines[1]->rfu.adapterPending && polls < 32; ++polls) advanceOne(1, 128);
			CHECK(machines[1]->rfu.adapterPending);
			if (variant == 1) {
				EQ(mTimingUntil(&machines[1]->gba.timing, &machines[1]->rfu.adapterEvent), 256);
				advanceOne(1, 255);
				EQ(machines[1]->irqs, clientIrqs);
			} else finishPendingRadioHeader(1, clientIrqs, 0x99660028);
		}
		CHECK(machines[1]->rfu.radioPending && machines[1]->rfu.radioTicket == ticketB);
		CHECK(!machines[1]->rfu.radioMatched);
		const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
		advanceOne(0, FRAME_CYCLES / 6 + 1024);
		EQ(machines[0]->irqs, hostIrqs + 1);
		EQ(rx(0), 0x99660128);
		EQ(machines[0]->rfu.txAckMask, 0);
		if (variant == 0) {
			advanceOne(1, 512); /* Drain B's CANCEL before old A can form an async word. */
			EQ(machines[1]->irqs, clientIrqs);
			const unsigned beforeA = armAsyncWithoutAdvancing(1);
			EQ(beforeA, clientIrqs);
			for (unsigned polls = 0; !machines[1]->rfu.adapterPending && polls < 32; ++polls) advanceOne(1, 128);
			finishPendingRadioHeader(1, clientIrqs, 0x99660028);
		} else if (variant == 1) {
			advanceOne(1, 1);
			EQ(machines[1]->irqs, clientIrqs + 1);
			EQ(rx(1), 0x99660028);
		} else {
			advanceOne(1, 512);
			EQ(machines[1]->irqs, clientIrqs + 1);
			EQ(rx(1), 0x99660028);
		}
		EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
		const unsigned afterA = machines[1]->irqs;
		advanceOne(1, 4096);
		EQ(machines[1]->irqs, afterA);
		EQ(machines[1]->rfu.state, GBA_RFU_COMMAND);
		EQ(machines[1]->rfu.rx.count, 1);
		CHECK(memcmp(machines[1]->rfu.rx.packets[0].data, &hostA[1], 4) == 0);
		EQ(asynchronousReceive(0, CLOCK_FAST), 0);
		EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
		expectShortPacket(1, 4, hostA[1]);
		EQ(command(1, 0x25, clientRetry, 2, NULL), 0);
		EQ(commandOne(0, 0x37, NULL, 0, NULL), 0);
		finishSynchronizedRound(true);
		expectShortPacket(1, 4, hostB[1]);
		expectShortPacket(0, 4, clientRetry[1]);
		end();
	}
}

static void testCompletedNoticeCannotFinishNewTXWait(void) {
	beginPrearmed("Old A completion cannot finish the newer client25 TX WAIT before its B exchange");
	synchronizePair();
	const uint32_t clientA[] = {4 << 8, 0x12345678};
	const uint32_t clientB[] = {4 << 8, 0xAABBCCDD};
	const uint32_t hostA[] = {4, 0x11223344};
	const uint32_t hostB[] = {4, 0x55667788};
	EQ(command(1, 0x24, clientA, 2, NULL), 0);
	advanceOne(0, FRAME_CYCLES * 2);
	EQ(commandOne(0, 0x24, hostA, 2, NULL), 0);
	EQ(commandOne(1, 0x25, clientB, 2, NULL), 0);
	CHECK(machines[1]->rfu.radioPending && machines[1]->rfu.clientTxPending);
	const unsigned irqs = armAsyncWithoutAdvancing(1);
	advance(FRAME_CYCLES * 3);
	EQ(machines[1]->irqs, irqs);
	CHECK(!machines[1]->rfu.adapterPending);
	CHECK(machines[1]->rfu.clientWaitNeedsExchange && machines[1]->rfu.clientTxPending);
	EQ(machines[1]->rfu.rx.count, 1);
	CHECK(memcmp(machines[1]->rfu.rx.packets[0].data, &hostA[1], 4) == 0);
	EQ(commandOne(0, 0x25, hostB, 2, NULL), 0);
	awaitAsyncHeader(1, irqs, 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	const unsigned hostIrqs = armAsyncWithoutAdvancing(0);
	awaitAsyncHeader(0, hostIrqs, 0x99660028);
	EQ(asynchronousReceive(0, CLOCK_FAST), 0x80000000);
	expectShortPacket(1, 4, hostB[1]);
	expectShortPacket(0, 4, clientB[1]);
	end();
}

static void testCompletedNoticeUnreadyOfferDeadline(void) {
	beginPrearmed("Unready B's finite local radio deadline cannot replace completed A clock-handoff success");
	synchronizePair();
	const uint32_t hostFinite = 0x0120;
	const uint32_t clientFinite = 0x0001;
	const uint32_t clientA[] = {4 << 8, 0x12345678};
	const uint32_t hostA[] = {4, 0x11223344};
	const uint32_t hostB[] = {4, 0x55667788};
	EQ(command(0, 0x17, &hostFinite, 1, NULL), 0);
	EQ(command(1, 0x17, &clientFinite, 1, NULL), 0);
	EQ(command(1, 0x25, clientA, 2, NULL), 0);
	EQ(commandOne(0, 0x24, hostA, 2, NULL), 0);
	advanceOne(0, 8192);
	EQ(machines[0]->rfu.txAckMask, 1);
	EQ(commandOne(0, 0x25, hostB, 2, NULL), 0);
	const unsigned clientIrqs = machines[1]->irqs;
	CHECK(machines[1]->gba.sio.siocnt & CLOCK_INTERNAL);
	advanceOne(1, 4096);
	CHECK(machines[1]->rfu.radioNotice && machines[1]->rfu.radioPending);
	CHECK(!machines[1]->rfu.radioMatched);
	const int32_t remaining = (int32_t) (machines[1]->rfu.radioDeadline - effectiveRadioClock(1));
	CHECK(remaining > 0 && (unsigned) remaining < FRAME_CYCLES + 4096);
	advanceOne(1, remaining + 512);
	CHECK(!machines[1]->rfu.radioPending);
	CHECK(machines[1]->rfu.radioNotice && !machines[1]->rfu.radioTimedOut);
	EQ(machines[1]->irqs, clientIrqs);
	EQ(machines[0]->rfu.txAckMask, 0);
	const unsigned beforeA = armAsyncWithoutAdvancing(1);
	EQ(beforeA, clientIrqs);
	finishPendingRadioHeader(1, clientIrqs, 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	EQ(machines[1]->rfu.state, GBA_RFU_COMMAND);
	EQ(machines[1]->rfu.rx.count, 1);
	CHECK(memcmp(machines[1]->rfu.rx.packets[0].data, &hostA[1], 4) == 0);
	EQ(machines[0]->rfu.txAckMask, 0);
	expectShortPacket(1, 4, hostA[1]);
	end();
}

static void testFreshClaimGuardsOldUnreadStandaloneWait(void) {
	beginPrearmed("Fresh prearmed CLAIM must finish its real exchange before old unread RX can wake client27");
	synchronizePair();
	const uint32_t oldClient[] = {4 << 8, 0x12345678};
	const uint32_t oldHost[] = {4, 0x11223344};
	const uint32_t newHost[] = {4, 0x55667788};
	EQ(command(1, 0x24, oldClient, 2, NULL), 0);
	EQ(commandOne(0, 0x24, oldHost, 2, NULL), 0);
	uint32_t response[32];
	EQ(commandOne(1, 0x12, NULL, 0, response), 1);
	advance(8192);
	EQ(machines[1]->rfu.rx.count, 1);
	EQ(commandOne(1, 0x27, NULL, 0, NULL), 0);
	CHECK(prearmedSnapshot(1).published);
	advanceOne(0, FRAME_CYCLES * 2);
	EQ(commandOne(0, 0x24, newHost, 2, NULL), 0);
	CHECK(prearmedSnapshot(1).claimed);
	const unsigned irqs = armAsyncWithoutAdvancing(1);
	advanceOne(1, 1024);
	EQ(machines[1]->irqs, irqs);
	CHECK(machines[1]->rfu.radioPending && machines[1]->rfu.radioMatched);
	CHECK(machines[1]->rfu.radioDue - effectiveRadioClock(1) > FRAME_CYCLES);
	EQ(machines[1]->rfu.rx.count, 1);
	CHECK(memcmp(machines[1]->rfu.rx.packets[0].data, &oldHost[1], 4) == 0);
	for (unsigned polls = 0; !machines[1]->rfu.adapterPending && polls < 10000; ++polls) advanceOne(1, 128);
	finishPendingRadioHeader(1, irqs, 0x99660028);
	EQ(asynchronousReceive(1, CLOCK_FAST), 0x80000000);
	expectShortPacket(1, 4, newHost[1]);
	end();
}

#include "rfu-cooperative-tests.h"
#include "rfu-multiplayer-tests.h"

int main(void) {
	setvbuf(stdout, NULL, _IONBF, 0);
	testHandshakeAndTiming();
	testExternalClockOwnership();
	testPackets();
	testAsyncReadiness();
	testWaitTimeoutAndDisconnect();
	testSendAndWait();
	testHostRetryNotification();
	testDisconnectedFallbacks();
	testGhostSendBuffers();
	testGhostSendAndWait();
	testGhostSendValidation();
	testExactAdapterTiming();
	testAdapterCancellation();
	testGenericCompletionModeChange();
	testAutomaticMacAck();
	testPartialMacAck();
	testRetransmitAcceptsDelayedAck();
	testRetransmitPreservesPartialAck();
	testRetransmitMasksRemovedPeer();
	testStaleMacAck();
	testZeroWaitLimits();
	testInactiveClientRecovery();
	testAckWithFullFifoAndZeroData();
	testDocumentedHostDrivenExchange();
	testStagedRetransmitAndZero();
	testUnreadRxCannotFinishNewClientSendWait();
	testHostFrameBeforeClientCommandCompletion();
	testClientExchangeGuardCleared();
	testPendingBuffersCleared();
	testSynchronizedRendezvous();
	testSynchronizedEmptyRounds();
	testSynchronizedPausedPeerProgress();
	testSynchronizedFreshWaitAndRetransmit();
	testSynchronizedQueuedOffer();
	testSynchronizedRetryBudget();
	testSynchronizedSessionReplay();
	testSynchronizedLifecycle();
	testSynchronizedSerialCancellation();
	testSynchronizationOptionAndMissingPeer();
	testSynchronizedFiniteWaitCancellation();
	testSynchronizedPlainSendReadiness();
	testSynchronizedSameSessionReplay();
	testSynchronizedFullMailboxRecovery();
	testSynchronizedMatchedClientTimeout();
	testSynchronizedRemoteCancellationReadiness();
	testSynchronizedCancelledQueuedOffer();
	testSynchronizedElapsedMailboxAlignment();
	testSynchronizedMatchedClockOrigin();
	testSynchronizedPausedAlignmentProgress();
	testBufferedLogExplicitFlush();
	testBufferedLogOverflow();
	testBufferedLogPathChanges();
	testBufferedLogFailedFlush();
	testAlreadyReadyHostOnlyBaseline();
	testPrearmedHostOnlyReceipt();
	testPrearmedNoImplicitReadiness();
	testPrearmedRequiresSynchronization();
	testPrearmedLatestAndZeroSnapshot();
	testPrearmedNewStagingDuringPriorAlignment();
	testPrearmedPublicationLifecycle();
	testPrearmedFullMailboxRecovery();
	testPrearmedClaimSessionReplay();
	testPrearmedClaimedLocalTimeout();
	testPrearmedFullReleaseCancellation();
	testPrearmedRetirementBeforeHostAcceptance();
	testPrearmedCommittedReceiptSurvivesHostRetirement();
	testPrearmedRetiredClaimWithFullHostMailbox();
	testPrearmedStandaloneWaitWithUnreadRX();
	testRemoteCancellationReadiness(true);
	testPacketFingerprintRoundTrip();
	testPacketFingerprintNewerStagedTX();
	testPacketFingerprintCancelledTicket();
	testCompletedNoticeWithQueuedOffer();
	testCompletedNoticeCancellationTiming();
	testCompletedNoticeCannotFinishNewTXWait();
	testCompletedNoticeUnreadyOfferDeadline();
	testFreshClaimGuardsOldUnreadStandaloneWait();
	testCooperativeScheduling();
	testFourWayDiscovery();
	testFourWayIndependentPairs();
	testMultiClientExchange();
	testMultiClientMembership();
	printf("All %u RFU regression cases passed (%u assertions).\n", testCases, assertions);
	return 0;
}
