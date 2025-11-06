#ifndef LIME_SDL_APPLICATION_H
#define LIME_SDL_APPLICATION_H


#include <SDL.h>
#include <app/Application.h>
#include <app/ApplicationEvent.h>
#include <graphics/RenderEvent.h>
#include <system/ClipboardEvent.h>
#include <system/SensorEvent.h>
#include <ui/DropEvent.h>
#include <ui/GamepadEvent.h>
#include <ui/JoystickEvent.h>
#include <ui/KeyEvent.h>
#include <ui/MouseEvent.h>
#include <ui/TextEvent.h>
#include <ui/TouchEvent.h>
#include <ui/WindowEvent.h>
#include "SDLWindow.h"

struct NativeEvent; // forward declaration, or include where it's defined

namespace lime {


	class SDLApplication : public Application {

		public:

			SDLApplication ();
			~SDLApplication ();

			virtual int Exec ();
			virtual void Init ();
			virtual int Quit ();
			virtual void SetFrameRate (double frameRate);
			virtual void SetRenderFrameRate (double renderFrameRate);
			virtual bool Update ();

			void RegisterWindow (SDLWindow *window);

			void HandleEvent (SDL_Event* event);
			NativeEvent ConvertSDLEventToNative (const SDL_Event &e, int64_t nowUs);
			void HandleNativeEvent (const NativeEvent &ne);
			void PollAndEnqueueSDLEvents (void);
			void ProcessNativeEventsForUpdate (int maxEventsPerUpdate);

		private:

			static void UpdateFrame ();
			static void UpdateFrame (void*);

			static SDLApplication* currentApplication;

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

			double lastUpdate;
			bool active;

	};


}


#endif