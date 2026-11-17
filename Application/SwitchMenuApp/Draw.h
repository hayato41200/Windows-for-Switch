/** @file
  SwitchMenuApp drawing: the landscape 1280 x 720 menu picture.

  No UEFI services in here, so Tools/menu-assets/preview.c can build the same
  code on a PC (-DHOST_PREVIEW) and write the picture to a file.
**/

#ifndef SWITCH_MENU_DRAW_H_
#define SWITCH_MENU_DRAW_H_

#ifdef HOST_PREVIEW
#include <stdint.h>
typedef uint8_t         UINT8;
typedef int8_t          INT8;
typedef uint16_t        UINT16;
typedef int16_t         INT16;
typedef uint32_t        UINT32;
typedef int32_t         INT32;
typedef uint64_t        UINT64;
typedef int64_t         INTN;
typedef uint64_t        UINTN;
typedef unsigned short  CHAR16;          // build with -fshort-wchar
typedef unsigned char   BOOLEAN;
#define VOID   void
#define CONST  const
#define STATIC static
#define TRUE   1
#define FALSE  0
#define NULL   ((void *)0)
#else
#include <Uefi.h>
#endif

#define LW  1280
#define LH  720

typedef enum {
  ActBootWindows,
  ActHekate,
  ActSettings,
  ActPowerOff,
  ActRotate,
  ActCount
} MENU_ACTION;

typedef struct {
  INTN  X, Y, W, H;
} MENU_RECT;

typedef struct {
  UINTN         Focus;          // ActBootWindows..ActPowerOff
  INTN          Pressed;        // button under a finger, or -1
  INTN          TouchX;         // last touch, landscape; -1 = none
  INTN          TouchY;
  BOOLEAN       Touching;
  BOOLEAN       TouchDriver;    // SwitchTouchDxe present
  BOOLEAN       ShowTouch;      // draw the touch marker and coordinates
  CONST CHAR16  *Status;        // message line, or NULL
  CONST CHAR16  *Version;       // under the title
} MENU_VIEW;

extern CONST MENU_RECT  gMenuButtons[ActCount];

// Background (gradient and light ribbons) into an LW x LH buffer, once.
VOID
MenuRenderBackground (
  UINT32  *Buffer
  );

// Canvas: LW x LH pixels, 0x00RRGGBB (the GOP BGRX byte order). Background:
// from MenuRenderBackground, or NULL to draw it each time.
VOID
MenuRender (
  UINT32           *Canvas,
  CONST UINT32     *Background,
  CONST MENU_VIEW  *View
  );

INTN
MenuHitTest (
  INTN  X,
  INTN  Y
  );

#endif
