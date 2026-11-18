#include "TouchInteraction.h"
#include <cstdlib>
#include <iostream>
#include <vector>

static void check(bool value,const char* label) {
    if(!value){std::cerr<<label<<" failed\n";std::exit(1);}
}
int main() {
    using namespace sle::android;
    const TouchTarget first{10,20,90,36,14,"first"},second{10,84,90,36,14,"second"};
    TouchPress press;
    press.begin(first);
    check(press.isPressed(first)&&!press.isPressed(second),"only original control paints pressed");
    std::vector<TouchTarget> reordered{second,first};
    check(press.release(reordered,40,30)->id=="first","frame reorder retains control identity");
    check(!press.isPressed(first)&&!press.release(reordered,40,30),"release clears state and fires once");
    press.begin(first);press.move(101,30);
    check(!press.isPressed(first)&&!press.release(reordered,101,30),"drag out cancels feedback and activation");
    press.begin(first);press.move(101,30);press.move(40,30);
    check(!press.release(reordered,40,30),"drag out then back remains cancelled");
    press.begin(first);press.cancel();
    check(!press.isPressed(first)&&!press.release(reordered,40,30),"ACTION_CANCEL clears press");
    press.begin(first);
    check(!press.release(std::vector<TouchTarget>{second},40,30),"removed control never activates replacement index");
    press.begin(first);
    auto moved=first;moved.x+=5;
    check(!press.release(std::vector<TouchTarget>{moved},40,30),"layout scale change invalidates held press");
    check(first.contains(10,20)&&!first.contains(100,20)&&!first.contains(10,56),"right and bottom edges are excluded");
    std::cout<<"Android touch identity, feedback, drag, cancel and layout invalidation: PASS\n";
}
