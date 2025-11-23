#ifndef LIME_SDL_APPLICATION_H
#define LIME_SDL_APPLICATION_H


#include <SDL.h>
#include <app/Application.h>
#include <app/ApplicationEvent.h>
#include <graphics/RenderEvent.h>
#include <ui/SubLoopTickEvent.h>
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


namespace lime {


	class SDLApplication : public Application {

		public:

			SDLApplication ();
			~SDLApplication ();

			int Exec ();
			void Init ();
			int Quit ();
			void SetFrameRate (double frameRate);
			void SetRenderFrameRate (double renderFrameRate);
			void PollInputs ();
			bool Update ();

			void RegisterWindow (SDLWindow *window);

		private:

			void HandleEvent (SDL_Event* event);
			void HandleInputEvent (SDL_Event* event);
			void ProcessClipboardEvent (SDL_Event* event);
			void ProcessDropEvent (SDL_Event* event);
			void ProcessGamepadEvent (SDL_Event* event);
			void ProcessJoystickEvent (SDL_Event* event);
			void ProcessKeyEvent (SDL_Event* event);
			void ProcessMouseEvent (SDL_Event* event);
			void ProcessSensorEvent (SDL_Event* event);
			void ProcessTextEvent (SDL_Event* event);
			void ProcessTouchEvent (SDL_Event* event);
			void ProcessWindowEvent (SDL_Event* event);

			static SDLApplication* currentApplication;

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

			bool active;

	};


}


#endif