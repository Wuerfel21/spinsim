/*******************************************************************************
' Author: Dave Hein
' Version 0.75
' Copyright (c) 2010 - 2014
' See end of file for terms of use.
'******************************************************************************/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#ifdef LINUX
#include <dirent.h>
#include <sys/stat.h>
#include "conion.h"
#else
#include <conio.h>
#include <direct.h>
#endif
#include "interp.h"
#include "rom.h"
#include "spindebug.h"
#include "eeprom.h"

// Define system I/O addresses and commands
//#define SYS_COMMAND    0x12340000
//#define SYS_LOCKNUM    0x12340002
//#define SYS_PARM       0x12340004
//#define SYS_DEBUG      0x12340008

#define SYS_CON_PUTCH     1
#define SYS_CON_GETCH     2
#define SYS_FILE_OPEN     3
#define SYS_FILE_CLOSE    4
#define SYS_FILE_READ     5
#define SYS_FILE_WRITE    6
#define SYS_FILE_OPENDIR  7
#define SYS_FILE_CLOSEDIR 8
#define SYS_FILE_READDIR  9
#define SYS_FILE_SEEK     10
#define SYS_FILE_TELL     11
#define SYS_FILE_REMOVE   12
#define SYS_FILE_CHDIR    13
#define SYS_FILE_GETCWD   14
#define SYS_FILE_MKDIR    15
#define SYS_FILE_GETMOD   16
#define SYS_EXTMEM_READ   17
#define SYS_EXTMEM_WRITE  18
#define SYS_EXTMEM_ALLOC  19

#define GCC_REG_BASE 0

static char rootdir[100];
char *hubram;
char *extram[4];
int32_t extmemsize[4];
uint32_t extmembase[4];
int32_t extmemnum = 0;
char lockstate[8];
char lockalloc[8];

char objname[100][20];
int32_t methodnum[100];
int32_t methodlev = 0;

int32_t printflag = 0;
int32_t symflag = 0;
int32_t pasmspin = 0;
int32_t profile = 0;
int32_t memsize = 64;
int32_t cycleaccurate = 0;
int32_t loopcount = 0;
int32_t proptwo = 0;
int32_t baudrate = 0;
int32_t pin_val = -1;
int32_t gdbmode = 0;
int32_t eeprom = 0;
int32_t debugmode = 0;
int32_t printbreak = 0;

FILE *logfile = NULL;
FILE *tracefile = NULL;
FILE *cmdfile = NULL;

PasmVarsT PasmVars[8];

void PrintOp(SpinVarsT *spinvars);
void ExecuteOp(SpinVarsT *spinvars);
char *FindChar(char *str, int32_t val);
void Debug(void);
int32_t RunProp(int32_t maxloops);
void gdb(void);

void spinsim_exit(int32_t exitcode)
{
// dbetz: not defined for Windows and a nop for anything else
//    restore_console_io();
    exit(exitcode);
}

void usage(void)
{
    fprintf(stderr, "Spinsim Version 0.75\n");
    fprintf(stderr, "usage: spinsim [options] file\n");
    fprintf(stderr, "The options are as follows:\n");
    fprintf(stderr, "     -v# Set verbosity level\n");
    //fprintf(stderr, "     -l  List executed instructions\n");
    fprintf(stderr, "     -l <filename>  List executed instructions to <filename>\n");
    fprintf(stderr, "     -p  Use PASM Spin interpreter\n");
    fprintf(stderr, "     -#  Execute # instructions\n");
    fprintf(stderr, "     -P  Profile Spin opcode usage\n");
    fprintf(stderr, "     -m# Set the hub memory size to # K-bytes\n");
    //fprintf(stderr, "     -c  Enable cycle-accurate mode for pasm cogs\n");
    fprintf(stderr, "     -t  Enable the Prop 2 mode\n");
    fprintf(stderr, "     -b# Enable the serial port and set the baudrate to # (default 115200)\n");
    fprintf(stderr, "     -gdb Operate as a GDB target over stdin/stdout\n");
    fprintf(stderr, "     -L <filename> Log GDB remote comm to <filename>\n");
    fprintf(stderr, "     -r <filename> Replay GDB session from <filename>\n");
    //fprintf(stderr, "     -x# Set the external memory size to # K-bytes\n");
    fprintf(stderr, "     -e Use eeprom.dat\n");
    fprintf(stderr, "     -d Use debugger\n");
    spinsim_exit(1);
}

void putchx(int32_t val)
{
    putchar(val);
    fflush(stdout);
}

int32_t getchx(void)
{
    uint8_t val = 0;
    // GCC compiler issues warning for ignored fread return value
    if(fread(&val, 1, 1, cmdfile /*stdin*/))
    	if (val == 10) val = 13;
    return val;
}

char *FindExtMem(uint32_t addr, int32_t num)
{
    int i;
    char *ptr = 0;
    uint32_t addr1 = addr + num - 1;
    uint32_t curraddr, curraddr1;

    //fprintf(stderr, "FindExtMem(%d, %d)\n", addr, num);

    for (i = 0; i < extmemnum; i++)
    {
	//fprintf(stderr, "i = %d\n", i);
        curraddr = extmembase[i];
        curraddr1 = curraddr + extmemsize[i] - 1;
        if (curraddr <= addr && addr <= curraddr1)
	{
	    //fprintf(stderr, "1: %d %d %d\n", addr, curraddr, curraddr1);
	    //fprintf(stderr, "2: %d %d %d\n", addr1, curraddr, curraddr1);
            if (curraddr <= addr1 && addr1 <= curraddr1)
	        ptr = extram[i] + addr - extmembase[i];
	    break;
	}
    }

    return ptr;
}

// This routine prevents us from calling kbhit too often.  This improves
// the performance of the simulator when calling kbhit in a tight loop.
int kbhit1(void)
{
    static int last = 0;

    if (loopcount - last < 6000) return 0;
    last = loopcount;
    return kbhit();
}

void CheckCommand(void)
{
    int32_t parm;
    int32_t command = WORD(SYS_COMMAND);
    FILE *stream;
    DIR *pdir;
    struct dirent *pdirent;

    if (!command) return;

    parm = LONG(SYS_PARM);

    if (command == SYS_CON_PUTCH)
    {
	if (parm == 13)
	    putchx(10);
	else
	    putchx(parm);
    }
    else if (command == SYS_CON_GETCH)
    {
	if (kbhit1())
	    parm = getch();
	else
	    parm = -1;
	LONG(SYS_PARM) = parm;
    }
    else if (command == SYS_FILE_OPEN)
    {
	char *fname;
	char *fmode;

	fname = (char *)&BYTE(LONG(parm));
	fmode = (char *)&BYTE(LONG(parm+4));
	stream = fopen(fname, fmode);
	LONG(SYS_PARM) = (long)stream;
    }
    else if (command == SYS_FILE_CLOSE)
    {
	if (parm) fclose((FILE *)(long)parm);
	LONG(SYS_PARM) = 0;
    }
    else if (command == SYS_FILE_READ)
    {
	int32_t num;
	char *buffer;

         if (!parm) LONG(SYS_PARM) = -1;
	 else
	 {
	    stream = (FILE *)(long)LONG(parm);
	    buffer = (char *)&BYTE(LONG(parm+4));
	    num = LONG(parm+8);
	    LONG(SYS_PARM) = fread(buffer, 1, num, stream);
	}
    }
    else if (command == SYS_FILE_WRITE)
    {
	int32_t num;
	char *buffer;

	if (!parm) LONG(SYS_PARM) = -1;
	else
	{
	    stream = (FILE *)(long)LONG(parm);
	    buffer = (char *)&BYTE(LONG(parm+4));
	    num = LONG(parm+8);
	    LONG(SYS_PARM) = fwrite(buffer, 1, num, stream);
	}
    }
    else if (command == SYS_FILE_OPENDIR)
    {
	char *dname = ".";

	pdir = opendir(dname);
	LONG(SYS_PARM) = (long)pdir;
    }
    else if (command == SYS_FILE_CLOSEDIR)
    {
	if (parm) closedir((DIR *)(long)parm);
	LONG(SYS_PARM) = 0;
    }
    else if (command == SYS_FILE_READDIR)
    {
	int32_t *buffer;
        pdir = (DIR *)(long)LONG(parm);
        buffer = (int32_t *)&BYTE(LONG(parm+4));
	if (!pdir) LONG(SYS_PARM) = 0;
	else
	{
	    pdirent = readdir(pdir);
	    if (pdirent)
	    {
#ifdef LINUX
		FILE *infile;
		int32_t d_size = 0;
		int32_t d_attr = 0;
#if 0
		int32_t d_type = pdirent->d_type;

		if (d_type & DT_DIR) d_attr |= 0x10;
		if (d_type & S_IXUSR) d_attr |= 0x20;
		if (!(d_type & S_IWUSR)) d_attr |= 0x01;
#endif

                if ((infile = fopen(pdirent->d_name, "r")))
		{
		    fseek(infile, 0, SEEK_END);
		    d_size = ftell(infile);
		    fclose(infile);
		}

 	        buffer[0] = d_size;
 	        buffer[1] = d_attr;
#else
	        buffer[0] = pdirent->d_size;
	        buffer[1] = pdirent->d_attr;
#endif
	        strcpy((char *)&buffer[2], pdirent->d_name);
	    }
	    LONG(SYS_PARM) = (long)pdirent;
	}
    }
    else if (command == SYS_FILE_SEEK)
    {
	int32_t offset, whence;
        stream = (FILE *)(long)LONG(parm);
        offset = LONG(parm+4);
        whence = LONG(parm+8);
	LONG(SYS_PARM) = fseek(stream, offset, whence);
    }
    else if (command == SYS_FILE_TELL)
    {
        stream = (FILE *)(long)parm;
	LONG(SYS_PARM) = ftell(stream);
    }
    else if (command == SYS_FILE_REMOVE)
    {
	char *fname = (char *)&BYTE(parm);
	LONG(SYS_PARM) = remove(fname);
    }
    else if (command == SYS_FILE_CHDIR)
    {
	char *path = (char *)&BYTE(parm);
	char fullpath[200];
	char *ptr;
	if (path[0] == '/')
	{
	    strcpy(fullpath, rootdir);
	    strcat(fullpath, path);
	}
	else
	    strcpy(fullpath, path);

	ptr = fullpath;
#ifndef LINUX
	while (*ptr)
	{
	    if (*ptr == '/') *ptr = 0x5c;
	    ptr++;
	}
#endif
	parm = chdir(fullpath);
	LONG(SYS_PARM) = parm;
    }
    else if (command == SYS_FILE_GETCWD)
    {
        char *ptr;
	char *str = (char *)&BYTE(LONG(parm));
	int32_t num = LONG(parm+4);
	ptr = getcwd(str, num);
	LONG(SYS_PARM) = LONG(parm);
    }
    else if (command == SYS_FILE_MKDIR)
    {
	//char *fname = (char *)&BYTE(parm);
#ifdef LINUX
#if 0
	LONG(SYS_PARM) = mkdir(fname, S_IRWXU | S_IRWXG | S_IRWXO);
#endif
#else
	LONG(SYS_PARM) = mkdir(fname);
#endif
    }
    else if (command == SYS_FILE_GETMOD)
    {
	char *fname = (char *)&BYTE(parm);
	int32_t attrib = -1;

	pdir = opendir(".");
	while (pdir)
	{
	    pdirent = readdir(pdir);
	    if (!pdirent) break;
	    if (strcmp(pdirent->d_name, fname) == 0)
	    {
#ifdef LINUX
#if 0
		int32_t d_type = pdirent->d_type;
		attrib = 0;
		if (d_type & DT_DIR) attrib |= 0x10;
		if (d_type & S_IXUSR) attrib |= 0x20;
		if (!(d_type & S_IWUSR)) attrib |= 0x01;
#else
		attrib = 0;
#endif
#else
		attrib = pdirent->d_attr;
#endif
		break;
	    }
	}
	if (pdir) closedir(pdir);
	LONG(SYS_PARM) = attrib;
    }
    else if (command == SYS_EXTMEM_READ)
    {
	uint32_t extaddr = (uint32_t)LONG(parm);
	char *hubaddr = (char *)&BYTE(LONG(parm+4));
	int32_t num = LONG(parm+8);
	char *extmemptr = FindExtMem(extaddr, num);
	if (extmemptr)
	{
	    memcpy(hubaddr, extmemptr, num);
	    LONG(SYS_PARM) = num;
	}
	else
	{
	    LONG(SYS_PARM) = 0;
	}
    }
    else if (command == SYS_EXTMEM_WRITE)
    {
	uint32_t extaddr = (int32_t)LONG(parm);
	char *hubaddr = (char *)&BYTE(LONG(parm+4));
	int32_t num = LONG(parm+8);
	char *extmemptr = FindExtMem(extaddr, num);
	if (extmemptr)
	{
	    memcpy(extmemptr, hubaddr, num);
	    LONG(SYS_PARM) = num;
	}
	else
	{
	    LONG(SYS_PARM) = 0;
	}
    }
    else if (command == SYS_EXTMEM_ALLOC)
    {
	uint32_t extaddr = (int32_t)LONG(parm);
	int32_t num = LONG(parm+4);
	uint32_t extaddr1 = extaddr + num - 1;

	if (num <= 0 || extmemnum >= 4) num = 0;
	else
	{
	    int i;
	    uint32_t curraddr, curraddr1;
	    for (i = 0; i < extmemnum; i++)
	    {
	        curraddr = extmembase[i];
	        curraddr1 = curraddr + extmemsize[i] - 1;
	        if (curraddr <= extaddr && extaddr <= curraddr1) break;
	        if (curraddr <= extaddr1 && extaddr1 <= curraddr1) break;
	        if (extaddr <= curraddr && curraddr <= extaddr1) break;
	        if (extaddr <= curraddr1 && curraddr1 <= extaddr1) break;
	    }
	    if (i != extmemnum) num = 0;
	    else
	    {
	        extram[extmemnum] = malloc(num);
	        if (extram[extmemnum] == 0) num = 0;
		else
	        {
		    extmemsize[extmemnum] = num;
		    extmembase[extmemnum] = extaddr;
		    extmemnum++;
	        }
	    }
	}
	LONG(SYS_PARM) = num;
    }
    WORD(SYS_COMMAND) = 0;
}

int CheckSerialIn(void)
{
    static int state = 0;
    static int count = 0;
    static int val;

    if (state == 0)
    {
	if (kbhit1())
	{
	    val = getch();
            if (val == 0x1d) return 1;
	    val |= 0x300;
            if (proptwo)
                count = 80000000 / baudrate;
            else
            {
	        count = LONG(0) / baudrate;
                count >>= 2;
            }
	    //if (!proptwo) count >>= 2;
	    pin_val &= 0x7fffffff;
	    state = 1;
	}
    }
    else if (--count <= 0)
    {
	if (++state > 11)
	{
	    state = 0;
	}
	else
	{
	    pin_val = (pin_val & 0x7fffffff) | ((val & 1) << 31);
	    val >>= 1;
#if 0
	    count = LONG(0) / baudrate;
	    if (!proptwo) count >>= 2;
#endif
            if (proptwo)
                count = 80000000 / baudrate;
            else
            {
	        count = LONG(0) / baudrate;
                count >>= 2;
            }
	}
    }
    return 0;
}

void CheckSerialOut(void)
{
    int txbit = 0;
    static int val;
    static int state = -2;
    static int count;
    //static int txbit0 = 0;

    txbit = (pin_val >> 30) & 1;

    //if (txbit != txbit0) fprintf(stderr, "txbit = %d, loopcount = %d\n", txbit, loopcount);
    //txbit0 = txbit;


    if (state == -2)
    {
	if (txbit)
	{
	    state = -1;
	    //fprintf(stderr, "Start Serial\n");
	}
    }
    else if (state == -1)
    {
	if (!txbit)
	{
	    val = 0;
	    state = 0;
#if 0
            count = LONG(0) / baudrate;
            if (!proptwo) count >>= 2;
#endif
            if (proptwo)
                count = 80000000 / baudrate;
            else
            {
	        count = LONG(0) / baudrate;
                count >>= 2;
            }
	    count += count >> 1;
	}
    }
    else
    {
	if (--count <= 0)
	{
	    if (state > 7)
	    {
		state = -1;
#if 1
		if (val == 13)
		    putchx(10);
		else
		    putchx(val);
#else
		printf("<%2.2x>\n", val);
#endif
	    }
	    else
	    {
		//fprintf(stderr, "%d", txbit);
	        val |= txbit << state;
#if 0
	        count = LONG(0) / baudrate;
	        if (!proptwo) count >>= 2;
#endif
                if (proptwo)
                    count = 80000000 / baudrate;
                else
                {
	            count = LONG(0) / baudrate;
                    count >>= 2;
                }
		state++;
	    }
	}
    }
}

void PrintStack(SpinVarsT *spinvars)
{
    int32_t dcurr = spinvars->dcurr;
    printf("PrintStack: %4.4x %8.8x %8.8x %8.8x\n",
        dcurr, LONG(dcurr-4), LONG(dcurr-8), LONG(dcurr-12));
}

char *bootfile;

void RebootProp(void)
{
    int32_t i;
    int32_t dbase;
    char *ptr;
    FILE *infile;

    if (!proptwo) memset(hubram, 0, 32768);
    memset(lockstate, 0, 8);
    memset(lockalloc, 0, 8);

    chdir(rootdir);

    if(!gdbmode && eeprom){
      EEPromCopy(hubram);
    } else
    if(!gdbmode){
      infile = fopen(bootfile, "rb");
      
      if (infile == 0)
	{
	  fprintf(stderr, "Could not open %s\n", bootfile);
	  spinsim_exit(1);
	}

      i = fread(hubram, 1, 32768, infile);
      fclose(infile);
    }

    // Copy in the ROM contents
    if (!proptwo)
    {
        memcpy(hubram + 32768, romdata, 32768);
        dbase = WORD(10);
        LONG(dbase-8) = 0xfff9ffff;
        LONG(dbase-4) = 0xfff9ffff;
        LONG(dbase) = 0;
    }

    WORD(SYS_COMMAND) = 0;
    WORD(SYS_LOCKNUM) = 1;
    lockalloc[0] = 1;

    for (i = 0; i < 8; i++) PasmVars[i].state = 0;

    if (pasmspin)
    {
	if (proptwo)
            StartPasmCog2(&PasmVars[0], 0, 0x0e00, 0);
	else
            StartPasmCog(&PasmVars[0], 0x0004, 0xf004, 0);
    }
    else
        StartCog((SpinVarsT *)&PasmVars[0].mem[0x1e0], 4, 0);

    if(!gdbmode){
      strcpy(objname[0], "xxx");
      if (bootfile)
        strcpy(objname[1], bootfile);
      else
        strcpy(objname[1], "");
      ptr = FindChar(objname[1], '.');
      if (*ptr)
	{
	  *ptr = 0;
	  if (symflag && strcmp(ptr + 1, "bin") == 0)
	    symflag = 2;
	}
      if (symflag == 2)
        strcat(objname[1], ".spn");
      else
        strcat(objname[1], ".spin");
      methodnum[0] = 1;
      methodnum[1] = 1;
      methodlev = 1;
    }

    //LONG(SYS_DEBUG) = printflag;
}

int step_chip(void)
{
    int i;
    int state;
    int runflag = 0;
    int breakflag = 0;
    SpinVarsT *spinvars;
    for (i = 0; i < 8; i++)
    {
        state = PasmVars[i].state;
        PasmVars[i].printflag = (LONG(SYS_DEBUG) >> (i*4)) & 15;
        if (state & 4)
        {
            if (PasmVars[i].printflag && state == 5)
            {
                if (!proptwo)
                {
                    fprintf(tracefile, "Cog %d:  ", i);
                    DebugPasmInstruction(&PasmVars[i]);
                }
            }
            if (proptwo)
            {
                breakflag = ExecutePasmInstruction2(&PasmVars[i]);
                if (PasmVars[i].printflag && state == 5)
                    fprintf(tracefile, "\n");
            }
            else
            {
                ExecutePasmInstruction(&PasmVars[i]);
                if (PasmVars[i].printflag && state == 5) printf("\n");
            }
	    if (!breakflag &&
		!(printbreak && PasmVars[i].printflag && state == 5))
		runflag = 1;
        }
        else if (state)
        {
            spinvars = (SpinVarsT *)&PasmVars[i].mem[0x1e0];
            if (PasmVars[i].printflag && state == 1)
            {
                int32_t dcurr = spinvars->dcurr;
                fprintf(tracefile, "Cog %d: %4.4x %8.8x - ", i, dcurr, LONG(dcurr - 4));
                PrintOp(spinvars);
            }
            if (profile) CountOp(spinvars);
            ExecuteOp(spinvars);
            runflag = 1;
        }
    }
    loopcount++;
    return runflag;
}

int main(int argc, char **argv)
{
    char *ptr;
    char *fname = 0;
    int32_t i;
    int32_t maxloops = -1;

    tracefile = stdout;
    ptr = getcwd(rootdir, 100);

    for (i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "-l") == 0){
            if (i+1 == argc || argv[i+1][0] == '-')
            {
		fprintf(stderr, "Trace file not specified\n");
		spinsim_exit(1);
            }
            if (!printflag) printflag = 0xffffffff;
	    i++;
	    tracefile = fopen(argv[i], "wt");
	    if(!tracefile){
		fprintf(stderr, "Unable to open trace file %s.\n", argv[i]);
		spinsim_exit(1);
	    }
	} else if (strcmp(argv[i], "-t") == 0)
	{
            proptwo = 1;
	    pasmspin = 1;
	    memsize = 256;
	    cycleaccurate = 1;
	}
	else if (strcmp(argv[i], "-p") == 0)
	{
	    pasmspin = 1;
	    cycleaccurate = 1;
	}
	else if (strcmp(argv[i], "-s") == 0)
	    symflag = 1;
	else if (strcmp(argv[i], "-P") == 0)
	    profile = 1;
	else if (strncmp(argv[i], "-m", 2) == 0)
	{
	    sscanf(&argv[i][2], "%d", &memsize);
	}
	else if (strncmp(argv[i], "-b", 2) == 0)
	{
	    pasmspin = 1;
	    cycleaccurate = 1;
            if (argv[i][2] == 0)
                baudrate = 115200;
            else
	        sscanf(&argv[i][2], "%d", &baudrate);
	}
	else if (strcmp(argv[i], "-gdb") == 0)
	{
	    gdbmode = 1;
	    cycleaccurate = 1;
	    pasmspin = 1;
	}
	else if (strcmp(argv[i], "-L") == 0)
	{
	  logfile = fopen(argv[++i], "wt");
	}
	else if (strcmp(argv[i], "-r") == 0)
	{
	  cmdfile = fopen(argv[++i], "rt");
	}
	else if (strcmp(argv[i], "-e") == 0)
	{
          eeprom = 1;
	}
	else if (strncmp(argv[i], "-v", 2) == 0)
	{
	    if (argv[i][2])
	    {
		sscanf(&argv[i][2], "%x", &printflag);
		if (!argv[i][3])
		    printflag *= 0x11111111;
	    }
	    else
		printflag = 0xffffffff;
	}
	else if (strcmp(argv[i], "-d") == 0)
	{
	    debugmode = 1;
	}
#if 0
	else if (strncmp(argv[i], "-x", 2) == 0)
	{
	    sscanf(&argv[i][2], "%d", &extmemsize);
	}
#endif
	else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9')
	    sscanf(argv[i] + 1, "%d", &maxloops);
	else if (argv[i][0] == '-')
	    usage();
	else if (!fname)
	    fname = argv[i];
	else
	    usage();
    }

    if (eeprom)
        EEPromInit(fname);

    if(!cmdfile) cmdfile = stdin;

    // Check the hub memory size and allocate it
    if (memsize < 32)
    {
      fprintf(stderr, "Specified memory size is too small\n");
      spinsim_exit(1);
    }
    if (memsize < 64) memsize = 64;
    memsize <<= 10; // Multiply it by 1024
    hubram = malloc(memsize + 16 + 3);
    if (!hubram)
    {
      fprintf(stderr, "Specified memory size is too large\n");
	spinsim_exit(1);
    }
    // Make sure it's long aligned
#ifdef __LP64__
    hubram =  (char *)(((uint64_t)hubram) & 0xfffffffffffffffc);
#else
    hubram =  (char *)(((uint32_t)hubram) & 0xfffffffc);
#endif

    LONG(SYS_DEBUG) = printflag;

#if 0
    // Check the ext memory size and allocate it if non-zero
    if (extmemsize < 0)
    {
      fprintf(stderr, "Invalid external memory size\n");
      spinsim_exit(1);
    }
    else if (extmemsize)
    {
        extmemsize <<= 10; // Multiply it by 1024
        extram = malloc(extmemsize);
        if (!extram)
        {
	  fprintf(stderr, "Specified external memory size is too large\n");
	  spinsim_exit(1);
        }
    }
#endif

    if (profile) ResetStats();

    bootfile = fname;

    if (!fname && !gdbmode && !eeprom) usage();
    
    RebootProp();
// dbetz: not defined for Windows and a nop for anything else
//    initialize_console_io();
    if (gdbmode)
	gdb();
    else if (debugmode)
	Debug();
    else
      RunProp(maxloops);
//    restore_console_io();
    if (eeprom) EEPromClose();
    if (profile) PrintStats();
    return 0;
}
/*
+------------------------------------------------------------------------------------------------------------------------------+
|                                                   TERMS OF USE: MIT License                                                  |
+------------------------------------------------------------------------------------------------------------------------------+
|Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation    |
|files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy,    |
|modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software|
|is furnished to do so, subject to the following conditions:                                                                   |
|                                                                                                                              |
|The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.|
|                                                                                                                              |
|THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE          |
|WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR         |
|COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,   |
|ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                         |
+------------------------------------------------------------------------------------------------------------------------------+
*/
