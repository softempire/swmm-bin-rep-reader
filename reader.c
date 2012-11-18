#include "reader.h"
#include <inttypes.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "enums.h"
#include "parse_types.h"

#define MAGIC 516114522

struct sbrr_t {
    struct stat buf;
    int fd;
    char *map;
    header_t *header;
    footer_t *footer;

//ids
    char **node_ids;
    char **link_ids;
    char **subcatchment_ids;
    int32_t *pollutant_types;

//sizes
    size_t subcatchment_result_len;
    size_t node_result_len;
    size_t link_result_len;
    size_t bytes_per_period;
};

size_t read_ids(char **ids, char *map, size_t _offset, int index ) {
    size_t offset = _offset;
    for (int i = 0; i < index ; i++) {
        swmm_id_t *id = (swmm_id_t*) (map + offset);
        ids[i] = (char*) malloc(id->len+1);
        assert(id->len < 512);
        strncpy(ids[i], id->id, id->len);
        ids[i][id->len] = '\0';
        offset += sizeof(int32_t) + id->len*sizeof(char);
    }
    return offset - _offset;
}


int sbrr_create(sbrr *handle, const char *binary_report_file) {
    size_t offset;

    *handle = NULL;

    sbrr tmp_handle = (sbrr) malloc(sizeof(struct sbrr_t));

    tmp_handle->fd = open(binary_report_file, O_RDONLY);
    if (tmp_handle->fd < 0) {
        free(tmp_handle);
        return errno;
    }

    if (fstat(tmp_handle->fd, &tmp_handle->buf) < 0) {
        free(tmp_handle);
        close(tmp_handle->fd);
        return errno;
    }

    tmp_handle->map = (char *) mmap(0, tmp_handle->buf.st_size, PROT_READ, MAP_SHARED, tmp_handle->fd, 0);

    if (tmp_handle->map == MAP_FAILED) {
        free(tmp_handle);
        close(tmp_handle->fd);
        return errno;
    }

    tmp_handle->header = (header_t*) tmp_handle->map;
    tmp_handle->footer = (footer_t*) (tmp_handle->map + tmp_handle->buf.st_size-sizeof(footer_t));
    if (tmp_handle->header->magic != MAGIC || tmp_handle->footer->magic != MAGIC) {
        free(tmp_handle);
        close(tmp_handle->fd);
        munmap((void *) tmp_handle->map, tmp_handle->buf.st_size);
        return -1;
    }

    offset = sizeof(header_t);

    tmp_handle->subcatchment_ids = (char **) malloc(tmp_handle->header->num_subcatchments * sizeof(char **));
    tmp_handle->node_ids = (char **) malloc(tmp_handle->header->num_nodes * sizeof(char **));
    tmp_handle->link_ids = (char **) malloc(tmp_handle->header->num_links * sizeof(char **));

    offset += read_ids(tmp_handle->subcatchment_ids, tmp_handle->map, offset, tmp_handle->header->num_subcatchments);
    offset += read_ids(tmp_handle->node_ids, tmp_handle->map, offset, tmp_handle->header->num_nodes);
    offset += read_ids(tmp_handle->link_ids, tmp_handle->map, offset, tmp_handle->header->num_links);

    tmp_handle->pollutant_types = malloc(sizeof(int)*tmp_handle->header->num_pollutants);
    for (int i = 0; i < tmp_handle->header->num_pollutants; i++) {
        int32_t *type = (int32_t*) (tmp_handle->map+offset);
        tmp_handle->pollutant_types[i] = *type;
        offset += sizeof(int32_t);
    }

    int npol = tmp_handle->header->num_pollutants;
    tmp_handle->subcatchment_result_len =  MAX_SUBCATCH_RESULTS - 1 + npol;
    tmp_handle->node_result_len = MAX_NODE_RESULTS - 1 + npol;
    tmp_handle->link_result_len = MAX_LINK_RESULTS - 1 + npol;

    tmp_handle->bytes_per_period = sizeof(double)
        + tmp_handle->header->num_subcatchments * tmp_handle->subcatchment_result_len * sizeof(float)
        + tmp_handle->header->num_nodes * tmp_handle->node_result_len * sizeof(float)
        + tmp_handle->header->num_links * tmp_handle->link_result_len * sizeof(float)
        + MAX_SYS_RESULTS * sizeof(float);

    *handle = (sbrr) tmp_handle;
    return 0;
}

void sbrr_destroy(sbrr handle) {
    free(handle->pollutant_types);
    for (int i = 0; i < handle->header->num_links; ++i) {
        free(handle->link_ids[i]);
    }
    free(handle->link_ids);
    for (int i = 0; i < handle->header->num_nodes; ++i) {
        free(handle->node_ids[i]);
    }
    free(handle->node_ids);
    for (int i = 0; i < handle->header->num_subcatchments; ++i) {
        free(handle->subcatchment_ids[i]);
    }
    free(handle->subcatchment_ids);
    munmap((void*) handle->map, handle->buf.st_size);
    close(handle->fd);
    free(handle);
}

int sbrr_get_num_elements(sbrr handle,
                          ElementType type) {
    switch (type) {
    case Node:
        return handle->header->num_nodes;
        break;
    case Link:
        return handle->header->num_links;
        break;
    case SubCatchement:
        return handle->header->num_subcatchments;
        break;
    case Pollutant:
        return handle->header->num_pollutants;
        break;
    default:
        return -1;
        break;
    }
    return -1;
}

int sbrr_get_num_periods(sbrr handle) {
    return handle->footer->num_periods;
}

char    *sbrr_get_element_id(sbrr handle,
                             int index ,
                             ElementType type) {
    char **ids;
    if (type == Node) ids = handle->node_ids;
    if (type == Link) ids = handle->link_ids;
    if (type == SubCatchement) ids = handle->subcatchment_ids;

    return ids[index ];
}

double sbrr_get_result_date(sbrr handle, int period) {
    size_t offset = handle->footer->output_offset + (period-1)*handle->bytes_per_period;
    return *((double *) (handle->map + offset));
}

float *sbrr_get_element_results(sbrr handle,
                                int period,
                                int index,
                                ElementType type) {
    assert(type == SubCatchement);
    size_t offset = handle->footer->output_offset + (period-1)*handle->bytes_per_period;
    offset += sizeof(double);
    offset += index * handle->subcatchment_result_len*sizeof(float);
    return (float *) (handle->map + offset);
}
