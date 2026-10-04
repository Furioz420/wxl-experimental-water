#pragma once
#include <d3d9.h>

namespace wxl::water {
// Own only the bindings this pass writes. GetTexture's references live until restoration.
template<class Device, class Texture> class TextureState {
    Device* device_;
    static constexpr unsigned stages_[16] = {2,3,4,6,8,9,10,11,12,13,14,15,257,258,259,260};
    static constexpr D3DSAMPLERSTATETYPE states_[5] = {
        D3DSAMP_MINFILTER,D3DSAMP_MAGFILTER,D3DSAMP_MIPFILTER,D3DSAMP_ADDRESSU,D3DSAMP_ADDRESSV};
    Texture* textures_[16] = {};
    bool valid_[16] = {};
    DWORD samplers_[4][5] = {};
    bool samplerValid_[4][5] = {};
public:
    unsigned failures = 0;
    explicit TextureState(Device* device):device_(device) {
        for(unsigned i=0;i<16;++i) {
            valid_[i]=SUCCEEDED(device_->GetTexture(stages_[i],&textures_[i]));
            if(!valid_[i]) ++failures;
        }
        for(unsigned i=0;i<4;++i) for(unsigned j=0;j<5;++j) {
            samplerValid_[i][j]=SUCCEEDED(device_->GetSamplerState(257+i,states_[j],&samplers_[i][j]));
            if(!samplerValid_[i][j]) ++failures;
        }
    }
    TextureState(const TextureState&)=delete;
    TextureState& operator=(const TextureState&)=delete;
    void Restore() {
        for(unsigned i=0;i<16;++i) {
            if(valid_[i] && FAILED(device_->SetTexture(stages_[i],textures_[i]))) ++failures;
            if(textures_[i]) textures_[i]->Release();
            textures_[i]=nullptr;valid_[i]=false;
        }
        for(unsigned i=0;i<4;++i) for(unsigned j=0;j<5;++j) {
            if(samplerValid_[i][j] && FAILED(device_->SetSamplerState(257+i,states_[j],samplers_[i][j]))) ++failures;
            samplerValid_[i][j]=false;
        }
    }
    void Discard() {
        for(unsigned i=0;i<16;++i) {
            if(textures_[i]) textures_[i]->Release();
            textures_[i]=nullptr;valid_[i]=false;
        }
        for(auto& row:samplerValid_) for(bool& valid:row) valid=false;
    }
    ~TextureState(){Restore();}
};
}
