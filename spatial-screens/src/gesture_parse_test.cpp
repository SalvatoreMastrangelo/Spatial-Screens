// Standalone unit test for the two-hand gesture event parser. No framework:
// asserts via CHECK, returns non-zero on any failure. Build+run with
//   make gesture-parse-test && ./gesture-parse-test
#include "gesture_parse.h"
#include <cmath>
#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static std::string lm21(float base) {
    std::string s = "[";
    for (int i = 0; i < 21; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s[%.2f,%.2f]", i ? "," : "", base + i * 0.01f,
                 base + i * 0.01f + 0.5f);
        s += buf;
    }
    return s + "]";
}

int main() {
    // Both hands present: left pinching (0.12 < 0.5), right open (0.9 >= 0.5).
    std::string both =
        "{\"type\":\"hand\",\"t\":1.5,"
        "\"left\":{\"present\":true,\"handedness\":\"left\",\"pinch_norm\":0.12,"
        "\"pinch_pos\":[0.4,0.5],\"pose\":\"point\",\"landmarks\":" + lm21(0.10f) + "},"
        "\"right\":{\"present\":true,\"handedness\":\"right\",\"pinch_norm\":0.90,"
        "\"pinch_pos\":[0.6,0.7],\"pose\":\"open_palm\",\"landmarks\":" + lm21(0.30f) + "}}";
    GestureEvent ev = parse_event(both);
    CHECK(ev.left.present == true);
    CHECK(ev.left.pinching == true);
    CHECK(ev.left.pose == "point");
    CHECK(std::fabs(ev.left.pinch_x - 0.4f) < 1e-4f);
    CHECK(std::fabs(ev.left.pinch_y - 0.5f) < 1e-4f);
    CHECK(ev.left.has_landmarks == true);
    CHECK(std::fabs(ev.left.landmarks[0][0] - 0.10f) < 1e-4f);
    CHECK(ev.right.present == true);
    CHECK(ev.right.pinching == false);            // 0.90 >= 0.5
    CHECK(ev.right.pose == "open_palm");
    CHECK(std::fabs(ev.right.pinch_x - 0.6f) < 1e-4f);
    CHECK(ev.right.has_landmarks == true);
    CHECK(std::fabs(ev.right.landmarks[20][0] - (0.30f + 20 * 0.01f)) < 1e-4f);

    // One hand present.
    std::string one =
        "{\"type\":\"hand\",\"t\":2.0,"
        "\"left\":{\"present\":false},"
        "\"right\":{\"present\":true,\"handedness\":\"right\",\"pinch_norm\":0.20,"
        "\"pinch_pos\":[0.5,0.5],\"pose\":\"fist\",\"landmarks\":" + lm21(0.20f) + "}}";
    GestureEvent ev2 = parse_event(one);
    CHECK(ev2.left.present == false);
    CHECK(ev2.left.pinching == false);
    CHECK(ev2.left.has_landmarks == false);
    CHECK(ev2.right.present == true);
    CHECK(ev2.right.pinching == true);
    CHECK(ev2.right.pose == "fist");

    // Neither hand present.
    std::string none = "{\"type\":\"hand\",\"t\":3.0,"
                       "\"left\":{\"present\":false},\"right\":{\"present\":false}}";
    GestureEvent ev3 = parse_event(none);
    CHECK(ev3.left.present == false);
    CHECK(ev3.right.present == false);
    CHECK(ev3.left.has_landmarks == false);

    if (failures == 0) { printf("gesture_parse_test: all checks passed\n"); return 0; }
    printf("gesture_parse_test: %d failure(s)\n", failures);
    return 1;
}
