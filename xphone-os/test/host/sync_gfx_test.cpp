#include "../../src/Gfx.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>
int main() {
 EInkDisplay d; Gfx gfx(d); assert(gfx.begin()); d.clearScreen();
#ifdef CHARACTERIZE_OLD_SYNC
 d.fb=nullptr; // The SDK releases ownership but Gfx retains writable storage.
 gfx.fillRect(0,0,480,800,true);
 assert(d.bytes[0]==0); std::cout<<"BASELINE: Gfx writes through its stale framebuffer cache\n";
#else
 assert(gfx.releaseFramebufferForSync());
 gfx.drawPixel(0,0,true); gfx.fillRect(0,0,480,800,true);
 const uint8_t bitmap[]={255};
 const EpdGlyph glyphs[]={{2,2,32,0,2,1,0}};
 const EpdUnicodeInterval intervals[]={{65,65,0}};
 const XpFont f{bitmap,glyphs,intervals,1,24,20};
 gfx.drawText(f,0,0,"A");
 gfx.setOrientation(Gfx::Orient::Landscape); gfx.drawText(f,0,0,"A");
 gfx.fillRect(0,0,800,480,true); gfx.drawPixel(0,0,true); gfx.invert();
 gfx.setOrientation(Gfx::Orient::Portrait);
 gfx.clear(); gfx.flush(EInkDisplay::FAST_REFRESH); gfx.flushWindow(0,0,10,10);
 for(auto b:d.bytes) assert(b==255);
 assert(gfx.restoreFramebufferAfterSync()); gfx.drawPixel(0,0,true);
 assert(d.bytes[47900]==127);
 std::cout<<"PASS: disabled drawing preserves released bytes; restored drawing works\n";
#endif

 // Regression: NFD text ("abra\u0301zame") must be composed to NFC before the
 // UI text funnel measures/draws it, so the combining mark U+0301 resolves to
 // the precomposed U+00E1 glyph instead of the '?' fallback. The UI fonts
 // cover 0x20-0x7E + 0xA0-0x17F but have no combining-mark block (U+0300-036F).
 {
   const uint8_t bitmap[] = {0x80};  // 1 black pixel (bit 7), one per width-1 glyph
   std::vector<EpdGlyph> glyphs(319, EpdGlyph{1, 1, 16, 0, 0, 1, 0});
   glyphs[0x3F - 0x20].advanceX = 64;         // '?'  -> 4px, so any fallback is detectable
   glyphs[95 + (0xE1 - 0xA0)].advanceX = 32;  // U+00E1 'á' -> 2px
   const EpdUnicodeInterval intervals[] = {{0x20, 0x7E, 0}, {0xA0, 0x17F, 95}};
   const XpFont ui{bitmap, glyphs.data(), intervals, 2, 24, 8};

   const char* nfd = "abra\u0301zame";
   const char* nfc = "abrázame";

   // Without composition U+0301 has no glyph, so canRender must report false.
   assert(gfx.canRender(ui, nfd));

   // textWidth must measure the NFC form (one glyph fewer, no '?' advance).
   assert(gfx.textWidth(ui, nfd) == gfx.textWidth(ui, nfc));

   // drawText must paint identical pixels for NFD and NFC: no '?' fallback.
   auto blackPixels = [&]() {
     int n = 0;
     for (size_t i = 0; i < sizeof(d.bytes); i++)
       n += 8 - __builtin_popcount(d.bytes[i]);
     return n;
   };
   d.clearScreen();
   gfx.drawText(ui, 0, 0, nfd);
   const int nfdBlack = blackPixels();
   d.clearScreen();
   gfx.drawText(ui, 0, 0, nfc);
   const int nfcBlack = blackPixels();
   assert(nfdBlack == nfcBlack);
   assert(nfdBlack == 8);  // 8 glyphs at 1px each, none of them '?'

   // drawTextScaled at scale>1 walks codepoints itself (the scale==1 branch
   // delegates to drawText, already covered). Its measure (textWidthScaled) and
   // its draw must both match the NFC form.
   assert(gfx.textWidthScaled(ui, nfd, 2) == gfx.textWidthScaled(ui, nfc, 2));
   d.clearScreen();
   gfx.drawTextScaled(ui, 0, 0, nfd, 2);
   const int nfdScaledBlack = blackPixels();
   d.clearScreen();
   gfx.drawTextScaled(ui, 0, 0, nfc, 2);
   const int nfcScaledBlack = blackPixels();
   assert(nfdScaledBlack == nfcScaledBlack);
   assert(nfdScaledBlack == 32);  // 8 glyphs at 2x2 each, none of them '?'

   // Long NFD strings must not be truncated: composition never lengthens the
   // source, so a 256+ B composed result must keep every glyph instead of being
   // clipped to a fixed stack buffer (which would drop a lead byte and paint a
   // '?').
   std::string longNfd, longNfc;
   for (int i = 0; i < 200; i++) { longNfd += "a\u0301"; longNfc += "á"; }
   assert(longNfd.size() > 255);
   assert(gfx.canRender(ui, longNfd.c_str()));
   assert(gfx.textWidth(ui, longNfd.c_str()) == gfx.textWidth(ui, longNfc.c_str()));

   std::cout << "PASS: NFD input composes to NFC (no '?' fallback), black pixels="
             << nfdBlack << "; scaled=" << nfdScaledBlack
             << "; long string not truncated\n";
 }
}
