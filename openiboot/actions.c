#include "actions.h"
#include "openiboot-asmhelpers.h"
#include "arm.h"
#include "lcd.h"
#include "mmu.h"
#include "util.h"

#define MACH_APPLE_IPHONE 1506

/* this code is adapted from http://www.simtec.co.uk/products/SWLINUX/files/booting_article.html, which is distributed under the BSD license */

/* list of possible tags */
#define ATAG_NONE       0x00000000
#define ATAG_CORE       0x54410001
#define ATAG_MEM        0x54410002
#define ATAG_VIDEOTEXT  0x54410003
#define ATAG_RAMDISK    0x54410004
#define ATAG_INITRD2    0x54420005
#define ATAG_SERIAL     0x54410006
#define ATAG_REVISION   0x54410007
#define ATAG_VIDEOLFB   0x54410008
#define ATAG_CMDLINE    0x54410009

/* structures for each atag */
struct atag_header {
	uint32_t size; /* length of tag in words including this header */
	uint32_t tag;  /* tag type */
};

struct atag_core {
	uint32_t flags;
	uint32_t pagesize;
	uint32_t rootdev;
};

struct atag_mem {
	uint32_t     size;
	uint32_t     start;
};

struct atag_videotext {
	uint8_t              x;
	uint8_t              y;
	uint16_t             video_page;
	uint8_t              video_mode;
	uint8_t              video_cols;
	uint16_t             video_ega_bx;
	uint8_t              video_lines;
	uint8_t              video_isvga;
	uint16_t             video_points;
};

struct atag_ramdisk {
	uint32_t flags;
	uint32_t size;
	uint32_t start;
};

struct atag_initrd2 {
	uint32_t start;
	uint32_t size;
};

struct atag_serialnr {
	uint32_t low;
	uint32_t high;
};

struct atag_revision {
	uint32_t rev;
};

struct atag_videolfb {
	uint16_t             lfb_width;
	uint16_t             lfb_height;
	uint16_t             lfb_depth;
	uint16_t             lfb_linelength;
	uint32_t             lfb_base;
	uint32_t             lfb_size;
	uint8_t              red_size;
	uint8_t              red_pos;
	uint8_t              green_size;
	uint8_t              green_pos;
	uint8_t              blue_size;
	uint8_t              blue_pos;
	uint8_t              rsvd_size;
	uint8_t              rsvd_pos;
};

struct atag_cmdline {
	char    cmdline[1];
};

struct atag {
	struct atag_header hdr;
	union {
		struct atag_core         core;
		struct atag_mem          mem;
		struct atag_videotext    videotext;
		struct atag_ramdisk      ramdisk;
		struct atag_initrd2      initrd2;
		struct atag_serialnr     serialnr;
		struct atag_revision     revision;
		struct atag_videolfb     videolfb;
		struct atag_cmdline      cmdline;
	} u;
};

typedef void (*LinuxKernel)(int zero, int arch, uint32_t params);

void chainload(uint32_t address) {
	EnterCriticalSection();
	arm_disable_caches();
	mmu_disable();
	CallArm(address);
}

#define tag_next(t)     ((struct atag *)((uint32_t *)(t) + (t)->hdr.size))
#define tag_size(type)  ((sizeof(struct atag_header) + sizeof(struct type)) >> 2)
static struct atag *params; /* used to point at the current tag */

static void setup_core_tag(void * address, long pagesize)
{
	params = (struct atag *)address;         /* Initialise parameters to start at given address */

	params->hdr.tag = ATAG_CORE;            /* start with the core tag */
	params->hdr.size = tag_size(atag_core); /* size the tag */

	params->u.core.flags = 1;               /* ensure read-only */
	params->u.core.pagesize = pagesize;     /* systems pagesize (4k) */
	params->u.core.rootdev = 0;             /* zero root device (typicaly overidden from commandline )*/

	params = tag_next(params);              /* move pointer to next tag */
}

static void setup_mem_tag(uint32_t start, uint32_t len)
{
	params->hdr.tag = ATAG_MEM;             /* Memory tag */
	params->hdr.size = tag_size(atag_mem);  /* size tag */

	params->u.mem.start = start;            /* Start of memory area (physical address) */
	params->u.mem.size = len;               /* Length of area */

	params = tag_next(params);              /* move pointer to next tag */
}

static void setup_cmdline_tag(const char * line)
{
	int linelen = strlen(line);

	if(!linelen)
		return;                             /* do not insert a tag for an empty commandline */

	params->hdr.tag = ATAG_CMDLINE;         /* Commandline tag */
	params->hdr.size = (sizeof(struct atag_header) + linelen + 1 + 4) >> 2;

	strcpy(params->u.cmdline.cmdline,line); /* place commandline into tag */

	params = tag_next(params);              /* move pointer to next tag */
}

static void setup_video_lfb_tag() {
	params->hdr.tag = ATAG_VIDEOLFB;             /* Memory tag */
	params->hdr.size = tag_size(atag_videolfb);  /* size tag */

	params->u.videolfb.lfb_width = currentWindow->framebuffer.width;
	params->u.videolfb.lfb_height = currentWindow->framebuffer.height;
	params->u.videolfb.lfb_depth = 32;
	params->u.videolfb.lfb_linelength = currentWindow->lineBytes;
	params->u.videolfb.lfb_base = (uint32_t) currentWindow->framebuffer.buffer;
	params->u.videolfb.lfb_size = params->u.videolfb.lfb_linelength * params->u.videolfb.lfb_height;
	params->u.videolfb.blue_size = 8;
	params->u.videolfb.blue_pos = 0;
	params->u.videolfb.green_size = 8;
	params->u.videolfb.green_pos = 8;
	params->u.videolfb.red_size = 8;
	params->u.videolfb.red_pos = 16;
	params->u.videolfb.rsvd_size = 8;
	params->u.videolfb.rsvd_pos = 24;

	params = tag_next(params);              /* move pointer to next tag */
}

static void setup_end_tag()
{
	params->hdr.tag = ATAG_NONE;            /* Empty tag ends list */
	params->hdr.size = 0;                   /* zero length */
}

static void setup_tags(struct atag* parameters, const char* commandLine)
{
	setup_core_tag(parameters, 4096);       /* standard core tag 4k pagesize */
	setup_mem_tag(MemoryStart, 0x08000000);    /* 128Mb at 0x00000000 */
	setup_cmdline_tag(commandLine);
	setup_video_lfb_tag();
	setup_end_tag();                    /* end of tags */
}

void boot_linux(uint32_t image) {
	uint32_t exec_at = 0x09000000;
	uint32_t param_at = exec_at + 0x01000000;
	memmove((void*) exec_at, (void*) image, 0x01000000);
	setup_tags((struct atag*) param_at, "");

	uint32_t mach_type = MACH_APPLE_IPHONE;

	LinuxKernel kernel = (LinuxKernel) exec_at;

	arm_disable_caches();
	mmu_disable();
	kernel(0, mach_type, param_at);
}

