#include <system/CFFI.h>
#include <ui/SubLoopTickEvent.h>


namespace lime {


	ValuePointer* SubLoopTickEvent::callback = 0;
	ValuePointer* SubLoopTickEvent::eventObject = 0;

	static int64_t id_timestamp;
	static bool init = false;


	SubLoopTickEvent::SubLoopTickEvent () {

		timestamp = 0LL;

	}


	void SubLoopTickEvent::Dispatch (SubLoopTickEvent* event) {

		if (SubLoopTickEvent::callback) {

			if (SubLoopTickEvent::eventObject->IsCFFIValue ()) {

				if (!init) {

					id_timestamp = val_id ("timestamp");
					init = true;

				}

				value object = (value)SubLoopTickEvent::eventObject->Get ();

				alloc_field (object, id_timestamp, alloc_int (event->timestamp));

			} else {

				SubLoopTickEvent* eventObject = (SubLoopTickEvent*)SubLoopTickEvent::eventObject->Get ();

				eventObject->timestamp = event->timestamp;

			}

			SubLoopTickEvent::callback->Call ();

		}

	}


}