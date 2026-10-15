/* Native startup regression for the real production entry-point object.
 * The runner only renames its main/wmain symbols; their bodies, static plugin
 * imports, application construction, default renderer and cleanup are reused.
 * The timer supplies bounded assertions/normal close from inside that process.
 * No ROM, BIOS, CPU loop, save, external UI automation or renderer override. */
#include "ConfigController.h"
#include "DisplayGL.h"
#include "GBAApp.h"
#include "Window.h"

#include <QApplication>
#include <QDir>
#include <QTimer>

#include <windows.h>
#include <cstdio>
#include <cstdlib>

#ifndef _WIN32
#error This fixture exercises the Windows Unicode application entry point
#endif
#if !defined(BUILD_GL) && !defined(BUILD_GLES2) && !defined(BUILD_GLES3) && !defined(USE_EPOXY)
#error This fixture requires the production OpenGL display path
#endif

extern "C" int mgba_app_wmain(int argc, wchar_t* argv[]);

static FILE* report;
static unsigned checks;
static UINT_PTR timerId;
static DWORD ownerThread;
static ULONGLONG startedAt;
static bool observedStartup;
static bool observedQuit;

static void note(const char* message) {
	std::fprintf(report, "%s\n", message);
	std::fflush(report);
}

static void require(bool success, const char* expression, int line) {
	++checks;
	if (success) return;
	std::fprintf(report, "FAIL line %d: %s\n", line, expression);
	std::fflush(report);
	std::_Exit(1); // Failure only: the outer runner also has an owned-PID timeout.
}
#define CHECK(expr) require(bool(expr), #expr, __LINE__)

static void CALLBACK validateAndClose(HWND, UINT, UINT_PTR id, DWORD) {
	KillTimer(nullptr, id);
	timerId = 0;
	CHECK(GetCurrentThreadId() == ownerThread);
	auto* app = qobject_cast<QGBA::GBAApp*>(QCoreApplication::instance());
	QGBA::Window* window = nullptr;
	if (app) {
		// A native timer can also fire inside a nested startup event loop.
		// Do not read GBAApp members until production has shown its Window.
		for (QWidget* widget : QApplication::topLevelWidgets()) {
			auto* candidate = qobject_cast<QGBA::Window*>(widget);
			if (candidate && candidate->isVisible()) { window = candidate; break; }
		}
	}
	if (!window) {
		CHECK(GetTickCount64() - startedAt < 12000);
		note("WAIT production window not ready yet");
		timerId = SetTimer(nullptr, 0, 250, validateAndClose);
		CHECK(timerId != 0);
		return;
	}
	note("TIMER production event loop/window reached");
	CHECK(app);
	CHECK(QGuiApplication::platformName() == QStringLiteral("windows"));
	CHECK(QGBA::ConfigController::isPortable());
	CHECK(QDir(QGBA::ConfigController::configDir()).canonicalPath() == QDir::current().canonicalPath());
	CHECK(app->windows().size() == 1);
	CHECK(window == app->windows().first());
	CHECK(window && window->isVisible());
	CHECK(!window->controller());
	CHECK(window->config()->getOption("gbaLinkMode").toInt() == 0);
	const QVariant configuredRenderer = window->config()->getQtOption("displayDriver");
	CHECK(configuredRenderer.isNull() || configuredRenderer.toInt() == int(QGBA::Display::Driver::OPENGL));
	const QList<QGBA::DisplayGL*> displays = window->findChildren<QGBA::DisplayGL*>();
	CHECK(displays.size() == 1);
	CHECK(!displays.first()->isDrawing());
	note("READY visible QGBA Window with default DisplayGL, no game/controller");
	observedStartup = true;
	QObject::connect(app, &QCoreApplication::aboutToQuit, [] {
		observedQuit = true;
		note("QUIT production aboutToQuit observed");
	});
	// These are in-process Qt test operations, not native desktop input or
	// messages sent to another application's windows.
	QApplication::closeAllWindows();
	QTimer::singleShot(0, app, &QCoreApplication::quit);
}

extern "C" int wmain(int argc, wchar_t* argv[]) {
	// Keep an expected negative-control crash from opening a system error
	// dialog. This changes error reporting for this fixture process only.
	SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
	report = std::fopen("startup-report.txt", "wb");
	if (!report) return 2;
	note("BEGIN wrapper calling renamed production wmain/main object");
	CHECK(argc == 1); // No fixture options alter production argument parsing.
	ownerThread = GetCurrentThreadId();
	startedAt = GetTickCount64();
	timerId = SetTimer(nullptr, 0, 4000, validateAndClose);
	CHECK(timerId != 0);
	const int result = mgba_app_wmain(argc, argv);
	if (timerId) KillTimer(nullptr, timerId);
	CHECK(observedStartup);
	CHECK(observedQuit);
	CHECK(result == 0);
	CHECK(QCoreApplication::instance() == nullptr);
	std::fprintf(report, "PASS production Windows startup/default OpenGL/normal close (%u checks). No ROM or renderer override.\n", checks);
	std::fclose(report);
	return result;
}
