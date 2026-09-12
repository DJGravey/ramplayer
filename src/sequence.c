#include "sequence.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "util.h"

/* ---- small helpers ------------------------------------------------------ */

static int has_image_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    return strcasecmp(dot, ".exr") == 0;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static char *dir_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash) return rp_strdup(".");
    if (slash == path) return rp_strdup("/");
    size_t n = (size_t)(slash - path);
    char *d = rp_xmalloc(n + 1);
    memcpy(d, path, n);
    d[n] = '\0';
    return d;
}

static char *join_path(const char *dir, const char *name)
{
    size_t dn = strlen(dir);
    int need_slash = dn > 0 && dir[dn - 1] != '/';
    size_t n = dn + (size_t)need_slash + strlen(name) + 1;
    char *p = rp_xmalloc(n);
    snprintf(p, n, "%s%s%s", dir, need_slash ? "/" : "", name);
    return p;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* ---- name templates ----------------------------------------------------- */

/* A template is the part of a basename before and after the frame number. */
typedef struct {
    char prefix[512];
    char suffix[256];
    int  digits; /* width seen in the sample name, for the display string */
} Template;

/* Splits a basename around its *last* run of digits, which is where the frame
 * number lives in every naming convention we care about ("sh010_beauty.0042.exr"
 * must resolve to 0042, not 010). Returns 0 if the name has no digits. */
static int split_basename(const char *name, Template *t, long *number)
{
    size_t len = strlen(name);
    size_t end = len;
    while (end > 0 && !isdigit((unsigned char)name[end - 1])) end--;
    if (end == 0) return 0;

    size_t start = end;
    while (start > 0 && isdigit((unsigned char)name[start - 1])) start--;

    if (start >= sizeof t->prefix || len - end >= sizeof t->suffix) return 0;

    memcpy(t->prefix, name, start);
    t->prefix[start] = '\0';
    memcpy(t->suffix, name + end, len - end);
    t->suffix[len - end] = '\0';
    t->digits = (int)(end - start);

    if (number) *number = strtol(name + start, NULL, 10);
    return 1;
}

/* Recognises "name.%04d.exr" and "name.####.exr" and turns either into a
 * template. Returns 0 if the string holds no such placeholder. */
static int split_pattern(const char *name, Template *t)
{
    const char *hash = strchr(name, '#');
    const char *pct  = strchr(name, '%');
    const char *start = NULL, *end = NULL;
    int digits = 4;

    if (hash) {
        start = hash;
        end = hash;
        while (*end == '#') end++;
        digits = (int)(end - start);
    } else if (pct) {
        const char *p = pct + 1;
        while (*p == '0') p++;
        const char *dstart = p;
        while (isdigit((unsigned char)*p)) p++;
        if (*p != 'd' && *p != 'i') return 0;
        digits = (p > dstart) ? atoi(dstart) : 1;
        start = pct;
        end = p + 1;
    } else {
        return 0;
    }

    size_t plen = (size_t)(start - name);
    size_t slen = strlen(end);
    if (plen >= sizeof t->prefix || slen >= sizeof t->suffix) return 0;

    memcpy(t->prefix, name, plen);
    t->prefix[plen] = '\0';
    memcpy(t->suffix, end, slen + 1);
    t->digits = digits > 0 ? digits : 1;
    return 1;
}

/* Does `name` fit the template, and if so what is its frame number? Any run
 * length of digits is accepted so that both padded and unpadded numbering
 * work. */
static int template_match(const Template *t, const char *name, long *number)
{
    size_t nlen = strlen(name);
    size_t plen = strlen(t->prefix);
    size_t slen = strlen(t->suffix);
    if (nlen < plen + slen + 1) return 0;
    if (strncmp(name, t->prefix, plen) != 0) return 0;
    if (slen && strcmp(name + nlen - slen, t->suffix) != 0) return 0;

    for (size_t i = plen; i < nlen - slen; i++)
        if (!isdigit((unsigned char)name[i])) return 0;

    if (number) *number = strtol(name + plen, NULL, 10);
    return 1;
}

/* ---- frame list building ------------------------------------------------ */

typedef struct {
    SeqFrame *v;
    int       n, cap;
} FrameVec;

static void fv_push(FrameVec *fv, char *path, long number)
{
    if (fv->n == fv->cap) {
        fv->cap = fv->cap ? fv->cap * 2 : 64;
        fv->v = realloc(fv->v, (size_t)fv->cap * sizeof *fv->v);
        if (!fv->v) rp_fatal("out of memory building frame list");
    }
    fv->v[fv->n].path = path;
    fv->v[fv->n].number = number;
    fv->n++;
}

static int frame_cmp(const void *a, const void *b)
{
    const SeqFrame *fa = a, *fb = b;
    if (fa->number < fb->number) return -1;
    if (fa->number > fb->number) return 1;
    return strcmp(fa->path, fb->path);
}

/* Collects every file in `dir` matching `t`. */
static int collect_matches(const char *dir, const Template *t, FrameVec *fv, char *err, size_t errsz)
{
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(err, errsz, "cannot open directory '%s'", dir);
        return 0;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        long num;
        if (!template_match(t, de->d_name, &num)) continue;
        char *full = join_path(dir, de->d_name);
        if (is_file(full)) fv_push(fv, full, num);
        else free(full);
    }
    closedir(d);
    return 1;
}

/* Finds the largest group of numbered image files in a directory. */
static int largest_group(const char *dir, Template *out, char *err, size_t errsz)
{
    DIR *d = opendir(dir);
    if (!d) {
        snprintf(err, errsz, "cannot open directory '%s'", dir);
        return 0;
    }

    struct {
        Template t;
        int      count;
    } *groups = NULL;
    int ngroups = 0, cap = 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!has_image_ext(de->d_name)) continue;

        Template t;
        if (!split_basename(de->d_name, &t, NULL)) {
            /* Unnumbered file: it can only ever be a sequence of one, so give
             * it a template that matches nothing else. */
            continue;
        }

        int found = -1;
        for (int i = 0; i < ngroups; i++) {
            if (strcmp(groups[i].t.prefix, t.prefix) == 0 &&
                strcmp(groups[i].t.suffix, t.suffix) == 0) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            if (ngroups == cap) {
                cap = cap ? cap * 2 : 16;
                groups = realloc(groups, (size_t)cap * sizeof *groups);
                if (!groups) rp_fatal("out of memory scanning '%s'", dir);
            }
            groups[ngroups].t = t;
            groups[ngroups].count = 0;
            found = ngroups++;
        }
        groups[found].count++;
    }
    closedir(d);

    if (ngroups == 0) {
        free(groups);
        snprintf(err, errsz, "no EXR files found in '%s'", dir);
        return 0;
    }

    int best = 0;
    for (int i = 1; i < ngroups; i++) {
        if (groups[i].count > groups[best].count ||
            (groups[i].count == groups[best].count &&
             strcmp(groups[i].t.prefix, groups[best].t.prefix) < 0))
            best = i;
    }
    *out = groups[best].t;
    free(groups);
    return 1;
}

static char *make_display(const Template *t)
{
    char hashes[32];
    int n = RP_CLAMP(t->digits, 1, 16);
    for (int i = 0; i < n; i++) hashes[i] = '#';
    hashes[n] = '\0';

    size_t len = strlen(t->prefix) + (size_t)n + strlen(t->suffix) + 1;
    char *s = rp_xmalloc(len);
    snprintf(s, len, "%s%s%s", t->prefix, hashes, t->suffix);
    return s;
}

static Sequence *finish(FrameVec *fv, const char *dir, char *display, char *err, size_t errsz)
{
    if (fv->n == 0) {
        free(fv->v);
        free(display);
        if (!*err) snprintf(err, errsz, "no frames found");
        return NULL;
    }
    qsort(fv->v, (size_t)fv->n, sizeof *fv->v, frame_cmp);

    Sequence *s = rp_xcalloc(1, sizeof *s);
    s->frames  = fv->v;
    s->count   = fv->n;
    s->dir     = rp_strdup(dir);
    s->display = display ? display : rp_strdup(base_name(fv->v[0].path));
    return s;
}

Sequence *sequence_open(char *const *inputs, int n_inputs, char *err, size_t errsz)
{
    FrameVec fv = { 0 };
    if (errsz) err[0] = '\0';

    if (n_inputs <= 0) {
        snprintf(err, errsz, "no input given");
        return NULL;
    }

    /* Several paths: take them literally, in numeric order. */
    if (n_inputs > 1) {
        for (int i = 0; i < n_inputs; i++) {
            if (!is_file(inputs[i])) {
                snprintf(err, errsz, "not a file: '%s'", inputs[i]);
                for (int j = 0; j < fv.n; j++) free(fv.v[j].path);
                free(fv.v);
                return NULL;
            }
            Template t;
            long num = i;
            if (!split_basename(base_name(inputs[i]), &t, &num)) num = i;
            fv_push(&fv, rp_strdup(inputs[i]), num);
        }
        char *dir = dir_name(inputs[0]);
        Sequence *s = finish(&fv, dir, NULL, err, errsz);
        free(dir);
        return s;
    }

    const char *in = inputs[0];
    Template t;
    char *dir = NULL;
    char *display = NULL;

    if (is_dir(in)) {
        dir = rp_strdup(in);
        if (!largest_group(dir, &t, err, errsz)) {
            free(dir);
            return NULL;
        }
    } else if (split_pattern(base_name(in), &t)) {
        dir = dir_name(in);
    } else if (is_file(in)) {
        dir = dir_name(in);
        if (!split_basename(base_name(in), &t, NULL)) {
            /* A single unnumbered file is a one frame sequence. */
            fv_push(&fv, rp_strdup(in), 0);
            Sequence *s = finish(&fv, dir, rp_strdup(base_name(in)), err, errsz);
            free(dir);
            return s;
        }
    } else {
        snprintf(err, errsz, "no such file or directory: '%s'", in);
        return NULL;
    }

    display = make_display(&t);
    if (!collect_matches(dir, &t, &fv, err, errsz)) {
        free(dir);
        free(display);
        return NULL;
    }
    if (fv.n == 0)
        snprintf(err, errsz, "no frames matching '%s' in '%s'", display, dir);

    Sequence *s = finish(&fv, dir, display, err, errsz);
    free(dir);
    return s;
}

void sequence_free(Sequence *s)
{
    if (!s) return;
    for (int i = 0; i < s->count; i++) free(s->frames[i].path);
    free(s->frames);
    free(s->dir);
    free(s->display);
    free(s);
}
