#ifndef MP_BUF_SIZES_H
#define MP_BUF_SIZES_H

#define MP_MAXMSGLEN 4096
#define MP_HEX_ADDR_LEN (sizeof(void*) * 2 + 3) /* room for a ptr address in hexadecimal
                                              + 0,x, null terminator*/
#define MP_MAXREADCHUNK 8192

#endif