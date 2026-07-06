#include "gesture_parse.h"

#include <cstdlib>

namespace {

constexpr float PINCH_THRESHOLD = 0.5f; // tune after hands-on test

// Minimal key-based scanners for the fixed, known event schema (see
// protocol.py's encode_event). Not a general JSON parser — deliberately
// so, to avoid adding a JSON library dependency for a schema we control
// on both ends.
bool json_find_bool(const std::string& s, const char* key, bool& out) {
    std::string pat = std::string("\"") + key + "\":";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    out = s.compare(pos, 4, "true") == 0;
    return true;
}

bool json_find_number(const std::string& s, const char* key, float& out) {
    std::string pat = std::string("\"") + key + "\":";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    out = strtof(s.c_str() + pos, nullptr);
    return true;
}

bool json_find_string(const std::string& s, const char* key, std::string& out) {
    std::string pat = std::string("\"") + key + "\":\"";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    auto end = s.find('"', pos);
    if (end == std::string::npos) return false;
    out = s.substr(pos, end - pos);
    return true;
}

// pinch_pos is emitted as "pinch_pos":[x,y] — json_find_number would stop
// at the array's leading '[', so it needs its own extractor.
bool json_find_pair(const std::string& s, const char* key, float& x, float& y) {
    std::string pat = std::string("\"") + key + "\":[";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    x = strtof(s.c_str() + pos, nullptr);
    auto comma = s.find(',', pos);
    if (comma == std::string::npos) return false;
    y = strtof(s.c_str() + comma + 1, nullptr);
    return true;
}

// landmarks is emitted as "landmarks":[[x,y],[x,y],...] — exactly 21 pairs.
// Walk pair by pair; return true only if all 21 parse. Any short/garbled
// array leaves out[] partially written and returns false (caller gates on
// the return value via has_landmarks).
bool json_find_landmarks(const std::string& s, float out[21][2]) {
    std::string pat = "\"landmarks\":[";
    auto found = s.find(pat);
    if (found == std::string::npos) return false;
    const char* c = s.c_str();
    size_t pos = found + pat.size();  // at the '[' of pair 0
    for (int i = 0; i < 21; i++) {
        while (c[pos] && c[pos] != '[' && c[pos] != ']') pos++;
        if (c[pos] != '[') return false;               // fewer than 21 pairs
        pos++;
        char* end = nullptr;
        out[i][0] = strtof(c + pos, &end);
        if (end == c + pos) return false;              // no x number
        pos = size_t(end - c);
        while (c[pos] && c[pos] != ',') pos++;
        if (c[pos] != ',') return false;
        pos++;
        out[i][1] = strtof(c + pos, &end);
        if (end == c + pos) return false;              // no y number
        pos = size_t(end - c);
        while (c[pos] && c[pos] != ']') pos++;
        if (c[pos] != ']') return false;               // unterminated pair
        pos++;
    }
    return true;
}

// Extract the "{...}" body of a hand sub-object ("left"/"right") — its contents
// contain no nested '{' (values are bools/numbers/strings/arrays), so the first
// '}' after the opening brace closes it. Returns "" if the key is absent.
std::string hand_object(const std::string& s, const char* key) {
    std::string pat = std::string("\"") + key + "\":{";
    auto pos = s.find(pat);
    if (pos == std::string::npos) return std::string();
    pos += pat.size();
    auto end = s.find('}', pos);
    if (end == std::string::npos) return std::string();
    return s.substr(pos, end - pos);
}

HandState parse_hand(const std::string& obj) {
    HandState h;
    json_find_bool(obj, "present", h.present);
    float pinch_norm = 999.f;
    json_find_number(obj, "pinch_norm", pinch_norm);
    h.pinching = h.present && pinch_norm < PINCH_THRESHOLD;
    json_find_pair(obj, "pinch_pos", h.pinch_x, h.pinch_y);
    json_find_string(obj, "pose", h.pose);
    h.has_landmarks = json_find_landmarks(obj, h.landmarks);
    h.has_depth = json_find_number(obj, "depth", h.depth);  // absent => false, depth stays 0
    return h;
}

} // namespace

GestureEvent parse_event(const std::string& line) {
    GestureEvent ev;
    ev.left = parse_hand(hand_object(line, "left"));
    ev.right = parse_hand(hand_object(line, "right"));
    return ev;
}
