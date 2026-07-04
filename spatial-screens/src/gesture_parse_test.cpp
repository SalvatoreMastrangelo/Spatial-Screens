// Standalone unit test for the hand-gesture event parser. No framework:
// asserts via CHECK, returns non-zero on any failure. Build+run with
//   make gesture-parse-test && ./gesture-parse-test
#include "gesture_parse.h"
#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main() {
    // Realistic event line (compact JSON, exactly as the sidecar emits;
    // poll() strips the trailing newline before calling parse_event).
    // 21 landmark pairs; thumb tip = index 4 -> (0.40,0.50),
    // index tip = index 8 -> (0.60,0.70).
    std::string line =
      "{\"type\":\"hand\",\"t\":1.5,\"present\":true,\"handedness\":\"Right\","
      "\"landmarks\":[[0.10,0.20],[0.11,0.21],[0.12,0.22],[0.13,0.23],"
      "[0.40,0.50],[0.15,0.25],[0.16,0.26],[0.17,0.27],[0.60,0.70],"
      "[0.19,0.29],[0.20,0.30],[0.21,0.31],[0.22,0.32],[0.23,0.33],"
      "[0.24,0.34],[0.25,0.35],[0.26,0.36],[0.27,0.37],[0.28,0.38],"
      "[0.29,0.39],[0.30,0.40]],"
      "\"pinch_norm\":0.12,\"pinch_pos\":[0.5,0.6],\"pose\":\"none\"}";

    GestureEvent ev = parse_event(line);
    CHECK(ev.present == true);
    CHECK(ev.has_landmarks == true);
    CHECK(ev.pinching == true);                            // 0.12 < 0.5 threshold
    CHECK(std::fabs(ev.landmarks[4][0] - 0.40f) < 1e-4f);  // thumb tip x
    CHECK(std::fabs(ev.landmarks[4][1] - 0.50f) < 1e-4f);  // thumb tip y
    CHECK(std::fabs(ev.landmarks[8][0] - 0.60f) < 1e-4f);  // index tip x
    CHECK(std::fabs(ev.landmarks[8][1] - 0.70f) < 1e-4f);  // index tip y
    CHECK(std::fabs(ev.landmarks[20][0] - 0.30f) < 1e-4f); // last pair x
    CHECK(std::fabs(ev.landmarks[20][1] - 0.40f) < 1e-4f); // last pair y

    // No-hand event: present=false, 21 zero pairs still parse.
    std::string empty =
      "{\"type\":\"hand\",\"t\":2.0,\"present\":false,\"handedness\":\"\","
      "\"landmarks\":[[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],"
      "[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],"
      "[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0],"
      "[0.0,0.0],[0.0,0.0],[0.0,0.0],[0.0,0.0]],"
      "\"pinch_norm\":999.0,\"pinch_pos\":[0.0,0.0],\"pose\":\"none\"}";
    GestureEvent ev2 = parse_event(empty);
    CHECK(ev2.present == false);
    CHECK(ev2.pinching == false);       // not present -> never pinching
    CHECK(ev2.has_landmarks == true);   // 21 pairs present (all zero)

    // Truncated landmarks array -> has_landmarks false; other fields still parse.
    std::string trunc =
      "{\"present\":true,\"landmarks\":[[0.1,0.2],[0.3,0.4]],"
      "\"pinch_norm\":0.9,\"pinch_pos\":[0.1,0.2],\"pose\":\"none\"}";
    GestureEvent ev3 = parse_event(trunc);
    CHECK(ev3.present == true);
    CHECK(ev3.has_landmarks == false);  // only 2 of 21 pairs
    CHECK(ev3.pinching == false);       // 0.9 >= 0.5

    if (failures == 0) { printf("gesture_parse_test: all checks passed\n"); return 0; }
    printf("gesture_parse_test: %d failure(s)\n", failures);
    return 1;
}
