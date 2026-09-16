#include "cross_layout.h"
#include "protocol.h"
#include <cassert>
#include <iostream>
#include <limits>
using namespace thunderlink::cross;
static Display local(std::string id,Rect r) { return {id,id,r,false}; }
static Display mac(std::string id,Rect r) { return {id,id,r,true}; }
static void near(double a,double b) { assert(std::abs(a-b)<0.001); }
int main() {
    static_assert(TLNK_VERSION==4 && TLNK_HEADER_SIZE==48);
    static_assert(TLNK_CROSS_PING_MS*4==TLNK_CROSS_LEASE_MS);
    Layout l{local("win1",{-1920,0,1920,1080}),local("win2",{0,0,2560,1440}),mac("mac",{2560,180,1920,1080})};
    assert(ValidateLayout(l).empty());
    auto r=CrossEdge(l,1,2559,500,4,0); assert(r && r->target==2); near(r->x,2560.5); near(r->y,500);
    auto p=Normalized(l[2].bounds,r->x,r->y); assert(p.first>0 && p.first<0.01); near(p.second,320.0/1080);
    r=CrossEdge(l,2,2560.5,500,-2,0); assert(r && r->target==1); near(r->x,2559.5);
    assert(!CrossEdge(l,1,2559,100,100,0)); // exposed segment is a wall
    assert(!CrossEdge(l,1,2559,1260,100,0)); // half-open endpoint
    assert(!CrossEdge(l,1,2559,500,0,100)); // motion along edge
    assert(!CrossEdge(l,2,2560.5,500,1,0)); // inset prevents bouncing
    assert(!CrossEdge(l,1,2500,500,40,0)); // no edge reached
    assert(!CrossEdge(l,1,2500,1,100,-100)); // first hit is upper wall: cannot tunnel to right
    r=CrossEdge(l,1,10,500,-20,0); assert(r && r->target==0); near(r->x,-0.5);
    l={local("windows",{0,0,1920,1080}),mac("portrait",{300,-1920,1080,1920})};
    assert(ValidateLayout(l).empty());
    r=CrossEdge(l,0,700,0.5,0,-10); assert(r && r->target==1); near(r->y,-0.5);
    r=CrossEdge(l,1,700,-0.5,0,10); assert(r && r->target==0); near(r->y,0.5);
    l={local("windows",{0,0,1920,1080}),mac("below",{0,1080,1920,1200})};
    r=CrossEdge(l,0,100,1079,0,10); assert(r && r->target==1);
    r=CrossEdge(l,1,100,1080.5,0,-10); assert(r && r->target==0);
    l.push_back(mac("mac2",{1920,1080,1000,1000})); assert(ValidateLayout(l).empty());
    r=CrossEdge(l,1,1919,1300,10,0); assert(r && r->target==2);
    l[2].bounds.x+=1; assert(!ValidateLayout(l).empty()); // gap
    l[2].bounds.x-=2; assert(!ValidateLayout(l).empty()); // overlap
    l[2].bounds.x=1920; l[2].id="below"; assert(!ValidateLayout(l).empty()); // ambiguous identity
    l[2].id="mac2"; l[2].bounds.width=std::numeric_limits<double>::quiet_NaN(); assert(!ValidateLayout(l).empty());
    assert(!ValidateLayout({mac("onlyMac",{0,0,100,100})}).empty());
    assert(!ValidateLayout({local("onlyWin",{0,0,100,100})}).empty());
    assert(!ValidateLayout({local("w",{0,0,100,100}),mac("m",{100,100,100,100})}).empty()); // corners not linked
    assert(!ValidateLayout({local("w",{0,0,100,100}),mac("m",{100,0,0,100})}).empty());
    assert(!CrossEdge(l,99,0,0,1,1));
    assert(!CrossEdge(l,0,0,0,std::numeric_limits<double>::infinity(),1));
    // Differently sized tiles preserve continuous landing coordinates; normalization
    // maps correctly to native display coordinates regardless of pixel density.
    Layout scaled{local("w",{0,0,1920,1080}),mac("m",{1920,0,960,540})};
    r=CrossEdge(scaled,0,1919,270,2,0); assert(r); p=Normalized(scaled[1].bounds,r->x,r->y); near(p.second,0.5);
    // Upper and lower remote tiles share a Windows edge, never overlap routing.
    Layout split{local("w",{0,0,100,100}),mac("top",{100,0,100,50}),mac("bottom",{100,50,100,50})};
    assert(ValidateLayout(split).empty());
    assert(CrossEdge(split,0,99,49,2,0)->target==1);
    assert(CrossEdge(split,0,99,50,2,0)->target==2);
    std::cout << "Cross geometry, routing, layout validation, and v4 contract passed\n";
}
