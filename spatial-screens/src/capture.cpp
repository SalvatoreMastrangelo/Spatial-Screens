#include "capture.h"

#include <chrono>
#include <cstring>

namespace {

class TestBackend : public CaptureBackend {
public:
    bool start() override {
        buf_.resize(size_t(W) * H);
        t0_ = now_s();
        return true;
    }
    bool latest_frame(CaptureFrame& out) override {
        int shift_px = int((now_s() - t0_) * 40) % 64;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
                buf_[size_t(y) * W + x] =
                    (((x + shift_px) / 64 + y / 64) & 1) ? 0xff2d3646 : 0xff59c2ff;
        out.data = reinterpret_cast<const uint8_t*>(buf_.data());
        out.w = W; out.h = H; out.pitch = W * 4;
        return true;
    }
    bool alive() const override { return true; }  // the pattern cannot fail
    const char* name() const override { return "test"; }
    void stop() override {}

private:
    static double now_s() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }
    static constexpr int W = 1024, H = 576;
    std::vector<uint32_t> buf_;
    double t0_ = 0;
};

}  // namespace

std::unique_ptr<CaptureBackend> capture_create_test() {
    return std::make_unique<TestBackend>();
}
