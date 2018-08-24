#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

#include <sys/mman.h>

#include "entries.h"
#include "config.h"
#include "file_utils.h"


struct search {
    uint8_t status:1;
    uint8_t case_insensitive:1;
    uint8_t raw_search:1;

    /* search parameters */
    char *directory;
    char *pattern;
    char * (*parser)(const char *, const char *, int);
    char *file_types;

    /* storage */
    struct entries *entries;
};


/* SEARCH ALGORITHMS **********************************************************/
static char * normal_search(const char *line, const char *pattern, int size)
{
    (void) size;

    return strstr(line, pattern);
}

static char * insensitive_search(const char *line, const char *pattern, int size)
{
    (void) size;

    return strcasestr(line, pattern);
}


/* FILE PARSING ***************************************************************/
static void parse_file_contents(struct search *this, const char *file, char *p,
                                const size_t p_len)
{
    char *endline;
    uint8_t first = 1;
    uint32_t line_number = 1;
    char *orig_p = p;

    while ((endline = strchr(p, '\n'))) {

        *endline = '\0';

        if (this->parser(p, this->pattern, endline - p) != NULL) {

            if (first) {
                /* add file */
                entries_add(this->entries, 0, file);
                first = 0;
            }

            entries_add(this->entries, line_number, p);
        }

        p = endline + 1;
        line_number++;
    }

    /* special case of not newline terminated file */
    if (endline == NULL && p < orig_p + p_len) {
        if (this->parser(p, this->pattern, endline - p) != NULL) {

            if (first) {
                /* add file */
                entries_add(this->entries, 0, file);
                first = 0;
            }

            entries_add(this->entries, line_number, p);
        }
    }
}

static uint8_t lookup_file(struct search *this, const char *file)
{
    /* check file extension */
    if (!file_utils_check_extension(file, this->file_types) &&
        !this->raw_search) {
        return EXIT_FAILURE;
    }

    int f = open(file, O_RDONLY);
    if (f == -1) {
        //printf("Failed opening file %s\n", file);
        return EXIT_FAILURE;
    }

    struct stat sb;
    if (fstat(f, &sb) < 0) {
        //printf("Failed stat on file %s\n", file);
        close(f);
        return EXIT_FAILURE;
    }

    /* return if file is empty */
    if (sb.st_size == 0) {
        close(f);
        return EXIT_SUCCESS;
    }

    char *p = mmap(0, sb.st_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, f, 0);
    if (p == MAP_FAILED) {
        //printf("Failed mapping file %s\n", file);
        close(f);
        return EXIT_FAILURE;
    }

    char *pp = p;
    parse_file_contents(this, file, p, sb.st_size);

    if (munmap(pp, sb.st_size) < 0) {
        //printf("Failed unmapping file %s\n", file);
        close(f);
        return EXIT_FAILURE;
    }

    close(f);
    return EXIT_SUCCESS;
}


/* DIRECTORY PARSING **********************************************************/
static uint32_t lookup_directory(struct search *this, const char *directory)
{
    //printf("Looking up directory %s\n", directory);

    DIR *dir_stream = opendir(directory);
    if (dir_stream == NULL) {
        //printf("Failed opening directory %s\n", directory);
        return EXIT_FAILURE;
    }

    while (1) {
        struct dirent *dir_entry = readdir(dir_stream);

        if (dir_entry == NULL) {
            break;
        }

        char dir_entry_path[PATH_MAX];
        snprintf(dir_entry_path, PATH_MAX, "%s/%s", directory, dir_entry->d_name);

        if (!(dir_entry->d_type&DT_DIR)) {
            lookup_file(this, dir_entry_path);
        } else if (!file_utils_is_dir_special(dir_entry->d_name)) {
            lookup_directory(this, dir_entry_path);
        }
    }

    closedir(dir_stream);

    return EXIT_SUCCESS;
}


/* GET ************************************************************************/
char * search_get_pattern(const struct search *this)
{
    return this->pattern;
}

uint8_t search_get_status(const struct search *this)
{
    return this->status;
}


/* SEARCH THREAD ENTRY POINT **************************************************/
void * search_thread_start(void *context)
{
    struct search *this = context;

    if (file_utils_is_file(this->directory)) {
        lookup_file(this, this->directory);
    } else if (file_utils_is_dir(this->directory)) {
        lookup_directory(this, this->directory);
    }

    /* search is done */
    this->status = 0;

    return NULL;
}


/* CONSTRUCTOR ****************************************************************/
struct search * search_new(const char *directory, const char *pattern,
                           struct entries *entries, struct config *config)
{
    struct search *this = calloc(1, sizeof(struct search));
    this->directory = strdup(directory);
    this->pattern = strdup(pattern);
    this->entries = entries;
    this->case_insensitive = config->insensitive_search;
    this->raw_search = config->raw_search;
    this->file_types = config->file_types;

    if (config->insensitive_search) {
        this->parser = insensitive_search;
    } else {
        this->parser = normal_search;
    }

    return this;
}

void search_delete(struct search *this)
{
    free(this->pattern);
    free(this->directory);
    free(this);
}
