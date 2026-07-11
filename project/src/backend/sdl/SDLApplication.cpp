/**
 * This class is where the main loop goes. For one, windows 10;
 * The said main loop uses:
   - A combination of vsync counter and SDL3 sleep with EMA to create a surreal rhythm game experience!
 * On the other hand, I wanted to create a fun crispy smooth experience for literally everyone who are on windows,
 so that meant doing it in the first place to compensate. How about I make a literal main loop library out of this?
**/

#include "FramePredictor.h"
#include "SDLApplication.h"
#include "SDLGamepad.h"
#include "SDLJoystick.h"
#include "SDLWindow.h"
#include <system/System.h>
#include <SDL_syswm.h>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <string>
#include <stdio.h>
#include <vector>
#include <array>
#include <atomic>
#include <mutex>

using namespace std;

#ifdef HX_WINDOWS
#include <windows.h>
#include <cstdint>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

#ifdef HX_LINUX
#include <fcntl.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <errno.h>
#include <cstring>
#undef KEY_DOWN
#undef KEY_UP
#endif

#ifdef HX_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef EMSCRIPTEN
#include "emscripten.h"
#endif

#include <fstream>
#include <sstream>
#include <cmath>

#include <iostream>
#include <stdexcept>

#ifdef _WIN32
#include <windows.h>
#include <immintrin.h>
#else
#include <sched.h>
#if HX_LINUX
#include <poll.h>
#include <x86intrin.h>
#include <dlfcn.h>
#endif
#if HX_ANDROID
#endif
#ifdef __APPLE__
#include <thread>
#include <mach/thread_policy.h>
#include <mach/mach.h>
#endif
#endif

namespace lime
{

	AutoGCRoot *Application::callback = 0;
	SDLApplication *SDLApplication::currentApplication = 0;

	const int analogAxisDeadZone = 1000;
	std::map<int, std::map<int, int>> gamepadsAxisMap;
	bool inBackground = false;

	void SDLApplication::HandleEvent(SDL_Event *event)
	{

#if defined(IPHONE) || defined(EMSCRIPTEN)

		int top = 0;
		gc_set_top_of_stack(&top, false);

#endif

		switch (event->type)
		{

		case SDL_APP_WILLENTERBACKGROUND:

			inBackground = true;

			windowEvent.type = WINDOW_DEACTIVATE;
			WindowEvent::Dispatch(&windowEvent);
			break;

		case SDL_APP_WILLENTERFOREGROUND:

			break;

		case SDL_APP_DIDENTERFOREGROUND:

			windowEvent.type = WINDOW_ACTIVATE;
			WindowEvent::Dispatch(&windowEvent);

			inBackground = false;
			break;

		case SDL_WINDOWEVENT:

			switch (event->window.event)
			{

			case SDL_WINDOWEVENT_ENTER:
			case SDL_WINDOWEVENT_LEAVE:
			case SDL_WINDOWEVENT_SHOWN:
			case SDL_WINDOWEVENT_HIDDEN:
			case SDL_WINDOWEVENT_FOCUS_GAINED:
			case SDL_WINDOWEVENT_FOCUS_LOST:
			case SDL_WINDOWEVENT_MAXIMIZED:
			case SDL_WINDOWEVENT_MINIMIZED:
			case SDL_WINDOWEVENT_MOVED:
			case SDL_WINDOWEVENT_RESTORED:

				ProcessWindowEvent(event);
				break;

			case SDL_WINDOWEVENT_EXPOSED:

				ProcessWindowEvent(event);

				break;

			case SDL_WINDOWEVENT_SIZE_CHANGED:

				ProcessWindowEvent(event);

				if (!inBackground)
				{

					RenderEvent::Dispatch(&renderEvent);
				}

				break;

			case SDL_WINDOWEVENT_CLOSE:

				ProcessWindowEvent(event);

				// Avoid handling SDL_QUIT if in response to window.close
				SDL_Event event;

				if (SDL_PollEvent(&event))
				{

					if (event.type != SDL_QUIT)
					{

						HandleEvent(&event);
					}
				}

				break;
			}

			break;

		case SDL_QUIT:

			active = false;
			Quit();

			break;
		}
	}

	void SDLApplication::HandleInputEvent(SDL_Event *event)
	{

#if defined(IPHONE) || defined(EMSCRIPTEN)
		int top = 0;
		gc_set_top_of_stack(&top, false);
#endif

		switch (event->type)
		{
		// Clipboard
		case SDL_CLIPBOARDUPDATE:
			ProcessClipboardEvent(event);
			break;

		// Gamepad
		case SDL_CONTROLLERAXISMOTION:
		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP:
		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED:
			ProcessGamepadEvent(event);
			break;

		// Joystick / Sensors
		case SDL_JOYAXISMOTION:
			if (SDLJoystick::IsAccelerometer(event->jaxis.which))
			{
				ProcessSensorEvent(event);
			}
			else
			{
				ProcessJoystickEvent(event);
			}
			break;

		case SDL_JOYBALLMOTION:
		case SDL_JOYBUTTONDOWN:
		case SDL_JOYBUTTONUP:
		case SDL_JOYHATMOTION:
		case SDL_JOYDEVICEADDED:
		case SDL_JOYDEVICEREMOVED:
			ProcessJoystickEvent(event);
			break;

		// Keyboard
		case SDL_KEYDOWN:
		case SDL_KEYUP:
			ProcessKeyEvent(event);
			break;

		// Mouse
		case SDL_MOUSEMOTION:
		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP:
		case SDL_MOUSEWHEEL:
			ProcessMouseEvent(event);
			break;

		// Text input
		case SDL_TEXTINPUT:
		case SDL_TEXTEDITING:
			ProcessTextEvent(event);
			break;

		// Touch
		case SDL_FINGERMOTION:
		case SDL_FINGERDOWN:
		case SDL_FINGERUP:
			ProcessTouchEvent(event);
			break;
		}
	}

	void SDLApplication::ProcessClipboardEvent(SDL_Event *event)
	{

		if (ClipboardEvent::callback)
		{

			clipboardEvent.type = CLIPBOARD_UPDATE;

			ClipboardEvent::Dispatch(&clipboardEvent);
		}
	}

	void SDLApplication::ProcessDropEvent(SDL_Event *event)
	{

		if (DropEvent::callback)
		{

			dropEvent.type = DROP_FILE;
			dropEvent.file = (vbyte *)event->drop.file;

			DropEvent::Dispatch(&dropEvent);
			SDL_free(dropEvent.file);
		}
	}

	void SDLApplication::ProcessGamepadEvent(SDL_Event *event)
	{

		if (GamepadEvent::callback)
		{

			switch (event->type)
			{

			case SDL_CONTROLLERAXISMOTION:

				if (gamepadsAxisMap[event->caxis.which].empty())
				{

					gamepadsAxisMap[event->caxis.which][event->caxis.axis] = event->caxis.value;
				}
				else if (gamepadsAxisMap[event->caxis.which][event->caxis.axis] == event->caxis.value)
				{

					break;
				}

				gamepadEvent.type = GAMEPAD_AXIS_MOVE;
				gamepadEvent.axis = event->caxis.axis;
				gamepadEvent.id = event->caxis.which;

				if (event->caxis.value > -analogAxisDeadZone && event->caxis.value < analogAxisDeadZone)
				{

					if (gamepadsAxisMap[event->caxis.which][event->caxis.axis] != 0)
					{

						gamepadsAxisMap[event->caxis.which][event->caxis.axis] = 0;
						gamepadEvent.axisValue = 0;
						GamepadEvent::Dispatch(&gamepadEvent);
					}

					break;
				}

				gamepadsAxisMap[event->caxis.which][event->caxis.axis] = event->caxis.value;
				gamepadEvent.axisValue = event->caxis.value / (event->caxis.value > 0 ? 32767.0 : 32768.0);

				GamepadEvent::Dispatch(&gamepadEvent);
				break;

			case SDL_CONTROLLERBUTTONDOWN:

				gamepadEvent.type = GAMEPAD_BUTTON_DOWN;
				gamepadEvent.button = event->cbutton.button;
				gamepadEvent.id = event->cbutton.which;

				GamepadEvent::Dispatch(&gamepadEvent);
				break;

			case SDL_CONTROLLERBUTTONUP:

				gamepadEvent.type = GAMEPAD_BUTTON_UP;
				gamepadEvent.button = event->cbutton.button;
				gamepadEvent.id = event->cbutton.which;

				GamepadEvent::Dispatch(&gamepadEvent);
				break;

			case SDL_CONTROLLERDEVICEADDED:

				if (SDLGamepad::Connect(event->cdevice.which))
				{

					gamepadEvent.type = GAMEPAD_CONNECT;
					gamepadEvent.id = SDLGamepad::GetInstanceID(event->cdevice.which);

					GamepadEvent::Dispatch(&gamepadEvent);
				}

				break;

			case SDL_CONTROLLERDEVICEREMOVED:
			{

				gamepadEvent.type = GAMEPAD_DISCONNECT;
				gamepadEvent.id = event->cdevice.which;

				GamepadEvent::Dispatch(&gamepadEvent);
				SDLGamepad::Disconnect(event->cdevice.which);
				break;
			}
			}
		}
	}

	void SDLApplication::ProcessJoystickEvent(SDL_Event *event)
	{

		if (JoystickEvent::callback)
		{

			switch (event->type)
			{

			case SDL_JOYAXISMOTION:

				if (!SDLJoystick::IsAccelerometer(event->jaxis.which))
				{

					joystickEvent.type = JOYSTICK_AXIS_MOVE;
					joystickEvent.index = event->jaxis.axis;
					joystickEvent.x = event->jaxis.value / (event->jaxis.value > 0 ? 32767.0 : 32768.0);
					joystickEvent.id = event->jaxis.which;

					JoystickEvent::Dispatch(&joystickEvent);
				}
				break;

			case SDL_JOYBALLMOTION:

				if (!SDLJoystick::IsAccelerometer(event->jball.which))
				{

					joystickEvent.type = JOYSTICK_TRACKBALL_MOVE;
					joystickEvent.index = event->jball.ball;
					joystickEvent.x = event->jball.xrel / (event->jball.xrel > 0 ? 32767.0 : 32768.0);
					joystickEvent.y = event->jball.yrel / (event->jball.yrel > 0 ? 32767.0 : 32768.0);
					joystickEvent.id = event->jball.which;

					JoystickEvent::Dispatch(&joystickEvent);
				}
				break;

			case SDL_JOYBUTTONDOWN:

				if (!SDLJoystick::IsAccelerometer(event->jbutton.which))
				{

					joystickEvent.type = JOYSTICK_BUTTON_DOWN;
					joystickEvent.index = event->jbutton.button;
					joystickEvent.id = event->jbutton.which;

					JoystickEvent::Dispatch(&joystickEvent);
				}
				break;

			case SDL_JOYBUTTONUP:

				if (!SDLJoystick::IsAccelerometer(event->jbutton.which))
				{

					joystickEvent.type = JOYSTICK_BUTTON_UP;
					joystickEvent.index = event->jbutton.button;
					joystickEvent.id = event->jbutton.which;

					JoystickEvent::Dispatch(&joystickEvent);
				}
				break;

			case SDL_JOYHATMOTION:

				if (!SDLJoystick::IsAccelerometer(event->jhat.which))
				{

					joystickEvent.type = JOYSTICK_HAT_MOVE;
					joystickEvent.index = event->jhat.hat;
					joystickEvent.eventValue = event->jhat.value;
					joystickEvent.id = event->jhat.which;

					JoystickEvent::Dispatch(&joystickEvent);
				}
				break;

			case SDL_JOYDEVICEADDED:

				if (SDLJoystick::Connect(event->jdevice.which))
				{

					joystickEvent.type = JOYSTICK_CONNECT;
					joystickEvent.id = SDLJoystick::GetInstanceID(event->jdevice.which);

					JoystickEvent::Dispatch(&joystickEvent);
				}
				break;

			case SDL_JOYDEVICEREMOVED:

				if (!SDLJoystick::IsAccelerometer(event->jdevice.which))
				{

					joystickEvent.type = JOYSTICK_DISCONNECT;
					joystickEvent.id = event->jdevice.which;

					JoystickEvent::Dispatch(&joystickEvent);
					SDLJoystick::Disconnect(event->jdevice.which);
				}
				break;
			}
		}
	}

	void SDLApplication::ProcessKeyEvent(SDL_Event *event)
	{

		if (KeyEvent::callback)
		{

			switch (event->type)
			{

			case SDL_KEYDOWN:
				keyEvent.type = KEY_DOWN;
				break;
			case SDL_KEYUP:
				keyEvent.type = KEY_UP;
				break;
			}

			keyEvent.keyCode = event->key.keysym.sym;
			keyEvent.modifier = event->key.keysym.mod;
			keyEvent.windowID = event->key.windowID;

			if (keyEvent.type == KEY_DOWN)
			{

				if (keyEvent.keyCode == SDLK_CAPSLOCK)
					keyEvent.modifier |= KMOD_CAPS;
				if (keyEvent.keyCode == SDLK_LALT)
					keyEvent.modifier |= KMOD_LALT;
				if (keyEvent.keyCode == SDLK_LCTRL)
					keyEvent.modifier |= KMOD_LCTRL;
				if (keyEvent.keyCode == SDLK_LGUI)
					keyEvent.modifier |= KMOD_LGUI;
				if (keyEvent.keyCode == SDLK_LSHIFT)
					keyEvent.modifier |= KMOD_LSHIFT;
				if (keyEvent.keyCode == SDLK_MODE)
					keyEvent.modifier |= KMOD_MODE;
				if (keyEvent.keyCode == SDLK_NUMLOCKCLEAR)
					keyEvent.modifier |= KMOD_NUM;
				if (keyEvent.keyCode == SDLK_RALT)
					keyEvent.modifier |= KMOD_RALT;
				if (keyEvent.keyCode == SDLK_RCTRL)
					keyEvent.modifier |= KMOD_RCTRL;
				if (keyEvent.keyCode == SDLK_RGUI)
					keyEvent.modifier |= KMOD_RGUI;
				if (keyEvent.keyCode == SDLK_RSHIFT)
					keyEvent.modifier |= KMOD_RSHIFT;
			}

			KeyEvent::Dispatch(&keyEvent);
		}
	}

	void SDLApplication::ProcessMouseEvent(SDL_Event *event)
	{

		if (MouseEvent::callback)
		{

			switch (event->type)
			{

			case SDL_MOUSEMOTION:

				mouseEvent.type = MOUSE_MOVE;
				mouseEvent.x = event->motion.x;
				mouseEvent.y = event->motion.y;
				mouseEvent.movementX = event->motion.xrel;
				mouseEvent.movementY = event->motion.yrel;
				break;

			case SDL_MOUSEBUTTONDOWN:

				SDL_CaptureMouse(SDL_TRUE);

				mouseEvent.type = MOUSE_DOWN;
				mouseEvent.button = event->button.button - 1;
				mouseEvent.x = event->button.x;
				mouseEvent.y = event->button.y;
				mouseEvent.clickCount = event->button.clicks;
				break;

			case SDL_MOUSEBUTTONUP:

				SDL_CaptureMouse(SDL_FALSE);

				mouseEvent.type = MOUSE_UP;
				mouseEvent.button = event->button.button - 1;
				mouseEvent.x = event->button.x;
				mouseEvent.y = event->button.y;
				mouseEvent.clickCount = event->button.clicks;
				break;

			case SDL_MOUSEWHEEL:

				mouseEvent.type = MOUSE_WHEEL;

				if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
				{

					mouseEvent.x = -event->wheel.x;
					mouseEvent.y = -event->wheel.y;
				}
				else
				{

					mouseEvent.x = event->wheel.x;
					mouseEvent.y = event->wheel.y;
				}
				break;
			}

			mouseEvent.windowID = event->button.windowID;
			MouseEvent::Dispatch(&mouseEvent);
		}
	}

	void SDLApplication::ProcessSensorEvent(SDL_Event *event)
	{

		if (SensorEvent::callback)
		{

			double value = event->jaxis.value / 32767.0f;

			switch (event->jaxis.axis)
			{

			case 0:
				sensorEvent.x = value;
				break;
			case 1:
				sensorEvent.y = value;
				break;
			case 2:
				sensorEvent.z = value;
				break;
			default:
				break;
			}

			SensorEvent::Dispatch(&sensorEvent);
		}
	}

	void SDLApplication::ProcessTextEvent(SDL_Event *event)
	{
		if (TextEvent::callback)
		{
			switch (event->type)
			{
			case SDL_TEXTINPUT:
				textEvent.type = TEXT_INPUT;
				break;

			case SDL_TEXTEDITING:
				textEvent.type = TEXT_EDIT;
				textEvent.start = event->edit.start;
				textEvent.length = event->edit.length;
				break;
			}

			static char textBuffer[SDL_TEXTINPUTEVENT_TEXT_SIZE];
			strncpy(textBuffer, event->text.text, SDL_TEXTINPUTEVENT_TEXT_SIZE - 1);
			textBuffer[SDL_TEXTINPUTEVENT_TEXT_SIZE - 1] = '\0';
			textEvent.text = (vbyte *)textBuffer;

			textEvent.windowID = event->text.windowID;
			TextEvent::Dispatch(&textEvent);
		}
	}

	void SDLApplication::ProcessTouchEvent(SDL_Event *event)
	{

		if (TouchEvent::callback)
		{

			switch (event->type)
			{

			case SDL_FINGERMOTION:

				touchEvent.type = TOUCH_MOVE;
				break;

			case SDL_FINGERDOWN:

				touchEvent.type = TOUCH_START;
				break;

			case SDL_FINGERUP:

				touchEvent.type = TOUCH_END;
				break;
			}

			touchEvent.x = event->tfinger.x;
			touchEvent.y = event->tfinger.y;
			touchEvent.id = event->tfinger.fingerId;
			touchEvent.dx = event->tfinger.dx;
			touchEvent.dy = event->tfinger.dy;
			touchEvent.pressure = event->tfinger.pressure;
			touchEvent.device = event->tfinger.touchId;

			TouchEvent::Dispatch(&touchEvent);
		}
	}

	void SDLApplication::ProcessWindowEvent(SDL_Event *event)
	{

		if (WindowEvent::callback)
		{

			switch (event->window.event)
			{

			case SDL_WINDOWEVENT_SHOWN:
				windowEvent.type = WINDOW_SHOW;
				break;
			case SDL_WINDOWEVENT_CLOSE:
				windowEvent.type = WINDOW_CLOSE;
				// For main window close, set a flag to break out of the main loop
				if (event->window.windowID == 1) { // Assuming main window ID is 1
					active = false;
				}
				break;
			case SDL_WINDOWEVENT_HIDDEN:
				windowEvent.type = WINDOW_HIDE;
				break;
			case SDL_WINDOWEVENT_ENTER:
				windowEvent.type = WINDOW_ENTER;
				break;
			case SDL_WINDOWEVENT_FOCUS_GAINED:
				windowEvent.type = WINDOW_FOCUS_IN;
				break;
			case SDL_WINDOWEVENT_FOCUS_LOST:
				windowEvent.type = WINDOW_FOCUS_OUT;
				break;
			case SDL_WINDOWEVENT_LEAVE:
				windowEvent.type = WINDOW_LEAVE;
				break;
			case SDL_WINDOWEVENT_MAXIMIZED:
				windowEvent.type = WINDOW_MAXIMIZE;
				break;
			case SDL_WINDOWEVENT_MINIMIZED:
				windowEvent.type = WINDOW_MINIMIZE;
				break;
			case SDL_WINDOWEVENT_EXPOSED:
				windowEvent.type = WINDOW_EXPOSE;
				break;

			case SDL_WINDOWEVENT_MOVED:

				windowEvent.type = WINDOW_MOVE;
				windowEvent.x = event->window.data1;
				windowEvent.y = event->window.data2;

				break;

			case SDL_WINDOWEVENT_SIZE_CHANGED:

				windowEvent.type = WINDOW_RESIZE;
				windowEvent.width = event->window.data1;
				windowEvent.height = event->window.data2;
				break;

			case SDL_WINDOWEVENT_RESTORED:
				windowEvent.type = WINDOW_RESTORE;
				break;
			}

			windowEvent.windowID = event->window.windowID;
			WindowEvent::Dispatch(&windowEvent);
		}
	}

	// ---------- Timing configuration in 10ns ticks ----------
	// 1 second = 100000000 ticks of 10ns
	constexpr int64_t TICKS_PER_SECOND_10NS = 100000000LL;

	// Default target frame rates
	static int64_t UPDATE_PERIOD_10NS = TICKS_PER_SECOND_10NS / 120LL;
	static int64_t RENDER_PERIOD_10NS = TICKS_PER_SECOND_10NS / 60LL;

	static int64_t lastRenderTime = 0;
	static int64_t render_timestamp = 0;


	static Uint32 initFlags;

#if HX_WINDOWS
	static HMODULE ntdll;
	static LARGE_INTEGER qpcFrequency = {};

	void fixTimeResolution()
	{
		typedef NTSTATUS(NTAPI * NtSetTimerResolution_t)(ULONG, BOOLEAN, PULONG);
		typedef NTSTATUS(NTAPI * NtQueryTimerResolution_t)(PULONG, PULONG, PULONG);

		if (!ntdll)
			ntdll = LoadLibraryA("ntdll.dll");
		if (!ntdll)
			return;

		static NtSetTimerResolution_t NtSetTimerResolution =
			(NtSetTimerResolution_t)GetProcAddress(ntdll, "NtSetTimerResolution");
		static NtQueryTimerResolution_t NtQueryTimerResolution =
			(NtQueryTimerResolution_t)GetProcAddress(ntdll, "NtQueryTimerResolution");

		if (!NtSetTimerResolution || !NtQueryTimerResolution)
			return;

		ULONG minRes = 0, maxRes = 0, curRes = 0;
		NtQueryTimerResolution(&minRes, &maxRes, &curRes);

		printf("Timer Resolution Range: min=%.3f ms, max=%.3f ms, current=%.3f ms\n",
			   minRes / 10000.0, maxRes / 10000.0, curRes / 10000.0);

		ULONG current = 0;
		NTSTATUS status = NtSetTimerResolution(5000, TRUE, &current);

		printf("NtSetTimerResolution -> Status: 0x%08X, Current: %.3f ms\n",
			   (unsigned int)status, current / 10000.0);

		NtQueryTimerResolution(&minRes, &maxRes, &curRes);
		printf("Updated Timer Resolution: min=%.3f ms, max=%.3f ms, current=%.3f ms\n\n",
			   minRes / 10000.0, maxRes / 10000.0, curRes / 10000.0);
	}
#endif
	static bool alreadyQuit = false;

	SDLApplication::SDLApplication()
		: active(true)  // Initialize active flag
	{
#ifdef HX_LINUX
		// Initialize Xlib thread safety - CRITICAL for multi-threaded X11 access
		XInitThreads();
#endif

		initFlags = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK;
#if defined(LIME_MOJOAL) || defined(LIME_OPENALSOFT)
		initFlags |= SDL_INIT_AUDIO;
#endif

		if (SDL_Init(initFlags) != 0)
		{
			printf("Could not initialize SDL: %s.\n", SDL_GetError());
		}

		SDL_LogSetPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN);

		currentApplication = this;

		ApplicationEvent applicationEvent;
		AsyncKeyEvent asyncKeyEvent;
		SubLoopTickEvent subLoopTickEvent;
		ClipboardEvent clipboardEvent;
		DropEvent dropEvent;
		GamepadEvent gamepadEvent;
		JoystickEvent joystickEvent;
		KeyEvent keyEvent;
		MouseEvent mouseEvent;
		RenderEvent renderEvent;
		SensorEvent sensorEvent;
		TextEvent textEvent;
		TouchEvent touchEvent;
		WindowEvent windowEvent;

		SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
		SDLJoystick::Init();

#ifdef HX_MACOS
		CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL(CFBundleGetMainBundle());
		char path[PATH_MAX];

		if (CFURLGetFileSystemRepresentation(resourcesURL, TRUE, (UInt8 *)path, PATH_MAX))
		{
			chdir(path);
		}

		CFRelease(resourcesURL);
#endif

#if HX_WINDOWS
		QueryPerformanceFrequency(&qpcFrequency);
		fixTimeResolution();
#endif
		
		// Reset static variables
		alreadyQuit = false;
	}

	SDLApplication::~SDLApplication()
	{
#if HX_WINDOWS
		if (ntdll)
			FreeLibrary(ntdll);
#endif
	}

	int64_t getTime10ns()
	{
#ifdef HX_WINDOWS
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		return (now.QuadPart * TICKS_PER_SECOND_10NS) / qpcFrequency.QuadPart;
#else
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return ts.tv_sec * TICKS_PER_SECOND_10NS + (ts.tv_nsec / 10LL);
#endif
	}

	// SDL3-style precise delay implementation
	// as seen here: https://github.com/libsdl-org/SDL/blob/370e9407b585466b5ac54cb5240d5eb1e11fc80b/src/timer/SDL_timer.c#L664
	//
	// FIX: smoothedOvershootNs is no longer static — it resets each call so that
	// transient scheduler spikes early in a session don't permanently bias future
	// sleeps and eventually collapse the loop into a pure spin.
	// The EMA still tracks within a single sleep call, which is all it needs to do.

#if HX_WINDOWS
	static bool hasHighRes = false;
#endif

	void coolSleepUntil10ns(int64_t wakeTime10ns)
	{
		int64_t current_value = getTime10ns();
		const int64_t target_value = wakeTime10ns;

		if (current_value >= target_value)
			return;

#if HX_WINDOWS
		static HANDLE localTimer = nullptr;
		static bool triedHighRes = false;

		if (!triedHighRes) {
			localTimer = CreateWaitableTimerEx(nullptr, nullptr,
				CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
			hasHighRes = (localTimer != nullptr);
			triedHighRes = true;
			if (!hasHighRes)
				printf("High-res waitable timer unavailable, falling back to Sleep()\n");
		}
#endif

		const int64_t SPIN_WINDOW_10NS = 2000LL; // 20µs spin window

		// Per-call overshoot estimate — starts at a conservative 0.5ms.
		// Not static: we don't want a bad sleep from one frame (or one session startup)
		// to permanently shrink all future sleeps into a spin loop.
		int64_t localOvershootNs = 50000LL;

		// --- Coarse sleep pass ---
		while (true) {
			current_value = getTime10ns();
			int64_t remaining_10ns = target_value - current_value;

			if (remaining_10ns <= SPIN_WINDOW_10NS)
				break;

			int64_t sleep_10ns = remaining_10ns - SPIN_WINDOW_10NS;
			int64_t sleep_ns   = sleep_10ns * 10LL;
			int64_t before_sleep = current_value;

#if HX_WINDOWS
			if (hasHighRes) {
				LARGE_INTEGER due;
				due.QuadPart = -(LONGLONG)(sleep_ns / 100LL);
				if (due.QuadPart == 0) due.QuadPart = -1;
				if (SetWaitableTimer(localTimer, &due, 0, nullptr, nullptr, FALSE)) {
					WaitForSingleObject(localTimer, INFINITE);
				} else {
					Sleep(0);
				}
			} else {
				DWORD ms = (DWORD)(sleep_ns / 1000000LL);
				if (ms > 0) Sleep(ms); else Sleep(0);
			}
#else
			struct timespec ts;
			ts.tv_sec  = sleep_ns / 1000000000LL;
			ts.tv_nsec = sleep_ns % 1000000000LL;
			nanosleep(&ts, nullptr);
#endif

			int64_t now = getTime10ns();
			int64_t actual_ns    = (now - before_sleep) * 10LL;
			int64_t overshoot_ns = actual_ns - sleep_ns;

			if (overshoot_ns > 25000LL) {
				// EMA within this call only — tracks the trend for the remaining
				// sleep iterations without persisting across frames.
				localOvershootNs = (int64_t)(localOvershootNs * 0.75 + overshoot_ns * 0.25);
				if (localOvershootNs <= 25000LL)   localOvershootNs = 25000LL;
				if (localOvershootNs >= 2000000LL) localOvershootNs = 2000000LL;

				if (now >= target_value)
					return;
			}
		}

		// --- Final spin: only covers SPIN_WINDOW_10NS = 200µs ---
		while (getTime10ns() < target_value) {
#if defined(_MSC_VER)
			_mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
			__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
			__asm__ __volatile__("yield" ::: "memory");
#endif
		}
	}

	static int64_t minimalSleepCalc10ns = 0;

	// The canonical sleep chunk size for the current refresh rate.
	// minimalSleepCalc10ns is allowed to shrink per-frame near vblank, but this
	// value is the floor it resets to at the start of each frame so that one
	// bad prediction can never permanently collapse the sleep into a spin.
	static int64_t minimalSleepCalcBase10ns = 0;

	static int cachedRefreshRate = 0;

	static void calculateMinimalSleepTime()
	{
		SDL_Window* kbFocus = SDL_GetKeyboardFocus();
		if (!kbFocus) {
			minimalSleepCalcBase10ns = minimalSleepCalc10ns = 100000;
			return;
		}
		uint32_t focusedWindowID = SDL_GetWindowID(kbFocus);
		SDLWindow* focusedWindow = SDLWindow::windows[focusedWindowID];

		if (!focusedWindow || !focusedWindow->sdlWindow) {
			minimalSleepCalcBase10ns = minimalSleepCalc10ns = 100000;
			return;
		}

		SDL_DisplayMode mode;
		if (SDL_GetWindowDisplayMode(focusedWindow->sdlWindow, &mode) != 0) {
			minimalSleepCalcBase10ns = minimalSleepCalc10ns = 100000;
			return;
		}

		if (mode.refresh_rate == cachedRefreshRate && minimalSleepCalc10ns != 0)
			return;

		cachedRefreshRate = mode.refresh_rate;

		if (cachedRefreshRate == 0) {
			minimalSleepCalcBase10ns = minimalSleepCalc10ns = 100000;
			return;
		}

		// Sub-frame sleep chunks sized so N fit cleanly in one frame period.
		// 50Hz:  100000  (20 chunks, 1.0ms each)
		// 60Hz:  104167  (16 chunks, ~1.0417ms)
		// 75Hz:  102564  (13 chunks, ~1.0256ms)
		// 85Hz:  106951  (11 chunks, ~1.0695ms)
		// 144Hz: 115741  ( 7 chunks, ~1.1574ms)
		// 165Hz: 101010  ( 6 chunks, ~1.0101ms)
		if (cachedRefreshRate % 50 == 0) {
			minimalSleepCalcBase10ns = 100000;
		} else if (cachedRefreshRate % 60 == 0) {
			minimalSleepCalcBase10ns = 104167;
		} else if (cachedRefreshRate % 75 == 0) {
			minimalSleepCalcBase10ns = 102564;
		} else if (cachedRefreshRate % 85 == 0) {
			minimalSleepCalcBase10ns = 106951;
		} else if (cachedRefreshRate % 144 == 0) {
			minimalSleepCalcBase10ns = 115741;
		} else if (cachedRefreshRate % 165 == 0) {
			minimalSleepCalcBase10ns = 101010;
		} else {
			minimalSleepCalcBase10ns = 100000;
		}

		#if HX_WINDOWS
		if (hasHighRes) {
			minimalSleepCalcBase10ns /= 2;
		}
		#endif

		minimalSleepCalc10ns = minimalSleepCalcBase10ns;
	}

	// ---------- FramePredictor integration ----------
	// When true, Update() uses the FramePredictor's isTime() flag to dispatch
	// both UPDATE and RENDER together at the monitor's actual vblank, with
	// the exact measured frame time. Bypasses the 120Hz/60Hz grid math.
	static bool useFramePredictor = false;
	static FramePredictor g_predictor;

	// ------------------------------------------------------------------
	// FramePredictor bootstrap.
	//
	// Captures getTime10ns2() as the anchor and uses the current
	// UPDATE_PERIOD_10NS to derive the frame rate. No swaps, no
	// glFinish, no DWM/DRM/Choreographer polling — just a plain
	// spin-wait predictor driven by the caller-supplied rate.
	// ------------------------------------------------------------------
	void InitFramePredictor()
	{
		if (UPDATE_PERIOD_10NS <= 0) {
			printf("[FramePredictor] UPDATE_PERIOD_10NS is 0 — cannot init (no frame rate)\n");
			return;
		}

		// --- On Linux, try to initialize Wayland vblank source ---
		// This must happen BEFORE FramePredictor::Init() so that
		// Init() can anchor to a real vblank via Wayland.
#if HX_LINUX
		{
			SDL_Window* kbFocus = SDL_GetKeyboardFocus();
			if (kbFocus) {
				SDL_SysWMinfo wmInfo;
				SDL_VERSION(&wmInfo.version);
				if (SDL_GetWindowWMInfo(kbFocus, &wmInfo)) {
					#if defined(SDL_VIDEO_DRIVER_WAYLAND)
					if (wmInfo.subsystem == SDL_SYSWM_WAYLAND) {
						VblankSource::SetWaylandSurface(
							wmInfo.info.wl.surface,
							wmInfo.info.wl.display);
					}
					#endif
				}
			}
		}
#endif

		double frameRate = (double)TICKS_PER_SECOND_10NS / (double)UPDATE_PERIOD_10NS;

		FramePredictor::InitResult ir = FramePredictor::Init(frameRate);
		if (!ir.ok) {
			printf("[FramePredictor] Init FAILED: %s\n", ir.error ? ir.error : "(unknown)");
			return;
		}

		printf("[FramePredictor] initialized:\n");
		printf("  anchor       : %lld (10ns)\n", (long long)ir.anchor10ns);
		printf("  refresh rate : %.4f Hz\n", ir.refreshRateHz);
		printf("  frame period : %lld (10ns) = %.3f us\n",
			(long long)ir.framePeriod10ns, (double)ir.framePeriod10ns / 100.0);

		g_predictor.Configure(ir);

		useFramePredictor = true;
		printf("[FramePredictor] active - Update() will use predictor path\n");
	}

	int SDLApplication::Exec()
	{
		Init();

#ifdef EMSCRIPTEN
		emscripten_cancel_main_loop();
#endif

        int sleeptimeclocktimer = 0;

		InitFramePredictor();

		while (active)
		{
            if (sleeptimeclocktimer > 100) {
                sleeptimeclocktimer = 0;
                calculateMinimalSleepTime();
            }
            sleeptimeclocktimer++;
            if (UPDATE_PERIOD_10NS == 0) Update_Vsync();
            else Update ();
		}

		return 0;
	}

	int SDLApplication::Quit()
	{
		if (alreadyQuit)
			return 0;

		// Stop the main loop first
		active = false;
		
		// Give the main loop time to exit gracefully
		std::this_thread::sleep_for(std::chrono::milliseconds(10));

		applicationEvent.type = EXIT;
		ApplicationEvent::Dispatch(&applicationEvent);

		// Process remaining events but don't process any new DWM queries
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			// Only process non-timing related events
			if (event.type != SDL_WINDOWEVENT || 
				(event.type == SDL_WINDOWEVENT && event.window.event != SDL_WINDOWEVENT_CLOSE)) {
				HandleEvent(&event);
			}
		}

		// Clean up SDL subsystems in reverse order
		SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
		SDL_QuitSubSystem(SDL_INIT_JOYSTICK);
#if defined(LIME_MOJOAL) || defined(LIME_OPENALSOFT)
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
#endif
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
		SDL_Quit();
		
		alreadyQuit = true;

		return 0;
	}

	void SDLApplication::RegisterWindow(SDLWindow *window)
	{
#ifdef IPHONE
		SDL_Window* kbFocus = SDL_GetKeyboardFocus();
		if (!kbFocus) return;
		uint32_t focusedWindowID = SDL_GetWindowID(kbFocus);
		SDLWindow* focusedWindow = SDLWindow::windows[focusedWindowID];
		SDL_iPhoneSetAnimationCallback(focusedWindow->sdlWindow, 1, Update, NULL);
#endif
	}

	void SDLApplication::SetFrameRate(double frameRate)
	{
		if (frameRate > 0)
		{
			UPDATE_PERIOD_10NS = TICKS_PER_SECOND_10NS / frameRate;
			RENDER_PERIOD_10NS = TICKS_PER_SECOND_10NS / 60.0;
		}
		else
		{
			UPDATE_PERIOD_10NS = 0;
			RENDER_PERIOD_10NS = 0;
		}
		VsyncCounter::NotifyFrameRateChange();
	}

	void SDLApplication::SetRenderFrameRate(double renderFrameRate)
	{
		if (renderFrameRate > 60)
		{
			RENDER_PERIOD_10NS = TICKS_PER_SECOND_10NS / renderFrameRate;
		}
		else if (renderFrameRate == 0)
		{
			RENDER_PERIOD_10NS = 0.0;
		}
		else
		{
			RENDER_PERIOD_10NS = TICKS_PER_SECOND_10NS / 60.0;
		}
		VsyncCounter::NotifyFrameRateChange();
	}

	int64_t startTimestamp10ns = 0;

	void SDLApplication::Init()
	{
		active = true;
		VsyncCounter::NotifyFrameRateChange();

#if HX_WINDOWS
		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif

	}

	bool SDLApplication::IsWindowValid()
	{
		SDL_Window* kbFocus = SDL_GetKeyboardFocus();
		if (!kbFocus) return false;
		
		uint32_t focusedWindowID = SDL_GetWindowID(kbFocus);
		auto it = SDLWindow::windows.find(focusedWindowID);
		
		if (it != SDLWindow::windows.end() && it->second && it->second->sdlWindow) {
			// Additional check: verify the window hasn't been marked for destruction
			return true;
		}
		
		return false;
	}

	void SDLApplication::PollInputs()
	{
		SDL_Event event;
		while (SDL_PollEvent(&event))
		{
			bool isInputEvent = false;
			switch (event.type)
			{
			case SDL_KEYDOWN:
			case SDL_KEYUP:
			case SDL_MOUSEMOTION:
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
			case SDL_MOUSEWHEEL:
			case SDL_FINGERMOTION:
			case SDL_FINGERDOWN:
			case SDL_FINGERUP:
			case SDL_TEXTINPUT:
			case SDL_TEXTEDITING:
			case SDL_JOYBALLMOTION:
			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
			case SDL_JOYHATMOTION:
			case SDL_JOYDEVICEADDED:
			case SDL_JOYDEVICEREMOVED:
			case SDL_JOYAXISMOTION:
			case SDL_CONTROLLERAXISMOTION:
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP:
			case SDL_CONTROLLERDEVICEADDED:
			case SDL_CONTROLLERDEVICEREMOVED:
			case SDL_CLIPBOARDUPDATE:
				isInputEvent = true;
				break;
			}
			if (isInputEvent)
				HandleInputEvent(&event);
			else
				HandleEvent(&event);
		}
	}

    static bool schedulerUnthrottled = false;
    static double currentUpdate = 0.0;
    static double nextUpdate = 0.0;
    static double framePeriod = 0.0;

    static bool IsFrameDueLocal(double now)
    {
        if (schedulerUnthrottled)
            return true;
        return (now >= nextUpdate);
    }

    static void AdvanceNextUpdateLocal()
    {
        if (schedulerUnthrottled || framePeriod <= 0.0)
        {
            nextUpdate = currentUpdate;
            return;
        }

        nextUpdate += framePeriod;
        while (nextUpdate <= currentUpdate)
        {
            nextUpdate += framePeriod;
        }
    }

    // --- Static anchors for precise 10ns pacing using frame units ---
    static int64_t startAnchor10ns = 0;
    static int64_t nextUpdateFrame = 0;
    static int64_t nextRenderFrame = 0;
    static double lastUpdate = 0.0;
    static bool firstFrame = true;

	void RenderPresent() {
		SDL_Window* kbFocus = SDL_GetKeyboardFocus();
		if (!kbFocus) {
			minimalSleepCalcBase10ns = minimalSleepCalc10ns = 100000;
		}
		uint32_t focusedWindowID = SDL_GetWindowID(kbFocus);
		SDLWindow* focusedWindow = SDLWindow::windows[focusedWindowID];
		
		if (!focusedWindow || !focusedWindow->sdlWindow) return;

		SDL_RenderPresent(SDL_GetRenderer(focusedWindow->sdlWindow));
	}

    bool SDLApplication::Update()
    {
        if (!active)
            return false;

        int64_t now10ns = getTime10ns();

        subLoopTickEvent.timestamp = now10ns;
        SubLoopTickEvent::Dispatch(&subLoopTickEvent);

        PollInputs();

		// ================================================================
		// FramePredictor path - when active, dispatch UPDATE+RENDER together
		// at the monitor's actual vblank with the exact measured frame time.
		// ================================================================
		if (useFramePredictor)
		{
			int64_t frameTime10ns = g_predictor.PredictorRun();
			now10ns = getTime10ns();

			if (frameTime10ns > 0)
			{
				int64_t frameTimeNow = getTime10ns();

				// --- Dispatch UPDATE with exact frame time ---
				applicationEvent.type = UPDATE;
				applicationEvent.deltaTime = frameTime10ns;
				ApplicationEvent::Dispatch(&applicationEvent);

				// --- Dispatch RENDER with exact frame time ---
				renderEvent.type = RENDER;
				RenderEvent::Dispatch(&renderEvent);

				// --- This is actually what keeps it smooth ---
				// wow. i found the perfect thing in mind. you should call this strictly after renderevent.
				// i love my life
				if (getTime10ns() - frameTimeNow > frameTime10ns + 100000)
					RenderPresent();
			}
		}

        return active;
    }

	bool SDLApplication::Update_Vsync()
	{
		static int64_t nextUpdateTime10ns = 0;
		static bool firstFrame = true;

		int64_t now10ns = 0;

		// FIX: Restore minimalSleepCalc10ns to the base value at the top of every
		// frame. The vblank-approach shrink below is intentionally transient - it
		// applies only for the last few iterations before the predicted vblank, then
		// resets here so a bad prediction or a missed vblank can never leave
		// minimalSleepCalc10ns permanently at its 50us floor.
		if (minimalSleepCalcBase10ns > 0)
			minimalSleepCalc10ns = minimalSleepCalcBase10ns;

		now10ns = getTime10ns();

		if (firstFrame)
		{
			startTimestamp10ns = now10ns;
			nextUpdateTime10ns = now10ns + UPDATE_PERIOD_10NS;
			firstFrame = false;
		}

		int64_t targetTime = now10ns + minimalSleepCalc10ns;
		coolSleepUntil10ns(targetTime);

		now10ns = getTime10ns();

		subLoopTickEvent.timestamp = now10ns;
		SubLoopTickEvent::Dispatch(&subLoopTickEvent);

		// --- Vsync poll: one call, all platform logic in FramePredictor.h ---
		int winX = 0, winY = 0;
		SDL_Window* kbFocus = SDL_GetKeyboardFocus();
		if (kbFocus) SDL_GetWindowPosition(kbFocus, &winX, &winY);

		auto vr = VsyncCounter::Poll(now10ns, RENDER_PERIOD_10NS, winX, winY);

#ifdef HX_WINDOWS
		// Shrink sleep as we approach the predicted vblank
		int64_t predicted = VsyncCounter::GetPredictedNextVblank10ns();
		if (predicted > 0)
		{
			int64_t timeUntilVBlank = predicted - now10ns;
			if (timeUntilVBlank > 0 && timeUntilVBlank < minimalSleepCalcBase10ns * 2)
			{
				minimalSleepCalc10ns = std::max<int64_t>(timeUntilVBlank / 2, 5000LL);
			}
		}
#endif

		PollInputs();

		if (vr.shouldRender)
		{
			applicationEvent.type = UPDATE;
			applicationEvent.deltaTime = vr.frameTime10ns;
			ApplicationEvent::Dispatch(&applicationEvent);

			renderEvent.type = RENDER;
			RenderEvent::Dispatch(&renderEvent);
		}

		return active;
	}

	Application *CreateApplication()
	{
		return new SDLApplication();
	}

}

#ifdef ANDROID
int SDL_main(int argc, char *argv[]) { return 0; }
#endif