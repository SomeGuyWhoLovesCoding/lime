/**
 * This class is where the main loop goes. For one, windows 10;
 * The said main loop uses:
   - A combination of vsync counter and SDL3 sleep with EMA to create a surreal rhythm game experience!
 * On the other hand, I wanted to create a fun crispy smooth experience for literally everyone who are on windows,
 so that meant doing it in the first place to compensate. How about I make a literal main loop library out of this?
**/

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
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <poll.h>
#include <x86intrin.h>
#include <dlfcn.h>
#endif
#if HX_ANDROID
#include <android/choreographer.h>
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
	static bool uncappedFramerate = false;

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

#if HX_ANDROID
	static AChoreographer *choreographer = nullptr;
	static bool shouldRenderFromCallback = false;

	static void choreographer_callback(long frameTimeNanos, void *data)
	{
		shouldRenderFromCallback = true;
		int64_t frameTime10ns = frameTimeNanos / 10;
		render_timestamp = frameTime10ns - lastRenderTime;
		lastRenderTime = frameTime10ns;
	}
#endif

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

	namespace AsyncKB {
		static constexpr size_t MAX_EVENTS = 512;
		
		// Queue: array of arrays of doubles
		// [0] = scanCode, [1] = state (1.0 = down, 0.0 = up), [2] = timestamp
		static std::array<std::array<double, 3>, MAX_EVENTS> eventQueue;
		static std::atomic<size_t> writeIndex{0};
		static std::atomic<size_t> readIndex{0};
		static std::atomic<size_t> eventCount{0};
		static std::mutex queueMutex;
		
		static std::atomic<bool> running{false};
		static std::thread workerThread;
		
		static double getCurrentTimestamp() {
			return AsyncKeyEvent::Timestamp();
		}
		
		static void addEvent(double scanCode, double state, double timestamp) {
			std::lock_guard<std::mutex> lock(queueMutex);
			size_t currentWrite = writeIndex.load(std::memory_order_acquire);
			eventQueue[currentWrite][0] = scanCode;
			eventQueue[currentWrite][1] = state;
			eventQueue[currentWrite][2] = timestamp;
			writeIndex.store((currentWrite + 1) % MAX_EVENTS, std::memory_order_release);
			
			size_t count = eventCount.load(std::memory_order_acquire);
			if (count < MAX_EVENTS) {
				eventCount.store(count + 1, std::memory_order_release);
			} else {
				size_t currentRead = readIndex.load(std::memory_order_acquire);
				readIndex.store((currentRead + 1) % MAX_EVENTS, std::memory_order_release);
			}
		}
		
	#ifdef HX_WINDOWS
		// Windows implementation
		static HHOOK keyboardHook = nullptr;
		static HANDLE quitEvent = nullptr;
		static DWORD processId = 0;
		
		static int windowsToLimeKeyCode(int winKeyCode) {
			if (winKeyCode >= 'A' && winKeyCode <= 'Z') return 0x61 + (winKeyCode - 'A');
			if (winKeyCode >= '0' && winKeyCode <= '9') return winKeyCode;
			
			switch (winKeyCode) {
				case VK_BACK: return 0x08; case VK_TAB: return 0x09; case VK_RETURN: return 0x0D;
				case VK_ESCAPE: return 0x1B; case VK_SPACE: return 0x20; case VK_DELETE: return 0x7F;
				case VK_INSERT: return 0x40000049; case VK_HOME: return 0x4000004A; case VK_END: return 0x4000004D;
				case VK_PRIOR: return 0x4000004B; case VK_NEXT: return 0x4000004E; case VK_UP: return 0x40000052;
				case VK_DOWN: return 0x40000051; case VK_LEFT: return 0x40000050; case VK_RIGHT: return 0x4000004F;
				case VK_LCONTROL: return 0x400000E0; case VK_RCONTROL: return 0x400000E4; case VK_LSHIFT: return 0x400000E1;
				case VK_RSHIFT: return 0x400000E5; case VK_LMENU: return 0x400000E2; case VK_RMENU: return 0x400000E6;
				case VK_LWIN: return 0x400000E3; case VK_RWIN: return 0x400000E7; case VK_CAPITAL: return 0x40000039;
				case VK_NUMLOCK: return 0x40000053; case VK_SCROLL: return 0x40000047; case VK_F1: return 0x4000003A;
				case VK_F2: return 0x4000003B; case VK_F3: return 0x4000003C; case VK_F4: return 0x4000003D;
				case VK_F5: return 0x4000003E; case VK_F6: return 0x4000003F; case VK_F7: return 0x40000040;
				case VK_F8: return 0x40000041; case VK_F9: return 0x40000042; case VK_F10: return 0x40000043;
				case VK_F11: return 0x40000044; case VK_F12: return 0x40000045; case VK_OEM_MINUS: return 0x2D;
				case VK_OEM_PLUS: return 0x3D; case VK_OEM_4: return 0x5B; case VK_OEM_6: return 0x5D;
				case VK_OEM_5: return 0x5C; case VK_OEM_1: return 0x3B; case VK_OEM_7: return 0x27;
				case VK_OEM_3: return 0x60; case VK_OEM_COMMA: return 0x2C; case VK_OEM_PERIOD: return 0x2E;
				case VK_OEM_2: return 0x2F; default: return 0x00;
			}
		}
		
		static LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
			if (nCode >= 0) {
				HWND foreground = GetForegroundWindow();
				DWORD pid = 0;
				if (foreground) GetWindowThreadProcessId(foreground, &pid);
				bool isFocused = (pid == processId);

				if (isFocused) {
					KBDLLHOOKSTRUCT* kb = (KBDLLHOOKSTRUCT*)lParam;
					
					if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN || 
						wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
						
						double scanCode = (double)windowsToLimeKeyCode(kb->vkCode);
						double state = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) ? 1.0 : 0.0;
						double timestamp = getCurrentTimestamp();
						addEvent(scanCode, state, timestamp);
					}
				}
			}
			return CallNextHookEx(NULL, nCode, wParam, lParam);
		}
		
		static void workerFunction() {
			quitEvent = CreateEvent(NULL, TRUE, FALSE, NULL);
			processId = GetCurrentProcessId();
			
			keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
			if (!keyboardHook) {
				CloseHandle(quitEvent);
				return;
			}
			
			MSG msg;
			HANDLE handles[] = { quitEvent };
			while (running) {
				DWORD result = MsgWaitForMultipleObjects(1, handles, FALSE, 100, QS_ALLINPUT);
				if (result == WAIT_OBJECT_0) break;
				else if (result == WAIT_OBJECT_0 + 1) {
					while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
						TranslateMessage(&msg);
						DispatchMessage(&msg);
					}
				}
			}
			UnhookWindowsHookEx(keyboardHook);
			CloseHandle(quitEvent);
		}
	#elif defined(HX_LINUX)
		// Linux implementation using SDL_GetKeyboardState with focus check
		static std::array<int, SDL_NUM_SCANCODES> lastState;
		static std::once_flag initFlag;
		
		static int linuxToLimeKeycode(SDL_Scancode scancode) {
			// Convert SDL_Scancode to Lime keycodes (matching Windows virtual keycodes)
			switch (scancode) {
				case SDL_SCANCODE_A: return 0x61;
				case SDL_SCANCODE_B: return 0x62;
				case SDL_SCANCODE_C: return 0x63;
				case SDL_SCANCODE_D: return 0x64;
				case SDL_SCANCODE_E: return 0x65;
				case SDL_SCANCODE_F: return 0x66;
				case SDL_SCANCODE_G: return 0x67;
				case SDL_SCANCODE_H: return 0x68;
				case SDL_SCANCODE_I: return 0x69;
				case SDL_SCANCODE_J: return 0x6A;
				case SDL_SCANCODE_K: return 0x6B;
				case SDL_SCANCODE_L: return 0x6C;
				case SDL_SCANCODE_M: return 0x6D;
				case SDL_SCANCODE_N: return 0x6E;
				case SDL_SCANCODE_O: return 0x6F;
				case SDL_SCANCODE_P: return 0x70;
				case SDL_SCANCODE_Q: return 0x71;
				case SDL_SCANCODE_R: return 0x72;
				case SDL_SCANCODE_S: return 0x73;
				case SDL_SCANCODE_T: return 0x74;
				case SDL_SCANCODE_U: return 0x75;
				case SDL_SCANCODE_V: return 0x76;
				case SDL_SCANCODE_W: return 0x77;
				case SDL_SCANCODE_X: return 0x78;
				case SDL_SCANCODE_Y: return 0x79;
				case SDL_SCANCODE_Z: return 0x7A;
				
				case SDL_SCANCODE_0: return 0x30;
				case SDL_SCANCODE_1: return 0x31;
				case SDL_SCANCODE_2: return 0x32;
				case SDL_SCANCODE_3: return 0x33;
				case SDL_SCANCODE_4: return 0x34;
				case SDL_SCANCODE_5: return 0x35;
				case SDL_SCANCODE_6: return 0x36;
				case SDL_SCANCODE_7: return 0x37;
				case SDL_SCANCODE_8: return 0x38;
				case SDL_SCANCODE_9: return 0x39;
				
				case SDL_SCANCODE_BACKSPACE: return 0x08;
				case SDL_SCANCODE_TAB: return 0x09;
				case SDL_SCANCODE_RETURN: return 0x0D;
				case SDL_SCANCODE_ESCAPE: return 0x1B;
				case SDL_SCANCODE_SPACE: return 0x20;
				case SDL_SCANCODE_DELETE: return 0x7F;
				case SDL_SCANCODE_INSERT: return 0x40000049;
				case SDL_SCANCODE_HOME: return 0x4000004A;
				case SDL_SCANCODE_END: return 0x4000004D;
				case SDL_SCANCODE_PAGEUP: return 0x4000004B;
				case SDL_SCANCODE_PAGEDOWN: return 0x4000004E;
				case SDL_SCANCODE_UP: return 0x40000052;
				case SDL_SCANCODE_DOWN: return 0x40000051;
				case SDL_SCANCODE_LEFT: return 0x40000050;
				case SDL_SCANCODE_RIGHT: return 0x4000004F;
				
				case SDL_SCANCODE_LCTRL: return 0x400000E0;
				case SDL_SCANCODE_RCTRL: return 0x400000E4;
				case SDL_SCANCODE_LSHIFT: return 0x400000E1;
				case SDL_SCANCODE_RSHIFT: return 0x400000E5;
				case SDL_SCANCODE_LALT: return 0x400000E2;
				case SDL_SCANCODE_RALT: return 0x400000E6;
				case SDL_SCANCODE_LGUI: return 0x400000E3;
				case SDL_SCANCODE_RGUI: return 0x400000E7;
				case SDL_SCANCODE_CAPSLOCK: return 0x40000039;
				case SDL_SCANCODE_NUMLOCKCLEAR: return 0x40000053;
				case SDL_SCANCODE_SCROLLLOCK: return 0x40000047;
				
				case SDL_SCANCODE_F1: return 0x4000003A;
				case SDL_SCANCODE_F2: return 0x4000003B;
				case SDL_SCANCODE_F3: return 0x4000003C;
				case SDL_SCANCODE_F4: return 0x4000003D;
				case SDL_SCANCODE_F5: return 0x4000003E;
				case SDL_SCANCODE_F6: return 0x4000003F;
				case SDL_SCANCODE_F7: return 0x40000040;
				case SDL_SCANCODE_F8: return 0x40000041;
				case SDL_SCANCODE_F9: return 0x40000042;
				case SDL_SCANCODE_F10: return 0x40000043;
				case SDL_SCANCODE_F11: return 0x40000044;
				case SDL_SCANCODE_F12: return 0x40000045;
				
				case SDL_SCANCODE_MINUS: return 0x2D;
				case SDL_SCANCODE_EQUALS: return 0x3D;
				case SDL_SCANCODE_LEFTBRACKET: return 0x5B;
				case SDL_SCANCODE_RIGHTBRACKET: return 0x5D;
				case SDL_SCANCODE_BACKSLASH: return 0x5C;
				case SDL_SCANCODE_SEMICOLON: return 0x3B;
				case SDL_SCANCODE_APOSTROPHE: return 0x27;
				case SDL_SCANCODE_GRAVE: return 0x60;
				case SDL_SCANCODE_COMMA: return 0x2C;
				case SDL_SCANCODE_PERIOD: return 0x2E;
				case SDL_SCANCODE_SLASH: return 0x2F;
				
				default: return 0x00;
			}
		}
		
		static void initLastState() {
			lastState.fill(0);
		}
		
		static void workerFunction() {
			std::call_once(initFlag, initLastState);
			
			while (running) {
				// Check if our window has focus
				SDL_Window* focusedWindow = SDL_GetKeyboardFocus();
				bool hasFocus = (focusedWindow != nullptr);
				
				if (hasFocus) {
					const Uint8* keyboardState = SDL_GetKeyboardState(nullptr);
					
					// Check all possible key scancodes
					for (int i = 0; i < SDL_NUM_SCANCODES; i++) {
						Uint8 currentState = keyboardState[i];
						if (currentState != lastState[i]) {
							double scanCode = (double)linuxToLimeKeycode((SDL_Scancode)i);
							double state = (double)(currentState ? 1 : 0);
							double timestamp = getCurrentTimestamp();
							addEvent(scanCode, state, timestamp);
							lastState[i] = currentState;
						}
					}
					
					// Poll aggressively when focused (1ms sleep)
					coolSleepUntil10ns(getTime10ns() + (TICKS_PER_SECOND_10NS * 0.001));
				} else {
					// When not focused, conserve CPU
					coolSleepUntil10ns(getTime10ns() + (TICKS_PER_SECOND_10NS * 0.01));
				}
			}
		}
	#else
		// Empty implementation for other platforms
		static void workerFunction() {
			while (running) {
				std::this_thread::sleep_for(std::chrono::milliseconds(10));
			}
		}
	#endif

		static void start() {
			if (running) return;
			running = true;
			workerThread = std::thread(workerFunction);
		}
		
		static void stop() {
			if (!running) return;
			running = false;
			if (workerThread.joinable()) {
				workerThread.join();
			}
		}
		
		bool hasEvent() {
			return eventCount.load(std::memory_order_acquire) > 0;
		}
		
		bool getEvent(double& scanCode, double& state, double& timestamp) {
			// Quick check without lock first
			if (eventCount.load(std::memory_order_acquire) == 0) return false;
			
			// Minimal critical section
			{
				std::lock_guard<std::mutex> lock(queueMutex);
				if (eventCount.load(std::memory_order_acquire) == 0) return false;
				
				size_t currentRead = readIndex.load(std::memory_order_acquire);
				scanCode = eventQueue[currentRead][0];
				state = eventQueue[currentRead][1];
				timestamp = eventQueue[currentRead][2];
				readIndex.store((currentRead + 1) % MAX_EVENTS, std::memory_order_release);
				eventCount.fetch_sub(1, std::memory_order_release);
			}
			return true;
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

	int SDLApplication::Exec()
	{
		Init();

#ifdef EMSCRIPTEN
		emscripten_cancel_main_loop();
#endif

        int sleeptimeclocktimer = 0;

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

		AsyncKB::stop();

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

	/*static double GetGlobalKeyboardTimestampComparison() {
		return AysncKeyboard::getCurrentTimestamp();
	}*/

	void SDLApplication::SetUncappedFrameRate(bool value)
	{
		printf("Setting uncapped framerate to %i\n", (int)value);
		uncappedFramerate = value;

		if (value) {
			render_timestamp = 0;
		}

		printf("Uncapped framerate set to %i\n", (int)uncappedFramerate);
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
	}

	static int64_t lag = 0;
	int64_t startTimestamp10ns = 0;

	void SDLApplication::Init()
	{
		active = true;
		lag = getTime10ns();

#if HX_WINDOWS
		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
#endif
		AsyncKB::start();

#ifdef HX_ANDROID
		if (!choreographer)
		{
			choreographer = AChoreographer_getInstance();
			if (choreographer)
			{
				AChoreographer_postFrameCallback(choreographer,
												 choreographer_callback,
												 nullptr);
			}
		}
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
		// Process async keyboard events first
		double scanCode, state, timestamp;
		while (AsyncKB::hasEvent()) {
			if (AsyncKB::getEvent(scanCode, state, timestamp)) {
					
				//printf("Keycode: %.3f, state: %.0f, timestamp: %.9f\n", scanCode, state, timestamp);
				asyncKeyEvent.keyCode = (int)scanCode;
				asyncKeyEvent.state = (int)state;
				asyncKeyEvent.timestamp = timestamp;
				AsyncKeyEvent::Dispatch(&asyncKeyEvent);
				
			}
		}

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

	#if defined(HX_LINUX)

	static uint64_t VblankSequence = 0;

	// Minimal Wayland definitions to avoid compile-time dependency on libwayland-dev
	struct wl_surface;
	struct wl_callback;

	struct wl_callback_listener {
		void (*done)(void *data, struct wl_callback *callback, uint32_t time);
	};

	// Function pointer types
	typedef struct wl_callback* (*wl_surface_frame_t)(struct wl_surface *surface);
	typedef int (*wl_callback_add_listener_t)(struct wl_callback *callback, const struct wl_callback_listener *listener, void *data);
	typedef void (*wl_callback_destroy_t)(struct wl_callback *callback);

	// Global function pointers
	static void* wl_lib_handle = nullptr;
	static wl_surface_frame_t p_wl_surface_frame = nullptr;
	static wl_callback_add_listener_t p_wl_callback_add_listener = nullptr;
	static wl_callback_destroy_t p_wl_callback_destroy = nullptr;

	static bool waylandLoaded = false;

	void loadWaylandDynamically() {
		if (waylandLoaded) return;
		waylandLoaded = true;

		wl_lib_handle = dlopen("libwayland-client.so.0", RTLD_LAZY);
		if (wl_lib_handle) {
			p_wl_surface_frame = (wl_surface_frame_t)dlsym(wl_lib_handle, "wl_surface_frame");
			p_wl_callback_add_listener = (wl_callback_add_listener_t)dlsym(wl_lib_handle, "wl_callback_add_listener");
			p_wl_callback_destroy = (wl_callback_destroy_t)dlsym(wl_lib_handle, "wl_callback_destroy");
			
			if (!p_wl_surface_frame || !p_wl_callback_add_listener || !p_wl_callback_destroy) {
				dlclose(wl_lib_handle);
				wl_lib_handle = nullptr;
			}
		}
	}

	    // --- Wayland Vsync Support ---
    static bool waylandVsyncFired = false;
    static int64_t waylandLastCallbackTime10ns = 0;
    static struct wl_surface* cachedWaylandSurface = nullptr;
    static struct wl_callback* cachedWaylandCallback = nullptr;

    // 1. Define the struct FIRST, but leave the function pointer null
    static struct wl_callback_listener waylandFrameListener = { nullptr };

    // 2. Define the function SECOND (it can now see the struct above it)
    static void waylandFrameCallbackHandler(void* data, struct wl_callback* callback, uint32_t time) {
        waylandVsyncFired = true;
        
        int64_t now = getTime10ns();
        if (waylandLastCallbackTime10ns > 0) {
            render_timestamp = now - waylandLastCallbackTime10ns;
        } else {
            render_timestamp = RENDER_PERIOD_10NS;
        }
        waylandLastCallbackTime10ns = now;
        lastRenderTime = now; // Keep consistent with DRM logic
        
        if (p_wl_callback_destroy) p_wl_callback_destroy(callback); // One-shot callback, destroy it
        
        // We can just use the global cachedWaylandSurface here directly
        if (p_wl_surface_frame && p_wl_callback_add_listener && cachedWaylandSurface) {
            cachedWaylandCallback = p_wl_surface_frame(cachedWaylandSurface);
            p_wl_callback_add_listener(cachedWaylandCallback, &waylandFrameListener, cachedWaylandSurface);
        }
    }

    void initWaylandVsync(SDL_Window* sdlWindow) {
        loadWaylandDynamically();
        if (!p_wl_surface_frame || !p_wl_callback_add_listener) return;

        // 3. Wire them together HERE at runtime (no forward declarations needed!)
        waylandFrameListener.done = waylandFrameCallbackHandler;

        // FIX 2: Guard the SDL2 Wayland info access. 
        // If the user's SDL2 was compiled without Wayland support, this safely skips.
    #if defined(SDL_VIDEO_DRIVER_WAYLAND)
        SDL_SysWMinfo wmInfo;
        SDL_VERSION(&wmInfo.version);
        if (SDL_GetWindowWMInfo(sdlWindow, &wmInfo)) {
            if (wmInfo.subsystem == SDL_SYSWM_WAYLAND) {
                cachedWaylandSurface = wmInfo.info.wl.surface;
                if (cachedWaylandSurface && !cachedWaylandCallback) {
                    cachedWaylandCallback = p_wl_surface_frame(cachedWaylandSurface);
                    p_wl_callback_add_listener(cachedWaylandCallback, &waylandFrameListener, cachedWaylandSurface);
                }
            }
        }
    #endif
    }

	// --- DRM Vsync Support (Extracted) ---
	static int drmFd = -1;
	static uint32_t drmCrtcId = 0;
	static uint64_t lastVBlankSeq = 0;
	static int lastWindowX = -1, lastWindowY = -1;
	static bool drmInitializedLocal = false;

	void updateDrmVsync(SDL_Window* sdlWindow, int64_t now10ns, bool& shouldRender) {
		int windowX = 0, windowY = 0;
		SDL_GetWindowPosition(sdlWindow, &windowX, &windowY);

		// Automatically re-initialize if the window moved to a different monitor
		if (!drmInitializedLocal || windowX != lastWindowX || windowY != lastWindowY) {
			lastWindowX = windowX;
			lastWindowY = windowY;

			if (drmFd >= 0) {
				close(drmFd);
				drmFd = -1;
			}

			drmDevicePtr devices[16];
			int deviceCount = drmGetDevices(devices, 16);

			if (deviceCount > 0) {
				bool foundDevice = false;

				for (int i = 0; i < deviceCount && !foundDevice; i++) {
					drmDevicePtr dev = devices[i];
					if (!dev->nodes[DRM_NODE_PRIMARY]) continue;

					int fd = open(dev->nodes[DRM_NODE_PRIMARY], O_RDWR | O_CLOEXEC);
					if (fd < 0) continue;

					drmModeResPtr res = drmModeGetResources(fd);
					if (!res) {
						close(fd);
						continue;
					}

					for (int c = 0; c < res->count_crtcs && !foundDevice; c++) {
						uint32_t crtcId = res->crtcs[c];
						drmModeCrtcPtr crtc = drmModeGetCrtc(fd, crtcId);

						if (!crtc) continue;

						if (crtc->mode_valid && crtc->width > 0 && crtc->height > 0) {
							if (windowX >= crtc->x && windowX < crtc->x + crtc->width &&
								windowY >= crtc->y && windowY < crtc->y + crtc->height) {
								
								drmFd = fd;
								drmCrtcId = crtcId;
								drmInitializedLocal = true;
								foundDevice = true;

								drmVBlank primeVbl;
								memset(&primeVbl, 0, sizeof(primeVbl));
								primeVbl.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT);
	#if defined(DRM_VBLANK_HIGH_CRTC_MASK)
								primeVbl.request.type = (drmVBlankSeqType)(primeVbl.request.type | (crtcId << DRM_VBLANK_HIGH_CRTC_SHIFT));
	#endif
								primeVbl.request.sequence = 1;
								primeVbl.request.signal = (unsigned long)&VblankSequence;
								drmWaitVBlank(fd, &primeVbl);
							}
						}
						drmModeFreeCrtc(crtc);
					}
					drmModeFreeResources(res);
					if (!foundDevice) close(fd);
				}
				drmFreeDevices(devices, deviceCount);
			}
		}

		if (drmFd >= 0 && drmCrtcId != 0) {
			struct pollfd pfd;
			pfd.fd = drmFd;
			pfd.events = POLLIN;
			pfd.revents = 0;

			int pollResult = poll(&pfd, 1, 0);

			if (pollResult > 0 && (pfd.revents & POLLIN)) {
				drmEventContext evctx;
				memset(&evctx, 0, sizeof(evctx));
				evctx.version = DRM_EVENT_CONTEXT_VERSION;

				evctx.vblank_handler = [](int fd, unsigned int sequence,
										unsigned int tv_sec, unsigned int tv_usec,
										void *user_data) {
					uint64_t *seqPtr = (uint64_t *)user_data;
					*seqPtr = sequence;
				};

				drmHandleEvent(drmFd, &evctx);

				if (VblankSequence != lastVBlankSeq) {
					shouldRender = true;
					render_timestamp = now10ns - lastRenderTime;
					lastRenderTime = now10ns;
					lastVBlankSeq = VblankSequence;
				}
			}

			if (shouldRender) {
				drmVBlank nextVbl;
				memset(&nextVbl, 0, sizeof(nextVbl));
				nextVbl.request.type = (drmVBlankSeqType)(DRM_VBLANK_RELATIVE | DRM_VBLANK_EVENT);
	#if defined(DRM_VBLANK_HIGH_CRTC_MASK)
				nextVbl.request.type = (drmVBlankSeqType)(nextVbl.request.type | (crtcId << DRM_VBLANK_HIGH_CRTC_SHIFT));
	#endif
				nextVbl.request.sequence = 1;
				nextVbl.request.signal = (unsigned long)&VblankSequence;
				drmWaitVBlank(drmFd, &nextVbl);
			}
		}
	}

	// --- Unified Linux Vsync Wrapper ---
	void handleLinuxVsync(SDL_Window* sdlWindow, int64_t now10ns, int64_t lag, int64_t& nextRenderTime10ns, bool& shouldRender) {
		shouldRender = false;

		// 1. Try Wayland first
		if (!cachedWaylandSurface) {
			initWaylandVsync(sdlWindow);
		}

		if (cachedWaylandSurface) {
			if (waylandVsyncFired) {
				shouldRender = true;
				waylandVsyncFired = false;
				nextRenderTime10ns = now10ns + RENDER_PERIOD_10NS;
			}
		} 
		// 2. Fallback to DRM if not on Wayland (e.g., X11)
		else {
			updateDrmVsync(sdlWindow, now10ns, shouldRender);
		}

		// 3. Universal timer-based fallback
		if (!shouldRender) {
			shouldRender = (now10ns >= (nextRenderTime10ns - std::max<int64_t>(getTime10ns() - lag, RENDER_PERIOD_10NS / 2)));
			if (shouldRender) {
				render_timestamp = now10ns - lastRenderTime;
				lastRenderTime = now10ns;
				nextRenderTime10ns += RENDER_PERIOD_10NS;
			}
		}
	}

	#endif

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

    bool SDLApplication::Update()
    {
        if (!active)
            return false;

        int64_t now10ns = getTime10ns();

        // Initialize absolute anchors and frame counters on the very first frame
        if (firstFrame)
        {
            startAnchor10ns = now10ns;
            nextUpdateFrame = 1;
            nextRenderFrame = 1;
            lastUpdate = (double)now10ns / 100000.0;
            firstFrame = false;
        }

        // Reset chunked sleep base at the start of every 1ms iteration
        if (minimalSleepCalcBase10ns > 0)
            minimalSleepCalc10ns = minimalSleepCalcBase10ns;

        // --- PREDICT TARGETS (Using your exact grid math to avoid truncation) ---
        // We multiply by the full second first so it stays perfectly synced with the division logic below
        int64_t nextUpdateTarget10ns = startAnchor10ns + ((nextUpdateFrame * TICKS_PER_SECOND_10NS) / 120LL);
        int64_t nextRenderTarget10ns = startAnchor10ns + ((nextRenderFrame * TICKS_PER_SECOND_10NS) / 60LL);

        // Find the soonest upcoming boundary
        int64_t nextBoundary10ns = nextUpdateTarget10ns;
        if (RENDER_PERIOD_10NS > 0 && nextRenderTarget10ns < nextBoundary10ns) {
            nextBoundary10ns = nextRenderTarget10ns;
        }

        // --- 1. Sleep in a SINGLE chunk and return to Exec() ---
        // This is the core reason Update_Vsync never freezes on lag.
        // By sleeping 1ms and returning, the main loop stays highly responsive.
        int64_t targetTime = now10ns + minimalSleepCalc10ns;
        
        // JITTER FIX: If our standard 1ms chunk is going to overshoot the upcoming 
        // frame boundary, shrink this specific chunk to hit the boundary exactly.
        // (If we are lagging, the boundary is in the past, so this safely ignores it)
        if (nextBoundary10ns > now10ns && targetTime > nextBoundary10ns) {
            targetTime = nextBoundary10ns;
        }
        
        coolSleepUntil10ns(targetTime);

        now10ns = getTime10ns();

        // --- 2. SubLoopTick (Fires every 1ms chunk, preventing any freeze) ---
        subLoopTickEvent.timestamp = now10ns;
        SubLoopTickEvent::Dispatch(&subLoopTickEvent);

        // --- 3. Poll Inputs ---
        PollInputs();

        // --- 4. Round down to frame time units (Your phase-lock logic) ---
        int64_t to_units_update = (now10ns - startAnchor10ns) / UPDATE_PERIOD_10NS;
        int64_t to_units_render = (now10ns - startAnchor10ns) / RENDER_PERIOD_10NS;

        // --- 5. Update Condition ---
        if (to_units_update >= nextUpdateFrame)
        {
            applicationEvent.type = UPDATE;
            applicationEvent.deltaTime = (int64_t)((now10ns / 100000.0 - lastUpdate) * 100000.0);
            if (applicationEvent.deltaTime < 0) applicationEvent.deltaTime = 0;
            lastUpdate = now10ns / 100000.0;

            ApplicationEvent::Dispatch(&applicationEvent);

            // Advance update frame unit
            nextUpdateFrame++;

            // If a lag spike caused us to miss multiple update frames, 
            // skip them instantly to prevent a spiral-of-death freeze.
            while (to_units_update >= nextUpdateFrame) {
                nextUpdateFrame++;
            }
        }

        // --- 6. Render Condition (Operates completely separate from Update) ---
        if (RENDER_PERIOD_10NS > 0 && to_units_render >= nextRenderFrame)
        {
            renderEvent.type = RENDER;
            RenderEvent::Dispatch(&renderEvent);

            // Advance render frame unit
            nextRenderFrame++;

            // Skip missed render frames
            while (to_units_render >= nextRenderFrame) {
                nextRenderFrame++;
            }
        }

        return active;
    }

	bool SDLApplication::Update_Vsync()
	{
		static int64_t nextUpdateTime10ns = 0;
		static int64_t nextRenderTime10ns = 0;
		static int64_t lastRenderTime = getTime10ns();
		static int64_t renderCounter = 0;
		static bool firstFrame = true;
		static unsigned int lastVBlankCounter = 0;

		int64_t now10ns = 0;

		if (uncappedFramerate) {
			PollInputs();

			now10ns = getTime10ns();

			subLoopTickEvent.timestamp = now10ns;
			SubLoopTickEvent::Dispatch(&subLoopTickEvent);

			now10ns = getTime10ns();

			// Only tick game logic at UPDATE_PERIOD_10NS intervals,
			// same cadence as the capped path — prevents logic running
			// thousands of times per second and causing visual speedup.
			applicationEvent.type = UPDATE;
			applicationEvent.deltaTime = now10ns - lag;
			ApplicationEvent::Dispatch(&applicationEvent);

			lag = now10ns;
			nextUpdateTime10ns = now10ns;

			renderEvent.type = RENDER;
			RenderEvent::Dispatch(&renderEvent);

			lag = getTime10ns();
			return active;
		}

		// FIX: Restore minimalSleepCalc10ns to the base value at the top of every
		// frame. The vblank-approach shrink below is intentionally transient — it
		// applies only for the last few iterations before the predicted vblank, then
		// resets here so a bad prediction or a missed vblank can never leave
		// minimalSleepCalc10ns permanently at its 50µs floor.
		if (minimalSleepCalcBase10ns > 0)
			minimalSleepCalc10ns = minimalSleepCalcBase10ns;

		now10ns = getTime10ns();

		if (firstFrame)
		{
			startTimestamp10ns = now10ns;
			nextUpdateTime10ns = now10ns + UPDATE_PERIOD_10NS;
			nextRenderTime10ns = now10ns + RENDER_PERIOD_10NS;
			firstFrame = false;
		}

		int64_t targetTime = now10ns + minimalSleepCalc10ns;
		coolSleepUntil10ns(targetTime);

		now10ns = getTime10ns();

		subLoopTickEvent.timestamp = now10ns;
		SubLoopTickEvent::Dispatch(&subLoopTickEvent);

		// --- Render scheduling ---
		bool shouldRender = false;
		int64_t timeSinceLastRender = now10ns - lastRenderTime;
		int64_t vsyncThreshold = RENDER_PERIOD_10NS / 4; // 1/4 of vsync interval

#ifdef HX_WINDOWS
	{
		static QPC_TIME lastQpcVBlank = 0;
		static int64_t predictedNextVBlank10ns = 0;

		static DWM_TIMING_INFO timingInfo = {};
		timingInfo.cbSize = sizeof(DWM_TIMING_INFO);

		HRESULT hr = DwmGetCompositionTimingInfo(NULL, &timingInfo);

		if (SUCCEEDED(hr))
		{
			if (lastQpcVBlank == 0 || lastQpcVBlank != timingInfo.qpcVBlank)
			{
				// Only render if enough time has passed since last render
				if (timeSinceLastRender >= vsyncThreshold) {
					shouldRender = true;

					if (lastQpcVBlank != 0)
					{
						int64_t qpcDelta = (int64_t)(timingInfo.qpcVBlank - lastQpcVBlank);
						render_timestamp = (qpcDelta * TICKS_PER_SECOND_10NS) / qpcFrequency.QuadPart;
					}
					else
					{
						render_timestamp = RENDER_PERIOD_10NS;
					}

					lastQpcVBlank = timingInfo.qpcVBlank;
					nextRenderTime10ns = now10ns + RENDER_PERIOD_10NS;

					int64_t vblank10ns = (timingInfo.qpcVBlank * TICKS_PER_SECOND_10NS) / qpcFrequency.QuadPart;
					predictedNextVBlank10ns = vblank10ns + render_timestamp;
				}
			}
			else if (now10ns >= nextRenderTime10ns + RENDER_PERIOD_10NS)
			{
				// Only render if enough time has passed since last render
				if (timeSinceLastRender >= vsyncThreshold) {
					shouldRender = true;
					render_timestamp = now10ns - lastRenderTime;
					lastRenderTime = now10ns;
					nextRenderTime10ns = now10ns + RENDER_PERIOD_10NS;
				}
			}

			// Shrink sleep chunk as we approach the predicted vblank.
			// This only affects minimalSleepCalc10ns for the remaining iterations
			// this frame; it resets to minimalSleepCalcBase10ns at the top of the
			// next frame so a bad prediction cannot cause permanent spin-lock.
			if (predictedNextVBlank10ns > 0)
			{
				int64_t timeUntilVBlank = predictedNextVBlank10ns - now10ns;
				if (timeUntilVBlank > 0 && timeUntilVBlank < minimalSleepCalcBase10ns * 2)
				{
					minimalSleepCalc10ns = std::max<int64_t>(timeUntilVBlank / 2, 5000LL);
				}
			}
		}
		else
		{
			// Fallback timer-based approach
			shouldRender = (now10ns >= (nextRenderTime10ns - std::max<int64_t>(getTime10ns() - lag, RENDER_PERIOD_10NS / 2)));
			if (shouldRender)
			{
				// Only render if enough time has passed since last render
				if (timeSinceLastRender >= vsyncThreshold) {
					render_timestamp = now10ns - lastRenderTime;
					lastRenderTime = now10ns;
					nextRenderTime10ns += RENDER_PERIOD_10NS;
				} else {
					shouldRender = false;
				}
			}
		}
	}
#elif defined(HX_LINUX)
		{
			SDL_Window* kbFocus = SDL_GetKeyboardFocus();
			if (kbFocus) {
				uint32_t focusedWindowID = SDL_GetWindowID(kbFocus);
				SDLWindow* focusedWindow = SDLWindow::windows[focusedWindowID];
				if (focusedWindow && focusedWindow->sdlWindow) {
					handleLinuxVsync(focusedWindow->sdlWindow, now10ns, lag, nextRenderTime10ns, shouldRender);
					// Check if we should skip rendering due to too small delta
					if (shouldRender && timeSinceLastRender < vsyncThreshold) {
						shouldRender = false;
					}
				} else {
					shouldRender = (now10ns >= (nextRenderTime10ns - std::max<int64_t>(getTime10ns() - lag, RENDER_PERIOD_10NS / 2)));
					if (shouldRender) {
						// Only render if enough time has passed since last render
						if (timeSinceLastRender >= vsyncThreshold) {
							render_timestamp = now10ns - lastRenderTime;
							lastRenderTime = now10ns;
							nextRenderTime10ns += RENDER_PERIOD_10NS;
						} else {
							shouldRender = false;
						}
					}
				}
			} else {
				shouldRender = (now10ns >= (nextRenderTime10ns - std::max<int64_t>(getTime10ns() - lag, RENDER_PERIOD_10NS / 2)));
				if (shouldRender) {
					// Only render if enough time has passed since last render
					if (timeSinceLastRender >= vsyncThreshold) {
						render_timestamp = now10ns - lastRenderTime;
						lastRenderTime = now10ns;
						nextRenderTime10ns += RENDER_PERIOD_10NS;
					} else {
						shouldRender = false;
					}
				}
			}
		}
#elif defined(HX_ANDROID)
		if (choreographer)
		{
			if (shouldRenderFromCallback)
			{
				// Only render if enough time has passed since last render
				if (timeSinceLastRender >= vsyncThreshold) {
					shouldRender = true;
					shouldRenderFromCallback = false;
					AChoreographer_postFrameCallback(choreographer,
													 choreographer_callback,
													 nullptr);
				} else {
					shouldRenderFromCallback = false;
					shouldRender = false;
				}
			}
		}
		else
		{
			shouldRender = (now10ns >= nextRenderTime10ns);
			if (shouldRender)
			{
				// Only render if enough time has passed since last render
				if (timeSinceLastRender >= vsyncThreshold) {
					render_timestamp = now10ns - lastRenderTime;
					lastRenderTime = now10ns;
					nextRenderTime10ns += RENDER_PERIOD_10NS;
				} else {
					shouldRender = false;
				}
			}
		}
#else
		shouldRender = (now10ns >= (nextRenderTime10ns - std::max<int64_t>(getTime10ns() - lag, RENDER_PERIOD_10NS / 2)));
		if (shouldRender)
		{
			// Only render if enough time has passed since last render
			if (timeSinceLastRender >= vsyncThreshold) {
				render_timestamp = RENDER_PERIOD_10NS;
				nextRenderTime10ns += RENDER_PERIOD_10NS;
			} else {
				shouldRender = false;
			}
		}
#endif

		PollInputs();

		if (shouldRender)
		{
			applicationEvent.type = UPDATE;
			applicationEvent.deltaTime = render_timestamp;
			ApplicationEvent::Dispatch(&applicationEvent);

			renderEvent.type = RENDER;
			RenderEvent::Dispatch(&renderEvent);

			lag = getTime10ns();
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