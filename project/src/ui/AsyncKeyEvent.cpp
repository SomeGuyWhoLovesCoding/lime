#include <system/CFFI.h>
#include <ui/AsyncKeyEvent.h>
#include <chrono>


namespace lime {


	ValuePointer* AsyncKeyEvent::callback = 0;
	ValuePointer* AsyncKeyEvent::eventObject = 0;

	static int id_keyCode;
	static int id_state;
	static int id_timestamp;
	static bool init = false;


	AsyncKeyEvent::AsyncKeyEvent () {

		keyCode = 0;
		state = 0;
		timestamp = 0.0;

	}


	void AsyncKeyEvent::Dispatch (AsyncKeyEvent* event) {

		if (AsyncKeyEvent::callback) {

			if (AsyncKeyEvent::eventObject->IsCFFIValue ()) {

				if (!init) {

					id_keyCode = val_id ("keyCode");
					id_state = val_id ("state");
					id_timestamp = val_id ("timestamp");
					init = true;

				}

				value object = (value)AsyncKeyEvent::eventObject->Get ();

				alloc_field (object, id_keyCode, alloc_int (event->keyCode));
				alloc_field (object, id_state, alloc_int (event->state));
				alloc_field (object, id_timestamp, alloc_float (event->timestamp));

			} else {

				AsyncKeyEvent* eventObject = (AsyncKeyEvent*)AsyncKeyEvent::eventObject->Get ();

				eventObject->keyCode = event->keyCode;
				eventObject->state = event->state;
				eventObject->timestamp = event->timestamp;

			}

			AsyncKeyEvent::callback->Call ();

		}

	}

	double AsyncKeyEvent::Timestamp() {
		auto now = std::chrono::steady_clock::now();
		auto elapsed = std::chrono::duration<double>(now.time_since_epoch());
		return elapsed.count();
	}


}