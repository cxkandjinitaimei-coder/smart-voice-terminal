#ifndef BRIDGE_MOTION_CACHE_H
#define BRIDGE_MOTION_CACHE_H
#include "bridge_motion.h"

/* One small profile file, separate from /mic-audio.pcm and never formatted by
 * this code. Atomic rename preserves the last good profile after interrupted
 * writes. The same production code is tested with a host filesystem mock. */
template<class FileSystem>
class SavedMotionProfile {
    FileSystem *fs=nullptr;
    char saved[205]={0};
    uint32_t rev=0;
    bool durable=false;
    static const char *path(){return "/servo-profile.hex";}
    static const char *temporary(){return "/servo-profile.tmp";}
public:
    void begin(FileSystem &storage) {
        fs=&storage; saved[0]=0;rev=0;durable=false;
        auto f=fs->open(path(),"r");
        if(!f) return;
        size_t n=f.size();
        if(n<sizeof(saved) && n && f.read((uint8_t*)saved,n)==(int)n) {
            saved[n]=0;
            if(motionProfileValid(saved,rev)) durable=true;
        }
        f.close();
        if(!durable) {saved[0]=0;rev=0;}
    }
    bool accept(const char *wire) {
        uint32_t revision=0;
        if(!motionProfileValid(wire,revision)) return false;
        if(durable && rev==revision && !strcmp(saved,wire)) return true;
        // Apply valid live data even if storage is unavailable, but report that
        // the offline copy was not saved. Do not remove the previous good file.
        strcpy(saved,wire);rev=revision;durable=false;
        if(!fs) return true;
        auto f=fs->open(temporary(),"w");
        if(!f) return true;
        size_t n=strlen(wire);
        bool ok=f.write((const uint8_t*)wire,n)==n;
        f.close();
        if(ok) {
            auto check=fs->open(temporary(),"r");char content[205];
            ok=check && check.size()==n && check.read((uint8_t*)content,n)==(int)n;
            if(check) check.close();
            if(ok) {content[n]=0;uint32_t r=0;ok=motionProfileValid(content,r) && r==rev;}
        }
        if(ok) durable=fs->rename(temporary(),path());
        if(!durable) fs->remove(temporary());
        return true;
    }
    const char *wire() const{return saved;}
    uint32_t revision() const{return rev;}
    bool cached() const{return durable;}
};
#endif
