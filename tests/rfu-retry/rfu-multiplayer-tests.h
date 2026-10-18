/* Four-adapter coverage through the real serial command/timing fixture.
 * Union Room discovery and two simultaneous independent peer interactions
 * must work on the same coordinator without global pair assumptions. */
static void prepareFourWireless(const char* name) {
	beginConfigured(name, 4, true);
	const uint32_t settings = 0x0432;
	for (unsigned id = 0; id < machineCount; ++id) {
		GBASIORFUDriverSetSynchronize(&machines[id]->rfu, true);
		handshake(id);
		EQ(command(id, 0x17, &settings, 1, NULL), 0);
	}
}

static uint32_t startWirelessHost(unsigned id) {
	const uint32_t broadcast[] = {0x12340000 | id, 0x50607080, 0x90A0B0C0, 0xD0E0F000, 0x01020304, 0x05060708};
	EQ(command(id, 0x16, broadcast, 6, NULL), 0);
	EQ(command(id, 0x19, NULL, 0, NULL), 0);
	uint32_t response[32];
	EQ(command(id, 0x13, NULL, 0, response), 1);
	EQ(response[0] >> 24, 1);
	return response[0] & 0xFFFF;
}

static void joinWirelessHost(unsigned host, unsigned client) {
	advance(FRAME_CYCLES * 2);
	uint32_t response[32];
	EQ(command(host, 0x13, NULL, 0, response), 1);
	const uint32_t device = response[0] & 0xFFFF;
	bool found = false;
	for (unsigned sweep = 0; sweep < GBA_RFU_MAX_ADAPTERS && !found; ++sweep) {
		EQ(command(client, 0x1C, NULL, 0, NULL), 0);
		unsigned count = command(client, 0x1D, NULL, 0, response);
		CHECK(count <= 28 && count % 7 == 0);
		for (unsigned i = 0; i < count; i += 7) if ((response[i] & 0xFFFF) == device) found = true;
		command(client, 0x1E, NULL, 0, NULL);
	}
	CHECK(found);
	EQ(command(client, 0x1F, &device, 1, NULL), 0);
	advance(FRAME_CYCLES * 2);
	EQ(command(client, 0x20, NULL, 0, response), 1);
	CHECK((response[0] & 0xFFFF) != 0);
	CHECK(!(response[0] & 0xFF000000));
	EQ(command(client, 0x21, NULL, 0, response), 1);
	EQ(machines[client]->rfu.hostPeer, host);
}

static void testFourWayDiscovery(void) {
	prepareFourWireless("Four Union Room advertisers each discover the other three adapters");
	uint32_t devices[4];
	for (unsigned id = 0; id < 4; ++id) devices[id] = startWirelessHost(id);
	advance(FRAME_CYCLES * 3);
	for (unsigned id = 0; id < 4; ++id) {
		uint32_t response[32];
		EQ(command(id, 0x1D, NULL, 0, response), 21);
		unsigned seen = 0;
		for (unsigned index = 0; index < 21; index += 7) {
			const unsigned peer = response[index + 1] & 0xFFFF;
			CHECK(peer < 4 && peer != id);
			CHECK(!(seen & (1U << peer)));
			EQ(response[index], devices[peer]);
			seen |= 1U << peer;
		}
		EQ(seen, 15U & ~(1U << id));
	}
	end();
}

static void finishWirelessWaits(const unsigned* ids, unsigned count) {
	unsigned previous[GBA_RFU_MAX_ADAPTERS];
	CHECK(count <= GBA_RFU_MAX_ADAPTERS);
	for (unsigned i = 0; i < count; ++i) previous[i] = armAsyncWithoutAdvancing(ids[i]);
	for (unsigned i = 0; i < count; ++i) awaitAsyncHeader(ids[i], previous[i], 0x99660028);
	for (unsigned i = 0; i < count; ++i) EQ(asynchronousReceive(ids[i], CLOCK_FAST), 0x80000000);
}

static void expectWirelessPacket(unsigned id, unsigned bytes, uint32_t data) {
	uint32_t response[32];
	EQ(command(id, 0x26, NULL, 0, response), bytes ? 2 : 1);
	EQ(response[0], machines[id]->rfu.role == GBA_RFU_HOST ? bytes << 8 : bytes);
	if (bytes) EQ(response[1], data);
}

static void independentPairRound(const unsigned pairs[4], unsigned round, bool both) {
	const unsigned pairCount = both ? 2 : 1;
	unsigned ids[4];
	for (unsigned pair = 0; pair < pairCount; ++pair) {
		const unsigned host = pairs[pair * 2];
		const unsigned client = pairs[pair * 2 + 1];
		const uint32_t hostData[] = {4, 0xA0000000 | (host << 16) | round};
		const unsigned slot = machines[client]->rfu.clientNumber;
		const uint32_t clientData[] = {4U << (8 + slot * 5), 0xB0000000 | (client << 16) | round};
		EQ(commandOne(client, 0x25, clientData, 2, NULL), 0);
		EQ(commandOne(host, 0x25, hostData, 2, NULL), 0);
		ids[pair * 2] = host;
		ids[pair * 2 + 1] = client;
	}
	finishWirelessWaits(ids, pairCount * 2);
	for (unsigned pair = 0; pair < pairCount; ++pair) {
		const unsigned host = pairs[pair * 2];
		const unsigned client = pairs[pair * 2 + 1];
		expectWirelessPacket(client, 4, 0xA0000000 | (host << 16) | round);
		const unsigned slot = machines[client]->rfu.clientNumber;
		uint32_t response[32];
		EQ(command(host, 0x26, NULL, 0, response), 2);
		EQ(response[0], 4U << (8 + slot * 5));
		EQ(response[1], 0xB0000000 | (client << 16) | round);
		EQ(machines[host]->rfu.txAckMask, 1U << slot);
		CHECK(!machines[host]->rfu.radioPending);
		CHECK(!machines[client]->rfu.radioPending);
	}
}

static void testFourWayIndependentPairs(void) {
	const unsigned pairings[][4] = {{0,1,2,3}, {0,2,1,3}, {3,1,2,0}};
	for (unsigned variant = 0; variant < COUNT_OF(pairings); ++variant) {
		prepareFourWireless("Four adapters support two independent synchronized interactions and reconnect");
		const unsigned* pairs = pairings[variant];
		startWirelessHost(pairs[0]);
		startWirelessHost(pairs[2]);
		joinWirelessHost(pairs[0], pairs[1]);
		joinWirelessHost(pairs[2], pairs[3]);
		for (unsigned round = 0; round < 8; ++round) independentPairRound(pairs, round, true);
		EQ(command(pairs[3], 0x30, NULL, 0, NULL), 0);
		advance(FRAME_CYCLES);
		CHECK(!machines[pairs[2]]->rfu.clients[0].active);
		independentPairRound(pairs, 10, false);
		joinWirelessHost(pairs[2], pairs[3]);
		independentPairRound(pairs, 11, true);
		end();
	}
}

static void testMultiClientExchange(void) {
	for (unsigned zero = 0; zero < 2; ++zero) {
		prepareFourWireless(zero ? "One host exchanges zero-byte notifications with all three clients" :
			"One host exchanges distinct payloads and receipt masks with all three clients");
		startWirelessHost(0);
		for (unsigned client = 1; client < 4; ++client) joinWirelessHost(0, client);
		uint32_t response[32];
		EQ(command(0, 0x1A, NULL, 0, response), 3);
		for (unsigned round = 0; round < 4; ++round) {
			for (unsigned client = 1; client < 4; ++client) {
				const uint32_t data[] = {zero ? 0 : 4U << (8 + (client - 1) * 5), 0xB0000000 | (client << 16) | round};
				EQ(commandOne(client, 0x25, data, zero ? 1 : 2, NULL), 0);
			}
			const uint32_t data[] = {zero ? 0 : 4, 0xA0000000 | round};
			EQ(commandOne(0, 0x25, data, zero ? 1 : 2, NULL), 0);
			const unsigned ids[] = {0,1,2,3};
			finishWirelessWaits(ids, 4);
			EQ(machines[0]->rfu.txTargetMask, 7);
			EQ(machines[0]->rfu.txAckMask, 7);
			for (unsigned client = 1; client < 4; ++client) expectShortPacket(client, zero ? 0 : 4, zero ? 0 : data[1]);
			EQ(command(0, 0x26, NULL, 0, response), zero ? 1 : 4);
			EQ(response[0], zero ? 0 : (4U << 8) | (4U << 13) | (4U << 18));
			if (!zero) for (unsigned client = 1; client < 4; ++client) EQ(response[client], 0xB0000000 | (client << 16) | round);
		}
		end();
	}
}

static void testMultiClientMembership(void) {
	for (unsigned survivor = 1; survivor < 4; ++survivor) {
		prepareFourWireless("A wireless host switches between pair and group exchanges as peers join and leave");
		startWirelessHost(0);
		joinWirelessHost(0, 1);
		unsigned pair[] = {0,1,2,3};
		independentPairRound(pair, 20, false);
		for (unsigned cycle = 0; cycle < 2; ++cycle) {
			for (unsigned client = 1; client < 4; ++client) if (client != pair[1]) joinWirelessHost(0, client);
			for (unsigned client = 1; client < 4; ++client) {
				const uint32_t data[] = {4U << (8 + machines[client]->rfu.clientNumber * 5), 0xC0000000 | (client << 16) | cycle};
				EQ(commandOne(client, 0x25, data, 2, NULL), 0);
			}
			const uint32_t data[] = {4, 0xD0000000 | cycle};
			EQ(commandOne(0, 0x25, data, 2, NULL), 0);
			const unsigned ids[] = {0,1,2,3};
			finishWirelessWaits(ids, 4);
			for (unsigned client = 1; client < 4; ++client) expectWirelessPacket(client, 4, data[1]);
			uint32_t response[32];
			EQ(command(0, 0x26, NULL, 0, response), 4);
			EQ(response[0], (4U << 8) | (4U << 13) | (4U << 18));
			for (unsigned client = 1; client < 4; ++client) {
				EQ(response[machines[client]->rfu.clientNumber + 1], 0xC0000000 | (client << 16) | cycle);
				if (client != survivor) EQ(command(client, 0x30, NULL, 0, NULL), 0);
			}
			advance(FRAME_CYCLES);
			for (unsigned slot = 0; slot < 3; ++slot) {
				EQ(machines[0]->rfu.clients[slot].active, slot == machines[survivor]->rfu.clientNumber);
			}
			pair[1] = survivor;
			independentPairRound(pair, 21 + cycle, false);
		}
		end();
	}
}
