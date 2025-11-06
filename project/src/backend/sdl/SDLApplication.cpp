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

// ---------------------------
// Lightweight POD event types
// ---------------------------
enum class NativeEventType : uint8_t {
    None = 0,
    KeyDown,
    KeyUp,
    MouseMove,
    MouseButtonDown,
    MouseButtonUp,
    MouseWheel,
    TextInput,
    ControllerButtonDown,
    ControllerButtonUp,
    ControllerAxisMotion,
    JoystickAxisMotion,
    WindowResized,
    WindowClose,
    DropFile,
    Quit
};

struct NativeEvent {
    NativeEventType type;
    int64_t timestampUs; // microseconds from getTime()
    int32_t data1;       // generic (keycode, button, axis index, etc)
    int32_t data2;       // generic
    int32_t x, y;        // position
    float   fdata;       // analog value
    char    text[16];    // small UTF-8 buffer (for TEXTINPUT or DROPFILE truncated)
};

// ---------------------------
// SPSC Ring buffer (POD only)
// ---------------------------
class InputRing {
public:
    InputRing(size_t capacity = 4096) : cap(capacity), head(0), tail(0) {
        buf = (NativeEvent*)malloc(sizeof(NativeEvent) * cap);
        // initialize to zero to be safe
        memset(buf, 0, sizeof(NativeEvent) * cap);
    }
    ~InputRing() { free(buf); }

    // push by value (called from SDL poll on main thread)
    bool push(const NativeEvent& e) {
        size_t next = (head + 1) % cap;
        if (next == tail) return false; // full
        buf[head] = e;
        head = next;
        return true;
    }

    // pop into out (called from Update() in the same thread too)
    bool pop(NativeEvent &out) {
        if (tail == head) return false; // empty
        out = buf[tail];
        tail = (tail + 1) % cap;
        return true;
    }

    void clear() { head = tail = 0; }
    bool empty() const { return head == tail; }

private:
    NativeEvent *buf;
    size_t cap;
    size_t head, tail;
};

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

		// Set process priority to real-time
		SetPriorityClass(GetCurrentProcess(), REALTIME_PRIORITY_CLASS);

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

	// ---------------------------
	// Global static ring instance
	// (allocated once at startup)
	// ---------------------------
	InputRing inputRing(4096); // tune capacity to expected burst sizes

	// ---------------------------
	// Convert SDL_Event -> NativeEvent
	// ---------------------------
	NativeEvent SDLApplication::ConvertSDLEventToNative(const SDL_Event &e, int64_t nowUs) {
		NativeEvent ne;
		ne.type = NativeEventType::None;
		ne.timestampUs = nowUs;
		ne.data1 = ne.data2 = 0;
		ne.x = ne.y = 0;
		ne.fdata = 0.0f;
		ne.text[0] = '\0';

		switch (e.type) {
			case SDL_KEYDOWN:
				ne.type = NativeEventType::KeyDown;
				ne.data1 = e.key.keysym.sym;
				ne.data2 = e.key.keysym.scancode;
				break;

			case SDL_KEYUP:
				ne.type = NativeEventType::KeyUp;
				ne.data1 = e.key.keysym.sym;
				ne.data2 = e.key.keysym.scancode;
				break;

			case SDL_MOUSEMOTION:
				ne.type = NativeEventType::MouseMove;
				ne.x = e.motion.x;
				ne.y = e.motion.y;
				ne.fdata = static_cast<float>(e.motion.xrel); // store last rel in fdata for convenience
				// we'll also use data2 to hold yrel as int if needed
				ne.data2 = e.motion.yrel;
				break;

			case SDL_MOUSEBUTTONDOWN:
				ne.type = NativeEventType::MouseButtonDown;
				ne.data1 = e.button.button;
				ne.x = e.button.x;
				ne.y = e.button.y;
				ne.data2 = e.button.clicks;
				ne.fdata = (float)e.button.state;
				break;

			case SDL_MOUSEBUTTONUP:
				ne.type = NativeEventType::MouseButtonUp;
				ne.data1 = e.button.button;
				ne.x = e.button.x;
				ne.y = e.button.y;
				ne.data2 = e.button.clicks;
				ne.fdata = (float)e.button.state;
				break;

			case SDL_MOUSEWHEEL:
				ne.type = NativeEventType::MouseWheel;
				ne.fdata = static_cast<float>(e.wheel.x);
				ne.data2 = e.wheel.y;
				break;

			case SDL_TEXTINPUT:
				ne.type = NativeEventType::TextInput;
				strncpy(ne.text, e.text.text, sizeof(ne.text)-1);
				ne.text[sizeof(ne.text)-1] = '\0';
				break;

			case SDL_CONTROLLERBUTTONDOWN:
				ne.type = NativeEventType::ControllerButtonDown;
				ne.data1 = e.cbutton.button;
				ne.data2 = e.cbutton.which;
				break;

			case SDL_CONTROLLERBUTTONUP:
				ne.type = NativeEventType::ControllerButtonUp;
				ne.data1 = e.cbutton.button;
				ne.data2 = e.cbutton.which;
				break;

			case SDL_CONTROLLERAXISMOTION:
				ne.type = NativeEventType::ControllerAxisMotion;
				ne.data1 = e.caxis.axis;
				ne.fdata = e.caxis.value / 32767.0f;
				ne.data2 = e.caxis.which;
				break;

			case SDL_JOYAXISMOTION:
				ne.type = NativeEventType::JoystickAxisMotion;
				ne.data1 = e.jaxis.axis;
				ne.fdata = e.jaxis.value / 32767.0f;
				ne.data2 = e.jaxis.which;
				break;

			case SDL_WINDOWEVENT:
				if (e.window.event == SDL_WINDOWEVENT_RESIZED || e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
					ne.type = NativeEventType::WindowResized;
					ne.data1 = e.window.data1;
					ne.data2 = e.window.data2;
					ne.x = e.window.windowID;
				} else if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
					ne.type = NativeEventType::WindowClose;
				} else {
					// map other window events into WindowResized/WindowClose or ignore
					ne.type = NativeEventType::None;
				}
				break;

			case SDL_DROPFILE:
				ne.type = NativeEventType::DropFile;
				if (e.drop.file) {
					// copy limited path into text (truncated) - no allocation
					strncpy(ne.text, e.drop.file, sizeof(ne.text)-1);
					ne.text[sizeof(ne.text)-1] = '\0';
					// SDL_malloc/SDL_free not needed here; SDL docs say event->drop.file must be freed by user via SDL_free
				}
				break;

			case SDL_QUIT:
				ne.type = NativeEventType::Quit;
				break;

			default:
				ne.type = NativeEventType::None;
				break;
		}

		return ne;
	}

	// ---------------------------
	// New lightweight handler
	// Converts NativeEvent -> your existing dispatch objects (allocation-free)
	// ---------------------------
	void SDLApplication::HandleNativeEvent(const NativeEvent &ne) {
		// NOTE: relies on existing global event structs (keyEvent, mouseEvent, gamepadEvent, etc.)
		// The original code in your file dispatches those same objects; we replicate that here
		switch (ne.type) {
			case NativeEventType::KeyDown:
			case NativeEventType::KeyUp:
				if (KeyEvent::callback) {
					keyEvent.type = (ne.type == NativeEventType::KeyDown) ? KEY_DOWN : KEY_UP;
					keyEvent.keyCode = ne.data1;
					keyEvent.modifier = 0; // we don't have modifiers from this POD; keep previous logic if needed
					// windowID not preserved in Key POD (could be added if required)
					KeyEvent::Dispatch(&keyEvent);
				}
				break;

			case NativeEventType::MouseMove:
				if (MouseEvent::callback) {
					mouseEvent.type = MOUSE_MOVE;
					mouseEvent.x = ne.x;
					mouseEvent.y = ne.y;
					mouseEvent.movementX = static_cast<int>(ne.fdata);
					mouseEvent.movementY = ne.data2;
					MouseEvent::Dispatch(&mouseEvent);
				}
				break;

			case NativeEventType::MouseButtonDown:
				if (MouseEvent::callback) {
					SDL_CaptureMouse(SDL_TRUE);
					mouseEvent.type = MOUSE_DOWN;
					mouseEvent.button = ne.data1 - 1;
					mouseEvent.x = ne.x;
					mouseEvent.y = ne.y;
					mouseEvent.clickCount = ne.data2;
					MouseEvent::Dispatch(&mouseEvent);
				}
				break;

			case NativeEventType::MouseButtonUp:
				if (MouseEvent::callback) {
					SDL_CaptureMouse(SDL_FALSE);
					mouseEvent.type = MOUSE_UP;
					mouseEvent.button = ne.data1 - 1;
					mouseEvent.x = ne.x;
					mouseEvent.y = ne.y;
					mouseEvent.clickCount = ne.data2;
					MouseEvent::Dispatch(&mouseEvent);
				}
				break;

			case NativeEventType::MouseWheel:
				if (MouseEvent::callback) {
					mouseEvent.type = MOUSE_WHEEL;
					// SDL wheel direction handled earlier; here we just set x/y deltas
					mouseEvent.x = static_cast<int>(ne.fdata);
					mouseEvent.y = ne.data2;
					MouseEvent::Dispatch(&mouseEvent);
				}
				break;

			case NativeEventType::TextInput:
				if (TextEvent::callback) {
					textEvent.type = TEXT_INPUT;
					// we store pointer to static small buffer — textEvent expects vbyte*; ensure static lifetime
					static char smallTextBuf[64];
					strncpy(smallTextBuf, ne.text, sizeof(smallTextBuf)-1);
					smallTextBuf[sizeof(smallTextBuf)-1] = '\0';
					textEvent.text = (vbyte*)smallTextBuf;
					TextEvent::Dispatch(&textEvent);
				}
				break;

			case NativeEventType::ControllerAxisMotion:
			case NativeEventType::JoystickAxisMotion:
				if (GamepadEvent::callback) {
					gamepadEvent.type = GAMEPAD_AXIS_MOVE;
					gamepadEvent.axis = ne.data1;
					gamepadEvent.id = ne.data2;
					// apply deadzone logic as in original
					if (ne.fdata > -analogAxisDeadZone/32767.0f && ne.fdata < analogAxisDeadZone/32767.0f) {
						// small movement — send zero if previously non-zero
						if (gamepadsAxisMap[gamepadEvent.id].count(gamepadEvent.axis) && gamepadsAxisMap[gamepadEvent.id][gamepadEvent.axis] != 0) {
							gamepadsAxisMap[gamepadEvent.id][gamepadEvent.axis] = 0;
							gamepadEvent.axisValue = 0;
							GamepadEvent::Dispatch(&gamepadEvent);
						}
					} else {
						// store scaled integer for change detection
						int scaled = static_cast<int>(ne.fdata * 32767.0f);
						if (gamepadsAxisMap[gamepadEvent.id][gamepadEvent.axis] == scaled) break;
						gamepadsAxisMap[gamepadEvent.id][gamepadEvent.axis] = scaled;
						gamepadEvent.axisValue = ne.fdata;
						GamepadEvent::Dispatch(&gamepadEvent);
					}
				}
				break;

			case NativeEventType::ControllerButtonDown:
			case NativeEventType::ControllerButtonUp:
				if (GamepadEvent::callback) {
					gamepadEvent.type = (ne.type == NativeEventType::ControllerButtonDown) ? GAMEPAD_BUTTON_DOWN : GAMEPAD_BUTTON_UP;
					gamepadEvent.button = ne.data1;
					gamepadEvent.id = ne.data2;
					GamepadEvent::Dispatch(&gamepadEvent);
				}
				break;

			case NativeEventType::WindowResized:
				if (WindowEvent::callback) {
					windowEvent.type = WINDOW_RESIZE;
					windowEvent.width = ne.data1;
					windowEvent.height = ne.data2;
					WindowEvent::Dispatch(&windowEvent);
				}
				break;

			case NativeEventType::WindowClose:
				if (WindowEvent::callback) {
					windowEvent.type = WINDOW_CLOSE;
					WindowEvent::Dispatch(&windowEvent);
					// If window close should exit main loop:
					// active = false; // careful: this modifies outer symbol; uncomment only if desired
				}
				break;

			case NativeEventType::DropFile:
				if (DropEvent::callback) {
					dropEvent.type = DROP_FILE;
					static char dropBuf[256];
					strncpy(dropBuf, ne.text, sizeof(dropBuf)-1);
					dropBuf[sizeof(dropBuf)-1] = '\0';
					dropEvent.file = (vbyte*)dropBuf;
					DropEvent::Dispatch(&dropEvent);
					// Do NOT call SDL_free here — we didn't allocate; owner is static buffer
				}
				break;

			case NativeEventType::Quit:
				// keep same behavior as SDL_QUIT
				active = false;
				break;

			default:
				break;
		}
	}

	// ---------------------------
	// Poll SDL -> convert -> push POD to ring
	// (call this on the main thread frequently, before Update/Render decision)
	// ---------------------------
	void SDLApplication::PollAndEnqueueSDLEvents() {
		SDL_Event e;
		// timestamp at start of poll batch (microseconds)
		int64_t nowUs = getTime();

		while (SDL_PollEvent(&e)) {
			bool isInputEvent = false;
			switch (e.type) {
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
				case SDL_QUIT:
					isInputEvent = true;
					break;
				default:
					isInputEvent = false;
					break;
			}

			if (isInputEvent) {
				NativeEvent ne = ConvertSDLEventToNative(e, nowUs);
				if (ne.type == NativeEventType::None) {
					// skip, not interesting
					continue;
				}
				if (!inputRing.push(ne)) {
					// ring full — drop oldest or warn
	#ifdef _DEBUG
					printf("[WARN] Input ring full, dropping event type %u\n", (unsigned)ne.type);
	#endif
				}

				// For dropfile, SDL gives ownership of string to us via e.drop.file.
				// Since we copied path into ne.text (truncated), we must free SDL's allocated string.
				if (e.type == SDL_DROPFILE) {
					SDL_free(e.drop.file);
				}
			} else {
				// Non-input events handled immediately (app lifecycle, render reset, etc)
				HandleEvent(&e);
				if (!active) return;
			}
		}
	}

	// ---------------------------
	// Process (consume) NativeEvents during UPDATE.
	// Includes optional batching / averaging for mouse motion.
	// ---------------------------
	void SDLApplication::ProcessNativeEventsForUpdate(int maxEventsPerUpdate = 256) {
		NativeEvent ne;
		int processed = 0;

		// Mouse batching accumulator
		bool haveMouse = false;
		int accumX = 0, accumY = 0, accumMoves = 0;
		int lastMouseX = 0, lastMouseY = 0;

		while (inputRing.pop(ne)) {
			// Mouse motion batching: accumulate multiple motions and dispatch a single averaged move
			if (ne.type == NativeEventType::MouseMove) {
				haveMouse = true;
				accumX += ne.x;
				accumY += ne.y;
				accumMoves++;
				lastMouseX = ne.x;
				lastMouseY = ne.y;
			} else {
				// If we have pending mouse moves, dispatch them first (averaged)
				if (haveMouse) {
					NativeEvent avg;
					avg.type = NativeEventType::MouseMove;
					avg.timestampUs = ne.timestampUs;
					avg.x = accumX / accumMoves;
					avg.y = accumY / accumMoves;
					avg.fdata = 0.0f;
					HandleNativeEvent(avg);
					haveMouse = false;
					accumMoves = 0;
					accumX = accumY = 0;
				}

				HandleNativeEvent(ne);
			}

			processed++;
		}

		// if we exited loop with pending mouse moves, flush them
		if (haveMouse) {
			NativeEvent avg;
			avg.type = NativeEventType::MouseMove;
			avg.timestampUs = getTime();
			avg.x = (accumMoves > 0) ? (accumX / accumMoves) : lastMouseX;
			avg.y = (accumMoves > 0) ? (accumY / accumMoves) : lastMouseY;
			HandleNativeEvent(avg);
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

		// Poll SDL and enqueue input PODs first
		PollAndEnqueueSDLEvents();

		static int64_t baseTime = 0;
		static int64_t updateCounter = 0;
		static int64_t renderCounter = 0;

		if (baseTime == 0) {
			baseTime = currentTime;
			prevFrameTime = currentTime;
			inputRing.clear();
		}

		int64_t deltaTime = currentTime - prevFrameTime;
		if (deltaTime > 100000) {
			baseTime = currentTime - updateCounter * UPDATE_PERIOD;
		}

		prevFrameTime = currentTime;

		double nextUpdateTime = baseTime + (updateCounter + 1) * UPDATE_PERIOD;
		double nextRenderTime = baseTime + (renderCounter + 1) * RENDER_PERIOD;

		if (currentTime >= nextUpdateTime) {
			ProcessNativeEventsForUpdate(512); // tune per frame budget

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

		coolSleepUntil(nextEventTime);

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
