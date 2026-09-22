#include "histogramrgbstyle.h"
#include <cairomm/surface.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using Data = std::array<std::array<double, 256>, 3>;
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
std::array<int, 3> pixel(const Cairo::RefPtr<Cairo::ImageSurface>& surface, int x, int y)
{
    surface->flush();
    uint32_t value;
    std::memcpy(&value, surface->get_data()+y*surface->get_stride()+x*4, sizeof(value));
    return {{int((value>>16)&255), int((value>>8)&255), int(value&255)}};
}
Cairo::RefPtr<Cairo::ImageSurface> render(const Data& data, std::array<bool,3> enabled, bool light, int scale=1)
{
    constexpr int w=300, h=156;
    auto surface=Cairo::ImageSurface::create(Cairo::FORMAT_ARGB32,w*scale,h*scale);
    auto cr=Cairo::Context::create(surface);
    cr->scale(scale,scale);
    const double bg=light ? .93 : .08;
    cr->set_source_rgb(bg,bg,bg);
    cr->paint();
    cr->set_line_width(3);
    histogramstyle::drawRGB(cr,w,h,enabled,light,[&](int channel,bool fill) {
        if (fill) cr->move_to(0,h-1);
        for (int i=0;i<256;++i) {
            const double x=i*(w-1)/255., y=h-2-data[channel][i]*(h-4);
            if (!fill && i==0) cr->move_to(x,y);
            else cr->line_to(x,y);
        }
        if (fill) { cr->line_to(w-1,h-1); cr->close_path(); }
    });
    require(cr->get_line_width()==3 && cairo_get_operator(cr->cobj())==CAIRO_OPERATOR_OVER,
        "RGB treatment must restore context for clipping marks");
    return surface;
}
}

int main(int argc,char** argv)
{
    try {
        require(argc==2,"usage: steep-histogram-style-tests <existing output directory>");
        const std::string output=argv[1];
        Data flat;
        for (auto& channel:flat) channel.fill(.65);
        const auto original=flat;
        for (bool light : {false,true}) {
            auto blank=render(flat,{{false,false,false}},light);
            const auto bg=pixel(blank,150,100);
            require(bg==pixel(blank,150,30),"disabled channels leave background untouched");
            for (int c=0;c<3;++c) {
                std::array<bool,3> enabled{{false,false,false}};
                enabled[c]=true;
                const auto p=pixel(render(flat,enabled,light),150,100);
                require(p[c]>p[(c+1)%3]+8 && p[c]>p[(c+2)%3]+8,"single channels remain identifiable");
            }
            auto all=render(flat,{{true,true,true}},light);
            const auto mid=pixel(all,150,100);
            require(*std::max_element(mid.begin(),mid.end())<250,"overlapping fills must not clip to white");
            require(mid!=bg,"overlap must be visible on either theme");
            require(pixel(all,150,30)==bg,"glow is local to the trace, not the entire plot");
            require(pixel(all,150,70)!=pixel(all,150,140),"layer density changes gradually with height");
            const auto hidpi=pixel(render(flat,{{true,true,true}},light,2),300,200);
            for (int c=0;c<3;++c) require(std::abs(mid[c]-hidpi[c])<=2,"HiDPI keeps the same compositing");
        }
        require(flat==original,"drawing must not modify the histogram data");
        Data scene;
        for (int c=0;c<3;++c) for(int i=0;i<256;++i) {
            const double peak=(i-(85+c*38.))/27;
            const double toe=(i-33.)/14;
            scene[c][i]=.02+.7*std::exp(-peak*peak)+.17*std::exp(-toe*toe);
        }
        render(scene,{{true,true,true}},false)->write_to_png(output+"/histogram-dark.png");
        render(scene,{{true,true,true}},true)->write_to_png(output+"/histogram-light.png");
        render(scene,{{true,true,true}},false,2)->write_to_png(output+"/histogram-hidpi.png");
        const auto start=std::chrono::steady_clock::now();
        for(int i=0;i<100;++i) render(scene,{{true,true,true}},false);
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-start).count();
        std::cout<<"PASS channel toggles, bounded overlaps, light/dark themes, HiDPI, immutable data and Cairo state\n"
                 <<"100 native Cairo renders: "<<ms<<" ms\n";
        return 0;
    } catch(const std::exception& e) { std::cerr<<"FAIL "<<e.what()<<'\n'; }
    return 1;
}
