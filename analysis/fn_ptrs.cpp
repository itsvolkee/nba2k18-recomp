// fn_ptrs: finds functions that are only reached through pointers stored in
// data (static-initializer tables, vtables, callbacks). Nothing calls these
// with `bl`, so XenonRecomp's discovery can miss them.
//
//   fn_ptrs default.xex ppc_func_mapping.cpp switch_tables.toml > functions.toml
//
// A data word is a candidate when it is 4-byte aligned, points into .text,
// isn't already a known function start or a jump-table case, and the word
// before it ends a function (blr, b, bctr, padding) or a known function ends
// right there. Each candidate's size comes from Function::Analyze (with the
// jump tables known), clipped at the next known start. Prints addresses only.
#include <function.h>
#include <image.h>
#include <file.h>
#include <disasm.h>
#include <byteswap.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>

static std::string ReadText(const char* p)
{
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: fn_ptrs default.xex ppc_func_mapping.cpp switch_tables.toml\n");
        return 1;
    }
    auto file = LoadFile(argv[1]);
    Image image = Image::ParseImage(file.data(), file.size());
    const Section* text = image.Find(".text");
    if (!text) { fprintf(stderr, "no .text\n"); return 1; }
    const size_t tBegin = text->base, tEnd = text->base + text->size;
    auto word = [&](size_t a) -> uint32_t
    {
        const void* p = image.Find(a);
        return p ? ByteSwap(*(const uint32_t*)p) : 0;
    };

    // known function starts from the recompiler's output
    std::set<size_t> known;
    {
        std::string m = ReadText(argv[2]);
        std::regex re(R"(\{ 0x([0-9A-Fa-f]+), )");
        for (auto it = std::sregex_iterator(m.begin(), m.end(), re); it != std::sregex_iterator(); ++it)
            known.insert(std::stoul((*it)[1].str(), nullptr, 16));
    }

    // jump tables: case targets are code pointers in data but not functions
    std::set<size_t> caseLabels;
    std::unordered_map<size_t, std::vector<size_t>> switchTargets;
    {
        std::string t = ReadText(argv[3]);
        size_t pos = 0;
        while ((pos = t.find("base = 0x", pos)) != std::string::npos)
        {
            size_t bctr = std::stoul(t.substr(pos + 7, 10), nullptr, 16);
            size_t lb = t.find("labels", pos), le = t.find(']', lb);
            std::string body = t.substr(lb, le - lb);
            std::regex re(R"(0x([0-9A-Fa-f]{8}))");
            auto& v = switchTargets[bctr];
            for (auto it = std::sregex_iterator(body.begin(), body.end(), re); it != std::sregex_iterator(); ++it)
            {
                size_t a = std::stoul((*it)[1].str(), nullptr, 16);
                caseLabels.insert(a);
                v.push_back(a);
            }
            pos = le;
        }
    }
    Function::SwitchTargets = &switchTargets;

    auto endsFunction = [&](uint32_t insn)
    {
        return insn == 0x4E800020 || insn == 0x4E800420 || insn == 0 ||          // blr, bctr, padding
               ((insn >> 26) == 18 && !(insn & 1));                              // b (not bl)
    };

    // .pdata functions have known extents: a pointer into one is a label, not a new function
    std::map<size_t, size_t> pdata;   // begin -> end
    if (const Section* pd = image.Find(".pdata"))
        for (size_t off = 0; off + 8 <= pd->size; off += 8)
        {
            uint32_t begin = ByteSwap(*(const uint32_t*)(pd->data + off));
            uint32_t info  = ByteSwap(*(const uint32_t*)(pd->data + off + 4));
            if (begin) pdata[begin] = begin + ((info >> 8) & 0x3FFFFF) * 4;
        }
    // Jump tables: everything from the bctr to its furthest case belongs to one function
    std::map<size_t, size_t> switchSpans;   // begin -> end
    for (auto& [bctr, labels] : switchTargets)
    {
        size_t lo = bctr, hi = bctr;
        for (size_t t : labels) { lo = std::min(lo, t); hi = std::max(hi, t); }
        auto& e = switchSpans[lo];
        e = std::max(e, hi + 4);
    }
    auto insideSpan = [](const std::map<size_t, size_t>& spans, size_t a)
    {
        // spans can overlap, so check every span starting at or before a (they're short)
        for (auto it = spans.upper_bound(a); it != spans.begin();)
        {
            --it;
            if (a >= it->first && a < it->second) return true;
            if (a - it->first > 0x20000) break;
        }
        return false;
    };
    auto insidePdata = [&](size_t a)
    {
        return insideSpan(pdata, a) || insideSpan(switchSpans, a);
    };

    std::set<size_t> candidates;
    size_t dataWords = 0;
    for (auto& s : image.sections)
    {
        if (s.flags & SectionFlags_Code) continue;
        for (size_t off = 0; off + 4 <= s.size; off += 4)
        {
            uint32_t v = ByteSwap(*(const uint32_t*)(s.data + off));
            if (v < tBegin || v >= tEnd || (v & 3)) continue;
            dataWords++;
            if (known.count(v) || caseLabels.count(v) || insidePdata(v)) continue;
            if (!endsFunction(word(v - 4))) continue;
            ppc_insn insn;
            uint32_t raw = *(const uint32_t*)image.Find(v);
            ppc::Disassemble(&raw, v, insn);
            if (!insn.opcode) continue;
            candidates.insert(v);
        }
    }

    // addresses built in code: lis rD,hi ... addi/ori rX,rD,lo (callbacks passed as arguments)
    size_t codeRefs = 0;
    auto consider = [&](size_t v)
    {
        if (v < tBegin || v >= tEnd || (v & 3)) return;
        codeRefs++;
        if (known.count(v) || caseLabels.count(v) || insidePdata(v)) return;
        if (!endsFunction(word(v - 4))) return;
        ppc_insn insn;
        uint32_t raw = *(const uint32_t*)image.Find(v);
        ppc::Disassemble(&raw, v, insn);
        if (insn.opcode) candidates.insert(v);
    };
    for (size_t a = tBegin; a + 4 <= tEnd; a += 4)
    {
        uint32_t x = word(a);
        if ((x >> 26) != 15 || ((x >> 16) & 31) != 0) continue;          // lis rD, hi
        uint32_t rd = (x >> 21) & 31;
        uint32_t hi = x & 0xFFFF;
        for (size_t k = 1; k <= 16; k++)
        {
            uint32_t y = word(a + 4 * k);
            uint32_t op = y >> 26;
            if (op == 14 && ((y >> 16) & 31) == rd)                        // addi rX, rD, lo
            {
                consider(size_t((hi << 16) + uint32_t(int32_t(int16_t(y & 0xFFFF)))) & 0xFFFFFFFF);
                break;
            }
            if (op == 24 && ((y >> 21) & 31) == rd)                        // ori rX, rD, lo
            {
                consider((hi << 16) | (y & 0xFFFF));
                break;
            }
            // stop if rD is overwritten by something else
            if (((y >> 21) & 31) == rd && op != 36 && op != 37 && op != 44 && op != 45 && op != 38 && op != 39 && op != 62)
                break;
        }
    }

    // sizes: analyze, then clip at the next known start or candidate
    std::set<size_t> starts = known;
    starts.insert(candidates.begin(), candidates.end());
    printf("# functions reached only through data pointers (generated by analysis/fn_ptrs.cpp)\n");
    printf("# %zu code pointers in data, %zu built in code, %zu new function starts\n", dataWords, codeRefs, candidates.size());
    printf("functions = [\n");
    size_t emitted = 0;
    for (size_t a : candidates)
    {
        // Size by linear scan, bounded by the next known start: the function ends
        // at the first return/jump/padding past every forward branch target seen.
        auto next = starts.upper_bound(a);
        size_t end = next != starts.end() ? *next : tEnd;
        auto pd = pdata.upper_bound(a);
        if (pd != pdata.end() && pd->first < end) end = pd->first;
        size_t maxTarget = a, size = end - a;
        for (size_t pc = a; pc < end; pc += 4)
        {
            uint32_t x = word(pc);
            uint32_t op = x >> 26;
            if (op == 16 && !(x & 1))                                     // bc (not bcl)
            {
                size_t t = pc + int32_t(int16_t(x & 0xFFFC));
                if (t > maxTarget && t < end) maxTarget = t;
            }
            else if (op == 18 && !(x & 1))                                // b
            {
                int32_t li = int32_t(x & 0x03FFFFFC);
                if (li & 0x02000000) li |= int32_t(0xFC000000);
                size_t t = pc + li;
                if (t > pc && t < end && t > maxTarget) { maxTarget = t; continue; }
                if (pc >= maxTarget) { size = pc + 4 - a; break; }           // tail call / end
            }
            else if (x == 0x4E800420)                                     // bctr
            {
                auto sw = switchTargets.find(pc);
                if (sw != switchTargets.end())
                {
                    for (size_t t : sw->second) if (t < end && t > maxTarget) maxTarget = t;
                    continue;
                }
                if (pc >= maxTarget) { size = pc + 4 - a; break; }
            }
            else if (x == 0x4E800020 || x == 0)                           // blr / padding
            {
                if (pc >= maxTarget) { size = (x ? pc + 4 : pc) - a; break; }
            }
        }
        if (size == 0) continue;
        printf("    { address = 0x%08zX, size = 0x%zX },\n", a, size);
        emitted++;
    }
    printf("]\n");
    fprintf(stderr, "code pointers in data: %zu, built in code: %zu, new function starts: %zu\n", dataWords, codeRefs, emitted);
    return 0;
}
