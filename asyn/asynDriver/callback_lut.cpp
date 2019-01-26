#include "callback_lut.h"

callback_LUT nodeLUT;

void clear_LUT(void *pasynPvt){
    nodeLUT.clear(pasynPvt);
}
void enable_LUT(int enablekey){
    nodeLUT.enable(enablekey!=0);
}
