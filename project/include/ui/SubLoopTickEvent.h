#ifndef LIME_UI_SUB_LOOP_TICK_EVENT_H
#define LIME_UI_SUB_LOOP_TICK_EVENT_H


#include <system/CFFI.h>
#include <system/ValuePointer.h>
#include <stdint.h>


namespace lime {


	struct SubLoopTickEvent {

		hl_type* t;
		int64_t timestamp;

		static ValuePointer* callback;
		static ValuePointer* eventObject;

		SubLoopTickEvent ();

		static void Dispatch (SubLoopTickEvent* event);

	};


}


#endif