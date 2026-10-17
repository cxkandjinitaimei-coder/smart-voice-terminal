/**
 * @file diskio.h
 * 用途：FatFs 扇区接口到本工程 SDIO 驱动的适配层。
 * 使用：由 FatFs 调用 initialize/read/write/ioctl；返回 DRESULT，不直接绘图或解释目录。
 * 时序与边界：遵守 512 字节扇区和驱动的卡状态；FatFs 公共头及许可证保持原样。
 * 接口说明：本头声明或内联接口；调用方包含后仍须按上述顺序初始化对应驱动。
 * 维护：修改协议/数据格式时同时核对另一端与 docs 中的约定；不依赖外层项目文件夹名称。
 */
#ifndef _DISKIO

#define _READONLY 1  /* 1: Remove write functions */
#define _USE_IOCTL 1 /* 1: Use disk_ioctl fucntion */

#include "integer.h"

/* Status of Disk Functions */
typedef BYTE DSTATUS;

/* Results of Disk Functions */
typedef enum
{
    RES_OK = 0, /* 0: Successful */
    RES_ERROR,  /* 1: R/W Error */
    RES_WRPRT,  /* 2: Write Protected */
    RES_NOTRDY, /* 3: Not Ready */
    RES_PARERR  /* 4: Invalid Parameter */
} DRESULT;

/*---------------------------------------*/
/* Prototypes for disk control functions */

int assign_drives(int, int);
DSTATUS disk_initialize(BYTE);
DSTATUS disk_status(BYTE);
DRESULT disk_read(BYTE, BYTE *, DWORD, BYTE);
#if _READONLY == 0
DRESULT disk_write(BYTE, const BYTE *, DWORD, BYTE);
#endif
DRESULT disk_ioctl(BYTE, BYTE, void *);

/* Disk Status Bits (DSTATUS) */

#define STA_NOINIT 0x01  /* Drive not initialized */
#define STA_NODISK 0x02  /* No medium in the drive */
#define STA_PROTECT 0x04 /* Write protected */

/* Command code for disk_ioctrl fucntion */

/* Generic command (defined for FatFs) */
#define CTRL_SYNC 0         /* Flush disk cache (for write functions) */
#define GET_SECTOR_COUNT 1  /* Get media size (for only f_mkfs()) */
#define GET_SECTOR_SIZE 2   /* Get sector size (for multiple sector size (_MAX_SS >= 1024)) */
#define GET_BLOCK_SIZE 3    /* Get erase block size (for only f_mkfs()) */
#define CTRL_ERASE_SECTOR 4 /* Force erased a block of sectors (for only _USE_ERASE) */

/* Generic command */
#define CTRL_POWER 5 /* Get/Set power status */
#define CTRL_LOCK 6  /* Lock/Unlock media removal */
#define CTRL_EJECT 7 /* Eject media */

/* MMC/SDC specific ioctl command */
#define MMC_GET_TYPE 10   /* Get card type */
#define MMC_GET_CSD 11    /* Get CSD */
#define MMC_GET_CID 12    /* Get CID */
#define MMC_GET_OCR 13    /* Get OCR */
#define MMC_GET_SDSTAT 14 /* Get SD status */

/* ATA/CF specific ioctl command */
#define ATA_GET_REV 20   /* Get F/W revision */
#define ATA_GET_MODEL 21 /* Get model name */
#define ATA_GET_SN 22    /* Get serial number */

/* NAND specific ioctl command */
#define NAND_FORMAT 30 /* Create physical format */

#define _DISKIO
#endif
