// SPDX-License-Identifier: GPL-3.0-or-later
#include "gif_export.h"
#include "color_pipeline.h"
#include <QColorSpace>
#include <QHash>
#include <QSaveFile>
#include <QTransform>
#include <gif_lib.h>
#include <array>
#include <climits>
#include <memory>

namespace Sr {
namespace {
constexpr int transparentIndex=255;
constexpr int histogramSize=32*32*32;
struct Bin { quint64 count=0,red=0,green=0,blue=0; };
int colorBin(int r,int g,int b) { return ((r>>3)<<10)|((g>>3)<<5)|(b>>3); }
QString cancelledMessage() { return QStringLiteral("GIF保存を中止しました"); }
QString gifError(int code) {
    const auto* text=GifErrorString(code);
    return QStringLiteral("GIFを書き込めません: ")+(text?QString::fromLatin1(text):QString::number(code));
}
struct GifCloser { void operator()(GifFileType* file) const { int error=0; EGifCloseFile(file,&error); } };

bool applicationBlock(GifFileType* file,const char* name,const QByteArray& bytes) {
    if(EGifPutExtensionLeader(file,APPLICATION_EXT_FUNC_CODE)==GIF_ERROR ||
       EGifPutExtensionBlock(file,11,name)==GIF_ERROR) return false;
    for(qsizetype offset=0;offset<bytes.size();offset+=255)
        if(EGifPutExtensionBlock(file,int(qMin<qsizetype>(255,bytes.size()-offset)),bytes.constData()+offset)==GIF_ERROR) return false;
    return EGifPutExtensionTrailer(file)!=GIF_ERROR;
}
}

QString writeGif(const QString& path,const QVector<QImage>& frames,const QVector<int>& delays,
                 int loops,int rotation,const std::atomic_bool& cancelled,const std::function<void(int)>& progress) {
    if(frames.isEmpty() || frames.size()!=delays.size() || loops < -1 || loops>65535 || rotation%90!=0)
        return QStringLiteral("GIFのフレーム数・再生情報が不正です");
    const QSize size=frames.first().size();
    if(!size.isValid() || size.width()>65535 || size.height()>65535)
        return QStringLiteral("GIFの各辺は65535画素以内にしてください");
    for(int i=0;i<frames.size();++i)
        if(frames[i].isNull() || frames[i].size()!=size || delays[i]<10 || delays[i]>655350 || delays[i]%10!=0)
            return QStringLiteral("GIFのフレーム寸法または表示時間が不正です");

    // Build one palette across the entire animation. Its colour assignments and
    // ordered dither stay fixed between frames. Extra memory is bounded: a small
    // histogram/sample set and one converted frame, never another full sequence.
    QVector<Bin> histogram(histogramSize);
    QHash<QRgb,int> exact;
    std::array<GifColorType,256> colors{};
    bool hasTransparency=false;
    quint64 visible=0;
    for(int frame=0;frame<frames.size();++frame) {
        const auto rgba=frames[frame].convertToFormat(QImage::Format_RGBA8888);
        if(rgba.isNull()) return QStringLiteral("GIF変換用メモリーを確保できません");
        for(int y=0;y<rgba.height();++y) {
            if(cancelled) return cancelledMessage();
            const auto* row=rgba.constScanLine(y);
            for(int x=0;x<rgba.width();++x) {
                const auto* p=row+x*4;
                if(p[3]<128) { hasTransparency=true; continue; }
                auto& bin=histogram[colorBin(p[0],p[1],p[2])];
                ++bin.count; bin.red+=p[0]; bin.green+=p[1]; bin.blue+=p[2]; ++visible;
                const QRgb rgb=qRgb(p[0],p[1],p[2]);
                if(exact.size()<=256 && !exact.contains(rgb)) {
                    const int index=exact.size(); exact.insert(rgb,index);
                    if(index<256) colors[index]={p[0],p[1],p[2]};
                }
            }
        }
        if(progress) progress(frame+1);
    }
    if(cancelled) return cancelledMessage();
    int colorCount=hasTransparency?255:256;
    const bool exactPalette=exact.size()<=colorCount;
    std::array<GifByteType,histogramSize> lookup{};
    if(!exactPalette) {
        QVector<GifByteType> red,green,blue;
        const quint64 samples=qMin<quint64>(visible,1024*1024);
        for(const auto& bin:histogram) {
            if(!bin.count) continue;
            const int n=int(qMax<quint64>(1,bin.count*samples/visible));
            const auto r=GifByteType((bin.red+bin.count/2)/bin.count);
            const auto g=GifByteType((bin.green+bin.count/2)/bin.count);
            const auto b=GifByteType((bin.blue+bin.count/2)/bin.count);
            for(int i=0;i<n;++i) { red<<r; green<<g; blue<<b; }
        }
        QVector<GifByteType> indices(red.size());
        if(GifQuantizeBuffer(red.size(),1,&colorCount,red.constData(),green.constData(),blue.constData(),indices.data(),colors.data())==GIF_ERROR)
            return QStringLiteral("GIFの減色に失敗しました");
        // Recover 8-bit means after giflib's 5-bit histogram quantization.
        std::array<Bin,256> means{};
        for(qsizetype i=0;i<indices.size();++i) {
            auto& mean=means[indices[i]]; ++mean.count; mean.red+=red[i]; mean.green+=green[i]; mean.blue+=blue[i];
        }
        for(int i=0;i<colorCount;++i) if(means[i].count) {
            const auto& m=means[i]; colors[i]={GifByteType((m.red+m.count/2)/m.count),GifByteType((m.green+m.count/2)/m.count),GifByteType((m.blue+m.count/2)/m.count)};
        }
        for(int bin=0;bin<histogramSize;++bin) {
            if(cancelled) return cancelledMessage();
            const int r=((bin>>10)&31)*8+4,g=((bin>>5)&31)*8+4,b=(bin&31)*8+4;
            int distance=INT_MAX,best=0;
            for(int i=0;i<colorCount;++i) {
                const int dr=r-colors[i].Red,dg=g-colors[i].Green,db=b-colors[i].Blue;
                const int d=dr*dr+dg*dg+db*db;
                if(d<distance) { distance=d; best=i; }
            }
            lookup[bin]=GifByteType(best);
        }
    }
    QSaveFile output(path);
    if(!output.open(QIODevice::WriteOnly)) return output.errorString();
    int error=0;
    std::unique_ptr<GifFileType,GifCloser> gif(EGifOpen(&output,[](GifFileType* file,const GifByteType* bytes,int count) {
        return int(static_cast<QSaveFile*>(file->UserData)->write(reinterpret_cast<const char*>(bytes),count));
    },&error));
    if(!gif) return gifError(error);
    std::unique_ptr<ColorMapObject,decltype(&GifFreeMapObject)> map(GifMakeMapObject(256,colors.data()),GifFreeMapObject);
    if(!map) return QStringLiteral("GIFパレット用メモリーを確保できません");
    QTransform transform; transform.rotate(rotation);
    const QSize target=transform.mapRect(QRect(QPoint(),size)).size();
    EGifSetGifVersion(gif.get(),true);
    if(EGifPutScreenDesc(gif.get(),target.width(),target.height(),8,hasTransparency?transparentIndex:0,map.get())==GIF_ERROR)
        return gifError(gif->Error);
    if(loops!=0) {
        const int repeats=loops<0?0:loops;
        const char data[]={1,char(repeats&255),char((repeats>>8)&255)};
        if(!applicationBlock(gif.get(),"NETSCAPE2.0",QByteArray(data,3))) return gifError(gif->Error);
    }
    if(!applicationBlock(gif.get(),"ICCRGBG1012",srgbProfile())) return gifError(gif->Error);
    constexpr int bayer[4][4]={{0,8,2,10},{12,4,14,6},{3,11,1,9},{15,7,13,5}};
    QVector<GifPixelType> line(target.width());
    for(int frame=0;frame<frames.size();++frame) {
        if(cancelled) return cancelledMessage();
        const auto rgba=frames[frame].transformed(transform).convertToFormat(QImage::Format_RGBA8888);
        if(rgba.isNull()) return QStringLiteral("GIF変換用メモリーを確保できません");
        const GraphicsControlBlock control{hasTransparency?DISPOSE_BACKGROUND:DISPOSE_DO_NOT,false,delays[frame]/10,hasTransparency?transparentIndex:NO_TRANSPARENT_COLOR};
        GifByteType extension[4]; EGifGCBToExtension(&control,extension);
        if(EGifPutExtension(gif.get(),GRAPHICS_EXT_FUNC_CODE,4,extension)==GIF_ERROR ||
           EGifPutImageDesc(gif.get(),0,0,target.width(),target.height(),false,nullptr)==GIF_ERROR) return gifError(gif->Error);
        for(int y=0;y<rgba.height();++y) {
            if(cancelled) return cancelledMessage();
            const auto* row=rgba.constScanLine(y);
            for(int x=0;x<rgba.width();++x) {
                const auto* p=row+x*4;
                if(p[3]<128) line[x]=transparentIndex;
                else if(exactPalette) line[x]=GifByteType(exact.value(qRgb(p[0],p[1],p[2])));
                else {
                    const int d=bayer[y&3][x&3]-8;
                    line[x]=lookup[colorBin(qBound(0,int(p[0])+d,255),qBound(0,int(p[1])+d,255),qBound(0,int(p[2])+d,255))];
                }
            }
            if(EGifPutLine(gif.get(),line.data(),line.size())==GIF_ERROR) return gifError(gif->Error);
        }
        if(progress) progress(frames.size()+frame+1);
    }
    if(EGifCloseFile(gif.release(),&error)==GIF_ERROR) return gifError(error);
    if(cancelled) return cancelledMessage();
    if(!output.commit()) return output.errorString();
    return {};
}
}
