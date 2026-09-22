// SPDX-License-Identifier: GPL-3.0-or-later
#include "tile_plan.h"
#include <cassert>
#include <iostream>
int main() {
    for (int width : {1,2,447,448,479,480,481,897})
        for (int height : {1,2,237,238,269,270,271,477}) {
            std::vector<int> coverage(size_t(width)*height,0);
            for (const auto& tile : sr::plan(width,height,16))
                for (int y=tile.y;y<tile.y+tile.height;++y)
                    for(int x=tile.x;x<tile.x+tile.width;++x) ++coverage[size_t(y)*width+x];
            for(int count : coverage) if (count!=1) return 1;
            for(int p=-500;p<1000;++p)
                if(sr::reflect(p,width)<0 || sr::reflect(p,width)>=width) return 2;
        }
    if(sr::reflect(-1,4)!=1 || sr::reflect(4,4)!=2 || sr::reflect(0,1)!=0) return 3;
    try { sr::plan(0,10,16); return 4; } catch(const std::runtime_error&) {}
    std::cout << "Tile coverage, singleton padding and reflect101 boundaries passed.\n";
}
