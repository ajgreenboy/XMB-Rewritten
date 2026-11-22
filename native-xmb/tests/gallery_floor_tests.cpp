// Photo Gallery floor brightness levels (src/gallery_floor.h): the level -> factor table of hknw_plugin/parameters.txt
// FLOOR_BRIGHTNESS_TYPE0..5, clamping, the parameter names and the option labels.
#include "gallery_floor.h"
#include <cmath>
#include <cstdio>
#include <cstring>
using namespace xmb::galleryfloor;
static int failures=0;
#define CHECK(c) do{if(!(c)){std::printf("FAIL line %d: %s\n",__LINE__,#c);++failures;}}while(0)
static bool near(double a,double b,double e=1e-12){return std::abs(a-b)<e;}
int main() {
    static_assert(levelCount==6);
    static_assert(defaultLevel==0);
    // The six factors of the 4.93 parameters.txt, in order, and the standard level is the identity.
    const double expected[6]={1.0,0.83,0.66,0.5,0.3,0.1};
    for(int i=0;i<6;++i)CHECK(near(fallbackFactor(i),expected[i]));
    CHECK(near(fallbackFactor(defaultLevel),1.0));
    // Each level darkens or keeps the one before it.
    for(int i=1;i<levelCount;++i)CHECK(fallbackFactor(i)<fallbackFactor(i-1));
    // Out-of-range stored values clamp to the nearest level.
    CHECK(clampLevel(-3)==0 && clampLevel(0)==0 && clampLevel(5)==5 && clampLevel(99)==5);
    CHECK(near(fallbackFactor(-1),1.0) && near(fallbackFactor(6),0.1));
    // The parameters.txt names.
    CHECK(std::strcmp(parameterName(0),"FLOOR_BRIGHTNESS_TYPE0")==0);
    CHECK(std::strcmp(parameterName(3),"FLOOR_BRIGHTNESS_TYPE3")==0);
    CHECK(std::strcmp(parameterName(5),"FLOOR_BRIGHTNESS_TYPE5")==0);
    CHECK(std::strcmp(parameterName(42),"FLOOR_BRIGHTNESS_TYPE5")==0);
    // The option labels.
    CHECK(std::strcmp(levelLabel(0),"Normal")==0 && std::strcmp(levelLabel(1),"-1")==0 && std::strcmp(levelLabel(5),"-5")==0);
    if(failures){std::printf("%d failure(s)\n",failures);return 1;}
    std::puts("gallery floor tests passed");
    return 0;
}
