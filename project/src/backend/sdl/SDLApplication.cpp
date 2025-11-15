/**
 * This class is where the main loop goes. For one, windows 10;
 * The said main loop uses:
   - A combination of high res waitable timer and crazy hacks and shit to create a surreal rhythm game experience!
 * On the other hand, linux just already has an accurate sleep function. I wanted to create a fun crispy smooth experience for literally everyone who are on windows,
 so that meant doing this bullshit to compensate. How about I make a literal main loop library out of this shit?
**/

#include "SDLApplication.h"
#include "SDLGamepad.h"
#include "SDLJoystick.h"
#include <system/System.h>
#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <string>
#include <stdio.h>
#include <vector>

using namespace std;

#ifdef HX_WINDOWS
#include <windows.h>
#include <cstdint>
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#include <SDL_syswm.h>
#endif

#ifdef HX_MACOS
#include <CoreFoundation/CoreFoundation.h>
#endif

#ifdef EMSCRIPTEN
#include "emscripten.h"
#endif


namespace lime {


	AutoGCRoot* Application::callback = 0;
	SDLApplication* SDLApplication::currentApplication = 0;

	const int analogAxisDeadZone = 1000;
	std::map<int, std::map<int, int> > gamepadsAxisMap;
	bool inBackground = false;

	// --- timing constants for decoupled loop ---
	static double UPDATE_PERIOD = 1000000.0 / 120; // fixed update @ 240Hz
	static double RENDER_PERIOD = 1000000.0 / 60;  // render @ 60Hz

    #if HX_WINDOWS
    static HANDLE timer;
    #endif

	static Uint32 initFlags;

	SDLApplication::SDLApplication () {
		initFlags = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_JOYSTICK;
		#if defined(LIME_MOJOAL) || defined(LIME_OPENALSOFT)
		initFlags |= SDL_INIT_AUDIO;
		#endif

		if (SDL_Init (initFlags) != 0) {

			printf ("Could not initialize SDL: %s.\n", SDL_GetError ());

		}

		SDL_LogSetPriority (SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_WARN);

		currentApplication = this;

		ApplicationEvent applicationEvent;
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

		SDL_EventState (SDL_DROPFILE, SDL_ENABLE);
		SDLJoystick::Init ();

		#ifdef HX_MACOS
		CFURLRef resourcesURL = CFBundleCopyResourcesDirectoryURL (CFBundleGetMainBundle ());
		char path[PATH_MAX];

		if (CFURLGetFileSystemRepresentation (resourcesURL, TRUE, (UInt8 *)path, PATH_MAX)) {

			chdir (path);

		}

		CFRelease (resourcesURL);
		#endif

	}


	SDLApplication::~SDLApplication () {

		#if HX_WINDOWS
		if (timer) CloseHandle(timer);
		#endif

	}


	int SDLApplication::Exec () {

		Init ();

		#ifdef EMSCRIPTEN
		emscripten_cancel_main_loop ();
		emscripten_set_main_loop (UpdateFrame, 0, 0);
		emscripten_set_main_loop_timing (EM_TIMING_RAF, 1);
		#endif

		#if defined(IPHONE) || defined(EMSCRIPTEN)

		return 0;

		#else

		while (active) {

			Update ();

		}

		return 0;

		#endif

	}

	void SDLApplication::HandleEvent (SDL_Event* event) {

		#if defined(IPHONE) || defined(EMSCRIPTEN)

		int top = 0;
		gc_set_top_of_stack(&top,false);

		#endif

		switch (event->type) {

			case SDL_APP_WILLENTERBACKGROUND:

				inBackground = true;

				windowEvent.type = WINDOW_DEACTIVATE;
				WindowEvent::Dispatch (&windowEvent);
				break;

			case SDL_APP_WILLENTERFOREGROUND:

				break;

			case SDL_APP_DIDENTERFOREGROUND:

				windowEvent.type = WINDOW_ACTIVATE;
				WindowEvent::Dispatch (&windowEvent);

				inBackground = false;
				break;

			case SDL_WINDOWEVENT:

				switch (event->window.event) {

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

						ProcessWindowEvent (event);
						break;

					case SDL_WINDOWEVENT_EXPOSED:

						ProcessWindowEvent (event);

						if (!inBackground) {

							RenderEvent::Dispatch (&renderEvent);

						}

						break;

					case SDL_WINDOWEVENT_SIZE_CHANGED:

						ProcessWindowEvent (event);

						if (!inBackground) {

							RenderEvent::Dispatch (&renderEvent);

						}

						break;

					case SDL_WINDOWEVENT_CLOSE:

						ProcessWindowEvent (event);

						// Avoid handling SDL_QUIT if in response to window.close
						SDL_Event event;

						if (SDL_PollEvent (&event)) {

							if (event.type != SDL_QUIT) {

								HandleEvent (&event);

							}

						}

						break;

				}

				break;

			case SDL_QUIT:

				active = false;
				Quit ();

				break;

		}

	}

	void SDLApplication::HandleInputEvent(SDL_Event* event) {

		#if defined(IPHONE) || defined(EMSCRIPTEN)
		int top = 0;
		gc_set_top_of_stack(&top, false);
		#endif

		switch (event->type) {
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
				if (SDLJoystick::IsAccelerometer(event->jaxis.which)) {
					ProcessSensorEvent(event);
				} else {
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



	void SDLApplication::ProcessClipboardEvent (SDL_Event* event) {

		if (ClipboardEvent::callback) {

			clipboardEvent.type = CLIPBOARD_UPDATE;

			ClipboardEvent::Dispatch (&clipboardEvent);

		}

	}


	void SDLApplication::ProcessDropEvent (SDL_Event* event) {

		if (DropEvent::callback) {

			dropEvent.type = DROP_FILE;
			dropEvent.file = (vbyte*)event->drop.file;

			DropEvent::Dispatch (&dropEvent);
			SDL_free (dropEvent.file);

		}

	}


	void SDLApplication::ProcessGamepadEvent (SDL_Event* event) {

		if (GamepadEvent::callback) {

			switch (event->type) {

				case SDL_CONTROLLERAXISMOTION:

					if (gamepadsAxisMap[event->caxis.which].empty ()) {

						gamepadsAxisMap[event->caxis.which][event->caxis.axis] = event->caxis.value;

					} else if (gamepadsAxisMap[event->caxis.which][event->caxis.axis] == event->caxis.value) {

						break;

					}

					gamepadEvent.type = GAMEPAD_AXIS_MOVE;
					gamepadEvent.axis = event->caxis.axis;
					gamepadEvent.id = event->caxis.which;

					if (event->caxis.value > -analogAxisDeadZone && event->caxis.value < analogAxisDeadZone) {

						if (gamepadsAxisMap[event->caxis.which][event->caxis.axis] != 0) {

							gamepadsAxisMap[event->caxis.which][event->caxis.axis] = 0;
							gamepadEvent.axisValue = 0;
							GamepadEvent::Dispatch (&gamepadEvent);

						}

						break;

					}

					gamepadsAxisMap[event->caxis.which][event->caxis.axis] = event->caxis.value;
					gamepadEvent.axisValue = event->caxis.value / (event->caxis.value > 0 ? 32767.0 : 32768.0);

					GamepadEvent::Dispatch (&gamepadEvent);
					break;

				case SDL_CONTROLLERBUTTONDOWN:

					gamepadEvent.type = GAMEPAD_BUTTON_DOWN;
					gamepadEvent.button = event->cbutton.button;
					gamepadEvent.id = event->cbutton.which;

					GamepadEvent::Dispatch (&gamepadEvent);
					break;

				case SDL_CONTROLLERBUTTONUP:

					gamepadEvent.type = GAMEPAD_BUTTON_UP;
					gamepadEvent.button = event->cbutton.button;
					gamepadEvent.id = event->cbutton.which;

					GamepadEvent::Dispatch (&gamepadEvent);
					break;

				case SDL_CONTROLLERDEVICEADDED:

					if (SDLGamepad::Connect (event->cdevice.which)) {

						gamepadEvent.type = GAMEPAD_CONNECT;
						gamepadEvent.id = SDLGamepad::GetInstanceID (event->cdevice.which);

						GamepadEvent::Dispatch (&gamepadEvent);

					}

					break;

				case SDL_CONTROLLERDEVICEREMOVED: {

					gamepadEvent.type = GAMEPAD_DISCONNECT;
					gamepadEvent.id = event->cdevice.which;

					GamepadEvent::Dispatch (&gamepadEvent);
					SDLGamepad::Disconnect (event->cdevice.which);
					break;

				}

			}

		}

	}


	void SDLApplication::ProcessJoystickEvent (SDL_Event* event) {

		if (JoystickEvent::callback) {

			switch (event->type) {

				case SDL_JOYAXISMOTION:

					if (!SDLJoystick::IsAccelerometer (event->jaxis.which)) {

						joystickEvent.type = JOYSTICK_AXIS_MOVE;
						joystickEvent.index = event->jaxis.axis;
						joystickEvent.x = event->jaxis.value / (event->jaxis.value > 0 ? 32767.0 : 32768.0);
						joystickEvent.id = event->jaxis.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYBALLMOTION:

					if (!SDLJoystick::IsAccelerometer (event->jball.which)) {

						joystickEvent.type = JOYSTICK_TRACKBALL_MOVE;
						joystickEvent.index = event->jball.ball;
						joystickEvent.x = event->jball.xrel / (event->jball.xrel > 0 ? 32767.0 : 32768.0);
						joystickEvent.y = event->jball.yrel / (event->jball.yrel > 0 ? 32767.0 : 32768.0);
						joystickEvent.id = event->jball.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYBUTTONDOWN:

					if (!SDLJoystick::IsAccelerometer (event->jbutton.which)) {

						joystickEvent.type = JOYSTICK_BUTTON_DOWN;
						joystickEvent.index = event->jbutton.button;
						joystickEvent.id = event->jbutton.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYBUTTONUP:

					if (!SDLJoystick::IsAccelerometer (event->jbutton.which)) {

						joystickEvent.type = JOYSTICK_BUTTON_UP;
						joystickEvent.index = event->jbutton.button;
						joystickEvent.id = event->jbutton.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYHATMOTION:

					if (!SDLJoystick::IsAccelerometer (event->jhat.which)) {

						joystickEvent.type = JOYSTICK_HAT_MOVE;
						joystickEvent.index = event->jhat.hat;
						joystickEvent.eventValue = event->jhat.value;
						joystickEvent.id = event->jhat.which;

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYDEVICEADDED:

					if (SDLJoystick::Connect (event->jdevice.which)) {

						joystickEvent.type = JOYSTICK_CONNECT;
						joystickEvent.id = SDLJoystick::GetInstanceID (event->jdevice.which);

						JoystickEvent::Dispatch (&joystickEvent);

					}
					break;

				case SDL_JOYDEVICEREMOVED:

					if (!SDLJoystick::IsAccelerometer (event->jdevice.which)) {

						joystickEvent.type = JOYSTICK_DISCONNECT;
						joystickEvent.id = event->jdevice.which;

						JoystickEvent::Dispatch (&joystickEvent);
						SDLJoystick::Disconnect (event->jdevice.which);

					}
					break;

			}

		}

	}


	void SDLApplication::ProcessKeyEvent (SDL_Event* event) {

		if (KeyEvent::callback) {

			switch (event->type) {

				case SDL_KEYDOWN: keyEvent.type = KEY_DOWN; break;
				case SDL_KEYUP: keyEvent.type = KEY_UP; break;

			}

			keyEvent.keyCode = event->key.keysym.sym;
			keyEvent.modifier = event->key.keysym.mod;
			keyEvent.windowID = event->key.windowID;

			if (keyEvent.type == KEY_DOWN) {

				if (keyEvent.keyCode == SDLK_CAPSLOCK) keyEvent.modifier |= KMOD_CAPS;
				if (keyEvent.keyCode == SDLK_LALT) keyEvent.modifier |= KMOD_LALT;
				if (keyEvent.keyCode == SDLK_LCTRL) keyEvent.modifier |= KMOD_LCTRL;
				if (keyEvent.keyCode == SDLK_LGUI) keyEvent.modifier |= KMOD_LGUI;
				if (keyEvent.keyCode == SDLK_LSHIFT) keyEvent.modifier |= KMOD_LSHIFT;
				if (keyEvent.keyCode == SDLK_MODE) keyEvent.modifier |= KMOD_MODE;
				if (keyEvent.keyCode == SDLK_NUMLOCKCLEAR) keyEvent.modifier |= KMOD_NUM;
				if (keyEvent.keyCode == SDLK_RALT) keyEvent.modifier |= KMOD_RALT;
				if (keyEvent.keyCode == SDLK_RCTRL) keyEvent.modifier |= KMOD_RCTRL;
				if (keyEvent.keyCode == SDLK_RGUI) keyEvent.modifier |= KMOD_RGUI;
				if (keyEvent.keyCode == SDLK_RSHIFT) keyEvent.modifier |= KMOD_RSHIFT;

			}

			KeyEvent::Dispatch (&keyEvent);

		}

	}


	void SDLApplication::ProcessMouseEvent (SDL_Event* event) {

		if (MouseEvent::callback) {

			switch (event->type) {

				case SDL_MOUSEMOTION:

					mouseEvent.type = MOUSE_MOVE;
					mouseEvent.x = event->motion.x;
					mouseEvent.y = event->motion.y;
					mouseEvent.movementX = event->motion.xrel;
					mouseEvent.movementY = event->motion.yrel;
					break;

				case SDL_MOUSEBUTTONDOWN:

					SDL_CaptureMouse (SDL_TRUE);

					mouseEvent.type = MOUSE_DOWN;
					mouseEvent.button = event->button.button - 1;
					mouseEvent.x = event->button.x;
					mouseEvent.y = event->button.y;
					mouseEvent.clickCount = event->button.clicks;
					break;

				case SDL_MOUSEBUTTONUP:

					SDL_CaptureMouse (SDL_FALSE);

					mouseEvent.type = MOUSE_UP;
					mouseEvent.button = event->button.button - 1;
					mouseEvent.x = event->button.x;
					mouseEvent.y = event->button.y;
					mouseEvent.clickCount = event->button.clicks;
					break;

				case SDL_MOUSEWHEEL:

					mouseEvent.type = MOUSE_WHEEL;

					if (event->wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {

						mouseEvent.x = -event->wheel.x;
						mouseEvent.y = -event->wheel.y;

					} else {

						mouseEvent.x = event->wheel.x;
						mouseEvent.y = event->wheel.y;

					}
					break;

			}

			mouseEvent.windowID = event->button.windowID;
			MouseEvent::Dispatch (&mouseEvent);

		}

	}


	void SDLApplication::ProcessSensorEvent (SDL_Event* event) {

		if (SensorEvent::callback) {

			double value = event->jaxis.value / 32767.0f;

			switch (event->jaxis.axis) {

				case 0: sensorEvent.x = value; break;
				case 1: sensorEvent.y = value; break;
				case 2: sensorEvent.z = value; break;
				default: break;

			}

			SensorEvent::Dispatch (&sensorEvent);

		}

	}


	void SDLApplication::ProcessTextEvent (SDL_Event* event) {
		if (TextEvent::callback) {
			switch (event->type) {
				case SDL_TEXTINPUT:
					textEvent.type = TEXT_INPUT;
					break;

				case SDL_TEXTEDITING:
					textEvent.type = TEXT_EDIT;
					textEvent.start = event->edit.start;
					textEvent.length = event->edit.length;
					break;
			}

			// Use static buffer instead of malloc/free
			static char textBuffer[SDL_TEXTINPUTEVENT_TEXT_SIZE];
			strncpy(textBuffer, event->text.text, SDL_TEXTINPUTEVENT_TEXT_SIZE - 1);
			textBuffer[SDL_TEXTINPUTEVENT_TEXT_SIZE - 1] = '\0';
			textEvent.text = (vbyte*)textBuffer;

			textEvent.windowID = event->text.windowID;
			TextEvent::Dispatch(&textEvent);
		}
	}


	void SDLApplication::ProcessTouchEvent (SDL_Event* event) {

		if (TouchEvent::callback) {

			switch (event->type) {

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

			TouchEvent::Dispatch (&touchEvent);

		}

	}


	void SDLApplication::ProcessWindowEvent (SDL_Event* event) {

		if (WindowEvent::callback) {

			switch (event->window.event) {

				case SDL_WINDOWEVENT_SHOWN: windowEvent.type = WINDOW_SHOW; break;
				case SDL_WINDOWEVENT_CLOSE: windowEvent.type = WINDOW_CLOSE; break;
				case SDL_WINDOWEVENT_HIDDEN: windowEvent.type = WINDOW_HIDE; break;
				case SDL_WINDOWEVENT_ENTER: windowEvent.type = WINDOW_ENTER; break;
				case SDL_WINDOWEVENT_FOCUS_GAINED: windowEvent.type = WINDOW_FOCUS_IN; break;
				case SDL_WINDOWEVENT_FOCUS_LOST: windowEvent.type = WINDOW_FOCUS_OUT; break;
				case SDL_WINDOWEVENT_LEAVE: windowEvent.type = WINDOW_LEAVE; break;
				case SDL_WINDOWEVENT_MAXIMIZED: windowEvent.type = WINDOW_MAXIMIZE; break;
				case SDL_WINDOWEVENT_MINIMIZED: windowEvent.type = WINDOW_MINIMIZE; break;
				case SDL_WINDOWEVENT_EXPOSED: windowEvent.type = WINDOW_EXPOSE; break;

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

				case SDL_WINDOWEVENT_RESTORED: windowEvent.type = WINDOW_RESTORE; break;

			}

			windowEvent.windowID = event->window.windowID;
			WindowEvent::Dispatch (&windowEvent);

		}

	}

	static bool alreadyQuit = false;
	int SDLApplication::Quit () {
		if (alreadyQuit) return 0;

		// You can call this quit function twice here, so that's why I implemented this static boolean variable here to check. And yes, I've tested the print here.
		applicationEvent.type = EXIT;
		ApplicationEvent::Dispatch (&applicationEvent);

		//windowEvent.type = WINDOW_CLOSE;
		//WindowEvent::Dispatch (&windowEvent);

		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			HandleEvent(&event);
		}

		//printf("CLOSED RAAAAAAAAAAAAAAGH\n");

		SDL_QuitSubSystem (initFlags);

		SDL_Quit ();

		alreadyQuit = true;

		return 0;

	}


	void SDLApplication::RegisterWindow (SDLWindow *window) {

		#ifdef IPHONE
		SDL_iPhoneSetAnimationCallback (window->sdlWindow, 1, UpdateFrame, NULL);
		#endif

	}


	void SDLApplication::SetFrameRate (double frameRate) {

		if (frameRate > 0) {

			UPDATE_PERIOD = 1000000.0 / frameRate;
			RENDER_PERIOD = 1000000.0 / 60.0;

		} else {

			UPDATE_PERIOD = 0;
			RENDER_PERIOD = 0;

		}

	}


	void SDLApplication::SetRenderFrameRate (double renderFrameRate) {

		if (renderFrameRate > 60) {

			RENDER_PERIOD = 1000000.0 / renderFrameRate;

		} else if (renderFrameRate == 0) {

			RENDER_PERIOD = 0.0;

		} else {

			RENDER_PERIOD = 1000000.0 / 60.0;

		}

	}

	// Add these static variables after your existing statics
	#ifdef HX_WINDOWS
	static int64_t monitorRefreshPeriodUs = 16667; // Default 60Hz
	static int64_t predictedNextVsync = 0;
	static std::vector<int64_t> recentPresentTimes;
	static std::vector<int64_t> recentRenderDurations;
	static int64_t vsyncPhaseOffset = 0;
	static int64_t avgRenderDuration = 2000; // Start with 2ms estimate
	static bool vsyncCalibrated = false;
	static int calibrationFrames = 0;
	#endif

	int64_t prevFrameTime = 0;

	int64_t getTime() {
		#ifdef HX_WINDOWS
		static LARGE_INTEGER freq = {};
		static LARGE_INTEGER start = {};
		if (freq.QuadPart == 0) {
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&start);
		}

		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);

		double elapsedSeconds = double(counter.QuadPart - start.QuadPart) / freq.QuadPart;
		return int64_t(elapsedSeconds * 1000000.0);
		#elif defined(HX_LINUX)
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC_RAW, &ts); 
		return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
		#else
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count();
		#endif
	}

	void coolSleepUntil(int64_t wakeTimeUs) {
		int64_t currentTime = getTime();
		int64_t sleepForUs = wakeTimeUs - currentTime;

		if (sleepForUs <= 0) return;

		#if HX_WINDOWS
		const int64_t SPIN_THRESHOLD_US = 500;

		if (sleepForUs > SPIN_THRESHOLD_US) {
			// Get current system time in FILETIME
			FILETIME ft;
			GetSystemTimePreciseAsFileTime(&ft);
			ULARGE_INTEGER now;
			now.LowPart = ft.dwLowDateTime;
			now.HighPart = ft.dwHighDateTime;
			
			// Add the sleep duration (minus spin threshold) to get absolute wake time
			LARGE_INTEGER due;
			due.QuadPart = now.QuadPart + ((sleepForUs - SPIN_THRESHOLD_US) * 10);
			
			SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
			WaitForSingleObject(timer, INFINITE);
		}

		// Spin for final precision
		while (getTime() < wakeTimeUs) {
			_mm_pause();
		}
		#elif defined(HX_LINUX)
		struct timespec wake;
		wake.tv_sec = wakeTimeUs / 1000000;
		wake.tv_nsec = (wakeTimeUs % 1000000) * 1000;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, nullptr);
		#endif
	}

	int64_t startTimestamp;
	void SDLApplication::Init() {
		active = true;
		int64_t now = getTime();
		startTimestamp = lastUpdate = now;

		#ifdef HX_WINDOWS
		TIMECAPS tc;
		timeGetDevCaps(&tc, sizeof(TIMECAPS));
		printf("Timer caps: min=%u, max=%u\n", tc.wPeriodMin, tc.wPeriodMax);

		SDL_Window* sdlWindow = SDL_GL_GetCurrentWindow();
		if (!sdlWindow) {
			sdlWindow = SDL_GetWindowFromID(1);
		}

		if (sdlWindow) {
			SDL_SysWMinfo wmInfo;
			SDL_VERSION(&wmInfo.version);
			if (SDL_GetWindowWMInfo(sdlWindow, &wmInfo)) {
				HWND hwnd = wmInfo.info.win.window;
				
				// Get actual monitor refresh rate
				HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
				MONITORINFOEX monitorInfo;
				monitorInfo.cbSize = sizeof(MONITORINFOEX);
				GetMonitorInfo(monitor, &monitorInfo);
				
				DEVMODE devMode;
				devMode.dmSize = sizeof(DEVMODE);
				EnumDisplaySettings(monitorInfo.szDevice, ENUM_CURRENT_SETTINGS, &devMode);
				
				double refreshRate = (double)devMode.dmDisplayFrequency;
				monitorRefreshPeriodUs = (int64_t)(1000000.0 / refreshRate);
				printf("Monitor refresh rate: %.2f Hz (period: %lld us)\n", refreshRate, monitorRefreshPeriodUs);
			}
		}

		HANDLE hThread = GetCurrentThread();
		SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL);
		SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

		if (!timer) {
			timer = CreateWaitableTimerEx(nullptr, nullptr,
												CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (!timer) {
				std::cout << "Failed to create high-res timer\ncreating regular timer instead\n";
				timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
			} else {
				std::cout << "Successfully created high-res timer!\n";
			}
		}
		
		recentPresentTimes.reserve(120);
		recentRenderDurations.reserve(120);
		predictedNextVsync = now + monitorRefreshPeriodUs;
		vsyncCalibrated = false;
		calibrationFrames = 0;
		avgRenderDuration = 2000; // 2ms initial estimate
		#endif
	}

	// Vsync phase calibration - learns when vsync actually happens AND how long rendering takes
	#ifdef HX_WINDOWS
	void CalibrateVsyncPhase(int64_t presentTime, int64_t renderDuration) {
		recentPresentTimes.push_back(presentTime);
		recentRenderDurations.push_back(renderDuration);
		
		// Keep last 60 samples
		if (recentPresentTimes.size() > 60) {
			recentPresentTimes.erase(recentPresentTimes.begin());
		}
		if (recentRenderDurations.size() > 60) {
			recentRenderDurations.erase(recentRenderDurations.begin());
		}
		
		calibrationFrames++;
		
		// Calculate rolling average of render duration
		if (recentRenderDurations.size() >= 10) {
			int64_t sum = 0;
			// Use recent 10 frames for average
			int startIdx = std::max<int>(0, (int)recentRenderDurations.size() - 10);
			for (size_t i = startIdx; i < recentRenderDurations.size(); i++) {
				sum += recentRenderDurations[i];
			}
			avgRenderDuration = sum / (recentRenderDurations.size() - startIdx);
		}
		
		// Need at least 30 frames to calibrate vsync phase
		if (calibrationFrames < 30) return;
		
		if (!vsyncCalibrated && recentPresentTimes.size() >= 30) {
			// Find the phase offset - where in the vsync cycle do presents actually happen?
			int64_t lastPresent = recentPresentTimes.back();
			int64_t firstPresent = recentPresentTimes.front();
			
			// Calculate average present interval
			int64_t totalInterval = lastPresent - firstPresent;
			int64_t avgInterval = totalInterval / (recentPresentTimes.size() - 1);
			
			// Round to nearest refresh period
			int64_t measuredPeriod = ((avgInterval + monitorRefreshPeriodUs / 2) / monitorRefreshPeriodUs) * monitorRefreshPeriodUs;
			
			if (measuredPeriod > 0) {
				// Find phase within the refresh cycle
				vsyncPhaseOffset = lastPresent % monitorRefreshPeriodUs;
				vsyncCalibrated = true;
				
				printf("Vsync calibrated! Phase offset: %lld us, Measured period: %lld us, Avg render: %lld us\n", 
					vsyncPhaseOffset, measuredPeriod, avgRenderDuration);
			}
		}
		
		// Update prediction based on recent presents
		if (vsyncCalibrated && recentPresentTimes.size() >= 3) {
			// Use last present time to predict next vsync
			int64_t lastPresent = recentPresentTimes.back();
			
			// Next vsync is last present + refresh period, aligned to phase
			int64_t nextVsync = lastPresent + monitorRefreshPeriodUs;
			
			// Snap to phase
			int64_t currentPhase = nextVsync % monitorRefreshPeriodUs;
			int64_t phaseDrift = vsyncPhaseOffset - currentPhase;
			
			// Adjust by small amounts to stay in sync
			if (abs(phaseDrift) < monitorRefreshPeriodUs / 4) {
				nextVsync += phaseDrift;
			}
			
			predictedNextVsync = nextVsync;
		}
	}
	#endif

	// Most of this rewritten function were generated with claude.ai with a side of chatgpt
	// Remove the input polling thread - SDL event polling MUST be on main thread
	// Keep only these for timestamping:
	struct TimestampedInputEvent {
		SDL_Event event;
		int64_t timestamp;
	};
	static std::vector<TimestampedInputEvent> inputEventQueue;

	// Modified Update - poll everything on main thread but timestamp inputs
	bool SDLApplication::Update() {
		int64_t currentTime = getTime();

		// Poll events
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			bool isInputEvent = false;
			switch (event.type) {
				case SDL_CLIPBOARDUPDATE:
				case SDL_CONTROLLERAXISMOTION:
				case SDL_CONTROLLERBUTTONDOWN:
				case SDL_CONTROLLERBUTTONUP:
				case SDL_CONTROLLERDEVICEADDED:
				case SDL_CONTROLLERDEVICEREMOVED:
				case SDL_JOYAXISMOTION:
				case SDL_JOYBALLMOTION:
				case SDL_JOYBUTTONDOWN:
				case SDL_JOYBUTTONUP:
				case SDL_JOYHATMOTION:
				case SDL_JOYDEVICEADDED:
				case SDL_JOYDEVICEREMOVED:
				case SDL_KEYDOWN:
				case SDL_KEYUP:
				case SDL_MOUSEMOTION:
				case SDL_MOUSEBUTTONDOWN:
				case SDL_MOUSEBUTTONUP:
				case SDL_MOUSEWHEEL:
				case SDL_TEXTINPUT:
				case SDL_TEXTEDITING:
				case SDL_FINGERMOTION:
				case SDL_FINGERDOWN:
				case SDL_FINGERUP:
					isInputEvent = true;
					break;
			}

			if (isInputEvent) {
				TimestampedInputEvent tie;
				tie.event = event;
				tie.timestamp = currentTime;
				inputEventQueue.push_back(tie);
			} else {
				HandleEvent(&event);
			}
		}

		static int64_t baseTime = 0;
		static int64_t tickCounter = 0;
		static int64_t lastRenderTime = 0;
		static bool shouldRender = false;

		if (baseTime == 0) {
			baseTime = currentTime;
			prevFrameTime = currentTime;
			inputEventQueue.reserve(32);
			tickCounter = 0;
			lastRenderTime = currentTime;
		}

		prevFrameTime = currentTime;

		// Calculate next update tick
		int64_t nextTickTime = baseTime + (tickCounter + 1) * UPDATE_PERIOD;

		// LAG DETECTION
		int64_t lagAmount = currentTime - nextTickTime;
		if (lagAmount > UPDATE_PERIOD * 2) {
			baseTime = currentTime;
			tickCounter = 0;
			nextTickTime = baseTime + UPDATE_PERIOD;
		}

		// Process update ticks
		if (currentTime >= nextTickTime) {
			// Process inputs with minimal lag
			for (size_t i = 0; i < inputEventQueue.size(); i++) {
				HandleInputEvent(&inputEventQueue[i].event);
			}
			inputEventQueue.clear();

			// Update at your target Hz (120 Hz in your case)
			applicationEvent.type = UPDATE;
			applicationEvent.deltaTime = UPDATE_PERIOD;
			ApplicationEvent::Dispatch(&applicationEvent);

			#ifdef HX_WINDOWS
			// Predictive rendering with render duration compensation
			// The key: Start rendering early enough that it FINISHES right at vsync
			
			const int64_t SAFETY_MARGIN_US = 500; // Extra 500us safety buffer
			const int64_t MIN_FRAME_INTERVAL_US = 15000; // Don't render more often than every 15ms
			
			// Calculate when we need to START rendering to finish by vsync
			// We need to finish avgRenderDuration + SAFETY_MARGIN before vsync
			int64_t desiredRenderStartTime = predictedNextVsync - avgRenderDuration - SAFETY_MARGIN_US;
			int64_t timeUntilRenderStart = desiredRenderStartTime - currentTime;
			int64_t timeUntilVsync = predictedNextVsync - currentTime;
			int64_t timeSinceRender = currentTime - lastRenderTime;
			
			// Decide if we should render on this tick
			shouldRender = false;
			
			if (timeSinceRender >= MIN_FRAME_INTERVAL_US) {
				// Check if we're at or past the time we should start rendering
				if (timeUntilRenderStart <= 0 && timeUntilVsync > 0) {
					shouldRender = true;
				}
				// If we missed this vsync entirely, skip to next one
				else if (timeUntilVsync < -monitorRefreshPeriodUs / 2) {
					predictedNextVsync += monitorRefreshPeriodUs;
				}
			}
			
			if (shouldRender) {
				// Render NOW - don't wait for anything
				int64_t renderStartTime = getTime();
				
				renderEvent.type = RENDER;
				RenderEvent::Dispatch(&renderEvent);
				
				int64_t renderEndTime = getTime();
				int64_t renderDuration = renderEndTime - renderStartTime;
				lastRenderTime = renderStartTime;
				
				// After present, calibrate our vsync prediction with actual render duration
				CalibrateVsyncPhase(renderEndTime, renderDuration);
				
				// Predict next vsync
				if (!vsyncCalibrated) {
					// During calibration, just assume regular intervals
					predictedNextVsync = renderEndTime + monitorRefreshPeriodUs;
				}
				// else: CalibrateVsyncPhase already updated predictedNextVsync
				
				// Debug output every second
				static int64_t lastDebugTime = 0;
				if (renderStartTime - lastDebugTime > 1000000) {
					int64_t actualTimeBeforeVsync = predictedNextVsync - renderEndTime;
					printf("Render: %lld us (avg: %lld us), Finished %lld us before vsync, Calibrated: %d\n", 
						renderDuration, avgRenderDuration, actualTimeBeforeVsync, vsyncCalibrated ? 1 : 0);
					lastDebugTime = renderStartTime;
				}
			}
			#else
			// Non-Windows: use original domain-based rendering
			int64_t domain = (int64_t)(RENDER_PERIOD / UPDATE_PERIOD);
			if (tickCounter % domain == 0) {
				renderEvent.type = RENDER;
				RenderEvent::Dispatch(&renderEvent);
			}
			#endif

			tickCounter++;
			nextTickTime = baseTime + (tickCounter + 1) * UPDATE_PERIOD;
		}

		// Sleep until next tick
		coolSleepUntil(nextTickTime);

		return active;
	}


	void SDLApplication::UpdateFrame () {
		currentApplication->Update ();
	}


	void SDLApplication::UpdateFrame (void*) {

		UpdateFrame ();

	}

	Application* CreateApplication () {

		return new SDLApplication ();

	}


}


#ifdef ANDROID
int SDL_main (int argc, char *argv[]) { return 0; }
#endif