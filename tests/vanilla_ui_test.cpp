#include "../src-client/VanillaUi.h"
#include <cassert>
#include <cmath>
#include <iostream>
#include <vector>
using namespace sle::client::vanilla;
bool near(float a,float b) { return std::abs(a-b)<0.0001f; }
int main() {
    assert(near(channel(198),198.0f/255.0f)); // must never become .565 (gamma regression)
    assert(ButtonPixels[0][4].r==198 && ButtonPixels[0][4].g==198);
    assert(ButtonPixels[1][4].r==33 && ButtonPixels[1][4].g==131 && ButtonPixels[1][4].b==6);
    assert(ButtonPixels[2][1].g==78 && ButtonPixels[2][7].g==205);
    for(auto size : std::vector<Rect>{{0,0,148,30},{5,9,112,26},{0,0,48,20},{0,0,3,3}}) {
        auto const inner=inset(size,1);
        auto const slices=nineSlice(inner,4);
        float area=0;
        for(auto const& s:slices) {
            assert(s.target.width()>=0 && s.target.height()>=0);
            assert(s.uv.x0>=0 && s.uv.x1<=1 && s.uv.y0>=0 && s.uv.y1<=1);
            area+=s.target.width()*s.target.height();
        }
        assert(near(area,inner.width()*inner.height()));
        assert(near(slices[4].uv.width(),0.5f));
        assert(near(slices[0].target.width(),std::min(1.0f,inner.width()/2)));
    }
    // Native button stays 148x30 LOGICAL pixels at every viewport/GUI scale.
    for(int guiScale:{1,2,3,4}) for(int width:{960,1280,1920,2560}) {
        Rect const play{width/(2.0f*guiScale)-74, 200, width/(2.0f*guiScale)+74,230};
        auto const entry=menuEntry(play,32);
        assert(near(entry.width(),play.width()) && near(entry.height(),play.height()));
        assert(near(play.y0-entry.y1,2));
        assert(near(entry.x0*guiScale,play.x0*guiScale));
    }
    for(int bits=0;bits<16;++bits) {
        bool const start=bits&1, play=bits&2, modal=bits&4, focus=bits&8;
        assert(allowOverlay(start,play,modal,focus)==(start && play && !modal && !focus));
    }
    assert(isInlineModal("modal_dialog_popup") && isInlineModal("modal_input"));
    assert(!isInlineModal("start_screen") && !isInlineModal("popup_dialog_factory"));
    assert(!allowOverlay(true,true,true,false)); // same start_screen, native quit popup visible
    assert(allowOverlay(true,true,false,false)); // cancel popup restores entry
    std::cout << "vanilla UI geometry, exact palette, 16 modal gate combinations, resize matrix: PASS\n";
}
