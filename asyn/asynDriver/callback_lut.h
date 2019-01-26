/*callback_lut.h
 *
 * LUT to perform faster callbacks
 * Author: Florian Luetticke
 *
 * Created Januarry 22, 2019
 */


#ifndef CALLBACK_LUT_H
#define CALLBACK_LUT_H

#include <asynDriver.h>
#include <ellLib.h>

#ifdef __cplusplus
#include <utility>
#include <unordered_map>
#include <vector>
#include <algorithm>
extern "C"{
#endif
void clear_LUT(void *pasynPvt);
void enable_LUT(int enablekey);
#ifdef __cplusplus
}

class callback_LUT
{
private:
    std::unordered_map<void *, std::unordered_map<int64_t,std::vector<const interruptNode*>>> nodes;
    std::vector<const interruptNode*> emptyVec;
    bool enabled;
    inline int64_t make_key(int command, int addr){
        return (int64_t(addr)*1l<<32)+command;
    }
public:
    callback_LUT():enabled(false){}

    // In:   key values for LUT
    // In:   value.
    // Adds a value to the vector in the map. If vector does not exist, create it.
    inline  void addNode(void *pasynPvt,int command, int addr,const interruptNode* item){
        if(!enabled) return;
        auto key=make_key(command, addr);
        auto &nodevec=nodes[pasynPvt][key];
        if(std::find(begin(nodevec),end(nodevec),item)==end(nodevec)){
            nodevec.push_back(item);
        }
    }
    // In:   key values for LUT
    // Adds an empty vector in the map. If key exists, warn, but do nothing!
    inline  void addEmptyNode(void *pasynPvt,int command, int addr){
        if(!enabled) return;
        auto key=make_key(command, addr);
        auto &nodemap=nodes[pasynPvt];
        if(nodemap.find(key)==nodemap.end()){
            nodemap.emplace(key, std::vector<const interruptNode*>());
        } else {
            printf("WARNING: Can't create empty node in LUT %d %d %p. This value already exists!\n", command,addr,pasynPvt);
        }
    }
    // In:   key values for LUT
    // out:  pair(vector,bool). The second value indicates if the key was available in the LUT.
    inline std::pair<std::vector<const interruptNode*>,bool> getNodes(void *pasynPvt,int command, int addr){
        if(!enabled)return std::make_pair(emptyVec,false);
        auto key=make_key(command, addr);
        auto &nodemap=nodes[pasynPvt];
        if(nodemap.find(key)==nodemap.end()) return std::make_pair(emptyVec,false);
        return std::make_pair(nodemap[key],true);
    }

    inline void clear(void *pasynPvt){
        if(!enabled) return;
        nodes[pasynPvt].clear();
    }

    inline bool is_enabled(){return enabled;}
    void enable(bool val){
        if(!val && enabled){
            enabled=val;  //first disable.
            for(auto& m:nodes){
                m.second.clear();
            }
        }
        enabled=val;
    }
};

extern callback_LUT nodeLUT;
#endif

#endif // CALLBACK_LUT_H
