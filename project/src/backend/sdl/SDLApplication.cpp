/**
 * This class is where the main loop goes. For one, windows 10;
 * The said main loop uses:
   - A combination of high res waitable timer and an undocumented ntdll function
   - abused to be set to your literal frame time, to create a surreal rhythm game experience!
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

	SDLApplication::SDLApplication () {
		Uint32 initFlags = SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER | SDL_INIT_JOYSTICK;
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

		#ifdef HX_WINDOWS
		HANDLE hThread = GetCurrentThread();
		// Set current thread priority
		SetThreadPriority(hThread, THREAD_PRIORITY_TIME_CRITICAL);

		// Disable power throttling for this process
		PROCESS_POWER_THROTTLING_STATE PowerThrottling;
		PowerThrottling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
		PowerThrottling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
		PowerThrottling.StateMask = 0; // Disable throttling
		SetProcessInformation(GetCurrentProcess(), 
							ProcessPowerThrottling, 
							&PowerThrottling, 
							sizeof(PowerThrottling));

							
		if (!timer) {
			timer = CreateWaitableTimerEx(nullptr, nullptr,
												CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
			if (!timer) {
				std::cout << "Failed to create high-res timer\ncreating regular timer instead\n";
				timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
			}
		}
		#endif

	}

	#if HX_WINDOWS
	static HMODULE ntdll;
	#endif


	SDLApplication::~SDLApplication () {

		#if HX_WINDOWS
		if (timer) CloseHandle(timer);
		if (ntdll) FreeLibrary(ntdll);
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

		return Quit ();

		#endif

	}

	void SDLApplication::HandleEvent(SDL_Event* event) {

		#if defined(IPHONE) || defined(EMSCRIPTEN)
		int top = 0;
		gc_set_top_of_stack(&top, false);
		#endif

		switch (event->type) {

			// App lifecycle
			case SDL_APP_WILLENTERBACKGROUND:
				inBackground = true;
				windowEvent.type = WINDOW_DEACTIVATE;
				WindowEvent::Dispatch(&windowEvent);
				break;

			case SDL_APP_DIDENTERFOREGROUND:
				inBackground = false;
				windowEvent.type = WINDOW_ACTIVATE;
				WindowEvent::Dispatch(&windowEvent);
				break;

			// Quit event
			case SDL_QUIT:
				active = false;
				break;

			#ifndef EMSCRIPTEN
			case SDL_RENDER_DEVICE_RESET:
				renderEvent.type = RENDER_CONTEXT_LOST;
				RenderEvent::Dispatch(&renderEvent);

				renderEvent.type = RENDER_CONTEXT_RESTORED;
				RenderEvent::Dispatch(&renderEvent);
				break;
			#endif

			// Controller joystick accelerometer fallback
			default:
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

			// Window events
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
						ProcessWindowEvent(event);
						break;

					case SDL_WINDOWEVENT_EXPOSED:
					case SDL_WINDOWEVENT_SIZE_CHANGED:
						ProcessWindowEvent(event);
						break;

					case SDL_WINDOWEVENT_CLOSE:
						ProcessWindowEvent(event);
						active = false; // quit main loop
						break;
				}
				break;

			// File drop
			case SDL_DROPFILE:
				ProcessDropEvent(event);
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


	int SDLApplication::Quit () {
		applicationEvent.type = EXIT;
		ApplicationEvent::Dispatch (&applicationEvent);

		SDL_Quit ();
		return 0;

	}


	void SDLApplication::RegisterWindow (SDLWindow *window) {

		#ifdef IPHONE
		SDL_iPhoneSetAnimationCallback (window->sdlWindow, 1, UpdateFrame, NULL);
		#endif

	}

	#if HX_WINDOWS
	void adjustTimerResolutionDynamic(int updatePeriodUs) {
		typedef NTSTATUS (NTAPI *NtSetTimerResolution_t)(ULONG, BOOLEAN, PULONG);

		if (!ntdll) ntdll = LoadLibraryA("ntdll.dll");
		if (!ntdll) return;

		static NtSetTimerResolution_t NtSetTimerResolution =
			(NtSetTimerResolution_t)GetProcAddress(ntdll, "NtSetTimerResolution");

		if (!NtSetTimerResolution) return;

		// Convert period to approximate FPS
		int fps = (updatePeriodUs > 0) ? static_cast<int>(1000000 / updatePeriodUs) : 120;
		//printf("FPS SET TO %d\n", fps);

		// Map FPS to ideal timer resolution (microseconds)
		ULONG resolutionUs = (fps > 0) ? (ULONG)(10000000 / fps) : 10000;

		//printf("Requested Resolution: %.3f ms\n", resolutionUs / 10000.0);

		// Apply new resolution
		ULONG current;
		NTSTATUS status = NtSetTimerResolution(resolutionUs, TRUE, &current);

		printf("NtSetTimerResolution -> Status: 0x%08X, Requested: %.3fus, Actual current: %.3fms\n",
			(unsigned int)status, resolutionUs / 10000.0, current / 10000.0);
	}
	#endif


	void SDLApplication::SetFrameRate (double frameRate) {

		if (frameRate > 0) {

			UPDATE_PERIOD = 1000000.0 / frameRate;
			RENDER_PERIOD = 1000000.0 / 60.0;

			#if HX_WINDOWS
			int64_t timerResolution = UPDATE_PERIOD;
			if (timerResolution < 500) timerResolution = UPDATE_PERIOD;

			adjustTimerResolutionDynamic(timerResolution);
			#endif

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
		return int64_t(elapsedSeconds * 1'000'000.0);
		#elif defined(HX_LINUX)
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC_RAW, &ts); 
		return ts.tv_sec * 1'000'000 + ts.tv_nsec / 1000;
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
		// At 240fps, wake early and spin
		const int64_t SPIN_THRESHOLD_US = (UPDATE_PERIOD < 5000) ? 600 : 0; // 0.6ms for 240fps

		if (sleepForUs > SPIN_THRESHOLD_US) {
			FILETIME ft;
			GetSystemTimePreciseAsFileTime(&ft);
			ULARGE_INTEGER now;
			now.LowPart = ft.dwLowDateTime;
			now.HighPart = ft.dwHighDateTime;

			LARGE_INTEGER due;
			due.QuadPart = now.QuadPart + ((sleepForUs - SPIN_THRESHOLD_US) * 10);

			SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
			WaitForSingleObject(timer, INFINITE);
		}

		// Spin for final precision (only if needed)
		if (SPIN_THRESHOLD_US > 0) {
			while (getTime() < wakeTimeUs) {
				_mm_pause();
			}
		}

		#elif defined(HX_LINUX)
		struct timespec wake;
		wake.tv_sec = wakeTimeUs / 1000000;
		wake.tv_nsec = (wakeTimeUs % 1000000) * 1000;
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, nullptr);
		#endif
	}

	int64_t startTimestamp;
	void SDLApplication::Init () {
		active = true;
		int now = getTime();
		startTimestamp = lastUpdate = now;
	}

	// Most of this rewritten function were generated with claude.ai with a side of chatgpt
	// also look at power throttling in this class it's disabled for a very good reason
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
    int64_t pollTimestamp = currentTime; // Single timestamp for this poll batch

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
				case SDL_WINDOWEVENT:
				case SDL_DROPFILE:
				case SDL_FINGERMOTION:
				case SDL_FINGERDOWN:
				case SDL_FINGERUP:
					isInputEvent = true;
					break;
			}
			
			if (isInputEvent) {
				TimestampedInputEvent tie;
				tie.event = event;
				tie.timestamp = pollTimestamp;
				inputEventQueue.push_back(tie);
			} else {
				HandleEvent(&event);
				if (!active) return active;
			}
		}

		static int64_t baseTime = 0;
		static int64_t updateCounter = 0;
		static int64_t renderCounter = 0;

		if (baseTime == 0) {
			baseTime = currentTime;
			prevFrameTime = currentTime;
			inputEventQueue.reserve(26); // Pre-allocate
		}

		int64_t deltaTime = currentTime - prevFrameTime;
		if (deltaTime > 100000) {
			baseTime = currentTime - updateCounter * UPDATE_PERIOD;
		}

		prevFrameTime = currentTime;

		double nextUpdateTime = baseTime + (updateCounter + 1) * UPDATE_PERIOD;
		double nextRenderTime = baseTime + (renderCounter + 1) * RENDER_PERIOD;

		if (currentTime >= nextUpdateTime) {
			// Process inputs
			for (size_t i = 0; i < inputEventQueue.size(); i++) {
				HandleInputEvent(&inputEventQueue[i].event);
			}
			inputEventQueue.clear(); // Keeps capacity

			applicationEvent.type = UPDATE;
			applicationEvent.deltaTime = UPDATE_PERIOD;
			ApplicationEvent::Dispatch(&applicationEvent);

			updateCounter++;
			nextUpdateTime = baseTime + (updateCounter + 1) * UPDATE_PERIOD;
		}

		if (currentTime >= nextRenderTime) {
			renderEvent.type = RENDER;
			RenderEvent::Dispatch(&renderEvent);
			renderCounter++;
			nextRenderTime = baseTime + (renderCounter + 1) * RENDER_PERIOD;
		}

		int64_t nextEventTime = std::min<int64_t>(nextUpdateTime, nextRenderTime);

		if (nextEventTime > currentTime) {
			coolSleepUntil(nextEventTime);
		}

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
