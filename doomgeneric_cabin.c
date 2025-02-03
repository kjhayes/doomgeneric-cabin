
#include "doomgeneric.h"
#include "doomkeys.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include "kanawha/uapi/kbd.h"
#include "kanawha/uapi/time.h"
#include "kanawha/sys-wrappers.h"

#define FB_CHARSIZE 2
#define FB_WIDTH 80
#define FB_HEIGHT 25

#define DOOM_X_PER_FB (DOOMGENERIC_RESX / FB_WIDTH)
#define DOOM_Y_PER_FB (DOOMGENERIC_RESY / FB_HEIGHT)

static fd_t vga_fb_file;
static void *vga_fb;

#define PIX_PER_TIX_X 2
#define PIX_PER_TIX_Y 2

struct tix {
    uint8_t red[PIX_PER_TIX_X * PIX_PER_TIX_Y];
    uint8_t green[PIX_PER_TIX_X * PIX_PER_TIX_Y];
    uint8_t blue[PIX_PER_TIX_X * PIX_PER_TIX_Y];
};

static uint32_t BRIGHTNESS_BOOST = 0x0;

static fd_t kbd_file;

void DG_Init(void) {
    // TODO
    //printf("DG_Init\n");
}

static inline uint16_t
vga_encode(uint8_t c, uint8_t fgcolor, uint8_t bgcolor) {
    return (((uint16_t)((fgcolor & 0xF)|(bgcolor<<4))) << 8) | c;
}

static inline void
sample_tix(struct tix *tix, size_t tl_x, size_t tl_y) {

    const static size_t sub_x_stride = DOOM_X_PER_FB / PIX_PER_TIX_X;
    const static size_t sub_y_stride = DOOM_Y_PER_FB / PIX_PER_TIX_Y;

    for(size_t sub_y = 0; sub_y < PIX_PER_TIX_Y; sub_y++) {
      for(size_t sub_x = 0; sub_x < PIX_PER_TIX_X; sub_x++) {

          size_t pix_x = tl_x + (sub_x*sub_x_stride);
          size_t pix_y = tl_y + (sub_y*sub_y_stride);
        
          uint8_t red;
          uint8_t green;
          uint8_t blue;

#define ANTIALIAS
#ifdef ANTIALIAS
          uint32_t blue_sum = 0;
          uint32_t green_sum = 0;
          uint32_t red_sum = 0;
          for(size_t sub_sub_y = 0; sub_sub_y < sub_y_stride; sub_sub_y++) {
            for(size_t sub_sub_x = 0; sub_sub_x < sub_x_stride; sub_sub_x++) {
                uint8_t *pix = (uint8_t*)&DG_ScreenBuffer[pix_x + sub_sub_x + (DOOMGENERIC_RESX * (pix_y + sub_sub_y))];
                blue_sum += pix[0];
                green_sum += pix[1];
                red_sum += pix[2];
            }
          }
          blue = blue_sum / (sub_x_stride * sub_y_stride);
          green = green_sum / (sub_x_stride * sub_y_stride);
          red = red_sum / (sub_x_stride * sub_y_stride);
#else
          uint8_t *pix = (uint8_t*)&DG_ScreenBuffer[pix_x + (DOOMGENERIC_RESX * pix_y)];
          blue = pix[0];
          green = pix[1];
          red = pix[2];
#endif
          tix->blue[sub_x + (sub_y * PIX_PER_TIX_X)] = blue;
          tix->green[sub_x + (sub_y * PIX_PER_TIX_X)] = green;
          tix->red[sub_x + (sub_y * PIX_PER_TIX_X)] = red;
      }
    }
}

#define NUM_BRIGHTNESSES 4
#define NUM_ORIENTATIONS FINAL_ORIENTATION

enum orientation {
    ORIENTATION_EQUAL = 0,
    ORIENTATION_BOTTOM,
    ORIENTATION_TOP,
    ORIENTATION_LEFT,
    ORIENTATION_RIGHT,

    ORIENTATION_TOP_LEFT,
    ORIENTATION_NON_TOP_LEFT,
    ORIENTATION_TOP_RIGHT,
    ORIENTATION_NON_TOP_RIGHT,

    FINAL_ORIENTATION,
};

// Type-able
static const char char_select[NUM_ORIENTATIONS][NUM_BRIGHTNESSES] = {
    [ORIENTATION_EQUAL] =         {'+', '#', '@', '$'},
    [ORIENTATION_TOP] =           {'`', '\'', '"', '^'},
    [ORIENTATION_BOTTOM] =        {'_', '.', 'n', 'a'},
    [ORIENTATION_LEFT] =          {'l', '>', '{', '['},
    [ORIENTATION_RIGHT] =         {'l', '<', '}', ']'},
    [ORIENTATION_TOP_LEFT] =      {'`', '\'', '"', '^'},
    [ORIENTATION_NON_TOP_LEFT] =  {'_', '.', ',', 'j'},
    [ORIENTATION_TOP_RIGHT] =     {'`', '\'', '"', '^'},
    [ORIENTATION_NON_TOP_RIGHT] = {'_', '.', ',', 'L'},
};

// Blocky
//static const char char_select[NUM_ORIENTATIONS][NUM_BRIGHTNESSES] = {
//    [ORIENTATION_EQUAL] =         {0xB0, 0xB1, 0xB2, 0xDB},
//    [ORIENTATION_TOP] =           {0x5E, 0x7E, 0xDF, 0xDF},
//    [ORIENTATION_BOTTOM] =        {0x5F, 0x16, 0xDC, 0xDC},
//    [ORIENTATION_LEFT] =          {0xB0, 0xB0, 0xB1, 0xDD},
//    [ORIENTATION_RIGHT] =         {0xB0, 0xB0, 0xB1, 0xDE},
//    [ORIENTATION_TOP_LEFT] =      {0xB0, 0xB0, 0xD9, 0xDF},
//    [ORIENTATION_NON_TOP_LEFT] =  {0xB0, 0xB1, 0xB2, 0xDC},
//    [ORIENTATION_TOP_RIGHT] =     {0xB0, 0xB0, 0xC0, 0xDF},
//    [ORIENTATION_NON_TOP_RIGHT] = {0xB0, 0xB1, 0xB2, 0xDC},
//};


static inline uint16_t
generate_vga_from_tix(struct tix *tix) {

    char c;
    char fg_color;
    char bg_color;

    uint8_t base_color;

    uint8_t avg_r;
    uint8_t avg_g;
    uint8_t avg_b;

    uint8_t pix_bright[PIX_PER_TIX_X * PIX_PER_TIX_Y];

    {
        uint32_t sum_r;
        uint32_t sum_g;
        uint32_t sum_b;
        for(size_t index = 0; index < PIX_PER_TIX_X*PIX_PER_TIX_Y; index++) {
           sum_r += tix->red[index];
           sum_g += tix->green[index];
           sum_b += tix->blue[index];
           pix_bright[index] = (((uint32_t)tix->red[index]
                   + (uint32_t)tix->green[index]
                   + (uint32_t)tix->blue[index])
               /3);
        }
        avg_r = sum_r / (PIX_PER_TIX_X * PIX_PER_TIX_Y);
        avg_g = sum_g / (PIX_PER_TIX_X * PIX_PER_TIX_Y);
        avg_b = sum_b / (PIX_PER_TIX_X * PIX_PER_TIX_Y);
    }

    uint8_t avg_bright = ((uint32_t)avg_r + (uint32_t)avg_g + (uint32_t)avg_b)/3;

    float rel_bright[PIX_PER_TIX_X * PIX_PER_TIX_Y];
    for(size_t i = 0; i < PIX_PER_TIX_X*PIX_PER_TIX_Y; i++) {
        rel_bright[i] = (float)pix_bright[i] / (float)avg_bright;
    }

    // After computing relative brightnesses,
    // boost the overall brightness by a fixed amount
    if((0xFFUL - (uint32_t)avg_bright) >= BRIGHTNESS_BOOST) {
        avg_bright += BRIGHTNESS_BOOST;
    } else {
        avg_bright = 0xFF;
    }

    // 4 Brightness Levels
    // 0 -> darkest 3 -> brightest

    // 1 Orientation
    enum orientation orientation = ORIENTATION_EQUAL;

    // This section assume 2x2 pix per tix

#define ORIENTATION_THRESHOLD 0.5f

    {
    float top_avg = (rel_bright[0] + rel_bright[1]) * 0.5f;
    float bot_avg = (rel_bright[2] + rel_bright[3]) * 0.5f;
    
    float top_bot_diff = top_avg - bot_avg;
    if(fabs(top_bot_diff) > ORIENTATION_THRESHOLD) {
        if(top_avg > bot_avg) {
            orientation = ORIENTATION_TOP;
        } else {
            orientation = ORIENTATION_BOTTOM;
        }
        goto after_orientation;
    }
    }

    {
    float left_avg = (rel_bright[0] + rel_bright[2]) * 0.5f;
    float right_avg = (rel_bright[1] + rel_bright[3]) * 0.5f;

    float left_right_diff = left_avg - right_avg;
    if(fabs(left_right_diff) > ORIENTATION_THRESHOLD) {
        if(left_avg > right_avg) {
            orientation = ORIENTATION_LEFT;
        } else {
            orientation = ORIENTATION_RIGHT;
        }
        goto after_orientation;
    }
    }

    {
    float top_left_avg = (rel_bright[0]);
    float non_top_left_avg = (rel_bright[1] + rel_bright[2] + rel_bright[3]) * (1.0f/3.0f);
    float top_left_diff = top_left_avg - non_top_left_avg;
    if(fabs(top_left_diff) > ORIENTATION_THRESHOLD) {
        if(top_left_avg > non_top_left_avg) {
            orientation = ORIENTATION_TOP_LEFT;
        } else {
            orientation = ORIENTATION_NON_TOP_LEFT;
        }
        goto after_orientation;
    }
    }

    {
    float top_right_avg = (rel_bright[1]);
    float non_top_right_avg = (rel_bright[0] + rel_bright[2] + rel_bright[3]) * (1.0f/3.0f);
    float top_right_diff = top_right_avg - non_top_right_avg;
    if(fabs(top_right_diff) > ORIENTATION_THRESHOLD) {
        if(top_right_avg > non_top_right_avg) {
            orientation = ORIENTATION_TOP_RIGHT;
        } else {
            orientation = ORIENTATION_NON_TOP_RIGHT;
        }
        goto after_orientation;
    }
    }

after_orientation:
    // end assumption section

#define COLOR_THRESHOLD 0x40
    base_color = (avg_b > COLOR_THRESHOLD) | ((avg_g > COLOR_THRESHOLD)<<1) | ((avg_r > COLOR_THRESHOLD)<<2);
#undef COLOR_THRESHOLD

    if(avg_bright > 0x80) {
        fg_color = base_color | (1<<3); // Bright
        bg_color = base_color;
    } else {
        fg_color = base_color;
        bg_color = 0;
    }

    avg_bright &= ~0x80;
    if(avg_bright > 0x60) {
        c = char_select[orientation][3];
    } else if(avg_bright > 0x40) {
        c = char_select[orientation][2];
    } else if(avg_bright > 0x20) {
        c = char_select[orientation][1];
    } else {
        c = char_select[orientation][0];
    }

    return vga_encode(c, fg_color, bg_color);
}

void DG_DrawFrame(void) {
    // TODO
//    printf("DG_DrawFrame\n");

    for(size_t tix_y = 0; tix_y < FB_HEIGHT; tix_y++) {
        for(size_t tix_x = 0; tix_x < FB_WIDTH; tix_x++)
        {
            struct tix tix;
            sample_tix(&tix,
                    DOOM_X_PER_FB * tix_x,
                    DOOM_Y_PER_FB * tix_y);
            uint16_t value = generate_vga_from_tix(&tix);
            ((uint16_t*)vga_fb)[tix_x + (tix_y*FB_WIDTH)] = value;
        }
    }
 
    kanawha_sys_flush(vga_fb_file, 0);
}

// HACK
static uint32_t ticks = 0;

void DG_SleepMs(uint32_t ms)
{
    //printf("DG_SleepMs\n");
    kanawha_sys_sleep(ms, SLEEP_DURATION_MSEC);
}

uint32_t DG_GetTicksMs()
{
    // TODO
    //printf("DG_GetTicksMs\n");
    // HACK
    return kanawha_sys_time(TIME_PROC|TIME_DURATION_MSEC);
}

static inline unsigned char
doomKeyFromKbdKey(kbd_key_t key) {
    switch(key) {
        case KBD_KEY_0: return '0';
        case KBD_KEY_1: return '1';
        case KBD_KEY_2: return '2';
        case KBD_KEY_3: return '3';
        case KBD_KEY_4: return '4';
        case KBD_KEY_5: return '5';
        case KBD_KEY_6: return '6';
        case KBD_KEY_7: return '7';
        case KBD_KEY_8: return '8';
        case KBD_KEY_9: return '9';
        case KBD_KEY_A: return KEY_LEFTARROW;
        case KBD_KEY_B: return 'B';
        case KBD_KEY_C: return 'C';
        case KBD_KEY_D: return KEY_RIGHTARROW;
        case KBD_KEY_E: return KEY_USE;
        case KBD_KEY_F: return 'F';
        case KBD_KEY_G: return 'G';
        case KBD_KEY_H: return 'H';
        case KBD_KEY_I: return 'I';
        case KBD_KEY_J: return 'J';
        case KBD_KEY_K: return 'K';
        case KBD_KEY_L: return 'L';
        case KBD_KEY_M: return 'M';
        case KBD_KEY_N: return 'N';
        case KBD_KEY_O: return 'O';
        case KBD_KEY_P: return 'P';
        case KBD_KEY_Q: return 'Q';
        case KBD_KEY_R: return 'R';
        case KBD_KEY_S: return KEY_DOWNARROW;
        case KBD_KEY_T: return 'T';
        case KBD_KEY_U: return 'U';
        case KBD_KEY_V: return 'V';
        case KBD_KEY_W: return KEY_UPARROW;
        case KBD_KEY_X: return 'X';
        case KBD_KEY_Y: return 'Y';
        case KBD_KEY_Z: return 'Z';
        case KBD_KEY_F1: return KEY_F1;
        case KBD_KEY_F2: return KEY_F2;
        case KBD_KEY_F3: return KEY_F3;
        case KBD_KEY_F4: return KEY_F4;
        case KBD_KEY_F5: return KEY_F5;
        case KBD_KEY_F6: return KEY_F6;
        case KBD_KEY_F7: return KEY_F7;
        case KBD_KEY_F8: return KEY_F8;
        case KBD_KEY_F9: return KEY_F9;
        case KBD_KEY_F10: return KEY_F10;
        case KBD_KEY_F11: return KEY_F11;
        case KBD_KEY_F12: return KEY_F12;
        case KBD_KEY_BACKSPACE: return KEY_BACKSPACE;
        case KBD_KEY_SPACE: return KEY_FIRE;
        case KBD_KEY_ENTER: return KEY_ENTER;
        case KBD_KEY_LEFT_ARROW: return KEY_LEFTARROW;
        case KBD_KEY_RIGHT_ARROW: return KEY_RIGHTARROW;
        case KBD_KEY_DOWN_ARROW: return KEY_DOWNARROW;
        case KBD_KEY_UP_ARROW: return KEY_UPARROW;
        default:
            return 0;
    }
}

int DG_GetKey(int* pressed, unsigned char* doomKey)
{
    // TODO
    //printf("DG_GetKey\n");

    struct kbd_event event;
    size_t read = kanawha_sys_read(kbd_file, &event, sizeof(struct kbd_event));
    if(read == 0) {
        return 0;
    }
    if(read != sizeof(struct kbd_event)) {
        fprintf(stderr, "Failed to read whole keyboard event!\n");
        return 0;
    }

    *pressed = (event.motion == KBD_MOTION_PRESSED) || (event.motion == KBD_MOTION_HELD);
    *doomKey = doomKeyFromKbdKey(event.key);

    if(*pressed) {
        switch(event.key) {
          case KBD_KEY_0:
            if(BRIGHTNESS_BOOST < 0xFF) {
                BRIGHTNESS_BOOST++;
            }
            break;
          case KBD_KEY_9:
            if(BRIGHTNESS_BOOST > 0) {
                BRIGHTNESS_BOOST--;
            }
            break;
          default:
            break;
        }
    }

    if(*doomKey == 0) {
        return 0;
    }

    return 1;
}

void DG_SetWindowTitle(const char * title)
{
    // TODO
    printf("DG_SetWindowTitle(%s)\n", title);
}

int main(int argc, char **argv)
{
    if(argc != 3) {
        fprintf(stderr, "Usage: doomgeneric [VGA-FB-PATH] [KBD-PATH]\n");
        exit(-1);
    }

    // Set up the framebuffer
    const char *vga_fb_path = argv[1];
    const char *kbd_path = argv[2];

    int res;
    res = kanawha_sys_open(
            vga_fb_path,
            FILE_PERM_READ|FILE_PERM_WRITE,
            0,
            &vga_fb_file);
    if(res) {
        fprintf(stderr, "Failed to open \"%s\"\n", vga_fb_path);
        exit(-1);
    }

    res = kanawha_sys_mmap(
            vga_fb_file,
            0,
            &vga_fb,
            ((FB_CHARSIZE * FB_WIDTH * FB_HEIGHT) + 0xFFF) & ~0xFFF,
            MMAP_SHARED|MMAP_PROT_READ|MMAP_PROT_WRITE);
    if(res) {
        fprintf(stderr, "Failed to mmap file: %s\n",
                vga_fb_path);
        exit(-1);
    }

    memset(vga_fb, 0, FB_CHARSIZE * FB_WIDTH * FB_HEIGHT);

    res = kanawha_sys_open(
            kbd_path,
            FILE_PERM_READ,
            FILE_MODE_NON_BLOCK,
            &kbd_file);
    if(res) {
        fprintf(stderr, "Failed to open \"%s\"\n", kbd_path);
        exit(-1);
    }

    doomgeneric_Create(argc, argv);

    while (1)
    {
        doomgeneric_Tick();
    }
    
    return 0;
}

