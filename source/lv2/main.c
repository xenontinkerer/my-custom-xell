#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <debug.h>
#include <xenos/xenos.h>
#include <console/console.h>
#include <time/time.h>
#include <ppc/timebase.h>
#include <usb/usbmain.h>
#include <sys/iosupport.h>
#include <ppc/register.h>
#include <xenon_nand/xenon_sfcx.h>
#include <xenon_nand/xenon_config.h>
#include <xenon_soc/xenon_secotp.h>
#include <xenon_soc/xenon_power.h>
#include <xenon_soc/xenon_io.h>
#include <xenon_sound/sound.h>
#include <xenon_smc/xenon_smc.h>
#include <xenon_smc/xenon_gpio.h>
#include <xb360/xb360.h>
#include <network/network.h>
#include <httpd/httpd.h>
#include <diskio/ata.h>
#include <elf/elf.h>
#include <version.h>
#include <byteswap.h>

#include "asciiart.h"
#include "config.h"
#include "file.h"
#include "tftp/tftp.h"

#include "log.h"

void do_asciiart() {
    char *p = asciiart;
    while (*p)
	    console_putch(*p++);
    printf(asciitail);
}

void dumpana() {
    int i;
    for (i = 0; i < 0x100; ++i)
    {
	    uint32_t v;
	    xenon_smc_ana_read(i, &v);
	    printf("0x%08x, ", (unsigned int)v);
	    if ((i&0x7)==0x7)
		    printf(" // %02x\n", (unsigned int)(i &~0x7));
    }
}

char FUSES[350]; /* this string stores the ascii dump of the fuses */
char CBLDV[17]; // 16 + terminate
char FGLDV[80];
int cbldvcount;
int fgldvcount;

unsigned char stacks[6][0x10000];

void reset_timebase_task()
{
    mtspr(284,0); // TBLW
    mtspr(285,0); // TBUW
    mtspr(284,0);
}

void synchronize_timebases()
{
    xenon_thread_startup();
    
    std((void*)0x200611a0,0); // stop timebase
    
    int i;
    for(i=1;i<6;++i){
	    xenon_run_thread_task(i,&stacks[i][0xff00],(void *)reset_timebase_task);
	    while(xenon_is_thread_task_running(i));
    }
    
    reset_timebase_task(); // don't forget thread 0
		    
    std((void*)0x200611a0,0x1ff); // restart timebase
}
    
int main(){
    LogInit();
    int i;

    printf("ANA dump before init:\n");
    dumpana();

    // linux needs this
    synchronize_timebases();
    
    // irqs preinit (SMC related)
    *(volatile uint32_t*)0xea00106c = 0x1000000;
    *(volatile uint32_t*)0xea001064 = 0x10;
    *(volatile uint32_t*)0xea00105c = 0xc000000;

    xenon_smc_start_bootanim();

    // flush console after each outputted char
    setbuf(stdout,NULL);

    xenos_init(VIDEO_MODE_AUTO);

    printf("ANA dump after init:\n");
    dumpana();

#ifdef SWIZZY_THEME
    console_set_colors(CONSOLE_COLOR_BLACK,CONSOLE_COLOR_ORANGE); // Orange text on black bg
#elif defined XTUDO_THEME
    console_set_colors(CONSOLE_COLOR_BLACK,CONSOLE_COLOR_PINK); // Pink text on black bg
#elif defined DEFAULT_THEME
    console_set_colors(0x7d235500,CONSOLE_COLOR_WHITE); // White text on blue bg
#else
    console_set_colors(CONSOLE_COLOR_BLACK,CONSOLE_COLOR_GREEN); // Green text on black bg
#endif
    console_init();

    printf("\nXeLL RELOADED - Xenon Linux Loader 2nd Stage " LONGVERSION "\n");
    //printf("\nBuilt with GCC " GCC_VERSION " and Binutils " BINUTILS_VERSION " \n"); // User doesn't need to know this
    do_asciiart();

    //delay(3); //give the user a chance to see our splash screen <- network init should last long enough...
    
    xenon_sound_init();
    xenon_make_it_faster(XENON_SPEED_FULL);

    if (xenon_get_console_type() != REV_CORONA_PHISON) //Not needed for MMC type of consoles! ;)
    {
	    printf(" * NAND init\n");
	    sfcx_init();
	    if (sfc.initialized != SFCX_INITIALIZED)
	    {
		    printf(" ! SFCX initialization failure\n");
		    printf(" ! NAND related features will not be available\n");
		    delay(5);
	    }
    }

    xenon_config_init();

#ifndef NO_NETWORKING

    printf(" * Network init\n");
    network_init();

    printf(" * Starting HTTPd server...");
    httpd_start();
    printf("Success!\n");
#endif

    printf(" * USB init\n");
    usb_init();
    usb_do_poll();

    printf(" * SATA HDD init\n");
    xenon_ata_init();

#ifndef NO_DVD
    printf(" * SATA DVD init\n");
    xenon_atapi_init();
#endif

    mount_all_devices();
    /*int device_list_size = */ findDevices();

    console_clrscr();
    printf(" ______________________________________\n|                                      |\n|  XeLL RELOADED - Xenon Linux Loader  |\n|______________________________________|\n"); // Fancy
    
#ifndef NO_PRINT_CONFIG
    printf("\n * FUSES - Write them down and keep them safe:\n");
    char *fusestr = FUSES;
    char *cbldvstr = CBLDV;
    char *fgldvstr = FGLDV;
    
    for (i = 0; i < 12; ++i){
	    u64 line;
	    unsigned int hi,lo;
	    
	    line = xenon_secotp_read_line(i);
	    hi=line >> 32;
	    lo=line & 0xffffffff;

	    fusestr += sprintf(fusestr, "fuseset %02d: %08x%08x\n", i, hi, lo);

	    if (i >= 7) {
		    fgldvstr += sprintf(fgldvstr, "%08x%08x", hi, lo) + '\0';
	    }
        if (i == 2) {
            cbldvstr += sprintf(cbldvstr, "%08x%08x", hi, lo);
        }

    }

    for (i = 0; CBLDV[i] != '\0' ; ++i) {
        if ('f' == CBLDV[i]) {
            cbldvcount = i + 1;
        }
    }

    
    for (i = 0; FGLDV[i] != '\0'; ++i) {
	    if ('f' == FGLDV[i]) {
		    ++fgldvcount;
	    }
    }
    
    printf(FUSES);
    
    print_cpu_dvd_keys();
    print_serials();
    
    printf(" * CPU PVR: %08x\n", mfspr(287));
    
    if (xenon_get_console_type() == 0) {
	    printf(" * Console: Xenon\n");
    } else if (xenon_get_console_type() == 1) {
	    printf(" * Console: Xenon/Zephyr\n");
    } else if (xenon_get_console_type() == 2) {
	    printf(" * Console: Falcon\n");
    } else if (xenon_get_console_type() == 3) {
	    printf(" * Console: Jasper\n");
    } else if (xenon_get_console_type() == 4) {
	    printf(" * Console: Trinity\n");
    } else if (xenon_get_console_type() == 5) {
	    printf(" * Console: Corona\n");
    } else if (xenon_get_console_type() == 6) {
	    printf(" * Console: Corona MMC\n");
    } else if (xenon_get_console_type() == 7) {
	    printf(" * Console: Winchester\n");
    } else if (xenon_get_console_type() == -1) {
	    printf(" * Console: Unknown\n");
    }
    
    printf(" * 2BL LDV: %d\n", cbldvcount);
    printf(" * 6BL LDV: %d\n", fgldvcount);
    
    network_print_config();
#endif
    /* Stop logging and save it to first USB Device found that is writeable */
    LogDeInit();
    //extern char device_list[STD_MAX][10];

    //for (i = 0; i < device_list_size; i++)
    //{
    //	if (strncmp(device_list[i], "ud", 2) == 0)
    //	{
    //		char tmp[STD_MAX + 8];
    //		sprintf(tmp, "%sxell.log", device_list[i]);
    //		if (LogWriteFile(tmp) == 0)
    //			i = device_list_size;
    //	}
    //}
    
    mount_all_devices();
    printf("\n * Looking for files on local media and TFTP...\n\n");
    for(;;){
	    fileloop();
	    tftp_loop(); //less likely to find something...
	    console_clrline();
	    usb_do_poll(); // Refresh USB
    }

    return 0;
}

