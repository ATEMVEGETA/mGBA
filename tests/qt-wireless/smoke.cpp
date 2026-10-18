/* No-ROM integration check for the actual Qt config/menu/controller wiring.
 * This links the real frontend objects. Only the inert core-thread fixture is
 * synthetic: no CPU runLoop, ROM, BIOS, audio device or save file is used. */
#define SDL_MAIN_HANDLED
#include "ConfigController.h"
#include "GBAApp.h"
#include "Window.h"
#include "CoreController.h"
#include "InputController.h"

#include <mgba/gba/core.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/core/thread.h>
#ifdef M_CORE_GB
#include <mgba/gb/core.h>
#include <mgba/internal/gb/gb.h>
#endif

#include <QDir>
#include <QFile>
#include <QMenuBar>
#include <QSet>
#include <QTemporaryDir>

#ifdef QT_STATIC
#include <QtPlugin>
// Keep the application's static imports when replacing its main object.
// Offscreen is an additional fixture-only import for headless execution.
#ifdef Q_OS_WIN
Q_IMPORT_PLUGIN(QJpegPlugin);
Q_IMPORT_PLUGIN(QWindowsIntegrationPlugin);
Q_IMPORT_PLUGIN(QWindowsVistaStylePlugin);
#ifdef BUILD_QT_MULTIMEDIA
Q_IMPORT_PLUGIN(QWindowsAudioPlugin);
Q_IMPORT_PLUGIN(DSServicePlugin);
#endif
#elif defined(Q_OS_MAC)
Q_IMPORT_PLUGIN(QCocoaIntegrationPlugin);
#ifdef BUILD_QT_MULTIMEDIA
Q_IMPORT_PLUGIN(CoreAudioPlugin);
Q_IMPORT_PLUGIN(AVFServicePlugin);
#endif
#elif defined(Q_OS_UNIX)
Q_IMPORT_PLUGIN(QXcbIntegrationPlugin);
Q_IMPORT_PLUGIN(QWaylandIntegrationPlugin);
#endif
Q_IMPORT_PLUGIN(QOffscreenIntegrationPlugin);
#endif

#ifdef BUILD_SDL
#include <SDL.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <array>
#include <vector>

using namespace QGBA;

static unsigned checks;
#define CHECK(expr) do { ++checks; if (!(expr)) { \
	std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); \
} } while (0)

/* Test-only pointer-to-member access observes the real GBAApp binding without
 * introducing a production accessor or replacing ConfigOption callbacks. */
struct MultiplayerMember {
	using type = MultiplayerController GBAApp::*;
	friend type testMember(MultiplayerMember);
};
template<class Tag, typename Tag::type Member> struct MemberAccess {
	friend typename Tag::type testMember(Tag) { return Member; }
};
template struct MemberAccess<MultiplayerMember, &GBAApp::m_multiplayer>;

struct ClaimedIdsMember {
	using type = QSet<int> MultiplayerController::*;
	friend type testMember(ClaimedIdsMember);
};
struct NextPidMember {
	using type = int MultiplayerController::*;
	friend type testMember(NextPidMember);
};
struct ClaimedSavesMember {
	using type = QHash<QPair<QString, QString>, QSet<int>> MultiplayerController::*;
	friend type testMember(ClaimedSavesMember);
};
template struct MemberAccess<ClaimedIdsMember, &MultiplayerController::m_claimedIds>;
template struct MemberAccess<NextPidMember, &MultiplayerController::m_nextPid>;
template struct MemberAccess<ClaimedSavesMember, &MultiplayerController::m_claimedSaves>;

static QAction* linkAction(Window* window, const QString& text) {
	for (QAction* top : window->menuBar()->actions()) {
		if (!top->menu()) continue;
		for (QAction* submenu : top->menu()->actions()) {
			if (!submenu->menu() || submenu->text() != QStringLiteral("GBA link type (next session)")) continue;
			for (QAction* action : submenu->menu()->actions()) if (action->text() == text) return action;
		}
	}
	return nullptr;
}

static QAction* newWindowAction(Window* window) {
	for (QAction* top : window->menuBar()->actions()) {
		if (!top->menu()) continue;
		for (QAction* action : top->menu()->actions()) {
			if (action->text() == QStringLiteral("New multiplayer window")) return action;
		}
	}
	return nullptr;
}

static THREAD_ENTRY inertThread(void*) { THREAD_EXIT(0); }

class InertCore {
public:
	explicit InertCore(ConfigController& config, mPlatform platform = mPLATFORM_GBA) {
		mCore* core = nullptr;
#ifdef M_CORE_GB
		if (platform == mPLATFORM_GB) core = GBCoreCreate();
#endif
		if (platform == mPLATFORM_GBA) core = GBACoreCreate();
		CHECK(core && core->init(core));
		mCoreInitConfig(core, nullptr);
		mCoreLoadForeignConfig(core, config.config());
		core->reset(core); // Reset register/timing state only; executes no instructions.
		controller = std::make_unique<CoreController>(core);
		controller->setPath(QStringLiteral("wireless-fixture"), QString());
		mCoreThread* thread = controller->thread();
		thread->impl = static_cast<mCoreThreadInternal*>(std::calloc(1, sizeof(mCoreThreadInternal)));
		CHECK(thread->impl);
		thread->impl->state = mTHREAD_SHUTDOWN;
		thread->impl->core = core;
		MutexInit(&thread->impl->stateMutex);
		ConditionInit(&thread->impl->stateOnThreadCond);
		ConditionInit(&thread->impl->stateOffThreadCond);
		MutexInit(&thread->impl->sync.videoFrameMutex);
		ConditionInit(&thread->impl->sync.videoFrameAvailableCond);
		ConditionInit(&thread->impl->sync.videoFrameRequiredCond);
		MutexInit(&thread->impl->sync.audioBufferMutex);
		ConditionInit(&thread->impl->sync.audioRequiredCond);
		ThreadCreate(&thread->impl->thread, inertThread, nullptr);
		m_handle = thread->impl->thread;
		CHECK(m_handle);
	}
	~InertCore() {
		controller.reset(); // Production stop/join/deinit runs against the inert thread.
#ifdef _WIN32
		CloseHandle(m_handle);
#endif
	}
	GBASIODriver* driver() const { return static_cast<GBA*>(controller->thread()->core->board)->sio.driver; }
#ifdef M_CORE_GB
	GBSIODriver* gbDriver() const { return static_cast<GB*>(controller->thread()->core->board)->sio.driver; }
#endif
	std::unique_ptr<CoreController> controller;
private:
	Thread m_handle;
};

static void checkWirelessRoom(MultiplayerController& multiplayer, ConfigController& config, unsigned count) {
	// Every fixture shares one ROM path. Save IDs therefore exercise the real
	// collision allocator, including suffixes above the old 32-bit-mask limit.
	std::vector<std::unique_ptr<InertCore>> cores(count);
	std::vector<GBASIORFUDriver*> adapters(count);
	std::vector<uint32_t> sessions(count);
	std::vector<int> saveIds(count);
	const int nextPidBefore = multiplayer.*testMember(NextPidMember{});
	CHECK(multiplayer.wirelessEnabled());
	CHECK(multiplayer.windowLimit() == GBA_RFU_MAX_ADAPTERS);
	CHECK(multiplayer.attached() == 0);
	CHECK(multiplayer.canAttachGame(mPLATFORM_GBA));
	CHECK((multiplayer.*testMember(ClaimedIdsMember{})).isEmpty());
	CHECK((multiplayer.*testMember(ClaimedSavesMember{})).isEmpty());
	for (unsigned i = 0; i < count; ++i) {
		cores[i] = std::make_unique<InertCore>(config);
		CHECK(multiplayer.attachGame(cores[i]->controller.get()));
		CHECK(multiplayer.attached() == static_cast<int>(i + 1));
		CHECK(!multiplayer.canAttachGame(mPLATFORM_GB));
		GBASIODriver* driver = cores[i]->driver();
		CHECK(driver && driver->driverId(driver) == 0x52465500);
		adapters[i] = reinterpret_cast<GBASIORFUDriver*>(driver);
		sessions[i] = adapters[i]->radioSession;
		saveIds[i] = multiplayer.saveId(cores[i]->controller.get());
		CHECK(adapters[i]->playerId == static_cast<int>(i));
		CHECK(adapters[i]->cooperativeUser);
		CHECK(reinterpret_cast<mLockstepThreadUser*>(adapters[i]->cooperativeUser)->thread == cores[i]->controller->thread());
		CHECK(sessions[i] != 0);
		CHECK(saveIds[i] == static_cast<int>(i + 1));
		CHECK(multiplayer.playerId(cores[i]->controller.get()) == static_cast<int>(i));
		for (unsigned j = 0; j < i; ++j) {
			CHECK(adapters[i] != adapters[j]);
			CHECK(adapters[i]->cooperativeUser != adapters[j]->cooperativeUser);
			CHECK(adapters[i]->coordinator == adapters[j]->coordinator);
			CHECK(sessions[i] != sessions[j]);
			CHECK(saveIds[i] != saveIds[j]);
		}
	}
	const auto claimedIdsBefore = multiplayer.*testMember(ClaimedIdsMember{});
	const auto claimedSavesBefore = multiplayer.*testMember(ClaimedSavesMember{});
	CHECK(claimedIdsBefore.size() == static_cast<int>(count));
	CHECK(claimedSavesBefore.size() == 1);
	CHECK(claimedSavesBefore.constBegin().value().size() == static_cast<int>(count));
	CHECK((multiplayer.*testMember(NextPidMember{})) == nextPidBefore + static_cast<int>(count));

	if (count == GBA_RFU_MAX_ADAPTERS) {
		CHECK(!multiplayer.canAttachGame(mPLATFORM_GBA));
		InertCore excess(config);
		GBASIODriver* excessDriver = excess.driver();
		const int nextPidAtCapacity = multiplayer.*testMember(NextPidMember{});
		CHECK(!multiplayer.attachGame(excess.controller.get()));
		CHECK(multiplayer.attached() == GBA_RFU_MAX_ADAPTERS);
		CHECK(excess.driver() == excessDriver);
		CHECK(multiplayer.playerId(excess.controller.get()) == -1);
		CHECK(multiplayer.saveId(excess.controller.get()) == -1);
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == claimedIdsBefore);
		CHECK((multiplayer.*testMember(NextPidMember{})) == nextPidAtCapacity);
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})) == claimedSavesBefore);
		for (unsigned i = 0; i < count; ++i) {
			CHECK(cores[i]->driver() == &adapters[i]->d);
			CHECK(adapters[i]->radioSession == sessions[i]);
			CHECK(multiplayer.playerId(cores[i]->controller.get()) == static_cast<int>(i));
			CHECK(multiplayer.saveId(cores[i]->controller.get()) == saveIds[i]);
		}
	}

	// Retire/rejoin a middle slot, and separately slot 33 in the full room.
	// Survivors keep their exact identities; only the returning session changes.
	std::vector<unsigned> middleSlots{count / 2};
	if (count > 33) middleSlots.push_back(33);
	for (unsigned middle : middleSlots) {
		const int middleId = adapters[middle]->playerId;
		multiplayer.detachGame(cores[middle]->controller.get());
		CHECK(multiplayer.attached() == static_cast<int>(count - 1));
		CHECK(multiplayer.canAttachGame(mPLATFORM_GBA));
		CHECK(!cores[middle]->driver());
		auto expectedIds = claimedIdsBefore;
		expectedIds.remove(middleId);
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == expectedIds);
		for (unsigned i = 0; i < count; ++i) {
			if (i == middle) continue;
			CHECK(cores[i]->driver() == &adapters[i]->d);
			CHECK(adapters[i]->radioSession == sessions[i]);
			CHECK(adapters[i]->playerId == static_cast<int>(i));
			CHECK(multiplayer.playerId(cores[i]->controller.get()) >= 0);
			CHECK(multiplayer.saveId(cores[i]->controller.get()) == saveIds[i]);
		}
		CHECK(multiplayer.attachGame(cores[middle]->controller.get()));
		CHECK(multiplayer.attached() == static_cast<int>(count));
		GBASIODriver* rejoined = cores[middle]->driver();
		CHECK(rejoined && rejoined->driverId(rejoined) == 0x52465500);
		adapters[middle] = reinterpret_cast<GBASIORFUDriver*>(rejoined);
		CHECK(adapters[middle]->playerId == middleId);
		CHECK(multiplayer.playerId(cores[middle]->controller.get()) >= 0);
		CHECK(adapters[middle]->radioSession != 0);
		CHECK(adapters[middle]->radioSession != sessions[middle]);
		CHECK(multiplayer.saveId(cores[middle]->controller.get()) == saveIds[middle]);
		sessions[middle] = adapters[middle]->radioSession;
		for (unsigned i = 0; i < count; ++i) {
			if (i == middle) continue;
			CHECK(adapters[middle]->radioSession != sessions[i]);
			CHECK(adapters[middle]->cooperativeUser != adapters[i]->cooperativeUser);
			CHECK(cores[i]->driver() == &adapters[i]->d);
		}
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == claimedIdsBefore);
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})) == claimedSavesBefore);
	}

	// Initial player first, then even IDs, then odd IDs: not stack order.
	std::vector<unsigned> closeOrder{0};
	for (unsigned i = 2; i < count; i += 2) closeOrder.push_back(i);
	for (unsigned i = 1; i < count; i += 2) closeOrder.push_back(i);
	for (unsigned i = 0; i < count; ++i) {
		const unsigned closing = closeOrder[i];
		multiplayer.detachGame(cores[closing]->controller.get());
		CHECK(!cores[closing]->driver());
		CHECK(multiplayer.attached() == static_cast<int>(count - i - 1));
		for (unsigned j = i + 1; j < count; ++j) {
			const unsigned survivor = closeOrder[j];
			CHECK(cores[survivor]->driver() == &adapters[survivor]->d);
			CHECK(adapters[survivor]->radioSession == sessions[survivor]);
			CHECK(adapters[survivor]->playerId == static_cast<int>(survivor));
			CHECK(multiplayer.playerId(cores[survivor]->controller.get()) >= 0);
			CHECK(multiplayer.saveId(cores[survivor]->controller.get()) == saveIds[survivor]);
		}
	}
	CHECK((multiplayer.*testMember(ClaimedIdsMember{})).isEmpty());
	CHECK((multiplayer.*testMember(ClaimedSavesMember{})).isEmpty());
	CHECK((multiplayer.*testMember(NextPidMember{})) == nextPidBefore + static_cast<int>(count + middleSlots.size()));
	std::printf("ROOM wireless adapters=%u PASS\n", count);
}

int main(int argc, char** argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	CHECK(argc == 2);
	const QString isolated = QString::fromLocal8Bit(argv[1]);
	CHECK(QDir(isolated).exists());
	// The runner places the test executable beside a fresh portable.ini. Assert
	// isolation before constructing anything that can read/write configuration.
	CHECK(ConfigController::isPortable());
	CHECK(QDir(ConfigController::configDir()).canonicalPath() == QDir(isolated).canonicalPath());
#ifdef BUILD_SDL
	SDL_SetMainReady();
#endif
	ConfigController config;
	CHECK(config.getOption("gbaLinkMode").toInt() == 0);
	config.setOption("updateAutoCheck", 0);
	config.setOption("useDiscordPresence", 0);
	config.setQtOption("displayDriver", 0);
	config.setQtOption("audioDriver", -1);
	GBAApp app(argc, argv, &config);
	MultiplayerController& multiplayer = app.*testMember(MultiplayerMember{});
	CHECK(!multiplayer.wirelessEnabled());
	CHECK(MAX_GBAS == 4);
	CHECK(GBA_RFU_MAX_ADAPTERS == 40);
	CHECK(multiplayer.windowLimit() == MAX_GBAS);
	CHECK(app.windowLimit() == MAX_GBAS);
	CHECK(multiplayer.canAttachGame(mPLATFORM_GBA));
#ifdef M_CORE_GB
	CHECK(multiplayer.canAttachGame(mPLATFORM_GB));
#endif
	CHECK(!multiplayer.canAttachGame(mPLATFORM_NONE));
	Window* first = app.newWindow();
	Window* second = app.newWindow();
	CHECK(first && second);
	QAction* cable[2] = {linkAction(first, QStringLiteral("Link Cable")), linkAction(second, QStringLiteral("Link Cable"))};
	QAction* wireless[2] = {linkAction(first, QStringLiteral("Wireless Adapter")), linkAction(second, QStringLiteral("Wireless Adapter"))};
	QAction* newMultiplayer[2] = {newWindowAction(first), newWindowAction(second)};
	for (unsigned i = 0; i < 2; ++i) {
		std::printf("MENU window=%u cable=%p checked=%d wireless=%p checked=%d\n", i,
			static_cast<void*>(cable[i]), cable[i] ? cable[i]->isChecked() : -1,
			static_cast<void*>(wireless[i]), wireless[i] ? wireless[i]->isChecked() : -1);
		CHECK(cable[i] && wireless[i] && cable[i]->isChecked() && !wireless[i]->isChecked());
		CHECK(newMultiplayer[i] && newMultiplayer[i]->isEnabled());
	}
	{
		InertCore one(config), two(config);
		CHECK(multiplayer.attachGame(one.controller.get()));
		CHECK(multiplayer.attachGame(two.controller.get()));
		CHECK(one.driver() && one.driver()->driverId(one.driver()) == 0x6B636F4C);
		wireless[0]->trigger();
		CHECK(multiplayer.wirelessEnabled());
		CHECK(multiplayer.windowLimit() == MAX_GBAS); // Active cable wins over requested wireless.
		CHECK(app.windowLimit() == MAX_GBAS);
		CHECK(config.getOption("gbaLinkMode").toInt() == 1);
		for (unsigned i = 0; i < 2; ++i) CHECK(wireless[i]->isChecked() && !cable[i]->isChecked());
		// A requested change does not replace either active cable driver.
		CHECK(one.driver()->driverId(one.driver()) == 0x6B636F4C);
		CHECK(two.driver()->driverId(two.driver()) == 0x6B636F4C);
		multiplayer.detachGame(two.controller.get());
		multiplayer.detachGame(one.controller.get());
		CHECK(multiplayer.attached() == 0);
		CHECK(multiplayer.windowLimit() == GBA_RFU_MAX_ADAPTERS);
		CHECK(multiplayer.attachGame(one.controller.get()));
		CHECK(one.driver() && one.driver()->driverId(one.driver()) == 0x52465500);
		cable[1]->trigger();
		CHECK(!multiplayer.wirelessEnabled());
		CHECK(multiplayer.windowLimit() == GBA_RFU_MAX_ADAPTERS); // Active RFU survives requested cable.
		CHECK(app.windowLimit() == GBA_RFU_MAX_ADAPTERS);
		for (unsigned i = 0; i < 2; ++i) CHECK(cable[i]->isChecked() && !wireless[i]->isChecked());
		CHECK(multiplayer.attachGame(two.controller.get()));
		CHECK(one.driver()->driverId(one.driver()) == 0x52465500);
		CHECK(two.driver()->driverId(two.driver()) == 0x52465500);
		multiplayer.detachGame(one.controller.get());
		CHECK(two.driver()->driverId(two.driver()) == 0x52465500); // Survivor keeps its adapter.
		multiplayer.detachGame(two.controller.get());
		CHECK(multiplayer.windowLimit() == MAX_GBAS);
		CHECK(multiplayer.canAttachGame(mPLATFORM_GBA));
		CHECK(multiplayer.attachGame(one.controller.get()));
		CHECK(multiplayer.attachGame(two.controller.get()));
		CHECK(one.driver()->driverId(one.driver()) == 0x6B636F4C);
		CHECK(two.driver()->driverId(two.driver()) == 0x6B636F4C);
		multiplayer.detachGame(two.controller.get());
		multiplayer.detachGame(one.controller.get());
	}
	// Exercise the actual newWindow gate on both sides of the old four-window
	// boundary without constructing forty display widgets in a headless test.
	Window* third = app.newWindow();
	Window* fourth = app.newWindow();
	CHECK(third && fourth);
	CHECK(app.windows().count() == MAX_GBAS);
	CHECK(!app.newWindow());
	for (QAction* action : newMultiplayer) CHECK(!action->isEnabled());
	wireless[1]->trigger();
	CHECK(app.windowLimit() == GBA_RFU_MAX_ADAPTERS);
	for (QAction* action : newMultiplayer) CHECK(action->isEnabled());
	Window* fifth = app.newWindow();
	CHECK(fifth);
	CHECK(app.windows().count() == 5);
	for (QAction* action : newMultiplayer) CHECK(action->isEnabled());
	QSet<int> windowInputs;
	for (Window* window : app.windows()) windowInputs.insert(window->inputController()->playerId());
	CHECK(windowInputs.size() == 5);
	CHECK(windowInputs.contains(4));
	cable[1]->trigger();
	CHECK(app.windowLimit() == MAX_GBAS);
	CHECK(!app.newWindow());
	for (QAction* action : newMultiplayer) CHECK(!action->isEnabled());
	delete third;
	delete fifth;
	delete fourth;
	CHECK(app.windows().count() == 2);
	for (QAction* action : newMultiplayer) CHECK(action->isEnabled());
	wireless[1]->trigger();
	checkWirelessRoom(multiplayer, config, 5);
	checkWirelessRoom(multiplayer, config, 8);
	checkWirelessRoom(multiplayer, config, GBA_RFU_MAX_ADAPTERS);
	{
		// Existing explicit suffixes are not bit positions or bounded by room
		// capacity. A collision takes the first free positive suffix instead.
		InertCore one(config), two(config), three(config);
		mCoreConfigSetOverrideIntValue(&one.controller->thread()->core->config, "savePlayerId", 40);
		mCoreConfigSetOverrideIntValue(&two.controller->thread()->core->config, "savePlayerId", 40);
		mCoreConfigSetOverrideIntValue(&three.controller->thread()->core->config, "savePlayerId", 100);
		CHECK(multiplayer.attachGame(one.controller.get()));
		CHECK(multiplayer.attachGame(two.controller.get()));
		CHECK(multiplayer.attachGame(three.controller.get()));
		CHECK(multiplayer.saveId(one.controller.get()) == 40);
		CHECK(multiplayer.saveId(two.controller.get()) == 1);
		CHECK(multiplayer.saveId(three.controller.get()) == 100);
		const auto claims = multiplayer.*testMember(ClaimedSavesMember{});
		CHECK(claims.size() == 1);
		CHECK(claims.constBegin().value() == QSet<int>({1, 40, 100}));
		const uint32_t oldSession = reinterpret_cast<GBASIORFUDriver*>(one.driver())->radioSession;
		multiplayer.detachGame(one.controller.get());
		CHECK(multiplayer.saveId(two.controller.get()) == 1);
		CHECK(multiplayer.saveId(three.controller.get()) == 100);
		CHECK(multiplayer.attachGame(one.controller.get()));
		CHECK(multiplayer.saveId(one.controller.get()) == 40);
		CHECK(reinterpret_cast<GBASIORFUDriver*>(one.driver())->radioSession != oldSession);
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})) == claims);
		multiplayer.detachGame(two.controller.get());
		multiplayer.detachGame(one.controller.get());
		multiplayer.detachGame(three.controller.get());
		CHECK(multiplayer.attached() == 0);
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})).isEmpty());
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})).isEmpty());
	}

	// Cable remains four-player, and rejected fifth attachments consume no IDs,
	// save slots or PIDs even after a forty-adapter RFU session.
	cable[0]->trigger();
	{
		std::array<std::unique_ptr<InertCore>, MAX_GBAS> cores;
		for (unsigned i = 0; i < cores.size(); ++i) {
			cores[i] = std::make_unique<InertCore>(config);
			CHECK(multiplayer.attachGame(cores[i]->controller.get()));
		}
		for (const auto& core : cores) CHECK(core->driver() && core->driver()->driverId(core->driver()) == 0x6B636F4C);
		CHECK(multiplayer.attached() == MAX_GBAS);
		CHECK(multiplayer.windowLimit() == MAX_GBAS);
		CHECK(!multiplayer.canAttachGame(mPLATFORM_GBA));
		CHECK(!multiplayer.canAttachGame(mPLATFORM_GB));
		wireless[0]->trigger();
		CHECK(multiplayer.wirelessEnabled());
		CHECK(!multiplayer.canAttachGame(mPLATFORM_GBA)); // Requested wireless does not expand active cable.
		const auto ids = multiplayer.*testMember(ClaimedIdsMember{});
		const auto saves = multiplayer.*testMember(ClaimedSavesMember{});
		const int pid = multiplayer.*testMember(NextPidMember{});
		InertCore excess(config);
		CHECK(!multiplayer.attachGame(excess.controller.get()));
		CHECK(!excess.driver());
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == ids);
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})) == saves);
		CHECK((multiplayer.*testMember(NextPidMember{})) == pid);
		for (unsigned i : {0U, 2U, 1U, 3U}) multiplayer.detachGame(cores[i]->controller.get());
		CHECK(multiplayer.attached() == 0);
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})).isEmpty());
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})).isEmpty());
	}
	wireless[1]->trigger();
#ifdef M_CORE_GB
	{
		InertCore one(config, mPLATFORM_GB), two(config, mPLATFORM_GB), excess(config, mPLATFORM_GB);
		CHECK(multiplayer.attachGame(one.controller.get()));
		CHECK(multiplayer.attachGame(two.controller.get()));
		CHECK(one.gbDriver() && two.gbDriver() && one.gbDriver() != two.gbDriver());
		CHECK(multiplayer.windowLimit() == MAX_GBS);
		CHECK(app.windowLimit() == MAX_GBS);
		CHECK(!multiplayer.canAttachGame(mPLATFORM_GB));
		CHECK(!multiplayer.canAttachGame(mPLATFORM_GBA));
		for (QAction* action : newMultiplayer) CHECK(!action->isEnabled());
		const auto ids = multiplayer.*testMember(ClaimedIdsMember{});
		const auto saves = multiplayer.*testMember(ClaimedSavesMember{});
		const int pid = multiplayer.*testMember(NextPidMember{});
		CHECK(!multiplayer.attachGame(excess.controller.get()));
		CHECK(!excess.gbDriver());
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})) == ids);
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})) == saves);
		CHECK((multiplayer.*testMember(NextPidMember{})) == pid);
		multiplayer.detachGame(one.controller.get());
		CHECK(two.gbDriver());
		CHECK(multiplayer.windowLimit() == MAX_GBS);
		multiplayer.detachGame(two.controller.get());
		CHECK(multiplayer.attached() == 0);
		CHECK(multiplayer.windowLimit() == GBA_RFU_MAX_ADAPTERS);
		CHECK(multiplayer.canAttachGame(mPLATFORM_GBA));
		CHECK(multiplayer.canAttachGame(mPLATFORM_GB));
		for (QAction* action : newMultiplayer) CHECK(action->isEnabled());
		CHECK((multiplayer.*testMember(ClaimedIdsMember{})).isEmpty());
		CHECK((multiplayer.*testMember(ClaimedSavesMember{})).isEmpty());
	}
#endif
	config.write();
	{
		ConfigController reloaded;
		CHECK(reloaded.getOption("gbaLinkMode").toInt() == 1);
	}
	delete second;
	delete first;
	CHECK(app.windows().isEmpty());
	CHECK(multiplayer.attached() == 0);
	{
		// Claim all forty real input slots without opening forty display windows.
		CHECK(InputController::MAX_INPUT_PLAYERS == GBA_RFU_MAX_ADAPTERS);
		std::array<std::unique_ptr<InputController>, GBA_RFU_MAX_ADAPTERS> inputs;
		QSet<int> ids;
		for (unsigned i = 0; i < inputs.size(); ++i) {
			inputs[i] = std::make_unique<InputController>();
			CHECK(inputs[i]->playerId() == static_cast<int>(i));
			ids.insert(inputs[i]->playerId());
		}
		CHECK(ids.size() == GBA_RFU_MAX_ADAPTERS);
		for (unsigned slot : {33U, 0U}) {
			inputs[slot].reset();
			inputs[slot] = std::make_unique<InputController>();
			CHECK(inputs[slot]->playerId() == static_cast<int>(slot));
		}
		for (unsigned i = 0; i < inputs.size(); i += 2) inputs[i].reset();
		for (unsigned i = 1; i < inputs.size(); i += 2) inputs[i].reset();
		InputController fresh;
		CHECK(fresh.playerId() == 0);
	}
	std::printf("PASS Qt wireless config/menu/session integration (%u checks). No ROM, BIOS, CPU loop, audio or save used.\n", checks);
	return 0;
}
