/**
 * This class is where the main loop goes. For one, windows 10;
 * The said main loop uses:
   - A combination of high res waitable timer and `updateOffset` to create a surreal rhythm game experience!
 * On the other hand, linux just already has an accurate sleep function. I wanted to create a fun crispy smooth experience for literally everyone who are on windows,
 so that meant doing this to compensate. How about I make a literal main loop library out of this?

 * Note: `updateOffset`, which naturally increases cpu usage in case you didn't know already, and is a tradeoff to the pure main loop accuracy it has.
**/

#include "SDLApplication.h"
#include "SDLGamepad.h"
#include "SDLJoystick.h"
#include "SDLWindow.h"
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

// YAY

#include <iostream>
#include <stdexcept>

#ifdef _WIN32
	#include <windows.h>
#else
	#include <unistd.h>
	#include <sched.h>
	#ifdef __linux__
	#include <X11/Xlib.h>
	#include <GL/gl.h>
	#include <GL/glx.h>
	#include <cstring> // for strstr
	#include <dlfcn.h> // for dynamic loading

	// Define RTLD_LAZY if not already defined (for some older systems)
	#ifndef RTLD_LAZY
	#define RTLD_LAZY 1
	#endif

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

class CPUAffinity {
public:
	static int getNumCores() {
#ifdef _WIN32
		SYSTEM_INFO sysinfo;
		GetSystemInfo(&sysinfo);
		return sysinfo.dwNumberOfProcessors;
#elif __APPLE__
		return std::thread::hardware_concurrency();
#elif __ANDROID__
		return sysconf(_SC_NPROCESSORS_ONLN);
#else // Linux
		return sysconf(_SC_NPROCESSORS_ONLN);
#endif
	}

	static void pinToLastTwoCores() {
		int numCores = getNumCores();
		if (numCores < 3) {
			printf("System has fewer than 3 cores. Cancelling by now.");
			return;
		}

		int core1 = numCores - 2;
		int core2 = numCores - 1;

		pinToCore(core1, core2);
	}

	static void pinToCore(int core1, int core2) {
#ifdef _WIN32
		pinToCore_Windows(core1, core2);
#elif __APPLE__
		pinToCore_macOS(core1, core2);
#elif __ANDROID__
		pinToCore_Android(core1, core2);
#else
		pinToCore_Linux(core1, core2);
#endif
	}

private:
#ifdef _WIN32
	static void pinToCore_Windows(int core1, int core2) {
		DWORD mask = (1ULL << core1) | (1ULL << core2);
		if (!SetThreadAffinityMask(GetCurrentThread(), mask)) {
			throw std::runtime_error("Failed to set thread affinity on Windows");
		}
		std::cout << "Pinned to cores " << (core1+1) << " and " << (core2+1)
				  << " on Windows" << std::endl;
	}

#elif __APPLE__
	static void pinToCore_macOS(int core1, int core2) {
		// macOS has limited CPU affinity support at the thread level
		// This uses thread_policy_set, but macOS may not honor it strictly
		thread_extended_policy_data_t policy;
		policy.timeshare = 0;

		thread_port_t thread = mach_thread_self();
		kern_return_t kr = thread_policy_set(
			thread,
			THREAD_EXTENDED_POLICY,
			(thread_policy_t)&policy,
			THREAD_EXTENDED_POLICY_COUNT
		);

		mach_port_deallocate(mach_task_self(), thread);

		if (kr != KERN_SUCCESS) {
			throw std::runtime_error("Failed to set thread policy on macOS");
		}
		std::cout << "Attempted to optimize for cores " << (core1+1) << " and "
				  << (core2+1) << " on macOS (limited support)" << std::endl;
	}

#elif __ANDROID__
	static void pinToCore_Android(int core1, int core2) {
		// Android uses the same Linux kernel, so sched_setaffinity works
		// However, some devices may have restrictions
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(core1, &set);
		CPU_SET(core2, &set);

		if (sched_setaffinity(0, sizeof(set), &set) == -1) {
			// On some Android devices, affinity may fail due to SELinux or permissions
			// Log warning but don't fail completely
			std::cerr << "Warning: Failed to set thread affinity on Android. "
					  << "This may require special permissions or root access." << std::endl;
			return;
		}
		std::cout << "Pinned to cores " << (core1+1) << " and " << (core2+1)
				  << " on Android" << std::endl;
	}

#else
	static void pinToCore_Linux(int core1, int core2) {
		cpu_set_t set;
		CPU_ZERO(&set);
		CPU_SET(core1, &set);
		CPU_SET(core2, &set);

		if (sched_setaffinity(0, sizeof(set), &set) == -1) {
			throw std::runtime_error("Failed to set thread affinity on Linux");
		}
		std::cout << "Pinned to cores " << (core1+1) << " and " << (core2+1)
				  << " on Linux" << std::endl;
	}
#endif
};

namespace lime {

	AutoGCRoot* Application::callback = 0;
	SDLApplication* SDLApplication::currentApplication = 0;

	const int analogAxisDeadZone = 1000;
	std::map<int, std::map<int, int> > gamepadsAxisMap;
	bool inBackground = false;

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

	// ---------- Timing configuration in 10ns ticks ----------
	// 1 second = 100000000 ticks of 10ns
	constexpr int64_t TICKS_PER_SECOND_10NS = 100000000LL;

	// Default target frame rates
	static int64_t UPDATE_PERIOD_10NS = TICKS_PER_SECOND_10NS / 120LL; // default update period (e.g. 120Hz)
	static int64_t RENDER_PERIOD_10NS = TICKS_PER_SECOND_10NS / 60LL;  // default render period (60Hz)

	// For cross-platform best sleep implementations (SDL3's SDL_DelayPrecise uses the same thing
	// except it does spinlock but this uses a high-precision waitable timer which has basically 10us of granularity)
	// And for cohesion sake it's 10 microseconds since linux has an accurate sleep implementation already
	// and it's nuts that windows can even handle 10us of sleep at minimum without throttling the cpu so yeah that's that
	// turned it if you do this every 10 microconds it would start throttling performance on linux and android so yeah I reduced the precision to 100 microseconds to be safe
	static int64_t TILES_PER_TICK_10NS = TICKS_PER_SECOND_10NS / 10000LL; // 100us

	#if HX_WINDOWS
	static HANDLE timer;
	#endif

	// Add these near the top of your file with other static variables
	static int64_t lastRenderTime = 0;
	static int64_t render_timestamp = 0;

	#if HX_ANDROID
	// Global choreographer state
	static AChoreographer* choreographer = nullptr;
	static bool shouldRenderFromCallback = false;

	static void choreographer_callback(long frameTimeNanos, void* data) {
		shouldRenderFromCallback = true;
		// Convert nanoseconds to 10ns ticks
		int64_t frameTime10ns = frameTimeNanos / 10;
		render_timestamp = frameTime10ns - lastRenderTime;
		lastRenderTime = frameTime10ns;
	}
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

	// ----------------- 10ns timestamp helpers -----------------
	// Returns monotonic timestamp in 10-ns ticks
	int64_t getTime10ns() {
	#ifdef HX_WINDOWS
		static LARGE_INTEGER freq = {};
		static LARGE_INTEGER start = {};

		LARGE_INTEGER now;

		QueryPerformanceFrequency(&freq);
		QueryPerformanceCounter(&now);

		int64_t delta = (now.QuadPart - start.QuadPart) * TICKS_PER_SECOND_10NS;
		return (int64_t)(delta / freq.QuadPart);

	#else
		struct timespec ts;
		clock_gettime(CLOCK_MONOTONIC, &ts);

		return ts.tv_sec * TICKS_PER_SECOND_10NS + (ts.tv_nsec / 10LL);
	#endif
	}


	int SDLApplication::Exec () {

		Init ();

		try {
			int numCores = CPUAffinity::getNumCores();
			std::cout << "System has " << numCores << " cores" << std::endl;

			CPUAffinity::pinToLastTwoCores();

			// Your work here
			std::cout << "Process affinity set successfully" << std::endl;
		} catch (const std::exception& e) {
			std::cerr << "Error: " << e.what() << std::endl;
			return 1;
		}

		#ifdef EMSCRIPTEN
		emscripten_cancel_main_loop ();
		#endif

		while (active) {

			Update ();

		}

		return 0;

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

		SDL_QuitSubSystem (initFlags);

		SDL_Quit ();

		alreadyQuit = true;

		return 0;

	}


	void SDLApplication::RegisterWindow (SDLWindow *window) {

		#ifdef IPHONE
		SDL_iPhoneSetAnimationCallback (SDLWindow::sdlWindow, 1, Update, NULL);
		#endif

	}


	void SDLApplication::SetFrameRate (double frameRate) {

		if (frameRate > 0) {

			UPDATE_PERIOD_10NS = TICKS_PER_SECOND_10NS / frameRate;
			RENDER_PERIOD_10NS = TICKS_PER_SECOND_10NS / 60.0;

		} else {

			UPDATE_PERIOD_10NS = 0;
			RENDER_PERIOD_10NS = 0;

		}

	}


	void SDLApplication::SetRenderFrameRate (double renderFrameRate) {

		if (renderFrameRate > 60) {

			RENDER_PERIOD_10NS = TICKS_PER_SECOND_10NS / renderFrameRate;

		} else if (renderFrameRate == 0) {

			RENDER_PERIOD_10NS = 0.0;

		} else {

			RENDER_PERIOD_10NS = TICKS_PER_SECOND_10NS / 60.0;

		}

	}

	// Sleep until wakeTime10ns (10ns ticks) using the platform's monotonic sleep; does not mix clock domains.
	void coolSleepUntil10ns(int64_t wakeTime10ns) {
		int64_t currentTime = getTime10ns();
		int64_t sleepForTicks = wakeTime10ns - currentTime;
		if (sleepForTicks <= 0) return;

	#if HX_WINDOWS
		// SetWaitableTimer uses 100-ns units for LARGE_INTEGER; relative time is negative.
		LARGE_INTEGER due = {};
		// round to nearest 10ns
		long long relative = - (long long) (sleepForTicks / 10); // already in 10ns ticks; negative => relative
		due.QuadPart = relative;

		BOOL ok = SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
		if (!ok) {
			// fallback coarse sleep in milliseconds (best-effort)
			DWORD ms = (DWORD)((sleepForTicks * 10) / (int)TICKS_PER_SECOND_10NS + 1); // sleepForTicks *100 ns -> nanoseconds -> ms
			if (ms > 0) Sleep(ms);
		} else {
			WaitForSingleObject(timer, INFINITE);
		}
	#elif defined(HX_LINUX)
		struct timespec wake;
		// convert 10ns ticks into seconds/nsec
		wake.tv_sec = wakeTime10ns / TICKS_PER_SECOND_10NS;
		long long remainder10ns = wakeTime10ns % TICKS_PER_SECOND_10NS;
		wake.tv_nsec = (long)(remainder10ns * 10); // 10ns -> ns
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &wake, nullptr);
	#else
		auto target = std::chrono::steady_clock::time_point(std::chrono::nanoseconds(wakeTime10ns * 10));
		std::this_thread::sleep_until(target);
	#endif
	}

	int64_t startTimestamp10ns = 0;

	void SDLApplication::Init () {
		active = true;

		#ifdef HX_WINDOWS
		if (!timer) {
			timer = CreateWaitableTimerEx(nullptr, nullptr,
				CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_MODIFY_STATE | SYNCHRONIZE);
			if (!timer) {
				std::cout << "Failed to create high-res timer, using regular timer\n";
				timer = CreateWaitableTimer(nullptr, TRUE, nullptr);
				TILES_PER_TICK_10NS = TICKS_PER_SECOND_10NS / 1000LL;
			} else {
				std::cout << "Successfully created high-res timer!\n";
			}
		}

		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
		#endif

		#ifdef HX_ANDROID
		if (!choreographer) {
	choreographer = AChoreographer_getInstance();
	if (choreographer) {
		AChoreographer_postFrameCallback(choreographer,
										choreographer_callback,
										nullptr);
	}
		}
		#endif
	}

	void SDLApplication::PollInputs() {
		SDL_Event event;
		while (SDL_PollEvent(&event)) {
			bool isInputEvent = false;
			switch (event.type) {
				case SDL_KEYDOWN: case SDL_KEYUP:
				case SDL_MOUSEMOTION: case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: case SDL_MOUSEWHEEL:
				case SDL_FINGERMOTION: case SDL_FINGERDOWN: case SDL_FINGERUP:
				case SDL_TEXTINPUT: case SDL_TEXTEDITING:
				case SDL_JOYBALLMOTION: case SDL_JOYBUTTONDOWN: case SDL_JOYBUTTONUP: case SDL_JOYHATMOTION:
				case SDL_JOYDEVICEADDED: case SDL_JOYDEVICEREMOVED:
				case SDL_JOYAXISMOTION:
				case SDL_CONTROLLERAXISMOTION: case SDL_CONTROLLERBUTTONDOWN: case SDL_CONTROLLERBUTTONUP:
				case SDL_CONTROLLERDEVICEADDED: case SDL_CONTROLLERDEVICEREMOVED:
				case SDL_CLIPBOARDUPDATE:
					isInputEvent = true;
					break;
			}
			if (isInputEvent) {
				HandleInputEvent(&event);
			} else {
				HandleEvent(&event);
			}
		}
	}

	bool SDLApplication::Update() {
		static int64_t nextUpdateTime10ns = 0;
		static int64_t nextRenderTime10ns = 0;
		static int64_t lastRenderTime = getTime10ns();
		static int64_t renderCounter = 0;
		static bool firstFrame = true;
		static unsigned int lastVBlankCounter = 0; // ← ADD THIS for Linux

		bool vsyncEnabled = SDLWindow::vsync;

		int64_t now10ns = getTime10ns();

		// Initialize timing on FIRST frame only
		if (firstFrame) {
			startTimestamp10ns = now10ns;
			nextUpdateTime10ns = now10ns + UPDATE_PERIOD_10NS;
			nextRenderTime10ns = now10ns + RENDER_PERIOD_10NS;
			firstFrame = false;
		}

		// --- Fixed scheduling with drift correction ---
		if (!vsyncEnabled) coolSleepUntil10ns(now10ns + TILES_PER_TICK_10NS);
		now10ns = getTime10ns();

		subLoopTickEvent.timestamp = getTime10ns();
		SubLoopTickEvent::Dispatch(&subLoopTickEvent);

		int64_t updateRefreshRate = UPDATE_PERIOD_10NS;

		if (vsyncEnabled) {
			SDL_DisplayMode currentMode;
			if (SDL_GetCurrentDisplayMode(0, &currentMode) != 0) {
				std::cerr << "Could not get display mode! SDL_Error: " << SDL_GetError() << std::endl;
				active = false;
				return active;
			}
			double refreshRate = currentMode.refresh_rate;
			if (refreshRate == 0) refreshRate = 60;
			updateRefreshRate = TICKS_PER_SECOND_10NS / refreshRate;
		}

		// --- Render scheduling ---
		bool shouldRender = false;

		#ifdef HX_WINDOWS
		static int64_t qpcVBlank = 0;
		// Use DWM composition timing for precise VSync synchronization
		static DWM_TIMING_INFO timingInfo = {};
		timingInfo.cbSize = sizeof(DWM_TIMING_INFO);

		HRESULT hr = DwmGetCompositionTimingInfo(NULL, &timingInfo);

		if (SUCCEEDED(hr)) {
			if (qpcVBlank == 0 || qpcVBlank != timingInfo.qpcVBlank) {
				shouldRender = true;
				render_timestamp = (timingInfo.qpcVBlank - qpcVBlank) * 10LL;
				qpcVBlank = timingInfo.qpcVBlank;
			}
		} else {
			// Fallback timer-based approach
			shouldRender = (now10ns >= nextRenderTime10ns);
			if (shouldRender) {
				render_timestamp = now10ns - lastRenderTime;
				lastRenderTime = now10ns;
				nextRenderTime10ns += RENDER_PERIOD_10NS;
			}
		}
		#elif defined(HX_LINUX)
		static bool glxExtensionsChecked = false;
		static bool glxVideoSyncSupported = false;
		static int (*glXGetVideoSyncSGI_ptr)(unsigned int*) = nullptr;
		static int (*glXWaitVideoSyncSGI_ptr)(int, int, unsigned int*) = nullptr;

		// Check and load GLX extensions on first use
		if (!glxExtensionsChecked) {
			Display* display = XOpenDisplay(NULL);
			if (display) {
				const char* extensions = glXQueryExtensionsString(display, DefaultScreen(display));
				if (extensions && strstr(extensions, "GLX_SGI_video_sync")) {
					// Dynamically load the extension functions
					void* libGL = dlopen("libGL.so.1", RTLD_LAZY);
					if (libGL) {
						glXGetVideoSyncSGI_ptr = (int (*)(unsigned int*))dlsym(libGL, "glXGetVideoSyncSGI");
						glXWaitVideoSyncSGI_ptr = (int (*)(int, int, unsigned int*))dlsym(libGL, "glXWaitVideoSyncSGI");
						
						if (glXGetVideoSyncSGI_ptr && glXWaitVideoSyncSGI_ptr) {
							glxVideoSyncSupported = true;
							std::cout << "GLX_SGI_video_sync extension supported" << std::endl;
						} else {
							std::cout << "GLX_SGI_video_sync symbols not found" << std::endl;
						}
						// Don't close the library - we need the symbols
					} else {
						std::cout << "Could not load libGL.so.1" << std::endl;
					}
				} else {
					std::cout << "GLX_SGI_video_sync extension not available" << std::endl;
				}
				XCloseDisplay(display);
			}
			glxExtensionsChecked = true;
		}

		if (glxVideoSyncSupported && glXGetVideoSyncSGI_ptr) {
			unsigned int vblankCount = 0;
			if (glXGetVideoSyncSGI_ptr(&vblankCount) == 0) {
				if (vblankCount != lastVBlankCounter) {
					shouldRender = true;
					render_timestamp = getTime10ns() - lastRenderTime;
					lastRenderTime = getTime10ns();
					lastVBlankCounter = vblankCount;
				}
			} else {
				// Fallback if glXGetVideoSyncSGI fails
				shouldRender = (now10ns >= nextRenderTime10ns);
				if (shouldRender) {
					render_timestamp = now10ns - lastRenderTime;
					lastRenderTime = now10ns;
					nextRenderTime10ns += RENDER_PERIOD_10NS;
				}
			}
		} else {
			// Fallback timer-based approach
			shouldRender = (now10ns >= nextRenderTime10ns);
			if (shouldRender) {
				render_timestamp = now10ns - lastRenderTime;
				lastRenderTime = now10ns;
				nextRenderTime10ns += RENDER_PERIOD_10NS;
			}
		}
		#elif defined(HX_ANDROID)
		// Android VSync detection
		if (choreographer) {
			if (shouldRenderFromCallback) {
				shouldRender = true;
				shouldRenderFromCallback = false;
				// Re-register for next frame
				AChoreographer_postFrameCallback(choreographer,
												choreographer_callback,
												nullptr);
			}
		} else {
			// Fallback timer-based approach
			shouldRender = (now10ns >= nextRenderTime10ns);
			if (shouldRender) {
				render_timestamp = now10ns - lastRenderTime;
				lastRenderTime = now10ns;
				nextRenderTime10ns += RENDER_PERIOD_10NS;
			}
		}
		#else
		// Other platforms use the original logic
		shouldRender = (now10ns >= nextRenderTime10ns);
		if (shouldRender) {
			render_timestamp = RENDER_PERIOD_10NS;
			nextRenderTime10ns += RENDER_PERIOD_10NS;
		}
		#endif

		if (shouldRender || vsyncEnabled) {
			applicationEvent.type = UPDATE;
			applicationEvent.deltaTime = render_timestamp;
			ApplicationEvent::Dispatch(&applicationEvent);

			renderEvent.type = RENDER;
			RenderEvent::Dispatch(&renderEvent);
		}

		PollInputs();

		return active;
	}

	Application* CreateApplication () {

		return new SDLApplication ();

	}


}


#ifdef ANDROID
int SDL_main (int argc, char *argv[]) { return 0; }
#endif
