#pragma once
#include <cstdint>
#include <cstddef>
#include <cmath>
#include <algorithm>

namespace wxl::water::lights {
// Read-only build-12340 scene layout; addresses are never retained after capture.
struct Node {
    uint32_t scene, stamp, type;
    float position[3], view[3], direction[3], ambient[3], color[3], specular[3], attenuation[3];
    uint32_t enabled, previous, next;
};
static_assert(sizeof(Node)==0x6c && offsetof(Node,next)==0x68);
struct Sample { float position[3], color[3], attenuation[3], distance; uint32_t stamp; };
struct Inventory {
    uint32_t visited=0, enabled=0, stale=0, invalid=0, nearby=0, samples=0;
    Sample nearest[16]{};
};
inline bool Pointer(uint32_t p,uint32_t size) { return p>=0x10000 && !(p&3) && p<0xfff00000 && size<=0xfff00000-p; }
inline bool Vector(const float* v,float low,float high) {
    for(int i=0;i<3;++i) if(!std::isfinite(v[i]) || v[i]<low || v[i]>high) return false;
    return true;
}
// Reader signature: bool(address, destination, byte count). Failure publishes no partial data.
template<class Reader> bool Capture(Reader read,const float* camera,Inventory& out) {
    out={}; Inventory next{};
    if(!Vector(camera,-1000000,1000000)) return false;
    uint32_t scene=0,buckets=0,stamp=0;
    if(!read(0xcd754c,&scene,4)) return false;
    if(!scene) return true;
    if(!Pointer(scene,0x28) || !read(scene+0x24,&buckets,4) || !read(scene+0x14,&stamp,4)) return false;
    if(buckets) {
        if(!Pointer(buckets,4096*4)) return false;
        for(uint32_t i=0;i<4096;++i) {
            uint32_t link=buckets+i*4,address=0,count=0;
            if(!read(link,&address,4)) return false;
            while(address) {
                Node n{};
                if(++count>512 || ++next.visited>8192 || !Pointer(address,sizeof(n)) || !read(address,&n,sizeof(n))) return false;
                if(n.scene!=scene || n.previous!=link || n.type!=1) return false;
                if(n.enabled) {
                    ++next.enabled;
                    if(n.stamp!=stamp) ++next.stale;
                    if(!Vector(n.position,-1000000,1000000) || !Vector(n.color,0,10000) || !Vector(n.attenuation,0,1000000)) ++next.invalid;
                    else {
                        double d=0; for(int j=0;j<3;++j) { double x=double(n.position[j])-camera[j]; d+=x*x; }
                        if(d<=200.0*200) ++next.nearby;
                        Sample sample{}; sample.distance=float(std::sqrt(d)); sample.stamp=n.stamp;
                        std::copy_n(n.position,3,sample.position); std::copy_n(n.color,3,sample.color); std::copy_n(n.attenuation,3,sample.attenuation);
                        uint32_t slot=0; while(slot<next.samples && next.nearest[slot].distance<=sample.distance) ++slot;
                        if(slot<16) {
                            auto size=std::min(next.samples+1,16u);
                            for(uint32_t j=size-1;j>slot;--j) next.nearest[j]=next.nearest[j-1];
                            next.nearest[slot]=sample; next.samples=size;
                        }
                    }
                }
                link=address+offsetof(Node,next); address=n.next;
            }
        }
    }
    uint32_t checkScene=0,checkBuckets=0,checkStamp=0;
    if(!read(0xcd754c,&checkScene,4) || checkScene!=scene || !read(scene+0x24,&checkBuckets,4) || checkBuckets!=buckets || !read(scene+0x14,&checkStamp,4) || checkStamp!=stamp) return false;
    out=next; return true;
}
void Panel();
void Frame(const float* camera,int map);
void Reset();
}
