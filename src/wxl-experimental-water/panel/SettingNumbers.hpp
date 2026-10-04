// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
namespace wxl::water::settingnumbers {
inline bool End(const char* p) {
    while(*p==' ' || *p=='\t' || *p=='\r') ++p;
    return !*p || *p=='\n';
}
inline bool Float(const char* p,float& out) {
    errno=0; char* end=nullptr; const float v=std::strtof(p,&end);
    if(end==p || errno==ERANGE || !std::isfinite(v) || !End(end)) return false;
    out=v; return true;
}
inline bool Seed(const char* p,uint32_t& out) {
    while(*p==' ' || *p=='\t') ++p;
    if(*p=='-') return false;
    errno=0; char* end=nullptr; const auto v=std::strtoull(p,&end,10);
    if(end==p || errno==ERANGE || v>UINT32_MAX || !End(end)) return false;
    out=static_cast<uint32_t>(v); return true;
}
}
