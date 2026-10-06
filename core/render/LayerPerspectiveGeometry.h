#pragma once
#include "LayerRenderOperation.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace TVPImageUtils {
inline bool GetPerspectiveTransform(
    const double* srcX, const double* srcY,
    const double* dstX, const double* dstY,
    double* mat8) // [a,b,c,d,e,f,g,h] for matrix [[a,b,c],[d,e,f],[g,h,1]]
{
    // Set up 8x8 linear system
    // Actually solve using the standard technique: 
    // We have 8 unknowns. Form matrix equation and solve via Gaussian elimination.
    
    double M[8][9]; // augmented matrix
    for (int i = 0; i < 4; ++i)
    {
        int r = i * 2;
        // Equation for dst_x
        M[r][0] = srcX[i];  M[r][1] = srcY[i];  M[r][2] = 1.0;
        M[r][3] = 0.0;      M[r][4] = 0.0;      M[r][5] = 0.0;
        M[r][6] = -srcX[i] * dstX[i]; M[r][7] = -srcY[i] * dstX[i];
        M[r][8] = dstX[i];
        // Equation for dst_y
        M[r+1][0] = 0.0;    M[r+1][1] = 0.0;    M[r+1][2] = 0.0;
        M[r+1][3] = srcX[i]; M[r+1][4] = srcY[i]; M[r+1][5] = 1.0;
        M[r+1][6] = -srcX[i] * dstY[i]; M[r+1][7] = -srcY[i] * dstY[i];
        M[r+1][8] = dstY[i];
    }

    // Gaussian elimination with partial pivoting
    for (int col = 0; col < 8; ++col)
    {
        // Find pivot
        int maxRow = col;
        double maxVal = fabs(M[col][col]);
        for (int row = col + 1; row < 8; ++row)
        {
            double v = fabs(M[row][col]);
            if (v > maxVal) { maxVal = v; maxRow = row; }
        }
        if (maxVal < 1e-15) return false; // singular
        if (maxRow != col) std::swap(M[col], M[maxRow]);

        // Eliminate rows below
        for (int row = col + 1; row < 8; ++row)
        {
            double factor = M[row][col] / M[col][col];
            for (int j = col; j <= 8; ++j)
                M[row][j] -= factor * M[col][j];
        }
    }

    // Back substitution
    for (int row = 7; row >= 0; --row)
    {
        double sum = M[row][8];
        for (int j = row + 1; j < 8; ++j)
            sum -= M[row][j] * mat8[j];
        mat8[row] = sum / M[row][row];
    }
    return true;
}

inline bool InvertPerspectiveTransform(const double* mat8,double* inverse) {
    double a=mat8[0],b=mat8[1],c=mat8[2],d=mat8[3],e=mat8[4],f=mat8[5],g=mat8[6],h=mat8[7];
    double det=a*(e*1-f*h)-b*(d*1-f*g)+c*(d*h-e*g);
    if(!std::isfinite(det) || std::abs(det)<1e-15) return false;
    double invDet=1.0/det;
    inverse[0]=(e*1-f*h)*invDet; inverse[1]=(c*h-b*1)*invDet; inverse[2]=(b*f-c*e)*invDet;
    inverse[3]=(f*g-d*1)*invDet; inverse[4]=(a*1-c*g)*invDet; inverse[5]=(c*d-a*f)*invDet;
    inverse[6]=(d*h-e*g)*invDet; inverse[7]=(b*g-a*h)*invDet; inverse[8]=(a*e-b*d)*invDet;
    return true;
}
}

namespace layer_perspective {
inline bool ValidateInverse(const double* inverse) {
    if(!inverse) return false;
    for(int i=0;i<9;++i) if(!std::isfinite(inverse[i])) return false;
    const double det=inverse[0]*(inverse[4]*inverse[8]-inverse[5]*inverse[7])-
        inverse[1]*(inverse[3]*inverse[8]-inverse[5]*inverse[6])+
        inverse[2]*(inverse[3]*inverse[7]-inverse[4]*inverse[6]);
    return std::isfinite(det) && det!=0;
}
template<class Point>
bool PrepareQuad(const Point* dst,const Point* src,int sw,int sh,const TVPLayerRect& clip,TVPLayerPerspectiveQuad& out) {
    if(!dst || !src || sw<=0 || sh<=0 || clip.right<clip.left || clip.bottom<clip.top) return false;
    for(int i=0;i<4;++i) for(double value:{double(dst[i].x),double(dst[i].y),double(src[i].x),double(src[i].y)})
        if(!std::isfinite(value)) return false;
    out={};out.clip=clip;
    auto close=[](double a,double b){return std::abs(a-b)<0.001;};
    auto rectangle=[&](const Point* p){return close(p[0].y,p[1].y)&&close(p[1].x,p[3].x)&&
        close(p[0].x,p[2].x)&&close(p[2].y,p[3].y);};
    bool integerSafe=true;
    for(int i=0;i<4;++i) for(double value:{double(dst[i].x),double(dst[i].y),double(src[i].x),double(src[i].y)})
        integerSafe &= value>std::numeric_limits<int>::min()/2 && value<std::numeric_limits<int>::max()/2;
    if(integerSafe && rectangle(dst) && rectangle(src) && dst[3].x>dst[0].x && dst[3].y>dst[0].y &&
       src[3].x>src[0].x && src[3].y>src[0].y) {
        // Correct the legacy index-2 bug: lt,rt,lb,rb use point 3 for rb.
        const TVPLayerRect destination{int(dst[0].x),int(dst[0].y),int(dst[3].x),int(dst[3].y)};
        TVPLayerRect source{int(src[0].x),int(src[0].y),int(src[3].x),int(src[3].y)};
        out.rectangle=true;
        out.destination={std::max(clip.left,destination.left),std::max(clip.top,destination.top),
            std::min(clip.right,destination.right),std::min(clip.bottom,destination.bottom)};
        if(destination.Width()<=0 || destination.Height()<=0 || out.destination.Width()<=0 || out.destination.Height()<=0) {
            out.clip=out.destination={0,0,0,0};out.source=source;return true;
        }
        const int dw=destination.Width(),dh=destination.Height(),rw=source.Width(),rh=source.Height();
        if(out.destination.left>destination.left) source.left+=(float)rw/dw*(out.destination.left-destination.left);
        if(out.destination.right<destination.right) source.right-=(float)rw/dw*(destination.right-out.destination.right);
        if(out.destination.top>destination.top) source.top+=(float)rh/dh*(out.destination.top-destination.top);
        if(out.destination.bottom<destination.bottom) source.bottom-=(float)rh/dh*(destination.bottom-out.destination.bottom);
        if(source.Width()<=0 || source.Height()<=0 || source.left<0 || source.top<0 || source.right>sw || source.bottom>sh) return false;
        out.source=source;out.clip=out.destination;return true;
    }
    double sx[]={src[0].x,src[1].x+1,src[3].x+1,src[2].x};
    double sy[]={src[0].y,src[1].y,src[3].y+1,src[2].y+1};
    double dx[]={dst[0].x-clip.left,dst[1].x-clip.left,dst[3].x-clip.left,dst[2].x-clip.left};
    double dy[]={dst[0].y-clip.top,dst[1].y-clip.top,dst[3].y-clip.top,dst[2].y-clip.top};
    double forward[8]={};
    if(!TVPImageUtils::GetPerspectiveTransform(sx,sy,dx,dy,forward) ||
       !TVPImageUtils::InvertPerspectiveTransform(forward,out.inverse) || !ValidateInverse(out.inverse)) return false;
    return true;
}
}
