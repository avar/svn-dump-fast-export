#ifndef LINE_BUFFER_H_
#define LINE_BUFFER_H_

#include <stdint.h>

int buffer_init(char *filename);
int buffer_deinit(void);
char *buffer_read_line(void);
char *buffer_read_string(uint32_t len);
void buffer_copy_bytes(uint32_t len);
void buffer_skip_bytes(uint32_t len);
void buffer_reset(void);

#endif
