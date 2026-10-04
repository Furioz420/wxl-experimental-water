#pragma once
#include <d3d9.h>

namespace wxl::water {
// Runs after the deferred flush. Preserve GPU values without changing the
// engine's cached constants. Pixel c22..c30 are outside water's ranges.
template<class Device> class ConstantState {
    Device* device_;
    float vertex_[27 * 4]{};
    float pixelLow_[16 * 4]{};
    float pixelHigh_[22 * 4]{};
    bool ready_ = false;
    bool active_ = false;
public:
    unsigned failures = 0;
    explicit ConstantState(Device* device) : device_(device) {
        const bool vertex = SUCCEEDED(device_->GetVertexShaderConstantF(46, vertex_, 27));
        const bool low = SUCCEEDED(device_->GetPixelShaderConstantF(6, pixelLow_, 16));
        const bool high = SUCCEEDED(device_->GetPixelShaderConstantF(31, pixelHigh_, 22));
        failures = (!vertex) + (!low) + (!high);
        ready_ = vertex && low && high;
        active_ = ready_;
    }
    ConstantState(const ConstantState&) = delete;
    ConstantState& operator=(const ConstantState&) = delete;
    bool Ready() const { return ready_; }
    void Set(int target, int start, const float* data, int count) {
        const bool range = count > 0 && start >= 0 &&
            ((target == 0 && start >= 46 && start <= 72 && count <= 73 - start) ||
             (target == 4 && ((start >= 6 && start <= 21 && count <= 22 - start) ||
                              (start >= 31 && start <= 52 && count <= 53 - start))));
        if (!active_ || !range || !data) { ++failures; return; }
        const HRESULT hr = target == 0
            ? device_->SetVertexShaderConstantF(start, data, count)
            : device_->SetPixelShaderConstantF(start, data, count);
        if (FAILED(hr)) ++failures;
    }
    void Restore() {
        if (!active_) return;
        active_ = false;
        if (FAILED(device_->SetVertexShaderConstantF(46, vertex_, 27))) ++failures;
        if (FAILED(device_->SetPixelShaderConstantF(6, pixelLow_, 16))) ++failures;
        if (FAILED(device_->SetPixelShaderConstantF(31, pixelHigh_, 22))) ++failures;
    }
    ~ConstantState() { Restore(); }
};
}
