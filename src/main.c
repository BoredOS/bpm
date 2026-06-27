// bpm - BoredOS Package Manager
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <syscall.h>
#include "sha256.h"

#define CACHE_DIR       "/var/cache/bpm"
#define LIB_DIR         "/Library/Receipts"
#define PACKAGES_DIR    "/Library/Receipts"
#define INSTALLED_TOML  "/Library/Receipts/bpm_installed.toml"
#define CONFIG_PATH     "/Library/AppData/org.boredos.bpm/bpmconf.toml"

static const char *g_root = NULL;

static const char *get_target_path(const char *path, char *buf, size_t buf_len) {
    if (!g_root) return path;
    size_t root_len = strlen(g_root);
    if (root_len > 0 && g_root[root_len - 1] == '/' && path[0] == '/') {
        snprintf(buf, buf_len, "%s%s", g_root, path + 1);
    } else {
        snprintf(buf, buf_len, "%s%s", g_root, path);
    }
    return buf;
}

// ─── Helpers ────────────────────────────────────────────────────────────────

static void ensure_dir(const char *path) {
    char temp[1024];
    snprintf(temp, sizeof(temp), "%s", path);
    char *p = temp;
    if (*p == '/') p++;
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(temp, 0755);
            *p = '/';
        }
        p++;
    }
    mkdir(temp, 0755);
}

static int extract_quoted(const char *line, char *out, size_t out_len) {
    const char *q = strchr(line, '"');
    if (!q) return 0;
    const char *q2 = strchr(q + 1, '"');
    if (!q2) return 0;
    size_t l = q2 - (q + 1);
    if (l >= out_len) l = out_len - 1;
    memcpy(out, q + 1, l);
    out[l] = '\0';
    return 1;
}
static char *ltrim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}
static void rtrim_nl(char *s) {
    size_t l = strlen(s);
    while (l > 0 && (s[l-1] == '\n' || s[l-1] == '\r' || s[l-1] == ' ')) {
        s[--l] = '\0';
    }
}

static int verify_sha256(const char *path, const char *expected) {
    char result[65];
    if (sha256_file(path, result, sizeof(result)) != 0) return 0;
    return strcmp(result, expected) == 0;
}



static int copy_file(const char *src, const char *dest) {
    FILE *fs = fopen(src, "rb");
    if (!fs) return -1;
    FILE *fd = fopen(dest, "wb");
    if (!fd) {
        fclose(fs);
        return -1;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fs)) > 0) {
        fwrite(buf, 1, n, fd);
    }
    fclose(fs);
    fclose(fd);
    return 0;
}

static void remove_dir_recursive(const char *path) {
    FAT32_FileInfo *entries = malloc(sizeof(FAT32_FileInfo) * 128);
    if (!entries) return;
    int count = sys_list(path, entries, 128);
    if (count > 0) {
        for (int i = 0; i < count; i++) {
            char subpath[1024];
            snprintf(subpath, sizeof(subpath), "%s/%s", path, entries[i].name);
            if (entries[i].is_directory) {
                remove_dir_recursive(subpath);
            } else {
                sys_delete(subpath);
            }
        }
    }
    free(entries);
    sys_delete(path);
}

// Stub timestamp, BoredOS libc has no strftime/gmtime yet and i'm too lazy to add one. xD
static void iso_timestamp(char *buf, size_t len) {
    snprintf(buf, len, "1970-01-01T00:00:00Z");
}

static int _bpm_wait_pid(int pid) {
    int status = 0;
    for (;;) {
        int rc = sys_waitpid(pid, &status, 1);
        if (rc == pid) {
            return status;
        }
        if (rc < 0 && rc != -2) {
            return -1;
        }
        sys_system(SYSTEM_CMD_SLEEP, 10, 0, 0, 0);
    }
}

static int bpm_system(const char *command) {
    char cmd[128];
    char args[1024];
    int i = 0;
    int j = 0;
    int pid;

    while (command[i] && (command[i] == ' ' || command[i] == '\t')) i++;
    while (command[i] && command[i] != ' ' && command[i] != '\t' && j < (int)sizeof(cmd) - 1) {
        cmd[j++] = command[i++];
    }
    cmd[j] = '\0';

    if (cmd[0] == '\0') {
        return 0;
    }

    while (command[i] && (command[i] == ' ' || command[i] == '\t')) i++;
    {
        int k = 0;
        while (command[i] && k < (int)sizeof(args) - 1) {
            args[k++] = command[i++];
        }
        args[k] = '\0';
    }

    pid = sys_spawn(cmd, args[0] ? args : NULL, SPAWN_FLAG_TERMINAL | SPAWN_FLAG_INHERIT_TTY, 0);
    if (pid >= 0) {
        return _bpm_wait_pid(pid);
    }

    if (cmd[0] != '/') {
        char path[160];
        snprintf(path, sizeof(path), "/bin/%s", cmd);
        pid = sys_spawn(path, args[0] ? args : NULL, SPAWN_FLAG_TERMINAL | SPAWN_FLAG_INHERIT_TTY, 0);
        if (pid >= 0) {
            return _bpm_wait_pid(pid);
        }

        snprintf(path, sizeof(path), "/bin/%s.elf", cmd);
        pid = sys_spawn(path, args[0] ? args : NULL, SPAWN_FLAG_TERMINAL | SPAWN_FLAG_INHERIT_TTY, 0);
        if (pid >= 0) {
            return _bpm_wait_pid(pid);
        }
    }

    return -1;
}

// ─── Config parsing ──────────────────────────────────────────────────────────

typedef struct {
    char name[256];
    char url[1024];
    int  enabled;
    int  priority;
} Repo;

typedef struct {
    Repo repos[64];
    int  count;
} Config;

static int load_config(const char *path, Config *cfg) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "Cannot open config '%s': %s\n", path, strerror(errno));
        return 1;
    }
    cfg->count = 0;
    char line[1024];
    int in_repo = 0;
    Repo cur = {0};
    cur.enabled = 1;  
    cur.priority = 99;

    while (fgets(line, sizeof(line), f)) {
        char *s = ltrim(line);
        rtrim_nl(s);
        if (*s == '#' || *s == '\0') continue;

        if (strncmp(s, "[[repositories]]", 16) == 0) {
            if (in_repo && cur.name[0] && cur.url[0]) {
                cfg->repos[cfg->count++] = cur;
            }
            memset(&cur, 0, sizeof(cur));
            cur.enabled = 1;
            cur.priority = 99;
            in_repo = 1;
            continue;
        }

        if (!in_repo) continue;

        if (strncmp(s, "name", 4) == 0)
            extract_quoted(s, cur.name, sizeof(cur.name));
        else if (strncmp(s, "url", 3) == 0)
            extract_quoted(s, cur.url, sizeof(cur.url));
        else if (strncmp(s, "enabled", 7) == 0) {
            cur.enabled = (strstr(s, "true") != NULL) ? 1 : 0;
        } else if (strncmp(s, "priority", 8) == 0) {
            char *eq = strchr(s, '=');
            if (eq) cur.priority = atoi(eq + 1);
        }
    }
    if (in_repo && cur.name[0] && cur.url[0])
        cfg->repos[cfg->count++] = cur;

    fclose(f);
    return 0;
}

// ─── Package index lookup ────────────────────────────────────────────────────

typedef struct {
    char name[256];
    char version[128];
    char description[1024];
    char author[256];
    char license[128];
    char url[1024];
    char sha256[128];
    char repo[256];
} PkgInfo;

static int find_package(const char *pkgname, PkgInfo *out) {
    Config cfg;
    if (load_config(CONFIG_PATH, &cfg) != 0) return 1;

    for (int i = 0; i < cfg.count; i++) {
        Repo *r = &cfg.repos[i];
        if (!r->enabled) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.toml", CACHE_DIR, r->name);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[1024];
        int in_pkg = 0;
        char hdr[512];
        snprintf(hdr, sizeof(hdr), "[package.%s]", pkgname);

        memset(out, 0, sizeof(*out));

        while (fgets(line, sizeof(line), f)) {
            char *s = ltrim(line);
            rtrim_nl(s);
            if (*s == '#' || *s == '\0') continue;

            if (s[0] == '[') {
                if (strcmp(s, hdr) == 0) { in_pkg = 1; continue; }
                else if (in_pkg) break; 
                continue;
            }
            if (!in_pkg) continue;

            char val[1024] = {0};
            if (!extract_quoted(s, val, sizeof(val))) continue;

            if      (strncmp(s, "name",        4) == 0) strncpy(out->name,        val, sizeof(out->name)-1);
            else if (strncmp(s, "version",     7) == 0) strncpy(out->version,     val, sizeof(out->version)-1);
            else if (strncmp(s, "description", 11)== 0) strncpy(out->description, val, sizeof(out->description)-1);
            else if (strncmp(s, "author",      6) == 0) strncpy(out->author,      val, sizeof(out->author)-1);
            else if (strncmp(s, "license",     7) == 0) strncpy(out->license,     val, sizeof(out->license)-1);
            else if (strncmp(s, "url",         3) == 0) strncpy(out->url,         val, sizeof(out->url)-1);
            else if (strncmp(s, "sha256",      6) == 0) strncpy(out->sha256,      val, sizeof(out->sha256)-1);
        }
        fclose(f);

        if (out->url[0]) {
            strncpy(out->repo, r->name, sizeof(out->repo)-1);
            out->repo[sizeof(out->repo)-1] = '\0';
            if (!out->name[0]) strncpy(out->name, pkgname, sizeof(out->name)-1);
            return 0;
        }
    }
    return 1;
}

// ─── MANIFEST.toml parsing ───────────────────────────────────────────────────

typedef struct {
    char name[256];
    char version[128];
    char install_bin[512];
    char install_assets[512];
    char install_config[512];
    int  has_install_script;
    int  has_remove_script;
} Manifest;

static int parse_manifest(const char *manifest_path, Manifest *m) {
    memset(m, 0, sizeof(*m));
    FILE *f = fopen(manifest_path, "r");
    if (!f) return 1;
    char line[1024];
    int in_install = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = ltrim(line);
        rtrim_nl(s);
        if (*s == '#' || *s == '\0') continue;
        if (strcmp(s, "[install]") == 0) { in_install = 1; continue; }
        if (s[0] == '[') { in_install = 0; continue; }
        
        char val[512] = {0};
        if (!extract_quoted(s, val, sizeof(val))) continue;
        if (in_install) {
            if      (strncmp(s, "bin",    3) == 0) strncpy(m->install_bin,    val, sizeof(m->install_bin)-1);
            else if (strncmp(s, "assets", 6) == 0) strncpy(m->install_assets, val, sizeof(m->install_assets)-1);
            else if (strncmp(s, "config", 6) == 0) strncpy(m->install_config, val, sizeof(m->install_config)-1);
        } else {
            if      (strncmp(s, "name",    4) == 0) strncpy(m->name,           val, sizeof(m->name)-1);
            else if (strncmp(s, "version", 7) == 0) strncpy(m->version,        val, sizeof(m->version)-1);
        }
    }
    fclose(f);
    return 0;
}

// ─── File tracking ───────────────────────────────────────────────────────────

// Append a file entry to installed.toml for a package
static void record_installed(const char *pkgname, const char *version,
                               const char *repo, const char **files, int nfiles) {
    char lib_dir_buf[1024];
    char inst_toml_buf[1024];
    ensure_dir(get_target_path(LIB_DIR, lib_dir_buf, sizeof(lib_dir_buf)));
    FILE *f = fopen(get_target_path(INSTALLED_TOML, inst_toml_buf, sizeof(inst_toml_buf)), "a");
    if (!f) { fprintf(stderr, "Cannot write to %s\n", inst_toml_buf); return; }
    char ts[64];
    iso_timestamp(ts, sizeof(ts));
    fprintf(f, "[package.%s]\n", pkgname);
    fprintf(f, "version = \"%s\"\n", version);
    fprintf(f, "repo = \"%s\"\n", repo);
    fprintf(f, "installed_at = \"%s\"\n", ts);
    fprintf(f, "files = [\n");
    for (int i = 0; i < nfiles; i++) {
        const char *fpath = files[i];
        if (g_root) {
            size_t rlen = strlen(g_root);
            if (rlen > 0 && g_root[rlen-1] == '/') rlen--;
            if (strncmp(fpath, g_root, rlen) == 0) {
                fpath += rlen;
            }
        }
        fprintf(f, "    \"%s\",\n", fpath);
    }
    fprintf(f, "]\n\n");
    fclose(f);
}

// Collect all files installed from a directory into an array
// Returns count of files found, writes paths into files[] array
static int collect_files(const char *srcdir, const char *destdir,
                          char files[][1024], int max_files) {
    int count = 0;
    FAT32_FileInfo *entries = malloc(sizeof(FAT32_FileInfo) * 128);
    if (!entries) return 0;
    int got = sys_list(srcdir, entries, 128);
    if (got < 0) {
        free(entries);
        return 0;
    }
    for (int i = 0; i < got && count < max_files; i++) {
        if (strncmp(entries[i].name, "._", 2) == 0) continue;
        
        char srcpath[1024];
        char destpath[1024];
        snprintf(srcpath, sizeof(srcpath), "%s/%s", srcdir, entries[i].name);
        snprintf(destpath, sizeof(destpath), "%s/%s", destdir, entries[i].name);
        
        if (entries[i].is_directory) {
            count += collect_files(srcpath, destpath, &files[count], max_files - count);
        } else {
            snprintf(files[count], 1024, "%s", destpath);
            count++;
        }
    }
    free(entries);
    return count;
}

// Copy files from srcdir to destdir
static void copy_dir(const char *srcdir, const char *destdir, int skip_existing) {
    ensure_dir(destdir);
    FAT32_FileInfo *entries = malloc(sizeof(FAT32_FileInfo) * 128);
    if (!entries) return;
    int offset = 0;
    while (1) {
        int count = sys_list_offset(srcdir, entries, 128, offset);
        if (count <= 0) break;
        for (int i = 0; i < count; i++) {
            if (strncmp(entries[i].name, "._", 2) == 0) continue;
            
            char srcpath[1024];
            char destpath[1024];
            snprintf(srcpath, sizeof(srcpath), "%s/%s", srcdir, entries[i].name);
            snprintf(destpath, sizeof(destpath), "%s/%s", destdir, entries[i].name);
            
            if (entries[i].is_directory) {
                copy_dir(srcpath, destpath, skip_existing);
            } else {
                if (skip_existing) {
                    struct stat st;
                    if (stat(destpath, &st) == 0) continue;
                }
                copy_file(srcpath, destpath);
            }
        }
        offset += count;
    }
    free(entries);
}


// ─── Commands ────────────────────────────────────────────────────────────────

int cmd_update(void) {
    Config cfg;
    if (load_config(CONFIG_PATH, &cfg) != 0) return 1;
    ensure_dir(CACHE_DIR);

    int fetched = 0;
    for (int i = 0; i < cfg.count; i++) {
        Repo *r = &cfg.repos[i];
        if (!r->enabled) {
            printf("Skipping %s (disabled)\n", r->name);
            continue;
        }
        char outpath[1024];
        snprintf(outpath, sizeof(outpath), "%s/%s.toml", CACHE_DIR, r->name);
        printf("Fetching %s from %s... ", r->name, r->url);
        fflush(stdout);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd),
            "curl.elf -fsSL -o %s %s", outpath, r->url);
        if (bpm_system(cmd) == 0) { printf("done\n"); fetched++; }
        else printf("FAILED\n");
    }
    printf("Updated %d/%d repositories\n", fetched, cfg.count);
    return 0;
}

int cmd_install(const char *pkgname, int keep_configs) {
    PkgInfo pkg;
    char bup_path[1024] = {0};
    int is_local = 0;

    size_t name_len = strlen(pkgname);
    if (name_len > 4 && strcmp(pkgname + name_len - 4, ".bup") == 0) {
        struct stat st;
        if (stat(pkgname, &st) == 0) {
            is_local = 1;
            strncpy(bup_path, pkgname, sizeof(bup_path) - 1);
        }
    }

    if (!is_local) {
        if (find_package(pkgname, &pkg) != 0) {
            fprintf(stderr, "Package '%s' not found. Run: bpm update\n", pkgname);
            return 1;
        }

        printf("Found %s %s in %s\n", pkg.name, pkg.version, pkg.repo);

        // Download
        ensure_dir(CACHE_DIR "/packages");
        snprintf(bup_path, sizeof(bup_path),
            "%s/packages/%s-%s.bup", CACHE_DIR, pkgname, pkg.version);

        printf("Downloading %s-%s.bup... ", pkgname, pkg.version);
        fflush(stdout);
        char cmd[2048];
        snprintf(cmd, sizeof(cmd), "curl -fsSL -o %s %s", bup_path, pkg.url);
        if (bpm_system(cmd) != 0) {
            fprintf(stderr, "Download failed\n");
            return 2;
        }
        printf("done\n");

        // Verify sha256
        if (pkg.sha256[0]) {
            printf("Verifying sha256... ");
            fflush(stdout);
            if (!verify_sha256(bup_path, pkg.sha256)) {
                fprintf(stderr, "MISMATCH — aborting\n");
                sys_delete(bup_path);
                return 3;
            }
            printf("ok\n");
        }
    }

    // Extract into temp dir
    char extract_dir[1024] = "/tmp/bpm_extract_temp";
    remove_dir_recursive(extract_dir);
    ensure_dir(extract_dir);
    printf("Extracting... ");
    fflush(stdout);
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "tar.elf -q --lz4 -xf %s -C %s", bup_path, extract_dir);
    if (bpm_system(cmd) != 0) {
        fprintf(stderr, "Extraction failed\n");
        return 4;
    }
    printf("done\n");

    // Parse MANIFEST.toml
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/MANIFEST.toml", extract_dir);
    Manifest m;
    if (parse_manifest(manifest_path, &m) != 0) {
        fprintf(stderr, "Failed to read MANIFEST.toml from package\n");
        return 5;
    }

    // Copy files to system paths and record them
    printf("Installing files... ");
    fflush(stdout);

    char (*files)[1024] = malloc(256 * 1024);
    if (!files) {
        fprintf(stderr, "Out of memory\n");
        return 5;
    }
    int nfiles = 0;

    // bin/
    char bin_src[1024];
    snprintf(bin_src, sizeof(bin_src), "%s/bin", extract_dir);
    struct stat st;
    if (stat(bin_src, &st) == 0) {
        const char *dest = m.install_bin[0] ? m.install_bin : "/usr/bin";
        char dest_buf[1024];
        const char *target_dest = get_target_path(dest, dest_buf, sizeof(dest_buf));
        ensure_dir(target_dest);
        copy_dir(bin_src, target_dest, 0);
        nfiles += collect_files(bin_src, target_dest, &files[nfiles], 256 - nfiles);
    }

    // assets/
    char assets_src[1024];
    snprintf(assets_src, sizeof(assets_src), "%s/assets", extract_dir);
    if (stat(assets_src, &st) == 0 && m.install_assets[0]) {
        char assets_buf[1024];
        const char *target_assets = get_target_path(m.install_assets, assets_buf, sizeof(assets_buf));
        ensure_dir(target_assets);
        copy_dir(assets_src, target_assets, 0);
        nfiles += collect_files(assets_src, target_assets, &files[nfiles], 256 - nfiles);
    }

    // config/
    char config_src[1024];
    snprintf(config_src, sizeof(config_src), "%s/config", extract_dir);
    if (stat(config_src, &st) == 0) {
        char default_dest[1024];
        snprintf(default_dest, sizeof(default_dest), "/Library/AppData/%s", m.name);
        const char *dest = m.install_config[0] ? m.install_config : default_dest;
        char dest_buf[1024];
        const char *target_dest = get_target_path(dest, dest_buf, sizeof(dest_buf));
        ensure_dir(target_dest);
        copy_dir(config_src, target_dest, keep_configs);
        nfiles += collect_files(config_src, target_dest, &files[nfiles], 256 - nfiles);
    }

    // usr/share/applications/ -> install desktop entries present in package
    char apps_src[1024];
    snprintf(apps_src, sizeof(apps_src), "%s/usr/share/applications", extract_dir);
    if (stat(apps_src, &st) == 0) {
        char dest_apps[1024];
        snprintf(dest_apps, sizeof(dest_apps), "/Library/AppData/%s", m.name);
        char apps_buf[1024];
        const char *target_apps = get_target_path(dest_apps, apps_buf, sizeof(apps_buf));
        ensure_dir(target_apps);
        copy_dir(apps_src, target_apps, 0);
        nfiles += collect_files(apps_src, target_apps, &files[nfiles], 256 - nfiles);
    }
    printf("done\n");

    const char *final_name = is_local ? m.name : pkgname;
    const char *final_ver = is_local ? m.version : pkg.version;
    const char *final_repo = is_local ? "local" : pkg.repo;

    // Store scripts + MANIFEST in /var/lib/bpm/packages/<name>/
    char pkglib[1024];
    char pkglib_buf[1024];
    snprintf(pkglib, sizeof(pkglib), "%s/%s", PACKAGES_DIR, final_name);
    const char *target_pkglib = get_target_path(pkglib, pkglib_buf, sizeof(pkglib_buf));
    ensure_dir(target_pkglib);
    char sm[1024], dm[1024];
    snprintf(sm, sizeof(sm), "%s/MANIFEST.toml", extract_dir);
    snprintf(dm, sizeof(dm), "%s/MANIFEST.toml", target_pkglib);
    copy_file(sm, dm);

    char scripts_src[1024];
    snprintf(scripts_src, sizeof(scripts_src), "%s/scripts", extract_dir);
    if (stat(scripts_src, &st) == 0) {
        FAT32_FileInfo *entries = malloc(sizeof(FAT32_FileInfo) * 128);
        if (entries) {
            int s_count = sys_list(scripts_src, entries, 128);
            for (int i = 0; i < s_count; i++) {
                if (!entries[i].is_directory) {
                    if (strncmp(entries[i].name, "._", 2) == 0) continue;
                    size_t nlen = strlen(entries[i].name);
                    if (nlen > 4 && strcmp(entries[i].name + nlen - 4, ".bsh") == 0) {
                        char src_bsh[1024], dest_bsh[1024];
                        snprintf(src_bsh, sizeof(src_bsh), "%s/%s", scripts_src, entries[i].name);
                        snprintf(dest_bsh, sizeof(dest_bsh), "%s/%s", target_pkglib, entries[i].name);
                        copy_file(src_bsh, dest_bsh);
                    }
                }
            }
            free(entries);
        }
        
        // Check which scripts exist
        char spath[1024];
        snprintf(spath, sizeof(spath), "%s/install.bsh", target_pkglib);
        m.has_install_script = (stat(spath, &st) == 0);
        snprintf(spath, sizeof(spath), "%s/remove.bsh", target_pkglib);
        m.has_remove_script = (stat(spath, &st) == 0);
    }

    // Run install.bsh if present
    if (m.has_install_script) {
        char script[1024];
        snprintf(script, sizeof(script), "%s/install.bsh", target_pkglib);
        printf("Running install script... ");
        fflush(stdout);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "bsh %s", script);
        bpm_system(cmd);
        printf("done\n");
    }

    // Record in installed.toml
    const char *fptrs[256];
    for (int i = 0; i < nfiles; i++) fptrs[i] = files[i];
    record_installed(final_name, final_ver, final_repo, fptrs, nfiles);

    // Cleanup
    remove_dir_recursive(extract_dir);

    printf("Installed %s %s\n", pkgname, pkg.version);
    free(files);
    return 0;
}

int cmd_remove(const char *pkgname, int keep_configs) {
    // Read files list from installed.toml
    FILE *f = fopen(INSTALLED_TOML, "r");
    if (!f) { fprintf(stderr, "Nothing installed (no %s)\n", INSTALLED_TOML); return 1; }

    char line[1024];
    int in_pkg = 0;
    char pkg_hdr[512];
    snprintf(pkg_hdr, sizeof(pkg_hdr), "[package.%s]", pkgname);
    int found = 0;

    Manifest m;
    char manifest_path[1024];
    snprintf(manifest_path, sizeof(manifest_path), "%s/%s/MANIFEST.toml", PACKAGES_DIR, pkgname);
    int has_manifest = (parse_manifest(manifest_path, &m) == 0);
    char default_config_path[1024];
    snprintf(default_config_path, sizeof(default_config_path), "/Library/AppData/%s", pkgname);
    const char *config_dir = (has_manifest && m.install_config[0]) ? m.install_config : default_config_path;

    // Run remove.bsh first if it exists
    char script[1024];
    snprintf(script, sizeof(script), "%s/%s/remove.bsh", PACKAGES_DIR, pkgname);
    struct stat st;
    if (stat(script, &st) == 0) {
        printf("Running remove script... ");
        fflush(stdout);
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "bsh %s", script);
        bpm_system(cmd);
        printf("done\n");
    }

    // Delete installed files
    printf("Removing files... ");
    fflush(stdout);
    while (fgets(line, sizeof(line), f)) {
        char *s = ltrim(line);
        rtrim_nl(s);
        if (strcmp(s, pkg_hdr) == 0) { in_pkg = 1; found = 1; continue; }
        if (s[0] == '[' && in_pkg) break;
        if (!in_pkg) continue;
        // Lines inside files = [ ... ]
        if (s[0] == '"') {
            char fpath[1024] = {0};
            extract_quoted(s, fpath, sizeof(fpath));
            if (fpath[0]) {
                int is_config = 0;
                if (keep_configs) {
                    size_t conf_len = strlen(config_dir);
                    if (strncmp(fpath, config_dir, conf_len) == 0 && (fpath[conf_len] == '/' || fpath[conf_len] == '\0')) {
                        is_config = 1;
                    }
                }
                if (!is_config) {
                    sys_delete(fpath);
                }
            }
        }
    }
    fclose(f);
    printf("done\n");

    if (!found) { fprintf(stderr, "Package '%s' is not installed\n", pkgname); return 1; }

    // Remove package lib dir
    char pdir[1024];
    snprintf(pdir, sizeof(pdir), "%s/%s", PACKAGES_DIR, pkgname);
    remove_dir_recursive(pdir);

    // Rewrite installed.toml without this package's block
    FILE *fin = fopen(INSTALLED_TOML, "r");
    if (fin) {
        FILE *fout = fopen(INSTALLED_TOML ".tmp", "w");
        if (fout) {
            char rline[1024];
            int skip = 0;
            while (fgets(rline, sizeof(rline), fin)) {
                char rline_copy[1024];
                strcpy(rline_copy, rline);
                char *s = ltrim(rline_copy);
                rtrim_nl(s);
                if (strcmp(s, pkg_hdr) == 0) {
                    skip = 1;
                    continue;
                }
                if (skip && strncmp(s, "[package.", 9) == 0) {
                    skip = 0;
                }
                if (!skip) {
                    fprintf(fout, "%s", rline);
                }
            }
            fclose(fout);
        }
        fclose(fin);
        sys_delete(INSTALLED_TOML);
        copy_file(INSTALLED_TOML ".tmp", INSTALLED_TOML);
        sys_delete(INSTALLED_TOML ".tmp");
    }

    printf("Removed %s\n", pkgname);
    return 0;
}

int cmd_upgrade(const char *pkgname) {
    // If pkgname is NULL, upgrade all installed packages
    FILE *f = fopen(INSTALLED_TOML, "r");
    if (!f) { printf("No packages installed\n"); return 0; }

    char line[1024];
    char (*pkgs)[256] = malloc(128 * 256);
    char (*vers)[128] = malloc(128 * 128);
    if (!pkgs || !vers) {
        if (pkgs) free(pkgs);
        if (vers) free(vers);
        fclose(f);
        printf("Out of memory\n");
        return -1;
    }
    int count = 0;

    char cur_pkg[256] = {0};
    char cur_ver[128] = {0};

    while (fgets(line, sizeof(line), f)) {
        char *s = ltrim(line);
        rtrim_nl(s);
        if (s[0] == '[') {
            if (cur_pkg[0] && cur_ver[0] && count < 128) {
                strncpy(pkgs[count], cur_pkg, sizeof(pkgs[count]) - 1);
                pkgs[count][sizeof(pkgs[count]) - 1] = '\0';
                strncpy(vers[count], cur_ver, sizeof(vers[count]) - 1);
                vers[count][sizeof(vers[count]) - 1] = '\0';
                count++;
            }
            if (strncmp(s, "[package.", 9) == 0) {
                size_t l = strlen(s);
                if (s[l-1] == ']') {
                    size_t nl = l - 10; 
                    if (nl >= sizeof(cur_pkg)) nl = sizeof(cur_pkg)-1;
                    memcpy(cur_pkg, s + 9, nl);
                    cur_pkg[nl] = '\0';
                    cur_ver[0] = '\0';
                }
            }
            continue;
        }
        if (strncmp(s, "version", 7) == 0)
            extract_quoted(s, cur_ver, sizeof(cur_ver));
    }
    if (cur_pkg[0] && cur_ver[0] && count < 128) {
        strncpy(pkgs[count], cur_pkg, sizeof(pkgs[count]) - 1);
        pkgs[count][sizeof(pkgs[count]) - 1] = '\0';
        strncpy(vers[count], cur_ver, sizeof(vers[count]) - 1);
        vers[count][sizeof(vers[count]) - 1] = '\0';
        count++;
    }
    fclose(f);

    int upgraded = 0;
    for (int i = 0; i < count; i++) {
        if (pkgname == NULL || strcmp(pkgname, pkgs[i]) == 0) {
            PkgInfo pkg;
            if (find_package(pkgs[i], &pkg) == 0 && strcmp(pkg.version, vers[i]) != 0) {
                printf("Upgrading %s %s → %s\n", pkgs[i], vers[i], pkg.version);
                // Run postupdate from old package scripts if present
                char script[1024];
                snprintf(script, sizeof(script), "%s/%s/remove.bsh", PACKAGES_DIR, pkgs[i]);
                struct stat st;
                if (stat(script, &st) == 0) {
                    char cmd[1024];
                    snprintf(cmd, sizeof(cmd), "bsh %s", script);
                    bpm_system(cmd);
                }
                cmd_remove(pkgs[i], 1);
                cmd_install(pkgs[i], 1);
                upgraded++;
            }
        }
    }

    free(pkgs);
    free(vers);
    if (upgraded == 0) printf("All packages up to date\n");
    return 0;
}

int cmd_list(void) {
    FILE *f = fopen(INSTALLED_TOML, "r");
    if (!f) { printf("No packages installed\n"); return 0; }
    char line[1024];
    char cur_pkg[256] = {0};
    char cur_ver[128] = {0};
    char cur_repo[256] = {0};
    int count = 0;
    while (fgets(line, sizeof(line), f)) {
        char *s = ltrim(line);
        rtrim_nl(s);
        if (strncmp(s, "[package.", 9) == 0) {
            if (cur_pkg[0])
                printf("  %-20s %-12s %s\n", cur_pkg, cur_ver, cur_repo);
            size_t l = strlen(s);
            size_t nl = l - 10;
            if (nl >= sizeof(cur_pkg)) nl = sizeof(cur_pkg)-1;
            memcpy(cur_pkg, s + 9, nl);
            cur_pkg[nl] = '\0';
            cur_ver[0] = '\0'; cur_repo[0] = '\0';
            count++;
            continue;
        }
        if (strncmp(s, "version", 7) == 0) extract_quoted(s, cur_ver, sizeof(cur_ver));
        if (strncmp(s, "repo",    4) == 0) extract_quoted(s, cur_repo, sizeof(cur_repo));
    }
    if (cur_pkg[0])
        printf("  %-20s %-12s %s\n", cur_pkg, cur_ver, cur_repo);
    fclose(f);
    if (count == 0) printf("No packages installed\n");
    else printf("\n%d package(s) installed\n", count);
    return 0;
}

int cmd_search(const char *query) {
    Config cfg;
    if (load_config(CONFIG_PATH, &cfg) != 0) {
        fprintf(stderr, "No config found.\n");
        return 1;
    }

    int found = 0;
    for (int i = 0; i < cfg.count; i++) {
        Repo *r = &cfg.repos[i];
        if (!r->enabled) continue;

        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.toml", CACHE_DIR, r->name);

        FILE *f = fopen(path, "r");
        if (!f) continue;

        char line[1024];
        char cur_name[256]={0}, cur_ver[128]={0}, cur_desc[1024]={0};
        int in_pkg = 0;

        while (fgets(line, sizeof(line), f)) {
            char *s = ltrim(line);
            rtrim_nl(s);
            if (strncmp(s, "[package.", 9) == 0) {
                if (in_pkg && cur_name[0]) {
                    char lname[256], ldesc[1024];
                    strncpy(lname, cur_name, sizeof(lname)-1);
                    strncpy(ldesc, cur_desc, sizeof(ldesc)-1);
                    for (char *p=lname; *p; p++) if (*p>='A'&&*p<='Z') *p+=32;
                    for (char *p=ldesc; *p; p++) if (*p>='A'&&*p<='Z') *p+=32;
                    char lquery[256]; strncpy(lquery, query, sizeof(lquery)-1);
                    for (char *p=lquery; *p; p++) if (*p>='A'&&*p<='Z') *p+=32;
                    if (strstr(lname, lquery) || strstr(ldesc, lquery)) {
                        printf("  %-20s %-12s %-16s %s\n", cur_name, cur_ver, r->name, cur_desc);
                        found++;
                    }
                }
                size_t l = strlen(s);
                size_t nl = l - 10;
                if (nl >= sizeof(cur_name)) nl = sizeof(cur_name)-1;
                memcpy(cur_name, s+9, nl); cur_name[nl]='\0';
                cur_ver[0]='\0'; cur_desc[0]='\0';
                in_pkg = 1;
                continue;
            }
            if (!in_pkg) continue;
            if (strncmp(s, "version",     7) == 0) extract_quoted(s, cur_ver,  sizeof(cur_ver));
            if (strncmp(s, "description", 11)== 0) extract_quoted(s, cur_desc, sizeof(cur_desc));
        }
        if (in_pkg && cur_name[0]) {
            char lname[256], ldesc[1024], lquery[256];
            strncpy(lname,  cur_name,  sizeof(lname)-1);
            strncpy(ldesc,  cur_desc,  sizeof(ldesc)-1);
            strncpy(lquery, query,     sizeof(lquery)-1);
            for (char *p=lname;  *p; p++) if (*p>='A'&&*p<='Z') *p+=32;
            for (char *p=ldesc;  *p; p++) if (*p>='A'&&*p<='Z') *p+=32;
            for (char *p=lquery; *p; p++) if (*p>='A'&&*p<='Z') *p+=32;
            if (strstr(lname, lquery) || strstr(ldesc, lquery)) {
                printf("  %-20s %-12s %-16s %s\n", cur_name, cur_ver, r->name, cur_desc);
                found++;
            }
        }
        fclose(f);
    }
    if (found == 0) printf("No packages found matching '%s'\n", query);
    return 0;
}

int cmd_info(const char *pkgname) {
    PkgInfo pkg;
    if (find_package(pkgname, &pkg) != 0) {
        fprintf(stderr, "Package '%s' not found. Run: bpm update\n", pkgname);
        return 1;
    }
    printf("Name:         %s\n", pkg.name);
    printf("Version:      %s\n", pkg.version);
    printf("Repo:         %s\n", pkg.repo);
    printf("Author:       %s\n", pkg.author);
    printf("License:      %s\n", pkg.license);
    printf("Description:  %s\n", pkg.description);
    printf("URL:          %s\n", pkg.url);
    return 0;
}

int cmd_reinstall(const char *pkgname) {
    printf("Reinstalling %s...\n", pkgname);
    cmd_remove(pkgname, 0);
    return cmd_install(pkgname, 0);
}

int cmd_clean(void) {
    printf("Cleaning package cache...\n");
    char path[1024];
    snprintf(path, sizeof(path), "%s/packages", CACHE_DIR);
    FAT32_FileInfo *entries = malloc(sizeof(FAT32_FileInfo) * 128);
    if (!entries) return -1;
    int got = sys_list(path, entries, 128);
    for (int i = 0; i < got; i++) {
        if (!entries[i].is_directory) {
            size_t len = strlen(entries[i].name);
            if (len > 4 && strcmp(entries[i].name + len - 4, ".bup") == 0) {
                char buppath[1024];
                snprintf(buppath, sizeof(buppath), "%s/%s", path, entries[i].name);
                sys_delete(buppath);
            }
        }
    }
    free(entries);
    printf("Cache cleaned\n");
    return 0;
}

int cmd_addrepo(const char *input) {
    char name[256] = {0};
    char url[1024] = {0};

    const char *domain = input;
    if (strncmp(domain, "https://", 8) == 0) {
        domain += 8;
    } else if (strncmp(domain, "http://", 7) == 0) {
        domain += 7;
    }

    const char *dot = strchr(domain, '.');
    if (dot && dot != domain) {
        size_t len = dot - domain;
        if (len >= sizeof(name)) len = sizeof(name) - 1;
        memcpy(name, domain, len);
        name[len] = '\0';
    } else {
        strncpy(name, domain, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
    }

    if (strncmp(input, "http://", 7) == 0 || strncmp(input, "https://", 8) == 0) {
        strncpy(url, input, sizeof(url) - 1);
    } else {
        snprintf(url, sizeof(url), "https://%s", input);
    }

    size_t url_len = strlen(url);
    if (url_len > 5 && strcmp(url + url_len - 5, ".toml") != 0) {
        if (url[url_len - 1] == '/') {
            strncat(url, "index.toml", sizeof(url) - url_len - 1);
        } else {
            strncat(url, "/index.toml", sizeof(url) - url_len - 1);
        }
    }

    FILE *f = fopen(CONFIG_PATH, "a");
    if (!f) { fprintf(stderr, "Cannot open %s\n", CONFIG_PATH); return 1; }
    fprintf(f, "\n[[repositories]]\n");
    fprintf(f, "name = \"%s\"\n", name);
    fprintf(f, "url = \"%s\"\n", url);
    fprintf(f, "priority = 99\n");
    fprintf(f, "enabled = true\n");
    fclose(f);
    printf("Added repository '%s' -> %s\n", name, url);
    printf("Run: bpm update\n");
    return 0;
}

int cmd_removerepo(const char *name) {
    char parsed_name[256] = {0};
    const char *domain = name;
    if (strncmp(domain, "https://", 8) == 0) {
        domain += 8;
    } else if (strncmp(domain, "http://", 7) == 0) {
        domain += 7;
    }
    const char *dot_ptr = strchr(domain, '.');
    if (dot_ptr && dot_ptr != domain) {
        size_t len = dot_ptr - domain;
        if (len >= sizeof(parsed_name)) len = sizeof(parsed_name) - 1;
        memcpy(parsed_name, domain, len);
        parsed_name[len] = '\0';
    } else {
        strncpy(parsed_name, domain, sizeof(parsed_name) - 1);
        parsed_name[sizeof(parsed_name) - 1] = '\0';
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.toml", CACHE_DIR, name);
    sys_delete(path);
    if (parsed_name[0]) {
        snprintf(path, sizeof(path), "%s/%s.toml", CACHE_DIR, parsed_name);
        sys_delete(path);
    }

    FILE *fin = fopen(CONFIG_PATH, "r");
    if (fin) {
        FILE *fout = fopen(CONFIG_PATH ".tmp", "w");
        if (fout) {
            char rline[1024];
            int in_repo = 0;
            int skip = 0;
            char repo_buf[4096] = {0};
            size_t repo_len = 0;
            while (fgets(rline, sizeof(rline), fin)) {
                char rline_copy[1024];
                strcpy(rline_copy, rline);
                char *s = ltrim(rline_copy);
                rtrim_nl(s);
                if (strcmp(s, "[[repositories]]") == 0) {
                    if (in_repo) {
                        if (!skip) {
                            fprintf(fout, "%s", repo_buf);
                        }
                    }
                    in_repo = 1;
                    skip = 0;
                    repo_buf[0] = '\0';
                    repo_len = 0;
                }
                if (in_repo) {
                    if (strncmp(s, "name", 4) == 0) {
                        char rname[256] = {0};
                        extract_quoted(s, rname, sizeof(rname));
                        if (strcmp(rname, name) == 0 || (parsed_name[0] && strcmp(rname, parsed_name) == 0)) {
                            skip = 1;
                        }
                    } else if (strncmp(s, "url", 3) == 0) {
                        char rurl[1024] = {0};
                        extract_quoted(s, rurl, sizeof(rurl));
                        if (strstr(rurl, name) != NULL) {
                            skip = 1;
                        }
                    }
                    size_t llen = strlen(rline);
                    if (repo_len + llen < sizeof(repo_buf) - 1) {
                        strcpy(repo_buf + repo_len, rline);
                        repo_len += llen;
                    }
                } else {
                    fprintf(fout, "%s", rline);
                }
            }
            if (in_repo && !skip) {
                fprintf(fout, "%s", repo_buf);
            }
            fclose(fout);
        }
        fclose(fin);
        sys_delete(CONFIG_PATH);
        copy_file(CONFIG_PATH ".tmp", CONFIG_PATH);
        sys_delete(CONFIG_PATH ".tmp");
    }
    printf("Removed repository '%s'\n", name);
    return 0;
}

// ─── Help ────────────────────────────────────────────────────────────────────

int cmd_help(void) {
    printf("bpm - BoredOS Package Manager\n\n");
    printf("Usage: bpm <command> [args]\n\n");
    printf("Commands:\n");
    printf("  update                  Fetch indexes from all enabled repositories\n");
    printf("  install <pkg>           Download and install a package\n");
    printf("  remove  <pkg>           Remove an installed package\n");
    printf("  upgrade [pkg]           Upgrade one or all installed packages\n");
    printf("  reinstall <pkg>         Remove and reinstall a package\n");
    printf("  list                    List all installed packages\n");
    printf("  search <query>          Search available packages by name or description\n");
    printf("  info <pkg>              Show details about a package\n");
    printf("  clean                   Clear downloaded package cache\n");
    printf("  addrepo <url>           Add a repository\n");
    printf("  removerepo <name>       Remove a repository\n");
    printf("  help                    Show this message\n");
    return 0;
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main(int argc, char **argv) {
    int arg_offset = 0;
    while (argc > 3 + arg_offset && (strcmp(argv[1 + arg_offset], "--root") == 0 || strcmp(argv[1 + arg_offset], "-r") == 0)) {
        g_root = argv[2 + arg_offset];
        arg_offset += 2;
    }
    if (arg_offset > 0) {
        for (int i = 1; i < argc - arg_offset; i++) {
            argv[i] = argv[i + arg_offset];
        }
        argc -= arg_offset;
    }

    if (argc < 2) return cmd_help();

    if (strcmp(argv[1], "update")     == 0) return cmd_update();
    if (strcmp(argv[1], "install")    == 0) { if (argc < 3) { fprintf(stderr, "Usage: bpm install <pkg>\n"); return 1; } return cmd_install(argv[2], 0); }
    if (strcmp(argv[1], "remove")     == 0) { if (argc < 3) { fprintf(stderr, "Usage: bpm remove <pkg>\n");  return 1; } return cmd_remove(argv[2], 0); }
    if (strcmp(argv[1], "upgrade")    == 0) return cmd_upgrade(argc >= 3 ? argv[2] : NULL);
    if (strcmp(argv[1], "reinstall")  == 0) { if (argc < 3) { fprintf(stderr, "Usage: bpm reinstall <pkg>\n"); return 1; } return cmd_reinstall(argv[2]); }
    if (strcmp(argv[1], "list")       == 0) return cmd_list();
    if (strcmp(argv[1], "search")     == 0) { if (argc < 3) { fprintf(stderr, "Usage: bpm search <query>\n"); return 1; } return cmd_search(argv[2]); }
    if (strcmp(argv[1], "info")       == 0) { if (argc < 3) { fprintf(stderr, "Usage: bpm info <pkg>\n");     return 1; } return cmd_info(argv[2]); }
    if (strcmp(argv[1], "clean")      == 0) return cmd_clean();
    if (strcmp(argv[1], "addrepo")    == 0) { if (argc < 3) { fprintf(stderr, "Usage: bpm addrepo <url>\n"); return 1; } return cmd_addrepo(argv[2]); }
    if (strcmp(argv[1], "removerepo") == 0) { if (argc < 3) { fprintf(stderr, "Usage: bpm removerepo <name>\n"); return 1; } return cmd_removerepo(argv[2]); }
    if (strcmp(argv[1], "help")       == 0 || strcmp(argv[1], "--help") == 0) return cmd_help();

    fprintf(stderr, "Unknown command: %s\nRun: bpm help\n", argv[1]);
    return 1;
}