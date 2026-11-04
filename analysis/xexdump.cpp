// XexDump: decrypts and decompresses an Xbox 360 executable into the raw
// memory image the runtime loads (default.img). Built by CI against XenonUtils.
//
//   XexDump default.xex default.img
#include <image.h>
#include <file.h>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: XexDump <default.xex> <default.img>\n");
        return 1;
    }
    auto file = LoadFile(argv[1]);
    if (file.empty())
    {
        fprintf(stderr, "cannot read %s\n", argv[1]);
        return 1;
    }
    Image image = Image::ParseImage(file.data(), file.size());
    if (!image.data || image.size == 0)
    {
        fprintf(stderr, "%s could not be decrypted\n", argv[1]);
        return 1;
    }
    FILE* f = fopen(argv[2], "wb");
    if (!f)
    {
        fprintf(stderr, "cannot write %s\n", argv[2]);
        return 1;
    }
    fwrite(image.data.get(), 1, image.size, f);
    fclose(f);
    printf("image base 0x%zx size 0x%x entry 0x%zx -> %s\n", image.base, image.size, image.entry_point, argv[2]);
    return 0;
}
