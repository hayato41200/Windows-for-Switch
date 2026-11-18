/** @file
  SwitchMenuApp drawing: background, glass cards, glow, anti-aliased text and
  icons on a 1280 x 720 canvas. Integer arithmetic only (no FPU in the firmware).
**/

#include "Draw.h"
#include "Assets.h"

// ---------------------------------------------------------------- layout

CONST MENU_RECT  gMenuButtons[ActCount] = {
  {   68, 172, 556, 180 },   // Boot Windows
  {  656, 172, 556, 180 },   // Return to hekate
  {   68, 380, 556, 180 },   // UEFI settings
  {  656, 380, 556, 180 },   // Power off
  { 1066,  40, 158,  58 },   // Rotate
};

typedef struct {
  CONST CHAR16     *Title;
  CONST CHAR16     *Sub;
  CONST MENU_ICON  *Icon;
} CARD_TEXT;

STATIC CONST CARD_TEXT  mCards[4] = {
  { L"Boot Windows",     L"Windows Boot Manager",      &mIconWindows },
  { L"Return to hekate", L"Reboot to the hekate menu", &mIconPeak    },
  { L"UEFI settings",    L"Boot Manager and setup",    &mIconChip    },
  { L"Power off",        L"Turn the Switch off",       &mIconPower   },
};

#define CARD_RADIUS   24
#define PILL_RADIUS   29

#define C_TEXT        0xFFFFFF
#define C_SUB         0xB8C6DE
#define C_SUB_FOCUS   0xDCE8FF
#define C_CARD        0x14223F
#define C_CARD_EDGE   0x6F8FC4
#define C_FOCUS_TOP   0x2F7DFF
#define C_FOCUS_BOT   0x0D4AD6
#define C_PRESS_TOP   0x58A0FF
#define C_PRESS_BOT   0x2466F0
#define C_FOCUS_EDGE  0x8FD0FF
#define C_GLOW        0x2E8BFF
#define C_WARN        0xFFC857
#define C_MARK        0xFF4D6D

STATIC UINT32  *mCanvas;

// ---------------------------------------------------------------- pixels

STATIC
VOID
Blend (
  INTN    X,
  INTN    Y,
  UINT32  Color,
  UINT32  Alpha      // 0..255
  )
{
  UINT32  *P, D, R, G, B;

  if ((X < 0) || (Y < 0) || (X >= LW) || (Y >= LH) || (Alpha == 0)) {
    return;
  }
  P = &mCanvas[Y * LW + X];
  if (Alpha >= 255) {
    *P = Color;
    return;
  }
  D = *P;
  R = (((Color >> 16) & 0xFF) * Alpha + ((D >> 16) & 0xFF) * (255 - Alpha) + 127) / 255;
  G = (((Color >> 8) & 0xFF) * Alpha + ((D >> 8) & 0xFF) * (255 - Alpha) + 127) / 255;
  B = ((Color & 0xFF) * Alpha + (D & 0xFF) * (255 - Alpha) + 127) / 255;
  *P = (R << 16) | (G << 8) | B;
}

STATIC
UINT32
Mix (
  UINT32  A,
  UINT32  B,
  UINT32  T,      // 0..256: 0 = A
  UINT32  Total
  )
{
  UINT32  R, G, Bl;

  R  = (((A >> 16) & 0xFF) * (Total - T) + ((B >> 16) & 0xFF) * T) / Total;
  G  = (((A >> 8) & 0xFF) * (Total - T) + ((B >> 8) & 0xFF) * T) / Total;
  Bl = ((A & 0xFF) * (Total - T) + (B & 0xFF) * T) / Total;
  return (R << 16) | (G << 8) | Bl;
}

STATIC
UINT32
ISqrt (
  UINT32  N
  )
{
  UINT32  R = 0, Bit = 1U << 30;

  while (Bit > N) {
    Bit >>= 2;
  }
  while (Bit != 0) {
    if (N >= R + Bit) {
      N -= R + Bit;
      R  = (R >> 1) + Bit;
    } else {
      R >>= 1;
    }
    Bit >>= 2;
  }
  return R;
}

// Coverage (0..16) of pixel (Px, Py) by a rounded rectangle.
STATIC
UINT32
Coverage (
  INTN  Px,
  INTN  Py,
  INTN  X,
  INTN  Y,
  INTN  W,
  INTN  H,
  INTN  R
  )
{
  INTN  Cx = Px * 16 + 8, Cy = Py * 16 + 8;
  INTN  L = X * 16, T = Y * 16, Rt = (X + W) * 16, Bt = (Y + H) * 16, R16 = R * 16;
  INTN  Dx, Dy, D;

  if ((Cx < L) || (Cx >= Rt) || (Cy < T) || (Cy >= Bt)) {
    return 0;
  }
  Dx = 0;
  Dy = 0;
  if (Cx < L + R16) {
    Dx = L + R16 - Cx;
  } else if (Cx > Rt - R16) {
    Dx = Cx - (Rt - R16);
  }
  if (Cy < T + R16) {
    Dy = T + R16 - Cy;
  } else if (Cy > Bt - R16) {
    Dy = Cy - (Bt - R16);
  }
  if ((Dx == 0) || (Dy == 0)) {
    return 16;
  }
  D = (INTN)ISqrt ((UINT32)(Dx * Dx + Dy * Dy));
  D = R16 - D + 8;
  return (D <= 0) ? 0 : (D >= 16 ? 16 : (UINT32)D);
}

// Fill a rounded rectangle, colour from Top to Bottom (vertical gradient).
STATIC
VOID
FillRound (
  INTN    X,
  INTN    Y,
  INTN    W,
  INTN    H,
  INTN    R,
  UINT32  Top,
  UINT32  Bottom,
  UINT32  Alpha
  )
{
  INTN    Px, Py;
  UINT32  Cov, Color;

  for (Py = Y; Py < Y + H; Py++) {
    Color = (Top == Bottom) ? Top : Mix (Top, Bottom, (UINT32)(Py - Y), (UINT32)H);
    for (Px = X; Px < X + W; Px++) {
      Cov = Coverage (Px, Py, X, Y, W, H, R);
      if (Cov != 0) {
        Blend (Px, Py, Color, Alpha * Cov / 16);
      }
    }
  }
}

// A ring: the rounded rectangle minus the same one inset by T.
STATIC
VOID
StrokeRound (
  INTN    X,
  INTN    Y,
  INTN    W,
  INTN    H,
  INTN    R,
  INTN    T,
  UINT32  Color,
  UINT32  Alpha
  )
{
  INTN    Px, Py, Ri;
  UINT32  Outer, Inner;

  INTN    M = (R > T) ? R : T;

  Ri = (R > T) ? R - T : 0;
  for (Py = Y; Py < Y + H; Py++) {
    for (Px = X; Px < X + W; Px++) {
      // Rows between the corners: only the left and right bands.
      if ((Py >= Y + M) && (Py < Y + H - M) && (Px == X + M) && (X + W - M > Px)) {
        Px = X + W - M - 1;
        continue;
      }
      // Inside the straight middle part nothing of the ring is there.
      if ((Px >= X + R) && (Px < X + W - R) && (Py >= Y + T) && (Py < Y + H - T)) {
        continue;
      }
      if ((Py >= Y + R) && (Py < Y + H - R) && (Px >= X + T) && (Px < X + W - T)) {
        continue;
      }
      Outer = Coverage (Px, Py, X, Y, W, H, R);
      Inner = Coverage (Px, Py, X + T, Y + T, W - 2 * T, H - 2 * T, Ri);
      if (Outer > Inner) {
        Blend (Px, Py, Color, Alpha * (Outer - Inner) / 16);
      }
    }
  }
}

STATIC
VOID
Glow (
  CONST MENU_RECT  *B,
  INTN             R,
  INTN             Size,
  UINT32           Color,
  UINT32           Strength
  )
{
  INTN    K;
  UINT32  A;

  for (K = Size; K >= 1; K--) {
    A = Strength * (UINT32)(Size - K + 1) * (UINT32)(Size - K + 1) / (UINT32)(Size * Size);
    StrokeRound (B->X - K, B->Y - K, B->W + 2 * K, B->H + 2 * K, R + K, 1, Color, A);
  }
}

// ---------------------------------------------------------------- text, icons

STATIC
UINT32
Nibble (
  CONST UINT8  *Bits,
  UINT32       Stride,
  UINT32       X,
  UINT32       Y
  )
{
  UINT8  B = Bits[Y * Stride + X / 2];

  return ((X & 1) != 0) ? (B >> 4) : (B & 0xF);
}

STATIC
INTN
TextWidth (
  CONST MENU_FONT  *Font,
  CONST CHAR16     *Text
  )
{
  INTN  W = 0;

  for (; *Text != 0; Text++) {
    if ((*Text >= 0x20) && (*Text < 0x7F)) {
      W += Font->Glyphs[*Text - 0x20].Advance;
    }
  }
  return W;
}

// Y is the top of the line (the ascender).
STATIC
VOID
DrawText (
  CONST MENU_FONT  *Font,
  INTN             X,
  INTN             Y,
  UINT32           Color,
  CONST CHAR16     *Text
  )
{
  CONST MENU_GLYPH  *G;
  UINT32            Gx, Gy, Stride, A;

  for (; *Text != 0; Text++) {
    if ((*Text < 0x20) || (*Text >= 0x7F)) {
      continue;
    }
    G      = &Font->Glyphs[*Text - 0x20];
    Stride = (G->W + 1) / 2;
    for (Gy = 0; Gy < G->H; Gy++) {
      for (Gx = 0; Gx < G->W; Gx++) {
        A = Nibble (Font->Bits + G->Offset, Stride, Gx, Gy);
        if (A != 0) {
          Blend (X + G->XOff + (INTN)Gx, Y + G->YOff + (INTN)Gy, Color, A * 17);
        }
      }
    }
    X += G->Advance;
  }
}

STATIC
VOID
DrawIcon (
  CONST MENU_ICON  *Icon,
  INTN             X,
  INTN             Y,
  UINT32           Color,
  UINT32           Alpha
  )
{
  UINT32  Ix, Iy, Stride = (Icon->Size + 1) / 2, A;

  for (Iy = 0; Iy < Icon->Size; Iy++) {
    for (Ix = 0; Ix < Icon->Size; Ix++) {
      A = Nibble (Icon->Bits, Stride, Ix, Iy);
      if (A != 0) {
        Blend (X + (INTN)Ix, Y + (INTN)Iy, Color, A * 17 * Alpha / 255);
      }
    }
  }
}

// ---------------------------------------------------------------- background

// Bilinear 8x upscale of the 160 x 90 picture, with a 4 x 4 ordered dither so
// the dark gradients do not band.
STATIC
VOID
DrawBackground (
  VOID
  )
{
  STATIC CONST UINT8  Bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
  INTN                X, Y, Sx, Sy, X0, Y0, X1, Y1, Fx, Fy, C;
  UINT32              Out, Ch, V, D;
  CONST UINT8         *P00, *P01, *P10, *P11;

  for (Y = 0; Y < LH; Y++) {
    Sy = (Y * MENU_BG_H * 256) / LH - 128;
    if (Sy < 0) {
      Sy = 0;
    }
    Y0 = Sy >> 8;
    Fy = Sy & 0xFF;
    Y1 = (Y0 + 1 < MENU_BG_H) ? Y0 + 1 : Y0;
    for (X = 0; X < LW; X++) {
      Sx = (X * MENU_BG_W * 256) / LW - 128;
      if (Sx < 0) {
        Sx = 0;
      }
      X0  = Sx >> 8;
      Fx  = Sx & 0xFF;
      X1  = (X0 + 1 < MENU_BG_W) ? X0 + 1 : X0;
      P00 = &mBackground[(Y0 * MENU_BG_W + X0) * 3];
      P01 = &mBackground[(Y0 * MENU_BG_W + X1) * 3];
      P10 = &mBackground[(Y1 * MENU_BG_W + X0) * 3];
      P11 = &mBackground[(Y1 * MENU_BG_W + X1) * 3];
      D   = Bayer[Y & 3][X & 3];
      Out = 0;
      for (C = 0; C < 3; C++) {
        V = (UINT32)(P00[C] * (256 - Fx) * (256 - Fy) + P01[C] * Fx * (256 - Fy) +
                     P10[C] * (256 - Fx) * Fy + P11[C] * Fx * Fy);   // value << 16
        Ch = (V + (D << 12)) >> 16;
        Out = (Out << 8) | ((Ch > 255) ? 255 : Ch);
      }
      mCanvas[Y * LW + X] = Out;
    }
  }
}

// Light ribbons over the base (Assets.h: per column centre, half width, level;
// a profile across the ribbon with a bright upper rim), added to the pixels.
STATIC
VOID
DrawRibbons (
  VOID
  )
{
  UINTN              K;
  INTN               X, Y, Cy, Hw, T, Y0, Y1;
  UINT32             Level, V, C, P, R, G, B;
  CONST MENU_RIBBON  *Rb;

  for (K = 0; K < sizeof (mRibbons) / sizeof (mRibbons[0]); K++) {
    Rb = &mRibbons[K];
    for (X = 0; X < LW; X++) {
      Level = Rb->Level[X];
      if (Level == 0) {
        continue;
      }
      Cy = Rb->Cy[X];
      Hw = Rb->Hw[X];
      Y0 = Cy - 2 * Hw;
      Y1 = Cy + 2 * Hw;
      if (Y0 < 0) {
        Y0 = 0;
      }
      if (Y1 > LH - 1) {
        Y1 = LH - 1;
      }
      for (Y = Y0; Y <= Y1; Y++) {
        T = (Y - Cy) * 32 / Hw + 64;
        if ((T < 0) || (T > 127)) {
          continue;
        }
        V = mRibbonProfile[T] * Level / 255;        // 0..255
        C = Rb->Color;
        P = mCanvas[Y * LW + X];
        R = ((P >> 16) & 0xFF) + ((C >> 16) & 0xFF) * V / 255;
        G = ((P >> 8) & 0xFF) + ((C >> 8) & 0xFF) * V / 255;
        B = (P & 0xFF) + (C & 0xFF) * V / 255;
        mCanvas[Y * LW + X] = ((R > 255 ? 255 : R) << 16) | ((G > 255 ? 255 : G) << 8) | (B > 255 ? 255 : B);
      }
    }
  }
}

// ---------------------------------------------------------------- widgets

STATIC
VOID
DrawCard (
  UINTN            Index,
  CONST MENU_VIEW  *View
  )
{
  CONST MENU_RECT  *B = &gMenuButtons[Index];
  CONST CARD_TEXT  *T = &mCards[Index];
  BOOLEAN          Focus   = (Index == View->Focus);
  BOOLEAN          Pressed = ((INTN)Index == View->Pressed);
  INTN             Cy      = B->Y + B->H / 2;
  INTN             TextX   = B->X + 168;

  // Card body: dark glass, or the blue gradient when selected.
  if (Pressed) {
    Glow (B, CARD_RADIUS, 16, C_GLOW, 120);
    FillRound (B->X, B->Y, B->W, B->H, CARD_RADIUS, C_PRESS_TOP, C_PRESS_BOT, 245);
    StrokeRound (B->X, B->Y, B->W, B->H, CARD_RADIUS, 3, C_FOCUS_EDGE, 255);
  } else if (Focus) {
    Glow (B, CARD_RADIUS, 16, C_GLOW, 120);
    FillRound (B->X, B->Y, B->W, B->H, CARD_RADIUS, C_FOCUS_TOP, C_FOCUS_BOT, 235);
    StrokeRound (B->X, B->Y, B->W, B->H, CARD_RADIUS, 3, C_FOCUS_EDGE, 255);
  } else {
    FillRound (B->X, B->Y, B->W, B->H, CARD_RADIUS, C_CARD, C_CARD, 175);
    StrokeRound (B->X, B->Y, B->W, B->H, CARD_RADIUS, 2, C_CARD_EDGE, 90);
  }
  // A faint top highlight, as on glass.
  StrokeRound (B->X + 2, B->Y + 2, B->W - 4, B->H - 4, CARD_RADIUS - 2, 1, 0xFFFFFF, Focus ? 60 : 25);

  DrawIcon (T->Icon, B->X + 84 - T->Icon->Size / 2, Cy - T->Icon->Size / 2, 0xFFFFFF, 255);
  DrawText (&mFontBig, TextX, Cy - 58, C_TEXT, T->Title);
  DrawText (&mFontSmall, TextX, Cy + 12, Focus || Pressed ? C_SUB_FOCUS : C_SUB, T->Sub);
  DrawIcon (&mIconChevron, B->X + B->W - 58, Cy - 16, 0xFFFFFF, Focus ? 255 : 200);
}

STATIC
VOID
DrawPill (
  INTN             X,
  INTN             Y,
  INTN             W,
  INTN             H,
  CONST MENU_ICON  *Icon,
  CONST CHAR16     *Text,
  BOOLEAN          Pressed
  )
{
  CONST MENU_FONT  *F = &mFontSmall;
  INTN             Content, Cx;

  FillRound (X, Y, W, H, H / 2, Pressed ? 0x2F6FE0 : 0x1A2848, Pressed ? 0x2F6FE0 : 0x1A2848, Pressed ? 240 : 170);
  StrokeRound (X, Y, W, H, H / 2, 2, C_CARD_EDGE, 80);
  Content = Icon->Size + 10 + TextWidth (F, Text);
  Cx      = X + (W - Content) / 2;
  DrawIcon (Icon, Cx, Y + (H - Icon->Size) / 2, 0xFFFFFF, 255);
  DrawText (F, Cx + Icon->Size + 10, Y + (H - F->LineHeight) / 2, C_TEXT, Text);
}

// ---------------------------------------------------------------- picture

VOID
MenuRenderBackground (
  UINT32  *Buffer
  )
{
  mCanvas = Buffer;
  DrawBackground ();
  DrawRibbons ();
}

VOID
MenuRender (
  UINT32           *Canvas,
  CONST UINT32     *Background,
  CONST MENU_VIEW  *View
  )
{
  UINTN             Index;
  CONST MENU_RECT   *R = &gMenuButtons[ActRotate];
  CHAR16            Line[40];
  INTN              N, I, X, Y;

  mCanvas = Canvas;
  if (Background != NULL) {
    for (I = 0; I < LW * LH; I++) {
      Canvas[I] = Background[I];
    }
  } else {
    DrawBackground ();
    DrawRibbons ();
  }

  // Header.
  DrawText (&mFontBig, 68, 30, C_TEXT, L"Switch Boot Manager");
  if (View->Version != NULL) {
    DrawText (&mFontSmall, 70, 92, C_SUB, View->Version);
  }
  DrawPill (R->X, R->Y, R->W, R->H, &mIconRotate, L"Rotate", View->Pressed == ActRotate);

  for (Index = 0; Index < 4; Index++) {
    DrawCard (Index, View);
  }

  if (View->Status != NULL) {
    N = TextWidth (&mFontSmall, View->Status);
    DrawText (&mFontSmall, (LW - N) / 2, 582, C_WARN, View->Status);
  }

  // Footer hints.
  DrawPill (68, 636, 220, 50, &mIconTap, L"Touch to select", FALSE);
  DrawPill (300, 636, 268, 50, &mIconVolume, L"VOL+ / VOL- to move", FALSE);
  DrawPill (580, 636, 236, 50, &mIconPowerSmall, L"POWER to select", FALSE);

  // Touch state at the bottom right.
  if (View->ShowTouch) {
    CONST CHAR16  *Msg = !View->TouchDriver ? L"Touch: no driver" :
                         (View->TouchX < 0 ? L"Touch: starting..." : NULL);
    if (Msg == NULL) {
      // "Touch: X, Y" without PrintLib (shared with the PC preview).
      CONST CHAR16  *Head = L"Touch: ";
      INTN          V[2] = { View->TouchX, View->TouchY }, K, Digits, Div;
      N = 0;
      for (I = 0; Head[I] != 0; I++) {
        Line[N++] = Head[I];
      }
      for (K = 0; K < 2; K++) {
        for (Digits = 1, Div = 1; V[K] / Div >= 10; Digits++, Div *= 10) {
        }
        for (; Div >= 1; Div /= 10) {
          Line[N++] = (CHAR16)(L'0' + (V[K] / Div) % 10);
        }
        if (K == 0) {
          Line[N++] = L',';
          Line[N++] = L' ';
        }
      }
      Line[N] = 0;
      Msg     = Line;
    }
    N = TextWidth (&mFontSmall, Msg);
    DrawText (&mFontSmall, LW - 56 - N, 648, C_SUB, Msg);

    if (View->TouchX >= 0) {
      X = View->TouchX;
      Y = View->TouchY;
      for (I = -18; I <= 18; I++) {
        Blend (X + I, Y, C_MARK, 255);
        Blend (X + I, Y + 1, C_MARK, 255);
        Blend (X, Y + I, C_MARK, 255);
        Blend (X + 1, Y + I, C_MARK, 255);
      }
    }
  }
}

INTN
MenuHitTest (
  INTN  X,
  INTN  Y
  )
{
  UINTN  Index;

  for (Index = 0; Index < ActCount; Index++) {
    if ((X >= gMenuButtons[Index].X) && (X < gMenuButtons[Index].X + gMenuButtons[Index].W) &&
        (Y >= gMenuButtons[Index].Y) && (Y < gMenuButtons[Index].Y + gMenuButtons[Index].H)) {
      return (INTN)Index;
    }
  }
  return -1;
}
