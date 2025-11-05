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
    vbyte textBuffer[256];        // For SDL_TEXTINPUT / TEXTEDITING
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

		if (!timer) timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
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

			// Quit event
			case SDL_QUIT:
				active = false;
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



	void SDLApplication::ProcessClipboardEvent (SDL_Event* event) {

		if (ClipboardEvent::callback) {

			clipboardEvent.type = CLIPBOARD_UPDATE;

			ClipboardEvent::Dispatch (&clipboardEvent);

		}

	}


	void SDLApplication::ProcessDropEvent(SDL_Event* event) {
		if (!DropEvent::callback) return;

		dropEvent.type = DROP_FILE;
		dropEvent.file = (vbyte*)event->drop.file; // SDL owns memory
		DropEvent::Dispatch(&dropEvent);
		SDL_free(dropEvent.file); // safe
	}


	void SDLApplication::ProcessGamepadEvent(SDL_Event* event) {
		if (!GamepadEvent::callback) return;

		int id = 0;
		switch (event->type) {
			case SDL_CONTROLLERAXISMOTION: {
				id = event->caxis.which;
				auto &axisMap = gamepadsAxisMap[id];
				int16_t value = event->caxis.value;

				// Only dispatch if value changed significantly
				if (axisMap[event->caxis.axis] == value) break;

				// Apply deadzone
				if (value > -analogAxisDeadZone && value < analogAxisDeadZone) {
					if (axisMap[event->caxis.axis] != 0) {
						axisMap[event->caxis.axis] = 0;
						gamepadEvent.type = GAMEPAD_AXIS_MOVE;
						gamepadEvent.axis = event->caxis.axis;
						gamepadEvent.axisValue = 0.0f;
						gamepadEvent.id = id;
						GamepadEvent::Dispatch(&gamepadEvent);
					}
					break;
				}

				axisMap[event->caxis.axis] = value;
				gamepadEvent.type = GAMEPAD_AXIS_MOVE;
				gamepadEvent.axis = event->caxis.axis;
				gamepadEvent.axisValue = value / (value > 0 ? 32767.0f : 32768.0f);
				gamepadEvent.id = id;
				GamepadEvent::Dispatch(&gamepadEvent);
				break;
			}

			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP: {
				gamepadEvent.type = (event->type == SDL_CONTROLLERBUTTONDOWN) ? GAMEPAD_BUTTON_DOWN : GAMEPAD_BUTTON_UP;
				gamepadEvent.button = event->cbutton.button;
				gamepadEvent.id = event->cbutton.which;
				GamepadEvent::Dispatch(&gamepadEvent);
				break;
			}

			case SDL_CONTROLLERDEVICEADDED:
				if (SDLGamepad::Connect(event->cdevice.which)) {
					gamepadEvent.type = GAMEPAD_CONNECT;
					gamepadEvent.id = SDLGamepad::GetInstanceID(event->cdevice.which);
					GamepadEvent::Dispatch(&gamepadEvent);
				}
				break;

			case SDL_CONTROLLERDEVICEREMOVED:
				gamepadEvent.type = GAMEPAD_DISCONNECT;
				gamepadEvent.id = event->cdevice.which;
				GamepadEvent::Dispatch(&gamepadEvent);
				SDLGamepad::Disconnect(event->cdevice.which);
				break;
		}
	}


	void SDLApplication::ProcessJoystickEvent(SDL_Event* event) {
		if (!JoystickEvent::callback) return;
		int id = event->jaxis.which; // adjust per event type

		switch (event->type) {
			case SDL_JOYAXISMOTION:
				if (SDLJoystick::IsAccelerometer(id)) return;
				joystickEvent.type = JOYSTICK_AXIS_MOVE;
				joystickEvent.id = id;
				joystickEvent.index = event->jaxis.axis;
				joystickEvent.x = event->jaxis.value / (event->jaxis.value > 0 ? 32767.0f : 32768.0f);
				JoystickEvent::Dispatch(&joystickEvent);
				break;

			case SDL_JOYBALLMOTION:
				if (SDLJoystick::IsAccelerometer(event->jball.which)) return;
				joystickEvent.type = JOYSTICK_TRACKBALL_MOVE;
				joystickEvent.id = event->jball.which;
				joystickEvent.index = event->jball.ball;
				joystickEvent.x = event->jball.xrel / 32767.0f;
				joystickEvent.y = event->jball.yrel / 32767.0f;
				JoystickEvent::Dispatch(&joystickEvent);
				break;

			case SDL_JOYBUTTONDOWN:
			case SDL_JOYBUTTONUP:
				if (SDLJoystick::IsAccelerometer(event->jbutton.which)) return;
				joystickEvent.type = (event->type == SDL_JOYBUTTONDOWN) ? JOYSTICK_BUTTON_DOWN : JOYSTICK_BUTTON_UP;
				joystickEvent.id = event->jbutton.which;
				joystickEvent.index = event->jbutton.button;
				JoystickEvent::Dispatch(&joystickEvent);
				break;

			case SDL_JOYHATMOTION:
				if (SDLJoystick::IsAccelerometer(event->jhat.which)) return;
				joystickEvent.type = JOYSTICK_HAT_MOVE;
				joystickEvent.id = event->jhat.which;
				joystickEvent.index = event->jhat.hat;
				joystickEvent.eventValue = event->jhat.value;
				JoystickEvent::Dispatch(&joystickEvent);
				break;

			case SDL_JOYDEVICEADDED:
				if (SDLJoystick::Connect(event->jdevice.which)) {
					joystickEvent.type = JOYSTICK_CONNECT;
					joystickEvent.id = SDLJoystick::GetInstanceID(event->jdevice.which);
					JoystickEvent::Dispatch(&joystickEvent);
				}
				break;

			case SDL_JOYDEVICEREMOVED:
				if (SDLJoystick::IsAccelerometer(event->jdevice.which)) return;
				joystickEvent.type = JOYSTICK_DISCONNECT;
				joystickEvent.id = event->jdevice.which;
				JoystickEvent::Dispatch(&joystickEvent);
				SDLJoystick::Disconnect(event->jdevice.which);
				break;
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


	void SDLApplication::ProcessTextEvent(SDL_Event* event) {
		if (!TextEvent::callback) return;

		textEvent.type = (event->type == SDL_TEXTINPUT) ? TEXT_INPUT : TEXT_EDIT;
		if (event->type == SDL_TEXTEDITING) {
			textEvent.start = event->edit.start;
			textEvent.length = event->edit.length;
		}

		strncpy((char*)textBuffer, event->text.text, sizeof(textBuffer) - 1);
		textBuffer[sizeof(textBuffer) - 1] = '\0';
		textEvent.text = textBuffer;
		textEvent.windowID = event->text.windowID;

		TextEvent::Dispatch(&textEvent);
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


	void SDLApplication::ProcessWindowEvent(SDL_Event* event) {
		if (!WindowEvent::callback) return;

		switch (event->window.event) {
			case SDL_WINDOWEVENT_SHOWN: windowEvent.type = WINDOW_SHOW; break;
			case SDL_WINDOWEVENT_CLOSE: windowEvent.type = WINDOW_CLOSE; break;
			case SDL_WINDOWEVENT_HIDDEN: windowEvent.type = WINDOW_HIDE; break;
			case SDL_WINDOWEVENT_FOCUS_GAINED: windowEvent.type = WINDOW_FOCUS_IN; break;
			case SDL_WINDOWEVENT_FOCUS_LOST: windowEvent.type = WINDOW_FOCUS_OUT; break;
			case SDL_WINDOWEVENT_EXPOSED:
			case SDL_WINDOWEVENT_SIZE_CHANGED:
				windowEvent.type = (event->window.event == SDL_WINDOWEVENT_EXPOSED) ? WINDOW_EXPOSE : WINDOW_RESIZE;
				break;
			case SDL_WINDOWEVENT_MOVED:
				windowEvent.type = WINDOW_MOVE;
				windowEvent.x = event->window.data1;
				windowEvent.y = event->window.data2;
				break;
			case SDL_WINDOWEVENT_RESTORED: windowEvent.type = WINDOW_RESTORE; break;
		}

		windowEvent.windowID = event->window.windowID;
		WindowEvent::Dispatch(&windowEvent);
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

	int64_t prevFrameTime = 0;

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

		/*printf("NtSetTimerResolution -> Status: 0x%08X, Current: %.3f ms\n",
			(unsigned int)status, current / 10000.0);*/
	}
	#endif

	int64_t getTime() {
		#ifdef HX_WINDOWS
		static LARGE_INTEGER freq = {0};
		static LARGE_INTEGER start = {0};

		if (freq.QuadPart == 0) {
			QueryPerformanceFrequency(&freq);
			QueryPerformanceCounter(&start);
		}

		LARGE_INTEGER counter;
		QueryPerformanceCounter(&counter);

		// Calculate elapsed ticks since start
		int64_t elapsed = counter.QuadPart - start.QuadPart;

		// Convert to microseconds without overflow
		return (elapsed * 1000000) / freq.QuadPart;
		#elif defined(__GNUC__) || defined(__clang__)
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);
		return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
		#else
		return std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now().time_since_epoch()
		).count();
		#endif
	}

	#if defined(__GNUC__) || defined(__clang__)
	// add microseconds to a timespec
	inline void timespecAddUs(struct timespec &ts, int64_t us) {
		ts.tv_nsec += (us % 1000000) * 1000;
		ts.tv_sec  += us / 1000000;
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_nsec -= 1000000000;
			ts.tv_sec++;
		}
	}
	#endif

	void coolSleepUntil(int64_t wakeTimeUs) {
		int64_t currentTime = getTime();
		int64_t sleepForUs = wakeTimeUs - currentTime;

		if (sleepForUs <= 0) return;

		#if HX_WINDOWS
		LARGE_INTEGER due;
		due.QuadPart = -sleepForUs * 10; // 100ns units, negative = relative
		SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
		WaitForSingleObject(timer, INFINITE);
		#elif defined(__GNUC__) || defined(__clang__)
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
	bool SDLApplication::Update() {
		int64_t currentTime = getTime();

		static double leftover = 0.0;

		// Poll events first (non-blocking)
		int64_t startPollTime = getTime();
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			HandleEvent(&event);
			if (!active) return active;
		}
		int64_t endPollTime = getTime();
		int64_t eventPollingOverhead = endPollTime - startPollTime;
		//printf("%lld\n", eventPollingOverhead);

		static int64_t baseTime = 0; // persistent base
		static int64_t updateCounter = 0;
		static int64_t renderCounter = 0;

		// Initialize on first run
		if (baseTime == 0) {
			baseTime = currentTime;
			prevFrameTime = currentTime;
		}

		// Detect long pauses or drift and adjust baseTime
		int64_t deltaTime = currentTime - prevFrameTime;
		if (deltaTime > 100000) {
			baseTime = currentTime - updateCounter * UPDATE_PERIOD;
		}

		prevFrameTime = currentTime;

		// Calculate next times from base
		double nextUpdateTime = baseTime + (updateCounter + 1) * UPDATE_PERIOD;
		double nextRenderTime = baseTime + (renderCounter + 1) * RENDER_PERIOD;

		// Process all due updates (with catch-up limit)
		int64_t startTime_process = getTime();
		int updateCount = 0;
		while (currentTime >= nextUpdateTime && updateCount < 4) {
			applicationEvent.type = UPDATE;
			applicationEvent.deltaTime = UPDATE_PERIOD;
			ApplicationEvent::Dispatch(&applicationEvent);
			printf("FUCK THIS MAN\n");

			updateCounter++;
			updateCount++;
			nextUpdateTime = baseTime + (updateCounter + 1) * UPDATE_PERIOD;
		}

		// Process render if due
		if (currentTime >= nextRenderTime) {
			renderEvent.type = RENDER;
			RenderEvent::Dispatch(&renderEvent);
			renderCounter++;
			nextRenderTime = baseTime + (renderCounter + 1) * RENDER_PERIOD;
		}
		int64_t endTime_process = getTime();

		#if HX_WINDOWS
		int64_t timerResolution = UPDATE_PERIOD - eventPollingOverhead;
		if (timerResolution < 500) timerResolution = UPDATE_PERIOD;

		adjustTimerResolutionDynamic(timerResolution);
		#endif

		// Sleep until next event, waking slightly early
		int64_t nextEventTime = std::min<int64_t>(nextUpdateTime, nextRenderTime);

		int64_t sleepUntil = nextEventTime;

		leftover = std::fmod((double)currentTime - (double)baseTime, UPDATE_PERIOD);

		if (sleepUntil > currentTime) {
			coolSleepUntil(sleepUntil);
		}

		//printf("%lld\n", (currentTime - baseTime) % UPDATE_PERIOD);

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
