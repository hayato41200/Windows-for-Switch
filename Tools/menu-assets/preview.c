/* PC preview of the touch menu: builds Application/SwitchMenuApp/Draw.c as is
 * and writes the picture as a PPM.
 *   gcc -O2 -fshort-wchar -DHOST_PREVIEW -I Application/SwitchMenuApp \
 *       Tools/menu-assets/preview.c Application/SwitchMenuApp/Draw.c -o preview
 *   ./preview out.ppm [focus] [pressed] [touchx touchy]
 */
#include <stdio.h>
#include <stdlib.h>
#include "Draw.h"

int main(int argc, char **argv)
{
    static UINT32 canvas[LW * LH];
    MENU_VIEW view = { 0 };
    FILE *f;
    int i;

    view.Focus = argc > 2 ? (UINTN)atoi(argv[2]) : 0;
    view.Pressed = argc > 3 ? atoi(argv[3]) : -1;
    view.TouchX = argc > 5 ? atoi(argv[4]) : -1;
    view.TouchY = argc > 5 ? atoi(argv[5]) : -1;
    view.TouchDriver = 1;
    view.ShowTouch = 1;
    view.Version = L"R422H  -  UEFI  -  hekate chain";
    if (argc > 6) view.Status = L"\\bootloader\\update.bin not found on the SD.";
    MenuRender(canvas, NULL, &view);

    f = fopen(argv[1], "wb");
    if (f == NULL) return 1;
    fprintf(f, "P6\n%d %d\n255\n", LW, LH);
    for (i = 0; i < LW * LH; i++) {
        unsigned char px[3] = { (unsigned char)(canvas[i] >> 16), (unsigned char)(canvas[i] >> 8), (unsigned char)canvas[i] };
        fwrite(px, 1, 3, f);
    }
    fclose(f);
    return 0;
}
