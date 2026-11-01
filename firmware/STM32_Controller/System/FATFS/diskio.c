/* ==================================================================
 * diskio.c —— FatFs 的物理层（把文件系统的扇区读写绑到 SD 卡驱动）
 *
 * FatFs 只认 disk_initialize / disk_status / disk_read / disk_ioctl 这几个
 * 函数名（R0.09 的规定，不能改名），内部转手调用 Hardware/16SD 的 Sd_* 接口。
 * 本工程是只读相册（ffconf.h 里 _FS_READONLY = 1），所以不实现 disk_write。
 *
 * 依赖：Hardware/16SD/sd.c
 * ================================================================== */

#include "diskio.h"
#include "sd.h"

static volatile DSTATUS s_status = STA_NOINIT;   /* 卡状态，Sd_Init 成功后清 STA_NOINIT */

/* 初始化物理盘：返回 0 表示 OK，STA_NOINIT 表示没准备好 */
DSTATUS disk_initialize(BYTE drv)
{
    if(drv != 0)
        return STA_NOINIT;

    /* main 已验证SD；挂载文件系统无需重跑卡初始化。 */
    s_status = Sd_Get_Sector_Count() ? 0 : STA_NOINIT;
    return s_status;
}

/* 查询卡状态（FatFs 在读写前会调） */
DSTATUS disk_status(BYTE drv)
{
    if(drv != 0)
        return STA_NOINIT;

    return s_status;
}

/* 读扇区：sector 是扇区号（不是字节地址），count 是要读的扇区数 */
DRESULT disk_read(BYTE drv, BYTE *buff, DWORD sector, BYTE count)
{
    if((drv != 0) || (count == 0) || (buff == 0))
        return RES_PARERR;
    if(s_status & STA_NOINIT)
        return RES_NOTRDY;

    if(Sd_Read_Sector((u32)sector, (u8 *)buff, (u16)count) != SD_OK)
        return RES_ERROR;

    return RES_OK;
}

/* 杂项控制命令（本工程只读，用的很少；都答上，别让 FatFs 报 PARERR） */
DRESULT disk_ioctl(BYTE drv, BYTE cmd, void *buff)
{
    if(drv != 0)
        return RES_PARERR;

    switch(cmd)
    {
        case CTRL_SYNC:                     /* 只读，没有写缓存要刷 */
            return RES_OK;

        case GET_SECTOR_COUNT:              /* 容量（扇区数），给 f_mkfs 用 */
            if(buff == 0)
                return RES_PARERR;
            *(DWORD *)buff = Sd_Get_Sector_Count();
            return RES_OK;

        case GET_SECTOR_SIZE:               /* 扇区大小，固定 512 */
            if(buff == 0)
                return RES_PARERR;
            *(WORD *)buff = 512;
            return RES_OK;

        case GET_BLOCK_SIZE:                /* 擦除块大小，SD 卡填 1 即可 */
            if(buff == 0)
                return RES_PARERR;
            *(DWORD *)buff = 1;
            return RES_OK;

        default:
            return RES_PARERR;
    }
}
