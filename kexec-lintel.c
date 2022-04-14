#include "common.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <utmp.h>
#include <utmpx.h>
#include <unistd.h>
#include <sys/klog.h>
#include <sys/ioctl.h>

typedef uint64_t u64;
#include <asm/kexec.h>

const size_t alignment = 4096;

const uint64_t LINTEL_BCD_SIGNATURE = 0x012345678ABCDEF0ull;

struct lintel_reboot_param lintel __attribute__((aligned(alignment)));

struct __attribute__((packed)) xrt_BcdHeader_t
{
    uint64_t signature;
    uint32_t files_num;
    uint64_t free_lba;
};

struct __attribute__((packed)) xrt_BcdFile_t
{
    uint64_t lba;
    uint64_t size;
    uint64_t init_size;
    uint32_t tag;
    uint32_t checksum;
};

enum xrt_BcdFileTag_t
{
    PRIORITY_TAG_LINTEL,
    PRIORITY_TAG_LINTEL_OBJ,
    PRIORITY_TAG_X86BIOS,
    PRIORITY_TAG_X86BIOS_RECOVERY,
    PRIORITY_TAG_LIBRCOMP,
    PRIORITY_TAG_BCDBOOTINFO,
    PRIORITY_TAG_CODEBASE,
    PRIORITY_TAG_LOG,
    PRIORITY_TAG_VIDEOBIOS,
    PRIORITY_TAG_KEXEC_JUMPER
};

void cancel(int num, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    exit(num);
}

static void check_runlevel(void)
{
    /* For the sake of not rebooting fully running system, restrict to runlevel 1 only. We suppose nothing that may leave garbage in filesystem is running there. */
    int runlevel = -1;
    struct utmpx *ut;

    setutxent();
    while ((ut = getutxent()) != NULL)
    {
        if (ut->ut_type == RUN_LVL)
        {
            runlevel = ut->ut_pid % 256 - '0';
            break;
        }
    }
    endutxent();

    if (runlevel < 0) cancel(C_RUNLEVEL_NONE, "Can't get current runlevel: %s\n", errno ? strerror(errno) : "No RUN_LVL entry in utmp file");
    if (runlevel != 1) cancel(C_RUNLEVEL_WRONG, "You should run this only from runlevel 1, but current runlevel is %d\n", runlevel);
}

static void free_lintel(void)
{
    free(lintel.image);
}

static void read_lintel(FILE *f, size_t realsize)
{
    lintel.image_size = realsize; /* Note: this should EXACTLY match the lintel binary size, because it is used to calculate jump address (mcstbug#133402 comment 38) */
    size_t aligned_size = realsize + alignment; aligned_size -= aligned_size % alignment;
    if (posix_memalign(&lintel.image, alignment, aligned_size)) { fclose(f); cancel(C_FILE_ALLOC, "Can't allocate %ld bytes for lintel file of %ld bytes\n", aligned_size, lintel.image_size); }
    atexit(free_lintel);
    if (fread(lintel.image, lintel.image_size, 1, f) != 1) { fclose(f); cancel(C_FILE_READ, "Can't read %ld bytes for lintel file, file might be truncated\n", lintel.image_size); }
    printf("Loaded lintel: %ld bytes at address %p (%ld bytes aligned at 0x%lx), ioctl struct at %p\n", lintel.image_size, lintel.image, aligned_size, alignment, &lintel);
    if(fclose(f)) cancel(C_FILE_CLOSE, "Can't close lintel file\n");
}

static struct xrt_BcdHeader_t bcd_check_files(FILE *f)
{
    if (fseek(f, 512, SEEK_SET) != 0) { fclose(f); cancel(C_BCD_SEEK, "Can't seek to possible header of file: %s\n", strerror(errno)); }
    struct xrt_BcdHeader_t header;
    if (fread(&header, sizeof(header), 1, f) != 1) { fclose(f); cancel(C_BCD_HEADER, "Can't read header of lintel file, file might be truncated\n"); }
    if (header.signature != LINTEL_BCD_SIGNATURE) header.files_num = -1;
    return header;
}

static void patch_jumper_info(const struct xrt_BcdFile_t super_file)
{
    printf("BCD file contains kexec jumper, patching the header.\n");

    struct xrt_BcdHeader_t *subheader = (struct xrt_BcdHeader_t*)((char*)lintel.image + (super_file.init_size - 1) * 512); /* BCD map should be located in the last sector of lintel file */
    if (subheader->signature != LINTEL_BCD_SIGNATURE) cancel(C_SUPER_HEADER, "Can't find BCD signature in super file\n");
    struct xrt_BcdFile_t *files = (struct xrt_BcdFile_t*)((char*)subheader + sizeof(struct xrt_BcdHeader_t));
    for (int i = 0; i < subheader->files_num; ++i)
    {
        if ( files[i].tag == PRIORITY_TAG_KEXEC_JUMPER )
        {
            files[i].lba = super_file.lba;
            files[i].size = super_file.size;
            return;
        }
    }
    cancel(C_SUPER_JUMPER, "Can't find kexec jumper in super file\n");
}

static void load_bcd_lintel(FILE *f, const struct xrt_BcdHeader_t header)
{
    printf ("File is BCD container (%d files).\n", header.files_num);

    struct xrt_BcdFile_t super_file = {0, 0, 0, 0, 0};
    for (int i = 0; i < header.files_num; ++i)
    {
        struct xrt_BcdFile_t file;
        if (fread(&file, sizeof(file), 1, f) != 1) { fclose(f); cancel(C_BCD_FILEHEADER, "Can't read file %d header of BCD file, file might be truncated\n"); }
        printf("BCD file %d: /%d, offset %ld blocks, size %ld blocks, init_size %ld blocks, checksum 0x%08x\n", i, file.tag, file.lba, file.size, file.init_size, file.checksum);

        if (file.tag == PRIORITY_TAG_LINTEL)
        {
            if (i != 0) { fclose(f); cancel(C_BCD_ORDER, "Lintel file must be the first one in BCD\n"); }
            if (file.size > file.init_size) { fclose(f); cancel(C_BCD_READ, "Can't read lintel file from BCD file: file is uninitialized\n"); }
            super_file.tag = file.tag;
            super_file.lba = file.lba;
            super_file.init_size = file.size; /* Save for future patching in case of kexec jumper exists */
            super_file.size = file.size;
        }
        if (file.tag == PRIORITY_TAG_KEXEC_JUMPER)
        {
            super_file.tag = file.tag;
            super_file.size = header.free_lba - super_file.lba;
            break;
        }
    }
    if (!super_file.size) { fclose(f); cancel(C_BCD_NOTFOUND, "Can't find lintel file in BCD file\n"); }

    if (fseek(f, 512 * super_file.lba, SEEK_SET) != 0) { fclose(f); cancel(C_BCD_SEEK, "Can't seek to start of lintel binary in BCD file: %s\n", strerror(errno)); }
    read_lintel(f, 512 * super_file.size);
    if (super_file.tag == PRIORITY_TAG_KEXEC_JUMPER) patch_jumper_info(super_file);
}

static void load_raw_lintel(FILE *f)
{
    size_t realsize;
    printf ("File seems to be raw lintel image.\n");
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); cancel(C_FILE_SEEK, "Can't seek lintel file: %s\n", strerror(errno)); }
    if ((realsize = ftell(f)) == -1) { fclose(f); cancel(C_FILE_TELL, "Can't get file position of lintel file: %s\n", strerror(errno)); }
    rewind(f);
    read_lintel(f, realsize);
}

static void load_lintel(const char *fname)
{
    FILE *f = fopen(fname,"r");
    if (f == NULL) cancel(C_FILE_OPEN, "Can't open %s: %s\n", fname, strerror(errno));
    printf("Loading lintel from %s:\n", fname);

    struct xrt_BcdHeader_t header = bcd_check_files(f);
    if (header.files_num == -1) load_raw_lintel(f);
    else load_bcd_lintel(f, header);
}

static int check_syslog(const char *marker)
{
    char buf[1001];
    memset(buf, 0, 1001);
    int len = klogctl(3, buf, 1000);
    return strstr(buf, marker) != NULL;
}

static void remount_filesystems()
{
    FILE *f = fopen("/proc/sysrq-trigger","w");
    if (f == NULL) cancel(C_SYSRQ_OPEN, "Can't open sysrq-trigger file: %s\n", strerror(errno));
    if (fprintf(f, "u\n") < 1) { fclose(f); cancel(C_SYSRQ_WRITE, "Can't write to sysrq-trigger file\n"); }
    if (fclose(f)) cancel(C_SYSRQ_CLOSE, "Can't close sysrq-trigger file: %s\n", strerror(errno));
    while(!check_syslog("Emergency Remount complete\n"));
}

static const char *check_args(int argc, char *argv[], const char *def, int *tty)
{
    if (argc > 1)
    {
        if (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))
        {
            cancel(C_SUCCESS, "Usage: %s [ [--tty <N>] <path> | -h | --help ]\n\t<N> active tty number (default is %d)\n\t<path> is path to lintel file (default is %s)\n\t-h | --help: Print this help\n", argv[0], *tty, def);
        }
        if (!strcmp(argv[1], "--tty"))
        {
            if (argc >= 3)
            {
                char *endp;
                errno = 0;
                *tty = strtol(argv[2], &endp, 0);
                if (errno || *endp)
                {
                    cancel(C_TTY_WRONG, "Malformed tty number %s (run %s --help for usage)", argv[2], argv[0]);
                }
                return (argc == 3) ? def : argv[3];
            }
            else
            {
                cancel(C_TTY_NONE, "You must specify tty number (run %s --help for usage)", argv[0]);
            }
        }
        return argv[1];
    }
    return def;
}

static int open_kexec()
{
    int fd;
    if ((fd = open("/dev/kexec", O_RDONLY)) == -1) cancel(C_DEV_OPEN, "Can't open kexec device: %s\n", strerror(errno));
    return fd;
}

int main(int argc, char *argv[])
{
    int tty = 1;
    const char *fname = check_args(argc, argv, "/opt/mcst/lintel/bin/lintel_e8c.disk", &tty);

    #ifndef NO_IOMMU_CHECK
        check_iommu();
    #endif

    check_runlevel();

    load_lintel(fname);

    #ifndef NO_FBRESET
        printf("Resetting video driver...\n");
        reset_fbdriver(tty);
    #endif

    printf("Flushing filesystems...\n");
    sync();
    remount_filesystems();

    printf("Rebooting to lintel...\n");
    int kexec_fd = open_kexec();
    int rv = ioctl(kexec_fd, LINTEL_REBOOT, &lintel);

    int err = errno;
    close(kexec_fd);
    cancel(C_DEV_IOCTL, "Failure performing ioctl (returned %d) to start lintel: %s\nNote: you should remount everything back to rw to bring system back to work\n", rv, strerror(err));
}
