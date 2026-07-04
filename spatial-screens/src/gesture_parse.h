#pragma once

#include <string>
#include "gesture_client.h"  // GestureEvent

// Parse one newline-delimited gesture event (see gestures/protocol.py's
// encode_event) into a GestureEvent. Hand-rolled key-scanners for this
// fixed, both-ends-owned schema — deliberately no JSON library. Extracted
// from gesture_client.cpp so it is unit-testable without the socket layer.
GestureEvent parse_event(const std::string& line);
