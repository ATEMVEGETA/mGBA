/* Real RFU/SIO/timing coverage of a shared radio environment larger than a
 * cable group. No ROMs, peer CPU state, UI or fabricated adapter replies. */
static void prepareWirelessEnvironment(const char* name, unsigned count) {
	beginConfigured(name, count, true);
	const uint32_t settings = 0x0432;
	for (unsigned id = 0; id < machineCount; ++id) {
		GBASIORFUDriverSetSynchronize(&machines[id]->rfu, true);
		handshake(id);
		EQ(command(id, 0x17, &settings, 1, NULL), 0);
	}
	EQ(coordinator.nAttached, count);
}

static void testLargeRoomDiscovery(void) {
	const unsigned sizes[] = {5, 8, GBA_RFU_MAX_ADAPTERS};
	for (unsigned variant = 0; variant < COUNT_OF(sizes); ++variant) {
		const unsigned count = sizes[variant];
		prepareWirelessEnvironment("Five, eight and forty independent adapters discover every peer through bounded scan pages", count);
		uint32_t devices[GBA_RFU_MAX_ADAPTERS];
		for (unsigned id = 0; id < count; ++id) {
			devices[id] = startWirelessHost(id);
			for (unsigned old = 0; old < id; ++old) CHECK(devices[old] != devices[id]);
		}
		advance(FRAME_CYCLES * 2);
		for (unsigned id = 0; id < count; ++id) {
			/* Cover every observer, including IDs above the old bitmask width. */
			uint64_t seen = 0;
			const uint64_t expected = ((UINT64_C(1) << count) - 1) & ~(UINT64_C(1) << id);
			for (unsigned sweep = 0; sweep < count && seen != expected; ++sweep) {
				uint32_t response[32], repeated[32], ended[32];
				EQ(command(id, 0x1C, NULL, 0, NULL), 0);
				const unsigned words = command(id, 0x1D, NULL, 0, response);
				CHECK(words <= 28 && words % 7 == 0);
				EQ(command(id, 0x1D, NULL, 0, repeated), words);
				CHECK(memcmp(response, repeated, words * sizeof(uint32_t)) == 0);
				EQ(command(id, 0x1E, NULL, 0, ended), words);
				CHECK(memcmp(response, ended, words * sizeof(uint32_t)) == 0);
				uint64_t page = 0;
				for (unsigned index = 0; index < words; index += 7) {
					const unsigned peer = response[index + 1] & 0xFFFF;
					CHECK(peer < count && peer != id);
					CHECK(!(page & (UINT64_C(1) << peer)));
					EQ(response[index] & 0xFFFF, devices[peer]);
					EQ(response[index] >> 16, 0);
					page |= UINT64_C(1) << peer;
				}
				seen |= page;
			}
			CHECK(seen == expected);
		}
		end();
	}
}

static void testFiveMemberGroup(void) {
	for (unsigned zero = 0; zero < 2; ++zero) {
		prepareWirelessEnvironment(zero ? "Five-member hardware group completes empty RF exchanges with a sixth nearby adapter" :
			"One host exchanges distinct payloads with four clients while a sixth adapter remains independent", 6);
		const uint32_t device = startWirelessHost(0);
		for (unsigned client = 1; client <= 4; ++client) joinWirelessHost(0, client);
		uint32_t response[32];
		EQ(command(0, 0x1A, NULL, 0, response), 4);
		for (unsigned round = 0; round < 3; ++round) {
			for (unsigned client = 1; client <= 4; ++client) {
				const uint32_t data[] = {zero ? 0 : 4U << (8 + (client - 1) * 5), 0xB0000000 | (client << 16) | round};
				EQ(commandOne(client, 0x25, data, zero ? 1 : 2, NULL), 0);
			}
			const uint32_t data[] = {zero ? 0 : 4, 0xA0000000 | round};
			EQ(commandOne(0, 0x25, data, zero ? 1 : 2, NULL), 0);
			const unsigned ids[] = {0,1,2,3,4};
			finishWirelessWaits(ids, 5);
			EQ(machines[0]->rfu.txTargetMask, 15);
			EQ(machines[0]->rfu.txAckMask, 15);
			for (unsigned client = 1; client <= 4; ++client) expectWirelessPacket(client, zero ? 0 : 4, data[1]);
			EQ(command(0, 0x26, NULL, 0, response), zero ? 1 : 5);
			EQ(response[0], zero ? 0 : (4U << 8) | (4U << 13) | (4U << 18) | (4U << 23));
			if (!zero) for (unsigned client = 1; client <= 4; ++client) EQ(response[client], 0xB0000000 | (client << 16) | round);
		}
		/* The sixth console is nearby, not an illegal fifth child. */
		EQ(command(5, 0x1C, NULL, 0, NULL), 0);
		EQ(command(5, 0x1D, NULL, 0, response), 7);
		EQ(response[0] & 0xFFFF, device);
		EQ(response[0] >> 16, 0xFF);
		EQ(command(5, 0x1E, NULL, 0, NULL), 7);
		EQ(command(5, 0x1F, &device, 1, NULL), 0);
		advance(FRAME_CYCLES * 2);
		EQ(command(5, 0x20, NULL, 0, response), 1);
		CHECK(response[0] & 0xFF000000);
		EQ(command(0, 0x1A, NULL, 0, response), 4);
		EQ(coordinator.nAttached, 6);
		end();
	}
}

static void testLargeRoomIndependentPairs(void) {
	prepareWirelessEnvironment("Forty nearby adapters perform twenty isolated synchronized pair exchanges", GBA_RFU_MAX_ADAPTERS);
	for (unsigned host = 0; host < machineCount; host += 2) startWirelessHost(host);
	for (unsigned host = 0; host < machineCount; host += 2) joinWirelessHost(host, host + 1);
	for (unsigned round = 0; round < 3; ++round) {
		for (unsigned host = 0; host < machineCount; host += 2) {
			const unsigned client = host + 1;
			const uint32_t reply[] = {4U << 8, 0xB0000000 | (client << 16) | round};
			const uint32_t data[] = {4, 0xA0000000 | (host << 16) | round};
			EQ(commandOne(client, 0x25, reply, 2, NULL), 0);
			EQ(commandOne(host, 0x25, data, 2, NULL), 0);
		}
		unsigned ids[GBA_RFU_MAX_ADAPTERS];
		for (unsigned id = 0; id < machineCount; ++id) ids[id] = id;
		finishWirelessWaits(ids, machineCount);
		for (unsigned host = 0; host < machineCount; host += 2) {
			const unsigned client = host + 1;
			expectWirelessPacket(client, 4, 0xA0000000 | (host << 16) | round);
			expectWirelessPacket(host, 4, 0xB0000000 | (client << 16) | round);
			EQ(machines[host]->rfu.txAckMask, 1);
			CHECK(!machines[host]->rfu.radioPending && !machines[client]->rfu.radioPending);
		}
	}
	end();
}
