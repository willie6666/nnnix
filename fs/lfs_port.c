/* SPDX-License-Identifier: GPL-3.0-or-later */
/* littlefs support code: CRC-32 and the onboard SPI flash as its block device. */
#include "lfs_port.h"
#include "../third_party/littlefs/lfs.h"
#include "../drivers/spiflash.h"

/* Same table-driven CRC-32 (polynomial 0x04c11db7, reflected) as upstream lfs_util.c. */
uint32_t lfs_crc(uint32_t crc, const void *buffer, size_t size)
{
    static const uint32_t rtable[16] = {
        0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
        0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
    };
    const uint8_t *data = buffer;
    for (size_t i = 0; i < size; i++) {
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 0)) & 0xf];
        crc = (crc >> 4) ^ rtable[(crc ^ (data[i] >> 4)) & 0xf];
    }
    return crc;
}

int lfs_port_read(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, void *buffer, lfs_size_t size)
{
    spiflash_read(block * c->block_size + off, buffer, size);
    return LFS_ERR_OK;
}

int lfs_port_prog(const struct lfs_config *c, lfs_block_t block, lfs_off_t off, const void *buffer, lfs_size_t size)
{
    return spiflash_program(block * c->block_size + off, buffer, size) ? LFS_ERR_IO : LFS_ERR_OK;
}

int lfs_port_erase(const struct lfs_config *c, lfs_block_t block)
{
    return spiflash_erase_sector(block * c->block_size) ? LFS_ERR_IO : LFS_ERR_OK;
}

int lfs_port_sync(const struct lfs_config *c)
{
    (void)c;
    return LFS_ERR_OK;
}
