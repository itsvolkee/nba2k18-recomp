// Finds the compiler's register save/restore helpers in an Xbox 360 XEX.
// Prints only addresses and match counts, never code.
#include <image.h>
#include <file.h>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <string>

static uint32_t be32(const uint8_t* p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

struct Pattern { const char* name; std::vector<uint32_t> words; std::vector<uint32_t> mask; };

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: scan default.xex [dump.bin]\n"); return 1; }
    auto file = LoadFile(argv[1]);
    if (file.empty()) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }
    Image image = Image::ParseImage(file.data(), file.size());
    printf("base 0x%zx size 0x%x entry 0x%zx\n", image.base, image.size, image.entry_point);
    for (auto& s : image.sections)
        printf("section %-8s 0x%08zx size 0x%08x %s\n", s.name.c_str(), s.base, s.size, (s.flags & SectionFlags_Code) ? "code" : "data");

    const uint32_t ALL = 0xFFFFFFFF;
    std::vector<Pattern> pats = {
        // std r14,-0x98(r1); std r15,-0x90(r1)
        {"savegprlr_14", {0xF9C1FF68, 0xF9E1FF70}, {ALL, ALL}},
        // ld  r14,-0x98(r1); ld  r15,-0x90(r1)
        {"restgprlr_14", {0xE9C1FF68, 0xE9E1FF70}, {ALL, ALL}},
        // stfd f14,-0x90(r12); stfd f15,-0x88(r12)
        {"savefpr_14",   {0xD9CCFF70, 0xD9ECFF78}, {ALL, ALL}},
        // lfd  f14,-0x90(r12); lfd  f15,-0x88(r12)
        {"restfpr_14",   {0xC9CCFF70, 0xC9ECFF78}, {ALL, ALL}},
        // li r11,-0x120; stvx v14,r11,r12
        {"savevmx_14",   {0x3960FEE0, 0x7DCB61CE}, {ALL, ALL}},
        // li r11,-0x120; lvx  v14,r11,r12
        {"restvmx_14",   {0x3960FEE0, 0x7DCB60CE}, {ALL, ALL}},
        // li r11,-0x400; stvx128 v64,r11,r12  (opcode 4, VMX128 store, any vD encoding bits)
        {"savevmx_64",   {0x3960FC00, 0x100B61C3}, {ALL, 0xFC1FFFF3}},
        // li r11,-0x400; lvx128  v64,r11,r12
        {"restvmx_64",   {0x3960FC00, 0x100B60C3}, {ALL, 0xFC1FFFF3}},
    };

    for (auto& p : pats)
    {
        std::vector<size_t> hits;
        for (auto& s : image.sections)
        {
            if (!(s.flags & SectionFlags_Code)) continue;
            for (uint32_t off = 0; off + 4 * p.words.size() <= s.size; off += 4)
            {
                bool ok = true;
                for (size_t i = 0; i < p.words.size() && ok; i++)
                    ok = (be32(s.data + off + 4 * i) & p.mask[i]) == (p.words[i] & p.mask[i]);
                if (ok) hits.push_back(s.base + off);
            }
        }
        printf("%-13s %zu hit(s):", p.name, hits.size());
        for (size_t i = 0; i < hits.size() && i < 8; i++) printf(" 0x%08zx", hits[i]);
        printf("\n");
    }

    // longjmp/setjmp have no fixed sequence; leave them to manual analysis.
    if (argc >= 3)
    {
        FILE* f = fopen(argv[2], "wb");
        fwrite(image.data.get(), 1, image.size, f);
        fclose(f);
        printf("wrote decrypted image to %s\n", argv[2]);
    }
    return 0;
}
