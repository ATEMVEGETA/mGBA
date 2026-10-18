/* Copyright (c) 2013-2015 Jeffrey Pfau
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */
#pragma once

#include <QHash>
#include <QElapsedTimer>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QWaitCondition>

#include <mgba/core/core.h>
#include <mgba/core/lockstep.h>
#ifdef M_CORE_GBA
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba/internal/gba/sio/rfu.h>
#endif
#ifdef M_CORE_GB
#include <mgba/internal/gb/sio/lockstep.h>
#endif

#include <atomic>
#include <memory>

struct GBSIOLockstepNode;
struct GBASIOLockstepNode;

namespace QGBA {

class CoreController;

class MultiplayerController : public QObject {
Q_OBJECT

public:
	MultiplayerController();
	~MultiplayerController();

	bool attachGame(CoreController*);
	void detachGame(CoreController*);
	// Read-only UI preflight. attachGame remains the final capacity check.
	bool canAttachGame(mPlatform platform) const;

	int attached();
	int playerId(CoreController*) const;
	int saveId(CoreController*) const;
	// An active session retains its transport and capacity until all games close.
	int windowLimit() const;
	// Changes apply to the next session, after all current games close.
	void setWirelessEnabled(bool enabled);
	bool wirelessEnabled() const { return m_wirelessRequested.load(std::memory_order_relaxed); }

signals:
	void gameAttached();
	void gameDetached();
	void windowLimitChanged();

private:
	union Node {
		GBSIOLockstepNode* gb;
		GBASIOLockstepDriver* gba;
#ifdef M_CORE_GBA
		GBASIORFUDriver* rfu;
#endif
	};
	struct Player {
		Player(CoreController* controller);

		int id() const;
		bool operator<(const Player&) const;

		CoreController* controller;
		Node node = {nullptr};
		int awake = 1;
		int32_t cyclesPosted = 0;
		unsigned waitMask = 0;
		int saveId = 1;
		int preferredId = 0;
		QPair<QString, QString> savePath;
		bool attached = false;
#ifdef M_CORE_GBA
		bool wireless = false;
		mLockstepThreadUser* wirelessUser = nullptr;
#endif
	};
	struct LockstepUser : mLockstepThreadUser {
		MultiplayerController* controller;
		int pid;
	};

	Player* player(int id);
	const Player* player(int id) const;
	void fixOrder();

	union {
		mLockstep m_lockstep;
#ifdef M_CORE_GB
		GBSIOLockstep m_gbLockstep;
#endif
	};

#ifdef M_CORE_GBA
	class WirelessServiceThread;
	GBASIOLockstepCoordinator m_gbaCoordinator;
	GBASIORFUCoordinator m_gbaRfuCoordinator;
	bool m_gbaWireless = false;
	// The watchdog never reads CPU/adapter protocol state or player maps.
	// This guard serializes coordinator lifetime with service, and must not
	// be held while interrupting or joining any core or the service thread.
	QMutex m_rfuServiceMutex;
	QWaitCondition m_rfuServiceWake;
	QElapsedTimer m_rfuServiceClock;
	bool m_rfuServiceActive = false;
	bool m_rfuServiceQuit = false;
	std::unique_ptr<WirelessServiceThread> m_rfuServiceThread;
#endif
	std::atomic<bool> m_wirelessRequested{false};
	// Menu/window checks run on the UI thread while cores attach/detach.
	// Zero means there is no active transport, so use the requested next mode.
	std::atomic<int> m_sessionWindowLimit{0};
	std::atomic<int> m_registeredPlayers{0};

	mPlatform m_platform = mPLATFORM_NONE;
	int m_nextPid = 0;
	QSet<int> m_claimedIds;
	QHash<int, Player> m_pids;
	QList<int> m_players;
	QMutex m_lock;
	QHash<QPair<QString, QString>, QSet<int>> m_claimedSaves;
};

}
