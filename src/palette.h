typedef struct {
	uint8 r,g,b;
} pal;

extern pal *palo;
/* Optional 512-entry direct RGB palette used by VT369 enhanced video.
 * Entries 0-255 are BG, 256-511 are OBJ.  The high-color blitter uses
 * this instead of the NES deemphasis table while enabled. */
extern pal FCEU_DirectPalette512[512];
extern bool FCEU_DirectPalette512Enabled;
void FCEU_ResetPalette(void);

void FCEU_ResetPalette(void);
void FCEU_ResetMessages();
void FCEU_LoadGamePalette(void);
void FCEU_DrawNTSCControlBars(uint8 *XBuf);
