// Probe the MU2000 firmware for the real FX-parameter SysEx addresses.
//
// Read-back uses Parameter Requests (F0 43 30 4C ah am al F7), answered with
// F0 43 10 4C ah am al data... F7 -- the same round trip xgtest uses. (An
// earlier version used Bulk Dump Requests, 0x20, which the firmware does not
// answer for these blocks, so every line printed "ignored".)
#include "mu2000.h"
#include <cstdio>
#include <string>
#include <vector>

namespace {
constexpr u32 RATE = 44100;
struct rig {
    mu2000 mu; u64 samples = 0;
    void send(const std::vector<u8>& m){ for (u8 b : m) mu.midi_in(b, 0); }
    std::vector<u8> pump(u32 ms){
        std::vector<u8> out; s32 l, r; u64 until = samples + u64(ms)*RATE/1000;
        for (; samples < until; samples++){ mu.run_sample(l,r); u8 b; while (mu.midi_out_take(b)) out.push_back(b); }
        return out;
    }
    // Parameter Request -> return the data bytes of the matching answer, or {}.
    std::vector<u8> req(u8 ah,u8 am,u8 al){
        send({0xF0,0x43,0x30,0x4C,ah,am,al,0xF7});
        auto r = pump(200);
        // Find F0 43 10 4C ah am al <data...> F7
        for (size_t i=0; i+8 < r.size(); i++){
            if (r[i]==0xF0 && r[i+1]==0x43 && r[i+2]==0x10 && r[i+3]==0x4C
                && r[i+4]==ah && r[i+5]==am && r[i+6]==al){
                std::vector<u8> d;
                for (size_t j=i+7; j<r.size() && r[j]!=0xF7; j++) d.push_back(r[j]);
                return d;
            }
        }
        return {};
    }
    void w1(u8 ah,u8 am,u8 al,u8 d){ send({0xF0,0x43,0x10,0x4C,ah,am,al,d,0xF7}); pump(40); }
    void w2(u8 ah,u8 am,u8 al,u8 d1,u8 d2){ send({0xF0,0x43,0x10,0x4C,ah,am,al,d1,d2,0xF7}); pump(40); }
};
const char* show(const std::vector<u8>& d){
    static char buf[64];
    if (d.empty()) { std::snprintf(buf,sizeof buf,"(no answer)"); return buf; }
    int n=0;
    for (u8 b : d) {
        if (n >= static_cast<int>(sizeof(buf)))
            break;
        int written = std::snprintf(buf+n, sizeof(buf)-n, "%02X ", b);
        if (written < 0 || written >= static_cast<int>(sizeof(buf)-n))
            break;
        n += written;
    }
    if (n>0) buf[n-1]=0;
    return buf;
}
}

int main(int argc, char** argv){
    if (argc < 2){ std::fprintf(stderr,"fx_probe <rom dir>\n"); return 2; }
    std::string dir = argv[1];
    rig g;
    if (!g.mu.load_program(dir+"/mu2000_flash.bin") || !g.mu.load_wave(dir+"/dump")){
        std::fprintf(stderr,"%s\n", g.mu.error().c_str()); return 2; }
    g.mu.load_sintab(dir+"/standin/sin-table.bin");
    g.mu.reset();
    s32 l,r; for (; g.samples < 30*RATE && !g.mu.midi_ready(); g.samples++) g.mu.run_sample(l,r);
    std::printf("boot %.2fs\n", double(g.samples)/RATE);
    g.pump(500);

    // ---- VARIATION (Tremolo): 2-byte atomic P1-10 at 42+2(N-1), 1-byte P11-16 at 70+.
    std::printf("\n== VARIATION param read-back (type = TREMOLO) ==\n");
    g.w2(0x02,0x01,0x40,0x46,0x00);                        // type = TREMOLO
    for (int P=1; P<=10; P++){
        u8 al = 0x42 + 2*(P-1);
        g.w2(0x02,0x01,al,0x00,0x55);                      // 2-byte atomic: MSB=0 LSB=0x55
        std::printf("  P%-2d  02 01 %02X (2-byte)  reads %s\n", P, al, show(g.req(0x02,0x01,al)));
    }
    for (int P=11; P<=16; P++){
        u8 al = 0x70 + (P-11);
        g.w1(0x02,0x01,al,0x55);
        std::printf("  P%-2d  02 01 %02X (1-byte)  reads %s\n", P, al, show(g.req(0x02,0x01,al)));
    }

    // ---- INSERTION 1, 1-byte type (Tremolo): P1-10 at 02-0B, P11-16 at 20-25.
    std::printf("\n== INSERTION 1 param read-back (1-byte type = TREMOLO) ==\n");
    g.w1(0x03,0x00,0x0C,0x00); g.w2(0x03,0x00,0x00,0x46,0x00);
    for (int P=1; P<=10; P++){
        u8 al = 0x01 + P;
        g.w1(0x03,0x00,al,0x55);
        std::printf("  P%-2d  03 00 %02X (1-byte)  reads %s\n", P, al, show(g.req(0x03,0x00,al)));
    }
    for (int P=11; P<=16; P++){
        u8 al = 0x20 + (P-11);
        g.w1(0x03,0x00,al,0x55);
        std::printf("  P%-2d  03 00 %02X (1-byte)  reads %s\n", P, al, show(g.req(0x03,0x00,al)));
    }
    std::printf("  (03 00 0D-11 are MW/bend/CAT/AC1/AC2 control depths, not params:)\n");
    for (u8 al=0x0D; al<=0x11; al++)
        std::printf("    03 00 %02X  reads %s\n", al, show(g.req(0x03,0x00,al)));

    // ---- INSERTION 1, 2-byte type (DIST+DELAY 5F): P1-10 at 30-43 (atomic).
    std::printf("\n== INSERTION 1 param read-back (2-byte type = DIST+DELAY 5F) ==\n");
    g.w2(0x03,0x00,0x00,0x5F,0x00);
    for (int P=1; P<=10; P++){
        u8 al = 0x30 + 2*(P-1);
        g.w2(0x03,0x00,al,0x01,0x23);                      // 2-byte atomic: 0x01,0x23
        std::printf("  P%-2d  03 00 %02X (2-byte)  reads %s\n", P, al, show(g.req(0x03,0x00,al)));
    }
    return 0;
}
