#pragma once
#include "LayerRenderOperation.h"
#include <algorithm>
#include <cmath>

namespace layer_affine {
template<class Point>
bool Prepare(const Point* dst, const Point* src, int width, int height,
             const TVPLayerRect& clip, TVPLayerAffineCopy& out) {
    if(!dst || !src || width<=0 || height<=0) return false;
    for(int i=0;i<6;++i) {
        if(!std::isfinite(dst[i].x) || !std::isfinite(dst[i].y) ||
           !std::isfinite(src[i].x) || !std::isfinite(src[i].y) ||
           std::abs(dst[i].x)>1000000 || std::abs(dst[i].y)>1000000) return false;
    }
    auto quad=[](const Point* p) {
        return p[1].x==p[3].x && p[1].y==p[3].y &&
               p[2].x==p[4].x && p[2].y==p[4].y;
    };
    if(!quad(dst) || !quad(src)) return false;
    if(src[0].y!=src[1].y || src[1].x!=src[5].x ||
       src[0].x!=src[2].x || src[2].y!=src[5].y) return false;
    const double l=src[0].x,t=src[0].y,r=src[5].x,b=src[5].y;
    if(l<0 || t<0 || r<=l || b<=t || r>width || b>height ||
       l!=std::floor(l) || t!=std::floor(t) || r!=std::floor(r) || b!=std::floor(b)) return false;
    // Restrict this first path to actual parallelograms and the same affine
    // branch the software manager chooses (rather than its perspective warp).
    auto dist=[](const Point& a,const Point& b) {
        double x=a.x-b.x,y=a.y-b.y; return x*x+y*y;
    };
    if(std::abs(dst[5].x-(dst[1].x-dst[0].x+dst[2].x))>1e-7 ||
       std::abs(dst[5].y-(dst[1].y-dst[0].y+dst[2].y))>1e-7 ||
       std::abs(dist(dst[0],dst[1])-dist(dst[2],dst[5]))>=0.001 ||
       std::abs(dist(dst[0],dst[2])-dist(dst[1],dst[5]))>=0.001) return false;
    out.clip=clip;
    out.sourceCrop={int(l),int(t),int(std::min(r+2,double(width))),int(std::min(b+2,double(height)))};
    const double sx[3]={0,r+1-l,r+1-l}, sy[3]={0,0,b+1-t};
    const double dx[3]={dst[0].x-clip.left,dst[1].x-clip.left,dst[5].x-clip.left};
    const double dy[3]={dst[0].y-clip.top,dst[1].y-clip.top,dst[5].y-clip.top};
    const double det=sx[0]*(sy[1]-sy[2])+sx[1]*(sy[2]-sy[0])+sx[2]*(sy[0]-sy[1]);
    const double invDet=1.0/det;
    auto solve=[&](const double* v,double* m) {
        m[0]=((sy[1]-sy[2])*v[0]+(sy[2]-sy[0])*v[1]+(sy[0]-sy[1])*v[2])*invDet;
        m[1]=((sx[2]-sx[1])*v[0]+(sx[0]-sx[2])*v[1]+(sx[1]-sx[0])*v[2])*invDet;
        m[2]=((sx[1]*sy[2]-sx[2]*sy[1])*v[0]+(sx[2]*sy[0]-sx[0]*sy[2])*v[1]+(sx[0]*sy[1]-sx[1]*sy[0])*v[2])*invDet;
    };
    double m[6]; solve(dx,m); solve(dy,m+3);
    const double d=m[0]*m[4]-m[1]*m[3];
    if(std::abs(d)<1e-15) return false;
    const double inv=1.0/d;
    out.inverse[0]=m[4]*inv; out.inverse[1]=-m[1]*inv;
    out.inverse[3]=-m[3]*inv; out.inverse[4]=m[0]*inv;
    out.inverse[2]=-(out.inverse[0]*m[2]+out.inverse[1]*m[5]);
    out.inverse[5]=-(out.inverse[3]*m[2]+out.inverse[4]*m[5]);
    for(double value:out.inverse) if(!std::isfinite(value) || std::abs(value)>1000000) return false;
    return true;
}
}
