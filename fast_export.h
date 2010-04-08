#ifndef FAST_EXPORT_H_
#define FAST_EXPORT_H_

#include <stdint.h>
#include <time.h>

void fast_export_delete(uint32_t depth, uint32_t * path);

void fast_export_modify(uint32_t depth, uint32_t * path, uint32_t mode,
                        uint32_t mark);

void fast_export_commit(uint32_t revision, char * author, char * log,
                        char * uuid, char * url, time_t timestamp);

void fast_export_blob(uint32_t mode, uint32_t mark, uint32_t len);

#endif
