#ifndef LIME_UI_ASYNC_KEY_EVENT_H
#define LIME_UI_ASYNC_KEY_EVENT_H


#include <system/CFFI.h>
#include <system/ValuePointer.h>
#include <stdint.h>


namespace lime {


	struct AsyncKeyEvent {

		hl_type* t;
		int keyCode;
		int state;
		double timestamp;

		static ValuePointer* callback;
		static ValuePointer* eventObject;

		AsyncKeyEvent ();

		static void Dispatch (AsyncKeyEvent* event);

	};


}


#endif